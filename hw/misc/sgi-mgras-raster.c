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




#define SI_CMD_SELECTOR_MASK 0x1fff803f
#define SI_CMD_SELECTOR_STATE 0x60001
#define SI_CMD_SELECTOR_EXEC 0x70001


#define SI_CMD_FIFO_FENCE_CLASS 0x000e
#define SI_CMD_FIFO_FENCE_REG 0x04
#define SI_HQ_FLAG_FENCE 0x00010000


#define SI_CMD_HDR_CLASS_STATE 0x0018
#define SI_CMD_HDR_CLASS_EXEC 0x001c
#define SI_CMD_HDR_CLASS_DISCARD SGI_MGRAS_CMD_TOKEN_DISCARD


#define SI_REG_BLOCKXYSTARTI 0x046
#define SI_REG_BLOCKXYENDI 0x047
#define SI_REG_PACKEDCOLOR 0x05b
#define SI_REG_CHAR_H 0x070
#define SI_REG_CHAR_L 0x071
#define SI_REG_BKGRD_RG 0x140
#define SI_REG_BKGRD_BA 0x141
#define SI_REG_PP1FILLMODE 0x161
#define SI_REG_COLORMASKMSBS 0x162


#define SI_REG_RE_POINT 0x040
#define SI_REG_RE_LINE 0x041
#define SI_REG_IR_ALIAS 0x045


#define SI_REG_ALPHA 0x05f
#define SI_REG_COLORMASKLSBSA 0x163
#define SI_REG_GLINE_XSTARTF 0x00c
#define SI_REG_DRBPOINTERS 0x16d


#define SI_REG_COLORMASKLSBSB 0x164

/*
 * FILLMODE (OpenBSD impact(4) driver name) picks what a CHAR_H/CHAR_L
 * execute paints. The two values below are the ones a live capture
 * observed (see tests/data/mgras/char-modes.replay); any other value, or
 * an execute on the register that mode does not use, stays unimplemented
 * and inert.
 */
#define SI_REG_FILLMODE 0x110
#define SI_FILLMODE_GLYPH_ROW 0x10400008
#define SI_FILLMODE_BYTE_XFER 0x10c00000

/*
 * Byte-transfer mode's own register 0x159 picks which of two guest-side
 * packings a CHAR_L execute uses. Inferred from exactly two observed
 * values, not read hardware state: 0x8000 selects the eight-pixel path
 * below (the Toolchest boxes and the desktop's larger transfers, see
 * tests/data/mgras/char-modes.replay) and 0x0080 selects two indexed
 * pixels per execute (the xdm chooser's own icons, see
 * tests/data/mgras/root-icon.replay). Any other value stays unimplemented
 * and inert.
 */
#define SI_REG_BYTE_XFER_PACKING 0x159
#define SI_BYTE_XFER_PACKING_BYTES 0x8000
#define SI_BYTE_XFER_PACKING_PAIRS 0x0080

/*
 * FILLMODE bit 20 or 22 also sends a rectangle fill to BKGRD_RG instead of
 * PACKEDCOLOR. Inferred: the stock X server sets one of them for every
 * rectangle it fills and leaves PACKEDCOLOR zero, while OpenBSD fills with
 * FILLMODE zero and its colour in PACKEDCOLOR. Which bit means what is
 * unknown.
 */
#define SI_FILLMODE_BACKGROUND_COLOR 0x00500000

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
static void si_raster_rss_write(SGIMgrasState *s, uint32_t reg,
                                uint32_t value);
/* Unimplemented execute registers log and leave state unchanged. */
static void si_raster_command_execute(
    SGIMgrasState *s, const SGIMgrasRasterSubmission *submission);
/* State payloads are one word; execute headers dispatch primitives. */
static void si_raster_command_fifo_dispatch(SGIMgrasState *s);


static void si_command_fifo_token(SGIMgrasState *s, uint32_t token)
{
    uint32_t cmd_class, reg, payload_bytes, true_class;

    if (s->cmd_token_words_remaining > 0) {
        if (s->cmd_token_class != SI_CMD_HDR_CLASS_DISCARD &&
            s->cmd_token_payload_count <
            SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS) {
            s->cmd_token_payload[s->cmd_token_payload_count++] = token;
        }
        if (--s->cmd_token_words_remaining == 0 &&
            s->cmd_token_class != SI_CMD_HDR_CLASS_DISCARD) {
            si_raster_command_fifo_dispatch(s);
        }
        return;
    }

    cmd_class = token >> 16;
    reg = (token >> 8) & 0xff;
    payload_bytes = token & 0xff;

    if (cmd_class == SI_CMD_FIFO_FENCE_CLASS && reg == SI_CMD_FIFO_FENCE_REG &&
        payload_bytes == 0) {
        s->hq_flags |= SI_HQ_FLAG_FENCE;
        return;
    }

    true_class = cmd_class & ~1u;
    if (true_class == SI_CMD_HDR_CLASS_STATE ||
        true_class == SI_CMD_HDR_CLASS_EXEC) {
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
            s->cmd_token_class = SI_CMD_HDR_CLASS_DISCARD;
            return;
        }
        if (s->cmd_token_words_remaining == 0) {
            si_raster_command_fifo_dispatch(s);
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
    s->cmd_token_class = SI_CMD_HDR_CLASS_DISCARD;
    s->cmd_token_payload_count = 0;
    s->cmd_token_words_remaining = payload_bytes / 4;
    if (s->cmd_token_words_remaining > SGI_MGRAS_CMD_TOKEN_MAX_PAYLOAD_WORDS) {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: command-FIFO token, class "
                      "0x%04x reg 0x%02x payload %u bytes exceeds %zu-byte "
                      "limit\n", cmd_class, reg, payload_bytes,
                      sizeof(s->cmd_token_payload));
    }
}

void si_raster_command(SGIMgrasState *s, uint64_t word)
{
    uint32_t selector = (word >> 34) & 0xffffff;
    uint32_t value = word;
    uint32_t reg;

    if (s->cmd_token_words_remaining == 0) {

        bool state = (selector & SI_CMD_SELECTOR_MASK) == SI_CMD_SELECTOR_STATE;
        bool exec = (selector & SI_CMD_SELECTOR_MASK) == SI_CMD_SELECTOR_EXEC;

        if (state || exec) {
            SGIMgrasRasterSubmission submission = {
                .form = SGI_MGRAS_RASTER_DIRECT,
                .header = (uint32_t)(word >> 32),
                .reg = (selector >> 6) & 0x1ff,
                .payload = &value,
                .payload_words = 1,
            };

            reg = (selector >> 6) & 0x1ff;
            si_raster_rss_write(s, reg, value);
            if (exec) {
                si_raster_command_execute(s, &submission);
            }
            return;
        }
    }

    si_command_fifo_token(s, (uint32_t)(word >> 32));
    si_command_fifo_token(s, (uint32_t)word);
}

static uint32_t si_logic_op(uint8_t op, uint32_t dst, uint32_t src)
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

static uint32_t si_paint_pixel(uint8_t op, uint32_t dst, uint32_t src,
                               uint32_t mask)
{
    uint32_t result = si_logic_op(op, dst, src) & 0x00ffffff;
    return (result & mask) | (dst & ~mask);
}

/* A paint writes colour only. The WID plane keeps whatever tagged it. */
static void si_put_pixel(SGIMgrasState *s, unsigned int idx, uint32_t color,
                         uint8_t op, uint32_t mask)
{
    if (!(mask & 0x00ffffff)) {
        return;
    }
    s->front[idx] = si_paint_pixel(op, s->front[idx], color, mask);
}


/* Normalize corners and clip. Returns false when nothing is visible. */
static bool si_rect_bounds(uint32_t xy0, uint32_t xy1, int *x_lo, int *y_lo,
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


static void si_fill_rect(SGIMgrasState *s, uint32_t xy0, uint32_t xy1,
                         uint32_t color, uint8_t op, uint32_t mask)
{
    int x_lo, y_lo, x_hi, y_hi;

    if (!si_rect_bounds(xy0, xy1, &x_lo, &y_lo, &x_hi, &y_hi)) {
        return;
    }
    for (int y = y_lo; y <= y_hi; y++) {
        for (int x = x_lo; x <= x_hi; x++) {
            si_put_pixel(s, y * SGI_MGRAS_WIDTH + x, color, op, mask);
        }
    }
}


static void si_fill_wid_rect(SGIMgrasState *s, uint32_t xy0, uint32_t xy1,
                             uint8_t wid)
{
    int x_lo, y_lo, x_hi, y_hi;

    if (!si_rect_bounds(xy0, xy1, &x_lo, &y_lo, &x_hi, &y_hi)) {
        return;
    }
    for (int y = y_lo; y <= y_hi; y++) {
        memset(&s->wid[y * SGI_MGRAS_WIDTH + x_lo], wid, x_hi - x_lo + 1);
    }
}


static void si_draw_fill(SGIMgrasState *s)
{
    bool background = s->rss[SI_REG_FILLMODE] & SI_FILLMODE_BACKGROUND_COLOR;
    uint32_t color = (background ? s->rss[SI_REG_BKGRD_RG] :
                      s->rss[SI_REG_PACKEDCOLOR]) & 0x00ffffff;
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;

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
    if (s->rss[SI_REG_COLORMASKMSBS]) {
        si_fill_wid_rect(s, s->rss[SI_REG_BLOCKXYSTARTI],
                         s->rss[SI_REG_BLOCKXYENDI],
                         s->rss[SI_REG_DRBPOINTERS] >> 16);
    }
    si_fill_rect(s, s->rss[SI_REG_BLOCKXYSTARTI], s->rss[SI_REG_BLOCKXYENDI],
                color, op, mask);
}


static void si_draw_ir_alias_fill(SGIMgrasState *s)
{
    uint32_t color = s->rss[SI_REG_BKGRD_RG] & 0x00ffffff;
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;

    si_fill_rect(s, s->rss[SI_REG_BLOCKXYSTARTI], s->rss[SI_REG_BLOCKXYENDI],
                color, op, mask);
}


/*
 * FILLMODE_GLYPH_ROW paints one row of up to 64 columns per execute: CHAR_H
 * bits 31:0 are columns 0-31 and CHAR_L bits 31:0 are columns 32-63, both
 * MSB first, combined into one 64-bit row so either register's execute bit
 * can trigger the same paint. A CHAR_H-only execute (rows of 32 columns or
 * fewer) reads CHAR_L's possibly-stale value too, but the width clamp below
 * never indexes past column 31 in that case, so no stale bit is ever
 * painted. Colour roles (set bit -> PACKEDCOLOR, clear bit -> BKGRD_RG)
 * carry over from the narrower rule this replaces.
 */
static void si_draw_glyph_row(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[SI_REG_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[SI_REG_BLOCKXYENDI];
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    uint32_t fg = s->rss[SI_REG_PACKEDCOLOR] & 0x00ffffff;
    uint32_t bg = s->rss[SI_REG_BKGRD_RG] & 0x00ffffff;
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;
    uint64_t bits = ((uint64_t)s->rss[SI_REG_CHAR_H] << 32) |
                    s->rss[SI_REG_CHAR_L];
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
        si_put_pixel(s, y * SGI_MGRAS_WIDTH + x0 + i, set ? fg : bg, op,
                     mask);
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
 * state.
 */
static void si_draw_byte_row_bytes(SGIMgrasState *s, int x0, int x1,
                                   int y0, int y1, uint32_t mask, uint8_t op)
{
    uint32_t char_h = s->rss[SI_REG_CHAR_H];
    uint32_t char_l = s->rss[SI_REG_CHAR_L];
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
            si_put_pixel(s, y * SGI_MGRAS_WIDTH + x0 + col, bytes[i], op,
                         mask);
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
static void si_draw_byte_row_pairs(SGIMgrasState *s, int x0, int x1,
                                   int y0, int y1, uint32_t mask, uint8_t op)
{
    uint8_t pixels[2] = {
        (uint8_t)s->rss[SI_REG_CHAR_H],
        (uint8_t)s->rss[SI_REG_CHAR_L],
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
            si_put_pixel(s, y * SGI_MGRAS_WIDTH + x0 + col, pixels[i], op,
                         mask);
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
 * SI_REG_BYTE_XFER_PACKING comment above.
 */
static void si_draw_byte_row(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[SI_REG_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[SI_REG_BLOCKXYENDI];
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;
    uint32_t packing = s->rss[SI_REG_BYTE_XFER_PACKING];

    if (x0 > x1 || y0 > y1 || x0 >= SGI_MGRAS_WIDTH || y0 >= SGI_MGRAS_HEIGHT) {
        return;
    }
    x1 = MIN(x1, SGI_MGRAS_WIDTH - 1);
    y1 = MIN(y1, SGI_MGRAS_HEIGHT - 1);

    /*
     * The real xdm chooser ORs a row-padding field (bits 16:14) into this
     * word alongside the packing selector bit, e.g. 0x00010080 or
     * 0x00018000 rather than the bare selector value (blockcopy-grammar.md).
     * Test the selector bits, not the whole register, so those padded
     * values still match their packing mode instead of falling through to
     * the unimplemented default.
     */
    if (packing & SI_BYTE_XFER_PACKING_BYTES) {
        si_draw_byte_row_bytes(s, x0, x1, y0, y1, mask, op);
    } else if (packing & SI_BYTE_XFER_PACKING_PAIRS) {
        si_draw_byte_row_pairs(s, x0, x1, y0, y1, mask, op);
    } else {
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented byte-transfer "
                      "packing, register 0x159 value 0x%08x\n", packing);
    }
}

/* Route a CHAR_H/CHAR_L execute by FILLMODE; anything else stays inert. */
static void si_char_execute(SGIMgrasState *s, uint32_t reg)
{
    uint32_t mode = s->rss[SI_REG_FILLMODE];

    if (mode == SI_FILLMODE_GLYPH_ROW) {
        si_draw_glyph_row(s);
        s->dirty = true;
        return;
    }
    if (mode == SI_FILLMODE_BYTE_XFER && reg == SI_REG_CHAR_L) {
        si_draw_byte_row(s);
        s->dirty = true;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented CHAR execute, "
                  "register 0x%03x, fillmode 0x%08x\n", reg, mode);
}


/* BKGRD_BA execute paints two pixels and advances the persistent cursor. */
static void si_draw_pio_pixel_pair(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[SI_REG_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[SI_REG_BLOCKXYENDI];
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    uint32_t pixels[2] = {
        s->rss[SI_REG_BKGRD_RG] & 0x00ffffff,
        s->rss[SI_REG_BKGRD_BA] & 0x00ffffff,
    };
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;

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
        si_put_pixel(s, y * SGI_MGRAS_WIDTH + x, pixels[i], op, mask);
        s->pio_x++;
        if (x0 + s->pio_x > x1) {
            s->pio_x = 0;
            s->pio_y++;
        }
    }
}


static void si_draw_point(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[SI_REG_BLOCKXYSTARTI];
    uint32_t color = s->rss[SI_REG_PACKEDCOLOR] & 0x00ffffff;
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;
    int x = (xy0 >> 16) & 0xffff, y = xy0 & 0xffff;

    if (x >= SGI_MGRAS_WIDTH || y >= SGI_MGRAS_HEIGHT) {
        return;
    }
    si_put_pixel(s, y * SGI_MGRAS_WIDTH + x, color, op, mask);
}


static void si_draw_line(SGIMgrasState *s)
{
    uint32_t xy0 = s->rss[SI_REG_BLOCKXYSTARTI];
    uint32_t xy1 = s->rss[SI_REG_BLOCKXYENDI];
    uint32_t color = s->rss[SI_REG_PACKEDCOLOR] & 0x00ffffff;
    uint32_t mask = s->rss[SI_REG_COLORMASKLSBSA];
    uint8_t op = s->rss[SI_REG_PP1FILLMODE] >> 26;
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
            si_put_pixel(s, y0 * SGI_MGRAS_WIDTH + x0, color, op, mask);
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
}

static void si_raster_command_execute(
    SGIMgrasState *s, const SGIMgrasRasterSubmission *submission)
{
    switch (submission->reg) {
    case SI_REG_RE_POINT:
        si_draw_point(s);
        s->dirty = true;
        return;
    case SI_REG_RE_LINE:
        si_draw_line(s);
        s->dirty = true;
        return;
    case SI_REG_BLOCKXYENDI:
        si_draw_fill(s);
        s->dirty = true;
        return;
    case SI_REG_IR_ALIAS:
        if (submission->form == SGI_MGRAS_RASTER_FIFO) {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented command-FIFO "
                          "execute, register 0x%03x\n", submission->reg);
            return;
        }
        si_draw_ir_alias_fill(s);
        s->dirty = true;
        return;
    case SI_REG_CHAR_H:
    case SI_REG_CHAR_L:
        si_char_execute(s, submission->reg);
        return;
    case SI_REG_BKGRD_BA:
        si_draw_pio_pixel_pair(s);
        s->dirty = true;
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-mgras: unimplemented direct-command "
                      "execute, register 0x%03x\n", submission->reg);
        return;
    }
}


static void si_raster_rss_write(SGIMgrasState *s, uint32_t reg, uint32_t value)
{
    s->rss[reg] = value;
    if (reg == SI_REG_BLOCKXYSTARTI) {

        s->glyph_row = 0;
        s->glyph_unit = 0;
        s->pio_x = s->pio_y = 0;
    }
}


static void si_raster_command_fifo_dispatch(SGIMgrasState *s)
{
    uint32_t reg = s->cmd_token_reg;
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

    if (s->cmd_token_class == SI_CMD_HDR_CLASS_STATE) {
        if (s->cmd_token_payload_count >= 1) {
            si_raster_rss_write(s, reg, s->cmd_token_payload[0]);
        } else {
            qemu_log_mask(LOG_UNIMP, "sgi-mgras: state-write command-FIFO "
                          "header, register 0x%03x with no payload word\n",
                          reg);
        }
        return;
    }

    si_raster_command_execute(s, &submission);
}
