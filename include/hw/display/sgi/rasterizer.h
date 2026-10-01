/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The software rasterizer the SGI graphics heads share: GL draws turned
 * into fragments and stored through the family's planes.
 */
#ifndef HW_DISPLAY_SGI_RASTERIZER_H
#define HW_DISPLAY_SGI_RASTERIZER_H

#include "hw/display/sgi/gl-context.h"

/*
 * The rules a head's rasterizer follows where OpenGL leaves a choice or a
 * head departs from it. Each head gives one set, every value labelled
 * with its evidence; the rasterizer holds no head's name.
 */

/* How a window coordinate or depth is rounded to its grid. */
typedef enum SGIRounding {
    SGI_ROUND_FLOOR,
    SGI_ROUND_HALF_UP,
    SGI_ROUND_HALF_EVEN,
    SGI_ROUND_HALF_AWAY,        /* llround() */
} SGIRounding;

/* The arithmetic attributes are interpolated in. */
typedef enum SGIArith {
    SGI_ARITH_FIXED16,
    SGI_ARITH_DOUBLE,
} SGIArith;

/* Which pixel centres on a shared triangle edge the edge owns. */
typedef enum SGITieRule {
    /* The top and left edges, with rows counted down from the top. */
    SGI_TIE_TOP_LEFT,
    /*
     * The top and left edges, with rows counted up from the bottom: a
     * counter-clockwise edge running down, or horizontal and running
     * left, owns its centres.
     */
    SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
} SGITieRule;

/* Which pixels a width-1 segment covers. */
typedef enum SGILineRule {
    /* Bresenham with the diamond-exit rule's ends. */
    SGI_LINE_BRESENHAM_DIAMOND,
    /*
     * Each pixel near the segment tested against the diamond-exit rule on
     * the subpixel grid, the minor-axis span walked in double arithmetic.
     */
    SGI_LINE_COVERAGE_DOUBLE,
} SGILineRule;

/*
 * How a fragment's colour is dithered to the rules' colour bits while
 * GL_DITHER is enabled (OpenGL 1.2.1, 4.1.7, which leaves the algorithm to
 * the implementation).
 */
typedef enum SGIDither {
    /* Rounded to the nearest step, as with GL_DITHER disabled. */
    SGI_DITHER_NONE,
    /*
     * A 4x4 ordered dither: channel c in [0, 1] is stored as
     * floor(c * max + B / 16), max the largest channel value and B the
     * threshold, 0 to 15, of the recursive 4x4 Bayer index matrix
     *
     *     0  8  2 10
     *    12  4 14  6
     *     3 11  1  9
     *    15  7 13  5
     *
     * at the pixel's column and row modulo 4, rows counted down from the
     * top. An approximation fitted to two hardware goldens, not a
     * specification.
     */
    SGI_DITHER_BAYER4_TOP_ROWS,
} SGIDither;

/* What a dither pattern is anchored to. */
typedef enum SGIDitherAnchor {
    /*
     * The display: the pattern's first column is plane column 0 and its
     * first row the target's screen_top, its rows running down the display
     * as the target says plane rows do.
     */
    SGI_DITHER_SCREEN,
    /*
     * The window, the pattern moving with it. Needs the window's top row,
     * which the drawable does not carry.
     */
    SGI_DITHER_WINDOW,
} SGIDitherAnchor;

typedef struct SGIRasterRules {
    uint8_t subpixel_bits;      /* window coordinates snap to 2^-n pixel */
    uint8_t color_bits;         /* per channel of a stored colour */
    uint8_t depth_bits;         /* of a stored depth */
    /*
     * Of a stored stencil value, 0 for planes with no stencil buffer; a
     * target without the stencil hooks has none either.
     */
    uint8_t stencil_bits;
    uint8_t rounding;           /* SGIRounding, for snapping and depth */
    uint8_t tie_rule;           /* SGITieRule */
    uint8_t arith;              /* SGIArith */
    uint8_t line_rule;          /* SGILineRule */
    uint8_t dither;             /* SGIDither */
    uint8_t dither_anchor;      /* SGIDitherAnchor */
    /* Where the values come from, for logs and reports. */
    const char *evidence;
    uint8_t alpha_bits;         /* stored A width, zero means no alpha */
} SGIRasterRules;

/*
 * Whether the rasterizer implements a rule set. It implements subpixel_bits
 * 1 to 12, color_bits 1 to 10, alpha_bits 0 to 10, depth_bits 1 to 32,
 * stencil_bits 0 to 16,
 * every rounding and dither, and otherwise only
 * SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN, SGI_ARITH_DOUBLE,
 * SGI_LINE_COVERAGE_DOUBLE and SGI_DITHER_SCREEN; the anchor is not
 * checked without a dither.
 */
bool sgi_raster_rules_supported(const SGIRasterRules *r);

/*
 * A vertex in clip coordinates, as view-volume clipping (OpenGL 1.2.1,
 * 2.11) takes and makes it: its clip and eye coordinates and its
 * attributes in the draw's vertex layout.
 */
typedef struct SGIVolumeVertex {
    double clip[4];
    double eye[4];
    float attr[SGI_GL_VERTEX_FLOATS];
} SGIVolumeVertex;

/* The most vertices clipping a triangle by the six planes leaves. */
#define SGI_VOLUME_TRIANGLE_MAX (3 + 6)

/*
 * Whether a point lies in the view volume, -w <= x, y, z <= w. No point
 * with w < 0 does.
 */
bool sgi_volume_contains(const SGIVolumeVertex *v);

/*
 * Clip a triangle to the view volume. out takes what is left, a convex
 * polygon in the triangle's winding, and the count is returned: 0 when
 * nothing is left, otherwise 3 to SGI_VOLUME_TRIANGLE_MAX. A vertex a
 * plane makes lies on the edge it cuts, its clip and eye coordinates and
 * float attributes interpolated at the same point (2.13.8), always from
 * the edge's inside end, so two triangles sharing the edge make the same
 * vertex; the material index and edge flag, bit patterns, are the inside
 * end's. A triangle inside every plane is returned unchanged.
 */
unsigned sgi_volume_clip_triangle(const SGIVolumeVertex in[3],
                                  SGIVolumeVertex out[SGI_VOLUME_TRIANGLE_MAX]);

/*
 * Clip a segment to the view volume in place, each end moved along the
 * segment to where it enters the volume and its attributes interpolated
 * as a triangle's are; false when no part of it lies inside.
 */
bool sgi_volume_clip_segment(SGIVolumeVertex *a, SGIVolumeVertex *b);

#define SGI_GL_TEX_LEVELS_MAX 31u

/* Absolute level identities in a borrowed, immutable normalized image view. */
typedef struct SGIGLTexLevel {
    uint32_t level;
    uint32_t width, height;
    uint32_t base_format;
    uint32_t internal_format;
    uint32_t border;
    bool (*read_texel)(void *opaque, uint32_t level,
                       uint32_t x, uint32_t y, uint32_t *rgba);
    void *opaque;
} SGIGLTexLevel;

typedef struct SGIGLTexPyramid {
    const SGIGLTexLevel *levels;
    uint32_t count;
    uint32_t base_level, max_level;
    double min_lod, max_lod;
} SGIGLTexPyramid;

/*
 * One family-owned level-zero image and its texture object's sampler.
 * Dimensions count interior texels; row zero has the lowest T. read_texel
 * gets in-range interior indices and returns canonical RRGGBBAA bytes.
 * The family validates storage and guest addressing. It owns object state,
 * binding and migration, rebuilding this borrowed descriptor after load.
 * Image and descriptor stay immutable through each synchronous draw; the
 * callback must neither change the binding nor re-enter drawing. Concurrent
 * mutation needs family locking or a stable snapshot. No pointer is retained.
 */
typedef struct SGIGLTexImage {
    uint32_t width, height;
    uint32_t base_format;
    uint32_t border;
    uint32_t min_filter, mag_filter;
    uint32_t wrap_s, wrap_t;
    float border_color[4];
    bool (*read_texel)(void *opaque, uint32_t x, uint32_t y, uint32_t *rgba);
    void *opaque;
    /*
     * NULL preserves the level-zero interface; otherwise image fields above
     * are ignored. The sampler fields still apply to every selected level.
     */
    const SGIGLTexPyramid *pyramid;
} SGIGLTexImage;

/*
 * The family's planes as the rasterizer reaches them, in plane pixels.
 * target is one of the drawable's colour targets, which only the family
 * interprets; a colour is the rules' color_bits a channel, red highest
 * and blue lowest. In colour-index mode store_index takes the index,
 * already masked to the draw state's index bits, and the write mask, whose
 * clear bits keep the stored index's (OpenGL 1.2.1, 4.2.2); a family
 * without colour-index planes leaves it NULL, and colour-index work then
 * draws nothing.
 */
/* Quantized scalar channels, each normalized by its own declared maximum. */
typedef struct SGIRasterColour {
    uint32_t component[4];      /* R, G, B, A; absent alpha is canonical zero */
} SGIRasterColour;

typedef struct SGIRasterTarget {
    void (*store_colour)(void *opaque, SGIColourTarget target, int x, int y,
                         uint32_t colour);
    /*
     * The colour stored at (x, y) as store_colour takes it, or in
     * colour-index mode the stored index; NULL for planes that cannot be
     * read back, where the operations that read the destination draw as
     * if disabled.
     */
    uint32_t (*load_colour)(void *opaque, SGIColourTarget target, int x,
                            int y);
    void (*store_index)(void *opaque, SGIColourTarget target, int x, int y,
                        uint32_t index, uint32_t mask);
    uint32_t (*load_depth)(void *opaque, int x, int y);
    void (*store_depth)(void *opaque, int x, int y, uint32_t depth);
    /*
     * The stencil buffer, a value of the rules' stencil bits a pixel;
     * NULL for planes without one, where the stencil test passes and
     * changes nothing (OpenGL 1.2.1, 4.1.5).
     */
    uint32_t (*load_stencil)(void *opaque, int x, int y);
    void (*store_stencil)(void *opaque, int x, int y, uint32_t value);
    /*
     * A fog factor the family supplies, for a board that takes fog as a
     * compiled curve rather than glFog's mode, density, start and end.
     * While fog is enabled and fog_factor is set, the rasterizer calls it
     * once per fragment, after interpolation, with the fragment's window
     * depth in [0, 1] (the z its depth test uses) and its eye distance
     * |z| (the fog coordinate the built-in modes take), and uses the
     * return value clamped to [0, 1] as f in place of the mode's. The
     * blend with the fog colour and fog index is unchanged. With
     * fog_factor NULL the built-in mode applies.
     *
     * Not GL state and not migrated: it is per draw, set by the family
     * with the rest of the target, and gets the target's opaque.
     */
    float (*fog_factor)(void *opaque, float window_z, float eye_z);
    void *opaque;
    /*
     * Where a dither anchored to the screen counts its rows from: the
     * plane row of the display's top line, and whether plane rows below it
     * have larger y, as in planes whose row 0 is the top line, or smaller.
     * Read only under such a dither.
     */
    int32_t screen_top;
    bool screen_rows_down;
    /* Current binding; resolved once by the family before drawing. */
    const SGIGLTexImage *texture;
    /*
     * Optional complete RGBA stores and reads. Positive alpha_bits requires
     * this store. Destination-dependent RGBA work requires the matching read,
     * which must return in-range components or false. Required reads for all
     * selected targets precede any pixel writes; failures drop that pixel's
     * colour, depth and stencil writes. Missing support drops the operation.
     * Spatial alpha dithering and clear dithering remain unimplemented.
     * Borrowed, never migrated.
     */
    void (*store_rgba)(void *opaque, SGIColourTarget target, int x, int y,
                       const SGIRasterColour *colour);
    bool (*load_rgba)(void *opaque, SGIColourTarget target, int x, int y,
                      SGIRasterColour *colour);
} SGIRasterTarget;

/*
 * Rasterize a draw into the target, under the draw's state and the head's
 * rules. A rule set sgi_raster_rules_supported() refuses draws nothing.
 * Aliased dimensions round half up to at least one; zero defaults to one.
 * Rounded widths above INT32_MAX exceed the internal arithmetic domain, not
 * an established hardware maximum. Polygon LINE stipple uses a draw-local
 * counter. Standalone lines borrow and commit d's phase after coverage,
 * or count locally when no phase is supplied. Smoothing is omitted.
 * Filled samples use d's borrowed canonical window stipple rows.
 * Borrowed modes select each polygon face's FILL/LINE/POINT policy; absent
 * modes retain FILL. Boundary workspace failure returns OUT_OF_MEMORY
 * before any fragment of that polygon. Callers must forward that result.
 */
SGIGLDrawResult sgi_soft_draw(const SGIRasterRules *r, const SGIRasterTarget *t,
                   const SGIGLDraw *d);
/* Carry out a raster op on the target, under the head's rules. */
void sgi_soft_raster(const SGIRasterRules *r, const SGIRasterTarget *t,
                     const SGIRasterOp *op);

#endif
