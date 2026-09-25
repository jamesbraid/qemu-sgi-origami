/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SI MGRAS raster command implementation.
 * Direct records use payload32 | (selector << 34), with selector
 * 0x60001/0x70001 | (register << 6). FIFO words are 32-bit
 * headers in high-half-first order and retain payload state across
 * MMIO stores. Unsupported traffic remains observable and opaque.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/sgi-mgras-raster.h"
#include "system/dma.h"




#define MGRAS_CMD_SELECTOR_MASK 0x1fff803f
#define MGRAS_CMD_SELECTOR_STATE 0x60001
#define MGRAS_CMD_SELECTOR_EXEC 0x70001


#define MGRAS_CMD_FIFO_FENCE_CLASS 0x000e
#define MGRAS_CMD_FIFO_FENCE_REG 0x04
#define MGRAS_HQ_FLAG_FENCE 0x00010000


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
 * selects direction (0 = draw a host image into the framebuffer, the path
 * implemented here; 1 = read the framebuffer back to host memory, an
 * unimplemented follow-up).
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
 * Register 0x05f: function unknown. The DDX writes it 32 times with
 * value 0 immediately before the PIO draw-pixel row loop
 * (mgrasDrawPixels..C, 0x577e68-0x577ee8) in every observed capture. No
 * read, branch or paint result has been tied to it, so it is not treated
 * as a colour or alpha register here; that would assert a meaning the
 * evidence does not establish. Latched silently by the generic
 * mgras_raster_rss_write() store like every other unmodelled register.
 */
#define MGRAS_RSS_UNK_05F 0x05f
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
 * tests/data/mgras/char-modes.replay). Bits 16:14 are a row-padding count
 * a capture also carries in this word (blockcopy-grammar.md) and do not
 * change which path runs.
 */
#define MGRAS_RSS_BYTE_XFER_PACKING 0x159
#define MGRAS_BYTE_XFER_PACKING_PAIRS 0x0080

/*
 * A BLOCKXYENDI execute paints a solid rectangle only when FILLMODE has
 * bit 20 set and bit 22 clear (MGRAS_FILLMODE_GLYPH_ROW, which sets both
 * bits, is a glyph-row latch and never a rectangle fill, so this mask
 * naturally excludes it). Any other FILLMODE value leaves the raster
 * unchanged; only the WID plane still tags.
 */
#define MGRAS_FILLMODE_RECT_FILL_MASK 0x00500000
#define MGRAS_FILLMODE_RECT_FILL_VALUE 0x00100000

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


static void mgras_command_fifo_token(SGIMgrasState *s, uint32_t token)
{
    uint32_t cmd_class, reg, payload_bytes, true_class;

    if (s->cmd_token_words_remaining > 0) {
        if (s->cmd_token_class != MGRAS_CMD_HDR_CLASS_DISCARD &&
            s->cmd_token_payload_count <
            SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS) {
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

    if (cmd_class == MGRAS_CMD_HDR_CLASS_DESC_20000 ||
        cmd_class == MGRAS_CMD_HDR_CLASS_DESC_28000 ||
        cmd_class == MGRAS_CMD_HDR_CLASS_DESC_30000) {
        if (payload_bytes % sizeof(uint32_t)) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: descriptor command-FIFO "
                          "header, class 0x%04x reg 0x%02x has unaligned "
                          "payload %u bytes\n", cmd_class, reg, payload_bytes);
            return;
        }
        s->cmd_token_class = cmd_class;
        s->cmd_token_reg = reg;
        s->cmd_token_payload_count = 0;
        s->cmd_token_words_remaining = payload_bytes / 4;
        if (s->cmd_token_words_remaining >
            SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: command-FIFO header, "
                          "class 0x%04x reg 0x%02x payload %u bytes "
                          "exceeds %zu-byte limit\n", cmd_class, reg,
                          payload_bytes, sizeof(s->cmd_token_payload));
            s->cmd_token_class = MGRAS_CMD_HDR_CLASS_DISCARD;
            return;
        }
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
            return;
        }
        s->cmd_token_class = true_class;
        s->cmd_token_reg = ((cmd_class & 1) << 8) | reg;
        s->cmd_token_payload_count = 0;
        s->cmd_token_words_remaining = payload_bytes / 4;
        if (s->cmd_token_words_remaining >
            SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: command-FIFO header, "
                          "class 0x%04x reg 0x%02x payload %u bytes "
                          "exceeds %zu-byte limit\n", cmd_class, reg,
                          payload_bytes, sizeof(s->cmd_token_payload));
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

    if (payload_bytes % sizeof(uint32_t)) {
        return;
    }

    /*
     * This class is not implemented, but its header still declares
     * payload_bytes/4 words in the same framing as STATE/EXEC. Frame and
     * discard them instead of leaving the next FIFO word to be mis-parsed
     * as a new header: that mis-parse is what corrupted the rest of the
     * burst before this fix.
     */
    s->cmd_token_class = MGRAS_CMD_HDR_CLASS_DISCARD;
    s->cmd_token_payload_count = 0;
    s->cmd_token_words_remaining = payload_bytes / 4;
    if (s->cmd_token_words_remaining > SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: command-FIFO token, class "
                      "0x%04x reg 0x%02x payload %u bytes exceeds %zu-byte "
                      "limit\n", cmd_class, reg, payload_bytes,
                      sizeof(s->cmd_token_payload));
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
 */
static uint8_t mgras_pixel_format(SGIMgrasState *s)
{
    return (s->rss[MGRAS_RSS_PP1FILLMODE] & 0x2000) ? SGI_MGRAS_PIXEL_FORMAT_RGB :
                                                    SGI_MGRAS_PIXEL_FORMAT_CI8;
}

/*
 * A paint writes colour and its format tag together. The WID plane keeps
 * whatever tagged it.
 */
static void mgras_put_pixel(SGIMgrasState *s, unsigned int idx, uint32_t color,
                         uint8_t op, uint32_t mask, uint8_t format)
{
    if (!(mask & 0x00ffffff)) {
        return;
    }
    s->front[idx] = mgras_paint_pixel(op, s->front[idx], color, mask);
    s->format[idx] = format;
}


/* Normalize corners and clip. Returns false when nothing is visible. */
static bool mgras_rect_bounds(uint32_t xy0, uint32_t xy1, int *x_lo, int *y_lo,
                           int *x_hi, int *y_hi)
{
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;

    *x_lo = MIN(x0, x1);
    *y_lo = MIN(y0, y1);
    if (*x_lo >= SGI_MGRAS_WIDTH || *y_lo >= SGI_MGRAS_HEIGHT) {
        return false;
    }
    *x_hi = MIN(MAX(x0, x1), SGI_MGRAS_WIDTH - 1);
    *y_hi = MIN(MAX(y0, y1), SGI_MGRAS_HEIGHT - 1);
    return true;
}


static void mgras_fill_rect(SGIMgrasState *s, uint32_t xy0, uint32_t xy1,
                         uint32_t color, uint8_t op, uint32_t mask,
                         uint8_t format)
{
    int x_lo, y_lo, x_hi, y_hi;

    if (!mgras_rect_bounds(xy0, xy1, &x_lo, &y_lo, &x_hi, &y_hi)) {
        return;
    }
    for (int y = y_lo; y <= y_hi; y++) {
        for (int x = x_lo; x <= x_hi; x++) {
            mgras_put_pixel(s, y * SGI_MGRAS_WIDTH + x, color, op, mask, format);
        }
    }
}


static void mgras_fill_wid_rect(SGIMgrasState *s, uint32_t xy0, uint32_t xy1,
                             uint8_t wid)
{
    int x_lo, y_lo, x_hi, y_hi;

    if (!mgras_rect_bounds(xy0, xy1, &x_lo, &y_lo, &x_hi, &y_hi)) {
        return;
    }
    for (int y = y_lo; y <= y_hi; y++) {
        memset(&s->wid[y * SGI_MGRAS_WIDTH + x_lo], wid, x_hi - x_lo + 1);
    }
}


static void mgras_draw_fill(SGIMgrasState *s)
{
    uint32_t mask = s->rss[MGRAS_RSS_COLORMASKLSBSA];
    uint8_t op = s->rss[MGRAS_RSS_PP1FILLMODE] >> 26;
    uint8_t format = mgras_pixel_format(s);
    uint32_t color;

    /*
     * COLORMASKLSBSA masks the colour and COLORMASKMSBS the WID plane, so
     * a fill writes DRBPOINTERS bits 23:16 over its rectangle whether or
     * not it also writes colour. A nonzero COLORMASKMSBS enables the whole
     * WID byte; it is not applied as a bit mask over it. The capture rules
     * that out: nine of its fills carry COLORMASKMSBS 0x70 and write WID
     * 1, and 0x70 masked over 1 would change no bit, leaving the root
     * window on the WID the preceding clear gave it and sending it through
     * the wrong colour map. Inferred from the 21 fills with a nonzero
     * COLORMASKMSBS in the retained desktop capture: 19 write no colour
     * and tag the root window, and the whole-screen clear that both paints
     * and tags leaves the two windows on a different entry. Where the
     * value physically comes from is unknown, and no other primitive in
     * the capture carries a nonzero COLORMASKMSBS.
     */
    if (s->rss[MGRAS_RSS_COLORMASKMSBS]) {
        mgras_fill_wid_rect(s, s->rss[MGRAS_RSS_BLOCKXYSTARTI],
                         s->rss[MGRAS_RSS_BLOCKXYENDI],
                         s->rss[MGRAS_RSS_DRBPOINTERS] >> 16);
    }

    if ((s->rss[MGRAS_RSS_FILLMODE] & MGRAS_FILLMODE_RECT_FILL_MASK) !=
        MGRAS_FILLMODE_RECT_FILL_VALUE) {
        return;
    }

    /*
     * FILLCOLOR_R/G/B, not BKGRD_RG/PACKEDCOLOR: the DDX writes a
     * rectangle's colour there before every BLOCKXYENDI fill execute
     * (see the FILLCOLOR_R/G/B comment above). CI8 reads only the low
     * byte of FILLCOLOR_R as a bare CMAP index; RGB reads all three,
     * each >> 4 to an 8-bit channel.
     */
    color = format == SGI_MGRAS_PIXEL_FORMAT_CI8 ?
        s->rss[MGRAS_RSS_FILLCOLOR_R] & 0xff :
        (((s->rss[MGRAS_RSS_FILLCOLOR_R] >> 4) & 0xff) << 16) |
        (((s->rss[MGRAS_RSS_FILLCOLOR_G] >> 4) & 0xff) << 8) |
        ((s->rss[MGRAS_RSS_FILLCOLOR_B] >> 4) & 0xff);
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
    uint32_t ink = format == SGI_MGRAS_PIXEL_FORMAT_CI8 ?
        (s->rss[MGRAS_RSS_RED] >> 12) & 0xff :
        s->rss[MGRAS_RSS_PACKEDCOLOR] & 0x00ffffff;
    uint32_t mask = s->rss[MGRAS_RSS_COLORMASKLSBSA];
    uint8_t op = s->rss[MGRAS_RSS_PP1FILLMODE] >> 26;
    uint64_t bits = ((uint64_t)s->rss[MGRAS_RSS_CHAR_H] << 32) |
                    s->rss[MGRAS_RSS_CHAR_L];
    int y = y0 + s->glyph_row;
    int width;

    if (x0 > x1 || y0 > y1 || x0 >= SGI_MGRAS_WIDTH || y0 >= SGI_MGRAS_HEIGHT) {
        return;
    }
    x1 = MIN(x1, SGI_MGRAS_WIDTH - 1);
    y1 = MIN(y1, SGI_MGRAS_HEIGHT - 1);
    s->glyph_row++;
    if (y > y1 || y >= SGI_MGRAS_HEIGHT) {
        return;
    }
    width = MIN(x1 - x0 + 1, 64);
    for (int i = 0; i < width; i++) {
        bool set = (bits >> (63 - i)) & 1;

        if (set) {
            mgras_put_pixel(s, y * SGI_MGRAS_WIDTH + x0 + i, ink, op, mask,
                        format);
        }
    }
}

/*
 * FILLMODE_BYTE_XFER delivers eight 8-bit indexed pixels per CHAR_L execute
 * (CHAR_H is staged first and never executes itself): CHAR_H's four bytes
 * MSB to LSB, then CHAR_L's four, painted left to right. The source
 * scanline the guest is transferring is padded to a 32-bit boundary before
 * it reaches the raster, so a row's first unit can start with padding
 * bytes rather than pixel 0, and the row only advances once enough units
 * have covered its width. `skip` is that row's offset from the 8-byte unit
 * boundary; the register carrying it was not identified in the capture, so
 * this is a stated approximation of the X server's 32-bit scanline
 * padding, derived by matching the painted pixel positions in a live
 * capture (tests/data/mgras/char-modes.replay):
 * skip = (row * 4 * ceil(width / 4)) % 8. It is not read from hardware
 * state. Painted bytes are always CI8 CMAP indices regardless of
 * PP1FILLMODE: this path (and mgras_draw_byte_row_pairs()) carries its own
 * 8-bit index data rather than a register-selected colour, so there is
 * no RGB form to select between.
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
    int skip = (row * 4 * DIV_ROUND_UP(width, 4)) % 8;
    int units_per_row = DIV_ROUND_UP(skip + width, 8);

    if (y <= y1 && y < SGI_MGRAS_HEIGHT) {
        for (int i = 0; i < 8; i++) {
            int col = s->glyph_unit * 8 + i - skip;

            if (col < 0 || col >= width || x0 + col >= SGI_MGRAS_WIDTH) {
                continue;
            }
            mgras_put_pixel(s, y * SGI_MGRAS_WIDTH + x0 + col, bytes[i], op,
                         mask, SGI_MGRAS_PIXEL_FORMAT_CI8);
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
 * execute delivers two indexed pixels rather than eight. Pixel 0 is
 * CHAR_H bits 7:0, pixel 1 is CHAR_L bits 7:0; a live capture's other
 * bytes did not reliably replicate that value, so only bits 7:0 are read
 * (tests/data/mgras/root-icon.replay). ceil(width / 2) executes cover a
 * row, no skip, and the row advances after the last unit.
 */
static void mgras_draw_byte_row_pairs(SGIMgrasState *s, int x0, int x1,
                                   int y0, int y1, uint32_t mask, uint8_t op)
{
    uint8_t pixels[2] = {
        (uint8_t)s->rss[MGRAS_RSS_CHAR_H],
        (uint8_t)s->rss[MGRAS_RSS_CHAR_L],
    };
    int width = x1 - x0 + 1;
    int row = s->glyph_row;
    int y = y0 + row;
    int units_per_row = DIV_ROUND_UP(width, 2);

    if (y <= y1 && y < SGI_MGRAS_HEIGHT) {
        for (int i = 0; i < 2; i++) {
            int col = s->glyph_unit * 2 + i;

            if (col >= width || x0 + col >= SGI_MGRAS_WIDTH) {
                continue;
            }
            mgras_put_pixel(s, y * SGI_MGRAS_WIDTH + x0 + col, pixels[i], op,
                         mask, SGI_MGRAS_PIXEL_FORMAT_CI8);
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

    if (x0 > x1 || y0 > y1 || x0 >= SGI_MGRAS_WIDTH || y0 >= SGI_MGRAS_HEIGHT) {
        return;
    }
    x1 = MIN(x1, SGI_MGRAS_WIDTH - 1);
    y1 = MIN(y1, SGI_MGRAS_HEIGHT - 1);

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

    if (x0 > x1 || y0 > y1 || x0 >= SGI_MGRAS_WIDTH || y0 >= SGI_MGRAS_HEIGHT) {
        return;
    }
    x1 = MIN(x1, SGI_MGRAS_WIDTH - 1);
    y1 = MIN(y1, SGI_MGRAS_HEIGHT - 1);

    for (int i = 0; i < 2; i++) {
        int x = x0 + s->pio_x;
        int y = y0 + s->pio_y;

        if (x > x1 || y > y1 || x >= SGI_MGRAS_WIDTH ||
            y >= SGI_MGRAS_HEIGHT) {
            return;
        }
        mgras_put_pixel(s, y * SGI_MGRAS_WIDTH + x, pixels[i], op, mask, format);
        s->pio_x++;
        if (x0 + s->pio_x > x1) {
            s->pio_x = 0;
            s->pio_y++;
        }
    }
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
 * a pen position for a following RE_LINE to draw from.
 */
static void mgras_draw_point(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[MGRAS_RSS_RE_POINT];
    uint8_t format = mgras_pixel_format(s);
    uint32_t color = format == SGI_MGRAS_PIXEL_FORMAT_CI8 ?
        (s->rss[MGRAS_RSS_RED] >> 12) & 0xff :
        s->rss[MGRAS_RSS_PACKEDCOLOR] & 0x00ffffff;
    uint32_t mask = s->rss[MGRAS_RSS_COLORMASKLSBSA];
    uint8_t op = s->rss[MGRAS_RSS_PP1FILLMODE] >> 26;
    int x = (xy0 >> 16) & 0xffff, y = xy0 & 0xffff;

    if (x >= SGI_MGRAS_WIDTH || y >= SGI_MGRAS_HEIGHT) {
        return;
    }
    mgras_put_pixel(s, y * SGI_MGRAS_WIDTH + x, color, op, mask, format);
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
 * end point chains a run of RE_LINEs into a polyline the same way the
 * capture chains them.
 */
static void mgras_draw_line(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[MGRAS_RSS_RE_POINT];
    uint32_t xy1 = s->rss[MGRAS_RSS_RE_LINE];
    uint8_t format = mgras_pixel_format(s);
    uint32_t color = format == SGI_MGRAS_PIXEL_FORMAT_CI8 ?
        (s->rss[MGRAS_RSS_RED] >> 12) & 0xff :
        s->rss[MGRAS_RSS_PACKEDCOLOR] & 0x00ffffff;
    uint32_t mask = s->rss[MGRAS_RSS_COLORMASKLSBSA];
    uint8_t op = s->rss[MGRAS_RSS_PP1FILLMODE] >> 26;
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        int e2;

        if (x0 < SGI_MGRAS_WIDTH && y0 < SGI_MGRAS_HEIGHT) {
            mgras_put_pixel(s, y0 * SGI_MGRAS_WIDTH + x0, color, op, mask,
                        format);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
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
 * Host-memory source address formula (see docs/notebooks and the DMA-src
 * capture evidence this model is built from): va = addr_low + addr_high +
 * row*pitch + byte, page_ix = va >> 12, and the source page's physical
 * address is (list_node << 32) | (pagelist[page_ix] << 12) | (va & 0xfff).
 * The page list itself is `count` big-endian 64-bit page-frame-number
 * entries at (list_node << 32) | list_pa, both taken from the selected
 * mapping slot. Reading byte-by-byte (rather than the whole row at once)
 * is deliberate: consecutive page-list entries need not be adjacent, so a
 * row's bytes can span a page boundary at an arbitrary offset.
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

/*
 * Fetch and byte-swap a slot's whole big-endian page-frame-number list,
 * shared by the draw and read paths. On success, *page_list is a freshly
 * allocated `count`-entry array the caller owns. On any composed fabric
 * address or DMA failure, this has already logged the guest error and
 * freed the partial list; the caller must stop the transfer rather than
 * paint or write anything from it.
 */
static bool mgras_raster_dma_load_page_list(SGIMgrasState *s,
                                            unsigned int widget,
                                            uint32_t list_node,
                                            uint32_t list_pa, uint32_t count,
                                            uint64_t **page_list)
{
    uint64_t *list = g_new0(uint64_t, count);

    for (uint32_t i = 0; i < count; i++) {
        hwaddr fabric;
        uint8_t entry[sizeof(uint64_t)];
        MemTxResult result;

        if (!sgi_mgras_xio_compose(s, widget,
                              ((uint64_t)list_node << 32) | (list_pa + i * 8),
                              &fabric)) {
            g_free(list);
            return false;
        }
        result = dma_memory_read(&s->fabric_as, fabric, entry, sizeof(entry),
                                 MEMTXATTRS_UNSPECIFIED);
        if (result != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: DMA image transfer "
                          "page-list entry %u read at 0x%" PRIx64 " failed "
                          "(result %d)\n", i, (uint64_t)fabric, result);
            g_free(list);
            return false;
        }
        list[i] = ldq_be_p(entry) & MGRAS_DMA_PAGE_LIST_PFN_MASK;
    }
    *page_list = list;
    return true;
}

/*
 * Despite their register order, SRC_OFF_LOW3 and SRC_OFF_ALIGNED are not
 * the low/high halves of a 64-bit address -- combining them that way
 * would need SRC_OFF_ALIGNED shifted into bits 63:32, and would send any
 * nonzero value there to a different node's memory entirely. Per the DMA
 * source-mapping decode the source-address formula in the comment above
 * mgras_raster_dma_load_page_list() already cites, they are non-overlapping
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

static void mgras_raster_dma_draw_image(SGIMgrasState *s, uint32_t kick_byte)
{
    unsigned int slot, widget;
    uint64_t base;
    uint32_t list_node, list_pa, count;
    uint32_t pitch = s->dma_desc_20000[MGRAS_DESC_20000_REG_PITCH];
    uint64_t addr = mgras_raster_dma_src_offset(s);
    uint32_t rows = s->dma_desc_20000[MGRAS_DESC_20000_REG_ROWS];
    uint32_t bpr = s->dma_desc_20000[MGRAS_DESC_20000_REG_BPR];
    uint32_t xy0 = s->rss[MGRAS_RSS_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[MGRAS_RSS_BLOCKXYENDI];
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    uint8_t op = s->rss[MGRAS_RSS_PP1FILLMODE] >> 26;
    uint32_t mask = s->rss[MGRAS_RSS_COLORMASKLSBSA];
    unsigned int width, bpp;
    uint64_t *page_list;

    if (!mgras_raster_dma_select_slot(s, kick_byte, &slot)) {
        return;
    }
    if (rows == 0 || bpr == 0 || x0 > x1 || y0 > y1 ||
        x0 >= SGI_MGRAS_WIDTH || y0 >= SGI_MGRAS_HEIGHT) {
        return;
    }
    x1 = MIN(x1, SGI_MGRAS_WIDTH - 1);
    y1 = MIN(y1, SGI_MGRAS_HEIGHT - 1);
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

    /*
     * The base pair's low word is entry 0's byte address, not a
     * page-aligned address plus flag bits: every observed mapping's low
     * word ends in 0x18 or 0x19 (three zero header words before the
     * page-frame-number entries begin), so only bits 2:0 are flag-like.
     * Masking to the page (as this used to) reads the header's first
     * zero word as page-frame number 0 and drops every transfer 24 bytes
     * short.
     */
    base = sgi_mgras_dma_map_slot_base(s, slot);
    list_node = (uint32_t)(base >> 32) & 0xff;
    list_pa = (uint32_t)base & ~0x7u;
    count = sgi_mgras_dma_map_slot_count(s, slot);
    widget = (sgi_mgras_dma_map_slot_control(s, slot) >> 16) & 0xf;

    if (count == 0 || count > (1u << 20)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: DMA source mapping has "
                      "unusable page count %u\n", count);
        return;
    }

    if (!mgras_raster_dma_load_page_list(s, widget, list_node, list_pa, count,
                                      &page_list)) {
        return;
    }

    rows = MIN(rows, (uint32_t)(y1 - y0 + 1));
    for (uint32_t row = 0; row < rows; row++) {
        int y = y0 + (int)row;
        uint8_t pixel_bytes[4];

        for (uint32_t b = 0; b < bpr; b++) {
            uint64_t va = addr + (uint64_t)row * pitch + b;
            uint64_t page_ix = va >> 12;
            uint64_t src_pa;
            hwaddr fabric;
            uint8_t byte;
            MemTxResult result;

            if (page_ix >= count) {
                qemu_log_mask(LOG_UNIMP, "sgi-mgras: DMA image transfer "
                              "source page %" PRIu64 " exceeds mapped "
                              "count %u\n", page_ix, count);
                continue;
            }
            src_pa = ((uint64_t)list_node << 32) |
                     (page_list[page_ix] << 12) | (va & 0xfff);
            if (!sgi_mgras_xio_compose(s, widget, src_pa, &fabric)) {
                continue;
            }
            result = dma_memory_read(&s->fabric_as, fabric, &byte,
                                     sizeof(byte), MEMTXATTRS_UNSPECIFIED);
            if (result != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: DMA image "
                              "transfer source read at 0x%" PRIx64 " failed "
                              "(result %d)\n", (uint64_t)fabric, result);
                g_free(page_list);
                return;
            }
            pixel_bytes[b % bpp] = byte;
            if (b % bpp == bpp - 1) {
                uint32_t col = b / bpp;
                /*
                 * bpp 1 is a bare CI8 CMAP index. bpp 4 assembles an
                 * MSB-first 32-bit pixel, but mgras_put_pixel()/
                 * mgras_paint_pixel() mask to the low 24 bits, so byte 0
                 * (the MSB) is discarded and bytes 1-3 survive as an
                 * 0x00RRGGBB triple: an RGB pixel.
                 */
                uint32_t color = bpp == 1 ? pixel_bytes[0] :
                    ((uint32_t)pixel_bytes[0] << 24) |
                    ((uint32_t)pixel_bytes[1] << 16) |
                    ((uint32_t)pixel_bytes[2] << 8) | pixel_bytes[3];
                uint8_t format = bpp == 1 ? SGI_MGRAS_PIXEL_FORMAT_CI8 :
                                            SGI_MGRAS_PIXEL_FORMAT_RGB;

                if (x0 + (int)col <= x1) {
                    mgras_put_pixel(s, y * SGI_MGRAS_WIDTH + x0 + (int)col,
                                 color, op, mask, format);
                }
            }
        }
    }
    g_free(page_list);
    s->dirty = true;
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
 * A stored pixel is mgras_put_pixel()'s COLORMASKLSBSA-masked 24-bit value;
 * the byte extraction below is the exact inverse of the draw path's
 * pixel_bytes[0]<<24 | pixel_bytes[1]<<16 | pixel_bytes[2]<<8 |
 * pixel_bytes[3] assembly, most-significant byte first, so byte 0 of a
 * 4-byte pixel always reads back zero (the draw path's mask never lets
 * that byte reach the framebuffer).
 */
static void mgras_raster_dma_read_image(SGIMgrasState *s, uint32_t kick_byte)
{
    unsigned int slot, widget;
    uint64_t base;
    uint32_t list_node, list_pa, count;
    uint32_t pitch = s->dma_desc_20000[MGRAS_DESC_20000_REG_PITCH];
    uint64_t addr = mgras_raster_dma_src_offset(s);
    uint32_t rows = s->dma_desc_20000[MGRAS_DESC_20000_REG_ROWS];
    uint32_t bpr = s->dma_desc_20000[MGRAS_DESC_20000_REG_BPR];
    uint32_t xy0 = s->rss[MGRAS_RSS_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[MGRAS_RSS_BLOCKXYENDI];
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    int y_top, y_bottom;
    unsigned int width, bpp;
    uint64_t *page_list;

    if (!mgras_raster_dma_select_slot(s, kick_byte, &slot)) {
        return;
    }
    if (rows == 0 || bpr == 0 || x0 > x1 || x0 >= SGI_MGRAS_WIDTH ||
        y0 >= SGI_MGRAS_HEIGHT || y1 >= SGI_MGRAS_HEIGHT) {
        return;
    }
    x1 = MIN(x1, SGI_MGRAS_WIDTH - 1);
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

    y_top = SGI_MGRAS_HEIGHT - 1 - y0;
    y_bottom = SGI_MGRAS_HEIGHT - 1 - y1;
    if (y_top > y_bottom || rows != (uint32_t)(y_bottom - y_top + 1)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: DMA image transfer read box "
                      "does not match rows %u\n", rows);
        return;
    }

    base = sgi_mgras_dma_map_slot_base(s, slot);
    list_node = (uint32_t)(base >> 32) & 0xff;
    list_pa = (uint32_t)base & ~0x7u;
    count = sgi_mgras_dma_map_slot_count(s, slot);
    widget = (sgi_mgras_dma_map_slot_control(s, slot) >> 16) & 0xf;

    if (count == 0 || count > (1u << 20)) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: DMA source mapping has "
                      "unusable page count %u\n", count);
        return;
    }

    if (!mgras_raster_dma_load_page_list(s, widget, list_node, list_pa, count,
                                      &page_list)) {
        return;
    }

    for (uint32_t row = 0; row < rows; row++) {
        int y = y_top + (int)row;

        for (uint32_t b = 0; b < bpr; b++) {
            uint32_t col = b / bpp;
            int x = x0 + (int)col;
            uint64_t va, src_pa;
            uint64_t page_ix;
            hwaddr fabric;
            uint32_t stored;
            uint8_t byte;
            MemTxResult result;

            if (x > x1) {
                continue;
            }
            stored = s->front[y * SGI_MGRAS_WIDTH + x];
            byte = bpp == 1 ? (uint8_t)stored :
                (uint8_t)(stored >> (24 - 8 * (b % bpp)));

            va = addr + (uint64_t)row * pitch + b;
            page_ix = va >> 12;
            if (page_ix >= count) {
                qemu_log_mask(LOG_UNIMP, "sgi-mgras: DMA image transfer "
                              "destination page %" PRIu64 " exceeds mapped "
                              "count %u\n", page_ix, count);
                continue;
            }
            src_pa = ((uint64_t)list_node << 32) |
                     (page_list[page_ix] << 12) | (va & 0xfff);
            if (!sgi_mgras_xio_compose(s, widget, src_pa, &fabric)) {
                continue;
            }
            result = dma_memory_write(&s->fabric_as, fabric, &byte,
                                      sizeof(byte), MEMTXATTRS_UNSPECIFIED);
            if (result != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: DMA image "
                              "transfer destination write at 0x%" PRIx64
                              " failed (result %d)\n", (uint64_t)fabric,
                              result);
                g_free(page_list);
                return;
            }
        }
    }
    g_free(page_list);
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

static void mgras_raster_desc_bank_store(SGIMgrasState *s, uint32_t cmd_class,
                                      uint32_t reg, const uint32_t *payload,
                                      uint32_t payload_words)
{
    uint32_t *regs;
    uint32_t regs_count;

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
