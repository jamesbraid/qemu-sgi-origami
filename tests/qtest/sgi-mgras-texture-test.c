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
 * A load whose six GE words at 0xe3d carry a border offset other than the
 * two traced unbordered ones is logged once and stores nothing, so the
 * draw is white and untextured; the traced values 0xffffffff and 0 draw
 * the level, as does a first block that cancels the offset.
 */
static void test_gl_texture_border_is_logged_untextured(void)
{
    static const struct {
        uint32_t block;
        uint32_t border;
        bool bordered;
    } cases[] = {
        { 0, 0xffffffff, false },
        { 0, 0, false },
        { 3, 2, false },
        { 0, 1, true },
        { 3, 7, true },
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *log = NULL;
    g_autofree char *args = NULL;
    g_autofree char *contents = NULL;
    GError *err = NULL;
    uint8_t texels[32];
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
        uint32_t want = texels[0] << 16 | texels[1] << 8 | texels[2];

        gl_word(t, GL_CONTEXT_OPENER);
        gl_viewport(t, 0, 0, 128, 128);
        gl_window_projection(t);
        gl_ge_load(t, GL_GE_ERAM, 0xe3d, words, 6);
        tex_load(t, TEX_TL_MODE_RGBA8, texels);
        tex_bind(t, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8);
        tex_quad(t);
        if (cases[i].bordered) {
            want = TEX_WHITE;
        }
        g_assert_cmphex(tex_texel_pixel(t, 0, 0), ==, want);
        logged += cases[i].bordered;
    }
    g_assert_cmphex(mgras_qom_get_uint(t, "x-test-error"), ==, 0);
    g_assert_true(g_file_get_contents(log, &contents, NULL, &err));
    g_assert_no_error(err);
    for (const char *p = contents;
         (p = strstr(p, "texture load with a border")); p++) {
        logged--;
    }
    g_assert_cmpint(logged, ==, 0);
    qtest_quit(t);
    unlink(log);
    g_rmdir(tmpfs);
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
 * level 1. A chain with a hole at level 1 draws untextured.
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

        tex_lambda_quad(quad, 2);
        tex_scene(t, &sc);
        g_assert_cmphex(gl_window_pixel(t, 24, 24), ==, 0xffffff);
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
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-border-is-logged-untextured",
                   test_gl_texture_border_is_logged_untextured);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-board-cap",
                   test_gl_texture_board_cap);
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

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
