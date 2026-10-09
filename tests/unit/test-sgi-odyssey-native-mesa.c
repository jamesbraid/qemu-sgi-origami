/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/sgi-odyssey-native-mesa.h"
#ifdef CONFIG_OPENGL
#include "qemu/module.h"
#include "ui/egl-helpers.h"
#include "ui/sgi-renderer.h"

int display_opengl;

void qemu_gl_run_texture_blit(QemuGLShader *shader, bool flip)
{
    g_assert_not_reached();
}

static void require_llvmpipe(void)
{
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE,
    };
    const EGLint context_attrs[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK,
        EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_NONE,
    };
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform_display;
    EGLDisplay display;
    EGLContext context;
    EGLConfig config;
    EGLint nconfigs;
    const char *renderer;

    platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_assert_nonnull(platform_display);
    display = platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                               EGL_DEFAULT_DISPLAY, NULL);
    g_assert_true(display != EGL_NO_DISPLAY);
    g_assert_true(eglInitialize(display, NULL, NULL));
    g_assert_true(eglBindAPI(EGL_OPENGL_API));
    g_assert_true(eglChooseConfig(display, config_attrs, &config, 1,
                                   &nconfigs));
    g_assert_cmpint(nconfigs, >, 0);
    context = eglCreateContext(display, config, EGL_NO_CONTEXT,
                               context_attrs);
    g_assert_true(context != EGL_NO_CONTEXT);
    g_assert_true(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                context));
    renderer = (const char *)glGetString(GL_RENDERER);
    g_assert_nonnull(renderer);
    g_test_message("Mesa renderer: %s", renderer);
    g_assert_nonnull(strstr(renderer, "llvmpipe"));
    g_assert_true(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                EGL_NO_CONTEXT));
    g_assert_true(eglDestroyContext(display, context));
    g_assert_true(eglTerminate(display));
}
#endif

typedef struct NativeFixture {
    SGIBoardMemory memory[2];
    const SGIPlane *plane[2];
    const SGIPlane *tag[2];
    OdysseyNativeMesa native;
    unsigned invalidations;
    unsigned stores;
    unsigned completed_stores;
    unsigned fail_store;
    OdysseyNativeStoreResult failure;
} NativeFixture;

static void invalidate_scanout_all(void *opaque)
{
    NativeFixture *f = opaque;

    f->invalidations++;
}

static void native_fixture_init(NativeFixture *f)
{
    SGIPlane plane = {
        .format = SGI_PLANE_RGBA8,
        .packing = SGI_PACK_BE_RED_HIGH,
        .width = 4, .height = 4, .stride = 16,
        .len = 64, .bottom_origin = false,
    };
    SGIPlane tag = {
        .base = 64, .len = 16,
        .format = SGI_PLANE_TAG,
        .packing = SGI_PACK_BE_RED_HIGH,
        .width = 4, .height = 4, .stride = 4,
    };
    Error *err = NULL;

    memset(f, 0, sizeof(*f));
    for (unsigned b = 0; b < 2; b++) {
        uint32_t id;

        sgi_board_memory_init(&f->memory[b], 80);
        id = sgi_board_memory_add_plane(&f->memory[b], &plane, &error_abort);
        f->plane[b] = sgi_board_memory_plane(&f->memory[b], id);
        id = sgi_board_memory_add_plane(&f->memory[b], &tag, &error_abort);
        f->tag[b] = sgi_board_memory_plane(&f->memory[b], id);
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                sgi_plane_store(&f->memory[b], f->plane[b], x, y,
                                0x10203040 + b * 0x01010101 + y * 16 + x);
                sgi_plane_store(&f->memory[b], f->tag[b], x, y,
                                0x40 + b * 0x20 + y * 4 + x);
            }
        }
    }
    g_assert_true(odyssey_native_mesa_bind(&f->native,
                  &f->memory[0], f->plane[0],
                  &f->memory[1], f->plane[1],
                  invalidate_scanout_all, f, &err));
    g_assert_null(err);
}

static void native_fixture_cleanup(NativeFixture *f)
{
    for (unsigned b = 0; b < 2; b++) {
        sgi_board_memory_cleanup(&f->memory[b]);
    }
}

static void assert_tag_sentinels(const NativeFixture *f)
{
    for (unsigned b = 0; b < 2; b++) {
        for (unsigned y = 0; y < 4; y++) {
            for (unsigned x = 0; x < 4; x++) {
                g_assert_cmphex(sgi_plane_load(&f->memory[b], f->tag[b],
                                x, y), ==, 0x40 + b * 0x20 + y * 4 + x);
            }
        }
    }
}

static OdysseyNativeStoreResult fault_store(void *opaque,
                                            SGIBoardMemory *memory,
                                            const SGIPlane *plane,
                                            uint32_t x, uint32_t y,
                                            uint32_t word)
{
    NativeFixture *f = opaque;

    f->stores++;
    if (f->stores == f->fail_store) {
        if (f->failure == ODYSSEY_NATIVE_STORE_PARTIAL ||
            f->failure == ODYSSEY_NATIVE_STORE_UNKNOWN) {
            memory->bytes[y * plane->stride + x * 4] = 0xa5;
        }
        return f->failure;
    }
    sgi_plane_store(memory, plane, x, y, word);
    f->completed_stores++;
    return ODYSSEY_NATIVE_STORE_OK;
}

static void test_native_snapshot_and_known_prefix(void)
{
    NativeFixture f;
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 3, 3 };
    uint32_t slots[64];
    uint8_t mask[16] = { [1] = 1, [2] = 1 };
    SGIRenderWriteCoverage writes = {
        .pixel = mask, .stride_bytes = 4, .capacity_bytes = sizeof(mask),
    };
    SGIRenderCommitResult result;
    Error *err = NULL;

    native_fixture_init(&f);
    target = odyssey_native_mesa_target(&f.native);
    g_assert_cmpint(sgi_render_resource_snapshot(&target,
                    &f.native.resources[0], 0, &rect, slots, 16,
                    ARRAY_SIZE(slots), &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmphex(slots[0], ==, 0x10);
    g_assert_cmphex(slots[3], ==, 0x40);
    g_assert_cmphex(slots[60], ==, 0x10);
    g_assert_cmphex(slots[61], ==, 0x20);
    g_assert_cmphex(slots[62], ==, 0x30);
    g_assert_cmphex(slots[63], ==, 0x73);
    for (unsigned i = 0; i < 16; i++) {
        slots[i * 4] = 0xff;
        slots[i * 4 + 1] = 0;
        slots[i * 4 + 2] = 0;
        slots[i * 4 + 3] = 0xff;
    }
    f.native.store = fault_store;
    f.native.store_opaque = &f;
    f.fail_store = 2;
    f.failure = ODYSSEY_NATIVE_STORE_UNCHANGED;
    result = sgi_render_resource_commit_covered(&target,
             &f.native.resources[0], 0, &rect, slots, 16,
             ARRAY_SIZE(slots), &writes, &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(result.complete_pixels, ==, 2);
    g_assert_true(result.extent_known);
    g_assert_false(result.partial_pixel);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmphex(sgi_plane_load(&f.memory[0], f.plane[0], 1, 0), ==,
                    0xff0000ff);
    g_assert_cmphex(sgi_plane_load(&f.memory[0], f.plane[0], 2, 0), ==,
                    0x10203042);
    g_assert_cmphex(sgi_plane_load(&f.memory[1], f.plane[1], 1, 0), ==,
                    0x11213142);
    g_assert_cmpuint(f.invalidations, ==, 1);
    native_fixture_cleanup(&f);
}

static void test_native_unknown_extent(void)
{
    NativeFixture f;
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 3, 3 };
    uint32_t slots[64] = { 0 };
    uint8_t mask[16] = { [1] = 1 };
    SGIRenderWriteCoverage writes = {
        .pixel = mask, .stride_bytes = 4, .capacity_bytes = sizeof(mask),
    };
    SGIRenderCommitResult result;
    Error *err = NULL;

    native_fixture_init(&f);
    target = odyssey_native_mesa_target(&f.native);
    f.native.store = fault_store;
    f.native.store_opaque = &f;
    f.fail_store = 1;
    f.failure = ODYSSEY_NATIVE_STORE_UNKNOWN;
    result = sgi_render_resource_commit_covered(&target,
             &f.native.resources[0], 0, &rect, slots, 16,
             ARRAY_SIZE(slots), &writes, &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_false(result.extent_known);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.invalidations, ==, 1);
    g_assert_cmphex(f.memory[0].bytes[4], ==, 0xa5);
    native_fixture_cleanup(&f);
}

static void test_native_unchanged_failure(void)
{
    NativeFixture f;
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 3, 3 };
    uint32_t slots[64] = { 0 };
    uint8_t mask[16] = { [1] = 1 };
    SGIRenderWriteCoverage writes = {
        .pixel = mask, .stride_bytes = 4, .capacity_bytes = sizeof(mask),
    };
    SGIRenderCommitResult result;
    uint8_t before[64];
    Error *err = NULL;

    native_fixture_init(&f);
    memcpy(before, f.memory[0].bytes, sizeof(before));
    target = odyssey_native_mesa_target(&f.native);
    f.native.store = fault_store;
    f.native.store_opaque = &f;
    f.fail_store = 1;
    f.failure = ODYSSEY_NATIVE_STORE_UNCHANGED;
    result = sgi_render_resource_commit_covered(&target,
             &f.native.resources[0], 0, &rect, slots, 16,
             ARRAY_SIZE(slots), &writes, &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(result.complete_pixels, ==, 1);
    g_assert_true(result.extent_known);
    g_assert_false(result.partial_pixel);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpmem(f.memory[0].bytes, sizeof(before), before,
                    sizeof(before));
    g_assert_cmpuint(f.invalidations, ==, 0);
    native_fixture_cleanup(&f);
}

static void test_native_reject_alias(void)
{
    NativeFixture f;
    SGIBoardMemory alias;
    OdysseyNativeMesa native;
    Error *err = NULL;

    native_fixture_init(&f);
    alias = f.memory[0];
    g_assert_false(odyssey_native_mesa_bind(&native,
                   &f.memory[0], f.plane[0], &alias, &alias.planes[0],
                   invalidate_scanout_all, &f, &err));
    g_assert_nonnull(err);
    error_free(err);
    native_fixture_cleanup(&f);
}

#ifdef CONFIG_OPENGL
/*
 * The rules the device draws under (odyssey_gl_operation_rules() in RGBA
 * mode), which the shared Mesa renderer requires.
 */
static const SGIRasterRules native_rules = {
    .subpixel_bits = 8,
    .color_bits = 8,
    .alpha_bits = 8,
    .depth_bits = 24,
    .rounding = SGI_ROUND_HALF_AWAY,
    .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
    .arith = SGI_ARITH_DOUBLE,
    .line_rule = SGI_LINE_COVERAGE_DOUBLE,
    .evidence = "model",
};

static void test_native_mesa_clip_scissor(void)
{
    NativeFixture f;
    SGIRasterRules rules = native_rules;
    SGIGLDrawState state = {
        .shade_model = SGI_GL_FLAT,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .drawable = {
            .bound = true, .front = 0,
            .clip = { .n = 1, .r[0] = { 0, 0, 3, 0 } },
        },
        .viewport = { 0, 0, 4, 4 },
        .scissor_test = 1, .scissor = { 1, 3, 2, 1 },
    };
    static const float xy[3][2] = {
        { -1, -1 }, { 3, -1 }, { -1, 3 },
    };
    float vertices[3][SGI_GL_VERTEX_FLOATS] = { 0 };
    SGIGLDraw draw = {
        .mode = SGI_GL_TRIANGLES, .count = 3,
        .vertices = &vertices[0][0], .state = &state,
    };
    SGIRenderer *renderer;
    uint8_t before[64];
    Error *err = NULL;

    if (!g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
        g_test_skip("Set QEMU_TEST_SGI_REQUIRE_MESA for native Mesa draw");
        return;
    }
    g_setenv("LIBGL_ALWAYS_SOFTWARE", "1", true);
    g_setenv("GALLIUM_DRIVER", "llvmpipe", true);
    require_llvmpipe();
    native_fixture_init(&f);
    memcpy(before, f.memory[0].bytes, sizeof(before));
    for (unsigned i = 0; i < 4; i++) {
        state.modelview[i * 5] = 1;
        state.projection[i * 5] = 1;
    }
    for (unsigned i = 0; i < 3; i++) {
        vertices[i][SGI_GL_V_POSITION] = xy[i][0];
        vertices[i][SGI_GL_V_POSITION + 1] = xy[i][1];
        vertices[i][SGI_GL_V_POSITION + 3] = 1;
        vertices[i][SGI_GL_V_COLOR] = 1;
        vertices[i][SGI_GL_V_COLOR + 3] = 1;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_cmpint(odyssey_native_mesa_draw(&f.native, renderer, &rules,
                    &draw, 1, SGI_RENDER_MONO, false, &err), ==,
                    SGI_RENDER_OK);
    g_assert_null(err);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            unsigned at = (y * 4 + x) * 4;

            if (y == 0 && (x == 1 || x == 2)) {
                g_assert_cmphex(sgi_plane_load(&f.memory[0], f.plane[0],
                                x, y), ==, 0xff0000ff);
            } else {
                g_assert_cmpmem(f.memory[0].bytes + at, 4, before + at, 4);
            }
        }
    }
    g_assert_cmpuint(f.invalidations, ==, 1);
    memcpy(before, f.memory[0].bytes, sizeof(before));
    for (unsigned rejected = 0; rejected < 3; rejected++) {
        uint64_t saved_size = f.memory[0].size;

        /* An attempted snapshot would report a resource failure here. */
        f.memory[0].size = 0;
        g_assert_cmpint(odyssey_native_mesa_draw(&f.native, renderer, &rules,
                        &draw, rejected == 0 ? 2 : 1,
                        rejected == 1 ? SGI_RENDER_LEFT : SGI_RENDER_MONO,
                        rejected == 2, &err), ==,
                        SGI_RENDER_UNSUPPORTED);
        f.memory[0].size = saved_size;
        g_assert_nonnull(err);
        error_free(err);
        err = NULL;
        g_assert_cmpmem(f.memory[0].bytes, sizeof(before), before,
                        sizeof(before));
        g_assert_cmpuint(f.invalidations, ==, 1);
    }
    sgi_renderer_free(renderer);
    native_fixture_cleanup(&f);
}

static void test_native_mesa_preserves_tags(void)
{
    NativeFixture f;
    SGIRasterRules rules = native_rules;
    SGIGLDrawState state = {
        .shade_model = SGI_GL_FLAT,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .drawable = {
            .bound = true, .front = 0,
            .clip = { .n = 1, .r[0] = { 0, 0, 3, 0 } },
        },
        .viewport = { 0, 0, 4, 4 },
        .scissor_test = 1, .scissor = { 1, 3, 2, 1 },
    };
    static const float xy[3][2] = {
        { -1, -1 }, { 3, -1 }, { -1, 3 },
    };
    float vertices[3][SGI_GL_VERTEX_FLOATS] = { 0 };
    SGIGLDraw draw = {
        .mode = SGI_GL_TRIANGLES, .count = 3,
        .vertices = &vertices[0][0], .state = &state,
    };
    SGIRenderer *renderer;
    uint8_t colour_before[2][64];
    Error *err = NULL;

    if (!g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
        g_test_skip("Set QEMU_TEST_SGI_REQUIRE_MESA for native Mesa draw");
        return;
    }
    g_setenv("LIBGL_ALWAYS_SOFTWARE", "1", true);
    g_setenv("GALLIUM_DRIVER", "llvmpipe", true);
    require_llvmpipe();
    native_fixture_init(&f);
    assert_tag_sentinels(&f);
    f.native.store = fault_store;
    f.native.store_opaque = &f;
    for (unsigned b = 0; b < 2; b++) {
        memcpy(colour_before[b], f.memory[b].bytes, sizeof(colour_before[b]));
    }
    for (unsigned i = 0; i < 4; i++) {
        state.modelview[i * 5] = 1;
        state.projection[i * 5] = 1;
    }
    for (unsigned i = 0; i < 3; i++) {
        vertices[i][SGI_GL_V_POSITION] = xy[i][0];
        vertices[i][SGI_GL_V_POSITION + 1] = xy[i][1];
        vertices[i][SGI_GL_V_POSITION + 3] = 1;
        vertices[i][SGI_GL_V_COLOR] = 1;
        vertices[i][SGI_GL_V_COLOR + 3] = 1;
    }
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);

    g_assert_cmpint(odyssey_native_mesa_draw(&f.native, renderer, &rules,
                    &draw, 1, SGI_RENDER_MONO, false, &err), ==,
                    SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.stores, ==, 2);
    g_assert_cmpuint(f.completed_stores, ==, 2);
    g_assert_cmpuint(f.invalidations, ==, 1);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            unsigned at = (y * 4 + x) * 4;

            if (y == 0 && (x == 1 || x == 2)) {
                g_assert_cmphex(sgi_plane_load(&f.memory[0], f.plane[0],
                                x, y), ==, 0xff0000ff);
            } else {
                g_assert_cmpmem(f.memory[0].bytes + at, 4,
                                colour_before[0] + at, 4);
            }
        }
    }
    g_assert_cmpmem(f.memory[1].bytes, sizeof(colour_before[1]),
                    colour_before[1], sizeof(colour_before[1]));
    assert_tag_sentinels(&f);

    memcpy(colour_before[0], f.memory[0].bytes, sizeof(colour_before[0]));
    for (unsigned i = 0; i < 3; i++) {
        vertices[i][SGI_GL_V_COLOR] = 0;
        vertices[i][SGI_GL_V_COLOR + 1] = 1;
    }
    f.stores = 0;
    f.completed_stores = 0;
    f.fail_store = 2;
    f.failure = ODYSSEY_NATIVE_STORE_UNCHANGED;
    g_assert_cmpint(odyssey_native_mesa_draw(&f.native, renderer, &rules,
                    &draw, 1, SGI_RENDER_MONO, false, &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.stores, ==, 2);
    g_assert_cmpuint(f.completed_stores, ==, 1);
    g_assert_cmpuint(f.invalidations, ==, 2);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            unsigned at = (y * 4 + x) * 4;

            if (y == 0 && x == 1) {
                g_assert_cmphex(sgi_plane_load(&f.memory[0], f.plane[0],
                                x, y), ==, 0x00ff00ff);
            } else {
                g_assert_cmpmem(f.memory[0].bytes + at, 4,
                                colour_before[0] + at, 4);
            }
        }
    }
    g_assert_cmpmem(f.memory[1].bytes, sizeof(colour_before[1]),
                    colour_before[1], sizeof(colour_before[1]));
    assert_tag_sentinels(&f);

    sgi_renderer_free(renderer);
    native_fixture_cleanup(&f);
}
#endif

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
#ifdef CONFIG_OPENGL
    module_call_init(MODULE_INIT_QOM);
#endif
    g_test_add_func("/sgi-odyssey-native/snapshot-known-prefix",
                    test_native_snapshot_and_known_prefix);
    g_test_add_func("/sgi-odyssey-native/unknown-extent",
                    test_native_unknown_extent);
    g_test_add_func("/sgi-odyssey-native/unchanged-failure",
                    test_native_unchanged_failure);
    g_test_add_func("/sgi-odyssey-native/reject-alias",
                    test_native_reject_alias);
#ifdef CONFIG_OPENGL
    g_test_add_func("/sgi-odyssey-native/mesa-clip-scissor",
                    test_native_mesa_clip_scissor);
    g_test_add_func("/sgi-odyssey-native/mesa-preserves-tags",
                    test_native_mesa_preserves_tags);
#endif
    return g_test_run();
}
