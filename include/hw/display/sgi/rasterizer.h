/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The software rasterizer the SGI graphics heads share: GL draws turned
 * into fragments and stored through the family's planes.
 */
#ifndef HW_DISPLAY_SGI_RASTERIZER_H
#define HW_DISPLAY_SGI_RASTERIZER_H

#include "hw/display/sgi/gl-context.h"
#include "ui/sgi-renderer.h"

/*
 * The rules a head's rasterizer follows where OpenGL leaves a choice or a
 * head departs from it. Each head gives one set; the rasterizer holds no
 * head's name.
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

/*
 * The coverage a smoothed point or line gives each fragment, under rules
 * with smooth_primitives (OpenGL 1.2.1, 3.2, which leaves the method to
 * the implementation within its stated conditions).
 */
typedef enum SGISmoothRule {
    /*
     * OpenGL's exact box filter: the area of the fragment square inside a
     * circle of the point's size centred on it (3.3.2), or inside the
     * rectangle the line's width spans about the segment, its ends
     * perpendicular through the endpoints (3.4.2).
     */
    SGI_SMOOTH_AREA,
    /*
     * A separable filter: in each axis a box of the point's size or the
     * line's width filtered by a one-pixel tent, a fragment whose centre
     * is d from the middle weighted min(1, max(0, (size + 1) / 2 - |d|)).
     * At size 1 that is 1 - |d|: a point weights the 2x2 pixels whose
     * centres surround it bilinearly, and a line the two minor-axis pixels
     * whose centres straddle it in each major-axis column. A point's weight
     * is the product of its x and y weights. A line's is its minor-axis
     * weight scaled by sqrt(1 + slope^2), so a diagonal is as bright as an
     * axis line, clamped to 1, and times the part of the major-axis column
     * the segment spans, which fades an endpoint across its pixel.
     */
    SGI_SMOOTH_TWO_PIXEL_FILTER,
} SGISmoothRule;

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
    uint8_t alpha_bits;         /* stored A width, zero means no alpha */
    uint8_t alpha_test_bits; /* Zero derives precision from color/alpha bits. */
    /*
     * Smooth points, lines and polygons as their enables ask; without it a
     * smoothed primitive is drawn aliased and reported. Points and lines
     * follow smooth_rule; polygons always take area coverage.
     */
    bool smooth_primitives;
    uint8_t smooth_rule;        /* SGISmoothRule */
    /*
     * A head whose depth is linear in eye distance, as a w-buffer keeps
     * it, in place of OpenGL's, which is linear in window coordinates: a
     * vertex's window depth is depth_row . e for its eye coordinates e,
     * and a fragment's is that interpolated with perspective correction,
     * its polygon offset scaled with it. The projection's z row then has
     * no say in depth, and a depth outside [0, 1] is dropped per fragment,
     * which is the near and far clip. The row depends on the head's own
     * state, so it travels with the rules of the draw.
     */
    bool depth_row_given;
    double depth_row[4];
    /*
     * With depth_row, a second row whose product with the eye coordinates
     * divides the first's: a vertex's window depth is (depth_row . e) /
     * (depth_divisor . e), each over clip w interpolated linearly in the
     * window, so their ratio is exact at every fragment whether the
     * divisor is constant, as under an affine model-view, or varies with
     * eye-space w. Not given, the divisor is one.
     */
    bool depth_divisor_given;
    double depth_divisor[4];
    /*
     * With depth_row, how far outside [0, 1] a fragment's depth may lie and
     * still be drawn, clamped to the range, rather than dropped as clipped:
     * room for the rounding of a row the head was given in single
     * precision. Zero drops every depth outside the range.
     */
    double depth_slack;
    /*
     * Stored depth is a floating-point code, not a fixed-point fraction:
     * the IEEE single-precision bits of the window depth shifted right by
     * 30 - depth_bits, which keeps the order of depths in [0, 1] and their
     * relative precision near 0 (sgi_raster_depth_code()). Needs
     * depth_bits 10 to 30.
     */
    bool depth_float;
} SGIRasterRules;

/*
 * Whether the rasterizer implements a rule set. It implements subpixel_bits
 * 1 to 12, color_bits 1 to 10, alpha_bits 0 to 10, depth_bits 1 to 32
 * (10 to 30 for a floating-point depth code), a finite depth row,
 * stencil_bits 0 to 16,
 * every rounding, dither and smooth rule, and otherwise only
 * SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN, SGI_ARITH_DOUBLE,
 * SGI_LINE_COVERAGE_DOUBLE and SGI_DITHER_SCREEN; the anchor is not
 * checked without a dither.
 */
bool sgi_raster_rules_supported(const SGIRasterRules *r);
/*
 * The coverage in [0, 1] the rules' smooth_rule gives the fragment square
 * with lower left corner (x, y) of a smoothed point of the given size
 * centred at (cx, cy) in window coordinates, or of a smoothed line of the
 * given width from (ax, ay) to (bx, by).
 */
double sgi_raster_point_coverage(const SGIRasterRules *r, double cx,
                                 double cy, double size, int x, int y);
double sgi_raster_line_coverage(const SGIRasterRules *r, double ax,
                                double ay, double bx, double by,
                                double width, int x, int y);
/* Packed RGB bits selected by channel masks under supported rules. */
uint32_t sgi_raster_colour_mask(const SGIRasterRules *r, uint32_t mask);
/* Compare incoming alpha and reference at the rules' alpha-test precision. */
bool sgi_raster_alpha_test(const SGIRasterRules *r, uint32_t func,
                           double alpha, double ref);
/* Packed RGB blending with quantized source alpha, destination alpha 1. */
uint32_t sgi_raster_blend_rgb(const SGIRasterRules *rules,
                              const SGIGLDrawState *st, uint32_t src,
                              double alpha, uint32_t dst);

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

/* Transform one draw vertex before clipping, retaining its attributes. */
void sgi_volume_from_draw_vertex(const SGIGLDrawState *state,
                                 const float *vertex, SGIVolumeVertex *out);

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
    uint32_t width, height, depth;
    uint32_t base_format;
    uint32_t internal_format;
    uint32_t border;
    bool (*read_texel)(void *opaque, uint32_t level,
                       uint32_t x, uint32_t y, uint32_t *rgba);
    /* Border indices range from -1 through width/height, inclusive. */
    bool (*read_border_texel)(void *opaque, uint32_t level,
                              int32_t x, int32_t y, uint32_t *rgba);
    bool (*read_texel_3d)(void *opaque, uint32_t level, uint32_t x,
                          uint32_t y, uint32_t z, uint32_t *rgba);
    void *opaque;
    /* Each signed axis ranges from -1 through its interior dimension. */
    bool (*read_border_texel_3d)(void *opaque, uint32_t level,
                                 int32_t x, int32_t y, int32_t z,
                                 uint32_t *rgba);
    /* Raw readers use base-format slots and per-channel widths. */
    uint8_t component_bits[4];
    bool (*read_texel_components)(void *opaque, uint32_t level, uint32_t x,
                                  uint32_t y, uint16_t component[4]);
    bool (*read_border_texel_components)(void *opaque, uint32_t level,
                                         int32_t x, int32_t y,
                                         uint16_t component[4]);
    bool (*read_texel_3d_components)(void *opaque, uint32_t level,
                                     uint32_t x, uint32_t y, uint32_t z,
                                     uint16_t component[4]);
    bool (*read_border_texel_3d_components)(void *opaque, uint32_t level,
                                            int32_t x, int32_t y, int32_t z,
                                            uint16_t component[4]);
} SGIGLTexLevel;

typedef struct SGIGLTexPyramid {
    const SGIGLTexLevel *levels;
    uint32_t count;
    uint32_t base_level, max_level;
    double min_lod, max_lod;
} SGIGLTexPyramid;

/*
 * One family-owned level-zero image and its texture object's sampler.
 * volume selects 3D sampling, including when depth is one. one_dimensional
 * selects S-only sampling with height one; T/R coordinates and derivatives
 * are ignored. The two flags are mutually exclusive. Otherwise depth and
 * wrap_r are ignored. Dimensions count interior texels; row zero has the
 * lowest T and slice zero the lowest R. Interior callbacks get in-range
 * indices; stored-border callbacks get signed indices. Packed callbacks
 * return canonical RRGGBBAA bytes; raw-component callbacks return
 * base-format channels in the corresponding slots, normalized using the
 * declared component widths. DEPTH_COMPONENT callbacks instead return an
 * unsigned normalized 32-bit depth value through the packed callback.
 * The family validates storage and guest addressing. It owns object state,
 * binding and migration, rebuilding this borrowed descriptor after load.
 * Image and descriptor stay immutable through each synchronous draw; the
 * callback must neither change the binding nor re-enter drawing. Concurrent
 * mutation needs family locking or a stable snapshot. No pointer is retained.
 */
typedef struct SGIGLTexImage {
    uint32_t width, height, depth;
    uint32_t base_format;
    uint32_t border;
    uint32_t min_filter, mag_filter;
    uint32_t wrap_s, wrap_t, wrap_r;
    float border_color[4];
    bool (*read_texel)(void *opaque, uint32_t x, uint32_t y, uint32_t *rgba);
    bool (*read_texel_3d)(void *opaque, uint32_t x, uint32_t y, uint32_t z,
                          uint32_t *rgba);
    bool volume;
    bool one_dimensional;
    void *opaque;
    /*
     * NULL selects the level-zero image fields. Otherwise dimensions, format,
     * border and read callbacks come from the selected level. Sampler fields,
     * dimensionality flags, opaque and post_filter apply to both interfaces.
     * Volume levels halve all three interior dimensions.
     */
    const SGIGLTexPyramid *pyramid;
    /* Required for a 2D image with border == 1; outside the interior only. */
    bool (*read_border_texel)(void *opaque, int32_t x, int32_t y,
                              uint32_t *rgba);
    /* Positive per-axis derivative factors; zero preserves identity. */
    double lod_scale_s, lod_scale_t;
    /* Expanded RGBA after filtering, before the texture environment. */
    void (*post_filter)(void *opaque, double rgba[4]);
    /* Required for a volume with border == 1, including depth one. */
    bool (*read_border_texel_3d)(void *opaque, int32_t x, int32_t y,
                                 int32_t z, uint32_t *rgba);
    /* Positive volume R derivative factor; zero preserves identity. */
    double lod_scale_r;
    /* NULL selects the public default curve; lambda is already clamped. */
    bool (*read_filter_weight)(void *opaque, double lambda, double *weight);
    /* Borrowed level-zero detail image; L <= 0, mode ADD or MODULATE. */
    const SGIGLTexLevel *detail_image;
    int32_t detail_level;
    uint32_t detail_mode;
    /* Zero disables comparison. SGIX operators compare depth to projected R. */
    uint32_t shadow_compare;
    double shadow_ambient;
    /* Appended to preserve the existing packed-callback initializer layout. */
    uint8_t component_bits[4];
    bool (*read_texel_components)(void *opaque, uint32_t x, uint32_t y,
                                  uint16_t component[4]);
    bool (*read_texel_3d_components)(void *opaque, uint32_t x, uint32_t y,
                                     uint32_t z, uint16_t component[4]);
    bool (*read_border_texel_components)(void *opaque, int32_t x, int32_t y,
                                         uint16_t component[4]);
    bool (*read_border_texel_3d_components)(void *opaque, int32_t x, int32_t y,
                                            int32_t z,
                                            uint16_t component[4]);
    /*
     * GL_SGIX_texture_coordinate_clamp: with max_clamp set, an axis whose
     * wrap is CLAMP clamps its coordinate to [0, max_clamp_*], each in
     * [0, 1], instead of [0, 1]. Other wrap modes ignore it.
     */
    bool max_clamp;
    double max_clamp_s, max_clamp_t, max_clamp_r;
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

typedef enum SGIRenderResult {
    SGI_RENDER_OK,
    SGI_RENDER_OOM,
    SGI_RENDER_UNSUPPORTED,
    SGI_RENDER_RESOURCE_ERROR,
    SGI_RENDER_HOST_ERROR,
} SGIRenderResult;

typedef enum SGIRenderRole {
    SGI_RENDER_COLOUR, SGI_RENDER_INDEX, SGI_RENDER_DEPTH,
    SGI_RENDER_STENCIL, SGI_RENDER_OVERLAY, SGI_RENDER_WID,
    SGI_RENDER_TAG, SGI_RENDER_ACCUM, SGI_RENDER_TEXTURE,
} SGIRenderRole;

typedef enum SGIRenderBuffer {
    SGI_RENDER_BUFFER_NONE, SGI_RENDER_FRONT, SGI_RENDER_BACK,
    SGI_RENDER_AUX,
} SGIRenderBuffer;

typedef enum SGIRenderEye {
    SGI_RENDER_MONO, SGI_RENDER_LEFT, SGI_RENDER_RIGHT,
} SGIRenderEye;

typedef enum SGIRenderNumeric {
    SGI_RENDER_UINT, SGI_RENDER_SINT, SGI_RENDER_UNORM,
    SGI_RENDER_SNORM, SGI_RENDER_FLOAT,
} SGIRenderNumeric;

typedef enum SGIRenderChannel {
    SGI_RENDER_R, SGI_RENDER_G, SGI_RENDER_B, SGI_RENDER_A,
    SGI_RENDER_SCALAR,
} SGIRenderChannel;

typedef struct SGIRenderComponent {
    SGIRenderChannel channel;
    SGIRenderNumeric numeric;
    uint8_t storage_bits;
} SGIRenderComponent;

typedef struct SGIRenderResource {
    uint64_t backing_id, backing_generation;
    uint64_t view_id, view_generation;
    SGIRenderRole role;
    SGIColourTarget target;
    SGIRenderBuffer buffer;
    uint32_t auxiliary_index;
    SGIRenderEye eye;
    uint32_t width, height, depth;
    uint32_t texture_level;
    uint8_t border[3];
    bool bottom_origin;
    uint8_t component_count;
    SGIRenderComponent component[4];
} SGIRenderResource;

typedef struct SGIRenderRect {
    uint32_t x0, y0, x1, y1;
} SGIRenderRect;

typedef struct SGIRenderCommitResult {
    SGIRenderResult status;
    uint64_t complete_pixels;
    bool partial_pixel;
    bool extent_known;
} SGIRenderCommitResult;

/* One byte per pixel, in the resource's row order, borrowed for one call. */
typedef struct SGIRenderWriteCoverage {
    const uint8_t *pixel;
    size_t stride_bytes;
    size_t capacity_bytes;
} SGIRenderWriteCoverage;

typedef struct SGIRenderResourceOps {
    SGIRenderResult (*snapshot)(void *opaque,
                                const SGIRenderResource *resource,
                                uint32_t slice, const SGIRenderRect *rect,
                                uint32_t *slots, size_t stride_slots,
                                size_t capacity_slots, Error **errp);
    SGIRenderCommitResult (*commit)(void *opaque,
                                    const SGIRenderResource *resource,
                                    uint32_t slice, const SGIRenderRect *rect,
                                    const uint32_t *slots, size_t stride_slots,
                                    size_t capacity_slots, Error **errp);
    SGIRenderCommitResult (*commit_covered)(
        void *opaque, const SGIRenderResource *resource, uint32_t slice,
        const SGIRenderRect *rect, const uint32_t *slots,
        size_t stride_slots, size_t capacity_slots,
        const SGIRenderWriteCoverage *writes, Error **errp);
    /*
     * An index resource's commit, the only one an indexed draw makes; an
     * index resource without it is refused before its snapshot. slots
     * hold final indices: the shared code has already merged the new
     * index into the old one under effective_mask (the draw's index mask
     * limited to the storage width), so the family stores the storage
     * bits of each covered pixel once and never merges again. A covered
     * pixel is one a fragment reached, whatever the mask and whether or
     * not its value changed; the family may still apply its own policy
     * for effective_mask, such as writing nothing while it is zero. An
     * uncovered pixel must keep its index, tag, slot and dirty state.
     */
    SGIRenderCommitResult (*commit_index_covered)(
        void *opaque, const SGIRenderResource *resource, uint32_t slice,
        const SGIRenderRect *rect, const uint32_t *slots,
        size_t stride_slots, size_t capacity_slots,
        const SGIRenderWriteCoverage *writes, uint32_t effective_mask,
        Error **errp);
} SGIRenderResourceOps;

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
     * An opaque polygon stipple, for a head that paints a stipple's clear
     * bits in a background colour where OpenGL discards them: a polygon
     * fragment whose stipple bit is clear takes stipple_background as its
     * colour, or stipple_background_index in colour-index mode, and goes on
     * through the per-fragment operations. Per draw, never migrated.
     */
    bool stipple_opaque;
    float stipple_background[4];
    uint32_t stipple_background_index;
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
    /* Borrowed until the synchronous render submission returns. */
    const SGIRenderResource *resources;
    size_t resource_count;
    const SGIRenderResourceOps *resource_ops;
    /*
     * Whether the callbacks above may run on several threads at once for
     * different pixels, while the thread that called the rasterizer waits
     * for them, holding whatever locks it holds (the BQL, for a device).
     * A large draw into such a target is drawn in bands of rows in
     * parallel (sgi_raster_set_parallel()); every pixel still sees the
     * same calls in the same order, but calls for different pixels
     * interleave. Off, the default, keeps every call on the caller's
     * thread.
     */
    bool concurrent_pixels;
    /*
     * Host-only, for tests: when nonzero, the pixel count from which a
     * draw or clear into this target runs in bands, in place of the
     * process-wide threshold (sgi_raster_set_parallel()). A family's
     * qtest sets it through a test property so that small draws, and
     * every callback they reach, run on band workers.
     */
    uint32_t band_min_pixels;
} SGIRasterTarget;

/*
 * Band-parallel drawing's limits: at most this many bands, by default
 * this many or the host's processor count if fewer, and only for draws of
 * filled primitives whose bounding boxes cover at least this many pixels.
 */
#define SGI_RASTER_PARALLEL_MAX_BANDS 16
#define SGI_RASTER_PARALLEL_DEFAULT_BANDS 4
#define SGI_RASTER_PARALLEL_MIN_PIXELS 2048

/*
 * Set the bands (0 for the default; 1 draws on one thread) and the pixel
 * threshold for every later draw, for tests and measurements.
 */
void sgi_raster_set_parallel(unsigned bands, uint64_t min_pixels);

SGIRenderResult sgi_render_resource_snapshot(const SGIRasterTarget *target,
                                              const SGIRenderResource *resource,
                                              uint32_t slice,
                                              const SGIRenderRect *rect,
                                              uint32_t *slots,
                                              size_t stride_slots,
                                              size_t capacity_slots,
                                              Error **errp);
SGIRenderCommitResult sgi_render_resource_commit(
    const SGIRasterTarget *target, const SGIRenderResource *resource,
    uint32_t slice, const SGIRenderRect *rect, const uint32_t *slots,
    size_t stride_slots, size_t capacity_slots, Error **errp);
SGIRenderCommitResult sgi_render_resource_commit_covered(
    const SGIRasterTarget *target, const SGIRenderResource *resource,
    uint32_t slice, const SGIRenderRect *rect, const uint32_t *slots,
    size_t stride_slots, size_t capacity_slots,
    const SGIRenderWriteCoverage *writes, Error **errp);
/*
 * The checked index commit: resource must be a one-component unsigned
 * index resource, and effective_mask may hold no bit above its storage
 * width.
 */
SGIRenderCommitResult sgi_render_resource_commit_index_covered(
    const SGIRasterTarget *target, const SGIRenderResource *resource,
    uint32_t slice, const SGIRenderRect *rect, const uint32_t *slots,
    size_t stride_slots, size_t capacity_slots,
    const SGIRenderWriteCoverage *writes, uint32_t effective_mask,
    Error **errp);
typedef struct SGIRenderer SGIRenderer;

/*
 * Why a checked draw or raster operation was refused: one feature row each,
 * so a family can count its fallbacks by reason. A refusal is decided
 * before any native access, or, for the last two, before the first native
 * write.
 */
typedef enum SGIRenderRefusal {
    SGI_RENDER_REFUSAL_NONE,
    /* State the shared renderer does not draw. */
    SGI_RENDER_REFUSAL_RULES,           /* raster rules */
    SGI_RENDER_REFUSAL_DITHER,          /* ordered dithering */
    SGI_RENDER_REFUSAL_PRIMITIVE,       /* mode, polygon mode, stipple */
    SGI_RENDER_REFUSAL_SMOOTHING,       /* smoothed points and lines */
    /* Smooth colour-index shading between different vertex indices. */
    SGI_RENDER_REFUSAL_COLOUR_INDEX,
    /* A smooth colour-index draw with a vertex outside the view volume. */
    SGI_RENDER_REFUSAL_INDEX_CLIPPED,
    SGI_RENDER_REFUSAL_BUFFERS,         /* draw buffers, colour mask */
    SGI_RENDER_REFUSAL_FRAGMENT_TESTS,  /* depth, stencil and alpha tests */
    SGI_RENDER_REFUSAL_FOG,
    SGI_RENDER_REFUSAL_FACING,          /* culling, back colours */
    SGI_RENDER_REFUSAL_CLIP_PLANES,
    SGI_RENDER_REFUSAL_LOGIC_OP,
    SGI_RENDER_REFUSAL_POLYGON_OFFSET,
    SGI_RENDER_REFUSAL_WINDOW,          /* window matrix, drawable origin */
    SGI_RENDER_REFUSAL_GEOMETRY,        /* non-finite or out-of-range input */
    SGI_RENDER_REFUSAL_LINE_SHAPE,      /* a segment not drawn exactly */
    SGI_RENDER_REFUSAL_RESOURCE,        /* target resource layout */
    SGI_RENDER_REFUSAL_STORE_WIDTH,     /* widths not proven */
    SGI_RENDER_REFUSAL_BLEND,           /* equation, factor or width */
    SGI_RENDER_REFUSAL_BLEND_OVERLAP,   /* blending several primitives */
    SGI_RENDER_REFUSAL_TEXTURE,         /* texture state or resource */
    SGI_RENDER_REFUSAL_PROVIDER,        /* missing resource callbacks */
    SGI_RENDER_REFUSAL_RASTER_OP,       /* raster operation or plane */
    SGI_RENDER_REFUSAL_MIXED_FRAME,     /* outside the proven set */
    /* Host limits, which vary by host. */
    SGI_RENDER_REFUSAL_HOST_VIEWPORT,
    SGI_RENDER_REFUSAL_HOST_POINT_SIZE,
    SGI_RENDER_REFUSAL_HOST_PROGRAM,    /* OpenGL too old for a program */
    SGI_RENDER_REFUSAL_HOST_PROBE,      /* lines or smooth triangles differ */
    SGI_RENDER_REFUSAL_HOST_STAGING,    /* host vertex or texture bounds */
    SGI_RENDER_REFUSAL_HOST_DRIVER,     /* mixed frame off the reference */
    /* The family's own refusals. */
    SGI_RENDER_REFUSAL_SNAPSHOT,        /* a view cannot hold stored data */
    SGI_RENDER_REFUSAL_PRECISION,       /* staged pixels differ */
    SGI_RENDER_REFUSAL__MAX,
} SGIRenderRefusal;

typedef enum SGIRenderRefusalClass {
    SGI_RENDER_REFUSAL_CLASS_STATE,
    SGI_RENDER_REFUSAL_CLASS_HOST_LIMIT,
    SGI_RENDER_REFUSAL_CLASS_PRECISION,
} SGIRenderRefusalClass;

/* A stable lowercase name for counters and traces, such as "fog". */
const char *sgi_render_refusal_name(SGIRenderRefusal reason);
SGIRenderRefusalClass sgi_render_refusal_class(SGIRenderRefusal reason);

/* Borrowed for one attempt; a zeroed struct asks for neither feature. */
typedef struct SGIRenderAttemptOptions {
    /*
     * The caller draws refused work on its scalar path within the same
     * frame, so refuse everything the scalar comparison tests do not prove
     * pixel-identical, as SGI_RENDER_REFUSAL_MIXED_FRAME.
     */
    bool mixed_frame;
    /*
     * Shadow comparison: the scalar draw has already made this draw's
     * once-per-draw reports, so the attempt makes none of its own.
     */
    bool quiet;
    /*
     * Optional precision check of each resource's staged pixels, after
     * host readback and before the first native commit. slots and writes
     * are as the commit would receive them. OK accepts, UNSUPPORTED
     * refuses as SGI_RENDER_REFUSAL_PRECISION and commits nothing; any
     * other result is a failure. It must change no native state.
     */
    SGIRenderResult (*check_staged)(void *opaque,
                                    const SGIRenderResource *resource,
                                    const SGIRenderRect *rect,
                                    const uint32_t *slots,
                                    size_t stride_slots,
                                    size_t capacity_slots,
                                    const SGIRenderWriteCoverage *writes,
                                    Error **errp);
    void *opaque;
} SGIRenderAttemptOptions;

typedef struct SGIRenderAttemptOutcome {
    /*
     * SGI_RENDER_OK, SGI_RENDER_UNSUPPORTED when refused, or the cause of
     * a failure: OOM, RESOURCE_ERROR or HOST_ERROR.
     */
    SGIRenderResult status;
    SGIRenderRefusal reason;    /* NONE unless refused */
} SGIRenderAttemptOutcome;

/*
 * Attempt a draw or raster operation on Mesa, for a caller that draws a
 * refused one on its scalar path instead. OK: every required pixel is
 * committed. REFUSED: nothing visible happened, no failure latched, and
 * outcome->reason says why. A snapshot callback refuses by returning
 * SGI_RENDER_UNSUPPORTED, and may do so only when it has read native
 * storage and changed nothing. FAILED: an allocation, resource, host or
 * commit failure, possibly after native writes; never retry it on the
 * scalar path. sgi_render_draw_checked() and sgi_render_raster_checked()
 * are these without options, returning outcome->status.
 */
SGIRenderDrawResult sgi_render_draw_attempt(
    SGIRenderer *renderer, const SGIRasterRules *rules,
    const SGIRasterTarget *target, const SGIGLDraw *draw,
    const SGIRenderAttemptOptions *opts, SGIRenderAttemptOutcome *outcome,
    Error **errp);
SGIRenderDrawResult sgi_render_raster_attempt(
    SGIRenderer *renderer, const SGIRasterRules *rules,
    const SGIRasterTarget *target, const SGIRasterOp *op,
    const SGIRenderAttemptOptions *opts, SGIRenderAttemptOutcome *outcome,
    Error **errp);

/*
 * A draw's state signature, which shadow comparison counts results by.
 * Version 1 holds the primitive class; the aliased point size, line width
 * and stipple enables; the shade model; blend enable, equation and
 * factors; texture enable, environment, filters, wraps and image size; the
 * raster rules' subpixel bits, rounding, tie rule, arithmetic and
 * smoothing; the colour and alpha storage widths; the row origin; whether
 * the viewport, scissor, clip rectangles and matrices each meet the
 * comparison fixture's conditions; and whether any vertex, after the
 * scalar path clips it, has a depth within 2^-32 of the near or far plane.
 * Version 2 adds the colour-index width, zero for RGBA; an indexed draw's
 * storage widths and row origin are its index resource's. Adding a field
 * the mixed-frame check accepts and that can change the result increments
 * the version; state neither path reads, such as the colour mask in index
 * mode, stays out.
 */
#define SGI_RENDER_SIGNATURE_VERSION 2
#define SGI_RENDER_SIGNATURE_PREFIX "v2 "

typedef struct SGIRenderSignature {
    uint32_t version;
    /*
     * Canonical text, equal exactly when the signatures are, "v2 ...".
     * Text that would not fit becomes "v2 overflow" rather than truncate.
     */
    char text[512];
    /* The mixed-frame check accepts the draw on the reference driver. */
    bool in_set;
} SGIRenderSignature;

void sgi_render_draw_signature(const SGIRasterRules *rules,
                               const SGIRasterTarget *target,
                               const SGIGLDraw *draw,
                               SGIRenderSignature *signature);
/*
 * Test-only: treat signature text of this many characters or more as
 * truncated, which makes it the single signature "v2 overflow"; zero
 * restores the buffer's own bound.
 */
void sgi_render_test_signature_limit(size_t length);
/*
 * A decoded raster operation's signature, under the same version: the
 * operation, the buffers it clears, the first colour target's storage
 * widths and row origin, and the raster rules; in-set when the mixed-frame
 * check accepts it on the reference driver.
 */
void sgi_render_raster_signature(const SGIRasterRules *rules,
                                 const SGIRasterTarget *target,
                                 const SGIRasterOp *op,
                                 SGIRenderSignature *signature);

SGIRenderResult sgi_render_draw_checked(SGIRenderer *renderer,
                                        const SGIRasterRules *rules,
                                        const SGIRasterTarget *target,
                                        const SGIGLDraw *draw, Error **errp);
/*
 * Test-only: move every Mesa line parallelogram this many subpixels along
 * its minor axis, so a renderer created afterwards fails its line probe.
 */
void sgi_render_test_line_band(int subpixels);
/*
 * Test-only: move every Mesa smooth triangle this many subpixels right, so
 * a renderer created afterwards fails its smooth triangle probe.
 */
void sgi_render_test_window_shift(int subpixels);
/*
 * Test-only: run the smooth triangle probe now if it has not run, so a
 * fault armed afterwards reaches the draw under test, not the probe.
 */
SGIRenderResult sgi_render_test_window_probe(SGIRenderer *renderer,
                                             Error **errp);
SGIRenderResult sgi_render_raster_checked(SGIRenderer *renderer,
                                          const SGIRasterRules *rules,
                                          const SGIRasterTarget *target,
                                          const SGIRasterOp *op, Error **errp);

/* The stored code of window depth z in [0, 1] under the rules. */
uint32_t sgi_raster_depth_code(const SGIRasterRules *rules, double z);
/* After alpha rejection, test and update canonical depth and stencil. */
bool sgi_raster_depth_stencil(const SGIRasterRules *rules,
                               const SGIGLDrawState *st,
                               const SGIRasterTarget *t, int x, int y,
                               double z);
/* Filled polygon bias under the model's slope and depth-step choices. */
double sgi_raster_polygon_offset(const SGIRasterRules *rules,
                                  const SGIGLDrawState *st,
                                  const double window[3][3]);
/* Map normalized depth through the depth range, add offset and clamp. */
bool sgi_raster_window_depth(const SGIGLDrawState *st, double z,
                              double offset, double *window);

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

/*
 * One triangle of a clipped polygon as sgi_soft_draw() rasterizes it, for a
 * renderer that reproduces its pixels: snapped window positions in
 * subpixels, wound counter-clockwise as the scan takes them, with each
 * vertex's normalized depth in [0, 1], 1 / clip w, clamped colour and
 * texture coordinates.
 */
typedef struct SGIRasterTriangle {
    int64_t x[3], y[3];
    double z[3];
    double inv_w[3];
    double rgba[3][4];
    double texcoord[3][4];
} SGIRasterTriangle;

/*
 * Clip the triangle va, vb, vc to the view volume, split it into the fan
 * the scalar path scans, and fill out with the triangles it draws, dropping
 * those it drops: one with a vertex it cannot project, or of zero area.
 * Returns how many. Mipmapped textures, whose level selection needs the
 * scan's gradients, are not described.
 */
unsigned sgi_raster_triangle_pieces(const SGIRasterRules *rules,
                                    const SGIGLDrawState *st,
                                    const float *va, const float *vb,
                                    const float *vc,
                                    SGIRasterTriangle
                                    out[SGI_VOLUME_TRIANGLE_MAX - 2]);

/*
 * Whether sgi_soft_draw() textures the triangle va, vb, vc with the
 * level-zero image: false when any vertex of its clipped polygon has a
 * clip w or texture q that is not positive, or a coordinate the image uses
 * that is not finite, which drops the whole triangle, including fan pieces
 * sgi_raster_triangle_pieces() keeps.
 */
bool sgi_raster_triangle_texture_valid(const SGIGLDrawState *st,
                                       const SGIGLTexImage *image,
                                       const float *va, const float *vb,
                                       const float *vc);

/*
 * One aliased segment as sgi_soft_draw() rasterizes it, for a renderer that
 * reproduces its pixels: the clipped, projected and snapped window
 * endpoints in subpixels, their clamped colours and 1 / clip w, and the
 * inclusive major-axis range of base pixels the width-one segment, shifted
 * for the width, covers. exact says every major step in that range has
 * exactly one base pixel; the base count is then hi - lo + 1.
 */
typedef struct SGIRasterLineSpan {
    int64_t ax, ay, bx, by;
    double a_inv_w, b_inv_w;
    double a_rgba[4], b_rgba[4];
    bool x_major;
    bool covered;               /* false draws and counts nothing */
    bool exact;
    int64_t lo, hi;
} SGIRasterLineSpan;

/*
 * Fill out for the segment from va to vb at aliased width, from the draw
 * state's matrices, viewport and depth range. False when the scalar path
 * drops the segment without counting stipple, as it does for a vertex it
 * cannot project.
 */
bool sgi_raster_line_span(const SGIRasterRules *rules,
                          const SGIGLDrawState *st, const float *va,
                          const float *vb, uint32_t width,
                          SGIRasterLineSpan *out);

/* Borrowed state and image remain immutable until the sampler is freed. */
typedef struct SGIRasterTexture SGIRasterTexture;
typedef enum SGIRasterTextureResult {
    SGI_RASTER_TEXTURE_READY,
    SGI_RASTER_TEXTURE_DISABLED,
    SGI_RASTER_TEXTURE_INCOMPLETE,
    SGI_RASTER_TEXTURE_UNSUPPORTED,
    SGI_RASTER_TEXTURE_OOM,
} SGIRasterTextureResult;
/*
 * Preflight is quiet. Callers that stop before prepare() own the draw's
 * diagnostic for unsupported or incomplete state.
 */
SGIRasterTextureResult sgi_raster_texture_check(
    const SGIGLDrawState *state, const SGIGLTexImage *texture);
SGIRasterTextureResult sgi_raster_texture_prepare(
    const SGIGLDrawState *state, const SGIGLTexImage *texture,
    SGIRasterTexture **out);
/* An allocator argument lets unit tests force the single allocation to fail. */
SGIRasterTextureResult sgi_raster_texture_prepare_with_allocator(
    const SGIGLDrawState *state, const SGIGLTexImage *texture,
    SGIRasterTexture **out, void *(*allocate)(size_t));
void sgi_raster_texture_free(SGIRasterTexture *texture);
/*
 * Stop the sampler's reports, for a caller whose scalar draw of the same
 * fragments makes the draw's reports, so none appears twice.
 */
void sgi_raster_texture_quiet(SGIRasterTexture *texture);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SGIRasterTexture, sgi_raster_texture_free)
bool sgi_raster_texture_fragment(SGIRasterTexture *texture,
                                 const double coord[4], double color[4]);

/* A failed or disabled sample leaves out unchanged. */
bool sgi_gl_texture_apply_pixel(const SGIGLDrawState *state,
                                const SGIGLTexImage *texture,
                                const float coord[4], const float color[4],
                                float out[4]);
/* Carry out a raster op on the target, under the head's rules. */
void sgi_soft_raster(const SGIRasterRules *r, const SGIRasterTarget *t,
                     const SGIRasterOp *op);

#endif
