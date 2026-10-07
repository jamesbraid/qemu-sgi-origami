/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/sgi-kona-mesa-provider.h"

typedef struct Fixture {
    SGIBoardMemory memory;
    SGIKonaMesaPlanes planes;
    SGIKonaMesaProvider *provider;
    uint32_t dirty_plane, dirty_y0, dirty_y1;
    unsigned dirty_planes, dirty_textures;
} Fixture;

static void dirty_plane(void *opaque, uint32_t id, uint32_t y0, uint32_t y1)
{
    Fixture *f = opaque;

    f->dirty_plane = id;
    f->dirty_y0 = y0;
    f->dirty_y1 = y1;
    f->dirty_planes++;
}

static void dirty_texture(void *opaque, uint32_t handle, uint32_t level,
                          uint32_t image_id)
{
    Fixture *f = opaque;

    g_assert_cmpuint(handle, ==, 41);
    g_assert_true(level == 2 || level == 3);
    g_assert_cmpuint(image_id, ==, 5);
    f->dirty_textures++;
}

static const SGIKonaMesaDirtyOps dirty_ops = {
    .plane = dirty_plane,
    .texture = dirty_texture,
};

static uint32_t add_plane(Fixture *f, uint64_t *base, SGIPlaneFormat format,
                          SGIPacking packing, unsigned bpp)
{
    SGIPlane plane = {
        .base = *base,
        .len = 4 * 2 * bpp,
        .width = 4,
        .height = 2,
        .stride = 4 * bpp,
        .format = format,
        .packing = packing,
        .bottom_origin = true,
        .evidence = "unit fixture",
    };
    uint32_t id = sgi_board_memory_add_plane(&f->memory, &plane,
                                              &error_abort);

    g_assert_cmpuint(id, !=, SGI_PLANE_NONE);
    *base += plane.len;
    return id;
}

static void fixture_init(Fixture *f, bool pages)
{
    uint64_t base = 0;

    memset(f, 0, sizeof(*f));
    if (pages) {
        sgi_board_memory_init_pages(&f->memory, 16, 8);
    } else {
        sgi_board_memory_init(&f->memory, 128);
    }
    f->planes.front = add_plane(f, &base, SGI_PLANE_RGB24,
                                SGI_PACK_LE_RED_HIGH, 4);
    f->planes.back = add_plane(f, &base, SGI_PLANE_RGB24,
                               SGI_PACK_LE_RED_HIGH, 4);
    f->planes.front_tag = add_plane(f, &base, SGI_PLANE_TAG,
                                    SGI_PACK_LE_RED_LOW, 1);
    f->planes.back_tag = add_plane(f, &base, SGI_PLANE_TAG,
                                   SGI_PACK_LE_RED_LOW, 1);
    f->planes.depth = add_plane(f, &base, SGI_PLANE_Z24,
                                SGI_PACK_LE_RED_LOW, 4);
    f->planes.stencil = add_plane(f, &base, SGI_PLANE_S8,
                                  SGI_PACK_LE_RED_LOW, 1);
    f->provider = sgi_kona_mesa_provider_new(&f->memory, &f->planes,
                                              &dirty_ops, f, &error_abort);
    g_assert_nonnull(f->provider);
}

static void fixture_clear(Fixture *f)
{
    sgi_kona_mesa_provider_free(f->provider);
    sgi_board_memory_cleanup(&f->memory);
}

static SGIRasterTarget fixture_target(Fixture *f)
{
    SGIRasterTarget target = { .opaque = f->provider };

    target.resources = sgi_kona_mesa_resources(f->provider,
                                                &target.resource_count);
    target.resource_ops = sgi_kona_mesa_resource_ops();
    return target;
}

static void test_aliases_and_precision(void)
{
    Fixture f;
    SGIRasterTarget target;
    const SGIRenderResource *r;
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint32_t rgb[3] = { 0 }, index = 0, tag = 0, z = 0, stencil = 0;
    uint32_t new_rgb[3] = { 0x21, 0x54, 0x87 };
    uint32_t new_index = 0xface;
    uint64_t before;
    SGIRenderCommitResult result;
    Error *err = NULL;

    fixture_init(&f, false);
    target = fixture_target(&f);
    r = target.resources;
    g_assert_cmpuint(target.resource_count, ==, 8);
    g_assert_cmpuint(r[0].backing_id, ==, r[1].backing_id);
    g_assert_cmpuint(r[0].view_id, !=, r[1].view_id);
    g_assert_cmpuint(r[0].backing_id, !=, r[2].backing_id);
    g_assert_cmpuint(r[4].backing_id, !=, r[5].backing_id);
    g_assert_cmpuint(r[6].backing_id, !=, r[7].backing_id);
    g_assert_cmpuint(r[0].component[0].storage_bits, ==, 8);
    g_assert_cmpuint(r[1].component[0].storage_bits, ==, 16);
    g_assert_cmpuint(r[6].component[0].storage_bits, ==, 24);

    sgi_plane_store(&f.memory, sgi_board_memory_plane(&f.memory,
                    f.planes.front), 0, 0, 0x0012ab34);
    sgi_plane_store(&f.memory, sgi_board_memory_plane(&f.memory,
                    f.planes.front_tag), 0, 0, 3);
    sgi_plane_store(&f.memory, sgi_board_memory_plane(&f.memory,
                    f.planes.depth), 0, 0, 0xabcdef);
    sgi_plane_store(&f.memory, sgi_board_memory_plane(&f.memory,
                    f.planes.stencil), 0, 0, 0xe1);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &r[0], 0, &rect,
                    rgb, 3, 3, &err), ==, SGI_RENDER_OK);
    g_assert_cmpmem(rgb, sizeof(rgb),
                    ((uint32_t[3]) { 0x12, 0xab, 0x34 }), sizeof(rgb));
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &r[1], 0, &rect,
                    &index, 1, 1, &err), ==, SGI_RENDER_OK);
    g_assert_cmphex(index, ==, 0xab34);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &r[4], 0, &rect,
                    &tag, 1, 1, &err), ==, SGI_RENDER_OK);
    g_assert_cmphex(tag, ==, 3);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &r[6], 0, &rect,
                    &z, 1, 1, &err), ==, SGI_RENDER_OK);
    g_assert_cmphex(z, ==, 0xabcdef);
    g_assert_cmpint(sgi_render_resource_snapshot(&target, &r[7], 0, &rect,
                    &stencil, 1, 1, &err), ==, SGI_RENDER_OK);
    g_assert_cmphex(stencil, ==, 0xe1);
    g_assert_null(err);

    before = r[0].backing_generation;
    result = sgi_render_resource_commit(&target, &r[1], 0, &rect,
                                        &new_index, 1, 1, &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_cmpuint(r[0].backing_generation, >, before);
    g_assert_cmpuint(r[0].backing_generation, ==,
                     r[1].backing_generation);
    g_assert_cmphex(sgi_plane_load(&f.memory,
                    sgi_board_memory_plane(&f.memory, f.planes.front),
                    0, 0), ==, 0xface);
    g_assert_cmphex(sgi_plane_load(&f.memory,
                    sgi_board_memory_plane(&f.memory, f.planes.front_tag),
                    0, 0), ==, 3);
    result = sgi_render_resource_commit(&target, &r[0], 0, &rect,
                                        new_rgb, 3, 3, &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_cmphex(sgi_plane_load(&f.memory,
                    sgi_board_memory_plane(&f.memory, f.planes.front),
                    0, 0), ==, 0x215487);
    g_assert_null(err);
    fixture_clear(&f);
}

static void test_partial_commit(void)
{
    Fixture f;
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 3, 0 };
    uint32_t slots[12] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
    };
    uint8_t first_page[8] = { 0 };
    SGIRenderCommitResult result;
    Error *err = NULL;

    fixture_init(&f, true);
    sgi_board_memory_set_page(&f.memory, 0, 0, first_page);
    target = fixture_target(&f);
    result = sgi_render_resource_commit(&target, &target.resources[0], 0,
                                        &rect, slots, 12, 12, &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(result.complete_pixels, ==, 2);
    g_assert_false(result.partial_pixel);
    g_assert_true(result.extent_known);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmphex(sgi_plane_load(&f.memory,
                    sgi_board_memory_plane(&f.memory, f.planes.front),
                    0, 0), ==, 0x010203);
    g_assert_cmphex(sgi_plane_load(&f.memory,
                    sgi_board_memory_plane(&f.memory, f.planes.front),
                    1, 0), ==, 0x040506);
    g_assert_cmpmem(first_page, 8,
                    ((uint8_t[8]) { 3, 2, 1, 0, 6, 5, 4, 0 }), 8);
    g_assert_cmpuint(target.resources[0].backing_generation, >, 1);
    g_assert_cmpuint(f.dirty_planes, ==, 1);
    g_assert_cmpuint(f.dirty_plane, ==, f.planes.front);
    g_assert_cmpuint(f.dirty_y0, ==, 0);
    g_assert_cmpuint(f.dirty_y1, ==, 0);
    fixture_clear(&f);
}

static void test_later_attachment_failure_keeps_colour(void)
{
    Fixture f;
    SGIRasterTarget target;
    SGIRenderRect rect = { 0, 0, 0, 0 };
    uint8_t colour_pages[4][8] = { 0 };
    uint32_t rgb[3] = { 0x31, 0x72, 0xb4 };
    uint32_t tag = 0;
    SGIRenderCommitResult result;
    Error *err = NULL;

    fixture_init(&f, true);
    for (unsigned i = 0; i < 4; i++) {
        sgi_board_memory_set_page(&f.memory, i, i * 8,
                                  colour_pages[i]);
    }
    target = fixture_target(&f);
    result = sgi_render_resource_commit(&target, &target.resources[0], 0,
                                        &rect, rgb, 3, 3, &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_null(err);
    result = sgi_render_resource_commit(&target, &target.resources[4], 0,
                                        &rect, &tag, 1, 1, &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_cmpuint(result.complete_pixels, ==, 0);
    g_assert_true(result.extent_known);
    g_assert_nonnull(err);
    error_free(err);
    g_assert_cmphex(sgi_plane_load(&f.memory,
                    sgi_board_memory_plane(&f.memory, f.planes.front),
                    0, 0), ==, 0x3172b4);
    fixture_clear(&f);
}

static void test_texture_view_and_invalidation(void)
{
    Fixture f;
    SGIRasterTarget target;
    uint32_t pixels[2] = { 0x12345678, 0x90abcdef };
    uint8_t coverage = 1;
    uint64_t covered = 1;
    SGIKonaMesaTextureView view = {
        .handle = 41, .level = 2, .image_id = 5,
        .width = 2, .height = 1, .depth = 1,
        .native_fragment = true, .bottom_origin = true,
        .pixels = pixels, .coverage = &coverage, .covered = &covered,
    };
    SGIRenderRect rect = { 0, 0, 1, 0 };
    uint32_t slots[8] = { 0 };
    uint64_t before;
    uint64_t view_id;
    uint64_t backing_id;
    uint64_t view_generation;
    Error *err = NULL;
    SGIRenderCommitResult result;

    fixture_init(&f, false);
    g_assert_true(sgi_kona_mesa_bind_texture(f.provider, &view, &err));
    target = fixture_target(&f);
    g_assert_cmpuint(target.resource_count, ==, 9);
    g_assert_cmpuint(target.resources[8].texture_level, ==, 2);
    g_assert_cmpint(sgi_render_resource_snapshot(&target,
                    &target.resources[8], 0, &rect, slots, 8, 8, &err),
                    ==, SGI_RENDER_RESOURCE_ERROR);
    g_assert_nonnull(err);
    error_free(err);
    err = NULL;
    result = sgi_render_resource_commit(&target, &target.resources[8], 0,
                    &(SGIRenderRect) { 1, 0, 1, 0 },
                    ((uint32_t[4]) { 0x90, 0xab, 0xcd, 0xef }), 4, 4,
                    &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_cmphex(coverage, ==, 3);
    g_assert_cmpuint(covered, ==, 2);
    g_assert_cmpint(sgi_render_resource_snapshot(&target,
                    &target.resources[8], 0, &rect, slots, 8, 8, &err),
                    ==, SGI_RENDER_OK);
    g_assert_cmpmem(slots, sizeof(slots),
                    ((uint32_t[8]) { 0x12, 0x34, 0x56, 0x78,
                                     0x90, 0xab, 0xcd, 0xef }),
                    sizeof(slots));
    slots[4] = 0xff;
    before = target.resources[8].backing_generation;
    result = sgi_render_resource_commit(&target, &target.resources[8],
                                        0, &rect, slots, 8, 8, &err);
    g_assert_cmpint(result.status, ==, SGI_RENDER_OK);
    g_assert_cmphex(pixels[1], ==, 0xffabcdef);
    g_assert_cmpuint(target.resources[8].backing_generation, >, before);
    g_assert_cmpuint(f.dirty_textures, ==, 2);
    g_assert_null(err);
    sgi_kona_mesa_touch_texture(f.provider, 41, 2);
    target = fixture_target(&f);
    g_assert_cmpuint(target.resource_count, ==, 8);
    g_assert_true(sgi_kona_mesa_bind_texture(f.provider, &view, &err));
    target = fixture_target(&f);
    g_assert_cmpuint(target.resources[8].backing_generation, >, before);
    view_id = target.resources[8].view_id;
    backing_id = target.resources[8].backing_id;
    view.level = 3;
    g_assert_true(sgi_kona_mesa_bind_texture(f.provider, &view, &err));
    target = fixture_target(&f);
    g_assert_cmpuint(target.resources[8].backing_id, ==, backing_id);
    g_assert_cmpuint(target.resources[8].view_id, !=, view_id);
    view_generation = target.resources[8].view_generation;
    g_assert_true(sgi_kona_mesa_bind_texture(f.provider, &view, &err));
    target = fixture_target(&f);
    g_assert_cmpuint(target.resources[8].view_generation, >,
                     view_generation);
    before = target.resources[8].backing_generation;
    sgi_kona_mesa_touch_texture(f.provider, 41, 2);
    target = fixture_target(&f);
    g_assert_cmpuint(target.resource_count, ==, 9);
    g_assert_cmpuint(target.resources[8].backing_generation, >, before);
    g_assert_null(err);
    fixture_clear(&f);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi-kona-mesa/aliases-precision",
                    test_aliases_and_precision);
    g_test_add_func("/sgi-kona-mesa/partial-commit", test_partial_commit);
    g_test_add_func("/sgi-kona-mesa/later-attachment-failure",
                    test_later_attachment_failure_keeps_colour);
    g_test_add_func("/sgi-kona-mesa/texture-view",
                    test_texture_view_and_invalidation);
    return g_test_run();
}
