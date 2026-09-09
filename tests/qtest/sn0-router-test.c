/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Router register target tests. The host fixture does not model a fabric. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

static void set(QTestState *qts, const char *name, uint64_t value)
{
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/router',"
        "'property':%s,'value':%llu}}", name, (unsigned long long)value);
}

static uint64_t get(QTestState *qts, const char *name)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/router',"
        "'property':%s}}", name);
    uint64_t value;

    g_assert_true(qdict_haskey(response, "return"));
    value = qdict_get_int(response, "return");
    qobject_unref(response);
    return value;
}

static uint64_t read_reg(QTestState *qts, uint64_t addr)
{
    set(qts, "address", addr);
    return get(qts, "register");
}

static void write_reg(QTestState *qts, uint64_t addr, uint64_t value)
{
    set(qts, "address", addr);
    set(qts, "register", value);
}

static QTestState *start(const char *extra)
{
    return qtest_initf("-M none -device sgi-router-test,id=router %s", extra);
}

static void test_registers(void)
{
    QTestState *qts = start("");

    set(qts, "port", 5);
    g_assert_cmphex(read_reg(qts, 0), ==, (UINT64_C(5) << 46) | 0x1111);
    g_assert_cmphex(read_reg(qts, 0x10), ==, 0x3f);
    g_assert_cmphex(read_reg(qts, 0x18), ==, UINT64_C(0x3fffffe0a01ffff1));
    write_reg(qts, 0x20, UINT64_C(0x123456789abcdef0));
    g_assert_cmphex(read_reg(qts, 0x20), ==, UINT64_C(0x123456789abcdef0));
    write_reg(qts, 0x28, UINT64_C(0x12345678));
    g_assert_cmphex(read_reg(qts, 0x28), ==, 0x5678);
    write_reg(qts, 0x28, 0);
    set(qts, "exchange", 0xffff);
    g_assert_cmphex(get(qts, "exchange"), ==, 0);
    set(qts, "exchange", 0x1234);
    g_assert_cmphex(get(qts, "exchange"), ==, 0xffff);
    g_assert_cmphex(read_reg(qts, 0x28), ==, 0xffff);
    for (unsigned int i = 0; i < 48; i++) {
        uint64_t address = 0x70000 + i * 8;

        write_reg(qts, address, UINT64_C(0xabc00000) | i);
        g_assert_cmphex(read_reg(qts, address), ==, 0xc00000 | i);
    }
    for (unsigned int port = 1; port <= 6; port++) {
        uint64_t base = port << 16;

        g_assert_cmphex(read_reg(qts, base), ==, 0x1bf0);
        g_assert_cmphex(read_reg(qts, base + 8), ==, 0);
        g_assert_cmphex(read_reg(qts, base + 0x10), ==, 0);
        g_assert_cmphex(read_reg(qts, base + 0x18), ==, 0x3f);
        write_reg(qts, base, 0x123fffff);
        g_assert_cmphex(read_reg(qts, base), ==, 0xfffff);
        write_reg(qts, base + 0x18, 0x40 | port);
        g_assert_cmphex(read_reg(qts, base + 0x18), ==, port);
        set(qts, "port", port);
        set(qts, "port-errors", 0x12345);
        write_reg(qts, base + 8, 0x45);
        g_assert_cmphex(read_reg(qts, base + 0x88), ==, 0x12300);
        g_assert_cmphex(read_reg(qts, base + 8), ==, 0);
    }
    qtest_quit(qts);
}

static void test_links_reset(void)
{
    QTestState *qts = start("");

    for (unsigned int port = 1; port <= 6; port++) {
        uint64_t working = UINT64_C(1) << (26 + 3 * port);
        uint64_t width = working << 1;

        set(qts, "port", port);
        set(qts, "link", 3);
        g_assert_cmphex(read_reg(qts, 0) & (working | width), ==,
                        working | width);
        set(qts, "link", 0);
        g_assert_cmphex(read_reg(qts, 0) & (working | width), ==, 0);
        set(qts, "link", 1);
        g_assert_cmphex(read_reg(qts, 0) & (working | width), ==, working);
        write_reg(qts, 0x30, (UINT64_C(1) << (5 + port)) |
                            (UINT64_C(1) << (11 + port)));
        g_assert_cmphex(get(qts, "diagnostic"), ==, 2);
        write_reg(qts, 0x30, 0);
        g_assert_cmphex(get(qts, "diagnostic"), ==, 1);
        write_reg(qts, (port << 16) + 0x18, port);
    }
    write_reg(qts, 0x20, 0x12345);
    write_reg(qts, 0x10, 0x1fffff);
    write_reg(qts, 0x70120, 0x123456);
    qtest_system_reset(qts);
    g_assert_cmphex(read_reg(qts, 0x20), ==, 0);
    g_assert_cmphex(read_reg(qts, 0x10), ==, 0x3f);
    for (unsigned int port = 1; port <= 6; port++) {
        set(qts, "port", port);
        g_assert_cmphex(get(qts, "diagnostic"), ==, 1);
        g_assert_cmphex(read_reg(qts, (port << 16) + 0x18), ==, 0x3f);
        g_assert_cmphex(read_reg(qts, 0) &
                        (UINT64_C(3) << (26 + 3 * port)), ==, 0);
    }
    qtest_quit(qts);
}

static void test_migration(void)
{
    g_autofree char *directory = g_dir_make_tmp("router-migrate-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(directory, "socket", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start("");
    QTestState *to = start("-incoming defer");
    int64_t deadline = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;

    for (unsigned int i = 0; i < 48; i++) {
        write_reg(from, 0x70000 + i * 8, 0x654300 | i);
    }
    write_reg(from, 0x10, 0x12345);
    write_reg(from, 0x30, 0x12345);
    for (unsigned int port = 1; port <= 6; port++) {
        write_reg(from, (port << 16) + 0x18, port);
        set(from, "port", port);
        set(from, "link", port & 3);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(from,
                                             "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(qdict_get_qdict(response, "return"),
                                           "status");

        if (!strcmp(status, "completed")) {
            break;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    for (unsigned int i = 0; i < 48; i++) {
        g_assert_cmphex(read_reg(to, 0x70000 + i * 8), ==, 0x654300 | i);
    }
    g_assert_cmphex(read_reg(to, 0x10), ==, 0x12345);
    g_assert_cmphex(read_reg(to, 0x30), ==, 0x12345);
    for (unsigned int port = 1; port <= 6; port++) {
        g_assert_cmphex(read_reg(to, (port << 16) + 0x18), ==, port);
        set(from, "port", port);
        set(to, "port", port);
        g_assert_cmphex(read_reg(to, 0), ==, read_reg(from, 0));
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/router/registers", test_registers);
    qtest_add_func("sn0/router/links-reset", test_links_reset);
    qtest_add_func("sn0/router/migration", test_migration);
    return g_test_run();
}
