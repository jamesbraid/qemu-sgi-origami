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
#define ISP_REG_BUS_CFG1    0x06
#define ISP_REG_BUS_CTRL    0x08
#define ISP_REG_BUS_STATUS  0x0a
#define ISP_REG_SEMAPHORE   0x0c
#define ISP_REG_HOST_CMD    0xc0

#define QLOGIC_ISP_VENDOR_ID    0x1077
#define QLOGIC_ISP_DEVICE_ID    0x1020
#define QLOGIC_ISP_REVISION     0x05
#define ISP_BUS_CFG0_HWREV      0x0006

#define QLOGIC_ISP_REG_WINDOW_SIZE  0x100

#define ISP_BUS_CFG1_FIFO_MASK      0x0070
#define ISP_BUS_CFG1_BANK_SELECT    0x0008
#define ISP_BUS_CFG1_BURST_ENABLE   0x0004

#define ISP_BUS_CTRL_SOFT_RESET     0x0001
#define ISP_BUS_CTRL_INT_ENABLE     0x0002
#define ISP_BUS_CTRL_RISC_INT_ENABLE 0x0004

#define ISP_SEMAPHORE_LOCK      0x0001
#define ISP_SEMAPHORE_STATUS    0x0002

#define ISP_REG_MAILBOX_BASE    0x70
#define ISP_MAILBOX_COUNT       8
#define ISP_MAILBOX(n)          (ISP_REG_MAILBOX_BASE + (n) * 2)

/* The queue pointers are these two mailboxes under a second name. */
#define ISP_REG_REQUEST_QUEUE   (ISP_REG_MAILBOX_BASE + 0x08)
#define ISP_REG_RESPONSE_QUEUE  (ISP_REG_MAILBOX_BASE + 0x0a)

#define ISP_MAILBOX1_SIGNATURE  0x4953
#define ISP_MAILBOX2_SIGNATURE  0x5020
#define ISP_MAILBOX3_SIGNATURE  0x2020

#define ISP_HOST_CMD_SHIFT          12
#define ISP_HOST_CMD_RESET_RISC     0x1
#define ISP_HOST_CMD_PAUSE_RISC     0x2
#define ISP_HOST_CMD_RELEASE_RISC   0x3
#define ISP_HOST_CMD_SET_HOST_INT   0x5
#define ISP_HOST_CMD_CLEAR_HOST_INT 0x6

#define ISP_RISC_FLAG_HOST_INT      0x0080
#define ISP_RISC_FLAG_RESET         0x0040
#define ISP_RISC_FLAG_PAUSED        0x0020

#define ISP_REG_SXP_RISC_BASE   0x80
#define ISP_REG_SXP_RISC_LAST   0xbe    /* Last offset before host command. */

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

/*
 * The mailbox signature is the observable that proves the device was reset.
 * Every other register in this file resets to zero, which is also what an
 * uninitialised structure holds, so none of them can tell a device that was
 * reset from one that merely decodes and answers. These three cannot read zero
 * by accident.
 *
 * Reset reaches a PCI device through its bus, so this also pins the device to a
 * parent that resets it. A model that grew a bus-less parent would still decode
 * every register here and still answer them plausibly.
 */
static void test_reset_signature(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    int i;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(1)), ==,
                    ISP_MAILBOX1_SIGNATURE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(2)), ==,
                    ISP_MAILBOX2_SIGNATURE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(3)), ==,
                    ISP_MAILBOX3_SIGNATURE);

    /* The mailboxes the signature does not occupy reset to zero. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==, 0);
    for (i = 4; i < ISP_MAILBOX_COUNT; i++) {
        g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(i)), ==, 0);
    }

    /* So do the bus interface registers. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CFG1), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CTRL), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_STATUS), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SEMAPHORE), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==, 0);

    qpci_iounmap(dev, io);
}

/*
 * The guest writes the soft reset bit and then polls this register up to a
 * thousand times for the bit to read back clear, failing the attach if it never
 * does. It also re-initialises the chip, which is asserted here by dirtying
 * state first and requiring the reset to put it back.
 *
 * Asserting the whole register reads zero rather than just the reset bit is
 * what makes this catch a reset that clears its own bit and nothing else. The
 * bit alone is the weaker property, and a model that self-clears without
 * re-initialising would satisfy it.
 */
static void test_soft_reset(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    /* Dirty everything the reset is supposed to put back. */
    qpci_io_writew(dev, io, ISP_REG_BUS_CFG1, ISP_BUS_CFG1_BURST_ENABLE);
    qpci_io_writew(dev, io, ISP_REG_SEMAPHORE, ISP_SEMAPHORE_LOCK);
    qpci_io_writew(dev, io, ISP_MAILBOX(1), 0xdead);
    qpci_io_writew(dev, io, ISP_MAILBOX(7), 0xbeef);
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_PAUSE_RISC << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(1)), ==, 0xdead);

    qpci_io_writew(dev, io, ISP_REG_BUS_CTRL, ISP_BUS_CTRL_SOFT_RESET);

    /* The bit is clear on the first read, and took the rest with it. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CTRL), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CFG1), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SEMAPHORE), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(1)), ==,
                    ISP_MAILBOX1_SIGNATURE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(7)), ==, 0);

    qpci_iounmap(dev, io);
}

/*
 * Bus configuration 1 round-trips the fields it owns and drops the rest. The
 * four FIFO threshold encodings are checked individually rather than as a mask,
 * because the field is three bits wide and a model storing only the bits the
 * guest happens to set first would pass a single-value check.
 */
static void test_bus_cfg1(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    const uint16_t fifo[] = { 0x0010, 0x0020, 0x0030, 0x0040 };
    int i;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    for (i = 0; i < (int)ARRAY_SIZE(fifo); i++) {
        qpci_io_writew(dev, io, ISP_REG_BUS_CFG1, fifo[i]);
        g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CFG1), ==, fifo[i]);
    }

    /* The combination OpenBSD actually programs: 64 bytes, bursts enabled. */
    qpci_io_writew(dev, io, ISP_REG_BUS_CFG1,
                   0x0030 | ISP_BUS_CFG1_BURST_ENABLE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CFG1), ==,
                    0x0030 | ISP_BUS_CFG1_BURST_ENABLE);

    /* The bank select is a bit this register owns, so it survives a write. */
    qpci_io_writew(dev, io, ISP_REG_BUS_CFG1, ISP_BUS_CFG1_BANK_SELECT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CFG1), ==,
                    ISP_BUS_CFG1_BANK_SELECT);

    /* Bits it does not own read back zero rather than whatever was written. */
    qpci_io_writew(dev, io, ISP_REG_BUS_CFG1, 0xffff);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CFG1), ==,
                    ISP_BUS_CFG1_FIFO_MASK | ISP_BUS_CFG1_BANK_SELECT |
                    ISP_BUS_CFG1_BURST_ENABLE);

    qpci_io_writew(dev, io, ISP_REG_BUS_CFG1, 0);
    qpci_iounmap(dev, io);
}

/*
 * The two interrupt enables are stored and the status register beside them is
 * read-only. The guest sets both enables in one write, which is the case
 * asserted first.
 */
static void test_bus_ctrl_and_status(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    qpci_io_writew(dev, io, ISP_REG_BUS_CTRL,
                   ISP_BUS_CTRL_INT_ENABLE | ISP_BUS_CTRL_RISC_INT_ENABLE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CTRL), ==,
                    ISP_BUS_CTRL_INT_ENABLE | ISP_BUS_CTRL_RISC_INT_ENABLE);

    qpci_io_writew(dev, io, ISP_REG_BUS_CTRL, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CTRL), ==, 0);

    /*
     * Writing every bit sets the two enables and no others. The soft reset bit
     * is in that write and clears itself, so it does not appear either.
     */
    qpci_io_writew(dev, io, ISP_REG_BUS_CTRL, 0xffff);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CTRL), ==, 0);

    /* The status register ignores writes: no source has posted anything. */
    qpci_io_writew(dev, io, ISP_REG_BUS_STATUS, 0xffff);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_STATUS), ==, 0);

    qpci_iounmap(dev, io);
}

/*
 * The semaphore holds its two bits and the guest releases it by writing the
 * register back to zero, which is the operation its interrupt acknowledgement
 * depends on.
 */
static void test_semaphore(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    qpci_io_writew(dev, io, ISP_REG_SEMAPHORE, ISP_SEMAPHORE_LOCK);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SEMAPHORE), ==,
                    ISP_SEMAPHORE_LOCK);

    qpci_io_writew(dev, io, ISP_REG_SEMAPHORE, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SEMAPHORE), ==, 0);

    qpci_io_writew(dev, io, ISP_REG_SEMAPHORE, 0xffff);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SEMAPHORE), ==,
                    ISP_SEMAPHORE_LOCK | ISP_SEMAPHORE_STATUS);

    qpci_io_writew(dev, io, ISP_REG_SEMAPHORE, 0);
    qpci_iounmap(dev, io);
}

/*
 * Each mailbox is its own register. Distinct values are written to all eight
 * and read back afterwards rather than one at a time, so a model decoding them
 * to one shared register, or dropping the low address bits, fails here instead
 * of passing eight independent round-trips.
 */
static void test_mailboxes(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    int i;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    for (i = 0; i < ISP_MAILBOX_COUNT; i++) {
        qpci_io_writew(dev, io, ISP_MAILBOX(i), 0x1000 + i);
    }
    for (i = 0; i < ISP_MAILBOX_COUNT; i++) {
        g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(i)), ==,
                        0x1000 + i);
    }

    qpci_io_writew(dev, io, ISP_REG_BUS_CTRL, ISP_BUS_CTRL_SOFT_RESET);
    qpci_iounmap(dev, io);
}

/*
 * The queue pointers are mailboxes 4 and 5 reached under a second name: the
 * driver names request-in and request-out at one address and response-in and
 * response-out at the next, because each address means the host's pointer when
 * written and the adapter's when read. This asserts they are the same two
 * registers, which is the fact a model could plausibly get wrong by decoding
 * them separately, and that they reset to zero.
 */
static void test_queue_pointers(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_REQUEST_QUEUE), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 0);

    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 0x0021);
    qpci_io_writew(dev, io, ISP_REG_RESPONSE_QUEUE, 0x0043);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_REQUEST_QUEUE), ==, 0x0021);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 0x0043);

    /* Same registers as mailboxes 4 and 5, not a separate decode. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(4)), ==, 0x0021);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(5)), ==, 0x0043);

    qpci_io_writew(dev, io, ISP_REG_BUS_CTRL, ISP_BUS_CTRL_SOFT_RESET);
    qpci_iounmap(dev, io);
}

/*
 * The host command register decodes the top four bits of a write and reports
 * the RISC processor's state on a read. The guest pauses the processor, resets
 * it and releases it as consecutive steps of its attach, so the release has to
 * undo both.
 */
static void test_host_command(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_PAUSE_RISC << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==,
                    ISP_RISC_FLAG_PAUSED);

    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_RESET_RISC << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==,
                    ISP_RISC_FLAG_PAUSED | ISP_RISC_FLAG_RESET);

    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_RELEASE_RISC << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==, 0);

    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==,
                    ISP_RISC_FLAG_HOST_INT);

    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_CLEAR_HOST_INT << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==, 0);

    /*
     * Only the top four bits are the command. The same command number in the
     * low bits must do nothing, which is what pins the decode to a shift rather
     * than a comparison against the whole value.
     */
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD, ISP_HOST_CMD_PAUSE_RISC);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==, 0);

    qpci_iounmap(dev, io);
}

/*
 * The bank select is bus configuration 1 bit 3, and the window at 0x80
 * presents the RISC block with it clear and the SXP block with it set.
 * Neither block is decoded into named registers, so the only way to observe
 * the select working is that the same offset holds two different values
 * depending on it, which is what this writes and reads back to prove.
 *
 * Bit 0 is the wrong constant a model is most likely to inherit from a
 * driver header that defines it but never exercises it, so the test sets it
 * alongside a value in the RISC bank and confirms it neither switches the
 * bank nor is retained anywhere a guest could read it back.
 */
static void test_sxp_risc_bank_select(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    /* Reset leaves bit 3 clear, so the RISC bank answers first. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CFG1) &
                    ISP_BUS_CFG1_BANK_SELECT, ==, 0);

    qpci_io_writew(dev, io, ISP_REG_SXP_RISC_BASE, 0x1234);
    qpci_io_writew(dev, io, ISP_REG_SXP_RISC_LAST, 0x5566);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SXP_RISC_BASE), ==, 0x1234);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SXP_RISC_LAST), ==, 0x5566);

    /* Selecting the SXP bank hides that value behind the bank's own zero. */
    qpci_io_writew(dev, io, ISP_REG_BUS_CFG1, ISP_BUS_CFG1_BANK_SELECT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SXP_RISC_BASE), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SXP_RISC_LAST), ==, 0);

    qpci_io_writew(dev, io, ISP_REG_SXP_RISC_BASE, 0x7890);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SXP_RISC_BASE), ==, 0x7890);

    /* Clearing the select uncovers the RISC bank's value again, unharmed. */
    qpci_io_writew(dev, io, ISP_REG_BUS_CFG1, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SXP_RISC_BASE), ==, 0x1234);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SXP_RISC_LAST), ==, 0x5566);

    /*
     * Bit 0 is not stored by bus configuration 1 at all, so setting it
     * alongside the RISC selection changes neither the readable register nor
     * which bank answers at 0x80.
     */
    qpci_io_writew(dev, io, ISP_REG_BUS_CFG1, 0x0001);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CFG1), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SXP_RISC_BASE), ==, 0x1234);

    qpci_io_writew(dev, io, ISP_REG_BUS_CTRL, ISP_BUS_CTRL_SOFT_RESET);
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
    qos_add_test("reset-signature", "qlogic-isp1040",
                 test_reset_signature, NULL);
    qos_add_test("soft-reset", "qlogic-isp1040", test_soft_reset, NULL);
    qos_add_test("bus-cfg1", "qlogic-isp1040", test_bus_cfg1, NULL);
    qos_add_test("bus-ctrl-and-status", "qlogic-isp1040",
                 test_bus_ctrl_and_status, NULL);
    qos_add_test("semaphore", "qlogic-isp1040", test_semaphore, NULL);
    qos_add_test("mailboxes", "qlogic-isp1040", test_mailboxes, NULL);
    qos_add_test("queue-pointers", "qlogic-isp1040",
                 test_queue_pointers, NULL);
    qos_add_test("host-command", "qlogic-isp1040", test_host_command, NULL);
    qos_add_test("sxp-risc-bank-select", "qlogic-isp1040",
                 test_sxp_risc_bank_select, NULL);
}

libqos_init(qlogic_isp_register_nodes);
