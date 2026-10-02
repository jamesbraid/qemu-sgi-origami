/*
 * SI Viz register-contract and protocol qtests: board identity, bring-up,
 * reset, interrupt and register-file cases.
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


#define REG_IDENTIFICATION     MGRAS_REG(0x000004)
#define REG_WIDGET_CONTROL     MGRAS_REG(0x000024)
#define REG_BRINGUP_STEP       MGRAS_REG(0x011098)
#define REG_RESET_ARG          MGRAS_REG(0x011008)
#define REG_BUS_STATUS         MGRAS_REG(0x020100)
#define REG_DMA_BUSY           MGRAS_REG(0x020200)
#define REG_DIRECT_COPY_READY  MGRAS_REG(0x02c578)
#define REG_I2C_SCRATCH        MGRAS_REG(0x073c08)
#define REG_I2C_STROBE         MGRAS_REG(0x073c88)
#define REG_SYNC_STATUS        MGRAS_REG(0x061038)
#define REG_REPP_SYNC          MGRAS_REG(0x071f80)
#define REG_DAC_LUT_DATA       MGRAS_REG(0x071890)
#define REG_DAC_CONFIG         MGRAS_REG(0x071988)
#define REG_HQ_UPLOAD           MGRAS_REG(0x050000)
#define REG_VC3_INDEX           MGRAS_REG(0x072008)
#define REG_VC3_DATA            MGRAS_REG(0x0720b0)
#define REG_BOARD_INVENTORY     MGRAS_REG(0x072488)
#define REG_INT_DEST_UPPER_ALT  MGRAS_REG(0x000030)
#define REG_INT_DEST_LOWER_ALT  MGRAS_REG(0x000038)
#define REG_HQ_CONFIG_A         MGRAS_REG(0x011030)
#define REG_HQ_CONFIG_B         MGRAS_REG(0x011038)

#define ID_PART(v)          (((v) >> 12) & 0xffff)
#define ID_MANUFACTURER(v)  (((v) >> 1) & 0x7ff)
#define ID_ALWAYS_ONE(v)    ((v) & 1)

#define MGRAS_PART             0xc003
#define MGRAS_MANUFACTURER     0x02aa

static uint64_t mgras_read_width(QTestState *qts, uint64_t addr,
                              unsigned int width)
{
    switch (width) {
    case 1:
        return qtest_readb(qts, addr);
    case 2:
        return qtest_readw(qts, addr);
    case 4:
        return qtest_readl(qts, addr);
    case 8:
        return qtest_readq(qts, addr);
    default:
        g_assert_not_reached();
    }
}

static void mgras_write_width(QTestState *qts, uint64_t addr,
                           unsigned int width, uint64_t value)
{
    switch (width) {
    case 1:
        qtest_writeb(qts, addr, value);
        return;
    case 2:
        qtest_writew(qts, addr, value);
        return;
    case 4:
        qtest_writel(qts, addr, value);
        return;
    case 8:
        qtest_writeq(qts, addr, value);
        return;
    default:
        g_assert_not_reached();
    }
}

/*
 * Replay a captured gamma-table load: a halfword reset to REG_DAC_LUT_INDEX,
 * then 256 rows of three halfword writes to REG_DAC_LUT_DATA (red, green,
 * blue), matching the 768-write bursts a desktop session's login sends. Each
 * write's high byte is the stored gamma output; the low byte is unused.
 */
static void mgras_load_dac_gamma(QTestState *qts, const uint8_t ramp[3][256])
{
    qtest_writew(qts, REG_DAC_LUT_INDEX, 0);
    for (unsigned int row = 0; row < 256; row++) {
        for (unsigned int channel = 0; channel < 3; channel++) {
            qtest_writew(qts, REG_DAC_LUT_DATA,
                         (uint16_t)ramp[channel][row] << 8);
        }
    }
}

static void test_identity_and_aperture(void)
{
    uint32_t id;

    mgras_reset();

    id = qtest_readl(qts, REG_IDENTIFICATION);
    g_assert_cmphex(ID_PART(id), ==, MGRAS_PART);
    g_assert_cmphex(ID_MANUFACTURER(id), ==, MGRAS_MANUFACTURER);
    g_assert_cmphex(ID_ALWAYS_ONE(id), ==, 1);


    g_assert_cmphex(qtest_readl(qts, REG_STARTUP_STATUS) & 0x1eff, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_DMA_BUSY) & 0x1f, ==, 0);
    /* Bits 7:4 are the default SI board's RE4 revision, 2. */
    g_assert_cmphex(qtest_readl(qts, REG_DIRECT_COPY_READY), ==, 0x120);
}

/*
 * Inferred (see the read/write-side comments above these two cases in
 * mgras_read_internal()/mgras_write()): the first halves of the
 * 64-bit-aligned widget-register slots ignore writes rather than raising
 * a guest error a real access hits. 0x000020 reads zero; 0x000000 reads
 * an identification word: the HQ revision in bits 31:28 (2 on the default
 * SI board), then part 0xc003 and manufacturer 0x2aa, which the stock
 * IP30 IDE's board probe requires there. Reading
 * REG_IDENTIFICATION back unchanged after writing both proves the write
 * side does not alias into the neighboring real register.
 */
static void test_widget_reserved_halves_raz_wi(void)
{
    uint32_t identification;

    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x000000)), ==, 0x2c003555);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x000020)), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    identification = qtest_readl(qts, REG_IDENTIFICATION);
    qtest_writel(qts, MGRAS_REG(0x000000), 0xffffffff);
    qtest_writel(qts, MGRAS_REG(0x000020), 0xffffffff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x000000)), ==, 0x2c003555);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x000020)), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_IDENTIFICATION), ==, identification);
}


static void test_widget_control_is_plain_storage(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0);

    qtest_writel(qts, REG_WIDGET_CONTROL, 0x0000300f);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0x0000300f);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writel(qts, REG_WIDGET_CONTROL, 0x00003000);
    g_assert_cmphex(qtest_readl(qts, REG_WIDGET_CONTROL), ==, 0x00003000);
}


static void test_generic_bringup_step(void)
{
    mgras_reset();

    qtest_writel(qts, REG_BRINGUP_STEP, (520u << 10) | (65u << 2));
    g_assert_cmphex(qtest_readl(qts, REG_BRINGUP_STEP) & 0x2, ==, 0x2);
}


static void test_bus_status_reads_settled(void)
{
    mgras_reset();

    g_assert_cmpuint(((qtest_readl(qts, REG_BUS_STATUS) & 0x1f) + 1) >> 1, <, 8);
}


static void test_reset_registers_are_plain_storage(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 0);
    qtest_writel(qts, REG_RESET_ARG, 0x765b900f);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 0x765b900f);


    qtest_writel(qts, REG_RESET_CONTROL, 0x40000000);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_ARG), ==, 0);
}


static void test_i2c_probe_loopback(void)
{
    mgras_reset();

    qtest_writeb(qts, REG_I2C_STROBE, 0x40);
    qtest_writeb(qts, REG_I2C_SCRATCH, 0x55);
    qtest_writeb(qts, REG_I2C_STROBE, 0x40);
    g_assert_cmphex(qtest_readb(qts, REG_I2C_SCRATCH), ==, 0x55);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    qtest_writeb(qts, REG_I2C_SCRATCH, 0xaa);
    g_assert_cmphex(qtest_readb(qts, REG_I2C_SCRATCH), ==, 0xaa);
}

/*
 * REG_I2C_STROBE's byte write above was already accepted as a no-op; this
 * proves the read side now mirrors it, answering the approximated
 * idle/ack value instead of raising a guest error.
 */
static void test_i2c_strobe_byte_read(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readb(qts, REG_I2C_STROBE), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}


static void test_repp_sync_handshake(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_REPP_SYNC) & 0x00000f0f, ==,
                     0x00000101);


    qtest_writeb(qts, REG_XMAP_INDEX, 0x1c);
    g_assert_cmphex(qtest_readb(qts, REG_XMAP_INDEX), ==, 0x1c);

    /*
     * 0x071c08 is PP1SELECT; 1 reaches every PP1, as the kernel leaves it
     * after its per-PP1 loops.
     */
    qtest_writeb(qts, REG_REPP_STATUS_1, 1);
    qtest_writeb(qts, REG_XMAP_INDEX, 0x1c);
    qtest_writel(qts, REG_REPP_STATUS_3, 0x653);
    qtest_writel(qts, REG_REPP_STATUS_4, 0x10214);
    qtest_writel(qts, REG_SYNC_STATUS, 1);
    qtest_writel(qts, REG_REPP_SYNC, 0x50214);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_1), ==, 1);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_3), ==, 0x653);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_4), ==, 0x10214);
    g_assert_cmphex(qtest_readl(qts, REG_SYNC_STATUS), ==, 1);

    g_assert_cmphex(qtest_readb(qts, REG_XMAP_INDEX), ==, 0x1c);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_SYNC) & 0x00000f0f, ==,
                     0x00000101);
}

/*
 * PP1SELECT (0x071c08, REG_REPP_STATUS_1) and CONFIGB (0x071d08,
 * REG_REPP_STATUS_4) are the two REPP status ports gxemul's
 * impactsr-bsd.h and OpenBSD's impactreg.h declare byte-wide
 * (IMPACTSR_REG8). test_repp_sync_handshake above already covers their
 * word-width storage; this proves the newly accepted byte width is the
 * same plain storage, at no width other than the ones now allowed.
 */
static void test_repp_status_byte_writes(void)
{
    mgras_reset();

    /* RE 0's second PP1, which CONFIGB below then reaches. */
    qtest_writeb(qts, REG_REPP_STATUS_1, 0x02);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_1), ==, 0x02);

    qtest_writeb(qts, REG_REPP_STATUS_4, 0x21);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_REPP_STATUS_4), ==, 0x21);

    mgras_reset();
    mgras_write_width(qts, REG_REPP_STATUS_1, 2, 0xff);
    mgras_assert_width_error(qts, REG_REPP_STATUS_1);
}

/*
 * XMAP_BUF_SELECT is a word register indexed by REG_XMAP_INDEX the same way as
 * REG_XMAP_MAIN_MODE. Other widths raise the width exception.
 */
static void test_xmap_buf_select_write(void)
{
    mgras_reset();

    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 0xffffffff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    qtest_writeb(qts, REG_XMAP_INDEX, 4);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 0x0000ffff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    mgras_reset();
    mgras_write_width(qts, REG_XMAP_BUF_SELECT, 1, 0xff);
    mgras_assert_width_error(qts, REG_XMAP_BUF_SELECT);

    mgras_reset();
    mgras_write_width(qts, REG_XMAP_BUF_SELECT, 8, 0xff);
    mgras_assert_width_error(qts, REG_XMAP_BUF_SELECT);

    mgras_reset();
    mgras_read_width(qts, REG_XMAP_BUF_SELECT, 1);
    mgras_assert_width_error(qts, REG_XMAP_BUF_SELECT);
}

/*
 * SGI's Octane IDE XMAP test (the XMAP register test, ide.IP30 6.5.30 at
 * 0xa80000002086907c) writes each of eight patterns to XMAP_BUF_SELECT
 * and reads it back: all 32 bits under index byte 0, which it calls the
 * main buffer select, and the low 8 bits under index byte 4, the overlay
 * buffer select. A mismatch fails the board.
 */
static void test_xmap_buf_select_reads_back(void)
{
    static const uint32_t patterns[] = {
        0x00000001, 0x00000002, 0x00000004, 0x00000008,
        0x10000000, 0x20000000, 0x40000000, 0x80000000,
    };

    mgras_reset();
    for (int i = 0; i < ARRAY_SIZE(patterns); i++) {
        qtest_writeb(qts, REG_XMAP_INDEX, 0);
        qtest_writel(qts, REG_XMAP_BUF_SELECT, patterns[i]);
        qtest_writeb(qts, REG_XMAP_INDEX, 0);
        g_assert_cmphex(qtest_readl(qts, REG_XMAP_BUF_SELECT), ==,
                        patterns[i]);

        qtest_writeb(qts, REG_XMAP_INDEX, 4);
        qtest_writel(qts, REG_XMAP_BUF_SELECT, patterns[i] & 0xff);
        qtest_writeb(qts, REG_XMAP_INDEX, 4);
        g_assert_cmphex(qtest_readl(qts, REG_XMAP_BUF_SELECT) & 0xff, ==,
                        patterns[i] & 0xff);
    }
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * The same IDE test reads 0x071d00 under two index bytes after writing
 * 0xff000000 there under index byte 1. Index 4 is the XMAP revision and
 * passes only on 0 or 1; index 8 is the FIFO capacity, whose low nibble
 * must be 1. The IRIX bring-up path reads the revision the same way
 * (index 4, low three bits). Neither read may echo the earlier write.
 */
static void test_xmap_revision_and_fifo_capacity(void)
{
    uint32_t rev, cap;

    mgras_reset();
    qtest_writeb(qts, REG_XMAP_INDEX, 1);
    qtest_writel(qts, REG_REPP_STATUS_3, 0xff000000);

    qtest_writeb(qts, REG_XMAP_INDEX, 4);
    rev = qtest_readl(qts, REG_REPP_STATUS_3);
    g_assert_true(rev == 0 || rev == 1);

    qtest_writeb(qts, REG_XMAP_INDEX, 8);
    cap = qtest_readl(qts, REG_REPP_STATUS_3);
    g_assert_cmphex(cap & 0xf, ==, 1);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}
#define PP1_SELECT_ALL          1

static void test_pp1_select_routes_xmap_on_two_re_board(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS " -global sgi-mgras.board=ssi");

    mgras_select_xmap(s, PP1_SELECT_ALL, 0);
    qtest_writel(s, REG_XMAP_MAIN_MODE, 0x111);
    qtest_writel(s, REG_XMAP_BUF_SELECT, 0x5);
    qtest_writel(s, REG_REPP_STATUS_3, 0x0018073f);
    for (int re = 0; re < 2; re++) {
        for (int pp = 0; pp < 2; pp++) {
            mgras_select_xmap(s, PP1_SELECT(re, pp), 0);
            g_assert_cmphex(qtest_readl(s, REG_XMAP_MAIN_MODE), ==, 0x111);
            g_assert_cmphex(qtest_readl(s, REG_XMAP_BUF_SELECT), ==, 0x5);
            g_assert_cmphex(qtest_readl(s, REG_REPP_STATUS_3), ==,
                            0x0018073f);
        }
    }

    /* One PP1 of RE 1 alone: the other three keep what they had. */
    mgras_select_xmap(s, PP1_SELECT(1, 1), 0);
    qtest_writel(s, REG_XMAP_MAIN_MODE, 0x222);
    qtest_writel(s, REG_REPP_STATUS_3, 0x0008073f);
    for (int re = 0; re < 2; re++) {
        for (int pp = 0; pp < 2; pp++) {
            bool picked = re == 1 && pp == 1;

            mgras_select_xmap(s, PP1_SELECT(re, pp), 0);
            g_assert_cmphex(qtest_readl(s, REG_XMAP_MAIN_MODE), ==,
                            picked ? 0x222 : 0x111);
            g_assert_cmphex(qtest_readl(s, REG_REPP_STATUS_3), ==,
                            picked ? 0x0008073f : 0x0018073f);
        }
    }

    /* The index register belongs to each PP1 as well. */
    mgras_select_xmap(s, PP1_SELECT(0, 1), 8);
    qtest_writeb(s, REG_PP1_SELECT, PP1_SELECT(0, 0));
    g_assert_cmphex(qtest_readb(s, REG_XMAP_INDEX), ==, 0);
    qtest_writeb(s, REG_PP1_SELECT, PP1_SELECT(0, 1));
    g_assert_cmphex(qtest_readb(s, REG_XMAP_INDEX), ==, 8);

    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/*
 * A one-RE board has no PP1s behind 4 and 6: a write there changes nothing
 * the board has, and the two PP1 it does have still take writes to all.
 * What real hardware returns for a read of an absent PP1 is unknown; the
 * model answers 0. Nothing here is a guest-visible board error.
 */
static void test_pp1_select_absent_pp1_on_one_re_board(void)
{
    mgras_reset();

    mgras_select_xmap(qts, PP1_SELECT_ALL, 0);
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 0x111);
    mgras_select_xmap(qts, PP1_SELECT(1, 0), 0);
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 0x333);
    g_assert_cmphex(qtest_readl(qts, REG_XMAP_MAIN_MODE), ==, 0);

    mgras_select_xmap(qts, PP1_SELECT(0, 0), 0);
    g_assert_cmphex(qtest_readl(qts, REG_XMAP_MAIN_MODE), ==, 0x111);
    mgras_select_xmap(qts, PP1_SELECT(0, 1), 0);
    g_assert_cmphex(qtest_readl(qts, REG_XMAP_MAIN_MODE), ==, 0x111);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    mgras_reset();
}

/*
 * Scanout reads the first PP1's mode tables. The kernel and X server give
 * every PP1 the same tables, so which one scanout reads is invisible to
 * them. Which PP1 drives which pixels is unknown, so a mode written to one
 * other PP1 alone does not change the screen.
 */
static void test_pp1_scanout_follows_first_pp1(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS " -global sgi-mgras.board=ssi");

    /* Map 29 is the one reset's mode 0x7a4 names; 0x384 names map 28. */
    mgras_enter_context(s);
    qtest_writel(s, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(s, 29, 0x40, 0x11223300);
    mgras_write_cmap_entry(s, 28, 0x40, 0x44556600);
    qtest_writeq(s, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(s, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x40));
    qtest_writeq(s, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(s, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (5u << 16) | 5u));
    g_assert_cmphex(mgras_test_display_pixel(s, 5, 5), ==, 0x00112233);

    mgras_select_xmap(s, PP1_SELECT(1, 0), 0);
    qtest_writel(s, REG_XMAP_MAIN_MODE, 0x384);
    g_assert_cmphex(mgras_test_display_pixel(s, 5, 5), ==, 0x00112233);

    mgras_select_xmap(s, PP1_SELECT(0, 0), 0);
    qtest_writel(s, REG_XMAP_MAIN_MODE, 0x384);
    g_assert_cmphex(mgras_test_display_pixel(s, 5, 5), ==, 0x00445566);
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

#define REG_XMAP_SYNC_0         MGRAS_REG(0x071f00)

/*
 * With every PP1 selected, a read answers from PP1 0. That is an
 * approximation: what hardware returns then is unknown. The four PP1s differ
 * first, so the answer shows which one gave it.
 */
static void test_pp1_select_all_reads_first_pp1(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS MGRAS_TWO_RE_ARGS);

    mgras_give_pp1s_distinct_xmaps(s);
    mgras_assert_pp1_xmap(s, PP1_SELECT_ALL, 0);
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/*
 * PP1SELECT bits 7:3 are zero in every value IRIX writes, and the model
 * ignores them: with all five set, reads and writes reach the PP1 bits
 * 2:1 name, or every PP1 when bit 0 is set. What hardware does with them
 * is unknown.
 */
static void test_pp1_select_ignores_bits_7_to_3(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS MGRAS_TWO_RE_ARGS);

    mgras_give_pp1s_distinct_xmaps(s);
    for (unsigned int n = 0; n < 4; n++) {
        mgras_assert_pp1_xmap(s, 0xf8 | mgras_pp1_select_one(n), n);
    }

    mgras_select_xmap(s, 0xf8 | PP1_SELECT(1, 0), 0);
    qtest_writel(s, REG_XMAP_MAIN_MODE, 0x999);
    for (unsigned int n = 0; n < 4; n++) {
        mgras_select_xmap(s, mgras_pp1_select_one(n), 0);
        g_assert_cmphex(qtest_readl(s, REG_XMAP_MAIN_MODE), ==,
                        n == 2 ? 0x999 : 0x100 + n);
    }

    mgras_select_xmap(s, 0xf8 | PP1_SELECT_ALL, 0);
    qtest_writel(s, REG_XMAP_MAIN_MODE, 0x777);
    for (unsigned int n = 0; n < 4; n++) {
        mgras_select_xmap(s, mgras_pp1_select_one(n), 0);
        g_assert_cmphex(qtest_readl(s, REG_XMAP_MAIN_MODE), ==, 0x777);
    }
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/*
 * 0x071f00 and 0x071f80 sit among the XMAP registers IRIX reaches through
 * PP1SELECT, but the model keeps no copy of them for any PP1, an
 * approximation of the model: under every
 * selection they read 1 and 0x101, and a write under any selection is
 * dropped.
 */
static void test_pp1_select_leaves_sync_registers_shared(void)
{
    static const uint8_t selects[] = {
        PP1_SELECT(0, 0), PP1_SELECT(0, 1), PP1_SELECT(1, 0),
        PP1_SELECT(1, 1), PP1_SELECT_ALL,
    };
    QTestState *s = qtest_init(MACHINE_ARGS MGRAS_TWO_RE_ARGS);

    for (size_t i = 0; i < ARRAY_SIZE(selects); i++) {
        mgras_select_xmap(s, selects[i], 0);
        qtest_writel(s, REG_XMAP_SYNC_0, 0x1230 + i);
        qtest_writel(s, REG_REPP_SYNC, 0x50210 + i);
        mgras_select_xmap(s, selects[i], 0x1c);
        qtest_writel(s, REG_REPP_SYNC, 0x650 + i);
    }
    for (size_t i = 0; i < ARRAY_SIZE(selects); i++) {
        mgras_select_xmap(s, selects[i], 0);
        g_assert_cmphex(qtest_readl(s, REG_XMAP_SYNC_0), ==, 1);
        g_assert_cmphex(qtest_readl(s, REG_REPP_SYNC), ==, 0x101);
        mgras_select_xmap(s, selects[i], 0x1c);
        g_assert_cmphex(qtest_readl(s, REG_REPP_SYNC), ==, 0x101);
    }
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/*
 * Both REs of a two-RE board draw into one set of planes in the model. The
 * OCTANE Technical Report has the second RE take alternate spans, so on
 * hardware adjacent rows may belong to different REs; no software path IRIX
 * uses can tell, and the model does not split them. Two adjacent rows filled
 * with different colours both read back from the planes scanout uses. A model
 * that split the REs' pixels would have to change this deliberately.
 */
static void test_two_re_board_draws_one_set_of_planes(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS MGRAS_TWO_RE_ARGS);

    mgras_enter_context(s);
    qtest_writeq(s, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    for (unsigned int y = 20; y < 24; y++) {
        qtest_writeq(s, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_FILLCOLOR_R, 0x30 + y));
        qtest_writeq(s, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND(REG_BLOCK_START, (10u << 16) | y));
        qtest_writeq(s, REG_CONTEXT_COMMAND,
                     MGRAS_COMMAND_EXEC(REG_BLOCK_END, (13u << 16) | y));
    }
    for (unsigned int y = 20; y < 24; y++) {
        g_assert_cmphex(mgras_test_pixel(s, 10, y), ==, 0x30 + y);
        g_assert_cmphex(mgras_test_pixel(s, 13, y), ==, 0x30 + y);
    }
    g_assert_cmphex(mgras_test_pixel(s, 10, 19), ==, 0);
    g_assert_cmphex(mgras_test_pixel(s, 10, 24), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

typedef struct SIVisualRegisterWidth {
    uint64_t addr;
    unsigned int width;
} SIVisualRegisterWidth;

static const SIVisualRegisterWidth mgras_visual_register_widths[] = {
    { REG_XMAP_INDEX, 1 },
    { REG_XMAP_MAIN_MODE, 4 },
    { REG_CMAP_WRITE_INDEX, 2 },
    { REG_CMAP_DATA, 4 },
};

static const unsigned int mgras_access_widths[] = { 1, 2, 4, 8 };

static void test_visual_register_read_widths(void)
{
    for (int reg = 0; reg < ARRAY_SIZE(mgras_visual_register_widths); reg++) {
        for (int width = 0; width < ARRAY_SIZE(mgras_access_widths); width++) {
            if (mgras_access_widths[width] ==
                mgras_visual_register_widths[reg].width) {
                continue;
            }
            mgras_reset();
            mgras_read_width(qts, mgras_visual_register_widths[reg].addr,
                          mgras_access_widths[width]);
            mgras_assert_width_error(qts,
                                     mgras_visual_register_widths[reg].addr);
        }
    }
}

static void test_visual_register_write_widths(void)
{
    for (int reg = 0; reg < ARRAY_SIZE(mgras_visual_register_widths); reg++) {
        for (int width = 0; width < ARRAY_SIZE(mgras_access_widths); width++) {
            if (mgras_access_widths[width] ==
                mgras_visual_register_widths[reg].width) {
                continue;
            }
            mgras_reset();
            mgras_write_width(qts, mgras_visual_register_widths[reg].addr,
                           mgras_access_widths[width], UINT64_MAX);
            mgras_assert_width_error(qts,
                                     mgras_visual_register_widths[reg].addr);
        }
    }
}


static void test_hq_status_bringup_bit(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x2, ==, 0x2);
}


static void test_hq_status_hiwater_poll_bit(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x10, ==, 0x10);
}


static void test_hq_status_independent_of_sticky_error(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS), ==, 0x13);


    qtest_writel(qts, MGRAS_REG(0x090000), 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 1);


    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS), ==, 0x13);
}


/*
 * The IRIX kernel's DMA read set-up writes 0xf200 and gives up on the read
 * unless 0x020000 bit 0 comes up within two seconds; nothing is queued
 * ahead of the request in the model, so the bit is already set. The
 * request is a known token: it is not reported as an unimplemented
 * command-FIFO token under -d unimp. Bit 0 reading set is not new with
 * the token, so the log is what discriminates.
 */
static void test_hq_status_re_sync_after_request(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *log = NULL;
    g_autofree char *args = NULL;
    g_autofree char *contents = NULL;
    GError *err = NULL;
    QTestState *s;

    tmpfs = g_dir_make_tmp("sn0-mgras-resync-XXXXXX", &err);
    g_assert_no_error(err);
    log = g_strdup_printf("%s/unimp.log", tmpfs);
    args = g_strdup_printf(MACHINE_ARGS " -d unimp -D %s", log);
    s = qtest_init(args);

    gl_enter_slot(s, GL_SLOT_A);
    gl_word(s, GL_CONTEXT_OPENER);
    qtest_writel(s, REG_CONTEXT_COMMAND, 0xf200);
    g_assert_cmphex(qtest_readl(s, REG_HQ_STATUS) & 0x1, ==, 0x1);
    g_assert_cmphex(qtest_readl(s, REG_HQ_STATUS) & 0x2, ==, 0x2);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(log, &contents, NULL, &err));
    g_assert_no_error(err);
    g_assert_null(strstr(contents, "unimplemented command-FIFO token"));
    unlink(log);
    g_rmdir(tmpfs);
}


/*
 * A captured desktop session brackets each gamma-table load with writes to
 * these three registers at the widths it actually uses (byte, confirmed
 * against the capture); their role is otherwise unconfirmed, so the model
 * keeps them as opaque flat storage. REG_DAC_LUT_INDEX takes the halfword
 * width the capture uses and resets the load position that
 * test_dac_gamma_identity_ramp_leaves_colors_unchanged and
 * test_dac_gamma_curve_applies_to_scanout exercise through the data port,
 * since the position itself has no readable register.
 */
static void test_dac_lut_load(void)
{
    mgras_reset();

    qtest_writeb(qts, REG_DAC_CONFIG, 0x1c);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_CONFIG), ==, 0x1c);

    /*
     * REG_DAC_LUT_INDEX_LOW sits at an odd address: no read width is both
     * aligned to it and within the read-side whitelist, so unlike the
     * other bracketing registers its write cannot be confirmed by a
     * readback. A captured session never reads it either.
     */
    qtest_writeb(qts, REG_DAC_LUT_INDEX_LOW, 0xff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    qtest_writeb(qts, REG_DAC_LUT_DATA_2, 0x01);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_LUT_DATA_2), ==, 0x01);

    qtest_writew(qts, REG_DAC_LUT_INDEX, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_DAC_LUT_INDEX), ==, 0);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}

/* The DAC mode byte reads back each walking pattern the IDE writes. */
static void test_dac_mode_byte_readback(void)
{
    mgras_reset();

    for (unsigned bit = 0; bit < 8; bit++) {
        qtest_writeb(qts, REG_DAC_CONFIG, 1u << bit);
        g_assert_cmphex(qtest_readb(qts, REG_DAC_CONFIG), ==, 1u << bit);
        qtest_writeb(qts, REG_DAC_CONFIG, (uint8_t)~(1u << bit));
        g_assert_cmphex(qtest_readb(qts, REG_DAC_CONFIG), ==,
                        (uint8_t)~(1u << bit));
    }
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * SGI's Octane IDE DAC reset (the DAC reset test, ide.IP30 6.5.30 at
 * 0xa800000020854920) addresses DAC registers through REG_DAC_LUT_INDEX,
 * a halfword whose high byte is the register number, and moves their
 * data through REG_DAC_LUT_DATA_2 a byte at a time. It fails the board
 * unless the address reads back, register 3 (the DAC ID) is 0x79 with
 * register 0xb (its revision) at 1 -- or 0x76 with 0, which it warns
 * about -- and each register it programs reads back under mask 0xff.
 * A recorded IRIX desktop session programs the same registers, in the
 * same order, before each gamma-table load (0x0600 takes 0xec there).
 */
static void test_dac_register_file_matches_ide_reset(void)
{
    static const struct {
        uint16_t addr;
        uint8_t value;
    } regs[] = {
        { 0x0400, 0xff }, { 0x0500, 0x89 }, { 0x0600, 0xe4 },
        { 0x0700, 0xc0 }, { 0x0800, 0x06 }, { 0x0402, 0x01 },
    };
    uint8_t id, rev;

    mgras_reset();
    qtest_writeb(qts, REG_DAC_CONFIG, 3);
    qtest_writeb(qts, REG_DAC_CONFIG, 2);
    qtest_writeb(qts, REG_DAC_CONFIG, 3);
    /* Register number 5 through the byte port: command register 1. */
    qtest_writeb(qts, REG_DAC_LUT_INDEX_LOW, 5);
    qtest_writeb(qts, REG_DAC_LUT_DATA_2, 0x89);
    qtest_writew(qts, REG_DAC_LUT_INDEX, 0x0500);
    g_assert_cmphex(qtest_readb(qts, REG_DAC_LUT_DATA_2), ==, 0x89);
    qtest_writew(qts, REG_DAC_LUT_INDEX, 0x0d00);
    qtest_writeb(qts, REG_DAC_LUT_DATA_2, 0x40);

    qtest_writew(qts, REG_DAC_LUT_INDEX, 0);
    g_assert_cmphex((qtest_readw(qts, REG_DAC_LUT_INDEX) >> 8) & 0xff, ==, 0);

    qtest_writew(qts, REG_DAC_LUT_INDEX, 0x0300);
    id = qtest_readb(qts, REG_DAC_LUT_DATA_2);
    qtest_writew(qts, REG_DAC_LUT_INDEX, 0x0b00);
    rev = qtest_readb(qts, REG_DAC_LUT_DATA_2);
    g_assert_true((id == 0x79 && rev == 1) || (id == 0x76 && rev == 0));

    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        qtest_writew(qts, REG_DAC_LUT_INDEX, regs[i].addr);
        qtest_writeb(qts, REG_DAC_LUT_DATA_2, regs[i].value);
    }
    for (int i = 0; i < ARRAY_SIZE(regs); i++) {
        qtest_writew(qts, REG_DAC_LUT_INDEX, regs[i].addr);
        g_assert_cmphex(qtest_readb(qts, REG_DAC_LUT_DATA_2), ==,
                        regs[i].value);
    }
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * SGI's Octane IDE DAC address test (the DAC address test, ide.IP30 6.5.30 at
 * 0xa800000020853c2c) writes a byte to 0x071811 and requires it back in
 * bits 15:8 of the halfword at 0x071810, the register number. It then
 * writes the halfword with the pattern in its high byte and checks the
 * same byte. The halfword's low byte is never compared.
 */
static void test_dac_address_byte_port(void)
{
    static const uint8_t patterns[] = {
        0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0xff,
    };

    mgras_reset();
    for (int i = 0; i < ARRAY_SIZE(patterns); i++) {
        qtest_writeb(qts, REG_DAC_LUT_INDEX_LOW, patterns[i]);
        g_assert_cmphex((qtest_readw(qts, REG_DAC_LUT_INDEX) >> 8) & 0xff, ==,
                        patterns[i]);
    }
    for (int i = 0; i < ARRAY_SIZE(patterns); i++) {
        qtest_writew(qts, REG_DAC_LUT_INDEX, (uint16_t)patterns[i] << 8);
        g_assert_cmphex((qtest_readw(qts, REG_DAC_LUT_INDEX) >> 8) & 0xff, ==,
                        patterns[i]);
    }
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * SGI's Octane IDE VC3 register test (the VC3 register test, ide.IP30 6.5.30
 * at 0xa80000002083da2c) stops video timing, then writes walking-one,
 * walking-zero, 0 and 0xffff patterns to VC3 registers 0-0x13 and reads
 * each straight back, all 16 bits. Register 7 is the SRAM address
 * (VC3_RAM_ADDR in the test's own names), so it reads back as the
 * address the write loaded.
 */
static void test_vc3_registers_read_back(void)
{
    mgras_reset();
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0));
    /* Walking one, walking zero, then 0 and 0xffff: 34 patterns. */
    for (int step = 0; step < 34; step++) {
        uint16_t pattern = step < 16 ? 1u << step :
                           step < 32 ? (uint16_t)~(1u << (step - 16)) :
                           step == 32 ? 0 : 0xffff;

        for (int reg = 0; reg <= 0x13; reg++) {
            qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(reg, pattern));
            qtest_writeb(qts, REG_VC3_INDEX, reg);
            g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, pattern);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

static void test_dac_gamma_identity_ramp_leaves_colors_unchanged(void)
{
    uint8_t ramp[3][256];

    mgras_reset();
    mgras_enter_context(qts);

    for (unsigned int channel = 0; channel < 3; channel++) {
        for (unsigned int i = 0; i < 256; i++) {
            ramp[channel][i] = i;
        }
    }

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(qts, 29, 0x40, 0x11223300);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x40));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (5u << 16) | 5u));
    g_assert_cmphex(mgras_test_display_pixel(qts, 5, 5), ==, 0x00112233);

    mgras_load_dac_gamma(qts, ramp);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    g_assert_cmphex(mgras_test_display_pixel(qts, 5, 5), ==, 0x00112233);
}

/*
 * The captured post-login desktop session's gamma table matches a
 * gamma-1.7 correction curve, 255*(i/255)^(1/1.7) rounded to the nearest
 * integer, at every one of its 256 captured entries within 1 and exactly
 * at the spot-checked entries 85 (-> 134) and 142 (-> 181). Only entry
 * 0x40's colour is asserted below; the rest of the curve stands in for
 * the untested majority of a real load.
 */
static const uint8_t mgras_test_gamma_1_7_curve[256] = {
    0, 10, 15, 19, 22, 25, 28, 31, 33, 36, 38, 40,
    42, 44, 46, 48, 50, 52, 54, 55, 57, 59, 60, 62,
    64, 65, 67, 68, 70, 71, 72, 74, 75, 77, 78, 79,
    81, 82, 83, 84, 86, 87, 88, 89, 91, 92, 93, 94,
    95, 97, 98, 99, 100, 101, 102, 103, 105, 106, 107, 108,
    109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120,
    121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132,
    133, 134, 135, 135, 136, 137, 138, 139, 140, 141, 142, 143,
    144, 144, 145, 146, 147, 148, 149, 150, 150, 151, 152, 153,
    154, 155, 156, 156, 157, 158, 159, 160, 160, 161, 162, 163,
    164, 164, 165, 166, 167, 168, 168, 169, 170, 171, 172, 172,
    173, 174, 175, 175, 176, 177, 178, 178, 179, 180, 181, 181,
    182, 183, 184, 184, 185, 186, 187, 187, 188, 189, 190, 190,
    191, 192, 192, 193, 194, 195, 195, 196, 197, 197, 198, 199,
    199, 200, 201, 202, 202, 203, 204, 204, 205, 206, 206, 207,
    208, 208, 209, 210, 210, 211, 212, 212, 213, 214, 214, 215,
    216, 216, 217, 218, 218, 219, 220, 220, 221, 222, 222, 223,
    224, 224, 225, 226, 226, 227, 227, 228, 229, 229, 230, 231,
    231, 232, 233, 233, 234, 234, 235, 236, 236, 237, 238, 238,
    239, 239, 240, 241, 241, 242, 242, 243, 244, 244, 245, 245,
    246, 247, 247, 248, 248, 249, 250, 250, 251, 251, 252, 253,
    253, 254, 254, 255,
};

static void test_dac_gamma_curve_applies_to_scanout(void)
{
    uint8_t ramp[3][256];

    mgras_reset();
    mgras_enter_context(qts);

    for (unsigned int channel = 0; channel < 3; channel++) {
        memcpy(ramp[channel], mgras_test_gamma_1_7_curve, 256);
    }

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(qts, 29, 0x40, 0x55555500);

    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(qts, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x40));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND(REG_BLOCK_START, (5u << 16) | 5u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                MGRAS_COMMAND_EXEC(REG_BLOCK_END, (5u << 16) | 5u));
    g_assert_cmphex(mgras_test_display_pixel(qts, 5, 5), ==, 0x00555555);

    mgras_load_dac_gamma(qts, ramp);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    g_assert_cmphex(mgras_test_display_pixel(qts, 5, 5), ==, 0x00868686);
}


static void test_write_path_fixes_dont_raise_the_sticky_error(void)
{
    mgras_reset();

    qtest_writel(qts, REG_HQ_STATUS, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x2, ==, 0x2);


    qtest_writeq(qts, REG_CONTEXT_COMMAND, 0x482b0400000000ULL);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}


static void test_board_bringup_scratch_registers(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x02c574)) & 0x8, ==, 0x8);

    qtest_writel(qts, MGRAS_REG(0x061008), 0x1088);
    qtest_writel(qts, MGRAS_REG(0x061040), 0x210a);
    qtest_writel(qts, MGRAS_REG(0x060008), 2);
    qtest_writel(qts, MGRAS_REG(0x011088), 0x68f);
    qtest_writel(qts, MGRAS_REG(0x040918), 0x200000);
    qtest_writel(qts, MGRAS_REG(0x040928), 0);
    qtest_writeq(qts, MGRAS_REG(0x130400), 0x482b0400000000ULL);
    qtest_writel(qts, MGRAS_REG(0x011058), 0xffffffff);
    qtest_writel(qts, MGRAS_REG(0x070d88), 0x3000000);
    qtest_writel(qts, MGRAS_REG(0x071e80), 0);

    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);

    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x061008)), ==, 0x1088);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x061040)), ==, 0x210a);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x060008)), ==, 2);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x011088)), ==, 0x68f);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x040918)), ==, 0x200000);
    g_assert_cmphex(qtest_readq(qts, MGRAS_REG(0x130400)), ==,
                    0x482b0400000000ULL);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x011058)), ==, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x070d88)), ==, 0x3000000);
}

/*
 * 0x070d88 (VC3_ARG) already stores and echoes a word write above; this
 * proves the newly accepted byte width behaves the same way.
 */
static void test_vc3_arg_byte_write(void)
{
    mgras_reset();

    qtest_writeb(qts, MGRAS_REG(0x070d88), 0x77);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x070d88)), ==, 0x77);
}

static void test_board_bringup_byte_poll_bit_position(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x071208)), ==, 0x08000000);
    g_assert_cmphex(qtest_readb(qts, MGRAS_REG(0x071208)) & 0x8, ==, 0x8);
}

/*
 * 0x071708 carries the CMAP and RA revisions and the no-video-option bit
 * in its top byte; the default SI board answers 0x83. Both the word and
 * the byte width IRIX uses give that byte without raising a guest error.
 */
static void test_unresolved_revision_byte_read(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, MGRAS_REG(0x071708)), ==, 0x83000000);
    g_assert_cmphex(qtest_readb(qts, MGRAS_REG(0x071708)), ==, 0x83);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

static void test_board_info_byte_read_does_not_raise_error(void)
{
    QTestState *qts = qtest_init(MACHINE_ARGS);

    /*
     * The kernel reads this byte while it builds the board record and on
     * every OpenGL context query. Bits 7:4 are the monitor ID, 0xf on
     * every captured Origin board, bits 2:0 the first CMAP's revision (3
     * on the default SI board), and the read leaves the error state clear.
     */
    g_assert_cmphex(qtest_readb(qts, MGRAS_REG(0x071308)), ==, 0xf3);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    qtest_quit(qts);
}

static void test_hq_upload_and_readback(void)
{
    mgras_reset();

    qtest_writel(qts, REG_HQ_UPLOAD, 0xff112233);
    qtest_writel(qts, REG_HQ_UPLOAD + 4, 0x00abcdef);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_UPLOAD), ==, 0x00112233);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_UPLOAD + 4), ==, 0x00abcdef);


    qtest_writel(qts, REG_HQ_UPLOAD, 0x00445566);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_UPLOAD), ==, 0x00445566);
}

static void test_reset_clears_context_acknowledgements(void)
{
    mgras_reset();

    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, i);
    }
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);
    g_assert_true(qtest_qom_get_bool(qts, MGRAS_QOM_PATH,
                                     "x-test-first-ack"));
    g_assert_true(qtest_qom_get_bool(qts, MGRAS_QOM_PATH,
                                     "x-test-second-ack"));

    qtest_system_reset(qts);
    g_assert_false(qtest_qom_get_bool(qts, MGRAS_QOM_PATH,
                                      "x-test-first-ack"));
    g_assert_false(qtest_qom_get_bool(qts, MGRAS_QOM_PATH,
                                      "x-test-second-ack"));
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 1);
}


/*
 * The VC3 cursor position registers (VC3 registers 2 and 3) name the
 * glyph's top-left pixel plus 31 on each axis. The stock IRIX 6.5.30 server
 * writes the pointer position minus the glyph hotspot plus 31: for the shape
 * whose hotspot is its corner, a pointer at (48, 48) writes (79, 79). The
 * VC2's public Linux console driver adds the same 31 to its cursor Y. A
 * cursor drawn at the register value itself lands 31 pixels right of and
 * below the X pointer.
 */
static void mgras_load_cursor_rows(QTestState *qts, uint16_t base,
                                   const uint32_t mask[3])
{
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, base));
    for (unsigned int row = 0; row < 3; row++) {
        qtest_writew(qts, REG_VC3_RAM, mask[row] >> 16);
        qtest_writew(qts, REG_VC3_RAM, mask[row] & 0xffff);
    }
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x01, base));
}

static void test_cursor_position_origin(void)
{
    /* Row 0 marks column 0, row 1 column 11 and row 2 column 10. */
    const uint32_t mask[3] = {
        0x80000000, 0x80000000 >> 11, 0x80000000 >> 10
    };

    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    /* The cursor ink is entry 0x1c of WID 0's map, 29 under reset mode. */
    mgras_write_cmap_entry(qts, 29, 0x1c, 0x11223300);
    mgras_load_cursor_rows(qts, 0x400, mask);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x2));

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 100 + 31));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 200 + 31));
    g_assert_cmphex(mgras_test_display_pixel(qts, 100, 200), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(qts, 111, 201), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(qts, 131, 231), ==, 0);

    g_assert_cmphex(mgras_test_display_pixel(qts, 110, 202), ==, 0x00112233);

    /*
     * A glyph partly above and left of the screen keeps its visible part.
     * Row 2's bit lands at (-1, 1); drawn unclipped it would wrap to the
     * last column of row 0. Row 0 lands at y = -1, before the buffer, which
     * only a sanitizer build observes.
     */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 31 - 11));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 31 - 1));
    g_assert_cmphex(mgras_test_display_pixel(qts, 0, 0), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(qts, 1279, 0), ==, 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 100, 200), ==, 0);
}


static void test_cursor_composites_when_enabled(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));


    /* Entry bits 15:8 are blue, so this cursor ink is blue, not red. */
    mgras_write_cmap_position(qts, 0x001d, 28, 0x0000ff00);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x01, 0x0100));


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0x0100));
    qtest_writew(qts, REG_VC3_RAM, 0x8000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0x0142));
    qtest_writew(qts, REG_VC3_RAM, 0x4000);
    qtest_writew(qts, REG_VC3_RAM, 0x0000);

    /* The position registers name the glyph corner plus 31. */
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 50 + 31));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 60 + 31));


    g_assert_cmphex(mgras_test_display_pixel(qts, 50, 60), ==, 0);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0002));
    g_assert_cmphex(mgras_screendump_pixel(qts, 50, 60), ==, 0x000000ff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 50, 60), ==, 0x000000ff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 51, 61), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 52, 62), ==, 0);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x01, 0x0200));
    g_assert_cmphex(mgras_test_display_pixel(qts, 50, 60), ==, 0);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x01, 0x0100));
    g_assert_cmphex(mgras_test_display_pixel(qts, 50, 60), ==, 0x000000ff);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0x0100));
    qtest_writew(qts, REG_VC3_RAM, 0x0000);
    g_assert_cmphex(mgras_screendump_pixel(qts, 50, 60), ==, 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 50, 60), ==, 0);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0000));
    g_assert_cmphex(mgras_test_display_pixel(qts, 50, 60), ==, 0);
}

static void test_vc3_sram_round_trip(void)
{
    mgras_reset();

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(7, 0x0100));
    qtest_writew(qts, REG_VC3_RAM, 0x1234);
    qtest_writew(qts, REG_VC3_RAM, 0x5678);

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(7, 0x0100));
    g_assert_cmphex(qtest_readw(qts, REG_VC3_RAM), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_RAM), ==, 0x5678);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(7, 0x7fff));
    qtest_writew(qts, REG_VC3_RAM, 0xaaaa);
    qtest_writew(qts, REG_VC3_RAM, 0xbbbb);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(7, 0x7fff));
    g_assert_cmphex(qtest_readw(qts, REG_VC3_RAM), ==, 0xaaaa);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_RAM), ==, 0xbbbb);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(7, 0x0000));
    g_assert_cmphex(qtest_readw(qts, REG_VC3_RAM), ==, 0xbbbb);
}

static void test_vc3_register_file(void)
{
    mgras_reset();

    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x06, 0x2468));
    qtest_writeb(qts, REG_VC3_INDEX, 0x06);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, 0x2468);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 100));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 200));
    qtest_writeb(qts, REG_VC3_INDEX, 0x02);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, 100);
    qtest_writeb(qts, REG_VC3_INDEX, 0x03);
    g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, 200);


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0002));
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x0004));


    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1f, 0x01ff));
    qtest_writeb(qts, REG_VC3_INDEX, 0x1f);
    /* Bits 7:6 read as the VC3 revision, 0 on the default SI board. */
    g_assert_cmphex(qtest_readw(qts, REG_VC3_DATA), ==, 0x013f);
}

static void test_board_inventory_registers(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_BOARD_INVENTORY), ==, 0x45000000);
    g_assert_cmphex(qtest_readb(qts, REG_BOARD_INVENTORY), ==, 0x45);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_CONTROL), ==, 0x0f000000);
    qtest_writel(qts, REG_RESET_CONTROL, 0xe8000000);
    g_assert_cmphex(qtest_readl(qts, REG_RESET_CONTROL), ==, 0x0f000000);
}

/*
 * Expected identity values for each model variant: version bytes,
 * revisions, counts, and the name returned by hinv.
 */
typedef struct MgrasBoardExpect {
    const char *board;
    bool implemented;
    unsigned int ge, re, tram;
    const char *hinv;
    uint8_t version1, version0;
    uint8_t hq, ge_revision, re4, pp1, vc3, cmap, ra;
} MgrasBoardExpect;

static const MgrasBoardExpect mgras_board_expect[] = {
    { "si",    true,  1, 1, 0, "SI", 0x45, 0x0f, 2, 2, 2, 0, 0, 3, 0 },
    { "si-t",  false, 1, 1, 4, "SI with texture option",
      0x45, 0x0c, 2, 2, 2, 0, 0, 3, 0 },
    { "ssi",   true,  2, 2, 0, "SSI", 0x46, 0x0f, 2, 2, 2, 0, 0, 3, 0 },
    { "mxi",   false, 2, 2, 4, "MXI", 0x46, 0x00, 2, 2, 2, 0, 0, 3, 0 },
    { "esi",   true,  1, 1, 0, "ESI", 0x51, 0x0f, 2, 3, 2, 4, 0, 3, 0 },
    { "esi-t", false, 1, 1, 4, "ESI with texture option",
      0x51, 0x0c, 2, 3, 2, 4, 0, 3, 0 },
    { "essi",  true,  2, 2, 0, "ESSI", 0x52, 0x0f, 2, 3, 2, 4, 0, 3, 0 },
    { "emxi",  false, 2, 2, 4, "EMXI", 0x52, 0x00, 2, 3, 2, 4, 0, 3, 0 },
};

#define REG_GE_STATUS          MGRAS_REG(0x028000)
#define REG_CMAP_RA_REVISION   MGRAS_REG(0x071708)
#define REG_MONITOR_ID         MGRAS_REG(0x071308)
#define XMAP_INDEX_PP1_REVISION 4

/*
 * The name IRIX 6.5.x hinv prints for the counts the board-information routine
 * decodes from the two version bytes, and the E-series marker the kernel sets
 * from a GE revision of 3 or more. Products 2 and 3 only: RE count equals GE
 * count, and a TRAM path holds 4 TRAMs unless its field is 3.
 */
static const char *mgras_decode_hinv_name(uint8_t version1, uint8_t version0,
                                          uint8_t ge_revision,
                                          unsigned int *ge, unsigned int *re,
                                          unsigned int *tram0,
                                          unsigned int *tram1)
{
    unsigned int product = (version1 >> 5) & 3;
    bool e_series = ge_revision >= 3;

    g_assert_true(product == 2 || product == 3);
    *ge = version1 & 3;
    *re = *ge;
    *tram0 = (version0 & 3) == 3 ? 0 : 4;
    *tram1 = ((version0 >> 2) & 3) == 3 ? 0 : 4;
    /*
     * With two GEs the kernel logs a TRAM mismatch unless both paths hold
     * the same count; name that case so the caller's comparison fails.
     */
    if (*ge == 2 && *tram0 != *tram1) {
        return "TRAM mismatch";
    }
    if (*ge == 1 && *re == 1) {
        if (*tram0) {
            return e_series ? "ESI with texture option" :
                   "SI with texture option";
        }
        return e_series ? "ESI" : "SI";
    }
    if (*ge == 2 && *re == 2) {
        if (*tram0) {
            return e_series ? "EMXI" : "MXI";
        }
        return e_series ? "ESSI" : "SSI";
    }
    return "Impact";
}

static QTestState *mgras_board_init(const MgrasBoardExpect *e)
{
    g_autofree char *args = g_strdup_printf(
        MACHINE_ARGS " -global sgi-mgras.board=%s%s", e->board,
        e->implemented ? "" : " -global sgi-mgras.x-unimplemented-board=on");

    return qtest_init(args);
}

/*
 * Every identity read the IRIX kernel makes, at the widths it makes them,
 * answers the documented value for the selected board, and those values
 * decode to the board's counts and hinv name.
 */
static void test_board_identity(gconstpointer data)
{
    const MgrasBoardExpect *e = data;
    QTestState *s = mgras_board_init(e);
    uint8_t version1, version0, ge_revision;
    unsigned int ge, re, tram0, tram1;
    const char *name;

    version1 = qtest_readl(s, REG_BOARD_INVENTORY) >> 24;
    version0 = qtest_readl(s, REG_RESET_CONTROL) >> 24;
    g_assert_cmphex(version1, ==, e->version1);
    g_assert_cmphex(qtest_readb(s, REG_BOARD_INVENTORY), ==, e->version1);
    g_assert_cmphex(version0, ==, e->version0);
    g_assert_cmphex(qtest_readl(s, REG_BOARD_INVENTORY) & 0x00ffffff, ==, 0);
    g_assert_cmphex(qtest_readl(s, REG_RESET_CONTROL) & 0x00ffffff, ==, 0);

    g_assert_cmphex(qtest_readl(s, MGRAS_REG(0x000000)) >> 28, ==, e->hq);

    /* The kernel reads 0x028000 first and keeps only 0x028004's low byte. */
    g_assert_cmphex(qtest_readl(s, REG_GE_STATUS), ==, 0);
    ge_revision = qtest_readl(s, REG_GE_REVISION) & 0xff;
    g_assert_cmphex(ge_revision, ==, e->ge_revision);

    g_assert_cmphex((qtest_readl(s, REG_DIRECT_COPY_READY) >> 4) & 0xf, ==,
                    e->re4);

    qtest_writeb(s, REG_XMAP_INDEX, XMAP_INDEX_PP1_REVISION);
    g_assert_cmphex(qtest_readl(s, REG_REPP_STATUS_3) & 7, ==, e->pp1);

    qtest_writeb(s, REG_VC3_INDEX, 0x1f);
    g_assert_cmphex((qtest_readw(s, REG_VC3_DATA) >> 6) & 3, ==, e->vc3);

    g_assert_cmphex(qtest_readb(s, REG_CMAP_RA_REVISION) & 7, ==, e->cmap);
    g_assert_cmphex((qtest_readb(s, REG_CMAP_RA_REVISION) >> 4) & 7, ==,
                    e->ra);
    /* Set: no video option board, as on every captured Origin board. */
    g_assert_cmphex(qtest_readb(s, REG_CMAP_RA_REVISION) & 0x80, ==, 0x80);
    g_assert_cmphex(qtest_readl(s, REG_CMAP_RA_REVISION), ==,
                    (uint32_t)qtest_readb(s, REG_CMAP_RA_REVISION) << 24);

    /*
     * The monitor ID in bits 7:4 belongs to the attached monitor, 0xf on
     * every captured Origin board, so every board answers alike. The
     * kernel keeps (byte >> 4) & 0xf; 0xf, like 0, picks its stock 60 Hz
     * timing record. Bits 2:0 are the first CMAP's revision, which the IDE
     * requires to match the second's at 0x071708.
     */
    g_assert_cmphex(qtest_readb(s, REG_MONITOR_ID), ==, 0xf0 | e->cmap);
    g_assert_cmphex(qtest_readl(s, REG_MONITOR_ID), ==,
                    (0xf0u | e->cmap) << 24);

    name = mgras_decode_hinv_name(version1, version0, ge_revision, &ge, &re,
                                  &tram0, &tram1);
    g_assert_cmpuint(ge, ==, e->ge);
    g_assert_cmpuint(re, ==, e->re);
    g_assert_cmpuint(tram0, ==, e->tram);
    /* One path on a one-GE board; both alike on a two-GE board. */
    g_assert_cmpuint(tram1, ==, e->ge == 2 ? e->tram : 0);
    g_assert_cmpstr(name, ==, e->hinv);

    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/* One MicroLAN bit slot on the board's chip; returns the sampled level. */
static bool board_nic_slot(QTestState *s, unsigned int pulse,
                           unsigned int sample)
{
    uint32_t result;

    qtest_writel(s, REG_BRINGUP_STEP, (pulse << 10) | (sample << 2));
    result = qtest_readl(s, REG_BRINGUP_STEP);
    g_assert_cmphex(result & 0x2, ==, 0x2);
    g_assert_cmphex(result & 0x000ffffc, ==, (pulse << 10) | (sample << 2));
    return result & 1;
}

static void board_nic_write_byte(QTestState *s, uint8_t value)
{
    for (unsigned int bit = 0; bit < 8; bit++) {
        board_nic_slot(s, value & (1u << bit) ? 6 : 80,
                       value & (1u << bit) ? 13 : 30);
    }
}

static uint8_t board_nic_read_byte(QTestState *s)
{
    uint8_t value = 0;

    for (unsigned int bit = 0; bit < 8; bit++) {
        value |= board_nic_slot(s, 6, 13) << bit;
    }
    return value;
}

typedef struct MgrasBoardRecordExpect {
    const char *board;
    const char *name;
    const char *part;
    const char *revision;
} MgrasBoardRecordExpect;

/* The board names and parts published Octane and Origin inventories show. */
static const MgrasBoardRecordExpect mgras_board_record_expect[] = {
    { "si",    "GM10",  "030-0938-003", "U" },
    { "si-t",  "GM10",  "030-0938-003", "U" },
    { "ssi",   "GM20",  "030-0957-003", "N" },
    { "mxi",   "GM20",  "030-0957-003", "N" },
    { "esi",   "MOT10", "030-1241-002", "J" },
    { "esi-t", "MOT10", "030-1241-002", "J" },
    { "essi",  "MOT20", "030-1240-003", "G" },
    { "emxi",  "MOT20", "030-1240-003", "G" },
};

/*
 * With its record fitted, the chip behind 0x011098 answers a reset with
 * presence, its ROM with the DS2505 family and the configured serial, and
 * its memory with the variant's board name and part at the record's fixed
 * offsets, which is where the PROM's "Name:" text comes from.
 */
static void test_board_record(gconstpointer data)
{
    const MgrasBoardRecordExpect *e = data;
    g_autofree char *args = g_strdup_printf(
        MACHINE_ARGS " -global sgi-mgras.board=%s"
        " -global sgi-mgras.x-unimplemented-board=on"
        " -global sgi-mgras.board-record=on"
        " -global sgi-mgras.board-record-barcode=SIMTEST001"
        " -global sgi-mgras.board-record-serial=0x123456789abc", e->board);
    QTestState *s = qtest_init(args);
    uint8_t rom[8], record[64];
    char text[16];

    g_assert_false(board_nic_slot(s, 500, 65));
    board_nic_write_byte(s, 0x33);
    for (unsigned int i = 0; i < sizeof(rom); i++) {
        rom[i] = board_nic_read_byte(s);
    }
    g_assert_cmphex(rom[0], ==, 0x0b);
    g_assert_cmphex(rom[1], ==, 0xbc);
    g_assert_cmphex(rom[6], ==, 0x12);

    g_assert_false(board_nic_slot(s, 500, 65));
    board_nic_write_byte(s, 0xcc);
    board_nic_write_byte(s, 0xf0);
    board_nic_write_byte(s, 0);
    board_nic_write_byte(s, 0);
    for (unsigned int i = 0; i < sizeof(record); i++) {
        record[i] = board_nic_read_byte(s);
    }
    g_assert_cmphex(record[0], ==, 0x01);
    g_assert_cmpmem(record + 1, 10, "SIMTEST001", 10);
    g_assert_cmpmem(record + 21, 9, e->part, 9);
    g_assert_cmpmem(record + 35, 3, e->part + 9, 3);
    g_assert_cmpuint(record[38], ==, e->revision[0]);
    memset(text, ' ', sizeof(text));
    memcpy(text, e->name, strlen(e->name));
    g_assert_cmpmem(record + 48, 14, text, 14);

    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/* Without a record the register keeps its older always-done answer. */
static void test_board_record_absent(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS);

    qtest_writel(s, REG_BRINGUP_STEP, (500u << 10) | (65u << 2));
    g_assert_cmphex(qtest_readl(s, REG_BRINGUP_STEP), ==, 0x2);
    qtest_quit(s);
}

/*
 * Select an RE for direct access: a raster write to RSS register 0x112.
 * The caller enters a context first.
 */
static void board_select_re(QTestState *s, uint32_t value)
{
    qtest_writeq(s, REG_CONTEXT_COMMAND, MGRAS_COMMAND(0x112, value));
}

/*
 * The RE4 status register reports the RE that register 0x112 bits 1:0
 * select in bits 3:2 and the board's RE count less one in bits 1:0, which
 * is what the IDE's status test expects after it selects each RE.
 */
static void test_board_re4_status_reports_selected_re(gconstpointer data)
{
    const MgrasBoardExpect *e = data;
    QTestState *s = mgras_board_init(e);

    mgras_enter_context(s);

    g_assert_cmphex(qtest_readl(s, REG_DIRECT_COPY_READY) & 0xf, ==,
                    e->re - 1);
    for (uint32_t re = 0; re < e->re; re++) {
        board_select_re(s, re);
        g_assert_cmphex(qtest_readl(s, REG_DIRECT_COPY_READY) & 0xf, ==,
                        re << 2 | (e->re - 1));
        /* Bit 2 of the written value does not select anything. */
        board_select_re(s, re | 4);
        g_assert_cmphex(qtest_readl(s, REG_DIRECT_COPY_READY) & 0xf, ==,
                        re << 2 | (e->re - 1));
    }
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/*
 * The IDE's raster-register writer: RSS register 0x112 through the mapping
 * port with class 0x59 (the main port's 0x19 and token bit 22), then its
 * data word, selects the RE just as the main port's write does.
 *
 * Only the multi-RE variants (ssi, essi) pin the forwarding. A board with
 * one RE can only select RE 0, which is the reset selection, so the si case
 * passes without it and checks just that the token raises no error.
 */
static void test_board_mapping_port_raster_write(gconstpointer data)
{
    const MgrasBoardExpect *e = data;
    QTestState *s = mgras_board_init(e);

    mgras_enter_context(s);
    for (uint32_t re = 0; re < e->re; re++) {
        qtest_writel(s, MGRAS_REG(0x130400),
                     (((0x112 | 0x1800) & 0x1fff) << 8) | 0x00400004);
        qtest_writel(s, MGRAS_REG(0x130400), re);
        g_assert_cmphex(qtest_readl(s, REG_DIRECT_COPY_READY) & 0xf, ==,
                        re << 2 | (e->re - 1));
    }
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/* Selecting an RE leaves the revision in bits 7:4 and the ready bit 8. */
static void test_board_re4_status_keeps_revision_bits(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(mgras_board_expect); i++) {
        const MgrasBoardExpect *e = &mgras_board_expect[i];
        QTestState *s;

        if (!e->implemented) {
            continue;
        }
        s = mgras_board_init(e);
        mgras_enter_context(s);
        for (uint32_t re = 0; re < e->re; re++) {
            board_select_re(s, re);
            g_assert_cmphex((qtest_readl(s, REG_DIRECT_COPY_READY) >> 4) & 0xf,
                            ==, e->re4);
            g_assert_cmphex(qtest_readl(s, REG_DIRECT_COPY_READY) & 0x100, ==,
                            0x100);
        }
        qtest_quit(s);
    }
}

/*
 * The RDRAM data port answers for the RE that register 0x112 selects, with
 * the same word for each RE the board has (an approximation the kernel's
 * per-RE loops accept). An RE the board lacks does not answer.
 */
static void test_board_rdram_data_same_for_each_re(gconstpointer data)
{
    const MgrasBoardExpect *e = data;
    QTestState *s = mgras_board_init(e);

    mgras_enter_context(s);

    for (uint32_t re = 0; re < 4; re++) {
        board_select_re(s, re);
        g_assert_cmphex(qtest_readl(s, MGRAS_REG(0x02c574)), ==,
                        re < e->re ? 0x8 : 0);
    }
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/*
 * The board's RE count, not a separate switch, decides how many PP1s answer
 * behind PP1SELECT: two per RE. A write
 * to every PP1 reaches each one the board has, and each reports the board's
 * PP1 revision; a PP1 past the board's count reads 0.
 */
static void test_board_pp1s_follow_re_count(gconstpointer data)
{
    const MgrasBoardExpect *e = data;
    QTestState *s = mgras_board_init(e);

    mgras_select_xmap(s, PP1_SELECT_ALL, 0);
    qtest_writel(s, REG_XMAP_MAIN_MODE, 0x111);
    for (unsigned int n = 0; n < 4; n++) {
        bool present = n < 2 * e->re;

        mgras_select_xmap(s, mgras_pp1_select_one(n), 0);
        g_assert_cmphex(qtest_readl(s, REG_XMAP_MAIN_MODE), ==,
                        present ? 0x111 : 0);
        qtest_writeb(s, REG_XMAP_INDEX, XMAP_INDEX_PP1_REVISION);
        g_assert_cmphex(qtest_readl(s, REG_REPP_STATUS_3) & 7, ==,
                        present ? e->pp1 : 0);
    }
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

/*
 * 0x028004 carries the GE revision from reset until the first 0xa2 query,
 * the kernel's or a client's, which replaces it with the answer; a reset
 * brings the revision back. The query is the kernel's finish-fence routine
 * fence, index 0x1fa, which the model answers with zero. Uses the E-series
 * board so the revision cannot be mistaken for the answer.
 */
static void test_board_ge_revision_until_query(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS " -global sgi-mgras.board=esi");

    g_assert_cmphex(qtest_readl(s, REG_GE_REVISION), ==, 3);
    g_assert_cmphex(qtest_readl(s, REG_GE_REVISION), ==, 3);

    mgras_enter_context(s);
    qtest_writel(s, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_QUERY);
    qtest_writel(s, REG_CONTEXT_COMMAND, MGRAS_QUERY_TOKEN);
    qtest_writel(s, REG_CONTEXT_COMMAND, 0x1fa);
    qtest_writel(s, REG_CONTEXT_COMMAND, 1);
    g_assert_cmphex(qtest_readl(s, REG_GE_REVISION), ==, 3);
    qtest_writel(s, REG_CONTEXT_COMMAND, 1);
    g_assert_cmphex(qtest_readl(s, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    MGRAS_HQ_FLAG_QUERY);
    g_assert_cmphex(qtest_readl(s, REG_GE_REVISION), ==, 0);

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readl(s, REG_GE_REVISION), ==, 3);
    qtest_quit(s);
}

/*
 * Without a board property the model is the textureless SI, the board
 * every existing Origin 2000 run has used.
 */
static void test_board_default_is_si(void)
{
    mgras_reset();

    g_assert_cmphex(qtest_readl(qts, REG_BOARD_INVENTORY), ==, 0x45000000);
    g_assert_cmphex(qtest_readl(qts, REG_GE_REVISION), ==, 2);
}

/*
 * A rejection reports through error_report and exits; it must not reach
 * the guest as a signal (SIGABRT from a failed assertion, SIGSEGV from a
 * wild pointer). WIFSIGNALED tells the two apart where a bare nonzero
 * exit status cannot. Reimplemented from tests/qtest/sn-builder-test.c's
 * own assert_qemu_rejects(): a qtest binary cannot link another qtest
 * file's static helper.
 */
static void assert_qemu_rejects(const char *const args[], const char *message)
{
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                               NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_false(WIFSIGNALED(wait_status));
    g_assert_nonnull(strstr(error_output, message));
}

/*
 * A board with TRAM is refused at realize, naming texture as what the model
 * lacks, unless x-unimplemented-board asks for it. "-qtest
 * stdio -accel qtest" gets past the machine's stock-PROM-only check the
 * same way sn0-origin2000-kona-test.c's rejection cases do.
 */
static void test_board_refused(gconstpointer data)
{
    const MgrasBoardExpect *e = data;
    g_autofree char *global = g_strdup_printf("sgi-mgras.board=%s", e->board);
    g_autofree char *message = g_strdup_printf("board '%s' (%s) needs",
                                               e->board, e->hinv);
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000", "-global", global,
        "-device", "sgi-mgras,slot=io3",
        "-display", "none", "-qtest", "stdio", "-accel", "qtest", NULL
    };

    /* Texture is the only thing any board still lacks, in part. */
    g_assert_cmpuint(e->tram, >, 0);
    assert_qemu_rejects(args, message);
    assert_qemu_rejects(args, "needs texture beyond the subset its datasheet "
                        "lists (TE1 and TRAM), which");
}

/*
 * Scanout builds each row's window IDs in stack buffers of
 * SGI_MGRAS_MAX_WIDTH entries, and realize refuses a descriptor that names
 * a wider screen rather than let scanout run past them. Every descriptor
 * states 1280x1024, so every board realizes, and a fill of the whole
 * screen reaches the display at both ends of the scanned rows.
 */
static void test_board_screen_fits_row_buffers(gconstpointer data)
{
    const MgrasBoardExpect *e = data;
    QTestState *s = mgras_board_init(e);

    mgras_enter_context(s);
    qtest_writel(s, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(s, 29, 0x40, 0x11223300);
    qtest_writeq(s, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(s, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLCOLOR_R, 0x40));
    qtest_writeq(s, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_BLOCK_START, 0));
    qtest_writeq(s, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (1279u << 16) | 1023u));
    g_assert_cmphex(mgras_test_display_pixel(s, 0, 0), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(s, 1279, 1023), ==, 0x00112233);
    /* Past the screen there is no pixel. */
    g_assert_cmphex(mgras_test_display_pixel(s, 1280, 1023), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

static void test_board_unknown_refused(void)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin2000",
        "-global", "sgi-mgras.board=vpro",
        "-device", "sgi-mgras,slot=io3",
        "-display", "none", "-qtest", "stdio", "-accel", "qtest", NULL
    };

    assert_qemu_rejects(args, "unknown board 'vpro'");
}


static void test_int_dest_alt_offset_aliases(void)
{
    mgras_reset();

    qtest_writel(qts, REG_INT_DEST_UPPER_ALT, 0x12340000);
    qtest_writel(qts, REG_INT_DEST_LOWER_ALT, 0x5678);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_UPPER), ==, 0x12340000);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_LOWER), ==, 0x5678);

    qtest_writel(qts, REG_INT_DEST_UPPER, 0xabcd0000);
    qtest_writel(qts, REG_INT_DEST_LOWER, 0xef01);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_UPPER_ALT), ==, 0xabcd0000);
    g_assert_cmphex(qtest_readl(qts, REG_INT_DEST_LOWER_ALT), ==, 0xef01);
}


static void test_hq_config_and_enable_registers(void)
{
    mgras_reset();

    qtest_writel(qts, REG_HQ_CONFIG_A, 0x11000f);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_CONFIG_A), ==, 0x11000f);
    qtest_writel(qts, REG_HQ_CONFIG_B, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_CONFIG_B), ==, 0xffffffff);

    qtest_writel(qts, REG_HQ_ENABLE, 0x1f8);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_ENABLE), ==, 0x1f8);

    qtest_writel(qts, REG_HQ_ENABLE, (0x1f8 & ~8) | 0x10);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_ENABLE), ==, 0x1f0);

    qtest_writel(qts, REG_HQ_ENABLE_CLEAR, 0x10);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_ENABLE), ==, 0x1e0);


    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 0x80000000, ==, 0);
}


static void test_hq_vector_registers(void)
{
    mgras_reset();

    qtest_writel(qts, REG_HQ_VECTOR_DEST_UPPER, 0x2000f000);
    qtest_writel(qts, REG_HQ_VECTOR_DEST_LOWER, 0x9000);
    qtest_writel(qts, REG_HQ_VECTORS, 0x01020304);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_VECTOR_DEST_UPPER), ==,
                     0x2000f000);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_VECTOR_DEST_LOWER), ==, 0x9000);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_VECTORS), ==, 0x01020304);
}
int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init(MACHINE_ARGS);


    qtest_add_func("sn0/origin2000/mgras/identity-and-aperture",
                   test_identity_and_aperture);
    qtest_add_func("sn0/origin2000/mgras/widget-reserved-halves-raz-wi",
                   test_widget_reserved_halves_raz_wi);
    qtest_add_func("sn0/origin2000/mgras/widget-control-is-plain-storage",
                   test_widget_control_is_plain_storage);
    qtest_add_func("sn0/origin2000/mgras/generic-bringup-step",
                   test_generic_bringup_step);
    qtest_add_func("sn0/origin2000/mgras/bus-status-reads-settled",
                   test_bus_status_reads_settled);
    qtest_add_func("sn0/origin2000/mgras/reset-registers-are-plain-storage",
                   test_reset_registers_are_plain_storage);
    qtest_add_func("sn0/origin2000/mgras/i2c-probe-loopback",
                   test_i2c_probe_loopback);
    qtest_add_func("sn0/origin2000/mgras/i2c-strobe-byte-read",
                   test_i2c_strobe_byte_read);
    qtest_add_func("sn0/origin2000/mgras/repp-sync-handshake",
                   test_repp_sync_handshake);
    qtest_add_func("sn0/origin2000/mgras/repp-status-byte-writes",
                   test_repp_status_byte_writes);
    qtest_add_func("sn0/origin2000/mgras/xmap-buf-select-reads-back",
                   test_xmap_buf_select_reads_back);
    qtest_add_func("sn0/origin2000/mgras/pp1-select-routes-xmap-two-re",
                   test_pp1_select_routes_xmap_on_two_re_board);
    qtest_add_func("sn0/origin2000/mgras/pp1-select-absent-pp1-one-re",
                   test_pp1_select_absent_pp1_on_one_re_board);
    qtest_add_func("sn0/origin2000/mgras/pp1-scanout-follows-first-pp1",
                   test_pp1_scanout_follows_first_pp1);
    qtest_add_func("sn0/origin2000/mgras/pp1-select-all-reads-first-pp1",
                   test_pp1_select_all_reads_first_pp1);
    qtest_add_func("sn0/origin2000/mgras/pp1-select-ignores-bits-7-to-3",
                   test_pp1_select_ignores_bits_7_to_3);
    qtest_add_func("sn0/origin2000/mgras/"
                   "pp1-select-leaves-sync-registers-shared",
                   test_pp1_select_leaves_sync_registers_shared);
    qtest_add_func("sn0/origin2000/mgras/two-re-board-draws-one-set-of-planes",
                   test_two_re_board_draws_one_set_of_planes);
    qtest_add_func("sn0/origin2000/mgras/xmap-revision-and-fifo-capacity",
                   test_xmap_revision_and_fifo_capacity);
    qtest_add_func("sn0/origin2000/mgras/xmap-buf-select-write",
                   test_xmap_buf_select_write);
    qtest_add_func("sn0/origin2000/mgras/visual-register-read-widths",
                   test_visual_register_read_widths);
    qtest_add_func("sn0/origin2000/mgras/visual-register-write-widths",
                   test_visual_register_write_widths);
    qtest_add_func("sn0/origin2000/mgras/hq-status-bringup-bit",
                   test_hq_status_bringup_bit);
    qtest_add_func("sn0/origin2000/mgras/hq-status-hiwater-poll-bit",
                   test_hq_status_hiwater_poll_bit);
    qtest_add_func("sn0/origin2000/mgras/hq-status-independent-of-error",
                   test_hq_status_independent_of_sticky_error);
    qtest_add_func("sn0/origin2000/mgras/hq-status-re-sync-after-request",
                   test_hq_status_re_sync_after_request);
    qtest_add_func("sn0/origin2000/mgras/dac-address-byte-port",
                   test_dac_address_byte_port);
    qtest_add_func("sn0/origin2000/mgras/dac-register-file-matches-ide-reset",
                   test_dac_register_file_matches_ide_reset);
    qtest_add_func("sn0/origin2000/mgras/vc3-registers-read-back",
                   test_vc3_registers_read_back);
    qtest_add_func("sn0/origin2000/mgras/dac-lut-load",
                   test_dac_lut_load);
    qtest_add_func("sn0/origin2000/mgras/dac-gamma-identity-ramp-noop",
                   test_dac_gamma_identity_ramp_leaves_colors_unchanged);
    qtest_add_func("sn0/origin2000/mgras/dac-gamma-curve-applies-to-scanout",
                   test_dac_gamma_curve_applies_to_scanout);
    qtest_add_func("sn0/origin2000/mgras/write-path-fixes-dont-raise-error",
                   test_write_path_fixes_dont_raise_the_sticky_error);
    qtest_add_func("sn0/origin2000/mgras/board-bringup-scratch-registers",
                   test_board_bringup_scratch_registers);
    qtest_add_func("sn0/origin2000/mgras/vc3-arg-byte-write",
                   test_vc3_arg_byte_write);
    qtest_add_func("sn0/origin2000/mgras/board-bringup-byte-poll-bit-position",
                   test_board_bringup_byte_poll_bit_position);
    qtest_add_func("sn0/origin2000/mgras/unresolved-revision-byte-read",
                   test_unresolved_revision_byte_read);
    qtest_add_func("sn0/origin2000/mgras/board-info-byte-read-no-error",
                   test_board_info_byte_read_does_not_raise_error);
    qtest_add_func("sn0/origin2000/mgras/hq-upload-readback",
                   test_hq_upload_and_readback);
    qtest_add_func("sn0/origin2000/mgras/reset-clears-context-acks",
                   test_reset_clears_context_acknowledgements);
    qtest_add_func("sn0/origin2000/mgras/cursor-position-origin",
                   test_cursor_position_origin);
    qtest_add_func("sn0/origin2000/mgras/cursor-composites-when-enabled",
                   test_cursor_composites_when_enabled);
    qtest_add_func("sn0/origin2000/mgras/vc3-sram-round-trip",
                   test_vc3_sram_round_trip);
    qtest_add_func("sn0/origin2000/mgras/vc3-register-file",
                   test_vc3_register_file);
    qtest_add_func("sn0/origin2000/mgras/board-default-is-si",
                   test_board_default_is_si);
    qtest_add_func("sn0/origin2000/mgras/board-ge-revision-until-query",
                   test_board_ge_revision_until_query);
    qtest_add_func("sn0/origin2000/mgras/board-unknown-refused",
                   test_board_unknown_refused);
    qtest_add_func("sn0/origin2000/mgras/dac-mode-byte-readback",
                   test_dac_mode_byte_readback);
    qtest_add_func("sn0/origin2000/mgras/board-record-absent",
                   test_board_record_absent);
    for (size_t i = 0; i < ARRAY_SIZE(mgras_board_record_expect); i++) {
        const MgrasBoardRecordExpect *e = &mgras_board_record_expect[i];
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/mgras/board-record/%s", e->board);

        qtest_add_data_func(path, e, test_board_record);
    }
    for (size_t i = 0; i < ARRAY_SIZE(mgras_board_expect); i++) {
        const MgrasBoardExpect *e = &mgras_board_expect[i];
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/mgras/board-identity/%s", e->board);
        g_autofree char *screen = g_strdup_printf(
            "sn0/origin2000/mgras/board-screen-fits-row-buffers/%s",
            e->board);

        g_autofree char *pp1s = g_strdup_printf(
            "sn0/origin2000/mgras/board-pp1s-follow-re-count/%s", e->board);

        qtest_add_data_func(path, e, test_board_identity);
        qtest_add_data_func(screen, e, test_board_screen_fits_row_buffers);
        qtest_add_data_func(pp1s, e, test_board_pp1s_follow_re_count);
        if (e->implemented && (!strcmp(e->board, "si") ||
                               !strcmp(e->board, "ssi"))) {
            g_autofree char *rdram = g_strdup_printf(
                "sn0/origin2000/mgras/rdram-data-same-for-each-re/%s",
                e->board);

            qtest_add_data_func(rdram, e,
                                test_board_rdram_data_same_for_each_re);
        }
        if (e->implemented && (!strcmp(e->board, "si") ||
                               !strcmp(e->board, "ssi") ||
                               !strcmp(e->board, "essi"))) {
            g_autofree char *re4 = g_strdup_printf(
                "sn0/origin2000/mgras/re4-status-reports-selected-re/%s",
                e->board);
            g_autofree char *mapping = g_strdup_printf(
                "sn0/origin2000/mgras/mapping-port-raster-write/%s",
                e->board);

            qtest_add_data_func(re4, e,
                                test_board_re4_status_reports_selected_re);
            qtest_add_data_func(mapping, e,
                                test_board_mapping_port_raster_write);
        }
        if (!e->implemented) {
            g_autofree char *refused = g_strdup_printf(
                "sn0/origin2000/mgras/board-refused/%s", e->board);

            qtest_add_data_func(refused, e, test_board_refused);
        }
    }
    qtest_add_func("sn0/origin2000/mgras/re4-status-keeps-revision-bits",
                   test_board_re4_status_keeps_revision_bits);
    qtest_add_func("sn0/origin2000/mgras/board-inventory-registers",
                   test_board_inventory_registers);
    qtest_add_func("sn0/origin2000/mgras/int-dest-alt-offset-aliases",
                   test_int_dest_alt_offset_aliases);
    qtest_add_func("sn0/origin2000/mgras/hq-config-and-enable-registers",
                   test_hq_config_and_enable_registers);
    qtest_add_func("sn0/origin2000/mgras/hq-vector-registers",
                   test_hq_vector_registers);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
