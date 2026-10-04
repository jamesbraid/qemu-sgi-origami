/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "hw/display/sgi/rasterizer.h"
#include "ui/sgi-renderer.h"

/* Temporary host staging bound for two CPU copies and a GL_RGBA32UI image. */
#define SGI_RENDER_HOST_TEXTURE_STAGING_BYTES_MAX (16u * 1024u * 1024u)

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

static const SGIRenderResource *sgi_render_scalar_resource(
    const SGIRasterTarget *target, SGIRenderRole role)
{
    const SGIRenderResource *found = NULL;

    if (!target->resources) {
        return NULL;
    }
    for (size_t i = 0; i < target->resource_count; i++) {
        if (target->resources[i].role == role) {
            if (found) {
                return NULL;
            }
            found = &target->resources[i];
        }
    }
    return found;
}

static const SGIRenderResource *sgi_render_texture_resource(
    const SGIRasterTarget *target, const SGIGLDrawState *st)
{
    const SGIGLTexImage *image = target->texture;
    const SGIRenderResource *res = NULL;

    if (!image || image->volume || image->pyramid || image->border ||
        !is_power_of_2(image->width) ||
        !is_power_of_2(image->height) ||
        image->base_format != SGI_GL_RGBA ||
        image->min_filter != image->mag_filter ||
        (image->min_filter != SGI_GL_NEAREST &&
         image->min_filter != SGI_GL_LINEAR) ||
        (image->wrap_s != SGI_GL_REPEAT &&
         image->wrap_s != SGI_GL_CLAMP &&
         image->wrap_s != SGI_GL_CLAMP_TO_EDGE &&
         image->wrap_s != SGI_GL_CLAMP_TO_BORDER) ||
        (image->wrap_t != SGI_GL_REPEAT &&
         image->wrap_t != SGI_GL_CLAMP &&
         image->wrap_t != SGI_GL_CLAMP_TO_EDGE &&
         image->wrap_t != SGI_GL_CLAMP_TO_BORDER) ||
        image->post_filter || image->detail_image ||
        image->read_filter_weight || image->lod_scale_s ||
        image->lod_scale_t ||
        (st->texture_env_mode != SGI_GL_REPLACE &&
         st->texture_env_mode != SGI_GL_MODULATE &&
         st->texture_env_mode != SGI_GL_DECAL &&
         st->texture_env_mode != SGI_GL_BLEND)) {
        return NULL;
    }
    for (unsigned c = 0; c < 4; c++) {
        if (!isfinite(image->border_color[c]) ||
            !isfinite(st->texture_env_color[c])) {
            return NULL;
        }
    }
    for (size_t i = 0; i < target->resource_count; i++) {
        const SGIRenderResource *candidate = &target->resources[i];

        if (candidate->role == SGI_RENDER_TEXTURE) {
            if (res) {
                return NULL;
            }
            res = candidate;
        }
    }
    if (!res || !res->width || !res->height || res->width > INT_MAX ||
        res->height > INT_MAX || res->width != image->width ||
        res->height != image->height || res->depth != 1 ||
        res->texture_level || res->border[0] || res->border[1] ||
        res->border[2] || !res->bottom_origin ||
        res->buffer != SGI_RENDER_BUFFER_NONE ||
        res->component_count != 4) {
        return NULL;
    }
    for (unsigned c = 0; c < 4; c++) {
        if (res->component[c].channel != SGI_RENDER_R + c ||
            res->component[c].numeric != SGI_RENDER_UNORM ||
            res->component[c].storage_bits != 8) {
            return NULL;
        }
    }
    return res;
}

static SGIRenderResult sgi_render_texture_span(const SGIRenderResource *res,
                                               size_t *stride_slots,
                                               size_t *capacity_slots,
                                               Error **errp)
{
    size_t width = res->width;
    size_t height = res->height;

    if (width > SIZE_MAX / 4) {
        error_setg(errp, "SGI Mesa texture stride exceeds host address space");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    *stride_slots = width * 4;
    if (height > SIZE_MAX / *stride_slots) {
        error_setg(errp, "SGI Mesa texture span exceeds host address space");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    *capacity_slots = *stride_slots * height;
    if (*capacity_slots > SIZE_MAX / sizeof(uint32_t)) {
        error_setg(errp, "SGI Mesa texture bytes exceed host address space");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    if (*capacity_slots > SGI_RENDER_HOST_TEXTURE_STAGING_BYTES_MAX /
                          sizeof(uint32_t)) {
        error_setg(errp, "SGI Mesa texture exceeds the temporary 16 MiB "
                   "host staging budget");
        return SGI_RENDER_UNSUPPORTED;
    }
    return SGI_RENDER_OK;
}

/*
 * Every OpenGL 1.2 factor except the constant colour ones, whose half-step
 * results llvmpipe's float32 blend rounds the other way from the scalar
 * rasterizer. Source alpha saturate is a source factor only.
 */
static bool sgi_render_blend_factor_supported(uint32_t factor, bool source)
{
    switch (factor) {
    case 0:
    case 1:
    case SGI_GL_SRC_COLOR:
    case SGI_GL_ONE_MINUS_SRC_COLOR:
    case SGI_GL_DST_COLOR:
    case SGI_GL_ONE_MINUS_DST_COLOR:
    case SGI_GL_SRC_ALPHA:
    case SGI_GL_ONE_MINUS_SRC_ALPHA:
    case SGI_GL_DST_ALPHA:
    case SGI_GL_ONE_MINUS_DST_ALPHA:
        return true;
    case SGI_GL_SRC_ALPHA_SATURATE:
        return source;
    default:
        return false;
    }
}

/*
 * The worker rounds the source colour to the rules' colour steps and its
 * alpha to the scalar rasterizer's incoming alpha steps before blending,
 * so the native components must hold exactly those steps. Blending is
 * limited to 8-bit components: the destination crosses the worker as
 * 16-bit UNORM, which holds a b-bit value exactly only when 2^b - 1
 * divides 65535, and only 8 bits is tested.
 */
static bool sgi_render_blend_supported(const SGIRasterRules *rules,
                                       const SGIGLDrawState *st,
                                       const SGIRenderResource *res)
{
    if (rules->color_bits != 8 ||
        (st->blend_equation != SGI_GL_FUNC_ADD &&
         st->blend_equation != SGI_GL_FUNC_SUBTRACT &&
         st->blend_equation != SGI_GL_FUNC_REVERSE_SUBTRACT &&
         st->blend_equation != SGI_GL_MIN &&
         st->blend_equation != SGI_GL_MAX) ||
        !sgi_render_blend_factor_supported(st->blend_src, true) ||
        !sgi_render_blend_factor_supported(st->blend_dst, false)) {
        return false;
    }
    for (unsigned c = 0; c < 3; c++) {
        if (res->component[c].storage_bits != rules->color_bits) {
            return false;
        }
    }
    if (res->component_count == 3) {
        return !rules->alpha_bits;
    }
    /* Stored alpha has the scalar path's incoming alpha precision. */
    return rules->alpha_bits == 8 && res->component[3].storage_bits == 8;
}

/*
 * The storage widths whose stores are proven equal to the scalar path's:
 * colour components of the rules' colour bits and, for RGBA, stored alpha
 * of the rules' alpha bits, equal to the scalar path's incoming alpha
 * precision MAX(2, colour bits, alpha bits) so its truncation to the store
 * changes nothing. An RGB store has no alpha, and the scalar path's colour
 * rounding does not depend on it. Under rules without alpha the scalar path
 * hands an RGBA store canonical zero alpha,
 * which the family may or may not store, so that pairing stays unproven.
 */
static bool sgi_render_widths_proven(const SGIRasterRules *rules,
                                     const SGIRenderResource *res)
{
    for (unsigned c = 0; c < 3; c++) {
        if (res->component[c].storage_bits != rules->color_bits) {
            return false;
        }
    }
    if (res->component_count == 3) {
        return true;
    }
    return res->component[3].storage_bits == rules->alpha_bits &&
           rules->alpha_bits == MAX(2, MAX(rules->color_bits,
                                           rules->alpha_bits));
}

static bool sgi_render_mode_polygons(uint32_t mode)
{
    return mode >= SGI_GL_TRIANGLES && mode <= SGI_GL_POLYGON;
}

static bool sgi_render_mode_lines(uint32_t mode)
{
    return mode >= SGI_GL_LINES && mode <= SGI_GL_LINE_STRIP;
}

/*
 * Points, lines and polygon modes. Smoothing is refused only where the
 * scalar path smooths: points, lines and polygons whose enable is set
 * under rules that smooth. Under rules that do not, sgi_soft_draw()
 * ignores the enables and draws aliased, and so does this path. Each
 * enable and the polygon modes apply only to their own class of primitive
 * (OpenGL 1.2.1, 3.3 to 3.5).
 */
static bool sgi_render_draw_supported(const SGIRasterRules *rules,
                                      const SGIRasterTarget *target,
                                      const SGIGLDraw *draw,
                                      const SGIRenderResource **result)
{
    const SGIGLDrawState *st = draw->state;
    const SGIRenderResource *res;

    if (draw->mode > SGI_GL_POLYGON ||
        (draw->mode == SGI_GL_POINTS &&
         (!isfinite(draw->point_size) || draw->point_size < 0)) ||
        (sgi_render_mode_lines(draw->mode) &&
         (!isfinite(draw->line_width) || draw->line_width < 0 ||
          (rules->smooth_primitives && draw->line_smooth))) ||
        (st->texture_2d &&
         (draw->mode != SGI_GL_TRIANGLES || draw->count != 3)) ||
        draw->back_colors ||
        (sgi_render_mode_polygons(draw->mode) && draw->polygon_modes &&
         (draw->polygon_modes[0] != SGI_GL_FILL ||
          draw->polygon_modes[1] != SGI_GL_FILL)) ||
        draw->polygon_stipple_enabled ||
        (!sgi_render_mode_lines(draw->mode) && draw->line_stipple_enabled) ||
        (draw->mode == SGI_GL_POINTS && rules->smooth_primitives &&
         draw->point_smooth) ||
        (sgi_render_mode_polygons(draw->mode) && rules->smooth_primitives &&
         draw->polygon_smooth) ||
        (st->shade_model != SGI_GL_SMOOTH &&
         st->shade_model != SGI_GL_FLAT) || st->index_bits ||
        st->draw_buffers != SGI_GL_DRAW_FRONT ||
        st->colour_mask != SGI_GL_COLOUR_MASK_ALL ||
        st->depth_test || st->stencil_test || st->blend > 1 ||
        st->alpha_test || st->fog || st->cull_face || st->clip_planes ||
        st->color_logic_op ||
        st->index_logic_op || st->texture_3d ||
        st->polygon_offset || st->window_matrix_given ||
        !st->drawable.bound ||
        st->drawable.origin[0] || st->drawable.origin[1] ||
        st->viewport[2] < 0 || st->viewport[3] < 0) {
        return false;
    }
    for (unsigned i = 0; i < 16; i++) {
        if (!isfinite(st->modelview[i]) || !isfinite(st->projection[i])) {
            return false;
        }
    }
    for (uint32_t i = 0; i < draw->count; i++) {
        const float *vertex = draw->vertices +
                              (size_t)i * SGI_GL_VERTEX_FLOATS;

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
        res->buffer != SGI_RENDER_FRONT ||
        (res->component_count != 3 && res->component_count != 4)) {
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
    if (!sgi_render_widths_proven(rules, res)) {
        return false;
    }
    if (st->blend && !sgi_render_blend_supported(rules, st, res)) {
        return false;
    }
    if (st->texture_2d) {
        if (st->blend) {
            return false;
        }
        for (unsigned c = 0; c < res->component_count; c++) {
            if (res->component[c].storage_bits != 8) {
                return false;
            }
        }
    }
    *result = res;
    return true;
}

/*
 * The scalar comparisons that establish this path use 8 subpixel bits,
 * rounding half away from zero, the bottom-origin top-left tie rule and
 * double arithmetic, as every family does; the worker cannot follow other
 * rules.
 */
static bool sgi_render_rules_supported(const SGIRasterRules *rules)
{
    return rules->subpixel_bits == 8 &&
           rules->rounding == SGI_ROUND_HALF_AWAY &&
           rules->tie_rule == SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN &&
           rules->arith == SGI_ARITH_DOUBLE;
}

/* The scalar rasterizer's aliased size: nearest integer, at least one. */
static float sgi_render_aliased_size(float requested)
{
    return requested == 0 ? 1 : MAX(floor((double)requested + 0.5), 1);
}

/* Independent vertices the mode decomposes into. */
static uint64_t sgi_render_piece_vertices(uint32_t mode, uint32_t n)
{
    switch (mode) {
    case SGI_GL_POINTS:
        return n;
    case SGI_GL_TRIANGLES:
        return n / 3 * 3;
    case SGI_GL_TRIANGLE_STRIP:
    case SGI_GL_TRIANGLE_FAN:
    case SGI_GL_POLYGON:
        return n >= 3 ? (uint64_t)(n - 2) * 3 : 0;
    case SGI_GL_QUADS:
        return n / 4 * 6;
    case SGI_GL_QUAD_STRIP:
        return n >= 4 ? (uint64_t)(n - 2) / 2 * 6 : 0;
    default:
        return 0;
    }
}

typedef struct SGIRenderPieces {
    float (*v)[SGI_RENDERER_FRAME_VERTEX_FLOATS];
    uint32_t count;
    uint32_t primitive;
    bool flat;
    const uint32_t *steps; /* The frame's colour steps, red to alpha. */
    /*
     * With window set, triangles become the scalar path's own clipped
     * triangles in window coordinates instead of vertices.
     */
    SGIRendererTriangle *window;
    uint32_t window_count, window_capacity;
    const SGIRasterRules *rules;
} SGIRenderPieces;

/*
 * One piece's vertices; flat shading gives each the provoking colour, so
 * the worker's last-vertex convention needs no reordering.
 */
static void sgi_render_piece(SGIRenderPieces *p, const SGIGLDraw *draw,
                             const uint32_t *index, unsigned n,
                             uint32_t provoking)
{
    const float *colour = draw->vertices +
                          (size_t)provoking * SGI_GL_VERTEX_FLOATS +
                          SGI_GL_V_COLOR;
    float rounded[4];

    if (p->window) {
        SGIRasterTriangle t[SGI_VOLUME_TRIANGLE_MAX - 2];
        unsigned count = sgi_raster_triangle_pieces(p->rules, draw->state,
            draw->vertices + (size_t)index[0] * SGI_GL_VERTEX_FLOATS,
            draw->vertices + (size_t)index[1] * SGI_GL_VERTEX_FLOATS,
            draw->vertices + (size_t)index[2] * SGI_GL_VERTEX_FLOATS, t);

        for (unsigned i = 0; i < count; i++) {
            SGIRendererTriangle *out;

            if (p->window_count == p->window_capacity) {
                p->window_count++;
                return;
            }
            out = &p->window[p->window_count++];
            for (unsigned v = 0; v < 3; v++) {
                out->x[v] = t[i].x[v];
                out->y[v] = t[i].y[v];
                out->inv_w[v] = t[i].inv_w[v];
                for (unsigned c = 0; c < 4; c++) {
                    out->colour[v][c] = t[i].rgba[v][c];
                }
            }
        }
        return;
    }
    /*
     * A constant colour is rounded here in double, as the scalar path
     * rounds it, so a product exactly on a half step in float32, such as
     * 0.9 * 255, reaches the worker already on a step.
     */
    if (p->flat || n == 1) {
        for (unsigned c = 0; c < 4; c++) {
            uint32_t s = p->steps[c];
            double v = colour[c];

            rounded[c] = !s ? colour[c] :
                         !(v > 0) ? 0 :
                         v >= 1 ? 1 :
                         (float)((uint32_t)(v * s + 0.5) / (double)s);
        }
    }
    for (unsigned i = 0; i < n; i++) {
        const float *src = draw->vertices +
                           (size_t)index[i] * SGI_GL_VERTEX_FLOATS;
        float *dst = p->v[p->count++];

        memcpy(dst, &src[SGI_GL_V_POSITION], 4 * sizeof(float));
        memcpy(dst + 4, p->flat || n == 1 ? rounded : &src[SGI_GL_V_COLOR],
               4 * sizeof(float));
        memset(dst + 8, 0, 4 * sizeof(float));
    }
}

/*
 * The pieces sgi_soft_draw() draws, in its order and with its provoking
 * vertices: odd strip triangles with their first two vertices swapped, and
 * quadrilaterals and polygons as fans from their first vertex.
 */
static void sgi_render_decompose(SGIRenderPieces *p, const SGIGLDraw *draw)
{
    uint32_t n = draw->count;

#define PIECE(provoking, ...) do { \
        const uint32_t index_[] = { __VA_ARGS__ }; \
        sgi_render_piece(p, draw, index_, ARRAY_SIZE(index_), provoking); \
    } while (0)
    p->primitive = draw->mode == SGI_GL_POINTS ? SGI_RENDERER_POINTS :
                                                 SGI_RENDERER_TRIANGLES;
    switch (draw->mode) {
    case SGI_GL_POINTS:
        for (uint32_t i = 0; i < n; i++) {
            PIECE(i, i);
        }
        break;
    case SGI_GL_TRIANGLES:
        for (uint32_t i = 0; i + 2 < n; i += 3) {
            PIECE(i + 2, i, i + 1, i + 2);
        }
        break;
    case SGI_GL_TRIANGLE_STRIP:
        for (uint32_t i = 0; i + 2 < n; i++) {
            if (i & 1) {
                PIECE(i + 2, i + 1, i, i + 2);
            } else {
                PIECE(i + 2, i, i + 1, i + 2);
            }
        }
        break;
    case SGI_GL_TRIANGLE_FAN:
    case SGI_GL_POLYGON:
        for (uint32_t i = 0; i + 2 < n; i++) {
            PIECE(draw->mode == SGI_GL_POLYGON ? 0 : i + 2, 0, i + 1, i + 2);
        }
        break;
    case SGI_GL_QUADS:
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            PIECE(i + 3, i, i + 1, i + 2);
            PIECE(i + 3, i, i + 2, i + 3);
        }
        break;
    case SGI_GL_QUAD_STRIP:
        for (uint32_t i = 0; i + 3 < n; i += 2) {
            PIECE(i + 3, i, i + 1, i + 3);
            PIECE(i + 3, i, i + 3, i + 2);
        }
        break;
    }
#undef PIECE
}

static int sgi_render_test_band;

void sgi_render_test_line_band(int subpixels)
{
    sgi_render_test_band = subpixels;
}

static int64_t sgi_render_gcd(int64_t a, int64_t b)
{
    while (b) {
        int64_t r = a % b;

        a = b;
        b = r;
    }
    return a;
}

/* Subpixels per pixel under the 8 subpixel bits the rules check requires. */
#define SGI_RENDER_LINE_SUB 256

/*
 * One segment's frame record. The parallelogram covers the band
 * [line - N/2, line + N/2] in the minor axis around the unshifted segment,
 * which under llvmpipe's triangle rules selects exactly the replicated rows
 * of the scalar path's shifted width-one line; its ends extend past the
 * base range by whole multiples of the segment's smallest step on the
 * subpixel grid, d / gcd(dx, dy), so every vertex stays exactly on the
 * line, and the fragment program keeps only steps lo to hi. False
 * when a vertex falls outside the line viewport, where GL would clip and
 * create new vertices.
 */
static bool sgi_render_line_record(const SGIRasterLineSpan *s,
                                   uint32_t width, const float *flat,
                                   uint32_t counter, SGIRendererLine *l)
{
    const int64_t reach = (int64_t)SGI_RENDERER_LINE_REACH *
                          SGI_RENDER_LINE_SUB;
    int64_t d[2] = { s->bx - s->ax, s->by - s->ay };
    int64_t major = s->x_major ? 0 : 1;
    int64_t g = sgi_render_gcd(llabs(d[0]), llabs(d[1]));
    int64_t dm;
    int64_t amaj = major ? s->ay : s->ax, bmaj = major ? s->by : s->bx;
    int64_t need, k, h = (int64_t)width * (SGI_RENDER_LINE_SUB / 2);
    int64_t ends[2][2], quad[4][2];

    if (!g || width > SGI_RENDERER_LINE_REACH) {
        return false;
    }
    d[0] /= g;
    d[1] /= g;
    dm = llabs(d[major]);
    need = MAX(MAX(MIN(amaj, bmaj) - s->lo * SGI_RENDER_LINE_SUB,
                   (s->hi + 1) * SGI_RENDER_LINE_SUB - MAX(amaj, bmaj)), 0) +
           SGI_RENDER_LINE_SUB / 4;
    k = (need + dm - 1) / dm;
    ends[0][0] = s->ax - k * d[0];
    ends[0][1] = s->ay - k * d[1];
    ends[1][0] = s->bx + k * d[0];
    ends[1][1] = s->by + k * d[1];
    for (unsigned v = 0; v < 4; v++) {
        const int64_t *e = ends[v == 1 || v == 2];
        int64_t offset = (v < 2 ? -h : h) + sgi_render_test_band;

        quad[v][0] = e[0] + (major ? offset : 0);
        quad[v][1] = e[1] + (major ? 0 : offset);
        for (unsigned c = 0; c < 2; c++) {
            if (!(quad[v][c] > -reach && quad[v][c] < 3 * reach)) {
                return false;
            }
            l->quad[v][c] = (float)quad[v][c] / SGI_RENDER_LINE_SUB;
        }
    }
    l->a[0] = s->ax;
    l->a[1] = s->ay;
    l->b[0] = s->bx;
    l->b[1] = s->by;
    l->inv_w[0] = s->a_inv_w;
    l->inv_w[1] = s->b_inv_w;
    for (unsigned c = 0; c < 4; c++) {
        l->colour[0][c] = flat ? flat[c] : s->a_rgba[c];
        l->colour[1][c] = flat ? flat[c] : s->b_rgba[c];
    }
    l->lo = s->lo;
    l->hi = s->hi;
    l->x_major = s->x_major;
    l->reverse = bmaj < amaj;
    l->counter = counter;
    return true;
}

/* A colour rounded to the steps in double, as the scalar path rounds it. */
static void sgi_render_round_colour(const float *colour,
                                    const uint32_t *steps, float *out)
{
    for (unsigned c = 0; c < 4; c++) {
        uint32_t s = steps[c];
        double v = colour[c];

        out[c] = !s ? colour[c] : !(v > 0) ? 0 : v >= 1 ? 1 :
                 (float)((uint32_t)(v * s + 0.5) / (double)s);
    }
}

/* Separate lines pair vertices; a strip joins them and a loop closes. */
static uint32_t sgi_render_line_segments(const SGIGLDraw *draw)
{
    if (draw->mode == SGI_GL_LINES) {
        return draw->count / 2;
    }
    if (draw->count < 2) {
        return 0;
    }
    return draw->count - 1 + (draw->mode == SGI_GL_LINE_LOOP);
}

typedef struct SGIRenderLines {
    SGIRendererLine *lines;
    uint32_t count, segments;
    uint32_t counter;           /* the stipple counter after the draw */
    bool consumed;              /* some base pixel was counted */
} SGIRenderLines;

/*
 * The segments sgi_soft_draw() draws, with its provoking vertices and its
 * stipple counter: separate lines restart it unless a phase is borrowed,
 * strips and loops carry it. False when a segment cannot be drawn exactly.
 */
static bool sgi_render_lines(const SGIRasterRules *rules,
                             const SGIGLDraw *draw, const uint32_t *steps,
                             SGIRenderLines *out)
{
    uint32_t n = draw->count, repeat, modulus, width;
    bool flat = draw->state->shade_model == SGI_GL_FLAT;
    double size = draw->line_width == 0 ? 1 :
                  floor((double)draw->line_width + 0.5);

    if (!(size <= SGI_RENDERER_LINE_REACH)) {
        return false;
    }
    width = MAX(size, 1);
    repeat = MAX(1, MIN(draw->line_stipple_repeat, 256));
    modulus = 16 * repeat;
    out->counter = (draw->line_phase && !draw->line_phase_reset ?
                    *draw->line_phase : 0) % modulus;
    out->count = out->segments = 0;
    out->consumed = false;
    for (uint32_t s = 0; s < sgi_render_line_segments(draw); s++) {
        /* Every segment's provoking vertex is its second. */
        uint32_t i = draw->mode == SGI_GL_LINES ? 2 * s : s;
        uint32_t j = draw->mode == SGI_GL_LINES ? i + 1 : (i + 1) % n;
        const float *va = draw->vertices + (size_t)i * SGI_GL_VERTEX_FLOATS;
        const float *vb = draw->vertices + (size_t)j * SGI_GL_VERTEX_FLOATS;
        float colour[4];
        SGIRasterLineSpan span;

        out->segments++;
        if (draw->mode == SGI_GL_LINES && !draw->line_phase) {
            out->counter = 0;
        }
        if (!sgi_raster_line_span(rules, draw->state, va, vb, width,
                                  &span) || !span.covered) {
            continue;
        }
        if (!span.exact || out->count >= SGI_RENDERER_LINE_SEGMENTS_MAX) {
            return false;
        }
        sgi_render_round_colour(vb + SGI_GL_V_COLOR, steps, colour);
        if (!sgi_render_line_record(&span, width, flat ? colour : NULL,
                                    out->counter, &out->lines[out->count])) {
            return false;
        }
        out->count++;
        out->counter = (out->counter + (uint32_t)(span.hi - span.lo + 1) %
                        modulus) % modulus;
        out->consumed = true;
    }
    return true;
}

/*
 * Before lines are drawn on a renderer, draw tie vectors through both
 * paths and compare: endpoints on diamond edges and corners, rows and
 * columns through pixel centres, slopes of one through corners and even
 * widths, in both directions. Lines stay unsupported on a renderer whose
 * host rasterizes any of them differently.
 */
#define SGI_RENDER_PROBE_SIZE 24

typedef struct SGIRenderProbe {
    uint8_t written[SGI_RENDER_PROBE_SIZE][SGI_RENDER_PROBE_SIZE];
    uint32_t rgb[SGI_RENDER_PROBE_SIZE][SGI_RENDER_PROBE_SIZE][3];
} SGIRenderProbe;

static void sgi_render_probe_store(void *opaque, SGIColourTarget target,
                                   int x, int y,
                                   const SGIRasterColour *colour)
{
    SGIRenderProbe *p = opaque;

    if (x >= 0 && x < SGI_RENDER_PROBE_SIZE && y >= 0 &&
        y < SGI_RENDER_PROBE_SIZE) {
        p->written[y][x] = 1;
        memcpy(p->rgb[y][x], colour->component, sizeof(p->rgb[y][x]));
    }
}

static bool sgi_render_probe_load(void *opaque, SGIColourTarget target,
                                  int x, int y, SGIRasterColour *colour)
{
    *colour = (SGIRasterColour) { { 0 } };
    return true;
}

static bool sgi_render_line_probe(SGIRenderer *renderer)
{
    /* The rules sgi_render_rules_supported() accepts, complete. */
    static const SGIRasterRules probe_rules = {
        .subpixel_bits = 8, .color_bits = 8, .depth_bits = 24,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .line_rule = SGI_LINE_COVERAGE_DOUBLE,
        .evidence = "shared line probe",
    };
    const SGIRasterRules *rules = &probe_rules;
    static const float probes[][5] = {
        { 2.75f, 14.25f, 7.5f, 11, 1 },
        { 7.5f, 11, 2.75f, 14.25f, 1 },
        { 3.25f, 2.75f, 9.5f, 4.5f, 1 },
        { 2.5f, 2, 8.5f, 8, 1 },
        { 1.5f, 5.5f, 12.5f, 5.5f, 1 },
        { 1.5f, 5.5f, 12.5f, 5.5f, 2 },
        { 12.5f, 6.5f, 1.5f, 6.5f, 3 },
        { 6.5f, 1.5f, 6.5f, 12.5f, 2 },
        { 7.5f, 20.5f, 7.5f, 2.5f, 4 },
        { 2, 2, 10, 10, 1 },
        { 12, 2, 4, 10, 2 },
        { 2.25f, 7, 14, 9.5f, 2 },
        { 14, 9.5f, 2.25f, 7, 4 },
        { 3, 3, 8, 8, 3 },
        { 14, 4, 9, 9, 2 },
        { 10.25f, 14.75f, 12.5f, 6.25f, 2 },
        { 1.5f, 3, 1.5f, 8.5f, 2 },
        { 8, 1, 14, 3.25f, 4 },
    };
    static const uint32_t steps[4] = { 255, 255, 255, 255 };
    static const float colours[2][4] = {
        { 1, 0.25f, 0.5f, 1 }, { 0.1f, 0.9f, 0.3f, 1 },
    };
    SGIGLDrawState st = {
        .shade_model = SGI_GL_SMOOTH,
        .draw_buffers = SGI_GL_DRAW_FRONT,
        .colour_mask = SGI_GL_COLOUR_MASK_ALL,
        .drawable = {
            .bound = true,
            .clip = { .n = 1, .r[0] = { 0, 0, SGI_RENDER_PROBE_SIZE - 1,
                                        SGI_RENDER_PROBE_SIZE - 1 } },
        },
        .viewport = { 0, 0, SGI_RENDER_PROBE_SIZE, SGI_RENDER_PROBE_SIZE },
        .depth_range = { 0, 1 },
    };
    g_autofree SGIRenderProbe *scalar = g_new0(SGIRenderProbe, 1);
    g_autofree uint16_t *rgba = g_new0(uint16_t, SGI_RENDER_PROBE_SIZE *
                                                 SGI_RENDER_PROBE_SIZE * 4);
    g_autofree uint8_t *written = g_new0(uint8_t, SGI_RENDER_PROBE_SIZE *
                                                  SGI_RENDER_PROBE_SIZE);
    SGIRasterTarget target = {
        .opaque = scalar,
        .store_rgba = sgi_render_probe_store,
        .load_rgba = sgi_render_probe_load,
    };

    for (unsigned i = 0; i < 16; i += 5) {
        st.modelview[i] = 1;
        st.projection[i] = 1;
    }
    for (size_t i = 0; i < ARRAY_SIZE(probes); i++) {
        float v[2][SGI_GL_VERTEX_FLOATS] = { { 0 } };
        SGIGLDraw draw = {
            .mode = SGI_GL_LINES, .count = 2, .vertices = &v[0][0],
            .state = &st, .line_width = probes[i][4],
        };
        SGIRendererLine line;
        SGIRenderLines lines = { .lines = &line };
        SGIRendererFrame frame = {
            .width = SGI_RENDER_PROBE_SIZE, .height = SGI_RENDER_PROBE_SIZE,
            .primitive = SGI_RENDERER_LINES, .lines = &line,
            .shade_model = SGI_GL_SMOOTH, .rgba = rgba, .written = written,
            .written_capacity = SGI_RENDER_PROBE_SIZE * SGI_RENDER_PROBE_SIZE,
        };
        Error *err = NULL;

        for (unsigned e = 0; e < 2; e++) {
            v[e][SGI_GL_V_POSITION] = probes[i][e * 2] * 2 /
                                      SGI_RENDER_PROBE_SIZE - 1;
            v[e][SGI_GL_V_POSITION + 1] = probes[i][e * 2 + 1] * 2 /
                                          SGI_RENDER_PROBE_SIZE - 1;
            v[e][SGI_GL_V_POSITION + 3] = 1;
            for (unsigned c = 0; c < 4; c++) {
                v[e][SGI_GL_V_COLOR + c] = colours[e][c];
                /* Clip w of 2 at every other end weights the colour. */
                if (e && (i & 1)) {
                    v[e][SGI_GL_V_POSITION + c] *= 2;
                }
            }
        }
        memcpy(frame.colour_steps, steps, sizeof(steps));
        memset(scalar, 0, sizeof(*scalar));
        sgi_soft_draw(rules, &target, &draw);
        if (!sgi_render_lines(rules, &draw, steps, &lines) ||
            lines.count != 1) {
            return false;
        }
        frame.count = lines.count;
        if (!sgi_renderer_draw_frame(renderer, &frame, &err)) {
            error_free(err);
            return false;
        }
        /* Coverage, and each covered pixel's colour, must match. */
        if (memcmp(written, scalar->written, sizeof(scalar->written))) {
            return false;
        }
        for (size_t px = 0; px < SGI_RENDER_PROBE_SIZE * SGI_RENDER_PROBE_SIZE;
             px++) {
            for (unsigned c = 0; written[px] && c < 3; c++) {
                uint32_t mesa = ((uint32_t)rgba[px * 4 + c] * 255 + 32767) /
                                65535;

                if (mesa != scalar->rgb[px / SGI_RENDER_PROBE_SIZE]
                                       [px % SGI_RENDER_PROBE_SIZE][c]) {
                    return false;
                }
            }
        }
    }
    return true;
}

/* Run the probe once per renderer; false when lines stay unsupported. */
static bool sgi_render_lines_proven(SGIRenderer *renderer, Error **errp)
{
    if (!sgi_renderer_primitive_supported(renderer, SGI_RENDERER_LINES, 1,
                                          errp)) {
        return false;
    }
    if (!sgi_renderer_line_probe_result(renderer)) {
        sgi_renderer_set_line_probe_result(renderer,
            sgi_render_line_probe(renderer) ? 1 : -1);
    }
    if (sgi_renderer_line_probe_result(renderer) < 0) {
        error_setg(errp, "SGI Mesa lines differ from the scalar path on "
                   "this host");
        return false;
    }
    return true;
}

SGIRenderResult sgi_render_draw_checked(SGIRenderer *renderer,
                                        const SGIRasterRules *rules,
                                        const SGIRasterTarget *target,
                                        const SGIGLDraw *draw, Error **errp)
{
    const SGIRenderResource *resource;
    const SGIRenderResource *texture_resource = NULL;
    const SGIClipList *clip;
    SGIRenderRect rect;
    SGIRendererFrame frame = { 0 };
    SGIVolumeVertex polygon[SGI_VOLUME_TRIANGLE_MAX];
    g_autofree uint32_t *slots = NULL;
    g_autofree uint16_t *rgba = NULL;
    g_autofree uint8_t *written = NULL;
    g_autofree uint32_t *texture_slots = NULL;
    float vertices[SGI_VOLUME_TRIANGLE_MAX]
                  [SGI_RENDERER_FRAME_VERTEX_FLOATS];
    size_t pixels, nslots, nrgba;
    size_t texture_stride_slots = 0, texture_capacity_slots = 0;
    unsigned polygon_count = 0;
    uint64_t piece_vertices = 0;
    SGIRenderPieces pieces = { 0 };
    g_autofree float *piece_storage = NULL;
    g_autofree SGIRendererTriangle *window_storage = NULL;
    bool window = false;
    float point_size = 0;
    bool any_written = false;
    bool upside_down;
    bool lines = sgi_render_mode_lines(draw ? draw->mode : 0);
    SGIRenderLines line_set = { 0 };
    g_autofree SGIRendererLine *line_storage = NULL;
    uint32_t steps[4] = { 0 };
    SGIRenderCommitResult committed;
    SGIRenderResult status;

    QEMU_BUILD_BUG_ON(SGI_VOLUME_TRIANGLE_MAX !=
                      SGI_RENDERER_FRAME_VERTICES_MAX);
    if (!renderer || !rules || !target || !draw || !draw->state ||
        !draw->vertices) {
        error_setg(errp, "Missing SGI Mesa draw input");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    clip = &draw->state->drawable.clip;
    if (!draw->state->drawable.bound) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: draw with no window bound "
                      "dropped\n");
        return SGI_RENDER_OK;
    }
    /* A line draw still counts its stipple phase with nothing visible. */
    if (!clip->n && !(lines && draw->line_phase)) {
        return SGI_RENDER_OK;
    }
    if (clip->n > SGI_CLIP_MAX) {
        error_setg(errp, "Invalid SGI Mesa clip rectangle count");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    if (draw->state->scissor_test &&
        (draw->state->scissor[2] < 0 || draw->state->scissor[3] < 0)) {
        error_setg(errp, "Invalid SGI Mesa scissor dimensions");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    if (draw->state->dither && rules->dither != SGI_DITHER_NONE) {
        error_setg(errp, "SGI Mesa ordered dithering is unsupported");
        return SGI_RENDER_UNSUPPORTED;
    }
    if (!sgi_render_rules_supported(rules)) {
        error_setg(errp, "Unsupported SGI Mesa raster rules");
        return SGI_RENDER_UNSUPPORTED;
    }
    if (!sgi_render_draw_supported(rules, target, draw, &resource)) {
        error_setg(errp, "Unsupported SGI Mesa draw state or resource");
        return SGI_RENDER_UNSUPPORTED;
    }
    for (unsigned c = 0; c < resource->component_count; c++) {
        steps[c] = (1u << resource->component[c].storage_bits) - 1;
    }
    if (draw->state->blend) {
        /* sgi_render_blend_supported() matched these to the storage. */
        for (unsigned c = 0; c < 3; c++) {
            steps[c] = (1u << rules->color_bits) - 1;
        }
        steps[3] = (1u << MAX(2, MAX(rules->color_bits,
                                     rules->alpha_bits))) - 1;
    }
    if (lines) {
        uint32_t segments = sgi_render_line_segments(draw);

        if (!segments) {
            return SGI_RENDER_OK;
        }
        /* As for other primitives, see the blending refusal below. */
        if (draw->state->blend && segments > 1) {
            error_setg(errp, "SGI Mesa blending of more than one primitive "
                       "in a draw is unsupported");
            return SGI_RENDER_UNSUPPORTED;
        }
        if (!sgi_render_lines_proven(renderer, errp)) {
            return SGI_RENDER_UNSUPPORTED;
        }
        line_storage = g_try_new(SGIRendererLine,
                                 MIN(segments,
                                     SGI_RENDERER_LINE_SEGMENTS_MAX));
        if (!line_storage) {
            error_setg(errp, "Cannot allocate SGI Mesa draw lines");
            return SGI_RENDER_OOM;
        }
        line_set.lines = line_storage;
        if (!sgi_render_lines(rules, draw, steps, &line_set)) {
            error_setg(errp, "SGI Mesa cannot draw a line exactly");
            return SGI_RENDER_UNSUPPORTED;
        }
    } else if (!draw->state->texture_2d) {
        piece_vertices = sgi_render_piece_vertices(draw->mode, draw->count);
        if (!piece_vertices) {
            return SGI_RENDER_OK;
        }
        if (piece_vertices > SGI_RENDERER_FRAME_PRIMITIVE_VERTICES_MAX) {
            error_setg(errp, "SGI Mesa draw has too many vertices");
            return SGI_RENDER_UNSUPPORTED;
        }
        /*
         * The float target rounds only at readback, so a second
         * overlapping primitive would blend against an unrounded value
         * where the scalar path blends against the stored one. An 8-bit
         * target rounds per primitive but llvmpipe's fixed-point blend
         * then differs from the scalar path for single primitives.
         */
        if (draw->state->blend &&
            piece_vertices > (draw->mode == SGI_GL_POINTS ? 1 : 3)) {
            error_setg(errp, "SGI Mesa blending of more than one primitive "
                       "in a draw is unsupported");
            return SGI_RENDER_UNSUPPORTED;
        }
        if (draw->mode == SGI_GL_POINTS) {
            point_size = sgi_render_aliased_size(draw->point_size);
            if (!sgi_renderer_primitive_supported(renderer,
                                                  SGI_RENDERER_POINTS,
                                                  point_size, errp)) {
                return SGI_RENDER_UNSUPPORTED;
            }
        }
        /*
         * Smooth colour comes from the scalar path's own clipped triangles,
         * drawn at its window positions and interpolated in double.
         */
        window = draw->mode != SGI_GL_POINTS &&
                 draw->state->shade_model == SGI_GL_SMOOTH;
    }
    if (window) {
        const double reach = (double)SGI_RENDERER_WINDOW_REACH * 256;

        if (!sgi_renderer_primitive_supported(renderer,
                                              SGI_RENDERER_WINDOW_TRIANGLES,
                                              1, errp)) {
            return SGI_RENDER_UNSUPPORTED;
        }
        pieces.window_capacity = MIN(piece_vertices / 3 *
                                     (SGI_VOLUME_TRIANGLE_MAX - 2),
                                     SGI_RENDERER_WINDOW_TRIANGLES_MAX);
        window_storage = g_try_new(SGIRendererTriangle,
                                   pieces.window_capacity);
        if (!window_storage) {
            error_setg(errp, "Cannot allocate SGI Mesa draw triangles");
            return SGI_RENDER_OOM;
        }
        pieces.window = window_storage;
        pieces.rules = rules;
        sgi_render_decompose(&pieces, draw);
        if (pieces.window_count > pieces.window_capacity) {
            error_setg(errp, "SGI Mesa draw has too many triangles");
            return SGI_RENDER_UNSUPPORTED;
        }
        if (!pieces.window_count) {
            return SGI_RENDER_OK;
        }
        /* Every vertex inside the fixed viewport, rows either way up. */
        for (uint32_t i = 0; i < pieces.window_count; i++) {
            const SGIRendererTriangle *t = &window_storage[i];

            for (unsigned v = 0; v < 3; v++) {
                double y = resource->height * 256.0 - t->y[v];

                if (!(t->x[v] > -reach && t->x[v] < 3 * reach &&
                      t->y[v] > -reach && t->y[v] < 3 * reach &&
                      y > -reach && y < 3 * reach)) {
                    error_setg(errp, "SGI Mesa triangle lies outside the "
                               "window viewport");
                    return SGI_RENDER_UNSUPPORTED;
                }
            }
        }
    }
    if (draw->state->texture_2d) {
        texture_resource = sgi_render_texture_resource(target, draw->state);
        if (!texture_resource) {
            error_setg(errp,
                       "Unsupported SGI Mesa texture resource or sampler");
            return SGI_RENDER_UNSUPPORTED;
        }
        status = sgi_render_texture_span(texture_resource,
                                         &texture_stride_slots,
                                         &texture_capacity_slots, errp);
        if (status != SGI_RENDER_OK) {
            return status;
        }
        if (!sgi_renderer_texture_supported(renderer, errp)) {
            return SGI_RENDER_UNSUPPORTED;
        }
    }
    if (!sgi_renderer_viewport_supported(renderer, draw->state->viewport,
                                         errp)) {
        return SGI_RENDER_UNSUPPORTED;
    }
    if (!target->resource_ops || !target->resource_ops->commit_covered) {
        error_setg(errp, "SGI Mesa draw needs a covered commit provider");
        return SGI_RENDER_UNSUPPORTED;
    }
    /*
     * Every refusal is above, so a refused draw leaves a borrowed phase
     * for the caller's scalar fallback to count once. From here the draw
     * counts as the scalar path's does.
     */
    if (lines) {
        if (draw->line_phase && line_set.consumed) {
            *draw->line_phase = line_set.counter;
        }
        if (!line_set.count) {
            return SGI_RENDER_OK;
        }
    }
    frame.width = resource->width;
    frame.height = resource->height;
    /*
     * A triangle frame holds window rows top first: llvmpipe applies its
     * top-left rule with rows running down, so drawing upside down gives a
     * pixel centre on a horizontal edge to the same triangle as the scalar
     * path's top-left rule in OpenGL's upward window rows. Points and
     * lines keep upward rows, where they already match.
     */
    upside_down = sgi_render_mode_polygons(draw->mode);
    frame.viewport_given = true;
    memcpy(frame.viewport, draw->state->viewport, sizeof(frame.viewport));
    if (upside_down) {
        int64_t y = (int64_t)resource->height - frame.viewport[1] -
                    frame.viewport[3];

        /* Refuse rather than wrap a mirrored origin GL cannot take. */
        if (y < INT32_MIN || y > INT32_MAX) {
            error_setg(errp, "SGI Mesa viewport origin is out of range");
            return SGI_RENDER_UNSUPPORTED;
        }
        frame.viewport[1] = y;
    }
    for (unsigned i = 0; i < clip->n; i++) {
        int64_t x0 = clip->r[i].x0;
        int64_t y0 = clip->r[i].y0;
        int64_t x1 = clip->r[i].x1;
        int64_t y1 = clip->r[i].y1;

        x0 = MAX(x0, 0);
        y0 = MAX(y0, 0);
        x1 = MIN(x1, (int64_t)resource->width - 1);
        y1 = MIN(y1, (int64_t)resource->height - 1);
        if (!resource->bottom_origin && y0 <= y1) {
            int64_t native_y0 = y0;

            /* Drawable clips use native rows; scissor uses GL window rows. */
            y0 = (int64_t)resource->height - 1 - y1;
            y1 = (int64_t)resource->height - 1 - native_y0;
        }
        if (draw->state->scissor_test) {
            const int32_t *s = draw->state->scissor;

            x0 = MAX(x0, s[0]);
            y0 = MAX(y0, s[1]);
            x1 = MIN(x1, (int64_t)s[0] + s[2] - 1);
            y1 = MIN(y1, (int64_t)s[1] + s[3] - 1);
        }
        if (window || lines) {
            /* The scalar path bounds these fragments by the viewport. */
            const int32_t *v = draw->state->viewport;

            x0 = MAX(x0, v[0]);
            y0 = MAX(y0, v[1]);
            x1 = MIN(x1, (int64_t)v[0] + v[2] - 1);
            y1 = MIN(y1, (int64_t)v[1] + v[3] - 1);
        }
        if (x0 <= x1 && y0 <= y1) {
            if (upside_down) {
                int64_t window_y0 = y0;

                y0 = (int64_t)resource->height - 1 - y1;
                y1 = (int64_t)resource->height - 1 - window_y0;
            }
            frame.clips[frame.clip_count++] =
                (SGIRendererClearRect) { x0, y0, x1, y1 };
        }
    }
    if (!frame.clip_count) {
        return SGI_RENDER_OK;
    }
    if (texture_resource) {
        SGIVolumeVertex source[3];

        for (unsigned i = 0; i < 3; i++) {
            sgi_volume_from_draw_vertex(draw->state,
                draw->vertices + i * SGI_GL_VERTEX_FLOATS, &source[i]);
        }
        polygon_count = sgi_volume_clip_triangle(source, polygon);
        if (!polygon_count) {
            return SGI_RENDER_OK;
        }
        for (unsigned i = 0; i < polygon_count; i++) {
            const SGIVolumeVertex *v = &polygon[i];
            bool valid = isfinite(v->clip[3]) && v->clip[3] > 0 &&
                         (float)v->clip[3] > 0 &&
                         isfinite(v->attr[SGI_GL_V_TEXCOORD + 3]) &&
                         v->attr[SGI_GL_V_TEXCOORD + 3] > 0;

            for (unsigned c = 0; c < 4; c++) {
                valid &= isfinite(v->clip[c]) &&
                         isfinite((float)v->clip[c]) &&
                         isfinite(v->attr[SGI_GL_V_TEXCOORD + c]);
            }
            if (!valid) {
                error_setg(errp,
                           "Unsupported SGI Mesa clipped texture coordinates");
                return SGI_RENDER_UNSUPPORTED;
            }
        }
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
    if (texture_resource) {
        SGIRenderRect texture_rect = {
            .x1 = texture_resource->width - 1,
            .y1 = texture_resource->height - 1,
        };

        texture_slots = g_try_new(uint32_t, texture_capacity_slots);
        if (!texture_slots) {
            error_setg(errp, "Cannot allocate SGI Mesa texture snapshot");
            return SGI_RENDER_OOM;
        }
        status = sgi_render_resource_snapshot(target, texture_resource, 0,
                                              &texture_rect, texture_slots,
                                              texture_stride_slots,
                                              texture_capacity_slots, errp);
        if (status != SGI_RENDER_OK) {
            return status;
        }
        for (size_t i = 0; i < texture_capacity_slots; i++) {
            if (texture_slots[i] > UINT8_MAX) {
                error_setg(errp, "SGI Mesa texture snapshot has invalid bits");
                return SGI_RENDER_RESOURCE_ERROR;
            }
        }
        frame.texture = (SGIRendererTexture2D) {
            .width = texture_resource->width,
            .height = texture_resource->height,
            .env_mode = draw->state->texture_env_mode,
            .wrap_s = target->texture->wrap_s,
            .wrap_t = target->texture->wrap_t,
            .linear_filter = target->texture->min_filter == SGI_GL_LINEAR,
            .component_bits = 8,
            .slots = texture_slots,
            .capacity_slots = texture_capacity_slots,
        };
        for (unsigned c = 0; c < 4; c++) {
            frame.texture.border_bytes[c] =
                MIN(MAX(target->texture->border_color[c], 0.0), 1.0) * 255.0;
            frame.texture.environment_bytes[c] =
                MIN(MAX(draw->state->texture_env_color[c], 0.0), 1.0) * 255.0;
        }
    }
    for (uint32_t y = 0; y < resource->height; y++) {
        size_t native_row = (size_t)y * resource->width;
        size_t frame_row = (size_t)(resource->bottom_origin != upside_down ?
                                    y : resource->height - 1 - y) *
                           resource->width;

        for (uint32_t x = 0; x < resource->width; x++) {
            size_t native_pixel = native_row + x;
            size_t frame_pixel = frame_row + x;

            for (unsigned c = 0; c < resource->component_count; c++) {
                unsigned bits = resource->component[c].storage_bits;
                uint32_t max = (1u << bits) - 1;

                rgba[frame_pixel * 4 + c] =
                    ((uint64_t)slots[native_pixel *
                                     resource->component_count + c] * 65535 +
                     max / 2) / max;
            }
            if (resource->component_count == 3) {
                rgba[frame_pixel * 4 + 3] = 65535;
            }
        }
    }
    if (texture_resource) {
        for (unsigned i = 0; i < polygon_count; i++) {
            const SGIVolumeVertex *v = &polygon[i];
            const float *colour = draw->state->shade_model == SGI_GL_FLAT ?
                draw->vertices + 2 * SGI_GL_VERTEX_FLOATS + SGI_GL_V_COLOR :
                &v->attr[SGI_GL_V_COLOR];

            for (unsigned c = 0; c < 4; c++) {
                vertices[i][c] = v->clip[c];
            }
            memcpy(&vertices[i][4], colour, 4 * sizeof(float));
            memcpy(&vertices[i][8], &v->attr[SGI_GL_V_TEXCOORD],
                   4 * sizeof(float));
        }
        frame.count = polygon_count;
        frame.vertices = &vertices[0][0];
    } else if (window) {
        memcpy(frame.colour_steps, steps, sizeof(steps));
        frame.primitive = SGI_RENDERER_WINDOW_TRIANGLES;
        frame.triangles = window_storage;
        frame.count = pieces.window_count;
    } else if (lines) {
        memcpy(frame.colour_steps, steps, sizeof(steps));
        frame.primitive = SGI_RENDERER_LINES;
        frame.lines = line_set.lines;
        frame.count = line_set.count;
        frame.line_stipple = draw->line_stipple_enabled;
        frame.line_stipple_repeat = MAX(1, MIN(draw->line_stipple_repeat,
                                               256));
        frame.line_stipple_pattern = draw->line_stipple_pattern & 0xffff;
    } else {
        piece_storage = g_try_new(float, piece_vertices *
                                         SGI_RENDERER_FRAME_VERTEX_FLOATS);
        if (!piece_storage) {
            error_setg(errp, "Cannot allocate SGI Mesa draw vertices");
            return SGI_RENDER_OOM;
        }
        pieces.v = (float (*)[SGI_RENDERER_FRAME_VERTEX_FLOATS])piece_storage;
        pieces.flat = draw->state->shade_model == SGI_GL_FLAT;
        memcpy(frame.colour_steps, steps, sizeof(steps));
        pieces.steps = frame.colour_steps;
        sgi_render_decompose(&pieces, draw);
        assert(pieces.count == piece_vertices);
        frame.primitive = pieces.primitive;
        frame.point_size = point_size;
        frame.count = pieces.count;
        frame.vertices = piece_storage;
    }
    frame.shade_model = draw->state->shade_model;
    frame.blend = draw->state->blend;
    frame.blend_src = draw->state->blend_src;
    frame.blend_dst = draw->state->blend_dst;
    frame.blend_equation = draw->state->blend_equation;
    frame.rgba = rgba;
    frame.written = written;
    frame.written_capacity = pixels;
    if (texture_resource) {
        for (unsigned i = 0; i < 4; i++) {
            frame.modelview[i * 5] = 1;
            frame.projection[i * 5] = 1;
        }
    } else {
        memcpy(frame.modelview, draw->state->modelview,
               sizeof(frame.modelview));
        memcpy(frame.projection, draw->state->projection,
               sizeof(frame.projection));
    }
    /* An upside-down frame negates clip y, the projection's second row. */
    for (unsigned i = 0; upside_down && i < 4; i++) {
        frame.projection[i * 4 + 1] = -frame.projection[i * 4 + 1];
    }
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
    for (uint32_t y = 0; y < resource->height; y++) {
        size_t native_row = (size_t)y * resource->width;
        size_t frame_row = (size_t)(resource->bottom_origin != upside_down ?
                                    y : resource->height - 1 - y) *
                           resource->width;

        for (uint32_t x = 0; x < resource->width; x++) {
            size_t native_pixel = native_row + x;
            size_t frame_pixel = frame_row + x;

            if (!written[frame_pixel]) {
                continue;
            }
            for (unsigned c = 0; c < resource->component_count; c++) {
                unsigned bits = resource->component[c].storage_bits;
                uint32_t max = (1u << bits) - 1;

                slots[native_pixel * resource->component_count + c] =
                    ((uint64_t)rgba[frame_pixel * 4 + c] * max + 32767) /
                    65535;
            }
        }
    }
    /* Coverage returns to native rows. */
    if (resource->bottom_origin == upside_down) {
        for (uint32_t y = 0; y < resource->height / 2; y++) {
            uint8_t *top = written + (size_t)y * resource->width;
            uint8_t *bottom = written +
                              (size_t)(resource->height - 1 - y) *
                              resource->width;

            for (uint32_t x = 0; x < resource->width; x++) {
                uint8_t value = top[x];

                top[x] = bottom[x];
                bottom[x] = value;
            }
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
    const SGIRenderResource *resources[SGI_GL_MAX_TARGETS + 2] = { 0 };
    SGIRendererClear clears[SGI_GL_MAX_TARGETS] = { 0 };
    uint32_t *slots[SGI_GL_MAX_TARGETS + 2] = { 0 };
    uint16_t *rgba[SGI_GL_MAX_TARGETS] = { 0 };
    uint8_t *covered = NULL;
    uint32_t max_width = 0;
    uint32_t *packed = NULL;
    size_t pixels[SGI_GL_MAX_TARGETS + 2] = { 0 };
    size_t nslots[SGI_GL_MAX_TARGETS + 2] = { 0 };
    unsigned colour_count, depth_index, stencil_index, clear_count;
    bool depth, stencil;
    SGIRenderResult status = SGI_RENDER_OK;

    if (!renderer || !rules || !target || !op) {
        error_setg(errp, "Missing SGI Mesa raster input");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    if (op->kind != SGI_RASTER_CLEAR ||
        (op->clear_buffers & ~(SGI_GL_COLOR_BUFFER_BIT |
                               SGI_GL_DEPTH_BUFFER_BIT |
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
    depth = !!(op->clear_buffers & SGI_GL_DEPTH_BUFFER_BIT);
    stencil = !!(op->clear_buffers & SGI_GL_STENCIL_BUFFER_BIT) &&
              !!(op->stencil_mask & 0xff);
    colour_count = (op->clear_buffers & SGI_GL_COLOR_BUFFER_BIT) &&
                   op->clear_colour_mask ? op->ntargets : 0;
    depth_index = colour_count;
    stencil_index = colour_count + !!depth;
    clear_count = MAX(colour_count, (unsigned)(depth || stencil));
    if (!clear_count || (!op->clip.n && op->clip.empty)) {
        return SGI_RENDER_OK;
    }
    if (colour_count) {
        for (unsigned c = 0; c < 4; c++) {
            if (!isfinite(op->clear_rgba[c])) {
                error_setg(errp, "Invalid SGI Mesa clear colour");
                return SGI_RENDER_RESOURCE_ERROR;
            }
        }
    }
    if ((depth && rules->depth_bits != 24) ||
        (stencil && rules->stencil_bits != 8)) {
        error_setg(errp, "Unsupported SGI Mesa clear scalar precision");
        return SGI_RENDER_UNSUPPORTED;
    }
    for (unsigned i = 0; i < colour_count; i++) {
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
        if (!sgi_render_widths_proven(rules, res)) {
            error_setg(errp, "Unsupported SGI Mesa clear component widths");
            return SGI_RENDER_UNSUPPORTED;
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
        clear->colour = true;
        clear->colour_mask = op->clear_colour_mask;
        memcpy(clear->rgba, op->clear_rgba, sizeof(clear->rgba));
        /*
         * Round the clear colour to the native steps in double, as the
         * scalar path does, so a half step in float32 such as 0.9 * 255
         * reaches the worker already on a step.
         */
        for (unsigned c = 0; c < res->component_count; c++) {
            uint32_t max = (1u << res->component[c].storage_bits) - 1;
            double v = op->clear_rgba[c];

            clear->rgba[c] = !(v > 0) ? 0 : v >= 1 ? 1 :
                             (float)((uint32_t)(v * max + 0.5) / (double)max);
        }
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
    for (unsigned k = 0; k < (unsigned)depth + (unsigned)stencil; k++) {
        unsigned i = colour_count + k;
        SGIRenderRole role = k == 0 && depth ? SGI_RENDER_DEPTH :
                                                   SGI_RENDER_STENCIL;
        const SGIRenderResource *res = sgi_render_scalar_resource(target,
                                                                  role);
        unsigned bits = role == SGI_RENDER_DEPTH ? 24 : 8;

        if (!res || !res->width || !res->height ||
            res->width > INT_MAX || res->height > INT_MAX ||
            res->depth != 1 || !res->bottom_origin ||
            res->component_count != 1 ||
            res->component[0].channel != SGI_RENDER_SCALAR ||
            res->component[0].numeric != SGI_RENDER_UINT ||
            res->component[0].storage_bits != bits ||
            (uint64_t)res->width * res->height >
                SIZE_MAX / sizeof(uint32_t) ||
            (colour_count && (res->width != clears[0].width ||
                              res->height != clears[0].height)) ||
            (k && (res->width != resources[colour_count]->width ||
                   res->height != resources[colour_count]->height))) {
            error_setg(errp, "Unsupported SGI Mesa clear scalar resource");
            return SGI_RENDER_UNSUPPORTED;
        }
        resources[i] = res;
        max_width = MAX(max_width, res->width);
    }
    if (depth || stencil) {
        for (unsigned i = 1; i < colour_count; i++) {
            if (clears[i].width != clears[0].width ||
                clears[i].height != clears[0].height) {
                error_setg(errp, "SGI Mesa clear attachment sizes differ");
                return SGI_RENDER_UNSUPPORTED;
            }
        }
    }
    if (depth || stencil) {
        SGIRendererClear *clear = &clears[0];

        clear->depth = depth;
        clear->stencil = stencil;
        clear->depth24 = op->clear_depth24 & 0xffffff;
        clear->stencil8 = op->clear_stencil & 0xff;
        clear->stencil_mask = op->stencil_mask & 0xff;
        if (!colour_count) {
            const SGIRenderResource *res = resources[0];
            int32_t x0 = MAX(op->x0, 0);
            int32_t y0 = MAX(op->y0, 0);
            int32_t x1 = MIN(op->x1, (int32_t)res->width - 1);
            int32_t y1 = MIN(op->y1, (int32_t)res->height - 1);

            clear->width = res->width;
            clear->height = res->height;
            if (x0 <= x1 && y0 <= y1) {
                if (!op->clip.n) {
                    clear->rects[clear->rect_count++] =
                        (SGIRendererClearRect) { x0, y0, x1, y1 };
                } else {
                    for (unsigned j = 0; j < op->clip.n; j++) {
                        const SGIClipRect *clip = &op->clip.r[j];
                        int32_t cx0 = MAX(x0, clip->x0);
                        int32_t cy0 = MAX(y0, clip->y0);
                        int32_t cx1 = MIN(x1, clip->x1);
                        int32_t cy1 = MIN(y1, clip->y1);

                        if (cx0 <= cx1 && cy0 <= cy1) {
                            clear->rects[clear->rect_count++] =
                                (SGIRendererClearRect) { cx0, cy0,
                                                         cx1, cy1 };
                        }
                    }
                }
            }
        }
    }
    for (unsigned i = 0; i < colour_count; i++) {
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
    for (unsigned i = colour_count; i < colour_count + depth + stencil; i++) {
        const SGIRenderResource *res = resources[i];
        SGIRenderRect rect = { .x1 = res->width - 1,
                               .y1 = res->height - 1 };

        if (!clears[0].rect_count) {
            continue;
        }
        pixels[i] = (size_t)res->width * res->height;
        nslots[i] = pixels[i];
        slots[i] = g_try_new(uint32_t, nslots[i]);
        if (!slots[i]) {
            error_setg(errp, "Cannot allocate SGI Mesa scalar slots");
            status = SGI_RENDER_OOM;
            goto out;
        }
        status = sgi_render_resource_snapshot(target, res, 0, &rect,
                                              slots[i], res->width,
                                              nslots[i], errp);
        if (status != SGI_RENDER_OK) {
            goto out;
        }
    }
    if ((depth || stencil) && clears[0].rect_count) {
        size_t count = (size_t)clears[0].width * clears[0].height;

        packed = g_try_new(uint32_t, count);
        if (!packed) {
            error_setg(errp, "Cannot allocate SGI Mesa scalar buffer");
            status = SGI_RENDER_OOM;
            goto out;
        }
        for (size_t pixel = 0; pixel < count; pixel++) {
            packed[pixel] = (depth ? slots[depth_index][pixel] << 8 : 0) |
                            (stencil ? slots[stencil_index][pixel] : 0);
        }
        clears[0].depth_stencil = packed;
    }
    for (unsigned i = 0; i < clear_count; i++) {
        const SGIRenderResource *res = i < colour_count ? resources[i] :
                                                     resources[0];
        SGIRendererClear *clear = &clears[i];

        if (!clear->rect_count) {
            continue;
        }
        if (!sgi_renderer_clear(renderer, clear, errp)) {
            status = SGI_RENDER_HOST_ERROR;
            goto out;
        }
        if (i >= colour_count) {
            continue;
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
    if ((depth || stencil) && clears[0].rect_count) {
        const SGIRendererClear *clear = &clears[0];

        for (unsigned j = 0; j < clear->rect_count; j++) {
            const SGIRendererClearRect *r = &clear->rects[j];

            for (uint32_t y = r->y0; y <= r->y1; y++) {
                for (uint32_t x = r->x0; x <= r->x1; x++) {
                    size_t pixel = (size_t)y * clear->width + x;

                    if (depth) {
                        slots[depth_index][pixel] = packed[pixel] >> 8;
                    }
                    if (stencil) {
                        slots[stencil_index][pixel] = packed[pixel] & 0xff;
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
    for (unsigned i = 0; i < colour_count + depth + stencil; i++) {
        const SGIRenderResource *res = resources[i];
        SGIRendererClear *clear = i < colour_count ? &clears[i] : &clears[0];

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
    for (unsigned i = 0; i < SGI_GL_MAX_TARGETS + 2; i++) {
        g_free(slots[i]);
        if (i < SGI_GL_MAX_TARGETS) {
            g_free(rgba[i]);
        }
    }
    g_free(packed);
    return status;
}
