/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Bedrock internal-crossbar MMIO on an empty machine; no Origin 300 model.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"

#define XB_PATH "/machine/peripheral/xb"
#define XB_BASE 0x11000000ULL
#define MACHINE "-M none -cpu R10000 -device sgi-bedrock-xb,id=xb -S"

#define PARAMETERS           0x000
#define SLOW_GRANT           0x008
#define SPEW_CONTROL         0x010
#define ARBITRATION_TRIGGER  0x018
#define QUEUE_ERROR(n)       (0x020 + (n) * 8)
#define FIRST_ERROR          0x090
#define DEBUG_DATA           0x098
#define DEBUG_ARBITRATION    0x0a0
#define QUEUE_CLEAR(n)       (0x120 + (n) * 8)
#define FIRST_ERROR_CLEAR    0x190

#define PI0_OUTPUT      0
#define PI0_INPUT       1
#define PI1_OUTPUT      2
#define PI1_INPUT       3
#define MD_TO_PI0       4
#define MD_TO_PI1       5
#define MD_TO_LB_NI_II  6
#define MD_INPUT        7
#define NI_OUTPUT       8
#define NI_INPUT        9
#define II_OUTPUT       10
#define II_INPUT        11
#define LB_OUTPUT       12
#define LB_INPUT        13
#define QUEUES          14

#define PARAMETERS_RESET  0x0008000080000021ULL
#define PARAMETERS_ACTIVE 0x00080000fffff021ULL
#define PARAMETERS_MASK   0x000ffffffffffff1ULL

#define FIRST_ERROR_VALID (1ULL << 12)
#define FIRST_ERROR_PI0_OUTPUT (FIRST_ERROR_VALID | (0xfULL << 8))
#define FIRST_ERROR_MD_INPUT   (FIRST_ERROR_VALID | (0x8ULL << 8))
#define FIRST_ERROR_LB_INPUT   (FIRST_ERROR_VALID | (0x2ULL << 8))

static const struct {
    unsigned queue;
    uint64_t mask;
} structured[] = {
    { PI0_OUTPUT, 0x3333 },
    { PI0_INPUT, 0x0033 },
    { MD_TO_PI0, 0x3330 },
    { MD_INPUT, 0x00f0 },
    { NI_OUTPUT, 0xfff0 },
    { LB_OUTPUT, 0x3303 },
    { LB_INPUT, 0x0011 },
};

static const unsigned structureless[] = {
    PI1_OUTPUT, PI1_INPUT, MD_TO_PI1, MD_TO_LB_NI_II, NI_INPUT, II_OUTPUT,
    II_INPUT,
};

static QTestState *xb_start(const char *extra)
{
    QTestState *s = qtest_initf(MACHINE " %s", extra);

    qtest_sysbus_mmio_map(s, XB_PATH, 0, XB_BASE);
    return s;
}

static void test_control_storage(void)
{
    QTestState *s = xb_start("");

    g_assert_cmphex(qtest_readq(s, XB_BASE + PARAMETERS), ==,
                    PARAMETERS_RESET);
    qtest_writeq(s, XB_BASE + PARAMETERS, PARAMETERS_ACTIVE);
    g_assert_cmphex(qtest_readq(s, XB_BASE + PARAMETERS), ==,
                    PARAMETERS_ACTIVE);
    qtest_writeq(s, XB_BASE + PARAMETERS, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, XB_BASE + PARAMETERS), ==,
                    PARAMETERS_MASK);

    g_assert_cmphex(qtest_readq(s, XB_BASE + SLOW_GRANT), ==, 0);
    qtest_writeq(s, XB_BASE + SLOW_GRANT, 0xfedcba9876543210ULL);
    g_assert_cmphex(qtest_readq(s, XB_BASE + SLOW_GRANT), ==, 0x76543210);
    qtest_writeq(s, XB_BASE + SLOW_GRANT, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, XB_BASE + SLOW_GRANT), ==, 0xffffffffULL);

    g_assert_cmphex(qtest_readq(s, XB_BASE + SPEW_CONTROL), ==, 0);
    qtest_writeq(s, XB_BASE + SPEW_CONTROL, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, XB_BASE + SPEW_CONTROL), ==, 0x7ff);
    g_assert_cmphex(qtest_readq(s, XB_BASE + ARBITRATION_TRIGGER), ==, 0);
    qtest_writeq(s, XB_BASE + ARBITRATION_TRIGGER, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, XB_BASE + ARBITRATION_TRIGGER), ==, 0xf);
    g_assert_cmphex(qtest_readq(s, XB_BASE + DEBUG_DATA), ==, 0);
    qtest_writeq(s, XB_BASE + DEBUG_DATA, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, XB_BASE + DEBUG_DATA), ==, 0xffff);
    g_assert_cmphex(qtest_readq(s, XB_BASE + DEBUG_ARBITRATION), ==, 0);
    qtest_writeq(s, XB_BASE + DEBUG_ARBITRATION, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, XB_BASE + DEBUG_ARBITRATION), ==, 0x7);
    qtest_quit(s);
}

static void test_pi0_output_injection(void)
{
    QTestState *s = xb_start("");

    qtest_irq_intercept_out_named(s, XB_PATH, "error");
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==, 0);
    qtest_writeq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT), 1);
    g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT)), ==, 1);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(s, 0));

    qtest_writeq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT), 1ULL << 12);
    g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT)), ==,
                    (1ULL << 12) | 1);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    qtest_writeq(s, XB_BASE + QUEUE_ERROR(LB_INPUT), 1ULL << 4);
    g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(LB_INPUT)), ==,
                    1ULL << 4);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);

    qtest_writeq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT), ~0x3333ULL);
    g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT)), ==,
                    (1ULL << 12) | 1);

    qtest_writeq(s, XB_BASE + FIRST_ERROR, 0);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    qtest_writeq(s, XB_BASE + FIRST_ERROR, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_quit(s);
}

static void test_undocumented_bits(void)
{
    QTestState *s = xb_start("");

    qtest_irq_intercept_out_named(s, XB_PATH, "error");
    for (unsigned i = 0; i < G_N_ELEMENTS(structured); i++) {
        uint64_t addr = XB_BASE + QUEUE_ERROR(structured[i].queue);

        qtest_writeq(s, addr, ~structured[i].mask);
        g_assert_cmphex(qtest_readq(s, addr), ==, 0);
    }
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));

    for (unsigned i = 0; i < G_N_ELEMENTS(structured); i++) {
        uint64_t addr = XB_BASE + QUEUE_ERROR(structured[i].queue);

        qtest_writeq(s, addr, UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, addr), ==, structured[i].mask);
    }
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_quit(s);
}

static void test_service_order(void)
{
    QTestState *s = xb_start("");

    qtest_irq_intercept_out_named(s, XB_PATH, "error");
    qtest_writeq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT), 1);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(s, 0));

    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_PI0_OUTPUT);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT)), ==, 1);
    g_assert_true(qtest_get_irq(s, 0));

    qtest_writeq(s, XB_BASE + QUEUE_CLEAR(PI0_OUTPUT), 1);
    g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT)), ==, 0);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_PI0_OUTPUT);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));

    qtest_writeq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT), 1);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, XB_BASE + QUEUE_CLEAR(PI0_OUTPUT), 1);
    qtest_writeq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT), 1ULL << 4);
    g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT)), ==,
                    1ULL << 4);
    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_PI0_OUTPUT);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, XB_BASE + QUEUE_CLEAR(PI0_OUTPUT), 1ULL << 4);
    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_PI0_OUTPUT);
    g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT)), ==, 0);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_quit(s);
}

static void test_premature_clear_retains_record(void)
{
    QTestState *s = xb_start("");

    qtest_irq_intercept_out_named(s, XB_PATH, "error");
    /*
     * A record on a higher queue must survive a premature clear even when a
     * lower queue is populated: the record is retained, not relatched from
     * the lowest populated queue.
     */
    qtest_writeq(s, XB_BASE + QUEUE_ERROR(LB_INPUT), 1);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_LB_INPUT);
    qtest_writeq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT), 1);
    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_LB_INPUT);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_LB_INPUT);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, XB_BASE + QUEUE_CLEAR(LB_INPUT), 1);
    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_LB_INPUT);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, XB_BASE + QUEUE_CLEAR(PI0_OUTPUT), 1);
    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_PI0_OUTPUT);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_quit(s);
}

static void test_relatch(void)
{
    QTestState *s = xb_start("");

    qtest_irq_intercept_out_named(s, XB_PATH, "error");
    qtest_writeq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT), 1);
    qtest_writeq(s, XB_BASE + QUEUE_ERROR(LB_INPUT), 1);
    qtest_writeq(s, XB_BASE + QUEUE_ERROR(MD_INPUT), 1ULL << 4);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);

    qtest_writeq(s, XB_BASE + QUEUE_CLEAR(PI0_OUTPUT), 1);
    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_PI0_OUTPUT);
    /*
     * PI0 output is 0xf and LB input is 0x2. MD input at 0x8 follows their
     * address order; its value is unconfirmed.
     */
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_MD_INPUT);
    g_assert_true(qtest_get_irq(s, 0));

    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_MD_INPUT);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_MD_INPUT);
    g_assert_true(qtest_get_irq(s, 0));

    qtest_writeq(s, XB_BASE + QUEUE_CLEAR(MD_INPUT), 1ULL << 4);
    g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(MD_INPUT)), ==, 0);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_MD_INPUT);
    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_MD_INPUT);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_LB_INPUT);
    g_assert_true(qtest_get_irq(s, 0));

    qtest_writeq(s, XB_BASE + QUEUE_CLEAR(LB_INPUT), 1);
    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_LB_INPUT);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_quit(s);
}

static void test_probe_replay(void)
{
    g_autofree char *path = g_strdup_printf("%s/sn1-xb-probe-XXXXXX.log",
                                            g_get_tmp_dir());
    g_autofree char *extra = NULL;
    char *found = NULL;
    QTestState *s;
    int fd = g_mkstemp(path);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = g_strdup_printf("-d unimp -D %s", path);
    s = xb_start(extra);
    qtest_irq_intercept_out_named(s, XB_PATH, "error");
    for (unsigned n = 0; n < QUEUES; n++) {
        qtest_writeq(s, XB_BASE + QUEUE_CLEAR(n), 0);
    }
    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, 0);
    /*
     * The replay is expected consumer traffic, not an unimplemented access.
     */
    g_assert_true(g_file_get_contents(path, &found, NULL, NULL));
    g_assert_null(strstr(found, "sgi-bedrock-xb"));
    g_free(found);

    for (unsigned n = 0; n < QUEUES; n++) {
        g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(n)), ==, 0);
    }
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, XB_BASE + PARAMETERS), ==,
                    PARAMETERS_RESET);
    /* Reads of unmodeled queues remain logged. */
    g_assert_true(g_file_get_contents(path, &found, NULL, NULL));
    g_assert_nonnull(strstr(found, "sgi-bedrock-xb"));
    g_free(found);

    /* Model policy: any value written to a clear register clears. */
    qtest_writeq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT), 1);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, XB_BASE + QUEUE_CLEAR(PI0_OUTPUT), 0);
    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, 0);
    g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT)), ==, 0);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_quit(s);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_structureless_queues(void)
{
    QTestState *s = xb_start("");

    qtest_irq_intercept_out_named(s, XB_PATH, "error");
    for (unsigned i = 0; i < G_N_ELEMENTS(structureless); i++) {
        uint64_t addr = XB_BASE + QUEUE_ERROR(structureless[i]);

        qtest_writeq(s, addr, UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, addr), ==, 0);
        qtest_writeq(s, XB_BASE + QUEUE_CLEAR(structureless[i]), UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, addr), ==, 0);
    }
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));

    qtest_writeq(s, XB_BASE + QUEUE_ERROR(LB_INPUT), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(LB_INPUT)), ==,
                    0x11);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_LB_INPUT);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_quit(s);
}

static void test_clear_readback_and_holes(void)
{
    QTestState *s = xb_start("");

    qtest_irq_intercept_out_named(s, XB_PATH, "error");
    qtest_writeq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT), 1);
    for (unsigned n = 0; n < QUEUES; n++) {
        g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_CLEAR(n)), ==, 0);
    }
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR_CLEAR), ==, 0);
    g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT)), ==, 1);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(s, 0));

    for (uint64_t hole = DEBUG_ARBITRATION + 8; hole < QUEUE_CLEAR(0);
         hole += 8) {
        g_assert_cmphex(qtest_readq(s, XB_BASE + hole), ==, 0);
        qtest_writeq(s, XB_BASE + hole, UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, XB_BASE + hole), ==, 0);
    }
    g_assert_cmphex(qtest_readq(s, XB_BASE + PARAMETERS), ==,
                    PARAMETERS_RESET);
    g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT)), ==, 1);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_quit(s);
}

static void test_byte_order_and_width(void)
{
    QTestState *s = xb_start("");
    const uint8_t bytes[8] = { 0, 1, 0x23, 0x45, 0x67, 0x89, 0xab, 0xc1 };
    uint8_t actual[8];
    uint64_t addr = XB_BASE + PARAMETERS;
    uint64_t pi0 = XB_BASE + QUEUE_ERROR(PI0_OUTPUT);

    qtest_irq_intercept_out_named(s, XB_PATH, "error");
    qtest_memwrite(s, addr, bytes, sizeof(bytes));
    g_assert_cmphex(qtest_readq(s, addr), ==, 0x000123456789abc1ULL);
    qtest_memread(s, addr, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), bytes, sizeof(bytes));
    qtest_writeb(s, addr + 7, 0);
    qtest_writew(s, addr + 6, 0);
    qtest_writel(s, addr + 4, 0);
    qtest_writeq(s, addr + 1, 0);
    g_assert_cmphex(qtest_readq(s, addr), ==, 0x000123456789abc1ULL);

    qtest_readl(s, pi0 + 4);
    qtest_writel(s, pi0 + 4, 1);
    qtest_writeb(s, pi0 + 7, 1);
    g_assert_cmphex(qtest_readq(s, pi0), ==, 0);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));

    qtest_writeq(s, pi0, 1);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writel(s, XB_BASE + QUEUE_CLEAR(PI0_OUTPUT) + 4, 1);
    g_assert_cmphex(qtest_readq(s, pi0), ==, 1);
    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_PI0_OUTPUT);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    qtest_writeq(s, XB_BASE + QUEUE_CLEAR(PI0_OUTPUT), 1);
    qtest_writel(s, XB_BASE + FIRST_ERROR_CLEAR + 4, 0);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_quit(s);
}

static void test_cold_reset(void)
{
    QTestState *s = xb_start("");

    qtest_irq_intercept_out_named(s, XB_PATH, "error");
    qtest_writeq(s, XB_BASE + PARAMETERS, PARAMETERS_ACTIVE);
    qtest_writeq(s, XB_BASE + SLOW_GRANT, 0x12345678);
    qtest_writeq(s, XB_BASE + SPEW_CONTROL, 0x400);
    qtest_writeq(s, XB_BASE + ARBITRATION_TRIGGER, 0x9);
    qtest_writeq(s, XB_BASE + DEBUG_DATA, 0x8421);
    qtest_writeq(s, XB_BASE + DEBUG_ARBITRATION, 0x5);
    qtest_writeq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT), 1);
    qtest_writeq(s, XB_BASE + QUEUE_ERROR(LB_INPUT), 1ULL << 4);
    g_assert_true(qtest_get_irq(s, 0));

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, XB_BASE + PARAMETERS), ==,
                    PARAMETERS_RESET);
    g_assert_cmphex(qtest_readq(s, XB_BASE + SLOW_GRANT), ==, 0);
    g_assert_cmphex(qtest_readq(s, XB_BASE + SPEW_CONTROL), ==, 0);
    g_assert_cmphex(qtest_readq(s, XB_BASE + ARBITRATION_TRIGGER), ==, 0);
    g_assert_cmphex(qtest_readq(s, XB_BASE + DEBUG_DATA), ==, 0);
    g_assert_cmphex(qtest_readq(s, XB_BASE + DEBUG_ARBITRATION), ==, 0);
    for (unsigned n = 0; n < QUEUES; n++) {
        g_assert_cmphex(qtest_readq(s, XB_BASE + QUEUE_ERROR(n)), ==, 0);
    }
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));

    qtest_writeq(s, XB_BASE + QUEUE_ERROR(PI0_OUTPUT), 1);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, XB_BASE + QUEUE_CLEAR(PI0_OUTPUT), 1);
    qtest_writeq(s, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_PI0_OUTPUT);
    g_assert_cmphex(qtest_readq(s, XB_BASE + FIRST_ERROR), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_quit(s);
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-xb-migration-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = xb_start("");
    to = xb_start("-incoming defer");
    qtest_writeq(from, XB_BASE + PARAMETERS, PARAMETERS_ACTIVE);
    qtest_writeq(from, XB_BASE + SLOW_GRANT, 0x89abcdef);
    qtest_writeq(from, XB_BASE + SPEW_CONTROL, 0x7ff);
    qtest_writeq(from, XB_BASE + ARBITRATION_TRIGGER, 0xa);
    qtest_writeq(from, XB_BASE + DEBUG_DATA, 0xbeef);
    qtest_writeq(from, XB_BASE + DEBUG_ARBITRATION, 0x6);
    qtest_writeq(from, XB_BASE + QUEUE_ERROR(PI0_OUTPUT), 1);
    qtest_writeq(from, XB_BASE + QUEUE_ERROR(LB_INPUT), 1ULL << 4);
    qtest_irq_intercept_out_named(to, XB_PATH, "error");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    /* The source completes when its state is sent, not when it is loaded. */
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, XB_BASE + PARAMETERS), ==,
                    PARAMETERS_ACTIVE);
    g_assert_cmphex(qtest_readq(to, XB_BASE + SLOW_GRANT), ==, 0x89abcdef);
    g_assert_cmphex(qtest_readq(to, XB_BASE + SPEW_CONTROL), ==, 0x7ff);
    g_assert_cmphex(qtest_readq(to, XB_BASE + ARBITRATION_TRIGGER), ==, 0xa);
    g_assert_cmphex(qtest_readq(to, XB_BASE + DEBUG_DATA), ==, 0xbeef);
    g_assert_cmphex(qtest_readq(to, XB_BASE + DEBUG_ARBITRATION), ==, 0x6);
    g_assert_cmphex(qtest_readq(to, XB_BASE + QUEUE_ERROR(PI0_OUTPUT)), ==,
                    1);
    g_assert_cmphex(qtest_readq(to, XB_BASE + QUEUE_ERROR(LB_INPUT)), ==,
                    1ULL << 4);
    g_assert_cmphex(qtest_readq(to, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(to, 0));

    qtest_writeq(to, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_PI0_OUTPUT);
    g_assert_cmphex(qtest_readq(to, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_PI0_OUTPUT);
    g_assert_true(qtest_get_irq(to, 0));
    qtest_writeq(to, XB_BASE + QUEUE_CLEAR(PI0_OUTPUT), 1);
    qtest_writeq(to, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_PI0_OUTPUT);
    g_assert_cmphex(qtest_readq(to, XB_BASE + FIRST_ERROR), ==,
                    FIRST_ERROR_LB_INPUT);
    g_assert_true(qtest_get_irq(to, 0));
    qtest_writeq(to, XB_BASE + QUEUE_CLEAR(LB_INPUT), 1ULL << 4);
    qtest_writeq(to, XB_BASE + FIRST_ERROR_CLEAR, FIRST_ERROR_LB_INPUT);
    g_assert_cmphex(qtest_readq(to, XB_BASE + FIRST_ERROR), ==, 0);
    g_assert_false(qtest_get_irq(to, 0));
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-xb/control-storage", test_control_storage);
    qtest_add_func("sn1/bedrock-xb/pi0-output-injection",
                   test_pi0_output_injection);
    qtest_add_func("sn1/bedrock-xb/undocumented-bits", test_undocumented_bits);
    qtest_add_func("sn1/bedrock-xb/service-order", test_service_order);
    qtest_add_func("sn1/bedrock-xb/premature-clear",
                   test_premature_clear_retains_record);
    qtest_add_func("sn1/bedrock-xb/relatch", test_relatch);
    qtest_add_func("sn1/bedrock-xb/probe-replay", test_probe_replay);
    qtest_add_func("sn1/bedrock-xb/structureless-queues",
                   test_structureless_queues);
    qtest_add_func("sn1/bedrock-xb/clear-readback-holes",
                   test_clear_readback_and_holes);
    qtest_add_func("sn1/bedrock-xb/byte-order-width",
                   test_byte_order_and_width);
    qtest_add_func("sn1/bedrock-xb/cold-reset", test_cold_reset);
    qtest_add_func("sn1/bedrock-xb/migration", test_migration);
    return g_test_run();
}
