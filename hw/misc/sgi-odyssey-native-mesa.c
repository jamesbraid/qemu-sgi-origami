/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/sgi-odyssey-native-mesa.h"

static bool native_plane_valid(const SGIBoardMemory *memory,
                               const SGIPlane *plane)
{
    uint64_t last;

    if (!memory || !plane || !memory->bytes ||
        plane->id >= memory->nplanes ||
        &memory->planes[plane->id] != plane ||
        plane->format != SGI_PLANE_RGBA8 ||
        plane->packing != SGI_PACK_BE_RED_HIGH || plane->bottom_origin ||
        !plane->width || !plane->height ||
        plane->width > INT_MAX || plane->height > INT_MAX ||
        (uint64_t)plane->width * 4 > plane->stride) {
        return false;
    }
    last = (uint64_t)(plane->height - 1) * plane->stride +
           (uint64_t)plane->width * 4;
    return last <= plane->len && plane->base <= memory->size &&
           plane->len <= memory->size - plane->base &&
           plane->base <= memory->bytes_len &&
           plane->len <= memory->bytes_len - plane->base;
}

bool odyssey_native_mesa_bind(OdysseyNativeMesa *native,
                              SGIBoardMemory *buffer0, const SGIPlane *plane0,
                              SGIBoardMemory *buffer1, const SGIPlane *plane1,
                              void (*invalidate_scanout_all)(void *opaque),
                              void *invalidate_opaque, Error **errp)
{
    SGIBoardMemory *memory[2] = { buffer0, buffer1 };
    const SGIPlane *plane[2] = { plane0, plane1 };
    uintptr_t first, second;

    if (!native || !native_plane_valid(buffer0, plane0) ||
        !native_plane_valid(buffer1, plane1) ||
        plane0->width != plane1->width ||
        plane0->height != plane1->height ||
        buffer0 == buffer1 ||
        !invalidate_scanout_all) {
        error_setg(errp, "Unsupported Odyssey native Mesa backings");
        return false;
    }
    first = (uintptr_t)buffer0->bytes;
    second = (uintptr_t)buffer1->bytes;
    if (first <= second ? second - first < buffer0->bytes_len :
                          first - second < buffer1->bytes_len) {
        error_setg(errp, "Aliased Odyssey native Mesa backings");
        return false;
    }
    *native = (OdysseyNativeMesa) { 0 };
    native->invalidate_scanout_all = invalidate_scanout_all;
    native->invalidate_opaque = invalidate_opaque;
    for (unsigned b = 0; b < 2; b++) {
        native->memory[b] = memory[b];
        native->plane[b] = plane[b];
        native->resources[b] = (SGIRenderResource) {
            .backing_id = b + 1,
            .backing_generation = 1,
            .view_id = b + 1,
            .view_generation = 1,
            .role = SGI_RENDER_COLOUR,
            .target = b,
            .buffer = b ? SGI_RENDER_BACK : SGI_RENDER_FRONT,
            .eye = SGI_RENDER_MONO,
            .width = plane[b]->width,
            .height = plane[b]->height,
            .depth = 1,
            .bottom_origin = false,
            .component_count = 4,
            .component = {
                { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_A, SGI_RENDER_UNORM, 8 },
            },
        };
    }
    return true;
}

static int native_resource_index(OdysseyNativeMesa *native,
                                 const SGIRenderResource *resource,
                                 uint32_t slice, const SGIRenderRect *rect)
{
    if (!native || !resource || !rect || slice ||
        rect->x0 > rect->x1 || rect->y0 > rect->y1) {
        return -1;
    }
    for (unsigned b = 0; b < 2; b++) {
        const SGIRenderResource *published = &native->resources[b];

        if (resource != published) {
            continue;
        }
        if (resource->backing_generation != 1 ||
            resource->view_generation != 1 ||
            resource->backing_id != b + 1 || resource->view_id != b + 1 ||
            resource->target != b || resource->role != SGI_RENDER_COLOUR ||
            resource->buffer != (b ? SGI_RENDER_BACK : SGI_RENDER_FRONT) ||
            resource->eye != SGI_RENDER_MONO || resource->bottom_origin ||
            resource->depth != 1 || resource->component_count != 4 ||
            resource->width != native->plane[b]->width ||
            resource->height != native->plane[b]->height ||
            rect->x1 >= resource->width || rect->y1 >= resource->height ||
            !native_plane_valid(native->memory[b], native->plane[b])) {
            return -1;
        }
        for (unsigned c = 0; c < 4; c++) {
            if (resource->component[c].channel != SGI_RENDER_R + c ||
                resource->component[c].numeric != SGI_RENDER_UNORM ||
                resource->component[c].storage_bits != 8) {
                return -1;
            }
        }
        return b;
    }
    return -1;
}

static SGIRenderResult native_snapshot(void *opaque,
                                      const SGIRenderResource *resource,
                                      uint32_t slice,
                                      const SGIRenderRect *rect,
                                      uint32_t *slots, size_t stride_slots,
                                      size_t capacity_slots, Error **errp)
{
    OdysseyNativeMesa *native = opaque;
    int b = native_resource_index(native, resource, slice, rect);

    if (b < 0) {
        error_setg(errp, "Invalid Odyssey native Mesa snapshot resource");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            const SGIPlane *plane = native->plane[b];
            uint32_t word = sgi_plane_load(native->memory[b], plane, x, y);
            size_t dst;

            dst = (size_t)(y - rect->y0) * stride_slots +
                  (size_t)(x - rect->x0) * 4;
            for (unsigned c = 0; c < 4; c++) {
                slots[dst + c] = (word >> (24 - 8 * c)) & 0xff;
            }
        }
    }
    return SGI_RENDER_OK;
}

static OdysseyNativeStoreResult native_store(OdysseyNativeMesa *native,
                                             unsigned b, uint32_t x,
                                             uint32_t y, uint32_t word)
{
    SGIBoardMemory *memory = native->memory[b];
    const SGIPlane *plane = native->plane[b];

    if (native->store) {
        return native->store(native->store_opaque, memory, plane, x, y,
                             word);
    }
    sgi_plane_store(memory, plane, x, y, word);
    return ODYSSEY_NATIVE_STORE_OK;
}

static void native_invalidate(OdysseyNativeMesa *native)
{
    native->invalidate_scanout_all(native->invalidate_opaque);
}

static SGIRenderCommitResult native_commit_covered(
    void *opaque, const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, const SGIRenderWriteCoverage *writes, Error **errp)
{
    OdysseyNativeMesa *native = opaque;
    SGIRenderCommitResult result = {
        .status = SGI_RENDER_RESOURCE_ERROR,
        .extent_known = true,
    };
    int b = native_resource_index(native, resource, slice, rect);
    bool changed = false;

    if (b < 0 || !writes) {
        error_setg(errp, "Invalid Odyssey native Mesa commit resource");
        return result;
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t at = (size_t)(y - rect->y0) * stride_slots +
                        (size_t)(x - rect->x0) * 4;
            size_t covered = (size_t)(y - rect->y0) *
                             writes->stride_bytes + (x - rect->x0);
            uint32_t word;
            OdysseyNativeStoreResult stored;

            if (!writes->pixel[covered] ||
                !sgi_clip_contains(&native->clip, x, y)) {
                result.complete_pixels++;
                continue;
            }
            word = (slots[at] << 24) | (slots[at + 1] << 16) |
                   (slots[at + 2] << 8) | slots[at + 3];
            stored = native_store(native, b, x, y, word);
            if (stored != ODYSSEY_NATIVE_STORE_OK) {
                bool unknown = stored == ODYSSEY_NATIVE_STORE_UNKNOWN ||
                               stored > ODYSSEY_NATIVE_STORE_UNKNOWN;

                result.partial_pixel = stored == ODYSSEY_NATIVE_STORE_PARTIAL;
                result.extent_known = !unknown;
                if (changed || result.partial_pixel || unknown) {
                    native_invalidate(native);
                }
                error_setg(errp, "Odyssey native Mesa colour store failed");
                return result;
            }
            changed = true;
            result.complete_pixels++;
        }
    }
    if (changed) {
        native_invalidate(native);
    }
    result.status = SGI_RENDER_OK;
    return result;
}

SGIRasterTarget odyssey_native_mesa_target(OdysseyNativeMesa *native)
{
    static const SGIRenderResourceOps ops = {
        .snapshot = native_snapshot,
        .commit_covered = native_commit_covered,
    };

    return (SGIRasterTarget) {
        .opaque = native,
        .resources = native->resources,
        .resource_count = ARRAY_SIZE(native->resources),
        .resource_ops = &ops,
    };
}

SGIRenderResult odyssey_native_mesa_draw(OdysseyNativeMesa *native,
                                         SGIRenderer *renderer,
                                         const SGIRasterRules *rules,
                                         const SGIGLDraw *draw,
                                         unsigned selected_buffers,
                                         SGIRenderEye eye, bool compare_active,
                                         Error **errp)
{
    SGIRasterTarget target;

    if (!native || selected_buffers != 1 || eye != SGI_RENDER_MONO ||
        compare_active) {
        error_setg(errp, "Unsupported Odyssey native Mesa window state");
        return SGI_RENDER_UNSUPPORTED;
    }
    if (!draw || !draw->state) {
        error_setg(errp, "Missing Odyssey native Mesa draw state");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    native->clip = draw->state->drawable.clip;
    target = odyssey_native_mesa_target(native);
    return sgi_render_draw_checked(renderer, rules, &target, draw, errp);
}
