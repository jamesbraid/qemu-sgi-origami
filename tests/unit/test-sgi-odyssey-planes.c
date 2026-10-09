/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/sgi-odyssey-planes.h"
#ifdef CONFIG_OPENGL
#include "qemu/module.h"
#include "ui/egl-helpers.h"
#include "ui/sgi-renderer.h"

int display_opengl;

void qemu_gl_run_texture_blit(QemuGLShader *shader, bool flip)
{
    g_assert_not_reached();
}

static void plane_assert_surfaceless_gl(void)
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
    g_assert_true(epoxy_has_egl_extension(display,
                                          "EGL_KHR_surfaceless_context"));
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
    g_test_message("GL renderer: %s", renderer);
    g_assert_true(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                 EGL_NO_CONTEXT));
    g_assert_true(eglDestroyContext(display, context));
    g_assert_true(eglTerminate(display));
}
#endif

typedef struct PlaneFixture {
    uint8_t bytes[80];
    bool written[16];
    bool owned[16];
    unsigned marks;
} PlaneFixture;

static bool plane_written(void *opaque, uint32_t x, uint32_t y)
{
    PlaneFixture *f = opaque;

    return f->written[y * 4 + x];
}

static bool plane_owned(void *opaque, uint32_t x, uint32_t y)
{
    PlaneFixture *f = opaque;

    return f->owned[y * 4 + x];
}

static void plane_mark_written(void *opaque, uint32_t x, uint32_t y)
{
    PlaneFixture *f = opaque;

    f->written[y * 4 + x] = true;
    f->marks++;
}

static OdysseyPlaneView plane_view(PlaneFixture *f, uint64_t view_id)
{
    return (OdysseyPlaneView) {
        .bytes = f->bytes,
        .capacity_bytes = 40,
        .stride_bytes = 20,
        .width = 4, .height = 2,
        .format = ODYSSEY_PLANE_SYNTH_RGBA8,
        .role = SGI_RENDER_COLOUR,
        .buffer = SGI_RENDER_FRONT, .eye = SGI_RENDER_LEFT,
        .target = view_id,
        .bottom_origin = true,
        .backing_id = 10, .view_id = view_id,
        .backing_generation = 1, .view_generation = 1,
        .channel_mask = 0xf,
        .opaque = f,
        .is_written = plane_written,
        .owns_pixel = plane_owned,
        .mark_written = plane_mark_written,
    };
}

static void plane_fixture_init(PlaneFixture *f)
{
    memset(f, 0, sizeof(*f));
    for (unsigned y = 0; y < 2; y++) {
        for (unsigned x = 0; x < 4; x++) {
            unsigned p = y * 4 + x;
            unsigned b = y * 20 + x * 4;

            f->owned[p] = f->written[p] = true;
            f->bytes[b] = p;
            f->bytes[b + 1] = p + 10;
            f->bytes[b + 2] = p + 20;
            f->bytes[b + 3] = p + 30;
        }
        memset(&f->bytes[y * 20 + 16], 0xa5, 4);
    }
}

static void plane_four_row_fixture_init(PlaneFixture *f)
{
    memset(f, 0, sizeof(*f));
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            unsigned p = y * 4 + x;
            unsigned b = y * 20 + x * 4;

            f->written[p] = true;
            f->owned[p] = y == 0;
            f->bytes[b] = p + 1;
            f->bytes[b + 1] = p + 41;
            f->bytes[b + 2] = p + 81;
            f->bytes[b + 3] = p + 121;
        }
        memset(&f->bytes[y * 20 + 16], 0xa5, 4);
    }
}

static void plane_four_row_views(PlaneFixture *f, OdysseyPlaneView views[2])
{
    plane_four_row_fixture_init(f);
    for (unsigned i = 0; i < 2; i++) {
        views[i] = plane_view(f, i + 1);
        views[i].height = 4;
        views[i].capacity_bytes = sizeof(f->bytes);
        views[i].storage_top_origin = true;
    }
}

static SGIRasterTarget plane_target(OdysseyPlanes *planes,
                                    SGIRenderResource *resources)
{
    return (SGIRasterTarget) {
        .opaque = planes,
        .resources = resources,
        .resource_count = planes->count,
        .resource_ops = odyssey_planes_resource_ops(),
    };
}

static OdysseyPlaneRawRef plane_raw_ref(size_t index,
                                        const OdysseyPlaneView *view)
{
    return (OdysseyPlaneRawRef) {
        .view_index = index, .buffer = view->buffer, .eye = view->eye,
        .backing_generation = view->backing_generation,
        .view_generation = view->view_generation,
    };
}

static void test_padded_roundtrip(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    SGIRenderResource next_resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 3, 1 };
    uint32_t slots[40];
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    g_assert_false(view.storage_top_origin);
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    g_assert_null(err);
    target = plane_target(&planes, &resource);
    for (unsigned i = 0; i < ARRAY_SIZE(slots); i++) {
        slots[i] = 0xdead;
    }
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 20, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(slots[0], ==, 0);
    g_assert_cmpuint(slots[3], ==, 30);
    g_assert_cmpuint(slots[12], ==, 3);
    g_assert_cmpuint(slots[20], ==, 4);
    g_assert_cmpuint(slots[35], ==, 37);
    g_assert_cmphex(slots[16], ==, 0xdead);

    slots[0] = 201;
    slots[20 + 3] = 202;
    result = sgi_render_resource_commit(&target, &resource, 0, &rect,
                                        slots, 20, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_cmpuint(result.complete_pixels, ==, 8);
    g_assert_true(result.extent_known);
    g_assert_false(result.partial_pixel);
    g_assert_null(err);
    g_assert_cmpuint(f.bytes[0], ==, 201);
    g_assert_cmpuint(f.bytes[20 + 3], ==, 202);
    g_assert_cmpuint(f.bytes[16], ==, 0xa5);
    g_assert_cmpuint(f.bytes[36], ==, 0xa5);
    g_assert_true(view.dirty);
    odyssey_planes_end_submission(&planes);
    g_assert_true(odyssey_planes_refresh(&planes, &next_resource, &err));
    target = plane_target(&planes, &next_resource);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &next_resource, 0,
                    &rect, slots, 20, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    g_assert_cmpuint(slots[0], ==, 201);
    g_assert_cmpuint(slots[23], ==, 202);
}

static void test_stale_generation(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint32_t slots[4] = { 0xdead };
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    target = plane_target(&planes, &resource);
    view.backing_generation++;
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmphex(slots[0], ==, 0xdead);
}

static void test_unwritten_and_unowned_snapshot(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 1, 0, 1, 0 };
    uint32_t slots[4] = { 0xdead };
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    target = plane_target(&planes, &resource);
    f.written[1] = false;
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmphex(slots[0], ==, 0xdead);
    f.written[1] = true;
    f.owned[1] = false;
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(slots[0], ==, 1);
}

static void test_partial_commit_invalidates_alias(void)
{
    PlaneFixture f;
    OdysseyPlaneView views[2];
    SGIRenderResource resources[2];
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect first = { 0, 0, 0, 0 };
    SGIRenderRect second = { 1, 0, 3, 0 };
    uint32_t first_slots[4] = { 80, 81, 82, 83 };
    uint32_t second_slots[12] = {
        90, 91, 92, 93, 100, 101, 102, 103, 110, 111, 112, 113,
    };
    uint32_t snapshot_a[4], snapshot_b[12];
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    views[0] = plane_view(&f, 1);
    views[1] = plane_view(&f, 2);
    g_assert_true(odyssey_planes_init(&planes, views, resources, 2, &err));
    target = plane_target(&planes, resources);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resources[0], 0,
                    &first, snapshot_a, 4, ARRAY_SIZE(snapshot_a), &err), ==,
                    SGI_RENDER_OK);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resources[1], 0,
                    &second, snapshot_b, 12, ARRAY_SIZE(snapshot_b), &err), ==,
                    SGI_RENDER_OK);
    g_assert_cmpuint(snapshot_a[0], ==, 0);
    g_assert_cmpuint(snapshot_b[0], ==, 1);
    result = sgi_render_resource_commit(&target, &resources[0], 0, &first,
                                        first_slots, 4,
                                        ARRAY_SIZE(first_slots), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_null(err);
    f.owned[3] = false;
    result = sgi_render_resource_commit(&target, &resources[1], 0, &second,
                                        second_slots, 12,
                                        ARRAY_SIZE(second_slots), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(result.complete_pixels, ==, 2);
    g_assert_true(result.extent_known);
    g_assert_false(result.partial_pixel);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.bytes[0], ==, 80);
    g_assert_cmpuint(f.bytes[4], ==, 90);
    g_assert_cmpuint(f.bytes[8], ==, 100);
    g_assert_cmpuint(f.bytes[12], ==, 3);
    g_assert_cmpuint(f.marks, ==, 3);
    g_assert_true(views[0].dirty);
    g_assert_true(views[1].dirty);
    g_assert_cmpuint(views[0].backing_generation, ==, 3);
    g_assert_cmpuint(views[1].backing_generation, ==, 3);
    g_assert_cmpuint(views[1].view_generation, ==, 3);
}

static void test_mask_preserves_other_channels(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint32_t slots[4] = { 50, 60, 70, 80 };
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    view.channel_mask = 0x5;
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    target = plane_target(&planes, &resource);
    result = sgi_render_resource_commit(&target, &resource, 0, &rect,
                                        slots, 4, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.bytes[0], ==, 50);
    g_assert_cmpuint(f.bytes[1], ==, 10);
    g_assert_cmpuint(f.bytes[2], ==, 70);
    g_assert_cmpuint(f.bytes[3], ==, 30);
}

static void test_rgba10_raw_precision(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 1, 0 };
    uint32_t slots[8];
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    memset(f.bytes, 0, sizeof(f.bytes));
    f.bytes[1] = 2;
    f.bytes[8] = 1;
    f.bytes[9] = 2;
    view = plane_view(&f, 1);
    view.format = (OdysseyPlaneFormat)3;
    view.stride_bytes = 40;
    view.capacity_bytes = sizeof(f.bytes);
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    target = plane_target(&planes, &resource);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 8, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    g_assert_cmpuint(slots[0], ==, 512);
    g_assert_cmpuint(slots[4], ==, 513);
    slots[0] = 513;
    result = sgi_render_resource_commit(&target, &resource, 0, &rect,
                                        slots, 8, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_cmpuint(f.bytes[0], ==, 1);
    g_assert_cmpuint(f.bytes[1], ==, 2);
}

static void test_ci12_raw_copy_and_mask(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    OdysseyPlanes planes = { 0 };
    OdysseyPlaneRawRef ref = {
        .view_index = 0, .buffer = SGI_RENDER_FRONT,
        .eye = SGI_RENDER_LEFT,
        .backing_generation = 1, .view_generation = 1,
    };
    SGIRenderRect source = { 0, 0, 2, 0 };
    SGIRenderRect destination = { 1, 0, 3, 0 };
    SGIRenderRect first = { 0, 0, 0, 0 };
    uint32_t slots[4] = { 0 };
    uint32_t value = 0xabc;
    OdysseyPlaneRawWrite write;
    OdysseyPlaneRawResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    memset(f.bytes, 0, sizeof(f.bytes));
    f.bytes[1] = 8;
    f.bytes[2] = 1;
    f.bytes[3] = 8;
    f.bytes[4] = 0xff;
    f.bytes[5] = 0x0f;
    view = plane_view(&f, 1);
    view.format = ODYSSEY_PLANE_SYNTH_CI12;
    view.channel_mask = 0;
    view.index_mask = 0xfff;
    view.stride_bytes = 8;
    view.capacity_bytes = 8;
    view.height = 1;
    g_assert_true(odyssey_planes_raw_init(&planes, &view, 1, &err));
    g_assert_cmpint(odyssey_planes_raw_snapshot(&planes, &ref, &destination,
                    slots, 3, ARRAY_SIZE(slots), &err), ==,
                    ODYSSEY_PLANE_RAW_OK);
    g_assert_cmpuint(slots[0], ==, 0x801);
    g_assert_cmpint(odyssey_planes_raw_copy(&planes, &ref, &source, &ref,
                    &destination, &result, &err), ==, ODYSSEY_PLANE_RAW_OK);
    g_assert_cmpuint(result.complete_pixels, ==, 3);
    g_assert_true(result.extent_known);
    g_assert_cmpuint(f.bytes[2], ==, 0x00);
    g_assert_cmpuint(f.bytes[3], ==, 0x08);
    g_assert_cmpuint(f.bytes[4], ==, 0x01);
    g_assert_cmpuint(f.bytes[5], ==, 0x08);
    g_assert_cmpuint(f.bytes[6], ==, 0xff);
    g_assert_cmpuint(f.bytes[7], ==, 0x0f);

    ref.backing_generation = view.backing_generation;
    ref.view_generation = view.view_generation;
    view.index_mask = 0x00f;
    write = (OdysseyPlaneRawWrite) {
        .ref = ref, .rect = first, .slots = &value,
        .stride_slots = 1, .capacity_slots = 1,
    };
    g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, &write, 1,
                    &result, &err), ==, ODYSSEY_PLANE_RAW_OK);
    g_assert_cmpuint(f.bytes[0], ==, 0x0c);
    g_assert_cmpuint(f.bytes[1], ==, 0x08);
    ref = plane_raw_ref(0, &view);
    write.ref = ref;
    value = 0x1000;
    g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, &write, 1,
                    &result, &err), ==, ODYSSEY_PLANE_RAW_INVALID);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.bytes[0], ==, 0x0c);
    g_assert_cmpuint(f.bytes[1], ==, 0x08);
    g_assert_cmpuint(view.backing_generation, ==, ref.backing_generation);
    f.bytes[0] = f.bytes[1] = 0xff;
    slots[0] = 0xdead;
    g_assert_cmpint(odyssey_planes_raw_snapshot(&planes, &ref, &first,
                    slots, 1, 1, &err), ==, ODYSSEY_PLANE_RAW_RESOURCE);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmphex(slots[0], ==, 0xdead);
}

static void test_raw_no_change_has_no_changed_components(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    OdysseyPlanes planes = { 0 };
    uint32_t slots[4] = { 0, 10, 20, 30 };
    OdysseyPlaneRawWrite write = {
        .ref = {
            .view_index = 0, .buffer = SGI_RENDER_FRONT,
            .eye = SGI_RENDER_LEFT,
            .backing_generation = 1, .view_generation = 1,
        },
        .rect = { 0, 0, 0, 0 },
        .slots = slots, .stride_slots = 4,
        .capacity_slots = ARRAY_SIZE(slots),
    };
    OdysseyPlaneRawResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    g_assert_true(odyssey_planes_raw_init(&planes, &view, 1, &err));
    g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, &write, 1,
                    &result, &err), ==, ODYSSEY_PLANE_RAW_OK);
    g_assert_cmpuint(result.complete_pixels, ==, 1);
    g_assert_cmpuint(result.component_mask, ==, 0);
    g_assert_cmpuint(view.backing_generation, ==, 1);
    g_assert_false(view.dirty);
}

static void test_raw_format_maxima_and_masks(void)
{
    static const struct {
        OdysseyPlaneFormat format;
        uint32_t rgb_max, alpha_max;
        bool index;
    } cases[] = {
        { ODYSSEY_PLANE_SYNTH_RGBA4, 15, 15 },
        { ODYSSEY_PLANE_SYNTH_RGB5_A1, 31, 1 },
        { ODYSSEY_PLANE_SYNTH_RGBA8, 255, 255 },
        { ODYSSEY_PLANE_SYNTH_RGB10_A2, 1023, 3 },
        { ODYSSEY_PLANE_SYNTH_RGBA12_A12, 4095, 4095 },
        { ODYSSEY_PLANE_SYNTH_CI8, 255, 0, true },
        { ODYSSEY_PLANE_SYNTH_CI12, 4095, 0, true },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        PlaneFixture f;
        OdysseyPlaneView view;
        OdysseyPlanes planes = { 0 };
        SGIRenderRect rect = { 0, 0, 0, 0 };
        uint32_t values[4] = {
            cases[i].rgb_max, cases[i].rgb_max,
            cases[i].rgb_max, cases[i].alpha_max,
        };
        uint32_t out[4] = { 0 };
        OdysseyPlaneRawWrite write;
        OdysseyPlaneRawResult result;
        OdysseyPlaneRawRef ref;
        size_t components = 4;
        Error *err = NULL;

        if (cases[i].index) {
            components = 1;
        }
        plane_fixture_init(&f);
        memset(f.bytes, 0, sizeof(f.bytes));
        view = plane_view(&f, 1);
        view.format = cases[i].format;
        view.width = view.height = 1;
        view.stride_bytes = view.capacity_bytes = 8;
        if (cases[i].index) {
            view.channel_mask = 0;
            view.index_mask = cases[i].rgb_max;
        }
        g_assert_true(odyssey_planes_raw_init(&planes, &view, 1, &err));
        ref = plane_raw_ref(0, &view);
        write = (OdysseyPlaneRawWrite) {
            .ref = ref, .rect = rect, .slots = values,
            .stride_slots = components, .capacity_slots = components,
        };
        g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, &write, 1,
                        &result, &err), ==, ODYSSEY_PLANE_RAW_OK);
        ref = plane_raw_ref(0, &view);
        g_assert_cmpint(odyssey_planes_raw_snapshot(&planes, &ref, &rect,
                        out, components, components, &err), ==,
                        ODYSSEY_PLANE_RAW_OK);
        for (size_t c = 0; c < components; c++) {
            g_assert_cmpuint(out[c], ==, values[c]);
        }
        if (cases[i].index) {
            view.index_mask = 0x0f;
        } else {
            view.channel_mask = 0x8;
        }
        memset(values, 0, sizeof(values));
        write.ref = ref;
        g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, &write, 1,
                        &result, &err), ==, ODYSSEY_PLANE_RAW_OK);
        ref = plane_raw_ref(0, &view);
        g_assert_cmpint(odyssey_planes_raw_snapshot(&planes, &ref, &rect,
                        out, components, components, &err), ==,
                        ODYSSEY_PLANE_RAW_OK);
        if (cases[i].index) {
            g_assert_cmpuint(out[0], ==, cases[i].rgb_max & ~0x0f);
        } else {
            g_assert_cmpuint(out[0], ==, cases[i].rgb_max);
            g_assert_cmpuint(out[1], ==, cases[i].rgb_max);
            g_assert_cmpuint(out[2], ==, cases[i].rgb_max);
            g_assert_cmpuint(out[3], ==, 0);
            for (size_t c = 0; c < 4; c++) {
                view.channel_mask = 1u << c;
                values[c] = 0;
                if (c == 3) {
                    values[c] = cases[i].alpha_max;
                }
                write.ref = ref;
                g_assert_cmpint(odyssey_planes_raw_commit_many(&planes,
                                &write, 1, &result, &err), ==,
                                ODYSSEY_PLANE_RAW_OK);
                ref = plane_raw_ref(0, &view);
                g_assert_cmpint(odyssey_planes_raw_snapshot(&planes, &ref,
                                &rect, out, 4, 4, &err), ==,
                                ODYSSEY_PLANE_RAW_OK);
                for (size_t check = 0; check < 4; check++) {
                    uint32_t expected = 0;

                    if (check == 3 && c == 3) {
                        expected = cases[i].alpha_max;
                    } else if (check != 3 && check > c) {
                        expected = cases[i].rgb_max;
                    }

                    g_assert_cmpuint(out[check], ==, expected);
                }
            }
        }
    }
}

static void test_raw_multi_prevalidates_all_writes(void)
{
    PlaneFixture fixtures[2];
    OdysseyPlaneView views[2];
    OdysseyPlanes planes = { 0 };
    uint32_t first[4] = { 80, 81, 82, 83 };
    uint32_t second[4] = { 256, 91, 92, 93 };
    OdysseyPlaneRawWrite writes[2];
    OdysseyPlaneRawResult results[2];
    Error *err = NULL;

    for (size_t i = 0; i < 2; i++) {
        plane_fixture_init(&fixtures[i]);
        views[i] = plane_view(&fixtures[i], i + 1);
        views[i].backing_id = 10 + i;
        views[i].buffer = i ? SGI_RENDER_BACK : SGI_RENDER_FRONT;
    }
    g_assert_true(odyssey_planes_raw_init(&planes, views, 2, &err));
    writes[0] = (OdysseyPlaneRawWrite) {
        .ref = plane_raw_ref(0, &views[0]),
        .rect = { 0, 0, 0, 0 }, .slots = first,
        .stride_slots = 4, .capacity_slots = 4,
    };
    writes[1] = (OdysseyPlaneRawWrite) {
        .ref = plane_raw_ref(1, &views[1]),
        .rect = { 0, 0, 0, 0 }, .slots = second,
        .stride_slots = 4, .capacity_slots = 4,
    };
    g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, writes, 2,
                    results, &err), ==, ODYSSEY_PLANE_RAW_INVALID);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpint(results[0].status, ==, ODYSSEY_PLANE_RAW_NOT_RUN);
    g_assert_cmpint(results[1].status, ==, ODYSSEY_PLANE_RAW_INVALID);
    g_assert_cmpuint(fixtures[0].bytes[0], ==, 0);
    g_assert_cmpuint(fixtures[1].bytes[0], ==, 0);
    second[0] = 90;
    writes[1].ref.view_generation++;
    g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, writes, 2,
                    results, &err), ==, ODYSSEY_PLANE_RAW_RESOURCE);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(fixtures[0].bytes[0], ==, 0);
    g_assert_cmpuint(views[0].backing_generation, ==, 1);
    g_assert_false(views[0].dirty);
}

static void test_raw_multi_reports_later_partial_failure(void)
{
    PlaneFixture fixtures[2];
    OdysseyPlaneView views[2];
    OdysseyPlanes planes = { 0 };
    uint32_t first[4] = { 80, 81, 82, 83 };
    uint32_t second[12] = {
        90, 91, 92, 93, 100, 101, 102, 103, 110, 111, 112, 113,
    };
    OdysseyPlaneRawWrite writes[2];
    OdysseyPlaneRawResult results[2];
    Error *err = NULL;

    for (size_t i = 0; i < 2; i++) {
        plane_fixture_init(&fixtures[i]);
        views[i] = plane_view(&fixtures[i], i + 1);
        views[i].backing_id = 10 + i;
        views[i].buffer = i ? SGI_RENDER_BACK : SGI_RENDER_FRONT;
    }
    g_assert_true(odyssey_planes_raw_init(&planes, views, 2, &err));
    fixtures[1].owned[2] = false;
    writes[0] = (OdysseyPlaneRawWrite) {
        .ref = plane_raw_ref(0, &views[0]),
        .rect = { 0, 0, 0, 0 }, .slots = first,
        .stride_slots = 4, .capacity_slots = 4,
    };
    writes[1] = (OdysseyPlaneRawWrite) {
        .ref = plane_raw_ref(1, &views[1]),
        .rect = { 0, 0, 2, 0 }, .slots = second,
        .stride_slots = ARRAY_SIZE(second),
        .capacity_slots = ARRAY_SIZE(second),
    };
    g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, writes, 2,
                    results, &err), ==, ODYSSEY_PLANE_RAW_RESOURCE);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpint(results[0].status, ==, ODYSSEY_PLANE_RAW_OK);
    g_assert_cmpuint(results[0].complete_pixels, ==, 1);
    g_assert_cmpint(results[1].status, ==, ODYSSEY_PLANE_RAW_RESOURCE);
    g_assert_cmpuint(results[1].complete_pixels, ==, 2);
    g_assert_true(results[1].extent_known);
    g_assert_false(results[1].partial_pixel);
    g_assert_cmpuint(fixtures[0].bytes[0], ==, 80);
    g_assert_cmpuint(fixtures[1].bytes[0], ==, 90);
    g_assert_cmpuint(fixtures[1].bytes[4], ==, 100);
    g_assert_cmpuint(fixtures[1].bytes[8], ==, 2);
    for (size_t i = 0; i < 2; i++) {
        g_assert_true(views[i].dirty);
        g_assert_cmpuint(views[i].backing_generation, ==, 2);
    }
}

static void test_raw_partial_commit_invalidates_alias(void)
{
    PlaneFixture f;
    OdysseyPlaneView views[2];
    OdysseyPlanes planes = { 0 };
    uint32_t slots[12] = {
        80, 81, 82, 83, 90, 91, 92, 93, 100, 101, 102, 103,
    };
    OdysseyPlaneRawWrite write;
    OdysseyPlaneRawResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    views[0] = plane_view(&f, 1);
    views[1] = plane_view(&f, 2);
    g_assert_true(odyssey_planes_raw_init(&planes, views, 2, &err));
    f.owned[2] = false;
    write = (OdysseyPlaneRawWrite) {
        .ref = plane_raw_ref(0, &views[0]),
        .rect = { 0, 0, 2, 0 }, .slots = slots,
        .stride_slots = ARRAY_SIZE(slots),
        .capacity_slots = ARRAY_SIZE(slots),
    };
    g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, &write, 1,
                    &result, &err), ==, ODYSSEY_PLANE_RAW_RESOURCE);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(result.complete_pixels, ==, 2);
    g_assert_true(result.extent_known);
    g_assert_false(result.partial_pixel);
    g_assert_cmpuint(result.component_mask, ==, 0xf);
    g_assert_cmpuint(f.bytes[0], ==, 80);
    g_assert_cmpuint(f.bytes[4], ==, 90);
    g_assert_cmpuint(f.bytes[8], ==, 2);
    for (size_t i = 0; i < 2; i++) {
        g_assert_true(views[i].dirty);
        g_assert_cmpuint(views[i].backing_generation, ==, 2);
        g_assert_cmpuint(views[i].view_generation, ==, 2);
    }
}

static void test_raw_changed_binding_rejected(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    OdysseyPlanes planes = { 0 };
    SGIRenderRect rect = { 0, 0, 0, 0 };
    OdysseyPlaneRawRef ref;
    uint32_t out = 0xdead;
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    view.format = ODYSSEY_PLANE_SYNTH_CI8;
    view.channel_mask = 0;
    view.index_mask = 0xff;
    g_assert_true(odyssey_planes_raw_init(&planes, &view, 1, &err));
    view.buffer = SGI_RENDER_BACK;
    ref = plane_raw_ref(0, &view);
    g_assert_cmpint(odyssey_planes_raw_snapshot(&planes, &ref, &rect,
                    &out, 1, 1, &err), ==, ODYSSEY_PLANE_RAW_RESOURCE);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    view.buffer = SGI_RENDER_FRONT;
    view.width = 3;
    ref = plane_raw_ref(0, &view);
    g_assert_cmpint(odyssey_planes_raw_snapshot(&planes, &ref, &rect,
                    &out, 1, 1, &err), ==, ODYSSEY_PLANE_RAW_RESOURCE);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmphex(out, ==, 0xdead);
}

static void test_raw_mask_rejects_invalid_preserved_component(void)
{
    PlaneFixture fixtures[2];
    OdysseyPlaneView views[2];
    OdysseyPlanes planes = { 0 };
    uint32_t first[4] = { 4095, 4095, 4095, 4095 };
    uint32_t second[4] = { 2048, 0, 0, 0 };
    OdysseyPlaneRawWrite writes[2];
    OdysseyPlaneRawResult results[2];
    Error *err = NULL;

    for (size_t i = 0; i < 2; i++) {
        plane_fixture_init(&fixtures[i]);
        memset(fixtures[i].bytes, 0, sizeof(fixtures[i].bytes));
        views[i] = plane_view(&fixtures[i], i + 1);
        views[i].format = ODYSSEY_PLANE_SYNTH_RGBA12_A12;
        views[i].width = views[i].height = 1;
        views[i].stride_bytes = views[i].capacity_bytes = 8;
        views[i].backing_id = 10 + i;
        views[i].buffer = i ? SGI_RENDER_BACK : SGI_RENDER_FRONT;
    }
    fixtures[1].bytes[3] = 0x10;
    views[1].channel_mask = 1;
    g_assert_true(odyssey_planes_raw_init(&planes, views, 2, &err));
    writes[0] = (OdysseyPlaneRawWrite) {
        .ref = plane_raw_ref(0, &views[0]),
        .rect = { 0, 0, 0, 0 }, .slots = first,
        .stride_slots = 4, .capacity_slots = 4,
    };
    writes[1] = (OdysseyPlaneRawWrite) {
        .ref = plane_raw_ref(1, &views[1]),
        .rect = { 0, 0, 0, 0 }, .slots = second,
        .stride_slots = 4, .capacity_slots = 4,
    };
    g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, writes, 2,
                    results, &err), ==, ODYSSEY_PLANE_RAW_RESOURCE);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpint(results[0].status, ==, ODYSSEY_PLANE_RAW_NOT_RUN);
    g_assert_cmpint(results[1].status, ==, ODYSSEY_PLANE_RAW_RESOURCE);
    g_assert_cmpuint(results[1].complete_pixels, ==, 0);
    g_assert_cmpuint(fixtures[0].bytes[0], ==, 0);
    g_assert_cmpuint(fixtures[1].bytes[0], ==, 0);
    g_assert_cmpuint(fixtures[1].bytes[3], ==, 0x10);
    for (size_t i = 0; i < 2; i++) {
        g_assert_cmpuint(views[i].backing_generation, ==, 1);
        g_assert_false(views[i].dirty);
    }
}

static void test_deep_overlapping_raw_copy(void)
{
    static const struct {
        OdysseyPlaneFormat format;
        uint32_t low, high, max, alpha_low, alpha_high, alpha_max;
    } cases[] = {
        { ODYSSEY_PLANE_SYNTH_RGB10_A2, 512, 513, 1023, 1, 2, 3 },
        { ODYSSEY_PLANE_SYNTH_RGBA12_A12, 2048, 2049, 4095,
          2048, 2049, 4095 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
        PlaneFixture f;
        OdysseyPlaneView view;
        OdysseyPlanes planes = { 0 };
        SGIRenderRect row = { 0, 0, 3, 0 };
        SGIRenderRect first_two = { 0, 0, 1, 0 };
        SGIRenderRect left = { 0, 0, 2, 0 };
        SGIRenderRect right = { 1, 0, 3, 0 };
        uint32_t slots[16] = {
            cases[i].low, cases[i].max, 1, cases[i].alpha_max,
            cases[i].high, cases[i].max - 1, 2, cases[i].alpha_max,
            cases[i].max, cases[i].low, cases[i].high,
            cases[i].alpha_max, 7, 8, 9, 0,
        };
        uint32_t alpha_slots[8] = {
            0, 0, 0, cases[i].alpha_low,
            0, 0, 0, cases[i].alpha_high,
        };
        uint32_t out[16];
        OdysseyPlaneRawWrite write;
        OdysseyPlaneRawResult result;
        OdysseyPlaneRawRef ref;
        Error *err = NULL;

        plane_fixture_init(&f);
        memset(f.bytes, 0, sizeof(f.bytes));
        view = plane_view(&f, 1);
        view.format = cases[i].format;
        view.height = 1;
        view.stride_bytes = view.capacity_bytes = 32;
        g_assert_true(odyssey_planes_raw_init(&planes, &view, 1, &err));
        ref = plane_raw_ref(0, &view);
        write = (OdysseyPlaneRawWrite) {
            .ref = ref, .rect = row, .slots = slots,
            .stride_slots = ARRAY_SIZE(slots),
            .capacity_slots = ARRAY_SIZE(slots),
        };
        g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, &write, 1,
                        &result, &err), ==, ODYSSEY_PLANE_RAW_OK);
        view.channel_mask = 0x8;
        write = (OdysseyPlaneRawWrite) {
            .ref = plane_raw_ref(0, &view), .rect = first_two,
            .slots = alpha_slots, .stride_slots = ARRAY_SIZE(alpha_slots),
            .capacity_slots = ARRAY_SIZE(alpha_slots),
        };
        g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, &write, 1,
                        &result, &err), ==, ODYSSEY_PLANE_RAW_OK);
        slots[3] = cases[i].alpha_low;
        slots[7] = cases[i].alpha_high;
        ref = plane_raw_ref(0, &view);
        g_assert_cmpint(odyssey_planes_raw_snapshot(&planes, &ref, &row,
                        out, 16, ARRAY_SIZE(out), &err), ==,
                        ODYSSEY_PLANE_RAW_OK);
        for (size_t scalar = 0; scalar < ARRAY_SIZE(out); scalar++) {
            g_assert_cmpuint(out[scalar], ==, slots[scalar]);
        }
        view.channel_mask = 0xf;
        g_assert_cmpint(odyssey_planes_raw_copy(&planes, &ref, &left, &ref,
                        &right, &result, &err), ==, ODYSSEY_PLANE_RAW_OK);
        ref = plane_raw_ref(0, &view);
        g_assert_cmpint(odyssey_planes_raw_snapshot(&planes, &ref, &row,
                        out, 16, ARRAY_SIZE(out), &err), ==,
                        ODYSSEY_PLANE_RAW_OK);
        for (size_t pixel = 0; pixel < 4; pixel++) {
            size_t source = pixel ? pixel - 1 : 0;

            for (size_t c = 0; c < 4; c++) {
                g_assert_cmpuint(out[pixel * 4 + c], ==,
                                 slots[source * 4 + c]);
            }
        }
        g_assert_cmpint(odyssey_planes_raw_copy(&planes, &ref, &right, &ref,
                        &left, &result, &err), ==, ODYSSEY_PLANE_RAW_OK);
        ref = plane_raw_ref(0, &view);
        g_assert_cmpint(odyssey_planes_raw_snapshot(&planes, &ref, &row,
                        out, 16, ARRAY_SIZE(out), &err), ==,
                        ODYSSEY_PLANE_RAW_OK);
        for (size_t pixel = 0; pixel < 4; pixel++) {
            size_t source = pixel == 3 ? 2 : pixel;

            for (size_t c = 0; c < 4; c++) {
                g_assert_cmpuint(out[pixel * 4 + c], ==,
                                 slots[source * 4 + c]);
            }
        }
    }
}

static void test_raw_cross_format_copy_rejected(void)
{
    PlaneFixture fixtures[2];
    OdysseyPlaneView views[2];
    OdysseyPlanes planes = { 0 };
    SGIRenderRect rect = { 0, 0, 0, 0 };
    OdysseyPlaneRawRef source, destination;
    OdysseyPlaneRawResult result;
    Error *err = NULL;

    for (size_t i = 0; i < 2; i++) {
        plane_fixture_init(&fixtures[i]);
        views[i] = plane_view(&fixtures[i], i + 1);
        views[i].backing_id = 10 + i;
        views[i].buffer = i ? SGI_RENDER_BACK : SGI_RENDER_FRONT;
        views[i].format = i ? ODYSSEY_PLANE_SYNTH_RGB10_A2 :
                              ODYSSEY_PLANE_SYNTH_RGBA12_A12;
        views[i].stride_bytes = 40;
        views[i].capacity_bytes = sizeof(fixtures[i].bytes);
    }
    g_assert_true(odyssey_planes_raw_init(&planes, views, 2, &err));
    source = plane_raw_ref(0, &views[0]);
    destination = plane_raw_ref(1, &views[1]);
    g_assert_cmpint(odyssey_planes_raw_copy(&planes, &source, &rect,
                    &destination, &rect, &result, &err), ==,
                    ODYSSEY_PLANE_RAW_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(fixtures[1].bytes[0], ==, 0);
    g_assert_cmpuint(views[1].backing_generation, ==, 1);
    g_assert_false(views[1].dirty);
}

static void test_unknown_format_rejected(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    view.format = 99;
    g_assert_false(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    g_assert_nonnull(err);
    error_free(err);
}

static void test_short_backing_rejected(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    view.capacity_bytes = 35;
    g_assert_false(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    g_assert_nonnull(err);
    error_free(err);
}

static void test_changed_component_rejected(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint32_t slots[4] = { 0xdead };
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    target = plane_target(&planes, &resource);
    resource.component[0].storage_bits = 10;
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmphex(slots[0], ==, 0xdead);
}

static void test_false_alias_rejected(void)
{
    PlaneFixture first, second;
    OdysseyPlaneView views[2];
    SGIRenderResource resources[2];
    OdysseyPlanes planes = { 0 };
    Error *err = NULL;

    plane_fixture_init(&first);
    plane_fixture_init(&second);
    views[0] = plane_view(&first, 1);
    views[1] = plane_view(&second, 2);
    g_assert_false(odyssey_planes_init(&planes, views, resources, 2, &err));
    g_assert_nonnull(err);
    error_free(err);

    err = NULL;
    views[1] = plane_view(&first, 2);
    views[1].storage_top_origin = true;
    g_assert_false(odyssey_planes_init(&planes, views, resources, 2, &err));
    g_assert_nonnull(err);
    error_free(err);

    err = NULL;
    views[1] = plane_view(&first, 2);
    views[1].width = 3;
    g_assert_false(odyssey_planes_init(&planes, views, resources, 2, &err));
    g_assert_nonnull(err);
    error_free(err);
}

static void test_alias_commits_share_submission(void)
{
    PlaneFixture f;
    OdysseyPlaneView views[2];
    SGIRenderResource resources[2];
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect first = { 0, 0, 0, 0 };
    SGIRenderRect second = { 1, 0, 1, 0 };
    uint32_t a[4] = { 80, 81, 82, 83 };
    uint32_t b[4] = { 90, 91, 92, 93 };
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    views[0] = plane_view(&f, 1);
    views[1] = plane_view(&f, 2);
    g_assert_true(odyssey_planes_init(&planes, views, resources, 2, &err));
    target = plane_target(&planes, resources);
    result = sgi_render_resource_commit(&target, &resources[0], 0, &first,
                                        a, 4, ARRAY_SIZE(a), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_null(err);
    result = sgi_render_resource_commit(&target, &resources[1], 0, &second,
                                        b, 4, ARRAY_SIZE(b), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.bytes[0], ==, 80);
    g_assert_cmpuint(f.bytes[4], ==, 90);
    g_assert_cmpuint(views[0].backing_generation, ==, 3);
    g_assert_cmpuint(views[1].backing_generation, ==, 3);

    odyssey_planes_end_submission(&planes);
    result = sgi_render_resource_commit(&target, &resources[0], 0, &first,
                                        a, 4, ARRAY_SIZE(a), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
}

static void test_external_rebind_stales_alias(void)
{
    PlaneFixture f;
    OdysseyPlaneView views[2];
    SGIRenderResource resources[2];
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect first = { 0, 0, 0, 0 };
    SGIRenderRect second = { 1, 0, 1, 0 };
    uint32_t a[4] = { 80, 81, 82, 83 };
    uint32_t b[4] = { 90, 91, 92, 93 };
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    views[0] = plane_view(&f, 1);
    views[1] = plane_view(&f, 2);
    g_assert_true(odyssey_planes_init(&planes, views, resources, 2, &err));
    target = plane_target(&planes, resources);
    result = sgi_render_resource_commit(&target, &resources[0], 0, &first,
                                        a, 4, ARRAY_SIZE(a), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_null(err);
    views[1].view_generation++;
    result = sgi_render_resource_commit(&target, &resources[1], 0, &second,
                                        b, 4, ARRAY_SIZE(b), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(result.complete_pixels, ==, 0);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.bytes[4], ==, 1);
}

static void test_refresh_does_not_retarget_snapshot(void)
{
    PlaneFixture first, second;
    OdysseyPlaneView initial_view, reset_view, rebind_view;
    SGIRenderResource resource, reset_resource, rebind_resource;
    SGIRenderResource reset_next, rebind_next;
    OdysseyPlanes planes = { 0 }, reset_planes = { 0 };
    OdysseyPlanes rebind_planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint32_t slots[4];
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&first);
    plane_fixture_init(&second);
    initial_view = plane_view(&first, 1);
    g_assert_true(odyssey_planes_init(&planes, &initial_view, &resource, 1,
                                      &err));
    target = plane_target(&planes, &resource);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    slots[0] = 200;
    result = sgi_render_resource_commit(&target, &resource, 0, &rect,
                                        slots, 4, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_cmpuint(first.bytes[0], ==, 200);

    plane_fixture_init(&first);
    reset_view = plane_view(&first, 1);
    g_assert_true(odyssey_planes_init(&reset_planes, &reset_view,
                                      &reset_resource, 1, &err));
    target = plane_target(&reset_planes, &reset_resource);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &reset_resource, 0,
                    &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    slots[0] = 201;
    reset_view.backing_generation = 2;
    reset_view.view_generation = 2;
    odyssey_planes_end_submission(&reset_planes);
    g_assert_true(odyssey_planes_refresh(&reset_planes, &reset_next, &err));
    result = sgi_render_resource_commit(&target, &reset_resource, 0, &rect,
                                        slots, 4, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(result.complete_pixels, ==, 0);
    g_assert_cmpuint(first.bytes[0], ==, 0);
    g_assert_cmpuint(first.marks, ==, 0);
    g_assert_false(reset_view.dirty);
    g_assert_cmpuint(reset_view.backing_generation, ==, 2);
    g_assert_nonnull(err);
    error_free(err);

    err = NULL;
    plane_fixture_init(&first);
    rebind_view = plane_view(&first, 1);
    g_assert_true(odyssey_planes_init(&rebind_planes, &rebind_view,
                                      &rebind_resource, 1, &err));
    target = plane_target(&rebind_planes, &rebind_resource);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &rebind_resource, 0,
                    &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    slots[0] = 202;
    rebind_view.bytes = second.bytes;
    rebind_view.opaque = &second;
    rebind_view.backing_id = 20;
    rebind_view.backing_generation = 2;
    rebind_view.view_generation = 2;
    odyssey_planes_end_submission(&rebind_planes);
    g_assert_true(odyssey_planes_refresh(&rebind_planes, &rebind_next, &err));
    result = sgi_render_resource_commit(&target, &rebind_resource, 0, &rect,
                                        slots, 4, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(result.complete_pixels, ==, 0);
    g_assert_cmpuint(second.bytes[0], ==, 0);
    g_assert_cmpuint(second.marks, ==, 0);
    g_assert_false(rebind_view.dirty);
    g_assert_cmpuint(rebind_view.backing_generation, ==, 2);
    g_assert_nonnull(err);
    error_free(err);
}

static void test_reinit_does_not_retarget_snapshot(void)
{
    PlaneFixture first, second;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint32_t slots[4];
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&first);
    plane_fixture_init(&second);
    view = plane_view(&first, 1);
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    target = plane_target(&planes, &resource);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    slots[0] = 203;
    view.bytes = second.bytes;
    view.opaque = &second;
    view.backing_id = 20;
    view.backing_generation = 2;
    view.view_generation = 2;
    g_assert_false(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    result = sgi_render_resource_commit(&target, &resource, 0, &rect,
                                        slots, 4, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(result.complete_pixels, ==, 0);
    g_assert_cmpuint(second.bytes[0], ==, 0);
    g_assert_cmpuint(second.marks, ==, 0);
    g_assert_nonnull(err);
    error_free(err);
}

static void plane_four_eyes(PlaneFixture fixtures[4],
                            OdysseyPlaneView views[4])
{
    for (unsigned i = 0; i < 4; i++) {
        plane_fixture_init(&fixtures[i]);
        fixtures[i].bytes[0] = (i + 1) * 10;
        views[i] = plane_view(&fixtures[i], i + 1);
        views[i].backing_id = 100 + i;
        views[i].target = 11 + i;
        views[i].buffer = i < 2 ? SGI_RENDER_FRONT : SGI_RENDER_BACK;
        views[i].eye = i % 2 ? SGI_RENDER_RIGHT : SGI_RENDER_LEFT;
    }
}

static void test_raw_four_eye_swap(void)
{
    PlaneFixture fixtures[4];
    OdysseyPlaneView views[4];
    OdysseyPlanes planes = { 0 };
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint32_t values[4] = { 10, 20, 30, 40 };
    uint32_t out;
    OdysseyPlaneRawWrite write;
    OdysseyPlaneRawResult result;
    OdysseyPlaneRawRef ref;
    Error *err = NULL;

    plane_four_eyes(fixtures, views);
    for (size_t i = 0; i < 4; i++) {
        memset(fixtures[i].bytes, 0, sizeof(fixtures[i].bytes));
        views[i].format = ODYSSEY_PLANE_SYNTH_CI12;
        views[i].channel_mask = 0;
        views[i].index_mask = 0xfff;
    }
    g_assert_true(odyssey_planes_raw_init(&planes, views, 4, &err));
    for (size_t i = 0; i < 4; i++) {
        write = (OdysseyPlaneRawWrite) {
            .ref = plane_raw_ref(i, &views[i]), .rect = rect,
            .slots = &values[i], .stride_slots = 1, .capacity_slots = 1,
        };
        g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, &write, 1,
                        &result, &err), ==, ODYSSEY_PLANE_RAW_OK);
    }
    views[3].index_mask = 0x0f;
    values[3] = 0x01;
    write.ref = plane_raw_ref(3, &views[3]);
    g_assert_cmpint(odyssey_planes_raw_commit_many(&planes, &write, 1,
                    &result, &err), ==, ODYSSEY_PLANE_RAW_OK);
    values[3] = 0x21;
    for (size_t i = 0; i < 4; i++) {
        ref = plane_raw_ref(i, &views[i]);
        g_assert_cmpint(odyssey_planes_raw_snapshot(&planes, &ref, &rect,
                        &out, 1, 1, &err), ==, ODYSSEY_PLANE_RAW_OK);
        g_assert_cmpuint(out, ==, values[i]);
    }
    g_assert_true(odyssey_planes_swap_bindings(&planes, &err));
    for (size_t eye = 0; eye < 2; eye++) {
        size_t front = eye;
        size_t back = 2 + eye;
        SGIRenderEye expected_eye = eye ? SGI_RENDER_RIGHT :
                                           SGI_RENDER_LEFT;

        g_assert_cmpint(views[front].buffer, ==, SGI_RENDER_BACK);
        g_assert_cmpint(views[back].buffer, ==, SGI_RENDER_FRONT);
        g_assert_cmpint(views[front].eye, ==, expected_eye);
        g_assert_cmpint(views[back].eye, ==, expected_eye);
        g_assert_cmpuint(views[front].target, ==, 13 + eye);
        g_assert_cmpuint(views[back].target, ==, 11 + eye);
        ref = plane_raw_ref(back, &views[back]);
        g_assert_cmpint(odyssey_planes_raw_snapshot(&planes, &ref, &rect,
                        &out, 1, 1, &err), ==, ODYSSEY_PLANE_RAW_OK);
        g_assert_cmpuint(out, ==, values[back]);
    }
}

static void test_four_eye_isolation(void)
{
    PlaneFixture fixtures[4];
    OdysseyPlaneView views[4];
    SGIRenderResource resources[4];
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint32_t slots[4], changed[4] = { 91, 92, 93, 94 };
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_four_eyes(fixtures, views);
    g_assert_true(odyssey_planes_init(&planes, views, resources, 4, &err));
    target = plane_target(&planes, resources);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmpint(sgi_render_resource_snapshot(&target, &resources[i],
                        0, &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                        SGI_RENDER_OK);
        g_assert_cmpuint(slots[0], ==, (i + 1) * 10);
    }
    result = sgi_render_resource_commit(&target, &resources[3], 0, &rect,
                                        changed, 4, ARRAY_SIZE(changed), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(fixtures[0].bytes[0], ==, 10);
    g_assert_cmpuint(fixtures[1].bytes[0], ==, 20);
    g_assert_cmpuint(fixtures[2].bytes[0], ==, 30);
    g_assert_cmpuint(fixtures[3].bytes[0], ==, 91);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_false(views[i].dirty);
        g_assert_cmpuint(views[i].backing_generation, ==, 1);
    }
    g_assert_true(views[3].dirty);
    g_assert_cmpuint(views[3].backing_generation, ==, 2);
}

static void test_top_origin_uses_plane_rows(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 3, 1 };
    uint32_t slots[40];
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    view.bottom_origin = false;
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    g_assert_false(resource.bottom_origin);
    target = plane_target(&planes, &resource);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 20, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    g_assert_cmpuint(slots[0], ==, 0);
    g_assert_cmpuint(slots[20], ==, 4);
    slots[20] = 201;
    result = sgi_render_resource_commit(&target, &resource, 0, &rect,
                                        slots, 20, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.bytes[0], ==, 0);
    g_assert_cmpuint(f.bytes[20], ==, 201);
    g_assert_cmpuint(f.bytes[16], ==, 0xa5);
    g_assert_cmpuint(f.bytes[36], ==, 0xa5);
}

static void test_bottom_origin_with_top_first_storage(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect first = { 0, 0, 0, 0 };
    SGIRenderRect both = { 0, 0, 3, 1 };
    uint32_t slots[40], pixel[4] = { 201, 202, 203, 204 };
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    view.storage_top_origin = true;
    f.owned[0] = false;
    f.written[0] = false;
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    g_assert_true(resource.bottom_origin);
    target = plane_target(&planes, &resource);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &first, slots, 4, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    g_assert_cmpuint(slots[0], ==, 4);

    f.owned[0] = true;
    f.written[0] = true;
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &both, slots, 20, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    g_assert_cmpuint(slots[0], ==, 4);
    g_assert_cmpuint(slots[20], ==, 0);
    f.owned[0] = false;
    f.written[0] = false;
    f.written[4] = false;
    result = sgi_render_resource_commit(&target, &resource, 0, &first,
                                        pixel, 4, ARRAY_SIZE(pixel), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.bytes[0], ==, 0);
    g_assert_cmpuint(f.bytes[20], ==, 201);
    g_assert_false(f.written[0]);
    g_assert_true(f.written[4]);
    g_assert_cmpuint(f.marks, ==, 1);
    g_assert_cmpuint(f.bytes[16], ==, 0xa5);
    g_assert_cmpuint(f.bytes[36], ==, 0xa5);
}

static void test_top_first_storage_three_rows(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 0, 2 };
    uint32_t slots[12];
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    memset(f.bytes, 0xa5, sizeof(f.bytes));
    for (unsigned y = 0; y < 3; y++) {
        f.bytes[y * 8] = 10 * (y + 1);
        f.owned[y * 4] = f.written[y * 4] = true;
    }
    view = plane_view(&f, 1);
    view.width = 1;
    view.height = 3;
    view.stride_bytes = 8;
    view.storage_top_origin = true;
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    target = plane_target(&planes, &resource);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    g_assert_cmpuint(slots[0], ==, 30);
    g_assert_cmpuint(slots[4], ==, 20);
    g_assert_cmpuint(slots[8], ==, 10);

    slots[0] = 101;
    slots[4] = 102;
    slots[8] = 103;
    result = sgi_render_resource_commit(&target, &resource, 0, &rect,
                                        slots, 4, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_cmpuint(result.complete_pixels, ==, 3);
    g_assert_null(err);
    g_assert_cmpuint(f.bytes[0], ==, 103);
    g_assert_cmpuint(f.bytes[8], ==, 102);
    g_assert_cmpuint(f.bytes[16], ==, 101);
    g_assert_cmpuint(f.bytes[4], ==, 0xa5);
    g_assert_cmpuint(f.bytes[12], ==, 0xa5);
    g_assert_cmpuint(f.bytes[20], ==, 0xa5);
    g_assert_cmpuint(f.marks, ==, 3);
}

static void test_top_origin_with_top_first_storage(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 0, 1 };
    uint32_t slots[8], pixel[4] = { 210, 211, 212, 213 };
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    view.bottom_origin = false;
    view.storage_top_origin = true;
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    g_assert_false(resource.bottom_origin);
    target = plane_target(&planes, &resource);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    g_assert_cmpuint(slots[0], ==, 0);
    g_assert_cmpuint(slots[4], ==, 4);

    rect.y1 = 0;
    result = sgi_render_resource_commit(&target, &resource, 0, &rect,
                                        pixel, 4, ARRAY_SIZE(pixel), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.bytes[0], ==, 210);
    g_assert_cmpuint(f.bytes[20], ==, 4);
}

static void test_covered_top_row_and_aliases(void)
{
    PlaneFixture f;
    OdysseyPlaneView views[2];
    SGIRenderResource resources[2];
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 3, 3 };
    uint32_t slots[64];
    uint8_t mask[16] = { 0 }, expected[80];
    SGIRenderWriteCoverage writes = {
        .pixel = mask, .stride_bytes = 4, .capacity_bytes = sizeof(mask),
    };
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_four_row_views(&f, views);
    views[0].channel_mask = 0x5;
    memcpy(expected, f.bytes, sizeof(expected));
    g_assert_true(odyssey_planes_init(&planes, views, resources, 2, &err));
    target = plane_target(&planes, resources);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resources[0], 0,
                    &rect, slots, 16, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(slots[0], ==, 13);
    g_assert_cmpuint(slots[48], ==, 1);
    g_assert_cmpuint(slots[52], ==, 2);

    result = sgi_render_resource_commit_covered(&target, &resources[0], 0,
                                                &rect, slots, 16,
                                                ARRAY_SIZE(slots), &writes,
                                                &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_cmpuint(result.complete_pixels, ==, 16);
    g_assert_true(result.extent_known);
    g_assert_false(result.partial_pixel);
    g_assert_null(err);
    g_assert_cmpmem(f.bytes, sizeof(f.bytes), expected, sizeof(expected));
    g_assert_cmpuint(f.marks, ==, 0);
    g_assert_false(views[0].dirty);
    g_assert_false(views[1].dirty);

    mask[13] = mask[14] = 1;
    slots[52] = 201;
    slots[54] = 203;
    slots[56] = 211;
    slots[58] = 213;
    result = sgi_render_resource_commit_covered(&target, &resources[0], 0,
                                                &rect, slots, 16,
                                                ARRAY_SIZE(slots), &writes,
                                                &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_cmpuint(result.complete_pixels, ==, 16);
    g_assert_true(result.extent_known);
    g_assert_false(result.partial_pixel);
    g_assert_null(err);
    expected[4] = 201;
    expected[6] = 203;
    expected[8] = 211;
    expected[10] = 213;
    g_assert_cmpmem(f.bytes, sizeof(f.bytes), expected, sizeof(expected));
    g_assert_cmpuint(f.marks, ==, 2);
    for (unsigned i = 0; i < 2; i++) {
        g_assert_true(views[i].dirty);
        g_assert_cmpuint(views[i].backing_generation, ==, 2);
        g_assert_cmpuint(views[i].view_generation, ==, 2);
    }
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resources[1], 0,
                    &rect, slots, 16, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    g_assert_cmpuint(slots[52], ==, 201);
    g_assert_cmpuint(slots[54], ==, 203);
    g_assert_cmpuint(slots[56], ==, 211);
    g_assert_cmpuint(slots[58], ==, 213);
}

static void test_covered_partial_prefix(void)
{
    PlaneFixture f;
    OdysseyPlaneView views[2];
    SGIRenderResource resources[2];
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 3, 3 };
    uint32_t slots[64];
    uint8_t mask[16] = { [13] = 1, [15] = 1 }, expected[80];
    SGIRenderWriteCoverage writes = {
        .pixel = mask, .stride_bytes = 4, .capacity_bytes = sizeof(mask),
    };
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_four_row_views(&f, views);
    f.owned[3] = false;
    memcpy(expected, f.bytes, sizeof(expected));
    g_assert_true(odyssey_planes_init(&planes, views, resources, 2, &err));
    target = plane_target(&planes, resources);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resources[0], 0,
                    &rect, slots, 16, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    slots[52] = 201;
    slots[53] = 202;
    slots[54] = 203;
    slots[55] = 204;
    result = sgi_render_resource_commit_covered(&target, &resources[0], 0,
                                                &rect, slots, 16,
                                                ARRAY_SIZE(slots), &writes,
                                                &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(result.complete_pixels, ==, 15);
    g_assert_true(result.extent_known);
    g_assert_false(result.partial_pixel);
    g_assert_nonnull(err);
    error_free(err);
    expected[4] = 201;
    expected[5] = 202;
    expected[6] = 203;
    expected[7] = 204;
    g_assert_cmpmem(f.bytes, sizeof(f.bytes), expected, sizeof(expected));
    g_assert_cmpuint(f.marks, ==, 1);
    g_assert_false(planes.active);
    for (unsigned i = 0; i < 2; i++) {
        g_assert_true(views[i].dirty);
        g_assert_cmpuint(views[i].backing_generation, ==, 2);
        g_assert_cmpuint(views[i].view_generation, ==, 2);
    }
}

static void test_origin_mismatch_rejected(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint32_t slots[4] = { 0xdead };
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    view.bottom_origin = false;
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    target = plane_target(&planes, &resource);
    resource.bottom_origin = true;
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmphex(slots[0], ==, 0xdead);

    err = NULL;
    resource.bottom_origin = false;
    view.storage_top_origin = true;
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmphex(slots[0], ==, 0xdead);
}

static void test_joint_origin_mutation_rejected(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint32_t snapshot[4] = { 0xdead };
    uint32_t slots[4] = { 80, 81, 82, 83 };
    uint8_t expected[sizeof(f.bytes)];
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    view.bottom_origin = false;
    view.storage_top_origin = true;
    memcpy(expected, f.bytes, sizeof(expected));
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    target = plane_target(&planes, &resource);
    view.bottom_origin = true;
    resource.bottom_origin = true;
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, snapshot, 4, ARRAY_SIZE(snapshot), &err), ==,
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmphex(snapshot[0], ==, 0xdead);
    result = sgi_render_resource_commit(&target, &resource, 0, &rect,
                                        slots, 4, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(result.complete_pixels, ==, 0);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpmem(f.bytes, sizeof(f.bytes), expected, sizeof(expected));
    g_assert_cmpuint(f.marks, ==, 0);
    g_assert_false(view.dirty);
    g_assert_cmpuint(view.backing_generation, ==, 1);
    g_assert_cmpuint(view.view_generation, ==, 1);
}

static void test_direct_callback_slot_bounds(void)
{
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    const SGIRenderResourceOps *ops = odyssey_planes_resource_ops();
    SGIRenderRect rect = { 0, 0, 0, 1 };
    uint32_t out[8];
    uint32_t slots[8] = { 80, 81, 82, 83, 90, 91, 92, 93 };
    uint8_t coverage[1] = { 1 };
    SGIRenderWriteCoverage writes = {
        .pixel = coverage, .stride_bytes = 1,
        .capacity_bytes = sizeof(coverage),
    };
    uint8_t expected[sizeof(f.bytes)];
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_fixture_init(&f);
    view = plane_view(&f, 1);
    memcpy(expected, f.bytes, sizeof(expected));
    for (size_t i = 0; i < ARRAY_SIZE(out); i++) {
        out[i] = 0xdead;
    }
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    g_assert_cmpint(ops->snapshot(&planes, &resource, 0, &rect, out, 4, 7,
                    &err), ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(out); i++) {
        g_assert_cmphex(out[i], ==, 0xdead);
    }
    result = ops->commit(&planes, &resource, 0, &rect, slots, 4, 7, &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(result.complete_pixels, ==, 0);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    result = ops->commit_covered(&planes, &resource, 0, &rect, slots, 4,
                                 ARRAY_SIZE(slots), &writes, &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(result.complete_pixels, ==, 0);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpmem(f.bytes, sizeof(f.bytes), expected, sizeof(expected));
    g_assert_cmpuint(f.marks, ==, 0);
    g_assert_false(view.dirty);
    g_assert_cmpuint(view.backing_generation, ==, 1);
}

static void test_four_eye_alias_generations(void)
{
    PlaneFixture fixtures[4];
    OdysseyPlaneView views[5];
    SGIRenderResource resources[5];
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint32_t slots[4] = { 81, 82, 83, 84 };
    SGIRenderCommitResult result;
    Error *err = NULL;

    plane_four_eyes(fixtures, views);
    views[4] = plane_view(&fixtures[0], 5);
    views[4].backing_id = 100;
    views[4].target = 15;
    g_assert_true(odyssey_planes_init(&planes, views, resources, 5, &err));
    target = plane_target(&planes, resources);
    result = sgi_render_resource_commit(&target, &resources[0], 0, &rect,
                                        slots, 4, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_true(views[0].dirty);
    g_assert_true(views[4].dirty);
    g_assert_cmpuint(views[0].backing_generation, ==, 2);
    g_assert_cmpuint(views[4].backing_generation, ==, 2);
    for (unsigned i = 1; i < 4; i++) {
        g_assert_false(views[i].dirty);
        g_assert_cmpuint(views[i].backing_generation, ==, 1);
    }
}

static void test_duplicate_eye_or_target_rejected(void)
{
    PlaneFixture fixtures[4];
    OdysseyPlaneView views[4];
    SGIRenderResource resources[4];
    OdysseyPlanes planes = { 0 };
    Error *err = NULL;

    plane_four_eyes(fixtures, views);
    views[1].eye = SGI_RENDER_LEFT;
    g_assert_false(odyssey_planes_init(&planes, views, resources, 4, &err));
    g_assert_nonnull(err);
    error_free(err);

    err = NULL;
    plane_four_eyes(fixtures, views);
    views[1].target = views[0].target;
    g_assert_false(odyssey_planes_init(&planes, views, resources, 4, &err));
    g_assert_nonnull(err);
    error_free(err);

    err = NULL;
    plane_four_eyes(fixtures, views);
    views[1].bytes = views[0].bytes;
    g_assert_false(odyssey_planes_init(&planes, views, resources, 4, &err));
    g_assert_nonnull(err);
    error_free(err);
}

static void test_four_eye_swap_changes_bindings(void)
{
    PlaneFixture fixtures[4];
    OdysseyPlaneView views[4];
    SGIRenderResource resources[4], next_resources[4];
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint32_t slots[4];
    Error *err = NULL;

    plane_four_eyes(fixtures, views);
    g_assert_true(odyssey_planes_init(&planes, views, resources, 4, &err));
    target = plane_target(&planes, resources);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmpint(sgi_render_resource_snapshot(&target, &resources[i],
                        0, &rect, slots, 4, ARRAY_SIZE(slots), &err), ==,
                        SGI_RENDER_OK);
        g_assert_cmpuint(slots[0], ==, (i + 1) * 10);
    }
    odyssey_planes_end_submission(&planes);
    g_assert_true(odyssey_planes_swap_bindings(&planes, &err));
    g_assert_null(err);
    g_assert_cmpint(views[0].buffer, ==, SGI_RENDER_BACK);
    g_assert_cmpint(views[1].buffer, ==, SGI_RENDER_BACK);
    g_assert_cmpint(views[2].buffer, ==, SGI_RENDER_FRONT);
    g_assert_cmpint(views[3].buffer, ==, SGI_RENDER_FRONT);
    g_assert_cmpuint(views[0].target, ==, 13);
    g_assert_cmpuint(views[2].target, ==, 11);
    g_assert_cmpuint(views[1].target, ==, 14);
    g_assert_cmpuint(views[3].target, ==, 12);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_true(views[i].dirty);
        g_assert_cmpuint(views[i].backing_generation, ==, 1);
        g_assert_cmpuint(views[i].view_generation, ==, 2);
        g_assert_true(views[i].bytes == fixtures[i].bytes);
        g_assert_cmpuint(fixtures[i].bytes[0], ==, (i + 1) * 10);
    }
    g_assert_true(odyssey_planes_refresh(&planes, next_resources, &err));
    target = plane_target(&planes, next_resources);
    g_assert_cmpint(sgi_render_resource_snapshot(&target,
                    &next_resources[2], 0, &rect, slots, 4,
                    ARRAY_SIZE(slots), &err), ==, SGI_RENDER_OK);
    g_assert_cmpuint(next_resources[2].target, ==, 11);
    g_assert_cmpuint(slots[0], ==, 30);
    g_assert_cmpint(sgi_render_resource_snapshot(&target,
                    &next_resources[3], 0, &rect, slots, 4,
                    ARRAY_SIZE(slots), &err), ==, SGI_RENDER_OK);
    g_assert_cmpuint(next_resources[3].target, ==, 12);
    g_assert_cmpuint(slots[0], ==, 40);
}

static void test_swap_requires_four_views(void)
{
    PlaneFixture fixtures[4];
    OdysseyPlaneView views[4];
    SGIRenderResource resources[3];
    OdysseyPlanes planes = { 0 };
    Error *err = NULL;

    plane_four_eyes(fixtures, views);
    g_assert_true(odyssey_planes_init(&planes, views, resources, 3, &err));
    odyssey_planes_end_submission(&planes);
    g_assert_false(odyssey_planes_swap_bindings(&planes, &err));
    g_assert_nonnull(err);
    error_free(err);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_false(views[i].dirty);
        g_assert_cmpuint(views[i].view_generation, ==, 1);
        g_assert_cmpuint(views[i].target, ==, 11 + i);
    }
}

static void test_swap_uses_declared_positions(void)
{
    PlaneFixture fixtures[4];
    OdysseyPlaneView ordered[4], views[4];
    SGIRenderResource resources[4];
    OdysseyPlanes planes = { 0 };
    Error *err = NULL;

    plane_four_eyes(fixtures, ordered);
    views[0] = ordered[3];
    views[1] = ordered[0];
    views[2] = ordered[2];
    views[3] = ordered[1];
    g_assert_true(odyssey_planes_init(&planes, views, resources, 4, &err));
    odyssey_planes_end_submission(&planes);
    g_assert_true(odyssey_planes_swap_bindings(&planes, &err));
    g_assert_null(err);
    g_assert_cmpint(views[0].buffer, ==, SGI_RENDER_FRONT);
    g_assert_cmpint(views[0].eye, ==, SGI_RENDER_RIGHT);
    g_assert_cmpuint(views[0].target, ==, 12);
    g_assert_cmpint(views[1].buffer, ==, SGI_RENDER_BACK);
    g_assert_cmpint(views[1].eye, ==, SGI_RENDER_LEFT);
    g_assert_cmpuint(views[1].target, ==, 13);
    g_assert_cmpint(views[2].buffer, ==, SGI_RENDER_FRONT);
    g_assert_cmpuint(views[2].target, ==, 11);
    g_assert_cmpint(views[3].buffer, ==, SGI_RENDER_BACK);
    g_assert_cmpuint(views[3].target, ==, 14);
}

static void test_swap_rejects_changed_bindings_atomically(void)
{
    PlaneFixture fixtures[4];
    OdysseyPlaneView views[4];
    SGIRenderResource resources[4];
    OdysseyPlanes planes = { 0 };
    Error *err = NULL;

    plane_four_eyes(fixtures, views);
    g_assert_true(odyssey_planes_init(&planes, views, resources, 4, &err));
    odyssey_planes_end_submission(&planes);
    views[0].target = views[1].target;
    g_assert_false(odyssey_planes_swap_bindings(&planes, &err));
    g_assert_nonnull(err);
    error_free(err);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmpuint(views[i].target, ==,
                         i == 0 ? 12 : 11 + i);
        g_assert_cmpint(views[i].buffer, ==,
                        i < 2 ? SGI_RENDER_FRONT : SGI_RENDER_BACK);
        g_assert_cmpint(views[i].eye, ==,
                        i % 2 ? SGI_RENDER_RIGHT : SGI_RENDER_LEFT);
        g_assert_cmpuint(views[i].view_generation, ==, 1);
        g_assert_false(views[i].dirty);
    }

    err = NULL;
    planes = (OdysseyPlanes) { 0 };
    plane_four_eyes(fixtures, views);
    g_assert_true(odyssey_planes_init(&planes, views, resources, 4, &err));
    odyssey_planes_end_submission(&planes);
    views[0].eye = SGI_RENDER_RIGHT;
    views[1].eye = SGI_RENDER_LEFT;
    g_assert_false(odyssey_planes_swap_bindings(&planes, &err));
    g_assert_nonnull(err);
    error_free(err);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmpuint(views[i].target, ==, 11 + i);
        g_assert_cmpint(views[i].buffer, ==,
                        i < 2 ? SGI_RENDER_FRONT : SGI_RENDER_BACK);
        g_assert_cmpint(views[i].eye, ==,
                        i == 0 ? SGI_RENDER_RIGHT :
                        i == 1 ? SGI_RENDER_LEFT :
                        i == 2 ? SGI_RENDER_LEFT : SGI_RENDER_RIGHT);
        g_assert_cmpuint(views[i].view_generation, ==, 1);
        g_assert_false(views[i].dirty);
    }
}

static void test_swap_rejects_changed_storage_origin(void)
{
    PlaneFixture fixtures[4];
    OdysseyPlaneView views[4];
    SGIRenderResource resources[4];
    OdysseyPlanes planes = { 0 };
    Error *err = NULL;

    plane_four_eyes(fixtures, views);
    g_assert_true(odyssey_planes_init(&planes, views, resources, 4, &err));
    odyssey_planes_end_submission(&planes);
    views[0].storage_top_origin = true;
    g_assert_false(odyssey_planes_swap_bindings(&planes, &err));
    g_assert_nonnull(err);
    error_free(err);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmpint(views[i].buffer, ==,
                        i < 2 ? SGI_RENDER_FRONT : SGI_RENDER_BACK);
        g_assert_cmpuint(views[i].target, ==, 11 + i);
        g_assert_cmpuint(views[i].view_generation, ==, 1);
        g_assert_false(views[i].dirty);
    }
}

static void test_swap_rejects_joint_origin_mutation(void)
{
    PlaneFixture fixtures[4];
    OdysseyPlaneView views[4];
    SGIRenderResource resources[4];
    OdysseyPlanes planes = { 0 };
    Error *err = NULL;

    plane_four_eyes(fixtures, views);
    g_assert_true(odyssey_planes_init(&planes, views, resources, 4, &err));
    odyssey_planes_end_submission(&planes);
    views[0].bottom_origin = false;
    resources[0].bottom_origin = false;
    g_assert_false(odyssey_planes_swap_bindings(&planes, &err));
    g_assert_nonnull(err);
    error_free(err);
    for (size_t i = 0; i < 4; i++) {
        g_assert_cmpint(views[i].buffer, ==,
                        i < 2 ? SGI_RENDER_FRONT : SGI_RENDER_BACK);
        g_assert_cmpuint(views[i].target, ==, 11 + i);
        g_assert_cmpuint(views[i].view_generation, ==, 1);
        g_assert_false(views[i].dirty);
    }
}

#ifdef CONFIG_OPENGL
static void test_checked_draw_top_first_storage(void)
{
    static const float xy[3][2] = {
        { -1, -1 }, { 3, -1 }, { -1, 3 },
    };
    PlaneFixture f;
    OdysseyPlaneView view;
    SGIRenderResource resource;
    OdysseyPlanes planes = { 0 };
    SGIRasterTarget target;
    SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8, .alpha_bits = 8,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .evidence = "synthetic",
    };
    SGIGLDrawState state = {
        .shade_model = SGI_GL_FLAT,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .drawable = {
            .bound = true,
            .clip = { .n = 1, .r[0] = { 1, 3, 2, 3 } },
            .front = 1,
        },
        .viewport = { 0, 0, 4, 4 },
    };
    float vertices[3][SGI_GL_VERTEX_FLOATS] = { 0 };
    SGIGLDraw draw = {
        .mode = SGI_GL_TRIANGLES, .count = 3,
        .vertices = &vertices[0][0], .state = &state,
    };
    SGIRenderRect rect = { 0, 0, 3, 3 };
    uint8_t expected[80];
    uint32_t slots[64];
    SGIRenderer *renderer;
    Error *err = NULL;

    if (!g_getenv("QEMU_TEST_SGI_REQUIRE_MESA")) {
        g_test_skip("Set QEMU_TEST_SGI_REQUIRE_MESA for the Mesa adapter test");
        return;
    }
    g_setenv("LIBGL_ALWAYS_SOFTWARE", "1", true);
    plane_assert_surfaceless_gl();
    plane_four_row_fixture_init(&f);
    view = plane_view(&f, 1);
    view.height = 4;
    view.capacity_bytes = sizeof(f.bytes);
    view.storage_top_origin = true;
    memcpy(expected, f.bytes, sizeof(expected));
    g_assert_true(odyssey_planes_init(&planes, &view, &resource, 1, &err));
    target = plane_target(&planes, &resource);
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
    g_assert_cmpint(sgi_render_draw_checked(renderer, &rules, &target,
                                            &draw, &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    for (unsigned x = 1; x <= 2; x++) {
        memset(&expected[x * 4], 0, 4);
        expected[x * 4] = expected[x * 4 + 3] = 255;
    }
    g_assert_cmpmem(f.bytes, sizeof(f.bytes), expected, sizeof(expected));
    g_assert_cmpuint(f.marks, ==, 2);
    g_assert_true(view.dirty);
    g_assert_cmpuint(view.backing_generation, ==, 2);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &resource, 0,
                    &rect, slots, 16, ARRAY_SIZE(slots), &err), ==,
                    SGI_RENDER_OK);
    g_assert_null(err);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            unsigned backing = (3 - y) * 20 + x * 4;
            unsigned pixel = (y * 4 + x) * 4;

            for (unsigned c = 0; c < 4; c++) {
                g_assert_cmpuint(slots[pixel + c], ==, expected[backing + c]);
            }
        }
    }
    sgi_renderer_free(renderer);
}
#endif

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
#ifdef CONFIG_OPENGL
    module_call_init(MODULE_INIT_QOM);
#endif
    g_test_add_func("/sgi-odyssey-planes/padded-roundtrip",
                    test_padded_roundtrip);
    g_test_add_func("/sgi-odyssey-planes/stale-generation",
                    test_stale_generation);
    g_test_add_func("/sgi-odyssey-planes/unwritten-unowned",
                    test_unwritten_and_unowned_snapshot);
    g_test_add_func("/sgi-odyssey-planes/partial-alias",
                    test_partial_commit_invalidates_alias);
    g_test_add_func("/sgi-odyssey-planes/channel-mask",
                    test_mask_preserves_other_channels);
    g_test_add_func("/sgi-odyssey-planes/rgba10-raw-precision",
                    test_rgba10_raw_precision);
    g_test_add_func("/sgi-odyssey-planes/ci12-raw-copy-mask",
                    test_ci12_raw_copy_and_mask);
    g_test_add_func("/sgi-odyssey-planes/raw-no-change-mask",
                    test_raw_no_change_has_no_changed_components);
    g_test_add_func("/sgi-odyssey-planes/raw-format-maxima-masks",
                    test_raw_format_maxima_and_masks);
    g_test_add_func("/sgi-odyssey-planes/raw-multi-prevalidate",
                    test_raw_multi_prevalidates_all_writes);
    g_test_add_func("/sgi-odyssey-planes/raw-multi-partial",
                    test_raw_multi_reports_later_partial_failure);
    g_test_add_func("/sgi-odyssey-planes/raw-partial-alias",
                    test_raw_partial_commit_invalidates_alias);
    g_test_add_func("/sgi-odyssey-planes/raw-changed-binding",
                    test_raw_changed_binding_rejected);
    g_test_add_func("/sgi-odyssey-planes/raw-masked-invalid-preserved",
                    test_raw_mask_rejects_invalid_preserved_component);
    g_test_add_func("/sgi-odyssey-planes/deep-overlap-copy",
                    test_deep_overlapping_raw_copy);
    g_test_add_func("/sgi-odyssey-planes/cross-format-copy",
                    test_raw_cross_format_copy_rejected);
    g_test_add_func("/sgi-odyssey-planes/unsupported-format",
                    test_unknown_format_rejected);
    g_test_add_func("/sgi-odyssey-planes/short-backing",
                    test_short_backing_rejected);
    g_test_add_func("/sgi-odyssey-planes/changed-component",
                    test_changed_component_rejected);
    g_test_add_func("/sgi-odyssey-planes/false-alias",
                    test_false_alias_rejected);
    g_test_add_func("/sgi-odyssey-planes/alias-submission",
                    test_alias_commits_share_submission);
    g_test_add_func("/sgi-odyssey-planes/external-rebind",
                    test_external_rebind_stales_alias);
    g_test_add_func("/sgi-odyssey-planes/refresh-stales-snapshot",
                    test_refresh_does_not_retarget_snapshot);
    g_test_add_func("/sgi-odyssey-planes/reinit-stales-snapshot",
                    test_reinit_does_not_retarget_snapshot);
    g_test_add_func("/sgi-odyssey-planes/four-eye-isolation",
                    test_four_eye_isolation);
    g_test_add_func("/sgi-odyssey-planes/raw-four-eye-swap",
                    test_raw_four_eye_swap);
    g_test_add_func("/sgi-odyssey-planes/top-origin-rows",
                    test_top_origin_uses_plane_rows);
    g_test_add_func("/sgi-odyssey-planes/top-first-storage",
                    test_bottom_origin_with_top_first_storage);
    g_test_add_func("/sgi-odyssey-planes/top-first-three-rows",
                    test_top_first_storage_three_rows);
    g_test_add_func("/sgi-odyssey-planes/top-first-top-resource",
                    test_top_origin_with_top_first_storage);
    g_test_add_func("/sgi-odyssey-planes/covered-top-row",
                    test_covered_top_row_and_aliases);
    g_test_add_func("/sgi-odyssey-planes/covered-partial-prefix",
                    test_covered_partial_prefix);
    g_test_add_func("/sgi-odyssey-planes/origin-mismatch",
                    test_origin_mismatch_rejected);
    g_test_add_func("/sgi-odyssey-planes/joint-origin-mutation",
                    test_joint_origin_mutation_rejected);
    g_test_add_func("/sgi-odyssey-planes/direct-slot-bounds",
                    test_direct_callback_slot_bounds);
    g_test_add_func("/sgi-odyssey-planes/four-eye-alias",
                    test_four_eye_alias_generations);
    g_test_add_func("/sgi-odyssey-planes/duplicate-eye-target",
                    test_duplicate_eye_or_target_rejected);
    g_test_add_func("/sgi-odyssey-planes/four-eye-swap",
                    test_four_eye_swap_changes_bindings);
    g_test_add_func("/sgi-odyssey-planes/swap-requires-four",
                    test_swap_requires_four_views);
    g_test_add_func("/sgi-odyssey-planes/swap-declared-positions",
                    test_swap_uses_declared_positions);
    g_test_add_func("/sgi-odyssey-planes/swap-changed-bindings",
                    test_swap_rejects_changed_bindings_atomically);
    g_test_add_func("/sgi-odyssey-planes/swap-changed-storage-origin",
                    test_swap_rejects_changed_storage_origin);
    g_test_add_func("/sgi-odyssey-planes/swap-joint-origin-mutation",
                    test_swap_rejects_joint_origin_mutation);
#ifdef CONFIG_OPENGL
    g_test_add_func("/sgi-odyssey-planes/checked-draw-top-first-storage",
                    test_checked_draw_top_first_storage);
#endif
    return g_test_run();
}
