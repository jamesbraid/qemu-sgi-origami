/* SPDX-License-Identifier: BSD-3-Clause */
/* Host scanout is derived from guest-visible state and is never its source. */
/* The display subset follows the SI visual-emulator architecture contract. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/sgi-mgras-display.h"


#define MGRAS_CURSOR_CMAP_INDEX 0x1c

/*
 * The VC3 cursor position registers hold the glyph's top-left pixel plus
 * 31 on each axis: the stock IRIX 6.5.30 server writes the pointer position
 * minus the hotspot plus 31, and the VC2's Linux console driver adds the
 * same 31 to its cursor Y.
 */
#define MGRAS_CURSOR_ORIGIN 31

/*
 * The VC3 takes each pixel's window ID (the XMAP entry it resolves
 * through) from display-ID tables in its SRAM: register 4 points at the
 * main planes' frame table, register 5 at the overlay's. A frame table
 * holds one word per scanline, and each word points at that line's run
 * list, whose 16-bit entries carry a starting column in bits 15:5 and a
 * window ID in bits 4:0. The first entry applies from column 0 and each
 * later one from its own column; a column past the screen edge (0xffe0)
 * ends the line. Table layout and register assignment: MAME's Newport
 * model at commit b1da4a1b278cf41300e95cce35b4d1f03a22023e.
 */
#define MGRAS_VC3_MAIN_DID_ENTRY 0x04
#define MGRAS_VC3_OVERLAY_DID_ENTRY 0x05
#define MGRAS_DID_COLUMN_SHIFT 5
#define MGRAS_DID_MASK 0x1f


/*
 * A WID selects an XMAP mode, whose bits 9:5 select the colour map. The
 * guest packs each entry left-justified, 0xRRGGBB00: every one of the
 * 24,708 observed live data-port writes has a zero low byte, and its
 * palette reads as an exact IRIS default when bits 31:24, 23:16 and 15:8
 * are taken as red, green and blue. The low byte's meaning is unknown.
 * Return the entry in the native 0x00BBGGRR order the callers convert.
 */
static uint32_t mgras_cmap_rgb(SGIMgrasState *s, uint8_t visual, uint8_t index)
{
    uint32_t map = (s->xmap[0].mode[visual & 0x1f] >> 5) & 0x1f;
    uint32_t entry = s->cmap[map * 256 + index];

    return ((entry & 0xff00) << 8) | ((entry >> 8) & 0xff00) | (entry >> 24);
}

/*
 * The IRIX desktop session's DDX loads a post-CMAP gamma-correction
 * table into the DAC after login. The shipped impactsr.so's dynamic
 * symbol table names the responsible routines mgrasLoadICOGammaRamps
 * and ico_progdac; see the header comment above SGI_MGRAS_DAC_LUT_INDEX
 * in sgi-mgras.h for the captured load protocol. Apply the table as a
 * final stage over any resolved colour, so indexed and, once modeled,
 * direct-colour pixels see the same correction. Operates on
 * mgras_cmap_rgb()'s native 0x00BBGGRR order.
 */
static uint32_t mgras_gamma_apply(SGIMgrasState *s, uint32_t bgr)
{
    uint32_t b = s->dac_gamma[2][(bgr >> 16) & 0xff];
    uint32_t g = s->dac_gamma[1][(bgr >> 8) & 0xff];
    uint32_t r = s->dac_gamma[0][bgr & 0xff];

    return (b << 16) | (g << 8) | r;
}

static void mgras_did_line(SGIMgrasState *s, unsigned int entry_reg,
                           unsigned int y, uint8_t *did)
{
    const uint16_t mask = SGI_MGRAS_VC3_SRAM_WORDS - 1;
    uint16_t ptr = s->vc3_sram[(s->vc3_reg[entry_reg] + y) & mask];
    uint16_t run = s->vc3_sram[ptr & mask];
    uint8_t current = run & MGRAS_DID_MASK;
    unsigned int steps = 0;

    run = s->vc3_sram[++ptr & mask];
    for (unsigned int x = 0; x < s->width; x++) {
        /* Bound the walk; a guest table can repeat a column forever. */
        while ((run >> MGRAS_DID_COLUMN_SHIFT) == x &&
               steps++ < s->width) {
            current = run & MGRAS_DID_MASK;
            run = s->vc3_sram[++ptr & mask];
        }
        did[x] = current;
    }
}

uint8_t sgi_mgras_display_did(SGIMgrasState *s, bool overlay, unsigned int x,
                              unsigned int y)
{
    uint8_t did[SGI_MGRAS_MAX_WIDTH];

    if (x >= s->width || y >= s->height) {
        return 0;
    }
    mgras_did_line(s, overlay ? MGRAS_VC3_OVERLAY_DID_ENTRY :
                   MGRAS_VC3_MAIN_DID_ENTRY, y, did);
    return did[x];
}

/*
 * A nonzero overlay index covers the main planes when the pixel's WID has
 * its overlay enabled, resolved through the map that WID's overlay mode
 * names in bits 7:3; index 0 is transparent. The X server's own
 * SERVER_OVERLAY_VISUALS property advertises transparent pixel 0 for its
 * overlay visual (guest console capture irix-20260926T050306-f62509,
 * console/ioc3-a.log line 272: visual 0x23, TransparentPixel, value 0),
 * and it never loads entry 0 of the overlay maps. The mode fields are
 * inferred (see SGI_MGRAS_XMAP_OVERLAY_MODE); an overlay drawn over a WID
 * whose overlay is disabled stays invisible.
 */
static bool mgras_overlay_bgr(SGIMgrasState *s, int i, uint8_t did,
                              uint32_t *bgr)
{
    uint8_t index = s->overlay[i];
    uint32_t mode = s->xmap[0].overlay_mode[did];
    uint32_t entry;

    if (!index || !(mode & 1)) {
        return false;
    }
    entry = s->cmap[((mode >> 3) & 0x1f) * 256 + index];
    *bgr = ((entry & 0xff00) << 8) | ((entry >> 8) & 0xff00) | (entry >> 24);
    return true;
}

/*
 * Resolve one pixel to mgras_cmap_rgb()'s native 0x00BBGGRR order, keyed by
 * the per-pixel format tag (sgi-mgras.h). CI8 indexes through the WID's
 * colour map as before. RGB is a direct 0x00RRGGBB triple a paint wrote
 * (see mgras_draw_fill() and its siblings in hw/misc/sgi-mgras-raster.c)
 * and bypasses the CMAP entirely; only its R and B bytes need swapping
 * to this function's native order. RGB12 is the 0xBGR nibble form a
 * 12-bit window's paint stores; each nibble widens to eight bits by
 * repeating itself, so 0xf is full scale. How the hardware widens a
 * four-bit channel is not known: shifting alone would show the white the
 * Icon Catalog draws its icons' bodies on as 240. Neither direct form's
 * main WID is read: no register or capture is known to source a map a
 * direct-colour pixel would use. A visible overlay pixel covers every
 * kind. A main display ID whose bit is set in the XMAP buffer-select
 * word under index 0 shows bank 1, which holds only the GL client's
 * direct colour (SGI_MGRAS_XMAP_BUF_SELECT in sgi-mgras.h).
 */
static uint32_t mgras_resolve_pixel_bgr(SGIMgrasState *s, int i,
                                        uint8_t main_did, uint8_t overlay_did)
{
    uint32_t overlay;

    if (mgras_overlay_bgr(s, i, overlay_did, &overlay)) {
        return overlay;
    }
    if ((s->xmap[0].buf_select[0] >> (main_did & MGRAS_DID_MASK)) & 1) {
        uint32_t rgb = s->back[i] & 0x00ffffff;

        return ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff);
    }
    if (s->format[i] == SGI_MGRAS_PIXEL_FORMAT_RGB) {
        uint32_t rgb = s->front[i] & 0x00ffffff;

        return ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff);
    }
    if (s->format[i] == SGI_MGRAS_PIXEL_FORMAT_RGB12) {
        uint32_t r = s->front[i] & 0xf;
        uint32_t g = (s->front[i] >> 4) & 0xf;
        uint32_t b = (s->front[i] >> 8) & 0xf;

        return (b * 0x11) << 16 | (g * 0x11) << 8 | r * 0x11;
    }
    return mgras_cmap_rgb(s, main_did, s->front[i] & 0xff);
}

/* mgras_cmap_rgb()'s native 0x00BBGGRR as the scanout's 0x00RRGGBB. */
static uint32_t mgras_bgr_to_rgb(uint32_t bgr)
{
    return ((bgr & 0xff) << 16) | (bgr & 0x00ff00) | ((bgr >> 16) & 0xff);
}

/*
 * The cursor's row on display line y, from the mask and select planes in
 * VC3 SRAM. It overwrites the pixels under it rather than blending, so it
 * is drawn here, from family state, not by the scanout's cursor.
 */
static void mgras_cursor_row(SGIMgrasState *s, unsigned y, uint32_t *dst)
{
    const uint16_t mask = SGI_MGRAS_VC3_SRAM_WORDS - 1;
    uint32_t base = s->vc3_reg[1];
    int row = (int)y - ((int)s->cursor_y - MGRAS_CURSOR_ORIGIN);
    uint32_t ink, mask_bits, sel_bits;

    if (row < 0 || row >= 32) {
        return;
    }
    ink = mgras_bgr_to_rgb(
        mgras_gamma_apply(s, mgras_cmap_rgb(s, 0, MGRAS_CURSOR_CMAP_INDEX)));
    mask_bits = ((uint32_t)s->vc3_sram[(base + row * 2) & mask] << 16) |
                s->vc3_sram[(base + row * 2 + 1) & mask];
    sel_bits = ((uint32_t)s->vc3_sram[(base + 64 + row * 2) & mask] << 16) |
               s->vc3_sram[(base + 64 + row * 2 + 1) & mask];

    for (unsigned col = 0; col < 32; col++) {
        int x = (int)s->cursor_x - MGRAS_CURSOR_ORIGIN + col;
        bool mask_bit = (mask_bits >> (31 - col)) & 1;
        bool sel_bit = (sel_bits >> (31 - col)) & 1;

        if (x < 0 || x >= s->width || (!mask_bit && !sel_bit)) {
            continue;
        }
        dst[x] = mask_bit ? ink : 0x00ffffff;
    }
}

/*
 * Display line y as the scanout presents it; blank while the VC3 has the
 * display disabled.
 *
 * A CI8 pixel resolves through the map its WID selects; an RGB pixel
 * bypasses the map. Either way the gamma table applies uniformly, so
 * every pixel reaches the display through the same final stage.
 */
void sgi_mgras_display_resolve_row(void *opaque, unsigned y, uint32_t *dst)
{
    SGIMgrasState *s = opaque;
    uint8_t main_did[SGI_MGRAS_MAX_WIDTH], overlay_did[SGI_MGRAS_MAX_WIDTH];

    if (!s->display_enabled) {
        memset(dst, 0, s->width * sizeof(*dst));
        return;
    }
    mgras_did_line(s, MGRAS_VC3_MAIN_DID_ENTRY, y, main_did);
    mgras_did_line(s, MGRAS_VC3_OVERLAY_DID_ENTRY, y, overlay_did);
    for (int x = 0; x < s->width; x++) {
        int i = y * s->width + x;

        dst[x] = mgras_bgr_to_rgb(mgras_gamma_apply(s,
            mgras_resolve_pixel_bgr(s, i, main_did[x], overlay_did[x])));
    }
    if (s->cursor_enabled) {
        mgras_cursor_row(s, y, dst);
    }
}

/*
 * The retrace period is the time the VC3 takes to walk its frame table
 * once. VID_EP (VC3 register 0) points at the frame table: pairs of
 * (line-sequence address, repeat count) ending at a count of 0. A line
 * sequence is a run of entries of one halfword (bit 7 set) or two, each
 * first halfword carrying a duration in VC3 clocks in bits 14:8; the line
 * ends at an entry whose halfwords all have bit 15 set. So one frame is
 * the sum of count x line length, and the IRIX 6.5.30 timing tables for
 * 1280x1024 at 60 Hz give 1065 lines of 840 clocks.
 *
 * The VC3 clock is half the pixel clock, which the RAMDAC's PLL makes from
 * a 33 MHz reference as
 *     f_ref * (4 * (v + 2) + cntl[7:6]) /
 *     ((r + 2) * 2^cntl[5:4] * 2^cntl[2])
 * from DAC registers 0x0c (r), 0x0f (v) and 0x09 (cntl). IRIX programs
 * r = 2, v = 4, cntl = 0x91: 107.25 MHz, so 894,600 clocks at 53.625 MHz,
 * 59.943 Hz rather than the nominal 60.
 *
 * This is inferred, not established hardware behaviour: the table walk
 * from every timing table IRIX ships and MAME's VC2 model, the PLL
 * relation and the clock ratio from the rates SGI's timing compiler
 * records for those tables. See the timing section of
 * docs/datasheets/mgras-raster-display.md in the Origami repository.
 *
 * The kernel sets 0x072408 bit 24 when a timing file asks for the other
 * PLL reference, but no shipped file does, and nothing gives that
 * reference's frequency. The model's 64 MHz for it is unknown, not
 * inferred: a placeholder no stock mode reaches.
 */
#define MGRAS_VC3_VID_EP 0x00
#define MGRAS_VT_DURATION(h) (((h) >> 8) & 0x7f)
#define MGRAS_VT_SINGLE 0x0080
#define MGRAS_VT_END 0x8000
/* Bounds the line walks; a stock table reads a few hundred halfwords. */
#define MGRAS_VT_WALK_BUDGET (SGI_MGRAS_VC3_SRAM_WORDS * 4)

#define MGRAS_DAC_PLL_R 0x0c00
#define MGRAS_DAC_PLL_V 0x0f00
#define MGRAS_DAC_PLL_CNTL 0x0900
#define MGRAS_PLL_REF_HZ 33000000u
#define MGRAS_PLL_REF_ALT_HZ 64000000u
#define MGRAS_PLL_REF_SELECT (1u << 24)

/*
 * Before the guest loads a table, the model runs the stock 1280x1024
 * 60 Hz timing: the X server's default timing file, and the IRIX 6.5.30
 * kernel's built-in records 0 and 3, which its board initialisation loads
 * for every monitor type but 0x01, 0x09, 0x0b (72 Hz) and 0x82 (1024x768).
 * That is an approximation, since what an unprogrammed VC3 does is
 * unknown.
 */
#define MGRAS_DEFAULT_FRAME_CLOCKS 894600
#define MGRAS_DEFAULT_VC3_CLOCK_HZ 53625000

/* The line sequence's length in VC3 clocks, or -1 if it does not end. */
static int64_t mgras_vt_line_clocks(SGIMgrasState *s, uint16_t addr,
                                    unsigned int *budget)
{
    const uint16_t mask = SGI_MGRAS_VC3_SRAM_WORDS - 1;
    int64_t clocks = 0;

    while (*budget) {
        uint16_t first = s->vc3_sram[addr++ & mask];
        bool end = first & MGRAS_VT_END;

        (*budget)--;
        clocks += MGRAS_VT_DURATION(first);
        if (!(first & MGRAS_VT_SINGLE)) {
            end = end && (s->vc3_sram[addr++ & mask] & MGRAS_VT_END);
        }
        if (end) {
            return clocks;
        }
    }
    return -1;
}

/* VC3 clocks per pass of the frame table, or 0 if it gives no frame. */
static uint64_t mgras_vt_frame_clocks(SGIMgrasState *s)
{
    const uint16_t mask = SGI_MGRAS_VC3_SRAM_WORDS - 1;
    uint16_t ep = s->vc3_reg[MGRAS_VC3_VID_EP];
    unsigned int budget = MGRAS_VT_WALK_BUDGET;
    uint64_t frame = 0;

    for (unsigned int i = 0; i < SGI_MGRAS_VC3_SRAM_WORDS / 2; i++) {
        uint16_t line = s->vc3_sram[(ep + 2 * i) & mask];
        uint16_t count = s->vc3_sram[(ep + 2 * i + 1) & mask];
        int64_t clocks;

        if (!count) {
            return frame;
        }
        clocks = mgras_vt_line_clocks(s, line, &budget);
        if (clocks < 0) {
            return 0;
        }
        frame += count * (uint64_t)clocks;
    }
    return 0;
}

/*
 * frame_clocks / clock_hz seconds in lowest terms; the scanout takes each
 * as 32 bits, so a ratio that still does not fit loses its low bits.
 */
static void mgras_timing_ratio(SGIScanoutMode *mode, uint64_t clocks,
                               uint64_t hz)
{
    uint64_t a = clocks, b = hz;

    while (b) {
        uint64_t t = a % b;

        a = b;
        b = t;
    }
    clocks /= a;
    hz /= a;
    while (clocks > UINT32_MAX || hz > UINT32_MAX) {
        clocks = (clocks + 1) >> 1;
        hz = (hz + 1) >> 1;
    }
    mode->frame_clocks = clocks;
    mode->clock_hz = hz;
}

void sgi_mgras_display_timing(SGIMgrasState *s, SGIScanoutMode *mode,
                              bool log)
{
    uint64_t frame = mgras_vt_frame_clocks(s);
    uint64_t r = s->dac_reg[MGRAS_DAC_PLL_R];
    uint64_t v = s->dac_reg[MGRAS_DAC_PLL_V];
    uint8_t cntl = s->dac_reg[MGRAS_DAC_PLL_CNTL];
    uint64_t ref = s->reset_control & MGRAS_PLL_REF_SELECT ?
                   MGRAS_PLL_REF_ALT_HZ : MGRAS_PLL_REF_HZ;
    uint64_t pix_num = ref * (4 * (v + 2) + (cntl >> 6));
    uint64_t pix_den = (r + 2) << ((cntl >> 4) & 3) << ((cntl >> 2) & 1);

    mode->width = s->width;
    mode->height = s->height;
    if (!frame) {
        if (log) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: the VC3 frame table "
                          "at 0x%04x gives no frame length; keeping the "
                          "default timing\n",
                          s->vc3_reg[MGRAS_VC3_VID_EP]);
        }
        mgras_timing_ratio(mode, MGRAS_DEFAULT_FRAME_CLOCKS,
                           MGRAS_DEFAULT_VC3_CLOCK_HZ);
        return;
    }
    /* frame VC3 clocks at half the pixel clock: 2 * frame / f_pix. */
    mgras_timing_ratio(mode, 2 * frame * pix_den, pix_num);
}

/*
 * Every change to state a frame resolves from sets s->dirty: VC3 tables
 * and registers, colour maps, gamma, XMAP state and each drawing path.
 * The model has no finer damage than that, so the whole frame is marked
 * for the scanout to resolve again at its next update.
 */
void sgi_mgras_display_flush(SGIMgrasState *s)
{
    if (s->dirty) {
        s->dirty = false;
        sgi_scanout_invalidate_all(&s->scanout);
    }
}
