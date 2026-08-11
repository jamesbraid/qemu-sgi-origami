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
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "hw/pci/pci_regs.h"
#include "scsi/constants.h"

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

#define ISP_MBOX_CMD_NOP                 0x0000
#define ISP_MBOX_CMD_EXEC_FIRMWARE       0x0002
#define ISP_MBOX_CMD_WRITE_RAM_WORD      0x0004
#define ISP_MBOX_CMD_VERIFY_CHECKSUM     0x0007
#define ISP_MBOX_CMD_ABOUT_FIRMWARE      0x0008
#define ISP_MBOX_CMD_GET_FIRMWARE_STATUS 0x001f

#define ISP_MBOX_COMPLETE       0x4000
#define ISP_MBOX_INVALID        0x4001
#define ISP_MBOX_TEST_FAILED    0x4003

#define ISP_FIRMWARE_STATUS_MAX_REQUESTS 512

#define ISP_MBOX_CMD_INIT_REQ_QUEUE  0x0010
#define ISP_MBOX_CMD_INIT_RSP_QUEUE  0x0011
#define ISP_MBOX_CMD_PARAM_ERROR     0x4006

#define ISP_QUEUE_ENTRY_SIZE     64

#define ISP_ENTRY_TYPE_REQUEST   0x01
#define ISP_ENTRY_TYPE_STATUS    0x03

#define ISP_REQ_OFF_HANDLE       4
#define ISP_REQ_OFF_DEVICE       8
#define ISP_REQ_OFF_CDBLEN       10
#define ISP_REQ_OFF_TIMEOUT      16
#define ISP_REQ_OFF_SEGCOUNT     18
#define ISP_REQ_OFF_CDB          20

#define ISP_STATUS_OFF_HANDLE       4
#define ISP_STATUS_OFF_SCSI_STATUS  8
#define ISP_STATUS_OFF_COMPLETION   10
#define ISP_STATUS_OFF_SENSE_DATA   32

#define ISP_IOCB_COMPLETE  0x0000

#define ISP_SCSI_STATUS_SENSE_VALID 0x0200

#define ISP_BUS_STATUS_INT_REQ      0x0002
#define ISP_BUS_STATUS_RISC_INT_REQ 0x0004

#define ISP_HOST_CMD_SHIFT          12
#define ISP_HOST_CMD_RESET_RISC     0x1
#define ISP_HOST_CMD_PAUSE_RISC     0x2
#define ISP_HOST_CMD_RELEASE_RISC   0x3
#define ISP_HOST_CMD_SET_HOST_INT   0x5
#define ISP_HOST_CMD_CLEAR_HOST_INT 0x6
#define ISP_HOST_CMD_CLEAR_RISC_INT 0x7

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

    /*
     * Resetting the RISC replaces every flag rather than adding to them, so
     * the paused flag set above does not survive it. A model that only set
     * the reset flag would leave the paused flag standing here, which is the
     * bug this asserts against.
     */
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_RESET_RISC << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==,
                    ISP_RISC_FLAG_RESET);

    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_RELEASE_RISC << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==, 0);

    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==,
                    ISP_RISC_FLAG_HOST_INT);

    /*
     * This command does nothing on real hardware, so the flag it might be
     * mistaken for clearing is still set afterwards.
     */
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_CLEAR_HOST_INT << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==,
                    ISP_RISC_FLAG_HOST_INT);

    /* Clearing the RISC interrupt is what actually clears it. */
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_CLEAR_RISC_INT << ISP_HOST_CMD_SHIFT);
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

/*
 * This suite asserts the register contract a guest driver actually reads and
 * writes, not the physical interrupt wire's assertion at the platform's
 * interrupt controller. No conventional-PCI storage or network device qtest
 * in this tree does the latter — the ones that observe a physical legacy
 * line at all are testing ISA-specific routing behaviour that is itself the
 * feature under test, which is not this device's contract. Reaching the wire
 * here would mean this qtest also programming the machine's PCI-to-ISA
 * interrupt routing, which is generic platform behaviour this device does
 * not own, tested nowhere else in this tree for a device that does not own
 * it either. What is tested is what the datasheet actually specifies at the
 * register level: bus interface status carries the pending-interrupt bits
 * unconditionally, and bus interface control carries the enables that gate
 * whether the wire those bits feed ever asserts.
 */

/*
 * A no-op is the only mailbox command OpenBSD's own attach sequence issues at
 * this stage, used purely to confirm the adapter is alive. Completing it is
 * what proves the mailbox engine — not just the registers it uses — actually
 * runs: the semaphore lock and bus interface status only become non-zero as
 * a side effect of processing a command, so asserting them here is what
 * distinguishes a working engine from one that merely stores whatever is
 * written to mailbox 0.
 */
static void test_mailbox_nop(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    qpci_io_writew(dev, io, ISP_MAILBOX(0), ISP_MBOX_CMD_NOP);
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SEMAPHORE), ==,
                    ISP_SEMAPHORE_LOCK);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_STATUS), ==,
                    ISP_BUS_STATUS_INT_REQ | ISP_BUS_STATUS_RISC_INT_REQ);

    /*
     * Acknowledgement is two steps for a mailbox result. Clearing the RISC
     * interrupt clears bus interface status and the host-interrupt flag;
     * releasing the semaphore is a separate write the driver issues after.
     */
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_CLEAR_RISC_INT << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_STATUS), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SEMAPHORE), ==,
                    ISP_SEMAPHORE_LOCK);

    qpci_io_writew(dev, io, ISP_REG_SEMAPHORE, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SEMAPHORE), ==, 0);

    qpci_iounmap(dev, io);
}

/*
 * A command this model does not implement still completes — as invalid,
 * posted through the same mechanism as a successful command. The mailbox
 * protocol has no way to leave a command pending: every write of the host
 * interrupt gets an answer, and unimplemented is one of the two answers
 * every command can receive.
 */
static void test_mailbox_invalid_command(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    qpci_io_writew(dev, io, ISP_MAILBOX(0), 0x00ff);
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_INVALID);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SEMAPHORE), ==,
                    ISP_SEMAPHORE_LOCK);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_STATUS), ==,
                    ISP_BUS_STATUS_INT_REQ | ISP_BUS_STATUS_RISC_INT_REQ);

    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_CLEAR_RISC_INT << ISP_HOST_CMD_SHIFT);
    qpci_io_writew(dev, io, ISP_REG_SEMAPHORE, 0);
    qpci_iounmap(dev, io);
}

/* Loads mailboxes 0 through 3 and triggers processing; caller reads back. */
static void qlogic_isp_mbox_call4(QPCIDevice *dev, QPCIBar io, uint16_t mb0,
                                  uint16_t mb1, uint16_t mb2, uint16_t mb3)
{
    qpci_io_writew(dev, io, ISP_MAILBOX(0), mb0);
    qpci_io_writew(dev, io, ISP_MAILBOX(1), mb1);
    qpci_io_writew(dev, io, ISP_MAILBOX(2), mb2);
    qpci_io_writew(dev, io, ISP_MAILBOX(3), mb3);
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);
}

static void qlogic_isp_ack(QPCIDevice *dev, QPCIBar io)
{
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_CLEAR_RISC_INT << ISP_HOST_CMD_SHIFT);
    qpci_io_writew(dev, io, ISP_REG_SEMAPHORE, 0);
}

/*
 * Builds a tiny firmware image the way the guest's own download loop does —
 * one write-ram-word command per word, address then data — verifies its
 * checksum, executes it, and confirms the mailbox engine still answers
 * afterward. None of this is ever run; accepting and answering it correctly
 * is the entire contract.
 *
 * The image is constructed so its checksum is genuinely zero rather than
 * asserting success and trusting the model to agree with itself: word 3 is
 * the length the guest's own loop would have used to know when to stop
 * sending, and the other three words are chosen so the sum of all four wraps
 * to zero, which is what a real firmware image built for this checksum
 * satisfies by construction rather than by accident.
 */
static void test_firmware_download(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    const uint16_t base = 0x1000;
    const uint16_t image[] = { 0x0000, 0x0000, 0xfffc, 0x0004 };
    size_t i;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    for (i = 0; i < ARRAY_SIZE(image); i++) {
        qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_WRITE_RAM_WORD,
                              base + i, image[i], 0);
        g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                        ISP_MBOX_COMPLETE);
        qlogic_isp_ack(dev, io);
    }

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_VERIFY_CHECKSUM, base, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(1)), ==, 0);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_EXEC_FIRMWARE, base, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    /* The mailbox engine answers a plain command the same as before. */
    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_NOP, 0, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qpci_iounmap(dev, io);
}

/*
 * A non-zero sum is answered as a failure, with the sum itself in mailbox 1
 * — what OpenBSD's own driver prints on this path. Without this, a model
 * that always answered complete would look identical to a correct one on
 * every image that happens to already sum to zero, which the download test
 * above uses and would not by itself catch a checksum that never runs.
 */
static void test_verify_checksum_failure(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    const uint16_t base = 0x2000;
    const uint16_t image[] = { 0x0001, 0x0002, 0x0003, 0x0004 };
    size_t i;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    for (i = 0; i < ARRAY_SIZE(image); i++) {
        qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_WRITE_RAM_WORD,
                              base + i, image[i], 0);
        qlogic_isp_ack(dev, io);
    }

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_VERIFY_CHECKSUM, base, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_TEST_FAILED);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(1)), ==, 0x000a);
    qlogic_isp_ack(dev, io);

    qpci_iounmap(dev, io);
}

/*
 * The driver prints this triple and aborts if the command fails, so the only
 * hard requirement is that it completes; the values are this model's own
 * declaration rather than a real QLogic release, which the datasheet states
 * is acceptable for a guest that never inspects them.
 */
static void test_about_firmware(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_ABOUT_FIRMWARE, 0, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(6)), ==, 0);
    qlogic_isp_ack(dev, io);

    qpci_iounmap(dev, io);
}

/*
 * Sizes every queue the guest allocates. 512 is asserted explicitly rather
 * than just checking the command completes, because the datasheet marks
 * this value as a recorded decision rather than a driver-confirmed one, and
 * a qtest is what pins a decision in place against an accidental change.
 */
static void test_get_firmware_status(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_GET_FIRMWARE_STATUS, 0, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(2)), ==,
                    ISP_FIRMWARE_STATUS_MAX_REQUESTS);
    qlogic_isp_ack(dev, io);

    qpci_iounmap(dev, io);
}

/* Writes a 32-bit little-endian value into a request/status entry buffer. */
static void isp_put32(uint8_t *entry, int off, uint32_t val)
{
    uint32_t le = cpu_to_le32(val);

    memcpy(entry + off, &le, 4);
}

static void isp_put16(uint8_t *entry, int off, uint16_t val)
{
    uint16_t le = cpu_to_le16(val);

    memcpy(entry + off, &le, 2);
}

static uint32_t isp_get32(const uint8_t *entry, int off)
{
    uint32_t le;

    memcpy(&le, entry + off, 4);
    return le32_to_cpu(le);
}

static uint16_t isp_get16(const uint8_t *entry, int off)
{
    uint16_t le;

    memcpy(&le, entry + off, 2);
    return le16_to_cpu(le);
}

/*
 * Loads mailboxes 1, 2, 3, 6 and 7 with a queue's entry count and 64-bit
 * base address split the way OpenBSD's own driver splits it — low half in
 * mailbox 3, next in mailbox 2, next in mailbox 7, top in mailbox 6 — then
 * runs the given init command and returns mailbox 0.
 */
static uint16_t isp_init_queue(QPCIDevice *dev, QPCIBar io, uint16_t cmd,
                               uint16_t count, uint64_t addr)
{
    qpci_io_writew(dev, io, ISP_MAILBOX(1), count);
    qpci_io_writew(dev, io, ISP_MAILBOX(2), (addr >> 16) & 0xffff);
    qpci_io_writew(dev, io, ISP_MAILBOX(3), addr & 0xffff);
    qpci_io_writew(dev, io, ISP_MAILBOX(6), (addr >> 48) & 0xffff);
    qpci_io_writew(dev, io, ISP_MAILBOX(7), (addr >> 32) & 0xffff);
    /*
     * Only mailbox 0 is loaded fresh here: this command's arguments span
     * mailboxes 1, 2, 3, 6 and 7, which the four-mailbox call helper would
     * overwrite with zero if used here instead.
     */
    qpci_io_writew(dev, io, ISP_MAILBOX(0), cmd);
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

    return qpci_io_readw(dev, io, ISP_MAILBOX(0));
}

/*
 * Initialising either queue with a zero address or a zero entry count is
 * rejected. No guest on this device's path sends either — the check exists
 * because 86Box's model makes the same one, evidence about the approach
 * worth keeping even where nothing here can watch it matter — so this pins
 * the one behaviour a qtest can actually exercise: the model does not
 * silently accept a queue it was not actually given.
 */
static void test_init_queue_rejects_zero(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   0, 0x1000), ==,
                    ISP_MBOX_CMD_PARAM_ERROR);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   4, 0), ==,
                    ISP_MBOX_CMD_PARAM_ERROR);

    qlogic_isp_ack(dev, io);
    qpci_iounmap(dev, io);
}

/*
 * The steady-state contract in full: with both queues initialised, a
 * command entry written at the request-in index and the pointer advanced
 * produces a status entry in the response queue carrying the same handle,
 * the response-in pointer advanced, and an interrupt raised with the
 * semaphore lock clear — the datasheet's own words for what this test
 * checks, because an I/O completion is not a mailbox result and must not be
 * decoded as one.
 *
 * A test-unit-ready command is used because it moves no data, which keeps
 * this test to the ring protocol itself rather than also exercising the
 * scatter-gather path — that is worth its own test once this one is
 * trusted, not folded into the first one to pass.
 */
static void test_command_round_trip(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t rqst_addr, resp_addr;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    const uint32_t handle = 0x1234abcd;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    qts = dev->bus->qts;

    rqst_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   4, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   4, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;    /* entry_count: no continuations needed. */
    isp_put32(entry, ISP_REQ_OFF_HANDLE, handle);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);    /* bus 0, target 0, lun 0. */
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);
    /* Test unit ready: an all-zero six-byte CDB. */

    /*
     * The first command to a freshly attached SCSI device answers a pending
     * power-on unit attention rather than the command itself — this is real
     * generic-SCSI-layer behaviour, not this device's protocol, and
     * virtio-scsi's own qtest suite documents and relies on the same thing.
     * A round trip that only sent one command and asserted good status
     * would not tell the difference between a model correctly relaying
     * whatever the SCSI layer said and one that invented a fixed answer;
     * asserting the specific condition here, and a clean status on the
     * second command below, is what tells them apart.
     */
    qtest_memwrite(qts, rqst_addr, entry, sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 1);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 1);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_SEMAPHORE), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_STATUS), ==,
                    ISP_BUS_STATUS_INT_REQ | ISP_BUS_STATUS_RISC_INT_REQ);

    qtest_memread(qts, resp_addr, status, sizeof(status));
    g_assert_cmphex(status[0], ==, ISP_ENTRY_TYPE_STATUS);
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, handle);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_SCSI_STATUS), ==,
                    CHECK_CONDITION | ISP_SCSI_STATUS_SENSE_VALID);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    g_assert_cmphex(status[ISP_STATUS_OFF_SENSE_DATA + 2] & 0x0f, ==,
                    UNIT_ATTENTION);

    qlogic_isp_ack(dev, io);

    /*
     * Second command: the unit attention is clear, so this one is clean.
     * This device consumed the first entry, so it expects the second at the
     * next ring slot, not the one it already read.
     */
    qtest_memwrite(qts, rqst_addr + ISP_QUEUE_ENTRY_SIZE, entry,
                   sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 2);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 2);

    qtest_memread(qts, resp_addr + ISP_QUEUE_ENTRY_SIZE, status,
                 sizeof(status));
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, handle);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_SCSI_STATUS), ==, GOOD);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);

    qlogic_isp_ack(dev, io);
    qpci_iounmap(dev, io);
}

static void qlogic_isp_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "addr=04.0,id=scsi0",
        .before_cmd_line = "-drive id=drv0,if=none,file=null-co://,"
                           "file.read-zeroes=on,format=raw",
        .after_cmd_line = "-device scsi-hd,bus=scsi0.0,drive=drv0",
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
    qos_add_test("mailbox-nop", "qlogic-isp1040", test_mailbox_nop, NULL);
    qos_add_test("mailbox-invalid-command", "qlogic-isp1040",
                 test_mailbox_invalid_command, NULL);
    qos_add_test("firmware-download", "qlogic-isp1040",
                 test_firmware_download, NULL);
    qos_add_test("verify-checksum-failure", "qlogic-isp1040",
                 test_verify_checksum_failure, NULL);
    qos_add_test("about-firmware", "qlogic-isp1040",
                 test_about_firmware, NULL);
    qos_add_test("get-firmware-status", "qlogic-isp1040",
                 test_get_firmware_status, NULL);
    qos_add_test("init-queue-rejects-zero", "qlogic-isp1040",
                 test_init_queue_rejects_zero, NULL);
    qos_add_test("command-round-trip", "qlogic-isp1040",
                 test_command_round_trip, NULL);
}

libqos_init(qlogic_isp_register_nodes);
