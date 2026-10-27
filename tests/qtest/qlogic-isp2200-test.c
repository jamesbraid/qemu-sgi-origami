/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * QTest for the QLogic ISP2200 Fibre Channel host adapter.
 *
 * Most cases run the adapter with nothing on its loop, so every command
 * completes "port unavailable" and what reached the loop service is read
 * back through the device's x-fc-* QOM properties. The loop cases plug
 * scsi-hd devices onto the adapter's bus as loop ports, at the loop ID
 * their SCSI target number gives. The register contract comes from
 * the public Linux qla2xxx, OpenBSD qla and NetBSD isp drivers; tests
 * marked "approximation" pin a model choice those drivers leave open, not
 * a measured hardware fact. The contract is in
 * docs/system/mips/sgi/hardware/qlogic-isp2200.rst.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "hw/pci/pci_regs.h"

#define ISP_VENDOR_ID       0x1077
#define ISP_DEVICE_ID       0x2200
#define ISP_WINDOW_SIZE     0x100
#define ISP_QOM_PATH        "/machine/peripheral/isp0"

#define REG_FLASH_ADDR  0x00
#define REG_FLASH_DATA  0x02
#define REG_CSR         0x06
#define REG_ICR         0x08
#define REG_ISR         0x0a
#define REG_SEMAPHORE   0x0c
#define REG_NVRAM       0x0e
#define REG_HCCR        0xc0
#define REG_GPIO_DATA   0xcc

#define MBOX(n)         ((n) < 8 ? 0x10 + (n) * 2 : 0xe0 + ((n) - 8) * 2)
#define REG_REQUEST     MBOX(4)
#define REG_RESPONSE    MBOX(5)
#define MBOX_COUNT      24

#define CSR_SOFT_RESET  0x0001
#define ICR_ENABLE      0x8008
#define ISR_PENDING     0x8000
#define ISR_RISC        0x0008
#define SEM_LOCK        0x0001

#define HCCR_RESET      0x1000
#define HCCR_PAUSE      0x2000
#define HCCR_RELEASE    0x3000
#define HCCR_PARITY4    0x4000
#define HCCR_PARITY_PAUSE 0x4001
#define HCCR_SET_HOST   0x5000
#define HCCR_CLEAR_HOST 0x6000
#define HCCR_CLEAR_RISC 0x7000

#define HCCR_HOST_INT   0x0080
#define HCCR_RISC_RESET 0x0040
#define HCCR_RISC_PAUSE 0x0020

#define MBOX_BUSY       0x0004

#define CMD_NOP             0x0000
#define CMD_LOAD_RAM_32     0x0001
#define CMD_DUMP_RAM        0x0003
#define CMD_EXEC_FIRMWARE   0x0002
#define CMD_WRITE_RAM_WORD  0x0004
#define CMD_READ_RAM_WORD   0x0005
#define CMD_REGISTER_TEST   0x0006
#define CMD_VERIFY_CHECKSUM 0x0007
#define CMD_ABOUT_FIRMWARE  0x0008
#define CMD_LOAD_RAM        0x0009
#define CMD_LOAD_RAM_EXT    0x000b
#define CMD_ABORT_IOCB      0x0015
#define CMD_FIRMWARE_STATUS 0x001f
#define CMD_GET_ADAPTER_ID  0x0020
#define CMD_GET_FW_OPTIONS  0x0028
#define CMD_SET_FW_OPTIONS  0x0038
#define CMD_RESOURCE_COUNTS 0x0042
#define CMD_INIT_FIRMWARE   0x0060
#define CMD_GET_FW_STATE    0x0069
#define CMD_GET_RETRY_COUNT 0x0022
#define CMD_GET_PORT_QUEUE  0x0029
#define CMD_SET_RETRY_COUNT 0x0032
#define CMD_RESET_LINK_STAT 0x0052
#define CMD_GET_LINK_STATUS 0x006b
#define CMD_ENH_PORT_DB     0x0047
#define CMD_INITIATE_LIP    0x0062
#define CMD_POSITION_MAP    0x0063
#define CMD_GET_PORT_DB     0x0064
#define CMD_GET_PORT_NAME   0x006a
#define CMD_FULL_LOGIN_LIP  0x0072
#define CMD_PORT_NAME_LIST  0x0075
#define CMD_GET_ID_LIST     0x007c

#define MBOX_COMPLETE       0x4000
#define MBOX_INVALID        0x4001
#define MBOX_HOST_IF_ERROR  0x4002
#define MBOX_TEST_FAILED    0x4003
#define MBOX_COMMAND_ERROR  0x4005
#define MBOX_PARAM_ERROR    0x4006

#define ASYNC_REQUEST_DMA   0x8003
#define ASYNC_RESPONSE_DMA  0x8004
#define ASYNC_LIP           0x8010
#define ASYNC_LOOP_UP       0x8011
#define ASYNC_LIP_RESET     0x8013
#define ASYNC_PORT_DB       0x8014

#define FW_STATE_CONFIG_WAIT 0
#define FW_STATE_WAIT_AL_PA  1
#define FW_STATE_READY       3

#define CODE_ORG            0x1000

#define ICB_SIZE            96
#define ICB_OPT_HARD_ADDR   0x0001
#define ICB_OPT_FAST_POST   0x0008
#define ICB_OPT_PORT_DB     0x0100

#define ENTRY_SIZE          64
#define ENTRY_CONT32        0x02
#define ENTRY_STATUS        0x03
#define ENTRY_MARKER        0x04
#define ENTRY_CONT64        0x0a
#define ENTRY_CMD32         0x11
#define ENTRY_CMD_LIST      0x15
#define ENTRY_CMD64         0x19

#define ENTRY_FLAG_BAD_TYPE  0x04
#define ENTRY_FLAG_BAD_PARAM 0x08
#define ENTRY_FLAG_BAD_COUNT 0x10
#define ENTRY_FLAG_BAD_ORDER 0x20
#define ENTRY_FLAG_DMA_ERROR 0x40

#define COMPLETION_COMPLETE         0x0000
#define COMPLETION_UNDERRUN         0x0015
#define COMPLETION_PORT_UNAVAILABLE 0x0028

#define SCSI_STATUS_SENSE_VALID     0x0200
#define SCSI_STATUS_RESID_UNDER     0x0800
#define SCSI_CHECK_CONDITION        0x02

/* The model's loop bring-up time, a named constant in the device. */
#define BRINGUP_NS          (100 * 1000 * 1000)

/* Past the Bridge test head's transfer memory: nothing backs it. */
#define UNMAPPED_PHYS       0x04000000ULL

/* The last request slot the transfer memory backs before it ends. */
#define TRANSFER_RAM_LAST_SLOT (0x03000000ULL - 64)

/*
 * Address bits 63:48, which only mailbox 6 carries. The test head's
 * Bridge takes bits 63:60 as the target widget and has nothing at widget
 * 0xe, so a transfer there is refused; with those bits dropped, the same
 * low 48 bits reach transfer memory.
 */
#define HIGH_WORD_WIDGET    (0xeULL << 60)

typedef struct QIsp2200 {
    QOSGraphObject obj;
    QPCIDevice dev;
} QIsp2200;

/* One adapter under test and the rings the host built for it. */
typedef struct Isp {
    QPCIDevice *dev;
    QPCIBar io;
    QTestState *qts;
    QGuestAllocator *alloc;
    uint64_t icb;
    uint64_t rq;
    uint64_t rs;
    uint16_t rqlen;
    uint16_t rslen;
    uint16_t rq_next;   /* Host producer. */
    uint16_t rs_next;   /* Host consumer. */
    uint16_t hard_addr; /* The control block's requested loop ID. */
    uint8_t retry_count; /* Control block byte 10. */
    uint8_t retry_delay; /* Control block byte 11. */
} Isp;

static void *isp2200_get_driver(void *obj, const char *interface)
{
    QIsp2200 *isp = obj;

    if (!g_strcmp0(interface, "pci-device")) {
        return &isp->dev;
    }
    fprintf(stderr, "%s not present in qlogic-isp2200\n", interface);
    g_assert_not_reached();
}

static void *isp2200_create(void *pci_bus, QGuestAllocator *alloc,
                            void *addr)
{
    QIsp2200 *isp = g_new0(QIsp2200, 1);

    qpci_device_init(&isp->dev, pci_bus, addr);
    isp->obj.get_driver = isp2200_get_driver;
    return &isp->obj;
}

static void isp_open(Isp *t, void *obj, QGuestAllocator *alloc)
{
    QIsp2200 *isp = obj;

    memset(t, 0, sizeof(*t));
    t->dev = &isp->dev;
    t->qts = t->dev->bus->qts;
    t->alloc = alloc;
    qpci_device_enable(t->dev);
    t->io = qpci_iomap(t->dev, 0, NULL);
}

static void isp_close(Isp *t)
{
    qpci_iounmap(t->dev, t->io);
}

static uint16_t rd(Isp *t, uint8_t reg)
{
    return qpci_io_readw(t->dev, t->io, reg);
}

static void wr(Isp *t, uint8_t reg, uint16_t val)
{
    qpci_io_writew(t->dev, t->io, reg, val);
}

static bool is_mips64(void)
{
    return g_str_equal(qtest_get_arch(), "mips64");
}

/* Mailboxes 0..n-1 in ascending order, then the doorbell. */
static uint16_t isp_mbox(Isp *t, const uint16_t *in, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        wr(t, MBOX(i), in[i]);
    }
    wr(t, REG_HCCR, HCCR_SET_HOST);
    return rd(t, MBOX(0));
}

static uint16_t isp_call(Isp *t, uint16_t cmd, uint16_t m1, uint16_t m2,
                         uint16_t m3)
{
    const uint16_t in[] = { cmd, m1, m2, m3 };

    return isp_mbox(t, in, ARRAY_SIZE(in));
}

/* The order both Linux and OpenBSD use: clear the latch, then release. */
static void isp_ack(Isp *t)
{
    wr(t, REG_HCCR, HCCR_CLEAR_RISC);
    wr(t, REG_SEMAPHORE, 0);
}

static uint16_t isp_call_ack(Isp *t, uint16_t cmd, uint16_t m1, uint16_t m2,
                             uint16_t m3)
{
    uint16_t status = isp_call(t, cmd, m1, m2, m3);

    g_assert_cmphex(rd(t, REG_SEMAPHORE) & SEM_LOCK, ==, SEM_LOCK);
    isp_ack(t);
    return status;
}

/*
 * A stand-in image built to sum to zero over the length in its word 3.
 * The model never runs it; no vendor firmware is involved.
 */
static const uint16_t test_image[] = {
    0x1234, 0x5678, 0x9abc, 8, 0x1111, 0x2222, 0x3333, 0x0000,
};

static uint16_t image_word(unsigned i)
{
    uint16_t sum = 0;
    unsigned j;

    if (i < ARRAY_SIZE(test_image) - 1) {
        return test_image[i];
    }
    for (j = 0; j < ARRAY_SIZE(test_image) - 1; j++) {
        sum += test_image[j];
    }
    return -sum;
}

static void isp_load_image(Isp *t)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(test_image); i++) {
        g_assert_cmphex(isp_call_ack(t, CMD_WRITE_RAM_WORD, CODE_ORG + i,
                                     image_word(i), 0), ==, MBOX_COMPLETE);
    }
}

/* What every driver does on attach: download, verify, execute. */
static void isp_start(Isp *t)
{
    isp_load_image(t);
    g_assert_cmphex(isp_call_ack(t, CMD_VERIFY_CHECKSUM, CODE_ORG, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(isp_call_ack(t, CMD_EXEC_FIRMWARE, CODE_ORG, 0, 0), ==,
                    MBOX_COMPLETE);
}

static void icb_build(uint8_t *icb, uint16_t rqlen, uint16_t rslen,
                      uint64_t rq, uint64_t rs, uint16_t options)
{
    memset(icb, 0, ICB_SIZE);
    icb[0] = 1;
    stw_le_p(icb + 2, options);
    stw_le_p(icb + 4, 1024);
    stw_le_p(icb + 36, rqlen);
    stw_le_p(icb + 38, rslen);
    stq_le_p(icb + 40, rq);
    stq_le_p(icb + 48, rs);
}

/* INIT_FIRMWARE in Linux's input form: mapping D in m2, m3, m6, m7. */
static uint16_t isp_init_firmware(Isp *t, uint64_t icb)
{
    const uint16_t in[] = {
        CMD_INIT_FIRMWARE, 0, (icb >> 16) & 0xffff, icb & 0xffff, 0, 0,
        (icb >> 48) & 0xffff, (icb >> 32) & 0xffff,
    };

    return isp_mbox(t, in, ARRAY_SIZE(in));
}

static void isp_rings_at(Isp *t, uint16_t rqlen, uint16_t rslen,
                         uint64_t icb, uint64_t rq, uint64_t rs,
                         uint16_t options, uint8_t xopts)
{
    uint8_t buf[ICB_SIZE];
    uint8_t zero[ENTRY_SIZE] = { 0 };
    unsigned i;

    t->icb = icb;
    t->rq = rq;
    t->rs = rs;
    t->rqlen = rqlen;
    t->rslen = rslen;
    t->rq_next = 0;
    t->rs_next = 0;

    for (i = 0; i < rslen && rs != UNMAPPED_PHYS; i++) {
        qtest_memwrite(t->qts, rs + i * ENTRY_SIZE, zero, sizeof(zero));
    }
    icb_build(buf, rqlen, rslen, rq, rs, options);
    buf[10] = t->retry_count;
    buf[11] = t->retry_delay;
    stw_le_p(buf + 20, t->hard_addr);
    buf[64] = xopts;
    qtest_memwrite(t->qts, icb, buf, sizeof(buf));
    g_assert_cmphex(isp_init_firmware(t, icb), ==, MBOX_COMPLETE);
    isp_ack(t);
}

static void isp_rings(Isp *t, uint16_t rqlen, uint16_t rslen)
{
    isp_rings_at(t, rqlen, rslen, guest_alloc(t->alloc, ICB_SIZE),
                 guest_alloc(t->alloc, rqlen * ENTRY_SIZE),
                 guest_alloc(t->alloc, rslen * ENTRY_SIZE), 0, 0);
}

static void entry_command(uint8_t *e, uint8_t type, uint32_t handle,
                          uint8_t loop_id, uint16_t lun, uint8_t opcode)
{
    memset(e, 0, ENTRY_SIZE);
    e[0] = type;
    e[1] = 1;
    stl_le_p(e + 4, handle);
    e[9] = loop_id;
    stw_le_p(e + 10, lun);
    e[20] = opcode;
}

static void isp_put_request(Isp *t, uint16_t index, const uint8_t *e)
{
    qtest_memwrite(t->qts, t->rq + index * ENTRY_SIZE, e, ENTRY_SIZE);
}

/* Writes the entry at the producer and publishes it. */
static void isp_publish(Isp *t, const uint8_t *e)
{
    isp_put_request(t, t->rq_next, e);
    t->rq_next = (t->rq_next + 1) % t->rqlen;
    wr(t, REG_REQUEST, t->rq_next);
}

static void isp_response(Isp *t, uint16_t index, uint8_t *e)
{
    qtest_memread(t->qts, t->rs + index * ENTRY_SIZE, e, ENTRY_SIZE);
}

/* Linux's ring handler order: consume, write response-out, clear. */
static void isp_consume(Isp *t, unsigned n)
{
    t->rs_next = (t->rs_next + n) % t->rslen;
    wr(t, REG_RESPONSE, t->rs_next);
    wr(t, REG_HCCR, HCCR_CLEAR_RISC);
}

/* One TEST UNIT READY through the whole ring round trip. */
static void isp_tur(Isp *t, uint32_t handle)
{
    uint8_t e[ENTRY_SIZE];

    entry_command(e, ENTRY_CMD64, handle, 1, 0, 0x00);
    isp_publish(t, e);
    g_assert_cmphex(rd(t, REG_ISR) & ISR_RISC, ==, ISR_RISC);
    g_assert_cmphex(rd(t, REG_SEMAPHORE), ==, 0);
    isp_response(t, t->rs_next, e);
    g_assert_cmphex(e[0], ==, ENTRY_STATUS);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, handle);
    g_assert_cmphex(lduw_le_p(e + 10), ==, COMPLETION_PORT_UNAVAILABLE);
    isp_consume(t, 1);
}

static uint32_t fc_submissions(Isp *t)
{
    QDict *rsp = qtest_qmp(t->qts,
                           "{ 'execute': 'qom-get', 'arguments': {"
                           " 'path': '" ISP_QOM_PATH "',"
                           " 'property': 'x-fc-submissions' } }");
    uint32_t val = qdict_get_int(rsp, "return");

    qobject_unref(rsp);
    return val;
}

static char *fc_last(Isp *t)
{
    QDict *rsp = qtest_qmp(t->qts,
                           "{ 'execute': 'qom-get', 'arguments': {"
                           " 'path': '" ISP_QOM_PATH "',"
                           " 'property': 'x-fc-last-submission' } }");
    char *val = g_strdup(qdict_get_str(rsp, "return"));

    qobject_unref(rsp);
    return val;
}

static void assert_fc_last_contains(Isp *t, const char *expect)
{
    g_autofree char *last = fc_last(t);

    if (!strstr(last, expect)) {
        g_test_message("last submission: %s", last);
    }
    g_assert_nonnull(strstr(last, expect));
}

static void assert_boot_image(Isp *t)
{
    unsigned i;

    g_assert_cmphex(rd(t, MBOX(0)), ==, 0x0000);
    g_assert_cmphex(rd(t, MBOX(1)), ==, 0x4953);
    g_assert_cmphex(rd(t, MBOX(2)), ==, 0x5020);
    g_assert_cmphex(rd(t, MBOX(3)), ==, 0x2020);
    g_assert_cmphex(rd(t, MBOX(7)), ==, 0x0004);
    for (i = 4; i < MBOX_COUNT; i++) {
        if (i != 7) {
            g_assert_cmphex(rd(t, MBOX(i)), ==, 0);
        }
    }
    g_assert_cmphex(rd(t, REG_HCCR) & (HCCR_RISC_RESET | HCCR_RISC_PAUSE),
                    ==, 0);
}

static void assert_held_in_reset(Isp *t)
{
    unsigned i;

    g_assert_cmphex(rd(t, MBOX(0)), ==, MBOX_BUSY);
    for (i = 1; i < MBOX_COUNT; i++) {
        g_assert_cmphex(rd(t, MBOX(i)), ==, 0);
    }
    g_assert_cmphex(rd(t, REG_HCCR) & HCCR_RISC_RESET, ==, HCCR_RISC_RESET);
}

/* The soft reset as the drivers poll it. */
static void isp_soft_reset(Isp *t)
{
    wr(t, REG_CSR, CSR_SOFT_RESET);
    g_assert_cmphex(rd(t, REG_CSR), ==, CSR_SOFT_RESET);
    g_assert_cmphex(rd(t, REG_CSR) & CSR_SOFT_RESET, ==, 0);
}

/* Vendor and device, and the model's choices where the drivers are silent. */
static void test_identity(void *obj, void *data, QGuestAllocator *alloc)
{
    QIsp2200 *isp = obj;
    QPCIDevice *dev = &isp->dev;

    g_assert_cmphex(qpci_config_readw(dev, PCI_VENDOR_ID), ==, ISP_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==, ISP_DEVICE_ID);
    /* Approximation: the Fibre Channel class; no driver reads it. */
    g_assert_cmphex(qpci_config_readw(dev, PCI_CLASS_DEVICE), ==, 0x0c04);
    /* The retail board's entry in the PCI ID list. */
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_VENDOR_ID), ==,
                    ISP_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_ID), ==, 0x0002);
    g_assert_cmphex(qpci_config_readb(dev, PCI_INTERRUPT_PIN), ==, 0x01);
}

/* BAR0 is I/O, BAR1 memory; both reach one 0x100-byte register file. */
static void test_window(void *obj, void *data, QGuestAllocator *alloc)
{
    QIsp2200 *isp = obj;
    QPCIDevice *dev = &isp->dev;
    uint64_t io_size = 0, mmio_size = 0;
    QPCIBar io, mmio;

    g_assert_cmphex(qpci_config_readl(dev, PCI_BASE_ADDRESS_0) &
                    PCI_BASE_ADDRESS_SPACE, ==, PCI_BASE_ADDRESS_SPACE_IO);
    g_assert_cmphex(qpci_config_readl(dev, PCI_BASE_ADDRESS_1) &
                    PCI_BASE_ADDRESS_SPACE, ==,
                    PCI_BASE_ADDRESS_SPACE_MEMORY);

    qpci_device_enable(dev);
    io = qpci_iomap(dev, 0, &io_size);
    mmio = qpci_iomap(dev, 1, &mmio_size);
    g_assert_cmpuint(io_size, ==, ISP_WINDOW_SIZE);
    g_assert_cmpuint(mmio_size, ==, ISP_WINDOW_SIZE);

    qpci_io_writew(dev, io, MBOX(6), 0x1357);
    g_assert_cmphex(qpci_io_readw(dev, mmio, MBOX(6)), ==, 0x1357);
    qpci_io_writew(dev, mmio, MBOX(23), 0x2468);
    g_assert_cmphex(qpci_io_readw(dev, io, MBOX(23)), ==, 0x2468);
    g_assert_cmphex(qpci_io_readw(dev, mmio, MBOX(1)), ==, 0x4953);

    qpci_iounmap(dev, io);
    qpci_iounmap(dev, mmio);
}

/*
 * Approximation: the registers no consumer gives a meaning -- the range
 * Linux leaves undescribed, the banked window, flash -- read 0 and take
 * writes without effect, logged. Includes NetBSD's DMA-engine writes
 * after a soft reset and OpenBSD's frame-buffer bank writes.
 */
static void test_undescribed_registers(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    Isp t;
    unsigned off;

    isp_open(&t, obj, alloc);
    wr(&t, 0x20, 0);
    wr(&t, 0x40, 0);
    wr(&t, 0x60, 0);
    wr(&t, REG_CSR, 0x0010);
    wr(&t, 0xb0, 0x1212);
    wr(&t, 0x96, 0x0001);
    wr(&t, REG_FLASH_ADDR, 0x5555);
    wr(&t, REG_FLASH_DATA, 0xaaaa);

    for (off = 0x20; off < 0xc0; off += 2) {
        g_assert_cmphex(rd(&t, off), ==, 0);
    }
    g_assert_cmphex(rd(&t, REG_FLASH_ADDR), ==, 0);
    g_assert_cmphex(rd(&t, REG_FLASH_DATA), ==, 0);
    g_assert_cmphex(rd(&t, 0x04), ==, 0);
    g_assert_cmphex(rd(&t, REG_CSR), ==, 0x0010);
    wr(&t, REG_CSR, 0);

    /* Nothing else moved: the boot image is still there. */
    assert_boot_image(&t);

    /* The GPIO pair is storage only. */
    wr(&t, REG_GPIO_DATA, 0x00c0);
    g_assert_cmphex(rd(&t, REG_GPIO_DATA), ==, 0x00c0);
    isp_close(&t);
}

/*
 * The IRIX 6.5.30 driver's soft reset, as it ran on the model: pause the
 * RISC, write 0x0004 to three DMA-engine control registers, set CSR bit
 * 0, then read HCCR (never CSR) until bit 6 clears, with no HCCR write
 * first.
 */
static void isp_soft_reset_hccr_poll(Isp *t)
{
    wr(t, REG_HCCR, HCCR_PAUSE);
    g_assert_cmphex(rd(t, REG_HCCR) & HCCR_RISC_PAUSE, ==, HCCR_RISC_PAUSE);
    wr(t, 0x40, 0x0004);
    wr(t, 0x60, 0x0004);
    wr(t, 0x20, 0x0004);
    wr(t, REG_CSR, CSR_SOFT_RESET);
    g_assert_cmphex(rd(t, REG_HCCR), ==, HCCR_RISC_RESET);
    g_assert_cmphex(rd(t, REG_HCCR), ==, 0);
}

/*
 * Approximation: the reset states. A device reset leaves the ROM code
 * running and its image readable. A soft reset ends at the first
 * register read after it, or at an HCCR write, with the ROM code
 * running and the image readable; only that first read, of CSR or of
 * HCCR, still shows the reset in progress. An HCCR reset holds the RISC
 * until an explicit release. The device reset checked is the system
 * reset the qos runner issues between cases, after earlier cases left
 * the adapter running firmware with live rings; a reset issued inside a
 * case would also clear the Bridge test head's I/O window, which only
 * the runner rebuilds.
 */
static void test_reset_states(void *obj, void *data, QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    assert_boot_image(&t);

    /* A release sent to a running RISC changes nothing. */
    wr(&t, REG_HCCR, HCCR_RELEASE);
    assert_boot_image(&t);

    /* IRIX's order: bit 6 reads set once, then the image, not paused. */
    isp_soft_reset_hccr_poll(&t);
    assert_boot_image(&t);
    g_assert_cmphex(rd(&t, REG_CSR) & CSR_SOFT_RESET, ==, 0);

    /* The public drivers' order: CSR polled, then their own hold. */
    isp_soft_reset(&t);
    g_assert_cmphex(rd(&t, REG_HCCR) & HCCR_RISC_RESET, ==, 0);
    wr(&t, REG_HCCR, HCCR_RESET);
    assert_held_in_reset(&t);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    assert_boot_image(&t);

    /* A first read of a mailbox ends the reset too, with the image. */
    wr(&t, REG_CSR, CSR_SOFT_RESET);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, 0);
    g_assert_cmphex(rd(&t, REG_HCCR), ==, 0);
    g_assert_cmphex(rd(&t, REG_CSR), ==, 0);
    assert_boot_image(&t);

    /* qlogicfc's order: HCCR reset and release with no read between. */
    wr(&t, REG_CSR, CSR_SOFT_RESET);
    wr(&t, REG_HCCR, HCCR_RESET);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    g_assert_cmphex(rd(&t, REG_HCCR), ==, 0);
    g_assert_cmphex(rd(&t, REG_CSR), ==, 0);
    assert_boot_image(&t);
    isp_close(&t);
}

/*
 * Approximation: either soft-reset order, followed by HCCR reset,
 * semaphore 0 and release, a download and Execute, reaches running
 * firmware. A model that holds bit 6 after a soft reset fails the HCCR
 * poll; one that skips the hold after HCCR 0x1000 fails the CSR order.
 */
static void test_soft_reset_orders(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    static const uint16_t pattern[] = {
        CMD_REGISTER_TEST, 0xaaaa, 0x5555, 0xaa55, 0x55aa, 0xa5a5,
        0x5a5a, 0x2525,
    };
    unsigned order, i;
    Isp t;

    isp_open(&t, obj, alloc);
    for (order = 0; order < 2; order++) {
        if (order == 0) {
            isp_soft_reset_hccr_poll(&t);
            g_assert_cmphex(rd(&t, MBOX(0)), ==, 0);
        } else {
            isp_soft_reset(&t);
        }
        wr(&t, REG_HCCR, HCCR_RESET);
        g_assert_cmphex(rd(&t, REG_HCCR), ==, HCCR_RISC_RESET);
        g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_BUSY);
        wr(&t, REG_SEMAPHORE, 0);
        wr(&t, REG_HCCR, HCCR_RELEASE);
        if (order == 0) {
            wr(&t, REG_HCCR, HCCR_PARITY_PAUSE);
        }
        assert_boot_image(&t);

        isp_start(&t);
        g_assert_cmphex(isp_call_ack(&t, CMD_NOP, 0, 0, 0), ==,
                        MBOX_COMPLETE);
        g_assert_cmphex(isp_mbox(&t, pattern, ARRAY_SIZE(pattern)), ==,
                        MBOX_COMPLETE);
        for (i = 1; i < ARRAY_SIZE(pattern); i++) {
            g_assert_cmphex(rd(&t, MBOX(i)), ==, pattern[i]);
        }
        isp_ack(&t);
        /* The firmware runs: a ROM-subset reset did not hide it. */
        g_assert_cmphex(isp_call_ack(&t, CMD_GET_FW_STATE, 0, 0, 0), ==,
                        MBOX_COMPLETE);
    }
    isp_close(&t);
}

/* Soft reset clears the interface, the rings and the in-flight work. */
static void test_soft_reset_clears(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    Isp t;
    uint8_t e[ENTRY_SIZE];

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 8);
    wr(&t, REG_ICR, ICR_ENABLE);

    /* One completion posted and unread, a second held behind its latch. */
    entry_command(e, ENTRY_CMD64, 0x1111, 1, 0, 0);
    isp_publish(&t, e);
    entry_command(e, ENTRY_CMD64, 0x2222, 1, 0, 0);
    isp_publish(&t, e);
    g_assert_cmphex(rd(&t, REG_ISR), ==, ISR_PENDING | ISR_RISC);

    isp_soft_reset(&t);
    g_assert_cmphex(rd(&t, REG_ICR), ==, 0);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, 0);
    g_assert_cmphex(rd(&t, REG_NVRAM), ==, 0);
    wr(&t, REG_HCCR, HCCR_RELEASE);

    /* The rings are gone: mailbox 4 is plain storage again. */
    wr(&t, REG_REQUEST, 0x55aa);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 0x55aa);

    /* The firmware is gone too, and comes back with a download. */
    g_assert_cmphex(isp_call_ack(&t, CMD_GET_FW_STATE, 0, 0, 0), ==,
                    MBOX_INVALID);
    isp_start(&t);
    isp_rings(&t, 8, 8);
    /* The held completion did not survive into the new ring. */
    isp_response(&t, 0, e);
    g_assert_cmphex(e[0], ==, 0);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    isp_tur(&t, 0x3333);
    isp_close(&t);
}

/* HCCR reset holds, release boots, pause defers a mailbox command. */
static void test_risc_control(void *obj, void *data, QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    wr(&t, REG_HCCR, HCCR_RESET);
    assert_held_in_reset(&t);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    assert_boot_image(&t);

    wr(&t, REG_HCCR, HCCR_PAUSE);
    g_assert_cmphex(rd(&t, REG_HCCR), ==, HCCR_RISC_PAUSE);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_BUSY);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 0x4953);

    /* Issued while paused, the command waits. */
    g_assert_cmphex(isp_call(&t, CMD_NOP, 0, 0, 0), ==, CMD_NOP);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, 0);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);

    wr(&t, REG_HCCR, HCCR_RELEASE);
    g_assert_cmphex(rd(&t, REG_HCCR) & HCCR_RISC_PAUSE, ==, 0);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, SEM_LOCK);
    isp_ack(&t);
    isp_close(&t);
}

/*
 * The firmware clears the doorbell when it takes the command, so a driver
 * that waits for bit 7 to drop (the stock PROM's) never has to write the
 * clear command. A command that cannot run yet keeps the bit set, and
 * the clear command still clears it.
 */
static void test_host_interrupt(void *obj, void *data, QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    isp_call(&t, CMD_NOP, 0, 0, 0);
    g_assert_cmphex(rd(&t, REG_HCCR) & HCCR_HOST_INT, ==, 0);
    isp_ack(&t);

    wr(&t, REG_HCCR, HCCR_PAUSE);
    isp_call(&t, CMD_NOP, 0, 0, 0);
    g_assert_cmphex(rd(&t, REG_HCCR) & HCCR_HOST_INT, ==, HCCR_HOST_INT);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    g_assert_cmphex(rd(&t, REG_HCCR) & HCCR_HOST_INT, ==, 0);
    isp_ack(&t);

    wr(&t, REG_HCCR, HCCR_PAUSE);
    isp_call(&t, CMD_NOP, 0, 0, 0);
    wr(&t, REG_HCCR, HCCR_CLEAR_HOST);
    g_assert_cmphex(rd(&t, REG_HCCR) & HCCR_HOST_INT, ==, 0);
    isp_close(&t);
}

static void snapshot_mailboxes(Isp *t, uint16_t *mb)
{
    unsigned i;

    for (i = 0; i < MBOX_COUNT; i++) {
        mb[i] = rd(t, MBOX(i));
    }
}

static void assert_mailboxes(Isp *t, const uint16_t *mb)
{
    unsigned i;

    for (i = 0; i < MBOX_COUNT; i++) {
        g_assert_cmphex(rd(t, MBOX(i)), ==, mb[i]);
    }
}

/*
 * Approximation: host commands without a model, and 0x4000/0x4001 which
 * the drivers write between reset and release, change neither the HCCR
 * bits nor a mailbox, running or held.
 */
static void test_unmodelled_host_commands(void *obj, void *data,
                                          QGuestAllocator *alloc)
{
    static const uint16_t cmds[] = {
        0xa000, 0xe000, 0x8000, 0x9000, 0xf000, HCCR_PARITY4,
        HCCR_PARITY_PAUSE,
    };
    uint16_t mb[MBOX_COUNT];
    uint16_t hccr;
    unsigned i;
    Isp t;

    isp_open(&t, obj, alloc);
    snapshot_mailboxes(&t, mb);
    hccr = rd(&t, REG_HCCR);
    for (i = 0; i < ARRAY_SIZE(cmds); i++) {
        wr(&t, REG_HCCR, cmds[i]);
        g_assert_cmphex(rd(&t, REG_HCCR), ==, hccr);
        assert_mailboxes(&t, mb);
    }

    /* OpenBSD's order: soft reset, reset, semaphore, 0x4000, release. */
    isp_soft_reset(&t);
    wr(&t, REG_HCCR, HCCR_RESET);
    wr(&t, REG_SEMAPHORE, 0);
    wr(&t, REG_HCCR, HCCR_PARITY4);
    assert_held_in_reset(&t);
    g_assert_cmphex(rd(&t, REG_HCCR) & HCCR_RISC_PAUSE, ==, 0);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    assert_boot_image(&t);

    /* NetBSD's: reset, 0x4001, release. */
    wr(&t, REG_HCCR, HCCR_RESET);
    wr(&t, REG_HCCR, HCCR_PARITY_PAUSE);
    assert_held_in_reset(&t);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    assert_boot_image(&t);
    isp_close(&t);
}

/* Echoes m1..m7 for Linux's pattern and another, before any queue. */
static void test_register_test(void *obj, void *data, QGuestAllocator *alloc)
{
    static const uint16_t patterns[2][8] = {
        { CMD_REGISTER_TEST, 0xaaaa, 0x5555, 0xaa55, 0x55aa, 0xa5a5,
          0x5a5a, 0x2525 },
        { CMD_REGISTER_TEST, 0x0102, 0x0304, 0x0506, 0x0708, 0x090a,
          0x0b0c, 0x0d0e },
    };
    unsigned p, i;
    Isp t;

    isp_open(&t, obj, alloc);
    for (p = 0; p < 2; p++) {
        g_assert_cmphex(isp_mbox(&t, patterns[p], 8), ==, MBOX_COMPLETE);
        for (i = 1; i < 8; i++) {
            g_assert_cmphex(rd(&t, MBOX(i)), ==, patterns[p][i]);
        }
        isp_ack(&t);
    }
    isp_close(&t);
}

/*
 * Approximation, mailbox storage and the queue pointers: every mailbox
 * reads back before INIT_FIRMWARE. After it, mailbox 4 and 5 read the
 * adapter's pointers while the semaphore is free and a result's data
 * while it is held. No mailbox write starts a command.
 */
static void test_mailbox_storage(void *obj, void *data, QGuestAllocator *alloc)
{
    unsigned i;
    Isp t;

    isp_open(&t, obj, alloc);
    for (i = 0; i < MBOX_COUNT; i++) {
        wr(&t, MBOX(i), 0x1000 + i);
    }
    for (i = 0; i < MBOX_COUNT; i++) {
        g_assert_cmphex(rd(&t, MBOX(i)), ==, 0x1000 + i);
    }
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, 0);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);

    isp_start(&t);
    isp_rings(&t, 8, 8);
    isp_tur(&t, 0x77);

    for (i = 0; i < MBOX_COUNT; i++) {
        if (i != 4 && i != 5) {
            wr(&t, MBOX(i), 0x2000 + i);
        }
    }
    for (i = 0; i < MBOX_COUNT; i++) {
        if (i != 4 && i != 5) {
            g_assert_cmphex(rd(&t, MBOX(i)), ==, 0x2000 + i);
        }
    }
    /* The window is open (mailbox 0 was written): these are inputs. */
    wr(&t, REG_REQUEST, 0x0bad);
    wr(&t, REG_RESPONSE, 0x0bad);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 1);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 1);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, 0);

    /* With a result held, the two mailboxes are its data: zero here. */
    wr(&t, MBOX(0), CMD_NOP);
    wr(&t, REG_HCCR, HCCR_SET_HOST);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, SEM_LOCK);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 0);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 0);
    isp_ack(&t);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 1);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 1);
    isp_close(&t);
}

/*
 * A completion: status, semaphore, ISR. The line follows the latch with
 * both enables set (ISR bit 15 mirrors the line) and stays low with both
 * clear; the single-enable cases are not pinned.
 */
static void test_completion_interrupt(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    wr(&t, REG_ICR, ICR_ENABLE);
    g_assert_cmphex(rd(&t, REG_ICR), ==, ICR_ENABLE);
    g_assert_cmphex(isp_call(&t, CMD_NOP, 0, 0, 0), ==, MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, SEM_LOCK);
    g_assert_cmphex(rd(&t, REG_ISR), ==, ISR_PENDING | ISR_RISC);

    wr(&t, REG_HCCR, HCCR_CLEAR_RISC);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, SEM_LOCK);
    wr(&t, REG_SEMAPHORE, 0);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, 0);

    /* Both enables clear: the latch sets, the line does not. */
    wr(&t, REG_ICR, 0);
    g_assert_cmphex(isp_call(&t, CMD_NOP, 0, 0, 0), ==, MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, REG_ISR), ==, ISR_RISC);
    isp_ack(&t);
    isp_close(&t);
}

/*
 * Approximation: a second result waits until both the semaphore and
 * the latch are released, then appears intact. The first stays readable
 * under the second command's writes until then.
 */
static void test_second_result_waits(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    const uint16_t second[] = {
        CMD_REGISTER_TEST, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666,
        0x7777,
    };
    unsigned i;
    Isp t;

    isp_open(&t, obj, alloc);
    g_assert_cmphex(isp_call(&t, CMD_ABOUT_FIRMWARE, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(isp_mbox(&t, second, ARRAY_SIZE(second)), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 1);

    wr(&t, REG_HCCR, HCCR_CLEAR_RISC);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    wr(&t, REG_SEMAPHORE, 0);

    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, SEM_LOCK);
    g_assert_cmphex(rd(&t, REG_ISR), ==, ISR_RISC);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    for (i = 1; i < ARRAY_SIZE(second); i++) {
        g_assert_cmphex(rd(&t, MBOX(i)), ==, second[i]);
    }
    isp_ack(&t);
    isp_close(&t);
}

/*
 * Approximation: results are never dropped. One completion waits behind
 * a posted result; a doorbell that finds that slot taken stays latched in
 * HCCR bit 7 and runs once the slot empties, on the mailbox inputs as they
 * are then. Ringing again before that runs one command, not two.
 */
static void test_doorbell_waits_for_slot(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    uint16_t in[8];
    unsigned i, round;
    Isp t;

    isp_open(&t, obj, alloc);
    g_assert_cmphex(isp_call(&t, CMD_NOP, 0, 0, 0), ==, MBOX_COMPLETE);

    /* Rounds 0..5: round 0 takes the slot, the rest wait on the latch. */
    for (round = 0; round < 6; round++) {
        in[0] = CMD_REGISTER_TEST;
        for (i = 1; i < ARRAY_SIZE(in); i++) {
            in[i] = (round << 8) | i;
        }
        isp_mbox(&t, in, ARRAY_SIZE(in));
        /* Round 0 is taken at once; the others find the slot full. */
        g_assert_cmphex(rd(&t, REG_HCCR) & HCCR_HOST_INT, ==,
                        round ? HCCR_HOST_INT : 0);
    }

    /* The posted No-op's release posts round 0 intact. */
    isp_ack(&t);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, SEM_LOCK);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    for (i = 1; i < ARRAY_SIZE(in); i++) {
        g_assert_cmphex(rd(&t, MBOX(i)), ==, i);
    }

    /* Then the latched doorbell, which ran on round 5's inputs. */
    isp_ack(&t);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, SEM_LOCK);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    for (i = 1; i < ARRAY_SIZE(in); i++) {
        g_assert_cmphex(rd(&t, MBOX(i)), ==, (5 << 8) | i);
    }

    isp_ack(&t);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, 0);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    g_assert_cmphex(isp_call_ack(&t, CMD_NOP, 0, 0, 0), ==, MBOX_COMPLETE);
    isp_close(&t);
}

/* Approximation: a semaphore write of 1 changes nothing; bit 1 reads 0. */
static void test_semaphore_write_one(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    wr(&t, REG_SEMAPHORE, 1);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, 0);
    wr(&t, REG_SEMAPHORE, 3);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, 0);

    isp_call(&t, CMD_NOP, 0, 0, 0);
    wr(&t, REG_SEMAPHORE, 1);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, SEM_LOCK);
    isp_ack(&t);
    isp_close(&t);
}

/*
 * Approximation: unknown and deferred commands answer 0x4001 and leave
 * the running firmware alone, as do the commands the loop service does
 * not model: abort device, bus reset, target reset, login, enhanced port
 * database.
 */
static void test_unknown_commands(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    g_assert_cmphex(isp_call_ack(&t, 0x00ee, 0, 0, 0), ==, MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, 0x0016, 0, 0, 0), ==, MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, 0x0018, 0, 0, 0), ==, MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, 0x0066, 0, 0, 0), ==, MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, CMD_RESOURCE_COUNTS, 0, 0, 0), ==,
                    MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, 0x006f, 0, 0, 0), ==, MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, CMD_ENH_PORT_DB, 1, 0, 0), ==,
                    MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, CMD_GET_FW_STATE, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    isp_close(&t);
}

/*
 * Approximation: before execute only the ROM subset answers; after
 * it Get Firmware State reports configuration wait, never ready.
 */
static void test_rom_subset(void *obj, void *data, QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    g_assert_cmphex(isp_call_ack(&t, CMD_GET_FW_STATE, 0, 0, 0), ==,
                    MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, CMD_FIRMWARE_STATUS, 0, 0, 0), ==,
                    MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, CMD_INIT_FIRMWARE, 0, 0, 0), ==,
                    MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, CMD_SET_FW_OPTIONS, 0, 0, 0), ==,
                    MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, CMD_NOP, 0, 0, 0), ==, MBOX_COMPLETE);
    g_assert_cmphex(isp_call_ack(&t, CMD_ABOUT_FIRMWARE, 0, 0, 0), ==,
                    MBOX_COMPLETE);

    isp_start(&t);
    g_assert_cmphex(isp_call(&t, CMD_GET_FW_STATE, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 0);
    isp_ack(&t);
    isp_close(&t);
}

static uint16_t isp_load_ram(Isp *t, uint16_t cmd, uint16_t base,
                             uint64_t host, uint16_t count, uint16_t m8)
{
    uint16_t in[9] = {
        cmd, base, (host >> 16) & 0xffff, host & 0xffff, count, 0,
        (host >> 48) & 0xffff, (host >> 32) & 0xffff, m8,
    };

    return isp_mbox(t, in, ARRAY_SIZE(in));
}

static uint16_t isp_read_ram(Isp *t, uint16_t addr)
{
    uint16_t val;

    g_assert_cmphex(isp_call(t, CMD_READ_RAM_WORD, addr, 0, 0), ==,
                    MBOX_COMPLETE);
    val = rd(t, MBOX(2));
    isp_ack(t);
    return val;
}

/*
 * The short Load RAM, which the stock PROM uses before any firmware
 * runs, takes its host address from m2/m3 alone and works from the
 * boot ROM.
 */
static void test_load_ram_32(void *obj, void *data, QGuestAllocator *alloc)
{
    const uint8_t bytes[] = { 0x34, 0x12, 0x78, 0x56 };
    uint64_t src;
    uint16_t in[8];
    Isp t;

    isp_open(&t, obj, alloc);
    src = guest_alloc(alloc, sizeof(bytes));
    g_assert_cmphex(src >> 32, ==, 0);
    qtest_memwrite(t.qts, src, bytes, sizeof(bytes));

    in[0] = CMD_LOAD_RAM_32;
    in[1] = 0x1000;
    in[2] = src >> 16;
    in[3] = src & 0xffff;
    in[4] = 2;
    /* Left over from earlier commands; the short form must not use them. */
    in[5] = 0;
    in[6] = 0xabcd;
    in[7] = 0x1234;
    g_assert_cmphex(isp_mbox(&t, in, ARRAY_SIZE(in)), ==, MBOX_COMPLETE);
    isp_ack(&t);
    g_assert_cmphex(isp_read_ram(&t, 0x1000), ==, 0x1234);
    g_assert_cmphex(isp_read_ram(&t, 0x1001), ==, 0x5678);
    isp_close(&t);
}

/*
 * Dump RAM, which the stock PROM's DMA self-test runs against the boot ROM
 * before any firmware: the words Load RAM stored come back as the same
 * little-endian bytes, and a range past RAM is refused.
 */
static void test_dump_ram(void *obj, void *data, QGuestAllocator *alloc)
{
    const uint8_t bytes[] = { 0x34, 0x12, 0x78, 0x56, 0xbc, 0x9a, 0xf0, 0xde };
    uint8_t back[sizeof(bytes)] = { 0 };
    uint64_t src, dst;
    Isp t;

    isp_open(&t, obj, alloc);
    src = guest_alloc(alloc, sizeof(bytes));
    dst = guest_alloc(alloc, sizeof(back));
    qtest_memwrite(t.qts, src, bytes, sizeof(bytes));
    qtest_memwrite(t.qts, dst, back, sizeof(back));

    g_assert_cmphex(isp_load_ram(&t, CMD_LOAD_RAM, 0x1000, src, 4, 0), ==,
                    MBOX_COMPLETE);
    isp_ack(&t);
    g_assert_cmphex(isp_load_ram(&t, CMD_DUMP_RAM, 0x1000, dst, 4, 0), ==,
                    MBOX_COMPLETE);
    isp_ack(&t);
    qtest_memread(t.qts, dst, back, sizeof(back));
    g_assert_cmpmem(back, sizeof(back), bytes, sizeof(bytes));

    g_assert_cmphex(isp_load_ram(&t, CMD_DUMP_RAM, 0xfffe, dst, 4, 0), ==,
                    MBOX_PARAM_ERROR);
    isp_ack(&t);
    isp_close(&t);
}

/*
 * Approximation: Load RAM moves little-endian words; a refused DMA
 * changes nothing; a range past RAM is refused.
 */
static void test_load_ram(void *obj, void *data, QGuestAllocator *alloc)
{
    const uint8_t bytes[] = { 0x34, 0x12, 0x78, 0x56, 0xbc, 0x9a, 0xf0, 0xde };
    uint64_t src;
    Isp t;

    isp_open(&t, obj, alloc);
    src = guest_alloc(alloc, sizeof(bytes));
    qtest_memwrite(t.qts, src, bytes, sizeof(bytes));

    g_assert_cmphex(isp_load_ram(&t, CMD_LOAD_RAM, 0x2000, src, 4, 0), ==,
                    MBOX_COMPLETE);
    isp_ack(&t);
    g_assert_cmphex(isp_read_ram(&t, 0x2000), ==, 0x1234);
    g_assert_cmphex(isp_read_ram(&t, 0x2001), ==, 0x5678);
    g_assert_cmphex(isp_read_ram(&t, 0x2003), ==, 0xdef0);

    if (is_mips64()) {
        g_assert_cmphex(isp_load_ram(&t, CMD_LOAD_RAM, 0x2000,
                                     UNMAPPED_PHYS, 4, 0), ==,
                        MBOX_HOST_IF_ERROR);
        isp_ack(&t);
        g_assert_cmphex(isp_read_ram(&t, 0x2000), ==, 0x1234);
        g_assert_cmphex(isp_read_ram(&t, 0x2003), ==, 0xdef0);
    }

    g_assert_cmphex(isp_load_ram(&t, CMD_LOAD_RAM, 0xfffe, src, 4, 0), ==,
                    MBOX_PARAM_ERROR);
    isp_ack(&t);
    g_assert_cmphex(isp_read_ram(&t, 0xfffe), ==, 0);

    /* Approximation: the extended load does not reach past 64K words. */
    g_assert_cmphex(isp_load_ram(&t, CMD_LOAD_RAM_EXT, 0x3000, src, 4, 1),
                    ==, MBOX_PARAM_ERROR);
    isp_ack(&t);
    g_assert_cmphex(isp_read_ram(&t, 0x3000), ==, 0);
    g_assert_cmphex(isp_load_ram(&t, CMD_LOAD_RAM_EXT, 0x3000, src, 4, 0),
                    ==, MBOX_COMPLETE);
    isp_ack(&t);
    g_assert_cmphex(isp_read_ram(&t, 0x3000), ==, 0x1234);
    isp_close(&t);
}

/*
 * The four address mailboxes place the read at a 64-bit address. The
 * Bridge test head moves its transfer memory above 4 GiB and is told to
 * pass the device's addresses through unchanged.
 */
static void test_load_ram_high(void *obj, void *data, QGuestAllocator *alloc)
{
    const uint8_t bytes[] = { 0xcd, 0xab, 0x01, 0xef };
    uint64_t bridge_device = 0x40000000ULL + 4 + 0x200 + 8 * 4;
    uint64_t src;
    Isp t;

    if (!is_mips64()) {
        g_test_skip("high memory belongs to the SGI Bridge test head");
        return;
    }
    isp_open(&t, obj, alloc);
    src = guest_alloc(alloc, sizeof(bytes)) + (1ULL << 32);
    qtest_sysbus_mmio_map(t.qts, "/machine/peripheral/transfer-ram", 0,
                          0x102000000ULL);
    qtest_writel(t.qts, bridge_device,
                 qtest_readl(t.qts, bridge_device) | (1u << 19));
    qtest_memwrite(t.qts, src, bytes, sizeof(bytes));

    /* Mailbox 6 counts: the same low 48 bits under widget 0xe fail. */
    g_assert_cmphex(isp_load_ram(&t, CMD_LOAD_RAM, 0x2000,
                                 HIGH_WORD_WIDGET | src, 2, 0), ==,
                    MBOX_HOST_IF_ERROR);
    isp_ack(&t);
    g_assert_cmphex(isp_read_ram(&t, 0x2000), ==, 0);

    g_assert_cmphex(isp_load_ram(&t, CMD_LOAD_RAM, 0x2000, src, 2, 0), ==,
                    MBOX_COMPLETE);
    isp_ack(&t);
    g_assert_cmphex(isp_read_ram(&t, 0x2000), ==, 0xabcd);
    g_assert_cmphex(isp_read_ram(&t, 0x2001), ==, 0xef01);
    isp_close(&t);
}

/*
 * Write RAM word loads one word. With 64K words and a 16-bit address in
 * mailbox 1, no Write RAM word address lies past RAM, so that refusal has
 * no input to test.
 */
static void test_write_ram_word(void *obj, void *data, QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    g_assert_cmphex(isp_call_ack(&t, CMD_WRITE_RAM_WORD, 0xffff, 0xbeef, 0),
                    ==, MBOX_COMPLETE);
    g_assert_cmphex(isp_read_ram(&t, 0xffff), ==, 0xbeef);
    g_assert_cmphex(isp_read_ram(&t, 0x0000), ==, 0);
    isp_close(&t);
}

/*
 * Approximation: the sum over the word-3 length must be zero; a
 * header or span past RAM is refused, leaving mailbox 1 as written.
 */
static void test_verify_checksum(void *obj, void *data, QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    isp_load_image(&t);
    g_assert_cmphex(isp_call(&t, CMD_VERIFY_CHECKSUM, CODE_ORG, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 0);
    g_assert_cmphex(rd(&t, MBOX(2)), ==, 0);
    isp_ack(&t);

    g_assert_cmphex(isp_call_ack(&t, CMD_WRITE_RAM_WORD, CODE_ORG + 5,
                                 image_word(5) ^ 0x0100, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(isp_call_ack(&t, CMD_VERIFY_CHECKSUM, CODE_ORG, 0, 0),
                    ==, MBOX_TEST_FAILED);

    /* A length that runs past the end of RAM. */
    g_assert_cmphex(isp_call_ack(&t, CMD_WRITE_RAM_WORD, 0xfff3, 0x0020, 0),
                    ==, MBOX_COMPLETE);
    g_assert_cmphex(isp_call(&t, CMD_VERIFY_CHECKSUM, 0xfff0, 0, 0), ==,
                    MBOX_PARAM_ERROR);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 0xfff0);
    isp_ack(&t);
    /* A header that does not fit. */
    g_assert_cmphex(isp_call_ack(&t, CMD_VERIFY_CHECKSUM, 0xfffd, 0, 0), ==,
                    MBOX_PARAM_ERROR);
    isp_close(&t);
}

/*
 * Approximation: the fixed identity -- a model version,
 * attributes 0x0002 (expanded LUN; no fabric 0x0004, no 2K logins
 * 0x0100) -- and a budget of 512 commands.
 */
static void test_firmware_identity(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    g_assert_cmphex(isp_call(&t, CMD_ABOUT_FIRMWARE, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 1);
    g_assert_cmphex(rd(&t, MBOX(2)), ==, 0);
    g_assert_cmphex(rd(&t, MBOX(3)), ==, 0);
    g_assert_cmphex(rd(&t, MBOX(6)), ==, 0x0002);
    g_assert_cmphex(rd(&t, MBOX(6)) & 0x0104, ==, 0);
    isp_ack(&t);
    g_assert_cmphex(isp_call(&t, CMD_FIRMWARE_STATUS, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmpuint(rd(&t, MBOX(2)), ==, 512);
    isp_ack(&t);
    isp_close(&t);
}

/* Approximation: firmware options are stored and change nothing. */
static void test_firmware_options(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    g_assert_cmphex(isp_call(&t, CMD_GET_FW_OPTIONS, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 0);
    isp_ack(&t);
    g_assert_cmphex(isp_call_ack(&t, CMD_SET_FW_OPTIONS, 0x0002, 0x1234,
                                 0x5678), ==, MBOX_COMPLETE);
    g_assert_cmphex(isp_call(&t, CMD_GET_FW_OPTIONS, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 0x0002);
    g_assert_cmphex(rd(&t, MBOX(2)), ==, 0x1234);
    g_assert_cmphex(rd(&t, MBOX(3)), ==, 0x5678);
    isp_ack(&t);

    isp_soft_reset(&t);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    isp_start(&t);
    g_assert_cmphex(isp_call(&t, CMD_GET_FW_OPTIONS, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 0);
    g_assert_cmphex(rd(&t, MBOX(2)), ==, 0);
    g_assert_cmphex(rd(&t, MBOX(3)), ==, 0);
    isp_ack(&t);
    isp_close(&t);
}

static void assert_queues_uninitialised(Isp *t)
{
    wr(t, REG_REQUEST, 0x1234);
    g_assert_cmphex(rd(t, REG_REQUEST), ==, 0x1234);
}

static uint16_t try_icb(Isp *t, uint64_t icb, const uint8_t *buf)
{
    uint16_t status;

    qtest_memwrite(t->qts, icb, buf, ICB_SIZE);
    status = isp_init_firmware(t, icb);
    isp_ack(t);
    return status;
}

/*
 * Approximation: each rejection leaves the queues uninitialised.
 * Precedence, which the sources leave open, is the model's: an unreadable
 * block first, then the field checks.
 */
static void test_init_firmware_rejects(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    uint64_t icb, rq, rs;
    uint8_t buf[ICB_SIZE];
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    icb = guest_alloc(alloc, ICB_SIZE);
    rq = guest_alloc(alloc, 8 * ENTRY_SIZE);
    rs = guest_alloc(alloc, 8 * ENTRY_SIZE);

    icb_build(buf, 8, 8, rq, rs, 0);
    buf[0] = 2;
    g_assert_cmphex(try_icb(&t, icb, buf), ==, MBOX_PARAM_ERROR);
    assert_queues_uninitialised(&t);

    icb_build(buf, 0, 8, rq, rs, 0);
    g_assert_cmphex(try_icb(&t, icb, buf), ==, MBOX_PARAM_ERROR);
    assert_queues_uninitialised(&t);

    icb_build(buf, 8, 0, rq, rs, 0);
    g_assert_cmphex(try_icb(&t, icb, buf), ==, MBOX_PARAM_ERROR);
    assert_queues_uninitialised(&t);

    icb_build(buf, 8, 8, 0, rs, 0);
    g_assert_cmphex(try_icb(&t, icb, buf), ==, MBOX_PARAM_ERROR);
    assert_queues_uninitialised(&t);

    icb_build(buf, 8, 8, rq, rs, ICB_OPT_FAST_POST);
    g_assert_cmphex(try_icb(&t, icb, buf), ==, MBOX_PARAM_ERROR);
    assert_queues_uninitialised(&t);

    if (is_mips64()) {
        g_assert_cmphex(isp_init_firmware(&t, UNMAPPED_PHYS), ==,
                        MBOX_HOST_IF_ERROR);
        isp_ack(&t);
        assert_queues_uninitialised(&t);
    }

    icb_build(buf, 8, 8, rq, rs, 0);
    g_assert_cmphex(try_icb(&t, icb, buf), ==, MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 0);
    isp_close(&t);
}

/* The block, both rings and every address mailbox above 4 GiB. */
static void test_init_firmware_high(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    uint64_t bridge_device = 0x40000000ULL + 4 + 0x200 + 8 * 4;
    uint64_t base;
    Isp t;

    if (!is_mips64()) {
        g_test_skip("high memory belongs to the SGI Bridge test head");
        return;
    }
    isp_open(&t, obj, alloc);
    isp_start(&t);
    base = guest_alloc(alloc, 32 * ENTRY_SIZE) + (1ULL << 32);
    qtest_sysbus_mmio_map(t.qts, "/machine/peripheral/transfer-ram", 0,
                          0x102000000ULL);
    qtest_writel(t.qts, bridge_device,
                 qtest_readl(t.qts, bridge_device) | (1u << 19));

    /* Mailbox 6 counts: the same block under widget 0xe is unreadable. */
    {
        uint8_t buf[ICB_SIZE];

        icb_build(buf, 4, 4, base + 8 * ENTRY_SIZE, base + 16 * ENTRY_SIZE,
                  0);
        qtest_memwrite(t.qts, base, buf, sizeof(buf));
        g_assert_cmphex(isp_init_firmware(&t, HIGH_WORD_WIDGET | base), ==,
                        MBOX_HOST_IF_ERROR);
        isp_ack(&t);
        assert_queues_uninitialised(&t);
    }

    isp_rings_at(&t, 4, 4, base, base + 8 * ENTRY_SIZE,
                 base + 16 * ENTRY_SIZE, 0, 0);
    isp_tur(&t, 0xfeed0001);
    isp_close(&t);
}

/*
 * Approximation: Linux asks for reduced-interrupt mode 4. It is
 * accepted and ignored: completions are ordinary 0x03 status entries.
 */
static void test_rio_mode_ignored(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings_at(&t, 8, 8, guest_alloc(alloc, ICB_SIZE),
                 guest_alloc(alloc, 8 * ENTRY_SIZE),
                 guest_alloc(alloc, 8 * ENTRY_SIZE), 0, 4);
    isp_tur(&t, 0x40);
    isp_tur(&t, 0x41);
    isp_close(&t);
}

static void put_seg64(uint8_t *p, uint64_t addr, uint32_t len)
{
    stq_le_p(p, addr);
    stl_le_p(p + 8, len);
}

static void put_seg32(uint8_t *p, uint32_t addr, uint32_t len)
{
    stl_le_p(p, addr);
    stl_le_p(p + 4, len);
}

/*
 * A 64-bit command reaches the service unchanged and comes back as a
 * whole 64-byte status entry: every byte of the slot is written, so
 * Linux's signature in bytes 60..63 is overwritten. No service is
 * attached, so the completion is "port unavailable".
 */
static void test_command_round_trip(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE], fill[ENTRY_SIZE];
    unsigned i;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 8);
    memset(fill, 0xde, sizeof(fill));
    qtest_memwrite(t.qts, t.rs, fill, sizeof(fill));

    /* A READ(10) whose every CDB byte differs, and a byte count. */
    entry_command(e, ENTRY_CMD64, 0xcafe0001, 5, 2, 0x28);
    stw_le_p(e + 12, 0x0028);
    for (i = 1; i < 16; i++) {
        e[20 + i] = 0xa0 + i;
    }
    stl_le_p(e + 36, 0x12345);
    stw_le_p(e + 18, 1);
    put_seg64(e + 40, 0x02100000, 0x12345);
    isp_publish(&t, e);

    g_assert_cmpuint(fc_submissions(&t), ==, 1);
    assert_fc_last_contains(&t, "handle=cafe0001 loop=5 lun=2 control=0028 "
                            "cdb=28a1a2a3a4a5a6a7a8a9aaabacadaeaf "
                            "bytes=74565 segs=1 seglen=74565");
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 1);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 1);

    isp_response(&t, 0, e);
    g_assert_cmphex(e[0], ==, ENTRY_STATUS);
    g_assert_cmphex(e[1], ==, 1);
    g_assert_cmphex(e[3], ==, 0);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0xcafe0001);
    g_assert_cmphex(lduw_le_p(e + 8), ==, 0);
    g_assert_cmphex(lduw_le_p(e + 10), ==, COMPLETION_PORT_UNAVAILABLE);
    for (i = 12; i < ENTRY_SIZE; i++) {
        g_assert_cmphex(e[i], ==, 0);
    }
    isp_close(&t);
}

/*
 * 32-bit commands are equivalent, and segments past the first entry come
 * from continuations: the entry count steps over them, and none is
 * dispatched as a command of its own.
 */
static void test_continuations(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE], c[ENTRY_SIZE];
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 8);

    /* A 32-bit command alone. */
    entry_command(e, ENTRY_CMD32, 0x32, 3, 0, 0x12);
    stw_le_p(e + 18, 1);
    put_seg32(e + 40, 0x02100000, 36);
    isp_publish(&t, e);
    g_assert_cmpuint(fc_submissions(&t), ==, 1);
    assert_fc_last_contains(&t, "handle=00000032 loop=3 lun=0");
    assert_fc_last_contains(&t, "segs=1 seglen=36");
    isp_response(&t, 0, e);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0x32);
    isp_consume(&t, 1);

    /* 64-bit: two inline segments, two more in a 0x0a continuation. */
    entry_command(e, ENTRY_CMD64, 0x64, 3, 0, 0x2a);
    e[1] = 2;
    stw_le_p(e + 18, 4);
    put_seg64(e + 40, 0x02100000, 0x100);
    put_seg64(e + 52, 0x02200000, 0x200);
    memset(c, 0, sizeof(c));
    c[0] = ENTRY_CONT64;
    c[1] = 1;
    put_seg64(c + 4, 0x02300000, 0x300);
    put_seg64(c + 16, 0x02400000, 0x400);
    isp_put_request(&t, 1, e);
    isp_put_request(&t, 2, c);
    t.rq_next = 3;
    wr(&t, REG_REQUEST, 3);
    g_assert_cmpuint(fc_submissions(&t), ==, 2);
    assert_fc_last_contains(&t, "segs=4 seglen=2560");
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 3);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 2);
    isp_response(&t, 1, e);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0x64);
    isp_consume(&t, 1);

    /* 32-bit: three inline, one in a 0x02 continuation from byte 8. */
    entry_command(e, ENTRY_CMD32, 0x320, 3, 0, 0x28);
    e[1] = 2;
    stw_le_p(e + 18, 4);
    put_seg32(e + 40, 0x02100000, 1);
    put_seg32(e + 48, 0x02200000, 2);
    put_seg32(e + 56, 0x02300000, 4);
    memset(c, 0, sizeof(c));
    c[0] = ENTRY_CONT32;
    c[1] = 1;
    put_seg32(c + 8, 0x02400000, 8);
    isp_put_request(&t, 3, e);
    isp_put_request(&t, 4, c);
    t.rq_next = 5;

    /* Only the command is published: it waits for its continuation. */
    wr(&t, REG_REQUEST, 4);
    g_assert_cmpuint(fc_submissions(&t), ==, 2);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 3);
    wr(&t, REG_REQUEST, 5);
    g_assert_cmpuint(fc_submissions(&t), ==, 3);
    assert_fc_last_contains(&t, "segs=4 seglen=15");
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 5);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 3);
    isp_close(&t);
}

/* OpenBSD's segment-list command reads its list from host memory. */
static void test_segment_list(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE], list[3 * 12];
    uint64_t addr;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 8);

    addr = guest_alloc(alloc, sizeof(list));
    put_seg64(list, 0x02100000, 0x1000);
    put_seg64(list + 12, 0x02200000, 0x2000);
    put_seg64(list + 24, 0x02300000, 0x0800);
    qtest_memwrite(t.qts, addr, list, sizeof(list));

    entry_command(e, ENTRY_CMD_LIST, 0x15, 4, 1, 0x28);
    stw_le_p(e + 18, 3);
    stw_le_p(e + 40, 1);
    stq_le_p(e + 46, addr);
    isp_publish(&t, e);
    g_assert_cmpuint(fc_submissions(&t), ==, 1);
    assert_fc_last_contains(&t, "handle=00000015 loop=4 lun=1");
    assert_fc_last_contains(&t, "segs=3 seglen=14336");
    isp_response(&t, 0, e);
    g_assert_cmphex(e[0], ==, ENTRY_STATUS);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0x15);
    isp_close(&t);
}

/* Approximation: a marker is consumed and answers nothing. */
static void test_marker(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE] = { ENTRY_MARKER, 1 };
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 8);
    e[10] = 2;
    isp_publish(&t, e);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 1);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 0);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    g_assert_cmpuint(fc_submissions(&t), ==, 0);
    isp_close(&t);
}

/* The producer wraps at the ring length; entries either side are taken. */
static void test_request_wrap(void *obj, void *data, QGuestAllocator *alloc)
{
    unsigned i;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 4, 8);
    for (i = 0; i < 6; i++) {
        isp_tur(&t, 0x100 + i);
        g_assert_cmphex(rd(&t, REG_REQUEST), ==, t.rq_next);
    }
    g_assert_cmpuint(t.rq_next, ==, 2);
    g_assert_cmpuint(fc_submissions(&t), ==, 6);
    isp_close(&t);
}

/*
 * Approximation: response-in advances after each whole entry; with
 * the ring full, later completions wait and post in order once the host
 * frees room.
 */
static void test_response_ring_full(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE];
    unsigned i;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 4);

    for (i = 0; i < 5; i++) {
        entry_command(e, ENTRY_CMD64, 0x500 + i, 1, 0, 0);
        isp_publish(&t, e);
        if (i < 3) {
            g_assert_cmphex(rd(&t, REG_RESPONSE), ==, i + 1);
        }
        /* The host clears the latch without consuming anything. */
        wr(&t, REG_HCCR, HCCR_CLEAR_RISC);
    }
    /* Three of four slots hold entries; two completions wait. */
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 3);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    g_assert_cmpuint(fc_submissions(&t), ==, 5);

    wr(&t, REG_RESPONSE, 3);
    t.rs_next = 3;
    g_assert_cmphex(rd(&t, REG_ISR), ==, ISR_RISC);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 1);
    isp_response(&t, 3, e);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0x503);
    isp_response(&t, 0, e);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0x504);
    isp_close(&t);
}

/* Approximation: an unknown type comes back as itself, flagged. */
static void test_unknown_entry_type(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE];
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 8);
    entry_command(e, 0x33, 0xabcd0033, 1, 0, 0);
    isp_publish(&t, e);
    g_assert_cmpuint(fc_submissions(&t), ==, 0);
    isp_response(&t, 0, e);
    g_assert_cmphex(e[0], ==, 0x33);
    g_assert_cmphex(e[3], ==, ENTRY_FLAG_BAD_TYPE);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0xabcd0033);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 1);
    isp_close(&t);
}

/* The entry at the response consumer, then consumed. */
static void assert_rejected(Isp *t, uint8_t type, uint32_t handle,
                            uint8_t flag)
{
    uint8_t e[ENTRY_SIZE];

    isp_response(t, t->rs_next, e);
    g_assert_cmphex(e[0], ==, type);
    g_assert_cmphex(e[3], ==, flag);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, handle);
    isp_consume(t, 1);
}

/*
 * Approximation: which entry-status flag a malformed request carries is
 * the model's choice. An entry count of 0 or one that does not fit the
 * ring sets bit 4 (invalid count); more segments than the command's
 * entries hold sets bit 3 (invalid parameter); a continuation with no
 * command, or one of the other width, sets bit 5 (invalid order). None
 * reaches the service, and each takes the slots its count claims.
 */
static void test_malformed_entries(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE], c[ENTRY_SIZE];
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 8);

    entry_command(e, ENTRY_CMD64, 0xbad00000, 1, 0, 0);
    e[1] = 0;
    isp_publish(&t, e);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 1);
    assert_rejected(&t, ENTRY_CMD64, 0xbad00000, ENTRY_FLAG_BAD_COUNT);

    entry_command(e, ENTRY_CMD32, 0xbad00008, 1, 0, 0);
    e[1] = 8;
    isp_publish(&t, e);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 2);
    assert_rejected(&t, ENTRY_CMD32, 0xbad00008, ENTRY_FLAG_BAD_COUNT);

    /* A lone 0x19 holds two segments, not three. */
    entry_command(e, ENTRY_CMD64, 0xbad00003, 1, 0, 0);
    stw_le_p(e + 18, 3);
    isp_publish(&t, e);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 3);
    assert_rejected(&t, ENTRY_CMD64, 0xbad00003, ENTRY_FLAG_BAD_PARAM);

    /* Continuations with no command before them. */
    memset(c, 0, sizeof(c));
    c[0] = ENTRY_CONT64;
    c[1] = 1;
    stl_le_p(c + 4, 0xc0a);
    isp_publish(&t, c);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 4);
    assert_rejected(&t, ENTRY_CONT64, 0xc0a, ENTRY_FLAG_BAD_ORDER);

    c[0] = ENTRY_CONT32;
    stl_le_p(c + 4, 0xc02);
    isp_publish(&t, c);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 5);
    assert_rejected(&t, ENTRY_CONT32, 0xc02, ENTRY_FLAG_BAD_ORDER);

    /* A 64-bit command followed by a 32-bit continuation: both slots go. */
    entry_command(e, ENTRY_CMD64, 0xbad00064, 1, 0, 0);
    e[1] = 2;
    stw_le_p(e + 18, 3);
    put_seg64(e + 40, 0x02100000, 0x100);
    put_seg64(e + 52, 0x02200000, 0x100);
    memset(c, 0, sizeof(c));
    c[0] = ENTRY_CONT32;
    c[1] = 1;
    put_seg32(c + 8, 0x02300000, 0x100);
    isp_put_request(&t, 5, e);
    isp_put_request(&t, 6, c);
    t.rq_next = 7;
    wr(&t, REG_REQUEST, 7);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 7);
    assert_rejected(&t, ENTRY_CMD64, 0xbad00064, ENTRY_FLAG_BAD_ORDER);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 6);

    g_assert_cmpuint(fc_submissions(&t), ==, 0);
    isp_close(&t);
}

/*
 * Approximation: a refused read after the entry's own header -- the
 * segment list of a 0x15 command, or a continuation -- returns the
 * command with entry-status bit 6 (request-queue DMA error).
 */
static void test_descriptor_dma_errors(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE];
    Isp t;

    if (!is_mips64()) {
        g_test_skip("an unbacked address belongs to the SGI Bridge head");
        return;
    }
    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 8);

    entry_command(e, ENTRY_CMD_LIST, 0xd15, 4, 0, 0x28);
    stw_le_p(e + 18, 3);
    stw_le_p(e + 40, 1);
    stq_le_p(e + 46, UNMAPPED_PHYS);
    isp_publish(&t, e);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 1);
    assert_rejected(&t, ENTRY_CMD_LIST, 0xd15, ENTRY_FLAG_DMA_ERROR);
    g_assert_cmpuint(fc_submissions(&t), ==, 0);

    /* Slot 0 is the last backed one; its continuation in slot 1 is not. */
    isp_rings_at(&t, 4, 8, guest_alloc(alloc, ICB_SIZE),
                 TRANSFER_RAM_LAST_SLOT, guest_alloc(alloc, 8 * ENTRY_SIZE),
                 0, 0);
    entry_command(e, ENTRY_CMD64, 0xd0a, 1, 0, 0x28);
    e[1] = 2;
    stw_le_p(e + 18, 3);
    isp_put_request(&t, 0, e);
    t.rq_next = 2;
    wr(&t, REG_REQUEST, 2);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 2);
    assert_rejected(&t, ENTRY_CMD64, 0xd0a, ENTRY_FLAG_DMA_ERROR);
    g_assert_cmpuint(fc_submissions(&t), ==, 0);
    isp_close(&t);
}

static void assert_event(Isp *t, uint16_t code)
{
    g_assert_cmphex(rd(t, REG_SEMAPHORE), ==, SEM_LOCK);
    g_assert_cmphex(rd(t, REG_ISR) & ISR_RISC, ==, ISR_RISC);
    g_assert_cmphex(rd(t, MBOX(0)), ==, code);
    g_assert_cmphex(rd(t, MBOX(1)), ==, 0);
    g_assert_cmphex(rd(t, MBOX(2)), ==, 0);
    g_assert_cmphex(rd(t, MBOX(3)), ==, 0);
    isp_ack(t);
}

/*
 * Approximation: a refused request-ring read raises 0x8003 and a
 * refused response write 0x8004, both through the semaphore path.
 */
static void test_ring_dma_errors(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE];
    Isp t;

    if (!is_mips64()) {
        g_test_skip("an unbacked address belongs to the SGI Bridge head");
        return;
    }
    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings_at(&t, 8, 8, guest_alloc(alloc, ICB_SIZE), UNMAPPED_PHYS,
                 guest_alloc(alloc, 8 * ENTRY_SIZE), 0, 0);
    wr(&t, REG_REQUEST, 1);
    assert_event(&t, ASYNC_REQUEST_DMA);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 0);
    g_assert_cmpuint(fc_submissions(&t), ==, 0);

    isp_rings_at(&t, 8, 8, guest_alloc(alloc, ICB_SIZE),
                 guest_alloc(alloc, 8 * ENTRY_SIZE), UNMAPPED_PHYS, 0, 0);
    entry_command(e, ENTRY_CMD64, 0x8004, 1, 0, 0);
    isp_publish(&t, e);
    g_assert_cmpuint(fc_submissions(&t), ==, 1);
    assert_event(&t, ASYNC_RESPONSE_DMA);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 0);
    isp_close(&t);
}

/*
 * Approximation: a posted result stays readable while the host writes a
 * command's mailboxes behind it. Linux writes all 24 mailboxes and rings
 * without draining a result first; the IRIX 6.5.30 driver wrote only
 * mailbox 0 over a posted event on the model. In both, mailboxes 0..3
 * read the event until its release, and the command then completes on
 * what the host wrote. A model that lets the write through to the read
 * side fails both halves.
 */
static void test_writes_behind_posted_result(void *obj, void *data,
                                             QGuestAllocator *alloc)
{
    uint16_t in[MBOX_COUNT];
    unsigned i;
    Isp t;

    if (!is_mips64()) {
        g_test_skip("an unbacked address belongs to the SGI Bridge head");
        return;
    }
    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings_at(&t, 8, 8, guest_alloc(alloc, ICB_SIZE), UNMAPPED_PHYS,
                 guest_alloc(alloc, 8 * ENTRY_SIZE), 0, 0);

    /* Linux's order: every mailbox, ascending, then the doorbell. */
    wr(&t, REG_REQUEST, 1);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, ASYNC_REQUEST_DMA);
    in[0] = CMD_REGISTER_TEST;
    for (i = 1; i < MBOX_COUNT; i++) {
        in[i] = 0x5a00 | i;
    }
    isp_mbox(&t, in, MBOX_COUNT);
    for (i = 0; i < 4; i++) {
        g_assert_cmphex(rd(&t, MBOX(i)), ==,
                        i == 0 ? ASYNC_REQUEST_DMA : 0);
    }
    g_assert_cmphex(rd(&t, MBOX(8)), ==, 0);
    isp_ack(&t);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, SEM_LOCK);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    for (i = 1; i < 8; i++) {
        g_assert_cmphex(rd(&t, MBOX(i)), ==, in[i]);
    }
    isp_ack(&t);

    /* IRIX's case: mailbox 0 alone, then the doorbell. */
    wr(&t, REG_REQUEST, 2);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, ASYNC_REQUEST_DMA);
    wr(&t, MBOX(0), CMD_NOP);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, ASYNC_REQUEST_DMA);
    wr(&t, REG_HCCR, HCCR_SET_HOST);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, ASYNC_REQUEST_DMA);
    wr(&t, REG_HCCR, HCCR_CLEAR_RISC);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, ASYNC_REQUEST_DMA);
    wr(&t, REG_SEMAPHORE, 0);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, SEM_LOCK);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    isp_ack(&t);

    /* With the lock free and nothing posted, a write reads back. */
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, 0);
    wr(&t, MBOX(1), 0x1234);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 0x1234);
    isp_close(&t);
}

/*
 * Approximation: events are never dropped, and a refused request read
 * adds no event while an identical one still waits to be posted: that
 * one already reports it. One 0x8003 can wait behind a posted one.
 */
static void test_request_dma_error_waits(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    Isp t;

    if (!is_mips64()) {
        g_test_skip("an unbacked address belongs to the SGI Bridge head");
        return;
    }
    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings_at(&t, 8, 8, guest_alloc(alloc, ICB_SIZE), UNMAPPED_PHYS,
                 guest_alloc(alloc, 8 * ENTRY_SIZE), 0, 0);

    /* Posted, then one waiting, then retries that add nothing. */
    wr(&t, REG_REQUEST, 1);
    wr(&t, REG_REQUEST, 2);
    wr(&t, REG_REQUEST, 3);
    wr(&t, REG_REQUEST, 4);

    assert_event(&t, ASYNC_REQUEST_DMA);
    assert_event(&t, ASYNC_REQUEST_DMA);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, 0);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 0);

    /* Released, the next producer write reports the ring again. */
    wr(&t, REG_REQUEST, 5);
    assert_event(&t, ASYNC_REQUEST_DMA);
    g_assert_cmpuint(fc_submissions(&t), ==, 0);
    isp_close(&t);
}

/* With bus mastering off, nothing that needs DMA moves, and it says so. */
static void test_bus_master_disabled(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    uint64_t src;
    uint16_t cmd;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 8);
    src = guest_alloc(alloc, 8);

    cmd = qpci_config_readw(t.dev, PCI_COMMAND);
    qpci_config_writew(t.dev, PCI_COMMAND, cmd & ~PCI_COMMAND_MASTER);

    g_assert_cmphex(isp_call_ack(&t, CMD_NOP, 0, 0, 0), ==, MBOX_COMPLETE);
    g_assert_cmphex(isp_load_ram(&t, CMD_LOAD_RAM, 0x2000, src, 4, 0), ==,
                    MBOX_HOST_IF_ERROR);
    isp_ack(&t);

    wr(&t, REG_REQUEST, 1);
    assert_event(&t, ASYNC_REQUEST_DMA);
    g_assert_cmpuint(fc_submissions(&t), ==, 0);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 0);
    isp_close(&t);
}

/*
 * Approximation: no NVRAM image. A read clocked the way OpenBSD
 * clocks it returns words that do not start with "ISP ".
 */
static void test_nvram_read_fails(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    uint16_t words[2] = { 0 };
    unsigned w, bit;
    Isp t;

    isp_open(&t, obj, alloc);
    for (w = 0; w < 2; w++) {
        uint16_t cmd = (6 << 8) | w;

        wr(&t, REG_NVRAM, 0x0002);
        for (bit = 0; bit < 11; bit++) {
            uint16_t out = (cmd & (1 << (10 - bit))) ? 0x0004 : 0;

            wr(&t, REG_NVRAM, 0x0002 | out);
            wr(&t, REG_NVRAM, 0x0003 | out);
            wr(&t, REG_NVRAM, 0x0002 | out);
        }
        for (bit = 0; bit < 16; bit++) {
            wr(&t, REG_NVRAM, 0x0003);
            words[w] = (words[w] << 1) | !!(rd(&t, REG_NVRAM) & 0x0008);
            wr(&t, REG_NVRAM, 0x0002);
        }
        wr(&t, REG_NVRAM, 0);
    }
    g_assert_false(words[0] == 0x4953 && words[1] == 0x5020);
    g_assert_false(words[0] == 0x5349 && words[1] == 0x2050);
    isp_close(&t);
}

/* Three commands through, so both request pointers read 3. */
static void isp_three_commands(Isp *t)
{
    isp_start(t);
    isp_rings(t, 8, 8);
    isp_tur(t, 1);
    isp_tur(t, 2);
    isp_tur(t, 3);
    g_assert_cmphex(rd(t, REG_REQUEST), ==, 3);
    g_assert_cmpuint(fc_submissions(t), ==, 3);
}

/*
 * Approximation: Linux writes all 24 mailboxes on every command,
 * mailbox 4 included; inside the command window that is an input, so
 * the ring stays put and nothing is resubmitted.
 */
static void test_window_linux_order(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    uint16_t in[MBOX_COUNT] = { CMD_NOP };
    uint8_t e[ENTRY_SIZE];
    Isp t;

    isp_open(&t, obj, alloc);
    isp_three_commands(&t);

    g_assert_cmphex(isp_mbox(&t, in, MBOX_COUNT), ==, MBOX_COMPLETE);
    isp_ack(&t);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 3);
    g_assert_cmpuint(fc_submissions(&t), ==, 3);

    isp_tur(&t, 4);
    g_assert_cmpuint(fc_submissions(&t), ==, 4);

    in[4] = 0x1234;
    in[5] = 0x5678;
    g_assert_cmphex(isp_mbox(&t, in, MBOX_COUNT), ==, MBOX_COMPLETE);
    isp_ack(&t);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 4);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 4);
    g_assert_cmpuint(fc_submissions(&t), ==, 4);
    isp_response(&t, 4, e);
    g_assert_cmphex(e[0], ==, 0);
    isp_close(&t);
}

/*
 * Approximation: OpenBSD's INIT_FIRMWARE writes m4 = m5 = 0 as
 * inputs, with the queues already live; the command resets the rings and
 * a new entry is taken once.
 */
static void test_window_openbsd_init(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    uint64_t icb;
    uint16_t in[8];
    uint8_t e[ENTRY_SIZE];
    Isp t;

    isp_open(&t, obj, alloc);
    isp_three_commands(&t);

    icb = t.icb;
    in[0] = CMD_INIT_FIRMWARE;
    in[2] = (icb >> 16) & 0xffff;
    in[3] = icb & 0xffff;
    in[4] = 0;
    in[5] = 0;
    in[6] = (icb >> 48) & 0xffff;
    in[7] = (icb >> 32) & 0xffff;
    wr(&t, MBOX(0), in[0]);
    wr(&t, MBOX(2), in[2]);
    wr(&t, MBOX(3), in[3]);
    wr(&t, MBOX(4), in[4]);
    wr(&t, MBOX(5), in[5]);
    wr(&t, MBOX(6), in[6]);
    wr(&t, MBOX(7), in[7]);
    wr(&t, REG_HCCR, HCCR_SET_HOST);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    isp_ack(&t);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 0);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 0);

    t.rq_next = 0;
    t.rs_next = 0;
    qtest_memset(t.qts, t.rs, 0, ENTRY_SIZE);
    entry_command(e, ENTRY_CMD64, 0x38, 1, 0, 0);
    isp_publish(&t, e);
    g_assert_cmpuint(fc_submissions(&t), ==, 4);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 1);
    isp_response(&t, 0, e);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0x38);
    isp_close(&t);
}

/*
 * OpenBSD's soft-reset sequence writes all four pointers as 0 after the
 * release; with no queue they are mailbox storage.
 */
static void test_window_openbsd_reset(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    isp_three_commands(&t);

    isp_soft_reset(&t);
    wr(&t, REG_HCCR, HCCR_RESET);
    wr(&t, REG_SEMAPHORE, 0);
    wr(&t, REG_HCCR, HCCR_PARITY4);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    wr(&t, REG_REQUEST, 0);
    wr(&t, REG_REQUEST, 0);
    wr(&t, REG_RESPONSE, 0);
    wr(&t, REG_RESPONSE, 0);
    g_assert_cmpuint(fc_submissions(&t), ==, 3);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);

    isp_start(&t);
    isp_rings(&t, 8, 8);
    isp_tur(&t, 0x39);
    g_assert_cmpuint(fc_submissions(&t), ==, 4);
    isp_close(&t);
}

/*
 * Approximation, the window's limits: an open window turns a
 * producer write into storage until the doorbell; a reset closes it too,
 * and takes the queues and firmware with it.
 */
static void test_window_boundaries(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE];
    Isp t;

    isp_open(&t, obj, alloc);
    isp_three_commands(&t);
    entry_command(e, ENTRY_CMD64, 4, 1, 0, 0);
    isp_put_request(&t, 3, e);

    /* (a) Mailbox 0, no doorbell: the producer write publishes nothing. */
    wr(&t, MBOX(0), CMD_NOP);
    wr(&t, REG_REQUEST, 4);
    g_assert_cmpuint(fc_submissions(&t), ==, 3);

    /* (b) The doorbell closes the window; the same write now publishes. */
    wr(&t, REG_HCCR, HCCR_SET_HOST);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    isp_ack(&t);
    wr(&t, REG_REQUEST, 4);
    g_assert_cmpuint(fc_submissions(&t), ==, 4);
    isp_consume(&t, 1);

    /* (c) A reset closes it as well, and empties the adapter. */
    wr(&t, MBOX(0), CMD_NOP);
    wr(&t, REG_HCCR, HCCR_RESET);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    wr(&t, REG_REQUEST, 4);
    g_assert_cmphex(rd(&t, REG_REQUEST), ==, 4);
    g_assert_cmpuint(fc_submissions(&t), ==, 4);
    g_assert_cmphex(isp_call_ack(&t, CMD_INIT_FIRMWARE, 0, 0, 0), ==,
                    MBOX_INVALID);

    isp_start(&t);
    isp_rings(&t, 8, 8);
    entry_command(e, ENTRY_CMD64, 5, 1, 0, 0);
    isp_put_request(&t, 0, e);
    t.rq_next = 1;
    wr(&t, REG_REQUEST, 1);
    g_assert_cmpuint(fc_submissions(&t), ==, 5);
    isp_close(&t);
}

/*
 * Fills a two-slot response ring (one usable slot) and leaves one more
 * completion held behind it, latch clear.
 */
static void isp_fill_response_ring(Isp *t)
{
    uint8_t e[ENTRY_SIZE];

    isp_start(t);
    isp_rings(t, 8, 2);
    entry_command(e, ENTRY_CMD64, 0xa1, 1, 0, 0);
    isp_publish(t, e);
    wr(t, REG_HCCR, HCCR_CLEAR_RISC);
    entry_command(e, ENTRY_CMD64, 0xa2, 1, 0, 0);
    isp_publish(t, e);
    g_assert_cmphex(rd(t, REG_RESPONSE), ==, 1);
    g_assert_cmphex(rd(t, REG_ISR), ==, 0);
    isp_response(t, 1, e);
    g_assert_cmphex(e[0], ==, 0);
}

/*
 * Approximation: a response-out write inside the window is an input
 * and leaves the ring where it was; after the doorbell it moves it.
 */
static void test_window_response_out(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE];
    Isp t;

    isp_open(&t, obj, alloc);
    isp_fill_response_ring(&t);

    wr(&t, MBOX(0), CMD_NOP);
    wr(&t, REG_RESPONSE, 1);
    wr(&t, REG_HCCR, HCCR_SET_HOST);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    isp_ack(&t);
    isp_response(&t, 1, e);
    g_assert_cmphex(e[0], ==, 0);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 1);

    wr(&t, REG_RESPONSE, 1);
    isp_response(&t, 1, e);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0xa2);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 0);
    g_assert_cmphex(rd(&t, REG_ISR), ==, ISR_RISC);
    isp_close(&t);
}

/*
 * Approximation: nothing is written while the latch is set. A
 * response-out write alone posts nothing; the clear does, and a batch of
 * held completions posts in order under one latch.
 */
static void test_posting_waits_for_latch(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE];
    unsigned i;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 8);

    entry_command(e, ENTRY_CMD64, 0xb1, 1, 0, 0);
    isp_publish(&t, e);
    g_assert_cmphex(rd(&t, REG_ISR), ==, ISR_RISC);
    entry_command(e, ENTRY_CMD64, 0xb2, 1, 0, 0);
    isp_publish(&t, e);
    g_assert_cmpuint(fc_submissions(&t), ==, 2);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 1);
    isp_response(&t, 1, e);
    g_assert_cmphex(e[0], ==, 0);

    wr(&t, REG_RESPONSE, 1);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 1);
    isp_response(&t, 1, e);
    g_assert_cmphex(e[0], ==, 0);

    wr(&t, REG_HCCR, HCCR_CLEAR_RISC);
    g_assert_cmphex(rd(&t, REG_ISR), ==, ISR_RISC);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 2);
    isp_response(&t, 1, e);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0xb2);

    /* Three more held behind the standing latch post as one batch. */
    for (i = 0; i < 3; i++) {
        entry_command(e, ENTRY_CMD64, 0xc0 + i, 1, 0, 0);
        isp_publish(&t, e);
    }
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 2);
    wr(&t, REG_RESPONSE, 2);
    wr(&t, REG_HCCR, HCCR_CLEAR_RISC);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 5);
    g_assert_cmphex(rd(&t, REG_ISR), ==, ISR_RISC);
    for (i = 0; i < 3; i++) {
        isp_response(&t, 2 + i, e);
        g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0xc0 + i);
    }
    isp_close(&t);
}

/*
 * Approximation: nothing is written while a mailbox result holds
 * the semaphore; a queued mailbox result posts first and alone; and
 * nothing posts while the RISC is paused.
 */
static void test_posting_waits_for_semaphore(void *obj, void *data,
                                             QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE];
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 8);

    /* A result holds the semaphore; its latch is cleared, the lock kept. */
    isp_call(&t, CMD_NOP, 0, 0, 0);
    wr(&t, REG_HCCR, HCCR_CLEAR_RISC);
    entry_command(e, ENTRY_CMD64, 0xd1, 1, 0, 0);
    isp_publish(&t, e);
    isp_response(&t, 0, e);
    g_assert_cmphex(e[0], ==, 0);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    wr(&t, REG_SEMAPHORE, 0);
    g_assert_cmphex(rd(&t, REG_ISR), ==, ISR_RISC);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 1);
    isp_response(&t, 0, e);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0xd1);

    /* With the latch still set: a mailbox result and a completion queue. */
    isp_call(&t, CMD_NOP, 0, 0, 0);
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, 0);
    entry_command(e, ENTRY_CMD64, 0xd2, 1, 0, 0);
    isp_publish(&t, e);
    isp_consume(&t, 1);
    /* The mailbox result went first, alone. */
    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, SEM_LOCK);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    isp_response(&t, 1, e);
    g_assert_cmphex(e[0], ==, 0);
    isp_ack(&t);
    g_assert_cmphex(rd(&t, REG_ISR), ==, ISR_RISC);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 2);
    isp_response(&t, 1, e);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0xd2);

    /* Held by the latch, then paused: the clear posts nothing. */
    entry_command(e, ENTRY_CMD64, 0xd3, 1, 0, 0);
    isp_publish(&t, e);
    wr(&t, REG_HCCR, HCCR_PAUSE);
    isp_consume(&t, 1);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    isp_response(&t, 2, e);
    g_assert_cmphex(e[0], ==, 0);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    g_assert_cmphex(rd(&t, REG_ISR), ==, ISR_RISC);
    isp_response(&t, 2, e);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0xd3);
    isp_close(&t);
}

/*
 * Approximation: the expanded-LUN layout -- target in the high byte
 * of the field at 8, LUN at 10 -- reaches the service intact.
 */
static void test_expanded_lun(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE];
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 8);
    entry_command(e, ENTRY_CMD64, 0x46, 0, 0, 0x12);
    stw_le_p(e + 8, 7 << 8);
    stw_le_p(e + 10, 3);
    isp_publish(&t, e);
    assert_fc_last_contains(&t, "handle=00000046 loop=7 lun=3");
    isp_close(&t);
}

/*
 * Approximation, not a hardware fact: OpenBSD can land a pointer
 * write between a command's mailbox 0 write and its doorbell. The model
 * takes it as mailbox storage. (a) A producer write is lost until the next
 * one; (b) a response-out write is lost, and the held completion stays
 * held. The repeat without the mailbox 0 write shows the difference.
 */
static void test_window_openbsd_interleave(void *obj, void *data,
                                           QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE];
    Isp t;

    isp_open(&t, obj, alloc);
    isp_three_commands(&t);
    entry_command(e, ENTRY_CMD64, 0x47a, 1, 0, 0);
    isp_put_request(&t, 3, e);
    entry_command(e, ENTRY_CMD64, 0x47b, 1, 0, 0);
    isp_put_request(&t, 4, e);

    wr(&t, MBOX(0), CMD_NOP);
    wr(&t, REG_REQUEST, 4);
    wr(&t, REG_HCCR, HCCR_SET_HOST);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    g_assert_cmpuint(fc_submissions(&t), ==, 3);
    isp_ack(&t);
    wr(&t, REG_REQUEST, 5);
    g_assert_cmpuint(fc_submissions(&t), ==, 5);
    isp_close(&t);
}

static void test_window_openbsd_interleave_response(void *obj, void *data,
                                                    QGuestAllocator *alloc)
{
    uint8_t e[ENTRY_SIZE];
    Isp t;

    isp_open(&t, obj, alloc);
    isp_fill_response_ring(&t);

    wr(&t, MBOX(0), CMD_NOP);
    wr(&t, REG_RESPONSE, 1);
    wr(&t, REG_HCCR, HCCR_SET_HOST);
    wr(&t, REG_HCCR, HCCR_CLEAR_RISC);
    wr(&t, REG_SEMAPHORE, 0);
    isp_response(&t, 1, e);
    g_assert_cmphex(e[0], ==, 0);

    /* The same, without the mailbox 0 write. */
    wr(&t, REG_RESPONSE, 1);
    wr(&t, REG_HCCR, HCCR_SET_HOST);
    wr(&t, REG_HCCR, HCCR_CLEAR_RISC);
    wr(&t, REG_SEMAPHORE, 0);
    isp_response(&t, 1, e);
    g_assert_cmphex((uint32_t)ldl_le_p(e + 4), ==, 0xa2);
    isp_close(&t);
}

/*
 * The loop cases. Each plugs scsi-hd devices onto the adapter's bus,
 * "isp0.0": a device's SCSI target number is its loop ID and its LUN the
 * LUN, and every device at one loop ID carries the same names.
 */
typedef enum LoopBacking {
    LOOP_ZEROES,    /* null-co, reads zeroes, completes at once. */
    LOOP_FILE,      /* A writable raw file the test can read back. */
    LOOP_SLOW,      /* null-co, reads zeroes after a second. */
} LoopBacking;

typedef struct LoopDisk {
    int loop_id;
    int lun;
    uint64_t port_wwn;
    uint64_t node_wwn;
    const char *serial;
    LoopBacking backing;
} LoopDisk;

typedef struct LoopConfig {
    const LoopDisk *disks;
    unsigned ndisks;
    /* Unplugged drives "spare0".. for device_add. */
    unsigned spares;
} LoopConfig;

typedef struct LoopTest {
    const LoopConfig *config;
    char *path[4];
} LoopTest;

/* Every byte of each name differs, so a byte-order slip shows. */
#define WWN_PORT(id)    ((uint64_t)0x21000011aabbcc00ULL | (id))
#define WWN_NODE(id)    ((uint64_t)0x20000011aabbcc00ULL | (id))

#define LOOP_DISK_SIZE  (1024 * 1024)

static void loop_test_destroy(void *opaque)
{
    LoopTest *test = opaque;
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(test->path); i++) {
        if (test->path[i]) {
            unlink(test->path[i]);
            g_free(test->path[i]);
        }
    }
    g_free(test);
}

static void *loop_test_setup(GString *cmd_line, void *arg)
{
    LoopTest *test = g_new0(LoopTest, 1);
    unsigned i, files = 0;

    test->config = arg;
    for (i = 0; i < test->config->ndisks; i++) {
        const LoopDisk *d = &test->config->disks[i];

        if (d->backing == LOOP_FILE) {
            int fd;

            g_assert_cmpuint(files, <, ARRAY_SIZE(test->path));
            fd = g_file_open_tmp("qtest-isp2200.XXXXXX", &test->path[files],
                                 NULL);
            g_assert_cmpint(fd, >=, 0);
            g_assert_cmpint(ftruncate(fd, LOOP_DISK_SIZE), ==, 0);
            close(fd);
            g_string_append_printf(cmd_line,
                                   " -drive id=disk%u,if=none,format=raw,"
                                   "file=%s", i, test->path[files]);
            files++;
        } else {
            g_string_append_printf(cmd_line,
                                   " -drive id=disk%u,if=none,format=raw,"
                                   "file=null-co://,file.read-zeroes=on%s",
                                   i, d->backing == LOOP_SLOW ?
                                   ",file.latency-ns=1000000000" : "");
        }
        g_string_append_printf(cmd_line,
                               " -device scsi-hd,bus=isp0.0,drive=disk%u,"
                               "id=port%u,scsi-id=%d,lun=%d,"
                               "port_wwn=0x%" PRIx64 ",wwn=0x%" PRIx64,
                               i, i, d->loop_id, d->lun, d->port_wwn,
                               d->node_wwn);
        if (d->serial) {
            g_string_append_printf(cmd_line, ",serial=%s", d->serial);
        }
    }
    for (i = 0; i < test->config->spares; i++) {
        g_string_append_printf(cmd_line,
                               " -drive id=spare%u,if=none,format=raw,"
                               "file=null-co://,file.read-zeroes=on", i);
    }
    g_test_queue_destroy(loop_test_destroy, test);
    return test;
}

/* Two ports: loop ID 5 with LUNs 0 and 1, loop ID 1 with LUN 0. */
static const LoopDisk loop_disks[] = {
    { 5, 0, WWN_PORT(5), WWN_NODE(5), "LOOP5", LOOP_FILE },
    { 5, 1, WWN_PORT(5), WWN_NODE(5), "LUN1", LOOP_ZEROES },
    { 1, 0, WWN_PORT(1), WWN_NODE(1), "LOOP1", LOOP_ZEROES },
};

static const LoopConfig loop_config = {
    .disks = loop_disks,
    .ndisks = ARRAY_SIZE(loop_disks),
};

/* One port at loop ID 5, and spare drives to plug in. */
static const LoopDisk plug_disks[] = {
    { 5, 0, WWN_PORT(5), WWN_NODE(5), NULL, LOOP_ZEROES },
};

static const LoopConfig plug_config = {
    .disks = plug_disks,
    .ndisks = ARRAY_SIZE(plug_disks),
    .spares = 2,
};

/* A port holding loop ID 0, the lowest one. */
static const LoopDisk low_disks[] = {
    { 0, 0, WWN_PORT(0x40), WWN_NODE(0x40), NULL, LOOP_ZEROES },
    { 2, 0, WWN_PORT(0x42), WWN_NODE(0x42), NULL, LOOP_ZEROES },
};

static const LoopConfig low_config = {
    .disks = low_disks,
    .ndisks = ARRAY_SIZE(low_disks),
};

static uint16_t isp_fw_state(Isp *t)
{
    uint16_t state;

    g_assert_cmphex(isp_call(t, CMD_GET_FW_STATE, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    state = rd(t, MBOX(1));
    g_assert_cmphex(rd(t, MBOX(2)), ==, 0);
    isp_ack(t);
    return state;
}

/* The event now posted: its code, and m1..m3 into @m. Then released. */
static uint16_t isp_take_event(Isp *t, uint16_t *m)
{
    uint16_t code;
    unsigned i;

    g_assert_cmphex(rd(t, REG_SEMAPHORE) & SEM_LOCK, ==, SEM_LOCK);
    g_assert_cmphex(rd(t, REG_ISR) & ISR_RISC, ==, ISR_RISC);
    code = rd(t, MBOX(0));
    for (i = 1; i < 4; i++) {
        m[i - 1] = rd(t, MBOX(i));
    }
    isp_ack(t);
    return code;
}

static void isp_expect_no_event(Isp *t)
{
    g_assert_cmphex(rd(t, REG_SEMAPHORE), ==, 0);
    g_assert_cmphex(rd(t, REG_ISR), ==, 0);
}

/* Exactly @codes, in order, with the payloads the design gives. */
static void isp_expect_events(Isp *t, const uint16_t *codes, unsigned n)
{
    uint16_t m[3];
    unsigned i;

    for (i = 0; i < n; i++) {
        g_assert_cmphex(isp_take_event(t, m), ==, codes[i]);
        g_assert_cmphex(m[0], ==, codes[i] == ASYNC_PORT_DB ? 0xffff : 0);
        g_assert_cmphex(m[1], ==, 0);
        g_assert_cmphex(m[2], ==, 0);
    }
    isp_expect_no_event(t);
}

static const uint16_t bringup_events[] = {
    ASYNC_LIP, ASYNC_LOOP_UP, ASYNC_PORT_DB,
};

/*
 * Download, execute, initialise with @options, then let the loop come
 * up and take the three events a bring-up posts.
 */
static void isp_loop_up(Isp *t, uint16_t options)
{
    isp_start(t);
    isp_rings_at(t, 16, 16, guest_alloc(t->alloc, ICB_SIZE),
                 guest_alloc(t->alloc, 16 * ENTRY_SIZE),
                 guest_alloc(t->alloc, 16 * ENTRY_SIZE), options, 0);
    g_assert_cmphex(isp_fw_state(t), ==, FW_STATE_WAIT_AL_PA);
    qtest_clock_step(t->qts, BRINGUP_NS);
    isp_expect_events(t, bringup_events,
                      options & ICB_OPT_PORT_DB ? 3 : 2);
    g_assert_cmphex(isp_fw_state(t), ==, FW_STATE_READY);
}

/* A mailbox command with a DMA address in mapping D. */
static uint16_t isp_call_dma(Isp *t, uint16_t cmd, uint16_t m1,
                             uint64_t addr)
{
    const uint16_t in[] = {
        cmd, m1, (addr >> 16) & 0xffff, addr & 0xffff, 0, 0,
        (addr >> 48) & 0xffff, (addr >> 32) & 0xffff,
    };

    return isp_mbox(t, in, ARRAY_SIZE(in));
}

/* Get ID List's own address split: m1 31:16, m2 15:0, m3 63:48, m6 47:32. */
static uint16_t isp_get_id_list(Isp *t, uint64_t addr)
{
    const uint16_t in[] = {
        CMD_GET_ID_LIST, (addr >> 16) & 0xffff, addr & 0xffff,
        (addr >> 48) & 0xffff, 0, 0, (addr >> 32) & 0xffff,
    };

    return isp_mbox(t, in, ARRAY_SIZE(in));
}

/* The ID list as "alpa:loop" pairs, for a compact comparison. */
static void assert_id_list(Isp *t, const char *expect)
{
    uint64_t buf = guest_alloc(t->alloc, 512);
    g_autoptr(GString) seen = g_string_new(NULL);
    uint8_t list[512];
    uint16_t count;
    unsigned i;

    qtest_memset(t->qts, buf, 0xa5, sizeof(list));
    g_assert_cmphex(isp_get_id_list(t, buf), ==, MBOX_COMPLETE);
    count = rd(t, MBOX(1));
    isp_ack(t);
    g_assert_cmpuint(count, <=, 126);
    qtest_memread(t->qts, buf, list, sizeof(list));
    for (i = 0; i < count; i++) {
        /* AL_PA, area 0, domain 0, loop ID. */
        g_assert_cmphex(list[i * 4 + 1], ==, 0);
        g_assert_cmphex(list[i * 4 + 2], ==, 0);
        g_string_append_printf(seen, "%s%02x:%u", i ? " " : "",
                               list[i * 4], list[i * 4 + 3]);
    }
    /* Nothing past the last entry is written. */
    g_assert_cmphex(list[count * 4], ==, 0xa5);
    g_assert_cmpstr(seen->str, ==, expect);
}

static void put_be64(uint8_t *p, uint64_t v)
{
    stq_be_p(p, v);
}

/* Whether @len bytes at @needle appear anywhere in @hay. */
static bool has_bytes(const uint8_t *hay, size_t hay_len,
                      const uint8_t *needle, size_t len)
{
    size_t i;

    for (i = 0; i + len <= hay_len; i++) {
        if (!memcmp(hay + i, needle, len)) {
            return true;
        }
    }
    return false;
}

/*
 * The identity of one port, five ways: the adapter's own ID, the ID
 * list, OpenBSD's port name list, the port database and the position
 * map. The adapter takes loop ID 0, the lowest free one; loop ID 5 is
 * AL_PA 0xe0 and loop ID 1 is 0xe8 in the standard table.
 */
static void test_loop_identity(void *obj, void *data, QGuestAllocator *alloc)
{
    uint8_t pd[129], want[128], names[32];
    uint64_t buf;
    unsigned i;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);

    g_assert_cmphex(isp_call(&t, CMD_GET_ADAPTER_ID, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 0);
    g_assert_cmphex(rd(&t, MBOX(2)), ==, 0x00ef);
    g_assert_cmphex(rd(&t, MBOX(3)), ==, 0);
    g_assert_cmphex(rd(&t, MBOX(6)), ==, 0);
    g_assert_cmphex(rd(&t, MBOX(7)), ==, 0);
    isp_ack(&t);

    /* Ascending loop ID, without the adapter. */
    assert_id_list(&t, "e8:1 e0:5");

    /*
     * Approximation: the port name list's WWN is the eight wire-order
     * bytes reversed, which OpenBSD decodes as little-endian to the same
     * name it reads big-endian from the port database.
     */
    buf = guest_alloc(alloc, sizeof(names));
    qtest_memset(t.qts, buf, 0xa5, sizeof(names));
    g_assert_cmphex(isp_call_dma(&t, CMD_PORT_NAME_LIST, 0x08, buf), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 20);
    isp_ack(&t);
    qtest_memread(t.qts, buf, names, sizeof(names));
    g_assert_cmphex(ldq_le_p(names), ==, WWN_PORT(1));
    g_assert_cmphex(lduw_le_p(names + 8), ==, 1);
    g_assert_cmphex(ldq_le_p(names + 10), ==, WWN_PORT(5));
    g_assert_cmphex(lduw_le_p(names + 18), ==, 5);
    g_assert_cmphex(names[20], ==, 0xa5);
    /* Option bits other than 0x02 and 0x08 are a parameter error. */
    g_assert_cmphex(isp_call_dma(&t, CMD_PORT_NAME_LIST, 0x01, buf), ==,
                    MBOX_PARAM_ERROR);
    isp_ack(&t);

    /* The port database: every byte the design does not name is zero. */
    buf = guest_alloc(alloc, sizeof(pd));
    qtest_memset(t.qts, buf, 0xa5, sizeof(pd));
    g_assert_cmphex(isp_call_dma(&t, CMD_GET_PORT_DB, 5 << 8, buf), ==,
                    MBOX_COMPLETE);
    isp_ack(&t);
    qtest_memread(t.qts, buf, pd, sizeof(pd));
    memset(want, 0, sizeof(want));
    want[2] = 6;
    want[3] = 6;
    want[6] = 0xe0;
    want[10] = 0xe0;
    put_be64(want + 12, WWN_NODE(5));
    put_be64(want + 20, WWN_PORT(5));
    want[78] = 0x10;
    stw_le_p(want + 80, 5);
    for (i = 0; i < sizeof(want); i++) {
        if (pd[i] != want[i]) {
            g_test_message("port database byte %u", i);
        }
        g_assert_cmphex(pd[i], ==, want[i]);
    }
    g_assert_cmphex(pd[128], ==, 0xa5);

    /* Refused: an empty loop ID, the adapter's own, past 125. */
    g_assert_cmphex(isp_call_dma(&t, CMD_GET_PORT_DB, 6 << 8, buf), ==,
                    MBOX_COMMAND_ERROR);
    isp_ack(&t);
    g_assert_cmphex(isp_call_dma(&t, CMD_GET_PORT_DB, 0 << 8, buf), ==,
                    MBOX_COMMAND_ERROR);
    isp_ack(&t);
    g_assert_cmphex(isp_call_dma(&t, CMD_GET_PORT_DB, 126 << 8, buf), ==,
                    MBOX_COMMAND_ERROR);
    isp_ack(&t);

    /* Approximation: the count, the adapter, then the ports by loop ID. */
    qtest_memset(t.qts, buf, 0xa5, sizeof(pd));
    g_assert_cmphex(isp_call_dma(&t, CMD_POSITION_MAP, 0, buf), ==,
                    MBOX_COMPLETE);
    isp_ack(&t);
    qtest_memread(t.qts, buf, pd, sizeof(pd));
    g_assert_cmphex(pd[0], ==, 3);
    g_assert_cmphex(pd[1], ==, 0xef);
    g_assert_cmphex(pd[2], ==, 0xe8);
    g_assert_cmphex(pd[3], ==, 0xe0);
    for (i = 4; i < 128; i++) {
        g_assert_cmphex(pd[i], ==, 0);
    }
    g_assert_cmphex(pd[128], ==, 0xa5);
    isp_close(&t);
}

/*
 * Approximation: the adapter's loop ID. A requested hard address it can
 * have is taken; one a port holds, or one past 125, gives the lowest free
 * loop ID instead. With nothing requested it is the lowest free one too.
 */
static void test_loop_hard_address(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    static const struct {
        uint16_t options;
        uint16_t hard;
        uint16_t loop_id;
        uint8_t alpa;
    } cases[] = {
        { ICB_OPT_HARD_ADDR, 0, 1, 0xe8 },
        { ICB_OPT_HARD_ADDR, 3, 3, 0xe2 },
        { ICB_OPT_HARD_ADDR, 2, 1, 0xe8 },
        { ICB_OPT_HARD_ADDR, 126, 1, 0xe8 },
        { 0, 3, 1, 0xe8 },
    };
    unsigned i;
    Isp t;

    isp_open(&t, obj, alloc);
    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        t.hard_addr = cases[i].hard;
        isp_loop_up(&t, cases[i].options);
        g_assert_cmphex(isp_call(&t, CMD_GET_ADAPTER_ID, 0, 0, 0), ==,
                        MBOX_COMPLETE);
        g_assert_cmphex(rd(&t, MBOX(1)), ==, cases[i].loop_id);
        g_assert_cmphex(rd(&t, MBOX(2)), ==, cases[i].alpa);
        isp_ack(&t);
        assert_id_list(&t, "ef:0 e4:2");
        /* An HCCR reset drops the firmware and the loop ID with it. */
        wr(&t, REG_HCCR, HCCR_RESET);
        wr(&t, REG_HCCR, HCCR_RELEASE);
    }
    isp_close(&t);
}

/*
 * State 0 until initialise-firmware, 1 while the loop comes up, 3 after.
 * Until ready, Get Adapter ID refuses with m1 7, Linux's "loop in
 * transition", and the discovery commands refuse too.
 */
static void test_loop_state_timeline(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    uint64_t buf = guest_alloc(alloc, 128);
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_CONFIG_WAIT);
    g_assert_cmphex(isp_call(&t, CMD_GET_ADAPTER_ID, 0, 0, 0), ==,
                    MBOX_COMMAND_ERROR);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 7);
    isp_ack(&t);
    /* A LIP before the loop exists is refused and starts nothing. */
    g_assert_cmphex(isp_call_ack(&t, CMD_INITIATE_LIP, 0, 0, 0), ==,
                    MBOX_COMMAND_ERROR);
    g_assert_cmphex(isp_call_ack(&t, CMD_FULL_LOGIN_LIP, 0, 0, 0), ==,
                    MBOX_COMMAND_ERROR);

    isp_rings(&t, 8, 8);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_WAIT_AL_PA);
    g_assert_cmphex(isp_call(&t, CMD_GET_ADAPTER_ID, 0, 0, 0), ==,
                    MBOX_COMMAND_ERROR);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 7);
    isp_ack(&t);
    g_assert_cmphex(isp_get_id_list(&t, buf), ==, MBOX_COMMAND_ERROR);
    isp_ack(&t);
    g_assert_cmphex(isp_call_dma(&t, CMD_PORT_NAME_LIST, 0, buf), ==,
                    MBOX_COMMAND_ERROR);
    isp_ack(&t);
    g_assert_cmphex(isp_call_dma(&t, CMD_POSITION_MAP, 0, buf), ==,
                    MBOX_COMMAND_ERROR);
    isp_ack(&t);

    /* The bring-up time is a model constant; one tick short is not up. */
    qtest_clock_step(t.qts, BRINGUP_NS - 1);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_WAIT_AL_PA);
    isp_expect_no_event(&t);
    qtest_clock_step(t.qts, 1);
    isp_expect_events(&t, bringup_events, 2);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_READY);

    /* An adapter alone on the loop is ready with an empty list. */
    g_assert_cmphex(isp_get_id_list(&t, buf), ==, MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 0);
    isp_ack(&t);

    /* A host LIP brings the loop up again. */
    g_assert_cmphex(isp_call_ack(&t, CMD_FULL_LOGIN_LIP, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_WAIT_AL_PA);
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_events(&t, bringup_events, 2);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_READY);

    /* A reset takes the loop back to state 0. */
    wr(&t, REG_HCCR, HCCR_RESET);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    isp_start(&t);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_CONFIG_WAIT);
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_no_event(&t);
    isp_close(&t);
}

/*
 * Approximation: the connection mode in extended options bits 6:4. Loop
 * only, loop preferred and point-to-point preferred bring the loop up;
 * point-to-point only and the undefined values leave state 0 and post
 * nothing. A reduced-completion mode in bits 3:0 changes nothing.
 */
static void test_loop_connection_mode(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    static const struct {
        uint8_t xopts;
        bool up;
    } cases[] = {
        { 0x00, true }, { 0x20, true }, { 0x30, true }, { 0x24, true },
        { 0x10, false }, { 0x40, false }, { 0x70, false },
    };
    unsigned i;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        isp_rings_at(&t, 8, 8, guest_alloc(alloc, ICB_SIZE),
                     guest_alloc(alloc, 8 * ENTRY_SIZE),
                     guest_alloc(alloc, 8 * ENTRY_SIZE), 0, cases[i].xopts);
        qtest_clock_step(t.qts, BRINGUP_NS);
        if (cases[i].up) {
            isp_expect_events(&t, bringup_events, 2);
            g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_READY);
        } else {
            isp_expect_no_event(&t);
            g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_CONFIG_WAIT);
        }
    }
    isp_close(&t);
}

/*
 * Approximation: the port-database event follows loop up only with
 * control-block option 0x0100. A new trigger drops every event an earlier
 * bring-up queued and has not posted; one already posted stays until the
 * host releases it, and a mailbox result waits behind it intact.
 */
static void test_loop_events(void *obj, void *data, QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, 0);

    /* Let the next bring-up post its first event and leave it unread. */
    g_assert_cmphex(isp_call_ack(&t, CMD_INITIATE_LIP, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    qtest_clock_step(t.qts, BRINGUP_NS);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, ASYNC_LIP);

    /*
     * The host reads the event, then runs a second trigger before it
     * releases it; the result waits in its slot until the release.
     */
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 0);
    isp_call(&t, CMD_INITIATE_LIP, 0, 0, 0);
    g_assert_cmphex(rd(&t, REG_HCCR) & HCCR_HOST_INT, ==, 0);
    isp_ack(&t);
    /* Its result, then nothing: loop up from the first one is gone. */
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    isp_ack(&t);
    isp_expect_no_event(&t);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_WAIT_AL_PA);

    /* The second bring-up posts its own whole set. */
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_events(&t, bringup_events, 2);

    /* An initialisation with option 0x0100 set adds the third event. */
    isp_rings_at(&t, 8, 8, guest_alloc(alloc, ICB_SIZE),
                 guest_alloc(alloc, 8 * ENTRY_SIZE),
                 guest_alloc(alloc, 8 * ENTRY_SIZE), ICB_OPT_PORT_DB, 0);
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_events(&t, bringup_events, 3);
    isp_close(&t);
}

/* One status entry, polled for: disk I/O completes from the main loop. */
static void isp_wait_response(Isp *t, uint8_t *e)
{
    unsigned i;

    for (i = 0; i < 10000 && !(rd(t, REG_ISR) & ISR_RISC); i++) {
        g_usleep(1000);
    }
    g_assert_cmphex(rd(t, REG_ISR) & ISR_RISC, ==, ISR_RISC);
    g_assert_cmphex(rd(t, REG_SEMAPHORE), ==, 0);
    g_assert_cmphex(rd(t, REG_RESPONSE), ==, (t->rs_next + 1) % t->rslen);
    isp_response(t, t->rs_next, e);
    g_assert_cmphex(e[0], ==, ENTRY_STATUS);
    isp_consume(t, 1);
}

typedef struct ScsiResult {
    uint16_t scsi_status;
    uint16_t completion;
    uint16_t sense_len;
    uint32_t residual;
    uint8_t sense[32];
} ScsiResult;

static void decode_status(const uint8_t *e, uint32_t handle, ScsiResult *r)
{
    g_assert_cmphex(ldl_le_p(e + 4), ==, handle);
    r->scsi_status = lduw_le_p(e + 8);
    r->completion = lduw_le_p(e + 10);
    /* State and status flags: unknown meaning, written as zero. */
    g_assert_cmphex(lduw_le_p(e + 12), ==, 0);
    g_assert_cmphex(lduw_le_p(e + 14), ==, 0);
    r->sense_len = lduw_le_p(e + 18);
    r->residual = ldl_le_p(e + 20);
    memcpy(r->sense, e + 32, sizeof(r->sense));
}

/*
 * A 64-bit command with at most two segments, published and waited for.
 * @control is the direction flag; the segments split @len at @split.
 */
static void isp_scsi(Isp *t, uint32_t handle, uint8_t loop_id, uint16_t lun,
                     const uint8_t *cdb, unsigned cdb_len, uint16_t control,
                     uint64_t buf, uint32_t len, uint32_t split,
                     ScsiResult *r)
{
    uint8_t e[ENTRY_SIZE];

    entry_command(e, ENTRY_CMD64, handle, loop_id, lun, cdb[0]);
    memcpy(e + 20, cdb, cdb_len);
    stw_le_p(e + 12, control);
    stl_le_p(e + 36, len);
    if (len && split && split < len) {
        stw_le_p(e + 18, 2);
        put_seg64(e + 40, buf, split);
        put_seg64(e + 52, buf + split, len - split);
    } else if (len) {
        stw_le_p(e + 18, 1);
        put_seg64(e + 40, buf, len);
    }
    isp_publish(t, e);
    isp_wait_response(t, e);
    decode_status(e, handle, r);
}

static const uint8_t cdb_tur[6] = { 0x00 };

/*
 * The first command each disk takes after a reset reports the reset as
 * a unit attention; INQUIRY is exempt. Clear it on every port.
 */
static void isp_clear_unit_attention(Isp *t, const LoopConfig *config)
{
    ScsiResult r;
    unsigned i;

    for (i = 0; i < config->ndisks; i++) {
        isp_scsi(t, 0x7700 + i, config->disks[i].loop_id,
                 config->disks[i].lun, cdb_tur, sizeof(cdb_tur), 0, 0, 0, 0,
                 &r);
        g_assert_cmphex(r.completion, ==, COMPLETION_COMPLETE);
        if (r.scsi_status) {
            g_assert_cmphex(r.scsi_status, ==,
                            SCSI_CHECK_CONDITION | SCSI_STATUS_SENSE_VALID);
            g_assert_cmphex(r.sense[2] & 0x0f, ==, 0x06);
        }
    }
}

static void cdb_inquiry(uint8_t *cdb, bool evpd, uint8_t page,
                        uint16_t alloc_len)
{
    memset(cdb, 0, 6);
    cdb[0] = 0x12;
    cdb[1] = evpd;
    cdb[2] = page;
    stw_be_p(cdb + 3, alloc_len);
}

static void cdb_rw10(uint8_t *cdb, uint8_t op, uint32_t lba, uint16_t blocks)
{
    memset(cdb, 0, 10);
    cdb[0] = op;
    stl_be_p(cdb + 2, lba);
    stw_be_p(cdb + 7, blocks);
}

/*
 * INQUIRY reaches the disk at its loop ID: standard data, and the
 * device-identification page carries the same port and node names as
 * the loop's port database.
 */
static void test_scsi_inquiry(void *obj, void *data, QGuestAllocator *alloc)
{
    uint64_t buf = guest_alloc(alloc, 256);
    uint8_t cdb[6], page[256], name[8];
    ScsiResult r;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);

    cdb_inquiry(cdb, false, 0, 36);
    isp_scsi(&t, 0x1201, 5, 0, cdb, sizeof(cdb), 0x20, buf, 36, 0, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_COMPLETE);
    g_assert_cmphex(r.scsi_status, ==, 0);
    g_assert_cmphex(r.residual, ==, 0);
    g_assert_cmphex(r.sense_len, ==, 0);
    qtest_memread(t.qts, buf, page, 36);
    g_assert_cmphex(page[0], ==, 0x00);
    g_assert_cmpint(memcmp(page + 8, "QEMU    ", 8), ==, 0);
    g_assert_cmpint(memcmp(page + 16, "QEMU HARDDISK   ", 16), ==, 0);

    cdb_inquiry(cdb, true, 0x83, 255);
    qtest_memset(t.qts, buf, 0, 256);
    isp_scsi(&t, 0x1202, 5, 0, cdb, sizeof(cdb), 0x20, buf, 255, 0, &r);
    g_assert_cmphex(r.scsi_status & 0xff, ==, 0);
    qtest_memread(t.qts, buf, page, sizeof(page));
    g_assert_cmphex(page[1], ==, 0x83);
    stq_be_p(name, WWN_PORT(5));
    g_assert_true(has_bytes(page, sizeof(page), name, sizeof(name)));
    stq_be_p(name, WWN_NODE(5));
    g_assert_true(has_bytes(page, sizeof(page), name, sizeof(name)));
    isp_close(&t);
}

/*
 * WRITE(10) then READ(10) of the same eight blocks, with the segments
 * split differently each way; the data lands in the disk's backing file
 * and comes back the same.
 */
static void test_scsi_read_write(void *obj, void *data,
                                 QGuestAllocator *alloc)
{
    LoopTest *test = data;
    const uint32_t len = 8 * 512;
    uint64_t out = guest_alloc(alloc, len), in = guest_alloc(alloc, len);
    uint64_t list = guest_alloc(alloc, 3 * 12);
    g_autofree uint8_t *pattern = g_malloc(len);
    g_autofree uint8_t *seen = g_malloc(len);
    uint8_t cdb[10], e[ENTRY_SIZE], segs[3 * 12];
    ScsiResult r;
    unsigned i;
    int fd;
    Isp t;

    isp_open(&t, obj, alloc);
    for (i = 0; i < len; i++) {
        pattern[i] = i * 7 + (i >> 9) + 1;
    }
    qtest_memwrite(t.qts, out, pattern, len);
    qtest_memset(t.qts, in, 0, len);
    isp_loop_up(&t, ICB_OPT_PORT_DB);
    isp_clear_unit_attention(&t, &loop_config);

    cdb_rw10(cdb, 0x2a, 16, 8);
    isp_scsi(&t, 0x2a2a, 5, 0, cdb, sizeof(cdb), 0x40, out, len, 1536, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_COMPLETE);
    g_assert_cmphex(r.scsi_status, ==, 0);
    g_assert_cmphex(r.residual, ==, 0);

    /* The data is in the file, at block 16. */
    fd = open(test->path[0], O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, seen, len, 16 * 512), ==, len);
    close(fd);
    g_assert_cmpint(memcmp(seen, pattern, len), ==, 0);

    /* Read back through OpenBSD's segment-list form, three pieces. */
    put_seg64(segs, in, 1024);
    put_seg64(segs + 12, in + 1024, 1024);
    put_seg64(segs + 24, in + 2048, 2048);
    qtest_memwrite(t.qts, list, segs, sizeof(segs));
    cdb_rw10(cdb, 0x28, 16, 8);
    entry_command(e, ENTRY_CMD_LIST, 0x2828, 5, 0, 0x28);
    memcpy(e + 20, cdb, sizeof(cdb));
    stw_le_p(e + 12, 0x20);
    stw_le_p(e + 18, 3);
    stl_le_p(e + 36, len);
    stw_le_p(e + 40, 1);
    stq_le_p(e + 46, list);
    isp_publish(&t, e);
    isp_wait_response(&t, e);
    decode_status(e, 0x2828, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_COMPLETE);
    g_assert_cmphex(r.scsi_status, ==, 0);
    qtest_memread(t.qts, in, seen, len);
    g_assert_cmpint(memcmp(seen, pattern, len), ==, 0);
    isp_close(&t);
}

/*
 * Commands go to the loop ID and LUN they name: the unit serial number
 * page tells the three disks apart.
 */
static void test_scsi_addressing(void *obj, void *data,
                                 QGuestAllocator *alloc)
{
    static const struct {
        uint8_t loop_id;
        uint16_t lun;
        const char *serial;
    } cases[] = {
        { 5, 1, "LUN1" }, { 5, 0, "LOOP5" }, { 1, 0, "LOOP1" },
    };
    uint64_t buf = guest_alloc(alloc, 64);
    uint8_t cdb[6], page[64];
    ScsiResult r;
    unsigned i;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);
    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        size_t n = strlen(cases[i].serial);

        cdb_inquiry(cdb, true, 0x80, 64);
        qtest_memset(t.qts, buf, 0, sizeof(page));
        isp_scsi(&t, 0x8000 + i, cases[i].loop_id, cases[i].lun, cdb,
                 sizeof(cdb), 0x20, buf, sizeof(page), 0, &r);
        g_assert_cmphex(r.scsi_status & 0xff, ==, 0);
        qtest_memread(t.qts, buf, page, sizeof(page));
        g_assert_cmphex(page[1], ==, 0x80);
        g_assert_cmpuint(page[3], ==, n);
        g_assert_cmpint(memcmp(page + 4, cases[i].serial, n), ==, 0);
    }
    isp_close(&t);
}

/*
 * Approximation: completion precedence. A failed SCSI status completes
 * 0x0000 with the status, sense valid and the sense bytes, and with
 * residual-under and the residual when the transfer was short. A good
 * short transfer is underrun 0x0015 with the residual; a good full one
 * is 0x0000 with no flags.
 */
static void test_scsi_check_condition(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    uint64_t buf = guest_alloc(alloc, 1024);
    uint8_t cdb[12], page[256];
    ScsiResult r;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);

    /* The reset's unit attention: CHECK CONDITION with no data asked. */
    isp_scsi(&t, 0x0601, 1, 0, cdb_tur, sizeof(cdb_tur), 0, 0, 0, 0, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_COMPLETE);
    g_assert_cmphex(r.scsi_status, ==,
                    SCSI_CHECK_CONDITION | SCSI_STATUS_SENSE_VALID);
    g_assert_cmpuint(r.sense_len, >=, 14);
    g_assert_cmphex(r.sense[0] & 0x7f, ==, 0x70);
    g_assert_cmphex(r.sense[2] & 0x0f, ==, 0x06);
    g_assert_cmphex(r.sense[12], ==, 0x29);
    g_assert_cmphex(r.residual, ==, 0);
    isp_scsi(&t, 0x0602, 1, 0, cdb_tur, sizeof(cdb_tur), 0, 0, 0, 0, &r);
    g_assert_cmphex(r.scsi_status, ==, 0);
    g_assert_cmphex(r.sense_len, ==, 0);

    /* A READ to a LUN the port lacks moves nothing: all 512 are residual. */
    cdb_rw10(cdb, 0x28, 0, 1);
    isp_scsi(&t, 0x0603, 1, 7, cdb, 10, 0x20, buf, 512, 0, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_COMPLETE);
    g_assert_cmphex(r.scsi_status, ==,
                    SCSI_CHECK_CONDITION | SCSI_STATUS_SENSE_VALID |
                    SCSI_STATUS_RESID_UNDER);
    g_assert_cmphex(r.sense[2] & 0x0f, ==, 0x05);
    g_assert_cmphex(r.sense[12], ==, 0x25);
    g_assert_cmphex(r.residual, ==, 512);

    /* INQUIRY to that LUN: peripheral qualifier 3, no device. */
    cdb_inquiry(cdb, false, 0, 36);
    qtest_memset(t.qts, buf, 0, 36);
    isp_scsi(&t, 0x0604, 1, 7, cdb, 6, 0x20, buf, 36, 0, &r);
    g_assert_cmphex(r.scsi_status & 0xff, ==, 0);
    qtest_memread(t.qts, buf, page, 36);
    g_assert_cmphex(page[0], ==, 0x7f);

    /* A LUN past 255: CHECK CONDITION, the same sense, nothing moved. */
    cdb_rw10(cdb, 0x28, 0, 1);
    isp_scsi(&t, 0x0605, 1, 300, cdb, 10, 0x20, buf, 512, 0, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_COMPLETE);
    g_assert_cmphex(r.scsi_status, ==,
                    SCSI_CHECK_CONDITION | SCSI_STATUS_SENSE_VALID |
                    SCSI_STATUS_RESID_UNDER);
    g_assert_cmphex(r.sense[2] & 0x0f, ==, 0x05);
    g_assert_cmphex(r.sense[12], ==, 0x25);
    g_assert_cmphex(r.residual, ==, 512);

    /*
     * A good transfer shorter than the host's buffer is an underrun:
     * REPORT LUNS on loop ID 5 lists its two LUNs in 24 bytes.
     */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xa0;
    stl_be_p(cdb + 6, 255);
    qtest_memset(t.qts, buf, 0, 256);
    isp_scsi(&t, 0x0606, 5, 0, cdb, 12, 0x20, buf, 255, 0, &r);
    qtest_memread(t.qts, buf, page, sizeof(page));
    g_assert_cmphex(ldl_be_p(page), ==, 16);
    g_assert_cmphex(r.completion, ==, COMPLETION_UNDERRUN);
    g_assert_cmphex(r.scsi_status, ==, SCSI_STATUS_RESID_UNDER);
    g_assert_cmphex(r.residual, ==, 255 - 24);
    g_assert_cmphex(r.sense_len, ==, 0);

    /* So is one block read into two blocks' worth of segments. */
    cdb_rw10(cdb, 0x28, 0, 1);
    isp_scsi(&t, 0x0607, 1, 0, cdb, 10, 0x20, buf, 512, 0, &r);
    isp_scsi(&t, 0x0608, 1, 0, cdb, 10, 0x20, buf, 1024, 512, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_UNDERRUN);
    g_assert_cmphex(r.scsi_status, ==, SCSI_STATUS_RESID_UNDER);
    g_assert_cmphex(r.residual, ==, 512);

    /* And a good full READ has no flags at all. */
    cdb_rw10(cdb, 0x28, 0, 1);
    isp_scsi(&t, 0x0609, 1, 0, cdb, 10, 0x20, buf, 512, 256, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_COMPLETE);
    g_assert_cmphex(r.scsi_status, ==, 0);
    g_assert_cmphex(r.residual, ==, 0);
    isp_close(&t);
}

/*
 * Approximation: a command to a loop ID with no port, to the adapter's
 * own loop ID or past 125 completes "port unavailable" with no SCSI
 * status. A marker between commands answers nothing.
 */
static void test_scsi_unavailable(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    static const uint8_t loop_ids[] = { 2, 0, 126, 255 };
    uint8_t e[ENTRY_SIZE] = { ENTRY_MARKER, 1 };
    ScsiResult r;
    unsigned i;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);
    for (i = 0; i < ARRAY_SIZE(loop_ids); i++) {
        isp_scsi(&t, 0x2800 + i, loop_ids[i], 0, cdb_tur, sizeof(cdb_tur),
                 0, 0, 0, 0, &r);
        g_assert_cmphex(r.completion, ==, COMPLETION_PORT_UNAVAILABLE);
        g_assert_cmphex(r.scsi_status, ==, 0);
        g_assert_cmphex(r.residual, ==, 0);

        e[9] = loop_ids[i];
        e[10] = 1;
        isp_publish(&t, e);
        g_assert_cmphex(rd(&t, REG_REQUEST), ==, t.rq_next);
        isp_expect_no_event(&t);
    }
    isp_close(&t);
}

/* A device_add onto the adapter's bus that must fail, saying @why. */
static void G_GNUC_PRINTF(3, 4) qmp_device_add_fails(QTestState *qts,
                                                     const char *why,
                                                     const char *fmt, ...)
{
    QDict *args, *rsp, *err;
    va_list ap;

    va_start(ap, fmt);
    args = qdict_from_vjsonf_nofail(fmt, ap);
    va_end(ap);
    qdict_put_str(args, "driver", "scsi-hd");
    qdict_put_str(args, "bus", "isp0.0");
    qdict_put_str(args, "drive", "spare0");

    rsp = qtest_qmp(qts, "{ 'execute': 'device_add', 'arguments': %p }",
                    args);
    g_assert_true(qdict_haskey(rsp, "error"));
    err = qdict_get_qdict(rsp, "error");
    if (!strstr(qdict_get_str(err, "desc"), why)) {
        g_test_message("device_add error: %s", qdict_get_str(err, "desc"));
    }
    g_assert_nonnull(strstr(qdict_get_str(err, "desc"), why));
    qobject_unref(rsp);
}

/*
 * A plug that makes a new port, or an unplug of a port's last LUN, runs
 * a bring-up; another LUN of a port that exists does not. A port needs
 * both names, one pair of names per loop ID, its own port name, and a
 * loop ID the adapter does not hold.
 */
static void test_loop_plug(void *obj, void *data, QGuestAllocator *alloc)
{
    ScsiResult r;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);

    qmp_device_add_fails(t.qts, "port_wwn",
                         "{ 'scsi-id': 9, 'lun': 0, 'wwn': 1 }");
    qmp_device_add_fails(t.qts, "wwn",
                         "{ 'scsi-id': 9, 'lun': 0, 'port_wwn': 1 }");
    qmp_device_add_fails(t.qts, "loop ID 5",
                         "{ 'scsi-id': 5, 'lun': 1, 'port_wwn': 2, "
                         "'wwn': 3 }");
    qmp_device_add_fails(t.qts, "loop ID 5",
                         "{ 'scsi-id': 9, 'lun': 0, 'port_wwn': %" PRIu64
                         ", 'wwn': 3 }", WWN_PORT(5));
    qmp_device_add_fails(t.qts, "adapter",
                         "{ 'scsi-id': 0, 'lun': 0, 'port_wwn': 2, "
                         "'wwn': 3 }");
    qmp_device_add_fails(t.qts, "scsi-id",
                         "{ 'lun': 0, 'port_wwn': 2, 'wwn': 3 }");
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_no_event(&t);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_READY);

    /* A new port at loop ID 9 (AL_PA 0xd6). */
    qtest_qmp_device_add(t.qts, "scsi-hd", "hot0",
                         "{ 'bus': 'isp0.0', 'drive': 'spare0', "
                         "'scsi-id': 9, 'lun': 0, 'port_wwn': %" PRIu64 ", "
                         "'wwn': %" PRIu64 " }", WWN_PORT(9), WWN_NODE(9));
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_WAIT_AL_PA);
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_events(&t, bringup_events, 3);
    assert_id_list(&t, "e0:5 d6:9");

    /* Its second LUN joins an existing port: no bring-up. */
    qtest_qmp_device_add(t.qts, "scsi-hd", "hot1",
                         "{ 'bus': 'isp0.0', 'drive': 'spare1', "
                         "'scsi-id': 9, 'lun': 1, 'port_wwn': %" PRIu64 ", "
                         "'wwn': %" PRIu64 " }", WWN_PORT(9), WWN_NODE(9));
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_no_event(&t);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_READY);

    /* Unplugging that LUN leaves the port: still no bring-up. */
    qtest_qmp_device_del(t.qts, "hot1");
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_no_event(&t);

    /* Unplugging the last one takes the port off the loop. */
    qtest_qmp_device_del(t.qts, "hot0");
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_WAIT_AL_PA);
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_events(&t, bringup_events, 3);
    assert_id_list(&t, "e0:5");
    isp_scsi(&t, 0x0900, 9, 0, cdb_tur, sizeof(cdb_tur), 0, 0, 0, 0, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_PORT_UNAVAILABLE);

    /* The bus now differs from the command line's. */
    qos_invalidate_command_line();
    isp_close(&t);
}

/*
 * Three ports' worth of disks whose reads take a second, so a command is
 * still in flight when the test acts on it. A cancellation waits for the
 * backend, so a recovery here takes up to that second too.
 */
static const LoopDisk slow_disks[] = {
    { 5, 0, WWN_PORT(5), WWN_NODE(5), NULL, LOOP_SLOW },
    { 5, 1, WWN_PORT(5), WWN_NODE(5), NULL, LOOP_SLOW },
    { 1, 0, WWN_PORT(1), WWN_NODE(1), NULL, LOOP_SLOW },
};

static const LoopConfig slow_config = {
    .disks = slow_disks,
    .ndisks = ARRAY_SIZE(slow_disks),
};

#define CMD_ABORT_TARGET    0x0017
#define CMD_LIP_RESET       0x006c
#define CMD_LUN_RESET       0x007e

#define COMPLETION_DMA_ERROR    0x0002
#define COMPLETION_RESET        0x0004
#define COMPLETION_ABORTED      0x0005
#define COMPLETION_LOGGED_OUT   0x0029

/* A one-block READ, published and left in flight. */
static void isp_submit_read(Isp *t, uint32_t handle, uint8_t loop_id,
                            uint16_t lun, uint64_t buf)
{
    uint8_t e[ENTRY_SIZE], cdb[10];

    cdb_rw10(cdb, 0x28, 0, 1);
    entry_command(e, ENTRY_CMD64, handle, loop_id, lun, 0x28);
    memcpy(e + 20, cdb, sizeof(cdb));
    stw_le_p(e + 12, 0x20);
    stl_le_p(e + 36, 512);
    stw_le_p(e + 18, 1);
    put_seg64(e + 40, buf, 512);
    isp_publish(t, e);
}

typedef struct Expect {
    uint32_t handle;
    uint16_t completion;
} Expect;

/*
 * Status entries until each of @want has come, with its completion, and
 * nothing else; each batch is consumed as it arrives. Order is not
 * checked: a command elsewhere finishes while a recovery waits on its
 * own cancellation, so which posts first depends on the backends.
 */
static void isp_collect(Isp *t, const Expect *want, unsigned n)
{
    bool seen[8] = { false };
    unsigned got = 0, i, j;

    g_assert_cmpuint(n, <=, ARRAY_SIZE(seen));
    while (got < n) {
        uint16_t in;

        for (i = 0; i < 10000 && !(rd(t, REG_ISR) & ISR_RISC); i++) {
            g_usleep(1000);
        }
        g_assert_cmphex(rd(t, REG_ISR) & ISR_RISC, ==, ISR_RISC);
        g_assert_cmphex(rd(t, REG_SEMAPHORE), ==, 0);
        in = rd(t, REG_RESPONSE);
        while (t->rs_next != in) {
            uint8_t e[ENTRY_SIZE];
            uint32_t handle;

            isp_response(t, t->rs_next, e);
            g_assert_cmphex(e[0], ==, ENTRY_STATUS);
            handle = ldl_le_p(e + 4);
            for (j = 0; j < n && (seen[j] || want[j].handle != handle); j++) {
                ;
            }
            if (j == n) {
                g_test_message("unexpected handle 0x%08x", handle);
            }
            g_assert_cmpuint(j, <, n);
            g_assert_cmphex(lduw_le_p(e + 10), ==, want[j].completion);
            g_assert_cmphex(lduw_le_p(e + 8), ==, 0);
            seen[j] = true;
            got++;
            t->rs_next = (t->rs_next + 1) % t->rslen;
        }
        wr(t, REG_RESPONSE, t->rs_next);
        wr(t, REG_HCCR, HCCR_CLEAR_RISC);
    }
}

/* What a TUR on the port finds: 0 good, else the sense key. */
static int isp_tur_sense(Isp *t, uint8_t loop_id, uint16_t lun)
{
    ScsiResult r;

    isp_scsi(t, 0x7e00, loop_id, lun, cdb_tur, sizeof(cdb_tur), 0, 0, 0, 0,
             &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_COMPLETE);
    return r.scsi_status ? r.sense[2] & 0x0f : 0;
}

static uint16_t isp_abort(Isp *t, uint8_t loop_id, uint16_t lun,
                          uint32_t handle)
{
    const uint16_t in[] = {
        CMD_ABORT_IOCB, loop_id << 8, handle & 0xffff, handle >> 16, 0, 0,
        lun,
    };
    uint16_t status = isp_mbox(t, in, ARRAY_SIZE(in));

    isp_ack(t);
    return status;
}

/*
 * Approximation: abort matches the entry's handle bytes read as a
 * little-endian word against m3:m2, and the loop ID and LUN. A handle
 * stored most significant byte first does not match a small m2; one
 * stored least significant first does, and completes "aborted" with its
 * own four bytes. The other command finishes normally.
 */
static void test_abort_command(void *obj, void *data, QGuestAllocator *alloc)
{
    static const Expect want[] = {
        { 0x00000005, COMPLETION_ABORTED },
        { 0x05000000, COMPLETION_COMPLETE },
    };
    uint64_t buf = guest_alloc(alloc, 1024);
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);
    isp_clear_unit_attention(&t, &slow_config);

    /* Entry bytes 00 00 00 05, then 05 00 00 00. */
    isp_submit_read(&t, 0x05000000, 5, 0, buf);
    isp_submit_read(&t, 0x00000005, 5, 0, buf + 512);
    isp_expect_no_event(&t);

    /* The right handle at the wrong LUN or loop ID matches nothing. */
    g_assert_cmphex(isp_abort(&t, 5, 1, 5), ==, MBOX_COMMAND_ERROR);
    g_assert_cmphex(isp_abort(&t, 1, 0, 5), ==, MBOX_COMMAND_ERROR);
    g_assert_cmphex(isp_abort(&t, 5, 0, 0x00050000), ==, MBOX_COMMAND_ERROR);

    /* The mailbox result posts before any entry. */
    g_assert_cmphex(isp_abort(&t, 5, 0, 5), ==, MBOX_COMPLETE);
    isp_collect(&t, want, ARRAY_SIZE(want));

    /* Gone now: a second abort finds nothing. */
    g_assert_cmphex(isp_abort(&t, 5, 0, 5), ==, MBOX_COMMAND_ERROR);
    isp_close(&t);
}

/*
 * Approximation: abort target retires every command at the loop ID with
 * "reset" and resets every LUN there; LUN reset does the same for one
 * LUN. Each leaves the other ports alone, and the reset devices report it
 * as a unit attention.
 */
static void test_target_and_lun_reset(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    static const Expect target_want[] = {
        { 0xa0, COMPLETION_RESET },
        { 0xa1, COMPLETION_RESET },
        { 0xa2, COMPLETION_COMPLETE },
    };
    static const Expect lun_want[] = {
        { 0xb1, COMPLETION_RESET },
        { 0xb0, COMPLETION_COMPLETE },
    };
    uint64_t buf = guest_alloc(alloc, 2048);
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);
    isp_clear_unit_attention(&t, &slow_config);

    isp_submit_read(&t, 0xa0, 5, 0, buf);
    isp_submit_read(&t, 0xa1, 5, 1, buf + 512);
    isp_submit_read(&t, 0xa2, 1, 0, buf + 1024);
    g_assert_cmphex(isp_call_ack(&t, CMD_ABORT_TARGET, 5 << 8, 0, 0), ==,
                    MBOX_COMPLETE);
    isp_collect(&t, target_want, ARRAY_SIZE(target_want));
    g_assert_cmpint(isp_tur_sense(&t, 5, 0), ==, 0x06);
    g_assert_cmpint(isp_tur_sense(&t, 5, 1), ==, 0x06);
    g_assert_cmpint(isp_tur_sense(&t, 1, 0), ==, 0);

    isp_submit_read(&t, 0xb0, 5, 0, buf);
    isp_submit_read(&t, 0xb1, 5, 1, buf + 512);
    g_assert_cmphex(isp_call_ack(&t, CMD_LUN_RESET, 5 << 8, 1, 0), ==,
                    MBOX_COMPLETE);
    isp_collect(&t, lun_want, ARRAY_SIZE(lun_want));
    g_assert_cmpint(isp_tur_sense(&t, 5, 1), ==, 0x06);
    g_assert_cmpint(isp_tur_sense(&t, 5, 0), ==, 0);

    /* An empty loop ID: nothing to retire, still complete. */
    g_assert_cmphex(isp_call_ack(&t, CMD_ABORT_TARGET, 7 << 8, 0, 0), ==,
                    MBOX_COMPLETE);
    isp_expect_no_event(&t);
    isp_close(&t);
}

static const uint16_t lip_reset_quiet_events[] = {
    ASYNC_LOOP_UP, ASYNC_PORT_DB,
};

static const uint16_t lip_reset_events[] = {
    ASYNC_LIP_RESET, ASYNC_LOOP_UP, ASYNC_PORT_DB,
};

/*
 * LIP reset retires the named loop ID's commands, or every port's for
 * 0xff, resets those targets and brings the loop up again. Approximation:
 * with set-firmware-options bit 1 clear it posts loop up and port
 * database changed but no LIP event; with it set, LIP reset first.
 */
static void test_lip_reset(void *obj, void *data, QGuestAllocator *alloc)
{
    static const Expect want[] = {
        { 0xc5, COMPLETION_RESET },
        { 0xc1, COMPLETION_COMPLETE },
    };
    uint64_t buf = guest_alloc(alloc, 1024);
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    g_assert_cmphex(isp_call_ack(&t, CMD_LIP_RESET, 0xff00, 0, 0), ==,
                    MBOX_COMMAND_ERROR);
    isp_loop_up(&t, ICB_OPT_PORT_DB);
    isp_clear_unit_attention(&t, &slow_config);

    isp_submit_read(&t, 0xc5, 5, 0, buf);
    isp_submit_read(&t, 0xc1, 1, 0, buf + 512);
    g_assert_cmphex(isp_call_ack(&t, CMD_LIP_RESET, 5 << 8, 0, 0), ==,
                    MBOX_COMPLETE);
    isp_collect(&t, want, ARRAY_SIZE(want));
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_WAIT_AL_PA);
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_events(&t, lip_reset_quiet_events,
                      ARRAY_SIZE(lip_reset_quiet_events));
    g_assert_cmpint(isp_tur_sense(&t, 5, 0), ==, 0x06);
    g_assert_cmpint(isp_tur_sense(&t, 1, 0), ==, 0);

    g_assert_cmphex(isp_call_ack(&t, CMD_SET_FW_OPTIONS, 0x0002, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(isp_call_ack(&t, CMD_LIP_RESET, 0xff00, 0, 0), ==,
                    MBOX_COMPLETE);
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_events(&t, lip_reset_events, ARRAY_SIZE(lip_reset_events));
    g_assert_cmpint(isp_tur_sense(&t, 5, 0), ==, 0x06);
    g_assert_cmpint(isp_tur_sense(&t, 5, 1), ==, 0x06);
    g_assert_cmpint(isp_tur_sense(&t, 1, 0), ==, 0x06);
    isp_close(&t);
}

/*
 * A port leaving the loop completes its commands "port logged out"
 * before it goes. Unplugging one LUN of two retires only that LUN's
 * commands and runs no bring-up; unplugging the last runs one, and the
 * loop ID is then unavailable. Other ports' commands finish normally.
 */
static void test_unplug_disconnect(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    static const Expect lun0_want[] = {
        { 0xd0, COMPLETION_LOGGED_OUT },
        { 0xd1, COMPLETION_LOGGED_OUT },
        { 0xd3, COMPLETION_COMPLETE },
    };
    static const Expect lun1_want[] = {
        { 0xd2, COMPLETION_LOGGED_OUT },
    };
    uint64_t buf = guest_alloc(alloc, 2048);
    ScsiResult r;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);
    isp_clear_unit_attention(&t, &slow_config);

    isp_submit_read(&t, 0xd0, 5, 0, buf);
    isp_submit_read(&t, 0xd1, 5, 0, buf + 512);
    isp_submit_read(&t, 0xd3, 1, 0, buf + 1536);
    qtest_qmp_device_del(t.qts, "port0");
    isp_collect(&t, lun0_want, ARRAY_SIZE(lun0_want));
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_no_event(&t);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_READY);
    assert_id_list(&t, "e8:1 e0:5");

    isp_submit_read(&t, 0xd2, 5, 1, buf + 1024);
    qtest_qmp_device_del(t.qts, "port1");
    isp_collect(&t, lun1_want, ARRAY_SIZE(lun1_want));
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_events(&t, bringup_events, 3);
    assert_id_list(&t, "e8:1");
    isp_scsi(&t, 0xd4, 5, 0, cdb_tur, sizeof(cdb_tur), 0, 0, 0, 0, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_PORT_UNAVAILABLE);

    qos_invalidate_command_line();
    isp_close(&t);
}

/* Reads the drive's backend has finished, cancelled ones included. */
static int64_t isp_reads_done(Isp *t, const char *drive)
{
    QDict *rsp = qtest_qmp(t->qts, "{ 'execute': 'query-blockstats' }");
    QListEntry *entry;
    int64_t done = -1;

    QLIST_FOREACH_ENTRY(qdict_get_qlist(rsp, "return"), entry) {
        QDict *dev = qobject_to(QDict, qlist_entry_obj(entry));

        if (qdict_haskey(dev, "device") &&
            !strcmp(qdict_get_str(dev, "device"), drive)) {
            done = qdict_get_int(qdict_get_qdict(dev, "stats"),
                                 "rd_operations");
        }
    }
    qobject_unref(rsp);
    g_assert_cmpint(done, >=, 0);
    return done;
}

/*
 * Until the drive's backend has finished @want reads. A slow disk's read
 * still running when the model should have cancelled it ends here, and a
 * command the model failed to cancel would deliver its data and complete
 * by the time this returns.
 */
static void isp_wait_reads(Isp *t, const char *drive, int64_t want)
{
    unsigned i;

    for (i = 0; i < 10000 && isp_reads_done(t, drive) < want; i++) {
        g_usleep(1000);
    }
    g_assert_cmpint(isp_reads_done(t, drive), ==, want);
}

/* Every byte of the host's buffer still holds @fill. */
static void assert_untouched(Isp *t, uint64_t buf, size_t len, uint8_t fill)
{
    g_autofree uint8_t *data = g_malloc(len);
    size_t i;

    qtest_memread(t->qts, buf, data, len);
    for (i = 0; i < len; i++) {
        g_assert_cmphex(data[i], ==, fill);
    }
}

/*
 * A soft reset cancels every command in flight without a status and
 * resets the loop's devices with the adapter. Once the disks' reads are
 * over, no data has reached the host's buffers: a command that finishes
 * while the reset waits on another's cancellation moves nothing either.
 */
static void test_soft_reset_cancels(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    uint64_t buf = guest_alloc(alloc, 1024);
    int64_t reads5, reads1;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);
    isp_clear_unit_attention(&t, &slow_config);
    qtest_memset(t.qts, buf, 0xa5, 1024);
    reads5 = isp_reads_done(&t, "disk0");
    reads1 = isp_reads_done(&t, "disk2");

    isp_submit_read(&t, 0xe5, 5, 0, buf);
    isp_submit_read(&t, 0xe1, 1, 0, buf + 512);
    isp_soft_reset(&t);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    isp_wait_reads(&t, "disk0", reads5 + 1);
    isp_wait_reads(&t, "disk2", reads1 + 1);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    assert_untouched(&t, buf, 1024, 0xa5);

    isp_loop_up(&t, ICB_OPT_PORT_DB);
    g_assert_cmpint(isp_tur_sense(&t, 5, 0), ==, 0x06);
    g_assert_cmpint(isp_tur_sense(&t, 5, 1), ==, 0x06);
    g_assert_cmpint(isp_tur_sense(&t, 1, 0), ==, 0x06);
    g_assert_cmpint(isp_tur_sense(&t, 1, 0), ==, 0);
    isp_close(&t);
}

/*
 * An HCCR RISC reset cancels every command in flight without a status,
 * as a soft reset does, but leaves the loop's devices alone: none reports
 * a unit attention afterwards. Once the disks' reads are over, no data
 * has reached the host's buffers, no entry has reached the ring and
 * ISR is clear.
 */
static void test_hccr_reset_cancels(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    uint64_t buf = guest_alloc(alloc, 1024);
    uint8_t e[ENTRY_SIZE], zero[ENTRY_SIZE] = { 0 };
    int64_t reads5, reads1;
    uint64_t slot;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);
    isp_clear_unit_attention(&t, &slow_config);
    slot = t.rs + t.rs_next * ENTRY_SIZE;
    qtest_memset(t.qts, buf, 0xa5, 1024);
    reads5 = isp_reads_done(&t, "disk0");
    reads1 = isp_reads_done(&t, "disk2");

    isp_submit_read(&t, 0xe5, 5, 0, buf);
    isp_submit_read(&t, 0xe1, 1, 0, buf + 512);
    wr(&t, REG_HCCR, HCCR_RESET);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    isp_wait_reads(&t, "disk0", reads5 + 1);
    isp_wait_reads(&t, "disk2", reads1 + 1);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    assert_untouched(&t, buf, 1024, 0xa5);
    qtest_memread(t.qts, slot, e, sizeof(e));
    g_assert_cmpint(memcmp(e, zero, sizeof(e)), ==, 0);

    isp_loop_up(&t, ICB_OPT_PORT_DB);
    g_assert_cmpint(isp_tur_sense(&t, 5, 0), ==, 0);
    g_assert_cmpint(isp_tur_sense(&t, 5, 1), ==, 0);
    g_assert_cmpint(isp_tur_sense(&t, 1, 0), ==, 0);
    isp_close(&t);
}

/*
 * Initialise-firmware again with a command in flight cancels it without
 * a status. Once its disk's read is over, no data has reached the host's
 * buffer, neither the old response ring nor the new one has an entry for
 * it, ISR is clear, and the new ring's first entry is the next command's.
 */
static void test_init_firmware_cancels(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    uint64_t buf = guest_alloc(alloc, 512);
    uint8_t e[ENTRY_SIZE], zero[ENTRY_SIZE] = { 0 };
    uint64_t old_slot;
    int64_t reads;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);
    isp_clear_unit_attention(&t, &slow_config);
    old_slot = t.rs + t.rs_next * ENTRY_SIZE;
    qtest_memset(t.qts, buf, 0xa5, 512);
    reads = isp_reads_done(&t, "disk0");

    isp_submit_read(&t, 0xf5, 5, 0, buf);
    isp_rings_at(&t, 16, 16, guest_alloc(alloc, ICB_SIZE),
                 guest_alloc(alloc, 16 * ENTRY_SIZE),
                 guest_alloc(alloc, 16 * ENTRY_SIZE), ICB_OPT_PORT_DB, 0);
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_events(&t, bringup_events, ARRAY_SIZE(bringup_events));
    isp_wait_reads(&t, "disk0", reads + 1);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    g_assert_cmphex(rd(&t, REG_RESPONSE), ==, 0);
    assert_untouched(&t, buf, 512, 0xa5);
    qtest_memread(t.qts, old_slot, e, sizeof(e));
    g_assert_cmpint(memcmp(e, zero, sizeof(e)), ==, 0);

    /* The new ring's first entry is this command's. */
    isp_tur_sense(&t, 1, 0);
    qtest_memread(t.qts, t.rs, e, sizeof(e));
    g_assert_cmphex(e[0], ==, ENTRY_STATUS);
    g_assert_cmphex(ldl_le_p(e + 4), ==, 0x7e00);
    isp_close(&t);
}

/*
 * A data DMA the host bridge refuses completes the command 0x0002 (DMA
 * error) with no SCSI status, whether the adapter is delivering read data
 * or fetching write data, and the port keeps working.
 */
static void test_scsi_dma_error(void *obj, void *data,
                                QGuestAllocator *alloc)
{
    uint8_t cdb[10], e[ENTRY_SIZE];
    ScsiResult r;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);
    isp_clear_unit_attention(&t, &loop_config);

    cdb_rw10(cdb, 0x28, 0, 1);
    isp_scsi(&t, 0xd0a0, 5, 0, cdb, sizeof(cdb), 0x20, UNMAPPED_PHYS, 512,
             0, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_DMA_ERROR);
    g_assert_cmphex(r.scsi_status, ==, 0);

    /* The whole command's data, from an unmapped buffer. */
    cdb_rw10(cdb, 0x2a, 0, 1);
    isp_scsi(&t, 0xd0a1, 5, 0, cdb, sizeof(cdb), 0x40, UNMAPPED_PHYS, 512,
             0, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_DMA_ERROR);
    g_assert_cmphex(r.scsi_status, ==, 0);

    /* A buffer shorter than the command's eight blocks, unmapped too. */
    cdb_rw10(cdb, 0x2a, 0, 8);
    entry_command(e, ENTRY_CMD64, 0xd0a2, 5, 0, 0x2a);
    memcpy(e + 20, cdb, sizeof(cdb));
    stw_le_p(e + 12, 0x40);
    stl_le_p(e + 36, 512);
    stw_le_p(e + 18, 1);
    put_seg64(e + 40, UNMAPPED_PHYS, 512);
    isp_publish(&t, e);
    isp_wait_response(&t, e);
    decode_status(e, 0xd0a2, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_DMA_ERROR);
    g_assert_cmphex(r.scsi_status, ==, 0);

    g_assert_cmpint(isp_tur_sense(&t, 5, 0), ==, 0);
    isp_close(&t);
}

/*
 * With the loop left down by the connection mode, firmware state 0 as
 * for a disconnected adapter, a command to a present port completes
 * "port unavailable" without reaching the disk, and the LIP commands
 * refuse. A later initialisation in loop mode brings the port back.
 */
static void test_loop_down_commands(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    uint64_t buf = guest_alloc(alloc, 512);
    uint8_t cdb[10], seen[512];
    ScsiResult r;
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings_at(&t, 8, 8, guest_alloc(alloc, ICB_SIZE),
                 guest_alloc(alloc, 8 * ENTRY_SIZE),
                 guest_alloc(alloc, 8 * ENTRY_SIZE), ICB_OPT_PORT_DB, 0x10);
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_no_event(&t);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_CONFIG_WAIT);
    g_assert_cmphex(isp_call_ack(&t, CMD_INITIATE_LIP, 0, 0, 0), ==,
                    MBOX_COMMAND_ERROR);
    g_assert_cmphex(isp_call_ack(&t, CMD_FULL_LOGIN_LIP, 0, 0, 0), ==,
                    MBOX_COMMAND_ERROR);
    g_assert_cmphex(isp_call_ack(&t, CMD_LIP_RESET, 0xff00, 0, 0), ==,
                    MBOX_COMMAND_ERROR);

    isp_scsi(&t, 0xdd00, 5, 0, cdb_tur, sizeof(cdb_tur), 0, 0, 0, 0, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_PORT_UNAVAILABLE);
    g_assert_cmphex(r.scsi_status, ==, 0);

    qtest_memset(t.qts, buf, 0xa5, sizeof(seen));
    cdb_rw10(cdb, 0x28, 0, 1);
    isp_scsi(&t, 0xdd01, 5, 0, cdb, sizeof(cdb), 0x20, buf, 512, 0, &r);
    g_assert_cmphex(r.completion, ==, COMPLETION_PORT_UNAVAILABLE);
    g_assert_cmphex(r.scsi_status, ==, 0);
    g_assert_cmphex(r.residual, ==, 0);
    qtest_memread(t.qts, buf, seen, sizeof(seen));
    g_assert_cmphex(seen[0], ==, 0xa5);
    g_assert_cmphex(seen[511], ==, 0xa5);

    isp_rings_at(&t, 8, 8, guest_alloc(alloc, ICB_SIZE),
                 guest_alloc(alloc, 8 * ENTRY_SIZE),
                 guest_alloc(alloc, 8 * ENTRY_SIZE), ICB_OPT_PORT_DB, 0);
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_events(&t, bringup_events, ARRAY_SIZE(bringup_events));
    isp_clear_unit_attention(&t, &loop_config);
    g_assert_cmpint(isp_tur_sense(&t, 5, 0), ==, 0);
    isp_close(&t);
}

/*
 * Approximation: a discovery command whose result DMA the host bridge
 * refuses answers 0x4002, as the host interface's other refused DMAs do.
 */
static void test_loop_dma_refused(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);
    g_assert_cmphex(isp_get_id_list(&t, UNMAPPED_PHYS), ==,
                    MBOX_HOST_IF_ERROR);
    isp_ack(&t);
    g_assert_cmphex(isp_call_dma(&t, CMD_PORT_NAME_LIST, 0x08,
                                 UNMAPPED_PHYS), ==, MBOX_HOST_IF_ERROR);
    isp_ack(&t);
    g_assert_cmphex(isp_call_dma(&t, CMD_GET_PORT_DB, 5 << 8, UNMAPPED_PHYS),
                    ==, MBOX_HOST_IF_ERROR);
    isp_ack(&t);
    g_assert_cmphex(isp_call_dma(&t, CMD_POSITION_MAP, 0, UNMAPPED_PHYS), ==,
                    MBOX_HOST_IF_ERROR);
    isp_ack(&t);
    isp_close(&t);
}

static uint16_t isp_port_name(Isp *t, uint8_t loop_id, uint8_t option)
{
    return isp_call(t, CMD_GET_PORT_NAME, loop_id << 8 | option, 0, 0);
}

/* The name a get port name reply carries, in wire order. */
static void port_name_bytes(Isp *t, uint8_t *name)
{
    stw_le_p(name, rd(t, MBOX(2)));
    stw_le_p(name + 2, rd(t, MBOX(3)));
    stw_le_p(name + 4, rd(t, MBOX(6)));
    stw_le_p(name + 6, rd(t, MBOX(7)));
}

/*
 * Approximation: get port name. Option 0 is the port name, option 1 the
 * node name, as Linux and NetBSD read them. Each mailbox carries two
 * wire-order bytes with the earlier in its low half, the order NetBSD
 * and qlogicfc decode on this part; mainline Linux decodes the other
 * way. The name agrees byte for byte with the port database's. An empty
 * loop ID, the adapter's own, the fabric port 0x7e and anything above
 * it are refused as the port database refuses them, so is every loop ID
 * before the loop is ready, and an option other than 0 or 1 is a
 * parameter error.
 */
static void test_port_name(void *obj, void *data, QGuestAllocator *alloc)
{
    static const uint8_t ports[] = { 1, 5 };
    static const uint8_t refused[] = { 0, 3, 6, 0x7d, 0x7e, 0x7f, 0xff };
    uint8_t name[8], pd[128];
    uint64_t buf;
    unsigned i, opt;
    Isp t;

    isp_open(&t, obj, alloc);
    t.hard_addr = 3;
    isp_start(&t);
    isp_rings_at(&t, 16, 16, guest_alloc(alloc, ICB_SIZE),
                 guest_alloc(alloc, 16 * ENTRY_SIZE),
                 guest_alloc(alloc, 16 * ENTRY_SIZE), ICB_OPT_HARD_ADDR, 0);

    /* Not ready yet: every loop ID, present or not. */
    for (i = 0; i < ARRAY_SIZE(ports); i++) {
        g_assert_cmphex(isp_port_name(&t, ports[i], 0), ==,
                        MBOX_COMMAND_ERROR);
        isp_ack(&t);
    }
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_events(&t, bringup_events, 2);
    g_assert_cmphex(isp_call(&t, CMD_GET_ADAPTER_ID, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 3);
    isp_ack(&t);

    buf = guest_alloc(alloc, sizeof(pd));
    for (i = 0; i < ARRAY_SIZE(ports); i++) {
        g_assert_cmphex(isp_call_dma(&t, CMD_GET_PORT_DB, ports[i] << 8,
                                     buf), ==, MBOX_COMPLETE);
        isp_ack(&t);
        qtest_memread(t.qts, buf, pd, sizeof(pd));

        for (opt = 0; opt < 2; opt++) {
            uint64_t want = opt ? WWN_NODE(ports[i]) : WWN_PORT(ports[i]);

            g_assert_cmphex(isp_port_name(&t, ports[i], opt), ==,
                            MBOX_COMPLETE);
            /* Byte 0 is the name's most significant byte. */
            g_assert_cmphex(rd(&t, MBOX(2)), ==,
                            bswap16((want >> 48) & 0xffff));
            g_assert_cmphex(rd(&t, MBOX(3)), ==,
                            bswap16((want >> 32) & 0xffff));
            g_assert_cmphex(rd(&t, MBOX(6)), ==,
                            bswap16((want >> 16) & 0xffff));
            g_assert_cmphex(rd(&t, MBOX(7)), ==, bswap16(want & 0xffff));
            port_name_bytes(&t, name);
            isp_ack(&t);
            g_assert_cmpmem(name, sizeof(name), pd + (opt ? 12 : 20), 8);
        }
    }

    for (i = 0; i < ARRAY_SIZE(refused); i++) {
        for (opt = 0; opt < 2; opt++) {
            g_assert_cmphex(isp_port_name(&t, refused[i], opt), ==,
                            MBOX_COMMAND_ERROR);
            isp_ack(&t);
        }
    }
    g_assert_cmphex(isp_port_name(&t, 5, 2), ==, MBOX_PARAM_ERROR);
    isp_ack(&t);
    g_assert_cmphex(isp_port_name(&t, 1, 0xff), ==, MBOX_PARAM_ERROR);
    isp_ack(&t);
    isp_close(&t);
}

/*
 * Approximation: IRIX 6.5.30 rang get firmware state on the model while
 * the bring-up's loop-up event was posted and unread. The event stays
 * readable until its release, and the completion then follows with the
 * loop ready, so neither is lost.
 */
static void test_writes_behind_loop_up(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    isp_rings(&t, 8, 8);
    qtest_clock_step(t.qts, BRINGUP_NS);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, ASYNC_LIP);
    isp_ack(&t);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, ASYNC_LOOP_UP);

    wr(&t, MBOX(0), CMD_GET_FW_STATE);
    wr(&t, REG_HCCR, HCCR_SET_HOST);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, ASYNC_LOOP_UP);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, 0);
    isp_ack(&t);

    g_assert_cmphex(rd(&t, REG_SEMAPHORE), ==, SEM_LOCK);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    g_assert_cmphex(rd(&t, MBOX(1)), ==, FW_STATE_READY);
    isp_ack(&t);
    isp_expect_no_event(&t);
    isp_close(&t);
}

/*
 * OpenBSD writes the port name list's inputs without mailbox 7, so the
 * address takes bits 47:32 from what the host last wrote there. Here
 * that is 1, from an earlier command: the list lands above 4 GiB, and
 * nothing reaches the same low 32 bits, which then hold no memory.
 */
static void test_port_name_list_retains_m7(void *obj, void *data,
                                           QGuestAllocator *alloc)
{
    uint64_t bridge_device = 0x40000000ULL + 4 + 0x200 + 8 * 4;
    uint8_t names[32];
    uint64_t low;
    unsigned i;
    Isp t;

    if (!is_mips64()) {
        g_test_skip("high memory belongs to the SGI Bridge test head");
        return;
    }
    isp_open(&t, obj, alloc);
    isp_loop_up(&t, ICB_OPT_PORT_DB);
    low = guest_alloc(alloc, sizeof(names));
    qtest_sysbus_mmio_map(t.qts, "/machine/peripheral/transfer-ram", 0,
                          0x102000000ULL);
    qtest_writel(t.qts, bridge_device,
                 qtest_readl(t.qts, bridge_device) | (1u << 19));
    qtest_memset(t.qts, low + (1ULL << 32), 0xa5, sizeof(names));

    /* A No-op that writes mailbox 7 as 1. */
    for (i = 0; i < 8; i++) {
        wr(&t, MBOX(i), i == 7 ? 1 : 0);
    }
    wr(&t, REG_HCCR, HCCR_SET_HOST);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, MBOX_COMPLETE);
    isp_ack(&t);

    {
        const uint16_t in[] = {
            CMD_PORT_NAME_LIST, 0x08, (low >> 16) & 0xffff, low & 0xffff,
            0, 0, 0,
        };

        g_assert_cmphex(isp_mbox(&t, in, ARRAY_SIZE(in)), ==, MBOX_COMPLETE);
        g_assert_cmphex(rd(&t, MBOX(1)), ==, 20);
        isp_ack(&t);
    }
    qtest_memread(t.qts, low + (1ULL << 32), names, sizeof(names));
    g_assert_cmphex(ldq_le_p(names), ==, WWN_PORT(1));
    g_assert_cmphex(ldq_le_p(names + 10), ==, WWN_PORT(5));
    g_assert_cmphex(names[20], ==, 0xa5);
    isp_close(&t);
}

/* The model's constant m3 in a get retry count reply. */
#define RETRY_TIMEOUT       200

static void assert_retry_count(Isp *t, uint16_t count, uint16_t delay)
{
    g_assert_cmphex(isp_call(t, CMD_GET_RETRY_COUNT, 0, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(rd(t, MBOX(1)), ==, count);
    g_assert_cmphex(rd(t, MBOX(2)), ==, delay);
    g_assert_cmphex(rd(t, MBOX(3)), ==, RETRY_TIMEOUT);
    isp_ack(t);
}

/*
 * Approximation: get retry count returns the control block's retry
 * count in m1 and retry delay in m2, the reading Linux and NetBSD give
 * the command, and a constant m3 no public source settles. It answers
 * from the moment initialise-firmware completes, loop up or not, and a
 * later initialisation replaces the pair. Before any control block it
 * is refused with 0x4005, and before execute with 0x4001.
 */
static void test_retry_count(void *obj, void *data, QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    g_assert_cmphex(isp_call_ack(&t, CMD_GET_RETRY_COUNT, 0, 0, 0), ==,
                    MBOX_INVALID);
    isp_start(&t);
    g_assert_cmphex(isp_call_ack(&t, CMD_GET_RETRY_COUNT, 0, 0, 0), ==,
                    MBOX_COMMAND_ERROR);

    t.retry_count = 6;
    t.retry_delay = 9;
    isp_rings(&t, 8, 8);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_WAIT_AL_PA);
    assert_retry_count(&t, 6, 9);
    qtest_clock_step(t.qts, BRINGUP_NS);
    isp_expect_events(&t, bringup_events, 2);
    g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_READY);
    assert_retry_count(&t, 6, 9);

    t.retry_count = 2;
    t.retry_delay = 3;
    isp_rings(&t, 8, 8);
    assert_retry_count(&t, 2, 3);

    /* Each byte is zero-extended, not sign-extended. */
    t.retry_count = 0xfe;
    t.retry_delay = 0x81;
    isp_rings(&t, 8, 8);
    assert_retry_count(&t, 0xfe, 0x81);

    /* A reset drops the control block with the firmware. */
    wr(&t, REG_HCCR, HCCR_RESET);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    g_assert_cmphex(isp_call_ack(&t, CMD_GET_RETRY_COUNT, 0, 0, 0), ==,
                    MBOX_INVALID);
    isp_start(&t);
    g_assert_cmphex(isp_call_ack(&t, CMD_GET_RETRY_COUNT, 0, 0, 0), ==,
                    MBOX_COMMAND_ERROR);
    isp_close(&t);
}

/*
 * Get port queue parameters, set retry count, reset link status and get
 * link status, which IRIX 6.5.30 sends on the model and no public source
 * settles the replies of. At ready each is refused with 0x4001, writes
 * nothing to the buffer its mailboxes could name and changes neither the
 * loop nor the retry pair: set retry count is not stored.
 */
static void test_refused_loop_commands(void *obj, void *data,
                                       QGuestAllocator *alloc)
{
    static const struct {
        uint16_t cmd;
        uint16_t m1;
    } cases[] = {
        { CMD_GET_PORT_QUEUE, 1 << 8 },
        { CMD_SET_RETRY_COUNT, 7 },
        { CMD_RESET_LINK_STAT, 0 },
        { CMD_GET_LINK_STATUS, 0 },
        { CMD_GET_LINK_STATUS, 1 << 8 },
        { CMD_GET_LINK_STATUS, 5 << 8 },
    };
    uint8_t seen[256], fill[256];
    uint64_t buf;
    unsigned i;
    Isp t;

    isp_open(&t, obj, alloc);
    t.retry_count = 4;
    t.retry_delay = 15;
    isp_loop_up(&t, 0);
    buf = guest_alloc(alloc, sizeof(seen));
    memset(fill, 0xa5, sizeof(fill));

    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        qtest_memwrite(t.qts, buf, fill, sizeof(fill));
        g_assert_cmphex(isp_call_dma(&t, cases[i].cmd, cases[i].m1, buf), ==,
                        MBOX_INVALID);
        isp_ack(&t);
        qtest_memread(t.qts, buf, seen, sizeof(seen));
        g_assert_cmpmem(seen, sizeof(seen), fill, sizeof(fill));
        isp_expect_no_event(&t);
        g_assert_cmphex(isp_fw_state(&t), ==, FW_STATE_READY);
        assert_retry_count(&t, 4, 15);
    }

    /* Set retry count with a new pair in m1 and m2 keeps the old one. */
    g_assert_cmphex(isp_call_ack(&t, CMD_SET_RETRY_COUNT, 7, 8, 0), ==,
                    MBOX_INVALID);
    assert_retry_count(&t, 4, 15);
    isp_close(&t);
}

static void isp2200_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "addr=04.0,id=isp0",
    };
    QOSGraphTestOptions loop_opts = {
        .before = loop_test_setup,
        .arg = (void *)&loop_config,
    };
    QOSGraphTestOptions plug_opts = {
        .before = loop_test_setup,
        .arg = (void *)&plug_config,
    };
    QOSGraphTestOptions slow_opts = {
        .before = loop_test_setup,
        .arg = (void *)&slow_config,
    };
    QOSGraphTestOptions low_opts = {
        .before = loop_test_setup,
        .arg = (void *)&low_config,
    };

    add_qpci_address(&opts, &(QPCIAddress) { .devfn = QPCI_DEVFN(4, 0) });

    qos_node_create_driver("qlogic-isp2200", isp2200_create);
    qos_node_consumes("qlogic-isp2200", "pci-bus", &opts);
    qos_node_consumes("qlogic-isp2200", "sgi-pci-bus", &opts);
    qos_node_produces("qlogic-isp2200", "pci-device");

    qos_add_test("identity", "qlogic-isp2200", test_identity, NULL);
    qos_add_test("window", "qlogic-isp2200", test_window, NULL);
    qos_add_test("undescribed-registers", "qlogic-isp2200",
                 test_undescribed_registers, NULL);
    qos_add_test("reset-states", "qlogic-isp2200", test_reset_states, NULL);
    qos_add_test("soft-reset-orders", "qlogic-isp2200",
                 test_soft_reset_orders, NULL);
    qos_add_test("soft-reset-clears", "qlogic-isp2200",
                 test_soft_reset_clears, NULL);
    qos_add_test("risc-control", "qlogic-isp2200", test_risc_control, NULL);
    qos_add_test("load-ram-32", "qlogic-isp2200", test_load_ram_32, NULL);
    qos_add_test("dump-ram", "qlogic-isp2200", test_dump_ram, NULL);
    qos_add_test("host-interrupt", "qlogic-isp2200", test_host_interrupt,
                 NULL);
    qos_add_test("unmodelled-host-commands", "qlogic-isp2200",
                 test_unmodelled_host_commands, NULL);
    qos_add_test("register-test", "qlogic-isp2200", test_register_test, NULL);
    qos_add_test("mailbox-storage", "qlogic-isp2200", test_mailbox_storage,
                 NULL);
    qos_add_test("completion-interrupt", "qlogic-isp2200",
                 test_completion_interrupt, NULL);
    qos_add_test("second-result-waits", "qlogic-isp2200",
                 test_second_result_waits, NULL);
    qos_add_test("doorbell-waits-for-slot", "qlogic-isp2200",
                 test_doorbell_waits_for_slot, NULL);
    qos_add_test("semaphore-write-one", "qlogic-isp2200",
                 test_semaphore_write_one, NULL);
    qos_add_test("unknown-commands", "qlogic-isp2200", test_unknown_commands,
                 NULL);
    qos_add_test("rom-subset", "qlogic-isp2200", test_rom_subset, NULL);
    qos_add_test("load-ram", "qlogic-isp2200", test_load_ram, NULL);
    qos_add_test("load-ram-high", "qlogic-isp2200", test_load_ram_high, NULL);
    qos_add_test("write-ram-word", "qlogic-isp2200", test_write_ram_word,
                 NULL);
    qos_add_test("verify-checksum", "qlogic-isp2200", test_verify_checksum,
                 NULL);
    qos_add_test("firmware-identity", "qlogic-isp2200",
                 test_firmware_identity, NULL);
    qos_add_test("firmware-options", "qlogic-isp2200",
                 test_firmware_options, NULL);
    qos_add_test("init-firmware-rejects", "qlogic-isp2200",
                 test_init_firmware_rejects, NULL);
    qos_add_test("init-firmware-high", "qlogic-isp2200",
                 test_init_firmware_high, NULL);
    qos_add_test("rio-mode-ignored", "qlogic-isp2200", test_rio_mode_ignored,
                 NULL);
    qos_add_test("command-round-trip", "qlogic-isp2200",
                 test_command_round_trip, NULL);
    qos_add_test("continuations", "qlogic-isp2200", test_continuations, NULL);
    qos_add_test("segment-list", "qlogic-isp2200", test_segment_list, NULL);
    qos_add_test("marker", "qlogic-isp2200", test_marker, NULL);
    qos_add_test("request-wrap", "qlogic-isp2200", test_request_wrap, NULL);
    qos_add_test("response-ring-full", "qlogic-isp2200",
                 test_response_ring_full, NULL);
    qos_add_test("unknown-entry-type", "qlogic-isp2200",
                 test_unknown_entry_type, NULL);
    qos_add_test("malformed-entries", "qlogic-isp2200",
                 test_malformed_entries, NULL);
    qos_add_test("descriptor-dma-errors", "qlogic-isp2200",
                 test_descriptor_dma_errors, NULL);
    qos_add_test("ring-dma-errors", "qlogic-isp2200", test_ring_dma_errors,
                 NULL);
    qos_add_test("writes-behind-posted-result", "qlogic-isp2200",
                 test_writes_behind_posted_result, NULL);
    qos_add_test("request-dma-error-waits", "qlogic-isp2200",
                 test_request_dma_error_waits, NULL);
    qos_add_test("bus-master-disabled", "qlogic-isp2200",
                 test_bus_master_disabled, NULL);
    qos_add_test("nvram-read-fails", "qlogic-isp2200", test_nvram_read_fails,
                 NULL);
    qos_add_test("window-linux-order", "qlogic-isp2200",
                 test_window_linux_order, NULL);
    qos_add_test("window-openbsd-init", "qlogic-isp2200",
                 test_window_openbsd_init, NULL);
    qos_add_test("window-openbsd-reset", "qlogic-isp2200",
                 test_window_openbsd_reset, NULL);
    qos_add_test("window-boundaries", "qlogic-isp2200",
                 test_window_boundaries, NULL);
    qos_add_test("window-response-out", "qlogic-isp2200",
                 test_window_response_out, NULL);
    qos_add_test("posting-waits-for-latch", "qlogic-isp2200",
                 test_posting_waits_for_latch, NULL);
    qos_add_test("posting-waits-for-semaphore", "qlogic-isp2200",
                 test_posting_waits_for_semaphore, NULL);
    qos_add_test("expanded-lun", "qlogic-isp2200", test_expanded_lun, NULL);
    qos_add_test("window-openbsd-interleave", "qlogic-isp2200",
                 test_window_openbsd_interleave, NULL);
    qos_add_test("window-openbsd-interleave-response", "qlogic-isp2200",
                 test_window_openbsd_interleave_response, NULL);
    qos_add_test("loop-identity", "qlogic-isp2200", test_loop_identity,
                 &loop_opts);
    qos_add_test("loop-hard-address", "qlogic-isp2200",
                 test_loop_hard_address, &low_opts);
    qos_add_test("loop-state-timeline", "qlogic-isp2200",
                 test_loop_state_timeline, NULL);
    qos_add_test("loop-connection-mode", "qlogic-isp2200",
                 test_loop_connection_mode, NULL);
    qos_add_test("loop-events", "qlogic-isp2200", test_loop_events, NULL);
    qos_add_test("loop-plug", "qlogic-isp2200", test_loop_plug, &plug_opts);
    qos_add_test("scsi-inquiry", "qlogic-isp2200", test_scsi_inquiry,
                 &loop_opts);
    qos_add_test("scsi-read-write", "qlogic-isp2200", test_scsi_read_write,
                 &loop_opts);
    qos_add_test("scsi-addressing", "qlogic-isp2200", test_scsi_addressing,
                 &loop_opts);
    qos_add_test("scsi-check-condition", "qlogic-isp2200",
                 test_scsi_check_condition, &loop_opts);
    qos_add_test("scsi-unavailable", "qlogic-isp2200", test_scsi_unavailable,
                 &loop_opts);
    qos_add_test("abort-command", "qlogic-isp2200", test_abort_command,
                 &slow_opts);
    qos_add_test("target-and-lun-reset", "qlogic-isp2200",
                 test_target_and_lun_reset, &slow_opts);
    qos_add_test("lip-reset", "qlogic-isp2200", test_lip_reset, &slow_opts);
    qos_add_test("unplug-disconnect", "qlogic-isp2200",
                 test_unplug_disconnect, &slow_opts);
    qos_add_test("soft-reset-cancels", "qlogic-isp2200",
                 test_soft_reset_cancels, &slow_opts);
    qos_add_test("hccr-reset-cancels", "qlogic-isp2200",
                 test_hccr_reset_cancels, &slow_opts);
    qos_add_test("init-firmware-cancels", "qlogic-isp2200",
                 test_init_firmware_cancels, &slow_opts);
    qos_add_test("scsi-dma-error", "qlogic-isp2200", test_scsi_dma_error,
                 &loop_opts);
    qos_add_test("loop-down-commands", "qlogic-isp2200",
                 test_loop_down_commands, &loop_opts);
    qos_add_test("loop-dma-refused", "qlogic-isp2200", test_loop_dma_refused,
                 &loop_opts);
    qos_add_test("port-name", "qlogic-isp2200", test_port_name, &loop_opts);
    qos_add_test("writes-behind-loop-up", "qlogic-isp2200",
                 test_writes_behind_loop_up, &loop_opts);
    qos_add_test("port-name-list-retains-m7", "qlogic-isp2200",
                 test_port_name_list_retains_m7, &loop_opts);
    qos_add_test("retry-count", "qlogic-isp2200", test_retry_count, NULL);
    qos_add_test("refused-loop-commands", "qlogic-isp2200",
                 test_refused_loop_commands, &loop_opts);
}

libqos_init(isp2200_register_nodes);
