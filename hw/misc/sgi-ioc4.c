/*
 * SGI IOC4 revision 0x53, serial-console subset.
 *
 * The four byte-wide 16550A ports and their local interrupt controller are
 * modeled. The IP53 profile also selects serial ring DMA, through the engine
 * shared with IOC3 in hw/char/sgi-ioc-serial-dma.c, ATA register readback and
 * a byte-bus clock alias. The PS/2 keyboard and mouse controller is always
 * present with both ports attached; GPIO is absent.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/runstate.h"
#include "qapi/error.h"
#include "hw/char/serial.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/misc/sgi-ioc4.h"
#include "hw/pci-host/sgi-pic.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "exec/memop.h"
#include "migration/vmstate.h"
#include "system/memory.h"
#include "system/dma.h"

#define SGI_IOC4_PCI_STATUS_COMPAT 0x0220
#define SGI_IOC4_BAR_SIZE 0x100000
#define SGI_IOC4_UART_BASE 0x380
#define SGI_IOC4_NVRAM_BAR 0x80000
#define SGI_IOC4_NVRAM_SIZE 0x8000
#define SGI_IOC4_CLOCK_BAR 0x87ff8
#define SGI_IOC4_CLOCK_NATIVE 0x7f8
#define SGI_IOC4_CLOCK_SIZE 8
#define SGI_IOC4_LEGACY_NVRAM_SIZE 0x800
#define SGI_IOC4_KBC_BASE 0x200
#define SGI_IOC4_KBC_SIZE SGI_IOC3_KBC_MMIO_SIZE
#define SGI_IOC4_KBC_IRQ 0x40
#define SGI_IOC4_KBC_WRITE_PENDING 0x03
#define SGI_IOC4_KBC_RX_MASK 0xe0ffffff
#define SGI_IOC4_ATA_READBACK 0x148
#define SGI_IOC4_DMA_BASE 0x300
#define SGI_IOC4_DMA_PORT_BASE(n) (0x310 + 0x1c * (n))
#define SGI_IOC4_DMA_CONTROL(n) (SGI_IOC4_DMA_PORT_BASE(n) + 0x00)
#define SGI_IOC4_DMA_TX_PRODUCE(n) (SGI_IOC4_DMA_PORT_BASE(n) + 0x04)
#define SGI_IOC4_DMA_TX_CONSUME(n) (SGI_IOC4_DMA_PORT_BASE(n) + 0x08)
#define SGI_IOC4_DMA_RX_PRODUCE(n) (SGI_IOC4_DMA_PORT_BASE(n) + 0x0c)
#define SGI_IOC4_DMA_RX_CONSUME(n) (SGI_IOC4_DMA_PORT_BASE(n) + 0x10)
#define SGI_IOC4_DMA_RX_TIMER(n) (SGI_IOC4_DMA_PORT_BASE(n) + 0x14)
#define SGI_IOC4_DMA_SHADOW(n) (SGI_IOC4_DMA_PORT_BASE(n) + 0x18)
#define SGI_IOC4_SSCR_THRESHOLD 0x000001ff
#define SGI_IOC4_SSCR_TX_TIMER_BUSY 0x00010000
#define SGI_IOC4_SSCR_DRAIN     0x08000000
#define SGI_IOC4_SSCR_DMA_EN    0x10000000
#define SGI_IOC4_SSCR_PAUSE     0x20000000
#define SGI_IOC4_SSCR_PAUSE_STATE 0x40000000
#define SGI_IOC4_SSCR_RESET     0x80000000
#define SGI_IOC4_SSCR_MODELLED \
    (SGI_IOC4_SSCR_THRESHOLD | SGI_IOC4_SSCR_DRAIN | SGI_IOC4_SSCR_DMA_EN | \
     SGI_IOC4_SSCR_PAUSE | SGI_IOC4_SSCR_RESET)
#define SGI_IOC4_SRCIR_TIMER_ARM 0x80000000
#define SGI_IOC4_RING_1K 0x400
#define SGI_IOC4_RING_4K 0x1000
#define SGI_IOC4_TX_VALID 0x40
#define SGI_IOC4_TX_EXPLICIT 0x20
#define SGI_IOC4_TX_MCR 0x80
#define SGI_IOC4_TX_DELAY 0xc0
#define SGI_IOC4_DMA_SHADOW_OUTPUTS 0x1f000000
#define SGI_IOC4_DMA_SHADOW_LOOP 0x10000000
#define SGI_IOC4_DMA_OTHER_SHIFT 2
#define SGI_IOC4_DMA_ERROR_VALID 1
#define SGI_IOC4_DMA_ERROR_MULTIPLE 0x20
#define SGI_IOC4_DMA_ERROR_REQUESTER(n) ((n) << 1)

typedef struct SGIIOC4Class {
    PCIDeviceClass parent_class;
    ResettablePhases parent_phases;
} SGIIOC4Class;

static void sgi_ioc4_update_irq(SGIIOC4State *s)
{
    /*
     * The UARTs load before the interrupt words, and their post_load
     * signals transmit-ready and their interrupt line. Driven from the
     * half-loaded words, the line would fall and rise again under a
     * source that was pending all along, which the PIC would pass on as
     * a second interrupt. post_load drives it once, from the full state.
     */
    if (s->loading) {
        return;
    }
    pci_set_irq(&s->parent_obj,
                !!((s->serial_pending & s->serial_enable) |
                   (s->uart_level & s->serial_enable) |
                   (s->threshold_level & s->serial_enable) |
                   (s->tx_empty_level & s->serial_enable) |
                   (s->rx_full_level & s->serial_enable) |
                   (s->other_pending & s->other_enable)));
}

static void sgi_ioc4_kbc_irq(void *opaque, int n, int level)
{
    SGIIOC4State *s = opaque;

    (void)n;
    if (level) {
        s->other_pending |= SGI_IOC4_KBC_IRQ;
        sgi_ioc4_update_irq(s);
    }
}

/*
 * A UART pass-through source follows the UART's own interrupt level: what
 * quiets it is the UART register access that clears the UART's interrupt,
 * and a write to the status register cannot clear it. Linux's IOC3 header
 * treats these bits as levels, and IOC4 follows its sibling.
 */
static void sgi_ioc4_uart_irq(void *opaque, int n, int level)
{
    SGIIOC4State *s = opaque;
    uint32_t bit = 0x40u << (8 * n);

    if (level) {
        s->uart_level |= bit;
    } else {
        s->uart_level &= ~bit;
    }
    sgi_ioc4_update_irq(s);

    /*
     * On every level, not assertion alone: a ring driver receives with the
     * UART's own interrupts disabled.
     */
    sgi_ioc_sdma_kick(&s->sdma, n);
}

/*
 * The serial ring DMA engine is shared with IOC3. What stays here is what
 * differs: the register decode, where the interrupt sources sit, the error
 * capture registers, and the policies only this model has so far.
 */
static const SGIIOCSerialDMAParams sgi_ioc4_sdma_params = {
    .name = "sgi-ioc4",
    /* IOC4's header does not define the packer counts; unknown here. */
    .held_counts = false,
    /* "Serial halted by mem err", in the header's other-interrupt word. */
    .halt_on_dma_error = true,
};

static void sgi_ioc4_sdma_raise(void *opaque, unsigned port,
                                SGIIOCSerialDMASource src)
{
    SGIIOC4State *s = opaque;

    s->serial_pending |= 1u << (8 * port + src);
    sgi_ioc4_update_irq(s);
}

static void sgi_ioc4_sdma_set_level(void *opaque, unsigned port,
                                    SGIIOCSerialDMASource src, bool level)
{
    SGIIOC4State *s = opaque;
    uint32_t bit = 1u << (8 * port + src);
    uint32_t *word;

    switch (src) {
    case SGI_IOC_SDMA_SRC_RX_HIGH:
        word = &s->threshold_level;
        break;
    case SGI_IOC_SDMA_SRC_TX_EMPTY:
        word = &s->tx_empty_level;
        break;
    case SGI_IOC_SDMA_SRC_RX_FULL:
        word = &s->rx_full_level;
        break;
    default:
        g_assert_not_reached();
    }
    *word = (*word & ~bit) | (level ? bit : 0);
    sgi_ioc4_update_irq(s);
}

/* The DMA profile still selects whether the engine reaches the UARTs. */
static bool sgi_ioc4_sdma_reachable(void *opaque, unsigned port)
{
    SGIIOC4State *s = opaque;

    return s->serial_dma_experimental;
}

/*
 * The first failure captures its address, with the low six bits dropped,
 * and its requester: 2 * port for a transmit fetch, one more for a receive
 * post. Later ones only set the multiple-error bit.
 */
static void sgi_ioc4_sdma_dma_error(void *opaque, unsigned port, bool rx,
                                    hwaddr address)
{
    SGIIOC4State *s = opaque;
    unsigned requester = 2 * port + rx;

    if (!(s->dma_error_low & SGI_IOC4_DMA_ERROR_VALID)) {
        s->dma_error_low = (address & UINT32_C(0xffffffc0)) |
                           SGI_IOC4_DMA_ERROR_REQUESTER(requester) |
                           SGI_IOC4_DMA_ERROR_VALID;
        s->dma_error_high = (uint64_t)address >> 32;
    } else {
        s->dma_error_low |= SGI_IOC4_DMA_ERROR_MULTIPLE;
    }
    s->other_pending |= 1u << (port + SGI_IOC4_DMA_OTHER_SHIFT);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-ioc4: serial DMA %s failed for port %u at 0x%"
                  HWADDR_PRIx "\n", rx ? "write" : "read", port, address);
    sgi_ioc4_update_irq(s);
}

static const SGIIOCSerialDMAOps sgi_ioc4_sdma_ops = {
    .raise = sgi_ioc4_sdma_raise,
    .set_level = sgi_ioc4_sdma_set_level,
    .reachable = sgi_ioc4_sdma_reachable,
    .dma_error = sgi_ioc4_sdma_dma_error,
};

static void sgi_ioc4_uart_tx_ready(void *opaque, int n, int level)
{
    SGIIOC4State *s = opaque;

    sgi_ioc_sdma_set_tx_ready(&s->sdma, n, level);
}

static uint32_t sgi_ioc4_sdma_control(SGIIOC4State *s, unsigned port)
{
    return s->sdma.port[port].reg[SGI_IOC_SDMA_CONTROL / 4];
}

static uint32_t sgi_ioc4_sdma_reg(SGIIOC4State *s, unsigned port, hwaddr offset)
{
    return s->sdma.port[port].reg[offset / 4];
}

/* Nothing queued, held or pending on the port: the model's restriction. */
static bool sgi_ioc4_port_quiescent(SGIIOC4State *s, unsigned port)
{
    SGIIOCSerialDMAPort *p = &s->sdma.port[port];

    return sgi_ioc4_sdma_reg(s, port, SGI_IOC_SDMA_TX_PRODUCE) ==
           sgi_ioc4_sdma_reg(s, port, SGI_IOC_SDMA_TX_CONSUME) &&
           sgi_ioc4_sdma_reg(s, port, SGI_IOC_SDMA_RX_PRODUCE) ==
           sgi_ioc4_sdma_reg(s, port, SGI_IOC_SDMA_RX_CONSUME) &&
           !p->tx_fetched && !p->tx_delay_due && !p->packed && !p->armed &&
           !p->drain_pending;
}

static bool sgi_ioc4_pair_quiescent(SGIIOC4State *s, unsigned pair)
{
    unsigned i;

    for (i = pair * 2; i < pair * 2 + 2; i++) {
        if ((sgi_ioc4_sdma_control(s, i) &
             (SGI_IOC4_SSCR_DMA_EN | SGI_IOC4_SSCR_PAUSE)) ||
            sgi_ioc4_sdma_reg(s, i, SGI_IOC_SDMA_TX_PRODUCE) ||
            sgi_ioc4_sdma_reg(s, i, SGI_IOC_SDMA_TX_CONSUME) ||
            sgi_ioc4_sdma_reg(s, i, SGI_IOC_SDMA_RX_PRODUCE) ||
            sgi_ioc4_sdma_reg(s, i, SGI_IOC_SDMA_RX_CONSUME) ||
            !sgi_ioc4_port_quiescent(s, i)) {
            return false;
        }
    }
    return true;
}

/*
 * The DMA profile used to refuse these mode switches. They are model
 * policy, not hardware, and with the engine driving the 16550 the UART
 * orders PIO and DMA itself, so they are only reported now.
 */
static void sgi_ioc4_control_check(SGIIOC4State *s, unsigned n, uint32_t value)
{
    uint32_t old = sgi_ioc4_sdma_control(s, n);
    bool want_dma = value & SGI_IOC4_SSCR_DMA_EN;
    unsigned pair = n / 2;
    uint64_t base = ((uint64_t)s->sdma.base_hi[pair] << 32 |
                     s->sdma.base_lo[pair]) & ~UINT64_C(1);
    unsigned size = (s->sdma.base_lo[pair] & 1) ? SGI_IOC4_RING_4K
                                                : SGI_IOC4_RING_1K;
    SerialState *uart = &s->uart[n];

    if ((value & SGI_IOC4_SSCR_RESET) ||
        !(old & SGI_IOC4_SSCR_DMA_EN) == !want_dma) {
        return;
    }
    if (want_dma && (base & (size * 4 - 1))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc4: DMA enabled on port %u with pair base 0x%"
                      PRIx64 " misaligned for its rings\n", n, base);
    }
    if (want_dma && (!fifo8_is_empty(&uart->recv_fifo) ||
                     !fifo8_is_empty(&uart->xmit_fifo))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc4: DMA enabled on port %u with PIO data in the "
                      "UART's FIFOs\n", n);
    }
    if (!want_dma && !sgi_ioc4_port_quiescent(s, n)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc4: DMA disabled on port %u with ring work "
                      "pending\n", n);
    }
}

/* Ring bases at 0x300-0x30c, then seven registers a port from 0x310. */
static bool sgi_ioc4_sdma_decode(hwaddr addr, bool *base, unsigned *index,
                                 hwaddr *offset)
{
    if (addr & 3) {
        return false;
    }
    if (addr >= SGI_IOC4_DMA_BASE && addr < SGI_IOC4_DMA_PORT_BASE(0)) {
        *base = true;
        *index = (addr - SGI_IOC4_DMA_BASE) / 8;
        *offset = addr & 4;
        return true;
    }
    if (addr >= SGI_IOC4_DMA_PORT_BASE(0) &&
        addr < SGI_IOC4_DMA_PORT_BASE(SGI_IOC4_PORTS)) {
        *base = false;
        *index = (addr - SGI_IOC4_DMA_PORT_BASE(0)) / SGI_IOC_SDMA_PORT_STRIDE;
        *offset = (addr - SGI_IOC4_DMA_PORT_BASE(0)) % SGI_IOC_SDMA_PORT_STRIDE;
        return true;
    }
    return false;
}

static uint32_t sgi_ioc4_sdma_read(SGIIOC4State *s, bool base, unsigned index,
                                   hwaddr offset)
{
    if (base) {
        /* Low word first on IOC4, the reverse of IOC3. */
        return sgi_ioc_sdma_base_read(&s->sdma, index, offset == 4);
    }
    return sgi_ioc_sdma_port_read(&s->sdma, index, offset);
}

static void sgi_ioc4_sdma_write(SGIIOC4State *s, bool base, unsigned index,
                                hwaddr offset, uint32_t value)
{
    if (base) {
        if (!sgi_ioc4_pair_quiescent(s, index)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc4: DMA base of pair %u changed while it is "
                          "active\n", index);
        }
        sgi_ioc_sdma_base_write(&s->sdma, index, offset == 4, value);
        return;
    }
    switch (offset) {
    case SGI_IOC_SDMA_CONTROL:
        if (value & ~SGI_IOC4_SSCR_MODELLED) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-ioc4: serial control bits 0x%08x on port %u "
                          "are stored but not modelled\n",
                          value & ~SGI_IOC4_SSCR_MODELLED, index);
        }
        sgi_ioc4_control_check(s, index, value);
        break;
    case SGI_IOC_SDMA_TX_PRODUCE:
    case SGI_IOC_SDMA_RX_CONSUME: {
        uint32_t size = (s->sdma.base_lo[index / 2] & 1) ? SGI_IOC4_RING_4K
                                                         : SGI_IOC4_RING_1K;
        uint32_t keep = (size - 8) |
                        (offset == SGI_IOC_SDMA_RX_CONSUME ?
                         SGI_IOC4_SRCIR_TIMER_ARM : 0);

        if (value & ~keep) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc4: %s 0x%08x on port %u has bits outside "
                          "the pointer field\n",
                          offset == SGI_IOC_SDMA_TX_PRODUCE ? "TX producer"
                                                            : "RX consumer",
                          value, index);
        }
        break;
    }
    case SGI_IOC_SDMA_RX_TIMER:
        if (value & ~0xfff) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-ioc4: unsupported RX timer bits 0x%08x on "
                          "port %u\n", value & ~0xfff, index);
        }
        break;
    }
    sgi_ioc_sdma_port_write(&s->sdma, index, offset, value);
}

static uint64_t sgi_ioc4_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC4State *s = opaque;
    bool base;
    unsigned index;
    hwaddr offset;

    sgi_ioc_sdma_catch_up(&s->sdma);

    if (addr <= 0x04) {
        return addr == 0 ? s->dma_error_low : s->dma_error_high;
    }
    if (sgi_ioc4_sdma_decode(addr, &base, &index, &offset)) {
        return sgi_ioc4_sdma_read(s, base, index, offset);
    }
    if (addr == 0x20 && s->serial_dma_experimental) {
        return sgi_ioc_sdma_idle(&s->sdma) ? 0x80 : 0;
    }
    if (addr == SGI_IOC4_ATA_READBACK && s->ata_readback_compat) {
        return s->ata_readback;
    }
    switch (addr) {
    case 0x08:
        return s->serial_pending | s->uart_level | s->threshold_level |
               s->tx_empty_level | s->rx_full_level;
    case 0x0c:
        return s->other_pending;
    case 0x10:
        return s->serial_enable;
    case 0x14:
        return s->other_enable;
    case 0x24:
        return s->clock_hz == 66666667;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: read of unsupported BAR offset 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void sgi_ioc4_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    SGIIOC4State *s = opaque;
    bool base;
    unsigned index;
    hwaddr offset;

    sgi_ioc_sdma_catch_up(&s->sdma);

    if (addr <= 0x04) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: write to read-only DMA error address at 0x%"
                      HWADDR_PRIx "\n", addr);
        return;
    }
    if (sgi_ioc4_sdma_decode(addr, &base, &index, &offset)) {
        sgi_ioc4_sdma_write(s, base, index, offset, val);
        return;
    }
    if (addr == 0x20 && s->serial_dma_experimental) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: write to read-only serial status at 0x20\n");
        return;
    }
    if (addr == SGI_IOC4_ATA_READBACK && s->ata_readback_compat) {
        s->ata_readback = val;
        return;
    }
    switch (addr) {
    case 0x08:
        s->serial_pending &= ~(uint32_t)val;
        break;
    case 0x0c:
        s->other_pending &= ~(uint32_t)val;
        break;
    case 0x10:
        s->serial_enable |= (uint32_t)val;
        break;
    case 0x14:
        s->other_enable |= (uint32_t)val;
        break;
    case 0x18:
        s->serial_enable &= ~(uint32_t)val;
        break;
    case 0x1c:
        s->other_enable &= ~(uint32_t)val;
        break;
    case 0x24:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: unsupported misc control write 0x%"
                      PRIx64 "\n", val);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: write to unsupported BAR offset 0x%"
                      HWADDR_PRIx "\n", addr);
        return;
    }
    sgi_ioc4_update_irq(s);
}

static bool sgi_ioc4_accepts(void *opaque, hwaddr addr, unsigned size,
                             bool is_write, MemTxAttrs attrs)
{
    SGIIOC4State *s = opaque;

    if (size != 4) {
        if (addr == SGI_IOC4_ATA_READBACK && s->ata_readback_compat) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc4: unsupported ATA readback %s width %u\n",
                          is_write ? "write" : "read", size);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc4: unsupported BAR %s width %u at 0x%"
                          HWADDR_PRIx "\n", is_write ? "write" : "read",
                          size, addr);
        }
        return false;
    }
    return true;
}

static const MemoryRegionOps sgi_ioc4_ops = {
    .read = sgi_ioc4_read,
    .write = sgi_ioc4_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = false, .accepts = sgi_ioc4_accepts },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static bool sgi_ioc4_kbc_accepts(void *opaque, hwaddr addr, unsigned size,
                                 bool is_write, MemTxAttrs attrs)
{
    if (size != 4 || (addr & 3)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc4: unsupported KBC %s width %u at 0x%"
                      HWADDR_PRIx "\n", is_write ? "write" : "read",
                      size, addr + SGI_IOC4_KBC_BASE);
        return false;
    }
    return true;
}

static MemTxResult sgi_ioc4_kbc_read(void *opaque, hwaddr addr, uint64_t *value,
                                     unsigned size, MemTxAttrs attrs)
{
    SGIIOC4State *s = opaque;
    uint64_t raw = 0;
    MemTxResult result;

    result = memory_region_dispatch_read(s->kbc_mmio, addr, &raw,
                                         size_memop(size) | MO_LE, attrs);
    if (addr == SGI_IOC3_KBC_CONTROL) {
        *value = raw & ~SGI_IOC4_KBC_WRITE_PENDING;
    } else {
        *value = raw & SGI_IOC4_KBC_RX_MASK;
    }
    return result;
}

static MemTxResult sgi_ioc4_kbc_write(void *opaque, hwaddr addr, uint64_t value,
                                      unsigned size, MemTxAttrs attrs)
{
    SGIIOC4State *s = opaque;

    if (addr == SGI_IOC3_KBC_CONTROL) {
        value &= ~SGI_IOC4_KBC_WRITE_PENDING;
    }
    return memory_region_dispatch_write(s->kbc_mmio, addr, value,
                                       size_memop(size) | MO_LE, attrs);
}

static const MemoryRegionOps sgi_ioc4_kbc_be_ops = {
    .read_with_attrs = sgi_ioc4_kbc_read,
    .write_with_attrs = sgi_ioc4_kbc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = false, .accepts = sgi_ioc4_kbc_accepts },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static bool sgi_ioc4_uart_accepts(void *opaque, hwaddr addr, unsigned size,
                                  bool is_write, MemTxAttrs attrs)
{
    if (size != 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc4: unsupported UART %s width %u at 0x%"
                      HWADDR_PRIx "\n", is_write ? "write" : "read",
                      size, addr);
        return false;
    }
    return true;
}

static uint64_t sgi_ioc4_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC4Port *p = opaque;

    return serial_io_ops.read(&p->ioc4->uart[p->index], addr, size);
}

static void sgi_ioc4_uart_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    SGIIOC4Port *p = opaque;
    SerialState *uart = &p->ioc4->uart[p->index];

    if (p->ioc4->serial_dma_experimental &&
        (sgi_ioc4_sdma_control(p->ioc4, p->index) & SGI_IOC4_SSCR_DMA_EN) &&
        addr == 0 && !(uart->lcr & 0x80)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc4: PIO transmit on port %u while DMA is "
                      "enabled\n", p->index);
    }
    serial_io_ops.write(uart, addr, val, size);
}

static const MemoryRegionOps sgi_ioc4_uart_ops = {
    .read = sgi_ioc4_uart_read,
    .write = sgi_ioc4_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* Preserve wide cycles until accepts can reject them. */
    .valid = { .min_access_size = 1, .max_access_size = 4,
               .unaligned = true, .accepts = sgi_ioc4_uart_accepts },
    .impl = { .min_access_size = 1, .max_access_size = 4,
              .unaligned = true },
};

static void sgi_ioc4_reset_hold(Object *obj, ResetType type)
{
    SGIIOC4State *s = (SGIIOC4State *)obj;
    SGIIOC4Class *ic = (SGIIOC4Class *)object_get_class(obj);
    int i;

    if (ic->parent_phases.hold) {
        ic->parent_phases.hold(obj, type);
    }

    s->loading = false;
    s->serial_pending = 0;
    s->serial_enable = 0;
    s->other_pending = 0;
    s->other_enable = 0;
    s->uart_level = 0;
    s->threshold_level = 0;
    s->tx_empty_level = 0;
    s->rx_full_level = 0;
    s->ata_readback = 0;
    if (s->pci_status_compat) {
        pci_set_word(s->parent_obj.config + PCI_STATUS,
                     SGI_IOC4_PCI_STATUS_COMPAT);
    }
    s->dma_error_low = s->dma_error_high = 0;
    sgi_ioc_sdma_reset(&s->sdma);
    for (i = 0; i < SGI_IOC4_PORTS; i++) {
        s->uart[i].baudbase = s->clock_hz / 16;
    }
    sgi_ioc4_update_irq(s);
}

static int sgi_ioc4_post_load(void *opaque, int version_id)
{
    SGIIOC4State *s = opaque;
    int i;

    for (i = 0; i < SGI_IOC4_PORTS; i++) {
        s->uart[i].baudbase = s->clock_hz / 16;
        serial_io_ops.write(&s->uart[i], 3, s->uart[i].lcr, 1);
        sgi_ioc_sdma_kick(&s->sdma, i);
    }
    sgi_ioc_sdma_refresh(&s->sdma);
    s->loading = false;
    sgi_ioc4_update_irq(s);
    return 0;
}

static int sgi_ioc4_pre_load(void *opaque)
{
    SGIIOC4State *s = opaque;

    s->loading = true;
    return 0;
}

static const VMStateDescription vmstate_ioc4 = {
    .name = "sgi-ioc4",
    .version_id = 3,
    .minimum_version_id = 3,
    .pre_load = sgi_ioc4_pre_load,
    .post_load = sgi_ioc4_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(dma_migration_mode, SGIIOC4State),
        VMSTATE_UINT8_EQUAL(pci_status_compat_mode, SGIIOC4State),
        VMSTATE_UINT8_EQUAL(ata_readback_compat_mode, SGIIOC4State),
        VMSTATE_UINT32(ata_readback, SGIIOC4State),
        VMSTATE_PCI_DEVICE(parent_obj, SGIIOC4State),
        VMSTATE_STRUCT_ARRAY(uart, SGIIOC4State, SGI_IOC4_PORTS, 0,
                             vmstate_serial, SerialState),
        VMSTATE_UINT32(serial_pending, SGIIOC4State),
        VMSTATE_UINT32(serial_enable, SGIIOC4State),
        VMSTATE_UINT32(other_enable, SGIIOC4State),
        VMSTATE_UINT32(uart_level, SGIIOC4State),
        VMSTATE_UINT32(clock_hz, SGIIOC4State),
        VMSTATE_UINT32(other_pending, SGIIOC4State),
        VMSTATE_UINT32(dma_error_low, SGIIOC4State),
        VMSTATE_UINT32(dma_error_high, SGIIOC4State),
        VMSTATE_SGI_IOC_SDMA(sdma, SGIIOC4State),
        VMSTATE_END_OF_LIST()
    }
};

static void sgi_ioc4_exit(PCIDevice *dev)
{
    SGIIOC4State *s = (SGIIOC4State *)dev;
    int i;

    if (s->kbc_realized) {
        memory_region_del_subregion(&s->bar, &s->kbc_be);
        qdev_unrealize(DEVICE(&s->kbc));
        s->kbc_realized = false;
    }

    sgi_ioc_sdma_finalize(&s->sdma);
    for (i = 0; i < SGI_IOC4_PORTS; i++) {
        memory_region_del_subregion(&s->bar, &s->uart[i].io);
        qdev_unrealize(DEVICE(&s->uart[i]));
    }
    if (s->clock_alias_mapped) {
        memory_region_del_subregion(&s->bar, &s->clock_alias);
        s->clock_alias_mapped = false;
    }
}

static void sgi_ioc4_realize(PCIDevice *dev, Error **errp)
{
    SGIIOC4State *s = (SGIIOC4State *)dev;
    uint64_t clock_offset = SGI_IOC4_CLOCK_NATIVE;
    uint64_t clock_size = SGI_IOC4_CLOCK_SIZE;
    uint64_t clock_bar = SGI_IOC4_CLOCK_BAR;
    int i;

    if (s->clock_hz != 33333333 && s->clock_hz != 66666667) {
        error_setg(errp, "sgi-ioc4 clock-hz must be 33333333 or 66666667");
        return;
    }
    if (s->clock) {
        uint64_t region_size = memory_region_size(s->clock);

        if (region_size == SGI_IOC4_NVRAM_SIZE) {
            clock_offset = 0;
            clock_size = SGI_IOC4_NVRAM_SIZE;
            clock_bar = SGI_IOC4_NVRAM_BAR;
        } else if (region_size != SGI_IOC4_LEGACY_NVRAM_SIZE) {
            error_setg(errp,
                       "sgi-ioc4 clock region must be 0x800 or 0x8000 bytes "
                       "(got 0x%" PRIx64 ")",
                       region_size);
            return;
        }
    }

    s->dma_migration_mode = s->serial_dma_experimental;
    s->pci_status_compat_mode = s->pci_status_compat;
    s->ata_readback_compat_mode = s->ata_readback_compat;
    /* Local PCI IRQ output needs a pin; IO9 board wiring is not modeled. */
    dev->config[PCI_INTERRUPT_PIN] = 1;

    memory_region_init_io(&s->bar, OBJECT(s), &sgi_ioc4_ops, s, "sgi-ioc4",
                          SGI_IOC4_BAR_SIZE);
    if (s->clock && !s->clock_alias_initialized) {
        memory_region_init_alias(&s->clock_alias, OBJECT(s),
                                 "sgi-ioc4-clock", s->clock,
                                 clock_offset, clock_size);
        s->clock_alias_initialized = true;
    }
    if (s->clock) {
        memory_region_add_subregion(&s->bar, clock_bar, &s->clock_alias);
        s->clock_alias_mapped = true;
    }
    for (i = 0; i < SGI_IOC4_PORTS; i++) {
        SerialState *uart = &s->uart[i];

        qdev_prop_set_uint32(DEVICE(uart), "baudbase", s->clock_hz / 16);
        if (!qdev_realize(DEVICE(uart), NULL, errp)) {
            while (i--) {
                memory_region_del_subregion(&s->bar, &s->uart[i].io);
                qdev_unrealize(DEVICE(&s->uart[i]));
            }
            if (s->clock_alias_mapped) {
                memory_region_del_subregion(&s->bar, &s->clock_alias);
                s->clock_alias_mapped = false;
            }
            return;
        }
        uart->irq = &s->uart_irq[i];
        qdev_connect_gpio_out_named(
            DEVICE(uart), SERIAL_GPIO_TX_READY, 0,
            qdev_get_gpio_in_named(DEVICE(dev), "uart-tx-ready", i));
        memory_region_init_io(&uart->io, OBJECT(s), &sgi_ioc4_uart_ops,
                              &s->dma[i],
                              "sgi-ioc4-uart", 8);
        memory_region_add_subregion(&s->bar, SGI_IOC4_UART_BASE + 8 * i,
                                    &uart->io);
    }

    /*
     * Every IP53 product's IO9 brings the keyboard and mouse ports out on
     * a daughtercard on its base module (inferred for Onyx 4), so a
     * keyboard and mouse are always attached, as on an IOC3 board that
     * wires its ports. A real system's connectors may be empty.
     */
    s->kbc.ports_present = true;
    if (!qdev_realize(DEVICE(&s->kbc), BUS(&s->kbc_bus), errp)) {
        for (i = 0; i < SGI_IOC4_PORTS; i++) {
            memory_region_del_subregion(&s->bar, &s->uart[i].io);
            qdev_unrealize(DEVICE(&s->uart[i]));
        }
        if (s->clock_alias_mapped) {
            memory_region_del_subregion(&s->bar, &s->clock_alias);
            s->clock_alias_mapped = false;
        }
        return;
    }
    s->kbc_realized = true;
    s->kbc_mmio = sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->kbc), 0);
    memory_region_init_io(&s->kbc_be, OBJECT(s), &sgi_ioc4_kbc_be_ops, s,
                          "sgi-ioc4-kbc", SGI_IOC4_KBC_SIZE);
    memory_region_add_subregion(&s->bar, SGI_IOC4_KBC_BASE, &s->kbc_be);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->kbc), 0,
                       qdev_get_gpio_in_named(DEVICE(dev), "kbc-irq", 0));

    sgi_ioc_sdma_init(&s->sdma, SGI_IOC4_PORTS, &sgi_ioc4_sdma_params,
                      &sgi_ioc4_sdma_ops, s, pci_get_address_space(dev));
    for (i = 0; i < SGI_IOC4_PORTS; i++) {
        sgi_ioc_sdma_attach_uart(&s->sdma, i, &s->uart[i]);
    }
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);
    if (s->pci_status_compat) {
        pci_set_word(dev->config + PCI_STATUS, SGI_IOC4_PCI_STATUS_COMPAT);
    }
}

static const Property sgi_ioc4_properties[] = {
    DEFINE_PROP_UINT32("clock-hz", SGIIOC4State, clock_hz, 33333333),
    DEFINE_PROP_BOOL("serial-dma-experimental", SGIIOC4State,
                     serial_dma_experimental, false),
    DEFINE_PROP_BOOL("pci-status-compat", SGIIOC4State, pci_status_compat,
                     false),
    DEFINE_PROP_BOOL("ata-readback-compat", SGIIOC4State,
                     ata_readback_compat, false),
    DEFINE_PROP_LINK("clock", SGIIOC4State, clock, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_CHR("chardev1", SGIIOC4State, uart[0].chr),
    DEFINE_PROP_CHR("chardev2", SGIIOC4State, uart[1].chr),
    DEFINE_PROP_CHR("chardev3", SGIIOC4State, uart[2].chr),
    DEFINE_PROP_CHR("chardev4", SGIIOC4State, uart[3].chr),
};

static void sgi_ioc4_instance_init(Object *obj)
{
    SGIIOC4State *s = (SGIIOC4State *)obj;
    int i;

    qbus_init(&s->kbc_bus, sizeof(s->kbc_bus), TYPE_SGI_IOC4_BUS,
              DEVICE(obj), "kbc-bus");
    object_initialize_child(obj, "kbc", &s->kbc, TYPE_SGI_IOC3_KBC);
    qdev_init_gpio_in_named(DEVICE(obj), sgi_ioc4_kbc_irq, "kbc-irq", 1);

    s->clock_hz = 33333333;
    qdev_init_gpio_in_named(DEVICE(obj), sgi_ioc4_uart_tx_ready,
                            "uart-tx-ready", SGI_IOC4_PORTS);
    for (i = 0; i < SGI_IOC4_PORTS; i++) {
        qemu_init_irq_child(obj, "uart-irq[*]", &s->uart_irq[i],
                            sgi_ioc4_uart_irq, s, i);
        object_initialize_child(obj, "uart[*]", &s->uart[i], TYPE_SERIAL);
        s->dma[i].ioc4 = s;
        s->dma[i].index = i;
    }
}

static bool sgi_ioc4_byte_lane_self_ordered(Object *obj, Error **errp)
{
    return true;
}

static void sgi_ioc4_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    SGIIOC4Class *ic = (SGIIOC4Class *)klass;

    object_class_property_add_bool(klass, SGI_PIC_BYTE_LANE_SELF_ORDERED,
                                   sgi_ioc4_byte_lane_self_ordered, NULL);
    pc->realize = sgi_ioc4_realize;
    pc->exit = sgi_ioc4_exit;
    pc->vendor_id = 0x10a9;
    pc->device_id = 0x100a;
    pc->revision = 0x53;
    pc->class_id = PCI_CLASS_PROCESSOR_CO;
    dc->vmsd = &vmstate_ioc4;
    device_class_set_props(dc, sgi_ioc4_properties);
    resettable_class_set_parent_phases(rc, NULL, sgi_ioc4_reset_hold, NULL,
                                       &ic->parent_phases);
}

static const TypeInfo sgi_ioc4_types[] = {
    {
        .name = TYPE_SGI_IOC4,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(SGIIOC4State),
        .class_size = sizeof(SGIIOC4Class),
        .instance_init = sgi_ioc4_instance_init,
        .class_init = sgi_ioc4_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
    {
        .name = TYPE_SGI_IOC4_BUS,
        .parent = TYPE_SYSTEM_BUS,
        .instance_size = sizeof(SGIIOC4BusState),
    },
};

static void sgi_ioc4_register_types(void)
{
    type_register_static_array(sgi_ioc4_types, ARRAY_SIZE(sgi_ioc4_types));
}
type_init(sgi_ioc4_register_types)
