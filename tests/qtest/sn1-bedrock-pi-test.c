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
#define MACHINE "-M none -cpu R10000 -m 256M " \
                "-object memory-backend-ram,id=bedrock-dma,size=2M " \
                "-device sgi-bedrock,id=bedrock,processor-count=1," \
                "dma-memory=/objects/bedrock-dma/bedrock-dma[0] -S"

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
#define RT_COMPARE_A 0x108
#define RT_COMPARE_B 0x110
#define PROFILE_COMPARE 0x118
#define RT_PENDING_A 0x120
#define RT_PENDING_B 0x128
#define PROFILE_PENDING_A 0x130
#define PROFILE_PENDING_B 0x138
#define RT_ENABLE_A 0x140
#define RT_ENABLE_B 0x148
#define PROFILE_ENABLE_A 0x150
#define PROFILE_ENABLE_B 0x158
#define PENDING_ALIAS 0x180
#define ERROR_SET 0x3f8
#define ERROR_PENDING 0x400
#define ERROR_MASK_A 0x408
#define ERROR_MASK_B 0x410
#define ERROR_STACK_A 0x418
#define ERROR_STACK_SIZE 0x428
#define RT_COUNTER 0x30100
#define RT_MASK ((1ULL << 55) - 1)
#define RT_TICK_NS 800

static QTestState *start(const char *extra)
{
    QTestState *s = qtest_initf(MACHINE " %s", extra);

    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 0, BASE);
    return s;
}

static QTestState *start_clock(const char *extra)
{
    QTestState *s = start(extra);

    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    return s;
}

static void ensure_clock_running(QTestState *s)
{
    g_autoptr(QDict) response =
        qtest_qmp(s, "{'execute':'query-status'}");
    QDict *status = qdict_get_qdict(response, "return");

    if (!qdict_get_bool(status, "running")) {
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
    }
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
    /*
     * The remap field retains its bits. Which of bank 1 bits 31:0 a field
     * value redirects to hardware interrupt 0 is not stated, so no line is
     * asserted here until the datasheet records the decode.
     */
    qtest_writeq(s, PI(0, REMAP), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PI(0, REMAP)), ==, 0x1f);
    qtest_writeq(s, PI(0, REMAP), 0);

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

static void test_real_time_comparators(void)
{
    QTestState *s = start_clock("");

    qtest_irq_intercept_out_named(s, BEDROCK_PATH, "hardware-irq");
    qtest_clock_set(s, 0);
    qtest_clock_step(s, RT_TICK_NS / 2);
    qtest_writeq(s, PI(0, RT_COMPARE_A), 1);
    qtest_writeq(s, PI(0, RT_ENABLE_A), 1);
    qtest_clock_step(s, RT_TICK_NS / 2 - 1);
    g_assert_cmphex(qtest_readq(s, PI(0, RT_PENDING_A)), ==, 0);
    qtest_clock_step(s, 2);
    g_assert_cmphex(qtest_readq(s, PI(0, RT_PENDING_A)), ==, 1);
    g_assert_true(qtest_get_irq(s, 2));
    qtest_writeq(s, PI(0, RT_PENDING_A), 0);

    qtest_writeq(s, PI(0, RT_COUNTER), 0);
    qtest_writeq(s, PI(0, RT_COMPARE_A), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PI(0, RT_COMPARE_A)), ==, RT_MASK);
    qtest_writeq(s, PI(0, RT_COMPARE_A), 3);
    qtest_writeq(s, PI(0, RT_ENABLE_A), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PI(0, RT_ENABLE_A)), ==, 1);
    qtest_clock_step(s, 2 * RT_TICK_NS);
    g_assert_cmphex(qtest_readq(s, PI(0, RT_COUNTER)), ==, 2);
    g_assert_cmphex(qtest_readq(s, PI(0, RT_PENDING_A)), ==, 0);
    g_assert_false(qtest_get_irq(s, 2));
    qtest_clock_step(s, 3 * RT_TICK_NS + 1);
    g_assert_cmphex(qtest_readq(s, PI(0, RT_COUNTER)), ==, 5);
    g_assert_cmphex(qtest_readq(s, PI(0, RT_PENDING_A)), ==, 1);
    g_assert_cmphex(qtest_readq(s, PI(0, RT_PENDING_B)), ==, 0);
    g_assert_true(qtest_get_irq(s, 2));
    qtest_writeq(s, PI(0, RT_PENDING_A), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PI(0, RT_PENDING_A)), ==, 0);
    g_assert_false(qtest_get_irq(s, 2));

    qtest_writeq(s, PI(1, RT_COUNTER), 10);
    qtest_writeq(s, PI(1, RT_COMPARE_B), 12);
    qtest_writeq(s, PI(1, RT_ENABLE_B), 1);
    qtest_clock_step(s, 2 * RT_TICK_NS + 1);
    g_assert_cmphex(qtest_readq(s, PI(1, RT_PENDING_A)), ==, 0);
    g_assert_cmphex(qtest_readq(s, PI(1, RT_PENDING_B)), ==, 1);
    g_assert_true(qtest_get_irq(s, 17));
    g_assert_false(qtest_get_irq(s, 12));
    qtest_writeq(s, PI(1, RT_PENDING_B), 0);
    g_assert_false(qtest_get_irq(s, 17));
    qtest_quit(s);
}

static void test_profile_and_counter_wrap(void)
{
    QTestState *s = start_clock("");

    qtest_irq_intercept_out_named(s, BEDROCK_PATH, "hardware-irq");
    qtest_clock_set(s, 0);
    qtest_writeq(s, PI(0, RT_COUNTER), 0);
    qtest_writeq(s, PI(0, PROFILE_COMPARE), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PI(0, PROFILE_COMPARE)), ==, UINT32_MAX);
    qtest_writeq(s, PI(0, PROFILE_COMPARE), 4);
    qtest_writeq(s, PI(0, PROFILE_ENABLE_A), 1);
    qtest_writeq(s, PI(0, PROFILE_ENABLE_B), 1);
    qtest_clock_step(s, 4 * RT_TICK_NS + 1);
    g_assert_cmphex(qtest_readq(s, PI(0, PROFILE_PENDING_A)), ==, 1);
    g_assert_cmphex(qtest_readq(s, PI(0, PROFILE_PENDING_B)), ==, 1);
    g_assert_true(qtest_get_irq(s, 3));
    g_assert_true(qtest_get_irq(s, 8));
    qtest_writeq(s, PI(0, PROFILE_PENDING_A), 0);
    g_assert_cmphex(qtest_readq(s, PI(0, PROFILE_PENDING_A)), ==, 0);
    g_assert_cmphex(qtest_readq(s, PI(0, PROFILE_PENDING_B)), ==, 1);
    g_assert_false(qtest_get_irq(s, 3));
    g_assert_true(qtest_get_irq(s, 8));
    qtest_writeq(s, PI(0, PROFILE_PENDING_B), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, PI(0, PROFILE_PENDING_B)), ==, 0);
    g_assert_false(qtest_get_irq(s, 8));

    qtest_writeq(s, PI(1, RT_COUNTER), 20);
    qtest_writeq(s, PI(1, PROFILE_COMPARE), 22);
    qtest_writeq(s, PI(1, PROFILE_ENABLE_A), 1);
    qtest_clock_step(s, 2 * RT_TICK_NS + 1);
    g_assert_cmphex(qtest_readq(s, PI(1, PROFILE_PENDING_A)), ==, 1);
    g_assert_cmphex(qtest_readq(s, PI(1, PROFILE_PENDING_B)), ==, 1);
    g_assert_true(qtest_get_irq(s, 13));
    g_assert_false(qtest_get_irq(s, 18));
    g_assert_cmphex(qtest_readq(s, PI(0, PROFILE_PENDING_A)), ==, 0);

    qtest_writeq(s, PI(1, RT_COUNTER), RT_MASK - 1);
    qtest_writeq(s, PI(1, RT_COMPARE_A), 1);
    qtest_writeq(s, PI(1, RT_ENABLE_A), 1);
    qtest_clock_step(s, 2 * RT_TICK_NS);
    g_assert_cmphex(qtest_readq(s, PI(1, RT_COUNTER)), ==, 0);
    g_assert_cmphex(qtest_readq(s, PI(1, RT_PENDING_A)), ==, 0);
    qtest_clock_step(s, RT_TICK_NS + 1);
    g_assert_cmphex(qtest_readq(s, PI(1, RT_COUNTER)), ==, 1);
    g_assert_cmphex(qtest_readq(s, PI(1, RT_PENDING_A)), ==, 1);
    g_assert_true(qtest_get_irq(s, 12));
    qtest_quit(s);
}

static void test_timer_reset_model(void)
{
    QTestState *s = start_clock("");

    qtest_clock_set(s, 0);
    qtest_writeq(s, PI(0, RT_COUNTER), 0x1234);
    qtest_writeq(s, PI(0, RT_COMPARE_A), 0x5678);
    qtest_writeq(s, PI(0, PROFILE_COMPARE), 0x9abc);
    qtest_writeq(s, PI(0, RT_ENABLE_A), 1);
    qtest_writeq(s, PI(0, PROFILE_ENABLE_A), 1);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, PI(0, RT_COUNTER)), ==, 0);
    g_assert_cmphex(qtest_readq(s, PI(0, RT_COMPARE_A)), ==, 0);
    g_assert_cmphex(qtest_readq(s, PI(0, PROFILE_COMPARE)), ==, 0);
    g_assert_cmphex(qtest_readq(s, PI(0, RT_ENABLE_A)), ==, 0);
    g_assert_cmphex(qtest_readq(s, PI(0, PROFILE_ENABLE_A)), ==, 0);
    qtest_clock_step(s, (UINT64_C(1) << 32) * RT_TICK_NS + 1);
    g_assert_cmphex(qtest_readq(s, PI(0, PROFILE_PENDING_A)), ==, 1);
    g_assert_cmphex(qtest_readq(s, PI(0, PROFILE_PENDING_B)), ==, 1);
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
    ensure_clock_running(from);
    qtest_irq_intercept_out_named(from, BEDROCK_PATH, "hardware-irq");
    qtest_writeq(from, PI(0, PENDING_MODIFY), 0x107);
    qtest_writeq(from, PI(0, MASK_0_A), 1ULL << 7);
    qtest_writeq(from, PI(1, PENDING_MODIFY), 0x168);
    qtest_writeq(from, PI(1, MASK_1_B), 1ULL << 40);
    qtest_writeq(from, LB(0x4040), 1ULL << 3);
    qtest_writeq(from, PI(1, MASK_1_A), 1ULL << 55);
    qtest_writeq(from, PI(0, RT_COUNTER), 100);
    qtest_writeq(from, PI(0, RT_COMPARE_B), 110);
    qtest_writeq(from, PI(0, RT_ENABLE_B), 1);
    qtest_clock_step(from, 4 * RT_TICK_NS);
    g_assert_true(qtest_get_irq(from, 0));
    g_assert_true(qtest_get_irq(from, 11));
    g_assert_true(qtest_get_irq(from, 16));
    qtest_irq_intercept_out_named(to, BEDROCK_PATH, "hardware-irq");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    wait_for_migration(from);
    ensure_clock_running(to);
    g_assert_cmphex(qtest_readq(to, PI(0, PENDING_0)), ==, 1ULL << 7);
    g_assert_cmphex(qtest_readq(to, PI(1, PENDING_1)), ==,
                    (1ULL << 40) | (1ULL << 55));
    g_assert_true(qtest_get_irq(to, 0));
    g_assert_true(qtest_get_irq(to, 11));
    g_assert_true(qtest_get_irq(to, 16));
    g_assert_false(qtest_get_irq(to, 5));
    g_assert_cmphex(qtest_readq(to, PI(0, RT_PENDING_B)), ==, 0);
    qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readq(to, PI(0, RT_PENDING_B)), ==, 0);
    qtest_clock_step_next(to);
    g_assert_cmphex(qtest_readq(to, PI(0, RT_PENDING_B)), ==, 1);
    g_assert_true(qtest_get_irq(to, 7));
    qtest_writeq(to, PI(0, RT_PENDING_B), 0);
    g_assert_false(qtest_get_irq(to, 7));
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

static void test_migration_preserves_counter_phase(void)
{
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sn1-bedrock-pi-phase-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start_clock("");
    to = start("-incoming defer");
    qtest_clock_set(from, 0);
    qtest_writeq(from, PI(0, RT_COUNTER), 0);
    qtest_writeq(from, PI(0, RT_COMPARE_A), 1);
    qtest_writeq(from, PI(0, RT_ENABLE_A), 1);
    qtest_clock_step(from, RT_TICK_NS / 2);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    wait_for_migration(from);
    ensure_clock_running(to);
    qtest_irq_intercept_out_named(to, BEDROCK_PATH, "hardware-irq");
    g_assert_cmphex(qtest_readq(to, PI(0, RT_COUNTER)), ==, 0);
    qtest_clock_step(to, RT_TICK_NS / 2 - 1);
    g_assert_cmphex(qtest_readq(to, PI(0, RT_COUNTER)), ==, 0);
    g_assert_cmphex(qtest_readq(to, PI(0, RT_PENDING_A)), ==, 0);
    qtest_clock_step(to, 2);
    g_assert_cmphex(qtest_readq(to, PI(0, RT_COUNTER)), ==, 1);
    g_assert_cmphex(qtest_readq(to, PI(0, RT_PENDING_A)), ==, 1);
    g_assert_true(qtest_get_irq(to, 2));
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
    qtest_add_func("sn1/bedrock-pi/real-time-comparators",
                   test_real_time_comparators);
    qtest_add_func("sn1/bedrock-pi/profile-counter-wrap",
                   test_profile_and_counter_wrap);
    qtest_add_func("sn1/bedrock-pi/timer-reset-model",
                   test_timer_reset_model);
    qtest_add_func("sn1/bedrock-pi/migration-recomputes-outputs",
                   test_migration_recomputes_outputs);
    qtest_add_func("sn1/bedrock-pi/migration-counter-phase",
                   test_migration_preserves_counter_phase);
    return g_test_run();
}
