/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the QLogic ISP1020 family SCSI host adapter.
 *
 * The device is a plain PCI adapter, so the qos graph exercises its register
 * contract on whichever generic PCI machine it can reach. Nothing asserted here
 * depends on the processor at the other end of the bus: the registers are
 * little-endian on the bus whatever the target, and the model declares that
 * rather than swapping by hand.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/module.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "hw/pci/pci_regs.h"

/* Register offsets within the 16-bit window. */
#define ISP_REG_ID_LOW      0x00
#define ISP_REG_ID_HIGH     0x02
#define ISP_REG_BUS_CFG0    0x04

#define QLOGIC_ISP_VENDOR_ID    0x1077
#define QLOGIC_ISP_DEVICE_ID    0x1020
#define QLOGIC_ISP_REVISION     0x05
#define ISP_BUS_CFG0_HWREV      0x0006

#define QLOGIC_ISP_REG_WINDOW_SIZE  0x100

typedef struct QQlogicIsp QQlogicIsp;

struct QQlogicIsp {
    QOSGraphObject obj;
    QPCIDevice dev;
};

static void *qlogic_isp_get_driver(void *obj, const char *interface)
{
    QQlogicIsp *isp = obj;

    if (!g_strcmp0(interface, "pci-device")) {
        return &isp->dev;
    }

    fprintf(stderr, "%s not present in qlogic-isp\n", interface);
    g_assert_not_reached();
}

static void *qlogic_isp_create(void *pci_bus, QGuestAllocator *alloc,
                               void *addr)
{
    QQlogicIsp *isp = g_new0(QQlogicIsp, 1);
    QPCIBus *bus = pci_bus;

    qpci_device_init(&isp->dev, bus, addr);
    isp->obj.get_driver = qlogic_isp_get_driver;

    return &isp->obj;
}

/*
 * Configuration space names the family and carries the revision that selects
 * the part within it. The revision is functional rather than decorative:
 * OpenBSD switches on this byte to derive the adapter's 60 MHz clock, so a
 * model reporting a different value negotiates every synchronous transfer
 * against the wrong reference.
 */
static void test_identity(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;

    g_assert_cmphex(qpci_config_readw(dev, PCI_VENDOR_ID), ==,
                    QLOGIC_ISP_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==,
                    QLOGIC_ISP_DEVICE_ID);
    g_assert_cmphex(qpci_config_readb(dev, PCI_REVISION_ID), ==,
                    QLOGIC_ISP_REVISION);
    /* Class 0x01 mass storage, subclass 0x00 SCSI. */
    g_assert_cmphex(qpci_config_readw(dev, PCI_CLASS_DEVICE), ==, 0x0100);
    /* The adapter presents one interrupt line, on INTA. */
    g_assert_cmphex(qpci_config_readb(dev, PCI_INTERRUPT_PIN), ==, 0x01);
}

/*
 * Both base address registers reach the same 0x100-byte window, and the window
 * is a file of 16-bit registers rather than one register aliased across it.
 *
 * Reading the identity words back is what proves the stride: the two sit at
 * adjacent 16-bit offsets and hold different values, so a model that decoded
 * the window at a coarser stride, or ignored the low address bits, would return
 * the same value from both. Asserting each word's full value rather than a
 * field of it leaves nothing weaker for a wrong decode to satisfy.
 */
static void test_window(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io, mmio;
    uint64_t io_size = 0, mmio_size = 0;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, &io_size);
    mmio = qpci_iomap(dev, 1, &mmio_size);

    g_assert_cmpuint(io_size, ==, QLOGIC_ISP_REG_WINDOW_SIZE);
    g_assert_cmpuint(mmio_size, ==, QLOGIC_ISP_REG_WINDOW_SIZE);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_ID_LOW), ==,
                    QLOGIC_ISP_VENDOR_ID);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_ID_HIGH), ==,
                    QLOGIC_ISP_DEVICE_ID);

    /* The memory window reaches the same registers as the I/O window. */
    g_assert_cmphex(qpci_io_readw(dev, mmio, ISP_REG_ID_LOW), ==,
                    QLOGIC_ISP_VENDOR_ID);
    g_assert_cmphex(qpci_io_readw(dev, mmio, ISP_REG_ID_HIGH), ==,
                    QLOGIC_ISP_DEVICE_ID);

    qpci_iounmap(dev, io);
    qpci_iounmap(dev, mmio);
}

/*
 * Bus configuration 0 carries the other revision, and the two are numbered from
 * different origins: this field runs one ahead of the configuration-space byte
 * across the family, so the same part reports 5 there and 6 here. Linux is the
 * guest that reads this one.
 *
 * The whole register is asserted, not the revision nibble alone. The nibble is
 * the field the guest uses, which makes it the weaker property and the one a
 * model with stray bits set elsewhere would still pass; pinning the register
 * says what the guest actually reads.
 */
static void test_bus_cfg0_revision(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CFG0), ==,
                    ISP_BUS_CFG0_HWREV);
    /*
     * The value Linux tests for is the ISP1040A's broken-FIFO workaround, and
     * this part must not select it.
     */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CFG0) & 0x000f, !=,
                    0x0004);

    qpci_iounmap(dev, io);
}

static void qlogic_isp_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "addr=04.0",
    };

    add_qpci_address(&opts, &(QPCIAddress) { .devfn = QPCI_DEVFN(4, 0) });

    qos_node_create_driver("qlogic-isp1040", qlogic_isp_create);
    qos_node_consumes("qlogic-isp1040", "pci-bus", &opts);
    qos_node_produces("qlogic-isp1040", "pci-device");

    qos_add_test("identity", "qlogic-isp1040", test_identity, NULL);
    qos_add_test("window", "qlogic-isp1040", test_window, NULL);
    qos_add_test("bus-cfg0-revision", "qlogic-isp1040",
                 test_bus_cfg0_revision, NULL);
}

libqos_init(qlogic_isp_register_nodes);
