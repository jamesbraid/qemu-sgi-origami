/*
 * MGRAS register-contract and protocol qtests: GL context and buffer
 * migration cases.
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


/* The slots migrate between two boards of the same build. */
static void test_gl_deep_slots_migrate(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-slots-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MIGRATION_ARGS MGRAS_TWO_RE_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS MGRAS_TWO_RE_ARGS
                                   " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    gl_deep_scene(from);
    gl_draw_buffer(from, 8);
    gl_clear_color(from, 1, 0, 1, 1);
    gl_word(from, GL_CLEAR_COLOR_BUFFER);
    sgi_migrate_stopped(from, uri);
    g_assert_cmphex(mgras_test_slot_pixel(to, 1, 300, 300), ==, 0xff00ff);
    g_assert_cmphex(mgras_test_slot_pixel(to, 0, 300, 300), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void test_gl_native_texcoord_migration(void)
{
    gl_migrate(gl_texcoord_context_setup, gl_texcoord_context_check);
}

/*
 * The GE's parameters and a draw stopped between rows cross a migration
 * and the draw finishes on the destination.
 */
static void test_gl_pixels_float_migration(void)
{
    static const uint32_t program[] = {
        0x1655, 0, 0x3e65, 0x1d6, 0x156a, 0x1b4, 0x352d, 0xdf3,
    };
    static const uint32_t unpack[] = {
        0x1c, 0x40f2, 0x464, 0, 2, 6, 0xfffffee4, 0x3f800000,
    };
    static const uint32_t values[] = {
        0x3e808081, 0x3e808081, 0, 0x3f3fbfc0, 0x3e808081, 0,
        0, 0x3e808081, 0x3e808081, 0, 0x3f3fbfc0, 0x3e808081,
    };
    static const uint32_t expected[2][2] = {
        { 0x404000, 0xbf4000 },
        { 0x004040, 0x00bf40 },
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-float-pixels-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);
    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_viewport(from, 0, 0, 128, 128);
    gl_window_projection(from);
    gl_raster_pos2f(from, 0, 0);
    gl_pixels_program(from, GL_PIX_3UB, 6, 0, 0, 1, 1, 1);
    gl_ge_load(from, GL_GE_CRAM, GL_GE_PROGRAM, program,
               ARRAY_SIZE(program));
    gl_ge_load(from, GL_GE_ERAM, 0x1d6, unpack, ARRAY_SIZE(unpack));
    gl_pixels_draw(from, 6);
    gl_word(from, GL_INLINE_BLOCK | 6 * sizeof(values[0]));
    for (int i = 0; i < 4; i++) {
        gl_word(from, values[i]);
    }

    sgi_migrate_stopped(from, uri);

    for (unsigned which = 0; which < 2; which++) {
        QTestState *s = which ? to : from;

        for (unsigned i = 4; i < 6; i++) {
            gl_word(s, values[i]);
        }
        gl_pixels_row(s, 0, values + 6, 6);
        gl_pixels_fence(s);
        g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);
    }
    for (unsigned y = 0; y < 3; y++) {
        for (unsigned x = 0; x < 3; x++) {
            uint32_t pixel = gl_window_pixel(from, x, y);

            g_assert_cmphex(pixel, ==, x < 2 && y < 2 ? expected[y][x] : 0);
            g_assert_cmphex(gl_window_pixel(to, x, y), ==, pixel);
        }
    }

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void test_gl_pixels_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-pixels-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_one(from, GL_INIT_CI_HDR, 8);
    gl_pixels_program(from, GL_PIX_INDEX, 6, 20, 30, 22, 31, 1);
    gl_pixels_draw(from, 2);
    gl_pixels_row(from, 0, (const uint32_t[]){ 0x00070008, 0x00070000 }, 2);
    gl_word(from, GL_INLINE_BLOCK | 8);
    gl_word(from, 0x0009000a);

    sgi_migrate_stopped(from, uri);

    gl_word(to, 0x00090000);
    gl_pixels_fence(to);
    g_assert_cmphex(gl_window_pixel(to, 20, 30), ==, 7);
    g_assert_cmphex(gl_window_pixel(to, 21, 30), ==, 8);
    g_assert_cmphex(gl_window_pixel(to, 20, 31), ==, 9);
    g_assert_cmphex(gl_window_pixel(to, 21, 31), ==, 0xa);
    g_assert_cmphex(gl_window_pixel(to, 22, 31), ==, 9);
    /* The parameters crossed: a second draw needs only 0x8d20. */
    gl_pixels_draw(to, 2);
    gl_pixels_row(to, 0, (const uint32_t[]){ 0x00010002, 0x00030000 }, 2);
    gl_pixels_row(to, 0, (const uint32_t[]){ 0x00040005, 0x00060000 }, 2);
    g_assert_cmphex(gl_window_pixel(to, 22, 31), ==, 6);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}
#define TEX_MIGRATION_ARGS MIGRATION_ARGS " -global sgi-mgras.board=si-t " \
    "-global sgi-mgras.x-unimplemented-board=on"

#define TEX_TEXMODE2_VOLUME 0x800000
#define TEX_TXSIZE_2X2      0x1111

/*
 * A stored level, the registers and a load waiting for its transfer
 * cross a migration: the destination draws the stored RGBA4 level into
 * a cleared window, then takes the transfer as the level's replacement
 * at the 8-bit depth the waiting load's TL_MODE named.
 */
static void tex_migration_load_captured_volume(QTestState *t,
                                               const uint8_t *image,
                                               uint32_t depth)
{
    uint32_t bytes = 2 * 2 * depth * 4;
    uint32_t box[6] = { 0, 0, 2, 2, 2 << 16 | 2, depth };
    uint32_t flags = depth == 1 ? 0x98 : 0x198;

    g_assert_cmpuint(depth, >=, 1);
    g_assert_cmpuint(depth, <=, 2);
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
    gl_pixels_dma_on(t, 0, image, bytes, 8, 2 * depth, 8, 0xa7);
    gl_word(t, TEX_LOAD_CLOSE);
    gl_word(t, TEX_RESTORE_RSS);
}

static void tex_migration_volume_quad_z(QTestState *t, float z)
{
    static const float v[4][4] = {
        { 16, 16, 0, 0 }, { 48, 16, 1, 0 },
        { 48, 32, 1, 1 }, { 16, 32, 0, 1 },
    };

    gl_color4f(t, 1, 1, 1, 1);
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

static void tex_migration_check_volume(QTestState *t,
                                       const uint32_t expected[2][4])
{
    for (int z = 0; z < 2; z++) {
        tex_bind(t, TEX_TEXMODE1_ON,
                 TEX_TEXMODE2_RGBA8 | TEX_TEXMODE2_VOLUME);
        tex_record(t, TEX_TXSIZE, TEX_TXSIZE_2X2 | (1u << 19));
        tex_migration_volume_quad_z(t, z ? 0.75f : 0.25f);
        for (int y = 0; y < 2; y++) {
            for (int x = 0; x < 2; x++) {
                g_assert_cmphex(gl_window_pixel(t, 24 + 16 * x,
                                                20 + 8 * y), ==,
                                expected[z][2 * y + x]);
            }
        }
    }
}

static void test_gl_volume_texture_migration(void)
{
    static const uint8_t slices[32] = {
        0xff, 0, 0, 0xff, 0, 0xff, 0, 0xff,
        0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0xff,
        0, 0xff, 0xff, 0xff, 0xff, 0, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0xff,
    };
    static const uint32_t expected[2][4] = {
        { 0xff0000, 0x00ff00, 0x0000ff, 0xffff00 },
        { 0x00ffff, 0xff00ff, 0xffffff, 0x000000 },
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-volume-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = tex_start(TEX_MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(TEX_MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    tex_migration_load_captured_volume(from, slices, 2);
    tex_migration_check_volume(from, expected);
    sgi_migrate_stopped(from, uri);

    gl_clear_color(to, 0, 0, 0, 0);
    gl_word(to, GL_CLEAR_COLOR_BUFFER);
    tex_migration_check_volume(to, expected);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void test_gl_texture_migration(void)
{
    static const uint32_t box[6] = { 0, 0, 4, 2, 2 << 16 | 4, 0 };
    static const uint32_t draw[8] = { 8, 0, 0, 1, 0, 1, 0x511a, 0x98 };
    static const uint8_t white_pixels[16] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint8_t texels[32];

    tmpfs = g_dir_make_tmp("sn0-mgras-texture-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = tex_start(TEX_MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(TEX_MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    tex_texels(texels);
    tex_load(from, TEX_TL_MODE_RGBA4, texels);
    tex_bind(from, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA4);
    tex_quad(from);
    tex_record(from, TEX_TL_MODE, TEX_TL_MODE_RGBA8);
    tex_record(from, TEX_TL_SPEC, TEX_SPEC_4X2);
    gl_word(from, TEX_LOAD_OPEN);
    gl_pixels_program(from, GL_PIX_4UB, 8, 0, 0, 3, 1, 1);
    gl_ge_load(from, GL_GE_ERAM, GL_GE_BOX, box, 6);
    gl_word(from, GL_PIXELS_HDR);
    for (int i = 0; i < 8; i++) {
        gl_word(from, draw[i]);
    }
    /* The destination's pixel draw must observe this context-local flag. */
    tex_record(from, 0x18a, 0x3c040045);

    sgi_migrate_stopped(from, uri);

    gl_clear_color(to, 0, 0, 0, 0);
    gl_word(to, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(tex_texel_pixel(to, 0, 0), ==, 0);
    gl_word(to, TEX_ON);
    tex_quad(to);
    g_assert_cmphex(tex_texel_pixel(to, 0, 0), ==, 0x00ee88);
    g_assert_cmphex(tex_texel_pixel(to, 3, 1), ==, 0x770088);

    texels[0] = 0x55;
    texels[1] = 0x66;
    texels[2] = 0x77;
    gl_pixels_dma(to, texels, sizeof(texels), 32, 1, 32, 0xa7);
    gl_word(to, TEX_LOAD_CLOSE);
    tex_record(to, TEX_TEXMODE2, TEX_TEXMODE2_RGBA8);
    gl_word(to, TEX_ON);
    tex_quad(to);
    g_assert_cmphex(tex_texel_pixel(to, 0, 0), ==, 0x556677);
    g_assert_cmphex(tex_texel_pixel(to, 3, 1), ==, 0x730087);

    /*
     * No 0x18a write on the destination: white pixels give s=t=1, which
     * repeat wrap maps to (0, 0), proving bit zero crossed with the level.
     */
    gl_word(to, TEX_ON);
    gl_pixels_program(to, GL_PIX_4UB, 2, 0, 0, 1, 1, 1);
    gl_raster_pos2f(to, 0, 0);
    gl_word(to, GL_PIXELS_HDR);
    gl_word(to, 2);
    gl_word(to, 0);
    gl_word(to, 0);
    gl_word(to, 2);
    gl_word(to, 0);
    gl_word(to, 1);
    gl_word(to, 0x4fc5);
    gl_word(to, 0x98);
    gl_pixels_dma_on(to, 0, white_pixels, sizeof(white_pixels), 8, 2, 8,
                     0xa7);
    tex_record(to, 0x18a, 0x3c040044);
    gl_pixels_fence(to);
    g_assert_cmphex(gl_window_pixel(to, 0, 0), ==, 0x556677);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/* A level that was never uploaded stays absent after a migration. */
static void test_gl_texture_missing_mip_level_migration(void)
{
    static const uint32_t box[6] = { 0, 0, 4, 4, 4 << 16 | 4, 0 };
    static const uint32_t pairs[4] = {
        TEX_TXSIZE, 0x2222, TEX_TXLOD, (64 << 16) | 1,
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    uint8_t level0[64];

    tmpfs = g_dir_make_tmp("sn0-mgras-missing-mip-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = tex_start(TEX_MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(TEX_MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    memset(level0, 0xff, sizeof(level0));
    tex_load_fmt(from, true, TEX_TL_MODE_RGBA8, 0x220, level0, 4, 4, 4, 4,
                 box, true, 0, 0);
    tex_bind(from, TEX_TEXMODE1_ON, TEX_TEXMODE2_RGBA8 | 0x80020);
    tex_records(from, pairs, ARRAY_SIZE(pairs) / 2);
    tex_quad(from);
    g_assert_cmphex(gl_window_pixel(from, 24, 24), ==, 0xffffff);

    sgi_migrate_stopped(from, uri);

    gl_word(to, TEX_ON);
    tex_quad(to);
    g_assert_cmphex(gl_window_pixel(to, 24, 24), ==, 0xffffff);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * Rewrite the waiting load's base and TXMIPMAP word in a migration stream
 * carrying one load of a 4-by-2 level of TL_MODE mode at base 0 and word
 * 0, the eleven fields from load on.
 */
static void tex_patch_load_place(const char *path, uint32_t mode,
                                 uint32_t base, uint32_t word)
{
    g_autofree char *data = NULL;
    gsize len;
    int found = 0;

    g_assert_true(g_file_get_contents(path, &data, &len, NULL));
    for (gsize i = 0; i + 44 <= len; i++) {
        const uint8_t *p = (const uint8_t *)data + i;

        /*
         * load, base, word, TL_SPEC, TL_MODE, x, y, size, components,
         * bytes a component, ABGR.
         */
        if (ldl_be_p(p) == 1 && ldl_be_p(p + 4) == 0 &&
            ldl_be_p(p + 8) == 0 && ldl_be_p(p + 12) == TEX_SPEC_4X2 &&
            ldl_be_p(p + 16) == mode && ldl_be_p(p + 20) == 0 &&
            ldl_be_p(p + 24) == 0 && ldl_be_p(p + 28) == (2 << 16 | 4) &&
            ldl_be_p(p + 32) == 4 && ldl_be_p(p + 36) == 1 &&
            ldl_be_p(p + 40) == 0) {
            stl_be_p(data + i + 4, base);
            stl_be_p(data + i + 8, word);
            found++;
        }
    }
    g_assert_cmpint(found, ==, 1);
    g_assert_true(g_file_set_contents(path, data, len, NULL));
}

/*
 * A load waiting for its transfer must have a place in TRAM: one whose base
 * and TXMIPMAP word put it past TRAM's 256 blocks is not a state the model
 * can reach (the load is refused when it starts), so a stream carrying it
 * is corrupt and is refused; the stream as saved loads.
 */
static void test_gl_texture_migration_rejects_load_past_tram(void)
{
    static const uint32_t box[6] = { 0, 0, 4, 2, 2 << 16 | 4, 0 };
    static const uint32_t draw[8] = { 8, 0, 0, 1, 0, 1, 0x511a, 0x98 };
    g_autofree char *tmpfs = NULL;
    g_autofree char *stream = NULL;
    g_autofree char *uri = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-texture-XXXXXX", &err);
    g_assert_no_error(err);
    stream = g_build_filename(tmpfs, "migration", NULL);
    uri = g_strdup_printf("file:%s", stream);

    from = tex_start(TEX_MIGRATION_ARGS);
    tex_record(from, TEX_TL_MODE, TEX_TL_MODE_RGBA8);
    tex_record(from, TEX_TL_SPEC, TEX_SPEC_4X2);
    gl_word(from, TEX_LOAD_OPEN);
    gl_pixels_program(from, GL_PIX_4UB, 8, 0, 0, 3, 1, 1);
    gl_ge_load(from, GL_GE_ERAM, GL_GE_BOX, box, 6);
    gl_word(from, GL_PIXELS_HDR);
    for (int i = 0; i < 8; i++) {
        gl_word(from, draw[i]);
    }
    sgi_migrate_stopped(from, uri);
    qtest_quit(from);

    to = qtest_init(TEX_MIGRATION_ARGS " -incoming defer");
    qtest_qmp_assert_success(
        to, "{ 'execute': 'migrate-incoming', "
            "'arguments': { 'uri': %s } }", uri);
    sgi_wait_migration_status(to, "completed", 30);
    qtest_quit(to);

    tex_patch_load_place(stream, TEX_TL_MODE_RGBA8, 0xff, 0xff);
    to = qtest_init(TEX_MIGRATION_ARGS " -incoming defer");
    qtest_set_expected_status(to, 1);
    qtest_qmp_assert_success(
        to, "{ 'execute': 'migrate-incoming', "
            "'arguments': { 'uri': %s } }", uri);
    qtest_wait_qemu(to);
    qtest_quit(to);

    g_assert_cmpint(g_unlink(stream), ==, 0);
    g_assert_cmpint(g_rmdir(tmpfs), ==, 0);
}

static void test_context_ack_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-context-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    qtest_writel(from, REG_PREFIX, 0);
    qtest_writel(from, REG_ENTRY, MGRAS_ENTRY_WORD);
    qtest_writel(from, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);

    sgi_migrate_stopped(from, uri);

    for (int i = 0; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(to, REG_CONTEXT_COMMAND, i);
    }
    g_assert_cmphex(qtest_readl(to, REG_PENDING) & MGRAS_PENDING_CONTEXT, ==,
                     MGRAS_PENDING_CONTEXT);
    qtest_writel(to, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);

    qtest_quit(to);
    qtest_quit(from);
}

static void test_context_payload_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-context-payload-migration-XXXXXX",
                           &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    qtest_writel(from, REG_PREFIX, 0);
    qtest_writel(from, REG_ENTRY, MGRAS_ENTRY_WORD);
    g_assert_cmphex(qtest_readl(from, REG_PENDING) & MGRAS_PENDING_FLAGS, ==,
                    MGRAS_PENDING_FLAGS);
    qtest_writel(from, REG_PENDING_ACK, MGRAS_PENDING_FLAGS);
    for (int i = 0; i < 3; i++) {
        qtest_writel(from, REG_CONTEXT_COMMAND, i);
    }

    sgi_migrate_stopped(from, uri);

    for (int i = 3; i < MGRAS_CONTEXT_WORDS; i++) {
        qtest_writel(to, REG_CONTEXT_COMMAND, i);
    }
    g_assert_cmphex(qtest_readl(to, REG_PENDING) & MGRAS_PENDING_CONTEXT, ==,
                    MGRAS_PENDING_CONTEXT);
    qtest_writel(to, REG_PENDING_ACK, MGRAS_PENDING_CONTEXT);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * glLoadMatrixf carries 16 payload words, more than the 8 the parser held
 * before, and GL state now includes the depth plane and the second colour
 * bank. A packet split by a migration resumes after its twelfth word, and
 * the depth and bank contents cross with it.
 */
static void test_gl_state_migration(void)
{
    static const float shift[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1,
    };
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    mgras_enter_gl_window(from, 256, 640, 128, 128, 0x11);
    qtest_writel(from, REG_VC3_INDEXDATA, VC3_INDEXDATA(0x1e, 0x4));
    gl_viewport(from, 0, 0, 128, 128);
    gl_draw_buffer(from, 2);
    gl_clear_color(from, 1, 0, 1, 1);
    gl_word(from, GL_CLEAR_COLOR_BUFFER);
    gl_one(from, GL_CLEAR_DEPTH_HDR, 0);
    gl_word(from, GL_CLEAR_DEPTH_BUFFER);
    gl_draw_buffer(from, 1);
    gl_one(from, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_word(from, GL_LOAD_MATRIX_HDR);
    for (int i = 0; i < 12; i++) {
        gl_float(from, shift[i]);
    }

    sgi_migrate_stopped(from, uri);

    for (int i = 12; i < 16; i++) {
        gl_float(to, shift[i]);
    }
    gl_color3f(to, 1, 1, 1);
    gl_word(to, GL_BEGIN_TRIANGLES);
    gl_vertex3f(to, -1, -1, 0);
    gl_vertex3f(to, 0, -1, 0);
    gl_vertex3f(to, -1, 0, 0);
    gl_word(to, GL_END_TRIANGLES);
    g_assert_cmphex(mgras_test_pixel(to, 256 + 33, 383 - 1), ==, 0xffffff);
    g_assert_cmphex(mgras_test_pixel(to, 256 + 1, 383 - 1), ==, 0);

    /* Depth cleared to 0 before the migration stops LESS after it. */
    gl_one(to, GL_DEPTH_TEST_HDR, 1);
    gl_one(to, GL_DEPTH_FUNC_HDR, GL_DEPTH_LESS);
    gl_color3f(to, 0, 1, 0);
    gl_word(to, GL_BEGIN_TRIANGLES);
    gl_vertex3f(to, -1, -1, 0);
    gl_vertex3f(to, 0, -1, 0);
    gl_vertex3f(to, -1, 0, 0);
    gl_word(to, GL_END_TRIANGLES);
    g_assert_cmphex(mgras_test_pixel(to, 256 + 33, 383 - 1), ==, 0xffffff);

    /* The back-buffer clear crossed in bank 1. */
    qtest_writeb(to, REG_XMAP_INDEX, 0);
    qtest_writel(to, REG_XMAP_BUF_SELECT, 1u);
    g_assert_cmphex(mgras_test_display_pixel(to, 300, 300), ==, 0xff00ff);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static uint32_t gl_stipple_reverse32(uint32_t value)
{
    uint32_t out = 0;

    for (int bit = 0; bit < 32; bit++) {
        out |= ((value >> bit) & 1) << (31 - bit);
    }
    return out;
}

/* A 32-row upload resumes after migration with all earlier rows retained. */
static void test_gl_polygon_stipple_upload_migration(void)
{
    uint32_t rows[32];
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    for (int row = 0; row < ARRAY_SIZE(rows); row++) {
        rows[row] = 1u << row;
    }
    tmpfs = g_dir_make_tmp("sn0-mgras-gl-stipple-migration-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    gl_transform_start(from);
    gl_width_clear(from);
    gl_word(from, 0x00004e80);
    for (int row = 0; row < 17; row++) {
        gl_word(from, gl_stipple_reverse32(rows[row]));
    }

    sgi_migrate_stopped(from, uri);

    for (int row = 17; row < ARRAY_SIZE(rows); row++) {
        gl_word(to, gl_stipple_reverse32(rows[row]));
    }
    gl_one(to, 0x00006d04, 1);
    gl_width_clear(to);
    gl_word(to, GL_BEGIN_QUADS);
    gl_vertex2f(to, 0, 0);
    gl_vertex2f(to, 32, 0);
    gl_vertex2f(to, 32, 32);
    gl_vertex2f(to, 0, 32);
    gl_word(to, GL_END_OF(GL_BEGIN_QUADS));
    for (int row = 0; row < ARRAY_SIZE(rows); row++) {
        g_assert_cmphex(gl_window_pixel(to, row, row), ==, 0xffffff);
        g_assert_cmphex(gl_window_pixel(to, (row + 1) & 31, row), ==, 0);
    }
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * Every context's GL state crosses a migration, and so does which one is
 * current: b's clear colour applies before any switch on the destination,
 * and a's after one. So does a's half-sent glClearColor, which a finishes
 * on the destination, b's matrix stack and b's edge flag.
 */
static void test_gl_context_table_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-contexts-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_clear_color(from, 1, 0, 0, 1);
    gl_word(from, GL_CLEAR_COLOR_HDR);
    gl_float(from, 1);
    gl_float(from, 0);
    gl_enter_slot(from, GL_SLOT_B);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_clear_color(from, 0, 1, 0, 1);
    gl_viewport(from, 0, 0, 128, 128);
    gl_window_projection(from);
    gl_one(from, GL_MATRIX_MODE_HDR, GL_MODELVIEW);
    gl_three(from, GL_TRANSLATE_HDR, 8, 0, 0);
    gl_word(from, GL_PUSH_MATRIX);
    gl_three(from, GL_TRANSLATE_HDR, 0, 16, 0);
    gl_word(from, 0x00001000);

    sgi_migrate_stopped(from, uri);

    g_assert_cmphex(gl_attribute_query(to, 15) & 0x01c00000, ==, 0);

    gl_word(to, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(to, 300, 300), ==, 0x00ff00);
    gl_word(to, GL_POP_MATRIX);
    gl_mark(to, 1, 1);
    g_assert_cmphex(gl_window_pixel(to, 9, 1), ==, 0xffffff);
    gl_enter_slot(to, GL_SLOT_A);
    gl_float(to, 1);
    gl_float(to, 1);
    gl_word(to, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(mgras_test_pixel(to, 300, 300), ==, 0xff00ff);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void migration_gl_read_map(QTestState *qts)
{
    static const uint8_t header[24] = { 0 };
    uint8_t pages[16] = { 0 };

    stq_be_p(pages, 0x30);
    stq_be_p(pages + 8, 0x31);
    qtest_memwrite(qts, 0x2000, header, sizeof(header));
    qtest_memwrite(qts, 0x2018, pages, sizeof(pages));
    qtest_writel(qts, MGRAS_REG(0x130400), 0x00482608);
    qtest_writeq(qts, MGRAS_REG(0x130400), 0x2018);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00483304 << 32) | 2);
    qtest_writeq(qts, MGRAS_REG(0x130400),
                 ((uint64_t)0x00484e04 << 32) | (9u << 16));
}

static void migration_gl_select_read_buffer(QTestState *qts, uint32_t first,
                                            uint32_t second, uint32_t mode)
{
    gl_word(qts, 0x0000440c);
    gl_word(qts, first);
    gl_word(qts, second);
    gl_word(qts, mode);
}

static void migration_gl_read_pixel(QTestState *qts,
                                    const uint8_t expected[4])
{
    static const uint8_t poison[12] = {
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
    };
    uint8_t got[sizeof(poison)];

    qtest_memwrite(qts, 0x30d4c, poison, sizeof(poison));
    gl_word(qts, 0x00009600);
    gl_word(qts, 0x0000d214);
    gl_word(qts, 0x11e23929);
    gl_word(qts, 0x01000000);
    gl_word(qts, 0xff);
    gl_word(qts, 0);
    gl_word(qts, 2);
    gl_word(qts, 0x0000cd24);
    gl_word(qts, 0);
    gl_word(qts, 1);
    gl_word(qts, 4);
    gl_word(qts, 0x31e);
    gl_word(qts, 0);
    gl_word(qts, 2);
    gl_word(qts, 1);
    gl_word(qts, 0x18d);
    gl_word(qts, 0);

    gl_word(qts, 0x00007c08);
    gl_word(qts, 1);
    gl_word(qts, 4);
    gl_word(qts, GL_INLINE_BLOCK | 16);
    gl_word(qts, 0x153);
    gl_word(qts, 0x00010001);
    gl_word(qts, 0x2b2);
    gl_word(qts, 0);
    gl_word(qts, 0x00007f08);
    gl_word(qts, 2);
    gl_word(qts, 4);
    gl_word(qts, GL_INLINE_BLOCK | 16);
    gl_word(qts, 0x158);
    gl_word(qts, 0x00010001);
    gl_word(qts, 0x159);
    gl_word(qts, 0x00400080);
    gl_word(qts, 0x00007c08);
    gl_word(qts, 2);
    gl_word(qts, 6);
    gl_word(qts, GL_INLINE_BLOCK | 24);
    gl_word(qts, REG_BLOCK_START);
    gl_word(qts, 0x00040004);
    gl_word(qts, 0x31a);
    gl_word(qts, REG_BLOCK_END);
    gl_word(qts, 0x00040004);
    gl_word(qts, 0x31c);

    gl_one(qts, 0x0000db04, 0x4009);
    gl_word(qts, 0x0000f200);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 1, ==, 1);
    gl_word(qts, 0x000c0004);
    gl_word(qts, 0x200);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x00080004 << 32) | 0x80000000);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x00080404 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 (uint64_t)0x00080504 << 32);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x00080604 << 32) | 0xd50);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x00080704 << 32) | 1);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x00080804 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x000a0204 << 32) | 4);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x000a0304 << 32) | 0x00010001);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x000a0404 << 32) | 0x00400080);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x000a0604 << 32) | 0xa);
    qtest_writel(qts, REG_HQ_FLAGS_CLEAR, MGRAS_HQ_FLAG_FENCE);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x00080b04 << 32) | 0xbf);
    qtest_writeq(qts, REG_CONTEXT_COMMAND,
                 ((uint64_t)0x000a0504 << 32) | 0x4009);
    gl_word(qts, 0x000e0400);
    gl_word(qts, 0x000e0100);
    gl_word(qts, 0x000a0704);
    gl_word(qts, 0);
    gl_word(qts, 0x000c0004);
    gl_word(qts, 0xe00);
    g_assert_cmphex(qtest_readl(qts, REG_HQ_STATUS) & 1, ==, 1);
    gl_word(qts, 0x0000da00);
    gl_word(qts, 0x0000d300);
    gl_word(qts, 0x00009700);

    qtest_memread(qts, 0x30d4c, got, sizeof(got));
    g_assert_cmpmem(got, 4, poison, 4);
    g_assert_cmpmem(got + 4, 4, expected, 4);
    g_assert_cmpmem(got + 8, 4, poison + 8, 4);
}

/* Read-buffer selection belongs to each GL context, not the shared drawable. */
static void test_gl_read_buffer_context_switch(void)
{
    static const uint8_t front[4] = { 0xff, 0, 0, 0xff };
    static const uint8_t back[4] = { 0, 0xff, 0, 0xff };
    QTestState *s = qtest_init(MIGRATION_ARGS);

    gl_enter_slot(s, GL_SLOT_A);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_viewport(s, 0, 0, 128, 128);
    gl_window_projection(s);
    gl_draw_buffer(s, 1);
    gl_clear_color(s, 1, 0, 0, 1);
    gl_word(s, GL_CLEAR_COLOR_BUFFER);
    gl_draw_buffer(s, 2);
    gl_clear_color(s, 0, 1, 0, 1);
    gl_word(s, GL_CLEAR_COLOR_BUFFER);
    migration_gl_select_read_buffer(s, 1, 0, 0x405);

    gl_enter_slot(s, GL_SLOT_B);
    gl_word(s, GL_CONTEXT_OPENER);
    gl_viewport(s, 0, 0, 128, 128);
    gl_window_projection(s);
    migration_gl_select_read_buffer(s, 0, 0, 0x404);
    migration_gl_read_map(s);

    gl_enter_slot(s, GL_SLOT_A);
    migration_gl_read_pixel(s, back);
    gl_enter_slot(s, GL_SLOT_B);
    migration_gl_read_pixel(s, front);
    g_assert_cmphex(mgras_qom_get_uint(s, "x-test-error"), ==, 0);

    qtest_quit(s);
}

/*
 * The longest block a header can frame: a 20-bit length of 0xfffff bytes
 * starting at byte 2 of its first word takes 0x40001 words. A migration
 * just after the header carries the discard, and the destination goes on
 * consuming the block's words: a fence token is data there.
 */
static void test_gl_longest_block_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-block-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_INLINE_BLOCK | 2 << 22 | 0xfffff);

    sgi_migrate_stopped(from, uri);

    gl_word(to, MGRAS_FENCE_TOKEN);
    g_assert_cmphex(qtest_readl(to, REG_HQ_FLAGS) & MGRAS_HQ_FLAG_FENCE, ==,
                    0);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

/*
 * A primitive left open by a migration finishes on the destination: the
 * vertices each context held before it cross, in the loaded context (the
 * default slot's, the no-context entry) and in one switched out.
 */
static void test_gl_open_primitive_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-primitive-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_viewport(from, 0, 0, 128, 128);
    gl_window_projection(from);
    gl_color3f(from, 1, 0, 0);
    gl_word(from, GL_BEGIN_TRIANGLES);
    gl_vertex2f(from, 10.25f, 10.25f);
    gl_vertex2f(from, 10.875f, 10.25f);
    gl_enter_slot(from, GL_SLOT_DEFAULT);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_viewport(from, 0, 0, 128, 128);
    gl_window_projection(from);
    gl_color3f(from, 0, 1, 0);
    gl_word(from, GL_BEGIN_TRIANGLES);
    gl_vertex2f(from, 20.25f, 20.25f);
    gl_vertex2f(from, 20.875f, 20.25f);

    sgi_migrate_stopped(from, uri);

    gl_vertex2f(to, 20.25f, 20.875f);
    gl_word(to, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(to, 20, 20), ==, 0x00ff00);
    g_assert_cmphex(gl_window_pixel(to, 10, 10), ==, 0);
    gl_enter_slot(to, GL_SLOT_A);
    gl_vertex2f(to, 10.25f, 10.875f);
    gl_word(to, GL_END_TRIANGLES);
    g_assert_cmphex(gl_window_pixel(to, 10, 10), ==, 0xff0000);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}

static void gl_native_stipple_hidden_migrate_setup(QTestState *s)
{
    gl_native_stipple_hidden_setup(s, 0x1f, true, 0x0400);
}

static void test_gl_native_stipple_hidden_migration(void)
{
    gl_migrate(gl_native_stipple_hidden_migrate_setup,
               gl_native_stipple_hidden_check);
}

static void test_gl_native_stipple_loop_migration(void)
{
    gl_migrate(gl_native_stipple_loop_setup, gl_native_stipple_loop_check);
}

static void test_gl_native_polygon_migration(void)
{
    gl_migrate(gl_polygon_context_setup, gl_polygon_context_check);
}

static void gl_texture_setup(QTestState *from)
{
    gl_enter_slot(from, GL_SLOT_A);
    gl_word(from, GL_CONTEXT_OPENER);
    gl_one(from, GL_MATRIX_MODE_HDR, GL_TEXTURE);
    gl_three(from, GL_TRANSLATE_HDR, 0.5f, 0, 0);
    gl_word(from, GL_PUSH_MATRIX);
    gl_three(from, GL_TRANSLATE_HDR, 0.25f, 0, 0);
}

static void gl_texture_check(QTestState *to)
{
    gl_enter_slot(to, GL_SLOT_A);
    g_assert_cmphex(gl_query(to, GL_Q_TEXTURE + 12, 0), ==, gl_bits(0.75f));
    g_assert_cmphex(gl_query(to, GL_Q_TEXTURE_DEPTH, 1), ==, 0xb55);
    gl_word(to, GL_POP_MATRIX);
    g_assert_cmphex(gl_query(to, GL_Q_TEXTURE + 12, 0), ==, gl_bits(0.5f));
}

/* Each context's texture matrix and stack cross a migration. */
static void test_gl_texture_matrix_migration(void)
{
    gl_migrate(gl_texture_setup, gl_texture_check);
}

/* A context read out before a migration is written back after it. */
static void test_gl_eviction_migration(void)
{
    g_autofree char *tmpfs = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_cmdline = NULL;
    GError *err = NULL;
    QTestState *from, *to;

    tmpfs = g_dir_make_tmp("sn0-mgras-gl-eviction-XXXXXX", &err);
    g_assert_no_error(err);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_init(MIGRATION_ARGS);
    dest_cmdline = g_strdup_printf(MIGRATION_ARGS " -incoming %s", uri);
    to = qtest_init(dest_cmdline);

    gl_eviction_contexts(from);
    gl_enter_slot(from, GL_SLOT_DEFAULT);
    gl_eram_map(from);
    gl_eram_read(from, GL_EVICT_FROM);
    gl_one(from, GL_ERAM_COUNT_HDR, GL_EVICT_CHUNK);
    gl_eram_kick(from, 0, GL_EVICT_CHUNK * 4, GL_ERAM_WRITE_KICK);

    sgi_migrate_stopped(from, uri);

    gl_word(to, GL_ERAM_WRITE_HDR);
    gl_word(to, GL_EVICT_TO);
    gl_word(to, GL_EVICT_CHUNK);
    gl_word(to, GL_ERAM_END);
    gl_enter_slot(to, GL_EVICT_TO);
    gl_word(to, GL_CLEAR_COLOR_BUFFER);
    g_assert_cmphex(gl_window_pixel(to, 100, 100), ==, 0xff0000);
    g_assert_cmphex(mgras_qom_get_uint(to, "x-test-error"), ==, 0);

    qtest_quit(to);
    qtest_quit(from);
    g_rmdir(tmpfs);
}
int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qts = qtest_init(MACHINE_ARGS);


    qtest_add_func("sn0/origin2000/mgras/gl-pixels-float-migration",
                   test_gl_pixels_float_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-pixels-migration",
                   test_gl_pixels_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-migration-rejects-"
                   "load-past-tram",
                   test_gl_texture_migration_rejects_load_past_tram);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-migration",
                   test_gl_texture_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-volume-texture-migration",
                   test_gl_volume_texture_migration);
    qtest_add_func("sn0/origin2000/mgras/"
                   "gl-texture-missing-mip-level-migration",
                   test_gl_texture_missing_mip_level_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-deep-slots-migrate",
                   test_gl_deep_slots_migrate);
    qtest_add_func("sn0/origin2000/mgras/gl-eviction-migration",
                   test_gl_eviction_migration);
    qtest_add_func("sn0/origin2000/mgras/context-ack-migration",
                   test_context_ack_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-state-migration",
                   test_gl_state_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-polygon-stipple/upload-migration",
                   test_gl_polygon_stipple_upload_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-texture-matrix-migration",
                   test_gl_texture_matrix_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-longest-block-migration",
                   test_gl_longest_block_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-context-table-migration",
                   test_gl_context_table_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-read-buffer-context-switch",
                   test_gl_read_buffer_context_switch);
    qtest_add_func("sn0/origin2000/mgras/gl-open-primitive-migration",
                   test_gl_open_primitive_migration);
    qtest_add_func("sn0/origin2000/mgras/context-payload-migration",
                   test_context_payload_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-native-stipple/hidden-migration",
                    test_gl_native_stipple_hidden_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-native-stipple/loop-migration",
                    test_gl_native_stipple_loop_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-native-texcoord/migration",
                    test_gl_native_texcoord_migration);
    qtest_add_func("sn0/origin2000/mgras/gl-native-polygon/migration",
                    test_gl_native_polygon_migration);

    ret = g_test_run();
    qtest_quit(qts);

    return ret;
}
