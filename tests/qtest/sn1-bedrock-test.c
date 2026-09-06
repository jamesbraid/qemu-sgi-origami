/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define BEDROCK_PATH "/machine/peripheral/bedrock"
#define BASE UINT64_C(0x10000000)
#define PIO_BASE UINT64_C(0x100000000)
#define INBOUND_BASE UINT64_C(0x20000000000)
#define IO_BASE (BASE + UINT64_C(0x400000))
#define LB_BASE (BASE + UINT64_C(0x600000))

#define MACHINE \
    "-M none -cpu R10000 -m 256M " \
    "-object memory-backend-ram,id=dma,size=2M " \
    "-object memory-backend-ram,id=widget8,size=2M " \
    "-object memory-backend-ram,id=widget9,size=2M " \
    "-device sgi-bedrock,id=bedrock,revision=2," \
    "dma-memory=/objects/dma/dma[0]," \
    "widget-8=/objects/widget8/widget8[0]," \
    "widget-9=/objects/widget9/widget9[0] -S"

static QTestState *bedrock_start(const char *extra)
{
    QTestState *s = qtest_initf(MACHINE " %s", extra);

    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 0, BASE);
    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 1, PIO_BASE);
    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 2, INBOUND_BASE);
    return s;
}

static void test_parent_dma_and_reset(void)
{
    QTestState *s = bedrock_start("");
    uint8_t source[128], actual[128];

    for (unsigned i = 0; i < sizeof(source); i++) {
        source[i] = i ^ 0xa5;
    }
    qtest_memwrite(s, INBOUND_BASE + 0x10000, source, sizeof(source));
    qtest_memset(s, INBOUND_BASE + 0x20000, 0, sizeof(actual));
    qtest_memset(s, 0x20000, 0x6d, sizeof(actual));
    qtest_writeq(s, IO_BASE + 0x10000, 1 | (1 << 20));
    qtest_writeq(s, IO_BASE + 0x10008, 0x10000);
    qtest_writeq(s, IO_BASE + 0x10010, 0x20000);
    qtest_writeq(s, IO_BASE + 0x10018, 1 << 4);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    qtest_clock_step(s, 1);
    qtest_memread(s, INBOUND_BASE + 0x20000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
    qtest_memread(s, 0x20000, actual, sizeof(actual));
    for (unsigned i = 0; i < sizeof(actual); i++) {
        g_assert_cmphex(actual[i], ==, 0x6d);
    }
    qtest_writel(s, INBOUND_BASE + 0x30000, 0x89abcdef);
    g_assert_cmphex(qtest_readl(s, INBOUND_BASE + 0x30000), ==, 0x89abcdef);
    g_assert_cmphex(qtest_readl(s, 0x30000), ==, 0);

    qtest_writeq(s, IO_BASE + 0x150, UINT64_C(0xfeedfacecafebeef));
    qtest_writeq(s, LB_BASE + 0x8008, UINT64_C(0x0123456789abcdef));
    qtest_writeq(s, BASE + 0x680008, 4);
    g_assert_cmphex(qtest_readq(s, IO_BASE + 0x150), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + 0x8008), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_quit(s);
}

static void test_parent_identity_and_pio(void)
{
    QTestState *s = bedrock_start("");

    g_assert_cmphex(qtest_readq(s, IO_BASE), ==, UINT64_C(0x2c11006c));
    g_assert_cmphex(qtest_readq(s, LB_BASE) & UINT64_C(0xffffffff), ==,
                    UINT64_C(0x2301206c));

    qtest_writeq(s, IO_BASE + 0x160, UINT64_C(0x800));
    qtest_writel(s, PIO_BASE + 0x20000100, 0x12345678);
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x20000100), ==, 0x12345678);
    qtest_writeq(s, IO_BASE + 0x160, UINT64_C(0x900));
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x20000100), ==, 0);
    qtest_writeq(s, IO_BASE + 0x100, 0x55);
    qtest_writeq(s, IO_BASE + 0x10100, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, IO_BASE + 0x100), ==, 0x55);
    qtest_quit(s);
}

static void migrate_wait(QTestState *from)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    for (;;) {
        g_autoptr(QDict) response =
            qtest_qmp(from, "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(qdict_get_qdict(response, "return"),
                                           "status");

        if (!strcmp(status, "completed")) {
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void test_parent_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sn1-bedrock-migration-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = bedrock_start("");
    to = bedrock_start("-incoming defer");
    qtest_writeq(from, IO_BASE + 0x150, UINT64_C(0x13579bdf2468ace0));
    qtest_writeq(from, IO_BASE + 0x160, 0x800);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    migrate_wait(from);
    g_assert_cmphex(qtest_readq(to, IO_BASE + 0x150), ==,
                    UINT64_C(0x13579bdf2468ace0));
    g_assert_cmphex(qtest_readq(to, IO_BASE + 0x160), ==, 0x800);
    qtest_writel(to, PIO_BASE + 0x20000100, 0x55aa33cc);
    g_assert_cmphex(qtest_readl(to, PIO_BASE + 0x20000100), ==, 0x55aa33cc);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/bedrock/composition/identity-pio",
                   test_parent_identity_and_pio);
    qtest_add_func("/sgi/bedrock/composition/dma-reset",
                   test_parent_dma_and_reset);
    qtest_add_func("/sgi/bedrock/composition/migration",
                   test_parent_migration);
    return g_test_run();
}
