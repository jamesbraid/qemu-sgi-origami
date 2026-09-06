/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Isolated Bedrock NI MMIO and reset requests. The empty machine's R10000
 * supplies qtest's address space only: no firmware or CPU execution occurs,
 * and this fixture does not represent an Origin 300.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define NI_PATH "/machine/peripheral/ni"
#define NI_BASE 0x10000000ULL
#define MACHINE "-M none -cpu R10000 -device sgi-bedrock-ni,id=ni -S"

static QTestState *ni_start(const char *extra)
{
    QTestState *s = qtest_initf(MACHINE " %s", extra);

    qtest_sysbus_mmio_map(s, NI_PATH, 0, NI_BASE);
    return s;
}

static void test_controls(void)
{
    QTestState *s = ni_start("");
    static const struct {
        uint64_t offset;
        uint64_t mask;
    } controls[] = {
        { 0x10, 1 }, { 0x18, 7 }, { 0x20, 0x1ff },
        { 0x30, 0x7efffffffULL }, { 0x38, 0xffff },
    };

    for (size_t i = 0; i < ARRAY_SIZE(controls); i++) {
        uint64_t addr = NI_BASE + controls[i].offset;
        uint64_t mask = controls[i].mask;

        qtest_writeq(s, addr, UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, addr), ==, mask);
        qtest_writeq(s, addr, 0xaaaaaaaaaaaaaaaaULL);
        g_assert_cmphex(qtest_readq(s, addr), ==,
                        mask & 0xaaaaaaaaaaaaaaaaULL);
        qtest_writeq(s, addr, 0);
        g_assert_cmphex(qtest_readq(s, addr), ==, 0);
    }
    qtest_quit(s);
}

static void test_byte_order_and_width(void)
{
    QTestState *s = ni_start("");
    const uint8_t bytes[8] = { 0, 0, 0, 0, 0, 0, 0x5a, 0xa5 };
    uint8_t actual[8];
    uint64_t addr = NI_BASE + 0x38;

    qtest_memwrite(s, addr, bytes, sizeof(bytes));
    g_assert_cmphex(qtest_readq(s, addr), ==, 0x5aa5);
    qtest_memread(s, addr, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), bytes, sizeof(bytes));

    /* Invalid widths and alignment must not alter a writable register. */
    qtest_writeb(s, addr + 7, 0);
    qtest_writew(s, addr + 6, 0);
    qtest_writel(s, addr + 4, 0);
    qtest_writeq(s, addr + 1, 0);
    g_assert_cmphex(qtest_readq(s, addr), ==, 0x5aa5);
    qtest_quit(s);
}

static void test_routing_storage(void)
{
    QTestState *s = ni_start("");

    for (unsigned i = 0; i < 128; i++) {
        qtest_writeq(s, NI_BASE + 0x1000 + i * 8, 0xe0 | (i % 32));
    }
    qtest_writeq(s, NI_BASE + 0x2000, 0xf3);
    for (unsigned i = 0; i < 128; i++) {
        g_assert_cmphex(qtest_readq(s, NI_BASE + 0x1000 + i * 8),
                        ==, i % 32);
    }
    g_assert_cmphex(qtest_readq(s, NI_BASE + 0x2000), ==, 0x13);
    qtest_quit(s);
}

static void test_reset_requests(void)
{
    QTestState *s = ni_start("");
    uint64_t pulses = 0;

    qtest_irq_intercept_out_named(s, NI_PATH, "local-reset");
    for (unsigned enable = 0; enable < 2; enable++) {
        qtest_writeq(s, NI_BASE + 0x10, enable);
        for (unsigned command = 0; command < 8; command++) {
            qtest_writeq(s, NI_BASE + 8, command);
            pulses += !!(command & 4);
            g_assert_cmpuint(qtest_get_irq_raise_count(s, 0), ==, pulses);
            g_assert_false(qtest_get_irq(s, 0));
        }
    }
    /* With no parent connected, the request does not reset this device. */
    g_assert_cmphex(qtest_readq(s, NI_BASE + 0x10), ==, 1);
    qtest_quit(s);
}

static void test_disconnected(void)
{
    QTestState *s = ni_start("");

    qtest_irq_intercept_out_named(s, NI_PATH, "error");
    qtest_writeq(s, NI_BASE, UINT64_MAX);
    qtest_writeq(s, NI_BASE + 0x18, 7);
    g_assert_cmphex(qtest_readq(s, NI_BASE), ==, 0);
    g_assert_cmphex(qtest_readq(s, NI_BASE + 0x100), ==, 0);
    g_assert_cmphex(qtest_readq(s, NI_BASE + 0x100), ==, 0);
    g_assert_cmphex(qtest_readq(s, NI_BASE + 0x120), ==, 0);
    g_assert_cmphex(qtest_readq(s, NI_BASE + 0x100), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_quit(s);
}

/*
 * A write to a read-only register changes nothing and is logged as a guest
 * error, so a guest driving the link's status or error capture leaves a
 * trace instead of vanishing into accepted storage. The register-core unit
 * test checks the unchanged state with seeded values; here every register
 * is idle, so the log is what discriminates.
 */
static void test_read_only_writes(void)
{
    g_autofree char *log = g_strdup_printf("%s/sn1-ni-readonly-XXXXXX.log",
                                           g_get_tmp_dir());
    g_autofree char *extra = NULL;
    g_autofree char *found = NULL;
    static const uint64_t offsets[] = { 0, 0x100, 0x108, 0x110, 0x118, 0x120 };
    QTestState *s;
    int fd = g_mkstemp(log);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = g_strdup_printf("-d guest_errors -D %s", log);
    s = ni_start(extra);
    for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
        qtest_writeq(s, NI_BASE + offsets[i], UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, NI_BASE + offsets[i]), ==, 0);
    }
    qtest_quit(s);

    g_assert_true(g_file_get_contents(log, &found, NULL, NULL));
    for (size_t i = 0; i < ARRAY_SIZE(offsets); i++) {
        g_autofree char *line = g_strdup_printf(
            "write to read-only register 0x%" PRIx64, offsets[i]);

        g_assert_nonnull(strstr(found, line));
    }
    g_assert_cmpint(unlink(log), ==, 0);
}

static void test_system_reset(void)
{
    QTestState *s = ni_start("");
    uint64_t sideband = qtest_readq(s, NI_BASE + 0x118);
    uint64_t age = qtest_readq(s, NI_BASE + 0x38);
    uint64_t route = qtest_readq(s, NI_BASE + 0x1000);

    qtest_writeq(s, NI_BASE + 0x38, age ^ 0xffff);
    qtest_writeq(s, NI_BASE + 0x1000, route ^ 0x1f);
    qtest_system_reset(s);
    /* Hardware defaults are unknown; compare with observed startup state. */
    g_assert_cmphex(qtest_readq(s, NI_BASE + 0x38), ==, age);
    g_assert_cmphex(qtest_readq(s, NI_BASE + 0x1000), ==, route);
    g_assert_cmphex(qtest_readq(s, NI_BASE + 0x118), ==, sideband);
    qtest_quit(s);
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-ni-migration-XXXXXX",
                                               &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    int64_t deadline;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = ni_start("");
    to = ni_start("-incoming defer");
    qtest_writeq(from, NI_BASE + 0x10, 1);
    qtest_writeq(from, NI_BASE + 0x30, 0x56abcdef0ULL);
    qtest_writeq(from, NI_BASE + 0x38, 0xa55a);
    qtest_writeq(from, NI_BASE + 0x1000, 0x15);
    qtest_writeq(from, NI_BASE + 0x13f8, 0x19);
    qtest_writeq(from, NI_BASE + 0x2000, 0x17);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(from,
                                            "{'execute':'query-migrate'}");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_str(result, "status");

        if (!strcmp(status, "completed")) {
            break;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readq(to, NI_BASE + 0x10), ==, 1);
    g_assert_cmphex(qtest_readq(to, NI_BASE + 0x30), ==, 0x56abcdef0ULL);
    g_assert_cmphex(qtest_readq(to, NI_BASE + 0x38), ==, 0xa55a);
    g_assert_cmphex(qtest_readq(to, NI_BASE + 0x1000), ==, 0x15);
    g_assert_cmphex(qtest_readq(to, NI_BASE + 0x13f8), ==, 0x19);
    g_assert_cmphex(qtest_readq(to, NI_BASE + 0x2000), ==, 0x17);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-ni/controls", test_controls);
    qtest_add_func("sn1/bedrock-ni/byte-order-width",
                   test_byte_order_and_width);
    qtest_add_func("sn1/bedrock-ni/routing-storage", test_routing_storage);
    qtest_add_func("sn1/bedrock-ni/reset-requests", test_reset_requests);
    qtest_add_func("sn1/bedrock-ni/disconnected", test_disconnected);
    qtest_add_func("sn1/bedrock-ni/read-only-writes", test_read_only_writes);
    qtest_add_func("sn1/bedrock-ni/system-reset", test_system_reset);
    qtest_add_func("sn1/bedrock-ni/migration", test_migration);
    return g_test_run();
}
