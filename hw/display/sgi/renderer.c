/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/display/sgi/rasterizer.h"
#include "ui/sgi-renderer.h"

static const SGIRenderResource *sgi_render_colour_resource(
    const SGIRasterTarget *target, SGIColourTarget colour)
{
    const SGIRenderResource *found = NULL;

    if (!target->resources) {
        return NULL;
    }
    for (size_t i = 0; i < target->resource_count; i++) {
        const SGIRenderResource *res = &target->resources[i];

        if (res->role == SGI_RENDER_COLOUR && res->target == colour) {
            if (found) {
                return NULL;
            }
            found = res;
        }
    }
    return found;
}

static bool sgi_render_plain_triangle(const SGIRasterTarget *target,
                                      const SGIGLDraw *draw,
                                      const SGIRenderResource **result)
{
    const SGIGLDrawState *st = draw->state;
    const SGIRenderResource *res;
    const SGIClipList *clip = &st->drawable.clip;

    if (draw->mode != SGI_GL_TRIANGLES || draw->count != 3 ||
        draw->back_colors || draw->polygon_modes ||
        draw->polygon_stipple_enabled || draw->point_smooth ||
        draw->line_smooth || draw->polygon_smooth ||
        draw->line_stipple_enabled ||
        st->shade_model != SGI_GL_SMOOTH || st->index_bits ||
        st->draw_buffers != SGI_GL_DRAW_FRONT ||
        st->colour_mask != SGI_GL_COLOUR_MASK_ALL ||
        st->depth_test || st->stencil_test || st->blend ||
        st->alpha_test || st->fog || st->cull_face || st->clip_planes ||
        st->scissor_test || st->color_logic_op ||
        st->index_logic_op || st->texture_2d || st->texture_3d ||
        st->polygon_offset || st->window_matrix_given ||
        !st->drawable.bound || clip->n != 1 || clip->empty ||
        st->drawable.origin[0] || st->drawable.origin[1]) {
        return false;
    }
    for (unsigned i = 0; i < 16; i++) {
        if (!isfinite(st->modelview[i]) || !isfinite(st->projection[i])) {
            return false;
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        const float *vertex = draw->vertices +
                              i * SGI_GL_VERTEX_FLOATS;

        for (unsigned c = 0; c < 4; c++) {
            if (!isfinite(vertex[SGI_GL_V_POSITION + c]) ||
                !isfinite(vertex[SGI_GL_V_COLOR + c])) {
                return false;
            }
        }
    }
    res = sgi_render_colour_resource(target, st->drawable.front);
    if (!res || !res->width || !res->height || res->width > INT_MAX ||
        res->height > INT_MAX || res->depth != 1 ||
        !res->bottom_origin || res->buffer != SGI_RENDER_FRONT ||
        (res->component_count != 3 && res->component_count != 4) ||
        st->viewport[0] || st->viewport[1] ||
        st->viewport[2] != res->width ||
        st->viewport[3] != res->height ||
        clip->r[0].x0 || clip->r[0].y0 ||
        clip->r[0].x1 != (int32_t)res->width - 1 ||
        clip->r[0].y1 != (int32_t)res->height - 1) {
        return false;
    }
    for (unsigned c = 0; c < res->component_count; c++) {
        if (res->component[c].channel != SGI_RENDER_R + c ||
            res->component[c].numeric != SGI_RENDER_UNORM ||
            !res->component[c].storage_bits ||
            res->component[c].storage_bits > 16) {
            return false;
        }
    }
    *result = res;
    return true;
}

SGIRenderResult sgi_render_draw_checked(SGIRenderer *renderer,
                                        const SGIRasterRules *rules,
                                        const SGIRasterTarget *target,
                                        const SGIGLDraw *draw, Error **errp)
{
    const SGIRenderResource *resource;
    SGIRenderRect rect;
    SGIRendererFrame frame = { 0 };
    g_autofree uint32_t *slots = NULL;
    g_autofree uint16_t *rgba = NULL;
    g_autofree uint8_t *written = NULL;
    float vertices[3][SGI_RENDERER_FRAME_VERTEX_FLOATS];
    size_t pixels, nslots, nrgba;
    bool any_written = false;
    SGIRenderCommitResult committed;
    SGIRenderResult status;

    if (!renderer || !rules || !target || !draw || !draw->state ||
        !draw->vertices) {
        error_setg(errp, "Missing SGI Mesa draw input");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    if (!draw->state->drawable.bound) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: draw with no window bound "
                      "dropped\n");
        return SGI_RENDER_OK;
    }
    if (!draw->state->drawable.clip.n) {
        return SGI_RENDER_OK;
    }
    if (draw->state->dither && rules->dither != SGI_DITHER_NONE) {
        error_setg(errp, "SGI Mesa ordered dithering is unsupported");
        return SGI_RENDER_UNSUPPORTED;
    }
    if (!sgi_render_plain_triangle(target, draw, &resource)) {
        error_setg(errp, "Unsupported SGI Mesa draw state or resource");
        return SGI_RENDER_UNSUPPORTED;
    }
    if (!target->resource_ops || !target->resource_ops->commit_covered) {
        error_setg(errp, "SGI Mesa draw needs a covered commit provider");
        return SGI_RENDER_UNSUPPORTED;
    }
    if (resource->width > SIZE_MAX / resource->height ||
        (uint64_t)resource->width * resource->height >
            SIZE_MAX / 4 / sizeof(*rgba)) {
        error_setg(errp, "SGI Mesa draw exceeds host address space");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    pixels = (size_t)resource->width * resource->height;
    if (pixels > SIZE_MAX / resource->component_count) {
        error_setg(errp, "SGI Mesa component array is too large");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    nslots = pixels * resource->component_count;
    nrgba = pixels * 4;
    slots = g_try_new(uint32_t, nslots);
    rgba = g_try_new(uint16_t, nrgba);
    written = g_try_new(uint8_t, pixels);
    if (!slots || !rgba || !written) {
        error_setg(errp, "Cannot allocate SGI Mesa draw buffers");
        return SGI_RENDER_OOM;
    }
    rect = (SGIRenderRect) {
        .x1 = resource->width - 1,
        .y1 = resource->height - 1,
    };
    status = sgi_render_resource_snapshot(target, resource, 0, &rect,
                                          slots,
                                          resource->width *
                                          resource->component_count,
                                          nslots, errp);
    if (status != SGI_RENDER_OK) {
        return status;
    }
    for (size_t i = 0; i < pixels; i++) {
        for (unsigned c = 0; c < resource->component_count; c++) {
            unsigned bits = resource->component[c].storage_bits;
            uint32_t max = (1u << bits) - 1;

            rgba[i * 4 + c] = ((uint64_t)slots[i *
                                resource->component_count + c] * 65535 +
                                max / 2) / max;
        }
        if (resource->component_count == 3) {
            rgba[i * 4 + 3] = 65535;
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        const float *src = draw->vertices +
                           i * SGI_GL_VERTEX_FLOATS;

        memcpy(&vertices[i][0], &src[SGI_GL_V_POSITION], 4 * sizeof(float));
        memcpy(&vertices[i][4], &src[SGI_GL_V_COLOR], 4 * sizeof(float));
    }
    frame.width = resource->width;
    frame.height = resource->height;
    frame.count = 3;
    frame.vertices = &vertices[0][0];
    frame.rgba = rgba;
    frame.written = written;
    frame.written_capacity = pixels;
    memcpy(frame.modelview, draw->state->modelview,
           sizeof(frame.modelview));
    memcpy(frame.projection, draw->state->projection,
           sizeof(frame.projection));
    if (!sgi_renderer_draw_frame(renderer, &frame, errp)) {
        return SGI_RENDER_HOST_ERROR;
    }
    for (size_t i = 0; i < pixels; i++) {
        if (written[i] > 1) {
            error_setg(errp, "SGI Mesa draw returned invalid write coverage");
            return SGI_RENDER_HOST_ERROR;
        }
        any_written |= written[i] != 0;
    }
    if (!any_written) {
        return SGI_RENDER_OK;
    }
    for (size_t i = 0; i < pixels; i++) {
        if (!written[i]) {
            continue;
        }
        for (unsigned c = 0; c < resource->component_count; c++) {
            unsigned bits = resource->component[c].storage_bits;
            uint32_t max = (1u << bits) - 1;

            slots[i * resource->component_count + c] =
                ((uint64_t)rgba[i * 4 + c] * max + 32767) / 65535;
        }
    }
    SGIRenderWriteCoverage writes = {
        .pixel = written,
        .stride_bytes = resource->width,
        .capacity_bytes = pixels,
    };

    committed = sgi_render_resource_commit_covered(target, resource, 0,
                                                   &rect, slots,
                                                   resource->width *
                                                   resource->component_count,
                                                   nslots, &writes, errp);
    return committed.status;
}

static void sgi_render_clear_coverage_row(const SGIRendererClear *clear,
                                          uint32_t width, uint32_t y,
                                          uint8_t *covered)
{
    memset(covered, 0, width);
    for (unsigned j = 0; j < clear->rect_count; j++) {
        const SGIRendererClearRect *r = &clear->rects[j];

        if (y >= r->y0 && y <= r->y1) {
            memset(covered + r->x0, 1, r->x1 - r->x0 + 1);
        }
    }
}

static bool sgi_render_clear_covers_plane(const SGIRendererClear *clear,
                                          const SGIRenderResource *res,
                                          uint8_t *covered)
{
    if (clear->rect_count == 1) {
        const SGIRendererClearRect *r = &clear->rects[0];

        return !r->x0 && !r->y0 && r->x1 == res->width - 1 &&
               r->y1 == res->height - 1;
    }
    for (uint32_t y = 0; y < res->height; y++) {
        sgi_render_clear_coverage_row(clear, res->width, y, covered);
        if (memchr(covered, 0, res->width)) {
            return false;
        }
    }
    return true;
}

SGIRenderResult sgi_render_raster_checked(SGIRenderer *renderer,
                                          const SGIRasterRules *rules,
                                          const SGIRasterTarget *target,
                                          const SGIRasterOp *op, Error **errp)
{
    const SGIRenderResource *resources[SGI_GL_MAX_TARGETS];
    SGIRendererClear clears[SGI_GL_MAX_TARGETS] = { 0 };
    uint32_t *slots[SGI_GL_MAX_TARGETS] = { 0 };
    uint16_t *rgba[SGI_GL_MAX_TARGETS] = { 0 };
    uint8_t *covered = NULL;
    uint32_t max_width = 0;
    size_t pixels[SGI_GL_MAX_TARGETS], nslots[SGI_GL_MAX_TARGETS];
    SGIRenderResult status = SGI_RENDER_OK;

    if (!renderer || !rules || !target || !op) {
        error_setg(errp, "Missing SGI Mesa raster input");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    if (op->kind != SGI_RASTER_CLEAR ||
        (op->clear_buffers & ~(SGI_GL_COLOR_BUFFER_BIT |
                               SGI_GL_DEPTH_BUFFER_BIT |
                               SGI_GL_STENCIL_BUFFER_BIT)) ||
        (op->clear_buffers & (SGI_GL_DEPTH_BUFFER_BIT |
                              SGI_GL_STENCIL_BUFFER_BIT)) ||
        op->index_bits) {
        error_setg(errp, "Unsupported SGI Mesa raster operation or plane");
        return SGI_RENDER_UNSUPPORTED;
    }
    if (op->x0 > op->x1 || op->y0 > op->y1 ||
        op->ntargets > SGI_GL_MAX_TARGETS ||
        op->clip.n > SGI_CLIP_MAX ||
        (op->clear_colour_mask & ~SGI_GL_COLOUR_MASK_ALL)) {
        error_setg(errp, "Invalid SGI Mesa clear geometry or mask");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    if (!(op->clear_buffers & SGI_GL_COLOR_BUFFER_BIT) ||
        !op->clear_colour_mask || !op->ntargets ||
        (!op->clip.n && op->clip.empty)) {
        return SGI_RENDER_OK;
    }
    for (unsigned c = 0; c < 4; c++) {
        if (!isfinite(op->clear_rgba[c])) {
            error_setg(errp, "Invalid SGI Mesa clear colour");
            return SGI_RENDER_RESOURCE_ERROR;
        }
    }
    for (unsigned i = 0; i < op->ntargets; i++) {
        SGIRendererClear *clear = &clears[i];
        const SGIRenderResource *res;
        int32_t x0, y0, x1, y1;

        for (unsigned j = 0; j < i; j++) {
            if (op->targets[j] == op->targets[i]) {
                error_setg(errp, "Duplicate SGI Mesa clear target");
                return SGI_RENDER_RESOURCE_ERROR;
            }
        }
        res = sgi_render_colour_resource(target, op->targets[i]);
        if (!res || !res->width || !res->height ||
            res->width > INT_MAX || res->height > INT_MAX ||
            res->depth != 1 || !res->bottom_origin ||
            (res->component_count != 3 && res->component_count != 4)) {
            error_setg(errp, "Unsupported SGI Mesa clear resource");
            return SGI_RENDER_UNSUPPORTED;
        }
        for (unsigned c = 0; c < res->component_count; c++) {
            if (res->component[c].channel != SGI_RENDER_R + c ||
                res->component[c].numeric != SGI_RENDER_UNORM ||
                !res->component[c].storage_bits ||
                res->component[c].storage_bits > 16) {
                error_setg(errp, "Unsupported SGI Mesa clear component");
                return SGI_RENDER_UNSUPPORTED;
            }
        }
        if ((uint64_t)res->width * res->height >
            MIN(SIZE_MAX / 4 / sizeof(uint16_t),
                SIZE_MAX / res->component_count)) {
            error_setg(errp, "SGI Mesa clear exceeds host address space");
            return SGI_RENDER_RESOURCE_ERROR;
        }
        resources[i] = res;
        max_width = MAX(max_width, res->width);
        clear->width = res->width;
        clear->height = res->height;
        clear->colour_mask = op->clear_colour_mask;
        memcpy(clear->rgba, op->clear_rgba, sizeof(clear->rgba));
        x0 = MAX(op->x0, 0);
        y0 = MAX(op->y0, 0);
        x1 = MIN(op->x1, (int32_t)res->width - 1);
        y1 = MIN(op->y1, (int32_t)res->height - 1);
        if (x0 > x1 || y0 > y1) {
            continue;
        }
        if (!op->clip.n) {
            clear->rects[clear->rect_count++] = (SGIRendererClearRect) {
                x0, y0, x1, y1,
            };
            continue;
        }
        for (unsigned j = 0; j < op->clip.n; j++) {
            const SGIClipRect *clip = &op->clip.r[j];
            int32_t cx0 = MAX(x0, clip->x0);
            int32_t cy0 = MAX(y0, clip->y0);
            int32_t cx1 = MIN(x1, clip->x1);
            int32_t cy1 = MIN(y1, clip->y1);

            if (cx0 <= cx1 && cy0 <= cy1) {
                clear->rects[clear->rect_count++] =
                    (SGIRendererClearRect) { cx0, cy0, cx1, cy1 };
            }
        }
    }
    for (unsigned i = 0; i < op->ntargets; i++) {
        const SGIRenderResource *res = resources[i];
        SGIRendererClear *clear = &clears[i];
        SGIRenderRect rect = { .x1 = res->width - 1,
                               .y1 = res->height - 1 };

        if (!clear->rect_count) {
            continue;
        }
        pixels[i] = (size_t)res->width * res->height;
        nslots[i] = pixels[i] * res->component_count;
        slots[i] = g_try_new(uint32_t, nslots[i]);
        rgba[i] = g_try_new(uint16_t, pixels[i] * 4);
        if (!slots[i] || !rgba[i]) {
            error_setg(errp, "Cannot allocate SGI Mesa clear buffers");
            status = SGI_RENDER_OOM;
            goto out;
        }
        status = sgi_render_resource_snapshot(target, res, 0, &rect,
                                              slots[i],
                                              res->width *
                                              res->component_count,
                                              nslots[i], errp);
        if (status != SGI_RENDER_OK) {
            goto out;
        }
        for (size_t pixel = 0; pixel < pixels[i]; pixel++) {
            for (unsigned c = 0; c < res->component_count; c++) {
                unsigned bits = res->component[c].storage_bits;
                uint32_t max = (1u << bits) - 1;

                rgba[i][pixel * 4 + c] =
                    ((uint64_t)slots[i][pixel * res->component_count + c] *
                     65535 + max / 2) / max;
            }
            if (res->component_count == 3) {
                rgba[i][pixel * 4 + 3] = 65535;
            }
        }
        clear->pixels = rgba[i];
    }
    for (unsigned i = 0; i < op->ntargets; i++) {
        const SGIRenderResource *res = resources[i];
        SGIRendererClear *clear = &clears[i];

        if (!clear->rect_count) {
            continue;
        }
        if (!sgi_renderer_clear(renderer, clear, errp)) {
            status = SGI_RENDER_HOST_ERROR;
            goto out;
        }
        for (unsigned j = 0; j < clear->rect_count; j++) {
            const SGIRendererClearRect *r = &clear->rects[j];

            for (uint32_t y = r->y0; y <= r->y1; y++) {
                for (uint32_t x = r->x0; x <= r->x1; x++) {
                    size_t pixel = (size_t)y * res->width + x;

                    for (unsigned c = 0; c < res->component_count; c++) {
                        unsigned bits = res->component[c].storage_bits;
                        uint32_t max = (1u << bits) - 1;

                        if (clear->colour_mask & (1u << c)) {
                            slots[i][pixel * res->component_count + c] =
                                ((uint64_t)rgba[i][pixel * 4 + c] * max +
                                 32767) / 65535;
                        }
                    }
                }
            }
        }
    }
    covered = g_try_new(uint8_t, max_width);
    if (!covered) {
        error_setg(errp, "Cannot allocate SGI Mesa clear coverage row");
        status = SGI_RENDER_OOM;
        goto out;
    }
    for (unsigned i = 0; i < op->ntargets; i++) {
        const SGIRenderResource *res = resources[i];
        SGIRendererClear *clear = &clears[i];

        if (!clear->rect_count) {
            continue;
        }
        if (sgi_render_clear_covers_plane(clear, res, covered)) {
            SGIRenderRect rect = { .x1 = res->width - 1,
                                   .y1 = res->height - 1 };
            SGIRenderCommitResult committed = sgi_render_resource_commit(
                target, res, 0, &rect, slots[i],
                res->width * res->component_count, nslots[i], errp);

            if (committed.status != SGI_RENDER_OK) {
                status = committed.status;
                goto out;
            }
            continue;
        }
        for (uint32_t y = 0; y < res->height; y++) {
            uint32_t x = 0;

            sgi_render_clear_coverage_row(clear, res->width, y, covered);
            while (x < res->width) {
                SGIRenderRect rect;
                SGIRenderCommitResult committed;
                size_t offset, length;

                if (!covered[x]) {
                    x++;
                    continue;
                }
                rect.x0 = x;
                rect.y0 = rect.y1 = y;
                do {
                    x++;
                } while (x < res->width && covered[x]);
                rect.x1 = x - 1;
                offset = ((size_t)y * res->width + rect.x0) *
                         res->component_count;
                length = (size_t)(rect.x1 - rect.x0 + 1) *
                         res->component_count;
                committed = sgi_render_resource_commit(target, res, 0,
                                                       &rect, slots[i] + offset,
                                                       length, length, errp);
                if (committed.status != SGI_RENDER_OK) {
                    status = committed.status;
                    goto out;
                }
            }
        }
    }
out:
    g_free(covered);
    for (unsigned i = 0; i < SGI_GL_MAX_TARGETS; i++) {
        g_free(slots[i]);
        g_free(rgba[i]);
    }
    return status;
}
