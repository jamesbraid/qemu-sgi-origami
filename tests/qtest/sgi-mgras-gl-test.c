/*
 * SI Viz register-contract and protocol qtests: GL context, state and query
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
 * __Mgr_ReturnMode clears flag 17, writes 0xa20c and three arguments, polls
 * 0x020010 until bit 17 is set, then reads the answer at 0x028004 once.
 * Index 0x3a is element 0 of the modelview matrix, 1 in the initial
 * identity (test_hq_query_answers_the_matrices).
 */
static void test_hq_query_sets_flag_after_arguments(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_QUERY);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_QUERY_TOKEN);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x3a);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    0);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 1);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    MGRAS_HQ_FLAG_QUERY);
    g_assert_cmphex(qtest_readl(qts, REG_QUERY_ANSWER), ==, 0x3f800000);

    /* The arguments were consumed: the next word is a header again. */
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
}

/* Other class-0 opcodes, the swap word among them, leave flag 17 alone. */
static void test_hq_query_flag_only_from_query(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_SWAP_TOKEN);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x00003600);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_QUERY, ==,
                    0);
}
#define GL_MULT_MATRIX_HDR    0x00002d40
#define GL_FRUSTUM_HDR        0x00003418
#define GL_SWAP               0x00003700
#define GL_RE_COUNT_HDR       0x00008c04
#define GL_BANKS_HDR          0x00009808
#define GL_DEPTH_EQUAL        2

static void gl_matrix(QTestState *qts, uint32_t header, const float m[16])
{
    gl_word(qts, header);
    for (int i = 0; i < 16; i++) {
        gl_float(qts, m[i]);
    }
}

/*
 * The kernel's MgrasValidateBanks as the swap captures show it: a 64-bit
 * store of the header and the first word, then a 32-bit store of the
 * second.
 */
static void gl_banks(QTestState *qts, uint32_t bank)
{
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)GL_BANKS_HDR << 32) | bank);
    gl_word(qts, bank);
}

/*
 * One swap as the kernel performs it in the captured swap case: the bank
 * token, the swap token, then, once the swap interrupt is served, the main
 * buffer-select mask at XMAP index 0 (0x4000 after the first swap of the
 * window with display ID 14, 0 after the second).
 */
static void gl_swap(QTestState *qts, uint32_t bank, uint32_t mask)
{
    gl_banks(qts, bank);
    gl_word(qts, GL_SWAP);
    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, mask);
}

/*
 * Window vertices (6, 98), (118, 54) and (38, 10) of the probe's ortho,
 * swap and swap-pending cases, as normalized device coordinates.
 */
static void gl_ortho_triangle_ndc(QTestState *qts)
{
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, -0.90625f, 0.53125f, 0);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, 0.84375f, -0.15625f, 0);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, -0.40625f, -0.84375f, 0);
    gl_word(qts, GL_END_TRIANGLES);
}

/*
 * SHA-256 of the 128x128 expected windows gl_min_expected.py writes for
 * the probe's scene cases (a probe branch),
 * computed from the OpenGL 1.1 rules and matched by Mesa llvmpipe, never
 * from this model.
 */
#define GL_ORTHO_SHA256 \
    "72929655985993b8505b211a3479c5841a5f93ab625cef6d0b32f7323abe99d8"
#define GL_FRUSTUM_SHA256 \
    "2411a9c279313dc62c8e536db291ba6d66abab75883d077aa70f92f127ed2746"

/*
 * The ortho case: glOrtho(0, 128, 0, 128, -1, 1) on the projection, then
 * glLoadMatrixf of a modelview that maps object (x, y) to eye
 * (120 - 2y, x + 4). The window is the product projection x modelview
 * applied to each vertex (OpenGL 1.2.1, 2.10); applying them in the other
 * order, or reading the array by rows, moves the triangle off the window.
 */
static void test_gl_ortho_matches_the_reference(void)
{
    static const float modelview[16] = {
        0, 1, 0, 0, -2, 0, 0, 0, 0, 0, 1, 0, 120, 4, 0, 1,
    };
    g_autofree uint32_t *rgb = NULL;

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_word(qts, GL_LOAD_IDENTITY);
    gl_six(qts, GL_ORTHO_HDR, 0, 128, 0, 128, -1, 1);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_matrix(qts, GL_LOAD_MATRIX_HDR, modelview);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, 94, 57, 0);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, 50, 1, 0);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, 6, 41, 0);
    gl_word(qts, GL_END_TRIANGLES);
    gl_word(qts, GL_FLUSH);

    rgb = mgras_screendump(qts);
    g_assert_cmphex(gl_dump_pixel(rgb, 64, 64), ==, 0xffff00);
    g_assert_cmphex(gl_dump_pixel(rgb, 20, 20), ==, 0x0000ff);
    gl_check_window_sha256(rgb, GL_ORTHO_SHA256);
}

/*
 * The frustum case: glFrustum(-1, 1, -1, 1, 1, 8) after a modelview
 * translation of (0.25, -0.125, -3). The vertices land at eye depths 2, 4
 * and 3, so the divide by w places the third at window y 109 1/3.
 */
static void test_gl_frustum_matches_the_reference(void)
{
    static const float modelview[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.25f, -0.125f, -3, 1,
    };
    g_autofree uint32_t *rgb = NULL;

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_word(qts, GL_LOAD_IDENTITY);
    gl_six(qts, GL_FRUSTUM_HDR, -1, 1, -1, 1, 1, 8);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_matrix(qts, GL_LOAD_MATRIX_HDR, modelview);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, -1.875f, -1.375f, 1);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, 3.25f, -0.375f, -1);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex3f(qts, 0.125f, 2.25f, 0);
    gl_word(qts, GL_END_TRIANGLES);
    gl_word(qts, GL_FLUSH);

    rgb = mgras_screendump(qts);
    g_assert_cmphex(gl_dump_pixel(rgb, 20, 20), ==, 0xffff00);
    g_assert_cmphex(gl_dump_pixel(rgb, 40, 60), ==, 0x0000ff);
    gl_check_window_sha256(rgb, GL_FRUSTUM_SHA256);
}

/*
 * glLoadMatrixf of a translation of +0.5 in x moves a triangle 32 pixels
 * right, and glMultMatrixf of it onto the identity does the same.
 */
static void gl_translated_triangle(QTestState *qts, uint32_t header)
{
    static const float shift[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1,
    };

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_word(qts, GL_LOAD_IDENTITY);
    gl_matrix(qts, header, shift);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, 0);
    gl_vertex3f(qts, 0, -1, 0);
    gl_vertex3f(qts, -1, 0, 0);
    gl_word(qts, GL_END_TRIANGLES);
}

static void test_gl_load_matrix_reads_columns(void)
{
    /* The triangle's right-angle corner moves from (0, 0) to (32, 0). */
    gl_translated_triangle(qts, GL_LOAD_MATRIX_HDR);
    g_assert_cmphex(gl_window_pixel(qts, 33, 1), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0);
}

/*
 * glMultMatrixf multiplies the current matrix on the right (OpenGL 1.2.1,
 * 2.10.2), so the argument applies to vertices first. A scale by 0.5
 * multiplied by a translation of +0.5 in x maps x to 0.5 (x + 0.5): the
 * triangle's corner at x -1 lands at window x 48. The other order,
 * 0.5 x + 0.5, would put it at 64.
 */
static void test_gl_mult_matrix_multiplies_on_the_right(void)
{
    static const float half[16] = {
        0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
    };
    static const float shift[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1,
    };

    gl_translated_triangle(qts, GL_MULT_MATRIX_HDR);
    g_assert_cmphex(gl_window_pixel(qts, 33, 1), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0);

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_matrix(qts, GL_LOAD_MATRIX_HDR, half);
    gl_matrix(qts, GL_MULT_MATRIX_HDR, shift);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, 0);
    gl_vertex3f(qts, 0, -1, 0);
    gl_vertex3f(qts, -1, 0, 0);
    gl_word(qts, GL_END_TRIANGLES);
    /* Corner at window (48, 32); the other order puts it at (64, 32). */
    g_assert_cmphex(gl_window_pixel(qts, 49, 33), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 47, 33), ==, 0);
}

/*
 * Matrix commands act on the matrix glMatrixMode selects. A texture-matrix
 * load, which the model does not use, must leave modelview alone.
 */
static void test_gl_matrix_mode_selects_the_matrix(void)
{
    static const float scale[16] = {
        4, 0, 0, 0, 0, 4, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1,
    };

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_TEXTURE);
    gl_matrix(qts, GL_LOAD_MATRIX_HDR, scale);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, 0);
    gl_vertex3f(qts, -0.75f, -1, 0);
    gl_vertex3f(qts, -1, -0.75f, 0);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 2, 2), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 20, 2), ==, 0);
}

/*
 * Approximated: matrix products are stored at single precision, the
 * precision the client sends; the GE's arithmetic is not published. With
 * a z scale a = 1 + 2^-12 loaded and then multiplied in, a * a =
 * 1 + 2^-11 + 2^-24 rounds to 1 + 2^-11 in single precision. At object z
 * 1887437 / 2^21 the two products give window depths of 15942041.45 and
 * 15942041.90 steps, which round to different 24-bit values; a second
 * layer at z 0.900439441 with the identity matrix lands on the first.
 */
static void test_gl_matrix_product_is_single_precision(void)
{
    const float a = 1.0f + 1.0f / 4096;
    const float scale[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, a, 0, 0, 0, 0, 1,
    };
    const float z = 1887437.0f / 2097152.0f;

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_ALWAYS);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_matrix(qts, GL_LOAD_MATRIX_HDR, scale);
    gl_matrix(qts, GL_MULT_MATRIX_HDR, scale);
    gl_color3f(qts, 1, 0, 0);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, z);
    gl_vertex3f(qts, 3, -1, z);
    gl_vertex3f(qts, -1, 3, z);
    gl_word(qts, GL_END_TRIANGLES);

    gl_word(qts, GL_LOAD_IDENTITY);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_EQUAL);
    gl_color3f(qts, 0, 1, 0);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, 0.900439441204071f);
    gl_vertex3f(qts, 3, -1, 0.900439441204071f);
    gl_vertex3f(qts, -1, 3, 0.900439441204071f);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, 0x00ff00);
}

/*
 * Inferred: the client's context-setup opener 0x0600 starts a new
 * context, whose matrices and depth state are OpenGL's initial ones even
 * though the setup after it sends none. A projection and depth test left
 * by an earlier context must not shrink or hide the new context's
 * drawing: in the probe's capture, the depth case followed the frustum
 * case in the same X session.
 */
static void test_gl_context_setup_restores_initial_state(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_six(qts, GL_FRUSTUM_HDR, -1, 1, -1, 1, 1, 8);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_NEVER);

    gl_word(qts, 0x00000600);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, 0);
    gl_vertex3f(qts, 0, -1, 0);
    gl_vertex3f(qts, -1, 0, 0);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 60, 1), ==, 0xffffff);
}

/*
 * Inferred from the captures: the draw-buffer triple's first word is 1 for
 * the single-buffered visual and 2 for the double-buffered one, the
 * front and back buffer; context-image words 16 and 17 are 1 in every GL
 * image and the kernel's bank token carries 0 then 1 at the two swaps, so
 * those words name the bank that holds the back buffer and the front is
 * the other. Bank 0 is the one the X server draws in. A back-buffer clear
 * therefore leaves bank 0 alone and shows only where the XMAP main
 * buffer-select mask selects bank 1 for the pixel's display ID.
 */
static void test_gl_back_buffer_draws_into_bank_one(void)
{
    gl_scene_start(qts, GL_FLAT);
    mgras_tag_wid(qts, 256, 256, 383, 383, 14);
    gl_draw_buffer(qts, 2);
    gl_clear_color(qts, 1, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);

    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 300, 300), ==, 0);
    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 1u << 14);
    g_assert_cmphex(mgras_test_display_pixel(qts, 300, 300), ==, 0xff00ff);
    /* Display ID 0 around the window still shows bank 0. */
    g_assert_cmphex(mgras_test_display_pixel(qts, 250, 300), ==, 0);
}

/* The front buffer is the bank the back is not. */
static void test_gl_front_buffer_is_the_other_bank(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_draw_buffer(qts, 1);
    gl_clear_color(qts, 0, 1, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0x00ff00);

    gl_banks(qts, 0);
    gl_clear_color(qts, 1, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0x00ff00);
    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 1u);
    g_assert_cmphex(mgras_test_display_pixel(qts, 300, 300), ==, 0xff0000);
}

/*
 * The bank token moves the back buffer: after (0, 0), which the kernel
 * sends at a window's first swap, back-buffer drawing lands in bank 0.
 */
static void test_gl_bank_token_moves_the_back_buffer(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_draw_buffer(qts, 2);
    gl_banks(qts, 0);
    gl_clear_color(qts, 0, 1, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0x00ffff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * Inferred: a context switch restores the bank words from image words 16
 * and 17. An image carrying 0 puts the back buffer in bank 0.
 */
static void test_gl_context_image_restores_the_banks(void)
{
    uint32_t image[MGRAS_CONTEXT_WORDS];

    gl_scene_start(qts, GL_FLAT);
    gl_banks(qts, 0);
    memcpy(image, gl_context_image, sizeof(image));
    qtest_writel(qts, REG_PREFIX, 0);
    qtest_writel(qts, REG_ENTRY, MGRAS_ENTRY_WORD);
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, image[i]);
    }
    qtest_writel(qts, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);
    gl_draw_buffer(qts, 2);
    gl_clear_color(qts, 1, 1, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0);
}

/*
 * The captured swap case: the stale magenta frame, a swap that selects
 * bank 1 for display ID 14, the scene into bank 0, a swap back. The
 * window shows the scene, not the stale frame.
 */
static void test_gl_swap_matches_the_reference(void)
{
    g_autofree uint32_t *rgb = NULL;

    gl_scene_start(qts, GL_FLAT);
    mgras_tag_wid(qts, 256, 256, 383, 383, 14);
    gl_draw_buffer(qts, 2);
    gl_clear_color(qts, 1, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_swap(qts, 0, 1u << 14);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_ortho_triangle_ndc(qts);
    gl_swap(qts, 1, 0);

    rgb = mgras_screendump(qts);
    g_assert_cmphex(gl_dump_pixel(rgb, 64, 64), ==, 0xffff00);
    gl_check_window_sha256(rgb, GL_ORTHO_SHA256);
}

/*
 * The captured swap-pending case: the scene, a swap, then the stale frame
 * into the new back buffer with no swap. The stale frame must not show.
 */
static void test_gl_swap_pending_matches_the_reference(void)
{
    g_autofree uint32_t *rgb = NULL;

    gl_scene_start(qts, GL_FLAT);
    mgras_tag_wid(qts, 256, 256, 383, 383, 14);
    gl_draw_buffer(qts, 2);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_ortho_triangle_ndc(qts);
    gl_swap(qts, 0, 1u << 14);
    gl_clear_color(qts, 1, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_FLUSH);

    rgb = mgras_screendump(qts);
    g_assert_cmphex(gl_dump_pixel(rgb, 64, 64), ==, 0xffff00);
    gl_check_window_sha256(rgb, GL_ORTHO_SHA256);
}

/*
 * Draw-buffer word 3 selects the front and the back buffer together, so a
 * clear and a triangle each reach both banks: bank 0 directly, and bank 1
 * where the XMAP buffer-select mask shows it for the window's display ID.
 */
static void test_gl_front_and_back_draw_into_both_banks(void)
{
    gl_scene_start(qts, GL_FLAT);
    mgras_tag_wid(qts, 256, 256, 383, 383, 14);
    gl_window_projection(qts);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_word(qts, GL_LOAD_IDENTITY);
    gl_draw_buffer(qts, 3);
    gl_clear_color(qts, 1, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, 10.25f, 10.25f);
    gl_vertex2f(qts, 10.875f, 10.25f);
    gl_vertex2f(qts, 10.25f, 10.875f);
    gl_word(qts, GL_END_TRIANGLES);

    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0xff00ff);
    g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, 0xffffff);
    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 1u << 14);
    g_assert_cmphex(mgras_test_display_pixel(qts, 300, 300), ==, 0xff00ff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 10, 383 - 10), ==,
                    0xffffff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * Client a's setup in the alternate case, then client b's, which sets a
 * smaller viewport, another clear and current colour, the identity
 * projection, a depth test that passes nothing and the back buffer. Back
 * in a, a clear and a triangle use a's state; back in b, b's.
 */
static void test_gl_state_follows_the_context_slot(void)
{
    mgras_reset();
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_clear_color(qts, 1, 0, 0, 1);
    gl_color3f(qts, 1, 1, 0);
    gl_window_projection(qts);
    gl_draw_buffer(qts, 1);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 64, 64);
    gl_clear_color(qts, 0, 1, 0, 1);
    gl_color3f(qts, 1, 0, 1);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_NEVER);
    gl_draw_buffer(qts, 2);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, 16, 16);
    gl_vertex2f(qts, 112, 24);
    gl_vertex2f(qts, 56, 111);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 61, 50), ==, 0xffff00);
    g_assert_cmphex(gl_window_pixel(qts, 120, 120), ==, 0xff0000);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 120, 120), ==, 0xff0000);
    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 1u);
    g_assert_cmphex(mgras_test_display_pixel(qts, 300, 300), ==, 0x00ff00);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * The opener starts only the context it is sent in: b's opener returns b
 * to the initial clear colour and leaves a's alone.
 */
static void test_gl_context_opener_resets_only_its_context(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_clear_color(qts, 1, 0, 0, 1);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_clear_color(qts, 0, 1, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0x00ff00);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xff0000);
}

/*
 * The kernel writes -1 in word 0 for a context with no GL state, and its
 * default context carries 0xb27 in the captures. Both select the one
 * state kept for no context, which a GL context's drawing leaves alone.
 */
static void test_gl_images_without_a_slot_share_one_state(void)
{
    mgras_reset();

    gl_enter_slot(qts, 0xffffffff);
    gl_clear_color(qts, 0, 0, 1, 1);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_clear_color(qts, 1, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xff0000);

    gl_enter_slot(qts, GL_SLOT_DEFAULT);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0x0000ff);
}

/*
 * The model holds 32 slots. A 33rd replaces the one selected least
 * recently, and a replaced slot selected again starts from OpenGL's
 * initial state. Selecting slot 0 again after filling the table makes
 * slot 1 the least recent, so a model that replaced the oldest entry
 * instead, or held more than 32, would keep slot 1's green.
 */
static void test_gl_context_table_replaces_the_least_recent(void)
{
    mgras_reset();

    for (int i = 0; i < 32; i++) {
        gl_enter_slot(qts, 0x1000 + i);
        gl_word(qts, GL_CONTEXT_OPENER);
        gl_clear_color(qts, i == 0, i == 1, i > 1, 1);
    }
    gl_enter_slot(qts, 0x1000);
    gl_enter_slot(qts, 0x1000 + 32);
    gl_enter_slot(qts, 0x1000);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xff0000);

    gl_enter_slot(qts, 0x1001);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A switch can land inside a packet. In a captured run
 * (seq 12929748 to 12930582) perfly's context 0x84c6 was switched out 33
 * words into the 156 data words of its fourth GE table load (header
 * 0x80000284), context 0x5420 then sent one glFlush, and 0x84c6 came back
 * and sent the rest of its block. Each context's words are its own
 * stream: b's clear must draw, and a's remaining block words, fence tokens
 * here, must stay data.
 */
static void test_gl_parser_follows_the_context(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_word(qts, 0x80000284);
    for (int i = 0; i < 5 + 33; i++) {
        gl_word(qts, MGRAS_FENCE_TOKEN);
    }

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_clear_color(qts, 0, 1, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0x00ff00);

    gl_enter_slot(qts, GL_SLOT_A);
    for (int i = 33; i < 156; i++) {
        gl_word(qts, MGRAS_FENCE_TOKEN);
    }
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);
    gl_clear_color(qts, 1, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xff0000);
    gl_word(qts, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
}

/*
 * The payload words a context has already sent wait with it: a's
 * glClearColor is split by b's whole clear, and a's colour is the four
 * words a sent, not b's.
 */
static void test_gl_packet_split_by_a_context_switch(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_word(qts, GL_CLEAR_COLOR_HDR);
    gl_float(qts, 1);
    gl_float(qts, 0);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_clear_color(qts, 0, 1, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0x00ffff);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_float(qts, 1);
    gl_float(qts, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xff00ff);
}

/*
 * Image word 1 is 0xc0000000 or more only on the first delivery of a new
 * context. The IRIX 6.5.30 kernel's context allocator (0xc0000000003f38c8
 * in unix.IP27) zeroes the context, sets image word 0 to -1 and ORs
 * 0xc0000000 into word 1. Once MgrasCtxsw has written the image it clears
 * bit 27 of word 1 for every context, and bits 31, 30, 29 and 26 for
 * every context but the default. In a captured run every
 * first delivery of a slot's new context carries both bits (0xe4000000 or
 * 0xec000000) and no later one does; the default context's image keeps
 * bit 31 alone (0x90000000, 0x98000000) on every switch.
 */
#define GL_IMAGE_NEW_CONTEXT     0xec000000
#define GL_IMAGE_DEFAULT_CONTEXT 0x98000000

static void gl_enter_new_slot(QTestState *qts, uint32_t slot)
{
    mgras_enter_gl_image(qts, slot, GL_IMAGE_NEW_CONTEXT, 256, 640, 128, 128,
                         0x11);
}

/*
 * The kernel gives a dead client's slot to the next new context. In a captured
 * run ivview was killed with its glVertex3f two words short in slot 0x5420
 * (seq 13911131), amesh's next context was given 0x5420 new (seq 14490524),
 * and the model took that context's opener and 0x8c04 as the vertex's last two
 * words. A new context's words start at a header: here the dead context's
 * glClearColor is two words short, and the new one's opener and clear colour
 * must not complete it.
 */
static void test_gl_new_context_starts_at_a_header(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_word(qts, GL_CLEAR_COLOR_HDR);
    gl_float(qts, 1);
    gl_float(qts, 0);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);

    gl_enter_new_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_clear_color(qts, 0, 1, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0x00ff00);
}

/*
 * A new context holds none of the dead one's GL state, before its opener
 * as after: the clear colour a new context's glClear uses is the initial
 * one, not the red the slot's last owner set.
 */
static void test_gl_new_context_starts_in_the_initial_state(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_clear_color(qts, 1, 0, 0, 1);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0x0000ff);

    gl_enter_new_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0);
}

/*
 * The default context's image keeps bit 31 of word 1 on every switch, so
 * that bit alone does not start a new context: a packet the default
 * context left short resumes when it is loaded again.
 */
static void test_gl_default_context_keeps_its_packet(void)
{
    mgras_reset();

    mgras_enter_gl_image(qts, GL_SLOT_DEFAULT, GL_IMAGE_DEFAULT_CONTEXT, 256,
                         640, 128, 128, 0x11);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_word(qts, GL_CLEAR_COLOR_HDR);
    gl_float(qts, 1);
    gl_float(qts, 0);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);

    mgras_enter_gl_image(qts, GL_SLOT_DEFAULT, GL_IMAGE_DEFAULT_CONTEXT, 256,
                         640, 128, 128, 0x11);
    gl_float(qts, 1);
    gl_float(qts, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xff00ff);
}

/*
 * Both new-context bits are needed, not bit 31 alone. No captured image of
 * a slotted context has bit 31 without bit 30, and the default context,
 * whose images do, shares the slotless entry the rule leaves alone, so
 * this is the model's choice of the narrower rule: the kernel's allocator
 * sets both bits and only both mark a new context here. A slotted context
 * loaded with 0xac000000 resumes the packet it left open.
 */
static void test_gl_bit_31_alone_keeps_the_packet(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_word(qts, GL_CLEAR_COLOR_HDR);
    gl_float(qts, 1);
    gl_float(qts, 0);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);

    mgras_enter_gl_image(qts, GL_SLOT_A, 0xac000000, 256, 640, 128, 128,
                         0x11);
    gl_float(qts, 1);
    gl_float(qts, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xff00ff);
}

/*
 * A context with no slot yet carries -1 in word 0, as the allocator
 * leaves it, and shares the default context's entry. Such an image with
 * both new-context bits must not start that shared entry afresh, or the
 * packet the default context left short is lost. Neither captured run has
 * such an image; the kernel paths that switch contexts in were not all
 * traced to rule it out.
 */
static void test_gl_new_context_without_a_slot_keeps_the_default(void)
{
    mgras_reset();

    mgras_enter_gl_image(qts, GL_SLOT_DEFAULT, GL_IMAGE_DEFAULT_CONTEXT, 256,
                         640, 128, 128, 0x11);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_word(qts, GL_CLEAR_COLOR_HDR);
    gl_float(qts, 1);
    gl_float(qts, 0);

    mgras_enter_gl_image(qts, 0xffffffff, GL_IMAGE_NEW_CONTEXT, 256, 640, 128,
                         128, 0x11);

    mgras_enter_gl_image(qts, GL_SLOT_DEFAULT, GL_IMAGE_DEFAULT_CONTEXT, 256,
                         640, 128, 128, 0x11);
    gl_float(qts, 1);
    gl_float(qts, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 1, 1), ==, 0xff00ff);
}

/*
 * glTranslatef, glRotatef and glScalef multiply the current matrix on the
 * right (OpenGL 1.2.1, 2.10.2), so the last one sent applies to the
 * vertex first. The rotation is counter-clockwise looking down the axis
 * toward the origin, by an angle in degrees, about the axis made unit
 * length. Here object (x, y) goes to window (64 - y / 2, 32 + 2x): the
 * triangle below lands around pixel centre (64.5, 52.5) and on no other.
 */
static void test_gl_transforms_multiply_on_the_right(void)
{
    gl_transform_start(qts);
    gl_three(qts, GL_TRANSLATE_HDR, 64, 32, 0);
    gl_rotate(qts, 90, 0, 0, 2);
    gl_three(qts, GL_SCALE_HDR, 2, 0.5f, 1);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, 10.125f, -0.5f);
    gl_vertex2f(qts, 10.125f, -1.75f);
    gl_vertex2f(qts, 10.4375f, -0.5f);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 64, 52), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 65, 52), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 63, 52), ==, 0);
}

/*
 * glPushMatrix saves the current matrix of the current mode and
 * glPopMatrix restores it (2.10.2). The stacks are separate: a pop in
 * projection mode with nothing pushed there changes nothing, and the
 * modelview's saved matrix waits for a modelview pop.
 */
static void test_gl_push_and_pop_restore_the_matrix(void)
{
    gl_transform_start(qts);
    gl_three(qts, GL_TRANSLATE_HDR, 8, 0, 0);
    gl_word(qts, GL_PUSH_MATRIX);
    gl_three(qts, GL_TRANSLATE_HDR, 0, 16, 0);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_word(qts, GL_POP_MATRIX);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_mark(qts, 1, 1);
    g_assert_cmphex(gl_window_pixel(qts, 9, 17), ==, 0xffffff);
    gl_word(qts, GL_POP_MATRIX);
    gl_mark(qts, 1, 1);
    g_assert_cmphex(gl_window_pixel(qts, 9, 1), ==, 0xffffff);

    /* A projection push and pop leave the projection as it was. */
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_word(qts, GL_PUSH_MATRIX);
    gl_three(qts, GL_TRANSLATE_HDR, 64, 0, 0);
    gl_word(qts, GL_POP_MATRIX);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_mark(qts, 1, 1);
    g_assert_cmphex(gl_window_pixel(qts, 9, 1), ==, 0xffffff);
}

/*
 * Approximated: the board's stack depths are not known. The model holds
 * 32 matrices in each stack, current one included, the least OpenGL
 * 1.2.1 allows for the modelview stack (table 6.25). A push onto a full
 * stack and a pop of the last matrix change nothing, as the specification
 * has the stack-overflow and stack-underflow errors do. Load a
 * translation of 3k + 1 before the k-th push: after 31 pushes the stack
 * is full, the 32nd push is ignored, so a pop returns translation 91, not
 * 94; after the pops return to translation 1 a further pop leaves it.
 */
static void test_gl_matrix_stack_holds_32(void)
{
    gl_transform_start(qts);
    for (int k = 0; k < 32; k++) {
        gl_word(qts, GL_LOAD_IDENTITY);
        gl_three(qts, GL_TRANSLATE_HDR, 3 * k + 1, 0, 0);
        gl_word(qts, GL_PUSH_MATRIX);
    }
    gl_word(qts, GL_LOAD_IDENTITY);
    gl_three(qts, GL_TRANSLATE_HDR, 120, 0, 0);
    gl_word(qts, GL_POP_MATRIX);
    gl_mark(qts, 0, 10);
    g_assert_cmphex(gl_window_pixel(qts, 91, 10), ==, 0xffffff);
    for (int k = 0; k < 30; k++) {
        gl_word(qts, GL_POP_MATRIX);
    }
    gl_mark(qts, 0, 10);
    g_assert_cmphex(gl_window_pixel(qts, 1, 10), ==, 0xffffff);
    gl_word(qts, GL_POP_MATRIX);
    gl_mark(qts, 0, 10);
    g_assert_cmphex(gl_window_pixel(qts, 1, 10), ==, 0xffffff);
}
#define GL_Q_PROJECTION       0x4a
#define GL_Q_MATRIX_MODE      0x29

/*
 * Each matrix query answers its own stack's current matrix, column-major,
 * whatever the matrix mode, from the loaded context: a translation's x is
 * element 12 of the modelview matrix, and glOrtho's x scale element 0 of
 * the projection matrix. perfly's draw process died reading zeros here.
 */
static void test_hq_query_answers_the_matrices(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    g_assert_cmphex(gl_query(qts, GL_Q_MATRIX_MODE, 0), ==,
                    (uint32_t)(GL_MODELVIEW - 0x1701));
    gl_three(qts, GL_TRANSLATE_HDR, 7, -2, 0.5f);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_six(qts, GL_ORTHO_HDR, 0, 4, 0, 2, -1, 1);
    g_assert_cmphex(gl_query(qts, GL_Q_MATRIX_MODE, 0), ==, 0);

    for (int i = 0; i < 16; i++) {
        static const float mv[16] = {
            1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 7, -2, 0.5f, 1,
        };
        static const float pr[16] = {
            0.5f, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, -1, -1, 0, 1,
        };

        g_assert_cmphex(gl_query(qts, GL_Q_MODELVIEW + i, 0), ==,
                        gl_bits(mv[i]));
        g_assert_cmphex(gl_query(qts, GL_Q_PROJECTION + i, 0), ==,
                        gl_bits(pr[i]));
    }

    /* Another context answers its own, here the initial identity. */
    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    g_assert_cmphex(gl_query(qts, GL_Q_MODELVIEW + 12, 0), ==, 0);
    g_assert_cmphex(gl_query(qts, GL_Q_MODELVIEW + 15, 0), ==, gl_bits(1));
    gl_enter_slot(qts, GL_SLOT_A);
    g_assert_cmphex(gl_query(qts, GL_Q_MODELVIEW + 12, 0), ==, gl_bits(7));
}
#define GL_Q_RASTER_W         0x350

/*
 * Initially (0, 0, 0, 1), valid, white; glRasterPos carries its point
 * through the modelview and projection matrices and the viewport and
 * takes the current colour clamped (OpenGL 1.2.1, 2.12); a point outside
 * the view volume resets only the valid bit.
 */
static void test_hq_query_answers_the_raster_position(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_X, 1), ==,
                    gl_bits(GL_RASTER_BIAS));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_W, 1), ==, gl_bits(1));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_VALID, 1), ==, UINT32_MAX);

    gl_viewport(qts, 10, 20, 100, 50);
    gl_three(qts, GL_TRANSLATE_HDR, 3, 4, 0);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_six(qts, GL_ORTHO_HDR, 0, 10, 0, 10, -4, 4);
    gl_three(qts, GL_COLOR3F_HDR, 2, -0.5f, 0.25f);
    /* Eye (5, 5, 2), clip (0, 0, -0.5, 1), window (60, 45, 0.25). */
    gl_three(qts, GL_RASTER_POS3F_HDR, 2, 1, 2);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_X, 1), ==,
                    gl_bits(60 + GL_RASTER_BIAS));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_Y, 1), ==,
                    gl_bits(45 + GL_RASTER_BIAS));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_Z, 1), ==, gl_bits(0.25f));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_W, 1), ==, gl_bits(1));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_VALID, 1), ==, UINT32_MAX);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_DISTANCE, 1), ==,
                    gl_bits(sqrtf(54)));
    for (int i = 0; i < 4; i++) {
        static const float colour[4] = { 1, 0, 0.25f, 1 };

        g_assert_cmphex(gl_query(qts, GL_Q_RASTER_COLOR + i, 1), ==,
                        gl_bits(colour[i]));
        g_assert_cmphex(gl_query(qts, GL_Q_RASTER_TEXCOORD + i, 1), ==,
                        gl_bits(i == 3));
    }

    /* Clip x 1.2: culled, and the position stays. */
    gl_raster_pos2f(qts, 8, 1);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_VALID, 1), ==, 0);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_X, 1), ==,
                    gl_bits(60 + GL_RASTER_BIAS));
    /* Back inside with z 0: window z 0.5. */
    gl_raster_pos2f(qts, 2, 1);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_VALID, 1), ==, UINT32_MAX);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_Z, 1), ==, gl_bits(0.5f));
}

/*
 * glRasterPos2i (0x58003808 and two integers) sets the raster position as
 * glRasterPos2f of the same values would.
 */
#define GL_RASTER_POS2I_HDR   0x58003808

static void test_gl_raster_pos2i(void)
{
    mgras_reset();
    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_window_projection(qts);
    gl_word(qts, GL_RASTER_POS2I_HDR);
    gl_word(qts, 20);
    gl_word(qts, 30);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_X, 1), ==,
                    gl_bits(20 + GL_RASTER_BIAS));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_Y, 1), ==,
                    gl_bits(30 + GL_RASTER_BIAS));
    /* Below the window: culled, so the position stays (2.12). */
    gl_word(qts, GL_RASTER_POS2I_HDR);
    gl_word(qts, 5);
    gl_word(qts, (uint32_t)-3);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_X, 1), ==,
                    gl_bits(20 + GL_RASTER_BIAS));
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}
#define GL_INDEXI_HDR         0x18000204
#define GL_INDEXF_HDR         0x00000204

/*
 * In a colour-index context gr_osview's bars store their index as a CI8
 * pixel, which scans out through the colour map of the window's display
 * ID: map 29 for display ID 0's reset mode. Its clear stores the clear
 * index, glIndexMask keeps the bits its mask leaves clear (OpenGL 1.2.1,
 * 4.2.2), a bitmap takes the raster index, and 0x0900 returns the context
 * to RGBA.
 */
static void test_gl_colour_index_draws_through_the_colour_map(void)
{
    mgras_reset();
    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_one(qts, GL_INIT_CI_HDR, 8);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_window_projection(qts);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    mgras_write_cmap_entry(qts, 29, 2, 0x00ff0000);
    mgras_write_cmap_entry(qts, 29, 0x13, 0x0000ff00);

    gl_word(qts, GL_CLEAR_INDEX_HDR);
    gl_float(qts, 0x17);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 100, 100), ==, 0x17);
    g_assert_cmphex(mgras_test_format(qts, 256 + 100, 383 - 100), ==,
                    MGRAS_PIXEL_FORMAT_CI8);

    gl_indexs(qts, 2);
    gl_window_rect(qts, 10, 10, 20, 30);
    g_assert_cmphex(gl_window_pixel(qts, 15, 20), ==, 2);
    g_assert_cmphex(mgras_test_format(qts, 256 + 15, 383 - 20), ==,
                    MGRAS_PIXEL_FORMAT_CI8);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 15, 383 - 20), ==,
                    0x00ff00);
    g_assert_cmphex(gl_window_pixel(qts, 25, 20), ==, 0x17);

    /* 300 keeps its low 8 bits; the mask 0x0f keeps 0x17's high ones. */
    gl_word(qts, GL_INDEXI_HDR);
    gl_word(qts, 300);
    gl_window_rect(qts, 40, 10, 50, 30);
    g_assert_cmphex(gl_window_pixel(qts, 45, 20), ==, 44);
    gl_one(qts, GL_INDEX_MASK_HDR, 0x0f);
    gl_word(qts, GL_INDEXF_HDR);
    gl_float(qts, 3);
    gl_window_rect(qts, 60, 10, 70, 30);
    g_assert_cmphex(gl_window_pixel(qts, 65, 20), ==, 0x13);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 65, 383 - 20), ==,
                    0x0000ff);
    gl_one(qts, GL_INDEX_MASK_HDR, 0xff);

    /* A bitmap in the raster index: glIndexs(-1) is index 255. */
    gl_indexs(qts, -1);
    gl_raster_pos2f(qts, 80, 80);
    gl_bitmap_packet(qts, 8, 1, 0, 0, 0, 0);
    gl_bitmap_image(qts, (const uint32_t[]){ 0x81000000 }, 1);
    g_assert_cmphex(gl_window_pixel(qts, 80, 80), ==, 0xff);
    g_assert_cmphex(gl_window_pixel(qts, 81, 80), ==, 0x17);
    g_assert_cmphex(gl_window_pixel(qts, 87, 80), ==, 0xff);

    gl_word(qts, GL_INIT_RGB);
    gl_color3f(qts, 1, 1, 1);
    gl_window_rect(qts, 90, 10, 100, 30);
    g_assert_cmphex(gl_window_pixel(qts, 95, 20), ==, 0xffffff);
    g_assert_cmphex(mgras_test_format(qts, 256 + 95, 383 - 20), ==,
                    MGRAS_PIXEL_FORMAT_RGB);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * glIndexMask(0) leaves the colour buffer unchanged (OpenGL 1.2.1, 4.2.2):
 * a colour-index rectangle under it keeps the direct-colour pixel an RGBA
 * context drew there, its value and its format.
 */
static void test_gl_colour_index_zero_mask_keeps_the_pixel(void)
{
    mgras_reset();
    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_window_projection(qts);
    gl_color3f(qts, 1, 0.5f, 0);
    gl_window_rect(qts, 10, 10, 20, 30);
    g_assert_cmphex(gl_window_pixel(qts, 15, 20), ==, 0xff8000);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_one(qts, GL_INIT_CI_HDR, 8);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_window_projection(qts);
    gl_one(qts, GL_INDEX_MASK_HDR, 0);
    gl_indexs(qts, 7);
    gl_window_rect(qts, 10, 10, 20, 30);
    g_assert_cmphex(gl_window_pixel(qts, 15, 20), ==, 0xff8000);
    g_assert_cmphex(mgras_test_format(qts, 256 + 15, 383 - 20), ==,
                    MGRAS_PIXEL_FORMAT_RGB);

    gl_one(qts, GL_INDEX_MASK_HDR, 0xff);
    gl_window_rect(qts, 10, 10, 20, 30);
    g_assert_cmphex(gl_window_pixel(qts, 15, 20), ==, 7);
}

/*
 * A double-buffered colour-index visual's back buffer holds indices as
 * its front does. __glMgrInitCI sends 0x0a04 once for each colour buffer
 * func_d9502f0 sets up, and it sets up a second one when the visual is
 * double-buffered, which bz's context shows by sending 0x0a04 twice. So
 * an index drawn into bank 1 is a CI8 pixel there as in bank 0, showing
 * through the colour map of the pixel's display ID once the buffer-select
 * mask selects bank 1 for that ID, under the index mask, and bank 0 is
 * left alone. An RGBA draw after it stores direct colour again.
 */
static void test_gl_colour_index_draws_into_bank_one(void)
{
    mgras_reset();
    gl_colour_index_back_buffer(qts);
    gl_word(qts, GL_CLEAR_INDEX_HDR);
    gl_float(qts, 5);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_indexs(qts, 0x13);
    gl_window_rect(qts, 10, 10, 20, 30);
    gl_raster_pos2f(qts, 80, 80);
    gl_bitmap_packet(qts, 8, 1, 0, 0, 0, 0);
    gl_bitmap_image(qts, (const uint32_t[]){ 0x80000000 }, 1);
    /* 0x13 under the mask 0xf0 over 5 keeps 5's low bits: 0x15. */
    gl_one(qts, GL_INDEX_MASK_HDR, 0xf0);
    gl_window_rect(qts, 40, 10, 50, 30);
    gl_one(qts, GL_INDEX_MASK_HDR, 0xff);

    g_assert_cmphex(mgras_test_pixel(qts, 256 + 100, 383 - 100), ==, 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 100, 383 - 100), ==,
                    0);
    qtest_writeb(qts, REG_XMAP_INDEX, 0);
    qtest_writel(qts, REG_XMAP_BUF_SELECT, 1u << 14);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 100, 383 - 100), ==,
                    0xff0000);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 15, 383 - 20), ==,
                    0x0000ff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 80, 383 - 80), ==,
                    0x0000ff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 81, 383 - 80), ==,
                    0xff0000);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 45, 383 - 20), ==,
                    0x00ff00);

    gl_word(qts, GL_INIT_RGB);
    gl_color3f(qts, 1, 1, 1);
    gl_window_rect(qts, 90, 10, 100, 30);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256 + 95, 383 - 20), ==,
                    0xffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 256 + 95, 383 - 20), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * Swapping a double-buffered colour-index window shows the bank drawn
 * last: frame A into bank 1, a swap that selects bank 1 for display ID 14
 * and moves the back buffer to bank 0, frame B there, which stays hidden
 * until the swap back.
 */
static void test_gl_colour_index_swap_shows_the_drawn_bank(void)
{
    mgras_reset();
    gl_colour_index_back_buffer(qts);
    gl_word(qts, GL_CLEAR_INDEX_HDR);
    gl_float(qts, 5);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_swap(qts, 0, 1u << 14);
    g_assert_cmphex(mgras_test_display_pixel(qts, 300, 300), ==, 0xff0000);

    gl_word(qts, GL_CLEAR_INDEX_HDR);
    gl_float(qts, 0x13);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0x13);
    g_assert_cmphex(mgras_test_display_pixel(qts, 300, 300), ==, 0xff0000);
    gl_swap(qts, 1, 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 300, 300), ==, 0x0000ff);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * A deep double-buffered window's client draws BACK as draw-buffer bit 2
 * only, and the kernel moves the bank token and the select bit together at
 * each swap. The pair is bank 0 and slot 2: frame A lands in slot 2 and the
 * first swap, bit set, shows it; frame B lands in bank 0 behind it and the
 * swap back, bit clear, shows that. Read through scanout, so a model that
 * draws the frames right but displays bank 1 shows black.
 */
static void test_gl_deep_swap_shows_the_drawn_buffer_ssi(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS MGRAS_TWO_RE_ARGS);

    gl_deep_scene(s);
    mgras_tag_wid(s, 256, 256, 383, 383, 14);
    gl_draw_buffer(s, 4);
    gl_clear_color(s, 1, 0, 1, 1);
    gl_word(s, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_display_pixel(s, 300, 300), ==, 0);
    gl_swap(s, 0, 1u << 14);
    g_assert_cmphex(mgras_test_display_pixel(s, 300, 300), ==, 0xff00ff);

    gl_clear_color(s, 0, 1, 0, 1);
    gl_word(s, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(s, 300, 300), ==, 0x00ff00);
    g_assert_cmphex(mgras_test_display_pixel(s, 300, 300), ==, 0xff00ff);
    gl_swap(s, 1, 0);
    g_assert_cmphex(mgras_test_display_pixel(s, 300, 300), ==, 0x00ff00);

    gl_clear_color(s, 0, 0, 1, 1);
    gl_word(s, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_slot_pixel(s, 0, 300, 300), ==, 0x0000ff);
    gl_swap(s, 0, 1u << 14);
    g_assert_cmphex(mgras_test_display_pixel(s, 300, 300), ==, 0x0000ff);
    qtest_quit(s);
}

/* A shallow back buffer keeps showing bank 1 on the same board. */
static void test_gl_shallow_swap_still_shows_bank_one_ssi(void)
{
    QTestState *s = qtest_init(MACHINE_ARGS MGRAS_TWO_RE_ARGS);

    gl_deep_scene(s);
    mgras_tag_wid(s, 256, 256, 383, 383, 14);
    gl_draw_buffer(s, 2);
    gl_clear_color(s, 1, 0, 1, 1);
    gl_word(s, GL_CLEAR_COLOR_BUFFER);
    gl_swap(s, 0, 1u << 14);
    g_assert_cmphex(mgras_test_display_pixel(s, 300, 300), ==, 0xff00ff);
    qtest_quit(s);
}

/*
 * A context setup sends 0x8c04 after the opener with the RE count shifted
 * right once: 0 on SI, 1 on SSI. The board consumes it as one word, records
 * it, and the next word is a header again.
 */
static void gl_context_re_count_token(const char *args, uint32_t count)
{
    QTestState *s = qtest_init(args);

    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-gl-re-count-tokens"), ==, 0);
    gl_enter_slot(s, GL_SLOT_A);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_one(s, GL_RE_COUNT_HDR, count);
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-gl-re-count-tokens"), ==, 1);
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-gl-re-count-word"), ==,
                    count);
    g_assert_cmphex(qtest_readl(s, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==, 0);
    qtest_writel(s, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(s, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    qtest_quit(s);
}

static void test_gl_context_re_count_token_si(void)
{
    gl_context_re_count_token(MACHINE_ARGS, 0);
}

static void test_gl_context_re_count_token_ssi(void)
{
    gl_context_re_count_token(MACHINE_ARGS MGRAS_TWO_RE_ARGS, 1);
}

/*
 * The client reads GL_CURRENT_INDEX through the 0xa104 query at index 4,
 * GL_CURRENT_COLOR's red, and GL_CURRENT_RASTER_INDEX through 0xa20c at
 * 0x351, the raster colour's red. In a
 * colour-index context both answer the index as a float; in an RGBA one,
 * red.
 */
static void test_hq_query_answers_the_colour_index(void)
{
    mgras_reset();
    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_one(qts, GL_INIT_CI_HDR, 8);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_color3f(qts, 0.25f, 0.5f, 0.75f);
    gl_indexs(qts, 9);
    g_assert_cmphex(gl_attribute_query(qts, 4), ==, gl_bits(9));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_INDEX, 1), ==, gl_bits(1));
    gl_raster_pos2f(qts, 0, 0);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_INDEX, 1), ==, gl_bits(9));

    /* The short is signed: glIndexs(-1) is the index -1. */
    gl_indexs(qts, -1);
    g_assert_cmphex(gl_attribute_query(qts, 4), ==, gl_bits(-1));

    gl_word(qts, GL_INIT_RGB);
    g_assert_cmphex(gl_attribute_query(qts, 4), ==, gl_bits(0.25f));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_INDEX, 1), ==, gl_bits(0.25f));
}

/*
 * The optional subsections a migration stream can carry for GL state:
 * MGRAS's own, and the shared context description's, each sent only while
 * its state has left what a reset gives.
 */
enum {
    GL_SUB_BLOCK = 1u << 0,
    GL_SUB_READBACK = 1u << 1,
    GL_SUB_PIXELS = 1u << 2,
    GL_SUB_PIXEL_FLAGS = 1u << 3,
    GL_SUB_SCISSOR = 1u << 4,
    GL_SUB_ALPHA_TEST = 1u << 5,
    GL_SUB_STENCIL = 1u << 6,
    GL_SUB_CLEAR_STENCIL = 1u << 7,
    GL_SUB_BLEND = 1u << 8,
    GL_SUB_LOGIC_OP = 1u << 9,
    GL_SUB_COLOUR_MASK = 1u << 10,
    GL_SUB_FOG = 1u << 11,
    GL_SUB_POLYGON_OFFSET = 1u << 12,
    GL_SUB_CLIP_PLANES = 1u << 13,
    GL_SUB_RASTERIZATION = 1u << 14,
};

/* Which of those a migration stream from qts carries, as GL_SUB_ bits. */
static unsigned gl_stream_subsections(QTestState *qts)
{
    static const char *const names[] = {
        "sgi-mgras/gl-block", "sgi-mgras/gl-readback",
        "sgi-mgras/gl-pixels", "sgi-mgras/gl-pixel-flags",
        "sgi-gl-draw-state/scissor", "sgi-gl-draw-state/alpha-test",
        "sgi-gl-draw-state/stencil", "sgi-gl-context/clear-stencil",
        "sgi-gl-draw-state/blend", "sgi-gl-draw-state/logic-op",
        "sgi-gl-draw-state/colour-mask", "sgi-gl-draw-state/fog",
        "sgi-gl-draw-state/polygon-offset", "sgi-gl-draw-state/clip-planes",
        "sgi-gl-context/rasterization",
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *contents = NULL;
    GError *err = NULL;
    gsize length;
    unsigned found = 0;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-stream-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_strdup_printf("%s/stream", tmpfs);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(qts, uri);
    g_assert_true(g_file_get_contents(path, &contents, &length, &err));
    g_assert_no_error(err);
    /* The shared context description is always sent. */
    g_assert_nonnull(memmem(contents, length, "sgi-mgras/gl-contexts",
                            strlen("sgi-mgras/gl-contexts")));
    for (int i = 0; i < ARRAY_SIZE(names); i++) {
        if (memmem(contents, length, names[i], strlen(names[i]))) {
            found |= 1u << i;
        }
    }
    unlink(path);
    g_rmdir(tmpfs);
    return found;
}

/*
 * The GL subsections' rules, each against a state that meets only it.
 * An RGBA context that drew, moved its raster position and holds a
 * vertex with a colour index other than the initial 1 sends none: all of
 * that is the shared context's base state. A glBitmap image under way
 * sends gl-block.
 */
static void test_gl_subsections_follow_their_state(void)
{
    QTestState *s;

    s = qtest_init(MIGRATION_ARGS);
    gl_enter_slot(s, GL_SLOT_A);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_viewport(s, 0, 0, 128, 128);
    gl_raster_pos2f(s, 0.5f, 0.5f);
    gl_indexs(s, 5);
    gl_word(s, GL_BEGIN_TRIANGLES);
    gl_vertex2f(s, 0, 0);
    gl_vertex2f(s, 0.5f, 0);
    gl_vertex2f(s, 0, 0.5f);
    gl_vertex2f(s, -0.5f, 0);
    gl_indexs(s, 1);
    g_assert_cmpuint(gl_stream_subsections(s), ==, 0);
    qtest_quit(s);

    s = qtest_init(MIGRATION_ARGS);
    gl_enter_slot(s, GL_SLOT_A);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_bitmap_packet(s, 16, 4, 0, 0, 0, 0);
    gl_word(s, GL_INLINE_BLOCK | 8);
    gl_word(s, 0xffff0000);
    g_assert_cmpuint(gl_stream_subsections(s), ==, GL_SUB_BLOCK);
    qtest_quit(s);
}
#define GL_Q_DEPTH            0x2ca

static void test_hq_query_answers_the_raster_state(void)
{
    uint32_t a;

    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    a = gl_query(qts, GL_Q_DEPTH, 1);
    g_assert_cmphex((a & 7) + 0x200, ==, 0x201);
    g_assert_cmphex(a & 0x0ffffff0, ==, 0x0ffffff0);
    a = gl_query(qts, GL_Q_PIXEL_ENABLES, 1);
    g_assert_cmphex(a & 1, ==, 1);
    g_assert_cmphex(a & 0x50, ==, 0);
    g_assert_cmphex(((a >> 26) & 0xf) + 0x1500, ==, 0x1503);
    g_assert_cmphex(gl_query(qts, GL_Q_FILL_ENABLES, 1) & 0x20021, ==, 0);
    g_assert_cmphex((gl_query(qts, GL_Q_STENCIL, 1) & 7) + 0x200, ==, 0x207);
    g_assert_cmphex(gl_query(qts, GL_Q_STENCIL_MASKS, 1) & 0xffff, ==,
                    0xffff);
    g_assert_cmphex((gl_query(qts, GL_Q_ALPHA, 1) & 7) + 0x200, ==, 0x207);

    gl_one(qts, GL_DITHER_HDR, 0);
    gl_one(qts, GL_DEPTH_FUNC_HDR, 3);
    gl_one(qts, GL_DEPTH_MASK_HDR, 0);
    a = gl_query(qts, GL_Q_DEPTH, 1);
    g_assert_cmphex((a & 7) + 0x200, ==, 0x203);
    g_assert_cmphex(a & 0x0ffffff0, ==, 0);
    a = gl_query(qts, GL_Q_PIXEL_ENABLES, 1);
    g_assert_cmphex(a & 1, ==, 0);
    g_assert_cmphex(((a >> 26) & 0xf) + 0x1500, ==, 0x1503);
    gl_one(qts, GL_DITHER_HDR, 3);
    g_assert_cmphex(gl_query(qts, GL_Q_PIXEL_ENABLES, 1) & 1, ==, 1);
}

/*
 * Two-sided lighting's back colour goes to the back face that 0x5604
 * names: with GL_CW the counter-clockwise triangle is the back and takes
 * the back material's green over the 0.04 ambient, and the clockwise one
 * the front colour, the ambient alone (the normal faces away).
 */
static void test_gl_front_face_selects_the_two_sided_colour(void)
{
    gl_lit_start(qts);
    gl_material(qts, GL_FRONT_FACE, GL_DIFFUSE, 1, 0, 0, 1);
    gl_material(qts, GL_BACK_FACE, GL_DIFFUSE, 0, 1, 0, 1);
    gl_three(qts, GL_NORMAL3F_HDR, 0, 0, -1);
    gl_one(qts, GL_LIGHT_MODEL_TWO_SIDE_HDR, 1);
    gl_one(qts, GL_FRONT_FACE_HDR, GL_CW);
    g_assert_cmphex(gl_lit_triangle(qts, false), ==, 0x0aff0a);
    g_assert_cmphex(gl_lit_triangle(qts, true), ==, 0x0a0a0a);
}

/*
 * glMapGrid1f 0xae10 and four words, glMapGrid2f 0xaf20 and eight and
 * glClearAccum 0xbb10 and four are consumed, not drawn: a clear after
 * them still parses and draws.
 */
static void test_gl_grids_and_accum_are_consumed(void)
{
    static const uint32_t packets[][9] = {
        { 0x0000ae10, 1, 0, 0x3f800000, 0x3f800000 },
        { 0x0000af20, 1, 0, 0x3f800000, 0x3f800000, 1, 0, 0x3f800000,
          0x3f800000 },
        { 0x0000bb10, 0, 0, 0, 0 },
    };

    gl_scene_start(qts, GL_FLAT);
    for (int i = 0; i < ARRAY_SIZE(packets); i++) {
        for (int k = 0; k <= (packets[i][0] & 0xff) / 4; k++) {
            gl_word(qts, packets[i][k]);
        }
    }
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x0000ff);
}

/*
 * GL_INDEX_WRITEMASK and GL_COLOR_WRITEMASK are both query 0x32b: the
 * index mask in colour-index mode, and red, green, blue and alpha in bits
 * 0 to 3 in RGBA mode, all set as OpenGL starts.
 */
#define GL_Q_WRITEMASK        0x32b

static void test_hq_query_answers_the_write_mask(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    g_assert_cmphex(gl_query(qts, GL_Q_WRITEMASK, 1) & 0xf, ==, 0xf);
    gl_one(qts, GL_INIT_CI_HDR, 8);
    gl_one(qts, GL_INDEX_MASK_HDR, 0x3c);
    g_assert_cmphex(gl_query(qts, GL_Q_WRITEMASK, 1) & 0xff, ==, 0x3c);
    gl_word(qts, GL_INIT_RGB);
    g_assert_cmphex(gl_query(qts, GL_Q_WRITEMASK, 1) & 0xf, ==, 0xf);
}

/*
 * The rules of the optional GL state subsections, each against a state
 * that meets only it. What every context setup sends, the opener,
 * glEnable(GL_DITHER) as 0x6704 with 3, glDepthRange(0, 1) and a
 * viewport, leaves the optional state as a reset has it and sends none of
 * them; so do the lighting, texture-matrix and culling state, which are
 * the shared context's base state. glGetLightfv's readback sends
 * gl-readback.
 */
static void test_gl_state_subsections_follow_their_state(void)
{
    QTestState *s;

    s = qtest_init(MIGRATION_ARGS);
    gl_enter_slot(s, GL_SLOT_A);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_one(s, GL_DITHER_HDR, 3);
    gl_depth_range(s, 0, 1);
    gl_viewport(s, 0, 0, 128, 128);
    g_assert_cmpuint(gl_stream_subsections(s), ==, 0);
    qtest_quit(s);

    s = qtest_init(MIGRATION_ARGS);
    gl_enter_slot(s, GL_SLOT_A);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_depth_range(s, 0.25f, 1);
    gl_one(s, GL_DITHER_HDR, 0);
    gl_word(s, GL_COLOR_MATERIAL_ON);
    gl_one(s, GL_MATRIX_MODE_HDR, GL_TEXTURE);
    gl_word(s, GL_PUSH_MATRIX);
    gl_word(s, GL_LIGHTING_ON);
    gl_word(s, GL_CULL_ON);
    g_assert_cmpuint(gl_stream_subsections(s), ==, 0);
    qtest_quit(s);

    s = qtest_init(MIGRATION_ARGS);
    gl_enter_slot(s, GL_SLOT_A);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_word(s, GL_GET_LIGHT_HDR);
    gl_word(s, 0);
    gl_word(s, GL_DIFFUSE);
    g_assert_cmpuint(gl_stream_subsections(s), ==, GL_SUB_READBACK);
    qtest_quit(s);

    /*
     * Pixels: one GE parameter load alone, or an inline draw waiting,
     * sends gl-pixels; a draw 0x8d20 marks as DMA waiting, or an inline
     * one another header has come after, sends gl-pixel-flags too. The
     * contexts above, which loaded nothing, sent neither.
     */
    for (int i = 0; i < 4; i++) {
        s = qtest_init(MIGRATION_ARGS);
        gl_enter_slot(s, GL_SLOT_A);
        gl_word(s, GL_CONTEXT_OPENER);
        if (i == 0) {
            gl_ge_load(s, GL_GE_CRAM, GL_GE_ZOOM,
                       (const uint32_t[]){ 0, 0x3f800000 }, 2);
        } else {
            gl_pixels_program(s, GL_PIX_4UB, 8, 0, 0, 1, 0, 1);
            gl_pixels_draw_as(s, 2, i == 2 ? GL_PIXELS_BY_DMA :
                                             GL_PIXELS_INLINE);
        }
        if (i == 3) {
            gl_word(s, GL_FLUSH);
        }
        g_assert_cmpuint(gl_stream_subsections(s), ==,
                         i >= 2 ? GL_SUB_PIXELS | GL_SUB_PIXEL_FLAGS :
                                  GL_SUB_PIXELS);
        qtest_quit(s);
    }

    /*
     * The scissor: a box or the enable sends its subsection, and the test
     * turned off again with no box sends nothing.
     */
    for (int i = 0; i < 3; i++) {
        s = qtest_init(MIGRATION_ARGS);
        gl_enter_slot(s, GL_SLOT_A);
        gl_word(s, GL_CONTEXT_OPENER);
        if (i == 0) {
            gl_scissor(s, 0, 0, 1, 1);
        } else {
            gl_one(s, GL_SCISSOR_TEST_HDR, 1);
        }
        if (i == 2) {
            gl_one(s, GL_SCISSOR_TEST_HDR, 0);
        }
        g_assert_cmpuint(gl_stream_subsections(s), ==,
                         i == 2 ? 0 : GL_SUB_SCISSOR);
        qtest_quit(s);
    }

    /*
     * The alpha test: its enable or a function or reference other than
     * GL_ALWAYS and 0 sends its subsection; the test turned off again sends
     * nothing.
     */
    for (int i = 0; i < 4; i++) {
        s = qtest_init(MIGRATION_ARGS);
        gl_enter_slot(s, GL_SLOT_A);
        gl_word(s, GL_CONTEXT_OPENER);
        if (i < 2) {
            gl_one(s, GL_ALPHA_TEST_HDR, 1);
        }
        if (i == 1) {
            gl_one(s, GL_ALPHA_TEST_HDR, 0);
        }
        if (i == 2) {
            gl_alpha_func(s, GL_GREATER, 0);
        }
        if (i == 3) {
            gl_alpha_func(s, 7, 1);
        }
        g_assert_cmpuint(gl_stream_subsections(s), ==,
                         i == 1 ? 0 : GL_SUB_ALPHA_TEST);
        qtest_quit(s);
    }

    /*
     * The stencil test: its enable, a function or a write mask sends its
     * subsection, a clear value the context's clear-stencil subsection,
     * and the test turned off again sends nothing. The buffer's values
     * travel in the depth plane, whatever they are.
     */
    for (int i = 0; i < 6; i++) {
        static const unsigned want[6] = {
            GL_SUB_STENCIL, 0, GL_SUB_STENCIL, GL_SUB_STENCIL,
            GL_SUB_CLEAR_STENCIL, GL_SUB_CLEAR_STENCIL,
        };

        s = qtest_init(MIGRATION_ARGS);
        gl_enter_slot(s, GL_SLOT_A);
        gl_word(s, GL_CONTEXT_OPENER);
        if (i < 2) {
            gl_one(s, GL_STENCIL_TEST_HDR, 1);
        }
        if (i == 1) {
            gl_one(s, GL_STENCIL_TEST_HDR, 0);
        }
        if (i == 2) {
            gl_stencil(s, GL_STENCIL_EQUAL, 0, 0xffffffff, GL_OP_KEEP,
                       GL_OP_KEEP, GL_OP_KEEP);
        }
        if (i == 3) {
            gl_one(s, GL_STENCIL_MASK_HDR, 0xff);
        }
        if (i >= 4) {
            gl_one(s, GL_CLEAR_STENCIL_HDR, i == 4 ? 9 : 0);
            gl_word(s, GL_CLEAR_STENCIL_BUFFER);
            if (i == 5) {
                gl_one(s, GL_CLEAR_STENCIL_HDR, 1);
            }
        }
        g_assert_cmpuint(gl_stream_subsections(s), ==, want[i]);
        qtest_quit(s);
    }

    /*
     * Blending: its enable, factors other than the setup's GL_ONE and
     * GL_ZERO, or a constant colour sends its subsection; the setup's own
     * factors and equation, and blending turned off again, send nothing.
     */
    for (int i = 0; i < 4; i++) {
        s = qtest_init(MIGRATION_ARGS);
        gl_enter_slot(s, GL_SLOT_A);
        gl_word(s, GL_CONTEXT_OPENER);
        gl_blend(s, 1, 0, GL_FUNC_ADD);
        if (i < 2) {
            gl_one(s, GL_BLEND_HDR, 1);
        }
        if (i == 1) {
            gl_one(s, GL_BLEND_HDR, 0);
        }
        if (i == 2) {
            gl_blend(s, GL_SRC_ALPHA, 0, GL_FUNC_ADD);
        }
        if (i == 3) {
            gl_word(s, GL_BLEND_COLOR_HDR);
            gl_float(s, 0);
            gl_float(s, 0);
            gl_float(s, 0);
            gl_float(s, 1);
        }
        g_assert_cmpuint(gl_stream_subsections(s), ==,
                         i == 1 ? 0 : GL_SUB_BLEND);
        qtest_quit(s);
    }

    /*
     * The logical operation: its enable or an operation other than
     * GL_COPY sends its subsection; the setup's GL_COPY, and the operation
     * turned off again, send nothing.
     */
    for (int i = 0; i < 3; i++) {
        s = qtest_init(MIGRATION_ARGS);
        gl_enter_slot(s, GL_SLOT_A);
        gl_word(s, GL_CONTEXT_OPENER);
        gl_one(s, GL_LOGIC_OP_HDR, i == 2 ? GL_LOGIC_XOR : 3);
        if (i < 2) {
            gl_one(s, GL_LOGIC_OP_ENABLE_HDR, 1);
        }
        if (i == 1) {
            gl_one(s, GL_LOGIC_OP_ENABLE_HDR, 0);
        }
        g_assert_cmpuint(gl_stream_subsections(s), ==,
                         i == 1 ? 0 : GL_SUB_LOGIC_OP);
        qtest_quit(s);
    }

    /* A masked channel sends its subsection; every channel again, nothing. */
    for (int i = 0; i < 2; i++) {
        s = qtest_init(MIGRATION_ARGS);
        gl_enter_slot(s, GL_SLOT_A);
        gl_word(s, GL_CONTEXT_OPENER);
        gl_one(s, GL_COLOR_MASK_HDR, 7);
        if (i == 1) {
            gl_one(s, GL_COLOR_MASK_HDR, 0xf);
        }
        g_assert_cmpuint(gl_stream_subsections(s), ==,
                         i ? 0 : GL_SUB_COLOUR_MASK);
        qtest_quit(s);
    }

    /*
     * Fog: its enable or a parameter other than OpenGL's initial one
     * sends its subsection; the initial parameters, and fog turned off again,
     * send nothing.
     */
    for (int i = 0; i < 3; i++) {
        s = qtest_init(MIGRATION_ARGS);
        gl_enter_slot(s, GL_SLOT_A);
        gl_word(s, GL_CONTEXT_OPENER);
        gl_word(s, GL_FOG_END_HDR);
        gl_float(s, i == 2 ? 3 : 1);
        if (i < 2) {
            gl_one(s, GL_FOG_HDR, 1);
        }
        if (i == 1) {
            gl_one(s, GL_FOG_HDR, 0);
        }
        g_assert_cmpuint(gl_stream_subsections(s), ==,
                         i == 1 ? 0 : GL_SUB_FOG);
        qtest_quit(s);
    }

    /*
     * Polygon offset: an enable or a nonzero factor, units or bias sends
     * its subsection; the setup's zero EXT offset, and the enables
     * cleared again, send nothing.
     */
    for (int i = 0; i < 3; i++) {
        s = qtest_init(MIGRATION_ARGS);
        gl_enter_slot(s, GL_SLOT_A);
        gl_word(s, GL_CONTEXT_OPENER);
        gl_polygon_offset(s, 0, i == 2 ? 2 : 0, true);
        if (i < 2) {
            gl_one(s, GL_OFFSET_ENABLES_HDR, 4);
        }
        if (i == 1) {
            gl_one(s, GL_OFFSET_ENABLES_HDR, 0);
        }
        g_assert_cmpuint(gl_stream_subsections(s), ==,
                         i == 1 ? 0 : GL_SUB_POLYGON_OFFSET);
        qtest_quit(s);
    }

    /*
     * Client clip planes: an enable or a plane other than zero sends
     * its subsection; the setup's zero planes, disabled, send nothing.
     */
    for (int i = 0; i < 3; i++) {
        s = qtest_init(MIGRATION_ARGS);
        gl_enter_slot(s, GL_SLOT_A);
        gl_word(s, GL_CONTEXT_OPENER);
        gl_clip_plane(s, 5, 0, 0, 0, i == 2 ? 1 : 0);
        if (i < 2) {
            gl_one(s, GL_CLIP_PLANE_ON_HDR, GL_CLIP_PLANE0 + 5);
        }
        if (i == 1) {
            gl_one(s, GL_CLIP_PLANE_OFF_HDR, GL_CLIP_PLANE0 + 5);
        }
        g_assert_cmpuint(gl_stream_subsections(s), ==,
                         i == 1 ? 0 : GL_SUB_CLIP_PLANES);
        qtest_quit(s);
    }

    /*
     * Point, line and polygon state: an enable or a width other than 1
     * sends the rasterization subsection; the setup's width 1 and solid
     * stipple, and the
     * enable turned off again, send nothing.
     */
    for (int i = 0; i < 3; i++) {
        s = qtest_init(MIGRATION_ARGS);
        gl_enter_slot(s, GL_SLOT_A);
        gl_word(s, GL_CONTEXT_OPENER);
        gl_word(s, GL_LINE_WIDTH_HDR);
        gl_float(s, i == 2 ? 2 : 1);
        gl_word(s, GL_LINE_STIPPLE_HDR);
        gl_word(s, 0);
        gl_word(s, 0xffff);
        if (i < 2) {
            gl_one(s, GL_POLYGON_STIPPLE_HDR, 1);
        }
        if (i == 1) {
            gl_one(s, GL_POLYGON_STIPPLE_HDR, 0);
        }
        g_assert_cmpuint(gl_stream_subsections(s), ==,
                         i == 1 ? 0 : GL_SUB_RASTERIZATION);
        qtest_quit(s);
    }
}
int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init(MACHINE_ARGS);


    qtest_add_func("sn0/origin2000/mgras/gl-context-re-count-token/si",
                   test_gl_context_re_count_token_si);
    qtest_add_func("sn0/origin2000/mgras/gl-context-re-count-token/ssi",
                   test_gl_context_re_count_token_ssi);
    qtest_add_func("sn0/origin2000/mgras/hq-query-sets-flag-after-arguments",
                   test_hq_query_sets_flag_after_arguments);
    qtest_add_func("sn0/origin2000/mgras/hq-query-answers-the-matrices",
                   test_hq_query_answers_the_matrices);
    qtest_add_func("sn0/origin2000/mgras/hq-query-answers-the-raster-position",
                   test_hq_query_answers_the_raster_position);
    qtest_add_func("sn0/origin2000/mgras/hq-query-answers-the-colour-index",
                   test_hq_query_answers_the_colour_index);
    qtest_add_func("sn0/origin2000/mgras/hq-query-answers-the-write-mask",
                   test_hq_query_answers_the_write_mask);
    qtest_add_func("sn0/origin2000/mgras/hq-query-answers-the-raster-state",
                   test_hq_query_answers_the_raster_state);
    qtest_add_func("sn0/origin2000/mgras/gl-raster-pos2i",
                   test_gl_raster_pos2i);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-colour-index-draws-through-the-colour-map",
                   test_gl_colour_index_draws_through_the_colour_map);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-colour-index-zero-mask-keeps-the-pixel",
                   test_gl_colour_index_zero_mask_keeps_the_pixel);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-colour-index-draws-into-bank-one",
                   test_gl_colour_index_draws_into_bank_one);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-colour-index-swap-shows-the-drawn-bank",
                   test_gl_colour_index_swap_shows_the_drawn_bank);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-deep-swap-shows-the-drawn-buffer-ssi",
                   test_gl_deep_swap_shows_the_drawn_buffer_ssi);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-shallow-swap-still-shows-bank-one-ssi",
                   test_gl_shallow_swap_still_shows_bank_one_ssi);
    qtest_add_func("sn0/origin2000/mgras/hq-query-flag-only-from-query",
                   test_hq_query_flag_only_from_query);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-ortho-matches-the-reference",
                   test_gl_ortho_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-frustum-matches-the-reference",
                   test_gl_frustum_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-load-matrix-reads-columns",
                   test_gl_load_matrix_reads_columns);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-mult-matrix-multiplies-on-the-right",
                   test_gl_mult_matrix_multiplies_on_the_right);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-matrix-mode-selects-the-matrix",
                   test_gl_matrix_mode_selects_the_matrix);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-matrix-product-is-single-precision",
                   test_gl_matrix_product_is_single_precision);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-context-setup-restores-initial-state",
                   test_gl_context_setup_restores_initial_state);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-back-buffer-draws-into-bank-one",
                   test_gl_back_buffer_draws_into_bank_one);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-front-and-back-draw-into-both-banks",
                   test_gl_front_and_back_draw_into_both_banks);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-front-buffer-is-the-other-bank",
                   test_gl_front_buffer_is_the_other_bank);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-bank-token-moves-the-back-buffer",
                   test_gl_bank_token_moves_the_back_buffer);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-context-image-restores-the-banks",
                   test_gl_context_image_restores_the_banks);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-swap-matches-the-reference",
                   test_gl_swap_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-swap-pending-matches-the-reference",
                   test_gl_swap_pending_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/gl-state-follows-the-context-slot",
                   test_gl_state_follows_the_context_slot);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-context-opener-resets-only-its-context",
                   test_gl_context_opener_resets_only_its_context);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-images-without-a-slot-share-one-state",
                   test_gl_images_without_a_slot_share_one_state);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-context-table-replaces-the-least-recent",
                   test_gl_context_table_replaces_the_least_recent);
    qtest_add_func("sn0/origin2000/mgras/gl-parser-follows-the-context",
                   test_gl_parser_follows_the_context);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-packet-split-by-a-context-switch",
                   test_gl_packet_split_by_a_context_switch);
    qtest_add_func("sn0/origin2000/mgras/gl-new-context-starts-at-a-header",
                   test_gl_new_context_starts_at_a_header);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-new-context-starts-in-the-initial-state",
                   test_gl_new_context_starts_in_the_initial_state);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-default-context-keeps-its-packet",
                   test_gl_default_context_keeps_its_packet);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-new-context-without-a-slot-keeps-the-default",
                   test_gl_new_context_without_a_slot_keeps_the_default);
    qtest_add_func("sn0/origin2000/mgras/gl-bit-31-alone-keeps-the-packet",
                   test_gl_bit_31_alone_keeps_the_packet);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-transforms-multiply-on-the-right",
                   test_gl_transforms_multiply_on_the_right);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-push-and-pop-restore-the-matrix",
                   test_gl_push_and_pop_restore_the_matrix);
    qtest_add_func("sn0/origin2000/mgras/gl-matrix-stack-holds-32",
                   test_gl_matrix_stack_holds_32);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-front-face-selects-the-two-sided-colour",
                   test_gl_front_face_selects_the_two_sided_colour);
    qtest_add_func("sn0/origin2000/mgras/gl-grids-and-accum-are-consumed",
                   test_gl_grids_and_accum_are_consumed);
    qtest_add_func("sn0/origin2000/mgras/gl-subsections-follow-their-state",
                   test_gl_subsections_follow_their_state);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-state-subsections-follow-their-state",
                   test_gl_state_subsections_follow_their_state);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
