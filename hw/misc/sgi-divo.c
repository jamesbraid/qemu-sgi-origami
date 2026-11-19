/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI DIVO video board: one LINC PCI function (a video pipe) with its
 * memory window, mailbox, flash and packet ring, and a firmware stand-in
 * that answers the mailbox.
 *
 * The facts and the stand-in's limits are in
 * docs/system/mips/sgi/hardware/divo.rst; comments cite its section names.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "system/memory.h"

#define TYPE_SGI_DIVO_LINC "sgi-divo-linc"
OBJECT_DECLARE_SIMPLE_TYPE(SGIDivoLincState, SGI_DIVO_LINC)

/* "Identity" and "Config space". */
#define LINC_VENDOR_ID          0x10a9
#define LINC_DEVICE_ID          0x0002
#define LINC_WINDOW_SIZE        (128 * MiB)
#define CFG_CONSUMER            0x14

/* "Window map". */
#define MAILBOX_BASE            0x4400000
#define MAILBOX_SLOTS           32
#define MAILBOX_STRIDE          0x10000
#define VIP_BASE                0x4c00800
#define VIP_SIZE                0x10
#define VIP_ID                  0x000810a9
#define CTL_BASE                0x7800000
#define CTL_SIZE                0x20
#define CTL_CONTROL             0x00
#define CTL_CONTROL_BUSY       BIT(6)
#define CTL_CONTROL_RESET       0xc
#define CTL_STATUS              0x18
#define STATUS_INT              BIT(20)
#define STATUS_PANIC            BIT(21)
#define FLASH_BASE              0x7c00000
#define FLASH_SIZE              (512 * KiB)

/* "Flash". */
#define FLASH_ID                0x1fa4
#define FLASH_SECTOR_SHIFT      8
#define FLASH_ADDR_MASK         0x7fff
#define FLASH_CMD1              0x5555
#define FLASH_CMD2              0x2aaa

/* "Mailbox". */
#define OP_SHIFT                58
#define OP_WRITE_BYTE           0x00
#define OP_WRITE_WORD           0x02
#define OP_CONTROL              0x03
#define OP_RING_BASE            0x09
#define OP_RING_COUNT           0x0a
#define OP_RING_CONSUMED        0x0b
#define OP_BUFFER_BASE          0x0c
#define OP_CLOCK_OFFSET         0x0d
#define OP_READ_WORD            0x10
#define OP_READ_HALF            0x12
#define OP_BLOCK_SEND           0x20
#define OP_LOAD_FIRST           0x23
#define OP_LOAD_AUDIO           0x28
#define CONSUMER_RESET          (MAILBOX_SLOTS - 1)

/* "Packet ring". */
#define RING_ENTRIES            64
#define RING_ENTRY_SIZE         0x80
#define RING_VALID              0x7c
#define PKT_SYNC_WORD           0x10000
#define PKT_SYNC_HALF           0x20000
#define PKT_CONSUMER            0x01000000
#define PKT_BLOCK_DONE          0x08000000
#define PKT_VIDEO_PACKER_DONE   0x20000000
#define PKT_AUDIO_PACKER_DONE   0x40000000
#define BLOCK_UNIT              128
#define PENDING_MAX             32
/* A real board answers after its firmware runs, never inside the store. */
#define REPLY_DELAY_NS          100000

typedef enum {
    FLASH_IDLE,
    FLASH_UNLOCK1,
    FLASH_UNLOCK2,
    FLASH_ERASE1,
    FLASH_ERASE2,
    FLASH_ERASE3,
} FlashState;

struct SGIDivoLincState {
    PCIDevice parent_obj;

    MemoryRegion window;
    MemoryRegion ram;
    MemoryRegion mailbox;
    MemoryRegion vip;
    MemoryRegion ctl;
    MemoryRegion flash_mr;

    uint32_t ctl_reg[CTL_SIZE / 4];
    uint32_t vip_reg[VIP_SIZE / 4];
    uint32_t mailbox_hi;
    uint32_t consumer;
    uint64_t ring_base;
    uint32_t ring_next;
    bool ring_set;
    QEMUTimer *reply_timer;
    uint32_t pending_count;
    uint32_t pending[PENDING_MAX][3];

    uint8_t flash[FLASH_SIZE];
    uint32_t flash_state;
    bool flash_id_mode;
    bool flash_program;
    uint32_t flash_sector;
};

static void divo_update_irq(SGIDivoLincState *s)
{
    pci_set_irq(PCI_DEVICE(s), !!(s->ctl_reg[CTL_STATUS / 4] & STATUS_INT));
}

/*
 * "Packet ring": one packet; the valid dword goes last. The dword at 0x78
 * is left as the host set it, as nothing says the board writes it.
 */
static void divo_post_now(SGIDivoLincState *s, uint32_t type, uint32_t w1,
                          uint32_t w2)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    hwaddr entry = s->ring_base + (hwaddr)s->ring_next * RING_ENTRY_SIZE;
    uint8_t body[RING_VALID - 4] = { 0 };
    uint32_t valid;

    if (!s->ring_set) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-divo: packet 0x%x before the "
                      "host announced the ring\n", type);
        return;
    }
    if (pci_dma_read(pdev, entry + RING_VALID, &valid, 4) != MEMTX_OK ||
        valid) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-divo: ring entry %u is not free; "
                      "packet 0x%x dropped\n", s->ring_next, type);
        return;
    }
    stl_le_p(body, type);
    stl_le_p(body + 4, w1);
    stl_le_p(body + 8, w2);
    stl_le_p(&valid, 1);
    if (pci_dma_write(pdev, entry, body, sizeof(body)) != MEMTX_OK ||
        pci_dma_write(pdev, entry + RING_VALID, &valid, 4) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-divo: DMA to ring entry %u "
                      "failed\n", s->ring_next);
        return;
    }
    s->ring_next = (s->ring_next + 1) % RING_ENTRIES;
    s->ctl_reg[CTL_STATUS / 4] |= STATUS_INT;
    divo_update_irq(s);
}

static void divo_reply_fire(void *opaque)
{
    SGIDivoLincState *s = opaque;
    uint32_t n = s->pending_count;
    uint32_t i;

    s->pending_count = 0;
    for (i = 0; i < n; i++) {
        divo_post_now(s, s->pending[i][0], s->pending[i][1],
                      s->pending[i][2]);
    }
}

static void divo_post(SGIDivoLincState *s, uint32_t type, uint32_t w1,
                      uint32_t w2)
{
    if (s->pending_count == PENDING_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-divo: reply 0x%x dropped, the "
                      "host sent too many words at once\n", type);
        return;
    }
    s->pending[s->pending_count][0] = type;
    s->pending[s->pending_count][1] = w1;
    s->pending[s->pending_count][2] = w2;
    if (!s->pending_count++) {
        timer_mod(s->reply_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + REPLY_DELAY_NS);
    }
}

/* Address >> 7 in bits 40:0, bits 62:56 in 47:41; bit 63 is implied. */
static uint64_t divo_dma_address(uint64_t word)
{
    return (extract64(word, 0, 41) << 7) | (extract64(word, 41, 7) << 56) |
           BIT_ULL(63);
}

static void divo_read_block(SGIDivoLincState *s, uint64_t word, size_t len)
{
    g_autofree uint8_t *buf = g_malloc(len);

    if (pci_dma_read(PCI_DEVICE(s), divo_dma_address(word), buf, len) !=
        MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-divo: DMA read of a block at "
                      "0x%" PRIx64 " failed\n", divo_dma_address(word));
    }
}

/* "Firmware stand-in": the whole of the board's answer to a command word. */
static void divo_command(SGIDivoLincState *s, unsigned slot, uint64_t word)
{
    unsigned op = word >> OP_SHIFT;
    unsigned scratch = extract64(word, 49, 2);

    s->consumer = slot;
    /* The host's writer sleeps until the board reports its progress. */
    if (op != OP_RING_BASE && op != OP_RING_CONSUMED) {
        divo_post(s, PKT_CONSUMER, slot, 0);
    }
    switch (op) {
    case OP_WRITE_BYTE:
    case OP_WRITE_WORD:
    case OP_CONTROL:
    case OP_RING_COUNT:
    case OP_BUFFER_BASE:
    case OP_CLOCK_OFFSET:
        break;
    case OP_RING_BASE:
        s->ring_base = divo_dma_address(word);
        s->ring_next = 0;
        s->ring_set = true;
        divo_post(s, PKT_CONSUMER, slot, 0);
        break;
    case OP_RING_CONSUMED:
        s->ring_next = ((uint32_t)word + 1) % RING_ENTRIES;
        break;
    case OP_READ_WORD:
        divo_post(s, PKT_SYNC_WORD, 0, 0);
        break;
    case OP_READ_HALF:
        divo_post(s, PKT_SYNC_HALF, 0, 0);
        break;
    case OP_BLOCK_SEND:
        divo_read_block(s, word, extract64(word, 51, 7) * 8);
        divo_post(s, PKT_BLOCK_DONE, 0, scratch);
        break;
    case OP_LOAD_FIRST ... OP_LOAD_AUDIO:
        divo_read_block(s, word, BLOCK_UNIT);
        divo_post(s, op == OP_LOAD_AUDIO ? PKT_AUDIO_PACKER_DONE :
                  PKT_VIDEO_PACKER_DONE, scratch, 0);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-divo: mailbox opcode 0x%x dropped\n",
                      op);
        break;
    }
}

static MemTxResult mailbox_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size, MemTxAttrs attrs)
{
    SGIDivoLincState *s = opaque;
    unsigned slot = addr / MAILBOX_STRIDE;
    unsigned off = addr % MAILBOX_STRIDE;

    if (size == 4 && off == 0) {
        s->mailbox_hi = val;
    } else if (size == 4 && off == 4) {
        divo_command(s, slot, ((uint64_t)s->mailbox_hi << 32) | val);
    } else {
        qemu_log_mask(LOG_UNIMP, "sgi-divo: mailbox write of %u bytes at "
                      "0x%" HWADDR_PRIx " ignored\n", size, addr);
    }
    return MEMTX_OK;
}

static uint64_t mailbox_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "sgi-divo: mailbox read at 0x%" HWADDR_PRIx
                  "\n", addr);
    return 0;
}

/* The Bridge splits a doubleword to this 32-bit window into word cycles. */
static uint32_t block_word(SGIDivoLincState *s, uint32_t *regs, hwaddr off)
{
    if (regs == s->ctl_reg && off == CTL_CONTROL) {
        return regs[0] & ~CTL_CONTROL_BUSY;
    }
    if (regs == s->vip_reg && off == 0) {
        return VIP_ID;
    }
    return regs[off / 4];
}

static uint64_t ctl_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIDivoLincState *s = opaque;

    return block_word(s, s->ctl_reg, addr);
}

static void ctl_write_word(SGIDivoLincState *s, hwaddr off, uint32_t val)
{
    uint32_t *reg = &s->ctl_reg[off / 4];

    switch (off) {
    case CTL_CONTROL:
        /* Bits 3:2 going clear is a release from reset: the ring drains. */
        if ((*reg & CTL_CONTROL_RESET) && !(val & CTL_CONTROL_RESET)) {
            s->consumer = CONSUMER_RESET;
        }
        *reg = val;
        break;
    case CTL_STATUS:
        /* Bit 20 is write-1-to-clear and the interrupt line; 21 never sets. */
        *reg = (*reg & (STATUS_INT | STATUS_PANIC) & ~val) |
               (val & ~(STATUS_INT | STATUS_PANIC));
        divo_update_irq(s);
        break;
    default:
        *reg = val;
        break;
    }
}

static void ctl_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    SGIDivoLincState *s = opaque;

    ctl_write_word(s, addr, val);
}

static uint64_t vip_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIDivoLincState *s = opaque;

    return block_word(s, s->vip_reg, addr);
}

static void vip_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    SGIDivoLincState *s = opaque;

    s->vip_reg[addr / 4] = val;
}

/* "Flash": one byte cycle of the command sequences. */
static void flash_cycle(SGIDivoLincState *s, hwaddr a, uint8_t d)
{

    uint32_t cmd = a & FLASH_ADDR_MASK;

    if (s->flash_program) {
        if (s->flash_sector == ~0u) {
            s->flash_sector = a >> FLASH_SECTOR_SHIFT;
        }
        if ((a >> FLASH_SECTOR_SHIFT) == s->flash_sector) {
            s->flash[a] = d;
            if ((a & 0xff) == 0xff) {
                s->flash_program = false;
            }
            return;
        }
        s->flash_program = false;
    }
    if (d == 0xf0) {
        s->flash_state = FLASH_IDLE;
        s->flash_id_mode = false;
        return;
    }
    switch (s->flash_state) {
    case FLASH_IDLE:
    case FLASH_ERASE1:
        if (cmd == FLASH_CMD1 && d == 0xaa) {
            s->flash_state++;
            return;
        }
        break;
    case FLASH_UNLOCK1:
    case FLASH_ERASE2:
        if (cmd == FLASH_CMD2 && d == 0x55) {
            s->flash_state++;
            return;
        }
        break;
    case FLASH_UNLOCK2:
        if (cmd == FLASH_CMD1) {
            s->flash_state = FLASH_IDLE;
            if (d == 0x90) {
                s->flash_id_mode = true;
            } else if (d == 0xa0) {
                s->flash_program = true;
                s->flash_sector = ~0u;
            } else if (d == 0x80) {
                s->flash_state = FLASH_ERASE1;
            } else {
                break;
            }
            return;
        }
        break;
    case FLASH_ERASE3:
        /* The library erases several sectors after one prefix. */
        if (d == 0x30) {
            memset(&s->flash[a & ~0xffULL], 0xff, 1 << FLASH_SECTOR_SHIFT);
            return;
        }
        break;
    default:
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-divo: flash write 0x%02x at 0x%"
                  HWADDR_PRIx " is not part of a command\n", d, a);
    s->flash_state = FLASH_IDLE;
}

static uint8_t flash_byte(SGIDivoLincState *s, hwaddr a)
{
    if (s->flash_id_mode && a < 2) {
        return a ? FLASH_ID & 0xff : FLASH_ID >> 8;
    }
    return s->flash[a];
}

/*
 * A byte or halfword reaches the device at its offset xor (4 - size), as
 * the Bridge places it; the flash is addressed by the host's offset.
 */
static uint64_t flash_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIDivoLincState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    if (size == 8) {
        return (flash_read(s, addr, 4) << 32) | flash_read(s, addr + 4, 4);
    }
    addr ^= size < 4 ? 4 - size : 0;
    for (i = 0; i < size; i++) {
        val = (val << 8) | flash_byte(s, addr + i);
    }
    return val;
}

static void flash_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned size)
{
    SGIDivoLincState *s = opaque;
    unsigned i;

    if (size == 8) {
        flash_write(s, addr, val >> 32, 4);
        flash_write(s, addr + 4, val, 4);
        return;
    }
    addr ^= size < 4 ? 4 - size : 0;
    for (i = 0; i < size; i++) {
        flash_cycle(s, addr + i, val >> (8 * (size - 1 - i)));
    }
}

static const MemoryRegionOps mailbox_ops = {
    .read = mailbox_read,
    .write_with_attrs = mailbox_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps ctl_ops = {
    .read = ctl_read,
    .write = ctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps vip_ops = {
    .read = vip_read,
    .write = vip_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps flash_ops = {
    .read = flash_read,
    .write = flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/* "Config space": dword 0x14 is the mailbox consumer index, not a BAR. */
static uint32_t divo_config_read(PCIDevice *dev, uint32_t addr, int len)
{
    SGIDivoLincState *s = SGI_DIVO_LINC(dev);
    uint32_t val = pci_default_read_config(dev, addr, len);
    uint32_t reg = s->consumer;
    int i;

    for (i = 0; i < len; i++) {
        if (addr + i >= CFG_CONSUMER && addr + i < CFG_CONSUMER + 4) {
            val = deposit32(val, 8 * i, 8,
                            reg >> (8 * (addr + i - CFG_CONSUMER)));
        }
    }
    return val;
}

static void divo_reset_hold(Object *obj, ResetType type)
{
    SGIDivoLincState *s = SGI_DIVO_LINC(obj);

    memset(s->ctl_reg, 0, sizeof(s->ctl_reg));
    memset(s->vip_reg, 0, sizeof(s->vip_reg));
    s->mailbox_hi = 0;
    s->consumer = CONSUMER_RESET;
    s->ring_base = 0;
    s->ring_next = 0;
    s->ring_set = false;
    s->pending_count = 0;
    timer_del(s->reply_timer);
    s->flash_state = FLASH_IDLE;
    s->flash_id_mode = false;
    s->flash_program = false;
    divo_update_irq(s);
}

static void divo_realize(PCIDevice *dev, Error **errp)
{
    SGIDivoLincState *s = SGI_DIVO_LINC(dev);
    Object *obj = OBJECT(s);

    dev->config[PCI_INTERRUPT_PIN] = 1;
    s->reply_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, divo_reply_fire, s);
    memset(s->flash, 0xff, sizeof(s->flash));

    memory_region_init(&s->window, obj, "sgi-divo.window", LINC_WINDOW_SIZE);
    if (!memory_region_init_ram(&s->ram, obj, "sgi-divo.storage",
                                LINC_WINDOW_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(&s->window, 0, &s->ram);
    memory_region_init_io(&s->mailbox, obj, &mailbox_ops, s,
                          "sgi-divo.mailbox", MAILBOX_SLOTS * MAILBOX_STRIDE);
    memory_region_add_subregion_overlap(&s->window, MAILBOX_BASE,
                                        &s->mailbox, 1);
    memory_region_init_io(&s->vip, obj, &vip_ops, s, "sgi-divo.vip", VIP_SIZE);
    memory_region_add_subregion_overlap(&s->window, VIP_BASE, &s->vip, 1);
    memory_region_init_io(&s->ctl, obj, &ctl_ops, s, "sgi-divo.ctl", CTL_SIZE);
    memory_region_add_subregion_overlap(&s->window, CTL_BASE, &s->ctl, 1);
    memory_region_init_io(&s->flash_mr, obj, &flash_ops, s, "sgi-divo.flash",
                          FLASH_SIZE);
    memory_region_add_subregion_overlap(&s->window, FLASH_BASE,
                                        &s->flash_mr, 1);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->window);
}

static void divo_exit(PCIDevice *dev)
{
    SGIDivoLincState *s = SGI_DIVO_LINC(dev);

    timer_free(s->reply_timer);
}

static const VMStateDescription vmstate_sgi_divo_linc = {
    .name = TYPE_SGI_DIVO_LINC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIDivoLincState),
        VMSTATE_UINT32_ARRAY(ctl_reg, SGIDivoLincState, CTL_SIZE / 4),
        VMSTATE_UINT32_ARRAY(vip_reg, SGIDivoLincState, VIP_SIZE / 4),
        VMSTATE_UINT32(mailbox_hi, SGIDivoLincState),
        VMSTATE_UINT32(consumer, SGIDivoLincState),
        VMSTATE_UINT64(ring_base, SGIDivoLincState),
        VMSTATE_UINT32(ring_next, SGIDivoLincState),
        VMSTATE_BOOL(ring_set, SGIDivoLincState),
        VMSTATE_TIMER_PTR(reply_timer, SGIDivoLincState),
        VMSTATE_UINT32(pending_count, SGIDivoLincState),
        VMSTATE_UINT32_2DARRAY(pending, SGIDivoLincState, PENDING_MAX, 3),
        VMSTATE_BUFFER(flash, SGIDivoLincState),
        VMSTATE_UINT32(flash_state, SGIDivoLincState),
        VMSTATE_BOOL(flash_id_mode, SGIDivoLincState),
        VMSTATE_BOOL(flash_program, SGIDivoLincState),
        VMSTATE_UINT32(flash_sector, SGIDivoLincState),
        VMSTATE_END_OF_LIST()
    },
};

static void divo_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI DIVO video board LINC (one video pipe)";
    dc->vmsd = &vmstate_sgi_divo_linc;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    pc->realize = divo_realize;
    pc->exit = divo_exit;
    pc->config_read = divo_config_read;
    pc->vendor_id = LINC_VENDOR_ID;
    pc->device_id = LINC_DEVICE_ID;
    pc->class_id = PCI_CLASS_MULTIMEDIA_VIDEO;
    rc->phases.hold = divo_reset_hold;
}

static const TypeInfo divo_types[] = {
    {
        .name = TYPE_SGI_DIVO_LINC,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(SGIDivoLincState),
        .class_init = divo_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(divo_types)
