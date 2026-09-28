/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SI MGRAS raster command implementation.
 * Direct records use payload32 | (selector << 34), with selector
 * 0x60001/0x70001 | (register << 6). FIFO words are 32-bit
 * headers in high-half-first order and retain payload state across
 * MMIO stores. Unsupported traffic remains observable and opaque.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/cutils.h"
#include "hw/misc/sgi-mgras-raster.h"
#include "hw/misc/sgi-mgras-gl.h"
#include "system/dma.h"
#include "hw/display/sgi/pixel-dma.h"
#include "trace.h"




#define MGRAS_CMD_SELECTOR_MASK 0x1fff803f
#define MGRAS_CMD_TOKEN_LIMIT_BYTES \
    (SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS * sizeof(uint32_t))
#define MGRAS_CMD_SELECTOR_STATE 0x60001
#define MGRAS_CMD_SELECTOR_EXEC 0x70001


/*
 * A header with bit 31 set opens an inline data block whose byte length is
 * bits 19:0, not the 8-bit field of the other headers.
 */
#define MGRAS_CMD_HDR_INLINE_BLOCK 0x80000000u
#define MGRAS_CMD_HDR_INLINE_BYTES 0x000fffffu

#define MGRAS_CMD_FIFO_FENCE_CLASS 0x000e
#define MGRAS_CMD_FIFO_FENCE_REG 0x04
/*
 * 0x000e0100 is the client's DMA-read fence marker, which
 * __glMgrWaitForDMARead sends before it polls 0x020200 & 0x1eff for zero
 * (mgras-gl-commands.md in the Origami repository, "DMA-read fence";
 * established as software expectation). The client also pads pooled
 * display lists with it. What the board does with it is unknown. It has
 * no payload, the model's DMA is always idle by the time the client
 * polls, and the word would otherwise be logged once per padding word.
 */
#define MGRAS_CMD_FIFO_DMA_READ_FENCE_REG 0x01
#define MGRAS_HQ_FLAG_FENCE 0x00010000

/*
 * Class-0 opcodes the HQ answers itself. The query is 0x0000a20c and three
 * arguments; the client then polls HQ flag bit 17 and reads the answer at
 * 0x028004. The swap is the single word 0x00003700 and raises general
 * interrupt source 10.
 */
#define MGRAS_CMD_HDR_CLASS_HQ 0x0000
#define MGRAS_HQ_OP_QUERY 0xa2
/* 0xa104: one current-attribute index; 0x8b08: a histogram bin query. */
#define MGRAS_HQ_OP_ATTR_QUERY 0xa1
#define MGRAS_HQ_OP_HISTOGRAM_QUERY 0x8b
#define MGRAS_HQ_OP_SWAP 0x37
#define MGRAS_HQ_FLAG_QUERY 0x00020000


#define MGRAS_CMD_HDR_CLASS_STATE 0x0018
#define MGRAS_CMD_HDR_CLASS_EXEC 0x001c
#define MGRAS_CMD_HDR_CLASS_DISCARD SGI_MGRAS_CMD_TOKEN_DISCARD

/*
 * Host-memory DMA image-transfer descriptor banks, main-port command-FIFO
 * classes 0x0008/0x000a/0x000c (bank_base >> 14). Unlike STATE/EXEC these
 * are plain register writes: a header names a starting register and
 * payload_bytes/4 consecutive words are stored there as they arrive,
 * mirroring mgras_dma_map_token's framing on the kernel mapping port. Writing
 * 0x20000 register 11 (the kick) starts a transfer: bit 3 of the low byte
 * selects direction (0 = draw a host image into the framebuffer; 1 = read
 * the framebuffer back to host memory). Both directions are implemented,
 * in mgras_raster_dma_draw_image() and mgras_raster_dma_read_image().
 */
#define MGRAS_CMD_HDR_CLASS_DESC_20000 0x0008
#define MGRAS_CMD_HDR_CLASS_DESC_28000 0x000a
#define MGRAS_CMD_HDR_CLASS_DESC_30000 0x000c
#define MGRAS_DESC_20000_REG_SRC_OFF_LOW3 5
#define MGRAS_DESC_20000_REG_SRC_OFF_ALIGNED 6
#define MGRAS_DESC_20000_REG_PITCH 4
#define MGRAS_DESC_20000_REG_ROWS 7
#define MGRAS_DESC_20000_REG_BPR 8
#define MGRAS_DESC_20000_REG_KICK 11
#define MGRAS_DESC_20000_REG_LIST_EXECUTE 0x0c
#define MGRAS_DMA_KICK_DIRECTION_READ 0x08


#define MGRAS_RSS_BLOCKXYSTARTI 0x046
#define MGRAS_RSS_BLOCKXYENDI 0x047
#define MGRAS_RSS_PACKEDCOLOR 0x05b
#define MGRAS_RSS_RED 0x05c
#define MGRAS_RSS_CHAR_H 0x070
#define MGRAS_RSS_CHAR_L 0x071
#define MGRAS_RSS_BKGRD_RG 0x140
#define MGRAS_RSS_BKGRD_BA 0x141
#define MGRAS_RSS_PP1FILLMODE 0x161
#define MGRAS_RSS_COLORMASKMSBS 0x162


#define MGRAS_RSS_RE_POINT 0x040
#define MGRAS_RSS_RE_LINE 0x041
#define MGRAS_RSS_IR_ALIAS 0x045

/*
 * XYWIN carries a per-window screen origin: mgrasLineSS writes it before
 * RE_POINT/RE_LINE, but mgrasPolyFillRectSolid never writes it before
 * BLOCKXYSTARTI/BLOCKXYENDI (both confirmed by the sites each function's
 * disassembly touches). OpenBSD's impact(4) driver
 * (sys/arch/sgi/dev/impactreg.h) names this register IMPACTSR_CMD_XYWIN
 * and comments it "YX coords", built through an IMPACTSR_YXCOORDS(x, y)
 * macro expanding to (y << 16) | x -- the low/high 16-bit split below is
 * that public macro, not a capture-only guess. What the driver does not
 * state is the y field's sense (whether it counts from the window's top
 * or bottom); see mgras_apply_window_origin() for how a retained greeter
 * capture and the driver's own reset value settle that. See
 * mgras_draw_point() and mgras_draw_line() for how the origin applies.
 */
#define MGRAS_RSS_XYWIN 0x115

/*
 * The raster clip rectangle. The X server draws a primitive once for each
 * box of its window's clip list: before each pass it loads the box into
 * 0x147 and 0x148 and 0x11 into 0x14f, and after the last it loads 0 into
 * 0x14f (mgrasPolyPoint in the IRIX 6.5.30 impactsr.so). From its
 * arithmetic on the X box (x1, y1) to (x2, y2), end exclusive:
 *
 *   0x147 = x1 << 16 | (x2 - 1)
 *   0x148 = (H - y2) << 16 | (H - 1 - y1), H the screen height
 *
 * so both are inclusive ranges and 0x148 counts rows from the bottom, the
 * same sense as XYWIN's y field. That a pixel outside the box is not
 * written is inferred: the per-box redraw only makes sense if the
 * hardware drops what falls outside, and in a scripted desktop session
 * (run irix-20260927T102421-204445) all 99,540 clipped CHAR_L executes
 * (glyph rows and byte-transfer units) and 1,738 clipped IR_ALIAS
 * transfer heads meet their box under this reading and none under an
 * unflipped one. Line boxes hold after the XYWIN origin, so the
 * box is in screen coordinates. Ignoring it let the Icon Catalog's icon
 * shadows paint through the Background Setting panel stacked above them.
 * gxemul's BSD impactsr-bsd.h names 0x14f IMPACTSR_CMD_WINMODE without
 * giving its bits. Only 0 and 0x11 have been seen in it; which bit enables
 * the clip is not known, so clipping at exactly 0x11 is an approximation,
 * and any other value is logged and treated as off.
 */
#define MGRAS_RSS_CLIP_X 0x147
#define MGRAS_RSS_CLIP_Y 0x148
#define MGRAS_RSS_CLIP_MODE 0x14f
#define MGRAS_CLIP_MODE_ON 0x11


/*
 * Register 0x05f, IMPACTSR_CMD_ALPHA in OpenBSD's impactreg.h. No public
 * driver writes it, and its function is unknown.
 * The DDX writes it 33 times per pass with value 0 immediately before
 * the PIO draw-pixel row loop (mgrasDrawPixels..C, 0x577e68-0x577ee8)
 * in every observed capture. No read, branch or paint result has been
 * tied to it, so the name alone does not make it an alpha or colour
 * register here. Latched silently by the generic mgras_raster_rss_write()
 * store like every other unmodelled register.
 */
#define MGRAS_RSS_ALPHA 0x05f
#define MGRAS_RSS_COLORMASKLSBSA 0x163
#define MGRAS_RSS_GLINE_XSTARTF 0x00c
#define MGRAS_RSS_DRBPOINTERS 0x16d


#define MGRAS_RSS_COLORMASKLSBSB 0x164

/*
 * Rectangle-fill colour source, register offsets 0x176-0x178. No public
 * name is known for these three; the DDX only ever writes them
 * immediately before a BLOCKXYENDI fill execute. Under CI8 (PP1FILLMODE
 * bit 13 clear) only FILLCOLOR_R is read, as a bare 8-bit CMAP index in
 * its low byte. Under RGB (bit 13 set) all three are read, each holding
 * an 8-bit channel in its top byte: the channel value is the register
 * value >> 4.
 */
#define MGRAS_RSS_FILLCOLOR_R 0x176
#define MGRAS_RSS_FILLCOLOR_G 0x177
#define MGRAS_RSS_FILLCOLOR_B 0x178

/*
 * FILLMODE (OpenBSD impact(4) driver name) picks what a CHAR_H/CHAR_L
 * execute paints. The two values below are the ones a live capture
 * observed (see tests/data/mgras/char-modes.replay); any other value, or
 * an execute on the register that mode does not use, stays unimplemented
 * and inert.
 */
#define MGRAS_RSS_FILLMODE 0x110
#define MGRAS_FILLMODE_GLYPH_ROW 0x10400008
#define MGRAS_FILLMODE_BYTE_XFER 0x10c00000

/*
 * Byte-transfer mode's own register 0x159 picks which of two guest-side
 * packings a CHAR_L execute uses. Bit 7 (0x0080) selects two indexed
 * pixels per execute (the xdm chooser's own icons, see
 * tests/data/mgras/root-icon.replay); every other value, including the
 * desktop backdrop's 4x4 seed weave (0x159 == 0) and its icons (0x400,
 * 0x10000), takes the eight-pixel path (see
 * tests/data/mgras/char-modes.replay). Bits 16:14 are the source row's
 * padding in bytes (see mgras_draw_byte_row_bytes()) and do not change
 * which path runs.
 */
#define MGRAS_RSS_BYTE_XFER_PACKING 0x159
#define MGRAS_BYTE_XFER_PACKING_PAIRS 0x0080
#define MGRAS_BYTE_XFER_PACKING_PAD_SHIFT 14
#define MGRAS_BYTE_XFER_PACKING_PAD_MASK 0x7

/*
 * A BLOCKXYENDI execute paints a solid rectangle only when FILLMODE has
 * bit 20 set and bit 22 clear (MGRAS_FILLMODE_GLYPH_ROW, which sets both
 * bits, is a glyph-row latch and never a rectangle fill, so this mask
 * naturally excludes it). Any other FILLMODE value leaves the raster
 * unchanged; only the WID plane still tags.
 */
#define MGRAS_FILLMODE_RECT_FILL_MASK 0x00500000
#define MGRAS_FILLMODE_RECT_FILL_VALUE 0x00100000

/*
 * A primitive draws into the overlay planes when DRBPOINTERS holds the
 * overlay pointer word and COLORMASKMSBS enables planes in bits 6:4.
 * Inferred from captured IRIX 6.5.30 desktop sessions: every fill, glyph
 * row, byte transfer and line the server issues for a window on its
 * overlay visual (popup and Toolchest menus, XOR drag outlines) carries
 * DRBPOINTERS 0x101c0, COLORMASKMSBS 0x70 and COLORMASKLSBSA 0, and no
 * main-plane drawing carries that pointer word. How the hardware decodes
 * the pointer word's fields is unknown, so the whole word is compared.
 * The same word with COLORMASKMSBS 0xf0 and logic op CLEAR is how the
 * server erases an overlay window, over the whole screen at start-up and
 * over a popup's rectangle when it is taken down.
 */
#define MGRAS_DRBPOINTERS_OVERLAY 0x000101c0
#define MGRAS_COLORMASKMSBS_OVERLAY 0x70

/*
 * Stippled lines. The IRIX 6.5.30 server draws stippled spans and
 * polygons as one-row RE_LINEs with FILLMODE 0x20 or 0x60 and a 32-bit
 * pattern in register 0x15a, rewritten before each row. FILLMODE 0x60 also
 * loads BKGRD_RG/BKGRD_BA and 0x20 does not. The greeter's text cursor is
 * 0x20 with pattern 0x30c30c30: a 2-pixel I-beam stem with the field
 * colour showing on either side. Its icon bodies are 0x60 with PACKEDCOLOR
 * 0xaaaaaa, BKGRD white and patterns 0xaaaaaaaa and 0x55555555: a 170/255
 * checkerboard, which is what the RAD4 reference frame shows. So a set
 * pattern bit paints the line colour, a clear bit paints the background
 * under 0x40 and nothing under 0x20 alone.
 *
 * The pattern starts at the line's first pixel, bit 31 first. The server
 * aligns it itself: each of the greeter's 798 opaque stippled spans
 * carries 0xaaaaaaaa when its start x + y is even and 0x55555555 when it
 * is odd, which keeps the checkerboard fixed to the screen only if the
 * pattern restarts at each span. That parity is the server's stipple
 * origin, not a hardware rule: the desktop capture flips it from one
 * drawable's batch of spans to the next. Every stippled line in the captures is
 * horizontal and preceded by its own RE_POINT and pattern, so stepping
 * one bit per drawn pixel along a diagonal or vertical line, and
 * restarting at an RE_LINE chained from the previous one, are untested
 * assumptions.
 */
#define MGRAS_RSS_LINE_STIPPLE 0x15a
#define MGRAS_FILLMODE_STIPPLE 0x00000020
#define MGRAS_FILLMODE_STIPPLE_OPAQUE 0x00000040

typedef enum SGIMgrasRasterSubmissionForm {
    SGI_MGRAS_RASTER_DIRECT,
    SGI_MGRAS_RASTER_FIFO,
} SGIMgrasRasterSubmissionForm;

typedef struct SGIMgrasRasterSubmission {
    SGIMgrasRasterSubmissionForm form;
    uint32_t header;
    uint32_t reg;
    const uint32_t *payload;
    uint32_t payload_words;
} SGIMgrasRasterSubmission;

/* A new block origin resets the glyph and PIO cursors. */
static void mgras_raster_rss_write(SGIMgrasState *s, uint32_t reg,
                                uint32_t value);
/* Unimplemented execute registers log and leave state unchanged. */
static void mgras_raster_command_execute(
    SGIMgrasState *s, const SGIMgrasRasterSubmission *submission);
/* State payloads are one word; execute headers dispatch primitives. */
static void mgras_raster_command_fifo_dispatch(SGIMgrasState *s);


/*
 * A header's payload is its byte length rounded up to whole words: the
 * IRIX GL client writes glColor3ub as a 3-byte header and one packed word,
 * and glVertex3s as a 6-byte header and two words.
 */
static uint32_t mgras_command_payload_words(uint32_t payload_bytes)
{
    return DIV_ROUND_UP(payload_bytes, sizeof(uint32_t));
}

/* Frame an unsupported header's payload so the next word is a header. */
static void mgras_command_fifo_discard(SGIMgrasState *s,
                                       uint32_t payload_bytes)
{
    s->cmd_token_class = MGRAS_CMD_HDR_CLASS_DISCARD;
    s->cmd_token_payload_count = 0;
    s->cmd_token_words_remaining = mgras_command_payload_words(payload_bytes);
}

static void mgras_command_fifo_token(SGIMgrasState *s, uint32_t token)
{
    uint32_t cmd_class, reg, payload_bytes, true_class;

    if (s->cmd_token_words_remaining > 0) {
        if (s->cmd_token_class == SGI_MGRAS_CMD_TOKEN_GL_BLOCK) {
            s->cmd_token_words_remaining--;
            sgi_mgras_gl_block_word(s, token);
            return;
        }
        if (s->cmd_token_class != MGRAS_CMD_HDR_CLASS_DISCARD &&
            s->cmd_token_payload_count < SGI_MGRAS_CMD_TOKEN_PAYLOAD_WORDS) {
            s->cmd_token_payload[s->cmd_token_payload_count++] = token;
        }
        if (--s->cmd_token_words_remaining == 0 &&
            s->cmd_token_class != MGRAS_CMD_HDR_CLASS_DISCARD) {
            mgras_raster_command_fifo_dispatch(s);
        }
        return;
    }

    cmd_class = token >> 16;
    reg = (token >> 8) & 0xff;
    payload_bytes = token & 0xff;

    if (cmd_class == MGRAS_CMD_FIFO_FENCE_CLASS &&
        reg == MGRAS_CMD_FIFO_FENCE_REG && payload_bytes == 0) {
        s->hq_flags |= MGRAS_HQ_FLAG_FENCE;
        return;
    }
    if (cmd_class == MGRAS_CMD_FIFO_FENCE_CLASS &&
        reg == MGRAS_CMD_FIFO_DMA_READ_FENCE_REG && payload_bytes == 0) {
        return;
    }

    /*
     * Established (mgras-gl-commands.md, "Word framing"): the IRIX 6.5.30
     * OpenGL client forms these blocks for its GE table loads and batched
     * raster register writes, for example 0x80000214 for five descriptor
     * words and 128 data words. The class-0 opcode before the block gives
     * it its meaning; the model interprets glBitmap's image
     * (sgi_mgras_gl_block()) and consumes any other by length, so the
     * words after it parse as headers again. A block whose first byte is
     * not word aligned carries the byte's offset in bits 23:22 and one
     * word more.
     */
    if (token & MGRAS_CMD_HDR_INLINE_BLOCK) {
        if (!sgi_mgras_gl_block(s, token)) {
            mgras_command_fifo_discard(s,
                ((token & SGI_MGRAS_CMD_BLOCK_OFFSET) >> 22) +
                (token & MGRAS_CMD_HDR_INLINE_BYTES));
        }
        return;
    }

    if (cmd_class == MGRAS_CMD_HDR_CLASS_DESC_20000 ||
        cmd_class == MGRAS_CMD_HDR_CLASS_DESC_28000 ||
        cmd_class == MGRAS_CMD_HDR_CLASS_DESC_30000) {
        if (payload_bytes % sizeof(uint32_t)) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: descriptor command-FIFO "
                          "header, class 0x%04x reg 0x%02x has unaligned "
                          "payload %u bytes\n", cmd_class, reg, payload_bytes);
            mgras_command_fifo_discard(s, payload_bytes);
            return;
        }
        s->cmd_token_class = cmd_class;
        s->cmd_token_reg = reg;
        s->cmd_token_payload_count = 0;
        s->cmd_token_words_remaining =
            mgras_command_payload_words(payload_bytes);
        if (s->cmd_token_words_remaining >
            SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: command-FIFO header, "
                          "class 0x%04x reg 0x%02x payload %u bytes "
                          "exceeds %zu-byte limit\n", cmd_class, reg,
                          payload_bytes, MGRAS_CMD_TOKEN_LIMIT_BYTES);
            s->cmd_token_class = MGRAS_CMD_HDR_CLASS_DISCARD;
            return;
        }
        if (s->cmd_token_words_remaining == 0) {
            mgras_raster_command_fifo_dispatch(s);
        }
        return;
    }

    if (cmd_class == MGRAS_CMD_HDR_CLASS_HQ &&
        (reg == MGRAS_HQ_OP_QUERY || reg == MGRAS_HQ_OP_SWAP ||
         reg == MGRAS_HQ_OP_ATTR_QUERY ||
         reg == MGRAS_HQ_OP_HISTOGRAM_QUERY)) {
        s->cmd_token_class = MGRAS_CMD_HDR_CLASS_HQ;
        s->cmd_token_reg = reg;
        s->cmd_token_payload_count = 0;
        s->cmd_token_words_remaining =
            mgras_command_payload_words(payload_bytes);
        if (s->cmd_token_words_remaining >
            SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: HQ opcode 0x%02x payload "
                          "%u bytes exceeds %zu-byte limit\n", reg,
                          payload_bytes, MGRAS_CMD_TOKEN_LIMIT_BYTES);
            s->cmd_token_class = MGRAS_CMD_HDR_CLASS_DISCARD;
            return;
        }
        if (s->cmd_token_words_remaining == 0) {
            mgras_raster_command_fifo_dispatch(s);
        }
        return;
    }

    /*
     * The OpenGL packets the model executes (sgi_mgras_gl_header() and
     * mgras-gl-commands.md). The class and opcode are kept like any other
     * header's, so the payload collects and dispatches through the usual
     * path; the longest, glLoadMatrixf's, fills the 16-word buffer.
     */
    if (sgi_mgras_gl_header(token)) {
        s->cmd_token_class = cmd_class;
        s->cmd_token_reg = reg;
        s->cmd_token_payload_count = 0;
        s->cmd_token_words_remaining =
            mgras_command_payload_words(payload_bytes);
        if (s->cmd_token_words_remaining == 0) {
            mgras_raster_command_fifo_dispatch(s);
        }
        return;
    }

    true_class = cmd_class & ~1u;
    if (true_class == MGRAS_CMD_HDR_CLASS_STATE ||
        true_class == MGRAS_CMD_HDR_CLASS_EXEC) {
        if (payload_bytes % sizeof(uint32_t)) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: command-FIFO header, "
                          "class 0x%04x reg 0x%02x has unaligned payload "
                          "%u bytes\n", cmd_class, reg, payload_bytes);
            mgras_command_fifo_discard(s, payload_bytes);
            return;
        }
        s->cmd_token_class = true_class;
        s->cmd_token_reg = ((cmd_class & 1) << 8) | reg;
        s->cmd_token_payload_count = 0;
        s->cmd_token_words_remaining =
            mgras_command_payload_words(payload_bytes);
        if (s->cmd_token_words_remaining >
            SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: command-FIFO header, "
                          "class 0x%04x reg 0x%02x payload %u bytes "
                          "exceeds %zu-byte limit\n", cmd_class, reg,
                          payload_bytes, MGRAS_CMD_TOKEN_LIMIT_BYTES);
            s->cmd_token_class = MGRAS_CMD_HDR_CLASS_DISCARD;
            return;
        }
        if (s->cmd_token_words_remaining == 0) {
            mgras_raster_command_fifo_dispatch(s);
        }
        return;
    }

    qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented command-FIFO token, "
                  "class 0x%04x reg 0x%02x payload %u bytes\n", cmd_class,
                  reg, payload_bytes);

    /*
     * This class is not implemented, but its header still declares its
     * payload in the same framing as STATE/EXEC. Frame and discard it
     * instead of leaving the next FIFO word to be mis-parsed as a new
     * header: that mis-parse is what corrupted the rest of the burst
     * before this fix.
     */
    mgras_command_fifo_discard(s, payload_bytes);
    if (s->cmd_token_words_remaining > SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: command-FIFO token, class "
                      "0x%04x reg 0x%02x payload %u bytes exceeds %zu-byte "
                      "limit\n", cmd_class, reg, payload_bytes,
                      MGRAS_CMD_TOKEN_LIMIT_BYTES);
    }
}

/*
 * The kernel-privileged mapping port (0x130400) shares the class/reg/len
 * token framing above, but a class-0x48 header writes straight into a
 * flat register file instead of buffering a payload for dispatch: it
 * names a starting register, and payload_bytes/4 consecutive words are
 * stored there as they arrive. Five identical slots hold a DMA source
 * mapping: base pair (registers 0x20+2k/0x21+2k), page count (0x30+k),
 * index width (0x2b+k) and control (0x4b+k). Extra registers 0x2a, 0x3a
 * and 0x4a are written once per kernel sequence and are retained even
 * though their meaning is unresolved.
 */
#define MGRAS_DMA_MAP_CLASS 0x0048
#define MGRAS_DMA_MAP_CLOSE_CLASS 0x000e
#define MGRAS_DMA_MAP_CLOSE_REG 0x01
#define MGRAS_DMA_MAP_SLOT_BASE_HI 0x20
#define MGRAS_DMA_MAP_SLOT_BASE_LO 0x21
#define MGRAS_DMA_MAP_SLOT_WIDTH 0x2b
#define MGRAS_DMA_MAP_SLOT_COUNT 0x30
#define MGRAS_DMA_MAP_SLOT_CONTROL 0x4b

static void mgras_dma_map_token(SGIMgrasState *s, uint32_t token)
{
    uint32_t cmd_class, reg, payload_bytes;

    if (s->dma_map_words_remaining > 0) {
        if (s->dma_map_reg < SGI_MGRAS_DMA_MAP_REGS) {
            s->dma_map_regs[s->dma_map_reg] = token;
        }
        s->dma_map_reg++;
        s->dma_map_words_remaining--;
        return;
    }

    cmd_class = token >> 16;
    reg = (token >> 8) & 0xff;
    payload_bytes = token & 0xff;

    if (cmd_class == MGRAS_DMA_MAP_CLOSE_CLASS &&
        reg == MGRAS_DMA_MAP_CLOSE_REG && payload_bytes == 0) {
        /* Closes a kernel sequence on this port; accepted and ignored. */
        return;
    }

    if (payload_bytes % sizeof(uint32_t)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: DMA-mapping command-FIFO "
                      "header, class 0x%04x reg 0x%02x has unaligned "
                      "payload %u bytes\n", cmd_class, reg, payload_bytes);
        return;
    }

    if (cmd_class != MGRAS_DMA_MAP_CLASS || reg >= SGI_MGRAS_DMA_MAP_REGS) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented DMA-mapping "
                      "command-FIFO token, class 0x%04x reg 0x%02x "
                      "payload %u bytes\n", cmd_class, reg, payload_bytes);
        /*
         * Frame and discard the declared payload so the next word is not
         * mis-parsed as a header, mirroring mgras_command_fifo_token's
         * discard path above.
         */
        s->dma_map_reg = SGI_MGRAS_DMA_MAP_REGS;
        s->dma_map_words_remaining = payload_bytes / 4;
        return;
    }

    s->dma_map_reg = reg;
    s->dma_map_words_remaining = payload_bytes / 4;
}

void sgi_mgras_dma_map_write(SGIMgrasState *s, uint64_t value,
                              unsigned int size)
{
    if (size == 8) {
        mgras_dma_map_token(s, (uint32_t)(value >> 32));
        mgras_dma_map_token(s, (uint32_t)value);
    } else {
        mgras_dma_map_token(s, (uint32_t)value);
    }
}

uint64_t sgi_mgras_dma_map_slot_base(SGIMgrasState *s, unsigned int slot)
{
    if (slot >= SGI_MGRAS_DMA_MAP_SLOTS) {
        return 0;
    }
    return ((uint64_t)s->dma_map_regs[MGRAS_DMA_MAP_SLOT_BASE_HI + 2 * slot]
             << 32) | s->dma_map_regs[MGRAS_DMA_MAP_SLOT_BASE_LO + 2 * slot];
}

uint32_t sgi_mgras_dma_map_slot_count(SGIMgrasState *s, unsigned int slot)
{
    if (slot >= SGI_MGRAS_DMA_MAP_SLOTS) {
        return 0;
    }
    return s->dma_map_regs[MGRAS_DMA_MAP_SLOT_COUNT + slot];
}

uint32_t sgi_mgras_dma_map_slot_width(SGIMgrasState *s, unsigned int slot)
{
    if (slot >= SGI_MGRAS_DMA_MAP_SLOTS) {
        return 0;
    }
    return s->dma_map_regs[MGRAS_DMA_MAP_SLOT_WIDTH + slot];
}

uint32_t sgi_mgras_dma_map_slot_control(SGIMgrasState *s, unsigned int slot)
{
    if (slot >= SGI_MGRAS_DMA_MAP_SLOTS) {
        return 0;
    }
    return s->dma_map_regs[MGRAS_DMA_MAP_SLOT_CONTROL + slot];
}

void sgi_mgras_raster_command(SGIMgrasState *s, uint64_t word)
{
    uint32_t selector = (word >> 34) & 0xffffff;
    uint32_t value = word;
    uint32_t reg;

    if (s->cmd_token_words_remaining == 0) {

        bool state = (selector & MGRAS_CMD_SELECTOR_MASK) ==
                     MGRAS_CMD_SELECTOR_STATE;
        bool exec = (selector & MGRAS_CMD_SELECTOR_MASK) ==
                    MGRAS_CMD_SELECTOR_EXEC;

        if (state || exec) {
            SGIMgrasRasterSubmission submission = {
                .form = SGI_MGRAS_RASTER_DIRECT,
                .header = (uint32_t)(word >> 32),
                .reg = (selector >> 6) & 0x1ff,
                .payload = &value,
                .payload_words = 1,
            };

            reg = (selector >> 6) & 0x1ff;
            mgras_raster_rss_write(s, reg, value);
            if (exec) {
                mgras_raster_command_execute(s, &submission);
            }
            return;
        }
    }

    mgras_command_fifo_token(s, (uint32_t)(word >> 32));
    mgras_command_fifo_token(s, (uint32_t)word);
}

/*
 * Post-handshake 4-byte writes to the main port (0x120400) carry command-
 * FIFO tokens too: the kick header and the read path's payload word both
 * arrive this way. mgras_context_write() only handles the pre-ready handshake.
 */
void sgi_mgras_raster_command_token(SGIMgrasState *s, uint32_t token)
{
    mgras_command_fifo_token(s, token);
}

static uint32_t mgras_logic_op(uint8_t op, uint32_t dst, uint32_t src)
{
    switch (op & 0xf) {
    case 0x0: return 0;
    case 0x1: return src & dst;
    case 0x2: return src & ~dst;
    case 0x3: return src;
    case 0x4: return ~src & dst;
    case 0x5: return dst;
    case 0x6: return src ^ dst;
    case 0x7: return src | dst;
    case 0x8: return ~(src | dst);
    case 0x9: return ~(src ^ dst);
    case 0xa: return ~dst;
    case 0xb: return src | ~dst;
    case 0xc: return ~src;
    case 0xd: return ~src | dst;
    case 0xe: return ~(src & dst);
    default: return ~0u;
    }
}

static uint32_t mgras_paint_pixel(uint8_t op, uint32_t dst, uint32_t src,
                               uint32_t mask)
{
    uint32_t result = mgras_logic_op(op, dst, src) & 0x00ffffff;
    return (result & mask) | (dst & ~mask);
}

/*
 * PP1FILLMODE bit 13 picks whether a paint's source colour is a CI8
 * CMAP index or a packed RGB triple; see the FILLCOLOR_R/G/B comment
 * above. Painting code reads this at the moment of each paint and
 * records it in the per-pixel format plane (sgi-mgras.h) so scanout can
 * resolve mixed-format content within one WID.
 *
 * With bit 13 clear, bit 14 set and bits 10:8 zero the paint is a 12-bit
 * RGB window's instead. The IRIX 6.5.30 server draws its CI8 windows
 * under 0x4504 and its 24-bit ones under 0x6204. The Icon Catalog's icon
 * pane is a window on the server's 12-bit TrueColor visual (xwininfo:
 * depth 12, visual 0x29). The server gives it a display ID of its own
 * whose XMAP mode it writes as 0x405 when the window opens, clears it
 * under 0xc804 with COLORMASKLSBSA 0xffffff, and draws every fill, line
 * and glyph row in it under 0x4004 with COLORMASKLSBSA 0xfff (run
 * irix-20260927T035120-7dd191, seq 3803845-3829697). Its fill colours
 * are whole nibbles (FILLCOLOR 0x500/0x800/0xa00), which is what a
 * four-bit channel keeps of the 8-bit-in-12-bit registers. Read as CI8,
 * each of those paints stored index 0 and the pane showed black. What
 * the three fields mean on their own, and what bit 11 adds to the clear,
 * is not known; only these encodings are recognised. The server also
 * offers 12-bit PseudoColor visuals, which no capture draws to.
 */
#define MGRAS_PP1FILLMODE_RGB 0x2000
#define MGRAS_PP1FILLMODE_RGB12_MASK 0x6700
#define MGRAS_PP1FILLMODE_RGB12_VALUE 0x4000

static uint8_t mgras_pixel_format(SGIMgrasState *s)
{
    uint32_t pp1 = s->rss[MGRAS_RSS_PP1FILLMODE];

    if (pp1 & MGRAS_PP1FILLMODE_RGB) {
        return SGI_MGRAS_PIXEL_FORMAT_RGB;
    }
    if ((pp1 & MGRAS_PP1FILLMODE_RGB12_MASK) == MGRAS_PP1FILLMODE_RGB12_VALUE) {
        return SGI_MGRAS_PIXEL_FORMAT_RGB12;
    }
    return SGI_MGRAS_PIXEL_FORMAT_CI8;
}

/*
 * A 12-bit pixel keeps each 0x00RRGGBB channel's top four bits, red in
 * bits 3:0, green in bits 7:4 and blue in bits 11:8: the red, green and
 * blue masks 0xf, 0xf0 and 0xf00 the server's xdpyinfo reports for its
 * 12-bit TrueColor visuals, 0x29 and 0x2a (run
 * irix-20260927T045444-f4307c, where xwininfo gives the Icon Catalog's
 * pane depth 12 and visual 0x29). No capture reads a 12-bit pixel back,
 * so the masks are the only evidence of the stored layout.
 */
static uint32_t mgras_rgb12_from_rgb(uint32_t rgb)
{
    return ((rgb >> 20) & 0xf) | ((rgb >> 8) & 0xf0) | ((rgb << 4) & 0xf00);
}

/*
 * A paint's colour in the form its format stores: an index under CI8,
 * the 0x00RRGGBB triple under RGB, that triple's nibbles under RGB12.
 */
static uint32_t mgras_format_color(uint8_t format, uint32_t index,
                                   uint32_t rgb)
{
    switch (format) {
    case SGI_MGRAS_PIXEL_FORMAT_CI8:
        return index & 0xff;
    case SGI_MGRAS_PIXEL_FORMAT_RGB12:
        return mgras_rgb12_from_rgb(rgb);
    default:
        return rgb;
    }
}

/*
 * An RGB pixel word the X server passes whole -- PACKEDCOLOR, a pair-packed
 * CHAR_H/CHAR_L pixel, a 4-byte DMA pixel -- has red in bits 7:0,
 * 0x00BBGGRR. X.Org's impact driver (impact_shadow.c, visual masks
 * 0xff/0xff00/0xff0000), OpenBSD's impact(4) (rasops red at bit 0, passed
 * straight to PACKEDCOLOR) and the Linux Impact framebuffer patch (red at
 * offset 0, the same word in PACKEDCOLOR and in its image blits) all use
 * that layout. The IRIX 6.5.30 Icon Catalog's 12-bit pane agrees: the
 * soffice icon's stipples carry PACKEDCOLOR 0xf00000 and show blue on
 * the RAD4 reference, and nedit's carry 0xf0f0 and show yellow (run
 * irix-20260927T035120-7dd191). The model stores 0x00RRGGBB, so such a
 * word is swapped on the way in, and a 12-bit window's paint swaps it
 * before keeping its nibbles. FILLCOLOR_R/G/B and BKGRD are per-channel
 * registers and need no swap.
 */
static uint32_t mgras_rgb_from_pixel_word(uint32_t word)
{
    return ((word & 0xff) << 16) | (word & 0xff00) | ((word >> 16) & 0xff);
}

static bool mgras_clipped(SGIMgrasState *s, unsigned int idx)
{
    uint32_t cx = s->rss[MGRAS_RSS_CLIP_X];
    uint32_t cy = s->rss[MGRAS_RSS_CLIP_Y];
    unsigned int x = idx % s->width;
    unsigned int row = s->height - 1 - idx / s->width;

    if (s->rss[MGRAS_RSS_CLIP_MODE] != MGRAS_CLIP_MODE_ON) {
        return false;
    }
    return x < (cx >> 16) || x > (cx & 0xffff) ||
           row < (cy >> 16) || row > (cy & 0xffff);
}

/*
 * A paint writes colour and its format tag together. The WID plane keeps
 * whatever tagged it.
 */
static void mgras_put_pixel(SGIMgrasState *s, unsigned int idx, uint32_t color,
                         uint8_t op, uint32_t mask, uint8_t format)
{
    if (!(mask & 0x00ffffff) || mgras_clipped(s, idx)) {
        return;
    }
    s->front[idx] = mgras_paint_pixel(op, s->front[idx], color, mask);
    s->format[idx] = format;
}


static bool mgras_overlay_target(SGIMgrasState *s)
{
    return s->rss[MGRAS_RSS_DRBPOINTERS] == MGRAS_DRBPOINTERS_OVERLAY &&
           (s->rss[MGRAS_RSS_COLORMASKMSBS] & MGRAS_COLORMASKMSBS_OVERLAY);
}

/* Overlay glyph and line ink is an index, RED >> 12, as for CI8. */
static uint8_t mgras_overlay_ink(SGIMgrasState *s)
{
    return s->rss[MGRAS_RSS_RED] >> 12;
}

/*
 * The logic op applies to the overlay index as it does to colour. No
 * overlay plane mask is modeled: how COLORMASKMSBS bits map onto index
 * bits is unknown, and the server only ever enables all of them.
 */
static void mgras_put_overlay(SGIMgrasState *s, unsigned int idx,
                              uint8_t index, uint8_t op)
{
    if (mgras_clipped(s, idx)) {
        return;
    }
    s->overlay[idx] = mgras_logic_op(op, s->overlay[idx], index);
}


/* Normalize corners and clip. Returns false when nothing is visible. */
static bool mgras_rect_bounds(SGIMgrasState *s, uint32_t xy0, uint32_t xy1,
                              int *x_lo, int *y_lo, int *x_hi, int *y_hi)
{
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;

    *x_lo = MIN(x0, x1);
    *y_lo = MIN(y0, y1);
    if (*x_lo >= s->width || *y_lo >= s->height) {
        return false;
    }
    *x_hi = MIN(MAX(x0, x1), s->width - 1);
    *y_hi = MIN(MAX(y0, y1), s->height - 1);
    return true;
}


static void mgras_fill_rect(SGIMgrasState *s, uint32_t xy0, uint32_t xy1,
                         uint32_t color, uint8_t op, uint32_t mask,
                         uint8_t format)
{
    int x_lo, y_lo, x_hi, y_hi;

    if (!mgras_rect_bounds(s, xy0, xy1, &x_lo, &y_lo, &x_hi, &y_hi)) {
        return;
    }
    for (int y = y_lo; y <= y_hi; y++) {
        for (int x = x_lo; x <= x_hi; x++) {
            mgras_put_pixel(s, y * s->width + x, color, op, mask, format);
        }
    }
}


static void mgras_fill_overlay_rect(SGIMgrasState *s, uint32_t xy0,
                                    uint32_t xy1, uint8_t index, uint8_t op)
{
    int x_lo, y_lo, x_hi, y_hi;

    if (!mgras_rect_bounds(s, xy0, xy1, &x_lo, &y_lo, &x_hi, &y_hi)) {
        return;
    }
    for (int y = y_lo; y <= y_hi; y++) {
        for (int x = x_lo; x <= x_hi; x++) {
            mgras_put_overlay(s, y * s->width + x, index, op);
        }
    }
}


static void mgras_draw_fill(SGIMgrasState *s)
{
    uint32_t mask = s->rss[MGRAS_RSS_COLORMASKLSBSA];
    uint8_t op = s->rss[MGRAS_RSS_PP1FILLMODE] >> 26;
    uint8_t format = mgras_pixel_format(s);
    uint32_t color;

    /*
     * No fill writes a window ID: scanout takes each pixel's WID from the
     * VC3's display-ID tables (sgi-mgras-display.c). The fills that carry
     * a nonzero COLORMASKMSBS are overlay draws and clears, handled below,
     * and the server's two start-up fills with COLORMASKMSBS 0xff and
     * DRBPOINTERS 0x90240 (a clip-ID set-up and a plane clear) write planes
     * nothing here displays; they are not modeled.
     */
    if ((s->rss[MGRAS_RSS_FILLMODE] & MGRAS_FILLMODE_RECT_FILL_MASK) !=
        MGRAS_FILLMODE_RECT_FILL_VALUE) {
        return;
    }
    /* An overlay fill takes FILLCOLOR_R's index, as a CI8 fill does. */
    if (mgras_overlay_target(s)) {
        mgras_fill_overlay_rect(s, s->rss[MGRAS_RSS_BLOCKXYSTARTI],
                                s->rss[MGRAS_RSS_BLOCKXYENDI],
                                s->rss[MGRAS_RSS_FILLCOLOR_R], op);
    }

    /*
     * FILLCOLOR_R/G/B, not BKGRD_RG/PACKEDCOLOR: the DDX writes a
     * rectangle's colour there before every BLOCKXYENDI fill execute
     * (see the FILLCOLOR_R/G/B comment above). CI8 reads only the low
     * byte of FILLCOLOR_R as a bare CMAP index; RGB reads all three,
     * each >> 4 to an 8-bit channel.
     */
    color = mgras_format_color(format, s->rss[MGRAS_RSS_FILLCOLOR_R],
        (((s->rss[MGRAS_RSS_FILLCOLOR_R] >> 4) & 0xff) << 16) |
        (((s->rss[MGRAS_RSS_FILLCOLOR_G] >> 4) & 0xff) << 8) |
        ((s->rss[MGRAS_RSS_FILLCOLOR_B] >> 4) & 0xff));
    mgras_fill_rect(s, s->rss[MGRAS_RSS_BLOCKXYSTARTI], s->rss[MGRAS_RSS_BLOCKXYENDI],
                color, op, mask, format);
}


/*
 * FILLMODE_GLYPH_ROW paints one row of up to 64 columns per execute: CHAR_H
 * bits 31:0 are columns 0-31 and CHAR_L bits 31:0 are columns 32-63, both
 * MSB first, combined into one 64-bit row so either register's execute bit
 * can trigger the same paint. A CHAR_H-only execute (rows of 32 columns or
 * fewer) reads CHAR_L's possibly-stale value too, but the width clamp below
 * never indexes past column 31 in that case, so no stale bit is ever
 * painted. Only a set bit paints, its ink RED>>12 (CI8) or PACKEDCOLOR
 * (RGB); a clear bit leaves the pixel untouched rather than painting a
 * background colour.
 */
static void mgras_draw_glyph_row(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[MGRAS_RSS_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[MGRAS_RSS_BLOCKXYENDI];
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    uint8_t format = mgras_pixel_format(s);
    uint32_t ink = mgras_format_color(format, s->rss[MGRAS_RSS_RED] >> 12,
        mgras_rgb_from_pixel_word(s->rss[MGRAS_RSS_PACKEDCOLOR]));
    uint32_t mask = s->rss[MGRAS_RSS_COLORMASKLSBSA];
    uint8_t op = s->rss[MGRAS_RSS_PP1FILLMODE] >> 26;
    uint64_t bits = ((uint64_t)s->rss[MGRAS_RSS_CHAR_H] << 32) |
                    s->rss[MGRAS_RSS_CHAR_L];
    bool overlay = mgras_overlay_target(s);
    int y = y0 + s->glyph_row;
    int width;

    if (x0 > x1 || y0 > y1 || x0 >= s->width || y0 >= s->height) {
        return;
    }
    x1 = MIN(x1, s->width - 1);
    y1 = MIN(y1, s->height - 1);
    s->glyph_row++;
    if (y > y1 || y >= s->height) {
        return;
    }
    width = MIN(x1 - x0 + 1, 64);
    for (int i = 0; i < width; i++) {
        bool set = (bits >> (63 - i)) & 1;

        if (set) {
            mgras_put_pixel(s, y * s->width + x0 + i, ink, op, mask,
                        format);
            if (overlay) {
                mgras_put_overlay(s, y * s->width + x0 + i,
                                  mgras_overlay_ink(s), op);
            }
        }
    }
}

/*
 * FILLMODE_BYTE_XFER delivers eight 8-bit indexed pixels per CHAR_L execute
 * (CHAR_H is staged first and never executes itself): CHAR_H's four bytes
 * MSB to LSB, then CHAR_L's four, painted left to right. The server sends
 * each row as the 8-byte-aligned words of its source buffer that cover
 * it, so a row's first unit can start with bytes of the previous row
 * rather than pixel 0, and the row only advances once enough units have
 * covered its width. `skip` is that row's offset from the unit boundary,
 * (row * pitch) % 8, where the source pitch is the width plus 0x159 bits
 * 16:14. That field is inferred. What settles the pitch is the
 * greeter's text-cursor restore (6 pixels wide, field 6): its buffer is
 * filled by a save-under DMA read whose descriptor pitch is 12, and its
 * restored field colour lands in place only at pitch 12. The execute
 * count alone does not decide it. Beyond that, width + field predicts the
 * execute count of every one of 1,445 eight-pixel transfers in the
 * greeter and desktop captures (tests/data/mgras/char-modes.replay among
 * them), where the width rounded up to 32 bits fails the 68 cursor
 * restores and the bare width fails two. Painted bytes are always CI8 CMAP
 * indices regardless of PP1FILLMODE: this path carries its own 8-bit index
 * data rather than a register-selected colour, and no capture shows it
 * under RGB, so there is no RGB form to select between.
 * mgras_draw_byte_row_pairs() differs.
 */
static void mgras_draw_byte_row_bytes(SGIMgrasState *s, int x0, int x1,
                                   int y0, int y1, uint32_t mask, uint8_t op)
{
    uint32_t char_h = s->rss[MGRAS_RSS_CHAR_H];
    uint32_t char_l = s->rss[MGRAS_RSS_CHAR_L];
    uint8_t bytes[8] = {
        char_h >> 24, char_h >> 16, char_h >> 8, char_h,
        char_l >> 24, char_l >> 16, char_l >> 8, char_l,
    };
    int width = x1 - x0 + 1;
    int row = s->glyph_row;
    int y = y0 + row;
    int pitch = width + ((s->rss[MGRAS_RSS_BYTE_XFER_PACKING] >>
                          MGRAS_BYTE_XFER_PACKING_PAD_SHIFT) &
                         MGRAS_BYTE_XFER_PACKING_PAD_MASK);
    int skip = (row * pitch) % 8;
    int units_per_row = DIV_ROUND_UP(skip + width, 8);

    if (y <= y1 && y < s->height) {
        for (int i = 0; i < 8; i++) {
            int col = s->glyph_unit * 8 + i - skip;

            if (col < 0 || col >= width || x0 + col >= s->width) {
                continue;
            }
            mgras_put_pixel(s, y * s->width + x0 + col, bytes[i], op,
                         mask, SGI_MGRAS_PIXEL_FORMAT_CI8);
            if (mgras_overlay_target(s)) {
                mgras_put_overlay(s, y * s->width + x0 + col, bytes[i],
                                  op);
            }
        }
    }
    s->glyph_unit++;
    if (s->glyph_unit >= units_per_row) {
        s->glyph_unit = 0;
        s->glyph_row++;
    }
}

/*
 * The chooser icons' own packing (register 0x159 == 0x0080): each CHAR_L
 * execute delivers two pixels rather than eight, pixel 0 from CHAR_H and
 * pixel 1 from CHAR_L. Under CI8 (PP1FILLMODE bit 13 clear) each is a CMAP
 * index in bits 7:0; a live capture's other bytes did not reliably
 * replicate that value, so only bits 7:0 are read
 * (tests/data/mgras/root-icon.replay). Under RGB (bit 13 set) each register
 * carries a whole 0x00BBGGRR pixel word (see mgras_rgb_from_pixel_word()):
 * the IRIX 6.5.30 greeter seeds its list
 * background this way with PP1FILLMODE 0xc006204 and CHAR_H/CHAR_L
 * 0x00c0c0c0/0x00c1c1c1, a grey dither, and then replicates it with
 * 4-byte-per-pixel DMA copies that must read those 24 bits back. In the
 * greeter and desktop captures every pair-packed transfer runs under RGB;
 * the CI8 form rests on the fixture above alone. The pixels carry the
 * same format tag as every other PP1FILLMODE-driven paint; a 12-bit
 * window keeps each word's nibbles as it does PACKEDCOLOR's, which no
 * capture exercises. ceil(width / 2) executes cover a row, no skip, and
 * the row advances after the last unit.
 */
static void mgras_draw_byte_row_pairs(SGIMgrasState *s, int x0, int x1,
                                   int y0, int y1, uint32_t mask, uint8_t op)
{
    uint8_t format = mgras_pixel_format(s);
    uint32_t pixels[2] = {
        mgras_format_color(format, s->rss[MGRAS_RSS_CHAR_H],
            mgras_rgb_from_pixel_word(s->rss[MGRAS_RSS_CHAR_H])),
        mgras_format_color(format, s->rss[MGRAS_RSS_CHAR_L],
            mgras_rgb_from_pixel_word(s->rss[MGRAS_RSS_CHAR_L])),
    };
    int width = x1 - x0 + 1;
    int row = s->glyph_row;
    int y = y0 + row;
    int units_per_row = DIV_ROUND_UP(width, 2);

    if (y <= y1 && y < s->height) {
        for (int i = 0; i < 2; i++) {
            int col = s->glyph_unit * 2 + i;

            if (col >= width || x0 + col >= s->width) {
                continue;
            }
            mgras_put_pixel(s, y * s->width + x0 + col, pixels[i], op,
                         mask, format);
            /*
             * Every captured overlay draw uses CI8 (the register file
             * exposes no overlay-index derivation for a packed RGB pixel,
             * and no capture shows this path targeting the overlay under
             * RGB). pixels[i] is masked to 0xff under CI8 above, so it is
             * a valid index here; skip the overlay write under RGB rather
             * than store a truncated colour byte as an invented index.
             */
            if (mgras_overlay_target(s) &&
                format == SGI_MGRAS_PIXEL_FORMAT_CI8) {
                mgras_put_overlay(s, y * s->width + x0 + col,
                                  pixels[i], op);
            }
        }
    }
    s->glyph_unit++;
    if (s->glyph_unit >= units_per_row) {
        s->glyph_unit = 0;
        s->glyph_row++;
    }
}

/*
 * Byte-transfer mode's packing is register 0x159, not FILLMODE; see the
 * MGRAS_RSS_BYTE_XFER_PACKING comment above.
 */
static void mgras_draw_byte_row(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[MGRAS_RSS_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[MGRAS_RSS_BLOCKXYENDI];
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    uint32_t mask = s->rss[MGRAS_RSS_COLORMASKLSBSA];
    uint8_t op = s->rss[MGRAS_RSS_PP1FILLMODE] >> 26;
    uint32_t packing = s->rss[MGRAS_RSS_BYTE_XFER_PACKING];

    if (x0 > x1 || y0 > y1 || x0 >= s->width || y0 >= s->height) {
        return;
    }
    x1 = MIN(x1, s->width - 1);
    y1 = MIN(y1, s->height - 1);

    /*
     * The real xdm chooser ORs a row-padding field (bits 16:14) into this
     * word alongside the packing selector bit, e.g. 0x00010080 or
     * 0x00018000 rather than the bare selector value (blockcopy-grammar.md).
     * Test the selector bit, not the whole register, so those padded
     * values still match their packing mode. Bit 7 set is the only value
     * that selects the pair packing; every other value, including 0 (the
     * desktop backdrop's seed weave) and values with neither bit 15 nor
     * bit 7 set, takes the eight-pixel path rather than being dropped.
     */
    if (packing & MGRAS_BYTE_XFER_PACKING_PAIRS) {
        mgras_draw_byte_row_pairs(s, x0, x1, y0, y1, mask, op);
    } else {
        mgras_draw_byte_row_bytes(s, x0, x1, y0, y1, mask, op);
    }
}

/* Route a CHAR_H/CHAR_L execute by FILLMODE; anything else stays inert. */
static void mgras_char_execute(SGIMgrasState *s, uint32_t reg)
{
    uint32_t mode = s->rss[MGRAS_RSS_FILLMODE];

    if (mode == MGRAS_FILLMODE_GLYPH_ROW) {
        mgras_draw_glyph_row(s);
        s->dirty = true;
        return;
    }
    if (mode == MGRAS_FILLMODE_BYTE_XFER && reg == MGRAS_RSS_CHAR_L) {
        mgras_draw_byte_row(s);
        s->dirty = true;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented CHAR execute, "
                  "register 0x%03x, fillmode 0x%08x\n", reg, mode);
}


/* BKGRD_BA execute paints two pixels and advances the persistent cursor. */
static void mgras_draw_pio_pixel_pair(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[MGRAS_RSS_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[MGRAS_RSS_BLOCKXYENDI];
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    uint32_t pixels[2] = {
        s->rss[MGRAS_RSS_BKGRD_RG] & 0x00ffffff,
        s->rss[MGRAS_RSS_BKGRD_BA] & 0x00ffffff,
    };
    uint32_t mask = s->rss[MGRAS_RSS_COLORMASKLSBSA];
    uint8_t op = s->rss[MGRAS_RSS_PP1FILLMODE] >> 26;
    uint8_t format = mgras_pixel_format(s);

    /*
     * No capture sends PIO pixel pairs to a 12-bit window, and which
     * channel order these words carry there is unknown, so the pair is
     * reported rather than stored as a guess.
     */
    if (format == SGI_MGRAS_PIXEL_FORMAT_RGB12) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented PIO pixel pair "
                      "in a 12-bit window, PP1FILLMODE 0x%08x\n",
                      s->rss[MGRAS_RSS_PP1FILLMODE]);
        return;
    }
    if (x0 > x1 || y0 > y1 || x0 >= s->width || y0 >= s->height) {
        return;
    }
    x1 = MIN(x1, s->width - 1);
    y1 = MIN(y1, s->height - 1);

    for (int i = 0; i < 2; i++) {
        int x = x0 + s->pio_x;
        int y = y0 + s->pio_y;

        if (x > x1 || y > y1 || x >= s->width ||
            y >= s->height) {
            return;
        }
        mgras_put_pixel(s, y * s->width + x, pixels[i], op, mask, format);
        s->pio_x++;
        if (x0 + s->pio_x > x1) {
            s->pio_x = 0;
            s->pio_y++;
        }
    }
}


/*
 * XYWIN's x field (low 16 bits) gates whether its (x, y) is a live window
 * origin to add to a point/line's own coordinate. A retained greeter
 * capture's RE_LINE executes split cleanly on this: 1,456 of 1,953 carry
 * XYWIN == 0x03ff0000 (x field 0) and already paint the icon-panel
 * outlines at their correct absolute position -- one 500-line run of
 * these (register 0x14f == 0x11) draws its box at x:459-519, y:700-765,
 * and adding XYWIN's y field to it would push it off-window. The other
 * eight observed XYWIN values all carry a nonzero x field, and adding
 * both fields lands each of those clusters inside a distinct,
 * non-overlapping icon-sized box. Gating on x != 0 is an inference from
 * that split, not a confirmed hardware or driver condition: disassembly
 * of mgrasLineSS finds two distinct sites that write XYWIN ahead of
 * RE_POINT/RE_LINE, one that structurally always stores x == 0 and one
 * that can store a genuine nonzero x, but which guest-side condition
 * chooses between them was not traced to a confirmed branch predicate.
 * mgrasPolyFillRectSolid never writes XYWIN at all, which is why
 * BLOCKXYSTARTI/BLOCKXYENDI fills need no such adjustment.
 *
 * The y field is packed top-down (0 at the bottom, s->height - 1
 * at the top of the window), the opposite sense from the framebuffer's
 * own top-down row order: adding it to a line/point's raw y unmodified
 * put the greeter's icon line art roughly 800 pixels too far down, past
 * the bottom of the window it was meant to land in. Flipping the field
 * with s->height - 1 - oy before adding lines it up with the
 * measured on-screen geometry: XYWIN 0x03390146 (the capture's largest
 * cluster, 379 RE_LINE executes, raw y in [16, 113]) lands at
 * [214, 311], matching the EZsetup icon's own measured rect (y 214-312,
 * see test_mgras_greeter.py) almost exactly, and the panel-outline
 * cluster (XYWIN 0x033c0143, raw y in [0, 476]) lands at [195, 671]
 * against the panel's own measured 201-666 -- the same few-pixel margin
 * that test's docstring attributes to the drawn border sitting just
 * outside the fill's own measured, inset bounding box. A bounded,
 * truncated replay of the same capture through this corrected formula
 * (up to and including both clusters) reproduces the greeter's icon row
 * -- labelled root/EZsetup/demos icons sitting together at the expected
 * position -- rather than the pre-fix ghost cluster near the origin or
 * the pre-fix placement roughly 800 pixels further down.
 *
 * The flip's magnitude (s->height - 1, not some other constant)
 * is not capture-only: OpenBSD's impact(4) driver initializes XYWIN
 * exactly once, to IMPACTSR_YXCOORDS(0, 0x3ff) -- x field 0, y field
 * 0x3ff (1023, s->height - 1) -- and 1,456 of the capture's 1,953
 * RE_LINE executes carry that same reset value, 0x03ff0000, while
 * drawing at their own already-correct absolute position (see the x != 0
 * gate above). That reset value is exactly the flip's fixed point --
 * s->height - 1 - 0x3ff == 0 -- which is why leaving x's own gate
 * on ox != 0 already excludes it: at oy == 0x3ff the corrected y term is
 * a no-op even if it were applied unconditionally. The driver never
 * exercises a nonzero-x window itself, so it settles the fixed point and
 * the field split, not the general in-window transform; the transform
 * for a genuine nonzero window remains grounded in the capture alone.
 */
static void mgras_apply_window_origin(SGIMgrasState *s, int *x, int *y)
{
    uint32_t xywin = s->rss[MGRAS_RSS_XYWIN];
    int ox = xywin & 0xffff;
    int oy = (xywin >> 16) & 0xffff;

    if (ox != 0) {
        *x += ox;
        *y += (s->height - 1) - oy;
    }
}

/*
 * The driver never writes an oy past s->height - 1 (see the reset
 * value above), but nothing here enforces that: OpenBSD's impactreg.h packs
 * XYWIN as IMPACTSR_YXCOORDS(x, y) == (y << 16) | x with no documented width
 * narrower than the 16-bit field already masked above, so a guest is free to
 * point the register anywhere in that range. An oy past 1023 drives
 * mgras_apply_window_origin()'s y term negative, and a plain
 * "x >= s->width || y >= s->height" rejection only ever
 * checks an upper bound: it has no "x < 0 || y < 0" term, so a negative x
 * or y sails through both sides of the OR. mgras_put_pixel()'s idx
 * parameter is unsigned, so a negative y * s->width + x then wraps
 * into a multi-gigabyte forward offset into s->front. Casting each
 * coordinate to unsigned before comparing rejects negative and over-large
 * values in the same check, without changing anything for an in-range
 * point.
 */
static bool mgras_xy_on_screen(SGIMgrasState *s, int x, int y)
{
    return (unsigned int)x < s->width &&
           (unsigned int)y < s->height;
}

/*
 * RE_POINT paints at its own command value, not at BLOCKXYSTARTI: nothing
 * in the retained Toolchest capture writes BLOCKXYSTARTI before an
 * RE_POINT execute, and every one of its 12 point executes there instead
 * carries the drawn coordinate in the low 32 bits of the command word,
 * which sgi_mgras_raster_command() already stores at
 * rss[MGRAS_RSS_RE_POINT] like any other register (see
 * mgras_raster_rss_write()). That latched value also
 * serves as RE_LINE's start point below: RE_POINT is a moveto that leaves
 * a pen position for a following RE_LINE to draw from. The window origin
 * in XYWIN (see mgras_apply_window_origin()) applies before the bounds
 * check; the Toolchest capture's XYWIN stays at its x-field-0 reset value
 * throughout, so this is unexercised there and rests on the greeter
 * evidence instead.
 */
static void mgras_draw_point(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[MGRAS_RSS_RE_POINT];
    uint8_t format = mgras_pixel_format(s);
    uint32_t color = mgras_format_color(format, s->rss[MGRAS_RSS_RED] >> 12,
        mgras_rgb_from_pixel_word(s->rss[MGRAS_RSS_PACKEDCOLOR]));
    uint32_t mask = s->rss[MGRAS_RSS_COLORMASKLSBSA];
    uint8_t op = s->rss[MGRAS_RSS_PP1FILLMODE] >> 26;
    int x = (xy0 >> 16) & 0xffff, y = xy0 & 0xffff;

    mgras_apply_window_origin(s, &x, &y);
    if (!mgras_xy_on_screen(s, x, y)) {
        return;
    }
    mgras_put_pixel(s, y * s->width + x, color, op, mask, format);
    if (mgras_overlay_target(s)) {
        mgras_put_overlay(s, y * s->width + x, mgras_overlay_ink(s), op);
    }
}


/*
 * An opaque stipple's background. Under CI8 the server loads BKGRD_RG with
 * the bare index. Under RGB the model reads red from BKGRD_RG bits 11:4,
 * green from bits 23:16 and blue from BKGRD_BA bits 11:4, the 8-bit-in-12-
 * bit packing of FILLCOLOR_R/G/B. Only white has been seen here, so the
 * channel assignment is inferred; OpenBSD's impact(4) impact_putchar()
 * packs its opaque-text background the same way (red's top nibble at
 * BKGRD_RG bits 11:8, green at bits 23:16, blue's at BKGRD_BA bits 11:8).
 */
static uint32_t mgras_stipple_background(SGIMgrasState *s, uint8_t format)
{
    uint32_t rg = s->rss[MGRAS_RSS_BKGRD_RG];

    return mgras_format_color(format, rg,
                              (((rg >> 4) & 0xff) << 16) |
                              (((rg >> 16) & 0xff) << 8) |
                              ((s->rss[MGRAS_RSS_BKGRD_BA] >> 4) & 0xff));
}

/*
 * RE_LINE's own command value is the line's end point, not BLOCKXYENDI,
 * and its start is the pen position the previous RE_POINT or RE_LINE left
 * at rss[MGRAS_RSS_RE_POINT], not BLOCKXYSTARTI: of the 2,862 RE_LINE
 * executes in the retained Toolchest capture, 2,861 draw from exactly the
 * previous RE_POINT/RE_LINE's own end point to this command's own value,
 * tracing closed button and glyph-drawer outlines (the one exception is
 * the capture's first line event, whose true predecessor point falls
 * outside the recording window). BLOCKXYSTARTI/BLOCKXYENDI instead sit
 * frozen on a stale, unrelated rectangle across thousands of consecutive
 * point/line draws there. Advancing rss[MGRAS_RSS_RE_POINT] to this line's
 * raw (unshifted) end point chains a run of RE_LINEs into a polyline the
 * same way the capture chains them; each chained execute reapplies
 * mgras_apply_window_origin() to that raw value itself, so a steady XYWIN
 * across the chain shifts every leg by the same amount. The Toolchest
 * capture's XYWIN stays at its x-field-0 reset value throughout, so the
 * origin shift below is unexercised there and rests on the greeter
 * evidence in mgras_apply_window_origin() instead.
 */
static void mgras_draw_line(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[MGRAS_RSS_RE_POINT];
    uint32_t xy1 = s->rss[MGRAS_RSS_RE_LINE];
    uint8_t format = mgras_pixel_format(s);
    uint32_t color = mgras_format_color(format, s->rss[MGRAS_RSS_RED] >> 12,
        mgras_rgb_from_pixel_word(s->rss[MGRAS_RSS_PACKEDCOLOR]));
    uint32_t fillmode = s->rss[MGRAS_RSS_FILLMODE];
    bool stipple = fillmode & MGRAS_FILLMODE_STIPPLE;
    bool opaque = fillmode & MGRAS_FILLMODE_STIPPLE_OPAQUE;
    uint32_t pattern = s->rss[MGRAS_RSS_LINE_STIPPLE];
    uint32_t background = mgras_stipple_background(s, format);
    uint32_t mask = s->rss[MGRAS_RSS_COLORMASKLSBSA];
    uint8_t op = s->rss[MGRAS_RSS_PP1FILLMODE] >> 26;
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    int dx, dy, sx, sy, err;
    unsigned int step = 0;
    bool overlay;

    mgras_apply_window_origin(s, &x0, &y0);
    mgras_apply_window_origin(s, &x1, &y1);
    dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx - dy;
    overlay = mgras_overlay_target(s);

    for (;;) {
        int e2;

        if (mgras_xy_on_screen(s, x0, y0)) {
            bool set = !stipple || ((pattern >> (31 - (step & 31))) & 1);

            if (set) {
                mgras_put_pixel(s, y0 * s->width + x0, color, op, mask,
                            format);
                if (overlay) {
                    mgras_put_overlay(s, y0 * s->width + x0,
                                      mgras_overlay_ink(s), op);
                }
            } else if (opaque) {
                /*
                 * No capture draws a stippled line into the overlay, so
                 * only the main planes take the background.
                 */
                mgras_put_pixel(s, y0 * s->width + x0, background, op,
                            mask, format);
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        step++;
        e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
    s->rss[MGRAS_RSS_RE_POINT] = xy1;
}

static void mgras_raster_command_execute(
    SGIMgrasState *s, const SGIMgrasRasterSubmission *submission)
{
    switch (submission->reg) {
    case MGRAS_RSS_RE_POINT:
        mgras_draw_point(s);
        s->dirty = true;
        return;
    case MGRAS_RSS_RE_LINE:
        mgras_draw_line(s);
        s->dirty = true;
        return;
    case MGRAS_RSS_BLOCKXYENDI:
        mgras_draw_fill(s);
        s->dirty = true;
        return;
    case MGRAS_RSS_IR_ALIAS:
        if (submission->form == SGI_MGRAS_RASTER_FIFO) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented command-FIFO "
                          "execute, register 0x%03x\n", submission->reg);
            return;
        }
        /*
         * A direct-command IR_ALIAS execute paints nothing. The DMA and
         * PIO paths that deliver its pixels (mgras_raster_dma_draw_image(),
         * mgras_draw_pio_pixel_pair()) already paint their own data; this
         * execute only latches the register write mgras_raster_rss_write()
         * already applied. This is established behaviour, not an
         * unimplemented command, so it is not logged.
         */
        return;
    case MGRAS_RSS_CHAR_H:
    case MGRAS_RSS_CHAR_L:
        mgras_char_execute(s, submission->reg);
        return;
    case MGRAS_RSS_BKGRD_BA:
        mgras_draw_pio_pixel_pair(s);
        s->dirty = true;
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented direct-command "
                      "execute, register 0x%03x\n", submission->reg);
        return;
    }
}


static void mgras_raster_rss_write(SGIMgrasState *s, uint32_t reg,
                                    uint32_t value)
{
    s->rss[reg] = value;
    if (reg == MGRAS_RSS_CLIP_MODE && value != 0 &&
        value != MGRAS_CLIP_MODE_ON) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: clip mode 0x%x not modelled, "
                      "clip off\n", value);
    }
    if (reg == MGRAS_RSS_BLOCKXYSTARTI) {

        s->glyph_row = 0;
        s->glyph_unit = 0;
        s->pio_x = s->pio_y = 0;
    }
}


/*
 * The kick byte (0x20000 register 11, low byte) names the DMA-mapping
 * slot (0x130400's class-0x48 register file) directly: bits 2:1 are the
 * slot index and bit 3 the transfer direction. The DDX keeps a
 * long-lived slot 0 mapping for its small-copy bounce buffer alongside a
 * per-transfer slot 3 mapping for larger copies, both open at once, so a
 * scan for "the only mapped slot" picks the wrong one whenever both are
 * live. Log and take no transfer if the named slot has no mapped pages.
 */
static bool mgras_raster_dma_select_slot(SGIMgrasState *s, uint32_t kick_byte,
                                      unsigned int *out_slot)
{
    unsigned int slot = (kick_byte >> 1) & 3;

    if (sgi_mgras_dma_map_slot_count(s, slot) == 0) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: DMA image transfer kicked "
                      "(kick byte 0x%02x) naming slot %u, which has no "
                      "mapped pages\n", kick_byte, slot);
        return false;
    }
    *out_slot = slot;
    return true;
}

/*
 * Host-memory source address formula from DMA-source captures:
 * va = addr_low + addr_high +
 * row*pitch + byte, page_ix = va >> 12, and the source page's physical
 * address is (list_node << 32) | (pagelist[page_ix] << 12) | (va & 0xfff).
 * The page list itself is `count` big-endian 64-bit page-frame-number
 * entries at (list_node << 32) | list_pa, both taken from the selected
 * mapping slot. Consecutive page-list entries need not be adjacent, so a
 * row's bytes can span a page boundary at an arbitrary offset: the shared
 * DMA code cuts every transfer at each guest page and resolves each page
 * through the list.
 *
 * A page-frame number wider than 20 bits would collide with the node
 * identifier this formula ORs in at bit 32 (PFN << 12 would reach bit 32
 * at PFN 0x100000): 20 bits is both the formula's own ceiling and what a
 * node's up-to-4-GiB RAM needs for a 4 KiB page (2**32 / 2**12 = 2**20
 * frames). The observed captures only exercise page-frame numbers under
 * 0x4000 (64 MiB/node) and show every entry's top six bytes zero, which
 * is consistent with a plain PFN field carrying no flag bits -- unlike
 * the unrelated DMA-map base pair, whose low word packs flags into bits
 * 2:0 -- but does not by itself distinguish a 16-bit field from a wider
 * one. Mask to the formula's 20-bit ceiling rather than guess narrower.
 */
#define MGRAS_DMA_PAGE_LIST_PFN_MASK 0xfffffu
#define MGRAS_DMA_PAGE_SIZE 4096

/*
 * One transfer's view of the bus for the shared DMA hooks. The widget
 * comes from the kicked slot's control word, so it travels with the
 * transfer rather than living in device state.
 */
typedef struct MgrasDmaBus {
    SGIMgrasState *s;
    unsigned int widget;
} MgrasDmaBus;

/*
 * The shared code hands over plain node addresses; the XIO packet fabric
 * reaches them with the slot's widget composed in.
 */
static bool mgras_raster_dma_address(void *opaque, uint64_t addr, hwaddr *bus)
{
    MgrasDmaBus *b = opaque;

    return sgi_mgras_xio_compose(b->s, b->widget, addr, bus);
}

/*
 * A replayed session has no guest RAM behind it, so a traced capture must
 * carry the bytes a host-to-screen DMA image transfer reads: the page-list
 * fetch and the pixel source. The shared code calls this once per chunk it
 * resolves -- at most SGI_DMA_CHUNK_MAX bytes, never crossing a guest page
 * -- at the plain node address before mgras_raster_dma_address() composed
 * it. The node address, not the fabric address, is what a replay's own
 * qtest_memwrite can actually preload: qtest's guest RAM has no
 * widget-shifted region behind it. mgras_write() has already incremented
 * and traced s->trace_sequence for the write that triggered this transfer,
 * so the transfer is submitted with that write's sequence number
 * (trace_sequence - 1): Origami's reduction places chunk records
 * immediately before the write they belong to.
 */
static void mgras_raster_dma_chunk(void *opaque, uint64_t seq, uint64_t addr,
                                   const uint8_t *bytes, uint32_t len)
{
    g_autofree char *hex = g_malloc(len * 2 + 1);

    qemu_hexdump_to_buffer(hex, len * 2 + 1, bytes, len);
    trace_sgi_mgras_dma_chunk(seq, addr, len, hex);
}

/*
 * A transfer runs to completion inside the kick that started it, and
 * mgras_raster_dma_kick() raises the fence flag the driver polls, whether
 * or not the transfer moved anything. The completion adds nothing to that.
 */
static void mgras_raster_dma_publish(void *opaque, const SGICompletion *c)
{
}

static const SGIHeadOps mgras_raster_dma_ops = {
    .publish = mgras_raster_dma_publish,
    .dma_address = mgras_raster_dma_address,
    .dma_chunk = mgras_raster_dma_chunk,
};

/*
 * Resolve the kicked slot's mapping: its bus, and its page list read from
 * guest memory as device page addresses. On success, *page_addr is a
 * freshly allocated `count`-entry array the caller owns. On an unusable
 * mapping or a failed read, this has already logged why and the caller
 * must stop the transfer rather than paint or write anything.
 */
static bool mgras_raster_dma_setup(SGIMgrasState *s, unsigned int slot,
                                   MgrasDmaBus *ctx, SGIDMABus *bus,
                                   SGIDMAPageList *pl, uint64_t **page_addr)
{
    uint64_t base = sgi_mgras_dma_map_slot_base(s, slot);
    uint32_t list_node = (uint32_t)(base >> 32) & 0xff;
    uint32_t count = sgi_mgras_dma_map_slot_count(s, slot);
    uint64_t list_addr;
    g_autofree uint64_t *addr = NULL;

    /*
     * The base pair's low word is entry 0's byte address, not a
     * page-aligned address plus flag bits: every observed mapping's low
     * word ends in 0x18 or 0x19 (three zero header words before the
     * page-frame-number entries begin), so only bits 2:0 are flag-like.
     * Masking to the page (as this used to) reads the header's first
     * zero word as page-frame number 0 and drops every transfer 24 bytes
     * short.
     */
    list_addr = ((uint64_t)list_node << 32) | ((uint32_t)base & ~0x7u);
    ctx->s = s;
    ctx->widget = (sgi_mgras_dma_map_slot_control(s, slot) >> 16) & 0xf;
    *bus = (SGIDMABus) {
        .as = &s->fabric_as,
        .ops = &mgras_raster_dma_ops,
        .opaque = ctx,
    };

    if (count == 0 || count > (1u << 20)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: DMA source mapping has "
                      "unusable page count %u\n", count);
        return false;
    }

    /*
     * The list lives in one node's memory: node in bits 39:32, offset in
     * 31:0. A list that ran past the node's last byte would carry into the
     * node number and read another node's memory.
     */
    if ((uint32_t)list_addr + (uint64_t)count * 8 > (1ull << 32)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: DMA page list of %u "
                      "entries at 0x%" PRIx64 " runs past its node\n",
                      count, list_addr);
        return false;
    }

    /* Read the big-endian entries and turn them into page addresses. */
    addr = g_new(uint64_t, count);
    if (!sgi_dma_read(bus, list_addr, addr, count * 8,
                      trace_event_get_state_backends(TRACE_SGI_MGRAS_DMA_CHUNK),
                      s->trace_sequence - 1)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        addr[i] = ((uint64_t)list_node << 32) |
                  ((ldq_be_p(&addr[i]) & MGRAS_DMA_PAGE_LIST_PFN_MASK) << 12);
    }
    *pl = (SGIDMAPageList) {
        .addr = addr,
        .npages = count,
        .page_size = MGRAS_DMA_PAGE_SIZE,
    };
    *page_addr = g_steal_pointer(&addr);
    return true;
}

/*
 * Despite their register order, SRC_OFF_LOW3 and SRC_OFF_ALIGNED are not
 * the low/high halves of a 64-bit address -- combining them that way
 * would need SRC_OFF_ALIGNED shifted into bits 63:32, and would send any
 * nonzero value there to a different node's memory entirely. Per the DMA
 * source-mapping decode the source-address formula in the comment above
 * mgras_raster_dma_setup() already cites, they are non-overlapping
 * parts of one byte offset into the page list: SRC_OFF_LOW3 carries the
 * offset's low 3 bits (src & 7), SRC_OFF_ALIGNED carries the same
 * offset's 8-aligned remainder (src & ~7). Addition reconstructs the
 * offset; a shift-and-OR would not.
 */
static uint64_t mgras_raster_dma_src_offset(SGIMgrasState *s)
{
    return (uint64_t)s->dma_desc_20000[MGRAS_DESC_20000_REG_SRC_OFF_LOW3] +
           s->dma_desc_20000[MGRAS_DESC_20000_REG_SRC_OFF_ALIGNED];
}

/*
 * The transfer between the resolved guest rows and the pixel banks, on
 * host memory only. The rows land through mgras_put_pixel(), so the logic
 * op, colour mask, clip and format tag apply as for any other paint.
 */
static int mgras_raster_dma_run_draw(SGIMgrasState *s, SGIPixelDMA *d,
                                     uint8_t op, uint32_t mask)
{
    uint32_t bpr = (uint32_t)d->w * d->guest_bpp;
    g_autofree uint8_t *bytes = g_malloc(bpr);

    for (int32_t row = 0; row < d->h; row++) {
        uint64_t pos = d->offset + (uint64_t)row * d->pitch;
        int y = d->y + row;

        if (!sgi_dma_map_copy(&d->map, pos, bytes, bpr, false)) {
            return SGI_FAIL_BOUNDS;
        }
        for (int32_t col = 0; col < d->w; col++) {
            const uint8_t *p = bytes + col * d->guest_bpp;
            /*
             * bpp 1 is a bare CI8 CMAP index. bpp 4 is a big-endian
             * 32-bit pixel with red in bits 7:0: bytes 3, 2 and 1 are
             * red, green and blue, and byte 0 is not stored. The public
             * drivers transfer this layout under the same pixel format
             * (0x600) and PP1FILLMODE format bits (0x62xx) the IRIX
             * server uses: X.Org's impact driver (impact_shadow.c,
             * visual masks 0xff/0xff00/0xff0000) and the Linux Impact
             * framebuffer patch (red at offset 0). The IRIX 6.5.30
             * greeter's EZsetup icon agrees: read red-first it is cyan
             * and blue-grey where the RAD4 reference shows yellow and
             * orange. PACKEDCOLOR and pair-packed pixels carry the same
             * word; see mgras_rgb_from_pixel_word().
             */
            uint32_t color = d->guest_bpp == 1 ? p[0] :
                ((uint32_t)p[3] << 16) | ((uint32_t)p[2] << 8) | p[1];
            uint8_t format = d->guest_bpp == 1 ? SGI_MGRAS_PIXEL_FORMAT_CI8 :
                                                 SGI_MGRAS_PIXEL_FORMAT_RGB;

            mgras_put_pixel(s, y * s->width + d->x + col, color, op, mask,
                            format);
        }
    }
    s->dirty = true;
    return 0;
}

/*
 * The inverse of mgras_raster_dma_run_draw()'s pixel assembly: bytes 1, 2
 * and 3 of a 4-byte pixel carry blue, green and red of the stored
 * 0x00RRGGBB value, and byte 0 always reads back zero because the draw
 * path never stores it.
 */
static int mgras_raster_dma_run_read(SGIMgrasState *s, SGIPixelDMA *d)
{
    uint32_t bpr = (uint32_t)d->w * d->guest_bpp;
    g_autofree uint8_t *bytes = g_malloc(bpr);

    for (int32_t row = 0; row < d->h; row++) {
        uint64_t pos = d->offset + (uint64_t)row * d->pitch;
        const uint32_t *stored = &s->front[(d->y + row) * s->width + d->x];

        for (int32_t col = 0; col < d->w; col++) {
            uint8_t *p = bytes + col * d->guest_bpp;

            if (d->guest_bpp == 1) {
                p[0] = stored[col];
            } else {
                p[0] = 0;
                p[1] = stored[col];
                p[2] = stored[col] >> 8;
                p[3] = stored[col] >> 16;
            }
        }
        if (!sgi_dma_map_copy(&d->map, pos, bytes, bpr, true)) {
            return SGI_FAIL_BOUNDS;
        }
    }
    return 0;
}

/*
 * One pixel transfer through the shared DMA code: its guest rows resolved
 * under the lock, the pixels moved on host memory, then the map released,
 * which writes back a read's rows. Both directions work on bank 0, the
 * bank the X server draws in, so that is the plane the transfer names. A
 * transfer that does not fit its page list or that plane fails whole
 * before anything is mapped, so it paints or writes nothing.
 */
static void mgras_raster_dma_transfer(SGIMgrasState *s, unsigned int slot,
                                      SGIPixelDMA *d, uint8_t op,
                                      uint32_t mask)
{
    MgrasDmaBus ctx;
    SGIDMABus bus;
    SGIDMAPageList pl;
    g_autofree uint64_t *page_addr = NULL;
    int ret;

    if (!mgras_raster_dma_setup(s, slot, &ctx, &bus, &pl, &page_addr)) {
        return;
    }
    d->plane = s->bank_plane[0];
    d->tag_plane = SGI_PLANE_NONE;
    d->trace_chunks =
        trace_event_get_state_backends(TRACE_SGI_MGRAS_DMA_CHUNK);
    d->trace_seq = s->trace_sequence - 1;
    d->seq = d->trace_seq;
    ret = sgi_dma_pixels_map(d, &bus, &pl, &s->mem);
    if (!ret) {
        ret = d->dir == SGI_DMA_TO_PLANE ?
              mgras_raster_dma_run_draw(s, d, op, mask) :
              mgras_raster_dma_run_read(s, d);
    }
    sgi_dma_pixels_finish(d, ret, &mgras_raster_dma_ops, &ctx);
}

static void mgras_raster_dma_draw_image(SGIMgrasState *s, uint32_t kick_byte)
{
    unsigned int slot;
    uint32_t pitch = s->dma_desc_20000[MGRAS_DESC_20000_REG_PITCH];
    uint32_t rows = s->dma_desc_20000[MGRAS_DESC_20000_REG_ROWS];
    uint32_t bpr = s->dma_desc_20000[MGRAS_DESC_20000_REG_BPR];
    uint32_t xy0 = s->rss[MGRAS_RSS_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[MGRAS_RSS_BLOCKXYENDI];
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    unsigned int width, bpp;
    SGIPixelDMA d;

    if (!mgras_raster_dma_select_slot(s, kick_byte, &slot)) {
        return;
    }
    if (rows == 0 || bpr == 0 || x0 > x1 || y0 > y1 ||
        x0 >= s->width || y0 >= s->height) {
        return;
    }
    x1 = MIN(x1, s->width - 1);
    y1 = MIN(y1, s->height - 1);
    width = x1 - x0 + 1;
    if (bpr % width) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: DMA image transfer, %u "
                      "bytes/row does not divide destination width %u\n",
                      bpr, width);
        return;
    }
    bpp = bpr / width;
    if (bpp != 1 && bpp != 4) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented DMA image "
                      "transfer pixel width %u bytes\n", bpp);
        return;
    }

    d = (SGIPixelDMA) {
        .dir = SGI_DMA_TO_PLANE,
        .x = x0,
        .y = y0,
        .w = width,
        .h = MIN(rows, (uint32_t)(y1 - y0 + 1)),
        .pitch = pitch,
        .guest_bpp = bpp,
        .offset = mgras_raster_dma_src_offset(s),
    };
    mgras_raster_dma_transfer(s, slot, &d, s->rss[MGRAS_RSS_PP1FILLMODE] >> 26,
                              s->rss[MGRAS_RSS_COLORMASKLSBSA]);
}

/*
 * Screen-to-host read: the inverse transfer of mgras_raster_dma_draw_image,
 * sharing its source-address formula and page-list layout. The box
 * arrives hardware-y and bottom-up rather than the draw's top-down
 * x0<=x1, y0<=y1 rectangle: BLOCKXYSTARTI's y is the box's hardware-y
 * bottom (screen row HEIGHT-1-y0) and BLOCKXYENDI's y is its top (screen
 * row HEIGHT-1-y1). Refuse a descriptor whose rows count does not match
 * that box height instead of guessing which end to trust.
 *
 * A stored pixel is mgras_put_pixel()'s COLORMASKLSBSA-masked 0x00RRGGBB
 * value, read back by mgras_raster_dma_run_read(). The Linux Impact
 * framebuffer patch's impact_smallcopy() relies on the same round trip:
 * it reads with PP1FILLMODE 0x2200 and pixel format 0x200, the values of
 * the IRIX server's save-under reads, and writes the words back with
 * 0x6200 and 0x600.
 */
static void mgras_raster_dma_read_image(SGIMgrasState *s, uint32_t kick_byte)
{
    unsigned int slot;
    uint32_t pitch = s->dma_desc_20000[MGRAS_DESC_20000_REG_PITCH];
    uint32_t rows = s->dma_desc_20000[MGRAS_DESC_20000_REG_ROWS];
    uint32_t bpr = s->dma_desc_20000[MGRAS_DESC_20000_REG_BPR];
    uint32_t xy0 = s->rss[MGRAS_RSS_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[MGRAS_RSS_BLOCKXYENDI];
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    int y_top, y_bottom;
    unsigned int width, bpp;
    SGIPixelDMA d;

    if (!mgras_raster_dma_select_slot(s, kick_byte, &slot)) {
        return;
    }
    if (rows == 0 || bpr == 0 || x0 > x1 || x0 >= s->width ||
        y0 >= s->height || y1 >= s->height) {
        return;
    }
    x1 = MIN(x1, s->width - 1);
    width = x1 - x0 + 1;
    if (bpr % width) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: DMA image transfer, %u "
                      "bytes/row does not divide destination width %u\n",
                      bpr, width);
        return;
    }
    bpp = bpr / width;
    if (bpp != 1 && bpp != 4) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented DMA image "
                      "transfer pixel width %u bytes\n", bpp);
        return;
    }

    y_top = s->height - 1 - y0;
    y_bottom = s->height - 1 - y1;
    if (y_top > y_bottom || rows != (uint32_t)(y_bottom - y_top + 1)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: DMA image transfer read box "
                      "does not match rows %u\n", rows);
        return;
    }

    d = (SGIPixelDMA) {
        .dir = SGI_DMA_FROM_PLANE,
        .x = x0,
        .y = y_top,
        .w = width,
        .h = rows,
        .pitch = pitch,
        .guest_bpp = bpp,
        .offset = mgras_raster_dma_src_offset(s),
    };
    mgras_raster_dma_transfer(s, slot, &d, 0, 0);
}

static void mgras_raster_dma_kick(SGIMgrasState *s, uint32_t kick_word)
{
    uint32_t kick_byte = kick_word & 0xff;

    if (kick_word & MGRAS_DMA_KICK_DIRECTION_READ) {
        mgras_raster_dma_read_image(s, kick_byte);
        return;
    }
    mgras_raster_dma_draw_image(s, kick_byte);
    /*
     * This model is synchronous: the transfer above already ran to
     * completion (or was a no-op on malformed descriptor state) by the
     * time this returns. The guest polls this bit after a kick before
     * reusing the descriptor/source buffer, mirroring the explicit FENCE
     * command-FIFO token above; without it a poller spins forever.
     */
    s->hq_flags |= MGRAS_HQ_FLAG_FENCE;
}

/*
 * Display-list execute, 0x00080c08 with two words: a byte offset into the
 * pool and a size. The IRIX 6.5.30 GL client sends it from
 * __glMgrDLExecuteDMA with nothing after it, just after the kernel
 * installs the pool's page list in DMA-map slot 4 (established from run
 * irix-20260928T023038-8a94ce: libGLcore.so-IP27GR532
 * notes-lists-arrays-lighting.md in the Origami repository, "Probe
 * measurements"). That the board reads the list from slot 4's pages is
 * inferred from that install being the only one before the execute.
 *
 * The board reads the words from host memory itself. The shared command
 * fetch resolves them from the page list and hands them to the same
 * command grammar the main port uses. The list gets a parser of its own,
 * starting at a header, and the port's resumes where it stood once the
 * list is done, so a list that ends inside a packet cannot swallow the
 * port's next words, and the port's words cannot finish a list's packet.
 * The list's words draw with the GL state of the context loaded when the
 * execute arrives, and change it as the same words on the port would.
 *
 * A list longer than a pool page is a chain of ranges, and the board
 * follows it. Established from run irix-20260928T111944-b1644a (seq
 * 7576878-7576887): for a 1,024-triangle list the client installed the
 * page list once and sent one execute, offset 0x1000 and size 0x3fa,
 * which covers the first of the list's 14 pool pages, and nothing else
 * reached the board before glFlush. So the rest of the list can only be
 * reached from the pool. Each range there begins with header words that
 * are not commands:
 *
 *   previous range's offset, next range's offset, next range's size
 *   previous range's offset, 0                    (on the last range)
 *
 * In that run's dump and in run irix-20260928T023038-8a94ce's, each
 * next offset and size is exactly the next range of the list, headers
 * included, in 32-bit words, across 19 ranges of three lists. Inferred:
 * that the board reads these fields this way, that the size counts
 * words, and that a next offset of 0 ends the list. That last is a gap:
 * offset 0 holds a real range in both runs, and no chain has been seen
 * to continue into offset 0, so a list that did would end early here.
 * The first word is not needed, and it is not stable: the same range
 * read 0x00083904 when the model fetched it and 0xf000 in the later
 * dump. The discriminating tests are gl-list-draws-the-probe-list,
 * gl-list-follows-the-chain and gl-list-big-matches-the-reference.
 *
 * A chain is followed for at most MGRAS_LIST_MAX_RANGES ranges and, over
 * all of them, MGRAS_LIST_MAX_WORDS words. Over either limit, or outside
 * the pool, the execute fails whole and nothing is drawn. The limits
 * stop a looping chain of small ranges, which would otherwise append
 * ranges until the pool's words ran out, hundreds of thousands of them
 * in one port write. The discriminating tests are
 * gl-list-over-the-range-limit and gl-list-past-the-pool-is-bounded.
 *
 * The client waits on nothing after the execute, and the board is not
 * known to report a list's end anywhere the guest can see, so the
 * completion this publishes goes nowhere (mgras_raster_dma_publish()).
 * A list is executed inside the port write that invoked it.
 */
#define MGRAS_LIST_SLOT 4

/*
 * The most words one execute may fetch. The board's own limit is not
 * known; this one bounds host memory and time, since the pool, and so
 * every size in it, is the guest's choice. It is 2^20 words (4 MiB),
 * twice the whole pool the IRIX 6.5.30 kernel installed in both runs
 * (0x204 pages, 528,384 words) and 78 times the largest list measured
 * (list-big, 13,355 words). An execute over it is refused whole.
 */
#define MGRAS_LIST_MAX_WORDS (1u << 20)

/*
 * The most ranges one execute may follow. The client fills each pool
 * page with one range of up to 1,017 words (list-big: 14 ranges), so a
 * list at MGRAS_LIST_MAX_WORDS laid out that way takes 1,032 ranges.
 */
#define MGRAS_LIST_MAX_RANGES 2048

/* A list fetch's view of the bus, and what its chain has refused. */
typedef struct MgrasListBus {
    MgrasDmaBus dma;
    bool refused;
} MgrasListBus;
#define MGRAS_LIST_NEXT 1
#define MGRAS_LIST_NEXT_SIZE 2

/* A range's header words: three when it names a next range, else two. */
static unsigned mgras_raster_list_header(const uint32_t *words, unsigned n)
{
    return n > MGRAS_LIST_NEXT && words[MGRAS_LIST_NEXT] ? 3 : 2;
}

static void mgras_raster_list_decode(void *opaque, uint32_t context,
                                     const uint32_t *words, unsigned n)
{
    MgrasDmaBus *b = opaque;
    unsigned header = mgras_raster_list_header(words, n);

    if (n < header) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: display-list range of %u "
                      "words is shorter than its header\n", n);
        return;
    }
    for (unsigned i = header; i < n; i++) {
        mgras_command_fifo_token(b->s, words[i]);
    }
}

static bool mgras_raster_list_next(void *opaque, const SGICommandFetch *f,
                                   uint64_t *offset, uint64_t *nwords)
{
    MgrasListBus *b = container_of(opaque, MgrasListBus, dma);
    const SGIFetchSegment *seg;

    if (!f->nsegments) {
        return false;
    }
    seg = &f->segments[f->nsegments - 1];
    if (seg->nwords <= MGRAS_LIST_NEXT_SIZE || !seg->words[MGRAS_LIST_NEXT]) {
        return false;
    }
    if (f->nsegments >= MGRAS_LIST_MAX_RANGES ||
        f->nwords + seg->words[MGRAS_LIST_NEXT_SIZE] > MGRAS_LIST_MAX_WORDS) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: display list goes on "
                      "past %u ranges or %u words, the model's limits\n",
                      MGRAS_LIST_MAX_RANGES, MGRAS_LIST_MAX_WORDS);
        b->refused = true;
        return false;
    }
    *offset = seg->words[MGRAS_LIST_NEXT];
    *nwords = seg->words[MGRAS_LIST_NEXT_SIZE];
    return true;
}

static void mgras_raster_list_execute(SGIMgrasState *s, uint32_t offset,
                                      uint32_t size)
{
    SGICommandFetch *f = &s->list_fetch;
    SGIMgrasCmdParser port;
    MgrasListBus ctx = { };
    SGIDMABus bus;
    SGIDMAPageList pl;
    g_autofree uint64_t *page_addr = NULL;
    int ret;

    /*
     * A list calling a list would arrive here from inside the fetch. The
     * client can pool call tokens in a list; how the board runs them is
     * not known.
     */
    if (f->open) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: a display list calls the "
                      "list at pool offset 0x%x; nested lists are not "
                      "modelled\n", offset);
        return;
    }
    if (size > MGRAS_LIST_MAX_WORDS) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: display list at 0x%x of "
                      "%u words is over the model's %u-word limit\n", offset,
                      size, MGRAS_LIST_MAX_WORDS);
        return;
    }
    if (sgi_mgras_dma_map_slot_count(s, MGRAS_LIST_SLOT) == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: display list executed "
                      "with no pool mapped in DMA-map slot %u\n",
                      MGRAS_LIST_SLOT);
        return;
    }
    if (!mgras_raster_dma_setup(s, MGRAS_LIST_SLOT, &ctx.dma, &bus, &pl,
                                &page_addr)) {
        return;
    }
    f->context = s->gl_current;
    f->decode = mgras_raster_list_decode;
    f->next = mgras_raster_list_next;
    f->little_endian = false;
    f->trace_chunks = trace_event_get_state_backends(TRACE_SGI_MGRAS_DMA_CHUNK);
    f->trace_seq = s->trace_sequence - 1;
    f->seq = f->trace_seq;
    f->cookie = 0;

    sgi_mgras_cmd_parser_save(s, &port);
    s->cmd_token_class = s->cmd_token_reg = 0;
    s->cmd_token_words_remaining = s->cmd_token_payload_count = 0;
    ret = sgi_fetch_map(f, &bus, &pl, offset, size);
    if (!ret && ctx.refused) {
        ret = SGI_FAIL_BOUNDS;
    }
    if (!ret) {
        sgi_fetch_run(f, bus.opaque);
        if (s->cmd_token_words_remaining) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: display list at 0x%x "
                          "ends %u words short of a packet\n", offset,
                          s->cmd_token_words_remaining);
        }
    }
    sgi_fetch_finish(f, ret, bus.ops, bus.opaque);
    sgi_mgras_cmd_parser_load(s, &port);
}

static void mgras_raster_desc_bank_store(SGIMgrasState *s, uint32_t cmd_class,
                                      uint32_t reg, const uint32_t *payload,
                                      uint32_t payload_words)
{
    uint32_t *regs;
    uint32_t regs_count;

    if (cmd_class == MGRAS_CMD_HDR_CLASS_DESC_20000 &&
        reg == MGRAS_DESC_20000_REG_LIST_EXECUTE) {
        if (payload_words == 2) {
            mgras_raster_list_execute(s, payload[0], payload[1]);
        } else {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: display-list execute with "
                          "%u words, not 2\n", payload_words);
        }
        return;
    }

    switch (cmd_class) {
    case MGRAS_CMD_HDR_CLASS_DESC_20000:
        regs = s->dma_desc_20000;
        regs_count = ARRAY_SIZE(s->dma_desc_20000);
        break;
    case MGRAS_CMD_HDR_CLASS_DESC_28000:
        regs = s->dma_desc_28000;
        regs_count = ARRAY_SIZE(s->dma_desc_28000);
        break;
    case MGRAS_CMD_HDR_CLASS_DESC_30000:
        regs = s->dma_desc_30000;
        regs_count = ARRAY_SIZE(s->dma_desc_30000);
        break;
    default:
        return;
    }

    for (uint32_t i = 0; i < payload_words; i++) {
        uint32_t r = reg + i;

        if (r < regs_count) {
            regs[r] = payload[i];
        }
    }

    if (cmd_class == MGRAS_CMD_HDR_CLASS_DESC_20000 &&
        reg <= MGRAS_DESC_20000_REG_KICK &&
        MGRAS_DESC_20000_REG_KICK < reg + payload_words) {
        mgras_raster_dma_kick(s, s->dma_desc_20000[MGRAS_DESC_20000_REG_KICK]);
    }
}

static void mgras_raster_command_fifo_dispatch(SGIMgrasState *s)
{
    uint32_t reg = s->cmd_token_reg;
    /* Every GL packet the model executes has a whole-word length. */
    uint32_t gl_header = (s->cmd_token_class << 16) | (reg << 8) |
                         (s->cmd_token_payload_count * sizeof(uint32_t));

    /*
     * Established as software expectation (mgras-hq4.md, "HQ flags"): the
     * IRIX 6.5.30 client's __Mgr_ReturnMode clears flag 17, writes the query,
     * then polls flag 17 with no bound and reads 0x028004 once it is set.
     * Approximated: the model has the answer as soon as the arguments
     * arrive, and it replaces the GE revision 0x028004 held since reset.
     * sgi_mgras_gl_query_answer() answers the indices it decodes from the
     * loaded context's GL state and every other index with zero; which
     * answer the board gives each index is unknown. The discriminating
     * tests are hq-query-sets-flag-after-arguments,
     * hq-query-flag-only-from-query and hq-query-answers-the-matrices.
     */
    if (s->cmd_token_class == MGRAS_CMD_HDR_CLASS_HQ &&
        reg == MGRAS_HQ_OP_QUERY) {
        s->ge_report = sgi_mgras_gl_query_answer(s, s->cmd_token_payload,
                                                 s->cmd_token_payload_count);
        s->hq_flags |= MGRAS_HQ_FLAG_QUERY;
        return;
    }

    /*
     * Established (notes-return-mode-queries.md in the Origami repository,
     * "Other queries answered at 0x028004"): the client's one-argument
     * 0xa104 query, sent for GL_CURRENT_COLOR and the other current
     * attributes, and its two-argument 0x8b08 histogram query clear flag
     * 17 and wait on it like 0xa20c. perfly waited forever on 0xa104 in
     * run irix-20260928T063220-a9945b. Approximated as for 0xa20c: the
     * flag is set once the query's own arguments arrive, and the answer
     * is the model's (sgi_mgras_gl_attribute_answer()). The
     * discriminating test is hq-attribute-queries-set-flag-after-arguments.
     */
    if (s->cmd_token_class == MGRAS_CMD_HDR_CLASS_HQ &&
        (reg == MGRAS_HQ_OP_ATTR_QUERY ||
         reg == MGRAS_HQ_OP_HISTOGRAM_QUERY)) {
        s->ge_report = reg == MGRAS_HQ_OP_ATTR_QUERY &&
                       s->cmd_token_payload_count == 1 ?
                       sgi_mgras_gl_attribute_answer(s,
                                                     s->cmd_token_payload[0]) :
                       0;
        s->hq_flags |= MGRAS_HQ_FLAG_QUERY;
        return;
    }

    /*
     * Established (mgras-hq4.md, "Swap scheduling"): MgrasSchedSwapBuf
     * enables source 10, writes 0x3700 and sleeps until its handler sees
     * source 10. Inferred there: the board raises it once the work queued
     * ahead of the word has drained, so the flip follows the rendering.
     * Approximated: the model executes each command as it arrives, so
     * nothing is queued ahead and it raises the source when the word is
     * consumed. The XMAP
     * buffer selection itself is the kernel's MgrasSchedSwapTask, not
     * this word. The discriminating tests are
     * swap-command-raises-interrupt-10 and swap-command-needs-enable.
     */
    if (s->cmd_token_class == MGRAS_CMD_HDR_CLASS_HQ &&
        reg == MGRAS_HQ_OP_SWAP) {
        sgi_mgras_raise_pending(s, SGI_MGRAS_PENDING_SWAP);
        return;
    }

    if (sgi_mgras_gl_header(gl_header)) {
        sgi_mgras_gl_execute(s, gl_header, s->cmd_token_payload,
                             s->cmd_token_payload_count);
        return;
    }

    if (s->cmd_token_class == MGRAS_CMD_HDR_CLASS_DESC_20000 ||
        s->cmd_token_class == MGRAS_CMD_HDR_CLASS_DESC_28000 ||
        s->cmd_token_class == MGRAS_CMD_HDR_CLASS_DESC_30000) {
        mgras_raster_desc_bank_store(s, s->cmd_token_class, reg,
                                  s->cmd_token_payload,
                                  s->cmd_token_payload_count);
        return;
    }

    /* Rebuild the accepted header from parser fields already in VMState. */
    SGIMgrasRasterSubmission submission = {
        .form = SGI_MGRAS_RASTER_FIFO,
        .header = ((s->cmd_token_class | (reg >> 8)) << 16) |
                  ((reg & 0xff) << 8) |
                  (s->cmd_token_payload_count * sizeof(uint32_t)),
        .reg = reg,
        .payload = s->cmd_token_payload,
        .payload_words = s->cmd_token_payload_count,
    };

    if (s->cmd_token_class == MGRAS_CMD_HDR_CLASS_STATE) {
        if (s->cmd_token_payload_count >= 1) {
            mgras_raster_rss_write(s, reg, s->cmd_token_payload[0]);
        } else {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: state-write command-FIFO "
                          "header, register 0x%03x with no payload word\n",
                          reg);
        }
        return;
    }

    mgras_raster_command_execute(s, &submission);
}
