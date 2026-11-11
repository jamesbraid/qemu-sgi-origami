/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * QTest for the QLogic ISP1040's migration state, on the SGI Bridge test
 * head.
 *
 * The adapter remembers whether it posted a status entry after the guest
 * last read the response pointer, and a clear of the RISC interrupt leaves
 * the request standing while it has. Each case migrates with a handler
 * part way through its ring, between that read and the clear, and checks
 * that the destination's clear does what the source's would have: keep
 * the request when an entry landed after the read, drop it when none did.
 * Migration is between identical builds only.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/libqos-malloc.h"
#include "libqos/pci.h"
#include "libqos/pci-sgi-bridge.h"
#include "qemu/bswap.h"
#include "hw/pci/pci_regs.h"
#include "sgi-migrate.h"

#define ISP_SLOT            4

#define MBOX(n)             (0x70 + (n) * 2)
#define REG_REQUEST         MBOX(4)
#define REG_RESPONSE        MBOX(5)
#define REG_BUS_STATUS      0x0a
#define REG_SEMAPHORE       0x0c
#define REG_HCCR            0xc0

#define BUS_STATUS_RISC     0x0006
#define HCCR_SET_HOST       0x5000
#define HCCR_CLEAR_RISC     0x7000

#define CMD_INIT_REQ_QUEUE  0x0010
#define CMD_INIT_RSP_QUEUE  0x0011
#define MBOX_COMPLETE       0x4000

#define ENTRY_SIZE          64
#define ENTRY_REQUEST       0x01
#define RING_LEN            8

#define ISP_DRIVE \
    "-drive id=disk0,if=none,format=raw,file=null-co://," \
    "file.read-zeroes=on "

#define ISP_DEVICES \
    "-display none " \
    "-device qlogic-isp1040,bus=pci,addr=04.0,id=isp0 " \
    "-device scsi-hd,bus=isp0.0,drive=disk0,scsi-id=0,lun=0 "

/* One adapter on one machine, and where its rings live. */
typedef struct Isp {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar io;
    uint64_t rq;
    uint64_t rs;
    uint16_t rq_next;
} Isp;

static QGuestAllocator alloc;

static uint16_t rd(Isp *t, uint8_t reg)
{
    return qpci_io_readw(t->dev, t->io, reg);
}

static void wr(Isp *t, uint8_t reg, uint16_t val)
{
    qpci_io_writew(t->dev, t->io, reg, val);
}

static void isp_detach(Isp *t)
{
    g_free(t->dev);
    qpci_free_sgi_bridge(t->bus);
    qtest_quit(t->qts);
}

static Isp isp_source(void)
{
    Isp t = { 0 };

    qpci_sgi_bridge_alloc_init(&alloc, 0);
    t.qts = qtest_initf("-M none " QPCI_SGI_BRIDGE_HEAD ISP_DRIVE
                        ISP_DEVICES);
    t.bus = qpci_init_sgi_bridge(t.qts, &alloc);
    t.dev = qpci_device_find(t.bus, QPCI_DEVFN(ISP_SLOT, 0));
    g_assert_nonnull(t.dev);
    t.io = qpci_iomap(t.dev, 0, NULL);
    qpci_device_enable(t.dev);
    return t;
}

/* Plain queue initialisation: count in mb1, address in mb2 and mb3. */
static void isp_init_queue(Isp *t, uint16_t cmd, uint64_t addr)
{
    wr(t, MBOX(1), RING_LEN);
    wr(t, MBOX(2), addr >> 16);
    wr(t, MBOX(3), addr);
    wr(t, MBOX(0), cmd);
    wr(t, REG_HCCR, HCCR_SET_HOST);
    g_assert_cmphex(rd(t, MBOX(0)), ==, MBOX_COMPLETE);
    wr(t, REG_HCCR, HCCR_CLEAR_RISC);
    wr(t, REG_SEMAPHORE, 0);
}

static void isp_init(Isp *t)
{
    t->rq = guest_alloc(&alloc, RING_LEN * ENTRY_SIZE);
    t->rs = guest_alloc(&alloc, RING_LEN * ENTRY_SIZE);
    isp_init_queue(t, CMD_INIT_REQ_QUEUE, t->rq);
    isp_init_queue(t, CMD_INIT_RSP_QUEUE, t->rs);
}

/* A TEST UNIT READY to target 0; scsi-hd answers it at once. */
static void isp_publish_tur(Isp *t, uint32_t handle)
{
    uint8_t e[ENTRY_SIZE];

    memset(e, 0, sizeof(e));
    e[0] = ENTRY_REQUEST;
    e[1] = 1;
    stl_le_p(e + 4, handle);
    stw_le_p(e + 10, 6);
    qtest_memwrite(t->qts, t->rq + t->rq_next * ENTRY_SIZE, e, sizeof(e));
    t->rq_next = (t->rq_next + 1) % RING_LEN;
    wr(t, REG_REQUEST, t->rq_next);
}

/*
 * Start a destination that waits for the stream, migrate the stopped
 * source into it and quit the source.
 */
static void isp_migrate(Isp *from, Isp *to, const char *dir)
{
    g_autofree char *uri = g_strdup_printf("unix:%s/migrate.sock", dir);
    QTestState *qts;

    qts = qtest_initf("-M none " QPCI_SGI_BRIDGE_HEAD ISP_DRIVE ISP_DEVICES
                      "-incoming %s", uri);
    qtest_qmp_assert_success(from->qts, "{ 'execute': 'stop' }");
    sgi_migrate_stopped(from->qts, uri);
    sgi_wait_migration(qts, "completed");
    memset(to, 0, sizeof(*to));
    to->qts = qts;
    to->bus = qpci_init_sgi_bridge(qts, &alloc);
    sgi_migrate_resume(qts);
    isp_detach(from);

    to->dev = qpci_device_find(to->bus, QPCI_DEVFN(ISP_SLOT, 0));
    g_assert_nonnull(to->dev);
    /* Already enabled and placed: the migrated config space says where. */
    to->io = qpci_legacy_iomap(to->dev,
                               qpci_config_readl(to->dev, PCI_BASE_ADDRESS_0)
                               & PCI_BASE_ADDRESS_IO_MASK);
    to->rq = from->rq;
    to->rs = from->rs;
    to->rq_next = from->rq_next;
}

static void isp_finish(Isp *t)
{
    isp_detach(t);
    alloc_destroy(&alloc);
}

/*
 * A second completion lands after the handler read the pointer, and the
 * machine migrates before the handler clears. The destination's clear
 * keeps the request for the entry the handler has not seen.
 */
static void test_unseen_entry_crosses(void)
{
    g_autofree char *dir = g_dir_make_tmp("isp-migrate-XXXXXX", NULL);
    Isp from = isp_source(), to;

    isp_init(&from);
    isp_publish_tur(&from, 0x5001);
    g_assert_cmpuint(rd(&from, REG_RESPONSE), ==, 1);
    isp_publish_tur(&from, 0x5002);
    wr(&from, REG_RESPONSE, 1);

    isp_migrate(&from, &to, dir);
    wr(&to, REG_HCCR, HCCR_CLEAR_RISC);
    g_assert_cmphex(rd(&to, REG_BUS_STATUS), ==, BUS_STATUS_RISC);

    g_assert_cmpuint(rd(&to, REG_RESPONSE), ==, 2);
    wr(&to, REG_RESPONSE, 2);
    wr(&to, REG_HCCR, HCCR_CLEAR_RISC);
    g_assert_cmphex(rd(&to, REG_BUS_STATUS), ==, 0);
    isp_finish(&to);
    g_rmdir(dir);
}

/*
 * Nothing lands after the read, so the stream leaves the flag out and the
 * destination's clear drops the request.
 */
static void test_seen_entries_clear(void)
{
    g_autofree char *dir = g_dir_make_tmp("isp-migrate-XXXXXX", NULL);
    Isp from = isp_source(), to;

    isp_init(&from);
    isp_publish_tur(&from, 0x6001);
    g_assert_cmpuint(rd(&from, REG_RESPONSE), ==, 1);
    wr(&from, REG_RESPONSE, 1);

    isp_migrate(&from, &to, dir);
    g_assert_cmphex(rd(&to, REG_BUS_STATUS), ==, BUS_STATUS_RISC);
    wr(&to, REG_HCCR, HCCR_CLEAR_RISC);
    g_assert_cmphex(rd(&to, REG_BUS_STATUS), ==, 0);
    isp_finish(&to);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("qlogic-isp1040/migration/unseen-entry-crosses",
                   test_unseen_entry_crosses);
    qtest_add_func("qlogic-isp1040/migration/seen-entries-clear",
                   test_seen_entries_clear);
    return g_test_run();
}
