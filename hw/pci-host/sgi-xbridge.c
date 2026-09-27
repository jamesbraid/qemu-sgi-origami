/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SGI XBridge PCI head. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "hw/1wire/crc.h"
#include "hw/1wire/ds2505.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/misc/sgi-xbridge-xbow.h"
#include "hw/mips/sgi-xio-port.h"
#include "hw/core/hotplug.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/core/irq.h"

#define TYPE_XBRIDGE_IOMMU "sgi-xbridge-iommu"

#define RAW_PCI_IRQS SGI_XBRIDGE_HEAD_RAW_PCI_IRQS
#define REG_COUNT SGI_XBRIDGE_HEAD_REG_COUNT
#define ATE_COUNT SGI_XBRIDGE_HEAD_ATE_COUNT

/*
 * The table's write range and the lower-word read view revision B adds; the
 * upper words read back at +4 in the write range itself.
 */
#define ATE_RAM         0x10000
#define ATE_RAM_END     (ATE_RAM + ATE_COUNT * 8)
#define ATE_LOW_VIEW    ATE_RAM_END
#define ATE_LOW_VIEW_END (ATE_LOW_VIEW + ATE_COUNT * 8)
#define REG(s, off) ((s)->reg[(off) / 8])

static void xbridge_packet(SGIXBridgeHeadState *s, unsigned pin, bool set)
{
    uint32_t upper = REG(s, 0x30);
    uint32_t host = REG(s, 0x130 + pin * 8);
    unsigned target = (upper >> 16) & 15;
    uint64_t addr = ((uint64_t)(upper & 0x3f) << 32) | REG(s, 0x38);
    MemTxResult result;

    addr |= (uint64_t)(host >> 8) << 38;
    if (!s->target[target]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge: interrupt target %x is absent\n", target);
        return;
    }
    address_space_stq_be(&s->endpoint[target].as, addr,
                         (host & 0xff) | (set ? 0x100 : 0),
                         MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge: interrupt destination refused write\n");
    }
}

static int xbridge_map_pci_irq(PCIDevice *dev, int pin)
{
    unsigned slot = PCI_SLOT(dev->devfn);

    if (slot >= 8 || pin < 0 || pin >= PCI_NUM_PINS) {
        /*
         * Unreachable with the root slot mask; keep a safe accounting sink.
         */
        return RAW_PCI_IRQS;
    }
    return slot * PCI_NUM_PINS + pin;
}

static void xbridge_pci_irq(void *opaque, int line, int level)
{
    SGIXBridgeHeadState *s = opaque;

    if (line < 0 || line >= RAW_PCI_IRQS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge: unsupported PCI interrupt source\n");
        return;
    }
    s->pci_levels = (s->pci_levels & ~(1U << line)) | (level ? 1U << line : 0);
    if (level && !qemu_irq_is_connected(s->pci_irq[line])) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-xbridge: PCI source %d has no board wiring\n", line);
    }
    qemu_set_irq(s->pci_irq[line], level);
}

static void xbridge_set_pin(void *opaque, int pin, int level)
{
    SGIXBridgeHeadState *s = opaque;
    uint8_t bit = 1U << pin;
    bool old = s->pins & bit;

    s->pins = (s->pins & ~bit) | (level ? bit : 0);
    REG(s, 0x100) = (REG(s, 0x100) & ~0xffU) | s->pins;
    if (old == !!level || !(REG(s, 0x108) & bit)) {
        return;
    }
    if (level || (REG(s, 0x118) & bit)) {
        xbridge_packet(s, pin, level);
    }
}

static MemTxResult xbridge_swap_read(void *opaque, hwaddr addr,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs)
{
    struct XBridgeTarget *target = opaque;
    uint8_t bytes[4] = { 0 };
    MemTxResult result;

    /* The implementation width is four; the helpers' wider cases are not. */
    assert(size <= sizeof(bytes));
    result = address_space_read(&target->as, addr ^ (4 - size), attrs,
                                bytes, size);
    *value = ldn_be_p(bytes, size);
    return result;
}

static MemTxResult xbridge_swap_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    struct XBridgeTarget *target = opaque;
    uint8_t bytes[4];

    assert(size <= sizeof(bytes));
    stn_be_p(bytes, size, value);
    return address_space_write(&target->as, addr ^ (4 - size), attrs,
                               bytes, size);
}

static const MemoryRegionOps xbridge_swap_ops = {
    .read_with_attrs = xbridge_swap_read,
    .write_with_attrs = xbridge_swap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static bool xbridge_slot_needs_correction(SGIXBridgeHeadState *s,
                                          unsigned slot)
{
    return !(s->self_ordered_slots & (1U << slot));
}

static uint64_t xbridge_flush_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void xbridge_flush_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    /* Transfers are synchronous; no buffered writes remain to drain. */
}

static const MemoryRegionOps xbridge_flush_ops = {
    .read = xbridge_flush_read,
    .write = xbridge_flush_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static IOMMUTLBEntry xbridge_translate(IOMMUMemoryRegion *iommu, hwaddr addr,
                                      IOMMUAccessFlags flag, int iommu_idx)
{
    struct XBridgeDMA *dma = container_of(iommu, struct XBridgeDMA, iommu);
    SGIXBridgeHeadState *s = dma->head;
    IOMMUTLBEntry entry = {
        .iova = addr & ~0xfffULL,
        .addr_mask = 0xfff,
        .perm = IOMMU_NONE,
    };
    unsigned widget;
    uint64_t destination;
    bool swap;

    if (addr >= 0x3fff0000 && addr < 0x40000000) {
        entry.target_as = &s->flush_as;
        entry.translated_addr = addr & 0xf000;
        entry.perm = IOMMU_RW;
        return entry;
    }
    if (addr >= 0x40000000 && addr <= 0x407fffff) {
        unsigned shift = (REG(s, 0x20) & (1U << 21)) ? 14 : 12;
        unsigned index = (addr & 0x3fffffff) >> shift;
        uint64_t ate;

        entry.addr_mask = (1ULL << shift) - 1;
        entry.iova = addr & ~entry.addr_mask;
        if (index >= SGI_XBRIDGE_HEAD_ATE_MAPPED || !(s->ate[index] & 1)) {
            REG(s, 0x100) |= 1U << 30;
            REG(s, 0x170) |= 1U << 30;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-xbridge: invalid page DMA at 0x%"
                          HWADDR_PRIx " (entry %u)\n", addr, index);
            return entry;
        }
        ate = s->ate[index];
        widget = (ate >> 8) & 15;
        destination = ate & 0x0000fffffffff000ULL;
        swap = ate & (1U << 5);
    } else if (addr >= 0x1000000000000000ULL) {
        widget = addr >> 60;
        destination = addr & 0x0000ffffffffffffULL;
        swap = addr & (1ULL << 55);
    } else if (addr >= 0x80000000 && addr <= UINT32_MAX) {
        uint32_t direct = REG(s, 0x80);

        widget = (direct >> 20) & 15;
        destination = ((uint64_t)(direct & 0x1ffff) << 31) |
                       (addr & 0x7fffffff);
        if (!(direct & 0x1ffff) && (direct & (1U << 17))) {
            destination += 0x20000000;
        }
        swap = REG(s, 0x200 + dma->slot * 8) & (1U << 19);
    } else {
        qemu_log_mask(addr >= 0x40000000 && addr < 0x80000000 ?
                      LOG_UNIMP : LOG_GUEST_ERROR,
                      "sgi-xbridge: unsupported DMA address 0x%"
                      HWADDR_PRIx "\n", addr);
        return entry;
    }
    if (!s->target[widget]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge: DMA target %x is absent\n", widget);
        return entry;
    }
    entry.target_as = swap || !xbridge_slot_needs_correction(s, dma->slot) ?
                      &s->endpoint[widget].as :
                      &s->endpoint[widget].swap_as;
    entry.translated_addr = destination & ~entry.addr_mask;
    entry.perm = IOMMU_RW;
    return entry;
}

static AddressSpace *xbridge_dma_as(PCIBus *bus, void *opaque, int devfn)
{
    SGIXBridgeHeadState *s = opaque;

    if (devfn < 0 || PCI_SLOT(devfn) >= G_N_ELEMENTS(s->dma)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge: unsupported requester devfn %x\n", devfn);
        return &s->denied_as;
    }
    return &s->dma[PCI_SLOT(devfn)].as;
}

static const PCIIOMMUOps xbridge_iommu_ops = {
    .get_address_space = xbridge_dma_as,
};

static int xbridge_notify_flags(IOMMUMemoryRegion *iommu,
                                IOMMUNotifierFlag old_flags,
                                IOMMUNotifierFlag new_flags, Error **errp)
{
    if (new_flags != IOMMU_NOTIFIER_NONE) {
        error_setg(errp, "XBridge does not support cached DMA mappings");
        return -EINVAL;
    }
    return 0;
}

static void xbridge_iommu_class_init(ObjectClass *oc, const void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(oc);

    imrc->translate = xbridge_translate;
    imrc->notify_flag_changed = xbridge_notify_flags;
}

static void xbridge_error_packet(SGIXBridgeHeadState *s)
{
    uint32_t upper = REG(s, 0x30);
    unsigned target = (upper >> 16) & 15;
    uint64_t addr = ((uint64_t)(upper & 0xffff) << 32) | REG(s, 0x38);
    MemTxResult result;

    if (!s->target[target]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge: error interrupt target %x is absent\n",
                      target);
        return;
    }
    address_space_stq_be(&s->endpoint[target].as, addr,
                         (REG(s, 0x128) & 0xff) | 0x100,
                         MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge: error destination refused write\n");
    }
}

/* Deferred register ranges contain valid addresses. */
static bool xbridge_deferred(hwaddr addr)
{
    return (addr >= 0x300 && addr < 0x600) ||
           (addr >= 0x30000 && addr < 0x30008) ||
           addr >= 0xc00000;
}

/*
 * The register file's defined slots below 0x300: the Bridge's map with the
 * XBridge's additions. A slot outside them is an undefined address, which the
 * firmware's sanity diagnostic provokes on purpose.
 */
static bool xbridge_register_defined(unsigned off)
{
    switch (off) {
    case 0x00 ... 0x70:
    case 0x80:
    case 0x90:
    case 0xa0:
    case 0xb0:
    case 0xc0 ... 0xd8:
    case 0x100 ... 0x1f8:
    case 0x200 ... 0x298:
        return true;
    default:
        return false;
    }
}

/*
 * Bridge-half board identity at offset 0xb0. A populated head answers as a
 * firmware-compatibility choice; an unpopulated head stays inert. The
 * selected rev-A XBridge contract calls this offset unused, so a modeled
 * response does not establish a physical IP45 connection.
 */
#define XBRIDGE_NIC             0x0b0
#define XBRIDGE_NIC_LEVEL       (1u << 0)
#define XBRIDGE_NIC_DONE        (1u << 1)
#define XBRIDGE_NIC_SAMPLE_SHIFT 2
#define XBRIDGE_NIC_SAMPLE_MASK (0xffu << XBRIDGE_NIC_SAMPLE_SHIFT)
#define XBRIDGE_NIC_PULSE_SHIFT 10
#define XBRIDGE_NIC_PULSE_MASK  (0x3ffu << XBRIDGE_NIC_PULSE_SHIFT)

#define XBRIDGE_NIC_ROM_FAMILY   0x0b
#define XBRIDGE_NIC_PAGE_SIZE    32
#define XBRIDGE_NIC_PAGES        2
#define XBRIDGE_NIC_ROM_SIZE     (XBRIDGE_NIC_PAGE_SIZE * XBRIDGE_NIC_PAGES)
#define XBRIDGE_NIC_SERIAL_MASK  MAKE_64BIT_MASK(0, 48)

/*
 * SGI_XBRIDGE_NIC_PLACEHOLDER_PART/_SERIAL (sgi-xbridge.h): the identity
 * bytes a real IP45 IO-8 board presents have not been observed on any board.
 * Family 0x0b matches the one board-identity NIC family confirmed for this
 * chip family (the Bridge's own part, OpenBSD's family-0x0b read); carrying
 * it to the IO-8 is inferred, not observed, for this specific board.
 */

static const char xbridge_onewire_image[XBRIDGE_NIC_ROM_SIZE] = {
    [0]  = 0x01, ' ', ' ', ' ', ' ', ';',
    [6]  = 'P', 'a', 'r', 't', ':',
    [11] = ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    [23] = ';', ' ', ' ', ' ', ' ', ' ', ' ',
    [32] = ' ', ' ', ' ', ' ', ' ', ' ',
};

static void xbridge_onewire_build_rom(uint8_t rom[XBRIDGE_NIC_ROM_SIZE],
                                      const char *board_part)
{
    unsigned int page;

    memcpy(rom, xbridge_onewire_image, XBRIDGE_NIC_ROM_SIZE);
    memcpy(&rom[11], board_part, 12);

    for (page = 0; page < XBRIDGE_NIC_PAGES; page++) {
        uint8_t *at = &rom[page * XBRIDGE_NIC_PAGE_SIZE];
        uint16_t crc = ~onewire_crc16(0, at, XBRIDGE_NIC_PAGE_SIZE - 2);

        at[XBRIDGE_NIC_PAGE_SIZE - 2] = crc & 0xff;
        at[XBRIDGE_NIC_PAGE_SIZE - 1] = crc >> 8;
    }
}

/* One bit slot; the guest polls the write's own readback for completion. */
static void xbridge_onewire_slot(SGIXBridgeHeadState *s, uint32_t val)
{
    uint32_t *held = &REG(s, XBRIDGE_NIC);
    uint32_t pulse = (val & XBRIDGE_NIC_PULSE_MASK) >> XBRIDGE_NIC_PULSE_SHIFT;
    uint32_t sample = (val & XBRIDGE_NIC_SAMPLE_MASK) >> XBRIDGE_NIC_SAMPLE_SHIFT;
    bool level = sgi_onewire_slot(s->onewire_bus, pulse, sample);

    *held &= ~XBRIDGE_NIC_LEVEL;
    *held |= XBRIDGE_NIC_DONE | (level ? XBRIDGE_NIC_LEVEL : 0);
}

static void xbridge_invalid(SGIXBridgeHeadState *s, hwaddr addr)
{
    bool first = !(REG(s, 0x100) & (1U << 24));

    REG(s, 0x100) |= 1U << 24;
    REG(s, 0x170) |= 1U << 24;
    if (first) {
        /* The widget-relative address; through the chip at most 44 bits. */
        REG(s, 0x10) = addr >> 32;
        REG(s, 0x18) = addr;
        if (REG(s, 0x108) & (1U << 24)) {
            xbridge_error_packet(s);
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-xbridge: invalid access at 0x%" HWADDR_PRIx "\n", addr);
}

static uint32_t xbridge_config_address(SGIXBridgeHeadState *s,
                                       hwaddr addr, unsigned size)
{
    uint32_t devfn;
    unsigned reg = addr & 0xff;
    unsigned bus = 0;

    if (addr < 0x28000) {
        devfn = PCI_DEVFN((addr - 0x20000) >> 12, (addr >> 8) & 7);
    } else {
        bus = (REG(s, 0xc8) >> 16) & 0xff;
        devfn = (REG(s, 0xc8) >> 8) & 0xf8;
        devfn |= (addr >> 8) & 7;
    }
    reg ^= 4 - size;
    return (bus << 16) | (devfn << 8) | reg;
}

static void xbridge_reverse(uint8_t *bytes, unsigned size)
{
    unsigned i;

    for (i = 0; i < size / 2; i++) {
        uint8_t v = bytes[i];

        bytes[i] = bytes[size - 1 - i];
        bytes[size - 1 - i] = v;
    }
}

/*
 * The widget's space above its 16 MB face: two 1 GB aliases of PCI memory,
 * for 32-bit and 64-bit targets, and the 4 GB of PCI I/O space, the offset
 * within a range being the PCI address. Everything else is an undefined
 * address.
 */
#define WIDGET_SPACE (1ULL << 44)
#define FACE_SIZE 0x1000000
#define MEM_ALIAS_SIZE (1ULL << 30)
#define CONTROL_MEM_SWAP (1U << 22)
#define CONTROL_IO_SWAP (1U << 23)

static const struct {
    const char *name;
    hwaddr base;
    uint64_t size;
    bool io;
    unsigned width;
    uint32_t swap;
} xbridge_ranges[SGI_XBRIDGE_HEAD_RANGES] = {
    { "xbridge.pci-mem32", 0x40000000, MEM_ALIAS_SIZE, false, 4,
      CONTROL_MEM_SWAP },
    { "xbridge.pci-mem64", 0x80000000, MEM_ALIAS_SIZE, false, 8,
      CONTROL_MEM_SWAP },
    { "xbridge.pci-io-space", 0x100000000ULL, 1ULL << 32, true, 4,
      CONTROL_IO_SWAP },
};

/*
 * Placement follows the control register's swap bit for the space, not a
 * device window's. With the bit clear a narrow access lands mirrored within
 * the target's width, as a window places it without its swap bit. That the
 * 64-bit alias mirrors within eight bytes is an inference from the alias
 * naming the target width.
 */
static MemTxResult xbridge_range_rw(struct XBridgeRange *range, hwaddr addr,
                                    uint8_t *bytes, unsigned size, bool write)
{
    bool swap = REG(range->head, 0x20) & range->swap;
    MemTxResult result;

    if (size > range->width) {
        result = xbridge_range_rw(range, addr, bytes, range->width, write);
        return result | xbridge_range_rw(range, addr + range->width,
                                         bytes + range->width,
                                         size - range->width, write);
    }
    if (!swap) {
        addr ^= range->width - size;
        if (write) {
            xbridge_reverse(bytes, size);
        }
    }
    result = address_space_rw(range->as, addr, MEMTXATTRS_UNSPECIFIED,
                              bytes, size, write);
    if (!swap && !write) {
        xbridge_reverse(bytes, size);
    }
    return result;
}

static uint64_t xbridge_range_read(void *opaque, hwaddr addr, unsigned size)
{
    uint8_t bytes[8] = { 0 };

    xbridge_range_rw(opaque, addr, bytes, size, false);
    return ldn_be_p(bytes, size);
}

static void xbridge_range_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    uint8_t bytes[8];

    stn_be_p(bytes, size, value);
    xbridge_range_rw(opaque, addr, bytes, size, true);
}

static const MemoryRegionOps xbridge_range_ops = {
    .read = xbridge_range_read,
    .write = xbridge_range_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/* Nothing decodes here: the undefined-address rule, address recorded. */
static uint64_t xbridge_undefined_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    xbridge_invalid(opaque, addr);
    return 0;
}

static void xbridge_undefined_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    xbridge_invalid(opaque, addr);
}

static const MemoryRegionOps xbridge_undefined_ops = {
    .read = xbridge_undefined_read,
    .write = xbridge_undefined_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static MemTxResult xbridge_pio(SGIXBridgeHeadState *s, hwaddr addr,
                               uint8_t *bytes, unsigned size, bool write)
{
    unsigned slot;
    hwaddr offset, base;
    uint32_t dev;
    unsigned width;
    AddressSpace *as;
    MemTxResult result;
    bool needs_correction, swizzle, stream;

    if (addr < 0x600000) {
        slot = (addr - 0x200000) >> 21;
        offset = addr & 0x1fffff;
    } else {
        slot = 2 + ((addr - 0x600000) >> 20);
        offset = addr & 0xfffff;
    }
    dev = REG(s, 0x200 + slot * 8);
    width = dev & (1U << 22) ? 8 : 4;
    if (size > width) {
        result = xbridge_pio(s, addr, bytes, 4, write);
        return result | xbridge_pio(s, addr + 4, bytes + 4, 4, write);
    }
    base = dev & (dev & (1U << 12) ? 0x3ff : 0xfff);
    if (slot < 2) {
        base &= ~1ULL;
    }
    base <<= 20;
    as = dev & (1U << 12) ? &s->pci_mem_as : &s->pci_io_as;
    /*
     * With the device's own swap bit (bit 13) clear, a narrow access is
     * placed by offset exclusive-or; the value crosses unchanged. Set, it
     * is a byte stream; the value is mirrored and the offset is not
     * touched. The two corrections are alternatives keyed on that one bit,
     * never both at once; conflating them (as this function used to) nets to
     * a mirrored value on the placement path a self-ordered device does not
     * take, which is what a live BASEIO PROM crash traced back to (OHCI's
     * HcControl read 0xc0000000 instead of 0xc0, so usb_ohci_bus_attach never
     * saw UsbSuspend). The big-window path (xbridge_range_rw) already applies
     * only the placement correction and already matches this rule.
     */
    needs_correction = xbridge_slot_needs_correction(s, slot);
    swizzle = needs_correction && !(dev & (1U << 13));
    stream = needs_correction && (dev & (1U << 13));
    if (swizzle) {
        offset ^= width - size;
    }
    if (stream && write) {
        xbridge_reverse(bytes, size);
    }
    result = address_space_rw(as, base + offset, MEMTXATTRS_UNSPECIFIED,
                              bytes, size, write);
    if (stream && !write) {
        xbridge_reverse(bytes, size);
    }
    return result;
}

static uint64_t xbridge_config_read(SGIXBridgeHeadState *s, hwaddr addr,
                                    unsigned size)
{
    uint32_t cfg = xbridge_config_address(s, addr, size);
    PCIBus *bus = PCI_HOST_BRIDGE(s)->bus;

    if (!pci_find_device(bus, cfg >> 16, (cfg >> 8) & 0xff)) {
        REG(s, 0x170) |= 1U << 11;
    }
    return pci_data_read(bus, cfg, size);
}

static uint64_t xbridge_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIXBridgeHeadState *s = opaque;
    uint8_t bytes[8] = { 0 };

    if (addr >= 0x20000 && addr < 0x29000) {
        if (size <= 4) {
            return xbridge_config_read(s, addr, size);
        }
        /*
         * A doubleword is two configuration words, the lower address in the
         * upper half: what the Origin 300's io8bt reads a header as and
         * compares with the two word reads.
         */
        return xbridge_config_read(s, addr, 4) << 32 |
               xbridge_config_read(s, addr + 4, 4);
    }
    if (addr >= 0x200000 && addr < 0xc00000) {
        xbridge_pio(s, addr, bytes, size, false);
        return ldn_le_p(bytes, size);
    }
    if (size == 4 && (addr & 7) == 4) {
        if (addr >= ATE_RAM && addr < ATE_RAM_END) {
            return s->ate[(addr - ATE_RAM) / 8] >> 32;
        }
        if (addr >= ATE_LOW_VIEW && addr < ATE_LOW_VIEW_END) {
            return (uint32_t)s->ate[(addr - ATE_LOW_VIEW) / 8];
        }
        if (addr < 0x300 && xbridge_register_defined(addr & ~7)) {
            return REG(s, addr);
        }
    }
    if (xbridge_deferred(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-xbridge: read of deferred register 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
    xbridge_invalid(s, addr);
    return 0;
}

static void xbridge_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    SGIXBridgeHeadState *s = opaque;
    unsigned off = addr & ~7;
    uint32_t mask;
    uint8_t bytes[8];

    if (addr >= 0x20000 && addr < 0x29000) {
        if (size <= 4) {
            pci_data_write(PCI_HOST_BRIDGE(s)->bus,
                           xbridge_config_address(s, addr, size), value,
                           size);
            return;
        }
        /* The read's split, inferred for a write; no guest issues one. */
        pci_data_write(PCI_HOST_BRIDGE(s)->bus,
                       xbridge_config_address(s, addr, 4), value >> 32, 4);
        pci_data_write(PCI_HOST_BRIDGE(s)->bus,
                       xbridge_config_address(s, addr + 4, 4),
                       (uint32_t)value, 4);
        return;
    }
    if (addr >= 0x200000 && addr < 0xc00000) {
        stn_le_p(bytes, size, value);
        xbridge_pio(s, addr, bytes, size, true);
        return;
    }
    if (size == 8 && !(addr & 7) &&
        addr >= ATE_RAM && addr < ATE_RAM_END) {
        s->ate[(addr - ATE_RAM) / 8] = value & 0x0000ffffffffffffULL;
        return;
    }
    if (xbridge_deferred(addr)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-xbridge: write to deferred register 0x%"
                      HWADDR_PRIx "\n", addr);
        return;
    }
    if (size != 4 || (addr & 7) != 4 || addr >= 0x300 ||
        !xbridge_register_defined(off)) {
        xbridge_invalid(s, addr);
        return;
    }
    if (off >= 0x180 && off <= 0x1f8) {
        unsigned pin = ((off - 0x180) / 8) & 7;

        if (!(REG(s, 0x108) & (1U << pin))) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-xbridge: masked force semantics unresolved\n");
        } else if (off < 0x1c0 || (s->pins & (1U << pin))) {
            xbridge_packet(s, pin, true);
        }
        return;
    }
    switch (off) {
    case 0x20:
        mask = 0x0fe1f20f;
        value = (value & mask) | ((uint32_t)s->speed << 4);
        break;
    case 0x30:
        value &= 0xfffff;
        break;
    case 0x38:
        break;
    case 0x40:
        /*
         * Error command word: read-only; a write posts and changes nothing.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-xbridge: write of 0x%" PRIx64 " to the read-only "
                      "error command word register\n", value);
        return;
    case 0x48:
        /* Link configuration; writable storage, inert in XBridge mode. */
        value &= 0x03ffffff;
        break;
    case 0x100:
        return;
    case 0x70:
        value &= 0x00ffffff;
        break;
    case XBRIDGE_NIC:
        if (s->onewire_bus) {
            xbridge_onewire_slot(s, (uint32_t)value);
            return;
        }
        /* No board behind this head: same unimplemented drop as before. */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-xbridge: unimplemented write at 0x%x\n", off);
        return;
    case 0xa0:
        value &= 0x3ff7f;
        break;
    case 0xc0:
        value &= 0x1f03ff;
        break;
    case 0x120:
        value &= 0xffffff;
        break;
    case 0x280:
    case 0x288:
        break;
    case 0x128:
    case 0x118:
        value &= 0xff;
        break;
    case 0x28:
        value &= 0xfffff;
        break;
    case 0x80:
        value &= 0xf3ffff;
        break;
    case 0xc8:
        value &= 0x00fff800;
        break;
    case 0x110:
        if (value & (1U << 1)) {
            REG(s, 0x100) &= ~(1U << 30);
            REG(s, 0x170) &= ~(1U << 30);
        }
        if (value & (1U << 3)) {
            REG(s, 0x100) &= ~(1U << 24);
            REG(s, 0x170) &= ~(1U << 24);
        }
        if (value & (1U << 6)) {
            REG(s, 0x170) = 0;
        }
        return;
    case 0x108:
        value &= 0x7ffefeff;
        break;
    default:
        if (off >= 0x130 && off <= 0x168) {
            value &= 0x3ffff;
            break;
        }
        if (off >= 0x200 && off <= 0x238) {
            value &= 0x1eefffff;
            break;
        }
        qemu_log_mask(LOG_UNIMP,
                      "sgi-xbridge: unimplemented write at 0x%x\n", off);
        return;
    }
    REG(s, off) = value;
}

static const MemoryRegionOps xbridge_ops = {
    .read = xbridge_read,
    .write = xbridge_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void xbridge_reset_hold(Object *obj, ResetType type)
{
    SGIXBridgeHeadState *s = SGI_XBRIDGE_HEAD(obj);
    unsigned i;

    s->pins = 0;
    s->pci_levels = 0;
    for (i = 0; i < RAW_PCI_IRQS; i++) {
        qemu_set_irq(s->pci_irq[i], 0);
    }
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->ate, 0, sizeof(s->ate));
    REG(s, 0) = 0x2d002049;
    REG(s, 8) = 0x20;
    REG(s, 0x20) = 0x0f00320f | ((uint32_t)s->speed << 4);
    REG(s, 0x28) = 0xfffff;
    REG(s, 0x48) = 0x03ff1810;
    REG(s, 0x80) = 0x20000;
    REG(s, 0xa0) = 0x20008;
    REG(s, 0xc0) = 0x10000;
    /*
     * Window bases the specification leaves unset: firmware-sourced, the
     * stock PROM's sanity diagnostic asserts all eight, the Bridge's layout
     * of two 2 MB windows then six of 1 MB.
     */
    for (i = 0; i < 8; i++) {
        REG(s, 0x200 + i * 8) = 0x18001000 | (i < 2 ? 2 * i : i + 2);
    }
}

static void xbridge_realize(DeviceState *dev, Error **errp)
{
    SGIXBridgeHeadState *s = SGI_XBRIDGE_HEAD(dev);
    /* Two heads in one chip cannot both be "head.0". */
    g_autofree char *bus_name = s->bus_name ? g_strdup(s->bus_name) :
        g_strdup_printf("%s.0", dev->id ? dev->id :
                        object_get_canonical_path_component(OBJECT(s)));
    unsigned i;

    if (s->speed > 3) {
        error_setg(errp, "XBridge PCI speed encoding must be at most 3");
        return;
    }
    if (s->board_part) {
        const char *board_part_error =
            sgi_bridge_board_part_error(s->board_part);
        uint8_t rom[XBRIDGE_NIC_ROM_SIZE];
        DeviceState *part;

        if (board_part_error) {
            error_setg(errp, "the XBridge head %s", board_part_error);
            return;
        }
        if (s->onewire_serial & ~XBRIDGE_NIC_SERIAL_MASK) {
            error_setg(errp, "the XBridge head one-wire serial 0x%" PRIx64
                       " does not fit in 48 bits", s->onewire_serial);
            return;
        }
        /*
         * Composed once, like sgi-bridge.c's own part: an EPROM keeps its
         * contents across a reset, and the bytes become the part's own
         * migrated state once realized.
         */
        s->onewire_bus = onewire_create_bus(dev, "onewire");
        xbridge_onewire_build_rom(rom, s->board_part);
        part = qdev_new(TYPE_ONEWIRE_DS2505);
        object_property_add_child(OBJECT(s), "onewire-part", OBJECT(part));
        qdev_prop_set_uint8(part, "family", XBRIDGE_NIC_ROM_FAMILY);
        qdev_prop_set_uint64(part, "serial", s->onewire_serial);
        ds2505_set_image(part, rom, sizeof(rom));
        if (!qdev_realize_and_unref(part, BUS(s->onewire_bus), errp)) {
            return;
        }
    }
    for (i = 0; i < 16; i++) {
        struct XBridgeTarget *endpoint = &s->endpoint[i];
        g_autofree char *name = g_strdup_printf("xbridge.target-%x", i);

        if (!s->target[i]) {
            continue;
        }
        address_space_init(&endpoint->as, s->target[i], name);
        memory_region_init_io(&endpoint->swap, OBJECT(s), &xbridge_swap_ops,
                              endpoint, name, UINT64_MAX);
        endpoint->swap.disable_reentrancy_guard = true;
        address_space_init(&endpoint->swap_as, &endpoint->swap, name);
    }
    memory_region_init(&s->denied, OBJECT(s), "xbridge.denied", UINT64_MAX);
    address_space_init(&s->denied_as, &s->denied, "xbridge.denied");
    memory_region_init_io(&s->flush, OBJECT(s), &xbridge_flush_ops, s,
                          "xbridge.flush", 0x10000);
    s->flush.disable_reentrancy_guard = true;
    address_space_init(&s->flush_as, &s->flush, "xbridge.flush");
    for (i = 0; i < 8; i++) {
        g_autofree char *name = g_strdup_printf("xbridge.dma-%u", i);

        s->dma[i].head = s;
        s->dma[i].slot = i;
        memory_region_init_iommu(&s->dma[i].iommu, sizeof(s->dma[i].iommu),
                                 TYPE_XBRIDGE_IOMMU, OBJECT(s), name,
                                 UINT64_MAX);
        address_space_init(&s->dma[i].as, MEMORY_REGION(&s->dma[i].iommu),
                           name);
    }
    qdev_init_gpio_in(dev, xbridge_set_pin, SGI_XBRIDGE_HEAD_PINS);
    qdev_init_gpio_out_named(dev, s->pci_irq, "pci-intx", RAW_PCI_IRQS);
    memory_region_init(&s->pci_mem, OBJECT(s), "xbridge.pci-mem", UINT64_MAX);
    memory_region_init(&s->pci_io, OBJECT(s), "xbridge.pci-io", UINT64_MAX);
    address_space_init(&s->pci_mem_as, &s->pci_mem, "xbridge.pci-mem");
    address_space_init(&s->pci_io_as, &s->pci_io, "xbridge.pci-io");
    PCI_HOST_BRIDGE(s)->bus = pci_register_root_bus(dev, bus_name,
        xbridge_pci_irq, xbridge_map_pci_irq, s, &s->pci_mem, &s->pci_io,
        0, RAW_PCI_IRQS + 1, TYPE_PCI_BUS);
    qbus_set_hotplug_handler(BUS(PCI_HOST_BRIDGE(s)->bus), OBJECT(s));
    pci_setup_iommu(PCI_HOST_BRIDGE(s)->bus, &xbridge_iommu_ops, s);
    pci_bus_set_slot_reserved_mask(PCI_HOST_BRIDGE(s)->bus, ~0xffU);

    /* The widget's space: the face, the PCI ranges, undefined elsewhere. */
    memory_region_init(&s->widget, OBJECT(s), TYPE_SGI_XBRIDGE_HEAD,
                       WIDGET_SPACE);
    memory_region_init_io(&s->face, OBJECT(s), &xbridge_ops, s,
                          "xbridge.face", FACE_SIZE);
    memory_region_add_subregion(&s->widget, 0, &s->face);
    for (i = 0; i < SGI_XBRIDGE_HEAD_RANGES; i++) {
        struct XBridgeRange *range = &s->range[i];

        range->head = s;
        range->as = xbridge_ranges[i].io ? &s->pci_io_as : &s->pci_mem_as;
        range->width = xbridge_ranges[i].width;
        range->swap = xbridge_ranges[i].swap;
        memory_region_init_io(&range->mr, OBJECT(s), &xbridge_range_ops,
                              range, xbridge_ranges[i].name,
                              xbridge_ranges[i].size);
        memory_region_add_subregion(&s->widget, xbridge_ranges[i].base,
                                    &range->mr);
    }
    memory_region_init_io(&s->undefined, OBJECT(s), &xbridge_undefined_ops,
                          s, "xbridge.undefined", WIDGET_SPACE);
    memory_region_add_subregion_overlap(&s->widget, 0, &s->undefined, -1);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->widget);
}

static void xbridge_unrealize(DeviceState *dev)
{
    SGIXBridgeHeadState *s = SGI_XBRIDGE_HEAD(dev);

    unsigned i;

    for (i = 0; i < 16; i++) {
        if (s->target[i]) {
            address_space_destroy(&s->endpoint[i].as);
            address_space_destroy(&s->endpoint[i].swap_as);
        }
    }
    for (i = 0; i < 8; i++) {
        address_space_destroy(&s->dma[i].as);
    }
    address_space_destroy(&s->denied_as);
    address_space_destroy(&s->flush_as);
    address_space_destroy(&s->pci_mem_as);
    address_space_destroy(&s->pci_io_as);
}

static int xbridge_post_load(void *opaque, int version_id)
{
    SGIXBridgeHeadState *s = opaque;
    unsigned i;

    for (i = 0; i < RAW_PCI_IRQS; i++) {
        qemu_set_irq(s->pci_irq[i], (s->pci_levels >> i) & 1);
    }
    return 0;
}

static const VMStateDescription vmstate_xbridge = {
    .name = TYPE_SGI_XBRIDGE_HEAD,
    .version_id = 2,
    .post_load = xbridge_post_load,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pci_levels, SGIXBridgeHeadState),
        VMSTATE_UINT8(pins, SGIXBridgeHeadState),
        VMSTATE_UINT32_ARRAY(reg, SGIXBridgeHeadState, REG_COUNT),
        VMSTATE_UINT64_SUB_ARRAY(ate, SGIXBridgeHeadState, 0,
                                 SGI_XBRIDGE_HEAD_ATE_MAPPED),
        /* Version 2 added revision B's second 512 entries. */
        VMSTATE_SUB_ARRAY(ate, SGIXBridgeHeadState,
                          SGI_XBRIDGE_HEAD_ATE_MAPPED,
                          ATE_COUNT - SGI_XBRIDGE_HEAD_ATE_MAPPED, 2,
                          vmstate_info_uint64, uint64_t),
        VMSTATE_END_OF_LIST()
    }
};

static void xbridge_plug(const HotplugHandler *handler, DeviceState *dev,
                         Error **errp)
{
    SGIXBridgeHeadState *s = SGI_XBRIDGE_HEAD(handler);
    Object *obj = OBJECT(dev);
    Error *local_err = NULL;
    bool self_ordered;

    if (!object_dynamic_cast(obj, TYPE_PCI_DEVICE) ||
        !object_property_find(obj, SGI_BRIDGE_BYTE_LANE_SELF_ORDERED)) {
        return;
    }

    self_ordered = object_property_get_bool(
        obj, SGI_BRIDGE_BYTE_LANE_SELF_ORDERED, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    if (self_ordered) {
        s->self_ordered_slots |= 1U << PCI_SLOT(PCI_DEVICE(dev)->devfn);
    }
}

static bool xbridge_is_hotpluggable_bus(const HotplugHandler *handler, BusState *bus)
{
    return false;
}

static const Property xbridge_properties[] = {
    DEFINE_PROP_UINT8("pci-speed", SGIXBridgeHeadState, speed, 0),
    DEFINE_PROP_STRING("board-part", SGIXBridgeHeadState, board_part),
    DEFINE_PROP_STRING("bus-name", SGIXBridgeHeadState, bus_name),
    DEFINE_PROP_UINT64("onewire-serial", SGIXBridgeHeadState, onewire_serial,
                       SGI_XBRIDGE_NIC_PLACEHOLDER_SERIAL),
};

static void xbridge_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI XBridge PCI head";
    dc->user_creatable = true;
    dc->realize = xbridge_realize;
    dc->unrealize = xbridge_unrealize;
    dc->vmsd = &vmstate_xbridge;
    hc->plug = xbridge_plug;
    hc->is_hotpluggable_bus = xbridge_is_hotpluggable_bus;
    rc->phases.hold = xbridge_reset_hold;
    device_class_set_props(dc, xbridge_properties);
}

static void xbridge_init(Object *obj)
{
    SGIXBridgeHeadState *s = SGI_XBRIDGE_HEAD(obj);
    unsigned i;

    for (i = 0; i < 16; i++) {
        g_autofree char *name = g_strdup_printf("target-%x", i);

        object_property_add_link(obj, name, TYPE_MEMORY_REGION,
                                 (Object **)&s->target[i],
                                 qdev_prop_allow_set_link_before_realize, 0);
    }
}

static const TypeInfo xbridge_type = {
    .name = TYPE_SGI_XBRIDGE_HEAD,
    .parent = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(SGIXBridgeHeadState),
    .class_init = xbridge_class_init,
    .instance_init = xbridge_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_HOTPLUG_HANDLER },
        { },
    },
};

/*
 * The chip: the crossbar half at widget 0, the two heads at widgets 0xe and
 * 0xf, and the routing between them and
 * the one host port. The machine instantiates this and gets the two PCI root
 * buses; it never composes the halves
 * itself.
 */

#define CHIP_WIDGET_SHIFT 44
#define CHIP_WIDGET_SPACE (1ULL << CHIP_WIDGET_SHIFT)
#define CHIP_HEAD_E 0xe
#define CHIP_HEAD_F 0xf
#define CHIP_LINK_FIRST 8

/* The widgets a machine gets a view of, in SysBus MMIO region order. */
static const unsigned chip_view_widget[] = { 0, CHIP_HEAD_E, CHIP_HEAD_F };

struct SGIXBridgeState {
    SysBusDevice parent_obj;
    SGIXBridgeXbowState xbow;
    SGIXBridgeHeadState head[2];
    MemoryRegion xio;
    MemoryRegion unmapped;
    MemoryRegion view[ARRAY_SIZE(chip_view_widget)];
    MemoryRegion *fabric;
    qemu_irq head_reset[2];
    /*
     * The chip's own lines, and the sinks that re-drive a child's onto them.
     */
    qemu_irq head_intx[2][SGI_XBRIDGE_HEAD_RAW_PCI_IRQS];
    qemu_irq head_intx_sink[2][SGI_XBRIDGE_HEAD_RAW_PCI_IRQS];
    qemu_irq warm_reset_out[SGI_XBRIDGE_EXTERNAL_PORTS];
    qemu_irq warm_reset_sink[SGI_XBRIDGE_EXTERNAL_PORTS];
    uint8_t host_port;
    uint8_t pci_speed[2];
    char *board_part[2];
    /*
     * Prefixed to each head's bus name, so a second chip in one machine (a
     * two-chassis Origin 300's second IO-8) names its buses apart.
     */
    char *bus_prefix;
    uint64_t onewire_serial[2];
};

/* NULL for a widget this chip does not answer for, rather than asserting. */
static MemoryRegion *chip_widget_view(SGIXBridgeState *s, unsigned widget)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(chip_view_widget); i++) {
        if (chip_view_widget[i] == widget) {
            return &s->view[i];
        }
    }
    return NULL;
}

MemoryRegion *sgi_xbridge_widget_view(SGIXBridgeState *s, unsigned widget)
{
    MemoryRegion *view = chip_widget_view(s, widget);

    g_assert(view);
    return view;
}

PCIBus *sgi_xbridge_pci_bus(SGIXBridgeState *s, unsigned widget)
{
    assert(widget == CHIP_HEAD_E || widget == CHIP_HEAD_F);
    return PCI_HOST_BRIDGE(&s->head[widget - CHIP_HEAD_E])->bus;
}

/* Ports without a widget; a head answers for the whole of its own slot. */
static uint64_t chip_unmapped_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-xbridge: %u-byte read of widget %x offset 0x%"
                  PRIx64 " reaches nothing\n",
                  size, (unsigned)(addr >> CHIP_WIDGET_SHIFT),
                  (uint64_t)(addr & (CHIP_WIDGET_SPACE - 1)));
    return 0;
}

static void chip_unmapped_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-xbridge: %u-byte write to widget %x offset 0x%"
                  PRIx64 " reaches nothing\n",
                  size, (unsigned)(addr >> CHIP_WIDGET_SHIFT),
                  (uint64_t)(addr & (CHIP_WIDGET_SPACE - 1)));
}

static const MemoryRegionOps chip_unmapped_ops = {
    .read = chip_unmapped_read,
    .write = chip_unmapped_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static bool chip_link_host(Object *child, unsigned port, MemoryRegion *fabric,
                           Error **errp)
{
    g_autofree char *name = g_strdup_printf("target-%x", port);

    return object_property_set_link(child, name, OBJECT(fabric), errp);
}

/* A warm reset the crossbar forwards to an internal port resets that head. */
static void chip_head_reset(void *opaque, int line, int level)
{
    if (level) {
        device_cold_reset(DEVICE(opaque));
    }
}

/* A child's line re-driven as the chip's, under the chip's group name. */
static void chip_forward(void *opaque, int line, int level)
{
    qemu_irq *out = opaque;

    qemu_set_irq(out[line], level);
}

static void chip_head_pin(void *opaque, int pin, int level)
{
    qemu_set_irq(qdev_get_gpio_in(DEVICE(opaque), pin), level);
}

static void chip_warm_reset(void *opaque, int port, int level)
{
    qemu_set_irq(qdev_get_gpio_in_named(DEVICE(opaque), "warm-reset", port),
                 level);
}

static void chip_realize(DeviceState *dev, Error **errp)
{
    SGIXBridgeState *s = SGI_XBRIDGE(dev);
    unsigned i, n;

    if (!s->fabric) {
        error_setg(errp, "XBridge needs the fabric link to its host");
        return;
    }
    if (s->host_port < CHIP_LINK_FIRST || s->host_port > 0xf ||
        s->host_port == CHIP_HEAD_E || s->host_port == CHIP_HEAD_F) {
        error_setg(errp, "XBridge host port must be 8 to 0xd");
        return;
    }

    /* The host and the two heads are present; the other links are empty. */
    qdev_prop_set_uint8(DEVICE(&s->xbow), "links-present",
                        (1U << (s->host_port - CHIP_LINK_FIRST)) |
                        (1U << (CHIP_HEAD_E - CHIP_LINK_FIRST)) |
                        (1U << (CHIP_HEAD_F - CHIP_LINK_FIRST)));
    if (!chip_link_host(OBJECT(&s->xbow), s->host_port, s->fabric, errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(&s->xbow), errp)) {
        return;
    }
    memory_region_add_subregion(&s->xio, 0,
                                sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->xbow),
                                s->host_port - CHIP_LINK_FIRST));
    /* The external ports' forwards go out; the heads' are wired below. */
    for (n = 0; n < SGI_XBRIDGE_EXTERNAL_PORTS; n++) {
        s->warm_reset_sink[n] = qemu_allocate_irq(chip_forward,
                                                  s->warm_reset_out, n);
        qdev_connect_gpio_out_named(DEVICE(&s->xbow), "warm-reset-out", n,
                                    s->warm_reset_sink[n]);
    }

    for (i = 0; i < 2; i++) {
        unsigned widget = i ? CHIP_HEAD_F : CHIP_HEAD_E;

        qdev_prop_set_uint8(DEVICE(&s->head[i]), "pci-speed", s->pci_speed[i]);
        if (s->board_part[i]) {
            qdev_prop_set_string(DEVICE(&s->head[i]), "board-part",
                                 s->board_part[i]);
        }
        qdev_prop_set_uint64(DEVICE(&s->head[i]), "onewire-serial",
                             s->onewire_serial[i]);
        if (s->bus_prefix) {
            g_autofree char *bus_name = g_strdup_printf(
                "%shead-%c.0", s->bus_prefix, i ? 'f' : 'e');

            qdev_prop_set_string(DEVICE(&s->head[i]), "bus-name", bus_name);
        }
        if (!chip_link_host(OBJECT(&s->head[i]), s->host_port, s->fabric,
                            errp) ||
            !sysbus_realize(SYS_BUS_DEVICE(&s->head[i]), errp)) {
            return;
        }
        s->head_reset[i] = qemu_allocate_irq(chip_head_reset, &s->head[i], 0);
        qdev_connect_gpio_out_named(DEVICE(&s->xbow), "warm-reset-out",
                                    widget - CHIP_LINK_FIRST,
                                    s->head_reset[i]);
        for (n = 0; n < SGI_XBRIDGE_HEAD_RAW_PCI_IRQS; n++) {
            s->head_intx_sink[i][n] = qemu_allocate_irq(chip_forward,
                                                        s->head_intx[i], n);
            qdev_connect_gpio_out_named(DEVICE(&s->head[i]), "pci-intx", n,
                                        s->head_intx_sink[i][n]);
        }
        memory_region_add_subregion(&s->xio,
                                    (uint64_t)widget << CHIP_WIDGET_SHIFT,
                                    sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(&s->head[i]), 0));
    }
}

static void chip_unrealize(DeviceState *dev)
{
    SGIXBridgeState *s = SGI_XBRIDGE(dev);
    unsigned i, n;

    for (n = 0; n < SGI_XBRIDGE_EXTERNAL_PORTS; n++) {
        qemu_free_irq(s->warm_reset_sink[n]);
    }
    for (i = 0; i < 2; i++) {
        for (n = 0; n < SGI_XBRIDGE_HEAD_RAW_PCI_IRQS; n++) {
            qemu_free_irq(s->head_intx_sink[i][n]);
        }
        qemu_free_irq(s->head_reset[i]);
    }
}

/* Bus speeds as OpenBSD reads them on an IP45: 66 MHz at 0xe, 33 at 0xf. */
static const Property chip_properties[] = {
    DEFINE_PROP_UINT8("host-port", SGIXBridgeState, host_port, 0),
    DEFINE_PROP_UINT8("pci-speed-e", SGIXBridgeState, pci_speed[0], 1),
    DEFINE_PROP_UINT8("pci-speed-f", SGIXBridgeState, pci_speed[1], 0),
    /*
     * A head's own MicroLAN board-identity NIC (BRIDGE_NIC, offset 0xb0):
     * unset leaves that head's register inert, matching a widget with no
     * board behind it (the empty option slot at widget 14, by default).
     */
    DEFINE_PROP_STRING("bus-prefix", SGIXBridgeState, bus_prefix),
    DEFINE_PROP_STRING("board-part-e", SGIXBridgeState, board_part[0]),
    DEFINE_PROP_STRING("board-part-f", SGIXBridgeState, board_part[1]),
    DEFINE_PROP_UINT64("onewire-serial-e", SGIXBridgeState,
                       onewire_serial[0], SGI_XBRIDGE_NIC_PLACEHOLDER_SERIAL),
    DEFINE_PROP_UINT64("onewire-serial-f", SGIXBridgeState,
                       onewire_serial[1], SGI_XBRIDGE_NIC_PLACEHOLDER_SERIAL),
};

/* No state of its own; the children carry theirs. */
static const VMStateDescription vmstate_chip = {
    .name = TYPE_SGI_XBRIDGE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    }
};

/*
 * ---- The XIO port interface ----
 *
 * index is a widget number: 0 (the crossbar's own identity), 0xe or 0xf
 * (the heads). cpu_face() is sgi_xbridge_widget_view() itself, where a
 * peer's processor-addressed traffic at that widget position lands,
 * exactly what the machine linked by hand before. Nothing in this model
 * separately packet-addresses an XBridge widget (unlike a Crossbow, this
 * chip's internal crossbar-to-head wiring never leaves the windowed
 * form for a destination-prefixed packet one), so packet_face() answers
 * with the same region: the leaf case the interface header documents,
 * a widget whose registers nothing packet-addresses separately.
 */

static const char *chip_xio_port_describe(Object *port, unsigned int index)
{
    SGIXBridgeState *s = SGI_XBRIDGE(port);

    if (index == s->host_port) {
        return "the XBridge's host port";
    }
    switch (index) {
    case 0: return "the XBridge's own crossbar widget";
    case CHIP_HEAD_E: return "the XBridge's head E";
    case CHIP_HEAD_F: return "the XBridge's head F";
    default: return "an XBridge widget";
    }
}

static MemoryRegion *chip_xio_port_cpu_face(Object *port,
                                            unsigned int index)
{
    return chip_widget_view(SGI_XBRIDGE(port), index);
}

static void chip_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXioPortClass *pc = SGI_XIO_PORT_CLASS(oc);

    dc->desc = "SGI XBridge XIO-to-PCI bridge";
    dc->user_creatable = true;
    dc->realize = chip_realize;
    dc->unrealize = chip_unrealize;
    dc->vmsd = &vmstate_chip;
    device_class_set_props(dc, chip_properties);
    pc->describe = chip_xio_port_describe;
    pc->cpu_face = chip_xio_port_cpu_face;
    pc->packet_face = chip_xio_port_cpu_face;
}

static void chip_init(Object *obj)
{
    SGIXBridgeState *s = SGI_XBRIDGE(obj);
    DeviceState *dev = DEVICE(obj);
    unsigned i;

    object_initialize_child(obj, "xbow", &s->xbow, TYPE_SGI_XBRIDGE_XBOW);
    object_initialize_child(obj, "head-e", &s->head[0], TYPE_SGI_XBRIDGE_HEAD);
    object_initialize_child(obj, "head-f", &s->head[1], TYPE_SGI_XBRIDGE_HEAD);
    object_property_add_link(obj, "fabric", TYPE_MEMORY_REGION,
                             (Object **)&s->fabric,
                             qdev_prop_allow_set_link_before_realize, 0);

    /*
     * The space and its views exist before realize: a host links the views
     * while the chip links the host's region, so neither can go first.
     */
    memory_region_init(&s->xio, obj, "sgi-xbridge.xio",
                       16 * CHIP_WIDGET_SPACE);
    memory_region_init_io(&s->unmapped, obj, &chip_unmapped_ops, s,
                          "sgi-xbridge.unmapped", 16 * CHIP_WIDGET_SPACE);
    memory_region_add_subregion_overlap(&s->xio, 0, &s->unmapped, -1);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->xio);
    for (i = 0; i < ARRAY_SIZE(chip_view_widget); i++) {
        g_autofree char *name = g_strdup_printf("sgi-xbridge.widget-%x",
                                                chip_view_widget[i]);

        memory_region_init_alias(&s->view[i], obj, name, &s->xio,
                                 (uint64_t)chip_view_widget[i]
                                 << CHIP_WIDGET_SHIFT, CHIP_WIDGET_SPACE);
        sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->view[i]);
    }

    qdev_init_gpio_in_named_with_opaque(dev, chip_head_pin, &s->head[0],
                                        "head-e-pin", SGI_XBRIDGE_HEAD_PINS);
    qdev_init_gpio_in_named_with_opaque(dev, chip_head_pin, &s->head[1],
                                        "head-f-pin", SGI_XBRIDGE_HEAD_PINS);
    qdev_init_gpio_out_named(dev, s->head_intx[0], "head-e-pci-intx",
                             SGI_XBRIDGE_HEAD_RAW_PCI_IRQS);
    qdev_init_gpio_out_named(dev, s->head_intx[1], "head-f-pci-intx",
                             SGI_XBRIDGE_HEAD_RAW_PCI_IRQS);
    qdev_init_gpio_in_named_with_opaque(dev, chip_warm_reset, &s->xbow,
                                        "warm-reset",
                                        SGI_XBRIDGE_EXTERNAL_PORTS);
    qdev_init_gpio_out_named(dev, s->warm_reset_out, "warm-reset-out",
                             SGI_XBRIDGE_EXTERNAL_PORTS);
}

static const TypeInfo chip_type = {
    .name = TYPE_SGI_XBRIDGE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIXBridgeState),
    .class_init = chip_class_init,
    .instance_init = chip_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_SGI_XIO_PORT },
        { }
    },
};

static void xbridge_register_types(void)
{
    static const TypeInfo iommu_type = {
        .name = TYPE_XBRIDGE_IOMMU,
        .parent = TYPE_IOMMU_MEMORY_REGION,
        .class_init = xbridge_iommu_class_init,
    };

    type_register_static(&iommu_type);
    type_register_static(&xbridge_type);
    type_register_static(&chip_type);
}

type_init(xbridge_register_types)
