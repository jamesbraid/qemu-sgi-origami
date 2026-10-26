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
static SGIRenderRefusal last_reason;

/*
 * One attempt, returned as its status; the outcome's reason is kept in
 * last_reason. The attempt's result must agree with the status.
 */
static SGIRenderResult native_draw(OdysseyNativeMesa *native,
                                   SGIRenderer *renderer,
                                   const SGIRasterRules *rules,
                                   const SGIGLDraw *draw,
                                   unsigned selected_buffers,
                                   SGIRenderEye eye, bool compare_active,
                                   Error **errp)
{
    SGIRenderAttemptOutcome outcome = { .reason = SGI_RENDER_REFUSAL__MAX };
    SGIRenderDrawResult result;

    result = odyssey_native_mesa_draw(native, renderer, rules, draw,
                                      selected_buffers, eye, compare_active,
                                      &outcome, errp);
    last_reason = outcome.reason;
    if (outcome.status == SGI_RENDER_OK) {
        g_assert_cmpint(result, ==, SGI_RENDER_DRAW_OK);
        g_assert_cmpint(outcome.reason, ==, SGI_RENDER_REFUSAL_NONE);
    } else if (outcome.status == SGI_RENDER_UNSUPPORTED) {
        g_assert_cmpint(result, ==, SGI_RENDER_DRAW_REFUSED);
        g_assert_cmpint(outcome.reason, >, SGI_RENDER_REFUSAL_NONE);
        g_assert_cmpint(outcome.reason, <, SGI_RENDER_REFUSAL__MAX);
    } else {
        g_assert_cmpint(result, ==, SGI_RENDER_DRAW_FAILED);
    }
    return outcome.status;
}

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
        .scissor_test = 1, .scissor = { 1, 0, 2, 1 },
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
    g_assert_cmpint(native_draw(&f.native, renderer, &rules,
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
        g_assert_cmpint(native_draw(&f.native, renderer, &rules,
                        &draw, rejected == 0 ? 2 : 1,
                        rejected == 1 ? SGI_RENDER_LEFT : SGI_RENDER_MONO,
                        rejected == 2, &err), ==,
                        SGI_RENDER_UNSUPPORTED);
        g_assert_cmpint(last_reason, ==, rejected == 2 ?
                        SGI_RENDER_REFUSAL_WINDOW :
                        SGI_RENDER_REFUSAL_BUFFERS);
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
        .scissor_test = 1, .scissor = { 1, 0, 2, 1 },
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

    g_assert_cmpint(native_draw(&f.native, renderer, &rules,
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
    g_assert_cmpint(native_draw(&f.native, renderer, &rules,
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

typedef struct ScalarRows {
    uint8_t mask[4][4];
    unsigned dropped;
    /* Of those, the stores past the right edge on a plane row. */
    unsigned dropped_right;
} ScalarRows;

/*
 * The scalar rasterizer stores Odyssey window pixel (x, y) at plane pixel
 * (origin x + x, origin y + y): window row 0 is the drawable's top plane
 * row. Record which plane pixels it covers. The device drops a store
 * outside the plane (odyssey_pixel_in_bounds()), so count those instead.
 */
static void scalar_rows_store(void *opaque, SGIColourTarget target,
                              int x, int y, uint32_t colour)
{
    ScalarRows *rows = opaque;

    g_assert_cmpint(target, ==, 0);
    if (x < 0 || x >= 4 || y < 0 || y >= 4) {
        rows->dropped++;
        rows->dropped_right += x >= 4 && y >= 0 && y < 4;
        return;
    }
    rows->mask[y][x] = 1;
}

static void native_rows_draw_init(SGIGLDrawState *state, SGIGLDraw *draw,
                                  float vertices[3][SGI_GL_VERTEX_FLOATS],
                                  const float xy[3][2])
{
    memset(vertices, 0, sizeof(float) * 3 * SGI_GL_VERTEX_FLOATS);
    for (unsigned i = 0; i < 3; i++) {
        vertices[i][SGI_GL_V_POSITION] = xy[i][0];
        vertices[i][SGI_GL_V_POSITION + 1] = xy[i][1];
        vertices[i][SGI_GL_V_POSITION + 3] = 1;
        vertices[i][SGI_GL_V_COLOR] = 1;
        vertices[i][SGI_GL_V_COLOR + 3] = 1;
    }
    *draw = (SGIGLDraw) {
        .mode = SGI_GL_TRIANGLES, .count = 3,
        .vertices = &vertices[0][0], .state = state,
    };
}

static SGIGLDrawState native_rows_state(void)
{
    SGIGLDrawState state = {
        .shade_model = SGI_GL_FLAT,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .front_face = SGI_GL_CCW,
        .drawable = {
            .bound = true, .front = 0,
            .clip = { .n = 1, .r[0] = { 0, 0, 3, 3 } },
        },
        .viewport = { 0, 0, 4, 4 },
        /* Odyssey keeps the scissor on with its 0x81d box (window rows). */
        .scissor_test = 1, .scissor = { 0, 0, 4, 4 },
    };

    for (unsigned i = 0; i < 4; i++) {
        state.modelview[i * 5] = 1;
        state.projection[i * 5] = 1;
    }
    return state;
}

/*
 * Run one draw through the scalar rasterizer and through the adapter's
 * checked Mesa path, and require the same native plane rows from both.
 * dropped and dropped_right are how many stores the scalar path must drop
 * outside the plane, and how many of those lie past its right edge alone,
 * which proves which resource edges the case reaches past.
 */
static void native_rows_compare(const SGIGLDrawState *in,
                                const float xy[3][2],
                                const uint8_t expected[4][4],
                                unsigned dropped,
                                unsigned dropped_right)
{
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8, .depth_bits = 24,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .line_rule = SGI_LINE_COVERAGE_DOUBLE,
    };
    SGIGLDrawState state = *in;
    float vertices[3][SGI_GL_VERTEX_FLOATS];
    SGIGLDraw draw;
    ScalarRows rows = { 0 };
    uint8_t (*scalar)[4] = rows.mask;
    SGIRasterTarget scalar_target = {
        .store_colour = scalar_rows_store, .opaque = &rows,
    };
    NativeFixture f;
    SGIRenderer *renderer;
    uint8_t before[64];
    Error *err = NULL;
    /* The caller's RGBA rules, as odyssey_gl_operation_rules() gives them. */
    SGIRasterRules mesa_rules = rules;

    mesa_rules.alpha_bits = 8;
    g_assert_true(sgi_raster_rules_supported(&rules));
    native_rows_draw_init(&state, &draw, vertices, xy);
    g_assert_cmpint(sgi_soft_draw(&rules, &scalar_target, &draw), ==,
                    SGI_GL_DRAW_OK);
    for (unsigned y = 0; y < 4; y++) {
        g_test_message("scalar plane row %u: %u%u%u%u", y, scalar[y][0],
                       scalar[y][1], scalar[y][2], scalar[y][3]);
    }
    g_test_message("scalar stores dropped outside the plane: %u, "
                   "past the right edge only: %u", rows.dropped,
                   rows.dropped_right);
    g_assert_cmpmem(rows.mask, sizeof(rows.mask), expected,
                    sizeof(rows.mask));
    g_assert_cmpuint(rows.dropped, ==, dropped);
    g_assert_cmpuint(rows.dropped_right, ==, dropped_right);

    native_fixture_init(&f);
    memcpy(before, f.memory[0].bytes, sizeof(before));
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    g_assert_cmpint(native_draw(&f.native, renderer,
                    &mesa_rules, &draw, 1, SGI_RENDER_MONO, false, &err), ==,
                    SGI_RENDER_OK);
    g_assert_null(err);
    /* The adapter maps a submission-local copy; the guest's stays put. */
    g_assert_cmpmem(&state, sizeof(state), in, sizeof(state));
    for (unsigned y = 0; y < 4; y++) {
        unsigned at = y * 16;

        g_test_message("Mesa native row %u: %u%u%u%u", y,
                       memcmp(f.memory[0].bytes + at, before + at, 4) != 0,
                       memcmp(f.memory[0].bytes + at + 4,
                              before + at + 4, 4) != 0,
                       memcmp(f.memory[0].bytes + at + 8,
                              before + at + 8, 4) != 0,
                       memcmp(f.memory[0].bytes + at + 12,
                              before + at + 12, 4) != 0);
    }
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            unsigned at = (y * 4 + x) * 4;

            if (scalar[y][x]) {
                g_assert_cmphex(sgi_plane_load(&f.memory[0], f.plane[0],
                                x, y), ==, 0xff0000ff);
            } else {
                g_assert_cmpmem(f.memory[0].bytes + at, 4, before + at, 4);
            }
        }
    }
    assert_tag_sentinels(&f);
    sgi_renderer_free(renderer);
    native_fixture_cleanup(&f);
}

static void native_rows_match_scalar(const SGIGLDrawState *in,
                                     const float xy[3][2],
                                     const uint8_t expected[4][4])
{
    native_rows_compare(in, xy, expected, 0, 0);
}

static bool native_rows_require_mesa(void)
{
    if (!g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
        g_test_skip("Set QEMU_TEST_SGI_REQUIRE_MESA for native Mesa draw");
        return false;
    }
    g_setenv("LIBGL_ALWAYS_SOFTWARE", "1", true);
    g_setenv("GALLIUM_DRIVER", "llvmpipe", true);
    require_llvmpipe();
    return true;
}

/* An asymmetric triangle in window rows 0 and 1 of an offset viewport. */
static void test_native_rows_viewport(void)
{
    SGIGLDrawState state = native_rows_state();
    /* x_w = 4 * x_ndc + 2, y_w = 4 * y_ndc + 3. */
    static const float xy[3][2] = {
        { -0.475f, -0.7f }, { 0.45f, -0.7f }, { -0.475f, -0.075f },
    };
    static const uint8_t expected[4][4] = {
        { 1, 1, 1, 0 },
        { 1, 1, 0, 0 },
    };

    if (!native_rows_require_mesa()) {
        return;
    }
    state.viewport[0] = -2;
    state.viewport[1] = -1;
    state.viewport[2] = 8;
    state.viewport[3] = 8;
    native_rows_match_scalar(&state, xy, expected);
}

/*
 * The combined-matrix profile: the reach viewport centred on the window
 * origin, with a matrix that makes x_w and y_w the vertex's own x and y.
 */
static void test_native_rows_reach_viewport(void)
{
    SGIGLDrawState state = native_rows_state();
    static const float xy[3][2] = {
        { 0.25f, 0.25f }, { 3.25f, 0.25f }, { 0.25f, 2.25f },
    };
    static const uint8_t expected[4][4] = {
        { 1, 1, 1, 0 },
        { 1, 0, 0, 0 },
    };

    if (!native_rows_require_mesa()) {
        return;
    }
    state.viewport[0] = -8192;
    state.viewport[1] = -8192;
    state.viewport[2] = 16384;
    state.viewport[3] = 16384;
    state.projection[0] = 1.0f / 8192;
    state.projection[5] = 1.0f / 8192;
    native_rows_match_scalar(&state, xy, expected);
}

/* A clip of plane rows 0 and 1 and a scissor box naming the same rows. */
static void test_native_rows_clip_scissor(void)
{
    SGIGLDrawState state = native_rows_state();
    static const float xy[3][2] = {
        { -1, -1 }, { 3, -1 }, { -1, 3 },
    };
    static const uint8_t expected[4][4] = {
        { 1, 1, 1, 1 },
        { 1, 1, 1, 1 },
    };

    if (!native_rows_require_mesa()) {
        return;
    }
    state.drawable.clip.r[0] = (SGIClipRect) { 0, 0, 3, 1 };
    state.scissor[3] = 2;
    native_rows_match_scalar(&state, xy, expected);
}

/* The scissor box alone selects window row 0, columns 1 and 2. */
static void test_native_rows_scissor(void)
{
    SGIGLDrawState state = native_rows_state();
    static const float xy[3][2] = {
        { -1, -1 }, { 3, -1 }, { -1, 3 },
    };
    static const uint8_t expected[4][4] = {
        { 0, 1, 1, 0 },
    };

    if (!native_rows_require_mesa()) {
        return;
    }
    state.scissor[0] = 1;
    state.scissor[2] = 2;
    state.scissor[3] = 1;
    native_rows_match_scalar(&state, xy, expected);
}

/* The reach viewport, whose matrix makes x_w and y_w the vertex's x, y. */
static void native_rows_reach(SGIGLDrawState *state)
{
    state->viewport[0] = -8192;
    state->viewport[1] = -8192;
    state->viewport[2] = 16384;
    state->viewport[3] = 16384;
    state->projection[0] = 1.0f / 8192;
    state->projection[5] = 1.0f / 8192;
}

/*
 * A right triangle in window rows 0 to 2 at origin (1,2), under a clip that
 * crosses the plane's right and bottom edges. Window row 0 covers column 0,
 * row 1 columns 0 to 2 and row 2 columns 0 to 4; the scalar path drops what
 * lands past the plane, and the shared renderer must clamp the clip to
 * the target the same way.
 */
static void test_native_rows_origin_edge(void)
{
    SGIGLDrawState state = native_rows_state();
    static const float xy[3][2] = {
        { 0.25f, 0.25f }, { 0.25f, 2.75f }, { 5.25f, 2.75f },
    };
    static const uint8_t expected[4][4] = {
        { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },
        { 0, 1, 0, 0 },
        { 0, 1, 1, 1 },
    };

    if (!native_rows_require_mesa()) {
        return;
    }
    native_rows_reach(&state);
    state.drawable.origin[0] = 1;
    state.drawable.origin[1] = 2;
    state.drawable.clip.r[0] = (SGIClipRect) { 1, 2, 6, 7 };
    state.scissor[2] = 8;
    state.scissor[3] = 8;
    native_rows_compare(&state, xy, expected, 5, 0);
}

/*
 * Origin (1,1) and a clip that crosses only the plane's right edge. Window
 * row 0 covers columns 0 to 3, which land on plane row 1 at columns 1 to
 * 4: plane column 4 lies past the right edge and is dropped. Window row 1
 * covers column 0, plane pixel (1,2).
 */
static void test_native_rows_origin_right_edge(void)
{
    SGIGLDrawState state = native_rows_state();
    static const float xy[3][2] = {
        { 0.2f, 0.2f }, { 4.8f, 0.2f }, { 0.2f, 1.9f },
    };
    static const uint8_t expected[4][4] = {
        { 0, 0, 0, 0 },
        { 0, 1, 1, 1 },
        { 0, 1, 0, 0 },
        { 0, 0, 0, 0 },
    };

    if (!native_rows_require_mesa()) {
        return;
    }
    native_rows_reach(&state);
    state.drawable.origin[0] = 1;
    state.drawable.origin[1] = 1;
    state.drawable.clip.r[0] = (SGIClipRect) { 1, 1, 6, 2 };
    state.scissor[2] = 8;
    state.scissor[3] = 8;
    native_rows_compare(&state, xy, expected, 1, 1);
}

/*
 * A negative origin (-2,-1) under a clip that crosses the plane's top, left
 * and bottom edges: window row 1 lands on plane row 0 and window row 2 on
 * plane row 1, two columns to the left.
 */
static void test_native_rows_origin_negative(void)
{
    SGIGLDrawState state = native_rows_state();
    static const float xy[3][2] = {
        { 0.25f, 0.25f }, { 0.25f, 2.75f }, { 5.25f, 2.75f },
    };
    static const uint8_t expected[4][4] = {
        { 1, 0, 0, 0 },
        { 1, 1, 1, 0 },
    };

    if (!native_rows_require_mesa()) {
        return;
    }
    native_rows_reach(&state);
    state.drawable.origin[0] = -2;
    state.drawable.origin[1] = -1;
    state.drawable.clip.r[0] = (SGIClipRect) { -2, -2, 2, 5 };
    state.scissor[2] = 8;
    state.scissor[3] = 8;
    native_rows_compare(&state, xy, expected, 5, 0);
}

/*
 * The offset viewport at origin (1,2), a clip touching the plane's right
 * and bottom edges, and a window-relative scissor that keeps window row 1
 * from column 1 on. Window rows 0 and 1 cover columns 0-2 and 0-1 without
 * the scissor, so only plane pixel (2,3) remains.
 */
static void test_native_rows_origin_clip_scissor(void)
{
    SGIGLDrawState state = native_rows_state();
    /* x_w = 4 * x_ndc + 2, y_w = 4 * y_ndc + 3. */
    static const float xy[3][2] = {
        { -0.475f, -0.7f }, { 0.45f, -0.7f }, { -0.475f, -0.075f },
    };
    static const uint8_t expected[4][4] = {
        { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },
        { 0, 0, 1, 0 },
    };

    if (!native_rows_require_mesa()) {
        return;
    }
    state.viewport[0] = -2;
    state.viewport[1] = -1;
    state.viewport[2] = 8;
    state.viewport[3] = 8;
    state.drawable.origin[0] = 1;
    state.drawable.origin[1] = 2;
    state.drawable.clip.r[0] = (SGIClipRect) { 1, 2, 3, 3 };
    state.scissor[0] = 1;
    state.scissor[1] = 1;
    state.scissor[2] = 7;
    state.scissor[3] = 1;
    native_rows_match_scalar(&state, xy, expected);
}

/* The same draw with a known 3 by 2 drawable extent that holds the clip. */
static void test_native_rows_origin_known_extent(void)
{
    SGIGLDrawState state = native_rows_state();
    static const float xy[3][2] = {
        { -0.475f, -0.7f }, { 0.45f, -0.7f }, { -0.475f, -0.075f },
    };
    static const uint8_t expected[4][4] = {
        { 0, 0, 0, 0 },
        { 0, 0, 0, 0 },
        { 1, 1, 1, 0 },
        { 1, 1, 0, 0 },
    };

    if (!native_rows_require_mesa()) {
        return;
    }
    state.viewport[0] = -2;
    state.viewport[1] = -1;
    state.viewport[2] = 8;
    state.viewport[3] = 8;
    state.drawable.origin[0] = 0;
    state.drawable.origin[1] = 2;
    state.drawable.width = 3;
    state.drawable.height = 2;
    state.drawable.clip.r[0] = (SGIClipRect) { 0, 2, 2, 3 };
    native_rows_match_scalar(&state, xy, expected);
}

/*
 * Run a draw the adapter must refuse before any snapshot, and require its
 * own message rather than the shared renderer's.
 */
static void native_rows_refused(NativeFixture *f, SGIRenderer *renderer,
                                SGIGLDraw *draw, const char *message)
{
    SGIRasterRules rules = native_rules;
    uint64_t saved_size = f->memory[0].size;
    uint8_t before[64];
    Error *err = NULL;

    memcpy(before, f->memory[0].bytes, sizeof(before));
    /* An attempted snapshot would report a resource failure here. */
    f->memory[0].size = 0;
    g_assert_cmpint(native_draw(&f->native, renderer, &rules,
                    draw, 1, SGI_RENDER_MONO, false, &err), ==,
                    SGI_RENDER_UNSUPPORTED);
    g_assert_cmpint(last_reason, ==, SGI_RENDER_REFUSAL_WINDOW);
    f->memory[0].size = saved_size;
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), message));
    error_free(err);
    g_assert_cmpmem(f->memory[0].bytes, sizeof(before), before,
                    sizeof(before));
    g_assert_cmpuint(f->invalidations, ==, 0);
}

/*
 * A half-specified extent, and a known extent that does not hold the clip,
 * are refused before any snapshot.
 */
static void test_native_rows_extent_refused(void)
{
    static const float xy[3][2] = {
        { -1, -1 }, { 3, -1 }, { -1, 3 },
    };
    float vertices[3][SGI_GL_VERTEX_FLOATS];
    SGIGLDrawState state;
    SGIGLDraw draw;
    NativeFixture f;
    SGIRenderer *renderer;
    Error *err = NULL;

    if (!native_rows_require_mesa()) {
        return;
    }
    native_fixture_init(&f);
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    for (unsigned rejected = 0; rejected < 6; rejected++) {
        state = native_rows_state();
        native_rows_draw_init(&state, &draw, vertices, xy);
        state.drawable.origin[0] = 1;
        state.drawable.origin[1] = 2;
        state.drawable.width = 2;
        state.drawable.height = 2;
        state.drawable.clip.r[0] = (SGIClipRect) { 1, 2, 2, 3 };
        switch (rejected) {
        case 0:
            state.drawable.height = 0;
            break;
        case 1:
            state.drawable.width = 0;
            break;
        case 2:
            state.drawable.clip.r[0].x0 = 0;
            break;
        case 3:
            state.drawable.clip.r[0].y0 = 1;
            break;
        case 4:
            state.drawable.clip.r[0].x1 = 3;
            break;
        default:
            state.drawable.clip.r[0].y1 = 4;
            break;
        }
        native_rows_refused(&f, renderer, &draw, "drawable extent");
    }
    sgi_renderer_free(renderer);
    native_fixture_cleanup(&f);
}

/*
 * Only the observed profile is mapped: no window matrix or rows-down flag,
 * and origin-shifted viewport and scissor values inside 32 bits. Each other
 * case is refused before any snapshot.
 */
static void test_native_rows_unsupported_profile(void)
{
    SGIRasterRules rules = native_rules;
    static const float xy[3][2] = {
        { -1, -1 }, { 3, -1 }, { -1, 3 },
    };
    float vertices[3][SGI_GL_VERTEX_FLOATS];
    SGIGLDrawState state;
    SGIGLDraw draw;
    NativeFixture f;
    SGIRenderer *renderer;
    uint8_t before[64];
    Error *err = NULL;

    if (!native_rows_require_mesa()) {
        return;
    }
    native_fixture_init(&f);
    memcpy(before, f.memory[0].bytes, sizeof(before));
    renderer = sgi_renderer_new(&err);
    g_assert_nonnull(renderer);
    g_assert_null(err);
    for (unsigned rejected = 0; rejected < 6; rejected++) {
        uint64_t saved_size = f.memory[0].size;

        state = native_rows_state();
        native_rows_draw_init(&state, &draw, vertices, xy);
        switch (rejected) {
        case 0:
            state.window_rows_down = 1;
            break;
        case 1:
            state.window_matrix_given = 1;
            break;
        /* Each case leaves 32 bits in exactly one mapped value. */
        case 2:
            /* Viewport x plus origin x; scissor x is 0 + origin x. */
            state.drawable.origin[0] = INT32_MAX;
            state.viewport[0] = 1;
            break;
        case 3:
            /* Viewport y plus origin y; scissor y is 0 + origin y. */
            state.drawable.origin[1] = INT32_MAX;
            state.viewport[1] = 1;
            break;
        case 4:
            /* Scissor x plus origin x; viewport x is 0 + origin x. */
            state.drawable.origin[0] = 1;
            state.scissor[0] = INT32_MAX;
            break;
        default:
            /* Scissor y plus origin y; viewport y is 0 + origin y. */
            state.drawable.origin[1] = 1;
            state.scissor[1] = INT32_MAX;
            break;
        }
        /* An attempted snapshot would report a resource failure here. */
        f.memory[0].size = 0;
        g_assert_cmpint(native_draw(&f.native, renderer, &rules,
                        &draw, 1, SGI_RENDER_MONO, false, &err), ==,
                        SGI_RENDER_UNSUPPORTED);
        g_assert_cmpint(last_reason, ==, SGI_RENDER_REFUSAL_WINDOW);
        f.memory[0].size = saved_size;
        g_assert_nonnull(err);
        /* The shared renderer refuses some of these too; require ours. */
        g_assert_nonnull(strstr(error_get_pretty(err), "row convention"));
        error_free(err);
        err = NULL;
        g_assert_cmpmem(f.memory[0].bytes, sizeof(before), before,
                        sizeof(before));
        g_assert_cmpuint(f.invalidations, ==, 0);
    }
    sgi_renderer_free(renderer);
    native_fixture_cleanup(&f);
}

/*
 * Per-draw fallback: an 8 by 8 pair of planes the device's way round,
 * drawn by the adapter or by the scalar rasterizer storing as the device
 * does, at plane pixel (origin + window pixel) when inside the plane and a
 * drawable clip rectangle.
 */
#define EDGE_SIZE 8

typedef struct EdgeFixture {
    SGIBoardMemory memory[2];
    const SGIPlane *plane[2];
    OdysseyNativeMesa native;
    SGIRenderer *renderer;
    OdysseyMesaCounts counts;
    unsigned invalidations;
    /* The scalar draws, and the last draw they were given. */
    unsigned scalar_calls;
    const SGIGLDraw *scalar_draw;
    SGIGLDrawResult scalar_result;
    /* Staged-pixel checks so far, and which of them refuse. */
    unsigned staged_checks;
    unsigned refuse_parity;
    /* Colour writes to buffer 0 by either path, per plane pixel. */
    unsigned writes[EDGE_SIZE][EDGE_SIZE];
} EdgeFixture;

static void edge_invalidate(void *opaque)
{
    EdgeFixture *f = opaque;

    f->invalidations++;
}

static uint32_t edge_sentinel(unsigned b, uint32_t x, uint32_t y)
{
    return 0x10203000 + b * 0x01000000 + y * EDGE_SIZE + x;
}

static OdysseyNativeStoreResult edge_native_store(void *opaque,
                                                  SGIBoardMemory *memory,
                                                  const SGIPlane *plane,
                                                  uint32_t x, uint32_t y,
                                                  uint32_t word)
{
    EdgeFixture *f = opaque;

    sgi_plane_store(memory, plane, x, y, word);
    if (memory == &f->memory[0]) {
        f->writes[y][x]++;
    }
    return ODYSSEY_NATIVE_STORE_OK;
}

static void edge_fixture_init(EdgeFixture *f)
{
    SGIPlane plane = {
        .format = SGI_PLANE_RGBA8,
        .packing = SGI_PACK_BE_RED_HIGH,
        .width = EDGE_SIZE, .height = EDGE_SIZE,
        .stride = EDGE_SIZE * 4, .len = EDGE_SIZE * EDGE_SIZE * 4,
    };
    Error *err = NULL;

    memset(f, 0, sizeof(*f));
    for (unsigned b = 0; b < 2; b++) {
        uint32_t id;

        sgi_board_memory_init(&f->memory[b], plane.len);
        id = sgi_board_memory_add_plane(&f->memory[b], &plane, &error_abort);
        f->plane[b] = sgi_board_memory_plane(&f->memory[b], id);
        for (uint32_t y = 0; y < EDGE_SIZE; y++) {
            for (uint32_t x = 0; x < EDGE_SIZE; x++) {
                sgi_plane_store(&f->memory[b], f->plane[b], x, y,
                                edge_sentinel(b, x, y));
            }
        }
    }
    g_assert_true(odyssey_native_mesa_bind(&f->native,
                  &f->memory[0], f->plane[0],
                  &f->memory[1], f->plane[1],
                  edge_invalidate, f, &err));
    g_assert_null(err);
    f->native.store = edge_native_store;
    f->native.store_opaque = f;
    g_assert_cmpint(sgi_renderer_create(&f->renderer, &err), ==,
                    SGI_RENDER_CREATE_OK);
    g_assert_null(err);
}

static void edge_fixture_cleanup(EdgeFixture *f)
{
    sgi_renderer_free(f->renderer);
    for (unsigned b = 0; b < 2; b++) {
        sgi_board_memory_cleanup(&f->memory[b]);
    }
}

static const SGIGLDraw *edge_store_draw;

static void edge_store_rgba(void *opaque, SGIColourTarget target, int x,
                            int y, const SGIRasterColour *colour)
{
    EdgeFixture *f = opaque;
    const SGIClipList *clip = &edge_store_draw->state->drawable.clip;
    const uint32_t *c = colour->component;

    g_assert_cmpint(target, ==, 0);
    if (x < 0 || y < 0 || x >= EDGE_SIZE || y >= EDGE_SIZE) {
        return;
    }
    for (uint32_t i = 0; i < clip->n; i++) {
        if (x >= clip->r[i].x0 && x <= clip->r[i].x1 &&
            y >= clip->r[i].y0 && y <= clip->r[i].y1) {
            sgi_plane_store(&f->memory[0], f->plane[0], x, y,
                            (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3]);
            f->writes[y][x]++;
            return;
        }
    }
}

/* The device's scalar draw: the shared rasterizer under its RGBA rules. */
static SGIGLDrawResult edge_scalar_draw(void *opaque, const SGIGLDraw *draw)
{
    EdgeFixture *f = opaque;
    SGIRasterTarget target = {
        .store_rgba = edge_store_rgba,
        .opaque = f,
    };
    SGIGLDrawResult result;

    f->scalar_calls++;
    f->scalar_draw = draw;
    edge_store_draw = draw;
    result = sgi_soft_draw(&native_rules, &target, draw);
    edge_store_draw = NULL;
    return f->scalar_result != SGI_GL_DRAW_OK ? f->scalar_result : result;
}

static SGIGLDrawResult edge_scalar_nothing(void *opaque,
                                           const SGIGLDraw *draw)
{
    EdgeFixture *f = opaque;

    f->scalar_calls++;
    f->scalar_draw = draw;
    return SGI_GL_DRAW_OK;
}

/* Refuse every other attempt that reaches its staged pixels. */
static SGIRenderResult edge_check_staged(void *opaque,
                                         const SGIRenderResource *resource,
                                         const SGIRenderRect *rect,
                                         const uint32_t *slots,
                                         size_t stride_slots,
                                         size_t capacity_slots,
                                         const SGIRenderWriteCoverage *writes,
                                         Error **errp)
{
    EdgeFixture *f = opaque;

    if (f->staged_checks++ % 2 == f->refuse_parity) {
        error_setg(errp, "test refusal");
        return SGI_RENDER_UNSUPPORTED;
    }
    return SGI_RENDER_OK;
}

/*
 * Window pixels map to the plane through viewport (-8, -8, 32, 32) and
 * identity matrices: window = 16 * ndc + 8. Rows run down the plane from
 * the drawable origin, as the device's do.
 */
static SGIGLDrawState edge_state(int32_t ox, int32_t oy)
{
    SGIGLDrawState state = {
        .shade_model = SGI_GL_FLAT,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .front_face = SGI_GL_CCW,
        .drawable = {
            .bound = true, .front = 0, .origin = { ox, oy },
            .clip = { .n = 1,
                      .r[0] = { 0, 0, EDGE_SIZE - 1, EDGE_SIZE - 1 } },
        },
        .viewport = { -8, -8, 32, 32 },
        .scissor_test = 1, .scissor = { -8, -8, 32, 32 },
    };

    for (unsigned i = 0; i < 4; i++) {
        state.modelview[i * 5] = 1;
        state.projection[i * 5] = 1;
    }
    return state;
}

typedef struct EdgeTriangle {
    float vertices[3][SGI_GL_VERTEX_FLOATS];
    SGIGLDraw draw;
} EdgeTriangle;

static void edge_triangle(EdgeTriangle *t, SGIGLDrawState *state,
                          const float window[3][2], const float colour[4])
{
    memset(t, 0, sizeof(*t));
    for (unsigned i = 0; i < 3; i++) {
        t->vertices[i][SGI_GL_V_POSITION] = (window[i][0] - 8) / 16;
        t->vertices[i][SGI_GL_V_POSITION + 1] = (window[i][1] - 8) / 16;
        t->vertices[i][SGI_GL_V_POSITION + 3] = 1;
        memcpy(&t->vertices[i][SGI_GL_V_COLOR], colour, sizeof(float) * 4);
    }
    t->draw = (SGIGLDraw) {
        .mode = SGI_GL_TRIANGLES, .count = 3,
        .vertices = &t->vertices[0][0], .state = state,
    };
}

static SGIGLDrawResult edge_draw(EdgeFixture *f, const SGIGLDraw *draw)
{
    return odyssey_native_mesa_draw_or_scalar(&f->native, f->renderer,
                                              &native_rules, draw, 1,
                                              SGI_RENDER_MONO, false,
                                              SGI_RENDER_REFUSAL_NONE,
                                              edge_scalar_draw, f,
                                              &f->counts);
}

static void assert_planes_untouched(const EdgeFixture *f)
{
    for (unsigned b = 0; b < 2; b++) {
        for (uint32_t y = 0; y < EDGE_SIZE; y++) {
            for (uint32_t x = 0; x < EDGE_SIZE; x++) {
                g_assert_cmphex(sgi_plane_load(&f->memory[b], f->plane[b],
                                x, y), ==, edge_sentinel(b, x, y));
            }
        }
    }
}

static uint64_t edge_fallbacks(const EdgeFixture *f)
{
    uint64_t total = 0;

    for (unsigned r = 0; r < SGI_RENDER_REFUSAL__MAX; r++) {
        total += f->counts.fallbacks[r];
    }
    return total;
}

static const float edge_red[4] = { 1, 0, 0, 1 };
static const float edge_green[4] = { 0, 1, 0, 1 };

/*
 * State the shared renderer refuses goes to the scalar path: the same draw,
 * whose result is returned as it is, counted once by reason, after a
 * refusal that touched nothing.
 */
static void test_native_fallback_refused_state(void)
{
    static const float window[3][2] = { { 1, 1 }, { 7, 1 }, { 1, 7 } };
    SGIGLDrawState state = edge_state(0, 0);
    EdgeTriangle t;
    EdgeFixture f;

    if (!native_rows_require_mesa()) {
        return;
    }
    edge_fixture_init(&f);
    state.fog = 1;
    edge_triangle(&t, &state, window, edge_red);
    f.scalar_result = SGI_GL_DRAW_OUT_OF_MEMORY;
    /* The scalar draw's own result reaches the shared context. */
    g_assert_cmpint(edge_draw(&f, &t.draw), ==, SGI_GL_DRAW_OUT_OF_MEMORY);
    g_assert_cmpuint(f.scalar_calls, ==, 1);
    g_assert_true(f.scalar_draw == &t.draw);
    g_assert_cmpuint(f.counts.fallbacks[SGI_RENDER_REFUSAL_FOG], ==, 1);
    g_assert_cmpuint(edge_fallbacks(&f), ==, 1);
    g_assert_cmpuint(f.counts.committed, ==, 0);
    g_assert_cmpuint(f.invalidations, ==, 0);

    /* The scalar draw wrote the pixels; the next draw tries Mesa again. */
    g_assert_cmphex(sgi_plane_load(&f.memory[0], f.plane[0], 2, 2), ==,
                    0xff0000ff);
    state.fog = 0;
    f.scalar_result = SGI_GL_DRAW_OK;
    edge_triangle(&t, &state, window, edge_green);
    g_assert_cmpint(edge_draw(&f, &t.draw), ==, SGI_GL_DRAW_OK);
    g_assert_cmpuint(f.scalar_calls, ==, 1);
    g_assert_cmpuint(f.counts.committed, ==, 1);
    g_assert_cmpuint(edge_fallbacks(&f), ==, 1);
    g_assert_cmpuint(f.invalidations, ==, 1);
    g_assert_cmphex(sgi_plane_load(&f.memory[0], f.plane[0], 2, 2), ==,
                    0x00ff00ff);
    edge_fixture_cleanup(&f);
}

/*
 * The device's own refusals, and the adapter's, fall back the same way:
 * colour-index before binding, unmappable rows, both buffers, and state
 * that only the mixed-frame set excludes, here blending away from the
 * comparisons' fixture.
 */
static void test_native_fallback_family_refusals(void)
{
    static const float window[3][2] = { { 1, 1 }, { 7, 1 }, { 1, 7 } };
    SGIGLDrawState state = edge_state(0, 0);
    EdgeTriangle t;
    EdgeFixture f;

    if (!native_rows_require_mesa()) {
        return;
    }
    edge_fixture_init(&f);
    edge_triangle(&t, &state, window, edge_red);
    g_assert_cmpint(odyssey_native_mesa_draw_or_scalar(NULL, f.renderer,
                    &native_rules, &t.draw, 1, SGI_RENDER_MONO, false,
                    SGI_RENDER_REFUSAL_COLOUR_INDEX, edge_scalar_draw, &f,
                    &f.counts), ==, SGI_GL_DRAW_OK);
    g_assert_cmpuint(f.counts.fallbacks[SGI_RENDER_REFUSAL_COLOUR_INDEX], ==,
                     1);
    g_assert_cmpint(odyssey_native_mesa_draw_or_scalar(&f.native, f.renderer,
                    &native_rules, &t.draw, 3, SGI_RENDER_MONO, false,
                    SGI_RENDER_REFUSAL_NONE, edge_scalar_draw, &f,
                    &f.counts), ==, SGI_GL_DRAW_OK);
    g_assert_cmpuint(f.counts.fallbacks[SGI_RENDER_REFUSAL_BUFFERS], ==, 1);
    state.window_rows_down = 1;
    g_assert_cmpint(edge_draw(&f, &t.draw), ==, SGI_GL_DRAW_OK);
    g_assert_cmpuint(f.counts.fallbacks[SGI_RENDER_REFUSAL_WINDOW], ==, 1);
    state.window_rows_down = 0;
    state.blend = 1;
    state.blend_src = SGI_GL_SRC_ALPHA;
    state.blend_dst = SGI_GL_ONE_MINUS_SRC_ALPHA;
    state.blend_equation = SGI_GL_FUNC_ADD;
    g_assert_cmpint(edge_draw(&f, &t.draw), ==, SGI_GL_DRAW_OK);
    g_assert_cmpuint(f.counts.fallbacks[SGI_RENDER_REFUSAL_MIXED_FRAME], ==,
                     1);
    g_assert_cmpuint(f.scalar_calls, ==, 4);
    g_assert_cmpuint(edge_fallbacks(&f), ==, 4);
    g_assert_cmpuint(f.counts.committed, ==, 0);
    g_assert_cmpuint(f.invalidations, ==, 0);
    g_assert_cmphex(sgi_plane_load(&f.memory[0], f.plane[0], 2, 2), ==,
                    0xff0000ff);
    edge_fixture_cleanup(&f);
}

/*
 * A refusal after host readback, from the staged-pixel check, also leaves
 * native storage, scanout and every other buffer as they were before the
 * scalar draw.
 */
static void test_native_fallback_staged_refusal(void)
{
    static const float window[3][2] = { { 1, 1 }, { 7, 1 }, { 1, 7 } };
    SGIGLDrawState state = edge_state(0, 0);
    EdgeTriangle t;
    EdgeFixture f;

    if (!native_rows_require_mesa()) {
        return;
    }
    edge_fixture_init(&f);
    f.native.check_staged = edge_check_staged;
    f.native.check_staged_opaque = &f;
    edge_triangle(&t, &state, window, edge_red);
    /* A scalar draw that draws nothing: whatever changed was Mesa's. */
    g_assert_cmpint(odyssey_native_mesa_draw_or_scalar(&f.native, f.renderer,
                    &native_rules, &t.draw, 1, SGI_RENDER_MONO, false,
                    SGI_RENDER_REFUSAL_NONE, edge_scalar_nothing, &f,
                    &f.counts), ==, SGI_GL_DRAW_OK);
    g_assert_cmpuint(f.scalar_calls, ==, 1);
    g_assert_cmpuint(f.staged_checks, ==, 1);
    g_assert_cmpuint(f.counts.fallbacks[SGI_RENDER_REFUSAL_PRECISION], ==, 1);
    g_assert_cmpuint(f.invalidations, ==, 0);
    assert_planes_untouched(&f);
    edge_fixture_cleanup(&f);
}

/*
 * A Mesa failure is reported as itself and never drawn again on the
 * scalar path, nor counted as a fallback.
 */
static void test_native_fallback_failure_not_retried(void)
{
    static const float window[3][2] = { { 1, 1 }, { 7, 1 }, { 1, 7 } };
    SGIGLDrawState state = edge_state(0, 0);
    EdgeTriangle t;
    EdgeFixture f;

    if (!native_rows_require_mesa()) {
        return;
    }
    edge_fixture_init(&f);
    edge_triangle(&t, &state, window, edge_red);
    /* An attempted snapshot reports a resource failure. */
    f.memory[0].size = 0;
    g_assert_cmpint(edge_draw(&f, &t.draw), ==, SGI_GL_DRAW_RESOURCE_ERROR);
    g_assert_cmpuint(f.scalar_calls, ==, 0);
    g_assert_cmpuint(edge_fallbacks(&f), ==, 0);
    g_assert_cmpuint(f.counts.committed, ==, 0);
    edge_fixture_cleanup(&f);
}

/*
 * Two triangles sharing an edge through a line of pixel centres. Which
 * triangle owns those centres follows the scalar path's tie rule in the
 * device's own window rows, which the adapter gives Mesa unmirrored. A
 * frame that splits the pair between the paths, in either order, must
 * equal the all-scalar frame, with every edge centre written exactly once,
 * by the triangle the scalar path gives it to.
 */
static const struct {
    const char *name;
    float a[3][2], b[3][2];
} edge_pairs[] = {
    { "horizontal",
      { { -1, 3.5 }, { 9, 3.5 }, { 4, -1 } },
      { { 9, 3.5 }, { -1, 3.5 }, { 4, 9 } } },
    { "vertical",
      { { 3.5, -1 }, { 3.5, 9 }, { -1, 4 } },
      { { 3.5, 9 }, { 3.5, -1 }, { 9, 4 } } },
    { "diagonal",
      { { -0.5, -0.5 }, { 8.5, 8.5 }, { -0.5, 8.5 } },
      { { -0.5, -0.5 }, { 8.5, -0.5 }, { 8.5, 8.5 } } },
    { "antidiagonal",
      { { -0.5, 8.5 }, { 8.5, -0.5 }, { -0.5, -0.5 } },
      { { 8.5, -0.5 }, { -0.5, 8.5 }, { 8.5, 8.5 } } },
};

/* Whether window pixel (x, y)'s centre lies on pair p's shared edge. */
static bool edge_centre(unsigned p, int32_t x, int32_t y)
{
    if (x < 0 || y < 0 || x >= EDGE_SIZE || y >= EDGE_SIZE) {
        return false;
    }
    switch (p) {
    case 0:
        return y == 3;
    case 1:
        return x == 3;
    case 2:
        return x == y;
    default:
        return x + y == EDGE_SIZE - 1;
    }
}

/* Drawable origins: the plane's top row, and rows partway down it. */
static const int32_t edge_origins[][2] = { { 0, 0 }, { 1, 3 } };

static void edge_plane_copy(const EdgeFixture *f,
                            uint32_t out[EDGE_SIZE][EDGE_SIZE])
{
    for (uint32_t y = 0; y < EDGE_SIZE; y++) {
        for (uint32_t x = 0; x < EDGE_SIZE; x++) {
            out[y][x] = sgi_plane_load(&f->memory[0], f->plane[0], x, y);
        }
    }
}

/*
 * No pixel written twice, and every edge centre in the plane written,
 * which together mean each edge centre was drawn exactly once.
 */
static void edge_assert_writes(const EdgeFixture *f, unsigned p,
                               const int32_t origin[2])
{
    for (uint32_t y = 0; y < EDGE_SIZE; y++) {
        for (uint32_t x = 0; x < EDGE_SIZE; x++) {
            g_assert_cmpuint(f->writes[y][x], <=, 1);
            if (edge_centre(p, (int32_t)x - origin[0],
                            (int32_t)y - origin[1])) {
                g_assert_cmpuint(f->writes[y][x], ==, 1);
            }
        }
    }
}

static void test_native_rows_shared_edges(void)
{
    if (!native_rows_require_mesa()) {
        return;
    }
    for (unsigned p = 0; p < ARRAY_SIZE(edge_pairs); p++) {
        for (unsigned o = 0; o < ARRAY_SIZE(edge_origins); o++) {
            SGIGLDrawState state = edge_state(edge_origins[o][0],
                                              edge_origins[o][1]);
            uint32_t scalar[EDGE_SIZE][EDGE_SIZE];
            uint32_t mixed[EDGE_SIZE][EDGE_SIZE];
            unsigned edge_red_pixels = 0, edge_green_pixels = 0;
            EdgeTriangle a, b;
            EdgeFixture f;

            edge_triangle(&a, &state, edge_pairs[p].a, edge_red);
            edge_triangle(&b, &state, edge_pairs[p].b, edge_green);

            edge_fixture_init(&f);
            g_assert_cmpint(edge_scalar_draw(&f, &a.draw), ==,
                            SGI_GL_DRAW_OK);
            g_assert_cmpint(edge_scalar_draw(&f, &b.draw), ==,
                            SGI_GL_DRAW_OK);
            edge_plane_copy(&f, scalar);
            edge_assert_writes(&f, p, edge_origins[o]);
            edge_fixture_cleanup(&f);
            /* Both draw, and every centre on the shared edge is drawn. */
            for (uint32_t y = 0; y < EDGE_SIZE; y++) {
                for (uint32_t x = 0; x < EDGE_SIZE; x++) {
                    int32_t wx = x - edge_origins[o][0];
                    int32_t wy = y - edge_origins[o][1];

                    edge_red_pixels += scalar[y][x] == 0xff0000ff;
                    edge_green_pixels += scalar[y][x] == 0x00ff00ff;
                    if (edge_centre(p, wx, wy)) {
                        g_assert_true(scalar[y][x] == 0xff0000ff ||
                                      scalar[y][x] == 0x00ff00ff);
                    }
                }
            }
            g_assert_cmpuint(edge_red_pixels, >, 0);
            g_assert_cmpuint(edge_green_pixels, >, 0);

            /*
             * Order 1 draws b first. Parity 0 sends the first draw to the
             * scalar path, parity 1 the second.
             */
            for (unsigned split = 0; split < 4; split++) {
                unsigned order = split / 2, parity = split % 2;
                const SGIGLDraw *first = order ? &b.draw : &a.draw;
                const SGIGLDraw *second = order ? &a.draw : &b.draw;

                edge_fixture_init(&f);
                f.native.check_staged = edge_check_staged;
                f.native.check_staged_opaque = &f;
                f.refuse_parity = parity;
                g_assert_cmpint(edge_draw(&f, first), ==, SGI_GL_DRAW_OK);
                g_assert_cmpint(edge_draw(&f, second), ==, SGI_GL_DRAW_OK);
                g_assert_cmpuint(f.counts.committed, ==, 1);
                g_assert_cmpuint(
                    f.counts.fallbacks[SGI_RENDER_REFUSAL_PRECISION], ==, 1);
                g_assert_cmpuint(f.scalar_calls, ==, 1);
                g_assert_true(f.scalar_draw == (parity ? second : first));
                edge_plane_copy(&f, mixed);
                for (uint32_t y = 0; y < EDGE_SIZE; y++) {
                    for (uint32_t x = 0; x < EDGE_SIZE; x++) {
                        if (mixed[y][x] != scalar[y][x] ||
                            f.writes[y][x] > 1) {
                            g_test_message("%s origin %d,%d split %u: "
                                           "pixel %u,%u is %08x written "
                                           "%u times, all-scalar %08x",
                                           edge_pairs[p].name,
                                           edge_origins[o][0],
                                           edge_origins[o][1], split, x, y,
                                           mixed[y][x], f.writes[y][x],
                                           scalar[y][x]);
                        }
                    }
                }
                g_assert_cmpmem(mixed, sizeof(mixed), scalar,
                                sizeof(scalar));
                edge_assert_writes(&f, p, edge_origins[o]);
                for (uint32_t y = 0; y < EDGE_SIZE; y++) {
                    for (uint32_t x = 0; x < EDGE_SIZE; x++) {
                        g_assert_cmphex(sgi_plane_load(&f.memory[1],
                                        f.plane[1], x, y), ==,
                                        edge_sentinel(1, x, y));
                    }
                }
                edge_fixture_cleanup(&f);
            }
        }
    }
}
/*
 * The scalar path snaps window coordinates and adds the drawable origin
 * after; Mesa snaps with the origin folded in. Window x = -1/512 is half a
 * subpixel below zero and rounds away from zero to -1/256, but 1 - 1/512
 * rounds up to 1. At origin x 1 the adapter refuses the draw before any
 * native access. The same vertex at origin 0, and +1/512 at origin 1,
 * keep their sign and draw on Mesa.
 */
static void test_native_rows_origin_half_subpixel(void)
{
    static const float minus[3][2] = {
        { -1.0f / 512, 1 }, { 7, 1 }, { -1.0f / 512, 7 },
    };
    static const float plus[3][2] = {
        { 1.0f / 512, 1 }, { 7, 1 }, { 1.0f / 512, 7 },
    };
    static const struct {
        const float (*window)[2];
        int32_t ox;
        SGIRenderResult result;
    } cases[] = {
        { minus, 1, SGI_RENDER_UNSUPPORTED },
        { minus, 0, SGI_RENDER_OK },
        { plus, 1, SGI_RENDER_OK },
    };

    if (!native_rows_require_mesa()) {
        return;
    }
    for (unsigned c = 0; c < ARRAY_SIZE(cases); c++) {
        SGIGLDrawState state = edge_state(cases[c].ox, 0);
        EdgeTriangle t;
        EdgeFixture f;
        Error *err = NULL;
        uint64_t saved_size;

        edge_fixture_init(&f);
        edge_triangle(&t, &state, cases[c].window, edge_red);
        /* A refused draw must not reach the snapshot. */
        saved_size = f.memory[0].size;
        if (cases[c].result != SGI_RENDER_OK) {
            f.memory[0].size = 0;
        }
        g_assert_cmpint(native_draw(&f.native, f.renderer, &native_rules,
                        &t.draw, 1, SGI_RENDER_MONO, false, &err), ==,
                        cases[c].result);
        f.memory[0].size = saved_size;
        if (cases[c].result == SGI_RENDER_OK) {
            g_assert_null(err);
            g_assert_cmpuint(f.invalidations, ==, 1);
        } else {
            g_assert_nonnull(err);
            g_assert_nonnull(strstr(error_get_pretty(err), "snaps"));
            g_assert_cmpint(last_reason, ==, SGI_RENDER_REFUSAL_WINDOW);
            error_free(err);
            g_assert_cmpuint(f.invalidations, ==, 0);
            for (uint32_t y = 0; y < EDGE_SIZE; y++) {
                for (uint32_t x = 0; x < EDGE_SIZE; x++) {
                    g_assert_cmphex(sgi_plane_load(&f.memory[0], f.plane[0],
                                    x, y), ==, edge_sentinel(0, x, y));
                }
            }
        }
        edge_fixture_cleanup(&f);
    }
}
/*
 * The draw above that would snap differently at origin x 1 falls back:
 * counted under window, and the frame is the all-scalar one exactly.
 */
static void test_native_fallback_origin_half_subpixel(void)
{
    static const float window[3][2] = {
        { -1.0f / 512, 1 }, { 7, 1 }, { -1.0f / 512, 7 },
    };
    SGIGLDrawState state = edge_state(1, 0);
    uint32_t scalar[EDGE_SIZE][EDGE_SIZE], mixed[EDGE_SIZE][EDGE_SIZE];
    EdgeTriangle t;
    EdgeFixture f;

    if (!native_rows_require_mesa()) {
        return;
    }
    edge_triangle(&t, &state, window, edge_red);
    edge_fixture_init(&f);
    g_assert_cmpint(edge_scalar_draw(&f, &t.draw), ==, SGI_GL_DRAW_OK);
    edge_plane_copy(&f, scalar);
    edge_fixture_cleanup(&f);

    edge_fixture_init(&f);
    g_assert_cmpint(edge_draw(&f, &t.draw), ==, SGI_GL_DRAW_OK);
    g_assert_cmpuint(f.counts.fallbacks[SGI_RENDER_REFUSAL_WINDOW], ==, 1);
    g_assert_cmpuint(edge_fallbacks(&f), ==, 1);
    g_assert_cmpuint(f.counts.committed, ==, 0);
    g_assert_cmpuint(f.scalar_calls, ==, 1);
    g_assert_cmpuint(f.invalidations, ==, 0);
    edge_plane_copy(&f, mixed);
    g_assert_cmpmem(mixed, sizeof(mixed), scalar, sizeof(scalar));
    /* The triangle drew: plane column 1 is window column 0. */
    g_assert_cmphex(mixed[2][1], ==, 0xff0000ff);
    edge_fixture_cleanup(&f);
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
    g_test_add_func("/sgi-odyssey-native/mesa-rows-viewport",
                    test_native_rows_viewport);
    g_test_add_func("/sgi-odyssey-native/mesa-rows-reach-viewport",
                    test_native_rows_reach_viewport);
    g_test_add_func("/sgi-odyssey-native/mesa-rows-clip-scissor",
                    test_native_rows_clip_scissor);
    g_test_add_func("/sgi-odyssey-native/mesa-rows-scissor",
                    test_native_rows_scissor);
    g_test_add_func("/sgi-odyssey-native/mesa-rows-origin-edge",
                    test_native_rows_origin_edge);
    g_test_add_func("/sgi-odyssey-native/mesa-rows-origin-right-edge",
                    test_native_rows_origin_right_edge);
    g_test_add_func("/sgi-odyssey-native/mesa-rows-origin-negative",
                    test_native_rows_origin_negative);
    g_test_add_func("/sgi-odyssey-native/mesa-rows-origin-clip-scissor",
                    test_native_rows_origin_clip_scissor);
    g_test_add_func("/sgi-odyssey-native/mesa-rows-origin-known-extent",
                    test_native_rows_origin_known_extent);
    g_test_add_func("/sgi-odyssey-native/mesa-rows-extent-refused",
                    test_native_rows_extent_refused);
    g_test_add_func("/sgi-odyssey-native/mesa-rows-unsupported-profile",
                    test_native_rows_unsupported_profile);
    g_test_add_func("/sgi-odyssey-native/mesa-rows-shared-edges",
                    test_native_rows_shared_edges);
    g_test_add_func("/sgi-odyssey-native/mesa-rows-origin-half-subpixel",
                    test_native_rows_origin_half_subpixel);
    g_test_add_func("/sgi-odyssey-native/fallback-refused-state",
                    test_native_fallback_refused_state);
    g_test_add_func("/sgi-odyssey-native/fallback-family-refusals",
                    test_native_fallback_family_refusals);
    g_test_add_func("/sgi-odyssey-native/fallback-staged-refusal",
                    test_native_fallback_staged_refusal);
    g_test_add_func("/sgi-odyssey-native/fallback-failure-not-retried",
                    test_native_fallback_failure_not_retried);
    g_test_add_func("/sgi-odyssey-native/fallback-origin-half-subpixel",
                    test_native_fallback_origin_half_subpixel);
#endif
    return g_test_run();
}
