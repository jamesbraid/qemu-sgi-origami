/*
 * SI Viz register-contract and protocol qtests: texture cases.
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

static void gl_vertex3i(QTestState *qts, int32_t x, int32_t y, int32_t z)
{
    gl_word(qts, GL_VERTEX3I_HDR);
    gl_word(qts, x);
    gl_word(qts, y);
    gl_word(qts, z);
}

static void test_gl_integer_and_texture_forms(void)
{
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
}

static void test_gl_native_texcoord_current(void)
{
    static const float initial[4] = { 0, 0, 0, 1 };
    static const float four[4] = { 0.25f, -0.5f, 2, 0.5f };
    static const float two[4] = { -0.25f, 0.75f, 0, 1 };
    static const float integer[4] = { -3, 5, 7, 1 };

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
}

/* T * S preserves homogeneous q: (0.25, 0.5, -1, 2) maps as below. */
static void test_gl_native_texcoord_raster(void)
{
    static const float current[4] = { 0.25f, 0.5f, -1, 2 };
    static const float transformed[4] = { 6.5f, -2, 7.5f, 2 };
    static const float changed[4] = { -1, -2, 0, 1 };

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
}

static void test_gl_native_texcoord_contexts(void)
{
    mgras_reset();
    gl_texcoord_context_setup(qts);
    gl_texcoord_context_check(qts);
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
#define TEX_ARGS MACHINE_ARGS " -global sgi-mgras.board=si-t " \
    "-global sgi-mgras.x-unimplemented-board=on"
#define TEX_WHITE             0xffffff

/*
 * An RGBA8 level loaded and drawn nearest with modulate by white: each
 * texel's centre pixel takes the texel's own bytes, the first row sent
 * at t = 0, the bottom, and the quad's edges keep to its pixels.
 */
static void test_gl_texture_load_draws_the_level(void)
{
    QTestState *t = tex_start(TEX_ARGS);
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

/*
 * The model keeps an RGBA4 level by truncating each 8-bit component to
 * its high four bits and draws it widened by replication, its own choice
 * (hw/misc/sgi-mgras-texture.c). The components chosen tell truncation
 * from (v + 8) >> 4 (0x18, 0x7f, 0xf8) and from round(v * 15 / 255)
 * (0x0f, 0xc0). The traced texels, 0x10 + i, 0x40 + i and 0x80 + i,
 * come out 0x11, 0x44, 0x88 under all three.
 */
static void test_gl_texture_rgba4_truncates(void)
{
    static const uint8_t comp[] = {
        0x0f, 0x18, 0x7f, 0xc0, 0xf8, 0x00, 0xff, 0x2a,
    };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32];

    for (int i = 0; i < 8; i++) {
        texels[4 * i] = comp[i];
        texels[4 * i + 1] = comp[(i + 3) % 8];
        texels[4 * i + 2] = comp[(i + 5) % 8];
        texels[4 * i + 3] = 0xc0 + i;
    }
    tex_load(t, TEX_TL_MODE_RGBA4, texels);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA4);
    tex_quad(t);
    for (int i = 0; i < 8; i++) {
        uint32_t want = 0;

        for (int k = 0; k < 3; k++) {
            want = want << 8 | (texels[4 * i + k] >> 4) * 0x11;
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

/*
 * The fifth of the load's GE words at 0xe3d is the level's TXBORDER word,
 * not a sign of a border: the traced one-level value, the first block
 * less one, and the words perfly's unbordered mipmapped levels carried
 * (TXMIPMAP 0xc with TXBORDER 0x10, 0x111 with 0x13, 0x14 with 0x15)
 * all draw the level, each through the TXMIPMAP word its load named. A
 * bordered image, two texels wider and taller than its level, does not
 * lie inside the level: it is logged and stores nothing, so its draw is
 * white and untextured.
 */
static void test_gl_texture_border_is_logged_untextured(void)
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
    static const uint32_t bordered[6] = { 0, 0, 6, 4, 4 << 16 | 6, 0 };
    static const uint32_t border_words[6] = { 4, 0, 0, 9, 8, 0 };
    g_autofree char *tmpfs = NULL;
    g_autofree char *log = NULL;
    g_autofree char *args = NULL;
    g_autofree char *contents = NULL;
    GError *err = NULL;
    uint8_t texels[32], image[96];
    QTestState *t;
    int logged = 0;

    tmpfs = g_dir_make_tmp("sn0-mgras-tex-border-XXXXXX", &err);
    g_assert_no_error(err);
    log = g_strdup_printf("%s/unimp.log", tmpfs);
    args = g_strdup_printf(TEX_ARGS " -d unimp -D %s", log);
    t = tex_start(args);
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

    memset(image, 0x5a, sizeof(image));
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
    /*
     * The load names TXMIPMAP word 9, which no load above reached, and the
     * draw goes through the same word, so the colour drawn shows whether
     * the refused load stored a level there. The 0x5a image would not
     * draw white.
     */
    gl_ge_load(t, GL_GE_ERAM, 0xe3d, border_words, 6);
    tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_4X2, image, 6, 4,
                bordered, true);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_mipmap_word(t, 9);
    tex_quad(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, TEX_WHITE);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    g_assert_true(g_file_get_contents(log, &contents, NULL, &err));
    g_assert_no_error(err);
    for (const char *p = contents;
         (p = strstr(p, "is not inside TL_SPEC")); p++) {
        logged++;
    }
    g_assert_cmpint(logged, ==, 1);
    qtest_quit(t);
    unlink(log);
    g_rmdir(tmpfs);
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
 * A level loaded as strips inside one bracket, as perfly's client loads
 * its 256-wide levels 64 texels at a time: each transfer carries its
 * strip's x and y at GE parameters 0xe34 and 0xe35 and its size in the
 * size word, and lands there in the level. The two 2-by-2 strips of a
 * 4-by-2 level, sent right one first, make the whole level; a strip that
 * would run past the level's edge stores nothing and leaves it whole.
 */
static void test_gl_texture_level_from_strips(void)
{
    static const uint32_t past[6] = { 3, 0, 2, 2, 2 << 16 | 2, 0 };
    QTestState *t = tex_start(TEX_ARGS);
    uint8_t texels[32], strip[2][16], other[16];

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
    tex_load_as(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_4X2, other, 2, 2, past,
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
                     rgb, 4, 2, 3, box, true, 0, 0);
    gl_clear_color(t, 0, 0, 0, 1);
    gl_word(t, GL_CLEAR_COLOR_BUFFER);
    gl_blend(t, GL_SRC_ALPHA, GL_SRC_ALPHA + 1, GL_FUNC_ADD);
    gl_one(t, GL_BLEND_HDR, 1);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA4);
    tex_quad(t);
    for (int i = 0; i < 8; i++) {
        uint32_t want = 0;

        for (int k = 0; k < 3; k++) {
            want = want << 8 | (rgb[3 * i + k] >> 4) * 0x11;
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
    "-device sgi-mgras,slot=io3 -global sgi-mgras.board=si-t " \
    "-global sgi-mgras.x-unimplemented-board=on"

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
    mgras_reset();

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
                 sc->texels, w, h, tex_bytes_a_texel(sc->layout, sc->depth),
                 box, true, 0, 0);
    for (uint32_t n = 1; n <= 3; n++) {
        uint32_t lw = MAX(1, w >> n), lh = MAX(1, h >> n);
        uint32_t lbox[6] = { 0, 0, lw, lh, lh << 16 | lw, 0 };

        if (sc->mip[n - 1]) {
            tex_load_fmt(t, true, 0x34000 | sc->layout << 1 | sc->depth << 5,
                         n | tex_log2(lh) << 8 | tex_log2(lw) << 4,
                         sc->mip[n - 1], lw, lh,
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
 * level 1. A chain with no level 1 loaded reads TRAM where level 1 would
 * lie, in the block level 0 was loaded to: on a fresh board those bits
 * are 0, so the lambda 1 draw is black, as the board would read what its
 * TRAM held.
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
        g_assert_cmphex(gl_window_pixel(t, 24, 24), ==, 0);
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
#define MXI_BOARD_FLAG " -global sgi-mgras.x-unimplemented-board=on"
#define MXI_TEX_BASE_HDR      0x0000eb04
#define MXI_ROW_EVEN          16
#define MXI_ROW_ODD           17

static QTestState *mxi_start(const char *board)
{
    g_autofree char *args = g_strdup_printf(
        MACHINE_ARGS " -global sgi-mgras.board=%s" MXI_BOARD_FLAG, board);

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
 * screen row 0 that would take a 16 KiB row as 4,096 pixels.
 */
static void test_mxi_tram_save_restore(gconstpointer data)
{
    QTestState *t = mxi_start(data);
    g_autofree uint8_t *buffer = g_malloc(TRAM_BLOCK_BYTES);
    uint8_t texels[32], other[32];
    uint32_t before[4];

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
    tex_load_fmt(t, true, TEX_TL_MODE_RGB8, TEX_SPEC_4X2, first, 4, 2, 3,
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

    tex_load_fmt(t, false, 0, TEX_SPEC_4X2, second, 4, 2, 3, box, true, 0, 0);
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

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init(MACHINE_ARGS);


    qtest_add_func("sn0/origin2000/mgras/"
                   "hq-query-answers-the-texture-matrix",
                   test_hq_query_answers_the_texture_matrix);
    qtest_add_func("sn0/origin2000/mgras/gl-integer-and-texture-forms",
                   test_gl_integer_and_texture_forms);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-load-draws-the-level",
                   test_gl_texture_load_draws_the_level);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-rgba4-truncates",
                   test_gl_texture_rgba4_truncates);
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
    qtest_add_func("sn0/origin2000/mgras/tram-ge-dma-is-not-a-raster-image",
                   test_tram_ge_dma_is_not_a_raster_image);
    qtest_add_func("sn0/origin2000/mgras/tram-open-save-ends-at-other-kick",
                   test_tram_open_save_ends_at_other_kick);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-border-is-logged-untextured",
                   test_gl_texture_border_is_logged_untextured);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-table-defaults-are-silent",
                   test_gl_texture_table_defaults_are_silent);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-class-intensity-vs-luminance",
                   test_gl_texture_class_intensity_vs_luminance);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-class-alpha",
                   test_gl_texture_class_alpha);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-env-blend-constant",
                   test_gl_texture_env_blend_constant);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-replace-is-modulate",
                   test_gl_texture_replace_is_modulate);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-env-decal-rgba",
                   test_gl_texture_env_decal_rgba);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-clamp-border-nearest",
                   test_gl_texture_clamp_border_nearest);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-wrap-mixed-axes",
                   test_gl_texture_wrap_mixed_axes);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-border-per-class",
                   test_gl_texture_border_per_class);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-linear-midpoint",
                   test_gl_texture_linear_midpoint);
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

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
