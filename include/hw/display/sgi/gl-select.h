/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI graphics: the geometry of an OpenGL selection hit.
 *
 * In selection mode (OpenGL 1.2.1, section 5.2) a primitive that is not
 * culled and leaves something inside the view volume after clipping is a
 * hit, at the minimum and maximum window depth of its clipped vertices.
 * The family keeps the name stack and the hit records; this file finds
 * the hits of one draw and the depths each one reaches, in the depth
 * range's units (0 to 1), and reports them in primitive order.
 *
 * Points, lines, triangles, quads, quad strips and polygons are taken;
 * strips and fans of lines and triangles are not, and nor are draws with
 * a window matrix or user clip planes.
 */
#ifndef HW_DISPLAY_SGI_GL_SELECT_H
#define HW_DISPLAY_SGI_GL_SELECT_H

#include "hw/display/sgi/gl-context.h"

/* One hit: the least and greatest window depth the primitive reached. */
typedef void SGIGLSelectHit(void *opaque, double minimum, double maximum);

typedef enum SGIGLSelectResult {
    SGI_GL_SELECT_OK,
    /* The draw's geometry state is not taken: no hit was reported. */
    SGI_GL_SELECT_UNSUPPORTED_STATE,
    /* The draw's primitive mode is not taken: no hit was reported. */
    SGI_GL_SELECT_UNSUPPORTED_MODE,
} SGIGLSelectResult;

/* Report each hit of draw d, in primitive order, to hit(opaque, ...). */
SGIGLSelectResult sgi_gl_select_draw(const SGIGLDraw *d, SGIGLSelectHit *hit,
                                     void *opaque);

#endif
