/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The software rasterizer the SGI graphics heads share.
 *
 * A draw's vertices are transformed by the draw state's matrices, its
 * primitives clipped to the view volume, mapped through its viewport, or
 * a window matrix in place of projection and viewport, into window
 * coordinates, snapped to a subpixel grid and rasterized with edge
 * functions at pixel centres, clipped by the enabled client clip planes,
 * textured from a family-owned level-zero image while supported, and
 * fogged while fog is enabled. Each fragment then passes the
 * per-fragment operations in OpenGL 1.2.1's order (4.1):
 * pixel ownership against the drawable's rectangles, the scissor test, the
 * alpha test, the stencil test, the depth test, blending, dithering and
 * the logical operation, and is stored under the write masks through the
 * family's target. What it draws follows OpenGL 1.2.1 for
 * points, lines, polygons and untextured bitmaps at size and width 1,
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
    SGIColourTarget targets[2];
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
    unsigned *texture_reports;
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
           r->stencil_bits <= 16 &&
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

/*
 * Blend the packed source colour src, with alpha alpha, and the packed
 * destination dst (4.1.6), each component the fixed point of the rules'
 * colour bits taken as a fraction of its largest value. The planes hold
 * no alpha, so the destination's alpha is 1. The result is clamped to
 * [0, 1] and stored at plane pixel (x, y) as a fragment's colour is.
 */
static uint32_t sgi_soft_blend(const SGISoftDraw *r, int x, int y,
                               uint32_t src, uint32_t alpha, uint32_t dst)
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
    s[3] = alpha / max;
    d[3] = 1;
    for (int k = 0; k < 3; k++) {
        double sf = sgi_soft_blend_factor(st->blend_src, k, s, d,
                                          st->blend_color);
        double df = sgi_soft_blend_factor(st->blend_dst, k, s, d,
                                          st->blend_color);

        switch (st->blend_equation) {
        case SGI_GL_MIN:
            out[k] = MIN(s[k], d[k]);
            break;
        case SGI_GL_MAX:
            out[k] = MAX(s[k], d[k]);
            break;
        case SGI_GL_FUNC_SUBTRACT:
            out[k] = s[k] * sf - d[k] * df;
            break;
        case SGI_GL_FUNC_REVERSE_SUBTRACT:
            out[k] = d[k] * df - s[k] * sf;
            break;
        default:
            out[k] = s[k] * sf + d[k] * df;
            break;
        }
    }
    return sgi_soft_store_rgb(r, x, y, out);
}

/*
 * glLogicOp's operation (4.1.8, table 4.3) on the source s and the
 * destination d, bit by bit.
 */
static uint32_t sgi_soft_logic_op(uint32_t op, uint32_t s, uint32_t d)
{
    switch (op - SGI_GL_CLEAR) {
    case 0x0:
        return 0;
    case 0x1:
        return s & d;
    case 0x2:
        return s & ~d;
    case 0x4:
        return ~s & d;
    case 0x5:
        return d;
    case 0x6:
        return s ^ d;
    case 0x7:
        return s | d;
    case 0x8:
        return ~(s | d);
    case 0x9:
        return ~(s ^ d);
    case 0xa:
        return ~d;
    case 0xb:
        return s | ~d;
    case 0xc:
        return ~s;
    case 0xd:
        return ~s | d;
    case 0xe:
        return ~(s & d);
    case 0xf:
        return UINT32_MAX;
    default:
        return s;
    }
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
static void sgi_soft_setup_ops(SGISoftDraw *r)
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

/* Incomplete images disable texturing; unsupported states drop the draw. */
static bool sgi_soft_texture_setup(SGISoftDraw *r)
{
    const SGIGLTexImage *im = r->t->texture;
    const SGIGLDrawState *st = r->st;

    if (!st->texture_2d || st->index_bits) {
        return true;
    }
    if (im) {
        bool border_finite = true;

        for (int k = 0; k < 4; k++) {
            border_finite &= isfinite(im->border_color[k]);
        }
        if (im->border || !border_finite ||
            !sgi_soft_texture_format(im->base_format) ||
            !sgi_soft_texture_wrap(im->wrap_s) ||
            !sgi_soft_texture_wrap(im->wrap_t) ||
            im->min_filter != im->mag_filter ||
            (im->min_filter != SGI_GL_NEAREST &&
             im->min_filter != SGI_GL_LINEAR) ||
            (st->texture_env_mode != SGI_GL_REPLACE &&
             st->texture_env_mode != SGI_GL_MODULATE &&
             st->texture_env_mode != SGI_GL_DECAL &&
             st->texture_env_mode != SGI_GL_BLEND) ||
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

static bool sgi_soft_texture_tap(const SGISoftDraw *r, int64_t x, int64_t y,
                                 double out[4])
{
    const SGIGLTexImage *im = r->texture;
    uint32_t rgba;

    if (im->wrap_s == SGI_GL_REPEAT) {
        x = (x % im->width + im->width) % im->width;
    }
    if (im->wrap_t == SGI_GL_REPEAT) {
        y = (y % im->height + im->height) % im->height;
    }
    if (x < 0 || y < 0 || x >= im->width || y >= im->height) {
        for (int k = 0; k < 4; k++) {
            out[k] = MIN(MAX(im->border_color[k], 0), 1);
        }
    } else {
        if (!im->read_texel(im->opaque, x, y, &rgba)) {
            sgi_soft_texture_report(r, 1, "image-read failure, "
                                    "fragment dropped");
            return false;
        }
        for (int k = 0; k < 4; k++) {
            out[k] = ((rgba >> (24 - k * 8)) & 255) / 255.0;
        }
    }
    sgi_soft_texture_components(im->base_format, out);
    return true;
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

/* OpenGL 1.2.1's level-zero nearest/linear rules and environment tables. */
static bool sgi_soft_texture(const SGISoftDraw *r, SGISoftFrag *frag)
{
    const SGIGLTexImage *im = r->texture;
    const SGIGLDrawState *st = r->st;
    double texel[4] = { 0 }, s, t, q = frag->texcoord[3];

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
    s = sgi_soft_texture_axis(s, im->width, im->wrap_s);
    t = sgi_soft_texture_axis(t, im->height, im->wrap_t);
    if (im->min_filter == SGI_GL_NEAREST) {
        int64_t x = floor(im->width * s), y = floor(im->height * t);

        if (s == 1 && im->wrap_s != SGI_GL_CLAMP_TO_BORDER) {
            x = im->width - 1;
        }
        if (t == 1 && im->wrap_t != SGI_GL_CLAMP_TO_BORDER) {
            y = im->height - 1;
        }
        if (!sgi_soft_texture_tap(r, x, y, texel)) {
            return false;
        }
    } else {
        double u = im->width * s - 0.5, v = im->height * t - 0.5;
        int64_t x = floor(u), y = floor(v);
        double a = u - x, b = v - y;

        for (int j = 0; j < 2; j++) {
            for (int i = 0; i < 2; i++) {
                double weight = (i ? a : 1 - a) * (j ? b : 1 - b);
                double tap[4];

                if (!weight) {
                    continue;
                }
                if (!sgi_soft_texture_tap(r, x + i, y + j, tap)) {
                    return false;
                }
                for (int k = 0; k < 4; k++) {
                    texel[k] += weight * tap[k];
                }
            }
        }
    }
    for (int k = 0; k < 4; k++) {
        bool alpha = k == 3;

        if ((!alpha && im->base_format == SGI_GL_ALPHA) ||
            (alpha && (im->base_format == SGI_GL_RGB ||
                       im->base_format == SGI_GL_LUMINANCE))) {
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
                frag->rgba[k] = im->base_format == SGI_GL_RGB ? texel[k] :
                    frag->rgba[k] * (1 - texel[3]) + texel[k] * texel[3];
            }
            break;
        case SGI_GL_BLEND:
            frag->rgba[k] = alpha && im->base_format != SGI_GL_INTENSITY ?
                frag->rgba[k] * texel[k] :
                frag->rgba[k] * (1 - texel[k]) +
                st->texture_env_color[k] * texel[k];
            break;
        }
    }
    return true;
}

/*
 * Fog (3.10): the fog factor f from the fragment's fog coordinate c by
 * the mode, clamped to [0, 1], blends an RGBA fragment's red, green and
 * blue toward the fog colour, f C + (1 - f) Cf, and adds (1 - f) times
 * the fog index to a colour index. GL_LINEAR with start equal to end
 * divides by zero; the model takes f as 1 before the end and 0 from it,
 * the limit of the equation from either side.
 */
static void sgi_soft_fog(const SGIGLDrawState *st, SGISoftFrag *frag)
{
    double c = frag->fog, f;

    switch (st->fog_mode) {
    case SGI_GL_LINEAR:
        f = st->fog_end == st->fog_start ? (c < st->fog_end ? 1 : 0) :
            (st->fog_end - c) / (st->fog_end - st->fog_start);
        break;
    case SGI_GL_EXP2:
        f = exp(-(st->fog_density * c) * (st->fog_density * c));
        break;
    default:
        f = exp(-st->fog_density * c);
        break;
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

    if (!(z >= 0.0 && z <= 1.0)) {
        return;
    }
    if (r->texture && !sgi_soft_texture(r, &fogged)) {
        return;
    }
    if (r->st->fog) {
        sgi_soft_fog(r->st, &fogged);
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
     * Alpha and reference each as the fixed point a stored alpha of the
     * rules' colour bits would hold (4.1.4, 2.13.9).
     */
    if (r->st->alpha_test && !r->st->index_bits &&
        !sgi_soft_compare(r->st->alpha_func,
                          sgi_soft_channel(r->rules, f->rgba[3]),
                          sgi_soft_channel(r->rules, r->st->alpha_ref))) {
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
                                 sgi_soft_channel(r->rules, f->rgba[3]),
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
 * depth step; EXT_polygon_offset's bias adds in window depth. Only filled
 * polygons are drawn here, so GL_POLYGON_OFFSET_FILL alone applies.
 */
static double sgi_soft_offset(const SGISoftDraw *r, const SGISoftVertex v[3])
{
    const SGIGLDrawState *st = r->st;
    const double sub = 1 << r->rules->subpixel_bits;
    double x1, y1, z1, x2, y2, z2, det, m, n, f;

    if (!(st->polygon_offset & SGI_GL_OFFSET_FILL)) {
        return 0;
    }
    sgi_soft_depth_range(st, &n, &f);
    x1 = (v[1].x - v[0].x) / sub, y1 = (v[1].y - v[0].y) / sub;
    x2 = (v[2].x - v[0].x) / sub, y2 = (v[2].y - v[0].y) / sub;
    z1 = v[1].z - v[0].z, z2 = v[2].z - v[0].z;
    det = x1 * y2 - x2 * y1;
    m = MAX(fabs(z1 * y2 - z2 * y1), fabs(x1 * z2 - x2 * z1)) / fabs(det) *
        fabs(f - n);
    return m * st->offset_factor +
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
        int64_t cy = ((int64_t)py << sub) +
                     (1 << (sub - 1));

        for (int px = px0; px <= px1; px++) {
            int64_t cx = ((int64_t)px << sub) +
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

/*
 * Whether a width-1 segment from a to b produces the fragment at pixel
 * (px, py): the diamond-exit rule of OpenGL 1.2.1, 3.4.1. The diamond is
 * the open square |x - xc| + |y - yc| < 1/2 about the pixel's centre, and
 * the segment produces the fragment when it meets the diamond, unless b
 * lies in it. The distance from the centre along the segment is convex
 * and piecewise linear, so its least value lies at an end or where the
 * segment crosses the centre's row or column; the test is exact in the
 * subpixel grid the vertices were snapped to.
 *
 * This is SGI_LINE_COVERAGE_DOUBLE's test. The specification lets an
 * implementation depart from this rule within stated bounds, and it
 * perturbs endpoints on a diamond's edge in an unstated direction; the
 * rule is applied as written with no perturbation.
 */
static bool sgi_soft_line_covers(const SGISoftVertex *a,
                                 const SGISoftVertex *b, int px, int py,
                                 int sub)
{
    const int64_t half = 1 << (sub - 1);
    int64_t cx = ((int64_t)px << sub) + half;
    int64_t cy = ((int64_t)py << sub) + half;
    int64_t dx = b->x - a->x, dy = b->y - a->y;
    int64_t ex = cx - a->x, ey = cy - a->y;

    if (llabs(b->x - cx) + llabs(b->y - cy) < half) {
        return false;
    }
    if (llabs(ex) + llabs(ey) < half) {
        return true;
    }
    /* Crossing the centre's column at t = ex / dx, scaled by |dx|. */
    if (dx && (dx > 0 ? ex >= 0 && ex <= dx : ex <= 0 && ex >= dx) &&
        llabs(dy * ex - ey * dx) < half * llabs(dx)) {
        return true;
    }
    /* Crossing the centre's row at t = ey / dy, scaled by |dy|. */
    return dy && (dy > 0 ? ey >= 0 && ey <= dy : ey <= 0 && ey >= dy) &&
           llabs(dx * ey - ex * dy) < half * llabs(dy);
}

/*
 * Transform, clip, map and rasterize one width-1 segment (OpenGL 1.2.1,
 * 3.4.1) with the provoking vertex's colour when flat shaded. The segment
 * is clipped to the view volume in clip coordinates (2.11), approximated
 * as a triangle is. A fragment's depth and smooth colour come from t, the
 * centre's projection onto the clipped segment (equations 3.5 to 3.7),
 * the colour corrected for perspective. The fragments are also bounded by
 * the viewport, when there is one. glLineWidth, glLineStipple and line
 * antialiasing are not modelled.
 */
static void sgi_soft_line(const SGISoftDraw *r, const float *va,
                          const float *vb, const float *provoking)
{
    const SGIGLDrawState *st = r->st;
    bool smooth = st->shade_model != SGI_GL_FLAT;
    const int sub = r->rules->subpixel_bits;
    bool x_major;
    SGIVolumeVertex ca, cb;
    SGISoftVertex a, b;
    double flat[4], len2;
    int major0, major1, bx0, by0, bx1, by1;

    if (!sgi_soft_bounds(r, &bx0, &by0, &bx1, &by1)) {
        return;
    }
    sgi_soft_to_volume(r, va, &ca);
    sgi_soft_to_volume(r, vb, &cb);
    if (!sgi_volume_clip_segment(&ca, &cb) ||
        !sgi_soft_project(r, &ca, &a) || !sgi_soft_project(r, &cb, &b) ||
        (a.x == b.x && a.y == b.y)) {
        return;
    }
    for (int k = 0; k < 4; k++) {
        flat[k] = provoking[SGI_GL_V_COLOR + k];
    }
    len2 = (double)(b.x - a.x) * (b.x - a.x) +
           (double)(b.y - a.y) * (b.y - a.y);
    x_major = llabs(b.x - a.x) >= llabs(b.y - a.y);

    /*
     * Walk the major axis a pixel at a time, and in each step try the
     * minor-axis pixels within one of those the segment spans there.
     */
    major0 = (x_major ? MIN(a.x, b.x) : MIN(a.y, b.y)) >>
             sub;
    major1 = (x_major ? MAX(a.x, b.x) : MAX(a.y, b.y)) >>
             sub;
    for (int m = major0 - 1; m <= major1 + 1; m++) {
        const SGISoftVertex *lo = x_major ? (a.x <= b.x ? &a : &b) :
                                            (a.y <= b.y ? &a : &b);
        const SGISoftVertex *hi = lo == &a ? &b : &a;
        double l0 = x_major ? lo->x : lo->y, l1 = x_major ? hi->x : hi->y;
        double n0 = x_major ? lo->y : lo->x, n1 = x_major ? hi->y : hi->x;
        double e0 = MAX((double)m * (1 << sub), l0);
        double e1 = MIN((double)(m + 1) * (1 << sub), l1);
        double f0, f1;
        int minor0, minor1;

        if (l1 > l0) {
            e0 = MIN(e0, l1);
            e1 = MAX(e1, l0);
            f0 = n0 + (n1 - n0) * (e0 - l0) / (l1 - l0);
            f1 = n0 + (n1 - n0) * (e1 - l0) / (l1 - l0);
        } else {
            f0 = n0;
            f1 = n1;
        }
        minor0 = (int)floor(MIN(f0, f1) / (1 << sub)) - 1;
        minor1 = (int)floor(MAX(f0, f1) / (1 << sub)) + 1;
        for (int n = minor0; n <= minor1; n++) {
            int px = x_major ? m : n, py = x_major ? n : m;
            double cx = (px + 0.5) * (1 << sub);
            double cy = (py + 0.5) * (1 << sub);
            double t, wa, wb;
            SGISoftFrag frag;

            if (px < bx0 || px > bx1 || py < by0 || py > by1 ||
                !sgi_soft_line_covers(&a, &b, px, py, sub)) {
                continue;
            }
            t = ((cx - a.x) * (b.x - a.x) + (cy - a.y) * (b.y - a.y)) / len2;
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
            if (st->clip_planes) {
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
            sgi_soft_fragment(r, px, py, a.z + t * (b.z - a.z), 0, &frag);
        }
    }
}

/*
 * A size-1 point covers the pixel holding its window position (OpenGL
 * 1.2.1, 3.3), at the subpixel grid the vertex was snapped to. A point
 * outside the view volume is dropped (2.11), and so is one outside the
 * viewport, when there is one, which only a point on the volume's edge
 * can be. glPointSize and point antialiasing are not modelled.
 */
static void sgi_soft_point(const SGISoftDraw *r, const float *vp)
{
    const SGIGLDrawState *st = r->st;
    const int sub = r->rules->subpixel_bits;
    SGIVolumeVertex cv;
    SGISoftVertex v;
    SGISoftFrag frag;
    int px, py, bx0, by0, bx1, by1;

    sgi_soft_to_volume(r, vp, &cv);
    if (!sgi_volume_contains(&cv) || !sgi_soft_project(r, &cv, &v) ||
        sgi_soft_clipped(st, v.clip)) {
        return;
    }
    px = v.x >> sub;
    py = v.y >> sub;
    if (!sgi_soft_bounds(r, &bx0, &by0, &bx1, &by1) ||
        px < bx0 || px > bx1 || py < by0 || py > by1) {
        return;
    }
    memcpy(frag.rgba, v.rgba, sizeof(frag.rgba));
    frag.index = v.index;
    frag.fog = v.fog;
    memcpy(frag.texcoord, v.texcoord, sizeof(frag.texcoord));
    sgi_soft_fragment(r, px, py, v.z, 0, &frag);
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
void sgi_soft_draw(const SGIRasterRules *rules, const SGIRasterTarget *t,
                   const SGIGLDraw *d)
{
    SGISoftDraw r = {
        .rules = rules, .t = t, .st = d->state,
        .back_colors = d->back_colors,
    };
    uint32_t n = d->count;
    unsigned texture_reports = 0;

    r.texture_reports = &texture_reports;

#define V(i) (&d->vertices[(i) * SGI_GL_VERTEX_FLOATS])
    r.ntargets = sgi_gl_draw_targets(d->state, r.targets);
    if (!sgi_soft_check_rules(rules) || !sgi_soft_window(d->state, &r.win) ||
        !sgi_soft_check_index(t, d->state->index_bits)) {
        return;
    }
    if (!sgi_soft_texture_setup(&r)) {
        return;
    }
    sgi_soft_setup_ops(&r);
    switch (d->mode) {
    case SGI_GL_POINTS:
        for (uint32_t i = 0; i < n; i++) {
            sgi_soft_point(&r, V(i));
        }
        return;
    case SGI_GL_LINES:
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            sgi_soft_line(&r, V(i), V(i + 1), V(i + 1));
        }
        return;
    case SGI_GL_LINE_STRIP:
    case SGI_GL_LINE_LOOP:
        for (uint32_t i = 0; i + 1 < n; i++) {
            sgi_soft_line(&r, V(i), V(i + 1), V(i + 1));
        }
        if (d->mode == SGI_GL_LINE_LOOP && n >= 2) {
            sgi_soft_line(&r, V(n - 1), V(0), V(0));
        }
        return;
    case SGI_GL_TRIANGLES:
        for (uint32_t i = 0; i + 2 < n; i += 3) {
            sgi_soft_triangle(&r, V(i), V(i + 1), V(i + 2), V(i + 2));
        }
        return;
    case SGI_GL_TRIANGLE_STRIP:
        for (uint32_t i = 0; i + 2 < n; i++) {
            if (i & 1) {
                sgi_soft_triangle(&r, V(i + 1), V(i), V(i + 2), V(i + 2));
            } else {
                sgi_soft_triangle(&r, V(i), V(i + 1), V(i + 2), V(i + 2));
            }
        }
        return;
    case SGI_GL_TRIANGLE_FAN:
    case SGI_GL_POLYGON:
        if (r.texture && d->mode == SGI_GL_POLYGON) {
            for (uint32_t i = 0; i + 2 < n; i++) {
                if (!sgi_soft_texture_triangle_valid(&r, V(0), V(i + 1),
                                                      V(i + 2))) {
                    return;
                }
            }
        }
        for (uint32_t i = 0; i + 2 < n; i++) {
            sgi_soft_triangle(&r, V(0), V(i + 1), V(i + 2),
                              d->mode == SGI_GL_POLYGON ? V(0) : V(i + 2));
        }
        return;
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
        return;
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
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-gl: draw of mode %u ignored\n",
                      d->mode);
        return;
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
    SGISoftFrag frag = {
        .rgba = { b->rgba[0], b->rgba[1], b->rgba[2], b->rgba[3] },
        .index = b->index,
        .fog = b->distance,
    };

    if (!sgi_soft_window(b->state, &r.win) ||
        !sgi_soft_check_index(t, b->state->index_bits)) {
        return;
    }
    sgi_soft_setup_ops(&r);
    r.ntargets = MIN(op->ntargets, 2);
    memcpy(r.targets, op->targets, sizeof(r.targets));
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
    if (colour && !op->index_bits) {
        double c[3] = { op->clear_rgba[0], op->clear_rgba[1],
                        op->clear_rgba[2] };

        mask = sgi_soft_colour_mask(rules, op->clear_colour_mask);
        if (mask != sgi_soft_colour_bits(rules) && mask && !t->load_colour) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: a partial colour mask needs "
                          "planes that can be read back; cleared without "
                          "it\n");
            mask = sgi_soft_colour_bits(rules);
        }
        colour = mask != 0;
        rgb = sgi_soft_rgb(rules, c);
    }
    for (int y = op->y0; y <= op->y1; y++) {
        for (int x = op->x0; x <= op->x1; x++) {
            if (op->clip.n > 1 && !sgi_clip_contains(&op->clip, x, y)) {
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
            for (uint32_t i = 0; colour && i < MIN(op->ntargets, 2); i++) {
                if (op->index_bits) {
                    t->store_index(t->opaque, op->targets[i], x, y,
                                   op->clear_index, op->index_mask);
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
