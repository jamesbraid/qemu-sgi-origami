/*
 * SI Viz register-contract and protocol qtests: migration cases.
 *
 * Unsupported behavior remains an approximation unless the public MGRAS
 * contracts specify it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
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


/*
 * One retrace migration: the source runs src until half-way through its
 * third frame; the destination must fire that retrace where the source
 * would have, then run dst.
 */
static void mgras_retrace_migration(bool programmed, bool stale_table,
                                    const MgrasFrameTime *src,
                                    const MgrasFrameTime *dst)
{
    const unsigned int retrace_vector = 54;
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    int64_t reset_at, dest_now;

    tmpfs = g_dir_make_tmp("sn0-mgras-retrace-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    /* An offset phase, so a destination that restarts it is caught. */
    reset_at = qtest_clock_step(from, 12345);
    qtest_system_reset(from);
    if (programmed) {
        mgras_load_vga_timing(from);
    }
    mgras_retrace_to_hub_on(from, retrace_vector);
    if (stale_table) {
        /*
         * Word 0x1000 is empty; without an enable write the model keeps
         * running the period it took from the table at 0x380.
         */
        qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x00, 0x1000));
    }
    mgras_expect_retraces(from, src, 1, 2, retrace_vector);
    qtest_clock_step(from, mgras_retrace_gap(src, 3) / 2);

    sgi_migrate_stopped(from, uri);

    /*
     * qtest cannot step by zero; one nanosecond reads the clock. The
     * destination counts its frames from one period before the loaded
     * retrace, so the gaps after it are those after the first.
     */
    dest_now = qtest_clock_step(to, 1);
    mgras_expect_retrace_in(to,
                            reset_at + mgras_retrace_at(src, 3) - dest_now,
                            retrace_vector);
    g_assert_cmpuint(mgras_qom_get_uint(to, "x-test-retraces"), ==, 3);
    mgras_expect_retraces(to, dst, 2, 3, retrace_vector);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * Across two binaries, QTEST_QEMU_BINARY_SRC to QTEST_QEMU_BINARY_DST,
 * either of which may run the older exact 60 Hz period, check what does
 * not depend on the period: the stream loads, the retrace count travels,
 * and the pending retrace lands between the two models' third retrace.
 */
static void mgras_retrace_cross_binary_migration(void)
{
    const unsigned int retrace_vector = 54;
    const int64_t sixtieth = NANOSECONDS_PER_SECOND / 60;
    const MgrasFrameTime *t = &mgras_stock_frame;
    const uint64_t bit = UINT64_C(1) << retrace_vector;
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    int64_t reset_at, dest_now;

    tmpfs = g_dir_make_tmp("sn0-mgras-retrace-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init_ext("QTEST_QEMU_BINARY_SRC", MIGRATION_ARGS, NULL,
                          true);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init_ext("QTEST_QEMU_BINARY_DST", dest_cmdline, NULL, true);

    reset_at = qtest_clock_step(from, 12345);
    qtest_system_reset(from);
    mgras_retrace_to_hub_on(from, retrace_vector);
    qtest_clock_step(from, 5 * sixtieth / 2);
    g_assert_cmpuint(mgras_qom_get_uint(from, "x-test-retraces"), ==, 2);

    sgi_migrate_stopped(from, uri);

    dest_now = qtest_clock_step(to, 1);
    mgras_hub_clear(to, retrace_vector);
    qtest_clock_step(to, reset_at + 3 * sixtieth - 1 - dest_now);
    g_assert_cmpuint(mgras_qom_get_uint(to, "x-test-retraces"), ==, 2);
    g_assert_cmphex(mgras_hub_pending(to) & bit, ==, 0);
    qtest_clock_step(to, mgras_retrace_at(t, 3) - 3 * sixtieth + 1);
    g_assert_cmpuint(mgras_qom_get_uint(to, "x-test-retraces"), ==, 3);
    g_assert_cmphex(mgras_hub_pending(to) & bit, ==, bit);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void test_retrace_phase_migration(void)
{
    if (g_getenv("QTEST_QEMU_BINARY_SRC") ||
        g_getenv("QTEST_QEMU_BINARY_DST")) {
        mgras_retrace_cross_binary_migration();
        return;
    }
    mgras_retrace_migration(false, false, &mgras_stock_frame,
                            &mgras_stock_frame);
}

/* The destination takes the period from the loaded VC3 and DAC state. */
static void test_retrace_programmed_period_migration(void)
{
    mgras_retrace_migration(true, false, &mgras_vga_frame,
                            &mgras_vga_frame);
}

/*
 * The guest moved VID_EP to another table after its last enable write, so
 * the migrated state no longer gives the period the source runs. The
 * stream carries that period, and the destination keeps it.
 */
static void test_retrace_running_period_migration(void)
{
    mgras_retrace_migration(true, true, &mgras_vga_frame,
                            &mgras_vga_frame);
}

static void mgras_public_pending_migration(bool wide)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-public-glyph-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    mgras_public_setup(from, true);
    mgras_public_rect(from, 31, 43, wide ? 12 : 5, wide ? 1 : 2, true);
    mgras_public_write(from, 0x70, wide ? 0x81240000 : 0x88000000, !wide);
    sgi_migrate_stopped(from, uri);
    g_assert_cmphex(mgras_test_pixel(to, 31, 43), ==, wide ? 0 : 0x112233);
    g_assert_cmphex(mgras_test_pixel(to, 31, 44), ==, 0);
    mgras_public_write(to, wide ? 0x71 : 0x70,
                       wide ? 0x24000000 : 0x50000000, true);
    for (unsigned i = 0; i < (wide ? 12 : 5); i++) {
        bool set = wide ? i == 0 || i == 7 || i == 10 : i == 1 || i == 3;
        g_assert_cmphex(mgras_test_pixel(to, 31 + i, wide ? 43 : 44), ==,
                        set ? 0x112233 : 0xa0b0d0);
    }
    mgras_public_write(to, wide ? 0x71 : 0x70, 0xff000000, true);
    g_assert_cmphex(mgras_test_pixel(to, 32, wide ? 43 : 44), ==,
                    wide ? 0xa0b0d0 : 0x112233);
    g_assert_cmphex(mgras_test_pixel(to, 31, wide ? 44 : 45), ==, 0);
    qtest_quit(to);
    qtest_quit(from);
    rmdir(tmpfs);
}

static void test_public_fifo_glyph_narrow_migration(void)
{
    mgras_public_pending_migration(false);
}

static void test_public_fifo_glyph_wide_migration(void)
{
    mgras_public_pending_migration(true);
}

static void test_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    mgras_enter_context(from);

    /*
     * A query replaces the GE revision (2 on the default SI board) at
     * 0x028004 with its answer, here element 0 of the default context's
     * identity modelview matrix. The destination starts from reset, so it
     * reads 2 unless that word travels with the stream.
     */
    g_assert_cmphex(qtest_readl(to, REG_GE_REVISION), ==, 2);
    qtest_writel(from, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_QUERY);
    qtest_writel(from, REG_CONTEXT_COMMAND, MGRAS_QUERY_TOKEN);
    qtest_writel(from, REG_CONTEXT_COMMAND, 0x3a);
    qtest_writel(from, REG_CONTEXT_COMMAND, 0);
    qtest_writel(from, REG_CONTEXT_COMMAND, 1);
    g_assert_cmphex(qtest_readl(from, REG_GE_REVISION), ==, 0x3f800000);

    qtest_writel(from, REG_GE_CTRL_A, 0x200000);
    qtest_writel(from, REG_GE_DATA_A, 0x11223344);
    qtest_writel(from, REG_GE_DATA_A, 0x55667788);
    qtest_writel(from, REG_GE_CTRL_A, 0x80000002);
    g_assert_cmphex(qtest_readl(from, REG_GE_READ_2), ==, 0x55667788);
    g_assert_cmphex(qtest_readl(from, REG_GE_READ_1), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(from, REG_GE_READ_1), ==, 0);
    g_assert_cmphex(qtest_readl(from, REG_GE_READ_1), ==, 0);

    mgras_write_cmap_entry(from, 29, 0x66, 0x66554400);
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0x66));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (2u << 16) | 2u));

    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    qtest_writew(from, REG_DAC_LUT_INDEX, 0x0600);
    qtest_writeb(from, REG_DAC_LUT_DATA_2, 0xe4);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readb(to, REG_DAC_LUT_DATA_2), ==, 0xe4);
    g_assert_cmphex(qtest_readl(to, REG_GE_REVISION), ==, 0x3f800000);
    g_assert_cmphex(qtest_readl(to, REG_GE_READ_2), ==, 0);
    g_assert_cmphex(qtest_readl(to, REG_GE_READ_1), ==, 0);
    g_assert_cmphex(qtest_readl(to, REG_CONTEXT_COMMAND), ==,
                    MGRAS_FINAL_TOKEN);
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (3u << 16) | 3u));
    g_assert_cmphex(mgras_test_pixel(to, 2, 2), ==, 0x66);

    g_assert_cmphex(mgras_test_display_pixel(to, 2, 2), ==, 0x00665544);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * Each PP1's XMAP register file and PP1SELECT itself travel with the
 * stream: PP1 0 in the fields the single XMAP used, PP1s 1 to 3 in the
 * version-19 section. The four PP1s differ, so a stream that carried only
 * one, or copied PP1 0 into the others, fails a read below.
 */
static void test_pp1_xmap_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-pp1-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS MGRAS_TWO_RE_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS MGRAS_TWO_RE_ARGS
                                   " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    mgras_give_pp1s_distinct_xmaps(from);
    qtest_writeb(from, REG_PP1_SELECT, PP1_SELECT(1, 0));

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readl(to, REG_PP1_SELECT), ==, PP1_SELECT(1, 0));
    for (unsigned int n = 0; n < 4; n++) {
        mgras_assert_pp1_xmap(to, mgras_pp1_select_one(n), n);
    }
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void test_discarded_fifo_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-discard-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    mgras_enter_context(from);
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 (uint64_t)MGRAS_STATE_TOKEN(REG_PACKEDCOLOR, 252) << 32);

    sgi_migrate_stopped(from, uri);

    for (int i = 0; i < 31; i++) {
        qtest_writeq(to, REG_CONTEXT_COMMAND, 0);
    }
    /* RE_POINT reads PACKEDCOLOR only once PP1FILLMODE selects RGB. */
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE,
                            (MGRAS_LOGIC_OP_COPY << 26) | 0x2000));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, MGRAS_PIXEL_WORD(0x00112233)));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, 0));
    g_assert_cmphex(mgras_test_pixel(to, 0, 0), ==, 0x00112233);

    qtest_quit(to);
    qtest_quit(from);
}

static void mgras_begin_partial_fifo_fill(QTestState *qts)
{
    mgras_enter_context(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(qts, 29, 0x99, 0x99663300);
    /* A rect fill reads FILLCOLOR_R, never PACKEDCOLOR, and needs the gate. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_FILLMODE, 4) << 32) |
                 MGRAS_FILLMODE_RECT_FILL_VALUE);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_FILLCOLOR_R, 4) << 32) |
                 0x99);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_BLOCK_START, 4) << 32) |
                 ((96u << 16) | 88u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_BLOCK_END, 4) << 32) |
                 ((97u << 16) | 89u));
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_COLORMASKLSBSA, 4) << 32) |
                 0x00ffffff);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_STATE_TOKEN(REG_PP1FILLMODE, 4) << 32) |
                 ((uint32_t)MGRAS_LOGIC_OP_COPY << 26));

    /* One accepted execute payload word remains after this write. */
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)MGRAS_EXEC_TOKEN(REG_BLOCK_END, 8) << 32) |
                 0xdeadbeef);
}

static void mgras_finish_partial_fifo_fill(QTestState *qts)
{
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0xcafebabe << 32) | MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                     MGRAS_HQ_FLAG_FENCE);
}

static void test_partial_fifo_fill_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *expected, *from, *to;
    uint32_t expected_surface, migrated_surface;

    tmpfs = g_dir_make_tmp("sn0-mgras-partial-fill-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    expected = qtest_init(MIGRATION_ARGS);
    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    mgras_begin_partial_fifo_fill(expected);
    mgras_begin_partial_fifo_fill(from);
    g_assert_cmphex(mgras_test_pixel(expected, 96, 88), ==, 0);
    g_assert_cmphex(mgras_test_pixel(from, 96, 88), ==, 0);
    sgi_migrate_stopped(from, uri);

    mgras_finish_partial_fifo_fill(expected);
    mgras_finish_partial_fifo_fill(to);

    /* Sample the console before a display-property query forces a refresh. */
    expected_surface = mgras_screendump_pixel(expected, 96, 88);
    migrated_surface = mgras_screendump_pixel(to, 96, 88);

    g_assert_cmphex(expected_surface, ==, 0x00996633);
    g_assert_cmphex(mgras_test_pixel(expected, 96, 88), ==, 0x99);
    g_assert_cmphex(mgras_test_wid(expected, 96, 88), ==, 0);
    g_assert_cmphex(mgras_test_pixel(expected, 98, 90), ==, 0);
    g_assert_cmphex(mgras_test_pixel(to, 96, 88), ==,
                     mgras_test_pixel(expected, 96, 88));
    g_assert_cmphex(mgras_test_wid(to, 96, 88), ==,
                     mgras_test_wid(expected, 96, 88));
    g_assert_cmphex(mgras_test_pixel(to, 98, 90), ==,
                     mgras_test_pixel(expected, 98, 90));
    g_assert_cmphex(migrated_surface, ==, expected_surface);

    qtest_quit(to);
    qtest_quit(from);
    qtest_quit(expected);
}

static void test_ge_upload_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    static const uint32_t words[4] = {
        0x11111111, 0x22222222, 0x33333333, 0x44444444,
    };

    tmpfs = g_dir_make_tmp("sn0-mgras-ge-upload-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    qtest_writel(from, REG_GE_CTRL_A, 0x200000);
    for (int i = 0; i < ARRAY_SIZE(words) / 2; i++) {
        qtest_writel(from, REG_GE_DATA_A, words[i]);
    }

    sgi_migrate_stopped(from, uri);

    for (int i = ARRAY_SIZE(words) / 2; i < ARRAY_SIZE(words); i++) {
        qtest_writel(to, REG_GE_DATA_A, words[i]);
    }
    qtest_writel(to, REG_GE_CTRL_A, 0x80000002);
    for (int i = 0; i < ARRAY_SIZE(words) / 2; i++) {
        g_assert_cmphex(qtest_readl(to, REG_GE_READ_2), ==, words[2 * i + 1]);
        g_assert_cmphex(qtest_readl(to, REG_GE_READ_1), ==, words[2 * i]);
    }
    g_assert_cmphex(qtest_readl(to, REG_GE_READ_2), ==, 0);
    g_assert_cmphex(qtest_readl(to, REG_GE_READ_1), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
}

static void test_visual_state_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-visual-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    mgras_enter_context(from);
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    qtest_writeb(from, REG_XMAP_INDEX, 3 << 2);
    qtest_writel(from, REG_XMAP_MAIN_MODE, 15 << 5);
    qtest_writeb(from, REG_XMAP_INDEX, 9 << 2);
    qtest_writel(from, REG_XMAP_MAIN_MODE, 29 << 5);

    mgras_write_cmap_position(from, 0xe00f, 16, 0x11223300);
    mgras_write_cmap_position(from, 0xe01d, 16, 0xa1b2c300);
    mgras_write_cmap_position(from, 0xe00f, 16, 0x11223300);

    mgras_tag_wid(from, 30, 30, 30, 30, 3);
    mgras_tag_wid(from, 31, 30, 31, 30, 9);
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0xf0));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (30u << 16) | 30u));
    qtest_writeq(from, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (31u << 16) | 30u));

    g_assert_cmphex(mgras_test_display_pixel(from, 30, 30), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(from, 31, 30), ==, 0x00a1b2c3);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(qtest_readb(to, REG_XMAP_INDEX), ==, 9 << 2);
    g_assert_cmphex(mgras_test_display_pixel(to, 30, 30), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(to, 31, 30), ==, 0x00a1b2c3);

    /* Continue the map-15 load at the migrated position, entry 0xf1. */
    qtest_writel(to, REG_CMAP_DATA, 0x04050600);
    mgras_tag_wid(to, 32, 30, 32, 30, 3);
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLMODE, MGRAS_FILLMODE_RECT_FILL_VALUE));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_FILLCOLOR_R, 0xf1));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (32u << 16) | 30u));
    qtest_writeq(to, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (32u << 16) | 30u));
    g_assert_cmphex(mgras_test_display_pixel(to, 32, 30), ==, 0x00040506);
    g_assert_cmphex(mgras_test_display_pixel(to, 30, 30), ==, 0x00112233);
    g_assert_cmphex(mgras_test_display_pixel(to, 31, 30), ==, 0x00a1b2c3);

    qtest_quit(to);
    qtest_quit(from);
}

static void test_cursor_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-cursor-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_position(from, 0x001d, 28, 0x0000ff00);
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x01, 0x0100));
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0x0100));
    qtest_writew(from, REG_VC3_RAM, 0x8000);
    qtest_writew(from, REG_VC3_RAM, 0x0000);
    qtest_writew(from, REG_VC3_RAM, 0x0000);
    qtest_writew(from, REG_VC3_RAM, 0x0000);
    qtest_writew(from, REG_VC3_RAM, 0x0000);
    qtest_writew(from, REG_VC3_RAM, 0x0000);
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x07, 0x0142));
    qtest_writew(from, REG_VC3_RAM, 0x4000);
    qtest_writew(from, REG_VC3_RAM, 0x0000);
    /* The position registers name the glyph corner plus 31. */
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x02, 50 + 31));
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x03, 60 + 31));
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1d, 0x0002));
    g_assert_cmphex(mgras_test_display_pixel(from, 50, 60), ==, 0x000000ff);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(mgras_test_display_pixel(to, 50, 60), ==, 0x000000ff);
    g_assert_cmphex(mgras_test_display_pixel(to, 51, 61), ==, 0x00ffffff);
    g_assert_cmphex(mgras_test_display_pixel(to, 52, 62), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
}
int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init(MACHINE_ARGS);


    qtest_add_func("sn0/origin2000/mgras/retrace-phase-migration",
                   test_retrace_phase_migration);
    qtest_add_func("sn0/origin2000/mgras/"
                   "retrace-programmed-period-migration",
                   test_retrace_programmed_period_migration);
    qtest_add_func("sn0/origin2000/mgras/"
                   "retrace-running-period-migration",
                   test_retrace_running_period_migration);
    qtest_add_func("sn0/origin2000/mgras/public-fifo/glyph-wide-migration",
                   test_public_fifo_glyph_wide_migration);
    qtest_add_func("sn0/origin2000/mgras/public-fifo/glyph-narrow-migration",
                   test_public_fifo_glyph_narrow_migration);
    qtest_add_func("sn0/origin2000/mgras/migration", test_migration);
    qtest_add_func("sn0/origin2000/mgras/pp1-xmap-migration",
                   test_pp1_xmap_migration);
    qtest_add_func("sn0/origin2000/mgras/discarded-fifo-migration",
                   test_discarded_fifo_migration);
    qtest_add_func("sn0/origin2000/mgras/partial-fifo-fill-migration",
                   test_partial_fifo_fill_migration);
    qtest_add_func("sn0/origin2000/mgras/ge-upload-migration",
                   test_ge_upload_migration);
    qtest_add_func("sn0/origin2000/mgras/visual-state-migration",
                   test_visual_state_migration);
    qtest_add_func("sn0/origin2000/mgras/cursor-migration",
                   test_cursor_migration);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
