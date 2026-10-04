/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <glib/gstdio.h>
#ifdef CONFIG_MODULES
#include <gmodule.h>
#endif
#include "qemu/cutils.h"
#ifdef CONFIG_MODULES
#include "qemu/module.h"
#endif
#ifdef CONFIG_MODULE_UPGRADES
#include "qemu-version.h"
#endif
#include <math.h>
#include "ui/sgi-renderer.h"

static SGIRendererRequest *fixture(void)
{
    SGIRendererRequest *request = g_new0(SGIRendererRequest, 1);
    const float vertices[3][4] = {
        { 16.25f, 16.25f, 0, 1 },
        { 111.25f, 16.25f, 0, 1 },
        { 16.25f, 111.25f, 0, 1 },
    };

    memcpy(request->vertices, vertices, sizeof(vertices));
    request->rgb[0] = 255;
    request->clip[2] = request->clip[3] = 127;
    for (unsigned y = 16; y < 112; y++) {
        for (unsigned x = 16; x < 112; x++) {
            if (x + y < 127) {
                unsigned offset = (y * 128 + x) * 4;
                request->rgba[offset] = 255;
                request->rgba[offset + 3] = 255;
            }
        }
    }
    return request;
}

static void test_smooth(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    g_autofree SGIRendererRequest *request = g_new0(SGIRendererRequest, 1);
    const double inverse_w[3] = { 1, mode == 1 ? 2 : 1, mode == 1 ? 4 : 1 };

    request->smooth = true;
    request->vertices[1][0] = request->vertices[2][1] = 128;
    request->clip[2] = request->clip[3] = 127;
    for (unsigned i = 0; i < 3; i++) {
        request->vertices[i][3] = 1;
        request->colour[i][i] = 1;
        request->colour[i][3] = i * 0.5;
        request->inverse_w[i] = inverse_w[i];
    }
    if (mode == 2) {
        float vertex[4];
        double colour[4];

        memcpy(vertex, request->vertices[1], sizeof(vertex));
        memcpy(request->vertices[1], request->vertices[2], sizeof(vertex));
        memcpy(request->vertices[2], vertex, sizeof(vertex));
        memcpy(colour, request->colour[1], sizeof(colour));
        memcpy(request->colour[1], request->colour[2], sizeof(colour));
        memcpy(request->colour[2], colour, sizeof(colour));
    }
    if (mode == 3) {
        const int64_t bounds[4] = { 0, 0, 127, 127 };

        g_assert_true(sgi_renderer_request_tile(request, 16, 32, 64, 64,
                                               bounds));
    }
    for (unsigned y = 0; y < 128; y++) {
        for (unsigned x = 0; x < 128; x++) {
            unsigned wx = x + (mode == 3 ? 16 : 0);
            unsigned wy = y + (mode == 3 ? 32 : 0);
            uint8_t *pixel = request->rgba + (y * 128 + x) * 4;
            double colour[4];
            bool covered = wx + wy < 127 &&
                           (mode != 3 || (x < 64 && y < 64));

            g_assert_cmpint(sgi_renderer_fragment(request, x, y, colour),
                            ==, covered);
            if (covered) {
                double weights[3] = { 254 - 2 * (double)(wx + wy),
                                      2 * wx + 1, 2 * wy + 1 };
                double sum = 0;

                for (unsigned c = 0; c < 3; c++) {
                    weights[c] *= inverse_w[c];
                    sum += weights[c];
                }
                for (unsigned c = 0; c < 3; c++) {
                    double expected = weights[c] / sum;

                    g_assert_cmpfloat_with_epsilon(colour[c], expected,
                                                   0.00000000000001);
                    pixel[c] = floor(expected * 255 + 0.5);
                }
                g_assert_cmpfloat_with_epsilon(colour[3],
                    (weights[1] * 0.5 + weights[2]) / sum,
                    0.00000000000001);
                pixel[3] = 255;
            }
        }
    }
    g_assert_true(sgi_renderer_staging_valid(request));
    request->rgba[4 * (16 * 128 + 16)] ^= 1;
    g_assert_false(sgi_renderer_staging_valid(request));
}

static void test_smooth_invalid(void)
{
    g_autofree SGIRendererRequest *request = fixture();

    request->smooth = true;
    for (unsigned i = 0; i < 3; i++) {
        request->inverse_w[i] = 1;
    }
    g_assert_true(sgi_renderer_request_valid(request));
    request->colour[1][2] = NAN;
    g_assert_false(sgi_renderer_request_valid(request));
    request->colour[1][2] = 2;
    g_assert_false(sgi_renderer_request_valid(request));
    request->colour[1][2] = -1;
    g_assert_false(sgi_renderer_request_valid(request));
    request->colour[1][2] = 0;
    request->inverse_w[0] = 0;
    g_assert_false(sgi_renderer_request_valid(request));
    request->inverse_w[0] = INFINITY;
    g_assert_false(sgi_renderer_request_valid(request));
    request->inverse_w[0] = -1;
    g_assert_false(sgi_renderer_request_valid(request));
}

static void test_staging(void)
{
    g_autofree SGIRendererRequest *request = fixture();

    g_assert_true(sgi_renderer_staging_valid(request));
    request->rgba[(16 * 128 + 16) * 4 + 3] = 0;
    g_assert_false(sgi_renderer_staging_valid(request));
}

static void test_clip(void)
{
    g_autofree SGIRendererRequest *request = fixture();

    request->clip[0] = 50;
    for (unsigned y = 0; y < 128; y++) {
        for (unsigned x = 0; x < 50; x++) {
            memset(request->rgba + (y * 128 + x) * 4, 0, 4);
        }
    }
    g_assert_true(sgi_renderer_staging_valid(request));
    request->rgba[(16 * 128 + 49) * 4 + 3] = 255;
    g_assert_false(sgi_renderer_staging_valid(request));
    request->clip[0] = -1;
    g_assert_false(sgi_renderer_request_valid(request));
}

static void test_empty_clip(void)
{
    g_autofree SGIRendererRequest *request = fixture();

    request->clip[0] = request->clip[1] = 0;
    request->clip[2] = request->clip[3] = -1;
    memset(request->rgba, 0, sizeof(request->rgba));
    g_assert_true(sgi_renderer_request_valid(request));
    g_assert_true(sgi_renderer_staging_valid(request));
    request->rgba[3] = 255;
    g_assert_false(sgi_renderer_staging_valid(request));
    request->clip[2] = -2;
    g_assert_false(sgi_renderer_request_valid(request));
}

static void test_uncovered(void)
{
    g_autofree SGIRendererRequest *request = fixture();

    request->rgba[3] = 255;
    g_assert_false(sgi_renderer_staging_valid(request));
}

static void test_colour(void)
{
    g_autofree SGIRendererRequest *request = fixture();

    request->rgba[(16 * 128 + 16) * 4] = 254;
    g_assert_false(sgi_renderer_staging_valid(request));
}

static void test_request(void)
{
    g_autofree SGIRendererRequest *request = fixture();
    double colour[4];

    request->alpha = NAN;
    g_assert_false(sgi_renderer_request_valid(request));
    request->alpha = 0.375;
    g_assert_true(sgi_renderer_fragment(request, 16, 16, colour));
    g_assert_cmpfloat(colour[0], ==, 1);
    g_assert_cmpfloat(colour[3], ==, 0.375);
    request->vertices[1][0] = 112;
    g_assert_true(sgi_renderer_request_valid(request));
    request->rgb[0] = 127;
    g_assert_true(sgi_renderer_request_valid(request));
    request->vertices[1][0] = NAN;
    g_assert_false(sgi_renderer_request_valid(request));
    request->vertices[1][0] = 16384.25f;
    g_assert_false(sgi_renderer_request_valid(request));
    request->vertices[1][0] = 0.001f;
    g_assert_false(sgi_renderer_request_valid(request));
    request->vertices[1][0] = 111.25f;
    request->vertices[1][2] = 1;
    g_assert_false(sgi_renderer_request_valid(request));
    request->vertices[1][2] = 0;
    request->vertices[1][3] = 0;
    g_assert_false(sgi_renderer_request_valid(request));
}

static void test_large_coordinates(void)
{
    static const float vertices[3][3][4] = {
        { { -128, -128, 0, 1 }, { 256, -128, 0, 1 }, { -128, 256, 0, 1 } },
        { { -16384, -16384, 0, 1 }, { 16384, -16384, 0, 1 },
          { 16384, 16384, 0, 1 } },
        { { 256, 256, 0, 1 }, { 512, 256, 0, 1 }, { 256, 512, 0, 1 } },
    };
    g_autofree SGIRendererRequest *request = fixture();

    for (unsigned mode = 0; mode < ARRAY_SIZE(vertices); mode++) {
        memcpy(request->vertices, vertices[mode], sizeof(request->vertices));
        memset(request->rgba, 0, sizeof(request->rgba));
        for (unsigned y = 0; y < 128; y++) {
            for (unsigned x = 0; x < 128; x++) {
                bool covered = mode == 0 ? x + y < 127 :
                               mode == 1 ? x >= y : false;
                uint8_t *pixel = request->rgba + (y * 128 + x) * 4;

                if (covered) {
                    pixel[0] = pixel[3] = 255;
                }
            }
        }
        g_assert_true(sgi_renderer_request_valid(request));
        g_assert_true(sgi_renderer_staging_valid(request));
    }
    request->vertices[0][0] = -16384.25;
    g_assert_false(sgi_renderer_request_valid(request));
    request->vertices[0][0] = 16384.25;
    g_assert_false(sgi_renderer_request_valid(request));
}

static void test_tiles(gconstpointer opaque)
{
    unsigned mode = GPOINTER_TO_UINT(opaque);
    const int64_t bounds[4] = { mode == 1 ? 120 : 0, mode == 1 ? 70 : 0,
                               mode == 1 ? 189 : 255,
                               mode == 1 ? 149 : 255 };
    const int64_t offsets[6][2] = {
        { 0, 0 }, { 128, 0 }, { 0, 128 }, { 128, 128 },
        { 256, 0 }, { -64, -64 },
    };
    g_autofree SGIRendererRequest *request = g_new0(SGIRendererRequest, 1);

    for (unsigned tile = 0; tile < ARRAY_SIZE(offsets); tile++) {
        int64_t ox = offsets[tile][0], oy = offsets[tile][1];
        unsigned width = mode == 2 ? 65 : 128;
        unsigned height = mode == 2 ? 63 : 128;

        memset(request, 0, sizeof(*request));
        request->vertices[0][3] = request->vertices[1][3] =
            request->vertices[2][3] = 1;
        request->vertices[1][0] = request->vertices[2][0] = 256;
        request->vertices[2][1] = 256;
        request->generation = 19;
        request->rgb[0] = 17;
        request->rgb[1] = 63;
        request->rgb[2] = 191;
        g_assert_true(sgi_renderer_request_tile(request, ox, oy, width, height,
                                               bounds));
        g_assert_cmpfloat(request->vertices[0][0], ==, -ox);
        g_assert_cmpfloat(request->vertices[0][1], ==, -oy);
        g_assert_cmpfloat(request->vertices[2][0], ==, 256 - ox);
        g_assert_cmpfloat(request->vertices[2][1], ==, 256 - oy);
        g_assert_cmpuint(request->generation, ==, 19);
        for (unsigned y = 0; y < 128; y++) {
            for (unsigned x = 0; x < 128; x++) {
                int64_t wx = x + ox, wy = y + oy;
                bool covered = x < width && y < height && wx >= wy &&
                               wx >= bounds[0] && wx <= bounds[2] &&
                               wy >= bounds[1] && wy <= bounds[3];
                uint8_t *pixel = request->rgba + (y * 128 + x) * 4;

                if (covered) {
                    memcpy(pixel, request->rgb, sizeof(request->rgb));
                    pixel[3] = 255;
                }
            }
        }
        g_assert_true(sgi_renderer_staging_valid(request));
        request->rgba[3] ^= 255;
        g_assert_false(sgi_renderer_staging_valid(request));
    }
}

static void test_tile_invalid(void)
{
    g_autofree SGIRendererRequest *request = fixture();
    const int64_t bounds[4] = { INT64_MIN, INT64_MIN, INT64_MAX, INT64_MAX };

    g_assert_false(sgi_renderer_request_tile(request, 0, 0, 0, 128, bounds));
    g_assert_false(sgi_renderer_request_tile(request, 0, 0, 128, 0, bounds));
    g_assert_false(sgi_renderer_request_tile(request, 0, 0, 129, 128, bounds));
    g_assert_false(sgi_renderer_request_tile(request, 0, 0, 128, 129, bounds));
    g_assert_false(sgi_renderer_request_tile(request, INT64_MAX, 0,
                                            128, 128, bounds));
    g_assert_false(sgi_renderer_request_tile(request, 0, INT64_MAX,
                                            128, 128, bounds));
    g_assert_true(sgi_renderer_request_tile(request, 0, 0, 128, 128, bounds));
    g_assert_true(sgi_renderer_staging_valid(request));
}

static void test_culling(void)
{
    for (unsigned mode = 0; mode < 16; mode++) {
        g_autofree SGIRendererRequest *request = fixture();
        bool reverse = mode & 1, clockwise = mode & 2;
        bool cull_front = mode & 4, cull_back = mode & 8;
        bool front = reverse == clockwise;
        bool culled = front ? cull_front : cull_back;
        float vertices[3][4];

        if (reverse) {
            float vertex[4];

            memcpy(vertex, request->vertices[1], sizeof(vertex));
            memcpy(request->vertices[1], request->vertices[2],
                   sizeof(vertex));
            memcpy(request->vertices[2], vertex, sizeof(vertex));
        }
        memcpy(vertices, request->vertices, sizeof(vertices));
        g_assert_true(sgi_renderer_request_cull(request, clockwise,
                                               cull_front, cull_back));
        g_assert_cmpmem(request->vertices, sizeof(request->vertices),
                        vertices, sizeof(vertices));
        g_assert_cmpint(request->clip[2], ==, culled ? -1 : 127);
        if (culled) {
            memset(request->rgba, 0, sizeof(request->rgba));
        }
        g_assert_true(sgi_renderer_staging_valid(request));
    }
    g_autofree SGIRendererRequest *request = fixture();

    memcpy(request->vertices[1], request->vertices[0],
           sizeof(request->vertices[1]));
    memset(request->rgba, 0, sizeof(request->rgba));
    g_assert_true(sgi_renderer_request_cull(request, false, false, false));
    g_assert_true(sgi_renderer_staging_valid(request));
    request->vertices[0][0] = NAN;
    g_assert_false(sgi_renderer_request_cull(request, false, true, true));
}

static void test_triangle_coverage(void)
{
    static const float vertices[4][3][4] = {
        { { 0, 0, 0, 1 }, { 4, 0, 0, 1 }, { 0, 4, 0, 1 } },
        { { 0, 0, 0, 1 }, { 4, 4, 0, 1 }, { 0, 4, 0, 1 } },
        { { 0, 0, 0, 1 }, { 4, 0, 0, 1 }, { 4, 4, 0, 1 } },
        { { 0, 0, 0, 1 }, { 2, 2, 0, 1 }, { 4, 4, 0, 1 } },
    };
    g_autofree SGIRendererRequest *request = g_new0(SGIRendererRequest, 1);

    request->rgb[0] = 17;
    request->rgb[1] = 63;
    request->rgb[2] = 191;
    request->clip[2] = request->clip[3] = 3;
    for (unsigned mode = 0; mode < 8; mode++) {
        unsigned shape = mode % 4;

        memset(request->rgba, 0, sizeof(request->rgba));
        memcpy(request->vertices, vertices[shape], sizeof(request->vertices));
        if (mode >= 4) {
            memcpy(request->vertices[1], vertices[shape][2],
                   sizeof(request->vertices[1]));
            memcpy(request->vertices[2], vertices[shape][1],
                   sizeof(request->vertices[2]));
        }
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                bool covered = shape == 0 ? x + y < 3 :
                               shape == 1 ? x < y :
                               shape == 2 ? x >= y : false;
                uint8_t *pixel = request->rgba + (y * 128 + x) * 4;

                if (covered) {
                    memcpy(pixel, request->rgb, sizeof(request->rgb));
                    pixel[3] = 255;
                }
            }
        }
        g_assert_true(sgi_renderer_request_valid(request));
        g_assert_true(sgi_renderer_staging_valid(request));
        request->rgba[3] = request->rgba[3] ? 0 : 255;
        g_assert_false(sgi_renderer_staging_valid(request));
    }
}

#if !defined(CONFIG_OPENGL) && !defined(CONFIG_MODULES)
static void test_missing_backend(void)
{
    SGIRenderer *renderer = (void *)1;
    Error *err = NULL;

    g_assert_cmpint(sgi_renderer_create(&renderer, &err), ==,
                    SGI_RENDER_CREATE_UNAVAILABLE);
    g_assert_null(renderer);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_null(sgi_renderer_new(&err));
    g_assert_nonnull(err);
    error_free(err);
}

#endif

#if defined(CONFIG_OPENGL) && !defined(CONFIG_MODULES)
static void test_missing_registration(void)
{
    SGIRenderer *renderer = (void *)1;
    Error *err = NULL;

    g_assert_cmpint(sgi_renderer_create(&renderer, &err), ==,
                    SGI_RENDER_CREATE_HOST_ERROR);
    g_assert_null(renderer);
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "SGI OpenGL module did not register a renderer");
    error_free(err);
}
#endif

static unsigned draw_count;
static bool corrupt_colour, corrupt_clip, corrupt_vertices;
static unsigned corrupt_attributes = UINT_MAX;

static void *generation_create(SGIRenderCreateResult *result, Error **errp)
{
    *result = SGI_RENDER_CREATE_OK;
    return &draw_count;
}

static bool generation_draw(void *opaque, SGIRendererRequest *request,
                            Error **errp)
{
    g_autofree SGIRendererRequest *expected = fixture();

    draw_count++;
    if (corrupt_attributes == 0) {
        request->smooth = false;
    } else if (corrupt_attributes == 1) {
        request->colour[0][3] = 0.5;
    } else if (corrupt_attributes == 2) {
        request->inverse_w[1] = 2;
    } else if (corrupt_attributes == 3) {
        request->alpha = 0.5;
    } else if (corrupt_vertices) {
        float vertex[4];

        memcpy(vertex, request->vertices[1], sizeof(vertex));
        memcpy(request->vertices[1], request->vertices[2], sizeof(vertex));
        memcpy(request->vertices[2], vertex, sizeof(vertex));
    } else if (corrupt_clip) {
        request->clip[0] = 50;
        for (unsigned y = 0; y < 128; y++) {
            memset(expected->rgba + y * 128 * 4, 0, 50 * 4);
        }
    } else if (corrupt_colour) {
        request->rgb[0] = 0;
        request->rgb[1] = 255;
        for (unsigned i = 0; i < SGI_RENDERER_BYTES; i += 4) {
            if (expected->rgba[i + 3]) {
                expected->rgba[i] = 0;
                expected->rgba[i + 1] = 255;
            }
        }
    } else {
        request->generation++;
    }
    memcpy(request->rgba, expected->rgba, sizeof(request->rgba));
    return true;
}

static void generation_destroy(void *opaque)
{
}

static const SGIRendererOps response_ops = {
    .create = generation_create,
    .draw = generation_draw,
    .destroy = generation_destroy,
};

static void test_generation(void)
{
    g_autofree SGIRendererRequest *request = fixture();
    Error *err = NULL;
    SGIRenderer *renderer;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    sgi_renderer_register_gl(&response_ops);
    renderer = sgi_renderer_new(&err);
    g_assert_null(err);
    g_assert_nonnull(renderer);
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_cmpuint(draw_count, ==, 1);
    error_free(err);
    sgi_renderer_free(renderer);
}

static void test_input_colour(void)
{
    g_autofree SGIRendererRequest *request = fixture();
    SGIRenderer *renderer;
    Error *err = NULL;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    corrupt_colour = true;
    sgi_renderer_register_gl(&response_ops);
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_cmpuint(draw_count, ==, 1);
    error_free(err);
    sgi_renderer_free(renderer);
}

static void test_input_clip(void)
{
    g_autofree SGIRendererRequest *request = fixture();
    SGIRenderer *renderer;
    Error *err = NULL;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    corrupt_clip = true;
    sgi_renderer_register_gl(&response_ops);
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_cmpuint(draw_count, ==, 1);
    error_free(err);
    sgi_renderer_free(renderer);
}

static void test_input_vertices(void)
{
    g_autofree SGIRendererRequest *request = fixture();
    Error *err = NULL;
    SGIRenderer *renderer;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    corrupt_vertices = true;
    sgi_renderer_register_gl(&response_ops);
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_nonnull(err);
    error_free(err);
    sgi_renderer_free(renderer);
}

static void test_input_attributes(gconstpointer opaque)
{
    g_autofree SGIRendererRequest *request = fixture();
    Error *err = NULL;
    SGIRenderer *renderer;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    request->smooth = true;
    for (unsigned i = 0; i < 3; i++) {
        request->colour[i][0] = request->colour[i][3] = 1;
        request->inverse_w[i] = 1;
    }
    corrupt_attributes = GPOINTER_TO_UINT(opaque);
    sgi_renderer_register_gl(&response_ops);
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_cmpuint(draw_count, ==, 1);
    error_free(err);
    sgi_renderer_free(renderer);
}

static unsigned batch_failure;

static bool batch_draw(void *opaque, SGIRendererRequest *request, Error **errp)
{
    g_autofree SGIRendererRequest *expected = fixture();

    draw_count++;
    if (draw_count == 2 && batch_failure == 1) {
        error_setg(errp, "Injected second-request failure");
        return false;
    }
    for (unsigned offset = 0; offset < SGI_RENDERER_BYTES; offset += 4) {
        if (expected->rgba[offset + 3]) {
            memcpy(expected->rgba + offset, request->rgb, sizeof(request->rgb));
        }
    }
    memcpy(request->rgba, expected->rgba, sizeof(request->rgba));
    if (draw_count == 2 && batch_failure == 2) {
        request->rgba[3] = 255;
    }
    return true;
}

static const SGIRendererOps batch_ops = {
    .create = generation_create,
    .draw = batch_draw,
    .destroy = generation_destroy,
};

static void test_batch(gconstpointer opaque)
{
    unsigned failure = GPOINTER_TO_UINT(opaque);
    g_autofree SGIRendererRequest *source = fixture();
    g_autofree SGIRendererRequest *requests = g_new0(SGIRendererRequest, 2);
    Error *err = NULL;
    SGIRenderer *renderer;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    requests[0] = requests[1] = *source;
    requests[1].rgb[0] = 0;
    requests[1].rgb[1] = 255;
    if (failure == 3) {
        requests[1].clip[0] = -1;
    }
    batch_failure = failure;
    sgi_renderer_register_gl(&batch_ops);
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_cmpint(sgi_renderer_draw_batch(renderer, requests, 2, &err), ==,
                    !failure);
    if (!failure) {
        g_assert_null(err);
        g_assert_true(sgi_renderer_staging_valid(&requests[0]));
        g_assert_true(sgi_renderer_staging_valid(&requests[1]));
        g_assert_cmpuint(requests[0].rgba[(16 * 128 + 16) * 4], ==, 255);
        g_assert_cmpuint(requests[1].rgba[(16 * 128 + 16) * 4 + 1], ==, 255);
    } else {
        g_assert_nonnull(err);
        error_free(err);
        err = NULL;
        if (failure == 3) {
            g_assert_cmpuint(draw_count, ==, 0);
            requests[1].clip[0] = 0;
            g_assert_true(sgi_renderer_draw_batch(renderer, requests, 2, &err));
            g_assert_null(err);
        } else {
            g_assert_true(sgi_renderer_staging_valid(&requests[0]));
            g_assert_false(sgi_renderer_draw_batch(renderer, requests, 2,
                                                   &err));
            g_assert_nonnull(err);
            error_free(err);
        }
    }
    g_assert_cmpuint(draw_count, ==, 2);
    sgi_renderer_free(renderer);
}

static bool test_draw(void *opaque, SGIRendererRequest *request, Error **errp)
{
    draw_count++;
    return true;
}

static bool test_corrupt_draw(void *opaque, SGIRendererRequest *request,
                              Error **errp)
{
    draw_count++;
    request->rgba[0] = 1;
    return true;
}

static const SGIRendererOps test_ops = {
    .create = generation_create,
    .draw = test_draw,
    .destroy = generation_destroy,
};

static const SGIRendererOps test_corrupt_ops = {
    .create = generation_create,
    .draw = test_corrupt_draw,
    .destroy = generation_destroy,
};

static void test_instance_draw_failure(void)
{
    g_autofree SGIRendererRequest *request = fixture();
    SGIRenderer *renderer, *other;
    Error *err = NULL;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    draw_count = 0;
    sgi_renderer_register_gl(&test_ops);
    renderer = sgi_renderer_new(&err);
    other = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_nonnull(other);
    g_assert_null(err);
    sgi_renderer_test_fail_draw(renderer, 2);

    g_assert_true(sgi_renderer_draw(other, request, &err));
    g_assert_true(sgi_renderer_draw(renderer, request, &err));
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "Injected SGI renderer test draw failure");
    g_assert_cmpuint(draw_count, ==, 3);
    g_assert_true(sgi_renderer_staging_valid(request));
    error_free(err);
    err = NULL;

    sgi_renderer_test_fail_draw(renderer, 0);
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_nonnull(err);
    g_assert_cmpuint(draw_count, ==, 3);
    error_free(err);
    err = NULL;
    g_assert_true(sgi_renderer_draw(other, request, &err));
    g_assert_null(err);
    g_assert_cmpuint(draw_count, ==, 4);
    sgi_renderer_free(renderer);
    sgi_renderer_free(other);
}

static void test_draw_failure_order(void)
{
    g_autofree SGIRendererRequest *request = fixture();
    SGIRenderer *renderer;
    Error *err = NULL;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    draw_count = 0;
    sgi_renderer_register_gl(&test_ops);
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    sgi_renderer_test_fail_draw(renderer, 1);
    request->clip[0] = -1;
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_nonnull(err);
    g_assert_cmpuint(draw_count, ==, 0);
    error_free(err);
    err = NULL;
    request->clip[0] = 0;
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "Injected SGI renderer test draw failure");
    g_assert_cmpuint(draw_count, ==, 1);
    error_free(err);
    sgi_renderer_free(renderer);
}

static void test_draw_failure_staging_precedence(void)
{
    g_autofree SGIRendererRequest *request = fixture();
    SGIRenderer *renderer;
    Error *err = NULL;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    draw_count = 0;
    sgi_renderer_register_gl(&test_corrupt_ops);
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    sgi_renderer_test_fail_draw(renderer, 1);
    g_assert_false(sgi_renderer_draw(renderer, request, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "Invalid SGI OpenGL renderer staging");
    g_assert_cmpuint(draw_count, ==, 1);
    error_free(err);
    sgi_renderer_free(renderer);
}

static bool frame_callback_fails;

static bool test_draw_frame(void *opaque, SGIRendererFrame *frame,
                            Error **errp)
{
    draw_count++;
    if (frame_callback_fails) {
        error_setg(errp, "Test frame backend failed");
        return false;
    }
    frame->written[0] = 1;
    return true;
}

static const SGIRendererOps frame_ops = {
    .create = generation_create,
    .draw = generation_draw,
    .draw_frame = test_draw_frame,
    .destroy = generation_destroy,
};

static void test_frame_failure(void)
{
    float vertices[3 * SGI_RENDERER_FRAME_VERTEX_FLOATS] = { 0 };
    uint16_t rgba[4] = { 0 };
    uint8_t written[1] = { 0 };
    SGIRendererFrame frame = {
        .width = 1, .height = 1, .count = 3, .vertices = vertices,
        .rgba = rgba, .written = written, .written_capacity = 1,
    };
    SGIRenderer *renderer;
    Error *err = NULL;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    draw_count = 0;
    sgi_renderer_register_gl(&frame_ops);
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    sgi_renderer_test_fail_draw(renderer, 1);
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "Injected SGI renderer test draw failure");
    g_assert_cmpuint(draw_count, ==, 1);
    g_assert_cmpuint(written[0], ==, 1);
    error_free(err);
    err = NULL;
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "SGI OpenGL renderer has failed");
    g_assert_cmpuint(draw_count, ==, 1);
    error_free(err);
    sgi_renderer_free(renderer);

    frame_callback_fails = true;
    written[0] = 0;
    draw_count = 0;
    err = NULL;
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    sgi_renderer_test_fail_draw(renderer, 1);
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "Test frame backend failed");
    g_assert_cmpuint(draw_count, ==, 1);
    g_assert_cmpuint(written[0], ==, 0);
    g_clear_pointer(&err, error_free);
    g_assert_false(sgi_renderer_draw_frame(renderer, &frame, &err));
    g_assert_nonnull(err);
    g_assert_cmpstr(error_get_pretty(err), ==,
                    "SGI OpenGL renderer has failed");
    g_assert_cmpuint(draw_count, ==, 1);
    g_clear_pointer(&err, error_free);
    sgi_renderer_free(renderer);
    frame_callback_fails = false;
}

static SGIRenderCreateResult create_result;
static bool create_return_pointer;

static void *result_create(SGIRenderCreateResult *result, Error **errp)
{
    *result = create_result;
    if (!create_return_pointer) {
        error_setg(errp, "Test backend creation failed");
        return NULL;
    }
    return &draw_count;
}

static const SGIRendererOps result_ops = {
    .create = result_create,
    .draw = test_draw,
    .destroy = generation_destroy,
};

static void test_create_result(gconstpointer opaque)
{
    SGIRenderer *renderer = NULL;
    SGIRenderCreateResult expected = GPOINTER_TO_INT(opaque);
    Error *err = NULL;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    create_result = expected;
    create_return_pointer = expected == SGI_RENDER_CREATE_OK;
    sgi_renderer_register_gl(&result_ops);
    g_assert_cmpint(sgi_renderer_create(&renderer, &err), ==, expected);
    if (expected == SGI_RENDER_CREATE_OK) {
        g_assert_nonnull(renderer);
        g_assert_null(err);
        sgi_renderer_free(renderer);
    } else {
        g_assert_null(renderer);
        g_assert_nonnull(err);
        error_free(err);
    }
}

static void test_create_unclassified_failure(void)
{
    SGIRenderer *renderer = NULL;
    Error *err = NULL;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    create_result = SGI_RENDER_CREATE_OK;
    create_return_pointer = false;
    sgi_renderer_register_gl(&result_ops);
    g_assert_cmpint(sgi_renderer_create(&renderer, &err), ==,
                    SGI_RENDER_CREATE_HOST_ERROR);
    g_assert_null(renderer);
    g_assert_nonnull(err);
    error_free(err);
}

#ifdef CONFIG_MODULES
static bool test_opengl_module_installed(void)
{
    g_autofree char *name = g_strdup_printf("ui-opengl%s", CONFIG_HOST_DSOSUF);
    g_autofree char *dir = get_relocated_path(CONFIG_QEMU_MODDIR);
    g_autofree char *path = g_build_filename(dir, name, NULL);

    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        return true;
    }
#ifdef CONFIG_MODULE_UPGRADES
    g_autofree char *version = g_strcanon(g_strdup(QEMU_PKGVERSION),
        G_CSET_A_2_Z G_CSET_a_2_z G_CSET_DIGITS "+-.~", '_');
    g_autofree char *upgrade_path =
        g_build_filename("/var/run/qemu", version, name, NULL);

    return g_file_test(upgrade_path, G_FILE_TEST_EXISTS);
#else
    return false;
#endif
}

static void test_module_create_result(gconstpointer opaque)
{
    bool bad_module = GPOINTER_TO_UINT(opaque);
    g_autofree char *directory = NULL;
    g_autofree char *module_name = NULL;
    g_autofree char *module_path = NULL;
    SGIRenderer *renderer = (void *)1;
    GError *glib_error = NULL;
    Error *err = NULL;

    if (!g_module_supported()) {
        g_test_skip("GLib module loading is unavailable");
        return;
    }
    if (!bad_module && test_opengl_module_installed()) {
        g_test_skip("An OpenGL module is installed outside the test directory");
        return;
    }
    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_passed();
        return;
    }
    directory = g_dir_make_tmp("sgi-renderer-module-XXXXXX", &glib_error);
    g_assert_no_error(glib_error);
    g_assert_nonnull(directory);
    module_name = g_strdup_printf("ui-opengl%s", CONFIG_HOST_DSOSUF);
    module_path = g_build_filename(directory, module_name, NULL);
    if (bad_module) {
        g_assert_true(g_file_set_contents(module_path, "not a module", -1,
                                          &glib_error));
        g_assert_no_error(glib_error);
    }
    g_setenv("QEMU_MODULE_DIR", directory, true);
    if (!bad_module) {
        g_assert_cmpint(ui_module_load("opengl", &err), ==, 0);
        g_assert_null(err);
    }
    g_assert_cmpint(sgi_renderer_create(&renderer, &err), ==,
                    bad_module ? SGI_RENDER_CREATE_HOST_ERROR :
                                 SGI_RENDER_CREATE_UNAVAILABLE);
    g_assert_null(renderer);
    g_assert_nonnull(err);
    if (bad_module) {
        g_assert_true(g_str_has_prefix(error_get_pretty(err),
                                       "failed to open module:"));
    } else {
        g_assert_cmpstr(error_get_pretty(err), ==,
                        "SGI OpenGL renderer is unavailable");
    }
    error_free(err);
    if (bad_module) {
        g_assert_cmpint(g_unlink(module_path), ==, 0);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}
#endif

int main(int argc, char **argv)
{
#ifdef CONFIG_MODULES
    qemu_init_exec_dir(argv[0]);
#endif
    g_test_init(&argc, &argv, NULL);
    for (unsigned mode = 0; mode < 4; mode++) {
        g_autofree char *path = g_strdup_printf("/sgi-renderer/smooth/%u",
                                                mode);

        g_test_add_data_func(path, GUINT_TO_POINTER(mode), test_smooth);
    }
    g_test_add_func("/sgi-renderer/smooth/invalid", test_smooth_invalid);
    g_test_add_func("/sgi-renderer/staging", test_staging);
    g_test_add_func("/sgi-renderer/clip", test_clip);
    g_test_add_func("/sgi-renderer/empty-clip", test_empty_clip);
    g_test_add_func("/sgi-renderer/uncovered", test_uncovered);
    g_test_add_func("/sgi-renderer/colour", test_colour);
    g_test_add_func("/sgi-renderer/request", test_request);
    g_test_add_func("/sgi-renderer/triangle-coverage", test_triangle_coverage);
    g_test_add_func("/sgi-renderer/large-coordinates", test_large_coordinates);
    g_test_add_data_func("/sgi-renderer/tiles/full", GUINT_TO_POINTER(0),
                         test_tiles);
    g_test_add_data_func("/sgi-renderer/tiles/scissor", GUINT_TO_POINTER(1),
                         test_tiles);
    g_test_add_data_func("/sgi-renderer/tiles/partial", GUINT_TO_POINTER(2),
                         test_tiles);
    g_test_add_func("/sgi-renderer/tiles/invalid", test_tile_invalid);
    g_test_add_func("/sgi-renderer/culling", test_culling);
#if !defined(CONFIG_OPENGL) && !defined(CONFIG_MODULES)
    g_test_add_func("/sgi-renderer/missing-backend", test_missing_backend);
#endif
#if defined(CONFIG_OPENGL) && !defined(CONFIG_MODULES)
    g_test_add_func("/sgi-renderer/create/missing-registration",
                    test_missing_registration);
#endif
    g_test_add_func("/sgi-renderer/generation", test_generation);
    g_test_add_func("/sgi-renderer/input-colour", test_input_colour);
    g_test_add_func("/sgi-renderer/input-clip", test_input_clip);
    g_test_add_func("/sgi-renderer/input-vertices", test_input_vertices);
    for (unsigned input = 0; input < 4; input++) {
        g_autofree char *path = g_strdup_printf(
            "/sgi-renderer/input-attributes/%u", input);

        g_test_add_data_func(path, GUINT_TO_POINTER(input),
                             test_input_attributes);
    }
    static const char *batch_cases[] = {
        "success", "second-backend-failure", "second-staging-failure",
        "late-invalid-input",
    };
    for (unsigned failure = 0; failure < ARRAY_SIZE(batch_cases); failure++) {
        g_autofree char *path = g_strdup_printf("/sgi-renderer/batch/%s",
                                                batch_cases[failure]);

        g_test_add_data_func(path, GUINT_TO_POINTER(failure), test_batch);
    }
    g_test_add_func("/sgi-renderer/test-fault/instance",
                    test_instance_draw_failure);
    g_test_add_func("/sgi-renderer/test-fault/order",
                    test_draw_failure_order);
    g_test_add_func("/sgi-renderer/test-fault/staging-precedence",
                    test_draw_failure_staging_precedence);
    g_test_add_func("/sgi-renderer/test-fault/frame",
                    test_frame_failure);
    g_test_add_data_func("/sgi-renderer/create/success",
                         GINT_TO_POINTER(SGI_RENDER_CREATE_OK),
                         test_create_result);
    g_test_add_data_func("/sgi-renderer/create/unavailable",
                         GINT_TO_POINTER(SGI_RENDER_CREATE_UNAVAILABLE),
                         test_create_result);
    g_test_add_data_func("/sgi-renderer/create/oom",
                         GINT_TO_POINTER(SGI_RENDER_CREATE_OOM),
                         test_create_result);
    g_test_add_data_func("/sgi-renderer/create/host-error",
                         GINT_TO_POINTER(SGI_RENDER_CREATE_HOST_ERROR),
                         test_create_result);
    g_test_add_func("/sgi-renderer/create/unclassified",
                    test_create_unclassified_failure);
#ifdef CONFIG_MODULES
    g_test_add_data_func("/sgi-renderer/create/module-absent",
                         GUINT_TO_POINTER(0), test_module_create_result);
    g_test_add_data_func("/sgi-renderer/create/module-load-error",
                         GUINT_TO_POINTER(1), test_module_create_result);
#endif
    return g_test_run();
}
