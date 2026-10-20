/*
 * MGRAS register-contract and protocol qtests: GL pipeline-state migration
 * cases.
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


/*
 * Each context's raster position crosses a migration: a's valid one and
 * b's culled one, whose position stays from before the cull.
 */
static void test_gl_raster_position_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-raster-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("file:%s/migrate.bin", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);

    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_viewport(from, 0, 0, 128, 128);
    gl_three(from, GL_COLOR3F_HDR, 0, 0.5f, 1);
    gl_three(from, GL_RASTER_POS3F_HDR, 0.5f, -0.5f, 0.25f);
    gl_enter_slot(from, GL_SLOT_B);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_viewport(from, 0, 0, 64, 64);
    gl_raster_pos2f(from, 0.5f, 0);
    gl_raster_pos2f(from, 2, 0);

    sgi_migrate_stopped(from, uri);
    qtest_quit(from);
    to = qtest_init(dest_cmdline);
    sgi_wait_migration_status(to, "completed", 30);

    g_assert_cmphex(gl_query(to, GL_Q_RASTER_VALID, 1), ==, 0);
    g_assert_cmphex(gl_query(to, GL_Q_RASTER_X, 1), ==,
                    gl_bits(48 + GL_RASTER_BIAS));
    gl_enter_slot(to, GL_SLOT_A);
    g_assert_cmphex(gl_query(to, GL_Q_RASTER_VALID, 1), ==, UINT32_MAX);
    g_assert_cmphex(gl_query(to, GL_Q_RASTER_X, 1), ==,
                    gl_bits(96 + GL_RASTER_BIAS));
    g_assert_cmphex(gl_query(to, GL_Q_RASTER_Y, 1), ==,
                    gl_bits(32 + GL_RASTER_BIAS));
    g_assert_cmphex(gl_query(to, GL_Q_RASTER_Z, 1), ==, gl_bits(0.625f));
    g_assert_cmphex(gl_query(to, GL_Q_RASTER_DISTANCE, 1), ==,
                    gl_bits(0.75f));
    g_assert_cmphex(gl_query(to, GL_Q_RASTER_COLOR + 1, 1), ==,
                    gl_bits(0.5f));
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    g_remove(uri + sizeof("file:") - 1);
    g_rmdir(tmpfs);
}

/*
 * Each context's colour-index state crosses a migration: a's mode and
 * current index, b's mode, write mask and clear index.
 */
static void test_gl_colour_index_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-ci-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("file:%s/migrate.bin", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);

    gl_enter_slot(from, GL_SLOT_B);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_one(from, GL_INIT_CI_HDR, 8);
    gl_one(from, GL_INDEX_MASK_HDR, 0x0f);
    gl_word(from, GL_CLEAR_INDEX_HDR);
    gl_float(from, 3);
    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_one(from, GL_INIT_CI_HDR, 8);
    gl_viewport(from, 0, 0, 128, 128);
    gl_window_projection(from);
    gl_indexs(from, 0x17);

    sgi_migrate_stopped(from, uri);
    qtest_quit(from);
    to = qtest_init(dest_cmdline);
    sgi_wait_migration_status(to, "completed", 30);

    g_assert_cmphex(gl_attribute_query(to, 4), ==, gl_bits(0x17));
    gl_window_rect(to, 10, 10, 20, 20);
    g_assert_cmphex(gl_window_pixel(to, 15, 15), ==, 0x17);
    /* b clears to index 3 under its mask 0x0f: 0x17 becomes 0x13. */
    gl_enter_slot(to, GL_SLOT_B);
    gl_word(to, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(to, 15, 15), ==, 0x13);
    g_assert_cmphex(gl_window_pixel(to, 30, 30), ==, 0x03);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    g_remove(uri + sizeof("file:") - 1);
    g_rmdir(tmpfs);
}

/*
 * Bank 1's pixels cross a migration with their format: an index shown
 * through the colour map and a direct colour shown as it is.
 */
static void test_gl_colour_index_bank_one_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-ci-bank-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("file:%s/migrate.bin", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);

    gl_colour_index_back_buffer(from);
    gl_indexs(from, 0x13);
    gl_window_rect(from, 10, 10, 20, 30);
    gl_word(from, GL_INIT_RGB);
    gl_color3f(from, 1, 1, 1);
    gl_window_rect(from, 90, 10, 100, 30);
    qtest_writeb(from, REG_XMAP_INDEX, 0);
    qtest_writel(from, REG_XMAP_BUF_SELECT, 1u << 14);

    sgi_migrate_stopped(from, uri);
    qtest_quit(from);
    to = qtest_init(dest_cmdline);
    sgi_wait_migration_status(to, "completed", 30);

    g_assert_cmphex(mgras_test_display_pixel(to, 256 + 15, 383 - 20), ==,
                    0x0000ff);
    g_assert_cmphex(mgras_test_display_pixel(to, 256 + 95, 383 - 20), ==,
                    0xffffff);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    g_remove(uri + sizeof("file:") - 1);
    g_rmdir(tmpfs);
}

/*
 * A 12-bit index crosses a migration whole and still resolves through the
 * 4096-entry map of its display ID's 12-bit mode (0x403), as the IRIX 6.5.30
 * server sets one up for its 12-plane PseudoColor visuals: index 300 shows
 * CMAP entry 300, not entry 44.
 */
static void test_gl_colour_index_twelve_bit_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-ci12-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("file:%s/migrate.bin", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);

    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_one(from, GL_INIT_CI_HDR, 12);
    gl_viewport(from, 0, 0, 128, 128);
    gl_window_projection(from);
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    qtest_writeb(from, REG_XMAP_INDEX, 8 << 2);
    qtest_writel(from, REG_XMAP_MAIN_MODE, 0x403);
    mgras_tag_wid(from, 256, 256, 383, 383, 8);
    mgras_write_cmap_entry(from, 0, 0x2c, 0xff000000);
    mgras_write_cmap_entry(from, 1, 0x2c, 0x00ff0000);
    gl_indexs(from, 300);
    gl_window_rect(from, 10, 10, 20, 30);

    sgi_migrate_stopped(from, uri);
    qtest_quit(from);
    to = qtest_init(dest_cmdline);
    sgi_wait_migration_status(to, "completed", 30);

    g_assert_cmphex(gl_window_pixel(to, 15, 20), ==, 300);
    g_assert_cmphex(mgras_test_display_pixel(to, 256 + 15, 383 - 20), ==,
                    0x00ff00);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    g_remove(uri + sizeof("file:") - 1);
    g_rmdir(tmpfs);
}

/*
 * Colour-index state and a bitmap's image under way cross a migration:
 * b's index mode, write mask and clear index, and a's image stopped after
 * its first word, which finishes on the destination in a's raster index.
 */
static void test_gl_colour_index_and_bitmap_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-index-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("file:%s/migrate.bin", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);

    gl_enter_slot(from, GL_SLOT_B);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_one(from, GL_INIT_CI_HDR, 8);
    gl_one(from, GL_INDEX_MASK_HDR, 0x0f);
    gl_word(from, GL_CLEAR_INDEX_HDR);
    gl_float(from, 3);
    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_one(from, GL_INIT_CI_HDR, 8);
    gl_viewport(from, 0, 0, 128, 128);
    gl_window_projection(from);
    gl_indexs(from, 0x17);
    gl_raster_pos2f(from, 20, 30);
    gl_bitmap_packet(from, 16, 4, 0, 0, 0, 0);
    gl_word(from, GL_INLINE_BLOCK | 8);
    gl_word(from, 0xffff0000);

    sgi_migrate_stopped(from, uri);
    qtest_quit(from);
    to = qtest_init(dest_cmdline);
    sgi_wait_migration_status(to, "completed", 30);

    gl_word(to, 0x0000ffff);
    for (int x = 20; x < 36; x++) {
        g_assert_cmphex(gl_window_pixel(to, x, 30), ==, 0x17);
        g_assert_cmphex(gl_window_pixel(to, x, 31), ==, 0);
        g_assert_cmphex(gl_window_pixel(to, x, 33), ==, 0x17);
    }
    gl_word(to, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(to, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    g_assert_cmphex(gl_query(to, GL_Q_RASTER_INDEX, 1), ==, gl_bits(0x17));

    /* b clears to index 3 under its mask 0x0f: 0x17 becomes 0x13. */
    gl_enter_slot(to, GL_SLOT_B);
    gl_word(to, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(to, 20, 30), ==, 0x13);
    g_assert_cmphex(gl_window_pixel(to, 20, 31), ==, 0x03);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    g_remove(uri + sizeof("file:") - 1);
    g_rmdir(tmpfs);
}

static void gl_depth_range_setup(QTestState *from)
{
    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_depth_range(from, 0.125f, 0.75f);
    gl_enter_slot(from, GL_SLOT_B);
    gl_word(from, GL_CONTEXT_OPENER);
}

static void gl_depth_range_check(QTestState *to)
{
    g_assert_cmphex(gl_query(to, GL_Q_DEPTH_RANGE + 1, 1), ==, gl_bits(1));
    gl_enter_slot(to, GL_SLOT_A);
    g_assert_cmphex(gl_query(to, GL_Q_DEPTH_RANGE, 1), ==, gl_bits(0.125f));
    g_assert_cmphex(gl_query(to, GL_Q_DEPTH_RANGE + 1, 1), ==,
                    gl_bits(0.75f));
}

/* Each context's depth range crosses a migration. */
static void test_gl_depth_range_migration(void)
{
    gl_migrate(gl_depth_range_setup, gl_depth_range_check);
}

static void gl_dither_setup(QTestState *from)
{
    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_one(from, GL_DITHER_HDR, 0);
    gl_enter_slot(from, GL_SLOT_B);
    gl_word(from, GL_CONTEXT_OPENER);
}

static void gl_dither_check(QTestState *to)
{
    g_assert_cmphex(gl_query(to, GL_Q_PIXEL_ENABLES, 1) & 1, ==, 1);
    gl_enter_slot(to, GL_SLOT_A);
    g_assert_cmphex(gl_query(to, GL_Q_PIXEL_ENABLES, 1) & 1, ==, 0);
}

/* Each context's dither enable crosses a migration. */
static void test_gl_dither_migration(void)
{
    gl_migrate(gl_dither_setup, gl_dither_check);
}

static void gl_lighting_setup(QTestState *from)
{
    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_light4(from, GL_LIGHT_DIFFUSE_HDR, 5, 0.5f, 0.25f, 0.125f, 1);
    gl_word(from, GL_GET_LIGHT_HDR);
    gl_word(from, 5);
    gl_word(from, GL_DIFFUSE);
    gl_enter_slot(from, GL_SLOT_B);
    gl_word(from, GL_CONTEXT_OPENER);
}

static void gl_lighting_check(QTestState *to)
{
    gl_enter_slot(to, GL_SLOT_A);
    g_assert_cmphex(gl_query(to, GL_Q_READBACK + 1, 1), ==, gl_bits(0.25f));
    gl_readback(to, GL_GET_LIGHT_HDR, 5, GL_DIFFUSE,
                (const float[4]) { 0.5f, 0.25f, 0.125f, 1 });
}

/* Each context's lighting state and readback cross a migration. */
static void test_gl_lighting_migration(void)
{
    gl_migrate(gl_lighting_setup, gl_lighting_check);
}
#define GL_EMISSION           0x1600

/*
 * Slot A holds two vertices of a back-facing, smooth, two-sided lit
 * triangle whose back colour is the back material's emission, green, and
 * no ambient; slot B has drawn one triangle of a strip, so its next is an
 * odd one, with a red front emission. Slot A's third vertex after the
 * migration completes the triangle in green only if the held vertices'
 * back colours travelled; slot B's fourth draws its triangle in the front
 * colour only if the strip's parity did. Both need the lighting enables.
 */
static void gl_light_enables_setup(QTestState *from)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(from, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_word(from, GL_CONTEXT_OPENER);
        gl_viewport(from, 0, 0, 128, 128);
        gl_three(from, GL_LIGHT_MODEL_AMBIENT_HDR, 0, 0, 0);
        gl_one(from, GL_LIGHT_MODEL_TWO_SIDE_HDR, 1);
        gl_material(from, GL_FRONT_FACE, GL_EMISSION, 1, 0, 0, 1);
        gl_material(from, GL_BACK_FACE, GL_EMISSION, 0, 1, 0, 1);
        gl_one(from, GL_LIGHT_ON_HDR, 3);
        gl_one(from, GL_NORMALIZE_HDR, 1);
        gl_word(from, GL_LIGHTING_ON);
    }
    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_BEGIN_TRIANGLES);
    gl_vertex3f(from, -0.9f, -0.9f, 0);
    gl_vertex3f(from, 0, 0.9f, 0);
    gl_enter_slot(from, GL_SLOT_B);
    gl_word(from, GL_BEGIN_TRI_STRIP);
    gl_vertex3f(from, -1, -1, 0);
    gl_vertex3f(from, -0.5f, -1, 0);
    gl_vertex3f(from, -1, -0.5f, 0);
}

static void gl_light_enables_check(QTestState *to)
{
    gl_enter_slot(to, GL_SLOT_A);
    gl_vertex3f(to, 0.9f, -0.9f, 0);
    gl_word(to, GL_END_TRIANGLES);
    g_assert_cmphex(mgras_test_pixel(to, 256 + 64, 383 - 50), ==, 0x00ff00);
    gl_enter_slot(to, GL_SLOT_B);
    gl_vertex3f(to, -0.5f, -0.5f, 0);
    gl_word(to, GL_END_TRIANGLES);
    g_assert_cmphex(mgras_test_pixel(to, 256 + 24, 383 - 24), ==, 0xff0000);
}

/* Each context's lighting enables, parity and back colours migrate. */
static void test_gl_light_enables_migration(void)
{
    gl_migrate(gl_light_enables_setup, gl_light_enables_check);
}

/*
 * Both slots cull the front face, slot B with clockwise front. After the
 * migration a clockwise triangle draws in slot A's context only and a
 * counter-clockwise one in slot B's only.
 */
static void gl_cull_setup(QTestState *from)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(from, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_word(from, GL_CONTEXT_OPENER);
        gl_viewport(from, 0, 0, 128, 128);
        gl_word(from, GL_CULL_ON);
        gl_one(from, GL_CULL_FACE_HDR, GL_FRONT_FACE);
        gl_color3f(from, 0, 1, 0);
    }
    gl_one(from, GL_FRONT_FACE_HDR, GL_CW);
}

static void gl_cull_check(QTestState *to)
{
    for (int slot = 0; slot < 2; slot++) {
        for (int cw = 0; cw < 2; cw++) {
            gl_enter_slot(to, slot ? GL_SLOT_B : GL_SLOT_A);
            gl_clear_color(to, 0, 0, 0, 1);
            gl_word(to, GL_CLEAR_COLOR_BUFFER);
            gl_lit_triangle(to, cw);
            g_assert_cmphex(mgras_test_pixel(to, 256 + 64, 383 - 50), ==,
                            slot != cw ? 0x00ff00 : 0);
        }
    }
}

/* Each context's culling enable, face and front face migrate. */
static void test_gl_cull_migration(void)
{
    gl_migrate(gl_cull_setup, gl_cull_check);
}

/* Slot A scissors to the window's left half, slot B does not. */
static void gl_scissor_setup(QTestState *from)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(from, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_word(from, GL_CONTEXT_OPENER);
        gl_viewport(from, 0, 0, 128, 128);
        gl_scissor(from, 0, 0, 64, 128);
    }
    gl_enter_slot(from, GL_SLOT_A);
    gl_one(from, GL_SCISSOR_TEST_HDR, 1);
}

static void gl_scissor_check(QTestState *to)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(to, slot ? GL_SLOT_B : GL_SLOT_A);
        g_assert_cmphex(gl_query(to, GL_Q_SCISSOR_TEST, 1), ==, !slot);
        gl_clear_color(to, 0, 0, 0, 1);
        gl_one(to, GL_SCISSOR_TEST_HDR, 0);
        gl_word(to, GL_CLEAR_COLOR_BUFFER);
        gl_one(to, GL_SCISSOR_TEST_HDR, !slot);
        gl_fill_window(to, 0, 1, 0);
        g_assert_cmphex(mgras_test_pixel(to, 256 + 96, 383 - 50), ==,
                        slot ? 0x00ff00 : 0);
        g_assert_cmphex(mgras_test_pixel(to, 256 + 10, 383 - 50), ==,
                        0x00ff00);
    }
    g_assert_cmphex(gl_query(to, GL_Q_SCISSOR_BOX + 2, 1), ==, 63);
}

/* Each context's scissor enable and box migrate. */
static void test_gl_scissor_migration(void)
{
    gl_migrate(gl_scissor_setup, gl_scissor_check);
}

/* Slot A tests alpha against GL_GREATER 0.5; slot B keeps the initial. */
static void gl_alpha_setup(QTestState *from)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(from, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_word(from, GL_CONTEXT_OPENER);
        gl_viewport(from, 0, 0, 128, 128);
    }
    gl_enter_slot(from, GL_SLOT_A);
    gl_alpha_func(from, GL_GREATER, 2048);
    gl_one(from, GL_ALPHA_TEST_HDR, 1);
}

static void gl_alpha_check(QTestState *to)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(to, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_clear_color(to, 0, 0, 0, 1);
        g_assert_cmphex(mgras_test_pixel(to, 256 + 64, 383 - 64), ==, 0);
        g_assert_cmphex(gl_alpha_window(to, 0.25f), ==, slot ? 0x00ff00 : 0);
        g_assert_cmphex((gl_query(to, GL_Q_ALPHA, 1) >> 3) & 0xfff, ==,
                        slot ? 0 : 2048);
    }
}

/* Each context's alpha test migrates. */
static void test_gl_alpha_migration(void)
{
    gl_migrate(gl_alpha_setup, gl_alpha_check);
}

/*
 * Slot A tests for stencil 3, which the window's pixels hold, and slot B
 * keeps the test off; both the state and the buffer migrate.
 */
static void gl_stencil_setup(QTestState *from)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(from, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_word(from, GL_CONTEXT_OPENER);
        gl_viewport(from, 0, 0, 128, 128);
    }
    gl_enter_slot(from, GL_SLOT_A);
    gl_one(from, GL_CLEAR_STENCIL_HDR, 3);
    gl_word(from, GL_CLEAR_STENCIL_BUFFER);
    gl_one(from, GL_STENCIL_TEST_HDR, 1);
    gl_stencil(from, GL_STENCIL_EQUAL, 3, 0xff, GL_OP_KEEP, GL_OP_KEEP,
               GL_OP_INCR);
}

static void gl_stencil_check(QTestState *to)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(to, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_clear_color(to, 0, 0, 0, 1);
        gl_word(to, GL_CLEAR_COLOR_BUFFER);
        gl_fill_window(to, 0, 1, 0);
        g_assert_cmphex(mgras_test_pixel(to, 256 + 64, 383 - 64), ==,
                        0x00ff00);
    }
    /* Slot A's pass incremented the stencil to 4: it now fails. */
    gl_enter_slot(to, GL_SLOT_A);
    gl_word(to, GL_CLEAR_COLOR_BUFFER);
    gl_fill_window(to, 0, 1, 0);
    g_assert_cmphex(mgras_test_pixel(to, 256 + 64, 383 - 64), ==, 0);
    g_assert_cmphex((gl_query(to, GL_Q_STENCIL, 1) >> 9) & 7, ==,
                    GL_OP_INCR);
}

/* Each context's stencil state and the stencil buffer migrate. */
static void test_gl_stencil_migration(void)
{
    gl_migrate(gl_stencil_setup, gl_stencil_check);
}

/* Slot A adds its colour to the window; slot B replaces it. */
static void gl_blend_setup(QTestState *from)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(from, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_word(from, GL_CONTEXT_OPENER);
        gl_viewport(from, 0, 0, 128, 128);
    }
    gl_enter_slot(from, GL_SLOT_A);
    gl_one(from, GL_BLEND_HDR, 1);
    gl_blend(from, 1, 1, GL_FUNC_ADD);
}

static void gl_blend_check(QTestState *to)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(to, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_clear_color(to, 1, 0, 0, 1);
        gl_word(to, GL_CLEAR_COLOR_BUFFER);
        gl_fill_window(to, 0, 1, 0);
        g_assert_cmphex(mgras_test_pixel(to, 256 + 64, 383 - 64), ==,
                        slot ? 0x00ff00 : 0xffff00);
    }
}

/* Each context's blending state migrates. */
static void test_gl_blend_migration(void)
{
    gl_migrate(gl_blend_setup, gl_blend_check);
}

/* Slot A inverts the window; slot B keeps GL_COPY. */
static void gl_logic_op_setup(QTestState *from)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(from, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_word(from, GL_CONTEXT_OPENER);
        gl_viewport(from, 0, 0, 128, 128);
    }
    gl_enter_slot(from, GL_SLOT_A);
    gl_one(from, GL_LOGIC_OP_HDR, 0xa);
    gl_one(from, GL_LOGIC_OP_ENABLE_HDR, 1);
}

static void gl_logic_op_check(QTestState *to)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(to, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_clear_color(to, 1, 0, 0, 1);
        gl_word(to, GL_CLEAR_COLOR_BUFFER);
        gl_fill_window(to, 0, 1, 0);
        g_assert_cmphex(mgras_test_pixel(to, 256 + 64, 383 - 64), ==,
                        slot ? 0x00ff00 : 0x00ffff);
    }
}

/* Each context's logical operation migrates. */
static void test_gl_logic_op_migration(void)
{
    gl_migrate(gl_logic_op_setup, gl_logic_op_check);
}

/* Slot A writes red only; slot B every channel. */
static void gl_colour_mask_setup(QTestState *from)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(from, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_word(from, GL_CONTEXT_OPENER);
        gl_viewport(from, 0, 0, 128, 128);
        gl_clear_color(from, 0, 0, 1, 1);
        gl_word(from, GL_CLEAR_COLOR_BUFFER);
    }
    gl_enter_slot(from, GL_SLOT_A);
    gl_one(from, GL_COLOR_MASK_HDR, 1);
}

static void gl_colour_mask_check(QTestState *to)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(to, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_fill_window(to, 1, 1, 0);
        g_assert_cmphex(mgras_test_pixel(to, 256 + 64, 383 - 64), ==,
                        slot ? 0xffff00 : 0xff00ff);
        gl_fill_window(to, 0, 0, 1);
    }
}

/* Each context's colour mask migrates. */
static void test_gl_colour_mask_migration(void)
{
    gl_migrate(gl_colour_mask_setup, gl_colour_mask_check);
}

/* Slot A fogs from 0 to 2; slot B has fog off. */
static void gl_fog_setup(QTestState *from)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(from, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_word(from, GL_CONTEXT_OPENER);
        gl_viewport(from, 0, 0, 128, 128);
    }
    gl_enter_slot(from, GL_SLOT_A);
    gl_fog_linear(from, 0, 2);
    gl_one(from, GL_FOG_HDR, 1);
}

static void gl_fog_check(QTestState *to)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(to, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_fill_window_at(to, 0, 0, 1, -0.5f);
        g_assert_cmphex(mgras_test_pixel(to, 256 + 64, 383 - 64), ==,
                        slot ? 0x0000ff : 0x4000bf);
    }
}

/* Each context's fog state migrates. */
static void test_gl_fog_migration(void)
{
    gl_migrate(gl_fog_setup, gl_fog_check);
}

/* Slot A offsets its fills one step nearer; slot B does not. */
static void gl_offset_setup(QTestState *from)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(from, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_word(from, GL_CONTEXT_OPENER);
        gl_viewport(from, 0, 0, 128, 128);
        gl_one(from, GL_DEPTH_TEST_HDR, 1);
        gl_one(from, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
    }
    gl_enter_slot(from, GL_SLOT_A);
    gl_polygon_offset(from, 0, -1, false);
    gl_one(from, GL_OFFSET_ENABLES_HDR, 1);
}

/*
 * A quad at z 0 has window depth 0x800000 in 24 bits, so against a depth
 * buffer cleared to that it passes GL_LESS only when offset nearer.
 */
static void gl_offset_check(QTestState *to)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(to, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_clear_color(to, 0, 0, 0, 1);
        gl_word(to, GL_CLEAR_COLOR_BUFFER);
        gl_one(to, GL_CLEAR_DEPTH_HDR, 0x800000);
        gl_word(to, GL_CLEAR_DEPTH_BUFFER);
        gl_fill_window(to, 0, 1, 0);
        g_assert_cmphex(mgras_test_pixel(to, 256 + 64, 383 - 64), ==,
                        slot ? 0 : 0x00ff00);
    }
}

/* Each context's polygon offset migrates. */
static void test_gl_polygon_offset_migration(void)
{
    gl_migrate(gl_offset_setup, gl_offset_check);
}

/* Slot A clips to x >= 0; slot B does not. */
static void gl_clip_setup(QTestState *from)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(from, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_word(from, GL_CONTEXT_OPENER);
        gl_viewport(from, 0, 0, 128, 128);
    }
    gl_enter_slot(from, GL_SLOT_A);
    gl_clip_plane(from, 0, 1, 0, 0, 0);
    gl_one(from, GL_CLIP_PLANE_ON_HDR, GL_CLIP_PLANE0);
}

static void gl_clip_check(QTestState *to)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(to, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_clear_color(to, 0, 0, 0, 1);
        gl_word(to, GL_CLEAR_COLOR_BUFFER);
        gl_fill_window(to, 0, 1, 0);
        g_assert_cmphex(mgras_test_pixel(to, 256 + 32, 383 - 64), ==,
                        slot ? 0x00ff00 : 0);
        g_assert_cmphex(mgras_test_pixel(to, 256 + 96, 383 - 64), ==,
                        0x00ff00);
    }
}

/* Each context's client clip planes migrate. */
static void test_gl_clip_plane_migration(void)
{
    gl_migrate(gl_clip_setup, gl_clip_check);
}

/* Slot A has a point size of 3 and line smoothing on; slot B neither. */
static void gl_clip_query_record(QTestState *qts, uint32_t first,
                                 uint32_t second)
{
    gl_word(qts, 0x00007c08);
    gl_word(qts, 2);
    gl_word(qts, 6);
    gl_word(qts, GL_INLINE_BLOCK | 24);
    gl_word(qts, 0x113);
    gl_word(qts, first);
    gl_word(qts, 0x2b6);
    gl_word(qts, 0x114);
    gl_word(qts, second);
    gl_word(qts, 0x2b6);
}

static void gl_clip_query_setup(QTestState *from)
{
    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_clip_query_record(from, 0xfffe8001, 0x8000ffff);
    gl_enter_slot(from, GL_SLOT_B);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_clip_query_record(from, 0x76543210, 0x89abcdef);
}

static void gl_clip_query_check(QTestState *to)
{
    g_assert_cmphex(gl_query(to, 0x28c, 1), ==, 0x76543210);
    g_assert_cmphex(gl_query(to, 0x28e, 1), ==, 0x89abcdef);
    gl_enter_slot(to, GL_SLOT_A);
    g_assert_cmphex(gl_query(to, 0x28c, 1), ==, 0xfffe8001);
    g_assert_cmphex(gl_query(to, 0x28e, 1), ==, 0x8000ffff);
}

static void test_gl_clip_query_migration(void)
{
    gl_migrate(gl_clip_query_setup, gl_clip_query_check);
}

/*
 * Slot A has IRIS GL's colour and normal rule on and a colour after its
 * last normal, so its lit triangle takes the colour, 0x336699; slot B
 * has the rule on but a normal after the colour, so its triangle is lit,
 * 0x8a4a2a. Either colour from the other slot means a field did not
 * travel with its context.
 */
static void gl_iris_rule_setup(QTestState *from)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(from, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_word(from, GL_CONTEXT_OPENER);
        gl_viewport(from, 0, 0, 128, 128);
        gl_one(from, GL_LIGHT_ON_HDR, 0);
        gl_word(from, GL_LIGHTING_ON);
        gl_material(from, GL_FRONT_FACE, GL_DIFFUSE, 0.5f, 0.25f, 0.125f,
                    1);
        gl_word(from, GL_IRIS_LMC_COLOR);
        gl_word(from, GL_IRIS_RULE_ON);
        if (slot) {
            gl_color3f(from, 0.2f, 0.4f, 0.6f);
            gl_three(from, GL_NORMAL3F_HDR, 0, 0, 1);
        } else {
            gl_three(from, GL_NORMAL3F_HDR, 0, 0, 1);
            gl_color3f(from, 0.2f, 0.4f, 0.6f);
        }
    }
}

static void gl_iris_rule_check(QTestState *to)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(to, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_clear_color(to, 0, 0, 0, 1);
        gl_word(to, GL_CLEAR_COLOR_BUFFER);
        g_assert_cmphex(gl_lit_triangle(to, false), ==,
                        slot ? 0x8a4a2a : 0x336699);
    }
}

/* Each context's IRIS GL colour and normal rule state migrates. */
static void test_gl_iris_rule_migration(void)
{
    gl_migrate(gl_iris_rule_setup, gl_iris_rule_check);
}

static void gl_line_point_setup(QTestState *from)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(from, slot ? GL_SLOT_B : GL_SLOT_A);
        gl_word(from, GL_CONTEXT_OPENER);
    }
    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_POINT_SIZE_HDR);
    gl_float(from, 3);
    gl_one(from, GL_LINE_SMOOTH_HDR, 1);
}

static void gl_line_point_check(QTestState *to)
{
    for (int slot = 0; slot < 2; slot++) {
        gl_enter_slot(to, slot ? GL_SLOT_B : GL_SLOT_A);
        g_assert_cmphex(gl_query(to, GL_Q_POINT_SIZE, 1), ==,
                        gl_bits(slot ? 1 : 3));
        g_assert_cmphex(gl_query(to, GL_Q_LINE_SMOOTH, 1), ==, !slot);
    }
}

/* Each context's point, line and polygon state migrates. */
static void test_gl_line_point_migration(void)
{
    gl_migrate(gl_line_point_setup, gl_line_point_check);
}
int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);


    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-raster-position-migration",
        test_gl_raster_position_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-colour-index-migration",
        test_gl_colour_index_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-colour-index-bank-one-migration",
        test_gl_colour_index_bank_one_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-colour-index-twelve-bit-migration",
        test_gl_colour_index_twelve_bit_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-depth-range-migration",
        test_gl_depth_range_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-dither-migration",
        test_gl_dither_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-lighting-migration",
        test_gl_lighting_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-light-enables-migration",
        test_gl_light_enables_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-cull-migration",
        test_gl_cull_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-scissor-migration",
        test_gl_scissor_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-alpha-migration",
        test_gl_alpha_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-stencil-migration",
        test_gl_stencil_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-blend-migration",
        test_gl_blend_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-logic-op-migration",
        test_gl_logic_op_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-colour-mask-migration",
        test_gl_colour_mask_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-fog-migration",
        test_gl_fog_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-polygon-offset-migration",
        test_gl_polygon_offset_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-clip-plane-migration",
        test_gl_clip_plane_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-clip-query-migration",
        test_gl_clip_query_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-iris-rule-migration",
        test_gl_iris_rule_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-line-point-migration",
        test_gl_line_point_migration, false);
    mgras_qtest_add_func(
        "sn0/origin2000/mgras/gl-colour-index-and-bitmap-migration",
        test_gl_colour_index_and_bitmap_migration, false);

    ret = g_test_run();
    if (qts) {
        qtest_quit(qts);
        qts = NULL;
    }

    return ret;
}
