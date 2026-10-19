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

static void test_depth_storage(void)
{
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    const uint32_t pixels[] = { 0x7fffffff, 0x80000000 };
    const uint32_t patch = UINT32_MAX;
    GByteArray *a;
    const SGIGLTexImage *im;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    sgi_kona_texture_defaults(&p);
    p.min_filter = p.mag_filter = SGI_GL_NEAREST;
    p.shadow_compare = SGI_GL_TEXTURE_GEQUAL_R_SGIX;
    p.shadow_ambient = .25;
    g_assert_true(sgi_kona_texture_upload(&src, 73, 0, 2, 1,
        SGI_GL_DEPTH_COMPONENT, SGI_GL_DEPTH_COMPONENT32_SGIX, pixels));
    g_assert_true(sgi_kona_texture_bind(&src, 73, &p));
    g_assert_cmphex(texel(&src, 0, 0), ==, pixels[0]);
    g_assert_cmphex(texel(&src, 0, 1), ==, pixels[1]);
    g_assert_false(sgi_kona_texture_upload(&src, 73, 0, 2, 1,
        SGI_GL_DEPTH_COMPONENT, SGI_GL_LUMINANCE16, pixels));
    g_assert_false(sgi_kona_texture_upload(&src, 73, 0, 2, 1,
        SGI_GL_LUMINANCE, SGI_GL_DEPTH_COMPONENT32_SGIX, pixels));
    a = save(&src);
    g_assert_true(load(a, &dst));
    im = sgi_kona_texture_resolve(&dst);
    g_assert_cmpuint(im->shadow_compare, ==, SGI_GL_TEXTURE_GEQUAL_R_SGIX);
    g_assert_cmpfloat(im->shadow_ambient, ==, .25);
    g_assert_cmpuint(im->pyramid->levels[0].base_format, ==,
                     SGI_GL_DEPTH_COMPONENT);
    g_assert_cmphex(texel(&dst, 0, 0), ==, pixels[0]);
    g_assert_cmphex(texel(&dst, 0, 1), ==, pixels[1]);
    g_assert_true(sgi_kona_texture_subimage(&dst, 73, 0, 1, 0, 1, 1, &patch));
    g_assert_cmphex(texel(&dst, 0, 1), ==, patch);
    p.shadow_compare = 1;
    g_assert_false(sgi_kona_texture_bind(&dst, 73, &p));
    p.shadow_compare = SGI_GL_TEXTURE_LEQUAL_R_SGIX;
    p.shadow_ambient = NAN;
    g_assert_false(sgi_kona_texture_bind(&dst, 73, &p));
    /* The object-tail fields follow layout and do not shift pixel records. */
    stl_be_p(a->data + a->len - 44, 1);
    g_assert_false(load(a, &dst));
    g_assert_cmphex(texel(&dst, 0, 1), ==, patch);
    g_byte_array_unref(a);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_depth_storage(void)
{
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [1] = 8, [2] = 4, [3] = 13, [4] = 5,
        [9] = 0xd, [10] = 73, [13] = 7, [14] = 3, [16] = 0x23,
        [17] = 0x2200, [18] = 23,
    };
    uint32_t pixels[32], table[16];
    SGIGLDrawState st = { .texture_2d = true,
                         .texture_env_mode = SGI_GL_REPLACE };
    const float coord[4] = { .5, .5, .5, 1 };
    const float color[4] = { 1, 1, 1, .25 };
    float out[4];
    GByteArray *a;

    for (unsigned i = 0; i < ARRAY_SIZE(pixels); i++) {
        pixels[i] = i & 1 ? 0x80000000 : 0x7fffffff;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(table); i++) {
        table[i] = UINT32_MAX;
    }
    table[0] = 23;
    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    sgi_kona_texture_defaults(&p);
    p.min_filter = p.mag_filter = SGI_GL_LINEAR;
    p.shadow_compare = SGI_GL_TEXTURE_LEQUAL_R_SGIX;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, pixels,
        SGI_GL_DEPTH_COMPONENT, SGI_GL_DEPTH_COMPONENT));
    g_assert_true(sgi_kona_texture_native_dimensions(&src, 73, 2, 2));
    g_assert_true(sgi_kona_texture_native_layout(&src, 73, 0x11));
    g_assert_true(sgi_kona_texture_native_table(&src, 73, table));
    g_assert_true(sgi_kona_texture_native_internal_format(&src, 73,
        SGI_GL_DEPTH_COMPONENT, SGI_GL_DEPTH_COMPONENT));
    g_assert_true(sgi_kona_texture_bind(&src, 73, &p));
    g_assert_true(sgi_gl_texture_apply_pixel(&st,
        sgi_kona_texture_resolve(&src), coord, color, out));
    g_assert_cmpfloat(out[0], ==, .5);
    g_assert_cmpfloat(out[3], ==, .25);
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_assert_true(sgi_gl_texture_apply_pixel(&st,
        sgi_kona_texture_resolve(&dst), coord, color, out));
    g_assert_cmpfloat(out[0], ==, .5);
    /* A luminance binding cannot reinterpret depth words as RGBA bytes. */
    g_assert_true(sgi_kona_texture_native_internal_format(&dst, 73,
        SGI_GL_LUMINANCE, SGI_GL_LUMINANCE16));
    out[0] = -1;
    g_assert_false(sgi_gl_texture_apply_pixel(&st,
        sgi_kona_texture_resolve(&dst), coord, color, out));
    g_assert_cmpfloat(out[0], ==, -1);
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
    /*
     * The same address in the other bank is another level's: the binding
     * names the first level's bank, and only that bank's image matches.
     */
    h[17] ^= 8;
    h[19] = h[18];
    h[18] = 0;
    g_assert_true(sgi_kona_texture_native_upload(&s, h, updated,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_true(sgi_kona_texture_bind(&s, 43, &p));
    assert_image(&s, pixels, 2, 2);
    g_assert_true(sgi_kona_texture_native_bank(&s, 43, true));
    g_assert_true(sgi_kona_texture_bind(&s, 43, &p));
    assert_image(&s, updated, 2, 2);
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
    stl_be_p(a->data + 332, 1);
    g_assert_false(load(a, &dst));
    stl_be_p(a->data + 332, 0);
    a->data[348] |= 0x80;
    g_assert_false(load(a, &dst));
    a->data[348] = 0;
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
    /* The level is in bank one, which the binding's bank must migrate. */
    g_assert_true(sgi_kona_texture_native_bank(&src, 99, true));
    g_assert_true(sgi_kona_texture_bind(&src, 99, NULL));
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_assert_cmpuint(sgi_kona_texture_resolve(&dst)->pyramid->count, ==, 0);
    g_assert_true(sgi_kona_texture_native_table(&dst, 99, table));
    assert_image(&dst, red, 1, 1);
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
    stl_be_p(a->data + a->len - 108, 3);
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
    uint32_t bind[5] = { 7, 0, 3 << 2, 0, 1 };
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

static void test_filter_curves(void)
{
    SGIKonaTextureState s;
    uint32_t words[33] = { 0x9ffc0101, 0x80 };
    const double *values;
    uint64_t bytes;

    sgi_kona_texture_init(&s);
    g_assert_null(sgi_kona_texture_curve(&s, 0));
    g_assert_true(sgi_kona_texture_curve_upload(&s, 0, words));
    g_assert_true(sgi_kona_texture_curve_upload(&s, 4095, words));
    values = sgi_kona_texture_curve(&s, 0);
    g_assert_nonnull(values);
    g_assert_cmpfloat(values[0], ==, 1);
    g_assert_cmpfloat(values[1], ==, -0.5);
    g_assert_cmpfloat(values[2], ==, 7.9375);
    g_assert_cmpfloat(values[3], ==, -4);
    bytes = s.bytes;
    words[0] = 0x200;
    words[1] = 0;
    g_assert_true(sgi_kona_texture_curve_upload(&s, 0, words));
    g_assert_cmpfloat(sgi_kona_texture_curve(&s, 0)[0], ==, -1);
    g_assert_cmpfloat(sgi_kona_texture_curve(&s, 4095)[0], ==, 1);
    g_assert_cmpuint(s.bytes, ==, bytes);
    /* A table that is not a spline still replaces the slot. */
    words[31] = 0x100;
    g_assert_false(sgi_kona_texture_curve_upload(&s, 4096, words));
    g_assert_cmpfloat(sgi_kona_texture_curve(&s, 0)[0], ==, -1);
    g_assert_true(sgi_kona_texture_curve_upload(&s, 0, words));
    g_assert_null(sgi_kona_texture_curve(&s, 0));
    g_assert_cmpfloat(sgi_kona_texture_curve(&s, 4095)[0], ==, 1);
    g_assert_cmpuint(s.bytes, ==, bytes);
    sgi_kona_texture_reset(&s);
    g_assert_null(sgi_kona_texture_curve(&s, 0));
    g_assert_cmpuint(s.bytes, ==, 0);
    sgi_kona_texture_cleanup(&s);
}

static void test_sharpen_curve_sampling(void)
{
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    SGIGLContext c;
    SGIGLDrawable d = {
        .bound = true, .clip = { .n = 1, .r[0] = { 0, 0, 1, 1 } },
    };
    const uint32_t pixels[] = {
        0x80808080, 0x80808080, 0x80808080, 0x80808080,
    };
    const uint32_t coarse[] = { 0x40404040 };
    uint32_t words[33] = { 0 };
    const SGIGLTexImage *im;
    GByteArray *a;
    double weight;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    sgi_gl_init(&c, 0);
    sgi_gl_set_drawable(&c, &d);
    c.cur.viewport[2] = c.cur.viewport[3] = 2;
    c.cur.texture_2d = 1;
    c.cur.texture_env_mode = SGI_GL_REPLACE;
    g_assert_true(sgi_kona_texture_upload(&src, 9, 0, 2, 2,
                                         SGI_GL_RGBA, SGI_GL_RGBA, pixels));
    g_assert_true(sgi_kona_texture_upload(&src, 9, 1, 1, 1,
                                         SGI_GL_RGBA, SGI_GL_RGBA, coarse));
    sgi_kona_texture_defaults(&p);
    p.mag_filter = SGI_GL_LINEAR_SHARPEN_SGIS;
    p.filter_curve_slot = 13;
    p.min_lod = p.max_lod = -1;
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    im = sgi_kona_texture_resolve(&src);
    g_assert_nonnull(im->read_filter_weight);
    weight = 123;
    g_assert_false(im->read_filter_weight(im->opaque, -1, &weight));
    g_assert_cmpfloat(weight, ==, 123);
    /* Constant -1 in native units becomes public sharpening weight +1. */
    for (unsigned k = 0; k < 16; k++) {
        words[2 * k] = 0x200 | (0x200 << 10) | (0x200 << 20);
        words[2 * k + 1] = 0x80;
    }
    g_assert_true(sgi_kona_texture_curve_upload(&src, 13, words));
    g_assert_cmphex(draw(&src, &c), ==, 0xc0c0c0);
    im = sgi_kona_texture_resolve(&src);
    g_assert_true(im->read_filter_weight(im->opaque, -1000, &weight));
    g_assert_cmpfloat(weight, ==, 1);
    g_assert_true(im->read_filter_weight(im->opaque, 5, &weight));
    g_assert_cmpfloat(weight, ==, 1);
    g_assert_false(im->read_filter_weight(im->opaque, NAN, &weight));
    /* Nodes zero and one are distinct; interpolate in rho, not lambda. */
    words[0] &= ~0x3ffu;
    g_assert_true(sgi_kona_texture_curve_upload(&src, 13, words));
    im = sgi_kona_texture_resolve(&src);
    g_assert_true(im->read_filter_weight(im->opaque, -7, &weight));
    g_assert_cmpfloat(weight, ==, 0.5);
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_assert_cmphex(draw(&dst, &c), ==, 0xc0c0c0);
    im = sgi_kona_texture_resolve(&dst);
    g_assert_true(im->read_filter_weight(im->opaque, -7, &weight));
    g_assert_cmpfloat(weight, ==, 0.5);
    /* The curve slot precedes the newer object fields and detail binding. */
    stl_be_p(a->data + a->len - 224, SGI_KONA_TEXTURE_CURVE_SLOTS);
    g_assert_false(load(a, &dst));
    g_assert_cmphex(draw(&dst, &c), ==, 0xc0c0c0);
    g_byte_array_unref(a);
    sgi_kona_texture_defaults(&p);
    p.filter_curve_slot = SGI_KONA_TEXTURE_CURVE_SLOTS;
    g_assert_false(sgi_kona_texture_bind(&src, 9, &p));
    sgi_gl_cleanup(&c);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_internal_format(void)
{
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [1] = 1, [2] = 1, [9] = 0x30,
        [10] = 9, [16] = 0x11, [17] = 0x2181, [18] = 23,
    };
    const uint32_t pixels[] = { 0x80604020 };
    uint32_t table[16];
    GByteArray *a;
    const SGIGLTexImage *im;
    const float coord[4] = { .5, .5, 0, 1 };
    const float color[4] = { 1, 1, 1, .75 };
    SGIGLDrawState state = { .texture_2d = true,
                            .texture_env_mode = SGI_GL_REPLACE };
    float out[4];

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[0] = 23;
    sgi_kona_texture_defaults(&p);
    p.min_filter = p.mag_filter = SGI_GL_NEAREST;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_true(sgi_kona_texture_native_table(&src, 9, table));
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    g_assert_true(sgi_kona_texture_native_internal_format(&src, 9,
                                                          SGI_GL_RGB,
                                                          SGI_GL_RGB8));
    g_assert_cmphex(texel(&src, 0, 0), ==, 0x806040ff);
    im = sgi_kona_texture_resolve(&src);
    g_assert_cmphex(im->pyramid->levels[0].internal_format, ==, SGI_GL_RGB8);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat(out[3], ==, .75);
    g_assert_false(sgi_kona_texture_native_internal_format(&src, 9,
                                                           SGI_GL_RGB,
                                                           SGI_GL_RGBA8));
    g_assert_cmphex(texel(&src, 0, 0), ==, 0x806040ff);
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_assert_cmphex(texel(&dst, 0, 0), ==, 0x806040ff);
    /* One object, no curves; new fields precede the global detail binding. */
    stl_be_p(a->data + a->len - 88, 2);
    g_assert_false(load(a, &dst));
    g_assert_cmphex(texel(&dst, 0, 0), ==, 0x806040ff);
    g_byte_array_unref(a);
    g_assert_true(sgi_kona_texture_native_internal_format(&dst, 9,
                                                          SGI_GL_RGBA,
                                                          SGI_GL_RGBA8));
    g_assert_cmphex(texel(&dst, 0, 0), ==, pixels[0]);
    im = sgi_kona_texture_resolve(&dst);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat_with_epsilon(out[3], 32.0 / 255, .000001);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_detail(void)
{
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [1] = 2, [2] = 2, [9] = 0x30,
        [10] = 77, [13] = 1, [14] = 1, [16] = 0x23, [17] = 0x2181,
        [18] = 23,
    };
    uint32_t args[6] = { 77, 1, 1, 2, 23, 0 };
    const uint32_t base[] = { 0x80808080 };
    const uint32_t detail[] = {
        0xc0408080, 0xc0408080, 0xc0408080, 0xc0408080,
    };
    uint32_t words[33] = { 0 };
    const SGIGLTexImage *im;
    SGIGLDrawState state = { .texture_2d = true,
                            .texture_env_mode = SGI_GL_REPLACE };
    const float coord[4] = { .5, .5, 0, 1 };
    const float color[4] = { 1, 1, 1, 1 };
    float out[4];
    GByteArray *a;
    double weight;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    sgi_kona_texture_defaults(&p);
    p.min_filter = SGI_GL_LINEAR;
    p.mag_filter = SGI_GL_LINEAR_DETAIL_SGIS;
    p.detail_mode = SGI_GL_ADD;
    p.detail_level = -1;
    p.min_lod = p.max_lod = -1;
    p.filter_curve_slot = 13;
    g_assert_true(sgi_kona_texture_upload(&src, 9, 0, 1, 1,
                                         SGI_GL_RGBA, SGI_GL_RGBA, base));
    g_assert_true(sgi_kona_texture_native_upload(&src, h, detail,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    for (unsigned k = 0; k < 16; k++) {
        words[2 * k] = 0x100 | (0x100 << 10) | (0x100 << 20);
        words[2 * k + 1] = 0x40;
    }
    g_assert_true(sgi_kona_texture_curve_upload(&src, 13, words));
    g_assert_true(sgi_kona_texture_detail_bind(&src, args));
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    im = sgi_kona_texture_resolve(&src);
    g_assert_nonnull(im->detail_image);
    g_assert_true(im->read_filter_weight(im->opaque, -1, &weight));
    g_assert_cmpfloat(weight, ==, .25);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat_with_epsilon(out[0], 160.25 / 255, .000001);
    g_assert_cmpfloat_with_epsilon(out[1], 96.25 / 255, .000001);
    a = save(&src);
    g_assert_true(load(a, &dst));
    im = sgi_kona_texture_resolve(&dst);
    g_assert_cmpint(im->detail_level, ==, -1);
    g_assert_cmpuint(im->detail_mode, ==, SGI_GL_ADD);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat_with_epsilon(out[0], 160.25 / 255, .000001);
    stl_be_p(a->data + a->len - 28, 2);
    g_assert_false(load(a, &dst));
    g_assert_nonnull(sgi_kona_texture_resolve(&dst)->detail_image);
    g_byte_array_unref(a);
    a = save(&src);
    stl_be_p(a->data + a->len - 12, 63);
    g_assert_false(load(a, &dst));
    g_byte_array_unref(a);
    p.detail_mode = 0;
    g_assert_false(sgi_kona_texture_bind(&src, 9, &p));
    p.detail_mode = SGI_GL_MODULATE;
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    im = sgi_kona_texture_resolve(&src);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat_with_epsilon(out[0],
                                  (128.0 / 255) * (1 + .25 * (129.0 / 255)),
                                  .000001);
    p.detail_level = 1;
    g_assert_false(sgi_kona_texture_bind(&src, 9, &p));
    p.detail_level = -1;
    /* Invalid selector metadata cannot replace a usable binding. */
    args[1] = 32;
    g_assert_false(sgi_kona_texture_detail_bind(&src, args));
    args[1] = 1;
    args[3] = 63;
    g_assert_false(sgi_kona_texture_detail_bind(&src, args));
    args[3] = 2;
    for (unsigned mismatch = 0; mismatch < 3; mismatch++) {
        args[0] = mismatch == 0 ? 78 : 77;
        args[1] = mismatch == 1 ? 0 : 1;
        args[4] = mismatch == 2 ? 24 : 23;
        g_assert_true(sgi_kona_texture_detail_bind(&src, args));
        im = sgi_kona_texture_resolve(&src);
        out[0] = .123f;
        g_assert_false(sgi_gl_texture_apply_pixel(&state, im,
                                                 coord, color, out));
        g_assert_cmpfloat(out[0], ==, .123f);
    }
    args[0] = 77;
    args[1] = 1;
    args[4] = 23;
    /* Shared storage does not make RGB and RGBA internal formats identical. */
    args[3] = 38;
    g_assert_true(sgi_kona_texture_detail_bind(&src, args));
    im = sgi_kona_texture_resolve(&src);
    g_assert_cmphex(im->detail_image->internal_format, ==, SGI_GL_RGB);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat_with_epsilon(out[0], 128.0 / 255, .000001);
    args[3] = 2;
    g_assert_true(sgi_kona_texture_detail_bind(&src, args));
    /* A second physical upload with the same address pair is ambiguous. */
    h[16]++;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, detail,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    im = sgi_kona_texture_resolve(&src);
    g_assert_false(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    args[0] = UINT32_MAX;
    g_assert_true(sgi_kona_texture_detail_bind(&src, args));
    im = sgi_kona_texture_resolve(&src);
    g_assert_null(im->detail_image);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat_with_epsilon(out[0], 128.0 / 255, .000001);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void native_volume_setup(SGIKonaTextureState *s,
                                 SGIKonaTextureParams *p)
{
    uint32_t table[16];

    sgi_kona_texture_init(s);
    sgi_kona_texture_defaults(p);
    p->min_filter = p->mag_filter = SGI_GL_NEAREST;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[0] = 23;
    table[1] = 47;
    g_assert_true(sgi_kona_texture_native_table(s, 9, table));
    g_assert_true(sgi_kona_texture_native_volume_dimensions(s, 9, 2, 2, 2));
    g_assert_true(sgi_kona_texture_bind(s, 9, p));
}

static uint32_t native_voxel(SGIKonaTextureState *s, unsigned level,
                              unsigned x, unsigned y, unsigned z)
{
    const SGIGLTexImage *im = sgi_kona_texture_resolve(s);
    const SGIGLTexLevel *l = &im->pyramid->levels[level];
    uint32_t rgba;

    g_assert_true(im->volume);
    g_assert_nonnull(l->read_texel_3d);
    g_assert_true(l->read_texel_3d(l->opaque, l->level, x, y, z, &rgba));
    return rgba;
}

static void test_native_volume_slices(void)
{
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_3D, [1] = 1, [2] = 2, [9] = 0x30,
        [10] = 9, [11] = 1, [13] = 1, [14] = 1, [15] = 1,
        [16] = 0x333, [17] = 0x40, [18] = 23, [19] = 47,
    };
    const uint32_t right[] = { 0x00ff00ff, 0x0000ffff };
    const uint32_t left[] = { 0xffffffff, 0x808080ff };
    const uint32_t plane[] = { 0xff0000ff, 0xff0000ff,
                               0xff0000ff, 0xff0000ff };
    GByteArray *a;
    uint64_t bytes;

    native_volume_setup(&src, &p);
    sgi_kona_texture_init(&dst);
    /* A high slice arriving first retains holes in all earlier slices. */
    g_assert_true(sgi_kona_texture_native_upload(&src, h, right,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_cmpuint(sgi_kona_texture_resolve(&src)->pyramid->count, ==, 0);
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_assert_cmpuint(dst.bytes, ==, src.bytes);
    g_assert_cmpuint(sgi_kona_texture_resolve(&dst)->pyramid->count, ==, 0);
    /* A nonzero uncovered voxel cannot be restored from a stream. */
    stl_be_p(a->data + 332, 1);
    g_assert_false(load(a, &dst));
    g_byte_array_unref(a);
    h[1] = 2;
    h[11] = 0;
    h[15] = 0;
    g_assert_true(sgi_kona_texture_native_upload(&dst, h, plane,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    h[1] = 1;
    h[13] = 0;
    h[15] = 1;
    g_assert_true(sgi_kona_texture_native_upload(&dst, h, left,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_cmpuint(sgi_kona_texture_resolve(&dst)->pyramid->count, ==, 1);
    for (unsigned y = 0; y < 2; y++) {
        for (unsigned x = 0; x < 2; x++) {
            g_assert_cmphex(native_voxel(&dst, 0, x, y, 0), ==, plane[0]);
            g_assert_cmphex(native_voxel(&dst, 0, x, y, 1), ==,
                            x ? right[y] : left[y]);
        }
    }
    bytes = dst.bytes;
    h[2] = 1;
    h[14] = 0;
    g_assert_true(sgi_kona_texture_native_upload(&dst, h, red,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_cmpuint(dst.bytes, ==, bytes);
    g_assert_cmphex(native_voxel(&dst, 0, 0, 0, 1), ==, red[0]);
    g_assert_cmphex(native_voxel(&dst, 0, 0, 1, 1), ==, left[1]);
    g_assert_cmphex(native_voxel(&dst, 0, 1, 0, 1), ==, right[0]);
    a = save(&dst);
    g_assert_true(load(a, &src));
    g_assert_cmphex(native_voxel(&src, 0, 0, 0, 1), ==, red[0]);
    stl_be_p(a->data + 328, UINT32_MAX);
    g_assert_false(load(a, &src));
    g_assert_cmphex(native_voxel(&src, 0, 0, 0, 1), ==, red[0]);
    g_byte_array_unref(a);
    a = save(&dst);
    stl_be_p(a->data + 292, 2);
    g_assert_false(load(a, &src));
    g_byte_array_unref(a);
    a = save(&dst);
    stl_be_p(a->data + a->len - 68, 3);
    g_assert_false(load(a, &src));
    g_byte_array_unref(a);
    a = save(&dst);
    stl_be_p(a->data + a->len - 64, 0);
    g_assert_false(load(a, &src));
    g_byte_array_unref(a);
    g_assert_false(sgi_kona_texture_native_volume_dimensions(&src, 9, 2, 2, 3));
    g_assert_false(sgi_kona_texture_native_volume_dimensions(&src, 9,
                                                            4096, 4096, 2));
    h[15] = UINT32_MAX;
    g_assert_false(sgi_kona_texture_native_upload(&src, h, red,
                                                 SGI_GL_RGBA, SGI_GL_RGBA));
    h[15] = 2;
    g_assert_false(sgi_kona_texture_native_upload(&src, h, red,
                                                 SGI_GL_RGBA, SGI_GL_RGBA));
    h[15] = 0;
    /* Layout mode three; a volume loads in mode one or two. */
    h[17] = 0x60;
    g_assert_false(sgi_kona_texture_native_upload(&src, h, red,
                                                 SGI_GL_RGBA, SGI_GL_RGBA));
    h[17] = 0x40;
    bytes = src.bytes;
    h[16]++;
    src.bytes = SGI_KONA_TEXTURE_BYTES;
    g_assert_false(sgi_kona_texture_native_upload(&src, h, red,
                                                 SGI_GL_RGBA, SGI_GL_RGBA));
    src.bytes = bytes;
    sgi_kona_texture_reset(&src);
    g_assert_cmpuint(src.bytes, ==, 0);
    g_assert_null(sgi_kona_texture_resolve(&src));
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

typedef struct VolumeLinePixels {
    unsigned count;
    uint32_t last;
} VolumeLinePixels;

static void volume_line_store(void *opaque, SGIColourTarget target,
                               int x, int y, uint32_t colour)
{
    VolumeLinePixels *pixels = opaque;

    pixels->count++;
    pixels->last = colour;
}

static void native_volume_r_derivatives(SGIKonaTextureState *s,
                                         SGIKonaTextureParams *p)
{
    SGIGLContext c;
    SGIGLDrawable drawable = {
        .bound = true, .clip = { .n = 1, .r[0] = { 0, 0, 4, 4 } },
    };
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8, .depth_bits = 24,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .line_rule = SGI_LINE_COVERAGE_DOUBLE,
        .evidence = "unit test",
    };
    float vertices[2][SGI_GL_VERTEX_FLOATS] = { 0 };
    VolumeLinePixels pixels;
    SGIRasterTarget target = { .store_colour = volume_line_store,
                              .opaque = &pixels };
    SGIGLDraw draw = { .mode = SGI_GL_LINES, .count = 2,
                      .vertices = &vertices[0][0] };

    sgi_gl_init(&c, 0);
    sgi_gl_set_drawable(&c, &drawable);
    c.cur.viewport[2] = c.cur.viewport[3] = 4;
    c.cur.texture_3d = 1;
    c.cur.texture_env_mode = SGI_GL_REPLACE;
    draw.state = &c.cur;
    for (unsigned i = 0; i < 2; i++) {
        vertices[i][0] = i ? .5 : -.5;
        vertices[i][3] = 1;
        vertices[i][4] = vertices[i][5] = vertices[i][6] = vertices[i][7] = 1;
        vertices[i][SGI_GL_V_TEXCOORD] = .5;
        vertices[i][SGI_GL_V_TEXCOORD + 1] = .5;
        vertices[i][SGI_GL_V_TEXCOORD + 2] = i;
        vertices[i][SGI_GL_V_TEXCOORD + 3] = 1;
    }
    p->min_filter = SGI_GL_NEAREST_MIPMAP_NEAREST;
    p->mag_filter = SGI_GL_NEAREST;
    p->min_lod = -1000;
    p->max_lod = 1000;
    for (unsigned strong = 0; strong < 2; strong++) {
        p->lod_scale_r = strong ? 4 : 1;
        g_assert_true(sgi_kona_texture_bind(s, 9, p));
        target.texture = sgi_kona_texture_resolve(s);
        pixels = (VolumeLinePixels) { 0 };
        sgi_soft_draw(&rules, &target, &draw);
        g_assert_cmpuint(pixels.count, >, 0);
        if (strong) {
            g_assert_cmphex(pixels.last, ==, 0x0000ff);
        } else {
            g_assert_cmphex(pixels.last, !=, 0x0000ff);
        }
    }
    sgi_gl_cleanup(&c);
}

static void test_native_volume_sampling(void)
{
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_3D, [1] = 2, [2] = 2, [9] = 0x30,
        [10] = 9, [13] = 1, [14] = 1,
        [16] = 0x333, [17] = 0x40, [18] = 23, [19] = 47,
    };
    const uint32_t red_plane[] = { 0xff0000ff, 0xff0000ff,
                                   0xff0000ff, 0xff0000ff };
    const uint32_t green_plane[] = { 0x00ff00ff, 0x00ff00ff,
                                     0x00ff00ff, 0x00ff00ff };
    const uint32_t blue[] = { 0x0000ffff };
    SGIGLDrawState state = { .texture_3d = true,
                            .texture_env_mode = SGI_GL_REPLACE };
    const float color[4] = { 1, 1, 1, 1 };
    float coord[4] = { .5, .5, .25, 1 }, out[4];
    const SGIGLTexImage *im;
    GByteArray *a;

    native_volume_setup(&src, &p);
    sgi_kona_texture_init(&dst);
    g_assert_true(sgi_kona_texture_native_upload(&src, h, red_plane,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    h[15] = 1;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, green_plane,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    /* Identical tile exponents do not identify the logical mipmap level. */
    h[1] = h[2] = 1;
    h[13] = h[14] = h[15] = 0;
    h[17] |= 8;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, blue,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    im = sgi_kona_texture_resolve(&src);
    g_assert_cmpuint(im->pyramid->count, ==, 2);
    g_assert_cmpuint(im->pyramid->levels[0].depth, ==, 2);
    g_assert_cmpuint(im->pyramid->levels[1].depth, ==, 1);
    g_assert_cmphex(native_voxel(&src, 1, 0, 0, 0), ==, blue[0]);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat(out[0], ==, 1);
    coord[2] = 2.75;
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat(out[1], ==, 1);
    p.min_filter = p.mag_filter = SGI_GL_LINEAR;
    p.wrap_r = SGI_GL_CLAMP_TO_EDGE;
    p.lod_scale_r = 2;
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    im = sgi_kona_texture_resolve(&src);
    g_assert_cmpfloat(im->lod_scale_r, ==, 2);
    coord[2] = -.5;
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat(out[0], ==, 1);
    coord[2] = .5;
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat(out[0], ==, .5);
    g_assert_cmpfloat(out[1], ==, .5);
    p.min_filter = SGI_GL_NEAREST_MIPMAP_NEAREST;
    p.min_lod = p.max_lod = 1;
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    im = sgi_kona_texture_resolve(&src);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat(out[2], ==, 1);
    a = save(&src);
    g_assert_true(load(a, &dst));
    im = sgi_kona_texture_resolve(&dst);
    g_assert_cmpfloat(im->lod_scale_r, ==, 2);
    g_assert_cmpuint(im->wrap_r, ==, SGI_GL_CLAMP_TO_EDGE);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat(out[2], ==, 1);
    g_byte_array_unref(a);
    native_volume_r_derivatives(&src, &p);
    p.lod_scale_r = NAN;
    g_assert_false(sgi_kona_texture_bind(&src, 9, &p));
    /* A second key matching the same pair/parity/dimensions refuses drawing. */
    p.min_filter = p.mag_filter = SGI_GL_NEAREST;
    p.lod_scale_r = 1;
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    h[16]++;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, blue,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    im = sgi_kona_texture_resolve(&src);
    out[0] = .123f;
    g_assert_false(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat(out[0], ==, .123f);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_volume_interior(gconstpointer data)
{
    unsigned level = GPOINTER_TO_UINT(data) >> 24;
    uint32_t width = (GPOINTER_TO_UINT(data) >> 16) & 255;
    uint32_t height = (GPOINTER_TO_UINT(data) >> 8) & 255;
    uint32_t depth = GPOINTER_TO_UINT(data) & 255;
    unsigned ps = MAX(ctz32(width) + 1, 3);
    unsigned pt = MAX(ctz32(height) + 1, 1);
    unsigned pr = MAX(ctz32(depth) + 1, 1);
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_3D, [9] = 0x30, [10] = 9,
        [17] = 0x40, [18] = 23, [19] = 47,
    };
    uint32_t table[16], pixels[64], rgba;
    const SGIGLTexImage *im;
    const SGIGLTexLevel *view;
    SGIKonaTextureObject *o;
    SGIKonaTextureNativeImage *n;
    uint64_t at;
    GByteArray *a;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    sgi_kona_texture_defaults(&p);
    p.min_filter = p.mag_filter = SGI_GL_NEAREST;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[level & ~1u] = 23;
    table[(level & ~1u) + 1] = 47;
    g_assert_true(sgi_kona_texture_native_table(&src, 9, table));
    g_assert_true(sgi_kona_texture_native_volume_dimensions(&src, 9,
        width << level, height << level, depth << level));
    g_assert_false(sgi_kona_texture_native_layout(&src, 9, 0x1000));
    g_assert_false(sgi_kona_texture_native_layout(&src, 9, 0xe00));
    g_assert_true(sgi_kona_texture_native_layout(&src, 9,
        (ps + level) | ((pt + level) << 4) | ((pr + level) << 8)));
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    h[1] = width + 1;
    h[2] = height + 1;
    h[13] = width;
    h[14] = height;
    h[16] = ps | (pt << 4) | (pr << 8);
    h[17] |= (level & 1) << 3;
    for (unsigned z = 0; z <= depth; z++) {
        for (unsigned y = 0; y <= height; y++) {
            for (unsigned x = 0; x <= width; x++) {
                pixels[y * (width + 1) + x] =
                    x == width || y == height || z == depth ? 0x0000ffff :
                    z ? 0x00ff00ff : 0xff0000ff;
            }
        }
        h[15] = z;
        g_assert_true(sgi_kona_texture_native_upload(&src, h, pixels,
                                                   SGI_GL_RGBA, SGI_GL_RGBA));
    }
    im = sgi_kona_texture_resolve(&src);
    g_assert_cmpuint(im->pyramid->count, ==, 1);
    view = &im->pyramid->levels[0];
    g_assert_cmpuint(view->level, ==, level);
    g_assert_cmpuint(view->width, ==, width);
    g_assert_cmpuint(view->height, ==, height);
    g_assert_cmpuint(view->depth, ==, depth);
    for (unsigned z = 0; z < depth; z++) {
        g_assert_true(view->read_texel_3d(view->opaque, level, width - 1,
                                         height - 1, z, &rgba));
        g_assert_cmphex(rgba, ==, z ? 0x00ff00ff : 0xff0000ff);
    }
    g_assert_false(view->read_texel_3d(view->opaque, level, 0, 0, depth,
                                      &rgba));
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_byte_array_unref(a);
    im = sgi_kona_texture_resolve(&dst);
    view = &im->pyramid->levels[0];
    g_assert_true(view->read_texel_3d(view->opaque, level, width - 1,
                                     height - 1, depth - 1, &rgba));
    g_assert_cmphex(rgba, ==, depth > 1 ? 0x00ff00ff : 0xff0000ff);
    a = save(&src);
    stl_be_p(a->data, 11);
    g_assert_false(load(a, &dst));
    g_byte_array_unref(a);
    a = save(&src);
    stl_be_p(a->data + a->len - 48, 0x1000);
    g_assert_false(load(a, &dst));
    g_byte_array_unref(a);
    o = g_hash_table_lookup(src.objects, GUINT_TO_POINTER(9));
    n = &o->native[0];
    at = ((uint64_t)(depth - 1) * n->image.height + height - 1) *
         n->image.width + width - 1;
    n->coverage[at / 8] &= ~(1u << (at % 8));
    n->covered--;
    g_assert_cmpuint(sgi_kona_texture_resolve(&src)->pyramid->count, ==, 0);
    n->coverage[at / 8] |= 1u << (at % 8);
    n->covered++;
    /* Other exponent planes cannot satisfy the emitted interior geometry. */
    h[16]++;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_cmpuint(sgi_kona_texture_resolve(&src)->pyramid->count, ==, 1);
    p.base_level = 1;
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    g_assert_cmpuint(sgi_kona_texture_resolve(&src)->pyramid->count, ==, 0);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_volume_border(gconstpointer data)
{
    unsigned level = GPOINTER_TO_UINT(data) >> 24;
    uint32_t width = (GPOINTER_TO_UINT(data) >> 16) & 255;
    uint32_t height = (GPOINTER_TO_UINT(data) >> 8) & 255;
    uint32_t depth = GPOINTER_TO_UINT(data) & 255;
    unsigned ps = ctz32(width), pt = ctz32(height), pr = ctz32(depth);
    unsigned bs = MIN(ps + 1, 3), bt = MIN(pt + 1, 1);
    uint32_t nx = (1u << bs) - 1, ny = (1u << bt) - 1;
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_3D, [9] = 0x30, [10] = 9,
        [17] = 0x1141, [18] = 23, [19] = 47,
    };
    uint32_t table[16], pixels[32], rgba;
    SGIGLDrawState state = { .texture_3d = true,
                            .texture_env_mode = SGI_GL_REPLACE };
    const float color[4] = { 1, 1, 1, 1 };
    float coord[4] = { 0, .5, .5, 1 }, out[4];
    const SGIGLTexImage *im;
    const SGIGLTexLevel *view;
    SGIKonaTextureObject *o;
    SGIKonaTextureNativeImage *n;
    uint64_t at;
    GByteArray *a;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    sgi_kona_texture_defaults(&p);
    p.min_filter = p.mag_filter = SGI_GL_LINEAR;
    p.wrap_s = p.wrap_t = SGI_GL_CLAMP;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[level & ~1u] = 23;
    table[(level & ~1u) + 1] = 47;
    g_assert_true(sgi_kona_texture_native_table(&src, 9, table));
    g_assert_true(sgi_kona_texture_native_volume_dimensions(&src, 9,
        width << level, height << level, depth << level));
    g_assert_true(sgi_kona_texture_native_layout(&src, 9,
        MAX(ps + level + 1, 3) | (MAX(pt + level + 1, 1) << 4) |
        (MAX(pr + level + 1, 1) << 8)));
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    h[1] = width;
    h[2] = height;
    h[13] = width - 1;
    h[14] = height - 1;
    h[16] = MAX(ps + 1, 3) | (MAX(pt + 1, 1) << 4) |
            (MAX(pr + 1, 1) << 8);
    h[17] |= (level & 1) << 3;
    for (unsigned k = 0; k < width * height; k++) {
        pixels[k] = 0xff0000ff;
    }
    for (unsigned z = 0; z < depth; z++) {
        h[15] = z;
        g_assert_true(sgi_kona_texture_native_upload(&src, h, pixels,
                                                    SGI_GL_RGBA, SGI_GL_RGBA));
    }
    im = sgi_kona_texture_resolve(&src);
    g_assert_cmpuint(im->pyramid->count, ==, 1);
    view = &im->pyramid->levels[0];
    g_assert_cmpuint(view->border, ==, 1);
    g_assert_nonnull(view->read_border_texel_3d);
    g_assert_false(view->read_border_texel_3d(view->opaque, level,
                                             -1, 0, 0, &rgba));
    if (!level) {
        out[0] = .123f;
        g_assert_false(sgi_gl_texture_apply_pixel(&state, im, coord,
                                                 color, out));
        g_assert_cmpfloat(out[0], ==, .123f);
    }
    h[1] = h[2] = 1;
    h[16] = bs | (bt << 4) | (MIN(pr + 1, 1) << 8);
    for (unsigned z = 0; z < depth; z++) {
        uint32_t border = z ? 0x00ff00ff : 0x0000ffff;

        h[15] = z;
        for (int32_t y = -1; y <= (int32_t)height; y++) {
            for (int32_t x = -1; x <= (int32_t)width; x++) {
                if (x >= 0 && x < width && y >= 0 && y < height) {
                    continue;
                }
                h[11] = h[13] = x < 0 ? nx : x;
                h[12] = h[14] = y < 0 ? ny : y;
                g_assert_true(sgi_kona_texture_native_upload(&src, h, &border,
                                                   SGI_GL_RGBA, SGI_GL_RGBA));
            }
        }
    }
    im = sgi_kona_texture_resolve(&src);
    view = &im->pyramid->levels[0];
    for (unsigned z = 0; z < depth; z++) {
        g_assert_true(view->read_border_texel_3d(view->opaque, level,
                                                 -1, -1, z, &rgba));
        g_assert_cmphex(rgba, ==, z ? 0x00ff00ff : 0x0000ffff);
        g_assert_true(view->read_border_texel_3d(view->opaque, level,
                                                 width, height, z, &rgba));
        g_assert_cmphex(rgba, ==, z ? 0x00ff00ff : 0x0000ffff);
    }
    g_assert_false(view->read_border_texel_3d(view->opaque, level,
                                             -2, 0, 0, &rgba));
    g_assert_false(view->read_border_texel_3d(view->opaque, level,
                                             0, 0, 0, &rgba));
    g_assert_false(view->read_border_texel_3d(view->opaque, level,
                                             -1, -1, -1, &rgba));
    g_assert_false(view->read_border_texel_3d(view->opaque, level,
                                             width, height, depth, &rgba));
    if (!level) {
        g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord,
                                                 color, out));
        g_assert_cmpfloat(out[0], ==, .5);
        g_assert_cmpfloat(out[1], ==, depth > 1 ? .25 : 0);
        g_assert_cmpfloat(out[2], ==, depth > 1 ? .25 : .5);
        coord[1] = 0;
        g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord,
                                                 color, out));
        g_assert_cmpfloat(out[0], ==, .25);
        g_assert_cmpfloat(out[1], ==, depth > 1 ? .375 : 0);
        g_assert_cmpfloat(out[2], ==, depth > 1 ? .375 : .75);
        p.wrap_r = SGI_GL_CLAMP_TO_EDGE;
        g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
        coord[2] = -1;
        im = sgi_kona_texture_resolve(&src);
        g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord,
                                                 color, out));
        g_assert_cmpfloat(out[0], ==, .25);
        g_assert_cmpfloat(out[2], ==, .75);
        p.wrap_s = SGI_GL_CLAMP_TO_BORDER;
        g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
        coord[0] = -1;
        im = sgi_kona_texture_resolve(&src);
        g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord,
                                                 color, out));
        g_assert_cmpfloat(out[2], ==, 1);
        p.wrap_s = p.wrap_t = SGI_GL_REPEAT;
        p.wrap_r = SGI_GL_CLAMP_TO_BORDER;
        g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
        im = sgi_kona_texture_resolve(&src);
        coord[0] = coord[1] = .5;
        coord[2] = 2;
        out[0] = .123f;
        g_assert_false(sgi_gl_texture_apply_pixel(&state, im, coord,
                                                 color, out));
        g_assert_cmpfloat(out[0], ==, .123f);
    }
    p.wrap_s = p.wrap_t = SGI_GL_CLAMP;
    p.wrap_r = SGI_GL_REPEAT;
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_byte_array_unref(a);
    im = sgi_kona_texture_resolve(&dst);
    view = &im->pyramid->levels[0];
    g_assert_true(view->read_border_texel_3d(view->opaque, level,
                                             -1, -1, depth - 1, &rgba));
    g_assert_cmphex(rgba, ==, depth > 1 ? 0x00ff00ff : 0x0000ffff);
    o = g_hash_table_lookup(dst.objects, GUINT_TO_POINTER(9));
    n = &o->native[o->native_count - 1];
    at = ((uint64_t)(depth - 1) * n->image.height + ny) * n->image.width + nx;
    n->coverage[at / 8] &= ~(1u << (at % 8));
    n->covered--;
    g_assert_false(view->read_border_texel_3d(view->opaque, level,
                                              -1, -1, depth - 1, &rgba));
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_mixed_transport(void)
{
    for (unsigned volume = 0; volume < 2; volume++) {
        SGIKonaTextureState src, dst;
        SGIKonaTextureParams p;
        uint32_t h[20] = {
            [0] = volume ? SGI_GL_TEXTURE_3D : SGI_GL_TEXTURE_2D,
            [1] = 1, [2] = 1, [9] = 0x30, [10] = 9,
            [16] = 0x333, [17] = volume ? 0x40 : 0,
            [18] = 23, [19] = volume ? 47 : 0,
        };
        const uint32_t blue[] = { 0x0000ffff };
        const uint32_t white[] = { 0xffffffff };
        uint32_t table[16];
        SGIKonaTextureObject *o;
        GByteArray *a;
        SGIGLDrawState state = {
            .texture_2d = !volume, .texture_3d = volume,
            .texture_env_mode = SGI_GL_REPLACE,
        };
        const float coord[4] = { .25, .5, .25, 1 };
        const float color[4] = { 1, 1, 1, 1 };
        float out[4];

        sgi_kona_texture_init(&src);
        sgi_kona_texture_init(&dst);
        sgi_kona_texture_defaults(&p);
        p.min_filter = p.mag_filter = SGI_GL_NEAREST;
        for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
            table[k] = UINT32_MAX;
        }
        table[0] = 23;
        table[1] = volume ? 47 : UINT32_MAX;
        g_assert_true(sgi_kona_texture_native_table(&src, 9, table));
        if (volume) {
            g_assert_true(sgi_kona_texture_native_volume_dimensions(&src, 9,
                                                                    2, 1, 2));
        } else {
            g_assert_true(sgi_kona_texture_native_dimensions(&src, 9, 2, 1));
        }
        g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
        g_assert_true(sgi_kona_texture_native_upload(&src, h, red,
                                                    SGI_GL_RGBA, SGI_GL_RGBA));
        h[6] = 1;
        h[17] |= 4;
        h[11] = h[13] = 1;
        g_assert_true(sgi_kona_texture_native_upload(&src, h, green,
                                                    SGI_GL_RGBA, SGI_GL_RGBA));
        if (volume) {
            h[15] = 1;
            h[11] = h[13] = 0;
            h[6] = 0;
            h[17] &= ~4u;
            g_assert_true(sgi_kona_texture_native_upload(&src, h, blue,
                                                        SGI_GL_RGBA,
                                                        SGI_GL_RGBA));
            h[11] = h[13] = 1;
            h[6] = 1;
            h[17] |= 4;
            g_assert_true(sgi_kona_texture_native_upload(&src, h, white,
                                                        SGI_GL_RGBA,
                                                        SGI_GL_RGBA));
            g_assert_cmphex(native_voxel(&src, 0, 0, 0, 1), ==, blue[0]);
            g_assert_cmphex(native_voxel(&src, 0, 1, 0, 1), ==, white[0]);
        }
        o = g_hash_table_lookup(src.objects, GUINT_TO_POINTER(9));
        g_assert_cmpuint(o->native_count, ==, 1);
        g_assert_cmpuint(sgi_kona_texture_resolve(&src)->pyramid->count, ==, 1);
        g_assert_cmphex(texel(&src, 0, 0), ==, red[0]);
        g_assert_cmphex(texel(&src, 0, 1), ==, green[0]);
        a = save(&src);
        g_assert_true(load(a, &dst));
        g_assert_cmphex(texel(&dst, 0, 0), ==, red[0]);
        g_assert_cmphex(texel(&dst, 0, 1), ==, green[0]);
        g_byte_array_unref(a);
        /* A DMA patch must replace the complete PIO image's visible texel. */
        h[15] = h[11] = h[13] = 0;
        h[6] = 0;
        h[17] &= ~4u;
        g_assert_true(sgi_kona_texture_native_upload(&dst, h, red,
                                                    SGI_GL_RGBA, SGI_GL_RGBA));
        h[6] = 1;
        h[17] |= 4;
        g_assert_true(sgi_kona_texture_native_upload(&dst, h, blue,
                                                    SGI_GL_RGBA, SGI_GL_RGBA));
        g_assert_true(sgi_gl_texture_apply_pixel(&state,
                                                sgi_kona_texture_resolve(&dst),
                                                coord, color, out));
        g_assert_cmpfloat(out[0], ==, 0);
        g_assert_cmpfloat(out[2], ==, 1);
        g_assert_cmphex(texel(&dst, 0, 1), ==, green[0]);
        /* Physical geometry and level parity still distinguish image keys. */
        h[16]++;
        g_assert_true(sgi_kona_texture_native_upload(&dst, h, green,
                                                    SGI_GL_RGBA, SGI_GL_RGBA));
        h[16]--;
        h[17] ^= 8;
        g_assert_true(sgi_kona_texture_native_upload(&dst, h, white,
                                                    SGI_GL_RGBA, SGI_GL_RGBA));
        o = g_hash_table_lookup(dst.objects, GUINT_TO_POINTER(9));
        g_assert_cmpuint(o->native_count, ==, 3);
        sgi_kona_texture_cleanup(&src);
        sgi_kona_texture_cleanup(&dst);
    }
}

static void test_native_duplicate_transport_stream(void)
{
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [1] = 1, [2] = 1, [9] = 0x30,
        [10] = 9, [16] = 0x333, [18] = 23,
    };
    uint32_t table[16];
    GByteArray *a;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    sgi_kona_texture_defaults(&p);
    p.min_filter = p.mag_filter = SGI_GL_NEAREST;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[0] = 23;
    g_assert_true(sgi_kona_texture_native_table(&src, 9, table));
    g_assert_true(sgi_kona_texture_native_dimensions(&src, 9, 1, 1));
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    g_assert_true(sgi_kona_texture_native_upload(&src, h, red,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_byte_array_unref(a);
    h[16]++;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, green,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    a = save(&src);
    /* First header232, then header80 + metadata20 + pixel4 + coverage1. */
    stl_be_p(a->data + 337 + 16 * 4, 0x333);
    stl_be_p(a->data + 337 + 17 * 4, 4);
    g_assert_false(load(a, &dst));
    g_assert_cmphex(texel(&dst, 0, 0), ==, red[0]);
    g_byte_array_unref(a);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_one_dimensional(void)
{
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_1D, [1] = 2, [2] = 1, [9] = 0x30,
        [10] = 9, [13] = 1, [16] = 0x23, [18] = 23,
    };
    uint32_t table[16];
    const uint32_t pixels[] = { 0xff0000ff, 0x00ff00ff };
    const SGIGLTexImage *im;
    GByteArray *a;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    sgi_kona_texture_defaults(&p);
    p.min_filter = p.mag_filter = SGI_GL_NEAREST;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[0] = 23;
    table[1] = 47;
    g_assert_false(sgi_kona_texture_native_dimensions_1d(&src, 9, 3));
    g_assert_true(sgi_kona_texture_native_dimensions_1d(&src, 9, 2));
    g_assert_true(sgi_kona_texture_native_layout(&src, 9, 0x23));
    g_assert_true(sgi_kona_texture_native_table(&src, 9, table));
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    h[2] = 2;
    h[14] = 1;
    g_assert_false(sgi_kona_texture_native_upload(&src, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    h[2] = 1;
    h[12] = h[14] = 1;
    g_assert_false(sgi_kona_texture_native_upload(&src, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    h[12] = h[14] = 0;
    h[15] = 1;
    g_assert_false(sgi_kona_texture_native_upload(&src, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    h[15] = 0;
    /* Identical descriptors on a 2D image cannot satisfy a 1D binding. */
    h[0] = SGI_GL_TEXTURE_2D;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_cmpuint(sgi_kona_texture_resolve(&src)->pyramid->count, ==, 0);
    h[0] = SGI_GL_TEXTURE_1D;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    h[17] = 4;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    h[1] = 1;
    h[13] = 0;
    h[17] = 8;
    h[18] = 0;
    h[19] = 47;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, pixels + 1,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_true(sgi_kona_texture_native_dimensions_1d(&src, 9, 2));
    g_assert_cmpuint(sgi_kona_texture_resolve(&src)->pyramid->count, ==, 0);
    g_assert_true(sgi_kona_texture_native_layout(&src, 9, 0x25));
    g_assert_cmpuint(sgi_kona_texture_resolve(&src)->pyramid->count, ==, 0);
    g_assert_true(sgi_kona_texture_native_layout(&src, 9, 0x23));
    im = sgi_kona_texture_resolve(&src);
    g_assert_true(im->one_dimensional);
    g_assert_false(im->volume);
    g_assert_cmpuint(im->pyramid->count, ==, 2);
    g_assert_cmpuint(im->pyramid->levels[0].height, ==, 1);
    g_assert_cmpuint(im->pyramid->levels[1].width, ==, 1);
    g_assert_cmphex(texel(&src, 1, 0), ==, pixels[1]);
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_assert_true(sgi_kona_texture_resolve(&dst)->one_dimensional);
    g_assert_cmphex(texel(&dst, 1, 0), ==, pixels[1]);
    a = save(&src);
    /* A 1D binding cannot carry a volume depth. */
    stl_be_p(a->data + a->len - 68, 1);
    g_assert_false(load(a, &dst));
    g_byte_array_unref(a);
    g_assert_true(sgi_kona_texture_native_dimensions(&dst, 9, 2, 1));
    g_assert_false(sgi_kona_texture_resolve(&dst)->one_dimensional);
    g_assert_true(sgi_kona_texture_native_dimensions_1d(&dst, 9, 2));
    g_assert_true(sgi_kona_texture_native_volume_dimensions(&dst, 9, 2, 1, 1));
    g_assert_false(sgi_kona_texture_resolve(&dst)->one_dimensional);
    sgi_kona_texture_cleanup(&dst);
    sgi_kona_texture_init(&dst);
    /* Upload-first streams acquire dimensionality only from their binding. */
    h[1] = 2;
    h[13] = 1;
    h[17] = 0;
    h[18] = 23;
    h[19] = 0;
    g_assert_true(sgi_kona_texture_native_upload(&dst, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_true(sgi_kona_texture_native_dimensions_1d(&dst, 9, 2));
    g_assert_true(sgi_kona_texture_native_layout(&dst, 9, 0x23));
    g_assert_true(sgi_kona_texture_native_table(&dst, 9, table));
    g_assert_true(sgi_kona_texture_bind(&dst, 9, &p));
    g_assert_true(sgi_kona_texture_resolve(&dst)->one_dimensional);
    g_assert_cmpuint(sgi_kona_texture_resolve(&dst)->pyramid->count, ==, 1);
    g_assert_cmphex(texel(&dst, 0, 1), ==, pixels[1]);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_repeat_interior(void)
{
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [1] = 3, [2] = 2, [9] = 0x30,
        [10] = 9, [13] = 2, [14] = 1, [16] = 0x23, [18] = 23,
    };
    const uint32_t pixels[] = {
        0xff0000ff, 0x00ff00ff, 0x0000ffff,
        0x808080ff, 0xffffffff, 0x0000ffff,
    };
    const uint32_t edge[] = { 0x0000ffff };
    uint32_t table[16];
    const SGIGLTexImage *im;
    SGIGLDrawState state = { .texture_2d = true,
                            .texture_env_mode = SGI_GL_REPLACE };
    const float color[4] = { 1, 1, 1, 1 };
    float coord[4] = { .25, .75, 0, 1 }, out[4];
    GByteArray *a;
    SGIKonaTextureObject *o;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    g_assert_false(sgi_kona_texture_native_layout(&src, 9, 0x23));
    sgi_kona_texture_defaults(&p);
    p.min_filter = p.mag_filter = SGI_GL_NEAREST;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[0] = 23;
    g_assert_true(sgi_kona_texture_native_table(&src, 9, table));
    g_assert_true(sgi_kona_texture_native_dimensions(&src, 9, 2, 2));
    g_assert_true(sgi_kona_texture_native_layout(&src, 9, 0x23));
    g_assert_false(sgi_kona_texture_native_layout(&src, 9, 0x2e));
    g_assert_false(sgi_kona_texture_native_layout(&src, 9, 0x100));
    g_assert_false(sgi_kona_texture_native_layout(&src, 9, 0x1000));
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    /* The received positive border expands row stride beyond logical width. */
    g_assert_true(sgi_kona_texture_native_upload(&src, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    h[1] = h[2] = 1;
    h[11] = h[13] = 7;
    h[12] = h[14] = 3;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, edge,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    im = sgi_kona_texture_resolve(&src);
    g_assert_cmpuint(im->pyramid->count, ==, 1);
    g_assert_cmpuint(im->pyramid->levels[0].width, ==, 2);
    g_assert_cmpuint(im->pyramid->levels[0].height, ==, 2);
    g_assert_cmpuint(im->pyramid->levels[0].border, ==, 0);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat_with_epsilon(out[0], 128.0 / 255, .000001);
    g_assert_cmpfloat(out[2], ==, out[0]);
    coord[0] = 1.25;
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat_with_epsilon(out[0], 128.0 / 255, .000001);
    p.min_filter = p.mag_filter = SGI_GL_LINEAR;
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    coord[0] = 0;
    coord[1] = .25;
    g_assert_true(sgi_gl_texture_apply_pixel(&state,
                                            sgi_kona_texture_resolve(&src),
                                            coord, color, out));
    g_assert_cmpfloat(out[0], ==, .5);
    g_assert_cmpfloat(out[1], ==, .5);
    g_assert_cmpfloat(out[2], ==, 0);
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_assert_true(sgi_gl_texture_apply_pixel(&state,
                                            sgi_kona_texture_resolve(&dst),
                                            coord, color, out));
    g_assert_cmpfloat(out[0], ==, .5);
    g_assert_cmpfloat(out[2], ==, 0);
    stl_be_p(a->data + a->len - 52, 2);
    g_assert_false(load(a, &dst));
    g_byte_array_unref(a);
    a = save(&src);
    stl_be_p(a->data + a->len - 48, 0x2e);
    g_assert_false(load(a, &dst));
    g_byte_array_unref(a);
    for (unsigned bit = 8; bit <= 12; bit += 4) {
        a = save(&src);
        stl_be_p(a->data + a->len - 48, 0x23 | (1u << bit));
        g_assert_false(load(a, &dst));
        g_byte_array_unref(a);
    }
    /* A different exponent plane with the same descriptor cannot shadow it. */
    h[16] = 0x24;
    h[11] = h[12] = h[13] = h[14] = 0;
    g_assert_true(sgi_kona_texture_native_upload(&src, h, edge,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    g_assert_cmpuint(sgi_kona_texture_resolve(&src)->pyramid->count, ==, 1);
    o = g_hash_table_lookup(src.objects, GUINT_TO_POINTER(9));
    g_assert_cmpuint(o->native_count, ==, 2);
    p.wrap_s = SGI_GL_CLAMP_TO_EDGE;
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    g_assert_cmpuint(sgi_kona_texture_resolve(&src)->pyramid->count, ==, 1);
    p.wrap_s = SGI_GL_REPEAT;
    p.base_level = 1;
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    g_assert_cmpuint(sgi_kona_texture_resolve(&src)->pyramid->count, ==, 0);
    g_assert_true(sgi_kona_texture_native_dimensions(&src, 9, 2, 2));
    o = g_hash_table_lookup(src.objects, GUINT_TO_POINTER(9));
    g_assert_false(o->native_layout_valid);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_border(gconstpointer data)
{
    bool one_dimensional = GPOINTER_TO_UINT(data) & (1u << 24);
    unsigned level = (GPOINTER_TO_UINT(data) >> 16) & 255;
    uint32_t width = (GPOINTER_TO_UINT(data) >> 8) & 255;
    uint32_t height = GPOINTER_TO_UINT(data) & 255;
    SGIKonaTextureState src, dst;
    SGIKonaTextureParams p;
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [9] = 0x30, [10] = 9,
    };
    uint32_t table[16], pixels[32];
    const uint32_t blue[] = { 0x0000ffff };
    unsigned ps = ctz32(width), pt = ctz32(height);
    unsigned bs = MIN(ps + 1, 3), bt = MIN(pt + 1, 2);
    uint32_t nx = (1u << bs) - 1, ny = (1u << bt) - 1;
    const SGIGLTexImage *im;
    const SGIGLTexLevel *view;
    SGIGLDrawState state = { .texture_2d = true,
                            .texture_env_mode = SGI_GL_REPLACE };
    const float color[4] = { 1, 1, 1, 1 };
    float coord[4] = { 0, .5, 0, 1 }, out[4];
    uint32_t rgba;
    SGIKonaTextureObject *o;
    SGIKonaTextureNativeImage *border;
    GByteArray *a;
    uint64_t at;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    sgi_kona_texture_defaults(&p);
    h[0] = one_dimensional ? SGI_GL_TEXTURE_1D : SGI_GL_TEXTURE_2D;
    state.texture_1d = one_dimensional;
    state.texture_2d = !one_dimensional;
    p.min_filter = p.mag_filter = SGI_GL_LINEAR;
    p.wrap_s = p.wrap_t = SGI_GL_CLAMP;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[level] = 23;
    g_assert_true(sgi_kona_texture_native_table(&src, 9, table));
    g_assert_true(one_dimensional ?
        sgi_kona_texture_native_dimensions_1d(&src, 9, width << level) :
        sgi_kona_texture_native_dimensions(&src, 9, width << level,
                                            height << level));
    g_assert_true(sgi_kona_texture_native_layout(&src, 9,
        MAX(ps + level + 1, 3) | (MAX(pt + level + 1, 2) << 4)));
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    h[1] = width;
    h[2] = height;
    h[13] = width - 1;
    h[14] = height - 1;
    h[16] = MAX((int)ps + 1, 3) | (MAX((int)pt + 1, 2) << 4);
    h[17] = level & 1 ? 8 : 0;
    h[18] = level & 1 ? 0 : 23;
    h[19] = level & 1 ? 23 : 0;
    for (unsigned k = 0; k < width * height; k++) {
        pixels[k] = red[0];
    }
    g_assert_true(sgi_kona_texture_native_upload(&src, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    im = sgi_kona_texture_resolve(&src);
    g_assert_cmpuint(im->pyramid->count, ==, 1);
    view = &im->pyramid->levels[0];
    g_assert_cmpuint(view->border, ==, 1);
    g_assert_nonnull(view->read_border_texel);
    if (one_dimensional) {
        g_assert_false(view->read_border_texel(view->opaque, level,
                                              -1, 1, &rgba));
    }
    g_assert_false(view->read_border_texel(view->opaque, level, -1, 0, &rgba));
    if (!level) {
        out[0] = .123f;
        g_assert_false(sgi_gl_texture_apply_pixel(&state, im, coord,
                                                 color, out));
        g_assert_cmpfloat(out[0], ==, .123f);
    }
    h[1] = h[2] = 1;
    h[16] = bs | (bt << 4);
    for (int32_t y = one_dimensional ? 0 : -1;
         y <= (one_dimensional ? 0 : (int32_t)height); y++) {
        for (int32_t x = -1; x <= (int32_t)width; x++) {
            if (x >= 0 && x < width && y >= 0 && y < height) {
                continue;
            }
            h[11] = h[13] = x < 0 ? nx : x;
            h[12] = h[14] = y < 0 ? ny : y;
            g_assert_true(sgi_kona_texture_native_upload(&src, h, blue,
                                                   SGI_GL_RGBA, SGI_GL_RGBA));
        }
    }
    im = sgi_kona_texture_resolve(&src);
    view = &im->pyramid->levels[0];
    for (int32_t y = one_dimensional ? 0 : -1;
         y <= (one_dimensional ? 0 : (int32_t)height); y++) {
        for (int32_t x = -1; x <= (int32_t)width; x++) {
            if (x >= 0 && x < width && y >= 0 && y < height) {
                continue;
            }
            g_assert_true(view->read_border_texel(view->opaque, level,
                                                  x, y, &rgba));
            g_assert_cmphex(rgba, ==, blue[0]);
        }
    }
    g_assert_false(view->read_border_texel(view->opaque, level,
                                          -2, 0, &rgba));
    g_assert_false(view->read_border_texel(view->opaque, level,
                                          0, 0, &rgba));
    g_assert_true(view->read_texel(view->opaque, level, width - 1,
                                  height - 1, &rgba));
    g_assert_cmphex(rgba, ==, red[0]);
    if (!level) {
        g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord,
                                                color, out));
        g_assert_cmpfloat(out[0], ==, .5);
        g_assert_cmpfloat(out[2], ==, .5);
        coord[1] = one_dimensional ? NAN : 0;
        coord[2] = one_dimensional ? NAN : 0;
        g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord,
                                                color, out));
        g_assert_cmpfloat(out[0], ==, one_dimensional ? .5 : .25);
        g_assert_cmpfloat(out[2], ==, one_dimensional ? .5 : .75);
        coord[1] = .5;
        p.wrap_s = SGI_GL_CLAMP_TO_EDGE;
        p.wrap_t = SGI_GL_REPEAT;
        g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
        im = sgi_kona_texture_resolve(&src);
        g_assert_cmpuint(im->pyramid->levels[0].border, ==, 0);
        g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord,
                                                color, out));
        g_assert_cmpfloat(out[0], ==, 1);
        p.wrap_s = SGI_GL_CLAMP_TO_BORDER;
        g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
        im = sgi_kona_texture_resolve(&src);
        coord[0] = -1;
        g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord,
                                                color, out));
        g_assert_cmpfloat(out[2], ==, 1);
        p.wrap_s = SGI_GL_REPEAT;
        p.wrap_t = SGI_GL_CLAMP;
        g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
        im = sgi_kona_texture_resolve(&src);
        coord[1] = 0;
        g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord,
                                                color, out));
        g_assert_cmpfloat(out[0], ==, one_dimensional ? 1 : .5);
        g_assert_cmpfloat(out[2], ==, one_dimensional ? 0 : .5);
        if (one_dimensional) {
            g_assert_cmpuint(im->pyramid->levels[0].border, ==, 0);
        }
    }
    p.wrap_s = p.wrap_t = SGI_GL_CLAMP;
    g_assert_true(sgi_kona_texture_bind(&src, 9, &p));
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_byte_array_unref(a);
    im = sgi_kona_texture_resolve(&dst);
    view = &im->pyramid->levels[0];
    g_assert_true(view->read_border_texel(view->opaque, level,
                                              -1, one_dimensional ? 0 : -1,
                                              &rgba));
    g_assert_cmphex(rgba, ==, blue[0]);
    o = g_hash_table_lookup(dst.objects, GUINT_TO_POINTER(9));
    border = &o->native[o->native_count - 1];
    at = (one_dimensional ? 0 : (uint64_t)ny * border->image.width) + nx;

    border->coverage[at / 8] &= ~(1u << (at % 8));
    border->covered--;
    g_assert_false(view->read_border_texel(view->opaque, level,
                                              -1, one_dimensional ? 0 : -1,
                                              &rgba));
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

static void test_native_border_interior_alias(gconstpointer data)
{
    bool volume = GPOINTER_TO_UINT(data);
    SGIKonaTextureState s;
    SGIKonaTextureParams p;
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [1] = 8, [2] = 4, [9] = 0x30,
        [10] = 9, [13] = 7, [14] = 3, [16] = 0x23, [18] = 23,
    };
    uint32_t pixels[32], table[16];
    const SGIGLTexImage *im;
    const SGIGLTexLevel *view;
    uint32_t rgba;

    if (volume) {
        h[0] = SGI_GL_TEXTURE_3D;
        h[2] = 1;
        h[14] = 0;
        h[16] = 0x113;
        h[17] = 0x1141;
        h[19] = 47;
    }
    sgi_kona_texture_init(&s);
    sgi_kona_texture_defaults(&p);
    p.wrap_s = p.wrap_t = SGI_GL_CLAMP;
    p.min_filter = p.mag_filter = SGI_GL_LINEAR;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = UINT32_MAX;
    }
    table[0] = 23;
    table[1] = volume ? 47 : UINT32_MAX;
    g_assert_true(sgi_kona_texture_native_table(&s, 9, table));
    g_assert_true(volume ?
        sgi_kona_texture_native_volume_dimensions(&s, 9, 8, 1, 1) :
        sgi_kona_texture_native_dimensions(&s, 9, 8, 4));
    g_assert_true(sgi_kona_texture_native_layout(&s, 9, h[16]));
    g_assert_true(sgi_kona_texture_bind(&s, 9, &p));
    for (unsigned k = 0; k < ARRAY_SIZE(pixels); k++) {
        pixels[k] = red[0];
    }
    g_assert_true(sgi_kona_texture_native_upload(&s, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    im = sgi_kona_texture_resolve(&s);
    g_assert_cmpuint(im->pyramid->count, ==, 1);
    view = &im->pyramid->levels[0];
    /* Complete interior coverage cannot supply overlapping border addresses. */
    if (volume) {
        g_assert_false(view->read_border_texel_3d(view->opaque, 0,
                                                 -1, 0, 0, &rgba));
        g_assert_false(view->read_border_texel_3d(view->opaque, 0,
                                                 0, -1, 0, &rgba));
    } else {
        g_assert_false(view->read_border_texel(view->opaque, 0, -1, 0, &rgba));
        g_assert_false(view->read_border_texel(view->opaque, 0, 0, -1, &rgba));
    }
    g_assert_true(view->read_texel(view->opaque, 0, 7, h[2] - 1, &rgba));
    g_assert_cmphex(rgba, ==, red[0]);
    sgi_kona_texture_cleanup(&s);
}

static void test_native_repeat_interior_mipmaps(void)
{
    SGIKonaTextureState s;
    SGIKonaTextureParams p;
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [1] = 8, [2] = 4, [9] = 0x30,
        [10] = 9, [13] = 7, [14] = 3, [16] = 0x45, [18] = 23,
    };
    uint32_t pixels[32], table[16];
    SGIKonaTextureObject *o;
    SGIGLDrawState state = { .texture_2d = true,
                            .texture_env_mode = SGI_GL_REPLACE };
    const float color[4] = { 1, 1, 1, 1 };
    const float coord[4] = { .5, .5, 0, 1 };
    float out[4];
    const SGIGLTexImage *im;

    sgi_kona_texture_init(&s);
    sgi_kona_texture_defaults(&p);
    p.min_filter = SGI_GL_NEAREST_MIPMAP_NEAREST;
    p.mag_filter = SGI_GL_NEAREST;
    p.min_lod = p.max_lod = 1;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = 23 + k;
    }
    g_assert_true(sgi_kona_texture_native_table(&s, 9, table));
    g_assert_true(sgi_kona_texture_native_dimensions(&s, 9, 8, 4));
    g_assert_true(sgi_kona_texture_native_layout(&s, 9, 0x45));
    g_assert_true(sgi_kona_texture_bind(&s, 9, &p));
    for (unsigned level = 0; level < 4; level++) {
        uint32_t w = MAX(1u, 8u >> level), hgt = MAX(1u, 4u >> level);

        h[1] = w;
        h[2] = hgt;
        h[13] = w - 1;
        h[14] = hgt - 1;
        h[16] = MAX(5 - (int)level, 3) | (MAX(4 - (int)level, 2) << 4);
        h[17] = level & 1 ? 8 : 0;
        h[18] = level & 1 ? 0 : table[level];
        h[19] = level & 1 ? table[level] : 0;
        for (unsigned k = 0; k < w * hgt; k++) {
            pixels[k] = level == 1 ? green[0] : red[0];
        }
        g_assert_true(sgi_kona_texture_native_upload(&s, h, pixels,
                                                    SGI_GL_RGBA, SGI_GL_RGBA));
    }
    im = sgi_kona_texture_resolve(&s);
    g_assert_cmpuint(im->pyramid->count, ==, 4);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat(out[1], ==, 1);
    o = g_hash_table_lookup(s.objects, GUINT_TO_POINTER(9));
    o->native[1].coverage[0] &= ~1u;
    o->native[1].covered--;
    im = sgi_kona_texture_resolve(&s);
    g_assert_cmpuint(im->pyramid->count, ==, 3);
    out[0] = .123f;
    g_assert_false(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat(out[0], ==, .123f);
    o->native[1].coverage[0] |= 1;
    o->native[1].covered++;
    /*
     * The same address in the other bank is not level 0, whose bank the
     * binding gives, so the level still resolves.
     */
    h[1] = 8;
    h[2] = 4;
    h[13] = 7;
    h[14] = 3;
    h[16] = 0x45;
    h[17] = 8;
    h[18] = 0;
    h[19] = table[0];
    g_assert_true(sgi_kona_texture_native_upload(&s, h, pixels,
                                                SGI_GL_RGBA, SGI_GL_RGBA));
    im = sgi_kona_texture_resolve(&s);
    g_assert_cmpuint(im->pyramid->count, ==, 4);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat(out[1], ==, 1);
    sgi_kona_texture_cleanup(&s);
}

/*
 * tex_cube's small levels: an 8x8 texture whose 4x4, 2x2 and 1x1 levels
 * arrive in 8x4 tiles at (4, 0), (6, 2) and (7, 3), the tile's extent
 * minus the level's. Level 2 holds green, the others red.
 */
static const SGIGLTexImage *load_far_corner_levels(SGIKonaTextureState *s,
                                                   uint32_t wrap)
{
    SGIKonaTextureParams p;
    static const uint32_t origin[4][2] = { { 0, 0 }, { 4, 0 }, { 6, 2 },
                                           { 7, 3 } };
    uint32_t h[20] = {
        [0] = SGI_GL_TEXTURE_2D, [9] = 0x30, [10] = 9,
    };
    uint32_t pixels[64], table[16];

    sgi_kona_texture_init(s);
    sgi_kona_texture_defaults(&p);
    p.min_filter = SGI_GL_NEAREST_MIPMAP_NEAREST;
    p.mag_filter = SGI_GL_NEAREST;
    p.min_lod = p.max_lod = 2;
    p.wrap_s = p.wrap_t = wrap;
    for (unsigned k = 0; k < ARRAY_SIZE(table); k++) {
        table[k] = 23 + k;
    }
    g_assert_true(sgi_kona_texture_native_table(s, 9, table));
    g_assert_true(sgi_kona_texture_native_dimensions(s, 9, 8, 8));
    g_assert_true(sgi_kona_texture_native_layout(s, 9, 0x33));
    g_assert_true(sgi_kona_texture_bind(s, 9, &p));
    for (unsigned level = 0; level < 4; level++) {
        uint32_t w = 8u >> level;

        h[1] = h[2] = w;
        h[11] = origin[level][0];
        h[12] = origin[level][1];
        h[13] = h[11] + w - 1;
        h[14] = h[12] + w - 1;
        h[16] = level ? 0x23 : 0x33;
        h[17] = level & 1 ? 8 : 0;
        h[18] = level & 1 ? 0 : table[level];
        h[19] = level & 1 ? table[level] : 0;
        for (unsigned k = 0; k < w * w; k++) {
            pixels[k] = level == 2 ? green[0] : red[0];
        }
        g_assert_true(sgi_kona_texture_native_upload(s, h, pixels,
                                                    SGI_GL_RGBA, SGI_GL_RGBA));
    }
    return sgi_kona_texture_resolve(s);
}

/* The pyramid is complete, and level 2 samples the texels at (6, 2). */
static void test_native_small_levels_far_corner(void)
{
    SGIKonaTextureState s;
    SGIGLDrawState state = { .texture_2d = true,
                            .texture_env_mode = SGI_GL_REPLACE };
    const float color[4] = { 1, 1, 1, 1 };
    const float coord[4] = { .5, .5, 0, 1 };
    float out[4];
    const SGIGLTexImage *im = load_far_corner_levels(&s, SGI_GL_REPEAT);

    g_assert_cmpuint(im->pyramid->count, ==, 4);
    g_assert_true(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    g_assert_cmpfloat(out[0], ==, 0);
    g_assert_cmpfloat(out[1], ==, 1);
    sgi_kona_texture_cleanup(&s);
}

/*
 * Under GL_CLAMP a far-corner level has no border reader: one addressed
 * from the tile origin would return the level's own interior texels as
 * its border. The origin level keeps its reader, and sampling the
 * far-corner level refuses rather than drawing interior as border.
 */
static void test_native_far_corner_level_has_no_border(void)
{
    SGIKonaTextureState s;
    SGIGLDrawState state = { .texture_2d = true,
                            .texture_env_mode = SGI_GL_REPLACE };
    const float color[4] = { 1, 1, 1, 1 };
    const float coord[4] = { .5, .5, 0, 1 };
    float out[4];
    const SGIGLTexImage *im = load_far_corner_levels(&s, SGI_GL_CLAMP);

    g_assert_cmpuint(im->pyramid->count, ==, 4);
    g_assert_nonnull(im->pyramid->levels[0].read_border_texel);
    for (unsigned k = 1; k < 4; k++) {
        g_assert_true(im->pyramid->levels[k].border);
        g_assert_null(im->pyramid->levels[k].read_border_texel);
    }
    g_assert_false(sgi_gl_texture_apply_pixel(&state, im, coord, color, out));
    sgi_kona_texture_cleanup(&s);
}

static void test_filter_curve_budget(void)
{
    SGIKonaTextureState s;
    uint32_t words[33] = { 0 };
    uint64_t first;

    sgi_kona_texture_init(&s);
    g_assert_true(sgi_kona_texture_curve_upload(&s, 1, words));
    first = s.bytes;
    s.bytes = SGI_KONA_TEXTURE_BYTES;
    g_assert_false(sgi_kona_texture_curve_upload(&s, 2, words));
    /* Replacing an existing slot requires no additional budget. */
    words[0] = 0x200;
    g_assert_true(sgi_kona_texture_curve_upload(&s, 1, words));
    g_assert_cmpfloat(sgi_kona_texture_curve(&s, 1)[0], ==, -1);
    g_assert_null(sgi_kona_texture_curve(&s, 2));
    s.bytes = first;
    sgi_kona_texture_cleanup(&s);
}

static void test_filter_curve_migration(void)
{
    SGIKonaTextureState src, dst;
    uint32_t words[33] = { 0x9ffc0101, 0x80 };
    GByteArray *a;
    uint64_t bytes;

    sgi_kona_texture_init(&src);
    sgi_kona_texture_init(&dst);
    g_assert_true(sgi_kona_texture_curve_upload(&src, 13, words));
    a = save(&src);
    g_assert_true(load(a, &dst));
    g_assert_cmpfloat(sgi_kona_texture_curve(&dst, 13)[3], ==, -4);
    g_assert_cmpuint(dst.bytes, ==, src.bytes);
    bytes = dst.bytes;
    /* Header16, count4, slot4; each subsequent entry has 32 packed words. */
    /* Words that are not a spline are slot state and arrive without one. */
    stl_be_p(a->data + 28, 0x100);
    g_assert_true(load(a, &dst));
    g_assert_null(sgi_kona_texture_curve(&dst, 13));
    g_assert_cmpuint(dst.bytes, ==, bytes);
    g_byte_array_unref(a);
    a = save(&src);
    stl_be_p(a->data + 16, SGI_KONA_TEXTURE_CURVE_SLOTS + 1);
    g_assert_false(load(a, &dst));
    g_byte_array_unref(a);
    a = save(&src);
    stl_be_p(a->data + 20, SGI_KONA_TEXTURE_CURVE_SLOTS);
    g_assert_false(load(a, &dst));
    g_byte_array_unref(a);
    a = save(&src);
    g_byte_array_set_size(a, a->len - 1);
    g_assert_false(load(a, &dst));
    g_byte_array_unref(a);
    g_assert_true(sgi_kona_texture_curve_upload(&src, 14, words));
    a = save(&src);
    /* Duplicate slots cannot replace one another inside a migration stream. */
    stl_be_p(a->data + 152, ldl_be_p(a->data + 20));
    g_assert_false(load(a, &dst));
    g_byte_array_unref(a);
    sgi_kona_texture_reset(&src);
    a = save(&src);
    /* Loading a stream with no curves clears the destination's slots. */
    g_assert_true(load(a, &dst));
    g_assert_null(sgi_kona_texture_curve(&dst, 13));
    g_assert_cmpuint(dst.bytes, ==, 0);
    g_byte_array_unref(a);
    sgi_kona_texture_cleanup(&src);
    sgi_kona_texture_cleanup(&dst);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    g_test_add_func("/sgi/kona/texture/objects", test_objects);
    g_test_add_func("/sgi/kona/texture/sampling", test_sampling);
    g_test_add_func("/sgi/kona/texture/depth/native-storage",
                    test_native_depth_storage);
    g_test_add_func("/sgi/kona/texture/depth/storage", test_depth_storage);
    g_test_add_func("/sgi/kona/texture/migration", test_migration);
    g_test_add_func("/sgi/kona/texture/subimage", test_subimage);
    g_test_add_func("/sgi/kona/texture/native", test_native);
    g_test_add_func("/sgi/kona/texture/native-chunks", test_native_chunks);
    g_test_add_func("/sgi/kona/texture/native-growth", test_native_growth);
    g_test_add_func("/sgi/kona/texture/native-format", test_native_format);
    g_test_add_func("/sgi/kona/texture/native-internal-format",
                    test_native_internal_format);
    g_test_add_func("/sgi/kona/texture/native-detail", test_native_detail);
    g_test_add_func("/sgi/kona/texture/native-repeat-interior",
                    test_native_repeat_interior);
    g_test_add_func("/sgi/kona/texture/native-one-dimensional",
                    test_native_one_dimensional);
    g_test_add_data_func("/sgi/kona/texture/native-border/separate-geometry",
                         GUINT_TO_POINTER(0x804), test_native_border);
    g_test_add_data_func("/sgi/kona/texture/native-border/shared-geometry",
                         GUINT_TO_POINTER(0x202), test_native_border);
    g_test_add_data_func("/sgi/kona/texture/native-border/odd-mipmap",
                         GUINT_TO_POINTER(0x10804), test_native_border);
    g_test_add_data_func("/sgi/kona/texture/native-border/single-texel",
                         GUINT_TO_POINTER(0x20101), test_native_border);
    g_test_add_data_func("/sgi/kona/texture/native-border/1d-separate-geometry",
                         GUINT_TO_POINTER(0x1000801), test_native_border);
    g_test_add_data_func("/sgi/kona/texture/native-border/1d-small-interior",
                         GUINT_TO_POINTER(0x1000201), test_native_border);
    g_test_add_data_func("/sgi/kona/texture/native-border/1d-odd-mipmap",
                         GUINT_TO_POINTER(0x1010801), test_native_border);
    g_test_add_data_func("/sgi/kona/texture/native-border/1d-single-texel",
                         GUINT_TO_POINTER(0x1020101), test_native_border);
    g_test_add_data_func("/sgi/kona/texture/native-border/interior-alias",
                         GUINT_TO_POINTER(0),
                         test_native_border_interior_alias);
    g_test_add_data_func("/sgi/kona/texture/native-volume/border-alias",
                         GUINT_TO_POINTER(1),
                         test_native_border_interior_alias);
    g_test_add_func("/sgi/kona/texture/native-repeat-interior-mipmaps",
                    test_native_repeat_interior_mipmaps);
    g_test_add_func("/sgi/kona/texture/native-small-levels-far-corner",
                    test_native_small_levels_far_corner);
    g_test_add_func("/sgi/kona/texture/native-far-corner-level-has-no-border",
                    test_native_far_corner_level_has_no_border);
    g_test_add_func("/sgi/kona/texture/native-mixed-transport",
                    test_native_mixed_transport);
    g_test_add_func("/sgi/kona/texture/native-duplicate-transport-stream",
                    test_native_duplicate_transport_stream);
    g_test_add_func("/sgi/kona/texture/native-volume/slices",
                    test_native_volume_slices);
    g_test_add_data_func("/sgi/kona/texture/native-volume/padded-interior",
                         GUINT_TO_POINTER(0x20202),
                         test_native_volume_interior);
    g_test_add_data_func("/sgi/kona/texture/native-volume/odd-interior",
                         GUINT_TO_POINTER(0x1010101),
                         test_native_volume_interior);
    g_test_add_data_func("/sgi/kona/texture/native-volume/wide-interior",
                         GUINT_TO_POINTER(0x80402),
                         test_native_volume_interior);
    g_test_add_data_func("/sgi/kona/texture/native-volume/border-separate",
                         GUINT_TO_POINTER(0x20202), test_native_volume_border);
    g_test_add_data_func("/sgi/kona/texture/native-volume/border-shared",
                         GUINT_TO_POINTER(0x40101), test_native_volume_border);
    g_test_add_data_func("/sgi/kona/texture/native-volume/border-wide",
                         GUINT_TO_POINTER(0x80202), test_native_volume_border);
    g_test_add_data_func("/sgi/kona/texture/native-volume/border-odd",
                         GUINT_TO_POINTER(0x1020101),
                         test_native_volume_border);
    g_test_add_data_func("/sgi/kona/texture/native-volume/border-single-voxel",
                         GUINT_TO_POINTER(0x10101), test_native_volume_border);
    g_test_add_func("/sgi/kona/texture/native-volume/sampling",
                    test_native_volume_sampling);
    g_test_add_func("/sgi/kona/texture/native-migration",
                    test_native_migration);
    g_test_add_func("/sgi/kona/texture/native-sampling", test_native_sampling);
    g_test_add_func("/sgi/kona/texture/native-negative-lod",
                    test_native_negative_lod);
    g_test_add_func("/sgi/kona/texture/filter-curves/storage",
                    test_filter_curves);
    g_test_add_func("/sgi/kona/texture/filter-curves/sharpen-sampling",
                    test_sharpen_curve_sampling);
    g_test_add_func("/sgi/kona/texture/filter-curves/budget",
                    test_filter_curve_budget);
    g_test_add_func("/sgi/kona/texture/filter-curves/migration",
                    test_filter_curve_migration);
    return g_test_run();
}
