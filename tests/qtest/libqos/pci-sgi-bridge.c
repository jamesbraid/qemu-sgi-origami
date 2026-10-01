/*
 * libqos PCI bindings for the SGI Bridge head
 *
 * The head is one Bridge on the "none" machine with a block of memory beside
 * it, so this file owns the whole address map: where the chip's XIO
 * face is mapped, which of its windows carries PCI I/O space, and which
 * addresses the allocator may hand out.
 *
 * Three windows carry everything. Configuration space is the chip's type 0
 * window, eight devices of 4 KB. PCI memory space is the window that reaches
 * PCI address zero whole, so no per-card window has to be aimed. PCI I/O
 * space is only reachable through a slot's device window, so the bus takes
 * one over.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "../libqtest.h"
#include "../sgi-test.h"
#include "pci-sgi-bridge.h"
#include "qemu/bswap.h"
#include "qemu/module.h"

/* Where this bus puts the head's XIO face and the transfer memory. */
#define WIDGET_BASE             0x40000000ULL
#define TRANSFER_RAM_BASE       0x02000000ULL
#define TRANSFER_RAM_SIZE       (16 * 1024 * 1024)
#define TRANSFER_RAM_PAGE       4096

/* Offsets the chip answers at inside its widget space. */
#define REG_DATA                4
#define DEVICE_REG(slot)        (0x200 + 8 * (slot))
#define CFG0_BASE               0x020000
#define CFG0_DEVICE_STRIDE      0x1000
#define CFG0_FUNCTION_STRIDE    0x100
#define XLATE_BASE              0x40000000ULL
#define XLATE_SIZE              0x40000000ULL

/* The per-slot register fields this bus writes. */
#define DEV_WINDOW_BASE         0xfffu

#define SLOTS                   8

/* Device windows: two 2 MB slots, then six 1 MB slots. */
static const uint64_t window_offset[SLOTS] = {
    0x200000, 0x400000, 0x600000, 0x700000,
    0x800000, 0x900000, 0xa00000, 0xb00000,
};

/*
 * Allocated transfer addresses name both PCI and qtest memory: below the
 * translated window, transfers reach the same fabric address. Card registers
 * sit above transfer memory, within the translated window.
 */
#define PIO_BASE                0x1000
#define PIO_LIMIT               0x100000
#define MMIO_BASE               0x10000000ULL
#define MMIO_LIMIT              XLATE_SIZE

static QPCIBusSGIBridge *sgi_bridge_bus(QPCIBus *bus)
{
    return container_of(bus, QPCIBusSGIBridge, bus);
}

/*
 * Narrow accesses use offset xor (4 - width). Every window reverses this
 * mapping, so callers supply the offset the chip maps to the target byte.
 */
static uint64_t sgi_bridge_aim(uint64_t offset, unsigned int size)
{
    return (offset & ~UINT64_C(3)) | ((offset & 3) ^ (4 - size));
}

static uint64_t sgi_bridge_pio_at(QPCIBus *bus, uint32_t addr,
                                  unsigned int size)
{
    QPCIBusSGIBridge *s = sgi_bridge_bus(bus);

    return WIDGET_BASE + window_offset[s->io_slot] + sgi_bridge_aim(addr, size);
}

static uint8_t qpci_sgi_bridge_pio_readb(QPCIBus *bus, uint32_t addr)
{
    return qtest_readb(bus->qts, sgi_bridge_pio_at(bus, addr, 1));
}

static void qpci_sgi_bridge_pio_writeb(QPCIBus *bus, uint32_t addr, uint8_t val)
{
    qtest_writeb(bus->qts, sgi_bridge_pio_at(bus, addr, 1), val);
}

static uint16_t qpci_sgi_bridge_pio_readw(QPCIBus *bus, uint32_t addr)
{
    return qtest_readw(bus->qts, sgi_bridge_pio_at(bus, addr, 2));
}

static void qpci_sgi_bridge_pio_writew(QPCIBus *bus, uint32_t addr,
                                       uint16_t val)
{
    qtest_writew(bus->qts, sgi_bridge_pio_at(bus, addr, 2), val);
}

static uint32_t qpci_sgi_bridge_pio_readl(QPCIBus *bus, uint32_t addr)
{
    return qtest_readl(bus->qts, sgi_bridge_pio_at(bus, addr, 4));
}

static void qpci_sgi_bridge_pio_writel(QPCIBus *bus, uint32_t addr,
                                       uint32_t val)
{
    qtest_writel(bus->qts, sgi_bridge_pio_at(bus, addr, 4), val);
}

/* Two words, low half first: a doubleword through a window is a pair. */
static uint64_t qpci_sgi_bridge_pio_readq(QPCIBus *bus, uint32_t addr)
{
    return (uint64_t)qpci_sgi_bridge_pio_readl(bus, addr) |
           ((uint64_t)qpci_sgi_bridge_pio_readl(bus, addr + 4) << 32);
}

static void qpci_sgi_bridge_pio_writeq(QPCIBus *bus, uint32_t addr,
                                       uint64_t val)
{
    qpci_sgi_bridge_pio_writel(bus, addr, val);
    qpci_sgi_bridge_pio_writel(bus, addr + 4, val >> 32);
}

/*
 * Use the widest aligned cycle allowed by the caller's buffer. A halfword
 * request still reaches a halfword-only register file as one halfword cycle.
 */
static unsigned int sgi_bridge_step(uint32_t addr, size_t len)
{
    if (len >= 4 && !(addr & 3)) {
        return 4;
    }
    if (len >= 2 && !(addr & 1)) {
        return 2;
    }
    return 1;
}

static void qpci_sgi_bridge_memread(QPCIBus *bus, uint32_t addr, void *buf,
                                    size_t len)
{
    uint8_t *out = buf;

    while (len) {
        unsigned int size = sgi_bridge_step(addr, len);
        uint64_t at = WIDGET_BASE + XLATE_BASE + sgi_bridge_aim(addr, size);

        switch (size) {
        case 4:
            stl_le_p(out, qtest_readl(bus->qts, at));
            break;
        case 2:
            stw_le_p(out, qtest_readw(bus->qts, at));
            break;
        default:
            *out = qtest_readb(bus->qts, at);
            break;
        }
        addr += size;
        out += size;
        len -= size;
    }
}

static void qpci_sgi_bridge_memwrite(QPCIBus *bus, uint32_t addr,
                                     const void *buf, size_t len)
{
    const uint8_t *in = buf;

    while (len) {
        unsigned int size = sgi_bridge_step(addr, len);
        uint64_t at = WIDGET_BASE + XLATE_BASE + sgi_bridge_aim(addr, size);

        switch (size) {
        case 4:
            qtest_writel(bus->qts, at, ldl_le_p(in));
            break;
        case 2:
            qtest_writew(bus->qts, at, lduw_le_p(in));
            break;
        default:
            qtest_writeb(bus->qts, at, *in);
            break;
        }
        addr += size;
        in += size;
        len -= size;
    }
}

static uint64_t sgi_bridge_config_at(int devfn, uint8_t offset,
                                     unsigned int size)
{
    return WIDGET_BASE + CFG0_BASE +
           (devfn >> 3) * CFG0_DEVICE_STRIDE +
           (devfn & 7) * CFG0_FUNCTION_STRIDE +
           sgi_bridge_aim(offset, size);
}

/* Past the eighth slot the chip decodes nothing, which reads as no card. */
static bool sgi_bridge_has_slot(int devfn)
{
    return (devfn >> 3) < SLOTS;
}

static uint8_t qpci_sgi_bridge_config_readb(QPCIBus *bus, int devfn,
                                            uint8_t offset)
{
    if (!sgi_bridge_has_slot(devfn)) {
        return 0xff;
    }
    return qtest_readb(bus->qts, sgi_bridge_config_at(devfn, offset, 1));
}

static uint16_t qpci_sgi_bridge_config_readw(QPCIBus *bus, int devfn,
                                             uint8_t offset)
{
    if (!sgi_bridge_has_slot(devfn)) {
        return 0xffff;
    }
    return qtest_readw(bus->qts, sgi_bridge_config_at(devfn, offset, 2));
}

static uint32_t qpci_sgi_bridge_config_readl(QPCIBus *bus, int devfn,
                                             uint8_t offset)
{
    if (!sgi_bridge_has_slot(devfn)) {
        return 0xffffffff;
    }
    return qtest_readl(bus->qts, sgi_bridge_config_at(devfn, offset, 4));
}

static void qpci_sgi_bridge_config_writeb(QPCIBus *bus, int devfn,
                                          uint8_t offset, uint8_t value)
{
    g_assert(sgi_bridge_has_slot(devfn));
    qtest_writeb(bus->qts, sgi_bridge_config_at(devfn, offset, 1), value);
}

static void qpci_sgi_bridge_config_writew(QPCIBus *bus, int devfn,
                                          uint8_t offset, uint16_t value)
{
    g_assert(sgi_bridge_has_slot(devfn));
    qtest_writew(bus->qts, sgi_bridge_config_at(devfn, offset, 2), value);
}

static void qpci_sgi_bridge_config_writel(QPCIBus *bus, int devfn,
                                          uint8_t offset, uint32_t value)
{
    g_assert(sgi_bridge_has_slot(devfn));
    qtest_writel(bus->qts, sgi_bridge_config_at(devfn, offset, 4), value);
}

/*
 * Map PCI I/O through an empty slot's device window. A card may disable the
 * byte-lane correction by declaring that it orders the bytes itself.
 */
static unsigned int sgi_bridge_take_io_window(QTestState *qts)
{
    unsigned int slot;

    for (slot = 0; slot < SLOTS; slot++) {
        uint64_t reg = WIDGET_BASE + REG_DATA + DEVICE_REG(slot);
        uint32_t control;

        if (qtest_readl(qts, WIDGET_BASE + CFG0_BASE +
                             slot * CFG0_DEVICE_STRIDE) != 0xffffffff) {
            continue;
        }

        control = qtest_readl(qts, reg);
        control &= ~(uint32_t)(DEV_MEMORY_SPACE | DEV_WINDOW_BASE);
        qtest_writel(qts, reg, control);
        return slot;
    }

    g_assert_not_reached();
}

static void *qpci_sgi_bridge_get_driver(void *obj, const char *interface)
{
    QPCIBusSGIBridge *qpci = obj;

    if (!g_strcmp0(interface, "sgi-pci-bus")) {
        return &qpci->bus;
    }
    fprintf(stderr, "%s not present in pci-bus-sgi-bridge\n", interface);
    g_assert_not_reached();
}

void qpci_sgi_bridge_alloc_init(QGuestAllocator *alloc, QAllocOpts flags)
{
    alloc_init(alloc, flags, TRANSFER_RAM_BASE,
               TRANSFER_RAM_BASE + TRANSFER_RAM_SIZE, TRANSFER_RAM_PAGE);
}

QPCIBus *qpci_init_sgi_bridge(QTestState *qts, QGuestAllocator *alloc)
{
    QPCIBusSGIBridge *qpci = g_new0(QPCIBusSGIBridge, 1);

    g_assert(qts);

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/bridge", 0, WIDGET_BASE);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/transfer-ram", 0,
                          TRANSFER_RAM_BASE);

    qpci->bus.has_buggy_msi = false;

    qpci->bus.pio_readb = qpci_sgi_bridge_pio_readb;
    qpci->bus.pio_readw = qpci_sgi_bridge_pio_readw;
    qpci->bus.pio_readl = qpci_sgi_bridge_pio_readl;
    qpci->bus.pio_readq = qpci_sgi_bridge_pio_readq;

    qpci->bus.pio_writeb = qpci_sgi_bridge_pio_writeb;
    qpci->bus.pio_writew = qpci_sgi_bridge_pio_writew;
    qpci->bus.pio_writel = qpci_sgi_bridge_pio_writel;
    qpci->bus.pio_writeq = qpci_sgi_bridge_pio_writeq;

    qpci->bus.memread = qpci_sgi_bridge_memread;
    qpci->bus.memwrite = qpci_sgi_bridge_memwrite;

    qpci->bus.config_readb = qpci_sgi_bridge_config_readb;
    qpci->bus.config_readw = qpci_sgi_bridge_config_readw;
    qpci->bus.config_readl = qpci_sgi_bridge_config_readl;

    qpci->bus.config_writeb = qpci_sgi_bridge_config_writeb;
    qpci->bus.config_writew = qpci_sgi_bridge_config_writew;
    qpci->bus.config_writel = qpci_sgi_bridge_config_writel;

    qpci->bus.qts = qts;
    qpci->bus.pio_alloc_ptr = PIO_BASE;
    qpci->bus.pio_limit = PIO_LIMIT;
    qpci->bus.mmio_alloc_ptr = MMIO_BASE;
    qpci->bus.mmio_limit = MMIO_LIMIT;

    qpci->io_slot = sgi_bridge_take_io_window(qts);
    qpci->obj.get_driver = qpci_sgi_bridge_get_driver;

    return &qpci->bus;
}

void qpci_free_sgi_bridge(QPCIBus *bus)
{
    if (!bus) {
        return;
    }
    g_free(sgi_bridge_bus(bus));
}

/*
 * Expose this bus by name: its Bridge and memory lack the message-signalled
 * interrupts, hotplug handler and NUMA-backed vhost memory expected by
 * generic PCI device tests.
 */
static void qpci_sgi_bridge_register_nodes(void)
{
    qos_node_create_driver("pci-bus-sgi-bridge", NULL);
    qos_node_produces("pci-bus-sgi-bridge", "sgi-pci-bus");
}

libqos_init(qpci_sgi_bridge_register_nodes);
