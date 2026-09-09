/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define CONTROL 0x10000000ULL
#define CPU (1ULL << 54)
#define PACKET (2ULL << 54)
#define XBOW "/machine/peripheral/test/xbow"

static QTestState *start(const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
                                "-device sgi-xbow-transport-test,id=test %s",
                                extra);

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/test", 0, CONTROL);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/test", 1, CPU);
    qtest_sysbus_mmio_map(qts, "/machine/peripheral/test", 2, PACKET);
    return qts;
}

static uint64_t field(QTestState *qts, unsigned endpoint, unsigned offset)
{
    return qtest_readq(qts, CONTROL + endpoint * 32 + offset);
}

static void test_separate_faces(void)
{
    QTestState *qts = start("");
    uint64_t high = (1ULL << 47) | (1ULL << 33) | 0x12340;

    qtest_writeq(qts, CPU + (10ULL << 34) + (1ULL << 33) + 0x12340,
                 0x1020304050607080ULL);
    g_assert_cmphex(field(qts, 2, 8), ==, (1ULL << 33) + 0x12340);
    g_assert_cmphex(field(qts, 2, 16), ==, 0x1020304050607080ULL);
    qtest_writeq(qts, PACKET + (10ULL << 48) + high, 0x8877665544332211ULL);
    g_assert_cmphex(field(qts, 3, 0), ==, 1);
    g_assert_cmphex(field(qts, 3, 8), ==, high);
    g_assert_cmphex(field(qts, 3, 16), ==, 0x8877665544332211ULL);
    g_assert_cmphex(field(qts, 2, 0), ==, 1);
    g_assert_cmphex(qtest_readq(qts, PACKET + (10ULL << 48) + high), ==,
                    0x8877665544332211ULL);
    qtest_quit(qts);
}

static void health(QTestState *qts, unsigned mask)
{
    qtest_qmp_assert_success(qts,
        "{ 'execute': 'qom-set', 'arguments': { 'path': '" XBOW "',"
        "'property': 'links-alive', 'value': %u } }", mask);
}

static void test_gating(void)
{
    QTestState *qts = start("");
    uint64_t cpu = CPU + (10ULL << 34) + 0x80;
    uint64_t packet = PACKET + (10ULL << 48) + 0x80;
    unsigned masks[] = { 0x500, 0x300 };
    unsigned i;

    for (i = 0; i < G_N_ELEMENTS(masks); i++) {
        health(qts, masks[i]);
        qtest_writeq(qts, cpu, 1);
        qtest_writeq(qts, packet, 2);
        g_assert_cmphex(field(qts, 2, 0), ==, i);
        g_assert_cmphex(field(qts, 3, 0), ==, i);
        health(qts, 0x700);
        qtest_writeq(qts, cpu, 3);
        qtest_writeq(qts, packet, 4);
    }
    qtest_writeq(qts, PACKET + (9ULL << 48) + 0x80, 5);
    qtest_writeq(qts, PACKET + (11ULL << 48) + 0x80, 6);
    g_assert_cmphex(field(qts, 2, 0), ==, 2);
    g_assert_cmphex(field(qts, 3, 0), ==, 2);
    health(qts, 0x300);
    qtest_system_reset(qts);
    qtest_writeq(qts, packet, 7);
    g_assert_cmphex(field(qts, 3, 16), ==, 7);
    qtest_quit(qts);
}

static void test_attributes_errors(void)
{
    QTestState *qts = start("");

    qtest_writeq(qts, CONTROL + 0x80, (10ULL << 48) | 0x100);
    qtest_writeq(qts, CONTROL + 0x88, 1);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x80), ==, 0);
    g_assert_cmphex(field(qts, 3, 24), ==, 0x1234);
    qtest_writeq(qts, CONTROL + 0x88, 2);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x80), ==, 0);
    g_assert_cmphex(field(qts, 3, 16), ==, 0x1032547698badcfeULL);
    g_assert_cmphex(field(qts, 3, 24), ==, 0x1234);
    qtest_writeq(qts, CONTROL + 0x80, (10ULL << 48) | 0xdead0);
    qtest_writeq(qts, CONTROL + 0x88, 1);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x80), !=, 0);
    qtest_writeq(qts, PACKET + (10ULL << 48) + 0x100,
                 0xfedcba9876543210ULL);
    g_assert_cmphex(field(qts, 3, 0), ==, 4);
    g_assert_cmphex(field(qts, 3, 16), ==, 0xfedcba9876543210ULL);
    qtest_writeq(qts, CONTROL + 0x88, 2);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x80), !=, 0);
    g_assert_cmphex(field(qts, 3, 0), ==, 5);
    g_assert_cmphex(field(qts, 3, 16), ==, 0xfedcba9876543210ULL);
    g_assert_cmphex(field(qts, 3, 24), ==, 0x1234);
    qtest_quit(qts);
}

static void test_bounds_guards(void)
{
    QTestState *qts = start("");

    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x88), ==, 0xff);
    qtest_writeb(qts, CPU + (8ULL << 34) + 4095, 0x12);
    qtest_writeb(qts, CPU + (8ULL << 34) + 4096, 0x34);
    g_assert_cmphex(field(qts, 0, 0), ==, 1);
    g_assert_cmphex(field(qts, 0, 8), ==, 4095);
    qtest_writeb(qts, PACKET + (8ULL << 48) + 8191, 0x56);
    qtest_writeb(qts, PACKET + (8ULL << 48) + 8192, 0x78);
    g_assert_cmphex(field(qts, 1, 0), ==, 1);
    g_assert_cmphex(field(qts, 1, 8), ==, 8191);
    qtest_writeb(qts, PACKET + (11ULL << 48) - 1, 0x9a);
    g_assert_cmphex(field(qts, 3, 8), ==, (1ULL << 48) - 1);
    qtest_writeb(qts, PACKET + (11ULL << 48), 0xbc);
    g_assert_cmphex(field(qts, 3, 0), ==, 1);
    qtest_quit(qts);
}

static void test_interrupt_packet_face(void)
{
    QTestState *qts = start("");

    /* The existing access-error event must send through the packet face. */
    qtest_writel(qts, CPU + 0x34, (0x27U << 24) | (10U << 16) | 0x8002);
    qtest_writel(qts, CPU + 0x3c, 0x12340);
    qtest_writel(qts, CPU + 0x24, 1U << 5);
    qtest_readl(qts, CPU + 0x7c);
    g_assert_cmphex(field(qts, 3, 8), ==,
                    (1ULL << 47) | (1ULL << 33) | 0x12340);
    g_assert_cmphex(field(qts, 3, 16), ==, 0x127);
    g_assert_cmphex(field(qts, 2, 0), ==, 0);
    qtest_readl(qts, CPU + 0x54);
    g_assert_cmphex(field(qts, 3, 16), ==, 0x27);
    qtest_quit(qts);
}

static void test_migration(gconstpointer opaque)
{
    g_autofree char *dir = g_dir_make_tmp("xbow-transport-XXXXXX", NULL);
    g_autofree char *socket = g_strdup_printf("%s/migrate.sock", dir);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *extra = g_strdup_printf("-incoming %s", uri);
    QTestState *from = start("");
    QTestState *to = start(extra);
    uint64_t address = PACKET + (10ULL << 48) + (1ULL << 47) + 0x100;
    uint64_t cpu_address = CPU + (10ULL << 34) + (1ULL << 33);

    qtest_writeq(from, address, 0xaabbccdd);
    qtest_writeq(from, cpu_address, 0x99887766);
    health(from, GPOINTER_TO_UINT(opaque));
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    g_test_timer_start();
    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(from,
            "{ 'execute': 'query-migrate' }");
        g_autofree char *status = g_strdup(qdict_get_str(ret, "status"));
        qobject_unref(ret);
        g_assert_cmpstr(status, !=, "failed");
        if (!strcmp(status, "completed")) {
            break;
        }
        g_assert_cmpfloat(g_test_timer_elapsed(), <, 30);
        g_usleep(1000);
    }
    qtest_writeq(to, address, 0x11223344);
    qtest_writeq(to, cpu_address, 0x55667788);
    qtest_readq(to, address);
    qtest_readq(to, cpu_address);
    g_assert_cmphex(field(to, 3, 16), ==, 0xaabbccdd);
    g_assert_cmphex(field(to, 2, 16), ==, 0x99887766);
    g_assert_cmphex(field(to, 3, 0), ==, 1);
    g_assert_cmphex(field(to, 2, 0), ==, 1);
    health(to, 0x700);
    qtest_writeq(to, address, 0x11223344);
    g_assert_cmphex(field(to, 3, 16), ==, 0x11223344);
    qtest_writeq(to, cpu_address, 0x55667788);
    g_assert_cmphex(field(to, 2, 16), ==, 0x55667788);
    qtest_quit(to);
    qtest_quit(from);
    g_unlink(socket);
    g_rmdir(dir);
}

static QTestState *start_refusal(void)
{
    return start("-global sgi-xbow-transport-test.stop-on-link-reset=on");
}

static void reset_write(QTestState *qts, unsigned link, bool packet)
{
    qtest_writeq(qts, CONTROL + 0x80, 0x134 + link * 0x40);
    qtest_writeq(qts, CONTROL + 0xc0, packet);
}

static void test_reset_refusal(void)
{
    QTestState *qts = start_refusal();
    uint64_t state;
    uint32_t registers[8][3];
    const unsigned offsets[] = { 4, 12, 60 };
    uint64_t high = qtest_readq(qts, CONTROL + 0xa0);
    uint64_t low = qtest_readq(qts, CONTROL + 0xa8);
    unsigned i, j, port;

    qtest_writel(qts, CPU + 0x10c, (1U << 24) | 2);
    state = qtest_readq(qts, CONTROL + 0x90);
    for (port = 0; port < 8; port++) {
        for (j = 0; j < G_N_ELEMENTS(offsets); j++) {
            registers[port][j] = qtest_readl(qts,
                CPU + 0x100 + port * 0x40 + offsets[j]);
        }
    }

    for (i = 0; i < 8; i++) {
        reset_write(qts, i, i & 1);
        g_assert_cmphex(qtest_readq(qts, CONTROL + 0x80), !=, 0);
        g_assert_cmphex(qtest_readq(qts, CONTROL + 0x90), ==, state);
        g_assert_cmphex(qtest_readq(qts, CONTROL + 0x98), ==, 0);
        for (port = 0; port < 8; port++) {
            for (j = 0; j < G_N_ELEMENTS(offsets); j++) {
                g_assert_cmphex(qtest_readl(qts,
                    PACKET + 0x100 + port * 0x40 + offsets[j]), ==,
                    registers[port][j]);
            }
        }
    }
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(qtest_readq(qts, CONTROL + 0xa0), ==, high);
    g_assert_cmpuint(qtest_readq(qts, CONTROL + 0xa8), ==, low);
    qtest_writeq(qts, PACKET + (10ULL << 48) + 0x100, 0x1234);
    g_assert_cmphex(field(qts, 3, 16), ==, 0x1234);
    qtest_quit(qts);
}

static void test_quiesce(void)
{
    QTestState *qts = start("");
    uint64_t low;

    reset_write(qts, 0, true);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x80), ==, 0);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x98), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, CONTROL + 0xa0), ==, 1);
    low = qtest_readq(qts, CONTROL + 0xa8);
    qtest_writeq(qts, CONTROL + 0xb8, 1);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x98), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, CONTROL + 0xa8), ==, low + 1);
    qtest_writeq(qts, CONTROL + 0xc8, 1);
    qtest_writeq(qts, CONTROL + 0xb8, 1);
    qtest_system_reset(qts);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(qtest_readq(qts, CONTROL + 0xa8), ==, low + 1);
    qtest_writeq(qts, PACKET + (10ULL << 48) + 0x100, 0x1234);
    g_assert_cmphex(field(qts, 3, 0), ==, 0);
    {
        QDict *error = qtest_qmp_assert_failure_ref(qts,
            "{ 'execute': 'qom-set', 'arguments': { 'path': '" XBOW "',"
            "'property': 'links-alive', 'value': 1792 } }");
        qobject_unref(error);
    }
    reset_write(qts, 2, false);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x80), !=, 0);
    qtest_quit(qts);
}

static void test_reset_load(gconstpointer opaque)
{
    bool truncated = GPOINTER_TO_UINT(opaque);
    QTestState *qts = start_refusal();
    uint64_t high = qtest_readq(qts, CONTROL + 0xa0);
    uint64_t low = qtest_readq(qts, CONTROL + 0xa8);

    qtest_writeq(qts, CONTROL + 0xd0, truncated);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x80), !=, 0);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x98), ==, 0);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    qtest_clock_step(qts, 3000000);
    g_assert_cmpuint(qtest_readq(qts, CONTROL + 0xa0), ==, high);
    g_assert_cmpuint(qtest_readq(qts, CONTROL + 0xa8), ==, low);
    qtest_quit(qts);
}

static void test_stale_reset_timer(void)
{
    QTestState *qts = start_refusal();
    uint64_t low = qtest_readq(qts, CONTROL + 0xa8);

    qtest_writeq(qts, CONTROL + 0xd8, 1);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(qtest_readq(qts, CONTROL + 0xa8), ==, low);
    qtest_quit(qts);
}

static void test_default_reset_load(void)
{
    QTestState *qts = start("");
    uint64_t low = qtest_readq(qts, CONTROL + 0xa8);
    uint64_t high = qtest_readq(qts, CONTROL + 0xa0);

    qtest_writeq(qts, CONTROL + 0xd0, 0);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x80), ==, 0);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x98), ==, 1);
    g_assert_cmpuint(qtest_readq(qts, CONTROL + 0xa0), ==, high + 1);
    /* Restoration reports all eight levels, seven of them low. */
    g_assert_cmpuint(qtest_readq(qts, CONTROL + 0xa8), ==, low + 7);
    g_assert_cmpuint(qtest_readq(qts, CONTROL + 0xb0), ==, 1000000);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
    qtest_clock_step(qts, 999999);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x98), ==, 1);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x98), ==, 0);
    g_assert_cmpuint(qtest_readq(qts, CONTROL + 0xa8), ==, low + 8);
    qtest_quit(qts);
}

static void wait_status(QTestState *qts, const char *status)
{
    int64_t deadline = g_get_monotonic_time() + 5000000;

    for (;;) {
        QDict *reply = qtest_qmp(qts, "{ 'execute': 'query-status' }");
        bool matches = !strcmp(qdict_get_str(qdict_get_qdict(reply, "return"),
                                            "status"), status);
        qobject_unref(reply);
        if (matches) {
            return;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void test_running_refusal(void)
{
    QTestState *qts = qtest_initf(
        "-M sgi-xbow-transport-test -cpu R10000 -S -accel tcg "
        "-device loader,addr=0xffffffff80000000,cpu-num=0 "
        "-global sgi-xbow-transport-test.stop-on-link-reset=on");
    uint32_t program[] = {
        0x3c088000, 0x8d090100, 0x25290001,
        0xad090100, 0x1000fffc, 0,
    };
    unsigned i, pass;

    /* Public MIPS instructions increment RAM, proving CPU execution. */
    for (i = 0; i < G_N_ELEMENTS(program); i++) {
        qtest_writel(qts, i * 4, program[i]);
    }
    for (pass = 0; pass < 2; pass++) {
        int64_t deadline = g_get_monotonic_time() + 5000000;
        qtest_writel(qts, 0x100, 0);
        qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
        while (!qtest_readl(qts, 0x100)) {
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        }
        reset_write(qts, pass, pass);
        g_assert_cmphex(qtest_readq(qts, CONTROL + 0x80), !=, 0);
        wait_status(qts, "internal-error");
        g_assert_cmphex(qtest_readq(qts, CONTROL + 0x98), ==, 0);
        qtest_system_reset(qts);
    }
    qtest_quit(qts);
}

static void test_lifetime(void)
{
    QTestState *qts = start("");
    unsigned cycle;

    for (cycle = 0; cycle < 2; cycle++) {
        int64_t deadline = g_get_monotonic_time() + 5000000;
        qtest_writeq(qts, CONTROL + 0xe0, 1);
        g_assert_cmpuint(qtest_readq(qts, CONTROL + 0xe0), ==, cycle);
        qtest_writeq(qts, CONTROL + 0xe0, 0);
        while (qtest_readq(qts, CONTROL + 0xe0) != cycle + 1) {
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(1000);
        }
    }
    qtest_writeq(qts, CONTROL + 0xe8, 1);
    g_assert_cmphex(qtest_readq(qts, CONTROL + 0x80), ==, 3);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/xbow-transport/bounds-guards", test_bounds_guards);
    qtest_add_func("/sn0/xbow-transport/interrupt-packet-face",
                   test_interrupt_packet_face);
    qtest_add_data_func("/sn0/xbow-transport/migration/source-down",
                        GUINT_TO_POINTER(0x500), test_migration);
    qtest_add_data_func("/sn0/xbow-transport/migration/destination-down",
                        GUINT_TO_POINTER(0x300), test_migration);
    qtest_add_func("/sn0/xbow-transport/separate-faces", test_separate_faces);
    qtest_add_func("/sn0/xbow-transport/gating", test_gating);
    qtest_add_func("/sn0/xbow-transport/attributes-errors",
                   test_attributes_errors);
    qtest_add_func("/sn0/xbow-transport/reset-refusal", test_reset_refusal);
    qtest_add_func("/sn0/xbow-transport/quiesce", test_quiesce);
    qtest_add_func("/sn0/xbow-transport/stale-reset-timer",
                   test_stale_reset_timer);
    qtest_add_data_func("/sn0/xbow-transport/reset-load/invalid", NULL,
                        test_reset_load);
    qtest_add_data_func("/sn0/xbow-transport/reset-load/truncated",
                        GUINT_TO_POINTER(1), test_reset_load);
    qtest_add_func("/sn0/xbow-transport/reset-load/default",
                   test_default_reset_load);
    qtest_add_func("/sn0/xbow-transport/running-refusal", test_running_refusal);
    qtest_add_func("/sn0/xbow-transport/lifetime", test_lifetime);
    return g_test_run();
}
