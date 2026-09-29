/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SI MGRAS OpenGL command execution.
 *
 * On the board the HQ4 and GE11 run SGI programs that turn the OpenGL
 * client's command words into raster-engine work. Those programs are
 * opaque here, so the model reads the words at the boundary the IRIX
 * 6.5.30 client library defines and draws what the OpenGL 1.2.1 specification
 * says those commands produce. This is an approximation of the geometry
 * engine's function, not of its program.
 *
 * What it preserves: the pixels that colour and depth clears, untextured
 * points, lines, triangles, strips, fans, quads and polygons, at size and
 * width 1, flat or smooth shaded, lit per vertex by OpenGL's lighting
 * equation, culled by facing, transformed by the modelview and
 * projection matrices and their stacks, and bitmaps, leave in the
 * window's front or back buffer after the per-fragment operations the
 * shared rasterizer carries out (the scissor, alpha, stencil and depth
 * tests), in RGBA or
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
#include "hw/misc/sgi-mgras-gl.h"
#include "hw/display/sgi/rasterizer.h"
#include "trace.h"

/* Class-0 opcodes, bits 15:8 of the header, with their byte lengths. */
#define MGRAS_GL_CONTEXT_OPENER  0x00000600
/*
 * glBegin and glEnd: 0x1600 to 0x1f00 begin the ten primitive modes, and
 * each mode's end token is 0x0a00 above its begin token
 * (mgras-gl-commands.md, "Primitives"). The tokens do not follow the
 * modes' enumerant order: 0x1800 begins GL_LINE_STRIP (3) and 0x1900
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
#define MGRAS_GL_CLEAR_COLOR_BUF 0x00001500
#define MGRAS_GL_CLEAR_DEPTH_BUF 0x00004a00
#define MGRAS_GL_VIEWPORT        0x00003310
#define MGRAS_GL_FLUSH           0x00003600
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
 * decoder (notes-lists-arrays-lighting.md in the Origami repository):
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
 * glScissor: __glMgrim_Scissor (0xd97c260 in the IRIX 6.5.30 IP27GR532
 * libGLcore.so, SHA-256 626d6d94...) rejects a negative width or height
 * and writes 0x3910 and x, width - 1, y, height - 1 (0xd97c2a0-0xd97c2d8).
 * glEnable and glDisable of GL_SCISSOR_TEST write 0x6e04 and 1 or 0
 * (__glMgrim_Enable 0xd97e110 for capability 0xc11, __glMgrim_Disable
 * 0xd97fec4). Established from the listing. That the board discards
 * fragments outside the box as OpenGL 1.2.1, 4.1.2 says is inferred; the
 * discriminating test is gl-scissor-bounds-draws-and-clears.
 */
#define MGRAS_GL_SCISSOR         0x00003910
#define MGRAS_GL_SCISSOR_TEST    0x00006e04
/*
 * glAlphaFunc: __glMgrim_AlphaFunc (0xd95ea40) accepts a function from
 * GL_NEVER to GL_ALWAYS and writes 0x3e08, the function minus GL_NEVER,
 * and the reference clamped to [0, 1], times 4095.0, plus 0.5 and
 * truncated (the float at file offset 0x1d9414 is 4095.0 and the double
 * at 0x1d96a0 0.5), as a 16-bit signed value (0xd95eaa4-0xd95eae4): the
 * reference as a 12-bit fraction. glEnable and glDisable of
 * GL_ALPHA_TEST write 0x6404 and 1 or 0 (__glMgrim_Enable 0xd97dea4 for
 * capability 0xbc0; __glMgrim_Disable 0xd97fcb4). Established from the
 * listing; that the board tests fragments as OpenGL 1.2.1, 4.1.4 says is
 * inferred. The discriminating test is gl-alpha-test-discards.
 */
#define MGRAS_GL_ALPHA_FUNC      0x00003e08
#define MGRAS_GL_ALPHA_TEST      0x00006404
#define MGRAS_GL_ALPHA_REF_MAX   4095
/*
 * The stencil test, from the IRIX 6.5.30 IP27GR532 libGLcore.so's
 * emitters, established from the listing:
 *
 * - glStencilFunc (__glMgrim_StencilFunc 0xd95eec0): 0x410c, the function
 *   less GL_NEVER, the reference clamped to [0, 255] and the mask as the
 *   application passed it.
 * - glStencilOp (__glMgrim_StencilOp 0xd95ef80): 0x420c and the three
 *   operations as small numbers, GL_KEEP 0, GL_ZERO 1, GL_REPLACE 2,
 *   GL_INCR 3, GL_DECR 4 and GL_INVERT 5 (0xd95f074-0xd95f1b4); the
 *   glGet decoding maps them back the same way (__glMgrDoGet
 *   0xd993294-0xd9932e4).
 * - glStencilMask (0xd95e880): 0x3a04 and the mask.
 * - glClearStencil (0xd980c40): 0xbe04 and the value masked to the
 *   visual's stencil bits.
 * - glClear with GL_STENCIL_BUFFER_BIT (__glMgrim_Clear 0xd9e3670): 0x4c00
 *   alone, only when the visual has a stencil buffer.
 * - glEnable and glDisable of GL_STENCIL_TEST (0xd97e58c for capability
 *   0xb90, 0xd980354): 0x6f04 and 1 or 0.
 *
 * Every visual the X server offers with a depth buffer has 24 depth bits
 * and 8 stencil bits (glxinfo on the model, run
 * irix-20260928T004550-1040f3), so the board has an 8-bit stencil buffer
 * beside the 24-bit depth buffer (inferred). The model keeps it in bits
 * 31:24 of each pixel's depth word, an internal layout: how the board
 * packs the two is unknown. That the board tests and updates it as
 * OpenGL 1.2.1, 4.1.5 says is inferred. A context whose visual has no
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

/* glStencilOp's operations by the number the client sends for each. */
static const uint16_t mgras_gl_stencil_ops[] = {
    SGI_GL_KEEP, 0, SGI_GL_REPLACE, SGI_GL_INCR, SGI_GL_DECR, SGI_GL_INVERT,
};
/*
 * glDepthRange: __glMgrim_DepthRange (0xd960590 in the IRIX 6.5.30
 * IP27GR532 libGLcore.so, SHA-256 626d6d94...) writes 0x4808 and near and
 * far converted to single floats, unclamped, outside glBegin/glEnd.
 */
#define MGRAS_GL_DEPTH_RANGE     0x00004808
/*
 * glEnable and glDisable of GL_DITHER: __glMgrim_Enable writes 0x6704 and
 * 3 (0xd97d534, 0xd97e51c) and __glMgrim_Disable 0x6704 and 0 (0xd97f160
 * for pname 0xbd0, 0xd98028c), so a nonzero word enables (established).
 * Every context setup sends it with 3.
 */
#define MGRAS_GL_DITHER          0x00006704
/*
 * Polygon culling, from the IRIX 6.5.30 IP27GR532 libGLcore.so (SHA-256
 * 626d6d94...), established from the listing:
 *
 * - glCullFace: __glMgrim_CullFace (0xd95f260) accepts a mode from
 *   0x404 to 0x408 and writes 0x7404 and the mode unchanged (0xd95f2a8,
 *   0xd95f2ac); __glMgrlc_CullFace (0xd958c00) compiles the same pair
 *   into a display list.
 * - glFrontFace: __glMgrim_FrontFace (0xd9544b0) accepts 0x900 (GL_CW)
 *   or 0x901 (GL_CCW) and writes 0x5604 and the direction unchanged
 *   (0xd95450c, 0xd954510); __glMgrlc_FrontFace (0xd956450) compiles it.
 * - glEnable(GL_CULL_FACE): __glMgrim_Enable, for capability 0xb44,
 *   writes 0x7200 alone and sets bit 0x1000 of the context word at
 *   +0x700 (0xd97e300-0xd97e324). glDisable writes 0x7300 alone and
 *   clears the bit, only while it is set (__glMgrim_Disable,
 *   0xd980360-0xd980394).
 * - __glMgr_RestoreGE11CullState (0xd9e39a0) writes 0x7200 or 0x7300
 *   from that bit, and the host-culling glDrawArrays path
 *   (__glMgrim_HostCullDrawArraysEXT, 0xd9e3bc0) writes 0x7300 before
 *   drawing, so a client that culls on the host turns the board's
 *   culling off first and restores it after.
 *
 * Every context setup sends 0x7404 with 0x405 and 0x5604 with 0x901,
 * OpenGL's initial values (notes-unimplemented-tokens.md). That the
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
 * while they carry the default: 0xec10 from mgrTM_Init (0xd999454 in the
 * IRIX 6.5.30 IP27GR532 libGLcore.so), 32 per context setup, loading the
 * detail-texture function table glDetailTexFuncSGIS sets. Its words are
 * two HQ-table words, 0xc52 and 0xc78 from the hq3_revB_HQ4.u table, the
 * raster register 0x194 and one table value; the setup's values are 0x1f
 * three times and then 0x1d down to 1 (notes-unimplemented-tokens.md).
 * The SI has no texture memory and the model draws no texture, so the
 * table changes nothing it draws. The test is on each packet's values,
 * not on the setup's sequence, so an application's glDetailTexFuncSGIS
 * whose values fall in the same set also goes unlogged; that loses
 * nothing, since nothing is textured. Any other 0xec10, an application's
 * table with another value or a texture path's own register (0x191,
 * 0x193, 0x1a4), is logged.
 */
#define MGRAS_GL_TEXTURE_TABLE   0x0000ec10
#define MGRAS_GL_BANKS           0x00009808

/* Vertex-attribute headers: the class names the source format. */
#define MGRAS_GL_VERTEX2F        0x40000008
#define MGRAS_GL_VERTEX3F        0x2000000c
#define MGRAS_GL_VERTEX4F        0x00000010
#define MGRAS_GL_COLOR3F         0x2000020c
#define MGRAS_GL_COLOR4F         0x00000210
#define MGRAS_GL_VERTEX2I        0x58000008
#define MGRAS_GL_COLOR4UB        0x14800204
/*
 * glColor3ub is sent as 0x34800203 and one word. The model matches every
 * header with its byte length rounded up to the whole words the payload
 * takes (mgras-gl-commands.md, "Word framing").
 */
#define MGRAS_GL_COLOR3UB        0x34800204
/*
 * glNormal3f, and glEdgeFlag true and false (mgras-gl-commands.md and
 * notes-lists-arrays-lighting.md in the Origami repository).
 */
#define MGRAS_GL_NORMAL3F        0x0000070c
/*
 * The other glNormal3 forms, from their emitters in the IRIX 6.5.30
 * IP27GR532 libGLcore.so (SHA-256 626d6d94...), each the components as the
 * application passed them, established from the listing:
 *
 * - glNormal3d (0xdac8040) and 3dv: 0x12000718 and three doubles, each
 *   one 64-bit store, so its high word first.
 * - glNormal3i (0xdac80cc) and 3iv: 0x1c00070c and three ints.
 * - glNormal3s (0xdac8114) and 3sv: 0x1d000706, then x in bits 31:16 and
 *   y in 15:0 of one word, and z in bits 31:16 of a second.
 * - glNormal3b (0xdac7fd0) and 3bv: 0x1c800703, then x, y and z in bits
 *   31:24, 23:16 and 15:8 of one word.
 *
 * The short and byte headers are matched with their lengths rounded up
 * to the words they take. The client converts nothing, so the board does;
 * that it converts as OpenGL 1.2.1's table 2.6 gives for a signed normal
 * component, (2c + 1) / (2^b - 1) for b bits, is inferred from the
 * specification and the classes' reading as scaled types in
 * mgras-gl-commands.md.
 */
#define MGRAS_GL_NORMAL3D        0x12000718
#define MGRAS_GL_NORMAL3I        0x1c00070c
#define MGRAS_GL_NORMAL3S        0x1d000708
#define MGRAS_GL_NORMAL3B        0x1c800704
/* glRasterPos2f and 3f (mgras-gl-commands.md, attribute table). */
#define MGRAS_GL_RASTER_POS2F    0x40003808
#define MGRAS_GL_RASTER_POS3F    0x2000380c
#define MGRAS_GL_EDGE_FLAG_TRUE  0x00000f00
#define MGRAS_GL_EDGE_FLAG_FALSE 0x00001000

/*
 * Colour-index mode, from the IRIX 6.5.30 IP27GR532 libGLcore.so's own
 * disassembly (SHA-256 626d6d94...). The context setup calls
 * __glMgrInitCI (0xd94e830) for each colour buffer when the visual is a
 * colour-index one and another initializer otherwise; __glMgrInitCI
 * writes 0x0a04 and a word of the visual's configuration, from which it
 * also forms the index mask (1 << n) - 1, and __glMgrInitRGB (0xd973620)
 * writes 0x0900 and nothing more. So 0x0a04's word is the number of
 * bits a colour index holds, 8 in gr_osview's context setup and in
 * glxinfo's (the application survey's run irix-20260928T004550-1040f3,
 * and mgras-gl-commands.md), and 0x0900 marks an RGBA context. That the
 * board switches its colour handling on them is inferred.
 */
#define MGRAS_GL_INIT_CI         0x00000a04
#define MGRAS_GL_INIT_RGB        0x00000900
/*
 * glIndexs, glIndexi and glIndexf: headers 0x19000202, 0x18000204 and
 * 0x00000204, the colour register 0x02 under the attribute classes for a
 * short, an integer and a float (__glMgrim_Indexs 0xdac7f90,
 * __glMgrim_Indexi 0xdac7f5c, __glMgrim_Indexf 0xdac7f30). glIndexs
 * sends the short's 16 bits in bits 31:16 of its one word and zeros in
 * 15:0 (andi 0xffff, dsll 16); gr_osview's words 0x20000 and 0x30000 are
 * indices 2 and 3. The short is read signed, as the class's short type
 * reads for a vertex (inferred from the attribute table); glIndexs's
 * header is matched with its length rounded up to the word it takes.
 */
#define MGRAS_GL_INDEXS          0x19000204
#define MGRAS_GL_INDEXI          0x18000204
#define MGRAS_GL_INDEXF          0x00000204
/*
 * glIndexMask (__glMgrim_IndexMask 0xd95e9d0): 0x3d04 and the mask ANDed
 * with the context's index mask. glClearIndex (__glMgrim_ClearIndex
 * 0xd980ba0): 0xbd04 and a float, trunc(c * 16.0) ANDed with (index mask
 * << 4) | 0xf and scaled by 0.0625, the constants the lit4 words at file
 * offsets 0x1d940c and 0x1d9410 hold: the index with four fraction bits.
 */
#define MGRAS_GL_INDEX_MASK      0x00003d04
#define MGRAS_GL_CLEAR_INDEX     0x0000bd04

/*
 * glBitmap (__glMgrim_Bitmap 0xd9c1b70, and __glMgrRenderBitmap 0xd9c1e70
 * for a display list). In render mode, with glPixelStore's unpack state at
 * its defaults but an alignment of one, and an image of at most 512
 * words, the client writes 0x9420 and eight words: a flags word, the width
 * and height as integers, xorig, yorig, xmove and ymove as floats, and the
 * image's length in words, ceil(ceil(width / 8) * height / 4). An inline
 * data block follows, 0x80000000 | length * 4, whose words
 * __glMgr_PushPixels (0xdac61a0) copies from the application's image in
 * memory order; a 64-bit store is two stream words, high first. When
 * the image is not word aligned the client sets bits 23:22 of the block
 * header to the image's byte offset in its first word and sends one word
 * more, from the word holding the first byte. A zero word follows when
 * the number of words sent is odd: when the length is odd for an aligned
 * image, and when it is even for one with an offset (0xd9c1e3c-0xd9c1e50
 * and 0xd9c1d00-0xd9c1d14). gr_osview's 94 bitmaps in the survey run
 * (irix-20260928T004550-1040f3) are all this form: flags 0x1800f, width
 * 16, height 1 or 2, one data word and the zero word. The other paths,
 * for a larger image or other unpack state (0x9114, 0x9008 with raster
 * register records, then 0x9208 with xmove and ymove), are not modelled.
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
 * __glMgrUpdateRasterPos (0xd9c9040) writes 0xd608 and two floats; the
 * client's glBitmap calls it with xmove and ymove for an empty image, so
 * it moves a valid raster position by them (inferred, 3.7).
 */
#define MGRAS_GL_MOVE_RASTER     0x0000d608
/*
 * __glMgrim_RasterPos2i (0xdac81e8) writes 0x58003808 and the two
 * integers; OpenGL takes them as glRasterPos4f(x, y, 0, 1) (2.12).
 */
#define MGRAS_GL_RASTER_POS2I    0x58003808
/*
 * The words each pixel transfer brackets its GE program with:
 * __glMgrSaveRSS (0xd9c8da0) writes 0xd214 and five words and
 * __glMgrRestoreRSS 0xd300, __glMgrSaveTexmode (0xd9a17c0) 0xe910 and
 * four words and __glMgrRestoreTexmode 0xea00, and
 * __glMgrLoadRasterPosInfo (0xd9c9030) 0xd700 alone. The model draws the
 * image without raster registers or texturing, so it consumes them;
 * that they change nothing a later draw sees is inferred from their
 * names and pairing.
 */
#define MGRAS_GL_SAVE_RSS        0x0000d214
#define MGRAS_GL_RESTORE_RSS     0x0000d300
#define MGRAS_GL_LOAD_RASTER_INFO 0x0000d700
#define MGRAS_GL_SAVE_TEXMODE    0x0000e910
#define MGRAS_GL_RESTORE_TEXMODE 0x0000ea00
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
 * (run irix-20260927T172059-fdbada), where OpenGL's default draw buffer is
 * the front and the back buffer respectively. Reading it as a bit mask of
 * the front and back buffers, SGI_GL_DRAW_FRONT and SGI_GL_DRAW_BACK, is
 * inferred from those two values; the other bits are not modelled.
 */

/*
 * The window the current context draws into, from the 63-word image the
 * IRIX kernel's MgrasCtxsw writes at each context switch. In the image of
 * the probe's GL context, whose window's top-left pixel is at screen (256,
 * 256) and which is 128 pixels square (run irix-20260927T135825-92207b,
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
 * The kernel's MgrasValidateClip (IRIX 6.5.30 mgras.a member mgras.o,
 * SHA-256 6a65e70d3cdc..., listing 0x61d8-0x67b4) writes words 2 to 15
 * from the window and its visible rectangles, bottom-up and inclusive,
 * each as first << 16 | last. Established from that listing:
 *
 * - Word 3 is (mode << 4) | enables. Enable bit i is set for each of the
 *   first four visible rectangles, rectangle 0 in words 11 and 12, 1 in
 *   9 and 10, 2 in 7 and 8 and 3 in 5 and 6. All four pairs are stored
 *   on every call (0x6534-0x6558 on the port, 0x63e8-0x6424 in the saved
 *   copy); a pair whose bit is clear carries leftover stack contents that
 *   mean nothing.
 * - A window with one to four visible rectangles gets mode 1 and those
 *   rectangles. One with none, but a nonzero width, gets mode 1 and its
 *   own rectangle clamped to the screen.
 * - One with five or more gets mode 1 and no enable bits: 0x10.
 * - The kernel's null clip (MgrasSetNullClip, a window of no size and no
 *   rectangles) gets mode 2 with rectangles 0 and 1 both the whole
 *   screen, 0x23, and word 2 then puts the origin at row 1024. Every
 *   image of perfly's context 0x84c6 in run irix-20260928T000530-78d0c5
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
#define MGRAS_GL_CLIP_RECTS      4
#define MGRAS_GL_CLIP_INSIDE     1
#define MGRAS_GL_CLIP_NULL       2

/* Each rectangle's column and row words, rectangle 0 first. */
static const uint8_t mgras_gl_clip_words[MGRAS_GL_CLIP_RECTS][2] = {
    { 11, 12 }, { 9, 10 }, { 7, 8 }, { 5, 6 },
};

/*
 * Words 16 and 17 of every captured GL context image are 1, those of the
 * X server's images 0 (run irix-20260927T172059-fdbada). The kernel's
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
 * Lighting, from the IRIX 6.5.30 IP27GR532 libGLcore.so's emitters
 * (notes-lists-arrays-lighting.md in the Origami repository, established
 * there): glLightfv writes a header per parameter, the light's number
 * (light - GL_LIGHT0) and the parameter's floats, the application's
 * values untransformed; glLightModelfv writes the ambient's red, green and
 * blue and not its alpha, and the two booleans as integers; glMaterialfv
 * writes the face and parameter enumerants and always four floats;
 * glColorMaterial writes one header per face with the parameter, and
 * glEnable and glDisable of GL_COLOR_MATERIAL one word each. glGetLightfv
 * and glGetMaterialfv write 0xef08 with the light's number and parameter
 * or 0xee08 with the face and parameter, then query 0x3ba to 0x3bd and
 * read four floats, keeping as many as the parameter has; the index does
 * not depend on the light or face, so the board puts the parameter in
 * one place for the queries (inferred). The model keeps the state in the
 * shared context, which transforms positions and spot directions by the
 * modelview when they arrive, and answers from it.
 *
 * The enables, from __glMgrim_Enable (0xd97d970) and __glMgrim_Disable
 * (0xd97f710), established from the listing:
 *
 * - glEnable(GL_LIGHTING) sets bit 0x40 of the context's flags word and
 *   calls __glMgrEnableLighting (0xd9543a0), which writes 0x0d00 alone
 *   while that bit is set; the probe's lighting-only case captured
 *   0x0d00 and nothing else. glDisable(GL_LIGHTING) writes 0x0e00 through
 *   __glMgrDisableLighting (0xd9543d0), and __glMgrSetSoftHardwareState
 *   (0xd9dfbfc) writes it as a header too. The other 0x0e00 words in the
 *   listing are payloads of 0x000c0004 descriptor headers.
 * - glEnable(GL_LIGHTi) writes 0x7104 and i, the capability less
 *   GL_LIGHT0 (0xd97de60, __glMgrEnableLightSource 0xd97d950), and
 *   glDisable 0xa004 and i (0xd97fc28, 0xd97f6f0).
 * - glEnable(GL_NORMALIZE) writes 0xa304 and 1 (0xd97e1cc, the 1 held in
 *   $a3 from 0xd97d980), glDisable 0xa304 and 0 (0xd97ff54).
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

static uint32_t mgras_gl_begin_of_mode(uint32_t mode)
{
    assert(mode < ARRAY_SIZE(mgras_gl_begin_tokens));
    return mgras_gl_begin_tokens[mode];
}

/*
 * A vertex holding OpenGL's initial attributes (2.7): position and
 * texture coordinate (0, 0, 0, 1), normal (0, 0, 1), edge flag true,
 * colour index 1, material 0.
 */
static void mgras_gl_vertex_init(float v[SGI_GL_VERTEX_FLOATS])
{
    static const uint32_t edge = 1;

    memset(v, 0, SGI_GL_VERTEX_FLOATS * sizeof(float));
    v[SGI_GL_V_POSITION + 3] = 1.0f;
    v[SGI_GL_V_NORMAL + 2] = 1.0f;
    v[SGI_GL_V_TEXCOORD + 3] = 1.0f;
    memcpy(&v[SGI_GL_V_EDGE], &edge, sizeof(edge));
    v[SGI_GL_V_INDEX] = 1.0f;
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
    switch (header) {
    case MGRAS_GL_CONTEXT_OPENER:
    case MGRAS_GL_CLEAR_COLOR_BUF:
    case MGRAS_GL_CLEAR_DEPTH_BUF:
    case MGRAS_GL_VIEWPORT:
    case MGRAS_GL_FLUSH:
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
    case MGRAS_GL_VERTEX2F:
    case MGRAS_GL_VERTEX3F:
    case MGRAS_GL_VERTEX4F:
    case MGRAS_GL_COLOR3F:
    case MGRAS_GL_COLOR4F:
    case MGRAS_GL_VERTEX2I:
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
        return true;
    default:
        return false;
    }
}

/*
 * glColor3ub's and glColor4ub's bytes in one word, the first component in
 * bits 31:24: colorbars' bars (run irix-20260928T004550-1040f3, from seq
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
 * Context image word 0 names the context's GL state. In run
 * irix-20260927T191513-b4997c it is 0x3bcd and 0x5420 for two GL clients,
 * 0x237a for the X server and 0xb27 for the kernel's default context.
 * These are allocations observed in that run, not published values, and
 * MGRAS_GL_DEFAULT_SLOT is the one observed there. The IRIX 6.5.30 kernel
 * allocates the word as the context's slot in GE external RAM and writes
 * -1 for a context with no GL state. The image
 * carries no other GL state, the clients never re-send theirs after a
 * switch, and nothing touches the GE ports at those switches, so the board
 * keeps each context's state under its slot (inferred;
 * mgras-gl-commands.md, "Per-context state").
 *
 * Approximated: the model keys its state by the slot address the kernel
 * chose, not by GE external RAM contents, and never interprets the GE
 * ports. When the kernel runs out of slots it copies one out through the
 * GE ports and later back in, possibly under another address; the model
 * cannot follow that. It keeps the old state under a reused address,
 * relying on the opener to start a new context, and cannot restore a
 * copied-back one. No capture has had enough GL contexts to show it. The
 * model holds 32 slots and replaces the least recently loaded when a 33rd
 * arrives. The discriminating tests are gl-state-follows-the-context-slot,
 * gl-context-opener-resets-only-its-context and
 * gl-context-table-replaces-the-least-recent.
 */
#define MGRAS_GL_IMAGE_SLOT      0
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
 * the screen; with none, the context has no window.
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
static void mgras_gl_bind(SGIMgrasState *s)
{
    uint32_t origin = s->context[MGRAS_GL_IMAGE_ORIGIN];
    uint32_t clip = s->context[MGRAS_GL_IMAGE_CLIP_MODE];
    uint32_t mode = clip >> 4;
    unsigned int back = s->gl_banks[0] & 1;
    SGIGLDrawable d = {
        .origin = { origin & 0xffff, origin >> 16 },
        .front = 1u << (back ^ 1),
        .back = 1u << back,
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
    d.bound = d.clip.n != 0;
    if ((mode == MGRAS_GL_CLIP_INSIDE && !(clip & 0xf)) ||
        (mode != MGRAS_GL_CLIP_INSIDE && clip != 0 &&
         mode != MGRAS_GL_CLIP_NULL)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: context image clip word 0x%x "
                      "gives no rectangle the model can bind; nothing "
                      "draws\n", clip);
    }
    trace_sgi_mgras_gl_bind(s->context[MGRAS_GL_IMAGE_SLOT], origin, clip,
                            s->context[11], s->context[12], s->context[9],
                            s->context[10], d.bound);
    sgi_gl_set_drawable(mgras_gl(s), &d);
}

void sgi_mgras_gl_reset(SGIMgrasState *s)
{
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        s->gl_context[i].gl = &s->gl[i];
        memset(&s->gl_context[i].parser, 0, sizeof(s->gl_context[i].parser));
        memset(s->gl_context[i].readback, 0,
               sizeof(s->gl_context[i].readback));
        memset(&s->gl_context[i].pixels, 0,
               sizeof(s->gl_context[i].pixels));
        sgi_gl_init(&s->gl[i], 0);
    }
    memset(s->ge_param, 0, sizeof(s->ge_param));
    s->ge_param_logged = false;
    memset(s->gl_slot, 0, sizeof(s->gl_slot));
    memset(s->gl_selected, 0, sizeof(s->gl_selected));
    s->gl_selections = 0;
    s->gl_current = 0;
    s->gl_ge_port_logged = false;
    /* The value every captured GL context image restores. */
    s->gl_banks[0] = s->gl_banks[1] = 1;
    mgras_gl_bind(s);
}

/* The entry holding slot's state, taking a new one if it has none. */
static unsigned int mgras_gl_lookup(SGIMgrasState *s, uint32_t slot)
{
    unsigned int free = 0, oldest = 0;

    for (unsigned int i = 1; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (!s->gl_selected[i]) {
            free = free ? free : i;
        } else if (s->gl_slot[i] == slot) {
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
    sgi_gl_init(&s->gl[free], slot);
    memset(&s->gl_context[free].parser, 0,
           sizeof(s->gl_context[free].parser));
    memset(s->gl_context[free].readback, 0,
           sizeof(s->gl_context[free].readback));
    memset(&s->gl_context[free].pixels, 0,
           sizeof(s->gl_context[free].pixels));
    s->gl_slot[free] = slot;
    return free;
}

/*
 * A context switch can land inside a packet, and each context's words
 * continue its own packet when it is loaded again. In run
 * irix-20260928T000530-78d0c5 (seq 12929748 to 12930582) perfly's context
 * 0x84c6 was switched out 33 words into the 156 data words of a GE table
 * load, context 0x5420 was loaded and sent one glFlush header, and 0x84c6
 * came back and sent its remaining 123 data words. With one parser for
 * the board the glFlush was consumed as table data and 0x84c6's words then
 * parsed as headers, which later left the X server polling HQ flag bit 16
 * forever. The client cannot know where a switch falls, so the board
 * keeps the position per context (inferred). Approximated: the model
 * keeps it with the context's GL state, keyed the same way, not in
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
    sgi_mgras_cmd_parser_load(s, &s->gl_context[s->gl_current].parser);
    mgras_gl_bind(s);
}

/*
 * The answer to the client's 0xa104 query for one current attribute.
 * __Mgr_GetCurrentAttrState reads indices 4 to 7 as GL_CURRENT_COLOR's
 * four floats, 8 to 10 as GL_CURRENT_NORMAL's, 11 to 14 as
 * GL_CURRENT_TEXTURE_COORDS' and 15 as GL_EDGE_FLAG, true when the answer
 * has any of bits 0x01c00000 (notes-return-mode-queries.md in the Origami
 * repository). The model answers with the loaded context's colour, normal
 * and edge flag; that the board answers with its copy of that state is
 * inferred. Which of the edge flag's three bits the board sets is
 * unknown, so the model sets all three. Texture coordinates are not
 * modelled: the model answers OpenGL's initial (0, 0, 0, 1) (2.7), wrong
 * once a client has sent glTexCoord. Other indices answer zero, logged.
 * The discriminating tests are hq-attribute-queries-set-flag-after-arguments
 * and hq-attribute-query-answers-normal-and-edge-flag.
 *
 * The same routine reads GL_CURRENT_INDEX at index 4, and the raster
 * index shares 0x351 with the raster colour's red in the 0xa20c table,
 * so the board keeps an index where it keeps red (inferred). In
 * colour-index mode the model answers both with the index, as a float
 * the client truncates.
 */
#define MGRAS_GL_ATTR_INDEX      4
#define MGRAS_GL_QUERY_RASTER_INDEX 0x351

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
} MgrasGLQueryPacking;

/*
 * The client adds this double, read at $gp - 0x39e8 in
 * __glMgrDoGet, to the x and y words of GL_CURRENT_RASTER_POSITION
 * before returning them (notes-return-mode-queries.md).
 */
#define MGRAS_GL_RASTER_BIAS     49151.5

/*
 * The 0xa20c state queries the model answers. __Mgr_ReturnMode sends an
 * index, a1 and a2 and reads one word back at 0x028004
 * (notes-return-mode-queries.md in the Origami repository). No index is a
 * literal: __glMgrDoGet takes each from a table the library loads from
 * the HQ microcode file's symbol table, so the index is an address the
 * board's program gives for that piece of GL state. The notes list each
 * glGet caller's index and what it does with the word, established from
 * the IRIX 6.5.30 IP27GR532 libGLcore.so; the values below are the
 * hq3_revB_HQ4.u column. That the board answers with its copy of the
 * state OpenGL 1.2.1 has glGet return is inferred from how the client
 * uses each word, and the model answers the loaded context's.
 *
 * - GL_MODELVIEW_MATRIX and GL_PROJECTION_MATRIX: 16 queries with a1 0,
 *   index 0x3a + i and 0x4a + i, each word stored as a float into
 *   element i of the result (the loop at 0xd992d28 advances its output
 *   one float per query), which glGet returns column-major.
 * - GL_TEXTURE_MATRIX: 0x6a + i with a1 0 (table offset 0x164), read the
 *   same way (0xd994030).
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
 *   with the microcode symbol table's word at offset 0x240 and returns
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
 *   less the first two plus 1 (0xd994f1c-0xd994f5c). GL_SCISSOR_TEST at
 *   0xc28, the whole word the boolean.
 * - GL_STENCIL_CLEAR_VALUE at 0xc14, an integer.
 *
 * What a1 and a2 select is unknown; the model answers by index alone.
 * Every other index answers zero, logged, which is right for the
 * glFinish and MgrasFinish fences and glGetError, which discard or fall
 * back from a zero, and wrong for the other state a client reads back.
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
    { 0xc14, 1, MGRAS_GL_QUERY_WORD, SGI_GL_STENCIL_CLEAR_VALUE, 0 },
};

/*
 * GL_MODELVIEW_STACK_DEPTH and GL_PROJECTION_STACK_DEPTH. __glMgrDoGet
 * queries index 0x83 (table offset 0x1f4, a1 0) and 0x922 (0x1f8, a1 1)
 * and returns ((answer - base) >> 4) + 1, the base the symbol-table word
 * at offset 0x1e4 for the modelview stack and 0x1e8 for the projection
 * stack (0xd99427c-0xd9942a8 and 0xd993678-0xd9936a4 in the IRIX 6.5.30
 * IP27GR532 libGLcore.so). Those words, at file offsets 0x210 and 0x214
 * of /usr/gfx/ucode/MGRAS/hq3_revB_HQ4.u (SHA-256
 * a696e9772a876ed02378111e80a01270161cb4ad8fc39355f40fe96d1fcdb062), are
 * 0x925 and 0xb25; the index words at 0x220 and 0x224 are 0x83 and
 * 0x922, as the query notes give them. So the answer is an address in
 * the GE's matrix stack, 16 words a matrix, and the model answers base +
 * 16 * (depth - 1) from the loaded context's stack. That the board's
 * word is that address is inferred from the client's arithmetic; the
 * modelview base lies 32 matrices below the projection base, the depth
 * the model gives both stacks.
 *
 * GL_TEXTURE_STACK_DEPTH is index 0x923 (table offset 0x1fc) with its
 * base at table offset 0x1ec (0xd994db8-0xd994de4); the file words at
 * 0x228 and 0x218 are 0x923 and 0xb45. The texture base lies two
 * matrices above the projection base, so the board's projection stack
 * holds two (inferred), where the model's holds 32 like its others. The
 * colour-matrix stack is not kept, and its query still answers zero.
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
 * and one RE (notes-return-mode-queries.md, "Where the index comes
 * from"). The kernel records the low byte of 0x028004 after the GE
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
 * holds it (notes-return-mode-queries.md), and the client decodes each
 * field as below (established from the listing). Every context setup's
 * 0x7c08 block pairs a raster register and value with one of these
 * indices as a third word, and each value decodes, as the client decodes
 * that query, to OpenGL's initial state (notes-unimplemented-tokens.md,
 * "The triple's third word is a query address"):
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
 *   the depth write mask true when bits 27:4 are all set
 *   (0xd9949bc-0xd9949d4);
 * - 0x2cc, setup 7: the alpha function less GL_NEVER in bits 2:0, the
 *   reference as the 12-bit fraction glAlphaFunc sends in bits 14:3.
 *
 * The model does not answer from a table of those 0x7c08 values: the
 * same state reaches the raster engine through GL opcodes, such as
 * glDepthFunc (0x4304), which the GE turns into register writes the model
 * never sees as triples, so a stored triple would go stale at the first
 * of them. Instead each field comes from the loaded context's state
 * (inferred: that the board answers with the state the client last set).
 * A field the model does not track keeps the setup value, and such an
 * answer is logged; the bits no query reads keep the setup value's.
 */
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
    bool untracked = false;

    switch (index) {
    case MGRAS_GL_QUERY_FILL_ENABLES:
        *answer = MGRAS_GL_FILL_SETUP;
        untracked = true;
        break;
    case MGRAS_GL_QUERY_PIXEL_ENABLES:
        *answer = (MGRAS_GL_PIXEL_SETUP & ~0x51u) | gl->cur.dither |
                  st->stencil_test << 4 | st->alpha_test << 6;
        untracked = true;
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
    if (untracked) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL state query index 0x%x "
                      "answers OpenGL's initial value for state the model "
                      "does not track\n", index);
    }
    return true;
}

/*
 * GL_COLOR_WRITEMASK and GL_INDEX_WRITEMASK are both query 0x32b (table
 * offset 0x4c0; notes-return-mode-queries.md). For the colour mask the
 * client tests the answer's bits 0 to 3 in turn as red, green, blue and
 * alpha (0xd992858-0xd9928e0); for the index mask it returns the answer
 * ANDed with (1 << n) - 1, n the context word at 0xcb4, the index's bits
 * (0xd9959f4-0xd995a30). Established from the IRIX 6.5.30 IP27GR532
 * libGLcore.so. That the board keeps one write-mask word for both is
 * inferred from the shared index. In colour-index mode the model answers
 * glIndexMask's mask; in RGBA mode it answers 0xf, all four colour
 * channels writable as OpenGL starts, since glColorMask is not decoded,
 * and logs that answer. 0x32c is glGetError's index, not a mask, and its
 * zero answer stays right.
 */
#define MGRAS_GL_QUERY_WRITEMASK 0x32b
#define MGRAS_GL_COLOUR_WRITES   0xf

static uint32_t mgras_gl_writemask_answer(SGIMgrasState *s)
{
    uint32_t out[SGI_GL_GET_WORDS];
    unsigned n;

    if (mgras_gl(s)->cur.index_bits &&
        sgi_gl_get(mgras_gl(s), SGI_GL_INDEX_WRITEMASK, out, &n)) {
        return out[0];
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mgras: GL colour write mask query answers "
                  "OpenGL's initial mask; glColorMask is not tracked\n");
    return MGRAS_GL_COLOUR_WRITES;
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
    if (mgras_gl_raster_query(s, index, &out[0])) {
        return out[0];
    }
    if (index == MGRAS_GL_QUERY_WRITEMASK) {
        return mgras_gl_writemask_answer(s);
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
        assert(element < n);
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

/*
 * The sizes MGRAS's stream carries: three held vertices since version 15
 * and 31 saved matrices a stack since version 21. They are the stream's,
 * not the shared context's, so a change to the shared sizes stops the
 * build here instead of changing the stream's length.
 */
#define MGRAS_GL_WIRE_HELD       3
#define MGRAS_GL_WIRE_STACK      31
QEMU_BUILD_BUG_ON(SGI_GL_HELD_VERTICES != MGRAS_GL_WIRE_HELD);
QEMU_BUILD_BUG_ON(SGI_GL_STACK_DEPTH - 1 != MGRAS_GL_WIRE_STACK);

/*
 * The per-context GL state the stream has carried since version 15, in
 * that version's layout, which the shared context's fields no longer
 * follow for the open primitive: there the stream has the begin token,
 * the vertex count and each held vertex's position and colour, as two
 * arrays.
 *
 * The stream carries no other attribute of a held vertex, so one loaded
 * from it takes OpenGL's initial normal, texture coordinate and edge
 * flag, not the ones current when it was sent. Nothing reads those once
 * the vertex is held: lighting reads the normal as the vertex arrives,
 * and a lit vertex's colour is its lit colour. Its back colour travels
 * in the light-enables subsection; edge flags will need theirs carried.
 */
typedef struct MgrasGLPrimitiveWire {
    SGIGLContext *parent;
    uint32_t begin;
    uint32_t vertex_count;
    uint32_t vertex[MGRAS_GL_WIRE_HELD][4];
    uint32_t vertex_color[MGRAS_GL_WIRE_HELD][4];
} MgrasGLPrimitiveWire;

static int mgras_gl_primitive_pre_save(void *opaque)
{
    MgrasGLPrimitiveWire *w = opaque;
    SGIGLContext *gl = w->parent;

    w->begin = gl->in_begin ? mgras_gl_begin_of_mode(gl->begin_mode) : 0;
    w->vertex_count = gl->nheld;
    for (int i = 0; i < MGRAS_GL_WIRE_HELD; i++) {
        memcpy(w->vertex[i], &gl->held[i][SGI_GL_V_POSITION],
               sizeof(w->vertex[i]));
        memcpy(w->vertex_color[i], &gl->held[i][SGI_GL_V_COLOR],
               sizeof(w->vertex_color[i]));
    }
    return 0;
}

static int mgras_gl_primitive_post_load(void *opaque, int version_id)
{
    MgrasGLPrimitiveWire *w = opaque;
    SGIGLContext *gl = w->parent;

    if (w->vertex_count > MGRAS_GL_WIRE_HELD ||
        (w->begin && !mgras_gl_begin_token(w->begin, false))) {
        return -EINVAL;
    }
    gl->in_begin = w->begin != 0;
    gl->begin_mode = w->begin ? mgras_gl_mode_of_begin(w->begin) : 0;
    gl->nheld = w->vertex_count;
    for (int i = 0; i < MGRAS_GL_WIRE_HELD; i++) {
        mgras_gl_vertex_init(gl->held[i]);
        memcpy(&gl->held[i][SGI_GL_V_POSITION], w->vertex[i],
               sizeof(w->vertex[i]));
        memcpy(&gl->held[i][SGI_GL_V_COLOR], w->vertex_color[i],
               sizeof(w->vertex_color[i]));
    }
    return 0;
}

static const VMStateDescription vmstate_mgras_gl_primitive_wire = {
    .name = "tmp",
    .pre_save = mgras_gl_primitive_pre_save,
    .post_load = mgras_gl_primitive_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(begin, MgrasGLPrimitiveWire),
        VMSTATE_UINT32(vertex_count, MgrasGLPrimitiveWire),
        VMSTATE_UINT32_2DARRAY(vertex, MgrasGLPrimitiveWire,
                               MGRAS_GL_WIRE_HELD, 4),
        VMSTATE_UINT32_2DARRAY(vertex_color, MgrasGLPrimitiveWire,
                               MGRAS_GL_WIRE_HELD, 4),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_sgi_mgras_gl_primitive = {
    .name = TYPE_SGI_MGRAS "/gl-primitive",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_WITH_TMP(SGIGLContext, MgrasGLPrimitiveWire,
                         vmstate_mgras_gl_primitive_wire),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_sgi_mgras_gl_context = {
    .name = TYPE_SGI_MGRAS "/gl-context",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32_ARRAY(cur.viewport, SGIGLContext, 4),
        VMSTATE_SGI_GL_FLOATS(clear_color, SGIGLContext, 4, 0),
        VMSTATE_SGI_GL_FLOATS(cur_color, SGIGLContext, 4, 0),
        VMSTATE_UINT32(cur.shade_model, SGIGLContext),
        VMSTATE_WITH_TMP(SGIGLContext, MgrasGLPrimitiveWire,
                         vmstate_mgras_gl_primitive_wire),
        VMSTATE_UINT32(matrix_mode, SGIGLContext),
        VMSTATE_SGI_GL_FLOATS(cur.modelview, SGIGLContext, 16, 0),
        VMSTATE_SGI_GL_FLOATS(cur.projection, SGIGLContext, 16, 0),
        VMSTATE_UINT32(cur.depth_test, SGIGLContext),
        VMSTATE_UINT32(cur.depth_func, SGIGLContext),
        VMSTATE_UINT32(cur.depth_mask, SGIGLContext),
        VMSTATE_UINT32(clear_depth, SGIGLContext),
        VMSTATE_UINT32(cur.draw_buffers, SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_cmd_parser = {
    .name = TYPE_SGI_MGRAS "/cmd-parser",
    .version_id = 1,
    .minimum_version_id = 1,
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

/* The state each context gained in version 20, sent for every entry. */
const VMStateDescription vmstate_sgi_mgras_gl_context_v20 = {
    .name = TYPE_SGI_MGRAS "/gl-context-v20",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parser, SGIMgrasGLContext, 1,
                       vmstate_sgi_mgras_cmd_parser, SGIMgrasCmdParser),
        VMSTATE_END_OF_LIST()
    }
};

/* The matrix stacks each context gained in version 21. */
const VMStateDescription vmstate_sgi_mgras_gl_context_v21 = {
    .name = TYPE_SGI_MGRAS "/gl-context-v21",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_2DARRAY(mv_stack, SGIGLContext, MGRAS_GL_WIRE_STACK, 16,
                        0, vmstate_info_uint32, float),
        VMSTATE_2DARRAY(pr_stack, SGIGLContext, MGRAS_GL_WIRE_STACK, 16,
                        0, vmstate_info_uint32, float),
        VMSTATE_UINT32(mv_depth, SGIGLContext),
        VMSTATE_UINT32(pr_depth, SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/* The current normal and edge flag each context gained in version 22. */
const VMStateDescription vmstate_sgi_mgras_gl_context_v22 = {
    .name = TYPE_SGI_MGRAS "/gl-context-v22",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_GL_FLOATS(cur_normal, SGIGLContext, 3, 0),
        VMSTATE_UINT32(cur_edge, SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_raster = {
    .name = TYPE_SGI_MGRAS "/gl-context-raster",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_GL_FLOATS(raster_pos, SGIGLContext, 4, 0),
        VMSTATE_UINT32(raster_valid, SGIGLContext),
        VMSTATE_SINGLE(raster_distance, SGIGLContext, 0, vmstate_info_uint32,
                       float),
        VMSTATE_SGI_GL_FLOATS(raster_color, SGIGLContext, 4, 0),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Every context's raster position, sent only once one has left OpenGL's
 * initial one, so a stream from a guest that never sent glRasterPos stays
 * loadable by a binary that predates it. A destination that lacks this
 * section refuses a stream that carries it rather than lose the position.
 * A stream without it leaves the destination's initial positions, which
 * are what a source that predates it had: it never decoded glRasterPos.
 */
static bool mgras_gl_raster_needed(void *opaque)
{
    SGIMgrasState *s = opaque;
    g_autofree SGIGLContext *init = g_new(SGIGLContext, 1);

    sgi_gl_init(init, 0);
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIGLContext *gl = &s->gl[i];

        if (memcmp(gl->raster_pos, init->raster_pos,
                   sizeof(gl->raster_pos)) ||
            gl->raster_valid != init->raster_valid ||
            memcmp(&gl->raster_distance, &init->raster_distance,
                   sizeof(gl->raster_distance)) ||
            memcmp(gl->raster_color, init->raster_color,
                   sizeof(gl->raster_color))) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_raster = {
    .name = TYPE_SGI_MGRAS "/gl-raster",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_raster_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_raster,
                             SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_index = {
    .name = TYPE_SGI_MGRAS "/gl-context-index",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cur.index_bits, SGIGLContext),
        VMSTATE_UINT32(cur.index_mask, SGIGLContext),
        VMSTATE_SINGLE(cur_index, SGIGLContext, 0, vmstate_info_uint32,
                       float),
        VMSTATE_SINGLE(raster_index, SGIGLContext, 0, vmstate_info_uint32,
                       float),
        VMSTATE_SINGLE(clear_index, SGIGLContext, 0, vmstate_info_uint32,
                       float),
        VMSTATE_SINGLE(held[0][SGI_GL_V_INDEX], SGIGLContext, 0,
                       vmstate_info_uint32, float),
        VMSTATE_SINGLE(held[1][SGI_GL_V_INDEX], SGIGLContext, 0,
                       vmstate_info_uint32, float),
        VMSTATE_SINGLE(held[2][SGI_GL_V_INDEX], SGIGLContext, 0,
                       vmstate_info_uint32, float),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Every context's colour-index state, sent only once some context has
 * left what a model without colour-index mode keeps, OpenGL's initial
 * state and a held vertex's index of 1, so a guest that never used it
 * migrates to and from such a binary, and one that did is refused by it.
 * A stream without the section comes from a model that drew every
 * context in RGBA mode, which is what the destination's initial state
 * does.
 */
static bool mgras_gl_index_needed(void *opaque)
{
    SGIMgrasState *s = opaque;
    static const float one = 1.0f;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIGLContext *gl = &s->gl[i];

        if (gl->cur.index_bits || gl->cur.index_mask != UINT32_MAX ||
            memcmp(&gl->cur_index, &one, sizeof(one)) ||
            memcmp(&gl->raster_index, &one, sizeof(one)) ||
            gl->clear_index != 0 || signbit(gl->clear_index)) {
            return true;
        }
        for (uint32_t v = 0; v < MIN(gl->nheld, SGI_GL_HELD_VERTICES); v++) {
            if (memcmp(&gl->held[v][SGI_GL_V_INDEX], &one, sizeof(one))) {
                return true;
            }
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_index = {
    .name = TYPE_SGI_MGRAS "/gl-index",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_index_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_index,
                             SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_depth_range = {
    .name = TYPE_SGI_MGRAS "/gl-context-depth-range",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_GL_FLOATS(cur.depth_range, SGIGLContext, 2, 0),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Every context's depth range, sent only once some context has left
 * OpenGL's initial [0, 1], so a guest that never sent glDepthRange
 * migrates to and from a binary without it, and one that did is refused
 * by such a binary rather than lose the range. A stream without it comes
 * from a model that ignored glDepthRange, whose every context answered
 * and drew as [0, 1]: the destination's initial ranges.
 */
static bool mgras_gl_depth_range_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const float *r = s->gl[i].cur.depth_range;

        if (r[0] != 0.0f || r[1] != 1.0f) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_depth_range = {
    .name = TYPE_SGI_MGRAS "/gl-depth-range",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_depth_range_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_depth_range,
                             SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_dither = {
    .name = TYPE_SGI_MGRAS "/gl-context-dither",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cur.dither, SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Every context's dither enable, sent only once some context has turned
 * dithering off. A stream without it comes from a model that consumed
 * 0x6704 without acting on it, for which every context kept OpenGL's
 * initial enable: the destination's.
 */
static bool mgras_gl_dither_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (!s->gl[i].cur.dither) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_dither = {
    .name = TYPE_SGI_MGRAS "/gl-dither",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_dither_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_dither,
                             SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_light = {
    .name = TYPE_SGI_MGRAS "/gl-light",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_GL_FLOATS(ambient, SGIGLLight, 4, 0),
        VMSTATE_SGI_GL_FLOATS(diffuse, SGIGLLight, 4, 0),
        VMSTATE_SGI_GL_FLOATS(specular, SGIGLLight, 4, 0),
        VMSTATE_SGI_GL_FLOATS(position, SGIGLLight, 4, 0),
        VMSTATE_SGI_GL_FLOATS(spot_direction, SGIGLLight, 3, 0),
        VMSTATE_SINGLE(spot_exponent, SGIGLLight, 0, vmstate_info_uint32,
                       float),
        VMSTATE_SINGLE(spot_cutoff, SGIGLLight, 0, vmstate_info_uint32,
                       float),
        VMSTATE_SGI_GL_FLOATS(attenuation, SGIGLLight, 3, 0),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_material = {
    .name = TYPE_SGI_MGRAS "/gl-material",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_GL_FLOATS(ambient, SGIGLMaterial, 4, 0),
        VMSTATE_SGI_GL_FLOATS(diffuse, SGIGLMaterial, 4, 0),
        VMSTATE_SGI_GL_FLOATS(specular, SGIGLMaterial, 4, 0),
        VMSTATE_SGI_GL_FLOATS(emission, SGIGLMaterial, 4, 0),
        VMSTATE_SINGLE(shininess, SGIGLMaterial, 0, vmstate_info_uint32,
                       float),
        VMSTATE_SGI_GL_FLOATS(color_indexes, SGIGLMaterial, 3, 0),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_lighting = {
    .name = TYPE_SGI_MGRAS "/gl-context-lighting",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(light, SGIGLContext, SGI_GL_LIGHTS, 1,
                             vmstate_sgi_mgras_gl_light, SGIGLLight),
        VMSTATE_STRUCT_ARRAY(material, SGIGLContext, 2, 1,
                             vmstate_sgi_mgras_gl_material, SGIGLMaterial),
        VMSTATE_SGI_GL_FLOATS(light_model_ambient, SGIGLContext, 4, 0),
        VMSTATE_UINT32(light_model_local_viewer, SGIGLContext),
        VMSTATE_UINT32(light_model_two_side, SGIGLContext),
        VMSTATE_UINT32(color_material, SGIGLContext),
        VMSTATE_UINT32(color_material_face, SGIGLContext),
        VMSTATE_UINT32(color_material_mode, SGIGLContext),
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
 * Every context's lighting state and light or material readback, sent
 * only once some context has left OpenGL's initial lighting state or
 * asked for a readback. A stream without it comes from a model that
 * consumed the lighting packets without acting on them and answered the
 * readback queries with zero: the destination's initial state gives the
 * same zeros and, for the lighting, what the client set up at context
 * creation.
 */
static bool mgras_gl_lighting_needed(void *opaque)
{
    SGIMgrasState *s = opaque;
    g_autofree SGIGLContext *init = g_new(SGIGLContext, 1);

    sgi_gl_init(init, 0);
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIGLContext *gl = &s->gl[i];
        static const uint32_t zero[4];

        if (memcmp(gl->light, init->light, sizeof(gl->light)) ||
            memcmp(gl->material, init->material, sizeof(gl->material)) ||
            memcmp(gl->light_model_ambient, init->light_model_ambient,
                   sizeof(gl->light_model_ambient)) ||
            gl->light_model_local_viewer != init->light_model_local_viewer ||
            gl->light_model_two_side != init->light_model_two_side ||
            gl->color_material != init->color_material ||
            gl->color_material_face != init->color_material_face ||
            gl->color_material_mode != init->color_material_mode ||
            memcmp(s->gl_context[i].readback, zero, sizeof(zero))) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_lighting = {
    .name = TYPE_SGI_MGRAS "/gl-lighting",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_lighting_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_lighting,
                             SGIGLContext),
        VMSTATE_STRUCT_ARRAY(gl_context, SGIMgrasState,
                             SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_readback,
                             SGIMgrasGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_texture = {
    .name = TYPE_SGI_MGRAS "/gl-context-texture",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_GL_FLOATS(texture, SGIGLContext, 16, 0),
        VMSTATE_2DARRAY(tx_stack, SGIGLContext, MGRAS_GL_WIRE_STACK, 16,
                        0, vmstate_info_uint32, float),
        VMSTATE_UINT32(tx_depth, SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Every context's texture matrix and stack, sent only once some context
 * has left the identity with an empty stack. A stream without it comes
 * from a model that dropped every texture-matrix command, whose contexts
 * all kept the identity: the destination's initial state.
 */
static bool mgras_gl_texture_needed(void *opaque)
{
    SGIMgrasState *s = opaque;
    g_autofree SGIGLContext *init = g_new(SGIGLContext, 1);

    sgi_gl_init(init, 0);
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        if (s->gl[i].tx_depth ||
            memcmp(s->gl[i].texture, init->texture, sizeof(init->texture))) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_texture = {
    .name = TYPE_SGI_MGRAS "/gl-texture",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_texture_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_texture,
                             SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_light_enables = {
    .name = TYPE_SGI_MGRAS "/gl-context-light-enables",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(lighting, SGIGLContext),
        VMSTATE_UINT32(lights_enabled, SGIGLContext),
        VMSTATE_UINT32(normalize, SGIGLContext),
        VMSTATE_UINT32(rescale_normal, SGIGLContext),
        VMSTATE_UINT32(strip_odd, SGIGLContext),
        VMSTATE_SUB_ARRAY(held[0], SGIGLContext, SGI_GL_V_BACK_COLOR, 4, 0,
                          vmstate_info_uint32, float),
        VMSTATE_SUB_ARRAY(held[1], SGIGLContext, SGI_GL_V_BACK_COLOR, 4, 0,
                          vmstate_info_uint32, float),
        VMSTATE_SUB_ARRAY(held[2], SGIGLContext, SGI_GL_V_BACK_COLOR, 4, 0,
                          vmstate_info_uint32, float),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Every context's lighting, light, normalize and rescale enables, its
 * strip's parity and its held vertices' back colours, sent only once
 * some context has one of the enables on. A stream without it comes from
 * a model that lit nothing, or from a guest that enabled none of them:
 * the destination's initial state, all off, draws the same. The parity
 * and the back colours matter only while lighting is on.
 */
static bool mgras_gl_light_enables_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIGLContext *gl = &s->gl[i];

        if (gl->lighting || gl->lights_enabled || gl->normalize ||
            gl->rescale_normal) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_light_enables = {
    .name = TYPE_SGI_MGRAS "/gl-light-enables",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_light_enables_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_light_enables,
                             SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_cull = {
    .name = TYPE_SGI_MGRAS "/gl-context-cull",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cur.cull_face, SGIGLContext),
        VMSTATE_UINT32(cur.cull_face_mode, SGIGLContext),
        VMSTATE_UINT32(cur.front_face, SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Every context's culling enable, cull face and front face, sent only
 * once some context has left OpenGL's initial disabled, GL_BACK and
 * GL_CCW. A stream without it comes from a model that consumed 0x7200,
 * 0x7300, 0x7404 and 0x5604 without acting on them, whose contexts all
 * drew as that initial state does: the destination's.
 */
static bool mgras_gl_cull_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIGLDrawState *st = &s->gl[i].cur;

        if (st->cull_face || st->cull_face_mode != SGI_GL_BACK ||
            st->front_face != SGI_GL_CCW) {
            return true;
        }
    }
    return false;
}

static const VMStateDescription vmstate_sgi_mgras_gl_context_scissor = {
    .name = TYPE_SGI_MGRAS "/gl-context-scissor",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cur.scissor_test, SGIGLContext),
        VMSTATE_INT32_ARRAY(cur.scissor, SGIGLContext, 4),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Every context's scissor enable and box, sent once some context has
 * left the initial disabled test and empty box. A stream without it
 * comes from a model that consumed 0x3910 and 0x6e04 without acting on
 * them, which drew as a disabled test does and answered zero: the
 * destination's initial state.
 */
static bool mgras_gl_scissor_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIGLDrawState *st = &s->gl[i].cur;

        if (st->scissor_test || st->scissor[0] || st->scissor[1] ||
            st->scissor[2] || st->scissor[3]) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_scissor = {
    .name = TYPE_SGI_MGRAS "/gl-scissor",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_scissor_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_scissor,
                             SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_alpha = {
    .name = TYPE_SGI_MGRAS "/gl-context-alpha",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cur.alpha_test, SGIGLContext),
        VMSTATE_UINT32(cur.alpha_func, SGIGLContext),
        VMSTATE_SINGLE(cur.alpha_ref, SGIGLContext, 0, vmstate_info_uint32,
                       float),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Every context's alpha test, sent once some context has left the
 * initial disabled test, GL_ALWAYS and reference 0. A stream without it
 * comes from a model that consumed 0x3e08 and 0x6404 and drew as that
 * initial state does.
 */
static bool mgras_gl_alpha_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIGLDrawState *st = &s->gl[i].cur;

        if (st->alpha_test || st->alpha_func != SGI_GL_ALWAYS ||
            st->alpha_ref != 0.0f) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_alpha = {
    .name = TYPE_SGI_MGRAS "/gl-alpha",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_alpha_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_alpha,
                             SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_mgras_gl_context_stencil = {
    .name = TYPE_SGI_MGRAS "/gl-context-stencil",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cur.stencil_test, SGIGLContext),
        VMSTATE_UINT32(cur.stencil_func, SGIGLContext),
        VMSTATE_INT32(cur.stencil_ref, SGIGLContext),
        VMSTATE_UINT32(cur.stencil_value_mask, SGIGLContext),
        VMSTATE_UINT32(cur.stencil_write_mask, SGIGLContext),
        VMSTATE_UINT32_ARRAY(cur.stencil_op, SGIGLContext, 3),
        VMSTATE_UINT32(clear_stencil, SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Every context's stencil state, sent once some context has left
 * OpenGL's initial state for it. A stream without it comes from a model
 * that consumed the stencil packets and drew as that initial state does,
 * with the test disabled.
 */
static bool mgras_gl_stencil_needed(void *opaque)
{
    SGIMgrasState *s = opaque;
    g_autofree SGIGLContext *init = g_new(SGIGLContext, 1);

    sgi_gl_init(init, 0);
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIGLContext *gl = &s->gl[i];

        if (gl->cur.stencil_test != init->cur.stencil_test ||
            gl->cur.stencil_func != init->cur.stencil_func ||
            gl->cur.stencil_ref != init->cur.stencil_ref ||
            gl->cur.stencil_value_mask != init->cur.stencil_value_mask ||
            gl->cur.stencil_write_mask != init->cur.stencil_write_mask ||
            memcmp(gl->cur.stencil_op, init->cur.stencil_op,
                   sizeof(init->cur.stencil_op)) ||
            gl->clear_stencil != init->clear_stencil) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_stencil = {
    .name = TYPE_SGI_MGRAS "/gl-stencil",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_stencil_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_stencil,
                             SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * The stencil buffer travels in bits 31:24 of the depth plane the stream
 * already carries. A binary without it would read those bits as depth, so
 * this empty section is sent once any pixel holds a stencil value, for
 * such a binary to refuse. Its name must not begin with another
 * subsection's: the loader takes a name that extends the one it is
 * loading as that one's own subsection.
 */
static bool mgras_gl_stencil_plane_needed(void *opaque)
{
    SGIMgrasState *s = opaque;

    for (uint32_t i = 0; i < s->depth_count; i++) {
        if (s->depth[i] & ~MGRAS_GL_DEPTH_MASK_24) {
            return true;
        }
    }
    return false;
}

const VMStateDescription vmstate_sgi_mgras_gl_stencil_plane = {
    .name = TYPE_SGI_MGRAS "/gl-plane-stencil",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_stencil_plane_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_sgi_mgras_gl_cull = {
    .name = TYPE_SGI_MGRAS "/gl-cull",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_cull_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(gl, SGIMgrasState, SGI_MGRAS_GL_CONTEXTS, 1,
                             vmstate_sgi_mgras_gl_context_cull,
                             SGIGLContext),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * The parser inside a glBitmap image, live or set aside with a context,
 * keeps its place in the payload fields the stream already carries. A
 * binary without the image's class would read its words as another
 * packet's, so this empty section is sent while one is under way, for
 * such a binary to refuse.
 */
static bool mgras_gl_block_needed(void *opaque)
{
    SGIMgrasState *s = opaque;
    bool block = s->cmd_token_class == SGI_MGRAS_CMD_TOKEN_GL_BLOCK;

    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        block |= s->gl_context[i].parser.cls == SGI_MGRAS_CMD_TOKEN_GL_BLOCK;
    }
    return block;
}

const VMStateDescription vmstate_sgi_mgras_gl_block = {
    .name = TYPE_SGI_MGRAS "/gl-block",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mgras_gl_block_needed,
    .fields = (const VMStateField[]) {
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
    .version_id = 1,
    .minimum_version_id = 1,
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

/* Whether a migrated pixel draw is one mgras_gl_pixels_start() makes. */
static bool mgras_gl_pixels_valid(const SGIMgrasGLPixels *px)
{
    if (!px->active) {
        return true;
    }
    return px->active == 1 && px->format <= MGRAS_GL_PIXELS_FORMATS &&
           px->bpp >= 1 && px->bpp <= 4 && px->w >= 1 &&
           px->w <= MGRAS_GL_PIXELS_MAX && px->h >= 1 &&
           px->h <= MGRAS_GL_PIXELS_MAX && px->row < px->h &&
           px->col <= px->w && px->ncarry < px->bpp && px->skip <= 3 &&
           px->dma_only <= 1 && px->apart <= 1 &&
           px->x >= -MGRAS_GL_PIXELS_FAR && px->x <= MGRAS_GL_PIXELS_FAR &&
           px->y >= -MGRAS_GL_PIXELS_FAR && px->y <= MGRAS_GL_PIXELS_FAR;
}

bool sgi_mgras_cmd_parser_valid(uint32_t cls, uint32_t reg,
                                uint32_t words_remaining,
                                uint32_t payload_count)
{
    if (reg >= sizeof_field(SGIMgrasState, rss) / sizeof(uint32_t) ||
        payload_count > SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS) {
        return false;
    }
    /* A block with an offset takes one word more than its length. */
    if (cls == SGI_MGRAS_CMD_TOKEN_DISCARD) {
        return words_remaining <= SGI_MGRAS_CMD_TOKEN_MAX_BLOCK_WORDS + 1;
    }
    if (cls == SGI_MGRAS_CMD_TOKEN_GL_BLOCK) {
        return (reg == MGRAS_GL_BITMAP_OP || reg == MGRAS_GL_PIXELS_OP) &&
               payload_count == MGRAS_GL_BITMAP_WORDS &&
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
        (s->gl_current && !s->gl_selected[s->gl_current])) {
        return false;
    }
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        const SGIMgrasCmdParser *p = &s->gl_context[i].parser;
        const SGIGLContext *gl = &s->gl[i];

        if (gl->nheld > SGI_GL_HELD_VERTICES ||
            gl->begin_mode > SGI_GL_POLYGON ||
            gl->mv_depth >= SGI_GL_STACK_DEPTH ||
            gl->pr_depth >= SGI_GL_STACK_DEPTH ||
            gl->tx_depth >= SGI_GL_STACK_DEPTH ||
            gl->raster_valid > 1 || gl->cur.index_bits > 32 ||
            gl->cur.dither > 1 || gl->light_model_local_viewer > 1 ||
            gl->light_model_two_side > 1 || gl->color_material > 1 ||
            gl->lighting > 1 || gl->lights_enabled >> SGI_GL_LIGHTS ||
            gl->normalize > 1 || gl->rescale_normal > 1 ||
            gl->strip_odd > 1 || gl->cur.cull_face > 1 ||
            gl->cur.scissor_test > 1 || gl->cur.alpha_test > 1 ||
            gl->cur.stencil_test > 1 ||
            gl->cur.stencil_func > SGI_GL_ALWAYS ||
            gl->cur.alpha_func > SGI_GL_ALWAYS ||
            !(gl->cur.alpha_ref >= 0.0f && gl->cur.alpha_ref <= 1.0f) ||
            (gl->cur.cull_face_mode != SGI_GL_FRONT &&
             gl->cur.cull_face_mode != SGI_GL_BACK &&
             gl->cur.cull_face_mode != SGI_GL_FRONT_AND_BACK) ||
            (gl->cur.front_face != SGI_GL_CW &&
             gl->cur.front_face != SGI_GL_CCW) ||
            !(gl->cur.depth_range[0] >= 0.0f &&
              gl->cur.depth_range[0] <= 1.0f) ||
            !(gl->cur.depth_range[1] >= 0.0f &&
              gl->cur.depth_range[1] <= 1.0f) ||
            !sgi_mgras_cmd_parser_valid(p->cls, p->reg, p->words_remaining,
                                        p->payload_count) ||
            !mgras_gl_pixels_valid(&s->gl_context[i].pixels)) {
            return false;
        }
    }
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        for (int k = 0; k < 3; k++) {
            bool known = false;

            for (int n = 0; n < ARRAY_SIZE(mgras_gl_stencil_ops); n++) {
                known |= s->gl[i].cur.stencil_op[k] == mgras_gl_stencil_ops[n];
            }
            if (!known) {
                return false;
            }
        }
    }
    /* An entry's identity is its slot, which the stream carries. */
    for (int i = 1; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        s->gl[i].hw_id = s->gl_selected[i] ? s->gl_slot[i] : 0;
    }
    /*
     * Nor does it carry drawables: the loaded context's comes from the
     * image and banks it does carry, and every other is set again when
     * that context is next loaded.
     */
    mgras_gl_bind(s);
    /*
     * It carries each spot cutoff in degrees, as glLightfv set it; the
     * lighting compares against the cosine glLightfv derived from it.
     */
    for (int i = 0; i < SGI_MGRAS_GL_CONTEXTS; i++) {
        for (int l = 0; l < SGI_GL_LIGHTS; l++) {
            SGIGLLight *light = &s->gl[i].light[l];

            light->spot_cos_cutoff = sgi_gl_spot_cos(light->spot_cutoff);
        }
    }
    return true;
}

/*
 * Store a direct-colour pixel at screen column x and row y counted from
 * the bottom, in each bank of @banks. The GL window's WID selects an XMAP
 * mode, 0x1408 for the probe's visual, whose colour map is never loaded;
 * an RGB pixel bypasses that map (sgi-mgras-display.c), which is what
 * makes the drawing visible. Bank 1 holds only direct colour in this
 * model, so it needs no format tag.
 */
static void mgras_gl_put(SGIMgrasState *s, unsigned int banks, int x, int y,
                         uint32_t rgb)
{
    unsigned int idx = (s->height - 1 - y) * s->width + x;

    if (banks & 1) {
        s->front[idx] = rgb;
        s->format[idx] = SGI_MGRAS_PIXEL_FORMAT_RGB;
    }
    if (banks & 2) {
        s->back[idx] = rgb;
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
    mgras_gl_put(opaque, target, x, y, colour);
}

/*
 * A colour index into bank 0 as a CI8 pixel, which scans out through the
 * colour map the window's display ID selects, the X server's own CI8 form
 * (mgras-raster-display.md, "Scanout"). The write mask keeps the stored
 * index's bits where it is clear. Bank 1 holds only direct colour in this
 * model, so an index for it is dropped; mgras_gl_check_index() logs that.
 * A context deeper than eight bits keeps its low eight, logged at its
 * setup.
 *
 * A mask with none of the eight bits set leaves the pixel as it was
 * (OpenGL 1.2.1, 4.2.2), whatever its format. Approximated: the model
 * tags each pixel with one format rather than keeping planes, so a
 * partial mask over a direct-colour pixel keeps that pixel's low byte
 * as the index's other bits and makes it CI8. The discriminating test is
 * gl-colour-index-zero-mask-keeps-the-pixel.
 */
static void mgras_gl_store_index(void *opaque, SGIColourTarget target,
                                 int x, int y, uint32_t index, uint32_t mask)
{
    SGIMgrasState *s = opaque;
    unsigned int idx = (s->height - 1 - y) * s->width + x;

    if ((target & 1) && (mask & 0xff)) {
        s->front[idx] = ((s->front[idx] & ~mask) | (index & mask)) & 0xff;
        s->format[idx] = SGI_MGRAS_PIXEL_FORMAT_CI8;
    }
}

/* Log colour-index work bound for bank 1, which the model cannot hold. */
static void mgras_gl_check_index(uint32_t index_bits,
                                 const SGIColourTarget *targets, uint32_t n)
{
    for (uint32_t i = 0; index_bits && i < n; i++) {
        if (targets[i] & 2) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: colour-index drawing into "
                          "pixel-memory bank 1 is not modelled; dropped\n");
            return;
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
 *   grid, with no endpoint perturbation (gl-reference/lines and
 *   gl-line-leaves-out-its-last-pixel).
 * - A primitive with a vertex at w <= 0 is dropped, not clipped
 *   (gl-triangle-with-negative-w-is-dropped).
 * - The stencil buffer holds 8 bits, as every visual with a depth buffer
 *   has (above, with the stencil packets).
 */
static const SGIRasterRules mgras_gl_raster_rules = {
    .subpixel_bits = 8,
    .color_bits = 8,
    .depth_bits = 24,
    .stencil_bits = 8,
    .rounding = SGI_ROUND_HALF_AWAY,
    .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
    .arith = SGI_ARITH_DOUBLE,
    .line_rule = SGI_LINE_COVERAGE_DOUBLE,
    .evidence = "MGRAS: 8 subpixel and 24 depth bits from the Octane "
                "Technical Report 4.2.3; the rest the model's choices, "
                "tested",
};

static const SGIRasterTarget mgras_gl_target = {
    .store_colour = mgras_gl_store_colour,
    .store_index = mgras_gl_store_index,
    .load_depth = mgras_gl_load_depth,
    .store_depth = mgras_gl_store_depth,
    .load_stencil = mgras_gl_load_stencil,
    .store_stencil = mgras_gl_store_stencil,
};

/* A GL context's draw, rasterized into the banks and the depth plane. */
static void mgras_gl_draw(void *opaque, const SGIGLDraw *d)
{
    SGIMgrasState *s = opaque;
    SGIRasterTarget t = mgras_gl_target;
    SGIColourTarget targets[SGI_GL_MAX_TARGETS];

    t.opaque = s;
    mgras_gl_check_index(d->state->index_bits, targets,
                         sgi_gl_draw_targets(d->state, targets));
    sgi_soft_draw(&mgras_gl_raster_rules, &t, d);
    s->dirty = true;
}

/* A GL context's clear or bitmap. */
static void mgras_gl_raster(void *opaque, const SGIRasterOp *op)
{
    SGIMgrasState *s = opaque;
    SGIRasterTarget t = mgras_gl_target;

    t.opaque = s;
    mgras_gl_check_index(op->kind == SGI_RASTER_BITMAP ?
                         op->bitmap.state->index_bits : op->index_bits,
                         op->targets, op->ntargets);
    sgi_soft_raster(&mgras_gl_raster_rules, &t, op);
    s->dirty = true;
}

/*
 * The client writes the opener first when it sets up a new context
 * (mgras-gl-commands.md, "Context setup opener"), and the setup that
 * follows sends no matrix, so the context must already hold the OpenGL
 * initial state then. The model restores it in the loaded context's entry
 * alone; the other contexts and the banks the image gave are the board's.
 * The discriminating tests are gl-context-setup-restores-initial-state and
 * gl-context-opener-resets-only-its-context.
 */
static void mgras_gl_context_start(SGIMgrasState *s)
{
    SGIGLContext *gl = mgras_gl(s);

    sgi_gl_init(gl, gl->hw_id);
    mgras_gl_bind(s);
}

/*
 * No MGRAS command is a glFinish: the client's glFinish is a query and a
 * status poll (mgras-gl-commands.md), which the HQ answers itself.
 */
static SGIGLSink mgras_gl_sink(SGIMgrasState *s)
{
    return (SGIGLSink) {
        .draw = mgras_gl_draw,
        .raster = mgras_gl_raster,
        .opaque = s,
    };
}

static void mgras_gl_command(SGIMgrasState *s, SGIGLOp op,
                             const uint32_t *args, unsigned nargs)
{
    SGIGLSink sink = mgras_gl_sink(s);
    SGIGLCmd cmd = { .op = op, .nargs = nargs, .args = args };

    sgi_gl_command(mgras_gl(s), &sink, &cmd);
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
 * rectwrite, which the IRIX 6.5.30 IGLOO libgl.so sends the same way.
 * The client (__glMgrSendPixels, 0xd9665c0 in IP27GR532 libGLcore.so,
 * SHA-256 626d6d94...) does not send the image's format to the board.
 * It builds a program for the GE out of routines, loads each routine's
 * parameters into GE memory with 0x80 and 0xcd packets, sends 0x8d20 and
 * eight words, and then the image: either inline, one block a row
 * (0xd9670bc-0xd96716c, through __glMgr_PushPixels), or through one
 * host-memory DMA transfer (__glMgr_WriteDMAHQSetup, kick byte
 * (slot << 1) | 0xa1). The GE runs the program over the pixels and draws
 * them.
 *
 * The loads. __glMgrInitPixelStateCRAM (0xd9bf030) and
 * __glMgrInitPixelStateERAM (0xd9bf260) collect two lists of words and
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
 * 0xe34 (__glMgrInitHostToRSS, 0xd9641b0): x0, y0, x1, y1, then
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
 * set when the format is 0x8000, 0xd966a38-0xd966ad0); without it the
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
 * nothing. 0x8d20's first word is the image's words a row: 69 for
 * ipaste's 69-pixel tiles of four bytes, 51 for imgview's 68 pixels of
 * three, 50 for mandel's 100 indices (inferred from those; it is the tile
 * descriptor's word +8 in the listing). A colour-index image's bytes a
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
 * Evidence: run irix-20260929T063522-625ddc, a trace of ipaste, imgview,
 * mandel, snapshot and i3dm on the SI. The discriminating tests are
 * gl-pixels-inline-colour-index, gl-pixels-inline-rgb, gl-pixels-dma-abgr,
 * gl-pixels-dma-rgba-and-rgb, gl-pixels-unmodelled-draw-nothing,
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

/* The rectangle and form of the draw 0x8d20 starts, or false, logged. */
static bool mgras_gl_pixels_program(SGIMgrasState *s, uint32_t row_words,
                                    SGIMgrasGLPixels *px)
{
    const uint32_t *ge = s->ge_param;
    const uint32_t *box = &ge[MGRAS_GE_PIXEL_BOX];
    uint32_t hq = s->dma_desc_30000[0];
    /* The guest's words, so the differences are taken in 64 bits. */
    int64_t w = (int64_t)(int32_t)box[2] - (int32_t)box[0] + 1;
    int64_t h = (int64_t)(int32_t)box[3] - (int32_t)box[1] + 1;
    uint32_t pairs = 0, format;

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
    if (w <= 0 || h <= 0 || w > MGRAS_GL_PIXELS_MAX ||
        h > MGRAS_GL_PIXELS_MAX || (uint32_t)w != (box[4] & 0xffff) ||
        (uint32_t)h != box[4] >> 16) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: pixel rectangle %d,%d to %d,%d "
                      "(size word 0x%x) is not modelled\n", box[0], box[1],
                      box[2], box[3], box[4]);
        return false;
    }
    px->h = h;
    while (pairs < MGRAS_GE_PIXEL_PROGRAM_PAIRS &&
           ge[MGRAS_GE_PIXEL_PROGRAM + 2 * pairs] != MGRAS_GE_ROUTINE_END) {
        pairs++;
    }
    pairs++;
    for (format = 0; format < ARRAY_SIZE(mgras_gl_pixel_programs); format++) {
        bool same = mgras_gl_pixel_programs[format].pairs == pairs;

        for (uint32_t i = 0; same && i < pairs; i++) {
            same = ge[MGRAS_GE_PIXEL_PROGRAM + 2 * i] ==
                   mgras_gl_pixel_programs[format].routine[i];
        }
        if (same && mgras_gl_pixel_programs[format].unpack) {
            uint32_t params = ge[MGRAS_GE_PIXEL_PROGRAM + 3];

            same = params < SGI_MGRAS_GE_PARAM_WORDS - 1 &&
                   ge[params + 1] == mgras_gl_pixel_programs[format].unpack;
        }
        if (same) {
            break;
        }
    }
    if (format >= ARRAY_SIZE(mgras_gl_pixel_programs)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: GE pixel program of %u "
                      "routines from 0x%x, 0x%x is not modelled; its image "
                      "is not drawn\n", pairs, ge[MGRAS_GE_PIXEL_PROGRAM],
                      ge[MGRAS_GE_PIXEL_PROGRAM + 2]);
        return false;
    }
    if (ge[MGRAS_GE_PIXEL_ZOOM] || ge[MGRAS_GE_PIXEL_ZOOM + 1] !=
        MGRAS_GL_FLOAT_ONE) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: pixel zoom (GE words 0x%x "
                      "0x%x) is not modelled; the image is not drawn\n",
                      ge[MGRAS_GE_PIXEL_ZOOM], ge[MGRAS_GE_PIXEL_ZOOM + 1]);
        return false;
    }
    px->format = format;
    px->x = box[0];
    px->y = box[1];
    px->w = w;
    px->bpp = 0;
    switch (format) {
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
                                  "%u wide in %u words a row could be %u or "
                                  "%u bytes an index; not drawn\n", px->w,
                                  row_words, px->bpp, bpp);
                    return false;
                }
                px->bpp = bpp;
            }
        }
        px->bpp = px->bpp ? px->bpp : 8;
        break;
    }
    if (px->bpp > 4 || DIV_ROUND_UP(px->w * px->bpp, 4) != row_words) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: pixel draw of %u words a row "
                      "for %u pixels is not modelled\n", row_words, px->w);
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
 * its DMA path (0xd966ca0-0xd966cbc, under two flags of the context), so
 * a draw with it set takes no inline rows; one without it takes either.
 */
#define MGRAS_GL_PIXELS_BY_DMA   0x80

static void mgras_gl_pixels_start(SGIMgrasState *s, const uint32_t *payload)
{
    SGIMgrasGLPixels *px = mgras_gl_pixels(s);
    SGIGLContext *c = mgras_gl(s);
    SGIColourTarget targets[SGI_GL_MAX_TARGETS];

    if (px->active) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: pixel draw ended after "
                      "%u of %u rows by the next\n", px->row, px->h);
    }
    if (!mgras_gl_pixels_program(s, payload[0], px)) {
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
    mgras_gl_check_index(c->cur.index_bits, targets,
                         sgi_gl_draw_targets(&c->cur, targets));
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
static void mgras_gl_pixels_put(SGIMgrasState *s, int32_t x, int32_t y,
                                const float rgba[4], float index)
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
    SGIColourTarget targets[SGI_GL_MAX_TARGETS];
    uint32_t n = sgi_gl_draw_targets(&c->cur, targets);

    memcpy(op.bitmap.rgba, rgba, sizeof(op.bitmap.rgba));
    for (uint32_t i = 0; i < n; i++) {
        /* An index for bank 1 is dropped, logged once at the draw's start. */
        if (!c->cur.index_bits || !(targets[i] & 2)) {
            op.targets[op.ntargets++] = targets[i];
        }
    }
    op.clip = c->cur.drawable.clip;
    sink.raster(sink.opaque, &op);
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
    mgras_gl_pixels_put(s, px->x + px->col, px->y + px->row, rgba, v);
}

/* Take n image bytes of the current row; bytes past its last pixel drop. */
static void mgras_gl_pixels_bytes(SGIMgrasState *s, const uint8_t *bytes,
                                  uint32_t n)
{
    SGIMgrasGLPixels *px = mgras_gl_pixels(s);

    for (uint32_t i = 0; i < n && px->col < px->w; i++) {
        px->carry = px->carry << 8 | bytes[i];
        if (++px->ncarry == px->bpp) {
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
    px->col = px->carry = px->ncarry = px->skip = 0;
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
    /*
     * The row's length counts the offset's word (0xd967118-0xd967140):
     * the words sent, not the bytes after the offset.
     */
    s->cmd_token_class = SGI_MGRAS_CMD_TOKEN_GL_BLOCK;
    s->cmd_token_reg = MGRAS_GL_PIXELS_OP;
    s->cmd_token_payload_count = MGRAS_GL_PIXELS_WORDS;
    s->cmd_token_words_remaining = DIV_ROUND_UP(bytes, sizeof(uint32_t));
    px->skip = offset;
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

bool sgi_mgras_gl_pixels_dma(SGIMgrasState *s, uint32_t rows, uint32_t bpr,
                             bool *take)
{
    SGIMgrasGLPixels *px = mgras_gl_pixels(s);

    *take = false;
    if (!px->active) {
        return false;
    }
    if (px->format == MGRAS_GL_PIXELS_FORMATS) {
        memset(px, 0, sizeof(*px));
        return true;
    }
    if (px->row || rows != px->h || bpr != px->w * px->bpp) {
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
}

void sgi_mgras_gl_pixels_row(SGIMgrasState *s, const uint8_t *bytes,
                             uint32_t n)
{
    if (mgras_gl_pixels(s)->active) {
        mgras_gl_pixels_bytes(s, bytes, n);
        mgras_gl_pixels_row_end(s);
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
    if (mgras_gl_pixels_block(s, header)) {
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
    if (mgras_gl_lighting(s, header, payload, words)) {
        return;
    }
    switch (header) {
    case MGRAS_GL_CONTEXT_OPENER:
        mgras_gl_context_start(s);
        return;
    case MGRAS_GL_VIEWPORT:
        /* The client has already clamped the size (__glMgrim_Viewport). */
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
         * The word is already scaled to the depth plane's 24 bits
         * (mgras-gl-commands.md), the form the shared context keeps.
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
         * (run irix-20260927T172059-fdbada).
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
    case MGRAS_GL_COLOR3F:
        mgras_gl_command(s, SGI_GL_OP_COLOR3F, payload, 3);
        return;
    case MGRAS_GL_COLOR4F:
        mgras_gl_command(s, SGI_GL_OP_COLOR4F, payload, 4);
        return;
    case MGRAS_GL_COLOR3UB:
        mgras_gl_color_bytes(word, payload[0]);
        mgras_gl_command(s, SGI_GL_OP_COLOR3UB, word, 3);
        return;
    case MGRAS_GL_COLOR4UB:
        mgras_gl_color_bytes(word, payload[0]);
        mgras_gl_command(s, SGI_GL_OP_COLOR4UB, word, 4);
        return;
    case MGRAS_GL_NORMAL3F:
        mgras_gl_command(s, SGI_GL_OP_NORMAL3F, payload, 3);
        return;
    case MGRAS_GL_NORMAL3D:
    case MGRAS_GL_NORMAL3I:
    case MGRAS_GL_NORMAL3S:
    case MGRAS_GL_NORMAL3B:
        mgras_gl_normal(s, header, payload);
        return;
    case MGRAS_GL_EDGE_FLAG_TRUE:
    case MGRAS_GL_EDGE_FLAG_FALSE:
        /* Kept for the 0xa104 query; polygon edge modes are not modelled. */
        word[0] = header == MGRAS_GL_EDGE_FLAG_TRUE;
        mgras_gl_command(s, SGI_GL_OP_EDGE_FLAG, word, 1);
        return;
    case MGRAS_GL_VERTEX2F:
        mgras_gl_command(s, SGI_GL_OP_VERTEX2F, payload, 2);
        return;
    case MGRAS_GL_VERTEX3F:
        mgras_gl_command(s, SGI_GL_OP_VERTEX3F, payload, 3);
        return;
    case MGRAS_GL_VERTEX4F:
        mgras_gl_command(s, SGI_GL_OP_VERTEX4F, payload, 4);
        return;
    case MGRAS_GL_VERTEX2I:
        mgras_gl_command(s, SGI_GL_OP_VERTEX2I, payload, 2);
        return;
    case MGRAS_GL_RASTER_POS2F:
        mgras_gl_command(s, SGI_GL_OP_RASTER_POS2F, payload, 2);
        return;
    case MGRAS_GL_RASTER_POS3F:
        mgras_gl_command(s, SGI_GL_OP_RASTER_POS3F, payload, 3);
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
        if (payload[0] > 8) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: colour-index context of "
                          "%u bits is stored as its low 8 bits\n",
                          payload[0]);
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
        return;
    default:
        g_assert_not_reached();
    }
}
