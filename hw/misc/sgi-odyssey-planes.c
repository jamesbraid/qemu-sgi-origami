/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/sgi-odyssey-planes.h"

static bool odyssey_plane_rgba_format(OdysseyPlaneFormat format,
                                      uint8_t bits[4], size_t *scalar_bytes)
{
    switch (format) {
    case ODYSSEY_PLANE_SYNTH_RGBA4:
        bits[0] = bits[1] = bits[2] = bits[3] = 4;
        *scalar_bytes = 1;
        return true;
    case ODYSSEY_PLANE_SYNTH_RGB5_A1:
        bits[0] = bits[1] = bits[2] = 5;
        bits[3] = 1;
        *scalar_bytes = 1;
        return true;
    case ODYSSEY_PLANE_SYNTH_RGBA8:
        bits[0] = bits[1] = bits[2] = bits[3] = 8;
        *scalar_bytes = 1;
        return true;
    case ODYSSEY_PLANE_SYNTH_RGB10_A2:
        bits[0] = bits[1] = bits[2] = 10;
        bits[3] = 2;
        *scalar_bytes = 2;
        return true;
    case ODYSSEY_PLANE_SYNTH_RGBA12_A12:
        bits[0] = bits[1] = bits[2] = bits[3] = 12;
        *scalar_bytes = 2;
        return true;
    default:
        return false;
    }
}

static bool odyssey_plane_raw_format(OdysseyPlaneFormat format,
                                     uint8_t bits[4], size_t *scalar_bytes,
                                     size_t *components)
{
    if (odyssey_plane_rgba_format(format, bits, scalar_bytes)) {
        *components = 4;
        return true;
    }
    switch (format) {
    case ODYSSEY_PLANE_SYNTH_CI8:
        bits[0] = 8;
        *scalar_bytes = 1;
        *components = 1;
        return true;
    case ODYSSEY_PLANE_SYNTH_CI12:
        bits[0] = 12;
        *scalar_bytes = 2;
        *components = 1;
        return true;
    default:
        return false;
    }
}

static uint32_t odyssey_plane_load(const uint8_t *bytes, size_t scalar_bytes)
{
    return scalar_bytes == 1 ? bytes[0] : bytes[0] | (uint32_t)bytes[1] << 8;
}

static void odyssey_plane_store(uint8_t *bytes, size_t scalar_bytes,
                                uint32_t value)
{
    bytes[0] = value;
    if (scalar_bytes == 2) {
        bytes[1] = value >> 8;
    }
}

static bool odyssey_plane_view_valid(const OdysseyPlaneView *view,
                                     Error **errp)
{
    uint8_t bits[4] = { 0 };
    size_t scalar_bytes = 0;
    size_t components = 0;
    size_t row_bytes;

    if (!view->bytes || !view->width || !view->height ||
        !odyssey_plane_raw_format(view->format, bits, &scalar_bytes,
                                  &components) ||
        view->role != SGI_RENDER_COLOUR ||
        (view->buffer != SGI_RENDER_FRONT &&
         view->buffer != SGI_RENDER_BACK) ||
        (view->eye != SGI_RENDER_LEFT && view->eye != SGI_RENDER_RIGHT) ||
        !view->backing_id || !view->view_id ||
        !view->backing_generation || !view->view_generation ||
        (components == 4 ? view->channel_mask & ~0xf || view->index_mask :
         view->channel_mask || view->index_mask >= (1u << bits[0])) ||
        !view->is_written ||
        !view->owns_pixel || !view->mark_written) {
        error_setg(errp, "Unsupported or incomplete synthetic Odyssey plane");
        return false;
    }
#if SIZE_MAX < UINT64_MAX
    if (view->width > SIZE_MAX / (components * scalar_bytes)) {
        error_setg(errp, "Synthetic Odyssey plane row is too wide");
        return false;
    }
#endif
    row_bytes = (size_t)view->width * components * scalar_bytes;
    if (view->stride_bytes < row_bytes ||
        view->height - 1 > (SIZE_MAX - row_bytes) / view->stride_bytes ||
        view->capacity_bytes < (size_t)(view->height - 1) *
                               view->stride_bytes + row_bytes) {
        error_setg(errp, "Synthetic Odyssey plane backing is too short");
        return false;
    }
    return true;
}

static OdysseyPlaneRawBinding odyssey_plane_raw_binding(
    const OdysseyPlaneView *view)
{
    return (OdysseyPlaneRawBinding) {
        .format = view->format, .role = view->role,
        .target = view->target, .buffer = view->buffer, .eye = view->eye,
        .width = view->width, .height = view->height,
        .stride_bytes = view->stride_bytes,
        .capacity_bytes = view->capacity_bytes,
        .backing_id = view->backing_id, .view_id = view->view_id,
        .bottom_origin = view->bottom_origin,
    };
}

static bool odyssey_plane_raw_binding_matches(const OdysseyPlaneView *view)
{
    const OdysseyPlaneRawBinding *saved = &view->submitted_raw;

    return view->format == saved->format && view->role == saved->role &&
           view->target == saved->target && view->buffer == saved->buffer &&
           view->eye == saved->eye && view->width == saved->width &&
           view->height == saved->height &&
           view->stride_bytes == saved->stride_bytes &&
           view->capacity_bytes == saved->capacity_bytes &&
           view->backing_id == saved->backing_id &&
           view->view_id == saved->view_id &&
           view->bottom_origin == saved->bottom_origin;
}

static bool odyssey_plane_alias_compatible(const OdysseyPlaneView *view,
                                            const OdysseyPlaneView *other)
{
    bool same_position = view->buffer == other->buffer &&
                         view->eye == other->eye;
    bool same_backing = view->backing_id == other->backing_id;
    uintptr_t a = (uintptr_t)view->bytes;
    uintptr_t b = (uintptr_t)other->bytes;
    bool overlapping = a <= b ? b - a < view->capacity_bytes :
                                a - b < other->capacity_bytes;

    return view->view_id != other->view_id &&
           view->target != other->target &&
           same_position == same_backing &&
           (!same_backing ||
            (view->backing_generation == other->backing_generation &&
             view->bytes == other->bytes &&
             view->capacity_bytes == other->capacity_bytes &&
             view->stride_bytes == other->stride_bytes &&
             view->width == other->width &&
             view->height == other->height &&
             view->format == other->format &&
             view->bottom_origin == other->bottom_origin &&
             view->storage_top_origin == other->storage_top_origin)) &&
           (same_backing || !overlapping);
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
        {
            uint8_t bits[4];
            size_t scalar_bytes;

            if (!odyssey_plane_rgba_format(view->format, bits,
                                           &scalar_bytes)) {
                error_setg(errp, "Colour index needs the raw Odyssey API");
                return false;
            }
        }
        for (size_t j = 0; j < i; j++) {
            if (!odyssey_plane_alias_compatible(view,
                                                &planes->views[j])) {
                error_setg(errp, "Inconsistent synthetic Odyssey plane alias");
                return false;
            }
        }
    }
    planes->resources = new_resources;
    for (size_t i = 0; i < planes->count; i++) {
        OdysseyPlaneView *view = &planes->views[i];
        uint8_t bits[4] = { 0 };
        size_t scalar_bytes = 0;

        odyssey_plane_rgba_format(view->format, bits, &scalar_bytes);

        view->submitted_bytes = view->bytes;
        view->submitted_storage_top_origin = view->storage_top_origin;
        view->submitted_backing_generation = view->backing_generation;
        view->submitted_view_generation = view->view_generation;
        view->expected_backing_generation = view->backing_generation;
        view->expected_view_generation = view->view_generation;
        view->submitted_raw = odyssey_plane_raw_binding(view);
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
                { SGI_RENDER_R, SGI_RENDER_UNORM, bits[0] },
                { SGI_RENDER_G, SGI_RENDER_UNORM, bits[1] },
                { SGI_RENDER_B, SGI_RENDER_UNORM, bits[2] },
                { SGI_RENDER_A, SGI_RENDER_UNORM, bits[3] },
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

bool odyssey_planes_raw_init(OdysseyPlanes *planes, OdysseyPlaneView *views,
                             size_t count, Error **errp)
{
    if (!planes || planes->initialized || !views || !count ||
        count > SIZE_MAX / sizeof(*views)) {
        error_setg(errp, "Missing synthetic Odyssey raw views");
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        OdysseyPlaneView *view = &views[i];

        if (!odyssey_plane_view_valid(view, errp)) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (!odyssey_plane_alias_compatible(view, &views[j])) {
                error_setg(errp, "Inconsistent synthetic Odyssey raw alias");
                return false;
            }
        }
    }
    *planes = (OdysseyPlanes) {
        .views = views, .count = count, .initialized = true,
    };
    for (size_t i = 0; i < count; i++) {
        OdysseyPlaneView *view = &views[i];

        view->submitted_bytes = view->bytes;
        view->submitted_storage_top_origin = view->storage_top_origin;
        view->submitted_backing_generation = view->backing_generation;
        view->submitted_view_generation = view->view_generation;
        view->expected_backing_generation = view->backing_generation;
        view->expected_view_generation = view->view_generation;
        view->submitted_raw = odyssey_plane_raw_binding(view);
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
        const SGIRenderResource *published = planes->resources ?
            &planes->resources[i] : NULL;
        uint8_t bits[4] = { 0 };
        size_t scalar_bytes = 0;
        unsigned buffer, eye;

        if (!odyssey_plane_view_valid(view, errp)) {
            return false;
        }
        if (!odyssey_plane_raw_binding_matches(view)) {
            error_setg(errp, "Changed synthetic Odyssey swap binding");
            return false;
        }
        odyssey_plane_rgba_format(view->format, bits, &scalar_bytes);
        if (view->bytes != view->submitted_bytes ||
            view->storage_top_origin !=
                view->submitted_storage_top_origin ||
            view->backing_generation != view->expected_backing_generation ||
            view->view_generation != view->expected_view_generation ||
            (published &&
             (view->backing_id != published->backing_id ||
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
              published->border[2])) ||
            view->view_generation == UINT64_MAX) {
            error_setg(errp, "Stale synthetic Odyssey swap view");
            return false;
        }
        if (published) {
            for (unsigned c = 0; c < 4; c++) {
                if (published->component[c].channel != channel[c] ||
                    published->component[c].numeric != SGI_RENDER_UNORM ||
                    published->component[c].storage_bits != bits[c]) {
                    error_setg(errp, "Changed synthetic Odyssey swap format");
                    return false;
                }
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
        if (!planes->resources) {
            front->expected_view_generation++;
            back->expected_view_generation++;
            front->submitted_raw = odyssey_plane_raw_binding(front);
            back->submitted_raw = odyssey_plane_raw_binding(back);
        }
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
        uint8_t bits[4] = { 0 };
        size_t scalar_bytes = 0;

        if (&planes->resources[i] != resource) {
            continue;
        }
        if (!odyssey_plane_view_valid(view, errp)) {
            return NULL;
        }
        odyssey_plane_rgba_format(view->format, bits, &scalar_bytes);
        if (slice || rect->x0 > rect->x1 || rect->y0 > rect->y1 ||
            rect->x1 >= view->width || rect->y1 >= view->height ||
            !odyssey_plane_raw_binding_matches(view) ||
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
            resource->bottom_origin != view->submitted_raw.bottom_origin) {
            error_setg(errp, "Stale or invalid synthetic Odyssey plane view");
            return NULL;
        }
        for (unsigned c = 0; c < 4; c++) {
            if (resource->component[c].channel != channel[c] ||
                resource->component[c].numeric != SGI_RENDER_UNORM ||
                resource->component[c].storage_bits != bits[c]) {
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

static bool odyssey_plane_raw_slots_valid(const SGIRenderRect *rect,
                                          size_t components,
                                          size_t stride_slots,
                                          size_t capacity_slots);

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
    uint8_t bits[4] = { 0 };
    size_t scalar_bytes = 0;

    if (!view) {
        return SGI_RENDER_RESOURCE_ERROR;
    }
    if (!slots || !odyssey_plane_raw_slots_valid(rect, 4, stride_slots,
                                                  capacity_slots)) {
        error_setg(errp, "Invalid synthetic Odyssey plane slots");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    odyssey_plane_rgba_format(view->format, bits, &scalar_bytes);
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        uint32_t row = odyssey_plane_storage_row(view, y);

        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t src = (size_t)row * view->stride_bytes +
                         (size_t)x * 4 * scalar_bytes;

            if (!view->is_written(view->opaque, x, row)) {
                error_setg(errp, "Synthetic Odyssey plane pixel unavailable");
                return SGI_RENDER_RESOURCE_ERROR;
            }
            for (unsigned c = 0; c < 4; c++) {
                if (odyssey_plane_load(view->bytes + src + c * scalar_bytes,
                                       scalar_bytes) >= (1u << bits[c])) {
                    error_setg(errp, "Synthetic Odyssey plane scalar "
                                     "out of range");
                    return SGI_RENDER_RESOURCE_ERROR;
                }
            }
        }
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        uint32_t row = odyssey_plane_storage_row(view, y);

        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t src = (size_t)row * view->stride_bytes +
                         (size_t)x * 4 * scalar_bytes;
            size_t dst = (size_t)(y - rect->y0) * stride_slots +
                         (size_t)(x - rect->x0) * 4;

            for (unsigned c = 0; c < 4; c++) {
                slots[dst + c] = odyssey_plane_load(
                    view->bytes + src + c * scalar_bytes, scalar_bytes);
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
    uint8_t bits[4] = { 0 };
    size_t scalar_bytes = 0;
    bool wrote = false;

    if (!view) {
        return result;
    }
    if (!slots || !odyssey_plane_raw_slots_valid(rect, 4, stride_slots,
                                                  capacity_slots)) {
        error_setg(errp, "Invalid synthetic Odyssey plane slots");
        return result;
    }
    if (writes) {
        size_t width = (size_t)rect->x1 - rect->x0 + 1;
        size_t height = (size_t)rect->y1 - rect->y0 + 1;

        if (!writes->pixel || writes->stride_bytes < width ||
            height - 1 > (SIZE_MAX - width) / writes->stride_bytes ||
            writes->capacity_bytes <
                (height - 1) * writes->stride_bytes + width) {
            error_setg(errp, "Invalid synthetic Odyssey write coverage");
            return result;
        }
    }
    odyssey_plane_rgba_format(view->format, bits, &scalar_bytes);
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t src = (size_t)(y - rect->y0) * stride_slots +
                         (size_t)(x - rect->x0) * 4;

            if (writes &&
                !writes->pixel[(size_t)(y - rect->y0) *
                               writes->stride_bytes + x - rect->x0]) {
                continue;
            }
            for (unsigned c = 0; c < 4; c++) {
                if ((view->channel_mask & (1u << c)) &&
                    slots[src + c] >= (1u << bits[c])) {
                    error_setg(errp, "Synthetic Odyssey plane scalar "
                                     "out of range");
                    return result;
                }
            }
        }
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        uint32_t row = odyssey_plane_storage_row(view, y);

        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t src = (size_t)(y - rect->y0) * stride_slots +
                         (size_t)(x - rect->x0) * 4;
            size_t dst = (size_t)row * view->stride_bytes +
                         (size_t)x * 4 * scalar_bytes;

            if (writes &&
                !writes->pixel[(size_t)(y - rect->y0) *
                               writes->stride_bytes + x - rect->x0]) {
                result.complete_pixels++;
                continue;
            }
            if (view->channel_mask &&
                (!view->owns_pixel(view->opaque, x, row) ||
                (view->channel_mask != 0xf &&
                 !view->is_written(view->opaque, x, row)))) {
                error_setg(errp, "Synthetic Odyssey plane write unavailable");
                if (wrote) {
                    odyssey_plane_invalidate_aliases(planes, view->backing_id);
                }
                planes->active = false;
                return result;
            }
            for (unsigned c = 0; c < 4; c++) {
                if (view->channel_mask & (1u << c)) {
                    odyssey_plane_store(view->bytes + dst + c * scalar_bytes,
                                        scalar_bytes, slots[src + c]);
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

static OdysseyPlaneRawStatus odyssey_plane_raw_ref(
    OdysseyPlanes *planes, const OdysseyPlaneRawRef *ref,
    const SGIRenderRect *rect, OdysseyPlaneView **viewp, Error **errp)
{
    OdysseyPlaneView *view;

    if (!planes || !planes->initialized || !planes->views || !ref || !rect ||
        ref->view_index >= planes->count || rect->x0 > rect->x1 ||
        rect->y0 > rect->y1) {
        error_setg(errp, "Invalid synthetic Odyssey raw reference");
        return ODYSSEY_PLANE_RAW_INVALID;
    }
    view = &planes->views[ref->view_index];
    if (!odyssey_plane_view_valid(view, errp)) {
        return ODYSSEY_PLANE_RAW_RESOURCE;
    }
    if (rect->x1 >= view->width || rect->y1 >= view->height ||
        !odyssey_plane_raw_binding_matches(view) ||
        ref->buffer != view->buffer || ref->eye != view->eye ||
        ref->backing_generation != view->backing_generation ||
        ref->view_generation != view->view_generation ||
        view->backing_generation != view->expected_backing_generation ||
        view->view_generation != view->expected_view_generation ||
        view->bytes != view->submitted_bytes ||
        view->storage_top_origin != view->submitted_storage_top_origin ||
        view->backing_generation == UINT64_MAX ||
        view->view_generation == UINT64_MAX) {
        error_setg(errp, "Stale synthetic Odyssey raw reference");
        return ODYSSEY_PLANE_RAW_RESOURCE;
    }
    *viewp = view;
    return ODYSSEY_PLANE_RAW_OK;
}

static bool odyssey_plane_raw_slots_valid(const SGIRenderRect *rect,
                                          size_t components,
                                          size_t stride_slots,
                                          size_t capacity_slots)
{
    size_t width = (size_t)rect->x1 - rect->x0 + 1;
    size_t height = (size_t)rect->y1 - rect->y0 + 1;

    return width <= SIZE_MAX / components &&
           stride_slots >= width * components &&
           height - 1 <= (SIZE_MAX - width * components) / stride_slots &&
           capacity_slots >= (height - 1) * stride_slots +
                             width * components;
}

OdysseyPlaneRawStatus odyssey_planes_raw_snapshot(
    OdysseyPlanes *planes, const OdysseyPlaneRawRef *ref,
    const SGIRenderRect *rect, uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp)
{
    OdysseyPlaneView *view;
    OdysseyPlaneRawStatus status;
    uint8_t bits[4] = { 0 };
    size_t scalar_bytes = 0, components = 0;

    status = odyssey_plane_raw_ref(planes, ref, rect, &view, errp);
    if (status != ODYSSEY_PLANE_RAW_OK) {
        return status;
    }
    odyssey_plane_raw_format(view->format, bits, &scalar_bytes, &components);
    if (!slots || !odyssey_plane_raw_slots_valid(rect, components,
                                                 stride_slots,
                                                 capacity_slots)) {
        error_setg(errp, "Invalid synthetic Odyssey raw slots");
        return ODYSSEY_PLANE_RAW_INVALID;
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        uint32_t row = odyssey_plane_storage_row(view, y);

        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t src = (size_t)row * view->stride_bytes +
                         (size_t)x * components * scalar_bytes;

            if (!view->is_written(view->opaque, x, row)) {
                error_setg(errp, "Synthetic Odyssey raw pixel unavailable");
                return ODYSSEY_PLANE_RAW_RESOURCE;
            }
            for (size_t c = 0; c < components; c++) {
                uint32_t value = odyssey_plane_load(
                    view->bytes + src + c * scalar_bytes, scalar_bytes);

                if (value >= (1u << bits[c])) {
                    error_setg(errp, "Synthetic Odyssey raw scalar "
                                     "out of range");
                    return ODYSSEY_PLANE_RAW_RESOURCE;
                }
            }
        }
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        uint32_t row = odyssey_plane_storage_row(view, y);

        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t src = (size_t)row * view->stride_bytes +
                         (size_t)x * components * scalar_bytes;
            size_t dst = (size_t)(y - rect->y0) * stride_slots +
                         (size_t)(x - rect->x0) * components;

            for (size_t c = 0; c < components; c++) {
                slots[dst + c] = odyssey_plane_load(
                    view->bytes + src + c * scalar_bytes, scalar_bytes);
            }
        }
    }
    return ODYSSEY_PLANE_RAW_OK;
}

static bool odyssey_plane_raw_covered(const OdysseyPlaneRawWrite *write,
                                      uint32_t x, uint32_t y)
{
    return !write->coverage ||
           write->coverage->pixel[(size_t)(y - write->rect.y0) *
                                  write->coverage->stride_bytes +
                                  x - write->rect.x0];
}

OdysseyPlaneRawStatus odyssey_planes_raw_commit_many(
    OdysseyPlanes *planes, const OdysseyPlaneRawWrite *writes, size_t count,
    OdysseyPlaneRawResult *results, Error **errp)
{
    if (!writes || !results || !count) {
        error_setg(errp, "Missing synthetic Odyssey raw writes");
        return ODYSSEY_PLANE_RAW_INVALID;
    }
    for (size_t i = 0; i < count; i++) {
        results[i] = (OdysseyPlaneRawResult) {
            .status = ODYSSEY_PLANE_RAW_NOT_RUN, .extent_known = true,
        };
    }
    /* Validate the entire submission before the first backing write. */
    for (size_t i = 0; i < count; i++) {
        const OdysseyPlaneRawWrite *write = &writes[i];
        OdysseyPlaneView *view;
        OdysseyPlaneRawStatus status;
        uint8_t bits[4] = { 0 };
        size_t scalar_bytes = 0, components = 0;
        size_t width, height;

        status = odyssey_plane_raw_ref(planes, &write->ref, &write->rect,
                                       &view, errp);
        if (status != ODYSSEY_PLANE_RAW_OK) {
            results[i].status = status;
            return status;
        }
        odyssey_plane_raw_format(view->format, bits, &scalar_bytes,
                                  &components);
        width = (size_t)write->rect.x1 - write->rect.x0 + 1;
        height = (size_t)write->rect.y1 - write->rect.y0 + 1;
        if (!write->slots ||
            !odyssey_plane_raw_slots_valid(&write->rect, components,
                                           write->stride_slots,
                                           write->capacity_slots) ||
            (write->coverage &&
             (!write->coverage->pixel ||
              write->coverage->stride_bytes < width ||
              height - 1 > (SIZE_MAX - width) /
                           write->coverage->stride_bytes ||
              write->coverage->capacity_bytes <
                  (height - 1) * write->coverage->stride_bytes + width))) {
            error_setg(errp, "Invalid synthetic Odyssey raw write slots");
            results[i].status = ODYSSEY_PLANE_RAW_INVALID;
            return ODYSSEY_PLANE_RAW_INVALID;
        }
        for (uint32_t y = write->rect.y0; y <= write->rect.y1; y++) {
            uint32_t row = odyssey_plane_storage_row(view, y);

            for (uint32_t x = write->rect.x0; x <= write->rect.x1; x++) {
                size_t src = (size_t)(y - write->rect.y0) *
                             write->stride_slots +
                             (size_t)(x - write->rect.x0) * components;
                size_t dst = (size_t)row * view->stride_bytes +
                             (size_t)x * components * scalar_bytes;

                if (!odyssey_plane_raw_covered(write, x, y)) {
                    continue;
                }
                for (size_t c = 0; c < components; c++) {
                    bool selected = components == 1 ? view->index_mask :
                                    view->channel_mask & (1u << c);

                    if (selected &&
                        write->slots[src + c] >= (1u << bits[c])) {
                        error_setg(errp, "Synthetic Odyssey raw scalar "
                                         "out of range");
                        results[i].status = ODYSSEY_PLANE_RAW_INVALID;
                        return ODYSSEY_PLANE_RAW_INVALID;
                    }
                    if (components == 4 && view->channel_mask &&
                        !selected && view->is_written(view->opaque, x, row) &&
                        odyssey_plane_load(view->bytes + dst +
                                           c * scalar_bytes,
                                           scalar_bytes) >= (1u << bits[c])) {
                        error_setg(errp, "Synthetic Odyssey raw preserved "
                                         "scalar out of range");
                        results[i].status = ODYSSEY_PLANE_RAW_RESOURCE;
                        return ODYSSEY_PLANE_RAW_RESOURCE;
                    }
                }
                if (components == 1 && view->index_mask !=
                    (1u << bits[0]) - 1 && view->index_mask &&
                    view->is_written(view->opaque, x, row) &&
                    odyssey_plane_load(view->bytes + dst, scalar_bytes) >=
                    (1u << bits[0])) {
                    error_setg(errp, "Synthetic Odyssey raw index "
                                     "out of range");
                    results[i].status = ODYSSEY_PLANE_RAW_RESOURCE;
                    return ODYSSEY_PLANE_RAW_RESOURCE;
                }
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        const OdysseyPlaneRawWrite *write = &writes[i];
        OdysseyPlaneRawResult *result = &results[i];
        OdysseyPlaneView *view = &planes->views[write->ref.view_index];
        uint8_t bits[4] = { 0 };
        size_t scalar_bytes = 0, components = 0;
        bool changed = false;

        odyssey_plane_raw_format(view->format, bits, &scalar_bytes,
                                  &components);
        for (uint32_t y = write->rect.y0; y <= write->rect.y1; y++) {
            uint32_t row = odyssey_plane_storage_row(view, y);

            for (uint32_t x = write->rect.x0; x <= write->rect.x1; x++) {
                size_t src = (size_t)(y - write->rect.y0) *
                             write->stride_slots +
                             (size_t)(x - write->rect.x0) * components;
                size_t dst = (size_t)row * view->stride_bytes +
                             (size_t)x * components * scalar_bytes;
                uint8_t mask = components == 1 ? !!view->index_mask :
                               view->channel_mask;
                bool written;

                if (!odyssey_plane_raw_covered(write, x, y) || !mask) {
                    result->complete_pixels++;
                    continue;
                }
                written = view->is_written(view->opaque, x, row);
                if (!view->owns_pixel(view->opaque, x, row) ||
                    (!written && (components == 1 ?
                     view->index_mask != (1u << bits[0]) - 1 :
                     mask != 0xf))) {
                    error_setg(errp, "Synthetic Odyssey raw write unavailable");
                    if (changed) {
                        odyssey_plane_invalidate_aliases(planes,
                                                         view->backing_id);
                    }
                    result->status = ODYSSEY_PLANE_RAW_RESOURCE;
                    return ODYSSEY_PLANE_RAW_RESOURCE;
                }
                if (components == 1) {
                    uint32_t old = written ? odyssey_plane_load(
                        view->bytes + dst, scalar_bytes) : 0;
                    uint32_t next = (old & ~view->index_mask) |
                                    (write->slots[src] & view->index_mask);

                    if (!written || old != next) {
                        changed = true;
                        result->component_mask |= 1;
                    }
                    odyssey_plane_store(view->bytes + dst, scalar_bytes, next);
                } else {
                    for (size_t c = 0; c < components; c++) {
                        uint32_t old;
                        uint32_t next;

                        if (!(mask & (1u << c))) {
                            continue;
                        }
                        old = odyssey_plane_load(view->bytes + dst +
                                                 c * scalar_bytes,
                                                 scalar_bytes);
                        next = write->slots[src + c];
                        if (!written || old != next) {
                            changed = true;
                            result->component_mask |= 1u << c;
                        }
                        odyssey_plane_store(view->bytes + dst +
                                            c * scalar_bytes,
                                            scalar_bytes, next);
                    }
                }
                view->mark_written(view->opaque, x, row);
                result->complete_pixels++;
            }
        }
        if (changed) {
            odyssey_plane_invalidate_aliases(planes, view->backing_id);
        }
        result->status = ODYSSEY_PLANE_RAW_OK;
    }
    return ODYSSEY_PLANE_RAW_OK;
}

OdysseyPlaneRawStatus odyssey_planes_raw_copy(
    OdysseyPlanes *planes, const OdysseyPlaneRawRef *source,
    const SGIRenderRect *source_rect, const OdysseyPlaneRawRef *destination,
    const SGIRenderRect *destination_rect, OdysseyPlaneRawResult *result,
    Error **errp)
{
    OdysseyPlaneView *src_view, *dst_view;
    OdysseyPlaneRawStatus status;
    OdysseyPlaneRawWrite write;
    uint8_t bits[4] = { 0 };
    size_t scalar_bytes = 0, components = 0;
    size_t width, height, stride, capacity;
    uint32_t *slots;

    if (!result) {
        error_setg(errp, "Missing synthetic Odyssey raw copy result");
        return ODYSSEY_PLANE_RAW_INVALID;
    }
    *result = (OdysseyPlaneRawResult) {
        .status = ODYSSEY_PLANE_RAW_NOT_RUN, .extent_known = true,
    };
    status = odyssey_plane_raw_ref(planes, source, source_rect,
                                   &src_view, errp);
    if (status == ODYSSEY_PLANE_RAW_OK) {
        status = odyssey_plane_raw_ref(planes, destination,
                                       destination_rect, &dst_view, errp);
    }
    if (status != ODYSSEY_PLANE_RAW_OK) {
        result->status = status;
        return status;
    }
    if (src_view->format != dst_view->format) {
        error_setg(errp, "Synthetic Odyssey raw copy format mismatch");
        result->status = ODYSSEY_PLANE_RAW_UNSUPPORTED;
        return result->status;
    }
    width = (size_t)source_rect->x1 - source_rect->x0 + 1;
    height = (size_t)source_rect->y1 - source_rect->y0 + 1;
    if (width != (size_t)destination_rect->x1 - destination_rect->x0 + 1 ||
        height != (size_t)destination_rect->y1 - destination_rect->y0 + 1) {
        error_setg(errp, "Synthetic Odyssey raw copy rectangles differ");
        result->status = ODYSSEY_PLANE_RAW_INVALID;
        return result->status;
    }
    odyssey_plane_raw_format(src_view->format, bits, &scalar_bytes,
                              &components);
    if (width > SIZE_MAX / components ||
        height > SIZE_MAX / (width * components)) {
        error_setg(errp, "Synthetic Odyssey raw copy is too large");
        result->status = ODYSSEY_PLANE_RAW_INVALID;
        return result->status;
    }
    stride = width * components;
    capacity = height * stride;
    slots = g_try_malloc_n(capacity, sizeof(*slots));
    if (!slots) {
        error_setg(errp, "Cannot allocate synthetic Odyssey raw copy");
        result->status = ODYSSEY_PLANE_RAW_NO_MEMORY;
        return result->status;
    }
    status = odyssey_planes_raw_snapshot(planes, source, source_rect,
                                         slots, stride, capacity, errp);
    if (status == ODYSSEY_PLANE_RAW_OK) {
        write = (OdysseyPlaneRawWrite) {
            .ref = *destination, .rect = *destination_rect,
            .slots = slots, .stride_slots = stride,
            .capacity_slots = capacity,
        };
        status = odyssey_planes_raw_commit_many(planes, &write, 1,
                                                result, errp);
    } else {
        result->status = status;
    }
    g_free(slots);
    return status;
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
