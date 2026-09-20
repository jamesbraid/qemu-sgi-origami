/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host scanout is derived from guest-visible state and is never its source. */
/* The display subset follows the SI visual-emulator architecture contract. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-mgras-display.h"


#define SI_CURSOR_CMAP_INDEX 0x1c


/* CMAP entries use the native 0x00BBGGRR packing. */
static uint32_t si_cmap_rgb(SGIMgrasState *s, uint8_t index)
{
    return s->cmap[index] & 0x00ffffff;
}

/* The cursor uses mask and select planes from VC3 SRAM. */
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


/* Convert native pixels to the host surface and blank when disabled. */
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


/* The host surface wraps display_buffer without changing guest state. */
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
