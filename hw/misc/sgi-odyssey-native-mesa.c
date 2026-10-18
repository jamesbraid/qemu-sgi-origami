/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
#include "hw/misc/sgi-odyssey-native-mesa.h"
#include "qemu/error-report.h"
#include "trace.h"

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
            /* GL row g is plane row g; see native_map_rows(). */
            .bottom_origin = true,
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
            resource->eye != SGI_RENDER_MONO || !resource->bottom_origin ||
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
    uint32_t successful_stores = 0;
    bool changed = false;

    if (b < 0 || !writes) {
        error_setg(errp, "Invalid Odyssey native Mesa commit resource");
        trace_sgi_odyssey_native_commit_covered(
            b, 0, 0, result.partial_pixel, result.extent_known,
            result.status);
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
                trace_sgi_odyssey_native_commit_covered(
                    b, result.complete_pixels, successful_stores,
                    result.partial_pixel, result.extent_known, result.status);
                return result;
            }
            changed = true;
            successful_stores++;
            result.complete_pixels++;
        }
    }
    if (changed) {
        native_invalidate(native);
    }
    result.status = SGI_RENDER_OK;
    trace_sgi_odyssey_native_commit_covered(
        b, result.complete_pixels, successful_stores,
        result.partial_pixel, result.extent_known, result.status);
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

static void native_trace_draw(OdysseyNativeMesa *native,
                              const SGIGLDraw *draw)
{
    const SGIGLDrawState *st = draw->state;
    const SGIRenderResource *resource;
    uint32_t draw_flags, state_flags;
    bool finite_matrices = true, finite_vertices;

    resource = st->drawable.front < ARRAY_SIZE(native->resources) ?
               &native->resources[st->drawable.front] : NULL;
    draw_flags = (!!draw->back_colors) |
                 (!!draw->polygon_modes << 1) |
                 (!!draw->polygon_stipple_enabled << 2) |
                 (!!draw->point_smooth << 3) |
                 (!!draw->line_smooth << 4) |
                 (!!draw->polygon_smooth << 5) |
                 (!!draw->line_stipple_enabled << 6);
    state_flags = (!!st->depth_test) |
                  (!!st->stencil_test << 1) |
                  (!!st->blend << 2) |
                  (!!st->alpha_test << 3) |
                  (!!st->fog << 4) |
                  (!!st->cull_face << 5) |
                  (!!st->clip_planes << 6) |
                  (!!st->color_logic_op << 7) |
                  (!!st->index_logic_op << 8) |
                  (!!st->texture_3d << 9) |
                  (!!st->polygon_offset << 10) |
                  (!!st->window_matrix_given << 11) |
                  (!!st->drawable.bound << 12);
    for (unsigned i = 0; i < 16; i++) {
        finite_matrices &= isfinite(st->modelview[i]) &&
                           isfinite(st->projection[i]);
    }
    finite_vertices = draw->vertices && draw->count >= 3;
    if (finite_vertices) {
        for (unsigned i = 0; i < 3; i++) {
            const float *vertex = draw->vertices + i * SGI_GL_VERTEX_FLOATS;

            for (unsigned c = 0; c < 4; c++) {
                finite_vertices &= isfinite(vertex[SGI_GL_V_POSITION + c]) &&
                                   isfinite(vertex[SGI_GL_V_COLOR + c]);
            }
        }
    }
    trace_sgi_odyssey_mesa_draw_input(
        draw->mode, draw->count, draw_flags, state_flags, st->shade_model,
        st->index_bits, st->draw_buffers, st->colour_mask,
        st->drawable.front, st->blend);
    trace_sgi_odyssey_mesa_draw_window(
        st->drawable.origin[0], st->drawable.origin[1],
        st->drawable.width, st->drawable.height,
        st->viewport[0], st->viewport[1], st->viewport[2], st->viewport[3],
        resource ? resource->width : 0, resource ? resource->height : 0);
    trace_sgi_odyssey_mesa_draw_resource(
        finite_matrices, finite_vertices, resource != NULL,
        resource ? resource->buffer : 0, resource ? resource->depth : 0,
        resource ? resource->component_count : 0);
}

/*
 * Odyssey's window rows run down the display: the scalar rasterizer stores
 * window pixel (x, y) at plane pixel (origin x + x, origin y + y), and its
 * 0x81d scissor box counts rows from the window's top. It applies its
 * bottom-origin top-left tie rule to those rows as they come, as if they
 * ran up. Mesa must see the same rows the same way round, or a pixel
 * centre on a horizontal edge shared by a Mesa draw and a scalar draw
 * goes to both triangles or to neither. So the adapter's views number GL
 * rows as the plane does, GL row g being plane row g, and call row 0 the
 * bottom one, which is what the scalar path's rule assumes; the display
 * being upside down in GL's terms changes no pixel. The mapping is then a
 * translation: a submission-local copy whose window is the whole resource,
 * with the viewport and scissor boxes moved by the drawable origin. Native
 * clips are plane rows already and pass through; the shared renderer
 * clamps them to the resource, as the scalar path drops a pixel outside
 * the plane. The guest's library folds the window height into its
 * combined matrix, so no drawable extent is needed.
 *
 * Only drawables without a window matrix or rows-down flag are mapped,
 * and only while every mapped value fits 32 bits; every other row
 * convention stays unsupported.
 */
static bool native_map_rows(const SGIGLDrawState *in, SGIGLDrawState *out)
{
    int64_t ox = in->drawable.origin[0];
    int64_t oy = in->drawable.origin[1];
    int64_t viewport_x, viewport_y, scissor_x, scissor_y;

    if (in->window_matrix_given || in->window_rows_down) {
        return false;
    }
    viewport_x = in->viewport[0] + ox;
    viewport_y = in->viewport[1] + oy;
    scissor_x = in->scissor[0] + ox;
    scissor_y = in->scissor[1] + oy;
    if (viewport_x < INT32_MIN || viewport_x > INT32_MAX ||
        viewport_y < INT32_MIN || viewport_y > INT32_MAX ||
        (in->scissor_test &&
         (scissor_x < INT32_MIN || scissor_x > INT32_MAX ||
          scissor_y < INT32_MIN || scissor_y > INT32_MAX))) {
        return false;
    }
    *out = *in;
    out->drawable.origin[0] = 0;
    out->drawable.origin[1] = 0;
    /* Checked by native_extent_valid(); not meaningful at the new origin. */
    out->drawable.width = 0;
    out->drawable.height = 0;
    out->viewport[0] = viewport_x;
    out->viewport[1] = viewport_y;
    if (in->scissor_test) {
        out->scissor[0] = scissor_x;
        out->scissor[1] = scissor_y;
    }
    return true;
}

/*
 * Whether one window coordinate snaps to the same subpixel with the
 * drawable origin folded in as without it. The scalar path snaps the
 * window coordinate and adds the origin after; Mesa sees the coordinate
 * with the origin already added. A whole-pixel shift keeps the fraction,
 * so only rounding half away from zero can differ: an exact half subpixel
 * whose sign the shift changes rounds the other way.
 */
static bool native_snaps_alike(double window, int64_t origin,
                               uint8_t subpixel_bits)
{
    double scale = (double)(1u << subpixel_bits);
    double s = window * scale;
    double shifted = s + (double)origin * scale;

    if (!isfinite(s) || s - floor(s) != 0.5) {
        return true;
    }
    return (s < 0) == (shifted < 0);
}

static bool native_vertex_snaps_alike(const SGIGLDrawState *st,
                                      const SGIVolumeVertex *v,
                                      uint8_t subpixel_bits)
{
    double win[3];

    if (!(v->clip[3] > 0)) {
        /* No window position; the scalar path drops it too. */
        return true;
    }
    sgi_gl_clip_to_window(st, v->clip, win);
    return native_snaps_alike(win[0], st->drawable.origin[0],
                              subpixel_bits) &&
           native_snaps_alike(win[1], st->drawable.origin[1],
                              subpixel_bits);
}

/* A triangle's vertices, and those view-volume clipping makes from it. */
static bool native_triangle_snaps_alike(const SGIGLDrawState *st,
                                        const float *a, const float *b,
                                        const float *c,
                                        uint8_t subpixel_bits)
{
    SGIVolumeVertex in[3], out[SGI_VOLUME_TRIANGLE_MAX];
    unsigned n;

    sgi_volume_from_draw_vertex(st, a, &in[0]);
    sgi_volume_from_draw_vertex(st, b, &in[1]);
    sgi_volume_from_draw_vertex(st, c, &in[2]);
    if (sgi_volume_contains(&in[0]) && sgi_volume_contains(&in[1]) &&
        sgi_volume_contains(&in[2])) {
        n = 3;
        memcpy(out, in, sizeof(in));
    } else {
        n = sgi_volume_clip_triangle(in, out);
    }
    for (unsigned i = 0; i < n; i++) {
        if (!native_vertex_snaps_alike(st, &out[i], subpixel_bits)) {
            return false;
        }
    }
    return true;
}

static bool native_segment_snaps_alike(const SGIGLDrawState *st,
                                       const float *a, const float *b,
                                       uint8_t subpixel_bits)
{
    SGIVolumeVertex va, vb;

    sgi_volume_from_draw_vertex(st, a, &va);
    sgi_volume_from_draw_vertex(st, b, &vb);
    if (!sgi_volume_clip_segment(&va, &vb)) {
        return true;
    }
    return native_vertex_snaps_alike(st, &va, subpixel_bits) &&
           native_vertex_snaps_alike(st, &vb, subpixel_bits);
}

/*
 * Whether every vertex the scalar path would snap for this draw, clipped
 * ones included, snaps alike once the origin moves into the viewport.
 * Polygons are checked as fans and quadrilaterals as triangle pairs, which
 * cover every edge clipping can cut, whichever way the scalar path splits
 * them.
 */
static bool native_draw_snaps_alike(const SGIRasterRules *rules,
                                    const SGIGLDraw *draw)
{
    const SGIGLDrawState *st = draw->state;
    const float *v = draw->vertices;
    uint32_t n = draw->count;
    uint8_t subpixel_bits = rules->subpixel_bits;

#define V(i) (v + (size_t)(i) * SGI_GL_VERTEX_FLOATS)
    /* Other roundings, and rules the renderer refuses, never differ here. */
    if ((!st->drawable.origin[0] && !st->drawable.origin[1]) || !v ||
        rules->rounding != SGI_ROUND_HALF_AWAY || subpixel_bits > 16) {
        return true;
    }
    switch (draw->mode) {
    case SGI_GL_POINTS:
        for (uint32_t i = 0; i < n; i++) {
            SGIVolumeVertex p;

            sgi_volume_from_draw_vertex(st, V(i), &p);
            if (sgi_volume_contains(&p) &&
                !native_vertex_snaps_alike(st, &p, subpixel_bits)) {
                return false;
            }
        }
        return true;
    case SGI_GL_LINES:
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            if (!native_segment_snaps_alike(st, V(i), V(i + 1),
                                            subpixel_bits)) {
                return false;
            }
        }
        return true;
    case SGI_GL_LINE_LOOP:
    case SGI_GL_LINE_STRIP:
        for (uint32_t i = 0; i + 1 < n; i++) {
            if (!native_segment_snaps_alike(st, V(i), V(i + 1),
                                            subpixel_bits)) {
                return false;
            }
        }
        return draw->mode != SGI_GL_LINE_LOOP || n < 2 ||
               native_segment_snaps_alike(st, V(n - 1), V(0),
                                          subpixel_bits);
    case SGI_GL_TRIANGLES:
        for (uint32_t i = 0; i + 2 < n; i += 3) {
            if (!native_triangle_snaps_alike(st, V(i), V(i + 1), V(i + 2),
                                             subpixel_bits)) {
                return false;
            }
        }
        return true;
    case SGI_GL_TRIANGLE_STRIP:
        for (uint32_t i = 0; i + 2 < n; i++) {
            if (!native_triangle_snaps_alike(st, V(i), V(i + 1), V(i + 2),
                                             subpixel_bits)) {
                return false;
            }
        }
        return true;
    case SGI_GL_QUADS:
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            if (!native_triangle_snaps_alike(st, V(i), V(i + 1), V(i + 2),
                                             subpixel_bits) ||
                !native_triangle_snaps_alike(st, V(i), V(i + 2), V(i + 3),
                                             subpixel_bits)) {
                return false;
            }
        }
        return true;
    case SGI_GL_QUAD_STRIP:
        for (uint32_t i = 0; i + 3 < n; i += 2) {
            if (!native_triangle_snaps_alike(st, V(i), V(i + 1), V(i + 3),
                                             subpixel_bits) ||
                !native_triangle_snaps_alike(st, V(i), V(i + 3), V(i + 2),
                                             subpixel_bits)) {
                return false;
            }
        }
        return true;
    default:
        /* Triangle fans and polygons; any other mode the renderer refuses. */
        for (uint32_t i = 1; i + 1 < n; i++) {
            if (!native_triangle_snaps_alike(st, V(0), V(i), V(i + 1),
                                             subpixel_bits)) {
                return false;
            }
        }
        return true;
    }
#undef V
}

/*
 * An unknown extent (both zero) bounds nothing: the resource does. A
 * half-specified one is invalid, and a known one must hold every
 * non-empty native clip, in plane pixels from the drawable origin.
 */
static bool native_extent_valid(const SGIGLDrawable *d)
{
    int64_t x1, y1;

    if (!d->width && !d->height) {
        return true;
    }
    if (!d->width || !d->height || d->clip.n > SGI_CLIP_MAX) {
        return false;
    }
    x1 = (int64_t)d->origin[0] + d->width - 1;
    y1 = (int64_t)d->origin[1] + d->height - 1;
    for (uint32_t i = 0; i < d->clip.n; i++) {
        const SGIClipRect *r = &d->clip.r[i];

        if (r->x0 > r->x1 || r->y0 > r->y1) {
            continue;
        }
        if (r->x0 < d->origin[0] || r->y0 < d->origin[1] ||
            r->x1 > x1 || r->y1 > y1) {
            return false;
        }
    }
    return true;
}

static SGIRenderDrawResult native_refuse(SGIRenderAttemptOutcome *outcome,
                                         SGIRenderRefusal reason)
{
    *outcome = (SGIRenderAttemptOutcome) {
        .status = SGI_RENDER_UNSUPPORTED,
        .reason = reason,
    };
    trace_sgi_odyssey_mesa_renderer_result(SGI_RENDER_UNSUPPORTED);
    return SGI_RENDER_DRAW_REFUSED;
}

SGIRenderDrawResult odyssey_native_mesa_draw(OdysseyNativeMesa *native,
                                             SGIRenderer *renderer,
                                             const SGIRasterRules *rules,
                                             const SGIGLDraw *draw,
                                             unsigned selected_buffers,
                                             SGIRenderEye eye,
                                             bool compare_active,
                                             SGIRenderAttemptOutcome *outcome,
                                             Error **errp)
{
    SGIRenderAttemptOptions opts = { .mixed_frame = true };
    SGIRasterTarget target;
    SGIGLDrawState state;
    SGIGLDraw mapped;
    SGIRenderDrawResult result;

    if (!native || !draw || !draw->state) {
        error_setg(errp, "Missing Odyssey native Mesa draw state");
        *outcome = (SGIRenderAttemptOutcome) {
            .status = SGI_RENDER_RESOURCE_ERROR,
        };
        return SGI_RENDER_DRAW_FAILED;
    }
    if (selected_buffers != 1 || eye != SGI_RENDER_MONO) {
        error_setg(errp, "Unsupported Odyssey native Mesa buffers");
        return native_refuse(outcome, SGI_RENDER_REFUSAL_BUFFERS);
    }
    if (compare_active) {
        /* A drawable-id compare owns which window pixels may be written. */
        error_setg(errp, "Unsupported Odyssey native Mesa window compare");
        return native_refuse(outcome, SGI_RENDER_REFUSAL_WINDOW);
    }
    native->clip = draw->state->drawable.clip;
    target = odyssey_native_mesa_target(native);
    if (trace_event_get_state_backends(TRACE_SGI_ODYSSEY_MESA_DRAW_INPUT) ||
        trace_event_get_state_backends(TRACE_SGI_ODYSSEY_MESA_DRAW_WINDOW) ||
        trace_event_get_state_backends(TRACE_SGI_ODYSSEY_MESA_DRAW_RESOURCE)) {
        native_trace_draw(native, draw);
    }
    if (!native_extent_valid(&draw->state->drawable)) {
        error_setg(errp, "Unsupported Odyssey native Mesa drawable extent");
        return native_refuse(outcome, SGI_RENDER_REFUSAL_WINDOW);
    }
    if (!native_map_rows(draw->state, &state)) {
        error_setg(errp, "Unsupported Odyssey native Mesa row convention");
        return native_refuse(outcome, SGI_RENDER_REFUSAL_WINDOW);
    }
    if (!native_draw_snaps_alike(rules, draw)) {
        error_setg(errp, "Odyssey native Mesa vertex snaps differently "
                   "at the drawable origin");
        return native_refuse(outcome, SGI_RENDER_REFUSAL_WINDOW);
    }
    mapped = *draw;
    mapped.state = &state;
    opts.check_staged = native->check_staged;
    opts.opaque = native->check_staged_opaque;
    result = sgi_render_draw_attempt(renderer, rules, &target, &mapped,
                                     &opts, outcome, errp);
    trace_sgi_odyssey_mesa_renderer_result(outcome->status);
    return result;
}

static SGIGLDrawResult native_failure(SGIRenderResult status)
{
    switch (status) {
    case SGI_RENDER_OOM:
        return SGI_GL_DRAW_OUT_OF_MEMORY;
    case SGI_RENDER_RESOURCE_ERROR:
        return SGI_GL_DRAW_RESOURCE_ERROR;
    default:
        return SGI_GL_DRAW_HOST_ERROR;
    }
}

SGIGLDrawResult odyssey_native_mesa_draw_or_scalar(
    OdysseyNativeMesa *native, SGIRenderer *renderer,
    const SGIRasterRules *rules, const SGIGLDraw *draw,
    unsigned selected_buffers, SGIRenderEye eye, bool compare_active,
    SGIRenderRefusal refusal, OdysseyScalarDraw scalar, void *scalar_opaque,
    OdysseyMesaCounts *counts)
{
    SGIRenderAttemptOutcome outcome = {
        .status = SGI_RENDER_UNSUPPORTED,
        .reason = refusal,
    };
    SGIRenderDrawResult result = SGI_RENDER_DRAW_REFUSED;
    Error *err = NULL;

    if (refusal == SGI_RENDER_REFUSAL_NONE) {
        result = odyssey_native_mesa_draw(native, renderer, rules, draw,
                                          selected_buffers, eye,
                                          compare_active, &outcome, &err);
    }
    switch (result) {
    case SGI_RENDER_DRAW_OK:
        counts->committed++;
        return SGI_GL_DRAW_OK;
    case SGI_RENDER_DRAW_REFUSED:
        /*
         * Nothing is visible yet, so the scalar draw is the whole draw. Its
         * own result goes back to the shared context, which never sees the
         * refusal. The counter and trace stand for the refusal's message,
         * which would otherwise repeat on every such draw.
         */
        error_free(err);
        counts->fallbacks[outcome.reason]++;
        trace_sgi_odyssey_mesa_fallback(sgi_render_refusal_name(outcome.reason),
                                        draw->mode, draw->count);
        return scalar(scalar_opaque, draw);
    default:
        if (err) {
            error_report_err(err);
        }
        return native_failure(outcome.status);
    }
}
