/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/sgi-mgras-display.h"

/*
 * The CMAP slot this device's own live trace ties to the hardware cursor:
 * see si_composite_cursor's own header comment for the evidence. Not an
 * MMIO register offset -- a slot number into SGIMgrasState.cmap[], the
 * same array si_cmap_rgb below resolves any PseudoColor pixel through.
 */
#define SI_CURSOR_CMAP_INDEX 0x1c

/*
 * Resolve a CMAP slot to a front[]-format (0x00BBGGRR) colour -- the same
 * packing PACKEDCOLOR and front[] itself already use, so every caller
 * (the PseudoColor scanout loop and the cursor compositor below) can feed
 * the result through the one final byte-swap si_gfx_update already does
 * per pixel, instead of duplicating colour-map logic. CMAP entries are
 * assumed to share PACKEDCOLOR's own packing convention: never
 * independently confirmed for the colour-map array specifically, but
 * consistent with every other colour value this device stores.
 */
static uint32_t si_cmap_rgb(SGIMgrasState *s, uint8_t index)
{
    return s->cmap[index] & 0x00ffffff;
}
/*
 * Composites the 32x32 hardware cursor sprite on top of display_buffer,
 * which the caller (si_gfx_update) has already converted to
 * PIXMAN_x8r8g8b8. The sprite lives in VC3 SRAM as two 32x32 1-bit planes
 * (64 halfwords each, 128 total, MSB-first per row -- the same convention
 * si_draw_glyph_row already uses for CHAR_H) at the halfword address the
 * cursor-entry pointer (VC3 index 1) names -- established mechanism, see
 * the header comment on SGI_MGRAS_VC3_RAM and this project's own
 * datasheet. Position (cursor_x/cursor_y, VC3 indices 2/3) and enable
 * (cursor_enabled, VC3 index 0x1d) are already tracked; this function's
 * own job is only the part that was still missing -- actually drawing the
 * sprite into the scanout, which si_gfx_update only calls while enabled.
 *
 * Plane-to-colour mapping was this device's own bounded approximation
 * before this pass, and live evidence now settles it: reconstructing the
 * actual glyph a real cursor install left in VC3 SRAM
 * (/storage/nvme/origami/runs/irix-20260918T112956-2cabd8/qemu.trace, the
 * cursor's own base pointer at VC3 index 1) shows the two planes are
 * never both set for the same pixel -- "mask" traces a solid, growing
 * arrow-head silhouette (the classic pointer body) while "sel" traces a
 * complementary one-pixel border just outside it, together forming an
 * ordinary bordered arrow. The previous reading gated visibility on
 * "mask" alone and picked colour from "sel" alone; since the two planes
 * never overlap, every visible pixel had sel=0 there, so the compositor
 * always chose bg and never ink -- solid white/background colour with no
 * split at all, exactly the bug this pass fixes. The corrected model:
 * a pixel is visible when either plane is set (mask OR sel, not mask
 * alone -- the body and its border are both part of the glyph), coloured
 * ink where mask is set (the body) and bg where only sel is set (the
 * border); both planes clear is the only transparent case (outside the
 * glyph). Neither plane being set together in any traced row leaves that
 * fourth combination unevidenced; this device breaks the tie toward ink,
 * consistent with mask already gating visibility before this pass.
 *
 * The "ink" colour comes from CMAP slot 0x1c (SI_CURSOR_CMAP_INDEX):
 * live-tracing the same session found 46 writes there, every one
 * immediately following a cursor position/enable update (the same VC3
 * index 2/3/0x1d write burst MgrasPositionCursor makes) -- the tightest
 * live correlation this pass found to any single register, and a real,
 * evidenced cursor-colour source, resolved through the same si_cmap_rgb
 * path the CMAP-index scanout path above uses. The other colour is not
 * independently evidenced from this trace (no second CMAP slot showed the
 * same correlation): this device fills it with plain white, matching the
 * factory `-cursorBG white` default this same session was launched with,
 * a labelled constant rather than a second confirmed register. The
 * project's own design doc (docs/designs/2026-09-17-si-viz-pixel-output-
 * design.md, "Genuinely unresolved") names three CMAP-indexed cursor
 * colours, not two; this pass found live evidence for only one dynamic
 * slot (0x1c) and leaves the second as the same labelled white constant
 * as before -- a third colour stays unimplemented, not guessed at.
 *
 * CONTRACT CHANGE, needs independent review: the visibility rule (union,
 * not mask-alone) and the ink/bg plane assignment are corrected by this
 * pass against live SRAM content; the background colour and the missing
 * third colour remain this pass's own bounded reading, not a confirmed
 * VC3 cursor contract.
 */
static void si_composite_cursor(SGIMgrasState *s)
{
    uint32_t base = s->vc3_reg[1];
    uint32_t ink = si_cmap_rgb(s, SI_CURSOR_CMAP_INDEX);
    uint32_t bg = 0x00ffffff;

    for (unsigned row = 0; row < 32; row++) {
        uint32_t y = s->cursor_y + row;
        uint32_t mask_bits, sel_bits;

        if (y >= SGI_MGRAS_HEIGHT) {
            continue;
        }
        mask_bits = ((uint32_t)s->vc3_sram[(base + row * 2) &
                     (SGI_MGRAS_VC3_SRAM_WORDS - 1)] << 16) |
                    s->vc3_sram[(base + row * 2 + 1) &
                                (SGI_MGRAS_VC3_SRAM_WORDS - 1)];
        sel_bits = ((uint32_t)s->vc3_sram[(base + 64 + row * 2) &
                    (SGI_MGRAS_VC3_SRAM_WORDS - 1)] << 16) |
                   s->vc3_sram[(base + 64 + row * 2 + 1) &
                               (SGI_MGRAS_VC3_SRAM_WORDS - 1)];

        for (unsigned col = 0; col < 32; col++) {
            uint32_t x = s->cursor_x + col;
            bool mask_bit = (mask_bits >> (31 - col)) & 1;
            bool sel_bit = (sel_bits >> (31 - col)) & 1;
            uint32_t rgb;

            if (x >= SGI_MGRAS_WIDTH || (!mask_bit && !sel_bit)) {
                continue;
            }
            rgb = mask_bit ? ink : bg;
            s->display_buffer[y * SGI_MGRAS_WIDTH + x] =
                ((rgb & 0xff) << 16) | (rgb & 0x00ff00) | ((rgb >> 16) & 0xff);
        }
    }
}

/*
 * Convert front[] (PACKEDCOLOR register 0x05b's own 0x00BBGGRR packing -- see
 * si_draw_fill and docs/datasheets/mgras-hq4.md's 0x120400 row) into
 * display_buffer's PIXMAN_x8r8g8b8 (0x00RRGGBB): B and R swap byte
 * position, G is already in the right place. Called on whatever cadence
 * the active UI backend drives (the same pattern RAD4's rad4_gfx_update
 * uses, hw/display/psitech-rad4.c), so this only redoes the conversion,
 * or the blank fill, when dirty says something could actually be
 * different from what display_buffer already holds.
 *
 * A real VC3 blanks its output while the display-enable bit is clear
 * (SGI_MGRAS_VC3_INDEXDATA index 0x1e bit 2, SGIMgrasState.
 * display_enabled) rather than scanning out whatever front[] holds; this
 * device fills display_buffer black to match instead of showing stale or
 * partially-drawn content from before the guest enabled the display.
 *
 * PseudoColor-vs-TrueColor split (contract change, needs independent
 * review): a pixel whose stored byte (bits 7:0, the same position
 * PACKEDCOLOR calls RR) is a CMAP index the real driver writes, not a
 * direct colour, has to be looked up (si_read/si_write already accept and
 * store CMAP loads at 0x070c30/0x070d18) or an indexed pixel paints its
 * own raw index straight into the red channel -- a small integer like
 * 0x07 or 0x11, producing near-black/dark-red speckle instead of the real
 * desktop colour.
 *
 * This pass replaces an earlier version of this split that read the most
 * recently programmed XMAP main-mode word (SGI_MGRAS_XMAP_MAIN_MODE) as a
 * single device-global "is the whole frame PseudoColor" flag. Live-
 * tracing a full boot-to-desktop session end to end
 * (/storage/nvme/origami/runs/irix-20260918T112956-2cabd8/qemu.trace)
 * falsifies that flag as a classifier, not just as an ordering bug: the
 * real desktop's background rows paint with BKGRD_RG=0x11 while
 * SGI_MGRAS_XMAP_MAIN_MODE already reads 0x7a4 (this device's old
 * TrueColor bucket) at that exact fill, and the same mode word never
 * changes again before the frame this device captured -- so sampling it
 * at fill time instead of scan time would have made no difference. What
 * does settle it: this device's own CMAP state at that same point has
 * index 0x11 loaded with a real teal-ish colour that matches the
 * desktop's own background, while the flag's TrueColor reading painted
 * that index's raw byte value as near-black dark red -- confirmed live,
 * not inferred. The same mode word also covers an interleaved fill using
 * a plausible direct 24-bit colour (0xff0ff0), so no single global value,
 * sampled at any time, can be right for both -- a real per-window visual
 * class was never modelled at all, and a global mode flag cannot stand in
 * for one. si_fill_rect/si_draw_glyph_row now record which register
 * supplied each pixel's colour in s->wid[] (see si_fill_rect's own header
 * comment) at the point they paint it, and this loop reads that per-pixel
 * record instead of the device-global flag; SGI_MGRAS_XMAP_MAIN_MODE
 * itself is still stored and echoed back for read-back correctness, just
 * no longer consulted here.
 */
static bool si_gfx_update(void *opaque)
{
    SGIMgrasState *s = opaque;

    if (!s->dirty) {
        return true;
    }
    if (!s->display_enabled) {
        memset(s->display_buffer, 0,
              SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT * sizeof(*s->display_buffer));
        qemu_console_update_full(s->con);
        s->dirty = false;
        return true;
    }

    for (int i = 0; i < SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT; i++) {
        uint32_t v = s->front[i];
        uint32_t rgb = s->wid[i] ? si_cmap_rgb(s, v & 0xff) : v;

        s->display_buffer[i] = ((rgb & 0xff) << 16) | (rgb & 0x00ff00) |
                               ((rgb >> 16) & 0xff);
    }
    if (s->cursor_enabled) {
        si_composite_cursor(s);
    }
    qemu_console_update_full(s->con);
    s->dirty = false;
    return true;
}

const GraphicHwOps si_mgras_display_ops = {
    .gfx_update = si_gfx_update,
};

/*
 * qemu_graphic_console_create() starts the console on a small placeholder
 * surface of its own; recognize this device's own surface by its backing
 * buffer, the same check RAD4's rad4_ensure_surface uses
 * (hw/display/psitech-rad4.c), so a second call from si_post_load is a
 * no-op once si_realize has already installed the real one.
 */
void si_mgras_display_ensure_surface(SGIMgrasState *s)
{
    DisplaySurface *ds = qemu_console_surface(s->con);

    if (ds && surface_data(ds) == (uint8_t *)s->display_buffer) {
        return;
    }
    ds = qemu_create_displaysurface_from(SGI_MGRAS_WIDTH, SGI_MGRAS_HEIGHT,
                                         PIXMAN_x8r8g8b8,
                                         SGI_MGRAS_WIDTH * 4,
                                         (uint8_t *)s->display_buffer);
    qemu_console_set_surface(s->con, ds);
}
