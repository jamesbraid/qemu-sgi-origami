/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host scanout is derived from guest-visible state and is never its source. */
/* The display subset follows the SI visual-emulator architecture contract. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-mgras-display.h"


#define MGRAS_CURSOR_CMAP_INDEX 0x1c


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
    uint32_t map = (s->xmap_mode[visual & 0x1f] >> 5) & 0x1f;
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

/*
 * Resolve one pixel to mgras_cmap_rgb()'s native 0x00BBGGRR order, keyed by
 * the per-pixel format tag (sgi-mgras.h). CI8 indexes through the WID's
 * colour map as before. RGB is a direct 0x00RRGGBB triple a paint wrote
 * (see mgras_draw_fill() and its siblings in hw/misc/sgi-mgras-raster.c)
 * and bypasses the CMAP entirely; only its R and B bytes need swapping
 * to this function's native order. The WID plane still tags an RGB
 * pixel (fills tag it unconditionally, see mgras_draw_fill()), but nothing
 * here reads that tag: no register or capture is known to source a map
 * a direct-colour pixel would use.
 */
static uint32_t mgras_resolve_pixel_bgr(SGIMgrasState *s, int i)
{
    if (s->format[i] == SGI_MGRAS_PIXEL_FORMAT_RGB) {
        uint32_t rgb = s->front[i] & 0x00ffffff;

        return ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff);
    }
    return mgras_cmap_rgb(s, s->wid[i], s->front[i] & 0xff);
}

/* The cursor uses mask and select planes from VC3 SRAM. */
static void mgras_composite_cursor(SGIMgrasState *s)
{
    uint32_t base = s->vc3_reg[1];
    uint32_t ink =
        mgras_gamma_apply(s, mgras_cmap_rgb(s, 0, MGRAS_CURSOR_CMAP_INDEX));
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


/* Convert native pixels to the host surface and blank when disabled. */
static bool mgras_gfx_update(void *opaque)
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

    /*
     * A CI8 pixel resolves through the map its WID selects; an RGB pixel
     * bypasses the map. Either way the gamma table applies uniformly, so
     * every pixel reaches the display through the same final stage.
     */
    for (int i = 0; i < SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT; i++) {
        uint32_t rgb = mgras_gamma_apply(s, mgras_resolve_pixel_bgr(s, i));

        s->display_buffer[i] = ((rgb & 0xff) << 16) | (rgb & 0x00ff00) |
                               ((rgb >> 16) & 0xff);
    }
    if (s->cursor_enabled) {
        mgras_composite_cursor(s);
    }
    qemu_console_update_full(s->con);
    s->dirty = false;
    return true;
}

const GraphicHwOps sgi_mgras_display_ops = {
    .gfx_update = mgras_gfx_update,
};


/* The host surface wraps display_buffer without changing guest state. */
void sgi_mgras_display_ensure_surface(SGIMgrasState *s)
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
