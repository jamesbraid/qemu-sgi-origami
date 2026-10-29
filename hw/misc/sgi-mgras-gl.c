/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * MGRAS OpenGL command execution.
 *
 * On the board the HQ4 and GE11 run SGI programs that turn the OpenGL
 * client's command words into raster-engine work. Those programs are
 * opaque here, so the model reads the words at the boundary the IRIX
 * 6.5.30 client library defines and draws what the OpenGL 1.2.1 specification
 * says those commands produce. This is an approximation of the geometry
 * engine's function, not of its program.
 *
 * What it preserves: the pixels that colour and depth clears, untextured
 * points, lines, triangles, strips, fans, quads and polygons, including
 * point sizes, line widths and polygon raster modes, flat or smooth shaded,
 * lit per vertex by OpenGL's lighting
 * equation, fogged, culled by facing, offset in depth, clipped by the
 * client clip planes, transformed by the modelview and projection
 * matrices and their stacks, and bitmaps, leave in the
 * window's front or back buffer after the per-fragment operations the
 * shared rasterizer carries out (the scissor, alpha, stencil and depth
 * tests, blending, the logical operation and the write masks), in RGBA or
 * colour-index mode, and the main-port framing,
 * so the words after a packet still parse as headers. What it does not: the
 * raster-engine register writes the GE would make, the timing of the
 * work, any GE state visible to a later readback, and every command
 * outside the subset below, which the parser logs through LOG_UNIMP and
 * consumes by length as before.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "system/runstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "ui/sgi-renderer.h"
#include "hw/misc/sgi-mgras-gl.h"
#include "hw/misc/sgi-mgras-texture.h"
#include "hw/misc/sgi-mgras-raster.h"
#include "hw/display/sgi/gl-select.h"
#include "hw/display/sgi/rasterizer.h"
#include "trace.h"

/* Class-0 opcodes, bits 15:8 of the header, with their byte lengths. */
#define MGRAS_GL_CONTEXT_OPENER  0x00000600
/*
 * glBegin and glEnd: 0x1600 to 0x1f00 begin the ten primitive modes, and each
 * mode's end token is 0x0a00 above its begin token. The tokens do not follow
 * the modes' enumerant order: 0x1800 begins GL_LINE_STRIP (3) and 0x1900
 * GL_LINE_LOOP (2). The defines below and mgras_gl_begin_tokens[] are the
 * mapping; no arithmetic on the mode stands in for them.
 */
#define MGRAS_GL_BEGIN_POINTS    0x00001600
#define MGRAS_GL_BEGIN_LINES     0x00001700
#define MGRAS_GL_BEGIN_LINE_STRIP 0x00001800
#define MGRAS_GL_BEGIN_LINE_LOOP 0x00001900
#define MGRAS_GL_BEGIN_POLYGON   0x00001f00
#define MGRAS_GL_END_OFFSET      0x00000a00
#define MGRAS_GL_BEGIN_TRIANGLES 0x00001a00
#define MGRAS_GL_BEGIN_TRI_STRIP 0x00001b00
#define MGRAS_GL_BEGIN_TRI_FAN   0x00001c00
#define MGRAS_GL_BEGIN_QUADS     0x00001d00
#define MGRAS_GL_BEGIN_QUAD_STRIP 0x00001e00
#define MGRAS_GL_RENDER_SELECT   0x0000cb04
#define MGRAS_GL_RENDER          0x0000c900
#define MGRAS_GL_INIT_NAMES      0x0000c400
#define MGRAS_GL_LOAD_NAME       0x0000c504
#define MGRAS_GL_PUSH_NAME       0x0000c704
#define MGRAS_GL_POP_NAME        0x0000c600
#define MGRAS_GL_SELECTION_QUERY 0x00000cec
#define MGRAS_GL_CLEAR_COLOR_BUF 0x00001500
#define MGRAS_GL_CLEAR_DEPTH_BUF 0x00004a00
#define MGRAS_GL_VIEWPORT        0x00003310
#define MGRAS_GL_FLUSH           0x00003600
#define MGRAS_GL_SWAP_TMESH      0x0000dc00
#define MGRAS_GL_SHADE_MODEL     0x00004704
#define MGRAS_GL_CLEAR_COLOR     0x0000ba10
#define MGRAS_GL_CLEAR_DEPTH     0x0000bc04
#define MGRAS_GL_MATRIX_MODE     0x00002a04
#define MGRAS_GL_LOAD_MATRIX     0x00002b40
#define MGRAS_GL_LOAD_IDENTITY   0x00002c00
#define MGRAS_GL_MULT_MATRIX     0x00002d40
#define MGRAS_GL_FRUSTUM         0x00003418
#define MGRAS_GL_ORTHO           0x00003518
/*
 * The matrix stack and transforms, named by the client's display-list
 * decoder:
 * glRotatef's words are the angle in degrees and the axis.
 */
#define MGRAS_GL_POP_MATRIX      0x00002e00
#define MGRAS_GL_PUSH_MATRIX     0x00002f00
#define MGRAS_GL_ROTATE          0x00003010
#define MGRAS_GL_SCALE           0x0000310c
#define MGRAS_GL_TRANSLATE       0x0000320c
#define MGRAS_GL_DEPTH_MASK      0x00003c04
#define MGRAS_GL_DEPTH_FUNC      0x00004304
#define MGRAS_GL_DRAW_BUFFER     0x0000490c
#define MGRAS_GL_DEPTH_TEST      0x00006604
/*
 * glScissor: the scissor routine in the IRIX 6.5.30 GL client library
 * (SHA-256 626d6d94...) rejects a negative width or height and writes
 * 0x3910 and x, width - 1, y, height - 1. glEnable and glDisable of
 * GL_SCISSOR_TEST write 0x6e04 and 1 or 0 (the enable routine for
 * capability 0xc11, and the disable routine). Established from the
 * listing. That the board discards
 * fragments outside the box as OpenGL 1.2.1, 4.1.2 says is inferred; the
 * discriminating test is gl-scissor-bounds-draws-and-clears.
 */
#define MGRAS_GL_SCISSOR         0x00003910
#define MGRAS_GL_SCISSOR_TEST    0x00006e04
/*
 * glAlphaFunc: the alpha-function routine accepts a function from
 * GL_NEVER to GL_ALWAYS and writes 0x3e08, the function minus GL_NEVER,
 * and the reference clamped to [0, 1], times the float 4095.0, plus the
 * double 0.5 and truncated, as a 16-bit signed value: the reference as a
 * 12-bit fraction. glEnable and glDisable of GL_ALPHA_TEST write 0x6404
 * and 1 or 0 (the enable routine for capability 0xbc0, and the disable
 * routine). Established from the listing; that the board tests fragments
 * as OpenGL 1.2.1, 4.1.4 says is inferred. The discriminating test is
 * gl-alpha-test-discards.
 */
#define MGRAS_GL_ALPHA_FUNC      0x00003e08
#define MGRAS_GL_ALPHA_TEST      0x00006404
#define MGRAS_GL_ALPHA_REF_MAX   4095
/*
 * The stencil test, from the IRIX 6.5.30 GL client library's
 * emitters, established from the listing:
 *
 * - glStencilFunc: 0x410c, the function less GL_NEVER, the reference
 *   clamped to [0, 255] and the mask as the application passed it.
 * - glStencilOp: 0x420c and the three operations as small numbers,
 *   GL_KEEP 0, GL_ZERO 1, GL_REPLACE 2, GL_INCR 3, GL_DECR 4 and
 *   GL_INVERT 5; the glGet routine's decoding maps them back the same way.
 * - glStencilMask: 0x3a04 and the mask.
 * - glClearStencil: 0xbe04 and the value masked to the visual's stencil
 *   bits.
 * - glClear with GL_STENCIL_BUFFER_BIT: 0x4c00 alone, only when the visual
 *   has a stencil buffer.
 * - glEnable and glDisable of GL_STENCIL_TEST (capability 0xb90): 0x6f04
 *   and 1 or 0.
 *
 * Every visual the X server offers with a depth buffer has 24 depth bits
 * and 8 stencil bits (glxinfo on the model, in a captured run), so the board
 * has an 8-bit stencil buffer beside the 24-bit depth buffer (inferred). The
 * model keeps it in bits 31:24 of each pixel's depth word, an internal layout:
 * how the board packs the two is unknown. That the board tests and updates it
 * as OpenGL 1.2.1, 4.1.5 says is inferred. A context whose visual has no
 * stencil buffer still tests against the planes, which the specification
 * says pass every fragment; the model cannot tell the visual. The
 * discriminating tests are gl-stencil-masks-drawing and
 * gl-stencil-clear-keeps-depth.
 */
#define MGRAS_GL_STENCIL_FUNC    0x0000410c
#define MGRAS_GL_STENCIL_OP      0x0000420c
#define MGRAS_GL_STENCIL_MASK    0x00003a04
#define MGRAS_GL_CLEAR_STENCIL   0x0000be04
#define MGRAS_GL_CLEAR_STENCIL_BUF 0x00004c00
#define MGRAS_GL_STENCIL_TEST    0x00006f04
#define MGRAS_GL_STENCIL_SHIFT   24
#define MGRAS_GL_DEPTH_MASK_24   0x00ffffffu

/*
 * Blending, established from the listing: glBlendFunc validates both
 * factors and writes 0x3f08 and the two enumerants unchanged;
 * glBlendEquationEXT accepts GL_LOGIC_OP, GL_FUNC_ADD, GL_MIN, GL_MAX,
 * GL_FUNC_SUBTRACT and GL_FUNC_REVERSE_SUBTRACT and writes 0x6004 and the
 * enumerant; glBlendColorEXT writes 0xd910 and four floats as the
 * application passed them; glEnable and glDisable of GL_BLEND (capability
 * 0xbe2) write 0x6504 and 1 or 0. Every
 * context setup sends 0x3f08 with 1 and 0 and 0x6004 with 0x8006,
 * OpenGL's initial GL_ONE, GL_ZERO and GL_FUNC_ADD. That the board blends
 * as OpenGL 1.2.1, 4.1.6 says, with the stored colour at the eight bits a
 * channel the model keeps, is inferred; the stored colour has no alpha.
 * The discriminating test is gl-blend-mixes-with-the-window.
 */
#define MGRAS_GL_BLEND_FUNC      0x00003f08
#define MGRAS_GL_BLEND_EQUATION  0x00006004
#define MGRAS_GL_BLEND_COLOR     0x0000d910
#define MGRAS_GL_BLEND           0x00006504
/*
 * The logical operation, established from the listing: glLogicOp accepts
 * GL_CLEAR to GL_SET and writes 0x4004 and the operation less GL_CLEAR.
 * glEnable of GL_COLOR_LOGIC_OP (capability 0xbf2) writes 0x6b04 and 1
 * when a context byte is set, and of GL_INDEX_LOGIC_OP
 * (capability 0xbf1) when it is clear, so one token enables the operation
 * for the context's own colour form, the byte marking an RGBA context
 * (inferred from which capability each path serves); glDisable writes
 * 0x6b04 and 0. The model applies the token to GL_INDEX_LOGIC_OP in a
 * colour-index context and GL_COLOR_LOGIC_OP otherwise. That the board
 * combines fragments as OpenGL 1.2.1, 4.1.8 says is inferred; the
 * discriminating test is gl-logic-op-combines-with-the-window.
 */
#define MGRAS_GL_LOGIC_OP        0x00004004
#define MGRAS_GL_LOGIC_OP_ENABLE 0x00006b04
/*
 * glColorMask: the colour-mask routine writes 0x3b04 and one word with
 * bit 0 set for red, 1 green, 2 blue and 3 alpha, the bits the
 * GL_COLOR_WRITEMASK query decodes. Established
 * from the listing; that the board keeps a masked channel's stored bits
 * (OpenGL 1.2.1, 4.2.2) is inferred. The discriminating test is
 * gl-colour-mask-keeps-channels.
 */
#define MGRAS_GL_COLOR_MASK      0x00003b04
/*
 * Fog, established from the listing: glFogfv (glFogf and glFogi call its
 * routine) writes, by parameter, GL_FOG_COLOR as 0xb410
 * and the four floats as the application passed them, GL_FOG_START as
 * 0xb504, GL_FOG_END as 0xb604 and GL_FOG_DENSITY as 0xb704 and a float,
 * a negative density being the client's own GL_INVALID_VALUE, and
 * GL_FOG_INDEX as 0xb804 and GL_FOG_MODE as 0xb904 and the value
 * truncated to an integer, the mode only when it is GL_EXP, GL_EXP2 or
 * GL_LINEAR. glEnable and glDisable of GL_FOG (capability 0xb60) write
 * 0x6804 and 1 or 0. That the board fogs
 * fragments as OpenGL 1.2.1, 3.10 says is inferred; the discriminating
 * test is gl-fog-blends-toward-the-fog-colour.
 */
#define MGRAS_GL_FOG_COLOR       0x0000b410
#define MGRAS_GL_FOG_START       0x0000b504
#define MGRAS_GL_FOG_END         0x0000b604
#define MGRAS_GL_FOG_DENSITY     0x0000b704
#define MGRAS_GL_FOG_INDEX       0x0000b804
#define MGRAS_GL_FOG_MODE        0x0000b904
#define MGRAS_GL_FOG             0x00006804
/*
 * Polygon offset, established from the listing: glPolygonOffsetEXT
 * writes 0xe70c, the factor, the bias and the float 1.0; glPolygonOffset
 * writes 0xe70c, the factor, the units and the float 1.1. glEnable and
 * glDisable write 0xe804 and the context's word of three enables, bit 0
 * for GL_POLYGON_OFFSET_FILL (capability 0x8037), bit 1 for _LINE
 * (0x2a02) and bit 2 for _POINT (0x2a01); glDisable clears its bit.
 *
 * The third word is all that tells the two commands apart, and what the
 * board does with its value is unknown. The model reads 1.0 as the EXT
 * form, whose second word is a bias in window depth, and any other value
 * as the core form, whose second word counts depth steps, logging a value
 * other than 1.1. The GE keeps the bias and the units apart, since
 * GL_POLYGON_OFFSET_BIAS_EXT and GL_POLYGON_OFFSET_UNITS are different
 * query indices (0xca3 and 0xca4). That the board offsets depth as OpenGL
 * 1.2.1, 3.5.5 and EXT_polygon_offset say is inferred; the discriminating
 * test is gl-polygon-offset-moves-depth.
 */
#define MGRAS_GL_POLYGON_OFFSET  0x0000e70c
#define MGRAS_GL_OFFSET_ENABLES  0x0000e804
#define MGRAS_GL_OFFSET_EXT      0x3f800000u
#define MGRAS_GL_OFFSET_CORE     0x3f8ccccdu
/*
 * Client clip planes, established from the listing: glClipPlane accepts a
 * plane below the context's count and writes 0xa514, the plane's
 * enumerant and its four coefficients converted from double to float,
 * untransformed. glEnable and glDisable of GL_CLIP_PLANEi write 0xa604
 * and 0xa704 and the enumerant. Every context setup sends all six planes
 * as zeros through two unnamed routines and perfly disables all six each
 * frame. The client does not transform the plane, so the board does, by
 * the inverse of the modelview current when it arrives (OpenGL 1.2.1,
 * 2.11; inferred), and clips as that section says. The discriminating
 * test is gl-clip-plane-clips-in-eye-space.
 */
#define MGRAS_GL_CLIP_PLANE      0x0000a514
#define MGRAS_GL_CLIP_PLANE_ON   0x0000a604
#define MGRAS_GL_CLIP_PLANE_OFF  0x0000a704
/*
 * Rasterization state the model keeps and answers but does not draw
 * with, established from the listing:
 *
 * - glLineWidth and glPointSize reject a value not above 0 with the
 *   client's own GL_INVALID_VALUE and write 0x4504 or 0x4d04 and the
 *   float.
 * - glLineStipple writes 0x4608, the repeat clamped to [1, 256] less 1,
 *   and the pattern with its 16 bits reversed; the queries undo both
 *   (0xc46's low 16 bits reversed, 0xc47's answer plus 1).
 * - glPolygonMode writes 0xa808 and the face and mode enumerants
 *   unchanged.
 * - glEnable and glDisable write 1 or 0 after 0x6904 for GL_LINE_SMOOTH
 *   (capability 0xb20), 0x6a04 for GL_LINE_STIPPLE (0xb24), 0x6c04 for
 *   GL_POINT_SMOOTH (0xb10) and 0x6d04 for GL_POLYGON_STIPPLE (0xb42).
 *   GL_POLYGON_SMOOTH sends no token, so the model cannot follow it.
 *
 * The rasterizer follows point size, line width and polygon mode.
 * Line stipple follows the reviewed shared phase policy. Smoothing remains
 * unsupported and is logged. perfly
 * reads all of it back at start; with zero answers it
 * sent glPointSize(0) and glLineWidth(0), which the client rejects.
 */
#define MGRAS_GL_LINE_WIDTH      0x00004504
#define MGRAS_GL_POINT_SIZE      0x00004d04
#define MGRAS_GL_LINE_STIPPLE    0x00004608
#define MGRAS_GL_POLYGON_MODE    0x0000a808
#define MGRAS_GL_LINE_SMOOTH     0x00006904
#define MGRAS_GL_LINE_STIPPLE_ON 0x00006a04
#define MGRAS_GL_POINT_SMOOTH    0x00006c04
#define MGRAS_GL_POLYGON_STIPPLE 0x00006d04
#define MGRAS_GL_POLYGON_STIPPLE_UPLOAD 0x00004e80
#define MGRAS_GL_POLYGON_STIPPLE_UPLOAD_REG \
    ((MGRAS_GL_POLYGON_STIPPLE_UPLOAD >> 8) & 0xff)

/*
 * Consumed and named, not modelled: glMapGrid1f writes 0xae10, the count,
 * u1, u2 and the step; glMapGrid2f 0xaf20 and the same for u then v;
 * glClearAccum 0xbb10 and the four clamped components. Established from
 * the listing. The client evaluates and accumulates on the host:
 * glClear's accumulation bit calls a routine through the context and
 * writes no port token, so the board has no accumulation buffer for these
 * words to reach (inferred). What the GE does with the grids is unknown;
 * the model draws nothing for them, logged.
 */
#define MGRAS_GL_MAP_GRID1       0x0000ae10
#define MGRAS_GL_MAP_GRID2       0x0000af20
#define MGRAS_GL_CLEAR_ACCUM     0x0000bb10

/* The capability each enable token names. */
static const struct {
    uint32_t header;
    uint32_t cap;
} mgras_gl_raster_enables[] = {
    { MGRAS_GL_LINE_SMOOTH, SGI_GL_LINE_SMOOTH },
    { MGRAS_GL_LINE_STIPPLE_ON, SGI_GL_LINE_STIPPLE },
    { MGRAS_GL_POINT_SMOOTH, SGI_GL_POINT_SMOOTH },
    { MGRAS_GL_POLYGON_STIPPLE, SGI_GL_POLYGON_STIPPLE },
};

/* glStencilOp's operations by the number the client sends for each. */
static const uint16_t mgras_gl_stencil_ops[] = {
    SGI_GL_KEEP, 0, SGI_GL_REPLACE, SGI_GL_INCR, SGI_GL_DECR, SGI_GL_INVERT,
};
/*
 * glDepthRange: the depth-range routine in the IRIX 6.5.30 GL
 * client library (SHA-256 626d6d94...) writes 0x4808 and near and far
 * converted to single floats, unclamped, outside glBegin/glEnd.
 */
#define MGRAS_GL_DEPTH_RANGE     0x00004808
/*
 * glEnable and glDisable of GL_DITHER (pname 0xbd0): the enable routine
 * writes 0x6704 and 3 and the disable routine 0x6704 and 0, so a nonzero
 * word enables (established).
 * Every context setup sends it with 3.
 */
#define MGRAS_GL_DITHER          0x00006704
/*
 * Polygon culling, from the IRIX 6.5.30 GL client library (SHA-256
 * 626d6d94...), established from the listing:
 *
 * - glCullFace: the cull-face routine accepts a mode from 0x404 to 0x408
 *   and writes 0x7404 and the mode unchanged; the display-list compiler's
 *   cull-face routine compiles the same pair into a display list.
 * - glFrontFace: the front-face routine accepts 0x900 (GL_CW) or 0x901
 *   (GL_CCW) and writes 0x5604 and the direction unchanged; the
 *   display-list compiler's front-face routine compiles it.
 * - glEnable(GL_CULL_FACE): the enable routine, for capability 0xb44,
 *   writes 0x7200 alone and sets bit 0x1000 of a context word.
 *   glDisable writes 0x7300 alone and clears the bit, only while
 *   it is set (the disable routine).
 * - the GE11 cull-state restore routine writes 0x7200 or 0x7300 from that
 *   bit, and the host-culling glDrawArrays path writes 0x7300 before
 *   drawing, so a client that culls on the host turns the board's
 *   culling off first and restores it after.
 *
 * Every context setup sends 0x7404 with 0x405 and 0x5604 with 0x901,
 * OpenGL's initial values. That the
 * board then culls and chooses two-sided colours as OpenGL 1.2.1, 3.5.1
 * and 2.13.1 give is inferred from the specification; the discriminating
 * tests are gl-cull-face-culls-by-facing and
 * gl-front-face-selects-the-two-sided-colour.
 */
#define MGRAS_GL_CULL_ON         0x00007200
#define MGRAS_GL_CULL_OFF        0x00007300
#define MGRAS_GL_CULL_FACE       0x00007404
#define MGRAS_GL_FRONT_FACE      0x00005604
/*
 * The context setup's texture-only defaults, consumed without a log line
 * while they carry the default: 0xec10 from the texture-memory initialization
 * routine in the IRIX 6.5.30 GL client library, 32 per context
 * setup, loading the detail-texture function table glDetailTexFuncSGIS sets.
 * Its words are two HQ-table words, 0xc52 and 0xc78 from the hq3_revB_HQ4.u
 * table, the raster register 0x194 and one table value; the setup's values are
 * 0x1f three times and then 0x1d down to 1.
 * The SI has no texture memory and the model draws no texture, so the
 * table changes nothing it draws. The test is on each packet's values,
 * not on the setup's sequence, so an application's glDetailTexFuncSGIS
 * whose values fall in the same set also goes unlogged; that loses
 * nothing, since nothing is textured. Any other 0xec10, an application's
 * table with another value or a texture path's own register (0x191,
 * 0x193, 0x1a4), is logged. On a board with TRAM the texture path takes
 * the 0x191 and 0x193 tables first (hw/misc/sgi-mgras-texture.c).
 */
#define MGRAS_GL_TEXTURE_TABLE   0x0000ec10
#define MGRAS_GL_BANKS           0x00009808

/*
 * The kernel's GE external RAM transfers ("Context eviction" below): a
 * read of an address and a word count after a DMA read kick, the word
 * count of a write before its DMA kick, the write's address and count
 * after it, and the end of a run of writes.
 */
#define MGRAS_GL_ERAM_READ       0x0000fc08
#define MGRAS_GL_ERAM_COUNT      0x0000fd04
#define MGRAS_GL_ERAM_WRITE      0x0000fb08
#define MGRAS_GL_ERAM_END        0x0000fa00
/* The class of the DMA descriptor words a write's count precedes. */
#define MGRAS_GL_DMA_DESC_CLASS  0x0008

/* Where an ERAM transfer stands between its DMA kick and its token. */
enum {
    MGRAS_ERAM_IDLE,
    MGRAS_ERAM_READ_KICKED,
    MGRAS_ERAM_WRITE_ARMED,
    MGRAS_ERAM_WRITE_KICKED,
    MGRAS_ERAM_WRITE_INLINE,
    MGRAS_ERAM_STATES,
};
/* An inline data block's header bit. */
#define MGRAS_GL_INLINE_BLOCK    0x80000000u

/* Vertex-attribute headers: the class names the source format. */
#define MGRAS_GL_VERTEX2F        0x40000008
#define MGRAS_GL_VERTEX3F        0x2000000c
#define MGRAS_GL_VERTEX4F        0x00000010
#define MGRAS_GL_COLOR3F         0x2000020c
#define MGRAS_GL_COLOR4F         0x00000210
#define MGRAS_GL_VERTEX2I        0x58000008
/*
 * glVertex2d, 3d and 4d: the doubles' class 0x12 with the component-count
 * bits, as the client sends them. The Octane SI Icon Catalog's
 * pane sends glVertex2d for every filled icon polygon: a model run logged
 * 122 of them as unimplemented headers, where the Origin 2000's X server
 * draws the same icons' fills as stippled spans.
 */
#define MGRAS_GL_VERTEX2D        0x52000010
#define MGRAS_GL_VERTEX3D        0x32000018
#define MGRAS_GL_VERTEX4D        0x12000020
#define MGRAS_GL_COLOR4UB        0x14800204
/*
 * glColor3ub is sent as 0x34800203 and one word. The model matches every
 * header with its byte length rounded up to the whole words the payload
 * takes.
 */
#define MGRAS_GL_COLOR3UB        0x34800204
/*
 * glNormal3f, and glEdgeFlag true and false.
 */
#define MGRAS_GL_NORMAL3F        0x0000070c
/*
 * The other glNormal3 forms, from their emitters in the IRIX 6.5.30
 * the GL client library (SHA-256 626d6d94...), each the components as the
 * application passed them, established from the listing:
 *
 * - glNormal3d and 3dv: 0x12000718 and three doubles, each one 64-bit
 *   store, so its high word first.
 * - glNormal3i and 3iv: 0x1c00070c and three ints.
 * - glNormal3s and 3sv: 0x1d000706, then x in bits 31:16 and y in 15:0 of
 *   one word, and z in bits 31:16 of a second.
 * - glNormal3b and 3bv: 0x1c800703, then x, y and z in bits
 *   31:24, 23:16 and 15:8 of one word.
 *
 * The short and byte headers are matched with their lengths rounded up
 * to the words they take. The client converts nothing, so the board does;
 * that it converts as OpenGL 1.2.1's table 2.6 gives for a signed normal
 * component, (2c + 1) / (2^b - 1) for b bits, is inferred from the
 * specification and the classes' reading as scaled types.
 */
#define MGRAS_GL_NORMAL3D        0x12000718
#define MGRAS_GL_NORMAL3I        0x1c00070c
#define MGRAS_GL_NORMAL3S        0x1d000708
#define MGRAS_GL_NORMAL3B        0x1c800704
/* glRasterPos2f and 3f. */
#define MGRAS_GL_RASTER_POS2F    0x40003808
#define MGRAS_GL_RASTER_POS3F    0x2000380c
/*
 * The class-0x3800 forms, three integers, and glTexCoord2f, from their
 * emitters in the IRIX 6.5.30 GL client library (SHA-256
 * 626d6d94...), each the header and then the components as the
 * application passed them, established from the listing:
 *
 * - glVertex3i and 3iv: 0x3800000c, x, y, z.
 * - glRasterPos3i and 3iv: 0x3800380c, x, y, z.
 * - glTexCoord3i and 3iv: 0x3800080c, s, t, r.
 * - glTexCoord2f and 2fv: 0x40000808, s, t as floats; 2fv stores the pair
 *   with one 64-bit store, s first.
 * - glTexCoord4f and 4fv: 0x00000810, s, t, r, q as floats.
 *
 * The client converts nothing, so the board does; that it takes an
 * integer coordinate unscaled, as OpenGL 1.2.1 does (2.7), is inferred.
 * blast and mview send glVertex3i, blast and buttonfly glTexCoord2f
 * (the 2026-09-28 demo survey's -d unimp logs), and mirrors and perfly
 * glTexCoord4f. The model retains coordinates for current-attribute
 * queries and captures the texture-matrix result at raster-position
 * updates. On a board with TRAM the texture path
 * (hw/misc/sgi-mgras-texture.c) samples a loaded level with them.
 */
#define MGRAS_GL_VERTEX3I        0x3800000c
#define MGRAS_GL_RASTER_POS3I    0x3800380c
#define MGRAS_GL_TEX_COORD3F     0x2000080c
#define MGRAS_GL_TEX_COORD3I     0x3800080c
#define MGRAS_GL_TEX_COORD2F     0x40000808
#define MGRAS_GL_TEX_COORD4F     0x00000810
#define MGRAS_GL_EDGE_FLAG_TRUE  0x00000f00
#define MGRAS_GL_EDGE_FLAG_FALSE 0x00001000

/*
 * Colour-index mode, from the IRIX 6.5.30 GL client library's own
 * code (SHA-256 626d6d94...). The context setup calls
 * the colour-index initializer for each colour buffer when the
 * visual is a colour-index one and another initializer otherwise; the
 * colour-index initializer writes 0x0a04 and a word of the visual's
 * configuration, from which it also forms the index mask (1 << n) - 1, and the
 * RGB initializer writes 0x0900 and nothing more. So 0x0a04's word
 * is the number of bits a colour index holds, 8 in gr_osview's context setup
 * and in glxinfo's, 12 in showmap's and interp's on the server's 12-plane
 * PseudoColor visuals, and 0x0900 marks an RGBA context. That the
 * board switches its colour handling on them is inferred.
 */
#define MGRAS_GL_INIT_CI         0x00000a04
#define MGRAS_GL_INIT_RGB        0x00000900
/* The deepest colour index a pixel keeps (mgras_gl_store_index()). */
#define MGRAS_GL_INDEX_BITS      12
/*
 * glIndexs, glIndexi and glIndexf: headers 0x19000202, 0x18000204 and
 * 0x00000204, the colour register 0x02 under the attribute classes for a
 * short, an integer and a float, from the client's short-index,
 * integer-index and float-index routines. glIndexs sends the short's 16
 * bits in bits 31:16 of its one word and zeros in 15:0 (andi 0xffff,
 * dsll 16); gr_osview's words 0x20000 and 0x30000 are
 * indices 2 and 3. The short is read signed, as the class's short type
 * reads for a vertex (inferred from the attribute table); glIndexs's
 * header is matched with its length rounded up to the word it takes.
 */
#define MGRAS_GL_INDEXS          0x19000204
#define MGRAS_GL_INDEXI          0x18000204
#define MGRAS_GL_INDEXF          0x00000204
/*
 * glIndexMask: 0x3d04 and the mask ANDed with the context's index mask.
 * glClearIndex: 0xbd04 and a float, trunc(c * 16.0) ANDed with (index
 * mask << 4) | 0xf and scaled by 0.0625, both constants held as
 * single-precision literals in the library: the index with four fraction
 * bits.
 */
#define MGRAS_GL_INDEX_MASK      0x00003d04
#define MGRAS_GL_CLEAR_INDEX     0x0000bd04

/*
 * glBitmap (the bitmap routine, and the bitmap render routine for a
 * display list). In render mode, with glPixelStore's unpack
 * state at its defaults but an alignment of one, and an image of at most 512
 * words, the client writes 0x9420 and eight words: a flags word, the width
 * and height as integers, xorig, yorig, xmove and ymove as floats, and the
 * image's length in words, ceil(ceil(width / 8) * height / 4). An inline
 * data block follows, 0x80000000 | length * 4, whose words
 * the pixel push routine copies from the application's image in
 * memory order; a 64-bit store is two stream words, high first. When
 * the image is not word aligned the client sets bits 23:22 of the block
 * header to the image's byte offset in its first word and sends one word
 * more, from the word holding the first byte. A zero word follows when
 * the number of words sent is odd: when the length is odd for an aligned
 * image, and when it is even for one with an offset. gr_osview's 94
 * bitmaps in the survey run are all
 * this form: flags 0x1800f, width 16, height 1 or 2, one data word and the
 * zero word. The other paths, for a larger image or other unpack state
 * (0x9114, 0x9008 with raster register records, then 0x9208 with xmove and
 * ymove), are not modelled.
 *
 * Established from the listing: the packet's words and their order,
 * the block's length and offset, and the bytes' order in the stream.
 * Inferred: that the image is rows of ceil(width / 8) bytes from the
 * bottom row up with the most significant bit leftmost (OpenGL 1.2.1,
 * 3.6.4 and 3.7, glPixelStore's defaults; the client requires them before
 * taking this path, and its other path sends the least-significant-first
 * flag as bit 17 of the flags word, which this path leaves clear), that
 * the set bits take the raster position's colour or index and depth, and
 * that the board moves the raster position by xmove and ymove, which the
 * client does not do itself on this path but sends 0x9208 for on the
 * other. The flags word's bits 16:14, (8 - ceil(width / 8) % 8) % 8, and
 * bits 3:0, 0xf, are established as values; their meaning is unknown, and
 * the model does not use them.
 */
#define MGRAS_GL_BITMAP          0x00009420
#define MGRAS_GL_BITMAP_OP       0x94
#define MGRAS_GL_BITMAP_WORDS    8
#define MGRAS_GL_BITMAP_LSB_FIRST 0x00020000
/*
 * The raster-position update routine writes 0xd608 and two floats;
 * the client's glBitmap calls it with xmove and ymove for an empty image, so
 * it moves a valid raster position by them (inferred, 3.7).
 */
#define MGRAS_GL_MOVE_RASTER     0x0000d608
/*
 * The raster-position routine writes 0x58003808 and the two
 * integers; OpenGL takes them as glRasterPos4f(x, y, 0, 1) (2.12).
 */
#define MGRAS_GL_RASTER_POS2I    0x58003808
/*
 * The words each pixel transfer brackets its GE program with:
 * The RSS save routine writes 0xd214 and five words and the RSS restore
 * routine 0xd300, the texture-mode save routine 0xe910 and four words and
 * the texture-mode restore routine 0xea00, and the raster-position load
 * routine 0xd700 alone. The model draws
 * the image without raster registers or texturing, so it consumes them;
 * that they change nothing a later draw sees is inferred from their
 * names and pairing.
 */
#define MGRAS_GL_SAVE_RSS        0x0000d214
#define MGRAS_GL_RESTORE_RSS     0x0000d300
#define MGRAS_GL_LOAD_RASTER_INFO 0x0000d700
#define MGRAS_GL_SAVE_TEXMODE    0x0000e910
#define MGRAS_GL_RESTORE_TEXMODE 0x0000ea00
/*
 * glReadPixels and glCopyPixels, as the IRIX 6.5.30 client sends them (the
 * pixel read-back and copy contract; no capture has the board's side).
 * The pixel-read routine brackets a whole read with 0x9600 and 0x9700,
 * neither with a payload, and the data-port read ends its register setup
 * with 0xdb04 and a word and starts with 0x8f08 and two zeros. The DMA
 * read's 0xda00 follows its kick. The client batches raster registers into
 * 0x7f08 packets of (register, value) pairs, a record count and a word
 * count and then an inline block, as 0x7c08 does with triples.
 * Approximated: the brackets, the setup word and the ERAM push around a
 * read are consumed as opaque, since what the board does with them beyond
 * ordering is unknown; the model reads at the 0x8f08 trigger, the only
 * word the data-port path has that the server's does not.
 */
#define MGRAS_GL_RSS_PAIRS       0x00007f08
#define MGRAS_GL_RSS_PAIRS_OP    0x7f
#define MGRAS_GL_READ_OPEN       0x00009600
#define MGRAS_GL_READ_CLOSE      0x00009700
#define MGRAS_GL_READ_BUFFER     0x0000440c
#define MGRAS_GL_READ_SETUP      0x0000db04
/*
 * The RE count shifted right once: a context setup sends it right after the
 * opener (1 on SSI, 0 on SI), a cross-check and no draw.
 */
#define MGRAS_GL_RE_COUNT        0x00008c04
#define MGRAS_GL_READ_PIO        0x00008f08
#define MGRAS_GL_READ_DMA_DONE   0x0000da00
/* Parser payload slots a records block keeps its place in; see below. */
#define MGRAS_GL_REC_NEXT        8
#define MGRAS_GL_REC_REG         9
#define MGRAS_GL_REC_VALUE       10
#define MGRAS_GL_REC_TEXTURE     11     /* the texture path takes the words */
/* A GE pixel draw, whose image follows it; see mgras_gl_pixels_start(). */
#define MGRAS_GL_PIXELS          0x00008d20
#define MGRAS_GL_PIXELS_OP       0x8d
#define MGRAS_GL_PIXELS_WORDS    8
/* The GE parameter loads and pixel draws; see mgras_gl_pixels_start(). */
#define MGRAS_GL_GE_CRAM_OP      0x80
#define MGRAS_GL_GE_ERAM_OP      0xcd
#define MGRAS_GL_GE_LOAD_HEAD    7      /* payload words before list one */
#define MGRAS_GL_GE_LOAD_ADDR    3
#define MGRAS_GL_GE_LOAD_COUNT   6
/* Parser payload slots a load keeps its place in. */
#define MGRAS_GL_GE_LOAD_SECOND  7      /* list two's length */
#define MGRAS_GL_GE_LOAD_TOTAL   8      /* the load's payload words */
#define MGRAS_GL_GE_LOAD_DONE    9      /* payload words taken */
#define MGRAS_GL_GE_LOAD_SLOTS   10
#define MGRAS_GE_PIXEL_PROGRAM   0x13b
#define MGRAS_GE_PIXEL_PROGRAM_PAIRS 8
#define MGRAS_GE_PIXEL_ZOOM      0x191
#define MGRAS_GE_PIXEL_BOX       0xe34
#define MGRAS_GE_ROUTINE_END     0x352d
/*
 * The writing routine that ends a pixel program: 0x352d unzoomed and for
 * a whole x zoom with y zoom 1, 0x3663 with a y zoom other than 1, 0x3715
 * with a fractional x zoom (model runs of a glPixelZoom probe: zooms 2,2
 * 1,3 and 0.5,0.5 load 0x3663; 3,1, -1,1 and 1,-1 load 0x352d; 1.5,1.5
 * loads 0x3715). The model reads the zoom from the box and from 0x192.
 */
#define MGRAS_GE_ROUTINE_ZOOM_Y  0x3663
#define MGRAS_GE_ROUTINE_ZOOM_XF 0x3715
/*
 * The raster engine's x zoom (the raster and display contract's zoom
 * factor reading and its traced words). Before a draw the client's 0x7c08
 * records load 0x154 and 0x155 with one 8.8 factor (256 = 1) and its
 * 0x7f08 pairs load the transfer mode 0x159, whose bit 19 makes the
 * factor a magnification and bit 20 a source step. A zoom that is not a
 * whole number reaches the raster engine as a step through each source
 * pixel replicated n times by the geometry engine (zoom 1.5: n = 2, step
 * 341; zoom 3.25: n = 4, step 315). Zoom 1 sends 0x1000f424 and 0 with
 * neither bit, which the raster engine is taken to ignore. Established
 * as the IRIX 6.5.30 client's words in a model trace; that RE4 zooms by
 * them is inferred.
 *
 * Approximated: the board's sampling rule is unverified, so the model
 * draws with OpenGL 1.2.1's own glPixelZoom rule (3.6.4: a source pixel
 * fills the window pixels whose centres its zoomed footprint covers) at
 * the zoom the factor gives, n * 256 / step or factor / 256. That keeps
 * the factor's truncation (1.5 draws as 512 / 341) but not whatever
 * rounding RE4 applies within a span; a board read-back of the same
 * draw would settle it. n is not in any register the raster engine sees,
 * so the model takes the whole number nearest the box's width times the
 * step over 256 source pixels. 0x154 differing from 0x155, a second
 * client path whose words are not decoded, and both bits together are
 * logged and draw at the box's zoom. The discriminating test is
 * gl-pixels-raster-zoom.
 */
#define MGRAS_RSS_ZOOM_INITIAL   0x154
#define MGRAS_RSS_ZOOM_STEP      0x155
#define MGRAS_RSS_XFRMODE        0x159
#define MGRAS_XFRMODE_MAGNIFY    (1u << 19)
#define MGRAS_XFRMODE_DECIMATE   (1u << 20)
#define MGRAS_ZOOM_FACTOR_MAX    0x1ffff
#define MGRAS_GE_UNPACK_4UB      0x4064
#define MGRAS_GE_UNPACK_3UB      0x40f2
#define MGRAS_HQ_PIXEL_ABGR      0x80
#define MGRAS_HQ_PIXEL_SIZE      0x03
#define MGRAS_GL_FLOAT_ONE       0x3f800000u
#define MGRAS_GL_PIXELS_MAX      4096
/* How far from the origin a draw may lie; see mgras_gl_pixels_place(). */
#define MGRAS_GL_PIXELS_FAR      0x40000000

enum {
    MGRAS_GL_PIXELS_ABGR8,
    MGRAS_GL_PIXELS_RGBA8,
    MGRAS_GL_PIXELS_RGB8,
    MGRAS_GL_PIXELS_FLOAT_RGB,
    MGRAS_GL_PIXELS_FLOAT_RGBA,
    MGRAS_GL_PIXELS_INDEX,
    MGRAS_GL_PIXELS_FORMATS,
};

/*
 * glShadeModel's and glMatrixMode's words are the OpenGL enumerants, and
 * glDepthFunc's is the function minus GL_NEVER (OpenGL 4.1.5), the form
 * SGIGLDrawState keeps.
 *
 * The draw-buffer triple's first word: 1 in the context setup of the
 * probe's single-buffered visual, 2 in that of its double-buffered one
 * (a captured run), where OpenGL's default draw buffer is
 * the front and the back buffer respectively. Reading it as a bit mask of
 * the front and back buffers, SGI_GL_DRAW_FRONT and SGI_GL_DRAW_BACK, is
 * inferred from those two values. Bits 2 and 3 name slots 2 and 3 of a
 * two-RE board (mgras_gl_bind()); bits above 3 are not modelled.
 */

/*
 * The window the current context draws into, from the 63-word image the
 * IRIX kernel's context-switch routine writes at each context switch. In the
 * image of the probe's GL context, whose window's top-left pixel is at screen
 * (256, 256) and which is 128 pixels square (a captured run,
 * seq 3959112), word 2 is 0x02800100, the window's lower-left corner as
 * (y << 16) | x with y counted up from the bottom of the screen (640 =
 * 1024 - 256 - 128), the packing OpenBSD's impact(4) gives XYWIN. Words 11
 * and 12 are 0x0100017f and 0x028002ff, the window's columns 256-383 and
 * rows 640-767 from the bottom as first << 16 | last, the layout the X
 * server writes to the clip registers 0x147 and 0x148, and word 3 is 0x11.
 * The X server's own image carries 0x03ff0000 in word 2, XYWIN's reset
 * value. That the board restores these words into those registers is
 * inferred from the matching values; the model reads them from the image
 * and keeps them out of the 2D register file.
 *
 * The kernel's clip validation routine (in the IRIX 6.5.30 kernel graphics
 * driver, SHA-256 6a65e70d3cdc...) writes words 2 to 15
 * from the window and its visible rectangles, bottom-up and inclusive,
 * each as first << 16 | last. Established from that listing:
 *
 * - Word 3 is (mode << 4) | enables. Enable bit i is set for each of the
 *   first four visible rectangles, rectangle 0 in words 11 and 12, 1 in
 *   9 and 10, 2 in 7 and 8 and 3 in 5 and 6. All four pairs are stored
 *   on every call, on the port and in the saved copy; a pair whose bit is
 *   clear carries leftover stack contents that
 *   mean nothing.
 * - A window with one to four visible rectangles gets mode 1 and those
 *   rectangles. One with none, but a nonzero width, gets mode 1 and its
 *   own rectangle clamped to the screen.
 * - One with five or more gets mode 1 and no enable bits: 0x10.
 * - The kernel's null clip (the null-clip routine, a window of no size and no
 *   rectangles) gets mode 2 with rectangles 0 and 1 both the whole
 *   screen, 0x23, and word 2 then puts the origin at row 1024. Every
 *   image of perfly's context 0x84c6 in a captured run
 *   is this form.
 *
 * Inferred: that the board draws only inside the enabled rectangles in
 * mode 1, and that mode 2 excludes them, so a null clip draws nothing.
 * The model binds the union of the enabled rectangles in mode 1 and
 * nothing otherwise. What bounds a window of five or more rectangles
 * (0x10) is unknown; the model draws nothing for it and logs that, as it
 * does for any other mode. Only one window position has been captured.
 */
#define MGRAS_GL_IMAGE_ORIGIN    2
#define MGRAS_GL_IMAGE_CLIP_MODE 3
#define MGRAS_GL_IMAGE_CLIP_ID   4
#define MGRAS_GL_CLIP_RECTS      4
#define MGRAS_GL_CLIP_INSIDE     1
#define MGRAS_GL_CLIP_NULL       2

/* Each rectangle's column and row words, rectangle 0 first. */
static const uint8_t mgras_gl_clip_words[MGRAS_GL_CLIP_RECTS][2] = {
    { 11, 12 }, { 9, 10 }, { 7, 8 }, { 5, 6 },
};

/*
 * Words 16 and 17 of every captured GL context image are 1, those of the
 * X server's images 0 (a captured run). The kernel's
 * bank token 0x9808 carries two words of the same form, 0 at a window's
 * first swap and 1 at its second, and the swaps then show first the frame
 * drawn before the first swap and then the one drawn between them. Both
 * fit these words naming the pixel-memory bank that holds the back
 * buffer, with the front buffer in the other; that the image restores
 * them is inferred from the matching values. The model uses the first
 * word; what distinguishes the second is unknown.
 */
#define MGRAS_GL_IMAGE_BANKS     16

/*
 * Lighting, from the IRIX 6.5.30 GL client library's emitters: glLightfv
 * writes a header per parameter, the light's number (light - GL_LIGHT0) and
 * the parameter's floats, the application's values untransformed;
 * glLightModelfv writes the ambient's red, green and blue and not its alpha,
 * and the two booleans as integers; glMaterialfv writes the face and parameter
 * enumerants and always four floats; glColorMaterial writes one header per
 * face with the parameter, and glEnable and glDisable of GL_COLOR_MATERIAL one
 * word each. glGetLightfv and glGetMaterialfv write 0xef08 with the light's
 * number and parameter or 0xee08 with the face and parameter, then query 0x3ba
 * to 0x3bd and read four floats, keeping as many as the parameter has; the
 * index does not depend on the light or face, so the board puts the parameter
 * in one place for the queries (inferred). The model keeps the state in the
 * shared context, which transforms positions and spot directions by the
 * modelview when they arrive, and answers from it.
 *
 * The enables, from the enable routine and the disable routine,
 * established from the listing:
 *
 * - glEnable(GL_LIGHTING) sets bit 0x40 of the context's flags word and
 *   calls the lighting enable routine, which writes 0x0d00 alone while
 *   that bit is set; the probe's lighting-only case captured 0x0d00 and
 *   nothing else. glDisable(GL_LIGHTING) writes 0x0e00 through the
 *   lighting disable routine, and the soft hardware-state routine writes
 *   it as a header too. The other 0x0e00 words in the listing are
 *   payloads of 0x000c0004 descriptor headers.
 * - glEnable(GL_LIGHTi) writes 0x7104 and i, the capability less
 *   GL_LIGHT0 (through the light-source enable routine), and glDisable
 *   0xa004 and i.
 * - glEnable(GL_NORMALIZE) writes 0xa304 and 1, glDisable 0xa304 and 0.
 *   GL_RESCALE_NORMAL is GL_INVALID_ENUM to this client, which sends
 *   nothing for it.
 *
 * That the board lights each vertex as OpenGL 1.2.1, 2.13.1 gives while
 * these are enabled is inferred from the specification. The shared
 * context evaluates the lighting equation in single-precision floats,
 * which approximates the geometry engine's own arithmetic: a real
 * board's pixels for a lit primitive would settle how closely. The
 * discriminating tests are gl-lit-triangle-matches-the-equation,
 * gl-light-enables-select-the-lights, gl-normalize-enable,
 * gl-normal-forms-convert and gl-two-sided-lighting-lights-the-back.
 */
#define MGRAS_GL_LIGHT_AMBIENT   0x00001214
#define MGRAS_GL_LIGHT_DIFFUSE   0x00001314
#define MGRAS_GL_LIGHT_SPECULAR  0x00001414
#define MGRAS_GL_LIGHT_POSITION  0x00004f14
#define MGRAS_GL_LIGHT_SPOT_DIR  0x00005010
#define MGRAS_GL_LIGHT_SPOT_EXP  0x00005108
#define MGRAS_GL_LIGHT_SPOT_CUT  0x00005208
#define MGRAS_GL_LIGHT_CONST_ATT 0x00005308
#define MGRAS_GL_LIGHT_LIN_ATT   0x00005408
#define MGRAS_GL_LIGHT_QUAD_ATT  0x00005508
#define MGRAS_GL_LIGHT_MODEL_AMBIENT 0x0000570c
#define MGRAS_GL_LIGHT_MODEL_LOCAL 0x00005804
#define MGRAS_GL_LIGHT_MODEL_TWO_SIDE 0x00005904
#define MGRAS_GL_MATERIAL        0x00001118
#define MGRAS_GL_COLOR_MATERIAL_FRONT 0x0000bf04
#define MGRAS_GL_COLOR_MATERIAL_BACK 0x0000c004
#define MGRAS_GL_COLOR_MATERIAL_BOTH 0x0000c104
#define MGRAS_GL_COLOR_MATERIAL_ON 0x0000c200
#define MGRAS_GL_COLOR_MATERIAL_OFF 0x0000c300
#define MGRAS_GL_LIGHTING_ON     0x00000d00
#define MGRAS_GL_LIGHTING_OFF    0x00000e00
#define MGRAS_GL_LIGHT_ON        0x00007104
#define MGRAS_GL_LIGHT_OFF       0x0000a004
#define MGRAS_GL_NORMALIZE       0x0000a304
#define MGRAS_GL_GET_LIGHT       0x0000ef08
#define MGRAS_GL_GET_MATERIAL    0x0000ee08

/*
 * IRIS GL's colour and normal rule: a vertex whose colour came after the
 * last normal is not lit (SGI's OpenGL Porting Guide, 007-1797-030,
 * appendix B, "Color/normal flag"). The IRIX 6.5.30 GL client
 * library (SHA-256 626d6d94...) leaves it to the board. Its
 * glIglooInterfaceSGIX, established from the listing:
 *
 * - GL_IGLOO_COLORNORMAL_SGIX (0x81a1) with a nonzero parameter sets
 *   context flag bit 1 and writes 0x00010200 when bit 0 is set; with 0 it
 *   clears bit 1 and writes 0x0000df00.
 * - GL_IGLOO_LMC_COLOR_SGIX (0x81a3) calls a helper with the parameter.
 *   With 1 that sets bit 0 and writes 0x0000dd00, then 0x00010200 when
 *   bit 1 is set and 0x0000df00 when it is not. With anything else it
 *   clears bit 0 and writes 0x0000de00 or 0x0000df00, by a flag the
 *   listing reads. It writes nothing while the context byte
 *   that marks an RGBA context (inferred, as for the logical
 *   operation above), is clear.
 *
 * So 0x00010200 arrives exactly when both are on. The library sends it
 * again after 0x0000dd00 when its bit 1 stayed set through a 0x0000de00,
 * so the board has dropped the rule by then. The model reads 0x00010200
 * as turning the rule on and 0x0000df00 and 0x0000de00 as turning it off;
 * 0x0000dd00 changes nothing by itself. Inferred from the library: the
 * board's meaning of each word, and what else 0x0000dd00 and 0x0000de00
 * do, are unknown.
 *
 * While the rule is on and lighting is enabled in an RGBA context, the
 * model lights a vertex whose colour came after the last normal with
 * ambient, diffuse and specular (0, 0, 0, 1) on both faces and the
 * current colour as both emissions, then restores the materials. That
 * is what the Octane2 VPro library does in software for the same rule
 * (the Origami odyssey datasheet, "IRIS GL colour after a normal").
 * Approximated: how the GE carries out the rule is unknown; the two
 * differ from leaving the vertex unlit only in its alpha, which the wrap
 * makes 1. A colour-index context is lit as before: what the rule does
 * to lit colour indices is unknown. The discriminating test is
 * gl-iris-colour-after-normal-is-unlit.
 */
/*
 * Texture coordinate generation, from the IRIX 6.5.30 GL client
 * library's glTexGenfv, established from the listing:
 *
 * - GL_TEXTURE_GEN_MODE writes 0x5a0c, the coordinate (GL_S to GL_Q,
 *   0x2000 to 0x2003), 0x2500 and the mode's enumerant as an integer;
 *   GL_SPHERE_MAP for R or Q is refused there.
 * - GL_OBJECT_PLANE and GL_EYE_PLANE write 0x5b08, the coordinate and the
 *   parameter (0x2501 or 0x2502), then 0x5c10 and the plane's four
 *   floats in order; glTexGeniv's integers come under 0x18005c10. The
 *   eye plane goes as the application gave it, so the board transforms
 *   it by the modelview's inverse (inferred: the client multiplies
 *   nothing, and OpenGL 1.2.1, 2.10.4 defines the eye plane that way).
 * - glEnable and glDisable of GL_TEXTURE_GEN_S to Q write 0x7504 or 0x7604
 *   and the capability, on a board with texture (from the client; 0x7604
 *   with 0x0c60 to 0x0c63 traced from pfCity on SI with texture). Other
 *   capabilities under these two words are logged.
 *
 * The model keeps the generation state in the shared context, which
 * generates as OpenGL 1.2.1, 2.10.4 gives (approximated: the geometry
 * engine's arithmetic is unknown). A 0x5c10 with no 0x5b08 before it is
 * logged. The discriminating test is gl-texture-generation.
 */
#define MGRAS_GL_TEX_GEN_MODE    0x00005a0c
#define MGRAS_GL_TEX_GEN_PLANE   0x00005b08
#define MGRAS_GL_TEX_GEN_PLANE_F 0x00005c10
#define MGRAS_GL_TEX_GEN_PLANE_I 0x18005c10
#define MGRAS_GL_ENABLE_CAP      0x00007504
#define MGRAS_GL_DISABLE_CAP     0x00007604

#define MGRAS_GL_IRIS_LMC_COLOR  0x0000dd00
#define MGRAS_GL_IRIS_LMC_OTHER  0x0000de00
#define MGRAS_GL_IRIS_RULE_OFF   0x0000df00
#define MGRAS_GL_IRIS_RULE_ON    0x00010200
#define MGRAS_GL_QUERY_READBACK  0x3ba

/* The light parameter each glLightfv header carries. */
static const struct {
    uint32_t header;
    uint16_t pname;
} mgras_gl_light_headers[] = {
    { MGRAS_GL_LIGHT_AMBIENT, SGI_GL_AMBIENT },
    { MGRAS_GL_LIGHT_DIFFUSE, SGI_GL_DIFFUSE },
    { MGRAS_GL_LIGHT_SPECULAR, SGI_GL_SPECULAR },
    { MGRAS_GL_LIGHT_POSITION, SGI_GL_POSITION },
    { MGRAS_GL_LIGHT_SPOT_DIR, SGI_GL_SPOT_DIRECTION },
    { MGRAS_GL_LIGHT_SPOT_EXP, SGI_GL_SPOT_EXPONENT },
    { MGRAS_GL_LIGHT_SPOT_CUT, SGI_GL_SPOT_CUTOFF },
    { MGRAS_GL_LIGHT_CONST_ATT, SGI_GL_CONSTANT_ATTENUATION },
    { MGRAS_GL_LIGHT_LIN_ATT, SGI_GL_LINEAR_ATTENUATION },
    { MGRAS_GL_LIGHT_QUAD_ATT, SGI_GL_QUADRATIC_ATTENUATION },
};

/* The begin token of the mode a glBegin or glEnd token names, or 0. */
static uint32_t mgras_gl_begin_token(uint32_t header, bool end)
{
    uint32_t begin = end ? header - MGRAS_GL_END_OFFSET : header;

    if (begin < MGRAS_GL_BEGIN_POINTS || begin > MGRAS_GL_BEGIN_POLYGON ||
        (begin & 0xff)) {
        return 0;
    }
    return begin;
}

/* The GL primitive a begin token opens, and the token that opens one. */
static const uint32_t mgras_gl_begin_tokens[] = {
    [SGI_GL_POINTS] = MGRAS_GL_BEGIN_POINTS,
    [SGI_GL_LINES] = MGRAS_GL_BEGIN_LINES,
    [SGI_GL_LINE_LOOP] = MGRAS_GL_BEGIN_LINE_LOOP,
    [SGI_GL_LINE_STRIP] = MGRAS_GL_BEGIN_LINE_STRIP,
    [SGI_GL_TRIANGLES] = MGRAS_GL_BEGIN_TRIANGLES,
    [SGI_GL_TRIANGLE_STRIP] = MGRAS_GL_BEGIN_TRI_STRIP,
    [SGI_GL_TRIANGLE_FAN] = MGRAS_GL_BEGIN_TRI_FAN,
    [SGI_GL_QUADS] = MGRAS_GL_BEGIN_QUADS,
    [SGI_GL_QUAD_STRIP] = MGRAS_GL_BEGIN_QUAD_STRIP,
    [SGI_GL_POLYGON] = MGRAS_GL_BEGIN_POLYGON,
};

static uint32_t mgras_gl_mode_of_begin(uint32_t begin)
{
    for (uint32_t mode = 0; mode < ARRAY_SIZE(mgras_gl_begin_tokens);
         mode++) {
        if (mgras_gl_begin_tokens[mode] == begin) {
            return mode;
        }
    }
    g_assert_not_reached();
}

bool sgi_mgras_gl_header(uint32_t header)
{
    header = (header & ~0xffu) |
             (DIV_ROUND_UP(header & 0xff, sizeof(uint32_t)) *
              sizeof(uint32_t));
    if (mgras_gl_begin_token(header, false) ||
        mgras_gl_begin_token(header, true)) {
        return true;
    }
    for (int i = 0; i < ARRAY_SIZE(mgras_gl_light_headers); i++) {
        if (header == mgras_gl_light_headers[i].header) {
            return true;
        }
    }
    if (sgi_mgras_texture_header(header)) {
        return true;
    }
    switch (header) {
    case MGRAS_GL_POLYGON_STIPPLE_UPLOAD:
        return true;
    case MGRAS_GL_IRIS_LMC_COLOR:
    case MGRAS_GL_IRIS_LMC_OTHER:
    case MGRAS_GL_IRIS_RULE_OFF:
    case MGRAS_GL_IRIS_RULE_ON:
    case MGRAS_GL_TEX_GEN_MODE:
    case MGRAS_GL_TEX_GEN_PLANE:
    case MGRAS_GL_TEX_GEN_PLANE_F:
    case MGRAS_GL_TEX_GEN_PLANE_I:
    case MGRAS_GL_ENABLE_CAP:
    case MGRAS_GL_DISABLE_CAP:
    case MGRAS_GL_CONTEXT_OPENER:
    case MGRAS_GL_RENDER_SELECT:
    case MGRAS_GL_RENDER:
    case MGRAS_GL_INIT_NAMES:
    case MGRAS_GL_LOAD_NAME:
    case MGRAS_GL_PUSH_NAME:
    case MGRAS_GL_POP_NAME:
    case MGRAS_GL_RE_COUNT:
    case MGRAS_GL_CLEAR_COLOR_BUF:
    case MGRAS_GL_CLEAR_DEPTH_BUF:
    case MGRAS_GL_VIEWPORT:
    case MGRAS_GL_FLUSH:
    case MGRAS_GL_SWAP_TMESH:
    case MGRAS_GL_SHADE_MODEL:
    case MGRAS_GL_CLEAR_COLOR:
    case MGRAS_GL_CLEAR_DEPTH:
    case MGRAS_GL_MATRIX_MODE:
    case MGRAS_GL_LOAD_MATRIX:
    case MGRAS_GL_LOAD_IDENTITY:
    case MGRAS_GL_MULT_MATRIX:
    case MGRAS_GL_FRUSTUM:
    case MGRAS_GL_ORTHO:
    case MGRAS_GL_POP_MATRIX:
    case MGRAS_GL_PUSH_MATRIX:
    case MGRAS_GL_ROTATE:
    case MGRAS_GL_SCALE:
    case MGRAS_GL_TRANSLATE:
    case MGRAS_GL_DEPTH_MASK:
    case MGRAS_GL_DEPTH_FUNC:
    case MGRAS_GL_DRAW_BUFFER:
    case MGRAS_GL_DEPTH_TEST:
    case MGRAS_GL_SCISSOR:
    case MGRAS_GL_SCISSOR_TEST:
    case MGRAS_GL_ALPHA_FUNC:
    case MGRAS_GL_ALPHA_TEST:
    case MGRAS_GL_STENCIL_FUNC:
    case MGRAS_GL_STENCIL_OP:
    case MGRAS_GL_STENCIL_MASK:
    case MGRAS_GL_CLEAR_STENCIL:
    case MGRAS_GL_CLEAR_STENCIL_BUF:
    case MGRAS_GL_STENCIL_TEST:
    case MGRAS_GL_BLEND_FUNC:
    case MGRAS_GL_BLEND_EQUATION:
    case MGRAS_GL_BLEND_COLOR:
    case MGRAS_GL_BLEND:
    case MGRAS_GL_LOGIC_OP:
    case MGRAS_GL_LOGIC_OP_ENABLE:
    case MGRAS_GL_COLOR_MASK:
    case MGRAS_GL_FOG_COLOR:
    case MGRAS_GL_FOG_START:
    case MGRAS_GL_FOG_END:
    case MGRAS_GL_FOG_DENSITY:
    case MGRAS_GL_FOG_INDEX:
    case MGRAS_GL_FOG_MODE:
    case MGRAS_GL_FOG:
    case MGRAS_GL_POLYGON_OFFSET:
    case MGRAS_GL_OFFSET_ENABLES:
    case MGRAS_GL_CLIP_PLANE:
    case MGRAS_GL_CLIP_PLANE_ON:
    case MGRAS_GL_CLIP_PLANE_OFF:
    case MGRAS_GL_LINE_WIDTH:
    case MGRAS_GL_POINT_SIZE:
    case MGRAS_GL_LINE_STIPPLE:
    case MGRAS_GL_POLYGON_MODE:
    case MGRAS_GL_LINE_SMOOTH:
    case MGRAS_GL_LINE_STIPPLE_ON:
    case MGRAS_GL_POINT_SMOOTH:
    case MGRAS_GL_POLYGON_STIPPLE:
    case MGRAS_GL_MAP_GRID1:
    case MGRAS_GL_MAP_GRID2:
    case MGRAS_GL_CLEAR_ACCUM:
    case MGRAS_GL_DEPTH_RANGE:
    case MGRAS_GL_DITHER:
    case MGRAS_GL_CULL_ON:
    case MGRAS_GL_CULL_OFF:
    case MGRAS_GL_CULL_FACE:
    case MGRAS_GL_FRONT_FACE:
    case MGRAS_GL_TEXTURE_TABLE:
    case MGRAS_GL_LIGHT_MODEL_AMBIENT:
    case MGRAS_GL_LIGHT_MODEL_LOCAL:
    case MGRAS_GL_LIGHT_MODEL_TWO_SIDE:
    case MGRAS_GL_MATERIAL:
    case MGRAS_GL_COLOR_MATERIAL_FRONT:
    case MGRAS_GL_COLOR_MATERIAL_BACK:
    case MGRAS_GL_COLOR_MATERIAL_BOTH:
    case MGRAS_GL_COLOR_MATERIAL_ON:
    case MGRAS_GL_COLOR_MATERIAL_OFF:
    case MGRAS_GL_LIGHTING_ON:
    case MGRAS_GL_LIGHTING_OFF:
    case MGRAS_GL_LIGHT_ON:
    case MGRAS_GL_LIGHT_OFF:
    case MGRAS_GL_NORMALIZE:
    case MGRAS_GL_GET_LIGHT:
    case MGRAS_GL_GET_MATERIAL:
    case MGRAS_GL_BANKS:
    case MGRAS_GL_ERAM_READ:
    case MGRAS_GL_ERAM_COUNT:
    case MGRAS_GL_ERAM_WRITE:
    case MGRAS_GL_ERAM_END:
    case MGRAS_GL_VERTEX2F:
    case MGRAS_GL_VERTEX3F:
    case MGRAS_GL_VERTEX4F:
    case MGRAS_GL_COLOR3F:
    case MGRAS_GL_COLOR4F:
    case MGRAS_GL_VERTEX2I:
    case MGRAS_GL_VERTEX3I:
    case MGRAS_GL_VERTEX2D:
    case MGRAS_GL_VERTEX3D:
    case MGRAS_GL_VERTEX4D:
    case MGRAS_GL_RASTER_POS3I:
    case MGRAS_GL_TEX_COORD3F:
    case MGRAS_GL_TEX_COORD3I:
    case MGRAS_GL_TEX_COORD2F:
    case MGRAS_GL_TEX_COORD4F:
    case MGRAS_GL_COLOR3UB:
    case MGRAS_GL_COLOR4UB:
    case MGRAS_GL_NORMAL3F:
    case MGRAS_GL_NORMAL3D:
    case MGRAS_GL_NORMAL3I:
    case MGRAS_GL_NORMAL3S:
    case MGRAS_GL_NORMAL3B:
    case MGRAS_GL_EDGE_FLAG_TRUE:
    case MGRAS_GL_EDGE_FLAG_FALSE:
    case MGRAS_GL_RASTER_POS2F:
    case MGRAS_GL_RASTER_POS3F:
    case MGRAS_GL_INIT_CI:
    case MGRAS_GL_INIT_RGB:
    case MGRAS_GL_INDEXS:
    case MGRAS_GL_INDEXI:
    case MGRAS_GL_INDEXF:
    case MGRAS_GL_INDEX_MASK:
    case MGRAS_GL_CLEAR_INDEX:
    case MGRAS_GL_BITMAP:
    case MGRAS_GL_MOVE_RASTER:
    case MGRAS_GL_PIXELS:
    case MGRAS_GL_RASTER_POS2I:
    case MGRAS_GL_SAVE_RSS:
    case MGRAS_GL_RESTORE_RSS:
    case MGRAS_GL_LOAD_RASTER_INFO:
    case MGRAS_GL_SAVE_TEXMODE:
    case MGRAS_GL_RESTORE_TEXMODE:
    case MGRAS_GL_RSS_PAIRS:
    case MGRAS_GL_READ_OPEN:
    case MGRAS_GL_READ_CLOSE:
    case MGRAS_GL_READ_SETUP:
    case MGRAS_GL_READ_BUFFER:
    case MGRAS_GL_READ_PIO:
    case MGRAS_GL_READ_DMA_DONE:
        return true;
    default:
        return false;
    }
}

/*
 * glColor3ub's and glColor4ub's bytes in one word, the first component in
 * bits 31:24: colorbars' bars (a captured run, from seq
 * 26371573) are 0x87878700, 0x87870000, 0x00878700, 0x00870000,
 * 0x87008700 and 0x87000000, white, yellow, cyan, green, magenta and red.
 * Alpha in bits 7:0 follows from that component order; noof's constant
 * glColor4ub word 0x00000060 is consistent with it but cannot fix it
 * alone.
 */
static void mgras_gl_color_bytes(uint32_t bytes[4], uint32_t word)
{
    for (int i = 0; i < 4; i++) {
        bytes[i] = (word >> (24 - 8 * i)) & 0xff;
    }
}

/*
 * Context image word 0 names the context's GL state. In a captured run it is
 * 0x3bcd and 0x5420 for two GL clients, 0x237a for the X server and 0xb27 for
 * the kernel's default context. These are allocations observed in that run,
 * not published values, and MGRAS_GL_DEFAULT_SLOT is the one observed there.
 * The IRIX 6.5.30 kernel allocates the word as the context's slot in GE
 * external RAM and writes -1 for a context with no GL state. The image
 * carries no other GL state, the clients never re-send theirs after a
 * switch, and nothing touches the GE ports at those switches, so the board
 * keeps each context's state under its slot.
 *
 * Approximated: the model keys its state by the slot address the kernel
 * chose, not by GE external RAM contents, and never interprets the GE
 * ports. When the kernel runs out of room it reads a context out to host
 * memory and later writes it back, possibly at another address; the model
 * follows that by host address ("Context eviction" below). The model
 * holds 32 slots and replaces the least recently loaded when a 33rd
 * arrives. The discriminating tests are gl-state-follows-the-context-slot,
 * gl-context-opener-resets-only-its-context and
 * gl-context-table-replaces-the-least-recent.
 */
#define MGRAS_GL_IMAGE_SLOT      0

/*
 * Image word 1 has both of bits 31 and 30 set only on the first delivery
 * of a new context. The IRIX 6.5.30 kernel's context allocator
 * (in unix.IP27) zeroes the new context, sets image word
 * 0 to -1 and ORs 0xc0000000 into word 1. After writing the image
 * the context-switch routine clears bit 27 of word 1 for every context and bits
 * 31, 30, 29 and 26 for every context but the default, whose image keeps bit 31
 * (established from the client's code). In a captured run each of the 36
 * openers the clients sent follows exactly one image with both bits in its
 * slot since that slot's previous opener, and the default context's images
 * carry 0x90000000 or 0x98000000.
 *
 * The kernel gives a dead client's slot to the next new context, so the
 * entry kept under that slot can hold a packet the dead client never
 * finished. In that run ivview was killed two words into a glVertex3f in
 * slot 0x5420, and the model took the opener and 0x8c04 of amesh's next
 * context, new under 0x5420, as the vertex's last two words. A new
 * context's words start at a header and its GL state starts at OpenGL's
 * initial state (inferred: the client's setup assumes both, and the board
 * cannot resume a dead client's packet for a new one). What the board does
 * with each bit is unknown.
 *
 * A new context whose word 0 is still -1 would share entry 0 with the
 * default context and every other context without a slot. That entry is
 * not started afresh, so a packet the default left open survives. Neither
 * captured run has such an image, and the kernel paths that switch a
 * context in were not all traced to rule one out; a new slotless context
 * therefore keeps whatever entry 0 holds (approximated).
 *
 * Both bits are required, not bit 31 alone: no captured slotted image
 * has bit 31 without bit 30, so this is the model's choice of the
 * narrower rule.
 *
 * The discriminating tests are gl-new-context-starts-at-a-header,
 * gl-new-context-starts-in-the-initial-state,
 * gl-bit-31-alone-keeps-the-packet,
 * gl-new-context-without-a-slot-keeps-the-default and
 * gl-default-context-keeps-its-packet.
 */
#define MGRAS_GL_IMAGE_STATUS    1
#define MGRAS_GL_IMAGE_NEW       0xc0000000u
#define MGRAS_GL_NO_SLOT         0x80000000u
#define MGRAS_GL_DEFAULT_SLOT    0xb27

static SGIGLContext *mgras_gl(SGIMgrasState *s)
{
    return s->gl_context[s->gl_current].gl;
}

/*
 * The loaded context image's window and the banks the image or the bank
 * token last named, as the drawable the loaded GL context draws into.
 * MGRAS's colour targets are masks of pixel-memory banks: bank 0, the
 * model's front[] plane and the one the X server draws in, and bank 1,
 * back[]. Only mgras_gl_store_colour() reads them. The window's pixels
 * are the enabled rectangles of a mode-1 clip word that are not empty on
 * the screen. Mode 1 remains bound when that union is empty, so geometric
 * stipple positions advance while every fragment is rejected.
 *
 * The drawable is bound when a context image completes, and at the other
 * points that change it, not read from the image at each draw. The two
 * differ only after a guest protocol error: mgras_error() can end a
 * context transaction partway through the image, with the port still
 * ready, and GL words that follow draw into the window bound at the last
 * completed switch, where a per-draw read would take the partly written
 * image's. A migration inside such a transaction rebinds from the partly
 * written image, since that is what the stream carries, so the two sides
 * can then draw into different windows. The board's behaviour after such
 * an error is unknown either way.
 */
/*
 * The overlay planes as a colour target, a bit beside the bank and slot
 * bits. The IRIX 6.5.30 GL client library's draw-buffer routine
 * sends 0x48 as the triple's first word
 * for GL_FRONT, and for GL_BACK on a shallow visual (a deep visual's
 * GL_BACK sends 4 first), and 0x43 for GL_FRONT_AND_BACK, when a flag in
 * its context is set, and 1 to 0xf otherwise; established
 * from that listing. SceneViewer's logo context, the only context of that
 * program's overlay scene graph (SoXtRenderArea::setOverlaySceneGraph on
 * a level-1 colour-index visual; it sets glViewport(0, 0, 80, 80) and
 * draws index 1 lines), sends 0x48, and
 * the other programs that send it, ivview and pointblank, also draw in
 * the overlay. That bit 6 names the overlay planes is inferred from those
 * observations; what bits 3:0 add beside it is unknown, and the model
 * draws such a word into the overlay alone.
 *
 * Overlay index stores pass the same ownership test as the main planes:
 * the drawable's clip rectangles and the main planes' two-bit clip ID
 * under the context's accept mask (mgras_gl_owns()). Whether the board
 * gates overlay writes by the main planes' clip ID is not established;
 * gating them so is a model choice.
 */
#define MGRAS_GL_OVERLAY_TARGET  0x10

static void mgras_gl_bind(SGIMgrasState *s)
{
    uint32_t origin = s->context[MGRAS_GL_IMAGE_ORIGIN];
    uint32_t clip = s->context[MGRAS_GL_IMAGE_CLIP_MODE];
    uint32_t mode = clip >> 4;
    unsigned int back = s->gl_banks[0] & 1;
    SGIGLDrawable d = {
        .origin = { sextract32(origin, 0, 16), sextract32(origin, 16, 16) },
        .front = 1u << (back ^ 1),
        .back = 1u << back,
        /*
         * Draw-buffer bits 2 and 3 name colour buffers beyond the two
         * banks where the board has slots; a stereo visual sends bit 3 too
         * and nothing here is known of it. A deep double-buffered visual
         * sends FRONT as (1, 4) and BACK as (4, 1), so its pair is bank 0
         * and slot 2 and bit 2 is its back buffer, which the bank token
         * places as it does a shallow back buffer: token 1 names slot 2
         * (the pair's second member), token 0 bank 0. The kernel's
         * captured swaps flip the select bit in step with the token, and
         * select bit set shows the second member (see sgi-mgras-display.c).
         * Inferred from the client's triples and that capture; the
         * hardware's own mapping is not published. Limitation: FRONT
         * (bit 0) on a deep visual still goes to the shallow front bank,
         * which is bank 0 only while the token is 1.
         */
        .extra = { s->slot[0] ? (back ? 4 : 1) : 0, s->slot[1] ? 8 : 0 },
        .overlay = MGRAS_GL_OVERLAY_TARGET,
    };

    for (int i = 0; mode == MGRAS_GL_CLIP_INSIDE && i < MGRAS_GL_CLIP_RECTS;
         i++) {
        uint32_t x = s->context[mgras_gl_clip_words[i][0]];
        uint32_t y = s->context[mgras_gl_clip_words[i][1]];
        SGIClipRect r = {
            .x0 = x >> 16,
            .y0 = y >> 16,
            .x1 = MIN(x & 0xffff, s->width - 1),
            .y1 = MIN(y & 0xffff, s->height - 1),
        };

        if ((clip & (1u << i)) && r.x0 <= r.x1 && r.y0 <= r.y1) {
            d.clip.r[d.clip.n++] = r;
        }
    }
    d.bound = mode == MGRAS_GL_CLIP_INSIDE;
    if ((mode == MGRAS_GL_CLIP_INSIDE && !(clip & 0xf)) ||
        (mode != MGRAS_GL_CLIP_INSIDE && clip != 0 &&
         mode != MGRAS_GL_CLIP_NULL)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: context image clip word 0x%x "
                      "has unsupported bounds; no fragments are stored\n",
                      clip);
    }
    trace_sgi_mgras_gl_bind(s->context[MGRAS_GL_IMAGE_SLOT], origin, clip,
                            s->context[11], s->context[12], s->context[9],
                            s->context[10], d.bound);
    sgi_gl_set_drawable(mgras_gl(s), &d);
}

static SGIGLSink mgras_gl_sink(SGIMgrasState *s);
static void mgras_gl_test_get_resource(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp);
static void mgras_gl_test_set_resource(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp);

static void mgras_gl_test_get_failed(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = s->gl_renderer_unavailable;

    visit_type_uint32(v, name, &value, errp);
}

static void mgras_gl_test_get_validated(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = s->gl_render_validated;

    visit_type_uint32(v, name, &value, errp);
}

/* The current context's retained shared GL error, for host-only qtests. */
static void mgras_gl_test_get_draw_error(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = mgras_gl(s)->polygon.error;

    visit_type_uint32(v, name, &value, errp);
}

static void mgras_gl_get_fallbacks(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint64_t total = 0;

    for (unsigned i = 0; i < SGI_RENDER_REFUSAL__MAX; i++) {
        total += s->gl_mesa_fallbacks[i];
    }
    /* A board drawing without Mesa is never a zero-fallback session. */
    total += s->gl_renderer_scalar;
    visit_type_uint64(v, name, &total, errp);
}

static void mgras_gl_get_unavailable(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    uint32_t value = s->gl_renderer_scalar;

    visit_type_uint32(v, name, &value, errp);
}

/*
 * Host diagnostics for measuring what the opt-in Mesa path still refuses:
 * one read-only count per refusal reason and their sum, which also counts
 * a board without Mesa on its host once. They count for the life of the
 * process, across resets, and are not migrated.
 */
static void mgras_gl_init_fallbacks(SGIMgrasState *s)
{
    for (unsigned i = SGI_RENDER_REFUSAL_NONE + 1;
         i < SGI_RENDER_REFUSAL__MAX; i++) {
        g_autofree char *name = g_strdup_printf(
            "x-mesa-fallback-%s", sgi_render_refusal_name(i));

        object_property_add_uint64_ptr(OBJECT(s), name,
                                       &s->gl_mesa_fallbacks[i],
                                       OBJ_PROP_FLAG_READ);
    }
    object_property_add(OBJECT(s), "x-mesa-fallbacks", "uint64",
                        mgras_gl_get_fallbacks, NULL, NULL, NULL);
}

void sgi_mgras_gl_init(SGIMgrasState *s)
{
    mgras_gl_init_fallbacks(s);
    object_property_add(OBJECT(s), "x-mesa-fallback-unavailable", "uint32",
                        mgras_gl_get_unavailable, NULL, NULL, NULL);
    object_property_add(OBJECT(s), "x-test-render-validated", "uint32",
                        mgras_gl_test_get_validated, NULL, NULL, NULL);
    object_property_add(OBJECT(s), "x-test-gl-draw-error", "uint32",
                        mgras_gl_test_get_draw_error, NULL, NULL, NULL);
    object_property_add_uint64_ptr(OBJECT(s), "x-test-render-snapshots",
                                   &s->gl_render_snapshots, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(OBJECT(s), "x-test-render-commits",
                                   &s->gl_render_commits, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(OBJECT(s), "x-test-render-result",
                                   &s->gl_render_result, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(OBJECT(s), "x-test-render-inject-result",
                                   &s->gl_render_inject_result,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(OBJECT(s), "x-test-scalar-inject-oom",
                                   &s->gl_scalar_inject_oom,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(OBJECT(s),
                                   "x-test-render-inject-commit-failure",
                                   &s->gl_render_inject_commit_failure,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(OBJECT(s), "x-test-render-failed", "uint32",
                        mgras_gl_test_get_failed, NULL, NULL, NULL);
    object_property_add(OBJECT(s), "x-test-render-resource-pixel", "uint32",
                        mgras_gl_test_get_resource,
                        mgras_gl_test_set_resource, NULL, NULL);
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        sgi_gl_init(&s->gl[i], 0);
        s->gl_context[i].gl = &s->gl[i];
    }
}

/*
 * Only a host without Mesa lets an opt-in board run without it, drawing
 * every draw on the scalar path and recording that once. Running out of
 * memory or a host error while creating the renderer fails realization,
 * so a board that asked for Mesa never silently draws without it.
 */
bool sgi_mgras_gl_realize(SGIMgrasState *s, Error **errp)
{
    SGIRenderCreateResult result;
    Error *err = NULL;

    if (!s->gl_renderer_opt_in) {
        return true;
    }
    if (s->gl_render_create_fault) {
        result = s->gl_render_create_fault;
        error_setg(&err, "injected creation result %u", result);
    } else {
        result = sgi_renderer_create(&s->gl_renderer, &err);
    }
    switch (result) {
    case SGI_RENDER_CREATE_OK:
        return true;
    case SGI_RENDER_CREATE_UNAVAILABLE:
        s->gl_renderer_scalar = true;
        warn_report("sgi-mgras: Mesa renderer unavailable; every draw "
                    "takes the scalar path: %s", error_get_pretty(err));
        error_free(err);
        return true;
    default:
        if (!err) {
            error_setg(&err, "creation failed");
        }
        error_propagate_prepend(errp, err, "sgi-mgras: Mesa renderer: ");
        return false;
    }
}

void sgi_mgras_gl_cleanup(SGIMgrasState *s)
{
    sgi_renderer_free(s->gl_renderer);
    s->gl_renderer = NULL;
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        sgi_gl_cleanup(&s->gl[i]);
        sgi_mgras_texture_reset(&s->gl_context[i].tex);
    }
}

void sgi_mgras_gl_reset(SGIMgrasState *s)
{
    SGIGLSink sink = mgras_gl_sink(s);

    /* The renderer holds no guest state; it lives as long as the board. */
    s->gl_render_validated = false;
    s->gl_render_snapshots = 0;
    s->gl_render_commits = 0;
    if (!s->gl_renderer_unavailable) {
        s->gl_render_result = SGI_GL_DRAW_OK;
    }
    s->gl_render_inject_result = 0;
    s->gl_render_inject_commit_failure = 0;
    s->gl_scalar_inject_oom = 0;
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        s->gl_context[i].gl = &s->gl[i];
        memset(&s->gl_context[i].parser, 0, sizeof(s->gl_context[i].parser));
        s->gl_context[i].client_read_bracket = 0;
        s->gl_context[i].client_read_kicked = 0;
        s->gl_context[i].client_read_bank = 0;
        memset(s->gl_context[i].readback, 0,
               sizeof(s->gl_context[i].readback));
        memset(s->gl_context[i].clip_query, 0,
               sizeof(s->gl_context[i].clip_query));
        s->gl_context[i].iris_rule = 0;
        s->gl_context[i].iris_colour_last = 0;
        s->gl_context[i].tex_gen_plane_open = 0;
        memset(s->gl_context[i].tex_gen_plane, 0,
               sizeof(s->gl_context[i].tex_gen_plane));
        memset(&s->gl_context[i].pixels, 0,
               sizeof(s->gl_context[i].pixels));
        memset(&s->gl_selection[i], 0, sizeof(s->gl_selection[i]));
        sgi_mgras_texture_reset(&s->gl_context[i].tex);
        sgi_gl_reset(&s->gl[i], 0);
        if (!sgi_gl_enable_polygon_modes(&s->gl[i], &sink) ||
            !sgi_gl_enable_line_stipple(&s->gl[i], &sink)) {
            g_assert_not_reached();
        }
    }
    s->gl_oom_logged = false;
    memset(s->ge_param, 0, sizeof(s->ge_param));
    s->ge_param_logged = false;
    memset(s->gl_slot, 0, sizeof(s->gl_slot));
    memset(s->gl_selected, 0, sizeof(s->gl_selected));
    memset(s->gl_saved, 0, sizeof(s->gl_saved));
    memset(s->gl_is_saved, 0, sizeof(s->gl_is_saved));
    s->gl_selections = 0;
    s->gl_current = 0;
    s->gl_ge_port_logged = false;
    s->eram_state = MGRAS_ERAM_IDLE;
    s->eram_host = 0;
    s->eram_rows = s->eram_bpr = s->eram_next_addr = 0;
    /* The value every captured GL context image restores. */
    s->gl_banks[0] = s->gl_banks[1] = 1;
    mgras_gl_bind(s);
}

/*
 * Start entry i afresh for hw_id: the initial GL state, no packet in
 * progress, no query answer and no pixel draw.
 */
static void mgras_gl_forget(SGIMgrasState *s, unsigned int i, uint32_t hw_id)
{
    sgi_gl_reset(&s->gl[i], hw_id);
    s->gl_saved[i] = 0;
    s->gl_is_saved[i] = false;
    memset(&s->gl_context[i].parser, 0, sizeof(s->gl_context[i].parser));
    s->gl_context[i].client_read_bracket = 0;
    s->gl_context[i].client_read_kicked = 0;
    memset(s->gl_context[i].readback, 0, sizeof(s->gl_context[i].readback));
    memset(s->gl_context[i].clip_query, 0,
           sizeof(s->gl_context[i].clip_query));
    s->gl_context[i].iris_rule = 0;
    s->gl_context[i].iris_colour_last = 0;
    s->gl_context[i].tex_gen_plane_open = 0;
    memset(s->gl_context[i].tex_gen_plane, 0,
           sizeof(s->gl_context[i].tex_gen_plane));
    memset(&s->gl_context[i].pixels, 0, sizeof(s->gl_context[i].pixels));
    memset(&s->gl_selection[i], 0, sizeof(s->gl_selection[i]));
    sgi_mgras_texture_reset(&s->gl_context[i].tex);
}

/*
 * The entry holding slot's state, taking a new one if it has none. An
 * entry read out to host memory holds no slot until it is written back.
 */
static unsigned int mgras_gl_lookup(SGIMgrasState *s, uint32_t slot)
{
    unsigned int free = 0, oldest = 0;

    for (unsigned int i = 1; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (!s->gl_selected[i]) {
            free = free ? free : i;
        } else if (s->gl_slot[i] == slot && !s->gl_is_saved[i]) {
            return i;
        } else if (!oldest || s->gl_selected[i] < s->gl_selected[oldest]) {
            oldest = i;
        }
    }
    if (!free) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL context slot 0x%x replaces "
                      "slot 0x%x, loaded least recently; that context's GL "
                      "state is lost\n", slot, s->gl_slot[oldest]);
        free = oldest;
    }
    mgras_gl_forget(s, free, slot);
    s->gl_slot[free] = slot;
    return free;
}

/*
 * Context eviction. The IRIX 6.5.30 kernel keeps each GL context in GE
 * external RAM (ERAM) at the address its context image carries in word 0,
 * and moves contexts between ERAM and a host buffer when it needs room.
 * Established from the IRIX 6.5.30 kernel graphics driver (SHA-256
 * 6a65e70d3cdc...): the ERAM read routine builds a DMA read
 * descriptor, kicks it and then writes 0xfc08 with the ERAM address and a
 * word count, then 0x0100; the ERAM write routine sends the
 * image back in chunks of at most 0x384 words, each 0xfd04 with the count, the
 * descriptor and its kick, then 0xfb08 with the ERAM address and count,
 * and ends with 0xfa00.
 *
 * Established from a captured run (i3dm, seq 3906430 to
 * 3962650): three read-outs of 0x1854 words, from addresses 0x3bcd,
 * 0x84c6 and 0x9d19, each kicked 0xbd from DMA-map slot 2 to offsets
 * 0x9000, 0x21000 and 0x29000 of one kernel buffer; then one write-back,
 * kicked 0xa5, of the image at offset 0x29000 to address 0x5420 in seven
 * chunks of 0x384 words and one of 0x33b. Each address is the word 0 of
 * the context images loaded before and after, so the context that drew
 * i3dm's top view under 0x9d19 came back under 0x5420, whose earlier GL
 * state, the tool panel's, the model then drew the view with: its
 * 111-column scissor box clipped the view's clear, and the view stayed
 * stale.
 *
 * The model follows the move without the bytes: a read-out marks the
 * entry under the ERAM address as saved at the host address of the
 * transfer's first byte, and a write-back whose first byte comes from
 * that host address gives the entry the new ERAM address, dropping the
 * one the write overwrites there. Inferred: that the kernel only stores
 * and returns the buffer, since it writes back exactly what it read, from
 * the same place. Approximated: the model writes nothing into the host
 * buffer, where the board writes the context's ERAM image, and keeps the
 * board's GE parameters (ge_param) for the board, not per context. A
 * write-back from a buffer the model never saw read out leaves the
 * address's GL state as it was, logged. The discriminating tests are
 * gl-eviction-moves-the-context, gl-eviction-read-without-token-logs and
 * the replay fixture gl-eviction-i3dm.
 */
static void mgras_gl_eram_read(SGIMgrasState *s, uint32_t addr,
                               uint32_t words)
{
    if (s->eram_state != MGRAS_ERAM_READ_KICKED) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: ERAM read of %u words at "
                      "0x%x with no DMA read kicked before it\n", words,
                      addr);
        return;
    }
    s->eram_state = MGRAS_ERAM_IDLE;
    for (unsigned int i = 1; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (s->gl_selected[i] && !s->gl_is_saved[i] &&
            s->gl_slot[i] == addr) {
            s->gl_saved[i] = s->eram_host;
            s->gl_is_saved[i] = true;
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mgras: ERAM read of %u words at 0x%x "
                  "matches no GL context the model tracks; nothing is "
                  "saved for a later write-back\n", words, addr);
}

static void mgras_gl_eram_write(SGIMgrasState *s, uint32_t addr,
                                uint32_t words)
{
    uint64_t host = s->eram_host;
    bool inline_data = s->eram_state == MGRAS_ERAM_WRITE_INLINE;

    if (s->eram_state != MGRAS_ERAM_WRITE_KICKED && !inline_data) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: ERAM write of %u words "
                      "at 0x%x with no data before it\n", words, addr);
        return;
    }
    s->eram_state = MGRAS_ERAM_IDLE;
    /*
     * A later chunk of the image being written back follows the last in
     * ERAM; in host memory it may sit on a page the list does not put
     * next to the last one's.
     */
    if (s->eram_next_addr && addr == s->eram_next_addr) {
        s->eram_next_addr += words;
        return;
    }
    s->eram_next_addr = 0;
    if (inline_data) {
        /* A few words from the port itself: no context image moves. */
        return;
    }
    for (unsigned int i = 1; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (!s->gl_selected[i] || !s->gl_is_saved[i] ||
            s->gl_saved[i] != host) {
            continue;
        }
        for (unsigned int j = 1; j < SGI_MGRAS_GL_CONTEXTS; j++) {
            if (j != i && s->gl_selected[j] && !s->gl_is_saved[j] &&
                s->gl_slot[j] == addr) {
                mgras_gl_forget(s, j, 0);
                s->gl_selected[j] = 0;
                if (s->gl_current == j) {
                    s->gl_current = i;
                }
            }
        }
        s->gl_saved[i] = 0;
        s->gl_is_saved[i] = false;
        s->gl_slot[i] = addr;
        s->gl[i].hw_id = addr;
        s->eram_next_addr = addr + words;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mgras: ERAM write of %u words at 0x%x "
                  "from host 0x%" PRIx64 ", which the model saw no context "
                  "read out to; that address keeps its GL state\n", words,
                  addr, host);
}

void sgi_mgras_gl_eram_read_kicked(SGIMgrasState *s, uint64_t host,
                                   uint32_t rows, uint32_t bpr)
{
    s->eram_state = MGRAS_ERAM_READ_KICKED;
    s->eram_host = host;
    s->eram_rows = rows;
    s->eram_bpr = bpr;
}

bool sgi_mgras_gl_eram_write_armed(SGIMgrasState *s)
{
    return s->eram_state == MGRAS_ERAM_WRITE_ARMED;
}

void sgi_mgras_gl_eram_write_kicked(SGIMgrasState *s, uint64_t host)
{
    s->eram_state = MGRAS_ERAM_WRITE_KICKED;
    s->eram_host = host;
}

void sgi_mgras_gl_eram_header(SGIMgrasState *s, uint32_t header)
{
    switch (s->eram_state) {
    case MGRAS_ERAM_READ_KICKED:
        if (header != MGRAS_GL_ERAM_READ) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: DMA read of %u rows of %u "
                          "bytes kicked with no raster box that fits it "
                          "and no ERAM read after it; nothing transferred\n",
                          s->eram_rows, s->eram_bpr);
            s->eram_state = MGRAS_ERAM_IDLE;
        }
        break;
    case MGRAS_ERAM_WRITE_ARMED:
        /*
         * The write's descriptor and kick come between count and address,
         * or, for a few words, an inline block carries them instead
         * (0xfd04 1, an 8-byte block, 0xfb08 at 0xb26 in a captured run).
         */
        if (header & MGRAS_GL_INLINE_BLOCK) {
            s->eram_state = MGRAS_ERAM_WRITE_INLINE;
        } else if (header >> 16 != MGRAS_GL_DMA_DESC_CLASS) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: ERAM write count "
                          "with no data after it, header 0x%08x\n", header);
            s->eram_state = MGRAS_ERAM_IDLE;
        }
        break;
    case MGRAS_ERAM_WRITE_INLINE:
    case MGRAS_ERAM_WRITE_KICKED:
        if (header != MGRAS_GL_ERAM_WRITE) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: DMA to the GE kicked "
                          "for an ERAM write with no address after it\n");
            s->eram_state = MGRAS_ERAM_IDLE;
        }
        break;
    }
}

bool sgi_mgras_gl_eram_valid(SGIMgrasState *s)
{
    return s->eram_state < MGRAS_ERAM_STATES;
}

/*
 * A context switch can land inside a packet, and each context's words
 * continue its own packet when it is loaded again. In a captured run (seq
 * 12929748 to 12930582) perfly's context 0x84c6 was switched out 33 words into
 * the 156 data words of a GE table load, context 0x5420 was loaded and sent
 * one glFlush header, and 0x84c6 came back and sent its remaining 123 data
 * words. With one parser for the board the glFlush was consumed as table data
 * and 0x84c6's words then parsed as headers, which later left the X server
 * polling HQ flag bit 16 forever. The client cannot know where a switch falls,
 * so the board keeps the position per context (inferred). Approximated: the
 * model keeps it with the context's GL state, keyed the same way, not in
 * whatever HQ state the board saves. The discriminating tests are
 * gl-parser-follows-the-context and gl-packet-split-by-a-context-switch.
 */
void sgi_mgras_cmd_parser_save(SGIMgrasState *s, SGIMgrasCmdParser *p)
{
    p->cls = s->cmd_token_class;
    p->reg = s->cmd_token_reg;
    p->words_remaining = s->cmd_token_words_remaining;
    memcpy(p->payload, s->cmd_token_payload, sizeof(p->payload));
    p->payload_count = s->cmd_token_payload_count;
}

void sgi_mgras_cmd_parser_load(SGIMgrasState *s, const SGIMgrasCmdParser *p)
{
    s->cmd_token_class = p->cls;
    s->cmd_token_reg = p->reg;
    s->cmd_token_words_remaining = p->words_remaining;
    memcpy(s->cmd_token_payload, p->payload, sizeof(p->payload));
    s->cmd_token_payload_count = p->payload_count;
}

void sgi_mgras_gl_context_loaded(SGIMgrasState *s)
{
    uint32_t slot = s->context[MGRAS_GL_IMAGE_SLOT];

    sgi_mgras_cmd_parser_save(s, &s->gl_context[s->gl_current].parser);
    s->gl_banks[0] = s->context[MGRAS_GL_IMAGE_BANKS];
    s->gl_banks[1] = s->context[MGRAS_GL_IMAGE_BANKS + 1];
    s->gl_ge_port_logged = false;
    if ((slot & MGRAS_GL_NO_SLOT) || slot == MGRAS_GL_DEFAULT_SLOT) {
        s->gl_current = 0;
    } else {
        s->gl_current = mgras_gl_lookup(s, slot);
        s->gl_selected[s->gl_current] = ++s->gl_selections;
    }
    if (s->gl_current &&
        (s->context[MGRAS_GL_IMAGE_STATUS] & MGRAS_GL_IMAGE_NEW) ==
        MGRAS_GL_IMAGE_NEW) {
        mgras_gl_forget(s, s->gl_current, s->gl[s->gl_current].hw_id);
    }
    sgi_mgras_cmd_parser_load(s, &s->gl_context[s->gl_current].parser);
    mgras_gl_bind(s);
}

bool sgi_mgras_gl_client_read_active(SGIMgrasState *s)
{
    return s->gl_current < SGI_MGRAS_GL_CONTEXTS &&
           s->gl_context[s->gl_current].client_read_bracket;
}

bool sgi_mgras_gl_client_read_done(SGIMgrasState *s)
{
    uint32_t context = s->gl_current;
    bool kicked = context < SGI_MGRAS_GL_CONTEXTS &&
                  s->gl_context[context].client_read_kicked;

    if (!kicked) {
        return false;
    }
    s->gl_context[s->gl_current].client_read_kicked = 0;
    return true;
}

bool sgi_mgras_gl_client_read_window(SGIMgrasState *s, uint32_t *origin_x,
                                     uint32_t *origin_y, uint32_t *x0,
                                     uint32_t *y0, uint32_t *x1,
                                     uint32_t *y1)
{
    uint32_t origin;

    if (!s->gl_current || s->gl_current >= SGI_MGRAS_GL_CONTEXTS ||
        !s->gl_selected[s->gl_current]) {
        return false;
    }
    origin = s->context[MGRAS_GL_IMAGE_ORIGIN];
    *origin_x = origin & 0xffff;
    *origin_y = origin >> 16;
    *x0 = s->context[11] >> 16;
    *x1 = s->context[11] & 0xffff;
    *y0 = s->context[12] >> 16;
    *y1 = s->context[12] & 0xffff;
    return *x0 <= *x1 && *y0 <= *y1;
}

/*
 * The answer to the client's 0xa104 query for one current attribute.
 * The current-attribute query routine reads indices 4 to 7 as
 * GL_CURRENT_COLOR's four floats, 8 to 10 as GL_CURRENT_NORMAL's, 11 to 14 as
 * GL_CURRENT_TEXTURE_COORDS' and 15 as GL_EDGE_FLAG, true when the answer has
 * any of bits 0x01c00000. The model answers with the loaded context's colour,
 * normal, texture coordinates and edge flag; that the board answers with its
 * copy of that state is inferred. Which of the edge flag's three bits the
 * board sets is unknown, so the model sets all three. Other indices answer
 * zero, logged. The discriminating tests are
 * hq-attribute-queries-set-flag-after-arguments and
 * hq-attribute-query-answers-normal-and-edge-flag.
 *
 * The same routine reads GL_CURRENT_INDEX at index 4, and the raster
 * index shares 0x351 with the raster colour's red in the 0xa20c table,
 * so the board keeps an index where it keeps red (inferred). In
 * colour-index mode the model answers both with the index, as a float
 * the client truncates.
 */
#define MGRAS_GL_ATTR_INDEX      4
#define MGRAS_GL_QUERY_RASTER_INDEX 0x351
#define MGRAS_GL_QUERY_WINDOW_ORIGIN 0x31f

uint32_t sgi_mgras_gl_attribute_answer(SGIMgrasState *s, uint32_t index)
{
    static const struct {
        uint32_t pname;
        uint32_t first;
    } attrs[] = {
        { SGI_GL_CURRENT_COLOR, 4 },
        { SGI_GL_CURRENT_NORMAL, 8 },
        { SGI_GL_CURRENT_TEXTURE_COORDS, 11 },
        { SGI_GL_EDGE_FLAG, 15 },
    };
    uint32_t out[SGI_GL_GET_WORDS];
    unsigned n;

    if (index == MGRAS_GL_ATTR_INDEX && mgras_gl(s)->cur.index_bits &&
        sgi_gl_get(mgras_gl(s), SGI_GL_CURRENT_INDEX, out, &n)) {
        return out[0];
    }
    for (int i = 0; i < ARRAY_SIZE(attrs); i++) {
        if (index < attrs[i].first ||
            !sgi_gl_get(mgras_gl(s), attrs[i].pname, out, &n) ||
            index - attrs[i].first >= n) {
            continue;
        }
        if (attrs[i].pname == SGI_GL_EDGE_FLAG) {
            return out[0] ? 0x01c00000 : 0;
        }
        return out[index - attrs[i].first];
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL current-attribute query index "
                  "%u answered with zero\n", index);
    return 0;
}

/* How a query's answer word holds its piece of glGet's answer. */
typedef enum MgrasGLQueryPacking {
    /* The word glGet gives, float or integer. */
    MGRAS_GL_QUERY_WORD,
    /*
     * A box's last column or row, its start plus its size less 1, from
     * glGet's x, y, width and height.
     */
    MGRAS_GL_QUERY_BOX_END,
    /* The matrix mode's enumerant less GL_PROJECTION. */
    MGRAS_GL_QUERY_MATRIX_MODE,
    /* A raster window coordinate plus MGRAS_GL_RASTER_BIAS. */
    MGRAS_GL_QUERY_RASTER_XY,
    /* All ones for true, zero for false. */
    MGRAS_GL_QUERY_BOOLEAN_MASK,
    /* A float glGet keeps, as the integer the client reads. */
    MGRAS_GL_QUERY_INTEGER,
} MgrasGLQueryPacking;

/*
 * The client adds this double, read at $gp - 0x39e8 in
 * the glGet routine, to the x and y words of GL_CURRENT_RASTER_POSITION
 * before returning them.
 */
#define MGRAS_GL_RASTER_BIAS     49151.5

/*
 * The 0xa20c state queries the model answers. The state-query routine sends an
 * index, a1 and a2 and reads one word back at 0x028004. No index is a
 * literal: the glGet routine takes each from a table the library loads from
 * the HQ microcode file's symbol table, so the index is an address the
 * board's program gives for that piece of GL state. The notes list each
 * glGet caller's index and what it does with the word, established from
 * the IRIX 6.5.30 GL client library; the values below are the
 * hq3_revB_HQ4.u column. That the board answers with its copy of the
 * state OpenGL 1.2.1 has glGet return is inferred from how the client
 * uses each word, and the model answers the loaded context's.
 *
 * - GL_MODELVIEW_MATRIX and GL_PROJECTION_MATRIX: 16 queries with a1 0,
 *   index 0x3a + i and 0x4a + i, each word stored as a float into
 *   element i of the result (the client's loop advances its output one
 *   float per query), which glGet returns column-major.
 * - GL_TEXTURE_MATRIX: 0x6a + i with a1 0, read the
 *   same way.
 * - GL_MATRIX_MODE: index 0x29, a1 0; the client returns the word plus
 *   0x1701, so the word is the mode's enumerant less GL_PROJECTION.
 * - GL_VIEWPORT: 0xc1d to 0xc20, stored as integers x, y, width, height.
 * - GL_SHADE_MODEL: 0xc19, returned as the integer it is.
 * - GL_COLOR_CLEAR_VALUE: 0xc0a to 0xc0d, four floats.
 * - GL_CURRENT_RASTER_POSITION: x, y, z and w at 0x359, 0x35a, 0x35b and
 *   0x350, as floats. The client adds -49151.5 in double to x and y and
 *   returns z and w as they are, so the words are the window x and y plus
 *   49151.5, and z and clip w. In single precision the biased x and y
 *   hold the 1/256 pixel the model snaps vertices to; how the board rounds
 *   them is not published, and the model rounds to nearest.
 * - GL_CURRENT_RASTER_POSITION_VALID: 0x35c. The client ANDs the word
 *   with a word from the microcode symbol table and returns
 *   whether the result is nonzero. That mask's value is not recorded, so
 *   the model answers all ones for a valid position and zero otherwise,
 *   which reads correctly for any mask but zero; which bits the board
 *   sets is unknown.
 * - GL_CURRENT_RASTER_COLOR 0x351 to 0x354, GL_CURRENT_RASTER_TEXTURE_COORDS
 *   0x355 to 0x358 and GL_CURRENT_RASTER_DISTANCE 0x34b, as floats.
 * - GL_DEPTH_RANGE: near at 0xc21 and far at 0xc22, as floats. perfly
 *   reads both after each context setup.
 * - GL_LIGHT_MODEL_AMBIENT's red, green and blue at 0x55e to 0x560, as
 *   floats; GL_LIGHT_MODEL_LOCAL_VIEWER 0xc1a and GL_LIGHT_MODEL_TWO_SIDE
 *   0xc1b, stored as a byte; GL_COLOR_MATERIAL_PARAMETER 0x56b and
 *   GL_COLOR_MATERIAL_FACE 0x56c, as the enumerant.
 * - GL_SCISSOR_BOX at 0xc24 to 0xc27 as the integers x, y, x + width - 1
 *   and y + height - 1: the client returns the first two and the last two
 *   less the first two plus 1. GL_SCISSOR_TEST at
 *   0xc28, the whole word the boolean.
 * - GL_INDEX_CLEAR_VALUE at 0xc13, a float, and GL_STENCIL_CLEAR_VALUE at
 *   0xc14, an integer. GL_DEPTH_CLEAR_VALUE at 0xc12 the client divides
 *   by the float 16777215.0, so the answer is the clear depth in the
 *   24-bit units 0xbc04 sends it in.
 * - GL_BLEND at 0xc4a, the whole word the boolean, and GL_BLEND_SRC,
 *   GL_BLEND_DST and GL_BLEND_EQUATION at 0xc4b to 0xc4d, integers.
 * - GL_FOG_MODE at 0xbce, an integer; GL_FOG_DENSITY, _START and _END at
 *   0xbcf to 0xbd1 and GL_FOG_COLOR at 0xbd2 to 0xbd5, floats; and
 *   GL_FOG_INDEX at 0xbd6, an integer.
 * - GL_POLYGON_OFFSET_FACTOR, GL_POLYGON_OFFSET_BIAS_EXT and
 *   GL_POLYGON_OFFSET_UNITS at 0xca2 to 0xca4, floats.
 * - GL_POINT_SIZE and GL_LINE_WIDTH at 0xc3d and 0xc3e, floats;
 *   GL_CULL_FACE_MODE and GL_FRONT_FACE at 0xc3f and 0xc40 and
 *   GL_POLYGON_MODE's front and back at 0xc17 and 0xc18, enumerants; and
 *   GL_LINE_SMOOTH at 0xc30, the whole word the boolean. The line
 *   stipple's pattern and repeat, at 0xc46 and 0xc47, are answered in the
 *   form 0x4608 sends them (mgras_gl_client_form_query()).
 *
 * What a1 and a2 select is unknown; the model answers by index alone.
 * Every other index answers zero, logged, which is right for the
 * glFinish and the finish-fence routine fences and glGetError, which discard or
 * fall back from a zero, and wrong for the other state a client reads back.
 * The discriminating tests are hq-query-answers-the-matrices,
 * hq-query-answers-viewport-shade-and-clear-colour and
 * hq-query-answers-the-raster-position.
 */
static const struct {
    uint16_t first;
    uint8_t count;
    uint8_t packing;            /* MgrasGLQueryPacking */
    uint16_t pname;
    uint8_t element;            /* the first query's element of glGet's */
} mgras_gl_queries[] = {
    { 0x03a, 16, MGRAS_GL_QUERY_WORD, SGI_GL_MODELVIEW_MATRIX, 0 },
    { 0x04a, 16, MGRAS_GL_QUERY_WORD, SGI_GL_PROJECTION_MATRIX, 0 },
    { 0x06a, 16, MGRAS_GL_QUERY_WORD, SGI_GL_TEXTURE_MATRIX, 0 },
    { 0x029, 1, MGRAS_GL_QUERY_MATRIX_MODE, SGI_GL_MATRIX_MODE, 0 },
    { 0xc1d, 4, MGRAS_GL_QUERY_WORD, SGI_GL_VIEWPORT, 0 },
    { 0xc19, 1, MGRAS_GL_QUERY_WORD, SGI_GL_SHADE_MODEL, 0 },
    { 0xc0a, 4, MGRAS_GL_QUERY_WORD, SGI_GL_COLOR_CLEAR_VALUE, 0 },
    { 0x359, 2, MGRAS_GL_QUERY_RASTER_XY, SGI_GL_CURRENT_RASTER_POSITION, 0 },
    { 0x35b, 1, MGRAS_GL_QUERY_WORD, SGI_GL_CURRENT_RASTER_POSITION, 2 },
    { 0x350, 1, MGRAS_GL_QUERY_WORD, SGI_GL_CURRENT_RASTER_POSITION, 3 },
    { 0x35c, 1, MGRAS_GL_QUERY_BOOLEAN_MASK,
      SGI_GL_CURRENT_RASTER_POSITION_VALID, 0 },
    { 0x351, 4, MGRAS_GL_QUERY_WORD, SGI_GL_CURRENT_RASTER_COLOR, 0 },
    { 0x355, 4, MGRAS_GL_QUERY_WORD, SGI_GL_CURRENT_RASTER_TEXTURE_COORDS, 0 },
    { 0x34b, 1, MGRAS_GL_QUERY_WORD, SGI_GL_CURRENT_RASTER_DISTANCE, 0 },
    { 0xc21, 2, MGRAS_GL_QUERY_WORD, SGI_GL_DEPTH_RANGE, 0 },
    { 0x55e, 3, MGRAS_GL_QUERY_WORD, SGI_GL_LIGHT_MODEL_AMBIENT, 0 },
    { 0xc1a, 1, MGRAS_GL_QUERY_WORD, SGI_GL_LIGHT_MODEL_LOCAL_VIEWER, 0 },
    { 0xc1b, 1, MGRAS_GL_QUERY_WORD, SGI_GL_LIGHT_MODEL_TWO_SIDE, 0 },
    { 0x56b, 1, MGRAS_GL_QUERY_WORD, SGI_GL_COLOR_MATERIAL_PARAMETER, 0 },
    { 0x56c, 1, MGRAS_GL_QUERY_WORD, SGI_GL_COLOR_MATERIAL_FACE, 0 },
    { 0xc24, 2, MGRAS_GL_QUERY_WORD, SGI_GL_SCISSOR_BOX, 0 },
    { 0xc26, 2, MGRAS_GL_QUERY_BOX_END, SGI_GL_SCISSOR_BOX, 2 },
    { 0xc28, 1, MGRAS_GL_QUERY_WORD, SGI_GL_SCISSOR_TEST, 0 },
    { 0xc13, 1, MGRAS_GL_QUERY_WORD, SGI_GL_INDEX_CLEAR_VALUE, 0 },
    { 0xc14, 1, MGRAS_GL_QUERY_WORD, SGI_GL_STENCIL_CLEAR_VALUE, 0 },
    { 0xc4a, 1, MGRAS_GL_QUERY_WORD, SGI_GL_BLEND, 0 },
    { 0xc4b, 1, MGRAS_GL_QUERY_WORD, SGI_GL_BLEND_SRC, 0 },
    { 0xc4c, 1, MGRAS_GL_QUERY_WORD, SGI_GL_BLEND_DST, 0 },
    { 0xc4d, 1, MGRAS_GL_QUERY_WORD, SGI_GL_BLEND_EQUATION, 0 },
    { 0xbce, 1, MGRAS_GL_QUERY_WORD, SGI_GL_FOG_MODE, 0 },
    { 0xbcf, 1, MGRAS_GL_QUERY_WORD, SGI_GL_FOG_DENSITY, 0 },
    { 0xbd0, 1, MGRAS_GL_QUERY_WORD, SGI_GL_FOG_START, 0 },
    { 0xbd1, 1, MGRAS_GL_QUERY_WORD, SGI_GL_FOG_END, 0 },
    { 0xbd2, 4, MGRAS_GL_QUERY_WORD, SGI_GL_FOG_COLOR, 0 },
    { 0xbd6, 1, MGRAS_GL_QUERY_INTEGER, SGI_GL_FOG_INDEX, 0 },
    { 0xca2, 1, MGRAS_GL_QUERY_WORD, SGI_GL_POLYGON_OFFSET_FACTOR, 0 },
    { 0xca3, 1, MGRAS_GL_QUERY_WORD, SGI_GL_POLYGON_OFFSET_BIAS, 0 },
    { 0xca4, 1, MGRAS_GL_QUERY_WORD, SGI_GL_POLYGON_OFFSET_UNITS, 0 },
    { 0xc3d, 1, MGRAS_GL_QUERY_WORD, SGI_GL_POINT_SIZE, 0 },
    { 0xc3e, 1, MGRAS_GL_QUERY_WORD, SGI_GL_LINE_WIDTH, 0 },
    { 0xc3f, 1, MGRAS_GL_QUERY_WORD, SGI_GL_CULL_FACE_MODE, 0 },
    { 0xc40, 1, MGRAS_GL_QUERY_WORD, SGI_GL_FRONT_FACE, 0 },
    { 0xc17, 2, MGRAS_GL_QUERY_WORD, SGI_GL_POLYGON_MODE, 0 },
    { 0xc30, 1, MGRAS_GL_QUERY_WORD, SGI_GL_LINE_SMOOTH, 0 },
};

/* A 16-bit line stipple pattern with its bits in the other order. */
static uint32_t mgras_gl_reverse16(uint32_t pattern)
{
    uint32_t out = 0;

    for (int i = 0; i < 16; i++) {
        out |= extract32(pattern, i, 1) << (15 - i);
    }
    return out;
}

static uint32_t mgras_gl_reverse32(uint32_t pattern)
{
    uint32_t out = 0;

    for (int i = 0; i < 32; i++) {
        out |= extract32(pattern, i, 1) << (31 - i);
    }
    return out;
}

#define MGRAS_GL_QUERY_CLEAR_DEPTH 0xc12

/*
 * The answers kept in the form the client sends them rather than glGet's:
 * GL_DEPTH_CLEAR_VALUE in 24-bit units; GL_LINE_STIPPLE_PATTERN at 0xc46,
 * whose low 16 bits the client reverses; and GL_LINE_STIPPLE_REPEAT at
 * 0xc47, to which it adds 1.
 */
static bool mgras_gl_client_form_query(SGIMgrasState *s, uint32_t index,
                                       uint32_t *answer)
{
    const SGIGLContext *gl = mgras_gl(s);

    switch (index) {
    case MGRAS_GL_QUERY_CLEAR_DEPTH:
        *answer = gl->clear_depth & MGRAS_GL_DEPTH_MASK_24;
        return true;
    case 0xc46:
        *answer = mgras_gl_reverse16(gl->line_stipple_pattern);
        return true;
    case 0xc47:
        *answer = gl->line_stipple_repeat - 1;
        return true;
    default:
        return false;
    }
}

/*
 * GL_MODELVIEW_STACK_DEPTH and GL_PROJECTION_STACK_DEPTH. The glGet routine
 * queries index 0x83 (a1 0) and 0x922 (a1 1)
 * and returns ((answer - base) >> 4) + 1, the base a symbol-table word
 * for each stack (from the IRIX 6.5.30 GL client library). Those
 * words, in /usr/gfx/ucode/MGRAS/hq3_revB_HQ4.u (SHA-256
 * a696e9772a876ed02378111e80a01270161cb4ad8fc39355f40fe96d1fcdb062), are
 * 0x925 for the modelview stack and 0xb25 for the projection stack; the
 * file's index words for the two are 0x83 and 0x922, as the query notes
 * give them. So the answer is an address in
 * the GE's matrix stack, 16 words a matrix, and the model answers base +
 * 16 * (depth - 1) from the loaded context's stack. That the board's
 * word is that address is inferred from the client's arithmetic; the
 * modelview base lies 32 matrices below the projection base, the depth
 * the model gives both stacks.
 *
 * GL_TEXTURE_STACK_DEPTH is index 0x923 with its base in the same
 * table; the file's words for them are 0x923 and 0xb45. The texture
 * base lies two matrices above the projection base, so the board's
 * projection stack holds two (inferred), where the model's holds 32 like
 * its others. The colour-matrix stack is not kept, and its query still
 * answers zero.
 */
static const struct {
    uint16_t index;
    uint16_t base;
    uint16_t pname;
} mgras_gl_stack_queries[] = {
    { 0x083, 0x925, SGI_GL_MODELVIEW_STACK_DEPTH },
    { 0x922, 0xb25, SGI_GL_PROJECTION_STACK_DEPTH },
    { 0x923, 0xb45, SGI_GL_TEXTURE_STACK_DEPTH },
};

#define MGRAS_GL_STACK_WORDS     16

/*
 * The library loads hq3.u's table instead of hq3_revB_HQ4.u's when the
 * GE revision the kernel recorded is below 2, or the board has two GEs
 * and one RE. The kernel records the low byte of 0x028004 after the GE
 * program download, which the model holds at the board's GE revision
 * until the first query. Every board the model presents reports 2 or 3
 * and none has two GEs with one RE, so only the hq3_revB_HQ4.u indices
 * are decoded; a board that would select hq3.u answers nothing.
 */
static bool mgras_gl_query_table_known(const SGIMgrasBoard *b)
{
    return b->ge_revision >= 2 && !(b->ge_count == 2 && b->re_count == 1);
}

/*
 * Six queries read raster-register state packed as the raster engine
 * holds it, and the client decodes each
 * field as below (established from the listing). Every context setup's
 * 0x7c08 block pairs a raster register and value with one of these
 * indices as a third word, and each value decodes, as the client decodes
 * that query, to OpenGL's initial state:
 *
 * - 0x286, setup 0x10009000: the polygon stipple, line stipple and fog
 *   enables in bits 0, 5 and 17;
 * - 0x288, setup 0x0c000503: the dither, stencil and alpha test enables in
 *   bits 0, 4 and 6, the logic op less GL_CLEAR in bits 29:26;
 * - 0x2c6, setup 7: the stencil function less GL_NEVER in bits 2:0, the
 *   fail, depth-fail and depth-pass operations in bits 5:3, 8:6 and 11:9,
 *   the reference in bits 19:12;
 * - 0x2c8, setup 0xffff: the stencil value mask in bits 7:0 and the write
 *   mask in bits 15:8;
 * - 0x2ca, setup 0x0ffffff1: the depth function less GL_NEVER in bits 2:0,
 *   the depth write mask true when bits 27:4 are all set;
 * - 0x2cc, setup 7: the alpha function less GL_NEVER in bits 2:0, the
 *   reference as the 12-bit fraction glAlphaFunc sends in bits 14:3.
 *
 * The model does not answer from a table of those 0x7c08 values: the
 * same state reaches the raster engine through GL opcodes, such as
 * glDepthFunc (0x4304), which the GE turns into register writes the model
 * never sees as triples, so a stored triple would go stale at the first
 * of them. Instead each field comes from the loaded context's state
 * (inferred: that the board answers with the state the client last set);
 * the bits no query reads keep the setup value's.
 *
 * glIsEnabled(GL_FOG) reads a second index, 0x3 with a1 0, only when bit
 * 17 of 0x286 is clear, and tests it against the table word at offset
 * 0x440, 0x40000. What the word holds is
 * unknown; the model answers zero, which reads as fog disabled, as bit 17
 * already said.
 */
#define MGRAS_GL_QUERY_FOG_SECOND 0x003
#define MGRAS_GL_QUERY_FILL_ENABLES 0x286
#define MGRAS_GL_QUERY_PIXEL_ENABLES 0x288
#define MGRAS_GL_QUERY_STENCIL   0x2c6
#define MGRAS_GL_QUERY_STENCIL_MASKS 0x2c8
#define MGRAS_GL_QUERY_DEPTH     0x2ca
#define MGRAS_GL_QUERY_ALPHA     0x2cc
#define MGRAS_GL_DEPTH_WRITES    0x0ffffff0u
#define MGRAS_GL_FILL_SETUP      0x10009000u
#define MGRAS_GL_PIXEL_SETUP     0x0c000503u

static bool mgras_gl_raster_query(SGIMgrasState *s, uint32_t index,
                                  uint32_t *answer)
{
    const SGIGLContext *gl = mgras_gl(s);
    const SGIGLDrawState *st = &gl->cur;

    switch (index) {
    case MGRAS_GL_QUERY_FOG_SECOND:
        *answer = 0;
        break;
    case MGRAS_GL_QUERY_FILL_ENABLES:
        *answer = (MGRAS_GL_FILL_SETUP & ~0x20021u) | st->fog << 17 |
                  !!(gl->raster_enables & SGI_GL_RASTER_POLYGON_STIPPLE) |
                  !!(gl->raster_enables & SGI_GL_RASTER_LINE_STIPPLE) << 5;
        break;
    case MGRAS_GL_QUERY_PIXEL_ENABLES:
        *answer = (MGRAS_GL_PIXEL_SETUP & ~0x3c000051u) | gl->cur.dither |
                  st->stencil_test << 4 | st->alpha_test << 6 |
                  (st->logic_op_mode - SGI_GL_CLEAR) << 26;
        break;
    case MGRAS_GL_QUERY_STENCIL:
        *answer = st->stencil_func |
                  MIN(MAX(st->stencil_ref, 0), 0xff) << 12;
        for (int i = 0; i < 3; i++) {
            for (int n = 0; n < ARRAY_SIZE(mgras_gl_stencil_ops); n++) {
                if (st->stencil_op[i] == mgras_gl_stencil_ops[n]) {
                    *answer |= n << (3 + 3 * i);
                }
            }
        }
        break;
    case MGRAS_GL_QUERY_STENCIL_MASKS:
        *answer = (st->stencil_value_mask & 0xff) |
                  (st->stencil_write_mask & 0xff) << 8;
        break;
    case MGRAS_GL_QUERY_DEPTH:
        *answer = (st->depth_mask ? MGRAS_GL_DEPTH_WRITES : 0) |
                  st->depth_func;
        break;
    case MGRAS_GL_QUERY_ALPHA: {
        /* The reference in [0, 1] as the client's 12-bit fraction. */
        uint32_t ref = lroundf(st->alpha_ref * MGRAS_GL_ALPHA_REF_MAX);

        *answer = st->alpha_func | ref << 3;
        break;
    }
    default:
        return false;
    }
    return true;
}

/*
 * GL_COLOR_WRITEMASK and GL_INDEX_WRITEMASK are both query 0x32b. For the
 * colour mask the client tests the answer's bits 0 to 3 in turn as red, green,
 * blue and alpha; for the index mask it returns the answer ANDed with
 * (1 << n) - 1, n a context word holding the index's bits. Established
 * from the IRIX 6.5.30 GL client library. That the board keeps one
 * write-mask word for both is inferred from the shared index. In
 * colour-index mode the model answers glIndexMask's mask, and in RGBA mode
 * glColorMask's channels in the bits 0x3b04 sends them in. 0x32c is
 * glGetError's index, not a mask, and its zero answer stays right.
 */
#define MGRAS_GL_QUERY_WRITEMASK 0x32b

/*
 * glIsEnabled(GL_CLIP_PLANEi) queries 0xb87 + i and takes the answer's
 * sign bit as the boolean; glGetClipPlane queries 0xb8d + 4i + k for
 * coefficient k and reads floats. The
 * model answers the plane in eye coordinates, as the specification has
 * glGetClipPlane return it.
 */
#define MGRAS_GL_QUERY_CLIP_ENABLES 0xb87
#define MGRAS_GL_QUERY_CLIP_PLANES 0xb8d

#define MGRAS_GL_QUERY_PIXEL_CLIP_0 0x28c
#define MGRAS_GL_QUERY_PIXEL_CLIP_1 0x28e
#define MGRAS_GL_RECORD_PIXEL_CLIP_0 0x113
#define MGRAS_GL_RECORD_PIXEL_CLIP_1 0x114

static bool mgras_gl_pixel_clip_query(SGIMgrasState *s, uint32_t index,
                                      uint32_t *answer)
{
    uint32_t element;

    switch (index) {
    case MGRAS_GL_QUERY_PIXEL_CLIP_0:
        element = 0;
        break;
    case MGRAS_GL_QUERY_PIXEL_CLIP_1:
        element = 1;
        break;
    default:
        return false;
    }
    *answer = s->gl_context[s->gl_current].clip_query[element];
    return true;
}

static bool mgras_gl_pixel_clip_record(SGIMgrasState *s, uint32_t reg,
                                      uint32_t value)
{
    uint32_t element;

    switch (reg) {
    case MGRAS_GL_RECORD_PIXEL_CLIP_0:
        element = 0;
        break;
    case MGRAS_GL_RECORD_PIXEL_CLIP_1:
        element = 1;
        break;
    default:
        return false;
    }
    s->gl_context[s->gl_current].clip_query[element] = value;
    return true;
}

static bool mgras_gl_clip_query(SGIMgrasState *s, uint32_t index,
                                uint32_t *answer)
{
    uint32_t n = index - MGRAS_GL_QUERY_CLIP_ENABLES;
    uint32_t plane[4];

    if (n < SGI_GL_CLIP_PLANES) {
        *answer = mgras_gl(s)->cur.clip_planes & (1u << n) ? 0x80000000u : 0;
        return true;
    }
    n = index - MGRAS_GL_QUERY_CLIP_PLANES;
    if (n < 4 * SGI_GL_CLIP_PLANES &&
        sgi_gl_get_clip_plane(mgras_gl(s), n / 4, plane)) {
        *answer = plane[n % 4];
        return true;
    }
    return false;
}

static uint32_t mgras_gl_writemask_answer(SGIMgrasState *s)
{
    const SGIGLDrawState *st = &mgras_gl(s)->cur;

    return st->index_bits ? st->index_mask : st->colour_mask;
}

static void mgras_gl_selection_overflow(SGIMgrasGLSelection *sel)
{
    if (!sel->overflow) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: GL selection storage "
                      "overflowed; later data was dropped\n");
        sel->overflow = true;
    }
}

static void mgras_gl_selection_flush(SGIMgrasState *s)
{
    SGIMgrasGLSelection *sel = &s->gl_selection[s->gl_current];
    SGIMgrasGLSelectionHit *hit;

    if (!sel->pending) {
        return;
    }
    sel->pending = false;
    if (sel->hit_count == SGI_MGRAS_GL_SELECTION_HITS) {
        mgras_gl_selection_overflow(sel);
        return;
    }
    hit = &sel->hits[sel->hit_count++];
    hit->name_count = sel->name_count;
    hit->depth_min = sel->depth_min;
    hit->depth_max = sel->depth_max;
    memcpy(hit->names, sel->names, sel->name_count * sizeof(sel->names[0]));
}

static void mgras_gl_selection_name(SGIMgrasState *s, uint32_t header,
                                    const uint32_t *payload)
{
    SGIMgrasGLSelection *sel = &s->gl_selection[s->gl_current];

    if (!sel->active) {
        return;
    }
    mgras_gl_selection_flush(s);
    switch (header) {
    case MGRAS_GL_INIT_NAMES:
        sel->name_count = 0;
        break;
    case MGRAS_GL_PUSH_NAME:
        if (sel->name_count == SGI_MGRAS_GL_SELECTION_NAMES) {
            mgras_gl_selection_overflow(sel);
            break;
        }
        sel->names[sel->name_count++] = payload[0];
        break;
    case MGRAS_GL_LOAD_NAME:
        if (!sel->name_count) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: GL selection "
                          "load-name on an empty stack ignored\n");
            break;
        }
        sel->names[sel->name_count - 1] = payload[0];
        break;
    case MGRAS_GL_POP_NAME:
        if (!sel->name_count) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: GL selection "
                          "pop-name on an empty stack ignored\n");
            break;
        }
        sel->name_count--;
        break;
    default:
        g_assert_not_reached();
    }
}

static void mgras_gl_selection_mode(SGIMgrasState *s, uint32_t header,
                                    const uint32_t *payload)
{
    SGIMgrasGLSelection *sel = &s->gl_selection[s->gl_current];

    if (header == MGRAS_GL_RENDER_SELECT) {
        if (payload[0]) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL selection mode value "
                          "0x%x is not supported\n", payload[0]);
            return;
        }
        memset(sel, 0, sizeof(*sel));
        sel->active = true;
        return;
    }
    if (sel->active) {
        mgras_gl_selection_flush(s);
        sel->result_phase = true;
        sel->active = false;
    } else {
        sel->result_phase = false;
    }
    sel->name_count = 0;
}

static uint32_t mgras_gl_selection_query_word(const SGIMgrasGLSelection *sel,
                                               uint32_t index)
{
    uint32_t start = 1;

    if (!index) {
        uint32_t words = 0;

        for (uint32_t i = 0; i < sel->hit_count; i++) {
            words += 4 + sel->hits[i].name_count;
        }
        return words;
    }
    for (uint32_t i = 0; i < sel->hit_count; i++) {
        const SGIMgrasGLSelectionHit *hit = &sel->hits[i];

        if (index == start) {
            return hit->name_count;
        }
        if (index == start + 1) {
            return hit->depth_min;
        }
        if (index == start + 2) {
            return hit->depth_max;
        }
        if (index >= start + 3 && index < start + 3 + hit->name_count) {
            return hit->names[index - start - 3];
        }
        start += 4 + hit->name_count;
    }
    return 0;
}

static bool mgras_gl_selection_query(SGIMgrasState *s, const uint32_t *args,
                                     uint32_t *answer)
{
    const SGIMgrasGLSelection *sel = &s->gl_selection[s->gl_current];

    if (!sel->result_phase) {
        return false;
    }
    if (args[0] == MGRAS_GL_SELECTION_QUERY &&
        args[1] == 1 && args[2] == 1) {
        *answer = mgras_gl_selection_query_word(sel, 0);
        return true;
    }
    if (args[1] == UINT32_MAX && !args[2]) {
        *answer = mgras_gl_selection_query_word(sel, args[0]);
        return true;
    }
    return false;
}

uint32_t sgi_mgras_gl_query_answer(SGIMgrasState *s, const uint32_t *args,
                                   uint32_t nargs)
{
    uint32_t index = nargs ? args[0] : 0, out[SGI_GL_GET_WORDS];
    unsigned n;
    float f;

    if (nargs != 3 || !mgras_gl_query_table_known(s->board)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL state query with %u "
                      "arguments on this board answered with zero\n", nargs);
        return 0;
    }
    if (mgras_gl_selection_query(s, args, &out[0])) {
        return out[0];
    }
    if (mgras_gl_raster_query(s, index, &out[0])) {
        return out[0];
    }
    if (mgras_gl_pixel_clip_query(s, index, &out[0])) {
        return out[0];
    }
    if (index == MGRAS_GL_QUERY_WINDOW_ORIGIN &&
        mgras_gl(s)->cur.drawable.bound) {
        const int32_t *origin = mgras_gl(s)->cur.drawable.origin;

        return (uint16_t)origin[0] | (uint32_t)(uint16_t)origin[1] << 16;
    }
    if (index == MGRAS_GL_QUERY_WRITEMASK) {
        return mgras_gl_writemask_answer(s);
    }
    if (mgras_gl_clip_query(s, index, &out[0]) ||
        mgras_gl_client_form_query(s, index, &out[0])) {
        return out[0];
    }
    if (index >= MGRAS_GL_QUERY_READBACK &&
        index < MGRAS_GL_QUERY_READBACK + 4) {
        return s->gl_context[s->gl_current].readback[index -
                                                     MGRAS_GL_QUERY_READBACK];
    }
    if (index == MGRAS_GL_QUERY_RASTER_INDEX && mgras_gl(s)->cur.index_bits &&
        sgi_gl_get(mgras_gl(s), SGI_GL_CURRENT_RASTER_INDEX, out, &n)) {
        return out[0];
    }
    for (int i = 0; i < ARRAY_SIZE(mgras_gl_stack_queries); i++) {
        if (index == mgras_gl_stack_queries[i].index &&
            sgi_gl_get(mgras_gl(s), mgras_gl_stack_queries[i].pname, out,
                       &n)) {
            return mgras_gl_stack_queries[i].base +
                   MGRAS_GL_STACK_WORDS * (out[0] - 1);
        }
    }
    for (int i = 0; i < ARRAY_SIZE(mgras_gl_queries); i++) {
        uint32_t element = index - mgras_gl_queries[i].first;

        if (index < mgras_gl_queries[i].first ||
            element >= mgras_gl_queries[i].count ||
            !sgi_gl_get(mgras_gl(s), mgras_gl_queries[i].pname, out, &n)) {
            continue;
        }
        element += mgras_gl_queries[i].element;
        if (element >= n) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: GL state query index "
                          "0x%x names word %u of an answer of %u words; "
                          "answered with zero\n", index, element, n);
            return 0;
        }
        switch (mgras_gl_queries[i].packing) {
        case MGRAS_GL_QUERY_BOX_END:
            return out[element - 2] + out[element] - 1;
        case MGRAS_GL_QUERY_MATRIX_MODE:
            return out[element] - SGI_GL_PROJECTION;
        case MGRAS_GL_QUERY_RASTER_XY:
            memcpy(&f, &out[element], sizeof(f));
            f = f + MGRAS_GL_RASTER_BIAS;
            memcpy(&out[element], &f, sizeof(f));
            return out[element];
        case MGRAS_GL_QUERY_BOOLEAN_MASK:
            return out[element] ? UINT32_MAX : 0;
        case MGRAS_GL_QUERY_INTEGER:
            memcpy(&f, &out[element], sizeof(f));
            /* INT32_MAX rounds up to 2^31 in float: clamp in double. */
            return (int32_t)MIN(MAX((double)f, (double)INT32_MIN),
                                (double)INT32_MAX);
        default:
            return out[element];
        }
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL state query index 0x%x "
                  "answered with zero\n", index);
    return 0;
}

void sgi_mgras_gl_ge_port_access(SGIMgrasState *s, hwaddr addr, bool write)
{
    bool loaded = false;

    for (unsigned int i = 1; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        loaded |= s->gl_selected[i] != 0;
    }
    if (!loaded || s->gl_ge_port_logged) {
        return;
    }
    s->gl_ge_port_logged = true;
    qemu_log_mask(LOG_UNIMP, "sgi-mgras: GE port 0x%06" HWADDR_PRIx " %s "
                  "after GL contexts were loaded; the model does not save "
                  "or restore GL state through the GE ports\n", addr,
                  write ? "write" : "read");
}

static const VMStateDescription vmstate_sgi_mgras_cmd_parser = {
    .name = TYPE_SGI_MGRAS "/cmd-parser",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cls, SGIMgrasCmdParser),
        VMSTATE_UINT32(reg, SGIMgrasCmdParser),
        VMSTATE_UINT32(words_remaining, SGIMgrasCmdParser),
        VMSTATE_UINT32_ARRAY(payload, SGIMgrasCmdParser,
                             SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS),
        VMSTATE_UINT32(payload_count, SGIMgrasCmdParser),
        VMSTATE_END_OF_LIST()
    }
};

/* Each context's parser position, sent for every entry. */
const VMStateDescription vmstate_sgi_mgras_gl_context_parser = {
    .name = TYPE_SGI_MGRAS "/gl-context-parser",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parser, SGIMgrasGLContext, 1,
                       vmstate_sgi_mgras_cmd_parser, SGIMgrasCmdParser),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_readback = {
    .name = TYPE_SGI_MGRAS "/gl-context-readback",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(readback, SGIMgrasGLContext, 4),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Every context's light or material readback, sent only once some
 * context has asked for one; a stream without it leaves the zeros a
 * reset gives.
 */
static bool mgras_gl_readback_needed(void *opaque)
{
    SGIMgrasState *s = opaque;
    static const uint32_t zero[4];

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (memcmp(s->gl_context[i].readback, zero, sizeof(zero))) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_readback = {
    .name = TYPE_SGI_MGRAS "/gl-readback",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_readback_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl_context, SGIMgrasState,
                             SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_readback,
                             SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_client_read = {
    .name = TYPE_SGI_MGRAS "/gl-context-client-read",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(client_read_bracket, SGIMgrasGLContext),
        VMSTATE_UINT32(client_read_kicked, SGIMgrasGLContext),
        VMSTATE_UINT32(client_read_bank, SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static bool mgras_gl_client_read_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (s->gl_context[i].client_read_bracket ||
            s->gl_context[i].client_read_kicked ||
            s->gl_context[i].client_read_bank) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_client_read = {
    .name = TYPE_SGI_MGRAS "/gl-client-read",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_client_read_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl_context, SGIMgrasState,
                             SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_client_read,
                             SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_clip_query = {
    .name = TYPE_SGI_MGRAS "/gl-context-clip-query",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(clip_query, SGIMgrasGLContext, 2),
        VMSTATE_END_OF_LIST()
    }
};

static bool mgras_gl_clip_query_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (s->gl_context[i].clip_query[0] ||
            s->gl_context[i].clip_query[1]) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_clip_query = {
    .name = TYPE_SGI_MGRAS "/gl-clip-query",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_clip_query_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl_context, SGIMgrasState,
                             SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_clip_query,
                             SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_iris_rule = {
    .name = TYPE_SGI_MGRAS "/gl-context-iris-rule",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(iris_rule, SGIMgrasGLContext),
        VMSTATE_UINT32(iris_colour_last, SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/* Sent once some context has the rule on or a colour after a normal. */
static bool mgras_gl_iris_rule_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (s->gl_context[i].iris_rule ||
            s->gl_context[i].iris_colour_last) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_iris_rule = {
    .name = TYPE_SGI_MGRAS "/gl-iris-rule",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_iris_rule_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl_context, SGIMgrasState,
                             SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_iris_rule,
                             SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_tex_gen = {
    .name = TYPE_SGI_MGRAS "/gl-context-tex-gen",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(tex_gen_plane_open, SGIMgrasGLContext),
        VMSTATE_UINT32_ARRAY(tex_gen_plane, SGIMgrasGLContext, 2),
        VMSTATE_END_OF_LIST()
    }
};

/* Sent while some context is between a 0x5b08 and its plane. */
static bool mgras_gl_tex_gen_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (s->gl_context[i].tex_gen_plane_open) {
            return true;
        }
    }
    return false;
}

static int mgras_gl_tex_gen_post_load(void *opaque, int version_id)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (s->gl_context[i].tex_gen_plane_open > 1) {
            return -EINVAL;
        }
    }
    return 0;
}

const VMStateDescription vmstate_sgi_mgras_gl_tex_gen = {
    .name = TYPE_SGI_MGRAS "/gl-tex-gen",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_tex_gen_needed,
    .post_load = mgras_gl_tex_gen_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl_context, SGIMgrasState,
                             SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_tex_gen,
                             SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * The GE's pixel parameters and each context's pixel draw, sent once a
 * parameter has been loaded, a draw or load is under way, or a parser is
 * inside a load or a pixel row: a binary without them would read those
 * words as other packets.
 */
static bool mgras_gl_pixels_needed(void *opaque)
{
    SGIMgrasState *s = opaque;
    bool used = s->cmd_token_class == SGI_MGRAS_CMD_TOKEN_GE_LOAD ||
                (s->cmd_token_class == SGI_MGRAS_CMD_TOKEN_GL_BLOCK &&
                 s->cmd_token_reg == MGRAS_GL_PIXELS_OP);

    for (int i = 0; !used && i < SGI_MGRAS_GE_PARAM_WORDS; i++) {
        used = s->ge_param[i] != 0;
    }
    for (int i = 0; !used && i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIMgrasCmdParser *p = &s->gl_context[i].parser;

        used = s->gl_context[i].pixels.active ||
               p->cls == SGI_MGRAS_CMD_TOKEN_GE_LOAD ||
               (p->cls == SGI_MGRAS_CMD_TOKEN_GL_BLOCK &&
                p->reg == MGRAS_GL_PIXELS_OP);
    }
    return used;
}

static const VMStateDescription vmstate_sgi_mgras_gl_context_pixels = {
    .name = TYPE_SGI_MGRAS "/gl-context-pixels",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pixels.active, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.format, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.bpp, SGIMgrasGLContext),
        VMSTATE_INT32(pixels.x, SGIMgrasGLContext),
        VMSTATE_INT32(pixels.y, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.w, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.h, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.row, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.col, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.skip, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.carry, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.ncarry, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.ncomponent, SGIMgrasGLContext),
        VMSTATE_UINT32_ARRAY(pixels.components, SGIMgrasGLContext, 4),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_sgi_mgras_gl_pixels = {
    .name = TYPE_SGI_MGRAS "/gl-pixels",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_pixels_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(ge_param, SGIMgrasState,
                             SGI_MGRAS_GE_PARAM_WORDS),
        VMSTATE_STRUCT_ARRAY(gl_context, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS,
                             1, vmstate_sgi_mgras_gl_context_pixels,
                             SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Each context's DMA mark on its waiting pixel draw, and whether another
 * header has come since the draw's last word, sent only while some
 * waiting draw has either. A binary without them lets such a draw take
 * the next inline block, which no captured client sends.
 */
static bool mgras_gl_pixel_flags_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIMgrasGLPixels *px = &s->gl_context[i].pixels;

        if (px->active && (px->dma_only || px->apart)) {
            return true;
        }
    }
    return false;
}

static const VMStateDescription vmstate_sgi_mgras_gl_context_pixel_flags = {
    .name = TYPE_SGI_MGRAS "/gl-context-pixel-flags",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pixels.dma_only, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.apart, SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_sgi_mgras_gl_pixel_flags = {
    .name = TYPE_SGI_MGRAS "/gl-pixel-flags",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_pixel_flags_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl_context, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS,
                             1, vmstate_sgi_mgras_gl_context_pixel_flags,
                             SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Each context's pixel-texture pass, sent only while some waiting draw is
 * one RE's pass of a two-RE board's pixel-texture draw. A binary without
 * it would draw the pass's rows one after another from the box's bottom.
 */
static bool mgras_gl_pixel_pass_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIMgrasGLPixels *px = &s->gl_context[i].pixels;

        if (px->active && (px->pass || px->row_step > 1)) {
            return true;
        }
    }
    return false;
}

static const VMStateDescription vmstate_sgi_mgras_gl_context_pixel_pass = {
    .name = TYPE_SGI_MGRAS "/gl-context-pixel-pass",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pixels.pass, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.row_step, SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_sgi_mgras_gl_pixel_pass = {
    .name = TYPE_SGI_MGRAS "/gl-pixel-pass",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_pixel_pass_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl_context, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS,
                             1, vmstate_sgi_mgras_gl_context_pixel_pass,
                             SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/* Each context's pixel zoom, sent only while some waiting draw is zoomed. */
static bool mgras_gl_pixel_zoom_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIMgrasGLPixels *px = &s->gl_context[i].pixels;

        if (px->active && px->zoom_x) {
            return true;
        }
    }
    return false;
}

static const VMStateDescription vmstate_sgi_mgras_gl_context_pixel_zoom = {
    .name = TYPE_SGI_MGRAS "/gl-context-pixel-zoom",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pixels.zoom_x, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.zoom_y, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.dest_w, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.dest_h, SGIMgrasGLContext),
        VMSTATE_UINT32(pixels.flip, SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_sgi_mgras_gl_pixel_zoom = {
    .name = TYPE_SGI_MGRAS "/gl-pixel-zoom",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_pixel_zoom_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl_context, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS,
                             1, vmstate_sgi_mgras_gl_context_pixel_zoom,
                             SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static bool mgras_gl_pixels_zoom_valid(const SGIMgrasGLPixels *px)
{
    float zx, zy;

    if (!px->zoom_x) {
        return !px->zoom_y && !px->dest_w && !px->dest_h && !px->flip;
    }
    memcpy(&zx, &px->zoom_x, sizeof(zx));
    memcpy(&zy, &px->zoom_y, sizeof(zy));
    return isfinite(zx) && zx > 0 && isfinite(zy) && zy > 0 &&
           px->dest_w >= 1 && px->dest_w <= 2 * MGRAS_GL_PIXELS_MAX &&
           px->dest_h >= 1 && px->dest_h <= 2 * MGRAS_GL_PIXELS_MAX &&
           px->flip <= 3;
}

/* Whether a migrated pixel draw is one mgras_gl_pixels_start() makes. */
static bool mgras_gl_pixels_valid(const SGIMgrasGLPixels *px)
{
    bool float_rgb = px->format == MGRAS_GL_PIXELS_FLOAT_RGB;
    bool float_rgba = px->format == MGRAS_GL_PIXELS_FLOAT_RGBA;
    uint32_t components = float_rgb ? 3 : 4;

    if (!px->active) {
        return true;
    }
    if (px->active != 1 || px->format > MGRAS_GL_PIXELS_FORMATS ||
        px->w < 1 || px->w > MGRAS_GL_PIXELS_MAX || px->h < 1 ||
        px->h > MGRAS_GL_PIXELS_MAX || px->row >= px->h ||
        px->col > px->w || px->skip > 3 || px->dma_only > 1 ||
        px->apart > 1 || px->row_step > 2 ||
        px->pass >= MAX(px->row_step, 1u) ||
        !mgras_gl_pixels_zoom_valid(px) ||
        px->x < -MGRAS_GL_PIXELS_FAR ||
        px->x > MGRAS_GL_PIXELS_FAR || px->y < -MGRAS_GL_PIXELS_FAR ||
        px->y > MGRAS_GL_PIXELS_FAR) {
        return false;
    }
    if (float_rgb || float_rgba) {
        if (px->bpp != components * sizeof(uint32_t) ||
            px->ncarry >= sizeof(uint32_t) ||
            px->ncomponent >= components ||
            (px->ncomponent && px->col >= px->w)) {
            return false;
        }
        for (uint32_t i = 0; i < px->ncomponent; i++) {
            union {
                uint32_t bits;
                float value;
            } component = { .bits = px->components[i] };

            if (!isfinite(component.value)) {
                return false;
            }
        }
        return true;
    }
    return px->bpp >= 1 && px->bpp <= 4 &&
           px->ncarry < px->bpp && !px->ncomponent;
}

static bool mgras_gl_selection_valid(const SGIMgrasGLSelection *sel)
{
    float pending_min, pending_max;

    if (sel->name_count > SGI_MGRAS_GL_SELECTION_NAMES ||
        sel->hit_count > SGI_MGRAS_GL_SELECTION_HITS ||
        (sel->active && sel->result_phase) ||
        (sel->pending && !sel->active) ||
        (!sel->active && sel->name_count)) {
        return false;
    }
    memcpy(&pending_min, &sel->depth_min, sizeof(pending_min));
    memcpy(&pending_max, &sel->depth_max, sizeof(pending_max));
    if (sel->pending && (!isfinite(pending_min) || !isfinite(pending_max) ||
                         pending_min < 0 || pending_max > 1 ||
                         pending_min > pending_max)) {
        return false;
    }
    for (uint32_t i = 0; i < sel->hit_count; i++) {
        const SGIMgrasGLSelectionHit *hit = &sel->hits[i];
        float minimum, maximum;

        if (hit->name_count > SGI_MGRAS_GL_SELECTION_NAMES) {
            return false;
        }
        memcpy(&minimum, &hit->depth_min, sizeof(minimum));
        memcpy(&maximum, &hit->depth_max, sizeof(maximum));
        if (!isfinite(minimum) || !isfinite(maximum) || minimum < 0 ||
            maximum > 1 || minimum > maximum) {
            return false;
        }
    }
    return true;
}

bool sgi_mgras_cmd_parser_valid(uint32_t cls, uint32_t reg,
                                uint32_t words_remaining,
                                uint32_t payload_count)
{
    if (reg >= sizeof_field(SGIMgrasState, rss) / sizeof(uint32_t) ||
        payload_count > SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS) {
        return false;
    }
    if (reg == MGRAS_GL_POLYGON_STIPPLE_UPLOAD_REG && cls == 0) {
        return payload_count + words_remaining == 32;
    }
    /* A block with an offset takes one word more than its length. */
    if (cls == SGI_MGRAS_CMD_TOKEN_DISCARD) {
        return words_remaining <= SGI_MGRAS_CMD_TOKEN_MAX_BLOCK_WORDS + 1;
    }
    if (cls == SGI_MGRAS_CMD_TOKEN_GL_BLOCK) {
        return (((reg == MGRAS_GL_BITMAP_OP || reg == MGRAS_GL_PIXELS_OP) &&
                 payload_count == MGRAS_GL_BITMAP_WORDS) ||
                ((reg == SGI_MGRAS_TEX_RECORDS_OP ||
                  reg == MGRAS_GL_RSS_PAIRS_OP) &&
                 payload_count == SGI_MGRAS_TEX_RECORD_SLOTS)) &&
               words_remaining <= SGI_MGRAS_CMD_TOKEN_MAX_BLOCK_WORDS + 1;
    }
    if (cls == SGI_MGRAS_CMD_TOKEN_GE_LOAD) {
        return (reg == MGRAS_GL_GE_CRAM_OP || reg == MGRAS_GL_GE_ERAM_OP) &&
               payload_count == MGRAS_GL_GE_LOAD_SLOTS &&
               words_remaining <= SGI_MGRAS_CMD_TOKEN_MAX_BLOCK_WORDS;
    }
    return words_remaining <= SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS;
}

bool sgi_mgras_gl_post_load(SGIMgrasState *s)
{
    if (s->gl_current >= SGI_MGRAS_GL_CONTEXTS ||
        (s->gl_current && !s->gl_selected[s->gl_current]) ||
        !sgi_mgras_gl_eram_valid(s)) {
        return false;
    }
    /*
     * The shared description has already refused a context no GL command
     * could have left; what remains is each context's MGRAS state.
     */
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIMgrasCmdParser *p = &s->gl_context[i].parser;

        if (!sgi_gl_context_valid(&s->gl[i], NULL) ||
            !sgi_mgras_cmd_parser_valid(p->cls, p->reg, p->words_remaining,
                                        p->payload_count) ||
            s->gl_context[i].client_read_bracket > 1 ||
            s->gl_context[i].client_read_kicked > 1 ||
            s->gl_context[i].client_read_bank >
                MGRAS_CLIENT_READ_DEEP_BACK ||
            s->gl_context[i].iris_rule > 1 ||
            s->gl_context[i].iris_colour_last > 1 ||
            (s->gl_context[i].client_read_bank >=
                 MGRAS_CLIENT_READ_DEEP_FRONT &&
             !sgi_mgras_deep_reads(s)) ||
            !mgras_gl_pixels_valid(&s->gl_context[i].pixels) ||
            !mgras_gl_selection_valid(&s->gl_selection[i]) ||
            !sgi_mgras_texture_valid(&s->gl_context[i].tex)) {
            return false;
        }
    }
    /* An entry's identity is its slot, which the stream carries. */
    for (int i = 1; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        s->gl[i].hw_id = s->gl_selected[i] ? s->gl_slot[i] : 0;
    }
    /*
     * The loaded context's drawable comes from the image and banks the
     * stream carries; every other is set again when that context is next
     * loaded.
     */
    mgras_gl_bind(s);
    return true;
}

/*
 * Store a direct-colour pixel at screen column x and row y counted from
 * the bottom, in each bank of @banks. The GL window's WID selects an XMAP
 * mode, 0x1408 for the probe's visual, whose colour map is never loaded;
 * an RGB pixel bypasses that map (sgi-mgras-display.c), which is what
 * makes the drawing visible. Each bank keeps its own format tag.
 */
/*
 * Clip IDs. When a window has no visible rectangle the kernel can give (five or
 * more), its clip validation routine (in the IRIX 6.5.30 kernel graphics
 * driver) binds the whole window as rectangle 0, mode 1, and, when the window
 * has a clip ID, sets bit 4 + ID of image word 4 (computed as (x & 3) | (y & 3)
 * << 2 | 1 << (4 + ID)). The same word goes to the kernel's shadow of raster
 * register 0x17b, which the X server sets to 0xc00 for its own drawing. The X
 * server tags the window's visible region with that ID first
 * (sgi-mgras-raster.c, mgras_fill_clip_id()): twilight's window got ID 1 over
 * exactly its visible boxes and word 4 0x20.
 *
 * Inferred: a fragment of a context whose word 4 has any of bits 7:4 set
 * belongs to the window only where the pixel's clip ID selects one of
 * them, OpenGL's pixel ownership test (1.2.1, 4.1.1); elsewhere it is
 * discarded before any test or write. With bits 7:4 clear, as every
 * window the kernel describes by rectangles has them, there is no test.
 * Word 4 is read as the image now holds it rather than at the bind, like
 * nothing else the bind keeps; the two differ only inside a context
 * transaction a guest error cut short. Bits 11:10 (the X server's 0xc00)
 * and bits 3:0 are not modelled. The discriminating test is
 * gl-clip-id-owns-the-tagged-pixels.
 *
 * Limitation: the test covers GL fragment stores alone. Pixels a GL
 * context writes through the raster path (DMA image draws, rectangle
 * fills) are not tested against the clip ID; no capture shows a GL
 * client writing that way into a clip-ID window.
 */
static bool mgras_gl_owns(SGIMgrasState *s, int x, int y)
{
    uint32_t accept = (s->context[MGRAS_GL_IMAGE_CLIP_ID] >> 4) & 0xf;

    return !accept ||
           (accept >> s->clip_id[(s->height - 1 - y) * s->width + x]) & 1;
}

static void mgras_gl_put(SGIMgrasState *s, unsigned int banks, int x, int y,
                         uint32_t rgb)
{
    unsigned int idx = (s->height - 1 - y) * s->width + x;

    if (!mgras_gl_owns(s, x, y)) {
        return;
    }
    if (banks & 1) {
        s->front[idx] = rgb;
        s->format[idx] = SGI_MGRAS_PIXEL_FORMAT_RGB;
    }
    if (banks & 2) {
        s->back[idx] = rgb;
        s->back_format[idx] = SGI_MGRAS_PIXEL_FORMAT_RGB;
        /* A shallow window's pixel is not a deep window's. */
        if (s->slot[0]) {
            s->slot[0][idx] &= ~SGI_MGRAS_SLOT_WRITTEN;
        }
    }
    for (int i = 0; i < 2; i++) {
        if ((banks & (4u << i)) && s->slot[i]) {
            s->slot[i][idx] = (rgb & 0xffffff) |
                              (i ? 0 : SGI_MGRAS_SLOT_WRITTEN);
        }
    }
}

/* The pixel's depth word: the depth in bits 23:0, the stencil in 31:24. */
static uint32_t *mgras_gl_depth(SGIMgrasState *s, int x, int y)
{
    return &s->depth[(s->height - 1 - y) * s->width + x];
}

/* target is the bank mask mgras_gl_bind() gave the drawable. */
static void mgras_gl_store_colour(void *opaque, SGIColourTarget target,
                                  int x, int y, uint32_t colour)
{
    if (target & MGRAS_GL_OVERLAY_TARGET) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: direct colour into the overlay "
                      "planes is not modelled; fragment dropped\n");
        return;
    }
    mgras_gl_put(opaque, target, x, y, colour);
}

static void mgras_gl_store_rgba(void *opaque, SGIColourTarget target,
                                int x, int y,
                                const SGIRasterColour *colour)
{
    SGIMgrasState *s = opaque;
    unsigned int idx = (s->height - 1 - y) * s->width + x;
    uint32_t rgb = colour->component[0] << 16 |
                   colour->component[1] << 8 | colour->component[2];

    if (target & MGRAS_GL_OVERLAY_TARGET) {
        mgras_gl_store_colour(s, target, x, y, rgb);
        return;
    }
    if (!mgras_gl_owns(s, x, y)) {
        return;
    }
    mgras_gl_put(s, target, x, y, rgb);
    if (target & 1) {
        s->front_alpha[idx] = colour->component[3];
    }
    if (target & 2) {
        s->back_alpha[idx] = colour->component[3];
    }
    for (int i = 0; i < 2; i++) {
        if (target & (4u << i)) {
            s->slot_alpha[i][idx] = colour->component[3];
        }
    }
}

/*
 * The stored pixel of the first bank in target: a direct colour's 24
 * bits, or a CI8 pixel's index.
 */
static uint32_t mgras_gl_load_colour(void *opaque, SGIColourTarget target,
                                     int x, int y)
{
    SGIMgrasState *s = opaque;
    unsigned int idx = (s->height - 1 - y) * s->width + x;

    if (target & MGRAS_GL_OVERLAY_TARGET) {
        return s->overlay[idx];
    }
    if (target & 1) {
        return s->front[idx] & 0xffffff;
    }
    if (target & 2) {
        return s->back[idx] & 0xffffff;
    }
    return s->slot[target & 4 ? 0 : 1][idx] & 0xffffff;
}

static bool mgras_gl_load_rgba(void *opaque, SGIColourTarget target,
                               int x, int y, SGIRasterColour *colour)
{
    SGIMgrasState *s = opaque;
    unsigned int idx = (s->height - 1 - y) * s->width + x;
    uint32_t rgb;

    if (target & 1) {
        rgb = s->front[idx];
        colour->component[3] = s->front_alpha[idx];
    } else if (target & 2) {
        rgb = s->back[idx];
        colour->component[3] = s->back_alpha[idx];
    } else if (target & 4) {
        rgb = s->slot[0][idx];
        colour->component[3] = s->slot_alpha[0][idx];
    } else if (target & 8) {
        rgb = s->slot[1][idx];
        colour->component[3] = s->slot_alpha[1][idx];
    } else {
        return false;
    }
    colour->component[0] = (rgb >> 16) & 0xff;
    colour->component[1] = (rgb >> 8) & 0xff;
    colour->component[2] = rgb & 0xff;
    return true;
}

/*
 * A colour index as a CI8 pixel in each bank of target, which scans out
 * through the colour map the window's display ID selects, the X server's
 * own CI8 form. The write mask keeps
 * the stored index's bits where it is clear. The pixel keeps as many bits
 * as the context's index holds, up to twelve: the X server offers
 * 12-plane PseudoColor visuals with 4096 colour-map entries, and showmap
 * and interp set up 12-bit contexts on them; a deeper context keeps its
 * low twelve, logged at its setup. Whether scanout shows eight or twelve
 * of them is the display ID's mode's (sgi-mgras-display.c). The
 * discriminating test is gl-colour-index-twelve-bits-reach-4096-entries.
 *
 * Bank 1 takes indices as bank 0 does. The client sets up each colour
 * buffer of a colour-index visual with 0x0a04 (the colour-index initializer,
 * which the per-buffer setup routine calls a second time for the back buffer of
 * a double-buffered visual), so it expects its back buffer to hold
 * indices; bz's double-buffered context sends 0x0a04 twice. How the board
 * stores an index in either bank is not published. The discriminating
 * tests are gl-colour-index-draws-into-bank-one and
 * gl-colour-index-swap-shows-the-drawn-bank.
 *
 * A mask with none of the context's bits set leaves the pixel as it was
 * (OpenGL 1.2.1, 4.2.2), whatever its format. Approximated: the model
 * tags each pixel with one format rather than keeping planes, so a
 * partial mask over a direct-colour pixel keeps that pixel's low bits
 * as the index's other bits and makes it CI8. The discriminating test is
 * gl-colour-index-zero-mask-keeps-the-pixel.
 */
static void mgras_gl_store_index(void *opaque, SGIColourTarget target,
                                 int x, int y, uint32_t index, uint32_t mask)
{
    SGIMgrasState *s = opaque;
    unsigned int idx = (s->height - 1 - y) * s->width + x;
    uint32_t planes = MAKE_64BIT_MASK(0, MIN(mgras_gl(s)->cur.index_bits,
                                             MGRAS_GL_INDEX_BITS));

    if (!(mask & planes) || !mgras_gl_owns(s, x, y)) {
        return;
    }
    if (target & MGRAS_GL_OVERLAY_TARGET) {
        s->overlay[idx] = (s->overlay[idx] & ~mask) | (index & mask);
        return;
    }
    if (target & 1) {
        s->front[idx] = ((s->front[idx] & ~mask) | (index & mask)) & planes;
        s->format[idx] = SGI_MGRAS_PIXEL_FORMAT_CI8;
    }
    if (target & 2) {
        s->back[idx] = ((s->back[idx] & ~mask) | (index & mask)) & planes;
        s->back_format[idx] = SGI_MGRAS_PIXEL_FORMAT_CI8;
    }
    for (int i = 0; i < 2; i++) {
        if ((target & (4u << i)) && s->slot[i]) {
            s->slot[i][idx] = ((s->slot[i][idx] & ~mask) | (index & mask)) &
                              planes;
        }
    }
}

static uint32_t mgras_gl_load_depth(void *opaque, int x, int y)
{
    return *mgras_gl_depth(opaque, x, y) & MGRAS_GL_DEPTH_MASK_24;
}

static void mgras_gl_store_depth(void *opaque, int x, int y, uint32_t depth)
{
    uint32_t *word = mgras_gl_depth(opaque, x, y);

    if (!mgras_gl_owns(opaque, x, y)) {
        return;
    }
    *word = (*word & ~MGRAS_GL_DEPTH_MASK_24) |
            (depth & MGRAS_GL_DEPTH_MASK_24);
}

static uint32_t mgras_gl_load_stencil(void *opaque, int x, int y)
{
    return *mgras_gl_depth(opaque, x, y) >> MGRAS_GL_STENCIL_SHIFT;
}

static void mgras_gl_store_stencil(void *opaque, int x, int y,
                                   uint32_t value)
{
    uint32_t *word = mgras_gl_depth(opaque, x, y);

    if (!mgras_gl_owns(opaque, x, y)) {
        return;
    }
    *word = (*word & MGRAS_GL_DEPTH_MASK_24) |
            value << MGRAS_GL_STENCIL_SHIFT;
}

/*
 * The rules MGRAS's GL drawing follows, each implemented and tested:
 *
 * - Window coordinates snap to 1/256 pixel. The Octane Technical Report
 *   (section 4.2.3) gives the RE4 eight bits of subpixel precision; how
 *   the GE rounds to them is not published, so rounding to nearest, half
 *   away from zero, is the model's choice. The discriminating test is
 *   gl-triangle-snaps-to-the-nearest-subpixel.
 * - The depth plane holds 24 bits, the Z width the same report gives the
 *   RE4. A fragment's window depth rounds the same way, the model's
 *   choice; the discriminating test is gl-depth-rounds-to-24-bits.
 * - A colour channel is stored at eight bits. The 12-bit-per-channel
 *   visual the probe uses (visual 0x30, 12 red bits) is stored at the
 *   eight bits a direct-colour pixel holds in this model; 0.0 and 1.0 are
 *   exact either way, and how the board reduces an interpolated 12-bit
 *   value is not published. The discriminating test is
 *   gl-smooth-rounds-to-the-nearest-step.
 * - A shared edge's centres belong to the left and top edges, rows
 *   counted up from the bottom, the model's choice; the discriminating
 *   test is gl-triangle-shared-edge-centres-drawn-once.
 * - Attributes interpolate in double arithmetic, not the GE's, which is
 *   not published; a result that differs only in rounding is within the
 *   probe's margins.
 * - Lines cover the pixels the diamond-exit rule gives on the subpixel
 *   grid, with the declared endpoint perturbation (gl-reference/lines and
 *   gl-line-leaves-out-its-last-pixel).
 * - Primitives are clipped to the view volume as OpenGL 1.2.1, 2.11
 *   describes, in double arithmetic, not by the GE's own clipper, whose
 *   arithmetic and any guard band are not published (the shared
 *   rasterizer's sgi_soft_triangle()).
 * - The stencil buffer holds 8 bits, as every visual with a depth buffer
 *   has (above, with the stencil packets).
 * - Smoothed points and lines take the board's own filter, not OpenGL's
 *   area coverage. The Octane Technical Report (4.4.3 and 4.4.4) draws an
 *   antialiased point over a 2x2 grid of pixels weighted by their
 *   distance from the subpixel point, and an antialiased line two pixels
 *   wide, weighted higher nearer the line in the minor axis, corrected
 *   for slope and with its endpoints faded by their major-axis subpixel
 *   position; for a colour-index line the weight replaces the index's
 *   low 4 bits, which the model does for points too, inferred. The
 *   weights themselves are not published: the bilinear point
 *   weight, the linear minor-axis weight, the sqrt(1 + slope^2) slope
 *   correction and the endpoint fade by covered column length are the
 *   model's choices (SGI_SMOOTH_TWO_PIXEL_FILTER). The report says
 *   nothing of size, but the IRIX 6.4 and 6.5.30 client libraries send
 *   the board every point size and line width, smoothed or not, unless
 *   SOFT_WIDE_AA_LINES or (6.5.30) SOFT_WIDE_AA_POINTS is set, when they
 *   draw large smoothed lines or points on the host. Both are unset by
 *   default, so the board draws them; the model widens the filtered box
 *   to the size, which is the report's filter at size 1. The
 *   discriminating tests are
 *   gl-smooth-point-filters-over-four-pixels and
 *   gl-smooth-line-filters-over-two-pixels.
 */
static const SGIRasterRules mgras_gl_raster_rules = {
    .subpixel_bits = 8,
    .color_bits = 8,
    .alpha_bits = 8,
    .depth_bits = 24,
    .stencil_bits = 8,
    .rounding = SGI_ROUND_HALF_AWAY,
    .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
    .arith = SGI_ARITH_DOUBLE,
    .line_rule = SGI_LINE_COVERAGE_DOUBLE,
    .smooth_primitives = true,
    .smooth_rule = SGI_SMOOTH_TWO_PIXEL_FILTER,
};

/* Resource coordinates count rows up, while the bank arrays count down. */
static bool mgras_gl_render_bank(SGIMgrasState *s,
                                 const SGIRenderResource *resource,
                                 uint32_t **pixels, uint8_t **alpha,
                                 uint8_t **format)
{
    if (resource->role != SGI_RENDER_COLOUR ||
        resource->buffer != SGI_RENDER_FRONT ||
        resource->width != s->width || resource->height != s->height ||
        resource->depth != 1 || resource->component_count != 4 ||
        !resource->bottom_origin) {
        return false;
    }
    if (resource->target == 1 && resource->backing_id == 1 &&
        s->front_count == s->width * s->height) {
        *pixels = s->front;
        *alpha = s->front_alpha;
        *format = s->format;
    } else if (resource->target == 2 && resource->backing_id == 2 &&
               s->back_count == s->width * s->height) {
        *pixels = s->back;
        *alpha = s->back_alpha;
        *format = s->back_format;
    } else {
        return false;
    }
    return *pixels && *alpha && *format;
}

static SGIRenderResult mgras_gl_render_snapshot(
    void *opaque, const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp)
{
    SGIMgrasState *s = opaque;
    uint32_t *pixels;
    uint8_t *alpha, *format;

    if (slice || !mgras_gl_render_bank(s, resource, &pixels, &alpha, &format)) {
        error_setg(errp, "MGRAS colour resource has no ordinary bank");
        return SGI_RENDER_RESOURCE_ERROR;
    }
    s->gl_render_snapshots++;
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t idx = (s->height - 1 - y) * s->width + x;

            if (format[idx] != SGI_MGRAS_PIXEL_FORMAT_RGB ||
                (resource->target == 2 && s->slot[0] &&
                 (s->slot[0][idx] & SGI_MGRAS_SLOT_WRITTEN))) {
                error_setg(errp, "MGRAS RGB8 resource overlaps another "
                                  "pixel format or a deep slot");
                return SGI_RENDER_UNSUPPORTED;
            }
        }
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t idx = (s->height - 1 - y) * s->width + x;
            size_t dst = (y - rect->y0) * stride_slots +
                         (x - rect->x0) * 4;
            uint32_t rgb = pixels[idx];

            slots[dst] = (rgb >> 16) & 0xff;
            slots[dst + 1] = (rgb >> 8) & 0xff;
            slots[dst + 2] = rgb & 0xff;
            slots[dst + 3] = alpha[idx];
        }
    }
    return SGI_RENDER_OK;
}

static SGIRenderCommitResult mgras_gl_render_commit_pixels(
    void *opaque, const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    const SGIRenderWriteCoverage *writes, Error **errp)
{
    SGIMgrasState *s = opaque;
    SGIRenderCommitResult result = {
        .status = SGI_RENDER_RESOURCE_ERROR,
        .extent_known = true,
    };
    uint32_t *pixels;
    uint8_t *alpha, *format;
    bool wrote = false;

    if (slice || !mgras_gl_render_bank(s, resource, &pixels, &alpha, &format)) {
        error_setg(errp, "MGRAS colour resource has no ordinary bank");
        return result;
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t row = y - rect->y0;
            size_t col = x - rect->x0;
            size_t idx = (s->height - 1 - y) * s->width + x;

            if (writes && !writes->pixel[row * writes->stride_bytes + col]) {
                continue;
            }
            if (format[idx] != SGI_MGRAS_PIXEL_FORMAT_RGB ||
                (resource->target == 2 && s->slot[0] &&
                 (s->slot[0][idx] & SGI_MGRAS_SLOT_WRITTEN))) {
                error_setg(errp, "MGRAS RGB8 resource changed format or "
                                  "overlaps a deep slot before commit");
                result.status = SGI_RENDER_UNSUPPORTED;
                return result;
            }
        }
    }
    for (uint32_t y = rect->y0; y <= rect->y1; y++) {
        for (uint32_t x = rect->x0; x <= rect->x1; x++) {
            size_t row = y - rect->y0;
            size_t col = x - rect->x0;
            size_t src = row * stride_slots + col * 4;
            SGIRasterColour colour;

            if (!writes || writes->pixel[row * writes->stride_bytes + col]) {
                colour = (SGIRasterColour) { .component = {
                    slots[src], slots[src + 1], slots[src + 2],
                    slots[src + 3],
                } };
                mgras_gl_store_rgba(s, resource->target, x, y, &colour);
                wrote = true;
            }
            result.complete_pixels++;
        }
    }
    s->gl_render_commits++;
    s->dirty |= wrote;
    result.status = SGI_RENDER_OK;
    return result;
}

static SGIRenderCommitResult mgras_gl_render_commit(
    void *opaque, const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, Error **errp)
{
    return mgras_gl_render_commit_pixels(opaque, resource, slice, rect,
                                         slots, stride_slots, NULL, errp);
}

static SGIRenderCommitResult mgras_gl_render_commit_covered(
    void *opaque, const SGIRenderResource *resource, uint32_t slice,
    const SGIRenderRect *rect, const uint32_t *slots, size_t stride_slots,
    size_t capacity_slots, const SGIRenderWriteCoverage *writes, Error **errp)
{
    SGIMgrasState *s = opaque;

    if (s->gl_render_inject_commit_failure) {
        /* Fail after the snapshot and before any native write. */
        SGIRenderCommitResult result = {
            .status = SGI_RENDER_RESOURCE_ERROR,
            .extent_known = true,
        };

        s->gl_render_inject_commit_failure = 0;
        error_setg(errp, "injected MGRAS native commit failure");
        return result;
    }
    return mgras_gl_render_commit_pixels(opaque, resource, slice, rect,
                                         slots, stride_slots, writes, errp);
}

static const SGIRenderResourceOps mgras_gl_render_ops = {
    .snapshot = mgras_gl_render_snapshot,
    .commit = mgras_gl_render_commit,
    .commit_covered = mgras_gl_render_commit_covered,
};

static SGIRenderResource mgras_gl_render_resource(const SGIMgrasState *s,
                                                   SGIColourTarget front)
{
    return (SGIRenderResource) {
        .backing_id = front,
        .backing_generation = 1,
        .view_id = front,
        .view_generation = 1,
        .role = SGI_RENDER_COLOUR,
        .target = front,
        .buffer = SGI_RENDER_FRONT,
        .eye = SGI_RENDER_MONO,
        .width = s->width,
        .height = s->height,
        .depth = 1,
        .bottom_origin = true,
        .component_count = 4,
        .component = {
            { SGI_RENDER_R, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_G, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_B, SGI_RENDER_UNORM, 8 },
            { SGI_RENDER_A, SGI_RENDER_UNORM, 8 },
        },
    };
}

/*
 * The callbacks may run on band workers (concurrent_pixels, unless the
 * x-raster-bands property is off): each writes only its own pixel's
 * elements of the planes, and reads the clip-ID plane, the context image
 * and the texture views, which nothing changes during a draw. They keep
 * no counters or caches; the one log message is thread-safe. Its lines,
 * and the rasterizer's once-per-draw reports, can come out in a different
 * order than from one thread; their text and count, and every pixel, do
 * not change. A slice's draw on the board's thread has let go of the big
 * lock, but its workers touch only what the draw itself would, and finish
 * before it takes the lock back (mgras_gl_soft_draw()).
 */
static const SGIRasterTarget mgras_gl_target = {
    .store_colour = mgras_gl_store_colour,
    .load_colour = mgras_gl_load_colour,
    .store_rgba = mgras_gl_store_rgba,
    .load_rgba = mgras_gl_load_rgba,
    .store_index = mgras_gl_store_index,
    .load_depth = mgras_gl_load_depth,
    .store_depth = mgras_gl_store_depth,
    .load_stencil = mgras_gl_load_stencil,
    .store_stencil = mgras_gl_store_stencil,
};

/* The GL target for @s's own drawing. */
static SGIRasterTarget mgras_gl_raster_target(SGIMgrasState *s)
{
    SGIRasterTarget t = mgras_gl_target;

    t.opaque = s;
    t.concurrent_pixels = s->raster_bands;
    t.band_min_pixels = s->raster_band_pixels;
    return t;
}

/* Host-only qtest access to one typed colour pixel of the current front. */
static bool mgras_gl_test_resource(SGIMgrasState *s,
                                   SGIRasterTarget *target,
                                   SGIRenderResource *resource,
                                   SGIRenderRect *rect, Error **errp)
{
    if (!DEVICE(s)->realized || s->test_pixel_x >= s->width ||
        s->test_pixel_y >= s->height) {
        error_setg(errp, "MGRAS resource test pixel is outside the board");
        return false;
    }
    *resource = mgras_gl_render_resource(s,
                            mgras_gl(s)->cur.drawable.front);
    *target = mgras_gl_raster_target(s);
    target->resources = resource;
    target->resource_count = 1;
    target->resource_ops = &mgras_gl_render_ops;
    rect->x0 = rect->x1 = s->test_pixel_x;
    rect->y0 = rect->y1 = s->height - 1 - s->test_pixel_y;
    return true;
}

static void mgras_gl_test_get_resource(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    SGIRasterTarget target;
    SGIRenderResource resource;
    SGIRenderRect rect;
    uint32_t slots[4], value;

    if (!mgras_gl_test_resource(s, &target, &resource, &rect, errp) ||
        sgi_render_resource_snapshot(&target, &resource, 0, &rect, slots,
                                     4, ARRAY_SIZE(slots), errp) !=
            SGI_RENDER_OK) {
        return;
    }
    value = slots[0] << 24 | slots[1] << 16 | slots[2] << 8 | slots[3];
    visit_type_uint32(v, name, &value, errp);
}

static void mgras_gl_test_set_resource(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(obj);
    SGIRasterTarget target;
    SGIRenderResource resource;
    SGIRenderRect rect;
    uint32_t value, slots[4];

    visit_type_uint32(v, name, &value, errp);
    if (errp && *errp) {
        return;
    }
    slots[0] = value >> 24;
    slots[1] = (value >> 16) & 0xff;
    slots[2] = (value >> 8) & 0xff;
    slots[3] = value & 0xff;
    if (!mgras_gl_test_resource(s, &target, &resource, &rect, errp)) {
        return;
    }
    sgi_render_resource_commit(&target, &resource, 0, &rect, slots,
                               4, ARRAY_SIZE(slots), errp);
}

static void mgras_gl_selection_hit(void *opaque, double minimum,
                                   double maximum)
{
    SGIMgrasState *s = opaque;
    SGIMgrasGLSelection *sel = &s->gl_selection[s->gl_current];
    float min_f = minimum, max_f = maximum;
    uint32_t min_bits, max_bits;

    memcpy(&min_bits, &min_f, sizeof(min_bits));
    memcpy(&max_bits, &max_f, sizeof(max_bits));
    if (!sel->pending) {
        sel->depth_min = min_bits;
        sel->depth_max = max_bits;
        sel->pending = true;
        return;
    }
    {
        float old_min, old_max;

        memcpy(&old_min, &sel->depth_min, sizeof(old_min));
        memcpy(&old_max, &sel->depth_max, sizeof(old_max));
        if (min_f < old_min) {
            sel->depth_min = min_bits;
        }
        if (max_f > old_max) {
            sel->depth_max = max_bits;
        }
    }
}

static SGIGLDrawResult mgras_gl_selection_draw(SGIMgrasState *s,
                                               const SGIGLDraw *d)
{
    switch (sgi_gl_select_draw(d, mgras_gl_selection_hit, s)) {
    case SGI_GL_SELECT_UNSUPPORTED_STATE:
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: selection ignores unsupported "
                      "GL geometry state\n");
        break;
    case SGI_GL_SELECT_UNSUPPORTED_MODE:
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: selection ignores unsupported "
                      "primitive mode 0x%x\n", d->mode);
        break;
    case SGI_GL_SELECT_OK:
        break;
    }
    return SGI_GL_DRAW_OK;
}

/* The scalar path: the draw as a board without Mesa makes it. */
static SGIGLDrawResult mgras_gl_soft_draw(SGIMgrasState *s,
                                          const SGIGLDraw *d)
{
    SGIRasterTarget t = mgras_gl_raster_target(s);
    SGIGLTexImage image;
    SGIGLDrawResult result;

    if (s->gl_selection[s->gl_current].active) {
        return mgras_gl_selection_draw(s, d);
    }
    /* Stands for the rasterizer failing an allocation before it draws. */
    if (s->gl_scalar_inject_oom) {
        s->gl_scalar_inject_oom = 0;
        return SGI_GL_DRAW_OUT_OF_MEMORY;
    }
    t.texture = sgi_mgras_texture_image(s, &image);
    /*
     * In a slice of display-list work on the board's thread the draw runs
     * without the big lock, which a large one would otherwise hold for
     * milliseconds, keeping the vCPUs from their interrupts. It reads
     * and writes only the planes and the state the command stream sets;
     * meanwhile an access that would touch either waits for the slice,
     * and command-FIFO words queue (sgi-mgras-raster.c).
     */
    if (sgi_head_work_may_unlock(&s->work)) {
        sgi_head_work_unlock(&s->work);
        result = sgi_soft_draw(&mgras_gl_raster_rules, &t, d);
        sgi_head_work_relock(&s->work);
    } else {
        result = sgi_soft_draw(&mgras_gl_raster_rules, &t, d);
    }
    s->dirty = true;
    return result;
}

/*
 * A draw that ends on the scalar path on a board with Mesa, committed and
 * reported as a board without Mesa does. That board also draws through the
 * checked callback, because its polygon modes are always on, but with no
 * result consumer: a scalar allocation failure there retains the shared
 * GL out-of-memory error and the command's later draws still run. Here
 * the command's result belongs to Mesa, so retain the error directly and
 * report success, which latches nothing and ends no command.
 */
static SGIGLDrawResult mgras_gl_scalar(SGIMgrasState *s, const SGIGLDraw *d)
{
    SGIGLContext *c = mgras_gl(s);

    if (mgras_gl_soft_draw(s, d) == SGI_GL_DRAW_OUT_OF_MEMORY &&
        !c->polygon.error) {
        c->polygon.error = SGI_GL_OUT_OF_MEMORY;
    }
    return SGI_GL_DRAW_OK;
}

/*
 * A refused Mesa attempt changed nothing, so the scalar path draws the same
 * draw here, before the callback returns.
 */
static SGIGLDrawResult mgras_gl_fall_back(SGIMgrasState *s,
                                          const SGIGLDraw *d,
                                          SGIRenderRefusal reason)
{
    s->gl_mesa_fallbacks[reason]++;
    trace_sgi_mgras_gl_mesa_fallback(sgi_render_refusal_name(reason),
                                     s->gl_render_op, d->mode, d->count);
    return mgras_gl_scalar(s, d);
}

/*
 * The draws this caller sends to Mesa: one triangle in a bound drawable at
 * the origin, with one clip rectangle covering the board, no scissor box
 * and no client clip plane, into a bank the resource bridge can expose.
 * Everything else is refused here, before any native access.
 */
static SGIRenderRefusal mgras_gl_mesa_preflight(const SGIMgrasState *s,
                                                const SGIGLDraw *d)
{
    const SGIGLDrawState *st = d->state;

    if (d->mode != SGI_GL_TRIANGLES || d->count != 3) {
        return SGI_RENDER_REFUSAL_PRIMITIVE;
    }
    if (!st->drawable.bound || st->drawable.origin[0] ||
        st->drawable.origin[1] || st->drawable.clip.n != 1 ||
        st->drawable.clip.r[0].x0 != 0 || st->drawable.clip.r[0].y0 != 0 ||
        st->drawable.clip.r[0].x1 != s->width - 1 ||
        st->drawable.clip.r[0].y1 != s->height - 1 || st->scissor_test ||
        (s->context[MGRAS_GL_IMAGE_CLIP_ID] >> 4) & 0xf) {
        return SGI_RENDER_REFUSAL_WINDOW;
    }
    if ((st->drawable.front != 1 && st->drawable.front != 2) ||
        (st->draw_buffers & SGI_GL_DRAW_OVERLAY)) {
        return SGI_RENDER_REFUSAL_BUFFERS;
    }
    if (st->clip_planes) {
        return SGI_RENDER_REFUSAL_CLIP_PLANES;
    }
    return SGI_RENDER_REFUSAL_NONE;
}

/*
 * A GL context's draw, rasterized into the banks and the depth plane. With
 * the opt-in Mesa renderer each draw runs wholly on Mesa or wholly on the
 * scalar path. Mesa runs as for a frame mixed with scalar draws, so it
 * accepts only state whose pixels are proven identical to the scalar
 * path's. A refusal falls back here; a failure is never retried.
 */
static SGIGLDrawResult mgras_gl_draw_checked(void *opaque, const SGIGLDraw *d)
{
    SGIMgrasState *s = opaque;
    SGIRasterTarget t = mgras_gl_raster_target(s);
    SGIGLDraw render_draw = *d;
    SGIRenderResource resource;
    SGIRenderAttemptOptions opts = { .mixed_frame = true };
    SGIRenderAttemptOutcome outcome = { 0 };
    SGIRenderDrawResult attempt;
    SGIRenderRefusal refusal;
    Error *err = NULL;

    if (!s->gl_renderer_opt_in || s->gl_renderer_scalar) {
        return mgras_gl_soft_draw(s, d);
    }
    /*
     * Selection draws, and draws with no clip rectangle to land in, put no
     * pixels on either path, so Mesa could never take them over: they are
     * not fallbacks.
     */
    if (s->gl_selection[s->gl_current].active ||
        !d->state->drawable.bound || !d->state->drawable.clip.n) {
        return mgras_gl_scalar(s, d);
    }
    refusal = mgras_gl_mesa_preflight(s, d);
    if (refusal != SGI_RENDER_REFUSAL_NONE) {
        return mgras_gl_fall_back(s, d, refusal);
    }
    /* An injected failure stands for a failed attempt. */
    if (s->gl_render_inject_result) {
        uint32_t injected = s->gl_render_inject_result;

        s->gl_render_inject_result = 0;
        s->gl_render_validated = true;
        return injected == SGI_GL_DRAW_OUT_OF_MEMORY ||
               injected == SGI_GL_DRAW_HOST_ERROR ? injected :
                                                   SGI_GL_DRAW_RESOURCE_ERROR;
    }
    if (s->gl_renderer_unavailable || !s->gl_renderer) {
        s->gl_render_validated = true;
        return s->gl_render_result != SGI_GL_DRAW_OK ?
               s->gl_render_result : SGI_GL_DRAW_HOST_ERROR;
    }
    if (d->polygon_modes &&
        d->polygon_modes[0] == SGI_GL_FILL &&
        d->polygon_modes[1] == SGI_GL_FILL) {
        render_draw.polygon_modes = NULL;
    }
    resource = mgras_gl_render_resource(s, d->state->drawable.front);
    t.resources = &resource;
    t.resource_count = 1;
    t.resource_ops = &mgras_gl_render_ops;
    attempt = sgi_render_draw_attempt(s->gl_renderer, &mgras_gl_raster_rules,
                                      &t, &render_draw, &opts, &outcome,
                                      &err);
    if (attempt == SGI_RENDER_DRAW_REFUSED &&
        outcome.reason > SGI_RENDER_REFUSAL_NONE &&
        outcome.reason < SGI_RENDER_REFUSAL__MAX) {
        /* The reason is counted; the error text would repeat per draw. */
        error_free(err);
        return mgras_gl_fall_back(s, d, outcome.reason);
    }
    s->gl_render_validated = true;
    if (attempt == SGI_RENDER_DRAW_OK) {
        s->dirty = true;
        return SGI_GL_DRAW_OK;
    }
    if (outcome.status != SGI_RENDER_OOM) {
        sgi_renderer_free(s->gl_renderer);
        s->gl_renderer = NULL;
        s->gl_renderer_unavailable = true;
    }
    if (err) {
        error_report("sgi-mgras: Mesa checked draw: %s",
                     error_get_pretty(err));
        error_free(err);
    }
    switch (outcome.status) {
    case SGI_RENDER_OOM:
        return SGI_GL_DRAW_OUT_OF_MEMORY;
    case SGI_RENDER_HOST_ERROR:
        return SGI_GL_DRAW_HOST_ERROR;
    default:
        return SGI_GL_DRAW_RESOURCE_ERROR;
    }
}

static void mgras_gl_draw(void *opaque, const SGIGLDraw *d)
{
    mgras_gl_draw_checked(opaque, d);
}

/* A GL context's clear or bitmap. */
static void mgras_gl_raster(void *opaque, const SGIRasterOp *op)
{
    SGIMgrasState *s = opaque;
    SGIRasterTarget t = mgras_gl_raster_target(s);

    if (s->gl_selection[s->gl_current].active) {
        return;
    }
    sgi_soft_raster(&mgras_gl_raster_rules, &t, op);
    s->dirty = true;
}

/*
 * The client writes the opener first when it sets up a new context, and the
 * setup that follows sends no matrix, so the context must already hold the
 * OpenGL initial state then. The model restores it in the loaded context's
 * entry alone; the other contexts and the banks the image gave are the
 * board's. The discriminating tests are
 * gl-context-setup-restores-initial-state and
 * gl-context-opener-resets-only-its-context.
 */
static void mgras_gl_context_start(SGIMgrasState *s)
{
    SGIGLContext *gl = mgras_gl(s);

    memset(&s->gl_selection[s->gl_current], 0,
           sizeof(s->gl_selection[s->gl_current]));
    sgi_gl_reset(gl, gl->hw_id);
    s->gl_context[s->gl_current].client_read_bracket = 0;
    s->gl_context[s->gl_current].client_read_kicked = 0;
    sgi_mgras_texture_reset(&s->gl_context[s->gl_current].tex);
    mgras_gl_bind(s);
}

/*
 * No MGRAS command is a glFinish: the client's glFinish is a query and a
 * status poll, which the HQ answers itself.
 */
static SGIGLSink mgras_gl_sink(SGIMgrasState *s)
{
    return (SGIGLSink) {
        .draw = mgras_gl_draw,
        .draw_checked = mgras_gl_draw_checked,
        .raster = mgras_gl_raster,
        .opaque = s,
    };
}

static void mgras_gl_command(SGIMgrasState *s, SGIGLOp op,
                             const uint32_t *args, unsigned nargs)
{
    SGIGLSink sink = mgras_gl_sink(s);
    SGIGLCmd cmd = { .op = op, .nargs = nargs, .args = args };
    SGIGLDrawResult checked = SGI_GL_DRAW_OK;

    /*
     * A board without Mesa on this host draws as a board that never asked
     * for it, with no Mesa result for the command.
     */
    if (s->gl_renderer_opt_in && !s->gl_renderer_scalar) {
        sink.result = &checked;
    }
    s->gl_render_validated = false;
    s->gl_render_op = op;
    if (op == SGI_GL_OP_BEGIN && !s->gl_renderer_unavailable) {
        s->gl_render_result = SGI_GL_DRAW_OK;
    }
    sgi_gl_command(mgras_gl(s), &sink, &cmd);
    if (s->gl_renderer_opt_in && checked != SGI_GL_DRAW_OK) {
        s->gl_render_result = checked;
        static const char *const failures[] = {
            "ok", "out of memory", "unsupported state",
            "resource failure", "host failure",
        };
        const char *failure = checked <= SGI_GL_DRAW_HOST_ERROR ?
                              failures[checked] : "invalid result";

        error_report("sgi-mgras: Mesa checked draw %s in GL command %u",
                     failure, op);
        if (checked == SGI_GL_DRAW_HOST_ERROR ||
            checked == SGI_GL_DRAW_RESOURCE_ERROR) {
            s->gl_renderer_unavailable = true;
            assert(bql_locked());
            vm_stop(RUN_STATE_INTERNAL_ERROR);
        }
    }
    if (mgras_gl(s)->polygon.error == SGI_GL_OUT_OF_MEMORY &&
        !s->gl_oom_logged) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: GL draw allocation "
                      "failed; shared GL error retained\n");
        s->gl_oom_logged = true;
    }
}

/* A colour index, as the float the shared context keeps. */
static void mgras_gl_index(SGIMgrasState *s, float index)
{
    uint32_t word;

    memcpy(&word, &index, sizeof(word));
    mgras_gl_command(s, SGI_GL_OP_INDEXF, &word, 1);
}

/*
 * Where glBitmap's image is while its words arrive: the parser keeps the
 * 0x9420 packet's eight words in its payload, and these slots after them,
 * so a context switch inside the image carries them with the context's
 * parser position.
 */
#define MGRAS_GL_BITMAP_PLACED   8
#define MGRAS_GL_BITMAP_X        9
#define MGRAS_GL_BITMAP_Y        10
#define MGRAS_GL_BITMAP_SKIP     11
#define MGRAS_GL_BITMAP_NEXT     12
QEMU_BUILD_BUG_ON(MGRAS_GL_BITMAP_NEXT >= SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS);

static void mgras_gl_bitmap(SGIMgrasState *s, const uint32_t *payload)
{
    uint32_t *slot = s->cmd_token_payload;
    int32_t width = payload[1], height = payload[2];
    float f[4];
    SGIGLBitmap b;
    bool placed;

    if ((payload[0] & MGRAS_GL_BITMAP_LSB_FIRST) ||
        (payload[0] & 0xf) != 0xf) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: glBitmap flags 0x%x are not "
                      "the form the client sends; drawn as that form\n",
                      payload[0]);
    }
    /*
     * A negative size is GL_INVALID_VALUE and the command is ignored
     * (OpenGL 1.2.1, 3.7): the image after it is consumed, not drawn, and
     * the raster position stays.
     */
    if (width < 0 || height < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: glBitmap of %d by %d "
                      "ignored\n", width, height);
        placed = false;
    } else {
        memcpy(f, &payload[3], sizeof(f));
        placed = sgi_gl_bitmap_place(mgras_gl(s), width, height, f[0], f[1],
                                     f[2], f[3], &b);
    }
    slot[MGRAS_GL_BITMAP_PLACED] = placed && width && height;
    slot[MGRAS_GL_BITMAP_X] = placed ? b.x : 0;
    slot[MGRAS_GL_BITMAP_Y] = placed ? b.y : 0;
    slot[MGRAS_GL_BITMAP_SKIP] = 0;
    slot[MGRAS_GL_BITMAP_NEXT] = 0;
}

/*
 * Pixel image transfers: glDrawPixels, and IRIS GL's lrectwrite and
 * rectwrite, which the IRIX 6.5.30 IGLOO GL library sends the same way.
 * The client (the pixel send routine in the GL client library, SHA-256
 * 626d6d94...) does not send the image's format to the board.
 * It builds a program for the GE out of routines, loads each routine's
 * parameters into GE memory with 0x80 and 0xcd packets, sends 0x8d20 and
 * eight words, and then the image: either inline, one block a row
 * (through the pixel push routine), or through one
 * host-memory DMA transfer (the DMA setup routine, kick byte
 * (slot << 1) | 0xa1). The GE runs the program over the pixels and draws
 * them.
 *
 * The loads. The CRAM pixel-state initializer and
 * the ERAM pixel-state initializer collect two lists of words and
 * send them as one packet, header 0x8000 or 0xcd00 with the byte length
 * 0x20 + 4 * (a + b) in bits 7:0, or, from 0x100 bytes up, that header
 * with no length and an inline block giving it. The payload is seven
 * words, a / 5 and a % 5 (a / 4 and a % 4 for 0xcd), 4 * a, the address,
 * 0, (a == 0) | (b == 0) << 1 and a; then the a words of the first list,
 * b, and the b words of the second. Established: the words, from the
 * listing and from every load in the captures below. Inferred: that the
 * GE stores the first list from the address on, which is what each
 * load's name and the routines reading it back need. The model stores the
 * first list in ge_param[] and consumes the second: it was either empty or
 * the same as the first in every load captured, and what the GE does with
 * it is unknown. Whether 0x80 and 0xcd address one memory or two is
 * unknown; the client never used an address in both, and the model keeps
 * one. It keeps one for the board, too, where the kernel may save and
 * restore GE memory at a context switch; a context whose program another
 * context overwrote between its load and its draw would draw with the
 * other's here.
 *
 * The program. The client puts it at 0x13b as (routine, parameters)
 * pairs ending with routine 0x352d, and puts the destination rectangle at
 * 0xe34 (the host-to-RSS initializer): x0, y0, x1, y1, then
 * height << 16 | width, then 1. The rectangle is the image's own, its
 * tiles' offsets from the raster position: mandel's 293 rows all come at
 * 0, 0 to 99, 0 while its glRasterPos2i moves up the window, and ipaste's
 * two tiles at x 0 and 69. The GE adds the raster position, which 0xd700
 * loads before each draw (inferred from those and the word's emitter's
 * name).
 * The routine and parameter words are GE addresses as the client passes
 * them, the same in every capture, and the model treats them as opaque
 * values; it recognises the three programs the survey programs build,
 * each with the pixel transfer state at OpenGL's initial values:
 *
 *   0x1f44/0, 0x3e65/0xd91, 0x156a/0x1b4, 0x352d/0xdf3, with 0x4064 in
 *   0xd91's second word: 4 unsigned bytes a pixel (ipaste, i3dm);
 *   0x1f44/0, 0x1655/0xd91, 0x3e65/0x1d6, 0x156a/0x1b4, 0x352d/0xdf3,
 *   with 0x40f2 there: 3 unsigned bytes a pixel, GL_RGB (imgview);
 *   0x1a3e/0, 0x352d/p: colour indices (mandel's rectwrite).
 *
 * Any other program, and a pixel zoom other than 1 (the client loads 0.0
 * and 1 / y zoom at 0x191, both floats), is logged and its image consumed
 * without drawing: its rows, or its DMA transfer, which moves nothing.
 * Of the four-byte form the client marks GL_ABGR_EXT with bit 7 of the
 * HQ pixel-format word (0x000c0004 before 0x8d20, value 0xe8d in ipaste;
 * set when the format is 0x8000); without it the
 * model reads GL_RGBA (inferred: no capture has it).
 *
 * The rows. A block is a row of the waiting draw only when it follows
 * the draw's 0x8d20 or the row before (and the zero word after an odd
 * one), and only when 0x8d20's last word lacks bit 7, which the client
 * sets only on its DMA path; any other block is the packet's before it.
 *
 * The draw. Pixel n of a row lands on the window pixel whose centre lies
 * between xr + n and xr + n + 1 for the raster position's x, and the same
 * for rows (OpenGL 1.2.1, 3.6.4); an invalid raster position draws
 * nothing. 0x8d20's first two words together are the image's words a
 * row: 69 and 0 for ipaste's 69-pixel tiles of four bytes, 51 and 0 for
 * imgview's 68 pixels of three on the SI, 50 and 0 for mandel's 100
 * indices, and 48 and 48 for imgview's 128 pixels of three on the SSI
 * (inferred from those; they are two words of the library's tile
 * descriptor in the 64-bit library's listing, which sets bit 4 of 0x8d20's last
 * word when the second is 0). Why the SSI's client splits a row in two
 * is not known: the image bytes are whole rows. A colour-index image's
 * bytes a
 * pixel are the 1, 2 or 4 that give that many words, which is 2 for every
 * rectwrite captured; an image too narrow to tell (one index wide, or two
 * at 1 or 2 bytes) is logged and not drawn. The image is the bottom row
 * first and each row's first pixel leftmost (OpenGL 1.2.1, 3.6.4), host
 * bytes in the order EXT_abgr or glDrawPixels names; an index is
 * big-endian. Each pixel goes down as a fragment of the raster position's
 * depth through the same path as glBitmap's, so the window's clip, the
 * depth test and the index write mask apply (3.6.5, 4.1). With the
 * transfer state at its initial values the program's result is the host
 * colour or index unchanged (3.6.5), which is what the model draws:
 * inferred, as the GE program is not read.
 *
 * Evidence: a captured run, a trace of ipaste, imgview,
 * mandel, snapshot and i3dm on the SI, and imgview's draws on the SSI
 * logged by the model. The discriminating tests are
 * gl-pixels-inline-colour-index, gl-pixels-inline-rgb, gl-pixels-dma-abgr,
 * gl-pixels-dma-rgba-and-rgb, gl-pixels-dma-rows-in-two-parts,
 * gl-pixels-unmodelled-draw-nothing,
 * gl-pixels-rows-follow-their-draw, gl-pixels-raster-kick-leaves-the-draw,
 * gl-pixels-far-raster-position, gl-pixels-follow-the-raster-position,
 * gl-pixels-long-load, gl-pixels-follow-the-context and
 * gl-pixels-migration, and the replay fixture gl-pixels-rectwrite, one
 * row of mandel's words from that run.
 */
static const struct {
    uint32_t pairs;
    uint32_t routine[5];
    uint32_t unpack;            /* the routine's word, 0 for none */
} mgras_gl_pixel_programs[] = {
    [MGRAS_GL_PIXELS_ABGR8] = { 4, { 0x1f44, 0x3e65, 0x156a, 0x352d },
                                MGRAS_GE_UNPACK_4UB },
    [MGRAS_GL_PIXELS_RGB8] = { 5, { 0x1f44, 0x1655, 0x3e65, 0x156a, 0x352d },
                               MGRAS_GE_UNPACK_3UB },
    [MGRAS_GL_PIXELS_INDEX] = { 2, { 0x1a3e, 0x352d } },
};

static SGIMgrasGLPixels *mgras_gl_pixels(SGIMgrasState *s)
{
    return &s->gl_context[s->gl_current].pixels;
}

bool sgi_mgras_gl_ge_load(SGIMgrasState *s, uint32_t header)
{
    uint32_t op = (header >> 8) & 0xff;

    if ((header >> 16) || (op != MGRAS_GL_GE_CRAM_OP &&
                           op != MGRAS_GL_GE_ERAM_OP)) {
        return false;
    }
    s->cmd_token_class = 0;
    s->cmd_token_reg = op;
    s->cmd_token_payload_count = 0;
    s->cmd_token_words_remaining = 0;
    /* No length: the inline block after it carries the words. */
    if (!(header & 0xff)) {
        return true;
    }
    s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_GE_LOAD;
    s->cmd_token_words_remaining = DIV_ROUND_UP(header & 0xff,
                                                sizeof(uint32_t));
    memset(s->cmd_token_payload, 0, sizeof(s->cmd_token_payload));
    s->cmd_token_payload[MGRAS_GL_GE_LOAD_TOTAL] =
        s->cmd_token_words_remaining;
    s->cmd_token_payload_count = MGRAS_GL_GE_LOAD_SLOTS;
    return true;
}

void sgi_mgras_gl_ge_load_word(SGIMgrasState *s, uint32_t word)
{
    uint32_t *p = s->cmd_token_payload;
    uint32_t k = p[MGRAS_GL_GE_LOAD_DONE]++;
    uint32_t first = p[MGRAS_GL_GE_LOAD_COUNT];
    uint32_t total = p[MGRAS_GL_GE_LOAD_TOTAL];
    /* The words both lists share: all but the head and list two's length. */
    uint32_t lists = total > MGRAS_GL_GE_LOAD_HEAD ?
                     total - MGRAS_GL_GE_LOAD_HEAD - 1 : 0;

    if (k < MGRAS_GL_GE_LOAD_HEAD) {
        p[k] = word;
        if (k == MGRAS_GL_GE_LOAD_COUNT && word > lists) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: GE parameter load of "
                          "%u words names %u in its first list; nothing "
                          "stored\n", total, word);
            p[MGRAS_GL_GE_LOAD_COUNT] = 0;
        }
    } else if (k < MGRAS_GL_GE_LOAD_HEAD + first) {
        uint32_t addr = p[MGRAS_GL_GE_LOAD_ADDR] + k - MGRAS_GL_GE_LOAD_HEAD;

        if (addr < SGI_MGRAS_GE_PARAM_WORDS) {
            s->ge_param[addr] = word;
        } else if (!s->ge_param_logged) {
            s->ge_param_logged = true;
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: GE parameter load at 0x%x "
                          "is past the 0x%x words the model keeps\n", addr,
                          SGI_MGRAS_GE_PARAM_WORDS);
        }
    } else if (k == MGRAS_GL_GE_LOAD_HEAD + first) {
        p[MGRAS_GL_GE_LOAD_SECOND] = word;
        if (word != lists - first) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: GE parameter load of "
                          "%u words gives lists of %u and %u\n", total,
                          first, word);
        }
    }
    if (!s->cmd_token_words_remaining) {
        s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_DISCARD;
    }
}

/* Captured float RGBA headers whose inline rows are understood below. */
static bool mgras_gl_pixels_captured_float_rgba(uint32_t width,
                                                uint32_t height,
                                                uint64_t row_words,
                                                bool dma_only,
                                                uint32_t unpack_first)
{
    uint64_t pixels = (uint64_t)width * height;

    return !dma_only &&
           ((row_words == pixels * 4 &&
             ((width == 2 && height == 1 && unpack_first == 1) ||
              (width == 1 && height == 2 && unpack_first == 1) ||
              (width == 2 && height == 2 && unpack_first == 2) ||
              (width == 2 && height == 3 && unpack_first == 3) ||
              (width == 3 && height == 2 && unpack_first == 3) ||
              (width == 2 && height == 4 && unpack_first == 4) ||
              (width == 4 && height == 2 && unpack_first == 4))) ||
            (width == 47 && height == 1 && row_words == pixels * 4 &&
             unpack_first == 0x17) ||
            (width == 1 && height == 1 && row_words == pixels * 4 &&
             unpack_first == 0) ||
            (width == 17 && height == 1 && row_words == pixels * 4 &&
             unpack_first == 8) ||
            (width == 32 && height == 1 && row_words == pixels * 4 &&
             unpack_first == 0x10) ||
            (width == 2 && height == 32 && row_words == 0x80 &&
             unpack_first == 0x10));
}

/*
 * Which of mgras_gl_pixel_programs[] the GE holds at 0x13b, or
 * MGRAS_GL_PIXELS_FORMATS for none of them; *pairs is its length.
 */
static uint32_t mgras_gl_pixels_float_signature(const uint32_t *ge,
                                                uint32_t width,
                                                uint32_t height,
                                                uint64_t row_words,
                                                bool dma_only)
{
    static const uint32_t rgb_routines[] = {
        0x1655, 0x3e65, 0x156a, 0x352d,
    };
    static const uint32_t rgba_routines[] = {
        0x3e65, 0x156a, 0x352d,
    };
    static const uint32_t rgb_unpack_fixed[] = {
        0x40f2, 0x464, 0, 6, 0xfffffee4, 0x3f800000,
    };
    static const uint32_t rgb_unpack_indices[] = {
        0x1d7, 0x1d8, 0x1d9, 0x1db, 0x1dc, 0x1dd,
    };
    static const uint32_t rgba_unpack[] = {
        0, 0x4064, 0x464, 0x44,
    };
    bool rgb = true;
    bool rgba = true;
    bool rgba_packet = ge[0x1b5] == 0 ||
        (ge[0x1b5] == 4 && height == 1 &&
         ((width == 47 && ge[0x1b4] == 0x17) ||
          (width == 1 && ge[0x1b4] == 0) ||
          (width == 17 && ge[0x1b4] == 8)));

    for (uint32_t i = 0; i < ARRAY_SIZE(rgb_routines); i++) {
        rgb &= ge[MGRAS_GE_PIXEL_PROGRAM + 2 * i] == rgb_routines[i];
    }
    for (uint32_t i = 0; i < ARRAY_SIZE(rgba_routines); i++) {
        rgba &= ge[MGRAS_GE_PIXEL_PROGRAM + 2 * i] == rgba_routines[i];
    }
    rgb &= (width == 2 && height == 2 && row_words == 6 &&
            ge[0x1d6] == 0x1c && ge[0x1da] == 2) ||
           (width == 3 && height == 2 && row_words == 9 &&
            ge[0x1d6] == 0x24 && ge[0x1da] == 3);
    for (uint32_t i = 0; rgb && i < ARRAY_SIZE(rgb_unpack_fixed); i++) {
        rgb &= ge[rgb_unpack_indices[i]] == rgb_unpack_fixed[i];
    }
    for (uint32_t i = 1; rgba && i < ARRAY_SIZE(rgba_unpack) - 1; i++) {
        rgba &= ge[0x1b5 + i] == rgba_unpack[i];
    }
    if (rgba) {
        uint32_t signature = ge[0x1b8];

        rgba = rgba_packet &&
               mgras_gl_pixels_captured_float_rgba(width, height,
                                                    row_words, dma_only,
                                                    ge[0x1b4]) &&
               signature == 4 + 4 * row_words;
    }
    if (rgb) {
        return MGRAS_GL_PIXELS_FLOAT_RGB;
    }
    if (rgba) {
        return MGRAS_GL_PIXELS_FLOAT_RGBA;
    }
    return MGRAS_GL_PIXELS_FORMATS;
}

static bool mgras_gl_pixels_captured_inline_rgba_count(uint32_t width,
                                                        uint32_t height,
                                                        uint64_t words)
{
    return (width == 1 && height == 2 && words == 1) ||
           (width == 3 && height == 2 && words == 3) ||
           (width == 32 && height == 32 && words == 64);
}

static bool mgras_gl_pixels_has_routine(const uint32_t *ge, uint32_t routine)
{
    for (uint32_t i = 0; i < MGRAS_GE_PIXEL_PROGRAM_PAIRS; i++) {
        if (ge[MGRAS_GE_PIXEL_PROGRAM + 2 * i] == routine) {
            return true;
        }
    }
    return false;
}

static bool mgras_gl_pixels_write_routine(uint32_t routine)
{
    return routine == MGRAS_GE_ROUTINE_END ||
           routine == MGRAS_GE_ROUTINE_ZOOM_Y ||
           routine == MGRAS_GE_ROUTINE_ZOOM_XF;
}

static uint32_t mgras_gl_pixels_match(const uint32_t *ge, uint32_t *pairs)
{
    uint32_t format;

    *pairs = 0;
    while (*pairs < MGRAS_GE_PIXEL_PROGRAM_PAIRS &&
           !mgras_gl_pixels_write_routine(
               ge[MGRAS_GE_PIXEL_PROGRAM + 2 * *pairs])) {
        (*pairs)++;
    }
    (*pairs)++;
    for (format = 0; format < ARRAY_SIZE(mgras_gl_pixel_programs); format++) {
        bool same = mgras_gl_pixel_programs[format].pairs == *pairs;

        for (uint32_t i = 0; same && i < *pairs; i++) {
            uint32_t routine = ge[MGRAS_GE_PIXEL_PROGRAM + 2 * i];

            same = routine == mgras_gl_pixel_programs[format].routine[i] ||
                   (i == *pairs - 1 &&
                    mgras_gl_pixels_write_routine(routine));
        }
        if (same && mgras_gl_pixel_programs[format].unpack) {
            uint32_t params = ge[MGRAS_GE_PIXEL_PROGRAM + 3];

            same = params < SGI_MGRAS_GE_PARAM_WORDS - 1 &&
                   ge[params + 1] == mgras_gl_pixel_programs[format].unpack;
        }
        if (same) {
            return format;
        }
    }
    return MGRAS_GL_PIXELS_FORMATS;
}

/*
 * A pixel-texture draw's GE box: the client draws one pass per RE, and
 * the IRIX client library's pixel-texture setup appends, after the
 * corners, a size word with the pass's row count in its high half, 1, an
 * x and a y direction of +1 or -1, and the pass number. Each pass's
 * corners are the whole image's, the top one at y0 + n * rows for n REs;
 * pass k carries source rows k, k + n, ... The client's glDrawPixels
 * takes this path only with pixel texgen on, which it brackets with
 * MDDMA_CNTRL bit 0, and with a 3-D or 4-D texture bound.
 */
static bool mgras_gl_pixels_texture_box(SGIMgrasState *s, uint32_t *rows,
                                        uint32_t *pass)
{
    const uint32_t *box = &s->ge_param[MGRAS_GE_PIXEL_BOX];
    uint32_t passes = s->board->re_count;

    *rows = box[4] >> 16;
    *pass = box[8];
    /*
     * Not only DMA draws: a zoomed pass ends 0x8d20 with 0x18 though its
     * image comes by DMA (the zoom probe's runs).
     */
    return (s->gl_context[s->gl_current].tex.mddma_cntrl & 1) &&
           box[5] == 1 && box[6] == 1 && box[7] == 1 && *pass < passes &&
           *rows && *rows <= MGRAS_GL_PIXELS_MAX &&
           (int64_t)(int32_t)box[3] - (int32_t)box[1] ==
           (int64_t)*rows * passes;
}

typedef struct MgrasGLZoom {
    uint32_t src_w, src_h;      /* source pixels a row, rows expected */
    uint32_t dest_w, dest_h;    /* destination box in window pixels */
    float zx, zy;               /* the zoom factors' magnitudes */
    uint32_t flip;              /* bit 0: x decreasing; bit 1: y */
    bool zoomed;
} MgrasGLZoom;

static uint32_t float_bits(float f)
{
    uint32_t bits;

    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static float bits_float(uint32_t bits)
{
    float f;

    memcpy(&f, &bits, sizeof(f));
    return f;
}

/*
 * The draw's zoom, from the words the client loads for glPixelZoom (model
 * runs of a zoom probe on SI+T and MXI): the box 0xe34 to 0xe37 is the
 * destination in window pixels from the raster position, its corners in
 * the order the image runs, so a negative zoom gives x1 < x0 or y1 < y0;
 * 0xe38's low half is the source width and its high half the destination
 * rows; 0x192 holds 1 / |y zoom| (0x191 is 0). The x zoom is the box's
 * width over the source's. A pixel-texture pass's box rises its rows times
 * the RE count, with the y direction in 0xe3b. Source rows are the
 * destination's times 1 / |y zoom|, rounded up; a DMA image gives its own.
 * How the GE rounds a fractional zoom is not published: the model fills
 * the window pixels whose centres the source pixel covers (OpenGL 1.2.1,
 * 3.6.4), counted from the box's first corner.
 */
static bool mgras_gl_pixels_zoom(SGIMgrasState *s, bool pixel_texture,
                                 uint32_t pass_rows, MgrasGLZoom *z)
{
    const uint32_t *ge = s->ge_param;
    const uint32_t *box = &ge[MGRAS_GE_PIXEL_BOX];
    int64_t dx = (int64_t)(int32_t)box[2] - (int32_t)box[0];
    int64_t dy = (int64_t)(int32_t)box[3] - (int32_t)box[1];
    float zinv = bits_float(ge[MGRAS_GE_PIXEL_ZOOM + 1]);
    double rows;

    *z = (MgrasGLZoom) { .src_w = box[4] & 0xffff };
    if (ge[MGRAS_GE_PIXEL_ZOOM] || !isfinite(zinv) || zinv <= 0 ||
        !z->src_w || z->src_w > MGRAS_GL_PIXELS_MAX) {
        return false;
    }
    z->dest_w = (dx < 0 ? -dx : dx) + 1;
    if (pixel_texture) {
        z->dest_h = pass_rows * MAX(s->board->re_count, 1);
        z->flip = (ge[MGRAS_GE_PIXEL_BOX + 6] != 1) |
                  (ge[MGRAS_GE_PIXEL_BOX + 7] != 1) << 1;
    } else {
        z->dest_h = (dy < 0 ? -dy : dy) + 1;
        if (z->dest_h != box[4] >> 16) {
            return false;
        }
        z->flip = (dx < 0) | (dy < 0) << 1;
    }
    if (z->dest_w > 2 * MGRAS_GL_PIXELS_MAX ||
        z->dest_h > 2 * MGRAS_GL_PIXELS_MAX) {
        return false;
    }
    z->zoomed = zinv != 1.0f || z->dest_w != z->src_w || z->flip;
    if (!z->zoomed) {
        z->src_h = pixel_texture ? pass_rows : z->dest_h;
        return true;
    }
    z->zx = (float)z->dest_w / z->src_w;
    z->zy = 1.0f / zinv;
    rows = ceil((pixel_texture ? pass_rows : z->dest_h) * (double)zinv -
                0.0001);
    if (rows < 1 || rows > MGRAS_GL_PIXELS_MAX) {
        return false;
    }
    z->src_h = rows;
    return true;
}

/* Replace the box's x zoom with the raster engine's, when it has one. */
static void mgras_gl_pixels_raster_zoom(SGIMgrasState *s,
                                        SGIMgrasGLPixels *px)
{
    uint32_t mode = s->rss[MGRAS_RSS_XFRMODE] &
                    (MGRAS_XFRMODE_MAGNIFY | MGRAS_XFRMODE_DECIMATE);
    uint32_t f = s->rss[MGRAS_RSS_ZOOM_STEP];
    double zx, n;

    if (!mode) {
        return;
    }
    if (mode != MGRAS_XFRMODE_MAGNIFY && mode != MGRAS_XFRMODE_DECIMATE) {
        goto unmodelled;
    }
    if (!px->zoom_x || !px->w || !f || f > MGRAS_ZOOM_FACTOR_MAX ||
        s->rss[MGRAS_RSS_ZOOM_INITIAL] != f) {
        goto unmodelled;
    }
    if (mode == MGRAS_XFRMODE_MAGNIFY) {
        zx = f / 256.0;
    } else {
        n = MAX(1, round((double)px->dest_w * f / (256.0 * px->w)));
        zx = n * 256.0 / f;
    }
    px->zoom_x = float_bits(zx);
    return;

unmodelled:
    qemu_log_mask(LOG_UNIMP, "sgi-mgras: raster zoom 0x%x 0x%x under "
                  "transfer mode 0x%x is not modelled; the box's zoom is "
                  "used\n", s->rss[MGRAS_RSS_ZOOM_INITIAL], f,
                  s->rss[MGRAS_RSS_XFRMODE]);
}

/* The rectangle and form of the draw 0x8d20 starts, or false, logged. */
static bool mgras_gl_pixels_program(SGIMgrasState *s, uint64_t row_words,
                                    bool dma_only, SGIMgrasGLPixels *px)
{
    const uint32_t *ge = s->ge_param;
    const uint32_t *box = &ge[MGRAS_GE_PIXEL_BOX];
    uint32_t hq = s->dma_desc_30000[0];
    /* The guest's words, so the differences are taken in 64 bits. */
    int64_t w = (int64_t)(int32_t)box[2] - (int32_t)box[0] + 1;
    int64_t h = (int64_t)(int32_t)box[3] - (int32_t)box[1] + 1;
    uint32_t pass_rows, pass;
    bool pixel_texture = mgras_gl_pixels_texture_box(s, &pass_rows, &pass);
    uint32_t pairs, format;

    /*
     * A refused draw still owns the image that follows it: its rows as
     * the rectangle gives them, or the next block alone when the
     * rectangle is not one.
     */
    *px = (SGIMgrasGLPixels) {
        .format = MGRAS_GL_PIXELS_FORMATS,
        .bpp = 1,
        .w = MGRAS_GL_PIXELS_MAX,
        .h = 1,
    };
    MgrasGLZoom zoom;

    if (!mgras_gl_pixels_zoom(s, pixel_texture, pass_rows, &zoom)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: pixel rectangle %d,%d to %d,%d "
                      "(size word 0x%x, zoom words 0x%x 0x%x, pass %u) is "
                      "not modelled\n", box[0], box[1], box[2], box[3],
                      box[4], ge[MGRAS_GE_PIXEL_ZOOM],
                      ge[MGRAS_GE_PIXEL_ZOOM + 1], box[8]);
        return false;
    }
    w = zoom.src_w;
    h = zoom.src_h;
    px->h = h;
    if (pixel_texture) {
        px->pass = pass;
        px->row_step = s->board->re_count;
    }
    if (zoom.zoomed) {
        px->zoom_x = float_bits(zoom.zx);
        px->zoom_y = float_bits(zoom.zy);
        px->dest_w = zoom.dest_w;
        px->dest_h = zoom.dest_h;
        px->flip = zoom.flip;
    }
    format = mgras_gl_pixels_float_signature(ge, w, h, row_words,
                                             dma_only);
    if (format == MGRAS_GL_PIXELS_FORMATS) {
        format = mgras_gl_pixels_match(ge, &pairs);
    } else {
        pairs = 0;
    }
    if (pixel_texture && format != MGRAS_GL_PIXELS_ABGR8) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: pixel-texture draw with GE "
                      "program %u is not modelled\n", format);
        return false;
    }
    if (format >= ARRAY_SIZE(mgras_gl_pixel_programs)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GE pixel program of %u "
                      "routines from 0x%x, 0x%x is not modelled; its image "
                      "is not drawn\n", pairs, ge[MGRAS_GE_PIXEL_PROGRAM],
                      ge[MGRAS_GE_PIXEL_PROGRAM + 2]);
        return false;
    }
    px->format = format;
    px->x = box[0];
    px->y = box[1];
    px->w = w;
    px->bpp = 0;
    switch (format) {
    case MGRAS_GL_PIXELS_FLOAT_RGB:
        px->bpp = 12;
        break;
    case MGRAS_GL_PIXELS_FLOAT_RGBA:
        px->bpp = 16;
        break;
    case MGRAS_GL_PIXELS_ABGR8:
        px->bpp = 4;
        if (!(hq & MGRAS_HQ_PIXEL_ABGR)) {
            px->format = MGRAS_GL_PIXELS_RGBA8;
        } else if ((hq & MGRAS_HQ_PIXEL_SIZE) != 1) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: HQ pixel format 0x%x with a "
                          "four-byte GE program is not modelled\n", hq);
            return false;
        }
        break;
    case MGRAS_GL_PIXELS_RGB8:
        px->bpp = 3;
        break;
    default:
        /*
         * An image one or two indices wide gives the same words a row at
         * more than one width; which the client sent cannot be told.
         */
        for (uint32_t bpp = 1; bpp <= 4; bpp *= 2) {
            if (DIV_ROUND_UP(px->w * bpp, 4) == row_words) {
                if (px->bpp) {
                    qemu_log_mask(LOG_UNIMP, "sgi-mgras: colour-index image "
                                  "%u wide in %" PRIu64 " words a row could "
                                  "be %u or %u bytes an index; not drawn\n",
                                  px->w, row_words, px->bpp, bpp);
                    return false;
                }
                px->bpp = bpp;
            }
        }
        px->bpp = px->bpp ? px->bpp : 8;
        break;
    }
    if (px->zoom_x && dma_only && px->bpp && (row_words * 4) % px->bpp == 0 &&
        (row_words * 4) / px->bpp != px->w &&
        mgras_gl_pixels_has_routine(ge, MGRAS_GE_ROUTINE_ZOOM_XF)) {
        /*
         * With a fractional x zoom 0xe38's low half is not the source
         * width (8 for a 4-pixel image at 1.5 in the one run); the row's
         * words are.
         */
        px->w = (row_words * 4) / px->bpp;
        px->zoom_x = float_bits((float)px->dest_w / px->w);
    }
    mgras_gl_pixels_raster_zoom(s, px);
    uint64_t expected_words = DIV_ROUND_UP(px->w * px->bpp, 4);
    /*
     * A DMA draw at x zoom 0.5 or 2 keeps the unzoomed program, and its
     * 0x8d20 counts the whole image's words, 0x40 for 16 by 4 pixels of
     * four bytes, with the image as one DMA line and last word 0x98: the
     * GL contract's x-zoom trace (established as the IRIX 6.5.30 client's
     * words on a model run; bit 0 of the last word is unknown and not
     * read). The discriminating test is gl-pixels-zoomed-dma-whole-image.
     */
    bool dma_whole_image = dma_only &&
        !mgras_gl_pixels_has_routine(ge, MGRAS_GE_ROUTINE_ZOOM_Y) &&
        !mgras_gl_pixels_has_routine(ge, MGRAS_GE_ROUTINE_ZOOM_XF) &&
        px->format == MGRAS_GL_PIXELS_RGBA8 &&
        row_words == expected_words * px->h;

    /*
     * Under the y-zoom and fractional-x writing routines 0x8d20 counts one
     * row's words, not the image's (the zoom probe's runs).
     */
    if (!dma_only &&
        !mgras_gl_pixels_has_routine(ge, MGRAS_GE_ROUTINE_ZOOM_Y) &&
        !mgras_gl_pixels_has_routine(ge, MGRAS_GE_ROUTINE_ZOOM_XF) &&
        (px->format == MGRAS_GL_PIXELS_RGBA8 ||
         px->format == MGRAS_GL_PIXELS_FLOAT_RGBA)) {
        expected_words *= px->h;
    }
    if ((px->bpp > 4 && px->format != MGRAS_GL_PIXELS_FLOAT_RGB &&
         px->format != MGRAS_GL_PIXELS_FLOAT_RGBA) ||
        (expected_words != row_words && !dma_whole_image &&
         (dma_only || px->format != MGRAS_GL_PIXELS_RGBA8 ||
         !mgras_gl_pixels_captured_inline_rgba_count(px->w, px->h,
                                                      row_words)) &&
         !(px->format == MGRAS_GL_PIXELS_FLOAT_RGBA &&
           mgras_gl_pixels_captured_float_rgba(px->w, px->h, row_words,
                                               dma_only, ge[0x1b4])))) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: pixel draw of %" PRIu64 " words "
                      "for %u by %u pixels is not modelled\n", row_words,
                      px->w, px->h);
        return false;
    }
    if (dma_only && (format == MGRAS_GL_PIXELS_FLOAT_RGB ||
                     format == MGRAS_GL_PIXELS_FLOAT_RGBA)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: DMA float pixel input is not "
                      "modelled\n");
        return false;
    }
    if (format == MGRAS_GL_PIXELS_INDEX && !mgras_gl(s)->cur.index_bits) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: colour-index image into an RGBA "
                      "context is not modelled\n");
        return false;
    }
    return true;
}

/*
 * The window pixel an image's column or row offset lands on for one
 * raster-position coordinate. The position comes from the guest's
 * viewport and matrices, and the offset is a guest word, so both are
 * kept within 2^30 of the origin in 64-bit arithmetic, as glBitmap's
 * placement keeps its corner (sgi_gl_bitmap_place()): beyond that an
 * image lies off any plane, and there it stays. A position that is not
 * a number clamps to the lower bound.
 */
static int32_t mgras_gl_pixels_place(int32_t offset, float pos)
{
    double at = ceil(round(pos * 256.0) / 256.0 - 0.5);
    int64_t p = MIN(MAX(at, -MGRAS_GL_PIXELS_FAR), MGRAS_GL_PIXELS_FAR);

    p += offset;
    return MIN(MAX(p, -MGRAS_GL_PIXELS_FAR), MGRAS_GL_PIXELS_FAR);
}

/*
 * 0x8d20's last word: 0x99 for every draw captured whose image came by
 * DMA and 0x18 for every one sent inline. The listing sets bit 7 only on
 * its DMA path (under two flags of the context), so
 * a draw with it set takes no inline rows; one without it takes either.
 */
#define MGRAS_GL_PIXELS_BY_DMA   0x80

static void mgras_gl_pixels_start(SGIMgrasState *s, const uint32_t *payload)
{
    SGIMgrasGLPixels *px = mgras_gl_pixels(s);
    SGIGLContext *c = mgras_gl(s);

    if (px->active) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: pixel draw ended after "
                      "%u of %u rows by the next\n", px->row, px->h);
    }
    /* A texture load's image is the texture path's, never drawn here. */
    if (sgi_mgras_texture_load_start(s, payload)) {
        *px = (SGIMgrasGLPixels) {
            .active = 1,
            .format = MGRAS_GL_PIXELS_FORMATS,
            .bpp = 1,
            .w = MGRAS_GL_PIXELS_MAX,
            .h = 1,
            .dma_only = !!(payload[7] & MGRAS_GL_PIXELS_BY_DMA),
        };
        return;
    }
    if (!mgras_gl_pixels_program(s, (uint64_t)payload[0] + payload[1],
                                 !!(payload[7] & MGRAS_GL_PIXELS_BY_DMA), px)) {
        px->format = MGRAS_GL_PIXELS_FORMATS;
        px->bpp = 1;
    }
    px->active = 1;
    px->dma_only = !!(payload[7] & MGRAS_GL_PIXELS_BY_DMA);
    px->apart = 0;
    if (px->format == MGRAS_GL_PIXELS_FORMATS) {
        return;
    }
    /*
     * The rectangle is the image's own, and the GE places it at the
     * raster position (0xd700 loads it before each draw): mandel's rows
     * all come at 0, 0 to 99, 0 while its glRasterPos2i moves up the
     * window. Pixel n of a row covers window x from xr + n to xr + n + 1,
     * so its fragment is the pixel whose centre lies there (OpenGL 1.2.1,
     * 3.6.4). With the raster position invalid nothing is drawn (3.6.4).
     * The position is first snapped to the 1/256 pixel the model snaps
     * vertices to (mgras_gl_raster_rules): mandel's glRasterPos2i(0, 0)
     * through its projection comes out at 0.500003, a pixel-centre
     * position the client meant exactly, which unsnapped would move the
     * image one pixel. How the GE rounds it is not published.
     */
    if (!c->raster_valid) {
        px->format = MGRAS_GL_PIXELS_FORMATS;
        return;
    }
    px->x = mgras_gl_pixels_place(px->x, c->raster_pos[0]);
    px->y = mgras_gl_pixels_place(px->y, c->raster_pos[1]);
    if (!c->cur.drawable.bound) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: pixel draw with no window bound "
                      "dropped\n");
    }
}

/*
 * One fragment at window pixel (x, y) in a colour or index, drawn as a
 * one-pixel glBitmap so that it meets the window's clip, the depth test
 * and the index mask on the path every bitmap takes. This costs one
 * raster op a pixel: a DMA transfer of the largest draw, 4096 by 4096,
 * makes 16.7 million of them inside the single port write that kicks
 * it. A raster op for a run of pixels in shared code (hw/display/sgi)
 * would take a row at once.
 */
static void mgras_gl_pixels_put(SGIMgrasState *s,
                                const SGIMgrasGLPixels *px, int32_t x,
                                int32_t y, const float rgba[4], float index)
{
    static const uint8_t set = 0x80;
    SGIGLContext *c = mgras_gl(s);
    SGIGLSink sink = mgras_gl_sink(s);
    SGIRasterOp op = {
        .kind = SGI_RASTER_BITMAP,
        .bitmap = {
            .state = &c->cur,
            .x = x,
            .y = y,
            .width = 1,
            .height = 1,
            .count = 1,
            .bytes = &set,
            .z = c->raster_pos[2],
            .index = index,
        },
    };

    memcpy(op.bitmap.rgba, rgba, sizeof(op.bitmap.rgba));
    op.ntargets = sgi_gl_draw_targets(&c->cur, op.targets);
    op.clip = c->cur.drawable.clip;
    sink.raster(sink.opaque, &op);
}

/*
 * The window row of the current image row: in order, or, for one RE's
 * pixel-texture pass, every row_step-th row from the pass's own. Which
 * rows an RE owns is the dual-RE row split; that pass k's row i lands on
 * row k + n * i is inferred from it.
 */
static int32_t mgras_gl_pixels_y(const SGIMgrasGLPixels *px)
{
    return px->y + (int32_t)(px->pass + px->row * MAX(px->row_step, 1u));
}

/* The window pixels source index n covers at zoom z, clipped to [0, limit). */
static void mgras_gl_pixels_span(uint32_t n, float z, uint32_t limit,
                                 uint32_t *lo, uint32_t *hi)
{
    double a = ceil(n * (double)z - 0.5), b = ceil((n + 1) * (double)z - 0.5);

    *lo = MIN(MAX(a, 0), limit);
    *hi = MIN(MAX(b, 0), limit);
}

/*
 * Draw the current source pixel: one window pixel unzoomed, or every
 * pixel of its zoomed footprint inside the box.
 */
static void mgras_gl_pixels_emit(SGIMgrasState *s, const SGIMgrasGLPixels *px,
                                 const float rgba[4], float index)
{
    uint32_t m = px->pass + px->row * MAX(px->row_step, 1u);
    uint32_t x0, x1, y0, y1;

    if (!px->zoom_x) {
        mgras_gl_pixels_put(s, px, px->x + px->col, mgras_gl_pixels_y(px),
                            rgba, index);
        return;
    }
    mgras_gl_pixels_span(px->col, bits_float(px->zoom_x), px->dest_w,
                         &x0, &x1);
    mgras_gl_pixels_span(m, bits_float(px->zoom_y), px->dest_h, &y0, &y1);
    for (uint32_t j = y0; j < y1; j++) {
        int32_t y = px->flip & 2 ? px->y - (int32_t)j : px->y + (int32_t)j;

        for (uint32_t i = x0; i < x1; i++) {
            int32_t x = px->flip & 1 ? px->x - (int32_t)i :
                                       px->x + (int32_t)i;

            mgras_gl_pixels_put(s, px, x, y, rgba, index);
        }
    }
}

static void mgras_gl_pixels_pixel(SGIMgrasState *s, SGIMgrasGLPixels *px,
                                  uint32_t v)
{
    float rgba[4] = { 0, 0, 0, 1.0f };
    uint8_t b[4];

    if (px->format == MGRAS_GL_PIXELS_FORMATS) {
        return;
    }
    stl_be_p(b, v << (8 * (4 - px->bpp)));
    switch (px->format) {
    case MGRAS_GL_PIXELS_ABGR8:
        rgba[0] = b[3] / 255.0f;
        rgba[1] = b[2] / 255.0f;
        rgba[2] = b[1] / 255.0f;
        rgba[3] = b[0] / 255.0f;
        break;
    case MGRAS_GL_PIXELS_RGBA8:
        rgba[3] = b[3] / 255.0f;
        /* fall through */
    case MGRAS_GL_PIXELS_RGB8:
        rgba[0] = b[0] / 255.0f;
        rgba[1] = b[1] / 255.0f;
        rgba[2] = b[2] / 255.0f;
        break;
    }
    float textured[4];

    if (sgi_mgras_texture_pixel(s, rgba, textured)) {
        memcpy(rgba, textured, sizeof(rgba));
    }
    mgras_gl_pixels_emit(s, px, rgba, v);
}

/* Take n image bytes of the current row; bytes past its last pixel drop. */
static bool mgras_gl_pixels_float_component(SGIMgrasState *s,
                                            SGIMgrasGLPixels *px,
                                            uint32_t bits)
{
    union {
        uint32_t bits;
        float value;
    } component = { .bits = bits };
    uint32_t count = px->format == MGRAS_GL_PIXELS_FLOAT_RGB ? 3 : 4;
    float rgba[4] = { 0, 0, 0, 1 };

    if (!isfinite(component.value)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: non-finite float pixel "
                      "component; remaining image is dropped\n");
        px->format = MGRAS_GL_PIXELS_FORMATS;
        px->ncomponent = 0;
        return false;
    }
    px->components[px->ncomponent++] = bits;
    if (px->ncomponent < count) {
        return true;
    }
    for (uint32_t i = 0; i < count; i++) {
        component.bits = px->components[i];
        rgba[i] = component.value;
    }
    float textured[4];

    if (sgi_mgras_texture_pixel(s, rgba, textured)) {
        memcpy(rgba, textured, sizeof(rgba));
    }
    mgras_gl_pixels_emit(s, px, rgba, 0);
    px->ncomponent = 0;
    px->col++;
    return true;
}

static void mgras_gl_pixels_bytes(SGIMgrasState *s, const uint8_t *bytes,
                                  uint32_t n)
{
    SGIMgrasGLPixels *px = mgras_gl_pixels(s);

    for (uint32_t i = 0; i < n && px->col < px->w; i++) {
        px->carry = px->carry << 8 | bytes[i];
        if (px->format == MGRAS_GL_PIXELS_FLOAT_RGB ||
            px->format == MGRAS_GL_PIXELS_FLOAT_RGBA) {
            if (++px->ncarry == sizeof(uint32_t)) {
                uint32_t bits = px->carry;

                px->carry = px->ncarry = 0;
                if (!mgras_gl_pixels_float_component(s, px, bits)) {
                    return;
                }
            }
        } else if (++px->ncarry == px->bpp) {
            mgras_gl_pixels_pixel(s, px, px->carry);
            px->col++;
            px->carry = px->ncarry = 0;
        }
    }
}

static void mgras_gl_pixels_row_end(SGIMgrasState *s)
{
    SGIMgrasGLPixels *px = mgras_gl_pixels(s);

    if (px->col < px->w && px->format != MGRAS_GL_PIXELS_FORMATS) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: pixel row %u ended after "
                      "%u of %u pixels\n", px->row, px->col, px->w);
    }
    px->col = px->carry = px->ncarry = px->ncomponent = px->skip = 0;
    if (++px->row == px->h) {
        memset(px, 0, sizeof(*px));
    }
}

/* Whether an inline block is the next row of a pixel draw. */
static bool mgras_gl_pixels_block(SGIMgrasState *s, uint32_t header)
{
    SGIMgrasGLPixels *px = mgras_gl_pixels(s);
    uint32_t offset = (header & SGI_MGRAS_CMD_BLOCK_OFFSET) >> 22;
    uint32_t bytes = header & SGI_MGRAS_CMD_BLOCK_BYTES;

    /*
     * A row follows its draw's 0x8d20, or the row before it and the zero
     * word the client adds after an odd count, as glBitmap's image
     * follows its packet; a block after any other header is that
     * packet's (sgi_mgras_gl_pixels_header()).
     */
    if (!px->active || px->dma_only || px->apart) {
        return false;
    }
    if (px->format == MGRAS_GL_PIXELS_FORMATS) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-mgras: rejected pixel block context=%u slot=0x%x "
                      "w=%u h=%u row=%u col=%u active=%u dma_only=%u "
                      "apart=%u offset=%u bytes=%u words=%u\n",
                      s->gl_current, s->gl_slot[s->gl_current], px->w, px->h,
                      px->row, px->col, px->active, px->dma_only, px->apart,
                      offset, bytes,
                      (unsigned int)DIV_ROUND_UP(bytes, sizeof(uint32_t)));
    }
    /*
     * The row's length counts the offset's word: the words sent, not the
     * bytes after the offset.
     */
    s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_GL_BLOCK;
    s->cmd_token_reg = MGRAS_GL_PIXELS_OP;
    s->cmd_token_payload_count = MGRAS_GL_PIXELS_WORDS;
    s->cmd_token_words_remaining = DIV_ROUND_UP(bytes, sizeof(uint32_t));
    /* Captured 48x1 and 64x1 RGBA rows start at byte zero despite offset 2. */
    px->skip = px->format == MGRAS_GL_PIXELS_FLOAT_RGBA &&
               px->w == 47 && px->h == 1 && px->row == 0 &&
               offset == 2 && bytes == 47 * 4 * sizeof(uint32_t) ?
               0 : offset;
    if (!s->cmd_token_words_remaining) {
        s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_DISCARD;
        mgras_gl_pixels_row_end(s);
    }
    return true;
}

void sgi_mgras_gl_pixels_header(SGIMgrasState *s, uint32_t header)
{
    if (header && !(header & 0x80000000u)) {
        mgras_gl_pixels(s)->apart = 1;
    }
}

static void mgras_gl_pixels_word(SGIMgrasState *s, uint32_t word)
{
    SGIMgrasGLPixels *px = mgras_gl_pixels(s);
    uint8_t b[4];
    uint32_t n = 0;

    for (int i = 0; i < 4; i++) {
        if (px->skip) {
            px->skip--;
        } else {
            b[n++] = word >> (24 - 8 * i);
        }
    }
    mgras_gl_pixels_bytes(s, b, n);
    if (!s->cmd_token_words_remaining) {
        s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_DISCARD;
        mgras_gl_pixels_row_end(s);
    }
}

/*
 * A transfer's rows are the image's rows, or runs of whole image rows: the
 * client sends an image whose rows lie back to back in host memory as one
 * transfer row, as imgview's 128-pixel tiles of a 128-pixel image came on
 * the SSI, 1 row of 12288 bytes for a draw of 128 by 32 at three bytes.
 */
bool sgi_mgras_gl_pixels_dma(SGIMgrasState *s, uint32_t rows, uint32_t bpr,
                             bool *take)
{
    SGIMgrasGLPixels *px = mgras_gl_pixels(s);
    uint32_t row_bytes = px->w * px->bpp;

    /* A transfer this ends without drawing moves nothing. */
    *take = false;
    if (sgi_mgras_texture_dma(s, rows, bpr, take)) {
        memset(px, 0, sizeof(*px));
        return true;
    }
    if (!px->active) {
        return false;
    }
    if (px->format == MGRAS_GL_PIXELS_FORMATS) {
        memset(px, 0, sizeof(*px));
        return true;
    }
    if (px->zoom_x && !px->row && row_bytes && !(bpr % row_bytes) &&
        (uint64_t)rows * (bpr / row_bytes) >= 1 &&
        (uint64_t)rows * (bpr / row_bytes) <= MGRAS_GL_PIXELS_MAX) {
        /* The client sends every source row; the zoom picks the rows. */
        px->h = rows * (bpr / row_bytes);
    }
    if (px->row || !row_bytes || bpr % row_bytes ||
        (uint64_t)rows * (bpr / row_bytes) != px->h) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: DMA of %u rows of %u "
                      "bytes for a pixel draw of %u by %u at %u bytes a "
                      "pixel; not drawn\n", rows, bpr, px->w, px->h,
                      px->bpp);
        memset(px, 0, sizeof(*px));
        return true;
    }
    *take = true;
    return true;
}

void sgi_mgras_gl_pixels_abort(SGIMgrasState *s)
{
    memset(mgras_gl_pixels(s), 0, sizeof(SGIMgrasGLPixels));
    sgi_mgras_texture_abort(s);
}

void sgi_mgras_gl_pixels_row(SGIMgrasState *s, const uint8_t *bytes,
                             uint32_t n)
{
    if (sgi_mgras_texture_row(s, bytes, n)) {
        return;
    }
    while (n && mgras_gl_pixels(s)->active) {
        SGIMgrasGLPixels *px = mgras_gl_pixels(s);
        uint32_t len = MIN(n, px->w * px->bpp);

        mgras_gl_pixels_bytes(s, bytes, len);
        mgras_gl_pixels_row_end(s);
        bytes += len;
        n -= len;
    }
}

/*
 * The raster register records of the 0x7c08 and 0x7f08 packets: an inline
 * block of triples (register, value, the GE's shadow address) or pairs
 * (register, value), a pad word after an odd count of the former. The
 * block's words are applied here as each record completes
 * (sgi_mgras_raster_record()); a register the raster engine leaves is
 * logged, except under 0x7c08 on a board with TRAM, where the texture path
 * owns the block's words and logs its own. The texture path's log also
 * names the registers applied here as not modelled by it.
 */
static bool mgras_gl_records_block(SGIMgrasState *s, uint32_t header)
{
    uint32_t *slot = s->cmd_token_payload;
    uint32_t offset = (header & SGI_MGRAS_CMD_BLOCK_OFFSET) >> 22;
    uint32_t bytes = header & SGI_MGRAS_CMD_BLOCK_BYTES;
    bool triples = s->cmd_token_reg == SGI_MGRAS_TEX_RECORDS_OP;

    if (s->cmd_token_class != 0 || s->cmd_token_payload_count != 2 ||
        (!triples && s->cmd_token_reg != MGRAS_GL_RSS_PAIRS_OP)) {
        return false;
    }
    if (triples && sgi_mgras_texture_records(s, header)) {
        slot[MGRAS_GL_REC_TEXTURE] = 1;
    } else {
        if (offset || bytes != slot[1] * sizeof(uint32_t)) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: raster register "
                          "records block of %u bytes at offset %u after a "
                          "packet giving %u words\n", bytes, offset,
                          slot[1]);
        }
        s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_GL_BLOCK;
        s->cmd_token_words_remaining = DIV_ROUND_UP(offset + bytes,
                                                    sizeof(uint32_t));
        s->cmd_token_payload_count = SGI_MGRAS_TEX_RECORD_SLOTS;
        slot[MGRAS_GL_REC_TEXTURE] = 0;
        if (!s->cmd_token_words_remaining) {
            s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_DISCARD;
        }
    }
    slot[MGRAS_GL_REC_NEXT] = 0;
    return true;
}

static void mgras_gl_record_word(SGIMgrasState *s, uint32_t word)
{
    uint32_t *slot = s->cmd_token_payload;
    bool triples = s->cmd_token_reg == SGI_MGRAS_TEX_RECORDS_OP;
    uint32_t stride = triples ? 3 : 2;
    uint32_t k = slot[MGRAS_GL_REC_NEXT]++;

    if (k / stride < slot[0]) {
        if (k % stride == 0) {
            slot[MGRAS_GL_REC_REG] = word;
        } else if (k % stride == 1) {
            slot[MGRAS_GL_REC_VALUE] = word;
        }
        if (k % stride == stride - 1 &&
            !sgi_mgras_raster_record(s, slot[MGRAS_GL_REC_REG],
                                     slot[MGRAS_GL_REC_VALUE]) &&
            !mgras_gl_pixel_clip_record(s, slot[MGRAS_GL_REC_REG],
                                        slot[MGRAS_GL_REC_VALUE]) &&
            !slot[MGRAS_GL_REC_TEXTURE] &&
            (triples ||
             !sgi_mgras_texture_kernel_record(s, slot[MGRAS_GL_REC_REG],
                                              slot[MGRAS_GL_REC_VALUE]))) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: raster register record "
                          "0x%x value 0x%x is not modelled\n",
                          slot[MGRAS_GL_REC_REG], slot[MGRAS_GL_REC_VALUE]);
        }
    }
    if (slot[MGRAS_GL_REC_TEXTURE]) {
        sgi_mgras_texture_record_word(s, word);
    } else if (!s->cmd_token_words_remaining) {
        s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_DISCARD;
    }
}

bool sgi_mgras_gl_block(SGIMgrasState *s, uint32_t header)
{
    const uint32_t *slot = s->cmd_token_payload;
    uint32_t offset = (header & SGI_MGRAS_CMD_BLOCK_OFFSET) >> 22;
    uint32_t bytes = header & SGI_MGRAS_CMD_BLOCK_BYTES;

    /* A long GE parameter load: its header had no length. */
    if (s->cmd_token_class == 0 && s->cmd_token_payload_count == 0 &&
        (s->cmd_token_reg == MGRAS_GL_GE_CRAM_OP ||
         s->cmd_token_reg == MGRAS_GL_GE_ERAM_OP) && !offset && bytes) {
        s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_GE_LOAD;
        s->cmd_token_words_remaining = DIV_ROUND_UP(bytes, sizeof(uint32_t));
        memset(s->cmd_token_payload, 0, sizeof(s->cmd_token_payload));
        s->cmd_token_payload[MGRAS_GL_GE_LOAD_TOTAL] =
            s->cmd_token_words_remaining;
        s->cmd_token_payload_count = MGRAS_GL_GE_LOAD_SLOTS;
        return true;
    }
    if (mgras_gl_pixels_block(s, header) ||
        mgras_gl_records_block(s, header)) {
        return true;
    }
    if (s->cmd_token_class != 0 || s->cmd_token_reg != MGRAS_GL_BITMAP_OP ||
        s->cmd_token_payload_count != MGRAS_GL_BITMAP_WORDS) {
        return false;
    }
    if (bytes != slot[7] * sizeof(uint32_t)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: glBitmap image of %u "
                      "bytes after a packet giving %u words\n", bytes,
                      slot[7]);
    }
    s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_GL_BLOCK;
    s->cmd_token_words_remaining = DIV_ROUND_UP(offset + bytes,
                                                sizeof(uint32_t));
    s->cmd_token_payload[MGRAS_GL_BITMAP_SKIP] = offset;
    if (!s->cmd_token_words_remaining) {
        s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_DISCARD;
    }
    return true;
}

void sgi_mgras_gl_block_word(SGIMgrasState *s, uint32_t word)
{
    uint32_t *slot = s->cmd_token_payload;
    uint32_t first = slot[MGRAS_GL_BITMAP_NEXT], n = 0;
    uint8_t bytes[4];

    if (s->cmd_token_reg == MGRAS_GL_PIXELS_OP) {
        mgras_gl_pixels_word(s, word);
        return;
    }
    if (s->cmd_token_reg == SGI_MGRAS_TEX_RECORDS_OP ||
        s->cmd_token_reg == MGRAS_GL_RSS_PAIRS_OP) {
        mgras_gl_record_word(s, word);
        return;
    }
    for (int i = 0; i < 4; i++) {
        if (slot[MGRAS_GL_BITMAP_SKIP]) {
            slot[MGRAS_GL_BITMAP_SKIP]--;
            continue;
        }
        bytes[n++] = word >> (24 - 8 * i);
    }
    slot[MGRAS_GL_BITMAP_NEXT] += n;
    if (slot[MGRAS_GL_BITMAP_PLACED] && n) {
        SGIGLSink sink = mgras_gl_sink(s);
        SGIGLBitmap b = {
            .x = slot[MGRAS_GL_BITMAP_X],
            .y = slot[MGRAS_GL_BITMAP_Y],
            .width = slot[1],
            .height = slot[2],
        };

        sgi_gl_bitmap_bytes(mgras_gl(s), &sink, &b, first, bytes, n);
    }
    if (!s->cmd_token_words_remaining) {
        s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_DISCARD;
    }
}

/*
 * The four words 0x3ba to 0x3bd answer after 0xef08 or 0xee08: the
 * parameter's floats, zero past its count. A light, face or parameter the
 * context does not keep answers zeros, logged.
 */
static void mgras_gl_readback(SGIMgrasState *s, uint32_t header,
                              const uint32_t *payload)
{
    uint32_t *rb = s->gl_context[s->gl_current].readback;
    uint32_t out[SGI_GL_GET_WORDS];
    unsigned n = 0;
    bool known = header == MGRAS_GL_GET_LIGHT ?
                 sgi_gl_get_light(mgras_gl(s), payload[0], payload[1], out,
                                  &n) :
                 sgi_gl_get_material(mgras_gl(s), payload[0], payload[1],
                                     out, &n);

    memset(rb, 0, 4 * sizeof(*rb));
    if (!known) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: %s 0x%x parameter 0x%x "
                      "is not kept; its queries answer zero\n",
                      header == MGRAS_GL_GET_LIGHT ? "light" : "face",
                      payload[0], payload[1]);
        return;
    }
    memcpy(rb, out, MIN(n, 4) * sizeof(*rb));
}

/* Execute a texture coordinate generation packet; false for any other. */
static bool mgras_gl_tex_gen(SGIMgrasState *s, uint32_t header,
                             const uint32_t *payload)
{
    SGIMgrasGLContext *g = &s->gl_context[s->gl_current];
    uint32_t word[6] = { 0 };
    float f;

    switch (header) {
    case MGRAS_GL_TEX_GEN_MODE:
        word[0] = payload[0];
        word[1] = payload[1];
        f = payload[2];
        memcpy(&word[2], &f, sizeof(f));
        mgras_gl_command(s, SGI_GL_OP_TEX_GEN, word, 6);
        return true;
    case MGRAS_GL_TEX_GEN_PLANE:
        g->tex_gen_plane_open = 1;
        g->tex_gen_plane[0] = payload[0];
        g->tex_gen_plane[1] = payload[1];
        return true;
    case MGRAS_GL_TEX_GEN_PLANE_F:
    case MGRAS_GL_TEX_GEN_PLANE_I:
        if (!g->tex_gen_plane_open) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: glTexGen plane 0x%x "
                          "with no coordinate before it ignored\n", header);
            return true;
        }
        g->tex_gen_plane_open = 0;
        word[0] = g->tex_gen_plane[0];
        word[1] = g->tex_gen_plane[1];
        for (int i = 0; i < 4; i++) {
            if (header == MGRAS_GL_TEX_GEN_PLANE_I) {
                f = (int32_t)payload[i];
                memcpy(&word[i + 2], &f, sizeof(f));
            } else {
                word[i + 2] = payload[i];
            }
        }
        mgras_gl_command(s, SGI_GL_OP_TEX_GEN, word, 6);
        return true;
    case MGRAS_GL_ENABLE_CAP:
    case MGRAS_GL_DISABLE_CAP:
        if (payload[0] < SGI_GL_TEXTURE_GEN_S ||
            payload[0] > SGI_GL_TEXTURE_GEN_Q) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: capability 0x%x under "
                          "0x%x is not modelled\n", payload[0], header);
            return true;
        }
        word[0] = payload[0];
        mgras_gl_command(s, header == MGRAS_GL_ENABLE_CAP ?
                         SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, word, 1);
        return true;
    }
    return false;
}

/* Execute a lighting packet; false for any other header. */
static bool mgras_gl_lighting(SGIMgrasState *s, uint32_t header,
                              const uint32_t *payload, uint32_t words)
{
    uint32_t word[6] = { 0 };
    float f;

    for (int i = 0; i < ARRAY_SIZE(mgras_gl_light_headers); i++) {
        if (header == mgras_gl_light_headers[i].header) {
            word[0] = payload[0];
            word[1] = mgras_gl_light_headers[i].pname;
            memcpy(&word[2], &payload[1], (words - 1) * sizeof(uint32_t));
            mgras_gl_command(s, SGI_GL_OP_LIGHT, word, 6);
            return true;
        }
    }
    switch (header) {
    case MGRAS_GL_LIGHT_MODEL_AMBIENT: {
        uint32_t out[SGI_GL_GET_WORDS];
        unsigned n;

        /* Alpha is not sent: keep the context's. */
        sgi_gl_get(mgras_gl(s), SGI_GL_LIGHT_MODEL_AMBIENT, out, &n);
        word[0] = SGI_GL_LIGHT_MODEL_AMBIENT;
        memcpy(&word[1], payload, 3 * sizeof(uint32_t));
        word[4] = out[3];
        mgras_gl_command(s, SGI_GL_OP_LIGHT_MODEL, word, 5);
        return true;
    }
    case MGRAS_GL_LIGHT_MODEL_LOCAL:
    case MGRAS_GL_LIGHT_MODEL_TWO_SIDE:
        word[0] = header == MGRAS_GL_LIGHT_MODEL_LOCAL ?
                  SGI_GL_LIGHT_MODEL_LOCAL_VIEWER :
                  SGI_GL_LIGHT_MODEL_TWO_SIDE;
        f = payload[0] != 0;
        memcpy(&word[1], &f, sizeof(f));
        mgras_gl_command(s, SGI_GL_OP_LIGHT_MODEL, word, 5);
        return true;
    case MGRAS_GL_MATERIAL:
        mgras_gl_command(s, SGI_GL_OP_MATERIAL, payload, 6);
        return true;
    case MGRAS_GL_COLOR_MATERIAL_FRONT:
    case MGRAS_GL_COLOR_MATERIAL_BACK:
    case MGRAS_GL_COLOR_MATERIAL_BOTH:
        word[0] = header == MGRAS_GL_COLOR_MATERIAL_FRONT ? SGI_GL_FRONT :
                  header == MGRAS_GL_COLOR_MATERIAL_BACK ? SGI_GL_BACK :
                  SGI_GL_FRONT_AND_BACK;
        word[1] = payload[0];
        mgras_gl_command(s, SGI_GL_OP_COLOR_MATERIAL, word, 2);
        return true;
    case MGRAS_GL_COLOR_MATERIAL_ON:
    case MGRAS_GL_COLOR_MATERIAL_OFF:
        word[0] = SGI_GL_COLOR_MATERIAL;
        mgras_gl_command(s, header == MGRAS_GL_COLOR_MATERIAL_ON ?
                         SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, word, 1);
        return true;
    case MGRAS_GL_LIGHTING_ON:
    case MGRAS_GL_LIGHTING_OFF:
        word[0] = SGI_GL_LIGHTING;
        mgras_gl_command(s, header == MGRAS_GL_LIGHTING_ON ?
                         SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, word, 1);
        return true;
    case MGRAS_GL_LIGHT_ON:
    case MGRAS_GL_LIGHT_OFF:
        if (payload[0] >= SGI_GL_LIGHTS) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: light %u enable "
                          "ignored\n", payload[0]);
            return true;
        }
        word[0] = SGI_GL_LIGHT0 + payload[0];
        mgras_gl_command(s, header == MGRAS_GL_LIGHT_ON ?
                         SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, word, 1);
        return true;
    case MGRAS_GL_NORMALIZE:
        word[0] = SGI_GL_NORMALIZE;
        mgras_gl_command(s, payload[0] ? SGI_GL_OP_ENABLE :
                         SGI_GL_OP_DISABLE, word, 1);
        return true;
    case MGRAS_GL_GET_LIGHT:
    case MGRAS_GL_GET_MATERIAL:
        mgras_gl_readback(s, header, payload);
        return true;
    default:
        return false;
    }
}

/* A glFog packet as glFogfv: the index and mode arrive as integers. */
static void mgras_gl_fog(SGIMgrasState *s, uint32_t header,
                         const uint32_t *payload)
{
    uint32_t word[5] = { 0 };
    float f;

    switch (header) {
    case MGRAS_GL_FOG_COLOR:
        word[0] = SGI_GL_FOG_COLOR;
        memcpy(&word[1], payload, 4 * sizeof(uint32_t));
        break;
    case MGRAS_GL_FOG_START:
    case MGRAS_GL_FOG_END:
    case MGRAS_GL_FOG_DENSITY:
        word[0] = header == MGRAS_GL_FOG_START ? SGI_GL_FOG_START :
                  header == MGRAS_GL_FOG_END ? SGI_GL_FOG_END :
                  SGI_GL_FOG_DENSITY;
        word[1] = payload[0];
        break;
    default:
        word[0] = header == MGRAS_GL_FOG_INDEX ? SGI_GL_FOG_INDEX :
                  SGI_GL_FOG_MODE;
        f = (int32_t)payload[0];
        memcpy(&word[1], &f, sizeof(f));
        break;
    }
    mgras_gl_command(s, SGI_GL_OP_FOG, word, 5);
}

/*
 * A glNormal3 packet's components as the floats glNormal3f takes, by
 * table 2.6's conversion for a signed component of the packet's type.
 */
static void mgras_gl_normal(SGIMgrasState *s, uint32_t header,
                            const uint32_t *payload)
{
    uint32_t word[3];
    float f[3];

    for (int i = 0; i < 3; i++) {
        switch (header) {
        case MGRAS_GL_NORMAL3D: {
            uint64_t bits = deposit64(payload[2 * i + 1], 32, 32,
                                      payload[2 * i]);
            double d;

            memcpy(&d, &bits, sizeof(d));
            f[i] = d;
            break;
        }
        case MGRAS_GL_NORMAL3I:
            f[i] = (2.0 * (int32_t)payload[i] + 1) / 4294967295.0;
            break;
        case MGRAS_GL_NORMAL3S:
            f[i] = (2.0 * (int16_t)(payload[i / 2] >>
                                    (i == 1 ? 0 : 16)) + 1) / 65535.0;
            break;
        default:
            f[i] = (2.0 * (int8_t)(payload[0] >> (24 - 8 * i)) + 1) / 255.0;
            break;
        }
    }
    memcpy(word, f, sizeof(word));
    mgras_gl_command(s, SGI_GL_OP_NORMAL3F, word, 3);
}

static void mgras_gl_material4(SGIMgrasState *s, uint32_t face,
                               uint32_t pname, const float v[4])
{
    uint32_t word[6] = { face, pname };

    memcpy(&word[2], v, 4 * sizeof(float));
    mgras_gl_command(s, SGI_GL_OP_MATERIAL, word, 6);
}

/*
 * A vertex, under IRIS GL's colour and normal rule when it applies: see
 * MGRAS_GL_IRIS_RULE_ON.
 */
static void mgras_gl_vertex(SGIMgrasState *s, SGIGLOp op,
                            const uint32_t *args, unsigned nargs)
{
    static const float none[4] = { 0, 0, 0, 1 };
    static const uint32_t faces[2] = { SGI_GL_FRONT, SGI_GL_BACK };
    const SGIMgrasGLContext *g = &s->gl_context[s->gl_current];
    SGIGLContext *c = mgras_gl(s);
    SGIGLMaterial saved[2];
    float colour[4];

    if (!g->iris_rule || !g->iris_colour_last || !c->lighting ||
        c->cur.index_bits) {
        mgras_gl_command(s, op, args, nargs);
        return;
    }
    memcpy(saved, c->material, sizeof(saved));
    memcpy(colour, c->cur_color, sizeof(colour));
    mgras_gl_material4(s, SGI_GL_FRONT_AND_BACK, SGI_GL_AMBIENT, none);
    mgras_gl_material4(s, SGI_GL_FRONT_AND_BACK, SGI_GL_DIFFUSE, none);
    mgras_gl_material4(s, SGI_GL_FRONT_AND_BACK, SGI_GL_SPECULAR, none);
    mgras_gl_material4(s, SGI_GL_FRONT_AND_BACK, SGI_GL_EMISSION, colour);
    mgras_gl_command(s, op, args, nargs);
    for (int i = 0; i < 2; i++) {
        mgras_gl_material4(s, faces[i], SGI_GL_AMBIENT, saved[i].ambient);
        mgras_gl_material4(s, faces[i], SGI_GL_DIFFUSE, saved[i].diffuse);
        mgras_gl_material4(s, faces[i], SGI_GL_SPECULAR, saved[i].specular);
        mgras_gl_material4(s, faces[i], SGI_GL_EMISSION, saved[i].emission);
    }
}

void sgi_mgras_gl_execute(SGIMgrasState *s, uint32_t header,
                          const uint32_t *payload, uint32_t words)
{
    uint32_t word[4];

    if (words != (header & 0xff) / sizeof(uint32_t)) {
        return;
    }
    if (mgras_gl_begin_token(header, false)) {
        word[0] = mgras_gl_mode_of_begin(header);
        mgras_gl_command(s, SGI_GL_OP_BEGIN, word, 1);
        return;
    }
    if (mgras_gl_begin_token(header, true)) {
        /* Any end token ends whichever primitive is open. */
        mgras_gl_command(s, SGI_GL_OP_END, payload, 0);
        return;
    }
    if (header == MGRAS_GL_POLYGON_STIPPLE_UPLOAD) {
        /*
         * Scoped here: the build zeroes every automatic array where it is
         * declared, and at the top of this function that cost every
         * packet 128 bytes of stores.
         */
        uint32_t rows[32];

        /* The client reverses each row's bits, but keeps the row order. */
        for (int i = 0; i < ARRAY_SIZE(rows); i++) {
            rows[i] = mgras_gl_reverse32(payload[i]);
        }
        sgi_gl_polygon_stipple(mgras_gl(s), rows);
        return;
    }
    if (mgras_gl_lighting(s, header, payload, words) ||
        mgras_gl_tex_gen(s, header, payload) ||
        sgi_mgras_texture_execute(s, header, payload, words)) {
        return;
    }
    switch (header) {
    case MGRAS_GL_CONTEXT_OPENER:
        mgras_gl_context_start(s);
        return;
    case MGRAS_GL_RENDER_SELECT:
    case MGRAS_GL_RENDER:
        mgras_gl_selection_mode(s, header, payload);
        return;
    case MGRAS_GL_INIT_NAMES:
    case MGRAS_GL_LOAD_NAME:
    case MGRAS_GL_PUSH_NAME:
    case MGRAS_GL_POP_NAME:
        mgras_gl_selection_name(s, header, payload);
        return;
    case MGRAS_GL_RE_COUNT:
        s->gl_re_count_word = payload[0];
        s->gl_re_count_tokens++;
        if (payload[0] != s->board->re_count >> 1) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: client sent RE count "
                          "token %u on a board with %u RE(s)\n", payload[0],
                          s->board->re_count);
        }
        return;
    case MGRAS_GL_VIEWPORT:
        /* The client has already clamped the size (the viewport routine). */
        mgras_gl_command(s, SGI_GL_OP_VIEWPORT, payload, 4);
        return;
    case MGRAS_GL_CLEAR_COLOR:
        mgras_gl_command(s, SGI_GL_OP_CLEAR_COLOR, payload, 4);
        return;
    case MGRAS_GL_CLEAR_COLOR_BUF:
        word[0] = SGI_GL_COLOR_BUFFER_BIT;
        mgras_gl_command(s, SGI_GL_OP_CLEAR, word, 1);
        return;
    case MGRAS_GL_CLEAR_DEPTH:
        /*
         * The word is already scaled to the depth plane's 24 bits, the form
         * the shared context keeps.
         */
        mgras_gl_command(s, SGI_GL_OP_CLEAR_DEPTH, payload, 1);
        return;
    case MGRAS_GL_CLEAR_DEPTH_BUF:
        word[0] = SGI_GL_DEPTH_BUFFER_BIT;
        mgras_gl_command(s, SGI_GL_OP_CLEAR, word, 1);
        return;
    case MGRAS_GL_SHADE_MODEL:
        mgras_gl_command(s, SGI_GL_OP_SHADE_MODEL, payload, 1);
        return;
    case MGRAS_GL_MATRIX_MODE:
        mgras_gl_command(s, SGI_GL_OP_MATRIX_MODE, payload, 1);
        return;
    case MGRAS_GL_LOAD_MATRIX:
        mgras_gl_command(s, SGI_GL_OP_LOAD_MATRIX, payload, 16);
        return;
    case MGRAS_GL_LOAD_IDENTITY:
        mgras_gl_command(s, SGI_GL_OP_LOAD_IDENTITY, payload, 0);
        return;
    case MGRAS_GL_MULT_MATRIX:
        mgras_gl_command(s, SGI_GL_OP_MULT_MATRIX, payload, 16);
        return;
    case MGRAS_GL_FRUSTUM:
        mgras_gl_command(s, SGI_GL_OP_FRUSTUM, payload, 6);
        return;
    case MGRAS_GL_ORTHO:
        mgras_gl_command(s, SGI_GL_OP_ORTHO, payload, 6);
        return;
    case MGRAS_GL_PUSH_MATRIX:
        mgras_gl_command(s, SGI_GL_OP_PUSH_MATRIX, payload, 0);
        return;
    case MGRAS_GL_POP_MATRIX:
        mgras_gl_command(s, SGI_GL_OP_POP_MATRIX, payload, 0);
        return;
    case MGRAS_GL_TRANSLATE:
        mgras_gl_command(s, SGI_GL_OP_TRANSLATE, payload, 3);
        return;
    case MGRAS_GL_SCALE:
        mgras_gl_command(s, SGI_GL_OP_SCALE, payload, 3);
        return;
    case MGRAS_GL_ROTATE:
        mgras_gl_command(s, SGI_GL_OP_ROTATE, payload, 4);
        return;
    case MGRAS_GL_DEPTH_TEST:
        /*
         * glEnable(GL_DEPTH_TEST) sent 0x6604 with 1 and no other word in
         * the probe's depth case, and every context setup sends it with 0
         * (a captured run).
         */
        word[0] = SGI_GL_DEPTH_TEST;
        mgras_gl_command(s, payload[0] ? SGI_GL_OP_ENABLE :
                         SGI_GL_OP_DISABLE, word, 1);
        return;
    case MGRAS_GL_DEPTH_FUNC:
        word[0] = SGI_GL_NEVER_ENUM + (payload[0] & 7);
        mgras_gl_command(s, SGI_GL_OP_DEPTH_FUNC, word, 1);
        return;
    case MGRAS_GL_DEPTH_MASK:
        mgras_gl_command(s, SGI_GL_OP_DEPTH_MASK, payload, 1);
        return;
    case MGRAS_GL_TEXTURE_TABLE:
        if (payload[0] != 0xc52 || payload[1] != 0xc78 ||
            payload[2] != 0x194 || !payload[3] ||
            (payload[3] > 0x1d && payload[3] != 0x1f)) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: texture table write 0x%x "
                          "0x%x 0x%x 0x%x is not the context setup's "
                          "default; texturing is not modelled\n",
                          payload[0], payload[1], payload[2], payload[3]);
        }
        return;
    case MGRAS_GL_DITHER:
        word[0] = SGI_GL_DITHER;
        mgras_gl_command(s, payload[0] ? SGI_GL_OP_ENABLE :
                         SGI_GL_OP_DISABLE, word, 1);
        return;
    case MGRAS_GL_CULL_ON:
    case MGRAS_GL_CULL_OFF:
        word[0] = SGI_GL_CULL_FACE;
        mgras_gl_command(s, header == MGRAS_GL_CULL_ON ? SGI_GL_OP_ENABLE :
                         SGI_GL_OP_DISABLE, word, 1);
        return;
    case MGRAS_GL_CULL_FACE:
        mgras_gl_command(s, SGI_GL_OP_CULL_FACE, payload, 1);
        return;
    case MGRAS_GL_FRONT_FACE:
        mgras_gl_command(s, SGI_GL_OP_FRONT_FACE, payload, 1);
        return;
    case MGRAS_GL_SCISSOR:
        word[0] = payload[0];
        word[1] = payload[2];
        word[2] = payload[1] + 1;
        word[3] = payload[3] + 1;
        mgras_gl_command(s, SGI_GL_OP_SCISSOR, word, 4);
        return;
    case MGRAS_GL_SCISSOR_TEST:
        word[0] = SGI_GL_SCISSOR_TEST;
        mgras_gl_command(s, payload[0] ? SGI_GL_OP_ENABLE :
                         SGI_GL_OP_DISABLE, word, 1);
        return;
    case MGRAS_GL_ALPHA_FUNC: {
        float ref = (int16_t)payload[1] / (float)MGRAS_GL_ALPHA_REF_MAX;

        word[0] = SGI_GL_NEVER_ENUM + payload[0];
        memcpy(&word[1], &ref, sizeof(ref));
        mgras_gl_command(s, SGI_GL_OP_ALPHA_FUNC, word, 2);
        return;
    }
    case MGRAS_GL_ALPHA_TEST:
        word[0] = SGI_GL_ALPHA_TEST;
        mgras_gl_command(s, payload[0] ? SGI_GL_OP_ENABLE :
                         SGI_GL_OP_DISABLE, word, 1);
        return;
    case MGRAS_GL_STENCIL_FUNC:
        word[0] = SGI_GL_NEVER_ENUM + payload[0];
        word[1] = payload[1];
        word[2] = payload[2];
        mgras_gl_command(s, SGI_GL_OP_STENCIL_FUNC, word, 3);
        return;
    case MGRAS_GL_STENCIL_OP:
        for (int i = 0; i < 3; i++) {
            if (payload[i] >= ARRAY_SIZE(mgras_gl_stencil_ops)) {
                qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: stencil operations "
                              "%u, %u, %u ignored\n", payload[0], payload[1],
                              payload[2]);
                return;
            }
            word[i] = mgras_gl_stencil_ops[payload[i]];
        }
        mgras_gl_command(s, SGI_GL_OP_STENCIL_OP, word, 3);
        return;
    case MGRAS_GL_STENCIL_MASK:
        mgras_gl_command(s, SGI_GL_OP_STENCIL_MASK, payload, 1);
        return;
    case MGRAS_GL_CLEAR_STENCIL:
        mgras_gl_command(s, SGI_GL_OP_CLEAR_STENCIL, payload, 1);
        return;
    case MGRAS_GL_CLEAR_STENCIL_BUF:
        word[0] = SGI_GL_STENCIL_BUFFER_BIT;
        mgras_gl_command(s, SGI_GL_OP_CLEAR, word, 1);
        return;
    case MGRAS_GL_STENCIL_TEST:
        word[0] = SGI_GL_STENCIL_TEST;
        mgras_gl_command(s, payload[0] ? SGI_GL_OP_ENABLE :
                         SGI_GL_OP_DISABLE, word, 1);
        return;
    case MGRAS_GL_BLEND_FUNC:
        mgras_gl_command(s, SGI_GL_OP_BLEND_FUNC, payload, 2);
        return;
    case MGRAS_GL_BLEND_EQUATION:
        mgras_gl_command(s, SGI_GL_OP_BLEND_EQUATION, payload, 1);
        return;
    case MGRAS_GL_BLEND_COLOR:
        mgras_gl_command(s, SGI_GL_OP_BLEND_COLOR, payload, 4);
        return;
    case MGRAS_GL_BLEND:
        word[0] = SGI_GL_BLEND;
        mgras_gl_command(s, payload[0] ? SGI_GL_OP_ENABLE :
                         SGI_GL_OP_DISABLE, word, 1);
        return;
    case MGRAS_GL_LOGIC_OP:
        word[0] = SGI_GL_CLEAR + payload[0];
        mgras_gl_command(s, SGI_GL_OP_LOGIC_OP, word, 1);
        return;
    case MGRAS_GL_LOGIC_OP_ENABLE:
        word[0] = mgras_gl(s)->cur.index_bits ? SGI_GL_INDEX_LOGIC_OP :
                                                SGI_GL_COLOR_LOGIC_OP;
        mgras_gl_command(s, payload[0] ? SGI_GL_OP_ENABLE :
                         SGI_GL_OP_DISABLE, word, 1);
        return;
    case MGRAS_GL_COLOR_MASK:
        for (int i = 0; i < 4; i++) {
            word[i] = extract32(payload[0], i, 1);
        }
        mgras_gl_command(s, SGI_GL_OP_COLOR_MASK, word, 4);
        return;
    case MGRAS_GL_FOG_COLOR:
    case MGRAS_GL_FOG_START:
    case MGRAS_GL_FOG_END:
    case MGRAS_GL_FOG_DENSITY:
    case MGRAS_GL_FOG_INDEX:
    case MGRAS_GL_FOG_MODE:
        mgras_gl_fog(s, header, payload);
        return;
    case MGRAS_GL_FOG:
        word[0] = SGI_GL_FOG;
        mgras_gl_command(s, payload[0] ? SGI_GL_OP_ENABLE :
                         SGI_GL_OP_DISABLE, word, 1);
        return;
    case MGRAS_GL_POLYGON_OFFSET:
        if (payload[2] != MGRAS_GL_OFFSET_EXT &&
            payload[2] != MGRAS_GL_OFFSET_CORE) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: polygon offset form 0x%x "
                          "is not the client's; taken as glPolygonOffset's\n",
                          payload[2]);
        }
        mgras_gl_command(s, payload[2] == MGRAS_GL_OFFSET_EXT ?
                         SGI_GL_OP_POLYGON_OFFSET_EXT :
                         SGI_GL_OP_POLYGON_OFFSET, payload, 2);
        return;
    case MGRAS_GL_CLIP_PLANE: {
        uint32_t plane[5] = { payload[0] - SGI_GL_CLIP_PLANE0, payload[1],
                              payload[2], payload[3], payload[4] };

        mgras_gl_command(s, SGI_GL_OP_CLIP_PLANE, plane, 5);
        return;
    }
    case MGRAS_GL_MAP_GRID1:
    case MGRAS_GL_MAP_GRID2:
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: glMapGrid%d is not modelled; "
                      "evaluator grids draw nothing\n",
                      header == MGRAS_GL_MAP_GRID1 ? 1 : 2);
        return;
    case MGRAS_GL_CLEAR_ACCUM:
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: glClearAccum is not modelled; "
                      "the client accumulates on the host\n");
        return;
    case MGRAS_GL_LINE_WIDTH:
        mgras_gl_command(s, SGI_GL_OP_LINE_WIDTH, payload, 1);
        return;
    case MGRAS_GL_POINT_SIZE:
        mgras_gl_command(s, SGI_GL_OP_POINT_SIZE, payload, 1);
        return;
    case MGRAS_GL_LINE_STIPPLE:
        word[0] = payload[0] + 1;
        word[1] = mgras_gl_reverse16(payload[1]);
        mgras_gl_command(s, SGI_GL_OP_LINE_STIPPLE, word, 2);
        return;
    case MGRAS_GL_POLYGON_MODE:
        mgras_gl_command(s, SGI_GL_OP_POLYGON_MODE, payload, 2);
        return;
    case MGRAS_GL_LINE_SMOOTH:
    case MGRAS_GL_LINE_STIPPLE_ON:
    case MGRAS_GL_POINT_SMOOTH:
    case MGRAS_GL_POLYGON_STIPPLE:
        for (int i = 0; i < ARRAY_SIZE(mgras_gl_raster_enables); i++) {
            if (header == mgras_gl_raster_enables[i].header) {
                word[0] = mgras_gl_raster_enables[i].cap;
            }
        }
        mgras_gl_command(s, payload[0] ? SGI_GL_OP_ENABLE :
                         SGI_GL_OP_DISABLE, word, 1);
        return;
    case MGRAS_GL_CLIP_PLANE_ON:
    case MGRAS_GL_CLIP_PLANE_OFF:
        if (payload[0] - SGI_GL_CLIP_PLANE0 >= SGI_GL_CLIP_PLANES) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: clip plane 0x%x "
                          "enable ignored\n", payload[0]);
            return;
        }
        mgras_gl_command(s, header == MGRAS_GL_CLIP_PLANE_ON ?
                         SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE, payload, 1);
        return;
    case MGRAS_GL_OFFSET_ENABLES: {
        static const uint32_t caps[3] = {
            SGI_GL_POLYGON_OFFSET_FILL, SGI_GL_POLYGON_OFFSET_LINE,
            SGI_GL_POLYGON_OFFSET_POINT,
        };

        for (int i = 0; i < 3; i++) {
            mgras_gl_command(s, extract32(payload[0], i, 1) ?
                             SGI_GL_OP_ENABLE : SGI_GL_OP_DISABLE,
                             &caps[i], 1);
        }
        return;
    }
    case MGRAS_GL_DEPTH_RANGE:
        /* The client does not clamp, so the board does (2.10.1). */
        mgras_gl_command(s, SGI_GL_OP_DEPTH_RANGE, payload, 2);
        return;
    case MGRAS_GL_DRAW_BUFFER:
        mgras_gl_command(s, SGI_GL_OP_DRAW_BUFFERS, payload, 1);
        return;
    case MGRAS_GL_BANKS:
        if (payload[0] != payload[1]) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: bank token words %u and "
                          "%u differ; the model uses the first\n",
                          payload[0], payload[1]);
        }
        s->gl_banks[0] = payload[0];
        s->gl_banks[1] = payload[1];
        mgras_gl_bind(s);
        return;
    case MGRAS_GL_ERAM_READ:
        mgras_gl_eram_read(s, payload[0], payload[1]);
        return;
    case MGRAS_GL_ERAM_COUNT:
        s->eram_state = MGRAS_ERAM_WRITE_ARMED;
        return;
    case MGRAS_GL_ERAM_WRITE:
        mgras_gl_eram_write(s, payload[0], payload[1]);
        return;
    case MGRAS_GL_ERAM_END:
        s->eram_next_addr = 0;
        return;
    case MGRAS_GL_IRIS_LMC_COLOR:
        return;
    case MGRAS_GL_IRIS_LMC_OTHER:
    case MGRAS_GL_IRIS_RULE_OFF:
    case MGRAS_GL_IRIS_RULE_ON:
        s->gl_context[s->gl_current].iris_rule =
            header == MGRAS_GL_IRIS_RULE_ON;
        return;
    case MGRAS_GL_COLOR3F:
        s->gl_context[s->gl_current].iris_colour_last = 1;
        mgras_gl_command(s, SGI_GL_OP_COLOR3F, payload, 3);
        return;
    case MGRAS_GL_COLOR4F:
        s->gl_context[s->gl_current].iris_colour_last = 1;
        mgras_gl_command(s, SGI_GL_OP_COLOR4F, payload, 4);
        return;
    case MGRAS_GL_COLOR3UB:
        s->gl_context[s->gl_current].iris_colour_last = 1;
        mgras_gl_color_bytes(word, payload[0]);
        mgras_gl_command(s, SGI_GL_OP_COLOR3UB, word, 3);
        return;
    case MGRAS_GL_COLOR4UB:
        s->gl_context[s->gl_current].iris_colour_last = 1;
        mgras_gl_color_bytes(word, payload[0]);
        mgras_gl_command(s, SGI_GL_OP_COLOR4UB, word, 4);
        return;
    case MGRAS_GL_NORMAL3F:
        s->gl_context[s->gl_current].iris_colour_last = 0;
        mgras_gl_command(s, SGI_GL_OP_NORMAL3F, payload, 3);
        return;
    case MGRAS_GL_NORMAL3D:
    case MGRAS_GL_NORMAL3I:
    case MGRAS_GL_NORMAL3S:
    case MGRAS_GL_NORMAL3B:
        s->gl_context[s->gl_current].iris_colour_last = 0;
        mgras_gl_normal(s, header, payload);
        return;
    case MGRAS_GL_EDGE_FLAG_TRUE:
    case MGRAS_GL_EDGE_FLAG_FALSE:
        /* Retained for attribute queries and polygon boundary modes. */
        word[0] = header == MGRAS_GL_EDGE_FLAG_TRUE;
        mgras_gl_command(s, SGI_GL_OP_EDGE_FLAG, word, 1);
        return;
    case MGRAS_GL_VERTEX2F:
        mgras_gl_vertex(s, SGI_GL_OP_VERTEX2F, payload, 2);
        return;
    case MGRAS_GL_VERTEX3F:
        mgras_gl_vertex(s, SGI_GL_OP_VERTEX3F, payload, 3);
        return;
    case MGRAS_GL_VERTEX4F:
        mgras_gl_vertex(s, SGI_GL_OP_VERTEX4F, payload, 4);
        return;
    case MGRAS_GL_VERTEX2I:
        mgras_gl_vertex(s, SGI_GL_OP_VERTEX2I, payload, 2);
        return;
    case MGRAS_GL_VERTEX2D:
    case MGRAS_GL_VERTEX3D:
    case MGRAS_GL_VERTEX4D: {
        static const uint32_t ops[] = {
            SGI_GL_OP_VERTEX2F, SGI_GL_OP_VERTEX3F, SGI_GL_OP_VERTEX4F,
        };
        uint32_t n = header == MGRAS_GL_VERTEX2D ? 2 :
                     header == MGRAS_GL_VERTEX3D ? 3 : 4;
        float f[4];

        /* Each double comes high word first, as glNormal3d's do. */
        for (uint32_t i = 0; i < n; i++) {
            uint64_t bits = deposit64(payload[2 * i + 1], 32, 32,
                                      payload[2 * i]);
            double d;

            memcpy(&d, &bits, sizeof(d));
            f[i] = d;
        }
        memcpy(word, f, n * sizeof(f[0]));
        mgras_gl_vertex(s, ops[n - 2], word, n);
        return;
    }
    case MGRAS_GL_VERTEX3I:
    case MGRAS_GL_RASTER_POS3I: {
        float f[3] = { (int32_t)payload[0], (int32_t)payload[1],
                       (int32_t)payload[2] };

        memcpy(word, f, sizeof(f));
        if (header == MGRAS_GL_VERTEX3I) {
            mgras_gl_vertex(s, SGI_GL_OP_VERTEX3F, word, 3);
        } else {
            mgras_gl_command(s, SGI_GL_OP_RASTER_POS3F, word, 3);
        }
        return;
    }
    case MGRAS_GL_TEX_COORD3F:
        mgras_gl_command(s, SGI_GL_OP_TEXCOORD3F, payload, 3);
        return;
    case MGRAS_GL_TEX_COORD3I: {
        float f[3] = { (int32_t)payload[0], (int32_t)payload[1],
                       (int32_t)payload[2] };

        memcpy(word, f, sizeof(f));
        mgras_gl_command(s, SGI_GL_OP_TEXCOORD3F, word, 3);
        return;
    }
    case MGRAS_GL_TEX_COORD2F:
        mgras_gl_command(s, SGI_GL_OP_TEXCOORD2F, payload, 2);
        return;
    case MGRAS_GL_TEX_COORD4F:
        mgras_gl_command(s, SGI_GL_OP_TEXCOORD4F, payload, 4);
        return;
    case MGRAS_GL_RASTER_POS2F:
        mgras_gl_command(s, SGI_GL_OP_RASTER_POS2F, payload, 2);
        return;
    case MGRAS_GL_RASTER_POS3F:
        mgras_gl_command(s, SGI_GL_OP_RASTER_POS3F, payload, 3);
        return;
    case MGRAS_GL_SWAP_TMESH:
        mgras_gl_command(s, SGI_GL_OP_SWAP_TMESH, payload, 0);
        return;
    case MGRAS_GL_FLUSH:
        /* Approximated: every command has already run to completion. */
        mgras_gl_command(s, SGI_GL_OP_FLUSH, payload, 0);
        return;
    case MGRAS_GL_INIT_CI:
        if (!payload[0]) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: colour-index context "
                          "of 0 bits ignored\n");
            return;
        }
        if (payload[0] > MGRAS_GL_INDEX_BITS) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: colour-index context of "
                          "%u bits is stored as its low %u bits\n",
                          payload[0], MGRAS_GL_INDEX_BITS);
        }
        sgi_gl_set_index_mode(mgras_gl(s), payload[0]);
        return;
    case MGRAS_GL_INIT_RGB:
        sgi_gl_set_index_mode(mgras_gl(s), 0);
        return;
    case MGRAS_GL_INDEXS:
        mgras_gl_index(s, (int16_t)(payload[0] >> 16));
        return;
    case MGRAS_GL_INDEXI:
        mgras_gl_index(s, (int32_t)payload[0]);
        return;
    case MGRAS_GL_INDEXF:
        mgras_gl_command(s, SGI_GL_OP_INDEXF, payload, 1);
        return;
    case MGRAS_GL_INDEX_MASK:
        mgras_gl_command(s, SGI_GL_OP_INDEX_MASK, payload, 1);
        return;
    case MGRAS_GL_CLEAR_INDEX:
        mgras_gl_command(s, SGI_GL_OP_CLEAR_INDEX, payload, 1);
        return;
    case MGRAS_GL_BITMAP:
        mgras_gl_bitmap(s, payload);
        return;
    case MGRAS_GL_MOVE_RASTER: {
        SGIGLBitmap b;
        float f[2];

        memcpy(f, payload, sizeof(f));
        sgi_gl_bitmap_place(mgras_gl(s), 0, 0, 0, 0, f[0], f[1], &b);
        return;
    }
    case MGRAS_GL_RASTER_POS2I: {
        float f[2] = { (int32_t)payload[0], (int32_t)payload[1] };

        memcpy(word, f, sizeof(f));
        mgras_gl_command(s, SGI_GL_OP_RASTER_POS2F, word, 2);
        return;
    }
    case MGRAS_GL_PIXELS:
        mgras_gl_pixels_start(s, payload);
        return;
    case MGRAS_GL_SAVE_RSS:
    case MGRAS_GL_RESTORE_RSS:
    case MGRAS_GL_LOAD_RASTER_INFO:
    case MGRAS_GL_SAVE_TEXMODE:
    case MGRAS_GL_RESTORE_TEXMODE:
    case MGRAS_GL_READ_SETUP:
        return;
    case MGRAS_GL_READ_BUFFER:
        if (payload[0] == 0 && payload[2] == 0x404 &&
            payload[1] <= 1) {
            s->gl_context[s->gl_current].client_read_bank =
                MGRAS_CLIENT_READ_FRONT;
        } else if (payload[0] == 1 && payload[1] == 0 &&
                   payload[2] == 0x405) {
            s->gl_context[s->gl_current].client_read_bank =
                MGRAS_CLIENT_READ_BACK;
        } else if (sgi_mgras_deep_reads(s) &&
                   payload[0] == 0 && payload[1] == 2 &&
                   payload[2] == 0x404) {
            s->gl_context[s->gl_current].client_read_bank =
                MGRAS_CLIENT_READ_DEEP_FRONT;
        } else if (sgi_mgras_deep_reads(s) &&
                   payload[0] == 2 && payload[1] == 0 &&
                   payload[2] == 0x405) {
            s->gl_context[s->gl_current].client_read_bank =
                MGRAS_CLIENT_READ_DEEP_BACK;
        } else {
            s->gl_context[s->gl_current].client_read_bank =
                MGRAS_CLIENT_READ_UNSUPPORTED;
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: unsupported client read "
                          "buffer tuple %u, %u, 0x%x\n", payload[0],
                          payload[1], payload[2]);
        }
        return;
    case MGRAS_GL_READ_OPEN:
        s->gl_context[s->gl_current].client_read_bracket = 1;
        return;
    case MGRAS_GL_READ_CLOSE:
        s->gl_context[s->gl_current].client_read_bracket = 0;
        return;
    case MGRAS_GL_RSS_PAIRS:
        /* Its records follow in an inline block; see mgras_gl_records(). */
        if (words != 2 || payload[1] != 2 * payload[0]) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: raster register "
                          "pairs packet of %u records in %u words\n",
                          payload[0], payload[1]);
        }
        return;
    case MGRAS_GL_READ_PIO:
        sgi_mgras_raster_pio_read_trigger(s);
        return;
    case MGRAS_GL_READ_DMA_DONE:
        if (sgi_mgras_texture_tram_read_done(s)) {
            return;
        }
        if (sgi_mgras_gl_client_read_done(s)) {
            return;
        }
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: the OpenGL client's DMA read "
                      "(0xda00 after its kick) is not modelled; nothing "
                      "returned\n");
        return;
    default:
        g_assert_not_reached();
    }
}
