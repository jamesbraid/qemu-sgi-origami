/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include "io/channel-buffer.h"
#include "../migration/qemu-file.h"
#include "hw/misc/sgi-kona-texture.h"

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
    g_assert_true(sgi_kona_texture_bind(&src, 0xf2345678, &p));
    a = save(&src);
    g_assert_true(load(a, &dst));
    {
        const SGIGLTexImage *im = sgi_kona_texture_resolve(&dst);
        double rgba[] = { 1, 0, 0, 1 };

        im->post_filter(im->opaque, rgba);
        g_assert_cmpfloat(rgba[0], ==, 0.5);
        g_assert_cmpfloat(rgba[2], ==, 0.25);
    }
    g_assert_cmpuint(dst.bytes, ==, src.bytes);
    g_assert_cmphex(texel(&dst, 0, 0), ==, red[0]);
    g_assert_cmphex(texel(&dst, 1, 0), ==, green[0]);
    bytes = dst.bytes;
    /* First level width: header16 + object8 + sampler120 + level id4. */
    stl_be_p(a->data + 148, UINT32_MAX);
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    g_test_add_func("/sgi/kona/texture/objects", test_objects);
    g_test_add_func("/sgi/kona/texture/sampling", test_sampling);
    g_test_add_func("/sgi/kona/texture/migration", test_migration);
    return g_test_run();
}
