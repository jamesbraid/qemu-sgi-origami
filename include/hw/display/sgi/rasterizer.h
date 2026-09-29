/* SPDX-License-Identifier: GPL-2.0-or-later */
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
    /* Drop a primitive with a vertex at w <= 0, instead of clipping it. */
    bool drop_nonpositive_w;
    /* Where the values come from, for logs and reports. */
    const char *evidence;
} SGIRasterRules;

/*
 * Whether the rasterizer implements a rule set. It implements subpixel_bits
 * 1 to 12, color_bits 1 to 10, depth_bits 1 to 32, stencil_bits 0 to 16,
 * every rounding, and otherwise only SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
 * SGI_ARITH_DOUBLE, SGI_LINE_COVERAGE_DOUBLE and dropping w <= 0.
 */
bool sgi_raster_rules_supported(const SGIRasterRules *r);

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
    void *opaque;
} SGIRasterTarget;

/*
 * Rasterize a draw into the target, under the draw's state and the head's
 * rules. A rule set sgi_raster_rules_supported() refuses draws nothing.
 */
void sgi_soft_draw(const SGIRasterRules *r, const SGIRasterTarget *t,
                   const SGIGLDraw *d);
/* Carry out a raster op on the target, under the head's rules. */
void sgi_soft_raster(const SGIRasterRules *r, const SGIRasterTarget *t,
                     const SGIRasterOp *op);

#endif
