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
 * named. Levels belong to their context: another context has none, and
 * the first still draws its own after switching back. A 0x9c00 without
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
 * TEXMODE1 bit 0 clear, a linear magnification filter (TEXMODE2 bit 7),
 * a clamp (bit 11), the decal environment (TEXMODE1 bits 2:1 = 1), a
 * level kept at 12 bits, which is not stored, and 0x0c00 after the
 * bind. Each quad is white.
 */
static void test_gl_texture_unsupported_draws_untextured(void)
{
    static const uint32_t cases[][3] = {
        { TEX_TL_MODE_RGBA8, TEX_TEXMODE1_ON & ~1u, TEX_TEXMODE2_RGBA8 },
        { TEX_TL_MODE_RGBA8, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | 0x80 },
        { TEX_TL_MODE_RGBA8, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | 0x800 },
        { TEX_TL_MODE_RGBA8, TEX_TEXMODE1_ON | 2, TEX_TEXMODE2_RGBA8 },
        { 0x34046, TEX_TEXMODE1_ON, 0x10008 },
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

/* A 1024-by-1024 RGBA8 level is exactly one TRAM's 4 MiB. */
#define TEX_SPEC_1024_LEVEL1  0xaa1

/* Context slot, with a viewport and projection, ready to load and draw. */
static void tex_enter(QTestState *t, uint32_t slot)
{
    gl_enter_slot(t, slot);
    gl_word(t, GL_CONTEXT_OPENER);
    gl_viewport(t, 0, 0, 128, 128);
    gl_window_projection(t);
}

/* Level 1 of a 1024-by-1024 in a fresh context. */
static void tex_load_big(QTestState *t, uint32_t slot)
{
    static const uint32_t box[6] = { 0, 0, 1024, 1024, 1024 << 16 | 1024, 0 };
    uint8_t row[4096];

    memset(row, 0x5a, sizeof(row));
    tex_enter(t, slot);
    tex_load_full(t, true, TEX_TL_MODE_RGBA8, TEX_SPEC_1024_LEVEL1, row, 1024,
                  1024, box, true, 0, sizeof(row));
}

/* Load a 4-by-2 level 0 in the current context and draw with it. */
static void tex_small_level_draw(QTestState *t)
{
    uint8_t texels[32];

    tex_texels(texels);
    tex_load(t, TEX_TL_MODE_RGBA8, texels);
    tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
    tex_quad(t);
}

/*
 * A context keeps at most one TRAM's 4 MiB: with a level of exactly that
 * size loaded, a further level has no room, is not stored (logged), and
 * the draw finds no level 0 and is untextured, the quad's own white.
 * gl-texture-load-draws-the-level is the same load in an empty context.
 */
static void test_gl_texture_context_cap(void)
{
    QTestState *t = qtest_init(TEX_ARGS);

    tex_load_big(t, GL_SLOT_A);
    tex_small_level_draw(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, TEX_WHITE);
    g_assert_cmphex(tex_texel_pixel(t, 3, 1), ==, TEX_WHITE);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
}

/*
 * The board keeps four TRAMs' worth, 16 MiB, across its contexts: four
 * contexts holding 4 MiB each leave no room in a fifth.
 */
static void test_gl_texture_board_cap(void)
{
    QTestState *t = qtest_init(TEX_ARGS);

    for (uint32_t i = 0; i < 4; i++) {
        tex_load_big(t, 0x1000 + i);
    }
    tex_enter(t, 0x1004);
    tex_small_level_draw(t);
    g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, TEX_WHITE);
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    qtest_quit(t);
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
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-textureless-board-ignores",
                   test_gl_texture_textureless_board_ignores);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-load-from-another-node",
                   test_gl_texture_load_from_another_node);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-context-cap",
                   test_gl_texture_context_cap);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-board-cap",
                   test_gl_texture_board_cap);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-table-defaults-are-silent",
                   test_gl_texture_table_defaults_are_silent);
    qtest_add_func("sn0/origin2000/mgras/gl-native-texcoord/current",
                    test_gl_native_texcoord_current);
    qtest_add_func("sn0/origin2000/mgras/gl-native-texcoord/raster",
                    test_gl_native_texcoord_raster);
    qtest_add_func("sn0/origin2000/mgras/gl-native-texcoord/contexts",
                    test_gl_native_texcoord_contexts);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
