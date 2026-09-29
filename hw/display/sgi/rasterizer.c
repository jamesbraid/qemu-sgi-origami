/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The software rasterizer the SGI graphics heads share.
 *
 * A draw's vertices are transformed by the draw state's matrices, mapped
 * through its viewport into window coordinates, snapped to a subpixel
 * grid and rasterized with edge functions at pixel centres, then depth
 * tested and stored through the family's target. What it draws follows
 * OpenGL 1.2.1 for untextured points, lines, polygons and bitmaps at size
 * and width 1, flat or smooth shaded, in RGBA or colour-index mode, from
 * vertex colours the context has already lit, with a back-facing
 * polygon's back colours under two-sided lighting, and polygons culled by
 * facing. Where OpenGL leaves a choice, the head's SGIRasterRules make
 * it.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
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
} SGISoftDraw;

/* A vertex after transformation, ready to rasterize. */
typedef struct SGISoftVertex {
    int64_t x, y;         /* window position in subpixels */
    double z;             /* normalized depth; [0, 1] inside the view volume */
    double inv_w;         /* 1 / clip w, for perspective-correct colour */
    double rgb[3];
    double index;         /* the colour index, in colour-index mode */
} SGISoftVertex;

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
           r->rounding <= SGI_ROUND_HALF_AWAY &&
           r->tie_rule == SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN &&
           r->arith == SGI_ARITH_DOUBLE &&
           r->line_rule == SGI_LINE_COVERAGE_DOUBLE &&
           r->drop_nonpositive_w;
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

static bool sgi_soft_depth_passes(uint32_t func, uint32_t z, uint32_t stored)
{
    switch (func) {
    case SGI_GL_NEVER:
        return false;
    case SGI_GL_LESS:
        return z < stored;
    case SGI_GL_EQUAL:
        return z == stored;
    case SGI_GL_LEQUAL:
        return z <= stored;
    case SGI_GL_GREATER:
        return z > stored;
    case SGI_GL_NOTEQUAL:
        return z != stored;
    case SGI_GL_GEQUAL:
        return z >= stored;
    default:
        return true;
    }
}

/*
 * One fragment at window pixel (px, py) with window depth z: dropped
 * outside [0, 1] and outside the window's rectangles, depth tested when
 * the test is enabled, then written to each colour target the draw
 * buffers select. colour is the rules' packed colour in RGBA mode and the
 * stored index in colour-index mode, which glIndexMask masks (4.2.2).
 */
static void sgi_soft_fragment_at(const SGISoftDraw *r, int px, int py,
                                 double z, uint32_t colour)
{
    const SGIRasterTarget *t = r->t;
    int x = r->win.ox + px, y = r->win.oy + py;
    uint32_t zq;

    if (!(z >= 0.0 && z <= 1.0)) {
        return;
    }
    if (r->win.clip->n > 1 && !sgi_clip_contains(r->win.clip, x, y)) {
        return;
    }
    zq = (uint32_t)sgi_soft_round(r->rules,
                                  z * sgi_soft_depth_max(r->rules));
    if (r->st->depth_test) {
        if (!sgi_soft_depth_passes(r->st->depth_func, zq,
                                   t->load_depth(t->opaque, x, y))) {
            return;
        }
        if (r->st->depth_mask) {
            t->store_depth(t->opaque, x, y, zq);
        }
    }
    for (uint32_t i = 0; i < r->ntargets; i++) {
        if (r->st->index_bits) {
            t->store_index(t->opaque, r->targets[i], x, y, colour,
                           r->st->index_mask);
        } else {
            t->store_colour(t->opaque, r->targets[i], x, y, colour);
        }
    }
}

/*
 * A primitive's fragment at normalized depth z, (zd + 1) / 2 for the
 * normalized device z zd: dropped outside [0, 1], the view volume's near
 * and far planes, and otherwise mapped to window depth n + (f - n) z
 * through the depth range [n, f] (OpenGL 1.2.1, 2.10.1). Clipping on the
 * normalized depth keeps a range with n equal to f from passing depths
 * beyond the far plane.
 */
static void sgi_soft_fragment(const SGISoftDraw *r, int px, int py, double z,
                              uint32_t colour)
{
    double n = r->st->depth_range[0], f = r->st->depth_range[1];

    if (!(z >= 0.0 && z <= 1.0)) {
        return;
    }
    sgi_soft_fragment_at(r, px, py, n + (f - n) * z, colour);
}

/* A fragment's colour value from an interpolated or flat index. */
static uint32_t sgi_soft_index(const SGISoftDraw *r, double index)
{
    return sgi_gl_index_value(r->st->index_bits, index);
}

/*
 * Transform one vertex (OpenGL 1.2.1, 2.10): object to eye by the
 * modelview, eye to clip by the projection, the divide by w and the
 * viewport map, with depth left normalized to [0, 1] for clipping; the
 * depth range maps it per fragment. A vertex with w <= 0
 * needs clipping the model does not do, and returns false.
 */
static bool sgi_soft_transform(const SGISoftDraw *r, const float *v,
                               SGISoftVertex *out)
{
    const SGIGLDrawState *st = r->st;
    const float *pos = &v[SGI_GL_V_POSITION];
    const float *rgba = &v[SGI_GL_V_COLOR];
    double obj[4], eye[4], clip[4];
    int vx = st->viewport[0], vy = st->viewport[1];
    int vw = st->viewport[2], vh = st->viewport[3];
    double xw, yw;

    for (int k = 0; k < 4; k++) {
        obj[k] = pos[k];
    }
    for (int row = 0; row < 4; row++) {
        eye[row] = 0;
        for (int k = 0; k < 4; k++) {
            eye[row] += st->modelview[k * 4 + row] * obj[k];
        }
    }
    for (int row = 0; row < 4; row++) {
        clip[row] = 0;
        for (int k = 0; k < 4; k++) {
            clip[row] += st->projection[k * 4 + row] * eye[k];
        }
    }
    if (!(clip[3] > 0)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: vertex with w %g needs clipping, "
                      "primitive dropped\n", clip[3]);
        return false;
    }
    /* 2.10.1: xw = (px / 2) xd + ox, ox = x + w / 2. */
    xw = (clip[0] / clip[3] + 1.0) * vw / 2.0 + vx;
    yw = (clip[1] / clip[3] + 1.0) * vh / 2.0 + vy;
    if (!(fabs(xw) < 65536.0 && fabs(yw) < 65536.0)) {
        qemu_log_mask(LOG_UNIMP, "sgi-gl: vertex outside the rasterizer's "
                      "range, primitive dropped\n");
        return false;
    }
    out->x = sgi_soft_round(r->rules, xw * (1 << r->rules->subpixel_bits));
    out->y = sgi_soft_round(r->rules, yw * (1 << r->rules->subpixel_bits));
    out->z = (clip[2] / clip[3] + 1.0) / 2.0;
    out->inv_w = 1.0 / clip[3];
    for (int k = 0; k < 3; k++) {
        out->rgb[k] = MIN(MAX(rgba[k], 0.0f), 1.0f);
    }
    out->index = v[SGI_GL_V_INDEX];
    return true;
}

/*
 * Transform, map and rasterize one triangle, given its three vertices
 * and, for flat shading, the provoking vertex (OpenGL 1.2.1, 2.13.7 and
 * table 2.9).
 *
 * Facing (2.13.1) is the sign of the triangle's area in window
 * coordinates on the subpixel grid: a positive area is front facing under
 * glFrontFace(GL_CCW) and a negative one under GL_CW. With GL_CULL_FACE
 * enabled a triangle whose facing glCullFace names is not rasterized
 * (3.5.1). When the draw carries back colours a back-facing triangle
 * takes them. A polygon or quad is drawn as triangles, each facing by its
 * own area, which for a convex polygon is the polygon's facing.
 *
 * Clipping to the view volume is approximated. In x and y the fragments
 * are bounded by the viewport rectangle, which matches clipping for a
 * positive w; the discriminating test is
 * gl-triangle-bounded-by-the-viewport. In z a fragment whose interpolated
 * window depth lies outside [0, 1] is dropped: window depth is affine in
 * window x and y over the triangle, so that removes exactly the part
 * beyond the near or far plane (gl-triangle-clipped-at-the-far-plane). A
 * vertex with w <= 0 drops the whole triangle
 * (gl-triangle-with-negative-w-is-dropped).
 *
 * Depth is interpolated linearly in window coordinates and colour with
 * the perspective correction of equation 3.4 (OpenGL 1.2.1, 3.5.1).
 */
static void sgi_soft_triangle(const SGISoftDraw *r, const float *va,
                              const float *vb, const float *vc,
                              const float *provoking)
{
    const SGIGLDrawState *st = r->st;
    const SGISoftWindow *win = &r->win;
    const float *verts[3] = { va, vb, vc };
    const float *flat = &provoking[SGI_GL_V_COLOR];
    double flat_c[3];
    uint32_t flat_rgb;
    SGISoftVertex v[3];
    int64_t min_x, max_x, min_y, max_y, area;
    bool front;
    int vx = st->viewport[0], vy = st->viewport[1];
    int vw = st->viewport[2], vh = st->viewport[3];
    int px0, px1, py0, py1;
    bool smooth = st->shade_model != SGI_GL_FLAT;
    const int sub = r->rules->subpixel_bits;

    if (vw <= 0 || vh <= 0) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        if (!sgi_soft_transform(r, verts[i], &v[i])) {
            return;
        }
    }

    area = sgi_soft_edge(&v[0], &v[1], v[2].x, v[2].y);
    if (area == 0) {
        return;
    }
    front = st->front_face == SGI_GL_CW ? area < 0 : area > 0;
    if (st->cull_face &&
        (st->cull_face_mode == SGI_GL_FRONT_AND_BACK ||
         st->cull_face_mode == (front ? SGI_GL_FRONT : SGI_GL_BACK))) {
        return;
    }
    if (!front && r->back_colors) {
        flat = &provoking[SGI_GL_V_BACK_COLOR];
        for (int i = 0; i < 3; i++) {
            for (int k = 0; k < 3; k++) {
                v[i].rgb[k] = MIN(MAX(verts[i][SGI_GL_V_BACK_COLOR + k],
                                      0.0f), 1.0f);
            }
        }
    }
    for (int k = 0; k < 3; k++) {
        flat_c[k] = flat[k];
    }
    flat_rgb = st->index_bits ? sgi_soft_index(r, provoking[SGI_GL_V_INDEX]) :
                                sgi_soft_rgb(r->rules, flat_c);
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
    px0 = MAX(MAX((int)min_x, vx), win->x0 - win->ox);
    px1 = MIN(MIN((int)max_x, vx + vw - 1), win->x1 - win->ox);
    py0 = MAX(MAX((int)min_y, vy), win->y0 - win->oy);
    py1 = MIN(MIN((int)max_y, vy + vh - 1), win->y1 - win->oy);

    for (int py = py0; py <= py1; py++) {
        int64_t cy = ((int64_t)py << sub) +
                     (1 << (sub - 1));

        for (int px = px0; px <= px1; px++) {
            int64_t cx = ((int64_t)px << sub) +
                         (1 << (sub - 1));
            int64_t e0 = sgi_soft_edge(&v[1], &v[2], cx, cy);
            int64_t e1 = sgi_soft_edge(&v[2], &v[0], cx, cy);
            int64_t e2 = sgi_soft_edge(&v[0], &v[1], cx, cy);
            double b1, b2, z;
            uint32_t rgb = flat_rgb;

            if (!sgi_soft_owns(&v[1], &v[2], e0) ||
                !sgi_soft_owns(&v[2], &v[0], e1) ||
                !sgi_soft_owns(&v[0], &v[1], e2)) {
                continue;
            }
            b1 = (double)e1 / area;
            b2 = (double)e2 / area;
            z = v[0].z + b1 * (v[1].z - v[0].z) + b2 * (v[2].z - v[0].z);
            if (smooth) {
                double w0 = (1.0 - b1 - b2) * v[0].inv_w;
                double w1 = b1 * v[1].inv_w, w2 = b2 * v[2].inv_w;
                double sum = w0 + w1 + w2;
                double c[3];

                for (int k = 0; k < 3; k++) {
                    c[k] = (w0 * v[0].rgb[k] + w1 * v[1].rgb[k] +
                            w2 * v[2].rgb[k]) / sum;
                }
                /*
                 * An index is taken as a difference from the first
                 * vertex's, so a constant one stays exact before its
                 * integer part is taken.
                 */
                rgb = st->index_bits ?
                      sgi_soft_index(r, v[0].index +
                                     (w1 * (v[1].index - v[0].index) +
                                      w2 * (v[2].index - v[0].index)) / sum) :
                      sgi_soft_rgb(r->rules, c);
            }
            sgi_soft_fragment(r, px, py, z, rgb);
        }
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
 * Transform, map and rasterize one width-1 segment (OpenGL 1.2.1, 3.4.1)
 * with the provoking vertex's colour when flat shaded. A fragment's
 * depth and smooth colour come from t, the centre's projection onto the
 * segment (equations 3.5 to 3.7), the colour corrected for perspective.
 *
 * Approximated like a triangle: the fragments are bounded by the viewport
 * rather than the segment clipped to the view volume, which differs only
 * in the pixels next to where a segment leaves the viewport, and a vertex
 * with w <= 0 drops the segment. The discriminating test is
 * gl-line-bounded-by-the-viewport. glLineWidth, glLineStipple and line
 * antialiasing are not modelled.
 */
static void sgi_soft_line(const SGISoftDraw *r, const float *va,
                          const float *vb, const float *provoking)
{
    const SGIGLDrawState *st = r->st;
    const SGISoftWindow *win = &r->win;
    int vx = st->viewport[0], vy = st->viewport[1];
    int vw = st->viewport[2], vh = st->viewport[3];
    bool smooth = st->shade_model != SGI_GL_FLAT;
    const int sub = r->rules->subpixel_bits;
    bool x_major;
    SGISoftVertex a, b;
    double flat[3], len2;
    int major0, major1;

    if (vw <= 0 || vh <= 0 ||
        !sgi_soft_transform(r, va, &a) ||
        !sgi_soft_transform(r, vb, &b) || (a.x == b.x && a.y == b.y)) {
        return;
    }
    for (int k = 0; k < 3; k++) {
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
            double t, wa, wb, c[3];
            uint32_t colour;

            if (px < MAX(vx, win->x0 - win->ox) ||
                px > MIN(vx + vw - 1, win->x1 - win->ox) ||
                py < MAX(vy, win->y0 - win->oy) ||
                py > MIN(vy + vh - 1, win->y1 - win->oy) ||
                !sgi_soft_line_covers(&a, &b, px, py, sub)) {
                continue;
            }
            t = ((cx - a.x) * (b.x - a.x) + (cy - a.y) * (b.y - a.y)) / len2;
            t = MIN(MAX(t, 0.0), 1.0);
            wa = (1 - t) * a.inv_w;
            wb = t * b.inv_w;
            if (st->index_bits) {
                colour = sgi_soft_index(r, smooth ?
                                        a.index + wb * (b.index - a.index) /
                                        (wa + wb) :
                                        provoking[SGI_GL_V_INDEX]);
            } else {
                for (int k = 0; k < 3; k++) {
                    c[k] = smooth ?
                           (wa * a.rgb[k] + wb * b.rgb[k]) / (wa + wb) :
                           flat[k];
                }
                colour = sgi_soft_rgb(r->rules, c);
            }
            sgi_soft_fragment(r, px, py, a.z + t * (b.z - a.z), colour);
        }
    }
}

/*
 * A size-1 point covers the pixel holding its window position (OpenGL
 * 1.2.1, 3.3), at the subpixel grid the vertex was snapped to. A point
 * outside the viewport or beyond the near or far plane is dropped, which
 * is what clipping a point does while w is positive. glPointSize and
 * point antialiasing are not modelled.
 */
static void sgi_soft_point(const SGISoftDraw *r, const float *vp)
{
    const SGIGLDrawState *st = r->st;
    const SGISoftWindow *win = &r->win;
    int vx = st->viewport[0], vy = st->viewport[1];
    int vw = st->viewport[2], vh = st->viewport[3];
    const int sub = r->rules->subpixel_bits;
    SGISoftVertex v;
    int px, py;

    if (!sgi_soft_transform(r, vp, &v)) {
        return;
    }
    px = v.x >> sub;
    py = v.y >> sub;
    if (px < MAX(vx, win->x0 - win->ox) ||
        px > MIN(vx + vw - 1, win->x1 - win->ox) ||
        py < MAX(vy, win->y0 - win->oy) ||
        py > MIN(vy + vh - 1, win->y1 - win->oy)) {
        return;
    }
    sgi_soft_fragment(r, px, py, v.z,
                      st->index_bits ? sgi_soft_index(r, v.index) :
                                       sgi_soft_rgb(r->rules, v.rgb));
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

#define V(i) (&d->vertices[(i) * SGI_GL_VERTEX_FLOATS])
    r.ntargets = sgi_gl_draw_targets(d->state, r.targets);
    if (!sgi_soft_check_rules(rules) || !sgi_soft_window(d->state, &r.win) ||
        !sgi_soft_check_index(t, d->state->index_bits)) {
        return;
    }
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
        for (uint32_t i = 0; i + 2 < n; i++) {
            sgi_soft_triangle(&r, V(0), V(i + 1), V(i + 2),
                              d->mode == SGI_GL_POLYGON ? V(0) : V(i + 2));
        }
        return;
    case SGI_GL_QUADS:
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            sgi_soft_triangle(&r, V(i), V(i + 1), V(i + 2), V(i + 3));
            sgi_soft_triangle(&r, V(i), V(i + 2), V(i + 3), V(i + 3));
        }
        return;
    case SGI_GL_QUAD_STRIP:
        /* Vertices 2i - 1, 2i, 2i + 2 and 2i + 1 in order (2.6.1). */
        for (uint32_t i = 0; i + 3 < n; i += 2) {
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
    double rgba[3] = { b->rgba[0], b->rgba[1], b->rgba[2] };
    uint32_t colour;

    if (!sgi_soft_window(b->state, &r.win) ||
        !sgi_soft_check_index(t, b->state->index_bits)) {
        return;
    }
    colour = b->state->index_bits ? sgi_soft_index(&r, b->index) :
                                    sgi_soft_rgb(rules, rgba);
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
            sgi_soft_fragment_at(&r, px, py, b->z, colour);
        }
    }
}

/*
 * CLEAR writes the clear colour, or in colour-index mode the clear index,
 * to the op's targets and the clear depth to the depth plane at every
 * pixel of its rectangle its clip list keeps. BITMAP draws its run of a
 * bitmap. The other kinds are not executed here yet.
 */
void sgi_soft_raster(const SGIRasterRules *rules, const SGIRasterTarget *t,
                     const SGIRasterOp *op)
{
    bool colour = op->clear_buffers & SGI_GL_COLOR_BUFFER_BIT;
    bool depth = op->clear_buffers & SGI_GL_DEPTH_BUFFER_BIT;
    uint32_t rgb = 0;

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
    if (colour && op->index_bits && !sgi_soft_check_index(t, op->index_bits)) {
        colour = false;
    }
    if (colour && !op->index_bits) {
        double c[3] = { op->clear_rgba[0], op->clear_rgba[1],
                        op->clear_rgba[2] };

        if (op->clear_colour_mask != UINT32_MAX) {
            qemu_log_mask(LOG_UNIMP, "sgi-gl: clear colour mask 0x%x is not "
                          "modelled\n", op->clear_colour_mask);
        }
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
            for (uint32_t i = 0; colour && i < MIN(op->ntargets, 2); i++) {
                if (op->index_bits) {
                    t->store_index(t->opaque, op->targets[i], x, y,
                                   op->clear_index, op->index_mask);
                } else {
                    t->store_colour(t->opaque, op->targets[i], x, y, rgb);
                }
            }
        }
    }
}
