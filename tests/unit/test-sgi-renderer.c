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

    request->vertices[1][0] = 112;
    g_assert_true(sgi_renderer_request_valid(request));
    request->rgb[0] = 127;
    g_assert_true(sgi_renderer_request_valid(request));
    request->vertices[1][0] = NAN;
    g_assert_false(sgi_renderer_request_valid(request));
    request->vertices[1][0] = 128.25f;
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
    if (corrupt_vertices) {
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
    g_test_add_func("/sgi-renderer/staging", test_staging);
    g_test_add_func("/sgi-renderer/clip", test_clip);
    g_test_add_func("/sgi-renderer/uncovered", test_uncovered);
    g_test_add_func("/sgi-renderer/colour", test_colour);
    g_test_add_func("/sgi-renderer/request", test_request);
    g_test_add_func("/sgi-renderer/triangle-coverage", test_triangle_coverage);
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
