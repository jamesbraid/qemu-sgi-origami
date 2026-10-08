/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/sgi-odyssey-planes.h"

static bool odyssey_plane_view_valid(const OdysseyPlaneView *view,
                                     Error **errp)
{
    size_t row_bytes;

    if (!view->bytes || !view->width || !view->height ||
        view->format != ODYSSEY_PLANE_SYNTH_RGBA8 ||
        view->role != SGI_RENDER_COLOUR ||
        (view->buffer != SGI_RENDER_FRONT &&
         view->buffer != SGI_RENDER_BACK) ||
        (view->eye != SGI_RENDER_LEFT && view->eye != SGI_RENDER_RIGHT) ||
        !view->backing_id || !view->view_id ||
        !view->backing_generation || !view->view_generation ||
        view->channel_mask & ~0xf || !view->is_written ||
        !view->owns_pixel || !view->mark_written) {
        error_setg(errp, "Unsupported or incomplete synthetic Odyssey plane");
        return false;
    }
#if SIZE_MAX < UINT64_MAX
    if (view->width > SIZE_MAX / 4) {
        error_setg(errp, "Synthetic Odyssey plane row is too wide");
        return false;
    }
#endif
    row_bytes = (size_t)view->width * 4;
    if (view->stride_bytes < row_bytes ||
        view->height - 1 > (SIZE_MAX - row_bytes) / view->stride_bytes ||
        view->capacity_bytes < (size_t)(view->height - 1) *
                               view->stride_bytes + row_bytes) {
        error_setg(errp, "Synthetic Odyssey plane backing is too short");
        return false;
    }
    return true;
}

bool odyssey_planes_refresh(OdysseyPlanes *planes,
                            SGIRenderResource *new_resources, Error **errp)
{
    size_t descriptor_bytes;

    if (!planes || !planes->initialized || !planes->views ||
        !new_resources || !planes->count ||
        planes->count > SIZE_MAX / sizeof(*new_resources)) {
        error_setg(errp, "Missing synthetic Odyssey plane views");
        return false;
    }
    if (planes->active) {
        error_setg(errp, "Synthetic Odyssey plane submission is active");
        return false;
    }
    descriptor_bytes = planes->count * sizeof(*new_resources);
    if (planes->resources) {
        uintptr_t old = (uintptr_t)planes->resources;
        uintptr_t next = (uintptr_t)new_resources;

        if ((old <= next && next - old < descriptor_bytes) ||
            (next < old && old - next < descriptor_bytes)) {
            error_setg(errp, "Synthetic Odyssey descriptors are still "
                             "borrowed");
            return false;
        }
    }
    for (size_t i = 0; i < planes->count; i++) {
        OdysseyPlaneView *view = &planes->views[i];

        if (!odyssey_plane_view_valid(view, errp)) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            OdysseyPlaneView *other = &planes->views[j];
            bool same_position = view->buffer == other->buffer &&
                                 view->eye == other->eye;
            bool same_backing = view->backing_id == other->backing_id;
            uintptr_t a = (uintptr_t)view->bytes;
            uintptr_t b = (uintptr_t)other->bytes;
            bool overlapping = a <= b ? b - a < view->capacity_bytes :
                                        a - b < other->capacity_bytes;

            if (view->view_id == other->view_id ||
                view->target == other->target ||
                same_position != same_backing ||
                (same_backing &&
                 (view->backing_generation != other->backing_generation ||
                  view->bytes != other->bytes ||
                  view->capacity_bytes != other->capacity_bytes ||
                  view->stride_bytes != other->stride_bytes ||
                  view->width != other->width ||
                  view->height != other->height ||
                  view->format != other->format ||
                  view->bottom_origin != other->bottom_origin ||
                  view->storage_top_origin != other->storage_top_origin)) ||
                (!same_backing && overlapping)) {
                error_setg(errp, "Inconsistent synthetic Odyssey plane alias");
                return false;
            }
        }
    }
    planes->resources = new_resources;
    for (size_t i = 0; i < planes->count; i++) {
        OdysseyPlaneView *view = &planes->views[i];

        view->submitted_bytes = view->bytes;
        view->submitted_storage_top_origin = view->storage_top_origin;
        view->submitted_backing_generation = view->backing_generation;
        view->submitted_view_generation = view->view_generation;
        view->expected_backing_generation = view->backing_generation;
        view->expected_view_generation = view->view_generation;
        new_resources[i] = (SGIRenderResource) {
            .backing_id = view->backing_id,
            .backing_generation = view->backing_generation,
            .view_id = view->view_id,
            .view_generation = view->view_generation,
            .role = view->role,
            .target = view->target,
            .buffer = view->buffer,
            .eye = view->eye,
            .width = view->width,
            .height = view->height,
            .depth = 1,
            .bottom_origin = view->bottom_origin,
            .component_count = 4,
            .component = {
                { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
                { SGI_RENDER_A, SGI_RENDER_UNORM, 8 },
            },
        };
    }
    planes->active = true;
    return true;
}

bool odyssey_planes_init(OdysseyPlanes *planes, OdysseyPlaneView *views,
                         SGIRenderResource *resources, size_t count,
                         Error **errp)
{
    if (!planes) {
        error_setg(errp, "Missing synthetic Odyssey plane provider");
        return false;
    }
    if (planes->initialized) {
        error_setg(errp, "Synthetic Odyssey plane provider already "
                         "initialized");
        return false;
    }
    *planes = (OdysseyPlanes) {
        .views = views,
        .count = count,
        .initialized = true,
    };
    if (!odyssey_planes_refresh(planes, resources, errp)) {
        *planes = (OdysseyPlanes) { 0 };
        return false;
    }
    return true;
}

void odyssey_planes_end_submission(OdysseyPlanes *planes)
{
    if (planes) {
        planes->active = false;
    }
}

bool odyssey_planes_swap_bindings(OdysseyPlanes *planes, Error **errp)
{
    static const SGIRenderChannel channel[4] = {
        SGI_RENDER_R, SGI_RENDER_G, SGI_RENDER_B, SGI_RENDER_A,
    };
    size_t position[2][2] = {
        { SIZE_MAX, SIZE_MAX }, { SIZE_MAX, SIZE_MAX },
    };

    if (!planes || !planes->initialized || planes->active ||
        planes->count != 4) {
        error_setg(errp, "Synthetic Odyssey swap needs four idle views");
        return false;
    }
    for (size_t i = 0; i < 4; i++) {
        OdysseyPlaneView *view = &planes->views[i];
        const SGIRenderResource *published = &planes->resources[i];
        unsigned buffer, eye;

        if (!odyssey_plane_view_valid(view, errp)) {
            return false;
        }
        if (view->bytes != view->submitted_bytes ||
            view->storage_top_origin !=
                view->submitted_storage_top_origin ||
            view->backing_generation != view->expected_backing_generation ||
            view->view_generation != view->expected_view_generation ||
            view->backing_id != published->backing_id ||
            view->view_id != published->view_id ||
            view->role != published->role ||
            view->target != published->target ||
            view->buffer != published->buffer ||
            view->eye != published->eye ||
            view->width != published->width ||
            view->height != published->height ||
            view->bottom_origin != published->bottom_origin ||
            published->backing_generation !=
                view->submitted_backing_generation ||
            published->view_generation != view->submitted_view_generation ||
            published->depth != 1 || published->component_count != 4 ||
            published->auxiliary_index || published->texture_level ||
            published->border[0] || published->border[1] ||
            published->border[2] ||
            view->view_generation == UINT64_MAX) {
            error_setg(errp, "Stale synthetic Odyssey swap view");
            return false;
        }
        for (unsigned c = 0; c < 4; c++) {
            if (published->component[c].channel != channel[c] ||
                published->component[c].numeric != SGI_RENDER_UNORM ||
                published->component[c].storage_bits != 8) {
                error_setg(errp, "Changed synthetic Odyssey swap format");
                return false;
            }
        }
        for (size_t j = 0; j < i; j++) {
            if (view->target == planes->views[j].target) {
                error_setg(errp, "Duplicate synthetic Odyssey swap target");
                return false;
            }
        }
        buffer = view->buffer == SGI_RENDER_FRONT ? 0 : 1;
        eye = view->eye == SGI_RENDER_LEFT ? 0 : 1;
        if (position[buffer][eye] != SIZE_MAX) {
            error_setg(errp, "Duplicate synthetic Odyssey swap view");
            return false;
        }
        position[buffer][eye] = i;
    }
    for (unsigned eye = 0; eye < 2; eye++) {
        if (position[0][eye] == SIZE_MAX || position[1][eye] == SIZE_MAX) {
            error_setg(errp, "Missing synthetic Odyssey swap view");
            return false;
        }
    }
    for (unsigned eye = 0; eye < 2; eye++) {
        OdysseyPlaneView *front, *back;
        SGIColourTarget target;

        front = &planes->views[position[0][eye]];
        back = &planes->views[position[1][eye]];
        target = front->target;
        front->target = back->target;
        back->target = target;
        front->buffer = SGI_RENDER_BACK;
        back->buffer = SGI_RENDER_FRONT;
        front->view_generation++;
        back->view_generation++;
        front->dirty = true;
        back->dirty = true;
    }
    return true;
}

static OdysseyPlaneView *odyssey_plane_lookup(OdysseyPlanes *planes,
                                              const SGIRenderResource *resource,
                                              uint32_t slice,
                                              const SGIRenderRect *rect,
                                              Error **errp)
{
    static const SGIRenderChannel channel[4] = {
        SGI_RENDER_R, SGI_RENDER_G, SGI_RENDER_B, SGI_RENDER_A,
    };

    if (!planes || !planes->active || !planes->views ||
        !planes->resources || !rect) {
        error_setg(errp, "Missing synthetic Odyssey plane resource");
        return NULL;
    }
    for (size_t i = 0; i < planes->count; i++) {
        OdysseyPlaneView *view = &planes->views[i];

        if (&planes->resources[i] != resource) {
            continue;
        }
        if (!odyssey_plane_view_valid(view, errp)) {
            return NULL;
        }
        if (slice || rect->x0 > rect->x1 || rect->y0 > rect->y1 ||
            rect->x1 >= view->width || rect->y1 >= view->height ||
            resource->backing_id != view->backing_id ||
            resource->backing_generation !=
                view->submitted_backing_generation ||
            resource->view_id != view->view_id ||
            resource->view_generation != view->submitted_view_generation ||
            view->backing_generation != view->expected_backing_generation ||
            view->view_generation != view->expected_view_generation ||
            view->bytes != view->submitted_bytes ||
            view->storage_top_origin !=
                view->submitted_storage_top_origin ||
            resource->role != view->role || resource->target != view->target ||
            resource->buffer != view->buffer || resource->eye != view->eye ||
            resource->width != view->width ||
            resource->height != view->height ||
            resource->depth != 1 ||
            resource->auxiliary_index || resource->texture_level ||
            resource->border[0] || resource->border[1] ||
            resource->border[2] || resource->component_count != 4 ||
            resource->bottom_origin != view->bottom_origin) {
            error_setg(errp, "Stale or invalid synthetic Odyssey plane view");
            return NULL;
        }
        for (unsigned c = 0; c < 4; c++) {
            if (resource->component[c].channel != channel[c] ||
                resource->component[c].numeric != SGI_RENDER_UNORM ||
                resource->component[c].storage_bits != 8) {
                error_setg(errp, "Changed synthetic Odyssey plane format");
                return NULL;
            }
        }
        return view;
    }
    error_setg(errp, "Unknown synthetic Odyssey plane resource");
    return NULL;
}

static void odyssey_plane_invalidate_aliases(OdysseyPlanes *planes,
                                              uint64_t backing_id)
{
    for (size_t i = 0; i < planes->count; i++) {
        OdysseyPlaneView *view = &planes->views[i];

        if (view->backing_id == backing_id) {
            view->dirty = true;
            view->backing_generation++;
            view->view_generation++;
            view->expected_backing_generation++;
            view->expected_view_generation++;
        }
    }
}

static uint32_t odyssey_plane_storage_row(const OdysseyPlaneView *view,
                                          uint32_t y)
{
    return view->storage_top_origin && view->bottom_origin ?
        view->height - 1 - y : y;
}

static SGIRenderResult odyssey_plane_snapshot(void *opaque,
                                              const SGIRenderResource *resource,
                                               uint32_t slice,
                                               const SGIRenderRect *rect,
                                               uint32_t *slots,
                                               size_t stride_slots,
                                               size_t capacity_slots,
                                               Error **errp)
{
    OdysseyPlanes *planes = opaque;
    OdysseyPlaneView *view = odyssey_plane_lookup(planes, resource, slice,
                                                  rect, errp);

    if (!view) {
        return SGI_RENDER_RESOURCE_ERROR;
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        uint32_t row = odyssey_plane_storage_row(view, y);

        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            if (!view->is_written(view->opaque, x, row)) {
                error_setg(errp, "Synthetic Odyssey plane pixel unavailable");
                return SGI_RENDER_RESOURCE_ERROR;
            }
        }
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        uint32_t row = odyssey_plane_storage_row(view, y);

        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t src = (size_t)row * view->stride_bytes + (size_t)x * 4;
            size_t dst = (size_t)(y - rect->y0) * stride_slots +
                         (size_t)(x - rect->x0) * 4;

            for (unsigned c = 0; c < 4; c++) {
                slots[dst + c] = view->bytes[src + c];
            }
        }
    }
    return SGI_RENDER_OK;
}

static SGIRenderCommitResult odyssey_plane_commit_pixels(
    void *opaque, const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, const SGIRenderWriteCoverage *writes, Error **errp)
{
    OdysseyPlanes *planes = opaque;
    OdysseyPlaneView *view = odyssey_plane_lookup(planes, resource, slice,
                                                  rect, errp);
    SGIRenderCommitResult result = {
        .status = SGI_RENDER_RESOURCE_ERROR,
        .extent_known = true,
    };
    bool wrote = false;

    if (!view) {
        return result;
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        uint32_t row = odyssey_plane_storage_row(view, y);

        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t src = (size_t)(y - rect->y0) * stride_slots +
                         (size_t)(x - rect->x0) * 4;
            size_t dst = (size_t)row * view->stride_bytes + (size_t)x * 4;

            if (writes &&
                !writes->pixel[(size_t)(y - rect->y0) *
                               writes->stride_bytes + x - rect->x0]) {
                result.complete_pixels++;
                continue;
            }
            if (!view->owns_pixel(view->opaque, x, row) ||
                (view->channel_mask != 0xf &&
                 !view->is_written(view->opaque, x, row))) {
                error_setg(errp, "Synthetic Odyssey plane write unavailable");
                if (wrote) {
                    odyssey_plane_invalidate_aliases(planes, view->backing_id);
                }
                planes->active = false;
                return result;
            }
            for (unsigned c = 0; c < 4; c++) {
                if (view->channel_mask & (1u << c)) {
                    view->bytes[dst + c] = slots[src + c];
                    wrote = true;
                }
            }
            if (view->channel_mask) {
                view->mark_written(view->opaque, x, row);
            }
            result.complete_pixels++;
        }
    }
    if (wrote) {
        odyssey_plane_invalidate_aliases(planes, view->backing_id);
    }
    result.status = SGI_RENDER_OK;
    return result;
}

static SGIRenderCommitResult odyssey_plane_commit(
    void *opaque, const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp)
{
    return odyssey_plane_commit_pixels(opaque, resource, slice, rect, slots,
                                       stride_slots, capacity_slots, NULL,
                                       errp);
}

static SGIRenderCommitResult odyssey_plane_commit_covered(
    void *opaque, const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, const SGIRenderWriteCoverage *writes, Error **errp)
{
    return odyssey_plane_commit_pixels(opaque, resource, slice, rect, slots,
                                       stride_slots, capacity_slots, writes,
                                       errp);
}

const SGIRenderResourceOps *odyssey_planes_resource_ops(void)
{
    static const SGIRenderResourceOps ops = {
        .snapshot = odyssey_plane_snapshot,
        .commit = odyssey_plane_commit,
        .commit_covered = odyssey_plane_commit_covered,
    };

    return &ops;
}
