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

#define ISP_REG_DMA_CMD_CFG   0x20
#define ISP_REG_DMA_DATA_CFG  0x40
#define ISP_DMA_CFG_BURST_ENABLE  0x0002

#define ISP_BUS_CTRL_SOFT_RESET     0x0001
#define ISP_BUS_CTRL_INT_ENABLE     0x0002
#define ISP_BUS_CTRL_RISC_INT_ENABLE 0x0004

#define ISP_SEMAPHORE_LOCK      0x0001
#define ISP_SEMAPHORE_STATUS    0x0002

#define ISP_REG_NVRAM        0x0e
#define ISP_NVRAM_CLOCK      0x0001
#define ISP_NVRAM_CHIP_SEL   0x0002
#define ISP_NVRAM_DATA_OUT   0x0004
#define ISP_NVRAM_DATA_IN    0x0008
#define ISP_NVRAM_CMD_READ   6

#define ISP_REG_MAILBOX_BASE    0x70
#define ISP_MAILBOX_COUNT       8
#define ISP_MAILBOX(n)          (ISP_REG_MAILBOX_BASE + (n) * 2)

/* The queue pointers are these two mailboxes under a second name. */
#define ISP_REG_REQUEST_QUEUE   (ISP_REG_MAILBOX_BASE + 0x08)
#define ISP_REG_RESPONSE_QUEUE  (ISP_REG_MAILBOX_BASE + 0x0a)

#define ISP_MAILBOX1_SIGNATURE  0x4953
#define ISP_MAILBOX2_SIGNATURE  0x5020
#define ISP_MAILBOX3_SIGNATURE  0x2020
#define ISP_MAILBOX4_SIGNATURE  0x0001

#define ISP_MBOX_CMD_NOP                 0x0000
#define ISP_MBOX_CMD_EXEC_FIRMWARE       0x0002
#define ISP_MBOX_CMD_WRITE_RAM_WORD      0x0004
#define ISP_MBOX_CMD_VERIFY_CHECKSUM     0x0007
#define ISP_MBOX_CMD_ABOUT_FIRMWARE      0x0008
#define ISP_MBOX_CMD_GET_FIRMWARE_STATUS 0x001f

#define ISP_MBOX_CMD_SET_INITIATOR_ID      0x0030
#define ISP_MBOX_CMD_SET_SELECTION_TIMEOUT 0x0031
#define ISP_MBOX_CMD_SET_RETRY_COUNT       0x0032
#define ISP_MBOX_CMD_SET_TAG_AGE_LIMIT      0x0033
#define ISP_MBOX_CMD_SET_CLOCK_RATE         0x0034
#define ISP_MBOX_CMD_SET_ACTIVE_NEGATION    0x0035
#define ISP_MBOX_CMD_SET_ASYNC_DATA_SETUP   0x0036
#define ISP_MBOX_CMD_SET_PCI_CONTROL        0x0037
#define ISP_MBOX_CMD_SET_TARGET_PARAMETERS  0x0038
#define ISP_MBOX_CMD_SET_DEVICE_QUEUE       0x0039
#define ISP_MBOX_CMD_SET_SYSTEM_PARAMETER   0x0045
#define ISP_MBOX_CMD_SET_FIRMWARE_FEATURES  0x004a
#define ISP_MBOX_CMD_SET_DATA_OVERRUN_RECOVERY 0x005a

#define ISP_MBOX_COMPLETE       0x4000
#define ISP_MBOX_INVALID        0x4001
#define ISP_MBOX_TEST_FAILED    0x4003

#define ISP_FIRMWARE_STATUS_MAX_REQUESTS 512

#define ISP_MBOX_CMD_INIT_REQ_QUEUE  0x0010
#define ISP_MBOX_CMD_INIT_RSP_QUEUE  0x0011
#define ISP_MBOX_CMD_PARAM_ERROR     0x4006

#define ISP_QUEUE_ENTRY_SIZE     64

#define ISP_ENTRY_TYPE_REQUEST      0x01
#define ISP_ENTRY_TYPE_CONTINUATION 0x02
#define ISP_ENTRY_TYPE_STATUS       0x03

#define ISP_CONT_OFF_SEGS 8
#define ISP_CONT_SEGS     7

#define ISP_REQ_OFF_HANDLE       4
#define ISP_REQ_OFF_DEVICE       8
#define ISP_REQ_OFF_CDBLEN       10
#define ISP_REQ_OFF_TIMEOUT      16
#define ISP_REQ_OFF_SEGCOUNT     18
#define ISP_REQ_OFF_CDB          20
#define ISP_REQ_OFF_SEGS         32
#define ISP_SEG_SIZE             8

#define ISP_STATUS_OFF_HANDLE       4
#define ISP_STATUS_OFF_SCSI_STATUS  8
#define ISP_STATUS_OFF_COMPLETION   10
#define ISP_STATUS_OFF_STATE_FLAGS  12
#define ISP_STATUS_OFF_SENSE_LEN    18
#define ISP_STATUS_OFF_SENSE_DATA   32

#define ISP_IOCB_COMPLETE    0x0000
#define ISP_IOCB_INCOMPLETE  0x0001

#define ISP_STATE_FLAG_GOT_BUS            0x0100
#define ISP_STATE_FLAGS_ALL_PHASES        0x7f00

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
 * reset from one that merely decodes and answers. These four cannot read zero
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
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(4)), ==,
                    ISP_MAILBOX4_SIGNATURE);

    /* The mailboxes the signature does not occupy reset to zero. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==, 0);
    for (i = 5; i < ISP_MAILBOX_COUNT; i++) {
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
 * Linux's qla1280_chip_diag() runs its own product-ID check straight off
 * these same four mailboxes, before it issues any mailbox command, and
 * aborts the adapter's attach outright on a mismatch -- unlike OpenBSD,
 * which never performs this check at all. Reproduces the exact
 * PROD_ID_1..PROD_ID_4 comparison from drivers/scsi/qla1280.h (0x4953,
 * 0x0000 or 0x5020, 0x2020, 0x0001) so a regression here fails as
 * concretely as the driver's own "Wrong product ID" abort would.
 */
static void test_linux_product_id_check(void *obj, void *data,
                                        QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    uint16_t mbox1, mbox2, mbox3, mbox4;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    mbox1 = qpci_io_readw(dev, io, ISP_MAILBOX(1));
    mbox2 = qpci_io_readw(dev, io, ISP_MAILBOX(2));
    mbox3 = qpci_io_readw(dev, io, ISP_MAILBOX(3));
    mbox4 = qpci_io_readw(dev, io, ISP_MAILBOX(4));

    g_assert_cmphex(mbox1, ==, 0x4953);
    g_assert_true(mbox2 == 0x0000 || mbox2 == 0x5020);
    g_assert_cmphex(mbox3, ==, 0x2020);
    g_assert_cmphex(mbox4, ==, 0x0001);

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
 * The command and data DMA blocks' config registers, at the base of each
 * block. Only the burst-enable bit is on any boot path this device targets;
 * the two blocks are independent storage, not two names for one value, and
 * the read-modify-write OpenBSD's own attach sequence performs must see
 * back whatever it just wrote rather than the unimplemented-register
 * default of zero.
 */
static void test_dma_block_cfg(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    /* Resets to zero. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_DMA_CMD_CFG), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_DMA_DATA_CFG), ==, 0);

    /* The exact read-modify-write OpenBSD performs on each block. */
    qpci_io_writew(dev, io, ISP_REG_DMA_CMD_CFG,
                   qpci_io_readw(dev, io, ISP_REG_DMA_CMD_CFG) |
                   ISP_DMA_CFG_BURST_ENABLE);
    qpci_io_writew(dev, io, ISP_REG_DMA_DATA_CFG,
                   qpci_io_readw(dev, io, ISP_REG_DMA_DATA_CFG) |
                   ISP_DMA_CFG_BURST_ENABLE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_DMA_CMD_CFG), ==,
                    ISP_DMA_CFG_BURST_ENABLE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_DMA_DATA_CFG), ==,
                    ISP_DMA_CFG_BURST_ENABLE);

    /* The two blocks are independent: clearing one leaves the other set. */
    qpci_io_writew(dev, io, ISP_REG_DMA_CMD_CFG, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_DMA_CMD_CFG), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_DMA_DATA_CFG), ==,
                    ISP_DMA_CFG_BURST_ENABLE);

    /* Reserved bits read back zero rather than whatever was written. */
    qpci_io_writew(dev, io, ISP_REG_DMA_DATA_CFG, 0xffff);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_DMA_DATA_CFG), ==,
                    ISP_DMA_CFG_BURST_ENABLE);

    qpci_io_writew(dev, io, ISP_REG_DMA_DATA_CFG, 0);
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
 * Each mailbox is its own register. Distinct values are written to six of
 * the eight and read back afterwards rather than one at a time, so a model
 * decoding them to one shared register, or dropping the low address bits,
 * fails here instead of passing six independent round-trips.
 *
 * Mailboxes 4 and 5 are excluded: both are queue pointers under a second
 * name (request-in and response-in respectively), and neither is plain
 * storage — a read always answers this adapter's own position in that
 * direction, never whatever was last written here, by the datasheet's own
 * in-and-out-share-one-address requirement for both queues. Mailbox 5 has
 * been excluded since the response-queue completion-erasure fix; mailbox 4
 * joined it once the adversarial review (task #74) found the same defect
 * still open on the request queue. test_queue_pointers covers both
 * registers' actual contract; a plain write-then-read-back here would
 * assert the bug this device used to have.
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
        if (i == 4 || i == 5) {
            continue;
        }
        qpci_io_writew(dev, io, ISP_MAILBOX(i), 0x1000 + i);
    }
    for (i = 0; i < ISP_MAILBOX_COUNT; i++) {
        if (i == 4 || i == 5) {
            continue;
        }
        g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(i)), ==,
                        0x1000 + i);
    }

    qpci_io_writew(dev, io, ISP_REG_BUS_CTRL, ISP_BUS_CTRL_SOFT_RESET);
    qpci_iounmap(dev, io);
}

/*
 * The queue pointers are mailboxes 4 and 5 reached under a second name, and
 * both now share the identical in-and-out-share-one-address shape: a write
 * is the guest's producer advance, and a read always answers this adapter's
 * own consumer position, never whatever the guest last wrote. Response-in
 * had this from the completion-erasure fix; request-in did not, until the
 * adversarial review (task #74) caught it as the same defect on the other
 * queue -- previously it fell through to plain storage on read, so a guest
 * recomputing free request-ring space (Linux's qla1280_start_scsi() does
 * exactly this) read back its own last write and learned nothing real.
 *
 * Request-in resets to ISP_MAILBOX4_SIGNATURE (0x0001), not zero -- the same
 * value Linux's product-ID check requires there, and the same value 86Box's
 * ql_reset_asic sets at raw reset. Both OpenBSD and Linux still work with
 * this: OpenBSD's qlw_softreset() explicitly writes zero to this register as
 * one of its own reset steps, before ever treating it as a live pointer, so
 * it never depends on the hardware's own post-reset value either way. This
 * datasheet previously claimed the queue pointers reset to zero at this
 * offset; that claim was never checked against a source that observes the
 * raw hardware value rather than a driver's own subsequent write, and it was
 * wrong for this one, the same class of mistake as the mailbox-signature
 * misattribution corrected alongside it.
 *
 * With no request queue configured (rqst_len == 0 throughout this test), a
 * write to request-in still updates the guest-producer storage internally
 * (qlogic_isp_process_requests() reads it once a real queue exists), but
 * qlogic_isp_process_requests() itself no-ops on an unconfigured ring, so
 * this adapter's own consumer position never advances. A read answering the
 * write back would be indistinguishable from a read answering rqst_cons
 * once a real ring is configured and fully drains every write synchronously
 * -- the two coincide there, which is exactly why this bug stayed masked --
 * so an unconfigured ring is the one place in this file that can show a read
 * genuinely sourcing from rqst_cons and not from an echo of the write.
 *
 * Response-in (mailbox 5) is not plain storage. A read always answers the
 * adapter's own producer count — still 0 here, since nothing has posted a
 * completion — never whatever the guest just wrote to that address; the
 * datasheet is explicit that letting a guest's write overwrite the
 * adapter's published count is the bug this device used to have. The
 * guest's write is recorded as its consumer position instead, which this
 * test has no completion to make observable; that half of the contract is
 * test_ack_does_not_erase_a_later_completion's job.
 */
static void test_queue_pointers(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_REQUEST_QUEUE), ==,
                    ISP_MAILBOX4_SIGNATURE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 0);

    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 0x0021);
    qpci_io_writew(dev, io, ISP_REG_RESPONSE_QUEUE, 0x0043);

    /* Neither read reflects the write just sent to that same address. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_REQUEST_QUEUE), ==,
                    ISP_MAILBOX4_SIGNATURE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 0);

    /* The mailbox name is the same address, so it reads the same answer. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(4)), ==,
                    ISP_MAILBOX4_SIGNATURE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(5)), ==, 0);

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

/*
 * The "config setters" the datasheet names in bring-up order: firmware
 * features, clock rate, retry count, async data setup, active negation, tag
 * age limit, selection timeout, then initiator ID and the per-target
 * parameter and queue-depth commands. OpenBSD's qlw_attach() aborts the
 * whole attach the instant any one of these answers invalid, before it ever
 * creates the SCSI bus or probes a target — a real boot against a build of
 * this model that had every earlier command but none of these attached the
 * adapter and then enumerated no disk at all, because the bus these commands
 * gate into existence was never created. None of them carries a value this
 * model reads back, so completion is the entire contract, the same as
 * bus-reset already relies on.
 */
static void test_config_setters(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_SET_FIRMWARE_FEATURES, 1, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_SET_CLOCK_RATE, 60, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_SET_RETRY_COUNT, 2, 1, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_SET_ASYNC_DATA_SETUP, 6, 6, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_SET_ACTIVE_NEGATION, 0, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_SET_TAG_AGE_LIMIT, 8, 8, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_SET_SELECTION_TIMEOUT,
                          250, 250, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_SET_INITIATOR_ID, 7, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_SET_TARGET_PARAMETERS,
                          0, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_SET_DEVICE_QUEUE, 0, 32, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    /*
     * Three more, issued only by Linux's qla1280_nvram_config() (confirmed
     * directly against qla1280.c) and never by OpenBSD's qlw.c (confirmed by
     * an exhaustive grep of the file, not a name search that merely failed
     * to find a call site). Unlike the group above, Linux's own caller
     * discards qla1280_nvram_config()'s returned status outright, so
     * whether this model answers complete or invalid here is not
     * load-bearing for either guest -- these are included anyway so the
     * model's behavior matches what a real adapter would answer to a
     * documented command, rather than logging every one of them as
     * unmodelled noise.
     */
    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_SET_PCI_CONTROL, 2, 2, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_SET_SYSTEM_PARAMETER, 0, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_SET_DATA_OVERRUN_RECOVERY,
                          2, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
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

/*
 * Every status entry this device posts now zeroes its whole 64-byte slot
 * before writing into it (task #74/#97's adversarial review), rather than
 * leaving whatever a previous occupant of that ring slot wrote at any
 * offset this function does not explicitly set. A two-entry response ring
 * proves it the way test_reset_signature proves reset ran: not by reading
 * a value back, since a value that happens to be right could equally mean
 * "correctly zeroed" or "never overwritten and already zero" -- by first
 * putting a real, non-zero value where a stale leftover would show up, and
 * then confirming a later write to the same slot removes it rather than
 * leaving it standing.
 *
 * Slot 0 gets a real sense length and real sense bytes first (test unit
 * ready's automatic power-on unit attention, the same mechanism
 * test_command_round_trip uses). Slot 1 gets a clean command. A third
 * command wraps back onto slot 0, reusing the slot that held real sense
 * data, and this is the assertion that matters: slot 0 now reports
 * sense_len zero and thirty-two zero sense bytes, not the unit attention
 * this same slot held two completions ago. Before the fix this only
 * happened to hold because sense_len was never written and unit attention
 * sense data happens to still be present at that offset either way; the
 * new field is what makes "zeroed" and "never written" distinguishable.
 */
static void test_status_entry_clears_stale_bytes(void *obj, void *data,
                                                  QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t rqst_addr, resp_addr;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    int i;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    qts = dev->bus->qts;

    rqst_addr = guest_alloc(alloc, 2 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 2 * ISP_QUEUE_ENTRY_SIZE);

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   2, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   2, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x1);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);

    /* Command 1, into slot 0: the automatic power-on unit attention. */
    qtest_memwrite(qts, rqst_addr, entry, sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 1);

    qtest_memread(qts, resp_addr, status, sizeof(status));
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_SCSI_STATUS), ==,
                    CHECK_CONDITION | ISP_SCSI_STATUS_SENSE_VALID);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_STATE_FLAGS), ==,
                    ISP_STATE_FLAGS_ALL_PHASES);
    g_assert_cmpint(isp_get16(status, ISP_STATUS_OFF_SENSE_LEN), >, 0);
    g_assert_cmphex(status[ISP_STATUS_OFF_SENSE_DATA + 2] & 0x0f, ==,
                    UNIT_ATTENTION);

    qlogic_isp_ack(dev, io);
    qpci_io_writew(dev, io, ISP_REG_RESPONSE_QUEUE,
                   qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE));

    /* Command 2, into slot 1: clean, no sense. Slot 0 is untouched so far. */
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x2);
    qtest_memwrite(qts, rqst_addr + ISP_QUEUE_ENTRY_SIZE, entry,
                   sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 2);

    qtest_memread(qts, resp_addr + ISP_QUEUE_ENTRY_SIZE, status,
                 sizeof(status));
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_SCSI_STATUS), ==, GOOD);

    qlogic_isp_ack(dev, io);
    qpci_io_writew(dev, io, ISP_REG_RESPONSE_QUEUE,
                   qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE));

    /*
     * Command 3 wraps the two-entry ring back onto slot 0 -- the slot
     * command 1's unit attention sense data occupied. Clean again, so if
     * anything from command 1 survives here it is stale, not this
     * command's own answer.
     */
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x3);
    qtest_memwrite(qts, rqst_addr, entry, sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 3);

    qtest_memread(qts, resp_addr, status, sizeof(status));
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, 0x3);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_SCSI_STATUS), ==, GOOD);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_STATE_FLAGS), ==,
                    ISP_STATE_FLAGS_ALL_PHASES);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_SENSE_LEN), ==, 0);
    for (i = 0; i < 32; i++) {
        g_assert_cmphex(status[ISP_STATUS_OFF_SENSE_DATA + i], ==, 0);
    }

    qlogic_isp_ack(dev, io);
    qpci_iounmap(dev, io);
}

/*
 * Linux's qla1280_return_status() reads state_flags only when the
 * completion code is incomplete, walking GOT_BUS, GOT_TARGET, SENT_CDB,
 * TRANSFERRED_DATA, GOT_STATUS and GOT_SENSE in order to decide which of
 * several specific failure reasons to report -- not just that something
 * failed. This device's one incomplete completion is "no SCSI device at
 * the requested target/lun", and the state_flags it reports has to say
 * exactly that: the bus was reached, nothing past it was.
 */
static void test_no_device_incomplete_state_flags(void *obj, void *data,
                                                   QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t rqst_addr, resp_addr;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    qts = dev->bus->qts;

    /*
     * Two entries, not one: the request-in pointer wraps modulo the queue
     * length (qlogic_isp_process_requests()), and a one-entry queue can
     * never tell "one new entry has arrived" apart from "nothing has" --
     * writing 1 wraps straight back to the same 0 the ring already sits at,
     * so the loop that dispatches new entries never runs at all. The same
     * reserve-one-slot shape the response ring's own contract already
     * documents, hit here from the request side by writing this test with
     * too small a ring the first time.
     */
    rqst_addr = guest_alloc(alloc, 2 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 2 * ISP_QUEUE_ENTRY_SIZE);

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   2, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   2, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x9);
    /* Target 5, lun 0: nothing this qos-graph node attaches there. */
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 5 << 8);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);

    qtest_memwrite(qts, rqst_addr, entry, sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 1);

    qtest_memread(qts, resp_addr, status, sizeof(status));
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, 0x9);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_INCOMPLETE);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_STATE_FLAGS), ==,
                    ISP_STATE_FLAG_GOT_BUS);

    qlogic_isp_ack(dev, io);
    qpci_iounmap(dev, io);
}

/*
 * Linux's 64-bit command entry (qla1280.h's cmd_a64_entry_t, entry type 9)
 * is a request-queue entry type this model does not implement -- the
 * live-confirmed failure from adversarial review task #74/#97 finding
 * #2/#4: a real Linux/IP27 boot posts one of these and then waits forever
 * for a completion this adapter used to silently never send, because the
 * request-processing loop's default case consumed the entry and moved on
 * without answering it. This test does not exercise the fix that would
 * make a 64-bit command actually work -- no data moves and no SCSI
 * command reaches a target -- it exercises the guarantee that the guest
 * is told INCOMPLETE for the handle it is actually waiting on, using the
 * offset every entry type carrying a handle places it at (confirmed
 * against cmd_a64_entry_t: offset 4, identical to the 32-bit request
 * entry this model implements). state_flags is zero: no phase of the
 * command was understood well enough to claim otherwise.
 */
#define ISP_ENTRY_TYPE_COMMAND_A64 0x09
#define ISP_ENTRY_TYPE_CONTINUE_A64 0x0a

/*
 * cont_a64_entry (qla1280.h) is the other live-confirmed entry type from the
 * same finding, and it does not carry a handle at all: its offset 4 is
 * dseg_0_address's low 32 bits -- the start of a DMA buffer address, a
 * completely different meaning from ISP_REQ_OFF_HANDLE at the same offset in
 * every entry type that does carry one. Answering it as though it were a
 * handle would be strictly worse than the original silent-hang bug: it would
 * complete an arbitrary in-flight command (whichever handle the address
 * happens to collide with) instead of the one actually waiting. This offset
 * is named separately from ISP_REQ_OFF_HANDLE, even though both are 4, so a
 * reader sees the two are the same number for unrelated reasons rather than
 * one constant doing double duty.
 */
#define ISP_CONT_A64_OFF_DSEG0_LOW 4

static void test_unrecognized_entry_type_incomplete(void *obj, void *data,
                                                     QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t rqst_addr, resp_addr;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    qts = dev->bus->qts;

    /*
     * Two entries, not one, for the same reason
     * test_no_device_incomplete_state_flags needs them: the request-in
     * pointer wraps modulo the queue length, and a one-entry queue can
     * never tell "one new entry has arrived" apart from "nothing has".
     */
    rqst_addr = guest_alloc(alloc, 2 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 2 * ISP_QUEUE_ENTRY_SIZE);

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   2, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   2, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_COMMAND_A64;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x64);

    qtest_memwrite(qts, rqst_addr, entry, sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 1);

    qtest_memread(qts, resp_addr, status, sizeof(status));
    g_assert_cmphex(status[0], ==, ISP_ENTRY_TYPE_STATUS);
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, 0x64);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_INCOMPLETE);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_STATE_FLAGS), ==, 0);

    qlogic_isp_ack(dev, io);
    qpci_iounmap(dev, io);
}

/*
 * fabric-dev's review of the fix above (task #106) confirmed it is correct
 * only because of something nothing tested: qlogic_isp_process_requests()
 * advances rqst_cons by the dispatched entry's own entry_count, so a
 * COMMAND_A64_TYPE entry with a CONTINUE_A64_TYPE entry in the next slot
 * has its continuation consumed as part of the same dispatch -- the
 * continuation never reaches the default case as an entry of its own. If
 * that advance ever regressed to a plain +1, the continuation WOULD reach
 * the default case next, and this model would read
 * ISP_CONT_A64_OFF_DSEG0_LOW as a handle: the low 32 bits of a DMA buffer
 * address, misread as the identifier of whichever in-flight command that
 * value happens to collide with. That is a silent, spurious I/O failure
 * on an unrelated healthy command -- worse than the hang this fix
 * replaced, because nothing would name the cause.
 *
 * This test is the one that would fail if that regression happened:
 * exactly one completion posted, carrying the command's own handle, not
 * two, and not the continuation's DMA address.
 *
 * "Exactly one" is asserted two ways, not just via the response-queue
 * producer count: that count catches a second post through modular
 * arithmetic (two posts into a two-entry ring leave the producer back at
 * 0, and 0 happens not to equal 1), which stops working the moment the
 * ring size changes. Poisoning the second response slot before the
 * dispatch and reading it back afterward is the direct property this
 * test actually means: that slot was never written at all.
 */
static void test_unrecognized_entry_type_consumes_continuation(void *obj,
                                                                void *data,
                                                                QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t rqst_addr, resp_addr;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t cont[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t poison[ISP_QUEUE_ENTRY_SIZE];

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    qts = dev->bus->qts;

    /*
     * The request ring is 3 entries, not 2: this command occupies both of a
     * 2-entry ring's slots, so writing producer=2 would wrap modulo 2 back
     * to 0 -- indistinguishable from rqst_cons, which also starts at 0, so
     * the dispatch loop would never run at all. A 3-entry ring makes
     * producer=2 distinct from cons=0, the same reserve-past-the-command
     * shape every other test here already uses for a one-entry command.
     * The response ring stays at 2: only one completion is ever expected,
     * and the second slot exists solely as the poisoned witness below.
     */
    rqst_addr = guest_alloc(alloc, 3 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 2 * ISP_QUEUE_ENTRY_SIZE);

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   3, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   2, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    /* Poison the second response slot so an unwanted write to it is visible. */
    memset(poison, 0xaa, sizeof(poison));
    qtest_memwrite(qts, resp_addr + ISP_QUEUE_ENTRY_SIZE, poison,
                   sizeof(poison));

    /* Slot 0: the command, entry_count 2 -- this entry plus one continuation. */
    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_COMMAND_A64;
    entry[1] = 2;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x64);
    qtest_memwrite(qts, rqst_addr, entry, sizeof(entry));

    /*
     * Slot 1: the continuation. A DMA buffer address at offset 4, far from
     * the command's real handle (0x64), so a wrong-handle regression fails
     * loudly rather than passing by coincidence.
     */
    memset(cont, 0, sizeof(cont));
    cont[0] = ISP_ENTRY_TYPE_CONTINUE_A64;
    cont[1] = 1;
    isp_put32(cont, ISP_CONT_A64_OFF_DSEG0_LOW, 0xdeadbeef);
    qtest_memwrite(qts, rqst_addr + ISP_QUEUE_ENTRY_SIZE, cont, sizeof(cont));

    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 2);

    /* Exactly one completion: the continuation must not dispatch on its own. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 1);

    qtest_memread(qts, resp_addr, status, sizeof(status));
    g_assert_cmphex(status[0], ==, ISP_ENTRY_TYPE_STATUS);
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, 0x64);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_INCOMPLETE);

    /* The direct property: the second slot was never touched. */
    qtest_memread(qts, resp_addr + ISP_QUEUE_ENTRY_SIZE, status,
                 sizeof(status));
    g_assert_cmpint(memcmp(status, poison, sizeof(poison)), ==, 0);

    qlogic_isp_ack(dev, io);
    qpci_iounmap(dev, io);
}

/*
 * The round trip above only ever sends test unit ready, which carries no
 * data phase at all — it is not what actually exercises a scatter-gather
 * transfer, and a real driver's target probe issues something that does on
 * the very first command: inquiry. This model shipped with no scatter-
 * gather list handed to the SCSI layer, so any command that reached
 * scsi_req_data() found a null bus->info->transfer_data and crashed the
 * emulator itself — invisible to every qtest above, because none of them
 * carried data. A real OpenBSD boot found it the only way it could: it
 * reached "scsibus1 at qlw0: 16 targets" and crashed on the very next
 * command. This test sends an inquiry with a real data segment and reads
 * the answer back out of guest memory, which is what actually exercises
 * the path that crashed.
 */
static void test_inquiry_data_transfer(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t rqst_addr, resp_addr, data_addr;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t inquiry[36];

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    qts = dev->bus->qts;

    rqst_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);
    data_addr = guest_alloc(alloc, sizeof(inquiry));

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   4, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   4, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    /* First command absorbs the power-on unit attention, same as above. */
    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x1);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);
    qtest_memwrite(qts, rqst_addr, entry, sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 1);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 1);
    qlogic_isp_ack(dev, io);

    /* Second command: a real inquiry with a data-in segment. */
    memset(inquiry, 0xff, sizeof(inquiry));
    qtest_memwrite(qts, data_addr, inquiry, sizeof(inquiry));

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x2);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 1);
    entry[ISP_REQ_OFF_CDB + 0] = INQUIRY;
    entry[ISP_REQ_OFF_CDB + 4] = sizeof(inquiry);    /* allocation length */
    isp_put32(entry, ISP_REQ_OFF_SEGS + 0, (uint32_t)data_addr);
    isp_put32(entry, ISP_REQ_OFF_SEGS + 4, sizeof(inquiry));

    qtest_memwrite(qts, rqst_addr + ISP_QUEUE_ENTRY_SIZE, entry,
                   sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 2);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 2);

    qtest_memread(qts, resp_addr + ISP_QUEUE_ENTRY_SIZE, status,
                 sizeof(status));
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, 0x2);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_SCSI_STATUS), ==, GOOD);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    qlogic_isp_ack(dev, io);

    /*
     * The buffer started as all-0xff; only a real DMA from the model can
     * have put a device-type byte and the disk's vendor/product strings
     * here. This is the assertion that would have failed loudly instead of
     * segfaulting the emulator, had it existed before today.
     */
    qtest_memread(qts, data_addr, inquiry, sizeof(inquiry));
    g_assert_cmphex(inquiry[0], ==, 0x00);
    g_assert_cmpint(memcmp(&inquiry[8], "QEMU    ", 8), ==, 0);
    g_assert_cmpint(memcmp(&inquiry[16], "QEMU HARDDISK   ", 16), ==, 0);

    qpci_iounmap(dev, io);
}

/*
 * Every command above fits its whole transfer in the four segments a
 * request entry carries inline. Nothing has ever forced a fifth segment
 * into a continuation entry — not real firmware download, not the earlier
 * inquiry, not either real OpenBSD boot, since none of them needed more
 * than four scatter-gather pieces. This is exactly the shape that produced
 * both real bugs this device has had: an entire code path
 * (qlogic_isp_read_segments' continuation loop) with no test anywhere
 * near it. Deliberately split a 36-byte inquiry into five pieces — four
 * inline segments that exactly fill the request entry, then one more in a
 * continuation entry — so the boundary itself is what gets exercised, not
 * just "some segments somewhere in a continuation."
 *
 * The five destination regions are not contiguous in guest memory, which
 * is deliberate: scatter-gather exists for exactly this, and a model that
 * happened to work only because a test's segments were adjacent would be
 * passing for the wrong reason.
 */
static void test_continuation_entry_segments(void *obj, void *data,
                                             QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t rqst_addr, resp_addr;
    uint64_t piece[5];
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t cont[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t inquiry[36];
    size_t i;
    const uint8_t piece_len[5] = { 8, 8, 8, 8, 4 };

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    qts = dev->bus->qts;

    rqst_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);
    for (i = 0; i < 5; i++) {
        piece[i] = guest_alloc(alloc, 64);   /* deliberately not adjacent */
    }

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   4, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   4, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    /* Slot 0: absorb the power-on unit attention, same as every test above. */
    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x1);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);
    qtest_memwrite(qts, rqst_addr, entry, sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 1);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 1);
    qlogic_isp_ack(dev, io);

    /*
     * Slots 1 and 2: the inquiry itself, five segments, two ring entries.
     * Poison every destination first so a segment the model never writes
     * shows up as 0xff instead of the zeroed guest_alloc default, which
     * would silently pass either way.
     */
    for (i = 0; i < 5; i++) {
        uint8_t poison[64];
        memset(poison, 0xff, sizeof(poison));
        qtest_memwrite(qts, piece[i], poison, sizeof(poison));
    }

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 2;    /* this entry plus one continuation. */
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x2);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 5);
    entry[ISP_REQ_OFF_CDB + 0] = INQUIRY;
    entry[ISP_REQ_OFF_CDB + 4] = sizeof(inquiry);
    for (i = 0; i < 4; i++) {
        isp_put32(entry, ISP_REQ_OFF_SEGS + i * ISP_SEG_SIZE,
                  (uint32_t)piece[i]);
        isp_put32(entry, ISP_REQ_OFF_SEGS + i * ISP_SEG_SIZE + 4,
                  piece_len[i]);
    }
    qtest_memwrite(qts, rqst_addr + ISP_QUEUE_ENTRY_SIZE, entry,
                   sizeof(entry));

    memset(cont, 0, sizeof(cont));
    cont[0] = ISP_ENTRY_TYPE_CONTINUATION;
    cont[1] = 1;
    isp_put32(cont, ISP_CONT_OFF_SEGS, (uint32_t)piece[4]);
    isp_put32(cont, ISP_CONT_OFF_SEGS + 4, piece_len[4]);
    qtest_memwrite(qts, rqst_addr + 2 * ISP_QUEUE_ENTRY_SIZE, cont,
                   sizeof(cont));

    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 3);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 2);

    qtest_memread(qts, resp_addr + ISP_QUEUE_ENTRY_SIZE, status,
                 sizeof(status));
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, 0x2);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_SCSI_STATUS), ==, GOOD);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
    qlogic_isp_ack(dev, io);

    /* Reassemble the five DMA'd pieces and check them like one buffer. */
    for (i = 0; i < 5; i++) {
        qtest_memread(qts, piece[i], inquiry + (i < 4 ? i * 8 : 32),
                     piece_len[i]);
    }
    g_assert_cmphex(inquiry[0], ==, 0x00);
    g_assert_cmpint(memcmp(&inquiry[8], "QEMU    ", 8), ==, 0);
    g_assert_cmpint(memcmp(&inquiry[16], "QEMU HARDDISK   ", 16), ==, 0);

    /*
     * Slot 3: the ring walker must have advanced rqst_cons by this
     * command's entry_count (2), not by 1 — otherwise it would now be
     * reading the continuation entry as if it were a fresh top-level
     * request, and this command would never see its response. A plain
     * test unit ready here is the check: it only gets a clean answer if
     * the cursor already skipped past both of the previous command's
     * slots correctly.
     */
    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x3);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);
    qtest_memwrite(qts, rqst_addr + 3 * ISP_QUEUE_ENTRY_SIZE, entry,
                   sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 4);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 3);

    qtest_memread(qts, resp_addr + 2 * ISP_QUEUE_ENTRY_SIZE, status,
                 sizeof(status));
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, 0x3);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_SCSI_STATUS), ==, GOOD);
    qlogic_isp_ack(dev, io);

    qpci_iounmap(dev, io);
}

/*
 * Bit-bangs a read of the EEPROM's word 0 the way OpenBSD's driver does —
 * chip select and clock asserted, the read command and a word address
 * shifted out, then sixteen clocks reading data-in on each — and checks
 * every one of those sixteen bits comes back clear. That is what a guest
 * bit-banging any command or address ever gets from this register: nothing
 * here drives data-in, so the identifier bytes a real EEPROM would return
 * can never appear, and the guest's own validation is what turns that into
 * "nvram corrupt" and its built-in defaults. The register's own contract —
 * that the three bits it owns round-trip and the fourth never reads set —
 * is what this test actually pins; it does not need to reproduce the
 * guest's validation arithmetic to know that arithmetic will fail.
 */
static void test_nvram_read_fails(void *obj, void *data, QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    int bit;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    /* The three bits this register owns round-trip. */
    qpci_io_writew(dev, io, ISP_REG_NVRAM,
                   ISP_NVRAM_CLOCK | ISP_NVRAM_CHIP_SEL | ISP_NVRAM_DATA_OUT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_NVRAM), ==,
                    ISP_NVRAM_CLOCK | ISP_NVRAM_CHIP_SEL | ISP_NVRAM_DATA_OUT);

    /* Nothing ever asserts data-in, regardless of what else is written. */
    qpci_io_writew(dev, io, ISP_REG_NVRAM, 0xffff);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_NVRAM) & ISP_NVRAM_DATA_IN,
                    ==, 0);

    /* Assert chip select, then shift out the read command and an address. */
    qpci_io_writew(dev, io, ISP_REG_NVRAM, ISP_NVRAM_CHIP_SEL);
    for (bit = 8; bit >= 0; bit--) {
        uint16_t out = ISP_NVRAM_CHIP_SEL;

        if ((ISP_NVRAM_CMD_READ >> bit) & 1) {
            out |= ISP_NVRAM_DATA_OUT;
        }
        qpci_io_writew(dev, io, ISP_REG_NVRAM, out);
        qpci_io_writew(dev, io, ISP_REG_NVRAM, out | ISP_NVRAM_CLOCK);
        qpci_io_writew(dev, io, ISP_REG_NVRAM, out);
    }

    /* Sixteen more clocks, reading data-in each time: it never sets. */
    for (bit = 0; bit < 16; bit++) {
        qpci_io_writew(dev, io, ISP_REG_NVRAM,
                       ISP_NVRAM_CHIP_SEL | ISP_NVRAM_CLOCK);
        g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_NVRAM) &
                        ISP_NVRAM_DATA_IN, ==, 0);
        qpci_io_writew(dev, io, ISP_REG_NVRAM, ISP_NVRAM_CHIP_SEL);
    }

    qpci_io_writew(dev, io, ISP_REG_NVRAM, 0);
    qpci_iounmap(dev, io);
}

/*
 * Four completions with nothing acknowledged, on a four-entry response ring.
 *
 * This is the shape the adapter's own datasheet names: the driver issues each
 * command as its own doorbell, and a model with no bus latency can complete all
 * of them before the guest reads the response queue once. A ring that tracks no
 * acknowledged position and reserves no slot wraps its producer back onto the
 * consumer, and producer equal to consumer is how every driver spells *empty* —
 * so four posted completions become invisible and the guest sleeps.
 *
 * The request ring is given eight entries so that only the response side is
 * under test.
 */
static void test_four_completions_do_not_read_as_empty(void *obj, void *data,
                                                       QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t rqst_addr, resp_addr;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    int i;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    qts = dev->bus->qts;

    rqst_addr = guest_alloc(alloc, 8 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   8, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   4, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);

    for (i = 0; i < 4; i++) {
        isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x1000 + i);
        qtest_memwrite(qts, rqst_addr + i * ISP_QUEUE_ENTRY_SIZE,
                       entry, sizeof(entry));
    }

    /* One doorbell per command, which is how the driver issues them. */
    for (i = 0; i < 4; i++) {
        qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, i + 1);
    }

    /*
     * Nothing has been acknowledged, so four entries are outstanding and the
     * producer must not have come to rest on the consumer.
     */
    g_assert_cmpuint(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), !=, 0);
}

/*
 * The guest's acknowledgement must not erase a completion posted behind it.
 *
 * The response queue's in and out pointers share one address: the adapter
 * publishes its producer there and the driver writes its consumer there. The
 * driver reads the producer once at the top of its handler and writes that same
 * value back as its acknowledgement at the end, so any completion landing in
 * between is one the adapter published into storage the driver is about to
 * overwrite with an older number.
 *
 * The ring is given eight entries so that a wrap cannot be the cause; this is
 * about the shared address alone.
 */
static void test_ack_does_not_erase_a_later_completion(void *obj, void *data,
                                                       QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t rqst_addr, resp_addr;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint16_t first_read;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    qts = dev->bus->qts;

    rqst_addr = guest_alloc(alloc, 8 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 8 * ISP_QUEUE_ENTRY_SIZE);

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   8, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   8, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);

    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x2001);
    qtest_memwrite(qts, rqst_addr, entry, sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 1);

    /* The driver reads the producer once, at the top of its handler. */
    first_read = qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE);
    g_assert_cmpuint(first_read, ==, 1);

    /* A second completion lands while the handler is still running. */
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x2002);
    qtest_memwrite(qts, rqst_addr + ISP_QUEUE_ENTRY_SIZE, entry, sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 2);

    /* The driver finishes and acknowledges what it read: one entry. */
    qpci_io_writew(dev, io, ISP_REG_RESPONSE_QUEUE, first_read);

    /*
     * The adapter has two completions posted. The next read of the producer
     * must be the adapter's own count, not the number the driver just wrote.
     */
    g_assert_cmpuint(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 2);
}

/*
 * A completion held for lack of ring room must re-signal the guest once it
 * finally posts, not just update the adapter's internal state silently.
 *
 * Fills a four-entry response ring to its reserve-one-slot capacity with
 * three real completions, nothing acknowledged, so a fourth has nowhere to
 * post and is held. Clears the interrupt the way a real driver's handler
 * does before checking anything further, so the checks that follow mean
 * something rather than passing on bits a completion earlier in the test
 * already set. The held completion must stay silent — no producer
 * movement, no interrupt — until the guest's next acknowledgement frees a
 * slot; only then does it post, and posting must raise the interrupt
 * again. A retry that writes a correct status entry but leaves bus_status
 * untouched would be a completion the driver has no reason to go looking
 * for, indistinguishable from one that never finished.
 */
static void test_held_completion_resignals_on_drain(void *obj, void *data,
                                                     QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t rqst_addr, resp_addr;
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    int i;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    qts = dev->bus->qts;

    rqst_addr = guest_alloc(alloc, 8 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   8, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   4, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);

    /* Three real completions fill the ring to its reserve-one-slot limit;
     * nothing acknowledged, so resp_cons is still 0. */
    for (i = 0; i < 3; i++) {
        isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x3000 + i);
        qtest_memwrite(qts, rqst_addr + i * ISP_QUEUE_ENTRY_SIZE,
                       entry, sizeof(entry));
        qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, i + 1);
    }
    g_assert_cmpuint(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 3);

    /* Clear the interrupt without acknowledging any ring position, the way
     * a driver's handler clears RISC_INT_REQ before it is done reading. */
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_STATUS), ==, 0);

    /* A fourth command has nowhere to post: (resp_prod + 1) % resp_len
     * would land exactly on the still-untouched resp_cons. */
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x3003);
    qtest_memwrite(qts, rqst_addr + 3 * ISP_QUEUE_ENTRY_SIZE,
                   entry, sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 4);

    /* Held, not lost and not yet announced: the producer has not moved and
     * the interrupt this device raises on every real post has not fired. */
    g_assert_cmpuint(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 3);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_STATUS), ==, 0);

    /* The guest acknowledges one entry -- the only event that can free
     * room for the held completion to drain. */
    qpci_io_writew(dev, io, ISP_REG_RESPONSE_QUEUE, 1);

    /* The decisive check: the drained completion re-signals the guest. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_STATUS), ==,
                    ISP_BUS_STATUS_INT_REQ | ISP_BUS_STATUS_RISC_INT_REQ);
    g_assert_cmpuint(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 0);

    /* And its data actually landed, rather than the interrupt alone. */
    qtest_memread(qts, resp_addr + 3 * ISP_QUEUE_ENTRY_SIZE, status,
                 sizeof(status));
    g_assert_cmphex(status[0], ==, ISP_ENTRY_TYPE_STATUS);
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, 0x3003);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_COMPLETE);
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
    qos_add_test("linux-product-id-check", "qlogic-isp1040",
                 test_linux_product_id_check, NULL);
    qos_add_test("soft-reset", "qlogic-isp1040", test_soft_reset, NULL);
    qos_add_test("bus-cfg1", "qlogic-isp1040", test_bus_cfg1, NULL);
    qos_add_test("dma-block-cfg", "qlogic-isp1040", test_dma_block_cfg, NULL);
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
    qos_add_test("config-setters", "qlogic-isp1040",
                 test_config_setters, NULL);
    qos_add_test("init-queue-rejects-zero", "qlogic-isp1040",
                 test_init_queue_rejects_zero, NULL);
    qos_add_test("command-round-trip", "qlogic-isp1040",
                 test_command_round_trip, NULL);
    qos_add_test("status-entry-clears-stale-bytes", "qlogic-isp1040",
                 test_status_entry_clears_stale_bytes, NULL);
    qos_add_test("no-device-incomplete-state-flags", "qlogic-isp1040",
                 test_no_device_incomplete_state_flags, NULL);
    qos_add_test("unrecognized-entry-type-incomplete", "qlogic-isp1040",
                 test_unrecognized_entry_type_incomplete, NULL);
    qos_add_test("unrecognized-entry-type-consumes-continuation",
                 "qlogic-isp1040",
                 test_unrecognized_entry_type_consumes_continuation, NULL);
    qos_add_test("inquiry-data-transfer", "qlogic-isp1040",
                 test_inquiry_data_transfer, NULL);
    qos_add_test("continuation-entry-segments", "qlogic-isp1040",
                 test_continuation_entry_segments, NULL);
    qos_add_test("nvram-read-fails", "qlogic-isp1040",
                 test_nvram_read_fails, NULL);
    qos_add_test("four-completions-not-empty", "qlogic-isp1040",
                 test_four_completions_do_not_read_as_empty, NULL);
    qos_add_test("ack-does-not-erase-completion", "qlogic-isp1040",
                 test_ack_does_not_erase_a_later_completion, NULL);
    qos_add_test("held-completion-resignals-on-drain", "qlogic-isp1040",
                 test_held_completion_resignals_on_drain, NULL);
}

libqos_init(qlogic_isp_register_nodes);
