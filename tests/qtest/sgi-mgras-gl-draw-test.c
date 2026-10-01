/*
 * SI Viz register-contract and protocol qtests: GL primitive, clip, depth
 * and lighting drawing cases.
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
 * An inline data block's header has bit 31 set and a 20-bit byte length.
 * The GL client's first GE table load is 0x80000214: five descriptor words
 * and 128 data words. Filling the block with fence tokens shows whether
 * any of it parsed as headers.
 */
static void test_inline_block_uses_20_bit_length(void)
{
    mgras_reset();

    mgras_enter_context(qts);
    qtest_writel(qts, REG_CONTEXT_COMMAND, 0x80000214);
    for (int i = 0; i < 0x214 / 4; i++) {
        qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    }
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);
    qtest_writel(qts, REG_CONTEXT_COMMAND, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
}

/*
 * The probe's clear case: glViewport(0, 0, 128, 128), glClearColor(1, 0,
 * 1, 1), glClear(GL_COLOR_BUFFER_BIT), glFlush, in a 128x128 window whose
 * top-left pixel is at (256, 256). Every window pixel becomes magenta and
 * nothing outside it changes (OpenGL 1.2.1, 4.2.3: a clear passes only the
 * pixel ownership, scissor and dither steps, and colour 1.0 converts to
 * all ones by 2.13.9).
 */
static void test_gl_clear_fills_the_window(void)
{
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();

    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(qts, 0, 0, 128, 128);
    gl_clear_color(qts, 1, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_FLUSH);

    g_assert_cmphex(mgras_test_pixel(qts, 256, 256), ==, 0xff00ff);
    g_assert_cmphex(mgras_test_format(qts, 256, 256), ==,
                    MGRAS_PIXEL_FORMAT_RGB);
    rgb = mgras_screendump(qts);
    for (int y = 250; y < 390; y++) {
        for (int x = 250; x < 390; x++) {
            bool inside = x >= 256 && x < 384 && y >= 256 && y < 384;

            if (rgb[y * 1280 + x] != (inside ? 0xff00ff : 0)) {
                g_error("pixel (%d, %d) is 0x%06x", x, y, rgb[y * 1280 + x]);
            }
        }
    }
}

/*
 * A clear covers the window, not the viewport (OpenGL 1.2.1, 4.2.3), and
 * lands wherever the context image puts the window.
 */
static void test_gl_clear_ignores_the_viewport(void)
{
    mgras_reset();

    /* Rows 200-231 from the bottom are screen rows 792-823. */
    mgras_enter_gl_window(qts, 100, 200, 64, 32, 0x11);
    gl_viewport(qts, 0, 0, 8, 8);
    gl_clear_color(qts, 0, 1, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);

    g_assert_cmphex(mgras_test_pixel(qts, 100, 792), ==, 0x00ff00);
    g_assert_cmphex(mgras_test_pixel(qts, 163, 823), ==, 0x00ff00);
    g_assert_cmphex(mgras_test_pixel(qts, 163, 792), ==, 0x00ff00);
    g_assert_cmphex(mgras_test_pixel(qts, 99, 792), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 164, 823), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 100, 791), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 100, 824), ==, 0);
}

/*
 * Without the image's window rectangle (word 3 not 0x11) the model does not
 * know which pixels the window owns, so the clear paints nothing.
 */
static void test_gl_clear_needs_the_window_rectangle(void)
{
    mgras_reset();

    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_clear_color(qts, 1, 1, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);

    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0);
    g_assert_cmphex(mgras_test_format(qts, 300, 300), ==,
                    MGRAS_PIXEL_FORMAT_CI8);
}

/*
 * The server gives the probe's window display ID 14 and XMAP mode 0x1408
 * (run irix-20260927T135825-92207b, seq 3899859-3899880). Bits 9:5 of that
 * mode are zero, so an indexed pixel left under the window resolves
 * through colour map 0, which the server never loads, and shows black. A
 * GL clear stores direct colour, which bypasses the map.
 */
static void test_gl_clear_shows_through_a_direct_colour_display_id(void)
{
    mgras_reset();

    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    /* WID 0's reset mode 0x7a4 selects map 29; make index 0 white there. */
    mgras_write_cmap_entry(qts, 29, 0, 0xffffff00);
    qtest_writeb(qts, REG_XMAP_INDEX, 14 << 2);
    qtest_writel(qts, REG_XMAP_MAIN_MODE, 0x1408);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256, 256), ==, 0xffffff);
    mgras_tag_wid(qts, 256, 256, 259, 256, 14);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256, 256), ==, 0);
    g_assert_cmphex(mgras_test_display_pixel(qts, 260, 256), ==, 0xffffff);

    gl_viewport(qts, 0, 0, 128, 128);
    gl_clear_color(qts, 1, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_display_pixel(qts, 256, 256), ==, 0xff00ff);
    g_assert_cmphex(mgras_test_display_pixel(qts, 260, 256), ==, 0xff00ff);
}
static const GlSpan gl_triangle_moved_spans[] = {
    {26, 87, 87}, {27, 86, 88}, {28, 85, 88}, {29, 84, 88}, {30, 83, 89},
    {31, 83, 89}, {32, 82, 89}, {33, 81, 90}, {34, 80, 90}, {35, 79, 90},
    {36, 78, 90}, {37, 78, 91}, {38, 77, 91}, {39, 76, 91}, {40, 75, 92},
    {41, 74, 92}, {42, 74, 92}, {43, 73, 93}, {44, 72, 93}, {45, 71, 93},
    {46, 70, 94}, {47, 69, 94}, {48, 69, 94}, {49, 68, 94}, {50, 67, 95},
    {51, 66, 95}, {52, 65, 95}, {53, 64, 96}, {54, 64, 96}, {55, 63, 96},
    {56, 62, 97}, {57, 61, 97}, {58, 60, 97}, {59, 59, 97}, {60, 59, 98},
    {61, 58, 98}, {62, 57, 98}, {63, 56, 99}, {64, 55, 99}, {65, 54, 99},
    {66, 54, 100}, {67, 53, 100}, {68, 52, 100}, {69, 51, 101}, {70, 50, 101},
    {71, 50, 101}, {72, 49, 101}, {73, 48, 102}, {74, 47, 102}, {75, 46, 102},
    {76, 45, 103}, {77, 45, 103}, {78, 44, 103}, {79, 43, 104}, {80, 42, 104},
    {81, 41, 104}, {82, 40, 104}, {83, 40, 105}, {84, 39, 105}, {85, 38, 105},
    {86, 37, 106}, {87, 36, 106}, {88, 35, 106}, {89, 35, 107}, {90, 34, 107},
    {91, 33, 107}, {92, 32, 108}, {93, 31, 108}, {94, 30, 108}, {95, 30, 108},
    {96, 29, 109}, {97, 28, 109}, {98, 27, 109}, {99, 26, 110}, {100, 26, 110},
    {101, 25, 110}, {102, 24, 111}, {103, 23, 111}, {104, 22, 105},
    {105, 21, 93}, {106, 21, 81}, {107, 20, 69}, {108, 19, 57}, {109, 18, 45},
    {110, 17, 33}, {111, 16, 21},
};

static void gl_draw_probe_triangle(QTestState *qts, const float v[3][2])
{
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(qts, 0, 0, 128, 128);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_SHADE_MODEL_HDR);
    gl_word(qts, GL_FLAT);
    gl_color3f(qts, 1, 1, 0);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        gl_vertex2f(qts, v[i][0], v[i][1]);
    }
    gl_word(qts, GL_END_TRIANGLES);
    gl_word(qts, GL_FLUSH);
}

/*
 * The probe's triangle case: window vertices (16, 16), (112, 24) and
 * (56, 111) as normalized device coordinates. A pixel is drawn exactly
 * when its centre lies inside (OpenGL 1.2.1, 3.5.1); no centre lies on an
 * edge, so the edge rule cannot change the census.
 */
static void test_gl_flat_triangle_matches_the_reference(void)
{
    static const float v[3][2] = {
        { -0.75f, -0.75f }, { 0.75f, -0.625f }, { -0.125f, 0.734375f },
    };
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    gl_draw_probe_triangle(qts, v);
    rgb = mgras_screendump(qts);
    gl_check_spans(rgb, gl_triangle_spans, ARRAY_SIZE(gl_triangle_spans),
                   0xffff00, 0x0000ff);
}

/* Moving the third vertex to (88, 103) changes exactly the census. */
static void test_gl_flat_triangle_moved_vertex(void)
{
    static const float v[3][2] = {
        { -0.75f, -0.75f }, { 0.75f, -0.625f }, { 0.375f, 0.609375f },
    };
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    gl_draw_probe_triangle(qts, v);
    rgb = mgras_screendump(qts);
    gl_check_spans(rgb, gl_triangle_moved_spans,
                   ARRAY_SIZE(gl_triangle_moved_spans), 0xffff00, 0x0000ff);
}

/*
 * Culling is off by default (OpenGL 1.2.1, 3.5.1), so the reversed
 * winding draws the same pixels.
 */
static void test_gl_flat_triangle_reversed_winding(void)
{
    static const float v[3][2] = {
        { -0.75f, -0.75f }, { -0.125f, 0.734375f }, { 0.75f, -0.625f },
    };
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    gl_draw_probe_triangle(qts, v);
    rgb = mgras_screendump(qts);
    gl_check_spans(rgb, gl_triangle_spans, ARRAY_SIZE(gl_triangle_spans),
                   0xffff00, 0x0000ff);
}

/*
 * A flat-shaded independent triangle takes the colour current at its
 * third vertex (OpenGL 1.2.1, 2.13.7 and Table 2.9).
 */
static void test_gl_flat_triangle_takes_the_third_vertex_colour(void)
{
    mgras_reset();

    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_word(qts, GL_SHADE_MODEL_HDR);
    gl_word(qts, GL_FLAT);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex2f(qts, -1, -1);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex2f(qts, 1, -1);
    gl_color3f(qts, 0, 0, 1);
    gl_vertex2f(qts, -1, 1);
    gl_word(qts, GL_END_TRIANGLES);

    /* Window pixel (10, 10) from the bottom is screen (266, 373). */
    g_assert_cmphex(mgras_test_pixel(qts, 266, 373), ==, 0x0000ff);
    g_assert_cmphex(mgras_test_format(qts, 266, 373), ==,
                    MGRAS_PIXEL_FORMAT_RGB);
    /* Above the diagonal nothing is drawn. */
    g_assert_cmphex(mgras_test_pixel(qts, 373, 266), ==, 0);
}

static void gl_vertex4f(QTestState *qts, float x, float y, float z, float w)
{
    gl_word(qts, 0x00000010);
    gl_float(qts, x);
    gl_float(qts, y);
    gl_float(qts, z);
    gl_float(qts, w);
}

/*
 * Draw one white triangle given in window coordinates of a 128x128
 * viewport, where NDC is x / 64 - 1, into a freshly reset probe window.
 */
static void gl_draw_window_triangle(QTestState *qts, const double v[3][2])
{
    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        gl_vertex2f(qts, v[i][0] / 64 - 1, v[i][1] / 64 - 1);
    }
    gl_word(qts, GL_END_TRIANGLES);
}

/*
 * The first vertex sits at x 10.5 + 3/1024, a quarter of a subpixel off
 * the 1/256 grid. Rounded to the nearest subpixel it moves to 10.5 +
 * 1/256, and the edge to (10.5 - 20/256, 40) then passes right of the
 * centres of window pixels (10, 0) and (10, 1), which stay undrawn.
 * Unsnapped, the edge passes left of (10, 1)'s centre and draws it;
 * truncated to 10.5 it draws both. No centre lies on an edge after
 * rounding, so the edge rule plays no part.
 */
static void test_gl_triangle_snaps_to_the_nearest_subpixel(void)
{
    static const double v[3][2] = {
        { 10.5 + 3.0 / 1024, 0 }, { 10.5 - 20.0 / 256, 40 }, { 30, 20 },
    };

    gl_draw_window_triangle(qts, v);
    g_assert_cmphex(gl_window_pixel(qts, 10, 0), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 10, 1), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 11, 1), ==, 0xffffff);
}

/*
 * Two pairs of triangles share an edge through pixel centres: a diagonal
 * from (0, 0) to (16, 16) and a horizontal edge at y 8.5. OpenGL 1.2.1,
 * 3.5.1 requires each such centre to be drawn by exactly one of the pair.
 * The model gives it to the triangle for which the edge is a left edge
 * (the one below and right of the diagonal) or a top edge (the one below
 * the horizontal).
 */
static unsigned int gl_count_drawn(QTestState *qts, const double v[3][2],
                                   bool diagonal)
{
    unsigned int drawn = 0;

    gl_draw_window_triangle(qts, v);
    for (int i = 0; i < 16; i++) {
        uint32_t pixel = diagonal ? gl_window_pixel(qts, i, i) :
                                    gl_window_pixel(qts, i, 8);

        g_assert_true(pixel == 0 || pixel == 0xffffff);
        drawn += pixel != 0;
    }
    return drawn;
}

static void test_gl_triangle_shared_edge_centres_drawn_once(void)
{
    static const double below_right[3][2] = {
        { 0, 0 }, { 16, 0 }, { 16, 16 },
    };
    static const double above_left[3][2] = {
        { 0, 0 }, { 16, 16 }, { 0, 16 },
    };
    static const double upper[3][2] = {
        { 0, 8.5 }, { 16, 8.5 }, { 8, 16 },
    };
    static const double lower[3][2] = {
        { 16, 8.5 }, { 0, 8.5 }, { 8, 1 },
    };

    g_assert_cmpuint(gl_count_drawn(qts, below_right, true), ==, 16);
    g_assert_cmpuint(gl_count_drawn(qts, above_left, true), ==, 0);
    g_assert_cmpuint(gl_count_drawn(qts, lower, false), ==, 16);
    g_assert_cmpuint(gl_count_drawn(qts, upper, false), ==, 0);
}

/*
 * A triangle reaching NDC -5 and 5 is clipped to the view volume's side
 * planes (OpenGL 1.2.1, 2.11): it covers the whole 64x64 viewport at
 * (32, 32) and draws nothing outside it, though its interior extends
 * there.
 */
static void test_gl_triangle_bounded_by_the_viewport(void)
{
    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 32, 32, 64, 64);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, -5, -5);
    gl_vertex2f(qts, 5, -5);
    gl_vertex2f(qts, 0, 5);
    gl_word(qts, GL_END_TRIANGLES);

    g_assert_cmphex(gl_window_pixel(qts, 32, 32), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 95, 32), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 32, 95), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 95, 95), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 31, 64), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 96, 64), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 64, 31), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 64, 96), ==, 0);
}

/*
 * A triangle with a vertex at w < 0 is clipped where it leaves the view
 * volume, before the divide (OpenGL 1.2.1, 2.11). With (-1/2, -1/2, 0, 1),
 * (1/2, -1/2, 0, 1) and (0, 0, -3, -s) scaled by s, the near plane cuts
 * the two edges to the third vertex a fifth of the way along, at
 * normalized (-2/3, -2/3) and (2/3, -2/3), leaving a trapezoid from
 * window row 21.3 to 32 in the 128-pixel viewport. Row 26 draws, and row
 * 40, inside the triangle the three vertices would make divided by their
 * w, does not. With every vertex negated (s = -1) the triangle lies
 * wholly outside the volume, since no point there has w < 0, and draws
 * nothing, though it divides to the same window points.
 */
static void gl_draw_w_triangle(QTestState *qts, float s)
{
    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex4f(qts, -0.5f * s, -0.5f * s, 0, s);
    gl_vertex4f(qts, 0.5f * s, -0.5f * s, 0, s);
    gl_vertex4f(qts, 0, 0, -3 * s, -s);
    gl_word(qts, GL_END_TRIANGLES);
}

static void test_gl_triangle_behind_the_eye_is_clipped(void)
{
    gl_draw_w_triangle(qts, 1);
    g_assert_cmphex(gl_window_pixel(qts, 64, 26), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 64, 20), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 64, 40), ==, 0);

    gl_draw_w_triangle(qts, -1);
    g_assert_cmphex(gl_window_pixel(qts, 64, 26), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 64, 40), ==, 0);
}
#define GL_SMOOTH             0x1d01
#define GL_DEPTH_GREATER      4
#define GL_DEPTH_ONE          0xffffff
#define GL_DEPTH_SHA256 \
    "92a05b9a5c7c93ddee9a6c0a7717dff6b9983c968c4229e05bb04732fff2dbfe"
#define GL_SMOOTH_SHA256 \
    "77ae4e7831ad45733d6a4ac20c2ed0bfc030e5f321a55d496d0028638b8b1851"

/*
 * The depth case: a red triangle at depth 0, then a green one on the plane
 * z = x / 2, depth test LESS after clearing depth to 1.0. Where they
 * overlap green is nearer left of window x 64 and red to the right; with
 * no depth test green would cover all of the overlap.
 */
static void test_gl_depth_matches_the_reference(void)
{
    g_autofree uint32_t *rgb = NULL;

    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
    gl_one(qts, GL_CLEAR_DEPTH_HDR, GL_DEPTH_ONE);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex3f(qts, -0.78125f, -0.71875f, 0);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex3f(qts, 0.71875f, -0.59375f, 0);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex3f(qts, -0.09375f, 0.78125f, 0);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex3f(qts, -0.90625f, 0.53125f, -0.453125f);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex3f(qts, 0.84375f, 0.40625f, 0.421875f);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex3f(qts, 0.0625f, -0.921875f, 0.03125f);
    gl_word(qts, GL_END_TRIANGLES);
    gl_word(qts, GL_FLUSH);

    rgb = mgras_screendump(qts);
    g_assert_cmphex(gl_dump_pixel(rgb, 40, 60), ==, 0x00ff00);
    g_assert_cmphex(gl_dump_pixel(rgb, 90, 50), ==, 0xff0000);
    gl_check_window_sha256(rgb, GL_DEPTH_SHA256);
}

/* A whole-window triangle at normalized depth -1 + j / 2^24. */
static void gl_depth_layer(QTestState *qts, int j, float r, float g)
{
    float z = -1.0f + (float)j / 16777216.0f;

    gl_color3f(qts, r, g, 0);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, z);
    gl_vertex3f(qts, 3, -1, z);
    gl_vertex3f(qts, -1, 3, z);
    gl_word(qts, GL_END_TRIANGLES);
}

static uint32_t gl_depth_pair(QTestState *qts, int first, int second)
{
    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
    gl_one(qts, GL_CLEAR_DEPTH_HDR, GL_DEPTH_ONE);
    gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
    gl_depth_layer(qts, first, 1, 0);
    gl_depth_layer(qts, second, 0, 1);
    return gl_window_pixel(qts, 10, 10);
}

/*
 * Approximated: the depth plane holds 24 bits, the RE4's published Z
 * width (Octane Technical Report, 4.2.3), and a fragment's window depth
 * is rounded to the nearest of them; how the board rounds is not
 * published. At z = -1 + j / 2^24 the stored value is j / 2 - j / 2^25:
 * j = 4 rounds to 2 (truncation gives 1), j = 3 to 1, j = 2 to 1
 * (truncation gives 0, and unquantized depth keeps it below j = 3's).
 * So the second layer of (4, 3) passes LESS only under rounding, and the
 * second of (3, 2) fails only under rounding to 24 bits.
 */
static void test_gl_depth_rounds_to_24_bits(void)
{
    g_assert_cmphex(gl_depth_pair(qts, 4, 3), ==, 0x00ff00);
    g_assert_cmphex(gl_depth_pair(qts, 3, 2), ==, 0xff0000);
}

/*
 * With the depth test disabled the depth buffer is not updated (OpenGL
 * 1.2.1, 4.1.5), so a far triangle drawn after enabling it still passes
 * against the cleared 1.0. With the test on but the mask off, the same
 * holds (4.2.2).
 */
static void test_gl_depth_writes_only_when_tested_and_masked_in(void)
{
    for (int masked = 0; masked < 2; masked++) {
        gl_scene_start(qts, GL_FLAT);
        gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
        gl_one(qts, GL_CLEAR_DEPTH_HDR, GL_DEPTH_ONE);
        gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
        if (masked) {
            gl_one(qts, GL_DEPTH_TEST_HDR, 1);
            gl_one(qts, GL_DEPTH_MASK_HDR, 0);
        }
        gl_depth_layer(qts, 0, 1, 0);
        gl_one(qts, GL_DEPTH_TEST_HDR, 1);
        gl_one(qts, GL_DEPTH_MASK_HDR, 1);
        gl_depth_layer(qts, 1 << 23, 0, 1);
        g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, 0x00ff00);
    }
}

/*
 * glDepthFunc sends func - GL_NEVER, and each comparison follows 4.1.5:
 * NEVER draws nothing, ALWAYS draws over a nearer surface, GREATER draws
 * only a farther one.
 */
static void test_gl_depth_func_follows_the_enum(void)
{
    static const struct {
        uint32_t func;
        int j;
        uint32_t expected;
    } cases[] = {
        { GL_DEPTH_NEVER, 0, 0xff0000 },
        { GL_DEPTH_ALWAYS, 1 << 24, 0x00ff00 },
        { GL_DEPTH_GREATER, 1 << 22, 0xff0000 },
        { GL_DEPTH_GREATER, 1 << 24, 0x00ff00 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        gl_scene_start(qts, GL_FLAT);
        gl_one(qts, GL_DEPTH_TEST_HDR, 1);
        gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_ALWAYS);
        gl_depth_layer(qts, 1 << 23, 1, 0);
        gl_one(qts, GL_DEPTH_FUNC_HDR, cases[i].func);
        gl_depth_layer(qts, cases[i].j, 0, 1);
        g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, cases[i].expected);
    }
}

/*
 * glClear's depth token fills the window's depth with the glClearDepth
 * value: cleared to 0, nothing passes LESS; cleared to 2^24 - 1, the same
 * triangle draws. Pixels outside the window keep their depth.
 */
static void test_gl_clear_depth_sets_the_plane(void)
{
    for (int far = 0; far < 2; far++) {
        gl_scene_start(qts, GL_FLAT);
        gl_one(qts, GL_DEPTH_TEST_HDR, 1);
        gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
        gl_one(qts, GL_CLEAR_DEPTH_HDR, far ? GL_DEPTH_ONE : 0);
        gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
        gl_depth_layer(qts, 1 << 23, 0, 1);
        g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==,
                        far ? 0x00ff00 : 0);
    }
}

/*
 * The part of a triangle beyond the far or near plane is clipped away
 * (OpenGL 1.2.1, 2.11). Window depth here is 0.25 + 1.5 * y / 128, so
 * rows from 64 up are beyond the far plane, depth test or not.
 */
static void test_gl_triangle_clipped_at_the_far_plane(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, -1, -1, -0.5f);
    gl_vertex3f(qts, 1, -1, -0.5f);
    gl_vertex3f(qts, -1, 1, 2.5f);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 2, 63), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 2, 64), ==, 0);
}

/*
 * The smooth case: red, green and blue vertices at window (0.4375,
 * 0.4375), (127.9375, 0.4375) and (64.1875, 127.9375). Every channel of
 * every pixel is at least 1/32 of a step from a rounding tie, so any
 * sound interpolation gives these 8,192 colours.
 */
static void test_gl_smooth_matches_the_reference(void)
{
    g_autofree uint32_t *rgb = NULL;

    gl_scene_start(qts, GL_SMOOTH);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex3f(qts, -0.9931640625f, -0.9931640625f, 0);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex3f(qts, 0.9990234375f, -0.9931640625f, 0);
    gl_color3f(qts, 0, 0, 1);
    gl_vertex3f(qts, 0.0029296875f, 0.9990234375f, 0);
    gl_word(qts, GL_END_TRIANGLES);
    gl_word(qts, GL_FLUSH);

    rgb = mgras_screendump(qts);
    /* Worked by hand: blue 126.125, green 63.0625, red 65.8125. */
    g_assert_cmphex(gl_dump_pixel(rgb, 63, 63), ==, 0x423f7e);
    g_assert_cmphex(gl_dump_pixel(rgb, 0, 0), ==, 0xff0000);
    gl_check_window_sha256(rgb, GL_SMOOTH_SHA256);
}

/*
 * Approximated: an interpolated colour is stored at eight bits a channel,
 * rounded to nearest (OpenGL 1.2.1, 2.13.9); the RE4 iterates twelve bits
 * and how the board reduces them is not published. Red runs from 0 at
 * window x 0 to 1 at x 128 along the bottom row, so pixel 0's centre has
 * 255 * 0.5 / 128 = 0.996 (1, truncation 0) and pixel 1's 2.988 (3,
 * truncation 2).
 */
static void test_gl_smooth_rounds_to_the_nearest_step(void)
{
    gl_scene_start(qts, GL_SMOOTH);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 0, 0, 1);
    gl_vertex3f(qts, -1, -1, 0);
    gl_color3f(qts, 1, 0, 1);
    gl_vertex3f(qts, 1, -1, 0);
    gl_color3f(qts, 0, 0, 1);
    gl_vertex3f(qts, -1, 1, 0);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 0, 0) >> 16, ==, 1);
    g_assert_cmphex(gl_window_pixel(qts, 1, 0) >> 16, ==, 3);
}

/*
 * Colour is interpolated with the perspective correction of OpenGL 1.2.1,
 * 3.5.1 (equation 3.4). The third vertex (-2, 2, 0, 2) projects to window
 * (0, 128) with red 1 and the others are black at w 1. At window pixel
 * (0, 31) the screen-space weight of the third vertex is 31.5 / 128, so
 * linear interpolation gives red 62.75 (63); corrected, the weight is
 * (b / 2) / (1 - b / 2) = 0.1403, red 35.78 (36).
 */
static void test_gl_smooth_is_perspective_correct(void)
{
    gl_scene_start(qts, GL_SMOOTH);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_color3f(qts, 0, 0, 0);
    gl_vertex4f(qts, -1, -1, 0, 1);
    gl_color3f(qts, 0, 0, 0);
    gl_vertex4f(qts, 1, -1, 0, 1);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex4f(qts, -2, 2, 0, 2);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 0, 31), ==, 0x240000);
}

/*
 * A deep double-buffered visual's client sends draw-buffer bit 2 for its
 * back buffer, and a stereo one bit 3 as well (mgras-dual-re.md, "What
 * IRIX sends a two-engine board"). On a two-RE board each bit is a colour
 * buffer a clear lands in and a read-back sees; bit 2 does not write the
 * front or back bank, and bit 0 still does. Scanout is not asserted: which
 * slot a swap displays is unknown.
 */
static void test_gl_deep_bit2_draws_ssi(void)
{
    static const struct {
        uint32_t word;
        uint32_t front, back, slot0, slot1;
    } cases[] = {
        { 4, 0, 0, 0xff00ff, 0 },
        { 8, 0, 0, 0, 0xff00ff },
        { 0xc, 0, 0, 0xff00ff, 0xff00ff },
        { 5, 0xff00ff, 0, 0xff00ff, 0 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        QTestState *s = qtest_init(MACHINE_ARGS MGRAS_TWO_RE_ARGS);

        gl_deep_scene(s);
        gl_draw_buffer(s, cases[i].word);
        gl_clear_color(s, 1, 0, 1, 1);
        gl_word(s, GL_CLEAR_COLOR_BUFFER);
        g_assert_cmphex(mgras_test_pixel(s, 300, 300), ==, cases[i].front);
        g_assert_cmphex(mgras_qom_get_uint(s, "x-test-back-pixel"), ==,
                        cases[i].back);
        g_assert_cmphex(mgras_test_slot_pixel(s, 0, 300, 300), ==,
                        cases[i].slot0);
        g_assert_cmphex(mgras_test_slot_pixel(s, 1, 300, 300), ==,
                        cases[i].slot1);
        qtest_quit(s);
    }
}

/* One RE has no slot 2: the bit stays ignored and nothing is drawn. */
static void test_gl_deep_bit2_draws_si(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_draw_buffer(qts, 4);
    gl_clear_color(qts, 1, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(qts, 300, 300), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-back-pixel"), ==, 0);
    g_assert_cmphex(mgras_test_slot_pixel(qts, 0, 300, 300), ==, 0);
}

/*
 * A switch can land between the vertices of a triangle: the client does
 * not know it happened. a's first two vertices wait in a's state while b
 * draws a whole triangle of its own, and a's third completes a's.
 */
static void test_gl_triangle_spans_a_context_switch(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_color3f(qts, 1, 1, 0);
    gl_window_projection(qts);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, 16, 16);
    gl_vertex2f(qts, 112, 24);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_color3f(qts, 1, 0, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, 0.5f, 0.5f);
    gl_vertex2f(qts, 0.9f, 0.5f);
    gl_vertex2f(qts, 0.5f, 0.9f);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 100, 100), ==, 0xff00ff);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_vertex2f(qts, 56, 111);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 61, 50), ==, 0xffff00);
}

/*
 * glColor3ub, glColor4ub and glVertex2i (mgras-gl-commands.md, vertex
 * attributes): 0x34800203 and one word, 0x14800204 and one word,
 * 0x58000008 and two ints.
 */
#define GL_COLOR3UB_HDR       0x34800203
#define GL_COLOR4UB_HDR       0x14800204
#define GL_VERTEX2I_HDR       0x58000008

/* A flat triangle over window pixel (10, 10) in the current colour. */
static void gl_corner_triangle(QTestState *qts)
{
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, -1, -1);
    gl_vertex2f(qts, 1, -1);
    gl_vertex2f(qts, -1, 1);
    gl_word(qts, GL_END_TRIANGLES);
}

/*
 * colorbars (run irix-20260928T004550-1040f3, from seq 26371573) sends its
 * bars' colours as glColor3ub words 0x87878700, 0x87870000, 0x00878700,
 * 0x00870000, 0x87008700 and 0x87000000: white, yellow, cyan, green,
 * magenta and red at 0x87, so the three bytes arrive red first from bit
 * 31. A byte converts to byte / 255 (OpenGL 1.2.1, table 2.6), so 0x80
 * stays 0x80 in an eight-bit channel. The fourth byte of glColor4ub is
 * alpha, which follows from that order; noof's constant 0x00000060 is
 * consistent with it.
 */
static void test_gl_color_bytes_unpack_red_first(void)
{
    mgras_reset();

    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_FLAT);
    gl_one(qts, GL_COLOR3UB_HDR, 0xff800100);
    gl_corner_triangle(qts);
    g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, 0xff8001);

    gl_one(qts, GL_COLOR4UB_HDR, 0x2040c0ff);
    gl_corner_triangle(qts);
    g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, 0x2040c0);

    /* The one payload word was consumed: the next word is a header. */
    gl_one(qts, GL_COLOR3UB_HDR, 0x00ff0000);
    gl_word(qts, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    gl_corner_triangle(qts);
    g_assert_cmphex(gl_window_pixel(qts, 10, 10), ==, 0x00ff00);
}

/*
 * The probe's triangle, sent as glVertex2i window units through
 * glOrtho(0, 128, 0, 128, -1, 1), draws the triangle case's pixels.
 */
static void test_gl_vertex2i_matches_the_reference(void)
{
    static const int32_t v[3][2] = { { 16, 16 }, { 112, 24 }, { 56, 111 } };
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(qts, 0, 0, 128, 128);
    gl_window_projection(qts);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_FLAT);
    gl_color3f(qts, 1, 1, 0);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    for (int i = 0; i < 3; i++) {
        gl_word(qts, GL_VERTEX2I_HDR);
        gl_word(qts, v[i][0]);
        gl_word(qts, v[i][1]);
    }
    gl_word(qts, GL_END_TRIANGLES);
    rgb = mgras_screendump(qts);
    gl_check_spans(rgb, gl_triangle_spans, ARRAY_SIZE(gl_triangle_spans),
                   0xffff00, 0x0000ff);
}

/*
 * A half turn about (2, 2, 0), made unit length, swaps x and y and
 * negates z: a mark at (40, 10) lands on (10, 40).
 */
static void test_gl_rotate_about_a_general_axis(void)
{
    gl_transform_start(qts);
    gl_rotate(qts, 180, 2, 2, 0);
    gl_mark(qts, 40, 10);
    g_assert_cmphex(gl_window_pixel(qts, 10, 40), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 40, 10), ==, 0);
}

/*
 * The probe's primitive, transform and format cases (Origami branch
 * mgras-probes-primitives, gl_min_probe.c's prim_cases), sent as the
 * IRIX 6.5.30 client frames them: glViewport, glDisable(GL_DITHER)
 * (0x6704 + 0), glShadeModel(GL_FLAT), a blue clear, glOrtho(0, 128, 0,
 * 128, -1, 1) on the projection, then the case's matrix commands and
 * primitives in order, then glFlush. Each window must match the image
 * gl_min_expected.py computes from the OpenGL 1.1 rules, which Mesa
 * llvmpipe matches exactly; the SHA-256 is of its 128x128 RGB bytes, top
 * row first, never of this model's output.
 */
enum {
    GL_PRIM_END, GL_PRIM_DRAW, GL_PRIM_PUSH, GL_PRIM_POP, GL_PRIM_TRANSLATE,
    GL_PRIM_ROTATE, GL_PRIM_SCALE,
};
enum {
    GL_PRIM_FLOAT, GL_PRIM_COLOR3UB, GL_PRIM_COLOR4UB, GL_PRIM_VERTEX2I,
};

typedef struct GlPrimOp {
    int op;
    float arg[4];
} GlPrimOp;

typedef struct GlPrimVertex {
    float x, y;
    uint8_t rgb[3];
} GlPrimVertex;

typedef struct GlPrimCase {
    const char *name;
    const char *sha256;
    uint32_t begin;
    int format;
    GlPrimOp op[6];
    int count;
    GlPrimVertex v[8];
} GlPrimCase;
/* glBegin's token; glEnd's is 0x0a00 above it (mgras-gl-commands.md). */
#define GL_BEGIN_POINTS       0x00001600
#define GL_BEGIN_TRI_FAN      0x00001c00
#define GL_BEGIN_QUAD_STRIP   0x00001e00

static const GlPrimCase gl_prim_cases[] = {
    { "points",
      "482d539f231a1f18291197f2e286d821c3f1a50de839fd6ca2d06492bed83d1c",
      GL_BEGIN_POINTS, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 6,
      { { 10.25f, 10.75f, { 255, 0, 0 } }, { 20.5f, 100.5f, { 0, 255, 0 } },
        { 64.75f, 64.25f, { 255, 255, 0 } },
        { 100.125f, 30.875f, { 255, 255, 255 } },
        { 120.5f, 120.5f, { 255, 0, 255 } },
        { 5.5f, 60.25f, { 0, 255, 255 } } } },
    { "lines",
      "0d557e4410ce6f4690519f5133090dde422327cc709289730388558035cf0ef0",
      GL_BEGIN_LINES, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 8,
      { { 10.5f, 20.5f, { 255, 0, 0 } }, { 60.5f, 20.5f, { 255, 255, 0 } },
        { 30.25f, 40.875f, { 255, 0, 0 } },
        { 30.25f, 110.125f, { 0, 255, 0 } },
        { 15.25f, 30.125f, { 255, 0, 0 } },
        { 110.625f, 70.28125f, { 255, 255, 255 } },
        { 70.375f, 120.25f, { 255, 0, 0 } },
        { 90.53125f, 35.59375f, { 0, 255, 255 } } } },
    { "line-strip",
      "000abf8ff15b541f2dc2cefc10606cadad946b5ae9d47d15f8334271a602d7e7",
      GL_BEGIN_LINE_STRIP, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 5,
      { { 8.15625f, 9.40625f, { 255, 0, 0 } },
        { 62.15625f, 36.40625f, { 255, 255, 0 } },
        { 102.15625f, 6.40625f, { 0, 255, 0 } },
        { 117.15625f, 81.40625f, { 255, 255, 255 } },
        { 37.15625f, 111.40625f, { 255, 0, 255 } } } },
    { "line-loop",
      "c5aa30dcc66e8c98bdbf708e77ca5767ff3fc302f670db1d1034fd2d315c1cf6",
      GL_BEGIN_LINE_LOOP, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 4,
      { { 20.15625f, 20.75f, { 255, 0, 0 } },
        { 100.15625f, 30.75f, { 255, 255, 0 } },
        { 90.15625f, 100.75f, { 0, 255, 0 } },
        { 30.15625f, 90.75f, { 255, 255, 255 } } } },
    { "triangle-strip",
      "ba7ee78db90a8f00ea0a6df54de7578922e0021e3b05c78afe84388320e27cae",
      GL_BEGIN_TRI_STRIP, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 6,
      { { 10.09375f, 20.34375f, { 0, 0, 0 } },
        { 20.09375f, 90.34375f, { 0, 0, 0 } },
        { 45.09375f, 15.34375f, { 255, 0, 0 } },
        { 55.09375f, 100.34375f, { 255, 255, 0 } },
        { 80.09375f, 25.34375f, { 0, 255, 0 } },
        { 100.09375f, 95.34375f, { 255, 255, 255 } } } },
    { "triangle-fan",
      "ae2e1ba0f39fa9efb41d2b459b71a96e55e46beb218834471d3fa0011f97cc20",
      GL_BEGIN_TRI_FAN, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 5,
      { { 64.09375f, 60.53125f, { 0, 0, 0 } },
        { 109.09375f, 45.53125f, { 0, 0, 0 } },
        { 100.09375f, 108.53125f, { 255, 0, 0 } },
        { 40.09375f, 116.53125f, { 255, 255, 0 } },
        { 16.09375f, 72.53125f, { 0, 255, 0 } } } },
    { "quads",
      "6d371b52adc58049677e317f13f4e40e2522998eec59a45f073ef914e304e11d",
      GL_BEGIN_QUADS, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 8,
      { { 10.03125f, 10.53125f, { 255, 0, 0 } },
        { 60.03125f, 15.53125f, { 255, 0, 0 } },
        { 55.03125f, 60.53125f, { 255, 0, 0 } },
        { 15.03125f, 55.53125f, { 255, 255, 0 } },
        { 70.03125f, 70.53125f, { 255, 0, 0 } },
        { 120.03125f, 75.53125f, { 255, 0, 0 } },
        { 115.03125f, 120.53125f, { 255, 0, 0 } },
        { 65.03125f, 110.53125f, { 0, 255, 0 } } } },
    { "quad-strip",
      "6a48120877e2d60a694003b7c6ff9a9f96e4c65bd12e098a8ee9200edb7fb64b",
      GL_BEGIN_QUAD_STRIP, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 6,
      { { 10.09375f, 20.15625f, { 255, 0, 0 } },
        { 10.09375f, 100.15625f, { 255, 0, 0 } },
        { 60.09375f, 10.15625f, { 255, 0, 0 } },
        { 55.09375f, 110.15625f, { 255, 255, 0 } },
        { 120.09375f, 30.15625f, { 255, 0, 0 } },
        { 110.09375f, 90.15625f, { 0, 255, 0 } } } },
    { "polygon",
      "fee3202432cb5881ba63abdd142b7007e54df5b5de6480b9953177da61a69731",
      GL_BEGIN_POLYGON, GL_PRIM_FLOAT, { { GL_PRIM_DRAW } }, 6,
      { { 64.125f, 8.40625f, { 255, 255, 0 } },
        { 112.125f, 36.40625f, { 255, 0, 0 } },
        { 108.125f, 92.40625f, { 0, 255, 0 } },
        { 60.125f, 120.40625f, { 255, 255, 255 } },
        { 16.125f, 92.40625f, { 255, 0, 255 } },
        { 20.125f, 36.40625f, { 0, 255, 255 } } } },
    { "push-pop",
      "e5b12c008b2c481baf89277d4c29f7ac858fa1c4baee90be25f6ad8b78dc63e2",
      GL_BEGIN_TRIANGLES, GL_PRIM_FLOAT,
      { { GL_PRIM_PUSH }, { GL_PRIM_TRANSLATE, { 60, 50, 0 } },
        { GL_PRIM_DRAW }, { GL_PRIM_POP }, { GL_PRIM_DRAW } }, 3,
      { { 10.03125f, 10.09375f, { 255, 255, 0 } },
        { 50.03125f, 20.09375f, { 255, 255, 0 } },
        { 20.03125f, 55.09375f, { 255, 255, 0 } } } },
    { "rotate",
      "8b3d9083f192fb194c6cfe36d1a550caa2cc9829181d68149f0e9af144cdb704",
      GL_BEGIN_TRIANGLES, GL_PRIM_FLOAT,
      { { GL_PRIM_TRANSLATE, { 64.5625f, 60.5f, 0 } },
        { GL_PRIM_ROTATE, { 30, 0, 0, 2 } }, { GL_PRIM_DRAW } }, 3,
      { { -20, -10, { 255, 255, 0 } }, { 25, -15, { 255, 255, 0 } },
        { 0, 30, { 255, 255, 0 } } } },
    { "scale",
      "91af7b3f7722ad9aaf7e1987b8fb96d9dba646f95144fc90c9ac2c7bbf9ace8a",
      GL_BEGIN_TRIANGLES, GL_PRIM_FLOAT,
      { { GL_PRIM_TRANSLATE, { 10, 15.625f, 0 } },
        { GL_PRIM_SCALE, { 1.5f, 0.75f, 1 } }, { GL_PRIM_DRAW } }, 3,
      { { 0, 0, { 255, 255, 0 } }, { 70, 10, { 255, 255, 0 } },
        { 20, 120, { 255, 255, 0 } } } },
    { "color3ub",
      "d09bf68e85017e80d767dd08a9495b9236a1d0e3cd023205ff1f26745b51e31b",
      GL_BEGIN_TRIANGLES, GL_PRIM_COLOR3UB, { { GL_PRIM_DRAW } }, 6,
      { { 10, 10.3125f, { 0, 0, 0 } }, { 60, 20.3125f, { 0, 0, 0 } },
        { 20, 60.3125f, { 255, 128, 1 } }, { 70, 60.3125f, { 0, 0, 0 } },
        { 120, 70.3125f, { 0, 0, 0 } }, { 80, 120.3125f, { 64, 192, 32 } } } },
    { "color4ub",
      "c04cac041e808b0c99e96cf4e3611512b18bdaac742cb6ad0934fda0058e5929",
      GL_BEGIN_TRIANGLES, GL_PRIM_COLOR4UB, { { GL_PRIM_DRAW } }, 6,
      { { 10, 10.3125f, { 0, 0, 0 } }, { 60, 20.3125f, { 0, 0, 0 } },
        { 20, 60.3125f, { 32, 64, 192 } }, { 70, 60.3125f, { 0, 0, 0 } },
        { 120, 70.3125f, { 0, 0, 0 } },
        { 80, 120.3125f, { 200, 100, 50 } } } },
    { "vertex2i",
      "d18b98ea09eb97c0803ac05d5c8fcb55d329817c833e3d3cd2679a355ecadf0f",
      GL_BEGIN_TRIANGLES, GL_PRIM_VERTEX2I, { { GL_PRIM_DRAW } }, 3,
      { { 30, 40, { 255, 255, 0 } }, { 70, 50, { 255, 255, 0 } },
        { 40, 85, { 255, 255, 0 } } } },
};

static void gl_prim_draw(QTestState *qts, const GlPrimCase *c)
{
    gl_word(qts, c->begin);
    for (int i = 0; i < c->count; i++) {
        const GlPrimVertex *v = &c->v[i];
        uint32_t bytes = (v->rgb[0] << 24) | (v->rgb[1] << 16) |
                         (v->rgb[2] << 8);

        switch (c->format) {
        case GL_PRIM_COLOR3UB:
            gl_one(qts, GL_COLOR3UB_HDR, bytes);
            break;
        case GL_PRIM_COLOR4UB:
            gl_one(qts, GL_COLOR4UB_HDR, bytes | 0xff);
            break;
        default:
            gl_color3f(qts, v->rgb[0] / 255.0f, v->rgb[1] / 255.0f,
                       v->rgb[2] / 255.0f);
            break;
        }
        if (c->format == GL_PRIM_VERTEX2I) {
            gl_word(qts, GL_VERTEX2I_HDR);
            gl_word(qts, (int32_t)v->x);
            gl_word(qts, (int32_t)v->y);
        } else {
            gl_vertex2f(qts, v->x, v->y);
        }
    }
    gl_word(qts, GL_END_OF(c->begin));
}

static void test_gl_prim_matches_the_reference(const void *data)
{
    const GlPrimCase *c = data;
    g_autofree uint32_t *rgb = NULL;

    mgras_reset();
    mgras_enter_gl_window(qts, 256, 640, 128, 128, 0x11);
    qtest_writel(qts, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(qts, 0, 0, 128, 128);
    gl_one(qts, GL_DITHER_HDR, 0);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_FLAT);
    gl_clear_color(qts, 0, 0, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_window_projection(qts);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    for (int i = 0; i < ARRAY_SIZE(c->op) && c->op[i].op != GL_PRIM_END;
         i++) {
        const GlPrimOp *op = &c->op[i];

        switch (op->op) {
        case GL_PRIM_PUSH:
            gl_word(qts, GL_PUSH_MATRIX);
            break;
        case GL_PRIM_POP:
            gl_word(qts, GL_POP_MATRIX);
            break;
        case GL_PRIM_TRANSLATE:
            gl_three(qts, GL_TRANSLATE_HDR, op->arg[0], op->arg[1],
                     op->arg[2]);
            break;
        case GL_PRIM_SCALE:
            gl_three(qts, GL_SCALE_HDR, op->arg[0], op->arg[1], op->arg[2]);
            break;
        case GL_PRIM_ROTATE:
            gl_rotate(qts, op->arg[0], op->arg[1], op->arg[2], op->arg[3]);
            break;
        default:
            gl_prim_draw(qts, c);
            break;
        }
    }
    gl_word(qts, GL_FLUSH);
    rgb = mgras_screendump(qts);
    gl_check_window_sha256(rgb, c->sha256);
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);
}

/*
 * Approximated: a quad, quad-strip quad or polygon is drawn as a fan of
 * triangles from its first vertex; OpenGL 1.2.1 (3.5.1) leaves the split
 * open, and the board's is not known. Smooth shading shows the split: in
 * the square below, with red, green, black and green corners, the fan
 * from the red corner gives centre (40.5, 20.5) 23.5/64 red and 20/64
 * green, 0x5e5000, where the other diagonal would give 3/64 red and 61/64
 * green; the same holds for (20.5, 40.5) by symmetry.
 */
static void test_gl_quad_splits_from_its_first_vertex(void)
{
    gl_transform_start(qts);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_SMOOTH);
    gl_word(qts, GL_BEGIN_QUADS);
    gl_color3f(qts, 1, 0, 0);
    gl_vertex2f(qts, 0, 0);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex2f(qts, 64, 0);
    gl_color3f(qts, 0, 0, 0);
    gl_vertex2f(qts, 64, 64);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex2f(qts, 0, 64);
    gl_word(qts, GL_END_OF(GL_BEGIN_QUADS));
    g_assert_cmphex(gl_window_pixel(qts, 40, 20), ==, 0x5e5000);
    g_assert_cmphex(gl_window_pixel(qts, 20, 40), ==, 0x5e5000);
}

/*
 * A strip, fan or polygon is drawn piece by piece as its vertices arrive,
 * and the vertices the next piece needs wait in the context: a switch to
 * another context between two strip vertices loses nothing.
 */
static void test_gl_strip_spans_a_context_switch(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_window_projection(qts);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_FLAT);
    gl_word(qts, GL_BEGIN_TRI_STRIP);
    gl_color3f(qts, 1, 1, 0);
    gl_vertex2f(qts, 8, 8);
    gl_vertex2f(qts, 8, 100);
    gl_vertex2f(qts, 60, 8);

    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_viewport(qts, 0, 0, 128, 128);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex2f(qts, 0.5f, 0.5f);
    gl_vertex2f(qts, 0.9f, 0.5f);
    gl_vertex2f(qts, 0.5f, 0.9f);
    gl_word(qts, GL_END_TRIANGLES);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_color3f(qts, 1, 0, 1);
    gl_vertex2f(qts, 60, 100);
    gl_word(qts, GL_END_OF(GL_BEGIN_TRI_STRIP));
    g_assert_cmphex(gl_window_pixel(qts, 20, 20), ==, 0xffff00);
    g_assert_cmphex(gl_window_pixel(qts, 50, 90), ==, 0xff00ff);
}

/*
 * The diamond-exit rule (OpenGL 1.2.1, 3.4.1) keeps a segment's starting
 * pixel and leaves out the one it ends in, so a strip's shared vertex is
 * drawn once, by the segment leaving it. A segment from centre (10.5,
 * 20.5) to centre (14.5, 20.5) covers columns 10 to 13, and one back
 * covers 11 to 14.
 */
static void test_gl_line_leaves_out_its_last_pixel(void)
{
    gl_transform_start(qts);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_LINES);
    gl_vertex2f(qts, 10.5f, 20.5f);
    gl_vertex2f(qts, 14.5f, 20.5f);
    gl_vertex2f(qts, 14.5f, 30.5f);
    gl_vertex2f(qts, 10.5f, 30.5f);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
    for (int x = 9; x <= 15; x++) {
        g_assert_cmphex(gl_window_pixel(qts, x, 20), ==,
                        x >= 10 && x <= 13 ? 0xffffff : 0);
        g_assert_cmphex(gl_window_pixel(qts, x, 30), ==,
                        x >= 11 && x <= 14 ? 0xffffff : 0);
    }
}

/*
 * Like a triangle, a segment is clipped to the view volume (2.11): one
 * across a 32-pixel viewport's right edge stops at column 31.
 */
static void test_gl_line_bounded_by_the_viewport(void)
{
    gl_transform_start(qts);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_viewport(qts, 0, 0, 32, 32);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_LINES);
    gl_vertex2f(qts, 8.5f, 8.5f);
    gl_vertex2f(qts, 180.5f, 8.5f);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
    /* Window units under glOrtho(0, 128) in a 32-pixel viewport: / 4. */
    g_assert_cmphex(gl_window_pixel(qts, 2, 2), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 31, 2), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 32, 2), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 40, 2), ==, 0);
}

/*
 * A segment's fragments take depth and smooth colour at t, the centre's
 * projection onto the segment (3.4.1). A red-to-green segment from depth
 * -1 to 1 across a depth-tested white triangle at depth 0 shows in front
 * of it only on its first half, with the colour a quarter of the way at
 * a quarter of its length.
 */
static void test_gl_line_depth_and_colour_follow_t(void)
{
    gl_transform_start(qts);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_one(qts, GL_CLEAR_DEPTH_HDR, GL_DEPTH_ONE);
    gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_vertex3f(qts, 0, 0, 0);
    gl_vertex3f(qts, 127, 0, 0);
    gl_vertex3f(qts, 0, 127, 0);
    gl_word(qts, GL_END_TRIANGLES);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_SMOOTH);
    gl_word(qts, GL_BEGIN_LINES);
    gl_color3f(qts, 1, 0, 0);
    /* glOrtho's near plane is at z = -1 in object units, window z 0. */
    gl_vertex3f(qts, 8.5f, 40.5f, 0.9375f);
    gl_color3f(qts, 0, 1, 0);
    gl_vertex3f(qts, 72.5f, 40.5f, -0.9375f);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
    /* Column 24 is t 0.25: 191.25 red and 63.75 green round to bf40. */
    g_assert_cmphex(gl_window_pixel(qts, 24, 40), ==, 0xbf4000);
    /* Past the middle the segment lies behind the triangle. */
    g_assert_cmphex(gl_window_pixel(qts, 56, 40), ==, 0xffffff);
}
#define GL_Q_VIEWPORT         0xc1d
#define GL_Q_SHADE_MODEL      0xc19
#define GL_Q_CLEAR_COLOR      0xc0a

/* The viewport, shade model and clear colour, as the client sent them. */
static void test_hq_query_answers_viewport_shade_and_clear_colour(void)
{
    static const uint32_t viewport[4] = { 3, 5, 120, 90 };
    static const float clear[4] = { 0.25f, 0.5f, 0.75f, 1 };

    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    g_assert_cmphex(gl_query(qts, GL_Q_SHADE_MODEL, 1), ==, GL_SMOOTH);
    gl_viewport(qts, viewport[0], viewport[1], viewport[2], viewport[3]);
    gl_one(qts, GL_SHADE_MODEL_HDR, GL_FLAT);
    gl_clear_color(qts, clear[0], clear[1], clear[2], clear[3]);
    for (int i = 0; i < 4; i++) {
        g_assert_cmphex(gl_query(qts, GL_Q_VIEWPORT + i, 1), ==, viewport[i]);
        g_assert_cmphex(gl_query(qts, GL_Q_CLEAR_COLOR + i, 1), ==,
                        gl_bits(clear[i]));
    }
    g_assert_cmphex(gl_query(qts, GL_Q_SHADE_MODEL, 1), ==, GL_FLAT);
}

/* Clear the bound window to white, and read screen pixels top-down. */
static void gl_clip_clear(QTestState *qts)
{
    gl_viewport(qts, 0, 0, 1280, 1024);
    gl_clear_color(qts, 1, 1, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
}

/*
 * A window partly covered has two visible rectangles, both enabled in
 * word 3 (0x13): a clear fills both and nothing between them. A window
 * with four fills all four, rectangle 3 from words 5 and 6.
 */
static void test_gl_clip_takes_every_enabled_rectangle(void)
{
    const uint32_t rects[4][2] = {
        { GL_CLIP_SPAN(100, 149), GL_CLIP_SPAN(200, 219) },
        { GL_CLIP_SPAN(200, 219), GL_CLIP_SPAN(200, 249) },
        { GL_CLIP_SPAN(300, 309), GL_CLIP_SPAN(100, 109) },
        { GL_CLIP_SPAN(400, 409), GL_CLIP_SPAN(100, 109) },
    };

    mgras_reset();
    mgras_enter_gl_clip(qts, (200 << 16) | 100, 0x13, rects);
    gl_clip_clear(qts);
    /* Rows 200-219 from the bottom are screen rows 804-823. */
    g_assert_cmphex(mgras_test_pixel(qts, 100, 823), ==, 0xffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 149, 804), ==, 0xffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 219, 774), ==, 0xffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 175, 810), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 150, 810), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 305, 918), ==, 0);

    mgras_reset();
    mgras_enter_gl_clip(qts, (200 << 16) | 100, 0x1f, rects);
    gl_clip_clear(qts);
    g_assert_cmphex(mgras_test_pixel(qts, 100, 823), ==, 0xffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 219, 774), ==, 0xffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 305, 918), ==, 0xffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 409, 914), ==, 0xffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 410, 914), ==, 0);
}

/*
 * A rectangle whose enable bit is clear is not the window's, whatever its
 * words hold: the kernel leaves them as the image last had them.
 */
static void test_gl_clip_ignores_disabled_rectangles(void)
{
    const uint32_t rects[4][2] = {
        { GL_CLIP_SPAN(100, 149), GL_CLIP_SPAN(200, 219) },
        { GL_CLIP_SPAN(200, 219), GL_CLIP_SPAN(200, 249) },
        { GL_CLIP_SPAN(300, 309), GL_CLIP_SPAN(100, 109) },
        { GL_CLIP_SPAN(400, 409), GL_CLIP_SPAN(100, 109) },
    };

    mgras_reset();
    mgras_enter_gl_clip(qts, (200 << 16) | 100, 0x15, rects);
    gl_clip_clear(qts);
    g_assert_cmphex(mgras_test_pixel(qts, 100, 823), ==, 0xffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 305, 918), ==, 0xffffff);
    g_assert_cmphex(mgras_test_pixel(qts, 219, 774), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 405, 918), ==, 0);
}

/*
 * The kernel's null clip, 0x23 with both rectangles the whole screen and
 * the origin at row 1024 (perfly's context 0x84c6 in run
 * irix-20260928T000530-78d0c5), draws nothing; nor does 0x10, a window of
 * five or more rectangles, whose bounds the image does not give.
 */
static void test_gl_clip_null_and_many_draw_nothing(void)
{
    const uint32_t full[4][2] = {
        { GL_CLIP_SPAN(0, 1279), GL_CLIP_SPAN(0, 1023) },
        { GL_CLIP_SPAN(0, 1279), GL_CLIP_SPAN(0, 1023) },
        { GL_CLIP_SPAN(0, 1279), GL_CLIP_SPAN(0, 1023) },
        { GL_CLIP_SPAN(0, 1279), GL_CLIP_SPAN(0, 1023) },
    };

    mgras_reset();
    mgras_enter_gl_clip(qts, 0x04000000, 0x23, full);
    gl_clip_clear(qts);
    g_assert_cmphex(mgras_test_pixel(qts, 640, 512), ==, 0);
    g_assert_cmphex(mgras_test_pixel(qts, 0, 1023), ==, 0);

    mgras_reset();
    mgras_enter_gl_clip(qts, 0, 0x10, full);
    gl_clip_clear(qts);
    g_assert_cmphex(mgras_test_pixel(qts, 640, 512), ==, 0);
}

static void test_hq_query_answers_the_depth_range(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    g_assert_cmphex(gl_query(qts, GL_Q_DEPTH_RANGE, 1), ==, gl_bits(0));
    g_assert_cmphex(gl_query(qts, GL_Q_DEPTH_RANGE + 1, 1), ==, gl_bits(1));
    gl_depth_range(qts, 0.25f, 2);
    g_assert_cmphex(gl_query(qts, GL_Q_DEPTH_RANGE, 1), ==, gl_bits(0.25f));
    g_assert_cmphex(gl_query(qts, GL_Q_DEPTH_RANGE + 1, 1), ==, gl_bits(1));
    gl_depth_range(qts, -1, 0.5f);
    g_assert_cmphex(gl_query(qts, GL_Q_DEPTH_RANGE, 1), ==, gl_bits(0));
    g_assert_cmphex(gl_query(qts, GL_Q_DEPTH_RANGE + 1, 1), ==,
                    gl_bits(0.5f));
    /* The opener of a new context restores the initial range. */
    gl_word(qts, GL_CONTEXT_OPENER);
    g_assert_cmphex(gl_query(qts, GL_Q_DEPTH_RANGE + 1, 1), ==, gl_bits(1));
}

/*
 * GL_MODELVIEW_STACK_DEPTH and GL_PROJECTION_STACK_DEPTH are queried at
 * 0x83 and 0x922, and the client returns ((answer - base) >> 4) + 1 with
 * base 0x925 and 0xb25, the hq3_revB_HQ4.u symbol-table words at file
 * offsets 0x210 and 0x214. perfly's draw process stopped after reading a
 * zero here.
 */
#define GL_Q_MODELVIEW_DEPTH  0x083
#define GL_Q_PROJECTION_DEPTH 0x922

static void test_hq_query_answers_the_stack_depths(void)
{
    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    g_assert_cmphex(gl_query(qts, GL_Q_MODELVIEW_DEPTH, 0), ==, 0x925);
    g_assert_cmphex(gl_query(qts, GL_Q_PROJECTION_DEPTH, 1), ==, 0xb25);
    gl_word(qts, GL_PUSH_MATRIX);
    gl_word(qts, GL_PUSH_MATRIX);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_PROJECTION);
    gl_word(qts, GL_PUSH_MATRIX);
    g_assert_cmpuint(gl_stack_depth(gl_query(qts, GL_Q_MODELVIEW_DEPTH, 0),
                                    0x925), ==, 3);
    g_assert_cmpuint(gl_stack_depth(gl_query(qts, GL_Q_PROJECTION_DEPTH, 1),
                                    0xb25), ==, 2);
    gl_word(qts, GL_POP_MATRIX);
    g_assert_cmphex(gl_query(qts, GL_Q_PROJECTION_DEPTH, 1), ==, 0xb25);
    g_assert_cmphex(gl_query(qts, GL_Q_MODELVIEW_DEPTH, 0), ==, 0x945);
}
#define GL_LIGHT_POSITION_HDR 0x00004f14
#define GL_LIGHT_SPOT_DIR_HDR 0x00005010
#define GL_COLOR_MATERIAL_FRONT_HDR 0x0000bf04
#define GL_GET_MATERIAL_HDR   0x0000ee08
#define GL_Q_LIGHT_MODEL_AMBIENT 0x55e
#define GL_Q_TWO_SIDE         0xc1b
#define GL_Q_COLOR_MATERIAL_PARAMETER 0x56b
#define GL_Q_COLOR_MATERIAL_FACE 0x56c
#define GL_POSITION           0x1203
#define GL_SPOT_DIRECTION     0x1204

static void test_hq_query_answers_lights_and_materials(void)
{
    static const float white[4] = { 1, 1, 1, 1 };
    static const float black[4] = { 0, 0, 0, 1 };
    static const float grey[4] = { 0.8f, 0.8f, 0.8f, 1 };
    static const float none[4] = { 0, 0, 0, 0 };

    mgras_reset();

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_readback(qts, GL_GET_LIGHT_HDR, 0, GL_DIFFUSE, white);
    gl_readback(qts, GL_GET_LIGHT_HDR, 1, GL_DIFFUSE, black);
    gl_readback(qts, GL_GET_MATERIAL_HDR, GL_BACK_FACE, GL_DIFFUSE, grey);

    /* A position goes to eye coordinates, a spot direction without w. */
    gl_three(qts, GL_TRANSLATE_HDR, 3, 4, 5);
    gl_light4(qts, GL_LIGHT_POSITION_HDR, 2, 1, 2, 3, 1);
    gl_word(qts, GL_LIGHT_SPOT_DIR_HDR);
    gl_word(qts, 2);
    gl_float(qts, 0);
    gl_float(qts, 1);
    gl_float(qts, 0);
    gl_readback(qts, GL_GET_LIGHT_HDR, 2, GL_POSITION,
                (const float[4]) { 4, 6, 8, 1 });
    gl_readback(qts, GL_GET_LIGHT_HDR, 2, GL_SPOT_DIRECTION,
                (const float[4]) { 0, 1, 0, 0 });
    gl_light4(qts, GL_LIGHT_DIFFUSE_HDR, 3, 0.5f, 0.25f, 0.125f, 1);
    gl_readback(qts, GL_GET_LIGHT_HDR, 3, GL_DIFFUSE,
                (const float[4]) { 0.5f, 0.25f, 0.125f, 1 });

    gl_material(qts, GL_FRONT_FACE, GL_DIFFUSE, 0.5f, 0.5f, 0.5f, 1);
    gl_readback(qts, GL_GET_MATERIAL_HDR, GL_FRONT_FACE, GL_DIFFUSE,
                (const float[4]) { 0.5f, 0.5f, 0.5f, 1 });
    gl_readback(qts, GL_GET_MATERIAL_HDR, GL_BACK_FACE, GL_DIFFUSE, grey);

    /* Colour material on the front diffuse tracks glColor. */
    gl_one(qts, GL_COLOR_MATERIAL_FRONT_HDR, GL_DIFFUSE);
    gl_word(qts, GL_COLOR_MATERIAL_ON);
    gl_three(qts, GL_COLOR3F_HDR, 0.25f, 0.5f, 0.75f);
    gl_readback(qts, GL_GET_MATERIAL_HDR, GL_FRONT_FACE, GL_DIFFUSE,
                (const float[4]) { 0.25f, 0.5f, 0.75f, 1 });
    g_assert_cmphex(gl_query(qts, GL_Q_COLOR_MATERIAL_FACE, 1), ==,
                    GL_FRONT_FACE);
    g_assert_cmphex(gl_query(qts, GL_Q_COLOR_MATERIAL_PARAMETER, 1), ==,
                    GL_DIFFUSE);

    /* The light model; alpha is not sent and stays 1. */
    g_assert_cmphex(gl_query(qts, GL_Q_LIGHT_MODEL_AMBIENT, 1), ==,
                    gl_bits(0.2f));
    gl_three(qts, GL_LIGHT_MODEL_AMBIENT_HDR, 0.5f, 0.25f, 0.125f);
    gl_one(qts, GL_LIGHT_MODEL_TWO_SIDE_HDR, 1);
    g_assert_cmphex(gl_query(qts, GL_Q_LIGHT_MODEL_AMBIENT + 2, 1), ==,
                    gl_bits(0.125f));
    g_assert_cmphex(gl_query(qts, GL_Q_TWO_SIDE, 1), ==, 1);

    /* A parameter the context does not keep reads back zeros. */
    gl_readback(qts, GL_GET_LIGHT_HDR, 9, GL_DIFFUSE, none);
}
#define GL_LIGHTING_OFF       0x00000e00
#define GL_LIGHT_OFF_HDR      0x0000a004
#define GL_NORMAL3D_HDR       0x12000718
#define GL_NORMAL3I_HDR       0x1c00070c
#define GL_NORMAL3S_HDR       0x1d000706
#define GL_NORMAL3B_HDR       0x1c800703

/*
 * Material diffuse (0.5, 0.25, 0.125) facing light 0 adds itself to the
 * 0.04 ambient: (0.54, 0.29, 0.165), rounded to eight bits 138, 74 and 42
 * (OpenGL 1.2.1, 2.13.1, 2.13.9). Disabled, lighting leaves the vertex
 * colour; with light 0 off only the ambient remains.
 */
static void test_gl_lit_triangle_matches_the_equation(void)
{
    gl_lit_start(qts);
    gl_material(qts, GL_FRONT_FACE, GL_DIFFUSE, 0.5f, 0.25f, 0.125f, 1);
    gl_three(qts, GL_NORMAL3F_HDR, 0, 0, 1);
    g_assert_cmphex(gl_lit_triangle(qts, false), ==, 0x8a4a2a);
    gl_word(qts, GL_LIGHTING_OFF);
    g_assert_cmphex(gl_lit_triangle(qts, false), ==, 0x00ff00);
    gl_word(qts, GL_LIGHTING_ON);
    gl_one(qts, GL_LIGHT_OFF_HDR, 0);
    g_assert_cmphex(gl_lit_triangle(qts, false), ==, 0x0a0a0a);
}

/*
 * 0x7104's word names the light: light 1 alone, red at OpenGL's initial
 * position, on the initial diffuse 0.8 gives red 0.84, 214.
 */
static void test_gl_light_enables_select_the_lights(void)
{
    gl_lit_start(qts);
    gl_one(qts, GL_LIGHT_OFF_HDR, 0);
    gl_light4(qts, GL_LIGHT_DIFFUSE_HDR, 1, 1, 0, 0, 1);
    gl_one(qts, GL_LIGHT_ON_HDR, 1);
    gl_three(qts, GL_NORMAL3F_HDR, 0, 0, 1);
    g_assert_cmphex(gl_lit_triangle(qts, false), ==, 0xd60a0a);
    gl_one(qts, GL_LIGHT_OFF_HDR, 1);
    g_assert_cmphex(gl_lit_triangle(qts, false), ==, 0x0a0a0a);
}

/*
 * The normal (0, 0, 2) doubles diffuse 0.25 to 0.5, 0.54 with the
 * ambient; 0xa304 with 1 makes it unit, 0.29, and with 0 no longer.
 */
static void test_gl_normalize_enable(void)
{
    gl_lit_start(qts);
    gl_material(qts, GL_FRONT_FACE, GL_DIFFUSE, 0.25f, 0.25f, 0.25f, 1);
    gl_three(qts, GL_NORMAL3F_HDR, 0, 0, 2);
    g_assert_cmphex(gl_lit_triangle(qts, false), ==, 0x8a8a8a);
    gl_one(qts, GL_NORMALIZE_HDR, 1);
    g_assert_cmphex(gl_lit_triangle(qts, false), ==, 0x4a4a4a);
    gl_one(qts, GL_NORMALIZE_HDR, 0);
    g_assert_cmphex(gl_lit_triangle(qts, false), ==, 0x8a8a8a);
}

/*
 * Each glNormal3 form's component reaches the lighting as table 2.6
 * converts it, (2c + 1) / (2^b - 1): a byte 63 is 127 / 255, a short
 * 16383 is 32767 / 65535, an int 0x3fffffff is 0.5 in single precision,
 * and the double 0.5 is itself. Facing a white light on diffuse 1, with
 * the 0.04 ambient, they give 137, 138, 138 and 138. A component read
 * from the wrong place gives 0 and the ambient alone, 10; one taken
 * unconverted gives 255. The z component is tested along +z, and the y
 * component of the short and byte forms along +y.
 */
static void test_gl_normal_forms_convert(void)
{
    static const struct {
        uint32_t header;
        uint32_t words[6];
        unsigned n;
        bool y;
        uint32_t pixel;
    } cases[] = {
        { GL_NORMAL3B_HDR, { 0x00003f00 }, 1, false, 0x898989 },
        { GL_NORMAL3B_HDR, { 0x003f0000 }, 1, true, 0x898989 },
        { GL_NORMAL3S_HDR, { 0, 0x3fff0000 }, 2, false, 0x8a8a8a },
        { GL_NORMAL3S_HDR, { 0x00003fff, 0 }, 2, true, 0x8a8a8a },
        { GL_NORMAL3I_HDR, { 0, 0, 0x3fffffff }, 3, false, 0x8a8a8a },
        { GL_NORMAL3D_HDR, { 0, 0, 0, 0, 0x3fe00000, 0 }, 6, false,
          0x8a8a8a },
    };

    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        gl_lit_start(qts);
        gl_material(qts, GL_FRONT_FACE, GL_DIFFUSE, 1, 1, 1, 1);
        if (cases[i].y) {
            gl_light4(qts, GL_LIGHT_POSITION_HDR, 0, 0, 1, 0, 0);
        }
        gl_word(qts, cases[i].header);
        for (unsigned k = 0; k < cases[i].n; k++) {
            gl_word(qts, cases[i].words[k]);
        }
        g_assert_cmphex(gl_lit_triangle(qts, false), ==, cases[i].pixel);
    }
}

/*
 * 0x5904 with 1 lights the back of a back-facing triangle (clockwise in
 * the window) with the back material and the reversed normal. The normal
 * (0, 0, -1) turns the front away from light 0, leaving the 0.04 ambient,
 * and the back toward it: back diffuse green 1 over the ambient, 0x0aff0a.
 * One-sided, the back-facing triangle takes the front colour.
 */
static void test_gl_two_sided_lighting_lights_the_back(void)
{
    gl_lit_start(qts);
    gl_material(qts, GL_FRONT_FACE, GL_DIFFUSE, 1, 0, 0, 1);
    gl_material(qts, GL_BACK_FACE, GL_DIFFUSE, 0, 1, 0, 1);
    gl_three(qts, GL_NORMAL3F_HDR, 0, 0, -1);
    g_assert_cmphex(gl_lit_triangle(qts, true), ==, 0x0a0a0a);
    gl_one(qts, GL_LIGHT_MODEL_TWO_SIDE_HDR, 1);
    g_assert_cmphex(gl_lit_triangle(qts, true), ==, 0x0aff0a);
    g_assert_cmphex(gl_lit_triangle(qts, false), ==, 0x0a0a0a);
}
#define GL_CULL_OFF           0x00007300
#define GL_CCW                0x901

/* The pixel a cleared window shows after gl_lit_triangle(). */
static uint32_t gl_cull_triangle(QTestState *qts, bool cw)
{
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    return gl_lit_triangle(qts, cw);
}

/*
 * Unlit and green over black, a triangle draws unless culled (OpenGL
 * 1.2.1, 3.5.1). Culling starts disabled; 0x7200 culls the clockwise,
 * back-facing one; 0x7404 with GL_FRONT the counter-clockwise one
 * instead, and with GL_FRONT_AND_BACK both; 0x5604 with GL_CW makes the
 * clockwise one the front, so GL_BACK culls the other; 0x7300 draws both
 * again.
 */
static void test_gl_cull_face_culls_by_facing(void)
{
    static const struct {
        uint32_t header;
        uint32_t word;
        uint32_t cw;
        uint32_t ccw;
    } steps[] = {
        { 0, 0, 0x00ff00, 0x00ff00 },
        { GL_CULL_ON, 0, 0, 0x00ff00 },
        { GL_CULL_FACE_HDR, GL_FRONT_FACE, 0x00ff00, 0 },
        { GL_CULL_FACE_HDR, GL_FRONT_AND_BACK, 0, 0 },
        { GL_CULL_FACE_HDR, GL_BACK_FACE, 0, 0x00ff00 },
        { GL_FRONT_FACE_HDR, GL_CW, 0x00ff00, 0 },
        { GL_CULL_OFF, 0, 0x00ff00, 0x00ff00 },
        { GL_FRONT_FACE_HDR, GL_CCW, 0x00ff00, 0x00ff00 },
    };

    gl_scene_start(qts, GL_FLAT);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_color3f(qts, 0, 1, 0);
    for (int i = 0; i < ARRAY_SIZE(steps); i++) {
        if (steps[i].header == GL_CULL_ON || steps[i].header == GL_CULL_OFF) {
            gl_word(qts, steps[i].header);
        } else if (steps[i].header) {
            gl_one(qts, steps[i].header, steps[i].word);
        }
        g_assert_cmphex(gl_cull_triangle(qts, true), ==, steps[i].cw);
        g_assert_cmphex(gl_cull_triangle(qts, false), ==, steps[i].ccw);
    }
}

/*
 * With the test enabled a draw and a clear touch only the box, window
 * columns 32 to 63 and rows 16 to 31 here (OpenGL 1.2.1, 4.1.2, 4.2.3);
 * disabled, the whole window again. The box and enable are answered.
 */
static void test_gl_scissor_bounds_draws_and_clears(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_query(qts, GL_Q_SCISSOR_TEST, 1), ==, 0);
    gl_scissor(qts, 32, 16, 32, 16);
    gl_one(qts, GL_SCISSOR_TEST_HDR, 1);
    g_assert_cmphex(gl_query(qts, GL_Q_SCISSOR_BOX, 1), ==, 32);
    g_assert_cmphex(gl_query(qts, GL_Q_SCISSOR_BOX + 1, 1), ==, 16);
    g_assert_cmphex(gl_query(qts, GL_Q_SCISSOR_BOX + 2, 1), ==, 63);
    g_assert_cmphex(gl_query(qts, GL_Q_SCISSOR_BOX + 3, 1), ==, 31);
    g_assert_cmphex(gl_query(qts, GL_Q_SCISSOR_TEST, 1), ==, 1);

    gl_fill_window(qts, 1, 1, 1);
    g_assert_cmphex(gl_window_pixel(qts, 32, 16), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 63, 31), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 31, 16), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 63, 32), ==, 0);
    gl_clear_color(qts, 1, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 40, 20), ==, 0xff0000);
    g_assert_cmphex(gl_window_pixel(qts, 64, 20), ==, 0);

    gl_one(qts, GL_SCISSOR_TEST_HDR, 0);
    g_assert_cmphex(gl_query(qts, GL_Q_SCISSOR_TEST, 1), ==, 0);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 64, 20), ==, 0xff0000);
}

/*
 * GL_GREATER against 2048 / 4095 discards alpha 0.25 and keeps 0.75 once
 * enabled (OpenGL 1.2.1, 4.1.4); disabled, both draw. The queries answer
 * the enable, the function and the reference as the client sent it.
 */
static void test_gl_alpha_test_discards(void)
{
    uint32_t a;

    gl_scene_start(qts, GL_FLAT);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_alpha_func(qts, GL_GREATER, 2048);
    g_assert_cmphex(gl_alpha_window(qts, 0.25f), ==, 0x00ff00);
    gl_one(qts, GL_ALPHA_TEST_HDR, 1);
    g_assert_cmphex(gl_alpha_window(qts, 0.25f), ==, 0);
    g_assert_cmphex(gl_alpha_window(qts, 0.75f), ==, 0x00ff00);
    g_assert_cmphex(gl_query(qts, GL_Q_PIXEL_ENABLES, 1) & 0x40, ==, 0x40);
    a = gl_query(qts, GL_Q_ALPHA, 1);
    g_assert_cmphex((a & 7) + 0x200, ==, 0x204);
    g_assert_cmphex((a >> 3) & 0xfff, ==, 2048);
    gl_one(qts, GL_ALPHA_TEST_HDR, 0);
    g_assert_cmphex(gl_alpha_window(qts, 0.25f), ==, 0x00ff00);
    g_assert_cmphex(gl_query(qts, GL_Q_PIXEL_ENABLES, 1) & 0x40, ==, 0);
}
#define GL_Q_STENCIL_CLEAR    0xc14

/*
 * Clear the stencil to 5, replace it with 1 in the window's left half
 * through the scissor, then draw green where the stencil equals 1 (OpenGL
 * 1.2.1, 4.1.5): only the left half turns green. The queries answer the
 * state as sent.
 */
static void test_gl_stencil_masks_drawing(void)
{
    uint32_t a;

    gl_scene_start(qts, GL_FLAT);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_one(qts, GL_CLEAR_STENCIL_HDR, 5);
    gl_word(qts, GL_CLEAR_STENCIL_BUFFER);
    gl_one(qts, GL_STENCIL_TEST_HDR, 1);
    gl_stencil(qts, GL_STENCIL_ALWAYS, 1, 0xff, GL_OP_KEEP, GL_OP_KEEP,
               GL_OP_REPLACE);
    gl_scissor(qts, 0, 0, 64, 128);
    gl_one(qts, GL_SCISSOR_TEST_HDR, 1);
    gl_fill_window(qts, 0, 0, 1);
    gl_one(qts, GL_SCISSOR_TEST_HDR, 0);
    gl_stencil(qts, GL_STENCIL_EQUAL, 1, 0xff, GL_OP_KEEP, GL_OP_KEEP,
               GL_OP_KEEP);
    gl_fill_window(qts, 0, 1, 0);
    g_assert_cmphex(gl_window_pixel(qts, 63, 64), ==, 0x00ff00);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0);

    gl_stencil(qts, GL_STENCIL_EQUAL, 0x1a5, 0x3c, GL_OP_INCR, GL_OP_KEEP,
               GL_OP_REPLACE);
    gl_one(qts, GL_STENCIL_MASK_HDR, 0x5a);
    gl_one(qts, GL_CLEAR_STENCIL_HDR, 7);
    g_assert_cmphex(gl_query(qts, GL_Q_PIXEL_ENABLES, 1) & 0x10, ==, 0x10);
    a = gl_query(qts, GL_Q_STENCIL, 1);
    g_assert_cmphex((a & 7) + 0x200, ==, 0x202);
    g_assert_cmphex((a >> 3) & 7, ==, GL_OP_INCR);
    g_assert_cmphex((a >> 6) & 7, ==, GL_OP_KEEP);
    g_assert_cmphex((a >> 9) & 7, ==, GL_OP_REPLACE);
    g_assert_cmphex((a >> 12) & 0xff, ==, 0xff);
    g_assert_cmphex(gl_query(qts, GL_Q_STENCIL_MASKS, 1) & 0xffff, ==,
                    0x5a3c);
    g_assert_cmphex(gl_query(qts, GL_Q_STENCIL_CLEAR, 1), ==, 7);
    /*
     * The depth and index clear values beside it, 0xc12 and 0xc13; the
     * initial depth is 1.0, 24 bits of ones.
     */
    g_assert_cmphex(gl_query(qts, GL_Q_STENCIL_CLEAR - 2, 1), ==, 0xffffff);
    gl_one(qts, GL_CLEAR_DEPTH_HDR, 0x123456);
    g_assert_cmphex(gl_query(qts, GL_Q_STENCIL_CLEAR - 2, 1), ==, 0x123456);
    gl_word(qts, GL_CLEAR_INDEX_HDR);
    gl_float(qts, 2.5f);
    g_assert_cmphex(gl_query(qts, GL_Q_STENCIL_CLEAR - 1, 1), ==,
                    gl_bits(2.5f));
    gl_one(qts, GL_STENCIL_TEST_HDR, 0);
    g_assert_cmphex(gl_query(qts, GL_Q_PIXEL_ENABLES, 1) & 0x10, ==, 0);
}

/*
 * glStencilOp's codes 3 and 4 are GL_INCR and GL_DECR, as the listing's
 * table gives them (__glMgrim_StencilOp 0xd95ef80, its mapping at
 * 0xd95f074-0xd95f1b4, in mgras-gl-commands.md). Over a stencil cleared
 * to 5, one pass with code 3 leaves 6 and then two with code 4 leave 4,
 * each shown by a green fill where the stencil equals the expected value
 * and not where it equals the other.
 */
#define GL_OP_DECR            4

static uint32_t gl_stencil_fill_if(QTestState *qts, uint32_t ref)
{
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_stencil(qts, GL_STENCIL_EQUAL, ref, 0xff, GL_OP_KEEP, GL_OP_KEEP,
               GL_OP_KEEP);
    gl_fill_window(qts, 0, 1, 0);
    return gl_window_pixel(qts, 64, 64);
}

static void test_gl_stencil_incr_and_decr_codes(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_one(qts, GL_CLEAR_STENCIL_HDR, 5);
    gl_word(qts, GL_CLEAR_STENCIL_BUFFER);
    gl_one(qts, GL_STENCIL_TEST_HDR, 1);
    gl_stencil(qts, GL_STENCIL_ALWAYS, 0, 0xff, GL_OP_KEEP, GL_OP_KEEP,
               GL_OP_INCR);
    gl_fill_window(qts, 0, 0, 1);
    g_assert_cmphex(gl_stencil_fill_if(qts, 4), ==, 0);
    g_assert_cmphex(gl_stencil_fill_if(qts, 6), ==, 0x00ff00);

    gl_stencil(qts, GL_STENCIL_ALWAYS, 0, 0xff, GL_OP_KEEP, GL_OP_KEEP,
               GL_OP_DECR);
    gl_fill_window(qts, 0, 0, 1);
    gl_fill_window(qts, 0, 0, 1);
    g_assert_cmphex(gl_stencil_fill_if(qts, 6), ==, 0);
    g_assert_cmphex(gl_stencil_fill_if(qts, 4), ==, 0x00ff00);
}

/*
 * The stencil and depth buffers share each pixel's plane in the model:
 * clearing either keeps the other. After a depth-tested quad at depth
 * 0.5, a stencil clear to 5 leaves that depth, so a farther quad fails;
 * after a depth clear to 1.0 a stencil test for 5 still passes.
 */
static void test_gl_stencil_clear_keeps_depth(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_one(qts, GL_CLEAR_DEPTH_HDR, GL_DEPTH_ONE);
    gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
    gl_fill_window_at(qts, 0, 0, 1, 0);
    gl_one(qts, GL_CLEAR_STENCIL_HDR, 5);
    gl_word(qts, GL_CLEAR_STENCIL_BUFFER);
    gl_fill_window_at(qts, 1, 0, 0, 0.5f);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x0000ff);

    gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
    gl_one(qts, GL_STENCIL_TEST_HDR, 1);
    gl_stencil(qts, GL_STENCIL_EQUAL, 5, 0xff, GL_OP_KEEP, GL_OP_KEEP,
               GL_OP_KEEP);
    gl_fill_window_at(qts, 0, 1, 0, 0.5f);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x00ff00);
    gl_stencil(qts, GL_STENCIL_EQUAL, 4, 0xff, GL_OP_KEEP, GL_OP_KEEP,
               GL_OP_KEEP);
    gl_fill_window_at(qts, 1, 1, 1, -0.5f);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x00ff00);
}
#define GL_Q_BLEND            0xc4a
#define GL_ONE_MINUS_SRC_ALPHA 0x303
#define GL_CONSTANT_COLOR     0x8001
#define GL_FUNC_REVERSE_SUBTRACT 0x800b

/*
 * Over a red window (OpenGL 1.2.1, 4.1.6): green of alpha 0.25 by its
 * alpha gives (191, 64, 0); the reverse difference with GL_ONE of red 0.5
 * gives 127; the constant (0.5, 0.25, 1, 1) of white 0.5 red and 0.25
 * green added to the window. Disabled, the source replaces the window.
 */
static void test_gl_blend_mixes_with_the_window(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_clear_color(qts, 1, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_one(qts, GL_BLEND_HDR, 1);
    gl_blend(qts, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD);
    g_assert_cmphex(gl_query(qts, GL_Q_BLEND, 1), ==, 1);
    g_assert_cmphex(gl_query(qts, GL_Q_BLEND + 1, 1), ==, GL_SRC_ALPHA);
    g_assert_cmphex(gl_query(qts, GL_Q_BLEND + 2, 1), ==,
                    GL_ONE_MINUS_SRC_ALPHA);
    g_assert_cmphex(gl_query(qts, GL_Q_BLEND + 3, 1), ==, GL_FUNC_ADD);
    g_assert_cmphex(gl_alpha_window(qts, 0.25f), ==, 0xbf4000);

    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_blend(qts, 1, 1, GL_FUNC_REVERSE_SUBTRACT);
    gl_fill_window(qts, 0.5f, 0, 0);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x7f0000);

    gl_word(qts, GL_BLEND_COLOR_HDR);
    gl_float(qts, 0.5f);
    gl_float(qts, 0.25f);
    gl_float(qts, 1);
    gl_float(qts, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_blend(qts, GL_CONSTANT_COLOR, 1, GL_FUNC_ADD);
    gl_fill_window(qts, 1, 1, 0);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0xff4000);

    gl_one(qts, GL_BLEND_HDR, 0);
    g_assert_cmphex(gl_query(qts, GL_Q_BLEND, 1), ==, 0);
    gl_fill_window(qts, 0, 0, 1);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x0000ff);
}

/*
 * 0xcc XOR 0xf0 is 0x3c in every channel (OpenGL 1.2.1, 4.1.8), and
 * disabled the source replaces the window again.
 */
static void test_gl_logic_op_combines_with_the_window(void)
{
    const float d = 0xf0 / 255.0f, c = 0xcc / 255.0f;

    gl_scene_start(qts, GL_FLAT);
    gl_clear_color(qts, d, d, d, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex((gl_query(qts, GL_Q_PIXEL_ENABLES, 1) >> 26) & 0xf, ==,
                    3);
    gl_one(qts, GL_LOGIC_OP_HDR, GL_LOGIC_XOR);
    gl_one(qts, GL_LOGIC_OP_ENABLE_HDR, 1);
    g_assert_cmphex((gl_query(qts, GL_Q_PIXEL_ENABLES, 1) >> 26) & 0xf, ==,
                    GL_LOGIC_XOR);
    gl_fill_window(qts, c, c, c);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x3c3c3c);
    gl_one(qts, GL_LOGIC_OP_ENABLE_HDR, 0);
    gl_fill_window(qts, c, c, c);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0xcccccc);
}
#define GL_Q_COLOR_WRITEMASK  0x32b

/*
 * Masking green over a window of (0, 0.5, 1) keeps its 0x80 (OpenGL
 * 1.2.1, 4.2.2) under a yellow draw and a white clear; masking every
 * channel writes nothing.
 */
static void test_gl_colour_mask_keeps_channels(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_clear_color(qts, 0, 0.5f, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_one(qts, GL_COLOR_MASK_HDR, 0xd);
    g_assert_cmphex(gl_query(qts, GL_Q_COLOR_WRITEMASK, 1), ==, 0xd);
    gl_fill_window(qts, 1, 1, 0);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0xff8000);
    gl_clear_color(qts, 1, 1, 1, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0xff80ff);
    gl_one(qts, GL_COLOR_MASK_HDR, 0x8);
    gl_fill_window(qts, 0, 0, 0);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0xff80ff);
    gl_one(qts, GL_COLOR_MASK_HDR, 0xf);
    g_assert_cmphex(gl_query(qts, GL_Q_COLOR_WRITEMASK, 1), ==, 0xf);
}
#define GL_FOG_DENSITY_HDR    0x0000b704
#define GL_FOG_INDEX_HDR      0x0000b804
#define GL_Q_FOG_MODE         0xbce

/*
 * Blue at eye depth 0.5 under GL_LINEAR fog from 0 to 2 toward red takes
 * f 0.75 (OpenGL 1.2.1, 3.10): (64, 0, 191). The queries answer the state
 * as sent.
 */
static void test_gl_fog_blends_toward_the_fog_colour(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_fog_linear(qts, 0, 2);
    gl_one(qts, GL_FOG_INDEX_HDR, 5);
    gl_word(qts, GL_FOG_DENSITY_HDR);
    gl_float(qts, 0.25f);
    gl_fill_window_at(qts, 0, 0, 1, -0.5f);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x0000ff);
    g_assert_cmphex(gl_query(qts, GL_Q_FILL_ENABLES, 1) & 0x20000, ==, 0);
    gl_one(qts, GL_FOG_HDR, 1);
    g_assert_cmphex(gl_query(qts, GL_Q_FILL_ENABLES, 1) & 0x20000, ==,
                    0x20000);
    gl_fill_window_at(qts, 0, 0, 1, -0.5f);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x4000bf);
    g_assert_cmphex(gl_query(qts, GL_Q_FOG_MODE, 1), ==, GL_LINEAR);
    g_assert_cmphex(gl_query(qts, GL_Q_FOG_MODE + 1, 1), ==,
                    gl_bits(0.25f));
    g_assert_cmphex(gl_query(qts, GL_Q_FOG_MODE + 3, 1), ==, gl_bits(2));
    g_assert_cmphex(gl_query(qts, GL_Q_FOG_MODE + 4, 1), ==, gl_bits(1));
    g_assert_cmphex(gl_query(qts, GL_Q_FOG_MODE + 8, 1), ==, 5);
    gl_one(qts, GL_FOG_HDR, 0);
    gl_fill_window_at(qts, 0, 0, 1, -0.5f);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x0000ff);
}

/* The float-backed index must saturate before its native integer query. */
static void test_gl_fog_index_integer_endpoints(void)
{
    static const int32_t values[] = {
        INT32_MIN, -123, 0, 123, INT32_MAX - 127, INT32_MAX,
    };

    mgras_reset();
    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_enter_slot(qts, GL_SLOT_B);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_one(qts, GL_FOG_INDEX_HDR, 17);
    for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
        gl_enter_slot(qts, GL_SLOT_A);
        gl_one(qts, GL_FOG_INDEX_HDR, values[i]);
        g_assert_cmphex(gl_query(qts, GL_Q_FOG_MODE + 8, 1), ==,
                        (uint32_t)values[i]);
        gl_enter_slot(qts, GL_SLOT_B);
        g_assert_cmphex(gl_query(qts, GL_Q_FOG_MODE + 8, 1), ==, 17);
    }
    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    g_assert_cmphex(gl_query(qts, GL_Q_FOG_MODE + 8, 1), ==, 0);
}
#define GL_Q_OFFSET_FACTOR    0xca2

/*
 * A second quad at the depth of a first fails GL_LESS (OpenGL 1.2.1,
 * 3.5.5) until offset one depth step nearer by glPolygonOffset, or by
 * glPolygonOffsetEXT's bias, with the fill enable; the line and point
 * enables alone offset no filled polygon.
 */
static void test_gl_polygon_offset_moves_depth(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_one(qts, GL_CLEAR_DEPTH_HDR, GL_DEPTH_ONE);
    gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
    gl_one(qts, GL_DEPTH_TEST_HDR, 1);
    gl_one(qts, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
    gl_fill_window(qts, 1, 0, 0);
    gl_polygon_offset(qts, 0, -1, false);
    gl_one(qts, GL_OFFSET_ENABLES_HDR, 6);
    gl_fill_window(qts, 0, 1, 0);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0xff0000);
    gl_one(qts, GL_OFFSET_ENABLES_HDR, 1);
    gl_fill_window(qts, 0, 1, 0);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x00ff00);
    g_assert_cmphex(gl_query(qts, GL_Q_OFFSET_FACTOR + 2, 1), ==,
                    gl_bits(-1));
    g_assert_cmphex(gl_query(qts, GL_Q_OFFSET_FACTOR + 1, 1), ==, 0);

    gl_polygon_offset(qts, 0, -1.0f / 1024, true);
    gl_fill_window(qts, 0, 0, 1);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x0000ff);
    g_assert_cmphex(gl_query(qts, GL_Q_OFFSET_FACTOR + 1, 1), ==,
                    gl_bits(-1.0f / 1024));
    g_assert_cmphex(gl_query(qts, GL_Q_OFFSET_FACTOR + 2, 1), ==, 0);
    gl_one(qts, GL_OFFSET_ENABLES_HDR, 0);
    gl_fill_window(qts, 1, 1, 1);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x0000ff);
}
#define GL_Q_CLIP_ENABLES     0xb87
#define GL_Q_CLIP_PLANES      0xb8d

/*
 * x >= 0 given under a translation of 0.5 in x keeps eye x >= 0.5
 * (OpenGL 1.2.1, 2.11), window columns 96 and up, while GL_CLIP_PLANE2 is
 * enabled; glGetClipPlane answers (1, 0, 0, -0.5).
 */
static void test_gl_clip_plane_clips_in_eye_space(void)
{
    gl_scene_start(qts, GL_FLAT);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_three(qts, GL_TRANSLATE_HDR, 0.5f, 0, 0);
    gl_clip_plane(qts, 2, 1, 0, 0, 0);
    gl_word(qts, GL_LOAD_IDENTITY);
    g_assert_cmphex(gl_query(qts, GL_Q_CLIP_ENABLES + 2, 1), ==, 0);
    gl_one(qts, GL_CLIP_PLANE_ON_HDR, GL_CLIP_PLANE0 + 2);
    g_assert_cmphex(gl_query(qts, GL_Q_CLIP_ENABLES + 2, 1) >> 31, ==, 1);
    g_assert_cmphex(gl_query(qts, GL_Q_CLIP_PLANES + 8, 1), ==, gl_bits(1));
    g_assert_cmphex(gl_query(qts, GL_Q_CLIP_PLANES + 11, 1), ==,
                    gl_bits(-0.5f));
    gl_fill_window(qts, 0, 1, 0);
    g_assert_cmphex(gl_window_pixel(qts, 96, 64), ==, 0x00ff00);
    g_assert_cmphex(gl_window_pixel(qts, 95, 64), ==, 0);
    gl_one(qts, GL_CLIP_PLANE_OFF_HDR, GL_CLIP_PLANE0 + 2);
    g_assert_cmphex(gl_query(qts, GL_Q_CLIP_ENABLES + 2, 1), ==, 0);
    gl_fill_window(qts, 0, 1, 0);
    g_assert_cmphex(gl_window_pixel(qts, 95, 64), ==, 0x00ff00);
}
#define GL_Q_POLYGON_MODE     0xc17
#define GL_Q_LINE_STIPPLE     0xc46
#define GL_FILL_MODE          0x1b02

/*
 * The context setup's initial values answer as OpenGL's, not zero, which
 * glPointSize and glLineWidth would reject; what the client sets answers
 * back; and the window still draws filled at size 1.
 */
static void test_gl_line_point_state_answers(void)
{
    gl_scene_start(qts, GL_FLAT);
    g_assert_cmphex(gl_query(qts, GL_Q_POINT_SIZE, 1), ==, gl_bits(1));
    g_assert_cmphex(gl_query(qts, GL_Q_POINT_SIZE + 1, 1), ==, gl_bits(1));
    g_assert_cmphex(gl_query(qts, GL_Q_POINT_SIZE + 2, 1), ==,
                    GL_BACK_FACE);
    g_assert_cmphex(gl_query(qts, GL_Q_POINT_SIZE + 3, 1), ==, GL_CCW);
    g_assert_cmphex(gl_query(qts, GL_Q_POLYGON_MODE, 1), ==, GL_FILL_MODE);
    g_assert_cmphex(gl_query(qts, GL_Q_LINE_STIPPLE, 1) & 0xffff, ==,
                    0xffff);
    g_assert_cmphex(gl_query(qts, GL_Q_LINE_STIPPLE + 1, 1) + 1, ==, 1);

    gl_word(qts, GL_POINT_SIZE_HDR);
    gl_float(qts, 4);
    gl_word(qts, GL_LINE_WIDTH_HDR);
    gl_float(qts, 2.5f);
    gl_word(qts, GL_LINE_STIPPLE_HDR);
    gl_word(qts, 2);
    gl_word(qts, 0xff00);
    gl_word(qts, GL_POLYGON_MODE_HDR);
    gl_word(qts, GL_BACK_FACE);
    gl_word(qts, GL_LINE_MODE);
    gl_one(qts, GL_LINE_SMOOTH_HDR, 1);
    gl_one(qts, GL_LINE_STIPPLE_ON_HDR, 1);
    gl_one(qts, GL_POLYGON_STIPPLE_HDR, 1);
    g_assert_cmphex(gl_query(qts, GL_Q_POINT_SIZE, 1), ==, gl_bits(4));
    g_assert_cmphex(gl_query(qts, GL_Q_POINT_SIZE + 1, 1), ==,
                    gl_bits(2.5f));
    g_assert_cmphex(gl_query(qts, GL_Q_POLYGON_MODE, 1), ==, GL_FILL_MODE);
    g_assert_cmphex(gl_query(qts, GL_Q_POLYGON_MODE + 1, 1), ==,
                    GL_LINE_MODE);
    g_assert_cmphex(gl_query(qts, GL_Q_LINE_SMOOTH, 1), ==, 1);
    g_assert_cmphex(gl_query(qts, GL_Q_LINE_STIPPLE, 1) & 0xffff, ==,
                    0xff00);
    g_assert_cmphex(gl_query(qts, GL_Q_LINE_STIPPLE + 1, 1) + 1, ==, 3);
    g_assert_cmphex(gl_query(qts, GL_Q_FILL_ENABLES, 1) & 0x21, ==, 0x21);
    gl_one(qts, GL_LINE_STIPPLE_ON_HDR, 0);
    g_assert_cmphex(gl_query(qts, GL_Q_FILL_ENABLES, 1) & 0x21, ==, 1);

    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_fill_window(qts, 0, 1, 0);
    g_assert_cmphex(gl_window_pixel(qts, 64, 64), ==, 0x00ff00);
}

/* Analytical OpenGL footprints in window pixels, including a black border. */
static void gl_width_check_rect(QTestState *qts, int x0, int y0,
                                int x1, int y1, uint32_t colour)
{
    for (int y = y0 - 1; y <= y1 + 1; y++) {
        for (int x = x0 - 1; x <= x1 + 1; x++) {
            uint32_t expected = x >= x0 && x <= x1 && y >= y0 && y <= y1 ?
                                colour : 0;
            uint32_t actual = gl_window_pixel(qts, x, y);

            if (actual != expected) {
                g_test_message("pixel (%d, %d), rectangle (%d, %d)-(%d, %d)",
                               x, y, x0, y0, x1, y1);
            }
            g_assert_cmphex(actual, ==, expected);
        }
    }
}

static void gl_polygon_quad(QTestState *s, bool reverse, bool first_edge)
{
    static const float xy[4][2] = {
        { 8.5f, 8.5f }, { 24.5f, 8.5f },
        { 24.5f, 24.5f }, { 8.5f, 24.5f },
    };

    gl_word(s, GL_BEGIN_QUADS);
    for (unsigned i = 0; i < 4; i++) {
        unsigned v = reverse ? 3 - i : i;

        gl_word(s, i || first_edge ? 0x00000f00 : 0x00001000);
        gl_vertex2f(s, xy[v][0], xy[v][1]);
    }
    gl_word(s, GL_END_OF(GL_BEGIN_QUADS));
}

static void test_gl_native_polygon_line(void)
{
    gl_transform_start(qts);
    gl_width_clear(qts);
    gl_polygon_mode(qts, GL_FRONT_AND_BACK, GL_LINE_MODE);
    gl_polygon_quad(qts, false, true);
    for (unsigned y = 7; y <= 25; y++) {
        for (unsigned x = 7; x <= 25; x++) {
            bool boundary = x >= 8 && x <= 24 && y >= 8 && y <= 24 &&
                            (x == 8 || x == 24 || y == 8 || y == 24);

            g_assert_cmphex(gl_window_pixel(qts, x, y), ==,
                            boundary ? 0xffffff : 0);
        }
    }
}

static void test_gl_native_polygon_point(void)
{
    gl_transform_start(qts);
    gl_width_clear(qts);
    gl_polygon_mode(qts, GL_FRONT_AND_BACK, 0x1b00);
    gl_word(qts, GL_POINT_SIZE_HDR);
    gl_float(qts, 3);
    gl_polygon_quad(qts, false, true);
    for (unsigned y = 6; y <= 26; y++) {
        for (unsigned x = 6; x <= 26; x++) {
            bool vertex = ((x >= 7 && x <= 9) || (x >= 23 && x <= 25)) &&
                          ((y >= 7 && y <= 9) || (y >= 23 && y <= 25));

            g_assert_cmphex(gl_window_pixel(qts, x, y), ==,
                            vertex ? 0xffffff : 0);
        }
    }
}

static void test_gl_native_polygon_edge_flags(void)
{
    gl_transform_start(qts);
    gl_width_clear(qts);
    gl_polygon_mode(qts, GL_FRONT_AND_BACK, GL_LINE_MODE);
    gl_polygon_quad(qts, false, false);
    g_assert_cmphex(gl_window_pixel(qts, 16, 8), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 24, 16), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 16, 16), ==, 0);
    gl_width_clear(qts);
    gl_polygon_mode(qts, GL_FRONT_AND_BACK, 0x1b00);
    gl_word(qts, GL_POINT_SIZE_HDR);
    gl_float(qts, 3);
    gl_polygon_quad(qts, false, false);
    g_assert_cmphex(gl_window_pixel(qts, 8, 8), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 24, 8), ==, 0xffffff);
}

/* A zero pattern masks native outlines and standalone lines alike. */
static void test_gl_native_polygon_stipple_applied(void)
{
    gl_transform_start(qts);
    gl_width_clear(qts);
    gl_polygon_mode(qts, GL_FRONT_AND_BACK, GL_LINE_MODE);
    gl_word(qts, GL_LINE_STIPPLE_HDR);
    gl_word(qts, 0);
    gl_word(qts, 0);
    gl_one(qts, GL_LINE_STIPPLE_ON_HDR, 1);
    gl_polygon_quad(qts, false, true);
    g_assert_cmphex(gl_window_pixel(qts, 16, 8), ==, 0);
    g_assert_cmphex(gl_query(qts, GL_Q_LINE_STIPPLE, 1) & 0xffff, ==, 0);
    g_assert_cmphex(gl_query(qts, GL_Q_FILL_ENABLES, 1) & 0x20, ==, 0x20);
    gl_word(qts, GL_BEGIN_LINES);
    gl_vertex2f(qts, 40.5f, 8.5f);
    gl_vertex2f(qts, 56.5f, 8.5f);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
    g_assert_cmphex(gl_window_pixel(qts, 48, 8), ==, 0);
}

static void test_gl_native_stipple_empty_binding(void)
{
    for (unsigned outside = 0; outside < 2; outside++) {
        mgras_reset();
        gl_native_stipple_hidden_setup(qts, outside ? 0x1f : 0x10,
                                       outside, 0x0400);
        gl_native_stipple_hidden_check(qts);
    }
}

/* All-one patterns distinguish rectangle rejection from stipple rejection. */
static void test_gl_native_stipple_no_write(void)
{
    static const uint32_t clips[] = { 0x10, 0x1f, 0x20, 0, 0x31 };

    for (unsigned i = 0; i < ARRAY_SIZE(clips); i++) {
        const uint32_t image[] = { 0xff000000 };

        mgras_reset();
        gl_native_stipple_hidden_setup(qts, clips[i], i == 1, 0xffff);
        g_assert_cmpuint(mgras_qom_get_uint(qts, "x-test-line-phase"), ==,
                         i < 2 ? 5 : 0);
        gl_word(qts, GL_END_OF(GL_BEGIN_LINE_STRIP));
        gl_clear_color(qts, 1, 1, 1, 1);
        gl_word(qts, GL_CLEAR_COLOR_BUFFER);
        gl_one(qts, GL_CLEAR_DEPTH_HDR, 0xffffff);
        gl_word(qts, GL_CLEAR_DEPTH_BUFFER);
        gl_one(qts, GL_CLEAR_STENCIL_HDR, 0xff);
        gl_word(qts, GL_CLEAR_STENCIL_BUFFER);
        gl_one(qts, GL_LINE_STIPPLE_ON_HDR, 0);
        gl_word(qts, GL_BEGIN_LINES);
        gl_vertex2f(qts, 8.5f, 8.5f);
        gl_vertex2f(qts, 13.5f, 8.5f);
        gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
        gl_raster_pos2f(qts, 8.5f, 8.5f);
        gl_bitmap_packet(qts, 8, 1, 0, 0, 0, 0);
        gl_bitmap_image(qts, image, ARRAY_SIZE(image));
        for (unsigned x = 8; x < 16; x++) {
            g_assert_cmphex(gl_window_pixel(qts, x, 8), ==, 0);
            g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-back-pixel"),
                            ==, 0);
            g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-depth-stencil"),
                            ==, 0);
        }
    }
}

static void test_gl_native_stipple_unbound(void)
{
    static const uint32_t clips[] = { 0x20, 0, 0x31 };

    for (unsigned i = 0; i < ARRAY_SIZE(clips); i++) {
        mgras_reset();
        gl_native_stipple_hidden_setup(qts, clips[i], false, 0x8000);
        g_assert_cmpuint(mgras_qom_get_uint(qts, "x-test-line-phase"), ==, 0);
        gl_native_stipple_bind(qts, 0x11, false);
        gl_vertex2f(qts, 17.5f, 8.5f);
        g_assert_cmpuint(mgras_qom_get_uint(qts, "x-test-line-phase"), ==, 4);
        gl_word(qts, GL_END_OF(GL_BEGIN_LINE_STRIP));
        for (unsigned x = 8; x < 18; x++) {
            g_assert_cmphex(gl_window_pixel(qts, x, 8), ==,
                            x == 13 ? 0xffffff : 0);
            g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-back-pixel"), ==,
                            x == 13 ? 0xffffff : 0);
            g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-depth-stencil"), ==,
                            x == 13 ? 0x07800000 : 0);
        }
    }
}

/* Packet bit 15 becomes API bit zero; width replicas share each decision. */
static void test_gl_native_stipple_axes(void)
{
    for (unsigned axis = 0; axis < 2; axis++) {
        for (unsigned reverse = 0; reverse < 2; reverse++) {
            gl_transform_start(qts);
            gl_width_clear(qts);
            gl_native_stipple(qts, 2, 0x8000);
            gl_word(qts, GL_LINE_WIDTH_HDR);
            gl_float(qts, 3);
            gl_word(qts, GL_BEGIN_LINES);
            float start = reverse ? 42.5f : 8.5f;
            float end = reverse ? 8.5f : 42.5f;

            gl_vertex2f(qts, axis ? 8.5f : start, axis ? start : 8.5f);
            gl_vertex2f(qts, axis ? 8.5f : end, axis ? end : 8.5f);
            gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
            for (unsigned pos = 7; pos <= 43; pos++) {
                bool on = reverse ? (pos == 42 || pos == 41 ||
                                      pos == 10 || pos == 9) :
                                     (pos == 8 || pos == 9 ||
                                      pos == 40 || pos == 41);

                for (unsigned cross = 6; cross <= 10; cross++) {
                    uint32_t pixel = gl_window_pixel(qts,
                                        axis ? cross : pos,
                                        axis ? pos : cross);

                    g_assert_cmphex(pixel, ==,
                                    on && cross >= 7 && cross <= 9 ?
                                    0xffffff : 0);
                }
            }
        }
    }
}

static void test_gl_native_stipple_pairs(void)
{
    gl_transform_start(qts);
    gl_width_clear(qts);
    gl_native_stipple(qts, 1, 0x8000);
    gl_word(qts, GL_BEGIN_LINES);
    for (unsigned row = 8; row <= 16; row += 8) {
        gl_vertex2f(qts, 8.5f, row + 0.5f);
        gl_vertex2f(qts, 13.5f, row + 0.5f);
    }
    gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
    for (unsigned row = 8; row <= 16; row += 8) {
        for (unsigned x = 8; x <= 13; x++) {
            g_assert_cmphex(gl_window_pixel(qts, x, row), ==,
                            x == 8 ? 0xffffff : 0);
        }
    }
}

static void test_gl_native_stipple_masks(void)
{
    for (unsigned scissor = 0; scissor < 2; scissor++) {
        gl_transform_start(qts);
        gl_width_clear(qts);
        gl_native_stipple(qts, 1, 0x0400);
        if (scissor) {
            gl_scissor(qts, 13, 8, 4, 1);
            gl_one(qts, GL_SCISSOR_TEST_HDR, 1);
        } else {
            uint32_t rects[4][2] = {
                { GL_CLIP_SPAN(269, 272), GL_CLIP_SPAN(648, 648) },
            };

            mgras_enter_gl_clip(qts, (640 << 16) | 256, 0x11, rects);
        }
        gl_word(qts, GL_BEGIN_LINE_STRIP);
        gl_vertex2f(qts, 8.5f, 8.5f);
        gl_vertex2f(qts, 13.5f, 8.5f);
        g_assert_cmpuint(mgras_qom_get_uint(qts, "x-test-line-phase"), ==, 5);
        gl_vertex2f(qts, 17.5f, 8.5f);
        gl_word(qts, GL_END_OF(GL_BEGIN_LINE_STRIP));
        for (unsigned x = 8; x < 18; x++) {
            g_assert_cmphex(gl_window_pixel(qts, x, 8), ==,
                            x == 13 ? 0xffffff : 0);
        }
    }
}

static void test_gl_native_stipple_contexts(void)
{
    mgras_reset();
    gl_native_stipple_slot(qts, GL_SLOT_A);
    gl_native_stipple(qts, 1, 0x0400);
    gl_word(qts, GL_BEGIN_LINE_STRIP);
    gl_vertex2f(qts, 8.5f, 8.5f);
    gl_vertex2f(qts, 13.5f, 8.5f);
    gl_native_stipple_slot(qts, GL_SLOT_B);
    gl_native_stipple(qts, 1, 0x8000);
    gl_word(qts, GL_BEGIN_LINE_STRIP);
    gl_vertex2f(qts, 8.5f, 16.5f);
    gl_vertex2f(qts, 11.5f, 16.5f);
    g_assert_cmpuint(mgras_qom_get_uint(qts, "x-test-line-phase"), ==, 3);
    gl_enter_slot(qts, GL_SLOT_A);
    g_assert_cmpuint(mgras_qom_get_uint(qts, "x-test-line-phase"), ==, 5);
    gl_vertex2f(qts, 17.5f, 8.5f);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINE_STRIP));
    g_assert_cmphex(gl_window_pixel(qts, 13, 8), ==, 0xffffff);
    gl_word(qts, GL_CONTEXT_OPENER);
    g_assert_cmpuint(mgras_qom_get_uint(qts, "x-test-line-phase"), ==, 0);
    g_assert_cmphex(gl_query(qts, GL_Q_FILL_ENABLES, 1) & 0x20, ==, 0);
    gl_enter_slot(qts, GL_SLOT_B);
    g_assert_cmpuint(mgras_qom_get_uint(qts, "x-test-line-phase"), ==, 3);
    gl_vertex2f(qts, 14.5f, 16.5f);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINE_STRIP));
    for (unsigned x = 8; x < 15; x++) {
        g_assert_cmphex(gl_window_pixel(qts, x, 16), ==,
                        x == 8 ? 0xffffff : 0);
    }
    mgras_reset();
    g_assert_cmpuint(mgras_qom_get_uint(qts, "x-test-line-phase"), ==, 0);
    gl_native_stipple_slot(qts, GL_SLOT_A);
    gl_native_stipple(qts, 1, 0x8000);
    gl_word(qts, GL_BEGIN_LINES);
    gl_vertex2f(qts, 8.5f, 8.5f);
    gl_vertex2f(qts, 13.5f, 8.5f);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
    g_assert_cmphex(gl_window_pixel(qts, 8, 8), ==, 0xffffff);
}


static void test_gl_native_stipple_parameters(void)
{
    gl_transform_start(qts);
    gl_native_stipple(qts, 3, 0x8400);
    g_assert_cmphex(gl_query(qts, GL_Q_LINE_STIPPLE, 1) & 0xffff,
                    ==, 0x8400);
    g_assert_cmpuint(gl_query(qts, GL_Q_LINE_STIPPLE + 1, 1), ==, 2);
    g_assert_cmphex(gl_query(qts, GL_Q_FILL_ENABLES, 1) & 0x20, ==, 0x20);
    gl_word(qts, GL_BEGIN_LINE_STRIP);
    gl_native_stipple(qts, 7, 0xffff);
    gl_one(qts, GL_LINE_STIPPLE_ON_HDR, 0);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINE_STRIP));
    g_assert_cmphex(gl_query(qts, GL_Q_LINE_STIPPLE, 1) & 0xffff,
                    ==, 0x8400);
    g_assert_cmpuint(gl_query(qts, GL_Q_LINE_STIPPLE + 1, 1), ==, 2);
    g_assert_cmphex(gl_query(qts, GL_Q_FILL_ENABLES, 1) & 0x20, ==, 0x20);
}

/* A 64-position boundary continues through corners without a diagonal. */
static void test_gl_native_stipple_polygon(void)
{
    gl_transform_start(qts);
    gl_width_clear(qts);
    gl_native_stipple(qts, 3, 0x8000);
    gl_polygon_mode(qts, GL_FRONT_AND_BACK, GL_LINE_MODE);
    gl_polygon_quad(qts, false, true);
    g_assert_cmpuint(mgras_qom_get_uint(qts, "x-test-line-phase"), ==, 0);
    for (unsigned y = 7; y <= 25; y++) {
        for (unsigned x = 7; x <= 25; x++) {
            bool on = (y == 8 && x >= 8 && x <= 10) ||
                      (x == 8 && y >= 22 && y <= 24);

            g_assert_cmphex(gl_window_pixel(qts, x, y), ==,
                            on ? 0xffffff : 0);
        }
    }
    gl_native_stipple(qts, 1, 0);
    gl_width_clear(qts);
    gl_polygon_mode(qts, GL_FRONT_AND_BACK, GL_FILL_MODE);
    gl_polygon_quad(qts, false, true);
    g_assert_cmphex(gl_window_pixel(qts, 16, 16), ==, 0xffffff);
    gl_width_clear(qts);
    gl_polygon_mode(qts, GL_FRONT_AND_BACK, 0x1b00);
    gl_polygon_quad(qts, false, true);
    g_assert_cmphex(gl_window_pixel(qts, 8, 8), ==, 0xffffff);
    gl_width_clear(qts);
    gl_word(qts, GL_BEGIN_POINTS);
    gl_vertex2f(qts, 16.5f, 16.5f);
    gl_word(qts, GL_END_OF(GL_BEGIN_POINTS));
    g_assert_cmphex(gl_window_pixel(qts, 16, 16), ==, 0xffffff);
    gl_raster_pos2f(qts, 32.5f, 8.5f);
    gl_bitmap_packet(qts, 8, 1, 0, 0, 0, 0);
    const uint32_t image[] = { 0xff000000 };

    gl_bitmap_image(qts, image, ARRAY_SIZE(image));
    for (unsigned x = 32; x < 40; x++) {
        g_assert_cmphex(gl_window_pixel(qts, x, 8), ==, 0xffffff);
    }
    gl_clear_color(qts, 1, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(qts, 16, 16), ==, 0xff0000);
}

static void test_gl_native_stipple_loop(void)
{
    mgras_reset();
    gl_native_stipple_loop_setup(qts);
    gl_native_stipple_loop_check(qts);
}

static void test_gl_native_polygon_facing(void)
{
    for (unsigned cw = 0; cw < 2; cw++) {
        gl_transform_start(qts);
        gl_width_clear(qts);
        gl_polygon_mode(qts, GL_FRONT_FACE, GL_LINE_MODE);
        gl_polygon_mode(qts, GL_BACK_FACE, 0x1b00);
        gl_polygon_quad(qts, cw, true);
        g_assert_cmphex(gl_window_pixel(qts, 16, 8), ==,
                        cw ? 0 : 0xffffff);
        g_assert_cmphex(gl_window_pixel(qts, 8, 8), ==, 0xffffff);
        g_assert_cmphex(gl_window_pixel(qts, 16, 16), ==, 0);

        gl_width_clear(qts);
        gl_word(qts, GL_CULL_ON);
        gl_polygon_quad(qts, cw, true);
        g_assert_cmphex(gl_window_pixel(qts, 8, 8), ==,
                        cw ? 0 : 0xffffff);
        gl_width_clear(qts);
        gl_one(qts, GL_FRONT_FACE_HDR, GL_CW);
        gl_polygon_quad(qts, cw, true);
        g_assert_cmphex(gl_window_pixel(qts, 16, 8), ==,
                        cw ? 0xffffff : 0);
    }
}

/* The client plane generates a new boundary at x = 16.5. */
static void test_gl_native_polygon_clipped_boundary(void)
{
    gl_transform_start(qts);
    gl_width_clear(qts);
    gl_polygon_mode(qts, GL_FRONT_AND_BACK, GL_LINE_MODE);
    gl_clip_plane(qts, 0, 1, 0, 0, -16.5f);
    gl_one(qts, GL_CLIP_PLANE_ON_HDR, GL_CLIP_PLANE0);
    gl_polygon_quad(qts, false, true);
    for (unsigned y = 7; y <= 25; y++) {
        for (unsigned x = 7; x <= 25; x++) {
            bool boundary = x >= 16 && x <= 24 && y >= 8 && y <= 24 &&
                            (x == 16 || x == 24 || y == 8 || y == 24);

            g_assert_cmphex(gl_window_pixel(qts, x, y), ==,
                            boundary ? 0xffffff : 0);
        }
    }
}

/* Both LINE and FILL defer the complete polygon until End. */
static void test_gl_native_polygon_deferred(void)
{
    for (unsigned fill = 0; fill < 2; fill++) {
        gl_transform_start(qts);
        gl_width_clear(qts);
        gl_polygon_mode(qts, GL_FRONT_AND_BACK,
                         fill ? GL_FILL_MODE : GL_LINE_MODE);
        gl_polygon_open(qts, 0);
        gl_polygon_check_owned(qts, 1, 6);
        for (unsigned y = 7; y <= 25; y++) {
            for (unsigned x = 7; x <= 25; x++) {
                g_assert_cmphex(gl_window_pixel(qts, x, y), ==, 0);
            }
        }
        gl_word(qts, GL_END_OF(GL_BEGIN_POLYGON));
        gl_polygon_check_owned(qts, 0, 0);
        g_assert_cmphex(gl_window_pixel(qts, 16, 16), ==,
                        fill ? 0xffffff : 0);
        /* FILL excludes the bottom tie; LINE covers its diamonds. */
        g_assert_cmphex(gl_window_pixel(qts, 16, 8), ==,
                        fill ? 0 : 0xffffff);
        g_assert_cmphex(gl_window_pixel(qts, 16, 24), ==, 0xffffff);
        g_assert_cmphex(gl_window_pixel(qts, 8, 16), ==, 0xffffff);
        g_assert_cmphex(gl_window_pixel(qts, 23, 16), ==,
                        fill ? 0xffffff : 0);
    }
}

static void test_gl_native_polygon_contexts(void)
{
    mgras_reset();
    gl_polygon_context_setup(qts);
    gl_polygon_context_check(qts);
}

static void test_gl_native_polygon_disposal(void)
{
    mgras_reset();
    gl_polygon_context_setup(qts);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_polygon_check_owned(qts, 1, 6);
    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    gl_polygon_check_owned(qts, 0, 0);

    for (unsigned i = 0; i < 32; i++) {
        gl_polygon_slot(qts, 0x1000 + i);
        gl_polygon_open(qts, 0);
    }
    gl_polygon_check_owned(qts, 32, 192);
    gl_enter_slot(qts, 0x2000);
    gl_polygon_check_owned(qts, 31, 186);
    mgras_reset();
    gl_polygon_check_owned(qts, 0, 0);
    mgras_reset();
    gl_polygon_check_owned(qts, 0, 0);
    /* Reset preserves activation, including the default context. */
    gl_transform_start(qts);
    gl_width_clear(qts);
    gl_polygon_mode(qts, GL_FRONT_AND_BACK, GL_LINE_MODE);
    gl_polygon_quad(qts, false, true);
    g_assert_cmphex(gl_window_pixel(qts, 16, 8), ==, 0xffffff);
}

/*
 * A cleanup-only registration must not add a stream section. Corrupt only
 * the second context's named assembly subsection in synthetic storage;
 * the first context has already allocated when that name is refused.
 */
static void test_gl_native_polygon_failed_restore(void)
{
    static const char name[] = "sgi-gl-context/polygon-assembly";
    g_autofree char *dir = NULL;
    g_autofree char *path = NULL;
    g_autofree char *cmdline = NULL;
    g_autofree char *contents = NULL;
    g_autofree char *ret = NULL;
    GError *err = NULL;
    QTestState *s;
    char *first, *second;
    gsize length;
    int fd;

    if (!have_qemu_img()) {
        g_test_skip("QTEST_QEMU_IMG not set; cannot create snapshot storage");
        return;
    }
    dir = g_dir_make_tmp("sn0-mgras-polygon-snapshot-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_strdup_printf("%s/state.qcow2", dir);
    g_assert_true(mkimg(path, "qcow2", 128));
    cmdline = g_strdup_printf(MACHINE_ARGS
                              " -drive file=%s,format=qcow2,if=none", path);
    s = qtest_init(cmdline);
    gl_polygon_context_setup(s);
    ret = qtest_hmp(s, "savevm polygon");
    g_assert_cmpstr(ret, ==, "");
    g_clear_pointer(&ret, g_free);
    /* Successful snapshot cleanup preserves both deferred boundaries. */
    ret = qtest_hmp(s, "loadvm polygon");
    g_assert_cmpstr(ret, ==, "");
    g_clear_pointer(&ret, g_free);
    gl_polygon_check_owned(s, 2, 12);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(path, &contents, &length, &err));
    g_assert_no_error(err);
    g_assert_null(memmem(contents, length, "sgi-mgras/load-cleanup", 22));
    first = memmem(contents, length, name, sizeof(name) - 1);
    g_assert_nonnull(first);
    second = memmem(first + sizeof(name) - 1,
                    length - (first - contents) - (sizeof(name) - 1),
                    name, sizeof(name) - 1);
    g_assert_nonnull(second);
    /* Refuse a same-length unknown name, without guessing field offsets. */
    fd = open(path, O_RDWR);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pwrite(fd, "X", 1, second - contents), ==, 1);
    g_assert_cmpint(close(fd), ==, 0);

    s = qtest_init(cmdline);
    for (unsigned i = 0; i < 2; i++) {
        ret = qtest_hmp(s, "loadvm polygon");
        g_assert_nonnull(strstr(ret, "Error"));
        g_clear_pointer(&ret, g_free);
        gl_polygon_check_owned(s, 0, 0);
    }
    /* Teardown also disposes a live open polygon. */
    gl_polygon_slot(s, GL_SLOT_A);
    gl_polygon_open(s, 0);
    gl_polygon_check_owned(s, 1, 6);
    qtest_quit(s);
    g_assert_cmpint(unlink(path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/* Halfway sizes round up by the renderer's declared arithmetic choice. */
static void test_gl_native_width_points(void)
{
    static const struct {
        float size;
        int x0, y0, x1, y1;
    } cases[] = {
        { 1, 4, 3, 4, 3 },
        { 2, 3, 3, 4, 4 },
        { 5, 2, 1, 6, 5 },
        { 2.5f, 3, 2, 5, 4 },
    };

    for (int i = 0; i < G_N_ELEMENTS(cases); i++) {
        gl_transform_start(qts);
        gl_width_clear(qts);
        /* The first case exercises the initial size without a setter. */
        if (i) {
            gl_word(qts, GL_POINT_SIZE_HDR);
            gl_float(qts, cases[i].size);
        }
        gl_word(qts, GL_BEGIN_POINTS);
        gl_vertex2f(qts, 4.25f, 3.75f);
        gl_word(qts, GL_END_OF(GL_BEGIN_POINTS));
        gl_width_check_rect(qts, cases[i].x0, cases[i].y0,
                            cases[i].x1, cases[i].y1, 0xffffff);
    }
}

/*
 * Width expands on the minor axis. For odd widths, reversing excludes the
 * opposite endpoint. For an even horizontal width, the shifted y is on a
 * diamond boundary: subtracting (epsilon, epsilon squared) puts the endpoint
 * outside that diamond since epsilon - epsilon squared is positive. Reversal
 * therefore keeps the same columns, unlike the odd-width and vertical cases.
 */
static void test_gl_native_width_lines(void)
{
    static const struct {
        float width, ax, ay, bx, by;
        int x0, y0, x1, y1;
    } cases[] = {
        { 1, 1.5f, 3.5f, 5.5f, 3.5f, 1, 3, 4, 3 },
        { 2, 1.5f, 3.5f, 5.5f, 3.5f, 1, 2, 4, 3 },
        { 3, 1.5f, 3.5f, 5.5f, 3.5f, 1, 2, 4, 4 },
        { 2.5f, 1.5f, 3.5f, 5.5f, 3.5f, 1, 2, 4, 4 },
        { 2, 5.5f, 3.5f, 1.5f, 3.5f, 1, 2, 4, 3 },
        { 3, 5.5f, 3.5f, 1.5f, 3.5f, 2, 2, 5, 4 },
        { 2, 3.5f, 1.5f, 3.5f, 5.5f, 2, 1, 3, 4 },
        { 3, 3.5f, 1.5f, 3.5f, 5.5f, 2, 1, 4, 4 },
        { 2, 3.5f, 5.5f, 3.5f, 1.5f, 2, 2, 3, 5 },
    };

    for (int i = 0; i < G_N_ELEMENTS(cases); i++) {
        gl_transform_start(qts);
        gl_width_clear(qts);
        if (i) {
            gl_word(qts, GL_LINE_WIDTH_HDR);
            gl_float(qts, cases[i].width);
        }
        gl_word(qts, GL_BEGIN_LINES);
        gl_vertex2f(qts, cases[i].ax, cases[i].ay);
        gl_vertex2f(qts, cases[i].bx, cases[i].by);
        gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
        gl_width_check_rect(qts, cases[i].x0, cases[i].y0,
                            cases[i].x1, cases[i].y1, 0xffffff);
    }
}

/* Neither setter is permitted inside Begin/End; both retain their old value. */
static void test_gl_native_width_rejected_inside_begin(void)
{
    gl_transform_start(qts);
    gl_width_clear(qts);
    gl_word(qts, GL_POINT_SIZE_HDR);
    gl_float(qts, 2);
    gl_word(qts, GL_BEGIN_POINTS);
    gl_word(qts, GL_POINT_SIZE_HDR);
    gl_float(qts, 5);
    gl_vertex2f(qts, 4.25f, 3.75f);
    gl_word(qts, GL_END_OF(GL_BEGIN_POINTS));
    gl_width_check_rect(qts, 3, 3, 4, 4, 0xffffff);
    g_assert_cmphex(gl_query(qts, GL_Q_POINT_SIZE, 1), ==, gl_bits(2));

    gl_width_clear(qts);
    gl_word(qts, GL_LINE_WIDTH_HDR);
    gl_float(qts, 2);
    gl_word(qts, GL_BEGIN_LINES);
    gl_word(qts, GL_LINE_WIDTH_HDR);
    gl_float(qts, 5);
    gl_vertex2f(qts, 1.5f, 3.5f);
    gl_vertex2f(qts, 5.5f, 3.5f);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
    gl_width_check_rect(qts, 1, 2, 4, 3, 0xffffff);
    g_assert_cmphex(gl_query(qts, GL_Q_POINT_SIZE + 1, 1), ==, gl_bits(2));
}

/* Both contexts use the same window, so only retained slot state can differ. */
static void test_gl_native_width_contexts(void)
{
    mgras_reset();
    for (int i = 0; i < 3; i++) {
        bool b = i == 1;

        gl_enter_slot(qts, b ? GL_SLOT_B : GL_SLOT_A);
        if (i < 2) {
            gl_word(qts, GL_CONTEXT_OPENER);
            gl_viewport(qts, 0, 0, 128, 128);
            gl_window_projection(qts);
            gl_word(qts, GL_POINT_SIZE_HDR);
            gl_float(qts, b ? 5 : 2);
            gl_word(qts, GL_LINE_WIDTH_HDR);
            gl_float(qts, b ? 3 : 2);
        }
        gl_width_clear(qts);
        gl_word(qts, GL_BEGIN_POINTS);
        gl_vertex2f(qts, 4.25f, 3.75f);
        gl_word(qts, GL_END_OF(GL_BEGIN_POINTS));
        gl_width_check_rect(qts, b ? 2 : 3, b ? 1 : 3,
                            b ? 6 : 4, b ? 5 : 4, 0xffffff);
        gl_word(qts, GL_BEGIN_LINES);
        gl_vertex2f(qts, 1.5f, 13.5f);
        gl_vertex2f(qts, 5.5f, 13.5f);
        gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
        gl_width_check_rect(qts, 1, 12, 4, b ? 14 : 13, 0xffffff);
    }
}

/* Scissor removes individual expanded fragments, without changing coverage. */
static void test_gl_native_width_scissor(void)
{
    gl_transform_start(qts);
    gl_width_clear(qts);
    gl_scissor(qts, 3, 3, 2, 2);
    gl_one(qts, GL_SCISSOR_TEST_HDR, 1);
    gl_word(qts, GL_POINT_SIZE_HDR);
    gl_float(qts, 5);
    gl_word(qts, GL_BEGIN_POINTS);
    gl_vertex2f(qts, 4.25f, 3.75f);
    gl_word(qts, GL_END_OF(GL_BEGIN_POINTS));
    gl_width_check_rect(qts, 3, 3, 4, 4, 0xffffff);

    gl_one(qts, GL_SCISSOR_TEST_HDR, 0);
    gl_width_clear(qts);
    gl_scissor(qts, 2, 3, 2, 1);
    gl_one(qts, GL_SCISSOR_TEST_HDR, 1);
    gl_word(qts, GL_LINE_WIDTH_HDR);
    gl_float(qts, 3);
    gl_word(qts, GL_BEGIN_LINES);
    gl_vertex2f(qts, 1.5f, 3.5f);
    gl_vertex2f(qts, 5.5f, 3.5f);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
    gl_width_check_rect(qts, 2, 3, 3, 3, 0xffffff);
}

/* Existing RGB and CI8 paths, in each supported colour bank. */
static void test_gl_native_width_targets(void)
{
    for (int ci = 0; ci < 2; ci++) {
        for (int bank = 0; bank < 2; bank++) {
            uint32_t colour = ci ? 0x13 : 0x00ff00;

            gl_transform_start(qts);
            gl_width_clear(qts);
            if (ci) {
                gl_one(qts, GL_INIT_CI_HDR, 8);
                gl_indexs(qts, colour);
                mgras_write_cmap_entry(qts, 29, colour, 0x00ff0000);
            } else {
                gl_color3f(qts, 0, 1, 0);
            }
            gl_draw_buffer(qts, bank ? 2 : 1);
            gl_word(qts, GL_POINT_SIZE_HDR);
            gl_float(qts, 2);
            gl_word(qts, GL_BEGIN_POINTS);
            gl_vertex2f(qts, 4.25f, 3.75f);
            gl_word(qts, GL_END_OF(GL_BEGIN_POINTS));
            gl_word(qts, GL_LINE_WIDTH_HDR);
            gl_float(qts, 2);
            gl_word(qts, GL_BEGIN_LINES);
            gl_vertex2f(qts, 1.5f, 13.5f);
            gl_vertex2f(qts, 5.5f, 13.5f);
            gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
            if (!bank) {
                gl_width_check_rect(qts, 3, 3, 4, 4, colour);
                gl_width_check_rect(qts, 1, 12, 4, 13, colour);
                g_assert_cmphex(mgras_test_format(qts, 259, 380), ==,
                                ci ? MGRAS_PIXEL_FORMAT_CI8 :
                                MGRAS_PIXEL_FORMAT_RGB);
                g_assert_cmphex(mgras_test_format(qts, 257, 371), ==,
                                ci ? MGRAS_PIXEL_FORMAT_CI8 :
                                MGRAS_PIXEL_FORMAT_RGB);
            } else {
                /* Plane reads expose bank 0; scanout selects bank 1. */
                qtest_writel(qts, REG_VC3_INDEXDATA,
                             VC3_INDEXDATA(0x1e, 0x4));
                mgras_tag_wid(qts, 256, 256, 383, 383, 0);
                qtest_writeb(qts, REG_XMAP_INDEX, 0);
                qtest_writel(qts, REG_XMAP_BUF_SELECT, 1);
                for (int y = 0; y <= 15; y++) {
                    for (int x = 0; x <= 6; x++) {
                        bool point = x >= 3 && x <= 4 && y >= 3 && y <= 4;
                        bool line = x >= 1 && x <= 4 && y >= 12 && y <= 13;

                        g_assert_cmphex(gl_window_pixel(qts, x, y), ==, 0);
                        g_assert_cmphex(mgras_test_display_pixel(qts,
                                        256 + x, 383 - y), ==,
                                        point || line ? 0x00ff00 : 0);
                    }
                }
            }
        }
    }
}
int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init(MACHINE_ARGS);


    qtest_add_func("sn0/origin2000/mgras/"
                   "hq-query-answers-viewport-shade-and-clear-colour",
                   test_hq_query_answers_viewport_shade_and_clear_colour);
    qtest_add_func("sn0/origin2000/mgras/hq-query-answers-the-depth-range",
                   test_hq_query_answers_the_depth_range);
    qtest_add_func("sn0/origin2000/mgras/hq-query-answers-the-stack-depths",
                   test_hq_query_answers_the_stack_depths);
    qtest_add_func("sn0/origin2000/mgras/"
                   "hq-query-answers-lights-and-materials",
                   test_hq_query_answers_lights_and_materials);
    qtest_add_func("sn0/origin2000/mgras/inline-block-uses-20-bit-length",
                   test_inline_block_uses_20_bit_length);
    qtest_add_func("sn0/origin2000/mgras/gl-clear-fills-the-window",
                   test_gl_clear_fills_the_window);
    qtest_add_func("sn0/origin2000/mgras/gl-clear-ignores-the-viewport",
                   test_gl_clear_ignores_the_viewport);
    qtest_add_func("sn0/origin2000/mgras/gl-clear-needs-the-window-rectangle",
                   test_gl_clear_needs_the_window_rectangle);
    qtest_add_func("sn0/origin2000/mgras/gl-clip-takes-every-enabled-rectangle",
                   test_gl_clip_takes_every_enabled_rectangle);
    qtest_add_func("sn0/origin2000/mgras/gl-clip-ignores-disabled-rectangles",
                   test_gl_clip_ignores_disabled_rectangles);
    qtest_add_func("sn0/origin2000/mgras/gl-clip-null-and-many-draw-nothing",
                   test_gl_clip_null_and_many_draw_nothing);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-clear-shows-through-a-direct-colour-display-id",
                   test_gl_clear_shows_through_a_direct_colour_display_id);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-flat-triangle-matches-the-reference",
                   test_gl_flat_triangle_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/gl-flat-triangle-moved-vertex",
                   test_gl_flat_triangle_moved_vertex);
    qtest_add_func("sn0/origin2000/mgras/gl-flat-triangle-reversed-winding",
                   test_gl_flat_triangle_reversed_winding);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-flat-triangle-takes-the-third-vertex-colour",
                   test_gl_flat_triangle_takes_the_third_vertex_colour);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-triangle-snaps-to-the-nearest-subpixel",
                   test_gl_triangle_snaps_to_the_nearest_subpixel);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-triangle-shared-edge-centres-drawn-once",
                   test_gl_triangle_shared_edge_centres_drawn_once);
    qtest_add_func("sn0/origin2000/mgras/gl-triangle-bounded-by-the-viewport",
                   test_gl_triangle_bounded_by_the_viewport);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-triangle-behind-the-eye-is-clipped",
                   test_gl_triangle_behind_the_eye_is_clipped);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-depth-matches-the-reference",
                   test_gl_depth_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-depth-rounds-to-24-bits",
                   test_gl_depth_rounds_to_24_bits);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-depth-writes-only-when-tested-and-masked-in",
                   test_gl_depth_writes_only_when_tested_and_masked_in);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-depth-func-follows-the-enum",
                   test_gl_depth_func_follows_the_enum);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-clear-depth-sets-the-plane",
                   test_gl_clear_depth_sets_the_plane);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-triangle-clipped-at-the-far-plane",
                   test_gl_triangle_clipped_at_the_far_plane);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-smooth-matches-the-reference",
                   test_gl_smooth_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-smooth-rounds-to-the-nearest-step",
                   test_gl_smooth_rounds_to_the_nearest_step);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-smooth-is-perspective-correct",
                   test_gl_smooth_is_perspective_correct);
    qtest_add_func("sn0/origin2000/mgras/gl-deep-bit2-draws/ssi",
                   test_gl_deep_bit2_draws_ssi);
    qtest_add_func("sn0/origin2000/mgras/gl-deep-bit2-draws/si",
                   test_gl_deep_bit2_draws_si);
    qtest_add_func("sn0/origin2000/mgras/gl-color-bytes-unpack-red-first",
                   test_gl_color_bytes_unpack_red_first);
    qtest_add_func("sn0/origin2000/mgras/gl-vertex2i-matches-the-reference",
                   test_gl_vertex2i_matches_the_reference);
    qtest_add_func("sn0/origin2000/mgras/gl-rotate-about-a-general-axis",
                   test_gl_rotate_about_a_general_axis);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-quad-splits-from-its-first-vertex",
                   test_gl_quad_splits_from_its_first_vertex);
    qtest_add_func("sn0/origin2000/mgras/gl-strip-spans-a-context-switch",
                   test_gl_strip_spans_a_context_switch);
    qtest_add_func("sn0/origin2000/mgras/gl-line-leaves-out-its-last-pixel",
                   test_gl_line_leaves_out_its_last_pixel);
    qtest_add_func("sn0/origin2000/mgras/gl-line-bounded-by-the-viewport",
                   test_gl_line_bounded_by_the_viewport);
    qtest_add_func("sn0/origin2000/mgras/gl-line-depth-and-colour-follow-t",
                   test_gl_line_depth_and_colour_follow_t);
    for (size_t i = 0; i < ARRAY_SIZE(gl_prim_cases); i++) {
        g_autofree char *path = g_strdup_printf(
            "sn0/origin2000/mgras/gl-reference/%s", gl_prim_cases[i].name);

        qtest_add_data_func(path, &gl_prim_cases[i],
                            test_gl_prim_matches_the_reference);
    }
    qtest_add_func("sn0/origin2000/mgras/gl-triangle-spans-a-context-switch",
                   test_gl_triangle_spans_a_context_switch);
    qtest_add_func("sn0/origin2000/mgras/gl-lit-triangle-matches-the-equation",
                   test_gl_lit_triangle_matches_the_equation);
    qtest_add_func("sn0/origin2000/mgras/gl-light-enables-select-the-lights",
                   test_gl_light_enables_select_the_lights);
    qtest_add_func("sn0/origin2000/mgras/gl-normalize-enable",
                   test_gl_normalize_enable);
    qtest_add_func("sn0/origin2000/mgras/gl-normal-forms-convert",
                   test_gl_normal_forms_convert);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-two-sided-lighting-lights-the-back",
                   test_gl_two_sided_lighting_lights_the_back);
    qtest_add_func("sn0/origin2000/mgras/gl-cull-face-culls-by-facing",
                   test_gl_cull_face_culls_by_facing);
    qtest_add_func("sn0/origin2000/mgras/gl-scissor-bounds-draws-and-clears",
                   test_gl_scissor_bounds_draws_and_clears);
    qtest_add_func("sn0/origin2000/mgras/gl-alpha-test-discards",
                   test_gl_alpha_test_discards);
    qtest_add_func("sn0/origin2000/mgras/gl-stencil-masks-drawing",
                   test_gl_stencil_masks_drawing);
    qtest_add_func("sn0/origin2000/mgras/gl-stencil-incr-and-decr-codes",
                   test_gl_stencil_incr_and_decr_codes);
    qtest_add_func("sn0/origin2000/mgras/gl-stencil-clear-keeps-depth",
                   test_gl_stencil_clear_keeps_depth);
    qtest_add_func("sn0/origin2000/mgras/gl-blend-mixes-with-the-window",
                   test_gl_blend_mixes_with_the_window);
    qtest_add_func("sn0/origin2000/mgras/gl-logic-op-combines-with-the-window",
                   test_gl_logic_op_combines_with_the_window);
    qtest_add_func("sn0/origin2000/mgras/gl-colour-mask-keeps-channels",
                   test_gl_colour_mask_keeps_channels);
    qtest_add_func("sn0/origin2000/mgras/gl-fog-index-integer-endpoints",
                   test_gl_fog_index_integer_endpoints);
    qtest_add_func("sn0/origin2000/mgras/gl-fog-blends-toward-the-fog-colour",
                   test_gl_fog_blends_toward_the_fog_colour);
    qtest_add_func("sn0/origin2000/mgras/gl-polygon-offset-moves-depth",
                   test_gl_polygon_offset_moves_depth);
    qtest_add_func("sn0/origin2000/mgras/gl-clip-plane-clips-in-eye-space",
                   test_gl_clip_plane_clips_in_eye_space);
    qtest_add_func("sn0/origin2000/mgras/gl-line-point-state-answers",
                   test_gl_line_point_state_answers);

    qtest_add_func("sn0/origin2000/mgras/gl-native-stipple/parameters",
                    test_gl_native_stipple_parameters);
    qtest_add_func("sn0/origin2000/mgras/gl-native-stipple/polygon",
                    test_gl_native_stipple_polygon);
    qtest_add_func("sn0/origin2000/mgras/gl-native-stipple/no-write",
                    test_gl_native_stipple_no_write);
    qtest_add_func("sn0/origin2000/mgras/gl-native-stipple/unbound",
                    test_gl_native_stipple_unbound);
    qtest_add_func("sn0/origin2000/mgras/gl-native-stipple/axes",
                    test_gl_native_stipple_axes);
    qtest_add_func("sn0/origin2000/mgras/gl-native-stipple/pairs",
                    test_gl_native_stipple_pairs);
    qtest_add_func("sn0/origin2000/mgras/gl-native-stipple/masks",
                    test_gl_native_stipple_masks);
    qtest_add_func("sn0/origin2000/mgras/gl-native-stipple/contexts",
                    test_gl_native_stipple_contexts);
    qtest_add_func("sn0/origin2000/mgras/gl-native-stipple/loop",
                    test_gl_native_stipple_loop);
    qtest_add_func("sn0/origin2000/mgras/gl-native-stipple/empty-binding",
                    test_gl_native_stipple_empty_binding);
    qtest_add_func("sn0/origin2000/mgras/gl-native-polygon/stipple-applied",
                    test_gl_native_polygon_stipple_applied);
    qtest_add_func("sn0/origin2000/mgras/gl-native-polygon/facing",
                    test_gl_native_polygon_facing);
    qtest_add_func("sn0/origin2000/mgras/gl-native-polygon/clipped-boundary",
                    test_gl_native_polygon_clipped_boundary);
    qtest_add_func("sn0/origin2000/mgras/gl-native-polygon/deferred",
                    test_gl_native_polygon_deferred);
    qtest_add_func("sn0/origin2000/mgras/gl-native-polygon/contexts",
                    test_gl_native_polygon_contexts);
    qtest_add_func("sn0/origin2000/mgras/gl-native-polygon/disposal",
                    test_gl_native_polygon_disposal);
    qtest_add_func("sn0/origin2000/mgras/gl-native-polygon/failed-restore",
                    test_gl_native_polygon_failed_restore);
    qtest_add_func("sn0/origin2000/mgras/gl-native-polygon/edge-flags",
                   test_gl_native_polygon_edge_flags);
    qtest_add_func("sn0/origin2000/mgras/gl-native-polygon/point",
                   test_gl_native_polygon_point);
    qtest_add_func("sn0/origin2000/mgras/gl-native-polygon/line",
                   test_gl_native_polygon_line);
    qtest_add_func("sn0/origin2000/mgras/gl-native-width/points",
                   test_gl_native_width_points);
    qtest_add_func("sn0/origin2000/mgras/gl-native-width/lines",
                   test_gl_native_width_lines);
    qtest_add_func("sn0/origin2000/mgras/gl-native-width/rejected-inside-begin",
                   test_gl_native_width_rejected_inside_begin);
    qtest_add_func("sn0/origin2000/mgras/gl-native-width/contexts",
                   test_gl_native_width_contexts);
    qtest_add_func("sn0/origin2000/mgras/gl-native-width/scissor",
                   test_gl_native_width_scissor);
    qtest_add_func("sn0/origin2000/mgras/gl-native-width/targets",
                   test_gl_native_width_targets);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
