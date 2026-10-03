/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "io/channel-buffer.h"
#include "../migration/qemu-file.h"
#include "hw/misc/sgi-kona-texture.h"
#include "hw/misc/sgi-kona-gl-decode.h"

static const uint32_t red[] = { 0xff0000ff };
static const uint32_t green[] = { 0x00ff00ff };

static uint32_t texel(SGIKonaTextureState *s, unsigned i, unsigned x)
{
    const SGIGLTexImage *im = sgi_kona_texture_resolve(s);
    const SGIGLTexLevel *l;
    uint32_t rgba;

    g_assert_nonnull(im);
    g_assert_cmpuint(i, <, im->pyramid->count);
    l = &im->pyramid->levels[i];
    g_assert_true(l->read_texel(l->opaque, l->level, x, 0, &rgba));
    return rgba;
}

static void test_objects(void)
{
    SGIKonaTextureState s;
    uint64_t bytes;
    SGIKonaTextureParams p;

    sgi_kona_texture_init(&s);
    for (unsigned k = 0; k < 40; k++) {
        g_assert_true(sgi_kona_texture_upload(&s, k * 100003, 0, 1, 1,
                                             SGI_GL_RGBA, SGI_GL_RGBA, red));
    }
    g_assert_cmpuint(g_hash_table_size(s.objects), ==, 40);
    g_assert_true(sgi_kona_texture_bind(&s, 0, NULL));
    g_assert_cmphex(texel(&s, 0, 0), ==, red[0]);
    bytes = s.bytes;
    g_assert_false(sgi_kona_texture_upload(&s, 0, 0, 3, 1,
                                          SGI_GL_RGBA, SGI_GL_RGBA, green));
    g_assert_false(sgi_kona_texture_upload(&s, 0, 0, 1, 1,
                                          SGI_GL_RGB, SGI_GL_RGBA, green));
    g_assert_false(sgi_kona_texture_upload(&s, 0, 31, 1, 1,
                                          SGI_GL_RGBA, SGI_GL_RGBA, green));
    /* The oversized image is rejected before the one-word source is read. */
    g_assert_false(sgi_kona_texture_upload(&s, 0, 0, 8192, 8192,
                                          SGI_GL_RGBA, SGI_GL_RGBA, green));
    /* Image alone fits 64MiB, but its object metadata must fit too. */
    g_assert_false(sgi_kona_texture_upload(&s, 0, 0, 4096, 4096,
                                          SGI_GL_RGBA, SGI_GL_RGBA, green));
    g_assert_cmpuint(s.bytes, ==, bytes);
    g_assert_cmphex(texel(&s, 0, 0), ==, red[0]);
    g_assert_true(sgi_kona_texture_upload(&s, 0, 0, 1, 1,
                                         SGI_GL_RGBA, SGI_GL_RGBA, green));
    g_assert_cmpuint(s.bytes, ==, bytes);
    g_assert_cmphex(texel(&s, 0, 0), ==, green[0]);
    sgi_kona_texture_defaults(&p);
    p.post_scale[0] = INFINITY;
    g_assert_false(sgi_kona_texture_bind(&s, 100003, &p));
    sgi_kona_texture_defaults(&p);
    p.mag_filter = 0;
    g_assert_false(sgi_kona_texture_bind(&s, 100003, &p));
    g_assert_cmphex(texel(&s, 0, 0), ==, green[0]);
    sgi_kona_texture_delete(&s, 100003);
    g_assert_nonnull(sgi_kona_texture_resolve(&s));
    sgi_kona_texture_delete(&s, 0);
    g_assert_null(sgi_kona_texture_resolve(&s));
    g_assert_true(sgi_kona_texture_bind(&s, 0, NULL));
    g_assert_cmpuint(sgi_kona_texture_resolve(&s)->pyramid->count, ==, 0);
    g_assert_true(sgi_kona_texture_bind(&s, UINT32_MAX, NULL));
    g_assert_null(sgi_kona_texture_resolve(&s));
    sgi_kona_texture_reset(&s);
    g_assert_cmpuint(s.bytes, ==, 0);
    g_assert_cmpuint(g_hash_table_size(s.objects), ==, 0);
    sgi_kona_texture_cleanup(&s);
}

static void assert_image(SGIKonaTextureState *s, const uint32_t *pixels,
                         uint32_t width, uint32_t height)
{
    const SGIGLTexImage *im = sgi_kona_texture_resolve(s);
    const SGIGLTexLevel *l;

    g_assert_nonnull(im);
    g_assert_cmpuint(im->pyramid->count, ==, 1);
    l = &im->pyramid->levels[0];
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width; x++) {
            uint32_t rgba;

            g_assert_true(l->read_texel(l->opaque, l->level, x, y, &rgba));
            g_assert_cmphex(rgba, ==, pixels[y * width + x]);
        }
    }
}

static void test_subimage(void)
{
    SGIKonaTextureState s;
    const uint32_t original[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const uint32_t patch[] = { 9, 10, 11, 12 };
    const uint32_t updated[] = { 1, 9, 10, 4, 5, 11, 12, 8 };
    const uint32_t aliased[] = { 1, 1, 9, 4, 5, 10, 4, 8 };
    SGIKonaTextureObject *o;
    uint64_t bytes;

    sgi_kona_texture_init(&s);
    g_assert_true(sgi_kona_texture_upload(&s, 7, 0, 4, 2,
                                         SGI_GL_RGBA, SGI_GL_RGBA, original));
    g_assert_true(sgi_kona_texture_bind(&s, 7, NULL));
    bytes = s.bytes;
    g_assert_true(sgi_kona_texture_subimage(&s, 7, 0, 1, 0, 2, 2, patch));
    assert_image(&s, updated, 4, 2);
    g_assert_cmpuint(s.bytes, ==, bytes);
    g_assert_false(sgi_kona_texture_subimage(&s, 7, 0, 3, 0, 2, 2, patch));
    g_assert_false(sgi_kona_texture_subimage(&s, 7, 0, UINT32_MAX, 0,
                                            2, 2, patch));
    g_assert_false(sgi_kona_texture_subimage(&s, 7, 0, 0, UINT32_MAX,
                                            2, 2, patch));
    g_assert_false(sgi_kona_texture_subimage(&s, 7, 0, 0, 0,
                                            UINT32_MAX, 2, patch));
    g_assert_false(sgi_kona_texture_subimage(&s, 7, 1, 0, 0, 2, 2, patch));
    g_assert_false(sgi_kona_texture_subimage(&s, 8, 0, 0, 0, 2, 2, patch));
    g_assert_false(sgi_kona_texture_subimage(&s, 7, 0, 0, 0, 2, 2, NULL));
    g_assert_true(sgi_kona_texture_subimage(&s, 7, 0, 4, 2, 0, 0, NULL));
    assert_image(&s, updated, 4, 2);
    g_assert_cmpuint(s.bytes, ==, bytes);
    g_assert_cmpuint(g_hash_table_size(s.objects), ==, 1);

    /* A source borrowed from the same level is copied before any store. */
    o = g_hash_table_lookup(s.objects, GUINT_TO_POINTER(7));
    g_assert_true(sgi_kona_texture_subimage(&s, 7, 0, 1, 0, 2, 2,
                                           o->levels[0].pixels));
    assert_image(&s, aliased, 4, 2);
    sgi_kona_texture_cleanup(&s);
}

static void store_colour(void *opaque, SGIColourTarget target, int x, int y,
                         uint32_t colour)
{
    g_assert_cmpint(x, ==, 1);
    g_assert_cmpint(y, ==, 1);
    *(uint32_t *)opaque = colour;
}

static uint32_t draw(SGIKonaTextureState *s, SGIGLContext *c)
{
    uint32_t colour = 0xdeadbeef;
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8, .depth_bits = 24,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .line_rule = SGI_LINE_COVERAGE_DOUBLE,
        .evidence = "unit test",
    };
    SGIRasterTarget target = {
        .store_colour = store_colour, .opaque = &colour,
        .texture = sgi_kona_texture_resolve(s),
    };
    float v[SGI_GL_VERTEX_FLOATS] = {
        [3] = 1, [4] = 1, [5] = 1, [6] = 1, [7] = 1,
        [SGI_GL_V_TEXCOORD] = 0.5,
        [SGI_GL_V_TEXCOORD + 1] = 0.5,
        [SGI_GL_V_TEXCOORD + 3] = 1,
    };
    SGIGLDraw d = {
        .mode = SGI_GL_POINTS, .count = 1, .vertices = v, .state = &c->cur,
    };

    sgi_soft_draw(&rules, &target, &d);
    return colour;
}

static void test_sampling(void)
{
    SGIKonaTextureState s;
    SGIKonaTextureParams p;
    SGIGLContext c;
    SGIGLDrawable d = {
        .bound = true, .clip = { .n = 1, .r[0] = { 0, 0, 1, 1 } },
    };
    const uint32_t pixels[] = { 0xff0000ff, 0x00ff00ff,
                                0x0000ffff, 0xffffffff };

    sgi_kona_texture_init(&s);
    sgi_gl_init(&c, 0);
    sgi_gl_set_drawable(&c, &d);
    c.cur.viewport[2] = c.cur.viewport[3] = 2;
    c.cur.texture_2d = 1;
    c.cur.texture_env_mode = SGI_GL_REPLACE;
    sgi_kona_texture_defaults(&p);
    p.min_filter = p.mag_filter = SGI_GL_NEAREST;
    g_assert_true(sgi_kona_texture_upload(&s, 99, 0, 2, 2,
                                         SGI_GL_RGBA, SGI_GL_RGBA, pixels));
    g_assert_true(sgi_kona_texture_bind(&s, 99, &p));
    g_assert_cmphex(draw(&s, &c), ==, 0xffffff);
    p.post_scale[0] = 0.5;
    p.post_bias[1] = -2;
    p.post_bias[2] = 2;
    g_assert_true(sgi_kona_texture_bind(&s, 99, &p));
    g_assert_cmphex(draw(&s, &c), ==, 0x8000ff);
    sgi_kona_texture_defaults(&p);
    p.min_filter = SGI_GL_NEAREST;
    p.mag_filter = SGI_GL_LINEAR;
    g_assert_true(sgi_kona_texture_bind(&s, 99, &p));
    g_assert_cmphex(draw(&s, &c), ==, 0x808080);
    p.min_filter = SGI_GL_NEAREST_MIPMAP_NEAREST;
    p.min_lod = p.max_lod = 1;
    g_assert_true(sgi_kona_texture_bind(&s, 99, &p));
    /* Missing required level disables texturing and leaves primary white. */
    g_assert_cmphex(draw(&s, &c), ==, 0xffffff);
    g_assert_true(sgi_kona_texture_upload(&s, 99, 1, 1, 1,
                                         SGI_GL_RGBA, SGI_GL_RGBA, red));
    g_assert_cmphex(draw(&s, &c), ==, 0xff0000);
    c.cur.fog = 1;
    c.cur.fog_mode = SGI_GL_LINEAR;
    c.cur.fog_start = -1;
    c.cur.fog_end = 1;
    c.cur.fog_color[2] = 1;
    g_assert_cmphex(draw(&s, &c), ==, 0x800080);
    sgi_gl_cleanup(&c);
    sgi_kona_texture_cleanup(&s);
}

static GByteArray *save(SGIKonaTextureState *s)
{
    QIOChannelBuffer *b = qio_channel_buffer_new(4096);
    QEMUFile *f = qemu_file_new_output(QIO_CHANNEL(b));
    GByteArray *a = g_byte_array_new();

    g_assert_true(vmstate_info_sgi_kona_texture.save(f, s, sizeof(*s), NULL,
                                                   NULL, &error_abort));
    g_assert_cmpint(qemu_fflush(f), ==, 0);
    g_byte_array_append(a, b->data, b->usage);
    qemu_fclose(f);
    object_unref(OBJECT(b));
    return a;
}

static bool load(GByteArray *a, SGIKonaTextureState *s)
{
    QIOChannelBuffer *b = qio_channel_buffer_new(a->len);
    QEMUFile *f;
    Error *err = NULL;
    bool ok;

    memcpy(b->data, a->data, a->len);
    b->usage = a->len;
    f = qemu_file_new_input(QIO_CHANNEL(b));
    ok = vmstate_info_sgi_kona_texture.load(f, s, sizeof(*s), NULL, &err);
    g_assert_true(ok == (err == NULL));
    error_free(err);
    qemu_fclose(f);
    object_unref(OBJECT(b));
    return ok;
}

static void test_migration(void)
{
    SGIKonaTextureState src, dst;
    GByteArray *a;
    uint64_t bytes;
    SGIKonaTextureParams p;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    g_assert_true(sgi_kona_texture_upload(&src, 0xf2345678, 1, 1, 1,
                                         SGI_GL_RGBA, SGI_GL_RGBA, green));
    g_assert_true(sgi_kona_texture_upload(&src, 0xf2345678, 0, 1, 1,
                                         SGI_GL_RGBA, SGI_GL_RGBA, red));
    g_assert_true(sgi_kona_texture_bind(&src, 0xf2345678, NULL));
    sgi_kona_texture_defaults(&p);
    p.post_scale[0] = 0.5;
    p.post_bias[2] = 0.25;
    p.lod_scale_s = 2;
    p.lod_scale_t = 0.5;
    g_assert_true(sgi_kona_texture_bind(&src, 0xf2345678, &p));
    a = save(&src);
    g_assert_true(load(a, &dst));
    {
        const SGIGLTexImage *im = sgi_kona_texture_resolve(&dst);
        double rgba[] = { 1, 0, 0, 1 };

        g_assert_cmpfloat(im->lod_scale_s, ==, 2);
        g_assert_cmpfloat(im->lod_scale_t, ==, 0.5);
        im->post_filter(im->opaque, rgba);
        g_assert_cmpfloat(rgba[0], ==, 0.5);
        g_assert_cmpfloat(rgba[2], ==, 0.25);
    }
    g_assert_cmpuint(dst.bytes, ==, src.bytes);
    g_assert_cmphex(texel(&dst, 0, 0), ==, red[0]);
    g_assert_cmphex(texel(&dst, 1, 0), ==, green[0]);
    bytes = dst.bytes;
    /* First level width: header16 + object8 + sampler136 + level id4. */
    stl_be_p(a->data + 164, UINT32_MAX);
    g_assert_false(load(a, &dst));
    g_assert_cmpuint(dst.bytes, ==, bytes);
    g_assert_cmphex(texel(&dst, 0, 0), ==, red[0]);
    g_byte_array_unref(a);
    a = save(&src);
    g_byte_array_set_size(a, a->len - 1);
    g_assert_false(load(a, &dst));
    g_assert_cmphex(texel(&dst, 1, 0), ==, green[0]);
    g_byte_array_unref(a);
    a = save(&src);
    stl_be_p(a->data + 4, UINT32_MAX);
    g_assert_false(load(a, &dst));
    g_assert_cmpuint(dst.bytes, ==, bytes);
    g_byte_array_unref(a);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native(void)
{
    SGIKonaTextureState s;
    SGIKonaTextureParams p;
    uint32_t table[16];
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [1] = 2, [2] = 2, [10] = 42,
        [13] = 1, [14] = 1, [16] = 0x23, [17] = 0x2181, [18] = 17,
    };
    const uint32_t pixels[] = { 0xff0000ff, 0x00ff00ff,
                                0x0000ffff, 0xffffffff };
    const uint32_t updated[] = { 0xff0000ff, 0x00ff00ff,
                                 0x0000ffff, 0x00ff00ff };
    uint64_t bytes;

    sgi_kona_texture_init(&s);
    sgi_kona_texture_defaults(&p);
    p.min_filter = p.mag_filter = SGI_GL_NEAREST;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[0] = 17;
    /* Physical data can arrive before the logical level map. */
    g_assert_true(sgi_kona_texture_native_upload(&s, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_true(sgi_kona_texture_bind(&s, 42, &p));
    g_assert_cmpuint(sgi_kona_texture_resolve(&s)->pyramid->count, ==, 0);
    g_assert_true(sgi_kona_texture_native_table(&s, 42, table));
    assert_image(&s, pixels, 2, 2);
    bytes = s.bytes;
    h[19] = 1;
    g_assert_false(sgi_kona_texture_native_upload(&s, h, pixels,
                                                 SGI_GL_RGBA, SGI_GL_RGBA));
    h[19] = 0;
    h[17] |= 0x20;
    g_assert_false(sgi_kona_texture_native_upload(&s, h, pixels,
                                                 SGI_GL_RGBA, SGI_GL_RGBA));
    h[17] &= ~0x20u;
    assert_image(&s, pixels, 2, 2);
    g_assert_cmpuint(s.bytes, ==, bytes);
    h[1] = h[2] = h[11] = h[12] = 1;
    g_assert_true(sgi_kona_texture_native_upload(&s, h, green,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    assert_image(&s, updated, 2, 2);
    g_assert_cmpuint(s.bytes, ==, bytes);
    h[11] = h[13] = UINT32_MAX;
    g_assert_false(sgi_kona_texture_native_upload(&s, h, green,
                                                 SGI_GL_RGBA, SGI_GL_RGBA));
    assert_image(&s, updated, 2, 2);
    h[10] = 43;
    h[1] = h[2] = 2;
    h[11] = h[12] = 0;
    h[13] = h[14] = 1;
    /* A different object can instead receive its table before its data. */
    g_assert_true(sgi_kona_texture_native_table(&s, 43, table));
    g_assert_true(sgi_kona_texture_native_upload(&s, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_true(sgi_kona_texture_bind(&s, 43, &p));
    assert_image(&s, pixels, 2, 2);
    /* Two banks matching the same descriptor leave the level unresolved. */
    h[17] ^= 8;
    g_assert_true(sgi_kona_texture_native_upload(&s, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_cmpuint(sgi_kona_texture_resolve(&s)->pyramid->count, ==, 0);
    sgi_kona_texture_delete(&s, 43);
    g_assert_true(sgi_kona_texture_bind(&s, 42, &p));
    assert_image(&s, updated, 2, 2);
    g_assert_cmpuint(s.bytes, ==, bytes);
    sgi_kona_texture_delete(&s, 42);
    g_assert_cmpuint(s.bytes, ==, 0);
    sgi_kona_texture_cleanup(&s);
}

static void test_native_chunks(void)
{
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    SGIGLContext c;
    SGIGLDrawable d = {
        .bound = true, .clip = { .n = 1, .r[0] = { 0, 0, 1, 1 } },
    };
    uint32_t table[16];
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [1] = 1, [2] = 2, [10] = 7,
        [11] = 1, [13] = 1, [14] = 1, [17] = 0x2181, [18] = 17,
    };
    const uint32_t right[] = { 0x00ff00ff, 0x00ff00ff };
    const uint32_t left[] = { 0xff0000ff, 0x0000ffff };
    const uint32_t full[] = { 0xff0000ff, 0x00ff00ff,
                              0x0000ffff, 0x00ff00ff };
    GByteArray *a;
    uint64_t bytes;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    sgi_gl_init(&c, 0);
    sgi_gl_set_drawable(&c, &d);
    c.cur.viewport[2] = c.cur.viewport[3] = 2;
    c.cur.texture_2d = 1;
    c.cur.texture_env_mode = SGI_GL_REPLACE;
    sgi_kona_texture_defaults(&p);
    p.min_filter = p.mag_filter = SGI_GL_NEAREST;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[0] = 17;
    g_assert_true(sgi_kona_texture_native_dimensions(&src, 7, 2, 2));
    g_assert_true(sgi_kona_texture_native_table(&src, 7, table));
    g_assert_true(sgi_kona_texture_bind(&src, 7, &p));
    /* The first rectangle arrives at the right edge, leaving a hole. */
    g_assert_true(sgi_kona_texture_native_upload(&src, h, right,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_cmpuint(sgi_kona_texture_resolve(&src)->pyramid->count, ==, 0);
    a = save(&src);
    g_assert_true(load(a, &dst));
    /* Missing texels and unused bitmap bits cannot carry forged state. */
    stl_be_p(a->data + 328, 1);
    g_assert_false(load(a, &dst));
    stl_be_p(a->data + 328, 0);
    a->data[344] |= 0x80;
    g_assert_false(load(a, &dst));
    a->data[344] = 0;
    g_assert_false(load(a, &dst));
    g_byte_array_unref(a);
    g_assert_cmpuint(dst.bytes, ==, src.bytes);
    g_assert_cmpuint(sgi_kona_texture_resolve(&dst)->pyramid->count, ==, 0);
    h[11] = h[13] = 0;
    g_assert_true(sgi_kona_texture_native_upload(&dst, h, left,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    assert_image(&dst, full, 2, 2);
    g_assert_cmphex(draw(&dst, &c), ==, 0x00ff00);
    bytes = dst.bytes;
    h[11] = h[13] = UINT32_MAX;
    g_assert_false(sgi_kona_texture_native_upload(&dst, h, left,
                                                 SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_cmpuint(dst.bytes, ==, bytes);
    assert_image(&dst, full, 2, 2);
    sgi_kona_texture_delete(&dst, 7);
    g_assert_cmpuint(dst.bytes, ==, 0);
    sgi_gl_cleanup(&c);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_growth(void)
{
    SGIKonaTextureState s;
    SGIKonaTextureParams p;
    uint32_t table[16];
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [1] = 1, [2] = 1, [10] = 7,
        [11] = 2, [13] = 2, [17] = 0x2181, [18] = 17,
    };
    const uint32_t first[] = { 0xff0000ff, 0x00ff00ff };
    const uint32_t full[] = { 0xff0000ff, 0x00ff00ff,
                              0xff0000ff, 0x00ff00ff };
    uint64_t bytes;
    GByteArray *a;

    sgi_kona_texture_init(&s);
    sgi_kona_texture_defaults(&p);
    p.min_filter = p.mag_filter = SGI_GL_NEAREST;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[0] = 17;
    g_assert_true(sgi_kona_texture_native_dimensions(&s, 7, 4, 1));
    g_assert_true(sgi_kona_texture_native_table(&s, 7, table));
    g_assert_true(sgi_kona_texture_bind(&s, 7, &p));
    /* A single texel first establishes a non-power-of-two physical extent. */
    g_assert_true(sgi_kona_texture_native_upload(&s, h, red,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_cmpuint(sgi_kona_texture_resolve(&s)->pyramid->count, ==, 0);
    a = save(&s);
    g_assert_true(load(a, &s));
    g_byte_array_unref(a);
    g_assert_cmpuint(sgi_kona_texture_resolve(&s)->pyramid->count, ==, 0);
    h[11] = h[13] = 3;
    g_assert_true(sgi_kona_texture_native_upload(&s, h, green,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_cmpuint(sgi_kona_texture_resolve(&s)->pyramid->count, ==, 0);
    h[1] = 2;
    h[11] = 0;
    h[13] = 1;
    g_assert_true(sgi_kona_texture_native_upload(&s, h, first,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    assert_image(&s, full, 4, 1);
    bytes = s.bytes;
    g_assert_true(sgi_kona_texture_native_upload(&s, h, first,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_cmpuint(s.bytes, ==, bytes);
    h[1] = 1;
    h[11] = h[13] = 4;
    g_assert_false(sgi_kona_texture_native_upload(&s, h, red,
                                                 SGI_GL_RGBA, SGI_GL_RGBA));
    assert_image(&s, full, 4, 1);
    h[10] = 8;
    h[11] = 0;
    h[13] = h[1] = 4096;
    h[13]--;
    h[2] = 4096;
    h[14] = 4095;
    g_assert_false(sgi_kona_texture_native_upload(&s, h, red,
                                                 SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_cmpuint(s.bytes, ==, bytes);
    sgi_kona_texture_cleanup(&s);
}

static void test_native_format(void)
{
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    uint32_t table[16];
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [1] = 1, [2] = 1, [10] = 7,
        [9] = 0x30, [17] = 0x2181, [18] = 17,
    };
    GByteArray *a;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    sgi_kona_texture_defaults(&p);
    p.min_filter = p.mag_filter = SGI_GL_NEAREST;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[0] = 17;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, red,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    h[9] = 0xe;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, green,
                                                SGI_GL_ALPHA, SGI_GL_ALPHA));
    g_assert_true(sgi_kona_texture_native_table(&src, 7, table));
    g_assert_true(sgi_kona_texture_bind(&src, 7, &p));
    g_assert_cmpuint(sgi_kona_texture_resolve(&src)->pyramid->count, ==, 0);
    g_assert_true(sgi_kona_texture_native_format(&src, 7, 0x30));
    assert_image(&src, red, 1, 1);
    g_assert_false(sgi_kona_texture_native_format(&src, 7, 0x80));
    assert_image(&src, red, 1, 1);
    a = save(&src);
    g_assert_true(load(a, &dst));
    assert_image(&dst, red, 1, 1);
    g_byte_array_unref(a);
    g_assert_true(sgi_kona_texture_native_format(&dst, 7, 0xe));
    assert_image(&dst, green, 1, 1);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_migration(void)
{
    SGIKonaTextureState src, dst;
    uint32_t table[16];
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [1] = 1, [2] = 1, [10] = 99,
        [16] = 0x11, [17] = 0x2189,
    };
    GByteArray *a;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[0] = 0;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, red,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_true(sgi_kona_texture_bind(&src, 99, NULL));
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_assert_cmpuint(sgi_kona_texture_resolve(&dst)->pyramid->count, ==, 0);
    g_assert_true(sgi_kona_texture_native_table(&dst, 99, table));
    assert_image(&dst, red, 1, 1);
    /* Version four had complete native images and no coverage/format fields. */
    g_byte_array_remove_range(a, 332, 1);
    g_byte_array_set_size(a, a->len - 8);
    stl_be_p(a->data, 4);
    g_assert_true(load(a, &dst));
    g_assert_true(sgi_kona_texture_native_table(&dst, 99, table));
    assert_image(&dst, red, 1, 1);
    g_assert_cmpuint(dst.bytes, ==, src.bytes);
    g_byte_array_unref(a);
    g_assert_true(sgi_kona_texture_native_table(&src, 99, table));
    a = save(&src);
    g_assert_true(load(a, &dst));
    assert_image(&dst, red, 1, 1);
    g_assert_cmpuint(dst.bytes, ==, src.bytes);
    /* Native width: header16 + object8 + sampler136 + map72 + header80. */
    stl_be_p(a->data + 312, UINT32_MAX);
    g_assert_false(load(a, &dst));
    assert_image(&dst, red, 1, 1);
    g_byte_array_unref(a);
    a = save(&src);
    g_byte_array_set_size(a, a->len - 1);
    g_assert_false(load(a, &dst));
    assert_image(&dst, red, 1, 1);
    g_byte_array_unref(a);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_sampling(void)
{
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    SGIGLContext c;
    SGIGLDrawable d = {
        .bound = true, .clip = { .n = 1, .r[0] = { 0, 0, 1, 1 } },
    };
    uint32_t table[16] = { 0 };
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [1] = 2, [2] = 2, [10] = 9,
        [13] = 1, [14] = 1, [16] = 0x23, [17] = 0x2181,
    };
    const uint32_t pixels[] = { 0x00ff00ff, 0x00ff00ff,
                                0x00ff00ff, 0x00ff00ff };
    GByteArray *a;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    sgi_gl_init(&c, 0);
    sgi_gl_set_drawable(&c, &d);
    c.cur.viewport[2] = c.cur.viewport[3] = 2;
    c.cur.texture_2d = 1;
    c.cur.texture_env_mode = SGI_GL_REPLACE;
    sgi_kona_texture_defaults(&p);
    p.min_filter = SGI_GL_NEAREST_MIPMAP_NEAREST;
    p.mag_filter = SGI_GL_NEAREST;
    p.min_lod = p.max_lod = 1;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    h[1] = h[2] = 1;
    h[13] = h[14] = 0;
    h[16] = 0x11;
    h[17] ^= 8;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, red,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_true(sgi_kona_texture_native_table(&src, 9, table));
    g_assert_true(sgi_kona_texture_native_dimensions(&src, 9, 2, 2));
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    /* Bind dimensions distinguish mipmap levels sharing zero descriptors. */
    g_assert_cmphex(draw(&src, &c), ==, 0xff0000);
    p.post_scale[0] = 0.5;
    p.post_bias[2] = 0.25;
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    g_assert_cmphex(draw(&src, &c), ==, 0x800040);
    g_assert_false(sgi_kona_texture_native_dimensions(&src, 9, 3, 2));
    g_assert_cmphex(draw(&src, &c), ==, 0x800040);
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_assert_cmphex(draw(&dst, &c), ==, 0x800040);
    stl_be_p(a->data + a->len - 16, 3);
    g_assert_false(load(a, &dst));
    g_assert_cmphex(draw(&dst, &c), ==, 0x800040);
    g_byte_array_unref(a);
    sgi_gl_cleanup(&c);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_negative_lod(void)
{
    SGIKonaTextureState s;
    SGIKonaTextureParams p;
    SGIGLContext c;
    SGIGLDrawable drawable = {
        .bound = true, .clip = { .n = 1, .r[0] = { 1, 1, 1, 1 } },
    };
    uint32_t colour;
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8, .depth_bits = 24,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .line_rule = SGI_LINE_COVERAGE_DOUBLE,
        .evidence = "unit test",
    };
    SGIRasterTarget target = {
        .store_colour = store_colour, .opaque = &colour,
    };
    float vertices[3][SGI_GL_VERTEX_FLOATS] = {
        { [0] = -1, [1] = -1, [3] = 1 },
        { [0] = 3, [1] = -1, [3] = 1, [SGI_GL_V_TEXCOORD] = 8 },
        { [0] = -1, [1] = 3, [3] = 1, [SGI_GL_V_TEXCOORD + 1] = 8 },
    };
    SGIGLDraw draw = {
        .mode = SGI_GL_TRIANGLES, .count = 3,
        .vertices = &vertices[0][0], .state = &c.cur,
    };
    uint32_t bind[5] = { 7, 0, 3 << 2, 0, 0 };
    uint32_t record[22] = { 0, 0x00101010, 32 << 10 };
    const uint32_t pixels[] = { 0xff0000ff, 0xff0000ff,
                                0xff0000ff, 0xff0000ff };

    sgi_kona_texture_init(&s);
    sgi_gl_init(&c, 0);
    sgi_gl_set_drawable(&c, &drawable);
    c.cur.viewport[2] = c.cur.viewport[3] = 2;
    c.cur.texture_2d = 1;
    c.cur.texture_env_mode = SGI_GL_REPLACE;
    for (unsigned v = 0; v < 3; v++) {
        for (unsigned k = 4; k < 8; k++) {
            vertices[v][k] = 1;
        }
        vertices[v][SGI_GL_V_TEXCOORD + 3] = 1;
    }
    g_assert_true(sgi_kona_texture_upload(&s, 7, 0, 2, 2,
                                         SGI_GL_RGBA, SGI_GL_RGBA, pixels));
    g_assert_true(sgi_kona_texture_upload(&s, 7, 1, 1, 1,
                                         SGI_GL_RGBA, SGI_GL_RGBA, green));
    sgi_kona_texture_defaults(&p);
    /* Negative upper bounds force magnification despite a large footprint. */
    g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
    g_assert_true(sgi_kona_texture_bind(&s, 7, &p));
    target.texture = sgi_kona_texture_resolve(&s);
    colour = 0xdeadbeef;
    sgi_soft_draw(&rules, &target, &draw);
    g_assert_cmphex(colour, ==, 0xff0000);
    record[2] = 256 << 10;
    g_assert_true(sgi_kona_gl_texture_sampler(bind, record, &p));
    g_assert_true(sgi_kona_texture_bind(&s, 7, &p));
    target.texture = sgi_kona_texture_resolve(&s);
    colour = 0xdeadbeef;
    sgi_soft_draw(&rules, &target, &draw);
    g_assert_cmphex(colour, ==, 0x00ff00);
    sgi_gl_cleanup(&c);
    sgi_kona_texture_cleanup(&s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    g_test_add_func("/sgi/kona/texture/objects", test_objects);
    g_test_add_func("/sgi/kona/texture/sampling", test_sampling);
    g_test_add_func("/sgi/kona/texture/migration", test_migration);
    g_test_add_func("/sgi/kona/texture/subimage", test_subimage);
    g_test_add_func("/sgi/kona/texture/native", test_native);
    g_test_add_func("/sgi/kona/texture/native-chunks", test_native_chunks);
    g_test_add_func("/sgi/kona/texture/native-growth", test_native_growth);
    g_test_add_func("/sgi/kona/texture/native-format", test_native_format);
    g_test_add_func("/sgi/kona/texture/native-migration",
                    test_native_migration);
    g_test_add_func("/sgi/kona/texture/native-sampling", test_native_sampling);
    g_test_add_func("/sgi/kona/texture/native-negative-lod",
                    test_native_negative_lod);
    return g_test_run();
}
