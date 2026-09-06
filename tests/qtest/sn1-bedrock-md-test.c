/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define BASE UINT64_C(0x78000000)
#define MD(_x) (BASE + (_x))

static QTestState *start(const char *extra)
{
    QTestState *s = qtest_initf("-M none -cpu R10000 "
                                "-device sgi-bedrock-md,id=md -S %s", extra);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/md", 0, BASE);
    return s;
}

static void test_reset(void)
{
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), UINT64_MAX);
    qtest_writeq(s, MD(0x20000), UINT64_C(0x8000000000000555));
    qtest_writeq(s, MD(0x40000), 3);
    qtest_writeq(s, MD(0x10010), 0x12345);
    qtest_writeq(s, MD(0), 0);
    qtest_set_irq_in(s, "/machine/peripheral/md", "soft-reset", 0, 1);
    g_assert_cmphex(qtest_readq(s, MD(0x8)), ==,
                    UINT64_C(0x373ff003fbfbfbfb));
    g_assert_cmphex(qtest_readq(s, MD(0x20000)), ==,
                    UINT64_C(0x8000000000000555));
    g_assert_cmphex(qtest_readq(s, MD(0x40000)), ==, 3);
    g_assert_cmphex(qtest_readq(s, MD(0x10010)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(0)), ==, UINT64_MAX);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, MD(0x8)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(0x20000)), ==, 0);
    qtest_quit(s);
}

static void test_register_contract(void)
{
    QTestState *s = start("");
    const uint8_t bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t actual[8];

    g_assert_cmphex(qtest_readq(s, MD(0)), ==, UINT64_MAX);
    qtest_writeq(s, MD(0), 0);
    g_assert_cmphex(qtest_readq(s, MD(0)), ==, 0);
    qtest_writeq(s, MD(0x8), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x8)), ==,
                    UINT64_C(0x373ff003fbfbfbfb));
    qtest_writeq(s, MD(0x10), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x10)), ==, 0xff);
    qtest_writeq(s, MD(0x10000), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x10000)), ==,
                    UINT64_C(0x8fffffffffff7ff3));
    qtest_writeq(s, MD(0x10010), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x10010)), ==,
                    (UINT64_C(1) << 41) - 1);
    qtest_writeq(s, MD(0x20000), UINT64_C(0x8000000000000c35));
    g_assert_cmphex(qtest_readq(s, MD(0x20000)), ==,
                    UINT64_C(0x8000000000000c35));
    qtest_writeq(s, MD(0x20008), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x20008)), ==,
                    UINT64_C(0xffffffffffff));
    qtest_writeq(s, MD(0x40000), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x40000)), ==, 3);
    g_assert_cmphex(qtest_readq(s, MD(0x60)), ==,
                    UINT64_C(0x001818180c0c0c00));
    g_assert_cmphex(qtest_readq(s, MD(0x40050)), ==,
                    UINT64_C(0x0000000004040400));
    qtest_writeq(s, MD(0x20030), 1);
    g_assert_cmphex(qtest_readq(s, MD(0x20030)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(0x20048)), ==, 0);
    qtest_writeq(s, MD(0x60040), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x60040)), ==, 1);
    qtest_writeq(s, MD(0x10100), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x10010)), ==,
                    (UINT64_C(1) << 41) - 1);

    qtest_memwrite(s, MD(0x20008), bytes, sizeof(bytes));
    qtest_memread(s, MD(0x20008), actual, sizeof(actual));
    {
        const uint8_t expected[8] = { 0, 0, 3, 4, 5, 6, 7, 8 };

        g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    }
    g_assert_cmphex(qtest_readq(s, MD(0x20008)), ==,
                    UINT64_C(0x030405060708));
    qtest_writeb(s, MD(0x20008), 0);
    g_assert_cmphex(qtest_readq(s, MD(0x20008)), ==,
                    UINT64_C(0x030405060708));
    qtest_quit(s);
}

static void test_writable_masks(void)
{
    static const struct {
        uint64_t offset;
        uint64_t mask;
    } registers[] = {
        { 0x18, UINT64_C(0x100ff03ff) },
        { 0x10000, UINT64_C(0x8fffffffffff7ff3) },
        { 0x10008, UINT64_C(0x8fffffffffff7ff3) },
        { 0x10010, UINT64_C(0x1ffffffffff) },
        { 0x10018, UINT64_C(0x1ffffffffff) },
        { 0x20038, 0xff }, { 0x20060, 3 },
        { 0x40040, UINT64_C(0x8000003f00007fff) },
        { 0x40048, UINT64_C(0xc000003f00007fff) },
        { 0x40060, 3 }, { 0x60000, 1 }, { 0x60040, 1 },
    };
    QTestState *s = start("");

    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        uint64_t value = UINT64_MAX ^ (UINT64_C(1) << (i + 2));

        for (unsigned j = 0; j < ARRAY_SIZE(registers); j++) {
            qtest_writeq(s, MD(registers[j].offset), 0);
        }
        qtest_writeq(s, MD(registers[i].offset), value);
        g_assert_cmphex(qtest_readq(s, MD(registers[i].offset)), ==,
                        value & registers[i].mask);
        for (unsigned j = 0; j < ARRAY_SIZE(registers); j++) {
            if (j != i) {
                g_assert_cmphex(qtest_readq(s, MD(registers[j].offset)), ==,
                                0);
            }
        }
    }
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

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-md-migration-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start("");
    to = start("-incoming defer");
    qtest_writeq(from, MD(0), 0x55aa);
    qtest_writeq(from, MD(0x10010), UINT64_C(0x123456789ab));
    qtest_writeq(from, MD(0x20008), UINT64_C(0x112233445566));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    migrate_wait(from);
    g_assert_cmphex(qtest_readq(to, MD(0)), ==, 0x55aa);
    g_assert_cmphex(qtest_readq(to, MD(0x10010)), ==,
                    UINT64_C(0x123456789ab));
    g_assert_cmphex(qtest_readq(to, MD(0x20008)), ==,
                    UINT64_C(0x112233445566));
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/bedrock-md/register-contract", test_register_contract);
    qtest_add_func("/sgi/bedrock-md/reset", test_reset);
    qtest_add_func("/sgi/bedrock-md/migration", test_migration);
    qtest_add_func("/sgi/bedrock-md/writable-masks", test_writable_masks);
    return g_test_run();
}
