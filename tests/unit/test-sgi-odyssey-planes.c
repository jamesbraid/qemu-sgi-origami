/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/sgi-odyssey-planes.h"

typedef struct PlaneFixture {
    uint8_t bytes[40];
    bool written[8];
    bool owned[8];
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
        .capacity_bytes = sizeof(f->bytes),
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

static void test_unwritten_and_foreign_snapshot(void)
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
                    SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmphex(slots[0], ==, 0xdead);
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-odyssey-planes/padded-roundtrip",
                    test_padded_roundtrip);
    g_test_add_func("/sgi-odyssey-planes/stale-generation",
                    test_stale_generation);
    g_test_add_func("/sgi-odyssey-planes/unwritten-foreign",
                    test_unwritten_and_foreign_snapshot);
    g_test_add_func("/sgi-odyssey-planes/partial-alias",
                    test_partial_commit_invalidates_alias);
    g_test_add_func("/sgi-odyssey-planes/channel-mask",
                    test_mask_preserves_other_channels);
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
    return g_test_run();
}
