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

/*
 * Whether the host program samples this image as the scalar path does.
 * A one-dimensional image under the 2D enable is dropped by the scalar
 * path, and the program has no texture comparison, so both are refused.
 * So are raw-component images: the scalar sampler reads their components
 * at up to 16 bits, and no comparison covers them against the 8-bit
 * texture resource the host samples.
 */
static bool sgi_render_texture_image_supported(const SGIGLTexImage *image,
                                               const SGIGLDrawState *st)
{
    if (!image || image->one_dimensional || image->shadow_compare ||
        image->read_texel_components ||
        image->read_border_texel_components ||
        image->volume || image->pyramid || image->border ||
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
        return false;
    }
    for (unsigned c = 0; c < 4; c++) {
        if (!isfinite(image->border_color[c]) ||
            !isfinite(st->texture_env_color[c])) {
            return false;
        }
    }
    return true;
}

static const SGIRenderResource *sgi_render_texture_resource(
    const SGIRasterTarget *target, const SGIGLDrawState *st)
{
    const SGIGLTexImage *image = target->texture;
    const SGIRenderResource *res = NULL;

    if (!sgi_render_texture_image_supported(image, st)) {
        return NULL;
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

/*
 * Whether the rules' depth row departs from OpenGL's depth for this draw:
 * it does wherever clip w varies, which the host's depth cannot follow.
 */
static bool sgi_render_depth_departs(const SGIRasterRules *rules,
                                     const SGIGLDrawState *st)
{
    return rules->depth_row_given &&
           (st->projection[3] != 0 || st->projection[7] != 0 ||
            st->projection[11] != 0 || st->modelview[3] != 0 ||
            st->modelview[7] != 0 || st->modelview[11] != 0);
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
 * (OpenGL 1.2.1, 3.3 to 3.5). One- and three-dimensional texturing have
 * no host path yet and are refused, so the scalar path keeps their
 * dimensionality checks.
 */
static SGIRenderRefusal sgi_render_draw_supported(
    const SGIRasterRules *rules, const SGIRasterTarget *target,
    const SGIGLDraw *draw, const SGIRenderResource **result)
{
    const SGIGLDrawState *st = draw->state;
    const SGIRenderResource *res;

    if (draw->mode > SGI_GL_POLYGON ||
        (draw->mode == SGI_GL_POINTS &&
         (!isfinite(draw->point_size) || draw->point_size < 0)) ||
        (sgi_render_mode_lines(draw->mode) &&
         (!isfinite(draw->line_width) || draw->line_width < 0)) ||
        (sgi_render_mode_polygons(draw->mode) && draw->polygon_modes &&
         (draw->polygon_modes[0] != SGI_GL_FILL ||
          draw->polygon_modes[1] != SGI_GL_FILL)) ||
        draw->polygon_stipple_enabled ||
        (!sgi_render_mode_lines(draw->mode) && draw->line_stipple_enabled) ||
        (st->shade_model != SGI_GL_SMOOTH &&
         st->shade_model != SGI_GL_FLAT)) {
        return SGI_RENDER_REFUSAL_PRIMITIVE;
    }
    if ((sgi_render_mode_lines(draw->mode) && rules->smooth_primitives &&
         draw->line_smooth) ||
        (draw->mode == SGI_GL_POINTS && rules->smooth_primitives &&
         draw->point_smooth) ||
        (sgi_render_mode_polygons(draw->mode) && rules->smooth_primitives &&
         draw->polygon_smooth)) {
        return SGI_RENDER_REFUSAL_SMOOTHING;
    }
    if (st->texture_2d &&
        (draw->mode != SGI_GL_TRIANGLES || draw->count != 3)) {
        return SGI_RENDER_REFUSAL_TEXTURE;
    }
    if (draw->back_colors || st->cull_face) {
        return SGI_RENDER_REFUSAL_FACING;
    }
    if (st->index_bits) {
        return SGI_RENDER_REFUSAL_COLOUR_INDEX;
    }
    if (st->draw_buffers != SGI_GL_DRAW_FRONT ||
        st->colour_mask != SGI_GL_COLOUR_MASK_ALL) {
        return SGI_RENDER_REFUSAL_BUFFERS;
    }
    if (st->depth_test || st->stencil_test || st->alpha_test) {
        return SGI_RENDER_REFUSAL_FRAGMENT_TESTS;
    }
    if (st->blend > 1) {
        return SGI_RENDER_REFUSAL_BLEND;
    }
    if (st->fog) {
        return SGI_RENDER_REFUSAL_FOG;
    }
    if (st->clip_planes) {
        return SGI_RENDER_REFUSAL_CLIP_PLANES;
    }
    if (st->color_logic_op || st->index_logic_op) {
        return SGI_RENDER_REFUSAL_LOGIC_OP;
    }
    if (st->texture_1d || st->texture_3d) {
        return SGI_RENDER_REFUSAL_TEXTURE;
    }
    if (st->polygon_offset) {
        return SGI_RENDER_REFUSAL_POLYGON_OFFSET;
    }
    if (st->window_matrix_given || !st->drawable.bound ||
        st->drawable.origin[0] || st->drawable.origin[1]) {
        return SGI_RENDER_REFUSAL_WINDOW;
    }
    if (sgi_render_depth_departs(rules, st)) {
        return SGI_RENDER_REFUSAL_RULES;
    }
    if (st->viewport[2] < 0 || st->viewport[3] < 0) {
        return SGI_RENDER_REFUSAL_GEOMETRY;
    }
    for (unsigned i = 0; i < 16; i++) {
        if (!isfinite(st->modelview[i]) || !isfinite(st->projection[i])) {
            return SGI_RENDER_REFUSAL_GEOMETRY;
        }
    }
    for (uint32_t i = 0; i < draw->count; i++) {
        const float *vertex = draw->vertices +
                              (size_t)i * SGI_GL_VERTEX_FLOATS;

        for (unsigned c = 0; c < 4; c++) {
            if (!isfinite(vertex[SGI_GL_V_POSITION + c]) ||
                !isfinite(vertex[SGI_GL_V_COLOR + c])) {
                return SGI_RENDER_REFUSAL_GEOMETRY;
            }
        }
    }
    res = sgi_render_colour_resource(target, st->drawable.front);
    if (!res || !res->width || !res->height || res->width > INT_MAX ||
        res->height > INT_MAX || res->depth != 1 ||
        res->buffer != SGI_RENDER_FRONT ||
        (res->component_count != 3 && res->component_count != 4)) {
        return SGI_RENDER_REFUSAL_RESOURCE;
    }
    for (unsigned c = 0; c < res->component_count; c++) {
        if (res->component[c].channel != SGI_RENDER_R + c ||
            res->component[c].numeric != SGI_RENDER_UNORM ||
            !res->component[c].storage_bits ||
            res->component[c].storage_bits > 16) {
            return SGI_RENDER_REFUSAL_RESOURCE;
        }
    }
    if (!sgi_render_widths_proven(rules, res)) {
        return SGI_RENDER_REFUSAL_STORE_WIDTH;
    }
    if (st->blend && !sgi_render_blend_supported(rules, st, res)) {
        return SGI_RENDER_REFUSAL_BLEND;
    }
    if (st->texture_2d) {
        if (st->blend) {
            return SGI_RENDER_REFUSAL_TEXTURE;
        }
        for (unsigned c = 0; c < res->component_count; c++) {
            if (res->component[c].storage_bits != 8) {
                return SGI_RENDER_REFUSAL_TEXTURE;
            }
        }
    }
    *result = res;
    return SGI_RENDER_REFUSAL_NONE;
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
    /*
     * With rules set, each triangle is also clipped and projected as the
     * scalar path does, to find depths it may round past a plane; with v
     * NULL as well, nothing else is produced.
     */
    const SGIRasterRules *rules;
    bool depth_inexact;
    bool near_plane;    /* some vertex within the margin of a plane */
    /* A textured triangle the scalar path drops whole; see below. */
    bool texture_invalid;
    const SGIGLTexImage *image;     /* a textured draw's level-zero image */
    /* With extent set, pieces only widen it; see sgi_render_piece_extent. */
    struct SGIRenderExtent *extent;
    float point_size;
    /*
     * With index set (an indexed draw, steps NULL), each vertex written
     * to v also writes its piece's index: the provoking vertex's, as the
     * scalar path converts it at index_bits.
     */
    uint32_t *index;
    uint32_t index_bits;
} SGIRenderPieces;

/*
 * Window pixels, rows up, that bound everything a draw can write. Unknown
 * when a vertex cannot be placed, and then the whole clip union is used.
 */
typedef struct SGIRenderExtent {
    bool known;
    double x0, y0, x1, y1;
} SGIRenderExtent;

static void sgi_render_extent_add(SGIRenderExtent *e, double x, double y,
                                  double pad)
{
    if (!isfinite(x) || !isfinite(y)) {
        e->known = false;
        return;
    }
    e->x0 = MIN(e->x0, x - pad);
    e->y0 = MIN(e->y0, y - pad);
    e->x1 = MAX(e->x1, x + pad);
    e->y1 = MAX(e->y1, y + pad);
}

/*
 * Widen the extent by one point or triangle as GL draws it: a triangle
 * clipped to the view volume, each vertex mapped to the window, or a
 * point's centre and half its size. Two pixels of margin cover snapping
 * and the pixel-centre rules. A clipped vertex at w <= 0 has no window
 * position, so the extent becomes unknown.
 */
static void sgi_render_piece_extent(SGIRenderPieces *p, const SGIGLDraw *draw,
                                    const uint32_t *index, unsigned n)
{
    SGIVolumeVertex in[3], poly[SGI_VOLUME_TRIANGLE_MAX];
    double pad = n == 1 ? p->point_size / 2 + 2 : 2;
    unsigned m;

    for (unsigned i = 0; i < n; i++) {
        sgi_volume_from_draw_vertex(draw->state,
            draw->vertices + (size_t)index[i] * SGI_GL_VERTEX_FLOATS, &in[i]);
    }
    if (n == 3) {
        m = sgi_volume_clip_triangle(in, poly);
    } else if (sgi_volume_contains(&in[0])) {
        poly[0] = in[0];
        m = 1;
    } else {
        return;
    }
    for (unsigned i = 0; i < m; i++) {
        double win[3];

        if (!(poly[i].clip[3] > 0)) {
            p->extent->known = false;
            return;
        }
        sgi_gl_clip_to_window(draw->state, poly[i].clip, win);
        sgi_render_extent_add(p->extent, win[0], win[1], pad);
    }
}

static int sgi_render_test_shift;

void sgi_render_test_window_shift(int subpixels)
{
    sgi_render_test_shift = subpixels;
}

/* Far below the plane, far above the scalar depth sum's rounding error. */
#define SGI_RENDER_DEPTH_MARGIN ldexp(1, -32)

/*
 * The scalar path drops a fragment whose depth, interpolated in double
 * from the first vertex, rounds outside [0, 1]; GL ignores depth here.
 * Equal depths interpolate exactly. Otherwise a sum can round past a plane
 * only within rounding error of it, so only where a vertex is that close,
 * as one on the near or far plane is. Such a triangle is drawn by the
 * window program, which repeats the scalar sum and its drop.
 */
static bool sgi_render_depth_exact(const SGIRasterTriangle *t)
{
    double lo = MIN(t->z[0], MIN(t->z[1], t->z[2]));
    double hi = MAX(t->z[0], MAX(t->z[1], t->z[2]));

    return lo == hi || (lo >= SGI_RENDER_DEPTH_MARGIN &&
                        hi <= 1 - SGI_RENDER_DEPTH_MARGIN);
}

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

    if (p->extent) {
        sgi_render_piece_extent(p, draw, index, n);
        return;
    }
    if (n == 3 && p->rules) {
        SGIRasterTriangle t[SGI_VOLUME_TRIANGLE_MAX - 2];
        unsigned count = sgi_raster_triangle_pieces(p->rules, draw->state,
            draw->vertices + (size_t)index[0] * SGI_GL_VERTEX_FLOATS,
            draw->vertices + (size_t)index[1] * SGI_GL_VERTEX_FLOATS,
            draw->vertices + (size_t)index[2] * SGI_GL_VERTEX_FLOATS, t);

        for (unsigned i = 0; i < count; i++) {
            p->depth_inexact |= !sgi_render_depth_exact(&t[i]);
            for (unsigned v = 0; v < 3; v++) {
                p->near_plane |= t[i].z[v] <= SGI_RENDER_DEPTH_MARGIN ||
                                 t[i].z[v] >= 1 - SGI_RENDER_DEPTH_MARGIN;
            }
        }
        if (draw->state->texture_2d && p->image) {
            p->texture_invalid |= !sgi_raster_triangle_texture_valid(
                draw->state, p->image,
                draw->vertices + (size_t)index[0] * SGI_GL_VERTEX_FLOATS,
                draw->vertices + (size_t)index[1] * SGI_GL_VERTEX_FLOATS,
                draw->vertices + (size_t)index[2] * SGI_GL_VERTEX_FLOATS);
        }
        if (!p->window && !p->v) {
            return;
        }
        for (unsigned i = 0; p->window && i < count; i++) {
            SGIRendererTriangle *out;

            if (p->window_count == p->window_capacity) {
                p->window_count++;
                return;
            }
            out = &p->window[p->window_count++];
            for (unsigned v = 0; v < 3; v++) {
                out->x[v] = t[i].x[v] + sgi_render_test_shift;
                out->y[v] = t[i].y[v];
                out->z[v] = t[i].z[v];
                out->inv_w[v] = t[i].inv_w[v];
                memcpy(out->texcoord[v], t[i].texcoord[v],
                       sizeof(out->texcoord[v]));
                for (unsigned c = 0; c < 4; c++) {
                    /* A flat piece takes the provoking colour, unclamped. */
                    out->colour[v][c] = p->flat ? colour[c] : t[i].rgba[v][c];
                }
            }
        }
        if (p->window) {
            return;
        }
    }
    /*
     * A constant colour is rounded here in double, as the scalar path
     * rounds it, so a product exactly on a half step in float32, such as
     * 0.9 * 255, reaches the worker already on a step.
     */
    if (p->steps && (p->flat || n == 1)) {
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

        if (p->index) {
            p->index[p->count - 1] = sgi_gl_index_value(p->index_bits,
                draw->vertices[(size_t)provoking * SGI_GL_VERTEX_FLOATS +
                               SGI_GL_V_INDEX]);
        }
        memcpy(dst, &src[SGI_GL_V_POSITION], 4 * sizeof(float));
        memcpy(dst + 4, p->steps && (p->flat || n == 1) ? rounded :
               &src[SGI_GL_V_COLOR], 4 * sizeof(float));
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

/*
 * OK when every probe vector matches the scalar path, UNSUPPORTED when one
 * differs, or the host failure that stopped the probe.
 */
static SGIRenderResult sgi_render_line_probe(SGIRenderer *renderer,
                                             Error **errp)
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
            return SGI_RENDER_UNSUPPORTED;
        }
        frame.count = lines.count;
        if (!sgi_renderer_draw_frame(renderer, &frame, errp)) {
            return SGI_RENDER_HOST_ERROR;
        }
        /* Coverage, and each covered pixel's colour, must match. */
        if (memcmp(written, scalar->written, sizeof(scalar->written))) {
            return SGI_RENDER_UNSUPPORTED;
        }
        for (size_t px = 0; px < SGI_RENDER_PROBE_SIZE * SGI_RENDER_PROBE_SIZE;
             px++) {
            for (unsigned c = 0; written[px] && c < 3; c++) {
                uint32_t mesa = ((uint32_t)rgba[px * 4 + c] * 255 + 32767) /
                                65535;

                if (mesa != scalar->rgb[px / SGI_RENDER_PROBE_SIZE]
                                       [px % SGI_RENDER_PROBE_SIZE][c]) {
                    return SGI_RENDER_UNSUPPORTED;
                }
            }
        }
    }
    return SGI_RENDER_OK;
}

static SGIRenderResult sgi_render_probe_snapshot(
    void *opaque, const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp)
{
    /* The probe starts from black, as sgi_render_probe_load() reads it. */
    memset(slots, 0, capacity_slots * sizeof(*slots));
    return SGI_RENDER_OK;
}

static SGIRenderCommitResult sgi_render_probe_commit(
    void *opaque, const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, const SGIRenderWriteCoverage *writes,
    Error **errp)
{
    SGIRenderProbe *p = opaque;

    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            if (writes->pixel[y * writes->stride_bytes + x]) {
                p->written[y][x] = 1;
                memcpy(p->rgb[y][x], &slots[y * stride_slots + x * 3],
                       sizeof(p->rgb[y][x]));
            }
        }
    }
    return (SGIRenderCommitResult) {
        .status = SGI_RENDER_OK,
        .complete_pixels = (uint64_t)(rect->x1 - rect->x0 + 1) *
                           (rect->y1 - rect->y0 + 1),
        .extent_known = true,
    };
}

/*
 * Smooth triangles draw exactly only where llvmpipe evaluates the fragment
 * program's doubles as IEEE binary64 and the scalar rasterizer's own
 * arithmetic is not contracted into fused operations, as on the pinned
 * x86-64 build the unit tests prove. Draw perspective-weighted smooth
 * triangles with a shared edge through pixel centres through both paths,
 * and report UNSUPPORTED if coverage or any covered pixel's colour differs.
 * A host failure is returned as it is.
 */
static SGIRenderResult sgi_render_window_probe(SGIRenderer *renderer,
                                               Error **errp)
{
    static const SGIRasterRules rules = {
        .subpixel_bits = 8, .color_bits = 8, .depth_bits = 24,
        .rounding = SGI_ROUND_HALF_AWAY,
        .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
        .arith = SGI_ARITH_DOUBLE, .line_rule = SGI_LINE_COVERAGE_DOUBLE,
        .evidence = "shared smooth triangle probe",
    };
    /*
     * Window x, y, clip w, normalized device z, then colour, for each
     * vertex. The last triangle has an edge in the near plane through pixel
     * centres, where the scalar depth sum drops pixels.
     */
    static const float probe[][8] = {
        { 1.5f, 1.5f, 1, 0, 0.9f, 0.1f, 0.3f, 1 },
        { 21.5f, 21.5f, 2, 0, 0.2f, 0.75f, 0.5f, 1 },
        { 1.5f, 21.5f, 0.5f, 0, 0.6f, 0.4f, 0.98f, 1 },
        { 21.5f, 21.5f, 2, 0, 0.31f, 0.77f, 0.05f, 1 },
        { 1.5f, 1.5f, 1, 0, 0.5f, 0.5f, 0.5f, 1 },
        { 21.5f, 1.5f, 0.5f, 0, 0.1f, 0.9f, 0.7f, 1 },
        { 2.25f, 7, 1, 0, 0.13f, 0.47f, 0.92f, 1 },
        { 22, 9.5f, 0.75f, 0, 0.88f, 0.02f, 0.35f, 1 },
        { 7.75f, 23.25f, 1.5f, 0, 0.4f, 0.6f, 0.2f, 1 },
        { 13.25f, 7.75f, 1, -0.37f, 0.7f, 0.2f, 0.4f, 1 },
        { 2.5f, 1.5f, 1, -1, 0.5f, 0.5f, 0.9f, 1 },
        { 2.5f, 14.5f, 1, -1, 0.3f, 0.8f, 0.1f, 1 },
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
    float v[ARRAY_SIZE(probe)][SGI_GL_VERTEX_FLOATS] = { { 0 } };
    SGIGLDraw draw = {
        .mode = SGI_GL_TRIANGLES, .count = ARRAY_SIZE(probe),
        .vertices = &v[0][0], .state = &st,
    };
    const SGIRenderResource resource = {
        .role = SGI_RENDER_COLOUR, .buffer = SGI_RENDER_FRONT,
        .width = SGI_RENDER_PROBE_SIZE, .height = SGI_RENDER_PROBE_SIZE,
        .depth = 1, .bottom_origin = true, .component_count = 3,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
        },
    };
    static const SGIRenderResourceOps ops = {
        .snapshot = sgi_render_probe_snapshot,
        .commit_covered = sgi_render_probe_commit,
    };
    g_autofree SGIRenderProbe *scalar = g_new0(SGIRenderProbe, 1);
    g_autofree SGIRenderProbe *mesa = g_new0(SGIRenderProbe, 1);
    SGIRasterTarget scalar_target = {
        .opaque = scalar,
        .store_rgba = sgi_render_probe_store,
        .load_rgba = sgi_render_probe_load,
    };
    SGIRasterTarget mesa_target = {
        .opaque = mesa, .resources = &resource, .resource_count = 1,
        .resource_ops = &ops,
    };
    SGIRenderResult status;

    for (unsigned i = 0; i < 16; i += 5) {
        st.modelview[i] = 1;
        st.projection[i] = 1;
    }
    for (size_t i = 0; i < ARRAY_SIZE(probe); i++) {
        float w = probe[i][2];

        v[i][SGI_GL_V_POSITION] = (probe[i][0] * 2 / SGI_RENDER_PROBE_SIZE -
                                   1) * w;
        v[i][SGI_GL_V_POSITION + 1] = (probe[i][1] * 2 /
                                       SGI_RENDER_PROBE_SIZE - 1) * w;
        v[i][SGI_GL_V_POSITION + 2] = probe[i][3] * w;
        v[i][SGI_GL_V_POSITION + 3] = w;
        memcpy(&v[i][SGI_GL_V_COLOR], &probe[i][4], 4 * sizeof(float));
    }
    sgi_soft_draw(&rules, &scalar_target, &draw);
    status = sgi_render_draw_checked(renderer, &rules, &mesa_target, &draw,
                                     errp);
    if (status != SGI_RENDER_OK) {
        return status;
    }
    if (memcmp(scalar, mesa, sizeof(*scalar))) {
        error_setg(errp, "SGI Mesa smooth triangles differ from the scalar "
                   "path on this host");
        return SGI_RENDER_UNSUPPORTED;
    }
    return SGI_RENDER_OK;
}

/*
 * Run the smooth triangle probe once per renderer. It draws through this
 * path itself, so its own draw finds the probe running and goes ahead.
 */
static SGIRenderResult sgi_render_window_proven(SGIRenderer *renderer,
                                                Error **errp)
{
    SGIRenderResult status;

    switch (sgi_renderer_window_probe_result(renderer)) {
    case 0:
        sgi_renderer_set_window_probe_result(renderer, 2);
        status = sgi_render_window_probe(renderer, errp);
        if (status == SGI_RENDER_OK || status == SGI_RENDER_UNSUPPORTED) {
            sgi_renderer_set_window_probe_result(renderer,
                status == SGI_RENDER_OK ? 1 : -1);
        } else {
            sgi_renderer_set_window_probe_result(renderer, 0);
        }
        return status;
    case -1:
        error_setg(errp, "SGI Mesa smooth triangles differ from the scalar "
                   "path on this host");
        return SGI_RENDER_UNSUPPORTED;
    default:
        return SGI_RENDER_OK;
    }
}

SGIRenderResult sgi_render_test_window_probe(SGIRenderer *renderer,
                                             Error **errp)
{
    return sgi_render_window_proven(renderer, errp);
}

/*
 * Run the line probe once per renderer. A host without line support, or
 * whose lines differ, refuses lines; a host failure in the probe is
 * returned as that failure, and the probe runs again next time.
 */
static SGIRenderResult sgi_render_lines_proven(SGIRenderer *renderer,
                                               SGIRenderRefusal *reason,
                                               Error **errp)
{
    SGIRenderResult status;

    if (!sgi_renderer_primitive_supported(renderer, SGI_RENDERER_LINES, 1,
                                          errp)) {
        *reason = SGI_RENDER_REFUSAL_HOST_PROGRAM;
        return SGI_RENDER_UNSUPPORTED;
    }
    if (!sgi_renderer_line_probe_result(renderer)) {
        status = sgi_render_line_probe(renderer, errp);
        if (status != SGI_RENDER_OK && status != SGI_RENDER_UNSUPPORTED) {
            return status;
        }
        sgi_renderer_set_line_probe_result(renderer,
            status == SGI_RENDER_OK ? 1 : -1);
    }
    if (sgi_renderer_line_probe_result(renderer) < 0) {
        error_setg(errp, "SGI Mesa lines differ from the scalar path on "
                   "this host");
        *reason = SGI_RENDER_REFUSAL_HOST_PROBE;
        return SGI_RENDER_UNSUPPORTED;
    }
    return SGI_RENDER_OK;
}

/* Count a line draw's stipple into its borrowed phase, as the scalar does. */
static void sgi_render_commit_phase(const SGIGLDraw *draw,
                                    const SGIRenderLines *lines)
{
    if (lines && draw->line_phase && lines->consumed) {
        *draw->line_phase = lines->counter;
    }
}

static SGIRenderResult sgi_render_refuse(SGIRenderRefusal *reason,
                                         SGIRenderRefusal why)
{
    *reason = why;
    return SGI_RENDER_UNSUPPORTED;
}

/* The proven widths: scalar-match-widths and the 8-bit cases. */
static bool sgi_render_mixed_width(unsigned bits)
{
    return bits == 1 || bits == 2 || bits == 4 || bits == 5 || bits == 6 ||
           bits == 8 || bits == 10;
}

/*
 * The fixture the point, blend and texture comparisons draw in: one clip
 * covering a bottom-origin resource, no scissor, the viewport equal to the
 * resource and identity matrices.
 */
static bool sgi_render_mixed_fixture(const SGIGLDrawState *st,
                                     const SGIRenderResource *res)
{
    const SGIClipRect *clip = &st->drawable.clip.r[0];

    if (!res->bottom_origin || st->drawable.clip.n != 1 || clip->x0 > 0 ||
        clip->y0 > 0 || clip->x1 < (int64_t)res->width - 1 ||
        clip->y1 < (int64_t)res->height - 1 || st->scissor_test ||
        st->viewport[0] || st->viewport[1] ||
        st->viewport[2] != (int64_t)res->width ||
        st->viewport[3] != (int64_t)res->height) {
        return false;
    }
    for (unsigned i = 0; i < 16; i++) {
        float identity = i % 5 == 0;

        if (st->modelview[i] != identity || st->projection[i] != identity) {
            return false;
        }
    }
    return true;
}

/*
 * The front index resource of an indexed draw: one unsigned scalar of the
 * draw's index width. An indexed draw never selects a colour resource.
 */
static const SGIRenderResource *sgi_render_index_resource(
    const SGIRasterTarget *target, const SGIGLDrawState *st)
{
    const SGIRenderResource *found = NULL;

    for (size_t i = 0; target->resources && i < target->resource_count;
         i++) {
        const SGIRenderResource *res = &target->resources[i];

        if (res->role == SGI_RENDER_INDEX &&
            res->target == st->drawable.front) {
            if (found) {
                return NULL;
            }
            found = res;
        }
    }
    if (!found || !found->width || !found->height ||
        found->width > INT_MAX || found->height > INT_MAX ||
        found->depth != 1 || found->buffer != SGI_RENDER_FRONT ||
        found->component_count != 1 ||
        found->component[0].channel != SGI_RENDER_SCALAR ||
        found->component[0].numeric != SGI_RENDER_UINT) {
        return NULL;
    }
    return found;
}

/*
 * Colour-index draws, inside the mixed-frame set only: points of sizes up
 * to 4 and filled polygons, into the front index resource whose storage
 * width is the index width, in the comparison fixture under the rules the
 * set names. Shading is flat, or smooth with every vertex index bitwise
 * equal and no vertex outside the view volume: the scalar path gives a
 * clipped vertex an interpolated index that need not floor to the shared
 * one. The scalar path ignores blending, the alpha test, the colour mask,
 * the RGBA logical operation and dithering in index mode, and so does
 * this path. Everything else is refused for the scalar path to draw.
 */
static SGIRenderRefusal sgi_render_index_supported(
    const SGIRasterRules *rules, const SGIRasterTarget *target,
    const SGIGLDraw *draw, const SGIRenderResource **result)
{
    const SGIGLDrawState *st = draw->state;
    const SGIRenderResource *res;
    SGIRenderPieces check = { .rules = rules };

    if (draw->mode > SGI_GL_POLYGON || sgi_render_mode_lines(draw->mode) ||
        (draw->mode == SGI_GL_POINTS &&
         (!isfinite(draw->point_size) || draw->point_size < 0)) ||
        (sgi_render_mode_polygons(draw->mode) && draw->polygon_modes &&
         (draw->polygon_modes[0] != SGI_GL_FILL ||
          draw->polygon_modes[1] != SGI_GL_FILL)) ||
        draw->polygon_stipple_enabled || draw->line_stipple_enabled ||
        (st->shade_model != SGI_GL_SMOOTH &&
         st->shade_model != SGI_GL_FLAT)) {
        return SGI_RENDER_REFUSAL_PRIMITIVE;
    }
    if ((draw->mode == SGI_GL_POINTS && rules->smooth_primitives &&
         draw->point_smooth) ||
        (sgi_render_mode_polygons(draw->mode) && rules->smooth_primitives &&
         draw->polygon_smooth)) {
        return SGI_RENDER_REFUSAL_SMOOTHING;
    }
    if (st->texture_1d || st->texture_2d || st->texture_3d) {
        return SGI_RENDER_REFUSAL_TEXTURE;
    }
    if (draw->back_colors || st->cull_face) {
        return SGI_RENDER_REFUSAL_FACING;
    }
    if (st->draw_buffers != SGI_GL_DRAW_FRONT) {
        return SGI_RENDER_REFUSAL_BUFFERS;
    }
    if (st->depth_test || st->stencil_test) {
        return SGI_RENDER_REFUSAL_FRAGMENT_TESTS;
    }
    if (st->fog) {
        return SGI_RENDER_REFUSAL_FOG;
    }
    if (st->clip_planes) {
        return SGI_RENDER_REFUSAL_CLIP_PLANES;
    }
    if (st->index_logic_op) {
        return SGI_RENDER_REFUSAL_LOGIC_OP;
    }
    if (st->polygon_offset) {
        return SGI_RENDER_REFUSAL_POLYGON_OFFSET;
    }
    if (st->window_matrix_given || !st->drawable.bound ||
        st->drawable.origin[0] || st->drawable.origin[1]) {
        return SGI_RENDER_REFUSAL_WINDOW;
    }
    if (sgi_render_depth_departs(rules, st)) {
        return SGI_RENDER_REFUSAL_RULES;
    }
    if (st->viewport[2] < 0 || st->viewport[3] < 0) {
        return SGI_RENDER_REFUSAL_GEOMETRY;
    }
    for (unsigned i = 0; i < 16; i++) {
        if (!isfinite(st->modelview[i]) || !isfinite(st->projection[i])) {
            return SGI_RENDER_REFUSAL_GEOMETRY;
        }
    }
    for (uint32_t i = 0; i < draw->count; i++) {
        const float *vertex = draw->vertices +
                              (size_t)i * SGI_GL_VERTEX_FLOATS;

        for (unsigned c = 0; c < 4; c++) {
            if (!isfinite(vertex[SGI_GL_V_POSITION + c])) {
                return SGI_RENDER_REFUSAL_GEOMETRY;
            }
        }
        if (!isfinite(vertex[SGI_GL_V_INDEX])) {
            return SGI_RENDER_REFUSAL_GEOMETRY;
        }
    }
    res = sgi_render_index_resource(target, st);
    if (!res) {
        return SGI_RENDER_REFUSAL_RESOURCE;
    }
    if (res->component[0].storage_bits != st->index_bits) {
        return SGI_RENDER_REFUSAL_STORE_WIDTH;
    }
    if (!sgi_raster_rules_supported(rules)) {
        return SGI_RENDER_REFUSAL_RULES;
    }
    /* State outside the mixed-frame set, as the RGBA path names it. */
    if (!sgi_render_mixed_fixture(st, res) ||
        (draw->mode == SGI_GL_POINTS &&
         sgi_render_aliased_size(draw->point_size) > 4)) {
        return SGI_RENDER_REFUSAL_MIXED_FRAME;
    }
    if (st->shade_model == SGI_GL_SMOOTH) {
        for (uint32_t i = 0; i < draw->count; i++) {
            SGIVolumeVertex v;

            if (memcmp(&draw->vertices[(size_t)i * SGI_GL_VERTEX_FLOATS +
                                       SGI_GL_V_INDEX],
                       &draw->vertices[SGI_GL_V_INDEX], sizeof(float))) {
                return SGI_RENDER_REFUSAL_COLOUR_INDEX;
            }
            sgi_volume_from_draw_vertex(st, draw->vertices +
                                        (size_t)i * SGI_GL_VERTEX_FLOATS,
                                        &v);
            if (!sgi_volume_contains(&v)) {
                return SGI_RENDER_REFUSAL_INDEX_CLIPPED;
            }
        }
    }
    /*
     * The integer program has no depth drop, so a triangle whose depth
     * the scalar path may round past a plane stays there.
     */
    if (draw->mode != SGI_GL_POINTS) {
        sgi_render_decompose(&check, draw);
        if (check.depth_inexact) {
            return SGI_RENDER_REFUSAL_GEOMETRY;
        }
    }
    *result = res;
    return SGI_RENDER_REFUSAL_NONE;
}

/*
 * Whether a draw lies inside the set the scalar comparisons prove
 * pixel-identical on the pinned llvmpipe, so that a frame mixing it with
 * scalar draws equals an all-scalar frame. Each limit names what is not
 * yet proven:
 *  - rules the scalar path implements, at proven widths;
 *  - lines at 8 bits and widths up to 6, under any accepted state;
 *  - points of sizes up to 4, unblended, in the fixture;
 *  - untextured polygons under any accepted state, blended only in the
 *    fixture;
 *  - textured triangles under any accepted state, with images the draw
 *    path accepts of at most 16 by 16 texels.
 */
static bool sgi_render_mixed_frame_proven(const SGIRasterRules *rules,
                                          const SGIRasterTarget *target,
                                          const SGIGLDraw *draw,
                                          const SGIRenderResource *res)
{
    const SGIGLDrawState *st = draw->state;

    if (!sgi_raster_rules_supported(rules) ||
        !sgi_render_mixed_width(rules->color_bits) ||
        (st->texture_2d &&
         (!sgi_render_texture_image_supported(target->texture, st) ||
          target->texture->width > 16 ||
          target->texture->height > 16))) {
        return false;
    }
    if (sgi_render_mode_lines(draw->mode)) {
        return rules->color_bits == 8 &&
               sgi_render_aliased_size(draw->line_width) <= 6;
    }
    if (draw->mode == SGI_GL_POINTS) {
        return !st->blend && sgi_render_aliased_size(draw->point_size) <= 4 &&
               sgi_render_mixed_fixture(st, res);
    }
    return !st->blend || sgi_render_mixed_fixture(st, res);
}

/*
 * The state checks a checked draw makes before any host work, in its
 * order: the refusal it returns for the draw's state, or NONE with the
 * colour resource found. With mixed_frame it also refuses state outside
 * the proven set. The in-set tag calls it too, so the tag and the draw
 * path cannot disagree on what the mixed-frame check accepts.
 */
static SGIRenderRefusal sgi_render_draw_accepts(
    const SGIRasterRules *rules, const SGIRasterTarget *target,
    const SGIGLDraw *draw, bool mixed_frame,
    const SGIRenderResource **resource)
{
    SGIRenderRefusal refusal;

    /* An index is never dithered. */
    if (!draw->state->index_bits && draw->state->dither &&
        rules->dither != SGI_DITHER_NONE) {
        return SGI_RENDER_REFUSAL_DITHER;
    }
    if (!sgi_render_rules_supported(rules)) {
        return SGI_RENDER_REFUSAL_RULES;
    }
    /* The indexed subset lies inside the mixed-frame set already. */
    if (draw->state->index_bits) {
        return sgi_render_index_supported(rules, target, draw, resource);
    }
    refusal = sgi_render_draw_supported(rules, target, draw, resource);
    if (refusal != SGI_RENDER_REFUSAL_NONE) {
        return refusal;
    }
    if (mixed_frame &&
        !sgi_render_mixed_frame_proven(rules, target, draw, *resource)) {
        return SGI_RENDER_REFUSAL_MIXED_FRAME;
    }
    return SGI_RENDER_REFUSAL_NONE;
}

static size_t sgi_render_signature_limit = SIZE_MAX;

void sgi_render_test_signature_limit(size_t length)
{
    sgi_render_signature_limit = length ? length : SIZE_MAX;
}

static const char *sgi_render_rule_name(const char *const *names, size_t n,
                                        unsigned value)
{
    return value < n && names[value] ? names[value] : "unknown";
}

/* scalar-match-clear proves CLEAR at 4, 8 and 10 bits. */
static bool sgi_render_clear_mixed_proven(const SGIRasterRules *rules)
{
    return sgi_raster_rules_supported(rules) &&
           (rules->color_bits == 4 || rules->color_bits == 8 ||
            rules->color_bits == 10);
}

/* Whether any clipped vertex of the draw lies within the depth margin. */
static bool sgi_render_draw_near_plane(const SGIRasterRules *rules,
                                       const SGIGLDraw *draw)
{
    if (sgi_render_mode_polygons(draw->mode)) {
        SGIRenderPieces check = { .rules = rules };

        sgi_render_decompose(&check, draw);
        return check.near_plane;
    }
    for (uint32_t i = 0; i < draw->count; i++) {
        SGIVolumeVertex v;
        double z;

        sgi_volume_from_draw_vertex(draw->state,
            draw->vertices + (size_t)i * SGI_GL_VERTEX_FLOATS, &v);
        if (!(v.clip[3] > 0)) {
            continue;
        }
        z = (v.clip[2] / v.clip[3] + 1) / 2;
        if (z <= SGI_RENDER_DEPTH_MARGIN || z >= 1 - SGI_RENDER_DEPTH_MARGIN) {
            return true;
        }
    }
    return false;
}

void sgi_render_draw_signature(const SGIRasterRules *rules,
                               const SGIRasterTarget *target,
                               const SGIGLDraw *draw,
                               SGIRenderSignature *signature)
{
    const SGIGLDrawState *st = draw->state;
    /* An indexed draw's storage is its index resource. */
    const SGIRenderResource *res = !st ? NULL :
        st->index_bits ? sgi_render_index_resource(target, st) :
        sgi_render_colour_resource(target, st->drawable.front);
    const SGIGLTexImage *image = st && st->texture_2d ? target->texture :
                                 NULL;
    const SGIRenderResource *proven;
    const char *class = sgi_render_mode_lines(draw->mode) ? "line" :
                        draw->mode == SGI_GL_POINTS ? "point" : "polygon";
    /* Names, not enum values, so renumbering cannot change a signature. */
    static const char *const rounding_names[] = {
        [SGI_ROUND_FLOOR] = "floor",
        [SGI_ROUND_HALF_UP] = "half-up",
        [SGI_ROUND_HALF_EVEN] = "half-even",
        [SGI_ROUND_HALF_AWAY] = "half-away",
    };
    static const char *const tie_names[] = {
        [SGI_TIE_TOP_LEFT] = "top-left",
        [SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN] = "top-left-bottom-origin",
    };
    static const char *const arith_names[] = {
        [SGI_ARITH_FIXED16] = "fixed16",
        [SGI_ARITH_DOUBLE] = "double",
    };
    unsigned widths[4] = { 0 };
    bool matrices = true, clips = false;
    int length;

    memset(signature, 0, sizeof(*signature));
    signature->version = SGI_RENDER_SIGNATURE_VERSION;
    if (!st) {
        g_strlcpy(signature->text, SGI_RENDER_SIGNATURE_PREFIX "invalid",
                  sizeof(signature->text));
        return;
    }
    for (unsigned c = 0; res && c < res->component_count && c < 4; c++) {
        widths[c] = res->component[c].storage_bits;
    }
    for (unsigned i = 0; i < 16; i++) {
        float identity = i % 5 == 0;

        matrices &= st->modelview[i] == identity &&
                    st->projection[i] == identity;
    }
    if (res && st->drawable.clip.n == 1) {
        const SGIClipRect *clip = &st->drawable.clip.r[0];

        clips = clip->x0 <= 0 && clip->y0 <= 0 &&
                clip->x1 >= (int64_t)res->width - 1 &&
                clip->y1 >= (int64_t)res->height - 1;
    }
    length = g_snprintf(signature->text, sizeof(signature->text),
               "v%u %s point=%g line=%g stipple=%u/%u shade=0x%x "
               "blend=%u/0x%x/0x%x/0x%x texture=%u/0x%x/0x%x/0x%x/0x%x/"
               "0x%x/%ux%u rules=%u/%s/%s/%s/%u widths=%u/%u/%u/%u "
               "rows=%s fixture=%u/%u/%u/%u near-plane=%u index=%u",
               SGI_RENDER_SIGNATURE_VERSION, class,
               draw->mode == SGI_GL_POINTS ?
                   sgi_render_aliased_size(draw->point_size) : 0,
               sgi_render_mode_lines(draw->mode) ?
                   sgi_render_aliased_size(draw->line_width) : 0,
               draw->line_stipple_enabled, draw->polygon_stipple_enabled,
               st->shade_model, st->blend,
               st->blend ? st->blend_equation : 0,
               st->blend ? st->blend_src : 0,
               st->blend ? st->blend_dst : 0,
               st->texture_2d, image ? st->texture_env_mode : 0,
               image ? image->min_filter : 0, image ? image->mag_filter : 0,
               image ? image->wrap_s : 0, image ? image->wrap_t : 0,
               image ? image->width : 0, image ? image->height : 0,
               rules->subpixel_bits,
               sgi_render_rule_name(rounding_names,
                                    ARRAY_SIZE(rounding_names),
                                    rules->rounding),
               sgi_render_rule_name(tie_names, ARRAY_SIZE(tie_names),
                                    rules->tie_rule),
               sgi_render_rule_name(arith_names, ARRAY_SIZE(arith_names),
                                    rules->arith),
               rules->smooth_primitives,
               widths[0], widths[1], widths[2], widths[3],
               !res ? "none" : res->bottom_origin ? "bottom" : "top",
               res && !st->viewport[0] && !st->viewport[1] &&
                   st->viewport[2] == (int64_t)res->width &&
                   st->viewport[3] == (int64_t)res->height,
               !st->scissor_test, clips, matrices,
               draw->vertices && sgi_render_draw_near_plane(rules, draw),
               st->index_bits);
    /* Truncation would merge distinct signatures; count them as one. */
    if (length >= MIN(sizeof(signature->text), sgi_render_signature_limit)) {
        g_strlcpy(signature->text, SGI_RENDER_SIGNATURE_PREFIX "overflow",
                  sizeof(signature->text));
    }
    signature->in_set = draw->vertices &&
        sgi_render_draw_accepts(rules, target, draw, true, &proven) ==
            SGI_RENDER_REFUSAL_NONE;
}

void sgi_render_raster_signature(const SGIRasterRules *rules,
                                 const SGIRasterTarget *target,
                                 const SGIRasterOp *op,
                                 SGIRenderSignature *signature)
{
    const SGIRenderResource *res = op->ntargets ?
        sgi_render_colour_resource(target, op->targets[0]) : NULL;
    bool colour = (op->clear_buffers & SGI_GL_COLOR_BUFFER_BIT) &&
                  op->clear_colour_mask;
    bool depth = op->clear_buffers & SGI_GL_DEPTH_BUFFER_BIT;
    bool stencil = (op->clear_buffers & SGI_GL_STENCIL_BUFFER_BIT) &&
                   (op->stencil_mask & 0xff);
    unsigned widths[4] = { 0 };
    bool in_set;

    memset(signature, 0, sizeof(*signature));
    signature->version = SGI_RENDER_SIGNATURE_VERSION;
    for (unsigned c = 0; res && c < res->component_count && c < 4; c++) {
        widths[c] = res->component[c].storage_bits;
    }
    if (g_snprintf(signature->text, sizeof(signature->text),
                   "v%u %s buffers=0x%x index=%u widths=%u/%u/%u/%u "
                   "rows=%s rules=%u/%u/%u", SGI_RENDER_SIGNATURE_VERSION,
                   op->kind == SGI_RASTER_CLEAR ? "clear" : "raster-op",
                   op->clear_buffers, op->index_bits, widths[0], widths[1],
                   widths[2], widths[3],
                   !res ? "none" : res->bottom_origin ? "bottom" : "top",
                   rules->color_bits, rules->depth_bits,
                   rules->stencil_bits) >=
        MIN(sizeof(signature->text), sgi_render_signature_limit)) {
        g_strlcpy(signature->text, SGI_RENDER_SIGNATURE_PREFIX "overflow",
                  sizeof(signature->text));
    }
    /* What sgi_render_raster_run() accepts with the mixed-frame flag. */
    in_set = op->kind == SGI_RASTER_CLEAR && !op->index_bits &&
             !(op->clear_buffers & ~(SGI_GL_COLOR_BUFFER_BIT |
                                     SGI_GL_DEPTH_BUFFER_BIT |
                                     SGI_GL_STENCIL_BUFFER_BIT)) &&
             (colour || depth || stencil) &&
             (!depth || rules->depth_bits == 24) &&
             (!stencil || rules->stencil_bits == 8) &&
             sgi_render_clear_mixed_proven(rules);
    for (unsigned i = 0; in_set && colour && i < op->ntargets; i++) {
        const SGIRenderResource *t = sgi_render_colour_resource(
            target, op->targets[i]);

        in_set = t && t->bottom_origin && t->depth == 1 &&
                 (t->component_count == 3 || t->component_count == 4) &&
                 sgi_render_widths_proven(rules, t);
    }
    signature->in_set = in_set;
}

/*
 * A checked draw. Every UNSUPPORTED result before the snapshot sets
 * *reason; one without a reason came from native work and is a failure.
 */
static SGIRenderResult sgi_render_draw_run(
    SGIRenderer *renderer, const SGIRasterRules *rules,
    const SGIRasterTarget *target, const SGIGLDraw *draw,
    const SGIRenderAttemptOptions *opts, SGIRenderRefusal *reason,
    Error **errp)
{
    const SGIRenderResource *resource;
    const SGIRenderResource *texture_resource = NULL;
    const SGIClipList *clip;
    SGIRenderRect rect;
    SGIRendererFrame frame = { 0 };
    g_autofree uint32_t *slots = NULL;
    g_autofree uint16_t *rgba = NULL;
    g_autofree uint8_t *written = NULL;
    g_autofree uint32_t *texture_slots = NULL;
    size_t pixels, nslots, nrgba;
    size_t texture_stride_slots = 0, texture_capacity_slots = 0;
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
    SGIRenderRefusal refusal;
    SGIRendererClearRect region;
    SGIRenderExtent extent;
    uint32_t region_width, region_height;
    unsigned kept;
    bool flipped;
    bool indexed;
    uint32_t effective_mask = 0;
    g_autofree uint32_t *index_out = NULL;
    g_autofree uint32_t *piece_index = NULL;

    if (!renderer || !rules || !target || !draw || !draw->state ||
        !draw->vertices) {
        error_setg(errp, "Missing SGI Mesa draw input");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    indexed = draw->state->index_bits != 0;
    clip = &draw->state->drawable.clip;
    if (!draw->state->drawable.bound) {
        if (!opts || !opts->quiet) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: draw with no window bound "
                          "dropped\n");
        }
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
    /* Indexed draws are accepted only inside the mixed-frame set. */
    if (((opts && opts->mixed_frame) || indexed) &&
        !sgi_renderer_identity(renderer)->reference) {
        error_setg(errp, "SGI Mesa mixed frames and colour-index draws are "
                   "proven only on the reference llvmpipe build");
        return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_HOST_DRIVER);
    }
    refusal = sgi_render_draw_accepts(rules, target, draw,
                                      opts && opts->mixed_frame, &resource);
    switch (refusal) {
    case SGI_RENDER_REFUSAL_NONE:
        break;
    case SGI_RENDER_REFUSAL_DITHER:
        error_setg(errp, "SGI Mesa ordered dithering is unsupported");
        return sgi_render_refuse(reason, refusal);
    case SGI_RENDER_REFUSAL_RULES:
        error_setg(errp, "Unsupported SGI Mesa raster rules");
        return sgi_render_refuse(reason, refusal);
    case SGI_RENDER_REFUSAL_MIXED_FRAME:
        error_setg(errp, "SGI Mesa draw state is not proven identical to "
                   "the scalar path for a mixed frame");
        return sgi_render_refuse(reason, refusal);
    default:
        error_setg(errp, "Unsupported SGI Mesa draw state or resource (%s)",
                   sgi_render_refusal_name(refusal));
        return sgi_render_refuse(reason, refusal);
    }
    for (unsigned c = 0; !indexed && c < resource->component_count; c++) {
        steps[c] = (1u << resource->component[c].storage_bits) - 1;
    }
    if (indexed) {
        unsigned bits = resource->component[0].storage_bits;

        effective_mask = draw->state->index_mask &
                         (bits >= 32 ? UINT32_MAX : (1u << bits) - 1);
    } else if (draw->state->blend) {
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
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_BLEND_OVERLAP);
        }
        status = sgi_render_lines_proven(renderer, reason, errp);
        if (status != SGI_RENDER_OK) {
            return status;
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
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_LINE_SHAPE);
        }
    } else {
        piece_vertices = sgi_render_piece_vertices(draw->mode, draw->count);
        if (!piece_vertices) {
            return SGI_RENDER_OK;
        }
        if (piece_vertices > SGI_RENDERER_FRAME_PRIMITIVE_VERTICES_MAX) {
            error_setg(errp, "SGI Mesa draw has too many vertices");
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_HOST_STAGING);
        }
        /*
         * The float target rounds only at readback, so a second
         * overlapping primitive would blend against an unrounded value
         * where the scalar path blends against the stored one. An 8-bit
         * target rounds per primitive but llvmpipe's fixed-point blend
         * then differs from the scalar path for single primitives.
         */
        if (draw->state->blend && !indexed &&
            piece_vertices > (draw->mode == SGI_GL_POINTS ? 1 : 3)) {
            error_setg(errp, "SGI Mesa blending of more than one primitive "
                       "in a draw is unsupported");
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_BLEND_OVERLAP);
        }
        if (draw->mode == SGI_GL_POINTS) {
            point_size = sgi_render_aliased_size(draw->point_size);
            if (!sgi_renderer_primitive_supported(renderer,
                                                  SGI_RENDERER_POINTS,
                                                  point_size, errp)) {
                return sgi_render_refuse(reason,
                                         SGI_RENDER_REFUSAL_HOST_POINT_SIZE);
            }
        }
        /*
         * Smooth colour and texture coordinates come from the scalar path's
         * own clipped triangles, drawn at its window positions and
         * interpolated in double.
         */
        window = !indexed && draw->mode != SGI_GL_POINTS &&
                 (draw->state->shade_model == SGI_GL_SMOOTH ||
                  draw->state->texture_2d);
        /*
         * So are flat triangles whose depth may round past a plane; the
         * indexed preflight has refused those.
         */
        if (!window && !indexed && draw->mode != SGI_GL_POINTS) {
            SGIRenderPieces check = { .rules = rules };

            sgi_render_decompose(&check, draw);
            window = check.depth_inexact;
        }
    }
    if (window) {
        const double reach = (double)SGI_RENDERER_WINDOW_REACH * 256;

        if (!sgi_renderer_primitive_supported(renderer,
                                              SGI_RENDERER_WINDOW_TRIANGLES,
                                              1, errp)) {
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_HOST_PROGRAM);
        }
        status = sgi_render_window_proven(renderer, errp);
        if (status == SGI_RENDER_UNSUPPORTED) {
            *reason = SGI_RENDER_REFUSAL_HOST_PROBE;
        }
        if (status != SGI_RENDER_OK) {
            return status;
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
        pieces.image = target->texture;
        pieces.flat = draw->state->shade_model == SGI_GL_FLAT;
        sgi_render_decompose(&pieces, draw);
        if (pieces.window_count > pieces.window_capacity) {
            error_setg(errp, "SGI Mesa draw has too many triangles");
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_HOST_STAGING);
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
                    return sgi_render_refuse(reason,
                                             SGI_RENDER_REFUSAL_GEOMETRY);
                }
            }
        }
    }
    if (draw->state->texture_2d) {
        /*
         * The scalar path drops the whole triangle when any vertex of its
         * clipped polygon cannot be textured, even one only in fan pieces
         * it would drop anyway, so refuse and let it.
         */
        if (pieces.texture_invalid) {
            error_setg(errp, "Unsupported SGI Mesa clipped texture "
                       "coordinates");
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_GEOMETRY);
        }
        texture_resource = sgi_render_texture_resource(target, draw->state);
        if (!texture_resource) {
            error_setg(errp,
                       "Unsupported SGI Mesa texture resource or sampler");
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_TEXTURE);
        }
        status = sgi_render_texture_span(texture_resource,
                                         &texture_stride_slots,
                                         &texture_capacity_slots, errp);
        if (status == SGI_RENDER_UNSUPPORTED) {
            *reason = SGI_RENDER_REFUSAL_HOST_STAGING;
        }
        if (status != SGI_RENDER_OK) {
            return status;
        }
        if (!sgi_renderer_texture_supported(renderer, errp)) {
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_HOST_PROGRAM);
        }
    }
    if (!sgi_renderer_viewport_supported(renderer, draw->state->viewport,
                                         errp)) {
        return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_HOST_VIEWPORT);
    }
    if (!target->resource_ops || !target->resource_ops->snapshot ||
        (indexed ? !target->resource_ops->commit_index_covered :
                   !target->resource_ops->commit_covered)) {
        error_setg(errp, "SGI Mesa draw needs snapshot and %s commit "
                   "providers", indexed ? "index" : "covered");
        return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_PROVIDER);
    }
    /*
     * A borrowed phase is written only once the draw can no longer be
     * refused, so a refused draw leaves it for the caller's scalar
     * fallback to count once.
     */
    if (lines && !line_set.count) {
        sgi_render_commit_phase(draw, &line_set);
        return SGI_RENDER_OK;
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
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_GEOMETRY);
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
        sgi_render_commit_phase(draw, lines ? &line_set : NULL);
        return SGI_RENDER_OK;
    }
    /*
     * Only the region the draw can write moves between native storage and
     * the host: the clip union, bounded by the geometry's window extent.
     * The geometry, viewport and clips are unchanged, so what the host
     * draws is too; pixels outside the region are never written.
     */
    region = frame.clips[0];
    for (unsigned i = 1; i < frame.clip_count; i++) {
        region.x0 = MIN(region.x0, frame.clips[i].x0);
        region.y0 = MIN(region.y0, frame.clips[i].y0);
        region.x1 = MAX(region.x1, frame.clips[i].x1);
        region.y1 = MAX(region.y1, frame.clips[i].y1);
    }
    extent = (SGIRenderExtent) {
        .known = true, .x0 = INFINITY, .y0 = INFINITY,
        .x1 = -INFINITY, .y1 = -INFINITY,
    };
    if (window) {
        for (uint32_t i = 0; i < pieces.window_count; i++) {
            for (unsigned v = 0; v < 3; v++) {
                sgi_render_extent_add(&extent, window_storage[i].x[v] / 256,
                                      window_storage[i].y[v] / 256, 2);
            }
        }
    } else if (lines) {
        for (uint32_t i = 0; i < line_set.count; i++) {
            for (unsigned v = 0; v < 4; v++) {
                sgi_render_extent_add(&extent, line_set.lines[i].quad[v][0],
                                      line_set.lines[i].quad[v][1], 2);
            }
        }
    } else {
        SGIRenderPieces bound = {
            .extent = &extent, .point_size = point_size,
        };

        sgi_render_decompose(&bound, draw);
    }
    if (extent.known) {
        int64_t x0, y0, x1, y1;

        if (!(extent.x0 <= extent.x1)) {
            /* Nothing to draw: no piece survived clipping. */
            sgi_render_commit_phase(draw, lines ? &line_set : NULL);
            return SGI_RENDER_OK;
        }
        /* Bounded both ways, so every conversion to int64_t is defined. */
        x0 = MIN(MAX(floor(extent.x0), -1.0), (double)resource->width);
        y0 = MIN(MAX(floor(extent.y0), -1.0), (double)resource->height);
        x1 = MIN(MAX(ceil(extent.x1), -1.0), (double)resource->width);
        y1 = MIN(MAX(ceil(extent.y1), -1.0), (double)resource->height);
        if (upside_down) {
            int64_t window_y0 = y0;

            y0 = (int64_t)resource->height - 1 - y1;
            y1 = (int64_t)resource->height - 1 - window_y0;
        }
        /*
         * Intersect in signed arithmetic. Geometry wholly before the first
         * column or row has a negative far edge, which the region's
         * unsigned fields would wrap to a huge one, and the draw would
         * fail to allocate instead of writing nothing.
         */
        x0 = MAX((int64_t)region.x0, x0);
        y0 = MAX((int64_t)region.y0, y0);
        x1 = MIN((int64_t)region.x1, x1);
        y1 = MIN((int64_t)region.y1, y1);
        if (x0 > x1 || y0 > y1) {
            sgi_render_commit_phase(draw, lines ? &line_set : NULL);
            return SGI_RENDER_OK;
        }
        region = (SGIRendererClearRect) { x0, y0, x1, y1 };
        /* Clips stay in the region, so the host never draws outside it. */
        kept = 0;
        for (unsigned i = 0; i < frame.clip_count; i++) {
            SGIRendererClearRect c = frame.clips[i];

            c.x0 = MAX(c.x0, region.x0);
            c.y0 = MAX(c.y0, region.y0);
            c.x1 = MIN(c.x1, region.x1);
            c.y1 = MIN(c.y1, region.y1);
            if (c.x0 <= c.x1 && c.y0 <= c.y1) {
                frame.clips[kept++] = c;
            }
        }
        frame.clip_count = kept;
        if (!frame.clip_count) {
            sgi_render_commit_phase(draw, lines ? &line_set : NULL);
            return SGI_RENDER_OK;
        }
    }
    region_width = region.x1 - region.x0 + 1;
    region_height = region.y1 - region.y0 + 1;
    /* The region's native rows, which may run the other way. */
    flipped = resource->bottom_origin == upside_down;
    rect = (SGIRenderRect) {
        .x0 = region.x0, .x1 = region.x1,
        .y0 = flipped ? resource->height - 1 - region.y1 : region.y0,
        .y1 = flipped ? resource->height - 1 - region.y0 : region.y1,
    };
    if ((uint64_t)region_width * region_height >
            SIZE_MAX / 4 / sizeof(*rgba)) {
        error_setg(errp, "SGI Mesa draw exceeds host address space");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    pixels = (size_t)region_width * region_height;
    if (pixels > SIZE_MAX / resource->component_count) {
        error_setg(errp, "SGI Mesa component array is too large");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    nslots = pixels * resource->component_count;
    nrgba = pixels * 4;
    slots = g_try_new(uint32_t, nslots);
    /* An indexed draw stages integers instead of RGBA. */
    if (indexed) {
        index_out = g_try_new(uint32_t, pixels);
    } else {
        rgba = g_try_new(uint16_t, nrgba);
    }
    written = g_try_new(uint8_t, pixels);
    if (!slots || (indexed ? !index_out : !rgba) || !written) {
        error_setg(errp, "Cannot allocate SGI Mesa draw buffers");
        return SGI_RENDER_OOM;
    }
    status = sgi_render_resource_snapshot(target, resource, 0, &rect,
                                          slots,
                                          region_width *
                                          resource->component_count,
                                          nslots, errp);
    if (status != SGI_RENDER_OK) {
        if (status == SGI_RENDER_UNSUPPORTED) {
            *reason = SGI_RENDER_REFUSAL_SNAPSHOT;
        }
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
            if (status == SGI_RENDER_UNSUPPORTED) {
                *reason = SGI_RENDER_REFUSAL_SNAPSHOT;
            }
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
        /* Clamped in double from the float state, as the scalar path does. */
        for (unsigned c = 0; c < 4; c++) {
            frame.texture.border[c] =
                MIN(MAX(target->texture->border_color[c], 0.0), 1.0);
            frame.texture.environment[c] =
                MIN(MAX(draw->state->texture_env_color[c], 0.0), 1.0);
        }
    }
    /* Region rows in the frame's order, native slots in native order. */
    for (uint32_t y = 0; !indexed && y < region_height; y++) {
        size_t native_row = (size_t)y * region_width;
        size_t frame_row = (size_t)(flipped ? region_height - 1 - y : y) *
                           region_width;

        for (uint32_t x = 0; x < region_width; x++) {
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
    if (window) {
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
        if (indexed) {
            piece_index = g_try_new(uint32_t, piece_vertices);
            if (!piece_index) {
                error_setg(errp, "Cannot allocate SGI Mesa draw indices");
                return SGI_RENDER_OOM;
            }
            pieces.index = piece_index;
            pieces.index_bits = draw->state->index_bits;
            frame.indices = piece_index;
            frame.index = index_out;
        } else {
            memcpy(frame.colour_steps, steps, sizeof(steps));
            pieces.steps = frame.colour_steps;
        }
        sgi_render_decompose(&pieces, draw);
        assert(pieces.count == piece_vertices);
        frame.primitive = pieces.primitive;
        frame.point_size = point_size;
        frame.count = pieces.count;
        frame.vertices = piece_storage;
    }
    frame.shade_model = draw->state->shade_model;
    /* An index is never blended. */
    frame.blend = indexed ? 0 : draw->state->blend;
    frame.blend_src = draw->state->blend_src;
    frame.blend_dst = draw->state->blend_dst;
    frame.blend_equation = draw->state->blend_equation;
    frame.region_given = true;
    frame.region = region;
    frame.rgba = rgba;
    frame.written = written;
    frame.written_capacity = pixels;
    memcpy(frame.modelview, draw->state->modelview, sizeof(frame.modelview));
    memcpy(frame.projection, draw->state->projection,
           sizeof(frame.projection));
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
        sgi_render_commit_phase(draw, lines ? &line_set : NULL);
        return SGI_RENDER_OK;
    }
    for (uint32_t y = 0; y < region_height; y++) {
        size_t native_row = (size_t)y * region_width;
        size_t frame_row = (size_t)(flipped ? region_height - 1 - y : y) *
                           region_width;

        for (uint32_t x = 0; x < region_width; x++) {
            size_t native_pixel = native_row + x;
            size_t frame_pixel = frame_row + x;

            if (!written[frame_pixel]) {
                continue;
            }
            /* The only merge: new bits under the mask, old bits elsewhere. */
            if (indexed) {
                unsigned bits = resource->component[0].storage_bits;
                uint32_t value = index_out[frame_pixel];

                if (bits < 32 && value >> bits) {
                    error_setg(errp, "SGI Mesa draw returned an index wider "
                               "than its resource");
                    return SGI_RENDER_HOST_ERROR;
                }
                slots[native_pixel] = (slots[native_pixel] & ~effective_mask) |
                                      (value & effective_mask);
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
    if (flipped) {
        for (uint32_t y = 0; y < region_height / 2; y++) {
            uint8_t *top = written + (size_t)y * region_width;
            uint8_t *bottom = written +
                              (size_t)(region_height - 1 - y) *
                              region_width;

            for (uint32_t x = 0; x < region_width; x++) {
                uint8_t value = top[x];

                top[x] = bottom[x];
                bottom[x] = value;
            }
        }
    }
    SGIRenderWriteCoverage writes = {
        .pixel = written,
        .stride_bytes = region_width,
        .capacity_bytes = pixels,
    };

    if (opts && opts->check_staged) {
        status = opts->check_staged(opts->opaque, resource, &rect, slots,
                                    region_width *
                                    resource->component_count, nslots,
                                    &writes, errp);
        if (status != SGI_RENDER_OK) {
            if (status == SGI_RENDER_UNSUPPORTED) {
                *reason = SGI_RENDER_REFUSAL_PRECISION;
            }
            return status;
        }
    }
    /* A failed commit is never retried, so the phase is counted here. */
    sgi_render_commit_phase(draw, lines ? &line_set : NULL);

    if (indexed) {
        committed = sgi_render_resource_commit_index_covered(
            target, resource, 0, &rect, slots, region_width, nslots,
            &writes, effective_mask, errp);
    } else {
        committed = sgi_render_resource_commit_covered(
            target, resource, 0, &rect, slots,
            region_width * resource->component_count, nslots, &writes,
            errp);
    }
    return committed.status;
}

SGIRenderResult sgi_render_draw_checked(SGIRenderer *renderer,
                                        const SGIRasterRules *rules,
                                        const SGIRasterTarget *target,
                                        const SGIGLDraw *draw, Error **errp)
{
    SGIRenderRefusal reason = SGI_RENDER_REFUSAL_NONE;

    return sgi_render_draw_run(renderer, rules, target, draw, NULL, &reason,
                               errp);
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

/*
 * The caller's precision check of each cleared resource's staged slots,
 * before the first native commit, with the clipped union as coverage.
 */
static SGIRenderResult sgi_render_clear_check(
    const SGIRasterTarget *target, const SGIRenderAttemptOptions *opts,
    unsigned colour_count, bool depth, bool stencil,
    const SGIRenderResource *const *resources,
    const SGIRendererClear *clears, uint32_t *const *slots,
    const size_t *nslots, Error **errp)
{
    for (unsigned i = 0; i < colour_count + depth + stencil; i++) {
        const SGIRenderResource *res = resources[i];
        const SGIRendererClear *clear = i < colour_count ? &clears[i] :
                                                           &clears[0];
        size_t pixels = (size_t)res->width * res->height;
        g_autofree uint8_t *covered = NULL;
        SGIRenderRect rect = { .x1 = res->width - 1, .y1 = res->height - 1 };
        SGIRenderWriteCoverage writes;
        SGIRenderResult status;

        if (!clear->rect_count) {
            continue;
        }
        covered = g_try_new(uint8_t, pixels);
        if (!covered) {
            error_setg(errp, "Cannot allocate SGI Mesa clear coverage");
            return SGI_RENDER_OOM;
        }
        for (uint32_t y = 0; y < res->height; y++) {
            sgi_render_clear_coverage_row(clear, res->width, y,
                                          covered + (size_t)y * res->width);
        }
        writes = (SGIRenderWriteCoverage) {
            .pixel = covered,
            .stride_bytes = res->width,
            .capacity_bytes = pixels,
        };
        status = opts->check_staged(opts->opaque, res, &rect, slots[i],
                                    res->width * res->component_count,
                                    nslots[i], &writes, errp);
        if (status != SGI_RENDER_OK) {
            return status;
        }
    }
    return SGI_RENDER_OK;
}

/* A checked raster operation, refusing as sgi_render_draw_run() does. */
static SGIRenderResult sgi_render_raster_run(
    SGIRenderer *renderer, const SGIRasterRules *rules,
    const SGIRasterTarget *target, const SGIRasterOp *op,
    const SGIRenderAttemptOptions *opts, SGIRenderRefusal *reason,
    Error **errp)
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
    if (opts && opts->mixed_frame &&
        !sgi_renderer_identity(renderer)->reference) {
        error_setg(errp, "SGI Mesa mixed frames are proven only on the "
                   "reference llvmpipe build");
        return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_HOST_DRIVER);
    }
    if (op->kind != SGI_RASTER_CLEAR ||
        (op->clear_buffers & ~(SGI_GL_COLOR_BUFFER_BIT |
                               SGI_GL_DEPTH_BUFFER_BIT |
                               SGI_GL_STENCIL_BUFFER_BIT)) ||
        op->index_bits) {
        error_setg(errp, "Unsupported SGI Mesa raster operation or plane");
        return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_RASTER_OP);
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
        return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_STORE_WIDTH);
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
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_RESOURCE);
        }
        for (unsigned c = 0; c < res->component_count; c++) {
            if (res->component[c].channel != SGI_RENDER_R + c ||
                res->component[c].numeric != SGI_RENDER_UNORM ||
                !res->component[c].storage_bits ||
                res->component[c].storage_bits > 16) {
                error_setg(errp, "Unsupported SGI Mesa clear component");
                return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_RESOURCE);
            }
        }
        if (!sgi_render_widths_proven(rules, res)) {
            error_setg(errp, "Unsupported SGI Mesa clear component widths");
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_STORE_WIDTH);
        }
        if (opts && opts->mixed_frame &&
            !sgi_render_clear_mixed_proven(rules)) {
            error_setg(errp, "SGI Mesa clear width is not proven identical "
                       "to the scalar path for a mixed frame");
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_MIXED_FRAME);
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
            return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_RESOURCE);
        }
        resources[i] = res;
        max_width = MAX(max_width, res->width);
    }
    if (depth || stencil) {
        for (unsigned i = 1; i < colour_count; i++) {
            if (clears[i].width != clears[0].width ||
                clears[i].height != clears[0].height) {
                error_setg(errp, "SGI Mesa clear attachment sizes differ");
                return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_RESOURCE);
            }
        }
    }
    if (!target->resource_ops || !target->resource_ops->snapshot ||
        !target->resource_ops->commit) {
        error_setg(errp, "SGI Mesa clear needs snapshot and commit providers");
        return sgi_render_refuse(reason, SGI_RENDER_REFUSAL_PROVIDER);
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
            if (status == SGI_RENDER_UNSUPPORTED) {
                *reason = SGI_RENDER_REFUSAL_SNAPSHOT;
            }
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
            if (status == SGI_RENDER_UNSUPPORTED) {
                *reason = SGI_RENDER_REFUSAL_SNAPSHOT;
            }
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
    if (opts && opts->check_staged) {
        status = sgi_render_clear_check(target, opts, colour_count, depth,
                                        stencil, resources, clears, slots,
                                        nslots, errp);
        if (status != SGI_RENDER_OK) {
            if (status == SGI_RENDER_UNSUPPORTED) {
                *reason = SGI_RENDER_REFUSAL_PRECISION;
            }
            goto out;
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

SGIRenderResult sgi_render_raster_checked(SGIRenderer *renderer,
                                          const SGIRasterRules *rules,
                                          const SGIRasterTarget *target,
                                          const SGIRasterOp *op, Error **errp)
{
    SGIRenderRefusal reason = SGI_RENDER_REFUSAL_NONE;

    return sgi_render_raster_run(renderer, rules, target, op, NULL, &reason,
                                 errp);
}

/*
 * A refusal is UNSUPPORTED with its reason. UNSUPPORTED without one came
 * from a provider once native work had begun, so it is a resource failure.
 */
static SGIRenderDrawResult sgi_render_attempt_result(
    SGIRenderResult status, SGIRenderRefusal reason,
    SGIRenderAttemptOutcome *outcome)
{
    if (status == SGI_RENDER_UNSUPPORTED &&
        reason == SGI_RENDER_REFUSAL_NONE) {
        status = SGI_RENDER_RESOURCE_ERROR;
    }
    *outcome = (SGIRenderAttemptOutcome) {
        .status = status,
        .reason = status == SGI_RENDER_UNSUPPORTED ? reason :
                                                     SGI_RENDER_REFUSAL_NONE,
    };
    return status == SGI_RENDER_OK ? SGI_RENDER_DRAW_OK :
           status == SGI_RENDER_UNSUPPORTED ? SGI_RENDER_DRAW_REFUSED :
                                              SGI_RENDER_DRAW_FAILED;
}

SGIRenderDrawResult sgi_render_draw_attempt(
    SGIRenderer *renderer, const SGIRasterRules *rules,
    const SGIRasterTarget *target, const SGIGLDraw *draw,
    const SGIRenderAttemptOptions *opts, SGIRenderAttemptOutcome *outcome,
    Error **errp)
{
    SGIRenderRefusal reason = SGI_RENDER_REFUSAL_NONE;
    SGIRenderResult status = sgi_render_draw_run(renderer, rules, target,
                                                 draw, opts, &reason, errp);

    return sgi_render_attempt_result(status, reason, outcome);
}

SGIRenderDrawResult sgi_render_raster_attempt(
    SGIRenderer *renderer, const SGIRasterRules *rules,
    const SGIRasterTarget *target, const SGIRasterOp *op,
    const SGIRenderAttemptOptions *opts, SGIRenderAttemptOutcome *outcome,
    Error **errp)
{
    SGIRenderRefusal reason = SGI_RENDER_REFUSAL_NONE;
    SGIRenderResult status = sgi_render_raster_run(renderer, rules, target,
                                                   op, opts, &reason, errp);

    return sgi_render_attempt_result(status, reason, outcome);
}

static const struct {
    const char *name;
    SGIRenderRefusalClass class;
} sgi_render_refusals[SGI_RENDER_REFUSAL__MAX] = {
    [SGI_RENDER_REFUSAL_NONE] = {
        "none", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_RULES] = {
        "rules", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_DITHER] = {
        "dither", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_PRIMITIVE] = {
        "primitive", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_SMOOTHING] = {
        "smoothing", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_COLOUR_INDEX] = {
        "colour-index", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_INDEX_CLIPPED] = {
        "index-clipped", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_BUFFERS] = {
        "buffers", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_FRAGMENT_TESTS] = {
        "fragment-tests", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_FOG] = {
        "fog", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_FACING] = {
        "facing", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_CLIP_PLANES] = {
        "clip-planes", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_LOGIC_OP] = {
        "logic-op", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_POLYGON_OFFSET] = {
        "polygon-offset", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_WINDOW] = {
        "window", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_GEOMETRY] = {
        "geometry", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_LINE_SHAPE] = {
        "line-shape", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_RESOURCE] = {
        "resource", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_STORE_WIDTH] = {
        "store-width", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_BLEND] = {
        "blend", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_BLEND_OVERLAP] = {
        "blend-overlap", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_TEXTURE] = {
        "texture", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_PROVIDER] = {
        "provider", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_RASTER_OP] = {
        "raster-op", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_MIXED_FRAME] = {
        "mixed-frame", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_HOST_VIEWPORT] = {
        "host-viewport", SGI_RENDER_REFUSAL_CLASS_HOST_LIMIT,
    },
    [SGI_RENDER_REFUSAL_HOST_POINT_SIZE] = {
        "host-point-size", SGI_RENDER_REFUSAL_CLASS_HOST_LIMIT,
    },
    [SGI_RENDER_REFUSAL_HOST_PROGRAM] = {
        "host-program", SGI_RENDER_REFUSAL_CLASS_HOST_LIMIT,
    },
    [SGI_RENDER_REFUSAL_HOST_PROBE] = {
        "host-probe", SGI_RENDER_REFUSAL_CLASS_HOST_LIMIT,
    },
    [SGI_RENDER_REFUSAL_HOST_STAGING] = {
        "host-staging", SGI_RENDER_REFUSAL_CLASS_HOST_LIMIT,
    },
    [SGI_RENDER_REFUSAL_HOST_DRIVER] = {
        "host-driver", SGI_RENDER_REFUSAL_CLASS_HOST_LIMIT,
    },
    [SGI_RENDER_REFUSAL_SNAPSHOT] = {
        "snapshot", SGI_RENDER_REFUSAL_CLASS_STATE,
    },
    [SGI_RENDER_REFUSAL_PRECISION] = {
        "precision", SGI_RENDER_REFUSAL_CLASS_PRECISION,
    },
};

const char *sgi_render_refusal_name(SGIRenderRefusal reason)
{
    return reason < SGI_RENDER_REFUSAL__MAX ?
           sgi_render_refusals[reason].name : "unknown";
}

SGIRenderRefusalClass sgi_render_refusal_class(SGIRenderRefusal reason)
{
    return reason < SGI_RENDER_REFUSAL__MAX ?
           sgi_render_refusals[reason].class : SGI_RENDER_REFUSAL_CLASS_STATE;
}
