/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Synthetic error latch clearing and migration. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-migrate.h"
#include "sgi-launch.h"

#define BASE 0x14000000ULL
#define MTE 0x15003000ULL
#define CRIME_PATH "/machine/peripheral/crime"
#define INJECT_PATH "/machine/peripheral/inject"

static QTestState *start(const char *extra)
{
    QTestState *s = qtest_initf(
        "-M none -cpu R10000 -S -object memory-backend-ram,id=fit,size=64M "
        "-device memory-transaction-test,id=memtx,"
        "migrate-ram=/objects/fit/fit[0] "
        "-device sgi-crime,id=crime,ram=/objects/fit/fit[0],"
        "crime-profile=legacy-software,functional-memory-transfer=on "
        "-device sgi-crime-test-fixture,id=inject,crime="
        CRIME_PATH " %s", extra);

    qtest_sysbus_mmio_map(s, CRIME_PATH, 0, BASE);
    qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
    return s;
}

static void seed(QTestState *s, const char *property, uint64_t value)
{
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':%s,'property':%s,"
        "'value':%" PRIu64 "}}", INJECT_PATH, property, value);
}

static void seed_errors(QTestState *s)
{
    const char *properties[] = {
        "cpu-error-address", "cpu-error-status", "memory-error-address",
        "memory-error-status",
    };
    const uint64_t values[] = {
        0xfedcba9876543210ULL, 5, 0x23456789, 7U << 25
    };

    seed(s, "cpu-error-address", 0xfedcba9876543210ULL);
    seed(s, "cpu-error-status", 5);
    seed(s, "memory-error-address", 0x23456789);
    seed(s, "memory-error-status", 7U << 25);
    for (unsigned i = 0; i < ARRAY_SIZE(properties); i++) {
        g_autoptr(QDict) reply = qtest_qmp(s,
            "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
            INJECT_PATH, properties[i]);

        g_assert_cmphex(qdict_get_uint(reply, "return"), ==, values[i]);
    }
}

static void assert_errors(QTestState *s, uint32_t cpu_status)
{
    g_assert_cmphex(qtest_readq(s, BASE + 0x40), ==, 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(s, BASE + 0x48), ==, cpu_status);
    g_assert_cmphex(qtest_readq(s, BASE + 0x250), ==, 7U << 25);
    g_assert_cmphex(qtest_readq(s, BASE + 0x258), ==, 0x23456789);
}

static void assert_outputs(QTestState *s, bool pending)
{
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmpint(qtest_get_irq(s, i), ==, i == 0 && pending);
    }
}

static void assert_stopped(QTestState *s)
{
    g_autoptr(QDict) status =
        qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");

    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "internal-error");
}

static void test_clear_isolation(void)
{
    QTestState *s = start("");
    uint32_t pending = (1U << 4) | (1U << 20) | (1U << 21);

    seed_errors(s);
    qtest_writeq(s, BASE + 0x18, pending);
    assert_outputs(s, false);
    qtest_set_irq_in(s, CRIME_PATH, "mace", 4, 1);
    qtest_set_irq_in(s, CRIME_PATH, "external", 4, 1);
    qtest_set_irq_in(s, CRIME_PATH, "external", 5, 1);
    assert_outputs(s, true);
    qtest_writeq(s, BASE + 0x250, 0);
    qtest_writeq(s, BASE + 0x200, 1);
    g_assert_cmphex(qtest_readq(s, BASE + 0x200), ==, 1);
    qtest_writeq(s, BASE + 0x200, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x200), ==, 0);
    assert_errors(s, 5);
    qtest_writeq(s, BASE + 0x48, 0);
    assert_errors(s, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, pending);
    assert_outputs(s, true);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    g_assert_cmpint(memtx_writeq(s, BASE + 0x48, 3), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, BASE + 0x40, 0), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, BASE + 0x250, 1), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, BASE + 0x258, 0), !=, MEMTX_OK);
    assert_errors(s, 0);
    assert_stopped(s);
    qtest_quit(s);
}

static void test_transfer_failure_retention(void)
{
    QTestState *s = start("");
    uint8_t bytes[64];

    seed_errors(s);
    qtest_memset(s, 0, 0x6c, sizeof(bytes));
    qtest_writeq(s, BASE + 0x200, 1);
    qtest_writeq(s, BASE + 0x18, 1U << 4);
    qtest_set_irq_in(s, CRIME_PATH, "mace", 4, 1);
    qtest_writel(s, MTE, (1U << 11) | (4U << 5) | (5U << 2));
    qtest_writel(s, MTE + 0x28, 63);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x18, 0xa5);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 63), !=, MEMTX_OK);
    assert_errors(s, 5);
    assert_stopped(s);
    g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, 1U << 4);
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1U << 4);
    g_assert_cmphex(qtest_readq(s, BASE + 0x200), ==, 1);
    assert_outputs(s, true);
    qtest_memread(s, 0, bytes, sizeof(bytes));
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        g_assert_cmphex(bytes[i], ==, 0x6c);
    }
    qtest_quit(s);
}

static void test_seed_validation(void)
{
    QTestState *s = start("");
    const char *properties[] = {
        "cpu-error-status", "memory-error-status", "memory-error-address",
    };
    const uint64_t invalid[] = { 8, 1, 0x40000000 };

    seed_errors(s);
    for (unsigned i = 0; i < ARRAY_SIZE(properties); i++) {
        g_autoptr(QDict) reply = qtest_qmp(s,
            "{'execute':'qom-set','arguments':{'path':%s,'property':%s,"
            "'value':%" PRIu64 "}}", INJECT_PATH, properties[i], invalid[i]);

        g_assert_nonnull(qdict_get_qdict(reply, "error"));
        assert_errors(s, 5);
    }
    qtest_quit(s);
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("crime-errors-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = start(""), *to = start("-incoming defer");
    uint32_t hardware = (1U << 16) | (1U << 20) | (1U << 21) | (1U << 31);

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    seed_errors(from);
    qtest_writeq(from, BASE + 0x200, 1);
    qtest_set_irq_in(from, CRIME_PATH, "external", 0, 1);
    qtest_set_irq_in(from, CRIME_PATH, "external", 0, 0);
    qtest_set_irq_in(from, CRIME_PATH, "external", 4, 1);
    qtest_set_irq_in(from, CRIME_PATH, "external", 5, 1);
    qtest_set_irq_in(from, CRIME_PATH, "external", 15, 1);
    qtest_writeq(from, BASE + 0x20, 1U << 30);
    qtest_writeq(from, 0x15001700, 0x8000000380000009ULL);
    qtest_writeq(from, 0x15001780, 0x8000000b8000000fULL);
    qtest_writel(from, MTE, 0xdeadbeef);
    qtest_writel(from, MTE + 8, 0x12345678);
    qtest_memset(from, 0x3000, 0x5d, 128);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    assert_errors(to, 5);
    g_assert_cmphex(qtest_readq(to, BASE + 0x28), ==, hardware);
    g_assert_cmphex(qtest_readq(to, BASE + 0x20), ==, 1U << 30);
    g_assert_cmphex(qtest_readq(to, BASE + 0x200), ==, 1);
    g_assert_cmphex(qtest_readq(to, 0x15001700), ==, 0x8000000380000009ULL);
    g_assert_cmphex(qtest_readq(to, 0x15001780), ==, 0x8000000b8000000fULL);
    g_assert_cmphex(qtest_readl(to, MTE), ==, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(to, MTE + 8), ==, 0x12345678);
    assert_outputs(to, false);
    qtest_writeq(to, BASE + 0x18, hardware | (1U << 30));
    assert_outputs(to, true);
    qtest_writeq(to, BASE + 0x48, 0);
    assert_errors(to, 0);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    g_assert_cmpint(memtx_writel(to, MTE + 0x800, UINT32_MAX), !=, MEMTX_OK);
    assert_stopped(to);
    assert_errors(to, 0);
    g_assert_cmphex(qtest_readl(to, MTE), ==, 0xdeadbeef);
    for (unsigned i = 0; i < 128; i++) {
        g_assert_cmphex(qtest_readb(to, 0x3000 + i), ==, 0x5d);
    }
    qtest_system_reset(to);
    g_assert_cmphex(qtest_readq(to, BASE + 0x40), ==, 0);
    g_assert_cmphex(qtest_readq(to, BASE + 0x48), ==, 0);
    g_assert_cmphex(qtest_readq(to, BASE + 0x250), ==, 0);
    g_assert_cmphex(qtest_readq(to, BASE + 0x258), ==, 0);
    g_assert_cmphex(qtest_readq(to, BASE + 0x200), ==, 0);
    g_assert_cmphex(qtest_readq(to, BASE + 0x28), ==, hardware & ~(1U << 16));
    g_assert_cmphex(qtest_readq(to, BASE), ==, 0xa1);
    assert_outputs(to, false);
    qtest_quit(from);
    qtest_quit(to);
    g_rmdir(directory);
}

static void test_requires_qtest_server(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "o2-test,crime-profile=legacy-software,"
        "bank-profile=8x128m-synthetic", "-m", "1G", "-cpu", "R10000",
        "-display", "none", "-nodefaults", "-S", "-qmp", "stdio",
        "-device", "sgi-crime-test-fixture,crime=/machine/crime", NULL,
    };

    assert_launch_rejected(args, "requires a qtest server");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/crime/errors/clear-isolation", test_clear_isolation);
    qtest_add_func("/sgi/crime/errors/transfer-failure-retention",
                   test_transfer_failure_retention);
    qtest_add_func("/sgi/crime/errors/seed-validation", test_seed_validation);
    qtest_add_func("/sgi/crime/errors/migration", test_migration);
    qtest_add_func("/sgi/crime/errors/requires-qtest-server",
                   test_requires_qtest_server);
    return g_test_run();
}
