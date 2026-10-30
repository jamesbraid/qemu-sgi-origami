/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* SGI MACE 2.0 platform registers. */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/rcu.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/mips/sgi/mace.h"
#include "hw/mips/sgi/crime.h"
#include "hw/mips/sgi/mace-id.h"
#include "hw/mips/sgi/mace-ps2.h"
#include "hw/net/sgi-mace-eth.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_host.h"
#include "hw/mips/sgi/mace-timer.h"
#include "mace-serial-dma.h"
#include "mace-audio.h"
#include "mace-i2c.h"
#include "hw/rtc/ds1685.h"
#include "hw/char/serial.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "system/qtest.h"
#include "system/runstate.h"

#define MACE_APERTURE_SIZE 0x400000
#define MACE_RING_BASE     0x310000
#define MACE_ID_BASE       0x310008
#define MACE_ETH_BASE      0x280000
#define MACE_PCI_BASE      0x080000
#define MACE_PCI_SIZE      0x080000
#define MACE_PCI_ERROR_ADDR  0x0000
#define MACE_PCI_ERROR_FLAGS 0x0004
#define MACE_PCI_CONTROL     0x0008
#define MACE_PCI_REVISION    0x000c
#define MACE_PCI_CFGADDR     0x0cf8
#define MACE_PCI_CFGDATA     0x0cfc
#define MACE_PCI_MASTER_ABORT (1U << 31)
#define MACE_PCI_ADDR_VALID   (1U << 19)
#define MACE_PCI_SUPPORTED_CONTROL (0xff0085ffU)
#define MACE_PCI_NO_PIN 8
#define MACE_UART_A        0x390000
#define MACE_UART_B        0x398000
#define MACE_ISTAT         0x310010
#define MACE_IMASK         0x310018
#define MACE_UST           0x340000
#define MACE_COMPARE0      0x340008
#define MACE_TIMER_NS      960
#define MACE_TIMER_BIT(i)  (1U << (13 + (i)))
#define MACE_RING_WRITABLE 0xffff8001U
#define MACE_IRQ_MASK UINT16_MAX

typedef struct MACEUARTPort {
    SGIMACEState *mace;
    MemoryRegion mmio;
    unsigned index;
} MACEUARTPort;

struct SGIMACEState {
    PCIHostState parent_obj;
    MemoryRegion aperture;
    MemoryRegion regs;
    SGIMACEEthState eth;
    DeviceState *identity;
    DeviceState *ps2;
    MemoryRegion eth_dma;
    MemoryRegion eth_low_ram;
    MemoryRegion eth_full_ram;
    MemoryRegion eth_no_ecc_ram;
    MemoryRegion pci_regs;
    MemoryRegion pci_memory;
    MemoryRegion pci_io;
    MemoryRegion pci_mem_block;
    MemoryRegion pci_io_block;
    MemoryRegion pci_lower_ram;
    MemoryRegion pci_native_ram;
    MemoryRegion cpu_pci_view;
    MemoryRegion cpu_pci_alias;
    MemoryRegion cpu_pci_high_view;
    MemoryRegion cpu_pci_pio_low;
    MemoryRegion cpu_pci_pio_high;
    MACEI2C i2c;
    bool i2c_untimed_test_profile;
    MACEAudio audio;
    MemoryRegion audio_regs;
    qemu_irq audio_ttl[2];
    MACESerialDMA serial_dma;
    AddressSpace serial_dma_as;
    MemoryRegion peripheral_dma;
    MemoryRegion peripheral_native_ram;
    MemoryRegion peripheral_no_ecc_ram;
    AddressSpace pci_dma_as;
    AddressSpace pci_ram_as;
    MemoryRegion *ram;
    DS1685State rtc;
    SerialState uart[2];
    MACEUARTPort uart_port[2];
    MACEUARTPort serial_dma_port[2];
    DeviceState *crime_device;
    qemu_irq crime[16];
    uint16_t crime_normal;
    /* Child post-load callbacks can precede these parent IRQ baselines. */
    bool crime_irq_loading;
    bool crime_irq_reconciled;
    bool rtc_irq_deferred;
    bool rtc_irq_deferred_level;
    qemu_irq isa_reset;
    QEMUTimer *timer;
    int64_t epoch_ns;
    int64_t saved_elapsed_ns;
    uint32_t ring_base;
    uint32_t istat;
    uint32_t imask;
    uint8_t uart_irq_level;
    uint32_t compare[3];
    uint64_t compare_due[3];
    bool compare_programmed[3];
    uint32_t pci_control;
    uint32_t pci_config_addr;
    uint32_t pci_error_addr;
    uint32_t pci_error_flags;
    uint8_t pci_wire_level;
    uint8_t pci_bus_level;
    bool aic_pci_diagnostic;
    bool o2_storage_diagnostic;
    bool pio_byte_test;
    bool identity_diagnostic;
    bool misc_word_pio;
    bool password_clear_input;
    bool eth_low_dma_compat;
    bool pci_native_word_swap;
    char *identity_test_serial;
    char *identity_eprom_file;
    char *identity_status;
    bool storage_diagnostic_authorized;
    bool storage_diagnostic_realized;
};

void sgi_mace_enable_storage_diagnostic(DeviceState *dev, Error **errp)
{
    SGIMACEState *s = SGI_MACE(dev);

    if (qdev_is_realized(dev) || s->storage_diagnostic_realized) {
        error_setg(errp, "sgi-mace storage diagnostic must be selected "
                         "before realize");
        return;
    }
    s->storage_diagnostic_authorized = true;
    s->o2_storage_diagnostic = true;
}

static uint64_t mace_elapsed_ticks(SGIMACEState *s)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->epoch_ns;

    return elapsed > 0 ? elapsed / MACE_TIMER_NS : 0;
}

static void mace_publish_normal(SGIMACEState *s, uint16_t mask,
                                uint16_t value, bool force)
{
    uint16_t changed;

    if (s->crime_irq_loading ||
        (runstate_check(RUN_STATE_INMIGRATE) && !s->crime_irq_reconciled)) {
        return;
    }
    changed = force ? mask : (s->crime_normal ^ value) & mask;

    s->crime_normal = (s->crime_normal & ~mask) | (value & mask);
    if (!changed) {
        return;
    }
    if (s->crime_device) {
        sgi_crime_update_mace_inputs(s->crime_device, changed, value);
    }
    for (unsigned i = 0; i < 16; i++) {
        if (changed & (1U << i)) {
            qemu_set_irq(s->crime[i], (value >> i) & 1);
        }
    }
}

static void mace_update_irqs(SGIMACEState *s)
{
    uint32_t active = s->istat & s->imask;
    uint16_t levels = ((active & 0xffff0000U) != 0 ? 1U << 4 : 0) |
                      ((active & 0x0000eb00U) != 0 ? 1U << 5 : 0) |
                      ((active & 0x000000ffU) != 0 ? 1U << 6 : 0);

    mace_publish_normal(s, (1U << 4) | (1U << 5) | (1U << 6), levels,
                        false);
}

static uint32_t mace_audio_ust(void *opaque, int64_t event_ns)
{
    SGIMACEState *s = opaque;

    return MAX(event_ns - s->epoch_ns, 0) / MACE_TIMER_NS;
}

static void mace_audio_irq(void *opaque, uint32_t status)
{
    SGIMACEState *s = opaque;

    s->istat = (s->istat & ~255U) | status;
    mace_update_irqs(s);
}

static void mace_audio_ttl(void *opaque, unsigned outputs)
{
    SGIMACEState *s = opaque;

    for (unsigned i = 0; i < 2; i++) {
        qemu_set_irq(s->audio_ttl[i], (outputs >> i) & 1);
    }
}

static void mace_audio_button(void *opaque, int n, int level)
{
    mace_audio_volume(&SGI_MACE(opaque)->audio, n, level);
}

static void mace_audio_power(void *opaque, int n, int level)
{
    mace_audio_pwrdwn(&SGI_MACE(opaque)->audio, level);
}

static void mace_audio_feedthrough(void *opaque, int n, int level)
{
    mace_audio_pdmnft(&SGI_MACE(opaque)->audio, level);
}

static bool mace_audio_timer_access(hwaddr offset, unsigned size)
{
    return offset >= 0x340020 && offset < 0x340038 &&
           (size == 4 || size == 8) && !(offset & (size - 1));
}

static void mace_eth_irq(void *opaque, int n, int level)
{
    SGIMACEState *s = opaque;

    mace_publish_normal(s, 1U << 3, level ? 1U << 3 : 0, false);
}

static void mace_eth_request(void *opaque, uint32_t request)
{
    SGIMACEState *s = opaque;
    uint16_t mask = request >> 16;

    if (s->crime_device) {
        sgi_crime_update_mace_inputs(s->crime_device, mask, request);
    } else {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mace: Ethernet interrupt request has no CRIME "
                      "controller\n");
    }
}

static void mace_eth_reset_irq(void *opaque)
{
    SGIMACEState *s = opaque;

    mace_publish_normal(s, 1U << 3, 0, true);
}

static void mace_ps2_irq(void *opaque, int n, int level)
{
    SGIMACEState *s = opaque;
    uint32_t bit = 1U << (9 + n);

    s->istat = (s->istat & ~bit) | (level ? bit : 0);
    mace_update_irqs(s);
}

static void mace_rtc_apply_irq(SGIMACEState *s, int level)
{
    s->istat = (s->istat & ~(1U << 8)) | (level ? (1U << 8) : 0);
    mace_update_irqs(s);
}

static void mace_rtc_irq(void *opaque, int n, int level)
{
    SGIMACEState *s = opaque;

    if (s->crime_irq_loading ||
        (runstate_check(RUN_STATE_INMIGRATE) && !s->crime_irq_reconciled)) {
        s->rtc_irq_deferred = true;
        s->rtc_irq_deferred_level = level;
        return;
    }
    mace_rtc_apply_irq(s, level);
}

void sgi_mace_enable_rx_threshold_retirement(DeviceState *dev, Error **errp)
{
    if (qdev_is_realized(dev)) {
        error_setg(errp, "sgi-mace RX threshold policy must be selected "
                         "before realize");
        return;
    }
    SGI_MACE(dev)->eth.rx_threshold_retirement = 1;
}

void sgi_mace_enable_serial_ring_offsets(DeviceState *dev, Error **errp)
{
    if (qdev_is_realized(dev)) {
        error_setg(errp, "sgi-mace serial ring policy must be selected "
                         "before realize");
        return;
    }
    SGI_MACE(dev)->serial_dma.ring_offsets = 1;
}

static void mace_serial_update_irq(void *opaque, uint32_t status)
{
    SGIMACEState *s = opaque;

    s->istat = (s->istat & ~0xfbe00000U) | status;
    for (unsigned p = 0; p < 2; p++) {
        uint32_t bit = 1U << (20 + 6 * p);
        bool rx = (s->serial_dma.channel[2 * p + 1].control & 0x600) == 0x200;
        bool tx = (s->serial_dma.channel[2 * p].control & 0x600) == 0x200;
        bool level = s->uart_irq_level & (1U << p);

        if (rx || tx) {
            level = serial_pio_irq_pending(&s->uart[p], rx, tx);
        }
        s->istat = (s->istat & ~bit) | (level ? bit : 0);
    }
    mace_update_irqs(s);
}

static void mace_uart_irq(void *opaque, int n, int level)
{
    SGIMACEState *s = opaque;
    uint32_t bit = 1U << (20 + 6 * n);

    if (s->serial_dma.loading) {
        return;
    }
    s->uart_irq_level = (s->uart_irq_level & ~(1U << n)) |
                        (level ? (1U << n) : 0);
    if (s->serial_dma.bh) {
        mace_serial_update_irq(s, s->istat & 0xfbe00000U);
    } else {
        s->istat = (s->istat & ~bit) | (level ? bit : 0);
        mace_update_irqs(s);
    }
}

static void mace_update_pci_irqs(SGIMACEState *s)
{
    uint16_t levels = (s->pci_error_flags & s->pci_control &
                       MACE_PCI_MASTER_ABORT) ? 1U << 7 : 0;

    for (unsigned i = 0; i < 8; i++) {
        if (((s->pci_wire_level | s->pci_bus_level) & (1U << i)) &&
            (s->pci_control & (1U << i))) {
            levels |= 1U << (8 + i);
        }
    }
    mace_publish_normal(s, (uint16_t)(1U << 7 | 0xff00), levels, false);
}

static void mace_pci_wire_irq(void *opaque, int n, int level)
{
    SGIMACEState *s = opaque;

    s->pci_wire_level = (s->pci_wire_level & ~(1U << n)) |
                        ((level != 0) << n);
    mace_update_pci_irqs(s);
}

static void mace_pci_set_irq(void *opaque, int n, int level)
{
    SGIMACEState *s = opaque;

    if (n == MACE_PCI_NO_PIN) {
        if (level) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-mace: PCI device asserted an unwired pin\n");
        }
        return;
    }
    s->pci_bus_level = (s->pci_bus_level & ~(1U << n)) |
                       ((level != 0) << n);
    mace_update_pci_irqs(s);
}

static int mace_pci_map_irq(PCIDevice *dev, int pin)
{
    unsigned slot = PCI_SLOT(dev->devfn);

    if (pin == 0 && slot >= 1 && slot <= 3) {
        return slot - 1;
    }
    return MACE_PCI_NO_PIN;
}

static void mace_pci_master_abort(SGIMACEState *s, uint32_t addr)
{
    if (!(s->pci_error_flags & MACE_PCI_ADDR_VALID)) {
        s->pci_error_addr = addr;
    }
    s->pci_error_flags |= MACE_PCI_MASTER_ABORT | MACE_PCI_ADDR_VALID;
    mace_update_pci_irqs(s);
}

static bool mace_pci_config_claimed(SGIMACEState *s, uint32_t addr)
{
    PCIBus *bus = PCI_HOST_BRIDGE(s)->bus;

    return pci_find_device(bus, addr >> 16, (addr >> 8) & 0xff) != NULL;
}

static bool mace_pci_onboard_aic(SGIMACEState *s, uint32_t addr)
{
    unsigned devfn = (addr >> 8) & 0xff;
    PCIDevice *dev = pci_find_device(PCI_HOST_BRIDGE(s)->bus,
                                     addr >> 16, devfn);

    return !(addr >> 16) &&
           (devfn == PCI_DEVFN(1, 0) || devfn == PCI_DEVFN(2, 0)) &&
           dev && object_dynamic_cast(OBJECT(dev), "aic7880");
}

static bool mace_pci_unused_aic_region(SGIMACEState *s, uint32_t addr)
{
    PCIDevice *dev;
    unsigned reg = addr & 0xfc;

    if (!mace_pci_onboard_aic(s, addr)) {
        return false;
    }
    dev = pci_find_device(PCI_HOST_BRIDGE(s)->bus,
                          addr >> 16, (addr >> 8) & 0xff);
    if (reg >= PCI_BASE_ADDRESS_2 && reg <= PCI_BASE_ADDRESS_5) {
        unsigned bar = (reg - PCI_BASE_ADDRESS_0) / 4;
        PCIIORegion *previous = &dev->io_regions[bar - 1];

        /* An unregistered slot can hold the upper half of a 64-bit BAR. */
        return !dev->io_regions[bar].size &&
               !(previous->size &&
                 (previous->type & PCI_BASE_ADDRESS_MEM_TYPE_64));
    }
    return reg == PCI_ROM_ADDRESS && !dev->has_rom &&
           (!dev->romfile || !dev->romfile[0]) &&
           !dev->io_regions[PCI_ROM_SLOT].size;
}

static bool mace_pci_byte_test_card(SGIMACEState *s, uint32_t addr)
{
    unsigned devfn = (addr >> 8) & 0xff;
    PCIDevice *dev = pci_find_device(PCI_HOST_BRIDGE(s)->bus,
                                     addr >> 16, devfn);

    return s->pio_byte_test && qtest_enabled() && !(addr >> 16) &&
           devfn == PCI_DEVFN(3, 0) && dev &&
           object_dynamic_cast(OBJECT(dev), "sgi-mace-pio-test");
}

static uint32_t mace_pci_config_address(SGIMACEState *s, hwaddr offset,
                                        unsigned size)
{
    unsigned lane = offset - MACE_PCI_CFGDATA;

    return (s->pci_config_addr & ~3U) | (lane ^ (4 - size));
}

static bool mace_pci_listed(hwaddr offset)
{
    switch (offset & ~3ULL) {
    case MACE_PCI_ERROR_ADDR:
    case MACE_PCI_ERROR_FLAGS:
    case MACE_PCI_CONTROL:
    case MACE_PCI_REVISION:
    case MACE_PCI_CFGADDR:
    case MACE_PCI_CFGDATA:
        return true;
    default:
        return false;
    }
}

static MemTxResult mace_pci_read(void *opaque, hwaddr offset, uint64_t *value,
                                 unsigned size, MemTxAttrs attrs)
{
    SGIMACEState *s = opaque;
    uint32_t addr;

    if (offset >= MACE_PCI_CFGDATA && offset < MACE_PCI_CFGDATA + 4 &&
        size <= 4 && !(offset & (size - 1)) &&
        offset + size <= MACE_PCI_CFGDATA + 4) {
        addr = mace_pci_config_address(s, offset, size);
        if (!mace_pci_config_claimed(s, addr)) {
            mace_pci_master_abort(s, addr);
            *value = MAKE_64BIT_MASK(0, size * 8);
            return MEMTX_OK;
        }
        *value = pci_data_read(PCI_HOST_BRIDGE(s)->bus, addr, size);
        return MEMTX_OK;
    }
    if (!mace_pci_listed(offset)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mace: unlisted PCI host read at 0x%" HWADDR_PRIx
                      "\n", offset);
        *value = MAKE_64BIT_MASK(0, size * 8);
        return MEMTX_OK;
    }
    if (size != 4 || (offset & 3)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace: illegal PCI host read width/address\n");
        *value = 0;
        return MEMTX_ERROR;
    }
    switch (offset) {
    case MACE_PCI_ERROR_ADDR:
        *value = s->pci_error_addr;
        return MEMTX_OK;
    case MACE_PCI_ERROR_FLAGS:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mace: PCI fixed error capability bits unresolved\n");
        *value = s->pci_error_flags;
        return MEMTX_OK;
    case MACE_PCI_CONTROL:
        *value = s->pci_control;
        return MEMTX_OK;
    case MACE_PCI_REVISION:
        *value = 1;
        return MEMTX_OK;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mace: unlisted PCI host read at 0x%" HWADDR_PRIx
                      "\n", offset);
        *value = 0xffffffff;
        return MEMTX_OK;
    }
}

static MemTxResult mace_pci_write(void *opaque, hwaddr offset, uint64_t value,
                                  unsigned size, MemTxAttrs attrs)
{
    SGIMACEState *s = opaque;
    uint32_t addr, unsupported;

    if (offset >= MACE_PCI_CFGDATA && offset < MACE_PCI_CFGDATA + 4 &&
        size <= 4 && !(offset & (size - 1)) &&
        offset + size <= MACE_PCI_CFGDATA + 4) {
        addr = mace_pci_config_address(s, offset, size);
        if (!mace_pci_config_claimed(s, addr)) {
            mace_pci_master_abort(s, addr);
            return MEMTX_OK;
        }
        if ((addr & 0xfc) == 0x04) {
            unsigned shift = (addr & 3) * 8;
            uint64_t mask = MAKE_64BIT_MASK(0, size * 8) << shift;
            uint32_t old = pci_data_read(PCI_HOST_BRIDGE(s)->bus,
                                         addr & ~3U, 4);
            uint32_t next = (old & ~mask) | (value << shift);

            if (mace_pci_onboard_aic(s, addr) && (next & 0xffb8)) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace: unsupported AIC PCI COMMAND bits\n");
                return MEMTX_ERROR;
            }
            if ((next & 7) && !mace_pci_onboard_aic(s, addr) &&
                !(mace_pci_byte_test_card(s, addr) && (next & 7) == 2)) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace: PCI BAR and bus-master enables "
                              "require a reviewed address view\n");
                return MEMTX_ERROR;
            }
        }
        if (!mace_pci_unused_aic_region(s, addr) &&
            (((addr & 0xfc) >= 0x10 && (addr & 0xfc) < 0x28 &&
             !((mace_pci_onboard_aic(s, addr) &&
                ((addr & 0xfc) == 0x10 || (addr & 0xfc) == 0x14)) ||
               (mace_pci_byte_test_card(s, addr) &&
                (addr & 0xfc) == 0x10))) ||
            (addr & 0xfc) == PCI_ROM_ADDRESS ||
            ((addr & 0xfc) == PCI_ROM_ADDRESS1 &&
             (pci_data_read(PCI_HOST_BRIDGE(s)->bus,
                            (addr & ~0xffU) | PCI_HEADER_TYPE, 1) &
              ~PCI_HEADER_TYPE_MULTI_FUNCTION) == PCI_HEADER_TYPE_BRIDGE))) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-mace: PCI BAR programming is unsupported\n");
            return MEMTX_ERROR;
        }
        pci_data_write(PCI_HOST_BRIDGE(s)->bus, addr, value, size);
        return MEMTX_OK;
    }
    if (!mace_pci_listed(offset)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mace: unlisted PCI host write at 0x%" HWADDR_PRIx
                      "\n", offset);
        return MEMTX_OK;
    }
    if (size != 4 || (offset & 3)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace: illegal PCI host write width/address\n");
        return MEMTX_ERROR;
    }
    switch (offset) {
    case MACE_PCI_ERROR_FLAGS:
        if (!(value & MACE_PCI_MASTER_ABORT)) {
            s->pci_error_flags &= ~(MACE_PCI_MASTER_ABORT |
                                    MACE_PCI_ADDR_VALID);
        }
        mace_update_pci_irqs(s);
        return MEMTX_OK;
    case MACE_PCI_CONTROL:
        unsupported = value & ~MACE_PCI_SUPPORTED_CONTROL;
        if (unsupported) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-mace: unsupported PCI control bits 0x%" PRIx32
                          "\n", unsupported);
            return MEMTX_ERROR;
        }
        s->pci_control = value;
        mace_update_pci_irqs(s);
        return MEMTX_OK;
    case MACE_PCI_REVISION:
        /* The write side invalidates prefetch buffers, which are absent. */
        return MEMTX_OK;
    case MACE_PCI_CFGADDR:
        s->pci_config_addr = value & 0x00fffffcU;
        return MEMTX_OK;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mace: unlisted PCI host write at 0x%" HWADDR_PRIx
                      "\n", offset);
        return MEMTX_OK;
    }
}

static const MemoryRegionOps mace_pci_ops = {
    .read_with_attrs = mace_pci_read,
    .write_with_attrs = mace_pci_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.unaligned = true,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    /* Keep invalid accesses intact for the register checks. */
    .impl.unaligned = true,
};

static MemTxResult mace_pci_dma_block_read(void *opaque, hwaddr offset,
                                           uint64_t *value, unsigned size,
                                           MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP,
                  "sgi-mace: PCI bus-master host-memory read is unsupported\n");
    *value = 0;
    return MEMTX_ERROR;
}

static MemTxResult mace_pci_dma_block_write(void *opaque, hwaddr offset,
                                            uint64_t value, unsigned size,
                                            MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP,
                  "sgi-mace: PCI bus-master host-memory write is unsupported\n");
    return MEMTX_ERROR;
}

static const MemoryRegionOps mace_pci_dma_block_ops = {
    .read_with_attrs = mace_pci_dma_block_read,
    .write_with_attrs = mace_pci_dma_block_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static MemTxResult mace_pci_native_read(void *opaque, hwaddr offset,
                                        uint64_t *value, unsigned size,
                                        MemTxAttrs attrs)
{
    SGIMACEState *s = opaque;
    uint8_t bytes[8] = { 0 };
    MemTxResult result;

    result = address_space_read(&s->pci_ram_as, offset ^ (4 - size), attrs,
                                bytes, size);
    *value = ldn_be_p(bytes, size);
    return result;
}

static MemTxResult mace_pci_native_write(void *opaque, hwaddr offset,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    SGIMACEState *s = opaque;
    uint8_t bytes[8];

    stn_be_p(bytes, size, value);
    return address_space_write(&s->pci_ram_as, offset ^ (4 - size), attrs,
                               bytes, size);
}

/* The selected O2 approximation reverses bytes within each aligned word. */
static const MemoryRegionOps mace_pci_native_ops = {
    .read_with_attrs = mace_pci_native_read,
    .write_with_attrs = mace_pci_native_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    /* Each callback stays within one naturally aligned word. */
    .impl.max_access_size = 4,
};

static bool mace_cpu_pci_access_valid(SGIMACEState *s, hwaddr addr,
                                      unsigned size, bool is_write,
                                      MemTxAttrs attrs)
{
    hwaddr offset, length = size;
    MemoryRegion *target;

    if (size != 1 && size != 4) {
        return false;
    }
    /* Reject AIC words before QEMU can split them into byte accesses. */
    target = address_space_translate(&s->pci_dma_as, addr, &offset,
                                     &length, is_write, attrs);
    return length == size &&
           memory_region_access_valid(target, offset, size, is_write, attrs);
}

static MemTxResult mace_cpu_pci_read(void *opaque, hwaddr offset,
                                     uint64_t *value, unsigned size,
                                     MemTxAttrs attrs)
{
    SGIMACEState *s = opaque;
    uint8_t bytes[4] = { 0 };
    hwaddr addr = 0x80000000ULL + (size == 1 ? offset ^ 3 : offset);
    MemTxResult result;

    RCU_READ_LOCK_GUARD();
    if (!mace_cpu_pci_access_valid(s, addr, size, false, attrs)) {
        *value = 0;
        return MEMTX_ERROR;
    }
    result = address_space_read(&s->pci_dma_as, addr, attrs, bytes, size);
    *value = size == 1 ? bytes[0] : ldl_be_p(bytes);
    return result;
}

static MemTxResult mace_cpu_pci_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size,
                                      MemTxAttrs attrs)
{
    SGIMACEState *s = opaque;
    uint8_t bytes[4];
    hwaddr addr = 0x80000000ULL + (size == 1 ? offset ^ 3 : offset);

    RCU_READ_LOCK_GUARD();
    if (!mace_cpu_pci_access_valid(s, addr, size, true, attrs)) {
        return MEMTX_ERROR;
    }
    if (size == 1) {
        bytes[0] = value;
    } else {
        stl_be_p(bytes, value);
    }
    return address_space_write(&s->pci_dma_as, addr, attrs, bytes, size);
}

static const MemoryRegionOps mace_cpu_pci_ops = {
    .read_with_attrs = mace_cpu_pci_read,
    .write_with_attrs = mace_cpu_pci_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static AddressSpace *mace_pci_dma_address_space(PCIBus *bus, void *opaque,
                                                int devfn)
{
    SGIMACEState *s = opaque;

    return &s->pci_dma_as;
}

static const PCIIOMMUOps mace_pci_dma_ops = {
    .get_address_space = mace_pci_dma_address_space,
};

static void mace_schedule_compare(SGIMACEState *s)
{
    int64_t next = INT64_MAX;
    unsigned i;

    timer_del(s->timer);
    for (i = 0; i < ARRAY_SIZE(s->compare); i++) {
        int64_t deadline;

        if (!s->compare_programmed[i] || (s->istat & MACE_TIMER_BIT(i)) ||
            s->compare_due[i] == UINT64_MAX) {
            continue;
        }
        deadline = s->epoch_ns + s->compare_due[i] * MACE_TIMER_NS;
        next = MIN(next, deadline);
    }
    if (next != INT64_MAX) {
        timer_mod(s->timer, next);
    }
}

static void mace_compare_fire(void *opaque)
{
    SGIMACEState *s = opaque;
    uint64_t current = mace_elapsed_ticks(s);
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(s->compare); i++) {
        if (s->compare_programmed[i] && !(s->istat & MACE_TIMER_BIT(i)) &&
            sgi_mace_compare_reached(s->compare_due[i], current)) {
            s->istat |= MACE_TIMER_BIT(i);
        }
    }
    mace_update_irqs(s);
    mace_schedule_compare(s);
}

static int mace_compare_index(hwaddr offset)
{
    if (offset >= MACE_COMPARE0 && offset <= MACE_COMPARE0 + 16 &&
        !(offset & 7)) {
        return (offset - MACE_COMPARE0) / 8;
    }
    if (offset >= 0x350000 && offset < 0x380000 && !(offset & 7)) {
        return (offset - 0x350000) / 0x10000;
    }
    return -1;
}

static uint64_t mace_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIMACEState *s = opaque;
    int index = mace_compare_index(offset);

    if (mace_audio_timer_access(offset, size)) {
        return mace_audio_timestamp_read(&s->audio, (offset - 0x340020) / 8,
                                          (offset & 4) != 0, size);
    }
    if (size == 8) {
        if (index >= 0) {
            if (!s->compare_programmed[index]) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace: compare reset value is unspecified\n");
            }
            return s->compare[index];
        }
        switch (offset) {
        case MACE_RING_BASE:
            return s->ring_base;
        case MACE_ISTAT:
            return s->istat;
        case MACE_IMASK:
            return s->imask;
        case MACE_UST:
            return (uint32_t)mace_elapsed_ticks(s);
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mace: read offset 0x%" HWADDR_PRIx
                  " size %u\n", offset, size);
    return 0;
}

static void mace_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    SGIMACEState *s = opaque;
    int index = mace_compare_index(offset);

    if (mace_audio_timer_access(offset, size)) {
        mace_audio_timestamp_write(&s->audio, (offset - 0x340020) / 8,
                                     (offset & 4) != 0, size, value);
        return;
    }
    if (size == 8) {
        if (index >= 0) {
            s->compare[index] = value;
            s->compare_due[index] = sgi_mace_compare_due_tick(
                mace_elapsed_ticks(s), s->compare[index]);
            s->compare_programmed[index] = true;
            if (s->compare_due[index] == UINT64_MAX) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace: compare equal to current UST is unsupported\n");
            }
            s->istat &= ~MACE_TIMER_BIT(index);
            mace_update_irqs(s);
            mace_schedule_compare(s);
            return;
        }
        switch (offset) {
        case MACE_RING_BASE: {
            bool assert_reset = !(s->ring_base & 1) && (value & 1);

            if (((s->ring_base ^ value) & 0xffff8000U) &&
                !mace_audio_base_allowed(&s->audio)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-mace: ring base change with audio DMA "
                              "out of reset\n");
                return;
            }
            if (!mace_serial_dma_base_allowed(&s->serial_dma, value)) {
                return;
            }
            s->ring_base = value & MACE_RING_WRITABLE;
            if (assert_reset) {
                serial_reset_device(&s->uart[0]);
                serial_reset_device(&s->uart[1]);
            }
            serial_set_rx_discard(&s->uart[0], s->ring_base & 1);
            serial_set_rx_discard(&s->uart[1], s->ring_base & 1);
            qemu_set_irq(s->isa_reset, s->ring_base & 1);
            mace_serial_dma_service(&s->serial_dma);
            return;
        }
        case MACE_IMASK:
            s->imask = value;
            mace_update_irqs(s);
            return;
        case MACE_ISTAT:
            if (value & 0x0000e000U) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-mace: timer status clears through compare writes\n");
            }
            if (value & 0x000f1fffU) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-mace: peripheral status write is unsupported\n");
            }
            mace_serial_dma_ack(&s->serial_dma, value);
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mace: write offset 0x%" HWADDR_PRIx
                  " size %u value 0x%" PRIx64 "\n", offset, size, value);
}

static uint64_t mace_uart_read(void *opaque, hwaddr offset, unsigned size)
{
    MACEUARTPort *port = opaque;
    SGIMACEState *s = port->mace;
    unsigned reg = offset >> 8;

    if (s->ring_base & 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace: UART read while external reset is asserted\n");
        return 0;
    }
    if (reg == 7 && (s->uart[port->index].lcr & 0x80)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mace: TI UART IR/clock register is unsupported\n");
        return 0;
    }
    return serial_io_ops.read(&s->uart[port->index], reg, 1);
}

static void mace_uart_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MACEUARTPort *port = opaque;
    SGIMACEState *s = port->mace;
    unsigned reg = offset >> 8;

    if (s->ring_base & 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-mace: UART write while external reset is asserted\n");
        return;
    }
    if (reg == 7 && (s->uart[port->index].lcr & 0x80)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mace: TI UART IR/clock register is unsupported\n");
        return;
    }
    serial_io_ops.write(&s->uart[port->index], reg, value, 1);
}

static uint64_t mace_serial_read(void *opaque, hwaddr offset, unsigned size)
{
    MACEUARTPort *port = opaque;

    return mace_serial_dma_read(&port->mace->serial_dma, port->index,
                                 offset / 8);
}

static void mace_serial_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    MACEUARTPort *port = opaque;

    mace_serial_dma_write(&port->mace->serial_dma, port->index,
                          offset / 8, value);
}

static const MemoryRegionOps mace_serial_ops = {
    .read = mace_serial_read,
    .write = mace_serial_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 8,
    .valid.max_access_size = 8,
    .impl.min_access_size = 8,
    .impl.max_access_size = 8,
};

static const MemoryRegionOps mace_uart_ops = {
    .read = mace_uart_read,
    .write = mace_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static const MemoryRegionOps mace_ops = {
    .read = mace_read,
    .write = mace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

static void mace_reset_hold(Object *obj, ResetType type)
{
    SGIMACEState *s = SGI_MACE(obj);
    unsigned i;

    s->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->saved_elapsed_ns = 0;
    s->ring_base = 1;
    s->uart_irq_level = 0;
    mace_serial_dma_reset(&s->serial_dma);
    mace_audio_reset(&s->audio);
    mace_i2c_reset(&s->i2c, true);
    s->istat = 0;
    s->imask = 0;
    s->pci_control = 0;
    s->pci_config_addr = 0;
    s->pci_error_addr = 0;
    s->pci_error_flags = 0;
    s->pci_wire_level = 0;
    s->pci_bus_level = 0;
    memset(s->compare, 0, sizeof(s->compare));
    for (i = 0; i < ARRAY_SIZE(s->compare_due); i++) {
        s->compare_due[i] = UINT64_MAX;
    }
    memset(s->compare_programmed, 0, sizeof(s->compare_programmed));
    timer_del(s->timer);
    s->crime_normal = 0;
    s->crime_irq_loading = false;
    s->crime_irq_reconciled = false;
    s->rtc_irq_deferred = false;
    if (s->crime_device) {
        sgi_crime_update_mace_inputs(s->crime_device, MACE_IRQ_MASK, 0);
    }
    for (i = 0; i < 16; i++) {
        qemu_set_irq(s->crime[i], 0);
    }
    serial_set_rx_discard(&s->uart[0], true);
    serial_set_rx_discard(&s->uart[1], true);
    qemu_set_irq(s->isa_reset, 1);
    mace_update_pci_irqs(s);
}

static int mace_pre_save(void *opaque)
{
    SGIMACEState *s = opaque;

    mace_audio_sync(&s->audio);
    s->saved_elapsed_ns = MAX(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                              s->epoch_ns, 0);
    return mace_serial_dma_pre_save(&s->serial_dma);
}

static int mace_pre_load(void *opaque)
{
    SGIMACEState *s = opaque;

    s->crime_irq_loading = true;
    s->crime_irq_reconciled = false;
    s->serial_dma.loading = true;
    return 0;
}

static int mace_post_load(void *opaque, int version_id)
{
    SGIMACEState *s = opaque;

    if ((s->ring_base & ~MACE_RING_WRITABLE) || (s->uart_irq_level & ~3U)) {
        return -EINVAL;
    }
    s->epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                  s->saved_elapsed_ns;
    serial_set_rx_discard(&s->uart[0], s->ring_base & 1);
    serial_set_rx_discard(&s->uart[1], s->ring_base & 1);
    qemu_set_irq(s->isa_reset, s->ring_base & 1);
    mace_schedule_compare(s);
    if (mace_audio_post_load(&s->audio) < 0) {
        return -EINVAL;
    }
    if (mace_serial_dma_post_load(&s->serial_dma) < 0) {
        return -EINVAL;
    }
    sgi_mace_ps2_restore_post_load(s->ps2);
    s->crime_irq_loading = false;
    s->crime_irq_reconciled = true;
    if (s->rtc_irq_deferred) {
        mace_rtc_apply_irq(s, s->rtc_irq_deferred_level);
        s->rtc_irq_deferred = false;
    }
    mace_update_irqs(s);
    mace_update_pci_irqs(s);
    /* Reconcile child IRQ outputs only after N and CRIME's P are restored. */
    sgi_mace_eth_restore_post_load(&s->eth);
    for (unsigned i = 0; i < 16; i++) {
        qemu_set_irq(s->crime[i], (s->crime_normal >> i) & 1);
    }
    return 0;
}

static const VMStateDescription vmstate_mace = {
    .name = TYPE_SGI_MACE,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = mace_pre_save,
    .pre_load = mace_pre_load,
    .post_load = mace_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(saved_elapsed_ns, SGIMACEState),
        VMSTATE_UINT32(ring_base, SGIMACEState),
        VMSTATE_UINT32(istat, SGIMACEState),
        VMSTATE_UINT32(imask, SGIMACEState),
        VMSTATE_UINT8(uart_irq_level, SGIMACEState),
        VMSTATE_UINT16(crime_normal, SGIMACEState),
        VMSTATE_UINT32_ARRAY(compare, SGIMACEState, 3),
        VMSTATE_UINT64_ARRAY(compare_due, SGIMACEState, 3),
        VMSTATE_BOOL_ARRAY(compare_programmed, SGIMACEState, 3),
        VMSTATE_UINT32(pci_control, SGIMACEState),
        VMSTATE_UINT32(pci_config_addr, SGIMACEState),
        VMSTATE_UINT32(pci_error_addr, SGIMACEState),
        VMSTATE_UINT32(pci_error_flags, SGIMACEState),
        VMSTATE_UINT8(pci_wire_level, SGIMACEState),
        VMSTATE_UINT8(pci_bus_level, SGIMACEState),
        VMSTATE_STRUCT_ARRAY(uart, SGIMACEState, 2, 0, vmstate_serial,
                             SerialState),
        VMSTATE_STRUCT(audio, SGIMACEState, 0, vmstate_mace_audio, MACEAudio),
        VMSTATE_UINT8_EQUAL(i2c.enabled, SGIMACEState),
        VMSTATE_STRUCT(serial_dma, SGIMACEState, 0,
                       vmstate_mace_serial_dma, MACESerialDMA),
        VMSTATE_END_OF_LIST()
    },
};

static void mace_init(Object *obj)
{
    SGIMACEState *s = SGI_MACE(obj);

    object_property_add_uint8_ptr(obj, "x-test-pci-bus-level",
                                  &s->pci_bus_level, OBJ_PROP_FLAG_READ);

    memory_region_init(&s->aperture, obj, "sgi-mace", MACE_APERTURE_SIZE);
    memory_region_init_io(&s->regs, obj, &mace_ops, s, "sgi-mace-regs",
                          MACE_APERTURE_SIZE);
    memory_region_add_subregion(&s->aperture, 0, &s->regs);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->aperture);
    memory_region_init(&s->cpu_pci_view, obj, "sgi-mace-cpu-pci-view",
                       16 * MiB);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->cpu_pci_view);
    memory_region_init(&s->cpu_pci_high_view, obj,
                       "sgi-mace-cpu-pci-high-view", 2 * GiB);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->cpu_pci_high_view);
    qdev_init_gpio_out_named(DEVICE(obj), s->crime, "crime", 16);
    qdev_init_gpio_out_named(DEVICE(obj), &s->isa_reset, "isa-reset", 1);
    qdev_init_gpio_in_named(DEVICE(obj), mace_eth_irq, "eth", 1);
    qdev_init_gpio_in_named(DEVICE(obj), mace_rtc_irq, "rtc", 1);
    qdev_init_gpio_in_named(DEVICE(obj), mace_ps2_irq, "ps2", 4);
    qdev_init_gpio_in_named(DEVICE(obj), mace_uart_irq, "uart", 2);
    qdev_init_gpio_in_named(DEVICE(obj), mace_pci_wire_irq, "pci", 8);
    qdev_init_gpio_out_named(DEVICE(obj), s->audio_ttl, "audio-ttl", 2);
    qdev_init_gpio_in_named(DEVICE(obj), mace_audio_button, "volume", 2);
    qdev_init_gpio_in_named(DEVICE(obj), mace_audio_power, "audio-pwrdwn", 1);
    qdev_init_gpio_in_named(DEVICE(obj), mace_audio_feedthrough,
                            "audio-pdmnft", 1);
    object_initialize_child(obj, "eth", &s->eth, TYPE_SGI_MACE_ETH);
    sgi_mace_eth_set_irq_handlers(&s->eth, s, mace_eth_request,
                                  mace_eth_reset_irq);
    s->eth.defer_post_load = true;
    memory_region_init(&s->eth_dma, obj, "sgi-mace-eth-dma", 1ULL << 32);
    memory_region_init(&s->peripheral_dma, obj, "sgi-mace-peripheral-dma",
                       1ULL << 32);
    address_space_init(&s->serial_dma_as, &s->peripheral_dma,
                       "sgi-mace-serial-dma");
    mace_audio_init(&s->audio, &s->serial_dma_as, &s->ring_base,
                     mace_audio_ust, mace_audio_irq, s);
    s->audio.update_ttl = mace_audio_ttl;
    memory_region_init_io(&s->audio_regs, obj, &mace_audio_ops, &s->audio,
                          "sgi-mace-audio", 0x10000);
    memory_region_add_subregion_overlap(&s->aperture, 0x300000,
                                        &s->audio_regs, 1);
    object_property_add_alias(obj, "netdev", OBJECT(&s->eth), "netdev");
    object_property_add_alias(obj, "mac", OBJECT(&s->eth), "mac");
    object_property_add_alias(obj, "phy-ics1890", OBJECT(&s->eth),
                              "phy-ics1890");
    object_property_add_alias(obj, "phy-address", OBJECT(&s->eth),
                              "phy-address");
    object_initialize_child(obj, "rtc", &s->rtc, TYPE_DS1685);
    object_property_add_alias(obj, "drive", OBJECT(&s->rtc), "drive");
    object_property_add_alias(obj, "clock-drive", OBJECT(&s->rtc),
                              "clock-drive");
    object_initialize_child(obj, "uart-a", &s->uart[0], TYPE_SERIAL);
    object_initialize_child(obj, "uart-b", &s->uart[1], TYPE_SERIAL);
    object_property_add_alias(obj, "chardev-a", OBJECT(&s->uart[0]),
                              "chardev");
    object_property_add_alias(obj, "chardev-b", OBJECT(&s->uart[1]),
                              "chardev");
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mace_compare_fire, s);
}

static void mace_finalize(Object *obj)
{
    SGIMACEState *s = SGI_MACE(obj);

    mace_audio_cleanup(&s->audio);
    timer_free(s->timer);
}

static void mace_realize(DeviceState *dev, Error **errp)
{
    SGIMACEState *s = SGI_MACE(dev);
    DeviceState *identity;
    unsigned i;

    if (s->i2c_untimed_test_profile && !qtest_enabled()) {
        error_setg(errp, "untimed MACE I2C is restricted to the qtest profile");
        return;
    }
    if (s->storage_diagnostic_realized) {
        error_setg(errp, "sgi-mace storage diagnostic cannot be "
                         "realized again");
        return;
    }
    if (s->ram &&
        !((memory_region_is_ram(s->ram) &&
           !memory_region_is_rom(s->ram) &&
           memory_region_size(s->ram)) ||
          (!memory_region_is_ram(s->ram) && !s->ram->alias &&
           !s->ram->terminates && memory_region_size(s->ram) == GiB))) {
        error_setg(errp, "sgi-mace requires writable RAM or a 1 GiB "
                         "decoded RAM container for its PCI view");
        return;
    }
    if (s->eth_low_dma_compat &&
        (!s->ram || memory_region_is_ram(s->ram) || s->ram->alias ||
         s->ram->terminates || memory_region_size(s->ram) != GiB)) {
        error_setg(errp, "sgi-mace low Ethernet DMA compatibility requires "
                         "the O2 decoded 1 GiB RAM container");
        return;
    }
    if (s->aic_pci_diagnostic && !qtest_enabled()) {
        error_setg(errp, "sgi-mace AIC PCI diagnostic requires QTest");
        return;
    }
    if (s->pio_byte_test &&
        (!qtest_enabled() || s->aic_pci_diagnostic ||
         s->o2_storage_diagnostic)) {
        error_setg(errp, "sgi-mace PIO byte fixture requires QTest and "
                         "no other PCI diagnostic");
        return;
    }
    if (!s->identity_diagnostic &&
        (s->identity_test_serial || s->identity_eprom_file ||
         s->identity_status)) {
        error_setg(errp, "sgi-mace identity inputs require "
                         "identity-diagnostic=on");
        return;
    }
    if (s->misc_word_pio && !s->identity_diagnostic) {
        error_setg(errp, "sgi-mace misc-word-pio requires "
                         "identity-diagnostic=on");
        return;
    }
    if (s->identity_diagnostic &&
        (!s->identity_test_serial || !s->identity_eprom_file ||
         !s->identity_status)) {
        error_setg(errp, "sgi-mace identity diagnostic requires "
                         "test-serial, eprom-file and status");
        return;
    }
    if (s->o2_storage_diagnostic &&
        (!s->storage_diagnostic_authorized ||
         s->aic_pci_diagnostic || !s->ram ||
         memory_region_size(s->ram) != GiB || memory_region_is_ram(s->ram))) {
        error_setg(errp, "sgi-mace storage diagnostic requires the O2 "
                         "decoded RAM view and machine-owned event sink");
        return;
    }

    if (s->ram) {
        uint64_t size = MIN(memory_region_size(s->ram), 1ULL << 30);

        memory_region_init_alias(&s->peripheral_native_ram, OBJECT(dev),
                                 "sgi-mace-peripheral-native", s->ram, 0, size);
        memory_region_init_alias(&s->peripheral_no_ecc_ram, OBJECT(dev),
                                 "sgi-mace-peripheral-no-ecc", s->ram, 0, size);
        memory_region_add_subregion(&s->peripheral_dma, 0x40000000,
                                    &s->peripheral_native_ram);
        memory_region_add_subregion(&s->peripheral_dma, 0x80000000,
                                    &s->peripheral_no_ecc_ram);
        if (s->eth_low_dma_compat) {
            memory_region_init_alias(&s->eth_low_ram, OBJECT(dev),
                                     "sgi-mace-eth-low-ram", s->ram, 0,
                                     256 * MiB);
            memory_region_add_subregion(&s->eth_dma, 0, &s->eth_low_ram);
        }
        memory_region_init_alias(&s->eth_full_ram, OBJECT(dev),
                                 "sgi-mace-eth-full-ram", s->ram, 0, size);
        memory_region_init_alias(&s->eth_no_ecc_ram, OBJECT(dev),
                                 "sgi-mace-eth-no-ecc-ram", s->ram, 0, size);
        memory_region_add_subregion(&s->eth_dma, 0x40000000,
                                    &s->eth_full_ram);
        memory_region_add_subregion(&s->eth_dma, 0x80000000,
                                    &s->eth_no_ecc_ram);
    }
    if (!mace_audio_realize(&s->audio, errp)) {
        return;
    }
    object_property_set_link(OBJECT(&s->eth), "dma-memory",
                             OBJECT(&s->eth_dma), &error_abort);
    qemu_configure_nic_device(DEVICE(&s->eth), true, NULL);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->eth), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(&s->aperture, MACE_ETH_BASE,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->eth), 0), 1);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->eth), 0,
                       qdev_get_gpio_in_named(dev, "eth", 0));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
        return;
    }
    memory_region_add_subregion_overlap(&s->aperture, 0x3a0000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->rtc), 2), 1);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc), 0,
                       qdev_get_gpio_in_named(dev, "rtc", 0));

    for (i = 0; i < ARRAY_SIZE(s->uart); i++) {
        MACEUARTPort *port = &s->uart_port[i];

        qdev_prop_set_uint32(DEVICE(&s->uart[i]), "baudbase", 115200);
        serial_set_rx_discard(&s->uart[i], true);
        s->uart[i].irq = qdev_get_gpio_in_named(dev, "uart", i);
        if (!qdev_realize(DEVICE(&s->uart[i]), NULL, errp)) {
            return;
        }
        port->mace = s;
        port->index = i;
        memory_region_init_io(&port->mmio, OBJECT(dev), &mace_uart_ops,
                              port, i ? "sgi-mace-uart-b" : "sgi-mace-uart-a",
                              0x800);
        memory_region_add_subregion_overlap(&s->aperture,
                                            i ? MACE_UART_B : MACE_UART_A,
                                            &port->mmio, 1);
    }

    mace_serial_dma_init(&s->serial_dma, s->uart, &s->serial_dma_as,
                         &s->ring_base, mace_serial_update_irq, s);
    for (unsigned p = 0; p < 2; p++) {
        MACEUARTPort *port = &s->serial_dma_port[p];

        port->mace = s;
        port->index = p;
        memory_region_init_io(&port->mmio, OBJECT(dev), &mace_serial_ops, port,
                              p ? "sgi-mace-serial-b" : "sgi-mace-serial-a",
                              0x40);
        memory_region_add_subregion_overlap(&s->aperture, 0x318000 + p * 0x4000,
                                            &port->mmio, 1);
    }

    memory_region_init_io(&s->pci_regs, OBJECT(dev), &mace_pci_ops, s,
                          "sgi-mace-pci-regs", MACE_PCI_SIZE);
    memory_region_add_subregion_overlap(&s->aperture, MACE_PCI_BASE,
                                        &s->pci_regs, 1);
    memory_region_init(&s->pci_memory, OBJECT(dev), "sgi-mace-pci-memory",
                       1ULL << 32);
    memory_region_init(&s->pci_io, OBJECT(dev), "sgi-mace-pci-io",
                       1ULL << 32);
    memory_region_init_io(&s->pci_mem_block, OBJECT(dev),
                          &mace_pci_dma_block_ops, s,
                          "sgi-mace-pci-memory-blocked", 1ULL << 31);
    memory_region_init_io(&s->pci_io_block, OBJECT(dev),
                          &mace_pci_dma_block_ops, s,
                          "sgi-mace-pci-io-blocked", 1ULL << 31);
    memory_region_add_subregion_overlap(&s->pci_memory, 0,
                                        &s->pci_mem_block, 2);
    memory_region_add_subregion_overlap(&s->pci_io, 0,
                                        &s->pci_io_block, 2);
    if (s->ram && !s->aic_pci_diagnostic) {
        uint64_t size = MIN(memory_region_size(s->ram), 1ULL << 30);

        if (s->pci_native_word_swap) {
            address_space_init(&s->pci_ram_as, s->ram, "sgi-mace-pci-ram");
            memory_region_init_io(&s->pci_native_ram, OBJECT(dev),
                                  &mace_pci_native_ops, s,
                                  "sgi-mace-pci-native-ram", size);
            /* CPU PIO through MACE can synchronously start PCI DMA. */
            s->pci_native_ram.disable_reentrancy_guard = true;
        } else {
            memory_region_init_alias(&s->pci_native_ram, OBJECT(dev),
                                     "sgi-mace-pci-native-ram", s->ram, 0,
                                     size);
        }
        memory_region_add_subregion_overlap(&s->pci_memory, 0x40000000,
                                            &s->pci_native_ram, 3);
        if (s->pci_native_word_swap || s->o2_storage_diagnostic) {
            /* The lower view remains a byte-identity approximation. */
            memory_region_init_alias(&s->pci_lower_ram, OBJECT(dev),
                                     "sgi-mace-pci-lower-ram", s->ram, 0,
                                     size);
            memory_region_add_subregion_overlap(&s->pci_memory, 0,
                                                &s->pci_lower_ram, 3);
        }
    }
    address_space_init(&s->pci_dma_as, &s->pci_memory, "sgi-mace-pci-dma");
    if (s->aic_pci_diagnostic) {
        memory_region_init_alias(&s->cpu_pci_alias, OBJECT(dev),
                                 "sgi-mace-cpu-pci-alias", &s->pci_memory,
                                 0x80000000, 8 * KiB);
        memory_region_add_subregion(&s->cpu_pci_view, 0,
                                    &s->cpu_pci_alias);
    }
    if (!s->aic_pci_diagnostic) {
        memory_region_init_io(&s->cpu_pci_pio_low, OBJECT(dev),
                              &mace_cpu_pci_ops, s, "sgi-mace-cpu-pci-pio-low",
                              16 * MiB);
        memory_region_add_subregion(&s->cpu_pci_view, 0,
                                    &s->cpu_pci_pio_low);
        memory_region_init_io(&s->cpu_pci_pio_high, OBJECT(dev),
                              &mace_cpu_pci_ops, s, "sgi-mace-cpu-pci-pio-high",
                              2 * GiB);
        memory_region_add_subregion(&s->cpu_pci_high_view, 0,
                                    &s->cpu_pci_pio_high);
    }
    if (s->o2_storage_diagnostic) {
        s->storage_diagnostic_realized = true;
    }
    PCI_HOST_BRIDGE(s)->bus = pci_register_root_bus(dev, "mace.0",
        mace_pci_set_irq, mace_pci_map_irq, s,
        &s->pci_memory, &s->pci_io, PCI_DEVFN(0, 0), 9, TYPE_PCI_BUS);
    pci_setup_iommu(PCI_HOST_BRIDGE(s)->bus, &mace_pci_dma_ops, s);
    if (s->o2_storage_diagnostic) {
        /* Only the two onboard AIC slots are available in this mode. */
        pci_bus_set_slot_reserved_mask(PCI_HOST_BRIDGE(s)->bus,
                                       ~((1U << 1) | (1U << 2)));
    } else if (s->pio_byte_test) {
        pci_bus_set_slot_reserved_mask(PCI_HOST_BRIDGE(s)->bus,
                                       ~(1U << 3));
    } else {
        pci_bus_set_slot_reserved_mask(PCI_HOST_BRIDGE(s)->bus,
                                       ~((1U << 1) | (1U << 2) | (1U << 3)));
    }
    s->ps2 = qdev_new(TYPE_SGI_MACE_PS2);
    object_property_add_child(OBJECT(dev), "ps2", OBJECT(s->ps2));
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(s->ps2), errp)) {
        object_unparent(OBJECT(s->ps2));
        s->ps2 = NULL;
        return;
    }
    for (i = 0; i < 4; i++) {
        qdev_connect_gpio_out(s->ps2, i,
                              qdev_get_gpio_in_named(dev, "ps2", i));
    }
    memory_region_add_subregion_overlap(&s->aperture, 0x320000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(s->ps2), 0), 1);
    identity = qdev_new(TYPE_SGI_MACE_ID);
    object_property_add_child(OBJECT(dev), "identity", OBJECT(identity));
    qdev_prop_set_bit(identity, "no-responder", !s->identity_diagnostic);
    if (s->identity_diagnostic) {
        qdev_prop_set_string(identity, "test-serial", s->identity_test_serial);
        qdev_prop_set_string(identity, "eprom-file", s->identity_eprom_file);
        qdev_prop_set_string(identity, "status", s->identity_status);
    }
    qdev_prop_set_bit(identity, "misc-word-pio", s->misc_word_pio);
    qdev_prop_set_bit(identity, "password-clear-input",
                      s->password_clear_input);
    if (!sysbus_realize_and_unref(SYS_BUS_DEVICE(identity), errp)) {
        object_unparent(OBJECT(identity));
        return;
    }
    s->identity = identity;
    qdev_pass_gpios(identity, dev, "flash-write-enable");
    qdev_pass_gpios(identity, dev, "password-clear-input");
    memory_region_add_subregion_overlap(&s->aperture, MACE_ID_BASE,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(identity), 0), 1);
    if (s->i2c_untimed_test_profile) {
        object_initialize_child(OBJECT(dev), "i2c-controller", &s->i2c,
                                TYPE_SGI_MACE_I2C);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->i2c), errp)) {
            return;
        }
        memory_region_add_subregion_overlap(&s->aperture, 0x330000,
                                            &s->i2c.regs, 1);
    }
}

static void mace_unrealize(DeviceState *dev)
{
    PCIHostState *phb = PCI_HOST_BRIDGE(dev);

    if (phb->bus) {
        pci_unregister_root_bus(phb->bus);
        phb->bus = NULL;
    }
    if (SGI_MACE(dev)->identity) {
        DeviceState *identity = SGI_MACE(dev)->identity;

        memory_region_del_subregion(&SGI_MACE(dev)->aperture,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(identity), 0));
        qdev_unrealize(identity);
        object_unparent(OBJECT(identity));
        SGI_MACE(dev)->identity = NULL;
    }
    if (SGI_MACE(dev)->ps2) {
        DeviceState *ps2 = SGI_MACE(dev)->ps2;

        memory_region_del_subregion(&SGI_MACE(dev)->aperture,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(ps2), 0));
        qdev_unrealize(ps2);
        object_unparent(OBJECT(ps2));
        SGI_MACE(dev)->ps2 = NULL;
    }
    mace_i2c_cleanup(&SGI_MACE(dev)->i2c);
    if (SGI_MACE(dev)->i2c.enabled) {
        memory_region_del_subregion(&SGI_MACE(dev)->aperture,
                                    &SGI_MACE(dev)->i2c.regs);
        qdev_unrealize(DEVICE(&SGI_MACE(dev)->i2c));
    }
    mace_audio_cleanup(&SGI_MACE(dev)->audio);
    mace_serial_dma_cleanup(&SGI_MACE(dev)->serial_dma);
    address_space_destroy(&SGI_MACE(dev)->serial_dma_as);
    address_space_destroy(&SGI_MACE(dev)->pci_dma_as);
    if (SGI_MACE(dev)->ram && !SGI_MACE(dev)->aic_pci_diagnostic &&
        SGI_MACE(dev)->pci_native_word_swap) {
        address_space_destroy(&SGI_MACE(dev)->pci_ram_as);
    }
    if (SGI_MACE(dev)->aic_pci_diagnostic) {
        memory_region_del_subregion(&SGI_MACE(dev)->cpu_pci_view,
                                    &SGI_MACE(dev)->cpu_pci_alias);
    }
    if (!SGI_MACE(dev)->aic_pci_diagnostic) {
        memory_region_del_subregion(&SGI_MACE(dev)->cpu_pci_view,
                                    &SGI_MACE(dev)->cpu_pci_pio_low);
        memory_region_del_subregion(&SGI_MACE(dev)->cpu_pci_high_view,
                                    &SGI_MACE(dev)->cpu_pci_pio_high);
    }
}

static const Property mace_properties[] = {
    DEFINE_AUDIO_PROPERTIES(SGIMACEState, audio.backend),
    DEFINE_PROP_BOOL("i2c-untimed-test-profile", SGIMACEState,
                     i2c_untimed_test_profile, false),
    DEFINE_PROP_BOOL("password-clear-input", SGIMACEState,
                     password_clear_input, false),
    DEFINE_PROP_LINK("ram", SGIMACEState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("crime-device", SGIMACEState, crime_device,
                     TYPE_SGI_CRIME, DeviceState *),
    DEFINE_PROP_BOOL("aic-pci-diagnostic", SGIMACEState,
                     aic_pci_diagnostic, false),
    DEFINE_PROP_BOOL("o2-storage-diagnostic", SGIMACEState,
                     o2_storage_diagnostic, false),
    DEFINE_PROP_BOOL("pci-native-word-swap", SGIMACEState,
                     pci_native_word_swap, false),
    DEFINE_PROP_BOOL("pio-byte-test", SGIMACEState, pio_byte_test, false),
    DEFINE_PROP_BOOL("identity-diagnostic", SGIMACEState,
                     identity_diagnostic, false),
    DEFINE_PROP_BOOL("misc-word-pio", SGIMACEState, misc_word_pio, false),
    DEFINE_PROP_BOOL("mace-eth-low-dma-compat", SGIMACEState,
                     eth_low_dma_compat, false),
    DEFINE_PROP_STRING("identity-test-serial", SGIMACEState,
                       identity_test_serial),
    DEFINE_PROP_STRING("identity-eprom-file", SGIMACEState,
                       identity_eprom_file),
    DEFINE_PROP_STRING("identity-status", SGIMACEState, identity_status),
};

static void mace_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI MACE 2.0 I/O controller";
    dc->realize = mace_realize;
    dc->unrealize = mace_unrealize;
    dc->user_creatable = true;
    dc->vmsd = &vmstate_mace;
    device_class_set_props(dc, mace_properties);
    rc->phases.hold = mace_reset_hold;
}

static const TypeInfo mace_type_info = {
    .name = TYPE_SGI_MACE,
    .parent = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(SGIMACEState),
    .instance_init = mace_init,
    .instance_finalize = mace_finalize,
    .class_init = mace_class_init,
};

static void mace_register_types(void)
{
    type_register_static(&mace_type_info);
}

type_init(mace_register_types)
