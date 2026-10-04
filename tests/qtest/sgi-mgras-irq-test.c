/*
 * MGRAS register-contract and protocol qtests: interrupt and retrace cases.
 *
 * Unsupported behavior remains an approximation unless the public MGRAS
 * contracts specify it.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include <math.h>
#include <sys/wait.h>
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-migrate.h"
#include "sgi-test.h"
#include "sn0-user-image.h"
#include "sgi-mgras-test.h"
#include "sgi-wait.h"


#define MGRAS_PENDING_ERROR         0x00000002
#define MGRAS_PENDING_SWAP          0x00000400
#define REG_IRQ_ENABLE_CLEAR        MGRAS_REG(0x011048)

static void test_pending_mask_and_reassert(void)
{
    mgras_reset();

    qtest_writel(qts, REG_IRQ_MASK, 0);
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_FLAGS, ==,
                     MGRAS_PENDING_FLAGS);


    qtest_writel(qts, REG_IRQ_MASK, MGRAS_PENDING_FLAGS);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_FLAGS, ==,
                     MGRAS_PENDING_FLAGS);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_FLAGS, ==, 0);
}

/*
 * Two of these keep a machine of their own. They assert that nothing has
 * been sent yet before arming a vector, and the model records the last
 * interrupt it sent in x-test-last-interrupt-addr and -value, which its
 * reset does not clear. Those two fields are diagnostic recordings, not
 * part of the register face, so a shared machine would carry an earlier
 * case's interrupt into this precondition.
 */
static void test_general_interrupt_vector(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);
    const unsigned int general_vector = 51;
    const unsigned int error_vector = 50;
    const uint64_t expected_addr =
        ((uint64_t)HQ_WIDGET << 48) | HQ_REG_OFFSET;

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, 0);

    qtest_writel(qts, REG_INT_DEST_UPPER,
                 (HUB_WIDGET << 16) | (error_vector << 24));
    qtest_writel(qts, REG_INT_DEST_LOWER, HUB_REG_OFFSET);

    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, HQ_WIDGET << 24);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, HQ_REG_OFFSET);
    qtest_writel(qts, REG_HQ_VECTORS, general_vector << 8);

    qtest_writel(qts, REG_IRQ_MASK, MGRAS_PENDING_FLAGS);
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);


    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, general_vector | 0x100);


    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, general_vector);

    qtest_quit(qts);
}

static uint64_t mgras_hub_read(uint64_t offset)
{
    uint8_t bytes[8];

    g_assert_true(qtest_memread_cpu(qts, 0, HUB_BASE + offset, bytes,
                                    sizeof(bytes)));
    return ldq_be_p(bytes);
}

static void mgras_hub_write(uint64_t offset, uint64_t value)
{
    uint8_t bytes[8];

    stq_be_p(bytes, value);
    g_assert_true(qtest_memwrite_cpu(qts, 0, HUB_BASE + offset, bytes,
                                     sizeof(bytes)));
}

static void mgras_retrace_to_hub(unsigned int retrace_vector)
{
    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, KERNEL_HQ_DEST_UPPER);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, KERNEL_HQ_DEST_LOWER);
    qtest_writel(qts, REG_HQ_VECTORS, retrace_vector << 16);
    qtest_writel(qts, REG_IRQ_MASK, KERNEL_IRQ_MASK);
}

static void test_retrace_interrupt_vector(void)
{
    const unsigned int retrace_vector = 54;
    const uint64_t bit = UINT64_C(1) << retrace_vector;

    mgras_reset();
    mgras_retrace_to_hub(retrace_vector);
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);

    qtest_clock_step(qts, mgras_retrace_gap(&mgras_stock_frame, 1));
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, bit);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, retrace_vector | 0x100);

    /* The kernel clears the Hub bit; the next retrace must set it again. */
    mgras_hub_write(HUB_PI_INT_MODIFY, retrace_vector);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);
    qtest_clock_step(qts, mgras_retrace_gap(&mgras_stock_frame, 2));
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, bit);

    /* Clearing the VC3 enable stops further retrace interrupts. */
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL, VC3_CONTROL_DISPLAY));
    mgras_hub_write(HUB_PI_INT_MODIFY, retrace_vector);
    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 30);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);
}

/* The 0x011040 enable word does not stand in for the VC3 enable. */
static void test_retrace_interrupt_needs_vc3_enable(void)
{
    const unsigned int retrace_vector = 54;
    const uint64_t bit = UINT64_C(1) << retrace_vector;

    mgras_reset();
    mgras_retrace_to_hub(retrace_vector);
    qtest_writel(qts, REG_IRQ_MASK, 0xffffffff);
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL, VC3_CONTROL_DISPLAY));

    qtest_clock_step(qts, NANOSECONDS_PER_SECOND / 30);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);
}

/*
 * Before the guest loads a timing table the model runs the stock one, the
 * first retrace one period after reset. A hundred frames on, the phase is
 * still where the exact period puts it, so it does not drift by a rounded
 * period per frame. Enabling the display again part-way through a frame,
 * with nothing that changes the period, keeps the phase: the kernel
 * rewrites VC3 register 0x1e to stop and restart timing and to enable the
 * retrace interrupt.
 */
static void test_retrace_period_and_phase(void)
{
    const unsigned int retrace_vector = 54;
    const MgrasFrameTime *t = &mgras_stock_frame;
    int64_t half = mgras_retrace_gap(t, 106) / 2;

    mgras_reset();
    mgras_retrace_to_hub_on(qts, retrace_vector);
    mgras_expect_retraces(qts, t, 1, 4, retrace_vector);
    qtest_clock_step(qts, mgras_retrace_at(t, 104) - mgras_retrace_at(t, 4));
    mgras_expect_retraces(qts, t, 105, 105, retrace_vector);

    qtest_clock_step(qts, half);
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    mgras_expect_retrace_in(qts, mgras_retrace_gap(t, 106) - half,
                            retrace_vector);
    mgras_expect_retraces(qts, t, 107, 107, retrace_vector);
}
/* The same table on the stock PLL bytes: 107.25 MHz, VC3 at 53.625 MHz. */
static const MgrasFrameTime mgras_vga_stock_pll_frame = { 525 * 400,
                                                          53625000 };

/*
 * The period follows the timing the guest programs: the VC3 frame and
 * line tables give the frame length in VC3 clocks, the DAC PLL the pixel
 * clock, and enabling the display takes both. A new period restarts the
 * phase at that write. A table that gives no frame, and reset, return to
 * the stock period.
 */
static void test_retrace_period_follows_vc3_tables(void)
{
    const unsigned int retrace_vector = 54;

    mgras_reset();
    mgras_load_vga_timing(qts);
    mgras_retrace_to_hub_on(qts, retrace_vector);
    mgras_expect_retraces(qts, &mgras_vga_frame, 1, 3, retrace_vector);

    mgras_dac_reg_write(qts, 0x0c, 2);
    mgras_dac_reg_write(qts, 0x0f, 4);
    mgras_dac_reg_write(qts, 0x09, 0x91);
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    mgras_expect_retraces(qts, &mgras_vga_stock_pll_frame, 1, 3,
                          retrace_vector);

    /* Word 0x1000 is still zero: an empty frame table. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x00, 0x1000));
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    mgras_expect_retraces(qts, &mgras_stock_frame, 1, 2, retrace_vector);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x00, 0x0380));
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    mgras_expect_retraces(qts, &mgras_vga_stock_pll_frame, 1, 1,
                          retrace_vector);
    mgras_reset();
    mgras_retrace_to_hub_on(qts, retrace_vector);
    mgras_expect_retraces(qts, &mgras_stock_frame, 1, 2, retrace_vector);
}

/*
 * The stream carries the retrace phase as the next retrace's time. The
 * destination's next retrace falls where the source's would have, the
 * ones after it one period apart, and none is made up for the frames the
 * destination's own clock never ran. Whether qtest's virtual clock travels
 * with the stream does not matter: the wait is taken from both clocks.
 */
/*
 * A table whose frame is shorter than the scanout's shortest retrace
 * period, 1 ms, runs the stock period, as a table with no frame does,
 * rather than stopping the retrace. One line of 10 VC3 clocks is 0.19 us
 * at the stock PLL. Enabling again with the same table keeps the phase.
 */
static void test_retrace_short_frame_runs_stock_period(void)
{
    const unsigned int retrace_vector = 54;
    const MgrasFrameTime *t = &mgras_stock_frame;
    static const uint16_t line[] = { 0x8080 | (10 << 8), 0x0040 };
    static const uint16_t frame[] = { 0x0040, 1, 0x0000, 0 };
    int64_t half = mgras_retrace_gap(t, 3) / 2;

    mgras_reset();
    mgras_load_vga_timing(qts);
    mgras_retrace_to_hub_on(qts, retrace_vector);
    mgras_expect_retraces(qts, &mgras_vga_frame, 1, 1, retrace_vector);

    mgras_vc3_sram_load(qts, 0x0040, line, ARRAY_SIZE(line));
    mgras_vc3_sram_load(qts, 0x0060, frame, ARRAY_SIZE(frame));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x00, 0x0060));
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    mgras_expect_retraces(qts, t, 1, 2, retrace_vector);

    qtest_clock_step(qts, half);
    qtest_writel(qts, REG_VC3_INDEXDATA,
                 VC3_INDEXDATA(VC3_CONTROL,
                               VC3_CONTROL_DISPLAY | VC3_CONTROL_VINTR));
    mgras_expect_retrace_in(qts, mgras_retrace_gap(t, 3) - half,
                            retrace_vector);
}

/*
 * The general line as the IRIX 6.5.30 kernel wires it: the board initialization
 * routine enables 0x3008a200, and the attach routine puts the general vector in
 * bits 15:8 of 0x011070. The Hub's pending bit for that vector shows the line.
 */
static void mgras_general_to_hub(unsigned int general_vector)
{
    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, KERNEL_HQ_DEST_UPPER);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, KERNEL_HQ_DEST_LOWER);
    qtest_writel(qts, REG_HQ_VECTORS, general_vector << 8);
    qtest_writel(qts, REG_IRQ_MASK, KERNEL_IRQ_MASK);
}

/*
 * The board initialization routine writes 0x3008a200 to 0x011040, the
 * swap-scheduling routine adds 0x400 with a plain store, and the general
 * interrupt handler and the context switch's tail remove bits through 0x011048.
 */
static void test_irq_enable_set_and_clear_ports(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_IRQ_MASK), ==, 0);
    qtest_writel(qts, REG_IRQ_MASK, KERNEL_IRQ_MASK);
    qtest_writel(qts, REG_IRQ_MASK, MGRAS_PENDING_SWAP);
    g_assert_cmphex(qtest_readl(qts, REG_IRQ_MASK), ==,
                    KERNEL_IRQ_MASK | MGRAS_PENDING_SWAP);
    qtest_writel(qts, REG_IRQ_ENABLE_CLEAR, MGRAS_PENDING_SWAP);
    g_assert_cmphex(qtest_readl(qts, REG_IRQ_MASK), ==, KERNEL_IRQ_MASK);
    qtest_writel(qts, REG_IRQ_ENABLE_CLEAR, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG_IRQ_MASK), ==, 0);
}

/*
 * The swap-scheduling routine enables source 10 and writes the one word 0x3700;
 * the handler disables the source at 0x011048 and acknowledges 0x401.
 * That the word raises bit 10 is inferred; the model raises it on
 * consuming the word.
 */
static void test_swap_command_raises_interrupt_10(void)
{
    const unsigned int general_vector = 57;
    const uint64_t bit = UINT64_C(1) << general_vector;

    mgras_reset();
    mgras_general_to_hub(general_vector);
    mgras_enter_context(qts);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);

    qtest_writel(qts, REG_IRQ_MASK, MGRAS_PENDING_SWAP);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_SWAP, ==, 0);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_SWAP_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_SWAP, ==,
                    MGRAS_PENDING_SWAP);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, bit);

    /*
     * The general interrupt handler serves the source it finds enabled and
     * pending.
     */
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) &
                    qtest_readl(qts, REG_IRQ_MASK) & MGRAS_PENDING_SWAP, ==,
                    MGRAS_PENDING_SWAP);
    qtest_writel(qts, REG_IRQ_ENABLE_CLEAR, MGRAS_PENDING_SWAP);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_SWAP | 0x1);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_SWAP, ==, 0);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);
}

/* A swap word with source 10 disabled latches the source but stays quiet. */
static void test_swap_command_needs_enable(void)
{
    const unsigned int general_vector = 57;
    const uint64_t bit = UINT64_C(1) << general_vector;

    mgras_reset();
    mgras_general_to_hub(general_vector);
    mgras_enter_context(qts);

    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_SWAP_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_PENDING) & MGRAS_PENDING_SWAP, ==,
                    MGRAS_PENDING_SWAP);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);
}

/*
 * The kernel writes 0x400 to 0x011040 with a plain store before a swap, and
 * nothing in the general interrupt handler, the swap-completion routine or the
 * next context-switch routine writes 0x011040 again before that switch
 * sleeps on source 19. If the store replaced the enable word, the source-19
 * wake would never arrive.
 * Inferred: 0x011040 sets enables and 0x011048 clears them.
 */
static void test_swap_keeps_context_switch_enable(void)
{
    const unsigned int general_vector = 57;
    const uint64_t bit = UINT64_C(1) << general_vector;

    mgras_reset();
    mgras_general_to_hub(general_vector);
    mgras_enter_context(qts);

    qtest_writel(qts, REG_IRQ_MASK, MGRAS_PENDING_SWAP);
    g_assert_cmphex(qtest_readl(qts, REG_IRQ_MASK), ==,
                    KERNEL_IRQ_MASK | MGRAS_PENDING_SWAP);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_SWAP_TOKEN);
    qtest_writel(qts, REG_IRQ_ENABLE_CLEAR, MGRAS_PENDING_SWAP);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_SWAP | 0x1);
    g_assert_cmphex(qtest_readl(qts, REG_IRQ_MASK), ==, KERNEL_IRQ_MASK);

    /* The line is low, so only the selection's source 19 can raise it. */
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, 0);
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    g_assert_cmphex(mgras_hub_read(HUB_PI_INT_PEND0) & bit, ==, bit);
}

/* Its own machine for the same reason as the general-vector case. */
static void test_widget_error_interrupt_vector(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);
    const unsigned int error_vector = 52;
    const unsigned int hq_vector = 53;
    const uint64_t expected_addr =
        ((uint64_t)HUB_WIDGET << 48) | HUB_REG_OFFSET;

    qtest_writel(qts, REG_INT_DEST_UPPER,
                 (HUB_WIDGET << 16) | (error_vector << 24));
    qtest_writel(qts, REG_INT_DEST_LOWER, HUB_REG_OFFSET);

    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, HQ_WIDGET << 24);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, HQ_REG_OFFSET);
    qtest_writel(qts, REG_HQ_VECTORS,
                 (hq_vector << 16) | (hq_vector << 8) | hq_vector);


    qtest_writel(qts, REG_IRQ_MASK, MGRAS_PENDING_ERROR);
    mgras_enter_context(qts);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, 0);


    qtest_writel(qts, MGRAS_REG(0x000010), 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 1);

    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-addr"),
                     ==, expected_addr);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-last-interrupt-value"),
                     ==, error_vector | 0x100);

    qtest_quit(qts);
}

/*
 * Graphics credits. The Hub charges a processor's uncached I/O store into
 * its slot's enabled graphics page on the processor's own path, which a
 * qtest access never takes, so these cases run a processor: it serves a
 * mailbox in node 0's memory holding a count of doubleword stores and the
 * address to make them at, and zeroes the count when done. The page is
 * the one IRIX enables for SI Viz, the widget base plus 0x12_0400, and the
 * stores go to the main command port there. A doubleword of zero before
 * the context entry is selected is opaque traffic with no other effect.
 */
#define CREDIT_HUB(reg)       (SPACE_IO + 0x01800000 + (reg))
#define CREDIT_PAGE(slot)     CREDIT_HUB(0x300 + 0x28 * (slot))
#define CREDIT_COUNT(slot)    CREDIT_HUB(0x308 + 0x28 * (slot))
#define CREDIT_BIAS(slot)     CREDIT_HUB(0x310 + 0x28 * (slot))
#define CREDIT_MAP(n)         CREDIT_HUB(0x400140 + 8 * (n))
#define CREDIT_PAGE_ENABLE    (1ULL << 40)
#define CREDIT_PORT_PAGE      ((uint64_t)SI_WIDGET << 24 | 0x120000)
#define CREDIT_PORT_STORE     (UINT64_C(0x9200000000000000) | \
                               (uint64_t)SI_WIDGET << 24 | 0x120400)
/* Map entry: valid, slot, node, widget (Linux sn0/hubio.h IIO_IGFX_INIT). */
#define CREDIT_ROUTE(slot, widget) \
    ((1ULL << 20) | ((uint64_t)(slot) << 16) | (widget))
/* What IRIX allocates SI Viz on an Origin 2000: 1024 bytes, bias 68. */
#define CREDIT_TOTAL          101
#define CREDIT_BIAS_VALUE     68
#define CREDIT_MAILBOX        0x50000u
#define CREDIT_ENTRY          0x800

static void credit_build_store_server(SN0Image *p)
{
    unsigned int at, loop, store;

    at = sn0_emit_branch(p, SN0_OPC_BEQ, 0, 0);
    sn0_patch_branch(p, at, CREDIT_ENTRY);
    for (unsigned int vec = SN0_VEC_TLB_REFILL; vec <= SN0_VEC_GENERAL;
         vec += 0x80) {
        p->pos = vec;
        sn0_emit_park(p);
    }
    p->pos = CREDIT_ENTRY;
    sn0_emit_li(p, SN0_R_T0, SN0_STATUS_KERNEL);
    sn0_emit_mtc0(p, SN0_R_T0, SN0_CP0_STATUS);
    /* Only processor 0 of node 0 serves; the rest park. */
    sn0_emit_cpu_number(p, SN0_R_T2);
    at = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T2, SN0_R_ZERO);
    sn0_patch_branch(p, at, SN0_VEC_GENERAL);
    sn0_emit_li(p, SN0_R_S0, SN0_KSEG1(CREDIT_MAILBOX));

    loop = p->pos;
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T1, 0);        /* count */
    at = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_T1, SN0_R_ZERO);
    sn0_patch_branch(p, at, loop);
    sn0_emit_i(p, SN0_OPC_LD, SN0_R_S0, SN0_R_T2, 8);        /* address */
    store = p->pos;
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_T2, SN0_R_ZERO, 0);
    sn0_emit_i(p, 25, SN0_R_T1, SN0_R_T1, 0xffff);           /* daddiu -1 */
    at = sn0_emit_branch(p, SN0_OPC_BNE, SN0_R_T1, SN0_R_ZERO);
    sn0_patch_branch(p, at, store);
    sn0_emit_i(p, SN0_OPC_SD, SN0_R_S0, SN0_R_ZERO, 0);      /* done */
    at = sn0_emit_branch(p, SN0_OPC_BEQ, SN0_R_ZERO, SN0_R_ZERO);
    sn0_patch_branch(p, at, loop);
}

/*
 * A stopped Origin 2000 with SI Viz in IO3 and slot A's page enabled at
 * IRIX's attach values, before any processor runs.
 */
static QTestState *credit_start(void)
{
    g_autofree SN0Image *image = g_new0(SN0Image, 1);
    g_autofree char *path = NULL;
    QTestState *s;

    credit_build_store_server(image);
    path = sn0_image_write(image, "sn0-mgras-credit");
    s = qtest_initf("%s -accel tcg,thread=multi -no-reboot -no-shutdown -S "
                    "-bios %s", MACHINE_ARGS, path);
    unlink(path);
    qtest_writeq(s, CREDIT_PAGE(0), CREDIT_PAGE_ENABLE | CREDIT_PORT_PAGE);
    qtest_writeq(s, CREDIT_COUNT(0), CREDIT_TOTAL);
    qtest_writeq(s, CREDIT_BIAS(0), CREDIT_BIAS_VALUE);
    return s;
}

static void credit_run(QTestState *s)
{
    qtest_qmp_assert_success(s, "{ 'execute': 'cont' }");
}

/* Processor 0 makes 'count' doubleword stores at 'addr'. */
static void credit_stores(QTestState *s, uint64_t addr, uint64_t count)
{
    SgiWait w;

    qtest_writeq(s, CREDIT_MAILBOX + 8, addr);
    qtest_writeq(s, CREDIT_MAILBOX, count);
    sgi_wait_start(&w, "guest credit store");
    while (qtest_readq(s, CREDIT_MAILBOX)) {
        sgi_wait_pause(&w, 1000);
    }
}

static uint64_t credit_count(QTestState *s, unsigned int slot)
{
    return qtest_readq(s, CREDIT_COUNT(slot));
}

/*
 * The machine links the board to the Hub of the node that controls IO3,
 * and a burst of command-port stores twice the allocation leaves the
 * count at the allocation: each store's credit comes back before the
 * next is charged. Without the return the count would drain through the
 * bias and wrap.
 */
static void test_credit_burst_returns_to_allocation(void)
{
    QTestState *s = credit_start();
    g_autoptr(QDict) link = qtest_qmp(s,
        "{ 'execute': 'qom-get', 'arguments': {"
        "  'path': %s, 'property': 'pi' } }", MGRAS_QOM_PATH);

    g_assert(qdict_haskey(link, "return"));
    g_assert_cmpstr(qdict_get_str(link, "return"), ==, "/machine/node0/hub");

    qtest_writeq(s, CREDIT_MAP(0), CREDIT_ROUTE(0, SI_WIDGET));
    credit_run(s);
    credit_stores(s, CREDIT_PORT_STORE, 2 * CREDIT_TOTAL);
    g_assert_cmpuint(credit_count(s, 0), ==, CREDIT_TOTAL);
    qtest_quit(s);
}

/*
 * The charge and the return told apart: the map sends SI Viz's credits
 * to slot B, so slot A's stores show as A's loss and B's gain. Each
 * doubleword store is one credit, not one per 32-bit word it carries.
 */
static void test_credit_doubleword_store_returns_one_credit(void)
{
    QTestState *s = credit_start();

    qtest_writeq(s, CREDIT_COUNT(1), 0);
    qtest_writeq(s, CREDIT_MAP(0), CREDIT_ROUTE(1, SI_WIDGET));
    credit_run(s);
    credit_stores(s, CREDIT_PORT_STORE, 7);
    g_assert_cmpuint(credit_count(s, 0), ==, CREDIT_TOTAL - 7);
    g_assert_cmpuint(credit_count(s, 1), ==, 7);
    qtest_quit(s);
}

/*
 * The return names SI Viz's own widget: an entry for another widget does
 * not match it, and the entry for SI Viz's widget does.
 */
static void test_credit_return_names_the_boards_widget(void)
{
    QTestState *s = credit_start();

    qtest_writeq(s, CREDIT_COUNT(1), 0);
    qtest_writeq(s, CREDIT_MAP(0), CREDIT_ROUTE(0, SI_WIDGET - 1));
    qtest_writeq(s, CREDIT_MAP(1), CREDIT_ROUTE(1, SI_WIDGET));
    credit_run(s);
    credit_stores(s, CREDIT_PORT_STORE, 5);
    g_assert_cmpuint(credit_count(s, 0), ==, CREDIT_TOTAL - 5);
    g_assert_cmpuint(credit_count(s, 1), ==, 5);
    qtest_quit(s);
}

/*
 * A store the Hub did not charge earns nothing back: with the page
 * disabled, as IRIX's detach leaves it, neither slot moves.
 */
static void test_credit_uncounted_store_returns_nothing(void)
{
    QTestState *s = credit_start();

    qtest_writeq(s, CREDIT_PAGE(0), CREDIT_PORT_PAGE);
    qtest_writeq(s, CREDIT_COUNT(1), 0);
    qtest_writeq(s, CREDIT_MAP(0), CREDIT_ROUTE(1, SI_WIDGET));
    credit_run(s);
    credit_stores(s, CREDIT_PORT_STORE, 4);
    g_assert_cmpuint(credit_count(s, 0), ==, CREDIT_TOTAL);
    g_assert_cmpuint(credit_count(s, 1), ==, 0);
    qtest_quit(s);
}
int main(int argc, char **argv)
{
    int ret;
    bool list_mode = false;
    int i;

    for (i = 1; i < argc; i++) {
        if (g_str_equal(argv[i], "-l") ||
            g_str_equal(argv[i], "--list")) {
            list_mode = true;
            break;
        }
    }

    g_test_init(&argc, &argv, NULL);


    mgras_qtest_add_func(
        "sn0/origin2000/mgras/pending-mask-and-reassert",
        test_pending_mask_and_reassert, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/general-interrupt-vector",
        test_general_interrupt_vector, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/retrace-interrupt-vector",
        test_retrace_interrupt_vector, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/retrace-interrupt-needs-vc3-enable",
        test_retrace_interrupt_needs_vc3_enable, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/retrace-period-and-phase",
        test_retrace_period_and_phase, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/retrace-period-follows-vc3-tables",
        test_retrace_period_follows_vc3_tables, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/retrace-short-frame-runs-stock-period",
        test_retrace_short_frame_runs_stock_period, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/irq-enable-set-and-clear-ports",
        test_irq_enable_set_and_clear_ports, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/swap-command-raises-interrupt-10",
        test_swap_command_raises_interrupt_10, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/swap-command-needs-enable",
        test_swap_command_needs_enable, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/swap-keeps-context-switch-enable",
        test_swap_keeps_context_switch_enable, true);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/widget-error-interrupt-vector",
        test_widget_error_interrupt_vector, false);
    /* The credit cases run a processor. */
    if (list_mode || qtest_has_accel("tcg")) {
        mgras_qtest_add_func(
            "sn0/origin2000/mgras/credit-burst-returns-to-allocation",
            test_credit_burst_returns_to_allocation, false);
        mgras_qtest_add_func(
            "sn0/origin2000/mgras/"
            "credit-doubleword-store-returns-one-credit",
            test_credit_doubleword_store_returns_one_credit, false);
        mgras_qtest_add_func(
            "sn0/origin2000/mgras/credit-return-names-the-boards-widget",
            test_credit_return_names_the_boards_widget, false);
        mgras_qtest_add_func(
            "sn0/origin2000/mgras/"
            "credit-uncounted-store-returns-nothing",
            test_credit_uncounted_store_returns_nothing, false);
    }

    ret = g_test_run();
    if (qts) {
        qtest_quit(qts);
        qts = NULL;
    }

    return ret;
}
