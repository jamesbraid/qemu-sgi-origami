/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/display/sgi/rasterizer.h"

static bool sgi_render_resource_span(const SGIRasterTarget *target,
                                     const SGIRenderResource *resource,
                                     uint32_t slice, const SGIRenderRect *rect,
                                     const uint32_t *slots,
                                     size_t stride_slots,
                                     size_t capacity_slots,
                                     uint64_t *pixel_count, Error **errp)
{
    uint32_t channels = 0;
    uint64_t width, rows;
    size_t row_slots, required;
    bool declared = false;

    if (!target || !resource || !rect || !slots || !target->resources ||
        !target->resource_ops) {
        error_setg(errp, "Missing SGI render resource or provider");
        return false;
    }
    for (size_t i = 0; i < target->resource_count; i++) {
        if (&target->resources[i] == resource) {
            declared = true;
            break;
        }
    }
    if (!declared || !resource->width || !resource->height ||
        !resource->depth || !resource->component_count ||
        resource->component_count > ARRAY_SIZE(resource->component) ||
        resource->role > SGI_RENDER_TEXTURE ||
        resource->buffer > SGI_RENDER_AUX ||
        resource->eye > SGI_RENDER_RIGHT) {
        error_setg(errp, "Invalid SGI render resource description");
        return false;
    }
    if (resource->role != SGI_RENDER_TEXTURE &&
        (resource->border[0] || resource->border[1] ||
         resource->border[2])) {
        error_setg(errp, "SGI render attachment has texture borders");
        return false;
    }
    if (resource->role == SGI_RENDER_TEXTURE &&
        ((uint64_t)resource->border[0] * 2 >= resource->width ||
         (uint64_t)resource->border[1] * 2 >= resource->height ||
         (uint64_t)resource->border[2] * 2 >= resource->depth)) {
        error_setg(errp, "SGI render texture has no interior texel");
        return false;
    }
    for (unsigned i = 0; i < resource->component_count; i++) {
        const SGIRenderComponent *component = &resource->component[i];

        if (component->channel > SGI_RENDER_SCALAR ||
            component->numeric > SGI_RENDER_FLOAT ||
            !component->storage_bits || component->storage_bits > 32 ||
            (component->numeric == SGI_RENDER_FLOAT &&
             component->storage_bits != 16 && component->storage_bits != 32) ||
            (channels & (1u << component->channel)) ||
            (component->channel == SGI_RENDER_SCALAR &&
             resource->component_count != 1)) {
            error_setg(errp, "Invalid SGI render component description");
            return false;
        }
        channels |= 1u << component->channel;
    }
    if (slice >= resource->depth || rect->x0 > rect->x1 ||
        rect->y0 > rect->y1 || rect->x1 >= resource->width ||
        rect->y1 >= resource->height) {
        error_setg(errp, "SGI render rectangle is outside its resource");
        return false;
    }
    width = (uint64_t)rect->x1 - rect->x0 + 1;
    rows = (uint64_t)rect->y1 - rect->y0 + 1;
    if (width > SIZE_MAX / resource->component_count) {
        error_setg(errp, "SGI render row exceeds host address space");
        return false;
    }
    row_slots = width * resource->component_count;
    if (stride_slots < row_slots ||
        rows - 1 > (SIZE_MAX - row_slots) / stride_slots) {
        error_setg(errp, "SGI render row stride is invalid");
        return false;
    }
    required = (rows - 1) * stride_slots + row_slots;
    if (capacity_slots < required) {
        error_setg(errp, "SGI render slot array is too small");
        return false;
    }
    *pixel_count = width * rows;
    return true;
}

static bool sgi_render_resource_slots(const SGIRenderResource *resource,
                                      const SGIRenderRect *rect,
                                      const uint32_t *slots,
                                      size_t stride_slots, Error **errp)
{
    size_t width = (uint64_t)rect->x1 - rect->x0 + 1;
    size_t rows = (uint64_t)rect->y1 - rect->y0 + 1;

    for (size_t y = 0; y < rows; y++) {
        for (size_t x = 0; x < width; x++) {
            for (unsigned c = 0; c < resource->component_count; c++) {
                unsigned bits = resource->component[c].storage_bits;
                uint32_t value = slots[y * stride_slots +
                                       x * resource->component_count + c];

                if (bits < 32 && value >> bits) {
                    if (errp && !*errp) {
                        error_setg(errp, "SGI render component exceeds its "
                                         "declared storage width");
                    }
                    return false;
                }
            }
        }
    }
    return true;
}

SGIRenderResult sgi_render_resource_snapshot(const SGIRasterTarget *target,
                                              const SGIRenderResource *resource,
                                              uint32_t slice,
                                              const SGIRenderRect *rect,
                                              uint32_t *slots,
                                              size_t stride_slots,
                                              size_t capacity_slots,
                                              Error **errp)
{
    SGIRenderResult result;
    uint64_t pixels;

    if (!sgi_render_resource_span(target, resource, slice, rect, slots,
                                  stride_slots, capacity_slots,
                                  &pixels, errp)) {
        return SGI_RENDER_RESOURCE_ERROR;
    }
    if (!target->resource_ops->snapshot) {
        error_setg(errp, "SGI render resource has no snapshot provider");
        return SGI_RENDER_UNSUPPORTED;
    }
    result = target->resource_ops->snapshot(target->opaque, resource, slice,
                                            rect, slots, stride_slots,
                                            capacity_slots, errp);
    if (result == SGI_RENDER_OK &&
        !sgi_render_resource_slots(resource, rect, slots,
                                   stride_slots, errp)) {
        return SGI_RENDER_HOST_ERROR;
    }
    if (result != SGI_RENDER_OK && errp && !*errp) {
        error_setg(errp, "SGI render resource snapshot failed");
    }
    return result;
}

static SGIRenderCommitResult sgi_render_commit_result(
    SGIRenderCommitResult result, uint64_t pixels, Error **errp)
{
    if (result.status == SGI_RENDER_OK) {
        if (result.complete_pixels != pixels || result.partial_pixel ||
            !result.extent_known) {
            if (errp && !*errp) {
                error_setg(errp, "SGI render commit reported "
                                 "incomplete success");
            }
            result.status = SGI_RENDER_HOST_ERROR;
            result.extent_known = false;
        }
    } else {
        if (result.extent_known &&
            (result.complete_pixels > pixels ||
             (result.partial_pixel && result.complete_pixels == pixels))) {
            if (errp && !*errp) {
                error_setg(errp, "SGI render commit reported invalid extent");
            }
            result.status = SGI_RENDER_HOST_ERROR;
            result.extent_known = false;
        } else if (errp && !*errp) {
            error_setg(errp, "SGI render resource commit failed");
        }
    }
    return result;
}

SGIRenderCommitResult sgi_render_resource_commit(
    const SGIRasterTarget *target, const SGIRenderResource *resource,
    uint32_t slice, const SGIRenderRect *rect, const uint32_t *slots,
    size_t stride_slots, size_t capacity_slots, Error **errp)
{
    SGIRenderCommitResult result = {
        .status = SGI_RENDER_RESOURCE_ERROR,
        .extent_known = true,
    };
    uint64_t pixels;

    if (!sgi_render_resource_span(target, resource, slice, rect, slots,
                                  stride_slots, capacity_slots,
                                  &pixels, errp)) {
        return result;
    }
    if (!target->resource_ops->commit) {
        error_setg(errp, "SGI render resource has no commit provider");
        result.status = SGI_RENDER_UNSUPPORTED;
        return result;
    }
    if (!sgi_render_resource_slots(resource, rect, slots,
                                   stride_slots, errp)) {
        result.status = SGI_RENDER_HOST_ERROR;
        return result;
    }
    result = target->resource_ops->commit(target->opaque, resource, slice,
                                          rect, slots, stride_slots,
                                          capacity_slots, errp);
    return sgi_render_commit_result(result, pixels, errp);
}

/* A write coverage array that holds a binary byte for each rect pixel. */
static bool sgi_render_resource_coverage(const SGIRenderRect *rect,
                                         const SGIRenderWriteCoverage *writes,
                                         Error **errp)
{
    uint64_t width = (uint64_t)rect->x1 - rect->x0 + 1;
    uint64_t rows = (uint64_t)rect->y1 - rect->y0 + 1;
    size_t required;

    if (!writes || !writes->pixel || width > SIZE_MAX ||
        writes->stride_bytes < width ||
        rows - 1 > (SIZE_MAX - width) / writes->stride_bytes) {
        error_setg(errp, "Invalid SGI render write coverage");
        return false;
    }
    required = (rows - 1) * writes->stride_bytes + width;
    if (writes->capacity_bytes < required) {
        error_setg(errp, "SGI render write coverage is too small");
        return false;
    }
    for (uint64_t y = 0; y < rows; y++) {
        for (uint64_t x = 0; x < width; x++) {
            if (writes->pixel[y * writes->stride_bytes + x] > 1) {
                error_setg(errp, "SGI render write coverage is not binary");
                return false;
            }
        }
    }
    return true;
}

SGIRenderCommitResult sgi_render_resource_commit_covered(
    const SGIRasterTarget *target, const SGIRenderResource *resource,
    uint32_t slice, const SGIRenderRect *rect, const uint32_t *slots,
    size_t stride_slots, size_t capacity_slots,
    const SGIRenderWriteCoverage *writes, Error **errp)
{
    SGIRenderCommitResult result = {
        .status = SGI_RENDER_RESOURCE_ERROR,
        .extent_known = true,
    };
    uint64_t pixels;

    if (!sgi_render_resource_span(target, resource, slice, rect, slots,
                                  stride_slots, capacity_slots,
                                  &pixels, errp) ||
        !sgi_render_resource_coverage(rect, writes, errp)) {
        return result;
    }
    if (!target->resource_ops->commit_covered) {
        error_setg(errp, "SGI render resource has no covered commit provider");
        result.status = SGI_RENDER_UNSUPPORTED;
        return result;
    }
    if (!sgi_render_resource_slots(resource, rect, slots,
                                   stride_slots, errp)) {
        result.status = SGI_RENDER_HOST_ERROR;
        return result;
    }
    result = target->resource_ops->commit_covered(target->opaque, resource,
                                                  slice, rect, slots,
                                                  stride_slots, capacity_slots,
                                                  writes, errp);
    return sgi_render_commit_result(result, pixels, errp);
}

SGIRenderCommitResult sgi_render_resource_commit_index_covered(
    const SGIRasterTarget *target, const SGIRenderResource *resource,
    uint32_t slice, const SGIRenderRect *rect, const uint32_t *slots,
    size_t stride_slots, size_t capacity_slots,
    const SGIRenderWriteCoverage *writes, uint32_t effective_mask,
    Error **errp)
{
    SGIRenderCommitResult result = {
        .status = SGI_RENDER_RESOURCE_ERROR,
        .extent_known = true,
    };
    uint64_t pixels;
    unsigned bits;

    if (!sgi_render_resource_span(target, resource, slice, rect, slots,
                                  stride_slots, capacity_slots,
                                  &pixels, errp) ||
        !sgi_render_resource_coverage(rect, writes, errp)) {
        return result;
    }
    bits = resource->component[0].storage_bits;
    if (resource->role != SGI_RENDER_INDEX ||
        resource->component_count != 1 ||
        resource->component[0].channel != SGI_RENDER_SCALAR ||
        resource->component[0].numeric != SGI_RENDER_UINT ||
        (bits < 32 && effective_mask >> bits)) {
        error_setg(errp, "Invalid SGI render index commit");
        return result;
    }
    if (!target->resource_ops->commit_index_covered) {
        error_setg(errp, "SGI render resource has no index commit provider");
        result.status = SGI_RENDER_UNSUPPORTED;
        return result;
    }
    if (!sgi_render_resource_slots(resource, rect, slots,
                                   stride_slots, errp)) {
        result.status = SGI_RENDER_HOST_ERROR;
        return result;
    }
    result = target->resource_ops->commit_index_covered(
        target->opaque, resource, slice, rect, slots, stride_slots,
        capacity_slots, writes, effective_mask, errp);
    return sgi_render_commit_result(result, pixels, errp);
}
