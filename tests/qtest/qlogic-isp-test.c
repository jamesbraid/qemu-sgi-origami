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
#define ISP_REG_DMA_CMD_CTRL  0x22
#define ISP_REG_DMA_DATA_CFG  0x40
#define ISP_REG_DMA_DATA_CTRL 0x42
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
#define ISP_MBOX_CMD_LOAD_RAM            0x0001
#define ISP_MBOX_CMD_EXEC_FIRMWARE       0x0002
#define ISP_MBOX_CMD_DUMP_RAM            0x0003
#define ISP_MBOX_CMD_WRITE_RAM_WORD      0x0004
#define ISP_MBOX_CMD_READ_RAM_WORD       0x0005
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

#define ISP_MBOX_CMD_MAILBOX_REG_TEST 0x0006
#define ISP_MBOX_CMD_INIT_REQ_QUEUE  0x0010
#define ISP_MBOX_CMD_INIT_RSP_QUEUE  0x0011
#define ISP_MBOX_CMD_BUS_RESET       0x0018
#define ISP_MBOX_CMD_PARAM_ERROR     0x4006

#define ISP_QUEUE_ENTRY_SIZE     64

#define ISP_ENTRY_TYPE_REQUEST      0x01
#define ISP_ENTRY_TYPE_CONTINUATION 0x02
#define ISP_ENTRY_TYPE_STATUS       0x03
#define ISP_ENTRY_TYPE_MARKER       0x04
/*
 * Documented but unused by every driver (qla1280.h): the stand-in for an
 * entry type this model does not implement.
 */
#define ISP_ENTRY_TYPE_EXTENDED_CMD 0x05

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

/*
 * The 64-bit addressing pair (datasheet's "64-bit addressing entries"
 * section): Linux/IP27's only request format. LUN and target are two
 * separate bytes, not one packed word; each segment is 12 bytes (a 64-bit
 * address as two 32-bit words, low half first, then a 32-bit length)
 * instead of 8; two inline segments instead of four; five per continuation
 * instead of seven.
 */
#define ISP_ENTRY_TYPE_COMMAND_A64  0x09
#define ISP_ENTRY_TYPE_CONTINUE_A64 0x0a

#define ISP_A64_REQ_OFF_LUN      8
#define ISP_A64_REQ_OFF_TARGET   9
#define ISP_A64_REQ_OFF_CDBLEN   10
#define ISP_A64_REQ_OFF_TIMEOUT  16
#define ISP_A64_REQ_OFF_SEGCOUNT 18
#define ISP_A64_REQ_OFF_CDB      20
#define ISP_A64_REQ_OFF_SEGS     40
#define ISP_A64_SEG_SIZE         12

#define ISP_A64_CONT_OFF_SEGS 4
#define ISP_A64_CONT_SEGS     5

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
 * The revision byte is functional: OpenBSD derives the adapter's 60 MHz
 * clock from it.
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
 * Both BARs reach the same window of 16-bit registers. The two identity
 * words prove the stride: adjacent offsets, different values, so a coarser
 * decode returns the same value from both.
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
 * Bus configuration 0's revision field runs one ahead of the PCI byte, so
 * the same part reports 5 there and 6 here; Linux reads this one. The
 * whole register is asserted -- the nibble alone passes with stray bits.
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
 * The signature is the observable that proves reset ran: every other
 * register resets to zero, which an uninitialised structure also holds.
 * It also pins the device to a parent that resets it.
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
 * Linux's qla1280_chip_diag() aborts the attach on any mismatch here,
 * before issuing a single mailbox command; the exact PROD_ID comparison
 * from qla1280.h.
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
 * The guest polls this bit to zero and fails the attach otherwise. State
 * is dirtied first, so a reset that clears its own bit and nothing else
 * fails here.
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
 * Round-trips owned fields, drops the rest. All four FIFO encodings run,
 * or a model storing only some bits passes a single-value check.
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
 * The two DMA blocks' config registers are independent storage; OpenBSD's
 * attach read-modify-writes each and must see back what it wrote.
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
 * The control register keeps nothing; what this guards is the neighbour.
 * A decode that swallows the low address bit lands this write on the
 * config register, invisibly -- so the assertion is that the config value
 * survives the control write beside it.
 */
static void test_dma_block_ctrl_is_inert(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    /* Clear-channel and reset-interrupt together, as the firmware writes it. */
    const uint16_t clear_channel = 0x000a;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    qpci_io_writew(dev, io, ISP_REG_DMA_CMD_CFG, ISP_DMA_CFG_BURST_ENABLE);
    qpci_io_writew(dev, io, ISP_REG_DMA_DATA_CFG, ISP_DMA_CFG_BURST_ENABLE);

    qpci_io_writew(dev, io, ISP_REG_DMA_CMD_CTRL, clear_channel);
    qpci_io_writew(dev, io, ISP_REG_DMA_DATA_CTRL, clear_channel);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_DMA_CMD_CFG), ==,
                    ISP_DMA_CFG_BURST_ENABLE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_DMA_DATA_CFG), ==,
                    ISP_DMA_CFG_BURST_ENABLE);

    qpci_io_writew(dev, io, ISP_REG_DMA_CMD_CFG, 0);
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
 * Distinct values in all eight, read back after all writes, so a shared
 * register or dropped address bits fail rather than pass eight separate
 * round-trips. Mailboxes 4 and 5 are ordinary storage with no queue
 * initialised, which the firmware's register test depends on.
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
 * Mailboxes 4 and 5 under their queue-pointer names. With no queue
 * configured both are plain storage; request-in resets to 0x0001, the
 * value Linux's product-ID check requires. Once queues exist, a read of
 * either answers the adapter's own count, never an echo of the guest's
 * write -- and a synchronously draining ring makes the two coincide,
 * which is why only the unconfigured state can show the sourcing. The
 * write half of response-in's contract is
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

    /*
     * No queue yet, so both echo: the firmware's register test writes a
     * pattern across these two before initialising anything.
     */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_REQUEST_QUEUE), ==, 0x0021);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 0x0043);

    /* The mailbox name is the same address, so it reads the same answer. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(4)), ==, 0x0021);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(5)), ==, 0x0043);

    /* Reset restores the identity value the product-ID check reads. */
    qpci_io_writew(dev, io, ISP_REG_BUS_CTRL, ISP_BUS_CTRL_SOFT_RESET);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(4)), ==,
                    ISP_MAILBOX4_SIGNATURE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(5)), ==, 0);

    qpci_io_writew(dev, io, ISP_REG_BUS_CTRL, ISP_BUS_CTRL_SOFT_RESET);
    qpci_iounmap(dev, io);
}

/*
 * The top four bits of a write are the command; a read reports the RISC
 * state. Pause, reset, release run consecutively in a real attach.
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

    /* Reset replaces every flag; the paused flag must not survive it. */
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

    /* A no-op on real hardware: the flag it suggests clearing survives. */
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_CLEAR_HOST_INT << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==,
                    ISP_RISC_FLAG_HOST_INT);

    /* Clearing the RISC interrupt is what actually clears it. */
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_CLEAR_RISC_INT << ISP_HOST_CMD_SHIFT);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==, 0);

    /* The same command number in the low bits must do nothing. */
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD, ISP_HOST_CMD_PAUSE_RISC);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_HOST_CMD), ==, 0);

    qpci_iounmap(dev, io);
}

/*
 * Bank select is bit 3; the observable of it working is the same offset
 * holding two values. Bit 0 is the wrong constant a driver header offers,
 * so it is set too and must switch nothing.
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
 * A no-op proves the mailbox engine runs, not just its registers: the
 * semaphore lock and status bits only go non-zero as side effects of
 * processing, distinguishing an engine from storage.
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
     * Two-step acknowledgement: clear the RISC interrupt, then release the
     * semaphore in a separate write.
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
 * An unimplemented command still completes, as invalid: the protocol has
 * no way to leave a command pending.
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
 * Download, verify, execute, and confirm the engine still answers; the
 * image is never run. Its checksum is genuinely zero by construction, so
 * the verify is not the model agreeing with itself.
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
 * A non-zero sum answers failure with the sum in mailbox 1; without this
 * half, a model that never runs the checksum passes the zero-sum test.
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
 * Loads a block transfer's mailboxes. The high address halves are written
 * even when zero -- mailboxes persist between commands, and a stale half
 * would be reassembled into this transfer's address. The split (high half
 * in 2, low in 3) is driver-sourced, so a transposition should fail a
 * test rather than a firmware boot.
 */
static void qlogic_isp_ram_block_call(QPCIDevice *dev, QPCIBar io,
                                      uint16_t cmd, uint16_t base,
                                      uint64_t host, uint16_t count)
{
    qpci_io_writew(dev, io, ISP_MAILBOX(4), count);
    qpci_io_writew(dev, io, ISP_MAILBOX(6), (host >> 48) & 0xffff);
    qpci_io_writew(dev, io, ISP_MAILBOX(7), (host >> 32) & 0xffff);
    qlogic_isp_mbox_call4(dev, io, cmd, base,
                          (host >> 16) & 0xffff, host & 0xffff);
}

/*
 * Round trip through the adapter's memory. The answer mailbox is poisoned
 * before each read, the reads run in reverse order of the writes, and the
 * highest offset is included as the off-by-one boundary.
 */
static void test_read_ram_word_round_trip(void *obj, void *data,
                                          QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    const uint16_t offsets[] = { 0x0000, 0x1234, 0xffff };
    const uint16_t values[] = { 0xa5a5, 0x0001, 0xdead };
    size_t i;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    for (i = 0; i < ARRAY_SIZE(offsets); i++) {
        qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_WRITE_RAM_WORD,
                              offsets[i], values[i], 0);
        g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                        ISP_MBOX_COMPLETE);
        qlogic_isp_ack(dev, io);
    }

    for (i = ARRAY_SIZE(offsets); i-- > 0; ) {
        qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_READ_RAM_WORD,
                              offsets[i], 0x5a5a, 0xc3c3);
        g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                        ISP_MBOX_COMPLETE);
        g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(2)), ==, values[i]);
        /*
         * Mailbox 3 is an output of this command and reads zero. It is loaded
         * non-zero above, so this fails against a model that answers only
         * mailbox 2 and leaves the previous command's value in 3.
         */
        g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(3)), ==, 0);
        qlogic_isp_ack(dev, io);
    }

    qpci_iounmap(dev, io);
}

/*
 * Load and dump round trip, cross-checked against the single-word path.
 * The destination is pre-filled so a dump that never ran is visible, and
 * the 16-bit offset wrap is driven: a load at the highest offset puts its
 * second word at zero.
 */
static void test_ram_block_dma_round_trip(void *obj, void *data,
                                          QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t src, dst;
    const uint16_t base = 0x2000;
    const uint16_t count = 8;
    uint8_t pattern[16], filler[16], got[16];
    size_t i;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    qts = dev->bus->qts;

    src = guest_alloc(alloc, sizeof(pattern));
    dst = guest_alloc(alloc, sizeof(got));

    for (i = 0; i < sizeof(pattern); i++) {
        pattern[i] = 0x10 + i;
        filler[i] = 0xbb;
    }
    qtest_memwrite(qts, src, pattern, sizeof(pattern));
    qtest_memwrite(qts, dst, filler, sizeof(filler));

    qlogic_isp_ram_block_call(dev, io, ISP_MBOX_CMD_LOAD_RAM, base, src,
                              count);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_ram_block_call(dev, io, ISP_MBOX_CMD_DUMP_RAM, base, dst,
                              count);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qtest_memread(qts, dst, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), pattern, sizeof(pattern));

    /*
     * The block path and the single-word path address the same storage. The
     * first pattern word sits little-endian in guest memory, which is the
     * order the byte-order check pins on its own.
     */
    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_READ_RAM_WORD, base,
                          0x5a5a, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(2)), ==,
                    pattern[0] | (pattern[1] << 8));
    qlogic_isp_ack(dev, io);

    qlogic_isp_ram_block_call(dev, io, ISP_MBOX_CMD_LOAD_RAM, 0xffff, src, 2);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_READ_RAM_WORD, 0x0000,
                          0x5a5a, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(2)), ==,
                    pattern[2] | (pattern[3] << 8));
    qlogic_isp_ack(dev, io);

    guest_free(alloc, src);
    guest_free(alloc, dst);
    qpci_iounmap(dev, io);
}

/*
 * The block path's byte order, read as raw bytes: a round trip cancels
 * either order. Both directions run -- a model could differ per direction
 * -- and the expected bytes are explicit, not derived from the host.
 */
static void test_ram_dma_byte_order(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t buf;
    const uint16_t base = 0x3000;
    uint8_t raw[2];

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    qts = dev->bus->qts;

    buf = guest_alloc(alloc, sizeof(raw));

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_WRITE_RAM_WORD, base,
                          0x1234, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_ram_block_call(dev, io, ISP_MBOX_CMD_DUMP_RAM, base, buf, 1);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qtest_memread(qts, buf, raw, sizeof(raw));
    g_assert_cmphex(raw[0], ==, 0x34);
    g_assert_cmphex(raw[1], ==, 0x12);

    raw[0] = 0x78;
    raw[1] = 0x56;
    qtest_memwrite(qts, buf, raw, sizeof(raw));

    qlogic_isp_ram_block_call(dev, io, ISP_MBOX_CMD_LOAD_RAM, base, buf, 1);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_READ_RAM_WORD, base,
                          0x5a5a, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(2)), ==, 0x5678);
    qlogic_isp_ack(dev, io);

    guest_free(alloc, buf);
    qpci_iounmap(dev, io);
}

/*
 * Completion is the only hard requirement; the version triple is this
 * model's own declaration, which no guest inspects.
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
 * 512 sizes every queue the guest allocates; asserted explicitly because
 * it is a recorded decision, not a driver-confirmed value.
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
 * The config setters in bring-up order. OpenBSD's qlw_attach() aborts the
 * attach the instant any one answers invalid, before creating the SCSI
 * bus; completion is the entire contract.
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
     * Three more only Linux issues, whose status its caller discards --
     * answered anyway so documented commands are not unmodelled noise.
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
 * base address split the way OpenBSD's own driver splits it -- low half in
 * mailbox 3, next in mailbox 2, next in mailbox 7, top in mailbox 6 -- then
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
 * A zero address or entry count is rejected: the model must not silently
 * accept a queue it was not given.
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
 * The firmware's walking-pattern register test runs before any queue is
 * initialised, so mailboxes 4 and 5 must echo then; with queues live, both
 * answer the adapter's own counts, and echo is deliberately not asserted
 * -- asserting it would re-encode the completion-erasure bug.
 */
static void test_mailbox_register_test(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    uint64_t rqst_addr, resp_addr;
    int i;

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);

    /* No queue initialised: every mailbox echoes, and the command completes. */
    for (i = 1; i < ISP_MAILBOX_COUNT; i++) {
        qpci_io_writew(dev, io, ISP_MAILBOX(i), 0x1111 * i);
    }
    qpci_io_writew(dev, io, ISP_MAILBOX(0), ISP_MBOX_CMD_MAILBOX_REG_TEST);
    qpci_io_writew(dev, io, ISP_REG_HOST_CMD,
                   ISP_HOST_CMD_SET_HOST_INT << ISP_HOST_CMD_SHIFT);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    for (i = 1; i < ISP_MAILBOX_COUNT; i++) {
        g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(i)), ==,
                        0x1111 * i);
    }
    qlogic_isp_ack(dev, io);

    /* With queues live, both addresses answer the adapter's own counts. */
    rqst_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   4, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   4, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    /* Nothing has been produced or consumed yet, so both read zero. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_REQUEST_QUEUE), ==, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 0);

    /*
     * An acknowledgement write to the response pointer is recorded as the
     * guest's consumer position and must leave the adapter's published
     * producer count alone -- which is what a read of that address answers.
     */
    qpci_io_writew(dev, io, ISP_REG_RESPONSE_QUEUE, 3);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 0);

    guest_free(alloc, rqst_addr);
    guest_free(alloc, resp_addr);
    qpci_io_writew(dev, io, ISP_REG_BUS_CTRL, ISP_BUS_CTRL_SOFT_RESET);
    qpci_iounmap(dev, io);
}

/*
 * The steady-state ring contract: a published command entry produces a
 * status entry with the same handle, the response pointer advanced, and an
 * interrupt with the semaphore clear -- an I/O completion is not a mailbox
 * result. Test-unit-ready moves no data, keeping this to the ring protocol.
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
     * The first command answers the power-on unit attention (generic SCSI
     * behaviour; virtio-scsi's suite relies on the same). Asserting the
     * specific condition plus a clean second command distinguishes a
     * relaying model from one inventing a fixed answer.
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
 * A status entry zeroes its whole slot before writing. Proven by putting
 * real sense data in a slot first, then wrapping a clean completion onto
 * it: sense_len zero and zero sense bytes afterwards distinguishes
 * "zeroed" from "never written", which a fresh slot cannot.
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
 * Linux walks state_flags on an incomplete completion to pick which
 * failure to report; a missing device must say bus reached, nothing past
 * it.
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
     * Two entries, not one: a one-entry ring wraps producer 1 back onto
     * consumer 0 and can never say "one entry arrived".
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
 * An unrecognized entry type answers INCOMPLETE for the handle the guest
 * is waiting on, at the offset every handle-carrying type shares, rather
 * than being consumed silently -- the failure that hung a real boot.
 */
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

    /* Two entries: a one-entry ring can never publish anything. */
    rqst_addr = guest_alloc(alloc, 2 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 2 * ISP_QUEUE_ENTRY_SIZE);

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   2, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   2, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_EXTENDED_CMD;
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
 * The cursor advances by an entry's own entry_count, so a continuation
 * never reaches the default case alone. A regression to plain +1 would
 * read the continuation's offset 4 -- a DMA address, not a handle -- and
 * fail some unrelated in-flight command. "Exactly one completion" is
 * asserted both by the producer count and by poisoning the second
 * response slot, since the count alone stops working when the ring size
 * changes.
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
     * Three request entries so producer=2 stays distinct from consumer=0;
     * the two-slot response ring's second slot is the poisoned witness.
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
    entry[0] = ISP_ENTRY_TYPE_EXTENDED_CMD;
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
    isp_put32(cont, ISP_A64_CONT_OFF_SEGS, 0xdeadbeef);
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
 * The walk must terminate for every producer and entry_count pair: an
 * exact-equality exit with a stride sharing a factor with the ring length
 * steps over the producer forever -- an infinite loop under the BQL from
 * one guest store. The walk is bounded by the published-entry count.
 */
static void test_ring_walk_bounded_by_producer(void *obj, void *data,
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

    rqst_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   4, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   4, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    /* Markers claiming two slots each, in every slot of a four-slot ring. */
    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_MARKER;
    entry[1] = 2;
    for (i = 0; i < 4; i++) {
        qtest_memwrite(qts, rqst_addr + i * ISP_QUEUE_ENTRY_SIZE, entry,
                       sizeof(entry));
    }

    /* One published entry: the cursor must stop exactly here. */
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 1);

    /* The walk consumed what was published and a marker posts nothing. */
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_REQUEST_QUEUE), ==, 1);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 0);

    /* The ring is still live: a real command from the next slot completes. */
    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x51);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);
    qtest_memwrite(qts, rqst_addr + ISP_QUEUE_ENTRY_SIZE, entry,
                   sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 2);

    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_REQUEST_QUEUE), ==, 2);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 1);
    qtest_memread(qts, resp_addr, status, sizeof(status));
    g_assert_cmphex(status[0], ==, ISP_ENTRY_TYPE_STATUS);
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, 0x51);

    qlogic_isp_ack(dev, io);
    qpci_iounmap(dev, io);
}

/*
 * Writes a test-unit-ready request entry at the given request ring slot,
 * publishes it, asserts a status entry for the same handle landed in the
 * given response slot, acknowledges the interrupt, and hands back the
 * SCSI status word. The full status entry is left in the caller's buffer
 * for any further field it wants to read.
 */
static uint16_t isp_test_unit_ready(QPCIDevice *dev, QPCIBar io,
                                    QTestState *qts, uint64_t rqst_addr,
                                    uint64_t resp_addr, int slot,
                                    int resp_slot, uint16_t prod,
                                    uint32_t handle, uint8_t *status)
{
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, handle);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);    /* bus 0, target 0, lun 0. */
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);
    qtest_memwrite(qts, rqst_addr + slot * ISP_QUEUE_ENTRY_SIZE, entry,
                   sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, prod);

    qtest_memread(qts, resp_addr + resp_slot * ISP_QUEUE_ENTRY_SIZE, status,
                  ISP_QUEUE_ENTRY_SIZE);
    g_assert_cmphex(status[0], ==, ISP_ENTRY_TYPE_STATUS);
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, handle);
    qlogic_isp_ack(dev, io);

    return isp_get16(status, ISP_STATUS_OFF_SCSI_STATUS);
}

/*
 * A soft reset must reach the SCSI bus behind the chip. The observable is
 * the disk's re-armed unit attention: consumed before the reset, it must
 * answer check condition again after -- a reset that never touched the
 * bus leaves the disk answering good. Everything here completes inside
 * its doorbell, so what is pinned is reach, not in-flight cancellation.
 */
static void test_soft_reset_resets_scsi_bus(void *obj, void *data,
                                            QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t rqst_addr, resp_addr;
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

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

    /* Consume the power-on unit attention, then see a clean command. */
    g_assert_cmphex(isp_test_unit_ready(dev, io, qts, rqst_addr, resp_addr,
                                        0, 0, 1, 0x11, status), ==,
                    CHECK_CONDITION | ISP_SCSI_STATUS_SENSE_VALID);
    g_assert_cmphex(isp_test_unit_ready(dev, io, qts, rqst_addr, resp_addr,
                                        1, 1, 2, 0x12, status), ==, GOOD);

    qpci_io_writew(dev, io, ISP_REG_BUS_CTRL, ISP_BUS_CTRL_SOFT_RESET);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_BUS_CTRL), ==, 0);

    /* The chip is back at power-on state: re-initialise the queues. */
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   4, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   4, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    /* The reset reached the disk: its unit attention is armed again. */
    g_assert_cmphex(isp_test_unit_ready(dev, io, qts, rqst_addr, resp_addr,
                                        0, 0, 1, 0x13, status), ==,
                    CHECK_CONDITION | ISP_SCSI_STATUS_SENSE_VALID);
    g_assert_cmphex(status[ISP_STATUS_OFF_SENSE_DATA + 2] & 0x0f, ==,
                    UNIT_ATTENTION);

    qpci_iounmap(dev, io);
}

/*
 * The bus-reset mailbox command is a SCSI bus reset, not a chip reset:
 * the rings survive it, but the devices on the bus must see it -- the
 * same unit-attention observable the soft-reset test pins, reached
 * through the command a driver actually issues to reset the bus.
 */
static void test_bus_reset_mailbox_resets_scsi_bus(void *obj, void *data,
                                                   QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t rqst_addr, resp_addr;
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];

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

    /* Consume the power-on unit attention, then see a clean command. */
    g_assert_cmphex(isp_test_unit_ready(dev, io, qts, rqst_addr, resp_addr,
                                        0, 0, 1, 0x21, status), ==,
                    CHECK_CONDITION | ISP_SCSI_STATUS_SENSE_VALID);
    g_assert_cmphex(isp_test_unit_ready(dev, io, qts, rqst_addr, resp_addr,
                                        1, 1, 2, 0x22, status), ==, GOOD);

    /* Reset the SCSI bus by mailbox command; mailbox 1 is the delay. */
    qlogic_isp_mbox_call4(dev, io, ISP_MBOX_CMD_BUS_RESET, 1, 0, 0);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_MAILBOX(0)), ==,
                    ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    /* The rings survived, and the disk saw the reset. */
    g_assert_cmphex(isp_test_unit_ready(dev, io, qts, rqst_addr, resp_addr,
                                        2, 2, 3, 0x23, status), ==,
                    CHECK_CONDITION | ISP_SCSI_STATUS_SENSE_VALID);
    g_assert_cmphex(status[ISP_STATUS_OFF_SENSE_DATA + 2] & 0x0f, ==,
                    UNIT_ATTENTION);

    qpci_iounmap(dev, io);
}

/*
 * The device word carries a bus bit and this part drives one bus: a
 * bus-1 request must answer incomplete with no phase reached, not
 * execute on bus 0 against a device the guest never named.
 */
static void test_second_bus_request_rejected(void *obj, void *data,
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

    rqst_addr = guest_alloc(alloc, 2 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 2 * ISP_QUEUE_ENTRY_SIZE);

    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_REQ_QUEUE,
                                   2, rqst_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);
    g_assert_cmphex(isp_init_queue(dev, io, ISP_MBOX_CMD_INIT_RSP_QUEUE,
                                   2, resp_addr), ==, ISP_MBOX_COMPLETE);
    qlogic_isp_ack(dev, io);

    /* Bus 1, target 0, lun 0: the address behind the bus bit is populated. */
    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_REQUEST;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x31);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 1 << 15);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);
    qtest_memwrite(qts, rqst_addr, entry, sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 1);

    qtest_memread(qts, resp_addr, status, sizeof(status));
    g_assert_cmphex(status[0], ==, ISP_ENTRY_TYPE_STATUS);
    g_assert_cmphex(isp_get32(status, ISP_STATUS_OFF_HANDLE), ==, 0x31);
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_COMPLETION), ==,
                    ISP_IOCB_INCOMPLETE);
    /* No phase reached: the second bus does not exist to be reached. */
    g_assert_cmphex(isp_get16(status, ISP_STATUS_OFF_STATE_FLAGS), ==, 0);

    qlogic_isp_ack(dev, io);
    qpci_iounmap(dev, io);
}

/*
 * An inquiry with a real data segment, read back out of guest memory: the
 * data phase no test-unit-ready round trip touches, and the first command
 * a real driver's target probe issues.
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
     * The buffer started all-0xff; only a real DMA can have put the
     * device-type byte and the vendor strings here.
     */
    qtest_memread(qts, data_addr, inquiry, sizeof(inquiry));
    g_assert_cmphex(inquiry[0], ==, 0x00);
    g_assert_cmpint(memcmp(&inquiry[8], "QEMU    ", 8), ==, 0);
    g_assert_cmpint(memcmp(&inquiry[16], "QEMU HARDDISK   ", 16), ==, 0);

    qpci_iounmap(dev, io);
}

/*
 * Five segments for a 36-byte inquiry: four exactly filling the inline
 * area, one in a continuation, so the boundary itself is exercised. The
 * destinations are deliberately not contiguous -- adjacency is the wrong
 * reason for a scatter-gather model to work.
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
     * Poison every destination first, or a segment the model never writes
     * matches the zeroed guest_alloc default.
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
     * A follow-up command only answers cleanly if the cursor advanced by
     * the previous command's entry_count, past its continuation.
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
 * The A64 counterpart of the inquiry test: Linux never falls back to the
 * type-0 format on a 64-bit kernel, so every command a real boot issues
 * is this format -- one inline 12-byte segment, low address half first.
 */
static void test_a64_inquiry_data_transfer(void *obj, void *data,
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
    entry[0] = ISP_ENTRY_TYPE_COMMAND_A64;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x1);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);
    qtest_memwrite(qts, rqst_addr, entry, sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 1);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 1);
    qlogic_isp_ack(dev, io);

    /* Second command: a real inquiry with a 64-bit-addressed data-in segment. */
    memset(inquiry, 0xff, sizeof(inquiry));
    qtest_memwrite(qts, data_addr, inquiry, sizeof(inquiry));

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_COMMAND_A64;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x2);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 1);
    entry[ISP_REQ_OFF_CDB + 0] = INQUIRY;
    entry[ISP_REQ_OFF_CDB + 4] = sizeof(inquiry);    /* allocation length */
    isp_put32(entry, ISP_A64_REQ_OFF_SEGS + 0, (uint32_t)data_addr);
    isp_put32(entry, ISP_A64_REQ_OFF_SEGS + 4, (uint32_t)(data_addr >> 32));
    isp_put32(entry, ISP_A64_REQ_OFF_SEGS + 8, sizeof(inquiry));

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

    qtest_memread(qts, data_addr, inquiry, sizeof(inquiry));
    g_assert_cmphex(inquiry[0], ==, 0x00);
    g_assert_cmpint(memcmp(&inquiry[8], "QEMU    ", 8), ==, 0);
    g_assert_cmpint(memcmp(&inquiry[16], "QEMU HARDDISK   ", 16), ==, 0);

    qpci_iounmap(dev, io);
}

/*
 * The A64 continuation boundary, which sits at two inline segments rather
 * than four; otherwise the same shape and the same non-adjacent
 * destinations as the type-0 version.
 */
static void test_a64_continuation_entry_segments(void *obj, void *data,
                                                  QGuestAllocator *alloc)
{
    QQlogicIsp *isp = obj;
    QPCIDevice *dev = &isp->dev;
    QPCIBar io;
    QTestState *qts;
    uint64_t rqst_addr, resp_addr;
    uint64_t piece[3];
    uint8_t entry[ISP_QUEUE_ENTRY_SIZE];
    uint8_t cont[ISP_QUEUE_ENTRY_SIZE];
    uint8_t status[ISP_QUEUE_ENTRY_SIZE];
    uint8_t inquiry[36];
    size_t i;
    const uint8_t piece_len[3] = { 16, 16, 4 };

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, NULL);
    qts = dev->bus->qts;

    rqst_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);
    resp_addr = guest_alloc(alloc, 4 * ISP_QUEUE_ENTRY_SIZE);
    for (i = 0; i < 3; i++) {
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
    entry[0] = ISP_ENTRY_TYPE_COMMAND_A64;
    entry[1] = 1;
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x1);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 0);
    qtest_memwrite(qts, rqst_addr, entry, sizeof(entry));
    qpci_io_writew(dev, io, ISP_REG_REQUEST_QUEUE, 1);
    g_assert_cmphex(qpci_io_readw(dev, io, ISP_REG_RESPONSE_QUEUE), ==, 1);
    qlogic_isp_ack(dev, io);

    /* Poison the destinations, as in the type-0 version. */
    for (i = 0; i < 3; i++) {
        uint8_t poison[64];
        memset(poison, 0xff, sizeof(poison));
        qtest_memwrite(qts, piece[i], poison, sizeof(poison));
    }

    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_COMMAND_A64;
    entry[1] = 2;    /* this entry plus one continuation. */
    isp_put32(entry, ISP_REQ_OFF_HANDLE, 0x2);
    isp_put16(entry, ISP_REQ_OFF_DEVICE, 0);
    isp_put16(entry, ISP_REQ_OFF_CDBLEN, 6);
    isp_put16(entry, ISP_REQ_OFF_SEGCOUNT, 3);
    entry[ISP_REQ_OFF_CDB + 0] = INQUIRY;
    entry[ISP_REQ_OFF_CDB + 4] = sizeof(inquiry);
    for (i = 0; i < 2; i++) {
        int off = ISP_A64_REQ_OFF_SEGS + i * ISP_A64_SEG_SIZE;

        isp_put32(entry, off + 0, (uint32_t)piece[i]);
        isp_put32(entry, off + 4, (uint32_t)(piece[i] >> 32));
        isp_put32(entry, off + 8, piece_len[i]);
    }
    qtest_memwrite(qts, rqst_addr + ISP_QUEUE_ENTRY_SIZE, entry,
                   sizeof(entry));

    memset(cont, 0, sizeof(cont));
    cont[0] = ISP_ENTRY_TYPE_CONTINUE_A64;
    cont[1] = 1;
    isp_put32(cont, ISP_A64_CONT_OFF_SEGS + 0, (uint32_t)piece[2]);
    isp_put32(cont, ISP_A64_CONT_OFF_SEGS + 4, (uint32_t)(piece[2] >> 32));
    isp_put32(cont, ISP_A64_CONT_OFF_SEGS + 8, piece_len[2]);
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

    /* Reassemble the three DMA'd pieces and check them like one buffer. */
    for (i = 0; i < 3; i++) {
        qtest_memread(qts, piece[i], inquiry + (i < 2 ? i * 16 : 32),
                     piece_len[i]);
    }
    g_assert_cmphex(inquiry[0], ==, 0x00);
    g_assert_cmpint(memcmp(&inquiry[8], "QEMU    ", 8), ==, 0);
    g_assert_cmpint(memcmp(&inquiry[16], "QEMU HARDDISK   ", 16), ==, 0);

    /* The cursor advanced by entry_count, as the type-0 version proves. */
    memset(entry, 0, sizeof(entry));
    entry[0] = ISP_ENTRY_TYPE_COMMAND_A64;
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
 * Bit-bangs an EEPROM read the way OpenBSD does and checks every data-in
 * bit comes back clear. What is pinned is the register's contract -- three
 * bits round-trip, the fourth never reads set -- not the guest's
 * validation arithmetic, which that contract guarantees will fail.
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
 * Four unacknowledged completions on a four-entry ring: without a
 * reserved slot the producer wraps onto the consumer, which every driver
 * spells "empty", and the guest sleeps on four posted completions. The
 * request ring is oversized so only the response side is under test.
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
 * An acknowledgement must not erase a completion posted behind it: the
 * driver reads the producer once and writes that value back at the end of
 * its handler, so anything landing in between would be overwritten by an
 * older number if the shared address were plain storage. The ring is
 * oversized so a wrap cannot be the cause.
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
 * A held completion must re-signal when it finally posts. The interrupt
 * is cleared before the fill checks so they cannot pass on bits an
 * earlier completion set; the held completion stays silent until an
 * acknowledgement frees a slot, then posts and raises the interrupt --
 * without which the driver has no reason to go looking.
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
    qos_add_test("dma-block-ctrl-is-inert", "qlogic-isp1040",
                 test_dma_block_ctrl_is_inert, NULL);
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
    qos_add_test("read-ram-word-round-trip", "qlogic-isp1040",
                 test_read_ram_word_round_trip, NULL);
    qos_add_test("ram-block-dma-round-trip", "qlogic-isp1040",
                 test_ram_block_dma_round_trip, NULL);
    qos_add_test("ram-dma-byte-order", "qlogic-isp1040",
                 test_ram_dma_byte_order, NULL);
    qos_add_test("about-firmware", "qlogic-isp1040",
                 test_about_firmware, NULL);
    qos_add_test("get-firmware-status", "qlogic-isp1040",
                 test_get_firmware_status, NULL);
    qos_add_test("config-setters", "qlogic-isp1040",
                 test_config_setters, NULL);
    qos_add_test("init-queue-rejects-zero", "qlogic-isp1040",
                 test_init_queue_rejects_zero, NULL);
    qos_add_test("mailbox-register-test", "qlogic-isp1040",
                 test_mailbox_register_test, NULL);
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
    qos_add_test("ring-walk-bounded-by-producer", "qlogic-isp1040",
                 test_ring_walk_bounded_by_producer, NULL);
    qos_add_test("soft-reset-resets-scsi-bus", "qlogic-isp1040",
                 test_soft_reset_resets_scsi_bus, NULL);
    qos_add_test("bus-reset-mailbox-resets-scsi-bus", "qlogic-isp1040",
                 test_bus_reset_mailbox_resets_scsi_bus, NULL);
    qos_add_test("second-bus-request-rejected", "qlogic-isp1040",
                 test_second_bus_request_rejected, NULL);
    qos_add_test("inquiry-data-transfer", "qlogic-isp1040",
                 test_inquiry_data_transfer, NULL);
    qos_add_test("continuation-entry-segments", "qlogic-isp1040",
                 test_continuation_entry_segments, NULL);
    qos_add_test("a64-inquiry-data-transfer", "qlogic-isp1040",
                 test_a64_inquiry_data_transfer, NULL);
    qos_add_test("a64-continuation-entry-segments", "qlogic-isp1040",
                 test_a64_continuation_entry_segments, NULL);
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
