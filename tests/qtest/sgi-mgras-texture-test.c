/*
 * MGRAS register-contract and protocol qtests: texture cases.
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
#include "sgi-raster-bands.h"

static QTestState *tex_sb_file_migrate_and_restart(QTestState *from,
                                                    const char *args,
                                                    const char *uri);


/*
 * The class-0x3800 forms and glTexCoord2f, as the IRIX 6.5.30 IP27GR532
 * client sends them: glVertex3i (0x3800000c) draws where glVertex3f of
 * the same values would, glRasterPos3i (0x3800380c) sets the raster
 * position with its z, window depth 0 for z 1 under glOrtho's -1 to 1,
 * and glTexCoord2f (0x40000808), glTexCoord3i (0x3800080c) and
 * glTexCoord4f (0x00000810) between the vertices are consumed by their
 * length and change nothing drawn.
 */
#define GL_VERTEX3I_HDR       0x3800000c
#define GL_RASTER_POS3I_HDR   0x3800380c
#define TEX_GL_TEX_COORD3F_HDR 0x2000080c
#define TEX_GL_BEGIN_POINTS    0x00001600

static void gl_vertex3i(QTestState *qts, int32_t x, int32_t y, int32_t z)
{
    gl_word(qts, GL_VERTEX3I_HDR);
    gl_word(qts, x);
    gl_word(qts, y);
    gl_word(qts, z);
}

static void test_gl_integer_and_texture_forms(void)
{
    QTestState *test = qtest_init(MACHINE_ARGS);

    qts = test;
    gl_transform_start(qts);
    gl_clear_color(qts, 0, 0, 0, 1);
    gl_word(qts, GL_CLEAR_COLOR_BUFFER);
    gl_color3f(qts, 1, 1, 1);
    gl_word(qts, GL_BEGIN_TRIANGLES);
    gl_word(qts, GL_TEX_COORD2F_HDR);
    gl_word(qts, gl_bits(0.25f));
    gl_word(qts, gl_bits(0.75f));
    gl_vertex3i(qts, 10, 10, 0);
    gl_word(qts, GL_TEX_COORD3I_HDR);
    gl_word(qts, 1);
    gl_word(qts, 2);
    gl_word(qts, 3);
    gl_vertex3i(qts, 40, 10, 0);
    gl_word(qts, GL_TEX_COORD4F_HDR);
    for (int i = 0; i < 4; i++) {
        gl_word(qts, gl_bits(0.5f));
    }
    gl_vertex3i(qts, 10, 40, 0);
    gl_word(qts, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(qts, 15, 15), ==, 0xffffff);
    g_assert_cmphex(gl_window_pixel(qts, 38, 38), ==, 0);
    g_assert_cmphex(gl_window_pixel(qts, 9, 15), ==, 0);

    gl_word(qts, GL_RASTER_POS3I_HDR);
    gl_word(qts, 20);
    gl_word(qts, 30);
    gl_word(qts, 1);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_VALID, 1), ==, UINT32_MAX);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_X, 1), ==,
                    gl_bits(20 + GL_RASTER_BIAS));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_Y, 1), ==,
                    gl_bits(30 + GL_RASTER_BIAS));
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_Z, 1), ==, gl_bits(0));
    g_assert_cmphex(mgras_qom_get_uint(qts, "x-test-error"), ==, 0);

    qtest_quit(test);
    qts = NULL;
}

static void test_gl_native_texcoord_current(void)
{
    static const float initial[4] = { 0, 0, 0, 1 };
    static const float four[4] = { 0.25f, -0.5f, 2, 0.5f };
    static const float two[4] = { -0.25f, 0.75f, 0, 1 };
    static const float integer[4] = { -3, 5, 7, 1 };
    QTestState *test = qtest_init(MACHINE_ARGS);

    qts = test;

    gl_transform_start(qts);
    gl_texcoord_check(qts, false, initial);
    gl_texcoord_check(qts, true, initial);
    gl_texcoord4f(qts, four[0], four[1], four[2], four[3]);
    gl_texcoord_check(qts, false, four);
    gl_word(qts, GL_TEX_COORD2F_HDR);
    gl_float(qts, two[0]);
    gl_float(qts, two[1]);
    gl_texcoord_check(qts, false, two);
    gl_word(qts, GL_TEX_COORD3I_HDR);
    gl_word(qts, -3);
    gl_word(qts, 5);
    gl_word(qts, 7);
    gl_texcoord_check(qts, false, integer);
    gl_word(qts, GL_BEGIN_LINES);
    gl_texcoord4f(qts, four[0], four[1], four[2], four[3]);
    gl_texcoord_check(qts, false, four);
    gl_word(qts, GL_END_OF(GL_BEGIN_LINES));
    gl_texcoord_check(qts, true, initial);

    qtest_quit(test);
    qts = NULL;
}

/* T * S preserves homogeneous q: (0.25, 0.5, -1, 2) maps as below. */
static void test_gl_native_texcoord_raster(void)
{
    static const float current[4] = { 0.25f, 0.5f, -1, 2 };
    static const float transformed[4] = { 6.5f, -2, 7.5f, 2 };
    static const float changed[4] = { -1, -2, 0, 1 };
    QTestState *test = qtest_init(MACHINE_ARGS);

    qts = test;

    gl_transform_start(qts);
    gl_texcoord4f(qts, current[0], current[1], current[2], current[3]);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_TEXTURE);
    gl_three(qts, GL_TRANSLATE_HDR, 3, -2, 4);
    gl_three(qts, GL_SCALE_HDR, 2, 4, 0.5f);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_raster_pos2f(qts, 16, 16);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_VALID, 1), ==, UINT32_MAX);
    gl_texcoord_check(qts, false, current);
    gl_texcoord_check(qts, true, transformed);
    gl_word(qts, GL_TEX_COORD2F_HDR);
    gl_float(qts, changed[0]);
    gl_float(qts, changed[1]);
    gl_texcoord_check(qts, false, changed);
    gl_texcoord_check(qts, true, transformed);
    gl_raster_pos2f(qts, 512, 16);
    g_assert_cmphex(gl_query(qts, GL_Q_RASTER_VALID, 1), ==, 0);
    gl_texcoord_check(qts, true, transformed);

    qtest_quit(test);
    qts = NULL;
}

static void test_gl_native_texcoord_contexts(void)
{
    QTestState *test = qtest_init(MACHINE_ARGS);

    qts = test;
    mgras_reset_state(test);
    gl_texcoord_context_setup(qts);
    gl_texcoord_context_check(qts);

    qtest_quit(test);
    qts = NULL;
}

/*
 * One-engine texture on SI+T (hw/misc/sgi-mgras-texture.c), in the words
 * the IRIX 6.5.30 IP27GR532 client sent when ivview loaded and drew one
 * textured quad. Raster register records
 * are 0x7c08, the record and word counts, and an inline block of
 * (register, value, shadow) triples padded to an even count with a stale
 * word. A load is TL_MODE and TL_SPEC, 0x9b00, the four-byte pixel
 * program with the destination 0, 0, w, h and size word h << 16 | w,
 * XFRMODE 0xc00080, 0x8d20 naming routine 0x511a with flags 0x98, the
 * DMA transfer kicked 0xa7, then 0x9c00. The bind is TEXMODE2, 0x0b00,
 * TEXMODE1 0x40859, 0x5f04, the border colour, TXADDR and TXMIPMAP by
 * 0xec10, and TXSIZE, TXLOD and TXCLAMPSIZE.
 */
#define TEX_ARGS MACHINE_ARGS ",product=si-t"
#define TEX_WHITE             0xffffff

static QTestState *texture_board_start(const char *board)
{
    g_autofree char *args = g_strdup_printf(
        MACHINE_ARGS ",product=%s", board);

    return tex_start(args);
}

/*
 * An RGBA8 level loaded and drawn nearest with modulate by white: each
 * texel's centre pixel takes the texel's own bytes, the first row sent
 * at t = 0, the bottom, and the quad's edges keep to its pixels.
 */
static void test_gl_texture_load_draws_the_level(gconstpointer data)
{
    QTestState *t = texture_board_start(data);
    uint8_t texels[32];

    tex_texels(texels);
    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 4; x++) {
            const uint8_t *p = &texels[4 * (4 * y + x)];

            g_assert_cmphex(tex_texel_pixel(t, x, y), ==,
                            p[0] << 16 | p[1] << 8 | p[2]);
        }
    }
    g_assert_cmphex(gl_window_pixel(t, 16, 16), ==, 0x03e080);
    g_assert_cmphex(gl_window_pixel(t, 47, 31), ==, 0x730087);
    g_assert_cmphex(gl_window_pixel(t, 15, 16), ==, 0);
    g_assert_cmphex(gl_window_pixel(t, 48, 31), ==, 0);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

#define GL_ONE_MINUS_SRC_ALPHA 0x303

/*
 * A textured quad blended at half alpha, writing a stencil value, then an
 * untextured blended quad drawn only where that stencil was written; the
 * window's 128 rows read back from the bank. Each quad covers fewer pixels
 * than the rasterizer's own band threshold, so only
 * x-test-raster-band-pixels puts it on band workers. Free the result.
 */
static uint32_t *tex_band_scene(QTestState *t, int *workers)
{
    static const float half[4] = { 1, 1, 1, 0.5 };
    static const float green[4] = { 0, 1, 0, 0.5 };
    static const float under[4][4] = {
        { 10, 10, 0, 0 }, { 48, 10, 1, 0 }, { 48, 48, 1, 1 },
        { 10, 48, 0, 1 },
    };
    static const float over[4][4] = {
        { 30, 20, 0, 0 }, { 60, 30, 0, 0 }, { 50, 60, 0, 0 },
        { 25, 50, 0, 0 },
    };
    uint32_t *window = g_new(uint32_t, 128 * 128);
    uint8_t texels[32];

    tex_texels(texels);
    gl_one(t, GL_STENCIL_TEST_HDR, 1);
    gl_stencil(t, GL_STENCIL_ALWAYS, 1, 0xff, GL_OP_KEEP, GL_OP_KEEP,
               GL_OP_REPLACE);
    gl_blend(t, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD);
    gl_one(t, GL_BLEND_HDR, 1);
    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad_rect(t, half, under);
    gl_stencil(t, GL_STENCIL_EQUAL, 1, 0xff, GL_OP_KEEP, GL_OP_KEEP,
               GL_OP_INCR);
    tex_quad_rect(t, green, over);
    for (int y = 0; y < 128; y++) {
        g_autofree uint32_t *row = mgras_test_pixel_row(t, 256, 256 + y, 128);

        memcpy(&window[y * 128], row, 128 * sizeof(uint32_t));
    }
    *workers = sgi_raster_band_workers(t);
    return window;
}

/*
 * The scene drawn with every draw in bands (x-test-raster-band-pixels=1)
 * leaves the same window as drawn on one thread (x-raster-bands=off),
 * through the texture readers and the blend and stencil callbacks; the
 * textured quad and the stencilled green show, and band workers drew.
 */
static void test_gl_bands_match_one_thread(gconstpointer data)
{
    g_autofree char *banded_args = g_strdup_printf(
        MACHINE_ARGS ",product=%s,x-test-raster-band-pixels=1", (char *)data);
    g_autofree char *serial_args = g_strdup_printf(
        MACHINE_ARGS ",product=%s,x-raster-bands=off", (char *)data);
    QTestState *t = tex_start(banded_args);
    g_autofree uint32_t *banded = NULL, *serial = NULL;
    int workers, serial_workers;

    banded = tex_band_scene(t, &workers);
    qtest_quit(t);
    t = tex_start(serial_args);
    serial = tex_band_scene(t, &serial_workers);
    qtest_quit(t);
    for (int i = 0; i < 128 * 128; i++) {
        g_assert_cmphex(banded[i], ==, serial[i]);
    }
    /*
     * Rows read top down: two texels of the under quad differ, the green
     * shows only over it, and the window outside both stays black.
     */
    g_assert_cmphex(serial[(127 - 15) * 128 + 15], !=,
                    serial[(127 - 15) * 128 + 43]);
    g_assert_cmphex(serial[(127 - 40) * 128 + 40], !=,
                    serial[(127 - 15) * 128 + 43]);
    g_assert_cmphex(serial[(127 - 58) * 128 + 50], ==, 0);
    g_assert_cmphex(serial[(127 - 100) * 128 + 100], ==, 0);
    if (workers < 0) {
        g_test_skip("needs Linux /proc and two host processors");
        return;
    }
    g_assert_cmpint(workers, >, 0);
    g_assert_cmpint(serial_workers, ==, 0);
}

/* The client's glTexGen and generic enable words (sgi-mgras-gl.c). */
#define TEX_GEN_MODE_HDR         0x00005a0c
#define TEX_GEN_PLANE_HDR        0x00005b08
#define TEX_GEN_PLANE_F_HDR      0x00005c10
#define TEX_GEN_ENABLE_HDR       0x00007504
#define TEX_GEN_DISABLE_HDR      0x00007604

static void tex_gen_object_plane(QTestState *t, uint32_t coord,
                                 const float p[4])
{
    gl_word(t, TEX_GEN_MODE_HDR);
    gl_word(t, coord);
    gl_word(t, 0x2500);
    gl_word(t, 0x2401);
    gl_word(t, TEX_GEN_PLANE_HDR);
    gl_word(t, coord);
    gl_word(t, 0x2501);
    gl_word(t, TEX_GEN_PLANE_F_HDR);
    for (int i = 0; i < 4; i++) {
        gl_float(t, p[i]);
    }
}

/*
 * glTexGen's words generate S and T from the object position: planes
 * mapping the quad's corners to 0 and 1 draw every texel where tex_quad()'s
 * explicit coordinates put it, though each vertex's own coordinates are 0;
 * the client's disable of GL_TEXTURE_GEN_S and T then gives texel 0, 0
 * everywhere.
 */
static void test_gl_texture_generation(gconstpointer data)
{
    static const float s_plane[4] = { 1.0f / 32, 0, 0, -0.5f };
    static const float t_plane[4] = { 0, 1.0f / 16, 0, -1 };
    static const float white[4] = { 1, 1, 1, 1 };
    static const float v[4][4] = {
        { 16, 16, 0, 0 }, { 48, 16, 0, 0 }, { 48, 32, 0, 0 }, { 16, 32, 0, 0 },
    };
    QTestState *t = texture_board_start(data);
    uint8_t texels[32];

    tex_texels(texels);
    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    tex_gen_object_plane(t, 0x2000, s_plane);
    tex_gen_object_plane(t, 0x2001, t_plane);
    gl_one(t, TEX_GEN_ENABLE_HDR, 0x0c60);
    gl_one(t, TEX_GEN_ENABLE_HDR, 0x0c61);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad_rect(t, white, v);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 4; x++) {
            const uint8_t *p = &texels[4 * (4 * y + x)];

            g_assert_cmphex(tex_texel_pixel(t, x, y), ==,
                            p[0] << 16 | p[1] << 8 | p[2]);
        }
    }

    gl_one(t, TEX_GEN_DISABLE_HDR, 0x0c60);
    gl_one(t, TEX_GEN_DISABLE_HDR, 0x0c61);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad_rect(t, white, v);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 4; x++) {
            g_assert_cmphex(tex_texel_pixel(t, x, y), ==,
                            texels[0] << 16 | texels[1] << 8 | texels[2]);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

#define TEX_COLOR_TABLE_SELECTOR 0x15c
#define TEX_COLOR_TABLE_DATA     0x15d
#define TEX_COLOR_TABLE_MODE     0x0000e000
#define TEX_COLOR_TABLE_ENTRIES  256

static void tex_color_table_upload(QTestState *t, uint32_t count)
{
    uint32_t *pairs = g_new(uint32_t, 2 * (count + 1));

    pairs[0] = TEX_COLOR_TABLE_SELECTOR;
    pairs[1] = 0x90000000;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t value;

        if (i < TEX_COLOR_TABLE_ENTRIES) {
            value = i ^ 0x3c;
        } else if (i < 2 * TEX_COLOR_TABLE_ENTRIES) {
            value = (i - TEX_COLOR_TABLE_ENTRIES + 0x57) & 0xff;
        } else {
            value = (i - 2 * TEX_COLOR_TABLE_ENTRIES) ^ 0xa5;
        }
        pairs[2 * (i + 1)] = TEX_COLOR_TABLE_DATA;
        pairs[2 * (i + 1) + 1] = value;
    }
    tex_records(t, pairs, count + 1);
    g_free(pairs);
}

static void tex_color_table_mode(QTestState *t, bool enabled)
{
    tex_record(t, TEX_TEXMODE1, TEX_TEXMODE1_ON |
               (enabled ? TEX_COLOR_TABLE_MODE : 0));
}

static void test_gl_texture_color_table(gconstpointer data)
{
    QTestState *t = texture_board_start(data);
    static const uint8_t texels[32] = {
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
    };

    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_record(t, TEX_COLOR_TABLE_DATA, 0x33);
    tex_color_table_mode(t, true);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, 0x4080c0);

    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_color_table_upload(t, 3 * TEX_COLOR_TABLE_ENTRIES - 1);
    tex_color_table_mode(t, true);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, 0x4080c0);

    tex_color_table_upload(t, 3 * TEX_COLOR_TABLE_ENTRIES);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, 0x4080c0);

    tex_record(t, TEX_COLOR_TABLE_DATA, 0xaa);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_color_table_mode(t, true);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, 0x4080c0);

    tex_color_table_upload(t, 3 * TEX_COLOR_TABLE_ENTRIES);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | 0x180);
    tex_color_table_mode(t, true);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, 0x7cd765);

    gl_clear_color(t, 0, 0, 0, 1);
    gl_word(t, GL_CLEAR_COLOR_BUFFER);
    gl_alpha_func(t, GL_GREATER, 0x400);
    gl_one(t, GL_ALPHA_TEST_HDR, 1);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_color_table_mode(t, true);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, 0x7cd765);
    gl_one(t, GL_ALPHA_TEST_HDR, 0);

    qtest_system_reset(t);
    gl_enter_slot(t, GL_SLOT_A);
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_color_table_mode(t, true);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, 0x4080c0);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static uint8_t tex_color_table_byte(uint32_t i)
{
    if (i < TEX_COLOR_TABLE_ENTRIES) {
        return i ^ 0x3c;
    }
    if (i < 2 * TEX_COLOR_TABLE_ENTRIES) {
        return (i - TEX_COLOR_TABLE_ENTRIES + 0x57) & 0xff;
    }
    return (i - 2 * TEX_COLOR_TABLE_ENTRIES) ^ 0xa5;
}

static void tex_color_table_continue(QTestState *t, uint32_t offset,
                                     uint32_t count)
{
    uint32_t *pairs = g_new(uint32_t, 2 * count);

    for (uint32_t i = 0; i < count; i++) {
        pairs[2 * i] = TEX_COLOR_TABLE_DATA;
        pairs[2 * i + 1] = tex_color_table_byte(offset + i);
    }
    tex_records(t, pairs, count);
    g_free(pairs);
}

#define TEX_COLOR_TABLE_MIGRATION_ARGS \
    MIGRATION_ARGS ",product=%s"

static void test_gl_texture_color_table_migration(gconstpointer data)
{
    static const uint8_t texels[32] = {
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *path = NULL;
    g_autofree char *source_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-color-table-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(tmpfs, "state", NULL);
    uri = g_strdup_printf("file:%s", path);
    source_args = g_strdup_printf(TEX_COLOR_TABLE_MIGRATION_ARGS,
                                  (char *)data);
    from = tex_start(source_args);

    tex_load(from, TEX_TL_MODE_RGBA8, texels);
    tex_bind(from, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_color_table_upload(from, 384);
    tex_color_table_mode(from, true);
    tex_quad(from);
    g_assert_cmphex(tex_texel_pixel(from, 0, 0), ==, 0x4080c0);
    to = tex_sb_file_migrate_and_restart(from,
                                            source_args, uri);

    tex_color_table_continue(to, 384, 3 * TEX_COLOR_TABLE_ENTRIES - 384);
    tex_bind(to, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_color_table_mode(to, true);
    tex_quad(to);
    g_assert_cmphex(tex_texel_pixel(to, 0, 0), ==, 0x7cd765);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    unlink(path);
    g_rmdir(tmpfs);
}

static void test_gl_texture_color_table_full_migration(gconstpointer data)
{
    static const uint8_t texels[32] = {
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *path = NULL;
    g_autofree char *source_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-color-table-full-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(tmpfs, "state", NULL);
    uri = g_strdup_printf("file:%s", path);
    source_args = g_strdup_printf(TEX_COLOR_TABLE_MIGRATION_ARGS,
                                  (char *)data);
    from = tex_start(source_args);

    tex_load(from, TEX_TL_MODE_RGBA8, texels);
    tex_bind(from, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_color_table_upload(from, 3 * TEX_COLOR_TABLE_ENTRIES);
    tex_color_table_mode(from, true);
    to = tex_sb_file_migrate_and_restart(from,
                                            source_args, uri);

    tex_quad(to);
    g_assert_cmphex(tex_texel_pixel(to, 0, 0), ==, 0x7cd765);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    unlink(path);
    g_rmdir(tmpfs);
}

static void test_gl_texture_color_table_reset_partial(gconstpointer data)
{
    static const uint8_t texels[32] = {
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
        0x40, 0x80, 0xc0, 0x80, 0x40, 0x80, 0xc0, 0x80,
    };
    QTestState *t = texture_board_start(data);

    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_color_table_upload(t, 384);
    qtest_system_reset(t);
    gl_enter_slot(t, GL_SLOT_A);
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
    tex_color_table_continue(t, 384, 3 * TEX_COLOR_TABLE_ENTRIES - 384);
    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_color_table_mode(t, true);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, 0x4080c0);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

#define TEX_SPEC_2X2             0x110
#define TEX_TXSIZE_2X2           0x1111
#define TEX_TEXMODE2_VOLUME      0x800000
#define TEX_MDDMA_PIXEL_ON       0x3c040045
#define TEX_MDDMA_PIXEL_OFF      0x3c040044

/*
 * The board's RE count, as the kernel reads it: the GE count in bits 25:24
 * of 0x072488, which products 2 and 3 give the RE count too.
 */
static uint32_t tex_re_count(QTestState *t)
{
    return (qtest_readl(t, MGRAS_REG(0x072488)) >> 24) & 3;
}

/*
 * One pass of the client's pixel-texture draw of a 2-pixel-wide image,
 * rows image rows, on a board with res REs: the GE box the client loads
 * (corners 0,0 to 1, the pass's rows times res; its rows and width; 1;
 * +1, +1; the pass), the draw, and a DMA of the source rows the pass
 * carries: pass, pass + res, and so on.
 */
static uint32_t tex_pixel_texture_pass_rows(uint32_t res, uint32_t pass,
                                            uint32_t rows)
{
    return (rows + res - pass - 1) / res;
}

/* The pass's GE box and its 0x8d20: the draw then waits for its image. */
static void tex_pixel_texture_pass_start(QTestState *t, uint32_t res,
                                         uint32_t pass, uint32_t rows,
                                         uint32_t enabled_control)
{
    uint32_t pass_rows = tex_pixel_texture_pass_rows(res, pass, rows);
    uint32_t box[9] = {
        0, 0, 1, pass_rows * res, pass_rows << 16 | 2, 1, 1, 1, pass,
    };

    tex_record(t, 0x18a, enabled_control);
    gl_pixels_program(t, GL_PIX_4UB, 2, 0, 0, 1, 1, 1);
    gl_ge_load(t, GL_GE_ERAM, GL_GE_BOX, box, ARRAY_SIZE(box));
    gl_raster_pos2f(t, 0, 0);
    gl_word(t, GL_PIXELS_HDR);
    gl_word(t, 2);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 2);
    gl_word(t, 0);
    gl_word(t, 1);
    gl_word(t, 0x4fc5);
    gl_word(t, 0x98);
}

/* The DMA of the source rows the pass carries, and the closing record. */
static void tex_pixel_texture_pass_finish(QTestState *t, uint32_t res,
                                          uint32_t pass, const uint8_t *pixels,
                                          uint32_t rows,
                                          uint32_t disabled_control)
{
    uint32_t pass_rows = tex_pixel_texture_pass_rows(res, pass, rows);
    uint8_t sent[8 * 4];

    g_assert_cmpuint(pass_rows, <=, 4);
    for (uint32_t i = 0; i < pass_rows; i++) {
        memcpy(&sent[8 * i], &pixels[8 * (pass + res * i)], 8);
    }
    gl_pixels_dma_on(t, 0, sent, 8 * pass_rows, 8, pass_rows, 8, 0xa7);
    tex_record(t, 0x18a, disabled_control);
    gl_pixels_fence(t);
}

static void tex_pixel_texture_pass(QTestState *t, uint32_t res, uint32_t pass,
                                   const uint8_t *pixels, uint32_t rows,
                                   uint32_t enabled_control,
                                   uint32_t disabled_control)
{
    tex_pixel_texture_pass_start(t, res, pass, rows, enabled_control);
    tex_pixel_texture_pass_finish(t, res, pass, pixels, rows,
                                  disabled_control);
}

/*
 * A 2x2 draw with pixel texgen on or off. captured_size 0x00020002 sends
 * it as the client's pixel-texture draw, one pass per RE; any other
 * nonzero size is one draw with that size word in the captured one-RE box;
 * 0 leaves the ordinary rectangle gl_pixels_program() loads.
 */
static void tex_volume_pixels_data(QTestState *t, bool enabled,
                                   uint32_t captured_size,
                                   const uint8_t pixels[16],
                                   uint32_t enabled_control,
                                   uint32_t disabled_control)
{
    if (captured_size == 0x00020002) {
        uint32_t res = tex_re_count(t);

        for (uint32_t pass = 0; pass < res; pass++) {
            tex_pixel_texture_pass(t, res, pass, pixels, 2,
                                   enabled ? enabled_control :
                                   disabled_control, disabled_control);
        }
        return;
    }
    tex_record(t, 0x18a, enabled ? enabled_control : disabled_control);
    gl_pixels_program(t, GL_PIX_4UB, 2, 0, 0, 1, 1, 1);
    if (captured_size) {
        uint32_t box[9] = {
            0, 0, 1, 2, captured_size, 1, 1, 1, 0,
        };

        gl_ge_load(t, GL_GE_ERAM, GL_GE_BOX, box, ARRAY_SIZE(box));
    }
    gl_raster_pos2f(t, 0, 0);
    gl_word(t, GL_PIXELS_HDR);
    gl_word(t, 2);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 2);
    gl_word(t, 0);
    gl_word(t, 1);
    gl_word(t, 0x4fc5);
    gl_word(t, 0x98);
    gl_pixels_dma_on(t, 0, pixels, 16, 8, 2, 8, 0xa7);
    tex_record(t, 0x18a, disabled_control);
    gl_pixels_fence(t);
}

static void tex_volume_pixels(QTestState *t, bool enabled,
                              uint32_t captured_size)
{
    static const uint8_t pixels[16] = {
        0x40, 0x40, 0x00, 0xff, 0xbf, 0x40, 0x40, 0xff,
        0x40, 0xbf, 0xbf, 0xff, 0xbf, 0xbf, 0xff, 0xff,
    };

    tex_volume_pixels_data(t, enabled, captured_size, pixels,
                           TEX_MDDMA_PIXEL_ON, TEX_MDDMA_PIXEL_OFF);
}

static uint32_t tex_captured_volume_size(uint32_t depth)
{
    return 2 * 2 * depth * 4;
}

static void tex_begin_captured_volume(QTestState *t, uint32_t depth)
{
    uint32_t bytes = tex_captured_volume_size(depth);
    /* Stock depth-two/four packets use 2,2 for this 2x2 rectangle. */
    uint32_t edge = depth == 1 ? 1 : 2;
    uint32_t box[6] = { 0, 0, edge, edge, 2 << 16 | 2, depth };
    uint32_t flags = depth == 1 ? 0x98 : 0x198;

    g_assert_cmpuint(depth, >=, 1);
    g_assert_true(depth == 1 || depth == 2 || depth == 4);
    tex_record(t, TEX_TL_MODE, TEX_TL_MODE_RGBA8);
    tex_record(t, TEX_TL_SPEC, 0x120);
    gl_word(t, TEX_SAVE_RSS);
    gl_word(t, 0xffffffff);
    gl_word(t, 0x1400000);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 3);
    gl_word(t, TEX_LOAD_OPEN);
    gl_pixels_program(t, GL_PIX_4UB, bytes / 4, 0, 0, 1, 1, 1);
    gl_ge_load(t, GL_GE_ERAM, GL_GE_BOX, box, 6);
    gl_word(t, TEX_PAIRS);
    gl_word(t, 1);
    gl_word(t, 2);
    gl_word(t, GL_INLINE_BLOCK | 8);
    gl_word(t, 0x159);
    gl_word(t, 0xc00080);
    gl_word(t, GL_PIXELS_HDR);
    gl_word(t, 4);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 1);
    gl_word(t, 0);
    gl_word(t, depth);
    gl_word(t, 0x511a);
    gl_word(t, flags);
}

static void tex_finish_captured_volume(QTestState *t, const uint8_t *image,
                                      uint32_t depth)
{
    uint32_t bytes = tex_captured_volume_size(depth);

    if (depth == 4) {
        gl_pixels_dma_on(t, 0, image, bytes, bytes, 1, bytes, 0xa7);
    } else {
        gl_pixels_dma_on(t, 0, image, bytes, 8, 2 * depth, 8, 0xa7);
    }
    gl_word(t, TEX_LOAD_CLOSE);
    gl_word(t, TEX_RESTORE_RSS);
}

static void tex_load_captured_volume(QTestState *t, const uint8_t *image,
                                     uint32_t depth)
{
    tex_begin_captured_volume(t, depth);
    tex_finish_captured_volume(t, image, depth);
}

static void tex_load_pixel_endpoint(QTestState *t, const uint8_t *image)
{
    static const uint32_t box[9] = {
        0, 0, 1, 2, 0x00020002, 1, 1, 1, 0,
    };

    tex_record(t, TEX_TL_MODE, TEX_TL_MODE_RGBA8);
    tex_record(t, TEX_TL_SPEC, 0x120);
    gl_word(t, TEX_SAVE_RSS);
    gl_word(t, 0xffffffff);
    gl_word(t, 0x1400000);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 3);
    gl_word(t, TEX_LOAD_OPEN);
    gl_pixels_program(t, GL_PIX_4UB, 4, 0, 0, 1, 1, 1);
    gl_ge_load(t, GL_GE_ERAM, GL_GE_BOX, box, ARRAY_SIZE(box));
    gl_word(t, TEX_PAIRS);
    gl_word(t, 1);
    gl_word(t, 2);
    gl_word(t, GL_INLINE_BLOCK | 8);
    gl_word(t, 0x159);
    gl_word(t, 0xc00080);
    gl_word(t, GL_PIXELS_HDR);
    gl_word(t, 4);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 1);
    gl_word(t, 0);
    gl_word(t, 1);
    gl_word(t, 0x511a);
    gl_word(t, 0x98);
    gl_pixels_dma_on(t, 0, image, 16, 8, 2, 8, 0xa7);
    gl_word(t, TEX_LOAD_CLOSE);
    gl_word(t, TEX_RESTORE_RSS);
}

static void tex_captured_two_slice_state(QTestState *t, bool enabled)
{
    tex_bind(t, 0x40859, 0x815804);
    tex_records(t, (const uint32_t[]){
        TEX_TXSIZE, 0x81112, TEX_TXLOD, 1, TEX_TXCLAMPSIZE, 0x4002,
    }, 3);
    tex_record(t, 0x18a, enabled ? 0x40445 : 0x40444);
}

static void tex_captured_four_slice_state(QTestState *t)
{
    tex_bind(t, 0x40859, 0x815804);
    tex_records(t, (const uint32_t[]){
        TEX_TXSIZE, 0x101112, TEX_TXLOD, 1, TEX_TXCLAMPSIZE, 0x4002,
    }, 3);
    tex_record(t, 0x18a, 0x4040845);
}

static void tex_quad_volume_z(QTestState *t, float z)
{
    static const float v[4][4] = {
        { 16, 16, 0, 0 }, { 48, 16, 1, 0 },
        { 48, 32, 1, 1 }, { 16, 32, 0, 1 },
    };
    static const float white[4] = { 1, 1, 1, 1 };

    gl_color4f(t, white[0], white[1], white[2], white[3]);
    gl_word(t, GL_BEGIN_QUADS);
    for (int i = 0; i < 4; i++) {
        gl_texcoord4f(t, v[i][2], v[i][3], z, 1);
        gl_vertex2f(t, v[i][0], v[i][1]);
    }
    gl_word(t, GL_END_OF(GL_BEGIN_QUADS));
    gl_word(t, 0x0000e910);
    for (int i = 0; i < 4; i++) {
        gl_word(t, i == 0);
    }
    gl_one(t, TEX_T_OFFSET, 0);
    gl_word(t, TEX_OFF);
}

static void tex_check_ordinary_4x2(QTestState *t)
{
    uint8_t ordinary[32];

    tex_texels(ordinary);
    tex_load(t, TEX_TL_MODE_RGBA8, ordinary);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 4; x++) {
            const uint8_t *p = &ordinary[4 * (4 * y + x)];

            g_assert_cmphex(tex_texel_pixel(t, x, y), ==,
                            p[0] << 16 | p[1] << 8 | p[2]);
        }
    }
}

static void test_gl_texture_pixel_rgba_nearest(gconstpointer data)
{
    static const uint32_t box[6] = { 0, 0, 2, 2, 2 << 16 | 2, 0 };
    static const uint8_t image[16] = {
        0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
        0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff,
    };
    static const uint8_t pixels[16] = {
        0x40, 0x40, 0, 0xff, 0xbf, 0x40, 0, 0xff,
        0x40, 0xbf, 0, 0xff, 0xbf, 0xbf, 0, 0xff,
    };
    static const uint32_t pixel_texgen_state[7] = {
        0, 0x42010000, 0x14, 1, 0x34000, 0, 2,
    };
    static const uint32_t pixel_texgen_mode[1] = { 0x352d };
    static const uint32_t pixel_texgen_blend[8] = {
        0x83800, 0x83000, 0x82800, 0x82000,
        0x81800, 0x81000, 0x80800, 0x40080000,
    };
    static const uint32_t expected[4] = {
        0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
    };
    QTestState *t = texture_board_start(data);

    tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_2X2, image, 2, 2,
                box, true);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_record(t, TEX_TXSIZE, TEX_TXSIZE_2X2);
    tex_record(t, TEX_TXCLAMPSIZE, 0x4002);
    tex_record(t, 0x18a, TEX_MDDMA_PIXEL_ON);

    /* These GE parameter loads precede the public pixel-texture draw. */
    gl_ge_load(t, GL_GE_ERAM, 0x187, pixel_texgen_state,
               ARRAY_SIZE(pixel_texgen_state));
    gl_ge_load(t, GL_GE_ERAM, 0x19b, pixel_texgen_mode,
               ARRAY_SIZE(pixel_texgen_mode));
    gl_ge_load(t, GL_GE_ERAM, 0x1ef, pixel_texgen_blend,
               ARRAY_SIZE(pixel_texgen_blend));
    gl_pixels_program(t, GL_PIX_4UB, 2, 0, 0, 1, 1, 1);
    gl_raster_pos2f(t, 0, 0);
    gl_word(t, GL_PIXELS_HDR);
    gl_word(t, 2);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 2);
    gl_word(t, 0);
    gl_word(t, 1);
    gl_word(t, 0x4fc5);
    gl_word(t, 0x98);
    gl_pixels_dma_on(t, 0, pixels, sizeof(pixels), 8, 2, 8, 0xa7);
    tex_record(t, 0x18a, TEX_MDDMA_PIXEL_OFF);
    gl_pixels_fence(t);

    for (uint32_t i = 0; i < ARRAY_SIZE(expected); i++) {
        g_assert_cmphex(gl_window_pixel(t, i % 2, i / 2), ==, expected[i]);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void test_gl_texture_volume_pixel_rgba(gconstpointer data)
{
    static const uint32_t box[6] = { 0, 0, 2, 2, 2 << 16 | 2, 0 };
    static const uint32_t input[4] = {
        0x404000, 0xbf4040, 0x40bfbf, 0xbfbfff,
    };
    static const uint32_t texels[4] = {
        0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
    };
    QTestState *t = texture_board_start(data);
    const uint8_t image[16] = {
        0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
        0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff,
    };

    /*
     * The upload box is 0,0 with a 2x2 size; DrawPixels below gets its
     * own inclusive 0..1 destination box and starts at raster position 0,0.
     * The primitive samples its 2x2 level at x=24/40 and y=20/28.
     */
    tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_2X2, image, 2, 2,
                box, true);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | TEX_TEXMODE2_VOLUME);
    tex_record(t, TEX_TXSIZE, TEX_TXSIZE_2X2);
    tex_record(t, 0x18a, TEX_MDDMA_PIXEL_OFF);
    tex_quad(t);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            g_assert_cmphex(gl_window_pixel(t, 24 + 16 * x, 20 + 8 * y), ==,
                            texels[y * 2 + x]);
        }
    }

    /*
     * tex_quad closes its primitive draw with TEX_OFF. Re-enable the same
     * volume level for pixel-texture sampling.
     */
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | TEX_TEXMODE2_VOLUME);
    tex_record(t, TEX_TXSIZE, TEX_TXSIZE_2X2);
    tex_volume_pixels(t, false, 0);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            g_assert_cmphex(gl_window_pixel(t, x, y), ==, input[y * 2 + x]);
        }
    }

    tex_volume_pixels(t, true, 0);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            g_assert_cmphex(gl_window_pixel(t, x, y), ==, texels[y * 2 + x]);
        }
    }
    {
        static const uint8_t synthetic_slices[32] = {
            0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
            0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff,
            0, 0xff, 0xff, 0xff, 0xff, 0, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0xff,
        };
        static const uint32_t slices[2][4] = {
            { 0xff0000, 0x00ff00, 0x0000ff, 0xffff00 },
            { 0x00ffff, 0xff00ff, 0xffffff, 0x000000 },
        };

        /* The 16-byte depth-one image is the captured DMA payload. */
        tex_check_ordinary_4x2(t);
        tex_load_captured_volume(t, image, 1);
        tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | TEX_TEXMODE2_VOLUME);
        tex_record(t, TEX_TXSIZE, TEX_TXSIZE_2X2);
        tex_quad(t);
        {
            for (int y = 0; y < 2; y++) {
                for (int x = 0; x < 2; x++) {
                    g_assert_cmphex(gl_window_pixel(t, 24 + 16 * x,
                                                    20 + 8 * y), ==,
                                    slices[0][y * 2 + x]);
                }
            }
        }

        /*
         * Exercise the captured enabled GL pixel draw with its exact GE
         * box and an empty destination. A rejected draw leaves black.
         */
        tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | TEX_TEXMODE2_VOLUME);
        tex_record(t, TEX_TXSIZE, TEX_TXSIZE_2X2);
        gl_clear_color(t, 0, 0, 0, 1);
        gl_word(t, GL_CLEAR_COLOR_BUFFER);
        tex_volume_pixels(t, true, 0x00020002);
        for (int y = 0; y < 2; y++) {
            for (int x = 0; x < 2; x++) {
                g_assert_cmphex(gl_window_pixel(t, x, y), ==,
                                texels[y * 2 + x]);
            }
        }

        /* The exception requires both the captured size and enabled state. */
        gl_clear_color(t, 0, 0, 0, 1);
        gl_word(t, GL_CLEAR_COLOR_BUFFER);
        tex_volume_pixels(t, false, 0x00020002);
        for (int y = 0; y < 2; y++) {
            for (int x = 0; x < 2; x++) {
                g_assert_cmphex(gl_window_pixel(t, x, y), ==, 0);
            }
        }

        gl_clear_color(t, 0, 0, 0, 1);
        gl_word(t, GL_CLEAR_COLOR_BUFFER);
        tex_volume_pixels(t, true, 0x00030002);
        for (int y = 0; y < 2; y++) {
            for (int x = 0; x < 2; x++) {
                g_assert_cmphex(gl_window_pixel(t, x, y), ==, 0);
            }
        }

        /* Slice one is synthetic; the capture had no second-slice DMA data. */
        tex_load_captured_volume(t, synthetic_slices, 2);
        for (int z = 0; z < 2; z++) {
            tex_bind(t, TEX_TEXMODE1_ON,
                     TEX_TEXMODE2_RGBA8 | TEX_TEXMODE2_VOLUME);
            tex_record(t, TEX_TXSIZE, TEX_TXSIZE_2X2 | (1u << 19));
            tex_quad_volume_z(t, z ? 0.75f : 0.25f);
            for (int y = 0; y < 2; y++) {
                for (int x = 0; x < 2; x++) {
                    g_assert_cmphex(gl_window_pixel(t, 24 + 16 * x,
                                                    20 + 8 * y),
                                    ==, slices[z][y * 2 + x]);
                }
            }
        }

        /* The tuple is captured; the second slice has no captured DMA bytes. */
        tex_captured_two_slice_state(t, true);
        for (int z = 0; z < 2; z++) {
            tex_quad_volume_z(t, z ? 0.75f : 0.25f);
            for (int y = 0; y < 2; y++) {
                for (int x = 0; x < 2; x++) {
                    g_assert_cmphex(gl_window_pixel(t, 24 + 16 * x,
                                                    20 + 8 * y),
                                    ==, slices[z][y * 2 + x]);
                }
            }
            if (z == 0) {
                tex_captured_two_slice_state(t, true);
            }
        }

        /* Clearing captured MDDMA enable leaves the quad untextured. */
        tex_captured_two_slice_state(t, false);
        tex_quad_volume_z(t, 0.25f);
        for (int y = 0; y < 2; y++) {
            for (int x = 0; x < 2; x++) {
                g_assert_cmphex(gl_window_pixel(t, 24 + 16 * x,
                                                20 + 8 * y), ==, TEX_WHITE);
            }
        }
        {
            /* Synthetic bytes cover all slices; the capture gives row size. */
            static const uint8_t four_slices[64] = {
                0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
                0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff,
                0, 0xff, 0xff, 0xff, 0xff, 0, 0xff, 0xff,
                0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0xff,
                0xff, 0x80, 0, 0xff, 0x80, 0, 0xff, 0xff,
                0, 0x80, 0xff, 0xff, 0x80, 0x80, 0x80, 0xff,
                0x20, 0x40, 0x60, 0xff, 0x60, 0x40, 0x20, 0xff,
                0xa0, 0x80, 0x60, 0xff, 0x60, 0x80, 0xa0, 0xff,
            };
            static const uint32_t expected[4][4] = {
                { 0xff0000, 0x00ff00, 0x0000ff, 0xffff00 },
                { 0x00ffff, 0xff00ff, 0xffffff, 0x000000 },
                { 0xff8000, 0x8000ff, 0x0080ff, 0x808080 },
                { 0x204060, 0x604020, 0xa08060, 0x6080a0 },
            };
            static const uint8_t red_green[4][2] = {
                { 64, 64 }, { 192, 64 }, { 64, 192 }, { 192, 192 },
            };
            uint8_t pixels[16];

            tex_load_captured_volume(t, four_slices, 4);
            tex_captured_four_slice_state(t);
            for (int z = 0; z < 4; z++) {
                /* Blue uses the four captured slice-centre values. */
                for (int i = 0; i < 4; i++) {
                    pixels[4 * i + 0] = red_green[i][0];
                    pixels[4 * i + 1] = red_green[i][1];
                    pixels[4 * i + 2] = 32 + z * 64;
                    pixels[4 * i + 3] = 0xff;
                }
                tex_volume_pixels_data(t, true, 0x00020002, pixels,
                                       0x4040845, 0x4040844);
                uint32_t got[4];

                gl_window_quad(t, got);
                for (int i = 0; i < 4; i++) {
                    g_assert_cmphex(got[i], ==, expected[z][i]);
                }
            }

            /* A depth-two state is unrelated to the depth-four tuple. */
            tex_records(t, (const uint32_t[]){ TEX_TXSIZE, 0x81112 }, 1);
            tex_volume_pixels_data(t, true, 0x00020002, pixels,
                                   0x4040845, 0x4040844);
            uint32_t got[4];

            gl_window_quad(t, got);
            for (int i = 0; i < 4; i++) {
                g_assert_cmphex(got[i], ==,
                                pixels[4 * i] << 16 |
                                pixels[4 * i + 1] << 8 |
                                pixels[4 * i + 2]);
            }
        }
    }
    tex_check_ordinary_4x2(t);

    tex_load_pixel_endpoint(t, image);
    tex_bind(t, 0x40859, 0x815804);
    tex_records(t, (const uint32_t[]){
        TEX_TXSIZE, 0x1112, TEX_TXLOD, 1, TEX_TXCLAMPSIZE, 0x4002,
    }, 3);
    gl_clear_color(t, 0, 0, 0, 1);
    gl_word(t, GL_CLEAR_COLOR_BUFFER);
    tex_volume_pixels(t, true, 0x00020002);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            g_assert_cmphex(gl_window_pixel(t, x, y), ==,
                            texels[y * 2 + x]);
        }
    }

    tex_check_ordinary_4x2(t);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void tex_ordinary_volume_state(QTestState *t, uint32_t depth)
{
    uint32_t size = depth == 1 ? 0x1112 :
                    depth == 2 ? 0x81112 : 0x101112;
    uint32_t control = depth == 1 ? 0x3c000044 :
                       depth == 2 ? 0x00000444 : 0x04000844;

    tex_bind(t, 0x40859, 0x815804);
    tex_records(t, (const uint32_t[]){
        TEX_TXSIZE, size,
        TEX_TXLOD, 1, TEX_TXCLAMPSIZE, 0x4002,
    }, 3);
    tex_record(t, 0x18a, control);
}

static void tex_ordinary_point_color(QTestState *t, float r,
                                     float red, float green, float blue)
{
    gl_color3f(t, red, green, blue);
    gl_word(t, TEX_GL_BEGIN_POINTS);
    gl_word(t, TEX_GL_TEX_COORD3F_HDR);
    gl_float(t, 0);
    gl_float(t, 0);
    gl_float(t, r);
    gl_vertex2f(t, 32, 32);
    gl_word(t, GL_END_OF(TEX_GL_BEGIN_POINTS));
}

static void tex_ordinary_point(QTestState *t, float r)
{
    tex_ordinary_point_color(t, r, 1, 1, 1);
}

static void test_gl_texture_ordinary_volume_point(gconstpointer data)
{
    static const uint8_t one_slice[16] = {
        0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
        0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff,
    };
    static const uint8_t two_slices[32] = {
        0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
        0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff,
        0, 0xff, 0xff, 0xff, 0xff, 0, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0xff,
    };
    QTestState *t = texture_board_start(data);
    uint8_t ordinary[32];

    gl_transform_start(t);
    tex_texels(ordinary);
    tex_load(t, TEX_TL_MODE_RGBA8, ordinary);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==,
                    ordinary[0] << 16 | ordinary[1] << 8 | ordinary[2]);

    tex_load_captured_volume(t, one_slice, 1);
    tex_ordinary_volume_state(t, 1);
    tex_ordinary_point(t, 0.5f);
    g_assert_cmphex(gl_window_pixel(t, 32, 32), ==, 0xff0000);

    tex_load_captured_volume(t, two_slices, 2);
    tex_ordinary_volume_state(t, 2);
    tex_ordinary_point(t, 0.25f);
    g_assert_cmphex(gl_window_pixel(t, 32, 32), ==, 0xff0000);
    tex_ordinary_point(t, 0.75f);
    g_assert_cmphex(gl_window_pixel(t, 32, 32), ==, 0x00ffff);

    /* The ordinary tuple requires its captured DMA-control word. */
    tex_record(t, 0x18a, 0x00000445);
    tex_ordinary_point(t, 0.75f);
    g_assert_cmphex(gl_window_pixel(t, 32, 32), ==, TEX_WHITE);

    /* A depth-one size word cannot select the uploaded depth-two volume. */
    tex_ordinary_volume_state(t, 1);
    tex_ordinary_point(t, 0.75f);
    g_assert_cmphex(gl_window_pixel(t, 32, 32), ==, TEX_WHITE);

    {
        static const uint8_t four_slices[64] = {
            0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
            0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff,
            0, 0xff, 0xff, 0xff, 0xff, 0, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0xff,
            0xff, 0x80, 0, 0xff, 0x80, 0, 0xff, 0xff,
            0, 0x80, 0xff, 0xff, 0x80, 0x80, 0x80, 0xff,
            0x20, 0x40, 0x60, 0xff, 0x60, 0x40, 0x20, 0xff,
            0xa0, 0x80, 0x60, 0xff, 0x60, 0x80, 0xa0, 0xff,
        };
        static const uint32_t expected[4] = {
            0xff0000, 0x00ffff, 0xff8000, 0x204060,
        };

        tex_load_captured_volume(t, four_slices, 4);
        tex_ordinary_volume_state(t, 4);
        for (int z = 0; z < 4; z++) {
            tex_ordinary_point(t, (z + 0.5f) / 4);
            g_assert_cmphex(gl_window_pixel(t, 32, 32), ==, expected[z]);
        }

        /* The depth-four draw requires its captured control word and enable. */
        tex_record(t, 0x18a, 0x04000845);
        tex_ordinary_point(t, 0.875f);
        g_assert_cmphex(gl_window_pixel(t, 32, 32), ==, TEX_WHITE);
        tex_ordinary_volume_state(t, 4);
        tex_bind(t, 0, 0x815804);
        tex_ordinary_point_color(t, 0.125f, 0, 1, 0);
        g_assert_cmphex(gl_window_pixel(t, 32, 32), ==, 0x00ff00);
    }

    tex_load(t, TEX_TL_MODE_RGBA8, ordinary);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==,
                    ordinary[0] << 16 | ordinary[1] << 8 | ordinary[2]);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * A volume the way pfCity's client loads it: TL_SPEC once, its width the
 * slice's doubled, the six GE load words at 0xe3d once, then one 0x9b00
 * bracket a slice, nothing in the bracket naming the slice. The load
 * words are pfCity's shape: the width, parity 0, level 0, TXMIPMAP word
 * 0x100, no border block and the blocks a slice pair takes.
 */
#define TEX_SLICE_W              4
#define TEX_SLICE_H              4
#define TEX_SLICE_D              4
#define TEX_SLICE_BYTES          (TEX_SLICE_W * TEX_SLICE_H * 4)

static void tex_load_volume_slices_shape(QTestState *t, const uint8_t *image,
                                         uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t bytes = w * h * 4;
    uint32_t box[6] = { 0, 0, w, h, h << 16 | w, 0 };
    uint32_t load[6] = { w, 0, 0, 0x100, 0xffffffff, 1 };
    static const uint32_t draw[8] = { 0, 0, 0, 1, 0, 1, 0x511a, 0x98 };
    uint32_t unpack[6] = {
        bytes / 4, tex_unpack_word(4), 0x464, 0xb, 0xc,
        tex_unpack_scale(1),
    };

    tex_record(t, TEX_TL_MODE, TEX_TL_MODE_RGBA8);
    tex_record(t, TEX_TL_SPEC, ctz32(h) << 8 | (ctz32(w) + 1) << 4);
    gl_word(t, TEX_SAVE_RSS);
    gl_word(t, 0xffffffff);
    gl_word(t, 0x1400000);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 3);
    gl_ge_load(t, GL_GE_ERAM, 0xe3d, load, ARRAY_SIZE(load));
    for (uint32_t z = 0; z < d; z++) {
        gl_word(t, TEX_LOAD_OPEN);
        gl_pixels_program(t, GL_PIX_4UB, bytes / 4, 0, 0, w - 1, h - 1, 1);
        gl_ge_load(t, GL_GE_ERAM, GL_GE_UNPACK, unpack, ARRAY_SIZE(unpack));
        gl_ge_load(t, GL_GE_ERAM, GL_GE_BOX, box, ARRAY_SIZE(box));
        gl_word(t, TEX_PAIRS);
        gl_word(t, 1);
        gl_word(t, 2);
        gl_word(t, GL_INLINE_BLOCK | 8);
        gl_word(t, 0x159);
        gl_word(t, 0xc00080);
        gl_word(t, GL_PIXELS_HDR);
        gl_word(t, bytes / 4);
        for (int i = 1; i < 8; i++) {
            gl_word(t, draw[i]);
        }
        gl_pixels_dma_on(t, 0, image + z * bytes, bytes, bytes, 1, bytes,
                         0xa7);
        gl_word(t, TEX_LOAD_CLOSE);
    }
    gl_word(t, TEX_RESTORE_RSS);
}

static void tex_load_volume_slices(QTestState *t, const uint8_t *image)
{
    tex_load_volume_slices_shape(t, image, TEX_SLICE_W, TEX_SLICE_H,
                                 TEX_SLICE_D);
}

/*
 * pfCity's draw words for its volume, scaled to this one: TEXMODE2 with
 * the volume bit, TXSIZE's low width nibble one above 11:8 and the log2
 * depth in 22:19, MDDMA_CNTRL's log2 sizes, TXMIPMAP 0x100.
 */
static void tex_volume_slices_state(QTestState *t, uint32_t texmode2)
{
    uint32_t lw = ctz32(TEX_SLICE_W), lh = ctz32(TEX_SLICE_H);
    uint32_t ld = ctz32(TEX_SLICE_D);

    tex_record(t, 0x18a, (ld - 1) << 26 | ld << 10 | lh << 6 | lw << 2);
    tex_bind(t, 0x40859, texmode2);
    gl_word(t, TEX_TABLE);
    gl_word(t, 0xc52);
    gl_word(t, 0xc53);
    gl_word(t, TEX_TXMIPMAP);
    gl_word(t, 0x100);
    tex_records(t, (const uint32_t[]){
        TEX_TXSIZE, ld << 19 | lh << 12 | lw << 8 | lh << 4 | (lw + 1),
        TEX_TXLOD, 1,
        TEX_TXCLAMPSIZE, TEX_SLICE_W << 13 | TEX_SLICE_H,
    }, 3);
}

/*
 * Each bracket after the first loads the next slice: nearest sampling
 * at every texel of every slice returns that texel, so the slices are
 * kept apart and each keeps its rows. pfCity's own linear, repeating
 * state then returns a slice's colour at the slice's centre.
 */
static void test_gl_texture_volume_slices(gconstpointer data)
{
    QTestState *t = texture_board_start(data);
    uint8_t image[TEX_SLICE_BYTES * TEX_SLICE_D];
    uint8_t flat[TEX_SLICE_BYTES * TEX_SLICE_D];

    for (uint32_t z = 0; z < TEX_SLICE_D; z++) {
        for (uint32_t y = 0; y < TEX_SLICE_H; y++) {
            for (uint32_t x = 0; x < TEX_SLICE_W; x++) {
                uint8_t *p = &image[((z * TEX_SLICE_H + y) * TEX_SLICE_W +
                                     x) * 4];
                uint8_t *f = &flat[((z * TEX_SLICE_H + y) * TEX_SLICE_W +
                                    x) * 4];

                p[0] = 0x10 + 0x30 * x;
                p[1] = 0x20 + 0x30 * y;
                p[2] = 0x08 + 0x40 * z;
                p[3] = 0xff;
                f[0] = 0x40 * z;
                f[1] = 0xff - 0x30 * z;
                f[2] = 0x11 * (z + 1);
                f[3] = 0xff;
            }
        }
    }

    tex_load_volume_slices(t, image);
    for (uint32_t z = 0; z < TEX_SLICE_D; z++) {
        /* Each quad ends with the client's 0x0c00, so bind again. */
        tex_volume_slices_state(t, 0x810004);
        tex_quad_volume_z(t, (z + 0.5f) / TEX_SLICE_D);
        for (uint32_t y = 0; y < TEX_SLICE_H; y++) {
            for (uint32_t x = 0; x < TEX_SLICE_W; x++) {
                const uint8_t *p = &image[((z * TEX_SLICE_H + y) *
                                           TEX_SLICE_W + x) * 4];

                g_assert_cmphex(gl_window_pixel(t, 20 + 8 * x, 18 + 4 * y),
                                ==, p[0] << 16 | p[1] << 8 | p[2]);
            }
        }
    }

    tex_load_volume_slices(t, flat);
    for (uint32_t z = 0; z < TEX_SLICE_D; z++) {
        const uint8_t *f = &flat[z * TEX_SLICE_BYTES];

        tex_volume_slices_state(t, 0x8101c4);
        tex_quad_volume_z(t, (z + 0.5f) / TEX_SLICE_D);
        g_assert_cmphex(gl_window_pixel(t, 32, 24), ==,
                        f[0] << 16 | f[1] << 8 | f[2]);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * The pixel-texture probe's 2x2x1 3-D texture as the IRIX client sends it:
 * one slice into a slice-pair level twice the slice's width (a 4x2 level
 * at TXMIPMAP 0x100, as a guest trace of the probe shows), then TXSIZE
 * 0x1112 and MDDMA_CNTRL 0x3c040045 for the pixel-texture draw. The draw
 * samples the 2x2 slice, not the 4x2 level: the probe's red and green
 * bytes 0x40 and 0xbf pick the four texels.
 */
static void test_gl_texture_pixel_texture_volume_slice(gconstpointer data)
{
    static const uint8_t slice[16] = {
        0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
        0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff,
    };
    static const uint8_t pixels[16] = {
        0x40, 0x40, 0x00, 0xff, 0xbf, 0x40, 0x00, 0xff,
        0x40, 0xbf, 0x00, 0xff, 0xbf, 0xbf, 0x00, 0xff,
    };
    static const uint32_t texels[4] = {
        0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
    };
    QTestState *t = texture_board_start(data);
    uint32_t res = tex_re_count(t);

    tex_load_volume_slices_shape(t, slice, 2, 2, 1);
    tex_bind(t, 0x40859, 0x815804);
    gl_word(t, TEX_TABLE);
    gl_word(t, 0xc52);
    gl_word(t, 0xc53);
    gl_word(t, TEX_TXMIPMAP);
    gl_word(t, 0x100);
    tex_records(t, (const uint32_t[]){
        TEX_TXSIZE, 0x1112, TEX_TXLOD, 1, TEX_TXCLAMPSIZE, 0x4002,
    }, 3);
    gl_clear_color(t, 0, 0, 0, 1);
    gl_word(t, GL_CLEAR_COLOR_BUFFER);
    for (uint32_t pass = 0; pass < res; pass++) {
        tex_pixel_texture_pass(t, res, pass, pixels, 2, 0x3c040045,
                               0x3c040044);
    }
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            g_assert_cmphex(gl_window_pixel(t, x, y), ==, texels[y * 2 + x]);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * A zoomed pixel-texture draw, glPixelZoom(2, 2) of a 2x2 image: the
 * client's zoomed branch gives each pass the source rows it carries, a
 * box whose x runs over the zoomed width and whose height is the pass's
 * zoomed rows times the RE count, 0x192 = 1/2 and the y-zoom writing
 * routine 0x3663. Every texel must fill its 2x2 footprint on one RE or
 * two.
 */
static void test_gl_texture_pixel_texture_zoom(gconstpointer data)
{
    static const uint8_t slice[16] = {
        0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
        0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff,
    };
    static const uint8_t pixels[16] = {
        0x40, 0x40, 0x00, 0xff, 0xbf, 0x40, 0x00, 0xff,
        0x40, 0xbf, 0x00, 0xff, 0xbf, 0xbf, 0x00, 0xff,
    };
    static const uint32_t texels[4] = {
        0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
    };
    static const uint32_t program[12] = {
        0x1f44, 0, 0x3e65, 0xd91, 0x156a, 0x1b4, 0x3663, 0xdf3,
        0x3ef5, 0x18e, 0x374a, 0xe1c,
    };
    QTestState *t = texture_board_start(data);
    uint32_t res = tex_re_count(t);

    tex_load_volume_slices_shape(t, slice, 2, 2, 1);
    tex_bind(t, 0x40859, 0x815804);
    gl_word(t, TEX_TABLE);
    gl_word(t, 0xc52);
    gl_word(t, 0xc53);
    gl_word(t, TEX_TXMIPMAP);
    gl_word(t, 0x100);
    tex_records(t, (const uint32_t[]){
        TEX_TXSIZE, 0x1112, TEX_TXLOD, 1, TEX_TXCLAMPSIZE, 0x4002,
    }, 3);
    gl_clear_color(t, 0, 0, 0, 1);
    gl_word(t, GL_CLEAR_COLOR_BUFFER);
    for (uint32_t pass = 0; pass < res; pass++) {
        uint32_t src_rows = (2 + res - pass - 1) / res;
        uint32_t zoomed = src_rows * res * 2;
        uint32_t dest_rows = (zoomed + res - pass - 1) / res;
        uint32_t box[9] = {
            0, 0, 3, dest_rows * res, dest_rows << 16 | 2, 1, 1, 1, pass,
        };
        uint8_t sent[16];

        for (uint32_t i = 0; i < src_rows; i++) {
            memcpy(&sent[8 * i], &pixels[8 * (pass + res * i)], 8);
        }
        tex_record(t, 0x18a, 0x3c040045);
        gl_pixels_program(t, GL_PIX_4UB, 2, 0, 0, 1, 1, 2);
        gl_ge_load(t, GL_GE_CRAM, GL_GE_PROGRAM, program,
                   ARRAY_SIZE(program));
        gl_ge_load(t, GL_GE_ERAM, GL_GE_BOX, box, ARRAY_SIZE(box));
        gl_raster_pos2f(t, 0, 0);
        gl_word(t, GL_PIXELS_HDR);
        gl_word(t, 2);
        gl_word(t, 0);
        gl_word(t, 0);
        gl_word(t, 2);
        gl_word(t, 0);
        gl_word(t, 1);
        gl_word(t, 0x4fc5);
        gl_word(t, 0x18);
        gl_pixels_dma_on(t, 0, sent, 8 * src_rows, 8, src_rows, 8, 0xa7);
        tex_record(t, 0x18a, 0x3c040044);
        gl_pixels_fence(t);
    }
    for (int y = 0; y < 5; y++) {
        for (int x = 0; x < 5; x++) {
            uint32_t want = x < 4 && y < 4 ? texels[(y / 2) * 2 + x / 2] : 0;

            g_assert_cmphex(gl_window_pixel(t, x, y), ==, want);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * A zoomed draw waiting for its DMA image crosses a migration and fills
 * its zoomed footprint on the destination (glPixelZoom(2, 2) of a 2x1
 * image, the client's words). Without the zoom in the migration stream
 * it would draw one pixel a source pixel.
 */
static void test_gl_pixels_zoom_migration(gconstpointer data)
{
    static const uint32_t program[8] = {
        0x1f44, 0, 0x3e65, 0xd91, 0x156a, 0x1b4, 0x3663, 0xdf3,
    };
    static const uint32_t box[6] = { 0, 0, 3, 1, 2 << 16 | 2, 1 };
    static const uint8_t image[8] = {
        0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-pixel-zoom-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(tmpfs, "state", NULL);
    uri = g_strdup_printf("file:%s", path);
    args = g_strdup_printf(TEX_COLOR_TABLE_MIGRATION_ARGS, (char *)data);
    from = tex_start(args);
    gl_clear_color(from, 0, 0, 0, 1);
    gl_word(from, GL_CLEAR_COLOR_BUFFER);
    gl_pixels_program(from, GL_PIX_4UB, 2, 0, 0, 1, 0, 2);
    gl_ge_load(from, GL_GE_CRAM, GL_GE_PROGRAM, program, 8);
    gl_ge_load(from, GL_GE_ERAM, GL_GE_BOX, box, 6);
    gl_raster_pos2f(from, 8, 8);
    gl_pixels_draw_as(from, 2, GL_PIXELS_BY_DMA);
    to = tex_sb_file_migrate_and_restart(from, args, uri);

    gl_pixels_dma_on(to, 0, image, sizeof(image), 8, 1, 8, 0xa7);
    gl_pixels_fence(to);
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 5; x++) {
            uint32_t want = y < 2 && x < 4 ? (x < 2 ? 0xff0000 : 0x00ff00) : 0;

            g_assert_cmphex(gl_window_pixel(to, 8 + x, 8 + y), ==, want);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);
    qtest_quit(to);
    unlink(path);
    g_rmdir(tmpfs);
}

static void tex_load_captured_rect_volume(QTestState *t, const uint8_t *image,
                                          uint32_t width, uint32_t height,
                                          uint32_t depth, bool bad_area)
{
    uint32_t spec = (ctz32(width) + 1) << 4 | ctz32(height) << 8;
    uint32_t box[6] = { 0, 0, width, height, height << 16 | width, depth };
    uint32_t bytes = width * height * depth * 4;

    g_assert_true(width == 2 || width == 4 || width == 8 ||
                  width == 16 || width == 32);
    g_assert_true(height == 2 || height == 4 || height == 8 ||
                  height == 16 || height == 32);
    g_assert_true(depth == 2 || depth == 4 || depth == 8 ||
                  depth == 16 || depth == 32 || depth == 64);
    g_assert_true(depth <= 4 || (width == 8 && height == 8) ||
                  (width == 32 && height == 32 && depth == 16) ||
                  (width == 16 && height == 16 && depth == 64));
    tex_record(t, TEX_TL_MODE, TEX_TL_MODE_RGBA8);
    tex_record(t, TEX_TL_SPEC, spec);
    gl_word(t, TEX_SAVE_RSS);
    gl_word(t, 0xffffffff);
    gl_word(t, 0x1400000);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 3);
    gl_word(t, TEX_LOAD_OPEN);
    gl_pixels_program(t, GL_PIX_4UB, width * height * depth, 0, 0, 1, 1, 1);
    gl_ge_load(t, GL_GE_ERAM, GL_GE_BOX, box, ARRAY_SIZE(box));
    gl_word(t, TEX_PAIRS);
    gl_word(t, 1);
    gl_word(t, 2);
    gl_word(t, GL_INLINE_BLOCK | 8);
    gl_word(t, 0x159);
    gl_word(t, 0xc00080);
    gl_word(t, GL_PIXELS_HDR);
    gl_word(t, (depth > 4 && width != 8 ? 0x40 : width * height) +
             bad_area);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 1);
    gl_word(t, width == 32 ? 5 : width == 16 ? 1 : 0);
    gl_word(t, depth);
    gl_word(t, 0x511a);
    gl_word(t, 0x198);
    gl_pixels_dma_on(t, 0, image, bytes, bytes, 1, bytes, 0xa7);
    gl_word(t, TEX_LOAD_CLOSE);
    gl_word(t, TEX_RESTORE_RSS);
}

static void tex_ordinary_deep_volume_state(QTestState *t, uint32_t depth)
{
    static const struct {
        uint32_t depth;
        uint32_t txsize;
        uint32_t control;
    } tuples[] = {
        { 8, 0x00183334, 0x08000ccc },
        { 16, 0x00203334, 0x0c0010cc },
        { 32, 0x00283334, 0x100014cc },
        { 64, 0x00303334, 0x140018cc },
    };
    uint32_t txsize = 0, control = 0;

    for (size_t i = 0; i < ARRAY_SIZE(tuples); i++) {
        if (tuples[i].depth == depth) {
            txsize = tuples[i].txsize;
            control = tuples[i].control;
            break;
        }
    }
    g_assert_cmphex(txsize, !=, 0);
    tex_bind(t, 0x40859, 0x815804);
    tex_records(t, (const uint32_t[]){
        TEX_TXSIZE, txsize,
        TEX_TXLOD, 1, TEX_TXCLAMPSIZE, 0x10008,
    }, 3);
    tex_record(t, 0x18a, control);
}

static uint32_t tex_deep_volume_pixel(uint32_t slice)
{
    return (0x10 + slice) << 16 | (0x20 + slice) << 8 | (0x30 + slice);
}

typedef struct TexBoundaryVolume {
    uint32_t width, height, depth;
    uint32_t txsize, clampsize, ordinary_control, pixel_control;
} TexBoundaryVolume;

static const TexBoundaryVolume tex_boundary_volumes[] = {
    { 32, 32, 16, 0x00205556, 0x40020, 0x0c001154, 0x0c041155 },
    { 16, 16, 64, 0x00304445, 0x20010, 0x14001910, 0x14041911 },
};

static void tex_boundary_volume_state(QTestState *t, size_t index)
{
    const TexBoundaryVolume *volume = &tex_boundary_volumes[index];

    tex_bind(t, 0x40859, 0x815804);
    tex_records(t, (const uint32_t[]){
        TEX_TXSIZE, volume->txsize,
        TEX_TXLOD, 1, TEX_TXCLAMPSIZE, volume->clampsize,
    }, 3);
    tex_record(t, 0x18a, volume->ordinary_control);
}

static void test_gl_texture_captured_boundary_volumes(gconstpointer data)
{
    QTestState *t = texture_board_start(data);

    gl_transform_start(t);
    for (size_t c = 0; c < ARRAY_SIZE(tex_boundary_volumes); c++) {
        const TexBoundaryVolume *volume = &tex_boundary_volumes[c];
        size_t bytes = (size_t)volume->width * volume->height *
                       volume->depth * 4;
        uint8_t *image = g_new(uint8_t, bytes);

        g_assert_cmpuint(bytes, ==, 65536);
        for (uint32_t z = 0; z < volume->depth; z++) {
            for (uint32_t pixel = 0;
                 pixel < volume->width * volume->height; pixel++) {
                size_t offset = ((size_t)z * volume->width * volume->height +
                                 pixel) * 4;

                image[offset] = 0x10 + z;
                image[offset + 1] = 0x20 + z;
                image[offset + 2] = 0x30 + z;
                image[offset + 3] = 0xff;
            }
        }

        tex_load_captured_rect_volume(t, image, volume->width,
                                      volume->height, volume->depth, true);
        tex_boundary_volume_state(t, c);
        tex_ordinary_point(t, (volume->depth - 0.5f) / volume->depth);
        g_assert_cmphex(gl_window_pixel(t, 32, 32), ==, TEX_WHITE);

        tex_load_captured_rect_volume(t, image, volume->width,
                                      volume->height, volume->depth, false);
        tex_boundary_volume_state(t, c);
        for (uint32_t z = 0; z < volume->depth; z++) {
            tex_ordinary_point(t, (z + 0.5f) / volume->depth);
            g_assert_cmphex(gl_window_pixel(t, 32, 32), ==,
                            tex_deep_volume_pixel(z));
        }
        g_free(image);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void test_gl_texture_captured_boundary_volumes_pixel(gconstpointer data)
{
    static const uint8_t coordinates[4][2] = {
        { 16, 16 }, { 112, 16 }, { 16, 112 }, { 112, 112 },
    };
    QTestState *t = texture_board_start(data);
    uint8_t pixels[16];

    gl_transform_start(t);
    for (size_t c = 0; c < ARRAY_SIZE(tex_boundary_volumes); c++) {
        const TexBoundaryVolume *volume = &tex_boundary_volumes[c];
        size_t bytes = (size_t)volume->width * volume->height *
                       volume->depth * 4;
        uint8_t *image = g_new(uint8_t, bytes);
        uint32_t enabled = volume->pixel_control;
        uint32_t disabled = enabled & ~1u;

        for (uint32_t z = 0; z < volume->depth; z++) {
            for (uint32_t y = 0; y < volume->height; y++) {
                for (uint32_t x = 0; x < volume->width; x++) {
                    size_t offset = ((size_t)z * volume->height + y) *
                                    volume->width * 4 + x * 4;

                    image[offset] = 0x10 + x;
                    image[offset + 1] = 0x20 + y;
                    image[offset + 2] = 0x30 + z;
                    image[offset + 3] = 0xff;
                }
            }
        }
        tex_load_captured_rect_volume(t, image, volume->width,
                                      volume->height, volume->depth, false);

        for (uint32_t z = 0; z < volume->depth; z++) {
            for (size_t i = 0; i < ARRAY_SIZE(coordinates); i++) {
                pixels[4 * i] = coordinates[i][0];
                pixels[4 * i + 1] = coordinates[i][1];
                pixels[4 * i + 2] = (2 * z + 1) * 128 / volume->depth;
                pixels[4 * i + 3] = 0xff;
            }
            tex_boundary_volume_state(t, c);
            tex_volume_pixels_data(t, true, 0x00020002, pixels,
                                   enabled, disabled);
            uint32_t got[4];

            gl_window_quad(t, got);
            for (size_t i = 0; i < ARRAY_SIZE(coordinates); i++) {
                uint32_t x = coordinates[i][0] * volume->width / 256;
                uint32_t y = coordinates[i][1] * volume->height / 256;
                uint32_t expected = (0x10 + x) << 16 |
                                    (0x20 + y) << 8 | (0x30 + z);

                g_assert_cmphex(got[i], ==, expected);
            }
        }
        g_free(image);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void test_gl_texture_captured_deep_volume(gconstpointer data)
{
    static const uint32_t depths[] = { 8, 16, 32, 64 };
    uint8_t malformed[8 * 8 * 8 * 4] = { 0 };
    uint8_t control[2 * 2 * 4 * 4];
    QTestState *t = texture_board_start(data);

    gl_transform_start(t);
    tex_load_captured_rect_volume(t, malformed, 8, 8, 8, true);
    tex_ordinary_deep_volume_state(t, 8);
    tex_ordinary_point(t, 7.5f / 8);
    g_assert_cmphex(gl_window_pixel(t, 32, 32), ==, TEX_WHITE);

    for (uint32_t z = 0; z < 4; z++) {
        for (uint32_t pixel = 0; pixel < 2 * 2; pixel++) {
            size_t offset = ((size_t)z * 2 * 2 + pixel) * 4;

            control[offset] = 0x10 + z;
            control[offset + 1] = 0x20 + z;
            control[offset + 2] = 0x30 + z;
            control[offset + 3] = 0xff;
        }
    }
    tex_load_captured_volume(t, control, 4);
    tex_ordinary_volume_state(t, 4);
    for (uint32_t z = 0; z < 4; z++) {
        tex_ordinary_point(t, (z + 0.5f) / 4);
        g_assert_cmphex(gl_window_pixel(t, 32, 32), ==,
                        tex_deep_volume_pixel(z));
    }

    for (size_t i = 0; i < ARRAY_SIZE(depths); i++) {
        uint32_t depth = depths[i];
        size_t bytes = 8 * 8 * depth * 4;
        uint8_t *image = g_new(uint8_t, bytes);

        for (uint32_t z = 0; z < depth; z++) {
            for (uint32_t pixel = 0; pixel < 8 * 8; pixel++) {
                size_t offset = ((size_t)z * 8 * 8 + pixel) * 4;

                image[offset] = 0x10 + z;
                image[offset + 1] = 0x20 + z;
                image[offset + 2] = 0x30 + z;
                image[offset + 3] = 0xff;
            }
        }
        tex_load_captured_rect_volume(t, image, 8, 8, depth, false);
        tex_ordinary_deep_volume_state(t, depth);
        for (uint32_t z = 0; z < depth; z++) {
            tex_ordinary_point(t, (z + 0.5f) / depth);
            g_assert_cmphex(gl_window_pixel(t, 32, 32), ==,
                            tex_deep_volume_pixel(z));
        }
        g_free(image);
    }

    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void test_gl_texture_captured_deep_volume_pixel(gconstpointer data)
{
    static const struct {
        uint32_t depth;
        uint32_t pixel_control;
    } cases[] = {
        { 8, 0x08040ccd }, { 16, 0x0c0410cd },
        { 32, 0x100414cd }, { 64, 0x140418cd },
    };
    static const uint8_t coordinates[4][2] = {
        { 32, 32 }, { 96, 32 }, { 32, 96 }, { 96, 96 },
    };
    QTestState *t = texture_board_start(data);
    uint8_t *image = g_new(uint8_t, 8 * 8 * 64 * 4);
    uint8_t pixels[16];

    gl_transform_start(t);
    for (size_t c = 0; c < ARRAY_SIZE(cases); c++) {
        uint32_t depth = cases[c].depth;
        uint32_t enabled = cases[c].pixel_control;
        uint32_t disabled = enabled & ~1u;

        for (uint32_t z = 0; z < depth; z++) {
            for (uint32_t y = 0; y < 8; y++) {
                for (uint32_t x = 0; x < 8; x++) {
                    size_t offset = ((size_t)z * 8 * 8 + y * 8 + x) * 4;

                    image[offset] = 16 + x * 20;
                    image[offset + 1] = 24 + y * 24;
                    image[offset + 2] = 32 + z;
                    image[offset + 3] = 0xff;
                }
            }
        }
        tex_load_captured_rect_volume(t, image, 8, 8, depth, false);
        tex_ordinary_deep_volume_state(t, depth);

        for (uint32_t z = 0; z < depth; z++) {
            for (size_t i = 0; i < ARRAY_SIZE(coordinates); i++) {
                pixels[4 * i] = coordinates[i][0];
                pixels[4 * i + 1] = coordinates[i][1];
                pixels[4 * i + 2] = (2 * z + 1) * 128 / depth;
                pixels[4 * i + 3] = 0xff;
            }
            tex_volume_pixels_data(t, true, 0x00020002, pixels,
                                   enabled, disabled);
            uint32_t got[4];

            gl_window_quad(t, got);
            for (size_t i = 0; i < ARRAY_SIZE(coordinates); i++) {
                uint32_t x = coordinates[i][0] * 8 / 256;
                uint32_t y = coordinates[i][1] * 8 / 256;
                uint32_t expected = (16 + 20 * x) << 16 |
                                    (24 + 24 * y) << 8 | (32 + z);

                g_assert_cmphex(got[i], ==, expected);
            }
        }

        /* A different full control word must fall back to the source pixels. */
        tex_ordinary_deep_volume_state(t, depth);
        for (size_t i = 0; i < ARRAY_SIZE(coordinates); i++) {
            pixels[4 * i] = coordinates[i][0];
            pixels[4 * i + 1] = coordinates[i][1];
            pixels[4 * i + 2] = 32;
            pixels[4 * i + 3] = 0xff;
        }
        tex_volume_pixels_data(t, true, 0x00020002, pixels,
                               enabled ^ 2u, disabled ^ 2u);
        uint32_t got[4];

        gl_window_quad(t, got);
        for (size_t i = 0; i < ARRAY_SIZE(coordinates); i++) {
            uint32_t source = pixels[4 * i] << 16 |
                              pixels[4 * i + 1] << 8 | pixels[4 * i + 2];

            g_assert_cmphex(got[i], ==, source);
        }

        /* A different clamp extent must also leave the source pixels visible. */
        tex_ordinary_deep_volume_state(t, depth);
        tex_record(t, TEX_TXCLAMPSIZE, 0x10004);
        tex_volume_pixels_data(t, true, 0x00020002, pixels, enabled,
                               disabled);
        gl_window_quad(t, got);
        for (size_t i = 0; i < ARRAY_SIZE(coordinates); i++) {
            uint32_t source = pixels[4 * i] << 16 |
                              pixels[4 * i + 1] << 8 | pixels[4 * i + 2];

            g_assert_cmphex(got[i], ==, source);
        }
    }
    g_free(image);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void tex_load_captured_stock_volume(QTestState *t, const uint8_t *image,
                                           uint32_t width, uint32_t height,
                                           uint32_t box_x, uint32_t box_y)
{
    uint32_t spec = (ctz32(width) + 1) << 4 | ctz32(height) << 8;
    uint32_t box[6] = { 0, 0, width, height, height << 16 | width, 4 };
    uint32_t bytes = width * height * 4 * 4;

    tex_record(t, TEX_TL_MODE, TEX_TL_MODE_RGBA8);
    tex_record(t, TEX_TL_SPEC, spec);
    gl_word(t, TEX_SAVE_RSS);
    gl_word(t, 0xffffffff);
    gl_word(t, 0x1400000);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 3);
    gl_word(t, TEX_LOAD_OPEN);
    gl_pixels_program(t, GL_PIX_4UB, width * height * 4, 0, 0, 1, 1, 1);
    gl_ge_load(t, GL_GE_ERAM, GL_GE_BOX, box, ARRAY_SIZE(box));
    gl_word(t, TEX_PAIRS);
    gl_word(t, 1);
    gl_word(t, 2);
    gl_word(t, GL_INLINE_BLOCK | 8);
    gl_word(t, 0x159);
    gl_word(t, 0xc00080);
    gl_word(t, GL_PIXELS_HDR);
    gl_word(t, 0x40);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, box_x);
    gl_word(t, box_y);
    gl_word(t, 4);
    gl_word(t, 0x511a);
    gl_word(t, 0x198);
    gl_pixels_dma_on(t, 0, image, bytes, bytes, 1, bytes, 0xa7);
    gl_word(t, TEX_LOAD_CLOSE);
    gl_word(t, TEX_RESTORE_RSS);
}

static void tex_captured_rect_volume_state(QTestState *t, uint32_t width,
                                           uint32_t height, uint32_t depth)
{
    uint32_t log2w = ctz32(width), log2h = ctz32(height);
    uint32_t size = (ctz32(depth) << 19) |
                    ((log2h << 4 | log2w) << 8) |
                    (log2h << 4 | (log2w + 1));
    uint32_t clamp = width << 13 | height;
    uint32_t control;

    if (width == 64 && (height == 16 || height == 64)) {
        control = 0x844 + (log2w - 1) * 4 + (log2h - 1) * 0x40;
        control |= 0x08800000;
    } else {
        control = (depth == 4 ? 0x44 : 0x444) +
                  (log2w - 1) * 4 + (log2h - 1) * 0x40;
        if (depth == 4) {
            control |= 0x04000800;
        }
    }

    tex_bind(t, 0x40859, 0x815804);
    tex_records(t, (const uint32_t[]){
        TEX_TXSIZE, size, TEX_TXLOD, 1, TEX_TXCLAMPSIZE, clamp,
    }, 3);
    tex_record(t, 0x18a, control);
}

static void test_gl_texture_ordinary_rectangular_volume(void)
{
    static const struct {
        uint32_t width, height, depth;
    } shapes[] = {
        { 4, 2, 2 }, { 2, 4, 2 }, { 8, 2, 2 }, { 2, 8, 2 },
        { 8, 2, 4 },
    };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t ordinary[32];
    uint8_t image[8 * 8 * 4 * 4];

    gl_transform_start(t);
    tex_texels(ordinary);
    tex_load(t, TEX_TL_MODE_RGBA8, ordinary);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    for (size_t s = 0; s < ARRAY_SIZE(shapes); s++) {
        uint32_t width = shapes[s].width, height = shapes[s].height;
        uint32_t depth = shapes[s].depth;

        for (uint32_t z = 0; z < depth; z++) {
            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    uint32_t index = ((z * height + y) * width + x) * 4;

                    image[index] = 40 + x * 24;
                    image[index + 1] = 16 + y * 32;
                    image[index + 2] = 20 + z * 50;
                    image[index + 3] = 0xff;
                }
            }
        }
        tex_load_captured_rect_volume(t, image, width, height, depth, false);
        for (uint32_t z = 0; z < depth; z++) {
            tex_captured_rect_volume_state(t, width, height, depth);
            tex_quad_volume_z(t, (z + 0.5f) / depth);
            for (uint32_t y = 0; y < height; y++) {
                uint32_t py = 16 + (2 * y + 1) * 8 / height;
                g_autofree uint32_t *got = gl_window_row(t, 16, py, 32);

                for (uint32_t x = 0; x < width; x++) {
                    uint32_t px = 16 + (2 * x + 1) * 16 / width;
                    uint32_t expected = (40 + x * 24) << 16 |
                                        (16 + y * 32) << 8 | (20 + z * 50);

                    g_assert_cmphex(got[px - 16], ==, expected);
                }
            }
        }
    }
    /* A rectangle area that disagrees with the transfer stores no image. */
    qtest_system_reset(t);
    gl_enter_slot(t, GL_SLOT_A);
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
    tex_load_captured_rect_volume(t, image, 4, 2, 2, true);
    tex_captured_rect_volume_state(t, 4, 2, 2);
    tex_ordinary_point(t, 0.25f);
    g_assert_cmphex(gl_window_pixel(t, 32, 32), ==, TEX_WHITE);

    /* Disabling texture state leaves the point's green color intact. */
    tex_bind(t, 0, 0x815804);
    tex_ordinary_point_color(t, 0.25f, 0, 1, 0);
    g_assert_cmphex(gl_window_pixel(t, 32, 32), ==, 0x00ff00);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void test_gl_texture_captured_large_volumes(void)
{
    static const struct {
        uint32_t width, height, box_x, box_y;
    } shapes[] = {
        { 16, 16, 1, 1 }, { 32, 16, 2, 2 },
        { 8, 32, 1, 1 }, { 32, 8, 1, 1 }, { 16, 32, 2, 2 },
        { 32, 32, 1, 5 }, { 64, 16, 1, 5 }, { 64, 64, 1, 0x15 },
    };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t image[64 * 64 * 4 * 4];

    gl_transform_start(t);
    for (size_t s = 0; s < ARRAY_SIZE(shapes); s++) {
        uint32_t width = shapes[s].width, height = shapes[s].height;

        for (uint32_t z = 0; z < 4; z++) {
            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    uint32_t i = ((z * height + y) * width + x) * 4;

                    /* Keep filtering from moving the sampled color. */
                    image[i] = 16;
                    image[i + 1] = 32;
                    image[i + 2] = 64 + z;
                    image[i + 3] = 0xff;
                }
            }
        }
        tex_load_captured_stock_volume(t, image, width, height,
                                       shapes[s].box_x, shapes[s].box_y);
        tex_captured_rect_volume_state(t, width, height, 4);
        tex_quad_volume_z(t, 0.875f);
        g_assert_cmphex(gl_window_pixel(t, 32, 24), ==,
                        16 << 16 | 32 << 8 | 67);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void test_gl_texture_ordinary_captured_large_volumes(gconstpointer data)
{
    static const struct {
        uint32_t width, height, box_x, box_y, control;
    } shapes[] = {
        { 64, 16, 1, 5, 0x08800918 },
        { 64, 64, 1, 0x15, 0x08800998 },
    };
    QTestState *t = texture_board_start(data);
    uint8_t image[64 * 64 * 4 * 4];

    gl_transform_start(t);
    for (size_t s = 0; s < ARRAY_SIZE(shapes); s++) {
        uint32_t width = shapes[s].width, height = shapes[s].height;
        uint32_t log2w = ctz32(width), log2h = ctz32(height);
        uint32_t size = (2u << 19) |
                      ((log2h << 4 | log2w) << 8) |
                      (log2h << 4 | (log2w + 1));

        for (uint32_t z = 0; z < 4; z++) {
            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    uint32_t i = ((z * height + y) * width + x) * 4;

                    image[i] = 16;
                    image[i + 1] = 32;
                    image[i + 2] = 64 + z;
                    image[i + 3] = 0xff;
                }
            }
        }
        tex_load_captured_stock_volume(t, image, width, height,
                                       shapes[s].box_x, shapes[s].box_y);
        tex_bind(t, 0x40859, 0x815804);
        tex_records(t, (const uint32_t[]){
            TEX_TXSIZE, size, TEX_TXLOD, 1,
            TEX_TXCLAMPSIZE, width << 13 | height,
        }, 3);
        tex_record(t, 0x18a, shapes[s].control);
        tex_ordinary_point(t, 0.875f);
        g_assert_cmphex(gl_window_pixel(t, 32, 32), ==, 0x102043);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void test_gl_texture_captured_volume_rejects_unknown_selector(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t image[16 * 16 * 4 * 4];

    for (uint32_t z = 0; z < 4; z++) {
        for (uint32_t i = 0; i < 16 * 16; i++) {
            uint32_t offset = (z * 16 * 16 + i) * 4;

            image[offset] = 16;
            image[offset + 1] = 32;
            image[offset + 2] = 64 + z;
            image[offset + 3] = 0xff;
        }
    }
    gl_transform_start(t);
    /* The 16x16 capture uses (1,1); this unobserved selector must be refused. */
    tex_load_captured_stock_volume(t, image, 16, 16, 2, 1);
    tex_captured_rect_volume_state(t, 16, 16, 4);
    tex_quad_volume_z(t, 0.875f);
    g_assert_cmphex(gl_window_pixel(t, 32, 24), ==, TEX_WHITE);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * A pixel-texture DrawPixels arrives as one pass per RE: on a two-RE board
 * pass 0 carries image rows 0 and 2 and pass 1 rows 1 and 3, each in a GE
 * box covering the whole image, and the passes together draw every row
 * once, as a one-RE board's single pass does. After pass 0 alone the odd
 * rows are still the clear colour; a model that drew each pass's rows one
 * after another, or refused the passes, fails here.
 */
static void test_gl_texture_pixel_texture_passes(gconstpointer data)
{
    static const uint8_t image[16] = {
        0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
        0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff,
    };
    static const uint32_t texels[4] = {
        0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
    };
    /* Rows 0 to 3: each row's two pixels pick texels by red and green. */
    static const uint8_t pixels[32] = {
        0x40, 0x40, 0x00, 0xff, 0xbf, 0x40, 0x40, 0xff,
        0x40, 0xbf, 0xbf, 0xff, 0xbf, 0xbf, 0xff, 0xff,
        0xbf, 0xbf, 0x00, 0xff, 0x40, 0xbf, 0x00, 0xff,
        0xbf, 0x40, 0x00, 0xff, 0x40, 0x40, 0x00, 0xff,
    };
    static const uint32_t expected[4][2] = {
        { 0xff0000, 0x00ff00 }, { 0x0000ff, 0xffff00 },
        { 0xffff00, 0x0000ff }, { 0x00ff00, 0xff0000 },
    };
    QTestState *t = texture_board_start(data);
    uint32_t res = tex_re_count(t);

    g_assert_true(res == 1 || res == 2);
    tex_load_captured_volume(t, image, 1);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | TEX_TEXMODE2_VOLUME);
    tex_record(t, TEX_TXSIZE, TEX_TXSIZE_2X2);
    tex_quad(t);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            g_assert_cmphex(gl_window_pixel(t, 24 + 16 * x, 20 + 8 * y), ==,
                            texels[y * 2 + x]);
        }
    }
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | TEX_TEXMODE2_VOLUME);
    tex_record(t, TEX_TXSIZE, TEX_TXSIZE_2X2);
    gl_clear_color(t, 0, 0, 0, 1);
    gl_word(t, GL_CLEAR_COLOR_BUFFER);
    for (uint32_t pass = 0; pass < res; pass++) {
        tex_pixel_texture_pass(t, res, pass, pixels, 4, TEX_MDDMA_PIXEL_ON,
                               TEX_MDDMA_PIXEL_OFF);
        for (uint32_t y = 0; y < 4; y++) {
            bool drawn = res == 1 || y % res <= pass;

            for (int x = 0; x < 2; x++) {
                g_assert_cmphex(gl_window_pixel(t, x, y), ==,
                                drawn ? expected[y][x] : 0);
            }
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * A two-RE board's second pass waiting for its image crosses a migration
 * and lands on the odd rows on the destination.
 */
static void test_gl_texture_pixel_texture_pass_migration(gconstpointer data)
{
    static const uint8_t image[16] = {
        0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
        0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff,
    };
    static const uint8_t pixels[16] = {
        0x40, 0x40, 0x00, 0xff, 0xbf, 0x40, 0x40, 0xff,
        0x40, 0xbf, 0xbf, 0xff, 0xbf, 0xbf, 0xff, 0xff,
    };
    static const uint32_t texels[4] = {
        0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-pixel-pass-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(tmpfs, "state", NULL);
    uri = g_strdup_printf("file:%s", path);
    args = g_strdup_printf(TEX_COLOR_TABLE_MIGRATION_ARGS, (char *)data);
    from = tex_start(args);
    g_assert_cmpuint(tex_re_count(from), ==, 2);
    tex_load_captured_volume(from, image, 1);
    tex_bind(from, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | TEX_TEXMODE2_VOLUME);
    tex_record(from, TEX_TXSIZE, TEX_TXSIZE_2X2);
    gl_clear_color(from, 0, 0, 0, 1);
    gl_word(from, GL_CLEAR_COLOR_BUFFER);
    tex_pixel_texture_pass(from, 2, 0, pixels, 2, TEX_MDDMA_PIXEL_ON,
                           TEX_MDDMA_PIXEL_OFF);
    tex_pixel_texture_pass_start(from, 2, 1, 2, TEX_MDDMA_PIXEL_ON);
    to = tex_sb_file_migrate_and_restart(from, args, uri);

    tex_pixel_texture_pass_finish(to, 2, 1, pixels, 2, TEX_MDDMA_PIXEL_OFF);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            g_assert_cmphex(gl_window_pixel(to, x, y), ==, texels[y * 2 + x]);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);
    qtest_quit(to);
    unlink(path);
    g_rmdir(tmpfs);
}

static void test_gl_texture_rectangular_volume_pixel_rgba(gconstpointer data)
{
    static const struct {
        uint32_t width, height, depth, pixel_control;
    } shapes[] = {
        /*
         * Captures 13173359/13173659 give 4x2x2 0x40449/0x40448;
         * 13180584/13180890 give 2x8x2 0x404c5/0x404c4. The earlier
         * 8x2x2 capture gives 0x4044d/0x4044c at 12988878/12989178,
         * and 8x2x4 gives 0x404084d/0x404084c at 13261039/13261339.
         */
        { 4, 2, 2, 0x40449 }, { 2, 4, 2, 0x40485 },
        { 8, 2, 2, 0x4044d }, { 2, 8, 2, 0x404c5 },
        { 8, 2, 4, 0x404084d },
        { 64, 16, 4, 0x08840919 }, { 64, 64, 4, 0x08840999 },
    };
    static const uint8_t coordinates[4][2] = {
        { 64, 64 }, { 192, 64 }, { 64, 192 }, { 192, 192 },
    };
    QTestState *t = texture_board_start(data);
    uint8_t image[64 * 64 * 4 * 4];
    uint8_t pixels[16];

    for (size_t s = 0; s < ARRAY_SIZE(shapes); s++) {
        uint32_t width = shapes[s].width;
        uint32_t height = shapes[s].height;
        uint32_t depth = shapes[s].depth;
        uint32_t enabled = shapes[s].pixel_control;
        uint32_t disabled = enabled & ~1u;

        for (uint32_t z = 0; z < depth; z++) {
            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    size_t index = ((z * height + y) * width + x) * 4;

                    image[index] = 16 + 20 * x;
                    image[index + 1] = 24 + 24 * y;
                    image[index + 2] = width == 64 ? 32 + 64 * z
                                                   : 32 + 40 * z;
                    image[index + 3] = 0xff;
                }
            }
        }
        if (width == 64) {
            tex_load_captured_stock_volume(t, image, width, height, 1,
                                           height == 16 ? 5 : 0x15);
        } else {
            tex_load_captured_rect_volume(t, image, width, height, depth,
                                          false);
        }
        tex_captured_rect_volume_state(t, width, height, depth);

        for (uint32_t z = 0; z < depth; z++) {
            for (uint32_t i = 0; i < ARRAY_SIZE(coordinates); i++) {
                pixels[4 * i] = coordinates[i][0];
                pixels[4 * i + 1] = coordinates[i][1];
                pixels[4 * i + 2] = depth == 2 ? 64 + 128 * z : 32 + 64 * z;
                pixels[4 * i + 3] = 0xff;
            }
            tex_volume_pixels_data(t, true, 0x00020002, pixels,
                                   enabled, disabled);
            uint32_t got[4];

            gl_window_quad(t, got);
            for (uint32_t i = 0; i < ARRAY_SIZE(coordinates); i++) {
                uint32_t x = coordinates[i][0] * width / 256;
                uint32_t y = coordinates[i][1] * height / 256;
                uint32_t blue = width == 64 ? 32 + 64 * z : 32 + 40 * z;
                uint32_t expected = ((16 + 20 * x) & 0xff) << 16 |
                                    ((24 + 24 * y) & 0xff) << 8 | blue;

                g_assert_cmphex(got[i], ==, expected);
            }
        }
    }

    /* An unsupported texture class leaves the same pixel source visible. */
    tex_captured_rect_volume_state(t, 8, 2, 4);
    tex_bind(t, 0x40e59, 0x815804);
    for (uint32_t i = 0; i < ARRAY_SIZE(coordinates); i++) {
        pixels[4 * i] = coordinates[i][0];
        pixels[4 * i + 1] = coordinates[i][1];
        pixels[4 * i + 2] = 224;
        pixels[4 * i + 3] = 0xff;
    }
    tex_volume_pixels_data(t, true, 0x00020002, pixels, 0x4040845,
                           0x4040844);
    uint32_t got[4];

    gl_window_quad(t, got);
    for (uint32_t i = 0; i < ARRAY_SIZE(coordinates); i++) {
        uint32_t source = pixels[4 * i] << 16 |
                          pixels[4 * i + 1] << 8 | pixels[4 * i + 2];

        g_assert_cmphex(got[i], ==, source);
    }

    /* The complete raw pixel control must match the captured value. */
    tex_captured_rect_volume_state(t, 8, 2, 4);
    tex_volume_pixels_data(t, true, 0x00020002, pixels, 0x4040847,
                           0x4040846);
    gl_window_quad(t, got);
    for (uint32_t i = 0; i < ARRAY_SIZE(coordinates); i++) {
        uint32_t source = pixels[4 * i] << 16 |
                          pixels[4 * i + 1] << 8 | pixels[4 * i + 2];

        g_assert_cmphex(got[i], ==, source);
    }

    /* A nonmatching clamp extent cannot select the stored volume. */
    tex_captured_rect_volume_state(t, 8, 2, 4);
    tex_record(t, TEX_TXCLAMPSIZE, 0x10004);
    tex_volume_pixels_data(t, true, 0x00020002, pixels, 0x4040845,
                           0x4040844);
    gl_window_quad(t, got);
    for (uint32_t i = 0; i < ARRAY_SIZE(coordinates); i++) {
        uint32_t source = pixels[4 * i] << 16 |
                          pixels[4 * i + 1] << 8 | pixels[4 * i + 2];

        g_assert_cmphex(got[i], ==, source);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static const uint8_t tex_four_slice_migration_image[64] = {
    0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
    0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff,
    0, 0xff, 0xff, 0xff, 0xff, 0, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0xff,
    0xff, 0x80, 0, 0xff, 0x80, 0, 0xff, 0xff,
    0, 0x80, 0xff, 0xff, 0x80, 0x80, 0x80, 0xff,
    0x20, 0x40, 0x60, 0xff, 0x60, 0x40, 0x20, 0xff,
    0xa0, 0x80, 0x60, 0xff, 0x60, 0x80, 0xa0, 0xff,
};

#define TEX_VOLUME_MIGRATION_ARGS \
    MIGRATION_ARGS ",product=si-t"

static void test_gl_texture_volume_depth4_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *path = NULL;
    g_autofree char *source_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-volume-migration-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(tmpfs, "state", NULL);
    uri = g_strdup_printf("file:%s", path);
    source_args = g_strdup(TEX_VOLUME_MIGRATION_ARGS);
    from = tex_start(source_args);

    /* Migrate after parsing the depth-four load packet, before its DMA. */
    tex_begin_captured_volume(from, 4);
    to = tex_sb_file_migrate_and_restart(from,
                                            source_args, uri);
    tex_finish_captured_volume(to, tex_four_slice_migration_image, 4);
    tex_captured_four_slice_state(to);
    tex_quad_volume_z(to, 0.875f);
    {
        static const uint32_t expected[4] = {
            0x204060, 0x604020, 0xa08060, 0x6080a0,
        };

        for (int i = 0; i < 4; i++) {
            g_assert_cmphex(gl_window_pixel(to, 24 + 16 * (i % 2),
                                            20 + 8 * (i / 2)), ==,
                            expected[i]);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    unlink(path);
    g_rmdir(tmpfs);
}

static void test_gl_texture_rectangular_volume_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *path = NULL;
    g_autofree char *source_args = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint8_t image[4 * 2 * 2 * 4];

    tmpfs = g_dir_make_tmp("sn0-mgras-rect-volume-migration-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(tmpfs, "state", NULL);
    uri = g_strdup_printf("file:%s", path);
    source_args = g_strdup(TEX_VOLUME_MIGRATION_ARGS);
    from = tex_start(source_args);

    for (uint32_t z = 0; z < 2; z++) {
        for (uint32_t y = 0; y < 2; y++) {
            for (uint32_t x = 0; x < 4; x++) {
                uint32_t i = ((z * 2 + y) * 4 + x) * 4;

                image[i] = 32 + x * 48;
                image[i + 1] = 48 + y * 128;
                image[i + 2] = 64 + z * 128;
                image[i + 3] = 0xff;
            }
        }
    }
    gl_transform_start(from);
    tex_load_captured_rect_volume(from, image, 4, 2, 2, false);
    to = tex_sb_file_migrate_and_restart(from,
                                            source_args, uri);

    gl_clear_color(to, 0, 0, 0, 0);
    gl_word(to, GL_CLEAR_COLOR_BUFFER);
    for (uint32_t z = 0; z < 2; z++) {
        tex_captured_rect_volume_state(to, 4, 2, 2);
        tex_quad_volume_z(to, z ? 0.75f : 0.25f);
        for (uint32_t y = 0; y < 2; y++) {
            uint32_t py = 16 + (2 * y + 1) * 8 / 2;
            g_autofree uint32_t *got = gl_window_row(to, 16, py, 32);

            for (uint32_t x = 0; x < 4; x++) {
                uint32_t px = 16 + (2 * x + 1) * 16 / 4;
                uint32_t expected = (32 + x * 48) << 16 |
                                    (48 + y * 128) << 8 | (64 + z * 128);

                g_assert_cmphex(got[px - 16], ==, expected);
            }
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    unlink(path);
    g_rmdir(tmpfs);
}

static void test_gl_texture_captured_volume_64k_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *path = NULL;
    g_autofree char *source_args = NULL;
    g_autofree uint8_t *image = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-volume-64k-migration-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(tmpfs, "state", NULL);
    uri = g_strdup_printf("file:%s", path);
    source_args = g_strdup(TEX_VOLUME_MIGRATION_ARGS);
    from = tex_start(source_args);

    image = g_malloc(64 * 64 * 4 * 4);
    for (uint32_t z = 0; z < 4; z++) {
        for (uint32_t i = 0; i < 64 * 64; i++) {
            uint32_t offset = (z * 64 * 64 + i) * 4;

            image[offset] = 16;
            image[offset + 1] = 32;
            image[offset + 2] = 64 + z;
            image[offset + 3] = 0xff;
        }
    }
    gl_transform_start(from);
    tex_load_captured_stock_volume(from, image, 64, 64, 1, 0x15);
    tex_captured_rect_volume_state(from, 64, 64, 4);
    tex_quad_volume_z(from, 0.875f);
    g_assert_cmphex(gl_window_pixel(from, 32, 24), ==, 0x102043);
    to = tex_sb_file_migrate_and_restart(from,
                                            source_args, uri);

    tex_captured_rect_volume_state(to, 64, 64, 4);
    tex_quad_volume_z(to, 0.875f);
    g_assert_cmphex(gl_window_pixel(to, 32, 24), ==, 0x102043);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    unlink(path);
    g_rmdir(tmpfs);
}

static void test_gl_texture_volume_depth4_reset_load(void)
{
    QTestState *t = texture_board_start("si-t");

    tex_begin_captured_volume(t, 4);
    qtest_system_reset(t);
    gl_enter_slot(t, GL_SLOT_A);
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
    tex_captured_four_slice_state(t);
    tex_quad_volume_z(t, 0.875f);
    g_assert_cmphex(gl_window_pixel(t, 24, 20), ==, TEX_WHITE);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * The model keeps an RGBA4 level by rounding each 8-bit component to the
 * nearest of 15, round(v * 15 / 255), and draws it widened by
 * replication, its own choice (hw/misc/sgi-mgras-texture.c). The
 * components chosen tell it from truncation, v >> 4 (0x0f, 0x18, 0x2a,
 * 0x7f, 0xf8), and from (v + 8) >> 4 (0x18, 0xc0). A dark near-grey texel
 * (0x10, 0x0c, 0x0c) stays grey, where truncation made it red. The
 * traced texels, 0x10 + i, 0x40 + i and 0x80 + i, come out 0x11, 0x44,
 * 0x88 under all three.
 */
static uint32_t tex_round4(uint8_t v)
{
    return (v * 15 + 127) / 255 * 0x11;
}

static void test_gl_texture_rgba4_rounds(gconstpointer data)
{
    static const uint8_t comp[] = {
        0x0f, 0x18, 0x7f, 0xc0, 0xf8, 0x00, 0xff, 0x2a,
    };
    QTestState *t = texture_board_start(data);
    uint8_t texels[32];

    for (int i = 0; i < 8; i++) {
        texels[4 * i] = comp[i];
        texels[4 * i + 1] = comp[(i + 3) % 8];
        texels[4 * i + 2] = comp[(i + 5) % 8];
        texels[4 * i + 3] = 0xc0 + i;
    }
    texels[0] = 0x10;
    texels[1] = 0x0c;
    texels[2] = 0x0c;
    tex_load(t, TEX_TL_MODE_RGBA4, texels);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA4);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, 0x111111);
    for (int i = 0; i < 8; i++) {
        uint32_t want = 0;

        for (int k = 0; k < 3; k++) {
            want = want << 8 | tex_round4(texels[4 * i + k]);
        }
        g_assert_cmphex(tex_texel_pixel(t, i % 4, i / 4), ==, want);
    }

    for (int i = 0; i < 8; i++) {
        texels[4 * i] = 0x10 + i;
        texels[4 * i + 1] = 0x40 + i;
        texels[4 * i + 2] = 0x80 + i;
        texels[4 * i + 3] = 0xc0 + i;
    }
    tex_load(t, TEX_TL_MODE_RGBA4, texels);
    gl_word(t, TEX_ON);
    tex_record(t, TEX_TEXMODE1, TEX_TEXMODE1_ON);
    tex_record(t, TEX_TXSIZE, TEX_TXSIZE_4X2);
    tex_quad(t);
    for (int i = 0; i < 8; i++) {
        g_assert_cmphex(tex_texel_pixel(t, i % 4, i / 4), ==, 0x114488);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * The level's size is TL_SPEC's, checked against the size word, and the
 * destination corners are not read: the traced 0, 0, 4, 2 loads a 4-by-2
 * level (the image path would take 5 by 3 from them), so do inclusive
 * corners, and a size word that disagrees with TL_SPEC stores nothing,
 * so the quad draws untextured.
 */
static void test_gl_texture_size_from_tl_spec(void)
{
    static const uint32_t inclusive[6] = { 0, 0, 3, 1, 2 << 16 | 4, 1 };
    static const uint32_t swapped[6] = { 0, 0, 4, 2, 4 << 16 | 2, 0 };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32];

    tex_texels(texels);
    tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_4X2, texels, 4, 2,
                inclusive, true);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 3, 1), ==, 0x730087);
    qtest_quit(t);

    t = tex_start(TEX_ARGS);
    tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_4X2, texels, 4, 2,
                swapped, true);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, TEX_WHITE);
    g_assert_cmphex(tex_texel_pixel(t, 3, 1), ==, TEX_WHITE);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * TL_MODE is state: a reload of the same image without it, as the
 * client sends one, keeps the level in the kept format the earlier load
 * named. Another context under a base of its own, as the kernel gives
 * each, finds nothing loaded there, and the first still draws its own
 * after switching back. A 0x9c00 without
 * its 0x9b00 and a 0x511a transfer outside a bracket are logged, not
 * refused.
 */
static void test_gl_texture_tl_mode_persists(void)
{
    static const uint32_t box[6] = { 0, 0, 4, 2, 2 << 16 | 4, 0 };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32];

    tex_texels(texels);
    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    texels[0] = 0x55;
    texels[1] = 0x66;
    texels[2] = 0x77;
    gl_word(t, TEX_LOAD_CLOSE);
    tex_load_as(t, false, 0, TEX_SPEC_4X2, texels, 4, 2, box, false);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, 0x556677);
    g_assert_cmphex(tex_texel_pixel(t, 1, 0), ==, 0x13c081);

    gl_enter_slot(t, GL_SLOT_B);
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
    gl_one(t, 0x0000eb04, 8);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, TEX_WHITE);

    gl_enter_slot(t, GL_SLOT_A);
    gl_word(t, TEX_ON);
    tex_record(t, TEX_TEXMODE1, TEX_TEXMODE1_ON);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, 0x556677);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * What the model does not draw stays untextured rather than drawn wrong:
 * TEXMODE1 bit 0 clear, only one of the two mipmap bits (5 or 19 of
 * TEXMODE2),
 * a detail texture (bit 9), environment code 3 on a colour class, a
 * level kept at depth code 3, which is not defined and not stored, and
 * 0x0c00 after the bind. Each quad is white.
 */
static void test_gl_texture_unsupported_draws_untextured(void)
{
    static const uint32_t cases[][3] = {
        { TEX_TL_MODE_RGBA8, TEX_TEXMODE1_ON & ~1u, TEX_TEXMODE2_RGBA8 },
        { TEX_TL_MODE_RGBA8, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | 0x20 },
        { TEX_TL_MODE_RGBA8, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | 0x80000 },
        { TEX_TL_MODE_RGBA8, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | 0x200 },
        { TEX_TL_MODE_RGBA8, TEX_TEXMODE1_ON | 6, TEX_TEXMODE2_RGBA8 },
        { 0x34066, TEX_TEXMODE1_ON, 0x1000c },
    };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32];

    tex_texels(texels);
    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        gl_word(t, GL_CONTEXT_OPENER);
        gl_viewport(t, 0, 0, 128, 128);
        gl_window_projection(t);
        tex_load(t, cases[i][0], texels);
        tex_bind(t, cases[i][1], cases[i][2]);
        tex_quad(t);
        g_assert_cmphex(tex_texel_pixel(t, 1, 1), ==, TEX_WHITE);
    }
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    gl_word(t, TEX_OFF);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 1, 1), ==, TEX_WHITE);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * The load's GE words at 0xe3d for a one-level texture at TXMIPMAP word
 * block, as perfly's client sent them: the width, 0, the level, the word,
 * the word less one and a sixth word the model does not read.
 */
static void tex_load_at(QTestState *t, uint32_t block, const uint8_t *texels)
{
    const uint32_t words[6] = { 4, 0, 0, block, block - 1, 0 };

    gl_ge_load(t, GL_GE_ERAM, 0xe3d, words, 6);
    tex_load(t, TEX_TL_MODE_RGBA8, texels);
}

/* Point TXMIPMAP entry 0 at block, as the draw setup does. */
static void tex_mipmap_word(QTestState *t, uint32_t block)
{
    tex_record(t, TEX_TXADDR, 0);
    gl_word(t, TEX_TABLE);
    gl_word(t, 0xc52);
    gl_word(t, 0xc53);
    gl_word(t, TEX_TXMIPMAP);
    gl_word(t, block);
}

/* Each texel of tex_texels() drawn as its own R, G and B. */
static void tex_expect_texels(QTestState *t, const uint8_t texels[32])
{
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 4; x++) {
            const uint8_t *p = &texels[4 * (4 * y + x)];

            g_assert_cmphex(tex_texel_pixel(t, x, y), ==,
                            p[0] << 16 | p[1] << 8 | p[2]);
        }
    }
}

/*
 * The fifth of the load's GE words at 0xe3d is the level's TXBORDER word,
 * not a sign of a border: the traced one-level value, the first block
 * less one, and the words perfly's unbordered mipmapped levels carried
 * (TXMIPMAP 0xc with TXBORDER 0x10, 0x111 with 0x13, 0x14 with 0x15)
 * all draw the level, each through the TXMIPMAP word its load named.
 */
static void test_gl_texture_txborder_word_is_not_a_border(void)
{
    static const struct {
        uint32_t block;
        uint32_t border;
    } cases[] = {
        { 0, 0xffffffff },
        { 0, 0 },
        { 3, 2 },
        { 0xc, 0x10 },
        { 0x111, 0x13 },
        { 0x14, 0x15 },
    };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32];

    tex_texels(texels);
    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        const uint32_t words[6] = { 4, 0, 0, cases[i].block,
                                    cases[i].border, 0 };

        gl_word(t, GL_CONTEXT_OPENER);
        gl_viewport(t, 0, 0, 128, 128);
        gl_window_projection(t);
        gl_ge_load(t, GL_GE_ERAM, 0xe3d, words, 6);
        tex_load(t, TEX_TL_MODE_RGBA8, texels);
        tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
        tex_mipmap_word(t, cases[i].block);
        tex_quad(t);
        g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==,
                        texels[0] << 16 | texels[1] << 8 | texels[2]);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * A bordered image, as distort loads its levels: sent at x = y = -1 with
 * the level's size plus two each way. A 6-by-4 image at -1, -1 into a
 * 4-by-2 level stores the 4-by-2 interior and drops the border ring
 * (approximated: what the board does with border texels is unknown), so
 * the draw shows the interior, not the ring's 0x5a, and not untextured
 * white as when the model refused the load.
 */
static void test_gl_texture_bordered_image_stores_interior(void)
{
    static const uint32_t bordered[6] = {
        0xffffffff, 0xffffffff, 4, 2, 4 << 16 | 6, 0,
    };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32], image[96];

    tex_texels(texels);
    memset(image, 0x5a, sizeof(image));
    for (int y = 0; y < 2; y++) {
        memcpy(&image[4 * (6 * (y + 1) + 1)], &texels[16 * y], 16);
    }
    tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_4X2, image, 6, 4,
                bordered, true);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    tex_expect_texels(t, texels);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * A bordered level loaded as strips, as distort's 512-wide level is: the
 * strips run -1 and on, then the interior, then the last border column,
 * and a row of border above and below. The three strips of a 4-by-2
 * level at x = -1 (3 wide), 2 (2 wide) and 4 (1 wide), then a border
 * row at y = 2 and at y = -1, leave the interior whole: the border-only
 * strips store nothing and a strip keeps only the part inside the level.
 */
static void test_gl_texture_bordered_strips(void)
{
    static const struct {
        int32_t x, y;
        uint32_t w, h;
    } strips[] = {
        { -1, 0, 3, 2 }, { 2, 0, 2, 2 }, { 4, 0, 1, 2 },
        { 0, 2, 4, 1 }, { 0, -1, 4, 1 },
    };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32], image[32];

    tex_texels(texels);
    gl_word(t, TEX_LOAD_OPEN);
    for (int k = 0; k < ARRAY_SIZE(strips); k++) {
        const uint32_t box[6] = {
            strips[k].x, strips[k].y, strips[k].w, strips[k].h,
            strips[k].h << 16 | strips[k].w, 0,
        };

        memset(image, 0x5a, sizeof(image));
        for (int y = 0; y < strips[k].h; y++) {
            for (int x = 0; x < strips[k].w; x++) {
                int tx = strips[k].x + x, ty = strips[k].y + y;

                if (tx >= 0 && tx < 4 && ty >= 0 && ty < 2) {
                    memcpy(&image[4 * (strips[k].w * y + x)],
                           &texels[4 * (4 * ty + tx)], 4);
                }
            }
        }
        tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_4X2, image,
                    strips[k].w, strips[k].h, box, false);
    }
    gl_word(t, TEX_LOAD_CLOSE);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    tex_expect_texels(t, texels);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * Two textures of one context, loaded to blocks 0 and 4 as perfly's
 * CH-53 rotor and body were, are both kept: a draw takes the level its
 * TXMIPMAP word names, in either order, and a word nothing was loaded
 * to draws untextured.
 */
static void test_gl_texture_levels_by_block(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t first[32], second[32];

    tex_texels(first);
    for (int i = 0; i < 32; i++) {
        second[i] = i % 4 == 3 ? 0xff : first[i] ^ 0xa0;
    }
    tex_load_at(t, 0, first);
    tex_load_at(t, 4, second);
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < 2; k++) {
            uint32_t block = (k ^ pass) ? 4 : 0;
            const uint8_t *p = block ? second : first;

            tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
            tex_mipmap_word(t, block);
            tex_quad(t);
            g_assert_cmphex(tex_texel_pixel(t, 3, 1), ==,
                            p[28] << 16 | p[29] << 8 | p[30]);
        }
    }
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_mipmap_word(t, 8);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 3, 1), ==, TEX_WHITE);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * A context holds as many textures as TRAM has room for, not a fixed
 * count of levels. perfly's esprit and iris scenes and pointblank load
 * more than sixteen levels into one context (esprit: two 7-level pyramids
 * and a 9-level one, 23 levels in all), far below one TRAM's 4 MiB; a
 * store of sixteen levels a context refused the seventeenth and the
 * Performer logo drew white. Twenty one-level textures at blocks 0 to 19
 * each draw their own texels.
 */
static void test_gl_texture_many_levels_in_one_context(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[20][32];

    for (int k = 0; k < 20; k++) {
        tex_texels(texels[k]);
        for (int i = 0; i < 32; i++) {
            if (i % 4 != 3) {
                texels[k][i] ^= 4 * k;
            }
        }
        tex_load_at(t, k, texels[k]);
    }
    for (int k = 0; k < 20; k++) {
        const uint8_t *p = &texels[k][28];

        tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
        tex_mipmap_word(t, k);
        tex_quad(t);
        g_assert_cmphex(tex_texel_pixel(t, 3, 1), ==,
                        p[0] << 16 | p[1] << 8 | p[2]);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/* The HQ pixel-format word, sent as 0x000c0004 before 0x8d20. */
#define TEX_HQ_PIXEL_FORMAT   0x000c0004
#define TEX_HQ_PIXEL_REST     0xe00
/* GL_ABGR_EXT of one-byte and of two-byte components. */
#define TEX_HQ_ABGR_8         0xe8d
#define TEX_HQ_ABGR_16        0xe8e
/* The unpack block's sixth word: 1/255 for bytes, 1/65535 for shorts. */
#define TEX_UNPACK_SCALE_8    0x3b808081
#define TEX_UNPACK_SCALE_16   0x37800080

/*
 * A 4-by-2 RGBA8 level 0 loaded from an image of bpt bytes a texel, the
 * client's four-component program with unpack scale word scale and the
 * HQ pixel-format word hq in effect for the transfer. The words are
 * tex_load_program's; only the unpack block's scale and the HQ word
 * differ, as atlantis (0xe8d, 1/255), buttonfly and blast (0xe8e and
 * 0xe00, 1/65535) sent them on SI+T.
 */
static void tex_load_4x2_as(QTestState *t, uint32_t hq, uint32_t scale,
                            const uint8_t *image, uint32_t bpt)
{
    static const uint32_t program[8] = {
        0x1f44, 0, 0x3e65, 0xd91, 0x156a, 0x1b4, 0x352d, 0xdf3,
    };
    static const uint32_t box[6] = { 0, 0, 4, 2, 2 << 16 | 4, 0 };
    static const uint32_t draw[8] = { 0, 0, 0, 1, 0, 1, 0x511a, 0x98 };
    uint32_t bytes = 4 * 2 * bpt;
    uint32_t unpack[6] = { bytes / 4, 0x4064, 0x464, 0xb, 0xc, scale };
    float z[2] = { 0, 1 };
    uint32_t zooms[2];

    memcpy(zooms, z, sizeof(zooms));
    tex_record(t, TEX_TL_MODE, TEX_TL_MODE_RGBA8);
    tex_record(t, TEX_TL_SPEC, TEX_SPEC_4X2);
    gl_word(t, TEX_SAVE_RSS);
    gl_word(t, 0xffffffff);
    gl_word(t, 0x1400000);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 3);
    gl_word(t, TEX_LOAD_OPEN);
    gl_ge_load(t, GL_GE_CRAM, GL_GE_PROGRAM, program, 8);
    gl_ge_load(t, GL_GE_ERAM, GL_GE_UNPACK, unpack, 6);
    gl_ge_load(t, GL_GE_CRAM, GL_GE_ZOOM, zooms, 2);
    gl_ge_load(t, GL_GE_ERAM, GL_GE_BOX, box, 6);
    gl_word(t, TEX_PAIRS);
    gl_word(t, 1);
    gl_word(t, 2);
    gl_word(t, GL_INLINE_BLOCK | 8);
    gl_word(t, 0x159);
    gl_word(t, 0xc00080);
    gl_one(t, TEX_HQ_PIXEL_FORMAT, hq);
    gl_word(t, GL_PIXELS_HDR);
    gl_word(t, bytes / 4);
    for (int i = 1; i < 8; i++) {
        gl_word(t, draw[i]);
    }
    gl_pixels_dma_on(t, 0, image, bytes, bytes, 1, bytes, 0xa7);
    gl_one(t, TEX_HQ_PIXEL_FORMAT, TEX_HQ_PIXEL_REST);
    gl_word(t, TEX_LOAD_CLOSE);
    gl_word(t, TEX_RESTORE_RSS);
    tex_record(t, TEX_TXSIZE, 0);
}

/*
 * GL_ABGR_EXT of unsigned bytes, as atlantis and mirrors load their
 * textures: the HQ word 0xe8d, the four-byte program and four host bytes
 * a texel in the order alpha, blue, green, red (EXT_abgr), the order the
 * pixel path already draws for the same word (gl-pixels-dma-abgr). The
 * level must hold the image's colours, not the reversed ones; a load
 * the model refused left atlantis's 128-by-128 skin untextured.
 */
static void test_gl_texture_load_abgr_bytes(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32], image[32];

    tex_texels(texels);
    for (int i = 0; i < 8; i++) {
        for (int k = 0; k < 4; k++) {
            image[4 * i + k] = texels[4 * i + 3 - k];
        }
    }
    tex_load_4x2_as(t, TEX_HQ_ABGR_8, TEX_UNPACK_SCALE_8, image, 4);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    tex_expect_texels(t, texels);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * An RGBA image of unsigned shorts, as blast loads every texture: the
 * unpack block's scale word is 1/65535 rather than 1/255 and the
 * transfer carries two big-endian bytes a component, eight a texel. Each
 * short is the texel byte times 257, so any reduction to eight bits that
 * OpenGL 1.2.1 allows (c * 255 / 65535, rounded or truncated, or the high
 * byte) gives the byte back. The model sizes the transfer by the kept
 * format's bytes, refused it ("DMA of 1 rows of 64 bytes for a texture
 * level of 32 bytes") and drew white.
 */
static void test_gl_texture_load_unsigned_shorts(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32], image[64];

    tex_texels(texels);
    for (int i = 0; i < 32; i++) {
        stw_be_p(&image[2 * i], texels[i] * 0x101);
    }
    tex_load_4x2_as(t, TEX_HQ_PIXEL_REST, TEX_UNPACK_SCALE_16, image, 8);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    tex_expect_texels(t, texels);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * GL_ABGR_EXT of unsigned shorts, as buttonfly, distort and blast load
 * their textures: HQ word 0xe8e (bits 1:0 the client's bytes an element,
 * 2), scale 1/65535, eight host bytes a texel, alpha, blue, green, red.
 */
static void test_gl_texture_load_abgr_shorts(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32], image[64];

    tex_texels(texels);
    for (int i = 0; i < 8; i++) {
        for (int k = 0; k < 4; k++) {
            stw_be_p(&image[8 * i + 2 * k], texels[4 * i + 3 - k] * 0x101);
        }
    }
    tex_load_4x2_as(t, TEX_HQ_ABGR_16, TEX_UNPACK_SCALE_16, image, 8);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    tex_expect_texels(t, texels);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * The reduction of a two-byte component to the kept eight bits is
 * round(c * 255 / 65535) = (c + 128) / 257 (approximated: the board's own
 * conversion is inferred). 0x0080 is the last value that rounds to 0 and
 * 0x0081 the first that rounds to 1, which the high byte alone would
 * not tell apart; 0x0182 rounds to 2 where truncating c * 255 / 65535
 * gives 1.
 */
static void test_gl_texture_shorts_reduce_rounded(void)
{
    static const uint16_t value[4] = { 0x0080, 0x0081, 0x017f, 0x0182 };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t image[64];

    for (int i = 0; i < 8; i++) {
        for (int k = 0; k < 4; k++) {
            stw_be_p(&image[8 * i + 2 * k], k == 3 ? 0xffff : value[i % 4]);
        }
    }
    tex_load_4x2_as(t, TEX_HQ_PIXEL_REST, TEX_UNPACK_SCALE_16, image, 8);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 4; x++) {
            uint32_t v = (value[x] + 128) / 257;

            g_assert_cmphex(tex_texel_pixel(t, x, y), ==,
                            v << 16 | v << 8 | v);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * An RGB image of unsigned shorts (blast's GL_RGB textures: unpack
 * routine 0x40f2, scale 1/65535, six bytes a texel) lands in an RGBA
 * level with alpha 1, as the byte image does.
 */
static void test_gl_texture_load_rgb_shorts(void)
{
    static const uint32_t box[6] = { 0, 0, 4, 2, 2 << 16 | 4, 0 };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32], image[48];

    tex_texels(texels);
    for (int i = 0; i < 8; i++) {
        for (int k = 0; k < 3; k++) {
            stw_be_p(&image[6 * i + 2 * k], texels[4 * i + k] * 0x101);
        }
    }
    tex_load_program(t, GL_PIX_3UB, true, TEX_TL_MODE_RGBA8, TEX_SPEC_4X2,
                     image, 4, 2, 3, 6, box, true, 0, 0);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    tex_expect_texels(t, texels);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}


/*
 * A level loaded as strips inside one bracket, as perfly's client loads
 * its 256-wide levels 64 texels at a time: each transfer carries its
 * strip's x and y at GE parameters 0xe34 and 0xe35 and its size in the
 * size word, and lands there in the level. The two 2-by-2 strips of a
 * 4-by-2 level, sent right one first, make the whole level; a strip that
 * would run past the level's edge and its one-texel border (here by two
 * texels) stores nothing and leaves it whole.
 */
static void test_gl_texture_level_from_strips(void)
{
    static const uint32_t past[6] = { 3, 0, 3, 2, 2 << 16 | 3, 0 };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32], strip[2][16], other[24];

    tex_texels(texels);
    memset(other, 0x5a, sizeof(other));
    for (int k = 0; k < 2; k++) {
        for (int y = 0; y < 2; y++) {
            memcpy(&strip[k][8 * y], &texels[4 * (4 * y + 2 * k)], 8);
        }
    }
    gl_word(t, TEX_LOAD_OPEN);
    for (int k = 1; k >= 0; k--) {
        const uint32_t box[6] = { 2 * k, 0, 2, 2, 2 << 16 | 2, 0 };

        tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_4X2, strip[k], 2, 2,
                    box, false);
    }
    gl_word(t, TEX_LOAD_CLOSE);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 4; x++) {
            const uint8_t *p = &texels[4 * (4 * y + x)];

            g_assert_cmphex(tex_texel_pixel(t, x, y), ==,
                            p[0] << 16 | p[1] << 8 | p[2]);
        }
    }
    tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_4X2, other, 3, 2, past,
                true);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 3, 1), ==, 0x730087);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * An RGB image of unsigned bytes, loaded through the three-byte program
 * into a level kept as RGBA4, as perfly's CH-53 body texture was: three
 * host bytes a texel, each kept at four bits, and alpha 1, so blending
 * by source alpha over black shows the texel's colour, not black.
 */
static void test_gl_texture_rgb_image_into_rgba(void)
{
    static const uint32_t box[6] = { 0, 0, 4, 2, 2 << 16 | 4, 0 };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32], rgb[24];

    tex_texels(texels);
    for (int i = 0; i < 8; i++) {
        memcpy(&rgb[3 * i], &texels[4 * i], 3);
    }
    tex_load_program(t, GL_PIX_3UB, true, TEX_TL_MODE_RGBA4, TEX_SPEC_4X2,
                     rgb, 4, 2, 3, 3, box, true, 0, 0);
    gl_clear_color(t, 0, 0, 0, 1);
    gl_word(t, GL_CLEAR_COLOR_BUFFER);
    gl_blend(t, GL_SRC_ALPHA, GL_SRC_ALPHA + 1, GL_FUNC_ADD);
    gl_one(t, GL_BLEND_HDR, 1);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA4);
    tex_quad(t);
    for (int i = 0; i < 8; i++) {
        uint32_t want = 0;

        for (int k = 0; k < 3; k++) {
            want = want << 8 | tex_round4(rgb[3 * i + k]);
        }
        g_assert_cmphex(tex_texel_pixel(t, i % 4, i / 4), ==, want);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * The SI has no TRAM: the same words draw the quad white, the records'
 * block and the transfer are consumed, and the fence after them parses.
 */
static void test_gl_texture_textureless_board_ignores(void)
{
    QTestState *t = tex_start(MACHINE_ARGS);
    uint8_t texels[32];

    tex_texels(texels);
    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, TEX_WHITE);
    gl_pixels_fence(t);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * A level's texels come from DMA pages on node 1, the page list naming
 * the frame as (1 << 20) | f as for a display list (gl-list-page-on-
 * another-node); node 0 holds 0xee bytes at the same offset. A transfer
 * that dropped the node bits would draw the 0xee bytes.
 */
#define TEX_NODE_ARGS \
    "-M origin2000,topology=origin2000-deskside,nodes=2 -smp 4 -m 128M " \
    "-device sgi-mgras,slot=io3,product=si-t"

static void test_gl_texture_load_from_another_node(void)
{
    static const uint32_t box[6] = { 0, 0, 4, 2, 2 << 16 | 4, 0 };
    QTestState *t = qtest_init(TEX_NODE_ARGS);
    uint8_t texels[32];

    sn0_program_nasid(t, 1);
    gl_enter_slot(t, GL_SLOT_A);
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
    tex_texels(texels);
    tex_load_full(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_4X2, texels, 4, 2, box,
                  true, 1, 0);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 4; x++) {
            const uint8_t *p = &texels[4 * (4 * y + x)];

            g_assert_cmphex(tex_texel_pixel(t, x, y), ==,
                            p[0] << 16 | p[1] << 8 | p[2]);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/* Context slot, with a viewport and projection, ready to load and draw. */
static void tex_enter(QTestState *t, uint32_t slot)
{
    gl_enter_slot(t, slot);
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
}

/*
 * The context setup's 32 detail-texture table writes, 0xec10 with 0xc52,
 * 0xc78, 0x194 and a value (0x1f three times, then 0x1d down to 1), are
 * texture-only defaults and consumed without a log line; any other value
 * is logged. Either way the words after them parse as headers.
 */
#define GL_TEXTURE_TABLE_HDR  0x0000ec10

static void gl_texture_table(QTestState *qts, uint32_t value)
{
    gl_word(qts, GL_TEXTURE_TABLE_HDR);
    gl_word(qts, 0xc52);
    gl_word(qts, 0xc78);
    gl_word(qts, 0x194);
    gl_word(qts, value);
}

static void test_gl_texture_table_defaults_are_silent(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *log = NULL;
    g_autofree char *args = NULL;
    g_autofree char *contents = NULL;
    GError *err = NULL;
    QTestState *t;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-log-XXXXXX", &err);
    g_assert_no_error(err);
    log = g_strdup_printf("%s/unimp.log", tmpfs);
    args = g_strdup_printf(MIGRATION_ARGS " -d unimp -D %s", log);
    t = qtest_init(args);

    gl_enter_slot(t, GL_SLOT_A);
    gl_word(t, GL_CONTEXT_OPENER);
    for (int i = 0; i < 3; i++) {
        gl_texture_table(t, 0x1f);
    }
    for (uint32_t v = 0x1d; v >= 1; v--) {
        gl_texture_table(t, v);
    }
    gl_depth_range(t, 0.5f, 1);
    g_assert_cmphex(gl_query(t, GL_Q_DEPTH_RANGE, 1), ==, gl_bits(0.5f));
    g_assert_true(g_file_get_contents(log, &contents, NULL, &err));
    g_assert_no_error(err);
    g_assert_null(strstr(contents, "texture table"));
    g_assert_null(strstr(contents, "reg 0xec"));
    g_free(contents);

    gl_texture_table(t, 0x1e);
    gl_depth_range(t, 0.25f, 1);
    g_assert_cmphex(gl_query(t, GL_Q_DEPTH_RANGE, 1), ==, gl_bits(0.25f));
    g_assert_true(g_file_get_contents(log, &contents, NULL, &err));
    g_assert_no_error(err);
    g_assert_nonnull(strstr(contents, "texture table write 0xc52 0xc78 "
                            "0x194 0x1e"));

    qtest_quit(t);
    unlink(log);
    g_rmdir(tmpfs);
}

static void test_hq_query_answers_the_texture_matrix(void)
{
    QTestState *test = qtest_init(MACHINE_ARGS);

    qts = test;
    mgras_reset_state(test);

    gl_enter_slot(qts, GL_SLOT_A);
    gl_word(qts, GL_CONTEXT_OPENER);
    g_assert_cmphex(gl_query(qts, GL_Q_TEXTURE, 0), ==, gl_bits(1));
    g_assert_cmphex(gl_query(qts, GL_Q_TEXTURE + 12, 0), ==, gl_bits(0));
    g_assert_cmphex(gl_query(qts, GL_Q_TEXTURE_DEPTH, 1), ==, 0xb45);
    gl_one(qts, GL_MATRIX_MODE_HDR, GL_TEXTURE);
    gl_word(qts, GL_PUSH_MATRIX);
    gl_three(qts, GL_TRANSLATE_HDR, 0.5f, 0.25f, 0);
    g_assert_cmphex(gl_query(qts, GL_Q_TEXTURE + 12, 0), ==, gl_bits(0.5f));
    g_assert_cmphex(gl_query(qts, GL_Q_TEXTURE + 13, 0), ==, gl_bits(0.25f));
    g_assert_cmpuint(gl_stack_depth(gl_query(qts, GL_Q_TEXTURE_DEPTH, 1),
                                    0xb45), ==, 2);
    g_assert_cmphex(gl_query(qts, GL_Q_MODELVIEW + 12, 0), ==, gl_bits(0));
    gl_word(qts, GL_POP_MATRIX);
    g_assert_cmphex(gl_query(qts, GL_Q_TEXTURE + 12, 0), ==, gl_bits(0));
    g_assert_cmphex(gl_query(qts, GL_Q_TEXTURE_DEPTH, 1), ==, 0xb45);

    qtest_quit(test);
    qts = NULL;
}

/*
 * The next increment: formats, environments, wrap, filtering and
 * mipmaps (hw/misc/sgi-mgras-texture.c). A scene opens a fresh context,
 * loads one level 0, binds it as the IRIX client does for the given
 * kept format, class, environment and TEXMODE2 bits, and draws a quad
 * of fragment colour frag. Registers in extra go out after the bind, so
 * they override it. Where a case wants the alpha of the stored colour it
 * blends over white: with a red fragment the green channel is then one
 * minus the alpha.
 */
#ifndef GL_ONE_MINUS_SRC_ALPHA
#define GL_ONE_MINUS_SRC_ALPHA 0x303
#endif
#define TEX_ENV_MODULATE      0
#define TEX_ENV_DECAL         1
#define TEX_ENV_BLEND         2
#define TEX_ENV_ALPHA         3
#define TEX_CLASS_ALPHA       1
#define TEX_CLASS_LUMINANCE   2
#define TEX_CLASS_INTENSITY   3
#define TEX_CLASS_COLOUR      4
#define TEX_LAYOUT_ONE        0
#define TEX_LAYOUT_LA         1
#define TEX_LAYOUT_RGB        2
#define TEX_LAYOUT_RGBA       3

typedef struct TexScene {
    uint32_t layout, depth, cls, env;
    uint32_t mode2;             /* TEXMODE2 bits beyond the depth */
    uint32_t w, h;              /* level 0, 4 by 2 when 0 */
    const uint8_t *texels;
    float frag[4];              /* white when all 0 */
    bool over_white;
    uint32_t extra[8][2];       /* registers after the bind */
    unsigned n_extra;
    const float (*quad)[4];     /* x, y, s, t a corner; tex_quad's if NULL */
    const uint8_t *mip[3];      /* levels 1 to 3, same format; NULL skips */
} TexScene;

static uint32_t tex_mode1(uint32_t env, uint32_t layout, uint32_t cls,
                          uint32_t depth)
{
    return 1 | env << 1 | layout << 3 | 2 << 5 | cls << 9 |
           (depth == 2 ? 0x10000 : 0x40000);
}

static uint32_t tex_bytes_a_texel(uint32_t layout, uint32_t depth)
{
    return (layout + 1) * (depth == 2 ? 2 : 1);
}

static uint32_t tex_log2(uint32_t n)
{
    return 31 - __builtin_clz(n);
}

static void tex_scene(QTestState *t, const TexScene *sc)
{
    static const float white[4] = { 1, 1, 1, 1 };
    static const float geometry[4][4] = {
        { 16, 16, 0, 0 }, { 48, 16, 1, 0 }, { 48, 32, 1, 1 }, { 16, 32, 0, 1 },
    };
    uint32_t w = sc->w ? sc->w : 4, h = sc->h ? sc->h : 2;
    uint32_t size = tex_log2(h) << 4 | tex_log2(w);
    uint32_t spec = tex_log2(h) << 8 | tex_log2(w) << 4;
    uint32_t box[6] = { 0, 0, w, h, h << 16 | w, 0 };
    const float *frag = sc->frag[3] ? sc->frag : white;

    tex_enter(t, GL_SLOT_A);
    gl_clear_color(t, 1, 1, 1, 1);
    gl_word(t, GL_CLEAR_COLOR_BUFFER);
    tex_load_fmt(t, true, 0x34000 | sc->layout << 1 | sc->depth << 5, spec,
                 sc->texels, w, h, sc->layout + 1,
                 tex_bytes_a_texel(sc->layout, sc->depth), box, true, 0, 0);
    for (uint32_t n = 1; n <= 3; n++) {
        uint32_t lw = MAX(1, w >> n), lh = MAX(1, h >> n);
        uint32_t lbox[6] = { 0, 0, lw, lh, lh << 16 | lw, 0 };

        if (sc->mip[n - 1]) {
            tex_load_fmt(t, true, 0x34000 | sc->layout << 1 | sc->depth << 5,
                         n | tex_log2(lh) << 8 | tex_log2(lw) << 4,
                         sc->mip[n - 1], lw, lh, sc->layout + 1,
                         tex_bytes_a_texel(sc->layout, sc->depth), lbox, true,
                         0, 0);
        }
    }
    tex_bind(t, tex_mode1(sc->env, sc->layout, sc->cls, sc->depth),
             0x10000 | sc->depth << 2 | sc->mode2);
    tex_records(t, (const uint32_t[]){ TEX_TXSIZE, size << 8 | size,
                                       TEX_TXCLAMPSIZE, w << 13 | h }, 2);
    for (unsigned i = 0; i < sc->n_extra; i++) {
        tex_record(t, sc->extra[i][0], sc->extra[i][1]);
    }
    if (sc->over_white) {
        gl_one(t, GL_BLEND_HDR, 1);
        gl_blend(t, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD);
    }
    tex_quad_rect(t, frag, sc->quad ? sc->quad : geometry);
}

/* Each channel of a pixel within one count of 255 * (r, g, b). */
static void tex_expect(uint32_t got, double r, double g, double b)
{
    const double want[3] = { r, g, b };

    for (int k = 0; k < 3; k++) {
        int have = got >> (16 - 8 * k) & 0xff;

        if (abs(have - (int)lround(want[k] * 255)) > 1) {
            g_printerr("channel %d: have %d, want %.1f\n", k, have,
                       want[k] * 255);
            g_assert_not_reached();
        }
    }
}

/*
 * A one-component level is stored once; alpha, luminance and intensity
 * differ only in the class the draw's TEXMODE1 names. With a red
 * fragment over white, luminance leaves the alpha of the stored colour
 * at the fragment's 1 and intensity makes it the texel, which shows in
 * the green channel. A model reading the class at load could not draw
 * both from one level.
 */
static void test_gl_texture_class_intensity_vs_luminance(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[8];

    for (int i = 0; i < 8; i++) {
        texels[i] = 0x20 * i + 0x10;
    }
    for (int cls = TEX_CLASS_LUMINANCE; cls <= TEX_CLASS_INTENSITY; cls++) {
        TexScene sc = {
            .layout = TEX_LAYOUT_ONE, .depth = 1, .cls = cls,
            .texels = texels, .frag = { 1, 0, 0, 1 }, .over_white = true,
        };

        tex_scene(t, &sc);
        for (int i = 0; i < 8; i++) {
            double v = texels[i] / 255.0;

            if (cls == TEX_CLASS_LUMINANCE) {
                tex_expect(tex_texel_pixel(t, i % 4, i / 4), v, 0, 0);
            } else {
                tex_expect(tex_texel_pixel(t, i % 4, i / 4), v * v + 1 - v,
                           1 - v, 1 - v);
            }
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * TRAM select. The client gives each one- and two-component texture a
 * TRAM select (TL_MODE bits 4:3 on load, TEXMODE1 bits 8:7 on draw) and
 * puts several at the same block, told apart only by it, as perfly's
 * town database does with two 32-by-32 levels at one TXMIPMAP word under
 * selects 0 and 3. Two LUMINANCE8 levels loaded so each draw their own
 * texels, a select nothing was loaded under draws untextured, and the
 * levels survive migration.
 */
#define TEX_TL_MODE_LUM8      0x34020

static void tex_select_load(QTestState *t, uint32_t select,
                            const uint8_t texels[8])
{
    static const uint32_t box[6] = { 0, 0, 4, 2, 2 << 16 | 4, 0 };

    tex_load_fmt(t, true, TEX_TL_MODE_LUM8 | select << 3, TEX_SPEC_4X2,
                 texels, 4, 2, 1, 1, box, true, 0, 0);
}

static void tex_select_draw(QTestState *t, uint32_t select)
{
    static const float white[4] = { 1, 1, 1, 1 };
    static const float geometry[4][4] = {
        { 16, 16, 0, 0 }, { 48, 16, 1, 0 }, { 48, 32, 1, 1 }, { 16, 32, 0, 1 },
    };

    tex_bind(t, tex_mode1(TEX_ENV_MODULATE, TEX_LAYOUT_ONE,
                          TEX_CLASS_LUMINANCE, 1) | select << 7,
             0x10000 | 1 << 2);
    tex_quad_rect(t, white, geometry);
}

static void tex_select_expect(QTestState *t, uint32_t select,
                              const uint8_t texels[8])
{
    tex_select_draw(t, select);
    for (int i = 0; i < 8; i++) {
        double v = texels ? texels[i] / 255.0 : 1.0;

        tex_expect(tex_texel_pixel(t, i % 4, i / 4), v, v, v);
    }
}

static void test_gl_texture_tram_select_keys_small_levels(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    GError *err = NULL;
    QTestState *t = tex_start(TEX_ARGS), *to;
    uint8_t first[8], second[8];

    for (int i = 0; i < 8; i++) {
        first[i] = 0x10 + 0x20 * i;
        second[i] = 0xf0 - 0x18 * i;
    }
    tex_enter(t, GL_SLOT_A);
    tex_select_load(t, 0, first);
    tex_select_load(t, 3, second);
    tex_select_expect(t, 0, first);
    tex_select_expect(t, 3, second);
    tex_select_expect(t, 1, NULL);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);

    tmpfs = g_dir_make_tmp("sn0-mgras-tram-select-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(tmpfs, "state", NULL);
    uri = g_strdup_printf("file:%s", path);
    sgi_migrate_stopped(t, uri);
    qtest_quit(t);
    to = qtest_initf("%s -incoming %s", TEX_ARGS, uri);
    sgi_wait_migration_status(to, "completed", 30);
    tex_select_expect(to, 3, second);
    tex_select_expect(to, 0, first);
    qtest_quit(to);
    unlink(path);
    g_rmdir(tmpfs);
}

/*
 * For RGB and RGBA the client leaves TEXMODE1 bits 8:7 undefined (an
 * uninitialised stack word), and the town database's RGBA draws carry
 * selects 1 and 2 for textures loaded under select 0: the bits are
 * ignored and the level draws.
 */
static void test_gl_texture_tram_select_ignored_for_rgba(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32];

    tex_texels(texels);
    for (uint32_t select = 1; select <= 3; select++) {
        tex_load(t, TEX_TL_MODE_RGBA8, texels);
        tex_bind(t, TEX_TEXMODE1_ON | select << 7, TEX_TEXMODE2_RGBA8);
        tex_quad(t);
        tex_expect_texels(t, texels);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * An ALPHA8 texel 0x80 under a red opaque fragment gives colour (1, 0,
 * 0) and alpha one half, whatever environment the draw names: the
 * client sends code 3 for it, and the model reads the alpha class as
 * that function (OpenGL 1.2.1 tables 3.18 and 3.19) for every code. The
 * other layouts put their components in the right channels: RGB texel
 * bytes R, G, B, luminance-alpha L then A, RGBA R, G, B, A.
 */
static void test_gl_texture_class_alpha(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t one[8], la[16], rgb[24], rgba[32];

    memset(one, 0x80, sizeof(one));
    for (int env = 0; env <= TEX_ENV_ALPHA; env++) {
        TexScene sc = {
            .layout = TEX_LAYOUT_ONE, .depth = 1, .cls = TEX_CLASS_ALPHA,
            .env = env, .texels = one, .frag = { 1, 0, 0, 1 },
        };

        tex_scene(t, &sc);
        tex_expect(tex_texel_pixel(t, 2, 1), 1, 0, 0);
        sc.over_white = true;
        tex_scene(t, &sc);
        tex_expect(tex_texel_pixel(t, 2, 1), 1, 1 - 0x80 / 255.0,
                   1 - 0x80 / 255.0);
    }

    for (int i = 0; i < 8; i++) {
        la[2 * i] = 0x30 + 0x10 * i;
        la[2 * i + 1] = 0xf0 - 0x10 * i;
        rgb[3 * i] = 0x10 * i;
        rgb[3 * i + 1] = 0x80 + 0x10 * i;
        rgb[3 * i + 2] = 0xf0 - 0x20 * i;
        rgba[4 * i] = 0x10 * i + 1;
        rgba[4 * i + 1] = 0x20 + 0x10 * i;
        rgba[4 * i + 2] = 0xe0 - 0x10 * i;
        rgba[4 * i + 3] = 0x40 + 0x10 * i;
    }
    {
        TexScene sc = {
            .layout = TEX_LAYOUT_LA, .depth = 1, .cls = TEX_CLASS_LUMINANCE,
            .texels = la,
        };

        tex_scene(t, &sc);
        for (int i = 0; i < 8; i++) {
            double l = la[2 * i] / 255.0;

            tex_expect(tex_texel_pixel(t, i % 4, i / 4), l, l, l);
        }
        sc.over_white = true;
        tex_scene(t, &sc);
        for (int i = 0; i < 8; i++) {
            double l = la[2 * i] / 255.0, a = la[2 * i + 1] / 255.0;
            double v = l * a + 1 - a;

            tex_expect(tex_texel_pixel(t, i % 4, i / 4), v, v, v);
        }
    }
    {
        TexScene sc = {
            .layout = TEX_LAYOUT_RGB, .depth = 1, .cls = TEX_CLASS_COLOUR,
            .texels = rgb,
        };

        tex_scene(t, &sc);
        for (int i = 0; i < 8; i++) {
            tex_expect(tex_texel_pixel(t, i % 4, i / 4), rgb[3 * i] / 255.0,
                       rgb[3 * i + 1] / 255.0, rgb[3 * i + 2] / 255.0);
        }
        sc.over_white = true;
        tex_scene(t, &sc);
        /* No alpha in the texel: the fragment's 1 stays, colour as it was. */
        for (int i = 0; i < 8; i++) {
            tex_expect(tex_texel_pixel(t, i % 4, i / 4), rgb[3 * i] / 255.0,
                       rgb[3 * i + 1] / 255.0, rgb[3 * i + 2] / 255.0);
        }
    }
    {
        TexScene sc = {
            .layout = TEX_LAYOUT_RGBA, .depth = 1, .cls = TEX_CLASS_COLOUR,
            .texels = rgba, .over_white = true,
        };

        tex_scene(t, &sc);
        for (int i = 0; i < 8; i++) {
            double a = rgba[4 * i + 3] / 255.0;

            tex_expect(tex_texel_pixel(t, i % 4, i / 4),
                       rgba[4 * i] / 255.0 * a + 1 - a,
                       rgba[4 * i + 1] / 255.0 * a + 1 - a,
                       rgba[4 * i + 2] / 255.0 * a + 1 - a);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * Blend is C = Cf (1 - Ct) + Cc Ct with the environment colour Cc, which
 * the client sends as 0x142 = R12 | G12 << 12 and 0x143 = B12. A texel
 * (0.5, 0.5, 0.5, 1) under a red fragment and a green Cc gives (0.5, 0.5,
 * 0). Intensity blend takes alpha as Af (1 - At), the environment alpha
 * being 0 because the client never sends one (approximated).
 */
static void test_gl_texture_env_blend_constant(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t rgba[32], inten[8];

    for (int i = 0; i < 8; i++) {
        rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = 0x80;
        rgba[4 * i + 3] = 0xff;
        inten[i] = 0x80;
    }
    {
        TexScene sc = {
            .layout = TEX_LAYOUT_RGBA, .depth = 1, .cls = TEX_CLASS_COLOUR,
            .env = TEX_ENV_BLEND, .texels = rgba, .frag = { 1, 0, 0, 1 },
            .extra = { { 0x142, 0xfff000 }, { 0x143, 0 } }, .n_extra = 2,
        };

        tex_scene(t, &sc);
        tex_expect(tex_texel_pixel(t, 1, 1), 0x80 / 255.0, 1 - 0x80 / 255.0,
                   0);
        /* The texel alpha is 1, so the alpha is the fragment's 1. */
        sc.over_white = true;
        tex_scene(t, &sc);
        tex_expect(tex_texel_pixel(t, 1, 1), 0x80 / 255.0, 1 - 0x80 / 255.0,
                   0);
    }
    {
        double i = 0x80 / 255.0;
        TexScene sc = {
            .layout = TEX_LAYOUT_ONE, .depth = 1, .cls = TEX_CLASS_INTENSITY,
            .env = TEX_ENV_BLEND, .texels = inten, .frag = { 1, 0, 0, 1 },
            .over_white = true,
        };

        tex_scene(t, &sc);
        /* C = (1 - I, 0, 0) with Cc = 0, A = 1 - I; over white. */
        tex_expect(tex_texel_pixel(t, 1, 1), (1 - i) * (1 - i) + i, i, i);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * The client stores GL_REPLACE as modulate before sending, so the word
 * for a replace draw is environment code 0 and the board gives the
 * modulate colour: a non-white fragment shows through the texel. This
 * differs from OpenGL replace unless the fragment is white, and is the
 * model's stated limitation.
 */
static void test_gl_texture_replace_is_modulate(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t rgba[32];

    for (int i = 0; i < 8; i++) {
        rgba[4 * i] = 0xc0;
        rgba[4 * i + 1] = 0x80;
        rgba[4 * i + 2] = 0x40;
        rgba[4 * i + 3] = 0xff;
    }
    {
        TexScene sc = {
            .layout = TEX_LAYOUT_RGBA, .depth = 1, .cls = TEX_CLASS_COLOUR,
            .env = TEX_ENV_MODULATE, .texels = rgba,
            .frag = { 0.5f, 1, 0.25f, 1 },
        };

        tex_scene(t, &sc);
        tex_expect(tex_texel_pixel(t, 2, 0), 0.5 * 0xc0 / 255.0, 0x80 / 255.0,
                   0.25 * 0x40 / 255.0);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * Decal on an RGBA texel is C = Cf (1 - At) + Ct At and keeps the
 * fragment's alpha: texel (0, 1, 0, 0.5) under red gives (0.5, 0.5, 0)
 * and alpha 1, which over white leaves the colour unmixed. An RGB texel
 * under decal returns its own colour. Decal on a luminance texel is
 * undefined in OpenGL; the model draws modulate, so the fragment is
 * scaled by L.
 */
static void test_gl_texture_env_decal_rgba(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t rgba[32], rgb[24], lum[8];

    for (int i = 0; i < 8; i++) {
        rgba[4 * i] = 0;
        rgba[4 * i + 1] = 0xff;
        rgba[4 * i + 2] = 0;
        rgba[4 * i + 3] = 0x80;
        rgb[3 * i] = 0x33;
        rgb[3 * i + 1] = 0x66;
        rgb[3 * i + 2] = 0x99;
        lum[i] = 0x40;
    }
    {
        double a = 0x80 / 255.0;
        TexScene sc = {
            .layout = TEX_LAYOUT_RGBA, .depth = 1, .cls = TEX_CLASS_COLOUR,
            .env = TEX_ENV_DECAL, .texels = rgba, .frag = { 1, 0, 0, 1 },
            .over_white = true,
        };

        tex_scene(t, &sc);
        tex_expect(tex_texel_pixel(t, 3, 1), 1 - a, a, 0);
    }
    {
        TexScene sc = {
            .layout = TEX_LAYOUT_RGB, .depth = 1, .cls = TEX_CLASS_COLOUR,
            .env = TEX_ENV_DECAL, .texels = rgb, .frag = { 1, 0, 0, 1 },
        };

        tex_scene(t, &sc);
        tex_expect(tex_texel_pixel(t, 0, 0), 0x33 / 255.0, 0x66 / 255.0,
                   0x99 / 255.0);
    }
    {
        TexScene sc = {
            .layout = TEX_LAYOUT_ONE, .depth = 1,
            .cls = TEX_CLASS_LUMINANCE, .env = TEX_ENV_DECAL, .texels = lum,
            .frag = { 1, 0.5f, 0, 1 },
        };

        tex_scene(t, &sc);
        tex_expect(tex_texel_pixel(t, 0, 0), 0x40 / 255.0,
                   0.5 * 0x40 / 255.0, 0);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

#define TEX_WRAP_S            0x0800
#define TEX_WRAP_T            0x1000
#define TEX_WRAP_CLAMP        0x4000    /* bit 14: CLAMP, not to border */

/* TXBCOLOR's two words for a colour of four 12-bit fields. */
#define TEX_BORDER_RG(r, g)   { 0x184, (r) | (g) << 12 }
#define TEX_BORDER_BA(b, a)   { 0x185, (b) | (a) << 12 }

/* Texel i of a 4-by-2 RGBA level, as tex_texels() makes it. */
static void tex_wrap_rgba(uint8_t texels[32])
{
    tex_texels(texels);
}

/*
 * Coordinate 1.5 under a nearest filter: with bit 14 set (GL_CLAMP) the
 * coordinate clamps to the edge texel, with it clear (clamp to border)
 * the texel is TXBCOLOR. The quad runs s over 0 to 2 across 32 pixels,
 * so pixel 42 is s = 1.6 and pixel 18 is inside the image.
 */
static void test_gl_texture_clamp_border_nearest(void)
{
    static const float wide[4][4] = {
        { 16, 16, 0, 0 }, { 48, 16, 2, 0 }, { 48, 32, 2, 1 }, { 16, 32, 0, 1 },
    };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32];

    tex_wrap_rgba(texels);
    for (int clamp = 0; clamp <= 1; clamp++) {
        TexScene sc = {
            .layout = TEX_LAYOUT_RGBA, .depth = 1, .cls = TEX_CLASS_COLOUR,
            .texels = texels, .quad = wide,
            .mode2 = TEX_WRAP_S | (clamp ? TEX_WRAP_CLAMP : 0),
            .extra = { TEX_BORDER_RG(0xfff, 0),
                       TEX_BORDER_BA(0xfff, 0xfff) },
            .n_extra = 2,
        };
        const uint8_t *edge = &texels[4 * 3];

        tex_scene(t, &sc);
        tex_expect(gl_window_pixel(t, 18, 20), texels[0] / 255.0,
                   texels[1] / 255.0, texels[2] / 255.0);
        if (clamp) {
            tex_expect(gl_window_pixel(t, 42, 20), edge[0] / 255.0,
                       edge[1] / 255.0, edge[2] / 255.0);
        } else {
            tex_expect(gl_window_pixel(t, 42, 20), 1, 0, 1);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * S repeats while T clamps: s = 1.28 wraps to texel 1 and t = 2.34
 * clamps to the top row, texel 5, where a shared clamp would give the
 * edge texel 7 and a shared repeat texel 1. With bit 14 clear, T clamps
 * to the border instead, which comes from the one flag all axes share.
 */
static void test_gl_texture_wrap_mixed_axes(void)
{
    static const float wide[4][4] = {
        { 16, 16, 0, 0 }, { 48, 16, 2, 0 }, { 48, 32, 2, 3 }, { 16, 32, 0, 3 },
    };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32];

    tex_wrap_rgba(texels);
    for (int clamp = 0; clamp <= 1; clamp++) {
        TexScene sc = {
            .layout = TEX_LAYOUT_RGBA, .depth = 1, .cls = TEX_CLASS_COLOUR,
            .texels = texels, .quad = wide,
            .mode2 = TEX_WRAP_T | (clamp ? TEX_WRAP_CLAMP : 0),
            .extra = { TEX_BORDER_RG(0xfff, 0),
                       TEX_BORDER_BA(0xfff, 0xfff) },
            .n_extra = 2,
        };
        const uint8_t *want = &texels[4 * 5];

        tex_scene(t, &sc);
        if (clamp) {
            tex_expect(gl_window_pixel(t, 36, 28), want[0] / 255.0,
                       want[1] / 255.0, want[2] / 255.0);
        } else {
            tex_expect(gl_window_pixel(t, 36, 28), 1, 0, 1);
        }
        /* Inside t, S still wraps: s = 1.28 is texel 1 of row 0. */
        tex_expect(gl_window_pixel(t, 36, 17), texels[4] / 255.0,
                   texels[5] / 255.0, texels[6] / 255.0);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * One border colour, (0.8, 0.4, 0.2, 0.6), reduced to each class as the
 * texture's own format (OpenGL 1.2.1 3.8.5, approximated): alpha takes
 * A, luminance and intensity take R, luminance-alpha takes R and A, RGB
 * takes R, G, B with alpha 1. Drawn outside a clamp-to-border texture
 * under a red fragment over white, so the green channel is one minus
 * the alpha.
 */
static void test_gl_texture_border_per_class(void)
{
    static const float wide[4][4] = {
        { 16, 16, 0, 0 }, { 48, 16, 2, 0 }, { 48, 32, 2, 1 }, { 16, 32, 0, 1 },
    };
    static const struct {
        uint32_t layout, cls;
        double r, g, b;
    } cases[] = {
        { TEX_LAYOUT_ONE, TEX_CLASS_ALPHA, 1, 0.4, 0.4 },
        { TEX_LAYOUT_ONE, TEX_CLASS_LUMINANCE, 0.8, 0, 0 },
        { TEX_LAYOUT_ONE, TEX_CLASS_INTENSITY,
          0.8 * 0.8 + 0.2, 0.2, 0.2 },
        { TEX_LAYOUT_LA, TEX_CLASS_LUMINANCE,
          0.8 * 0.6 + 0.4, 0.4, 0.4 },
        { TEX_LAYOUT_RGB, TEX_CLASS_COLOUR, 0.8, 0, 0 },
        { TEX_LAYOUT_RGBA, TEX_CLASS_COLOUR,
          0.8 * 0.6 + 0.4, 0.4, 0.4 },
    };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[64] = { 0 };

    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        TexScene sc = {
            .layout = cases[i].layout, .depth = 1, .cls = cases[i].cls,
            .texels = texels, .quad = wide, .frag = { 1, 0, 0, 1 },
            .over_white = true, .mode2 = TEX_WRAP_S,
            .extra = { TEX_BORDER_RG(0xccc, 0x666),
                       TEX_BORDER_BA(0x333, 0x999) },
            .n_extra = 2,
        };

        tex_scene(t, &sc);
        tex_expect(gl_window_pixel(t, 42, 20), cases[i].r, cases[i].g,
                   cases[i].b);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

#define TEX_MAG_LINEAR        0x0080
#define TEX_MIN_LINEAR        0x0100

/*
 * A 2-by-1 texture, black then white, drawn magnified across 30 or 31
 * pixels so that one pixel centre falls exactly on a texel join or edge.
 * At the join the linear mean is one half, 127 or 128; the tolerance of
 * one count is the model's own, the board's blend precision being
 * unknown. With only the minification filter linear (bit 8) the draw
 * is magnified and takes the nearest texel, which tells the two filter
 * bits apart.
 */
static void test_gl_texture_linear_midpoint(void)
{
    static const float quad[4][4] = {
        { 16, 16, 0, 0 }, { 47, 16, 1, 0 }, { 47, 32, 1, 1 }, { 16, 32, 0, 1 },
    };
    static const uint8_t texels[8] = {
        0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    QTestState *t = tex_start(TEX_ARGS);
    static const struct {
        uint32_t mode2;
        double want;
    } cases[] = {
        { TEX_MAG_LINEAR | TEX_MIN_LINEAR, 0.5 },
        { TEX_MAG_LINEAR, 0.5 },
        { TEX_MIN_LINEAR, 1 },
        { 0, 1 },
    };

    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        TexScene sc = {
            .layout = TEX_LAYOUT_RGBA, .depth = 1, .cls = TEX_CLASS_COLOUR,
            .w = 2, .h = 1, .texels = texels, .quad = quad,
            .mode2 = cases[i].mode2,
        };

        tex_scene(t, &sc);
        /* The pixel centred on s = 0.5. */
        tex_expect(gl_window_pixel(t, 31, 24), cases[i].want, cases[i].want,
                   cases[i].want);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * A coordinate on the texture's edge, s = 0, blends the edge texel with
 * its neighbour outside the image: under repeat that is the opposite
 * edge texel (white), under GL_CLAMP and clamp to border the border
 * colour (red). The quad's s runs -0.25 to 0.75 over 30 pixels, so pixel
 * 23's centre is s = 0.
 */
static void test_gl_texture_linear_preserves_12bit_components(void)
{
    static const float quad[4][4] = {
        { 16, 16, 0.625f, 0.5f }, { 47, 16, 0.625f, 0.5f },
        { 47, 32, 0.625f, 0.5f }, { 16, 32, 0.625f, 0.5f },
    };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[16] = { 0 };
    TexScene sc = {
        .layout = TEX_LAYOUT_RGBA, .depth = 2, .cls = TEX_CLASS_COLOUR,
        .mode2 = TEX_MAG_LINEAR | TEX_MIN_LINEAR,
        .w = 2, .h = 1, .texels = texels, .quad = quad,
    };

    stw_be_p(&texels[2], 0xfff0);
    stw_be_p(&texels[6], 0xffff);
    stw_be_p(&texels[8], 0x0090);
    stw_be_p(&texels[10], 0xfff0);
    stw_be_p(&texels[14], 0xffff);
    tex_scene(t, &sc);
    g_assert_cmphex(gl_window_pixel(t, 31, 24), ==, 0x00ff00);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void test_gl_texture_linear_edge_wrap(void)
{
    static const float quad[4][4] = {
        { 16, 16, -0.25f, 0 }, { 46, 16, 0.75f, 0 },
        { 46, 32, 0.75f, 1 }, { 16, 32, -0.25f, 1 },
    };
    static const uint8_t texels[8] = {
        0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    QTestState *t = tex_start(TEX_ARGS);
    static const struct {
        uint32_t wrap;
        double r, g, b;
    } cases[] = {
        { 0, 0.5, 0.5, 0.5 },
        { TEX_WRAP_S | TEX_WRAP_CLAMP, 0.5, 0, 0 },
        { TEX_WRAP_S, 0.5, 0, 0 },
    };

    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        TexScene sc = {
            .layout = TEX_LAYOUT_RGBA, .depth = 1, .cls = TEX_CLASS_COLOUR,
            .w = 2, .h = 1, .texels = texels, .quad = quad,
            .mode2 = TEX_MAG_LINEAR | TEX_MIN_LINEAR | cases[i].wrap,
            .extra = { TEX_BORDER_RG(0xfff, 0),
                       TEX_BORDER_BA(0, 0xfff) },
            .n_extra = 2,
        };

        tex_scene(t, &sc);
        tex_expect(gl_window_pixel(t, 23, 24), cases[i].r, cases[i].g,
                   cases[i].b);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * A 12-bit component arrives as a big-endian halfword of which the top 12
 * bits are kept (approximated; the board's choice is unknown) and is
 * drawn as the nearest 8-bit value, v * 255 / 4095. Luminance halfwords
 * 0x0f00 and 0x0ff0 differ only in the low 4 bits of their 12-bit values
 * and draw as 15 and 16, which a truncation to 8 bits would make equal;
 * 0x0ff0, 0x0ff8 and 0x0fff differ only in the dropped bits and draw
 * alike; 0x0000, 0x4000, 0x8000 and 0xffff differ in the high bits and
 * stay apart. An RGBA12 texel checks the component order and widths.
 */
static void test_gl_texture_depth12_halfword(void)
{
    static const uint16_t half[8] = {
        0x0000, 0x4000, 0x8000, 0xffff, 0x0f00, 0x0ff0, 0x0ff8, 0x0fff,
    };
    static const int want[8] = { 0, 64, 128, 255, 15, 16, 16, 16 };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t lum[16], rgba[64];
    TexScene sc = {
        .layout = TEX_LAYOUT_ONE, .depth = 2, .cls = TEX_CLASS_LUMINANCE,
        .texels = lum,
    };

    for (int i = 0; i < 8; i++) {
        stw_be_p(&lum[2 * i], half[i]);
    }
    tex_scene(t, &sc);
    for (int i = 0; i < 8; i++) {
        g_assert_cmphex(tex_texel_pixel(t, i % 4, i / 4), ==,
                        want[i] << 16 | want[i] << 8 | want[i]);
    }

    for (int i = 0; i < 8; i++) {
        stw_be_p(&rgba[8 * i], 0xf000);
        stw_be_p(&rgba[8 * i + 2], 0x8000);
        stw_be_p(&rgba[8 * i + 4], 0x1000);
        stw_be_p(&rgba[8 * i + 6], 0xffff);
    }
    sc = (TexScene) {
        .layout = TEX_LAYOUT_RGBA, .depth = 2, .cls = TEX_CLASS_COLOUR,
        .texels = rgba,
    };
    tex_scene(t, &sc);
    g_assert_cmphex(tex_texel_pixel(t, 1, 1), ==, 239 << 16 | 128 << 8 | 16);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

#define TEX_MIP               0x80020   /* bits 5 and 19: more than a level */
#define TEX_MIP_BETWEEN       0x0040    /* bit 6: blend between levels */

/*
 * The board derives its level of detail from the iterators, so a test
 * sets it through the geometry: a square of 16 pixels whose texture
 * coordinates span k texels of the 4-by-4 base level has rho = k / 16
 * and lambda = log2(rho). This quad has rho of the given value.
 */
static void tex_lambda_quad(float quad[4][4], double rho)
{
    float span = rho * 16 / 4;
    const float c[4][4] = {
        { 16, 16, 0, 0 }, { 32, 16, 1, 0 }, { 32, 32, 1, 1 }, { 16, 32, 0, 1 },
    };

    for (int i = 0; i < 4; i++) {
        quad[i][0] = c[i][0];
        quad[i][1] = c[i][1];
        quad[i][2] = c[i][2] * span;
        quad[i][3] = c[i][3] * span;
    }
}

static void tex_flat(uint8_t *texels, unsigned n, uint32_t rgba)
{
    for (unsigned i = 0; i < n; i++) {
        stl_be_p(&texels[4 * i], rgba);
    }
}

/*
 * Nearest-mipmap level selection. Three flat levels, red 4 by 4, green
 * 2 by 2, blue 1 by 1, drawn at lambda 0, 1 and 2 (k / n of 1, 2, 4) give
 * each in turn; lambda 3 is past the last level and gives the last, and
 * lambda -2 magnifies and gives the base level, never a level below it.
 * A minimum LOD of 1 in TXLOD bits 25:16 makes the lambda 0 draw return
 * level 1. A chain with no level 1 loaded is incomplete even when level 0
 * and a later level share its TRAM block, so the draw disables texturing.
 */
static void test_gl_texture_mip_level_select(void)
{
    static const struct {
        double rho;
        uint32_t txlod;
        uint32_t want;
    } cases[] = {
        { 1, 2, 0xff0000 }, { 2, 2, 0x00ff00 }, { 4, 2, 0x0000ff },
        { 8, 2, 0x0000ff }, { 0.25, 2, 0xff0000 },
        { 1, 64 << 16 | 2, 0x00ff00 },
    };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t l0[64], l1[16], l2[4];

    tex_flat(l0, 16, 0xff0000ff);
    tex_flat(l1, 4, 0x00ff00ff);
    tex_flat(l2, 1, 0x0000ffff);
    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        float quad[4][4];
        TexScene sc = {
            .layout = TEX_LAYOUT_RGBA, .depth = 1, .cls = TEX_CLASS_COLOUR,
            .w = 4, .h = 4, .texels = l0, .mip = { l1, l2 },
            .mode2 = TEX_MIP, .quad = quad,
            .extra = { { TEX_TXLOD, cases[i].txlod } }, .n_extra = 1,
        };

        tex_lambda_quad(quad, cases[i].rho);
        tex_scene(t, &sc);
        g_assert_cmphex(gl_window_pixel(t, 24, 24), ==, cases[i].want);
    }
    {
        float quad[4][4];
        TexScene sc = {
            .layout = TEX_LAYOUT_RGBA, .depth = 1, .cls = TEX_CLASS_COLOUR,
            .w = 4, .h = 4, .texels = l0, .mip = { NULL, l2 },
            .mode2 = TEX_MIP, .quad = quad,
            .extra = { { TEX_TXLOD, 2 } }, .n_extra = 1,
        };

        qtest_quit(t);
        t = tex_start(TEX_ARGS);
        tex_lambda_quad(quad, 2);
        tex_scene(t, &sc);
        g_assert_cmphex(gl_window_pixel(t, 24, 24), ==, TEX_WHITE);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * Trilinear blends the two levels around lambda by its fraction: red
 * over green at lambda 0.5 gives half and half, at 0.25 three to one.
 * At or past the last level (TXLOD bits 3:0 = 1) the result is that
 * level alone, with no level beyond it fetched, which a model that
 * went to level 2 would draw untextured.
 */
static void test_gl_texture_trilinear_blend(void)
{
    static const struct {
        double rho;
        double r, g;
    } cases[] = {
        { 1.4142135623730951, 0.5, 0.5 },
        { 1.189207115002721, 0.75, 0.25 },
        { 2, 0, 1 },
        { 2.8284271247461903, 0, 1 },
        { 8, 0, 1 },
    };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t l0[64], l1[16];

    tex_flat(l0, 16, 0xff0000ff);
    tex_flat(l1, 4, 0x00ff00ff);
    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        float quad[4][4];
        TexScene sc = {
            .layout = TEX_LAYOUT_RGBA, .depth = 1, .cls = TEX_CLASS_COLOUR,
            .w = 4, .h = 4, .texels = l0, .mip = { l1 },
            .mode2 = TEX_MIP | TEX_MIP_BETWEEN | TEX_MIN_LINEAR,
            .quad = quad, .extra = { { TEX_TXLOD, 1 } }, .n_extra = 1,
        };

        tex_lambda_quad(quad, cases[i].rho);
        tex_scene(t, &sc);
        tex_expect(gl_window_pixel(t, 24, 24), cases[i].r, cases[i].g, 0);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * The raster engine applies the read-back registers 0x46, 0x47, 0x153,
 * 0x158 and 0x159 from a 0x7c08 block itself, so the texture path, which
 * sees the same block on a board with TRAM, does not log them as not
 * modelled. A register nothing applies is still logged.
 */
static void test_gl_texture_raster_records_are_not_logged(void)
{
    static const uint32_t applied[] = { 0x46, 0x47, 0x153, 0x158, 0x159 };
    g_autofree char *tmpfs = NULL;
    g_autofree char *log = NULL;
    g_autofree char *args = NULL;
    g_autofree char *contents = NULL;
    GError *err = NULL;
    QTestState *t;

    tmpfs = g_dir_make_tmp("sn0-mgras-tex-log-XXXXXX", &err);
    g_assert_no_error(err);
    log = g_strdup_printf("%s/unimp.log", tmpfs);
    args = g_strdup_printf(TEX_ARGS " -d unimp -D %s", log);
    t = tex_start(args);
    for (int i = 0; i < ARRAY_SIZE(applied); i++) {
        tex_record(t, applied[i], 0x1234);
    }
    tex_record(t, 0x1ff, 0x5678);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    g_assert_true(g_file_get_contents(log, &contents, NULL, &err));
    g_assert_no_error(err);
    for (int i = 0; i < ARRAY_SIZE(applied); i++) {
        g_autofree char *needle =
            g_strdup_printf("raster register record 0x%x ", applied[i]);

        g_assert_null(strstr(contents, needle));
    }
    g_assert_nonnull(strstr(contents, "raster register record 0x1ff "));
    qtest_quit(t);
    unlink(log);
    g_rmdir(tmpfs);
}

/*
 * MXI and EMXI: a TE1 and its TRAM on each of two REs, each RE owning
 * alternate rows. The model keeps one texture store for both, so texture
 * state must reach every row alike. Rows 16 and 17 stand for an even and
 * an odd row; a quad two pixels tall with t from 0 to 0.25 keeps both in
 * texel row 0 of the 4-by-2 level, so each must read that row's texels.
 */
#define MXI_TEX_BASE_HDR      0x0000eb04
#define MXI_ROW_EVEN          16
#define MXI_ROW_ODD           17

static QTestState *mxi_start(const char *board)
{
    g_autofree char *args = g_strdup_printf(
        MACHINE_ARGS ",product=%s", board);

    return tex_start(args);
}

static void mxi_two_row_quad(QTestState *t)
{
    static const float white[4] = { 1, 1, 1, 1 };
    static const float v[4][4] = {
        { 16, 16, 0, 0 }, { 48, 16, 1, 0 },
        { 48, 18, 1, 0.25f }, { 16, 18, 0, 0.25f },
    };

    tex_quad_rect(t, white, v);
}

/* Both rows hold texel row 0's colours, or the quad's white when not. */
static void mxi_expect_rows(QTestState *t, const uint8_t texels[32],
                            bool textured)
{
    for (int x = 0; x < 4; x++) {
        const uint8_t *p = &texels[4 * x];
        uint32_t want = textured ? p[0] << 16 | p[1] << 8 | p[2] : TEX_WHITE;

        g_assert_cmphex(gl_window_pixel(t, 20 + 8 * x, MXI_ROW_EVEN), ==,
                        want);
        g_assert_cmphex(gl_window_pixel(t, 20 + 8 * x, MXI_ROW_ODD), ==,
                        want);
    }
}

static void test_mxi_texture_rows_match(gconstpointer data)
{
    QTestState *t = mxi_start(data);
    uint8_t texels[32];

    tex_texels(texels);
    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    mxi_two_row_quad(t);
    mxi_expect_rows(t, texels, true);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * One base word applies to the whole store, so to both rows. A level
 * loaded under base 5 is found again when 0xeb04 names 5 after the load,
 * and not under base 0, which holds no level: the draw is the quad's
 * white. A base consumed by one RE only would split the rows.
 */
static void test_mxi_texbase_both_rows(gconstpointer data)
{
    QTestState *t = mxi_start(data);
    uint8_t texels[32];

    gl_one(t, MXI_TEX_BASE_HDR, 5);
    tex_texels(texels);
    tex_load(t, TEX_TL_MODE_RGBA8, texels);

    gl_one(t, MXI_TEX_BASE_HDR, 0);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    mxi_two_row_quad(t);
    mxi_expect_rows(t, texels, false);

    gl_one(t, MXI_TEX_BASE_HDR, 5);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    mxi_two_row_quad(t);
    mxi_expect_rows(t, texels, true);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * TRAM's save and restore, in the words the IRIX 6.5.30 IP27 kernel and
 * IP27GR532 client sent in a captured run (sgi-mgras-texture.c, "TRAM"):
 * the kernel saves a block as 64 rows of 64 words, 16 KiB through one DMA
 * row, kicked 0xbd from DMA-map slot 2 after 0xe50c (rows, format, 0x400d)
 * and the 0xf200 sync request, and restores it after 0xe300 and a 0xd80c
 * GE word count in three 9-bit parts, kicked 0xa5. Its register writes go
 * as 0x7f08 pairs, with no GL context loaded. The host buffer sits 16 KiB
 * into slot 2's pages, clear of the texel source the loads leave at their
 * start.
 */
#define TRAM_READ_HDR         0x0000e50c
#define TRAM_READ_LAST_WORD   0x0000400d
#define TRAM_READ_DONE        0x0000da00
#define TRAM_RESTORE_OPEN     0x0000e300
#define TRAM_GE_COUNT_HDR     0x0000d80c
#define TRAM_GE_DMA_HDR       0x00009d10
#define TRAM_RE_SYNC          0x0000f200
#define TRAM_READ_FENCE       0x000e0100
#define TRAM_TEX_BASE_HDR     0x0000eb04
#define TRAM_FORMAT_RGBA8     0x00400080
#define TRAM_FORMAT_RGB8      0x00400070
#define TRAM_BLOCK_ROWS       0x40
#define TRAM_BLOCK_BYTES      0x4000
#define TRAM_BUFFER_OFFSET    0x4000u
#define TRAM_BUFFER           (0x7000u + TRAM_BUFFER_OFFSET)
#define TRAM_BUFFER_FILL      0x5a
#define TEX_TL_ADDR           0x1a3
#define TEX_TL_MIPMAP         0x1a4
#define TEX_TL_SPEC_WHOLE     0xd60
#define TEX_TL_MODE_RGB8      0x34024
#define TEX_TEXMODE1_RGB_ON   0x40851

/* 0x7f08 register pairs, as the kernel's register-write routine sends. */
static void tram_pairs(QTestState *t, const uint32_t *pairs, uint32_t n)
{
    gl_word(t, TEX_PAIRS);
    gl_word(t, n);
    gl_word(t, 2 * n);
    gl_word(t, GL_INLINE_BLOCK | 8 * n);
    for (uint32_t i = 0; i < 2 * n; i++) {
        gl_word(t, pairs[i]);
    }
}

static void tram_save_rss(QTestState *t)
{
    static const uint32_t save_rss[5] = {
        0xffffffff, 0x01001000, 0x001fc0ff, 0, 0,
    };

    gl_word(t, TEX_SAVE_RSS);
    for (int i = 0; i < 5; i++) {
        gl_word(t, save_rss[i]);
    }
}

/*
 * 0xe50c for rows of TRAM in format, the sync request, then the read of
 * bytes into the host buffer kicked kick, and the words that close it.
 */
static void tram_read(QTestState *t, uint32_t rows, uint32_t format,
                      uint32_t bytes, uint32_t kick)
{
    gl_word(t, TRAM_READ_HDR);
    gl_word(t, rows);
    gl_word(t, format);
    gl_word(t, TRAM_READ_LAST_WORD);
    gl_word(t, TRAM_RE_SYNC);
    g_assert_cmphex(qtest_readl(t, REG_HQ_STATUS) & 1, ==, 1);
    gl_eram_map(t);
    gl_eram_kick(t, TRAM_BUFFER_OFFSET, bytes, kick);
    gl_word(t, TRAM_READ_FENCE);
    gl_one(t, 0x000a0704, 0);
    gl_word(t, TRAM_READ_DONE);
    gl_word(t, TEX_RESTORE_RSS);
}

/* The kernel's save of one block, its register pairs as traced. */
static void tram_kernel_save(QTestState *t, uint32_t block)
{
    const uint32_t pairs[] = {
        TEX_TXSIZE, 0xd6d6, TEX_TXLOD, 0, TEX_TXCLAMPSIZE, 0x80000,
        TEX_TL_MODE, 0, TEX_TEXMODE1, 0x58, TEX_TEXMODE2, 0x11804,
        TEX_TXADDR, 0, TEX_TXMIPMAP, block,
    };

    gl_one(t, TRAM_TEX_BASE_HDR, 0);
    tram_pairs(t, pairs, ARRAY_SIZE(pairs) / 2);
    tram_save_rss(t);
    tram_read(t, TRAM_BLOCK_ROWS, TRAM_FORMAT_RGBA8, TRAM_BLOCK_BYTES,
              GL_ERAM_READ_KICK);
}

static void tram_save_map(QTestState *t);

/* Save a row range starting at the named 64-row TRAM block. */
static void tram_kernel_save_rows(QTestState *t, uint32_t block,
                                  uint32_t rows, uint32_t bytes)
{
    const uint32_t pairs[] = {
        TEX_TXSIZE, 0xd6d6, TEX_TXLOD, 0, TEX_TXCLAMPSIZE, 0x80000,
        TEX_TL_MODE, 0, TEX_TEXMODE1, 0x58, TEX_TEXMODE2, 0x11804,
        TEX_TXADDR, 0, TEX_TXMIPMAP, block,
    };

    gl_one(t, TRAM_TEX_BASE_HDR, 0);
    tram_pairs(t, pairs, ARRAY_SIZE(pairs) / 2);
    tram_save_rss(t);
    gl_word(t, TRAM_READ_HDR);
    gl_word(t, rows);
    gl_word(t, TRAM_FORMAT_RGBA8);
    gl_word(t, TRAM_READ_LAST_WORD);
    gl_word(t, TRAM_RE_SYNC);
    g_assert_cmphex(qtest_readl(t, REG_HQ_STATUS) & 1, ==, 1);
    tram_save_map(t);
    gl_eram_kick(t, TRAM_BUFFER_OFFSET, bytes, GL_ERAM_READ_KICK);
    gl_word(t, TRAM_READ_FENCE);
    gl_one(t, 0x000a0704, 0);
    gl_word(t, TRAM_READ_DONE);
    gl_word(t, TEX_RESTORE_RSS);
}

/* The kernel's restore of one block from the host buffer, as traced. */
static void tram_kernel_restore(QTestState *t, uint32_t block)
{
    const uint32_t pairs[] = {
        TEX_TL_MODE, 0x4026, 0x1a0, 0, 0x18b, 1, TEX_TEXMODE2, 0x11804,
        TEX_TEXMODE1, 0x58, TEX_TL_ADDR, 0, TEX_TL_MIPMAP, block,
        TEX_TL_SPEC, TEX_TL_SPEC_WHOLE,
    };
    const uint32_t copy[] = {
        0x1a9, 0x40, 0x1aa, TRAM_BLOCK_ROWS, 0x1ab, 0, 0x1ac, 0,
    };
    uint32_t count = TRAM_BLOCK_BYTES / 4;

    gl_one(t, TRAM_TEX_BASE_HDR, 0);
    tram_pairs(t, pairs, ARRAY_SIZE(pairs) / 2);
    gl_word(t, TEX_SAVE_RSS);
    gl_word(t, 0xffffffff);
    gl_word(t, 0x1400000);
    gl_word(t, 0);
    gl_word(t, 0);
    gl_word(t, 3);
    tram_pairs(t, copy, ARRAY_SIZE(copy) / 2);
    tram_pairs(t, (const uint32_t[]){ 0x159, 0x400080 }, 1);
    tram_pairs(t, (const uint32_t[]){ 0x153, 0x400040 }, 1);
    tram_pairs(t, (const uint32_t[]){ 0x158, 0x400040 }, 1);
    gl_word(t, TRAM_RESTORE_OPEN);
    gl_word(t, TRAM_GE_COUNT_HDR);
    gl_word(t, (count >> 18) & 0x1ff);
    gl_word(t, (count >> 9) & 0x1ff);
    gl_word(t, count & 0x1ff);
    gl_eram_map(t);
    gl_eram_kick(t, TRAM_BUFFER_OFFSET, TRAM_BLOCK_BYTES, GL_ERAM_WRITE_KICK);
    gl_word(t, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(t, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
    tram_pairs(t, (const uint32_t[]){ 0x156, 0xffffffff }, 1);
    gl_word(t, TEX_RESTORE_RSS);
}

/* Whether the n bytes of want lie in buf, in order, and only zeros else. */
static bool tram_buffer_holds(const uint8_t *buf, size_t len,
                              const uint8_t *want, size_t n)
{
    for (size_t at = 0; at + n <= len; at++) {
        if (!memcmp(buf + at, want, n)) {
            for (size_t i = 0; i < len; i++) {
                if ((i < at || i >= at + n) && buf[i]) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

/*
 * The kernel's save of a block returns the texels a load put there: the
 * 16 KiB read holds the RGBA8 level's 32 bytes, R, G, B and A of each
 * texel in order, and zeros elsewhere, where TRAM has held nothing since
 * reset. Which offset in the block the level takes is the model's own
 * layout and is not asserted.
 */
static void test_tram_save_returns_the_block(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    g_autofree uint8_t *buffer = g_malloc(TRAM_BLOCK_BYTES);
    uint8_t texels[32];

    tex_texels(texels);
    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    memset(buffer, TRAM_BUFFER_FILL, TRAM_BLOCK_BYTES);
    qtest_memwrite(t, TRAM_BUFFER, buffer, TRAM_BLOCK_BYTES);

    gl_enter_slot(t, GL_SLOT_DEFAULT);
    tram_kernel_save(t, 0);
    qtest_memread(t, TRAM_BUFFER, buffer, TRAM_BLOCK_BYTES);
    g_assert_true(tram_buffer_holds(buffer, TRAM_BLOCK_BYTES, texels,
                                    sizeof(texels)));
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * Every context loads into the one TRAM: context B, under the same base,
 * loads its texels to the block context A drew from, and A then draws B's,
 * which is why the kernel saves and restores. A level that would run past
 * TRAM's 256 blocks (a 1024-by-1024 RGBA8 level, all 4 MiB, at block 1)
 * is refused and draws untextured.
 */
#define TEX_SPEC_1024         0xaa0

static void test_tram_load_is_one_store(void)
{
    static const uint32_t box[6] = { 0, 0, 1024, 1024, 1024 << 16 | 1024, 0 };
    static const uint32_t at_block_1[6] = { 1024, 0, 0, 1, 0, 0 };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t first[32], second[32], row[4096];

    tex_texels(first);
    for (int i = 0; i < 32; i++) {
        second[i] = i % 4 == 3 ? 0xff : first[i] ^ 0xa0;
    }
    tex_load(t, TEX_TL_MODE_RGBA8, first);

    gl_enter_slot(t, GL_SLOT_B);
    gl_word(t, GL_CONTEXT_OPENER);
    tex_load(t, TEX_TL_MODE_RGBA8, second);

    gl_enter_slot(t, GL_SLOT_A);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 3, 1), ==,
                    second[28] << 16 | second[29] << 8 | second[30]);

    memset(row, 0x5a, sizeof(row));
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
    gl_ge_load(t, GL_GE_ERAM, 0xe3d, at_block_1, 6);
    tex_load_full(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_1024, row, 1024, 1024,
                  box, true, 0, sizeof(row));
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_records(t, (const uint32_t[]){ TEX_TXSIZE, 0xaaaa }, 1);
    gl_word(t, TEX_TABLE);
    gl_word(t, 0xc52);
    gl_word(t, 0xc53);
    gl_word(t, TEX_TXMIPMAP);
    gl_word(t, 0);
    tex_records(t, (const uint32_t[]){ TEX_TXADDR, 0 }, 1);
    gl_word(t, TEX_TABLE);
    gl_word(t, 0xc52);
    gl_word(t, 0xc53);
    gl_word(t, TEX_TXMIPMAP);
    gl_word(t, 1);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, TEX_WHITE);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * MXI and EMXI: a TE1 and its TRAM on each of two REs. Save and restore
 * around a context switch, on SI+T too, which sends the same words, as
 * the kernel does when another context's run overlaps: A loads and draws,
 * the kernel saves A's block with no GL context loaded, B loads its own
 * texels over it, the kernel restores A's block, and A draws its own
 * texels again on both rows with no fresh bind, only the 0x0b00 its next
 * draw sends: the kernel's register pairs leave A's texture state alone.
 * Neither transfer touches the planes, here under a raster box left over
 * screen row 0. A 16 KiB DMA row cannot fill that box or any box on this
 * screen (it would be 4,096 pixels a row), so a restore drawn as a raster
 * image would leave the pixels alone; what it does leave is a "DMA image
 * transfer" line under -d unimp, which the test requires to be absent.
 */
static void test_mxi_tram_save_restore(gconstpointer data)
{
    g_autofree uint8_t *buffer = g_malloc(TRAM_BLOCK_BYTES);
    g_autofree char *tmpfs = NULL;
    g_autofree char *log = NULL;
    g_autofree char *args = NULL;
    g_autofree char *contents = NULL;
    GError *err = NULL;
    uint8_t texels[32], other[32];
    uint32_t before[4];
    QTestState *t;

    tmpfs = g_dir_make_tmp("sn0-mgras-tram-restore-XXXXXX", &err);
    g_assert_no_error(err);
    log = g_strdup_printf("%s/unimp.log", tmpfs);
    args = g_strdup_printf(MACHINE_ARGS ",product=%s -d unimp -D %s",
                           (const char *)data, log);
    t = tex_start(args);

    tex_texels(texels);
    for (int i = 0; i < 32; i++) {
        other[i] = i % 4 == 3 ? 0xff : texels[i] ^ 0x5a;
    }
    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    mxi_two_row_quad(t);
    mxi_expect_rows(t, texels, true);

    qtest_writeq(t, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, 0));
    qtest_writeq(t, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (127u << 16) | 0));
    for (int i = 0; i < 4; i++) {
        before[i] = mgras_test_pixel(t, 32 * i + 3, 0);
    }

    gl_enter_slot(t, GL_SLOT_DEFAULT);
    tram_kernel_save(t, 0);
    qtest_memread(t, TRAM_BUFFER, buffer, TRAM_BLOCK_BYTES);
    g_assert_true(tram_buffer_holds(buffer, TRAM_BLOCK_BYTES, texels,
                                    sizeof(texels)));

    gl_enter_slot(t, GL_SLOT_B);
    gl_word(t, GL_CONTEXT_OPENER);
    tex_load(t, TEX_TL_MODE_RGBA8, other);

    gl_enter_slot(t, GL_SLOT_DEFAULT);
    tram_kernel_restore(t, 0);
    for (int i = 0; i < 4; i++) {
        g_assert_cmphex(mgras_test_pixel(t, 32 * i + 3, 0), ==, before[i]);
    }

    gl_enter_slot(t, GL_SLOT_A);
    gl_word(t, TEX_ON);
    mxi_two_row_quad(t);
    mxi_expect_rows(t, texels, true);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);

    g_assert_true(g_file_get_contents(log, &contents, NULL, &err));
    g_assert_no_error(err);
    g_assert_null(strstr(contents, "DMA image transfer"));
    unlink(log);
    g_rmdir(tmpfs);
}

#define TRAM_SAVE_ONE_ROWS       8192u
#define TRAM_SAVE_HALF_ROWS      4096u
#define TRAM_SAVE_ONE_BLOCKS     (TRAM_SAVE_ONE_ROWS / TRAM_BLOCK_ROWS)
#define TRAM_SAVE_HALF_BLOCKS    (TRAM_SAVE_HALF_ROWS / TRAM_BLOCK_ROWS)
#define TRAM_SAVE_ONE_BYTES      (TRAM_SAVE_ONE_ROWS * 64u * 4u)
#define TRAM_SAVE_HALF_BYTES     (TRAM_SAVE_HALF_ROWS * 64u * 4u)
#define TRAM_SAVE_PAGE_BYTES     4096u
#define TRAM_SAVE_PAGES          ((TRAM_BUFFER_OFFSET + \
                                   TRAM_SAVE_ONE_BYTES + \
                                   TRAM_SAVE_PAGE_BYTES - 1) / \
                                  TRAM_SAVE_PAGE_BYTES)
#define TRAM_SAVE_LIST           0x00300000u

/* Slot 2 maps enough pages for the complete save buffer. */
static void tram_save_map(QTestState *t)
{
    uint8_t header[24] = { 0 };
    g_autofree uint8_t *list = g_malloc0(TRAM_SAVE_PAGES * 8);

    for (uint32_t i = 0; i < TRAM_SAVE_PAGES; i++) {
        stq_be_p(list + i * 8, 7 + i);
    }
    qtest_memwrite(t, TRAM_SAVE_LIST, header, sizeof(header));
    qtest_memwrite(t, TRAM_SAVE_LIST + sizeof(header), list,
                   TRAM_SAVE_PAGES * 8);
    qtest_writel(t, MGRAS_REG(0x130400), 0x00482408);
    qtest_writeq(t, MGRAS_REG(0x130400),
                 TRAM_SAVE_LIST + sizeof(header));
    qtest_writeq(t, MGRAS_REG(0x130400),
                 ((uint64_t)0x00483204 << 32) | TRAM_SAVE_PAGES);
    qtest_writeq(t, MGRAS_REG(0x130400),
                 ((uint64_t)0x00484d04 << 32) | (GL_PIX_DMA_WIDGET << 16));
}

static void tram_assert_save_bytes(const char *shape, const uint8_t *actual,
                                   const uint8_t *expected, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (actual[i] != expected[i]) {
            g_test_message("%s first differs at byte 0x%zx: got 0x%02x, "
                           "expected 0x%02x", shape, i, actual[i],
                           expected[i]);
            break;
        }
    }
    g_assert_cmpmem(actual, len, expected, len);
}

/* Distinguish each saved 16 KiB block without assuming its TRAM contents. */
static void tram_fill_save_block(uint8_t block[TRAM_BLOCK_BYTES],
                                 uint32_t number)
{
    for (uint32_t i = 0; i < TRAM_BLOCK_BYTES; i++) {
        block[i] = number * 53 + i * 29 + (i >> 8);
    }
}

/*
 * The traced save words describe 64 rows of 64 four-byte locations per
 * 16 KiB block. On a two-RE board, compare one 8,192-row pass against two
 * 4,096-row passes, whose second TXMIPMAP entry starts at block 64. The
 * expected bytes come from the known blocks written before the saves, not
 * from either save result; the byte positions inside a block are the
 * model's stated approximation.
 */
static void test_mxi_tram_save_shapes(void)
{
    g_autofree uint8_t *one = g_malloc(TRAM_SAVE_ONE_BYTES);
    g_autofree uint8_t *first = g_malloc(TRAM_SAVE_HALF_BYTES);
    g_autofree uint8_t *second = g_malloc(TRAM_SAVE_HALF_BYTES);
    g_autofree uint8_t *expected = g_malloc(TRAM_SAVE_ONE_BYTES);
    uint8_t block[TRAM_BLOCK_BYTES];
    QTestState *t = mxi_start("mxi");

    g_assert_cmpuint(TRAM_SAVE_ONE_BLOCKS, ==, 128);
    g_assert_cmpuint(TRAM_SAVE_HALF_BLOCKS, ==, 64);
    for (uint32_t n = 0; n < TRAM_SAVE_ONE_BLOCKS; n++) {
        tram_fill_save_block(block, n);
        memcpy(expected + n * TRAM_BLOCK_BYTES, block, sizeof(block));
        qtest_memwrite(t, TRAM_BUFFER, block, sizeof(block));
        gl_enter_slot(t, GL_SLOT_DEFAULT);
        tram_kernel_restore(t, n);
    }

    gl_enter_slot(t, GL_SLOT_DEFAULT);
    tram_kernel_save_rows(t, 0, TRAM_SAVE_ONE_ROWS, TRAM_SAVE_ONE_BYTES);
    qtest_memread(t, TRAM_BUFFER, one, TRAM_SAVE_ONE_BYTES);
    tram_assert_save_bytes("one-pass save", one, expected,
                           TRAM_SAVE_ONE_BYTES);

    gl_enter_slot(t, GL_SLOT_DEFAULT);
    tram_kernel_save_rows(t, 0, TRAM_SAVE_HALF_ROWS, TRAM_SAVE_HALF_BYTES);
    qtest_memread(t, TRAM_BUFFER, first, TRAM_SAVE_HALF_BYTES);
    tram_assert_save_bytes("first half-pass save", first, expected,
                           TRAM_SAVE_HALF_BYTES);

    gl_enter_slot(t, GL_SLOT_DEFAULT);
    tram_kernel_save_rows(t, TRAM_SAVE_HALF_BLOCKS, TRAM_SAVE_HALF_ROWS,
                          TRAM_SAVE_HALF_BYTES);
    qtest_memread(t, TRAM_BUFFER, second, TRAM_SAVE_HALF_BYTES);
    tram_assert_save_bytes("second half-pass save", second,
                           expected + TRAM_SAVE_HALF_BYTES,
                           TRAM_SAVE_HALF_BYTES);
    g_assert_cmpmem(one, TRAM_SAVE_HALF_BYTES, first, TRAM_SAVE_HALF_BYTES);
    g_assert_cmpmem(one + TRAM_SAVE_HALF_BYTES, TRAM_SAVE_HALF_BYTES,
                    second, TRAM_SAVE_HALF_BYTES);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/* DMA-map slot 1's page list, eight pages from frame first, as the kernel. */
#define TRAM_SLOT1_LIST       0x3000u

static void tram_slot1_map(QTestState *t, uint32_t first)
{
    static const uint8_t header[24] = { 0 };
    uint8_t list[8 * 8];

    for (int i = 0; i < 8; i++) {
        stq_be_p(list + 8 * i, first + i);
    }
    qtest_memwrite(t, TRAM_SLOT1_LIST, header, sizeof(header));
    qtest_memwrite(t, TRAM_SLOT1_LIST + 0x18, list, sizeof(list));
    /* DMA-map slot 1: registers 0x22/0x23, 0x31 and 0x4c. */
    qtest_writel(t, MGRAS_REG(0x130400), 0x00482208);
    qtest_writeq(t, MGRAS_REG(0x130400), TRAM_SLOT1_LIST + 0x18);
    qtest_writeq(t, MGRAS_REG(0x130400), ((uint64_t)0x00483104 << 32) | 8);
    qtest_writeq(t, MGRAS_REG(0x130400),
                 ((uint64_t)0x00484c04 << 32) | (GL_PIX_DMA_WIDGET << 16));
}

/*
 * The client's GE transfer: 0x9d10 with bytes / 4 in three 9-bit parts and
 * the GE routine, then the HQ page-list form over slot 1's entries first
 * to first + pages - 1, whole pages, kicked 0xa3, and the fence.
 */
static void tram_ge_dma(QTestState *t, uint32_t routine, uint32_t bytes,
                        uint32_t first)
{
    uint32_t words = bytes / 4, pages = bytes / 0x1000;
    uint32_t list[4] = { 0 };

    gl_word(t, TRAM_GE_DMA_HDR);
    gl_word(t, (words >> 18) & 0x1ff);
    gl_word(t, (words >> 9) & 0x1ff);
    gl_word(t, words & 0x1ff);
    gl_word(t, routine);
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t e = (first + i) | (i == pages - 1 ? 0x8000 : 0);

        list[i / 2] |= e << (i % 2 ? 0 : 16);
    }
    gl_word(t, 0x00080404);
    gl_word(t, 0x1000);
    gl_word(t, 0x00080604);
    gl_word(t, 0);
    gl_word(t, 0x00080010);
    for (int i = 0; i < 4; i++) {
        gl_word(t, list[i]);
    }
    gl_word(t, 0x00080504);
    gl_word(t, 0);
    gl_word(t, 0x00080804);
    gl_word(t, 0x1000);
    gl_word(t, 0x00080904);
    gl_word(t, 0x1000);
    gl_word(t, 0x00080704);
    gl_word(t, 1);
    qtest_writel(t, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_FENCE);
    gl_word(t, 0x00080b04);
    gl_word(t, 0xa3);
    gl_word(t, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(t, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    MGRAS_HQ_FLAG_FENCE);
}

/* TL_ADDR 0 and TL_MIPMAP entry 0 = block, as the client's restore. */
static void tram_tl_mipmap(QTestState *t, uint32_t block)
{
    tex_record(t, TEX_TL_ADDR, 0);
    gl_word(t, TEX_TABLE);
    gl_word(t, 0xc65);
    gl_word(t, 0xc66);
    gl_word(t, TEX_TL_MIPMAP);
    gl_word(t, block);
}

/*
 * The client's own save and restore of a texture its manager evicts, as
 * the IP27GR532 client sent them for an RGB texture in a captured run:
 * the read through the kernel (0xe50c, format 0x400070) takes three bytes
 * a location, 12 KiB for a block's 64 rows, and holds the level's RGB
 * bytes in order; a texture loaded over it replaces it; and the restore,
 * a load under TL_SPEC 0xd60 of 64 rows 64 wide at TL_MIPMAP's block, its
 * bytes through 0x9d10 for routine 0x511a and the page-list form from
 * slot 1, puts the first texture back, which then draws. A model that took
 * the restore's kick as a raster image, as before, draws the second.
 */
#define TEX_GE_LOAD_ROUTINE   0x511a
#define TRAM_RGB_BLOCK_BYTES  0x3000

static void test_tram_client_restore_round_trip(void)
{
    static const uint32_t box[6] = { 0, 0, 4, 2, 2 << 16 | 4, 0 };
    static const uint32_t rows[6] = { 0, 0, 64, 64, 64 << 16 | 64, 0 };
    QTestState *t = tex_start(TEX_ARGS);
    g_autofree uint8_t *buffer = g_malloc(TRAM_RGB_BLOCK_BYTES);
    uint8_t texels[32], first[24], second[24];

    tex_texels(texels);
    for (int i = 0; i < 8; i++) {
        memcpy(&first[3 * i], &texels[4 * i], 3);
        for (int k = 0; k < 3; k++) {
            second[3 * i + k] = first[3 * i + k] ^ 0x5a;
        }
    }
    tex_load_fmt(t, true, TEX_TL_MODE_RGB8, TEX_SPEC_4X2, first, 4, 2, 3, 3,
                 box, true, 0, 0);
    memset(buffer, TRAM_BUFFER_FILL, TRAM_RGB_BLOCK_BYTES);
    qtest_memwrite(t, TRAM_BUFFER, buffer, TRAM_RGB_BLOCK_BYTES);

    tex_mipmap_word(t, 0);
    tram_save_rss(t);
    tram_read(t, TRAM_BLOCK_ROWS, TRAM_FORMAT_RGB8, TRAM_RGB_BLOCK_BYTES,
              GL_ERAM_READ_KICK);
    qtest_memread(t, TRAM_BUFFER, buffer, TRAM_RGB_BLOCK_BYTES);
    g_assert_true(tram_buffer_holds(buffer, TRAM_RGB_BLOCK_BYTES, first,
                                    sizeof(first)));

    tex_load_fmt(t, false, 0, TEX_SPEC_4X2, second, 4, 2, 3, 3, box, true, 0,
                 0);
    tex_bind(t, TEX_TEXMODE1_RGB_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==,
                    second[0] << 16 | second[1] << 8 | second[2]);

    /* The saved bytes sit at frame 0xb, entry 4 of slot 1's list from 7. */
    tram_slot1_map(t, 7);
    tex_record(t, TEX_TL_MODE, 0x4024);
    tram_tl_mipmap(t, 0);
    tex_record(t, TEX_TL_SPEC, TEX_TL_SPEC_WHOLE);
    tram_save_rss(t);
    gl_ge_load(t, GL_GE_ERAM, GL_GE_BOX, rows, 6);
    tram_ge_dma(t, TEX_GE_LOAD_ROUTINE, TRAM_RGB_BLOCK_BYTES, 4);
    gl_word(t, TEX_RESTORE_RSS);

    tex_bind(t, TEX_TEXMODE1_RGB_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    for (int i = 0; i < 8; i++) {
        const uint8_t *p = &first[3 * i];

        g_assert_cmphex(tex_texel_pixel(t, i % 4, i / 4), ==,
                        p[0] << 16 | p[1] << 8 | p[2]);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * The client's save and restore of a one-component texture under a TRAM
 * select, as perfly's town database sends them: the save reads one byte a
 * TRAM word over the block's 64 rows, with the texture's select in
 * TEXMODE1 (0x140, select 2) and another in TL_MODE (0x18, select 3), as
 * the client's save routine sends them, and returns the texels in order;
 * another texture loaded over it under the same select replaces it; the
 * restore, under TL_MODE 0x4030 (select 2) as traced, puts the first back
 * where a select-2 draw finds it. A load of an RGBA level over the block
 * then rewrites every TRAM there, so the select-2 draw finds no level and
 * draws untextured rather than the old texels.
 */
#define TRAM_LUM8_BLOCK_BYTES 0x1000

static void test_tram_client_restore_under_select(void)
{
    static const uint32_t rows[6] = { 0, 0, 64, 64, 64 << 16 | 64, 0 };
    QTestState *t = tex_start(TEX_ARGS);
    g_autofree uint8_t *buffer = g_malloc(TRAM_LUM8_BLOCK_BYTES);
    uint8_t first[8], second[8], rgba[32];

    for (int i = 0; i < 8; i++) {
        first[i] = 0x10 + 0x20 * i;
        second[i] = 0xf0 - 0x18 * i;
    }
    tex_enter(t, GL_SLOT_A);
    tex_select_load(t, 2, first);
    memset(buffer, TRAM_BUFFER_FILL, TRAM_LUM8_BLOCK_BYTES);
    qtest_memwrite(t, TRAM_BUFFER, buffer, TRAM_LUM8_BLOCK_BYTES);
    tex_mipmap_word(t, 0);
    tex_record(t, TEX_TL_MODE, 0x18);
    tex_record(t, TEX_TEXMODE1, 0x140);
    tram_save_rss(t);
    tram_read(t, TRAM_BLOCK_ROWS, TRAM_FORMAT_RGBA8, TRAM_LUM8_BLOCK_BYTES,
              GL_ERAM_READ_KICK);
    qtest_memread(t, TRAM_BUFFER, buffer, TRAM_LUM8_BLOCK_BYTES);
    g_assert_true(tram_buffer_holds(buffer, TRAM_LUM8_BLOCK_BYTES, first,
                                    sizeof(first)));

    tex_select_load(t, 2, second);
    tex_select_expect(t, 2, second);

    tram_slot1_map(t, 7);
    tex_record(t, TEX_TL_MODE, 0x4030);
    tram_tl_mipmap(t, 0);
    tex_record(t, TEX_TL_SPEC, TEX_TL_SPEC_WHOLE);
    tram_save_rss(t);
    gl_ge_load(t, GL_GE_ERAM, GL_GE_BOX, rows, 6);
    tram_ge_dma(t, TEX_GE_LOAD_ROUTINE, TRAM_LUM8_BLOCK_BYTES, 4);
    gl_word(t, TEX_RESTORE_RSS);
    tex_select_expect(t, 2, first);

    tex_texels(rgba);
    tex_load(t, TEX_TL_MODE_RGBA8, rgba);
    tex_select_expect(t, 2, NULL);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * A 0x9d10 for any other routine, or outside the whole-TRAM shape, opens a
 * transfer to the GE the model does not take: its kick moves nothing,
 * here under a raster box over screen row 0 1,024 pixels wide that would
 * take a 4 KiB page of slot 1, the listed one or the first, as 1,024
 * pixels of 0x5a bytes.
 */
static void test_tram_ge_dma_is_not_a_raster_image(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t fill[0x1000];
    uint32_t before[4];

    memset(fill, 0x5a, sizeof(fill));
    qtest_memwrite(t, 0x7000, fill, sizeof(fill));
    qtest_memwrite(t, 0xb000, fill, sizeof(fill));
    tram_slot1_map(t, 7);
    qtest_writeq(t, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_BLOCK_START, 0));
    qtest_writeq(t, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_END, (1023u << 16) | 0));
    for (int i = 0; i < 4; i++) {
        before[i] = mgras_test_pixel(t, 256 * i + 3, 0);
    }
    tex_record(t, TEX_TL_SPEC, TEX_SPEC_4X2);
    tram_ge_dma(t, 0x49d0, 0x1000, 4);
    for (int i = 0; i < 4; i++) {
        g_assert_cmphex(mgras_test_pixel(t, 256 * i + 3, 0), ==, before[i]);
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * A save whose read kick never came does not take the next read that is
 * not its own: after a lone 0xe50c, a raster read of three pixels of
 * screen row 0, 12 bytes and so not whole words of 64-word rows, still
 * returns the pixels.
 */
static void test_tram_open_save_ends_at_other_kick(void)
{
    static const uint8_t want[12] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0xf0, 0xe0, 0xd0,
    };
    uint8_t got[12];
    QTestState *t = tex_start(TEX_ARGS);

    memset(got, 0xee, sizeof(got));
    qtest_memwrite(t, 0x7000, got, sizeof(got));
    gl_eram_map(t);
    qtest_writeq(t, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_COLORMASKMSBS, 0));
    qtest_writeq(t, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_COLORMASKLSBSA, 0x00ffffff));
    qtest_writeq(t, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, 0x0c006204));
    qtest_writeq(t, REG_CONTEXT_COMMAND, MGRAS_COMMAND(REG_FILLMODE, 0));
    qtest_writeq(t, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PACKEDCOLOR, 0x00f0e0d0));
    qtest_writeq(t, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_RE_POINT, (2u << 16) | 0u));

    gl_word(t, TRAM_READ_HDR);
    gl_word(t, TRAM_BLOCK_ROWS);
    gl_word(t, TRAM_FORMAT_RGBA8);
    gl_word(t, TRAM_READ_LAST_WORD);

    qtest_writeq(t, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_PP1FILLMODE, 0x00002200));
    qtest_writeq(t, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND(REG_BLOCK_START, (0u << 16) | 1023u));
    qtest_writeq(t, REG_CONTEXT_COMMAND,
                 MGRAS_COMMAND_EXEC(REG_BLOCK_END, (2u << 16) | 1023u));
    gl_word(t, 0x00080404);
    gl_word(t, 12);
    gl_word(t, 0x00080504);
    gl_word(t, 0);
    gl_word(t, 0x00080604);
    gl_word(t, 0);
    gl_word(t, 0x00080704);
    gl_word(t, 1);
    gl_word(t, 0x00080804);
    gl_word(t, 12);
    gl_word(t, 0x00080b04);
    gl_word(t, GL_ERAM_READ_KICK);
    qtest_memread(t, 0x7000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), want, sizeof(want));
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * Register a texture case on each board with texture. si-t and mxi, a board
 * with one RE and one with two, run in every setup; their E-series
 * counterparts differ from them in GE and PP1 revision, so the thorough
 * setup adds them.
 */
static void add_texture_board_cases(const char *path, GTestDataFunc fn)
{
    static const struct {
        const char *board;
        bool quick;
    } boards[] = {
        { "si-t", true }, { "esi-t", false }, { "mxi", true },
        { "emxi", false },
    };

    for (size_t i = 0; i < ARRAY_SIZE(boards); i++) {
        g_autofree char *name = NULL;

        if (!boards[i].quick && !g_test_slow()) {
            continue;
        }
        name = g_strdup_printf("%s/%s", path, boards[i].board);
        qtest_add_data_func(name, boards[i].board, fn);
    }
}

#define TEX_SB_BASE_HDR 0x0000eb04
#define TEX_SB_CLAMP_BITS 0x00001800
#define TEX_SB_SPEC 0x110
#define TEX_SB_TEXSIZE_2X2 0x1111
#define TEX_SB_BLOCK_REG 0x0e3d

typedef struct TexStoredBorderSample {
    float s, t;
} TexStoredBorderSample;

static const uint8_t tex_sb_perimeter[12][4] = {
    { 17, 31, 47, 255 }, { 37, 71, 109, 255 },
    { 59, 113, 173, 255 }, { 83, 157, 239, 255 },
    { 107, 199, 53, 255 }, { 131, 241, 97, 255 },
    { 157, 61, 139, 255 }, { 181, 103, 191, 255 },
    { 211, 149, 23, 255 }, { 233, 197, 67, 255 },
    { 251, 227, 127, 255 }, { 73, 17, 211, 255 },
};

static const int tex_sb_perimeter_xy[12][2] = {
    { -1, -1 }, { 0, -1 }, { 1, -1 }, { 2, -1 },
    { 2, 0 }, { 2, 1 }, { 2, 2 }, { 1, 2 },
    { 0, 2 }, { -1, 2 }, { -1, 1 }, { -1, 0 },
};

static const TexStoredBorderSample tex_sb_linear_samples[12] = {
    { 0, 0.25f }, { 0, 0.75f }, { 1, 0.25f }, { 1, 0.75f },
    { 0.25f, 0 }, { 0.75f, 0 }, { 0.25f, 1 }, { 0.75f, 1 },
    { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 },
};

static void tex_sb_fill(uint8_t image[64], uint8_t core[16], uint8_t salt)
{
    static const uint8_t core_rgb[4][3] = {
        { 255, 0, 0 }, { 0, 255, 0 },
        { 0, 0, 255 }, { 255, 255, 255 },
    };

    memset(image, 0, 64);
    for (unsigned i = 0; i < ARRAY_SIZE(tex_sb_perimeter_xy); i++) {
        int x = tex_sb_perimeter_xy[i][0];
        int y = tex_sb_perimeter_xy[i][1];
        uint8_t *p = &image[4 * ((y + 1) * 4 + x + 1)];

        p[0] = tex_sb_perimeter[i][0] ^ salt;
        p[1] = tex_sb_perimeter[i][1];
        p[2] = tex_sb_perimeter[i][2];
        p[3] = 0xff;
    }
    for (unsigned i = 0; i < 4; i++) {
        core[4 * i] = core_rgb[i][0];
        core[4 * i + 1] = core_rgb[i][1];
        core[4 * i + 2] = core_rgb[i][2];
        core[4 * i + 3] = 0xff;
    }
    for (unsigned y = 0; y < 2; y++) {
        for (unsigned x = 0; x < 2; x++) {
            memcpy(&image[4 * ((y + 1) * 4 + x + 1)],
                   &core[4 * (y * 2 + x)], 4);
        }
    }
}

static void tex_sb_upload_rect(QTestState *t, const uint8_t *pixels,
                               uint32_t w, uint32_t h, int32_t x, int32_t y)
{
    const uint32_t box[6] = {
        x, y, w, h, h << 16 | w, 0,
    };

    tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_SB_SPEC, pixels, w, h,
                box, true);
}

static void tex_sb_set_base_block(QTestState *t, uint32_t base,
                                  uint32_t block)
{
    const uint32_t ge_block[6] = { 4, 0, 0, block, block - 1, 0 };

    gl_one(t, TEX_SB_BASE_HDR, base);
    gl_ge_load(t, GL_GE_ERAM, TEX_SB_BLOCK_REG, ge_block,
               ARRAY_SIZE(ge_block));
}

static void tex_sb_upload_core(QTestState *t, const uint8_t core[16])
{
    tex_sb_upload_rect(t, core, 2, 2, 0, 0);
}

static void tex_sb_upload_complete(QTestState *t, uint32_t base,
                                   uint32_t block, uint8_t salt,
                                   uint8_t image[64], uint8_t core[16])
{
    tex_sb_fill(image, core, salt);
    tex_sb_set_base_block(t, base, block);
    tex_sb_upload_rect(t, image, 4, 4, -1, -1);
    tex_sb_upload_core(t, core);
}

static void tex_sb_bind(QTestState *t, bool linear)
{
    uint32_t filters = linear ? TEX_MAG_LINEAR | TEX_MIN_LINEAR : 0;

    tex_bind(t, TEX_TEXMODE1_ON,
             TEX_TEXMODE2_RGBA8 | TEX_SB_CLAMP_BITS | filters);
    tex_records(t, (const uint32_t[]) {
        TEX_TXSIZE, TEX_SB_TEXSIZE_2X2,
        TEX_TXCLAMPSIZE, 2 << 13 | 2,
    }, 2);
    tex_mipmap_word(t, 4);
}

static uint32_t tex_sb_draw(QTestState *t, float s, float v, bool linear)
{
    static const float white[4] = { 1, 1, 1, 1 };
    float quad[4][4] = {
        { 16, 16, s, v }, { 32, 16, s, v },
        { 32, 32, s, v }, { 16, 32, s, v },
    };

    tex_sb_bind(t, linear);
    tex_quad_rect(t, white, (const float (*)[4])quad);
    return gl_window_pixel(t, 24, 24);
}

static const uint8_t *tex_sb_texel(const uint8_t image[64], int x, int y)
{
    return &image[4 * ((y + 1) * 4 + x + 1)];
}

static void tex_sb_linear_expected(const uint8_t image[64], float s, float t,
                                   double want[3])
{
    double x = 2.0 * s - 0.5;
    double y = 2.0 * t - 0.5;
    int x0 = (int)floor(x), y0 = (int)floor(y);
    double fx = x - x0, fy = y - y0;
    double sum[3] = { 0, 0, 0 };

    for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
            const uint8_t *p = tex_sb_texel(image, x0 + dx, y0 + dy);
            double wx = dx ? fx : 1.0 - fx;
            double wy = dy ? fy : 1.0 - fy;

            for (int c = 0; c < 3; c++) {
                sum[c] += wx * wy * p[c] / 255.0;
            }
        }
    }
    memcpy(want, sum, sizeof(sum));
}

static void tex_sb_expect_linear(QTestState *t, const uint8_t image[64],
                                 float s, float v)
{
    double want[3];
    uint32_t got;

    tex_sb_linear_expected(image, s, v, want);
    got = tex_sb_draw(t, s, v, true);
    if (abs((int)((got >> 16) & 0xff) - (int)lround(want[0] * 255)) > 1 ||
        abs((int)((got >> 8) & 0xff) - (int)lround(want[1] * 255)) > 1 ||
        abs((int)(got & 0xff) - (int)lround(want[2] * 255)) > 1) {
        g_test_message("stored-border linear sample (s=%.3f,t=%.3f): "
                       "got #%06x, want %.3f,%.3f,%.3f",
                       s, v, got, want[0], want[1], want[2]);
    }
    tex_expect(got, want[0], want[1], want[2]);
}

static void tex_sb_expect_nearest(QTestState *t, const uint8_t image[64],
                                  float s, float v)
{
    int x = (int)floor(2.0 * s), y = (int)floor(2.0 * v);
    const uint8_t *p = tex_sb_texel(image, x, y);
    uint32_t got = tex_sb_draw(t, s, v, false);

    if (abs((int)((got >> 16) & 0xff) - p[0]) > 1 ||
        abs((int)((got >> 8) & 0xff) - p[1]) > 1 ||
        abs((int)(got & 0xff) - p[2]) > 1) {
        g_test_message("stored-border nearest sample (s=%.3f,t=%.3f): "
                       "texel (%d,%d), got #%06x, want #%02x%02x%02x",
                       s, v, x, y, got, (unsigned)p[0],
                       (unsigned)p[1], (unsigned)p[2]);
    }
    tex_expect(got, p[0] / 255.0, p[1] / 255.0, p[2] / 255.0);
}

static void tex_sb_expect_all_linear(QTestState *t, const uint8_t image[64])
{
    for (unsigned i = 0; i < ARRAY_SIZE(tex_sb_linear_samples); i++) {
        tex_sb_expect_linear(t, image, tex_sb_linear_samples[i].s,
                             tex_sb_linear_samples[i].t);
    }
}

static void test_gl_texture_stored_border_perimeter_samples(gconstpointer data)
{
    static const float centers[4] = { -0.25f, 0.25f, 0.75f, 1.0f };
    QTestState *t = texture_board_start(data);
    uint8_t image[64], core[16];

    tex_sb_upload_complete(t, 0, 4, 0, image, core);
    tex_sb_expect_all_linear(t, image);
    for (unsigned y = 0; y < ARRAY_SIZE(centers); y++) {
        for (unsigned x = 0; x < ARRAY_SIZE(centers); x++) {
            tex_sb_expect_nearest(t, image, centers[x], centers[y]);
        }
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void tex_sb_upload_left_strip(QTestState *t, const uint8_t image[64])
{
    uint8_t strip[16];

    for (unsigned y = 0; y < 4; y++) {
        memcpy(&strip[4 * y], &image[4 * 4 * y], 4);
    }
    tex_sb_upload_rect(t, strip, 1, 4, -1, -1);
}

static void tex_sb_upload_other_strips(QTestState *t, const uint8_t image[64])
{
    uint8_t strip[16];

    memcpy(&strip[0], &image[4 * 1], 8);
    tex_sb_upload_rect(t, strip, 2, 1, 0, -1);
    memcpy(&strip[0], &image[4 * 13], 8);
    tex_sb_upload_rect(t, strip, 2, 1, 0, 2);
    for (unsigned y = 0; y < 4; y++) {
        memcpy(&strip[4 * y], &image[4 * (4 * y + 3)], 4);
    }
    tex_sb_upload_rect(t, strip, 1, 4, 2, -1);
}

static void test_gl_texture_stored_border_partial_and_updates(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t image[64], core[16], replacement[16];

    tex_sb_fill(image, core, 0);
    tex_sb_set_base_block(t, 0, 4);
    tex_sb_upload_left_strip(t, image);
    tex_sb_upload_core(t, core);
    tex_expect(tex_sb_draw(t, 0.25f, 0, true), 0.5, 0, 0);
    tex_sb_upload_other_strips(t, image);
    tex_sb_expect_all_linear(t, image);

    memset(core, 0, sizeof(core));
    for (unsigned i = 0; i < 4; i++) {
        core[4 * i] = 0x30 + 0x20 * i;
        core[4 * i + 1] = 0x90 - 0x10 * i;
        core[4 * i + 2] = 0x40 + 0x10 * i;
        core[4 * i + 3] = 0xff;
    }
    tex_sb_upload_core(t, core);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            uint8_t *p = &image[4 * ((y + 1) * 4 + x + 1)];

            memcpy(p, &core[4 * (y * 2 + x)], 4);
        }
    }
    tex_sb_expect_all_linear(t, image);

    memcpy(replacement, &image[4 * 4], 4);
    replacement[0] ^= 0xff;
    replacement[1] ^= 0x5a;
    tex_sb_upload_rect(t, replacement, 1, 1, -1, 0);
    memcpy(&image[4 * 4], replacement, 4);
    tex_sb_expect_linear(t, image, 0, 0.25f);
    tex_sb_expect_linear(t, image, 1, 0.75f);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void test_gl_texture_stored_border_incomplete_absent_and_reset(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t image[64], core[16];

    tex_sb_fill(image, core, 0);
    tex_sb_set_base_block(t, 0, 4);
    tex_sb_upload_core(t, core);
    tex_expect(tex_sb_draw(t, 0, 0.25f, true), 0.5, 0, 0);
    tex_sb_upload_left_strip(t, image);
    tex_sb_upload_core(t, core);
    tex_expect(tex_sb_draw(t, 0, 0.25f, true), 0.5, 0, 0);
    tex_sb_upload_other_strips(t, image);
    tex_sb_expect_all_linear(t, image);
    qtest_system_reset(t);
    gl_enter_slot(t, GL_SLOT_A);
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
    tex_sb_set_base_block(t, 0, 4);
    tex_sb_upload_core(t, core);
    tex_expect(tex_sb_draw(t, 0, 0.25f, true), 0.5, 0, 0);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void test_gl_texture_stored_border_base_context_isolation(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t a[64], acore[16], b[64], bcore[16], c[64], ccore[16];

    tex_sb_upload_complete(t, 0, 4, 0, a, acore);
    tex_sb_upload_complete(t, 1, 4, 0x40, b, bcore);
    tex_sb_set_base_block(t, 0, 4);
    tex_sb_expect_all_linear(t, a);
    tex_sb_set_base_block(t, 1, 4);
    tex_sb_expect_all_linear(t, b);
    tex_sb_set_base_block(t, 0, 4);
    tex_sb_expect_all_linear(t, a);

    gl_enter_slot(t, GL_SLOT_B);
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
    tex_sb_upload_complete(t, 2, 4, 0x80, c, ccore);
    tex_sb_set_base_block(t, 2, 4);
    tex_sb_expect_all_linear(t, c);
    gl_enter_slot(t, GL_SLOT_A);
    tex_sb_set_base_block(t, 0, 4);
    tex_sb_expect_all_linear(t, a);
    tex_sb_set_base_block(t, 1, 4);
    tex_sb_expect_all_linear(t, b);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void test_gl_texture_stored_border_overlapping_base_invalidates(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t a[64], acore[16], b[64], bcore[16];
    uint8_t right_strip[16];

    tex_sb_upload_complete(t, 0, 4, 0, a, acore);
    tex_sb_fill(b, bcore, 0x40);
    tex_sb_set_base_block(t, 1, 3);
    for (unsigned y = 0; y < 4; y++) {
        memcpy(&right_strip[4 * y], &b[4 * (4 * y + 3)], 4);
    }
    tex_sb_upload_rect(t, right_strip, 1, 4, 2, -1);
    tex_sb_set_base_block(t, 0, 4);
    tex_sb_expect_nearest(t, a, 0.25f, 0.25f);
    tex_expect(tex_sb_draw(t, 1, 0.25f, true),
               0, acore[5] / 510.0, 0);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void test_gl_texture_stored_border_bad_upload(void)
{
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t image[64], core[16], oversized_row[5 * 4];
    const uint32_t too_wide[6] = { -1, 0, 5, 1, 1 << 16 | 5, 0 };

    tex_sb_upload_complete(t, 0, 4, 0, image, core);
    tex_sb_expect_linear(t, image, 0, 0.25f);

    memset(oversized_row, 0xa5, sizeof(oversized_row));
    tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_SB_SPEC, oversized_row,
                5, 1, too_wide, true);

    tex_sb_expect_linear(t, image, 0, 0.25f);
    qtest_quit(t);
}

/*
 * The client's wide-level branch, as distort's 512-wide bordered level
 * traced it: the level's bracketed load, a bare 0x9c00, then 64-by-1 rows
 * at x 512 and on under a TL_SPEC twice as wide, with no 0x9b00. What the
 * board does with them is unknown, so the model refuses them and stores
 * nothing: the ring the bracketed load completed is kept, and a linear
 * sample at the left edge still blends it rather than the border colour.
 */
#define TEX_WIDE_SPEC_512X1   0x090
#define TEX_WIDE_SPEC_1024X1  0x0a0
#define TEX_WIDE_W            514
#define TEX_WIDE_H            3

static uint32_t tex_wide_edge_sample(QTestState *t)
{
    static const float white[4] = { 1, 1, 1, 1 };
    static const float quad[4][4] = {
        { 16, 16, 0, 0.5f }, { 32, 16, 0, 0.5f },
        { 32, 32, 0, 0.5f }, { 16, 32, 0, 0.5f },
    };

    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | TEX_SB_CLAMP_BITS |
             TEX_MAG_LINEAR | TEX_MIN_LINEAR);
    tex_records(t, (const uint32_t[]) {
        TEX_TXSIZE, 0x0909,
        TEX_TXCLAMPSIZE, 512 << 13 | 1,
    }, 2);
    tex_mipmap_word(t, 4);
    tex_quad_rect(t, white, quad);
    return gl_window_pixel(t, 24, 24);
}

static void test_gl_texture_wide_level_branch_refused(void)
{
    static const uint32_t level[6] = {
        -1, -1, TEX_WIDE_W, TEX_WIDE_H, TEX_WIDE_H << 16 | TEX_WIDE_W, 0,
    };
    static const uint32_t row_box[6] = { 512, -1, 64, 1, 1 << 16 | 64, 0 };
    QTestState *t = tex_start(TEX_ARGS);
    g_autofree uint8_t *image = g_malloc(TEX_WIDE_W * TEX_WIDE_H * 4);
    uint8_t row[64 * 4];

    /* Ring (200, 40, 40), interior (40, 200, 40). */
    for (int y = 0; y < TEX_WIDE_H; y++) {
        for (int x = 0; x < TEX_WIDE_W; x++) {
            uint8_t *p = &image[4 * (y * TEX_WIDE_W + x)];
            bool ring = y != 1 || x == 0 || x == TEX_WIDE_W - 1;

            p[0] = ring ? 200 : 40;
            p[1] = ring ? 40 : 200;
            p[2] = 40;
            p[3] = 0xff;
        }
    }
    memset(row, 0xa5, sizeof(row));

    tex_sb_set_base_block(t, 0, 4);
    tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_WIDE_SPEC_512X1, image,
                TEX_WIDE_W, TEX_WIDE_H, level, true);
    tex_expect(tex_wide_edge_sample(t), 120 / 255.0, 120 / 255.0,
               40 / 255.0);

    gl_word(t, TEX_LOAD_CLOSE);
    for (int k = 0; k < 2; k++) {
        tex_load_as(t, false, 0, TEX_WIDE_SPEC_1024X1, row, 64, 1, row_box,
                    false);
    }

    tex_expect(tex_wide_edge_sample(t), 120 / 255.0, 120 / 255.0,
               40 / 255.0);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * Unbracketed transfers that are not that traced shape are still taken
 * as loads: the first one after reset, of a 2-by-1 level whose TL_SPEC
 * happens to be twice the width of the reset TL_SPEC, and one under a
 * doubled TL_SPEC after a bare 0x9c00 that follows a 2-wide level's
 * bracketed load, not a 512-wide one.
 */
static void test_gl_texture_unbracketed_loads_taken(void)
{
    static const uint32_t box_2x1[6] = { 0, 0, 2, 1, 1 << 16 | 2, 0 };
    static const uint32_t box_2x2[6] = { 0, 0, 2, 2, 2 << 16 | 2, 0 };
    static const uint32_t box_4x2[6] = { 0, 0, 4, 2, 2 << 16 | 4, 0 };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32], other[32];

    tex_texels(texels);
    tex_load_as(t, true, TEX_TL_MODE_RGBA8, 0x010, texels, 2, 1, box_2x1,
                false);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_records(t, (const uint32_t[]) {
        TEX_TXSIZE, 0x0101,
        TEX_TXCLAMPSIZE, 2 << 13 | 1,
    }, 2);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==,
                    texels[0] << 16 | texels[1] << 8 | texels[2]);
    g_assert_cmphex(tex_texel_pixel(t, 2, 0), ==,
                    texels[4] << 16 | texels[5] << 8 | texels[6]);
    qtest_quit(t);

    t = tex_start(TEX_ARGS);
    for (int i = 0; i < 32; i++) {
        other[i] = i % 4 == 3 ? 0xff : texels[i] ^ 0x5a;
    }
    tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_2X2, other, 2, 2,
                box_2x2, true);
    gl_word(t, TEX_LOAD_CLOSE);
    tex_load_as(t, false, 0, TEX_SPEC_4X2, texels, 4, 2, box_4x2, false);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
    tex_expect_texels(t, texels);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

static void tex_sb_file_migration_check_images(QTestState *t,
                                               const uint8_t a[64],
                                               const uint8_t b[64],
                                               const uint8_t c[64])
{
    static const float centers[4] = { -0.25f, 0.25f, 0.75f, 1.0f };

    gl_enter_slot(t, GL_SLOT_A);
    gl_one(t, TEX_SB_BASE_HDR, 0);
    tex_sb_expect_all_linear(t, a);
    for (unsigned y = 0; y < ARRAY_SIZE(centers); y++) {
        for (unsigned x = 0; x < ARRAY_SIZE(centers); x++) {
            tex_sb_expect_nearest(t, a, centers[x], centers[y]);
        }
    }
    gl_one(t, TEX_SB_BASE_HDR, 1);
    tex_sb_expect_all_linear(t, b);
    for (unsigned y = 0; y < ARRAY_SIZE(centers); y++) {
        for (unsigned x = 0; x < ARRAY_SIZE(centers); x++) {
            tex_sb_expect_nearest(t, b, centers[x], centers[y]);
        }
    }
    gl_one(t, TEX_SB_BASE_HDR, 0);
    tex_sb_expect_all_linear(t, a);

    gl_enter_slot(t, GL_SLOT_B);
    gl_one(t, TEX_SB_BASE_HDR, 2);
    tex_sb_expect_all_linear(t, c);
    for (unsigned y = 0; y < ARRAY_SIZE(centers); y++) {
        for (unsigned x = 0; x < ARRAY_SIZE(centers); x++) {
            tex_sb_expect_nearest(t, c, centers[x], centers[y]);
        }
    }
}

static void tex_sb_file_migration_check_empty(QTestState *t,
                                               const uint8_t image[64])
{
    static const float centers[4] = { -0.25f, 0.25f, 0.75f, 1.0f };

    gl_enter_slot(t, GL_SLOT_A);
    gl_one(t, TEX_SB_BASE_HDR, 0);
    tex_sb_expect_all_linear(t, image);
    for (unsigned y = 0; y < ARRAY_SIZE(centers); y++) {
        for (unsigned x = 0; x < ARRAY_SIZE(centers); x++) {
            tex_sb_expect_nearest(t, image, centers[x], centers[y]);
        }
    }
}

static QTestState *tex_sb_file_migrate_and_restart(QTestState *from,
                                                    const char *args,
                                                    const char *uri)
{
    QTestState *to;

    sgi_migrate_stopped(from, uri);
    qtest_quit(from);
    to = qtest_initf("%s -incoming %s", args, uri);
    sgi_wait_migration_status(to, "completed", 30);
    return to;
}

static void tex_precision_border_upload(QTestState *t)
{
    uint8_t pixels[4 * 3 * 8] = { 0 };
    const uint32_t box[6] = { -1, -1, 4, 3, 3 << 16 | 4, 0 };

    for (unsigned i = 0; i < 12; i++) {
        stw_be_p(&pixels[8 * i + 2], 0xffff);
        stw_be_p(&pixels[8 * i + 6], 0xffff);
    }
    stw_be_p(&pixels[8 * 4], 0x0090);
    tex_enter(t, GL_SLOT_A);
    tex_sb_set_base_block(t, 0, 4);
    tex_load_fmt(t, true, 0x34000 | TEX_LAYOUT_RGBA << 1 | 2 << 5,
                 0x10, pixels, 4, 3, 4, 8, box, true, 0, 0);
}

static uint32_t tex_precision_border_draw(QTestState *t)
{
    static const float white[4] = { 1, 1, 1, 1 };
    static const float quad[4][4] = {
        { 16, 16, -0.125f, 0.5f }, { 32, 16, -0.125f, 0.5f },
        { 32, 32, -0.125f, 0.5f }, { 16, 32, -0.125f, 0.5f },
    };

    gl_enter_slot(t, GL_SLOT_A);
    tex_bind(t, tex_mode1(TEX_ENV_MODULATE, TEX_LAYOUT_RGBA,
                          TEX_CLASS_COLOUR, 2),
             0x10000 | 2 << 2 | TEX_MAG_LINEAR | TEX_MIN_LINEAR |
             TEX_SB_CLAMP_BITS);
    tex_records(t, (const uint32_t[]) {
        TEX_TXSIZE, 0x101,
        TEX_TXCLAMPSIZE, 2 << 13 | 1,
    }, 2);
    tex_mipmap_word(t, 4);
    tex_quad_rect(t, white, quad);
    return gl_window_pixel(t, 24, 24);
}

static void test_gl_texture_stored_border_preserves_12bit_after_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-12bit-border-migration-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(tmpfs, "state", NULL);
    uri = g_strdup_printf("file:%s", path);
    from = tex_start(TEX_ARGS);
    tex_precision_border_upload(from);
    g_assert_cmphex(tex_precision_border_draw(from), ==, 0x00ff00);

    to = tex_sb_file_migrate_and_restart(from, TEX_ARGS, uri);
    g_assert_cmphex(tex_precision_border_draw(to), ==, 0x00ff00);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);
    qtest_quit(to);
    unlink(path);
    g_rmdir(tmpfs);
}

static void test_gl_texture_stored_border_file_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint8_t a[64], acore[16], b[64], bcore[16], c[64], ccore[16];

    tmpfs = g_dir_make_tmp("sn0-mgras-stored-border-migration-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(tmpfs, "state", NULL);
    uri = g_strdup_printf("file:%s", path);
    from = tex_start(TEX_ARGS);
    tex_sb_upload_complete(from, 0, 4, 0, a, acore);
    tex_sb_upload_complete(from, 1, 4, 0x40, b, bcore);
    gl_enter_slot(from, GL_SLOT_B);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_viewport(from, 0, 0, 128, 128);
    gl_window_projection(from);
    tex_sb_upload_complete(from, 2, 4, 0x80, c, ccore);
    tex_sb_file_migration_check_images(from, a, b, c);

    to = tex_sb_file_migrate_and_restart(from, TEX_ARGS, uri);
    tex_sb_file_migration_check_images(to, a, b, c);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);
    qtest_quit(to);
    unlink(path);
    g_rmdir(tmpfs);
}

static void test_gl_texture_stored_border_vmstate_version(void)
{
    static const char id[] = "sgi-mgras";
    g_autofree char *tmpfs = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *stream = NULL;
    gsize length;
    GError *err = NULL;
    QTestState *from, *to;
    uint8_t image[64], core[16];
    unsigned found = 0;

    tmpfs = g_dir_make_tmp("sn0-mgras-border-version-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(tmpfs, "state", NULL);
    uri = g_strdup_printf("file:%s", path);
    from = tex_start(TEX_ARGS);
    tex_sb_upload_complete(from, 0, 4, 0, image, core);
    tex_sb_expect_nearest(from, image, -0.25f, 0.25f);
    sgi_migrate_stopped(from, uri);
    qtest_quit(from);

    g_assert_true(g_file_get_contents(path, &stream, &length, &err));
    g_assert_no_error(err);
    for (gsize i = 0; i + sizeof(id) + 8 <= length; i++) {
        if ((uint8_t)stream[i] != sizeof(id) - 1 ||
            memcmp(stream + i + 1, id, sizeof(id) - 1)) {
            continue;
        }
        g_assert_cmphex(ldl_be_p(stream + i + sizeof(id) + 4), ==, 36);
        found++;
    }
    g_assert_cmpuint(found, ==, 1);

    to = qtest_initf("%s -incoming %s", TEX_ARGS, uri);
    sgi_wait_migration_status(to, "completed", 30);
    tex_sb_expect_nearest(to, image, -0.25f, 0.25f);
    qtest_quit(to);
    unlink(path);
    g_rmdir(tmpfs);
}

static void tex_sb_fill_uncached_image(uint8_t image[64],
                                       const uint8_t core[16])
{
    memset(image, 0, 64);
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            memcpy(&image[4 * ((y + 1) * 4 + x + 1)],
                   &core[4 * (y * 2 + x)], 4);
        }
    }
}

static void test_gl_texture_stored_border_empty_file_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *path = NULL;
    g_autofree char *uri = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint8_t image[64], core[16];

    tmpfs = g_dir_make_tmp("sn0-mgras-stored-border-empty-XXXXXX", &err);
    g_assert_no_error(err);
    path = g_build_filename(tmpfs, "state", NULL);
    uri = g_strdup_printf("file:%s", path);
    tex_sb_fill(image, core, 0);
    tex_sb_fill_uncached_image(image, core);
    from = tex_start(TEX_ARGS);
    tex_sb_set_base_block(from, 0, 4);
    tex_sb_upload_core(from, core);
    tex_sb_file_migration_check_empty(from, image);

    to = tex_sb_file_migrate_and_restart(from, TEX_ARGS, uri);
    tex_sb_file_migration_check_empty(to, image);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);
    qtest_quit(to);
    unlink(path);
    g_rmdir(tmpfs);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);


    qtest_add_func("sn0/origin2000/mgras/"
                   "hq-query-answers-the-texture-matrix",
                   test_hq_query_answers_the_texture_matrix);
    qtest_add_func("sn0/origin2000/mgras/gl-integer-and-texture-forms",
                   test_gl_integer_and_texture_forms);
    qtest_add_data_func("sn0/origin2000/mgras/gl-texture-load-draws-the-level",
                        "si-t", test_gl_texture_load_draws_the_level);
    qtest_add_data_func("sn0/origin2000/mgras/gl-texture-color-table/si-t",
                        "si-t", test_gl_texture_color_table);
    qtest_add_data_func("sn0/origin2000/mgras/gl-texture-color-table/esi-t",
                        "esi-t", test_gl_texture_color_table);
    qtest_add_data_func("sn0/origin2000/mgras/gl-texture-color-table/mxi",
                        "mxi", test_gl_texture_color_table);
    qtest_add_data_func("sn0/origin2000/mgras/gl-texture-color-table/emxi",
                        "emxi", test_gl_texture_color_table);
    add_texture_board_cases(
        "sn0/origin2000/mgras/gl-texture-color-table/migration",
        test_gl_texture_color_table_migration);
    add_texture_board_cases(
        "sn0/origin2000/mgras/gl-texture-color-table/full-migration",
        test_gl_texture_color_table_full_migration);
    add_texture_board_cases(
        "sn0/origin2000/mgras/gl-texture-color-table/reset-partial",
        test_gl_texture_color_table_reset_partial);
    qtest_add_data_func(
        "sn0/origin2000/mgras/gl-texture-load-draws-the-level/esi-t",
        "esi-t", test_gl_texture_load_draws_the_level);
    qtest_add_data_func(
        "sn0/origin2000/mgras/gl-texture-pixel-rgba-nearest/si-t",
        "si-t", test_gl_texture_pixel_rgba_nearest);
    add_texture_board_cases(
        "sn0/origin2000/mgras/gl-texture-pixel-texture-passes",
        test_gl_texture_pixel_texture_passes);
    add_texture_board_cases(
        "sn0/origin2000/mgras/gl-texture-pixel-texture-volume-slice",
        test_gl_texture_pixel_texture_volume_slice);
    add_texture_board_cases(
        "sn0/origin2000/mgras/gl-texture-pixel-texture-zoom",
        test_gl_texture_pixel_texture_zoom);
    qtest_add_data_func(
        "sn0/origin2000/mgras/gl-texture-pixel-texture-pass-migration/mxi",
        "mxi", test_gl_texture_pixel_texture_pass_migration);
    qtest_add_data_func("sn0/origin2000/mgras/gl-pixels-zoom-migration/si-t",
                        "si-t", test_gl_pixels_zoom_migration);
    add_texture_board_cases("sn0/origin2000/mgras/gl-bands-match-one-thread",
                            test_gl_bands_match_one_thread);
    add_texture_board_cases("sn0/origin2000/mgras/gl-texture-volume-pixel-rgba",
                            test_gl_texture_volume_pixel_rgba);
    add_texture_board_cases(
        "sn0/origin2000/mgras/gl-texture-rectangular-volume-pixel-rgba",
        test_gl_texture_rectangular_volume_pixel_rgba);
    add_texture_board_cases(
        "sn0/origin2000/mgras/gl-texture-captured-boundary-volumes",
        test_gl_texture_captured_boundary_volumes);
    add_texture_board_cases(
        "sn0/origin2000/mgras/gl-texture-captured-boundary-volumes-pixel",
        test_gl_texture_captured_boundary_volumes_pixel);
    add_texture_board_cases(
        "sn0/origin2000/mgras/gl-texture-captured-deep-volume",
        test_gl_texture_captured_deep_volume);
    add_texture_board_cases(
        "sn0/origin2000/mgras/gl-texture-captured-deep-volume-pixel",
        test_gl_texture_captured_deep_volume_pixel);
    add_texture_board_cases(
        "sn0/origin2000/mgras/gl-texture-ordinary-volume-point",
        test_gl_texture_ordinary_volume_point);
    add_texture_board_cases("sn0/origin2000/mgras/gl-texture-volume-slices",
                            test_gl_texture_volume_slices);
    add_texture_board_cases("sn0/origin2000/mgras/gl-texture-generation",
                            test_gl_texture_generation);
    qtest_add_func(
        "sn0/origin2000/mgras/gl-texture-ordinary-rectangular-volume",
        test_gl_texture_ordinary_rectangular_volume);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-captured-large-volumes",
                   test_gl_texture_captured_large_volumes);
    add_texture_board_cases(
        "sn0/origin2000/mgras/gl-texture-ordinary-captured-large-volumes",
        test_gl_texture_ordinary_captured_large_volumes);
    qtest_add_func(
        "sn0/origin2000/mgras/gl-texture-captured-volume-unknown-selector",
        test_gl_texture_captured_volume_rejects_unknown_selector);
    qtest_add_func(
        "sn0/origin2000/mgras/gl-texture-volume-depth4-migration",
        test_gl_texture_volume_depth4_migration);
    qtest_add_func(
        "sn0/origin2000/mgras/gl-texture-rectangular-volume-migration",
        test_gl_texture_rectangular_volume_migration);
    qtest_add_func(
        "sn0/origin2000/mgras/gl-texture-captured-volume-64k-migration",
        test_gl_texture_captured_volume_64k_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-volume-depth4-reset-load",
                   test_gl_texture_volume_depth4_reset_load);
    qtest_add_data_func("sn0/origin2000/mgras/gl-texture-rgba4-rounds",
                        "si-t", test_gl_texture_rgba4_rounds);
    qtest_add_data_func(
        "sn0/origin2000/mgras/gl-texture-rgba4-rounds/esi-t",
        "esi-t", test_gl_texture_rgba4_rounds);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-size-from-tl-spec",
                   test_gl_texture_size_from_tl_spec);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-tl-mode-persists",
                   test_gl_texture_tl_mode_persists);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-unsupported-draws-untextured",
                   test_gl_texture_unsupported_draws_untextured);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-levels-by-block",
                   test_gl_texture_levels_by_block);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-many-levels-in-one-context",
                   test_gl_texture_many_levels_in_one_context);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-load-abgr-bytes",
                   test_gl_texture_load_abgr_bytes);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-load-unsigned-shorts",
                   test_gl_texture_load_unsigned_shorts);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-load-abgr-shorts",
                   test_gl_texture_load_abgr_shorts);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-shorts-reduce-rounded",
                   test_gl_texture_shorts_reduce_rounded);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-load-rgb-shorts",
                   test_gl_texture_load_rgb_shorts);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-level-from-strips",
                   test_gl_texture_level_from_strips);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-rgb-image-into-rgba",
                   test_gl_texture_rgb_image_into_rgba);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-textureless-board-ignores",
                   test_gl_texture_textureless_board_ignores);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-load-from-another-node",
                   test_gl_texture_load_from_another_node);
    qtest_add_func("sn0/origin2000/mgras/tram-load-is-one-store",
                   test_tram_load_is_one_store);
    qtest_add_func("sn0/origin2000/mgras/tram-save-returns-the-block",
                   test_tram_save_returns_the_block);
    qtest_add_func("sn0/origin2000/mgras/tram-client-restore-round-trip",
                   test_tram_client_restore_round_trip);
    qtest_add_func("sn0/origin2000/mgras/tram-client-restore-under-select",
                   test_tram_client_restore_under_select);
    qtest_add_func("sn0/origin2000/mgras/tram-ge-dma-is-not-a-raster-image",
                   test_tram_ge_dma_is_not_a_raster_image);
    qtest_add_func("sn0/origin2000/mgras/tram-open-save-ends-at-other-kick",
                   test_tram_open_save_ends_at_other_kick);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-txborder-word-is-not-a-border",
                   test_gl_texture_txborder_word_is_not_a_border);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-bordered-image-stores-interior",
                   test_gl_texture_bordered_image_stores_interior);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-bordered-strips",
                   test_gl_texture_bordered_strips);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-table-defaults-are-silent",
                   test_gl_texture_table_defaults_are_silent);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-class-intensity-vs-luminance",
                   test_gl_texture_class_intensity_vs_luminance);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-tram-select-keys-small-levels",
                   test_gl_texture_tram_select_keys_small_levels);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-tram-select-ignored-for-rgba",
                   test_gl_texture_tram_select_ignored_for_rgba);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-class-alpha",
                   test_gl_texture_class_alpha);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-env-blend-constant",
                   test_gl_texture_env_blend_constant);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-replace-is-modulate",
                   test_gl_texture_replace_is_modulate);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-env-decal-rgba",
                   test_gl_texture_env_decal_rgba);
    qtest_add_data_func(
        "sn0/origin2000/mgras/gl-texture-stored-border-perimeter-samples/si-t",
        "si-t", test_gl_texture_stored_border_perimeter_samples);
    qtest_add_data_func(
        "sn0/origin2000/mgras/gl-texture-stored-border-perimeter-samples/esi-t",
        "esi-t", test_gl_texture_stored_border_perimeter_samples);
    qtest_add_data_func(
        "sn0/origin2000/mgras/gl-texture-stored-border-perimeter-samples/mxi",
        "mxi", test_gl_texture_stored_border_perimeter_samples);
    qtest_add_data_func(
        "sn0/origin2000/mgras/gl-texture-stored-border-perimeter-samples/emxi",
        "emxi", test_gl_texture_stored_border_perimeter_samples);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-stored-border-partial-and-updates",
                   test_gl_texture_stored_border_partial_and_updates);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-stored-border-incomplete-reset",
                   test_gl_texture_stored_border_incomplete_absent_and_reset);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-stored-border-base-context-isolation",
                   test_gl_texture_stored_border_base_context_isolation);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-stored-border-overlap-invalidates",
                   test_gl_texture_stored_border_overlapping_base_invalidates);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-stored-border-rejected-upload",
                   test_gl_texture_stored_border_bad_upload);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-wide-level-branch-refused",
                   test_gl_texture_wide_level_branch_refused);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-unbracketed-loads-taken",
                   test_gl_texture_unbracketed_loads_taken);
    qtest_add_func(
        "sn0/origin2000/mgras/"
        "gl-texture-stored-border-preserves-12bit-after-migration",
        test_gl_texture_stored_border_preserves_12bit_after_migration);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-stored-border-file-migration",
                   test_gl_texture_stored_border_file_migration);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-stored-border-vmstate-version",
                   test_gl_texture_stored_border_vmstate_version);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-stored-border-empty-file-migration",
                   test_gl_texture_stored_border_empty_file_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-clamp-border-nearest",
                   test_gl_texture_clamp_border_nearest);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-wrap-mixed-axes",
                   test_gl_texture_wrap_mixed_axes);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-border-per-class",
                   test_gl_texture_border_per_class);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-linear-midpoint",
                   test_gl_texture_linear_midpoint);
    qtest_add_func(
        "sn0/origin2000/mgras/gl-texture-linear-preserves-12bit-components",
        test_gl_texture_linear_preserves_12bit_components);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-linear-edge-wrap",
                   test_gl_texture_linear_edge_wrap);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-depth12-halfword",
                   test_gl_texture_depth12_halfword);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-mip-level-select",
                   test_gl_texture_mip_level_select);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-trilinear-blend",
                   test_gl_texture_trilinear_blend);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-raster-records-are-not-logged",
                   test_gl_texture_raster_records_are_not_logged);
    qtest_add_func("sn0/origin2000/mgras/gl-native-texcoord/current",
                    test_gl_native_texcoord_current);
    qtest_add_func("sn0/origin2000/mgras/gl-native-texcoord/raster",
                    test_gl_native_texcoord_raster);
    qtest_add_func("sn0/origin2000/mgras/gl-native-texcoord/contexts",
                    test_gl_native_texcoord_contexts);
    qtest_add_data_func("sn0/origin2000/mgras/mxi-texture-rows-match",
                        "mxi", test_mxi_texture_rows_match);
    qtest_add_data_func("sn0/origin2000/mgras/mxi-texture-rows-match/emxi",
                        "emxi", test_mxi_texture_rows_match);
    qtest_add_data_func("sn0/origin2000/mgras/mxi-texbase-both-rows",
                        "mxi", test_mxi_texbase_both_rows);
    qtest_add_data_func("sn0/origin2000/mgras/mxi-texbase-both-rows/emxi",
                        "emxi", test_mxi_texbase_both_rows);
    qtest_add_data_func("sn0/origin2000/mgras/mxi-tram-save-restore",
                        "mxi", test_mxi_tram_save_restore);
    qtest_add_data_func("sn0/origin2000/mgras/mxi-tram-save-restore/emxi",
                        "emxi", test_mxi_tram_save_restore);
    qtest_add_data_func("sn0/origin2000/mgras/mxi-tram-save-restore/si-t",
                        "si-t", test_mxi_tram_save_restore);
    qtest_add_data_func("sn0/origin2000/mgras/mxi-tram-save-restore/esi-t",
                        "esi-t", test_mxi_tram_save_restore);
    qtest_add_func("sn0/origin2000/mgras/mxi-tram-save-shapes",
                   test_mxi_tram_save_shapes);

    ret = g_test_run();

    return ret;
}
