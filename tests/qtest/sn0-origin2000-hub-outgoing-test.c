/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define CONTROL 0x40000000000ULL
#define HUB 0x20001000000ULL
#define BWIN 0x20020000000ULL
#define PROBE 0x01600000ULL
#define WIDGET_SHIFT 34
#define RECORD_VALUE 0x123456789abcdef0ULL

static QTestState *start(const char *extra)
{
    return qtest_initf("-M origin2000-hub-outgoing-test,record-only=on %s",
                       extra);
}

static uint64_t cpu_read(QTestState *q, unsigned node, uint64_t addr)
{
    uint8_t bytes[8];
    g_assert_true(qtest_memread_cpu(q, node * 2, addr, bytes, 8));
    return ldq_be_p(bytes);
}

static void cpu_write(QTestState *q, unsigned node, uint64_t addr,
                      uint64_t value)
{
    uint8_t bytes[8];
    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(q, node * 2, addr, bytes, 8));
}

static uint64_t outgoing_access(QTestState *q, unsigned node, uint64_t addr,
                        unsigned size, bool write)
{
    qtest_writeq(q, CONTROL + 80, node);
    qtest_writeq(q, CONTROL, addr);
    qtest_writeq(q, CONTROL + 16, size);
    qtest_writeq(q, CONTROL + 24, write);
    return qtest_readq(q, CONTROL + 32);
}

static void test_cpu_encoding(void)
{
    QTestState *q = start("");
    unsigned node;

    for (node = 0; node < 4; node++) {
        qtest_writeq(q, CONTROL + 40, 0);
        cpu_write(q, node, HUB + 0x400160, 0x1800);
        g_assert_cmphex(cpu_read(q, node, BWIN + PROBE), ==, RECORD_VALUE);
        g_assert_cmpuint(qtest_readq(q, CONTROL + 40), ==, 1);
        g_assert_cmpuint(qtest_readq(q, CONTROL + 88), ==, node);
        g_assert_cmphex(qtest_readq(q, CONTROL + 56), ==,
                        (8ULL << 48) | (1ULL << 47) | PROBE);
        g_assert_cmpuint(qtest_readq(q, CONTROL + 64), ==, 8);

        /* Only changing the space selects the original memory plane. */
        qtest_writeq(q, CONTROL + 40, 0);
        cpu_write(q, node, HUB + 0x400160, 0x800);
        g_assert_cmphex(cpu_read(q, node, BWIN + PROBE), ==, RECORD_VALUE);
        g_assert_cmpuint(qtest_readq(q, CONTROL + 40), ==, 0);
        g_assert_cmphex(qtest_readq(q, CONTROL + 104), ==,
                        (8ULL << WIDGET_SHIFT) | PROBE);
        cpu_write(q, node, HUB + 0x400160, 0xb03);
        cpu_read(q, node, BWIN + 0x1234);
        g_assert_cmphex(qtest_readq(q, CONTROL + 104), ==,
                        (11ULL << WIDGET_SHIFT) | (3ULL << 29) | 0x1234);
    }
    qtest_quit(q);
}

static void test_admission(void)
{
    QTestState *q = start("");
    uint64_t addr = (8ULL << WIDGET_SHIFT) | PROBE;
    unsigned widget, bit, size;

    for (widget = 8; widget < 16; widget++) {
        g_assert_cmphex(outgoing_access(q, 2,
                         (uint64_t)widget << WIDGET_SHIFT | PROBE, 8, false),
                        ==, 0);
        g_assert_cmphex(qtest_readq(q, CONTROL + 8), ==, RECORD_VALUE);
        g_assert_cmphex(qtest_readq(q, CONTROL + 48), ==, 0x11234);
        g_assert_cmphex(qtest_readq(q, CONTROL + 56), ==,
                        (uint64_t)widget << 48 | (1ULL << 47) | PROBE);
        g_assert_cmpuint(qtest_readq(q, CONTROL + 64), ==, 8);
        g_assert_cmpuint(qtest_readq(q, CONTROL + 88), ==, 2);
    }
    qtest_writeq(q, CONTROL + 40, 0);
    for (widget = 0; widget < 8; widget++) {
        g_assert_cmphex(outgoing_access(q, 2,
                         (uint64_t)widget << WIDGET_SHIFT | PROBE, 8, false),
                        !=, 0);
    }
    for (bit = 0; bit < WIDGET_SHIFT; bit++) {
        g_assert_cmphex(outgoing_access(q, 2, addr ^ (1ULL << bit), 8, false),
                        !=, 0);
    }
    for (size = 1; size < 8; size *= 2) {
        g_assert_cmphex(outgoing_access(q, 2, addr, size, false), !=, 0);
    }
    g_assert_cmphex(outgoing_access(q, 2, addr, 8, true), !=, 0);
    g_assert_cmphex(outgoing_access(q, 2, 1ULL << 38, 8, false), !=, 0);
    g_assert_cmphex(outgoing_access(q, 2, (1ULL << 38) - 4, 8, false), !=, 0);
    g_assert_cmpuint(qtest_readq(q, CONTROL + 40), ==, 0);
    qtest_writeq(q, CONTROL + 72, 1);
    g_assert_cmphex(outgoing_access(q, 2, addr, 8, false), !=, 0);
    g_assert_cmpuint(qtest_readq(q, CONTROL + 40), ==, 1);
    qtest_quit(q);
}

static void test_cpu_windows(void)
{
    QTestState *q = start("");
    unsigned entry, slice;

    for (entry = 1; entry < 7; entry++) {
        cpu_write(q, 0, HUB + 0x400160 + entry * 8, 0x1800);
        g_assert_cmphex(cpu_read(q, 0, BWIN + entry * 0x20000000ULL + PROBE),
                        ==, 0);
    }
    for (slice = 1; slice < 32; slice++) {
        cpu_write(q, 0, HUB + 0x400160, 0x1800 | slice);
        g_assert_cmphex(cpu_read(q, 0, BWIN + PROBE), ==, 0);
    }
    g_assert_cmpuint(qtest_readq(q, CONTROL + 40), ==, 0);
    cpu_write(q, 0, HUB + 0x400160, 0x1800);
    g_assert_cmphex(cpu_read(q, 0, BWIN + PROBE), ==, RECORD_VALUE);
    qtest_quit(q);
}

static void test_reset(void)
{
    QTestState *q = start("");
    cpu_write(q, 0, HUB + 0x400160, 0x1800);
    g_assert_cmphex(cpu_read(q, 0, BWIN + PROBE), ==, RECORD_VALUE);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    qtest_qmp_eventwait(q, "RESET");
    qtest_clock_step(q, 1);
    qtest_writeq(q, CONTROL + 40, 0);
    g_assert_cmphex(cpu_read(q, 0, HUB + 0x400160), ==, 0);
    cpu_read(q, 0, BWIN + PROBE);
    g_assert_cmpuint(qtest_readq(q, CONTROL + 40), ==, 0);
    g_assert_cmphex(qtest_readq(q, CONTROL + 104), ==, PROBE);
    cpu_write(q, 0, HUB + 0x400160, 0x1900);
    g_assert_cmphex(cpu_read(q, 0, BWIN + PROBE), ==, RECORD_VALUE);
    qtest_quit(q);
}

static QTestState *start_peers(const char *extra)
{
    return qtest_initf("-M origin2000-hub-outgoing-test %s", extra);
}

static void health(QTestState *q, unsigned mask)
{
    qtest_qmp_assert_success(q,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/test-crossbow0','property':'links-alive','value':%u}}",
        mask);
}

static void test_peers(void)
{
    QTestState *q = start_peers("");
    uint64_t first, second;
    unsigned masks[] = { 0x400, 0x200 };
    unsigned i;

    /* Descriptor widgets 9 and 10 connect physical nodes 0 and 2. */
    cpu_write(q, 0, HUB + 0x600000, 0);
    cpu_write(q, 2, HUB + 0x600000, 1ULL << 18);
    first = cpu_read(q, 0, HUB + 0x600000);
    second = cpu_read(q, 2, HUB + 0x600000);
    g_assert_cmphex(first, !=, second);
    g_assert_cmphex(first & 0xff00, ==, second & 0xff00);
    cpu_write(q, 0, HUB + 0x400160, 0x1a00);
    cpu_write(q, 2, HUB + 0x400160, 0x1900);
    g_assert_cmphex(cpu_read(q, 0, BWIN + PROBE), ==, second);
    g_assert_cmphex(cpu_read(q, 2, BWIN + PROBE), ==, first);
    cpu_write(q, 2, HUB + 0x600000, (0x23ULL << 8) | (1ULL << 18));
    second = cpu_read(q, 2, HUB + 0x600000);
    g_assert_cmphex(cpu_read(q, 0, BWIN + PROBE), ==, second);
    for (i = 0; i < G_N_ELEMENTS(masks); i++) {
        health(q, masks[i]);
        g_assert_cmphex(cpu_read(q, 0, BWIN + PROBE), ==, 0);
        g_assert_cmphex(outgoing_access(q, 0, (10ULL << 34) | PROBE,
                                       8, false), !=, 0);
        health(q, 0x600);
        g_assert_cmphex(cpu_read(q, 0, BWIN + PROBE), ==, second);
    }
    cpu_write(q, 0, HUB + 0x400160, 0x1900);
    g_assert_cmphex(cpu_read(q, 0, BWIN + PROBE), ==, 0);
    g_assert_cmphex(outgoing_access(q, 0, (9ULL << 34) | PROBE, 8, false),
                    !=, 0);
    cpu_write(q, 0, HUB + 0x400160, 0x1800);
    g_assert_cmphex(cpu_read(q, 0, BWIN + PROBE), ==, 0);
    health(q, 0x200);
    qtest_system_reset(q);
    g_assert_cmphex(cpu_read(q, 0, HUB + 0x400160), ==, 0);
    cpu_write(q, 0, HUB + 0x400160, 0x1a00);
    g_assert_cmphex(cpu_read(q, 0, BWIN + PROBE), ==,
                    cpu_read(q, 2, HUB + 0x600000));
    qtest_quit(q);
}

static void test_peer_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("hub-outgoing-XXXXXX", NULL);
    g_autofree char *socket = g_strdup_printf("%s/migrate.sock", dir);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *extra = g_strdup_printf("-incoming %s", uri);
    QTestState *from = start_peers("");
    QTestState *to = start_peers(extra);
    uint64_t identity;

    cpu_write(from, 0, HUB + 0x400160, 0x1a00);
    cpu_write(from, 2, HUB + 0x600000, (0x23ULL << 8) | (1ULL << 18));
    identity = cpu_read(from, 2, HUB + 0x600000);
    g_assert_cmphex(cpu_read(from, 0, BWIN + PROBE), ==, identity);
    health(from, 0x200);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    g_test_timer_start();
    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(from,
            "{'execute':'query-migrate'}");
        g_autofree char *status = g_strdup(qdict_get_str(ret, "status"));
        qobject_unref(ret);
        g_assert_cmpstr(status, !=, "failed");
        if (!strcmp(status, "completed")) {
            break;
        }
        g_assert_cmpfloat(g_test_timer_elapsed(), <, 30);
        g_usleep(1000);
    }
    g_assert_cmphex(cpu_read(to, 0, HUB + 0x400160), ==, 0x1a00);
    g_assert_cmphex(cpu_read(to, 2, HUB + 0x600000), ==, identity);
    g_assert_cmphex(cpu_read(to, 0, BWIN + PROBE), ==, 0);
    health(to, 0x600);
    g_assert_cmphex(cpu_read(to, 0, BWIN + PROBE), ==, identity);
    cpu_write(to, 2, HUB + 0x600000, 0x45ULL << 8);
    g_assert_cmphex(cpu_read(to, 0, BWIN + PROBE), ==,
                    cpu_read(to, 2, HUB + 0x600000));
    qtest_quit(to);
    qtest_quit(from);
    g_unlink(socket);
    g_rmdir(dir);
}

static void test_teardown(void)
{
    QTestState *q = start_peers("");
    cpu_write(q, 0, HUB + 0x400160, 0x1a00);
    cpu_read(q, 0, BWIN + PROBE);
    qtest_writeq(q, CONTROL + 112, 1);
    g_assert_cmpuint(qtest_readq(q, CONTROL + 112), ==, 1);
    g_assert_cmphex(outgoing_access(q, 0, (10ULL << 34) | PROBE, 8, false),
                    !=, 0);
    qtest_quit(q);
    q = start("");
    qtest_writeq(q, CONTROL + 112, 1);
    g_assert_cmpuint(qtest_readq(q, CONTROL + 112), ==, 1);
    g_assert_cmphex(outgoing_access(q, 0, (10ULL << 34) | PROBE, 8, false),
                    !=, 0);
    qtest_quit(q);
}

static void test_retained_lifetime(const void *opaque)
{
    bool receiver = GPOINTER_TO_UINT(opaque);
    QTestState *q = qtest_initf("-M origin2000-hub-outgoing-test,"
                              "record-only=on,check-lifetime=on,"
                              "receiver-lifetime=%s", receiver ? "on" : "off");
    qtest_writeq(q, CONTROL + 120, 1);
    g_test_timer_start();
    while (!qtest_readq(q, CONTROL + 120)) {
        g_assert_cmpfloat(g_test_timer_elapsed(), <, 10);
        g_usleep(1000);
    }
    g_assert_cmpuint(qtest_readq(q, CONTROL + 128), ==, 0);
    qtest_writeq(q, CONTROL + 136, 1);
    g_assert_cmphex(qtest_readq(q, CONTROL + 32), !=, 0);
    qtest_writeq(q, CONTROL + 128, 1);
    g_test_timer_start();
    while (!qtest_readq(q, CONTROL + 128)) {
        g_assert_cmpfloat(g_test_timer_elapsed(), <, 10);
        g_usleep(1000);
    }
    g_assert_cmpuint(qtest_readq(q, CONTROL + 128), ==, 1);
    qtest_quit(q);
}

static void test_geometry(void)
{
    QTestState *q = qtest_init("-M origin2000-hub-outgoing-test,"
                             "record-only=on,check-geometry=on");
    qtest_quit(q);
}

static void test_unset(void)
{
    QTestState *q = qtest_init("-M origin200");
    cpu_write(q, 0, HUB + 0x400160, 0x1800);
    g_assert_cmphex(cpu_read(q, 0, BWIN + PROBE), ==, 0);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sn0/hub-outgoing/cpu-encoding", test_cpu_encoding);
    qtest_add_func("/sn0/hub-outgoing/admission", test_admission);
    qtest_add_func("/sn0/hub-outgoing/cpu-windows", test_cpu_windows);
    qtest_add_func("/sn0/hub-outgoing/reset", test_reset);
    qtest_add_func("/sn0/hub-outgoing/unset", test_unset);
    qtest_add_func("/sn0/hub-outgoing/geometry", test_geometry);
    qtest_add_func("/sn0/hub-outgoing/peers", test_peers);
    qtest_add_func("/sn0/hub-outgoing/peer-migration", test_peer_migration);
    qtest_add_func("/sn0/hub-outgoing/teardown", test_teardown);
    qtest_add_data_func("/sn0/hub-outgoing/retained-lifetime",
                        GUINT_TO_POINTER(0), test_retained_lifetime);
    qtest_add_data_func("/sn0/hub-outgoing/retained-receiver-lifetime",
                        GUINT_TO_POINTER(1), test_retained_lifetime);
    return g_test_run();
}
