/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * One OpenGL context's state, in the shape the SGI graphics heads share.
 *
 * A head's family code frames and decodes its own command words and keeps
 * its own table of contexts. What one table entry holds is here: the GL
 * state a draw is made under (SGIGLDrawState), the matrix stacks, the
 * current vertex attributes, the clear values and the primitive being
 * assembled. Nothing here knows which head it serves.
 *
 * Colours, matrices and vertex components are IEEE single floats holding
 * exactly the bits the client sent; arithmetic on them is double.
 */
#ifndef HW_DISPLAY_SGI_GL_CONTEXT_H
#define HW_DISPLAY_SGI_GL_CONTEXT_H

#include "hw/display/sgi/board-memory.h"

/*
 * A raster operation: pixel work on a head's planes that is not a GL
 * primitive, queued beside draws in the order the head received it.
 */

typedef enum SGIRasterKind {
    SGI_RASTER_FILL,
    SGI_RASTER_CLEAR,
    SGI_RASTER_COPY,
    SGI_RASTER_GLYPH,
    SGI_RASTER_LINE,
    SGI_RASTER_POINT,
    SGI_RASTER_BLIT,
    SGI_RASTER_BITMAP,
} SGIRasterKind;

/*
 * Where colour is stored: a value the family's drawable gives and only
 * the family's SGIRasterTarget store hook reads. Shared code copies it and
 * never interprets it.
 */
typedef uint32_t SGIColourTarget;

/* The channels glColorMask lets a draw or clear write (4.2.2). */
#define SGI_GL_COLOUR_MASK_RED   0x1
#define SGI_GL_COLOUR_MASK_GREEN 0x2
#define SGI_GL_COLOUR_MASK_BLUE  0x4
#define SGI_GL_COLOUR_MASK_ALPHA 0x8
#define SGI_GL_COLOUR_MASK_ALL   0xf

/* The buffers a clear names, glClear's mask bits. */
#define SGI_GL_DEPTH_BUFFER_BIT 0x0100
#define SGI_GL_STENCIL_BUFFER_BIT 0x0400
#define SGI_GL_COLOR_BUFFER_BIT 0x4000

struct SGIGLDrawState;

/*
 * BITMAP: a run of glBitmap's image (OpenGL 1.2.1, 3.7), drawn as
 * fragments under a context's draw state. The image is height rows of
 * (width + 7) / 8 bytes, the bottom row first, and within a byte the most
 * significant bit is the leftmost pixel, glPixelStore's defaults with an
 * alignment of one. bytes[0] is byte first of the image, so a family can
 * draw an image as its words arrive. Coordinates are window pixels.
 */
typedef struct SGIRasterBitmap {
    const struct SGIGLDrawState *state;
    /* The window pixel of the image's lower-left corner. */
    int32_t x, y;
    uint32_t width, height;
    uint32_t first;
    uint32_t count;
    const uint8_t *bytes;
    /*
     * The raster position's window depth, colour and colour index, and
     * its eye distance, the fragments' fog coordinate (3.10).
     */
    float z;
    float rgba[4];
    float index;
    float distance;
} SGIRasterBitmap;

/*
 * Only the fields a kind the rasterizer executes needs are here; the
 * others arrive with their kind. Coordinates are plane pixels, the
 * rectangle inclusive.
 */
typedef struct SGIRasterOp {
    uint32_t kind;              /* SGIRasterKind */
    /* The colour targets written, as the drawable names them. */
    SGIColourTarget targets[2];
    uint32_t ntargets;
    int32_t x0, y0, x1, y1;
    /*
     * Only pixels inside these are written; at most the rectangle's. An
     * op needs no bound window, so an empty list here is how a family
     * says that its window has no visible pixel.
     */
    SGIClipList clip;
    /* CLEAR: SGI_GL_COLOR_BUFFER_BIT and SGI_GL_DEPTH_BUFFER_BIT. */
    uint32_t clear_buffers;
    /* The GL clear colour; the rasterizer quantizes it to the plane. */
    float clear_rgba[4];
    /* glColorMask's channels written: SGI_GL_COLOUR_MASK_RED and so on. */
    uint32_t clear_colour_mask;
    /* In the depth plane's units; the plane keeps its low bits. */
    uint32_t clear_depth24;
    /*
     * SGI_GL_STENCIL_BUFFER_BIT: the clear value, written under the
     * stencil write mask; the plane keeps its low bits.
     */
    uint32_t clear_stencil;
    uint32_t stencil_mask;
    /*
     * CLEAR in colour-index mode: nonzero index_bits, and the clear index
     * already masked to them, written under index_mask.
     */
    uint32_t index_bits;
    uint32_t clear_index;
    uint32_t index_mask;
    SGIRasterBitmap bitmap;
} SGIRasterOp;

/*
 * A vertex is 24 floats: position xyzw, colour rgba, normal xyz, the
 * material index as a uint32 bit pattern, texture coordinate strq, fog
 * coordinate, the edge flag as a uint32 bit pattern, the colour index, a
 * pad and the back colour rgba. With lighting enabled the colour is the
 * vertex's lit front colour and the back colour its lit back colour
 * (OpenGL 1.2.1, 2.13.1); a draw says whether it carries back colours.
 */
#define SGI_GL_VERTEX_FLOATS    24
#define SGI_GL_V_POSITION       0
#define SGI_GL_V_COLOR          4
#define SGI_GL_V_NORMAL         8
#define SGI_GL_V_MATERIAL       11
#define SGI_GL_V_TEXCOORD       12
#define SGI_GL_V_FOG            16
#define SGI_GL_V_EDGE           17
#define SGI_GL_V_INDEX          18
#define SGI_GL_V_BACK_COLOR     20

/* OpenGL 1.2 enumerants the state holds. */
enum {
    SGI_GL_POINTS, SGI_GL_LINES, SGI_GL_LINE_LOOP, SGI_GL_LINE_STRIP,
    SGI_GL_TRIANGLES, SGI_GL_TRIANGLE_STRIP, SGI_GL_TRIANGLE_FAN,
    SGI_GL_QUADS, SGI_GL_QUAD_STRIP, SGI_GL_POLYGON,
};
#define SGI_GL_MODELVIEW        0x1700
#define SGI_GL_PROJECTION       0x1701
#define SGI_GL_TEXTURE          0x1702
#define SGI_GL_TEXTURE_2D       0x0de1
#define SGI_GL_TEXTURE_ENV_MODE 0x2200
#define SGI_GL_TEXTURE_ENV_COLOR 0x2201
#define SGI_GL_MODULATE         0x2100
#define SGI_GL_DECAL            0x2101
#define SGI_GL_NEAREST          0x2600
#define SGI_GL_NEAREST_MIPMAP_LINEAR 0x2702
#define SGI_GL_REPEAT           0x2901
#define SGI_GL_CLAMP            0x2900
#define SGI_GL_CLAMP_TO_EDGE    0x812f
#define SGI_GL_CLAMP_TO_BORDER  0x812d
#define SGI_GL_ALPHA            0x1906
#define SGI_GL_RGB              0x1907
#define SGI_GL_RGBA             0x1908
#define SGI_GL_LUMINANCE        0x1909
#define SGI_GL_LUMINANCE_ALPHA  0x190a
#define SGI_GL_INTENSITY        0x8049
#define SGI_GL_FLAT             0x1d00
#define SGI_GL_SMOOTH           0x1d01
/* The comparisons in GL_NEVER's order, as depth_func holds them. */
enum {
    SGI_GL_NEVER, SGI_GL_LESS, SGI_GL_EQUAL, SGI_GL_LEQUAL,
    SGI_GL_GREATER, SGI_GL_NOTEQUAL, SGI_GL_GEQUAL, SGI_GL_ALWAYS,
};
#define SGI_GL_DRAW_FRONT       0x1
#define SGI_GL_DRAW_BACK        0x2
#define SGI_GL_NEVER_ENUM       0x0200
#define SGI_GL_CURRENT_COLOR    0x0b00
#define SGI_GL_CURRENT_INDEX    0x0b01
#define SGI_GL_CURRENT_NORMAL   0x0b02
#define SGI_GL_CURRENT_TEXTURE_COORDS 0x0b03
#define SGI_GL_CURRENT_RASTER_COLOR 0x0b04
#define SGI_GL_CURRENT_RASTER_INDEX 0x0b05
#define SGI_GL_CURRENT_RASTER_TEXTURE_COORDS 0x0b06
#define SGI_GL_CURRENT_RASTER_POSITION 0x0b07
#define SGI_GL_CURRENT_RASTER_POSITION_VALID 0x0b08
#define SGI_GL_CURRENT_RASTER_DISTANCE 0x0b09
#define SGI_GL_POINT_SMOOTH     0x0b10
#define SGI_GL_POINT_SIZE       0x0b11
#define SGI_GL_LINE_SMOOTH      0x0b20
#define SGI_GL_LINE_WIDTH       0x0b21
#define SGI_GL_LINE_STIPPLE     0x0b24
#define SGI_GL_LINE_STIPPLE_PATTERN 0x0b25
#define SGI_GL_LINE_STIPPLE_REPEAT 0x0b26
#define SGI_GL_POLYGON_MODE     0x0b40
#define SGI_GL_POLYGON_SMOOTH   0x0b41
#define SGI_GL_POLYGON_STIPPLE  0x0b42
#define SGI_GL_EDGE_FLAG        0x0b43
#define SGI_GL_CULL_FACE        0x0b44
#define SGI_GL_CULL_FACE_MODE   0x0b45
#define SGI_GL_FRONT_FACE       0x0b46
#define SGI_GL_LIGHTING         0x0b50
#define SGI_GL_LIGHT_MODEL_LOCAL_VIEWER 0x0b51
#define SGI_GL_LIGHT_MODEL_TWO_SIDE 0x0b52
#define SGI_GL_LIGHT_MODEL_AMBIENT 0x0b53
#define SGI_GL_SHADE_MODEL      0x0b54
#define SGI_GL_COLOR_MATERIAL_FACE 0x0b55
#define SGI_GL_COLOR_MATERIAL_PARAMETER 0x0b56
#define SGI_GL_COLOR_MATERIAL   0x0b57
#define SGI_GL_FOG              0x0b60
#define SGI_GL_FOG_INDEX        0x0b61
#define SGI_GL_FOG_DENSITY      0x0b62
#define SGI_GL_FOG_START        0x0b63
#define SGI_GL_FOG_END          0x0b64
#define SGI_GL_FOG_MODE         0x0b65
#define SGI_GL_FOG_COLOR        0x0b66
#define SGI_GL_DEPTH_RANGE      0x0b70
#define SGI_GL_DEPTH_TEST       0x0b71
#define SGI_GL_DEPTH_WRITEMASK  0x0b72
#define SGI_GL_DEPTH_FUNC       0x0b74
#define SGI_GL_STENCIL_TEST     0x0b90
#define SGI_GL_STENCIL_CLEAR_VALUE 0x0b91
#define SGI_GL_STENCIL_FUNC     0x0b92
#define SGI_GL_STENCIL_VALUE_MASK 0x0b93
#define SGI_GL_STENCIL_FAIL     0x0b94
#define SGI_GL_STENCIL_PASS_DEPTH_FAIL 0x0b95
#define SGI_GL_STENCIL_PASS_DEPTH_PASS 0x0b96
#define SGI_GL_STENCIL_REF      0x0b97
#define SGI_GL_STENCIL_WRITEMASK 0x0b98
#define SGI_GL_MATRIX_MODE      0x0ba0
#define SGI_GL_NORMALIZE        0x0ba1
#define SGI_GL_VIEWPORT         0x0ba2
#define SGI_GL_MODELVIEW_STACK_DEPTH 0x0ba3
#define SGI_GL_PROJECTION_STACK_DEPTH 0x0ba4
#define SGI_GL_TEXTURE_STACK_DEPTH 0x0ba5
#define SGI_GL_MODELVIEW_MATRIX 0x0ba6
#define SGI_GL_PROJECTION_MATRIX 0x0ba7
#define SGI_GL_TEXTURE_MATRIX   0x0ba8
#define SGI_GL_ALPHA_TEST       0x0bc0
#define SGI_GL_ALPHA_TEST_FUNC  0x0bc1
#define SGI_GL_ALPHA_TEST_REF   0x0bc2
#define SGI_GL_DITHER           0x0bd0
#define SGI_GL_BLEND_DST        0x0be0
#define SGI_GL_BLEND_SRC        0x0be1
#define SGI_GL_BLEND            0x0be2
#define SGI_GL_LOGIC_OP_MODE    0x0bf0
#define SGI_GL_INDEX_LOGIC_OP   0x0bf1
#define SGI_GL_COLOR_LOGIC_OP   0x0bf2
#define SGI_GL_SCISSOR_BOX      0x0c10
#define SGI_GL_SCISSOR_TEST     0x0c11
#define SGI_GL_INDEX_CLEAR_VALUE 0x0c20
#define SGI_GL_INDEX_WRITEMASK  0x0c21
#define SGI_GL_COLOR_CLEAR_VALUE 0x0c22
#define SGI_GL_COLOR_WRITEMASK  0x0c23
#define SGI_GL_CLIP_PLANE0      0x3000
#define SGI_GL_LIGHT0           0x4000
#define SGI_GL_BLEND_COLOR      0x8005
#define SGI_GL_BLEND_EQUATION   0x8009
#define SGI_GL_RESCALE_NORMAL   0x803a
/* Polygon offset (3.5.5, and EXT_polygon_offset's bias). */
#define SGI_GL_POLYGON_OFFSET_UNITS 0x2a00
#define SGI_GL_POLYGON_OFFSET_POINT 0x2a01
#define SGI_GL_POLYGON_OFFSET_LINE 0x2a02
#define SGI_GL_POLYGON_OFFSET_FILL 0x8037
#define SGI_GL_POLYGON_OFFSET_FACTOR 0x8038
#define SGI_GL_POLYGON_OFFSET_BIAS 0x8039
/* The polygon_offset bits: the enable of each polygon mode's offset. */
#define SGI_GL_OFFSET_FILL      0x1
#define SGI_GL_OFFSET_LINE      0x2
#define SGI_GL_OFFSET_POINT     0x4

/*
 * glBlendFunc's factors (4.1.6, and the imaging subset's constant
 * factors) and glBlendEquation's equations. GL_ZERO is 0 and GL_ONE 1;
 * GL_LOGIC_OP is EXT_blend_logic_op's equation.
 */
#define SGI_GL_SRC_COLOR        0x0300
#define SGI_GL_ONE_MINUS_SRC_COLOR 0x0301
#define SGI_GL_SRC_ALPHA        0x0302
#define SGI_GL_ONE_MINUS_SRC_ALPHA 0x0303
#define SGI_GL_DST_ALPHA        0x0304
#define SGI_GL_ONE_MINUS_DST_ALPHA 0x0305
#define SGI_GL_DST_COLOR        0x0306
#define SGI_GL_ONE_MINUS_DST_COLOR 0x0307
#define SGI_GL_SRC_ALPHA_SATURATE 0x0308
#define SGI_GL_CONSTANT_COLOR   0x8001
#define SGI_GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define SGI_GL_CONSTANT_ALPHA   0x8003
#define SGI_GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#define SGI_GL_FUNC_ADD         0x8006
#define SGI_GL_MIN              0x8007
#define SGI_GL_MAX              0x8008
#define SGI_GL_FUNC_SUBTRACT    0x800a
#define SGI_GL_FUNC_REVERSE_SUBTRACT 0x800b
#define SGI_GL_LOGIC_OP         0x0bf1
/* glLogicOp's operations run from GL_CLEAR to GL_SET (4.1.8). */
#define SGI_GL_CLEAR            0x1500
#define SGI_GL_COPY             0x1503
#define SGI_GL_SET              0x150f

/* Faces, and the light and material parameters (OpenGL 1.2.1, 2.13.2). */
#define SGI_GL_FRONT            0x0404
#define SGI_GL_BACK             0x0405
#define SGI_GL_FRONT_AND_BACK   0x0408
/* glPolygonMode's modes (3.5.4). */
#define SGI_GL_POINT            0x1b00
#define SGI_GL_LINE             0x1b01
#define SGI_GL_FILL             0x1b02
/*
 * The rasterization enables a context keeps but does not draw with, as
 * bits of raster_enables.
 */
#define SGI_GL_RASTER_POINT_SMOOTH 0x01
#define SGI_GL_RASTER_LINE_SMOOTH 0x02
#define SGI_GL_RASTER_LINE_STIPPLE 0x04
#define SGI_GL_RASTER_POLYGON_SMOOTH 0x08
#define SGI_GL_RASTER_POLYGON_STIPPLE 0x10
/* glFog's modes (3.10). */
#define SGI_GL_EXP              0x0800
#define SGI_GL_EXP2             0x0801
#define SGI_GL_LINEAR           0x2601
/* glStencilOp's operations (4.1.5); GL_ZERO is 0. */
#define SGI_GL_INVERT           0x150a
#define SGI_GL_KEEP             0x1e00
#define SGI_GL_REPLACE          0x1e01
#define SGI_GL_INCR             0x1e02
#define SGI_GL_DECR             0x1e03
/* glFrontFace's directions (2.13.1). */
#define SGI_GL_CW               0x0900
#define SGI_GL_CCW              0x0901
#define SGI_GL_AMBIENT          0x1200
#define SGI_GL_DIFFUSE          0x1201
#define SGI_GL_SPECULAR         0x1202
#define SGI_GL_POSITION         0x1203
#define SGI_GL_SPOT_DIRECTION   0x1204
#define SGI_GL_SPOT_EXPONENT    0x1205
#define SGI_GL_SPOT_CUTOFF      0x1206
#define SGI_GL_CONSTANT_ATTENUATION 0x1207
#define SGI_GL_LINEAR_ATTENUATION 0x1208
#define SGI_GL_QUADRATIC_ATTENUATION 0x1209
#define SGI_GL_EMISSION         0x1600
#define SGI_GL_SHININESS        0x1601
#define SGI_GL_AMBIENT_AND_DIFFUSE 0x1602
#define SGI_GL_COLOR_INDEXES    0x1603

/* The client clip planes a context keeps, GL_CLIP_PLANE0 to 5 (2.11). */
#define SGI_GL_CLIP_PLANES      6

/* The lights a context keeps, GL_LIGHT0 to GL_LIGHT7. */
#define SGI_GL_LIGHTS           8

/*
 * One light's parameters (2.13.1, table 2.10): position and spot
 * direction in eye coordinates, as the modelview current when they were
 * set left them; attenuation constant, linear and quadratic.
 * spot_cutoff is in degrees, as glGetLightfv answers it; the lighting
 * equation compares against spot_cos_cutoff, its cosine, which is what a
 * family setting the light with sgi_gl_set_light() gives. Each is exact
 * for one setter and neither converts to the other without loss, so both
 * are kept. A family whose lights come only from glLightfv may migrate
 * spot_cutoff alone and recompute the cosine after loading with
 * sgi_gl_spot_cos(), the expression glLightfv itself uses; one that uses
 * sgi_gl_set_light() must migrate spot_cos_cutoff, or rebuild it from its
 * own migrated state.
 */
typedef struct SGIGLLight {
    float ambient[4];
    float diffuse[4];
    float specular[4];
    float position[4];
    float spot_direction[3];
    float spot_exponent;
    float spot_cutoff;
    float attenuation[3];
    float spot_cos_cutoff;
} SGIGLLight;

/*
 * A light as a family's board delivers it, already decoded to eye
 * coordinates: for sgi_gl_set_light(). position has w 0 for a light at
 * infinity. spot_cos_cutoff is the cosine of the spot cutoff; -1 or less
 * is a cutoff of 180, no spot. attenuation is constant, linear and
 * quadratic.
 */
typedef struct SGIGLEyeLight {
    float position[4];
    float spot_direction[3];
    float spot_cos_cutoff;
    float spot_exponent;
    float attenuation[3];
    float ambient[4];
    float diffuse[4];
    float specular[4];
} SGIGLEyeLight;

/* One face's material (2.13.1, table 2.10). */
typedef struct SGIGLMaterial {
    float ambient[4];
    float diffuse[4];
    float specular[4];
    float emission[4];
    float shininess;
    float color_indexes[3];     /* ambient, diffuse, specular */
} SGIGLMaterial;

/* Matrices a GL matrix stack holds, the current one included. */
#define SGI_GL_STACK_DEPTH      32

/*
 * The most vertices a context holds between pieces of a primitive: a quad
 * is drawn once its fourth arrives, so three wait.
 */
#define SGI_GL_HELD_VERTICES    3

/*
 * Where a context draws, as the family's context switch, window clip and
 * buffer selection give it. Coordinates are plane pixels.
 */
typedef struct SGIGLDrawable {
    /*
     * 0 while the family has no window for the context: nothing draws,
     * and each draw, clear or bitmap is logged as dropped.
     */
    uint32_t bound;             /* 0 or 1 */
    /*
     * The window's lower-left pixel, where window coordinates start; its
     * top-left under a window matrix whose rows run down the display.
     */
    int32_t origin[2];
    /*
     * The pixels the window owns. A bound window with no rectangle is one
     * other windows wholly cover, a normal state: nothing draws and
     * nothing is logged. The list's empty flag plays no part here.
     */
    SGIClipList clip;
    /* The colour targets glDrawBuffer's front and back buffers name. */
    SGIColourTarget front;
    SGIColourTarget back;
} SGIGLDrawable;

/* The GL state a draw is made under. */
typedef struct SGIGLDrawState {
    /* Column-major, as glLoadMatrixf lays a matrix out. */
    float modelview[16];
    float projection[16];
    int32_t viewport[4];
    uint32_t shade_model;       /* GL_FLAT or GL_SMOOTH */
    uint32_t depth_test;        /* 0 or 1 */
    uint32_t depth_func;        /* the comparison minus GL_NEVER, 0 to 7 */
    uint32_t depth_mask;        /* 0 or 1 */
    /*
     * glDepthRange's near and far, each clamped to [0, 1] (2.10.1), which
     * map normalized depth to window depth.
     */
    float depth_range[2];
    /* SGI_GL_DRAW_FRONT and SGI_GL_DRAW_BACK; other bits are ignored. */
    uint32_t draw_buffers;
    /*
     * 0 in RGBA mode; in colour-index mode the bits a colour index holds,
     * which the context's visual fixes (sgi_gl_set_index_mode()).
     */
    uint32_t index_bits;
    /* glIndexMask's write mask (4.2.2). */
    uint32_t index_mask;
    /* glColorMask's channels, SGI_GL_COLOUR_MASK_RED and so on (4.2.2). */
    uint32_t colour_mask;
    /*
     * Polygon culling (3.5.1): GL_CULL_FACE's enable, glCullFace's
     * GL_FRONT, GL_BACK or GL_FRONT_AND_BACK, and glFrontFace's GL_CW or
     * GL_CCW, which also chooses the colour two-sided lighting gives a
     * polygon (2.13.1). Any front_face but GL_CW is GL_CCW.
     */
    uint32_t cull_face;         /* 0 or 1 */
    uint32_t cull_face_mode;
    uint32_t front_face;
    /*
     * The scissor test (4.1.2): its enable and glScissor's box, x, y,
     * width and height in window coordinates.
     */
    uint32_t scissor_test;      /* 0 or 1 */
    int32_t scissor[4];
    /*
     * The alpha test (4.1.4): its enable, glAlphaFunc's comparison minus
     * GL_NEVER, 0 to 7, and its reference value clamped to [0, 1].
     */
    uint32_t alpha_test;        /* 0 or 1 */
    uint32_t alpha_func;
    float alpha_ref;
    /*
     * The stencil test (4.1.5): its enable, glStencilFunc's comparison
     * minus GL_NEVER, reference and value mask, glStencilMask's write
     * mask, and glStencilOp's operations on failing the stencil test, on
     * failing the depth test and on passing both, as their enumerants.
     * The reference is kept as sent and clamped to the stencil planes'
     * range where it is used.
     */
    uint32_t stencil_test;      /* 0 or 1 */
    uint32_t stencil_func;
    int32_t stencil_ref;
    uint32_t stencil_value_mask;
    uint32_t stencil_write_mask;
    uint32_t stencil_op[3];
    /*
     * Blending (4.1.6): its enable, glBlendFunc's source and destination
     * factors, glBlendEquation's equation, and glBlendColor's constant
     * colour clamped to [0, 1].
     */
    uint32_t blend;             /* 0 or 1 */
    uint32_t blend_src;
    uint32_t blend_dst;
    uint32_t blend_equation;
    float blend_color[4];
    /*
     * The logical operation (4.1.8): the GL_INDEX_LOGIC_OP and
     * GL_COLOR_LOGIC_OP enables and glLogicOp's operation.
     */
    uint32_t index_logic_op;    /* 0 or 1 */
    uint32_t color_logic_op;    /* 0 or 1 */
    uint32_t logic_op_mode;
    /*
     * Fog (3.10): its enable, glFog's mode, density, start and end, the
     * fog colour clamped to [0, 1] and the fog index.
     */
    uint32_t fog;               /* 0 or 1 */
    uint32_t fog_mode;
    float fog_density;
    float fog_start;
    float fog_end;
    float fog_color[4];
    float fog_index;
    /*
     * Polygon offset (3.5.5): the GL_POLYGON_OFFSET_FILL, _LINE and
     * _POINT enables as SGI_GL_OFFSET_FILL and so on, the factor, and the
     * constant term, either glPolygonOffset's units of the smallest
     * resolvable depth difference or EXT_polygon_offset's bias in window
     * depth; the command last given sets its own and zeroes the other.
     */
    uint32_t polygon_offset;
    float offset_factor;
    float offset_units;
    float offset_bias;
    /*
     * Client clip planes (2.11): bit i enables GL_CLIP_PLANEi, and each
     * plane is kept in eye coordinates, as glClipPlane's modelview left
     * it.
     */
    uint32_t clip_planes;
    float clip_plane[SGI_GL_CLIP_PLANES][4];
    SGIGLDrawable drawable;
    /*
     * GL_DITHER's enable, initially on (4.1.7). A draw is dithered while
     * it is on and the head's rules name a dither; clears never are.
     */
    uint32_t dither;            /* 0 or 1 */
    /*
     * A window matrix, for a family whose board takes one eye-to-window
     * transform in place of a projection and a viewport; none of it is GL
     * state (sgi_gl_set_window_matrix()). While window_matrix_given is 1,
     * an eye position e goes to h = W e by window_matrix W, column-major,
     * and h replaces the projection, the divide and the viewport map
     * (2.10): window x and y are h0 / h3 and h1 / h3, unrounded, in pixels
     * from the drawable's origin, and window depth is
     * window_depth_scale * h2 / h3, which the depth range does not map
     * again. With window_rows_down 1 window y counts rows down the display
     * from the drawable's origin, which is then the window's top-left
     * pixel; facing (2.13.1, 3.5.1) is still judged as the client sees it.
     * Clipping keeps OpenGL's near and far planes, window depth 0 and 1,
     * and bounds x and y by SGI_GL_WINDOW_GUARD instead of the viewport.
     *
     * Where this departs from OpenGL, for want of a viewport or because
     * rows run down:
     * - glRasterPos is valid anywhere inside the guard band, where OpenGL
     *   makes a raster position outside the viewport invalid (2.12), so a
     *   bitmap starting just outside the window draws its visible part;
     * - a bitmap's rows still step up window y, down the display under
     *   window_rows_down;
     * - the tie rule names edges in window rows as they run, so under
     *   window_rows_down SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN owns the bottom
     *   and left edges on the display;
     * - the scissor box is tested in window rows as they run, so under
     *   window_rows_down it is measured from the window's top, and the
     *   family gives it that way.
     */
    uint32_t window_matrix_given;       /* 0 or 1 */
    float window_matrix[16];
    float window_depth_scale;
    uint32_t window_rows_down;          /* 0 or 1 */
    /* The texture environment belongs to the context, not its objects. */
    uint32_t texture_2d;
    uint32_t texture_env_mode;
    float texture_env_color[4];
} SGIGLDrawState;

/*
 * Under a window matrix, the window pixels about the drawable's origin,
 * in each direction, that a primitive is clipped to in place of the
 * viewport's sides: more than any head's window, so nothing a window
 * owns is cut, and within the rasterizer's reach.
 */
#define SGI_GL_WINDOW_GUARD     16384

/* Host-owned deferred boundary. Never shallow-copy a live context. */
typedef struct SGIGLPolygonAssembly {
    float *vertices;
    uint32_t count, capacity;
    uint32_t failed, error;
    /* Host activation and transient migration marker, never serialized. */
    uint32_t enabled, loaded;
} SGIGLPolygonAssembly;

#define SGI_GL_OUT_OF_MEMORY 0x0505

typedef enum SGIGLDrawResult {
    SGI_GL_DRAW_OK,
    SGI_GL_DRAW_OUT_OF_MEMORY,
} SGIGLDrawResult;

typedef struct SGIGLContext {
    /* The hardware identity the family keys this context by. */
    uint32_t hw_id;
    SGIGLDrawState cur;
    /* Matrices glPushMatrix saved under the current ones, oldest first. */
    float mv_stack[SGI_GL_STACK_DEPTH - 1][16];
    float pr_stack[SGI_GL_STACK_DEPTH - 1][16];
    uint32_t mv_depth;
    uint32_t pr_depth;
    /*
     * The texture matrix and the ones glPushMatrix saved under it (2.10.2),
     * applied to each vertex before clipping.
     */
    float texture[16];
    float tx_stack[SGI_GL_STACK_DEPTH - 1][16];
    uint32_t tx_depth;
    /* GL_MODELVIEW, GL_PROJECTION, GL_TEXTURE or other */
    uint32_t matrix_mode;
    /*
     * The lighting state glLight, glLightModel, glMaterial and
     * glColorMaterial set, which lights each vertex while GL_LIGHTING is
     * enabled. material[0] is the front face's, [1] the back's.
     */
    SGIGLLight light[SGI_GL_LIGHTS];
    SGIGLMaterial material[2];
    float light_model_ambient[4];
    uint32_t light_model_local_viewer;  /* 0 or 1 */
    uint32_t light_model_two_side;      /* 0 or 1 */
    uint32_t color_material;            /* GL_COLOR_MATERIAL's enable */
    uint32_t color_material_face;       /* GL_FRONT, GL_BACK or both */
    uint32_t color_material_mode;       /* a material parameter */
    uint32_t lighting;                  /* GL_LIGHTING's enable */
    uint32_t lights_enabled;            /* bit i: GL_LIGHTi's enable */
    uint32_t normalize;                 /* GL_NORMALIZE's enable */
    uint32_t rescale_normal;            /* GL_RESCALE_NORMAL's enable */
    /*
     * A family whose board sends its own normal matrix: while
     * normal_matrix_given is 1, normals go to eye coordinates by
     * normal_matrix, column-major 3x3, exactly as given, in place of the
     * modelview's inverse transpose and GL_RESCALE_NORMAL's rescale.
     * normalize_always makes every eye normal unit length, as
     * GL_NORMALIZE does, whatever GL_NORMALIZE's enable. None of the three
     * is GL state; sgi_gl_init() clears them.
     */
    uint32_t normal_matrix_given;       /* 0 or 1 */
    float normal_matrix[9];
    uint32_t normalize_always;          /* 0 or 1 */
    float cur_color[4];
    float cur_index;
    float cur_normal[3];
    uint32_t cur_edge;          /* 0 or 1 */
    /*
     * The current raster position (2.12): window x, y and z and clip w,
     * whether it is valid, its eye distance and its colour.
     */
    float raster_pos[4];
    uint32_t raster_valid;      /* 0 or 1 */
    float raster_distance;
    float raster_color[4];
    float raster_index;
    float clear_color[4];
    float clear_index;
    /*
     * In the depth plane's units, as the family delivered it; a clear
     * keeps the low bits the plane holds, so all ones is 1.0 at any
     * depth.
     */
    uint32_t clear_depth;
    uint32_t clear_stencil;     /* glClearStencil's value */
    /*
     * Aliased sizes and retained polygon modes (3.3 to 3.5).
     * Smoothing and line stipple remain unimplemented.
     */
    float point_size;
    float line_width;
    uint32_t line_stipple_repeat;
    uint32_t line_stipple_pattern;
    uint32_t polygon_mode[2];
    uint32_t raster_enables;
    /* Canonical window rows: bit x of row y (OpenGL 1.2.1, 3.5.2). */
    uint32_t polygon_stipple[32];
    SGIGLPolygonAssembly polygon;
    /* The primitive open since glBegin, SGI_GL_POINTS to SGI_GL_POLYGON. */
    uint32_t begin_mode;
    uint32_t in_begin;          /* 0 or 1 */
    /* Vertices waiting for the rest of their piece, nheld of them. */
    float held[SGI_GL_HELD_VERTICES][SGI_GL_VERTEX_FLOATS];
    uint32_t nheld;
    /*
     * 1 while the next triangle of an open strip is an odd one, whose
     * first two vertices are sent swapped so that every triangle of the
     * strip keeps the strip's orientation (2.6.1), which facing needs.
     */
    uint32_t strip_odd;
    float cur_texcoord[4];
    float raster_texcoord[4];
} SGIGLContext;

/* The GL entry points a family decodes to, one per command. */
typedef enum SGIGLOp {
    SGI_GL_OP_VIEWPORT,         /* x, y, width, height as int32 */
    SGI_GL_OP_CLEAR_COLOR,      /* r, g, b, a */
    SGI_GL_OP_CLEAR_DEPTH,      /* in the depth plane's units */
    SGI_GL_OP_SHADE_MODEL,      /* GL_FLAT or GL_SMOOTH */
    SGI_GL_OP_MATRIX_MODE,      /* the mode's enumerant */
    SGI_GL_OP_LOAD_MATRIX,      /* 16, column-major */
    SGI_GL_OP_LOAD_IDENTITY,
    SGI_GL_OP_MULT_MATRIX,      /* 16, column-major */
    SGI_GL_OP_FRUSTUM,          /* left, right, bottom, top, near, far */
    SGI_GL_OP_ORTHO,            /* left, right, bottom, top, near, far */
    SGI_GL_OP_PUSH_MATRIX,
    SGI_GL_OP_POP_MATRIX,
    SGI_GL_OP_ROTATE,           /* angle in degrees, x, y, z */
    SGI_GL_OP_SCALE,            /* x, y, z */
    SGI_GL_OP_TRANSLATE,        /* x, y, z */
    SGI_GL_OP_ENABLE,           /* the capability's enumerant */
    SGI_GL_OP_DISABLE,          /* the capability's enumerant */
    SGI_GL_OP_DEPTH_FUNC,       /* GL_NEVER to GL_ALWAYS */
    SGI_GL_OP_DEPTH_MASK,       /* 0 or not */
    SGI_GL_OP_DRAW_BUFFERS,     /* SGI_GL_DRAW_FRONT and SGI_GL_DRAW_BACK */
    SGI_GL_OP_COLOR3F,          /* r, g, b */
    SGI_GL_OP_COLOR4F,          /* r, g, b, a */
    SGI_GL_OP_COLOR3UB,         /* r, g, b, each 0 to 255 */
    SGI_GL_OP_COLOR4UB,         /* r, g, b, a, each 0 to 255 */
    SGI_GL_OP_NORMAL3F,         /* x, y, z */
    SGI_GL_OP_EDGE_FLAG,        /* 0 or not */
    SGI_GL_OP_BEGIN,            /* SGI_GL_POINTS to SGI_GL_POLYGON */
    SGI_GL_OP_END,
    SGI_GL_OP_VERTEX2F,         /* x, y */
    SGI_GL_OP_VERTEX3F,         /* x, y, z */
    SGI_GL_OP_VERTEX4F,         /* x, y, z, w */
    SGI_GL_OP_VERTEX2I,         /* x, y as int32 */
    SGI_GL_OP_FLUSH,
    SGI_GL_OP_CLEAR,            /* SGI_GL_COLOR_BUFFER_BIT and so on */
    SGI_GL_OP_FINISH,
    SGI_GL_OP_RASTER_POS2F,     /* x, y */
    SGI_GL_OP_RASTER_POS3F,     /* x, y, z */
    SGI_GL_OP_RASTER_POS4F,     /* x, y, z, w */
    SGI_GL_OP_INDEXF,           /* the colour index */
    SGI_GL_OP_INDEX_MASK,       /* the write mask */
    SGI_GL_OP_CLEAR_INDEX,      /* the clear index */
    SGI_GL_OP_DEPTH_RANGE,      /* near, far */
    /*
     * glLightfv: the light's number, 0 for GL_LIGHT0, the parameter and
     * four floats, of which the parameter uses as many as it takes.
     */
    SGI_GL_OP_LIGHT,
    /* glMaterialfv: the face, the parameter and four floats, likewise. */
    SGI_GL_OP_MATERIAL,
    /*
     * glLightModelfv: the parameter and four floats; a boolean parameter
     * is true for a nonzero first float.
     */
    SGI_GL_OP_LIGHT_MODEL,
    SGI_GL_OP_COLOR_MATERIAL,   /* face, parameter */
    SGI_GL_OP_CULL_FACE,        /* GL_FRONT, GL_BACK or GL_FRONT_AND_BACK */
    SGI_GL_OP_FRONT_FACE,       /* GL_CW or GL_CCW */
    SGI_GL_OP_SCISSOR,          /* x, y, width, height as int32 */
    SGI_GL_OP_ALPHA_FUNC,       /* GL_NEVER to GL_ALWAYS, reference */
    /* GL_NEVER to GL_ALWAYS, the reference as int32, the value mask */
    SGI_GL_OP_STENCIL_FUNC,
    SGI_GL_OP_STENCIL_OP,       /* fail, depth fail and depth pass */
    SGI_GL_OP_STENCIL_MASK,     /* the write mask */
    SGI_GL_OP_CLEAR_STENCIL,    /* the clear value */
    SGI_GL_OP_BLEND_FUNC,       /* source and destination factors */
    SGI_GL_OP_BLEND_EQUATION,   /* the equation */
    SGI_GL_OP_BLEND_COLOR,      /* r, g, b, a */
    SGI_GL_OP_LOGIC_OP,         /* GL_CLEAR to GL_SET */
    SGI_GL_OP_COLOR_MASK,       /* r, g, b, a, each 0 or not */
    /*
     * glFogfv: the parameter and four floats, of which it uses as many as
     * it takes; GL_FOG_MODE's is the mode's enumerant as a float.
     */
    SGI_GL_OP_FOG,
    SGI_GL_OP_POLYGON_OFFSET,   /* factor, units */
    SGI_GL_OP_POLYGON_OFFSET_EXT, /* factor, bias */
    SGI_GL_OP_CLIP_PLANE,       /* plane number, a, b, c, d */
    SGI_GL_OP_POINT_SIZE,       /* the size */
    SGI_GL_OP_LINE_WIDTH,       /* the width */
    SGI_GL_OP_LINE_STIPPLE,     /* the repeat as int32, the pattern */
    SGI_GL_OP_POLYGON_MODE,     /* face, mode */
    SGI_GL_OP_TEXCOORD2F,       /* s, t */
    SGI_GL_OP_TEXCOORD3F,       /* s, t, r */
    SGI_GL_OP_TEXCOORD4F,       /* s, t, r, q */
    /* Parameter and four floats; mode's enumerant as a float. */
    SGI_GL_OP_TEX_ENV,
} SGIGLOp;

/*
 * A decoded command: its entry point and its scalar arguments as 32-bit
 * words in the specification's order, a float argument as the IEEE
 * single bits the client sent.
 */
typedef struct SGIGLCmd {
    uint16_t op;
    uint16_t nargs;
    const uint32_t *args;
} SGIGLCmd;

/*
 * A batch of vertices drawn as one primitive mode under one draw state.
 * The context lights each vertex before it is drawn, so no draw carries
 * materials: a vertex's material index is 0, the draw state's.
 */
typedef struct SGIGLDraw {
    uint32_t context;           /* the context's hw_id */
    uint32_t mode;              /* SGI_GL_POINTS to SGI_GL_POLYGON */
    uint32_t count;
    /* Includes texture-matrix-transformed strq, ready for clipping. */
    const float *vertices;      /* count * SGI_GL_VERTEX_FLOATS */
    const float *materials;
    uint32_t nmaterials;
    /*
     * True when the vertices carry a back colour that a back-facing
     * polygon takes instead of the colour: two-sided lighting (2.13.1).
     * Points and lines always take the colour.
     */
    bool back_colors;
    const SGIGLDrawState *state;
    /* Borrowed dimensions; zero in direct submissions means one. */
    float point_size;
    float line_width;
    /* Borrowed canonical rows; absent rows mean stipple is disabled. */
    const uint32_t *polygon_stipple;
    bool polygon_stipple_enabled;
    /* Absent modes preserve legacy FILL; borrowed through synchronous draw. */
    const uint32_t *polygon_modes;
    bool fill_by_triangle;
    bool line_stipple_enabled;
    uint32_t line_stipple_repeat, line_stipple_pattern;
} SGIGLDraw;

/*
 * Where a context's work goes. Every item is executed before the call
 * returns, so what it points at need only live that long.
 */
typedef struct SGIGLSink {
    void (*draw)(void *opaque, const SGIGLDraw *d);
    void (*raster)(void *opaque, const SGIRasterOp *op);
    /*
     * glFinish's fence, once everything sent before it is done; NULL
     * when the family has nothing to report for it.
     */
    void (*fence)(void *opaque, uint32_t context);
    void *opaque;
    /* Activated contexts require this result path and never use draw. */
    SGIGLDrawResult (*draw_checked)(void *opaque, const SGIGLDraw *d);
} SGIGLSink;

/* OpenGL 1.2.1's initial state, for the context with identity hw_id. */
void sgi_gl_init(SGIGLContext *c, uint32_t hw_id);
/* Unavailable until renderer coverage is complete; reset preserves policy. */
bool sgi_gl_enable_polygon_modes(SGIGLContext *c, const SGIGLSink *sink);
void sgi_gl_cleanup(SGIGLContext *c);
void sgi_gl_reset(SGIGLContext *c, uint32_t hw_id);
/* Both contexts initialized and distinct; failure leaves both unchanged. */
bool sgi_gl_clone(SGIGLContext *dst, const SGIGLContext *src);
bool sgi_gl_consume_error(SGIGLContext *c, uint32_t *error);

/*
 * Canonical 32x32 pattern, copied on upload. Bit x in row y names window
 * (x, y). These public-command helpers reject use inside Begin/End.
 * Device transport and public BITMAP unpacking belong to the caller.
 */
bool sgi_gl_polygon_stipple(SGIGLContext *c, const uint32_t rows[32]);
bool sgi_gl_get_polygon_stipple(const SGIGLContext *c, uint32_t rows[32]);

/*
 * Point the context at a drawable. The family calls it at each context
 * switch and whenever its window or buffer selection changes; sgi_gl_init()
 * leaves the context unbound.
 */
void sgi_gl_set_drawable(SGIGLContext *c, const SGIGLDrawable *d);
/*
 * Load m, column-major, as the current matrix of the stack slot names:
 * SGI_GL_MODELVIEW, SGI_GL_PROJECTION or SGI_GL_TEXTURE, whatever the
 * context's matrix mode. For a family whose matrices arrive as a target
 * select and a load rather than as glMatrixMode and glLoadMatrixf. False,
 * and logged, for a slot the context does not keep.
 */
bool sgi_gl_set_matrix(SGIGLContext *c, uint32_t slot, const float m[16]);
/*
 * The colour targets the draw state's draw buffers select, front before
 * back and each once, into out; returns how many.
 */
uint32_t sgi_gl_draw_targets(const SGIGLDrawState *st,
                             SGIColourTarget out[2]);
/*
 * Put the context in colour-index mode with index_bits bits a colour
 * index, or in RGBA mode for 0: a property of the context's visual, which
 * the family learns its own way, not a GL command.
 */
void sgi_gl_set_index_mode(SGIGLContext *c, uint32_t index_bits);
/*
 * A colour index as a colour-index buffer of index_bits bits stores it
 * (OpenGL 1.2.1, 2.13.9): the integer part, masked to the buffer's bits.
 */
uint32_t sgi_gl_index_value(uint32_t index_bits, double index);

/* Where glBitmap's image falls, from sgi_gl_bitmap_place(). */
typedef struct SGIGLBitmap {
    int32_t x, y;               /* window pixel of the lower-left corner */
    uint32_t width, height;
} SGIGLBitmap;

/*
 * glBitmap's placement and move (OpenGL 1.2.1, 3.7). With the current
 * raster position valid, the image's lower-left corner falls at window
 * pixel (floor(xr - xorig), floor(yr - yorig)), which goes into *b, and
 * the raster position then moves by (xmove, ymove); returns true. With it
 * invalid nothing is drawn or moved; returns false. The image itself
 * follows through sgi_gl_bitmap_bytes(), whole or in runs.
 */
bool sgi_gl_bitmap_place(SGIGLContext *c, uint32_t width, uint32_t height,
                         float xorig, float yorig, float xmove, float ymove,
                         SGIGLBitmap *b);
/*
 * Draw count bytes of the image b placed, from its byte first on, in the
 * raster position's colour or colour index and depth.
 */
void sgi_gl_bitmap_bytes(SGIGLContext *c, const SGIGLSink *sink,
                         const SGIGLBitmap *b, uint32_t first,
                         const uint8_t *bytes, uint32_t count);
/*
 * The cosine glLightfv stores for a spot cutoff of degrees: -1 for 180,
 * no spot, and otherwise the single-precision cosine.
 */
float sgi_gl_spot_cos(float degrees);
/*
 * Set light number light (0 for GL_LIGHT0) from values a family has
 * decoded from its board's own encoding, already in eye coordinates: no
 * modelview is applied, unlike glLightfv. glGetLightfv then answers the
 * cutoff in degrees. False, and logged, for a light the context does not
 * keep. The light's enable is GL_LIGHTi's, unchanged.
 */
bool sgi_gl_set_light(SGIGLContext *c, uint32_t light,
                      const SGIGLEyeLight *l);
/*
 * Take normals to eye coordinates by m, column-major 3x3, used as given
 * with no inverse or transpose and no rescale; NULL returns to the
 * modelview's inverse transpose (2.10.3).
 */
void sgi_gl_set_normal_matrix(SGIGLContext *c, const float m[9]);
/*
 * An eye position's clip coordinates (2.10) under the draw state: by the
 * projection, or under a window matrix the window matrix's h with x and y
 * scaled by 1 / SGI_GL_WINDOW_GUARD and z by 2 window_depth_scale, less
 * w, so that OpenGL's view volume, -w <= x, y, z <= w, is the guard band
 * and window depth 0 to 1. Either is clipped by sgi_volume_clip_*().
 */
void sgi_gl_eye_to_clip(const SGIGLDrawState *st, const double eye[4],
                        double clip[4]);
/*
 * Clip coordinates as sgi_gl_eye_to_clip() gives them to window x and y
 * and normalized depth, (z / w + 1) / 2, in out: by the divide and the
 * viewport, or under a window matrix the window matrix's own window
 * position, unrounded, and its window depth. w must be positive.
 */
void sgi_gl_clip_to_window(const SGIGLDrawState *st, const double clip[4],
                           double out[3]);
/*
 * Draw through the window matrix m, column-major, with depth scale
 * depth_scale and rows running down when rows_down, as the draw state
 * describes them; NULL returns to the projection and viewport, and clears
 * all three. The same state goes to glRasterPos.
 */
void sgi_gl_set_window_matrix(SGIGLContext *c, const float m[16],
                              float depth_scale, bool rows_down);
/* Make every eye normal unit length, whatever GL_NORMALIZE's enable. */
void sgi_gl_set_normalize_always(SGIGLContext *c, bool on);
/*
 * GL_LIGHTING's enable and GL_LIGHT_MODEL_LOCAL_VIEWER, for a family whose
 * board sets them in a state word rather than by glEnable and
 * glLightModel; the same state those commands set.
 */
void sgi_gl_set_lighting(SGIGLContext *c, bool on);
void sgi_gl_set_local_viewer(SGIGLContext *c, bool on);
/* Execute one command against the context, sending any work to sink. */
void sgi_gl_command(SGIGLContext *c, const SGIGLSink *sink,
                    const SGIGLCmd *cmd);
/*
 * glGetLightfv of light number light (0 for GL_LIGHT0) and
 * glGetMaterialfv of GL_FRONT or GL_BACK: the answer as *n float words
 * into out. False, with nothing written, for a light, face or parameter
 * the context does not keep.
 */
bool sgi_gl_get_light(const SGIGLContext *c, uint32_t light, uint32_t pname,
                      uint32_t *out, unsigned *n);
bool sgi_gl_get_material(const SGIGLContext *c, uint32_t face,
                         uint32_t pname, uint32_t *out, unsigned *n);
/*
 * glGetClipPlane of plane number plane (0 for GL_CLIP_PLANE0): its four
 * coefficients in eye coordinates as float words into out. False for a
 * plane the context does not keep.
 */
bool sgi_gl_get_clip_plane(const SGIGLContext *c, uint32_t plane,
                           uint32_t out[4]);
/* The most words one sgi_gl_get() answer takes: a matrix's. */
#define SGI_GL_GET_WORDS        16

/*
 * The glGet answer for pname as *n 32-bit words into out, which holds
 * SGI_GL_GET_WORDS; floats as their bits, enumerants and integers as
 * themselves, a matrix column-major. False for a pname the context does
 * not keep.
 */
bool sgi_gl_get(const SGIGLContext *c, uint32_t pname, uint32_t *out,
                unsigned *n);

/*
 * Whether c holds state the context's own commands could have left: each
 * enable 0 or 1, stack depths inside their stacks, enumerants the setters
 * accept, the depth range and values the setters clamp inside their
 * ranges, no box, size, width or fog density the setters refuse, at most
 * SGI_CLIP_MAX clip rectangles, and no more vertices held than the open
 * primitive keeps.
 * False, with the reason in errp, otherwise. Loading a context refuses
 * one that fails; a family that fills a context from its own older stream
 * layout checks it here too.
 */
bool sgi_gl_context_valid(const SGIGLContext *c, Error **errp);

/*
 * Migration of one context: every field of SGIGLContext, described field
 * by field so that neither the host's byte order nor its padding reaches
 * the stream, floats as the IEEE single bits they hold. A family keeps its
 * own table of contexts and sends the table as a subsection of its own
 * description, so adopting it does not change the family's version:
 *
 *     DEFINE_VMSTATE_SGI_GL_CONTEXTS(vmstate_foo_gl, "foo/gl-contexts",
 *                                    FooState, gl, FOO_GL_CONTEXTS);
 *     ... .subsections = (const VMStateDescription * const []) {
 *             &vmstate_foo_gl, NULL },
 *
 * The name must begin with the family description's name and a slash, and
 * the count is the length of the family's array. Loading refuses a context
 * sgi_gl_context_valid() refuses. hw_id and the drawable travel as saved;
 * a family that derives them from its own state sets them again in its own
 * post_load, which runs after this subsection's.
 *
 * Adding a field. A field added to SGIGLContext, or to a structure inside
 * it, fails the byte-coverage test in tests/unit/test-sgi-gl-vmstate.c
 * until it is described. The structure has no padding, and the test
 * counts a padding byte as one not described, so flags are uint32_t like
 * the rest.
 * A new field is described in a new subsection of vmstate_sgi_gl_context,
 * or of the nested description that holds it, never appended to a base
 * field list. The subsection's name is the name of the description it
 * joins, a slash and the field ("sgi-gl-light/<what>" for a light's):
 * loading looks for a subsection only under its parent's name. Unlike a
 * nested description, a subsection puts its own name and version on the
 * stream. Its .needed may return false only while the field
 * holds what sgi_gl_init() gives it; a stream without the subsection then
 * leaves the destination's initial value, as a source that predates the
 * field had, and a binary without it refuses a stream that needs it.
 * SGI_GL_VMSTATE_VERSION does not change.
 *
 * SGI_GL_VMSTATE_VERSION is the base layout's, carried by the family's
 * subsection. It changes only when a described field changes meaning,
 * type or position; the minimum version follows it, so the older layout
 * is refused by name rather than misread. The bare
 * VMSTATE_SGI_GL_CONTEXTS, embedded in a description of the family's own,
 * puts no GL version on the stream, which is enough while migration is
 * supported only between identical builds; the
 * DEFINE_ form carries it.
 */
#define SGI_GL_VMSTATE_VERSION  1

extern const VMStateDescription vmstate_sgi_gl_context;

#define VMSTATE_SGI_GL_CONTEXTS(_field, _state, _n)                      \
    VMSTATE_STRUCT_ARRAY(_field, _state, _n, 0, vmstate_sgi_gl_context,  \
                         SGIGLContext)

#define DEFINE_VMSTATE_SGI_GL_CONTEXTS(_vmsd, _name, _state, _field, _n) \
    static const VMStateDescription _vmsd = {                            \
        .name = (_name),                                                 \
        .version_id = SGI_GL_VMSTATE_VERSION,                            \
        .minimum_version_id = SGI_GL_VMSTATE_VERSION,                    \
        .fields = (const VMStateField[]) {                               \
            VMSTATE_SGI_GL_CONTEXTS(_field, _state, _n),                 \
            VMSTATE_END_OF_LIST()                                        \
        },                                                               \
    }

#endif
