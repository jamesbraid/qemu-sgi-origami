/* SPDX-License-Identifier: BSD-3-Clause */
/* InfiniteReality command-port selector tables. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-kona-ports.h"

const SGICommandPortSelector sgi_kona_fifo_selectors[] = {
    /*
     * KONAsetdisplaymode: selector, then 0, then the mode/timing argument,
     * then the fault-byte argument, the one entry in this table whose
     * selector-first word order is confirmed, not inferred.
     */
    { 0x1e6, 3 },
    { 0x1f5, 0 }, /* kona_start_selectfeed: bare */
    { 0x1f6, 0 }, /* kona_finish_selectfeed: bare */
};
const unsigned sgi_kona_fifo_num_selectors =
    ARRAY_SIZE(sgi_kona_fifo_selectors);

const SGICommandPortSelector sgi_kona_textport_selectors[] = {
    { 0x1, 1 },
    { 0x2, 1 }, /* same shape as kona_setcolor */
    { 0x3, 4 },
    { 0x4, 2 },
    { 0x5, 4 },
    { 0x6, 2 }, /* same shape as kona_point */
    { 0x7, 2 },
    { 0x8, 2 }, /* cached screen size */
    { 0x9, 1 }, /* sync probe, magic 0x1234abcd */
    { 0xc, 1 }, /* graphics info, five reply words */
};
const unsigned sgi_kona_textport_num_selectors =
    ARRAY_SIZE(sgi_kona_textport_selectors);

const SGICommandPortSelector sgi_kona_doorbell_selectors[] = {
    { 0x71,  1 }, /* glEnd */
    { 0xc7,  1 }, /* glFlush */
    { 0xc6,  1 }, /* glFinish / glXSwapBuffers */
    { 0x1cc, 1 }, /* glFlushRasterSGIX */
    { 0x116, 4 }, /* GetSingleCompFromPipe */
    { 0x1d6, 1 }, /* context resolution */
    { 0x1ca, 1 }, /* context resolution */
};
const unsigned sgi_kona_doorbell_num_selectors =
    ARRAY_SIZE(sgi_kona_doorbell_selectors);

/*
 * Each command is framed on its own token, never on the +0xc00108 kick
 * values: kona_checkpipe can send 0x1dd after only a 0x10000 kick, and a
 * graphics process's own commands carry no kicks at all.
 *
 * 0x1f1, 0x1f2 and 0x1fd take a page word, a tagged offset word and a
 * length, as the driver's KONAstart and context activation send them and
 * a traced run shows. 0x1f8 and 0x1dd take one reason word, from
 * KONAdestroyddrn and kona_checkpipe. 0x1fc takes the outgoing
 * context's index, the one word KONApcxswap sends after it before it
 * sleeps. 0x1e9's one word is observed only:
 * gfxinit writes nothing after it, so a longer command cannot be ruled
 * out. 0x1e6 takes a word, a table index and a byte, as
 * KONAsetdisplaymode and the swap paths send it.
 *
 * The X server's own tokens count the first argument, which travels
 * in the token's slot, and the data words after it. Each count comes
 * from one caller. A wrong count is not reliably detectable: a surplus
 * word would open a command of its own.
 *
 * Every other word stays on the textport's unexpected-word path.
 */
const SGICommandPortSelector sgi_kona_command_tokens[] = {
    { SGI_KONA_CMD_REASON_PAGE, 3 },
    { SGI_KONA_CMD_RETURN_PAGE, 3 },
    { SGI_KONA_CMD_CONTEXT_MAP, 3 },
    { SGI_KONA_CMD_POST_REASON, 1 },
    { SGI_KONA_CMD_CHECK_PIPE, 1 },
    { SGI_KONA_CMD_GET_CONFIG, 1 },
    { SGI_KONA_CMD_DISPLAY_MODE, 3 },
    { SGI_KONA_CMD_CONTEXT_DEACTIVATE, 1 },
    /*
     * The rest of a context swap, from KONApcxswap, kona_act_pcx,
     * KONAvalidateclip and kona_dmapool_update_pipe: each count holds at
     * every sender read. kona_act_pcx's last packet takes its
     * token from word 2 of the incoming context's save area and carries
     * one 0; sgi-kona.c leaves 0x172 there, framed above.
     */
    { SGI_KONA_CMD_CONTEXT_RESTORE, 4 },
    { SGI_KONA_CMD_CONTEXT_RECORDS, 11 },
    { SGI_KONA_CMD_CONTEXT_WORDS, 3 },
    { SGI_KONA_CMD_SWAP_PAIR, 2 },
    { SGI_KONA_CMD_DMA_POOL, 3 },
    /* kona_idma's packet and kona_dmawrite's list-write record. */
    { SGI_KONA_CMD_DMA_PACKET, 6 },
    { SGI_KONA_CMD_DMA_LIST_WRITE, 2 },
    /*
     * The display-list packets, each count read at its libGLcore sender.
     * 0x170 and 0x171 count their n only; the n pairs follow.
     */
    { SGI_KONA_CMD_LIST_CALL, 2 },
    { SGI_KONA_CMD_LIST_CALL_POOL, 2 },
    { SGI_KONA_CMD_LIST_CALLS, 1 },
    { SGI_KONA_CMD_LIST_CALLS_POOL, 1 },
    { SGI_KONA_CMD_LIST_PULL, 4 },
    { SGI_KONA_CMD_LIST_MOVE, 3 },
    /* KONAschedswapbuf and kona_retrace_activate: the retrace tag. */
    { SGI_KONA_CMD_RETRACE, 1 },
    /*
     * The pool packets, from the kernel's kona_mem routines. A region is
     * class, key, offset and size, both in granules; a copy-down class, old
     * offset, new offset and size; a swap-out class, offset, size, granule,
     * copy count, host address >> 12, (address & 0xfff) | 0x9000 and the
     * completion reason 0x102. 0x172 carries one 0 at every sender read. The
     * swap-in, suspend and resume packets are not here: swap-in can carry
     * its data inline, resume the words a suspend reply returned, and the
     * 0x104 after a suspend has no stated count.
     */
    { SGI_KONA_CMD_MEM_REGION, 4 },
    { SGI_KONA_CMD_MEM_COPY_DOWN, 4 },
    { SGI_KONA_CMD_MEM_SWAP_OUT, 8 },
    { SGI_KONA_CMD_RESOURCE_SYNC, 1 },
    { SGI_KONA_CMD_X_INIT, 3 },        /* width, height, depth code */
    /* kon_loadGC, from the GC private */
    { SGI_KONA_CMD_X_GC, SGI_KONA_X_GC_WORDS },
    { SGI_KONA_CMD_X_POSITION, SGI_KONA_X_POSITION_WORDS },
    /*
     * kon_loadClipRects: a rectangle count, then the rectangles. With one
     * rectangle 0x12b carries all five words. With more, X bdata4 carries
     * the count and the first rectangle's x1, y1, x2, then each further
     * rectangle's y2 and the next rectangle's x1, y1, x2, and 0x12b the
     * last y2 and the last rectangle. sgi-kona.c joins them.
     */
    { SGI_KONA_CMD_X_BDATA4, SGI_KONA_X_BDATA4_WORDS },
    { SGI_KONA_CMD_X_CLIP, SGI_KONA_X_CLIP_WORDS },
    /*
     * The glyph senders' clip box, x1, y1, width and height, sent right
     * after a one-rectangle X load clip with the same box.
     */
    { SGI_KONA_CMD_X_SCISSOR, SGI_KONA_X_SCISSOR_WORDS },
    /*
     * Start index and count n only. The n entries follow, framed by
     * sgi-kona.c: konInstallColormap sends 2 + n words, and kon_InitHW's
     * three-word load is n = 1.
     */
    { SGI_KONA_CMD_X_COLOR_MAP, 2 },
    /*
     * Gamma-RAM start address and count n only, like the colour map: the
     * n entries follow, framed by sgi-kona.c ("The gamma load, 0x12f").
     */
    { SGI_KONA_CMD_X_GAMMA, 2 },
    /*
     * The slot word only: 0, or 260 times a cursor-private word. The
     * SGI_KONA_X_CURSOR_DATA_WORDS data words follow, consumed by
     * sgi-kona.c: 261 words exceed the framer's argument array.
     */
    { SGI_KONA_CMD_X_CURSOR, 1 },
    { SGI_KONA_CMD_X_DID_MAP, 2 },     /* entry index, packed entry */
    { SGI_KONA_CMD_X_DID_CONFIG, 1 },  /* 1, or 0 from konDestroyWindow */
    { SGI_KONA_CMD_X_PIXEL_SETUP, SGI_KONA_X_PIXEL_SETUP_WORDS },
    /*
     * Destination x and y only. The row data follows, framed by
     * sgi-kona.c from the setup ("Pixel writes and copies").
     */
    { SGI_KONA_CMD_X_PIXEL_WRITE, 2 },
    { SGI_KONA_CMD_X_PIXEL_FINISH, 1 },
    /* Source x and y, then destination x and y (konCopyRect). */
    { SGI_KONA_CMD_X_PIXEL_COPY, 4 },
    /*
     * The stipple tokens: load takes a cache entry, or
     * 0xffffffff to unload; rotate an entry and the pattern origin x, y;
     * store an entry, the width and the height here, then height pattern
     * rows, framed by sgi-kona.c since a pattern can outrun the framer's
     * argument array.
     */
    { SGI_KONA_CMD_X_LOAD_STIPPLE, 1 },
    { SGI_KONA_CMD_X_STORE_STIPPLE, 3 },
    { SGI_KONA_CMD_X_ROTATE_STIPPLE, 3 },
    /*
     * Point senders carry one or six words. Short line-data runs carry
     * one or three.
     */
    { SGI_KONA_CMD_X_POINT, 1 },
    { SGI_KONA_CMD_X_POINT6, 6 },
    { SGI_KONA_CMD_X_DATA1, 1 },
    { SGI_KONA_CMD_X_DATA3, 3 },
    /*
     * The box routines' rectangles, two words each: X box6 carries six, X
     * box the last of a run, and the data tokens one to five before that X
     * box, or the rectangles before an X rect. Each count was read at the
     * senders.
     */
    { SGI_KONA_CMD_X_DATA2, 2 },
    { SGI_KONA_CMD_X_DATA4, 4 },
    { SGI_KONA_CMD_X_DATA6, 6 },
    { SGI_KONA_CMD_X_DATA8, 8 },
    { SGI_KONA_CMD_X_DATA10, 10 },
    { SGI_KONA_CMD_X_BOX, 2 },
    { SGI_KONA_CMD_X_BOX6, 12 },
    /*
     * konPolyFillRectSolid's rectangles, two words each, relative to the
     * X load position: X rect6 carries six, X rect the last of a run,
     * after a data token carrying the ones before it.
     */
    { SGI_KONA_CMD_X_RECT, 2 },
    { SGI_KONA_CMD_X_RECT6, 12 },
    /*
     * konFillSpansSolid's spans: a count of 1, 2 or 6, then that many
     * points and that many widths.
     */
    { SGI_KONA_CMD_X_SPANS, 3 },
    { SGI_KONA_CMD_X_SPANS2, 5 },
    { SGI_KONA_CMD_X_SPANS6, 13 },
    { SGI_KONA_CMD_WG_FLUSH, 1 },      /* 0xdeadbeef from kon_InitHW */
    /*
     * The video command's number only. Its data words and checksum
     * follow, framed by sgi-kona.c from sgi_kona_video_commands[] below.
     */
    { SGI_KONA_CMD_VIDEO, 1 },
};
const unsigned sgi_kona_command_num_tokens =
    ARRAY_SIZE(sgi_kona_command_tokens);

/*
 * Each count was read at the GL call's sender in libGLcore and is
 * established unless its comment says otherwise. Four commands carry more
 * words than their count, which sgi-kona.c consumes after the packet: the
 * texture table's data, the polygon stipple's rows, the pop-lights words
 * and a texel load's rows.
 */
const SGICommandPortSelector sgi_kona_gl_tokens[] = {
    /* The buffer configuration and pack, before the texture state. */
    { SGI_KONA_CMD_GL_BUFFER_CONFIG, SGI_KONA_GL_BUFFER_CONFIG_WORDS },
    { SGI_KONA_CMD_GL_PACK, 1 },
    { SGI_KONA_CMD_GL_READ_CONFIG, SGI_KONA_GL_BUFFER_CONFIG_WORDS },
    { SGI_KONA_CMD_GL_READ_DRAWABLE, 1 },
    /* The rest of the first make-current. */
    { SGI_KONA_CMD_GL_ACTIVE_TEXTURE, 1 },
    { SGI_KONA_CMD_GL_TEX_SCALE_BIAS, 4 },
    { SGI_KONA_CMD_GL_TEX_TABLE, 1 },
    { SGI_KONA_CMD_GL_LINE_DISTANCE, 1 },
    { SGI_KONA_CMD_GL_LAZY_MODE, 1 },
    { SGI_KONA_CMD_GL_VIEWPORT, 4 },
    { SGI_KONA_CMD_GL_SCISSOR, 4 },
    { SGI_KONA_CMD_GL_SET_ERROR, 1 },
    /* Clear, begin and end, flush, finish and the error query. */
    { SGI_KONA_CMD_GL_CLEAR_COLOR, 4 },
    { SGI_KONA_CMD_GL_CLEAR, 1 },
    { SGI_KONA_CMD_GL_CLEAR_ACCUM, 4 },
    { SGI_KONA_CMD_GL_CLEAR_INDEX, 1 },
    { SGI_KONA_CMD_GL_CLEAR_STENCIL, 1 },
    { SGI_KONA_CMD_GL_CLEAR_DEPTH, 2 },     /* one double */
    { SGI_KONA_CMD_GL_BEGIN_FIRST + 0, 1 },
    { SGI_KONA_CMD_GL_BEGIN_FIRST + 1, 1 },
    { SGI_KONA_CMD_GL_BEGIN_FIRST + 2, 1 },
    { SGI_KONA_CMD_GL_BEGIN_FIRST + 3, 1 },
    { SGI_KONA_CMD_GL_BEGIN_FIRST + 4, 1 },
    { SGI_KONA_CMD_GL_BEGIN_FIRST + 5, 1 },
    { SGI_KONA_CMD_GL_BEGIN_FIRST + 6, 1 },
    { SGI_KONA_CMD_GL_BEGIN_FIRST + 7, 1 },
    { SGI_KONA_CMD_GL_BEGIN_FIRST + 8, 1 },
    { SGI_KONA_CMD_GL_BEGIN_LAST, 1 },
    { SGI_KONA_CMD_GL_END, 1 },
    /* The words up to the end, as arithmetic on the senders gives them. */
    { SGI_KONA_CMD_GL_RECT, 1 },
    { SGI_KONA_CMD_GL_FLUSH, 1 },
    { SGI_KONA_CMD_GL_FINISH, 1 },
    { SGI_KONA_CMD_GL_FLUSH_RASTER, 1 },
    { SGI_KONA_CMD_GL_GET_ERROR, 1 },
    { SGI_KONA_CMD_GL_RESOLVE_A, 1 },
    { SGI_KONA_CMD_GL_RESOLVE_B, 1 },
    /* The matrices, a double counting two words. */
    { SGI_KONA_CMD_GL_DEPTH_RANGE, 4 },
    { SGI_KONA_CMD_GL_MATRIX_MODE, 1 },
    { SGI_KONA_CMD_GL_FRUSTUM, 12 },
    { SGI_KONA_CMD_GL_LOAD_IDENTITY, 1 },
    { SGI_KONA_CMD_GL_LOAD_MATRIXF, 16 },
    { SGI_KONA_CMD_GL_LOAD_MATRIXD, 32 },
    { SGI_KONA_CMD_GL_MULT_MATRIXF, 16 },
    { SGI_KONA_CMD_GL_MULT_MATRIXD, 32 },
    { SGI_KONA_CMD_GL_ORTHO, 12 },
    { SGI_KONA_CMD_GL_POP_MATRIX, 1 },
    { SGI_KONA_CMD_GL_PUSH_MATRIX, 1 },
    { SGI_KONA_CMD_GL_ROTATED, 8 },
    { SGI_KONA_CMD_GL_ROTATEF, 4 },
    { SGI_KONA_CMD_GL_SCALED, 6 },
    { SGI_KONA_CMD_GL_SCALEF, 3 },
    { SGI_KONA_CMD_GL_TRANSLATED, 6 },
    { SGI_KONA_CMD_GL_TRANSLATEF, 3 },
    /*
     * Colours, normals, texture coordinates and vertices. A packed form's
     * count is the words after the selector word, which carries data in
     * its top half. The four-short vertex's two words are inferred from
     * the two- and three-short forms; the other counts are established.
     */
    { SGI_KONA_CMD_GL_COLOR3B, 1 },
    { SGI_KONA_CMD_GL_COLOR3D, 6 },
    { SGI_KONA_CMD_GL_COLOR3, 3 },
    { SGI_KONA_CMD_GL_COLOR3I, 3 },
    { SGI_KONA_CMD_GL_COLOR3UB, 1 },
    { SGI_KONA_CMD_GL_COLOR3UI, 3 },
    { SGI_KONA_CMD_GL_COLOR4D, 8 },
    { SGI_KONA_CMD_GL_COLOR4F, 4 },
    { SGI_KONA_CMD_GL_COLOR4I, 4 },
    { SGI_KONA_CMD_GL_COLOR4S, 2 },
    { SGI_KONA_CMD_GL_COLOR4UB, 1 },
    { SGI_KONA_CMD_GL_COLOR4UI, 4 },
    { SGI_KONA_CMD_GL_NORMAL3B, 1 },
    { SGI_KONA_CMD_GL_NORMAL3D, 6 },
    { SGI_KONA_CMD_GL_NORMAL3F, 3 },
    { SGI_KONA_CMD_GL_NORMAL3I, 3 },
    { SGI_KONA_CMD_GL_NORMAL3S, 1 },
    { SGI_KONA_CMD_GL_TEXCOORD1F, 1 },
    { SGI_KONA_CMD_GL_TEXCOORD2F, 2 },
    { SGI_KONA_CMD_GL_TEXCOORD2S, 1 },
    { SGI_KONA_CMD_GL_TEXCOORD3F, 3 },
    { SGI_KONA_CMD_GL_TEXCOORD4F, 4 },
    { SGI_KONA_CMD_GL_VERTEX2D, 4 },
    { SGI_KONA_CMD_GL_VERTEX2, 2 },
    { SGI_KONA_CMD_GL_VERTEX2I, 2 },
    { SGI_KONA_CMD_GL_VERTEX2S, 1 },
    { SGI_KONA_CMD_GL_VERTEX3D, 6 },
    { SGI_KONA_CMD_GL_VERTEX3F, 3 },
    { SGI_KONA_CMD_GL_VERTEX3I, 3 },
    { SGI_KONA_CMD_GL_VERTEX3S, 1 },
    { SGI_KONA_CMD_GL_VERTEX4D, 8 },
    { SGI_KONA_CMD_GL_VERTEX4F, 4 },
    { SGI_KONA_CMD_GL_VERTEX4I, 4 },
    { SGI_KONA_CMD_GL_VERTEX4S, 2 },
    { SGI_KONA_CMD_GL_EDGE_FLAG, 1 },
    { SGI_KONA_CMD_GL_RASTER_POS2F, 2 },
    { SGI_KONA_CMD_GL_RASTER_POS3F, 3 },
    { SGI_KONA_CMD_GL_RASTER_POS4F, 4 },
    /* Lighting. */
    { SGI_KONA_CMD_GL_LIGHTF, 6 },
    { SGI_KONA_CMD_GL_LIGHTI, 6 },
    { SGI_KONA_CMD_GL_LIGHT_MODELF, 5 },
    { SGI_KONA_CMD_GL_LIGHT_MODELI, 5 },
    { SGI_KONA_CMD_GL_MATERIAL, 6 },
    { SGI_KONA_CMD_GL_COLOR_MATERIAL, 2 },
    /* Enables and raster state. */
    { SGI_KONA_CMD_GL_ENABLE, 1 },
    { SGI_KONA_CMD_GL_DISABLE, 1 },
    { SGI_KONA_CMD_GL_SHADE_MODEL, 1 },
    { SGI_KONA_CMD_GL_DEPTH_FUNC, 1 },
    { SGI_KONA_CMD_GL_DEPTH_MASK, 1 },
    { SGI_KONA_CMD_GL_BLEND_FUNC, 2 },
    { SGI_KONA_CMD_GL_BLEND_COLOR, 4 },
    { SGI_KONA_CMD_GL_BLEND_EQUATION, 1 },
    { SGI_KONA_CMD_GL_ALPHA_FUNC, 2 },
    { SGI_KONA_CMD_GL_STENCIL_FUNC, 3 },
    { SGI_KONA_CMD_GL_STENCIL_OP, 3 },
    { SGI_KONA_CMD_GL_STENCIL_MASK, 1 },
    { SGI_KONA_CMD_GL_COLOR_MASK, 4 },
    { SGI_KONA_CMD_GL_INDEX_MASK, 1 },
    { SGI_KONA_CMD_GL_LOGIC_OP, 1 },
    { SGI_KONA_CMD_GL_CULL_FACE, 1 },
    { SGI_KONA_CMD_GL_FRONT_FACE, 1 },
    { SGI_KONA_CMD_GL_POLYGON_MODE, 2 },
    { SGI_KONA_CMD_GL_POLYGON_OFFSET, 2 },
    { SGI_KONA_CMD_GL_OFFSET_EXT, 2 },
    /* Three words of its own; the rows follow. */
    { SGI_KONA_CMD_GL_POLYGON_STIPPLE, 3 },
    { SGI_KONA_CMD_GL_LINE_WIDTH, 1 },
    { SGI_KONA_CMD_GL_POINT_SIZE, 1 },
    { SGI_KONA_CMD_GL_LINE_STIPPLE, 2 },
    { SGI_KONA_CMD_GL_FOGF, 5 },
    { SGI_KONA_CMD_GL_FOGI, 5 },
    { SGI_KONA_CMD_GL_CLIP_PLANE, 9 },      /* the plane and four doubles */
    { SGI_KONA_CMD_GL_HINT, 2 },
    { SGI_KONA_CMD_GL_DRAW_BUFFER, 1 },
    { SGI_KONA_CMD_GL_READ_BUFFER, 1 },
    { SGI_KONA_CMD_GL_PIXEL_STORE, 2 },
    { SGI_KONA_CMD_GL_TEX_ENVF, 5 },
    { SGI_KONA_CMD_GL_TEX_ENVI, 5 },
    /* Pixels, queries and the attribute stack. */
    { SGI_KONA_CMD_GL_DRAW_PIXELS, 8 },     /* the header; rows follow */
    { SGI_KONA_CMD_GL_READ_PIXELS, 7 },
    { SGI_KONA_CMD_GL_GET, 1 },
    { SGI_KONA_CMD_GL_PUSH_ATTRIB, 1 },
    { SGI_KONA_CMD_GL_POP_ATTRIB, 1 },
    { SGI_KONA_CMD_GL_POP_LIGHTS, 1 },      /* the count; its words follow */
    /* Textures. */
    { SGI_KONA_CMD_GL_TEX_LOAD, SGI_KONA_GL_TEX_LOAD_WORDS },
    { SGI_KONA_CMD_GL_TEX_LOAD_ID, 1 },
    { SGI_KONA_CMD_GL_TEX_BIND_ID, 1 },
    { SGI_KONA_CMD_GL_TEX_BIND_2D, 22 },
    { SGI_KONA_CMD_GL_TEX_BIND, 5 },
    { SGI_KONA_CMD_GL_TEX_BIND_LINEAR, 5 },
    { SGI_KONA_CMD_GL_TEX_POST_SCALE, 4 },
};
const unsigned sgi_kona_gl_num_tokens = ARRAY_SIZE(sgi_kona_gl_tokens);


/*
 * The video command table, one row per command number. See the header
 * for what each basis comment means.
 */
#define U  SGI_KONA_VIDEO_DATA_UNKNOWN
#define NR SGI_KONA_VIDEO_NO_REPLY
const SGIKonaVideoCommand sgi_kona_video_commands[] = {
    { 0x00, "Test", U, 0, 4 }, /* tables only */
    { 0x01, "VCWrite", U, NR, 0 }, /* tables only */
    { 0x02, "VCRead", U, 1, 4 }, /* tables only */
    { 0x03, "DiagXmapColor", U, NR, 0 }, /* tables only */
    { 0x04, "DisableDiagXmapColor", U, NR, 0 }, /* tables only */
    { 0x05, "VCBusTrace", U, NR, 0 }, /* tables only */
    { 0x06, "DiagCursorFromImage", U, NR, 0 }, /* tables only */
    { 0x07, "DebugMask", U, NR, 0 }, /* tables only */
    { 0x08, "SetupFrameBuffer", 3, NR, 0 }, /* sender, 1 site */
    { 0x09, "InputSync", 1, NR, 0 }, /* sender, 2 sites */
    { 0x0a, "GetInputSync", U, 2, 4 }, /* tables only */
    { 0x0b, "HPhase", 2, NR, 0 }, /* sender, 1 site */
    { 0x0c, "GetHPhase", 1, 3, 4 }, /* sender and wait, 1 site */
    { 0x0d, "VPhase", 1, NR, 0 }, /* sender, 1 site */
    { 0x0e, "GetVPhase", 1, 4, 4 }, /* sender and wait, 1 site */
    { 0x0f, "ChannelGammaMap", 2, NR, 0 }, /* sender, 1 site */
    { 0x10, "GetChannelGammaMap", 1, 5, 4 }, /* sender and wait, 1 site */
    { 0x11, "Fetal", U, NR, 0 }, /* tables only */
    /* sender and wait, 1 site */
    { SGI_KONA_VIDEO_GET_BOARD_CONFIG, "GetBoardConfig", 1, 6, 20 },
    /* sender and wait, 6 sites */
    { SGI_KONA_VIDEO_GET_CHANNEL_CONFIG, "GetChannelConfig", 1, 7, 76 },
    { 0x14, "Resize", 8, NR, 0 }, /* sender, 2 sites */
    { 0x15, "GetResize", 1, 8, 72 }, /* sender and wait, 9 sites */
    { 0x16, "LoadFormat", 383, NR, 0 }, /* sender, 1 site */
    { 0x17, "EnableChannel", 3, NR, 0 }, /* sender, 5 sites */
    { 0x18, "DisableChannel", 1, NR, 0 }, /* sender, 1 site */
    { 0x19, "LoadGenlockParams", 16, NR, 0 }, /* sender, 1 site */
    { 0x1a, "EnableGenlock", 1, NR, 0 }, /* sender, 3 sites */
    { 0x1b, "CursorPosition", U, NR, 0 }, /* tables only */
    { 0x1c, "CursorChannel", U, NR, 0 }, /* tables only */
    { 0x1d, "CursorType", U, NR, 0 }, /* tables only */
    { 0x1e, "CursorPriority", 2, NR, 0 }, /* sender, 1 site */
    { 0x1f, "ChannelBlanking", 2, NR, 0 }, /* sender, 1 site */
    { 0x20, "GetChannelBlanking", 1, 9, 4 }, /* sender and wait, 2 sites */
    { 0x21, "OutputGain", 2, NR, 0 }, /* sender, 1 site */
    { 0x22, "GetOutputGain", 1, 10, 4 }, /* sender and wait, 1 site */
    { 0x23, "SyncSelects", 4, NR, 0 }, /* sender, 1 site */
    { 0x24, "GetSyncSelects", 1, 11, 24 }, /* sender and wait, 1 site */
    { 0x25, "OutputPedestal", 2, NR, 0 }, /* sender, 1 site */
    { 0x26, "GetOutputPedestal", 1, 12, 4 }, /* sender and wait, 2 sites */
    { 0x27, "InitTextport", U, NR, 0 }, /* tables only */
    { 0x28, "LoadTextportGlyph", U, NR, 0 }, /* tables only */
    { 0x29, "LoadTextportColorIndex", U, NR, 0 }, /* tables only */
    { 0x2a, "EnableVocReadback", U, NR, 0 }, /* tables only */
    { 0x2b, "MasterClockSelect", 1, NR, 0 }, /* sender, 1 site */
    { 0x2c, "GetMasterClockSelect", U, 13, 4 }, /* tables only */
    { 0x2d, "AutoInit", U, NR, 0 }, /* tables only */
    { 0x2e, "LoadEncoder", 1, NR, 0 }, /* sender, 1 site */
    { 0x2f, "EncoderOutputGain", U, NR, 0 }, /* tables only */
    { 0x30, "GetEncoderOutputGain", U, 14, 4 }, /* tables only */
    { 0x31, "SCHPhase", 1, NR, 0 }, /* sender, 1 site */
    { 0x32, "GetSCHPhase", 1, 15, 4 }, /* sender and wait, 1 site */
    { 0x33, "EncoderTestPattern", 1, NR, 0 }, /* sender, 1 site */
    { 0x34, "BindChannelToXWid", 2, NR, 0 }, /* sender, 2 sites */
    { 0x35, "LoadPabXilinx", U, NR, 0 }, /* tables only */
    { 0x36, "LoadPabSizeRegs", U, NR, 0 }, /* tables only */
    { 0x37, "LoadPabCtrlRegs", U, NR, 0 }, /* tables only */
    { 0x38, "LoadDacCursor", 263, NR, 0 }, /* sender, 1 site */
    { 0x39, "VampireAndFieldDominance", U, NR, 0 }, /* tables only */
    { 0x3a, "GetVampire", 1, 16, 24 }, /* sender and wait, 1 site */
    { 0x3b, "FastlistChunksize", 1, NR, 0 }, /* sender, 1 site */
    { 0x3c, "TempBlankAllChannels", 1, NR, 0 }, /* sender, 1 site */
    { 0x3d, "GetDGInterruptState", 1, 17, 12 }, /* sender and wait, 1 site */
    { 0x3e, "XMapClockSelect", 1, NR, 0 }, /* sender, 1 site */
    { 0x3f, "GetXMapClockSelect", U, 18, 4 }, /* tables only */
    { 0x40, "ChannelResetRatios", 3, NR, 0 }, /* sender, 1 site */
    { 0x41, "ResetRMBoards", U, NR, 0 }, /* tables only */
    { 0x42, "GetChannelEnables", 1, 19, 8 }, /* sender and wait, 1 site */
    { 0x43, "PlacementRes", 9, NR, 0 }, /* sender, 1 site */
    { 0x44, "GenlockSwitching", 1, NR, 0 }, /* sender, 2 sites */
    { 0x45, "GetGenlockSwitching", 1, 20, 4 }, /* sender and wait, 2 sites */
    { 0x46, "ChannelFieldDominance", 2, NR, 0 }, /* sender, 1 site */
    { 0x47, "GetChannelFieldDominance", U, 21, 4 }, /* tables only */
    { 0x48, "BoardResetRatio", 1, NR, 0 }, /* sender, 1 site */
    { 0x49, "GetBoardResetRatio", U, 22, 4 }, /* tables only */
    { 0x4a, "GetChannelFormatInfo", 1, 23, 20 }, /* sender and wait, 6 sites */
    { 0x4b, "ChannelGammaMapState", 5, NR, 0 }, /* sender, 2 sites */
    /* sender and wait, 2 sites */
    { 0x4c, "GetChannelGammaMapState", 1, 24, 16 },
    { 0x4d, "EnableEncoder", 1, NR, 0 }, /* sender, 1 site */
    { 0x4e, "ResizeSyncType", U, NR, 0 }, /* tables only */
    { 0x4f, "Vampire", 7, NR, 0 }, /* sender, 2 sites */
    { 0x50, "QueryMonitorName", 1, 25, 68 }, /* sender and wait, 1 site */
    { 0x51, "MonitorCommand", U, 26, 136 }, /* tables only */
    { 0x52, "LoadXilinx", 6, 27, 4 }, /* sender and wait, 2 sites */
    /* sender, 1 site */
    { SGI_KONA_VIDEO_X_SERVER_INIT, "XServerVideoInit", 1, NR, 0 },
    { 0x54, "InternalGenlock", 1, NR, 0 }, /* sender, 1 site */
    { 0x55, "GetInternalGenlock", 1, 28, 4 }, /* sender and wait, 1 site */
    { 0x56, "BufferParams", 3, NR, 0 }, /* sender, 1 site */
    { 0x57, "GetBufferParams", U, 29, 8 }, /* tables only */
    { 0x58, "DataFormat", 3, NR, 0 }, /* sender, 1 site */
    { 0x59, "GetDataFormat", U, 30, 8 }, /* tables only */
    { 0x5a, "DVPMult2Freq", 1, NR, 0 }, /* sender, 1 site */
    { 0x5b, "VBOBCommand", 42, NR, 0 }, /* sender, 1 site */
    { 0x5c, "GetVBOBReply", U, 31, 164 }, /* tables only */
};
#undef U
#undef NR
const unsigned sgi_kona_video_num_commands =
    ARRAY_SIZE(sgi_kona_video_commands);

const uint32_t sgi_kona_video_reply_sizes[SGI_KONA_VIDEO_REPLY_IDS] = {
    4, 4, 4, 4, 4, 4, 20, 76, 72, 4, 4, 24, 4, 4, 4, 4, 24, 12, 4, 8, 4, 4,
    4, 20, 16, 68, 136, 4, 4, 8, 8, 164,
};
