/* SPDX-License-Identifier: BSD-3-Clause */
/* InfiniteReality command-port roles and selector tables. */
#ifndef HW_MISC_SGI_KONA_PORTS_H
#define HW_MISC_SGI_KONA_PORTS_H

#include "hw/misc/sgi-command-port.h"

/*
 * Data only: sgi-kona.c initializes a generic command-port framer for each
 * role. The model maps the textport at +0xc10000 with mailbox and interrupt
 * cause registers. FIFO and doorbell MMIO placement remains unknown.
 *
 * The kernel FIFO push port has candidate offsets +0x7c and +0x80007c. The
 * low-window +0x7c write is modeled separately. Textport writes begin with
 * sync word 1, followed by a command and its arguments; the owner strips the
 * sync word before framing the command.
 *
 * The userspace doorbell is modeled at guest VA 0x4000. Candidate device
 * windows include +0x80000, +0x810000, and +0x10000. The +0x10000 candidate
 * is unresolved.
 *
 * The following selectors are omitted because their count or word order is
 * unresolved. Omission does not imply that real hardware ignores them:
 * - FIFO 0x1fd places its selector after a literal and another argument.
 * - FIFO 0x1f3 and 0x172 have conflicting argument counts across uses.
 * - FIFO 0x100 also appears on an indirect register pair.
 * - FIFO 0x1eb has no established argument count.
 * - FIFO 0x1ee, 0x1f9, and 0x1f2 have unknown word order.
 * - Doorbell 0x0, 0x500, 0x1a-0x1f, 0x8c, 0x90, and 0x39 have unknown
 *   counts.
 * - Doorbell 0x119 has inconsistent argument evidence; 0x1d6 and 0x1ca
 *   each have one word in the maintained table.
 * - Textport 0xa is a variable-length bitmap transfer and is framed by its
 *   owner outside the fixed-count command-port path.
 */
typedef enum SGIKonaPort {
    SGI_KONA_PORT_FIFO,
    SGI_KONA_PORT_TEXTPORT,
    SGI_KONA_PORT_DOORBELL,
} SGIKonaPort;

/* Fixed-count, selector-first kernel FIFO entries at +0x7c. */
extern const SGICommandPortSelector sgi_kona_fifo_selectors[];
extern const unsigned sgi_kona_fifo_num_selectors;

/* Textport +0xc10000 selectors follow sync word 1, stripped by the owner. */
extern const SGICommandPortSelector sgi_kona_textport_selectors[];
extern const unsigned sgi_kona_textport_num_selectors;

/* OpenGL 3D doorbell selectors at guest VA 0x4000. */
extern const SGICommandPortSelector sgi_kona_doorbell_selectors[];
extern const unsigned sgi_kona_doorbell_num_selectors;

/*
 * The command page, +0xc10000: the tokens the kernel driver, gfxinit, the
 * X server and the GL client send there, and the word count after each,
 * as the kernel's bring-up and the user-space open sequence send them.
 * Every command here is selector-first. sgi-kona.c frames them on the
 * same page as the PROM textport, whose own commands follow a sync word
 * and are keyed by sgi_kona_textport_selectors[] instead, and as the GL
 * client's commands, keyed by sgi_kona_gl_tokens[] below.
 */
#define SGI_KONA_CMD_REASON_PAGE    0x1f1
#define SGI_KONA_CMD_RETURN_PAGE    0x1f2
#define SGI_KONA_CMD_CONTEXT_MAP    0x1fd
#define SGI_KONA_CMD_POST_REASON    0x1f8
#define SGI_KONA_CMD_CHECK_PIPE     0x1dd
#define SGI_KONA_CMD_GET_CONFIG     0x1e9
#define SGI_KONA_CMD_DISPLAY_MODE   0x1e6
/*
 * The flash EEPROM operation, from the EEPROM programming tool: the operation
 * number, then that operation's words, framed by sgi-kona.c (see
 * SGI_KONA_FLASH_BASE).
 */
#define SGI_KONA_CMD_EEPROM         0x1ec
/*
 * The context deactivate the kernel driver's context swap sends when it
 * switches away from a context: the token, then the outgoing context's
 * index in the kernel's rendering-context table.
 */
#define SGI_KONA_CMD_CONTEXT_DEACTIVATE 0x1fc
/*
 * The kernel's pipe-memory pool packets: a region's placement, and the
 * copy-down and swap-out that pool pressure sends. 0x172 follows every
 * allocation and free; what it does inside the pipe is unknown.
 */
#define SGI_KONA_CMD_MEM_REGION     0x1e4
#define SGI_KONA_CMD_MEM_COPY_DOWN  0x1e1
#define SGI_KONA_CMD_MEM_SWAP_OUT   0x1e3
#define SGI_KONA_CMD_MEM_SWAP_IN    0x1e2
#define SGI_KONA_CMD_MEM_SUSPEND    0x1df
#define SGI_KONA_CMD_MEM_RESUME     0x1de
/*
 * The words the model's suspend reply returns, which the kernel's resume
 * then carries after 0x1de (see sgi_kona_mem_swap_out()'s comment).
 */
#define SGI_KONA_SUSPEND_REPLY_WORDS 0
/* The completion reasons of a swap and of a suspend. */
#define SGI_KONA_REASON_SWAP        0x102
#define SGI_KONA_REASON_SUSPEND     0x104
#define SGI_KONA_CMD_RESOURCE_SYNC  0x172
/*
 * The rest of a context swap. The kernel driver's context swap restores a
 * context that has run before with 0x1f9: the first word of its save area,
 * the save area's page word and tagged offset word, and the sentinel
 * 0xedededed. The driver's context activation and clip validation send
 * 0x1f3 with eleven words and 0x1ee with three when the context's state is
 * stale, and 0x1f4 with two signed halfwords, as its swap scheduling also
 * does. Its DMA pool update sends 0x1ed with a pool's page word, tagged
 * offset word and length. What 0x1f3, 0x1ee and 0x1f4 carry is unknown.
 */
#define SGI_KONA_CMD_CONTEXT_RESTORE 0x1f9
#define SGI_KONA_CMD_CONTEXT_RECORDS 0x1f3
#define SGI_KONA_CMD_CONTEXT_WORDS  0x1ee
#define SGI_KONA_CMD_SWAP_PAIR      0x1f4
#define SGI_KONA_CMD_DMA_POOL       0x1ed
/*
 * The kernel driver's DMA packet, 0x1eb and six words, and its list-write
 * record, 0x1c8 with a pipe-memory address and a byte count.
 * sgi-kona.c hands both to the DMA engine.
 */
#define SGI_KONA_CMD_DMA_PACKET     0x1eb
#define SGI_KONA_CMD_DMA_LIST_WRITE 0x1c8
/* Read descriptor (six words), then finish reason and context index. */
#define SGI_KONA_CMD_SELECT_FEED_START  0x1f5
#define SGI_KONA_CMD_SELECT_FEED_FINISH 0x1f6
/*
 * The GL client's display-list packets, which it writes to its doorbell:
 * invoke a list from pipe memory (0x16d, a byte offset and a word count;
 * 0x170, a count n of 1 to 8, then n such pairs) or from the pool 0x1ed
 * registered (0x16e and 0x171, pool byte offsets, n of 1 to 4), pull a block
 * from the pool into pipe memory (0x167: destination, pool byte offset, byte
 * count, 0) and move one within pipe memory (0x16f: destination, source,
 * length). The command page frames 0x170's and 0x171's count only;
 * sgi-kona.c hands the pairs after it to the DMA engine, which runs them
 * all.
 */
#define SGI_KONA_CMD_LIST_CALL        0x16d
#define SGI_KONA_CMD_LIST_CALL_POOL   0x16e
#define SGI_KONA_CMD_LIST_CALLS       0x170
#define SGI_KONA_CMD_LIST_CALLS_POOL  0x171
#define SGI_KONA_CMD_LIST_PULL        0x167
#define SGI_KONA_CMD_LIST_MOVE        0x16f
#define SGI_KONA_LIST_CALLS_MAX       8
#define SGI_KONA_LIST_CALLS_POOL_MAX  4
/*
 * The GL client's commands, which the GL client library writes to its doorbell
 * and which reach the command page with no +0xc00108 bracket. They are framed
 * by sgi_kona_gl_tokens[] on a port of their own (see sgi_kona_textport_push()
 * in sgi-kona.c). Each count is the words after the selector word as the
 * senders write them, a double counting two. The descriptions paraphrase the
 * pipe's command names.
 *
 * The make-current state: active texture takes a unit; post-texture scale
 * and bias a unit and three words of the texture object; the texture
 * table a slot word, then SGI_KONA_GL_TEX_TABLE_WORDS data words; the
 * line-mode distance query one 0, answered on the return page; lazy mode
 * one 0; viewport and scissor x, y, width and height; set error a GL
 * error code.
 */
#define SGI_KONA_CMD_GL_ACTIVE_TEXTURE 0x177
#define SGI_KONA_CMD_GL_TEX_SCALE_BIAS 0x1d7
#define SGI_KONA_CMD_GL_TEX_TABLE   0x17e
#define SGI_KONA_CMD_GL_LINE_DISTANCE 0xee
#define SGI_KONA_CMD_GL_LAZY_MODE   0x1ce
/*
 * glRenderMode: 0x19 and the mode, GL_RENDER 0x1c00, GL_FEEDBACK 0x1c01
 * or GL_SELECT 0x1c02, sent for every valid mode before the library acts
 * on it (established from the IRIX client library's sender).
 */
#define SGI_KONA_CMD_GL_RENDER_MODE 0x19
/*
 * The name stack, the feedback type and pass-through, one word each
 * after the selector (established from the IRIX client library's
 * senders): glInitNames 0x10c and 0, glLoadName 0x10d and the name,
 * glPopName 0xc2 and 0, glPushName 0xc3 and the name, glFeedbackBuffer
 * 0x17 and the type (GL_2D 0x600 to GL_4D_COLOR_TEXTURE 0x604; the size
 * and the buffer stay in the library) and glPassThrough 0xc1 and its
 * value as a binary32 word.
 */
#define SGI_KONA_CMD_GL_INIT_NAMES  0x10c
#define SGI_KONA_CMD_GL_LOAD_NAME   0x10d
#define SGI_KONA_CMD_GL_POP_NAME    0xc2
#define SGI_KONA_CMD_GL_PUSH_NAME   0xc3
#define SGI_KONA_CMD_GL_FEEDBACK_TYPE 0x17
#define SGI_KONA_CMD_GL_PASS_THROUGH 0xc1
#define SGI_KONA_CMD_GL_VIEWPORT    0xe3
#define SGI_KONA_CMD_GL_SCISSOR     0xc
#define SGI_KONA_CMD_GL_SET_ERROR   0xe4
/*
 * The drawing the minimal client sends: disable a capability (1), clear
 * colour (four floats), clear (a buffer mask), shade model (1),
 * three-float colour (3), the ten primitive begins, 0x57 plus the GL mode
 * 0 to 9, each with one 0, the two-float vertex (2), end (one 0), flush
 * (one 0), finish (1 for glFinish, 0 from a buffer swap) and get error
 * (one word the pipe ignores, 0xdbdbdbdb at the sender), answered on the
 * return page.
 */
#define SGI_KONA_CMD_GL_DISABLE     0x25
#define SGI_KONA_CMD_GL_CLEAR_COLOR 0x1d
#define SGI_KONA_CMD_GL_CLEAR       0x1a
#define SGI_KONA_CMD_GL_SHADE_MODEL 0xc0
#define SGI_KONA_CMD_GL_COLOR3      0x63
#define SGI_KONA_CMD_GL_BEGIN_FIRST 0x57
#define SGI_KONA_CMD_GL_BEGIN_LAST  0x60
#define SGI_KONA_CMD_GL_VERTEX2     0x8c
#define SGI_KONA_CMD_GL_END         0x71
#define SGI_KONA_CMD_GL_FLUSH       0xc7
#define SGI_KONA_CMD_GL_FINISH      0xc6
#define SGI_KONA_CMD_GL_GET_ERROR   0x3e
/*
 * The current-attribute and raster-position queries, each with one 0,
 * answered on the return page: glGet of GL_CURRENT_COLOR to
 * GL_CURRENT_TEXTURE_COORDS sends 0x3b, of GL_CURRENT_RASTER_COLOR to
 * GL_CURRENT_RASTER_DISTANCE 0x3d.
 */
#define SGI_KONA_CMD_GL_GET_CURRENT 0x3b
#define SGI_KONA_CMD_GL_GET_RASTER  0x3d
/*
 * The matrix commands: mode (the enumerant), load identity, push and pop
 * (each one 0), load and multiply with sixteen floats or sixteen doubles
 * in the caller's column-major order, frustum and ortho with six doubles,
 * rotate with four floats or doubles, scale and translate with three, and
 * the depth range with two doubles.
 */
#define SGI_KONA_CMD_GL_DEPTH_RANGE 0xcf
#define SGI_KONA_CMD_GL_MATRIX_MODE 0xd0
#define SGI_KONA_CMD_GL_FRUSTUM     0xd1
#define SGI_KONA_CMD_GL_LOAD_IDENTITY 0xd2
#define SGI_KONA_CMD_GL_LOAD_MATRIXF 0xd3
#define SGI_KONA_CMD_GL_LOAD_MATRIXD 0xd4
#define SGI_KONA_CMD_GL_MULT_MATRIXF 0xd5
#define SGI_KONA_CMD_GL_MULT_MATRIXD 0xd6
#define SGI_KONA_CMD_GL_ORTHO       0xd7
#define SGI_KONA_CMD_GL_POP_MATRIX  0xd8
#define SGI_KONA_CMD_GL_PUSH_MATRIX 0xd9
#define SGI_KONA_CMD_GL_ROTATED     0xda
#define SGI_KONA_CMD_GL_ROTATEF     0xdb
#define SGI_KONA_CMD_GL_SCALED      0xdc
#define SGI_KONA_CMD_GL_SCALEF      0xdd
#define SGI_KONA_CMD_GL_TRANSLATED  0xde
#define SGI_KONA_CMD_GL_TRANSLATEF  0xdf
/*
 * Vertex attributes. The byte and short forms carry data in the top half
 * of the selector word itself, so a selector is its word's low 16 bits:
 * glColor3ub is r << 24 | g << 16 | 0x66 and then b << 24. The counts here
 * are the words after that first word.
 */
#define SGI_KONA_CMD_GL_COLOR3B     0x61
#define SGI_KONA_CMD_GL_COLOR3D     0x62
#define SGI_KONA_CMD_GL_COLOR3I     0x64
#define SGI_KONA_CMD_GL_COLOR3UB    0x66
#define SGI_KONA_CMD_GL_COLOR3UI    0x67
#define SGI_KONA_CMD_GL_COLOR4D     0x6a
#define SGI_KONA_CMD_GL_COLOR4F     0x6b
#define SGI_KONA_CMD_GL_COLOR4I     0x6c
#define SGI_KONA_CMD_GL_COLOR4S     0x6d
#define SGI_KONA_CMD_GL_COLOR4UB    0x6e
#define SGI_KONA_CMD_GL_COLOR4UI    0x6f
#define SGI_KONA_CMD_GL_NORMAL3B    0x76
#define SGI_KONA_CMD_GL_NORMAL3D    0x77
#define SGI_KONA_CMD_GL_NORMAL3F    0x78
#define SGI_KONA_CMD_GL_NORMAL3I    0x79
#define SGI_KONA_CMD_GL_NORMAL3S    0x7a
#define SGI_KONA_CMD_GL_TEXCOORD1F  0x7c
#define SGI_KONA_CMD_GL_TEXCOORD2F  0x80
#define SGI_KONA_CMD_GL_TEXCOORD2S  0x82
#define SGI_KONA_CMD_GL_TEXCOORD3F  0x84
#define SGI_KONA_CMD_GL_TEXCOORD4F  0x88
#define SGI_KONA_CMD_GL_VERTEX2D    0x8b
#define SGI_KONA_CMD_GL_VERTEX2I    0x8d
#define SGI_KONA_CMD_GL_VERTEX2S    0x8e
#define SGI_KONA_CMD_GL_VERTEX3D    0x8f
#define SGI_KONA_CMD_GL_VERTEX3F    0x90
#define SGI_KONA_CMD_GL_VERTEX3I    0x91
#define SGI_KONA_CMD_GL_VERTEX3S    0x92
#define SGI_KONA_CMD_GL_VERTEX4D    0x93
#define SGI_KONA_CMD_GL_VERTEX4F    0x94
#define SGI_KONA_CMD_GL_VERTEX4I    0x95
#define SGI_KONA_CMD_GL_VERTEX4S    0x96
#define SGI_KONA_CMD_GL_EDGE_FLAG   0xa7
#define SGI_KONA_CMD_GL_RASTER_POS2F 0xa9
#define SGI_KONA_CMD_GL_RASTER_POS3F 0xad
#define SGI_KONA_CMD_GL_RASTER_POS4F 0xb1
/*
 * The IRIX client library's raster state: fourteen of the context's floats
 * and one integer field converted to a float.
 */
#define SGI_KONA_CMD_GL_RASTER_STATE 0x10e
/*
 * The IRIX client library's evaluator attribute update: four words, then
 * 0x29 with eight.
 */
#define SGI_KONA_CMD_GL_EVAL_ATTRIB  0x28
#define SGI_KONA_CMD_GL_EVAL_ATTRIB2 0x29
/*
 * The tail padding of a display-list block, inferred to fill four, two
 * and five words: 0x1d9 and three words, 0x1d8 and one, 0x1da and four.
 */
#define SGI_KONA_CMD_GL_LIST_PAD1    0x1d8
#define SGI_KONA_CMD_GL_LIST_PAD3    0x1d9
#define SGI_KONA_CMD_GL_LIST_PAD4    0x1da
/*
 * glPointParameterfSGIS and the fv form: 0x1a7 with the pname and one
 * value, or 0x1a8 with the distance-attenuation pname and three values.
 */
#define SGI_KONA_CMD_GL_POINT_PARAMETER   0x1a7
#define SGI_KONA_CMD_GL_POINT_ATTENUATION 0x1a8
/*
 * glSpriteParameter*SGIX: 0x1a4 with the mode pname and the mode, or 0x1a5 with
 * a pname and three words, the axis's or translation's floats, or the mode
 * three times as an integer.
 */
#define SGI_KONA_CMD_GL_SPRITE_MODE       0x1a4
#define SGI_KONA_CMD_GL_SPRITE_VECTOR     0x1a5
/*
 * glSampleMaskSGIS: 0x118, the value clamped
 * to zero through one, and the invert flag.
 */
#define SGI_KONA_CMD_GL_SAMPLE_MASK       0x118
/*
 * glSamplePatternSGIS: 0x117 and the pattern's enumerant, GL_1PASS_SGIS
 * to GL_4PASS_3_SGIS.
 */
#define SGI_KONA_CMD_GL_SAMPLE_PATTERN    0x117
#define SGI_KONA_GL_1PASS_SGIS            0x80a1
#define SGI_KONA_GL_4PASS_3_SGIS          0x80a6
/*
 * glDrawArrays (from the IRIX client library): 0x180 and the enabled
 * arrays' state word when it changed; then inside glBegin and glEnd,
 * 0x181 0, a short run, a bulk run and 0x1a3 0. A short run's token is
 * 0x1a3 - e * r (e = 1, 2 or 4 enabled arrays) or 0x1a1 - e * r (e = 3,
 * 5 or 6) for its r vertices, and has no words of its own. A bulk run
 * is 0x182, its vertex count and two 0xdeadbeef words. The vertices
 * follow either run with no selector.
 */
#define SGI_KONA_CMD_GL_ARRAY_STATE       0x180
#define SGI_KONA_CMD_GL_ARRAY_BEGIN       0x181
#define SGI_KONA_CMD_GL_ARRAY_BULK        0x182
#define SGI_KONA_CMD_GL_ARRAY_SHORT_FIRST 0x184
#define SGI_KONA_CMD_GL_ARRAY_SHORT_LAST  0x1a2
#define SGI_KONA_CMD_GL_ARRAY_END         0x1a3
/*
 * glRect: a count of the words up to and including the end that follows
 * the four vertices, which arrive as vertex commands.
 */
#define SGI_KONA_CMD_GL_RECT        0x3
/*
 * Lighting, as the caller's enumerants and values: a light, its parameter
 * and four floats or integers; the light model's parameter and four
 * values; a material's face, parameter and four floats; colour material's
 * face and mode. A single-value form pads to the same length with zeros.
 */
#define SGI_KONA_CMD_GL_LIGHTF      0x7
#define SGI_KONA_CMD_GL_LIGHTI      0x8
#define SGI_KONA_CMD_GL_LIGHT_MODELF 0x9
#define SGI_KONA_CMD_GL_LIGHT_MODELI 0xa
#define SGI_KONA_CMD_GL_MATERIAL    0x97
#define SGI_KONA_CMD_GL_COLOR_MATERIAL 0xb5
/* Raster state, each the GL call's arguments in order. */
#define SGI_KONA_CMD_GL_POLYGON_STIPPLE 0xb
#define SGI_KONA_CMD_GL_TEX_ENVF    0x11
#define SGI_KONA_CMD_GL_TEX_ENVI    0x12
#define SGI_KONA_CMD_GL_CLEAR_ACCUM 0x1b
/*
 * glAccum: 0xc5, the operation's enumerant and the value as a float, with
 * no check by the client library; glClearAccum is 0x1b and four floats
 * the library clamps to [-1, 1].
 */
#define SGI_KONA_CMD_GL_ACCUM       0xc5
#define SGI_KONA_CMD_GL_CLEAR_INDEX 0x1c
#define SGI_KONA_CMD_GL_CLEAR_STENCIL 0x1e
#define SGI_KONA_CMD_GL_CLEAR_DEPTH 0x1f
#define SGI_KONA_CMD_GL_STENCIL_MASK 0x20
#define SGI_KONA_CMD_GL_COLOR_MASK  0x21
#define SGI_KONA_CMD_GL_DEPTH_MASK  0x22
#define SGI_KONA_CMD_GL_INDEX_MASK  0x23
#define SGI_KONA_CMD_GL_INDEXD      0x72
#define SGI_KONA_CMD_GL_INDEXF      0x73
#define SGI_KONA_CMD_GL_INDEXI      0x74
#define SGI_KONA_CMD_GL_INDEX_PACKED 0x75
#define SGI_KONA_CMD_GL_ENABLE      0x24
#define SGI_KONA_CMD_GL_LOGIC_OP    0x2e
#define SGI_KONA_CMD_GL_BITMAP_INLINE 0x5
#define SGI_KONA_CMD_GL_BITMAP 0x6
#define SGI_KONA_CMD_GL_PIXEL_ZOOM 0x2f
#define SGI_KONA_CMD_GL_PIXEL_TRANSFER 0x30
#define SGI_KONA_CMD_GL_PIXEL_STORE 0x33
#define SGI_KONA_CMD_GL_PIXEL_MAP_F 0x34
#define SGI_KONA_CMD_GL_PIXEL_MAP_UI 0x35
#define SGI_KONA_CMD_GL_PIXEL_MAP_US 0x36
#define SGI_KONA_CMD_GL_READ_BUFFER 0x37
#define SGI_KONA_CMD_GL_CLIP_PLANE  0xb4
#define SGI_KONA_CMD_GL_CULL_FACE   0xb6
#define SGI_KONA_CMD_GL_FOGF        0xb7
#define SGI_KONA_CMD_GL_FOGI        0xb8
#define SGI_KONA_CMD_GL_FRONT_FACE  0xba
#define SGI_KONA_CMD_GL_HINT        0xbb
#define SGI_KONA_CMD_GL_LINE_STIPPLE 0xbc
#define SGI_KONA_CMD_GL_LINE_WIDTH  0xbd
#define SGI_KONA_CMD_GL_POINT_SIZE  0xbe
#define SGI_KONA_CMD_GL_POLYGON_MODE 0xbf
#define SGI_KONA_CMD_GL_DRAW_BUFFER 0xc4
#define SGI_KONA_CMD_GL_ALPHA_FUNC  0xca
#define SGI_KONA_CMD_GL_BLEND_FUNC  0xcb
#define SGI_KONA_CMD_GL_STENCIL_FUNC 0xcc
#define SGI_KONA_CMD_GL_STENCIL_OP  0xcd
#define SGI_KONA_CMD_GL_DEPTH_FUNC  0xce
#define SGI_KONA_CMD_GL_BLEND_COLOR 0xe6
#define SGI_KONA_CMD_GL_BLEND_EQUATION 0xe7
#define SGI_KONA_CMD_GL_OFFSET_EXT  0x11b
#define SGI_KONA_CMD_GL_POLYGON_OFFSET 0x168
#define SGI_KONA_CMD_GL_FLUSH_RASTER 0x1cc
#define SGI_KONA_CMD_GL_RESOLVE_A   0x1ca
#define SGI_KONA_CMD_GL_RESOLVE_B   0x1d6
/*
 * Pixels and the round trips: glDrawPixels' eight header words, whose
 * rows follow; glReadPixels' seven words, answered on the return page; a
 * glGet's name, answered there; glPushAttrib's mask, answered there from
 * byte 4; glPopAttrib's 0, and before it, when lighting was pushed, the
 * pop-lights command with a count n and n words.
 */
#define SGI_KONA_CMD_GL_DRAW_PIXELS 0x3a
/*
 * The EXT_convolution commands (established from the IRIX client library;
 * the layouts are in sgi-kona.c, before sgi_kona_gl_filter_header()).
 * The filters, each sent between 0x111 0x10 0x40000000 and 0x111 0x10 0:
 * 0xef 1D, 0xf0 2D and 0xf5 separable, twice, each followed by its
 * image's words unless the image comes by DMA; 0xf2 and 0xf3 copy a 1D
 * or 2D filter from the framebuffer. 0xf4 and 0xf7 read a filter back on
 * the return page; 0xf8 and 0xf9 set a filter's integer and four-float
 * parameters.
 */
#define SGI_KONA_CMD_GL_CONVOLUTION_1D       0xef
#define SGI_KONA_CMD_GL_CONVOLUTION_2D       0xf0
#define SGI_KONA_CMD_GL_COPY_CONVOLUTION_1D  0xf2
#define SGI_KONA_CMD_GL_COPY_CONVOLUTION_2D  0xf3
#define SGI_KONA_CMD_GL_GET_CONVOLUTION      0xf4
#define SGI_KONA_CMD_GL_SEPARABLE            0xf5
#define SGI_KONA_CMD_GL_GET_SEPARABLE        0xf7
#define SGI_KONA_CMD_GL_CONVOLUTION_PARAMETER  0xf8
#define SGI_KONA_CMD_GL_CONVOLUTION_PARAMETERS 0xf9
/*
 * SGI_color_table's commands (established from the IRIX client library;
 * the layouts are in sgi-kona.c, before sgi_kona_gl_colour_table_open()).
 * A table's image is sent between 0x111 8 0x40000000 and 0x111 8 0:
 * 0xe8 and its header, followed by the image's words unless the image
 * comes by DMA. 0xeb sets a table's scale or bias; 0xea and 0xec read a
 * table and a table parameter back on the return page.
 */
#define SGI_KONA_CMD_GL_COLOUR_TABLE         0xe8
#define SGI_KONA_CMD_GL_GET_COLOUR_TABLE     0xea
#define SGI_KONA_CMD_GL_COLOUR_TABLE_PARAMETERS 0xeb
#define SGI_KONA_CMD_GL_GET_COLOUR_TABLE_PARAMETER 0xec
/*
 * EXT_histogram's commands (established from the IRIX client library; the
 * layouts are in sgi-kona.c, before sgi_kona_gl_histogram()).
 */
#define SGI_KONA_CMD_GL_HISTOGRAM            0x102
#define SGI_KONA_CMD_GL_RESET_HISTOGRAM      0x103
#define SGI_KONA_CMD_GL_GET_HISTOGRAM        0x104
#define SGI_KONA_CMD_GL_GET_HISTOGRAM_PARAMETER 0x105
#define SGI_KONA_CMD_GL_MINMAX               0x107
#define SGI_KONA_CMD_GL_RESET_MINMAX         0x108
#define SGI_KONA_CMD_GL_GET_MINMAX           0x109
#define SGI_KONA_CMD_GL_GET_MINMAX_PARAMETER 0x10a
/*
 * 0x116 a b c d: the client library's one-word pipe query, which it sends
 * before splitting a convolved DrawPixels or CopyPixels into strips and
 * then waits for on the return page (see sgi_kona_gl_line_distance()).
 */
#define SGI_KONA_CMD_GL_SINGLE_COMPONENT 0x116
#define SGI_KONA_CMD_GL_READ_PIXELS 0x39
#define SGI_KONA_CMD_GL_COPY_PIXELS 0x38
#define SGI_KONA_CMD_GL_COPY_OVERLAP 0x110
#define SGI_KONA_CMD_GL_COPY_CONVOLUTION 0x115
#define SGI_KONA_CMD_GL_GET         0x4c
#define SGI_KONA_CMD_GL_PUSH_ATTRIB 0xc9
#define SGI_KONA_CMD_GL_POP_ATTRIB  0xc8
#define SGI_KONA_CMD_GL_POP_LIGHTS  0x119
/*
 * A texel readback, 0x52 and 18 words, which the library sends inside a
 * select-feed bracket to copy a resident texture to host memory before it
 * gives the texture's pipe memory to another, or answered on the return
 * page when small. The library's mode command 0x111 and two words comes
 * before it.
 */
#define SGI_KONA_CMD_GL_READ_TEXELS 0x52
#define SGI_KONA_GL_READ_TEXELS_WORDS 18
#define SGI_KONA_CMD_GL_MODE_STATE  0x111
/*
 * Textures: a texel load, 0x174 and 20 header words with the image rows
 * after them, closed by 0x176 and the texture's memory handle; the binds
 * 0x1d5 or 0x106 with five words, 0x179 with 22, and 0x178 with the
 * handle; the detail image 0x17d with six words; the post-texture scale
 * and bias 0xed with four words.
 */
#define SGI_KONA_CMD_GL_TEX_PREFIX  0x1c4
#define SGI_KONA_CMD_GL_TEX_CLEANUP_PREFIX 0x1c5
#define SGI_KONA_CMD_GL_TEX_LOAD    0x174
#define SGI_KONA_CMD_GL_TEX_LOAD_ID 0x176
#define SGI_KONA_CMD_GL_TEX_BIND_ID 0x178
#define SGI_KONA_CMD_GL_TEX_BIND_2D 0x179
#define SGI_KONA_CMD_GL_TEX_BIND_VOLUME 0x17b
#define SGI_KONA_CMD_GL_TEX_BIND_VOLUME_SHORT 0x17c
#define SGI_KONA_CMD_GL_TEX_DETAIL  0x17d
#define SGI_KONA_CMD_GL_TEX_BIND    0x1d5
#define SGI_KONA_CMD_GL_TEX_BIND_LINEAR 0x106
#define SGI_KONA_CMD_GL_TEX_POST_SCALE 0xed
#define SGI_KONA_GL_TEX_LOAD_WORDS  20
/*
 * 0x1e0 and two words, from the library's texture-table bookkeeping: a
 * selector store and either a literal 0, 1 or 2 and one computed store,
 * or two computed stores (the GL selector catalogue). A traced ivview
 * start sent nine of them in a row after its scissor. What the pipe does
 * with them is unknown, so the model frames and consumes them.
 */
#define SGI_KONA_CMD_GL_TEX_TABLE_USAGE 0x1e0
/*
 * glIglooInterfaceSGIX, which IRIS GL's IGLOO library calls to set IRIS GL
 * behaviour, sends one selector and one word for each of five parameters
 * (established from the GL client library's glIglooInterfaceSGIX): IRIS GL
 * mode 0x81a2 as 0x112, LMC colour 0x81a3 as 0x113, colour-normal 0x81a1
 * as 0x1be and tmesh mode 0x81a4 as 0x1c9, each with the caller's first
 * parameter word, and swap-tmesh 0x81a0 as 0x1a9 and a literal 0. Full
 * screen 0x819e is a kernel request and viewport offset 0x819f sends
 * nothing. The model carries out colour-normal and LMC colour (see
 * sgi_kona_gl_iris_vertex()); what the pipe does with the other three is
 * unknown, so the model frames and consumes them.
 */
#define SGI_KONA_CMD_GL_IGLOO_IRISGL_MODE 0x112
#define SGI_KONA_CMD_GL_IGLOO_LMC_COLOR   0x113
#define SGI_KONA_CMD_GL_IGLOO_SWAPTMESH   0x1a9
#define SGI_KONA_CMD_GL_IGLOO_COLORNORMAL 0x1be
#define SGI_KONA_CMD_GL_IGLOO_TMESHMODE   0x1c9
/*
 * glTexGen in every form sends 0x14 and nine words: the coordinate, the
 * parameter name and seven value words, the unused ones zero (established
 * from the GL client library's glTexGen senders). A mode goes as one integer
 * word; an object or eye plane as four floats; the two SGIX parameters
 * 0x81f6 and 0x81f7 as seven floats. The mode and the two planes go to
 * the shared GL's texture coordinate generation; the SGIS point and line
 * parameters 0x81f4 to 0x81f7 are logged and ignored.
 */
#define SGI_KONA_CMD_GL_TEX_GEN     0x14
#define SGI_KONA_GL_TEX_GEN_WORDS   9
/*
 * The glRasterPos forms other than the float ones above, each sent as
 * the caller's components (established from the GL client library's glRasterPos
 * senders): a double as two words, an integer or
 * short as one sign-extended word.
 */
#define SGI_KONA_CMD_GL_RASTER_POS2D 0xa8
#define SGI_KONA_CMD_GL_RASTER_POS2I 0xaa
#define SGI_KONA_CMD_GL_RASTER_POS2S 0xab
#define SGI_KONA_CMD_GL_RASTER_POS3D 0xac
#define SGI_KONA_CMD_GL_RASTER_POS3I 0xae
#define SGI_KONA_CMD_GL_RASTER_POS3S 0xaf
#define SGI_KONA_CMD_GL_RASTER_POS4D 0xb0
#define SGI_KONA_CMD_GL_RASTER_POS4I 0xb2
#define SGI_KONA_CMD_GL_RASTER_POS4S 0xb3
/* glPopAttrib's per-light words after 0x119's count. */
#define SGI_KONA_GL_POP_LIGHT_WORDS 12
/* glPolygonStipple's rows after its three words, one word a row. */
#define SGI_KONA_GL_STIPPLE_ROWS    32

/*
 * The retrace request the kernel's swap sends after its display-table
 * rewrite: one tag, allocated per request.
 */
#define SGI_KONA_CMD_RETRACE        0x1e7
#define SGI_KONA_CMD_X_INIT         0x128
#define SGI_KONA_CMD_X_GC           0x129
#define SGI_KONA_CMD_X_POSITION     0x12a
#define SGI_KONA_CMD_X_CLIP         0x12b
#define SGI_KONA_CMD_X_SCISSOR      0x12d
#define SGI_KONA_CMD_X_COLOR_MAP    0x12e
#define SGI_KONA_CMD_X_GAMMA        0x12f
#define SGI_KONA_CMD_X_CURSOR       0x130
#define SGI_KONA_CMD_X_DID_MAP      0x132
#define SGI_KONA_CMD_X_DID_CONFIG   0x133
#define SGI_KONA_CMD_X_PIXEL_SETUP  0x136
#define SGI_KONA_CMD_X_PIXEL_COPY   0x137
#define SGI_KONA_CMD_X_PIXEL_READ   0x138
#define SGI_KONA_CMD_X_PIXEL_WRITE  0x139
#define SGI_KONA_CMD_X_PIXEL_FINISH 0x13a
#define SGI_KONA_CMD_X_LOAD_STIPPLE 0x13c
#define SGI_KONA_CMD_X_STORE_STIPPLE 0x13d
#define SGI_KONA_CMD_X_ROTATE_STIPPLE 0x13e
#define SGI_KONA_CMD_X_DATA1        0x141
#define SGI_KONA_CMD_X_DATA2        0x142
#define SGI_KONA_CMD_X_DATA3        0x143
#define SGI_KONA_CMD_X_DATA4        0x144
#define SGI_KONA_CMD_X_BDATA4       0x145
#define SGI_KONA_CMD_X_DATA5        0x146
#define SGI_KONA_CMD_X_DATA6        0x147
#define SGI_KONA_CMD_X_DATA8        0x149
#define SGI_KONA_CMD_X_DATA10       0x14b
#define SGI_KONA_CMD_X_POINT        0x14c
#define SGI_KONA_CMD_X_POINT6       0x14d
#define SGI_KONA_CMD_X_POINT24      0x14e
#define SGI_KONA_CMD_X_RELATIVE_POINT  0x14f
#define SGI_KONA_CMD_X_RELATIVE_POINT6 0x150
#define SGI_KONA_CMD_X_RELATIVE_POINT24 0x151
#define SGI_KONA_CMD_X_RECT         0x152
#define SGI_KONA_CMD_X_RECT6        0x153
#define SGI_KONA_CMD_X_BOX          0x154
#define SGI_KONA_CMD_X_BOX6         0x155
#define SGI_KONA_CMD_X_SPANS        0x156
#define SGI_KONA_CMD_X_SPANS2       0x157
#define SGI_KONA_CMD_X_SPANS6       0x158
#define SGI_KONA_CMD_X_SEGMENT      0x159
#define SGI_KONA_CMD_X_SEGMENT6     0x15a
#define SGI_KONA_CMD_X_LINE         0x15b
#define SGI_KONA_CMD_X_LINE6        0x15c
#define SGI_KONA_CMD_X_RELATIVE_LINE  0x15d
#define SGI_KONA_CMD_X_RELATIVE_LINE6 0x15e
#define SGI_KONA_CMD_X_BACKGROUND_POINT   0x15f
#define SGI_KONA_CMD_X_BACKGROUND_POINT6  0x160
#define SGI_KONA_CMD_X_BACKGROUND_POINT24 0x161
#define SGI_KONA_CMD_WG_FLUSH       0x1ff
#define SGI_KONA_CMD_VIDEO          0x1a6

/*
 * The X cursor load's data words after its slot word: both of the X
 * server's cursor senders send 260. sgi-kona.c consumes them.
 */
#define SGI_KONA_X_CURSOR_DATA_WORDS 260

/*
 * The texture table's data words after its slot word: the IRIX client
 * library hands its data sender 33 words, which it sends as sixteen
 * doubleword stores and one word store. sgi-kona.c consumes them.
 */
#define SGI_KONA_GL_TEX_TABLE_WORDS 33

/* The X load GC's words, from both of the X server's GC loaders, framed. */
#define SGI_KONA_X_GC_WORDS 13

/*
 * X pixel setup's words: the mode word, the source-packing,
 * destination-packing and shift table entries, width, height and left pad
 * in bits (the X server's pixmap GC loader).
 */
#define SGI_KONA_X_PIXEL_SETUP_WORDS 7

/*
 * The X load position's and X load clip's words, and X bdata4's, which
 * carries the leading words of a clip list longer than one rectangle.
 */
#define SGI_KONA_X_POSITION_WORDS 4
#define SGI_KONA_X_CLIP_WORDS     5
#define SGI_KONA_X_BDATA4_WORDS   4

/* X load scissor's words: x1, y1, width and height (the glyph blit's). */
#define SGI_KONA_X_SCISSOR_WORDS  4

extern const SGICommandPortSelector sgi_kona_command_tokens[];
extern const unsigned sgi_kona_command_num_tokens;

/*
 * The GL client's buffer configuration, sent by its first make-current
 * before any texture state (established from the GL client library's senders):
 * buffer configuration 1 and ten words, pack 2 and one word, read-buffer
 * configuration 4 and ten words, and 0x114 and one word before the
 * read-buffer configuration when the read drawable's first word is 3.
 * The ten words are, in wire order, level, RGB component size, index
 * size, double-buffer, stereo, multisample, depth, stencil, accumulation
 * and auxiliary buffers, inferred from the field names of the library's
 * own debug print. What 0x114's word says is unknown.
 *
 * These numbers are the PROM textport's: 1 is its sync word, and 2 and 4
 * are its set colour and raster position.
 */
#define SGI_KONA_CMD_GL_BUFFER_CONFIG 0x1
#define SGI_KONA_CMD_GL_PACK          0x2
#define SGI_KONA_CMD_GL_READ_CONFIG   0x4
#define SGI_KONA_CMD_GL_READ_DRAWABLE 0x114
#define SGI_KONA_GL_BUFFER_CONFIG_WORDS 10
/*
 * The buffer configuration's words, from 0: level, RGB component bits,
 * index bits, double-buffer, stereo, samples, depth bits, stencil bits,
 * accumulation red bits and auxiliary buffers (established from the IRIX
 * client library's sender and its glGetIntegerv table, which reads the
 * same fields).
 */
#define SGI_KONA_GL_BUFFER_CONFIG_DOUBLE 3
#define SGI_KONA_GL_BUFFER_CONFIG_SAMPLES 5
#define SGI_KONA_GL_BUFFER_CONFIG_DEPTH  6
#define SGI_KONA_GL_BUFFER_CONFIG_ACCUM  8
#define SGI_KONA_GL_BUFFER_CONFIG_AUX    9

/*
 * Every GL command the model frames, keyed by its 16-bit selector, apart
 * from the kernel's tokens in sgi_kona_command_tokens[]: no number is in
 * both. GL selectors 0x1 to 0xa and 0xc are also PROM textport command
 * numbers, and how the pipe tells the two grammars apart on the shared
 * page is unknown; sgi_kona_textport_push() states the rule the model
 * uses. Selectors whose count is unknown are left out and reach the page
 * as unframed words: the integer, short and double raster positions
 * 0xa8 to 0xb3 apart from the three float forms, the colours 0x65, 0x68,
 * 0x69 and 0x70, and the commands the 3D milestone does not send.
 */
extern const SGICommandPortSelector sgi_kona_gl_tokens[];
extern const unsigned sgi_kona_gl_num_tokens;

/*
 * The video commands the X server sends the display generator through token
 * SGI_KONA_CMD_VIDEO: all 93 numbers of the KONA X driver module's command-name
 * table, 0 to 0x5c, in order, so entry c is command c. On the wire a video
 * command is the token, the command number (the token's one argument in
 * sgi_kona_command_tokens[]), its data words, then a checksum: the command
 * number plus the data words, modulo 2^32. Nothing on the wire carries the
 * length.
 *
 * data_words is the count found at the X server's send sites, a property of the
 * senders read rather than of the command (one pass-through sender can send
 * other lengths), or SGI_KONA_VIDEO_DATA_UNKNOWN where no sender was found.
 * reply_id and reply_bytes are the reply the X server waits for, from the KONA
 * X driver module's 32-entry reply-name and reply-size tables;
 * SGI_KONA_VIDEO_NO_REPLY marks a command whose name is not a reply name. Each
 * row's comment gives the grammar's basis: "sender and wait" (a send site
 * directly followed by a wait for that id and size, the rule checked at every
 * such pair), "sender" (send sites, no wait) or "tables only" (no sender found;
 * the reply id is inferred from the name match alone).
 */
#define SGI_KONA_VIDEO_NO_REPLY        UINT32_MAX
#define SGI_KONA_VIDEO_DATA_UNKNOWN    UINT32_MAX
#define SGI_KONA_VIDEO_COMMANDS        0x5d
#define SGI_KONA_VIDEO_REPLY_IDS       32
/* The longest count found at a sender: LoadFormat's 383 words. */
#define SGI_KONA_VIDEO_MAX_DATA_WORDS  383
/* The largest reply-size table entry: GetVBOBReply's 164 bytes. */
#define SGI_KONA_VIDEO_MAX_REPLY_BYTES 164

/* The command numbers the model gives a reply or other behaviour. */
#define SGI_KONA_VIDEO_GET_BOARD_CONFIG   0x12
#define SGI_KONA_VIDEO_GET_CHANNEL_CONFIG 0x13
#define SGI_KONA_VIDEO_GET_RESIZE         0x15
#define SGI_KONA_VIDEO_CHANNEL_GAMMA_MAP  0x0f
#define SGI_KONA_VIDEO_RESIZE             0x14
#define SGI_KONA_VIDEO_LOAD_FORMAT        0x16
#define SGI_KONA_VIDEO_ENABLE_CHANNEL     0x17
#define SGI_KONA_VIDEO_DISABLE_CHANNEL    0x18
#define SGI_KONA_VIDEO_CURSOR_PRIORITY    0x1e
#define SGI_KONA_VIDEO_CHANNEL_BLANKING   0x1f
#define SGI_KONA_VIDEO_GET_CHANNEL_BLANKING 0x20
#define SGI_KONA_VIDEO_SETUP_FRAME_BUFFER 0x08
#define SGI_KONA_VIDEO_BIND_TO_X_WINDOW   0x34
#define SGI_KONA_VIDEO_VAMPIRE            0x4f
#define SGI_KONA_VIDEO_GET_VAMPIRE        0x3a
#define SGI_KONA_VIDEO_GET_CHANNEL_ENABLES 0x42
#define SGI_KONA_VIDEO_GET_FORMAT_INFO    0x4a
#define SGI_KONA_VIDEO_TEMP_BLANK         0x3c
#define SGI_KONA_VIDEO_X_SERVER_INIT      0x53

typedef struct SGIKonaVideoCommand {
    uint32_t command;
    const char *name;
    uint32_t data_words;
    uint32_t reply_id;
    uint32_t reply_bytes;
} SGIKonaVideoCommand;

extern const SGIKonaVideoCommand sgi_kona_video_commands[];
extern const unsigned sgi_kona_video_num_commands;

/*
 * The KONA X driver module's reply-size table: reply id i carries entry i
 * bytes of data.
 */
extern const uint32_t sgi_kona_video_reply_sizes[SGI_KONA_VIDEO_REPLY_IDS];

#endif /* HW_MISC_SGI_KONA_PORTS_H */
