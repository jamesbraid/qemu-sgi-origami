/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/display/sgi/rasterizer.h"

typedef struct RenderResourceFixture {
    uint32_t native[4][4];
    uint32_t alpha[4];
    uint32_t tag[4];
    unsigned snapshots, commits, dirty;
    unsigned covered_commits;
    size_t fail_after;
} RenderResourceFixture;

static SGIRenderResult render_resource_snapshot(void *opaque,
                                                 const SGIRenderResource *res,
                                                 uint32_t slice,
                                                 const SGIRenderRect *rect,
                                                 uint32_t *slots,
                                                 size_t stride_slots,
                                                 size_t capacity_slots,
                                                 Error **errp)
{
    RenderResourceFixture *f = opaque;

    g_assert_cmpuint(slice, ==, 0);
    g_assert_cmpuint(res->component_count, ==, 3);
    g_assert_cmpuint(capacity_slots, >=,
                     (rect->x1 - rect->x0 + 1) * 3);
    f->snapshots++;
    for (uint32_t x = rect->x0; x <= rect->x1; x++) {
        for (unsigned c = 0; c < 3; c++) {
            slots[(x - rect->x0) * 3 + c] = f->native[x][c];
        }
    }
    return SGI_RENDER_OK;
}

static SGIRenderCommitResult render_resource_commit(void *opaque,
                                                  const SGIRenderResource *res,
                                                  uint32_t slice,
                                                  const SGIRenderRect *rect,
                                                  const uint32_t *slots,
                                                  size_t stride_slots,
                                                  size_t capacity_slots,
                                                  Error **errp)
{
    RenderResourceFixture *f = opaque;
    size_t count = rect->x1 - rect->x0 + 1;

    g_assert_cmpuint(slice, ==, 0);
    g_assert_cmpuint(res->component_count, ==, 3);
    g_assert_cmpuint(capacity_slots, >=,
                     (rect->x1 - rect->x0 + 1) * 3);
    f->commits++;
    for (size_t i = 0; i < count; i++) {
        if (i == f->fail_after) {
            error_setg(errp, "native commit stopped after %zu pixels", i);
            return (SGIRenderCommitResult) {
                .status = SGI_RENDER_RESOURCE_ERROR,
                .complete_pixels = i,
                .extent_known = true,
            };
        }
        for (unsigned c = 0; c < 3; c++) {
            f->native[rect->x0 + i][c] = slots[i * 3 + c];
        }
        f->dirty |= 1u << (rect->x0 + i);
    }
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_OK,
        .complete_pixels = count,
        .extent_known = true,
    };
}

static SGIRenderCommitResult render_resource_commit_covered(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots,
    size_t stride_slots, size_t capacity_slots,
    const SGIRenderWriteCoverage *writes, Error **errp)
{
    RenderResourceFixture *f = opaque;
    size_t width = rect->x1 - rect->x0 + 1;

    g_assert_cmpuint(slice, ==, 0);
    g_assert_cmpuint(rect->y0, ==, 0);
    g_assert_cmpuint(rect->y1, ==, 0);
    g_assert_cmpuint(res->component_count, ==, 3);
    f->covered_commits++;
    for (size_t i = 0; i < width; i++) {
        unsigned x = rect->x0 + i;

        if (!writes->pixel[i]) {
            continue;
        }
        if (i == f->fail_after) {
            f->native[x][0] = slots[i * 3];
            f->dirty |= 1u << x;
            error_setg(errp, "paired native write stopped at pixel %zu", i);
            return (SGIRenderCommitResult) {
                .status = SGI_RENDER_RESOURCE_ERROR,
                .complete_pixels = i,
                .partial_pixel = true,
                .extent_known = true,
            };
        }
        for (unsigned c = 0; c < 3; c++) {
            f->native[x][c] = slots[i * 3 + c];
        }
        f->tag[x] = 9;
        f->dirty |= 1u << x;
    }
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_OK,
        .complete_pixels = width,
        .extent_known = true,
    };
}

static const SGIRenderResourceOps render_resource_ops = {
    .snapshot = render_resource_snapshot,
    .commit = render_resource_commit,
    .commit_covered = render_resource_commit_covered,
};

static SGIRenderResource render_colour_resource(void)
{
    return (SGIRenderResource) {
        .backing_id = 1, .backing_generation = 1,
        .view_id = 2, .view_generation = 1,
        .role = SGI_RENDER_COLOUR, .target = 7,
        .buffer = SGI_RENDER_BACK, .eye = SGI_RENDER_LEFT,
        .width = 4, .height = 1, .depth = 1,
        .component_count = 3,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, 12 },
            { SGI_RENDER_G, SGI_RENDER_UNORM, 12 },
            { SGI_RENDER_B, SGI_RENDER_UNORM, 12 },
        },
    };
}

static void test_render_resource_precision(void)
{
    RenderResourceFixture f = {
        .native = {
            { 0, 4095, 17 }, { 1, 4094, 18 },
            { 4094, 1, 4095 }, { 4095, 0, 0 },
        },
        .alpha = { 11, 22, 33, 44 },
        .fail_after = SIZE_MAX,
    };
    SGIRenderResource res = render_colour_resource();
    SGIRasterTarget target = {
        .opaque = &f, .resources = &res, .resource_count = 1,
        .resource_ops = &render_resource_ops,
    };
    SGIRenderRect rect = { 0, 0, 3, 0 };
    uint32_t slots[16];
    SGIRenderCommitResult committed;
    Error *err = NULL;
    const uint32_t expected[12] = {
        0, 4095, 17, 1, 4094, 18, 4094, 1, 4095, 4095, 0, 0,
    };

    for (unsigned i = 0; i < ARRAY_SIZE(slots); i++) {
        slots[i] = 0xdead;
    }
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &res, 0, &rect,
                                                  slots, 12, ARRAY_SIZE(slots),
                                                  &err), ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpmem(slots, sizeof(expected), expected, sizeof(expected));
    g_assert_cmphex(slots[12], ==, 0xdead);
    slots[0] = 4095;
    slots[2] = 4094;
    committed = sgi_render_resource_commit(&target, &res, 0, &rect,
                                           slots, 12, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_OK);
    g_assert_cmpuint(committed.complete_pixels, ==, 4);
    g_assert_true(committed.extent_known);
    g_assert_false(committed.partial_pixel);
    g_assert_null(err);
    g_assert_cmpuint(f.snapshots, ==, 1);
    g_assert_cmpuint(f.commits, ==, 1);
    g_assert_cmphex(f.dirty, ==, 0xf);
    g_assert_cmpuint(f.native[0][0], ==, 4095);
    g_assert_cmpuint(f.native[0][2], ==, 4094);
    g_assert_cmpmem(f.alpha, sizeof(f.alpha),
                    ((uint32_t[4]) { 11, 22, 33, 44 }), sizeof(f.alpha));
}

static void test_render_resource_invalid_span(void)
{
    RenderResourceFixture f = { 0 };
    SGIRenderResource res = render_colour_resource();
    SGIRasterTarget target = {
        .opaque = &f, .resources = &res, .resource_count = 1,
        .resource_ops = &render_resource_ops,
    };
    SGIRenderRect rect = { 0, 0, 3, 1 };
    uint32_t slots[16] = { 0 };
    Error *err = NULL;

    res.height = 2;
    slots[0] = 0xdead;
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &res, 0, &rect,
                                                  slots, SIZE_MAX,
                                                  ARRAY_SIZE(slots), &err),
                    ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.snapshots, ==, 0);
    g_assert_cmphex(slots[0], ==, 0xdead);
}

static void test_render_resource_partial_commit(void)
{
    RenderResourceFixture f = {
        .native = {
            { 1, 2, 3 }, { 4, 5, 6 },
            { 7, 8, 9 }, { 10, 11, 12 },
        },
        .fail_after = 2,
    };
    SGIRenderResource res = render_colour_resource();
    SGIRasterTarget target = {
        .opaque = &f, .resources = &res, .resource_count = 1,
        .resource_ops = &render_resource_ops,
    };
    SGIRenderRect rect = { 0, 0, 3, 0 };
    uint32_t slots[12] = {
        4095, 0, 1, 4094, 1, 2, 4093, 2, 3, 4092, 3, 4,
    };
    SGIRenderCommitResult committed;
    Error *err = NULL;

    committed = sgi_render_resource_commit(&target, &res, 0, &rect,
                                           slots, 12, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(committed.complete_pixels, ==, 2);
    g_assert_true(committed.extent_known);
    g_assert_false(committed.partial_pixel);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.commits, ==, 1);
    g_assert_cmphex(f.dirty, ==, 0x3);
    g_assert_cmpuint(f.native[0][0], ==, 4095);
    g_assert_cmpuint(f.native[1][0], ==, 4094);
    g_assert_cmpuint(f.native[2][0], ==, 7);
    g_assert_cmpuint(f.native[3][0], ==, 10);
}

static void test_render_resource_covered_commit(void)
{
    RenderResourceFixture f = {
        .native = {
            { 1, 2, 3 }, { 4, 5, 6 },
            { 7, 8, 9 }, { 10, 11, 12 },
        },
        .tag = { 1, 1, 1, 1 },
        .fail_after = SIZE_MAX,
    };
    SGIRenderResource res = render_colour_resource();
    SGIRasterTarget target = {
        .opaque = &f, .resources = &res, .resource_count = 1,
        .resource_ops = &render_resource_ops,
    };
    SGIRenderRect rect = { 0, 0, 3, 0 };
    uint32_t slots[12] = {
        100, 101, 102, 4, 5, 6, 200, 201, 202, 300, 301, 302,
    };
    uint8_t mask[4] = { 0, 1, 0, 1 };
    SGIRenderWriteCoverage writes = {
        .pixel = mask, .stride_bytes = 4, .capacity_bytes = 4,
    };
    SGIRenderCommitResult committed;
    Error *err = NULL;

    committed = sgi_render_resource_commit_covered(&target, &res, 0, &rect,
                                                    slots, 12,
                                                    ARRAY_SIZE(slots),
                                                    &writes, &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_OK);
    g_assert_cmpuint(committed.complete_pixels, ==, 4);
    g_assert_true(committed.extent_known);
    g_assert_null(err);
    g_assert_cmpuint(f.covered_commits, ==, 1);
    g_assert_cmphex(f.dirty, ==, 0xa);
    g_assert_cmpuint(f.native[0][0], ==, 1);
    g_assert_cmpuint(f.native[1][0], ==, 4);
    g_assert_cmpuint(f.native[2][0], ==, 7);
    g_assert_cmpuint(f.native[3][0], ==, 300);
    g_assert_cmpmem(f.tag, sizeof(f.tag),
                    ((uint32_t[4]) { 1, 9, 1, 9 }), sizeof(f.tag));

    mask[2] = 2;
    committed = sgi_render_resource_commit_covered(&target, &res, 0, &rect,
                                                    slots, 12,
                                                    ARRAY_SIZE(slots),
                                                    &writes, &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.covered_commits, ==, 1);
    mask[2] = 0;

    writes.capacity_bytes = 3;
    committed = sgi_render_resource_commit_covered(&target, &res, 0, &rect,
                                                    slots, 12,
                                                    ARRAY_SIZE(slots),
                                                    &writes, &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.covered_commits, ==, 1);

    SGIRenderResourceOps ops = render_resource_ops;

    ops.commit_covered = NULL;
    target.resource_ops = &ops;
    writes.capacity_bytes = 4;
    committed = sgi_render_resource_commit_covered(&target, &res, 0, &rect,
                                                    slots, 12,
                                                    ARRAY_SIZE(slots),
                                                    &writes, &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_UNSUPPORTED);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.covered_commits, ==, 1);
    g_assert_cmpuint(f.commits, ==, 0);
}

typedef struct RenderIndexFixture {
    uint32_t native[4];
    unsigned commits;
    uint32_t mask;
} RenderIndexFixture;

static SGIRenderCommitResult render_index_commit(
    void *opaque, const SGIRenderResource *res, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots,
    size_t stride_slots, size_t capacity_slots,
    const SGIRenderWriteCoverage *writes, uint32_t effective_mask,
    Error **errp)
{
    RenderIndexFixture *f = opaque;
    size_t width = rect->x1 - rect->x0 + 1;

    f->commits++;
    f->mask = effective_mask;
    for (size_t i = 0; i < width; i++) {
        if (writes->pixel[i]) {
            f->native[rect->x0 + i] = slots[i];
        }
    }
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_OK,
        .complete_pixels = width,
        .extent_known = true,
    };
}

/*
 * The index commit takes final indices and the effective mask for a
 * one-component unsigned index resource only, with no mask bit above its
 * storage width, and needs its own provider.
 */
static void test_render_resource_index_commit(void)
{
    static const SGIRenderResourceOps ops = {
        .commit_index_covered = render_index_commit,
    };
    RenderIndexFixture f = { .native = { 1, 2, 3, 4 } };
    SGIRenderResource res = {
        .role = SGI_RENDER_INDEX, .width = 4, .height = 1, .depth = 1,
        .component_count = 1,
        .component = { { SGI_RENDER_SCALAR, SGI_RENDER_UINT, 12 } },
    };
    SGIRasterTarget target = {
        .opaque = &f, .resources = &res, .resource_count = 1,
        .resource_ops = &ops,
    };
    SGIRenderRect rect = { 0, 0, 3, 0 };
    uint32_t slots[4] = { 0xfff, 0x123, 0, 0x800 };
    uint8_t mask[4] = { 1, 0, 1, 1 };
    SGIRenderWriteCoverage writes = {
        .pixel = mask, .stride_bytes = 4, .capacity_bytes = 4,
    };
    SGIRenderResourceOps none = { 0 };
    SGIRenderCommitResult committed;
    Error *err = NULL;

    committed = sgi_render_resource_commit_index_covered(
        &target, &res, 0, &rect, slots, 4, 4, &writes, 0xff0, &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmpuint(f.commits, ==, 1);
    g_assert_cmphex(f.mask, ==, 0xff0);
    g_assert_cmpmem(f.native, sizeof(f.native),
                    ((uint32_t[4]) { 0xfff, 2, 0, 0x800 }), sizeof(f.native));

    /* A mask bit above the storage width. */
    committed = sgi_render_resource_commit_index_covered(
        &target, &res, 0, &rect, slots, 4, 4, &writes, 0x1000, &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_clear_pointer(&err, error_free);
    /* An index above the storage width. */
    slots[0] = 0x1000;
    committed = sgi_render_resource_commit_index_covered(
        &target, &res, 0, &rect, slots, 4, 4, &writes, 0xfff, &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_HOST_ERROR);
    g_clear_pointer(&err, error_free);
    slots[0] = 0xfff;
    /* Not an index resource, or not one unsigned scalar. */
    res.role = SGI_RENDER_COLOUR;
    committed = sgi_render_resource_commit_index_covered(
        &target, &res, 0, &rect, slots, 4, 4, &writes, 0xfff, &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_clear_pointer(&err, error_free);
    res.role = SGI_RENDER_INDEX;
    res.component[0].numeric = SGI_RENDER_UNORM;
    committed = sgi_render_resource_commit_index_covered(
        &target, &res, 0, &rect, slots, 4, 4, &writes, 0xfff, &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_clear_pointer(&err, error_free);
    res.component[0].numeric = SGI_RENDER_UINT;
    /* Coverage that is not binary. */
    mask[1] = 2;
    committed = sgi_render_resource_commit_index_covered(
        &target, &res, 0, &rect, slots, 4, 4, &writes, 0xfff, &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_clear_pointer(&err, error_free);
    mask[1] = 0;
    /* No provider. */
    target.resource_ops = &none;
    committed = sgi_render_resource_commit_index_covered(
        &target, &res, 0, &rect, slots, 4, 4, &writes, 0xfff, &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_UNSUPPORTED);
    g_clear_pointer(&err, error_free);
    g_assert_cmpuint(f.commits, ==, 1);
    /* A 32-bit resource takes a full mask. */
    target.resource_ops = &ops;
    res.component[0].storage_bits = 32;
    slots[1] = UINT32_MAX;
    mask[1] = 1;
    committed = sgi_render_resource_commit_index_covered(
        &target, &res, 0, &rect, slots, 4, 4, &writes, UINT32_MAX, &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_OK);
    g_assert_null(err);
    g_assert_cmphex(f.native[1], ==, UINT32_MAX);
}

static void test_render_resource_covered_partial(void)
{
    RenderResourceFixture f = {
        .native = {
            { 1, 2, 3 }, { 4, 5, 6 },
            { 7, 8, 9 }, { 10, 11, 12 },
        },
        .tag = { 1, 1, 1, 1 },
        .fail_after = 2,
    };
    SGIRenderResource res = render_colour_resource();
    SGIRasterTarget target = {
        .opaque = &f, .resources = &res, .resource_count = 1,
        .resource_ops = &render_resource_ops,
    };
    SGIRenderRect rect = { 0, 0, 3, 0 };
    uint32_t slots[12] = {
        100, 101, 102, 200, 201, 202, 300, 301, 302, 400, 401, 402,
    };
    uint8_t mask[4] = { 0, 1, 1, 0 };
    SGIRenderWriteCoverage writes = {
        .pixel = mask, .stride_bytes = 4, .capacity_bytes = 4,
    };
    SGIRenderCommitResult committed;
    Error *err = NULL;

    committed = sgi_render_resource_commit_covered(&target, &res, 0, &rect,
                                                    slots, 12,
                                                    ARRAY_SIZE(slots),
                                                    &writes, &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(committed.complete_pixels, ==, 2);
    g_assert_true(committed.partial_pixel);
    g_assert_true(committed.extent_known);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmphex(f.dirty, ==, 0x6);
    g_assert_cmpuint(f.native[0][0], ==, 1);
    g_assert_cmpuint(f.native[1][0], ==, 200);
    g_assert_cmpuint(f.native[2][0], ==, 300);
    g_assert_cmpuint(f.native[2][1], ==, 8);
    g_assert_cmpmem(f.tag, sizeof(f.tag),
                    ((uint32_t[4]) { 1, 9, 1, 1 }), sizeof(f.tag));
}

static void test_render_resource_rejects_attachment_border(void)
{
    RenderResourceFixture f = { 0 };
    SGIRenderResource res = render_colour_resource();
    SGIRasterTarget target = {
        .opaque = &f, .resources = &res, .resource_count = 1,
        .resource_ops = &render_resource_ops,
    };
    SGIRenderRect rect = { 0, 0, 1, 0 };
    uint32_t slots[12] = { 0 };
    Error *err = NULL;

    res.border[0] = 1;
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &res, 0, &rect,
                                                  slots, 6, ARRAY_SIZE(slots),
                                                  &err),
                    ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.snapshots, ==, 0);
}

static SGIRenderCommitResult render_invalid_extent(void *opaque,
                                                   const SGIRenderResource *res,
                                                   uint32_t slice,
                                                   const SGIRenderRect *rect,
                                                   const uint32_t *slots,
                                                   size_t stride_slots,
                                                   size_t capacity_slots,
                                                   Error **errp)
{
    error_setg(errp, "provider reported an invalid extent");
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_RESOURCE_ERROR,
        .complete_pixels = 5,
        .extent_known = true,
    };
}

static void test_render_resource_invalid_commit_extent(void)
{
    RenderResourceFixture f = { 0 };
    SGIRenderResource res = render_colour_resource();
    SGIRenderResourceOps ops = render_resource_ops;
    SGIRasterTarget target = {
        .opaque = &f, .resources = &res, .resource_count = 1,
        .resource_ops = &ops,
    };
    SGIRenderRect rect = { 0, 0, 3, 0 };
    uint32_t slots[12] = { 0 };
    SGIRenderCommitResult committed;
    Error *err = NULL;

    ops.commit = render_invalid_extent;
    committed = sgi_render_resource_commit(&target, &res, 0, &rect,
                                           slots, 12, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_HOST_ERROR);
    g_assert_false(committed.extent_known);
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "provider reported"));
    error_free(err);
}

static void test_render_resource_texture_interior(void)
{
    RenderResourceFixture f = { 0 };
    SGIRenderResource res = render_colour_resource();
    SGIRasterTarget target = {
        .opaque = &f, .resources = &res, .resource_count = 1,
        .resource_ops = &render_resource_ops,
    };
    SGIRenderRect rect = { 0, 0, 1, 0 };
    uint32_t slots[12] = { 0 };
    Error *err = NULL;

    res.role = SGI_RENDER_TEXTURE;
    res.width = 2;
    res.border[0] = 1;
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &res, 0, &rect,
                                                  slots, 6, ARRAY_SIZE(slots),
                                                  &err),
                    ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.snapshots, ==, 0);
}

static void test_render_resource_rejects_high_bits(void)
{
    RenderResourceFixture f = {
        .native = { { 4096, 1, 2 } },
        .fail_after = SIZE_MAX,
    };
    SGIRenderResource res = render_colour_resource();
    SGIRasterTarget target = {
        .opaque = &f, .resources = &res, .resource_count = 1,
        .resource_ops = &render_resource_ops,
    };
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint32_t slots[3] = { 0 };
    SGIRenderCommitResult committed;
    Error *err = NULL;

    g_assert_cmpint(sgi_render_resource_snapshot(&target, &res, 0, &rect,
                                                  slots, 3, ARRAY_SIZE(slots),
                                                  &err),
                    ==, SGI_RENDER_HOST_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    g_assert_cmpuint(f.snapshots, ==, 1);
    f.native[0][0] = 5;
    slots[0] = 4096;
    committed = sgi_render_resource_commit(&target, &res, 0, &rect,
                                           slots, 3, ARRAY_SIZE(slots), &err);
    g_assert_cmpint(committed.status, ==, SGI_RENDER_HOST_ERROR);
    g_assert_cmpuint(committed.complete_pixels, ==, 0);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmpuint(f.commits, ==, 0);
    g_assert_cmpuint(f.native[0][0], ==, 5);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-render-resource/precision",
                    test_render_resource_precision);
    g_test_add_func("/sgi-render-resource/invalid-span",
                    test_render_resource_invalid_span);
    g_test_add_func("/sgi-render-resource/partial-commit",
                    test_render_resource_partial_commit);
    g_test_add_func("/sgi-render-resource/covered-commit",
                    test_render_resource_covered_commit);
    g_test_add_func("/sgi-render-resource/index-commit",
                    test_render_resource_index_commit);
    g_test_add_func("/sgi-render-resource/covered-partial",
                    test_render_resource_covered_partial);
    g_test_add_func("/sgi-render-resource/attachment-border",
                    test_render_resource_rejects_attachment_border);
    g_test_add_func("/sgi-render-resource/invalid-extent",
                    test_render_resource_invalid_commit_extent);
    g_test_add_func("/sgi-render-resource/texture-interior",
                    test_render_resource_texture_interior);
    g_test_add_func("/sgi-render-resource/high-bits",
                    test_render_resource_rejects_high_bits);
    return g_test_run();
}
