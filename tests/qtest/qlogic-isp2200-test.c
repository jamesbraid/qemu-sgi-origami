/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * QTest for the QLogic ISP2200 Fibre Channel host adapter's host
 * interface.
 *
 * The adapter stands alone on a PCI bus with no Fibre Channel service
 * attached, so it reports a disconnected loop: every command completes
 * "port unavailable", and what reached the service is read back through
 * the device's x-fc-* QOM properties. The register contract comes from
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

#define MBOX_COMPLETE       0x4000
#define MBOX_INVALID        0x4001
#define MBOX_HOST_IF_ERROR  0x4002
#define MBOX_TEST_FAILED    0x4003
#define MBOX_PARAM_ERROR    0x4006

#define ASYNC_REQUEST_DMA   0x8003
#define ASYNC_RESPONSE_DMA  0x8004

#define CODE_ORG            0x1000

#define ICB_SIZE            96
#define ICB_OPT_FAST_POST   0x0008

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

#define COMPLETION_PORT_UNAVAILABLE 0x0028

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
 * Approximation: the reset states. A device reset leaves the ROM code
 * running and its image readable; a soft reset holds the RISC until an
 * explicit release. The device reset checked is the system reset the
 * qos runner issues between cases, after earlier cases left the adapter
 * running firmware with live rings; a reset issued inside a case would
 * also clear the Bridge test head's I/O window, which only the runner
 * rebuilds.
 */
static void test_reset_states(void *obj, void *data, QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    assert_boot_image(&t);

    /* A release sent to a running RISC changes nothing. */
    wr(&t, REG_HCCR, HCCR_RELEASE);
    assert_boot_image(&t);

    isp_soft_reset(&t);
    assert_held_in_reset(&t);
    wr(&t, REG_HCCR, HCCR_RELEASE);
    assert_boot_image(&t);
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

/* The doorbell bit stands until the host clears it. */
static void test_host_interrupt(void *obj, void *data, QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    isp_call(&t, CMD_NOP, 0, 0, 0);
    g_assert_cmphex(rd(&t, REG_HCCR) & HCCR_HOST_INT, ==, HCCR_HOST_INT);
    isp_ack(&t);
    g_assert_cmphex(rd(&t, REG_HCCR) & HCCR_HOST_INT, ==, HCCR_HOST_INT);
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
 * the latch are released, then appears intact.
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
                    CMD_REGISTER_TEST);

    wr(&t, REG_HCCR, HCCR_CLEAR_RISC);
    g_assert_cmphex(rd(&t, REG_ISR), ==, 0);
    g_assert_cmphex(rd(&t, MBOX(0)), ==, CMD_REGISTER_TEST);
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
        g_assert_cmphex(rd(&t, REG_HCCR) & HCCR_HOST_INT, ==, HCCR_HOST_INT);
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
 * Approximation: unknown and deferred commands answer 0x4001 and
 * leave the running firmware alone; with no Fibre Channel service, a
 * loop command does too.
 */
static void test_unknown_commands(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    Isp t;

    isp_open(&t, obj, alloc);
    isp_start(&t);
    g_assert_cmphex(isp_call_ack(&t, 0x00ee, 0, 0, 0), ==, MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, CMD_ABORT_IOCB, 0, 0, 0), ==,
                    MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, CMD_RESOURCE_COUNTS, 0, 0, 0), ==,
                    MBOX_INVALID);
    g_assert_cmphex(isp_call_ack(&t, CMD_GET_ADAPTER_ID, 0, 0, 0), ==,
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

static void isp2200_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "addr=04.0,id=isp0",
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
    qos_add_test("soft-reset-clears", "qlogic-isp2200",
                 test_soft_reset_clears, NULL);
    qos_add_test("risc-control", "qlogic-isp2200", test_risc_control, NULL);
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
}

libqos_init(isp2200_register_nodes);
