/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Synthetic bank populations for the standalone IP30 HEART profile. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-migrate.h"

#define HEART_PATH "/machine/peripheral/heart"
#define HEART_REGS 0x0ff00000ULL
#define HEART_RAM 0x20000000ULL
#define UNIT (32ULL * 1024 * 1024)
#define PROBE_DESC 0x81bf0040U
#define PROBE_RAM (HEART_RAM + 0x40 * UNIT)

static QTestState *heart_start(unsigned mib, const char *extra)
{
    QTestState *s = qtest_initf("-M none -cpu R10000 -device "
        "sgi-heart,id=heart,bank0-size=%" PRIu64 " -S %s",
        (uint64_t)mib * 1024 * 1024, extra);

    qtest_sysbus_mmio_map(s, HEART_PATH, 0, HEART_REGS);
    return s;
}

static void test_descriptors(gconstpointer data)
{
    unsigned mib = GPOINTER_TO_UINT(data);
    QTestState *s = heart_start(mib, "");
    uint64_t span = (uint64_t)mib * 1024 * 1024;

    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x20), ==, 0);
    qtest_writel(s, HEART_REGS + 0x20, PROBE_DESC);
    g_assert_cmphex(qtest_readl(s, HEART_REGS + 0x20), ==, PROBE_DESC);
    qtest_writeq(s, PROBE_RAM + 0x100, 0x123456789abcdef0ULL);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x100 + span), ==,
                    0x123456789abcdef0ULL);
    qtest_writeq(s, PROBE_RAM + 0x100 + 2 * span,
                 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x100), ==,
                    0xfedcba9876543210ULL);

    /* An absent descriptor claims probe space but cannot retain a pattern. */
    qtest_writel(s, HEART_REGS + 0x20, 0);
    qtest_writel(s, HEART_REGS + 0x24, PROBE_DESC);
    qtest_writeq(s, PROBE_RAM + 0x100, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM + 0x100), ==, 0);
    qtest_writeq(s, HEART_REGS + 0x20, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x20), ==, 0);
    qtest_quit(s);
}

static void test_interrupts(void)
{
    QTestState *s = heart_start(128, "");

    qtest_writeq(s, HEART_REGS + 0x10008, 0x2);
    qtest_writeq(s, HEART_REGS + 0x10020, 0x3);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0x3);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10038), ==, 0);
    qtest_writeq(s, HEART_REGS + 0x10000, 0x1);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10038), ==, 0x1);
    qtest_writeq(s, HEART_REGS + 0x10000, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0x3);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10038), ==, 0);
    qtest_writeq(s, HEART_REGS + 0x10028, 0x1);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0x2);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10008), ==, 0x2);
    qtest_quit(s);
}

static void test_reset(void)
{
    QTestState *s = heart_start(128, "");

    qtest_writeq(s, HEART_REGS, 0x200810ULL);
    qtest_writeq(s, HEART_REGS + 0x08, 0x21);
    qtest_writeq(s, HEART_REGS + 0x10, 0x84000);
    qtest_writel(s, HEART_REGS + 0x20, PROBE_DESC);
    qtest_writeq(s, PROBE_RAM, 0xfeedface);
    qtest_writeq(s, HEART_REGS + 0x10000, 0x8);
    qtest_writeq(s, HEART_REGS + 0x10020, 0x8);
    qtest_clock_step(s, 800);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x20000), ==, 10);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(s, HEART_REGS), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x08), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10), ==, 0);
    g_assert_cmphex(qtest_readl(s, HEART_REGS + 0x20), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10000), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x10030), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x80), ==, 0x10);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x50000), ==, 0);
    g_assert_cmphex(qtest_readq(s, HEART_REGS + 0x20000), ==, 0);
    qtest_writel(s, HEART_REGS + 0x20, PROBE_DESC);
    g_assert_cmphex(qtest_readq(s, PROBE_RAM), ==, 0xfeedface);
    qtest_quit(s);
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("heart-migration-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = heart_start(128, "");
    to = heart_start(128, "-incoming defer");
    qtest_writeq(from, HEART_REGS, 0x800);
    qtest_writeq(from, HEART_REGS + 0x08, 0x21);
    qtest_writeq(from, HEART_REGS + 0x10, 0x84000);
    qtest_writel(from, HEART_REGS + 0x20, PROBE_DESC);
    qtest_writeq(from, PROBE_RAM, 0x123456789abcdef0ULL);
    qtest_writeq(from, HEART_REGS + 0x10000, 0x1);
    qtest_writeq(from, HEART_REGS + 0x10008, 0x2);
    qtest_writeq(from, HEART_REGS + 0x10020, 0x3);
    qtest_clock_step(from, 800);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, HEART_REGS), ==, 0x800);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x08), ==, 0x21);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10), ==, 0x84000);
    g_assert_cmphex(qtest_readl(to, HEART_REGS + 0x20), ==, PROBE_DESC);
    g_assert_cmphex(qtest_readq(to, PROBE_RAM), ==, 0x123456789abcdef0ULL);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10030), ==, 0x3);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10038), ==, 0x1);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x10008), ==, 0x2);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x20000), ==, 10);
    qtest_clock_step(to, 80);
    g_assert_cmphex(qtest_readq(to, HEART_REGS + 0x20000), ==, 11);
    qtest_quit(to);
    qtest_quit(from);
    if (g_file_test(socket, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(socket), ==, 0);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_data_func("/heart/descriptor-128m", GUINT_TO_POINTER(128),
                         test_descriptors);
    g_test_add_data_func("/heart/descriptor-256m", GUINT_TO_POINTER(256),
                         test_descriptors);
    g_test_add_func("/heart/interrupts", test_interrupts);
    g_test_add_func("/heart/reset", test_reset);
    g_test_add_func("/heart/migration", test_migration);
    return g_test_run();
}
