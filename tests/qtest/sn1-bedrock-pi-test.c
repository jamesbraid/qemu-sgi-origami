/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Isolated Bedrock composition and processor interrupt contract. The empty
 * machine's R10000 supplies an address space only; no CPU executes.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define BEDROCK_PATH "/machine/peripheral/bedrock"
#define BASE 0x10000000ULL
#define PI(n, reg) (BASE + (n) * 0x200000ULL + (reg))
#define LB(reg) (BASE + 0x600000ULL + (reg))
#define NI(reg) (BASE + 0x680000ULL + (reg))
#define MACHINE "-M none -cpu R10000 -device sgi-bedrock,id=bedrock," \
                "processor-count=1 -S"

#define CPU_PROTECT 0x00
#define CPU_OVERRIDE 0x08
#define IO_PROTECT 0x10
#define REGION_PRESENT 0x18
#define CPU_NUMBER 0x20
#define CACHED_ALIAS 0x28
#define CPU_PRESENT_A 0x40
#define CPU_PRESENT_B 0x48
#define CPU_ENABLE_A 0x50
#define CPU_ENABLE_B 0x58
#define PENDING_MODIFY 0x90
#define PENDING_0 0x98
#define PENDING_1 0xa0
#define MASK_0_A 0xa8
#define MASK_1_A 0xb0
#define MASK_0_B 0xb8
#define MASK_1_B 0xc0
#define REMAP 0xf0
#define PENDING_ALIAS 0x180
#define ERROR_SET 0x3f8
#define ERROR_PENDING 0x400
#define ERROR_MASK_A 0x408
#define ERROR_MASK_B 0x410
#define ERROR_STACK_A 0x418
#define ERROR_STACK_SIZE 0x428

static QTestState *start(const char *extra)
{
    QTestState *s = qtest_initf(MACHINE " %s", extra);

    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 0, BASE);
    return s;
}

static void test_identity_storage_and_width(void)
{
    QTestState *s = start("");
    const uint8_t bytes[8] = { 0x01, 0x23, 0x45, 0x67,
                               0x89, 0xab, 0xcd, 0xef };
    uint8_t actual[8];

    g_assert_cmphex(qtest_readq(s, PI(0, CPU_NUMBER)), ==, 0);
    g_assert_cmphex(qtest_readq(s, PI(1, CPU_NUMBER)), ==, 2);
    g_assert_cmphex(qtest_readq(s, PI(0, CPU_PRESENT_A)), ==, 1);
    g_assert_cmphex(qtest_readq(s, PI(0, CPU_PRESENT_B)), ==, 0);
    g_assert_cmphex(qtest_readq(s, PI(1, CPU_PRESENT_A)), ==, 0);

    qtest_writeq(s, PI(0, REGION_PRESENT), 0x55aa55aa55aa55aaULL);
    qtest_writeq(s, PI(0, CACHED_ALIAS), UINT64_MAX);
    qtest_writeq(s, PI(0, CPU_ENABLE_A), UINT64_MAX);
    qtest_writeq(s, PI(0, CPU_ENABLE_B), 1);
    g_assert_cmphex(qtest_readq(s, PI(0, REGION_PRESENT)), ==,
                    0x55aa55aa55aa55aaULL);
    g_assert_cmphex(qtest_readq(s, PI(0, CACHED_ALIAS)), ==, 0xf);
    g_assert_cmphex(qtest_readq(s, PI(0, CPU_ENABLE_A)), ==, 1);
    g_assert_cmphex(qtest_readq(s, PI(0, CPU_ENABLE_B)), ==, 1);

    qtest_memwrite(s, PI(0, CPU_PROTECT), bytes, sizeof(bytes));
    g_assert_cmphex(qtest_readq(s, PI(0, CPU_PROTECT)), ==,
                    0x0123456789abcdefULL);
    qtest_memread(s, PI(0, CPU_PROTECT), actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), bytes, sizeof(bytes));
    qtest_writeb(s, PI(0, CPU_PROTECT) + 7, 0);
    qtest_writew(s, PI(0, CPU_PROTECT) + 6, 0);
    qtest_writel(s, PI(0, CPU_PROTECT) + 4, 0);
    qtest_writeq(s, PI(0, CPU_PROTECT) + 1, 0);
    g_assert_cmphex(qtest_readq(s, PI(0, CPU_PROTECT)), ==,
                    0x0123456789abcdefULL);

    qtest_writeq(s, PI(0, CPU_PROTECT), 0);
    qtest_writeq(s, PI(0, CPU_OVERRIDE), 0x1234);
    g_assert_cmphex(qtest_readq(s, PI(0, CPU_PROTECT)), ==, 0);
    qtest_writeq(s, PI(0, CPU_OVERRIDE), 0x53474972756c6573ULL);
    g_assert_cmphex(qtest_readq(s, PI(0, CPU_PROTECT)), ==, 1);
    qtest_writeq(s, PI(0, IO_PROTECT), 0x55aa55aa55aa55aaULL);
    g_assert_cmphex(qtest_readq(s, PI(0, IO_PROTECT)), ==,
                    0x55aa55aa55aa55aaULL);
    qtest_quit(s);
}

static void test_pending_modify_and_reflection(void)
{
    QTestState *s = start("");

    qtest_writeq(s, PI(0, PENDING_MODIFY), 0x107);
    g_assert_cmphex(qtest_readq(s, PI(0, PENDING_0)), ==, 1ULL << 7);
    qtest_writeq(s, PI(0, PENDING_MODIFY), 0x007);
    g_assert_cmphex(qtest_readq(s, PI(0, PENDING_0)), ==, 0);
    qtest_writeq(s, PI(0, PENDING_ALIAS), 0x14a);
    g_assert_cmphex(qtest_readq(s, PI(0, PENDING_1)), ==, 1ULL << 10);
    qtest_writeq(s, PI(0, PENDING_ALIAS), 0x04a);
    g_assert_cmphex(qtest_readq(s, PI(0, PENDING_1)), ==, 0);

    qtest_writeq(s, PI(0, PENDING_0), UINT64_MAX);
    qtest_writeq(s, PI(0, PENDING_1), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PI(0, PENDING_0)), ==, 0);
    g_assert_cmphex(qtest_readq(s, PI(0, PENDING_1)), ==, 0);

    qtest_writeq(s, LB(0x4040), 1);
    g_assert_cmphex(qtest_readq(s, PI(0, PENDING_1)), ==, 1ULL << 55);
    g_assert_cmphex(qtest_readq(s, PI(1, PENDING_1)), ==, 1ULL << 55);
    qtest_writeq(s, PI(0, PENDING_MODIFY), 0x177);
    g_assert_cmphex(qtest_readq(s, PI(0, PENDING_1)), ==, 1ULL << 55);
    qtest_writeq(s, LB(0x4048), 1);
    g_assert_cmphex(qtest_readq(s, PI(0, PENDING_1)), ==, 0);
    g_assert_cmphex(qtest_readq(s, PI(1, PENDING_1)), ==, 0);
    qtest_quit(s);
}

static void test_masks_remap_and_isolation(void)
{
    QTestState *s = start("");

    qtest_irq_intercept_out_named(s, BEDROCK_PATH, "hardware-irq");
    qtest_writeq(s, PI(0, PENDING_MODIFY), 0x107);
    qtest_writeq(s, PI(0, MASK_0_A), 1ULL << 7);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_false(qtest_get_irq(s, 5));
    g_assert_false(qtest_get_irq(s, 10));
    qtest_writeq(s, PI(0, MASK_0_A), 0);
    g_assert_false(qtest_get_irq(s, 0));

    qtest_writeq(s, PI(0, MASK_0_B), 1ULL << 7);
    g_assert_true(qtest_get_irq(s, 5));
    qtest_writeq(s, PI(0, MASK_0_B), 0);
    qtest_writeq(s, PI(0, PENDING_MODIFY), 0x007);

    qtest_writeq(s, PI(1, PENDING_MODIFY), 0x107);
    qtest_writeq(s, PI(1, MASK_0_A), 1ULL << 7);
    g_assert_true(qtest_get_irq(s, 10));
    g_assert_false(qtest_get_irq(s, 0));

    qtest_writeq(s, PI(0, PENDING_MODIFY), 0x145);
    qtest_writeq(s, PI(0, MASK_1_A), 1ULL << 5);
    g_assert_true(qtest_get_irq(s, 1));
    qtest_writeq(s, PI(0, REMAP), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PI(0, REMAP)), ==, 0x1f);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_true(qtest_get_irq(s, 1));

    qtest_writeq(s, PI(0, ERROR_SET), (1ULL << 3) | (1ULL << 26));
    g_assert_cmphex(qtest_readq(s, PI(0, ERROR_PENDING)), ==,
                    (1ULL << 3) | (1ULL << 26));
    qtest_writeq(s, PI(0, ERROR_SET), 0);
    g_assert_cmphex(qtest_readq(s, PI(0, ERROR_PENDING)), ==, 0);
    qtest_writeq(s, PI(0, ERROR_SET), 1ULL << 24);
    g_assert_cmphex(qtest_readq(s, PI(0, ERROR_PENDING)), ==, 0);
    qtest_writeq(s, PI(0, ERROR_SET), (1ULL << 3) | (1ULL << 26));
    qtest_writeq(s, PI(0, ERROR_MASK_A), 1ULL << 3);
    qtest_writeq(s, PI(0, ERROR_MASK_B), 1ULL << 26);
    g_assert_true(qtest_get_irq(s, 4));
    g_assert_true(qtest_get_irq(s, 9));
    qtest_writeq(s, PI(0, ERROR_PENDING), 1ULL << 3);
    g_assert_false(qtest_get_irq(s, 4));
    g_assert_true(qtest_get_irq(s, 9));
    qtest_writeq(s, PI(0, ERROR_PENDING), 1ULL << 26);
    g_assert_false(qtest_get_irq(s, 9));
    qtest_quit(s);
}

static void test_local_reset(void)
{
    QTestState *s = start("");

    qtest_writeq(s, PI(0, CPU_PROTECT), 0x1234);
    qtest_writeq(s, PI(0, IO_PROTECT), 0x5678);
    qtest_writeq(s, PI(0, REGION_PRESENT), 0x9abc);
    qtest_writeq(s, PI(0, CACHED_ALIAS), 7);
    qtest_writeq(s, PI(0, CPU_ENABLE_A), 1);
    qtest_writeq(s, PI(0, ERROR_STACK_A), UINT64_MAX);
    qtest_writeq(s, PI(0, ERROR_STACK_SIZE), UINT64_MAX);
    qtest_writeq(s, LB(0x8008), 0xfeed);
    qtest_writeq(s, LB(0x8000), 0xbeef);
    qtest_writeq(s, NI(0x38), 0xa55a);
    qtest_writeq(s, NI(0x08), 4);
    g_assert_cmphex(qtest_readq(s, PI(0, CPU_PROTECT)), ==, 0x1234);
    g_assert_cmphex(qtest_readq(s, PI(0, IO_PROTECT)), ==, 0x5678);
    g_assert_cmphex(qtest_readq(s, PI(0, REGION_PRESENT)), ==, 0x9abc);
    g_assert_cmphex(qtest_readq(s, PI(0, CACHED_ALIAS)), ==, 7);
    g_assert_cmphex(qtest_readq(s, PI(0, CPU_ENABLE_A)), ==, 1);
    g_assert_cmphex(qtest_readq(s, PI(0, ERROR_STACK_A)), ==,
                    0x1fffffff8ULL);
    g_assert_cmphex(qtest_readq(s, PI(0, ERROR_STACK_SIZE)), ==, 0xf);
    g_assert_cmphex(qtest_readq(s, LB(0x8000)), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB(0x8008)), ==, 0xfeed);
    g_assert_cmphex(qtest_readq(s, NI(0x38)), ==, 0);
    qtest_quit(s);
}

static void wait_for_migration(QTestState *from)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(from,
                                             "{'execute':'query-migrate'}");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_str(result, "status");

        if (!strcmp(status, "completed")) {
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void test_migration_recomputes_outputs(void)
{
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sn1-bedrock-pi-migration-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start("");
    to = start("-incoming defer");
    qtest_irq_intercept_out_named(from, BEDROCK_PATH, "hardware-irq");
    qtest_writeq(from, PI(0, PENDING_MODIFY), 0x107);
    qtest_writeq(from, PI(0, MASK_0_A), 1ULL << 7);
    qtest_writeq(from, PI(1, PENDING_MODIFY), 0x168);
    qtest_writeq(from, PI(1, MASK_1_B), 1ULL << 40);
    qtest_writeq(from, LB(0x4040), 1ULL << 3);
    qtest_writeq(from, PI(1, MASK_1_A), 1ULL << 55);
    g_assert_true(qtest_get_irq(from, 0));
    g_assert_true(qtest_get_irq(from, 11));
    g_assert_true(qtest_get_irq(from, 16));
    qtest_irq_intercept_out_named(to, BEDROCK_PATH, "hardware-irq");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    wait_for_migration(from);
    g_assert_cmphex(qtest_readq(to, PI(0, PENDING_0)), ==, 1ULL << 7);
    g_assert_cmphex(qtest_readq(to, PI(1, PENDING_1)), ==,
                    (1ULL << 40) | (1ULL << 55));
    g_assert_true(qtest_get_irq(to, 0));
    g_assert_true(qtest_get_irq(to, 11));
    g_assert_true(qtest_get_irq(to, 16));
    g_assert_false(qtest_get_irq(to, 5));
    qtest_writeq(to, PI(0, MASK_0_A), 0);
    qtest_writeq(to, PI(1, PENDING_MODIFY), 0x068);
    qtest_writeq(to, LB(0x4048), 1ULL << 3);
    g_assert_false(qtest_get_irq(to, 0));
    g_assert_false(qtest_get_irq(to, 11));
    g_assert_false(qtest_get_irq(to, 16));
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-pi/identity-storage-width",
                   test_identity_storage_and_width);
    qtest_add_func("sn1/bedrock-pi/pending-reflection",
                   test_pending_modify_and_reflection);
    qtest_add_func("sn1/bedrock-pi/masks-remap-isolation",
                   test_masks_remap_and_isolation);
    qtest_add_func("sn1/bedrock-pi/local-reset", test_local_reset);
    qtest_add_func("sn1/bedrock-pi/migration-recomputes-outputs",
                   test_migration_recomputes_outputs);
    return g_test_run();
}
