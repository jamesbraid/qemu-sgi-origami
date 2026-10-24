/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Bounded native X GC decoding for the SGI display model.
 * See the InfiniteReality command grammar's GC forms.
 * Canonical CI8/RGB8 values are model subsets, not visual identities.
 */
#include "qemu/osdep.h"
#include "hw/display/sgi/x-gc.h"
#include "hw/display/sgi/board-memory.h"

uint32_t sgi_x_logic_op(unsigned gx, uint32_t source, uint32_t destination)
{
    /* The board-memory helper masks op, while invalid GL enums use source. */
    return gx <= 15 ? sgi_logic_op(gx, source, destination) : source;
}

static uint32_t index_value(uint32_t high)
{
    return ((high >> 4) & 0xfff0) | ((high >> 3) & 0xf);
}

static uint32_t index_mask(uint32_t word)
{
    return ((word & 0xfff) << 4) | ((word >> 20) & 0xf);
}

/*
 * Whether a 12-bit colour field is a widened component: an 8-bit one
 * b << 4 | b >> 4 for 24-bit TrueColor, a 10-bit one c << 2 | c >> 8 for
 * 30-bit TrueColor (kon_parseGC, konal.so 0x541d1c-0x541f78).
 */
static bool canonical_component(unsigned component, bool rgb30)
{
    if (rgb30) {
        unsigned c = component >> 2;

        return component == ((c << 2) | (c >> 8));
    } else {
        unsigned byte = component >> 4;

        return component == ((byte << 4) | (byte >> 4));
    }
}

/*
 * The model keeps eight bits a component: a 30-bit TrueColor component's
 * low two bits are dropped.
 */
static bool canonical_rgb(uint64_t color, bool rgb30, uint32_t *out)
{
    const uint64_t fields = (0xfffULL << 40) | (0xfffULL << 27) |
                            (0xfffULL << 14) | 0x1ffe;
    unsigned alpha = (color >> 1) & 0xfff;

    if ((color & ~fields) || (alpha != 0 && alpha != 0xfff)) {
        return false;
    }
    *out = 0;
    for (unsigned k = 1; k <= 3; k++) {
        unsigned component = (color >> (13 * k + 1)) & 0xfff;

        if (!canonical_component(component, rgb30)) {
            return false;
        }
        *out |= (component >> 4) << (8 * (k - 1));
    }
    return true;
}

static SGIXGCStatus decode_did(const uint32_t *w, SGIXGC *gc)
{
    bool high = w[0] || w[4], low = w[1] || w[5];

    gc->target = SGI_X_GC_DID;
    gc->width_mask = 0xffff;
    if (w[6] != 8 || w[7] != 1 || w[8] || w[9] != 6 ||
        w[10] || w[11] != 1 || w[12] != 1 || w[2] || w[3]) {
        return SGI_X_GC_BAD_FIELDS;
    }
    if (high && low) {
        return SGI_X_GC_MIXED_DID;
    }
    if (w[0] & ~0xfff78U || w[1] & ~0x3ffde00U) {
        return SGI_X_GC_BAD_COLOR;
    }
    if (w[4] & ~0xf00fffU || w[5] & ~0xfff00fU) {
        return SGI_X_GC_BAD_MASK;
    }
    if (high) {
        gc->foreground = index_value(w[0]);
        gc->mask = index_mask(w[4]);
    } else {
        gc->foreground = ((w[1] >> 9) & 0xf) |
                         ((w[1] >> 10) & 0xfff0);
        gc->mask = ((w[5] >> 12) & 0xfff) | ((w[5] & 0xf) << 12);
    }
    return SGI_X_GC_OK;
}

static SGIXGCStatus decode(const uint32_t words[SGI_X_GC_WORDS],
                           SGIXGC *gc, bool native_index)
{
    const uint32_t *w = words;
    uint32_t plane = w[6] & 0x3c;
    uint32_t mask_word = w[4] & ~0x80000000U;
    /*
     * kon_parseGC copies the buffer selector in op bits 1:0 into bits 3:2
     * for a format with bit 0x80000 (konal.so 0x542e98-0x542ec0). What
     * the second copy selects is unknown; a copy equal to the first is
     * read as the same selection, and any other value is deferred.
     */
    uint32_t op = ((w[7] >> 2) & 3) == (w[7] & 3) ? w[7] & ~0xcU : w[7];
    uint32_t residual = op & ~0xfc0003U;
    bool geometry = w[10] == 0;
    bool rgb30 = w[6] == 0 && (w[10] == 0 || w[10] == 9);

    memset(gc, 0, sizeof(*gc));
    memcpy(gc->raw, w, sizeof(gc->raw));
    if (w[9] > 30 || (w[9] & 1)) {
        return SGI_X_GC_BAD_FIELDS;
    }
    gc->gx = w[9] / 2;
    gc->first_selector = w[7] & 3;
    gc->second_selector = (w[7] >> 18) & 0x3f;
    gc->opaque_or_tiled = w[4] & 0x80000000U;

    if (w[6] == 8) {
        return decode_did(w, gc);
    }
    /*
     * cfg 0x28 with buffer selector 3 is kon_loadGeomGC's form for an
     * 8-bit overlay window drawn into both overlay buffers: it repeats
     * the index and the mask in the high byte and replaces the plane
     * set with 0x28 (konal.so 0x53a1c8-0x53a2fc; selector 1 gives 0x2c
     * and 2 gives 0x30). konClearLayer sends it, through
     * kon_loadClearGC, to clear the overlay layer, index 0 under all
     * ones, before its boxes. A 15-bit overlay format (format bit
     * 0x2000) also gets 0x28, whatever its selector, with the value
     * p << 1 | 1 and the mask pm << 1 | 1 (0x53a058-0x53a10c); those
     * forms stay deferred. Limitation: a 15-bit GC sent with selector 3
     * whose value and mask happen to repeat one byte would be read as
     * this form. No caller is known to send one; konClearLayer passes
     * the 8-bit format for the overlay layer.
     */
    if ((plane == 0x28 && !(w[6] == 0x28 && op == 3)) ||
        (w[6] == 0x1008 && w[7] == 0x10001 && w[10] == 5)) {
        return SGI_X_GC_DEFERRED;
    }
    switch (w[6]) {
    case 0:
        /*
         * 30-bit TrueColor: cfg 0 from every builder, told apart by ifmt
         * 9, or 0 from the geometry builder (kon_parseGC sets ifmt 9 and
         * op residual 0x24000, konal.so 0x542194-0x542204).
         */
        if (!rgb30) {
            return SGI_X_GC_BAD_FIELDS;
        }
        gc->target = SGI_X_GC_MAIN_RGB8;
        gc->width_mask = 0xffffff;
        break;
    case 0xc:
    case 0x180c:
        gc->target = native_index ? SGI_X_GC_MAIN_INDEX16 : SGI_X_GC_MAIN_CI8;
        gc->width_mask = native_index ? 0xffff : 0xff;
        break;
    case 4:
    case 0x2804:
    case 0x3804:
        gc->target = SGI_X_GC_MAIN_RGB8;
        gc->width_mask = 0xffffff;
        break;
    case 0x2c:
    case 0x182c:
    case 0x102c:
        gc->target = SGI_X_GC_OVERLAY0;
        gc->width_mask = 0xffff;
        break;
    case 0x30:
    case 0x1030:
        return SGI_X_GC_DEFERRED;
    case 0x1830:
        gc->target = SGI_X_GC_OVERLAY1;
        gc->width_mask = 0xffff;
        break;
    case 0x28:
        gc->target = SGI_X_GC_OVERLAY_BOTH;
        gc->width_mask = 0xff;
        break;
    case 0x20:
    case 0x1820:
    case 0x1020:
        gc->target = SGI_X_GC_POPUP;
        gc->width_mask = 3;
        break;
    default:
        return SGI_X_GC_BAD_FIELDS;
    }
    if (op & 4) {
        return SGI_X_GC_DEFERRED;
    }
    if (!rgb30 && geometry != (w[6] < 0x1000)) {
        return SGI_X_GC_BAD_FIELDS;
    }
    if (gc->target == SGI_X_GC_MAIN_CI8 ||
        gc->target == SGI_X_GC_MAIN_INDEX16 ||
        gc->target == SGI_X_GC_MAIN_RGB8) {
        bool rgb = gc->target == SGI_X_GC_MAIN_RGB8;

        if (w[8] != 1 || w[11] != (rgb ? 0 : 1) ||
            w[12] != (rgb ? 0 : 1)) {
            return SGI_X_GC_BAD_FIELDS;
        }
        if (geometry) {
            if (op != 1 && !(rgb && w[6] == 4 && op == 3)) {
                return SGI_X_GC_BAD_FIELDS;
            }
        } else if (rgb30) {
            /* Selector 2 has no cfg form to show it in; deferred. */
            if (op != 0x64001) {
                return SGI_X_GC_DEFERRED;
            }
        } else if (w[10] != (rgb ? 8 : 5) ||
                   gc->first_selector < 1 || gc->first_selector > 2 ||
                   gc->second_selector < 1 ||
                   gc->second_selector > 2 ||
                   residual != (rgb ? 0x20000 : 0) ||
                   (rgb && ((w[6] == 0x3804) !=
                            (gc->second_selector == 2)))) {
            return SGI_X_GC_BAD_FIELDS;
        }
    } else if (w[8] || w[11] != 1 || w[12] != 1 ||
               (!geometry && w[10] != 5) ||
               op != (gc->target == SGI_X_GC_OVERLAY1 ? 2 :
                      gc->target == SGI_X_GC_OVERLAY_BOTH ? 3 : 1)) {
        return SGI_X_GC_BAD_FIELDS;
    }

    if (gc->target == SGI_X_GC_MAIN_RGB8) {
        unsigned red = mask_word & 0xfff;
        unsigned green = (mask_word >> 12) & 0xfff;
        unsigned blue = w[5] & 0xfff;

        if (!canonical_rgb(((uint64_t)w[0] << 32) | w[1], rgb30,
                           &gc->foreground) ||
            !canonical_rgb(((uint64_t)w[2] << 32) | w[3], rgb30,
                           &gc->background)) {
            return SGI_X_GC_BAD_COLOR;
        }
        if (mask_word & ~0xffffffU || w[5] & ~0xffffffU ||
            (w[5] & 0xfff000) != 0xfff000 ||
            !canonical_component(red, rgb30) ||
            !canonical_component(green, rgb30) ||
            !canonical_component(blue, rgb30)) {
            return SGI_X_GC_BAD_MASK;
        }
        gc->mask = ((red >> 4) << 16) | ((green >> 4) << 8) |
                   (blue >> 4);
    } else {
        if (w[0] & ~0xfff78U || w[2] & ~0xfff78U ||
            (w[1] != 0 && w[1] != 0x1ffe) ||
            (w[3] != 0 && w[3] != 0x1ffe)) {
            return SGI_X_GC_BAD_COLOR;
        }
        if (mask_word & ~0xf00fffU || w[5]) {
            return SGI_X_GC_BAD_MASK;
        }
        gc->foreground = index_value(w[0]);
        gc->background = index_value(w[2]);
        gc->mask = index_mask(mask_word);
        if (gc->target == SGI_X_GC_MAIN_CI8) {
            if ((gc->foreground | gc->background) & 0xff) {
                return SGI_X_GC_BAD_COLOR;
            }
            if ((gc->mask & 0xff) != 0xff) {
                return SGI_X_GC_BAD_MASK;
            }
            gc->foreground >>= 8;
            gc->background >>= 8;
            gc->mask >>= 8;
        } else if (gc->target == SGI_X_GC_OVERLAY_BOTH) {
            /* The two bytes are one index repeated; keep one of them. */
            if ((gc->foreground >> 8) != (gc->foreground & 0xff) ||
                (gc->background >> 8) != (gc->background & 0xff)) {
                return SGI_X_GC_BAD_COLOR;
            }
            if ((gc->mask >> 8) != (gc->mask & 0xff)) {
                return SGI_X_GC_BAD_MASK;
            }
            gc->foreground &= 0xff;
            gc->background &= 0xff;
            gc->mask &= 0xff;
        } else if (gc->target == SGI_X_GC_POPUP) {
            if (mask_word & ~0x300000U) {
                return SGI_X_GC_BAD_MASK;
            }
            gc->foreground &= 3;
            gc->background &= 3;
            gc->mask = (mask_word >> 20) & 3;
        }
    }
    return SGI_X_GC_OK;
}

SGIXGCStatus sgi_x_gc_decode(const uint32_t words[SGI_X_GC_WORDS],
                            SGIXGC *gc)
{
    return decode(words, gc, false);
}

SGIXGCStatus sgi_x_gc_decode_main_index(const uint32_t words[SGI_X_GC_WORDS],
                                       SGIXGC *gc)
{
    SGIXGCStatus status = decode(words, gc, true);

    return status == SGI_X_GC_OK && gc->target != SGI_X_GC_MAIN_INDEX16 ?
           SGI_X_GC_BAD_FIELDS : status;
}

bool sgi_x_gc_noop(const SGIXGC *gc)
{
    return !gc->mask || gc->gx == 5;
}

bool sgi_x_gc_needs_destination(const SGIXGC *gc)
{
    if (sgi_x_gc_noop(gc)) {
        return false;
    }
    return gc->mask != gc->width_mask ||
           (gc->gx != 0 && gc->gx != 3 && gc->gx != 12 && gc->gx != 15);
}

uint32_t sgi_x_gc_apply(const SGIXGC *gc, uint32_t source,
                        uint32_t destination)
{
    return ((sgi_x_logic_op(gc->gx, source, destination) & gc->mask) |
            (destination & ~gc->mask)) & gc->width_mask;
}
