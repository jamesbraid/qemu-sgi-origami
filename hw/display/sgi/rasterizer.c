/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The software rasterizer the SGI graphics heads share.
 *
 * A draw's vertices are transformed by the draw state's matrices, its
 * primitives clipped to the view volume, mapped through its viewport, or
 * a window matrix in place of projection and viewport, into window
 * coordinates, snapped to a subpixel grid and rasterized with edge
 * functions at pixel centres, clipped by the enabled client clip planes,
 * textured from a family-owned image view while supported, and
 * fogged while fog is enabled. Each fragment then passes the
 * per-fragment operations in OpenGL 1.2.1's order (4.1):
 * pixel ownership against the drawable's rectangles, the scissor test, the
 * alpha test, the stencil test, the depth test, blending, dithering and
 * the logical operation, and is stored under the write masks through the
 * family's target. What it draws follows OpenGL 1.2.1 for
 * aliased points and lines, polygons and untextured bitmaps,
 * flat or smooth shaded, in RGBA or colour-index mode, from vertex colours
 * the context has already lit, with a back-facing polygon's back colours under
 * two-sided lighting, and polygons culled by facing. Where OpenGL leaves a
 * choice, the head's SGIRasterRules make it.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "hw/display/sgi/rasterizer.h"
#include "hw/display/sgi/x-gc.h"

/*
 * The drawable's window: its origin and the bounds of the pixels it owns,
 * in plane pixels, and the rectangles themselves.
 */
typedef struct SGISoftWindow {
    int ox, oy;
    int x0, y0, x1, y1;
    const SGIClipList *clip;
} SGISoftWindow;

/* One draw's fixed inputs. */
typedef struct SGISoftDraw {
    const SGIRasterRules *rules;
    const SGIRasterTarget *t;
    const SGIGLDrawState *st;
    SGISoftWindow win;
    SGIColourTarget targets[SGI_GL_MAX_TARGETS];
    uint32_t ntargets;
    /* Back-facing polygons take the vertices' back colours. */
    bool back_colors;
    /* Fragments blend with the colour they replace. */
    bool blend;
    /* Fragments combine with it by the logical operation instead. */
    bool logic_op;
    /* In RGBA mode, the packed colour's bits glColorMask lets through. */
    uint32_t colour_mask;
    /* The stored colour is dithered: GL_DITHER and the rules' dither. */
    bool dither;
    const SGIGLTexImage *texture;
    const SGIGLTexLevel *texture_base;
    uint32_t texture_last;
    unsigned *texture_reports;
    uint32_t point_size, line_width;
    const uint32_t *polygon_stipple;
    uint32_t rgba_mask;
    bool rgba_read;
    bool *rgba_reported;
} SGISoftDraw;

/* A vertex after transformation, ready to rasterize. */
typedef struct SGISoftVertex {
    int64_t x, y;         /* window position in subpixels */
    double z;             /* normalized depth; [0, 1] inside the view volume */
    double inv_w;         /* 1 / clip w, for perspective-correct colour */
    double rgba[4];
    double texcoord[4];
    double index;         /* the colour index, in colour-index mode */
    double fog;           /* the fog coordinate, |z| in eye coordinates */
    /* Each client clip plane's value at the vertex, kept where >= 0. */
    double clip[SGI_GL_CLIP_PLANES];
} SGISoftVertex;

/*
 * A fragment's values as rasterization leaves them (OpenGL 1.2.1, 3.1),
 * before the per-fragment operations of chapter 4: its colour, each
 * component in [0, 1], and in colour-index mode its index.
 */
typedef struct SGISoftFrag {
    double rgba[4];
    double texcoord[4];
    double texture_rho;
    double index;
    double fog;           /* the fog coordinate (3.10) */
} SGISoftFrag;

static bool sgi_soft_window(const SGIGLDrawState *st, SGISoftWindow *win)
{
    const SGIGLDrawable *d = &st->drawable;

    if (!d->bound) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: draw with no window bound "
                      "dropped\n");
        return false;
    }
    if (!d->clip.n) {
        /* Other windows cover all of this one: nothing to draw. */
        return false;
    }
    win->ox = d->origin[0];
    win->oy = d->origin[1];
    win->x0 = d->clip.r[0].x0;
    win->y0 = d->clip.r[0].y0;
    win->x1 = d->clip.r[0].x1;
    win->y1 = d->clip.r[0].y1;
    for (uint32_t i = 1; i < MIN(d->clip.n, SGI_CLIP_MAX); i++) {
        win->x0 = MIN(win->x0, d->clip.r[i].x0);
        win->y0 = MIN(win->y0, d->clip.r[i].y0);
        win->x1 = MAX(win->x1, d->clip.r[i].x1);
        win->y1 = MAX(win->y1, d->clip.r[i].y1);
    }
    win->clip = &d->clip;
    return true;
}

bool sgi_raster_rules_supported(const SGIRasterRules *r)
{
    return r->subpixel_bits >= 1 && r->subpixel_bits <= 12 &&
           r->color_bits >= 1 && r->color_bits <= 10 &&
           r->depth_bits >= 1 && r->depth_bits <= 32 &&
           r->stencil_bits <= 16 && r->alpha_bits <= 10 &&
           r->rounding <= SGI_ROUND_HALF_AWAY &&
           r->tie_rule == SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN &&
           r->arith == SGI_ARITH_DOUBLE &&
           r->line_rule == SGI_LINE_COVERAGE_DOUBLE &&
           r->dither <= SGI_DITHER_BAYER4_TOP_ROWS &&
           (r->dither == SGI_DITHER_NONE ||
            r->dither_anchor == SGI_DITHER_SCREEN);
}

static bool sgi_soft_check_rules(const SGIRasterRules *r)
{
    if (!sgi_raster_rules_supported(r)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: raster rules (%s) are not "
                      "implemented; nothing drawn\n",
                      r->evidence ? r->evidence : "unlabelled");
        return false;
    }
    return true;
}

/* Whether the target can take the colour-index work a draw state asks. */
static bool sgi_soft_check_index(const SGIRasterTarget *t,
                                 uint32_t index_bits)
{
    if (index_bits && !t->store_index) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: colour-index drawing is not "
                      "implemented for these planes; nothing drawn\n");
        return false;
    }
    return true;
}

/* x rounded to an integer by the rules' rounding. */
static int64_t sgi_soft_round(const SGIRasterRules *r, double x)
{
    double f = floor(x), frac = x - f;

    switch (r->rounding) {
    case SGI_ROUND_FLOOR:
        return (int64_t)f;
    case SGI_ROUND_HALF_UP:
        return (int64_t)f + (frac >= 0.5);
    case SGI_ROUND_HALF_EVEN:
        if (frac == 0.5) {
            return (int64_t)f + ((int64_t)f & 1);
        }
        return (int64_t)f + (frac > 0.5);
    default:
        return llround(x);
    }
}

/* The largest stored depth. */
static uint32_t sgi_soft_depth_max(const SGIRasterRules *r)
{
    return UINT32_MAX >> (32 - r->depth_bits);
}

/*
 * A component clamped to [0, 1] and scaled to the rules' colour bits, a
 * fraction of a step rounded half up, whatever the rules' rounding: the
 * sum is truncated (OpenGL 1.2.1, 2.13.9).
 */
static uint32_t sgi_soft_channel(const SGIRasterRules *r, double c)
{
    uint32_t max = (1u << r->color_bits) - 1;

    if (!(c > 0.0)) {
        return 0;
    }
    if (c >= 1.0) {
        return max;
    }
    return (uint32_t)(c * max + 0.5);
}

/* Incoming alpha precision is independent of absent or narrower stored A. */
static uint32_t sgi_soft_alpha_max(const SGIRasterRules *r)
{
    return (1u << MAX(2, MAX(r->color_bits, r->alpha_bits))) - 1;
}

static uint32_t sgi_soft_alpha(const SGIRasterRules *r, double value)
{
    uint32_t max = sgi_soft_alpha_max(r);

    return !(value > 0) ? 0 : value >= 1 ? max : value * max + 0.5;
}

/* Internal nearest conversion precedes storage truncation, even on clears. */
static uint32_t sgi_soft_stored_alpha(const SGIRasterRules *r, double value)
{
    return sgi_soft_alpha(r, value) * ((1u << r->alpha_bits) - 1) /
           sgi_soft_alpha_max(r);
}

static uint32_t sgi_soft_rgba_mask(const SGIRasterRules *r, uint32_t mask)
{
    return mask & (r->alpha_bits ? SGI_GL_COLOUR_MASK_ALL :
                                  SGI_GL_COLOUR_MASK_ALPHA - 1);
}

/* Reject unsupported capabilities before the operation can write any plane. */
static bool sgi_soft_rgba_capable(const SGIRasterRules *r,
                                 const SGIRasterTarget *t, bool read)
{
    if ((r->alpha_bits && !t->store_rgba) ||
        (t->store_rgba && read && !t->load_rgba) ||
        (!t->store_rgba && t->load_rgba)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: inconsistent RGBA callbacks; "
                      "operation dropped\n");
        return false;
    }
    return true;
}

/* Read every selected destination before any colour/depth/stencil write. */
static bool sgi_soft_rgba_load(const SGIRasterRules *r,
                              const SGIRasterTarget *t,
                              const SGIColourTarget *targets, unsigned count,
                              int x, int y,
                              SGIRasterColour dest[SGI_GL_MAX_TARGETS],
                              bool *reported)
{
    for (unsigned i = 0; i < count; i++) {
        bool valid = t->load_rgba(t->opaque, targets[i], x, y, &dest[i]);

        for (unsigned k = 0; valid && k < 4; k++) {
            unsigned bits = k == 3 ? r->alpha_bits : r->color_bits;

            /* An absent A is ignored, including its returned representation. */
            valid = !bits || dest[i].component[k] < (1u << bits);
        }
        if (!valid) {
            if (!*reported) {
                qemu_log_mask(LOG_UNIMP, "sgi-gl: RGBA destination read "
                              "failed; pixel writes dropped\n");
                *reported = true;
            }
            return false;
        }
        if (!r->alpha_bits) {
            dest[i].component[3] = 0;
        }
    }
    return true;
}

/* A colour, each component clamped and rounded to the rules' bits. */
static uint32_t sgi_soft_rgb(const SGIRasterRules *r, const double c[3])
{
    return (sgi_soft_channel(r, c[0]) << (2 * r->color_bits)) |
           (sgi_soft_channel(r, c[1]) << r->color_bits) |
           sgi_soft_channel(r, c[2]);
}

/* SGI_DITHER_BAYER4_TOP_ROWS's thresholds, in sixteenths of a step. */
static const uint8_t sgi_soft_bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

/*
 * A fragment's colour at plane pixel (x, y) as the planes store it:
 * dithered while the draw dithers, otherwise rounded as sgi_soft_rgb()
 * rounds. A clamped component of 0 or 1 is exact either way, and one
 * below 1 stays below max + 1 with the largest threshold added.
 */
static uint32_t sgi_soft_store_rgb(const SGISoftDraw *r, int x, int y,
                                   const double c[3])
{
    const int bits = r->rules->color_bits;
    const uint32_t max = (1u << bits) - 1;
    int64_t row;
    double b;
    uint32_t out = 0;

    if (!r->dither) {
        return sgi_soft_rgb(r->rules, c);
    }
    row = r->t->screen_rows_down ? (int64_t)y - r->t->screen_top :
                                   (int64_t)r->t->screen_top - y;
    b = sgi_soft_bayer4[row & 3][x & 3] / 16.0;
    for (int k = 0; k < 3; k++) {
        uint32_t v;

        if (!(c[k] > 0.0)) {
            v = 0;
        } else if (c[k] >= 1.0) {
            v = max;
        } else {
            v = floor(c[k] * max + b);
        }
        out |= v << ((2 - k) * bits);
    }
    return out;
}

static int64_t sgi_soft_edge(const SGISoftVertex *a, const SGISoftVertex *b,
                             int64_t px, int64_t py)
{
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/*
 * SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN: a counter-clockwise edge owns the pixel
 * centres on it when it is a left edge (running down) or a top edge
 * (horizontal, running left), with rows counted up from the bottom of the
 * plane, so two triangles sharing an edge never both draw its centres, as
 * OpenGL 1.2.1, 3.5.1 requires.
 */
static bool sgi_soft_owns(const SGISoftVertex *a, const SGISoftVertex *b,
                          int64_t e)
{
    if (e != 0) {
        return e > 0;
    }
    return b->y < a->y || (b->y == a->y && b->x < a->x);
}

/*
 * Whether value passes a test comparing it with ref by func, the
 * comparison minus GL_NEVER: the depth test's incoming depth against the
 * stored one (4.1.5), the alpha test's alpha against its reference
 * (4.1.4), the stencil test's masked reference against the masked stored
 * value (4.1.5).
 */
static bool sgi_soft_compare(uint32_t func, uint32_t value, uint32_t ref)
{
    switch (func) {
    case SGI_GL_NEVER:
        return false;
    case SGI_GL_LESS:
        return value < ref;
    case SGI_GL_EQUAL:
        return value == ref;
    case SGI_GL_LEQUAL:
        return value <= ref;
    case SGI_GL_GREATER:
        return value > ref;
    case SGI_GL_NOTEQUAL:
        return value != ref;
    case SGI_GL_GEQUAL:
        return value >= ref;
    default:
        return true;
    }
}

/* Whether the planes have a stencil buffer, and its largest value. */
static bool sgi_soft_stencil_max(const SGIRasterRules *rules,
                                 const SGIRasterTarget *t, uint32_t *max)
{
    *max = (1u << rules->stencil_bits) - 1;
    return rules->stencil_bits && t->load_stencil && t->store_stencil;
}

/*
 * One of glStencilOp's operations on the stencil value at (x, y), the
 * result written under the stencil write mask (4.1.5, 4.2.2). GL_INCR
 * and GL_DECR clamp to the buffer's range.
 */
static void sgi_soft_stencil_op(const SGISoftDraw *r, int x, int y,
                                uint32_t op, uint32_t max, uint32_t ref)
{
    const SGIRasterTarget *t = r->t;
    uint32_t old = t->load_stencil(t->opaque, x, y) & max, value;
    uint32_t mask = r->st->stencil_write_mask & max;

    switch (op) {
    case SGI_GL_KEEP:
        return;
    case SGI_GL_REPLACE:
        value = ref;
        break;
    case SGI_GL_INCR:
        value = old < max ? old + 1 : max;
        break;
    case SGI_GL_DECR:
        value = old ? old - 1 : 0;
        break;
    case SGI_GL_INVERT:
        value = ~old & max;
        break;
    default:
        value = 0;
        break;
    }
    t->store_stencil(t->opaque, x, y, (old & ~mask) | (value & mask));
}

/*
 * One blend factor (OpenGL 1.2.1, tables 4.1 and 4.2) for component k,
 * red to alpha, from the source s, destination d and constant colour c.
 */
static double sgi_soft_blend_factor(uint32_t factor, int k, const double s[4],
                                    const double d[4], const float c[4])
{
    switch (factor) {
    case 1:
        return 1;
    case SGI_GL_SRC_COLOR:
        return s[k];
    case SGI_GL_ONE_MINUS_SRC_COLOR:
        return 1 - s[k];
    case SGI_GL_DST_COLOR:
        return d[k];
    case SGI_GL_ONE_MINUS_DST_COLOR:
        return 1 - d[k];
    case SGI_GL_SRC_ALPHA:
        return s[3];
    case SGI_GL_ONE_MINUS_SRC_ALPHA:
        return 1 - s[3];
    case SGI_GL_DST_ALPHA:
        return d[3];
    case SGI_GL_ONE_MINUS_DST_ALPHA:
        return 1 - d[3];
    case SGI_GL_SRC_ALPHA_SATURATE:
        return k == 3 ? 1 : MIN(s[3], 1 - d[3]);
    case SGI_GL_CONSTANT_COLOR:
        return c[k];
    case SGI_GL_ONE_MINUS_CONSTANT_COLOR:
        return 1 - c[k];
    case SGI_GL_CONSTANT_ALPHA:
        return c[3];
    case SGI_GL_ONE_MINUS_CONSTANT_ALPHA:
        return 1 - c[3];
    default:
        return 0;
    }
}

static double sgi_soft_blend_component(const SGIGLDrawState *st, unsigned k,
                                       const double src[4], const double dst[4])
{
    double sf = sgi_soft_blend_factor(st->blend_src, k, src, dst,
                                      st->blend_color);
    double df = sgi_soft_blend_factor(st->blend_dst, k, src, dst,
                                      st->blend_color);

    switch (st->blend_equation) {
    case SGI_GL_MIN:
        return MIN(src[k], dst[k]);
    case SGI_GL_MAX:
        return MAX(src[k], dst[k]);
    case SGI_GL_FUNC_SUBTRACT:
        return src[k] * sf - dst[k] * df;
    case SGI_GL_FUNC_REVERSE_SUBTRACT:
        return dst[k] * df - src[k] * sf;
    default:
        return src[k] * sf + dst[k] * df;
    }
}

/*
 * Blend the packed source colour src, with alpha alpha, and the packed
 * destination dst (4.1.6), each component the fixed point of the rules'
 * colour bits taken as a fraction of its largest value. The planes hold
 * no alpha, so the destination's alpha is 1. The result is clamped to
 * [0, 1] and stored at plane pixel (x, y) as a fragment's colour is.
 */
static uint32_t sgi_soft_blend(const SGISoftDraw *r, int x, int y,
                               uint32_t src, double alpha, uint32_t dst)
{
    const SGIGLDrawState *st = r->st;
    const int bits = r->rules->color_bits;
    const double max = (1u << bits) - 1;
    double s[4], d[4], out[3];

    for (int k = 0; k < 3; k++) {
        int shift = (2 - k) * bits;

        s[k] = ((src >> shift) & (uint32_t)max) / max;
        d[k] = ((dst >> shift) & (uint32_t)max) / max;
    }
    s[3] = alpha;
    d[3] = 1;
    for (int k = 0; k < 3; k++) {
        out[k] = sgi_soft_blend_component(st, k, s, d);
    }
    return sgi_soft_store_rgb(r, x, y, out);
}

/*
 * glLogicOp's operation (4.1.8, table 4.3) on the source s and the
 * destination d, bit by bit.
 */
static uint32_t sgi_soft_logic_op(uint32_t op, uint32_t s, uint32_t d)
{
    return sgi_x_logic_op(op - SGI_GL_CLEAR, s, d);
}

/* RGB retains its existing arithmetic; spatial alpha dithering is omitted. */
static SGIRasterColour sgi_soft_rgba_value(const SGIRasterRules *rules,
                                          uint32_t rgb, double alpha)
{
    SGIRasterColour c;
    uint32_t max = (1u << rules->color_bits) - 1;

    for (unsigned k = 0; k < 3; k++) {
        c.component[k] = (rgb >> ((2 - k) * rules->color_bits)) & max;
    }
    c.component[3] = sgi_soft_stored_alpha(rules, alpha);
    return c;
}

static SGIRasterColour sgi_soft_rgba_result(const SGISoftDraw *r, int x, int y,
                                           const SGISoftFrag *frag,
                                           const SGIRasterColour *dest)
{
    SGIRasterColour out;
    uint32_t max = (1u << r->rules->color_bits) - 1;
    double alpha = sgi_soft_alpha(r->rules, frag->rgba[3]) /
                   (double)sgi_soft_alpha_max(r->rules);
    uint32_t rgb = r->blend ? sgi_soft_rgb(r->rules, frag->rgba) :
                              sgi_soft_store_rgb(r, x, y, frag->rgba);

    out = sgi_soft_rgba_value(r->rules, rgb, alpha);
    if (r->blend) {
        double src[4], dst[4], blended[4];

        for (unsigned k = 0; k < 3; k++) {
            src[k] = out.component[k] / (double)max;
            dst[k] = dest->component[k] / (double)max;
        }
        src[3] = alpha;
        dst[3] = r->rules->alpha_bits ? dest->component[3] /
                  (double)((1u << r->rules->alpha_bits) - 1) : 1;
        for (unsigned k = 0; k < 4; k++) {
            blended[k] = sgi_soft_blend_component(r->st, k, src, dst);
        }
        out = sgi_soft_rgba_value(r->rules,
                                  sgi_soft_store_rgb(r, x, y, blended),
                                  blended[3]);
    }
    for (unsigned k = 0; k < 4; k++) {
        uint32_t bits = k == 3 ? r->rules->alpha_bits : r->rules->color_bits;

        if (r->logic_op) {
            out.component[k] = sgi_soft_logic_op(r->st->logic_op_mode,
                                  out.component[k], dest->component[k]) &
                               ((1u << bits) - 1);
        }
        if (!(r->rgba_mask & (1u << k))) {
            out.component[k] = dest->component[k];
        }
    }
    return out;
}

/*
 * The bits of a packed colour of the rules' colour bits that a
 * glColorMask channel mask lets through. The planes hold no alpha, so
 * its bit has no effect.
 */
static uint32_t sgi_soft_colour_mask(const SGIRasterRules *r, uint32_t mask)
{
    uint32_t channel = (1u << r->color_bits) - 1, out = 0;

    for (int k = 0; k < 3; k++) {
        if (mask & (SGI_GL_COLOUR_MASK_RED << k)) {
            out |= channel << ((2 - k) * r->color_bits);
        }
    }
    return out;
}

/* The packed colour's bits, all of which a full mask writes. */
static uint32_t sgi_soft_colour_bits(const SGIRasterRules *r)
{
    return sgi_soft_colour_mask(r, SGI_GL_COLOUR_MASK_ALL);
}

/*
 * Which of the operations that read the stored colour a draw state
 * enables. The logical operation applies to a colour index while
 * GL_INDEX_LOGIC_OP is enabled, and to an RGBA colour while
 * GL_COLOR_LOGIC_OP is, or while blending is enabled with
 * EXT_blend_logic_op's GL_LOGIC_OP equation; it replaces blending
 * (4.1.8). A colour mask with some channels clear keeps the stored
 * bits of those (4.2.2). Planes that cannot be read draw as if the
 * operations were disabled and the mask were all ones, logged once for
 * the draw. An RGBA draw is dithered while GL_DITHER is enabled under
 * rules that name a dither; a colour index never is.
 */
static bool sgi_soft_setup_ops(SGISoftDraw *r)
{
    const SGIGLDrawState *st = r->st;

    if (st->index_bits) {
        r->blend = false;
        r->logic_op = st->index_logic_op;
    } else {
        r->logic_op = st->color_logic_op ||
                      (st->blend && st->blend_equation == SGI_GL_LOGIC_OP);
        r->blend = st->blend && !r->logic_op;
    }
    r->colour_mask = sgi_soft_colour_mask(r->rules, st->colour_mask);
    r->dither = st->dither && r->rules->dither != SGI_DITHER_NONE;
    if (!st->index_bits) {
        r->rgba_mask = sgi_soft_rgba_mask(r->rules, st->colour_mask);
        r->rgba_read = r->ntargets && r->rgba_mask &&
            (r->blend || r->logic_op ||
            r->rgba_mask != sgi_soft_rgba_mask(r->rules,
                                              SGI_GL_COLOUR_MASK_ALL));
        if (!sgi_soft_rgba_capable(r->rules, r->t, r->rgba_read)) {
            return false;
        }
        if (r->t->store_rgba) {
            return true;
        }
    }
    if ((r->blend || r->logic_op ||
         (!st->index_bits && r->colour_mask &&
          r->colour_mask != sgi_soft_colour_bits(r->rules))) &&
        !r->t->load_colour) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: blending, logical operations and "
                      "partial colour masks need planes that can be read "
                      "back; drawn without them\n");
        r->blend = r->logic_op = false;
        r->colour_mask = sgi_soft_colour_bits(r->rules);
    }
    return true;
}

/* Runtime errors are observable without logging once for every pixel. */
static void sgi_soft_texture_report(const SGISoftDraw *r, unsigned reason,
                                    const char *message)
{
    if (!(*r->texture_reports & reason)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: texture %s\n", message);
        *r->texture_reports |= reason;
    }
}

static bool sgi_soft_texture_format(uint32_t format)
{
    return format == SGI_GL_ALPHA || format == SGI_GL_LUMINANCE ||
           format == SGI_GL_LUMINANCE_ALPHA || format == SGI_GL_INTENSITY ||
           format == SGI_GL_RGB || format == SGI_GL_RGBA;
}

static bool sgi_soft_texture_wrap(uint32_t wrap)
{
    return wrap == SGI_GL_REPEAT || wrap == SGI_GL_CLAMP ||
           wrap == SGI_GL_CLAMP_TO_EDGE || wrap == SGI_GL_CLAMP_TO_BORDER;
}

/* OpenGL 1.2.1 table 3.16: exact identities remain distinct in a chain. */
static uint32_t sgi_soft_texture_internal_format(uint32_t format)
{
    switch (format) {
    case SGI_GL_ALPHA4: case SGI_GL_ALPHA8:
    case SGI_GL_ALPHA12: case SGI_GL_ALPHA16:
        return SGI_GL_ALPHA;
    case SGI_GL_LUMINANCE4: case SGI_GL_LUMINANCE8:
    case SGI_GL_LUMINANCE12: case SGI_GL_LUMINANCE16:
        return SGI_GL_LUMINANCE;
    case SGI_GL_LUMINANCE4_ALPHA4: case SGI_GL_LUMINANCE6_ALPHA2:
    case SGI_GL_LUMINANCE8_ALPHA8: case SGI_GL_LUMINANCE12_ALPHA4:
    case SGI_GL_LUMINANCE12_ALPHA12: case SGI_GL_LUMINANCE16_ALPHA16:
        return SGI_GL_LUMINANCE_ALPHA;
    case SGI_GL_INTENSITY4: case SGI_GL_INTENSITY8:
    case SGI_GL_INTENSITY12: case SGI_GL_INTENSITY16:
        return SGI_GL_INTENSITY;
    case SGI_GL_R3_G3_B2: case SGI_GL_RGB4: case SGI_GL_RGB5:
    case SGI_GL_RGB8: case SGI_GL_RGB10: case SGI_GL_RGB12: case SGI_GL_RGB16:
        return SGI_GL_RGB;
    case SGI_GL_RGBA2: case SGI_GL_RGBA4: case SGI_GL_RGB5_A1:
    case SGI_GL_RGBA8: case SGI_GL_RGB10_A2:
    case SGI_GL_RGBA12: case SGI_GL_RGBA16:
        return SGI_GL_RGBA;
    default:
        return sgi_soft_texture_format(format) ? format : 0;
    }
}

static bool sgi_soft_texture_mipmap(uint32_t filter)
{
    return filter >= SGI_GL_NEAREST_MIPMAP_NEAREST &&
           filter <= SGI_GL_LINEAR_MIPMAP_LINEAR;
}

static bool sgi_soft_texture_dimensions(uint32_t w, uint32_t h)
{
    return w && h && w <= INT32_MAX && h <= INT32_MAX &&
           is_power_of_2(w) && is_power_of_2(h);
}

static const SGIGLTexLevel *sgi_soft_texture_level(const SGIGLTexPyramid *p,
                                                  uint32_t level)
{
    for (uint32_t i = 0; i < p->count; i++) {
        if (p->levels[i].level == level) {
            return &p->levels[i];
        }
    }
    return NULL;
}

static bool sgi_soft_texture_pyramid(SGISoftDraw *r, const SGIGLTexImage *im)
{
    const SGIGLTexPyramid *p = im->pyramid;
    const SGIGLTexLevel *base;
    bool mipmap = sgi_soft_texture_mipmap(im->min_filter);
    bool border_finite = true;
    uint32_t last;

    for (unsigned k = 0; k < 4; k++) {
        border_finite &= isfinite(im->border_color[k]);
    }
    if (p->count > SGI_GL_TEX_LEVELS_MAX || (p->count && !p->levels) ||
        !isfinite(p->min_lod) || !isfinite(p->max_lod) ||
        p->min_lod > p->max_lod || !border_finite ||
        !sgi_soft_texture_wrap(im->wrap_s) ||
        !sgi_soft_texture_wrap(im->wrap_t) ||
        (im->min_filter != SGI_GL_NEAREST && im->min_filter != SGI_GL_LINEAR &&
         !mipmap) ||
        (im->mag_filter != SGI_GL_NEAREST && im->mag_filter != SGI_GL_LINEAR)) {
        goto unsupported;
    }
    for (uint32_t i = 1; i < p->count; i++) {
        if (p->levels[i - 1].level >= p->levels[i].level) {
            goto unsupported;
        }
    }
    if (mipmap && p->max_level < p->base_level) {
        goto incomplete;
    }
    base = sgi_soft_texture_level(p, p->base_level);
    if (!base) {
        goto incomplete;
    }
    if (base->border > 1 ||
        (base->border && !base->read_border_texel) ||
        sgi_soft_texture_internal_format(base->internal_format) !=
        base->base_format || !sgi_soft_texture_format(base->base_format) ||
        (r->st->texture_env_mode == SGI_GL_DECAL &&
         base->base_format != SGI_GL_RGB && base->base_format != SGI_GL_RGBA)) {
        goto unsupported;
    }
    if (!sgi_soft_texture_dimensions(base->width, base->height)) {
        goto incomplete;
    }
    last = p->base_level;
    if (mipmap) {
        uint32_t k = 31 - clz32(MAX(base->width, base->height));
        last = MIN((uint64_t)p->base_level + k, p->max_level);
    }
    /* Unsupported relevant forms cannot be concealed by missing images. */
    for (uint32_t i = 0; i < p->count; i++) {
        const SGIGLTexLevel *l = &p->levels[i];

        if (l->level < p->base_level || l->level > last) {
            continue;
        }
        if (l->border > 1 || (l->border && !l->read_border_texel) ||
            !sgi_soft_texture_format(l->base_format) ||
            sgi_soft_texture_internal_format(l->internal_format) !=
            l->base_format) {
            goto unsupported;
        }
    }
    for (uint64_t id = p->base_level; id <= last; id++) {
        const SGIGLTexLevel *l = sgi_soft_texture_level(p, id);
        uint32_t shift = id - p->base_level;

        if (!l || !l->read_texel ||
            l->width != MAX(1, base->width >> shift) ||
            l->height != MAX(1, base->height >> shift) ||
            l->internal_format != base->internal_format ||
            l->border != base->border) {
            goto incomplete;
        }
    }
    r->texture = im;
    r->texture_base = base;
    r->texture_last = last;
    return true;

unsupported:
    qemu_log_mask(LOG_UNIMP, "sgi-gl: unsupported texture state in pyramid, "
                  "draw dropped\n");
    return false;
incomplete:
    qemu_log_mask(LOG_UNIMP, "sgi-gl: incomplete texture image pyramid, "
                  "texturing disabled for draw\n");
    return true;
}

/* Incomplete images disable texturing; unsupported states drop the draw. */
static bool sgi_soft_texture_setup(SGISoftDraw *r)
{
    const SGIGLTexImage *im = r->t->texture;
    const SGIGLDrawState *st = r->st;

    if (!st->texture_2d || st->index_bits) {
        return true;
    }
    if (st->texture_env_mode != SGI_GL_REPLACE &&
        st->texture_env_mode != SGI_GL_MODULATE &&
        st->texture_env_mode != SGI_GL_DECAL &&
        st->texture_env_mode != SGI_GL_BLEND) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: unsupported texture state "
                      "environment 0x%x, draw dropped\n",
                      st->texture_env_mode);
        return false;
    }
    if (im && (!isfinite(im->lod_scale_s) || !isfinite(im->lod_scale_t) ||
               im->lod_scale_s < 0 || im->lod_scale_t < 0)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-gl: invalid derivative scales, draw dropped\n");
        return false;
    }
    if (im && im->pyramid) {
        return sgi_soft_texture_pyramid(r, im);
    }
    if (im) {
        bool border_finite = true;

        for (int k = 0; k < 4; k++) {
            border_finite &= isfinite(im->border_color[k]);
        }
        if (im->border > 1 || (im->border && !im->read_border_texel) ||
            !border_finite ||
            !sgi_soft_texture_format(im->base_format) ||
            !sgi_soft_texture_wrap(im->wrap_s) ||
            !sgi_soft_texture_wrap(im->wrap_t) ||
            im->min_filter != im->mag_filter ||
            (im->min_filter != SGI_GL_NEAREST &&
             im->min_filter != SGI_GL_LINEAR) ||
            (st->texture_env_mode == SGI_GL_DECAL &&
             im->base_format != SGI_GL_RGB && im->base_format != SGI_GL_RGBA)) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: unsupported texture state "
                          "format 0x%x border %u filters 0x%x/0x%x "
                          "wraps 0x%x/0x%x environment 0x%x%s, draw dropped\n",
                          im->base_format, im->border, im->min_filter,
                          im->mag_filter, im->wrap_s, im->wrap_t,
                          st->texture_env_mode,
                          border_finite ? "" : " nonfinite border colour");
            return false;
        }
    }
    if (!im || !im->read_texel || !im->width || !im->height ||
        im->width > INT32_MAX || im->height > INT32_MAX ||
        !is_power_of_2(im->width) || !is_power_of_2(im->height)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: incomplete texture image, "
                      "texturing disabled for draw\n");
        return true;
    }
    r->texture = im;
    return true;
}

/* Validate every surviving vertex before any part of its primitive draws. */
static bool sgi_soft_texture_vertex_valid(const SGISoftDraw *r,
                                          const SGIVolumeVertex *v)
{
    bool valid;

    if (!r->texture) {
        return true;
    }
    valid = isfinite(v->clip[3]) && v->clip[3] > 0 &&
            v->attr[SGI_GL_V_TEXCOORD + 3] > 0;
    for (int k = 0; k < 4; k++) {
        valid &= isfinite(v->attr[SGI_GL_V_TEXCOORD + k]);
    }
    if (!valid) {
        sgi_soft_texture_report(r, 4, "invalid clipped coordinates, "
                                "primitive dropped");
    }
    return valid;
}

/* Meaningful base components, including the border, before filtering. */
static void sgi_soft_texture_components(uint32_t format, double v[4])
{
    switch (format) {
    case SGI_GL_ALPHA:
        v[0] = v[1] = v[2] = 1;
        break;
    case SGI_GL_LUMINANCE:
        v[3] = 1;
        /* fall through */
    case SGI_GL_LUMINANCE_ALPHA:
        v[1] = v[2] = v[0];
        break;
    case SGI_GL_INTENSITY:
        v[1] = v[2] = v[3] = v[0];
        break;
    case SGI_GL_RGB:
        v[3] = 1;
        break;
    }
}

static bool sgi_soft_texture_tap(const SGISoftDraw *r,
                                  const SGIGLTexLevel *level,
                                  int64_t x, int64_t y,
                                 double out[4])
{
    const SGIGLTexImage *im = r->texture;
    uint32_t width = level ? level->width : im->width;
    uint32_t height = level ? level->height : im->height;
    uint32_t format = level ? level->base_format : im->base_format;
    uint32_t rgba;

    if (im->wrap_s == SGI_GL_REPEAT) {
        x = (x % width + width) % width;
    }
    if (im->wrap_t == SGI_GL_REPEAT) {
        y = (y % height + height) % height;
    }
    if (x < 0 || y < 0 || x >= width || y >= height) {
        uint32_t border = level ? level->border : im->border;

        if (!border) {
            for (int k = 0; k < 4; k++) {
                out[k] = MIN(MAX(im->border_color[k], 0), 1);
            }
            sgi_soft_texture_components(format, out);
            return true;
        }
        /* A one-texel ring includes corners, with signed interior indices. */
        bool read = x >= -1 && y >= -1 && x <= width && y <= height &&
            (level ? level->read_border_texel &&
                level->read_border_texel(level->opaque, level->level,
                                         x, y, &rgba) :
                im->read_border_texel &&
                im->read_border_texel(im->opaque, x, y, &rgba));

        if (!read) {
            goto read_failure;
        }
    } else {
        bool read = level ?
            level->read_texel(level->opaque, level->level, x, y, &rgba) :
            im->read_texel(im->opaque, x, y, &rgba);

        if (!read) {
            goto read_failure;
        }
    }
    for (int k = 0; k < 4; k++) {
        out[k] = ((rgba >> (24 - k * 8)) & 255) / 255.0;
    }
    sgi_soft_texture_components(format, out);
    return true;

read_failure:
    sgi_soft_texture_report(r, 1, "image-read failure, fragment dropped");
    return false;
}

/* Reduce finite coordinates before any float-to-integer conversion. */
static double sgi_soft_texture_axis(double x, uint32_t n, uint32_t wrap)
{
    double half = 0.5 / n;

    switch (wrap) {
    case SGI_GL_REPEAT:
        return x - floor(x);
    case SGI_GL_CLAMP_TO_EDGE:
        return MIN(MAX(x, half), 1 - half);
    case SGI_GL_CLAMP_TO_BORDER:
        return MIN(MAX(x, -half), 1 + half);
    default:
        return MIN(MAX(x, 0), 1);
    }
}

/* Sampling always reduces coordinates before any integer conversion. */
static bool sgi_soft_texture_sample(const SGISoftDraw *r,
                                     const SGIGLTexLevel *level,
                                     uint32_t filter, double s, double t,
                                     double texel[4])
{
    const SGIGLTexImage *im = r->texture;
    uint32_t width = level ? level->width : im->width;
    uint32_t height = level ? level->height : im->height;

    memset(texel, 0, 4 * sizeof(*texel));
    s = sgi_soft_texture_axis(s, width, im->wrap_s);
    t = sgi_soft_texture_axis(t, height, im->wrap_t);
    if (filter == SGI_GL_NEAREST) {
        int64_t x = floor(width * s), y = floor(height * t);

        if (s == 1 && im->wrap_s != SGI_GL_CLAMP_TO_BORDER) {
            x = width - 1;
        }
        if (t == 1 && im->wrap_t != SGI_GL_CLAMP_TO_BORDER) {
            y = height - 1;
        }
        return sgi_soft_texture_tap(r, level, x, y, texel);
    }
    double u = width * s - 0.5, v = height * t - 0.5;
    int64_t x = floor(u), y = floor(v);
    double a = u - x, b = v - y;

    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            double weight = (i ? a : 1 - a) * (j ? b : 1 - b);
            double tap[4];

            if (!weight) {
                continue;
            }
            if (!sgi_soft_texture_tap(r, level, x + i, y + j, tap)) {
                return false;
            }
            for (int k = 0; k < 4; k++) {
                texel[k] += weight * tap[k];
            }
        }
    }
    return true;
}

typedef struct SGISoftTexSelection {
    uint32_t first, second, filter;
    double alpha, lambda;
} SGISoftTexSelection;

/* Relative levels avoid rounding a large absolute base in floating point. */
static bool sgi_soft_texture_select(uint32_t min, uint32_t mag,
                                     const SGIGLTexPyramid *p, uint32_t last,
                                     double rho, SGISoftTexSelection *out)
{
    double lambda, c = mag == SGI_GL_LINEAR &&
        (min == SGI_GL_NEAREST_MIPMAP_NEAREST ||
         min == SGI_GL_NEAREST_MIPMAP_LINEAR) ? 0.5 : 0;
    uint32_t range = last - p->base_level;

    if (!isfinite(rho) || rho < 0) {
        return false;
    }
    lambda = rho ? log2(rho) : p->min_lod;
    if (!isfinite(lambda)) {
        return false;
    }
    lambda = MIN(MAX(lambda, p->min_lod), p->max_lod);
    *out = (SGISoftTexSelection) {
        .first = p->base_level, .second = p->base_level,
        .filter = lambda <= c ? mag : min, .lambda = lambda,
    };
    if (lambda <= c || !sgi_soft_texture_mipmap(min)) {
        return true;
    }
    out->filter = min == SGI_GL_NEAREST_MIPMAP_NEAREST ||
                  min == SGI_GL_NEAREST_MIPMAP_LINEAR ? SGI_GL_NEAREST :
                                                      SGI_GL_LINEAR;
    if (min == SGI_GL_NEAREST_MIPMAP_NEAREST ||
        min == SGI_GL_LINEAR_MIPMAP_NEAREST) {
        double relative;

        if (lambda <= 0.5) {
            relative = 0;
        } else if (lambda > range + 0.5) {
            relative = range;
        } else {
            relative = floor(lambda);
            /* Adding 0.5 can round the next value above a half tie onto it. */
            relative += lambda - relative > 0.5;
        }
        out->first += (uint32_t)MIN(MAX(relative, 0), range);
        out->second = out->first;
    } else {
        double relative = MIN(MAX(lambda, 0), range);
        uint32_t n = floor(relative);

        out->first += n;
        out->second = out->first == last ? last : out->first + 1;
        out->alpha = relative - n;
    }
    return true;
}

/* Projective gradients precede wrapping; D is Q/W, not geometry W. */
static double sgi_soft_texture_lod_scale(double scale)
{
    return scale ? scale : 1.0;
}

static bool sgi_soft_texture_derivatives(const double value[3],
                                          const double dx[3],
                                          const double dy[3],
                                          double ns, double nt,
                                          double out[5])
{
    double d = value[2], d2 = d * d;

    if (!(d > 0) || !isfinite(d2) || !d2) {
        return false;
    }
    for (unsigned k = 0; k < 2; k++) {
        out[2 * k] = (dx[k] * d - value[k] * dx[2]) / d2;
        out[2 * k + 1] = (dy[k] * d - value[k] * dy[2]) / d2;
        if (!isfinite(value[k] / d) || !isfinite(out[2 * k]) ||
            !isfinite(out[2 * k + 1])) {
            return false;
        }
    }
    out[4] = MAX(hypot(ns * out[0], nt * out[2]),
                 hypot(ns * out[1], nt * out[3]));
    return isfinite(out[4]);
}

static void sgi_soft_texture_mix(double value[4], const double other[4],
                                  double alpha)
{
    for (unsigned k = 0; k < 4; k++) {
        value[k] = (1 - alpha) * value[k] + alpha * other[k];
    }
}

static bool sgi_soft_texture(const SGISoftDraw *r, SGISoftFrag *frag)
{
    const SGIGLTexImage *im = r->texture;
    const SGIGLDrawState *st = r->st;
    const SGIGLTexLevel *base = r->texture_base;
    uint32_t format = base ? base->base_format : im->base_format;
    double texel[4], s, t, q = frag->texcoord[3];

    if (!(q > 0) || !isfinite(q)) {
        sgi_soft_texture_report(r, 2, "invalid projected coordinates, "
                                "fragment dropped");
        return false;
    }
    s = frag->texcoord[0] / q;
    t = frag->texcoord[1] / q;
    if (!isfinite(s) || !isfinite(t)) {
        sgi_soft_texture_report(r, 2, "invalid projected coordinates, "
                                "fragment dropped");
        return false;
    }
    if (base) {
        SGISoftTexSelection selected;
        const SGIGLTexLevel *level;
        double other[4];

        if (!sgi_soft_texture_select(im->min_filter, im->mag_filter,
                                      im->pyramid, r->texture_last,
                                      frag->texture_rho, &selected)) {
            sgi_soft_texture_report(r, 8, "invalid footprint, "
                                          "fragment dropped");
            return false;
        }
        level = sgi_soft_texture_level(im->pyramid, selected.first);
        if (!sgi_soft_texture_sample(r, level, selected.filter, s, t, texel)) {
            return false;
        }
        if (selected.alpha && selected.first != selected.second) {
            level = sgi_soft_texture_level(im->pyramid, selected.second);
            if (!sgi_soft_texture_sample(r, level, selected.filter,
                                          s, t, other)) {
                return false;
            }
            sgi_soft_texture_mix(texel, other, selected.alpha);
        }
    } else if (!sgi_soft_texture_sample(r, NULL, im->min_filter, s, t, texel)) {
        return false;
    }
    if (im->post_filter) {
        im->post_filter(im->opaque, texel);
    }
    for (int k = 0; k < 4; k++) {
        bool alpha = k == 3;

        if ((!alpha && format == SGI_GL_ALPHA) ||
            (alpha && (format == SGI_GL_RGB ||
                       format == SGI_GL_LUMINANCE))) {
            continue;
        }
        switch (st->texture_env_mode) {
        case SGI_GL_REPLACE:
            frag->rgba[k] = texel[k];
            break;
        case SGI_GL_MODULATE:
            frag->rgba[k] *= texel[k];
            break;
        case SGI_GL_DECAL:
            if (!alpha) {
                frag->rgba[k] = format == SGI_GL_RGB ? texel[k] :
                    frag->rgba[k] * (1 - texel[3]) + texel[k] * texel[3];
            }
            break;
        case SGI_GL_BLEND:
            frag->rgba[k] = alpha && format != SGI_GL_INTENSITY ?
                frag->rgba[k] * texel[k] :
                frag->rgba[k] * (1 - texel[k]) +
                MIN(MAX(st->texture_env_color[k], 0), 1) * texel[k];
            break;
        }
    }
    return true;
}

/*
 * Fog (3.10): the fog factor f from the fragment's fog coordinate c by
 * the mode, clamped to [0, 1], blends an RGBA fragment's red, green and
 * blue toward the fog colour, f C + (1 - f) Cf, and adds (1 - f) times
 * the fog index to a colour index. A family's fog_factor, where set, gives
 * f from the window depth and fog coordinate instead. GL_LINEAR with start
 * equal to end divides by zero; the model takes f as 1 before the end and
 * 0 from it, the limit of the equation from either side.
 */
static void sgi_soft_fog(const SGISoftDraw *r, SGISoftFrag *frag, double z)
{
    const SGIGLDrawState *st = r->st;
    double c = frag->fog, f;

    if (r->t->fog_factor) {
        /* A family's own curve replaces the mode; the blend is the same. */
        f = r->t->fog_factor(r->t->opaque, z, c);
    } else if (st->fog_mode == SGI_GL_LINEAR) {
        f = st->fog_end == st->fog_start ? (c < st->fog_end ? 1 : 0) :
            (st->fog_end - c) / (st->fog_end - st->fog_start);
    } else if (st->fog_mode == SGI_GL_EXP2) {
        f = exp(-(st->fog_density * c) * (st->fog_density * c));
    } else {
        f = exp(-st->fog_density * c);
    }
    f = f >= 1 ? 1 : f > 0 ? f : 0;
    if (st->index_bits) {
        frag->index += (1 - f) * st->fog_index;
        return;
    }
    for (int k = 0; k < 3; k++) {
        frag->rgba[k] = f * frag->rgba[k] + (1 - f) * st->fog_color[k];
    }
}

/*
 * Whether a point whose client clip plane values are d lies outside an
 * enabled plane (2.11).
 */
static bool sgi_soft_clipped(const SGIGLDrawState *st, const double d[])
{
    for (int i = 0; i < SGI_GL_CLIP_PLANES; i++) {
        if ((st->clip_planes & (1u << i)) && !(d[i] >= 0)) {
            return true;
        }
    }
    return false;
}

/* A fragment's colour value from an interpolated or flat index. */
static uint32_t sgi_soft_index(const SGISoftDraw *r, double index)
{
    return sgi_gl_index_value(r->st->index_bits, index);
}

/*
 * One fragment at window pixel (px, py) with window depth z: fogged while
 * fog is enabled, dropped outside [0, 1], outside the window's rectangles
 * and, while the scissor test is enabled, outside the scissor box (4.1.2),
 * in RGBA mode alpha tested (4.1.4), stencil tested and depth tested (4.1.5)
 * when those tests are enabled, the stencil buffer updated by the operation
 * its outcome selects, blended in RGBA mode while blending is enabled or
 * combined by the logical operation while it is enabled, then written under
 * the index or colour mask to each colour target the draw buffers select.
 * Its colour is stored as the rules' packed colour in RGBA mode, dithered
 * after blending and before the logical operation while the draw dithers
 * (4.1.7), and as its index in colour-index mode, which glIndexMask masks
 * (4.2.2).
 */
static void sgi_soft_fragment_at(const SGISoftDraw *r, int px, int py,
                                 double z, const SGISoftFrag *frag)
{
    const SGIRasterTarget *t = r->t;
    SGISoftFrag fogged = *frag;
    const SGISoftFrag *f = &fogged;
    int x = r->win.ox + px, y = r->win.oy + py;
    uint32_t zq, colour, smax = 0, sref = 0;
    bool stencil;
    SGIRasterColour dest[SGI_GL_MAX_TARGETS] = { 0 };

    if (!(z >= 0.0 && z <= 1.0)) {
        return;
    }
    if (r->texture && !sgi_soft_texture(r, &fogged)) {
        return;
    }
    if (r->st->fog) {
        sgi_soft_fog(r, &fogged, z);
    }
    if (r->win.clip->n > 1 && !sgi_clip_contains(r->win.clip, x, y)) {
        return;
    }
    if (r->st->scissor_test &&
        !(px >= r->st->scissor[0] &&
          (int64_t)px < (int64_t)r->st->scissor[0] + r->st->scissor[2] &&
          py >= r->st->scissor[1] &&
          (int64_t)py < (int64_t)r->st->scissor[1] + r->st->scissor[3])) {
        return;
    }
    /*
     * Incoming alpha and reference use at least two bits and at least the
     * stored alpha width, independently of the colour mask (4.1.4, 2.13.9).
     */
    if (r->st->alpha_test && !r->st->index_bits &&
        !sgi_soft_compare(r->st->alpha_func,
                          sgi_soft_alpha(r->rules, f->rgba[3]),
                          sgi_soft_alpha(r->rules, r->st->alpha_ref))) {
        return;
    }
    if (!r->st->index_bits && t->store_rgba && r->rgba_read &&
        !sgi_soft_rgba_load(r->rules, t, r->targets, r->ntargets, x, y,
                            dest, r->rgba_reported)) {
        return;
    }
    /*
     * The reference clamps to the buffer's range; with no stencil buffer
     * the test passes and nothing is written.
     */
    stencil = r->st->stencil_test && sgi_soft_stencil_max(r->rules, t, &smax);
    if (stencil) {
        uint32_t vmask = r->st->stencil_value_mask;

        sref = r->st->stencil_ref < 0 ? 0 : MIN((uint32_t)r->st->stencil_ref,
                                                smax);
        if (!sgi_soft_compare(r->st->stencil_func, sref & vmask,
                              t->load_stencil(t->opaque, x, y) & smax &
                              vmask)) {
            sgi_soft_stencil_op(r, x, y, r->st->stencil_op[0], smax, sref);
            return;
        }
    }
    zq = (uint32_t)sgi_soft_round(r->rules,
                                  z * sgi_soft_depth_max(r->rules));
    if (r->st->depth_test) {
        if (!sgi_soft_compare(r->st->depth_func, zq,
                              t->load_depth(t->opaque, x, y))) {
            if (stencil) {
                sgi_soft_stencil_op(r, x, y, r->st->stencil_op[1], smax,
                                    sref);
            }
            return;
        }
        if (r->st->depth_mask) {
            t->store_depth(t->opaque, x, y, zq);
        }
    }
    /* A disabled depth test passes (4.1.5). */
    if (stencil) {
        sgi_soft_stencil_op(r, x, y, r->st->stencil_op[2], smax, sref);
    }
    if (!r->st->index_bits && t->store_rgba) {
        for (unsigned i = 0; r->rgba_mask && i < r->ntargets; i++) {
            SGIRasterColour out = sgi_soft_rgba_result(r, x, y, f, &dest[i]);

            t->store_rgba(t->opaque, r->targets[i], x, y, &out);
        }
        return;
    }
    /*
     * Dithering follows blending (4.1.7), so a colour to be blended is
     * rounded and the blend's result dithered.
     */
    colour = r->st->index_bits ? sgi_soft_index(r, f->index) :
             r->blend ? sgi_soft_rgb(r->rules, f->rgba) :
                        sgi_soft_store_rgb(r, x, y, f->rgba);
    for (uint32_t i = 0; i < r->ntargets; i++) {
        uint32_t out = colour;

        if (r->logic_op) {
            uint32_t bits = r->st->index_bits ? r->st->index_bits :
                                                3 * r->rules->color_bits;

            out = sgi_soft_logic_op(r->st->logic_op_mode, colour,
                                    t->load_colour(t->opaque, r->targets[i],
                                                   x, y)) &
                  (bits >= 32 ? UINT32_MAX : (1u << bits) - 1);
        }
        if (r->st->index_bits) {
            t->store_index(t->opaque, r->targets[i], x, y, out,
                           r->st->index_mask);
            continue;
        }
        if (!r->colour_mask) {
            continue;
        }
        if (r->blend) {
            out = sgi_soft_blend(r, x, y, colour,
                                 sgi_soft_alpha(r->rules, f->rgba[3]) /
                                 (double)sgi_soft_alpha_max(r->rules),
                                 t->load_colour(t->opaque, r->targets[i],
                                                x, y));
        }
        if (r->colour_mask != sgi_soft_colour_bits(r->rules)) {
            out = (out & r->colour_mask) |
                  (t->load_colour(t->opaque, r->targets[i], x, y) &
                   ~r->colour_mask);
        }
        t->store_colour(t->opaque, r->targets[i], x, y, out);
    }
}

/*
 * The depth range [n, f] normalized depth is mapped through (2.10.1): the
 * draw state's, or [0, 1] under a window matrix, whose depth is already
 * window depth.
 */
static void sgi_soft_depth_range(const SGIGLDrawState *st, double *n,
                                 double *f)
{
    *n = st->window_matrix_given ? 0 : st->depth_range[0];
    *f = st->window_matrix_given ? 1 : st->depth_range[1];
}

/*
 * The window pixels a primitive's fragments may fall on, inclusive: the
 * bounds of those the window owns, within the viewport unless a window
 * matrix replaces it. False when there are none.
 */
static bool sgi_soft_bounds(const SGISoftDraw *r, int *x0, int *y0, int *x1,
                            int *y1)
{
    const SGIGLDrawState *st = r->st;
    const SGISoftWindow *win = &r->win;
    int vx = st->viewport[0], vy = st->viewport[1];
    int vw = st->viewport[2], vh = st->viewport[3];

    *x0 = win->x0 - win->ox;
    *y0 = win->y0 - win->oy;
    *x1 = win->x1 - win->ox;
    *y1 = win->y1 - win->oy;
    if (st->window_matrix_given) {
        return true;
    }
    if (vw <= 0 || vh <= 0) {
        return false;
    }
    *x0 = MAX(*x0, vx);
    *y0 = MAX(*y0, vy);
    *x1 = MIN(*x1, vx + vw - 1);
    *y1 = MIN(*y1, vy + vh - 1);
    return true;
}

/*
 * A primitive's fragment at normalized depth z, (zd + 1) / 2 for the
 * normalized device z zd: dropped outside [0, 1], the view volume's near
 * and far planes, which clipping has already cut the primitive to, and
 * otherwise mapped to window depth n + (f - n) z through the depth range
 * [n, f] (OpenGL 1.2.1, 2.10.1). A polygon's offset is then added to the
 * window depth, and the sum clamped to [0, 1] (3.5.5).
 */
static void sgi_soft_fragment(const SGISoftDraw *r, int px, int py, double z,
                              double offset, const SGISoftFrag *frag)
{
    double n, f, zw;

    sgi_soft_depth_range(r->st, &n, &f);
    if (!(z >= 0.0 && z <= 1.0)) {
        return;
    }
    zw = n + (f - n) * z + offset;
    sgi_soft_fragment_at(r, px, py, zw >= 1 ? 1 : zw > 0 ? zw : 0, frag);
}

/*
 * A triangle's polygon offset (3.5.5), o = m factor + r units, where m is
 * the larger magnitude of its window depth's slopes in x and y, which the
 * specification allows in place of their length, and r the rules' one
 * depth step; EXT_polygon_offset's bias adds in window depth. Boundary modes
 * share this slope calculation and select their own offset enable.
 */
static double sgi_soft_slope(const SGISoftDraw *r, const SGISoftVertex v[3])
{
    const double sub = 1 << r->rules->subpixel_bits;
    double x1, y1, z1, x2, y2, z2, det, n, f;

    sgi_soft_depth_range(r->st, &n, &f);
    x1 = (v[1].x - v[0].x) / sub, y1 = (v[1].y - v[0].y) / sub;
    x2 = (v[2].x - v[0].x) / sub, y2 = (v[2].y - v[0].y) / sub;
    z1 = v[1].z - v[0].z, z2 = v[2].z - v[0].z;
    det = x1 * y2 - x2 * y1;
    /* Reviewed model choice for an entirely degenerate boundary: m = 0. */
    if (det == 0) {
        return 0;
    }
    return MAX(fabs(z1 * y2 - z2 * y1), fabs(x1 * z2 - x2 * z1)) /
           fabs(det) * fabs(f - n);
}

static double sgi_soft_offset(const SGISoftDraw *r, const SGISoftVertex v[3])
{
    const SGIGLDrawState *st = r->st;

    if (!(st->polygon_offset & SGI_GL_OFFSET_FILL)) {
        return 0;
    }
    return sgi_soft_slope(r, v) * st->offset_factor +
           st->offset_units / sgi_soft_depth_max(r->rules) + st->offset_bias;
}

/*
 * The six planes of the view volume (OpenGL 1.2.1, 2.11), -w <= x, y,
 * z <= w, as each one's value at a point, which is >= 0 inside it.
 */
#define SGI_VOLUME_PLANES 6

static double sgi_volume_plane(const SGIVolumeVertex *v, int plane)
{
    double c = v->clip[plane / 2];

    return plane & 1 ? v->clip[3] - c : v->clip[3] + c;
}

bool sgi_volume_contains(const SGIVolumeVertex *v)
{
    for (int i = 0; i < SGI_VOLUME_PLANES; i++) {
        if (!(sgi_volume_plane(v, i) >= 0)) {
            return false;
        }
    }
    return true;
}

/*
 * The point at t along from in to out, in clip and eye coordinates and
 * every float attribute (2.13.8); the material index and edge flag are
 * bit patterns and stay in's.
 */
static void sgi_volume_lerp(const SGIVolumeVertex *in,
                            const SGIVolumeVertex *out, double t,
                            SGIVolumeVertex *r)
{
    for (int k = 0; k < 4; k++) {
        r->clip[k] = in->clip[k] + t * (out->clip[k] - in->clip[k]);
        r->eye[k] = in->eye[k] + t * (out->eye[k] - in->eye[k]);
    }
    for (int k = 0; k < SGI_GL_VERTEX_FLOATS; k++) {
        r->attr[k] = k == SGI_GL_V_MATERIAL || k == SGI_GL_V_EDGE ?
                     in->attr[k] :
                     in->attr[k] + t * (out->attr[k] - in->attr[k]);
    }
}

/*
 * Where the edge from in, inside a plane with value din, to out, outside
 * it with value dout, meets the plane. It is always found from the inside
 * end, so that two primitives sharing the edge make the same vertex.
 */
static void sgi_volume_cut(const SGIVolumeVertex *in, double din,
                           const SGIVolumeVertex *out, double dout,
                           SGIVolumeVertex *r)
{
    sgi_volume_lerp(in, out, din / (din - dout), r);
}

/*
 * Sutherland and Hodgman's clipping, one plane at a time: each edge
 * keeps its inside end and adds the point where it crosses the plane.
 * A value that is not a number is outside.
 */
unsigned sgi_volume_clip_triangle(const SGIVolumeVertex in[3],
                                  SGIVolumeVertex out[SGI_VOLUME_TRIANGLE_MAX])
{
    SGIVolumeVertex buf[SGI_VOLUME_TRIANGLE_MAX];
    SGIVolumeVertex *src = out, *dst = buf, *t;
    unsigned n = 3;

    memcpy(out, in, 3 * sizeof(in[0]));
    if (sgi_volume_contains(&in[0]) && sgi_volume_contains(&in[1]) &&
        sgi_volume_contains(&in[2])) {
        return 3;
    }
    /* Six planes swap the buffers six times, ending in out. */
    for (int p = 0; p < SGI_VOLUME_PLANES; p++) {
        unsigned m = 0;

        for (unsigned i = 0; i < n; i++) {
            const SGIVolumeVertex *a = &src[i], *b = &src[(i + 1) % n];
            double da = sgi_volume_plane(a, p), db = sgi_volume_plane(b, p);

            if (da >= 0) {
                dst[m++] = *a;
                if (!(db >= 0)) {
                    sgi_volume_cut(a, da, b, db, &dst[m++]);
                }
            } else if (db >= 0) {
                sgi_volume_cut(b, db, a, da, &dst[m++]);
            }
        }
        n = m;
        t = src, src = dst, dst = t;
        if (n < 3) {
            return 0;
        }
    }
    return n;
}

/*
 * Liang and Barsky's clipping: the segment keeps the part every plane
 * allows. Each end that a plane bounds moves to where the segment meets
 * that plane, found from the other end, inside it, as a triangle's cut
 * is: a fraction sa of the way from b to a, and tb from a to b.
 */
bool sgi_volume_clip_segment(SGIVolumeVertex *a, SGIVolumeVertex *b)
{
    SGIVolumeVertex na = *a, nb = *b;
    double sa = 1, tb = 1;

    for (int p = 0; p < SGI_VOLUME_PLANES; p++) {
        double da = sgi_volume_plane(a, p), db = sgi_volume_plane(b, p);

        if (da >= 0 && db >= 0) {
            continue;
        }
        if (!(da >= 0) && !(db >= 0)) {
            return false;
        }
        if (da >= 0) {
            tb = MIN(tb, da / (da - db));
        } else {
            sa = MIN(sa, db / (db - da));
        }
    }
    if (!(sa + tb >= 1)) {
        return false;
    }
    if (sa < 1) {
        sgi_volume_lerp(b, a, sa, &na);
    }
    if (tb < 1) {
        sgi_volume_lerp(a, b, tb, &nb);
    }
    *a = na;
    *b = nb;
    return true;
}

/*
 * One vertex to clip coordinates (OpenGL 1.2.1, 2.10): object to eye by
 * the modelview and eye to clip by the projection, or by the window
 * matrix as sgi_gl_eye_to_clip() takes it.
 */
static void sgi_soft_to_volume(const SGISoftDraw *r, const float *v,
                               SGIVolumeVertex *out)
{
    const SGIGLDrawState *st = r->st;

    for (int row = 0; row < 4; row++) {
        out->eye[row] = 0;
        for (int k = 0; k < 4; k++) {
            out->eye[row] += st->modelview[k * 4 + row] *
                             (double)v[SGI_GL_V_POSITION + k];
        }
    }
    sgi_gl_eye_to_clip(st, out->eye, out->clip);
    memcpy(out->attr, v, sizeof(out->attr));
}

/*
 * Map one clipped vertex (2.10): the divide by w and the viewport map,
 * with depth left normalized to [0, 1]; the depth range maps it per
 * fragment. Under a window matrix the position is the matrix's and the
 * depth its window depth. Clipping has put the vertex in the view volume,
 * so its normalized depth is clamped to [0, 1] against rounding. The colour is
 * the front colour; a back-facing triangle takes its back colour later.
 * A vertex with w <= 0 is in the volume only at the clip-space origin,
 * where the divide has no answer, and returns false, as does one mapped
 * beyond the rasterizer's range.
 */
static bool sgi_soft_project(const SGISoftDraw *r, const SGIVolumeVertex *cv,
                             SGISoftVertex *out)
{
    const SGIGLDrawState *st = r->st;
    const double *clip = cv->clip, *eye = cv->eye;
    double win[3], xw, yw, z;

    if (!sgi_soft_texture_vertex_valid(r, cv)) {
        return false;
    }
    if (!(clip[3] > 0)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: clipped vertex at w %g has no "
                      "window position, primitive dropped\n", clip[3]);
        return false;
    }
    sgi_gl_clip_to_window(st, clip, win);
    xw = win[0];
    yw = win[1];
    if (!(fabs(xw) < 65536.0 && fabs(yw) < 65536.0)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: vertex outside the rasterizer's "
                      "range, primitive dropped\n");
        return false;
    }
    for (int k = 0; k < 4; k++) {
        out->texcoord[k] = cv->attr[SGI_GL_V_TEXCOORD + k];
    }
    out->x = sgi_soft_round(r->rules, xw * (1 << r->rules->subpixel_bits));
    out->y = sgi_soft_round(r->rules, yw * (1 << r->rules->subpixel_bits));
    z = win[2];
    out->z = z >= 1 ? 1 : z > 0 ? z : 0;
    out->inv_w = 1.0 / clip[3];
    /*
     * The fog coordinate approximates the eye distance by |z|, which
     * 3.10 allows; it is affine in eye space, so interpolating it as a
     * vertex attribute gives it exactly at each fragment.
     */
    out->fog = fabs(eye[3] != 0 ? eye[2] / eye[3] : eye[2]);
    for (int i = 0; i < SGI_GL_CLIP_PLANES; i++) {
        const float *p = st->clip_plane[i];

        out->clip[i] = p[0] * eye[0] + p[1] * eye[1] + p[2] * eye[2] +
                       p[3] * eye[3];
    }
    for (int k = 0; k < 4; k++) {
        out->rgba[k] = MIN(MAX(cv->attr[SGI_GL_V_COLOR + k], 0.0f), 1.0f);
    }
    out->index = cv->attr[SGI_GL_V_INDEX];
    return true;
}

typedef struct SGISoftTexPlane {
    double value[3], dx[3], dy[3];
    double x, y;
} SGISoftTexPlane;

static void sgi_soft_texture_values(const SGISoftVertex *v, double value[3])
{
    value[0] = v->texcoord[0] * v->inv_w;
    value[1] = v->texcoord[1] * v->inv_w;
    value[2] = v->texcoord[3] * v->inv_w;
}

static bool sgi_soft_texture_plane(const SGISoftVertex v[3], unsigned sub,
                                   SGISoftTexPlane *g)
{
    double scale = 1 << sub;
    double x1 = (v[1].x - v[0].x) / scale;
    double y1 = (v[1].y - v[0].y) / scale;
    double x2 = (v[2].x - v[0].x) / scale;
    double y2 = (v[2].y - v[0].y) / scale;
    double area = x1 * y2 - x2 * y1;
    double values[3][3];

    *g = (SGISoftTexPlane) { .x = v[0].x / scale, .y = v[0].y / scale };
    if (!area) {
        return true;
    }
    if (!isfinite(area)) {
        return false;
    }
    for (unsigned i = 0; i < 3; i++) {
        sgi_soft_texture_values(&v[i], values[i]);
    }
    for (unsigned k = 0; k < 3; k++) {
        double a = values[1][k] - values[0][k];
        double b = values[2][k] - values[0][k];

        g->value[k] = values[0][k];
        g->dx[k] = (a * y2 - b * y1) / area;
        g->dy[k] = (b * x1 - a * x2) / area;
        if (!isfinite(g->value[k]) || !isfinite(g->dx[k]) ||
            !isfinite(g->dy[k])) {
            return false;
        }
    }
    return true;
}

static bool sgi_soft_texture_line_gradient(const SGISoftVertex *a,
                                           const SGISoftVertex *b, unsigned sub,
                                           double values[3], double delta[3],
                                           double *length)
{
    double end[3];

    *length = hypot((double)(b->x - a->x), (double)(b->y - a->y)) /
              (1 << sub);
    if (!isfinite(*length)) {
        return false;
    }
    sgi_soft_texture_values(a, values);
    sgi_soft_texture_values(b, end);
    for (unsigned k = 0; k < 3; k++) {
        delta[k] = end[k] - values[k];
        if (!isfinite(values[k]) || !isfinite(delta[k])) {
            return false;
        }
    }
    return true;
}

static bool sgi_soft_texture_coefficients(const SGISoftDraw *r,
                                           const SGISoftVertex v[3])
{
    SGISoftTexPlane g;

    if (!r->texture_base || sgi_soft_texture_plane(v, r->rules->subpixel_bits,
                                                   &g)) {
        return true;
    }
    sgi_soft_texture_report(r, 16, "invalid primitive coefficients, "
                                  "primitive dropped");
    return false;
}

/*
 * Map and rasterize one triangle of a clipped polygon, given its three
 * vertices in the view volume and, for flat shading, the provoking
 * vertex of the triangle it was clipped from (OpenGL 1.2.1, 2.13.7 and
 * table 2.9).
 *
 * Facing (2.13.1) is the sign of the triangle's area in window
 * coordinates on the subpixel grid: a positive area is front facing under
 * glFrontFace(GL_CCW) and a negative one under GL_CW. Under a window
 * matrix whose rows run down the display, window y runs opposite to the
 * client's, which mirrors the winding, so the area's sign is taken
 * negated: a triangle counter-clockwise as the client drew it is
 * counter-clockwise here too, for culling and two-sided colour alike.
 * With GL_CULL_FACE enabled a triangle whose facing glCullFace names is
 * not rasterized (3.5.1). When the draw carries back colours a
 * back-facing triangle takes them. A polygon or quad is drawn as
 * triangles, each facing by its own area, which for a convex polygon is
 * the polygon's facing; so is each triangle of a clipped one. The tie
 * rule still names edges in window rows as they run, so under rows down
 * the display SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN's owned edges are the
 * bottom and left ones on the display.
 *
 * The fragments are also bounded by the viewport rectangle, when there
 * is one, which clipping has already made them lie in; a normalized depth
 * outside [0, 1] could now come only from rounding, and is still dropped.
 *
 * Depth is interpolated linearly in window coordinates and colour with
 * the perspective correction of equation 3.4 (OpenGL 1.2.1, 3.5.1).
 */
static void sgi_soft_clipped_triangle(const SGISoftDraw *r,
                                      const SGIVolumeVertex *va,
                                      const SGIVolumeVertex *vb,
                                      const SGIVolumeVertex *vc,
                                      const float *provoking)
{
    const SGIGLDrawState *st = r->st;
    const SGIVolumeVertex *verts[3] = { va, vb, vc };
    const float *flat = &provoking[SGI_GL_V_COLOR];
    SGISoftFrag flat_frag;
    SGISoftVertex v[3];
    SGISoftTexPlane gradient;
    int64_t min_x, max_x, min_y, max_y, area;
    double offset;
    bool ccw, front;
    int px0, px1, py0, py1;
    bool smooth = st->shade_model != SGI_GL_FLAT;
    const int sub = r->rules->subpixel_bits;

    for (int i = 0; i < 3; i++) {
        if (!sgi_soft_project(r, verts[i], &v[i])) {
            return;
        }
    }

    area = sgi_soft_edge(&v[0], &v[1], v[2].x, v[2].y);
    if (area == 0) {
        return;
    }
    if (r->texture_base &&
        !sgi_soft_texture_plane(v, sub, &gradient)) {
        sgi_soft_texture_report(r, 16, "invalid primitive coefficients, "
                                      "primitive dropped");
        return;
    }
    ccw = st->window_matrix_given && st->window_rows_down ? area < 0 :
                                                            area > 0;
    front = st->front_face == SGI_GL_CW ? !ccw : ccw;
    if (st->cull_face &&
        (st->cull_face_mode == SGI_GL_FRONT_AND_BACK ||
         st->cull_face_mode == (front ? SGI_GL_FRONT : SGI_GL_BACK))) {
        return;
    }
    if (!front && r->back_colors) {
        flat = &provoking[SGI_GL_V_BACK_COLOR];
        for (int i = 0; i < 3; i++) {
            const float *back = &verts[i]->attr[SGI_GL_V_BACK_COLOR];

            for (int k = 0; k < 4; k++) {
                v[i].rgba[k] = MIN(MAX(back[k], 0.0f), 1.0f);
            }
        }
    }
    for (int k = 0; k < 4; k++) {
        flat_frag.rgba[k] = flat[k];
    }
    flat_frag.index = provoking[SGI_GL_V_INDEX];
    offset = sgi_soft_offset(r, v);
    if (area < 0) {
        /* Rasterize either winding the same way from here. */
        SGISoftVertex t = v[1];

        v[1] = v[2];
        v[2] = t;
        area = -area;
    }

    min_x = MIN(v[0].x, MIN(v[1].x, v[2].x)) >> sub;
    max_x = MAX(v[0].x, MAX(v[1].x, v[2].x)) >> sub;
    min_y = MIN(v[0].y, MIN(v[1].y, v[2].y)) >> sub;
    max_y = MAX(v[0].y, MAX(v[1].y, v[2].y)) >> sub;

    /* Window pixels, bounded by the viewport and the owned rectangle. */
    if (!sgi_soft_bounds(r, &px0, &py0, &px1, &py1)) {
        return;
    }
    px0 = MAX((int)min_x, px0);
    px1 = MIN((int)max_x, px1);
    py0 = MAX((int)min_y, py0);
    py1 = MIN((int)max_y, py1);

    for (int py = py0; py <= py1; py++) {
        int64_t cy = ((int64_t)py * (1 << sub)) +
                     (1 << (sub - 1));

        for (int px = px0; px <= px1; px++) {
            int64_t cx = ((int64_t)px * (1 << sub)) +
                         (1 << (sub - 1));
            int64_t e0 = sgi_soft_edge(&v[1], &v[2], cx, cy);
            int64_t e1 = sgi_soft_edge(&v[2], &v[0], cx, cy);
            int64_t e2 = sgi_soft_edge(&v[0], &v[1], cx, cy);
            double b1, b2, z, w0, w1, w2, sum;
            SGISoftFrag frag = flat_frag;

            if (!sgi_soft_owns(&v[1], &v[2], e0) ||
                !sgi_soft_owns(&v[2], &v[0], e1) ||
                !sgi_soft_owns(&v[0], &v[1], e2)) {
                continue;
            }
            /* Window phase precedes plane translation and fragment work. */
            if (r->polygon_stipple &&
                !(r->polygon_stipple[(uint32_t)py & 31] &
                  (1u << ((uint32_t)px & 31)))) {
                continue;
            }
            b1 = (double)e1 / area;
            b2 = (double)e2 / area;
            z = v[0].z + b1 * (v[1].z - v[0].z) + b2 * (v[2].z - v[0].z);
            w0 = (1.0 - b1 - b2) * v[0].inv_w;
            w1 = b1 * v[1].inv_w;
            w2 = b2 * v[2].inv_w;
            sum = w0 + w1 + w2;
            for (int k = 0; k < 4; k++) {
                frag.texcoord[k] = (w0 * v[0].texcoord[k] +
                                    w1 * v[1].texcoord[k] +
                                    w2 * v[2].texcoord[k]) / sum;
            }
            frag.fog = (w0 * v[0].fog + w1 * v[1].fog + w2 * v[2].fog) / sum;
            if (st->clip_planes) {
                double d[SGI_GL_CLIP_PLANES];

                for (int i = 0; i < SGI_GL_CLIP_PLANES; i++) {
                    d[i] = (w0 * v[0].clip[i] + w1 * v[1].clip[i] +
                            w2 * v[2].clip[i]) / sum;
                }
                if (sgi_soft_clipped(st, d)) {
                    continue;
                }
            }
            if (smooth) {
                for (int k = 0; k < 4; k++) {
                    frag.rgba[k] = (w0 * v[0].rgba[k] + w1 * v[1].rgba[k] +
                                    w2 * v[2].rgba[k]) / sum;
                }
                /*
                 * An index is taken as a difference from the first
                 * vertex's, so a constant one stays exact before its
                 * integer part is taken.
                 */
                frag.index = v[0].index +
                             (w1 * (v[1].index - v[0].index) +
                              w2 * (v[2].index - v[0].index)) / sum;
            }
            if (r->texture_base) {
                double value[3], out[5];

                for (unsigned k = 0; k < 3; k++) {
                    value[k] = gradient.value[k] +
                               (px + 0.5 - gradient.x) * gradient.dx[k] +
                               (py + 0.5 - gradient.y) * gradient.dy[k];
                }
                frag.texture_rho = sgi_soft_texture_derivatives(value,
                    gradient.dx, gradient.dy, r->texture_base->width *
                    sgi_soft_texture_lod_scale(r->texture->lod_scale_s),
                    r->texture_base->height *
                    sgi_soft_texture_lod_scale(r->texture->lod_scale_t),
                    out) ? out[4] : NAN;
            }
            sgi_soft_fragment(r, px, py, z, offset, &frag);
        }
    }
}

/*
 * Transform, clip and rasterize one triangle. It is clipped to the view
 * volume in clip coordinates, before the divide by w (OpenGL 1.2.1,
 * 2.11), and what is left, a convex polygon, is drawn as a fan of
 * triangles from its first vertex, each flat shaded from the triangle's
 * provoking vertex.
 *
 * Approximated: the clipping is OpenGL's, in double arithmetic, not a
 * head's own clipper, whose arithmetic and any guard band are not known.
 * Pixels next to the edges clipping makes, and the attributes at them,
 * may differ from a head's; how a head draws a primitive crossing the
 * near plane would settle it.
 *
 * Under a window matrix there is no clip space of the projection's own,
 * so the clipping is in the window matrix's homogeneous output, the clip
 * space composed with the viewport map, which clipping commutes with.
 * The near and far planes are OpenGL's, window depth 0 and 1, and keep
 * w positive as OpenGL's do. The side planes, which the viewport would
 * place and the window matrix does not say, become a guard band of
 * SGI_GL_WINDOW_GUARD pixels about the drawable's origin, and the
 * window's own pixels and the scissor box bound what is drawn. That
 * matches OpenGL wherever the viewport the client meant covers the
 * window; where a head clips x and y is not known.
 */
static bool sgi_soft_clip_triangle(const SGISoftDraw *r, const float *va,
                                   const float *vb, const float *vc,
                                   SGIVolumeVertex *poly, unsigned *n)
{
    const float *verts[3] = { va, vb, vc };
    SGIVolumeVertex in[3];

    for (int i = 0; i < 3; i++) {
        sgi_soft_to_volume(r, verts[i], &in[i]);
    }
    *n = sgi_volume_clip_triangle(in, poly);
    for (unsigned i = 0; i < *n; i++) {
        if (!sgi_soft_texture_vertex_valid(r, &poly[i])) {
            return false;
        }
    }
    if (r->texture_base) {
        for (unsigned i = 1; i + 1 < *n; i++) {
            const SGIVolumeVertex *vertices[3] = {
                &poly[0], &poly[i], &poly[i + 1],
            };
            SGISoftVertex projected[3];

            for (unsigned j = 0; j < 3; j++) {
                if (!sgi_soft_project(r, vertices[j], &projected[j])) {
                    return false;
                }
            }
            if (!sgi_soft_texture_coefficients(r, projected)) {
                return false;
            }
        }
    }
    return true;
}

/* A quad or polygon validates all its clipped pieces before any are drawn. */
static bool sgi_soft_texture_triangle_valid(const SGISoftDraw *r,
                                           const float *a, const float *b,
                                           const float *c)
{
    SGIVolumeVertex poly[SGI_VOLUME_TRIANGLE_MAX];
    unsigned n;

    return sgi_soft_clip_triangle(r, a, b, c, poly, &n);
}

static void sgi_soft_triangle(const SGISoftDraw *r, const float *va,
                              const float *vb, const float *vc,
                              const float *provoking)
{
    SGIVolumeVertex poly[SGI_VOLUME_TRIANGLE_MAX];
    unsigned n;
    int x0, y0, x1, y1;

    if (!sgi_soft_bounds(r, &x0, &y0, &x1, &y1) ||
        !sgi_soft_clip_triangle(r, va, vb, vc, poly, &n)) {
        return;
    }
    for (unsigned i = 1; i + 1 < n; i++) {
        sgi_soft_clipped_triangle(r, &poly[0], &poly[i], &poly[i + 1],
                                  provoking);
    }
}

/* Sign of v + e1 * epsilon + e2 * epsilon squared as epsilon -> 0+. */
static int sgi_soft_limit_sign(int64_t v, int64_t e1, int64_t e2)
{
    int64_t first = v ? v : e1 ? e1 : e2;

    return (first > 0) - (first < 0);
}

/* Whether |v + e1*epsilon + e2*epsilon squared| is strictly below bound. */
static bool sgi_soft_limit_abs(int64_t v, int64_t e1, int64_t e2,
                               int64_t bound)
{
    int sign = sgi_soft_limit_sign(v, e1, e2);

    return sgi_soft_limit_sign(sign * v - bound,
                               sign * e1, sign * e2) < 0;
}

/* The endpoint minus (epsilon, epsilon squared) lies in the open diamond. */
static bool sgi_soft_diamond_contains(int64_t x, int64_t y, int64_t half)
{
    int sx = sgi_soft_limit_sign(x, -1, 0);
    int sy = sgi_soft_limit_sign(y, 0, -1);

    return sgi_soft_limit_sign(sx * x + sy * y - half, -sx, -sy) < 0;
}

/*
 * OpenGL 1.2.1, 3.4.1 subtracts (epsilon, epsilon squared) from both
 * endpoints. Lexicographic coefficients implement that limiting perturbation
 * on the snapped grid, including even-width shifts on diamond boundaries.
 * A segment produces a fragment if it enters the open diamond and its final
 * endpoint is outside. The minimum L1 distance occurs at an endpoint or a
 * centre-row/column crossing.
 *
 * Projected coordinates have magnitude at most 2^28 (65536 pixels, at most
 * 12 subpixel bits). The scanner tests base pixels within two pixels of the
 * shifted segment's minor span. Subtracting the identical width shift leaves
 * relative coordinates below 2^30, even for INT32_MAX width. Segment deltas
 * are at most 2^29, so the difference of two products fits signed 64 bits.
 */
static bool sgi_soft_line_covers(const SGISoftVertex *a,
                                 const SGISoftVertex *b, int64_t px,
                                 int64_t py, int sub)
{
    const int64_t half = 1 << (sub - 1);
    int64_t cx = px * (1 << sub) + half;
    int64_t cy = py * (1 << sub) + half;
    int64_t dx = b->x - a->x, dy = b->y - a->y;
    int64_t ex = cx - a->x, ey = cy - a->y;

    if (sgi_soft_diamond_contains(b->x - cx, b->y - cy, half)) {
        return false;
    }
    if (sgi_soft_diamond_contains(-ex, -ey, half)) {
        return true;
    }
    /* The perturbed column crossing has parameter (ex + epsilon) / dx. */
    if (dx && sgi_soft_limit_sign(ex, 1, 0) * (dx > 0 ? 1 : -1) >= 0 &&
        sgi_soft_limit_sign(dx - ex, -1, 0) * (dx > 0 ? 1 : -1) >= 0 &&
        sgi_soft_limit_abs(dy * ex - ey * dx, dy, -dx,
                           half * llabs(dx))) {
        return true;
    }
    return dy &&
           sgi_soft_limit_sign(ey, 0, 1) * (dy > 0 ? 1 : -1) >= 0 &&
           sgi_soft_limit_sign(dy - ey, 0, -1) * (dy > 0 ? 1 : -1) >= 0 &&
           sgi_soft_limit_abs(dx * ey - ex * dy, -dy, dx,
                              half * llabs(dy));
}

/*
 * Transform, clip, map and rasterize one aliased segment (OpenGL 1.2.1,
 * 3.4.1) with the provoking vertex's colour when flat shaded. The segment
 * is clipped to the view volume in clip coordinates (2.11), approximated
 * as a triangle is. A fragment's depth and smooth colour come from t, the
 * centre's projection onto the clipped segment (equations 3.5 to 3.7),
 * the colour corrected for perspective. The fragments are also bounded by
 * the viewport, when there is one. Width follows the minor-axis shift and
 * replication of 3.4.3. Polygon boundaries count stipple in submitted
 * direction; activated standalone lines count across streamed segments.
 * Antialiasing is not modelled.
 */
typedef struct SGISoftLineStipple {
    uint32_t counter;
    uint32_t repeat, pattern;
    bool enabled, consumed;
} SGISoftLineStipple;

static void sgi_soft_line_projected(const SGISoftDraw *r,
                                    const SGISoftVertex *pa,
                                    const SGISoftVertex *pb,
                                    const float *provoking, double offset,
                                    SGISoftLineStipple *stipple,
                                    bool client_clip)
{
    const SGIGLDrawState *st = r->st;
    bool smooth = st->shade_model != SGI_GL_FLAT;
    const int sub = r->rules->subpixel_bits;
    bool x_major;
    SGISoftVertex a = *pa, b = *pb, shifted_a, shifted_b;
    bool visible;
    int step;
    double flat[4], len2;
    double texture_value[3], texture_delta[3], texture_length;
    int major0, major1, bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;

    visible = sgi_soft_bounds(r, &bx0, &by0, &bx1, &by1);
    if ((!visible && !stipple) || (a.x == b.x && a.y == b.y)) {
        return;
    }
    if (r->texture_base && !sgi_soft_texture_line_gradient(&a, &b, sub,
            texture_value, texture_delta, &texture_length)) {
        sgi_soft_texture_report(r, 16, "invalid primitive coefficients, "
                                      "primitive dropped");
        return;
    }
    for (int k = 0; k < 4; k++) {
        flat[k] = provoking[SGI_GL_V_COLOR + k];
    }
    len2 = (double)(b.x - a.x) * (b.x - a.x) +
           (double)(b.y - a.y) * (b.y - a.y);
    x_major = llabs(b.x - a.x) >= llabs(b.y - a.y);
    shifted_a = a;
    shifted_b = b;
    /* Width <= INT32_MAX and sub <= 12 give a shift below 2^42. */
    if (x_major) {
        shifted_a.y -= (int64_t)(r->line_width - 1) * (1 << (sub - 1));
        shifted_b.y -= (int64_t)(r->line_width - 1) * (1 << (sub - 1));
    } else {
        shifted_a.x -= (int64_t)(r->line_width - 1) * (1 << (sub - 1));
        shifted_b.x -= (int64_t)(r->line_width - 1) * (1 << (sub - 1));
    }

    /*
     * Walk the major axis a pixel at a time, and in each step try the
     * minor-axis pixels within one of those the segment spans there.
     */
    major0 = (x_major ? MIN(a.x, b.x) : MIN(a.y, b.y)) >>
             sub;
    major1 = (x_major ? MAX(a.x, b.x) : MAX(a.y, b.y)) >>
             sub;
    major0--;
    major1++;
    step = 1;
    if (stipple) {
        /* Count coverage before ownership/scissor, in submitted direction. */
        if (x_major ? b.x < a.x : b.y < a.y) {
            int tmp = major0;

            major0 = major1;
            major1 = tmp;
            step = -1;
        }
    } else {
        major0 = MAX(major0, x_major ? bx0 : by0);
        major1 = MIN(major1, x_major ? bx1 : by1);
    }
    for (int m = major0; step > 0 ? m <= major1 : m >= major1; m += step) {
        const SGISoftVertex *lo = x_major ?
            (a.x <= b.x ? &shifted_a : &shifted_b) :
            (a.y <= b.y ? &shifted_a : &shifted_b);
        const SGISoftVertex *hi = lo == &shifted_a ? &shifted_b : &shifted_a;
        double l0 = x_major ? lo->x : lo->y, l1 = x_major ? hi->x : hi->y;
        double n0 = x_major ? lo->y : lo->x, n1 = x_major ? hi->y : hi->x;
        double e0 = MAX((double)m * (1 << sub), l0);
        double e1 = MIN((double)(m + 1) * (1 << sub), l1);
        double f0, f1;
        int64_t minor0, minor1;

        if (l1 > l0) {
            e0 = MIN(e0, l1);
            e1 = MAX(e1, l0);
            f0 = n0 + (n1 - n0) * (e0 - l0) / (l1 - l0);
            f1 = n0 + (n1 - n0) * (e1 - l0) / (l1 - l0);
        } else {
            f0 = n0;
            f1 = n1;
        }
        minor0 = (int64_t)floor(MIN(f0, f1) / (1 << sub)) - 1;
        minor1 = (int64_t)floor(MAX(f0, f1) / (1 << sub)) + 1;
        for (int64_t n = minor0; n <= minor1; n++) {
            int64_t base_x = x_major ? m : n, base_y = x_major ? n : m;
            int64_t replica0 = MAX(n, x_major ? by0 : bx0);
            int64_t replica1 = MIN(n + r->line_width - 1,
                                   x_major ? by1 : bx1);

            bool keep = true;

            if (!sgi_soft_line_covers(&shifted_a, &shifted_b,
                                      base_x, base_y, sub)) {
                continue;
            }
            if (stipple) {
                unsigned bit = (stipple->counter / stipple->repeat) % 16;

                keep = !stipple->enabled || (stipple->pattern & (1u << bit));
                stipple->counter = (stipple->counter + 1) %
                                   (16 * stipple->repeat);
                stipple->consumed = true;
            }
            if (!keep || !visible || replica0 > replica1 ||
                m < (x_major ? bx0 : by0) || m > (x_major ? bx1 : by1)) {
                continue;
            }
            /* Visit only drawable-visible replicas, never width entries. */
            for (int64_t replica = replica0; replica <= replica1; replica++) {
                int px = x_major ? m : replica, py = x_major ? replica : m;
                double cx = (px + 0.5) * (1 << sub);
                double cy = (py + 0.5) * (1 << sub);
                double t, wa, wb;
                SGISoftFrag frag;

                t = ((cx - a.x) * (b.x - a.x) +
                     (cy - a.y) * (b.y - a.y)) / len2;
                /* Approximation: retain endpoint values, not extrapolation. */
                t = MIN(MAX(t, 0.0), 1.0);
                wa = (1 - t) * a.inv_w;
                wb = t * b.inv_w;
                frag.index = smooth ? a.index + wb * (b.index - a.index) /
                                      (wa + wb) :
                             provoking[SGI_GL_V_INDEX];
                for (int k = 0; k < 4; k++) {
                    frag.texcoord[k] = (wa * a.texcoord[k] +
                                        wb * b.texcoord[k]) / (wa + wb);
                }
                frag.fog = (wa * a.fog + wb * b.fog) / (wa + wb);
                if (st->clip_planes && client_clip) {
                    double d[SGI_GL_CLIP_PLANES];

                    for (int i = 0; i < SGI_GL_CLIP_PLANES; i++) {
                        d[i] = (wa * a.clip[i] + wb * b.clip[i]) / (wa + wb);
                    }
                    if (sgi_soft_clipped(st, d)) {
                        continue;
                    }
                }
                for (int k = 0; k < 4; k++) {
                    frag.rgba[k] = smooth ?
                    (wa * a.rgba[k] + wb * b.rgba[k]) / (wa + wb) :
                                   flat[k];
                }
                if (r->texture_base) {
                    double value[3], out[5], zero[3] = { 0 };

                    for (unsigned k = 0; k < 3; k++) {
                        value[k] = texture_value[k] + t * texture_delta[k];
                    }
                    frag.texture_rho = sgi_soft_texture_derivatives(value,
                        texture_delta, zero, r->texture_base->width *
                        sgi_soft_texture_lod_scale(r->texture->lod_scale_s),
                        r->texture_base->height *
                        sgi_soft_texture_lod_scale(r->texture->lod_scale_t),
                        out) ?
                        out[4] / texture_length : NAN;
                }
                sgi_soft_fragment(r, px, py, a.z + t * (b.z - a.z),
                                  offset, &frag);
            }
        }
    }
}

/* Keep standalone client-plane rejection after geometric counting. */
static void sgi_soft_line(const SGISoftDraw *r, const float *va,
                          const float *vb, const float *provoking,
                          SGISoftLineStipple *stipple)
{
    SGIVolumeVertex ca, cb;
    SGISoftVertex a, b;
    int bx0, by0, bx1, by1;

    if (!stipple && !sgi_soft_bounds(r, &bx0, &by0, &bx1, &by1)) {
        return;
    }
    sgi_soft_to_volume(r, va, &ca);
    sgi_soft_to_volume(r, vb, &cb);
    if (sgi_volume_clip_segment(&ca, &cb) &&
        sgi_soft_project(r, &ca, &a) && sgi_soft_project(r, &cb, &b)) {
        sgi_soft_line_projected(r, &a, &b, provoking, 0, stipple, true);
    }
}

/*
 * Aliased square-point centring follows OpenGL 1.2.1, 3.3, on the shared
 * snapped subpixel grid rather than ideal unsnapped coordinates. A point
 * outside the view volume is dropped (2.11), and so is one outside the
 * viewport, when there is one, which only a point on the volume's edge
 * can be. Point antialiasing is not modelled.
 */
static void sgi_soft_point_projected(const SGISoftDraw *r,
                                     const SGISoftVertex *pv,
                                     const float *provoking, double offset)
{
    const int sub = r->rules->subpixel_bits;
    SGISoftVertex v = *pv;
    SGISoftFrag frag;
    int bx0, by0, bx1, by1;
    int64_t px, py, x0, y0, x1, y1;

    if (!sgi_soft_bounds(r, &bx0, &by0, &bx1, &by1)) {
        return;
    }
    /* Even dimensions centre on a pixel corner; odd ones on a centre. */
    px = (v.x + (r->point_size % 2 ? 0 : 1 << (sub - 1))) >> sub;
    py = (v.y + (r->point_size % 2 ? 0 : 1 << (sub - 1))) >> sub;
    x0 = MAX(px - r->point_size / 2, bx0);
    y0 = MAX(py - r->point_size / 2, by0);
    x1 = MIN(px - r->point_size / 2 + r->point_size - 1, bx1);
    y1 = MIN(py - r->point_size / 2 + r->point_size - 1, by1);
    memcpy(frag.rgba, v.rgba, sizeof(frag.rgba));
    frag.index = v.index;
    if (provoking && r->st->shade_model == SGI_GL_FLAT) {
        frag.index = provoking[SGI_GL_V_INDEX];
        for (unsigned k = 0; k < 4; k++) {
            frag.rgba[k] = provoking[SGI_GL_V_COLOR + k];
        }
    }
    frag.fog = v.fog;
    frag.texture_rho = 1;
    memcpy(frag.texcoord, v.texcoord, sizeof(frag.texcoord));
    for (int64_t y = y0; y <= y1; y++) {
        for (int64_t x = x0; x <= x1; x++) {
            sgi_soft_fragment(r, x, y, v.z, offset, &frag);
        }
    }
}

static void sgi_soft_point(const SGISoftDraw *r, const float *vp)
{
    SGIVolumeVertex cv;
    SGISoftVertex v;

    sgi_soft_to_volume(r, vp, &cv);
    if (sgi_volume_contains(&cv) && sgi_soft_project(r, &cv, &v) &&
        !sgi_soft_clipped(r->st, v.clip)) {
        sgi_soft_point_projected(r, &v, NULL, 0);
    }
}

/* Internal representation bound, not an SGI width range. Half ties round up. */
static bool sgi_soft_dimension(float requested, uint32_t *rounded)
{
    double n = requested == 0 ? 1 : floor((double)requested + 0.5);

    if (!isfinite(requested) || requested < 0 || n > INT32_MAX) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-gl: unsupported aliased dimension %g\n", requested);
        return false;
    }
    *rounded = MAX(n, 1);
    return true;
}


static bool sgi_soft_boundary_edge(const SGIVolumeVertex *v)
{
    uint32_t edge;

    memcpy(&edge, &v->attr[SGI_GL_V_EDGE], sizeof(edge));
    return edge != 0;
}

static void sgi_soft_boundary_set_edge(SGIVolumeVertex *v, bool enabled)
{
    uint32_t edge = enabled;

    memcpy(&v->attr[SGI_GL_V_EDGE], &edge, sizeof(edge));
}

static double sgi_soft_boundary_plane(const SGISoftDraw *r,
                                      const SGIVolumeVertex *v, unsigned p)
{
    if (p < SGI_VOLUME_PLANES) {
        return sgi_volume_plane(v, p);
    } else {
        const float *plane = r->st->clip_plane[p - SGI_VOLUME_PLANES];

        return plane[0] * v->eye[0] + plane[1] * v->eye[1] +
               plane[2] * v->eye[2] + plane[3] * v->eye[3];
    }
}

/*
 * Carry outgoing flags through complete polygon clipping (2.11). Original
 * retained edges keep their flags; the newly connecting cut edges are true.
 * Exact-plane endpoints avoid duplicate intersections without merging distinct
 * submitted records, whose coincident positions still matter in POINT mode.
 */
static bool sgi_soft_boundary_clip(const SGISoftDraw *r,
                                   SGIVolumeVertex **poly,
                                   SGIVolumeVertex *other, size_t *count,
                                   size_t capacity)
{
    SGIVolumeVertex *src = *poly, *dst = other, *tmp;
    size_t n = *count;

    for (unsigned p = 0; p < SGI_VOLUME_PLANES + SGI_GL_CLIP_PLANES; p++) {
        size_t m = 0;

        if (p >= SGI_VOLUME_PLANES &&
            !(r->st->clip_planes & (1u << (p - SGI_VOLUME_PLANES)))) {
            continue;
        }
        for (size_t i = 0; i < n; i++) {
            const SGIVolumeVertex *a = &src[i], *b = &src[(i + 1) % n];
            double da = sgi_soft_boundary_plane(r, a, p);
            double db = sgi_soft_boundary_plane(r, b, p);

            if (da >= 0) {
                if (m == capacity) {
                    return false;
                }
                dst[m++] = *a;
                if (!(db >= 0)) {
                    if (da == 0) {
                        sgi_soft_boundary_set_edge(&dst[m - 1], true);
                    } else {
                        if (m == capacity) {
                            return false;
                        }
                        sgi_volume_cut(a, da, b, db, &dst[m]);
                        sgi_soft_boundary_set_edge(&dst[m++], true);
                    }
                }
            } else if (db > 0) {
                if (m == capacity) {
                    return false;
                }
                sgi_volume_cut(b, db, a, da, &dst[m]);
                sgi_soft_boundary_set_edge(&dst[m++],
                                            sgi_soft_boundary_edge(a));
            }
        }
        n = m;
        tmp = src, src = dst, dst = tmp;
        if (!n) {
            break;
        }
    }
    *poly = src;
    *count = n;
    return true;
}

static const float *sgi_soft_polygon_vertex(const SGIGLDraw *d,
                                            const uint32_t *indices, size_t i)
{
    size_t index = indices ? indices[i] : i;

    return &d->vertices[index * SGI_GL_VERTEX_FLOATS];
}

/* Replay the original fan, including its validation and fragment ordering. */
static void sgi_soft_polygon_fill(const SGISoftDraw *r, const SGIGLDraw *d,
                                  const uint32_t *indices, size_t n,
                                  const float *provoking)
{
    const float *v0 = sgi_soft_polygon_vertex(d, indices, 0);
    bool prevalidate = r->texture &&
                       !(d->mode == SGI_GL_POLYGON && d->fill_by_triangle);

    if (prevalidate) {
        for (size_t i = 1; i + 1 < n; i++) {
            if (!sgi_soft_texture_triangle_valid(r, v0,
                    sgi_soft_polygon_vertex(d, indices, i),
                    sgi_soft_polygon_vertex(d, indices, i + 1))) {
                return;
            }
        }
    }
    for (size_t i = 1; i + 1 < n; i++) {
        sgi_soft_triangle(r, v0, sgi_soft_polygon_vertex(d, indices, i),
                          sgi_soft_polygon_vertex(d, indices, i + 1),
                          provoking);
    }
}

/* Workspace bounds are internal representations, not hardware maxima. */
static SGIGLDrawResult sgi_soft_polygon(const SGISoftDraw *r,
                                       const SGIGLDraw *d,
                                       const uint32_t *indices, size_t count,
                                       const float *provoking, bool force_edges)
{
    SGIVolumeVertex local[2][4 + SGI_VOLUME_PLANES + SGI_GL_CLIP_PLANES];
    SGIVolumeVertex *allocated = NULL, *poly, *other;
    SGISoftDraw positions = *r, coverage = *r;
    SGISoftVertex fan[3], first, previous, current;
    SGISoftLineStipple stipple = {
        .enabled = d->line_stipple_enabled,
        .repeat = MAX(1, MIN(d->line_stipple_repeat, 256)),
        .pattern = d->line_stipple_pattern,
    };
    float flat[SGI_GL_VERTEX_FLOATS];
    size_t capacity, n = count;
    double area = 0, slope = 0, offset = 0;
    bool front;
    uint32_t mode, offset_enable;

    if (count < 3) {
        return SGI_GL_DRAW_OK;
    }
    if (count > SIZE_MAX - SGI_VOLUME_PLANES - SGI_GL_CLIP_PLANES) {
        return SGI_GL_DRAW_OUT_OF_MEMORY;
    }
    capacity = count + SGI_VOLUME_PLANES + SGI_GL_CLIP_PLANES;
    if (capacity > SIZE_MAX / (2 * sizeof(*poly))) {
        return SGI_GL_DRAW_OUT_OF_MEMORY;
    }
    if (count <= 4) {
        poly = local[0];
        other = local[1];
    } else {
        allocated = g_try_realloc_n(NULL, capacity, 2 * sizeof(*poly));
        if (!allocated) {
            return SGI_GL_DRAW_OUT_OF_MEMORY;
        }
        poly = allocated;
        other = allocated + capacity;
    }
    /* Facing must not prevalidate texture Q and erase a valid FILL prefix. */
    positions.texture = NULL;
    for (size_t i = 0; i < count; i++) {
        sgi_soft_to_volume(r, sgi_soft_polygon_vertex(d, indices, i), &poly[i]);
        for (unsigned k = 0; k < 4; k++) {
            if (!isfinite(poly[i].clip[k]) || !isfinite(poly[i].eye[k])) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-gl: invalid polygon position dropped\n");
                goto out;
            }
        }
        if (force_edges) {
            sgi_soft_boundary_set_edge(&poly[i], true);
        }
    }
    if (!sgi_soft_boundary_clip(r, &poly, other, &n, capacity)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-gl: polygon exceeds convex clipping bound\n");
        goto out;
    }
    if (!n || !sgi_soft_project(&positions, &poly[0], &first)) {
        goto out;
    }
    previous = first;
    fan[0] = first;
    for (size_t i = 1; i < n; i++) {
        if (!sgi_soft_project(&positions, &poly[i], &current)) {
            goto out;
        }
        if (i >= 2) {
            area += sgi_soft_edge(&first, &previous, current.x, current.y);
            fan[1] = previous;
            fan[2] = current;
            slope = MAX(slope, sgi_soft_slope(r, fan));
        }
        previous = current;
    }
    if (r->st->window_matrix_given && r->st->window_rows_down) {
        area = -area;
    }
    if (r->st->front_face == SGI_GL_CW) {
        area = -area;
    }
    front = area > 0; /* Zero remains back-facing through both reversals. */
    if (r->st->cull_face &&
        (r->st->cull_face_mode == SGI_GL_FRONT_AND_BACK ||
         r->st->cull_face_mode == (front ? SGI_GL_FRONT : SGI_GL_BACK))) {
        goto out;
    }
    mode = d->polygon_modes[front ? 0 : 1];
    if (mode == SGI_GL_FILL) {
        sgi_soft_polygon_fill(r, d, indices, count, provoking);
        goto out;
    }
    if (mode != SGI_GL_LINE && mode != SGI_GL_POINT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-gl: invalid polygon mode 0x%x ignored\n", mode);
        goto out;
    }
    if ((mode == SGI_GL_LINE &&
         !sgi_soft_dimension(d->line_width, &coverage.line_width)) ||
        (mode == SGI_GL_POINT &&
         !sgi_soft_dimension(d->point_size, &coverage.point_size))) {
        goto out;
    }
    memcpy(flat, provoking, sizeof(flat));
    if (!front && r->back_colors) {
        memcpy(&flat[SGI_GL_V_COLOR], &provoking[SGI_GL_V_BACK_COLOR],
               4 * sizeof(float));
        for (size_t i = 0; i < n; i++) {
            memcpy(&poly[i].attr[SGI_GL_V_COLOR],
                   &poly[i].attr[SGI_GL_V_BACK_COLOR], 4 * sizeof(float));
        }
    }
    /* Validate the entire boundary before any LINE/POINT fragment or sample. */
    for (size_t i = 0; i < n; i++) {
        if (!sgi_soft_project(r, &poly[i], &current)) {
            goto out;
        }
        if (r->texture_base && mode == SGI_GL_LINE) {
            SGISoftVertex next;
            double values[3], delta[3], length;

            if (!sgi_soft_project(r, &poly[(i + 1) % n], &next) ||
                !sgi_soft_texture_line_gradient(&current, &next,
                    r->rules->subpixel_bits, values, delta, &length)) {
                sgi_soft_texture_report(r, 16,
                    "invalid primitive coefficients, primitive dropped");
                goto out;
            }
        }
    }
    offset_enable = mode == SGI_GL_LINE ? SGI_GL_OFFSET_LINE :
                                          SGI_GL_OFFSET_POINT;
    if (r->st->polygon_offset & offset_enable) {
        offset = slope * r->st->offset_factor +
                 r->st->offset_units / sgi_soft_depth_max(r->rules) +
                 r->st->offset_bias;
    }
    for (size_t i = 0; i < n; i++) {
        SGISoftVertex next;

        if (!sgi_soft_boundary_edge(&poly[i])) {
            continue;
        }
        sgi_soft_project(r, &poly[i], &current);
        if (mode == SGI_GL_POINT) {
            sgi_soft_point_projected(&coverage, &current, flat, offset);
        } else {
            sgi_soft_project(r, &poly[(i + 1) % n], &next);
            sgi_soft_line_projected(&coverage, &current, &next, flat,
                                     offset, &stipple, false);
        }
    }
out:
    g_free(allocated);
    return SGI_GL_DRAW_OK;
}

/* Dispatch each independently assembled polygon, preserving provoking rules. */
static SGIGLDrawResult sgi_soft_polygon_modes(const SGISoftDraw *r,
                                             const SGIGLDraw *d)
{
    uint32_t indices[4];
    uint32_t n = d->count, stride;
    SGIGLDrawResult result;

    if (d->mode == SGI_GL_POLYGON) {
        return sgi_soft_polygon(r, d, NULL, n, d->vertices, false);
    }
    stride = d->mode == SGI_GL_TRIANGLES ? 3 :
             d->mode == SGI_GL_QUADS ? 4 :
             d->mode == SGI_GL_QUAD_STRIP ? 2 : 1;
    for (uint32_t i = 0; i < n; i += stride) {
        unsigned count = d->mode == SGI_GL_QUADS ||
                         d->mode == SGI_GL_QUAD_STRIP ? 4 : 3;
        uint32_t provoking = i + count - 1;
        bool force = stride < 3;

        if (n - i < count) {
            break;
        }
        for (unsigned k = 0; k < count; k++) {
            indices[k] = i + k;
        }
        if (d->mode == SGI_GL_TRIANGLE_STRIP && (i & 1)) {
            indices[0] = i + 1;
            indices[1] = i;
        } else if (d->mode == SGI_GL_TRIANGLE_FAN) {
            indices[0] = 0;
        } else if (d->mode == SGI_GL_QUAD_STRIP) {
            indices[2] = i + 3;
            indices[3] = i + 2;
        }
        result = sgi_soft_polygon(r, d, indices, count,
                   &d->vertices[(size_t)provoking * SGI_GL_VERTEX_FLOATS],
                   force);
        if (result != SGI_GL_DRAW_OK) {
            return result;
        }
    }
    return SGI_GL_DRAW_OK;
}

/*
 * The pieces of a primitive (OpenGL 1.2.1, 2.6.1), each with its flat
 * colour from the vertex table 2.9 names: a point's own, the second
 * vertex of a segment, the last vertex of a triangle, strip or fan
 * triangle, quad or quad-strip quad, and the first vertex of a polygon.
 * Every triangle of a strip keeps the strip's orientation (2.6.1), so an
 * odd one takes its first two vertices swapped, which changes only its
 * facing.
 *
 * A quadrilateral a, b, c, d in order around its edge is drawn as the
 * triangles a b c and a c d. OpenGL 1.2.1 (3.5.1) allows any split of a
 * polygon with more than three vertices that gives a convex combination
 * of the vertices' values, and says nothing of non-convex or non-planar
 * ones; which diagonal a head takes is not known. Approximated: every
 * polygon is split into a fan from its first vertex. The discriminating
 * test is gl-quad-splits-from-its-first-vertex.
 */

SGIGLDrawResult sgi_soft_draw(const SGIRasterRules *rules,
                             const SGIRasterTarget *t, const SGIGLDraw *d)
{
    SGISoftDraw r = {
        .rules = rules, .t = t, .st = d->state,
        .back_colors = d->back_colors,
        .polygon_stipple = d->polygon_stipple_enabled ?
                           d->polygon_stipple : NULL,
    };
    SGISoftLineStipple line = {
        .counter = d->line_phase && !d->line_phase_reset ? *d->line_phase : 0,
        .enabled = d->line_stipple_enabled,
        .repeat = MAX(1, MIN(d->line_stipple_repeat, 256)),
        .pattern = d->line_stipple_pattern,
    };
    SGISoftLineStipple *stipple = d->line_phase || d->line_stipple_enabled ?
                                &line : NULL;
    uint32_t n = d->count;
    unsigned texture_reports = 0;
    bool rgba_reported = false;

    line.counter %= 16 * line.repeat;
    r.texture_reports = &texture_reports;
    r.rgba_reported = &rgba_reported;

#define V(i) (&d->vertices[(size_t)(i) * SGI_GL_VERTEX_FLOATS])
    r.ntargets = sgi_gl_draw_targets(d->state, r.targets);
    if (!sgi_soft_check_rules(rules) ||
        !sgi_soft_check_index(t, d->state->index_bits)) {
        return SGI_GL_DRAW_OK;
    }
    if (!sgi_soft_window(d->state, &r.win)) {
        if (!stipple || d->mode < SGI_GL_LINES ||
            d->mode > SGI_GL_LINE_STRIP || !d->state->drawable.bound ||
            d->state->drawable.clip.n) {
            return SGI_GL_DRAW_OK;
        }
        /* Empty ownership still counts standalone geometric coverage. */
        r.win.x1 = r.win.y1 = -1;
    }
    if (!sgi_soft_texture_setup(&r)) {
        return SGI_GL_DRAW_OK;
    }
    if (!sgi_soft_setup_ops(&r)) {
        return SGI_GL_DRAW_OK;
    }
    if (d->polygon_modes && d->mode >= SGI_GL_TRIANGLES &&
        d->mode <= SGI_GL_POLYGON &&
        (d->polygon_modes[0] != SGI_GL_FILL ||
         d->polygon_modes[1] != SGI_GL_FILL)) {
        return sgi_soft_polygon_modes(&r, d);
    }
    switch (d->mode) {
    case SGI_GL_POINTS:
        if (!sgi_soft_dimension(d->point_size, &r.point_size)) {
            return SGI_GL_DRAW_OK;
        }
        for (uint32_t i = 0; i < n; i++) {
            sgi_soft_point(&r, V(i));
        }
        return SGI_GL_DRAW_OK;
    case SGI_GL_LINES:
        if (!sgi_soft_dimension(d->line_width, &r.line_width)) {
            return SGI_GL_DRAW_OK;
        }
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            if (!d->line_phase) {
                line.counter = 0;
            }
            sgi_soft_line(&r, V(i), V(i + 1), V(i + 1), stipple);
        }
        if (d->line_phase && line.consumed) {
            *d->line_phase = line.counter;
        }
        return SGI_GL_DRAW_OK;
    case SGI_GL_LINE_STRIP:
    case SGI_GL_LINE_LOOP:
        if (!sgi_soft_dimension(d->line_width, &r.line_width)) {
            return SGI_GL_DRAW_OK;
        }
        for (uint32_t i = 0; i + 1 < n; i++) {
            sgi_soft_line(&r, V(i), V(i + 1), V(i + 1), stipple);
        }
        if (d->mode == SGI_GL_LINE_LOOP && n >= 2) {
            sgi_soft_line(&r, V(n - 1), V(0), V(0), stipple);
        }
        if (d->line_phase && line.consumed) {
            *d->line_phase = line.counter;
        }
        return SGI_GL_DRAW_OK;
    case SGI_GL_TRIANGLES:
        for (uint32_t i = 0; i + 2 < n; i += 3) {
            sgi_soft_triangle(&r, V(i), V(i + 1), V(i + 2), V(i + 2));
        }
        return SGI_GL_DRAW_OK;
    case SGI_GL_TRIANGLE_STRIP:
        for (uint32_t i = 0; i + 2 < n; i++) {
            if (i & 1) {
                sgi_soft_triangle(&r, V(i + 1), V(i), V(i + 2), V(i + 2));
            } else {
                sgi_soft_triangle(&r, V(i), V(i + 1), V(i + 2), V(i + 2));
            }
        }
        return SGI_GL_DRAW_OK;
    case SGI_GL_TRIANGLE_FAN:
    case SGI_GL_POLYGON:
        if (r.texture && d->mode == SGI_GL_POLYGON && !d->fill_by_triangle) {
            for (uint32_t i = 0; i + 2 < n; i++) {
                if (!sgi_soft_texture_triangle_valid(&r, V(0), V(i + 1),
                                                      V(i + 2))) {
                    return SGI_GL_DRAW_OK;
                }
            }
        }
        for (uint32_t i = 0; i + 2 < n; i++) {
            sgi_soft_triangle(&r, V(0), V(i + 1), V(i + 2),
                              d->mode == SGI_GL_POLYGON ? V(0) : V(i + 2));
        }
        return SGI_GL_DRAW_OK;
    case SGI_GL_QUADS:
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            if (r.texture &&
                (!sgi_soft_texture_triangle_valid(&r, V(i), V(i + 1),
                                                   V(i + 2)) ||
                 !sgi_soft_texture_triangle_valid(&r, V(i), V(i + 2),
                                                   V(i + 3)))) {
                continue;
            }
            sgi_soft_triangle(&r, V(i), V(i + 1), V(i + 2), V(i + 3));
            sgi_soft_triangle(&r, V(i), V(i + 2), V(i + 3), V(i + 3));
        }
        return SGI_GL_DRAW_OK;
    case SGI_GL_QUAD_STRIP:
        /* Vertices 2i - 1, 2i, 2i + 2 and 2i + 1 in order (2.6.1). */
        for (uint32_t i = 0; i + 3 < n; i += 2) {
            if (r.texture &&
                (!sgi_soft_texture_triangle_valid(&r, V(i), V(i + 1),
                                                   V(i + 3)) ||
                 !sgi_soft_texture_triangle_valid(&r, V(i), V(i + 3),
                                                   V(i + 2)))) {
                continue;
            }
            sgi_soft_triangle(&r, V(i), V(i + 1), V(i + 3), V(i + 3));
            sgi_soft_triangle(&r, V(i), V(i + 3), V(i + 2), V(i + 3));
        }
        return SGI_GL_DRAW_OK;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: draw of mode %u ignored\n",
                      d->mode);
        return SGI_GL_DRAW_OK;
    }
#undef V
}

/*
 * A run of a bitmap's image (OpenGL 1.2.1, 3.7): a fragment for each set
 * bit, at the image's corner plus the bit's column and row, bounded by
 * the window the draw state's drawable owns and not by the viewport.
 */
static void sgi_soft_bitmap(const SGIRasterRules *rules,
                            const SGIRasterTarget *t, const SGIRasterOp *op)
{
    const SGIRasterBitmap *b = &op->bitmap;
    SGISoftDraw r = { .rules = rules, .t = t, .st = b->state };
    uint32_t row_bytes = DIV_ROUND_UP(b->width, 8);
    bool rgba_reported = false;
    SGISoftFrag frag = {
        .rgba = { b->rgba[0], b->rgba[1], b->rgba[2], b->rgba[3] },
        .index = b->index,
        .fog = b->distance,
    };

    r.rgba_reported = &rgba_reported;
    if (!sgi_soft_window(b->state, &r.win) ||
        !sgi_soft_check_index(t, b->state->index_bits)) {
        return;
    }
    r.ntargets = MIN(op->ntargets, SGI_GL_MAX_TARGETS);
    memcpy(r.targets, op->targets, sizeof(r.targets));
    if (!sgi_soft_setup_ops(&r)) {
        return;
    }
    for (uint32_t i = 0; i < b->count && row_bytes; i++) {
        uint32_t byte = b->first + i;
        int64_t row = byte / row_bytes, col0 = (byte % row_bytes) * 8;

        if (row >= b->height) {
            break;
        }
        for (int bit = 0; bit < 8 && col0 + bit < b->width; bit++) {
            int64_t px = b->x + col0 + bit, py = b->y + row;

            if (!(b->bytes[i] & (0x80 >> bit)) ||
                px < r.win.x0 - r.win.ox || px > r.win.x1 - r.win.ox ||
                py < r.win.y0 - r.win.oy || py > r.win.y1 - r.win.oy) {
                continue;
            }
            sgi_soft_fragment_at(&r, px, py, b->z, &frag);
        }
    }
}

/*
 * CLEAR writes the clear colour under the colour mask, or in colour-index
 * mode the clear index under the index mask, to the op's targets, the
 * clear depth to the depth plane and the clear stencil value under its
 * mask to the stencil buffer, where the planes have one, at every pixel of
 * its rectangle its clip list keeps. BITMAP draws its run of a bitmap.
 * The other kinds are not executed here yet.
 */
void sgi_soft_raster(const SGIRasterRules *rules, const SGIRasterTarget *t,
                     const SGIRasterOp *op)
{
    bool colour = op->clear_buffers & SGI_GL_COLOR_BUFFER_BIT;
    bool depth = op->clear_buffers & SGI_GL_DEPTH_BUFFER_BIT;
    bool stencil = op->clear_buffers & SGI_GL_STENCIL_BUFFER_BIT;
    uint32_t rgb = 0, smax, mask = sgi_soft_colour_bits(rules);
    uint32_t rgba_mask = sgi_soft_rgba_mask(rules, op->clear_colour_mask);
    bool rgba = !op->index_bits && t->store_rgba;
    bool rgba_read = colour && op->ntargets && rgba_mask &&
                     rgba_mask != sgi_soft_rgba_mask(rules,
                                                    SGI_GL_COLOUR_MASK_ALL);
    uint32_t ntargets = MIN(op->ntargets, SGI_GL_MAX_TARGETS);
    bool rgba_reported = false;
    SGIRasterColour clear = { 0 };

    if (!sgi_soft_check_rules(rules)) {
        return;
    }
    if (op->kind == SGI_RASTER_BITMAP) {
        sgi_soft_bitmap(rules, t, op);
        return;
    }
    if (op->kind != SGI_RASTER_CLEAR) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: raster op of kind %u is not "
                      "executed\n", op->kind);
        return;
    }
    if (!op->clip.n && op->clip.empty) {
        return;
    }
    if (stencil && !sgi_soft_stencil_max(rules, t, &smax)) {
        stencil = false;
    }
    if (colour && op->index_bits && !sgi_soft_check_index(t, op->index_bits)) {
        colour = false;
    }
    if (!op->index_bits && !sgi_soft_rgba_capable(rules, t, rgba_read)) {
        return;
    }
    if (colour && !op->index_bits) {
        double c[3] = { op->clear_rgba[0], op->clear_rgba[1],
                        op->clear_rgba[2] };

        mask = sgi_soft_colour_mask(rules, op->clear_colour_mask);
        if (!rgba && mask != sgi_soft_colour_bits(rules) &&
            mask && !t->load_colour) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: a partial colour mask needs "
                          "planes that can be read back; cleared without "
                          "it\n");
            mask = sgi_soft_colour_bits(rules);
        }
        colour = rgba ? rgba_mask != 0 : mask != 0;
        rgb = sgi_soft_rgb(rules, c);
        /* Clears remain undithered: no dither state is submitted. */
        clear = sgi_soft_rgba_value(rules, rgb, op->clear_rgba[3]);
    }
    for (int y = op->y0; y <= op->y1; y++) {
        for (int x = op->x0; x <= op->x1; x++) {
            SGIRasterColour dest[SGI_GL_MAX_TARGETS] = { 0 };

            if (op->clip.n > 1 && !sgi_clip_contains(&op->clip, x, y)) {
                continue;
            }

            if (rgba && rgba_read &&
                !sgi_soft_rgba_load(rules, t, op->targets,
                                    ntargets, x, y, dest, &rgba_reported)) {
                continue;
            }
            if (depth) {
                t->store_depth(t->opaque, x, y,
                               op->clear_depth24 &
                               sgi_soft_depth_max(rules));
            }
            if (stencil) {
                uint32_t smask = op->stencil_mask & smax;

                t->store_stencil(t->opaque, x, y,
                                 (t->load_stencil(t->opaque, x, y) &
                                  ~smask & smax) |
                                 (op->clear_stencil & smask));
            }
            for (uint32_t i = 0; colour && i < ntargets; i++) {
                if (op->index_bits) {
                    t->store_index(t->opaque, op->targets[i], x, y,
                                   op->clear_index, op->index_mask);
                } else if (rgba) {
                    SGIRasterColour out = clear;

                    for (unsigned k = 0; k < 4; k++) {
                        if (!(rgba_mask & (1u << k))) {
                            out.component[k] = dest[i].component[k];
                        }
                    }
                    t->store_rgba(t->opaque, op->targets[i], x, y, &out);
                } else if (mask == sgi_soft_colour_bits(rules)) {
                    t->store_colour(t->opaque, op->targets[i], x, y, rgb);
                } else {
                    t->store_colour(t->opaque, op->targets[i], x, y,
                                    (rgb & mask) |
                                    (t->load_colour(t->opaque, op->targets[i],
                                                    x, y) & ~mask));
                }
            }
        }
    }
}

#ifdef SGI_GL_TEXTURE_TEST
/* Numeric observability exists only in the unit-test compilation. */
#include "tests/unit/sgi-gl-texture-test.h"

bool sgi_gl_texture_test_derivatives(const double value[3],
    const double dx[3], const double dy[3], uint32_t ns, uint32_t nt,
    double out[5])
{
    return sgi_soft_texture_derivatives(value, dx, dy, ns, nt, out);
}

bool sgi_gl_texture_test_derivatives_scaled(const double value[3],
    const double dx[3], const double dy[3], uint32_t ns, uint32_t nt,
    double scale_s, double scale_t, double out[5])
{
    return sgi_soft_texture_derivatives(value, dx, dy,
        ns * sgi_soft_texture_lod_scale(scale_s),
        nt * sgi_soft_texture_lod_scale(scale_t), out);
}

bool sgi_gl_texture_test_select(uint32_t min, uint32_t mag,
    const SGIGLTexPyramid *p, uint32_t last, double rho, uint32_t out[3],
    double numeric[2])
{
    SGISoftTexSelection selected;

    if (!sgi_soft_texture_select(min, mag, p, last, rho, &selected)) {
        return false;
    }
    out[0] = selected.first;
    out[1] = selected.second;
    out[2] = selected.filter;
    numeric[0] = selected.lambda;
    numeric[1] = selected.alpha;
    return true;
}

bool sgi_gl_texture_test_plane(const int64_t xy[3][2], unsigned sub,
    const double coords[3][4], const double iw[3], double value[3],
    double dx[3], double dy[3])
{
    SGISoftVertex v[3] = { 0 };
    SGISoftTexPlane g;

    for (unsigned i = 0; i < 3; i++) {
        v[i].x = xy[i][0];
        v[i].y = xy[i][1];
        v[i].inv_w = iw[i];
        memcpy(v[i].texcoord, coords[i], sizeof(v[i].texcoord));
    }
    if (!sgi_soft_texture_plane(v, sub, &g)) {
        return false;
    }
    memcpy(value, g.value, sizeof(g.value));
    memcpy(dx, g.dx, sizeof(g.dx));
    memcpy(dy, g.dy, sizeof(g.dy));
    return true;
}

bool sgi_gl_texture_test_sample(const SGIGLTexImage *im,
    const SGIGLTexLevel *level, uint32_t filter, double s, double t,
    double out[4])
{
    unsigned reports = 0;
    SGISoftDraw r = { .texture = im, .texture_reports = &reports };

    return sgi_soft_texture_sample(&r, level, filter, s, t, out);
}

void sgi_gl_texture_test_mix(double value[4], const double other[4],
    double alpha)
{
    sgi_soft_texture_mix(value, other, alpha);
}
#endif
