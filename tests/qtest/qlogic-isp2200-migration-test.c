/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * QTest for the QLogic ISP2200's migration state, on the SGI Bridge test
 * head.
 *
 * Each case builds one of the windows in which the adapter holds work it
 * has not yet handed to the host, migrates, and checks that the
 * destination hands over the same events and status entries as the
 * register-contract cases in qlogic-isp2200-test.c see without a
 * migration: a loop bring-up still running; an event posted with more
 * queued, a completion waiting behind it and a doorbell latched behind
 * that; and a command in flight on a loop port, held there by a read
 * error that stopped the source. Migration is between identical builds
 * only.
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

#define MBOX(n)             ((n) < 8 ? 0x10 + (n) * 2 : 0xe0 + ((n) - 8) * 2)
#define REG_REQUEST         MBOX(4)
#define REG_RESPONSE        MBOX(5)
#define REG_ISR             0x0a
#define REG_SEMAPHORE       0x0c
#define REG_HCCR            0xc0

#define ISR_RISC            0x0008
#define SEM_LOCK            0x0001
#define HCCR_SET_HOST       0x5000
#define HCCR_CLEAR_RISC     0x7000
#define HCCR_HOST_INT       0x0080

#define CMD_NOP             0x0000
#define CMD_EXEC_FIRMWARE   0x0002
#define CMD_WRITE_RAM_WORD  0x0004
#define CMD_REGISTER_TEST   0x0006
#define CMD_VERIFY_CHECKSUM 0x0007
#define CMD_GET_ID_LIST     0x007c
#define CMD_INIT_FIRMWARE   0x0060
#define CMD_GET_FW_STATE    0x0069

#define MBOX_COMPLETE       0x4000

#define ASYNC_LIP           0x8010
#define ASYNC_LOOP_UP       0x8011
#define ASYNC_PORT_DB       0x8014

#define CODE_ORG            0x1000
#define ICB_SIZE            96
#define ICB_OPT_PORT_DB     0x0100
#define ENTRY_SIZE          64
#define ENTRY_STATUS        0x03
#define ENTRY_CMD64         0x19
#define RING_LEN            8

#define COMPLETION_COMPLETE 0x0000

/* The model's bring-up time; see the register-contract cases. */
#define BRINGUP_NS          (100 * 1000 * 1000)

#define ISP_DRIVE \
    "-drive id=disk0,if=none,format=raw,file=null-co://," \
    "file.read-zeroes=on "

/*
 * The same disk, but its first read of block 8 fails and the error stops
 * the machine. Block 0 would not do: scsi-hd reads it at realize to guess
 * a geometry, and ignores the error. The SCSI layer keeps the command to
 * retry once the machine runs again, so it stays in flight for as long as
 * the test needs and no block I/O is pending when the state is saved.
 */
#define ISP_DRIVE_READ_STOPS \
    "-drive id=disk0,if=none,format=raw,rerror=stop," \
    "file.driver=blkdebug,file.image.driver=null-co," \
    "file.image.read-zeroes=on,file.inject-error.0.event=read_aio," \
    "file.inject-error.0.sector=8,file.inject-error.0.errno=5," \
    "file.inject-error.0.once=on "

#define ISP_DEVICES \
    "-display none " \
    "-device qlogic-isp2200,bus=pci,addr=04.0,id=isp0 " \
    "-device scsi-hd,bus=isp0.0,drive=disk0,scsi-id=5,lun=0," \
    "port_wwn=0x21000011aabbcc05,wwn=0x20000011aabbcc05 "

/* One adapter on one machine, and where its rings live. */
typedef struct Isp {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar io;
    uint64_t rq;
    uint64_t rs;
    uint16_t rq_next;
    uint16_t rs_next;
} Isp;

static QGuestAllocator alloc;

static void isp_attach(Isp *t, QTestState *qts)
{
    t->qts = qts;
    t->bus = qpci_init_sgi_bridge(qts, &alloc);
    t->dev = qpci_device_find(t->bus, QPCI_DEVFN(ISP_SLOT, 0));
    g_assert_nonnull(t->dev);
    t->io = qpci_iomap(t->dev, 0, NULL);
    qpci_device_enable(t->dev);
}

static void isp_detach(Isp *t)
{
    g_free(t->dev);
    qpci_free_sgi_bridge(t->bus);
    qtest_quit(t->qts);
}

static uint16_t rd(Isp *t, uint8_t reg)
{
    return qpci_io_readw(t->dev, t->io, reg);
}

static void wr(Isp *t, uint8_t reg, uint16_t val)
{
    qpci_io_writew(t->dev, t->io, reg, val);
}

static void isp_ring(Isp *t, const uint16_t *in, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        wr(t, MBOX(i), in[i]);
    }
    wr(t, REG_HCCR, HCCR_SET_HOST);
}

static void isp_ack(Isp *t)
{
    wr(t, REG_HCCR, HCCR_CLEAR_RISC);
    wr(t, REG_SEMAPHORE, 0);
}

/* A command whose result posts at once; its m0, then released. */
static uint16_t isp_call(Isp *t, uint16_t cmd, uint16_t m1, uint16_t m2,
                         uint16_t m3)
{
    const uint16_t in[] = { cmd, m1, m2, m3 };
    uint16_t status;

    isp_ring(t, in, ARRAY_SIZE(in));
    g_assert_cmphex(rd(t, REG_SEMAPHORE) & SEM_LOCK, ==, SEM_LOCK);
    status = rd(t, MBOX(0));
    isp_ack(t);
    return status;
}

/* The posted item's m0 and m1, then released. */
static uint16_t isp_take(Isp *t, uint16_t *m1)
{
    uint16_t m0;

    g_assert_cmphex(rd(t, REG_SEMAPHORE) & SEM_LOCK, ==, SEM_LOCK);
    g_assert_cmphex(rd(t, REG_ISR) & ISR_RISC, ==, ISR_RISC);
    m0 = rd(t, MBOX(0));
    *m1 = rd(t, MBOX(1));
    isp_ack(t);
    return m0;
}

static void isp_expect_nothing(Isp *t)
{
    g_assert_cmphex(rd(t, REG_SEMAPHORE), ==, 0);
    g_assert_cmphex(rd(t, REG_ISR), ==, 0);
}

/*
 * Download a stand-in image, its length in word 3 and summing to zero,
 * then verify and execute it. No vendor firmware is involved.
 */
static void isp_start(Isp *t)
{
    uint16_t image[] = { 0x1111, 0x2222, 0x3333, 5, 0 };
    unsigned i;

    for (i = 0; i < 4; i++) {
        image[4] -= image[i];
    }
    for (i = 0; i < ARRAY_SIZE(image); i++) {
        g_assert_cmphex(isp_call(t, CMD_WRITE_RAM_WORD, CODE_ORG + i,
                                 image[i], 0), ==, MBOX_COMPLETE);
    }
    g_assert_cmphex(isp_call(t, CMD_VERIFY_CHECKSUM, CODE_ORG, 0, 0), ==,
                    MBOX_COMPLETE);
    g_assert_cmphex(isp_call(t, CMD_EXEC_FIRMWARE, CODE_ORG, 0, 0), ==,
                    MBOX_COMPLETE);
}

/* Rings of eight, port-database events on; the loop starts coming up. */
static void isp_init(Isp *t)
{
    uint64_t icb = guest_alloc(&alloc, ICB_SIZE);
    uint8_t buf[ICB_SIZE] = { 1 };
    uint16_t in[8];

    t->rq = guest_alloc(&alloc, RING_LEN * ENTRY_SIZE);
    t->rs = guest_alloc(&alloc, RING_LEN * ENTRY_SIZE);
    qtest_memset(t->qts, t->rs, 0, RING_LEN * ENTRY_SIZE);
    stw_le_p(buf + 2, ICB_OPT_PORT_DB);
    stw_le_p(buf + 4, 1024);
    stw_le_p(buf + 36, RING_LEN);
    stw_le_p(buf + 38, RING_LEN);
    stq_le_p(buf + 40, t->rq);
    stq_le_p(buf + 48, t->rs);
    qtest_memwrite(t->qts, icb, buf, sizeof(buf));

    memset(in, 0, sizeof(in));
    in[0] = CMD_INIT_FIRMWARE;
    in[2] = icb >> 16;
    in[3] = icb;
    in[6] = icb >> 48;
    in[7] = icb >> 32;
    isp_ring(t, in, ARRAY_SIZE(in));
    g_assert_cmphex(rd(t, MBOX(0)), ==, MBOX_COMPLETE);
    isp_ack(t);
}

static uint16_t isp_fw_state(Isp *t)
{
    uint16_t state;

    isp_ring(t, (const uint16_t[]) { CMD_GET_FW_STATE }, 1);
    g_assert_cmphex(rd(t, MBOX(0)), ==, MBOX_COMPLETE);
    state = rd(t, MBOX(1));
    isp_ack(t);
    return state;
}

/* The three events a bring-up posts, in order. */
static void isp_expect_bringup(Isp *t)
{
    uint16_t m1;

    g_assert_cmphex(isp_take(t, &m1), ==, ASYNC_LIP);
    g_assert_cmphex(isp_take(t, &m1), ==, ASYNC_LOOP_UP);
    g_assert_cmphex(isp_take(t, &m1), ==, ASYNC_PORT_DB);
    g_assert_cmphex(m1, ==, 0xffff);
    isp_expect_nothing(t);
}

/* The one port, loop ID 5, at AL_PA 0xe0. */
static void isp_expect_port_5(Isp *t)
{
    uint64_t buf = guest_alloc(&alloc, 8);
    const uint16_t in[] = {
        CMD_GET_ID_LIST, buf >> 16, buf, buf >> 48, 0, 0, buf >> 32,
    };
    uint8_t list[4];

    isp_ring(t, in, ARRAY_SIZE(in));
    g_assert_cmphex(rd(t, MBOX(0)), ==, MBOX_COMPLETE);
    g_assert_cmphex(rd(t, MBOX(1)), ==, 1);
    isp_ack(t);
    qtest_memread(t->qts, buf, list, sizeof(list));
    g_assert_cmphex(list[0], ==, 0xe0);
    g_assert_cmphex(list[3], ==, 5);
}

static void isp_publish(Isp *t, const uint8_t *e)
{
    qtest_memwrite(t->qts, t->rq + t->rq_next * ENTRY_SIZE, e, ENTRY_SIZE);
    t->rq_next = (t->rq_next + 1) % RING_LEN;
    wr(t, REG_REQUEST, t->rq_next);
}

/* A one-block READ to loop ID 5 into one segment at @addr. */
static void isp_read_entry(uint8_t *e, uint32_t handle, uint64_t addr)
{
    memset(e, 0, ENTRY_SIZE);
    e[0] = ENTRY_CMD64;
    e[1] = 1;
    stl_le_p(e + 4, handle);
    e[9] = 5;
    stw_le_p(e + 12, 0x20);
    stw_le_p(e + 18, 1);
    e[20] = 0x28;
    e[28] = 1;
    stl_le_p(e + 36, 512);
    stq_le_p(e + 40, addr);
    stl_le_p(e + 48, 512);
}

/*
 * Start a destination that waits for the stream, migrate the stopped
 * source into it and quit the source. The destination's test head is
 * mapped again before it runs: that mapping is not device state, and a
 * command the destination restarts moves its data at once.
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
    to->rs_next = from->rs_next;
}

static Isp isp_source_with(const char *drive)
{
    Isp t = { 0 };

    qpci_sgi_bridge_alloc_init(&alloc, 0);
    isp_attach(&t, qtest_initf("-M none " QPCI_SGI_BRIDGE_HEAD "%s"
                               ISP_DEVICES, drive));
    return t;
}

static Isp isp_source(void)
{
    return isp_source_with(ISP_DRIVE);
}

/* Polls until @qts reports @status, for at most ten seconds. */
static void wait_run_state(QTestState *qts, const char *status)
{
    unsigned i;

    for (i = 0; i < 10000; i++) {
        QDict *rsp = qtest_qmp_assert_success_ref(qts,
            "{ 'execute': 'query-status' }");
        bool match = g_str_equal(qdict_get_str(rsp, "status"), status);

        qobject_unref(rsp);
        if (match) {
            return;
        }
        g_usleep(1000);
    }
    g_assert_not_reached();
}

static void isp_finish(Isp *t)
{
    isp_detach(t);
    alloc_destroy(&alloc);
}

/* A loop still coming up comes up on the destination, whole. */
static void test_bringup_in_flight(void)
{
    g_autofree char *dir = g_dir_make_tmp("isp2200-migrate-XXXXXX", NULL);
    Isp from = isp_source(), to;

    isp_start(&from);
    isp_init(&from);
    g_assert_cmphex(isp_fw_state(&from), ==, 1);
    qtest_clock_step(from.qts, BRINGUP_NS / 2);

    /*
     * The timer travels with its deadline, but a qtest destination's
     * virtual clock restarts from its own zero, so a whole bring-up time
     * is stepped: enough under either clock, and the loop must still be
     * waiting until then.
     */
    isp_migrate(&from, &to, dir);
    g_assert_cmphex(isp_fw_state(&to), ==, 1);
    isp_expect_nothing(&to);
    qtest_clock_step(to.qts, BRINGUP_NS);
    isp_expect_bringup(&to);
    g_assert_cmphex(isp_fw_state(&to), ==, 3);
    isp_expect_port_5(&to);
    isp_finish(&to);
    g_rmdir(dir);
}

/*
 * An event posted and read but not released, two more queued, a No-op's
 * completion waiting in its slot behind it, and a register test whose
 * doorbell is latched behind that. On the destination the release posts
 * the completion ahead of the queued events and frees the slot, so the
 * latched command runs, on the inputs written before the migration, and
 * its result posts next; then the queued events.
 */
static void test_posted_and_waiting(void)
{
    static const uint16_t test_in[] = {
        CMD_REGISTER_TEST, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666,
        0x7777,
    };
    g_autofree char *dir = g_dir_make_tmp("isp2200-migrate-XXXXXX", NULL);
    Isp from = isp_source(), to;
    uint16_t m1;
    unsigned i;

    isp_start(&from);
    isp_init(&from);
    qtest_clock_step(from.qts, BRINGUP_NS);
    g_assert_cmphex(rd(&from, MBOX(0)), ==, ASYNC_LIP);
    isp_ring(&from, (const uint16_t[]) { CMD_NOP }, 1);
    isp_ring(&from, test_in, ARRAY_SIZE(test_in));
    g_assert_cmphex(rd(&from, REG_HCCR) & HCCR_HOST_INT, ==, HCCR_HOST_INT);

    isp_migrate(&from, &to, dir);
    g_assert_cmphex(rd(&to, REG_SEMAPHORE) & SEM_LOCK, ==, SEM_LOCK);
    g_assert_cmphex(rd(&to, REG_HCCR) & HCCR_HOST_INT, ==, HCCR_HOST_INT);
    isp_ack(&to);
    g_assert_cmphex(isp_take(&to, &m1), ==, MBOX_COMPLETE);
    /* Freeing the slot runs the waiting command, which takes the doorbell. */
    g_assert_cmphex(rd(&to, REG_HCCR) & HCCR_HOST_INT, ==, 0);
    g_assert_cmphex(rd(&to, REG_SEMAPHORE) & SEM_LOCK, ==, SEM_LOCK);
    g_assert_cmphex(rd(&to, MBOX(0)), ==, MBOX_COMPLETE);
    for (i = 1; i < ARRAY_SIZE(test_in); i++) {
        g_assert_cmphex(rd(&to, MBOX(i)), ==, test_in[i]);
    }
    isp_ack(&to);
    g_assert_cmphex(isp_take(&to, &m1), ==, ASYNC_LOOP_UP);
    g_assert_cmphex(isp_take(&to, &m1), ==, ASYNC_PORT_DB);
    g_assert_cmphex(m1, ==, 0xffff);
    isp_expect_nothing(&to);
    isp_finish(&to);
    g_rmdir(dir);
}

/*
 * A READ the source's disk failed, with the error stopping the source,
 * is still in flight: nothing has posted for it. It travels with its
 * handle and segment, and when the destination runs the SCSI layer
 * retries it there, on a disk that reads, so it completes whole into the
 * buffer the host gave it.
 */
static void test_command_in_flight(void)
{
    g_autofree char *dir = g_dir_make_tmp("isp2200-migrate-XXXXXX", NULL);
    Isp from = isp_source_with(ISP_DRIVE_READ_STOPS), to;
    uint8_t e[ENTRY_SIZE], data[512];
    uint64_t buf;
    unsigned i;

    isp_start(&from);
    isp_init(&from);
    qtest_clock_step(from.qts, BRINGUP_NS);
    isp_expect_bringup(&from);
    buf = guest_alloc(&alloc, sizeof(data));
    qtest_memset(from.qts, buf, 0xa5, sizeof(data));

    /* The reset's unit attention goes to a first, data-free command. */
    isp_read_entry(e, 0x0a0a0a0a, buf);
    e[20] = 0x00;
    stw_le_p(e + 18, 0);
    stl_le_p(e + 36, 0);
    isp_publish(&from, e);
    g_assert_cmphex(rd(&from, REG_RESPONSE), ==, 1);
    from.rs_next = 1;
    wr(&from, REG_RESPONSE, 1);
    wr(&from, REG_HCCR, HCCR_CLEAR_RISC);

    isp_read_entry(e, 0x00c0ffee, buf);
    e[25] = 8;
    isp_publish(&from, e);
    wait_run_state(from.qts, "io-error");
    isp_expect_nothing(&from);
    g_assert_cmphex(rd(&from, REG_RESPONSE), ==, 1);

    isp_migrate(&from, &to, dir);
    for (i = 0; i < 10000 && !(rd(&to, REG_ISR) & ISR_RISC); i++) {
        g_usleep(1000);
    }
    g_assert_cmphex(rd(&to, REG_ISR) & ISR_RISC, ==, ISR_RISC);
    g_assert_cmphex(rd(&to, REG_SEMAPHORE) & SEM_LOCK, ==, 0);
    g_assert_cmphex(rd(&to, REG_RESPONSE), ==, 2);
    qtest_memread(to.qts, to.rs + ENTRY_SIZE, e, sizeof(e));
    g_assert_cmphex(e[0], ==, ENTRY_STATUS);
    g_assert_cmphex(ldl_le_p(e + 4), ==, 0x00c0ffee);
    g_assert_cmphex(lduw_le_p(e + 8), ==, 0);
    g_assert_cmphex(lduw_le_p(e + 10), ==, COMPLETION_COMPLETE);
    qtest_memread(to.qts, buf, data, sizeof(data));
    for (i = 0; i < sizeof(data); i++) {
        g_assert_cmphex(data[i], ==, 0);
    }
    isp_finish(&to);
    g_rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("qlogic-isp2200/migration/bringup-in-flight",
                   test_bringup_in_flight);
    qtest_add_func("qlogic-isp2200/migration/posted-and-waiting",
                   test_posted_and_waiting);
    qtest_add_func("qlogic-isp2200/migration/command-in-flight",
                   test_command_in_flight);
    return g_test_run();
}
