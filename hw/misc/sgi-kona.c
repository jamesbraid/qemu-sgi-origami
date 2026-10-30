/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA: the InfiniteReality graphics pipe.
 *
 * This sgi-xio-board subclass models the widget at an Origin 2000 XIO slot.
 * It provides the widget-header and subsystem-classification registers,
 * one-wire identity records, an additional register window, and a textport
 * command port. The command port uses the shared QEMU framer and adds local
 * handling for the variable-length bitmap transfer. The managed area, the
 * framebuffer of the video combination (1280x1024 at reset), is shown
 * through the shared scanout, one console per display channel, which
 * converts only the display lines drawing marked dirty. Beyond the
 * textport, the board decodes the X server's command pages, keeps
 * per-context state, moves data by DMA (hw/misc/sgi-kona-dma.c) and runs
 * resident display lists.
 *
 * Values beyond the widget-header and classification fields are synthetic
 * or approximated. Identity serials and several board-record fields are
 * placeholders. Where a value rests on a captured boot trace, the comment
 * at that site says so; the rest of the command and register model has no
 * real-device capture behind it. Unimplemented offsets read zero and
 * accept writes.
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/1wire/ds2502.h"
#include "hw/display/sgi/gl-select.h"
#include "hw/display/sgi/rasterizer.h"
#include "hw/display/sgi/x-gc.h"
#include "hw/mips/sgi/pi-gfx.h"
#include "qemu/timer.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xio-link.h"
#include "hw/mips/sgi/xio-widget.h"
#include "hw/misc/sgi-kona.h"
#include "hw/mips/sgi/sgi-synthetic-serial.h"
#include "hw/misc/sgi-kona-gl-decode.h"
#include "hw/misc/sgi-kona-mesa-provider.h"
#include "hw/misc/sgi-kona-ports.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/block-backend.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "migration/qemu-file.h"
#include "qapi/qapi-visit-common.h"
#include "qapi/util.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "system/dma.h"
#include "system/runstate.h"
#include "system/qtest.h"
#include "qemu/main-loop.h"
#include "trace.h"

/* Widget-header identification: revision:part:manufacturer:1, generic. */
#define WIDGET_ID_REVISION_SHIFT     28
#define WIDGET_ID_PART_SHIFT         12
#define WIDGET_ID_MANUFACTURER_SHIFT 1
#define WIDGET_ID_ALWAYS_ONE         1

/*
 * The DS2502 family code used by this model. The physical identity-part type
 * is unresolved, so this is a model choice rather than a board claim.
 */
#define SGI_KONA_ONEWIRE_FAMILY 0x09

/*
 * Marked synthetic serials, graphics-board instances 0 and 1 for the pipe's
 * two boards; these are not hardware identifiers.
 */
#define SGI_KONA_GEOMETRY_SERIAL SGI_SYNTHETIC_ONEWIRE_SERIAL('G', 0)
#define SGI_KONA_KTOWN_SERIAL    SGI_SYNTHETIC_ONEWIRE_SERIAL('G', 1)

/*
 * The pipe slots a machine's pipes take serials for (identity-index), and
 * shared-identity-index's value for a pipe whose Ktown is its own.
 */
#define SGI_KONA_IDENTITY_SLOTS 4
#define SGI_KONA_OWN_IDENTITY   UINT8_MAX

/*
 * These records use the shared fixed-offset board-record representation.
 * Barcode, part, revision, and name are stored fields. Group, capability,
 * and variety bytes use the unprogrammed value. The displayed serial is the
 * one-wire ROM number, not a separate stored field. Configured part numbers
 * are retained; the other identity fields are synthetic placeholders, and
 * each barcode is the marked one for its part's serial.
 */
static const SGIBridgeBoardRecord sgi_kona_geometry_record = {
    .barcode = "EMUG00",
    .part = "030-1398-001",
    .revision = "01",
    .name = "GEOM-BD",
};
static const SGIBridgeBoardRecord sgi_kona_ktown_record = {
    .barcode = "EMUG01",
    .part = "030-1067-001",
    .revision = "01",
    .name = "KTOWN-BD",
};

/*
 * The XIO board base builds the identity chain at realize. The geometry
 * record is the board's own record and its part can be selected with
 * "board-part". One additional record is included by default; machine
 * configurations without that part can provide an empty list.
 */
static const SGIXioBoardIdentityPart sgi_kona_ktown_part = {
    .child_name = "onewire-ktown",
    .record = &sgi_kona_ktown_record,
    .serial = SGI_KONA_KTOWN_SERIAL,
    .fitted = true,
};

static const SGIXioBoardIdentity sgi_kona_identity = {
    .family = SGI_KONA_ONEWIRE_FAMILY,
    .own = {
        .child_name = "onewire-geometry",
        .record = &sgi_kona_geometry_record,
        .serial = SGI_KONA_GEOMETRY_SERIAL,
    },
    .others = &sgi_kona_ktown_part,
    .num_others = 1,
};

/*
 * A guest-controlled width*height could otherwise demand an arbitrarily
 * large payload allocation; clamp to the model's own fixed framebuffer
 * size, which is more than any real glyph/logo-scale blit needs. This
 * bound is this model's own safety choice, not a hardware fact.
 */
#define SGI_KONA_BITMAP_MAX_PAYLOAD_WORDS \
    DIV_ROUND_UP((uint64_t)SGI_KONA_FB_WIDTH * SGI_KONA_FB_HEIGHT, 32)

static const SGIPlane *sgi_kona_plane(SGIKonaState *s, uint32_t id)
{
    return sgi_board_memory_plane(&s->mem, id);
}

/*
 * An X pixel's colour: X colour-map entry SGI_KONA_X_COLOR_MAP_WINDOW +
 * index, read as 10:10:10 with red in bits 29:20 and each component
 * scaled to 8 bits. That the X server's windows use the map loaded at
 * 0x2d00 is inferred. The guest check is two greeter pixels resolving
 * through it: (340, 20) to 213, 213, 213 grey and (270, 310) to 153, 153,
 * 153 grey. The shell's 192 grey is overdrawn and cannot serve.
 */
static uint32_t sgi_kona_rgb10(uint32_t entry)
{
    uint32_t rgb = 0;

    for (int shift = 20; shift >= 0; shift -= 10) {
        uint32_t c = (entry >> shift) & 0x3ff;

        rgb = (rgb << 8) | ((c * 255 + 511) / 1023);
    }
    return rgb;
}

static uint32_t sgi_kona_x_color(SGIKonaState *s, uint32_t index)
{
    uint64_t addr = (uint64_t)SGI_KONA_X_COLOR_MAP_WINDOW + index;
    uint32_t entry;

    if (addr >= SGI_KONA_X_COLOR_MAP_ENTRIES) {
        return 0;
    }
    entry = s->x_color_map[addr];
    return sgi_kona_rgb10(entry);
}

/* Installed unsupported maps stay visible as black and log their raw entry. */
static uint32_t sgi_kona_desktop_color(SGIKonaState *s, unsigned id,
                                      unsigned index, unsigned mode,
                                      unsigned count)
{
    uint32_t raw = s->desktop_map[id];
    unsigned base = (raw >> 7) & 0x1ffff;

    if (((raw >> 3) & 15) != mode ||
        base > SGI_KONA_X_COLOR_MAP_ENTRIES - count) {
        if (!s->desktop_map_reported[id]) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: unsupported desktop map "
                          "%u raw=0x%08x\n", id, raw);
            s->desktop_map_reported[id] = true;
        }
        return 0;
    }
    return sgi_kona_rgb10(s->x_color_map[base + index]);
}

/*
 * The main-plane display ID at plane pixel (x, y): bits 7:2 of the
 * display-ID plane's word. The X server writes a main-plane window's ID
 * there under the mask 0xfc (the command grammar's display-ID GC), and
 * the GL windows' IDs the kernel sends in 0x1f3 word 9 fit those bits. It
 * writes bits 12:8 separately, under mask 0x1f00, for a window's overlay
 * (established from the display-ID GCs of an IRIS GL mandel window); those
 * bits do not choose the main pixel's colour map.
 */
#define SGI_KONA_DISPLAY_ID_MAX 0x3f

static unsigned sgi_kona_display_id_at(SGIKonaState *s, int x, int y)
{
    return (sgi_plane_load(&s->desktop_mem,
                           sgi_board_memory_plane(&s->desktop_mem, 0),
                           x, y) >> 2) & SGI_KONA_DISPLAY_ID_MAX;
}

static uint32_t sgi_kona_desktop_index_color(SGIKonaState *s, uint32_t word,
                                            int x, int y, bool native_index)
{
    unsigned id = sgi_kona_display_id_at(s, x, y);

    if (id >= ARRAY_SIZE(s->desktop_map)) {
        if (!s->desktop_id_reported) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: unsupported display ID "
                          "%u\n", id);
            s->desktop_id_reported = true;
        }
        return 0;
    }
    if (!s->desktop_map_valid[id]) {
        return sgi_kona_x_color(s, native_index ? word >> 8 : word);
    }
    if (native_index && ((s->desktop_map[id] >> 3) & 15) == 11) {
        return sgi_kona_desktop_color(s, id, (word >> 4) & 0xfff, 11, 4096);
    }
    return sgi_kona_desktop_color(s, id,
                                   native_index ? word >> 8 : word & 0xff,
                                   7, 256);
}

/* A colour bank's plane and the plane of its tags; bank 0 or 1. */
static uint32_t sgi_kona_bank_plane(SGIKonaState *s, unsigned bank)
{
    return bank ? s->plane_back : s->plane_front;
}

static uint32_t sgi_kona_bank_tag_plane(SGIKonaState *s, unsigned bank)
{
    return bank ? s->plane_back_tag : s->plane_front_tag;
}

/*
 * The plane rectangle a GL context's window covers, cut to the
 * framebuffer; false when it covers nothing there.
 */
static bool sgi_kona_gl_window_rect(const SGIKonaState *s,
                                    const SGIKonaGLWindow *w, SGIClipRect *r)
{
    int64_t x0 = w->x, y0 = w->y;
    int64_t x1 = x0 + w->width - 1, y1 = y0 + w->height - 1;

    if (!w->placed || !w->width || !w->height) {
        return false;
    }
    x0 = MAX(x0, 0);
    y0 = MAX(y0, 0);
    x1 = MIN(x1, s->fb_width - 1);
    y1 = MIN(y1, s->fb_height - 1);
    if (x0 > x1 || y0 > y1) {
        return false;
    }
    *r = (SGIClipRect) { x0, y0, x1, y1 };
    return true;
}

/*
 * Whether window w owns only the pixels showing its display ID. Word 9
 * all ones with word 10 set is the kernel's word for a window the X
 * server has not given an ID yet (observed: ivview's contexts, and mag's
 * first placement, before the X server mapped the window), which is also
 * what its null clip sends. No pixel holds that ID, so such a window
 * owns none: mag's first clear, sent while its unmapped window was still
 * at the screen's corner, otherwise drew a grey box over the Toolchest
 * that nothing repainted. Inferred from the same ownership test as an
 * ID in the main planes'; how the pipe treats word 9 all ones is unknown.
 */
static bool sgi_kona_gl_window_tests_id(const SGIKonaGLWindow *w)
{
    return w->partial && (w->display_id <= SGI_KONA_DISPLAY_ID_MAX ||
                          w->display_id == UINT32_MAX);
}

/*
 * Whether window w owns plane pixel (x, y) of its rectangle, for drawing.
 *
 * Inferred: the kernel gives the pipe no clip list for a GL window, only
 * its rectangle and, in 0x1f3, the display ID the X server paints into
 * the pixels the window owns and a word that is 1 while another window
 * covers part of it (observed in the runs). The GL library sends no
 * window clip either, yet OpenGL's pixel ownership test (1.2.1, 4.1.1)
 * keeps a context out of pixels another window owns. So the model keeps
 * a partly covered window to the pixels that show its ID. How the pipe
 * performs the test, and whether it tests a wholly visible window at
 * all, is unknown; the model tests only while word 10 is set. A window
 * whose word 9 is outside the main planes' IDs (an overlay ID, or all
 * ones before the X server has given it one) keeps its rectangle.
 */
static bool sgi_kona_gl_window_owns(SGIKonaState *s,
                                    const SGIKonaGLWindow *w, int x, int y)
{
    return !sgi_kona_gl_window_tests_id(w) ||
           sgi_kona_display_id_at(s, x, y) == w->display_id;
}

/*
 * Whether the display shows window w's colour bank at plane pixel (x, y)
 * of its rectangle: where the display-ID planes hold its ID, whether or
 * not another window covers part of it. The X server repaints a window's
 * pixels with the IDs of what lies beneath when it unmaps the window,
 * which retires the bank of a client that died without its context
 * being torn down (observed in the runs: a killed client sends no
 * 0x1f8). Inferred, as for sgi_kona_gl_window_owns(); a window whose
 * word 9 is all ones with word 10 set, the kernel's words for a window
 * with no display ID, shows no pixel, as it owns none; another word 9
 * outside the main planes' IDs shows across its rectangle.
 */
static bool sgi_kona_gl_window_shows(SGIKonaState *s,
                                     const SGIKonaGLWindow *w, int x, int y)
{
    if (sgi_kona_gl_window_tests_id(w)) {
        return sgi_kona_display_id_at(s, x, y) == w->display_id;
    }
    return w->display_id > SGI_KONA_DISPLAY_ID_MAX ||
           sgi_kona_display_id_at(s, x, y) == w->display_id;
}

typedef struct SGIKonaOwnedRun {
    int x0, x1, y0;
    bool seen;
} SGIKonaOwnedRun;

/*
 * The pixels of rectangle r that window w owns, as clip rectangles: a run
 * of owned pixels that repeats with the same columns on the next row
 * extends the same rectangle. False when they need more than
 * SGI_CLIP_MAX rectangles, the most a drawable holds.
 */
static bool sgi_kona_gl_owned_clip(SGIKonaState *s, const SGIKonaGLWindow *w,
                                   const SGIClipRect *r, SGIClipList *clip)
{
    g_autofree SGIKonaOwnedRun *open =
        g_new(SGIKonaOwnedRun, r->x1 - r->x0 + 3);
    unsigned nopen = 0;

    clip->n = 0;
    clip->empty = 0;
    for (int y = r->y0; y <= r->y1 + 1; y++) {
        unsigned kept = 0;

        for (unsigned k = 0; k < nopen; k++) {
            open[k].seen = false;
        }
        for (int x = r->x0; y <= r->y1 && x <= r->x1; x++) {
            int x0 = x;
            unsigned k = 0;

            if (!sgi_kona_gl_window_owns(s, w, x, y)) {
                continue;
            }
            while (x < r->x1 && sgi_kona_gl_window_owns(s, w, x + 1, y)) {
                x++;
            }
            while (k < nopen && (open[k].x0 != x0 || open[k].x1 != x)) {
                k++;
            }
            if (k == nopen) {
                open[nopen++] = (SGIKonaOwnedRun) { x0, x, y, false };
            }
            open[k].seen = true;
        }
        for (unsigned k = 0; k < nopen; k++) {
            if (open[k].seen) {
                open[kept++] = open[k];
                continue;
            }
            if (clip->n == SGI_CLIP_MAX) {
                return false;
            }
            clip->r[clip->n++] = (SGIClipRect) {
                open[k].x0, open[k].y0, open[k].x1, y - 1,
            };
        }
        nopen = kept;
        if (clip->n + nopen > SGI_CLIP_MAX) {
            return false;
        }
    }
    return true;
}

/*
 * Whether a context other than i placed a window with i's display ID
 * after i's last placement. The X server hands a dead client's display
 * ID to the next window it maps: a killed client sends no teardown, so
 * its context keeps its window (sgi_kona_gl_window_shows()), and hist,
 * mapped over part of ivview's old rectangle with ivview's ID 0xe,
 * showed ivview's last frame (observed). Inferred: that the display
 * table entry for an ID follows its latest window, the only reading under
 * which the reused ID shows the new window's drawing. Overlay and
 * unassigned IDs are not compared.
 */
static bool sgi_kona_display_id_reused(SGIKonaState *s, unsigned i)
{
    const SGIKonaGLWindow *w = &s->state[i]->window;

    if (w->display_id > SGI_KONA_DISPLAY_ID_MAX) {
        return false;
    }
    for (unsigned j = 0; j < SGI_KONA_CONTEXTS; j++) {
        const SGIKonaGLWindow *o;

        if (j == i || !s->state[j] || !s->contexts[j].valid) {
            continue;
        }
        o = &s->state[j]->window;
        if (o->placed && o->display_id == w->display_id &&
            o->place_serial > w->place_serial) {
            return true;
        }
    }
    return false;
}

/*
 * The window of context table entry i if it shows colour bank 1, the
 * bank a double-buffered context's window shows after an odd number of
 * swaps; false otherwise, and false once a later window took its display
 * ID (sgi_kona_display_id_reused()).
 */
static bool sgi_kona_bank1_window(SGIKonaState *s, unsigned i,
                                  SGIClipRect *r)
{
    const SGIKonaGLWindow *w;

    if (!s->state[i]) {
        return false;
    }
    w = &s->state[i]->window;
    return s->contexts[i].valid && w->double_buffered &&
           w->displayed_bank && sgi_kona_gl_window_rect(s, w, r) &&
           !sgi_kona_display_id_reused(s, i);
}

/*
 * The colour bank the display shows at plane (x, y): bank 1 at the pixels
 * of a double-buffered context's window showing it that hold the
 * window's display ID, bank 0 everywhere else.
 *
 * Approximated: which bank a pixel shows is decided by window rectangles
 * and display IDs (sgi_kona_gl_window_shows()), standing in for the
 * display table the swap rewrites, whose layout is unknown.
 */
static unsigned sgi_kona_displayed_bank(SGIKonaState *s, int x, int y)
{
    SGIClipRect r;

    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        if (sgi_kona_bank1_window(s, i, &r) &&
            x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1 &&
            sgi_kona_gl_window_shows(s, &s->state[i]->window, x, y)) {
            return 1;
        }
    }
    return 0;
}

/*
 * A colour bank's pixel at plane (x, y), resolved as the scanout shows
 * it, except that a direct pixel keeps the top byte of its stored word:
 * a textport pixel is its textport colour-map entry, an X pixel its X
 * colour-map entry, a DMA or GL pixel the word stored.
 *
 * The graphics low-level design's InfiniteReality column gives the
 * textport colour map 10-bit entries. The textport's load (command 3)
 * carries 10-bit components, and the model narrows them to 8 bits as it
 * stores them rather than here; nothing reads an entry but this
 * resolve, so the narrowed entry resolves to the same colour and the
 * colour map's migrated contents keep their meaning.
 *
 * Approximated: resolving at scanout rather than at draw is what a
 * colour-indexed pixel does, and that this pipe's textport pixels are
 * colour-indexed rather than written resolved is inferred from command
 * 2 taking an index. A readback of a textport pixel across a colour-map
 * reload on a real pipe would settle it; the scanout qtest's
 * colour-map-resolves-at-scanout case checks the model's rule.
 */
static uint32_t sgi_kona_bank_read(SGIKonaState *s, unsigned bank, int x,
                                   int y)
{
    uint32_t word = sgi_plane_load(&s->mem,
                                   sgi_kona_plane(s,
                                                  sgi_kona_bank_plane(s, bank)),
                                   x, y);
    uint32_t tag = sgi_plane_load(&s->mem,
                                  sgi_kona_plane(s,
                                                 sgi_kona_bank_tag_plane(s,
                                                                         bank)),
                                  x, y);

    switch (tag) {
    case SGI_KONA_TAG_COLOR_INDEX:
        return s->colormap[word & 0xff];
    case SGI_KONA_TAG_X_INDEX:
        return sgi_kona_desktop_index_color(s, word, x, y, false);
    case SGI_KONA_TAG_X_INDEX16:
        return sgi_kona_desktop_index_color(s, word, x, y, true);
    default:
        return word;
    }
}

/* The first overlay is visible; the second retains pixels without scanout. */
static uint32_t sgi_kona_desktop_compose(SGIKonaState *s, int x, int y,
                                        uint32_t main)
{
    unsigned popup = sgi_plane_load(&s->desktop_mem,
                       sgi_board_memory_plane(&s->desktop_mem, 3), x, y);
    unsigned overlay = sgi_plane_load(&s->desktop_mem,
                         sgi_board_memory_plane(&s->desktop_mem, 1), x, y);

    if (popup) {
        if (!s->desktop_map_valid[0x3f]) {
            if (!s->desktop_map_reported[0x3f]) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-kona: popup map not installed\n");
                s->desktop_map_reported[0x3f] = true;
            }
            return 0;
        }
        return sgi_kona_desktop_color(s, 0x3f, popup & 3, 1, 4);
    }
    if (overlay) {
        if (!s->desktop_map_valid[0x40]) {
            if (!s->desktop_map_reported[0x40]) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-kona: overlay map not installed\n");
                s->desktop_map_reported[0x40] = true;
            }
            return 0;
        }
        return sgi_kona_desktop_color(s, 0x40, overlay & 0xff, 7, 256);
    }
    return main;
}

/* The pixel the display shows at plane (x, y), from the bank it shows. */
uint32_t sgi_kona_fb_read(SGIKonaState *s, int x, int y)
{
    return sgi_kona_desktop_compose(s, x, y,
             sgi_kona_bank_read(s, sgi_kona_displayed_bank(s, x, y), x, y));
}

/* A direct-colour word into the front bank at plane (x, y). */
void sgi_kona_fb_store_direct(SGIKonaState *s, int x, int y, uint32_t word)
{
    const SGIPlane *front = sgi_kona_plane(s, s->plane_front);

    sgi_plane_store(&s->mem, front, x, y, word);
    sgi_plane_store(&s->mem, sgi_kona_plane(s, s->plane_front_tag), x, y,
                    SGI_KONA_TAG_DIRECT);
    sgi_kona_invalidate_plane(s, front, y, y);
}

/*
 * A resolved x8r8g8b8 pixel through the gamma table, as the display
 * shows it. The X server loads one 4,096-entry table per component, red
 * at 0, green at 0x1000 and blue at 0x2000, with 12-bit entries (the
 * command grammar's gamma load); the stock server's tables are a 1.7
 * power curve (entry 2048 is 2724, read back in a run), and SI applies
 * its own server's table the same way. The model's 8-bit component
 * indexes the table with its bits repeated to 12, and the entry's top
 * eight bits are shown.
 *
 * Inferred: that the table sits after the colour maps, on every pixel,
 * as on SI; a component with no nonzero entry, before any load, is
 * shown unchanged. What the pipe's gamma RAM holds before the X server
 * loads it, its index width, and whether it applies to the cursor (the
 * shared scanout composes the cursor afterwards, uncorrected) are
 * unknown.
 *
 * Each channel shows through the map the gamma-map command gave it: map 0 is
 * this table, the only one the model holds, since the board answers
 * GetBoardConfig's word 4 with 0 and the X server then keeps one gamma
 * map; SGI_KONA_GAMMA_MAP_LINEAR, which the X server sends for a map it
 * marks linear, shows the pixel unchanged (inferred from its log
 * message, "Linear Gamma being used for bind").
 */
static uint32_t sgi_kona_gamma_apply(SGIKonaState *s, unsigned channel,
                                     uint32_t rgb)
{
    uint32_t out = 0;

    if (s->channel[channel].gamma_map == SGI_KONA_GAMMA_MAP_LINEAR) {
        return rgb;
    }
    for (unsigned c = 0; c < SGI_KONA_GAMMA_COMPONENTS; c++) {
        unsigned shift = 16 - 8 * c;
        unsigned v = (rgb >> shift) & 0xff;

        if (s->gamma_loaded[c]) {
            v = s->gamma[c * SGI_KONA_GAMMA_COMPONENT_ENTRIES +
                         ((v << 4) | (v >> 4))] >> 4;
        }
        out |= (v & 0xff) << shift;
    }
    return out;
}

/*
 * Managed-area display line y (0 the top) as the display generator takes
 * it in: each pixel resolved by its tag from the bank shown there (see
 * sgi_kona_displayed_bank()), with the unused top byte of an x8r8g8b8 word
 * cleared. The gamma table and blanking are each channel's, after.
 */
static void sgi_kona_resolve_managed_line(SGIKonaState *s, unsigned y,
                                          uint32_t *dst)
{
    int row = s->fb_height - 1 - y;
    SGIClipRect r;

    for (unsigned x = 0; x < s->fb_width; x++) {
        dst[x] = sgi_kona_bank_read(s, 0, x, row) & 0x00ffffff;
    }
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        if (!sgi_kona_bank1_window(s, i, &r) || row < r.y0 ||
            row > r.y1) {
            continue;
        }
        for (int x = r.x0; x <= r.x1; x++) {
            if (sgi_kona_gl_window_shows(s, &s->state[i]->window, x, row)) {
                dst[x] = sgi_kona_bank_read(s, 1, x, row) & 0x00ffffff;
            }
        }
    }
    for (unsigned x = 0; x < s->fb_width; x++) {
        dst[x] = sgi_kona_desktop_compose(s, x, row, dst[x]) & 0x00ffffff;
    }
}

/*
 * Display channels.
 *
 * Every channel of a pipe shows part of one framebuffer, the managed
 * area, which the X server manages as one screen; ircombine places each
 * channel's rectangle in it, and channels may overlap (public: SGI's
 * answers on comp.sys.sgi.admin, 27 June 1997, and comp.sys.sgi.graphics,
 * 13 June 2003). A channel's input rectangle is what GetResize reports
 * for it; its output is its video format's active size. The model shows
 * channel c on its own console, head c of the device: channel 0 on
 * s->scanout, which also keeps the retrace and the swap queue, the others
 * on s->channel_scanout[], with no retrace of their own.
 *
 * Approximated: an input rectangle smaller or larger than the format is
 * sampled to it, nearest pixel, and a part outside the managed area shows
 * black. How the display generator filters a resized channel is not
 * known; nearest sampling keeps unscaled channels exact.
 */
typedef struct SGIKonaChannelView {
    int64_t x, top;             /* managed-area column and display line */
    uint32_t width, height;     /* the input rectangle */
    uint32_t out_width, out_height; /* the format's active size */
} SGIKonaChannelView;

static unsigned sgi_kona_channel_count(const SGIKonaState *s)
{
    return s->config.channels ? s->config.channels : 1;
}

static SGIScanout *sgi_kona_channel_scanout(SGIKonaState *s, unsigned c)
{
    return c ? &s->channel_scanout[c - 1] : &s->scanout;
}

/* Whether channel c shows anything, and where it takes it from. */
static bool sgi_kona_channel_view(const SGIKonaState *s, unsigned c,
                                  SGIKonaChannelView *v)
{
    const SGIKonaChannel *ch = &s->channel[c];

    if (c >= sgi_kona_channel_count(s) || !ch->enabled ||
        !ch->format_width || !ch->format_height ||
        !ch->input_width || !ch->input_height) {
        return false;
    }
    *v = (SGIKonaChannelView) {
        .x = ch->input_x,
        .top = (int64_t)s->fb_height - 1 - ch->input_y,
        .width = ch->input_width,
        .height = ch->input_height,
        .out_width = ch->format_width,
        .out_height = ch->format_height,
    };
    return true;
}

/*
 * Managed-area display lines first..last changed: repaint the lines of
 * every channel whose input rectangle takes from them.
 */
static void sgi_kona_invalidate_lines(SGIKonaState *s, int64_t first,
                                      int64_t last)
{
    for (unsigned c = 0; c < sgi_kona_channel_count(s); c++) {
        SGIKonaChannelView v;
        int64_t a, b, l0, l1;

        if (!sgi_kona_channel_view(s, c, &v)) {
            continue;
        }
        a = MAX(first, v.top) - v.top;
        b = MIN(last, v.top + v.height - 1) - v.top;
        if (a > b) {
            continue;
        }
        /* The output lines l whose source line l * height / out_height. */
        l0 = DIV_ROUND_UP(a * v.out_height, v.height);
        l1 = DIV_ROUND_UP((b + 1) * v.out_height, v.height) - 1;
        sgi_scanout_invalidate(sgi_kona_channel_scanout(s, c), l0,
                               l1 - l0 + 1);
    }
}

void sgi_kona_invalidate_plane(SGIKonaState *s, const SGIPlane *p,
                               int y0, int y1)
{
    if (p->bottom_origin) {
        sgi_kona_invalidate_lines(s, (int64_t)p->height - 1 - MAX(y0, y1),
                                  (int64_t)p->height - 1 - MIN(y0, y1));
    } else {
        sgi_kona_invalidate_lines(s, MIN(y0, y1), MAX(y0, y1));
    }
}

/*
 * Plane line y of p changed under a store. Outside a software draw the
 * channels are invalidated at once. Inside one (between
 * sgi_kona_invalidate_defer() and sgi_kona_invalidate_flush()) every
 * store of a row would resolve every channel's view again, so the draw
 * only widens the range of display lines it touched, once per row per
 * band thread, and the flush invalidates that range when the draw
 * returns. A display refresh during the draw may show the rows it has
 * stored so far one refresh late; the lines are marked before the
 * command completes, so what the display shows afterwards is the same.
 */
static void sgi_kona_invalidate_store(SGIKonaState *s, const SGIPlane *p,
                                      int y)
{
    static __thread const SGIKonaState *memo_state;
    static __thread uint64_t memo_op;
    static __thread int64_t memo_line;
    int64_t line = p->bottom_origin ? (int64_t)p->height - 1 - y : y;
    int64_t seen;

    if (!qatomic_read(&s->invalidate_depth)) {
        sgi_kona_invalidate_lines(s, line, line);
        return;
    }
    if (memo_state == s && memo_op == qatomic_read(&s->invalidate_op) &&
        memo_line == line) {
        return;
    }
    memo_state = s;
    memo_op = qatomic_read(&s->invalidate_op);
    memo_line = line;
    seen = qatomic_read(&s->invalidate_first);
    while (line < seen) {
        int64_t was = qatomic_cmpxchg(&s->invalidate_first, seen, line);

        if (was == seen) {
            break;
        }
        seen = was;
    }
    seen = qatomic_read(&s->invalidate_last);
    while (line > seen) {
        int64_t was = qatomic_cmpxchg(&s->invalidate_last, seen, line);

        if (was == seen) {
            break;
        }
        seen = was;
    }
}

/* Collect a software draw's stored lines; see sgi_kona_invalidate_store(). */
static void sgi_kona_invalidate_defer(SGIKonaState *s)
{
    if (s->invalidate_depth++) {
        return;
    }
    s->invalidate_first = INT64_MAX;
    s->invalidate_last = INT64_MIN;
    qatomic_set(&s->invalidate_op, s->invalidate_op + 1);
}

/* The draw has returned and its band threads are done: mark its lines. */
static void sgi_kona_invalidate_flush(SGIKonaState *s)
{
    if (--s->invalidate_depth) {
        return;
    }
    if (s->invalidate_first <= s->invalidate_last) {
        sgi_kona_invalidate_lines(s, s->invalidate_first,
                                  s->invalidate_last);
    }
}

/* Repaint every channel. */
static void sgi_kona_invalidate_all(SGIKonaState *s)
{
    for (unsigned c = 0; c < sgi_kona_channel_count(s); c++) {
        sgi_scanout_invalidate_all(sgi_kona_channel_scanout(s, c));
    }
}

/*
 * Output line y of a channel for its scanout: the input rectangle's line
 * sampled to the format's width, then through the channel's gamma table;
 * every pixel black while the display is blanked (see
 * sgi_kona_video_temp_blank()) or outside the managed area. The shared
 * scanout composes the cursor after resolving the row.
 */
static void sgi_kona_channel_resolve_row(void *opaque, unsigned y,
                                         uint32_t *dst)
{
    SGIKonaChannelHook *h = opaque;
    SGIKonaState *s = h->kona;
    SGIKonaChannelView v;
    int64_t line;

    if (!sgi_kona_channel_view(s, h->index, &v)) {
        memset(dst, 0, sgi_kona_channel_scanout(s, h->index)->mode.width *
                       sizeof(*dst));
        return;
    }
    line = v.top + (int64_t)y * v.height / v.out_height;
    if (s->video_blanked || s->channel[h->index].blanked ||
        line < 0 || line >= s->fb_height) {
        memset(dst, 0, v.out_width * sizeof(*dst));
        return;
    }
    if (v.x == 0 && v.width == s->fb_width &&
        v.out_width == s->fb_width) {
        sgi_kona_resolve_managed_line(s, line, dst);
    } else {
        sgi_kona_resolve_managed_line(s, line, s->resolve_line);
        for (uint32_t x = 0; x < v.out_width; x++) {
            int64_t sx = v.x + (int64_t)x * v.width / v.out_width;

            dst[x] = sx >= 0 && sx < s->fb_width ?
                     s->resolve_line[sx] : 0;
        }
    }
    for (uint32_t x = 0; x < v.out_width; x++) {
        dst[x] = sgi_kona_gamma_apply(s, h->index, dst[x]);
    }
}

/*
 * The channels as the display generator starts: channel 0 shows the whole
 * managed area at 1280x1024, the format the PROM falls back to (see the
 * display datasheet), and every other channel is off.
 *
 * Approximated: a real display generator starts the combination stored
 * in its flash, which ircombine and setmon -x write, at power-up; the
 * model runs a stored one only when graphics is initialized (see
 * sgi_kona_flash_run_combination()), so a reset starts the
 * single-channel combination every captured pipe can show. Nothing is
 * known of what a reset does to a running combination.
 */
static void sgi_kona_channels_default(SGIKonaChannel *channel)
{
    memset(channel, 0, sizeof(*channel) * SGI_KONA_MAX_CHANNELS);
    for (unsigned c = 0; c < SGI_KONA_MAX_CHANNELS; c++) {
        channel[c] = (SGIKonaChannel) {
            .source = c,
            .pan = { 0, 0, SGI_KONA_FB_WIDTH, SGI_KONA_FB_HEIGHT },
            .placement = { 1, 1, 1, 1, SGI_KONA_FB_WIDTH,
                           SGI_KONA_FB_HEIGHT },
        };
    }
    channel[0].enabled = true;
    channel[0].loaded = true;
    channel[0].format_width = SGI_KONA_FB_WIDTH;
    channel[0].format_height = SGI_KONA_FB_HEIGHT;
    channel[0].input_x = 0;
    channel[0].input_y = SGI_KONA_FB_HEIGHT - 1;
    channel[0].input_width = SGI_KONA_FB_WIDTH;
    channel[0].input_height = SGI_KONA_FB_HEIGHT;
}

static void sgi_kona_channels_reset(SGIKonaState *s)
{
    sgi_kona_channels_default(s->channel);
}

/* Channel 0's rows; its scanout's hooks are handed the pipe itself. */
static void sgi_kona_resolve_row(void *opaque, unsigned y, uint32_t *dst)
{
    SGIKonaState *s = opaque;

    sgi_kona_channel_resolve_row(&s->channel_hook[0], y, dst);
}

static void sgi_kona_retrace(void *opaque);

/*
 * publish() takes the command ports' completions (sgi_kona_port_consumed()),
 * the retrace requests' swap records and, through the DMA engine's own
 * hooks, its transfers' and fetches'. retrace() raises the video bit at a
 * retrace that honoured a request.
 */
static const SGIHeadOps sgi_kona_head_ops = {
    .publish = sgi_kona_publish,
    .resolve_row = sgi_kona_resolve_row,
    .retrace = sgi_kona_retrace,
};

/* The other channels only present; they queue no swaps. */
static const SGIHeadOps sgi_kona_channel_head_ops = {
    .resolve_row = sgi_kona_channel_resolve_row,
};

/*
 * A channel's mode: its format's active size when it shows anything,
 * none otherwise. Channel 0's carries the retrace at SGI_KONA_FIELD_HZ
 * whatever it shows (see that macro for why the rate is the model's
 * choice); a 60 Hz clock counting one per frame expresses it exactly.
 * Inferred: the retrace interrupt the driver takes is the encoder
 * channel's, and the model makes channel 0 the encoder.
 */
static void sgi_kona_channel_apply(SGIKonaState *s, unsigned c)
{
    SGIScanout *so = sgi_kona_channel_scanout(s, c);
    SGIKonaChannelView v;
    SGIScanoutMode mode = {
        .frame_clocks = c ? 0 : 1,
        .clock_hz = c ? 0 : SGI_KONA_FIELD_HZ,
    };

    if (sgi_kona_channel_view(s, c, &v)) {
        mode.width = v.out_width;
        mode.height = v.out_height;
    }
    if (so->mode.width != mode.width || so->mode.height != mode.height ||
        so->mode.clock_hz != mode.clock_hz) {
        sgi_scanout_set_mode(so, &mode);
    } else {
        sgi_scanout_invalidate_all(so);
    }
}

static void sgi_kona_channels_apply(SGIKonaState *s)
{
    for (unsigned c = 0; c < sgi_kona_channel_count(s); c++) {
        sgi_kona_channel_apply(s, c);
    }
}

/*
 * The managed area.
 *
 * A combination's SetupFrameBuffer [width, height, pixel depth] gives the
 * size of the one framebuffer every channel takes its rectangle from, the
 * size the X server manages as its screen. The X server takes that size
 * from configuration block +0x84 and +0x88 (established, the command
 * grammar datasheet's configuration request), and a combination stored in
 * the display generator's flash sets it through its own SetupFrameBuffer
 * (observed: ircombine's stored 2@1280x1024_60 carries [2560, 1024, 0]).
 * That the pipe answers the size its last SetupFrameBuffer set is
 * inferred: no capture of a pipe running another size has its block.
 *
 * The planes are sized at realize for the most pixels the configured
 * raster managers hold (sgi_kona_fb_capacity()), and laid out again with
 * abutting rows whenever the size changes. What a size change leaves in
 * the framebuffer is not known; the model clears it.
 */
static uint64_t sgi_kona_fb_capacity(const SGIKonaState *s)
{
    uint64_t rms = MAX(s->config.rm_count, 1);

    return rms * SGI_KONA_RM_PIXEL_BYTES / 8;
}

/*
 * The pipe firmware's budget for a managed area: raster managers x
 * 20,971,520 / (height x width rounded up to 160). Small pixels need 8,
 * Medium 16, Large 32, Extra-Large 64 (the command grammar datasheet's
 * "SetupFrameBuffer on the ARM"; board sets 2 and 6 count differently, and
 * the model presents neither).
 */
static uint64_t sgi_kona_fb_budget(const SGIKonaState *s, uint32_t width,
                                   uint32_t height)
{
    uint64_t area = (uint64_t)QEMU_ALIGN_UP(width, SGI_KONA_FB_TILE_WIDTH) *
                    height;

    return area ? (uint64_t)MAX(s->config.rm_count, 1) *
                  SGI_KONA_RM_PIXEL_BYTES / area : 0;
}

static bool sgi_kona_managed_fits(const SGIKonaState *s, uint32_t width,
                                  uint32_t height)
{
    return width && height && width <= SGI_KONA_FB_MAX_DIMENSION &&
           height <= SGI_KONA_FB_MAX_DIMENSION &&
           (uint64_t)QEMU_ALIGN_UP(width, SGI_KONA_FB_TILE_WIDTH) * height <=
           s->fb_capacity;
}

/* Lay every plane the managed area covers out at its current size. */
static void sgi_kona_managed_layout(SGIKonaState *s)
{
    const uint32_t pixel_planes[] = {
        s->plane_front, s->plane_back, s->plane_front_tag,
        s->plane_back_tag, s->plane_depth, s->plane_stencil,
    };
    bool fits = true;

    for (unsigned i = 0; i < ARRAY_SIZE(pixel_planes); i++) {
        fits &= sgi_board_memory_resize_plane(&s->mem, pixel_planes[i],
                                              s->fb_width, s->fb_height);
    }
    for (unsigned i = 0; i < 4; i++) {
        fits &= sgi_board_memory_resize_plane(&s->desktop_mem, i,
                                              s->fb_width, s->fb_height);
    }
    for (unsigned i = 0; i < 3; i++) {
        fits &= sgi_board_memory_resize_plane(&s->x_write_mem, i,
                                              s->fb_width, s->fb_height);
    }
    /* Every size reaching here passed sgi_kona_managed_fits(). */
    assert(fits);
    /* Made again, at the new size, on the next accumulation operation. */
    sgi_board_memory_cleanup(&s->accum_mem);
    sgi_kona_mesa_resize_board_views(s->mesa_provider);
}

static void sgi_kona_x_write_clear(SGIKonaState *s);

/*
 * SetupFrameBuffer's size, applied: the framebuffer is cleared and every
 * channel shown again from the new area. A size the raster managers
 * cannot hold is logged and changes nothing; what the pipe does then is
 * not known, and the X server's own check refuses such a combination
 * before it is sent.
 */
static void sgi_kona_managed_resize(SGIKonaState *s, uint32_t width,
                                    uint32_t height)
{
    if (width == s->fb_width && height == s->fb_height) {
        return;
    }
    if (!width || !height || width > SGI_KONA_FB_MAX_DIMENSION ||
        height > SGI_KONA_FB_MAX_DIMENSION) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: managed area %ux%u is past the "
                      "model's %u-pixel bound on a side; kept at %dx%d\n",
                      width, height, SGI_KONA_FB_MAX_DIMENSION, s->fb_width,
                      s->fb_height);
        return;
    }
    if (!sgi_kona_managed_fits(s, width, height)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: managed area %ux%u does "
                      "not fit the %" PRIu64 " pixels the raster managers "
                      "hold; kept at %dx%d\n", width, height,
                      s->fb_capacity, s->fb_width, s->fb_height);
        return;
    }
    trace_sgi_kona_managed_area(s->fb_width, s->fb_height, width, height);
    /*
     * An X pixel write still open would stage past the new area, so it is
     * dropped, as a reset drops it. The X server is not known to resize
     * while one is open.
     */
    if (s->x_pixel_open) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: managed area resized "
                      "with an X pixel write open; the write is dropped\n");
    }
    sgi_kona_x_write_clear(s);
    memset(s->x_pixel_setup, 0, sizeof(s->x_pixel_setup));
    s->x_pixel_open = false;
    s->x_pixel_x = 0;
    s->x_pixel_y = 0;
    s->x_pixel_bpp = 0;
    s->x_pixel_stride = 0;
    s->x_pixel_words = 0;
    s->x_pixel_remaining = 0;
    s->x_pixel_owner = SGI_KONA_CONTEXT_NONE;
    s->x_pixel_owner_generation = 0;
    s->x_pixel_prev = 0;
    s->fb_width = width;
    s->fb_height = height;
    sgi_kona_managed_layout(s);
    /* The pixel planes, not the display lists that share their memory. */
    for (uint32_t id = 0; id < s->mem.nplanes; id++) {
        const SGIPlane *p = sgi_kona_plane(s, id);

        if (id != s->plane_list) {
            memset(s->mem.bytes + p->base, 0, p->len);
        }
    }
    memset(s->desktop_mem.bytes, 0, s->desktop_mem.bytes_len);
    memset(s->x_write_mem.bytes, 0, s->x_write_mem.bytes_len);
    sgi_kona_channels_apply(s);
}

/*
 * Every drawing primitive shares this one colormap resolve step: command 6
 * (point), sgi_kona_fill_rect() below (command 5) and sgi_kona_bitmap_blit()'s
 * own set pixels (command 0xa) all funnel through here.
 *
 * The wire's y axis is bottom-up (y increasing toward the top of the
 * screen), not the top-down row order qemu_create_displaysurface_from()'s
 * framebuffer uses: this model's earlier top-down write (row = y) rendered
 * a live stock-PROM InfiniteReality splash upside down and mirrored to the
 * wrong screen half. Tracing a `baseio-preset=io6g` boot's own textport
 * packets (trace_sgi_command_port_packet) found the entire splash --
 * wordmark, icon placeholders and gradient -- built from 4312 command-5
 * fills and zero command-0xa bitmap blits, ruling out the bitmap payload
 * path as the source of the earlier illegibility. Flipping only the row
 * written here, with sgi_kona_test_get_pixel() flipped the same way so
 * qtest assertions keep reading back the coordinate they drew at, turned
 * that same boot's screendump into a correctly oriented, fully legible
 * "InfiniteReality" wordmark. Confirmed against this one discriminating
 * boot capture, not an independent register or one-wire capture of real
 * hardware.
 */
static void sgi_kona_draw_point(SGIKonaState *s, int32_t x, int32_t y)
{
    const SGIPlane *front = sgi_kona_plane(s, s->plane_front);

    if (!sgi_plane_contains(front, x, y)) {
        return;
    }
    /*
     * The bottom-up wire row is the plane row: the front plane is
     * bottom-origin, so the scanout maps it to display line
     * managed height - 1 - y. The pixel keeps its colour-map index,
     * resolved at scanout (see sgi_kona_fb_read()).
     */
    sgi_plane_store(&s->mem, front, x, y, s->draw_color & 0xff);
    sgi_plane_store(&s->mem, sgi_kona_plane(s, s->plane_front_tag), x, y,
                    SGI_KONA_TAG_COLOR_INDEX);
    sgi_kona_invalidate_plane(s, front, y, y);
}

/*
 * Filled rectangle (command 5): corners x0, y0, x1, y1 with x0 <= x1 and
 * y0 <= y1, lighting columns x0 + 1 to x1 and rows y0 + 1 to y1. The
 * raster board leaves out the left column and the bottom row of a
 * whole-pixel box, and the IO6 PROM's sender compensates by pushing
 * `max(a1-1,0), max(a2-1,0), a3, a4` (established: the KONA raster
 * datasheet's fill rule, from the IRIX 6.5.7 standalone textport driver
 * and the shipped textport firmware, which passes the corners to the
 * raster board unchanged). A captured boot trace agrees: its fills keep x0
 * and x1 at 0 and 1279 while each call's y0 is the previous call's y1, so
 * adjacent fills meet with neither a gap nor an overlap. A box at the
 * screen's left or bottom edge leaves column or row 0 unlit, as the
 * clamped sender makes the hardware do.
 */
static void sgi_kona_fill_rect(SGIKonaState *s, int32_t x0, int32_t y0,
                               int32_t x1, int32_t y1)
{
    int64_t x, y;

    for (y = (int64_t)y0 + 1; y <= y1; y++) {
        for (x = (int64_t)x0 + 1; x <= x1; x++) {
            sgi_kona_draw_point(s, x, y);
        }
    }
}

/*
 * A textport bitmap as the framer leaves one, live or saved: idle and
 * empty, collecting its six header words, or collecting the payload the
 * header sized, within the model's bound and with its buffer present.
 */
static bool sgi_kona_bitmap_fields_valid(uint32_t state, const uint32_t *header,
                                         uint32_t header_have, uint32_t width,
                                         uint32_t height,
                                         const uint32_t *payload,
                                         uint32_t have, uint32_t expected)
{
    uint64_t words = DIV_ROUND_UP((uint64_t)header[0] * header[1], 32);

    switch (state) {
    case SGI_KONA_BITMAP_IDLE:
        return !header_have && !expected && !have;
    case SGI_KONA_BITMAP_HEADER:
        return header_have < SGI_KONA_BITMAP_HEADER_WORDS && !expected &&
               !have;
    case SGI_KONA_BITMAP_PAYLOAD:
        return header_have == SGI_KONA_BITMAP_HEADER_WORDS &&
               width == header[0] && height == header[1] &&
               words && words <= SGI_KONA_BITMAP_MAX_PAYLOAD_WORDS &&
               expected == words && have < expected && payload;
    default:
        return false;
    }
}

/* A payload count is checked before the load allocates that many words. */
static bool sgi_kona_bitmap_count_valid(void *opaque, int version_id)
{
    return ((SGIKonaState *)opaque)->bitmap_payload_expected <=
           SGI_KONA_BITMAP_MAX_PAYLOAD_WORDS;
}

static bool sgi_kona_saved_bitmap_count_valid(void *opaque, int version_id)
{
    return ((SGIKonaSavedInput *)opaque)->bitmap_payload_expected <=
           SGI_KONA_BITMAP_MAX_PAYLOAD_WORDS;
}

static void sgi_kona_bitmap_reset(SGIKonaState *s)
{
    s->bitmap_state = SGI_KONA_BITMAP_IDLE;
    memset(s->bitmap_header, 0, sizeof(s->bitmap_header));
    s->bitmap_header_have = 0;
    s->bitmap_width = 0;
    s->bitmap_height = 0;
    g_free(s->bitmap_payload);
    s->bitmap_payload = NULL;
    s->bitmap_payload_have = 0;
    s->bitmap_payload_expected = 0;
}

/* Empty a saved slot, freeing the buffers it owns. */
static void sgi_kona_saved_input_clear(SGIKonaSavedInput *v)
{
    g_free(v->bitmap_payload);
    g_free(v->texture_upload.pixels);
    g_free(v->gl_image_upload.words);
    memset(v, 0, sizeof(*v));
}

/*
 * Unpack the completed payload into a rectangle anchored on the
 * textport's own raster position, one bit per pixel, MSB-first,
 * row-major. The header's field order -- width, height, x origin, y
 * origin, x advance, y advance -- is established from the IO6 PROM's
 * textport bitmap routine, which sends six header words from consecutive
 * halfwords of the caller's descriptor in that order. The PROM also
 * places every bitmap relative to the raster position (command 4): every
 * bitmap it sends is preceded by one (icons) or starts a run of glyphs
 * that advance from it (labels). The PROM's glyph drawing routine shows
 * the origin fields are a glyph's signed x/y offsets negated into the
 * descriptor, i.e. subtracted back out here.
 *
 * Placement itself -- pixel (c, r) of the payload landing at (raster_x -
 * xorigin + c, raster_y - yorigin + r), with the payload's row 0 at the
 * bottom of the placed image, then the raster position advancing by
 * (xadvance, yadvance) for the next blit -- is this model's own
 * approximation, inferred from the legibility of a replayed maintenance-menu
 * render and matched against OpenGL glBitmap's own raster-position-plus-
 * origin-plus-advance convention, not independently confirmed against real
 * hardware. This replaces the previous absolute-coordinate placement (header
 * words 2/3 read directly as x, y), which piled every one of a boot's 308
 * bitmap blits into the framebuffer's bottom-left corner because the model
 * never tracked a raster position at all.
 *
 * Selector 4 (raster position) takes a full 32-bit guest argument with no
 * known range: only its two-argument shape is known, nothing about a valid
 * range or a hardware clamp -- unlike the six header fields above, which the
 * PROM's descriptor pins to 16-bit range. Placing and advancing a
 * near-INT32_MAX raster position against those header fields in plain int32
 * arithmetic is signed-integer-overflow undefined behaviour in C,
 * independent of whatever sgi_kona_draw_point()'s own framebuffer bounds
 * check does with the result. Every placement and advance below is therefore
 * computed in int64, where the true sum or difference always fits; a
 * per-pixel result outside int32 range (and so nowhere near the visible
 * 1280x1024 surface regardless) is dropped before narrowing rather than
 * passed to sgi_kona_draw_point(). Ordinary on-screen and near-edge
 * placements narrow to the exact int32 value the previous int32 arithmetic
 * would have produced without overflowing, so this changes nothing about the
 * placement rule itself, established or approximated -- only the overflow
 * case, which no evidence read for this device shows real hardware clamping,
 * is newly and explicitly rejected instead of left to undefined behaviour.
 */
static void sgi_kona_bitmap_blit(SGIKonaState *s)
{
    int32_t xorigin = (int16_t)s->bitmap_header[2];
    int32_t yorigin = (int16_t)s->bitmap_header[3];
    int32_t xadvance = (int16_t)s->bitmap_header[4];
    int32_t yadvance = (int16_t)s->bitmap_header[5];
    int64_t x0 = (int64_t)s->raster_x - xorigin;
    int64_t y0 = (int64_t)s->raster_y - yorigin;
    int64_t next_x = (int64_t)s->raster_x + xadvance;
    int64_t next_y = (int64_t)s->raster_y + yadvance;
    uint64_t total = (uint64_t)s->bitmap_width * s->bitmap_height;
    uint64_t bit;
    bool overflowed = false;

    for (bit = 0; bit < total; bit++) {
        uint32_t word = s->bitmap_payload[bit / 32];
        bool set = (word >> (31 - (bit % 32))) & 1;
        int64_t x = x0 + (int64_t)(bit % s->bitmap_width);
        int64_t y = y0 + (int64_t)(bit / s->bitmap_width);

        if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) {
            overflowed = true;
            continue;
        }
        if (set) {
            sgi_kona_draw_point(s, (int32_t)x, (int32_t)y);
        }
    }

    if (overflowed) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: raster position (%" PRId32 ", %" PRId32
                      ") overflows signed 32-bit bitmap placement"
                      " arithmetic; out-of-range pixels dropped\n",
                      s->raster_x, s->raster_y);
    }

    trace_sgi_kona_bitmap_blit(s->bitmap_width, s->bitmap_height,
                               xorigin, yorigin, xadvance, yadvance);
    s->raster_x = (int32_t)(uint32_t)next_x;
    s->raster_y = (int32_t)(uint32_t)next_y;
}

/*
 * The kernel driver's attach routine's own entry-count formula: N == 0
 * selects 8, otherwise N, then shifted left 15 (established). At this
 * model's default probe-value (0), N is 0, so the count is
 * SGI_KONA_MAPRAM_ENTRIES, the maximum the fixed-size backing array below
 * holds; a nonzero probe-value can only shrink it, as SGI_KONA_PROBE's
 * header comment says.
 */
static uint32_t sgi_kona_mapram_entry_count(SGIKonaState *s)
{
    uint32_t n = (s->probe_value & SGI_KONA_PROBE_MAPRAM_COUNT_MASK) >>
                 SGI_KONA_PROBE_MAPRAM_COUNT_SHIFT;

    return (n == 0 ? 8 : n) << 15;
}

static void sgi_kona_mailbox_enqueue(SGIKonaState *s, const uint32_t *words,
                                     unsigned count)
{
    unsigned i;

    assert(count <= SGI_KONA_MAILBOX_MAX_WORDS);
    for (i = 0; i < count; i++) {
        s->mailbox_queue[i] = words[i];
    }
    s->mailbox_len = count;
    s->mailbox_pos = 0;
    s->mailbox_completions = 0;
    if (count > 0) {
        s->int_cause |= SGI_KONA_INT_CAUSE_MAILBOX;
    }
}

/*
 * One unsolicited mailbox word, after any reply words still unread, the way
 * the driver's interrupt routine takes them one at a time. A queue with no
 * room left is logged and the word is lost.
 */
static void sgi_kona_mailbox_post(SGIKonaState *s, uint32_t word)
{
    uint32_t unread = s->mailbox_len - s->mailbox_pos;

    if (unread >= SGI_KONA_MAILBOX_MAX_WORDS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: mailbox full; completion word 0x%" PRIx32
                      " lost\n", word);
        return;
    }
    memmove(s->mailbox_queue, s->mailbox_queue + s->mailbox_pos,
            unread * sizeof(uint32_t));
    s->mailbox_completions >>= s->mailbox_pos;
    s->mailbox_completions |= 1u << unread;
    s->mailbox_queue[unread] = word;
    s->mailbox_len = unread + 1;
    s->mailbox_pos = 0;
    s->int_cause |= SGI_KONA_INT_CAUSE_MAILBOX;
}

static bool sgi_kona_raise_vector(SGIKonaState *s, bool general);

static void sgi_kona_gl_swap(SGIKonaState *s, unsigned i);

/*
 * A retrace request's swap record came back from the scanout. SWAPPED:
 * the retrace that took it honoured its tag, so the tag register reads
 * it and the scanout's retrace() hook raises the video bit, and a
 * double-buffered context the request arrived under exchanges its banks
 * (see sgi_kona_gl_swap()). FAILED: the
 * scanout's queue of SGI_SWAP_MAX records was full. The driver bounds its
 * own requests, and how the pipe answers one past its queue is unknown,
 * so the model logs it and completes nothing; the driver's retrace
 * timeout then completes the tag.
 */
static void sgi_kona_swap_published(SGIKonaState *s, const SGICompletion *c)
{
    uint32_t tag = SGI_KONA_SWAP_COOKIE_TAG(c->cookie);
    int slot = SGI_KONA_SWAP_COOKIE_SLOT(c->cookie);
    SGIKonaPendingSwap *owner = NULL;

    for (unsigned i = 0; i < SGI_SWAP_MAX; i++) {
        if (s->pending_swaps[i].valid &&
            s->pending_swaps[i].seq == c->seq) {
            owner = &s->pending_swaps[i];
            break;
        }
    }
    switch (c->kind) {
    case SGI_DONE_SWAPPED:
        s->retrace_tag = tag;
        s->retrace_completed = true;
        /* Channel 0's scanout repaints itself after a swap; the rest too. */
        sgi_kona_invalidate_all(s);
        trace_sgi_kona_retrace_done(tag);
        if (owner && owner->context == slot && slot >= 0 &&
            slot < SGI_KONA_CONTEXTS && s->state[slot] &&
            s->contexts[slot].valid &&
            s->contexts[slot].generation == owner->generation &&
            s->state[slot]->window.double_buffered) {
            sgi_kona_gl_swap(s, slot);
        }
        break;
    case SGI_DONE_FAILED:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: retrace request tag 0x%" PRIx32 " past the"
                      " %d the scanout queues; not completed\n", tag,
                      SGI_SWAP_MAX);
        break;
    default:
        g_assert_not_reached();
    }
    if (owner) {
        memset(owner, 0, sizeof(*owner));
    }

}

static void sgi_kona_video_deliver(SGIKonaState *s)
{
    if ((s->int_cause & s->int_enable & SGI_KONA_INT_CAUSE_VIDEO) &&
        !s->video_delivered) {
        s->video_delivered = sgi_kona_raise_vector(s, true);
    }
}

/* Periodic and queued video work share one pending cause per field. */
static void sgi_kona_retrace(void *opaque)
{
    SGIKonaState *s = opaque;

    if (s->context_failed) {
        return;
    }
    s->retrace_completed = false;
    s->int_cause |= SGI_KONA_INT_CAUSE_VIDEO;
    sgi_kona_video_deliver(s);
}

/*
 * A completion (hw/display/sgi/head-ops.h), mapped onto what the driver
 * reads:
 *
 *   CONSUMED from a command port returns the packet's words to the
 *   credit register. CONSUMED from a fetched display list returns
 *   nothing: its words came from memory, not the port.
 *   WRITTEN sets the cause bit its cookie names and posts the cookie's
 *   mailbox word, which for a DMA packet whose word 6 is 0x100 is 0x100
 *   under cause bit 0x10: the driver's interrupt routine reads +0x800240
 *   and posts the semaphore its DMA write sleeps on. A cookie naming the
 *   video bit 0x8 sets it with no mailbox word.
 *   FAILED for an item whose sender waits posts nothing: no mailbox word,
 *   no cause bit, no vector, only a guest-error log line. The driver
 *   reports a failed host DMA in three places: widget error bits (read
 *   timeout, response error, map RAM, DMA FIFO), cause bits 0x800 and
 *   0x40 for input- and output-DMA overflow, and its own timeout when
 *   the completion never comes (pipe death reason 4). No single "DMA
 *   failed" cause bit exists, and cause 0x100 is parity. Which widget
 *   error a pipe raises for an unmapped address or invalid map entry is
 *   inferred only (bit 8 or 13), and the model has no widget-error
 *   vector, so it takes the third path: the completion is withheld and
 *   the driver's timeout fires, as for a pipe whose read got no reply.
 *
 * A mailbox post then sends the general vector, after the word and the
 * cause bit, since only that vector's handler reads them. Whether the
 * pipe withholds the vector while the driver's mask at +0x800230 has bit
 * 0x10 clear is inferred and not modelled: the handler applies its own
 * copy of the mask either way. A FAILED completion sends no vector.
 */
void sgi_kona_publish(void *opaque, const SGICompletion *c)
{
    SGIKonaState *s = opaque;
    uint32_t cause = SGI_KONA_DMA_COOKIE_CAUSE(c->cookie);
    uint32_t word = SGI_KONA_DMA_COOKIE_WORD(c->cookie);

    if (s->context_failed) {
        return;
    }
    trace_sgi_kona_completion(c->seq, c->kind, c->cookie, c->payload);
    if (c->cookie & SGI_KONA_SWAP_COOKIE_FLAG) {
        sgi_kona_swap_published(s, c);
        return;
    }
    switch (c->kind) {
    case SGI_DONE_CONSUMED:
        if (c->cookie == SGI_KONA_DMA_COOKIE_PORT) {
            assert(c->payload <= s->credit_pending);
            s->credit_pending -= c->payload;
        }
        break;
    case SGI_DONE_WRITTEN:
        if (c->cookie == SGI_KONA_DMA_COOKIE_NONE) {
            break;
        }
        if (cause & SGI_KONA_INT_CAUSE_MAILBOX) {
            sgi_kona_mailbox_post(s, word);
        }
        s->int_cause |= cause & SGI_KONA_INT_CAUSE_VIDEO;
        /*
         * The kernel's DMA write waits for the mailbox's 0x100 in the
         * general vector's handler, which reads the cause register and
         * the mailbox only when that vector arrives (established from
         * the driver's interrupt handlers); nothing polls them.
         */
        if (cause & SGI_KONA_INT_CAUSE_MAILBOX) {
            sgi_kona_raise_vector(s, true);
        }
        break;
    case SGI_DONE_FAILED:
        if (c->cookie != SGI_KONA_DMA_COOKIE_NONE &&
            c->cookie != SGI_KONA_DMA_COOKIE_PORT) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: host DMA failed"
                          " (reason %" PRIu64 "); completion word 0x%"
                          PRIx32 " withheld\n", c->payload, word);
        }
        break;
    default:
        break;
    }
}

/*
 * A command port framed a packet of `words` words: the item is issued and
 * its words held until its CONSUMED completion comes back, which, since
 * the model executes a packet as it frames it, is at once. A packet
 * framed from a fetched display list's words took nothing from the port
 * and is not counted.
 */
void sgi_kona_port_consumed(SGIKonaState *s, uint32_t words)
{
    s->pi_packets++;
    if (s->dma.fetch.open) {
        return;
    }
    s->credit_pending += words;
    sgi_head_publish(&sgi_kona_head_ops, s, s->dma.seq++, SGI_DONE_CONSUMED,
                     SGI_KONA_DMA_COOKIE_PORT, words);
}

/* The words a command-port framer holds of a packet still arriving. */
static uint32_t sgi_kona_port_held(const SGICommandPort *p)
{
    return p->framing ? 1 + p->argc_have : 0;
}

/* The credit register: see SGI_KONA_CREDIT. */
static uint32_t sgi_kona_credit(SGIKonaState *s)
{
    uint64_t held = (uint64_t)sgi_kona_port_held(&s->textport) +
                    sgi_kona_port_held(&s->kernel_port) +
                    sgi_kona_port_held(&s->gl_port) +
                    sgi_kona_port_held(&s->dma.fifo_port) +
                    s->credit_pending;

    return (held << 1) & SGI_KONA_CREDIT_MASK;
}

/*
 * Whether the processor store now arriving was charged a graphics credit
 * by its processor interface. A qtest access, which no processor makes,
 * never was, and is not asked about: the interface would log the question
 * as a head asking off a processor's thread.
 */
static bool sgi_kona_store_counted(void)
{
    return current_cpu && sgi_pi_gfx_store_counted();
}

/*
 * Return credits to the Hub that sent the store now arriving. With none,
 * on the board's own thread settling credits held since the last store,
 * they go to that store's Hub, or with no store yet to the "pi" link, as
 * a qtest's store's do.
 */
static void sgi_kona_pi_return(SGIKonaState *s, uint32_t credits)
{
    Object *node = s->work_replaying && s->work_replay_node ?
                   s->work_replay_node :
                   sgi_pi_gfx_store_source() ?: s->pi_store_node ?: s->pi;

    if (!credits) {
        return;
    }
    /*
     * When the IRIX driver switches a thread out, it parks the credit
     * target, writing 1 to +0x7c and reading it back, and then detaches the
     * thread's flow; when it switches the thread back in, it attaches the
     * flow, writes back the counter it saved and restores the target (0x22,
     * its credit transaction number shifted left one). Credits freed while
     * the target is parked go back once it is restored. Inferred: a real
     * pipe's credits for stores already taken when the target is parked
     * must reach the counter after it is restored, or IRIX's count would
     * fall short of its total; how the pipe holds them is not known.
     */
    if (s->low_window_write & 1) {
        s->pi_parked += credits;
        return;
    }
    if (!node) {
        if (!s->pi_unlinked_logged) {
            s->pi_unlinked_logged = true;
            qemu_log_mask(LOG_UNIMP, "sgi-kona: no \"pi\" link; graphics"
                          " credits are not returned\n");
        }
        return;
    }
    /* The board's own field, not a checked cast: this runs every store. */
    trace_sgi_kona_pi_credits(s->parent_obj.slot, credits);
    /* Keyed on the store's own node: either Hub may master the pipe. */
    if (s->pi_class_node != node) {
        s->pi_class_node = node;
        s->pi_class = SGI_PI_GFX_GET_CLASS(node);
    }
    if (!s->pi_class->return_credits) {
        /* Logs that the node returns nothing. */
        sgi_pi_gfx_return_credits(node, s->parent_obj.slot, credits);
        return;
    }
    s->pi_class->return_credits(node, s->parent_obj.slot, credits);
}

/*
 * The graphics credits of the processor stores the board takes. The
 * driver's flow control charges one credit in the storing processor's
 * interface for each uncached store into the page it enabled for the
 * pipe, and the driver's credit wait waits for the count to come
 * back to the total it wrote before it lets the next process attach,
 * killing the pipe (death reason 25) when it does not. The board returns
 * one credit for each charged store once the words the store carried
 * have been taken: at once for a word a framer consumed or a data run
 * took, and with the packet's last word for one a framer still holds, so
 * the credits of a packet come back together. A doubleword store carries
 * two words and earns one credit, as the interface charged one. Words a
 * display list fetched from memory were never stored and earn none.
 *
 * So the stores still owed are those whose last word went into the
 * packet a framer holds after this store. If a packet ended while this
 * store was taken (`packets` is the framed-packet count before it) and
 * another is open now, the open one began in this store: every earlier
 * store's words are taken and its credit goes back, and only this store
 * stays owed. A GL client's doubleword stores often end that way, a
 * vertex's last float and the next selector in one store; returning
 * credits only when no packet was open ran the counter past zero there.
 * The rule assumes one framer holds a partial packet at a time. Two cases
 * break it, each returning some stores' credits early and none twice: a
 * DMA push packet open on its own port while command-page packets
 * complete, and a user command suspended by the kernel's forced bracket
 * (sgi_kona_bracket_write()) while the kernel framer takes its packet.
 * When that kernel packet ends, the suspended command's stores go back
 * although their words are still held, and the kernel's last store stays
 * owed until a store ends with nothing open.
 *
 * Inferred: that the pipe returns a credit when its front end takes a
 * store's words, and holds the credits of a packet still arriving, which
 * is the same reading SGI_KONA_CREDIT gives the board's own credit
 * register. That register stays: the PROM tests it for zero before every
 * textport command, and it and these credits both count the words a
 * framer holds. The credits are write responses, so they go back to the
 * Hub that sent the store now arriving, whose map for this widget routes
 * them; on the two-node Onyx2 deskside that can be either node's Hub. A
 * store with no sender (a qtest's) uses the "pi" link, and with neither
 * the board logs once and returns nothing. Limitation: credits held from
 * earlier stores go back with this store's, to its sender, so stores of
 * one packet sent through two Hubs would return some to the wrong one.
 */
static void sgi_kona_pi_credits(SGIKonaState *s, bool counted,
                                uint64_t packets)
{
    bool open = sgi_kona_port_held(&s->textport) ||
                sgi_kona_port_held(&s->kernel_port) ||
                sgi_kona_port_held(&s->gl_port) ||
                sgi_kona_port_held(&s->dma.fifo_port);
    uint32_t credits;

    if (!open) {
        credits = s->pi_held + counted;
        s->pi_held = 0;
    } else if (s->pi_packets != packets) {
        credits = s->pi_held;
        s->pi_held = counted;
    } else {
        s->pi_held += counted;
        return;
    }
    sgi_kona_pi_return(s, credits);
}

static void sgi_kona_textport_packet(void *opaque, unsigned port_id,
                                     uint32_t selector, const uint32_t *args,
                                     unsigned argc)
{
    SGIKonaState *s = opaque;

    if (s->context_failed) {
        return;
    }
    sgi_kona_port_consumed(s, 1 + argc);

    switch (selector) {
    case 0x2:
        /*
         * Set color: a colormap index, not the packed 0x00RRGGBB value
         * this model previously assumed -- established from a real
         * captured boot trace, whose argument values run 0-255, the
         * identical domain command 3's own index argument uses.
         */
        s->draw_color = args[0] & 0xff;
        break;
    case 0x3:
        /*
         * Load colormap: index, then R/G/B each pre-scaled <<2 by the
         * sender -- established from the same trace, whose component
         * values run 0-1020 (255 * 4), with one outlier value treated as
         * capture noise. Rescale back down on store.
         */
        {
            uint32_t index = args[0] & 0xff;
            uint32_t r = (args[1] >> 2) & 0xff;
            uint32_t g = (args[2] >> 2) & 0xff;
            uint32_t b = (args[3] >> 2) & 0xff;

            s->colormap[index] = (r << 16) | (g << 8) | b;
            /* Pixels already drawn in this entry change colour. */
            sgi_kona_invalidate_all(s);
        }
        break;
    case 0x5:
        /* Filled rectangle: see sgi_kona_fill_rect()'s own comment. */
        sgi_kona_fill_rect(s, (int32_t)args[0], (int32_t)args[1],
                           (int32_t)args[2], (int32_t)args[3]);
        break;
    case 0x6:
        /* Point: plots (a, b) in the current draw color. */
        sgi_kona_draw_point(s, (int32_t)args[0], (int32_t)args[1]);
        break;
    case 0x8:
        /*
         * Screen size: accepted and traced only; the managed area is
         * the video combination's (sgi_kona_managed_resize()).
         */
        break;
    case 0x9:
        /* Sync probe. */
        sgi_kona_mailbox_enqueue(s, (uint32_t[]){ 0x1234abcd }, 1);
        break;
    case 0xc:
        /*
         * Graphics info: width/height are established; the rest of the
         * reply is approximated zero.
         */
        sgi_kona_mailbox_enqueue(
            s, (uint32_t[]){ 0, 0, 0, s->fb_width, s->fb_height },
            5);
        break;
    case 0x4:
        /*
         * Raster position: established from the IO6 PROM's textport
         * raster-position routine as the position every subsequent
         * bitmap blit (command 0xa) places
         * relative to -- see sgi_kona_bitmap_blit()'s own comment. Draws
         * nothing itself; a completed blit is what advances it.
         */
        s->raster_x = (int32_t)args[0];
        s->raster_y = (int32_t)args[1];
        break;
    case 0x1:
    case 0x7:
        /*
         * The argument shape is known, but no rendering, blanking or cursor
         * role is: these commands are framed and traced without display
         * effects.
         */
        break;
    default:
        g_assert_not_reached(); /* not in sgi_kona_textport_selectors[] */
    }
    s->textport_expect_sync = true;
}

static void sgi_kona_textport_unsupported(void *opaque, unsigned port_id,
                                          uint32_t word)
{
    SGIKonaState *s = opaque;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-kona: unsupported textport selector 0x%" PRIx32 "\n",
                  word);
    trace_sgi_kona_word_dropped(0, word, "unsupported textport selector");
    s->textport_expect_sync = true;
}

static const SGICommandPortOps sgi_kona_textport_ops = {
    .packet = sgi_kona_textport_packet,
    .unsupported = sgi_kona_textport_unsupported,
};

static bool sgi_kona_kernel_token_known(SGIKonaState *s, uint32_t word)
{
    return sgi_command_port_selector(&s->kernel_port, word) != NULL;
}

/*
 * A word for the kernel framer. A word that opens a command records
 * whether the privileged bit was set, which decides what a forced store
 * does to the command if it is still open (sgi_kona_bracket_write()).
 */
static void sgi_kona_kernel_port_word(SGIKonaState *s, uint32_t word)
{
    if (!s->kernel_port.framing) {
        s->kernel_port_privileged = s->kernel_input;
    }
    sgi_command_port_write(&s->kernel_port, word, 4);
}

/* A word that can follow the textport's sync word as its command. */
static bool sgi_kona_textport_selector_known(uint32_t word)
{
    if (word == SGI_KONA_BITMAP_SELECTOR) {
        return true;
    }
    for (unsigned i = 0; i < sgi_kona_textport_num_selectors; i++) {
        if (sgi_kona_textport_selectors[i].selector == word) {
            return true;
        }
    }
    return false;
}


/*
 * The page a 0x1f1 or 0x1f2 command names in its first two words a and b.
 * With bit 28 of a set the page is host physical; otherwise a << 12 plus
 * b's low twelve bits is a device address, translated through the map RAM
 * entry for its 16 KiB page. Both readings are inferred from the driver's
 * uses, not from a hardware description (see SGI_KONA_PAGE_WORD_PHYSICAL).
 * A device address the map RAM does not map yields nothing.
 */
static bool sgi_kona_command_page(SGIKonaState *s, const char *what,
                                  uint32_t page, uint32_t offset,
                                  uint64_t *addr, bool *mapped)
{
    uint64_t device;
    uint32_t index;
    uint32_t entry;

    if (page & SGI_KONA_PAGE_WORD_PHYSICAL) {
        *addr = ((uint64_t)(page & SGI_KONA_PAGE_WORD_FRAME) << 12) |
                (offset & 0xfff);
        *mapped = false;
        return true;
    }

    device = ((uint64_t)page << 12) | (offset & 0xfff);
    index = device >> SGI_KONA_MAPRAM_PAGE_SHIFT;
    entry = index < sgi_kona_mapram_entry_count(s) ? s->map_ram[index] : 0;
    if ((entry & SGI_KONA_MAPRAM_KIND_MASK) != SGI_KONA_MAPRAM_KIND_16K) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: %s page at device address 0x%" PRIx64
                      " is not mapped (map RAM entry 0x%08x); not"
                      " registered\n", what, device, entry);
        return false;
    }
    *addr = ((uint64_t)(entry & SGI_KONA_PAGE_WORD_FRAME) << 12) +
            (device & ((1u << SGI_KONA_MAPRAM_PAGE_SHIFT) - 1));
    *mapped = true;
    return true;
}

/*
 * 0x1f1 a b len: remember the page the pipe reports reasons in. A page
 * that does not translate leaves any earlier registration in place.
 */
static void sgi_kona_register_reason_page(SGIKonaState *s, uint32_t page,
                                          uint32_t offset)
{
    bool mapped;

    if (sgi_kona_command_page(s, "reason", page, offset, &s->reason_page,
                              &mapped)) {
        s->reason_page_valid = true;
        s->reason_unread = false;
        trace_sgi_kona_reason_page(s->reason_page, mapped);
    }
}

static void sgi_kona_x_read_invalidate(SGIKonaState *s)
{
    s->x_read_gc_valid = false;
    s->x_read_setup_valid = false;
}

/* A retained outgoing page never authorizes a new context's indexed read. */
static bool sgi_kona_x_read_current(const SGIKonaState *s)
{
    int i = s->context_current;

    return i >= 0 && i < SGI_KONA_CONTEXTS && s->contexts[i].valid &&
           !s->context_page_pending && s->contexts[i].return_page_valid &&
           s->return_page_valid &&
           s->return_page == s->contexts[i].return_page;
}

/*
 * 0x1f2 a b len: remember the return-data page, where the pipe answers a
 * process's requests and the process polls word 0. The kernel's context
 * switch sends 0x1f2 straight after a context's first-activation 0x1fd and
 * never on a restore, so that page becomes the context's own, restored with
 * it by 0x1f9 (see sgi_kona_context_activate()). That the pipe keeps a
 * return page per context is inferred from those two paths. A 0x1f2 with no
 * first activation waiting, as gfxinit and the X server send one, only
 * replaces the page the pipe answers on. A page that does not translate
 * leaves any earlier registration in place, except for a context waiting
 * for its page, which is left with none rather than another context's.
 */
static void sgi_kona_register_return_page(SGIKonaState *s, uint32_t page,
                                          uint32_t offset)
{
    SGIKonaContext *c = NULL;
    uint64_t addr;
    bool mapped;

    if (s->context_page_pending) {
        c = &s->contexts[s->context_current];
        s->context_page_pending = false;
    }
    if (!sgi_kona_command_page(s, "return", page, offset, &addr, &mapped)) {
        if (c) {
            s->return_page_valid = false;
        }
        if (!sgi_kona_x_read_current(s)) {
            sgi_kona_x_read_invalidate(s);
        }
        return;
    }
    s->return_page = addr;
    s->return_page_valid = true;
    trace_sgi_kona_return_page(addr, mapped);
    if (c) {
        c->return_page = addr;
        c->return_page_valid = true;
        trace_sgi_kona_context_return_page(s->context_current, addr);
    }
    if (!sgi_kona_x_read_current(s)) {
        sgi_kona_x_read_invalidate(s);
    }
}

static void sgi_kona_gl_state_init(SGIKonaState *s, SGIKonaGLState *state,
                                    unsigned slot);
static void sgi_kona_renderer_stop(SGIKonaState *s);

static void sgi_kona_state_free(SGIKonaGLState *state)
{
    if (!state) {
        return;
    }
    sgi_gl_cleanup(&state->gl);
    for (unsigned i = 0; i < SGI_KONA_GL_ATTRIB_DEPTH; i++) {
        sgi_gl_cleanup(&state->attrib[i]);
    }
    g_free(state->parked_upload.pixels);
    g_free(state);
}

static void sgi_kona_contexts_clear(SGIKonaState *s)
{
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        sgi_kona_state_free(s->state[i]);
        s->state[i] = NULL;
    }
    memset(s->contexts, 0, sizeof(s->contexts));
    memset(s->pending_swaps, 0, sizeof(s->pending_swaps));
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        sgi_kona_saved_input_clear(&s->saved_input[i]);
    }
    s->context_count = 0;
    s->context_current = SGI_KONA_CONTEXT_NONE;
    s->texture_upload_context = SGI_KONA_CONTEXT_NONE;
    s->texture_regs_context = SGI_KONA_CONTEXT_NONE;
    sgi_kona_texture_set_owner(&s->textures, 0);
    s->context_page_pending = false;
}

/*
 * The texture owner of context table entry `slot`: texture handles are each
 * context's own (see SGI_KONA_TEXTURE_OWNERS), and 0 is no context.
 */
QEMU_BUILD_BUG_ON(SGI_KONA_CONTEXTS + 1 > SGI_KONA_TEXTURE_OWNERS);

static uint32_t sgi_kona_texture_owner(unsigned slot)
{
    return slot + 1;
}

static SGIKonaGLState *sgi_kona_state_new(SGIKonaState *s, unsigned slot)
{
    SGIKonaGLState *state;

    if (s->test_context_alloc_fail) {
        s->test_context_alloc_fail = false;
        return NULL;
    }
    state = g_try_new0(SGIKonaGLState, 1);
    if (state) {
        sgi_kona_gl_state_init(s, state, slot);
        for (unsigned i = 0; i < SGI_KONA_GL_ATTRIB_DEPTH; i++) {
            sgi_gl_init(&state->attrib[i], slot);
        }
    }
    return state;
}

static void sgi_kona_context_stop(SGIKonaState *s, uint32_t token,
                                  uint32_t page, uint32_t offset,
                                  const char *reason)
{
    s->context_failed = true;
    snprintf(s->context_error, sizeof(s->context_error),
             "sgi-kona: command 0x%x key 0x%x:0x%x: %s "
             "(%u retained, limit %u)", token, page, offset, reason,
             s->context_count, SGI_KONA_CONTEXT_LIMIT);
    qemu_log_mask(LOG_GUEST_ERROR, "%s\n", s->context_error);
    error_report("%s", s->context_error);
    assert(bql_locked());
    qemu_system_vmstop_request_prepare();
    qemu_system_vmstop_request(RUN_STATE_INTERNAL_ERROR);
}



/*
 * 0x1fd page offset len, or 0x1f9 word0 page offset sentinel: make the
 * context whose save area the two words name current, and answer on its
 * return page from here on. A 0x1fd starts the context afresh, its GL
 * state and window included, with no
 * return page until the 0x1f2 that follows it. A 0x1f9 restores the page
 * that 0x1f2 gave it; one naming a save area no 0x1fd named is logged and
 * leaves the pipe with no return page, so an answer goes nowhere rather
 * than to another context. The key is the two words as sent, which the
 * kernel derives from one fixed allocation per context. See
 * SGIKonaContext for the key. The host limit never evicts a logical record.
 */
static void sgi_kona_gl_image_clear(SGIKonaState *s);
/* The uploads an open GL data run fills (sgi_kona_gl_run_uploads()). */
#define SGI_KONA_RUN_TEXTURE (1u << 0)
#define SGI_KONA_RUN_IMAGE   (1u << 1)
#define SGI_KONA_RUN_BITMAP  (1u << 2)
#define SGI_KONA_RUN_MAP     (1u << 3)
static unsigned sgi_kona_gl_run_uploads(SGIKonaState *s);

static void sgi_kona_texture_upload_clear(SGIKonaState *s);
static bool sgi_kona_gl_imaging_active(const SGIKonaGLState *st);

/*
 * The imaging stage bits (SGIKonaGLImaging.stages): histogram and minmax,
 * then the three colour tables and the texture colour table.
 */
#define SGI_KONA_GL_STAGE_TABLE(n)    (1u << (2 + (n)))

/*
 * Whether context i's image transfers are refused, for pixel state the
 * model does not keep.
 */
static bool sgi_kona_gl_transfer_refused(const SGIKonaState *s, int i)
{
    return i != SGI_KONA_CONTEXT_NONE &&
           s->state[i]->pipe.pixel_transfer_unsupported;
}
static int sgi_kona_gl_filter_2d(const SGIKonaGLImaging *im);
static bool sgi_kona_gl_post_conv_default(const SGIKonaGLPixelTransfer *t);
static bool sgi_kona_gl_post_conv(const SGIKonaGLPixelTransfer *t,
                                  double rgba[4]);
static bool sgi_kona_gl_matrix_stage(const uint32_t m[16],
                                     const SGIKonaGLPixelTransfer *t);
static bool sgi_kona_gl_post_matrix(const uint32_t m[16],
                                    const SGIKonaGLPixelTransfer *t,
                                    double rgba[4]);
static bool sgi_kona_gl_imaging(SGIKonaGLState *st,
                                double (**image)[4], uint32_t *width,
                                uint32_t *height);
static bool sgi_kona_gl_pixel_path(SGIKonaGLState *st, double rgba[4]);
static bool sgi_kona_gl_histogram_sinks(const SGIKonaGLState *st);
static bool sgi_kona_gl_histogram_active(const SGIKonaGLState *st);
static unsigned sgi_kona_gl_histogram_slots(uint32_t format, unsigned c[4]);
static void sgi_kona_gl_histogram_stage(SGIKonaGLState *st,
                                        const double rgba[4]);
static bool sgi_kona_gl_tables_active(const SGIKonaGLState *st);
static void sgi_kona_gl_colour_table_word(SGIKonaState *s, uint32_t word);
static const SGIKonaGLColourTable *
sgi_kona_gl_table_enabled(const SGIKonaGLState *st, unsigned n);
static void sgi_kona_gl_table_lookup(const void *table, double rgba[4]);
static void sgi_kona_gl_table_stage(const SGIKonaGLState *st, unsigned n,
                                    double rgba[4]);
static void sgi_kona_gl_colour_table_open(SGIKonaState *s, int i,
                                          const uint32_t *args);
static void sgi_kona_gl_colour_table_parameters(SGIKonaState *s, int i,
                                                const uint32_t *args);
static void sgi_kona_gl_colour_table_get(SGIKonaState *s, int i,
                                         const uint32_t *args);
static void sgi_kona_gl_colour_table_get_parameter(SGIKonaState *s, int i,
                                                   const uint32_t *args);
static uint32_t sgi_kona_gl_table_base(uint32_t format);
static void sgi_kona_gl_histogram(SGIKonaState *s, int i, uint32_t selector,
                                  const uint32_t *args);
static void sgi_kona_texture_upload_switch(SGIKonaState *s, int slot);
static void sgi_kona_texture_regs_switch(SGIKonaState *s, int slot);

static uint64_t sgi_kona_context_generation(SGIKonaState *s, int context)
{
    return context >= 0 && context < SGI_KONA_CONTEXTS && s->state[context] ?
           s->contexts[context].generation : 0;
}

static bool sgi_kona_context_owner(SGIKonaState *s, int context,
                                   uint64_t generation)
{
    return generation && sgi_kona_context_generation(s, context) == generation;
}

static void sgi_kona_gl_read_dma_clear(SGIKonaState *s)
{
    g_free(s->gl_read_dma.data);
    memset(&s->gl_read_dma, 0, sizeof(s->gl_read_dma));
}

static void sgi_kona_gl_read_dma_refuse(SGIKonaState *s)
{
    SGIKonaGLReadDMA *d = &s->gl_read_dma;

    if (d->active) {
        g_clear_pointer(&d->data, g_free);
        d->count = 0;
        d->selector = 0;
        memset(d->header, 0, sizeof(d->header));
        d->refused = true;
    }
}

static void sgi_kona_context_activate(SGIKonaState *s, uint32_t token,
                                      uint32_t page, uint32_t offset)
{
    bool first = token == SGI_KONA_CMD_CONTEXT_MAP;
    bool fresh = first;
    uint64_t device = ((uint64_t)page << 12) | (offset & 0xfff);
    SGIKonaContext *c;
    SGIKonaGLState *replacement = NULL;
    int slot = SGI_KONA_CONTEXT_NONE;
    unsigned kept;

    /* The restore's 0x1e4 packets place the new context's regions. */
    s->dma.list_base = 0;
    offset &= 0xfff;
    for (unsigned i = 0; i < s->context_count; i++) {
        c = &s->contexts[i];
        if (c->save_page == page && c->save_offset == offset) {
            slot = i;
            break;
        }
    }
    if (slot == SGI_KONA_CONTEXT_NONE && !first) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: context restore names save area 0x%"
                      PRIx64 ", which no 0x%x set up; no return page\n",
                      device, SGI_KONA_CMD_CONTEXT_MAP);
        slot = SGI_KONA_CONTEXT_TRANSIENT;
        fresh = true;
    }
    if (slot == SGI_KONA_CONTEXT_NONE) {
        if (s->context_count == SGI_KONA_CONTEXT_LIMIT) {
            sgi_kona_context_stop(s, token, page, offset,
                                  "logical context limit reached");
            return;
        }
        slot = s->context_count;
    }
    if (fresh) {
        if (s->context_clock == UINT64_MAX) {
            sgi_kona_context_stop(s, token, page, offset,
                                  "logical context generation exhausted");
            return;
        }
        replacement = sgi_kona_state_new(s, slot);
        if (!replacement) {
            sgi_kona_context_stop(s, token, page, offset,
                                  "logical context state allocation failed");
            return;
        }
        /* A context started afresh has no command of its own left open. */
        sgi_kona_saved_input_clear(&s->saved_input[slot]);
        /* Nor any texture: its handles name none of a former one's. */
        sgi_kona_texture_drop_owner(&s->textures,
                                    sgi_kona_texture_owner(slot));
        if (s->x_pixel_remaining && s->x_pixel_owner == slot) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: context %d started"
                          " afresh with its X pixel write %" PRIu32
                          " words short; the write is refused\n", slot,
                          s->x_pixel_remaining);
            s->x_pixel_remaining = 0;
            s->x_write_refused = true;
        }
        if ((s->x_boxes_have || s->x_clip_run_have) &&
            s->x_run_owner == slot) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: context %d started"
                          " afresh with an X data run of %" PRIu32
                          " words open; the run is dropped\n", slot,
                          s->x_boxes_have + s->x_clip_run_have);
            sgi_kona_x_read_invalidate(s);
            s->x_boxes_have = 0;
            s->x_clip_run_have = 0;
        }
        /* Rendering and return writes are synchronous under the big lock. */
        sgi_kona_renderer_stop(s);
        if (s->texture_upload_context == slot) {
            sgi_kona_texture_upload_clear(s);
        }
        if (s->texture_regs_context == slot) {
            s->texture_regs_context = SGI_KONA_CONTEXT_NONE;
        }
        sgi_kona_state_free(s->state[slot]);
        s->state[slot] = replacement;
        s->contexts[slot] = (SGIKonaContext) {
            .valid = true,
            .save_page = page,
            .save_offset = offset,
            .generation = ++s->context_clock,
        };
        if (first && slot == s->context_count) {
            s->context_count++;
        }
    }
    sgi_kona_x_read_invalidate(s);
    sgi_kona_gl_read_dma_refuse(s);
    c = &s->contexts[slot];
    /* A suspended run's uploads go with its context, unless it starts anew. */
    kept = s->suspended_owner != SGI_KONA_CONTEXT_NONE &&
           !(fresh && s->suspended_owner == slot) ?
           sgi_kona_gl_run_uploads(s) : 0;
    if (!(kept & SGI_KONA_RUN_TEXTURE) && s->texture_upload.draw_pixels &&
        (fresh || s->context_current != slot)) {
        sgi_kona_texture_upload_clear(s);
    }
    if (!(kept & SGI_KONA_RUN_IMAGE) && s->gl_image_upload.count &&
        (fresh || s->context_current != slot)) {
        s->gl_image_upload.refused = true;
    }
    if (!(kept & SGI_KONA_RUN_BITMAP) && s->gl_bitmap_upload.count &&
        (fresh || s->context_current != slot)) {
        s->gl_bitmap_upload.refused = true;
    }
    if (!(kept & SGI_KONA_RUN_MAP) && s->gl_map_upload.header[1] &&
        (fresh || s->context_current != slot)) {
        s->gl_map_upload.refused = true;
    }
    s->texture_retire_pending = false;
    sgi_kona_texture_upload_switch(s, slot);
    sgi_kona_texture_regs_switch(s, slot);
    s->context_current = slot;
    sgi_kona_texture_set_owner(&s->textures, sgi_kona_texture_owner(slot));
    s->gl_grammar = true;
    s->context_page_pending = first;
    s->return_page_valid = c->return_page_valid;
    s->return_page = c->return_page;
    trace_sgi_kona_context_activate(token, device, slot, c->return_page_valid,
                                    c->return_page);
}

/*
 * The fabric address of a packet to `widget`, the widget in bits 51:48,
 * as every XIO board composes it (hw/mips/sgi/xio-widget.h).
 */
static bool sgi_kona_fabric_compose(SGIKonaState *s, unsigned int widget,
                                    uint64_t address, hwaddr *fabric)
{
    return sgi_xio_compose("sgi-kona", widget, address, fabric);
}

/*
 * Send an interrupt vector to the destination in
 * SGI_KONA_INT_DEST_UPPER/LOWER, as an assert-only interrupt-modify
 * write: the driver clears the Hub's pending bit itself, and no evidence
 * shows the pipe sending a deassert. The geometry-engine vector
 * (SGI_KONA_INT_VECTORS bits [16:9]) carries a posted reason; the general
 * vector (bits [7:0]) the cause bits. That both go to the one destination
 * the driver programs is inferred: the driver's adaptor reset writes one
 * destination and three vectors. Only video delivery uses the mask;
 * mailbox and geometry-engine delivery retain their existing rules.
 */
static bool sgi_kona_raise_vector(SGIKonaState *s, bool general)
{
    uint8_t vector = general ?
                     s->int_vectors & SGI_KONA_INT_VECTORS_GENERAL_MASK :
                     s->int_vectors >> SGI_KONA_INT_VECTORS_GE_SHIFT;
    const char *what = general ? "general" : "geometry-engine";
    unsigned int widget = (s->int_dest_upper >>
                           SGI_KONA_INT_DEST_WIDGET_SHIFT) &
                          SGI_KONA_INT_DEST_WIDGET_MASK;
    uint64_t address = ((uint64_t)(s->int_dest_upper & 0xffff) << 32) |
                       s->int_dest_lower;
    hwaddr where;
    MemTxResult result;

    if (!s->int_dest_upper && !s->int_dest_lower) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: %s interrupt with no"
                      " destination programmed; no packet sent\n", what);
        return false;
    }
    if (!sgi_kona_fabric_compose(s, widget, address, &where)) {
        return false;
    }
    if (general) {
        trace_sgi_kona_general_interrupt(where, vector);
    } else {
        trace_sgi_kona_ge_interrupt(where, vector);
    }
    result = sgi_xio_send_interrupt(&s->fabric_as, where, vector, true);

    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: %s interrupt delivery failed (result %d)\n",
                      what, result);
        return false;
    }
    return true;
}

/*
 * Write `len` bytes at host physical `addr` through the board's fabric to
 * the fixed fabric_hub_widget, the same address space and destination the
 * DMA engine uses. False, and logged, if the write does not complete.
 */
static bool sgi_kona_host_write(SGIKonaState *s, uint64_t addr,
                                const void *buf, uint32_t len)
{
    hwaddr where;
    MemTxResult result;

    if (!sgi_kona_fabric_compose(s, s->fabric_hub_widget, addr, &where)) {
        return false;
    }
    result = dma_memory_write(&s->fabric_as, where, buf, len,
                              MEMTXATTRS_UNSPECIFIED);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: %u-byte write to 0x%" PRIx64
                      " failed (result %d)\n", len, where, result);
        return false;
    }
    trace_sgi_kona_host_write(addr, len);
    return true;
}

/*
 * Completion reasons on the reason page. 0x1f8 R and 0x1dd R post R, 0x1fc
 * its fixed 0x101, a select feed 0x100 with its words, a swap-out 0x102
 * and a suspend 0x104 with its reply words: written big-endian from word 0
 * of the page, then the geometry-engine cause bit set and its interrupt
 * raised, which is what the driver's geometry-engine interrupt handler
 * reads and dispatches on. The handler reads word 0 once an interrupt and
 * writes it back ORed with 0xbad00000 (established from the IRIX kernel
 * driver), so a second reason written before that read would replace the
 * first, and a swap-out the kernel counts one completion at a time for
 * would time out. The model therefore posts one reason at a time: a
 * reason that finds the last one's word 0 still on the page waits in a
 * queue, and goes out once the page shows the driver's rewrite, looked for
 * when the cause register is accessed and every millisecond of virtual
 * time while reasons wait.
 *
 * Approximated: hardware is inferred to post only once its queued work is
 * done, and the model has no queued work, so a reason with nothing ahead
 * of it posts at once; how a real pipe orders and paces several reasons
 * is unknown. With no page registered it logs, traces and posts nothing.
 */
#define SGI_KONA_REASON_POLL_NS (1000 * 1000)

static void sgi_kona_reason_write(SGIKonaState *s, uint32_t token,
                                  const uint32_t *words, unsigned count)
{
    uint8_t bytes[SGI_KONA_REASON_WORDS * 4];

    for (unsigned k = 0; k < count; k++) {
        stl_be_p(bytes + 4 * k, words[k]);
    }
    if (!sgi_kona_host_write(s, s->reason_page, bytes, 4 * count)) {
        return;
    }
    trace_sgi_kona_reason_post(token, words[0], s->reason_page);
    s->reason_unread = true;
    s->reason_posted = words[0];
    s->int_cause |= SGI_KONA_INT_CAUSE_GE;
    sgi_kona_raise_vector(s, false);
}

/*
 * Whether the last reason's word 0 is still on the page unread. A page
 * that cannot be read back cannot hold the next reason back.
 */
static bool sgi_kona_reason_unread(SGIKonaState *s)
{
    uint8_t bytes[4];
    hwaddr where;

    if (!s->reason_unread) {
        return false;
    }
    if (!s->reason_page_valid ||
        !sgi_kona_fabric_compose(s, s->fabric_hub_widget, s->reason_page,
                                 &where) ||
        dma_memory_read(&s->fabric_as, where, bytes, sizeof(bytes),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
        ldl_be_p(bytes) != s->reason_posted) {
        s->reason_unread = false;
    }
    return s->reason_unread;
}

/* Post queued reasons while the page is free; look again later if not. */
static void sgi_kona_reason_drain(SGIKonaState *s)
{
    while (s->reason_queue_len && !sgi_kona_reason_unread(s)) {
        uint32_t token = s->reason_queue_token[0];
        uint32_t count = s->reason_queue_words[0];
        uint32_t words[SGI_KONA_REASON_WORDS];

        memcpy(words, s->reason_queue[0], sizeof(words));
        s->reason_queue_len--;
        memmove(s->reason_queue, s->reason_queue + 1,
                s->reason_queue_len * sizeof(s->reason_queue[0]));
        memmove(s->reason_queue_token, s->reason_queue_token + 1,
                s->reason_queue_len * sizeof(s->reason_queue_token[0]));
        memmove(s->reason_queue_words, s->reason_queue_words + 1,
                s->reason_queue_len * sizeof(s->reason_queue_words[0]));
        sgi_kona_reason_write(s, token, words, count);
    }
    if (s->reason_queue_len && s->reason_timer) {
        timer_mod(s->reason_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  SGI_KONA_REASON_POLL_NS);
    }
}

static void sgi_kona_reason_timer(void *opaque)
{
    sgi_kona_reason_drain(opaque);
}

static void sgi_kona_post_reason_words(SGIKonaState *s, uint32_t token,
                                       const uint32_t *words, unsigned count)
{
    uint32_t n = s->reason_queue_len;

    assert(count && count <= SGI_KONA_REASON_WORDS);
    if (!s->reason_page_valid) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%x reason 0x%x with no reason page"
                      " registered; nothing posted\n", token, words[0]);
        trace_sgi_kona_reason_no_page(token, words[0]);
        return;
    }
    if (!n && !sgi_kona_reason_unread(s)) {
        sgi_kona_reason_write(s, token, words, count);
        return;
    }
    if (n == SGI_KONA_REASON_QUEUE) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: %u completion reasons"
                      " wait unread; reason 0x%x lost\n", n, words[0]);
        return;
    }
    memset(s->reason_queue[n], 0, sizeof(s->reason_queue[n]));
    memcpy(s->reason_queue[n], words, count * sizeof(*words));
    s->reason_queue_token[n] = token;
    s->reason_queue_words[n] = count;
    s->reason_queue_len = n + 1;
    sgi_kona_reason_drain(s);
}

static void sgi_kona_post_reason(SGIKonaState *s, uint32_t token,
                                 uint32_t reason)
{
    sgi_kona_post_reason_words(s, token, &reason, 1);
}

/*
 * 0x1fc index: switch the current context out. The kernel's context
 * switch then sleeps until the pipe posts reason 0x101, and the driver's
 * geometry-engine interrupt handler wakes it and cancels its 90 s
 * deactivate timeout. When the context is restored later, the context
 * switch reads words 0 and 2 of its save area back (see
 * SGI_KONA_SAVE_AREA_MARKER), so they are written before the post. The
 * index names the context's entry in the kernel's table, not the save
 * area, so the save area is the one the last 0x1fd or 0x1f9 named.
 *
 * Approximated: hardware writes a real context record into the save area;
 * the model writes only the two words the kernel reads back, and keeps
 * every context's state, raster state included, in its own per-context
 * records, restored by key. The pipe's context image (0x1f9's header,
 * tagged records, a 0xedededed check word after each) holds no raster
 * record: the only raster-side state it saves is the back-end chip's
 * 89-word register shadow (tag 0x20), written back through the
 * raster-register path on restore (inferred). The model does not build
 * that image or that shadow, so its raster restore is its own, not the
 * pipe's. The raster boards have no interrupt lines, and the model
 * raises no interrupt for raster state either way.
 *
 * Hardware presumably posts once the outgoing context's queued work has
 * drained and its state is saved; the model
 * keeps no queued work, so it posts at once. The guest checks are the
 * minimal GLX program's first make-current going on to 0x1fd and 0x1f2
 * rather than the timeout, and a later swap back sending
 * 0x1f9 0x4b4f4e41 and 0x172 0.
 */
static void sgi_kona_context_deactivate(SGIKonaState *s, uint32_t index)
{
    unsigned kept;

    sgi_kona_x_read_invalidate(s);
    sgi_kona_gl_read_dma_refuse(s);
    trace_sgi_kona_context_deactivate(index);
    if (s->context_current == SGI_KONA_CONTEXT_NONE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: context deactivate %" PRIu32 " with no"
                      " context current; no save area written\n", index);
    } else {
        SGIKonaContext *c = &s->contexts[s->context_current];
        uint8_t word[4];
        uint64_t addr;
        bool mapped;

        if (sgi_kona_command_page(s, "save area", c->save_page,
                                  c->save_offset, &addr, &mapped)) {
            stl_be_p(word, SGI_KONA_SAVE_AREA_MARKER);
            sgi_kona_host_write(s, addr, word, sizeof(word));
            stl_be_p(word, SGI_KONA_SAVE_AREA_TOKEN);
            sgi_kona_host_write(s, addr + 8, word, sizeof(word));
            trace_sgi_kona_context_save(s->context_current, addr,
                                        SGI_KONA_SAVE_AREA_MARKER,
                                        SGI_KONA_SAVE_AREA_TOKEN);
        }
    }
    /* A suspended run's uploads are kept for its context. */
    kept = s->suspended_owner != SGI_KONA_CONTEXT_NONE ?
           sgi_kona_gl_run_uploads(s) : 0;
    if (!(kept & SGI_KONA_RUN_TEXTURE) && s->texture_upload.draw_pixels) {
        sgi_kona_texture_upload_clear(s);
    }
    if (!(kept & SGI_KONA_RUN_IMAGE) && s->gl_image_upload.count) {
        s->gl_image_upload.refused = true;
    }
    if (!(kept & SGI_KONA_RUN_BITMAP) && s->gl_bitmap_upload.count) {
        s->gl_bitmap_upload.refused = true;
    }
    if (!(kept & SGI_KONA_RUN_MAP)) {
        s->gl_map_upload.refused = true;
    }
    s->texture_retire_pending = false;
    s->context_current = SGI_KONA_CONTEXT_NONE;
    sgi_kona_texture_set_owner(&s->textures, 0);
    s->context_page_pending = false;
    sgi_kona_post_reason(s, SGI_KONA_CMD_CONTEXT_DEACTIVATE,
                         SGI_KONA_REASON_CONTEXT_DEACTIVATED);
}

/*
 * Configurations.
 *
 * With "product" set, the pipe presents one InfiniteReality configuration
 * whole: the board set and the inventory class the IRIX 6.5 driver
 * derives at attach, the raster managers, texture memory and display
 * channels its 0x1e9 answer reports, and an identity record for every
 * board in that answer. Without it the pipe answers as it always has,
 * which matches no real configuration (see sgi_kona_answer_config()).
 *
 * The products are named as IRIX's hinv names an Onyx2 pipe. The driver's
 * attach routine (in the 6.5.30 kernel graphics driver) reads two words through
 * the indirect port at +0x8003c0/+0x8003d0: after selecting 0x8800, a low
 * two bits of 1 makes the pipe an InfiniteReality3 (board set 5) and 2 an
 * InfiniteReality4 (board set 6); after selecting 0xb00, bit 10 makes it a
 * Reality (board set 2). Otherwise it looks for "GE16-" in the geometry
 * board's one-wire text: present, InfiniteReality2E (board set 4); absent,
 * InfiniteReality2 (board set 1). It records that class for hinv and
 * never reports the InfiniteReality name on this machine. All of that is
 * established by the disassembly; the indirect words' meaning on the pipe
 * is not. SGI's own answer (Usenet, comp.sys.sgi.graphics, 16 November
 * 1998) matches: InfiniteReality for Onyx2 prints InfiniteReality2, and
 * InfiniteReality2 for Onyx2 prints InfiniteReality2E.
 *
 * Every board below is one a real capture or owner's list names, with the
 * part, name and revision recorded there; a field no source gives is left
 * blank rather than chosen. The chip identity words gfxinfo -v prints
 * (ge:, rm0:, dg:) are the same on all three captured pipes and are
 * repeated for each configuration. The two words that differ between
 * captures, the board word gfxinfo prints after "brd:" and the one after
 * the raster managers, follow the geometry board here: that the first
 * does is inferred from two GE16-4 pipes sharing it, and the second is
 * approximated, as the two GE16-4 pipes differ there.
 */
typedef struct SGIKonaBoardSpec {
    const char *name;
    const char *part;
    const char *revision;
} SGIKonaBoardSpec;

typedef struct SGIKonaGeometry {
    const char *id;
    SGIKonaBoardSpec board;
    uint32_t engines;
    uint32_t occupancy;
    uint32_t board_word;
    uint32_t trailing_word;
} SGIKonaGeometry;

typedef struct SGIKonaRaster {
    const char *id;
    SGIKonaBoardSpec board;
    uint32_t board_word;
} SGIKonaRaster;

typedef struct SGIKonaTexture {
    uint32_t mb;
    uint32_t code;
    SGIKonaBoardSpec board;
} SGIKonaTexture;

typedef struct SGIKonaOption {
    const char *id;
    SGIKonaBoardSpec board;
    uint32_t code;
} SGIKonaOption;

typedef struct SGIKonaDisplay {
    const char *id;
    SGIKonaBoardSpec board;
    uint32_t channels;
    uint32_t external_gamma;
    const SGIKonaOption *option;
} SGIKonaDisplay;

#define SGI_KONA_PRODUCT_CHOICES 2

typedef struct SGIKonaProduct {
    const char *id;
    const char *refused;
    uint32_t board_set;
    uint32_t inventory_select;
    const SGIKonaGeometry *geometry[SGI_KONA_PRODUCT_CHOICES];
    const SGIKonaRaster *raster;
    const SGIKonaTexture *texture[SGI_KONA_PRODUCT_CHOICES];
} SGIKonaProduct;

/*
 * The captures are hinv, gfxinfo and board lists from real systems: an
 * Onyx2 deskside InfiniteReality2 (web:irixnet-post-32192), an Onyx2 rack
 * InfiniteReality3 (web:nekonomicon-16285), and an owner's board list and
 * gfxinfo for a two-GE InfiniteReality2.
 *
 * GE14-4: the deskside capture's board and hinv revision. GE14-2: the
 * two-GE owner's list (030-1051-002 rev K) and gfxinfo (2 GEs, occupancy
 * 0x03); its board words are the deskside's GE14-4 ones, approximated.
 * GE16-4: the rack capture's board, hinv revision and words.
 */
static const SGIKonaGeometry sgi_kona_ge14_2 = {
    "ge14-2", { "GE14-2", "030-1051-002", "K" }, 2, 0x3,
    0x80160a04, 0x51bf1002,
};
static const SGIKonaGeometry sgi_kona_ge14_4 = {
    "ge14-4", { "GE14-4", "030-1129-002", "F" }, 4, 0xf,
    0x80160a04, 0x51bf1002,
};
static const SGIKonaGeometry sgi_kona_ge16_4 = {
    "ge16-4", { "GE16-4", "030-1398-001", "E" }, 4, 0xf,
    0x80f61806, 0xf9311002,
};

/*
 * RM7: the deskside capture's part and board word, the two-GE owner's name and
 * revision. RM9: the rack capture's board (the stock configuration tool names
 * it RM9, revision E) and board word. gfxinfo names an RM9 only by that name,
 * right-justified in the record.
 */
static const SGIKonaRaster sgi_kona_rm7 = {
    "rm7", { "RM7", "030-1054-001", "G" }, 0x3020c06,
};
static const SGIKonaRaster sgi_kona_rm9 = {
    "rm9", { "RM9", "030-1402-001", "E" }, 0x3041606,
};

/*
 * Texture boards and the size code gfxinfo turns into its "Texture
 * Memory" text (2 is 64MB, 4 is 256MB). TM7 64 MB: the deskside
 * capture's part, the two-GE owner's name and revision; an RM9-64 is
 * "030-1402-00x + 030-1053-00x" (comp.sys.sgi.marketplace, 2 August 2004),
 * so the RM9's 64 MB board is taken to be the same TM7, inferred. 256 MB:
 * the rack capture's 030-1588-001, whose name and revision no source
 * gives.
 */
static const SGIKonaTexture sgi_kona_tm7_64 = {
    64, 2, { "TM7", "030-1053-001", "F" },
};
static const SGIKonaTexture sgi_kona_tm_256 = {
    256, 4, { "", "030-1588-001", "" },
};

/*
 * The rack capture's GVO option, revision B in the stock
 * configuration tool, and the code gfxinfo
 * prints "GVO option detected" for. The option's identity only: the model
 * has no GVO video output.
 */
static const SGIKonaOption sgi_kona_gvo = {
    "gvo", { "GVO", "030-1184-002", "B" }, 6,
};

/*
 * Display boards, by the "display" type: the board names a
 * community-compiled list of SGI PCA numbers gives, each presenting the
 * parts a capture shows for it.
 * dg5-8, 030-1087-001: the deskside capture's board, whose gfxinfo reports
 * eight channels and 64K of external gamma; the record's name and revision
 * are not given. dg5-2, 030-1055-001: the two-GE owner's DG5-2 (owner list,
 * revision unreadable), two channels. dg5-2-gvo: the rack capture's DG5-2
 * base for an option board, 030-1242-001 revision D, with its GVO record; the
 * model has no GVO output, so the machine catalogue does not offer it.
 */
static const SGIKonaDisplay sgi_kona_displays[] = {
    { "dg5-8", { "", "030-1087-001", "" }, 8, 1, NULL },
    { "dg5-2", { "DG5-2", "030-1055-001", "" }, 2, 0, NULL },
    { "dg5-2-gvo", { "DG5-2", "030-1242-001", "D" }, 2, 0, &sgi_kona_gvo },
};

static const SGIKonaProduct sgi_kona_products[] = {
    { .id = "ir", .refused = "IRIX 6.5 never names an Onyx2 pipe "
      "InfiniteReality: it calls the GE14 and RM7 pipe InfiniteReality2, "
      "product ir2; InfiniteReality is the Onyx name, a machine QEMU does "
      "not model" },
    { .id = "ir2", .board_set = 1, .inventory_select = 0,
      .geometry = { &sgi_kona_ge14_4, &sgi_kona_ge14_2 },
      .raster = &sgi_kona_rm7, .texture = { &sgi_kona_tm7_64 } },
    { .id = "ir2e", .board_set = 4, .inventory_select = 0,
      .geometry = { &sgi_kona_ge16_4 },
      .raster = &sgi_kona_rm9, .texture = { &sgi_kona_tm7_64 } },
    { .id = "ir3", .board_set = 5, .inventory_select = 1,
      .geometry = { &sgi_kona_ge16_4 },
      .raster = &sgi_kona_rm9, .texture = { &sgi_kona_tm_256 } },
    { .id = "reality", .refused = "no capture gives the Reality pipe's "
      "board parts or chip words, so its configuration cannot be "
      "presented" },
    { .id = "ir4", .refused = "no Onyx2 InfiniteReality4 has been captured: "
      "the only captured pipe is an Onyx 350's, whose Ktown2 host board "
      "this model cannot attach, so the Onyx2 pipe's host and display "
      "boards are unknown" },
};

/* The indirect-port selectors the driver's attach reads its class from. */
#define SGI_KONA_SELECT_PRODUCT 0x8800
#define SGI_KONA_SELECT_REDUCED 0xb00

/*
 * Instances for the synthetic serials of the boards the 0x1e9 answer
 * names; the geometry board and Ktown keep 0 and 1.
 */
#define SGI_KONA_SERIAL_RM(i)   (2 + (i))
/* A pipe's first instance: sixteen per slot, so slots never share one. */
#define SGI_KONA_SERIAL_BASE(s) (16 * (unsigned)(s)->identity_index)
#define SGI_KONA_SERIAL_TM(i)   (6 + (i))
#define SGI_KONA_SERIAL_DG      10
#define SGI_KONA_SERIAL_DGOPT   11

/* The Ktown record a rack pipe carries; a deskside pipe has none. */
#define SGI_KONA_KTOWN_PART "030-1067-001"

static bool sgi_kona_has_ktown(SGIXioBoardState *board)
{
    strList *p;

    for (p = board->other_identity_parts; p; p = p->next) {
        if (g_str_equal(p->value, SGI_KONA_KTOWN_PART)) {
            return true;
        }
    }
    return false;
}

static bool sgi_kona_unset(const char *value)
{
    return !value || !*value;
}

/*
 * The pixel depth a configured pipe reports at configuration block +0x8c:
 * gfxinfo prints it ("Small" or "Medium pixel depth") and the X server picks
 * its visual table from it, the Medium table adding 12-bit RGB multisample
 * visuals and double-buffered visuals with multisampling, an accumulation
 * buffer and a depth buffer together. On a real pipe the depth comes with
 * the video format combination loaded; the X server accepts a depth for a
 * combination only when the raster managers' pixel memory covers it, at
 * raster managers x 20,971,520 / (height x width rounded up to 160) of at
 * least 8 for Small and 16 for Medium (established from the IRIX X server's
 * check). The managed area at reset, 1280x1024, meets Medium with any
 * raster-manager count, so the property needs only the count to hold; a
 * combination's larger area must fit the same budget
 * (sgi_kona_managed_fits()).
 * Large and Extra-Large are refused: their visual tables add buffer
 * configurations the model has not been checked against.
 */
static bool sgi_kona_pixel_depth(SGIKonaState *s, SGIKonaConfig *c,
                                 Error **errp)
{
    static const char *const names[] = { "small", "medium" };
    static const uint32_t budget[] = { 8, 16 };
    uint64_t area = (uint64_t)SGI_KONA_FB_HEIGHT *
                    QEMU_ALIGN_UP(SGI_KONA_FB_WIDTH, 160);

    c->pixel_depth = 0;
    if (sgi_kona_unset(s->pixel_depth)) {
        return true;
    }
    for (c->pixel_depth = 0; c->pixel_depth < ARRAY_SIZE(names);
         c->pixel_depth++) {
        if (g_str_equal(s->pixel_depth, names[c->pixel_depth])) {
            break;
        }
    }
    if (c->pixel_depth == ARRAY_SIZE(names)) {
        error_setg(errp, "sgi-kona: pixel-depth '%s' is not modelled; the "
                   "model presents small or medium", s->pixel_depth);
        return false;
    }
    if ((uint64_t)c->rm_count * 20971520 / area < budget[c->pixel_depth]) {
        error_setg(errp, "sgi-kona: pixel-depth %s needs more pixel memory "
                   "than %u raster managers hold at %ux%u", s->pixel_depth,
                   c->rm_count, SGI_KONA_FB_WIDTH, SGI_KONA_FB_HEIGHT);
        return false;
    }
    return true;
}

/*
 * Check the configuration properties and resolve them into s->config.
 * Every value a product does not offer is refused with the reason, so a
 * configuration whose fields have no source is never presented.
 */
static bool sgi_kona_configure(SGIKonaState *s, Error **errp)
{
    SGIKonaConfig *c = &s->config;
    bool ktown = s->ktown2 || sgi_kona_has_ktown(SGI_XIO_BOARD(s));
    const char *dg_part;
    size_t i;

    memset(c, 0, sizeof(*c));
    if (sgi_kona_unset(s->product)) {
        if (!sgi_kona_unset(s->geometry) || !sgi_kona_unset(s->rm_type) ||
            s->rm_count || s->texture_mb || s->channels ||
            !sgi_kona_unset(s->display) || !sgi_kona_unset(s->pixel_depth)) {
            error_setg(errp, "sgi-kona: set product to configure the pipe");
            return false;
        }
        return true;
    }
    for (i = 0; i < ARRAY_SIZE(sgi_kona_products); i++) {
        if (g_str_equal(s->product, sgi_kona_products[i].id)) {
            c->product = &sgi_kona_products[i];
        }
    }
    if (!c->product) {
        error_setg(errp, "sgi-kona: unknown product '%s'; the products are "
                   "ir2, ir2e and ir3", s->product);
        return false;
    }
    if (c->product->refused) {
        error_setg(errp, "sgi-kona: product %s is refused: %s",
                   c->product->id, c->product->refused);
        c->product = NULL;
        return false;
    }

    for (i = 0; i < SGI_KONA_PRODUCT_CHOICES; i++) {
        const SGIKonaGeometry *g = c->product->geometry[i];

        if (g && (sgi_kona_unset(s->geometry) ? i == 0 :
                  g_str_equal(s->geometry, g->id))) {
            c->geometry = g;
        }
    }
    if (!c->geometry) {
        error_setg(errp, "sgi-kona: product %s has no geometry board '%s'; "
                   "it takes %s%s%s", c->product->id, s->geometry,
                   c->product->geometry[0]->id,
                   c->product->geometry[1] ? " or " : "",
                   c->product->geometry[1] ?
                   c->product->geometry[1]->id : "");
        goto refused;
    }

    c->raster = c->product->raster;
    if (!sgi_kona_unset(s->rm_type) &&
        !g_str_equal(s->rm_type, c->raster->id)) {
        error_setg(errp, "sgi-kona: product %s has %s raster managers, "
                   "not '%s'", c->product->id, c->raster->id, s->rm_type);
        goto refused;
    }

    c->rm_count = s->rm_count ? s->rm_count : 1;
    if (c->rm_count != 1 && c->rm_count != 2 && c->rm_count != 4) {
        error_setg(errp, "sgi-kona: rm-count %u: a pipe has 1, 2 or 4 "
                   "raster managers", s->rm_count);
        goto refused;
    }
    if (c->rm_count == 4 && !ktown) {
        error_setg(errp, "sgi-kona: rm-count 4: a pipe with no Ktown is an "
                   "Onyx2 deskside pipe, which holds at most two raster "
                   "managers");
        goto refused;
    }

    for (i = 0; i < SGI_KONA_PRODUCT_CHOICES; i++) {
        const SGIKonaTexture *t = c->product->texture[i];

        if (t && (s->texture_mb ? t->mb == s->texture_mb : i == 0)) {
            c->texture = t;
        }
    }
    if (!c->texture) {
        error_setg(errp, "sgi-kona: product %s with %u MB of texture per "
                   "raster manager has no known texture board; it takes "
                   "%u MB", c->product->id, s->texture_mb,
                   c->product->texture[0]->mb);
        goto refused;
    }

    for (i = 0; i < ARRAY_SIZE(sgi_kona_displays); i++) {
        const SGIKonaDisplay *d = &sgi_kona_displays[i];

        if (g_str_equal(d->id, sgi_kona_unset(s->display) ?
                        (ktown ? "dg5-2" : "dg5-8") : s->display)) {
            c->display = d;
        }
    }
    if (!c->display) {
        error_setg(errp, "sgi-kona: display '%s' is not a display board the "
                   "model presents: dg5-8, dg5-2 or dg5-2-gvo", s->display);
        goto refused;
    }
    c->channels = c->display->channels;
    dg_part = c->display->board.part;
    if (s->channels && s->channels != c->display->channels) {
        error_setg(errp, "sgi-kona: channels %u: display generator %s "
                   "reports %u", s->channels, dg_part,
                   c->display->channels);
        goto refused;
    }

    if (!sgi_kona_pixel_depth(s, c, errp)) {
        goto refused;
    }

    c->option = c->display->option;
    sgi_kona_texture_set_memory(&s->textures, (uint64_t)c->texture->mb * MiB);
    return true;

refused:
    memset(c, 0, sizeof(*c));
    return false;
}

/*
 * A configured pipe's geometry board and Ktown carry their real names and
 * revisions, the geometry board its configured part too: the driver reads
 * "GE16-" in that text to tell an InfiniteReality2E from an
 * InfiniteReality2, and hinv prints the record.
 */
static void sgi_kona_identity_record(SGIXioBoardState *board,
                                     const SGIXioBoardIdentityPart *part,
                                     SGIBridgeBoardRecord *record,
                                     uint64_t *serial)
{
    SGIKonaState *s = SGI_KONA(board);
    const SGIKonaConfig *c = &s->config;

    /*
     * Each pipe slot's records take their own serials, the barcode
     * following the serial: the geometry board the machine's pipe record
     * serial, the Ktown its slot's (or the slot it is shared with) second
     * instance.
     */
    if (part->record == &sgi_kona_geometry_record) {
        *serial = s->onewire_serial ? s->onewire_serial :
                  SGI_SYNTHETIC_ONEWIRE_SERIAL('G', 16 * s->identity_index);
        sgi_synthetic_barcode(s->geometry_barcode, 'G', *serial);
        record->barcode = s->geometry_barcode;
    } else if (part->record == &sgi_kona_ktown_record) {
        unsigned slot = s->shared_identity_index == SGI_KONA_OWN_IDENTITY ?
                        s->identity_index : s->shared_identity_index;

        *serial = SGI_SYNTHETIC_ONEWIRE_SERIAL('G', 16 * slot + 1);
        sgi_synthetic_barcode(s->ktown_barcode, 'G', *serial);
        record->barcode = s->ktown_barcode;
    }
    if (!c->product) {
        return;
    }
    if (part->record == &sgi_kona_geometry_record) {
        record->name = c->geometry->board.name;
        record->part = c->geometry->board.part;
        record->revision = c->geometry->board.revision;
    } else if (part->record == &sgi_kona_ktown_record) {
        /*
         * The rack capture's Ktown: hinv and the stock configuration tool
         * name it KTOWN,
         * rev F.
         */
        record->name = "KTOWN";
        record->revision = "F";
    }
}

/*
 * The board configuration the pipe answers 0x1e9 with. gfxinit hands the
 * 0x778 bytes from return page + 8 to the kernel, which copies them over
 * its board information; every later process reads them back from there.
 * Offsets are from page + 8.
 *
 * An unconfigured pipe (no "product") answers with the seven fields of
 * sgi_kona_config_fields[] and the pool descriptors below, every other
 * byte zero: the model's own choice for one pipe driving one 1280x1024
 * channel, which matches no real configuration (gfxinfo reports board set
 * 0's "0 RM6 board (of 0)"). A configured pipe answers with
 * sgi_kona_config_block(). Each field is written as a 32-bit big-endian
 * word, which the readers' widths do not establish. The guest test that
 * tells a working block from a wrong one is gfxinfo or gfxinit -v
 * reporting the configuration, with no "return area did not go valid".
 */
#define SGI_KONA_CONFIG_OFFSET 8
#define SGI_KONA_CONFIG_SIZE   0x778

/*
 * The four pipe-memory pool descriptors at block offset 0x90 + 0x1c *
 * class. The driver's pipe-memory setup points class i's pool at them
 * and its allocator checks every request against them; nothing in the
 * driver writes them afterwards. The classes are the co-processor, the
 * shared display lists, the geometry engines and texture.
 *
 * Approximated: no source gives a descriptor's raw words, so these are the
 * model's own choice inside the kernel's limits. The granule must be
 * non-zero (the first allocation of a class divides by it) and divide every
 * alignment the library requests, and alignment / granule must stay below
 * 256, which 0x400 against the largest alignment 0x4000 keeps. Class 2 needs
 * a largest key of at least 9 and a shared flag of 0, or the library's class
 * 2 allocation fails silently and make-current carries on without it. The
 * capacities follow the public figures: about 15 MB of the host interface
 * processor's SDRAM for display lists, and 64 MB of texture memory on each
 * raster manager in the InfiniteReality2 capture; class 0's 256 KiB and
 * class 2's 1 MiB are the model's. The copy count and copy-down flag matter
 * only under pool pressure, which the model does not emulate. The guest test
 * is the minimal GLX program: its first make-current's private request
 * 0x271a returns 0 instead of a kernel divide fault, and the trace shows
 * 0x1e4 2 9 0 1 then 0x172 0.
 */
#define SGI_KONA_POOL_OFFSET  0x90
#define SGI_KONA_POOL_CLASSES 4
#define SGI_KONA_POOL_GEOMETRY 2
#define SGI_KONA_POOL_TEXTURE  3

typedef struct SGIKonaPoolDescriptor {
    uint32_t max_key;    /* largest key accepted */
    uint32_t max_align;  /* largest alignment accepted, bytes */
    uint32_t copies;     /* host copies with request flag bit 0 set */
    uint32_t shared;     /* regions live in the context's share object */
    uint32_t copy_down;  /* compact the pool before evicting */
    uint32_t granule;    /* bytes */
    uint32_t capacity;   /* granules */
} SGIKonaPoolDescriptor;

QEMU_BUILD_BUG_ON(sizeof(SGIKonaPoolDescriptor) != 0x1c);

static const SGIKonaPoolDescriptor sgi_kona_pools[SGI_KONA_POOL_CLASSES] = {
    /* co-processor: 256 KiB */
    { .max_key = 0, .max_align = 0x4000, .copies = 1, .shared = 0,
      .copy_down = 1, .granule = 0x400, .capacity = 0x100 },
    /* display lists: 15 MiB */
    { .max_key = 0, .max_align = 0x4000, .copies = 1, .shared = 1,
      .copy_down = 1, .granule = 0x400, .capacity = 0x3c00 },
    /* geometry engines, keys 0 to 10, one copy per engine: 1 MiB */
    { .max_key = 10, .max_align = 0x4000, .copies = 4, .shared = 0,
      .copy_down = 1, .granule = 0x400, .capacity = 0x400 },
    /* texture: 64 MiB */
    { .max_key = 0, .max_align = 0x4000, .copies = 1, .shared = 0,
      .copy_down = 1, .granule = 0x400,
      .capacity = SGI_KONA_TEXTURE_MEMORY / 0x400 },
};

static const struct {
    uint32_t offset;
    uint32_t value;
} sgi_kona_config_fields[] = {
    { 0x00, 0 },                   /* board set, 0 to 6 */
    { 0x14, 1 },                   /* display channel count */
    { 0x58, 0 },                   /* count of 0x8c-byte records */
    { 0x5c, 0xf },                 /* occupancy mask */
    { 0x84, SGI_KONA_FB_WIDTH },   /* width, the managed area's */
    { 0x88, SGI_KONA_FB_HEIGHT },  /* height, the managed area's */
    { 0x8c, 0 },                   /* pixel-depth code, 0 to 3 */
};

/*
 * A configured pipe's answer, every field at the offset gfxinit's report
 * (6.5.30 gfxinit's board report and the routines it calls) reads it from;
 * see the InfiniteReality command grammar datasheet's configuration
 * request section for the readers. Fields no reader is known for stay
 * zero, as do the geometry board's and Ktown's identity records, which
 * the driver keeps from its own one-wire reading whatever the pipe sends.
 * The size is the managed area's, 1280x1024 until a combination's
 * SetupFrameBuffer changes it (sgi_kona_managed_resize()); the pixel depth
 * is Small unless "pixel-depth" asks for Medium (sgi_kona_pixel_depth()).
 *
 * The display generator's channel count goes both at +0x14, which gfxinfo
 * prints, and at +0x44, which the combination checker in setmon, ircombine
 * and the X server, reading the kernel's board-information copy, compares a
 * combination's enabled channels against; with 0 there every combination is
 * refused as "too many channels". That +0x44 is the display generator's own
 * count, and +0x48 to +0x50, which the checker also reads and the model
 * leaves zero, its revision and an option board's channel type and
 * revision, is inferred from that use. Every channel is driven: see
 * "Display channels" above.
 */
#define SGI_KONA_CFG_BOARD_SET      0x00
#define SGI_KONA_CFG_ENGINES        0x04
#define SGI_KONA_CFG_RMS            0x08
#define SGI_KONA_CFG_FIFO_RAM       0x0c
#define SGI_KONA_CFG_CHANNELS       0x14
#define SGI_KONA_CFG_VIDEO_OPTION   0x18
#define SGI_KONA_CFG_GAMMA          0x20
#define SGI_KONA_CFG_RM_PRESENT(i)  (0x24 + 8 * (i))
#define SGI_KONA_CFG_RM_TEXTURE(i)  (0x28 + 8 * (i))
#define SGI_KONA_CFG_DG_CHANNEL_COUNT 0x44
#define SGI_KONA_CFG_ACTIVE_ENGINES 0x54
#define SGI_KONA_CFG_ACTIVE_RMS     0x58
#define SGI_KONA_CFG_OCCUPANCY      0x5c
#define SGI_KONA_CFG_WIDTH          0x84
#define SGI_KONA_CFG_HEIGHT         0x88
#define SGI_KONA_CFG_PIXEL_DEPTH    0x8c
#define SGI_KONA_CFG_BOARD_WORD     0x100
#define SGI_KONA_CFG_GE_WORDS       0x104
#define SGI_KONA_CFG_RM_CHIPS(i)    (0x114 + 0x8c * (i))
#define SGI_KONA_CFG_TRAILING_WORD  0x344
#define SGI_KONA_CFG_DG_WORDS       0x348
#define SGI_KONA_CFG_DG_CHANNELS    0x35c
#define SGI_KONA_CFG_NIC_RM(i)      (0x3e8 + 0x4c * (i))
#define SGI_KONA_CFG_NIC_TM(i)      (0x564 + 0x4c * (i))
#define SGI_KONA_CFG_NIC_DG         0x6e0
#define SGI_KONA_CFG_NIC_DGOPT      0x72c

/* "4MB external" BEF RAM, as every captured pipe reports. */
#define SGI_KONA_FIFO_RAM_4MB_EXTERNAL 1

/*
 * Write `text` right-justified in a `width`-byte field of spaces, as the
 * driver stores the one-wire text it parses and as gfxinfo's RM9 test
 * (the last three bytes of the name field) needs.
 */
static void sgi_kona_put_text(uint8_t *field, size_t width, const char *text)
{
    size_t len = MIN(strlen(text), width);

    memset(field, ' ', width);
    memcpy(field + width - len, text, len);
}

/*
 * One 0x4c-byte identity record in the layout the driver's one-wire reader
 * builds and gfxinit's one-wire report prints: flags (0, both pages
 * present), the six-byte one-wire number, family 0x0b, a good CRC, page
 * format 1, then serial, part, revision, the three flag bytes and the name.
 * The serial and number are the model's marked synthetic ones; the flag
 * bytes are unprogrammed, like the model's one-wire records.
 */
static void sgi_kona_put_record(uint8_t *rec, const SGIKonaBoardSpec *b,
                                unsigned instance)
{
    uint64_t serial = SGI_SYNTHETIC_ONEWIRE_SERIAL('G', instance);
    char barcode[SGI_SYNTHETIC_BARCODE_SIZE];

    sgi_synthetic_barcode(barcode, 'G', serial);
    for (int i = 0; i < 6; i++) {
        rec[0x04 + i] = serial >> (40 - 8 * i);
    }
    rec[0x0a] = 0x0b;
    rec[0x0b] = 1;
    rec[0x0c] = 1;
    sgi_kona_put_text(rec + 0x0d, 10, barcode);
    sgi_kona_put_text(rec + 0x17, 19, b->part);
    memset(rec + 0x2c, ' ', 6);
    sgi_kona_put_text(rec + 0x32, 4, b->revision);
    memset(rec + 0x36, 0xff, 6);
    sgi_kona_put_text(rec + 0x3c, 14, b->name);
}

/* Fill `count` words from `at` with `word`. */
static void sgi_kona_put_words(uint8_t *at, unsigned count, uint32_t word)
{
    for (unsigned i = 0; i < count; i++) {
        stl_be_p(at + 4 * i, word);
    }
}

static void sgi_kona_config_block(const SGIKonaState *s, unsigned base,
                                  uint8_t *block)
{
    const SGIKonaConfig *c = &s->config;
    static const uint32_t ge_words[] = {
        0, 0x14832057, 0x24731057, 0x14231057,
    };
    uint32_t channels = c->display->channels;

    stl_be_p(block + SGI_KONA_CFG_BOARD_SET, c->product->board_set);
    stl_be_p(block + SGI_KONA_CFG_ENGINES, c->geometry->engines);
    stl_be_p(block + SGI_KONA_CFG_RMS, c->rm_count);
    stl_be_p(block + SGI_KONA_CFG_FIFO_RAM, SGI_KONA_FIFO_RAM_4MB_EXTERNAL);
    stl_be_p(block + SGI_KONA_CFG_CHANNELS, channels);
    stl_be_p(block + SGI_KONA_CFG_VIDEO_OPTION,
             c->option ? c->option->code : 0);
    stl_be_p(block + SGI_KONA_CFG_GAMMA, c->display->external_gamma);
    stl_be_p(block + SGI_KONA_CFG_DG_CHANNEL_COUNT, channels);
    stl_be_p(block + SGI_KONA_CFG_ACTIVE_ENGINES, c->geometry->engines);
    stl_be_p(block + SGI_KONA_CFG_ACTIVE_RMS, c->rm_count);
    stl_be_p(block + SGI_KONA_CFG_OCCUPANCY, c->geometry->occupancy);
    stl_be_p(block + SGI_KONA_CFG_WIDTH, s->cfg_width);
    stl_be_p(block + SGI_KONA_CFG_HEIGHT, s->cfg_height);
    stl_be_p(block + SGI_KONA_CFG_PIXEL_DEPTH, c->pixel_depth);

    stl_be_p(block + SGI_KONA_CFG_BOARD_WORD, c->geometry->board_word);
    for (unsigned i = 0; i < ARRAY_SIZE(ge_words); i++) {
        stl_be_p(block + SGI_KONA_CFG_GE_WORDS + 4 * i, ge_words[i]);
    }
    for (unsigned i = 0; i < c->rm_count; i++) {
        uint8_t *chips = block + SGI_KONA_CFG_RM_CHIPS(i);

        stl_be_p(block + SGI_KONA_CFG_RM_PRESENT(i), 1);
        stl_be_p(block + SGI_KONA_CFG_RM_TEXTURE(i), c->texture->code);
        stl_be_p(chips, c->raster->board_word);
        stl_be_p(chips + 0x04, 0x15032057);
        stl_be_p(chips + 0x08, 0x15431057);
        sgi_kona_put_words(chips + 0x0c, 4, 0x24631057);
        sgi_kona_put_words(chips + 0x1c, 8, 0x24d31057);
        sgi_kona_put_words(chips + 0x3c, 20, 0x54938057);
        sgi_kona_put_record(block + SGI_KONA_CFG_NIC_RM(i),
                            &c->raster->board, base + SGI_KONA_SERIAL_RM(i));
        sgi_kona_put_record(block + SGI_KONA_CFG_NIC_TM(i),
                            &c->texture->board,
                            base + SGI_KONA_SERIAL_TM(i));
    }
    stl_be_p(block + SGI_KONA_CFG_TRAILING_WORD, c->geometry->trailing_word);
    stl_be_p(block + SGI_KONA_CFG_DG_WORDS, 0x05532057);
    sgi_kona_put_words(block + SGI_KONA_CFG_DG_WORDS + 4, 4, 0x15838057);
    sgi_kona_put_words(block + SGI_KONA_CFG_DG_CHANNELS, channels,
                       0x15631057);
    sgi_kona_put_record(block + SGI_KONA_CFG_NIC_DG, &c->display->board,
                        base + SGI_KONA_SERIAL_DG);
    if (c->option) {
        sgi_kona_put_record(block + SGI_KONA_CFG_NIC_DGOPT, &c->option->board,
                            base + SGI_KONA_SERIAL_DGOPT);
    }
}

/*
 * Post a return-page answer: `len` bytes at `offset`, none when `len` is
 * 0, then a non-zero word 0, the only thing the waiting process polls. The
 * order is inferred: gfxinit hands the page to the kernel as soon as word
 * 0 goes non-zero, so the data must already be there. Approximated like the
 * reason post: the model answers at once rather than after queued work.
 * With no return page registered it logs and writes nothing.
 */
static bool sgi_kona_post_return(SGIKonaState *s, uint32_t token,
                                 uint32_t offset, const void *data,
                                 uint32_t len)
{
    uint8_t ready[4];

    if (!s->return_page_valid) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%x with no return page registered;"
                      " nothing posted\n", token);
        return false;
    }
    if (len && !sgi_kona_host_write(s, s->return_page + offset, data, len)) {
        return false;
    }
    stl_be_p(ready, 1);
    if (sgi_kona_host_write(s, s->return_page, ready, sizeof(ready))) {
        trace_sgi_kona_return_post(token, s->return_page);
        return true;
    }
    return false;
}

/* The block sgi_kona_answer_config() posts, into @block. */
static void sgi_kona_build_config(SGIKonaState *s, uint8_t *block)
{
    const SGIKonaConfig *c = &s->config;

    memset(block, 0, SGI_KONA_CONFIG_SIZE);

    if (c->product) {
        sgi_kona_config_block(s, SGI_KONA_SERIAL_BASE(s), block);
    } else {
        for (size_t i = 0; i < ARRAY_SIZE(sgi_kona_config_fields); i++) {
            stl_be_p(block + sgi_kona_config_fields[i].offset,
                     sgi_kona_config_fields[i].value);
        }
        stl_be_p(block + SGI_KONA_CFG_WIDTH, s->cfg_width);
        stl_be_p(block + SGI_KONA_CFG_HEIGHT, s->cfg_height);
    }
    for (size_t i = 0; i < ARRAY_SIZE(sgi_kona_pools); i++) {
        SGIKonaPoolDescriptor pool = sgi_kona_pools[i];
        uint8_t *p = block + SGI_KONA_POOL_OFFSET + sizeof(pool) * i;

        /*
         * A configured pipe sizes the geometry-engine class by its
         * engines and the texture class by one raster manager's texture
         * memory, which every raster manager holds a copy of.
         */
        if (c->product && i == SGI_KONA_POOL_GEOMETRY) {
            pool.copies = c->geometry->engines;
        }
        if (c->product && i == SGI_KONA_POOL_TEXTURE) {
            pool.capacity = c->texture->mb * MiB / pool.granule;
        }
        stl_be_p(p + 0x00, pool.max_key);
        stl_be_p(p + 0x04, pool.max_align);
        stl_be_p(p + 0x08, pool.copies);
        stl_be_p(p + 0x0c, pool.shared);
        stl_be_p(p + 0x10, pool.copy_down);
        stl_be_p(p + 0x14, pool.granule);
        stl_be_p(p + 0x18, pool.capacity);
    }
}

static void sgi_kona_answer_config(SGIKonaState *s)
{
    uint8_t block[SGI_KONA_CONFIG_SIZE];

    sgi_kona_build_config(s, block);
    sgi_kona_post_return(s, SGI_KONA_CMD_GET_CONFIG, SGI_KONA_CONFIG_OFFSET,
                         block, sizeof(block));
}

/*
 * qtest: the configuration block the pipe answers 0x1e9 with, as hex,
 * without the return page and DMA a guest sets up to fetch it.
 */
static void sgi_kona_test_get_config_block(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    uint8_t block[SGI_KONA_CONFIG_SIZE];
    g_autoptr(GString) hex = g_string_sized_new(2 * sizeof(block));

    sgi_kona_build_config(SGI_KONA(obj), block);
    for (size_t i = 0; i < sizeof(block); i++) {
        g_string_append_printf(hex, "%02x", block[i]);
    }
    visit_type_str(v, name, &hex->str, errp);
}

/*
 * The flash EEPROM operation, SGI_KONA_CMD_EEPROM ("The flash EEPROM operation,
 * 0x1ec, and the EEPROM programming tool" in the command grammar datasheet).
 * The operation number arrives as the command's one framed word; the words each
 * operation takes after it come here. Established from the pipe's ARM handler:
 * a write takes a type, an offset for the boot image only, a count and the
 * data; a read an offset and a byte length; op 3 the reply buffer; ops 4 and 5
 * one unused word; op 6 none; any other number nothing more.
 */
#define SGI_KONA_FLASH_OP_WRITE        1
#define SGI_KONA_FLASH_OP_READ         2
#define SGI_KONA_FLASH_OP_REPLY_BUFFER 3
#define SGI_KONA_FLASH_OP_CHECK        4
#define SGI_KONA_FLASH_OP_IDENTIFY     5
#define SGI_KONA_FLASH_OP_ERASE_COMBO  6

#define SGI_KONA_FLASH_TYPE_IMAGE  0
#define SGI_KONA_FLASH_TYPE_COMBO  1
#define SGI_KONA_FLASH_TYPE_HEADER 2

/* Words after the operation number, before any data. */
static const uint8_t sgi_kona_flash_op_words[] = {
    [SGI_KONA_FLASH_OP_WRITE] = 1,  /* the type; it decides the rest */
    [SGI_KONA_FLASH_OP_READ] = 2,
    [SGI_KONA_FLASH_OP_REPLY_BUFFER] = 3,
    [SGI_KONA_FLASH_OP_CHECK] = 1,
    [SGI_KONA_FLASH_OP_IDENTIFY] = 1,
};

/*
 * The starting image: zero, with the header state the PROM accepts, or
 * the "drive" image, which must be the flash's size.
 */
static bool sgi_kona_flash_init(SGIKonaState *s, Error **errp)
{
    int64_t length;

    sgi_kona_flash_fresh_image(s->flash);
    s->flash_written = false;
    if (!s->flash_blk) {
        return true;
    }
    length = blk_getlength(s->flash_blk);
    if (length < 0) {
        error_setg_errno(errp, -length, "sgi-kona: cannot read the size of "
                         "the flash image");
        return false;
    }
    if (length != SGI_KONA_FLASH_SIZE) {
        error_setg(errp, "sgi-kona: the flash image is %" PRId64 " bytes, "
                   "not the required %u", length, SGI_KONA_FLASH_SIZE);
        return false;
    }
    if (blk_set_perm(s->flash_blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_ALL, errp) < 0) {
        return false;
    }
    if (blk_pread(s->flash_blk, 0, SGI_KONA_FLASH_SIZE, s->flash, 0) < 0) {
        error_setg(errp, "sgi-kona: cannot read the flash image");
        return false;
    }
    /* The image may differ from the starting one: migration carries it. */
    s->flash_written = true;
    return true;
}

/*
 * Write flash bytes [offset, offset + len) through to the "drive" image.
 * A failed write is the host's, not the guest's: it is reported once and
 * the bytes stay in the flash, which from then on runs ahead of the image.
 */
static void sgi_kona_flash_sync(SGIKonaState *s, uint64_t offset,
                                uint64_t len)
{
    if (!s->flash_blk || offset >= SGI_KONA_FLASH_SIZE) {
        return;
    }
    len = MIN(len, SGI_KONA_FLASH_SIZE - offset);
    if (blk_pwrite(s->flash_blk, offset, len, &s->flash[offset], 0) < 0 &&
        !s->flash_sync_failed) {
        s->flash_sync_failed = true;
        error_report("sgi-kona: write to the flash image failed; later "
                     "writes may be lost from the image too");
    }
}

/*
 * A migrated flash reaches the destination's image only once the VM runs:
 * during incoming migration the image's node is not yet active, so
 * post_load only marks the write, as pflash_cfi01 defers its own. With an
 * image the source shares, the write puts back the same bytes.
 */
static void sgi_kona_flash_vm_change(void *opaque, bool running,
                                     RunState state)
{
    SGIKonaState *s = opaque;

    if (running && s->flash_sync_pending) {
        s->flash_sync_pending = false;
        sgi_kona_flash_sync(s, 0, SGI_KONA_FLASH_SIZE);
    }
}

/* The big-endian word at flash offset `offset`, or 0 past the end. */
static uint32_t sgi_kona_flash_load(SGIKonaState *s, uint32_t offset)
{
    if (offset > SGI_KONA_FLASH_SIZE - 4) {
        return 0;
    }
    return ldl_be_p(&s->flash[offset]);
}

/*
 * Program one word at the open write's address and advance it. Bytes past
 * the 512 KiB are dropped, and logged once per write. The address does not
 * wrap: what the ARM's own address arithmetic does there is unknown.
 */
static void sgi_kona_flash_store(SGIKonaState *s, uint32_t word)
{
    SGIKonaFlashOp *f = &s->flash_op;
    uint8_t bytes[4];

    stl_be_p(bytes, word);
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        uint64_t at = f->flash_addr + i;

        if (at < SGI_KONA_FLASH_SIZE) {
            s->flash[at] = bytes[i];
        } else if (!f->outside) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: EEPROM write reaches"
                          " flash offset 0x%" PRIx64 ", past its 512 KiB;"
                          " the bytes past it are dropped\n", at);
            f->outside = true;
        }
    }
    sgi_kona_flash_sync(s, f->flash_addr, sizeof(bytes));
    f->flash_addr += 4;
    s->flash_written = true;
}

/*
 * Post a reply: the word count, then the words, from word 1 of the reply
 * buffer, and word 0 last, as the ARM's reply routines write it.
 * Approximated: at once, where the ARM first programs the part.
 */
static void sgi_kona_flash_reply(SGIKonaState *s, const uint32_t *words,
                                 uint32_t count)
{
    g_autofree uint8_t *reply = g_malloc(4 + 4 * (size_t)count);

    stl_be_p(reply, count);
    for (uint32_t i = 0; i < count; i++) {
        stl_be_p(reply + 4 + 4 * i, words[i]);
    }
    sgi_kona_post_return(s, SGI_KONA_CMD_EEPROM, 4, reply, 4 + 4 * count);
}

static void sgi_kona_flash_status(SGIKonaState *s, uint32_t status)
{
    sgi_kona_flash_reply(s, &status, 1);
}

/*
 * Op 3: the reply buffer, which the pipe keeps in the same place 0x1f2
 * sets, so later answers to either land there. Unlike 0x1f2 it keeps the
 * offset word's low 12 bits.
 */
static void sgi_kona_flash_reply_buffer(SGIKonaState *s, uint32_t page,
                                        uint32_t offset)
{
    uint64_t addr;
    bool mapped;

    if (!sgi_kona_command_page(s, "EEPROM reply", page, offset, &addr,
                               &mapped)) {
        return;
    }
    s->return_page = addr;
    s->return_page_valid = true;
    trace_sgi_kona_flash_reply_buffer(addr, mapped);
    if (!sgi_kona_x_read_current(s)) {
        sgi_kona_x_read_invalidate(s);
    }
}

/*
 * Op 2: `bytes` bytes of flash from `offset`, rounded down to a word as
 * the ARM's word index does. A length that is not whole words would keep
 * the ARM's copy loop from ending, and a reply longer than the buffer
 * would run past it: both are logged and answered with nothing.
 * Approximated: words past the 512 KiB are answered zero, and the read is
 * logged. The ARM would read whatever its bus returns past the part.
 */
static void sgi_kona_flash_read(SGIKonaState *s, uint32_t offset,
                                uint32_t bytes)
{
    g_autofree uint32_t *words = NULL;
    uint32_t count = bytes / 4;

    trace_sgi_kona_flash_read(offset, bytes);
    if (bytes % 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: EEPROM read of 0x%" PRIx32
                      " bytes, not whole words; nothing posted\n", bytes);
        return;
    }
    if (8 + (uint64_t)bytes > SGI_KONA_FLASH_REPLY_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: EEPROM read of 0x%" PRIx32
                      " bytes is longer than the reply buffer; nothing"
                      " posted\n", bytes);
        return;
    }
    offset &= ~3u;
    if ((uint64_t)offset + bytes > SGI_KONA_FLASH_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: EEPROM read at flash"
                      " offset 0x%" PRIx32 " runs past its 512 KiB; read"
                      " as zero there\n", offset);
    }
    words = g_new(uint32_t, MAX(count, 1));
    for (uint32_t i = 0; i < count; i++) {
        words[i] = sgi_kona_flash_load(s, offset + 4 * i);
    }
    sgi_kona_flash_reply(s, words, count);
}

/* Op 6: four zero bytes over the combination's marker. Nothing posted. */
static void sgi_kona_flash_erase_combo(SGIKonaState *s)
{
    s->flash_op.flash_addr = SGI_KONA_FLASH_COMBO;
    sgi_kona_flash_store(s, 0);
    memset(&s->flash_op, 0, sizeof(s->flash_op));
}

static void sgi_kona_flash_open(SGIKonaState *s, uint32_t op)
{
    trace_sgi_kona_flash_op(op);
    memset(&s->flash_op, 0, sizeof(s->flash_op));
    if (op == SGI_KONA_FLASH_OP_ERASE_COMBO) {
        sgi_kona_flash_erase_combo(s);
        return;
    }
    if (op == 0 || op >= ARRAY_SIZE(sgi_kona_flash_op_words)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: EEPROM operation %" PRIu32
                      " is not one of 1 to 6; it takes no more words\n", op);
        return;
    }
    s->flash_op.op = op;
    s->flash_op.need = sgi_kona_flash_op_words[op];
}

/*
 * A write's fixed words are in: a count above SGI_KONA_FLASH_MAX_WORDS
 * fails before any data word is taken, so the next word opens a command.
 * Otherwise max(count, 1) data words follow (the ARM always reads a last
 * word), stored from the type's place: the image at its offset, the
 * combination behind its marker and count, the header at its own place.
 * An unknown type takes its data and stores none.
 */
static void sgi_kona_flash_write_open(SGIKonaState *s)
{
    SGIKonaFlashOp *f = &s->flash_op;
    uint32_t type = f->args[0];
    uint32_t count = f->args[f->need - 1];

    if (count > SGI_KONA_FLASH_MAX_WORDS) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: EEPROM write of %" PRIu32
                      " words, more than %u; refused\n", count,
                      SGI_KONA_FLASH_MAX_WORDS);
        trace_sgi_kona_flash_write(type, 0, count, 0);
        memset(f, 0, sizeof(*f));
        sgi_kona_flash_status(s, 0);
        return;
    }
    f->data = MAX(count, 1);
    f->store = true;
    switch (type) {
    case SGI_KONA_FLASH_TYPE_IMAGE:
        f->flash_addr = f->args[1];
        break;
    case SGI_KONA_FLASH_TYPE_COMBO:
        f->flash_addr = SGI_KONA_FLASH_COMBO;
        sgi_kona_flash_store(s, SGI_KONA_FLASH_COMBO_MARKER);
        sgi_kona_flash_store(s, count);
        break;
    case SGI_KONA_FLASH_TYPE_HEADER:
        f->flash_addr = SGI_KONA_FLASH_HEADER;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: EEPROM write of unknown"
                      " type %" PRIu32 "; its words are stored nowhere\n",
                      type);
        f->store = false;
    }
}

/* An operation's fixed words are all in. */
static void sgi_kona_flash_ready(SGIKonaState *s)
{
    SGIKonaFlashOp *f = &s->flash_op;
    SGIKonaFlashOp done = *f;

    if (f->op == SGI_KONA_FLASH_OP_WRITE) {
        sgi_kona_flash_write_open(s);
        return;
    }
    memset(f, 0, sizeof(*f));
    switch (done.op) {
    case SGI_KONA_FLASH_OP_READ:
        sgi_kona_flash_read(s, done.args[0], done.args[1]);
        break;
    case SGI_KONA_FLASH_OP_REPLY_BUFFER:
        sgi_kona_flash_reply_buffer(s, done.args[0], done.args[1]);
        break;
    case SGI_KONA_FLASH_OP_CHECK:
        /*
         * Approximated: the ARM sends a value round the transaction bus
         * and answers 1 when it comes back intact. The model has no such
         * bus; a working pipe's check passes.
         */
        sgi_kona_flash_status(s, 1);
        break;
    case SGI_KONA_FLASH_OP_IDENTIFY:
        sgi_kona_flash_status(s, SGI_KONA_FLASH_IDENTITY);
        break;
    default:
        g_assert_not_reached();
    }
}

/* A word on the command page while an EEPROM operation is open. */
static void sgi_kona_flash_word(SGIKonaState *s, uint32_t word)
{
    SGIKonaFlashOp *f = &s->flash_op;

    if (f->have < f->need) {
        f->args[f->have++] = word;
        if (f->op == SGI_KONA_FLASH_OP_WRITE && f->have == 1) {
            /* An offset and a count for the image, a count otherwise. */
            f->need = word == SGI_KONA_FLASH_TYPE_IMAGE ? 3 : 2;
        }
        if (f->have == f->need) {
            sgi_kona_flash_ready(s);
        }
        return;
    }
    if (f->store) {
        sgi_kona_flash_store(s, word);
    }
    if (--f->data == 0) {
        uint32_t type = f->args[0];
        uint32_t status = f->store;
        uint32_t start = type == SGI_KONA_FLASH_TYPE_IMAGE ? f->args[1] :
                         type == SGI_KONA_FLASH_TYPE_COMBO ?
                         SGI_KONA_FLASH_COMBO : SGI_KONA_FLASH_HEADER;

        trace_sgi_kona_flash_write(type, start, f->args[f->need - 1], status);
        memset(f, 0, sizeof(*f));
        sgi_kona_flash_status(s, status);
    }
}

/*
 * Whether `f` is a state sgi_kona_flash_word() could hold: nothing open,
 * an operation short of its fixed words, or a write taking at most the
 * data words its count allows.
 */
static bool sgi_kona_flash_op_valid(const SGIKonaFlashOp *f)
{
    uint32_t need;

    if (!f->op) {
        return !f->need && !f->have && !f->data;
    }
    if (f->op >= ARRAY_SIZE(sgi_kona_flash_op_words) ||
        f->op == SGI_KONA_FLASH_OP_ERASE_COMBO) {
        return false;
    }
    need = sgi_kona_flash_op_words[f->op];
    if (f->op == SGI_KONA_FLASH_OP_WRITE && f->have >= 1) {
        need = f->args[0] == SGI_KONA_FLASH_TYPE_IMAGE ? 3 : 2;
    }
    if (f->need != need || f->have > f->need) {
        return false;
    }
    if (f->have < f->need) {
        return !f->data;
    }
    return f->op == SGI_KONA_FLASH_OP_WRITE && f->data &&
           f->args[f->need - 1] <= SGI_KONA_FLASH_MAX_WORDS &&
           f->data <= MAX(f->args[f->need - 1], 1);
}

/*
 * 0x128 W H D, X initialise: write W, H and D to words 2, 3 and 4 of the
 * return page, then word 0 = 1. The X server's hardware initialisation
 * waits for word 0, then compares words 2 to 4 with the width, height and
 * depth code it sent and logs a mismatch. Approximated: the model echoes
 * the request, so it can never report a mode other than the one asked for,
 * where a real pipe presumably reports the mode it set up. The guest check
 * is an X server log with no "arm says" mismatch and no pipe health-check
 * message from the X server's hardware initialisation.
 */
static void sgi_kona_x_initialise(SGIKonaState *s, const uint32_t *args)
{
    uint8_t words[12];

    stl_be_p(words, args[0]);
    stl_be_p(words + 4, args[1]);
    stl_be_p(words + 8, args[2]);
    sgi_kona_post_return(s, SGI_KONA_CMD_X_INIT, 8, words, sizeof(words));
}

/*
 * The video command, SGI_KONA_CMD_VIDEO: the command number c, its data
 * words, then a checksum equal to c plus the data words, modulo 2^32. The
 * X server's sender is established; how the pipe frames what it receives is
 * not.
 *
 * Approximated: nothing on the wire carries the data length, so the model
 * frames by sgi_kona_video_commands[], whose counts are those of the senders
 * read. Where it gives a count, the model takes that many data words and the
 * next word as the checksum. A checksum that does not match drops the
 * command, logged, with nothing posted: the pipe's firmware compares the
 * sum, logs a mismatch and drops the command, running and answering only a
 * command whose sum matches (established from the firmware, which takes
 * the length from its own 93-entry table, whose contents are not known
 * here). Falling back to the checksum search on a mismatch would cover the
 * X server's pass-through sender, which can send other lengths; the model
 * does not, as its own choice, so a pass-through command of another length
 * misframes the words after it. Where the table gives no count (a "tables
 * only" row, or a number past the table), the model frames by that search,
 * which is inferred: every word that does not equal c plus the data words
 * before it is a data word, and the first that does is the checksum. The
 * search cannot tell a data word that happens to equal that sum from the
 * checksum, and a lost checksum would take every later word as data, so the
 * model gives up after SGI_KONA_VIDEO_SEARCH_MAX_WORDS data words, its own
 * bound, logs it and posts nothing. A number past the table is consumed and
 * logged with nothing posted; a real pipe presumably knows the length of
 * every command it accepts. The video command framing qtest checks the
 * counted framing as stated (0x12 at +0xc134cc, then 0 and 0x12 at
 * +0xc1000c, frame as one command whose checksum verifies), and the video
 * qtests check the search on a tables-only command; nothing here is checked
 * against a real pipe.
 */
#define SGI_KONA_VIDEO_SEARCH_MAX_WORDS 1024

static const SGIKonaVideoCommand *sgi_kona_video_lookup(uint32_t command)
{
    /* The table holds every number below its size, in order. */
    return command < sgi_kona_video_num_commands ?
           &sgi_kona_video_commands[command] : NULL;
}

static uint32_t sgi_kona_video_data_words(uint32_t command)
{
    const SGIKonaVideoCommand *v = sgi_kona_video_lookup(command);

    return v ? v->data_words : SGI_KONA_VIDEO_DATA_UNKNOWN;
}

/* What the checksum of the open command must be, given its data so far. */
static uint32_t sgi_kona_video_sum(SGIKonaState *s)
{
    return s->video_command + s->video_first + s->video_tail_sum;
}

static void sgi_kona_video_open(SGIKonaState *s, uint32_t command)
{
    s->video_open = true;
    s->video_command = command;
    s->video_have = 0;
    s->video_first = 0;
    s->video_tail_sum = 0;
}

/*
 * Whether a channel-describing Get names a channel the display generator
 * has: the board mask's bits are its physical channels 0 to n - 1.
 */
static bool sgi_kona_video_names_channel(uint32_t command)
{
    switch (command) {
    case SGI_KONA_VIDEO_GET_CHANNEL_CONFIG:
    case SGI_KONA_VIDEO_GET_RESIZE:
    case SGI_KONA_VIDEO_GET_VAMPIRE:
    case SGI_KONA_VIDEO_GET_CHANNEL_ENABLES:
    case SGI_KONA_VIDEO_GET_FORMAT_INFO:
    case SGI_KONA_VIDEO_GET_CHANNEL_BLANKING:
        return true;
    default:
        return false;
    }
}

/*
 * The data words of a Get's reply, from the channels' state; every word not set
 * here is zero. The layouts are those the 6.5.30 X server's video initialiser
 * in the KONA X driver module and its channel routines read, from the command
 * grammar datasheet's display channel section:
 *
 * - GetBoardConfig: version 1, the board mask (bit n: physical channel n
 *   exists), option mask 0, options code 0 (no option board), flag 0.
 * - GetChannelConfig: word 0 bit 0x1 present, 0x2 the encoder (channel
 *   0), 0x4 and 0x8 together enabled and active; 18 the cursor priority.
 *   Words 1-16 name the format, which the X server looks up as a .vfo
 *   file itself, and 17 is its datestamp. The model answers the words
 *   2-18 of the channel's last LoadFormat, which carry the name and the
 *   datestamp in the same order (observed in the combination the X
 *   server stores in the display generator's flash). That the pipe
 *   answers them from LoadFormat is inferred from that match; real
 *   InfiniteReality gfxinfo captures print format names such as
 *   1600x1200_72_DL.vfo, though the Onyx2 rack capture's channel 0
 *   printed none. A channel with no LoadFormat since its DisableChannel,
 *   and channel 0 at reset, names none.
 * - GetChannelBlanking: word 0 whether the blanking command blanks it.
 * - GetResize: words 0-3 the input rectangle (x, the top row counted from
 *   the bottom of the managed area, width, height), 4-7 the rectangle a
 *   resize may pan over, 8-11 the steps a resize moves in, 12-13 the
 *   largest size and 14-15 the smallest. As the firmware answers it, all
 *   from the channel's record and none from SetupFrameBuffer: 4-7 the
 *   rectangle the last Resize whose word 7 was 0 copied, 0, 0, 1280, 1024
 *   after a start's rebuild; 8-15 PlacementRes words 1-8. Approximated: a
 *   channel with no PlacementRes yet answers steps of 1, 1280x1024 largest
 *   and 0x0 smallest, where the firmware's value before any PlacementRes
 *   is not known; on the firmware a start always runs one.
 * - GetVampire: word 0 the physical channel whose timing this one takes,
 *   the one Vampire last named; the channel itself after reset.
 * - GetChannelEnables: word 1 whether the channel is enabled.
 *
 * Approximated: no reply has been captured from a pipe. The panning
 * rectangle (the whole managed area), the steps (one pixel), the largest
 * size (the managed area) and every word no reader takes are the model's
 * choices, and so is version 1, which the X server only prints. A step or
 * largest size of 0 would pin every resize the X server asks for to an
 * empty rectangle.
 */
static void sgi_kona_video_reply_words(SGIKonaState *s, uint32_t command,
                                       uint32_t channel, uint32_t *words)
{
    const SGIKonaChannel *ch = &s->channel[channel];

    switch (command) {
    case SGI_KONA_VIDEO_GET_BOARD_CONFIG:
        words[0] = 1;
        words[1] = MAKE_64BIT_MASK(0, sgi_kona_channel_count(s));
        break;
    case SGI_KONA_VIDEO_GET_CHANNEL_CONFIG:
        words[0] = 0x1 | (channel == 0 ? 0x2 : 0) | (ch->enabled ? 0xc : 0);
        memcpy(&words[1], ch->format_name, sizeof(ch->format_name));
        words[18] = ch->cursor_priority;
        break;
    case SGI_KONA_VIDEO_GET_RESIZE:
        words[0] = ch->input_x;
        words[1] = ch->input_y;
        words[2] = ch->input_width;
        words[3] = ch->input_height;
        memcpy(&words[4], ch->pan, sizeof(ch->pan));
        memcpy(&words[8], ch->placement, sizeof(ch->placement));
        break;
    case SGI_KONA_VIDEO_GET_VAMPIRE:
        words[0] = ch->source;
        break;
    case SGI_KONA_VIDEO_GET_CHANNEL_BLANKING:
        words[0] = ch->blanked;
        break;
    case SGI_KONA_VIDEO_GET_CHANNEL_ENABLES:
        words[1] = ch->enabled;
        break;
    default:
        break;
    }
}

/*
 * Post the reply to a video Get on the return page: word 2 the reply id,
 * words 3 onward the reply data, the word after the data the id plus the
 * data words, then word 0 = 1, the only word the X server's reply wait
 * polls. The layout is established from that wait; the order is
 * inferred, as for the X initialise echo. Every reply overwrites words 2
 * onward, so nothing an earlier answer left there is read as this one.
 *
 * Approximated: the model answers when the command's checksum arrives,
 * at once, rather than after queued work, and it answers into the one
 * return page it keeps rather than the current context's (see
 * sgi_kona_register_return_page()). The X server's own write-gather
 * flush after each Get is not what triggers the answer: in a stock guest
 * run the flush never reaches the pipe. The video reply placement qtest
 * checks the layout and order.
 */
static void sgi_kona_video_reply(SGIKonaState *s,
                                 const SGIKonaVideoCommand *v)
{
    uint8_t reply[4 + SGI_KONA_VIDEO_MAX_REPLY_BYTES + 4] = { 0 };
    uint32_t data[SGI_KONA_VIDEO_MAX_REPLY_BYTES / 4] = { 0 };
    uint32_t words = v->reply_bytes / 4;
    uint32_t sum = v->reply_id;
    uint32_t channel = sgi_kona_video_names_channel(v->command) ?
                       s->video_first : 0;

    assert(v->reply_bytes <= SGI_KONA_VIDEO_MAX_REPLY_BYTES);
    assert(channel < SGI_KONA_MAX_CHANNELS);
    sgi_kona_video_reply_words(s, v->command, channel, data);
    for (uint32_t w = 0; w < words; w++) {
        stl_be_p(reply + 4 + 4 * w, data[w]);
    }
    stl_be_p(reply, v->reply_id);
    for (uint32_t w = 0; w < words; w++) {
        sum += ldl_be_p(reply + 4 + 4 * w);
    }
    stl_be_p(reply + 4 + v->reply_bytes, sum);
    trace_sgi_kona_video_reply(v->command, v->reply_id, v->reply_bytes);
    sgi_kona_post_return(s, SGI_KONA_CMD_VIDEO, 8, reply,
                         v->reply_bytes + 8);
}

/*
 * TempBlankAllChannels, video command 0x3c: one data word, nonzero to blank the
 * display and 0 to show it again. The IRIX 6.5.30 X server's screen saver is
 * the sender (its screen-blank routine in the KONA X driver module passes its
 * blank flag, 1 or 0, as the word); a traced desktop session sends 1 when the
 * saver fires and 0 at the next pointer motion or key press.
 *
 * Inferred from the command's name in the server's video command table:
 * a blanked display shows every pixel black, and the framebuffer, colour
 * maps and gamma table are left as they were, so the unblank shows the
 * same frame. Whether a blank also hides the cursor, which the shared
 * scanout presents without asking the pipe, and what "temporary" means
 * on a pipe beyond the next unblank or reset, are unknown.
 */
static void sgi_kona_video_temp_blank(SGIKonaState *s, uint32_t blank)
{
    if (s->video_blanked != (blank != 0)) {
        s->video_blanked = blank != 0;
        sgi_kona_invalidate_all(s);
    }
}

/*
 * SetupFrameBuffer [width, height, pixel depth], as the pipe's firmware
 * runs it (the command grammar datasheet's "SetupFrameBuffer on the ARM",
 * established). It never replies. A budget below Small's, or a named depth
 * (0 to 3) the budget does not cover, raises the error flag, which stops a
 * stored combination; -1, -2 and other values pick a depth that fits. The
 * size is the managed area from then on, but configuration block +0x84
 * and +0x88 take it only at the end of a start (sgi_kona_dg_start()).
 *
 * Approximated: the firmware stores a size past the budget all the same;
 * the model keeps drawing the last size its planes hold, logged. And the
 * pixel-depth code at +0x8c stays the configured one, since the model
 * presents the visuals of that depth alone; a word naming another is
 * logged.
 */
static void sgi_kona_setup_frame_buffer(SGIKonaState *s, uint32_t width,
                                        uint32_t height, uint32_t depth)
{
    static const uint8_t need[] = { 8, 16, 32, 64 };
    uint64_t budget = sgi_kona_fb_budget(s, width, height);

    if (budget < need[0]) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: not enough raster "
                      "managers for Small pixels at %ux%u\n", width, height);
        s->dg_error = true;
    } else if (depth < ARRAY_SIZE(need) && budget < need[depth]) {
        s->dg_error = true;
    }
    if (depth < ARRAY_SIZE(need) && depth != s->config.pixel_depth) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: pixel depth %u: the pipe "
                      "presents depth %u\n", depth, s->config.pixel_depth);
    }
    sgi_kona_managed_resize(s, width, height);
}

/*
 * The channel setters, as the 6.5.30 X server's combination load in the KONA X
 * driver module and its channel requests send them; the command grammar
 * datasheet's display channel section gives each word's source. Word 0 of every
 * one is the physical channel, the board mask's bit number.
 *
 * - DisableChannel [channel]: the channel stops; its console shows no
 *   mode, and it holds no format until the next LoadFormat.
 * - LoadFormat [channel, 1, ...]: the channel's format; words 2-17 its
 *   file name and 18 its datestamp, kept for GetChannelConfig; words 23
 *   and 24 the format's active lines and pixels, the channel's output
 *   size. The channel runs.
 * - EnableChannel [channel, on, own timing]: word 1 nonzero runs a
 *   channel that holds a format; otherwise it changes nothing.
 * - Resize [channel, x, top row from the bottom, width, height, 1,
 *   flicker, 0]: the input rectangle, and GetResize's pan rectangle when
 *   word 7 is 0.
 * - PlacementRes [channel, x code, y code, w3, 1, largest width, height,
 *   smallest width, height]: GetResize words 8-15.
 * - Channel blanking [channel, blank]: nonzero shows the channel black.
 * - Channel gamma map [channel, map]: the gamma map the channel shows
 *   through.
 * - CursorPriority [channel, priority], Vampire [channel, source, ...]:
 *   kept for GetChannelConfig's word 18 and GetVampire's word 0.
 *
 * Inferred: the load sends DisableChannel for every channel, then each
 * enabled channel's LoadFormat, Resize and channel blanking [channel, 0],
 * then two EnableChannel passes, [channel, on, 0] then [channel, 0, own
 * timing]; it sends the master channel's as [1, 1, 0] and [1, 0, own]
 * whatever channel the master is. Word 1 cannot be the channel's state,
 * or the second pass would turn every channel off, and a master channel
 * 0 would get no EnableChannel at all. So the model runs a channel from
 * its LoadFormat, and takes EnableChannel's word 1 only as a request to
 * run a channel that has had a LoadFormat since its last DisableChannel.
 * The literal [1, 1, 0] then cannot start channel 1 when the combination
 * leaves it off: the load disabled it and loaded no format for it. With
 * that condition, a load leaves running exactly the channels it loaded a
 * format for; what words 1 and 2, and the literal 1, mean to the display
 * generator is unknown. A hardware trace of a load would settle it.
 *
 * Approximated: a vampire channel, one that takes another channel's
 * timing and pixels, and a channel bound to an X window are logged as
 * unimplemented; the channel keeps showing its own input rectangle.
 * Every other setter is framed and has no effect, the timing words of
 * LoadFormat included: each channel's console updates at the model's
 * one retrace rate.
 */
static void sgi_kona_cursor_update(SGIKonaState *s);

static void sgi_kona_video_setter(SGIKonaState *s, uint32_t command)
{
    const uint32_t *d = s->video_data;
    uint32_t channel = d[0];
    SGIKonaChannel *ch;

    switch (command) {
    case SGI_KONA_VIDEO_DISABLE_CHANNEL:
    case SGI_KONA_VIDEO_LOAD_FORMAT:
    case SGI_KONA_VIDEO_ENABLE_CHANNEL:
    case SGI_KONA_VIDEO_RESIZE:
    case SGI_KONA_VIDEO_CHANNEL_BLANKING:
    case SGI_KONA_VIDEO_CHANNEL_GAMMA_MAP:
    case SGI_KONA_VIDEO_CURSOR_PRIORITY:
    case SGI_KONA_VIDEO_VAMPIRE:
    case SGI_KONA_VIDEO_BIND_TO_X_WINDOW:
    case SGI_KONA_VIDEO_PLACEMENT_RES:
        break;
    case SGI_KONA_VIDEO_SETUP_FRAME_BUFFER:
        sgi_kona_setup_frame_buffer(s, d[0], d[1], d[2]);
        return;
    default:
        return;
    }
    if (channel >= sgi_kona_channel_count(s)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: unsupported video channel %u for 0x%x\n",
                      channel, command);
        return;
    }
    ch = &s->channel[channel];
    trace_sgi_kona_video_channel(command, channel, d[1], d[2], d[3], d[4]);
    switch (command) {
    case SGI_KONA_VIDEO_DISABLE_CHANNEL:
        ch->enabled = false;
        ch->loaded = false;
        break;
    case SGI_KONA_VIDEO_LOAD_FORMAT:
        if (!d[23] || !d[24] || d[23] > SGI_SCANOUT_MAX_DIM ||
            d[24] > SGI_SCANOUT_MAX_DIM) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: channel %u format "
                          "%ux%u is outside the sizes presented\n", channel,
                          d[24], d[23]);
            return;
        }
        ch->format_width = d[24];
        ch->format_height = d[23];
        memcpy(ch->format_name, &d[2], sizeof(ch->format_name));
        ch->loaded = true;
        ch->enabled = true;
        break;
    case SGI_KONA_VIDEO_ENABLE_CHANNEL:
        if (d[1] && ch->loaded) {
            ch->enabled = true;
        }
        break;
    case SGI_KONA_VIDEO_RESIZE:
        if (d[1] > SGI_SCANOUT_MAX_DIM || d[2] > SGI_SCANOUT_MAX_DIM ||
            d[3] > SGI_SCANOUT_MAX_DIM || d[4] > SGI_SCANOUT_MAX_DIM) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: channel %u input "
                          "%ux%u at %u,%u is outside the sizes presented\n",
                          channel, d[3], d[4], d[1], d[2]);
            return;
        }
        ch->input_x = d[1];
        ch->input_y = d[2];
        ch->input_width = d[3];
        ch->input_height = d[4];
        if (!d[7]) {
            memcpy(ch->pan, &d[1], sizeof(ch->pan));
        }
        break;
    case SGI_KONA_VIDEO_PLACEMENT_RES:
        memcpy(ch->placement, &d[1], sizeof(ch->placement));
        return;
    case SGI_KONA_VIDEO_CHANNEL_BLANKING:
        ch->blanked = d[1] != 0;
        break;
    case SGI_KONA_VIDEO_CHANNEL_GAMMA_MAP:
        if (d[1] != 0 && d[1] != SGI_KONA_GAMMA_MAP_LINEAR) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: channel %u gamma map %d: "
                          "the model holds map 0 alone\n", channel,
                          (int32_t)d[1]);
            return;
        }
        ch->gamma_map = d[1];
        break;
    case SGI_KONA_VIDEO_CURSOR_PRIORITY:
        ch->cursor_priority = d[1];
        return;
    case SGI_KONA_VIDEO_VAMPIRE:
        ch->source = d[1];
        if (d[1] != channel) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: channel %u taking channel "
                          "%u's timing is not modelled\n", channel, d[1]);
        }
        return;
    case SGI_KONA_VIDEO_BIND_TO_X_WINDOW:
        if (d[1]) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: channel %u bound to X "
                          "window 0x%x is not modelled\n", channel, d[1]);
        }
        return;
    }
    sgi_kona_channel_apply(s, channel);
    sgi_kona_cursor_update(s);
}

static void sgi_kona_video_complete(SGIKonaState *s, uint32_t checksum)
{
    const SGIKonaVideoCommand *v = sgi_kona_video_lookup(s->video_command);
    uint32_t sum = sgi_kona_video_sum(s);

    s->video_open = false;
    trace_sgi_kona_video_command(s->video_command, v ? v->name : "unknown",
                                 s->video_first, checksum);
    if (sgi_kona_video_data_words(s->video_command) ==
        SGI_KONA_VIDEO_DATA_UNKNOWN) {
        trace_sgi_kona_video_search(s->video_command, s->video_have);
    }
    if (sum != checksum) {
        trace_sgi_kona_video_checksum_mismatch(s->video_command, checksum,
                                               sum);
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: video command 0x%" PRIx32
                      " checksum 0x%" PRIx32 " is not 0x%" PRIx32
                      "; dropped\n", s->video_command, checksum, sum);
        return;
    }
    if (!v) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: unmodelled video command 0x%" PRIx32
                      " (data 0x%" PRIx32 "); consumed, nothing posted\n",
                      s->video_command, s->video_first);
        return;
    }
    if (v->command == SGI_KONA_VIDEO_TEMP_BLANK) {
        sgi_kona_video_temp_blank(s, s->video_first);
    }
    sgi_kona_video_setter(s, v->command);
    if (v->reply_id != SGI_KONA_VIDEO_NO_REPLY) {
        if (sgi_kona_video_names_channel(v->command) &&
            s->video_first >= sgi_kona_channel_count(s)) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-kona: unsupported video channel %u for 0x%x\n",
                          s->video_first, v->command);
            return;
        }
        sgi_kona_video_reply(s, v);
    }
}

/* A word on the command page while a video command is open. */
static void sgi_kona_video_word(SGIKonaState *s, uint32_t word)
{
    uint32_t length = sgi_kona_video_data_words(s->video_command);

    if (length == SGI_KONA_VIDEO_DATA_UNKNOWN ?
        word == sgi_kona_video_sum(s) : s->video_have == length) {
        sgi_kona_video_complete(s, word);
        return;
    }
    if (length != SGI_KONA_VIDEO_DATA_UNKNOWN &&
        s->video_have < SGI_KONA_VIDEO_MAX_DATA_WORDS) {
        s->video_data[s->video_have] = word;
    }
    if (s->video_have++ == 0) {
        s->video_first = word;
    } else {
        s->video_tail_sum += word;
    }
    if (length == SGI_KONA_VIDEO_DATA_UNKNOWN &&
        s->video_have == SGI_KONA_VIDEO_SEARCH_MAX_WORDS) {
        sgi_kona_x_read_invalidate(s);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: video command 0x%" PRIx32 " has no"
                      " checksum within %u data words; closed, nothing"
                      " posted\n", s->video_command,
                      SGI_KONA_VIDEO_SEARCH_MAX_WORDS);
        trace_sgi_kona_video_search_abandoned(s->video_command,
                                              s->video_have);
        s->video_open = false;
    }
}

/*
 * Starting the pipe's firmware: what the ARM does each time the loader
 * releases it after a firmware load (+0x800328 = 1), from the command
 * grammar datasheet's "What the ARM does with a stored combination"
 * (established from the shipped ARM firmware; that the release is the
 * start is inferred). It rebuilds the display generator's default state,
 * blanks every channel and runs the video format combination stored in the
 * flash (op 1 type 1, behind SGI_KONA_FLASH_COMBO_MARKER and its word
 * count) or, when the image is rejected or a command in it raises the
 * error flag, rebuilds again and runs its built-in fallback combination.
 * Only then does configuration block +0x84 and +0x88 take the managed
 * size, so the X server, which sizes its screen from the block, sees the
 * combination that ran. ircombine's eeprom destination and setmon -x store
 * the image; gfxinit, which initializes graphics, reads none of it back.
 *
 * The stored words are video commands framed as on the command page after
 * 0x1a6: the command, its data words and their checksum. The image is
 * accepted only whole: a word count below 0x8000, and every command known,
 * with the data-word count of the model's video command table, and its
 * checksum matching.
 *
 * Approximated: the default-state rebuild is the model's reset channel
 * state with every channel disabled and blanked and its format name
 * cleared (the firmware's rebuild clears the name's first word and the
 * date stamp, among other work); a command the table gives no length for
 * rejects the image, where the firmware has a count for each; a Get in
 * the image is run for no effect and posts no reply; a per-channel command
 * naming a channel the board does not have is logged, where the firmware
 * skips the option-board channels 0xc6, 0xc7 and 0xe6 until a second pass
 * at XServerVideoInit, which the model does not run; and a system reset
 * runs nothing, where a pipe's power-up program also runs the image before
 * the PROM.
 */
static bool sgi_kona_dg_image_valid(const uint32_t *words, uint32_t count)
{
    for (uint32_t at = 0; at < count;) {
        uint32_t length = sgi_kona_video_data_words(words[at]);
        uint32_t sum = words[at];

        if (words[at] >= sgi_kona_video_num_commands ||
            length == SGI_KONA_VIDEO_DATA_UNKNOWN ||
            length > SGI_KONA_VIDEO_MAX_DATA_WORDS ||
            length + 1 >= count - at) {
            return false;
        }
        for (uint32_t i = 1; i <= length; i++) {
            sum += words[at + i];
        }
        if (sum != words[at + length + 1]) {
            return false;
        }
        at += length + 2;
    }
    return true;
}

/*
 * Run a combination @valid already accepted, command by command, as the
 * firmware's runner does; false when a command raises the error flag.
 */
static bool sgi_kona_dg_run(SGIKonaState *s, const uint32_t *words,
                            uint32_t count)
{
    s->dg_error = false;
    for (uint32_t at = 0; at < count;) {
        uint32_t command = words[at];
        uint32_t length = sgi_kona_video_data_words(command);
        const SGIKonaVideoCommand *v = sgi_kona_video_lookup(command);

        memset(s->video_data, 0, sizeof(s->video_data));
        memcpy(s->video_data, &words[at + 1], length * sizeof(uint32_t));
        s->video_first = length ? words[at + 1] : 0;
        trace_sgi_kona_video_command(command, v->name, s->video_first,
                                     words[at + length + 1]);
        if (command == SGI_KONA_VIDEO_TEMP_BLANK) {
            sgi_kona_video_temp_blank(s, s->video_first);
        }
        sgi_kona_video_setter(s, command);
        if (s->dg_error) {
            return false;
        }
        at += length + 2;
    }
    return true;
}

/*
 * The display generator's default state, as the datasheet lists the fields
 * its rebuild resets for each present channel (established): the source
 * channel, the first name word and the date stamp, the pan rectangle (0,
 * 0, 1280, 1024), the enabled bit and gamma map 0. The input rectangle,
 * PlacementRes's words, name words 2-16, the cursor priority and the rest
 * keep their values. The fields the model does not hold (vertical phase,
 * reset ratios, field mode and dominance, the control word's other bits)
 * are not modelled. Then every channel is blanked, as the display set-up
 * that follows does.
 */
static void sgi_kona_dg_default_state(SGIKonaState *s)
{
    for (unsigned c = 0; c < sgi_kona_channel_count(s); c++) {
        SGIKonaChannel *ch = &s->channel[c];

        ch->enabled = false;
        ch->loaded = false;
        ch->source = c;
        ch->format_name[0] = 0;
        ch->format_name[SGI_KONA_FORMAT_NAME_WORDS - 1] = 0;
        ch->pan[0] = 0;
        ch->pan[1] = 0;
        ch->pan[2] = SGI_KONA_FB_WIDTH;
        ch->pan[3] = SGI_KONA_FB_HEIGHT;
        ch->gamma_map = 0;
        /* The display set-up that follows blanks every channel. */
        ch->blanked = true;
    }
}

/* Append video command @command, @data and their checksum to @image. */
static void sgi_kona_dg_append(GArray *image, uint32_t command,
                               const uint32_t *data, unsigned count)
{
    uint32_t sum = command;

    g_array_append_val(image, command);
    for (unsigned i = 0; i < count; i++) {
        g_array_append_val(image, data[i]);
        sum += data[i];
    }
    g_array_append_val(image, sum);
}

/* LoadFormat for @channel, named @name and @stamp, 1280x1024 at 60 Hz. */
static void sgi_kona_dg_append_format(GArray *image, uint32_t channel,
                                      const char *name, uint32_t stamp)
{
    uint32_t format[383] = { channel, 1 };
    char padded[64] = { 0 };

    memcpy(padded, name, strlen(name));
    for (unsigned w = 0; w < 16; w++) {
        format[2 + w] = ldl_be_p(padded + 4 * w);
    }
    format[18] = stamp;
    format[19] = 60 << 16;
    format[20] = 1;
    format[21] = 1065;
    format[22] = 1680;
    format[23] = 1024;
    format[24] = 1280;
    sgi_kona_dg_append(image, 0x16, format, ARRAY_SIZE(format));
}

/*
 * The firmware's built-in fallback combination, as the datasheet's "The
 * built-in fallback combination" gives its commands (established): a
 * 1280x1024 managed area, channel 0 showing 1280x1024_60.vfo, channel 1
 * named 640x486_30i.vfo with channel 0's timing, taking channel 0's timing
 * and blanked. Approximated: only the commands the model gives meaning
 * are built, so not the clock, encoder, gamma, sync, gain, pedestal and
 * phase settings, the cursor priorities, which the fallback sets to 0 as
 * the model's reset does, or the commands for option channel 30;
 * and LoadFormat's words past the size and rate are zero. Both channels'
 * Resize and PlacementRes words are the firmware's.
 */
static GArray *sgi_kona_dg_fallback(void)
{
    GArray *image = g_array_new(false, false, sizeof(uint32_t));

    for (uint32_t c = 0; c < 2; c++) {
        sgi_kona_dg_append(image, SGI_KONA_VIDEO_DISABLE_CHANNEL, &c, 1);
        sgi_kona_dg_append(image, SGI_KONA_VIDEO_CHANNEL_BLANKING,
                           (uint32_t[]){ c, 1 }, 2);
    }
    sgi_kona_dg_append(image, SGI_KONA_VIDEO_SETUP_FRAME_BUFFER,
                       (uint32_t[]){ 1280, 1024, UINT32_MAX }, 3);
    sgi_kona_dg_append_format(image, 0, "1280x1024_60.vfo", 0x31ea4b22);
    sgi_kona_dg_append(image, SGI_KONA_VIDEO_VAMPIRE,
                       (uint32_t[]){ 0, 0, 1280, 1024, 0, 5, 3 }, 7);
    sgi_kona_dg_append(image, SGI_KONA_VIDEO_RESIZE,
                       (uint32_t[]){ 0, 0, 1023, 1280, 1024, 1, 1, 0 }, 8);
    sgi_kona_dg_append(image, SGI_KONA_VIDEO_CHANNEL_BLANKING,
                       (uint32_t[]){ 0, 0 }, 2);
    sgi_kona_dg_append_format(image, 1, "640x486_30i.vfo", 0x31ea4b3c);
    sgi_kona_dg_append(image, SGI_KONA_VIDEO_VAMPIRE,
                       (uint32_t[]){ 1, 0, 640, 486, 1, 5, 3 }, 7);
    sgi_kona_dg_append(image, SGI_KONA_VIDEO_RESIZE,
                       (uint32_t[]){ 1, 0, 1023, 1280, 1024, 1, 1, 0 }, 8);
    sgi_kona_dg_append(image, SGI_KONA_VIDEO_PLACEMENT_RES,
                       (uint32_t[]){ 0, 4, 1, 4, 1, 1280, 1024, 4, 1 }, 9);
    sgi_kona_dg_append(image, SGI_KONA_VIDEO_PLACEMENT_RES,
                       (uint32_t[]){ 1, 4, 1, 4, 1, 2048, 2048, 640, 486 }, 9);
    sgi_kona_dg_append(image, SGI_KONA_VIDEO_CHANNEL_BLANKING,
                       (uint32_t[]){ 1, 1 }, 2);
    sgi_kona_dg_append(image, SGI_KONA_VIDEO_ENABLE_CHANNEL,
                       (uint32_t[]){ 1, 1, 0 }, 3);
    sgi_kona_dg_append(image, SGI_KONA_VIDEO_ENABLE_CHANNEL,
                       (uint32_t[]){ 1, 0, 0 }, 3);
    sgi_kona_dg_append(image, SGI_KONA_VIDEO_ENABLE_CHANNEL,
                       (uint32_t[]){ 0, 1, 0 }, 3);
    sgi_kona_dg_append(image, SGI_KONA_VIDEO_ENABLE_CHANNEL,
                       (uint32_t[]){ 0, 0, 1 }, 3);
    return image;
}

static void sgi_kona_dg_start(SGIKonaState *s)
{
    uint32_t saved_data[SGI_KONA_VIDEO_MAX_DATA_WORDS];
    uint32_t saved_first = s->video_first;
    g_autofree uint32_t *words = NULL;
    uint32_t count = 0;
    bool ran = false;

    memcpy(saved_data, s->video_data, sizeof(saved_data));
    sgi_kona_dg_default_state(s);
    if (sgi_kona_flash_load(s, SGI_KONA_FLASH_COMBO) ==
        SGI_KONA_FLASH_COMBO_MARKER) {
        count = sgi_kona_flash_load(s, SGI_KONA_FLASH_COMBO + 4);
        if (count >= 0x8000) {
            count = 0;
        }
        words = g_new(uint32_t, MAX(count, 1));
        for (uint32_t i = 0; i < count; i++) {
            words[i] = sgi_kona_flash_load(s,
                                           SGI_KONA_FLASH_COMBO + 8 + 4 * i);
        }
        trace_sgi_kona_flash_combination(count);
        ran = count && sgi_kona_dg_image_valid(words, count) &&
              sgi_kona_dg_run(s, words, count);
        if (!ran) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: stored combination "
                          "of %u words failed to load; running the "
                          "built-in one\n", count);
            sgi_kona_dg_default_state(s);
        }
    }
    if (!ran) {
        g_autoptr(GArray) fallback = sgi_kona_dg_fallback();

        trace_sgi_kona_dg_fallback();
        sgi_kona_dg_run(s, (uint32_t *)fallback->data, fallback->len);
    }
    s->cfg_width = s->fb_width;
    s->cfg_height = s->fb_height;
    sgi_kona_channels_apply(s);
    sgi_kona_cursor_update(s);
    s->video_first = saved_first;
    memcpy(s->video_data, saved_data, sizeof(saved_data));
}

/*
 * 0x12e, X load colour map: a start address, a count n, then n entries
 * (established from the X server's colour-map install and store routines).
 * The command port frames the first two words; entry i lands at start + i
 * in the X colour map. An entry past the table is counted and logged, not
 * stored.
 *
 * Approximated: no maximum count is known, so a wrong count misframes
 * every word after it; the variable colour-map length qtest checks the
 * framing. The entry layout, 10:10:10 with red in bits 29:20, is read
 * from the stock X server's load.
 */
static void sgi_kona_color_map_complete(SGIKonaState *s)
{
    if (s->color_map_outside) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: colour map load at 0x%" PRIx32 " count %"
                      PRIu32 ": %" PRIu32 " entries outside the model's"
                      " table; consumed, not stored\n", s->color_map_start,
                      s->color_map_count, s->color_map_outside);
    }
    trace_sgi_kona_color_map_done(s->color_map_start, s->color_map_count,
                                  s->color_map_outside);
}

static void sgi_kona_color_map_open(SGIKonaState *s, uint32_t start,
                                    uint32_t count)
{
    trace_sgi_kona_color_map(start, count);
    s->color_map_start = start;
    s->color_map_count = count;
    s->color_map_remaining = count;
    s->color_map_outside = 0;
    if (!count) {
        sgi_kona_color_map_complete(s);
    }
}

/* A word on the command page while a colour-map load takes its entries. */
static void sgi_kona_color_map_word(SGIKonaState *s, uint32_t word)
{
    uint64_t addr = (uint64_t)s->color_map_start +
                    (s->color_map_count - s->color_map_remaining);

    if (addr < SGI_KONA_X_COLOR_MAP_ENTRIES) {
        s->x_color_map[addr] = word;
        /* X pixels already drawn in this entry change colour. */
        sgi_kona_invalidate_all(s);
    } else {
        s->color_map_outside++;
    }
    if (--s->color_map_remaining == 0) {
        sgi_kona_color_map_complete(s);
    }
}

/*
 * 0x12f, X load gamma: a gamma-RAM start address, a count n, then n entries,
 * each a 16-bit table value stored as a zero-extended word (established from
 * the KONA X driver module's sender). The command port frames the first two
 * words; the entries follow here and land in s->gamma, one address per entry
 * from the start address. The table is stored for the scanout to apply later;
 * the framebuffer is still shown as stored.
 *
 * Approximated: the table layout. The sender establishes the start
 * addresses a type-0 gamma block sends, 0, 0x1000 and 0x2000 for
 * component masks 1, 2 and 4, each with count 0x1000; that those are
 * red, green and blue and that the gamma RAM holds 4,096 entries per
 * component is inferred. The model keeps exactly that much: three
 * components of the one channel it emulates, component c at c * 0x1000.
 * A type-1 block's addresses (0x40000 + channel * 0x4000), and any other
 * address past the table, are consumed, counted and logged, not stored:
 * what they reach on a pipe is unknown. An entry with bits above the low
 * sixteen keeps the low sixteen and is logged; the sender never makes
 * one. What would check the layout is a gamma-RAM readback on a real
 * pipe; the gamma-load qtests check the framing and placement as the
 * sender establishes them, and the stock guest's three loads framing as
 * three commands with no entry read as a token checks the framing in a
 * run.
 */
/* Note which gamma components hold a table, and repaint the display. */
static void sgi_kona_gamma_update(SGIKonaState *s)
{
    for (unsigned c = 0; c < SGI_KONA_GAMMA_COMPONENTS; c++) {
        const uint16_t *t = &s->gamma[c * SGI_KONA_GAMMA_COMPONENT_ENTRIES];

        s->gamma_loaded[c] = false;
        for (unsigned i = 0; i < SGI_KONA_GAMMA_COMPONENT_ENTRIES; i++) {
            if (t[i]) {
                s->gamma_loaded[c] = true;
                break;
            }
        }
    }
    sgi_kona_invalidate_all(s);
}

static void sgi_kona_gamma_complete(SGIKonaState *s)
{
    sgi_kona_gamma_update(s);
    if (s->gamma_outside) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: gamma load at 0x%" PRIx32 " count %" PRIu32
                      ": %" PRIu32 " entries outside the model's table;"
                      " consumed, not stored\n", s->gamma_start,
                      s->gamma_count, s->gamma_outside);
    }
    trace_sgi_kona_gamma_load_done(s->gamma_start, s->gamma_count,
                                   s->gamma_outside);
}

static void sgi_kona_gamma_open(SGIKonaState *s, uint32_t start,
                                uint32_t count)
{
    trace_sgi_kona_gamma_load(start, count);
    s->gamma_start = start;
    s->gamma_count = count;
    s->gamma_remaining = count;
    s->gamma_outside = 0;
    if (!count) {
        sgi_kona_gamma_complete(s);
    }
}

/* A word on the command page while a gamma load takes its entries. */
static void sgi_kona_gamma_word(SGIKonaState *s, uint32_t word)
{
    uint32_t addr = s->gamma_start + (s->gamma_count - s->gamma_remaining);

    if (addr < SGI_KONA_GAMMA_ENTRIES) {
        if (word > UINT16_MAX) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: gamma entry 0x%" PRIx32 " at 0x%" PRIx32
                          " is wider than 16 bits; low 16 bits kept\n",
                          word, addr);
        }
        s->gamma[addr] = word;
    } else {
        s->gamma_outside++;
    }
    if (--s->gamma_remaining == 0) {
        sgi_kona_gamma_complete(s);
    }
}

#define SGI_KONA_CURSOR_ENTRIES 64

static bool sgi_kona_cursor_offset_valid(uint32_t offset)
{
    return offset % SGI_KONA_X_CURSOR_DATA_WORDS == 0 &&
           offset / SGI_KONA_X_CURSOR_DATA_WORDS < SGI_KONA_CURSOR_ENTRIES;
}

static int sgi_kona_cursor_coord(uint32_t value)
{
    int coord = value & 0x1fff;

    return (coord >= 4096 ? coord - 8192 : coord) - 32;
}

/*
 * The cursor shows on one channel, the one the kernel last selected with
 * selector 0x100, at the position selector 0x102 gives, which the kernel
 * computes relative to that channel's origin (see the command grammar's
 * cursor section). Every other channel shows none. A channel number the
 * display generator does not have shows the cursor nowhere.
 */
static void sgi_kona_cursor_update(SGIKonaState *s)
{
    uint32_t argb[64 * 64];
    unsigned entry = s->cursor_offset / SGI_KONA_X_CURSOR_DATA_WORDS;
    int x = sgi_kona_cursor_coord(s->cursor_position);
    int y = sgi_kona_cursor_coord(s->cursor_position >> 13);
    unsigned channel = s->cursor_channel;
    SGIKonaChannelView v;
    bool on = s->cursor_mode == 2 &&
              sgi_kona_cursor_offset_valid(s->cursor_offset) &&
              s->cursor_valid[entry] &&
              sgi_kona_channel_view(s, channel, &v) &&
              x >= -63 && x < (int)v.out_width &&
              y >= -63 && y < (int)v.out_height;

    if (on) {
        const uint32_t *image = s->cursor_images + entry * 260;
        uint32_t colors[4] = { 0 };
        static const unsigned color_word[] = { 0, 257, 258, 256 };

        for (unsigned code = 1; code < 4; code++) {
            colors[code] = 0xff000000 |
                           sgi_kona_rgb10(image[color_word[code]]);
        }
        for (unsigned row = 0; row < 64; row++) {
            for (unsigned col = 0; col < 64; col++) {
                unsigned half = col / 32, shift = 31 - col % 32;
                unsigned a = (image[4 * row + half] >> shift) & 1;
                unsigned b = (image[4 * row + 2 + half] >> shift) & 1;

                argb[row * 64 + col] = colors[a + 2 * b];
            }
        }
        sgi_scanout_cursor_define(sgi_kona_channel_scanout(s, channel),
                                  64, 64, 0, 0, argb);
    }
    for (unsigned c = 0; c < sgi_kona_channel_count(s); c++) {
        sgi_scanout_cursor_move(sgi_kona_channel_scanout(s, c), x, y,
                                on && c == channel);
    }
}

static void sgi_kona_cursor_register(SGIKonaState *s, uint32_t value)
{
    switch (s->cursor_selector) {
    case 0x100:
        s->cursor_channel = value;
        if (value >= sgi_kona_channel_count(s)) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-kona: unsupported cursor channel %u of %u\n",
                          value, sgi_kona_channel_count(s));
        }
        break;
    case 0x101:
        s->cursor_offset = value;
        if (!sgi_kona_cursor_offset_valid(value) ||
            !s->cursor_valid[value / SGI_KONA_X_CURSOR_DATA_WORDS]) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-kona: invalid or unloaded cursor offset 0x%x\n",
                          value);
        }
        break;
    case 0x102: {
        int x = sgi_kona_cursor_coord(value);
        int y = sgi_kona_cursor_coord(value >> 13);

        s->cursor_position = value;
        if (x < -63 || x >= s->fb_width ||
            y < -63 || y >= s->fb_height) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-kona: unsupported cursor position %d,%d\n",
                          x, y);
        }
        break;
    }
    case 0x103:
        s->cursor_mode = value;
        if (value != 0 && value != 2) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: unsupported cursor mode %u\n",
                          value);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: unsupported cursor selector 0x%x value 0x%x\n",
                      s->cursor_selector, value);
        return;
    }
    sgi_kona_cursor_update(s);
}

static void sgi_kona_cursor_open(SGIKonaState *s, uint32_t offset, bool recolor)
{
    trace_sgi_kona_x_cursor(offset);
    s->cursor_stage_entry = SGI_KONA_CURSOR_ENTRIES;
    s->cursor_stage_words = recolor ? 4 : SGI_KONA_X_CURSOR_DATA_WORDS;
    s->cursor_remaining = s->cursor_stage_words;
    memset(s->cursor_stage, 0, sizeof(s->cursor_stage));
    if (recolor && offset == 0x269c) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: unsupported crosshair recolouring\n");
    } else if (!sgi_kona_cursor_offset_valid(offset)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: invalid cursor load offset 0x%x\n",
                      offset);
    } else {
        unsigned entry = offset / SGI_KONA_X_CURSOR_DATA_WORDS;

        if (!recolor || s->cursor_valid[entry]) {
            s->cursor_stage_entry = entry;
        } else {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-kona: recolour of unloaded cursor offset 0x%x\n",
                          offset);
        }
    }
}

static void sgi_kona_cursor_word(SGIKonaState *s, uint32_t word)
{
    unsigned have = s->cursor_stage_words - s->cursor_remaining;

    s->cursor_stage[have] = word;
    if (--s->cursor_remaining == 0) {
        unsigned entry = s->cursor_stage_entry;

        if (entry < SGI_KONA_CURSOR_ENTRIES) {
            uint32_t *image = s->cursor_images + entry * 260;

            memcpy(image + (s->cursor_stage_words == 4 ? 256 : 0),
                   s->cursor_stage, s->cursor_stage_words * sizeof(*image));
            s->cursor_valid[entry] = 1;
            sgi_kona_cursor_update(s);
        }
        s->cursor_stage_words = 0;
        memset(s->cursor_stage, 0, sizeof(s->cursor_stage));
    }
}

/* The widest or highest image the model takes: its bound, not a pipe's. */
#define SGI_KONA_GL_IMAGE_MAX 8192

/*
 * The GL client's 0xee 0 (get line-mode distance, paraphrasing the pipe's
 * command name): the IRIX client library's special-query routine waits
 * for word 0 of the return page to go non-zero, and its caller then copies 32
 * words from byte 8, eight records of four, words 0 and 1 of each into one
 * 16-word context block and words 2 and 3 into another, which the library's
 * pixel-transfer code reads later (established from the reader). The answer
 * goes on the current context's return page.
 *
 * Words 0 and 1 of record k are image widths for a convolution filter k
 * pixels wide (established from the readers): a convolved DrawPixels no
 * wider than word 1 is sent whole; a wider one is first put to the pipe
 * with 0x116, whose answer picks word 0 or word 1 as the widest strip,
 * and the image is then sent in overlapping strips. Approximated: the
 * pipe's own widths are unknown; the model answers its own image bound,
 * SGI_KONA_GL_IMAGE_MAX, for both, so it convolves any image it takes in
 * one piece. What words 2 and 3 mean is unknown, and zeros are the
 * model's choice. The guest checks are the minimal GLX program's probe
 * going on from 0xee to its viewport 0xe3 and scissor 0xc, and a
 * convolved DrawPixels drawing without 0x116.
 */
#define SGI_KONA_GL_LINE_DISTANCE_WORDS 32

static void sgi_kona_gl_line_distance(SGIKonaState *s)
{
    uint8_t words[SGI_KONA_GL_LINE_DISTANCE_WORDS * 4] = { 0 };

    for (unsigned k = 0; k < SGI_KONA_GL_LINE_DISTANCE_WORDS / 4; k++) {
        stl_be_p(words + k * 16, SGI_KONA_GL_IMAGE_MAX);
        stl_be_p(words + k * 16 + 4, SGI_KONA_GL_IMAGE_MAX);
    }
    sgi_kona_post_return(s, SGI_KONA_CMD_GL_LINE_DISTANCE, 8, words,
                         sizeof(words));
}

/*
 * 0x116 (see SGI_KONA_CMD_GL_SINGLE_COMPONENT): the library reads one
 * word at byte 8 of the return page. Approximated: what the four words
 * ask and what the pipe answers are unknown; the model answers 0, which
 * makes the library take word 1 of 0xee's record as its strip width.
 */
static void sgi_kona_gl_single_component(SGIKonaState *s)
{
    uint8_t word[4] = { 0 };

    sgi_kona_post_return(s, SGI_KONA_CMD_GL_SINGLE_COMPONENT, 8, word,
                         sizeof(word));
}

/* OpenGL 1.2.1 ordinary errors and TABLE_TOO_LARGE. Zero is no error. */
static bool sgi_kona_gl_error_valid(uint32_t error)
{
    return !error || (error >= 0x500 && error <= 0x505) || error == 0x8031;
}

static void sgi_kona_gl_set_error(SGIKonaState *s, uint32_t error)
{
    int i = s->context_current;

    if (!sgi_kona_gl_error_valid(error)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: unrecognized native GL error 0x%x;"
                      " pending error unchanged\n", error);
    } else if (i >= 0 && i < SGI_KONA_CONTEXTS && s->contexts[i].valid &&
               !s->state[i]->pipe.pending_error) {
        s->state[i]->pipe.pending_error = error;
    }
}

/*
 * The client reads a big-endian error code at byte eight after ready.
 * Native errors precede shared draw errors, with both consumed only after
 * payload and ready writes complete. Errors are independent of attributes.
 */
static void sgi_kona_gl_get_error(SGIKonaState *s)
{
    int i = s->context_current;
    uint32_t code;
    uint8_t error[4];

    if (!sgi_kona_x_read_current(s) || s->state[i]->gl.in_begin) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: GL error query without a usable"
                      " current context; pending errors retained\n");
        return;
    }
    code = s->state[i]->pipe.pending_error ? s->state[i]->pipe.pending_error :
                                       s->state[i]->gl.polygon.error;
    stl_be_p(error, code);
    if (!sgi_kona_post_return(s, SGI_KONA_CMD_GL_GET_ERROR, 8, error,
                              sizeof(error))) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: GL error reply failed;"
                      " error 0x%x retained\n", code);
        return;
    }
    if (s->state[i]->pipe.pending_error) {
        s->state[i]->pipe.pending_error = 0;
    } else {
        uint32_t consumed;

        bool completed = sgi_gl_consume_error(&s->state[i]->gl, &consumed);

        assert(completed && consumed == code);
    }
}

/*
 * The GL client's 0x3b 0 and 0x3d 0, the current-attribute and raster-position
 * queries. The GL client library's two senders (n32 6.5.30) pass the selector
 * to the client library's special-query routine, wait for word 0 of the return
 * page, and return a pointer into the words from byte 8 by the pname asked for;
 * the library's glGet descriptors read every one of those words as a binary32.
 * So the layouts below are established from the reader:
 *
 *   0x3b: current RGBA (+0x00), index (+0x10), normal xyz (+0x14),
 *         texture STRQ (+0x20), and one word for pname 0xb43 (+0x30);
 *   0x3d: raster RGBA (+0x00), index (+0x10), texture STRQ (+0x14),
 *         window xyz and clip w (+0x24), valid (+0x34), distance (+0x38).
 *
 * Inferred: that the pipe writes the whole block, since the client does
 * not say which pname it wants; and the valid flag as 1.0 or 0.0, the
 * value GL's float query gives a boolean. Unknown: what pname 0xb43 is,
 * which no OpenGL 1.2 enumerant names; the model answers 0. Like the
 * error query, a query with no usable current context posts nothing.
 */
#define SGI_KONA_GL_CURRENT_WORDS 13
#define SGI_KONA_GL_RASTER_WORDS  15

static void sgi_kona_gl_store_floats(uint8_t *p, const float *v, unsigned n)
{
    for (unsigned k = 0; k < n; k++) {
        uint32_t bits;

        memcpy(&bits, &v[k], sizeof(bits));
        stl_be_p(p + 4 * k, bits);
    }
}

static void sgi_kona_gl_get_block(SGIKonaState *s, uint32_t token)
{
    uint8_t block[SGI_KONA_GL_RASTER_WORDS * 4] = { 0 };
    const SGIGLContext *c;
    uint32_t len;

    if (!sgi_kona_x_read_current(s) ||
        s->state[s->context_current]->gl.in_begin) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: GL query 0x%x without a usable"
                      " current context; no reply\n", token);
        return;
    }
    c = &s->state[s->context_current]->gl;
    if (token == SGI_KONA_CMD_GL_GET_CURRENT) {
        sgi_kona_gl_store_floats(block, c->cur_color, 4);
        sgi_kona_gl_store_floats(block + 0x10, &c->cur_index, 1);
        sgi_kona_gl_store_floats(block + 0x14, c->cur_normal, 3);
        sgi_kona_gl_store_floats(block + 0x20, c->cur_texcoord, 4);
        len = SGI_KONA_GL_CURRENT_WORDS * 4;
    } else {
        float valid = c->raster_valid ? 1.0f : 0.0f;

        sgi_kona_gl_store_floats(block, c->raster_color, 4);
        sgi_kona_gl_store_floats(block + 0x10, &c->raster_index, 1);
        sgi_kona_gl_store_floats(block + 0x14, c->raster_texcoord, 4);
        sgi_kona_gl_store_floats(block + 0x24, c->raster_pos, 4);
        sgi_kona_gl_store_floats(block + 0x34, &valid, 1);
        sgi_kona_gl_store_floats(block + 0x38, &c->raster_distance, 1);
        len = SGI_KONA_GL_RASTER_WORDS * 4;
    }
    trace_sgi_kona_gl_reply(token, 0, len);
    sgi_kona_post_return(s, token, 8, block, len);
}

/*
 * The rules the GL client's drawing is rasterized under (OpenGL 1.1's
 * polygon rule: a pixel is drawn when its centre is inside).
 *
 * Chosen, pending capture: the subpixel bits, rounding and tie rule are
 * the shared rasterizer's first implemented set, not measured on this
 * pipe; the minimal GLX probe puts no pixel centre on an edge, so none of
 * them changes its images, and a capture of a triangle with a centre on
 * an edge would fix them. Eight bits a channel is the model's colour
 * planes' depth, not the pipe's: the client's visual carries twelve. No
 * dither is applied whatever GL_DITHER's enable; the probe disables it,
 * and this pipe's dither pattern is unknown.
 *
 * Smoothed points, lines and polygons take OpenGL's area coverage. The
 * Raster Manager's pixel generator calculates coverage alpha for
 * antialiasing (Kilgard, "Realizing OpenGL", SIGGRAPH 97, 3.4), but how
 * is not published; the area rule is inferred, and a capture of a
 * smoothed point and a smoothed diagonal line would settle it.
 */
static const SGIRasterRules sgi_kona_gl_rules = {
    .subpixel_bits = 8,
    .color_bits = 8,
    .depth_bits = 24,
    .rounding = SGI_ROUND_HALF_AWAY,
    .tie_rule = SGI_TIE_TOP_LEFT_BOTTOM_ORIGIN,
    .arith = SGI_ARITH_DOUBLE,
    .line_rule = SGI_LINE_COVERAGE_DOUBLE,
    .dither = SGI_DITHER_NONE,
    .dither_anchor = SGI_DITHER_SCREEN,
    .smooth_primitives = true,
    .smooth_rule = SGI_SMOOTH_AREA,
};

/*
 * The first overlay planes as a GL colour target, beside the colour
 * banks' plane ids: the target of a context whose visual is an overlay
 * one (sgi_kona_gl_overlay_config()). Its indices are stored in the
 * desktop's first overlay plane, which the X server's overlay GC writes
 * and the scanout shows through display-ID map entry 0x40 with index 0
 * transparent (sgi_kona_desktop_compose()).
 */
#define SGI_KONA_GL_OVERLAY_TARGET 0x100

/*
 * A buffer configuration for an overlay visual: level 1, colour index
 * of 2, 4 or 8 bits, single-buffered, nothing else.
 *
 * Established: the IRIX 6.5.30 n32 GL client library's configuration routine
 * sends the context's level as the first word, and SceneViewer's logo context
 * sent 1 0 8 0 0 0 0 0 0 0, whose window the
 * kernel then placed with display ID 0x44, an overlay ID. Inferred: that level
 * 1 draws into the overlay planes the X server's overlay visuals use, the
 * planes at the same level; the 2-, 4- and 8-bit sizes are those visuals'
 * depths (the KONA X driver module's small visual table). Only 8 bits has been
 * seen from a GL client. A double-buffered overlay, an RGB one and level 2 (the
 * popup planes) are not modelled: such a context draws nothing.
 */
static bool sgi_kona_gl_overlay_config(const uint32_t *v)
{
    return v[0] == 1 && !v[1] && (v[2] == 2 || v[2] == 4 || v[2] == 8) &&
           !v[3] && !v[4] && !v[5] && !v[6] && !v[7] && !v[8] && !v[9];
}

static const SGIPlane *sgi_kona_overlay_plane(SGIKonaState *s)
{
    return sgi_board_memory_plane(&s->desktop_mem, 1);
}

/*
 * The colour targets a drawable names are colour bank plane ids. A GL
 * pixel is direct colour, so its tag is too, and the scanout shows its
 * word as stored. Any other target, such as the back buffer of a
 * single-buffered context, which has none, stores nothing.
 */
static int sgi_kona_target_bank(SGIKonaState *s, SGIColourTarget target)
{
    if (target == s->plane_front) {
        return 0;
    }
    if (target == s->plane_back) {
        return 1;
    }
    return -1;
}

/*
 * Whether the current context may store at plane pixel (x, y): always,
 * unless its window's owned pixels overflowed the clip list, when the
 * pixel must show the window's display ID (sgi_kona_gl_window_owns()).
 */
static bool sgi_kona_gl_pixel_owned(SGIKonaState *s, int x, int y)
{
    const SGIKonaGLWindow *w;

    if (s->context_current == SGI_KONA_CONTEXT_NONE) {
        return true;
    }
    w = &s->state[s->context_current]->window;
    return !w->owned_by_pixel || sgi_kona_gl_window_owns(s, w, x, y);
}

static void sgi_kona_gl_store_colour(void *opaque, SGIColourTarget target,
                                     int x, int y, uint32_t colour)
{
    SGIKonaState *s = opaque;
    int bank = sgi_kona_target_bank(s, target);
    const SGIPlane *p;

    if (target == SGI_KONA_GL_OVERLAY_TARGET) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: direct colour into the overlay "
                      "planes is not modelled; fragment dropped\n");
        return;
    }
    if (bank < 0 || !sgi_kona_gl_pixel_owned(s, x, y)) {
        return;
    }
    p = sgi_kona_plane(s, sgi_kona_bank_plane(s, bank));
    sgi_plane_store(&s->mem, p, x, y, colour & 0x00ffffff);
    sgi_plane_store(&s->mem, sgi_kona_plane(s, sgi_kona_bank_tag_plane(s,
                                                                  bank)),
                    x, y, SGI_KONA_TAG_DIRECT);
    sgi_kona_invalidate_store(s, p, y);
}

static void sgi_kona_gl_store_index(void *opaque, SGIColourTarget target,
                                    int x, int y, uint32_t index, uint32_t mask)
{
    SGIKonaState *s = opaque;
    int bank = sgi_kona_target_bank(s, target);
    uint32_t bits = s->context_current == SGI_KONA_CONTEXT_NONE ? 0 :
                    s->state[s->context_current]->gl.cur.index_bits;
    const SGIPlane *p;
    uint32_t old, shift;

    if (target == SGI_KONA_GL_OVERLAY_TARGET) {
        /*
         * An overlay index sits in the plane word's low bits, as the X
         * server's overlay GC writes it; bits above the visual's depth
         * are left alone.
         */
        mask &= (1u << bits) - 1;
        if (!bits || !mask || !sgi_kona_gl_pixel_owned(s, x, y)) {
            return;
        }
        p = sgi_kona_overlay_plane(s);
        old = sgi_plane_load(&s->desktop_mem, p, x, y);
        sgi_plane_store(&s->desktop_mem, p, x, y,
                        (old & ~mask) | (index & mask));
        s->desktop_state_used = true;
        sgi_kona_invalidate_store(s, p, y);
        return;
    }
    if (bank < 0 || (bits != 8 && bits != 12) ||
        !sgi_kona_gl_pixel_owned(s, x, y)) {
        return;
    }
    shift = 16 - bits;
    mask = (mask & ((1u << bits) - 1)) << shift;
    if (!mask) {
        return;
    }
    p = sgi_kona_plane(s, sgi_kona_bank_plane(s, bank));
    old = sgi_plane_load(&s->mem, p, x, y);
    sgi_plane_store(&s->mem, p, x, y,
                    ((old & ~mask) | ((index << shift) & mask)) & 0xffff);
    sgi_plane_store(&s->mem, sgi_kona_plane(s,
                       sgi_kona_bank_tag_plane(s, bank)),
                    x, y, SGI_KONA_TAG_X_INDEX16);
    sgi_kona_invalidate_store(s, p, y);
}

static uint32_t sgi_kona_gl_load_colour(void *opaque, SGIColourTarget target,
                                        int x, int y)
{
    SGIKonaState *s = opaque;
    int bank = sgi_kona_target_bank(s, target);

    if (target == SGI_KONA_GL_OVERLAY_TARGET) {
        uint32_t bits = s->context_current == SGI_KONA_CONTEXT_NONE ? 0 :
                        s->state[s->context_current]->gl.cur.index_bits;

        return sgi_plane_load(&s->desktop_mem, sgi_kona_overlay_plane(s),
                              x, y) & ((1u << bits) - 1);
    }
    if (bank >= 0 && s->context_current != SGI_KONA_CONTEXT_NONE) {
        uint32_t bits = s->state[s->context_current]->gl.cur.index_bits;

        if (bits == 8 || bits == 12) {
            return (sgi_plane_load(&s->mem, sgi_kona_plane(s,
                    sgi_kona_bank_plane(s, bank)), x, y) >> (16 - bits)) &
                   ((1u << bits) - 1);
        }
    }
    return bank < 0 ? 0 : sgi_kona_bank_read(s, bank, x, y) & 0x00ffffff;
}

static uint32_t sgi_kona_gl_load_depth(void *opaque, int x, int y)
{
    SGIKonaState *s = opaque;

    return sgi_plane_load(&s->mem, sgi_kona_plane(s, s->plane_depth), x, y);
}

static void sgi_kona_gl_store_depth(void *opaque, int x, int y,
                                    uint32_t depth)
{
    SGIKonaState *s = opaque;

    if (!sgi_kona_gl_pixel_owned(s, x, y)) {
        return;
    }
    sgi_plane_store(&s->mem, sgi_kona_plane(s, s->plane_depth), x, y, depth);
}

static uint32_t sgi_kona_gl_load_stencil(void *opaque, int x, int y)
{
    SGIKonaState *s = opaque;

    return sgi_plane_load(&s->mem, sgi_kona_plane(s, s->plane_stencil), x, y);
}

static void sgi_kona_gl_store_stencil(void *opaque, int x, int y,
                                      uint32_t value)
{
    SGIKonaState *s = opaque;

    if (!sgi_kona_gl_pixel_owned(s, x, y)) {
        return;
    }
    sgi_plane_store(&s->mem, sgi_kona_plane(s, s->plane_stencil), x, y, value);
}

/*
 * Drawing classification does not widen the zero-stencil RGB read contract.
 *
 * mirrors (IRIX 6.5.30 demos) asks for the visual 0 10 0 1 0 4 15 1 0 0:
 * 10-bit RGB components, double-buffered, four samples, a 15-bit depth
 * buffer and a one-bit stencil. The model draws every visual single
 * sampled with eight-bit colour, so a 10-bit RGB visual and four or eight
 * samples, the counts the Small and Medium visual tables offer, are
 * approximated: drawing is the single-sample result OpenGL gives with
 * multisampling off, whatever GL_MULTISAMPLE_SGIS, the sample mask and the
 * sample pattern say (they are accepted and kept by the client, not
 * applied). Where the pipe resolves samples, and what a readback of a
 * multisampled buffer returns, is unknown; an edge drawn with and without
 * multisampling on a real pipe, read back, would settle it. A one-bit
 * stencil keeps
 * its stencil values to that bit, as OpenGL's stencil width rules say;
 * the model stores it in the low bit of its eight-bit plane.
 */
static bool sgi_kona_gl_extra_buffers(const uint32_t *v);

static int sgi_kona_gl_stencil_bits(SGIKonaState *s, uint32_t context)
{
    const SGIKonaGLPipe *p;
    const uint32_t *v;

    if (context >= SGI_KONA_CONTEXTS) {
        return -1;
    }
    p = &s->state[context]->pipe;
    v = p->draw_config;
    if (!p->draw_config_received || v[0] != 0 ||
        !(((v[1] == 8 || v[1] == 10 || v[1] == 12) && !v[2]) ||
          (!v[1] && (v[2] == 8 || v[2] == 12))) || v[3] > 1 ||
        v[4] != 0 || (v[5] != 0 && ((v[5] != 4 && v[5] != 8) || !v[1])) ||
        v[6] > 32 || (v[7] != 0 && v[7] != 1 && v[7] != 8) ||
        !sgi_kona_gl_extra_buffers(v)) {
        return -1;
    }
    return v[7];
}

/*
 * The draw target. `textured` is false when the operation enables no
 * texture target, and then the bound texture is not resolved: nothing
 * reads it without one, and resolving rebuilds the bound object's level
 * views, which every untextured primitive paid for.
 */
/*
 * In a slice of display-list work on the board's thread, a draw or pixel
 * rectangle runs without the big lock, which a large one would otherwise
 * hold for milliseconds, keeping the processors from their interrupts and
 * the main loop from their timers. It reads and writes only board memory
 * and the state the command stream sets; meanwhile an access that would
 * touch either waits for the slice, and command-stream words queue
 * (sgi_kona_work_defer()). Returns whether the lock was let go, for
 * sgi_kona_work_relock(); a nested call keeps it let go.
 */
static bool sgi_kona_work_unlock(SGIKonaState *s)
{
    if (s->work.drawing || !sgi_head_work_may_unlock(&s->work)) {
        return false;
    }
    sgi_head_work_unlock(&s->work);
    return true;
}

static void sgi_kona_work_relock(SGIKonaState *s, bool unlocked)
{
    if (unlocked) {
        sgi_head_work_relock(&s->work);
    }
}

static void sgi_kona_gl_target(SGIKonaState *s, SGIRasterTarget *t,
                               SGIRasterRules *rules, int stencil_bits,
                               bool textured)
{
    static const SGIGLTexImage unsupported_texture = {
        .base_format = UINT32_MAX,
    };

    *rules = sgi_kona_gl_rules;
    rules->stencil_bits = stencil_bits > 0 ? stencil_bits : 0;
    s->textures.colour_table = s->context_current == SGI_KONA_CONTEXT_NONE ?
        NULL : sgi_kona_gl_table_enabled(s->state[s->context_current], 3);
    s->textures.colour_lookup = s->textures.colour_table ?
                                sgi_kona_gl_table_lookup : NULL;
    *t = (SGIRasterTarget) {
        .store_colour = sgi_kona_gl_store_colour,
        .load_colour = sgi_kona_gl_load_colour,
        .store_index = sgi_kona_gl_store_index,
        .load_depth = sgi_kona_gl_load_depth,
        .store_depth = sgi_kona_gl_store_depth,
        .load_stencil = stencil_bits == 8 ? sgi_kona_gl_load_stencil : NULL,
        .store_stencil = stencil_bits == 8 ? sgi_kona_gl_store_stencil : NULL,
        .opaque = s,
        .screen_top = s->fb_height - 1,
        .screen_rows_down = false,
        .texture = textured ? sgi_kona_texture_resolve(&s->textures) : NULL,
        /*
         * With the host GPU, frames mix Mesa or host-batch draws with
         * these, and the host batch is confirmed against them.
         */
        .mixed_frame = s->host_gpu,
    };
    if (!textured) {
        return;
    }
    /* A rejected live binding must not become an untextured draw. */
    if ((s->context_current != SGI_KONA_CONTEXT_NONE &&
         s->state[s->context_current]->pipe.texture_rebind_refused) ||
        (!s->textures.bound && s->texture_bind_valid &&
         s->texture_bind[0] != UINT32_MAX &&
         sgi_kona_texture_lookup(&s->textures, s->textures.owner,
                                 s->texture_bind[0]))) {
        t->texture = &unsupported_texture;
    }
}

/*
 * Whether context table entry i's visual has a depth buffer: its buffer
 * configuration's depth word is not 0. GL gives a window without one no
 * depth test and nothing to clear (OpenGL 1.2.1, 4.1.5 and 4.2.3); the
 * shared draw state has no field for that, so the draw and the clear
 * drop the test and the depth bit here.
 */
static bool sgi_kona_gl_has_depth(SGIKonaState *s, uint32_t i)
{
    return i < SGI_KONA_CONTEXTS && s->state[i]->pipe.depth_bits;
}

static void sgi_kona_renderer_stop(SGIKonaState *s)
{
    if (s->renderer) {
        assert(bql_locked());
        sgi_renderer_free(s->renderer);
        s->renderer = NULL;
    }
    s->renderer_generation++;
}

static void sgi_kona_mesa_dirty_plane(void *opaque, uint32_t id,
                                       uint32_t y0, uint32_t y1)
{
    SGIKonaState *s = opaque;
    uint32_t colour = id == s->plane_front_tag ? s->plane_front :
                      id == s->plane_back_tag ? s->plane_back : id;

    if (colour == s->plane_front || colour == s->plane_back) {
        sgi_kona_invalidate_plane(s,
                                     sgi_kona_plane(s, colour), y0, y1);
    }
}

static void sgi_kona_mesa_dirty_texture(void *opaque, uint32_t handle,
                                         uint32_t level, uint32_t image_id)
{
    SGIKonaState *s = opaque;

    sgi_kona_renderer_stop(s);
    (void)handle;
    (void)level;
    (void)image_id;
}

static const SGIKonaMesaDirtyOps sgi_kona_mesa_dirty_ops = {
    .plane = sgi_kona_mesa_dirty_plane,
    .texture = sgi_kona_mesa_dirty_texture,
};

static bool sgi_kona_renderer_plane(SGIKonaState *s, const SGIPlane *p,
                                    SGIPlaneFormat format, int64_t x, int64_t y,
                                    unsigned width, unsigned height)
{
    unsigned bpp = sgi_plane_bytes_per_pixel(format);
    uint64_t end;

    if (!p || p->format != format || !p->bottom_origin ||
        p->packing != (format == SGI_PLANE_RGB24 ? SGI_PACK_LE_RED_HIGH :
                                                    SGI_PACK_LE_RED_LOW) ||
        x < 0 || y < 0 || !width || !height ||
        x + width > p->width || y + height > p->height ||
        p->stride < (uint64_t)p->width * bpp ||
        p->base > s->mem.bytes_len || p->len > s->mem.bytes_len - p->base) {
        return false;
    }
    end = (y + height - 1) * p->stride + (x + width) * bpp;
    return end <= p->len;
}

static bool sgi_kona_renderer_logic_op(const SGIGLDrawState *st)
{
    return st->color_logic_op ||
           (st->blend && st->blend_equation == SGI_GL_LOGIC_OP);
}

static bool sgi_kona_renderer_texture_apply(void *opaque,
                                            const double coord[4],
                                            double colour[4])
{
    return sgi_raster_texture_fragment(opaque, coord, colour);
}

/*
 * The draw's texture staged for the legacy host batch. An unsupported
 * result sets reason: texture for sampler state the batch cannot draw,
 * host-staging for an image past the host copy's size bound.
 */
static SGIRasterTextureResult sgi_kona_renderer_texture(
    SGIKonaState *s, const SGIGLDraw *d, const SGIRasterTarget *target,
    SGIRendererTexture *texture, SGIRasterTexture **sampler, uint8_t **pixels,
    unsigned *reason)
{
    static const uint32_t formats[] = {
        SGI_GL_ALPHA, SGI_GL_LUMINANCE, SGI_GL_LUMINANCE_ALPHA,
        SGI_GL_INTENSITY, SGI_GL_RGB, SGI_GL_RGBA,
    };
    static const uint32_t wraps[] = {
        SGI_GL_REPEAT, SGI_GL_CLAMP, SGI_GL_CLAMP_TO_EDGE,
        SGI_GL_CLAMP_TO_BORDER,
    };
    static const uint32_t environments[] = {
        SGI_GL_REPLACE, SGI_GL_MODULATE, SGI_GL_DECAL, SGI_GL_BLEND,
    };
    const SGIGLDrawState *st = d->state;
    const SGIGLTexImage *image = target->texture;
    const SGIGLTexLevel *base = NULL;
    const SGIKonaTextureObject *object;
    uint64_t bytes;
    unsigned format, environment;
    SGIRasterTextureResult result;

    *reason = SGI_RENDER_REFUSAL_TEXTURE;
    /*
     * Quiet, like the sampler below: the scalar draw that confirms or
     * replaces a host draw makes the draw's reports, so an incomplete
     * image reaches the host untextured without its own.
     */
    result = sgi_raster_texture_check(st, image);
    if (result != SGI_RASTER_TEXTURE_READY) {
        return result;
    }
    if (st->texture_1d || st->texture_3d ||
        image->one_dimensional || image->volume || !image->pyramid ||
        image->shadow_compare || image->min_filter != image->mag_filter ||
        (image->min_filter != SGI_GL_NEAREST &&
         image->min_filter != SGI_GL_LINEAR)) {
        return SGI_RASTER_TEXTURE_UNSUPPORTED;
    }
    for (unsigned i = 0; i < image->pyramid->count; i++) {
        if (image->pyramid->levels[i].level == image->pyramid->base_level) {
            base = &image->pyramid->levels[i];
            break;
        }
    }
    if (!base || base->border) {
        return SGI_RASTER_TEXTURE_UNSUPPORTED;
    }
    for (format = 0; format < ARRAY_SIZE(formats); format++) {
        if (base->base_format == formats[format]) {
            break;
        }
    }
    for (environment = 0; environment < ARRAY_SIZE(environments);
         environment++) {
        if (st->texture_env_mode == environments[environment]) {
            break;
        }
    }
    if (format == ARRAY_SIZE(formats) ||
        environment == ARRAY_SIZE(environments)) {
        return SGI_RASTER_TEXTURE_UNSUPPORTED;
    }
    texture->format = format;
    texture->environment = environment;
    for (unsigned axis = 0; axis < 2; axis++) {
        uint32_t wrap = axis ? image->wrap_t : image->wrap_s;
        unsigned mode;

        for (mode = 0; mode < ARRAY_SIZE(wraps); mode++) {
            if (wrap == wraps[mode]) {
                break;
            }
        }
        if (mode == ARRAY_SIZE(wraps)) {
            return SGI_RASTER_TEXTURE_UNSUPPORTED;
        }
        texture->wrap[axis] = mode;
    }
    object = sgi_kona_texture_lookup(&s->textures, s->textures.owner,
                                     s->textures.bound_handle);
    if (!object || image != &object->image) {
        return SGI_RASTER_TEXTURE_UNSUPPORTED;
    }
    for (unsigned c = 0; c < 4; c++) {
        texture->border[c] = CLAMP(image->border_color[c], 0, 1);
        texture->env_colour[c] = CLAMP(st->texture_env_color[c], 0, 1);
        texture->scale[c] = object->params.post_scale[c];
        texture->bias[c] = object->params.post_bias[c];
    }
    bytes = (uint64_t)base->width * base->height * 4;
    if (bytes > SGI_KONA_TEXTURE_BYTES) {
        *reason = SGI_RENDER_REFUSAL_HOST_STAGING;
        return SGI_RASTER_TEXTURE_UNSUPPORTED;
    }
    result = sgi_raster_texture_prepare(st, image, sampler);
    if (result != SGI_RASTER_TEXTURE_READY) {
        return result;
    }
    sgi_raster_texture_quiet(*sampler);
    *pixels = g_try_malloc(bytes);
    if (!*pixels) {
        return SGI_RASTER_TEXTURE_OOM;
    }
    for (unsigned y = 0; y < base->height; y++) {
        for (unsigned x = 0; x < base->width; x++) {
            uint32_t rgba;
            uint8_t *pixel = *pixels + ((size_t)y * base->width + x) * 4;

            if (!base->read_texel(base->opaque, base->level, x, y, &rgba)) {
                return SGI_RASTER_TEXTURE_UNSUPPORTED;
            }
            stl_be_p(pixel, rgba);
        }
    }
    texture->serial = s->renderer_requests + 1;
    texture->width = base->width;
    texture->height = base->height;
    texture->bytes = bytes;
    texture->texels = *pixels;
    texture->linear = image->min_filter == SGI_GL_LINEAR;
    texture->apply = sgi_kona_renderer_texture_apply;
    texture->opaque = *sampler;
    return SGI_RASTER_TEXTURE_READY;
}

/*
 * One host request for a triangle in a tile, or the feature-matrix row
 * that keeps the legacy route from drawing it. Lighting and per-vertex
 * materials have no row of their own; the batch draws only unlit
 * triangles, so they count as the primitive row.
 */
static SGIRenderRefusal sgi_kona_renderer_request(
    SGIKonaState *s, const SGIGLDraw *d, const SGIVolumeVertex vertices[3],
    const float *provoking, const SGIClipRect *tile,
    SGIRendererRequest *request, SGIColourTarget *target,
    const SGIRasterRules *rules, const SGIRendererTexture *texture)
{
    const SGIGLDrawState *st = d->state;
    const SGIGLDrawable *drawable = &st->drawable;
    const uint32_t *visual;
    int bank;
    int64_t x = tile->x0, y = tile->y0;
    unsigned width = tile->x1 - tile->x0 + 1;
    unsigned height = tile->y1 - tile->y0 + 1;
    int64_t tx = x - drawable->origin[0], ty = y - drawable->origin[1];
    int64_t bounds[4] = { tx, ty, tx + width - 1, ty + height - 1 };
    bool logic_op = sgi_kona_renderer_logic_op(st);
    double window_vertices[3][3];

    if (d->context >= SGI_KONA_CONTEXTS ||
        d->context != s->context_current ||
        !s->gl_grammar ||
        !s->contexts[d->context].valid ||
        !s->state[d->context]->pipe.draw_config_received) {
        return SGI_RENDER_REFUSAL_RESOURCE;
    }
    visual = s->state[d->context]->pipe.draw_config;
    if (visual[0] || (visual[1] != 8 && visual[1] != 12) || visual[2] ||
        visual[3] > 1 || visual[4] || visual[5] ||
        visual[6] > 32 || (visual[7] != 0 && visual[7] != 8) ||
        visual[8] || visual[9]) {
        return SGI_RENDER_REFUSAL_RESOURCE;
    }
    if (d->mode != SGI_GL_TRIANGLES || d->count != 3 ||
        d->nmaterials || d->polygon_stipple_enabled ||
        d->line_stipple_enabled || d->line_phase ||
        (s->state[d->context]->gl.raster_enables &
         (SGI_GL_RASTER_LINE_STIPPLE | SGI_GL_RASTER_POLYGON_STIPPLE)) ||
        (d->polygon_modes && (d->polygon_modes[0] != SGI_GL_FILL ||
                              d->polygon_modes[1] != SGI_GL_FILL)) ||
        s->state[d->context]->gl.lighting) {
        return SGI_RENDER_REFUSAL_PRIMITIVE;
    }
    if (s->state[d->context]->gl.raster_enables) {
        return SGI_RENDER_REFUSAL_SMOOTHING;
    }
    if (d->back_colors || s->state[d->context]->gl.light_model_two_side) {
        return SGI_RENDER_REFUSAL_FACING;
    }
    if (drawable->bound != 1 || drawable->clip.n != 1 || drawable->clip.empty ||
        s->state[d->context]->window.owned_by_pixel) {
        return SGI_RENDER_REFUSAL_WINDOW;
    }
    if (st->index_bits) {
        return SGI_RENDER_REFUSAL_COLOUR_INDEX;
    }
    if (st->draw_buffers != SGI_GL_DRAW_FRONT &&
        st->draw_buffers != SGI_GL_DRAW_BACK) {
        return SGI_RENDER_REFUSAL_BUFFERS;
    }
    *target = st->draw_buffers == SGI_GL_DRAW_FRONT ? drawable->front :
                                                    drawable->back;
    bank = sgi_kona_target_bank(s, *target);
    if (bank < 0) {
        return SGI_RENDER_REFUSAL_BUFFERS;
    }
    if (width > SGI_RENDERER_SIZE || height > SGI_RENDERER_SIZE ||
        !s->mem.bytes || s->mem.pages || !s->scanout.dirty ||
        x < 0 || y < 0 || x + width > s->scanout.frame_width ||
        y + height > s->scanout.frame_height ||
        !sgi_kona_renderer_plane(s, sgi_kona_plane(s,
                                   sgi_kona_bank_plane(s, bank)),
                                  SGI_PLANE_RGB24, x, y, width, height) ||
        !sgi_kona_renderer_plane(s, sgi_kona_plane(s,
                                   sgi_kona_bank_tag_plane(s, bank)),
                                  SGI_PLANE_TAG, x, y, width, height)) {
        return SGI_RENDER_REFUSAL_RESOURCE;
    }
    if (st->shade_model != SGI_GL_FLAT && st->shade_model != SGI_GL_SMOOTH) {
        return SGI_RENDER_REFUSAL_PRIMITIVE;
    }
    if (st->colour_mask & ~15) {
        return SGI_RENDER_REFUSAL_BUFFERS;
    }
    if (st->fog) {
        return SGI_RENDER_REFUSAL_FOG;
    }
    if (st->polygon_offset & ~(SGI_GL_OFFSET_FILL | SGI_GL_OFFSET_LINE |
                               SGI_GL_OFFSET_POINT)) {
        return SGI_RENDER_REFUSAL_POLYGON_OFFSET;
    }
    if (st->texture_1d || st->texture_3d) {
        return SGI_RENDER_REFUSAL_TEXTURE;
    }
    if (st->dither) {
        return SGI_RENDER_REFUSAL_DITHER;
    }
    if (st->window_matrix_given || st->window_rows_down) {
        return SGI_RENDER_REFUSAL_WINDOW;
    }
    if (!(st->depth_range[0] >= 0 && st->depth_range[0] <= 1) ||
        !(st->depth_range[1] >= 0 && st->depth_range[1] <= 1) ||
        st->viewport[2] < 0 || st->viewport[3] < 0) {
        return SGI_RENDER_REFUSAL_GEOMETRY;
    }
    if ((st->depth_test &&
         !sgi_kona_renderer_plane(s, sgi_kona_plane(s, s->plane_depth),
                                   SGI_PLANE_Z24, x, y, width, height)) ||
        (st->stencil_test && rules->stencil_bits &&
         !sgi_kona_renderer_plane(s, sgi_kona_plane(s, s->plane_stencil),
                                   SGI_PLANE_S8, x, y, width, height))) {
        return SGI_RENDER_REFUSAL_RESOURCE;
    }
    if (st->alpha_test && st->alpha_func > SGI_GL_ALWAYS) {
        return SGI_RENDER_REFUSAL_FRAGMENT_TESTS;
    }
    if (logic_op &&
        st->logic_op_mode - SGI_GL_CLEAR > SGI_GL_SET - SGI_GL_CLEAR) {
        return SGI_RENDER_REFUSAL_LOGIC_OP;
    }
    if (st->cull_face && st->cull_face_mode != SGI_GL_FRONT &&
        st->cull_face_mode != SGI_GL_BACK &&
        st->cull_face_mode != SGI_GL_FRONT_AND_BACK) {
        return SGI_RENDER_REFUSAL_FACING;
    }
    G_STATIC_ASSERT(SGI_GL_CLIP_PLANES == SGI_RENDERER_CLIP_PLANES);
    request->clip_planes = st->clip_planes;
    request->texture = texture;
    request->smooth = st->shade_model == SGI_GL_SMOOTH;
    for (unsigned i = 0; i < 3; i++) {
        const float *vertex = vertices[i].attr;
        const double *clip_position = vertices[i].clip;
        double window[3];

        if (!isfinite(clip_position[3]) || clip_position[3] <= 0) {
            return SGI_RENDER_REFUSAL_GEOMETRY;
        }
        for (unsigned axis = 0; axis < 3; axis++) {
            if (!isfinite(clip_position[axis])) {
                return SGI_RENDER_REFUSAL_GEOMETRY;
            }
        }
        sgi_gl_clip_to_window(st, clip_position, window);
        for (unsigned axis = 0; axis < 2; axis++) {
            double position = window[axis];

            if (!isfinite(position)) {
                return SGI_RENDER_REFUSAL_GEOMETRY;
            }
            if (fabs(position) > SGI_RENDERER_COORD_LIMIT) {
                return SGI_RENDER_REFUSAL_HOST_STAGING;
            }
            request->vertices[i][axis] = round(position * 256) / 256;
        }
        request->depth[i] = CLAMP(window[2], 0, 1);
        request->vertices[i][2] = 0;
        request->vertices[i][3] = 1;
        window_vertices[i][0] = request->vertices[i][0];
        window_vertices[i][1] = request->vertices[i][1];
        window_vertices[i][2] = request->depth[i];
        if (request->smooth || texture || st->clip_planes) {
            request->inverse_w[i] = 1.0 / clip_position[3];
        }
        for (unsigned p = 0; p < SGI_GL_CLIP_PLANES; p++) {
            if (st->clip_planes & (1u << p)) {
                const float *plane = st->clip_plane[p];
                const double *eye = vertices[i].eye;

                request->clip_distance[i][p] = plane[0] * eye[0] +
                    plane[1] * eye[1] + plane[2] * eye[2] + plane[3] * eye[3];
            }
        }
        if (request->smooth || texture) {
            for (unsigned c = 0; c < 4; c++) {
                double value = request->smooth ? vertex[SGI_GL_V_COLOR + c] :
                               provoking[SGI_GL_V_COLOR + c];

                if (!isfinite(value)) {
                    return SGI_RENDER_REFUSAL_GEOMETRY;
                }
                request->colour[i][c] = CLAMP(value, 0, 1);
            }
        }
    }
    if (texture) {
        for (unsigned i = 0; i < 3; i++) {
            for (unsigned c = 0; c < 4; c++) {
                request->texcoord[i][c] =
                    vertices[i].attr[SGI_GL_V_TEXCOORD + c];
                if (!isfinite(request->texcoord[i][c])) {
                    return SGI_RENDER_REFUSAL_GEOMETRY;
                }
            }
            if (request->texcoord[i][3] <= 0) {
                return SGI_RENDER_REFUSAL_GEOMETRY;
            }
        }
    }
    request->offset = sgi_raster_polygon_offset(rules, st, window_vertices);
    if (!isfinite(request->offset)) {
        return SGI_RENDER_REFUSAL_GEOMETRY;
    }
    request->alpha = provoking[SGI_GL_V_COLOR + 3];
    if (!isfinite(request->alpha)) {
        return SGI_RENDER_REFUSAL_GEOMETRY;
    }
    for (unsigned c = 0; c < 3; c++) {
        float colour = provoking[SGI_GL_V_COLOR + c];

        if (!isfinite(colour)) {
            return SGI_RENDER_REFUSAL_GEOMETRY;
        }
        request->rgb[c] = floor(CLAMP((double)colour, 0, 1) * 255 + 0.5);
    }
    if (st->scissor_test) {
        bounds[0] = MAX(bounds[0], st->scissor[0]);
        bounds[1] = MAX(bounds[1], st->scissor[1]);
        bounds[2] = MIN(bounds[2],
                         (int64_t)st->scissor[0] + st->scissor[2] - 1);
        bounds[3] = MIN(bounds[3],
                         (int64_t)st->scissor[1] + st->scissor[3] - 1);
    }
    request->generation = s->renderer_generation;
    if (!sgi_renderer_request_tile(request, tx, ty, width, height, bounds)) {
        return SGI_RENDER_REFUSAL_HOST_STAGING;
    }
    if (!request->smooth && !texture && st->alpha_test &&
        !sgi_raster_alpha_test(rules, st->alpha_func,
                               request->alpha, st->alpha_ref)) {
        request->clip[0] = request->clip[1] = 0;
        request->clip[2] = request->clip[3] = -1;
    }
    if (st->cull_face && !sgi_renderer_request_cull(request,
            st->front_face == SGI_GL_CW,
            st->cull_face_mode == SGI_GL_FRONT ||
            st->cull_face_mode == SGI_GL_FRONT_AND_BACK,
            st->cull_face_mode == SGI_GL_BACK ||
            st->cull_face_mode == SGI_GL_FRONT_AND_BACK)) {
        return SGI_RENDER_REFUSAL_HOST_STAGING;
    }
    return SGI_RENDER_REFUSAL_NONE;
}

static unsigned sgi_kona_renderer_count(const SGIGLDraw *d)
{
    if (d->count == 3 && d->mode == SGI_GL_TRIANGLES) {
        return 1;
    }
    if (d->count >= 3 && d->mode == SGI_GL_POLYGON) {
        return d->count - 2;
    }
    if (d->count == 4 && (d->mode == SGI_GL_QUADS ||
                          d->mode == SGI_GL_QUAD_STRIP)) {
        return 2;
    }
    return 0;
}

static void sgi_kona_renderer_indices(const SGIGLDraw *d, unsigned piece,
                                      unsigned indices[4])
{
    static const unsigned triangle[4] = { 0, 1, 2, 2 };
    static const unsigned quad[2][4] = { { 0, 1, 2, 3 }, { 0, 2, 3, 3 } };
    static const unsigned strip[2][4] = { { 0, 1, 3, 3 }, { 0, 3, 2, 3 } };

    if (d->mode == SGI_GL_POLYGON) {
        indices[0] = indices[3] = 0;
        indices[1] = piece + 1;
        indices[2] = piece + 2;
    } else {
        const unsigned *source = d->mode == SGI_GL_QUADS ? quad[piece] :
                                 d->mode == SGI_GL_QUAD_STRIP ? strip[piece] :
                                 triangle;

        memcpy(indices, source, sizeof(triangle));
    }
}

typedef struct SGIKonaRendererTriangle {
    SGIVolumeVertex vertices[3];
    const float *provoking;
} SGIKonaRendererTriangle;

/*
 * The draw's triangles after view-volume clipping, or the row that keeps
 * the legacy route from drawing them. A draw clipped away entirely is
 * refused as geometry and drawn, as nothing, by the scalar path.
 */
static SGIRenderRefusal sgi_kona_renderer_triangles(
    const SGIGLDraw *d, unsigned pieces, SGIKonaRendererTriangle *triangles,
    unsigned limit, unsigned *count)
{
    unsigned indices[4];

    *count = 0;
    for (unsigned piece = 0; piece < pieces; piece++) {
        SGIVolumeVertex in[3], poly[SGI_VOLUME_TRIANGLE_MAX];
        unsigned n;

        sgi_kona_renderer_indices(d, piece, indices);
        for (unsigned v = 0; v < 3; v++) {
            sgi_volume_from_draw_vertex(d->state,
                d->vertices + indices[v] * SGI_GL_VERTEX_FLOATS, &in[v]);
            for (unsigned k = 0; k < 4; k++) {
                if (!isfinite(in[v].clip[k]) || !isfinite(in[v].eye[k])) {
                    return SGI_RENDER_REFUSAL_GEOMETRY;
                }
            }
        }
        n = sgi_volume_clip_triangle(in, poly);
        for (unsigned v = 1; v + 1 < n; v++) {
            SGIKonaRendererTriangle *triangle;

            if (*count == limit) {
                return SGI_RENDER_REFUSAL_HOST_STAGING;
            }
            triangle = &triangles[(*count)++];
            triangle->vertices[0] = poly[0];
            triangle->vertices[1] = poly[v];
            triangle->vertices[2] = poly[v + 1];
            triangle->provoking = d->vertices +
                                  indices[3] * SGI_GL_VERTEX_FLOATS;
        }
    }
    return *count ? SGI_RENDER_REFUSAL_NONE : SGI_RENDER_REFUSAL_GEOMETRY;
}

static SGIClipRect sgi_kona_renderer_tile(const SGIClipRect *bounds,
                                         unsigned columns, unsigned index)
{
    SGIClipRect tile = {
        .x0 = bounds->x0 + index % columns * SGI_RENDERER_SIZE,
        .y0 = bounds->y0 + index / columns * SGI_RENDERER_SIZE,
    };

    tile.x1 = MIN((int64_t)tile.x0 + SGI_RENDERER_SIZE - 1, bounds->x1);
    tile.y1 = MIN((int64_t)tile.y0 + SGI_RENDERER_SIZE - 1, bounds->y1);
    return tile;
}

static void sgi_kona_renderer_arm_test_fault(SGIKonaState *s)
{
    if (s->test_renderer_fail_draw) {
        sgi_renderer_test_fail_draw(s->renderer, s->test_renderer_fail_draw);
        s->test_renderer_fail_draw = 0;
    }
}

static SGIRenderResult sgi_kona_renderer_create(SGIKonaState *s,
                                                Error **errp)
{
    SGIRenderCreateResult created;

    if (s->test_renderer_fail_create) {
        created = s->test_renderer_fail_create;
        s->test_renderer_fail_create = 0;
        error_setg(errp, "Injected SGI renderer creation failure");
    } else {
        created = sgi_renderer_create(&s->renderer, errp);
    }
    switch (created) {
    case SGI_RENDER_CREATE_OK:
        assert(s->renderer);
        return SGI_RENDER_OK;
    case SGI_RENDER_CREATE_UNAVAILABLE:
        s->renderer_failed = true;
        qemu_log_mask(LOG_UNIMP, "sgi-kona: OpenGL renderer unavailable; "
                      "using scalar fallback\n");
        return SGI_RENDER_UNSUPPORTED;
    case SGI_RENDER_CREATE_OOM:
        return SGI_RENDER_OOM;
    case SGI_RENDER_CREATE_HOST_ERROR:
        s->renderer_failed = true;
        return SGI_RENDER_HOST_ERROR;
    default:
        g_assert_not_reached();
    }
}

static const char *sgi_kona_fallback_name(unsigned reason)
{
    return reason == SGI_KONA_FALLBACK_UNAVAILABLE ? "unavailable" :
           sgi_render_refusal_name(reason);
}

static void sgi_kona_count_fallback(SGIKonaState *s, unsigned reason,
                                    const SGIGLDraw *d)
{
    assert(reason > SGI_RENDER_REFUSAL_NONE &&
           reason < SGI_KONA_FALLBACK__MAX);
    s->renderer_fallbacks[reason]++;
    trace_sgi_kona_gpu_fallback(sgi_kona_fallback_name(reason), d->context,
                                d->mode);
}

/* The planes a draw's writes are held for: colour banks 0 and 1 first. */
enum {
    SGI_KONA_SHADOW_DEPTH = 2,
    SGI_KONA_SHADOW_STENCIL,
    SGI_KONA_SHADOW_LAYERS,
};

/*
 * One plane's held writes. They are stored densely over area, which is
 * allocated on the first write and grows within the shadow's bounds as
 * writes land outside it. extent is the box around what was written, so
 * comparing and committing cost what the draw touched, not the window.
 */
typedef struct SGIKonaShadowLayer {
    uint32_t *values;
    uint8_t *written;
    SGIClipRect area;           /* inclusive; valid while values is set */
    SGIClipRect extent;         /* inclusive; valid while values is set */
    size_t width;               /* of area */
} SGIKonaShadowLayer;

/*
 * One draw's colour, depth and stencil writes held aside from the planes.
 * Reads through target see them, so a draw that reads what it wrote, such
 * as blending overlapping triangles, behaves as it would on the planes.
 * Nothing reaches the planes until the draw's path is final.
 *
 * Each plane the draw writes gets a dense layer within bounds, the box
 * around the drawable's clip rectangles that the scalar path also bounds
 * its writes by, within the planes. A write the planes would drop is
 * dropped here too. A write the shadow cannot hold, a colour index or a
 * place outside bounds, marks it escaped; a layer it cannot allocate or
 * grow marks it out of memory. Host-only, never migrated.
 */
typedef struct SGIKonaShadow {
    SGIRasterTarget target;
    const SGIRasterTarget *base;
    SGIKonaState *s;
    SGIClipRect bounds;         /* inclusive; empty when x0 > x1 */
    SGIKonaShadowLayer layers[SGI_KONA_SHADOW_LAYERS];
    bool escaped;
    bool oom;
} SGIKonaShadow;

static const SGIPlane *sgi_kona_shadow_plane(SGIKonaState *s, unsigned layer)
{
    switch (layer) {
    case SGI_KONA_SHADOW_DEPTH:
        return sgi_kona_plane(s, s->plane_depth);
    case SGI_KONA_SHADOW_STENCIL:
        return sgi_kona_plane(s, s->plane_stencil);
    default:
        return sgi_kona_plane(s, sgi_kona_bank_plane(s, layer));
    }
}

/* Where (x, y) is held in a layer; false where nothing is held. */
static bool sgi_kona_shadow_at(const SGIKonaShadow *sh, unsigned layer,
                               int x, int y, size_t *at)
{
    const SGIKonaShadowLayer *l = &sh->layers[layer];

    if (!l->written ||
        x < l->extent.x0 || x > l->extent.x1 ||
        y < l->extent.y0 || y > l->extent.y1) {
        return false;
    }
    *at = (size_t)(y - l->area.y0) * l->width + (x - l->area.x0);
    return l->written[*at];
}

/*
 * Make l's area cover (x, y), which is within the shadow's bounds. A new
 * area reaches 32 pixels each way from its first write, and a grown one
 * gains at least its own width or height, and 64 pixels, on each side it
 * grows, so a draw's writes reallocate a layer a few times, not per row.
 */
static bool sgi_kona_shadow_cover(SGIKonaShadow *sh, SGIKonaShadowLayer *l,
                                  int x, int y)
{
    SGIKonaState *s = sh->s;
    SGIClipRect area;
    int64_t grow_x, grow_y;
    size_t width, pixels;
    uint32_t *values;
    uint8_t *written;

    if (l->values) {
        if (x >= l->area.x0 && x <= l->area.x1 &&
            y >= l->area.y0 && y <= l->area.y1) {
            return true;
        }
        area = l->area;
        grow_x = MAX(l->area.x1 - l->area.x0 + 1, 64);
        grow_y = MAX(l->area.y1 - l->area.y0 + 1, 64);
    } else {
        /* qtest: fail the armed layer as if the host were out of memory. */
        if (s->test_renderer_fail_hold && !--s->test_renderer_fail_hold) {
            return false;
        }
        area = (SGIClipRect) { .x0 = x, .y0 = y, .x1 = x, .y1 = y };
        grow_x = grow_y = 32;
    }
    if (!l->values || x < area.x0) {
        area.x0 = MAX((int64_t)x - grow_x, sh->bounds.x0);
    }
    if (!l->values || x > area.x1) {
        area.x1 = MIN((int64_t)x + grow_x, sh->bounds.x1);
    }
    if (!l->values || y < area.y0) {
        area.y0 = MAX((int64_t)y - grow_y, sh->bounds.y0);
    }
    if (!l->values || y > area.y1) {
        area.y1 = MIN((int64_t)y + grow_y, sh->bounds.y1);
    }
    width = area.x1 - area.x0 + 1;
    pixels = width * (size_t)(area.y1 - area.y0 + 1);
    values = g_try_new(uint32_t, pixels);
    written = g_try_malloc0(pixels);
    if (!values || !written) {
        g_free(values);
        g_free(written);
        return false;
    }
    if (l->values) {
        size_t row = l->area.x1 - l->area.x0 + 1;

        for (int64_t oy = l->area.y0; oy <= l->area.y1; oy++) {
            size_t from = (size_t)(oy - l->area.y0) * l->width;
            size_t to = (size_t)(oy - area.y0) * width +
                        (l->area.x0 - area.x0);

            memcpy(&values[to], &l->values[from], row * sizeof(*values));
            memcpy(&written[to], &l->written[from], row);
        }
        g_free(l->values);
        g_free(l->written);
    } else {
        l->extent = (SGIClipRect) { .x0 = x, .y0 = y, .x1 = x, .y1 = y };
    }
    l->values = values;
    l->written = written;
    l->area = area;
    l->width = width;
    return true;
}

static void sgi_kona_shadow_store(SGIKonaShadow *sh, unsigned layer,
                                  int x, int y, uint32_t value)
{
    const SGIPlane *p = sgi_kona_shadow_plane(sh->s, layer);
    SGIKonaShadowLayer *l = &sh->layers[layer];
    size_t at;

    if (sh->escaped || sh->oom) {
        return;
    }
    if (!p) {
        sh->escaped = true;
        return;
    }
    if (!sgi_plane_contains(p, x, y)) {
        return;
    }
    if (x < sh->bounds.x0 || x > sh->bounds.x1 ||
        y < sh->bounds.y0 || y > sh->bounds.y1) {
        sh->escaped = true;
        return;
    }
    if (!sgi_kona_shadow_cover(sh, l, x, y)) {
        sh->oom = true;
        return;
    }
    l->extent.x0 = MIN(l->extent.x0, x);
    l->extent.y0 = MIN(l->extent.y0, y);
    l->extent.x1 = MAX(l->extent.x1, x);
    l->extent.y1 = MAX(l->extent.y1, y);
    at = (size_t)(y - l->area.y0) * l->width + (x - l->area.x0);
    l->values[at] = value;
    l->written[at] = 1;
}

static void sgi_kona_shadow_store_colour(void *opaque, SGIColourTarget target,
                                         int x, int y, uint32_t colour)
{
    SGIKonaShadow *sh = opaque;
    int bank = sgi_kona_target_bank(sh->s, target);

    /* The planes store nothing for another target, and hold 24 bits. */
    if (bank >= 0) {
        sgi_kona_shadow_store(sh, bank, x, y, colour & 0x00ffffff);
    }
}

static uint32_t sgi_kona_shadow_load_colour(void *opaque,
                                            SGIColourTarget target,
                                            int x, int y)
{
    SGIKonaShadow *sh = opaque;
    int bank = sgi_kona_target_bank(sh->s, target);
    size_t at;

    if (bank >= 0 && sgi_kona_shadow_at(sh, bank, x, y, &at)) {
        return sh->layers[bank].values[at];
    }
    return sh->base->load_colour(sh->base->opaque, target, x, y);
}

/* The legacy host route refuses colour-index draws, so none is compared. */
static void sgi_kona_shadow_store_index(void *opaque, SGIColourTarget target,
                                        int x, int y, uint32_t index,
                                        uint32_t mask)
{
    SGIKonaShadow *sh = opaque;

    sh->escaped = true;
}

static uint32_t sgi_kona_shadow_load_depth(void *opaque, int x, int y)
{
    SGIKonaShadow *sh = opaque;
    size_t at;

    if (sgi_kona_shadow_at(sh, SGI_KONA_SHADOW_DEPTH, x, y, &at)) {
        return sh->layers[SGI_KONA_SHADOW_DEPTH].values[at];
    }
    return sh->base->load_depth(sh->base->opaque, x, y);
}

static void sgi_kona_shadow_store_depth(void *opaque, int x, int y,
                                        uint32_t depth)
{
    sgi_kona_shadow_store(opaque, SGI_KONA_SHADOW_DEPTH, x, y, depth);
}

static uint32_t sgi_kona_shadow_load_stencil(void *opaque, int x, int y)
{
    SGIKonaShadow *sh = opaque;
    size_t at;

    if (sgi_kona_shadow_at(sh, SGI_KONA_SHADOW_STENCIL, x, y, &at)) {
        return sh->layers[SGI_KONA_SHADOW_STENCIL].values[at];
    }
    return sh->base->load_stencil(sh->base->opaque, x, y);
}

static void sgi_kona_shadow_store_stencil(void *opaque, int x, int y,
                                          uint32_t value)
{
    sgi_kona_shadow_store(opaque, SGI_KONA_SHADOW_STENCIL, x, y, value);
}

static void sgi_kona_shadow_init(SGIKonaShadow *sh, SGIKonaState *s,
                                 const SGIRasterTarget *base,
                                 const SGIGLDrawable *drawable)
{
    int64_t width = 0, height = 0;

    /* KONA's planes take no RGBA stores and no fog curve. */
    assert(!base->store_rgba && !base->load_rgba && !base->fog_factor);
    assert(base->opaque == s);
    *sh = (SGIKonaShadow) {
        .target = *base,
        .base = base,
        .s = s,
        .bounds = { .x0 = 0, .y0 = 0, .x1 = -1, .y1 = -1 },
    };
    sh->target.opaque = sh;
    sh->target.store_colour = sgi_kona_shadow_store_colour;
    sh->target.load_colour = base->load_colour ?
                             sgi_kona_shadow_load_colour : NULL;
    sh->target.store_index = sgi_kona_shadow_store_index;
    sh->target.load_depth = sgi_kona_shadow_load_depth;
    sh->target.store_depth = sgi_kona_shadow_store_depth;
    sh->target.load_stencil = base->load_stencil ?
                              sgi_kona_shadow_load_stencil : NULL;
    sh->target.store_stencil = base->store_stencil ?
                               sgi_kona_shadow_store_stencil : NULL;

    /* sgi_soft_window()'s box, within the largest plane a write can reach. */
    if (!drawable->bound || !drawable->clip.n) {
        return;
    }
    for (unsigned i = 0; i < SGI_KONA_SHADOW_LAYERS; i++) {
        const SGIPlane *p = sgi_kona_shadow_plane(s, i);

        if (p) {
            width = MAX(width, p->width);
            height = MAX(height, p->height);
        }
    }
    sh->bounds = drawable->clip.r[0];
    for (uint32_t i = 1; i < MIN(drawable->clip.n, SGI_CLIP_MAX); i++) {
        sh->bounds.x0 = MIN(sh->bounds.x0, drawable->clip.r[i].x0);
        sh->bounds.y0 = MIN(sh->bounds.y0, drawable->clip.r[i].y0);
        sh->bounds.x1 = MAX(sh->bounds.x1, drawable->clip.r[i].x1);
        sh->bounds.y1 = MAX(sh->bounds.y1, drawable->clip.r[i].y1);
    }
    sh->bounds.x0 = MAX(sh->bounds.x0, 0);
    sh->bounds.y0 = MAX(sh->bounds.y0, 0);
    sh->bounds.x1 = MIN((int64_t)sh->bounds.x1, width - 1);
    sh->bounds.y1 = MIN((int64_t)sh->bounds.y1, height - 1);
}

static void sgi_kona_shadow_free(SGIKonaShadow *sh)
{
    for (unsigned i = 0; i < SGI_KONA_SHADOW_LAYERS; i++) {
        g_clear_pointer(&sh->layers[i].values, g_free);
        g_clear_pointer(&sh->layers[i].written, g_free);
    }
}

/* The same values written to the same places, and nothing else. */
static bool sgi_kona_shadow_equal(const SGIKonaShadow *a,
                                  const SGIKonaShadow *b)
{
    if (a->escaped || b->escaped || a->oom || b->oom) {
        return false;
    }
    for (unsigned i = 0; i < SGI_KONA_SHADOW_LAYERS; i++) {
        const SGIKonaShadowLayer *la = &a->layers[i], *lb = &b->layers[i];

        if (!la->written != !lb->written) {
            return false;
        }
        if (!la->written) {
            continue;
        }
        if (memcmp(&la->extent, &lb->extent, sizeof(la->extent))) {
            return false;
        }
        for (int64_t y = la->extent.y0; y <= la->extent.y1; y++) {
            size_t n = la->extent.x1 - la->extent.x0 + 1;
            size_t at = (size_t)(y - la->area.y0) * la->width +
                        (la->extent.x0 - la->area.x0);
            size_t bt = (size_t)(y - lb->area.y0) * lb->width +
                        (la->extent.x0 - lb->area.x0);

            if (memcmp(&la->written[at], &lb->written[bt], n)) {
                return false;
            }
            for (size_t k = 0; k < n; k++) {
                if (la->written[at + k] &&
                    la->values[at + k] != lb->values[bt + k]) {
                    return false;
                }
            }
        }
    }
    return true;
}

static void sgi_kona_shadow_commit(const SGIKonaShadow *sh)
{
    const SGIRasterTarget *base = sh->base;
    SGIKonaState *s = sh->s;

    assert(!sh->escaped && !sh->oom);
    for (unsigned i = 0; i < SGI_KONA_SHADOW_LAYERS; i++) {
        const SGIKonaShadowLayer *l = &sh->layers[i];
        SGIColourTarget target = i == 0 ? s->plane_front : s->plane_back;

        if (!l->written) {
            continue;
        }
        for (int y = l->extent.y0; y <= l->extent.y1; y++) {
            for (int x = l->extent.x0; x <= l->extent.x1; x++) {
                size_t at = (size_t)(y - l->area.y0) * l->width +
                            (x - l->area.x0);

                if (!l->written[at]) {
                    continue;
                }
                switch (i) {
                case SGI_KONA_SHADOW_DEPTH:
                    base->store_depth(base->opaque, x, y, l->values[at]);
                    break;
                case SGI_KONA_SHADOW_STENCIL:
                    base->store_stencil(base->opaque, x, y, l->values[at]);
                    break;
                default:
                    base->store_colour(base->opaque, target, x, y,
                                       l->values[at]);
                    break;
                }
            }
        }
    }
}

/*
 * Commit a legacy host draw, held in host, only when the scalar path
 * writes exactly the same values to exactly the same places; otherwise
 * commit the scalar draw. The legacy route validates its staged pixels
 * against the renderer's own model, which is not the scalar path, so this
 * is what lets its pixels share a frame with scalar draws. The scalar draw
 * runs once either way, so its once-per-draw reports are made once. When
 * the scalar writes cannot be held for lack of memory, nothing is
 * committed and the draw is out of memory.
 */
static SGIGLDrawResult sgi_kona_renderer_confirm(SGIKonaState *s,
                                                 const SGIRasterRules *rules,
                                                 const SGIRasterTarget *t,
                                                 const SGIGLDraw *d,
                                                 const SGIKonaShadow *host,
                                                 bool *confirmed)
{
    SGIKonaShadow scalar;
    SGIGLDrawResult ret;

    sgi_kona_shadow_init(&scalar, s, t, &d->state->drawable);
    ret = sgi_soft_draw(rules, &scalar.target, d);
    *confirmed = ret == SGI_GL_DRAW_OK &&
                 sgi_kona_shadow_equal(host, &scalar);
    if (*confirmed) {
        sgi_kona_shadow_commit(host);
    } else if (scalar.oom) {
        ret = SGI_GL_DRAW_OUT_OF_MEMORY;
    } else if (!scalar.escaped) {
        sgi_kona_shadow_commit(&scalar);
    } else {
        /* Unreachable: the scalar path writes only inside the bounds. */
        ret = sgi_soft_draw(rules, t, d);
    }
    sgi_kona_shadow_free(&scalar);
    return ret;
}

/*
 * The legacy host batch. On success its writes are held in out for
 * sgi_kona_renderer_confirm(), not stored on the planes. A refusal sets
 * reason.
 */
static SGIRenderResult sgi_kona_renderer_draw(SGIKonaState *s,
                                            const SGIGLDraw *d,
                                            const SGIRasterRules *rules,
                                            const SGIRasterTarget *t,
                                            const SGIRasterTarget *out,
                                            unsigned *reason, Error **errp)
{
    SGIRendererRequest *requests;
    g_autofree SGIKonaRendererTriangle *triangles = NULL;
    g_autofree uint8_t *texture_pixels = NULL;
    SGIRendererTexture texture = { 0 };
    g_autoptr(SGIRasterTexture) sampler = NULL;
    SGIColourTarget target;
    const SGIGLDrawable *drawable = &d->state->drawable;
    const SGIPlane *plane;
    SGIClipRect bounds;
    unsigned count, columns, rows, tiles;
    uint64_t tile_count;
    uint32_t colour_mask;
    int bank;
    unsigned texture_reason;
    SGIGLDraw triangle = *d;
    SGIRenderResult result = SGI_RENDER_UNSUPPORTED;
    bool logic_op = sgi_kona_renderer_logic_op(d->state);
    bool blend = d->state->blend && !logic_op;
    SGIRasterTextureResult texture_result;

    *reason = SGI_KONA_FALLBACK_UNAVAILABLE;
    if (!s->host_gpu) {
        return SGI_RENDER_UNSUPPORTED;
    }
    assert(bql_locked());
    if (s->renderer_failed) {
        return SGI_RENDER_UNSUPPORTED;
    }
    count = sgi_kona_renderer_count(d);
    if (!count) {
        *reason = SGI_RENDER_REFUSAL_PRIMITIVE;
        return SGI_RENDER_UNSUPPORTED;
    }
    if (count > 256) {
        *reason = SGI_RENDER_REFUSAL_HOST_STAGING;
        return SGI_RENDER_UNSUPPORTED;
    }
    if (drawable->bound != 1 || drawable->clip.n != 1 || drawable->clip.empty ||
        s->state[d->context]->window.owned_by_pixel) {
        *reason = SGI_RENDER_REFUSAL_WINDOW;
        return SGI_RENDER_UNSUPPORTED;
    }
    target = d->state->draw_buffers == SGI_GL_DRAW_FRONT ? drawable->front :
                                                         drawable->back;
    bank = sgi_kona_target_bank(s, target);
    if (bank < 0) {
        *reason = SGI_RENDER_REFUSAL_BUFFERS;
        return SGI_RENDER_UNSUPPORTED;
    }
    plane = sgi_kona_plane(s, sgi_kona_bank_plane(s, bank));
    if (!plane) {
        *reason = SGI_RENDER_REFUSAL_RESOURCE;
        return SGI_RENDER_UNSUPPORTED;
    }
    bounds = drawable->clip.r[0];
    bounds.x0 = MAX(bounds.x0, 0);
    bounds.y0 = MAX(bounds.y0, 0);
    bounds.x1 = MIN((int64_t)bounds.x1, (int64_t)plane->width - 1);
    bounds.y1 = MIN((int64_t)bounds.y1, (int64_t)plane->height - 1);
    if (bounds.x0 > bounds.x1 || bounds.y0 > bounds.y1) {
        *reason = SGI_RENDER_REFUSAL_WINDOW;
        return SGI_RENDER_UNSUPPORTED;
    }
    columns = DIV_ROUND_UP(bounds.x1 - bounds.x0 + 1, SGI_RENDERER_SIZE);
    rows = DIV_ROUND_UP(bounds.y1 - bounds.y0 + 1, SGI_RENDERER_SIZE);
    tile_count = (uint64_t)columns * rows;
    /* Bound staging across all triangles and tiles before allocating. */
    if (tile_count > 256 / count) {
        *reason = SGI_RENDER_REFUSAL_HOST_STAGING;
        return SGI_RENDER_UNSUPPORTED;
    }
    texture_result = sgi_kona_renderer_texture(s, d, t, &texture, &sampler,
                                                &texture_pixels,
                                                &texture_reason);
    switch (texture_result) {
    case SGI_RASTER_TEXTURE_READY:
    case SGI_RASTER_TEXTURE_DISABLED:
    case SGI_RASTER_TEXTURE_INCOMPLETE:
        break;
    case SGI_RASTER_TEXTURE_UNSUPPORTED:
        *reason = texture_reason;
        return SGI_RENDER_UNSUPPORTED;
    case SGI_RASTER_TEXTURE_OOM:
        return SGI_RENDER_OOM;
    default:
        g_assert_not_reached();
    }
    triangles = g_try_new(SGIKonaRendererTriangle, 256 / tile_count);
    if (!triangles) {
        return SGI_RENDER_OOM;
    }
    *reason = sgi_kona_renderer_triangles(d, count, triangles,
                                          256 / tile_count, &count);
    if (*reason != SGI_RENDER_REFUSAL_NONE) {
        return SGI_RENDER_UNSUPPORTED;
    }
    tiles = tile_count;
    count *= tiles;
    requests = g_try_new0(SGIRendererRequest, count);
    if (!requests) {
        return SGI_RENDER_OOM;
    }
    triangle.mode = SGI_GL_TRIANGLES;
    triangle.count = 3;
    for (unsigned i = 0; i < count; i++) {
        const SGIKonaRendererTriangle *piece = &triangles[i / tiles];
        SGIClipRect tile = sgi_kona_renderer_tile(&bounds, columns, i % tiles);

        *reason = sgi_kona_renderer_request(s, &triangle, piece->vertices,
                                            piece->provoking, &tile,
                                            &requests[i], &target, rules,
                                            texture_pixels ? &texture : NULL);
        if (*reason != SGI_RENDER_REFUSAL_NONE) {
            goto out;
        }
    }
    s->renderer_requests += count;
    if (!s->renderer) {
        Error *create_err = NULL;

        result = sgi_kona_renderer_create(s, &create_err);
        if (result != SGI_RENDER_OK) {
            *reason = SGI_KONA_FALLBACK_UNAVAILABLE;
            error_propagate(errp, create_err);
            goto out;
        }
        error_free(create_err);
    }
    sgi_kona_renderer_arm_test_fault(s);
    switch (sgi_renderer_draw_batch_checked(s->renderer, requests, count,
                                            errp)) {
    case SGI_RENDER_DRAW_OK:
        break;
    case SGI_RENDER_DRAW_REFUSED:
        /* A precision refusal draws in software and keeps the renderer. */
        *reason = SGI_RENDER_REFUSAL_PRECISION;
        result = SGI_RENDER_UNSUPPORTED;
        goto out;
    case SGI_RENDER_DRAW_FAILED:
        s->renderer_failed = true;
        result = SGI_RENDER_HOST_ERROR;
        goto out;
    default:
        g_assert_not_reached();
    }
    /* Do not commit native pixels until every host request succeeds. */
    colour_mask = sgi_raster_colour_mask(rules,
                                         d->state->colour_mask);
    for (unsigned i = 0; i < count; i++) {
        SGIRendererRequest *request = &requests[i];
        SGIClipRect tile = sgi_kona_renderer_tile(&bounds, columns, i % tiles);
        double alpha = request->alpha;

        assert(request->generation == s->renderer_generation);
        for (unsigned y = 0; y < SGI_RENDERER_SIZE; y++) {
            for (unsigned x = 0; x < SGI_RENDERER_SIZE; x++) {
                const uint8_t *pixel = request->rgba +
                                      (y * SGI_RENDERER_SIZE + x) * 4;

                if (pixel[3]) {
                    uint32_t colour = (pixel[0] << 16) |
                                      (pixel[1] << 8) | pixel[2];

                    if (request->texture ||
                        (request->smooth && (d->state->alpha_test || blend))) {
                        double rgba[4];

                        if (!sgi_renderer_fragment(request, x, y, rgba)) {
                            g_assert_not_reached();
                        }
                        alpha = rgba[3];
                        if (d->state->alpha_test &&
                            !sgi_raster_alpha_test(rules,
                                d->state->alpha_func, alpha,
                                d->state->alpha_ref)) {
                            continue;
                        }
                    }
                    if (d->state->depth_test || d->state->stencil_test) {
                        double z, window;

                        if (!sgi_renderer_depth(request, x, y, &z)) {
                            g_assert_not_reached();
                        }
                        if (!sgi_raster_window_depth(d->state, z,
                                                     request->offset,
                                                     &window) ||
                            !sgi_raster_depth_stencil(rules, d->state, out,
                                tile.x0 + x, tile.y0 + y, window)) {
                            continue;
                        }
                    }
                    if (!colour_mask) {
                        continue;
                    }
                    if (logic_op || blend || colour_mask != 0xffffff) {
                        uint32_t old = out->load_colour(out->opaque, target,
                                                        tile.x0 + x,
                                                        tile.y0 + y);

                        if (logic_op) {
                            colour = sgi_x_logic_op(
                                d->state->logic_op_mode - SGI_GL_CLEAR,
                                colour, old) & 0xffffff;
                        } else if (blend) {
                            colour = sgi_raster_blend_rgb(rules,
                                d->state, colour, alpha, old);
                        }
                        colour = (colour & colour_mask) | (old & ~colour_mask);
                    }
                    if (s->test_renderer_skew) {
                        /* qtest: a host draw that differs from scalar. */
                        colour ^= 1;
                        s->test_renderer_skew = false;
                    }
                    out->store_colour(out->opaque, target,
                                      tile.x0 + x, tile.y0 + y, colour);
                }
            }
        }
    }
    result = SGI_RENDER_OK;
out:
    g_free(requests);
    return result;
}

/*
 * Attempt a draw on Mesa for a caller that draws a refusal on the scalar
 * path within the same frame, so only state the scalar comparisons prove
 * is accepted. A renderer that cannot be created on this host refuses
 * with no reason; any other creation failure fails.
 */
static SGIRenderDrawResult sgi_kona_mesa_draw(SGIKonaState *s,
                                              const SGIRasterRules *rules,
                                              const SGIGLDraw *d,
                                              SGIRenderAttemptOutcome *outcome,
                                              Error **errp)
{
    static const SGIRenderAttemptOptions opts = { .mixed_frame = true };
    SGIRasterTarget target;
    SGIRenderDrawResult result;
    const SGIRenderResource *resources;
    size_t count;

    if (!s->renderer) {
        outcome->status = sgi_kona_renderer_create(s, errp);
        outcome->reason = SGI_RENDER_REFUSAL_NONE;
        if (outcome->status == SGI_RENDER_UNSUPPORTED) {
            return SGI_RENDER_DRAW_REFUSED;
        }
        if (outcome->status != SGI_RENDER_OK) {
            return SGI_RENDER_DRAW_FAILED;
        }
    }
    sgi_kona_renderer_arm_test_fault(s);
    resources = sgi_kona_mesa_resources(s->mesa_provider, &count);
    target = (SGIRasterTarget) {
        .opaque = s->mesa_provider,
        .resources = resources,
        .resource_count = count,
        .resource_ops = sgi_kona_mesa_resource_ops(),
    };
    sgi_kona_mesa_refresh_board_views(s->mesa_provider);
    result = sgi_render_draw_attempt(s->renderer, rules, &target, d, &opts,
                                     outcome, errp);
    if (result != SGI_RENDER_DRAW_REFUSED) {
        s->renderer_requests++;
    }
    return result;
}

static void sgi_kona_mesa_stop(SGIKonaState *s, Error *err)
{
    s->mesa_failed = true;
    if (err) {
        error_report_err(err);
    } else {
        error_report("sgi-kona: Mesa draw failed without a diagnostic");
    }
    assert(bql_locked());
    qemu_system_vmstop_request_prepare();
    qemu_system_vmstop_request(RUN_STATE_INTERNAL_ERROR);
}

static bool sgi_kona_mesa_identity(const float matrix[16])
{
    /* Other transforms retain the established raster path until qualified. */
    for (unsigned i = 0; i < 16; i++) {
        if (matrix[i] != (i % 5 == 0 ? 1.0f : 0.0f)) {
            return false;
        }
    }
    return true;
}

static SGIGLDrawResult sgi_kona_gl_draw_checked(void *opaque,
                                                const SGIGLDraw *d)
{
    SGIKonaState *s = opaque;
    SGIRasterTarget t;
    SGIRasterRules rules;
    int stencil_bits = sgi_kona_gl_stencil_bits(s, d->context);
    SGIGLDrawState st;
    SGIGLDraw copy;
    SGIGLDrawResult ret;
    SGIRenderResult legacy_result;
    SGIKonaShadow host;
    unsigned reason;
    Error *err = NULL;
    bool unlocked;

    if (s->mesa_failed) {
        return SGI_GL_DRAW_OK;
    }

    if (d->state->stencil_test && stencil_bits < 0) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: stencil draw of context %" PRIu32
                      " refused for unavailable or unsupported visual\n",
                      d->context);
        return SGI_GL_DRAW_OK;
    }
    sgi_kona_gl_target(s, &t, &rules, stencil_bits,
                       d->state->texture_1d || d->state->texture_2d ||
                       d->state->texture_3d);
    if (d->state->depth_test && !sgi_kona_gl_has_depth(s, d->context)) {
        st = *d->state;
        st.depth_test = 0;
        copy = *d;
        copy.state = &st;
        d = &copy;
    }
    /*
     * The Mesa route writes board memory itself, bounded by the clip list
     * alone, so a window owning more pixels than the list holds
     * (owned_by_pixel) stays on the scalar path, whose stores test them.
     */
    if (s->host_gpu && s->mesa_provider && !s->renderer_failed &&
        !s->state[d->context]->window.owned_by_pixel &&
        d->mode == SGI_GL_TRIANGLES && d->count == 3 &&
        d->state->shade_model == SGI_GL_SMOOTH &&
        d->state->draw_buffers == SGI_GL_DRAW_FRONT &&
        d->state->drawable.front == s->plane_front &&
        sgi_kona_mesa_identity(d->state->modelview) &&
        sgi_kona_mesa_identity(d->state->projection) &&
        !d->state->texture_1d && !d->state->texture_2d &&
        !d->state->texture_3d &&
        (!d->polygon_modes ||
         (d->polygon_modes[0] == SGI_GL_FILL &&
          d->polygon_modes[1] == SGI_GL_FILL))) {
        SGIGLDraw checked = *d;
        SGIRenderAttemptOutcome outcome;

        checked.polygon_modes = NULL;
        switch (sgi_kona_mesa_draw(s, &rules, &checked, &outcome, &err)) {
        case SGI_RENDER_DRAW_OK:
            s->renderer_draws++;
            error_free(err);
            return SGI_GL_DRAW_OK;
        case SGI_RENDER_DRAW_REFUSED:
            /*
             * Nothing visible happened, so the scalar path draws it here,
             * before the borrowed draw data expires. It never goes on to
             * the legacy host batch, whose pixels mixed frames cannot use.
             */
            error_free(err);
            reason = outcome.reason != SGI_RENDER_REFUSAL_NONE ?
                     outcome.reason : SGI_KONA_FALLBACK_UNAVAILABLE;
            goto scalar;
        case SGI_RENDER_DRAW_FAILED:
            if (outcome.status == SGI_RENDER_OOM) {
                error_free(err);
                return SGI_GL_DRAW_OUT_OF_MEMORY;
            }
            /* Accepted work failed; a scalar retry would hide it. */
            sgi_kona_mesa_stop(s, err);
            return SGI_GL_DRAW_OK;
        default:
            g_assert_not_reached();
        }
    }
    sgi_kona_shadow_init(&host, s, &t, &d->state->drawable);
    legacy_result = sgi_kona_renderer_draw(s, d, &rules, &t, &host.target,
                                           &reason, &err);
    if (legacy_result == SGI_RENDER_OK && host.oom) {
        /* The host writes could not be held; nothing reached the planes. */
        legacy_result = SGI_RENDER_OOM;
    }
    if (legacy_result == SGI_RENDER_OK) {
        bool confirmed;

        error_free(err);
        ret = sgi_kona_renderer_confirm(s, &rules, &t, d, &host, &confirmed);
        sgi_kona_shadow_free(&host);
        if (confirmed) {
            s->renderer_draws++;
            return SGI_GL_DRAW_OK;
        }
        if (ret != SGI_GL_DRAW_OUT_OF_MEMORY) {
            sgi_kona_count_fallback(s, SGI_RENDER_REFUSAL_PRECISION, d);
        }
        goto report;
    }
    sgi_kona_shadow_free(&host);
    switch (legacy_result) {
    case SGI_RENDER_OOM:
        error_free(err);
        return SGI_GL_DRAW_OUT_OF_MEMORY;
    case SGI_RENDER_UNSUPPORTED:
        error_free(err);
        break;
    case SGI_RENDER_RESOURCE_ERROR:
    case SGI_RENDER_HOST_ERROR:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: legacy renderer draw "
                      "failed: %s\n", err ? error_get_pretty(err) :
                                           "no backend diagnostic");
        sgi_kona_mesa_stop(s, err);
        return SGI_GL_DRAW_OK;
    default:
        g_assert_not_reached();
    }
scalar:
    if (s->host_gpu) {
        sgi_kona_count_fallback(s, reason, d);
    }
    sgi_kona_invalidate_defer(s);
    unlocked = sgi_kona_work_unlock(s);
    ret = sgi_soft_draw(&rules, &t, d);
    sgi_kona_work_relock(s, unlocked);
    sgi_kona_invalidate_flush(s);
report:
    if (ret != SGI_GL_DRAW_OK) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: GL draw of context %" PRIu32
                      " not completed by the rasterizer\n", d->context);
    }
    return ret;
}

static void sgi_kona_gl_draw(void *opaque, const SGIGLDraw *d)
{
    SGIKonaState *s = opaque;

    if (sgi_kona_gl_draw_checked(s, d) == SGI_GL_DRAW_OUT_OF_MEMORY) {
        sgi_kona_gl_set_error(s, SGI_GL_OUT_OF_MEMORY);
    }
}

static void sgi_kona_gl_raster(void *opaque, const SGIRasterOp *op)
{
    SGIKonaState *s = opaque;
    SGIRasterTarget t;
    SGIRasterRules rules;
    SGIRasterOp copy;
    SGIGLDrawState state;
    int stencil_bits = sgi_kona_gl_stencil_bits(s, s->context_current);
    bool unlocked;

    if (op->kind == SGI_RASTER_PIXELS && op->pixels.stencil &&
        stencil_bits != 8) {
        if (!stencil_bits) {
            sgi_kona_gl_set_error(s, 0x502);
        }
        qemu_log_mask(LOG_UNIMP, "sgi-kona: stencil pixels require a "
                      "supported stencil visual\n");
        return;
    }
    if ((op->clear_buffers & SGI_GL_STENCIL_BUFFER_BIT) && stencil_bits < 0) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: stencil clear refused for "
                      "unavailable or unsupported visual\n");
        return;
    }
    if (op->kind == SGI_RASTER_PIXELS && op->pixels.depth &&
        op->pixels.state->stencil_test && stencil_bits < 0) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: depth pixels require a supported "
                      "stencil visual\n");
        return;
    }
    sgi_kona_gl_target(s, &t, &rules, stencil_bits, true);
    if ((op->clear_buffers & SGI_GL_DEPTH_BUFFER_BIT) &&
        (s->context_current == SGI_KONA_CONTEXT_NONE ||
         !sgi_kona_gl_has_depth(s, s->context_current))) {
        copy = *op;
        copy.clear_buffers &= ~SGI_GL_DEPTH_BUFFER_BIT;
        op = &copy;
    }
    if (op->kind == SGI_RASTER_PIXELS && op->pixels.depth &&
        !sgi_kona_gl_has_depth(s, s->context_current)) {
        copy = *op;
        state = *op->pixels.state;
        state.depth_test = 0;
        copy.pixels.state = &state;
        op = &copy;
    }
    sgi_kona_invalidate_defer(s);
    unlocked = sgi_kona_work_unlock(s);
    sgi_soft_raster(&rules, &t, op);
    sgi_kona_work_relock(s, unlocked);
    sgi_kona_invalidate_flush(s);
}

/*
 * glFinish's fence. Every draw has been stored by the time the context
 * sends it, so the finish completes at once: word 0 of the return page,
 * the one thing the client waits on.
 */
static void sgi_kona_gl_fence(void *opaque, uint32_t context)
{
    SGIKonaState *s = opaque;

    if (!s->mesa_failed) {
        sgi_kona_post_return(s, SGI_KONA_CMD_GL_FINISH, 0, NULL, 0);
    }
}

static void sgi_kona_gl_draw_nothing(void *opaque, const SGIGLDraw *d)
{
}

static SGIGLDrawResult sgi_kona_gl_draw_checked_nothing(void *opaque,
                                                        const SGIGLDraw *d)
{
    return SGI_GL_DRAW_OK;
}

static void sgi_kona_gl_raster_nothing(void *opaque, const SGIRasterOp *op)
{
}

/*
 * A window depth as a hit record carries it, round(z * (2^32 - 1)), the
 * scaling OpenGL gives selection depths (the rounding is the model's
 * choice; the pipe's is unknown).
 */
static uint32_t sgi_kona_gl_select_depth(double z)
{
    return llround(MIN(MAX(z, 0.0), 1.0) * (double)UINT32_MAX);
}

/*
 * A primitive that hit in selection mode: the current context's pending
 * hit takes in its depths, until the name stack next changes and
 * sgi_kona_gl_select_flush() writes it out.
 */
static void sgi_kona_gl_select_hit(void *opaque, double minimum,
                                   double maximum)
{
    SGIKonaState *s = opaque;
    SGIKonaGLRenderMode *r = &s->state[s->context_current]->render;
    uint32_t low = sgi_kona_gl_select_depth(minimum);
    uint32_t high = sgi_kona_gl_select_depth(maximum);

    r->hit_min = r->hit ? MIN(r->hit_min, low) : low;
    r->hit_max = r->hit ? MAX(r->hit_max, high) : high;
    r->hit = true;
}

static SGIGLDrawResult sgi_kona_gl_select_draw_checked(void *opaque,
                                                       const SGIGLDraw *d)
{
    switch (sgi_gl_select_draw(d, sgi_kona_gl_select_hit, opaque)) {
    case SGI_GL_SELECT_UNSUPPORTED_STATE:
        qemu_log_mask(LOG_UNIMP, "sgi-kona: selection ignores unsupported "
                      "GL geometry state\n");
        break;
    case SGI_GL_SELECT_UNSUPPORTED_MODE:
        qemu_log_mask(LOG_UNIMP, "sgi-kona: selection ignores unsupported "
                      "primitive mode 0x%x\n", d->mode);
        break;
    case SGI_GL_SELECT_OK:
        break;
    }
    return SGI_GL_DRAW_OK;
}

static void sgi_kona_gl_select_draw(void *opaque, const SGIGLDraw *d)
{
    sgi_kona_gl_select_draw_checked(opaque, d);
}

/*
 * The current context's sink. In feedback and selection modes no
 * fragment reaches the framebuffer (OpenGL 1.2.1, sections 5.2 and 5.3),
 * so every draw, pixel and bitmap goes nowhere. So does a clear, which
 * the specification does not name; Mesa skips it the same way (model
 * choice; what a real pipe does with a clear in these modes is unknown).
 * In selection mode a primitive's draw finds its hit instead, and so
 * does a valid raster position (sgi_kona_gl_run()). Limitation: a bitmap
 * or a pixel rectangle at a valid raster position makes no hit of its
 * own; whether the pipe makes one is unknown. The fence still answers
 * glFinish.
 */
static void sgi_kona_gl_sink(SGIKonaState *s, SGIGLSink *sink)
{
    int i = s->context_current;

    *sink = (SGIGLSink) {
        .draw = sgi_kona_gl_draw,
        .raster = sgi_kona_gl_raster,
        .fence = sgi_kona_gl_fence,
        .opaque = s,
        .draw_checked = sgi_kona_gl_draw_checked,
    };
    if (i != SGI_KONA_CONTEXT_NONE &&
        s->state[i]->render.mode != SGI_KONA_GL_RENDER) {
        bool select = s->state[i]->render.mode == SGI_KONA_GL_SELECT;

        sink->draw = select ? sgi_kona_gl_select_draw :
                              sgi_kona_gl_draw_nothing;
        sink->raster = sgi_kona_gl_raster_nothing;
        sink->draw_checked = select ? sgi_kona_gl_select_draw_checked :
                                      sgi_kona_gl_draw_checked_nothing;
    }
}

/*
 * Point context table entry i's GL context at its window and banks: see
 * SGIKonaGLWindow. A single-buffered context's front is bank 0 and it has
 * no back; a double-buffered one's front is the bank its window shows and
 * its back the other.
 */
static void sgi_kona_gl_set_drawable(SGIKonaState *s, unsigned i)
{
    SGIKonaGLWindow *w = &s->state[i]->window;
    const SGIKonaGLPipe *p = &s->state[i]->pipe;
    unsigned front = w->double_buffered ? w->displayed_bank : 0;
    SGIGLDrawable d = {
        .bound = w->placed,
        .origin = { w->x, w->y },
        .front = sgi_kona_bank_plane(s, front),
        .back = w->double_buffered ? sgi_kona_bank_plane(s, !front) :
                                     SGI_PLANE_NONE,
    };
    SGIClipRect r;

    /* A visual above the main planes draws into its level's planes. */
    if (p->draw_config_received && p->draw_config[0]) {
        d.front = sgi_kona_gl_overlay_config(p->draw_config) ?
                  SGI_KONA_GL_OVERLAY_TARGET : SGI_PLANE_NONE;
        d.back = SGI_PLANE_NONE;
    }

    w->owned_generation = s->display_id_generation;
    w->owned_by_pixel = false;
    if (!sgi_kona_gl_window_rect(s, w, &r)) {
        d.clip.empty = 1;
    } else if (!sgi_kona_gl_window_tests_id(w)) {
        d.clip.r[0] = r;
        d.clip.n = 1;
    } else if (!sgi_kona_gl_owned_clip(s, w, &r, &d.clip)) {
        /*
         * The whole rectangle bounds the draw, and each store keeps to
         * the pixels showing the window's ID (sgi_kona_gl_pixel_owned()).
         * A root-window client such as twilight owns the screen less
         * every window above it, far more than SGI_CLIP_MAX rectangles;
         * drawing its whole rectangle overwrote those windows, which the
         * X server does not repaint when the client exits.
         */
        d.clip = (SGIClipList) { .n = 1, .r[0] = r };
        w->owned_by_pixel = true;
    }
    sgi_gl_set_drawable(&s->state[i]->gl, &d);
}

/*
 * The buffer swap of double-buffered context i, at the retrace that
 * honours the kernel's 0x1e7 for it: its window shows the bank it has
 * been drawing into, and its back buffer becomes the bank shown until
 * now. The scanout repaints every line after a swap record.
 *
 * Inferred: that a default swap takes effect at the retrace that
 * completes its 0x1e7, which the kernel sends for every swap but a
 * decoupled one; which retrace the hardware flips at is unknown. A swap
 * the kernel completes without 0x1e7 (the decoupled mode) exchanges
 * nothing here. What 0x1f4 and 0x1e6, sent with the swap, select is
 * unknown, and the model keeps the bank itself.
 */
static void sgi_kona_gl_swap(SGIKonaState *s, unsigned i)
{
    s->state[i]->window.displayed_bank ^= 1;
    trace_sgi_kona_gl_swap(i, s->state[i]->window.displayed_bank);
    sgi_kona_gl_set_drawable(s, i);
}

/* Prepare complete native defaults before committing a logical generation. */
/* No filter, none enabled, filter scale 1 and bias 0 (EXT_convolution). */
/*
 * No histogram and no minmax table. A minmax table starts, and resets, to
 * a minimum of 1 and a maximum of 0. Approximated: EXT_histogram sets them
 * to the largest and smallest values the table can hold; the pipe's are
 * unknown, and the components it compares lie in [0, 1].
 */
static void sgi_kona_gl_minmax_reset(SGIKonaGLHistogram *h)
{
    for (unsigned c = 0; c < 4; c++) {
        h->min[c] = 0x3f800000;
        h->max[c] = 0;
    }
}

static void sgi_kona_gl_histogram_init(SGIKonaGLHistogram *h)
{
    memset(h, 0, sizeof(*h));
    sgi_kona_gl_minmax_reset(h);
}

/* No table, scale 1 and bias 0 (SGI_color_table). */
static void sgi_kona_gl_tables_init(SGIKonaGLColourTables *tb)
{
    memset(tb, 0, sizeof(*tb));
    for (unsigned n = 0; n < SGI_KONA_GL_TABLES; n++) {
        for (unsigned c = 0; c < 4; c++) {
            tb->table[n].scale[c] = 0x3f800000;
        }
    }
}

static void sgi_kona_gl_imaging_init(SGIKonaGLImaging *im)
{
    memset(im, 0, sizeof(*im));
    for (unsigned f = 0; f < SGI_KONA_GL_FILTERS; f++) {
        for (unsigned c = 0; c < 4; c++) {
            im->filters[f].scale[c] = 0x3f800000;
        }
    }
    for (unsigned k = 0; k < 4; k++) {
        im->colour_matrix[k * 5] = 0x3f800000;
    }
}

static void sgi_kona_gl_state_init(SGIKonaState *s, SGIKonaGLState *state,
                                    unsigned slot)
{
    SGIGLSink sink;
    SGIGLDrawable drawable = {
        .front = sgi_kona_bank_plane(s, 0),
        .back = SGI_PLANE_NONE,
        .clip.empty = 1,
    };

    sgi_gl_init(&state->gl, slot);
    /*
     * glPolygonMode's outlines and points and stippled lines go through
     * the shared context's checked draw path, which this sink has.
     */
    sgi_kona_gl_sink(s, &sink);
    sgi_gl_enable_polygon_modes(&state->gl, &sink);
    sgi_gl_enable_line_stipple(&state->gl, &sink);
    memset(&state->window, 0, sizeof(state->window));
    state->pipe = (SGIKonaGLPipe) {
        .pixel_zoom = { 0x3f800000, 0x3f800000 },
        .pixel_transfer.scale = { 0x3f800000, 0x3f800000,
                                  0x3f800000, 0x3f800000 },
        .pixel_transfer.depth_scale = 0x3f800000,
        .pixel_transfer.post_conv_scale = { 0x3f800000, 0x3f800000,
                                            0x3f800000, 0x3f800000 },
        .pixel_transfer.post_matrix_scale = { 0x3f800000, 0x3f800000,
                                              0x3f800000, 0x3f800000 },
        .unpack_alignment = 4,
        .pack_alignment = 4,
        .returns.depth_range = { 0, UINT64_C(0x3ff0000000000000) },
        .returns.clear_depth = UINT64_C(0x3ff0000000000000),
        .multisample_enables = 1,       /* GL_MULTISAMPLE_SGIS starts on */
        .sample_mask_value = 0x3f800000,
        .sample_pattern = SGI_KONA_GL_1PASS_SGIS,
    };
    state->iris = (SGIKonaGLIrisRule) { 0 };
    state->attrib_depth = 0;
    sgi_kona_gl_imaging_init(&state->imaging);
    sgi_kona_gl_tables_init(&state->tables);
    sgi_kona_gl_histogram_init(&state->histogram);
    state->render = (SGIKonaGLRenderMode) { .mode = SGI_KONA_GL_RENDER };
    memset(state->imaging_enabled_attrib, 0,
           sizeof(state->imaging_enabled_attrib));
    sgi_gl_set_drawable(&state->gl, &drawable);
}

/* The current context's index, or SGI_KONA_CONTEXT_NONE with a log. */
static int sgi_kona_gl_current(SGIKonaState *s, uint32_t selector)
{
    if (s->context_current == SGI_KONA_CONTEXT_NONE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: GL token 0x%x with no context current;"
                      " ignored\n", selector);
    }
    return s->context_current;
}

/* One past the latest placement of any context's window. */
static uint64_t sgi_kona_next_place_serial(SGIKonaState *s)
{
    uint64_t serial = 0;

    for (unsigned j = 0; j < SGI_KONA_CONTEXTS; j++) {
        if (s->state[j]) {
            serial = MAX(serial, s->state[j]->window.place_serial);
        }
    }
    return serial + 1;
}

/*
 * 0x1f3 and 0x1ee: the current context's window, as SGIKonaGLWindow
 * reads their words. The context keeps it until the next one, since the
 * kernel's context activation sends neither on a restore it does not mark
 * stale.
 */
static void sgi_kona_gl_window(SGIKonaState *s, uint32_t selector,
                               const uint32_t *args)
{
    int i = sgi_kona_gl_current(s, selector);
    SGIKonaGLWindow *w;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    w = &s->state[i]->window;
    w->x = args[0];
    w->y = args[1];
    if (selector == SGI_KONA_CMD_CONTEXT_RECORDS) {
        w->display_id = args[9];
        w->partial = args[10];
        if (w->partial && w->display_id > SGI_KONA_DISPLAY_ID_MAX &&
            w->display_id != UINT32_MAX) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: context %d's window with"
                          " display ID 0x%x outside the main planes keeps"
                          " its rectangle\n", i, w->display_id);
        }
        if (!w->placed) {
            /* Retirement clears placement, retaining the context visual. */
            w->double_buffered = s->state[i]->pipe.draw_config_received &&
                                 s->state[i]->pipe.draw_config[3] != 0;
            w->displayed_bank = 0;
        }
        w->width = args[6];
        w->height = args[7];
        w->placed = 1;
        w->place_serial = sgi_kona_next_place_serial(s);
    }
    trace_sgi_kona_gl_window(selector, i, w->x, w->y, w->width, w->height,
                             w->display_id, w->partial);
    sgi_kona_gl_set_drawable(s, i);
}

/*
 * The accumulation and auxiliary-buffer words a visual may carry: none, or
 * the 25-bit accumulation buffer and the one auxiliary buffer that the
 * Small and Medium visual tables offer (established from the IRIX X
 * server's tables). The accumulation buffer is modelled
 * (sgi_kona_gl_accum()); an auxiliary buffer is accepted with the visual,
 * and drawing into one stays refused by glDrawBuffer.
 */
static bool sgi_kona_gl_extra_buffers(const uint32_t *v)
{
    return (v[SGI_KONA_GL_BUFFER_CONFIG_ACCUM] == 0 ||
            v[SGI_KONA_GL_BUFFER_CONFIG_ACCUM] == 25) &&
           v[SGI_KONA_GL_BUFFER_CONFIG_AUX] <= 1;
}

/*
 * The RGB visuals whose colour glReadPixels reads: 8-, 10- and 12-bit
 * components, all kept in the eight-bit planes; 4 or 8 samples, drawn
 * single-sampled (see sgi_kona_gl_stencil_bits()); a stencil of none, 1 bit
 * (GLperf's visual is 0 12 0 1 0 0 23 1 0 0) or 8, which does not change
 * where colour is read from, as for index visuals; and the extra buffers
 * above. Not the physical visual layout.
 */
static bool sgi_kona_gl_rgb_config(const uint32_t *v)
{
    return v[0] == 0 && (v[1] == 8 || v[1] == 10 || v[1] == 12) &&
           v[2] == 0 && v[3] <= 1 && v[4] == 0 &&
           (v[5] == 0 || v[5] == 4 || v[5] == 8) && v[6] <= 32 &&
           (v[7] == 0 || v[7] == 1 || v[7] == 8) &&
           sgi_kona_gl_extra_buffers(v);
}

/*
 * Reuse the desktop's 16-bit index storage for mono main GL visuals. Their
 * stencil is none, an 8-bit one, or the 1-bit stencil of IRIS GL index
 * visuals with 23 depth bits (sysmeter's 0 0 12 1 0 0 23 1 0 0). Whether
 * stencil operations run on it is sgi_kona_gl_stencil_bits()'s decision;
 * colour-index drawing does not depend on it.
 */
static bool sgi_kona_gl_index_config(const uint32_t *v)
{
    return !v[0] && !v[1] && (v[2] == 8 || v[2] == 12) && v[3] <= 1 &&
           !v[4] && !v[5] && v[6] <= 32 &&
           (v[7] == 0 || v[7] == 1 || v[7] == 8) &&
           sgi_kona_gl_extra_buffers(v);
}

static bool sgi_kona_gl_colour_config(const uint32_t *v)
{
    return sgi_kona_gl_rgb_config(v) || sgi_kona_gl_index_config(v);
}

static bool sgi_kona_gl_read_compatible(const SGIKonaGLPipe *p)
{
    return p->draw_config_received && p->draw_config_valid &&
           (!p->read_config_received ||
            (p->read_config_valid &&
             !memcmp(p->draw_config, p->read_config, sizeof(p->draw_config))));
}

/* Only a complete framed packet replaces a visual record. */
static void sgi_kona_gl_config(SGIKonaGLPipe *p, bool read,
                               const uint32_t *args)
{
    if (read) {
        memcpy(p->read_config, args, sizeof(p->read_config));
        p->read_config_received = true;
        p->read_config_valid = sgi_kona_gl_colour_config(args);
    } else {
        memcpy(p->draw_config, args, sizeof(p->draw_config));
        p->draw_config_received = true;
        p->draw_config_valid = sgi_kona_gl_colour_config(args);
    }
    p->read_compatible = sgi_kona_gl_read_compatible(p);
}

/*
 * The buffer configuration's double-buffer word, for the current context.
 * GL draws a double-buffered context into its back buffer until told
 * otherwise, and a single-buffered one into its front (OpenGL 1.1,
 * 4.2.1); the client sends no draw-buffer command of its own for the
 * default.
 */
static void sgi_kona_gl_buffer_config(SGIKonaState *s, const uint32_t *args)
{
    int i = sgi_kona_gl_current(s, SGI_KONA_CMD_GL_BUFFER_CONFIG);
    uint32_t buffers;
    SGIGLSink sink;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    sgi_kona_gl_config(&s->state[i]->pipe, false, args);
    sgi_gl_set_index_mode(&s->state[i]->gl,
                           sgi_kona_gl_index_config(args) ||
                           sgi_kona_gl_overlay_config(args) ? args[2] : 0);
    if (args[0] && !sgi_kona_gl_overlay_config(args)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: context %d's visual at level %d"
                      " (index %u, RGB %u, double %u) is not modelled; it"
                      " draws nothing\n", i, (int32_t)args[0], args[2],
                      args[1], args[3]);
    }
    s->state[i]->window.double_buffered =
        args[SGI_KONA_GL_BUFFER_CONFIG_DOUBLE] != 0;
    s->state[i]->pipe.depth_bits = args[SGI_KONA_GL_BUFFER_CONFIG_DEPTH];
    buffers = s->state[i]->window.double_buffered ? SGI_GL_DRAW_BACK :
                                                SGI_GL_DRAW_FRONT;
    s->state[i]->pipe.returns.draw_buffer = 0;
    sgi_kona_gl_sink(s, &sink);
    sgi_gl_command(&s->state[i]->gl, &sink,
                   &(SGIGLCmd) { SGI_GL_OP_DRAW_BUFFERS, 1, &buffers });
    sgi_kona_gl_set_drawable(s, i);
}

/*
 * The GL client's 0xc6: 1 from glFinish, which waits for word 0 of the
 * return page to go non-zero and reads nothing else; 0 from a buffer
 * swap, which waits for nothing. Posting for a swap would leave word 0
 * set, and the next glGetError's wait would end at once on that stale
 * word, so a swap posts nothing (inferred from the two waits). Any other
 * word has no known sender and is logged. glFinish goes through the
 * current context to its fence, sgi_kona_gl_fence(); with none current
 * it still completes, since the client waits either way.
 */
static void sgi_kona_gl_finish(SGIKonaState *s, uint32_t wait)
{
    SGIGLSink sink;

    if (s->mesa_failed) {
        return;
    }

    switch (wait) {
    case 0:
        trace_sgi_kona_command_unmodelled(SGI_KONA_CMD_GL_FINISH, 1);
        break;
    case 1:
        if (s->context_current == SGI_KONA_CONTEXT_NONE) {
            sgi_kona_post_return(s, SGI_KONA_CMD_GL_FINISH, 0, NULL, 0);
            break;
        }
        sgi_kona_gl_sink(s, &sink);
        sgi_gl_command(&s->state[s->context_current]->gl, &sink,
                       &(SGIGLCmd) { SGI_GL_OP_FINISH, 0, NULL });
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: token 0x%x with word 0x%" PRIx32 ", which"
                      " no known sender sends; nothing posted\n",
                      SGI_KONA_CMD_GL_FINISH, wait);
    }
}

/*
 * IRIS GL's colour and normal rule: a vertex whose colour came after the
 * last normal is not lit (SGI's OpenGL Porting Guide, 007-1797-030,
 * appendix B, "Color/normal flag"). The IRIX 6.5.30 n32 IP27KONA
 * the GL client library (SHA-256 1639e2c6...352f) leaves it to the pipe: its
 * glIglooInterfaceSGIX sends GL_IGLOO_COLORNORMAL_SGIX as 0x1be and
 * GL_IGLOO_LMC_COLOR_SGIX as 0x113, each with the caller's parameter word, and
 * keeps no state of its own (established from the listing). Inferred: the rule
 * holds while the last colour-normal word is nonzero and the last LMC colour
 * word is 1, the values for which the MGRAS GL library of the same
 * release turns the rule on.
 *
 * While the rule holds and lighting is enabled in an RGBA context, the
 * model lights a vertex whose colour came after the last normal with
 * ambient, diffuse and specular (0, 0, 0, 1) on both faces and the
 * current colour as both emissions, then restores the materials. That is
 * what the Octane2 VPro library does in software for the same rule.
 * Approximated: how the pipe carries out the rule is unknown; this
 * differs from leaving the vertex unlit only in its alpha, which the wrap
 * makes 1. A colour-index context is lit as before: what the rule does to
 * lit colour indices is unknown.
 */
static bool sgi_kona_gl_iris_unlit(SGIKonaGLState *state)
{
    return state->iris.colour_normal && state->iris.lmc == 1 &&
           state->iris.colour_last && state->gl.lighting &&
           !state->gl.cur.index_bits;
}

static void sgi_kona_gl_iris_material(SGIKonaGLState *state, SGIGLSink *sink,
                                      uint32_t face, uint32_t pname,
                                      const float v[4])
{
    uint32_t word[6] = { face, pname };
    SGIGLCmd cmd = { SGI_GL_OP_MATERIAL, 6, word };

    memcpy(&word[2], v, 4 * sizeof(float));
    sgi_gl_command(&state->gl, sink, &cmd);
}

static void sgi_kona_gl_iris_vertex(SGIKonaGLState *state, SGIGLSink *sink,
                                    const SGIGLCmd *cmd)
{
    static const float none[4] = { 0, 0, 0, 1 };
    static const uint32_t faces[2] = { SGI_GL_FRONT, SGI_GL_BACK };
    SGIGLMaterial saved[2];
    float colour[4];

    memcpy(saved, state->gl.material, sizeof(saved));
    memcpy(colour, state->gl.cur_color, sizeof(colour));
    sgi_kona_gl_iris_material(state, sink, SGI_GL_FRONT_AND_BACK,
                              SGI_GL_AMBIENT, none);
    sgi_kona_gl_iris_material(state, sink, SGI_GL_FRONT_AND_BACK,
                              SGI_GL_DIFFUSE, none);
    sgi_kona_gl_iris_material(state, sink, SGI_GL_FRONT_AND_BACK,
                              SGI_GL_SPECULAR, none);
    sgi_kona_gl_iris_material(state, sink, SGI_GL_FRONT_AND_BACK,
                              SGI_GL_EMISSION, colour);
    sgi_gl_command(&state->gl, sink, cmd);
    for (int f = 0; f < 2; f++) {
        sgi_kona_gl_iris_material(state, sink, faces[f], SGI_GL_AMBIENT,
                                  saved[f].ambient);
        sgi_kona_gl_iris_material(state, sink, faces[f], SGI_GL_DIFFUSE,
                                  saved[f].diffuse);
        sgi_kona_gl_iris_material(state, sink, faces[f], SGI_GL_SPECULAR,
                                  saved[f].specular);
        sgi_kona_gl_iris_material(state, sink, faces[f], SGI_GL_EMISSION,
                                  saved[f].emission);
    }
}

/* One command for context i's GL state, under IRIS GL's rule. */
static void sgi_kona_gl_run(SGIKonaState *s, int i, SGIGLSink *sink,
                            const SGIGLCmd *cmd)
{
    SGIKonaGLState *state = s->state[i];

    switch (cmd->op) {
    case SGI_GL_OP_COLOR3F:
    case SGI_GL_OP_COLOR4F:
    case SGI_GL_OP_COLOR3UB:
    case SGI_GL_OP_COLOR4UB:
        state->iris.colour_last = true;
        break;
    case SGI_GL_OP_NORMAL3F:
        state->iris.colour_last = false;
        break;
    case SGI_GL_OP_VERTEX2F:
    case SGI_GL_OP_VERTEX3F:
    case SGI_GL_OP_VERTEX4F:
    case SGI_GL_OP_VERTEX2I:
        if (sgi_kona_gl_iris_unlit(state)) {
            sgi_kona_gl_iris_vertex(state, sink, cmd);
            return;
        }
        break;
    default:
        break;
    }
    sgi_gl_command(&state->gl, sink, cmd);
    switch (cmd->op) {
    case SGI_GL_OP_RASTER_POS2F:
    case SGI_GL_OP_RASTER_POS3F:
    case SGI_GL_OP_RASTER_POS4F:
        /*
         * In selection mode a raster position inside the view volume
         * makes a hit at its window depth, as OpenGL 1.2.1 section 5.2
         * has it, and the geometry program's selection has a path that
         * records one (inferred from its microcode: the KONA geometry
         * datasheet's feedback and selection section).
         */
        if (state->render.mode == SGI_KONA_GL_SELECT &&
            !state->gl.in_begin && state->gl.raster_valid &&
            i == s->context_current) {
            sgi_kona_gl_select_hit(s, state->gl.raster_pos[2],
                                   state->gl.raster_pos[2]);
        }
        break;
    default:
        break;
    }
}

/*
 * The GL client's drawing and make-current state, onto the current
 * context (hw/display/sgi/gl-context.c), whose draws reach the colour
 * banks through the shared rasterizer:
 *
 *   0xe3 x y w h     glViewport
 *   0xc x y w h      glScissor; the box is kept, the test stays off
 *   0x25 cap         glDisable of the capability's enumerant
 *   0x1d r g b a     glClearColor, four floats
 *   0x1a mask        glClear; only the colour buffers are modelled
 *   0xc0 mode        glShadeModel
 *   0x63 r g b       glColor3f, alpha 1
 *   0x57 + mode, w   glBegin; the word's meaning is unknown and unused
 *   0x8c x y         glVertex2f, z 0 and w 1
 *   0x71 w           glEnd, the word unused
 *   0xc7 w           glFlush, the word unused; pieces are already drawn
 *
 * Established, from the senders: each selector's words. The matrices
 * are never loaded by the client this was read from, so they stay the
 * identity GL starts with.
 */
static void sgi_kona_gl_accum(SGIKonaState *s, int i, uint32_t op,
                              const uint32_t *clear, float value);

static void sgi_kona_gl_command(SGIKonaState *s, uint32_t selector,
                                const uint32_t *args, unsigned argc)
{
    int i = sgi_kona_gl_current(s, selector);
    uint32_t word;
    SGIGLSink sink;
    SGIGLCmd cmd = { .args = args, .nargs = argc };

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    switch (selector) {
    case SGI_KONA_CMD_GL_VIEWPORT:
        cmd.op = SGI_GL_OP_VIEWPORT;
        break;
    case SGI_KONA_CMD_GL_SCISSOR:
        cmd.op = SGI_GL_OP_SCISSOR;
        break;
    case SGI_KONA_CMD_GL_DISABLE:
        cmd.op = SGI_GL_OP_DISABLE;
        break;
    case SGI_KONA_CMD_GL_CLEAR_COLOR:
        cmd.op = SGI_GL_OP_CLEAR_COLOR;
        break;
    case SGI_KONA_CMD_GL_CLEAR:
        word = args[0] & (SGI_GL_COLOR_BUFFER_BIT | SGI_GL_DEPTH_BUFFER_BIT |
                          SGI_GL_STENCIL_BUFFER_BIT);
        if (args[0] & SGI_GL_ACCUM_BUFFER_BIT) {
            sgi_kona_gl_accum(s, i, 0, s->state[i]->pipe.accum_clear, 0);
        }
        if (args[0] & ~(word | SGI_GL_ACCUM_BUFFER_BIT)) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: glClear of buffers 0x%"
                          PRIx32 "; only colour, depth, stencil and"
                          " accumulation buffers are modelled\n", args[0]);
        }
        cmd = (SGIGLCmd) { SGI_GL_OP_CLEAR, 1, &word };
        break;
    case SGI_KONA_CMD_GL_SHADE_MODEL:
        cmd.op = SGI_GL_OP_SHADE_MODEL;
        break;
    case SGI_KONA_CMD_GL_COLOR3:
        cmd.op = SGI_GL_OP_COLOR3F;
        break;
    case SGI_KONA_CMD_GL_BEGIN_FIRST ... SGI_KONA_CMD_GL_BEGIN_LAST:
        word = selector - SGI_KONA_CMD_GL_BEGIN_FIRST;
        cmd = (SGIGLCmd) { SGI_GL_OP_BEGIN, 1, &word };
        break;
    case SGI_KONA_CMD_GL_VERTEX2:
        cmd.op = SGI_GL_OP_VERTEX2F;
        break;
    case SGI_KONA_CMD_GL_END:
        cmd = (SGIGLCmd) { SGI_GL_OP_END, 0, NULL };
        break;
    case SGI_KONA_CMD_GL_FLUSH:
        cmd = (SGIGLCmd) { SGI_GL_OP_FLUSH, 0, NULL };
        break;
    default:
        g_assert_not_reached();
    }
    trace_sgi_kona_gl_command(selector, i);
    sgi_kona_gl_sink(s, &sink);
    sgi_kona_gl_run(s, i, &sink, &cmd);
}

/* A slot and 33 transport words, published only after complete decoding. */
static void sgi_kona_gl_tex_table_clear(SGIKonaState *s)
{
    s->gl_tex_table_remaining = 0;
    s->gl_tex_table_slot = 0;
    memset(s->gl_tex_table_words, 0, sizeof(s->gl_tex_table_words));
    s->gl_tex_table_refused = false;
}

static void sgi_kona_gl_tex_table_open(SGIKonaState *s, uint32_t slot)
{
    sgi_kona_gl_tex_table_clear(s);
    trace_sgi_kona_gl_tex_table(slot);
    s->gl_tex_table_slot = slot;
    s->gl_tex_table_refused = slot >= SGI_KONA_TEXTURE_CURVE_SLOTS;
    s->gl_tex_table_remaining = SGI_KONA_GL_TEX_TABLE_WORDS;
}

/* Arbitrary coefficient words cannot be interpreted as command selectors. */
static void sgi_kona_gl_tex_table_word(SGIKonaState *s, uint32_t word)
{
    unsigned index = SGI_KONA_GL_TEX_TABLE_WORDS - s->gl_tex_table_remaining;

    s->gl_tex_table_words[index] = word;
    if (--s->gl_tex_table_remaining == 0) {
        if (s->gl_tex_table_refused ||
            !sgi_kona_texture_curve_upload(&s->textures, s->gl_tex_table_slot,
                                            s->gl_tex_table_words)) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: invalid texture filter table "
                          "slot %u; stored coefficients unchanged\n",
                          s->gl_tex_table_slot);
            trace_sgi_kona_command_unmodelled(SGI_KONA_CMD_GL_TEX_TABLE,
                                              1 + SGI_KONA_GL_TEX_TABLE_WORDS);
        } else {
            s->texture_state_used = true;
        }
        sgi_kona_gl_tex_table_clear(s);
    }
}

/* A float's bits, as the shared context takes a float argument. */
static uint32_t sgi_kona_gl_bits(float f)
{
    uint32_t bits;

    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static float sgi_kona_gl_float(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

/*
 * A double the client stored with one 8-byte store, as the two words it
 * reaches the page as, as single precision: the shared context computes
 * in single precision. That the high word comes first is inferred from
 * the big-endian store; what precision the geometry engine keeps is
 * unknown.
 */
static uint32_t sgi_kona_gl_double(const uint32_t *w)
{
    uint64_t bits = ((uint64_t)w[0] << 32) | w[1];
    double d;

    memcpy(&d, &bits, sizeof(d));
    return sgi_kona_gl_bits(d);
}

/* Retain each finite depth value before the renderer narrows it. */
static bool sgi_kona_gl_native_depth(const uint32_t *args, uint64_t *out,
                                     unsigned count)
{
    uint64_t bits[2];

    for (unsigned k = 0; k < count; k++) {
        double d;

        bits[k] = ((uint64_t)args[2 * k] << 32) | args[2 * k + 1];
        if ((bits[k] & UINT64_C(0x7ff0000000000000)) ==
            UINT64_C(0x7ff0000000000000)) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: nonfinite depth setter;"
                          " state unchanged\n");
            return false;
        }
        memcpy(&d, &bits[k], sizeof(d));
        if (d < 0) {
            bits[k] = 0;
        } else if (d > 1) {
            bits[k] = UINT64_C(0x3ff0000000000000);
        }
    }
    memcpy(out, bits, count * sizeof(*out));
    return true;
}

/*
 * Signed integer colour and normal components as OpenGL 1.2.1's table
 * 2.6 maps them to [-1, 1]: (2c + 1) / (2^b - 1) for b bits. Unsigned
 * ones are c / (2^b - 1).
 */
static uint32_t sgi_kona_gl_signed(int64_t c, unsigned bits)
{
    return sgi_kona_gl_bits((2.0 * c + 1) / ((1ull << bits) - 1));
}

static uint32_t sgi_kona_gl_unsigned(uint64_t c, unsigned bits)
{
    return sgi_kona_gl_bits((double)c / ((1ull << bits) - 1));
}

/* An integer vertex or texture coordinate is converted as it is. */
static uint32_t sgi_kona_gl_int(int32_t v)
{
    return sgi_kona_gl_bits(v);
}

/*
 * glLighti and glLightModeli: the colours (ambient, diffuse, specular and
 * the light model's ambient) are signed integer colours, the rest are
 * converted as they are (OpenGL 1.2.1, 2.13.2).
 */
static void sgi_kona_gl_int_params(uint32_t pname, const uint32_t *in,
                                   uint32_t *out)
{
    bool colour = pname == SGI_GL_AMBIENT || pname == SGI_GL_DIFFUSE ||
                  pname == SGI_GL_SPECULAR ||
                  pname == SGI_GL_LIGHT_MODEL_AMBIENT;

    for (int k = 0; k < 4; k++) {
        out[k] = colour ? sgi_kona_gl_signed((int32_t)in[k], 32) :
                          sgi_kona_gl_int(in[k]);
    }
}

/*
 * A matrix command while the matrix mode is GL_COLOR (0x1800): the pipe
 * keeps the colour matrix with the other matrices, since the library
 * keeps none of them (as below), and applies it to image transfers
 * (sgi_kona_gl_after_convolution()). The commands act on it as on any matrix
 * (SGI_color_matrix). Returns false for a command that is not a matrix
 * command, which then goes on as usual.
 */
static bool sgi_kona_gl_colour_matrix(SGIKonaState *s, int i,
                                      const SGIGLCmd *cmd)
{
    SGIKonaGLImaging *im = &s->state[i]->imaging;
    float m[16];

    switch (cmd->op) {
    case SGI_GL_OP_PUSH_MATRIX:
    case SGI_GL_OP_POP_MATRIX:
    case SGI_GL_OP_LOAD_MATRIX:
    case SGI_GL_OP_LOAD_IDENTITY:
    case SGI_GL_OP_MULT_MATRIX:
    case SGI_GL_OP_TRANSLATE:
    case SGI_GL_OP_SCALE:
    case SGI_GL_OP_ROTATE:
    case SGI_GL_OP_FRUSTUM:
    case SGI_GL_OP_ORTHO:
        break;
    default:
        return false;
    }
    if (s->state[i]->gl.in_begin) {
        sgi_kona_gl_set_error(s, 0x502);
        return true;
    }
    if (cmd->op == SGI_GL_OP_PUSH_MATRIX) {
        if (im->colour_depth >= SGI_KONA_GL_COLOUR_STACK - 1) {
            sgi_kona_gl_set_error(s, 0x503);    /* GL_STACK_OVERFLOW */
            return true;
        }
        memcpy(im->colour_stack[im->colour_depth++], im->colour_matrix,
               sizeof(im->colour_matrix));
        return true;
    }
    if (cmd->op == SGI_GL_OP_POP_MATRIX) {
        if (!im->colour_depth) {
            sgi_kona_gl_set_error(s, 0x504);    /* GL_STACK_UNDERFLOW */
            return true;
        }
        memcpy(im->colour_matrix, im->colour_stack[--im->colour_depth],
               sizeof(im->colour_matrix));
        return true;
    }
    memcpy(m, im->colour_matrix, sizeof(m));
    sgi_gl_matrix_apply(m, cmd);
    memcpy(im->colour_matrix, m, sizeof(m));
    return true;
}

/*
 * The GL client's transform, vertex attribute and lighting commands, onto
 * the current context's shared operations: the matrix family 0xcf to
 * 0xdf, glRect's begin, the vertices, colours, normals, texture
 * coordinates, edge flag and raster positions in every framed form, and
 * glLight, glLightModel, glMaterial, glColorMaterial and glEnable. The
 * pipe keeps the matrices and lights the vertices: the library keeps
 * neither (established from its senders, which only write the caller's
 * arguments). Doubles become single floats, integer and packed forms
 * convert by OpenGL's rules, and a light's enumerant becomes its number.
 * Returns false for a selector this does not carry out.
 */
static bool sgi_kona_gl_transform(SGIKonaState *s, int i, uint32_t selector,
                                  const uint32_t *args)
{
    uint32_t a[16];
    uint32_t hi = s->gl_token_high;
    SGIGLCmd cmd = { .args = a };
    SGIGLSink sink;

    /*
     * The matrix family, depth range included, is not legal between a
     * begin and its end. The firmware's token map marks such tokens kind
     * 0, and the pipe diverts one arriving inside a begin to a routine that
     * records an invalid operation and discards its payload by count
     * (established from the firmware; that the hardware does the divert
     * is inferred). The payload is already framed here, so only the
     * error is left to do.
     */
    if (selector >= SGI_KONA_CMD_GL_DEPTH_RANGE &&
        selector <= SGI_KONA_CMD_GL_TRANSLATEF && s->state[i]->gl.in_begin) {
        sgi_kona_gl_set_error(s, 0x502);    /* GL_INVALID_OPERATION */
        return true;
    }

    switch (selector) {
    case SGI_KONA_CMD_GL_MATRIX_MODE:
        cmd = (SGIGLCmd) { SGI_GL_OP_MATRIX_MODE, 1, args };
        break;
    case SGI_KONA_CMD_GL_LOAD_IDENTITY:
        cmd = (SGIGLCmd) { SGI_GL_OP_LOAD_IDENTITY, 0, NULL };
        break;
    case SGI_KONA_CMD_GL_PUSH_MATRIX:
        cmd = (SGIGLCmd) { SGI_GL_OP_PUSH_MATRIX, 0, NULL };
        break;
    case SGI_KONA_CMD_GL_POP_MATRIX:
        cmd = (SGIGLCmd) { SGI_GL_OP_POP_MATRIX, 0, NULL };
        break;
    case SGI_KONA_CMD_GL_LOAD_MATRIXF:
    case SGI_KONA_CMD_GL_MULT_MATRIXF:
        cmd.op = selector == SGI_KONA_CMD_GL_LOAD_MATRIXF ?
                 SGI_GL_OP_LOAD_MATRIX : SGI_GL_OP_MULT_MATRIX;
        cmd.nargs = 16;
        cmd.args = args;
        break;
    case SGI_KONA_CMD_GL_LOAD_MATRIXD:
    case SGI_KONA_CMD_GL_MULT_MATRIXD:
        cmd.op = selector == SGI_KONA_CMD_GL_LOAD_MATRIXD ?
                 SGI_GL_OP_LOAD_MATRIX : SGI_GL_OP_MULT_MATRIX;
        cmd.nargs = 16;
        break;
    case SGI_KONA_CMD_GL_FRUSTUM:
        cmd.op = SGI_GL_OP_FRUSTUM;
        cmd.nargs = 6;
        break;
    case SGI_KONA_CMD_GL_ORTHO:
        cmd.op = SGI_GL_OP_ORTHO;
        cmd.nargs = 6;
        break;
    case SGI_KONA_CMD_GL_ROTATED:
        cmd.op = SGI_GL_OP_ROTATE;
        cmd.nargs = 4;
        break;
    case SGI_KONA_CMD_GL_SCALED:
    case SGI_KONA_CMD_GL_TRANSLATED:
        cmd.op = selector == SGI_KONA_CMD_GL_SCALED ? SGI_GL_OP_SCALE :
                                                      SGI_GL_OP_TRANSLATE;
        cmd.nargs = 3;
        break;
    case SGI_KONA_CMD_GL_DEPTH_RANGE:
        if (!s->state[i]->gl.in_begin &&
            !sgi_kona_gl_native_depth(args,
                                      s->state[i]->pipe.returns.depth_range,
                                       2)) {
            return true;
        }
        cmd.op = SGI_GL_OP_DEPTH_RANGE;
        cmd.nargs = 2;
        break;
    case SGI_KONA_CMD_GL_ROTATEF:
        cmd = (SGIGLCmd) { SGI_GL_OP_ROTATE, 4, args };
        break;
    case SGI_KONA_CMD_GL_SCALEF:
        cmd = (SGIGLCmd) { SGI_GL_OP_SCALE, 3, args };
        break;
    case SGI_KONA_CMD_GL_TRANSLATEF:
        cmd = (SGIGLCmd) { SGI_GL_OP_TRANSLATE, 3, args };
        break;
    case SGI_KONA_CMD_GL_RECT:
        /* The four vertices and the end follow as commands of their own. */
        a[0] = SGI_GL_POLYGON;
        cmd = (SGIGLCmd) { SGI_GL_OP_BEGIN, 1, a };
        break;
    case SGI_KONA_CMD_GL_VERTEX3F:
        cmd = (SGIGLCmd) { SGI_GL_OP_VERTEX3F, 3, args };
        break;
    case SGI_KONA_CMD_GL_VERTEX4F:
        cmd = (SGIGLCmd) { SGI_GL_OP_VERTEX4F, 4, args };
        break;
    case SGI_KONA_CMD_GL_VERTEX2D:
    case SGI_KONA_CMD_GL_VERTEX3D:
    case SGI_KONA_CMD_GL_VERTEX4D:
        cmd.nargs = selector == SGI_KONA_CMD_GL_VERTEX2D ? 2 :
                    selector == SGI_KONA_CMD_GL_VERTEX3D ? 3 : 4;
        cmd.op = SGI_GL_OP_VERTEX2F + cmd.nargs - 2;
        break;
    case SGI_KONA_CMD_GL_VERTEX2I:
    case SGI_KONA_CMD_GL_VERTEX3I:
    case SGI_KONA_CMD_GL_VERTEX4I:
        cmd.nargs = selector == SGI_KONA_CMD_GL_VERTEX2I ? 2 :
                    selector == SGI_KONA_CMD_GL_VERTEX3I ? 3 : 4;
        cmd.op = SGI_GL_OP_VERTEX2F + cmd.nargs - 2;
        for (int k = 0; k < cmd.nargs; k++) {
            a[k] = sgi_kona_gl_int(args[k]);
        }
        break;
    case SGI_KONA_CMD_GL_VERTEX2S:
        /* x << 16 | 0x8e, then y << 16. */
        a[0] = sgi_kona_gl_int((int16_t)(hi >> 16));
        a[1] = sgi_kona_gl_int((int16_t)(args[0] >> 16));
        cmd = (SGIGLCmd) { SGI_GL_OP_VERTEX2F, 2, a };
        break;
    case SGI_KONA_CMD_GL_VERTEX3S:
    case SGI_KONA_CMD_GL_VERTEX4S:
        /*
         * x << 16 | 0x92, then y << 16 | z; the four-short form's w in
         * the top half of its third word, by the same packing (inferred).
         */
        a[0] = sgi_kona_gl_int((int16_t)(hi >> 16));
        a[1] = sgi_kona_gl_int((int16_t)(args[0] >> 16));
        a[2] = sgi_kona_gl_int((int16_t)args[0]);
        a[3] = sgi_kona_gl_int((int16_t)(args[1] >> 16));
        cmd.op = selector == SGI_KONA_CMD_GL_VERTEX3S ? SGI_GL_OP_VERTEX3F :
                                                        SGI_GL_OP_VERTEX4F;
        cmd.nargs = selector == SGI_KONA_CMD_GL_VERTEX3S ? 3 : 4;
        break;
    case SGI_KONA_CMD_GL_INDEXD:
        cmd.op = SGI_GL_OP_INDEXF;
        cmd.nargs = 1;
        break;
    case SGI_KONA_CMD_GL_INDEXF:
        cmd = (SGIGLCmd) { SGI_GL_OP_INDEXF, 1, args };
        break;
    case SGI_KONA_CMD_GL_INDEXI:
        a[0] = sgi_kona_gl_int(args[0]);
        cmd = (SGIGLCmd) { SGI_GL_OP_INDEXF, 1, a };
        break;
    case SGI_KONA_CMD_GL_INDEX_PACKED:
        /*
         * glIndexs sends s << 16 and glIndexub b << 24, so a word whose
         * bits 23:0 are zero fits both. Approximated: such a word is read
         * as the short, the form the IRIS GL library imports (it does not
         * import glIndexub). The pipe's own rule is unknown; a capture or
         * its documentation would settle it. Bits 15:0 fit neither sender.
         */
        if (args[0] & 0xffff) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: unsupported packed index type;"
                          " current index preserved\n");
            return true;
        }
        if (args[0] && !(args[0] & 0xff0000) &&
            !s->packed_index_short_reported) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: packed index 0x%08" PRIx32
                          " fits glIndexs and glIndexub; read as the short"
                          " (reported once)\n", args[0]);
            s->packed_index_short_reported = true;
        }
        a[0] = sgi_kona_gl_int((int16_t)(args[0] >> 16));
        cmd = (SGIGLCmd) { SGI_GL_OP_INDEXF, 1, a };
        break;
    case SGI_KONA_CMD_GL_COLOR4F:
        cmd = (SGIGLCmd) { SGI_GL_OP_COLOR4F, 4, args };
        break;
    case SGI_KONA_CMD_GL_COLOR3D:
    case SGI_KONA_CMD_GL_COLOR4D:
        cmd.nargs = selector == SGI_KONA_CMD_GL_COLOR3D ? 3 : 4;
        cmd.op = cmd.nargs == 3 ? SGI_GL_OP_COLOR3F : SGI_GL_OP_COLOR4F;
        break;
    case SGI_KONA_CMD_GL_COLOR3I:
    case SGI_KONA_CMD_GL_COLOR4I:
    case SGI_KONA_CMD_GL_COLOR3UI:
    case SGI_KONA_CMD_GL_COLOR4UI:
        cmd.nargs = selector == SGI_KONA_CMD_GL_COLOR3I ||
                    selector == SGI_KONA_CMD_GL_COLOR3UI ? 3 : 4;
        cmd.op = cmd.nargs == 3 ? SGI_GL_OP_COLOR3F : SGI_GL_OP_COLOR4F;
        for (int k = 0; k < cmd.nargs; k++) {
            a[k] = selector == SGI_KONA_CMD_GL_COLOR3I ||
                   selector == SGI_KONA_CMD_GL_COLOR4I ?
                   sgi_kona_gl_signed((int32_t)args[k], 32) :
                   sgi_kona_gl_unsigned(args[k], 32);
        }
        break;
    case SGI_KONA_CMD_GL_COLOR3UB:
    case SGI_KONA_CMD_GL_COLOR4UB:
        /* r << 24 | g << 16 | 0x66, then b << 24 (and a << 16 for 4ub). */
        a[0] = hi >> 24;
        a[1] = (hi >> 16) & 0xff;
        a[2] = args[0] >> 24;
        a[3] = (args[0] >> 16) & 0xff;
        cmd.op = selector == SGI_KONA_CMD_GL_COLOR3UB ? SGI_GL_OP_COLOR3UB :
                                                        SGI_GL_OP_COLOR4UB;
        cmd.nargs = selector == SGI_KONA_CMD_GL_COLOR3UB ? 3 : 4;
        break;
    case SGI_KONA_CMD_GL_COLOR3B:
        a[0] = sgi_kona_gl_signed((int8_t)(hi >> 24), 8);
        a[1] = sgi_kona_gl_signed((int8_t)(hi >> 16), 8);
        a[2] = sgi_kona_gl_signed((int8_t)(args[0] >> 24), 8);
        cmd = (SGIGLCmd) { SGI_GL_OP_COLOR3F, 3, a };
        break;
    case SGI_KONA_CMD_GL_COLOR4S:
        /* r << 16 | 0x6d, g << 16 | b, a << 16. */
        a[0] = sgi_kona_gl_signed((int16_t)(hi >> 16), 16);
        a[1] = sgi_kona_gl_signed((int16_t)(args[0] >> 16), 16);
        a[2] = sgi_kona_gl_signed((int16_t)args[0], 16);
        a[3] = sgi_kona_gl_signed((int16_t)(args[1] >> 16), 16);
        cmd = (SGIGLCmd) { SGI_GL_OP_COLOR4F, 4, a };
        break;
    case SGI_KONA_CMD_GL_NORMAL3F:
        cmd = (SGIGLCmd) { SGI_GL_OP_NORMAL3F, 3, args };
        break;
    case SGI_KONA_CMD_GL_NORMAL3D:
        cmd = (SGIGLCmd) { SGI_GL_OP_NORMAL3F, 3, a };
        break;
    case SGI_KONA_CMD_GL_NORMAL3I:
        for (int k = 0; k < 3; k++) {
            a[k] = sgi_kona_gl_signed((int32_t)args[k], 32);
        }
        cmd = (SGIGLCmd) { SGI_GL_OP_NORMAL3F, 3, a };
        break;
    case SGI_KONA_CMD_GL_NORMAL3B:
        a[0] = sgi_kona_gl_signed((int8_t)(hi >> 24), 8);
        a[1] = sgi_kona_gl_signed((int8_t)(hi >> 16), 8);
        a[2] = sgi_kona_gl_signed((int8_t)(args[0] >> 24), 8);
        cmd = (SGIGLCmd) { SGI_GL_OP_NORMAL3F, 3, a };
        break;
    case SGI_KONA_CMD_GL_NORMAL3S:
        a[0] = sgi_kona_gl_signed((int16_t)(hi >> 16), 16);
        a[1] = sgi_kona_gl_signed((int16_t)(args[0] >> 16), 16);
        a[2] = sgi_kona_gl_signed((int16_t)args[0], 16);
        cmd = (SGIGLCmd) { SGI_GL_OP_NORMAL3F, 3, a };
        break;
    case SGI_KONA_CMD_GL_TEXCOORD1F:
        a[0] = args[0];
        a[1] = 0;
        cmd = (SGIGLCmd) { SGI_GL_OP_TEXCOORD2F, 2, a };
        break;
    case SGI_KONA_CMD_GL_TEXCOORD2F:
        cmd = (SGIGLCmd) { SGI_GL_OP_TEXCOORD2F, 2, args };
        break;
    case SGI_KONA_CMD_GL_TEXCOORD3F:
        cmd = (SGIGLCmd) { SGI_GL_OP_TEXCOORD3F, 3, args };
        break;
    case SGI_KONA_CMD_GL_TEXCOORD4F:
        cmd = (SGIGLCmd) { SGI_GL_OP_TEXCOORD4F, 4, args };
        break;
    case SGI_KONA_CMD_GL_TEXCOORD2S:
        a[0] = sgi_kona_gl_int((int16_t)(hi >> 16));
        a[1] = sgi_kona_gl_int((int16_t)(args[0] >> 16));
        cmd = (SGIGLCmd) { SGI_GL_OP_TEXCOORD2F, 2, a };
        break;
    case SGI_KONA_CMD_GL_EDGE_FLAG:
        cmd = (SGIGLCmd) { SGI_GL_OP_EDGE_FLAG, 1, args };
        break;
    case SGI_KONA_CMD_GL_RASTER_POS2F:
        cmd = (SGIGLCmd) { SGI_GL_OP_RASTER_POS2F, 2, args };
        break;
    case SGI_KONA_CMD_GL_RASTER_POS3F:
        cmd = (SGIGLCmd) { SGI_GL_OP_RASTER_POS3F, 3, args };
        break;
    case SGI_KONA_CMD_GL_RASTER_POS4F:
        cmd = (SGIGLCmd) { SGI_GL_OP_RASTER_POS4F, 4, args };
        break;
    case SGI_KONA_CMD_GL_RASTER_POS2D:
    case SGI_KONA_CMD_GL_RASTER_POS3D:
    case SGI_KONA_CMD_GL_RASTER_POS4D:
        cmd.nargs = selector == SGI_KONA_CMD_GL_RASTER_POS2D ? 2 :
                    selector == SGI_KONA_CMD_GL_RASTER_POS3D ? 3 : 4;
        cmd.op = SGI_GL_OP_RASTER_POS2F + cmd.nargs - 2;
        break;
    case SGI_KONA_CMD_GL_RASTER_POS2I:
    case SGI_KONA_CMD_GL_RASTER_POS2S:
    case SGI_KONA_CMD_GL_RASTER_POS3I:
    case SGI_KONA_CMD_GL_RASTER_POS3S:
    case SGI_KONA_CMD_GL_RASTER_POS4I:
    case SGI_KONA_CMD_GL_RASTER_POS4S:
        /* Shorts arrive sign-extended to a word, like integers. */
        cmd.nargs = selector <= SGI_KONA_CMD_GL_RASTER_POS2S ? 2 :
                    selector <= SGI_KONA_CMD_GL_RASTER_POS3S ? 3 : 4;
        cmd.op = SGI_GL_OP_RASTER_POS2F + cmd.nargs - 2;
        for (int k = 0; k < cmd.nargs; k++) {
            a[k] = sgi_kona_gl_int(args[k]);
        }
        break;
    case SGI_KONA_CMD_GL_LIGHTF:
    case SGI_KONA_CMD_GL_LIGHTI:
        if (args[0] - SGI_GL_LIGHT0 >= SGI_GL_LIGHTS) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: glLight of 0x%" PRIx32
                          ", not a light; ignored\n", args[0]);
            return true;
        }
        a[0] = args[0] - SGI_GL_LIGHT0;
        a[1] = args[1];
        if (selector == SGI_KONA_CMD_GL_LIGHTI) {
            sgi_kona_gl_int_params(args[1], &args[2], &a[2]);
        } else {
            memcpy(&a[2], &args[2], 4 * sizeof(uint32_t));
        }
        cmd = (SGIGLCmd) { SGI_GL_OP_LIGHT, 6, a };
        break;
    case SGI_KONA_CMD_GL_LIGHT_MODELF:
        cmd = (SGIGLCmd) { SGI_GL_OP_LIGHT_MODEL, 5, args };
        break;
    case SGI_KONA_CMD_GL_LIGHT_MODELI:
        a[0] = args[0];
        sgi_kona_gl_int_params(args[0], &args[1], &a[1]);
        cmd = (SGIGLCmd) { SGI_GL_OP_LIGHT_MODEL, 5, a };
        break;
    case SGI_KONA_CMD_GL_MATERIAL:
        cmd = (SGIGLCmd) { SGI_GL_OP_MATERIAL, 6, args };
        break;
    case SGI_KONA_CMD_GL_COLOR_MATERIAL:
        cmd = (SGIGLCmd) { SGI_GL_OP_COLOR_MATERIAL, 2, args };
        break;
    case SGI_KONA_CMD_GL_ENABLE:
        cmd = (SGIGLCmd) { SGI_GL_OP_ENABLE, 1, args };
        break;
    default:
        return false;
    }
    /* The double forms: each argument two words, converted. */
    switch (selector) {
    case SGI_KONA_CMD_GL_LOAD_MATRIXD:
    case SGI_KONA_CMD_GL_MULT_MATRIXD:
    case SGI_KONA_CMD_GL_FRUSTUM:
    case SGI_KONA_CMD_GL_ORTHO:
    case SGI_KONA_CMD_GL_ROTATED:
    case SGI_KONA_CMD_GL_SCALED:
    case SGI_KONA_CMD_GL_TRANSLATED:
    case SGI_KONA_CMD_GL_DEPTH_RANGE:
    case SGI_KONA_CMD_GL_VERTEX2D:
    case SGI_KONA_CMD_GL_VERTEX3D:
    case SGI_KONA_CMD_GL_VERTEX4D:
    case SGI_KONA_CMD_GL_RASTER_POS2D:
    case SGI_KONA_CMD_GL_RASTER_POS3D:
    case SGI_KONA_CMD_GL_RASTER_POS4D:
    case SGI_KONA_CMD_GL_COLOR3D:
    case SGI_KONA_CMD_GL_COLOR4D:
    case SGI_KONA_CMD_GL_NORMAL3D:
    case SGI_KONA_CMD_GL_INDEXD:
        for (int k = 0; k < cmd.nargs; k++) {
            a[k] = sgi_kona_gl_double(&args[2 * k]);
        }
        break;
    }
    trace_sgi_kona_gl_command(selector, i);
    if (s->state[i]->gl.matrix_mode == 0x1800 &&
        sgi_kona_gl_colour_matrix(s, i, &cmd)) {
        return true;
    }
    sgi_kona_gl_sink(s, &sink);
    sgi_kona_gl_run(s, i, &sink, &cmd);
    return true;
}

/*
 * glDrawBuffer's enumerant as the draw state's front and back mask: the
 * left buffers are the only ones a mono visual has.
 */
static bool sgi_kona_gl_draw_buffers(uint32_t buffer, uint32_t *mask)
{
    switch (buffer) {
    case 0:                     /* GL_NONE */
    case 0x0401:                /* GL_FRONT_RIGHT */
    case 0x0403:                /* GL_BACK_RIGHT */
    case 0x0407:                /* GL_RIGHT */
        *mask = 0;
        return true;
    case 0x0400:                /* GL_FRONT_LEFT */
    case SGI_GL_FRONT:
        *mask = SGI_GL_DRAW_FRONT;
        return true;
    case 0x0402:                /* GL_BACK_LEFT */
    case SGI_GL_BACK:
        *mask = SGI_GL_DRAW_BACK;
        return true;
    case 0x0406:                /* GL_LEFT */
    case SGI_GL_FRONT_AND_BACK:
        *mask = SGI_GL_DRAW_FRONT | SGI_GL_DRAW_BACK;
        return true;
    default:
        return false;
    }
}

/*
 * 0xb's 32 rows, the end of its data run: the pattern, bottom row first,
 * each row's leftmost pixel in the most significant bit of its word under
 * the default unpack state (inferred, as for the row count), into the
 * shared context's rows, whose bit x is pixel x.
 */
static void sgi_kona_gl_stipple_rows(SGIKonaState *s, int i)
{
    uint32_t rows[SGI_KONA_GL_STIPPLE_ROWS];

    for (int r = 0; r < SGI_KONA_GL_STIPPLE_ROWS; r++) {
        rows[r] = revbit32(s->gl_data[r]);
    }
    sgi_gl_polygon_stipple(&s->state[i]->gl, rows);
}

/*
 * SGIS_multisample's state: glEnable and glDisable of GL_MULTISAMPLE_SGIS,
 * GL_SAMPLE_ALPHA_TO_MASK_SGIS, GL_SAMPLE_ALPHA_TO_ONE_SGIS and
 * GL_SAMPLE_MASK_SGIS reach the pipe as the ordinary 0x24 and 0x25 words,
 * glSampleMaskSGIS as 0x118 with its value clamped to [0, 1] and the
 * invert flag, glSamplePatternSGIS as 0x117 and the pattern; the client
 * keeps none of it and asks the pipe for it back through 0x4c (established
 * from the IRIX client library). The model keeps the state for those
 * replies, with the extension's initial values, and draws single-sampled
 * whatever it says (see sgi_kona_gl_stencil_bits()). Returns false for a
 * word that is not one of these.
 */
#define SGI_KONA_GL_MULTISAMPLE_SGIS    0x809d
#define SGI_KONA_GL_SAMPLE_MASK_SGIS    0x80a0
#define SGI_KONA_GL_SAMPLE_MASK_VALUE   0x80aa
#define SGI_KONA_GL_SAMPLE_MASK_INVERT  0x80ab
#define SGI_KONA_GL_SAMPLE_PATTERN      0x80ac

static bool sgi_kona_gl_multisample(SGIKonaState *s, int i, uint32_t selector,
                                    const uint32_t *args)
{
    SGIKonaGLPipe *p = &s->state[i]->pipe;

    switch (selector) {
    case SGI_KONA_CMD_GL_ENABLE:
    case SGI_KONA_CMD_GL_DISABLE:
        if (args[0] < SGI_KONA_GL_MULTISAMPLE_SGIS ||
            args[0] > SGI_KONA_GL_SAMPLE_MASK_SGIS) {
            return false;
        }
        if (selector == SGI_KONA_CMD_GL_ENABLE) {
            p->multisample_enables |=
                1u << (args[0] - SGI_KONA_GL_MULTISAMPLE_SGIS);
        } else {
            p->multisample_enables &=
                ~(1u << (args[0] - SGI_KONA_GL_MULTISAMPLE_SGIS));
        }
        break;
    case SGI_KONA_CMD_GL_SAMPLE_MASK:
        p->sample_mask_value = args[0];
        p->sample_mask_invert = args[1] != 0;
        break;
    case SGI_KONA_CMD_GL_SAMPLE_PATTERN:
        if (args[0] < SGI_KONA_GL_1PASS_SGIS ||
            args[0] > SGI_KONA_GL_4PASS_3_SGIS) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: sample pattern 0x%"
                          PRIx32 " is not one\n", args[0]);
            sgi_kona_gl_set_error(s, SGI_GL_INVALID_ENUM);
            return true;
        }
        p->sample_pattern = args[0];
        break;
    default:
        return false;
    }
    trace_sgi_kona_gl_command(selector, i);
    return true;
}

/* The multisample state's 0x4c reply, or false for another name. */
static bool sgi_kona_gl_multisample_get(const SGIKonaGLPipe *p, uint32_t name,
                                        uint64_t *value)
{
    if (name >= SGI_KONA_GL_MULTISAMPLE_SGIS &&
        name <= SGI_KONA_GL_SAMPLE_MASK_SGIS) {
        *value = (p->multisample_enables >>
                  (name - SGI_KONA_GL_MULTISAMPLE_SGIS)) & 1;
    } else if (name == SGI_KONA_GL_SAMPLE_MASK_VALUE) {
        *value = p->sample_mask_value;
    } else if (name == SGI_KONA_GL_SAMPLE_MASK_INVERT) {
        *value = p->sample_mask_invert;
    } else if (name == SGI_KONA_GL_SAMPLE_PATTERN) {
        *value = p->sample_pattern;
    } else {
        return false;
    }
    return true;
}

/*
 * The GL client's raster state, onto the current context's draw state:
 * depth, blending, alpha, stencil, masks, logic op, culling and facing,
 * polygon modes and offset, lines and points, fog, clip planes, draw
 * buffer and texture environment, each carrying the caller's enumerants
 * and values (established from the senders). The integer forms of glFog
 * and glTexEnv convert by OpenGL's rules. glClearDepth's double in 0 to 1
 * is scaled to the 24-bit depth plane, rounded to nearest, the model's
 * choice. Stencil uses the shared stages and an internal S8 plane.
 * Hints retain shared enum state without changing rendering. glClearAccum
 * is absorbed. Returns false for a selector this does not carry out.
 */
static bool sgi_kona_gl_raster_state(SGIKonaState *s, int i,
                                     uint32_t selector, const uint32_t *args)
{
    uint32_t a[6];
    SGIGLCmd cmd = { .args = args };
    SGIGLSink sink;
    float f;

    switch (selector) {
    case SGI_KONA_CMD_GL_DEPTH_FUNC:
        cmd.op = SGI_GL_OP_DEPTH_FUNC;
        cmd.nargs = 1;
        break;
    case SGI_KONA_CMD_GL_DEPTH_MASK:
        cmd.op = SGI_GL_OP_DEPTH_MASK;
        cmd.nargs = 1;
        break;
    case SGI_KONA_CMD_GL_CLEAR_DEPTH:
        if (!s->state[i]->gl.in_begin &&
            !sgi_kona_gl_native_depth(args,
                                      &s->state[i]->pipe.returns.clear_depth,
                                       1)) {
            return true;
        }
        memcpy(&f, &(uint32_t){ sgi_kona_gl_double(args) }, sizeof(f));
        a[0] = CLAMP(f, 0.0f, 1.0f) *
               (double)((1u << sgi_kona_gl_rules.depth_bits) - 1) + 0.5;
        cmd = (SGIGLCmd) { SGI_GL_OP_CLEAR_DEPTH, 1, a };
        break;
    case SGI_KONA_CMD_GL_BLEND_FUNC:
        cmd.op = SGI_GL_OP_BLEND_FUNC;
        cmd.nargs = 2;
        break;
    case SGI_KONA_CMD_GL_BLEND_COLOR:
        cmd.op = SGI_GL_OP_BLEND_COLOR;
        cmd.nargs = 4;
        break;
    case SGI_KONA_CMD_GL_BLEND_EQUATION:
        cmd.op = SGI_GL_OP_BLEND_EQUATION;
        cmd.nargs = 1;
        break;
    case SGI_KONA_CMD_GL_ALPHA_FUNC:
        cmd.op = SGI_GL_OP_ALPHA_FUNC;
        cmd.nargs = 2;
        break;
    case SGI_KONA_CMD_GL_STENCIL_FUNC:
        cmd.op = SGI_GL_OP_STENCIL_FUNC;
        cmd.nargs = 3;
        break;
    case SGI_KONA_CMD_GL_STENCIL_OP:
        cmd.op = SGI_GL_OP_STENCIL_OP;
        cmd.nargs = 3;
        break;
    case SGI_KONA_CMD_GL_STENCIL_MASK:
        cmd.op = SGI_GL_OP_STENCIL_MASK;
        cmd.nargs = 1;
        break;
    case SGI_KONA_CMD_GL_CLEAR_STENCIL:
        cmd.op = SGI_GL_OP_CLEAR_STENCIL;
        cmd.nargs = 1;
        break;
    case SGI_KONA_CMD_GL_COLOR_MASK:
        cmd.op = SGI_GL_OP_COLOR_MASK;
        cmd.nargs = 4;
        break;
    case SGI_KONA_CMD_GL_INDEX_MASK:
        cmd.op = SGI_GL_OP_INDEX_MASK;
        cmd.nargs = 1;
        break;
    case SGI_KONA_CMD_GL_CLEAR_INDEX:
        cmd.op = SGI_GL_OP_CLEAR_INDEX;
        cmd.nargs = 1;
        break;
    case SGI_KONA_CMD_GL_LOGIC_OP:
        cmd.op = SGI_GL_OP_LOGIC_OP;
        cmd.nargs = 1;
        break;
    case SGI_KONA_CMD_GL_CULL_FACE:
        cmd.op = SGI_GL_OP_CULL_FACE;
        cmd.nargs = 1;
        break;
    case SGI_KONA_CMD_GL_FRONT_FACE:
        cmd.op = SGI_GL_OP_FRONT_FACE;
        cmd.nargs = 1;
        break;
    case SGI_KONA_CMD_GL_POLYGON_MODE:
        cmd.op = SGI_GL_OP_POLYGON_MODE;
        cmd.nargs = 2;
        break;
    case SGI_KONA_CMD_GL_POLYGON_OFFSET:
        cmd.op = SGI_GL_OP_POLYGON_OFFSET;
        cmd.nargs = 2;
        break;
    case SGI_KONA_CMD_GL_OFFSET_EXT:
        cmd.op = SGI_GL_OP_POLYGON_OFFSET_EXT;
        cmd.nargs = 2;
        break;
    /*
     * The client sends the width and the width limited to its maximum.
     * The context takes the first, the value glGet answers; which of the
     * two the pipe rasterizes with is unknown, and they differ only above
     * the library's maximum.
     */
    case SGI_KONA_CMD_GL_LINE_WIDTH:
        cmd.op = SGI_GL_OP_LINE_WIDTH;
        cmd.nargs = 1;
        break;
    case SGI_KONA_CMD_GL_POINT_SIZE:
        cmd.op = SGI_GL_OP_POINT_SIZE;
        cmd.nargs = 1;
        break;
    case SGI_KONA_CMD_GL_LINE_STIPPLE:
        cmd.op = SGI_GL_OP_LINE_STIPPLE;
        cmd.nargs = 2;
        break;
    case SGI_KONA_CMD_GL_FOGF:
    case SGI_KONA_CMD_GL_FOGI:
        if (args[0] == 0x8199) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: glFog's SGI fog function"
                          " is not modelled; ignored\n");
            return true;
        }
        a[0] = args[0];
        for (int k = 0; k < 4; k++) {
            a[1 + k] = selector == SGI_KONA_CMD_GL_FOGF ? args[1 + k] :
                       args[0] == SGI_GL_FOG_COLOR ?
                       sgi_kona_gl_signed((int32_t)args[1 + k], 32) :
                       sgi_kona_gl_int(args[1 + k]);
        }
        cmd = (SGIGLCmd) { SGI_GL_OP_FOG, 5, a };
        break;
    case SGI_KONA_CMD_GL_TEX_ENVF:
    case SGI_KONA_CMD_GL_TEX_ENVI:
        a[0] = args[0];
        for (int k = 0; k < 4; k++) {
            a[1 + k] = selector == SGI_KONA_CMD_GL_TEX_ENVF ? args[1 + k] :
                       args[0] == SGI_GL_TEXTURE_ENV_COLOR ?
                       sgi_kona_gl_signed((int32_t)args[1 + k], 32) :
                       sgi_kona_gl_int(args[1 + k]);
        }
        cmd = (SGIGLCmd) { SGI_GL_OP_TEX_ENV, 5, a };
        break;
    case SGI_KONA_CMD_GL_TEX_GEN:
        /*
         * glTexGen{i,f,d,iv,fv,dv} in the IRIX n32 client library: the
         * coordinate, the pname,
         * then for an object or eye plane four single floats, and for
         * anything else, the mode among them, one integer word.
         */
        a[0] = args[0];
        a[1] = args[1];
        if (args[1] == SGI_GL_OBJECT_PLANE || args[1] == SGI_GL_EYE_PLANE) {
            memcpy(&a[2], &args[2], 4 * sizeof(*a));
        } else if (args[1] == SGI_GL_TEXTURE_GEN_MODE) {
            a[2] = sgi_kona_gl_int(args[2]);
            a[3] = a[4] = a[5] = 0;
        } else {
            /* The SGIS point and line pnames, 0x81f4 to 0x81f7. */
            qemu_log_mask(LOG_UNIMP, "sgi-kona: glTexGen of coordinate 0x%"
                          PRIx32 " parameter 0x%" PRIx32 " is not modelled;"
                          " ignored\n", args[0], args[1]);
            return true;
        }
        cmd = (SGIGLCmd) { SGI_GL_OP_TEX_GEN, 6, a };
        break;
    case SGI_KONA_CMD_GL_CLIP_PLANE:
        a[0] = args[0] - SGI_GL_CLIP_PLANE0;
        for (int k = 0; k < 4; k++) {
            a[1 + k] = sgi_kona_gl_double(&args[1 + 2 * k]);
        }
        cmd = (SGIGLCmd) { SGI_GL_OP_CLIP_PLANE, 5, a };
        break;
    case SGI_KONA_CMD_GL_DRAW_BUFFER:
        if (!sgi_kona_gl_draw_buffers(args[0], &a[0])) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: glDrawBuffer of 0x%"
                          PRIx32 " ignored\n", args[0]);
            return true;
        }
        s->state[i]->pipe.returns.draw_buffer = args[0];
        cmd = (SGIGLCmd) { SGI_GL_OP_DRAW_BUFFERS, 1, a };
        break;
    case SGI_KONA_CMD_GL_HINT:
        cmd.op = SGI_GL_OP_HINT;
        cmd.nargs = 2;
        break;
    case SGI_KONA_CMD_GL_CLEAR_ACCUM:
        trace_sgi_kona_gl_command(selector, i);
        memcpy(s->state[i]->pipe.accum_clear, args,
               sizeof(s->state[i]->pipe.accum_clear));
        return true;
    case SGI_KONA_CMD_GL_ACCUM:
        trace_sgi_kona_gl_command(selector, i);
        sgi_kona_gl_accum(s, i, args[0], NULL, sgi_kona_gl_float(args[1]));
        return true;
    default:
        return false;
    }
    trace_sgi_kona_gl_command(selector, i);
    sgi_kona_gl_sink(s, &sink);
    sgi_kona_gl_run(s, i, &sink, &cmd);
    return true;
}

/*
 * The return page's size: the length every traced 0x1f2 registered, and
 * so the most a reply can carry (inferred).
 */
#define SGI_KONA_RETURN_PAGE_BYTES 0x4000
#define SGI_KONA_SMALL_RETURN_BYTES 16368

static bool sgi_kona_gl_read_enum(uint32_t buffer)
{
    return buffer == 0x400 || buffer == 0x402 ||
           buffer == SGI_GL_FRONT || buffer == SGI_GL_BACK;
}

/*
 * The bank glReadBuffer names for context i, resolving logical roles at
 * use time; an unavailable BACK never falls back. `readback` adds the
 * visual check of what glReadPixels may return (sgi_kona_gl_rgb_config());
 * the accumulation buffer's reads take the colour whatever the visual's
 * other buffers.
 */
static int sgi_kona_gl_colour_bank(SGIKonaState *s, unsigned i,
                                   uint32_t *selection, bool readback)
{
    const SGIKonaGLPipe *p = &s->state[i]->pipe;
    const SGIKonaGLWindow *w = &s->state[i]->window;
    uint32_t buffer = p->returns.read_buffer;
    bool back;

    if (!w->placed || !w->width || !w->height ||
        (readback && !sgi_kona_gl_read_compatible(p)) ||
        p->read_binding_unknown || p->returns.read_buffer_unsupported) {
        return -1;
    }
    if (!buffer) {
        buffer = w->double_buffered ? SGI_GL_BACK : SGI_GL_FRONT;
    }
    back = buffer == SGI_GL_BACK || buffer == 0x402;
    if (back && !w->double_buffered) {
        return -1;
    }
    *selection = buffer;
    return w->double_buffered ? w->displayed_bank ^ back : 0;
}

/* Resolve logical roles at use time; unavailable BACK never falls back. */
static int sgi_kona_gl_read_bank(SGIKonaState *s, unsigned i,
                                 uint32_t *selection)
{
    return sgi_kona_gl_colour_bank(s, i, selection, true);
}

/*
 * The accumulation buffer (OpenGL 1.2.1, 4.2.4). The client library sends
 * glAccum as 0xc5 with the operation's enumerant and the value as a float,
 * glClearAccum as 0x1b with four floats clamped to [-1, 1], and clears the
 * buffer through glClear's 0x200 bit; it does no accumulation itself and
 * checks nothing before sending glAccum (established from the IRIX client
 * library, which sends these from both immediate mode and display lists,
 * and from IRIS GL's acbuf, which calls them). So the pipe holds the
 * buffer and does the arithmetic. Visuals with one carry 25 in the buffer
 * configuration's accumulation word; the pipe's storage is not visible to
 * software.
 *
 * Approximated: the model keeps each component as a single-precision float
 * and follows OpenGL's arithmetic: LOAD and ACCUM add value times the read
 * buffer's colour, its alpha 1 as the planes hold none, MULT and ADD act on
 * the buffer, RETURN writes value times the buffer, clamped to [0, 1] and
 * rounded to eight bits, to the draw buffers under the colour mask. Each
 * reaches the pixels glClear would (sgi_gl_clear_area()) that the window
 * owns. How the pipe stores 25 bits a component, rounds on RETURN and
 * saturates past [-1, 1] is unknown; a real pipe's readback after a
 * sequence of operations would settle it. An operation in a context whose
 * visual has no accumulation buffer, in colour-index mode, or with an
 * unknown operation sets GL_INVALID_OPERATION or GL_INVALID_ENUM, as
 * OpenGL asks; that the pipe raises them, as the library does not, is
 * inferred. The accumulation qtests check the arithmetic as stated.
 */
#define SGI_KONA_ACCUM_PLANES 4

static bool sgi_kona_accum_ready(SGIKonaState *s)
{
    SGIBoardMemory *m = &s->accum_mem;
    const uint64_t plane = (uint64_t)s->fb_width * s->fb_height * 4;

    if (m->bytes) {
        return true;
    }
    sgi_board_memory_init(m, SGI_KONA_ACCUM_PLANES * plane);
    for (unsigned k = 0; k < SGI_KONA_ACCUM_PLANES; k++) {
        SGIPlane p = {
            .base = k * plane,
            .len = plane,
            .width = s->fb_width,
            .height = s->fb_height,
            .stride = s->fb_width * 4,
            .format = SGI_PLANE_RAW,
            .packing = SGI_PACK_BE_RED_LOW,
        };

        if (sgi_board_memory_add_plane(m, &p, NULL) == SGI_PLANE_NONE) {
            sgi_board_memory_cleanup(m);
            return false;
        }
    }
    return true;
}

static float sgi_kona_accum_load(SGIKonaState *s, unsigned k, int x, int y)
{
    return sgi_kona_gl_float(sgi_plane_load(&s->accum_mem,
                             sgi_board_memory_plane(&s->accum_mem, k), x, y));
}

static void sgi_kona_accum_store(SGIKonaState *s, unsigned k, int x, int y,
                                 float value)
{
    union { float f; uint32_t u; } bits = { .f = value };

    sgi_plane_store(&s->accum_mem, sgi_board_memory_plane(&s->accum_mem, k),
                    x, y, bits.u);
}

/* A colour component in [0, 1] as the eight-bit planes round it. */
static uint32_t sgi_kona_accum_channel(float value)
{
    return (uint32_t)floor(CLAMP(value, 0.0f, 1.0f) * 255.0 + 0.5);
}

static void sgi_kona_gl_accum(SGIKonaState *s, int i, uint32_t op,
                              const uint32_t *clear, float value)
{
    SGIKonaGLState *state = s->state[i];
    SGIGLContext *c = &state->gl;
    SGIRasterOp area = { 0 };
    SGIColourTarget targets[SGI_GL_MAX_TARGETS];
    uint32_t ntargets = 0, mask = 0, selection;
    int bank = -1;
    float fill[4];

    if (clear && (!state->pipe.draw_config_received ||
                  !state->pipe.draw_config[SGI_KONA_GL_BUFFER_CONFIG_ACCUM])) {
        /* A clear of a buffer the visual lacks does nothing (4.2.3). */
        return;
    }
    if (!state->pipe.draw_config_received ||
        !state->pipe.draw_config[SGI_KONA_GL_BUFFER_CONFIG_ACCUM] ||
        c->cur.index_bits) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: accumulation operation "
                      "0x%" PRIx32 " in context %d, whose visual has no "
                      "accumulation buffer\n", op, i);
        sgi_kona_gl_set_error(s, SGI_GL_INVALID_OPERATION);
        return;
    }
    if (op < SGI_GL_ACCUM || op > SGI_GL_ADD) {
        if (!clear) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: glAccum operation 0x%"
                          PRIx32 " is not one\n", op);
            sgi_kona_gl_set_error(s, SGI_GL_INVALID_ENUM);
            return;
        }
    }
    if (!sgi_gl_clear_area(c, &area)) {
        return;
    }
    if (!sgi_kona_accum_ready(s)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: no memory for the "
                      "accumulation buffer\n");
        sgi_kona_gl_set_error(s, SGI_GL_OUT_OF_MEMORY);
        return;
    }
    if (clear) {
        for (unsigned k = 0; k < 4; k++) {
            fill[k] = sgi_kona_gl_float(clear[k]);
        }
    } else if (op == SGI_GL_ACCUM || op == SGI_GL_LOAD) {
        bank = sgi_kona_gl_colour_bank(s, i, &selection, false);
        if (bank < 0) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: glAccum of 0x%" PRIx32
                          " with no readable colour buffer; dropped\n", op);
            return;
        }
    } else if (op == SGI_GL_RETURN) {
        ntargets = sgi_gl_draw_targets(&c->cur, targets);
        mask = sgi_raster_colour_mask(&sgi_kona_gl_rules, c->cur.colour_mask);
    }
    for (int y = area.y0; y <= area.y1; y++) {
        for (int x = area.x0; x <= area.x1; x++) {
            float acc[4], colour[4] = { 0 };

            if (!sgi_clip_contains(&area.clip, x, y) ||
                !sgi_kona_gl_pixel_owned(s, x, y)) {
                continue;
            }
            for (unsigned k = 0; k < 4; k++) {
                acc[k] = sgi_kona_accum_load(s, k, x, y);
            }
            if (bank >= 0) {
                uint32_t rgb = sgi_kona_bank_read(s, bank, x, y);

                colour[0] = ((rgb >> 16) & 0xff) / 255.0f;
                colour[1] = ((rgb >> 8) & 0xff) / 255.0f;
                colour[2] = (rgb & 0xff) / 255.0f;
                colour[3] = 1.0f;
            }
            for (unsigned k = 0; k < 4; k++) {
                if (clear) {
                    acc[k] = fill[k];
                } else if (op == SGI_GL_ACCUM) {
                    acc[k] += value * colour[k];
                } else if (op == SGI_GL_LOAD) {
                    acc[k] = value * colour[k];
                } else if (op == SGI_GL_MULT) {
                    acc[k] *= value;
                } else if (op == SGI_GL_ADD) {
                    acc[k] += value;
                }
            }
            if (op == SGI_GL_RETURN && !clear) {
                uint32_t rgb = sgi_kona_accum_channel(value * acc[0]) << 16 |
                               sgi_kona_accum_channel(value * acc[1]) << 8 |
                               sgi_kona_accum_channel(value * acc[2]);

                for (uint32_t t = 0; mask && t < ntargets; t++) {
                    uint32_t out = rgb;

                    if (mask != 0xffffff) {
                        out = (rgb & mask) |
                              (sgi_kona_gl_load_colour(s, targets[t], x, y) &
                               ~mask);
                    }
                    sgi_kona_gl_store_colour(s, targets[t], x, y, out);
                }
                continue;
            }
            for (unsigned k = 0; k < 4; k++) {
                sgi_kona_accum_store(s, k, x, y, acc[k]);
            }
        }
    }
}

/* Depth is independent of the colour ReadBuffer selection. */
static bool sgi_kona_gl_depth_source_ready(SGIKonaState *s, unsigned i)
{
    const SGIKonaGLPipe *p = &s->state[i]->pipe;
    const SGIKonaGLWindow *w = &s->state[i]->window;

    return w->placed && w->width && w->height &&
           sgi_kona_gl_read_compatible(p) && !p->read_binding_unknown &&
           p->draw_config[6] == 24;
}

static bool sgi_kona_gl_stencil_source_ready(SGIKonaState *s, unsigned i)
{
    const SGIKonaGLPipe *p = &s->state[i]->pipe;
    const SGIKonaGLWindow *w = &s->state[i]->window;

    return w->placed && w->width && w->height &&
           sgi_kona_gl_stencil_bits(s, i) == 8 && !p->read_binding_unknown &&
           (!p->read_config_received ||
            !memcmp(p->draw_config, p->read_config, sizeof(p->draw_config)));
}

static bool sgi_kona_gl_index_transfer(const SGIKonaGLPixelTransfer *t,
                                      double index, bool round, uint32_t *out)
{
    float shift = sgi_kona_gl_float(t->index_shift);
    float offset = sgi_kona_gl_float(t->index_offset);
    double rounded_shift = floor((double)shift + 0.5);
    double rounded_offset = floor((double)offset + 0.5);

    if (!isfinite(shift) || !isfinite(offset) ||
        rounded_shift < INT32_MIN || rounded_shift > INT32_MAX ||
        rounded_offset < INT32_MIN || rounded_offset > INT32_MAX) {
        return false;
    }
    return round ? sgi_kona_gl_pixel_index_round(index, rounded_shift,
                                                rounded_offset, out) :
                   sgi_kona_gl_pixel_index_transfer(index, rounded_shift,
                                                     rounded_offset, out);
}

static bool sgi_kona_gl_colour_index(const SGIKonaGLPixelTransfer *t,
                                     double index, uint32_t *out)
{
    if (!sgi_kona_gl_index_transfer(t, index, false, out)) {
        return false;
    }
    if (t->map_colour) {
        const SGIKonaGLPixelMap *map = &t->index_map;

        *out = map->count ? map->values[*out & (map->count - 1)] : 0;
    }
    return true;
}

static bool sgi_kona_gl_read_index(SGIKonaState *s, unsigned bank, int x, int y,
                                   uint32_t bits, uint32_t *index)
{
    uint32_t word = sgi_plane_load(&s->mem, sgi_kona_plane(s,
                                    sgi_kona_bank_plane(s, bank)), x, y);
    uint32_t tag = sgi_plane_load(&s->mem, sgi_kona_plane(s,
                                    sgi_kona_bank_tag_plane(s, bank)), x, y);

    if ((bits != 8 && bits != 12) || tag != SGI_KONA_TAG_X_INDEX16 ||
        word > 0xffff) {
        return false;
    }
    *index = (word >> (16 - bits)) & ((1u << bits) - 1);
    return true;
}

static bool sgi_kona_gl_stencil_index(const SGIKonaGLPipe *p, double index,
                                      uint32_t *out)
{
    if (!sgi_kona_gl_index_transfer(&p->pixel_transfer, index, false, out)) {
        return false;
    }
    if (p->pixel_transfer.map_stencil) {
        const SGIKonaGLPixelMap *map = &p->pixel_transfer.stencil_map;

        *out = map->count ? map->values[*out & (map->count - 1)] : 0;
    }
    return true;
}

static bool sgi_kona_gl_index_rgba(const SGIKonaGLPixelTransfer *t,
                                   double index, double rgba[4])
{
    uint32_t address;

    if (!sgi_kona_gl_index_transfer(t, index, true, &address)) {
        return false;
    }
    for (unsigned k = 0; k < 4; k++) {
        const SGIKonaGLPixelMap *map = &t->index_rgba[k];

        rgba[k] = map->count ?
            sgi_kona_gl_float(map->values[address & (map->count - 1)]) : 0;
    }
    return true;
}

/*
 * Whether sgi_kona_gl_rgba_transfer() leaves every finite component as it
 * is: scales of 1.0f, biases of +0.0f and no colour maps. x * 1.0 + 0.0
 * is x for every finite x but -0.0, which no decoded component is.
 */
static bool sgi_kona_gl_rgba_identity(const SGIKonaGLPixelTransfer *t)
{
    for (unsigned k = 0; k < 4; k++) {
        if (t->scale[k] != 0x3f800000 || t->bias[k]) {
            return false;
        }
    }
    return !t->map_colour;
}

static bool sgi_kona_gl_rgba_transfer(const SGIKonaGLPixelTransfer *t,
                                     double rgba[4])
{
    for (unsigned k = 0; k < 4; k++) {
        double scale = sgi_kona_gl_float(t->scale[k]);
        double bias = sgi_kona_gl_float(t->bias[k]);

        if (!isfinite(rgba[k]) || !isfinite(scale) || !isfinite(bias)) {
            return false;
        }
        rgba[k] = rgba[k] * scale + bias;
        if (t->map_colour) {
            const SGIKonaGLPixelMap *map = &t->colour_rgba[k];
            unsigned at = CLAMP(rgba[k], 0.0, 1.0) *
                          (map->count ? map->count - 1 : 0) + 0.5;

            rgba[k] = map->count ? sgi_kona_gl_float(map->values[at]) : 0;
        }
    }
    return true;
}

typedef struct SGIKonaGLCopySource {
    SGIKonaState *s;
    const SGIKonaGLWindow *window;
    unsigned bank;
} SGIKonaGLCopySource;

static bool sgi_kona_gl_copy_read(void *opaque, int32_t x, int32_t y,
                                  uint32_t *rgba)
{
    const SGIKonaGLCopySource *source = opaque;
    SGIKonaState *s = source->s;
    int64_t px = (int64_t)source->window->x + x;
    int64_t py = (int64_t)source->window->y + y;
    uint32_t rgb;

    if (sgi_plane_load(&s->mem,
            sgi_kona_plane(s, sgi_kona_bank_tag_plane(s, source->bank)),
            px, py) != SGI_KONA_TAG_DIRECT) {
        return false;
    }
    rgb = sgi_plane_load(&s->mem,
        sgi_kona_plane(s, sgi_kona_bank_plane(s, source->bank)), px, py);
    *rgba = (rgb << 8) | 0xff;
    return true;
}

static bool sgi_kona_gl_copy_source_ready(SGIKonaState *s, int i,
                                           int32_t x, int32_t y,
                                           uint32_t width, uint32_t height,
                                           uint32_t type,
                                           SGIKonaGLCopySource *source)
{
    const SGIKonaGLPipe *p;
    const SGIKonaGLWindow *w;
    uint32_t selection;
    int bank;
    int64_t px, py;
    bool depth = type == 0x1801, stencil = type == 0x1802;
    uint32_t index;

    if (i == SGI_KONA_CONTEXT_NONE || s->state[i]->gl.in_begin) {
        return false;
    }
    p = &s->state[i]->pipe;
    w = &s->state[i]->window;
    if (p->pixel_zoom_unsupported || sgi_kona_gl_transfer_refused(s, i) ||
        !isfinite(sgi_kona_gl_float(p->pixel_zoom[0])) ||
        !isfinite(sgi_kona_gl_float(p->pixel_zoom[1])) ||
        !width || !height ||
        (uint64_t)width * height > SGI_KONA_TEXTURE_BYTES /
                                (depth ? sizeof(double) :
                                 !stencil && (p->pixel_transfer.map_colour ||
                                 sgi_kona_gl_imaging_active(s->state[i])) ?
                                 4 * sizeof(double) : sizeof(uint32_t))) {
        return false;
    }
    for (unsigned k = 0; !depth && !stencil &&
                         !s->state[i]->gl.cur.index_bits && k < 4; k++) {
        if (!isfinite(sgi_kona_gl_float(p->pixel_transfer.scale[k])) ||
            !isfinite(sgi_kona_gl_float(p->pixel_transfer.bias[k]))) {
            return false;
        }
    }
    if (depth && (!isfinite(sgi_kona_gl_float(p->pixel_transfer.depth_scale)) ||
                  !isfinite(sgi_kona_gl_float(p->pixel_transfer.depth_bias)))) {
        return false;
    }
    if (stencil && !sgi_kona_gl_stencil_index(p, 0, &index)) {
        return false;
    }
    bank = depth ? (sgi_kona_gl_depth_source_ready(s, i) ? 0 : -1) :
           stencil ? (sgi_kona_gl_stencil_source_ready(s, i) ? 0 : -1) :
                     sgi_kona_gl_read_bank(s, i, &selection);
    px = (int64_t)w->x + x;
    py = (int64_t)w->y + y;
    if (bank < 0 || x < 0 || y < 0 ||
        (uint64_t)x + width > w->width || (uint64_t)y + height > w->height ||
        px < 0 || py < 0 || px + width > s->fb_width ||
        py + height > s->fb_height) {
        return false;
    }
    *source = (SGIKonaGLCopySource) { .s = s, .window = w, .bank = bank };
    return true;
}

static void sgi_kona_gl_copy_overlap(SGIKonaState *s, const uint32_t *a)
{
    SGIKonaGLCopySource source;
    int32_t x = a[0], y = a[1];
    uint8_t flags[4] = { 0 };
    uint32_t rgba;

    if (!sgi_kona_gl_copy_source_ready(s, s->context_current, x, y,
                                       a[2], a[3], 0x1800, &source)) {
        goto refused;
    }
    for (uint32_t row = 0; row < a[3]; row++) {
        for (uint32_t col = 0; col < a[2]; col++) {
            uint32_t bits = s->state[s->context_current]->gl.cur.index_bits;

            if (bits ?
                !sgi_kona_gl_read_index(s, source.bank,
                    source.window->x + x + col, source.window->y + y + row,
                    bits, &rgba) :
                !sgi_kona_gl_copy_read(&source, x + col, y + row, &rgba)) {
                goto refused;
            }
        }
    }
    /* Model compatibility: a complete snapshot needs no chunk direction. */
    if (sgi_kona_post_return(s, SGI_KONA_CMD_GL_COPY_OVERLAP,
                             8, flags, sizeof(flags))) {
        return;
    }
refused:
    qemu_log_mask(LOG_UNIMP, "sgi-kona: CopyPixels overlap query refused;"
                  " no flags published\n");
}

static void sgi_kona_gl_copy_pixels(SGIKonaState *s, const uint32_t *a)
{
    int i = s->context_current;
    int32_t x = a[0], y = a[1];
    const SGIKonaGLPipe *p;
    SGIKonaGLCopySource source;
    SGIGLPixelTransfer transfer;
    SGIGLSink sink;
    float zoom_x, zoom_y;
    bool depth = a[5] == 0x1801, stencil = a[5] == 0x1802;
    bool unlocked, copied;

    if (i != SGI_KONA_CONTEXT_NONE && s->state[i]->gl.in_begin) {
        sgi_kona_gl_set_error(s, 0x502);
        goto refused;
    }
    if (stencil && i != SGI_KONA_CONTEXT_NONE &&
        sgi_kona_gl_stencil_bits(s, i) == 0) {
        sgi_kona_gl_set_error(s, 0x502);
    }
    if (a[4] || (!depth && !stencil && a[5] != 0x1800) ||
        (a[6] && a[6] != 0x80000000) ||
        !sgi_kona_gl_copy_source_ready(s, i, x, y, a[2], a[3],
                                        a[5], &source)) {
        goto refused;
    }
    p = &s->state[i]->pipe;
    zoom_x = sgi_kona_gl_float(p->pixel_zoom[0]);
    zoom_y = sgi_kona_gl_float(p->pixel_zoom[1]);
    /* The unqueried producer form does not establish its flag's meaning. */
    if (a[6] == 0x80000000 && (zoom_x != 1 || zoom_y != 1)) {
        goto refused;
    }
    sgi_kona_gl_sink(s, &sink);
    if (stencil) {
        uint64_t count = (uint64_t)a[2] * a[3];
        g_autofree uint32_t *pixels = g_try_new(uint32_t, count);

        if (!pixels) {
            sgi_kona_gl_set_error(s, 0x505);
            goto refused;
        }
        for (uint32_t row = 0; row < a[3]; row++) {
            for (uint32_t col = 0; col < a[2]; col++) {
                uint32_t index = sgi_kona_gl_load_stencil(s,
                    source.window->x + x + col, source.window->y + y + row);

                if (!sgi_kona_gl_stencil_index(p, index,
                        &pixels[(uint64_t)row * a[2] + col])) {
                    goto refused;
                }
            }
        }
        if (sgi_gl_draw_pixels_stencil(&s->state[i]->gl, &sink, a[2], a[3],
                                       pixels,
                                        zoom_x, zoom_y)) {
            return;
        }
        goto refused;
    }
    if (depth) {
        uint64_t count = (uint64_t)a[2] * a[3];
        g_autofree double *pixels = g_try_new(double, count);

        if (!pixels) {
            sgi_kona_gl_set_error(s, 0x505);
            goto refused;
        }
        /* Snapshot before overlapping destinations can change the source. */
        for (uint32_t row = 0; row < a[3]; row++) {
            for (uint32_t col = 0; col < a[2]; col++) {
                pixels[(uint64_t)row * a[2] + col] =
                    sgi_kona_gl_load_depth(s, source.window->x + x + col,
                                             source.window->y + y + row) /
                    16777215.0;
            }
        }
        if (sgi_gl_draw_pixels_depth_transfer(&s->state[i]->gl, &sink,
                                              a[2], a[3],
                                              pixels, zoom_x, zoom_y,
                sgi_kona_gl_float(p->pixel_transfer.depth_scale),
                sgi_kona_gl_float(p->pixel_transfer.depth_bias))) {
            return;
        }
        goto refused;
    }
    if (s->state[i]->gl.cur.index_bits) {
        uint64_t count = (uint64_t)a[2] * a[3];
        g_autofree uint32_t *pixels = g_try_new(uint32_t, count);

        if (!pixels) {
            sgi_kona_gl_set_error(s, 0x505);
            goto refused;
        }
        for (uint32_t row = 0; row < a[3]; row++) {
            for (uint32_t col = 0; col < a[2]; col++) {
                uint64_t at = (uint64_t)row * a[2] + col;
                uint32_t index;

                if (!sgi_kona_gl_read_index(s, source.bank,
                        source.window->x + x + col,
                        source.window->y + y + row,
                        s->state[i]->gl.cur.index_bits, &index) ||
                    !sgi_kona_gl_colour_index(&p->pixel_transfer,
                                               index, &pixels[at])) {
                    goto refused;
                }
            }
        }
        if (sgi_gl_draw_pixels_index(&s->state[i]->gl, &sink, a[2], a[3],
                                     pixels,
                                      zoom_x, zoom_y)) {
            return;
        }
        goto refused;
    }
    if (p->pixel_transfer.map_colour ||
        sgi_kona_gl_imaging_active(s->state[i])) {
        uint64_t count = (uint64_t)a[2] * a[3];
        g_autofree double (*pixels)[4] = g_try_malloc_n(count, sizeof(*pixels));
        const SGIGLPixelTransfer identity = { .scale = { 1, 1, 1, 1 } };
        uint32_t width = a[2], height = a[3];

        if (!pixels) {
            sgi_kona_gl_set_error(s, 0x505);
            goto refused;
        }
        for (uint32_t row = 0; row < a[3]; row++) {
            for (uint32_t col = 0; col < a[2]; col++) {
                uint64_t at = (uint64_t)row * a[2] + col;
                uint32_t rgba;

                if (!sgi_kona_gl_copy_read(&source, x + col, y + row, &rgba)) {
                    goto refused;
                }
                for (unsigned k = 0; k < 4; k++) {
                    pixels[at][k] = ((rgba >> (24 - k * 8)) & 255) / 255.0;
                }
                if (!sgi_kona_gl_rgba_transfer(&p->pixel_transfer,
                                                pixels[at])) {
                    goto refused;
                }
            }
        }
        if (sgi_kona_gl_imaging_active(s->state[i]) &&
            !sgi_kona_gl_imaging(s->state[i], &pixels, &width, &height)) {
            goto refused;
        }
        if (!width || !height ||
            sgi_gl_draw_pixels_double_transfer(&s->state[i]->gl, &sink,
                                                width, height,
                                                pixels, zoom_x, zoom_y,
                                                &identity)) {
            return;
        }
        goto refused;
    }
    for (unsigned k = 0; k < 4; k++) {
        transfer.scale[k] = sgi_kona_gl_float(p->pixel_transfer.scale[k]);
        transfer.bias[k] = sgi_kona_gl_float(p->pixel_transfer.bias[k]);
    }
    /*
     * The whole transfer, the source snapshot and the destination writes,
     * touches only board memory and command-stream state
     * (sgi_kona_work_unlock()).
     */
    unlocked = sgi_kona_work_unlock(s);
    copied = sgi_gl_copy_pixels_rgba_transfer(&s->state[i]->gl, &sink,
                                              x, y, a[2], a[3],
                                              sgi_kona_gl_copy_read, &source,
                                              zoom_x, zoom_y, &transfer);
    sgi_kona_work_relock(s, unlocked);
    if (copied) {
        return;
    }
refused:
    qemu_log_mask(LOG_UNIMP, "sgi-kona: native CopyPixels profile refused\n");
}

static uint64_t sgi_kona_gl_read_row_bytes(uint32_t width, uint32_t format,
                                          uint32_t type)
{
    if ((format == 11 || format == 12) && type == 7) {
        return DIV_ROUND_UP((uint64_t)width, 8);
    }
    return (uint64_t)width * sgi_kona_gl_native_pixel_bytes(format, type);
}

/* Whether a read source pixel has the tag its visual's read decodes. */
static bool sgi_kona_gl_read_tag_ok(SGIKonaState *s, int bank, int64_t x,
                                    int64_t y, bool indexed_visual)
{
    return sgi_plane_load(&s->mem,
                          sgi_kona_plane(s, sgi_kona_bank_tag_plane(s, bank)),
                          x, y) ==
           (indexed_visual ? SGI_KONA_TAG_X_INDEX16 : SGI_KONA_TAG_DIRECT);
}

/*
 * Reads stage dense rows for either return transport.
 * The pointer's low bits do not offset the framebuffer source.
 * The selected logical bank supplies direct RGB at drawable-local lower-
 * left coordinates, bottom-first. A request the model cannot answer
 * from its source returns false, logged with `outcome`, which says what
 * the caller does instead (see sgi_kona_gl_read_pixels()).
 *
 * A wire extent of 8 by 1 is what the client sends for a public 1 by 1
 * read, of which it copies only the first pixel; a genuine 8 by 1 read
 * sends the same (established from the client, see the command grammar).
 * Approximated: the model answers eight pixels from the request's
 * corner, those past the window or the screen as zero, and needs only
 * the first pixel inside. Both readings then get the pixels they copy.
 */
static bool sgi_kona_gl_read_payload(SGIKonaState *s, const uint32_t *args,
                                      uint32_t limit, uint8_t **out_data,
                                      uint32_t *out_count,
                                      const char *outcome)
{
    int i = s->context_current;
    int32_t x0 = args[0], y0 = args[1];
    uint32_t w = args[2], h = args[3], format = args[4], type = args[5];
    const SGIKonaGLWindow *win;
    const SGIKonaGLPipe *p;
    uint32_t pixel_bytes;
    uint64_t row, len;
    int64_t px, py, cols;
    bool wide8 = w == 8 && h == 1;
    uint64_t foreign = 0;
    bool first_ok = false;
    uint32_t selection;
    int bank;
    bool depth = format == 13, stencil = format == 12;
    bool bitmap = (stencil || format == 11) && type == 7;
    bool indexed_visual, post_conv;
    uint32_t index;
    float depth_scale, depth_bias;
    g_autofree uint8_t *data = NULL;

    if (i == SGI_KONA_CONTEXT_NONE) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels with no current"
                      " context; %s\n", outcome);
        return false;
    }
    p = &s->state[i]->pipe;
    indexed_visual = s->state[i]->gl.cur.index_bits != 0;
    if (format == 11 && !indexed_visual) {
        sgi_kona_gl_set_error(s, 0x502);
        return false;
    }
    if (s->state[i]->gl.in_begin || sgi_kona_gl_transfer_refused(s, i)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels pixel transfer"
                      " unsupported; %s\n", outcome);
        return false;
    }
    /*
     * A convolved read returns an image smaller than the read, and the
     * library's read path sizes its copy from the read alone (it reads
     * no convolution state; see the command grammar): what the pipe
     * returns then is unknown.
     */
    if (!depth && !stencil && format != 11 &&
        sgi_kona_gl_histogram_sinks(s->state[i])) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels into a sinking"
                      " histogram or minmax table is not modelled; %s\n",
                      outcome);
        return false;
    }
    if (!depth && !stencil && format != 11 &&
        sgi_kona_gl_filter_2d(&s->state[i]->imaging) >= 0) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels with a convolution"
                      " filter enabled is not modelled; %s\n", outcome);
        return false;
    }
    if ((depth && p->draw_config_received && !p->draw_config[6]) ||
        (stencil && sgi_kona_gl_stencil_bits(s, i) == 0)) {
        sgi_kona_gl_set_error(s, 0x502);
        return false;
    }
    if (stencil && !sgi_kona_gl_stencil_index(p, 0, &index)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: stencil read transfer "
                      "unsupported; %s\n", outcome);
        return false;
    }
    depth_scale = sgi_kona_gl_float(p->pixel_transfer.depth_scale);
    depth_bias = sgi_kona_gl_float(p->pixel_transfer.depth_bias);
    if (depth && (p->draw_config[6] != 24 || !isfinite(depth_scale) ||
                  !isfinite(depth_bias))) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels depth profile "
                      "unsupported; %s\n", outcome);
        return false;
    }
    for (unsigned k = 0; !depth && !stencil && !indexed_visual && k < 4; k++) {
        if (!isfinite(sgi_kona_gl_float(p->pixel_transfer.scale[k])) ||
            !isfinite(sgi_kona_gl_float(p->pixel_transfer.bias[k]))) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels nonfinite"
                          " transfer; %s\n", outcome);
            return false;
        }
    }
    row = sgi_kona_gl_read_row_bytes(w, format, type);
    if (!depth && !stencil && !sgi_kona_gl_pixel_colour_format(format)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels of CMYK format %"
                      PRIu32 " is not modelled; %s\n", format, outcome);
        return false;
    }
    if (!row || args[6] > 3) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels of format 0x%"
                      PRIx32 " type 0x%" PRIx32 " low bits 0x%" PRIx32
                      " is not modelled; %s\n", format, type, args[6],
                      outcome);
        return false;
    }
    /* Check the complete dense extent before allocating or writing. */
    pixel_bytes = sgi_kona_gl_native_pixel_bytes(format, type);
    if ((int32_t)w <= 0 || (int32_t)h <= 0 ||
        row > limit || h > limit / row) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels of %" PRIu32 "x%"
                      PRIu32 " has an unsupported extent; %s\n",
                      w, h, outcome);
        return false;
    }
    len = row * h;
    win = &s->state[i]->window;
    px = (int64_t)win->x + x0;
    py = (int64_t)win->y + y0;
    bank = depth ? (sgi_kona_gl_depth_source_ready(s, i) ? 0 : -1) :
           stencil ? (sgi_kona_gl_stencil_source_ready(s, i) ? 0 : -1) :
                     sgi_kona_gl_read_bank(s, i, &selection);
    /* An 8 by 1 wire extent needs only its first pixel inside. */
    cols = wide8 && x0 >= 0 && px >= 0 ?
           MIN(w, MIN((int64_t)win->width - x0,
                      (int64_t)s->fb_width - px)) : w;
    if (bank < 0 || (int64_t)cols <= 0 ||
        x0 < 0 || y0 < 0 || (uint64_t)x0 + cols > win->width ||
        (uint64_t)y0 + h > win->height || px < 0 || py < 0 ||
        px + cols > s->fb_width || py + h > s->fb_height) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels has no supported"
                      " pixel source; %s\n", outcome);
        return false;
    }
    /*
     * Pixels of another kind than the visual reads are answered zero and
     * the rest with their values, so a rectangle crossing a window's edge,
     * as oglsnoop's read around the pointer does, keeps its own pixels.
     * A rectangle with none of the read's kind is refused whole, and so is
     * an 8 by 1 read, a 1 by 1 caller's, whose first pixel is foreign.
     * Model policy: what the pipe returns for foreign pixels is unknown.
     */
    for (uint32_t r = 0; !depth && !stencil && r < h; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            if (!sgi_kona_gl_read_tag_ok(s, bank, px + c, py + r,
                                         indexed_visual)) {
                foreign++;
            } else if (!r && !c) {
                first_ok = true;
            }
        }
    }
    if (foreign == (uint64_t)cols * h || (wide8 && !first_ok && foreign)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels has an"
                      " unsupported source tag; %s\n", outcome);
        return false;
    }
    if (foreign) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels of %" PRIu32 "x%"
                      PRIu32 " answered %" PRIu64 " pixels of another kind"
                      " with zero\n", w, h, foreign);
    }
    post_conv = sgi_kona_gl_imaging_active(s->state[i]);
    data = g_malloc0(len);
    for (uint32_t r = 0; r < h; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            if (foreign && !depth && !stencil &&
                !sgi_kona_gl_read_tag_ok(s, bank, px + c, py + r,
                                         indexed_visual)) {
                continue;
            }
            if (stencil) {
                uint32_t raw = sgi_kona_gl_load_stencil(s, px + c, py + r);

                if (!sgi_kona_gl_stencil_index(p, raw, &index)) {
                    return false;
                }
                if (bitmap) {
                    unsigned bit = p->pack_lsb_first ? c & 7 : 7 - (c & 7);

                    data[r * row + c / 8] |= (index & 1) << bit;
                    continue;
                }
                if (!sgi_kona_gl_pixel_index_pack(type, index,
                        p->pack_swap_bytes, data + r * row + c * pixel_bytes,
                        pixel_bytes)) {
                    return false;
                }
                continue;
            }
            if (depth) {
                double values[4] = {
                    sgi_kona_gl_load_depth(s, px + c, py + r) /
                    16777215.0 * depth_scale + depth_bias, 0, 0, 1,
                };

                if (!sgi_kona_gl_pixel_pack_double(0, type, values,
                        p->pack_swap_bytes, data + r * row + c * pixel_bytes,
                        pixel_bytes)) {
                    return false;
                }
                continue;
            }
            if (indexed_visual) {
                double values[4];
                uint32_t raw;
                uint8_t *out = data + r * row + c * pixel_bytes;

                if (!sgi_kona_gl_read_index(s, bank, px + c, py + r,
                                             s->state[i]->gl.cur.index_bits,
                                             &raw)) {
                    return false;
                }
                if (format == 11) {
                    if (!sgi_kona_gl_colour_index(&p->pixel_transfer,
                                                   raw, &index)) {
                        return false;
                    }
                    if (bitmap) {
                        unsigned bit = p->pack_lsb_first ? c & 7 :
                                                           7 - (c & 7);

                        data[r * row + c / 8] |= (index & 1) << bit;
                    } else if (!sgi_kona_gl_pixel_index_pack(type, index,
                                   p->pack_swap_bytes, out, pixel_bytes)) {
                        return false;
                    }
                } else if (!sgi_kona_gl_index_rgba(&p->pixel_transfer,
                                                    raw, values) ||
                           (post_conv &&
                            !sgi_kona_gl_pixel_path(s->state[i], values)) ||
                           !sgi_kona_gl_pixel_pack_double(format, type, values,
                                   p->pack_swap_bytes, out, pixel_bytes)) {
                    return false;
                }
                continue;
            }
            uint32_t rgb = sgi_plane_load(&s->mem,
                sgi_kona_plane(s, sgi_kona_bank_plane(s, bank)),
                px + c, py + r);
            double source[4] = {
                ((rgb >> 16) & 255) / 255.0,
                ((rgb >> 8) & 255) / 255.0,
                (rgb & 255) / 255.0, 1,
            };
            uint8_t *out = data + r * row + c * pixel_bytes;

            if (!sgi_kona_gl_rgba_transfer(&p->pixel_transfer, source) ||
                (post_conv &&
                 !sgi_kona_gl_pixel_path(s->state[i], source)) ||
                !sgi_kona_gl_pixel_pack_double(format, type, source,
                                                  p->pack_swap_bytes,
                                         out, pixel_bytes)) {
                return false;
            }
        }
    }
    *out_data = g_steal_pointer(&data);
    *out_count = len;
    return true;
}

/* Descriptor word six remains restricted to the established X zero form. */
static bool sgi_kona_gl_read_dma_descriptor(const SGIKonaGLReadDMA *d,
                                            uint64_t *io, uint64_t *pitch,
                                            uint32_t *lines)
{
    const uint32_t *a = d->descriptor;
    uint64_t span;

    if ((a[1] & ~0xfffu) != 0x9000 || !a[2] || a[2] > 64 * MiB ||
        !a[3] || a[3] > a[2] || a[2] % a[3] || (int32_t)a[4] < 0 ||
        a[5]) {
        return false;
    }
    *io = ((uint64_t)a[0] << 12) | (a[1] & 0xfff);
    *lines = a[2] / a[3];
    *pitch = (uint64_t)a[3] + a[4];
    span = (uint64_t)(*lines - 1) * *pitch + a[3];
    return span <= 64 * MiB && span <= UINT64_MAX - *io;
}

static bool sgi_kona_gl_read_dma_layout(const SGIKonaGLReadDMA *d,
                                        const uint32_t *h, uint32_t selector)
{
    uint64_t io, pitch, row, total;
    uint32_t lines;

    if (selector == SGI_KONA_CMD_GL_READ_TEXELS) {
        /* h[0] and h[1]: the region's width and height, four bytes each. */
        row = (uint64_t)h[0] * 4;
        total = row * h[1];
        return row && h[1] &&
               sgi_kona_gl_read_dma_descriptor(d, &io, &pitch, &lines) &&
               d->descriptor[2] == total &&
               ((d->descriptor[3] == total && !d->descriptor[4]) ||
                d->descriptor[3] == row);
    }
    if (selector == SGI_KONA_CMD_GL_GET_HISTOGRAM) {
        /* h[0] bins of h[1] four-byte counts, in whole copies, one line. */
        row = (uint64_t)h[0] * h[1] * 4;
        return row && h[0] <= SGI_KONA_GL_HISTOGRAM_MAX && h[1] <= 4 &&
               sgi_kona_gl_read_dma_descriptor(d, &io, &pitch, &lines) &&
               d->descriptor[2] % row == 0 &&
               d->descriptor[3] == d->descriptor[2] && !d->descriptor[4];
    }
    if (selector == SGI_KONA_CMD_X_PIXEL_READ) {
        if ((h[4] != 1 && h[4] != 2 && h[4] != 4) || h[5] || h[6]) {
            return false;
        }
        row = (uint64_t)h[2] * h[4];
    } else if (selector == SGI_KONA_CMD_GL_READ_PIXELS) {
        row = sgi_kona_gl_read_row_bytes(h[2], h[4], h[5]);
        if ((h[2] == 8 && h[3] == 1) || h[6] > 3) {
            return false;
        }
    } else {
        return false;
    }
    if (!row || (int32_t)h[2] <= 0 || (int32_t)h[3] <= 0 ||
        h[2] > SGI_KONA_FB_MAX_DIMENSION ||
        h[3] > SGI_KONA_FB_MAX_DIMENSION ||
        !sgi_kona_gl_read_dma_descriptor(d, &io, &pitch, &lines) ||
        (selector == SGI_KONA_CMD_GL_READ_PIXELS && (io & 3) != h[6])) {
        return false;
    }
    total = row * h[3];
    return d->descriptor[2] == total &&
           ((d->descriptor[3] == total && !d->descriptor[4]) ||
            d->descriptor[3] == row);
}

static void sgi_kona_gl_read_dma_start(SGIKonaState *s, const uint32_t *a)
{
    SGIKonaGLReadDMA *d = &s->gl_read_dma;
    uint64_t io, pitch;
    uint32_t lines;

    sgi_kona_gl_read_dma_clear(s);
    d->active = true;
    d->context = s->context_current;
    d->generation = sgi_kona_context_generation(s, d->context);
    memcpy(d->descriptor, a, sizeof(d->descriptor));
    d->render_mode = d->context == SGI_KONA_CONTEXT_NONE ?
                     SGI_KONA_GL_RENDER : s->state[d->context]->render.mode;
    if (d->context == SGI_KONA_CONTEXT_NONE ||
        !sgi_kona_gl_read_dma_descriptor(d, &io, &pitch, &lines)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: read DMA descriptor refused\n");
        d->refused = true;
    }
}

/*
 * A glReadPixels the model cannot answer from its source still completes,
 * as a refused X pixel read does (sgi_kona_x_read_answer_refused()): the
 * client waits for its reply with no timeout, and oglsnoop blocked there
 * before drawing anything. The reply is the whole usable return page
 * zeroed, then the ready word; inside a matched DMA bracket, the
 * descriptor's byte count of zeros. Model policy: what the pipe answers
 * to a read it cannot perform is unknown. A read with no page of the
 * current context's own is not answered, and a DMA request whose bracket,
 * descriptor or owner does not match is refused as before.
 */
static void sgi_kona_gl_read_pixels(SGIKonaState *s, const uint32_t *args)
{
    SGIKonaGLReadDMA *d = &s->gl_read_dma;
    g_autofree uint8_t *data = NULL;
    bool answer = sgi_kona_x_read_current(s);
    const char *outcome = answer ? "answered with zeros" : "not answered";
    uint32_t count;

    if (d->active) {
        if (d->refused || d->count || d->context != s->context_current ||
            !sgi_kona_context_owner(s, d->context, d->generation) ||
            !sgi_kona_gl_read_dma_layout(d, args,
                                            SGI_KONA_CMD_GL_READ_PIXELS)) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: ReadPixels DMA refused\n");
            sgi_kona_gl_read_dma_refuse(s);
            return;
        }
        if (!sgi_kona_gl_read_payload(s, args, 64 * MiB, &data, &count,
                                      outcome)) {
            if (!answer) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-kona: ReadPixels DMA refused\n");
                sgi_kona_gl_read_dma_refuse(s);
                return;
            }
            count = d->descriptor[2];
            data = g_malloc0(count);
        }
        memcpy(d->header, args, sizeof(d->header));
        d->selector = SGI_KONA_CMD_GL_READ_PIXELS;
        d->count = count;
        d->data = g_steal_pointer(&data);
    } else if (sgi_kona_gl_read_payload(s, args, SGI_KONA_SMALL_RETURN_BYTES,
                                         &data, &count, outcome)) {
        trace_sgi_kona_gl_reply(SGI_KONA_CMD_GL_READ_PIXELS, args[4], count);
        sgi_kona_post_return(s, SGI_KONA_CMD_GL_READ_PIXELS, 8, data, count);
    } else if (answer) {
        g_autofree uint8_t *zeros =
            g_malloc0(SGI_KONA_RETURN_PAGE_BYTES - 8);

        sgi_kona_post_return(s, SGI_KONA_CMD_GL_READ_PIXELS, 8, zeros,
                             SGI_KONA_RETURN_PAGE_BYTES - 8);
    }
}

/*
 * A texture copy: the format, type and storage code a texel readback
 * writes and a later load reads back, for a texture stored as
 * SGI_KONA_TEXTURE_COPIED_STORAGE (see sgi_kona_gl_read_texels()).
 */
#define SGI_KONA_TEXTURE_COPY_FORMAT    7
#define SGI_KONA_TEXTURE_COPY_TYPE      3
#define SGI_KONA_TEXTURE_COPY_STORAGE   SGI_KONA_GL_STORAGE_COPY
#define SGI_KONA_TEXTURE_COPIED_STORAGE 0x30

/*
 * 0x52, a texel readback: before the library gives a resident texture's pipe
 * memory to another, it copies the texture to host memory with this command
 * inside a select-feed bracket, and loads the copy back later with 0x174 when
 * the texture is used again. Its words, from the reader's descriptor in the GL
 * client library: 0 the target, 1 and 2 the width and height, 3 and 4 the
 * format and type the copy is written in, 5 the copy's storage code, 7 to 12
 * the region's first and last texel, 14 the geometry word and 16 and 17 the two
 * bank addresses a texel load names the texture's destination by (load header
 * words 16, 18 and 19). Established from the library and a guest trace: a
 * 1024x1024 texture stored as 0x30 was read back as format 7, type 3, copy
 * storage 0x28, and loaded back with those same three codes. Outside a bracket
 * the library waits on the return page.
 *
 * Inferred: bit 28 of word 13 is the bank a load's word 17 bit 3 names.
 * Words 13 and 15 come from the texture's bind: word 13 is the third word
 * of the bind command (0x1d5 or 0x106), whose bit 28 the model already
 * reads as the first level's bank, and word 15 the first word of the 0x179
 * record that follows it, the format and layout byte 0x43 or 0x44, which
 * says nothing of the bank. GLperf's mipmapped TexImage case read back a
 * 256x256 level loaded in bank one with word 13 0x138c0024 and word 15
 * 0x43; every earlier readback observed came from bank zero, with bit 28
 * clear. Word 6 (zero in every readback observed) and the rest of words 13
 * and 15 are not checked: what they carry is unknown.
 *
 * Approximated: the model keeps a texture as 32-bit RGBA texels, not in
 * the pipe's memory layout, and the copy it writes is those words,
 * big-endian. The library treats the copy as opaque and loads the same
 * bytes back (see sgi_kona_texture_copy()), so a texture survives the
 * round trip; what a real pipe writes is unknown, and a client that read
 * the copy itself would see the model's encoding. Only the observed copy
 * of 0x30 storage is accepted; any other is refused and logged.
 */
/*
 * A texel readback the model cannot answer, inside a select-feed bracket,
 * still completes: the target gets the descriptor's bytes as zeros, as a
 * ReadPixels the model cannot answer does. Without a completion the
 * kernel's select-feed timeout declares the pipe dead (death reason 5),
 * and a model refusal must not do that. The library then loads the zeros
 * back as the texture, so it draws black: logged as a guest error. A
 * bracket that is itself refused, or whose descriptor lines are not whole
 * texels, is still refused.
 *
 * Only a readback that names no texture the model holds is logged as a
 * guest error; that includes one whose extent no stored image covers,
 * which matches nothing. Copy codes (format, type or storage) the library
 * was not seen to ask for, an ambiguous match, or a found texture whose
 * target or extent the model does not copy are the model's gap, logged as
 * unimplemented.
 */
static void sgi_kona_gl_read_texels_zero(SGIKonaState *s)
{
    SGIKonaGLReadDMA *d = &s->gl_read_dma;
    uint64_t io, pitch;
    uint32_t lines;

    if (d->refused || d->count || d->context != s->context_current ||
        !sgi_kona_context_owner(s, d->context, d->generation) ||
        !sgi_kona_gl_read_dma_descriptor(d, &io, &pitch, &lines) ||
        d->descriptor[3] % 4) {
        sgi_kona_gl_read_dma_refuse(s);
        return;
    }
    memset(d->header, 0, sizeof(d->header));
    d->header[0] = d->descriptor[3] / 4;
    d->header[1] = lines;
    d->selector = SGI_KONA_CMD_GL_READ_TEXELS;
    d->count = d->descriptor[2];
    d->data = g_malloc0(d->count);
}

static void sgi_kona_gl_read_texels(SGIKonaState *s, const uint32_t *args)
{
    SGIKonaGLReadDMA *d = &s->gl_read_dma;
    const SGIKonaTextureLevel *image = NULL;
    uint32_t handle = UINT32_MAX, width, height, found = 0;
    g_autofree uint8_t *data = NULL;
    uint64_t bytes;

    width = args[10] - args[7] + 1;
    height = args[11] - args[8] + 1;
    if (args[3] == SGI_KONA_TEXTURE_COPY_FORMAT &&
        args[4] == SGI_KONA_TEXTURE_COPY_TYPE &&
        args[5] == SGI_KONA_TEXTURE_COPY_STORAGE) {
        found = sgi_kona_texture_find_native(&s->textures, args[0],
                                             SGI_KONA_TEXTURE_COPIED_STORAGE,
                                             args[14],
                                             (args[13] >> 28) & 1,
                                             args[16], args[17],
                                             args[10] + 1, args[11] + 1,
                                             &handle, &image);
    }
    trace_sgi_kona_gl_read_texels(args[0], width, height, args[15],
                                  args[16], args[17],
                                  found == 1 ? handle : UINT32_MAX);
    bytes = (uint64_t)width * height * 4;
    if (found != 1 || args[0] != SGI_GL_TEXTURE_2D || args[9] || args[12] ||
        args[10] < args[7] || args[11] < args[8] ||
        bytes > SGI_KONA_TEXTURE_BYTES) {
        bool copy = args[3] == SGI_KONA_TEXTURE_COPY_FORMAT &&
                    args[4] == SGI_KONA_TEXTURE_COPY_TYPE &&
                    args[5] == SGI_KONA_TEXTURE_COPY_STORAGE;

        qemu_log_mask(copy && !found ? LOG_GUEST_ERROR : LOG_UNIMP,
                      "sgi-kona: texel readback of %u "
                      "textures matching banks 0x%x 0x%x storage 0x%x; %s\n",
                      found, args[16], args[17], args[5],
                      d->active ? "answered with zeros" : "refused");
        if (d->active) {
            sgi_kona_gl_read_texels_zero(s);
        }
        return;
    }
    data = g_malloc(bytes);
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t *row = image->pixels +
                              (uint64_t)(args[8] + y) * image->width + args[7];

        for (uint32_t x = 0; x < width; x++) {
            stl_be_p(data + ((uint64_t)y * width + x) * 4, row[x]);
        }
    }
    if (d->active) {
        if (d->refused || d->count || d->context != s->context_current ||
            !sgi_kona_context_owner(s, d->context, d->generation)) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: texel readback DMA "
                          "refused\n");
            sgi_kona_gl_read_dma_refuse(s);
            return;
        }
        memset(d->header, 0, sizeof(d->header));
        d->header[0] = width;
        d->header[1] = height;
        if (!sgi_kona_gl_read_dma_layout(d, d->header,
                                         SGI_KONA_CMD_GL_READ_TEXELS)) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: texel readback DMA "
                          "refused\n");
            sgi_kona_gl_read_dma_refuse(s);
            return;
        }
        d->selector = SGI_KONA_CMD_GL_READ_TEXELS;
        d->count = bytes;
        d->data = g_steal_pointer(&data);
    } else if (bytes <= SGI_KONA_SMALL_RETURN_BYTES &&
               sgi_kona_x_read_current(s)) {
        trace_sgi_kona_gl_reply(SGI_KONA_CMD_GL_READ_TEXELS, handle, bytes);
        sgi_kona_post_return(s, SGI_KONA_CMD_GL_READ_TEXELS, 8, data, bytes);
    } else {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: texel readback of 0x%" PRIx64
                      " bytes outside a DMA bracket; not answered\n", bytes);
    }
}

static bool sgi_kona_device_write(SGIKonaState *s, uint64_t device,
                                   const uint8_t *buf, uint32_t len,
                                   bool validate);

/*
 * The finish of a feedback or selection pass's select feed, with nothing
 * read inside it. Word 2 of the completion is the bytes left unwritten,
 * since the kernel returns (total - word 2) / 4 as the word count, and
 * bit 0x800 of word 3 reports that a hit record did not fit. A feedback
 * pass, whose records the model does not write, completes with none.
 */
static bool sgi_kona_gl_render_feed_finish(SGIKonaState *s,
                                           const uint32_t *a)
{
    SGIKonaGLReadDMA *d = &s->gl_read_dma;

    if (!d->active || d->refused || d->count || d->selector ||
        d->render_mode == SGI_KONA_GL_RENDER || a[0] != 0x100 ||
        d->context != s->context_current ||
        !sgi_kona_context_owner(s, d->context, d->generation) ||
        !s->reason_page_valid) {
        return false;
    }
    sgi_kona_post_reason_words(s, SGI_KONA_CMD_SELECT_FEED_FINISH,
                               (uint32_t[]){ 0x100, a[1],
                                             d->descriptor[2] - d->written,
                                             d->overflow ? 0x800 : 0 }, 4);
    sgi_kona_gl_read_dma_clear(s);
    return true;
}

static void sgi_kona_gl_read_dma_finish(SGIKonaState *s, const uint32_t *a)
{
    SGIKonaGLReadDMA *d = &s->gl_read_dma;
    uint64_t io, pitch;
    uint32_t lines;

    if (sgi_kona_gl_render_feed_finish(s, a)) {
        return;
    }
    if (!d->active || d->refused || !d->count || a[0] != 0x100 ||
        d->context != s->context_current ||
        !sgi_kona_context_owner(s, d->context, d->generation) ||
        !s->reason_page_valid ||
        !sgi_kona_gl_read_dma_layout(d, d->header, d->selector) ||
        !sgi_kona_gl_read_dma_descriptor(d, &io, &pitch, &lines)) {
        goto refused;
    }
    /* An absent later map must not leave an earlier row partly written. */
    for (uint32_t row = 0; row < lines; row++) {
        if (!sgi_kona_device_write(s, io + row * pitch, NULL,
                                     d->descriptor[3], true)) {
            goto refused;
        }
    }
    for (uint32_t row = 0; row < lines; row++) {
        if (!sgi_kona_device_write(s, io + row * pitch,
                                     d->data + row * d->descriptor[3],
                                     d->descriptor[3], false)) {
            goto refused;
        }
    }
    sgi_kona_post_reason_words(s, SGI_KONA_CMD_SELECT_FEED_FINISH,
                               (uint32_t[]){ 0x100, a[1], 0, 0 }, 4);
    sgi_kona_gl_read_dma_clear(s);
    return;

refused:
    qemu_log_mask(LOG_UNIMP, "sgi-kona: incomplete or failed read DMA;"
                  " no completion posted\n");
    sgi_kona_gl_read_dma_clear(s);
}

/*
 * GL_DRAW_BUFFER's value: the enumerant glDrawBuffer last gave, else the
 * one the draw mask holds, the visual's FRONT or BACK default (OpenGL
 * 1.2.1 section 4.2.1). A NONE leaves the mask empty, so either way it
 * reads back as NONE.
 */
static uint32_t sgi_kona_gl_draw_buffer(SGIKonaGLState *st)
{
    uint32_t mask = st->gl.cur.draw_buffers &
                    (SGI_GL_DRAW_FRONT | SGI_GL_DRAW_BACK);

    if (st->pipe.returns.draw_buffer) {
        return st->pipe.returns.draw_buffer;
    }
    return mask == (SGI_GL_DRAW_FRONT | SGI_GL_DRAW_BACK) ?
               SGI_GL_FRONT_AND_BACK :
           mask == SGI_GL_DRAW_BACK ? SGI_GL_BACK :
           mask == SGI_GL_DRAW_FRONT ? SGI_GL_FRONT : 0;
}

/* Native descriptors authorize layout; the source mapping authorizes values. */
static void sgi_kona_gl_get(SGIKonaState *s, uint32_t name)
{
    static const SGIKonaGLQueryInfo current_colour = { 0xb00, 0x03, 4 };
    const SGIKonaGLQueryInfo *info = sgi_kona_gl_query_lookup(name);
    uint32_t out[SGI_GL_GET_WORDS];
    uint64_t native[SGI_GL_GET_WORDS];
    uint8_t bytes[SGI_GL_GET_WORDS * 8];
    size_t length;
    unsigned n = 0;
    int i = s->context_current;

    if (name == current_colour.pname) {
        /* Explicit model compatibility, not an observed kind-2 descriptor. */
        info = &current_colour;
    }
    if (i == SGI_KONA_CONTEXT_NONE || !info || name == 0x2a00) {
        goto unsupported;
    }
    if (name == 0xb70) {
        memcpy(native, s->state[i]->pipe.returns.depth_range,
               2 * sizeof(*native));
        n = 2;
    } else if (name == 0xb73) {
        native[0] = s->state[i]->pipe.returns.clear_depth;
        n = 1;
    } else if (name == 0xbb0) {
        native[0] = s->state[i]->attrib_depth;
        n = 1;
    } else if (name == 0xd70) {
        /* GL_NAME_STACK_DEPTH: the names on the stack now. */
        native[0] = s->state[i]->render.name_count;
        n = 1;
    } else if (name >= 0xcb0 && name <= 0xcb9) {
        const SGIKonaGLPixelTransfer *t = &s->state[i]->pipe.pixel_transfer;
        const SGIKonaGLPixelMap *map = name == 0xcb0 ? &t->index_map :
            name == 0xcb1 ? &t->stencil_map :
            name <= 0xcb5 ? &t->index_rgba[name - 0xcb2] :
                           &t->colour_rgba[name - 0xcb6];

        /* Empty storage represents the public one-entry zero table. */
        native[0] = map->count ? map->count : 1;
        n = 1;
    } else if (name >= 0x8010 && name <= 0x8012) {
        /* The convolution enables. */
        native[0] = (s->state[i]->imaging.enabled >> (name - 0x8010)) & 1;
        n = 1;
    } else if (name == 0x80b1) {
        /* GL_COLOR_MATRIX, the float bits in column order. */
        for (unsigned k = 0; k < 16; k++) {
            native[k] = s->state[i]->imaging.colour_matrix[k];
        }
        n = 16;
    } else if (name == 0x80b2) {
        native[0] = s->state[i]->imaging.colour_depth + 1;
        n = 1;
    } else if (name >= 0x80b4 && name <= 0x80bb) {
        const SGIKonaGLPixelTransfer *t = &s->state[i]->pipe.pixel_transfer;

        native[0] = name <= 0x80b7 ? t->post_matrix_scale[name - 0x80b4] :
                                     t->post_matrix_bias[name - 0x80b8];
        n = 1;
    } else if (name == 0xc01) {
        native[0] = sgi_kona_gl_draw_buffer(s->state[i]);
        n = 1;
    } else if (name == 0xb80) {
        /* GL_ACCUM_CLEAR_VALUE, as glClearAccum sent it. */
        for (n = 0; n < 4; n++) {
            native[n] = s->state[i]->pipe.accum_clear[n];
        }
    } else if (sgi_kona_gl_multisample_get(&s->state[i]->pipe, name,
                                           &native[0])) {
        n = 1;
    } else if (name == 0xc02) {
        if (sgi_kona_gl_read_bank(s, i, &out[0]) < 0) {
            goto unsupported;
        }
        native[0] = out[0];
        n = 1;
    } else {
        if (!sgi_gl_get(&s->state[i]->gl, name, out, &n) ||
            (info->type != 0x03 && info->type != 0x04 && info->type != 0x13)) {
            goto unsupported;
        }
        for (unsigned k = 0; k < n; k++) {
            native[k] = out[k];
        }
    }
    if (!sgi_kona_gl_query_serialize(info, native, n, bytes, sizeof(bytes),
                                     &length)) {
        goto unsupported;
    }
    trace_sgi_kona_gl_reply(SGI_KONA_CMD_GL_GET, name, length);
    sgi_kona_post_return(s, SGI_KONA_CMD_GL_GET, 8, bytes, length);
    return;

unsupported:
    qemu_log_mask(LOG_UNIMP, "sgi-kona: glGet of 0x%" PRIx32 " is not"
                  " answered; no reply\n", name);
}

/* A float's bits into reply word k. */
static void sgi_kona_reply_f(uint8_t *w, unsigned k, float f)
{
    stl_be_p(w + 4 * k, sgi_kona_gl_bits(f));
}

static void sgi_kona_reply_fv(uint8_t *w, unsigned k, const float *v,
                              unsigned n)
{
    for (unsigned j = 0; j < n; j++) {
        sgi_kona_reply_f(w, k + j, v[j]);
    }
}

/* A material face as 0xc9's lighting section holds it: 24 words. */
static void sgi_kona_reply_material(uint8_t *w, const SGIGLMaterial *m)
{
    sgi_kona_reply_fv(w, 0, m->ambient, 4);
    sgi_kona_reply_fv(w, 4, m->diffuse, 4);
    sgi_kona_reply_fv(w, 8, m->specular, 4);
    sgi_kona_reply_fv(w, 12, m->emission, 4);
    sgi_kona_reply_f(w, 16, m->shininess);
    sgi_kona_reply_fv(w, 20, m->color_indexes, 3);
}

/*
 * The words of each 0xc9 reply section, in the order the reader takes
 * them (established): the current state, fog, lighting (before its per
 * light words), polygon offset, polygon stipple and texture.
 */
#define SGI_KONA_PUSH_CURRENT_WORDS  28
#define SGI_KONA_PUSH_FOG_WORDS      (0x144 / 4)
#define SGI_KONA_PUSH_LIGHTING_WORDS (0xc0 / 4)
#define SGI_KONA_PUSH_OFFSET_WORDS   4

/*
 * 0xc9's reply for mask, from byte 4 of the return page: one section for
 * each of the mask bits the library's reader takes, in its order, with no
 * gap. The section sizes are established from the reader. Which value
 * each word holds is not: the model fills them by the OpenGL 1.2.1
 * attribute groups the bits name, ordered as the reader's integer and
 * float conversions suggest (inferred):
 *
 *   0x1    colour, index, normal, texture coordinates, edge flag, raster
 *          position and distance, raster colour, raster texture
 *          coordinates, raster validity, raster index
 *   0x80   mode, density, start, end, colour, index; no fog-function
 *          table (count 0)
 *   0x40   the front then the back material, 24 words each (ambient,
 *          diffuse, specular, emission, shininess, three unread words,
 *          the colour indexes, an unread word), then each of the
 *          context's eight lights' ambient, diffuse and specular colours
 *   0x8    the factor in word 1 and the units in word 3, the two words
 *          read
 *   0x10   the stipple's 32 rows, leftmost pixel in the high bit
 *
 * Texture (0x40000) and transform (0x1000) sections have no complete
 * model reply and are refused before reaching this serializer.
 * Returns the reply's bytes.
 */
static uint32_t sgi_kona_gl_push_reply(SGIKonaState *s, int i, uint32_t mask,
                                       uint8_t *w, uint32_t room)
{
    const SGIGLContext *c = &s->state[i]->gl;
    uint32_t used = 0;

    memset(w, 0, room);
    if (mask & 0x1) {
        uint8_t *p = w + used;

        sgi_kona_reply_fv(p, 0, c->cur_color, 4);
        sgi_kona_reply_f(p, 4, c->cur_index);
        sgi_kona_reply_fv(p, 5, c->cur_normal, 3);
        sgi_kona_reply_fv(p, 8, c->cur_texcoord, 4);
        stl_be_p(p + 4 * 12, c->cur_edge);
        sgi_kona_reply_fv(p, 13, c->raster_pos, 4);
        sgi_kona_reply_f(p, 17, c->raster_distance);
        sgi_kona_reply_fv(p, 18, c->raster_color, 4);
        sgi_kona_reply_fv(p, 22, c->raster_texcoord, 4);
        stl_be_p(p + 4 * 26, c->raster_valid);
        sgi_kona_reply_f(p, 27, c->raster_index);
        used += 4 * SGI_KONA_PUSH_CURRENT_WORDS;
    }
    if (mask & 0x80) {
        uint8_t *p = w + used;

        stl_be_p(p, c->cur.fog_mode);
        sgi_kona_reply_f(p, 1, c->cur.fog_density);
        sgi_kona_reply_f(p, 2, c->cur.fog_start);
        sgi_kona_reply_f(p, 3, c->cur.fog_end);
        sgi_kona_reply_fv(p, 4, c->cur.fog_color, 4);
        sgi_kona_reply_f(p, 8, c->cur.fog_index);
        used += 4 * SGI_KONA_PUSH_FOG_WORDS;
    }
    if (mask & 0x40) {
        sgi_kona_reply_material(w + used, &c->material[0]);
        sgi_kona_reply_material(w + used + 4 * 24, &c->material[1]);
        used += 4 * SGI_KONA_PUSH_LIGHTING_WORDS;
        for (int l = 0; l < SGI_GL_LIGHTS; l++) {
            sgi_kona_reply_fv(w + used, 0, c->light[l].ambient, 4);
            sgi_kona_reply_fv(w + used, 4, c->light[l].diffuse, 4);
            sgi_kona_reply_fv(w + used, 8, c->light[l].specular, 4);
            used += 4 * SGI_KONA_GL_POP_LIGHT_WORDS;
        }
    }
    if (mask & 0x8) {
        sgi_kona_reply_f(w + used, 1, c->cur.offset_factor);
        sgi_kona_reply_f(w + used, 3, c->cur.offset_units);
        used += 4 * SGI_KONA_PUSH_OFFSET_WORDS;
    }
    if (mask & 0x10) {
        for (int r = 0; r < SGI_KONA_GL_STIPPLE_ROWS; r++) {
            stl_be_p(w + used + 4 * r, revbit32(c->polygon_stipple[r]));
        }
        used += 4 * SGI_KONA_GL_STIPPLE_ROWS;
    }
    assert(used <= room);
    return used;
}

/*
 * 0xc9 mask, glPushAttrib: the client waits for the reply above, and the
 * pipe keeps its own copy of the context to restore at 0xc8 (inferred:
 * the library restores only the groups it holds itself). The stack holds
 * SGI_KONA_GL_ATTRIB_DEPTH copies, OpenGL 1.2.1's least; a push onto a
 * full stack is logged and keeps nothing, as GL_STACK_OVERFLOW requires,
 * but is still answered, since the client waits either way.
 */
static void sgi_kona_gl_push_attrib(SGIKonaState *s, uint32_t mask)
{
    int i = s->context_current;
    uint8_t reply[4 * (SGI_KONA_PUSH_CURRENT_WORDS + SGI_KONA_PUSH_FOG_WORDS +
                       SGI_KONA_PUSH_LIGHTING_WORDS +
                       SGI_GL_LIGHTS * SGI_KONA_GL_POP_LIGHT_WORDS +
                       SGI_KONA_PUSH_OFFSET_WORDS + SGI_KONA_GL_STIPPLE_ROWS)];
    uint32_t len;
    uint32_t *depth;

    if (i == SGI_KONA_CONTEXT_NONE) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glPushAttrib with no current"
                      " context; no reply\n");
        return;
    }
    if (mask & (0x40000 | 0x1000)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glPushAttrib mask 0x%" PRIx32
                      " requests unmodelled texture or transform data;"
                      " no reply\n", mask);
        return;
    }
    len = sgi_kona_gl_push_reply(s, i, mask, reply, sizeof(reply));
    depth = &s->state[i]->attrib_depth;
    if (*depth >= SGI_KONA_GL_ATTRIB_DEPTH) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: glPushAttrib on a full"
                      " attribute stack; nothing kept\n");
    } else if (sgi_gl_clone(&s->state[i]->attrib[*depth], &s->state[i]->gl)) {
        s->state[i]->return_attrib[*depth] =
            s->state[i]->pipe.returns;
        s->state[i]->zoom_attrib[*depth] =
            (SGIKonaGLPixelZoom) {
                .factors = { s->state[i]->pipe.pixel_zoom[0],
                             s->state[i]->pipe.pixel_zoom[1] },
                .unsupported = s->state[i]->pipe.pixel_zoom_unsupported,
            };
        s->state[i]->transfer_attrib[*depth] =
            s->state[i]->pipe.pixel_transfer;
        /* The imaging enables: convolution low, the other stages above. */
        s->state[i]->imaging_enabled_attrib[*depth] =
            s->state[i]->imaging.enabled |
            s->state[i]->imaging.stages << 3;
        s->state[i]->attrib_mask[*depth] = mask;
        ++*depth;
    } else {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glPushAttrib could not keep"
                      " a copy of the context\n");
    }
    trace_sgi_kona_gl_reply(SGI_KONA_CMD_GL_PUSH_ATTRIB, mask, len);
    sgi_kona_post_return(s, SGI_KONA_CMD_GL_PUSH_ATTRIB, 4, reply, len);
}

#define SGI_KONA_RESTORE(field) (c->field = saved->field)

/*
 * Restore the groups mask names from saved into c, as OpenGL 1.2.1's
 * glPushAttrib groups divide the state the shared context keeps. The
 * drawable is the pipe's, not the client's, and is never restored.
 */
static void sgi_kona_gl_restore(SGIGLContext *c, const SGIGLContext *saved,
                                uint32_t mask)
{
    const uint32_t enables_point = SGI_GL_RASTER_POINT_SMOOTH;
    const uint32_t enables_line = SGI_GL_RASTER_LINE_SMOOTH |
                                  SGI_GL_RASTER_LINE_STIPPLE;
    const uint32_t enables_polygon = SGI_GL_RASTER_POLYGON_SMOOTH |
                                     SGI_GL_RASTER_POLYGON_STIPPLE;
    uint32_t enables = 0;

    if (mask & 0x1) {                           /* GL_CURRENT_BIT */
        SGI_KONA_RESTORE(cur_index);
        SGI_KONA_RESTORE(cur_edge);
        SGI_KONA_RESTORE(raster_valid);
        SGI_KONA_RESTORE(raster_distance);
        SGI_KONA_RESTORE(raster_index);
        memcpy(c->cur_color, saved->cur_color, sizeof(c->cur_color));
        memcpy(c->cur_normal, saved->cur_normal, sizeof(c->cur_normal));
        memcpy(c->cur_texcoord, saved->cur_texcoord,
               sizeof(c->cur_texcoord));
        memcpy(c->raster_pos, saved->raster_pos, sizeof(c->raster_pos));
        memcpy(c->raster_color, saved->raster_color,
               sizeof(c->raster_color));
        memcpy(c->raster_texcoord, saved->raster_texcoord,
               sizeof(c->raster_texcoord));
    }
    if (mask & 0x2) {                           /* GL_POINT_BIT */
        SGI_KONA_RESTORE(point_size);
        enables |= enables_point;
    }
    if (mask & 0x4) {                           /* GL_LINE_BIT */
        SGI_KONA_RESTORE(line_width);
        SGI_KONA_RESTORE(line_stipple_repeat);
        SGI_KONA_RESTORE(line_stipple_pattern);
        enables |= enables_line;
    }
    if (mask & 0x8) {                           /* GL_POLYGON_BIT */
        SGI_KONA_RESTORE(cur.cull_face);
        SGI_KONA_RESTORE(cur.cull_face_mode);
        SGI_KONA_RESTORE(cur.front_face);
        SGI_KONA_RESTORE(cur.polygon_offset);
        SGI_KONA_RESTORE(cur.offset_factor);
        SGI_KONA_RESTORE(cur.offset_units);
        SGI_KONA_RESTORE(cur.offset_bias);
        memcpy(c->polygon_mode, saved->polygon_mode,
               sizeof(c->polygon_mode));
        enables |= enables_polygon;
    }
    if (mask & 0x10) {                          /* GL_POLYGON_STIPPLE_BIT */
        memcpy(c->polygon_stipple, saved->polygon_stipple,
               sizeof(c->polygon_stipple));
    }
    if (mask & 0x40) {                          /* GL_LIGHTING_BIT */
        memcpy(c->light, saved->light, sizeof(c->light));
        memcpy(c->material, saved->material, sizeof(c->material));
        memcpy(c->light_model_ambient, saved->light_model_ambient,
               sizeof(c->light_model_ambient));
        SGI_KONA_RESTORE(light_model_local_viewer);
        SGI_KONA_RESTORE(light_model_two_side);
        SGI_KONA_RESTORE(color_material);
        SGI_KONA_RESTORE(color_material_face);
        SGI_KONA_RESTORE(color_material_mode);
        SGI_KONA_RESTORE(lighting);
        SGI_KONA_RESTORE(lights_enabled);
        SGI_KONA_RESTORE(cur.shade_model);
    }
    if (mask & 0x80) {                          /* GL_FOG_BIT */
        SGI_KONA_RESTORE(cur.fog);
        SGI_KONA_RESTORE(cur.fog_mode);
        SGI_KONA_RESTORE(cur.fog_density);
        SGI_KONA_RESTORE(cur.fog_start);
        SGI_KONA_RESTORE(cur.fog_end);
        SGI_KONA_RESTORE(cur.fog_index);
        memcpy(c->cur.fog_color, saved->cur.fog_color,
               sizeof(c->cur.fog_color));
    }
    if (mask & 0x100) {                         /* GL_DEPTH_BUFFER_BIT */
        SGI_KONA_RESTORE(cur.depth_test);
        SGI_KONA_RESTORE(cur.depth_func);
        SGI_KONA_RESTORE(cur.depth_mask);
        SGI_KONA_RESTORE(clear_depth);
    }
    if (mask & 0x400) {                         /* GL_STENCIL_BUFFER_BIT */
        SGI_KONA_RESTORE(cur.stencil_test);
        SGI_KONA_RESTORE(cur.stencil_func);
        SGI_KONA_RESTORE(cur.stencil_ref);
        SGI_KONA_RESTORE(cur.stencil_value_mask);
        SGI_KONA_RESTORE(cur.stencil_write_mask);
        SGI_KONA_RESTORE(clear_stencil);
        memcpy(c->cur.stencil_op, saved->cur.stencil_op,
               sizeof(c->cur.stencil_op));
    }
    if (mask & 0x800) {                         /* GL_VIEWPORT_BIT */
        memcpy(c->cur.viewport, saved->cur.viewport,
               sizeof(c->cur.viewport));
        memcpy(c->cur.depth_range, saved->cur.depth_range,
               sizeof(c->cur.depth_range));
    }
    if (mask & 0x1000) {                        /* GL_TRANSFORM_BIT */
        SGI_KONA_RESTORE(matrix_mode);
        SGI_KONA_RESTORE(cur.clip_planes);
        SGI_KONA_RESTORE(normalize);
        SGI_KONA_RESTORE(rescale_normal);
        memcpy(c->cur.clip_plane, saved->cur.clip_plane,
               sizeof(c->cur.clip_plane));
    }
    if (mask & 0x2000) {                        /* GL_ENABLE_BIT */
        SGI_KONA_RESTORE(cur.alpha_test);
        SGI_KONA_RESTORE(cur.blend);
        SGI_KONA_RESTORE(color_material);
        SGI_KONA_RESTORE(cur.cull_face);
        SGI_KONA_RESTORE(cur.depth_test);
        SGI_KONA_RESTORE(cur.dither);
        SGI_KONA_RESTORE(cur.fog);
        SGI_KONA_RESTORE(lighting);
        SGI_KONA_RESTORE(lights_enabled);
        SGI_KONA_RESTORE(cur.index_logic_op);
        SGI_KONA_RESTORE(cur.color_logic_op);
        SGI_KONA_RESTORE(normalize);
        SGI_KONA_RESTORE(rescale_normal);
        SGI_KONA_RESTORE(cur.polygon_offset);
        SGI_KONA_RESTORE(cur.scissor_test);
        SGI_KONA_RESTORE(cur.stencil_test);
        SGI_KONA_RESTORE(cur.texture_1d);
        SGI_KONA_RESTORE(cur.texture_2d);
        SGI_KONA_RESTORE(cur.texture_3d);
        SGI_KONA_RESTORE(cur.clip_planes);
        enables |= enables_point | enables_line | enables_polygon;
    }
    if (mask & 0x4000) {                        /* GL_COLOR_BUFFER_BIT */
        SGI_KONA_RESTORE(cur.alpha_test);
        SGI_KONA_RESTORE(cur.alpha_func);
        SGI_KONA_RESTORE(cur.alpha_ref);
        SGI_KONA_RESTORE(cur.blend);
        SGI_KONA_RESTORE(cur.blend_src);
        SGI_KONA_RESTORE(cur.blend_dst);
        SGI_KONA_RESTORE(cur.blend_equation);
        SGI_KONA_RESTORE(cur.dither);
        SGI_KONA_RESTORE(cur.draw_buffers);
        SGI_KONA_RESTORE(cur.index_logic_op);
        SGI_KONA_RESTORE(cur.color_logic_op);
        SGI_KONA_RESTORE(cur.logic_op_mode);
        SGI_KONA_RESTORE(cur.colour_mask);
        SGI_KONA_RESTORE(cur.index_mask);
        SGI_KONA_RESTORE(clear_index);
        memcpy(c->cur.blend_color, saved->cur.blend_color,
               sizeof(c->cur.blend_color));
        memcpy(c->clear_color, saved->clear_color, sizeof(c->clear_color));
    }
    if (mask & 0x40000) {                       /* GL_TEXTURE_BIT */
        SGI_KONA_RESTORE(cur.texture_2d);
        SGI_KONA_RESTORE(cur.texture_env_mode);
        memcpy(c->cur.texture_env_color, saved->cur.texture_env_color,
               sizeof(c->cur.texture_env_color));
    }
    if (mask & 0x8000) {                        /* GL_HINT_BIT */
        memcpy(c->hints, saved->hints, sizeof(c->hints));
    }
    if (mask & 0x80000) {                       /* GL_SCISSOR_BIT */
        SGI_KONA_RESTORE(cur.scissor_test);
        memcpy(c->cur.scissor, saved->cur.scissor, sizeof(c->cur.scissor));
    }
    c->raster_enables = (c->raster_enables & ~enables) |
                        (saved->raster_enables & enables);
}

#undef SGI_KONA_RESTORE

/*
 * 0xc8 0, glPopAttrib: the groups the last push named come back from the
 * pipe's copy. An empty stack is GL_STACK_UNDERFLOW, logged.
 */
static void sgi_kona_gl_pop_attrib(SGIKonaState *s)
{
    int i = s->context_current;
    uint32_t k;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    if (!s->state[i]->attrib_depth || s->state[i]->gl.in_begin) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: glPopAttrib with %s;"
                      " ignored\n", s->state[i]->gl.in_begin ?
                      "a primitive open" :
                      "an empty attribute stack");
        return;
    }
    k = --s->state[i]->attrib_depth;
    sgi_kona_gl_restore(&s->state[i]->gl, &s->state[i]->attrib[k],
                         s->state[i]->attrib_mask[k]);
    if (s->state[i]->attrib_mask[k] & 0x800) {
        memcpy(s->state[i]->pipe.returns.depth_range,
               s->state[i]->return_attrib[k].depth_range,
               sizeof(s->state[i]->pipe.returns.depth_range));
    }
    if (s->state[i]->attrib_mask[k] & 0x100) {
        s->state[i]->pipe.returns.clear_depth =
            s->state[i]->return_attrib[k].clear_depth;
    }
    if (s->state[i]->attrib_mask[k] & 0x4000) {
        s->state[i]->pipe.returns.draw_buffer =
            s->state[i]->return_attrib[k].draw_buffer;
    }
    /* The convolution enables are both enable and pixel-mode state. */
    if (s->state[i]->attrib_mask[k] & (0x2000 | 0x20)) {
        s->state[i]->imaging.enabled =
            s->state[i]->imaging_enabled_attrib[k] & 7;
        s->state[i]->imaging.stages =
            s->state[i]->imaging_enabled_attrib[k] >> 3;
    }
    if (s->state[i]->attrib_mask[k] & 0x20) {
        memcpy(s->state[i]->pipe.pixel_zoom,
               s->state[i]->zoom_attrib[k].factors,
               sizeof(s->state[i]->pipe.pixel_zoom));
        s->state[i]->pipe.pixel_zoom_unsupported =
            s->state[i]->zoom_attrib[k].unsupported;
        s->state[i]->pipe.pixel_transfer = s->state[i]->transfer_attrib[k];
        s->state[i]->pipe.returns.read_buffer =
            s->state[i]->return_attrib[k].read_buffer;
        s->state[i]->pipe.returns.read_buffer_unsupported =
            s->state[i]->return_attrib[k].read_buffer_unsupported;
    }
    trace_sgi_kona_gl_command(SGI_KONA_CMD_GL_POP_ATTRIB, i);
}

/*
 * 0x119's words, twelve for each light: inferred to be each light's
 * ambient, diffuse and specular colours, the words the push reply gave
 * the library, which it sends back before 0xc8.
 */
static void sgi_kona_gl_pop_lights(SGIKonaState *s)
{
    int i = s->context_current;
    uint32_t n = MIN(s->gl_data_have, SGI_KONA_GL_DATA_MAX);
    SGIGLSink sink;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    sgi_kona_gl_sink(s, &sink);
    for (uint32_t l = 0; l < n / SGI_KONA_GL_POP_LIGHT_WORDS; l++) {
        static const uint32_t pnames[3] = {
            SGI_GL_AMBIENT, SGI_GL_DIFFUSE, SGI_GL_SPECULAR,
        };

        for (int k = 0; k < 3; k++) {
            uint32_t a[6] = { l, pnames[k] };

            memcpy(&a[2], &s->gl_data[l * SGI_KONA_GL_POP_LIGHT_WORDS +
                                      4 * k], 4 * sizeof(uint32_t));
            sgi_gl_command(&s->state[i]->gl, &sink,
                           &(SGIGLCmd) { SGI_GL_OP_LIGHT, 6, a });
        }
    }
}

static void sgi_kona_x_box_data_drop(SGIKonaState *s, uint32_t next);
static void sgi_kona_x_clip_run_drop(SGIKonaState *s, uint32_t next);

/* The GL command table's entry for a 16-bit selector, or NULL. */
static const SGICommandPortSelector *sgi_kona_gl_token(SGIKonaState *s,
                                                       uint32_t selector)
{
    return sgi_command_port_selector(&s->gl_port, selector);
}

/*
 * The GL commands a whole word opens before the first context activation:
 * the make-current and drawing commands the model framed before it framed
 * the rest of the GL grammar. No client sends them then; the page carries
 * the PROM's textport and the kernel, and taking any other GL selector
 * there would change how their stray words frame.
 */
static bool sgi_kona_gl_token_early(uint32_t selector)
{
    switch (selector) {
    case SGI_KONA_CMD_GL_ACTIVE_TEXTURE:
    case SGI_KONA_CMD_GL_TEX_SCALE_BIAS:
    case SGI_KONA_CMD_GL_TEX_TABLE:
    case SGI_KONA_CMD_GL_LINE_DISTANCE:
    case SGI_KONA_CMD_GL_LAZY_MODE:
    case SGI_KONA_CMD_GL_VIEWPORT:
    case SGI_KONA_CMD_GL_SCISSOR:
    case SGI_KONA_CMD_GL_SET_ERROR:
    case SGI_KONA_CMD_GL_DISABLE:
    case SGI_KONA_CMD_GL_CLEAR_COLOR:
    case SGI_KONA_CMD_GL_CLEAR:
    case SGI_KONA_CMD_GL_SHADE_MODEL:
    case SGI_KONA_CMD_GL_COLOR3:
    case SGI_KONA_CMD_GL_BEGIN_FIRST ... SGI_KONA_CMD_GL_BEGIN_LAST:
    case SGI_KONA_CMD_GL_VERTEX2:
    case SGI_KONA_CMD_GL_END:
    case SGI_KONA_CMD_GL_FLUSH:
    case SGI_KONA_CMD_GL_FINISH:
    case SGI_KONA_CMD_GL_GET_ERROR:
        return true;
    default:
        return false;
    }
}

/*
 * Whether a word between packets opens a GL command, and its selector.
 *
 * Once the page speaks the GL grammar, the selector is the word's low 16
 * bits: the byte and short attribute forms put data in the top half
 * (glColor3ub is r << 24 | g << 16 | 0x66), and no form uses bits 0 to 15
 * for data (established for the forms read). That no selector is wider
 * than 16 bits is inferred; the pipe names only 512 commands. Before the
 * first context activation only the whole words of
 * sgi_kona_gl_token_early() open one.
 */
static bool sgi_kona_gl_token_word(SGIKonaState *s, uint32_t word,
                                   uint32_t *selector)
{
    uint32_t low = word & 0xffff;

    if (!s->gl_grammar &&
        (word != low || !sgi_kona_gl_token_early(low))) {
        return false;
    }
    *selector = low;
    return sgi_kona_gl_token(s, low) != NULL;
}

/* Open a GL command at a word between packets: see sgi_kona_gl_packet(). */
static void sgi_kona_gl_open(SGIKonaState *s, uint32_t word,
                             uint32_t selector)
{
    sgi_kona_x_box_data_drop(s, word);
    sgi_kona_x_clip_run_drop(s, word);
    s->gl_token_high = word & 0xffff0000;
    if (s->gl_token_high) {
        trace_sgi_kona_gl_packed(selector, s->gl_token_high);
    }
    sgi_command_port_write(&s->gl_port, selector, 4);
}

/*
 * A run of data words that belongs to the GL command before it, consumed
 * here so the words cannot open commands of their own: the polygon
 * stipple's rows, the pop-lights words and a texel load's rows. The first
 * SGI_KONA_GL_DATA_MAX words are kept for the command to use when the run
 * ends.
 */
static void sgi_kona_texture_upload_clear(SGIKonaState *s)
{
    g_free(s->texture_upload.pixels);
    s->texture_upload = (SGIKonaTextureUpload) { 0 };
    s->texture_dma_discard = false;
    s->texture_upload_context = SGI_KONA_CONTEXT_NONE;
}

/*
 * A texture level is loaded by 0x174 and its rows, and published by 0x176
 * afterwards; the client library sends the two from the same context, but
 * a context switch can fall between them, and the other context's own
 * commands would then replace or clear the load. Each context's load is
 * therefore its own (inferred: what the pipe saves at a switch is
 * unknown) once its rows are in, or while its rows are to come
 * by DMA (atlantis's DMA load has been seen switched out before its DMA
 * packet): when context `slot` becomes current, such a load another
 * context made is set aside with that context, and one `slot` set aside
 * comes back. A load still taking FIFO rows travels with the open command
 * instead (sgi_kona_input_save()), and anything else is left as it was.
 */
static bool sgi_kona_texture_upload_parkable(const SGIKonaTextureUpload *u)
{
    return !u->draw_pixels &&
           (u->complete || (u->count && u->header[6] == 1));
}

static void sgi_kona_texture_upload_switch(SGIKonaState *s, int slot)
{
    int owner = s->texture_upload_context;
    SGIKonaGLState *state;

    if (owner != SGI_KONA_CONTEXT_NONE && owner != slot &&
        sgi_kona_texture_upload_parkable(&s->texture_upload) &&
        !s->texture_dma_discard && !s->gl_data_remaining && s->state[owner]) {
        state = s->state[owner];
        g_free(state->parked_upload.pixels);
        state->parked_upload = s->texture_upload;
        s->texture_upload = (SGIKonaTextureUpload) { 0 };
        s->texture_upload_context = SGI_KONA_CONTEXT_NONE;
    }
    state = slot != SGI_KONA_CONTEXT_NONE ? s->state[slot] : NULL;
    if (state && sgi_kona_texture_upload_parkable(&state->parked_upload) &&
        s->texture_upload_context == SGI_KONA_CONTEXT_NONE &&
        !s->texture_upload.count && !s->texture_upload.complete) {
        g_free(s->texture_upload.pixels);
        s->texture_upload = state->parked_upload;
        state->parked_upload = (SGIKonaTextureUpload) { 0 };
        s->texture_upload_context = slot;
    }
}

static void sgi_kona_texture_regs_store(const SGIKonaState *s,
                                        SGIKonaTextureRegs *r)
{
    memcpy(r->bind, s->texture_bind, sizeof(r->bind));
    memcpy(r->record, s->texture_record, sizeof(r->record));
    r->record_token = s->texture_record_token;
    memcpy(r->transform, s->texture_transform, sizeof(r->transform));
    r->bind_valid = s->texture_bind_valid;
    r->record_valid = s->texture_record_valid;
}

static void sgi_kona_texture_regs_load(SGIKonaState *s,
                                       const SGIKonaTextureRegs *r)
{
    memcpy(s->texture_bind, r->bind, sizeof(s->texture_bind));
    memcpy(s->texture_record, r->record, sizeof(s->texture_record));
    s->texture_record_token = r->record_token;
    memcpy(s->texture_transform, r->transform, sizeof(s->texture_transform));
    s->texture_bind_valid = r->bind_valid;
    s->texture_record_valid = r->record_valid;
}

/*
 * The bind words are pipe state the client library sends in pieces: a
 * bind, then its record, with 0x178 naming a texture between binds. A
 * context switch can fall between the pieces (mirrors' 0x106 and its 0x179
 * have been seen split by atlantis's own bind and 0x178), and the record
 * must then complete this context's bind, not the other program's. So the
 * words are each context's own (inferred: what the pipe saves at a switch
 * is unknown), kept with it while another is current, as
 * the textures they name are (sgi_kona_texture_set_owner()), along with
 * whether a bind still waits for its record and the association that
 * record would commit. A context that has none starts from the reset
 * words, and one made current again keeps what it had.
 */
static void sgi_kona_texture_regs_switch(SGIKonaState *s, int slot)
{
    int owner = s->texture_regs_context;
    SGIKonaGLState *state;

    if (owner == slot) {
        return;
    }
    if (owner != SGI_KONA_CONTEXT_NONE && s->state[owner]) {
        SGIKonaTextureRegs *r = &s->state[owner]->texture_regs;

        sgi_kona_texture_regs_store(s, r);
        r->bind_pending = s->texture_bind_context == owner &&
            sgi_kona_context_owner(s, owner, s->texture_bind_generation);
        r->associate_pending = s->texture_associate_pending &&
            s->texture_associate_context == owner &&
            sgi_kona_context_owner(s, owner,
                                   s->texture_associate_generation);
        r->associate_slot = s->texture_associate_slot;
        s->state[owner]->texture_regs_saved = true;
    }
    s->texture_bind_context = SGI_KONA_CONTEXT_NONE;
    s->texture_bind_generation = 0;
    s->texture_associate_pending = false;
    state = slot != SGI_KONA_CONTEXT_NONE ? s->state[slot] : NULL;
    if (state && state->texture_regs_saved) {
        const SGIKonaTextureRegs *r = &state->texture_regs;

        sgi_kona_texture_regs_load(s, r);
        if (r->bind_pending) {
            s->texture_bind_context = slot;
            s->texture_bind_generation = sgi_kona_context_generation(s, slot);
        }
        if (r->associate_pending) {
            s->texture_associate_pending = true;
            s->texture_associate_context = slot;
            s->texture_associate_generation =
                sgi_kona_context_generation(s, slot);
            s->texture_associate_slot = r->associate_slot;
        }
    } else {
        sgi_kona_texture_regs_load(s, &(SGIKonaTextureRegs) { 0 });
    }
    s->texture_regs_context = slot;
}

static uint32_t sgi_kona_texture_source_bytes(uint32_t format, uint32_t type)
{
    return format == 11 || format == 13 ?
           sgi_kona_gl_native_pixel_bytes(format, type) :
                          sgi_kona_gl_texture_pixel_bytes(format, type);
}

static bool sgi_kona_texture_bitmap(const uint32_t *h)
{
    return h[3] == 11 && h[4] == 7;
}

static bool sgi_kona_texture_source_valid(const uint32_t *h)
{
    return sgi_kona_texture_bitmap(h) ||
           sgi_kona_texture_source_bytes(h[3], h[4]);
}

static uint64_t sgi_kona_texture_row_bytes(const SGIKonaTextureUpload *u)
{
    return sgi_kona_texture_bitmap(u->header) ?
           ((uint64_t)u->header[1] + u->bitmap_skip + 7) / 8 :
           (uint64_t)u->header[1] *
           sgi_kona_texture_source_bytes(u->header[3], u->header[4]);
}

static uint32_t sgi_kona_texture_source_words(const SGIKonaTextureUpload *u)
{
    uint32_t bytes = sgi_kona_texture_source_bytes(u->header[3],
                                                      u->header[4]);

    return DIV_ROUND_UP(MAX(bytes, 4), 4);
}

static uint32_t sgi_kona_texture_buffer_words(const SGIKonaTextureUpload *u)
{
    return u->count * (u->complete ? 1 : sgi_kona_texture_source_words(u));
}

/*
 * Whether a texel load reads back a texture copy (see
 * sgi_kona_gl_read_texels()): its words are the model's own texels, which
 * the load keeps as they are, under the storage the copy was taken from.
 */
static bool sgi_kona_texture_copy(const uint32_t *h)
{
    return h[3] == SGI_KONA_TEXTURE_COPY_FORMAT &&
           h[4] == SGI_KONA_TEXTURE_COPY_TYPE &&
           h[9] == SGI_KONA_TEXTURE_COPY_STORAGE && (h[17] & 1);
}

static uint32_t sgi_kona_texture_storage_base(const uint32_t *h)
{
    if (sgi_kona_texture_copy(h)) {
        return SGI_GL_RGBA;
    }
    if (h[3] == 13) {
        return h[9] == 0xd && !(h[17] & 1) ? SGI_GL_DEPTH_COMPONENT : 0;
    }
    if (h[9] == 0xe && !(h[17] & 1)) {
        return SGI_GL_ALPHA;
    }
    if (h[9] == 0xd && !(h[17] & 1)) {
        return SGI_GL_LUMINANCE;
    }
    if (h[9] == 8 && !(h[17] & 1)) {
        return SGI_GL_LUMINANCE_ALPHA;
    }
    /* Preserve source alpha until the binding selects RGB or RGBA. */
    if (h[9] == 0x30 && (h[17] & 1) &&
        sgi_kona_texture_source_valid(h)) {
        return SGI_GL_RGBA;
    }
    /*
     * The colour formats below eight bits a component store as 0x10 or
     * 0x14 in layout family zero; the binding selects RGB or RGBA the same
     * way. The model keeps its eight-bit components: the format names the
     * requested precision, not the pipe's.
     */
    if ((h[9] == SGI_KONA_GL_STORAGE_RGB4 ||
         h[9] == SGI_KONA_GL_STORAGE_RGB5) && !(h[17] & 1) &&
        sgi_kona_texture_source_valid(h)) {
        return SGI_GL_RGBA;
    }
    return 0;
}

static uint32_t sgi_kona_texture_source_phase(const SGIKonaTextureUpload *u)
{
    return sgi_kona_texture_bitmap(u->header) ? u->bitmap_phase :
           u->pio_layout ? u->pio_phase : u->header[5];
}

/* Four rows repeat every possible word-alignment phase. */
static uint32_t sgi_kona_texture_row_words(const SGIKonaTextureUpload *u,
                                           uint32_t row)
{
    uint32_t pad = (sgi_kona_texture_source_phase(u) +
                    (uint64_t)row * u->source_stride) & 3;

    return DIV_ROUND_UP(pad + sgi_kona_texture_row_bytes(u), 4);
}

static uint64_t sgi_kona_texture_words(const SGIKonaTextureUpload *u)
{
    uint64_t cycle = 0, total;

    if (u->pio_layout && !u->header[7]) {
        return DIV_ROUND_UP(u->pio_phase +
                           (uint64_t)(u->header[2] - 1) * u->source_stride +
                           sgi_kona_texture_row_bytes(u), 4);
    }
    if (u->draw_pixels && !u->header[7]) {
        return DIV_ROUND_UP(u->header[5] + (uint64_t)u->count * 4, 4);
    }
    for (unsigned k = 0; k < 4; k++) {
        cycle += sgi_kona_texture_row_words(u, k);
    }
    total = cycle * (u->header[2] / 4);
    for (unsigned k = 0; k < u->header[2] % 4; k++) {
        total += sgi_kona_texture_row_words(u, k);
    }
    return total;
}

static void sgi_kona_texture_source_position(const SGIKonaTextureUpload *u,
                                             uint32_t have, uint32_t *row,
                                             uint32_t *word)
{
    uint32_t cycle = 0, words;

    for (unsigned k = 0; k < 4; k++) {
        cycle += sgi_kona_texture_row_words(u, k);
    }
    *row = (have / cycle) * 4;
    have %= cycle;
    while (have >= (words = sgi_kona_texture_row_words(u, *row))) {
        have -= words;
        (*row)++;
    }
    *word = have;
}

/*
 * Stage n source bytes of one row, from byte offset byte: each pixel's
 * bytes go most significant first into its sgi_kona_texture_source_words()
 * words, a bitmap's bits one texel each. Bytes past the row's end and
 * rows past the image's are dropped.
 */
static void sgi_kona_texture_upload_bytes(SGIKonaTextureUpload *u,
                                          uint32_t row, uint64_t byte,
                                          const uint8_t *value, size_t n)
{
    uint32_t width = u->header[1];
    uint64_t row_bytes = sgi_kona_texture_row_bytes(u);
    uint32_t bytes, words;
    uint64_t pixel;
    uint32_t within;

    if (row >= u->header[2] || byte >= row_bytes) {
        return;
    }
    n = MIN(n, row_bytes - byte);
    if (sgi_kona_texture_bitmap(u->header)) {
        for (; n; n--, byte++, value++) {
            for (unsigned bit = 0; bit < 8; bit++) {
                uint64_t x = byte * 8 + bit;

                if (x >= u->bitmap_skip && x - u->bitmap_skip < width) {
                    u->pixels[(uint64_t)row * width + x - u->bitmap_skip] =
                        (*value >> (u->bitmap_lsb ? bit : 7 - bit)) & 1;
                }
            }
        }
        return;
    }
    bytes = sgi_kona_texture_source_bytes(u->header[3], u->header[4]);
    words = sgi_kona_texture_source_words(u);
    if (bytes == words * 4) {
        /* Whole words a pixel: the row is one run of big-endian words. */
        sgi_kona_gl_words_or_bytes(u->pixels,
                                   (uint64_t)row * width * bytes + byte,
                                   value, n);
        return;
    }
    pixel = (uint64_t)row * width + byte / bytes;
    within = byte % bytes;
    for (; n; n--, value++) {
        u->pixels[pixel * words + within / 4] |=
            (uint32_t)*value << (24 - (within % 4) * 8);
        if (++within == bytes) {
            within = 0;
            pixel++;
        }
    }
}

static void sgi_kona_texture_upload_byte(SGIKonaTextureUpload *u,
                                         uint32_t row, uint32_t byte,
                                         uint8_t value)
{
    sgi_kona_texture_upload_bytes(u, row, byte, &value, 1);
}

/* Stage source components; defaults and replication wait for finish. */
static void sgi_kona_texture_upload_word(SGIKonaState *s, uint32_t value)
{
    SGIKonaTextureUpload *u = &s->texture_upload;
    uint32_t row, word, pad, offset;
    uint32_t bytes = sgi_kona_texture_source_bytes(u->header[3],
                                                      u->header[4]);

    if (!u->count) {
        return;
    }
    if (u->pio_layout && !u->header[7]) {
        uint64_t source_at = (uint64_t)s->gl_data_have * 4;

        for (unsigned k = 0; k < 4; k++, source_at++) {
            if (source_at >= u->pio_phase) {
                uint64_t byte = source_at - u->pio_phase;
                uint64_t source_row = byte / u->source_stride;
                uint32_t col = byte % u->source_stride;

                if (source_row < u->header[2]) {
                    sgi_kona_texture_upload_byte(u, source_row, col,
                                                  value >> (24 - k * 8));
                }
            }
        }
        return;
    }
    if (u->draw_pixels && !u->header[7]) {
        uint64_t source_at = (uint64_t)s->gl_data_have * 4;

        for (unsigned k = 0; k < 4; k++, source_at++) {
            if (source_at >= u->header[5] &&
                source_at - u->header[5] < (uint64_t)u->count * bytes) {
                uint32_t at = source_at - u->header[5];

                u->pixels[at / 4] |= ((value >> (24 - k * 8)) & 0xff) <<
                                     (24 - (at % 4) * 8);
            }
        }
        return;
    }
    sgi_kona_texture_source_position(u, s->gl_data_have, &row, &word);
    pad = (sgi_kona_texture_source_phase(u) +
                    (uint64_t)row * u->source_stride) & 3;
    offset = word * 4;
    for (unsigned k = 0; k < 4; k++, offset++) {
        if (offset >= pad) {
            sgi_kona_texture_upload_byte(u, row, offset - pad,
                                          value >> (24 - k * 8));
        }
    }
}

/*
 * The colour tables and the stages between them for a texture image, with
 * the pixel transfer and colour matrix the load captured: the colour
 * table, post-convolution scale and bias, the post-convolution table, the
 * colour matrix stage and the post-colour-matrix table (SGI_color_table
 * applies its tables to texture images as to DrawPixels). st is the
 * loading context, or NULL for none.
 *
 * Approximated: the tables are the context's as the image completes, not
 * as its header arrived; a table the context changes between a texture
 * header and its DMA image would apply to it. The client library sends
 * nothing between the two for one context.
 */
static bool sgi_kona_gl_texture_stages(SGIKonaGLState *st,
                                       const SGIKonaGLPixelTransfer *t,
                                       const uint32_t *matrix, bool post_conv,
                                       double values[4])
{
    if (st) {
        sgi_kona_gl_table_stage(st, 0, values);
    }
    if (!sgi_kona_gl_post_conv_default(t) &&
        !sgi_kona_gl_post_conv(t, values)) {
        return false;
    }
    if (st) {
        sgi_kona_gl_table_stage(st, 1, values);
    }
    if (post_conv && sgi_kona_gl_matrix_stage(matrix, t) &&
        !sgi_kona_gl_post_matrix(matrix, t, values)) {
        return false;
    }
    if (st) {
        sgi_kona_gl_table_stage(st, 2, values);
        sgi_kona_gl_histogram_stage(st, values);
    }
    return true;
}

static bool sgi_kona_texture_upload_normalize(SGIKonaState *s,
                                              SGIKonaTextureUpload *u)
{
    int i = s->texture_upload_context;
    SGIKonaGLState *st = i >= 0 && i < SGI_KONA_CONTEXTS &&
                               s->state[i] && u->transfer_captured ?
                               s->state[i] : NULL;
    uint32_t base = sgi_kona_texture_storage_base(u->header);
    uint32_t words = sgi_kona_texture_source_words(u);
    const SGIKonaGLPixelTransfer identity = {
        .scale = { 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000 },
        .depth_scale = 0x3f800000,
        .post_conv_scale = { 0x3f800000, 0x3f800000,
                             0x3f800000, 0x3f800000 },
        .post_matrix_scale = { 0x3f800000, 0x3f800000,
                               0x3f800000, 0x3f800000 },
    };
    static const uint32_t identity_matrix[16] = {
        0x3f800000, 0, 0, 0, 0, 0x3f800000, 0, 0,
        0, 0, 0x3f800000, 0, 0, 0, 0, 0x3f800000,
    };
    const SGIKonaGLPixelTransfer *t = u->transfer_captured ?
                                     &u->transfer : &identity;
    const uint32_t *matrix = u->transfer_captured ? u->colour_matrix :
                                                    identity_matrix;
    bool post_conv = !sgi_kona_gl_post_conv_default(t) ||
                     sgi_kona_gl_matrix_stage(matrix, t);
    bool staged = post_conv ||
                  (st && (sgi_kona_gl_tables_active(st) ||
                          sgi_kona_gl_histogram_active(st)));
    /*
     * Unsigned bytes through an identity transfer and no later stage come
     * back as the bytes they went in as (sgi_kona_gl_pixel_rgba8()), so
     * skip the floating-point round trip.
     */
    bool rgba8 = base != SGI_GL_DEPTH_COMPONENT && u->header[3] <= 10 &&
                 u->header[4] == 1 && !staged &&
                 sgi_kona_gl_rgba_identity(t);

    if (sgi_kona_texture_copy(u->header)) {
        /* The copy's words are already the model's texels. */
        return true;
    }
    for (uint32_t at = 0; at < u->count; at++) {
        double values[4];
        uint8_t bytes[4];
        uint32_t rgba;
        bool converted;

        if (rgba8) {
            /* One source word a texel: words is 1 for unsigned bytes. */
            if (!sgi_kona_gl_pixel_rgba8(u->header[3], u->pixels[at],
                                         &rgba)) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-kona: native texture component is "
                              "unsupported; upload refused\n");
                return false;
            }
            goto store;
        }
        if (base == SGI_GL_DEPTH_COMPONENT) {
            if (!sgi_kona_gl_texture_pixel_depth(u->header[4],
                    u->pixels + at * words, u->swap_bytes,
                    t->depth_scale, t->depth_bias, &u->pixels[at])) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-kona: unsupported depth texture transfer; "
                              "upload refused\n");
                return false;
            }
            continue;
        }
        if (u->header[3] == 11) {
            double index;

            if (sgi_kona_texture_bitmap(u->header)) {
                index = u->pixels[at];
                converted = sgi_kona_gl_index_rgba(t, index, values);
            } else {
                converted = sgi_kona_gl_pixel_index(u->header[4],
                    u->pixels + at * words, u->swap_bytes, &index) &&
                    sgi_kona_gl_index_rgba(t, index, values);
            }
        } else {
            converted = sgi_kona_gl_pixel_rgba_double(u->header[3],
                u->header[4], u->pixels + at * words, u->swap_bytes, values) &&
                sgi_kona_gl_rgba_transfer(t, values);
        }
        if (!converted ||
            (staged &&
             !sgi_kona_gl_texture_stages(st, t, matrix, post_conv,
                                         values)) ||
            !sgi_kona_gl_pixel_pack_double(5, 1, values, false,
                                            bytes, sizeof(bytes))) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-kona: native texture component is unsupported;"
                          " upload refused\n");
            return false;
        }
        rgba = ldl_be_p(bytes);
store:
        switch (base) {
        case SGI_GL_ALPHA:
            rgba &= 0xff;
            break;
        case SGI_GL_LUMINANCE:
            rgba = (rgba & 0xff000000) | 0xff;
            break;
        case SGI_GL_LUMINANCE_ALPHA:
            rgba &= 0xff0000ff;
            break;
        }
        u->pixels[at] = rgba;
    }
    return true;
}

static void sgi_kona_gl_image_clear(SGIKonaState *s)
{
    g_free(s->gl_image_upload.words);
    memset(&s->gl_image_upload, 0, sizeof(s->gl_image_upload));
}

static bool sgi_kona_gl_pixel_map_token(uint32_t token)
{
    return token == SGI_KONA_CMD_GL_PIXEL_MAP_F ||
           token == SGI_KONA_CMD_GL_PIXEL_MAP_UI ||
           token == SGI_KONA_CMD_GL_PIXEL_MAP_US;
}

static void sgi_kona_gl_data_open(SGIKonaState *s, uint32_t token,
                                  uint32_t words)
{
    if (token != SGI_KONA_CMD_GL_TEX_LOAD &&
        token != SGI_KONA_CMD_GL_DRAW_PIXELS &&
        !(sgi_kona_gl_pixel_map_token(token) &&
          s->texture_upload.count && !s->texture_upload.draw_pixels &&
          (s->texture_upload.complete || s->texture_upload.header[6]))) {
        sgi_kona_texture_upload_clear(s);
    }
    if (token != SGI_KONA_CMD_GL_BITMAP &&
        token != SGI_KONA_CMD_GL_DRAW_PIXELS) {
        sgi_kona_gl_image_clear(s);
    }
    if (token != SGI_KONA_CMD_GL_BITMAP_INLINE) {
        memset(&s->gl_bitmap_upload, 0, sizeof(s->gl_bitmap_upload));
    }
    if (token != SGI_KONA_CMD_GL_PIXEL_MAP_F &&
        token != SGI_KONA_CMD_GL_PIXEL_MAP_UI &&
        token != SGI_KONA_CMD_GL_PIXEL_MAP_US) {
        memset(&s->gl_map_upload, 0, sizeof(s->gl_map_upload));
    }
    s->gl_data_token = token;
    s->gl_data_remaining = words;
    s->gl_data_have = 0;
    trace_sgi_kona_gl_data(token, words);
}


/*
 * glDrawArrays' vertices (see SGI_KONA_CMD_GL_ARRAY_STATE). The state
 * word holds one field for each enabled array, zero for a disabled one
 * (state the client library keeps per context, which the
 * gl*PointerEXT calls and glEnable set): the
 * position's size and type in bits 0 to 7, the normal's type in 8 to 11,
 * the colour's type and size in 12 to 19, the texture coordinate's type
 * and size in 20 to 26, the edge flag in bit 27 and the colour index's
 * type in 28 to 31. A type code is 1 to 6 for GL_BYTE to
 * GL_UNSIGNED_INT, 7 for GL_FLOAT and 0xb for GL_DOUBLE. A vertex is each
 * enabled array's element in turn, normal, colour, texture coordinate,
 * then position, each element's bytes packed into whole words.
 */
typedef struct SGIKonaGLArrayLayout {
    unsigned enabled;           /* arrays sent, e */
    unsigned words;             /* words in one vertex */
    uint8_t size[4], code[4];   /* normal, colour, texcoord, position */
    const char *undrawn;        /* why the vertices are not drawn */
} SGIKonaGLArrayLayout;

enum {
    SGI_KONA_ARRAY_NORMAL,
    SGI_KONA_ARRAY_COLOUR,
    SGI_KONA_ARRAY_TEXCOORD,
    SGI_KONA_ARRAY_POSITION,
};

static unsigned sgi_kona_gl_array_type_bytes(unsigned code)
{
    static const uint8_t bytes[16] = {
        [1] = 1, [2] = 1, [3] = 2, [4] = 2, [5] = 4, [6] = 4, [7] = 4,
        [0xb] = 8,
    };

    return bytes[code & 15];
}

/* False when an enabled array's length is unknown: nothing can be framed. */
static bool sgi_kona_gl_array_layout(uint32_t state, SGIKonaGLArrayLayout *l)
{
    static const struct {
        unsigned code_shift, size_shift, size_mask, fixed_size, field;
    } arrays[] = {
        [SGI_KONA_ARRAY_NORMAL] = { 8, 0, 0, 3, 0x00000f00 },
        [SGI_KONA_ARRAY_COLOUR] = { 12, 16, 15, 0, 0x000ff000 },
        [SGI_KONA_ARRAY_TEXCOORD] = { 20, 24, 7, 0, 0x07f00000 },
        [SGI_KONA_ARRAY_POSITION] = { 0, 4, 15, 0, 0x000000ff },
    };

    memset(l, 0, sizeof(*l));
    for (unsigned k = 0; k < ARRAY_SIZE(arrays); k++) {
        unsigned code, size, bytes;

        if (!(state & arrays[k].field)) {
            continue;
        }
        code = (state >> arrays[k].code_shift) & 15;
        size = arrays[k].fixed_size ? arrays[k].fixed_size :
               (state >> arrays[k].size_shift) & arrays[k].size_mask;
        bytes = sgi_kona_gl_array_type_bytes(code);
        if (!bytes || !size) {
            return false;
        }
        l->size[k] = size;
        l->code[k] = code;
        l->enabled++;
        l->words += DIV_ROUND_UP(size * bytes, 4);
        if (code != 7 && !l->undrawn) {
            l->undrawn = "an array whose type is not GL_FLOAT";
        }
    }
    if (state & 0x08000000) {
        l->enabled++;
        l->words++;
        l->undrawn = "an edge-flag array, whose place in the vertex is unknown";
    }
    if (state & 0xf0000000) {
        unsigned bytes = sgi_kona_gl_array_type_bytes(state >> 28);

        if (!bytes) {
            return false;
        }
        l->enabled++;
        l->words += DIV_ROUND_UP(bytes, 4);
        l->undrawn = "a colour-index array, whose place in the vertex is"
                     " unknown";
    }
    if (!l->enabled) {
        return false;
    }
    if (!l->undrawn &&
        (l->size[SGI_KONA_ARRAY_POSITION] < 2 ||
         l->size[SGI_KONA_ARRAY_POSITION] > 4 ||
         (l->size[SGI_KONA_ARRAY_COLOUR] &&
          (l->size[SGI_KONA_ARRAY_COLOUR] < 3 ||
           l->size[SGI_KONA_ARRAY_COLOUR] > 4)) ||
         l->size[SGI_KONA_ARRAY_TEXCOORD] > 4)) {
        l->undrawn = "no position array, or a size OpenGL does not allow";
    }
    if (l->words > SGI_KONA_GL_DATA_MAX) {
        l->undrawn = "more words in a vertex than the model holds";
    }
    return true;
}

/* A vertex's words: the immediate-mode commands they stand for. */
static void sgi_kona_gl_array_vertex(SGIKonaState *s, int i,
                                     const SGIKonaGLArrayLayout *l,
                                     const uint32_t *w)
{
    static const uint32_t texcoord[] = {
        0, SGI_KONA_CMD_GL_TEXCOORD1F, SGI_KONA_CMD_GL_TEXCOORD2F,
        SGI_KONA_CMD_GL_TEXCOORD3F, SGI_KONA_CMD_GL_TEXCOORD4F,
    };
    uint32_t high = s->gl_token_high, colour[4];
    unsigned size;

    s->gl_token_high = 0;
    if (l->size[SGI_KONA_ARRAY_NORMAL]) {
        sgi_kona_gl_transform(s, i, SGI_KONA_CMD_GL_NORMAL3F, w);
        w += 3;
    }
    size = l->size[SGI_KONA_ARRAY_COLOUR];
    if (size) {
        /* glColor3f leaves alpha at 1 (OpenGL 1.2.1, 2.7). */
        colour[3] = 0x3f800000;
        memcpy(colour, w, size * sizeof(*w));
        sgi_kona_gl_transform(s, i, SGI_KONA_CMD_GL_COLOR4F, colour);
        w += size;
    }
    size = l->size[SGI_KONA_ARRAY_TEXCOORD];
    if (size) {
        sgi_kona_gl_transform(s, i, texcoord[size], w);
        w += size;
    }
    switch (l->size[SGI_KONA_ARRAY_POSITION]) {
    case 2:
        sgi_kona_gl_command(s, SGI_KONA_CMD_GL_VERTEX2, w, 2);
        break;
    case 3:
        sgi_kona_gl_transform(s, i, SGI_KONA_CMD_GL_VERTEX3F, w);
        break;
    default:
        sgi_kona_gl_transform(s, i, SGI_KONA_CMD_GL_VERTEX4F, w);
        break;
    }
    s->gl_token_high = high;
}

/*
 * A short run (selector holds its count) or a bulk run (count given).
 * The model frames the vertices only when it knows their length: inside
 * 0x181 and 0x1a3, after a state word, with every enabled array's type
 * known. Otherwise it logs, and the words fall to the textport as before.
 */
static void sgi_kona_gl_array_run(SGIKonaState *s, int i, uint32_t selector,
                                  const uint32_t *args)
{
    SGIKonaGLArrayLayout l;
    SGIKonaGLPipe *p;
    uint32_t count;
    uint64_t words;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    p = &s->state[i]->pipe;
    if (!p->array_open || !p->array_state_valid ||
        !sgi_kona_gl_array_layout(p->array_state, &l)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: glDrawArrays run 0x%"
                      PRIx32 " outside 0x181, with no state word or with an"
                      " array of unknown length (state 0x%" PRIx32 ");"
                      " vertices not framed\n", selector, p->array_state);
        return;
    }
    if (selector == SGI_KONA_CMD_GL_ARRAY_BULK) {
        count = args[0];
        if (args[1] != 0xdeadbeef || args[2] != 0xdeadbeef) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: glDrawArrays bulk run"
                          " padding 0x%" PRIx32 " 0x%" PRIx32 ", not"
                          " 0xdeadbeef\n", args[1], args[2]);
        }
    } else {
        unsigned e = l.enabled;
        uint32_t base = e == 1 || e == 2 || e == 4 ?
                        SGI_KONA_CMD_GL_ARRAY_END :
                        SGI_KONA_CMD_GL_ARRAY_END - 2;

        count = selector < base && !((base - selector) % e) ?
                (base - selector) / e : 0;
        if (!count || count >= 32 / e) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: glDrawArrays short run"
                          " 0x%" PRIx32 " does not fit %u arrays; vertices"
                          " not framed\n", selector, e);
            return;
        }
    }
    words = (uint64_t)count * l.words;
    if (!words || words > UINT32_MAX) {
        return;
    }
    if (l.undrawn) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glDrawArrays state 0x%" PRIx32
                      " has %s; %" PRIu32 " vertices framed, not drawn\n",
                      p->array_state, l.undrawn, count);
    }
    sgi_kona_gl_data_open(s, SGI_KONA_CMD_GL_ARRAY_BULK, words);
}

/* One word of a run's vertices; a vertex is drawn when it is whole. */
static void sgi_kona_gl_array_word(SGIKonaState *s, uint32_t word)
{
    int i = s->context_current;
    SGIKonaGLArrayLayout l;
    unsigned at;

    if (i == SGI_KONA_CONTEXT_NONE ||
        !sgi_kona_gl_array_layout(s->state[i]->pipe.array_state, &l) ||
        l.undrawn) {
        return;
    }
    at = s->gl_data_have % l.words;
    s->gl_data[at] = word;
    if (at == l.words - 1) {
        sgi_kona_gl_array_vertex(s, i, &l, s->gl_data);
    }
}

static void sgi_kona_gl_data_done(SGIKonaState *s);

static void sgi_kona_gl_data_word(SGIKonaState *s, uint32_t word)
{
    if (s->gl_data_token == SGI_KONA_CMD_GL_ARRAY_BULK) {
        sgi_kona_gl_array_word(s, word);
    } else if (s->gl_data_have < SGI_KONA_GL_DATA_MAX) {
        s->gl_data[s->gl_data_have] = word;
    }
    if (s->gl_data_token == SGI_KONA_CMD_GL_TEX_LOAD ||
        s->gl_data_token == SGI_KONA_CMD_GL_DRAW_PIXELS) {
        sgi_kona_texture_upload_word(s, word);
    }
    if (s->gl_data_token == SGI_KONA_CMD_GL_BITMAP_INLINE &&
        !s->gl_bitmap_upload.refused) {
        SGIKonaGLBitmapUpload *u = &s->gl_bitmap_upload;

        for (unsigned k = 0; k < 4; k++) {
            uint32_t at = s->gl_data_have * 4 + k;

            if (at < u->count) {
                u->bytes[at] = word >> (24 - k * 8);
            }
        }
    }
    if ((s->gl_data_token == SGI_KONA_CMD_GL_BITMAP ||
         (s->gl_data_token == SGI_KONA_CMD_GL_DRAW_PIXELS &&
          s->gl_image_upload.draw_pixels)) &&
        s->gl_image_upload.count &&
        !s->gl_image_upload.refused) {
        s->gl_image_upload.words[s->gl_data_have] = word;
    }
    if (s->gl_map_upload.header[1] && !s->gl_map_upload.refused &&
        s->gl_data_have < SGI_KONA_GL_PIXEL_MAP_MAX) {
        s->gl_map_upload.values[s->gl_data_have] = word;
    }
    if (s->gl_data_token == SGI_KONA_CMD_GL_COLOUR_TABLE) {
        sgi_kona_gl_colour_table_word(s, word);
    }
    s->gl_data_have++;
    if (--s->gl_data_remaining == 0) {
        sgi_kona_gl_data_done(s);
    }
}

/*
 * The bytes one pixel of a GL format and type takes in client memory
 * (OpenGL 1.2.1, tables 3.5 and 3.8), or 0 for a pair the model does not
 * know.
 */
static uint32_t sgi_kona_gl_pixel_bytes(uint32_t format, uint32_t type)
{
    uint32_t components, size;

    switch (format) {
    case 0x1900:                /* GL_COLOR_INDEX */
    case 0x1901:                /* GL_STENCIL_INDEX */
    case 0x1902:                /* GL_DEPTH_COMPONENT */
    case 0x1903:                /* GL_RED */
    case 0x1904:                /* GL_GREEN */
    case 0x1905:                /* GL_BLUE */
    case SGI_GL_ALPHA:
    case SGI_GL_LUMINANCE:
        components = 1;
        break;
    case SGI_GL_LUMINANCE_ALPHA:
        components = 2;
        break;
    case SGI_GL_RGB:
        components = 3;
        break;
    case SGI_GL_RGBA:
    case 0x8000:                /* GL_ABGR_EXT */
        components = 4;
        break;
    default:
        return 0;
    }
    switch (type) {
    case 0x1400:                /* GL_BYTE */
    case 0x1401:                /* GL_UNSIGNED_BYTE */
        size = 1;
        break;
    case 0x1402:                /* GL_SHORT */
    case 0x1403:                /* GL_UNSIGNED_SHORT */
        size = 2;
        break;
    case 0x1404:                /* GL_INT */
    case 0x1405:                /* GL_UNSIGNED_INT */
    case 0x1406:                /* GL_FLOAT */
        size = 4;
        break;
    default:
        return 0;
    }
    return components * size;
}

/*
 * Legacy public-enum headers keep their reviewed row framing: words 1/2
 * give dimensions, 3/4 format/type, 5 the source address low bits, and
 * nonzero 6 DMA ingress. Each row starts at those low bits only with a
 * whole-word unpack stride. SGI_KONA_GL_IMAGE_MAX bounds them.
 */

/* Destination layout is shared by ingress and migration validation. */
static bool sgi_kona_texture_native_header(const uint32_t *h)
{
    bool volume = h[0] == SGI_GL_TEXTURE_3D;
    bool one_dimensional = h[0] == SGI_GL_TEXTURE_1D;

    return (volume || one_dimensional || h[0] == SGI_GL_TEXTURE_2D) &&
           (!one_dimensional || (h[2] == 1 && !h[12] && !h[14] && !h[15])) &&
           h[6] <= 1 &&
           h[7] <= 1 && !h[8] &&
           sgi_kona_texture_storage_base(h) && h[10] != UINT32_MAX &&
           (volume ? h[15] < SGI_KONA_GL_IMAGE_MAX : !h[15]) &&
           h[16] <= (volume ? 0xfff : 0xff) && (h[16] & 15) <= 13 &&
           ((h[16] >> 4) & 15) <= 13 && (h[16] >> 8) <= 13 &&
           sgi_kona_gl_texture_native_layout(h) &&
           (uint64_t)h[11] + h[1] - 1 == h[13] &&
           (uint64_t)h[12] + h[2] - 1 == h[14];
}

/* Rebuild the active sampler only from complete native records. */
static void sgi_kona_gl_texture_bind(SGIKonaState *s)
{
    SGIKonaTextureParams params;
    uint32_t handle = s->texture_bind[0], width, height, depth, base, internal;
    uint32_t index = (s->texture_bind[1] >> 12) & 0xfff, format_storage;
    unsigned target = s->texture_bind[1] >> 28;
    bool volume = target == 3, one_dimensional = target == 1;
    /* The library sends the short volume record only in layout mode one. */
    bool single = volume && s->texture_record_token ==
                            SGI_KONA_CMD_GL_TEX_BIND_VOLUME_SHORT;
    uint32_t storage = (s->texture_bind[2] >> 14) & 0x7f;

    /* A bind's record follows it; until then nothing is bound or refused. */
    if (!s->texture_bind_valid || handle == UINT32_MAX ||
        !s->texture_record_valid) {
        sgi_kona_texture_bind(&s->textures, UINT32_MAX, NULL);
        return;
    }
    sgi_kona_texture_defaults(&params);
    if (target < 1 || target > 3 ||
        (target == 2 && (s->texture_bind[3] & 0xf00) !=
            ((s->texture_bind[2] & 0xc00) ? 0xf00 : 0)) ||
        !s->texture_record_valid ||
        !sgi_kona_gl_texture_internal_format(index, &base, &internal) ||
        !sgi_kona_gl_texture_format_storage(index, &format_storage) ||
        storage != format_storage ||
        !(single ?
          sgi_kona_gl_texture_storage_volume_single_layout(storage,
              s->texture_record[0]) :
          sgi_kona_gl_texture_storage_binding_layout(storage,
              volume ? SGI_GL_TEXTURE_3D :
              one_dimensional ? SGI_GL_TEXTURE_1D : SGI_GL_TEXTURE_2D,
              s->texture_record[0])) ||
        (volume ?
            ((s->texture_record_token != SGI_KONA_CMD_GL_TEX_BIND_VOLUME &&
              s->texture_record_token !=
                  SGI_KONA_CMD_GL_TEX_BIND_VOLUME_SHORT) ||
             !sgi_kona_gl_texture_volume_sampler(s->texture_bind,
                 s->texture_record,
                 s->texture_record_token == SGI_KONA_CMD_GL_TEX_BIND_VOLUME,
                 &params)) :
            (s->texture_record_token != SGI_KONA_CMD_GL_TEX_BIND_2D ||
             !(one_dimensional ?
               sgi_kona_gl_texture_sampler_1d(s->texture_bind,
                                               s->texture_record, &params) :
               sgi_kona_gl_texture_sampler(s->texture_bind, s->texture_record,
                                            &params)))) ||
        !sgi_kona_gl_texture_transform(s->texture_transform[1],
                                       s->texture_transform[2],
                                       s->texture_transform[3],
                                       params.post_scale, params.post_bias)) {
        sgi_kona_texture_bind(&s->textures, UINT32_MAX, NULL);
        qemu_log_mask(LOG_UNIMP, "sgi-kona: unsupported native texture sampler;"
                      " textured draws refused\n");
        return;
    }
    sgi_kona_gl_texture_volume_dimensions(s->texture_bind, &width, &height,
                                           &depth);
    if (!sgi_kona_texture_native_format(&s->textures, handle, storage) ||
        !sgi_kona_texture_native_internal_format(&s->textures, handle, base,
                                                 internal) ||
        (volume ?
            !sgi_kona_texture_native_volume_dimensions(&s->textures, handle,
                                                        width, height, depth) :
            !(one_dimensional ?
               sgi_kona_texture_native_dimensions_1d(&s->textures, handle,
                                                       width) :
               sgi_kona_texture_native_dimensions(&s->textures, handle, width,
                                                    height))) ||
        !sgi_kona_texture_native_layout(&s->textures, handle,
                                        s->texture_record[single ? 1 : 3]) ||
        (volume && !sgi_kona_texture_native_volume_single(&s->textures, handle,
                                                          single)) ||
        !sgi_kona_texture_bind(&s->textures, handle, &params)) {
        sgi_kona_texture_bind(&s->textures, UINT32_MAX, NULL);
        qemu_log_mask(LOG_UNIMP, "sgi-kona: native texture binding refused for"
                      " model storage budget\n");
    }
}

/* Keep a complete native load's texels in its texture. */
static void sgi_kona_texture_upload_publish(SGIKonaState *s)
{
    const SGIKonaTextureUpload *u = &s->texture_upload;
    uint32_t base = sgi_kona_texture_storage_base(u->header);
    uint32_t header[SGI_KONA_GL_TEX_LOAD_WORDS];

    memcpy(header, u->header, sizeof(header));
    if (sgi_kona_texture_copy(header)) {
        header[9] = SGI_KONA_TEXTURE_COPIED_STORAGE;
    }
    if (!sgi_kona_texture_native_upload(&s->textures, header, u->pixels,
                                        base, base)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: native texture publication"
                      " refused for handle 0x%" PRIx32 "\n", u->header[10]);
    }
}

/* An allocated texture object may still have no sampleable base image. */
static bool sgi_kona_texture_bound_ordinary_2d(SGIKonaState *s,
                                                uint32_t handle)
{
    const SGIGLTexImage *image;
    const SGIGLTexPyramid *pyramid;

    if (!s->textures.bound || s->textures.bound_handle != handle) {
        return false;
    }
    image = sgi_kona_texture_resolve(&s->textures);
    if (!image || image->one_dimensional || image->volume ||
        (image->min_filter != SGI_GL_NEAREST &&
         image->min_filter != SGI_GL_LINEAR) ||
        (image->mag_filter != SGI_GL_NEAREST &&
         image->mag_filter != SGI_GL_LINEAR) ||
        !image->pyramid || !image->pyramid->levels ||
        image->pyramid->count > SGI_GL_TEX_LEVELS_MAX) {
        return false;
    }
    pyramid = image->pyramid;
    for (unsigned k = 0; k < pyramid->count; k++) {
        const SGIGLTexLevel *level = &pyramid->levels[k];

        if (level->level == pyramid->base_level &&
            level->read_texel && level->width && level->height &&
            is_power_of_2(level->width) &&
            is_power_of_2(level->height)) {
            return true;
        }
    }
    return false;
}

static void sgi_kona_texture_association_forget(SGIKonaState *s,
                                                uint32_t handle)
{
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        SGIKonaGLPipe *p;

        if (!s->state[i]) {
            continue;
        }
        p = &s->state[i]->pipe;
        for (unsigned slot = 0; slot < 2; slot++) {
            if ((p->texture_assoc_valid & (1u << slot)) &&
                p->texture_assoc_handle[slot] == handle) {
                p->texture_assoc_valid &= ~(1u << slot);
            }
        }
    }
}


static uint32_t sgi_kona_gl_tex_native_words(SGIKonaState *s, int i,
                                              const uint32_t *h)
{
    const char *reason = NULL;
    uint64_t pixels, words, stride;
    uint32_t bytes = sgi_kona_texture_source_bytes(h[3], h[4]);
    SGIKonaTextureUpload *u = &s->texture_upload;
    SGIKonaGLPixelLayout layout = { 0 };
    const SGIKonaGLPipe *p;
    uint32_t dma_phase = 0;
    bool bitmap = sgi_kona_texture_bitmap(h);

    sgi_kona_texture_upload_clear(s);
    /* Unknown source pairs cannot be counted or staged. */
    if ((h[0] != SGI_GL_TEXTURE_1D && h[0] != SGI_GL_TEXTURE_2D &&
         h[0] != SGI_GL_TEXTURE_3D) || !sgi_kona_texture_source_valid(h) ||
        h[6] > 1 || h[7] > 1 || h[8] || !h[1] || !h[2] ||
        h[1] > SGI_KONA_GL_IMAGE_MAX || h[2] > SGI_KONA_GL_IMAGE_MAX ||
        h[5] > 3 || i == SGI_KONA_CONTEXT_NONE) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: native texture header refused:"
                      " payload length unknown, stream recovery unsafe\n");
        s->texture_dma_discard = h[6] == 1;
        return 0;
    }
    p = &s->state[i]->pipe;
    if (!h[6]) {
        if (!sgi_kona_gl_pixels_pio_layout(h + 1, p->unpack_row_length,
                                            p->unpack_skip_rows,
                                            p->unpack_skip_pixels,
                                            p->unpack_alignment, &layout) ||
            layout.payload_words > UINT32_MAX) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: native texture header refused:"
                          " payload length unknown, stream recovery unsafe\n");
            return 0;
        }
        stride = layout.stride;
        words = layout.payload_words;
    } else {
        SGIKonaGLPixelDMALayout dma;

        if (!sgi_kona_gl_pixels_dma_layout(h + 1, p->unpack_row_length,
                                            p->unpack_skip_rows,
                                            p->unpack_skip_pixels,
                                            p->unpack_alignment, &dma)) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: native texture DMA framing"
                          " unsupported; source completed unread\n");
            s->texture_dma_discard = true;
            return 0;
        }
        layout = dma.pixels;
        dma_phase = dma.source_phase;
        stride = layout.stride;
        words = 0;
    }
    if (stride > UINT32_MAX) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: native texture header refused:"
                      " payload length unknown, source stride exceeds "
                      "model addressing\n");
        return 0;
    }
    pixels = (uint64_t)h[1] * h[2];
    if (sgi_kona_gl_transfer_refused(s, i)) {
        reason = "native texture pixel transfer unsupported";
    } else if (sgi_kona_gl_filter_2d(&s->state[i]->imaging) >= 0 ||
               ((s->state[i]->imaging.enabled & 1) &&
                s->state[i]->imaging.filters[0].width)) {
        /*
         * A convolved texture image is smaller than its header's, and
         * how the library and the pipe place it is unknown.
         */
        reason = "convolution of texture images is not modelled";
    } else if (sgi_kona_gl_histogram_sinks(s->state[i])) {
        reason = "a sinking histogram or minmax on texture images is not"
                 " modelled";

    } else if (!sgi_kona_texture_native_header(h)) {
        /*
         * A DMA source whose stride is shorter than its rows never gets
         * here: sgi_kona_gl_pixels_dma_layout() refuses it above, and its
         * DMA completes unread.
         */
        reason = "unsupported native destination layout";
    } else if (words > SGI_KONA_TEXTURE_BYTES / sizeof(uint32_t) ||
               pixels * DIV_ROUND_UP(MAX(bytes, 4), 4) >
               SGI_KONA_TEXTURE_BYTES / sizeof(uint32_t)) {
        reason = "payload exceeds model staging capacity";
    } else {
        u->pixels = g_try_new0(uint32_t,
                              pixels * DIV_ROUND_UP(MAX(bytes, 4), 4));
        if (!u->pixels) {
            reason = "model staging allocation failed";
        }
    }
    if (reason) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: native texture header refused: %s;"
                      " %s\n", reason, h[6] ? "DMA source completed unread" :
                                           "draining framed rows");
        s->texture_dma_discard = h[6];
    } else {
        memcpy(u->header, h, sizeof(u->header));
        u->count = pixels;
        u->source_stride = stride;
        u->swap_bytes = p->unpack_swap_bytes;
        u->transfer = p->pixel_transfer;
        memcpy(u->colour_matrix, s->state[i]->imaging.colour_matrix,
               sizeof(u->colour_matrix));
        u->transfer_captured = true;
        u->pio_layout = !h[6];
        u->pio_phase = h[6] ? 0 : layout.phase;
        u->dma_layout = h[6];
        u->dma_phase = dma_phase;
        u->bitmap_lsb = bitmap && p->unpack_lsb_first;
        u->bitmap_skip = bitmap ? p->unpack_skip_pixels & 7 : 0;
        u->bitmap_phase = bitmap ? layout.phase : 0;
        s->texture_state_used = true;
    }
    return h[6] ? 0 : words;
}

static bool sgi_kona_gl_pixels_layout(const SGIKonaGLImageUpload *u,
                                      SGIKonaGLPixelLayout *layout)
{
    if (u->header[5]) {
        SGIKonaGLPixelDMALayout dma;

        if (!sgi_kona_gl_pixels_dma_layout(u->header, u->row_length,
                                            u->skip_rows, u->skip_pixels,
                                            u->alignment, &dma)) {
            return false;
        }
        *layout = dma.pixels;
        return true;
    }
    return sgi_kona_gl_pixels_pio_layout(u->header, u->row_length,
                                         u->skip_rows, u->skip_pixels,
                                         u->alignment, layout);
}

static uint32_t sgi_kona_gl_pixels_open(SGIKonaState *s, int i,
                                        const uint32_t *a,
                                        const SGIKonaGLPipe *p)
{
    SGIKonaGLImageUpload *u = &s->gl_image_upload;
    SGIKonaGLPixelLayout layout;
    SGIKonaGLImageUpload framing = {
        .row_length = p->unpack_row_length,
        .skip_rows = p->unpack_skip_rows,
        .skip_pixels = p->unpack_skip_pixels,
        .alignment = p->unpack_alignment,
        .lsb_first = a[3] == 7 && p->unpack_lsb_first,
    };
    bool refused;
    uint64_t count = (uint64_t)a[0] * a[1];

    memcpy(framing.header, a, 8 * sizeof(uint32_t));
    if (!sgi_kona_gl_pixels_layout(&framing, &layout) ||
        layout.payload_words > UINT32_MAX) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: DrawPixels source framing"
                      " unsupported; source length unknown,"
                      " stream recovery unsafe\n");
        s->image_dma_discard = a[5] == 1;
        return 0;
    }
    if (a[0] > SGI_KONA_GL_IMAGE_MAX || a[1] > SGI_KONA_GL_IMAGE_MAX ||
        layout.payload_words > SGI_KONA_TEXTURE_BYTES / sizeof(uint32_t) ||
        count > SGI_KONA_TEXTURE_BYTES /
                (a[3] == 1 && !p->pixel_transfer.map_colour &&
                 (i == SGI_KONA_CONTEXT_NONE ||
                  !sgi_kona_gl_imaging_active(s->state[i])) ?
                 sizeof(uint32_t) : 4 * sizeof(double))) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: DrawPixels capacity exceeded; %s\n",
                      a[5] ? "DMA completed unread" : "draining framed words");
        s->image_dma_discard = a[5];
        return a[5] ? 0 : layout.payload_words;
    }
    if (a[2] == 12 && i != SGI_KONA_CONTEXT_NONE &&
        sgi_kona_gl_stencil_bits(s, i) == 0) {
        sgi_kona_gl_set_error(s, 0x502);
    }
    if (i != SGI_KONA_CONTEXT_NONE &&
        s->state[i]->gl.cur.index_bits && a[2] < 11) {
        sgi_kona_gl_set_error(s, 0x502);
    }
    refused = a[7] || i == SGI_KONA_CONTEXT_NONE || s->state[i]->gl.in_begin ||
        (a[2] == 11 && !p->draw_config_valid) ||
        (a[2] < 11 && s->state[i]->gl.cur.index_bits) ||
        (a[2] == 12 && sgi_kona_gl_stencil_bits(s, i) != 8) ||
        (a[2] == 13 && p->depth_bits && p->depth_bits != 24) ||
        p->pixel_zoom_unsupported ||
        sgi_kona_gl_transfer_refused(s, i);
    if (refused) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: DrawPixels profile refused; %s\n",
                      a[5] ? "DMA not staged" : "draining framed words");
        if (!a[5]) {
            return layout.payload_words;
        }
    }
    *u = framing;
    u->header[8] = p->unpack_swap_bytes;
    u->refused = refused;
    u->context = i;
    u->generation = sgi_kona_context_generation(s, i);
    u->count = layout.payload_words;
    u->draw_pixels = true;
    u->words = g_try_new0(uint32_t, u->count);
    if (!u->words) {
        sgi_kona_gl_image_clear(s);
        s->image_dma_discard = a[5];
    }
    return a[5] ? 0 : layout.payload_words;
}

static bool sgi_kona_gl_pixels_done(SGIKonaState *s)
{
    SGIKonaGLImageUpload *u = &s->gl_image_upload;
    SGIKonaGLPixelLayout layout;
    SGIGLPixelTransfer transfer;
    SGIGLSink sink;
    uint32_t i = u->context;
    const SGIKonaGLPipe *p;
    g_autofree uint32_t *rgba = NULL;
    g_autofree double (*rgba_double)[4] = NULL;
    g_autofree double *depth = NULL;
    g_autofree uint8_t *line = NULL;
    bool depth_image = u->header[2] == 13;
    bool stencil_image = u->header[2] == 12;
    bool indexed_image = u->header[2] == 11;
    bool index_destination;
    bool scalar, mapped, imaged, rendered = false;
    uint64_t count;

    if (!u->count || u->refused || i != s->context_current ||
        !sgi_kona_context_owner(s, i, u->generation) ||
        s->state[i]->gl.in_begin || !sgi_kona_gl_pixels_layout(u, &layout)) {
        goto out;
    }
    p = &s->state[i]->pipe;
    if (p->pixel_zoom_unsupported || sgi_kona_gl_transfer_refused(s, i)) {
        goto out;
    }
    index_destination = indexed_image && s->state[i]->gl.cur.index_bits;
    imaged = !depth_image && !stencil_image && !index_destination &&
             sgi_kona_gl_imaging_active(s->state[i]);
    mapped = !depth_image && !stencil_image && !indexed_image &&
             (p->pixel_transfer.map_colour || imaged);
    scalar = u->header[3] != 1 || indexed_image || mapped;
    count = (uint64_t)layout.width * layout.height;
    if (depth_image) {
        depth = g_try_new(double, count);
    } else if (scalar && !stencil_image && !index_destination) {
        rgba_double = g_try_malloc_n(count, sizeof(*rgba_double));
    } else {
        rgba = g_try_new(uint32_t, count);
    }
    if (depth_image ? !depth :
        scalar && !stencil_image && !index_destination ? !rgba_double : !rgba) {
        goto out;
    }
    line = g_try_malloc((uint64_t)layout.width * layout.pixel_bytes);
    if (!line && layout.width && layout.pixel_bytes) {
        goto out;
    }
    for (uint32_t row = 0; row < layout.height; row++) {
        /* A failure anywhere in the row refuses the image, as it would. */
        if (!sgi_kona_gl_pixels_pio_bytes(&layout, u->words, u->count, row,
                                          0, line, (uint64_t)layout.width *
                                          layout.pixel_bytes)) {
            goto out;
        }
        for (uint32_t x = 0; x < layout.width; x++) {
            uint32_t source[SGI_KONA_GL_PIXEL_WORDS_MAX] = { 0 };
            const uint8_t *bytes = line + (uint64_t)x * layout.pixel_bytes;
            uint32_t pixel = row * layout.width + x;

            for (uint32_t k = 0; k < layout.pixel_bytes; k++) {
                source[k / 4] |= (uint32_t)bytes[k] << (24 - (k % 4) * 8);
            }
            if (u->header[3] == 7) {
                unsigned bit = (x + (u->skip_pixels & 7)) & 7;
                uint8_t byte;

                if (!sgi_kona_gl_pixels_pio_byte(&layout, u->words, u->count,
                    row, (x + (u->skip_pixels & 7)) / 8, &byte)) {
                    goto out;
                }
                source[0] = (byte >> (u->lsb_first ? bit : 7 - bit)) & 1;
            }
            if (stencil_image) {
                double index;

                if (!sgi_kona_gl_pixel_index(
                    u->header[3] == 7 ? 5 : u->header[3], source,
                    u->header[3] != 7 && u->header[8], &index) ||
                    !sgi_kona_gl_stencil_index(p, index, &rgba[pixel])) {
                    goto out;
                }
                continue;
            }
            if (indexed_image) {
                double index;

                if (!sgi_kona_gl_pixel_index(
                    u->header[3] == 7 ? 5 : u->header[3], source,
                    u->header[3] != 7 && u->header[8], &index) ||
                    (index_destination ?
                     !sgi_kona_gl_colour_index(&p->pixel_transfer, index,
                                                &rgba[pixel]) :
                     !sgi_kona_gl_index_rgba(&p->pixel_transfer, index,
                                              rgba_double[pixel]))) {
                    goto out;
                }
                continue;
            }
            if (depth_image) {
                double values[4];

                if (!sgi_kona_gl_pixel_rgba_double(0, u->header[3], source,
                                                    u->header[8], values)) {
                    goto out;
                }
                depth[pixel] = values[0];
                continue;
            }
            /* Not scalar: unsigned bytes, converted exactly as integers. */
            if (scalar ?
                !sgi_kona_gl_pixel_rgba_double(u->header[2], u->header[3],
                    source, u->header[8], rgba_double[pixel]) :
                !sgi_kona_gl_pixel_rgba8(u->header[2], source[0],
                                         &rgba[pixel])) {
                goto out;
            }
            if (mapped &&
                !sgi_kona_gl_rgba_transfer(&p->pixel_transfer,
                                            rgba_double[pixel])) {
                goto out;
            }
        }
    }
    sgi_kona_gl_sink(s, &sink);
    if (stencil_image) {
        rendered = sgi_gl_draw_pixels_stencil(&s->state[i]->gl, &sink,
            layout.width, layout.height, rgba,
            sgi_kona_gl_float(p->pixel_zoom[0]),
            sgi_kona_gl_float(p->pixel_zoom[1]));
        goto out;
    }
    if (depth_image) {
        rendered = sgi_gl_draw_pixels_depth_transfer(
            &s->state[i]->gl, &sink, layout.width, layout.height, depth,
            sgi_kona_gl_float(p->pixel_zoom[0]),
            sgi_kona_gl_float(p->pixel_zoom[1]),
            sgi_kona_gl_float(p->pixel_transfer.depth_scale),
            sgi_kona_gl_float(p->pixel_transfer.depth_bias));
        goto out;
    }
    if (index_destination) {
        rendered = sgi_gl_draw_pixels_index(&s->state[i]->gl, &sink,
            layout.width, layout.height, rgba,
            sgi_kona_gl_float(p->pixel_zoom[0]),
            sgi_kona_gl_float(p->pixel_zoom[1]));
        goto out;
    }
    if (indexed_image || mapped) {
        const SGIGLPixelTransfer identity = { .scale = { 1, 1, 1, 1 } };
        uint32_t width = layout.width, height = layout.height;

        if (imaged &&
            !sgi_kona_gl_imaging(s->state[i], &rgba_double, &width,
                                 &height)) {
            goto out;
        }
        if (width && height) {
            sgi_gl_draw_pixels_double_transfer(&s->state[i]->gl, &sink,
                                               width, height, rgba_double,
                sgi_kona_gl_float(p->pixel_zoom[0]),
                sgi_kona_gl_float(p->pixel_zoom[1]), &identity);
        }
        rendered = true;
        goto out;
    }
    for (unsigned k = 0; k < 4; k++) {
        transfer.scale[k] = sgi_kona_gl_float(p->pixel_transfer.scale[k]);
        transfer.bias[k] = sgi_kona_gl_float(p->pixel_transfer.bias[k]);
        if (!isfinite(transfer.scale[k]) || !isfinite(transfer.bias[k])) {
            goto out;
        }
    }
    if (scalar) {
        sgi_gl_draw_pixels_double_transfer(&s->state[i]->gl, &sink,
                                           layout.width,
                                          layout.height, rgba_double,
                                          sgi_kona_gl_float(p->pixel_zoom[0]),
                                          sgi_kona_gl_float(p->pixel_zoom[1]),
                                          &transfer);
    } else {
        sgi_gl_draw_pixels_rgba_transfer(&s->state[i]->gl, &sink, layout.width,
                                         layout.height, rgba,
                                         sgi_kona_gl_float(p->pixel_zoom[0]),
                                         sgi_kona_gl_float(p->pixel_zoom[1]),
                                         &transfer);
    }
    rendered = true;
out:
    sgi_kona_gl_image_clear(s);
    return rendered;
}

/* DrawPixels shares source staging, not texture destination semantics. */
static uint32_t sgi_kona_gl_draw_pixels_open(SGIKonaState *s, int i,
                                             const uint32_t *a)
{
    const SGIKonaGLPipe defaults = { .unpack_alignment = 4 };
    const SGIKonaGLPipe *p = i == SGI_KONA_CONTEXT_NONE ?
                            &defaults : &s->state[i]->pipe;

    sgi_kona_gl_image_clear(s);
    sgi_kona_texture_upload_clear(s);
    if (i != SGI_KONA_CONTEXT_NONE && s->state[i]->gl.in_begin) {
        sgi_kona_gl_set_error(s, 0x502);
    }
    return sgi_kona_gl_pixels_open(s, i, a, p);
}

static void sgi_kona_gl_draw_pixels_done(SGIKonaState *s)
{
    SGIKonaTextureUpload *u = &s->texture_upload;
    unsigned i = u->draw_context;
    SGIGLSink sink;
    SGIGLPixelTransfer transfer;

    if (u->count && u->draw_pixels && s->context_current == i &&
        !s->state[i]->pipe.pixel_zoom_unsupported &&
        !sgi_kona_gl_transfer_refused(s, i) &&
        sgi_kona_gl_imaging_active(s->state[i])) {
        const SGIGLPixelTransfer identity = { .scale = { 1, 1, 1, 1 } };
        const SGIKonaGLPipe *p = &s->state[i]->pipe;
        uint32_t width = u->header[1], height = u->header[2];
        g_autofree double (*pixels)[4] =
            g_try_malloc_n((uint64_t)width * height, sizeof(*pixels));

        for (uint64_t at = 0; pixels && at < (uint64_t)width * height;
             at++) {
            for (unsigned k = 0; k < 4; k++) {
                pixels[at][k] = ((u->pixels[at] >> (24 - k * 8)) & 255) /
                                255.0;
            }
            if (!sgi_kona_gl_rgba_transfer(&p->pixel_transfer, pixels[at])) {
                g_clear_pointer(&pixels, g_free);
            }
        }
        if (pixels &&
            sgi_kona_gl_imaging(s->state[i], &pixels, &width, &height) &&
            width && height) {
            sgi_kona_gl_sink(s, &sink);
            sgi_gl_draw_pixels_double_transfer(&s->state[i]->gl, &sink,
                                               width, height, pixels,
                                               sgi_kona_gl_float(
                                                   p->pixel_zoom[0]),
                                               sgi_kona_gl_float(
                                                   p->pixel_zoom[1]),
                                               &identity);
        }
    } else if (u->count && u->draw_pixels && s->context_current == i &&
        !s->state[i]->pipe.pixel_zoom_unsupported &&
        !sgi_kona_gl_transfer_refused(s, i)) {
        for (unsigned channel = 0; channel < 4; channel++) {
            transfer.scale[channel] = sgi_kona_gl_float(
                s->state[i]->pipe.pixel_transfer.scale[channel]);
            transfer.bias[channel] = sgi_kona_gl_float(
                s->state[i]->pipe.pixel_transfer.bias[channel]);
        }
        sgi_kona_gl_sink(s, &sink);
        sgi_gl_draw_pixels_rgba_transfer(&s->state[i]->gl, &sink, u->header[1],
                                         u->header[2], u->pixels,
                                         sgi_kona_gl_float(
                                             s->state[i]->pipe.pixel_zoom[0]),
                                         sgi_kona_gl_float(
                                             s->state[i]->pipe.pixel_zoom[1]),
                                         &transfer);
    }
    sgi_kona_texture_upload_clear(s);
}

static bool sgi_kona_gl_bitmap_position_valid(const uint32_t *a,
                                               bool totals)
{
    float xo = sgi_kona_gl_float(a[0]), yo = sgi_kona_gl_float(a[1]);
    float xm = sgi_kona_gl_float(a[2]), ym = sgi_kona_gl_float(a[3]);

    return isfinite(xo) && isfinite(yo) && isfinite(xm) && isfinite(ym) &&
           (!totals || (isfinite(xm - xo) && isfinite(ym - yo)));
}

static uint32_t sgi_kona_gl_bitmap_open(SGIKonaState *s, int i,
                                        const uint32_t *a)
{
    SGIKonaGLBitmapUpload *u = &s->gl_bitmap_upload;
    uint32_t words;

    memset(u, 0, sizeof(*u));
    if (i != SGI_KONA_CONTEXT_NONE && s->state[i]->gl.in_begin) {
        sgi_kona_gl_set_error(s, 0x502);
    }
    words = DIV_ROUND_UP((uint64_t)a[7], 4);
    if (!a[7] || a[7] > SGI_KONA_GL_BITMAP_BYTES) {
        u->refused = true;
        qemu_log_mask(LOG_UNIMP, "sgi-kona: native inline bitmap byte count"
                      " beyond supported profile; draining framed bytes\n");
        return words;
    }
    memcpy(u->header, a, sizeof(u->header));
    u->count = a[7];
    u->context = i;
    u->generation = sgi_kona_context_generation(s, i);
    u->refused = i == SGI_KONA_CONTEXT_NONE || s->state[i]->gl.in_begin ||
                 !a[0] || (a[0] & 7) || !a[1] ||
                 a[0] > SGI_KONA_GL_IMAGE_MAX ||
                 a[1] > SGI_KONA_GL_IMAGE_MAX ||
                 (uint64_t)(a[0] / 8) * a[1] != a[7] ||
                 a[6] != words + 1 ||
                 !sgi_kona_gl_bitmap_position_valid(a + 2, true);
    if (u->refused) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: native inline bitmap refused;"
                      " draining framed bytes\n");
    }
    return words;
}

static void sgi_kona_gl_bitmap_done(SGIKonaState *s)
{
    SGIKonaGLBitmapUpload *u = &s->gl_bitmap_upload;
    unsigned i = u->context;
    const uint32_t *a = u->header;
    SGIGLBitmap b;
    SGIGLSink sink;

    if (u->count && !u->refused && s->context_current == i &&
        sgi_kona_context_owner(s, i, u->generation) &&
        !s->state[i]->gl.in_begin &&
        sgi_gl_bitmap_place(&s->state[i]->gl, a[0], a[1],
                            sgi_kona_gl_float(a[2]), sgi_kona_gl_float(a[3]),
                            sgi_kona_gl_float(a[4]) - sgi_kona_gl_float(a[2]),
                            sgi_kona_gl_float(a[5]) - sgi_kona_gl_float(a[3]),
                            &b)) {
        sgi_kona_gl_sink(s, &sink);
        sgi_gl_bitmap_bytes(&s->state[i]->gl, &sink, &b, 0, u->bytes, u->count);
    }
    memset(u, 0, sizeof(*u));
}

static void sgi_kona_gl_bitmap_move(SGIKonaState *s, int i,
                                    const uint32_t *a)
{
    SGIGLBitmap b;

    if (i == SGI_KONA_CONTEXT_NONE) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: native bitmap without context"
                      " refused\n");
        return;
    }
    if (s->state[i]->gl.in_begin) {
        sgi_kona_gl_set_error(s, 0x502);
        return;
    }
    if (a[0] || a[1] || a[2] || a[3] || a[4]) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: nonzero raw bitmap profile refused;"
                      " source length unknown, stream recovery unsafe\n");
        return;
    }
    if (!sgi_kona_gl_bitmap_position_valid(a + 5, false)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: nonfinite bitmap movement"
                      " refused\n");
        return;
    }
    sgi_gl_bitmap_place(&s->state[i]->gl, 0, 0, sgi_kona_gl_float(a[5]),
                        sgi_kona_gl_float(a[6]), sgi_kona_gl_float(a[7]),
                        sgi_kona_gl_float(a[8]), &b);
}

static bool sgi_kona_gl_raw_bitmap_layout(const SGIKonaGLImageUpload *u,
                                           SGIKonaGLBitmapLayout *layout)
{
    if (u->header[3] == 1) {
        SGIKonaGLBitmapDMALayout d;

        if (!sgi_kona_gl_bitmap_dma_layout(u->header, u->row_length,
                                           u->skip_rows, u->skip_pixels,
                                           u->alignment, &d)) {
            return false;
        }
        *layout = d.bitmap;
        return true;
    }
    return sgi_kona_gl_bitmap_pio_layout(u->header, u->row_length,
                                         u->skip_rows, u->skip_pixels,
                                         u->alignment, layout);
}

static uint32_t sgi_kona_gl_raw_bitmap_open(SGIKonaState *s, int i,
                                            const uint32_t *a)
{
    SGIKonaGLImageUpload *u = &s->gl_image_upload;
    const SGIKonaGLPipe defaults = { .unpack_alignment = 4 };
    const SGIKonaGLPipe *p = i == SGI_KONA_CONTEXT_NONE ?
                            &defaults : &s->state[i]->pipe;
    SGIKonaGLBitmapLayout layout;
    SGIKonaGLImageUpload framing = {
        .row_length = p->unpack_row_length,
        .skip_rows = p->unpack_skip_rows,
        .skip_pixels = p->unpack_skip_pixels,
        .alignment = p->unpack_alignment,
    };

    sgi_kona_gl_image_clear(s);
    if (!a[0] && !a[1]) {
        sgi_kona_gl_bitmap_move(s, i, a);
        return 0;
    }
    if (i != SGI_KONA_CONTEXT_NONE && s->state[i]->gl.in_begin) {
        sgi_kona_gl_set_error(s, 0x502);
    }
    memcpy(framing.header, a, sizeof(framing.header));
    if (!sgi_kona_gl_raw_bitmap_layout(&framing, &layout) ||
        layout.payload_words > UINT32_MAX) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: raw bitmap framing unsupported;"
                      " source length unknown, stream recovery unsafe\n");
        s->image_dma_discard = a[3] == 1;
        return 0;
    }
    if (a[0] > SGI_KONA_GL_IMAGE_MAX || a[1] > SGI_KONA_GL_IMAGE_MAX ||
        layout.payload_words > SGI_KONA_TEXTURE_BYTES / sizeof(uint32_t)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: oversized raw bitmap; %s\n",
                      a[3] ? "DMA completed unread" : "draining framed words");
        s->image_dma_discard = a[3];
        return a[3] ? 0 : layout.payload_words;
    }
    if (a[3]) {
        sgi_kona_texture_upload_clear(s);
    }
    *u = framing;
    u->lsb_first = p->unpack_lsb_first;
    u->context = i;
    u->generation = sgi_kona_context_generation(s, i);
    u->count = layout.payload_words;
    u->refused = i == SGI_KONA_CONTEXT_NONE || s->state[i]->gl.in_begin ||
                 !sgi_kona_gl_bitmap_position_valid(a + 5, false);
    if (u->refused) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: raw bitmap profile refused; %s\n",
                      a[3] ? "DMA not staged" : "draining framed words");
    }
    u->words = g_try_new0(uint32_t, u->count);
    if (!u->words) {
        sgi_kona_gl_image_clear(s);
        s->image_dma_discard = a[3];
    }
    return a[3] ? 0 : layout.payload_words;
}

static bool sgi_kona_gl_raw_bitmap_done(SGIKonaState *s)
{
    SGIKonaGLImageUpload *u = &s->gl_image_upload;
    SGIKonaGLBitmapLayout layout;
    SGIGLBitmap b;
    SGIGLSink sink;
    uint32_t i = u->context, row_bytes;
    bool rendered = false;
    g_autofree uint8_t *bytes = NULL;

    if (!u->count || u->refused || i != s->context_current ||
        !sgi_kona_context_owner(s, i, u->generation) ||
        s->state[i]->gl.in_begin ||
        !sgi_kona_gl_raw_bitmap_layout(u, &layout)) {
        goto out;
    }
    row_bytes = DIV_ROUND_UP(layout.width, 8);
    bytes = g_try_malloc0((uint64_t)row_bytes * layout.height);
    if (!bytes) {
        goto out;
    }
    for (uint32_t row = 0; row < layout.height; row++) {
        for (uint32_t k = 0; k < row_bytes; k++) {
            uint8_t *out = &bytes[row * row_bytes + k];

            if (!sgi_kona_gl_bitmap_pio_byte(&layout, u->words, u->count,
                                              u->lsb_first, row, k,
                                              out)) {
                goto out;
            }
        }
    }
    if (sgi_gl_bitmap_place(&s->state[i]->gl, layout.width, layout.height,
                            sgi_kona_gl_float(u->header[5]),
                            sgi_kona_gl_float(u->header[6]),
                            sgi_kona_gl_float(u->header[7]),
                            sgi_kona_gl_float(u->header[8]), &b)) {
        sgi_kona_gl_sink(s, &sink);
        sgi_gl_bitmap_bytes(&s->state[i]->gl, &sink, &b, 0, bytes,
                            row_bytes * layout.height);
        rendered = true;
    }
out:
    sgi_kona_gl_image_clear(s);
    return rendered;
}

static uint32_t sgi_kona_gl_tex_load_words(SGIKonaState *s, int i,
                                           const uint32_t *h)
{
    const SGIKonaGLPipe *p;
    uint32_t bytes;
    uint64_t row, stride, words;

    if (h[3] == 5 || h[4] == 1 || i == SGI_KONA_CONTEXT_NONE) {
        return sgi_kona_gl_tex_native_words(s, i, h);
    }
    bytes = sgi_kona_gl_pixel_bytes(h[3], h[4]);
    if (!bytes) {
        return sgi_kona_gl_tex_native_words(s, i, h);
    }
    p = &s->state[i]->pipe;
    row = (uint64_t)(p->unpack_row_length ? p->unpack_row_length :
                     h[1]) * bytes;
    stride = ROUND_UP(row, MAX(p->unpack_alignment, 1));

    if (h[6]) {
        return 0;
    }
    if (!bytes || stride % 4 || h[1] > SGI_KONA_GL_IMAGE_MAX ||
        h[2] > SGI_KONA_GL_IMAGE_MAX) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: texel load of %" PRIu32 "x%"
                      PRIu32 " format 0x%" PRIx32 " type 0x%" PRIx32
                      " with unpack alignment %" PRIu32 " row length %"
                      PRIu32 "; its rows are not framed\n", h[1], h[2],
                      h[3], h[4], p->unpack_alignment,
                      p->unpack_row_length);
        return 0;
    }
    words = DIV_ROUND_UP((h[5] & 3) + (uint64_t)h[1] * bytes, 4) * h[2];
    return words;
}

/*
 * 0x33 pname value, glPixelStorei: the unpack and pack state the pipe
 * uses for images (GL_UNPACK_SWAP_BYTES, GL_UNPACK_ROW_LENGTH,
 * GL_UNPACK_ALIGNMENT and the row-length/alignment pack forms). The library
 * sends the state and converts no image, so the pipe holds it (inferred). Other
 * parameters are kept by no part of the model and logged.
 */
static void sgi_kona_gl_pixel_store(SGIKonaState *s, int i, uint32_t pname,
                                    uint32_t value)
{
    SGIKonaGLPipe *p = &s->state[i]->pipe;

    if (s->state[i]->gl.in_begin) {
        sgi_kona_gl_set_error(s, 0x502);
        return;
    }
    switch (pname) {
    case 0x0cf1:                /* GL_UNPACK_LSB_FIRST */
        p->unpack_lsb_first = value != 0;
        break;
    case 0x0cf3:                /* GL_UNPACK_SKIP_ROWS */
        p->unpack_skip_rows = value;
        break;
    case 0x0cf4:                /* GL_UNPACK_SKIP_PIXELS */
        p->unpack_skip_pixels = value;
        break;
    case 0x0cf0:                /* GL_UNPACK_SWAP_BYTES */
        p->unpack_swap_bytes = value != 0;
        break;
    case 0x0cf2:                /* GL_UNPACK_ROW_LENGTH */
        p->unpack_row_length = value;
        break;
    case 0x0cf5:                /* GL_UNPACK_ALIGNMENT */
        p->unpack_alignment = value;
        break;
    case 0x0d01:                /* GL_PACK_LSB_FIRST */
        p->pack_lsb_first = value != 0;
        break;
    case 0x0d00:                /* GL_PACK_SWAP_BYTES */
        p->pack_swap_bytes = value != 0;
        break;
    case 0x0d02:                /* GL_PACK_ROW_LENGTH */
        p->pack_row_length = value;
        break;
    case 0x0d05:                /* GL_PACK_ALIGNMENT */
        p->pack_alignment = value;
        break;
    /*
     * Packing for GetTexImage of 3D and 4D textures alone (OpenGL 1.2.1,
     * table 4.5 and section 6.1.4; SGIS_texture4D). Zero is their initial
     * value, so storing it changes nothing; the library sends it.
     */
    case 0x806d:                /* GL_PACK_SKIP_IMAGES */
    case 0x806e:                /* GL_PACK_IMAGE_HEIGHT */
    case 0x8132:                /* GL_PACK_SKIP_VOLUMES_SGIS */
    case 0x8133:                /* GL_PACK_IMAGE_DEPTH_SGIS */
        if (!value) {
            break;
        }
        /* fall through */
    default:
        p->pixel_transfer_unsupported = true;
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glPixelStore of 0x%" PRIx32
                      " to %" PRIu32 " is not modelled\n", pname, value);
    }
}

static bool sgi_kona_gl_pixel_map_header(const uint32_t *a)
{
    return a[0] >= 0xc70 && a[0] <= 0xc79 && a[1] &&
           a[1] <= SGI_KONA_GL_PIXEL_MAP_MAX &&
           (a[0] >= 0xc76 || is_power_of_2(a[1])) &&
           sgi_kona_gl_float(a[2]) == a[1] - 1 && a[3] >= a[1] &&
           (a[3] == 256 || a[3] == 1024 || a[3] == 4096);
}

static uint32_t sgi_kona_gl_pixel_map_open(SGIKonaState *s, int i,
                                         uint32_t token, const uint32_t *a)
{
    SGIKonaGLPixelMapUpload *u = &s->gl_map_upload;

    memset(u, 0, sizeof(*u));
    memcpy(u->header, a, sizeof(u->header));
    u->token = token;
    u->context = i;
    u->generation = sgi_kona_context_generation(s, i);
    u->refused = i == SGI_KONA_CONTEXT_NONE || s->state[i]->gl.in_begin ||
                 !sgi_kona_gl_pixel_map_header(a);
    if (i != SGI_KONA_CONTEXT_NONE && s->state[i]->gl.in_begin) {
        sgi_kona_gl_set_error(s, 0x502);
    } else if (a[0] < 0xc70 || a[0] > 0xc79) {
        sgi_kona_gl_set_error(s, 0x500);
    } else if (!a[1] || a[1] > SGI_KONA_GL_PIXEL_MAP_MAX ||
               (a[0] <= 0xc75 && !is_power_of_2(a[1]))) {
        sgi_kona_gl_set_error(s, 0x501);
    }
    if (u->refused) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: unsupported pixel map upload;"
                      " draining table entries\n");
    }
    if (!a[1]) {
        memset(u, 0, sizeof(*u));
    }
    return a[1];
}

static void sgi_kona_gl_pixel_map_done(SGIKonaState *s)
{
    SGIKonaGLPixelMapUpload *u = &s->gl_map_upload;
    SGIKonaGLPixelMap *map;

    if (u->refused || u->context != s->context_current ||
        !sgi_kona_context_owner(s, u->context, u->generation)) {
        goto out;
    }
    for (unsigned k = 0; k < u->header[1]; k++) {
        uint32_t value = u->values[k];

        if (u->token == SGI_KONA_CMD_GL_PIXEL_MAP_US && value > 0xffff) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: oversized short pixel"
                          " map entry; upload refused\n");
            goto out;
        }
        if (u->header[0] <= 0xc71) {
            if (u->token == SGI_KONA_CMD_GL_PIXEL_MAP_F &&
                !sgi_kona_gl_pixel_index_round(sgi_kona_gl_float(value),
                                                 0, 0, &u->values[k])) {
                goto invalid;
            }
        } else {
            float component = u->token == SGI_KONA_CMD_GL_PIXEL_MAP_F ?
                sgi_kona_gl_float(value) :
                value / (u->token == SGI_KONA_CMD_GL_PIXEL_MAP_US ?
                         65535.0 : 4294967295.0);

            if (!isfinite(component)) {
                goto invalid;
            }
            memcpy(&u->values[k], &component, sizeof(component));
        }
    }
    if (u->header[0] == 0xc70) {
        map = &s->state[u->context]->pipe.pixel_transfer.index_map;
    } else if (u->header[0] == 0xc71) {
        map = &s->state[u->context]->pipe.pixel_transfer.stencil_map;
    } else if (u->header[0] <= 0xc75) {
        map = &s->state[u->context]->pipe.pixel_transfer.index_rgba[
            u->header[0] - 0xc72];
    } else {
        map = &s->state[u->context]->pipe.pixel_transfer.colour_rgba[
            u->header[0] - 0xc76];
    }
    memset(map, 0, sizeof(*map));
    map->count = u->header[1];
    memcpy(map->values, u->values, map->count * sizeof(*map->values));
    goto out;
invalid:
    qemu_log_mask(LOG_UNIMP, "sgi-kona: nonfinite pixel map entry;"
                  " upload refused\n");
out:
    memset(u, 0, sizeof(*u));
}

static void sgi_kona_gl_pixel_transfer(SGIKonaState *s, int i,
                                       uint32_t pname, uint32_t value)
{
    SGIKonaGLPipe *p = &s->state[i]->pipe;
    uint32_t *destination;

    if (s->state[i]->gl.in_begin) {
        sgi_kona_gl_set_error(s, 0x502);
        return;
    }
    switch (pname) {
    case 0x0d14:                /* GL_RED_SCALE */
        destination = &p->pixel_transfer.scale[0];
        break;
    case 0x0d15:                /* GL_RED_BIAS */
        destination = &p->pixel_transfer.bias[0];
        break;
    case 0x0d18:                /* GL_GREEN_SCALE */
        destination = &p->pixel_transfer.scale[1];
        break;
    case 0x0d19:                /* GL_GREEN_BIAS */
        destination = &p->pixel_transfer.bias[1];
        break;
    case 0x0d1a:                /* GL_BLUE_SCALE */
        destination = &p->pixel_transfer.scale[2];
        break;
    case 0x0d1b:                /* GL_BLUE_BIAS */
        destination = &p->pixel_transfer.bias[2];
        break;
    case 0x0d1c:                /* GL_ALPHA_SCALE */
        destination = &p->pixel_transfer.scale[3];
        break;
    case 0x0d1d:                /* GL_ALPHA_BIAS */
        destination = &p->pixel_transfer.bias[3];
        break;
    case 0x0d11:                /* GL_MAP_STENCIL */
        p->pixel_transfer.map_stencil = sgi_kona_gl_float(value) != 0;
        return;
    case 0x0d12:                /* GL_INDEX_SHIFT */
        destination = &p->pixel_transfer.index_shift;
        break;
    case 0x0d13:                /* GL_INDEX_OFFSET */
        destination = &p->pixel_transfer.index_offset;
        break;
    case 0x0d1e:                /* GL_DEPTH_SCALE */
        destination = &p->pixel_transfer.depth_scale;
        break;
    case 0x0d1f:                /* GL_DEPTH_BIAS */
        destination = &p->pixel_transfer.depth_bias;
        break;
    case 0x0d10:                /* GL_MAP_COLOR */
        p->pixel_transfer.map_colour = sgi_kona_gl_float(value) != 0;
        return;
    case 0x801c ... 0x801f:     /* GL_POST_CONVOLUTION_c_SCALE */
        destination = &p->pixel_transfer.post_conv_scale[pname - 0x801c];
        break;
    case 0x8020 ... 0x8023:     /* GL_POST_CONVOLUTION_c_BIAS */
        destination = &p->pixel_transfer.post_conv_bias[pname - 0x8020];
        break;
    case 0x80b4 ... 0x80b7:     /* GL_POST_COLOR_MATRIX_c_SCALE */
        destination = &p->pixel_transfer.post_matrix_scale[pname - 0x80b4];
        break;
    case 0x80b8 ... 0x80bb:     /* GL_POST_COLOR_MATRIX_c_BIAS */
        destination = &p->pixel_transfer.post_matrix_bias[pname - 0x80b8];
        break;
    default:
        p->pixel_transfer_unsupported = true;
        qemu_log_mask(LOG_UNIMP, "sgi-kona: unsupported pixel transfer 0x%x;"
                      " DrawPixels refused\n", pname);
        return;
    }
    *destination = value;
}

/*
 * 0x119 n, glPopAttrib's lights: n words follow, twelve for each light
 * (established). More than the context's lights would take is logged and
 * its words left unframed.
 */
static void sgi_kona_gl_pop_lights_open(SGIKonaState *s, uint32_t n)
{
    if (n % SGI_KONA_GL_POP_LIGHT_WORDS ||
        n > SGI_GL_LIGHTS * SGI_KONA_GL_POP_LIGHT_WORDS) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: pop lights with %" PRIu32
                      " words, not twelve for each of at most %u lights;"
                      " its words are not framed\n", n, SGI_GL_LIGHTS);
        return;
    }
    if (n) {
        sgi_kona_gl_data_open(s, SGI_KONA_CMD_GL_POP_LIGHTS, n);
    }
}

/*
 * The GL client's commands, framed by sgi_kona_gl_tokens[]. selector is
 * the word's low 16 bits, and s->gl_token_high its top half, the data of
 * a packed attribute form.
 */
static void sgi_kona_gl_convolution_open(SGIKonaState *s, int i,
                                         uint32_t selector,
                                         const uint32_t *args);
static void sgi_kona_gl_convolution_copy(SGIKonaState *s, int i,
                                         uint32_t selector,
                                         const uint32_t *args);
static void sgi_kona_gl_convolution_parameter(SGIKonaState *s, int i,
                                              uint32_t selector,
                                              const uint32_t *args);
static void sgi_kona_gl_convolution_get(SGIKonaState *s, int i,
                                        uint32_t selector,
                                        const uint32_t *args);

/*
 * Write a selection pass's pending hit into its select feed, as the
 * record [tag, n, minimum depth, maximum depth, n names]: the names on
 * the stack, bottom first, and the depths as unsigned fractions of 2^32
 * - 1. Inferred from the library's reading of the feed: it takes records
 * of that shape, skips a leading 0 or 0x2bad2d00 as a pad word, merges a
 * record whose tag repeats the one before it, and copies the depths
 * unchanged, so the pipe must scale them. The model gives each record a
 * tag of its own, counting from 1 at each entry to selection mode and
 * passing over the pad values, writes no pads, and so leaves the library
 * nothing to merge: a hit per name-stack change, as OpenGL 1.2.1 section
 * 5.2 defines one. When a record does not fit in the rest of the target
 * nothing more is written and the completion reports an overflow (the
 * model's choice; the library then reports -1, as OpenGL requires).
 */
static void sgi_kona_gl_select_flush(SGIKonaState *s, int i)
{
    SGIKonaGLRenderMode *r = &s->state[i]->render;
    SGIKonaGLReadDMA *d = &s->gl_read_dma;
    uint32_t words = 4 + r->name_count;
    uint8_t record[(4 + SGI_KONA_GL_NAME_STACK_DEPTH) * 4];
    uint64_t io, pitch;
    uint32_t lines;

    if (!r->hit) {
        return;
    }
    r->hit = false;
    if (!d->active || d->refused || d->context != i ||
        d->render_mode != SGI_KONA_GL_SELECT ||
        !sgi_kona_context_owner(s, d->context, d->generation) ||
        !sgi_kona_gl_read_dma_descriptor(d, &io, &pitch, &lines)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: selection hit with no "
                      "select feed open for it; dropped\n");
        return;
    }
    if (d->overflow || words * 4 > d->descriptor[2] - d->written) {
        d->overflow = true;
        return;
    }
    stl_be_p(record, r->next_tag);
    stl_be_p(record + 4, r->name_count);
    stl_be_p(record + 8, r->hit_min);
    stl_be_p(record + 12, r->hit_max);
    for (uint32_t k = 0; k < r->name_count; k++) {
        stl_be_p(record + 16 + 4 * k, r->names[k]);
    }
    for (bool validate = true;; validate = false) {
        for (uint32_t done = 0; done < words * 4;) {
            uint32_t at = d->written + done;
            uint32_t line = at / d->descriptor[3];
            uint32_t column = at % d->descriptor[3];
            uint32_t len = MIN(words * 4 - done, d->descriptor[3] - column);

            if (!sgi_kona_device_write(s, io + line * pitch + column,
                                       validate ? NULL : record + done,
                                       len, validate)) {
                qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: selection record "
                              "target not mapped; record dropped\n");
                return;
            }
            done += len;
        }
        if (!validate) {
            break;
        }
    }
    d->written += words * 4;
    do {
        r->next_tag++;
    } while (!r->next_tag || r->next_tag == 0x2bad2d00);
}

/*
 * glRenderMode's 0x19. The library sends the new mode before it acts on
 * it: entering feedback or selection, it opens a select feed (0x1f5) for
 * the pass's records; leaving, it finishes the feed (0x1f6) and waits for
 * the pipe to complete it (sgi_kona_gl_read_dma_finish()). Nothing is
 * drawn in either mode (sgi_kona_gl_sink()). Leaving selection writes
 * the last hit out; the model writes no feedback records, so a feedback
 * pass reports no values. Each mode change empties the name stack
 * (inferred: the library sends glInitNames before it names anything).
 */
static void sgi_kona_gl_render_mode(SGIKonaState *s, int i, uint32_t mode)
{
    if (mode != SGI_KONA_GL_RENDER && mode != SGI_KONA_GL_FEEDBACK &&
        mode != SGI_KONA_GL_SELECT) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: render mode 0x%" PRIx32
                      " is not a mode; ignored\n", mode);
        return;
    }
    if (mode == SGI_KONA_GL_FEEDBACK) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: render mode 0x%" PRIx32
                      ": feedback records are not modelled; the pass returns"
                      " none\n", mode);
    }
    if (i != SGI_KONA_CONTEXT_NONE) {
        SGIKonaGLRenderMode *r = &s->state[i]->render;

        if (r->mode == SGI_KONA_GL_SELECT) {
            sgi_kona_gl_select_flush(s, i);
        }
        r->mode = mode;
        r->name_count = 0;
        r->hit = false;
        r->next_tag = 1;
    }
}

/*
 * The name stack: 0x10c empties it, 0x10d replaces the top name, 0xc2
 * pops and 0xc3 pushes. Outside selection mode the commands do nothing
 * (OpenGL 1.2.1, section 5.2). In it, a push onto a full stack raises
 * GL_STACK_OVERFLOW, a pop or a load on an empty one GL_STACK_UNDERFLOW
 * or GL_INVALID_OPERATION, and the stack is left as it was. Inferred:
 * that the pipe, not the library, keeps the stack and raises these
 * errors, since the library sends every call on.
 */
static void sgi_kona_gl_name_stack(SGIKonaState *s, int i, uint32_t selector,
                                   uint32_t name)
{
    SGIKonaGLRenderMode *r;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    r = &s->state[i]->render;
    if (r->mode != SGI_KONA_GL_SELECT) {
        return;
    }
    /*
     * The pending hit goes out before this command's own error check, as
     * Mesa writes it, so a push or pop that fails still ends the hit.
     */
    sgi_kona_gl_select_flush(s, i);
    switch (selector) {
    case SGI_KONA_CMD_GL_INIT_NAMES:
        r->name_count = 0;
        break;
    case SGI_KONA_CMD_GL_LOAD_NAME:
        if (!r->name_count) {
            sgi_kona_gl_set_error(s, 0x502);
        } else {
            r->names[r->name_count - 1] = name;
        }
        break;
    case SGI_KONA_CMD_GL_POP_NAME:
        if (!r->name_count) {
            sgi_kona_gl_set_error(s, 0x504);
        } else {
            r->name_count--;
        }
        break;
    case SGI_KONA_CMD_GL_PUSH_NAME:
        if (r->name_count == SGI_KONA_GL_NAME_STACK_DEPTH) {
            sgi_kona_gl_set_error(s, 0x503);
        } else {
            r->names[r->name_count++] = name;
        }
        break;
    default:
        g_assert_not_reached();
    }
}

static void sgi_kona_gl_packet(void *opaque, unsigned port_id,
                               uint32_t selector, const uint32_t *args,
                               unsigned argc)
{
    SGIKonaState *s = opaque;
    int i = s->context_current;
    uint32_t words;
    bool retire;

    if (s->context_failed) {
        return;
    }
    sgi_kona_port_consumed(s, 1 + argc);
    trace_sgi_kona_kernel_command(selector, args[0], argc > 1 ? args[1] : 0,
                                  argc > 2 ? args[2] : 0, argc);
    retire = s->texture_retire_pending &&
             selector == SGI_KONA_CMD_GL_TEX_LOAD &&
             args[10] == s->texture_retire_handle;
    s->texture_retire_pending = false;
    /* The X server repainted display IDs since the window was last cut. */
    if (i != SGI_KONA_CONTEXT_NONE &&
        sgi_kona_gl_window_tests_id(&s->state[i]->window) &&
        s->state[i]->window.owned_generation != s->display_id_generation) {
        sgi_kona_gl_set_drawable(s, i);
    }
    if (selector != SGI_KONA_CMD_GL_TEX_BIND_2D) {
        s->texture_associate_pending = false;
        s->texture_bind_context = SGI_KONA_CONTEXT_NONE;
        s->texture_bind_generation = 0;
    }
    /*
     * Histogram, minmax, the three colour tables and the texture one. KONA
     * applies these stages itself; the shared GL context has no state for
     * them, so the enable stops here.
     */
    if (i != SGI_KONA_CONTEXT_NONE && !s->state[i]->gl.in_begin &&
        (selector == 0x24 || selector == 0x25)) {
        static const uint32_t caps[] = {
            0x8024, 0x802e, 0x80d0, 0x80d1, 0x80d2, 0x80bc,
        };

        for (unsigned k = 0; k < ARRAY_SIZE(caps); k++) {
            if (args[0] != caps[k]) {
                continue;
            }
            if (selector == 0x24) {
                s->state[i]->imaging.stages |= 1u << k;
            } else {
                s->state[i]->imaging.stages &= ~(1u << k);
            }
            trace_sgi_kona_gl_command(selector, i);
            return;
        }
    }
    if (i != SGI_KONA_CONTEXT_NONE &&
        sgi_kona_gl_multisample(s, i, selector, args)) {
        return;
    }
    /*
     * The convolution enables are ordinary 0x24 and 0x25 caps, and the
     * library sends no other sign of them: the pipe convolves image
     * transfers from this state (inferred, see the command grammar).
     */
    if (i != SGI_KONA_CONTEXT_NONE && (selector == 0x24 || selector == 0x25) &&
        args[0] >= 0x8010 && args[0] <= 0x8012 &&
        !s->state[i]->gl.in_begin) {
        uint32_t bit = 1u << (args[0] - 0x8010);

        if (selector == 0x24) {
            s->state[i]->imaging.enabled |= bit;
        } else {
            s->state[i]->imaging.enabled &= ~bit;
        }
        trace_sgi_kona_gl_command(selector, i);
        return;
    }
    switch (selector) {
    case SGI_KONA_CMD_GL_BUFFER_CONFIG:
    case SGI_KONA_CMD_GL_READ_CONFIG:
        trace_sgi_kona_gl_buffer_config(selector, args[0], args[1], args[2],
                                        args[SGI_KONA_GL_BUFFER_CONFIG_DOUBLE]);
        if (selector == SGI_KONA_CMD_GL_BUFFER_CONFIG) {
            sgi_kona_gl_buffer_config(s, args);
        } else if (i != SGI_KONA_CONTEXT_NONE) {
            sgi_kona_gl_config(&s->state[i]->pipe, true, args);
        }
        return;
    case SGI_KONA_CMD_GL_READ_DRAWABLE:
        if (i != SGI_KONA_CONTEXT_NONE) {
            s->state[i]->pipe.read_binding_unknown = true;
            s->state[i]->pipe.read_binding_word = args[0];
            qemu_log_mask(LOG_UNIMP, "sgi-kona: unknown read binding;"
                          " reads refused for this context\n");
        }
        return;
    case SGI_KONA_CMD_GL_TEX_TABLE:
        sgi_kona_gl_tex_table_open(s, args[0]);
        return;
    case SGI_KONA_CMD_GL_LINE_DISTANCE:
        sgi_kona_gl_line_distance(s);
        return;
    case SGI_KONA_CMD_GL_RENDER_MODE:
        sgi_kona_gl_render_mode(s, i, args[0]);
        trace_sgi_kona_gl_command(selector, i);
        return;
    case SGI_KONA_CMD_GL_INIT_NAMES:
    case SGI_KONA_CMD_GL_LOAD_NAME:
    case SGI_KONA_CMD_GL_POP_NAME:
    case SGI_KONA_CMD_GL_PUSH_NAME:
        sgi_kona_gl_name_stack(s, i, selector, args[0]);
        trace_sgi_kona_gl_command(selector, i);
        return;
    case SGI_KONA_CMD_GL_FEEDBACK_TYPE:
        if (args[0] < 0x600 || args[0] > 0x604) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: feedback type 0x%"
                          PRIx32 " is not a type; ignored\n", args[0]);
        } else if (i != SGI_KONA_CONTEXT_NONE) {
            s->state[i]->render.feedback_type = args[0];
        }
        trace_sgi_kona_gl_command(selector, i);
        return;
    case SGI_KONA_CMD_GL_PASS_THROUGH:
        /* Only a feedback record, which the model does not write. */
        trace_sgi_kona_gl_command(selector, i);
        return;
    case SGI_KONA_CMD_GL_SINGLE_COMPONENT:
        qemu_log_mask(LOG_UNIMP, "sgi-kona: single-component query 0x%"
                      PRIx32 " 0x%" PRIx32 " 0x%" PRIx32 " 0x%" PRIx32
                      " answered 0\n", args[0], args[1], args[2], args[3]);
        sgi_kona_gl_single_component(s);
        return;
    case SGI_KONA_CMD_GL_SET_ERROR:
        sgi_kona_gl_set_error(s, args[0]);
        return;
    case SGI_KONA_CMD_GL_GET_ERROR:
        sgi_kona_gl_get_error(s);
        return;
    case SGI_KONA_CMD_GL_GET_CURRENT:
    case SGI_KONA_CMD_GL_GET_RASTER:
        sgi_kona_gl_get_block(s, selector);
        return;
    case SGI_KONA_CMD_GL_FINISH:
        sgi_kona_gl_finish(s, args[0]);
        return;
    case SGI_KONA_CMD_GL_IGLOO_COLORNORMAL:
    case SGI_KONA_CMD_GL_IGLOO_LMC_COLOR:
        if (i != SGI_KONA_CONTEXT_NONE) {
            SGIKonaGLIrisRule *r = &s->state[i]->iris;

            if (selector == SGI_KONA_CMD_GL_IGLOO_COLORNORMAL) {
                r->colour_normal = args[0];
            } else {
                r->lmc = args[0];
            }
            trace_sgi_kona_gl_iris_rule(i, r->colour_normal, r->lmc);
        }
        return;
    case SGI_KONA_CMD_GL_IGLOO_SWAPTMESH:
        /*
         * IRIS GL's swaptmesh, sent as the caller makes it. The
         * distributor exchanges its two retained strip vertices and
         * flips the strip's parity; outside a triangle strip the token
         * does nothing (field values established, meanings inferred:
         * the KONA geometry datasheet's vertex history).
         */
        if (i != SGI_KONA_CONTEXT_NONE) {
            SGIGLSink sink;

            sgi_kona_gl_sink(s, &sink);
            sgi_kona_gl_run(s, i, &sink,
                            &(SGIGLCmd) { SGI_GL_OP_EXCHANGE_TMESH, 0, args });
        }
        trace_sgi_kona_gl_command(selector, i);
        return;
    case SGI_KONA_CMD_GL_VIEWPORT:
    case SGI_KONA_CMD_GL_SCISSOR:
    case SGI_KONA_CMD_GL_DISABLE:
    case SGI_KONA_CMD_GL_CLEAR_COLOR:
    case SGI_KONA_CMD_GL_CLEAR:
    case SGI_KONA_CMD_GL_SHADE_MODEL:
    case SGI_KONA_CMD_GL_COLOR3:
    case SGI_KONA_CMD_GL_BEGIN_FIRST ... SGI_KONA_CMD_GL_BEGIN_LAST:
    case SGI_KONA_CMD_GL_VERTEX2:
    case SGI_KONA_CMD_GL_END:
    case SGI_KONA_CMD_GL_FLUSH:
        sgi_kona_gl_command(s, selector, args, argc);
        return;
    case SGI_KONA_CMD_GL_ARRAY_STATE:
        if (i != SGI_KONA_CONTEXT_NONE) {
            s->state[i]->pipe.array_state = args[0];
            s->state[i]->pipe.array_state_valid = true;
        }
        return;
    case SGI_KONA_CMD_GL_ARRAY_BEGIN:
    case SGI_KONA_CMD_GL_ARRAY_END:
        if (args[0]) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: glDrawArrays bracket"
                          " 0x%" PRIx32 " with word 0x%" PRIx32 ", not 0\n",
                          selector, args[0]);
        }
        if (i != SGI_KONA_CONTEXT_NONE) {
            s->state[i]->pipe.array_open =
                selector == SGI_KONA_CMD_GL_ARRAY_BEGIN;
        }
        return;
    case SGI_KONA_CMD_GL_ARRAY_BULK:
    case SGI_KONA_CMD_GL_ARRAY_SHORT_FIRST ... SGI_KONA_CMD_GL_ARRAY_SHORT_LAST:
        sgi_kona_gl_array_run(s, i, selector, args);
        return;
    case SGI_KONA_CMD_GL_POLYGON_STIPPLE:
        /*
         * The 32 rows of the pattern follow, one word each: inferred from
         * the 32-by-32 pattern and the default unpack state, under which
         * a row is four bytes and starts on a word.
         */
        sgi_kona_gl_data_open(s, selector, SGI_KONA_GL_STIPPLE_ROWS);
        return;
    case SGI_KONA_CMD_GL_POP_LIGHTS:
        sgi_kona_gl_pop_lights_open(s, args[0]);
        return;
    case SGI_KONA_CMD_GL_CONVOLUTION_1D:
    case SGI_KONA_CMD_GL_CONVOLUTION_2D:
    case SGI_KONA_CMD_GL_SEPARABLE:
        sgi_kona_gl_convolution_open(s, i, selector, args);
        return;
    case SGI_KONA_CMD_GL_COPY_CONVOLUTION_1D:
    case SGI_KONA_CMD_GL_COPY_CONVOLUTION_2D:
        sgi_kona_gl_convolution_copy(s, i, selector, args);
        return;
    case SGI_KONA_CMD_GL_CONVOLUTION_PARAMETER:
    case SGI_KONA_CMD_GL_CONVOLUTION_PARAMETERS:
        sgi_kona_gl_convolution_parameter(s, i, selector, args);
        return;
    case SGI_KONA_CMD_GL_GET_CONVOLUTION:
    case SGI_KONA_CMD_GL_GET_SEPARABLE:
        sgi_kona_gl_convolution_get(s, i, selector, args);
        return;
    case SGI_KONA_CMD_GL_COLOUR_TABLE:
        sgi_kona_gl_colour_table_open(s, i, args);
        return;
    case SGI_KONA_CMD_GL_HISTOGRAM:
    case SGI_KONA_CMD_GL_RESET_HISTOGRAM:
    case SGI_KONA_CMD_GL_GET_HISTOGRAM:
    case SGI_KONA_CMD_GL_GET_HISTOGRAM_PARAMETER:
    case SGI_KONA_CMD_GL_MINMAX:
    case SGI_KONA_CMD_GL_RESET_MINMAX:
    case SGI_KONA_CMD_GL_GET_MINMAX:
    case SGI_KONA_CMD_GL_GET_MINMAX_PARAMETER:
        sgi_kona_gl_histogram(s, i, selector, args);
        return;
    case SGI_KONA_CMD_GL_COLOUR_TABLE_PARAMETERS:
        sgi_kona_gl_colour_table_parameters(s, i, args);
        return;
    case SGI_KONA_CMD_GL_GET_COLOUR_TABLE:
        sgi_kona_gl_colour_table_get(s, i, args);
        return;
    case SGI_KONA_CMD_GL_GET_COLOUR_TABLE_PARAMETER:
        sgi_kona_gl_colour_table_get_parameter(s, i, args);
        return;
    /*
     * The library sends these ids from its texture cache: 0x1c4 when it
     * gives a texture an entry, 0x1c5 when it frees one. Freeing an entry
     * and taking it again re-binds the texture without reloading it, so
     * neither discards the image; a load header right after 0x1c4 retires
     * it instead, when the id has gone to a new texture. So an image whose
     * entry is freed for good, by glDeleteTextures for example, stays until
     * its id is taken for a new load or the pipe is reset. The model's
     * texture budget bounds what such images hold, and a load refused for
     * that budget is logged.
     */
    case SGI_KONA_CMD_GL_TEX_PREFIX:
        s->texture_state_used = true;
        s->texture_retire_pending = true;
        s->texture_retire_handle = args[0];
        sgi_kona_texture_association_forget(s, args[0]);
        if (s->texture_upload.count &&
            s->texture_upload.header[10] == args[0]) {
            sgi_kona_texture_upload_clear(s);
        }
        return;
    case SGI_KONA_CMD_GL_TEX_CLEANUP_PREFIX:
        s->texture_state_used = true;
        sgi_kona_texture_association_forget(s, args[0]);
        if (s->texture_upload.count &&
            s->texture_upload.header[10] == args[0]) {
            sgi_kona_texture_upload_clear(s);
        }
        return;
    case SGI_KONA_CMD_GL_TEX_DETAIL:
        if (sgi_kona_texture_detail_bind(&s->textures, args)) {
            s->texture_state_used = true;
        } else {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: native detail texture binding"
                          " refused; prior binding retained\n");
        }
        return;
    case SGI_KONA_CMD_GL_TEX_BIND_ID:
        s->texture_state_used = true;
        if (i != SGI_KONA_CONTEXT_NONE) {
            SGIKonaGLPipe *p = &s->state[i]->pipe;

            p->texture_selector = args[0];
            p->texture_selector_valid = args[0] < 2;
        }
        s->texture_bind[0] = args[0];
        sgi_kona_gl_texture_bind(s);
        if (i != SGI_KONA_CONTEXT_NONE &&
            s->state[i]->pipe.texture_rebind_refused &&
            sgi_kona_texture_bound_ordinary_2d(s, args[0])) {
            s->state[i]->pipe.texture_rebind_refused = false;
        }
        return;
    case SGI_KONA_CMD_GL_TEX_BIND:
    case SGI_KONA_CMD_GL_TEX_BIND_LINEAR: {
        uint32_t handle = args[0];

        s->texture_state_used = true;
        memcpy(s->texture_bind, args, sizeof(s->texture_bind));
        s->texture_bind_context = i;
        s->texture_bind_generation = sgi_kona_context_generation(s, i);
        if (selector == SGI_KONA_CMD_GL_TEX_BIND_LINEAR &&
            (args[1] >> 28) == 2 && i != SGI_KONA_CONTEXT_NONE) {
            SGIKonaGLPipe *p = &s->state[i]->pipe;

            if (handle == UINT32_MAX) {
                /*
                 * The record that follows locates the image; until it
                 * does, draws are refused. It is reported there if it
                 * cannot.
                 */
                p->texture_rebind_refused = true;
            } else if (p->texture_selector_valid) {
                s->texture_associate_pending = true;
                s->texture_associate_context = i;
                s->texture_associate_generation =
                    sgi_kona_context_generation(s, i);
                s->texture_associate_slot = p->texture_selector;
            }
        }
        s->texture_bind_valid = true;
        s->texture_record_valid = false;
        s->texture_record_token = 0;
        sgi_kona_gl_texture_bind(s);
        return;
    }
    case SGI_KONA_CMD_GL_TEX_BIND_2D:
    case SGI_KONA_CMD_GL_TEX_BIND_VOLUME:
    case SGI_KONA_CMD_GL_TEX_BIND_VOLUME_SHORT: {
        uint32_t table[16], found = UINT32_MAX;
        bool by_table = false, resolved;

        s->texture_state_used = true;
        memset(s->texture_record, 0, sizeof(s->texture_record));
        memcpy(s->texture_record, args, argc * sizeof(*args));
        s->texture_record_token = selector;
        s->texture_record_valid = true;
        memset(table, 0xff, sizeof(table));
        if (selector == SGI_KONA_CMD_GL_TEX_BIND_2D) {
            memcpy(table, args + 6, sizeof(table));
        } else if (selector == SGI_KONA_CMD_GL_TEX_BIND_VOLUME) {
            memcpy(table, args + 4, 10 * sizeof(*args));
        } else {
            memcpy(table, args + 2, 2 * sizeof(*args));
        }
        if (s->texture_bind_valid && s->texture_bind[0] != UINT32_MAX) {
            /* Bit 28 of the bind's third word: the first level's bank. */
            if (!sgi_kona_texture_native_table(&s->textures,
                                               s->texture_bind[0], table) ||
                (selector == SGI_KONA_CMD_GL_TEX_BIND_2D &&
                 !sgi_kona_texture_native_bank(&s->textures, s->texture_bind[0],
                                               (s->texture_bind[2] >> 28) &
                                               1))) {
                s->texture_record_valid = false;
                qemu_log_mask(LOG_UNIMP,
                              "sgi-kona: native texture table refused for"
                              " model storage budget\n");
            }
        } else if (s->texture_bind_valid) {
            bool volume = selector != SGI_KONA_CMD_GL_TEX_BIND_2D;
            bool bank_one = (s->texture_bind[2] >> 28) & 1;
            SGIKonaGLPipe *p = i != SGI_KONA_CONTEXT_NONE ?
                               &s->state[i]->pipe : NULL;

            /*
             * A bind without its cache number draws before the 0x178 that
             * names it, so the record must locate the image: take the one
             * texture an earlier complete bind gave this level table, an
             * ordinary texture's with its first level in the same bank.
             * Two textures may share a table word list in opposite banks,
             * as mirrors' do. pfCity rebinds its volume this way too, with
             * 0x17c.
             */
            by_table = sgi_kona_texture_find_table(&s->textures, table,
                                                   volume, bank_one, &found);
            /*
             * The retained association names a texture only while its
             * own stored table is this record's; anything else is a
             * different texture, and taking it would rewrite that
             * texture's table and size with this record's.
             */
            if (!by_table && !volume && p && p->texture_selector_valid &&
                (p->texture_assoc_valid & (1u << p->texture_selector))) {
                found = p->texture_assoc_handle[p->texture_selector];
                by_table = sgi_kona_texture_table_matches(&s->textures, found,
                                                          table, false,
                                                          bank_one);
            }
            if (by_table) {
                s->texture_bind[0] = found;
            }
        }
        sgi_kona_gl_texture_bind(s);
        resolved = by_table ?
            s->textures.bound && s->textures.bound_handle == found :
            sgi_kona_texture_bound_ordinary_2d(s, s->texture_bind[0]);
        /* A volume found by its record replaces a refused 2D rebind too. */
        if ((selector == SGI_KONA_CMD_GL_TEX_BIND_2D || by_table) &&
            i != SGI_KONA_CONTEXT_NONE && i == s->texture_bind_context &&
            s->state[i]->pipe.texture_rebind_refused) {
            if (resolved) {
                s->state[i]->pipe.texture_rebind_refused = false;
            } else if (s->texture_bind[0] == UINT32_MAX) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-kona: texture rebind without an "
                              "associated selector; draw refused\n");
            }
        }
        s->texture_bind_context = SGI_KONA_CONTEXT_NONE;
        s->texture_bind_generation = 0;
        if (selector == SGI_KONA_CMD_GL_TEX_BIND_2D &&
            s->texture_associate_pending) {
            uint32_t handle = s->texture_bind[0];
            SGIKonaTextureObject *object = sgi_kona_texture_lookup(
                &s->textures, s->textures.owner, handle);

            if (i == s->texture_associate_context &&
                s->texture_record_valid && s->textures.bound &&
                s->textures.bound_handle == handle && object &&
                object->native_count) {
                SGIKonaGLPipe *p = &s->state[i]->pipe;
                unsigned slot = s->texture_associate_slot;

                p->texture_assoc_handle[slot] = handle;
                p->texture_assoc_valid |= 1u << slot;
            }
            s->texture_associate_pending = false;
        }
        return;
    }
    case SGI_KONA_CMD_GL_TEX_POST_SCALE:
    case SGI_KONA_CMD_GL_TEX_SCALE_BIAS:
        if (args[0] < 2) {
            s->texture_state_used = true;
            memcpy(s->texture_transform + args[0] * 4, args, 4 * sizeof(*args));
            if (!args[0]) {
                sgi_kona_gl_texture_bind(s);
            }
        } else {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: unsupported post-filter unit %"
                          PRIu32 "\n", args[0]);
        }
        return;
    case SGI_KONA_CMD_GL_TEX_LOAD_ID:
        if (s->texture_upload.complete && !s->texture_upload.draw_pixels &&
            s->texture_upload.header[10] == args[0]) {
            sgi_kona_texture_upload_publish(s);
            sgi_kona_gl_texture_bind(s);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: texture publication without a complete"
                          " matching upload\n");
        }
        sgi_kona_texture_upload_clear(s);
        return;
    case SGI_KONA_CMD_GL_TEX_LOAD:
        sgi_kona_gl_image_clear(s);
        /*
         * One 0x176 finishes a texture level however many loads make it
         * up: the library's level producer sends it once, after a
         * volume's slice loop or the copies that fill a small level's
         * tile have each sent a load. A complete load of the same texture
         * followed by the next load is kept, not dropped.
         */
        if (s->texture_upload.complete && !s->texture_upload.draw_pixels &&
            s->texture_upload.header[10] == args[10]) {
            sgi_kona_texture_upload_publish(s);
        }
        sgi_kona_texture_upload_clear(s);
        if (retire) {
            sgi_kona_texture_delete(&s->textures, args[10]);
            sgi_kona_texture_association_forget(s, args[10]);
        }
        words = sgi_kona_gl_tex_load_words(s, i, args);
        if (s->texture_upload.count) {
            s->texture_upload_context = i;
        }
        if (words) {
            sgi_kona_gl_data_open(s, selector, words);
        }
        return;
    case SGI_KONA_CMD_GL_BITMAP_INLINE:
        words = sgi_kona_gl_bitmap_open(s, i, args);
        if (words) {
            sgi_kona_gl_data_open(s, selector, words);
        }
        return;
    case SGI_KONA_CMD_GL_BITMAP:
        words = sgi_kona_gl_raw_bitmap_open(s, i, args);
        if (words) {
            sgi_kona_gl_data_open(s, selector, words);
        }
        return;
    case SGI_KONA_CMD_GL_DRAW_PIXELS:
        words = sgi_kona_gl_draw_pixels_open(s, i, args);
        if (words) {
            sgi_kona_gl_data_open(s, selector, words);
        }
        return;
    case SGI_KONA_CMD_GL_PIXEL_ZOOM:
        if (i != SGI_KONA_CONTEXT_NONE) {
            if (s->state[i]->gl.in_begin) {
                sgi_kona_gl_set_error(s, 0x502);
                return;
            }
            memcpy(s->state[i]->pipe.pixel_zoom, args,
                   sizeof(s->state[i]->pipe.pixel_zoom));
            s->state[i]->pipe.pixel_zoom_unsupported =
                !isfinite(sgi_kona_gl_float(args[0])) ||
                !isfinite(sgi_kona_gl_float(args[1]));
            if (s->state[i]->pipe.pixel_zoom_unsupported) {
                qemu_log_mask(LOG_UNIMP, "sgi-kona: nonfinite pixel zoom;"
                              " DrawPixels refused\n");
            }
        }
        return;
    case SGI_KONA_CMD_GL_PIXEL_TRANSFER:
        if (i != SGI_KONA_CONTEXT_NONE) {
            sgi_kona_gl_pixel_transfer(s, i, args[0], args[1]);
        }
        return;
    case SGI_KONA_CMD_GL_PIXEL_MAP_F:
    case SGI_KONA_CMD_GL_PIXEL_MAP_UI:
    case SGI_KONA_CMD_GL_PIXEL_MAP_US:
        words = sgi_kona_gl_pixel_map_open(s, i, selector, args);
        if (words) {
            sgi_kona_gl_data_open(s, selector, words);
        }
        return;
    case SGI_KONA_CMD_GL_PIXEL_STORE:
        if (i != SGI_KONA_CONTEXT_NONE) {
            sgi_kona_gl_pixel_store(s, i, args[0], args[1]);
        }
        return;
    case SGI_KONA_CMD_GL_READ_BUFFER:
        if (i != SGI_KONA_CONTEXT_NONE) {
            s->state[i]->pipe.returns.read_buffer = args[0];
            s->state[i]->pipe.returns.read_buffer_unsupported =
                !sgi_kona_gl_read_enum(args[0]);
            if (s->state[i]->pipe.returns.read_buffer_unsupported ||
                ((args[0] == SGI_GL_BACK || args[0] == 0x402) &&
                 !s->state[i]->window.double_buffered)) {
                qemu_log_mask(LOG_UNIMP, "sgi-kona: unsupported read buffer"
                              " 0x%" PRIx32 "; reads refused\n", args[0]);
            }
        }
        return;
    case SGI_KONA_CMD_GL_COPY_OVERLAP:
        sgi_kona_gl_copy_overlap(s, args);
        return;
    case SGI_KONA_CMD_GL_COPY_CONVOLUTION:
        qemu_log_mask(LOG_UNIMP, "sgi-kona: CopyPixels convolution query"
                      " has an unknown response field; no reply\n");
        return;
    case SGI_KONA_CMD_GL_COPY_PIXELS:
        sgi_kona_gl_copy_pixels(s, args);
        return;
    case SGI_KONA_CMD_GL_READ_PIXELS:
        sgi_kona_gl_read_pixels(s, args);
        return;
    case SGI_KONA_CMD_GL_READ_TEXELS:
        sgi_kona_gl_read_texels(s, args);
        return;
    case SGI_KONA_CMD_GL_MODE_STATE:
        /*
         * Consumed whole so the readback after it frames. What the two
         * words select on the pipe is unknown.
         */
        trace_sgi_kona_command_unmodelled(selector, 2);
        return;
    case SGI_KONA_CMD_GL_GET:
        sgi_kona_gl_get(s, args[0]);
        return;
    case SGI_KONA_CMD_GL_PUSH_ATTRIB:
        sgi_kona_gl_push_attrib(s, args[0]);
        return;
    case SGI_KONA_CMD_GL_SPRITE_MODE:
    case SGI_KONA_CMD_GL_SPRITE_VECTOR: {
        bool mode = args[0] == SGI_GL_SPRITE_MODE_SGIX;
        float v[3] = {
            sgi_kona_gl_float(args[1]),
            selector == SGI_KONA_CMD_GL_SPRITE_VECTOR ?
                sgi_kona_gl_float(args[2]) : 0,
            selector == SGI_KONA_CMD_GL_SPRITE_VECTOR ?
                sgi_kona_gl_float(args[3]) : 0,
        };

        if (i == SGI_KONA_CONTEXT_NONE ||
            (selector == SGI_KONA_CMD_GL_SPRITE_MODE && !mode) ||
            !(mode ? sgi_gl_sprite_mode(&s->state[i]->gl, args[1]) :
              sgi_gl_sprite_parameter(&s->state[i]->gl, args[0], v))) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: glSpriteParameter of"
                          " 0x%" PRIx32 " with 0x%" PRIx32 " refused\n",
                          args[0], args[1]);
        }
        return;
    }
    case SGI_KONA_CMD_GL_POP_ATTRIB:
        sgi_kona_gl_pop_attrib(s);
        return;
    default:
        if (i != SGI_KONA_CONTEXT_NONE &&
            (sgi_kona_gl_transform(s, i, selector, args) ||
             sgi_kona_gl_raster_state(s, i, selector, args))) {
            return;
        }
        /* Framed and consumed; not carried out. */
        trace_sgi_kona_command_unmodelled(selector, argc);
        return;
    }
}

/*
 * EXT_convolution (established from the IRIX 6.5.30 client library, see
 * the command grammar). Each filter command is sent between 0x111 0x10
 * 0x40000000 and 0x111 0x10 0:
 *
 *   0xef  1D:  format, width, f, t, low, dma, rows
 *   0xf0  2D:  format, width, height, f, t, low, dma, rows
 *   0xf5  separable, twice: format, width, height, f, t, low, which, dma,
 *         rows; `which` 1 carries the row filter's `width` pixels, 2 the
 *         column filter's `height`
 *
 * `format` is the filter's internal format code (alpha 0, RGB 1, RGBA 2,
 * luminance 3, luminance-alpha 4, intensity 5); f and t are the native
 * format and type codes DrawPixels uses; `low` is the image address's low
 * two bits; dma 1 sends the image by kernel DMA, not FIFO words; rows 1
 * sends it row by row, each row's words on their own. The image's words
 * are framed as DrawPixels frames its own under the context's unpack
 * state (the library's own rule, and the same row test). The target is
 * implied by the token.
 *
 * The image is unpacked and expanded to RGBA as DrawPixels would, with no
 * pixel transfer, then scaled and biased by the filter's own parameters
 * (0xf9) and kept as its internal format keeps it (the OpenGL reference
 * pages for glConvolutionFilter2D and glSeparableFilter2D).
 */
static void sgi_kona_gl_filter_header(uint32_t token, const uint32_t *a,
                                      unsigned *filter, uint32_t h[8])
{
    memset(h, 0, 8 * sizeof(*h));
    if (token == SGI_KONA_CMD_GL_CONVOLUTION_1D) {
        *filter = 0;
        h[0] = a[1];
        h[1] = 1;
        memcpy(h + 2, a + 2, 5 * sizeof(*h));
    } else if (token == SGI_KONA_CMD_GL_CONVOLUTION_2D) {
        *filter = 1;
        memcpy(h, a + 1, 7 * sizeof(*h));
    } else {
        *filter = 2;
        h[0] = a[6] == 1 ? a[1] : a[2];
        h[1] = 1;
        memcpy(h + 2, a + 3, 3 * sizeof(*h));
        h[5] = a[7];
        h[6] = a[8];
    }
}

static const SGIKonaGLPipe sgi_kona_gl_unpack_defaults = {
    .unpack_alignment = 4,
};

static const SGIKonaGLPipe *sgi_kona_gl_unpack(SGIKonaState *s, int i)
{
    return i == SGI_KONA_CONTEXT_NONE ? &sgi_kona_gl_unpack_defaults :
                                        &s->state[i]->pipe;
}

/* The filter's kernel for component c at entry k, or false if c passes. */
static bool sgi_kona_gl_filter_kernel(const SGIKonaGLFilter *f, unsigned k,
                                      unsigned c, double *value)
{
    unsigned from;

    switch (f->format) {
    case 0:                     /* alpha */
        from = c == 3 ? 3 : 4;
        break;
    case 1:                     /* RGB */
        from = c < 3 ? c : 4;
        break;
    case 2:                     /* RGBA */
        from = c;
        break;
    case 3:                     /* luminance */
        from = c < 3 ? 0 : 4;
        break;
    case 4:                     /* luminance-alpha */
        from = c < 3 ? 0 : 3;
        break;
    default:                    /* intensity */
        from = 0;
        break;
    }
    if (from == 4) {
        return false;
    }
    *value = sgi_kona_gl_float(f->values[k][from]);
    return true;
}

/*
 * A filter's stored components as glGetConvolutionFilter returns them
 * before packing: luminance and intensity as red, the internal format's
 * other components as themselves, and the components it lacks as zero
 * (the OpenGL reference page for glGetConvolutionFilter).
 */
static void sgi_kona_gl_filter_rgba(const SGIKonaGLFilter *f, unsigned k,
                                    double rgba[4])
{
    double v[4];

    for (unsigned c = 0; c < 4; c++) {
        v[c] = sgi_kona_gl_float(f->values[k][c]);
    }
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
    switch (f->format) {
    case 0:
        rgba[3] = v[3];
        break;
    case 1:
    case 2:
        memcpy(rgba, v, (f->format == 2 ? 4 : 3) * sizeof(*v));
        break;
    case 4:
        rgba[3] = v[3];
        /* fall through */
    default:
        rgba[0] = v[0];
        break;
    }
}

/* Store one filter entry: its RGBA after the filter's scale and bias. */
static bool sgi_kona_gl_filter_store(SGIKonaGLFilter *f, unsigned k,
                                     const double rgba[4])
{
    for (unsigned c = 0; c < 4; c++) {
        float value = rgba[c] * sgi_kona_gl_float(f->scale[c]) +
                      sgi_kona_gl_float(f->bias[c]);

        if (!isfinite(value)) {
            return false;
        }
        memcpy(&f->values[k][c], &value, sizeof(value));
    }
    return true;
}

/*
 * The pending filter's image from `words`, framed as the PIO staging of
 * sgi_kona_gl_pixels_pio_layout() orders it. Returns false, logged, when
 * the image cannot be read; the filter is then left undefined.
 */
static void sgi_kona_gl_filter_load(SGIKonaState *s, int i,
                                    const uint32_t *words, size_t nwords)
{
    SGIKonaGLImaging *im = &s->state[i]->imaging;
    const SGIKonaGLPipe *p = &s->state[i]->pipe;
    SGIKonaGLFilter *f;
    SGIKonaGLPixelLayout layout;
    uint32_t h[8], which = 0;
    unsigned filter, base = 0;

    sgi_kona_gl_filter_header(im->pending_token, im->pending, &filter, h);
    f = &im->filters[filter];
    if (im->pending_token == SGI_KONA_CMD_GL_SEPARABLE) {
        which = im->pending[6];
        base = which == 2 ? SGI_KONA_GL_FILTER_MAX : 0;
    }
    h[5] = 0;
    if (!sgi_kona_gl_pixels_pio_layout(h, p->unpack_row_length,
                                        p->unpack_skip_rows,
                                        p->unpack_skip_pixels,
                                        p->unpack_alignment, &layout) ||
        !layout.pixel_bytes || h[2] > 10 ||
        layout.payload_words > nwords) {
        goto refused;
    }
    for (uint32_t row = 0; row < h[1]; row++) {
        for (uint32_t x = 0; x < h[0]; x++) {
            uint32_t source[SGI_KONA_GL_PIXEL_WORDS_MAX] = { 0 };
            double rgba[4];

            for (uint32_t k = 0; k < layout.pixel_bytes; k++) {
                uint8_t byte;

                if (!sgi_kona_gl_pixels_pio_byte(&layout, words, nwords, row,
                        (uint64_t)x * layout.pixel_bytes + k, &byte)) {
                    goto refused;
                }
                source[k / 4] |= (uint32_t)byte << (24 - (k % 4) * 8);
            }
            if (!sgi_kona_gl_pixel_rgba_double(h[2], h[3], source,
                                                p->unpack_swap_bytes, rgba) ||
                !sgi_kona_gl_filter_store(f, base + x + row * h[0], rgba)) {
                goto refused;
            }
        }
    }
    f->format = im->pending[0];
    if (filter == 2) {
        f->width = im->pending[1];
        f->height = im->pending[2];
    } else {
        f->width = h[0];
        f->height = h[1];
    }
    trace_sgi_kona_gl_filter(im->pending_token, f->format, f->width,
                             f->height, which);
    im->pending_token = 0;
    return;
refused:
    qemu_log_mask(LOG_UNIMP, "sgi-kona: convolution filter image of format"
                  " %" PRIu32 " type %" PRIu32 " could not be read; filter"
                  " left undefined\n", h[2], h[3]);
    f->width = f->height = 0;
    im->pending_token = 0;
}

static void sgi_kona_gl_convolution_open(SGIKonaState *s, int i,
                                         uint32_t selector,
                                         const uint32_t *args)
{
    const SGIKonaGLPipe *p = sgi_kona_gl_unpack(s, i);
    uint32_t h[8];
    SGIKonaGLPixelLayout layout;
    unsigned filter, argc;
    uint64_t words = 0;
    bool framed;

    argc = selector == SGI_KONA_CMD_GL_CONVOLUTION_1D ? 7 :
           selector == SGI_KONA_CMD_GL_CONVOLUTION_2D ? 8 : 9;
    sgi_kona_gl_filter_header(selector, args, &filter, h);
    if (h[5] > 1 || h[6] > 1 ||
        (selector == SGI_KONA_CMD_GL_SEPARABLE &&
         args[6] != 1 && args[6] != 2)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: convolution filter header"
                      " 0x%" PRIx32 " has unknown flags; its image will not"
                      " frame\n", selector);
        return;
    }
    if (i != SGI_KONA_CONTEXT_NONE) {
        SGIKonaGLFilter *f = &s->state[i]->imaging.filters[filter];

        /* A size of zero defines an empty filter, and no image follows. */
        if (!h[0] || !h[1] ||
            (selector == SGI_KONA_CMD_GL_SEPARABLE && (!args[1] || !args[2]))) {
            f->width = f->height = 0;
        }
    }
    if (!h[0] || !h[1]) {
        return;
    }
    framed = !h[5] &&
             sgi_kona_gl_pixels_pio_layout(h, p->unpack_row_length,
                                           p->unpack_skip_rows,
                                           p->unpack_skip_pixels,
                                           p->unpack_alignment, &layout);
    if (framed) {
        words = layout.payload_words;
    } else if (!h[5]) {
        /*
         * The unpack state the model holds does not give the library's row
         * test the same answer: frame the image as one span from its low
         * bits on, which is what the library sends when its rows are
         * contiguous.
         */
        uint32_t bytes = sgi_kona_gl_native_pixel_bytes(h[2], h[3]);

        if (!bytes) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: convolution filter of"
                          " format %u type %u has no known size; its words"
                          " will not frame\n", h[2], h[3]);
            return;
        }
        words = (h[4] + (uint64_t)h[0] * h[1] * bytes + 3) / 4;
        qemu_log_mask(LOG_UNIMP, "sgi-kona: convolution filter rows do not"
                      " match the unpack state; framed as one span\n");
    }
    if (i != SGI_KONA_CONTEXT_NONE && (h[5] || framed) &&
        h[0] <= SGI_KONA_GL_FILTER_MAX && h[1] <= SGI_KONA_GL_FILTER_MAX &&
        args[0] <= 5) {
        SGIKonaGLImaging *im = &s->state[i]->imaging;

        im->pending_token = selector;
        memset(im->pending, 0, sizeof(im->pending));
        memcpy(im->pending, args, argc * sizeof(*args));
    } else if (i != SGI_KONA_CONTEXT_NONE) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: convolution filter %" PRIu32
                      " by %" PRIu32 " of format code %" PRIu32 " not kept\n",
                      h[0], h[1], args[0]);
        s->state[i]->imaging.filters[filter].width = 0;
        s->state[i]->imaging.filters[filter].height = 0;
    }
    if (words > UINT32_MAX) {
        return;
    }
    if (words) {
        sgi_kona_gl_data_open(s, selector, words);
    }
}

/* The FIFO words of a filter image, at the end of its data run. */
static void sgi_kona_gl_convolution_done(SGIKonaState *s)
{
    int i = s->context_current;

    if (i == SGI_KONA_CONTEXT_NONE ||
        s->state[i]->imaging.pending_token != s->gl_data_token) {
        return;
    }
    if (s->gl_data_have > SGI_KONA_GL_DATA_MAX) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: convolution filter image of %"
                      PRIu32 " words exceeds the model's staging\n",
                      s->gl_data_have);
        s->state[i]->imaging.pending_token = 0;
        return;
    }
    sgi_kona_gl_filter_load(s, i, s->gl_data, s->gl_data_have);
}

/*
 * SGI_color_table (established from the IRIX 6.5.30 client library, see
 * the command grammar). A table's image is sent between 0x111 8
 * 0x40000000 and 0x111 8 0:
 *
 *   0xe8  target, internal format, width, width - 1 as a float, f, t,
 *         low, dma, rows
 *
 * The targets are 0x80d0 to 0x80d2, their proxies 0x80d3 to 0x80d5, and
 * the texture colour table 0x80bc with its proxy 0x80bd. f, t, low, dma
 * and rows are DrawPixels' own, as in the convolution filter commands; the
 * image is one row of `width` pixels. A proxy's image is never sent, and
 * neither is a table the library finds it cannot hold: it sends width 0.
 *
 *   0xeb  target, pname, r, g, b, a (floats): COLOR_TABLE_SCALE_SGI 0x80d6
 *         or COLOR_TABLE_BIAS_SGI 0x80d7
 *   0xea  target, format, first entry, count of at most 512: answered on
 *         the return page at byte 8 with `count` entries of four floats,
 *         R, G, B and A, which the library packs itself
 *   0xec  target, pname: answered at byte 8 with four floats for the scale
 *         and bias, one word for anything else
 *
 * The image is unpacked and expanded to RGBA as DrawPixels would, with no
 * pixel transfer, scaled and biased by the table's parameters, clamped
 * to [0, 1] and kept as its internal format keeps it (SGI_color_table).
 */
static int sgi_kona_gl_table_index(uint32_t target, bool *proxy)
{
    *proxy = (target >= 0x80d3 && target <= 0x80d5) || target == 0x80bd;
    switch (target) {
    case 0x80d0 ... 0x80d2:
        return target - 0x80d0;
    case 0x80d3 ... 0x80d5:
        return target - 0x80d3;
    case 0x80bc:
    case 0x80bd:
        return 3;
    default:
        return -1;
    }
}

/* A 0xe8 header as DrawPixels' layout takes it: one row of width pixels. */
static void sgi_kona_gl_table_header(const uint32_t *a, uint32_t h[8])
{
    memset(h, 0, 8 * sizeof(*h));
    h[0] = a[2];
    h[1] = 1;
    memcpy(h + 2, a + 4, 5 * sizeof(*h));
}

static void sgi_kona_gl_table_refuse(SGIKonaGLColourTables *tb,
                                     const char *why)
{
    qemu_log_mask(LOG_UNIMP, "sgi-kona: colour table 0x%" PRIx32 " of"
                  " format 0x%" PRIx32 " and width %" PRIu32 " %s; table"
                  " left empty\n", tb->pending[0], tb->pending[1],
                  tb->pending[2], why);
    tb->pending[0] = 0;
    tb->staged_have = 0;
}

/* The pending table's image from `words`, in PIO order. */
static void sgi_kona_gl_table_load(SGIKonaState *s, int i,
                                   const uint32_t *words, size_t nwords)
{
    SGIKonaGLColourTables *tb = &s->state[i]->tables;
    const SGIKonaGLPipe *p = &s->state[i]->pipe;
    SGIKonaGLPixelLayout layout;
    SGIKonaGLColourTable *t;
    uint32_t h[8];
    bool proxy;
    int n = sgi_kona_gl_table_index(tb->pending[0], &proxy);

    t = &tb->table[n];
    sgi_kona_gl_table_header(tb->pending, h);
    h[5] = 0;
    t->width = 0;
    if (!sgi_kona_gl_pixels_pio_layout(h, p->unpack_row_length,
                                        p->unpack_skip_rows,
                                        p->unpack_skip_pixels,
                                        p->unpack_alignment, &layout) ||
        !layout.pixel_bytes || h[2] > 10 || layout.payload_words > nwords) {
        sgi_kona_gl_table_refuse(tb, "could not be read");
        return;
    }
    for (uint32_t x = 0; x < h[0]; x++) {
        uint32_t source[SGI_KONA_GL_PIXEL_WORDS_MAX] = { 0 };
        double rgba[4];

        for (uint32_t k = 0; k < layout.pixel_bytes; k++) {
            uint8_t byte;

            if (!sgi_kona_gl_pixels_pio_byte(&layout, words, nwords, 0,
                    (uint64_t)x * layout.pixel_bytes + k, &byte)) {
                sgi_kona_gl_table_refuse(tb, "could not be read");
                return;
            }
            source[k / 4] |= (uint32_t)byte << (24 - (k % 4) * 8);
        }
        if (!sgi_kona_gl_pixel_rgba_double(h[2], h[3], source,
                                            p->unpack_swap_bytes, rgba)) {
            sgi_kona_gl_table_refuse(tb, "could not be read");
            return;
        }
        for (unsigned c = 0; c < 4; c++) {
            double value = rgba[c] * sgi_kona_gl_float(t->scale[c]) +
                           sgi_kona_gl_float(t->bias[c]);
            float stored;

            if (!isfinite(value)) {
                sgi_kona_gl_table_refuse(tb, "scales to a non-finite value");
                return;
            }
            stored = CLAMP(value, 0.0, 1.0);
            memcpy(&t->values[x][c], &stored, sizeof(stored));
        }
    }
    t->format = tb->pending[1];
    t->width = h[0];
    trace_sgi_kona_gl_colour_table(tb->pending[0], t->format, t->width);
    tb->pending[0] = 0;
    tb->staged_have = 0;
}

static void sgi_kona_gl_colour_table_open(SGIKonaState *s, int i,
                                          const uint32_t *args)
{
    const SGIKonaGLPipe *p = sgi_kona_gl_unpack(s, i);
    SGIKonaGLPixelLayout layout;
    SGIKonaGLColourTables *tb = NULL;
    uint32_t h[8];
    uint64_t words = 0;
    bool proxy, framed;
    int n = sgi_kona_gl_table_index(args[0], &proxy);

    sgi_kona_gl_table_header(args, h);
    if (n < 0 || h[5] > 1 || h[6] > 1) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: colour table header for"
                      " target 0x%" PRIx32 " has unknown fields; its image"
                      " will not frame\n", args[0]);
        return;
    }
    if (i != SGI_KONA_CONTEXT_NONE) {
        tb = &s->state[i]->tables;
        tb->pending[0] = 0;
        tb->staged_have = 0;
    }
    if (proxy) {
        /* A proxy only records what a table so defined would be. */
        if (tb) {
            tb->proxy_format[n] = args[2] ? args[1] : 0;
            tb->proxy_width[n] = args[2];
        }
        return;
    }
    if (tb && !args[2]) {
        tb->table[n].width = 0;
        tb->table[n].format = args[1];
    }
    if (!args[2]) {
        return;
    }
    framed = !h[5] &&
             sgi_kona_gl_pixels_pio_layout(h, p->unpack_row_length,
                                           p->unpack_skip_rows,
                                           p->unpack_skip_pixels,
                                           p->unpack_alignment, &layout);
    if (framed) {
        words = layout.payload_words;
    } else if (!h[5]) {
        uint32_t bytes = sgi_kona_gl_native_pixel_bytes(h[2], h[3]);

        if (!bytes) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: colour table of"
                          " format %u type %u has no known size; its words"
                          " will not frame\n", h[2], h[3]);
            return;
        }
        words = (h[4] + (uint64_t)h[0] * bytes + 3) / 4;
    }
    if (tb) {
        memcpy(tb->pending, args, sizeof(tb->pending));
        if (args[2] > SGI_KONA_GL_TABLE_MAX ||
            !sgi_kona_gl_table_base(args[1])) {
            sgi_kona_gl_table_refuse(tb, "is not supported");
            tb->table[n].width = 0;
        } else if (!h[5] && (!framed || words > SGI_KONA_GL_TABLE_WORDS)) {
            sgi_kona_gl_table_refuse(tb, "does not match the unpack state");
            tb->table[n].width = 0;
        }
    }
    if (words && words <= UINT32_MAX) {
        sgi_kona_gl_data_open(s, SGI_KONA_CMD_GL_COLOUR_TABLE, words);
    }
}

/* One FIFO word of a table's image. */
static void sgi_kona_gl_colour_table_word(SGIKonaState *s, uint32_t word)
{
    int i = s->context_current;
    SGIKonaGLColourTables *tb;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    tb = &s->state[i]->tables;
    if (tb->pending[0] && tb->staged_have < SGI_KONA_GL_TABLE_WORDS) {
        tb->staged[tb->staged_have++] = word;
    }
}

/* The FIFO words of a table's image, at the end of its data run. */
static void sgi_kona_gl_colour_table_done(SGIKonaState *s)
{
    int i = s->context_current;
    SGIKonaGLColourTables *tb;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    tb = &s->state[i]->tables;
    if (tb->pending[0]) {
        sgi_kona_gl_table_load(s, i, tb->staged, tb->staged_have);
    }
}

/* 0xeb target pname r g b a: a table's scale or bias. */
static void sgi_kona_gl_colour_table_parameters(SGIKonaState *s, int i,
                                                const uint32_t *args)
{
    bool proxy;
    int n = sgi_kona_gl_table_index(args[0], &proxy);
    SGIKonaGLColourTable *t;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    if (n < 0 || proxy || (args[1] != 0x80d6 && args[1] != 0x80d7)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: colour table parameter"
                      " 0x%" PRIx32 " of target 0x%" PRIx32 " ignored\n",
                      args[1], args[0]);
        return;
    }
    t = &s->state[i]->tables.table[n];
    memcpy(args[1] == 0x80d6 ? t->scale : t->bias, args + 2,
           4 * sizeof(*args));
}

/*
 * 0xea target format first count: entries first to first + count - 1 as
 * four floats each, R, G, B and A, with luminance and intensity as red
 * and the components the table lacks as zero (SGI_color_table's query
 * rule); the library reads them as RGBA floats and packs them itself.
 */
static void sgi_kona_gl_colour_table_get(SGIKonaState *s, int i,
                                         const uint32_t *args)
{
    const SGIKonaGLColourTable *t;
    uint32_t base, first = args[2], count = args[3];
    g_autofree uint8_t *data = NULL;
    bool proxy;
    int n = sgi_kona_gl_table_index(args[0], &proxy);

    if (i == SGI_KONA_CONTEXT_NONE || n < 0 || proxy || count > 512 ||
        first > SGI_KONA_GL_TABLE_MAX ||
        count > SGI_KONA_GL_TABLE_MAX - first) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: colour table read of target"
                      " 0x%" PRIx32 " entries %" PRIu32 "+%" PRIu32
                      " refused; no reply\n", args[0], first, count);
        return;
    }
    t = &s->state[i]->tables.table[n];
    base = sgi_kona_gl_table_base(t->format);
    data = g_malloc0(MAX(count * 16, 1));
    for (uint32_t k = 0; k < count && first + k < t->width; k++) {
        for (unsigned c = 0; c < 4; c++) {
            bool has = base == SGI_GL_RGBA ||
                       (base == SGI_GL_RGB && c < 3) ||
                       (c == 3 && (base == SGI_GL_ALPHA ||
                                   base == SGI_GL_LUMINANCE_ALPHA)) ||
                       (c == 0 && base != SGI_GL_ALPHA);

            if (has) {
                stl_be_p(data + k * 16 + c * 4, t->values[first + k][c]);
            }
        }
    }
    trace_sgi_kona_gl_reply(SGI_KONA_CMD_GL_GET_COLOUR_TABLE, args[0],
                            count * 16);
    sgi_kona_post_return(s, SGI_KONA_CMD_GL_GET_COLOUR_TABLE, 8, data,
                         count * 16);
}

/*
 * 0xec target pname. Each component of the internal format reads 12 bits
 * deep, the others 0. Approximated: the "OpenGL on Silicon Graphics
 * Systems" manual gives InfiniteReality's texture colour table 12-bit
 * entries; what the pipe answers for the other tables is unknown.
 */
static void sgi_kona_gl_colour_table_get_parameter(SGIKonaState *s, int i,
                                                   const uint32_t *args)
{
    const SGIKonaGLColourTables *tb;
    uint32_t format, width, base, word = 0;
    uint8_t data[16] = { 0 };
    bool proxy;
    int n = sgi_kona_gl_table_index(args[0], &proxy);

    if (i == SGI_KONA_CONTEXT_NONE || n < 0) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: colour table parameter read of"
                      " target 0x%" PRIx32 " refused; no reply\n", args[0]);
        return;
    }
    tb = &s->state[i]->tables;
    format = proxy ? tb->proxy_format[n] : tb->table[n].format;
    width = proxy ? tb->proxy_width[n] : tb->table[n].width;
    base = sgi_kona_gl_table_base(format);
    if (args[1] == 0x80d6 || args[1] == 0x80d7) {
        const uint32_t *v = args[1] == 0x80d6 ? tb->table[n].scale :
                                                tb->table[n].bias;

        for (unsigned c = 0; c < 4; c++) {
            stl_be_p(data + 4 * c, proxy ? 0 : v[c]);
        }
        sgi_kona_post_return(s, SGI_KONA_CMD_GL_GET_COLOUR_TABLE_PARAMETER,
                             8, data, sizeof(data));
        return;
    }
    switch (args[1]) {
    case 0x80d8:                /* COLOR_TABLE_FORMAT_SGI */
        word = format ? format : SGI_GL_RGBA;
        break;
    case 0x80d9:                /* COLOR_TABLE_WIDTH_SGI */
        word = width;
        break;
    case 0x80da ... 0x80dc:     /* red, green, blue */
        word = width && (base == SGI_GL_RGB || base == SGI_GL_RGBA) ? 12 : 0;
        break;
    case 0x80dd:                /* alpha */
        word = width && (base == SGI_GL_ALPHA || base == SGI_GL_RGBA ||
                         base == SGI_GL_LUMINANCE_ALPHA) ? 12 : 0;
        break;
    case 0x80de:                /* luminance */
        word = width && (base == SGI_GL_LUMINANCE ||
                         base == SGI_GL_LUMINANCE_ALPHA) ? 12 : 0;
        break;
    case 0x80df:                /* intensity */
        word = width && base == SGI_GL_INTENSITY ? 12 : 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: colour table parameter"
                      " 0x%" PRIx32 " is not known; answered 0\n", args[1]);
        break;
    }
    stl_be_p(data, word);
    sgi_kona_post_return(s, SGI_KONA_CMD_GL_GET_COLOUR_TABLE_PARAMETER, 8,
                         data, 4);
}

static bool sgi_kona_device_read(SGIKonaState *s, uint64_t device,
                                 uint8_t *buf, uint32_t len);

/*
 * EXT_histogram (established from the IRIX 6.5.30 client library, see the
 * command grammar):
 *
 *   0x102  target, width, width - 1 as a float, internal format, sink:
 *          GL_HISTOGRAM_EXT 0x8024 or its proxy 0x8025; width 0 when the
 *          library could not place the bins
 *   0x103  target: reset the histogram's counts
 *   0x104  width: the counts, answered from byte 4 of the return page, or
 *          into the buffer of a select-feed read DMA bracket
 *   0x105  target, pname: answered with a word count at byte 4 and the
 *          words from byte 8
 *   0x107  internal format, sink: GL_MINMAX_EXT
 *   0x108  target: reset the minmax table
 *   0x109  one word: the format at byte 4, the values from byte 8
 *   0x10a  pname: answered at byte 8
 *
 * Observed through the IRIX library on the model (a guest probe that
 * answered with numbered words): the library reads the counts as copies
 * of width bins of the format's components, 32-bit integers with no
 * padding, four copies back to back here, and adds them up; and the
 * minmax values as copies of eight floats, minimum R, G, B and A then
 * maximum R, G, B and A, taking the least minimum and the greatest
 * maximum. The number of copies comes from the library's board query.
 * Approximated: the model answers with the whole counts in the first copy
 * and zeros after it, and with the one minmax pair repeated, which every
 * copy count reads correctly.
 */
static void sgi_kona_gl_histogram_counts(const SGIKonaGLHistogram *h,
                                         uint8_t *out, uint32_t width,
                                         unsigned n)
{
    for (uint32_t bin = 0; bin < width && bin < h->width; bin++) {
        for (unsigned k = 0; k < n; k++) {
            stl_be_p(out + (bin * n + k) * 4, h->counts[bin][k]);
        }
    }
}

static void sgi_kona_gl_histogram_get(SGIKonaState *s, int i,
                                      const uint32_t *args)
{
    SGIKonaGLReadDMA *d = &s->gl_read_dma;
    SGIKonaGLHistogram *h = &s->state[i]->histogram;
    unsigned c[4], n = sgi_kona_gl_histogram_slots(h->format, c);
    uint32_t width = args[0];
    g_autofree uint8_t *data = NULL;
    uint32_t header[7] = { width, n };

    if (!h->width || !n || !width || width > SGI_KONA_GL_HISTOGRAM_MAX) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: histogram read of %"
                      PRIu32 " bins with no histogram defined; no reply\n",
                      width);
        if (d->active) {
            sgi_kona_gl_read_dma_refuse(s);
        }
        return;
    }
    if (d->active) {
        if (d->refused || d->count || d->context != s->context_current ||
            !sgi_kona_context_owner(s, d->context, d->generation) ||
            !sgi_kona_gl_read_dma_layout(d, header,
                                            SGI_KONA_CMD_GL_GET_HISTOGRAM)) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: histogram read DMA"
                          " refused\n");
            sgi_kona_gl_read_dma_refuse(s);
            return;
        }
        data = g_malloc0(d->descriptor[2]);
        sgi_kona_gl_histogram_counts(h, data, width, n);
        memcpy(d->header, header, sizeof(d->header));
        d->selector = SGI_KONA_CMD_GL_GET_HISTOGRAM;
        d->count = d->descriptor[2];
        d->data = g_steal_pointer(&data);
        return;
    }
    if ((uint64_t)width * n * 4 > SGI_KONA_RETURN_PAGE_BYTES - 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: histogram of %" PRIu32
                      " bins does not fit the return page; no reply\n",
                      width);
        return;
    }
    data = g_malloc0(SGI_KONA_RETURN_PAGE_BYTES - 4);
    sgi_kona_gl_histogram_counts(h, data, width, n);
    sgi_kona_post_return(s, SGI_KONA_CMD_GL_GET_HISTOGRAM, 4, data,
                         SGI_KONA_RETURN_PAGE_BYTES - 4);
}

/* 0x105's answer for pname: GL_HISTOGRAM_WIDTH_EXT to _SINK_EXT. */
static uint32_t sgi_kona_gl_histogram_parameter(uint32_t format,
                                                uint32_t width, bool sink,
                                                uint32_t pname)
{
    uint32_t base = sgi_kona_gl_table_base(format);
    bool has;

    switch (pname) {
    case 0x8026:
        return width;
    case 0x8027:
        return format ? format : SGI_GL_RGBA;
    case 0x802d:
        return sink;
    case 0x8028 ... 0x802a:     /* red, green, blue */
        has = base == SGI_GL_RGB || base == SGI_GL_RGBA;
        break;
    case 0x802b:                /* alpha */
        has = base == SGI_GL_ALPHA || base == SGI_GL_RGBA ||
              base == SGI_GL_LUMINANCE_ALPHA;
        break;
    case 0x802c:                /* luminance */
        has = base == SGI_GL_LUMINANCE || base == SGI_GL_LUMINANCE_ALPHA;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: histogram parameter 0x%"
                      PRIx32 " is not known; answered 0\n", pname);
        return 0;
    }
    /* Approximated: the counts are 32-bit words; the pipe's depth is not. */
    return width && has ? 32 : 0;
}

static void sgi_kona_gl_histogram(SGIKonaState *s, int i, uint32_t selector,
                                  const uint32_t *args)
{
    SGIKonaGLHistogram *h;
    uint32_t words[3] = { 0 };
    g_autofree uint8_t *data = NULL;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    h = &s->state[i]->histogram;
    switch (selector) {
    case SGI_KONA_CMD_GL_HISTOGRAM:
        if (args[0] == 0x8025) {
            h->proxy_width = args[1];
            h->proxy_format = args[1] ? args[3] : 0;
            return;
        }
        if (args[0] != 0x8024 || args[1] > SGI_KONA_GL_HISTOGRAM_MAX ||
            (args[1] && !sgi_kona_gl_histogram_slots(args[3],
                                                     (unsigned[4]) { 0 }))) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: histogram 0x%" PRIx32
                          " of %" PRIu32 " bins and format 0x%" PRIx32
                          " not kept\n", args[0], args[1], args[3]);
            h->width = 0;
            return;
        }
        memset(h->counts, 0, sizeof(h->counts));
        h->width = args[1];
        h->format = args[3];
        h->sink = args[4] != 0;
        return;
    case SGI_KONA_CMD_GL_RESET_HISTOGRAM:
        memset(h->counts, 0, sizeof(h->counts));
        return;
    case SGI_KONA_CMD_GL_GET_HISTOGRAM:
        sgi_kona_gl_histogram_get(s, i, args);
        return;
    case SGI_KONA_CMD_GL_GET_HISTOGRAM_PARAMETER:
        words[0] = cpu_to_be32(1);
        words[1] = cpu_to_be32(args[0] == 0x8025 ?
            sgi_kona_gl_histogram_parameter(h->proxy_format, h->proxy_width,
                                            false, args[1]) :
            sgi_kona_gl_histogram_parameter(h->format, h->width, h->sink,
                                            args[1]));
        sgi_kona_post_return(s, selector, 4, words, 8);
        return;
    case SGI_KONA_CMD_GL_MINMAX:
        if (!sgi_kona_gl_histogram_slots(args[0], (unsigned[4]) { 0 })) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: minmax format 0x%"
                          PRIx32 " not kept\n", args[0]);
            h->minmax_format = 0;
            return;
        }
        h->minmax_format = args[0];
        h->minmax_sink = args[1] != 0;
        sgi_kona_gl_minmax_reset(h);
        return;
    case SGI_KONA_CMD_GL_RESET_MINMAX:
        sgi_kona_gl_minmax_reset(h);
        return;
    case SGI_KONA_CMD_GL_GET_MINMAX:
        data = g_malloc0(SGI_KONA_RETURN_PAGE_BYTES - 4);
        stl_be_p(data, h->minmax_format ? h->minmax_format : SGI_GL_RGBA);
        for (uint32_t at = 4; at + 32 <= SGI_KONA_RETURN_PAGE_BYTES - 4;
             at += 32) {
            for (unsigned c = 0; c < 4; c++) {
                stl_be_p(data + at + 4 * c, h->min[c]);
                stl_be_p(data + at + 16 + 4 * c, h->max[c]);
            }
        }
        sgi_kona_post_return(s, selector, 4, data,
                             SGI_KONA_RETURN_PAGE_BYTES - 4);
        return;
    default:                    /* SGI_KONA_CMD_GL_GET_MINMAX_PARAMETER */
        words[0] = cpu_to_be32(args[0] == 0x802f ?
                               (h->minmax_format ? h->minmax_format :
                                                   SGI_GL_RGBA) :
                               args[0] == 0x8030 ? h->minmax_sink : 0);
        sgi_kona_post_return(s, selector, 8, words, 4);
        return;
    }
}

/*
 * Whether a filter or colour table image is to come by DMA (see
 * sgi-kona-dma.c).
 */
bool sgi_kona_filter_dma_waiting(SGIKonaState *s)
{
    int i = s->context_current;
    const SGIKonaGLImaging *im;
    uint32_t h[8];
    unsigned filter;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return false;
    }
    if (s->state[i]->tables.pending[0]) {
        return s->state[i]->tables.pending[7] == 1;
    }
    im = &s->state[i]->imaging;
    if (!im->pending_token) {
        return false;
    }
    sgi_kona_gl_filter_header(im->pending_token, im->pending, &filter, h);
    return h[5] == 1;
}

/*
 * An image of header h that a DMA packet carries, read as DrawPixels' DMA
 * is (sgi_kona_pixels_image_dma()) into PIO order: no more than max_words
 * words, or NULL, logged with `what`, when the source does not frame.
 */
static uint32_t *sgi_kona_gl_image_dma_words(SGIKonaState *s, int i,
                                             const uint32_t h[8], uint64_t io,
                                             uint32_t lines,
                                             uint32_t per_line, uint64_t pitch,
                                             uint32_t max_words,
                                             const char *what,
                                             uint32_t *nwords)
{
    const SGIKonaGLPipe *p = &s->state[i]->pipe;
    SGIKonaGLPixelDMALayout d;
    g_autofree uint32_t *words = NULL;
    uint8_t chunk[256];

    if (!sgi_kona_gl_pixels_dma_layout(h, p->unpack_row_length,
                                       p->unpack_skip_rows,
                                       p->unpack_skip_pixels,
                                       p->unpack_alignment, &d) ||
        (io & 3) != d.source_phase || lines != d.lines ||
        per_line != d.line_bytes || pitch != d.pitch ||
        d.span > UINT64_MAX - io || d.pixels.payload_words > max_words) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: %s DMA source framing"
                      " mismatch; completed unread\n", what);
        return NULL;
    }
    words = g_new0(uint32_t, d.pixels.payload_words);
    for (uint32_t line = 0; line < lines; line++) {
        for (uint32_t offset = 0; offset < per_line;) {
            uint32_t n = MIN(sizeof(chunk), per_line - offset);

            if (!sgi_kona_device_read(s, io + line * pitch + offset,
                                       chunk, n)) {
                return NULL;
            }
            for (uint32_t k = 0; k < n; k++) {
                uint32_t row = d.pixels.strided ? line :
                               (offset + k) / d.pixels.stride;
                uint32_t col = d.pixels.strided ? offset + k :
                               (offset + k) % d.pixels.stride;
                uint64_t at;

                if (col < d.pixels.row_bytes &&
                    sgi_kona_gl_pixels_pio_offset(&d.pixels, row, col, &at) &&
                    at / 4 < d.pixels.payload_words) {
                    words[at / 4] |= (uint32_t)chunk[k] << (24 - 8 * (at % 4));
                }
            }
            offset += n;
        }
    }
    *nwords = d.pixels.payload_words;
    return g_steal_pointer(&words);
}

/*
 * The DMA packet that carries a filter or colour table image, then kept.
 * The transfer completes whether or not the image is kept, as a refused
 * image's does (the kernel kills the pipe when a DMA it started never
 * completes).
 */
void sgi_kona_filter_image_dma(SGIKonaState *s, uint64_t io, uint32_t lines,
                               uint32_t per_line, uint64_t pitch)
{
    int i = s->context_current;
    SGIKonaGLImaging *im = &s->state[i]->imaging;
    SGIKonaGLColourTables *tb = &s->state[i]->tables;
    g_autofree uint32_t *words = NULL;
    uint32_t h[8], nwords;
    unsigned filter;

    if (tb->pending[0]) {
        sgi_kona_gl_table_header(tb->pending, h);
        words = sgi_kona_gl_image_dma_words(s, i, h, io, lines, per_line,
                                            pitch, SGI_KONA_GL_TABLE_WORDS,
                                            "colour table", &nwords);
        if (!words) {
            sgi_kona_gl_table_refuse(tb, "did not arrive");
            return;
        }
        sgi_kona_gl_table_load(s, i, words, nwords);
        return;
    }
    sgi_kona_gl_filter_header(im->pending_token, im->pending, &filter, h);
    words = sgi_kona_gl_image_dma_words(s, i, h, io, lines, per_line, pitch,
                                        SGI_KONA_GL_DATA_MAX,
                                        "convolution filter", &nwords);
    if (!words) {
        im->filters[filter].width = im->filters[filter].height = 0;
        im->pending_token = 0;
        return;
    }
    sgi_kona_gl_filter_load(s, i, words, nwords);
}

/*
 * 0xf2 (1D) and 0xf3 (2D) copy a filter from the framebuffer: the format
 * code, then x, y, width and, for 2D, height, window-relative as
 * CopyPixels' source (established from the client library). The pixels
 * are read as CopyPixels reads them, with no pixel transfer, then kept as
 * a filter image is.
 */
static void sgi_kona_gl_convolution_copy(SGIKonaState *s, int i,
                                         uint32_t selector,
                                         const uint32_t *args)
{
    bool two = selector == SGI_KONA_CMD_GL_COPY_CONVOLUTION_2D;
    SGIKonaGLFilter *f;
    SGIKonaGLCopySource source;
    uint32_t width = args[3], height = two ? args[4] : 1;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    f = &s->state[i]->imaging.filters[two ? 1 : 0];
    f->width = f->height = 0;
    if (!width || !height) {
        return;
    }
    if (args[0] > 5 || width > SGI_KONA_GL_FILTER_MAX ||
        height > SGI_KONA_GL_FILTER_MAX ||
        !sgi_kona_gl_copy_source_ready(s, i, args[1], args[2], width, height,
                                        0x1800, &source)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: convolution filter copy of %"
                      PRIu32 " by %" PRIu32 " refused; filter left"
                      " undefined\n", width, height);
        return;
    }
    for (uint32_t row = 0; row < height; row++) {
        for (uint32_t col = 0; col < width; col++) {
            uint32_t rgba;
            double values[4];

            if (!sgi_kona_gl_copy_read(&source, args[1] + col,
                                        args[2] + row, &rgba)) {
                return;
            }
            for (unsigned k = 0; k < 4; k++) {
                values[k] = ((rgba >> (24 - k * 8)) & 255) / 255.0;
            }
            if (!sgi_kona_gl_filter_store(f, col + row * width, values)) {
                return;
            }
        }
    }
    f->format = args[0];
    f->width = width;
    f->height = height;
    trace_sgi_kona_gl_filter(selector, f->format, width, height, 0);
}

/*
 * 0xf8 target pname value and 0xf9 target pname r g b a, the filter
 * parameters (established from the client library). 0xf8 carries
 * GL_CONVOLUTION_BORDER_MODE 0x8013, whose only value the library lets
 * through is GL_REDUCE 0x8016, the border the model applies; 0xf9 the
 * filter scale (0x8014) and bias (0x8015), applied to filter images taken
 * after it. The same tokens carry GL_PIXEL_TRANSFORM_2D_EXT's parameters,
 * which the model does not keep.
 */
static void sgi_kona_gl_convolution_parameter(SGIKonaState *s, int i,
                                              uint32_t selector,
                                              const uint32_t *args)
{
    SGIKonaGLFilter *f;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    if (args[0] < 0x8010 || args[0] > 0x8012) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: parameter 0x%" PRIx32 " of"
                      " target 0x%" PRIx32 " is not modelled\n", args[1],
                      args[0]);
        return;
    }
    f = &s->state[i]->imaging.filters[args[0] - 0x8010];
    if (selector == SGI_KONA_CMD_GL_CONVOLUTION_PARAMETER) {
        if (args[1] != 0x8013 || args[2] != 0x8016) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: convolution border"
                          " parameter 0x%" PRIx32 " 0x%" PRIx32 " is not"
                          " GL_REDUCE; ignored\n", args[1], args[2]);
        }
        return;
    }
    if (args[1] != 0x8014 && args[1] != 0x8015) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: convolution parameter"
                      " 0x%" PRIx32 " is not a filter scale or bias;"
                      " ignored\n", args[1]);
        return;
    }
    memcpy(args[1] == 0x8014 ? f->scale : f->bias, args + 2,
           4 * sizeof(*args));
}

/*
 * 0xf4 target f t and 0xf7 0x8012 f t, glGetConvolutionFilter and
 * glGetSeparableFilter (established from the client library): the pipe
 * answers on the return page at offset 8, rows of exactly width pixels
 * in the native format and type codes, bottom row first; the separable
 * reply's column follows its row at 5 pixels for filters up to 5 wide or
 * high, else 7. The components are packed as ReadPixels packs them, from
 * the filter's own (OpenGL 1.2.1, table 6.1), with no pixel transfer.
 */
static void sgi_kona_gl_convolution_get(SGIKonaState *s, int i,
                                        uint32_t selector,
                                        const uint32_t *args)
{
    const SGIKonaGLFilter *f;
    const SGIKonaGLPipe *p;
    uint32_t bpp = sgi_kona_gl_native_pixel_bytes(args[1], args[2]);
    uint32_t len, column = 0;
    g_autofree uint8_t *data = NULL;

    if (i == SGI_KONA_CONTEXT_NONE || args[0] < 0x8010 || args[0] > 0x8012 ||
        (selector == SGI_KONA_CMD_GL_GET_SEPARABLE) != (args[0] == 0x8012) ||
        !bpp || args[1] > 10 || !sgi_kona_gl_pixel_colour_format(args[1])) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: convolution filter read of"
                      " target 0x%" PRIx32 " format %" PRIu32 " type %"
                      PRIu32 " refused; no reply\n", args[0], args[1],
                      args[2]);
        return;
    }
    p = &s->state[i]->pipe;
    f = &s->state[i]->imaging.filters[args[0] - 0x8010];
    if (args[0] == 0x8012) {
        column = MAX(f->width, f->height) <= 5 ? 5 : 7;
        len = (column + f->height) * bpp;
    } else {
        len = f->width * f->height * bpp;
    }
    data = g_malloc0(MAX(len, 1));
    for (uint32_t k = 0; args[0] != 0x8012 && k < f->width * f->height;
         k++) {
        double rgba[4];

        sgi_kona_gl_filter_rgba(f, k, rgba);
        sgi_kona_gl_pixel_pack_double(args[1], args[2], rgba,
                                      p->pack_swap_bytes, data + k * bpp,
                                      bpp);
    }
    for (uint32_t k = 0; args[0] == 0x8012 && k < f->width; k++) {
        double rgba[4];

        sgi_kona_gl_filter_rgba(f, k, rgba);
        sgi_kona_gl_pixel_pack_double(args[1], args[2], rgba,
                                      p->pack_swap_bytes, data + k * bpp,
                                      bpp);
    }
    for (uint32_t k = 0; args[0] == 0x8012 && k < f->height; k++) {
        double rgba[4];

        sgi_kona_gl_filter_rgba(f, SGI_KONA_GL_FILTER_MAX + k, rgba);
        sgi_kona_gl_pixel_pack_double(args[1], args[2], rgba,
                                      p->pack_swap_bytes,
                                      data + (column + k) * bpp, bpp);
    }
    trace_sgi_kona_gl_reply(selector, args[0], len);
    sgi_kona_post_return(s, selector, 8, data, len);
}

/*
 * The 2D filter a 2D image transfer applies: the 2D filter when
 * GL_CONVOLUTION_2D is enabled, else the separable one when
 * GL_SEPARABLE_2D is (OpenGL 1.2.1's imaging subset; inferred for this
 * pipe), or -1. A filter not yet defined applies none.
 */
static int sgi_kona_gl_filter_2d(const SGIKonaGLImaging *im)
{
    if ((im->enabled & 2) && im->filters[1].width) {
        return 1;
    }
    if (!(im->enabled & 2) && (im->enabled & 4) && im->filters[2].width) {
        return 2;
    }
    return -1;
}

static bool sgi_kona_gl_post_conv_default(const SGIKonaGLPixelTransfer *t)
{
    for (unsigned c = 0; c < 4; c++) {
        if (t->post_conv_scale[c] != 0x3f800000 || t->post_conv_bias[c]) {
            return false;
        }
    }
    return true;
}

/* Whether the colour matrix stage changes anything. */
static bool sgi_kona_gl_matrix_stage(const uint32_t m[16],
                                     const SGIKonaGLPixelTransfer *t)
{
    for (unsigned k = 0; k < 16; k++) {
        if (m[k] != (k % 5 ? 0 : 0x3f800000)) {
            return true;
        }
    }
    for (unsigned c = 0; c < 4; c++) {
        if (t->post_matrix_scale[c] != 0x3f800000 || t->post_matrix_bias[c]) {
            return true;
        }
    }
    return false;
}

/*
 * Whether colour image transfers have a stage after the pixel maps: a 2D
 * filter, an enabled colour table that is not empty, post-convolution
 * scale and bias other than 1 and 0, or a colour matrix or post-matrix
 * scale and bias other than identity. With none,
 * the transfer is exactly what it was without the imaging subset.
 */
static bool sgi_kona_gl_imaging_active(const SGIKonaGLState *st)
{
    return sgi_kona_gl_filter_2d(&st->imaging) >= 0 ||
           sgi_kona_gl_tables_active(st) ||
           sgi_kona_gl_histogram_active(st) ||
           !sgi_kona_gl_post_conv_default(&st->pipe.pixel_transfer) ||
           sgi_kona_gl_matrix_stage(st->imaging.colour_matrix,
                                    &st->pipe.pixel_transfer);
}

/*
 * The colour matrix, post-matrix scale and bias, then the clamp to
 * [0, 1] (the OpenGL reference page for glPixelTransfer: R' = (m1 R + m5
 * G + m9 B + m13 A) Rscale + Rbias, the matrix in column order).
 */
static bool sgi_kona_gl_post_matrix(const uint32_t m[16],
                                    const SGIKonaGLPixelTransfer *t,
                                    double rgba[4])
{
    double in[4];

    memcpy(in, rgba, sizeof(in));
    for (unsigned r = 0; r < 4; r++) {
        double value = 0;

        for (unsigned c = 0; c < 4; c++) {
            value += sgi_kona_gl_float(m[c * 4 + r]) * in[c];
        }
        value = value * sgi_kona_gl_float(t->post_matrix_scale[r]) +
                sgi_kona_gl_float(t->post_matrix_bias[r]);
        if (!isfinite(value)) {
            return false;
        }
        rgba[r] = CLAMP(value, 0.0, 1.0);
    }
    return true;
}

/*
 * A colour table's base internal format (EXT_texture's mapping, which
 * SGI_color_table uses), or 0 for a token it does not name.
 */
static uint32_t sgi_kona_gl_table_base(uint32_t format)
{
    switch (format) {
    case SGI_GL_ALPHA:
    case SGI_GL_ALPHA4 ... SGI_GL_ALPHA16:
        return SGI_GL_ALPHA;
    case 1:
    case SGI_GL_LUMINANCE:
    case SGI_GL_LUMINANCE4 ... SGI_GL_LUMINANCE16:
        return SGI_GL_LUMINANCE;
    case 2:
    case SGI_GL_LUMINANCE_ALPHA:
    case SGI_GL_LUMINANCE4_ALPHA4 ... SGI_GL_LUMINANCE16_ALPHA16:
        return SGI_GL_LUMINANCE_ALPHA;
    case SGI_GL_INTENSITY:
    case SGI_GL_INTENSITY4 ... SGI_GL_INTENSITY16:
        return SGI_GL_INTENSITY;
    case 3:
    case SGI_GL_RGB:
    case SGI_GL_R3_G3_B2:
    case SGI_GL_RGB4 ... SGI_GL_RGB16:
        return SGI_GL_RGB;
    case 4:
    case SGI_GL_RGBA:
    case SGI_GL_RGBA2 ... SGI_GL_RGBA16:
        return SGI_GL_RGBA;
    default:
        return 0;
    }
}

/*
 * The table slot a component of the base format takes its value from, or
 * -1 when the table leaves it alone (SGI_color_table, table E14.2). A
 * luminance or intensity table keeps its value in the red slot.
 */
static int sgi_kona_gl_table_slot(uint32_t base, unsigned c)
{
    switch (base) {
    case SGI_GL_ALPHA:
        return c == 3 ? 3 : -1;
    case SGI_GL_LUMINANCE:
        return c < 3 ? 0 : -1;
    case SGI_GL_LUMINANCE_ALPHA:
        return c < 3 ? 0 : 3;
    case SGI_GL_INTENSITY:
        return 0;
    case SGI_GL_RGB:
        return c < 3 ? (int)c : -1;
    case SGI_GL_RGBA:
        return c;
    default:
        return -1;
    }
}

/*
 * A colour table's lookup: each component the table replaces is clamped
 * to [0, 1], scaled by one less than the width, rounded to an index, and
 * replaced by the table's value there (SGI_color_table, "Color Table
 * Lookup"). SGI_texture_color_table modifies texel components by the same
 * table of base formats.
 */
static void sgi_kona_gl_table_lookup(const void *table, double rgba[4])
{
    const SGIKonaGLColourTable *t = table;
    uint32_t base = sgi_kona_gl_table_base(t->format);
    double in[4];

    memcpy(in, rgba, sizeof(in));
    for (unsigned c = 0; c < 4; c++) {
        int slot = sgi_kona_gl_table_slot(base, c);
        uint32_t index;

        if (slot < 0) {
            continue;
        }
        index = lround(CLAMP(in[c], 0.0, 1.0) * (t->width - 1));
        rgba[c] = sgi_kona_gl_float(t->values[index][slot]);
    }
}

/* Table n of st when it is enabled and not empty, else NULL. */
static const SGIKonaGLColourTable *
sgi_kona_gl_table_enabled(const SGIKonaGLState *st, unsigned n)
{
    const SGIKonaGLColourTable *t = &st->tables.table[n];

    return (st->imaging.stages & SGI_KONA_GL_STAGE_TABLE(n)) && t->width ?
           t : NULL;
}

static void sgi_kona_gl_table_stage(const SGIKonaGLState *st, unsigned n,
                                    double rgba[4])
{
    const SGIKonaGLColourTable *t = sgi_kona_gl_table_enabled(st, n);

    if (t) {
        sgi_kona_gl_table_lookup(t, rgba);
    }
}

/* Whether an enabled colour table changes colour image transfers. */
static bool sgi_kona_gl_tables_active(const SGIKonaGLState *st)
{
    for (unsigned n = 0; n < 3; n++) {
        if ((st->imaging.stages & SGI_KONA_GL_STAGE_TABLE(n)) &&
            st->tables.table[n].width) {
            return true;
        }
    }
    return false;
}

/*
 * The RGBA components a histogram or minmax format holds, in its own
 * order (luminance is red), or 0 for a format it cannot take.
 */
static unsigned sgi_kona_gl_histogram_slots(uint32_t format, unsigned c[4])
{
    switch (sgi_kona_gl_table_base(format)) {
    case SGI_GL_ALPHA:
        c[0] = 3;
        return 1;
    case SGI_GL_LUMINANCE:
        c[0] = 0;
        return 1;
    case SGI_GL_LUMINANCE_ALPHA:
        c[0] = 0;
        c[1] = 3;
        return 2;
    case SGI_GL_RGB:
        c[0] = 0;
        c[1] = 1;
        c[2] = 2;
        return 3;
    case SGI_GL_RGBA:
        c[0] = 0;
        c[1] = 1;
        c[2] = 2;
        c[3] = 3;
        return 4;
    default:
        return 0;
    }
}

/* Whether an enabled histogram or minmax table takes pixel groups. */
static bool sgi_kona_gl_histogram_active(const SGIKonaGLState *st)
{
    return ((st->imaging.stages & 1) && st->histogram.width) ||
           ((st->imaging.stages & 2) && st->histogram.minmax_format);
}

/* Whether an enabled histogram or minmax table consumes the groups. */
static bool sgi_kona_gl_histogram_sinks(const SGIKonaGLState *st)
{
    return ((st->imaging.stages & 1) && st->histogram.width &&
            st->histogram.sink) ||
           ((st->imaging.stages & 2) && st->histogram.minmax_format &&
            st->histogram.minmax_sink);
}

/*
 * The histogram, then minmax, after the post-colour-matrix table. Each
 * component the format holds, luminance taken from red and clamped to
 * [0, 1], counts once in bin round((width - 1) c), and moves the minimum
 * and maximum (the "OpenGL on Silicon Graphics Systems" manual,
 * EXT_histogram section). A count stops at its largest value.
 */
static void sgi_kona_gl_histogram_stage(SGIKonaGLState *st,
                                        const double rgba[4])
{
    SGIKonaGLHistogram *h = &st->histogram;
    unsigned c[4], n;

    if ((st->imaging.stages & 1) && h->width) {
        n = sgi_kona_gl_histogram_slots(h->format, c);
        for (unsigned k = 0; k < n; k++) {
            uint32_t bin = lround(CLAMP(rgba[c[k]], 0.0, 1.0) *
                                  (h->width - 1));

            if (h->counts[bin][k] != UINT32_MAX) {
                h->counts[bin][k]++;
            }
        }
    }
    if ((st->imaging.stages & 2) && h->minmax_format) {
        n = sgi_kona_gl_histogram_slots(h->minmax_format, c);
        for (unsigned k = 0; k < n; k++) {
            float v = rgba[c[k]];

            if (v < sgi_kona_gl_float(h->min[c[k]])) {
                memcpy(&h->min[c[k]], &v, sizeof(v));
            }
            if (v > sgi_kona_gl_float(h->max[c[k]])) {
                memcpy(&h->max[c[k]], &v, sizeof(v));
            }
        }
    }
}

/*
 * The per-pixel stages after convolution for context state st, with the
 * two colour tables in their places: post-convolution scale and bias, the
 * post-convolution table, the colour matrix stage and the post-colour-
 * matrix table (SGI_color_table names their order).
 */
static bool sgi_kona_gl_after_convolution(SGIKonaGLState *st,
                                          bool convolved, double rgba[4])
{
    const SGIKonaGLPixelTransfer *t = &st->pipe.pixel_transfer;
    const uint32_t *m = st->imaging.colour_matrix;

    if ((convolved || !sgi_kona_gl_post_conv_default(t)) &&
        !sgi_kona_gl_post_conv(t, rgba)) {
        return false;
    }
    sgi_kona_gl_table_stage(st, 1, rgba);
    if (sgi_kona_gl_matrix_stage(m, t) &&
        !sgi_kona_gl_post_matrix(m, t, rgba)) {
        return false;
    }
    sgi_kona_gl_table_stage(st, 2, rgba);
    sgi_kona_gl_histogram_stage(st, rgba);
    return true;
}

/*
 * Every per-pixel imaging stage after the pixel maps for a transfer that
 * is not convolved: the colour table, then the stages after convolution.
 */
static bool sgi_kona_gl_pixel_path(SGIKonaGLState *st, double rgba[4])
{
    sgi_kona_gl_table_stage(st, 0, rgba);
    return sgi_kona_gl_after_convolution(st, false, rgba);
}

/* Post-convolution scale and bias, then the clamp to [0, 1]. */
static bool sgi_kona_gl_post_conv(const SGIKonaGLPixelTransfer *t,
                                  double rgba[4])
{
    for (unsigned c = 0; c < 4; c++) {
        double scale = sgi_kona_gl_float(t->post_conv_scale[c]);
        double bias = sgi_kona_gl_float(t->post_conv_bias[c]);
        double value = rgba[c] * scale + bias;

        if (!isfinite(value)) {
            return false;
        }
        rgba[c] = CLAMP(value, 0.0, 1.0);
    }
    return true;
}

/*
 * The imaging stages after the pixel maps for a colour image of `*width`
 * by `*height` pixels, bottom row first: the 2D or separable filter with
 * the REDUCE border, which leaves an image smaller by the filter's size
 * less one in each direction (EXT_convolution, as the "OpenGL on Silicon
 * Graphics Systems" manual's equations give it), then post-convolution
 * scale and bias with the clamp (the OpenGL reference page for
 * glPixelTransfer).
 *
 * A component the filter's internal format does not have is not filtered.
 * Approximated: it takes the source pixel's value at the filter window's
 * origin, which is what an identity filter would give; what the pipe
 * returns for it is unknown.
 */
static bool sgi_kona_gl_imaging(SGIKonaGLState *st,
                                double (**image)[4], uint32_t *width,
                                uint32_t *height)
{
    int filter = sgi_kona_gl_filter_2d(&st->imaging);
    double (*in)[4] = *image;
    g_autofree double (*out)[4] = NULL;
    uint32_t w = *width, h = *height;

    if (filter >= 0) {
        const SGIKonaGLFilter *f = &st->imaging.filters[filter];

        /* The colour table comes before the filter. */
        for (uint64_t k = 0; k < (uint64_t)w * h; k++) {
            sgi_kona_gl_table_stage(st, 0, in[k]);
        }
        bool separable = filter == 2;
        uint32_t ow = w >= f->width ? w - f->width + 1 : 0;
        uint32_t oh = h >= f->height ? h - f->height + 1 : 0;

        out = g_try_malloc_n(MAX((uint64_t)ow * oh, 1), sizeof(*out));
        if (!out) {
            return false;
        }
        for (uint32_t y = 0; y < oh; y++) {
            for (uint32_t x = 0; x < ow; x++) {
                double *d = out[(uint64_t)y * ow + x];

                for (unsigned c = 0; c < 4; c++) {
                    double sum = 0, kernel, column;
                    bool filtered = true;

                    for (uint32_t n = 0; filtered && n < f->height; n++) {
                        if (separable &&
                            !sgi_kona_gl_filter_kernel(f,
                                SGI_KONA_GL_FILTER_MAX + n, c, &column)) {
                            filtered = false;
                            break;
                        }
                        for (uint32_t m = 0; m < f->width; m++) {
                            unsigned k = separable ? m : m + n * f->width;

                            if (!sgi_kona_gl_filter_kernel(f, k, c,
                                                            &kernel)) {
                                filtered = false;
                                break;
                            }
                            if (separable) {
                                kernel *= column;
                            }
                            sum += kernel *
                                   in[(uint64_t)(y + n) * w + x + m][c];
                        }
                    }
                    d[c] = filtered ? sum : in[(uint64_t)y * w + x][c];
                }
                if (!sgi_kona_gl_after_convolution(st, true, d)) {
                    return false;
                }
            }
        }
        g_free(*image);
        *image = g_steal_pointer(&out);
        *width = ow;
        *height = oh;
        goto sink;
    }
    for (uint64_t k = 0; k < (uint64_t)w * h; k++) {
        if (!sgi_kona_gl_pixel_path(st, in[k])) {
            return false;
        }
    }
sink:
    /* A sinking histogram or minmax table consumes the groups. */
    if (sgi_kona_gl_histogram_sinks(st)) {
        *width = *height = 0;
    }
    return true;
}

/* The end of a GL data run. */
static void sgi_kona_gl_data_done(SGIKonaState *s)
{
    if (s->gl_data_token == SGI_KONA_CMD_GL_POLYGON_STIPPLE &&
        s->context_current != SGI_KONA_CONTEXT_NONE) {
        sgi_kona_gl_stipple_rows(s, s->context_current);
    }
    if (s->gl_data_token == SGI_KONA_CMD_GL_POP_LIGHTS) {
        sgi_kona_gl_pop_lights(s);
    }
    if (s->gl_data_token == SGI_KONA_CMD_GL_TEX_LOAD &&
        s->texture_upload.count) {
        if (sgi_kona_texture_upload_normalize(s, &s->texture_upload)) {
            s->texture_upload.complete = true;
        } else {
            sgi_kona_texture_upload_clear(s);
        }
    }
    if (s->gl_data_token == SGI_KONA_CMD_GL_BITMAP) {
        sgi_kona_gl_raw_bitmap_done(s);
    }
    if (s->gl_data_token == SGI_KONA_CMD_GL_BITMAP_INLINE) {
        sgi_kona_gl_bitmap_done(s);
    }
    if (s->gl_data_token == SGI_KONA_CMD_GL_DRAW_PIXELS) {
        if (s->gl_image_upload.draw_pixels) {
            sgi_kona_gl_pixels_done(s);
        } else {
            sgi_kona_gl_draw_pixels_done(s);
        }
    }
    if (s->gl_data_token == SGI_KONA_CMD_GL_PIXEL_MAP_F ||
        s->gl_data_token == SGI_KONA_CMD_GL_PIXEL_MAP_UI ||
        s->gl_data_token == SGI_KONA_CMD_GL_PIXEL_MAP_US) {
        sgi_kona_gl_pixel_map_done(s);
    }
    if (s->gl_data_token == SGI_KONA_CMD_GL_CONVOLUTION_1D ||
        s->gl_data_token == SGI_KONA_CMD_GL_CONVOLUTION_2D ||
        s->gl_data_token == SGI_KONA_CMD_GL_SEPARABLE) {
        sgi_kona_gl_convolution_done(s);
    }
    if (s->gl_data_token == SGI_KONA_CMD_GL_COLOUR_TABLE) {
        sgi_kona_gl_colour_table_done(s);
    }
    if (s->gl_data_token != SGI_KONA_CMD_GL_ARRAY_BULK) {
        trace_sgi_kona_command_unmodelled(s->gl_data_token,
                                          s->gl_data_have);
    }
    s->gl_data_token = 0;
    s->gl_data_have = 0;
}

static const SGICommandPortOps sgi_kona_gl_ops = {
    .packet = sgi_kona_gl_packet,
};

/*
 * fg_h in the one-bit form the X server's bitmap GC loader sends: its GC
 * parser sets it to this constant in the colour-index case, and the
 * foreground travels in word 1 instead (established). No solid fg_h below
 * can equal it: that packing never sets bit 7.
 */
#define SGI_KONA_X_GC_BITMAP_FG_H 0xfff0

/*
 * The colour index of the one-bit form's word 1, P(v) = ((v >> 4) & 0xfff)
 * | ((v & 0xf) << 20) with v = index << (16 - depth) (the same subsection,
 * established for the senders).
 */
static uint32_t sgi_kona_x_packed_index(uint32_t w1)
{
    uint32_t v = ((w1 & 0xfff) << 4) | ((w1 >> 20) & 0xf);

    return v >> (16 - SGI_KONA_X_TRACE_INDEX_DEPTH);
}

/* Preserve the historical trace projection without narrowing draw state. */
static uint32_t sgi_kona_x_gc_index(const uint32_t *gc)
{
    uint32_t fg_h = gc[0];
    uint32_t v = ((fg_h >> 3) & 0xf) | ((fg_h >> 4) & 0xfff0);

    if (fg_h == SGI_KONA_X_GC_BITMAP_FG_H) {
        return sgi_kona_x_packed_index(gc[1]);
    }
    return v >> (16 - SGI_KONA_X_TRACE_INDEX_DEPTH);
}

/*
 * Retain all thirteen native GC words. The historical trace prints an
 * eight-bit projection; drawing uses the full decoded index and mask.
 */
static void sgi_kona_x_load_gc(SGIKonaState *s, const uint32_t *args)
{
    memcpy(s->x_gc, args, sizeof(s->x_gc));
    s->x_read_gc_valid = sgi_kona_x_read_current(s);
    trace_sgi_kona_x_gc(s->x_gc[0], sgi_kona_x_gc_index(s->x_gc));
}

/*
 * 0x12a, X load position: the target drawable's x, y, width and height, as
 * the X server's GC loader sends them. It is the origin of X rect
 * coordinates, see sgi_kona_x_fill_rect(). Box and pixel-write coordinates
 * arrive absolute, every sender of them setting the position to 0, 0 first,
 * so the model never adds it to those.
 */
static void sgi_kona_x_load_position(SGIKonaState *s, const uint32_t *args)
{
    memcpy(s->x_position, args, sizeof(s->x_position));
    trace_sgi_kona_x_position(args[0], args[1], args[2], args[3]);
}

/*
 * Whether a word that would end the waiting X data or clip-list run
 * leaves it in place instead: the kernel's privileged words, and the
 * words of a context other than the run's owner. The run is part of the
 * X server's command, as an X pixel write's rows are, so a context
 * switch the kernel makes between two of the X server's commands, and
 * whatever another context sends while switched in, leave it for the X
 * server's next command (inferred: the pipe keeps a context's open
 * command with the context, see sgi_kona_input_save()). A run begun with
 * no context current has no owner and ends at any such word, as before.
 */
static bool sgi_kona_x_run_held(SGIKonaState *s, uint32_t next)
{
    uint32_t words = s->x_boxes_have + s->x_clip_run_have;

    if (!words ||
        (!s->kernel_input &&
         (s->x_run_owner == SGI_KONA_CONTEXT_NONE ||
          s->x_run_owner == s->context_current))) {
        return false;
    }
    trace_sgi_kona_x_run_held(words, next, s->x_run_owner);
    return true;
}

/* The current context starts an X data or clip-list run. */
static void sgi_kona_x_run_begin(SGIKonaState *s)
{
    if (!s->x_boxes_have && !s->x_clip_run_have) {
        s->x_run_owner = s->context_current;
        s->x_run_owner_generation =
            sgi_kona_context_generation(s, s->context_current);
    }
}

/*
 * An X token from a context other than the held run's owner. The appenders
 * and the box, rect, point, line and clip handlers share the one pair of
 * run buffers, so such a token adds to or ends the owner's run. Only the X
 * server's context is inferred to send X tokens (see the command grammar),
 * so this is not expected; the token is still taken, as an X pixel write
 * takes a DMA packet that arrives while another context is current, and
 * the log makes the case visible.
 */
static void sgi_kona_x_run_foreign(SGIKonaState *s, uint32_t token)
{
    if ((s->x_boxes_have || s->x_clip_run_have) &&
        s->x_run_owner != SGI_KONA_CONTEXT_NONE &&
        s->x_run_owner != s->context_current) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: context %d's X run of %" PRIu32
                      " words is taken by token 0x%" PRIx32 " while context"
                      " %d is current\n", s->x_run_owner,
                      s->x_boxes_have + s->x_clip_run_have, token,
                      s->context_current);
    }
}

/* A clip list's words ended by something other than X load clip. */
static void sgi_kona_x_clip_run_drop(SGIKonaState *s, uint32_t next)
{
    if (s->x_clip_run_have && !sgi_kona_x_run_held(s, next)) {
        sgi_kona_x_read_invalidate(s);
        trace_sgi_kona_x_clip_run_dropped(s->x_clip_run_have, next);
        s->x_clip_run_have = 0;
    }
}

/*
 * 0x145, X bdata4: the leading words of a clip list longer than one
 * rectangle, as the X server's clip loader sends it, kept until the X
 * load clip ending it.
 */
static void sgi_kona_x_clip_data(SGIKonaState *s, const uint32_t *args)
{
    if (s->x_clip_run_have + SGI_KONA_X_BDATA4_WORDS +
        SGI_KONA_X_CLIP_WORDS > SGI_KONA_X_CLIP_LIST_WORDS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X clip list longer than %u rectangles;"
                      " dropped\n", SGI_KONA_X_CLIP_MAX_RECTS);
        sgi_kona_x_clip_run_drop(s, SGI_KONA_CMD_X_BDATA4);
        return;
    }
    sgi_kona_x_run_begin(s);
    memcpy(s->x_clip_run + s->x_clip_run_have, args,
           SGI_KONA_X_BDATA4_WORDS * sizeof(*args));
    s->x_clip_run_have += SGI_KONA_X_BDATA4_WORDS;
}

/*
 * 0x12b, X load clip: the end of a clip list, a rectangle count n and n
 * rectangles x1, y1, x2, y2, the first words carried by any X bdata4 before
 * it, as the X server's clip loader sends it. Inferred: each rectangle is
 * absolute with an exclusive far corner, and drawing is clipped to their
 * union. A list whose length does not match its count, including a count of
 * 0, is logged and leaves drawing clipped only to the screen, since what
 * the pipe makes of one is unknown.
 */
static void sgi_kona_x_load_clip(SGIKonaState *s, const uint32_t *args)
{
    uint32_t *list = s->x_clip_run;
    uint32_t words = s->x_clip_run_have + SGI_KONA_X_CLIP_WORDS;
    uint32_t count;

    memcpy(list + s->x_clip_run_have, args,
           SGI_KONA_X_CLIP_WORDS * sizeof(*args));
    s->x_clip_run_have = 0;
    s->x_scissor_valid = false;
    count = list[0];
    trace_sgi_kona_x_clip(count, args[1], args[2], args[3], args[4]);
    if (count == 0 || words != 1 + 4 * (uint64_t)count) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: X clip list of %" PRIu32 " words for %"
                      PRIu32 " rectangles; clipping only to the screen\n",
                      words, count);
        s->x_clip_count = 0;
        return;
    }
    s->x_clip_count = count;
    memcpy(s->x_clip_rects, list + 1, 4 * count * sizeof(*list));
}

/*
 * 0x12d, X load scissor: the clip box's x1, y1, width and height, which the
 * X server's solid and image glyph senders and its fixed-width ones send
 * right after a one-rectangle X load clip with the same box (established).
 * The model applies it as a second rectangle with the clip, to the pixel
 * writes, copies and fills after it, see sgi_kona_x_clip(). A scissor of
 * zero width or height clips everything, logged.
 *
 * Approximated: that the pipe applies it with the clip is inferred, and
 * for one box it adds nothing to the clip. How long it lasts is
 * unknown. The model clears it at the next X load clip. The fill and span
 * senders read send a clip and no scissor, so a scissor outliving its
 * clip would confine their later drawing to the last label's box; the
 * greeter's scroll-bar arrow, spans at (305, 143) sent after the labels'
 * scissor of 240, 295, 72, 32, would not appear. The scissor qtest checks
 * the framing; the guest check is the labels and that arrow both
 * appearing.
 */
static void sgi_kona_x_load_scissor(SGIKonaState *s, const uint32_t *args)
{
    memcpy(s->x_scissor, args, sizeof(s->x_scissor));
    s->x_scissor_valid = true;
    trace_sgi_kona_x_scissor(args[0], args[1], args[2], args[3]);
    if (!args[2] || !args[3]) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: X scissor of %" PRIu32 " by %" PRIu32
                      "; clipping everything\n", args[2], args[3]);
    }
}

/*
 * Whether a store of a width by height pattern into `entry` fits the
 * model's stipple cache.
 */
static bool sgi_kona_x_stipple_fits(uint32_t entry, uint32_t width,
                                    uint32_t height)
{
    return entry < SGI_KONA_X_STIPPLE_ENTRIES &&
           width && width <= SGI_KONA_X_STIPPLE_MAX &&
           height && height <= SGI_KONA_X_STIPPLE_MAX;
}

/* A stipple store's last row: the entry takes the pattern's size. */
static void sgi_kona_x_stipple_stored(SGIKonaState *s)
{
    uint32_t entry = s->x_stipple_store_entry;
    bool fits = sgi_kona_x_stipple_fits(entry, s->x_stipple_store_width,
                                        s->x_stipple_store_height);

    if (fits) {
        s->x_stipple_width[entry] = s->x_stipple_store_width;
        s->x_stipple_height[entry] = s->x_stipple_store_height;
    }
    trace_sgi_kona_x_stipple_stored(entry, fits);
}

/*
 * 0x13d, X store stipple: a stipple-cache entry, the pattern's width and
 * height, then `height` rows, one word each, most significant bit leftmost
 * (the X server's cached-stipple loader, established for the senders; the
 * bit order is inferred from a traced run's 6 by 20 I-beam). The rows
 * arrive through sgi_kona_x_stipple_word().
 *
 * Approximated: the cache's size, SGI_KONA_X_STIPPLE_ENTRIES patterns of
 * up to SGI_KONA_X_STIPPLE_MAX square, is the model's choice. A store
 * outside it is logged and its rows consumed, and the entry, if it exists,
 * is left holding no pattern. A store replaces the entry's pattern, which
 * a loaded entry's later fills then use, and keeps its origin; whether the
 * pipe does either is unknown. The stipple qtests are the discriminating
 * test.
 */
static void sgi_kona_x_store_stipple(SGIKonaState *s, const uint32_t *args)
{
    uint32_t entry = args[0], width = args[1], height = args[2];

    trace_sgi_kona_x_stipple_store(entry, width, height);
    s->x_stipple_store_entry = entry;
    s->x_stipple_store_width = width;
    s->x_stipple_store_height = height;
    s->x_stipple_store_remaining = height;
    if (!sgi_kona_x_stipple_fits(entry, width, height)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: X stipple entry 0x%" PRIx32 " of %" PRIu32
                      " by %" PRIu32 " does not fit the model's %u entries"
                      " of up to %u by %u; not kept\n", entry, width, height,
                      SGI_KONA_X_STIPPLE_ENTRIES, SGI_KONA_X_STIPPLE_MAX,
                      SGI_KONA_X_STIPPLE_MAX);
    }
    if (entry < SGI_KONA_X_STIPPLE_ENTRIES) {
        s->x_stipple_width[entry] = 0;
        s->x_stipple_height[entry] = 0;
    }
    if (!height) {
        sgi_kona_x_stipple_stored(s);
    }
}

/* One row of an open stipple store. */
static void sgi_kona_x_stipple_word(SGIKonaState *s, uint32_t word)
{
    uint32_t entry = s->x_stipple_store_entry;
    uint32_t row = s->x_stipple_store_height - s->x_stipple_store_remaining;

    if (sgi_kona_x_stipple_fits(entry, s->x_stipple_store_width,
                                s->x_stipple_store_height)) {
        s->x_stipple_rows[entry * SGI_KONA_X_STIPPLE_MAX + row] = word;
    }
    if (--s->x_stipple_store_remaining == 0) {
        sgi_kona_x_stipple_stored(s);
    }
}

/*
 * 0x13e, X rotate stipple: an entry, then the pattern origin x, y, which
 * the fills masked with that entry measure the pattern from (established
 * for the senders; what the origin does is inferred, see
 * sgi_kona_x_stipple_bit()). An entry outside the cache is logged.
 */
static void sgi_kona_x_rotate_stipple(SGIKonaState *s, const uint32_t *args)
{
    uint32_t entry = args[0];

    trace_sgi_kona_x_stipple_rotate(entry, args[1], args[2]);
    if (entry >= SGI_KONA_X_STIPPLE_ENTRIES) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: X stipple rotate of entry 0x%" PRIx32
                      " outside the model's %u; dropped\n", entry,
                      SGI_KONA_X_STIPPLE_ENTRIES);
        return;
    }
    s->x_stipple_origin_x[entry] = args[1];
    s->x_stipple_origin_y[entry] = args[2];
}

/*
 * 0x13c, X load stipple: an entry, whose pattern then masks the box, rect
 * and span fills that follow (the X server's stippled rect and span fills),
 * or SGI_KONA_X_STIPPLE_NONE, which ends the masking (its stipple load and
 * unload routines; both uses are inferred). An entry outside the cache is
 * logged and unloads the stipple; an entry holding no pattern is logged,
 * and the fills are not masked while it holds none.
 */
static void sgi_kona_x_load_stipple(SGIKonaState *s, uint32_t entry)
{
    trace_sgi_kona_x_stipple_load(entry);
    if (entry == SGI_KONA_X_STIPPLE_NONE) {
        s->x_stipple_loaded = SGI_KONA_X_STIPPLE_NONE;
        return;
    }
    if (entry >= SGI_KONA_X_STIPPLE_ENTRIES) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: X stipple load of entry 0x%" PRIx32
                      " outside the model's %u; stipple unloaded\n", entry,
                      SGI_KONA_X_STIPPLE_ENTRIES);
        s->x_stipple_loaded = SGI_KONA_X_STIPPLE_NONE;
        return;
    }
    if (!s->x_stipple_width[entry]) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X stipple load of entry 0x%" PRIx32
                      ", which holds no pattern\n", entry);
    }
    s->x_stipple_loaded = entry;
}

/*
 * Whether the loaded stipple lets a fill, box, rect or span, paint
 * (x, y): the bit at ((x - ox) mod width, (y - oy) mod height) of the
 * pattern, most significant bit leftmost, where (ox, oy) is the entry's
 * rotate origin plus the X load position's x and y. With no stipple
 * loaded, or one holding no pattern, every pixel is painted.
 *
 * Approximated from inference: the origin is drawable-relative like a
 * rect's, bits past the width are ignored, and a clear bit leaves its
 * pixel unchanged. The origin words come from a private
 * record nobody traced, and the drawable-relative reading rests only on
 * the run's I-beam lining up with its rect; an absolute origin would
 * shift the pattern by the position. What clear bits paint is unknown;
 * no stippled fill in the run paints on the screen to tell a transparent
 * stipple from an opaque one. The discriminating test is the stipple
 * qtests; the guest's is the text field's I-beam cursor, drawn under
 * entry 0x1f.
 */
static bool sgi_kona_x_stipple_bit(SGIKonaState *s, int64_t x, int64_t y)
{
    uint32_t entry = s->x_stipple_loaded;
    uint32_t width, height, row;
    int64_t ox, oy;

    if (entry >= SGI_KONA_X_STIPPLE_ENTRIES || !s->x_stipple_width[entry]) {
        return true;
    }
    width = s->x_stipple_width[entry];
    height = s->x_stipple_height[entry];
    ox = (int64_t)(int32_t)s->x_position[0] + s->x_stipple_origin_x[entry];
    oy = (int64_t)(int32_t)s->x_position[1] + s->x_stipple_origin_y[entry];
    row = s->x_stipple_rows[entry * SGI_KONA_X_STIPPLE_MAX +
                            (((y - oy) % height) + height) % height];
    return row & (0x80000000u >> ((((x - ox) % width) + width) % width));
}

/* The rectangle X drawing lands in, far corner exclusive. */
typedef struct SGIKonaRect {
    int64_t x1, y1, x2, y2;
} SGIKonaRect;

/*
 * The screen, narrowed to the bounds of the X clip list's rectangles when
 * one is loaded, and to the scissor when one is. Within it,
 * sgi_kona_x_clip_contains() says whether a pixel is in one of the list's
 * rectangles.
 */
static void sgi_kona_x_clip(SGIKonaState *s, SGIKonaRect *r)
{
    r->x1 = 0;
    r->y1 = 0;
    r->x2 = s->fb_width;
    r->y2 = s->fb_height;
    if (s->x_clip_count) {
        const int32_t *c = (const int32_t *)s->x_clip_rects;
        int64_t x1 = INT64_MAX, y1 = INT64_MAX;
        int64_t x2 = INT64_MIN, y2 = INT64_MIN;

        for (uint32_t i = 0; i < s->x_clip_count; i++, c += 4) {
            x1 = MIN(x1, c[0]);
            y1 = MIN(y1, c[1]);
            x2 = MAX(x2, c[2]);
            y2 = MAX(y2, c[3]);
        }
        r->x1 = MAX(r->x1, x1);
        r->y1 = MAX(r->y1, y1);
        r->x2 = MIN(r->x2, x2);
        r->y2 = MIN(r->y2, y2);
    }
    if (s->x_scissor_valid) {
        int64_t x1 = (int32_t)s->x_scissor[0];
        int64_t y1 = (int32_t)s->x_scissor[1];

        r->x1 = MAX(r->x1, x1);
        r->y1 = MAX(r->y1, y1);
        r->x2 = MIN(r->x2, x1 + s->x_scissor[2]);
        r->y2 = MIN(r->y2, y1 + s->x_scissor[3]);
    }
}

/* Whether (x, y), inside sgi_kona_x_clip()'s bounds, is in the clip. */
static bool sgi_kona_x_clip_contains(SGIKonaState *s, int64_t x, int64_t y)
{
    const int32_t *c = (const int32_t *)s->x_clip_rects;

    if (s->x_clip_count <= 1) {
        return true;
    }
    for (uint32_t i = 0; i < s->x_clip_count; i++, c += 4) {
        if (x >= c[0] && y >= c[1] && x < c[2] && y < c[3]) {
            return true;
        }
    }
    return false;
}

/*
 * Complete GC diagnostics retain unsupported semantic fields. Only the
 * fills take an opaque or tiled GC (`opaque`); see sgi_kona_x_fill_source().
 */
static bool sgi_kona_x_decode_opaque(SGIKonaState *s, SGIXGC *gc,
                                     bool opaque)
{
    SGIXGCStatus status = s->x_gc[6] == 0xc || s->x_gc[6] == 0x180c ?
        sgi_x_gc_decode_main_index(s->x_gc, gc) : sgi_x_gc_decode(s->x_gc, gc);

    if (status == SGI_X_GC_OK && (opaque || !gc->opaque_or_tiled)) {
        return true;
    }
    qemu_log_mask(LOG_UNIMP, "sgi-kona: unsupported X GC status %u:", status);
    for (unsigned i = 0; i < SGI_KONA_X_GC_WORDS; i++) {
        qemu_log_mask(LOG_UNIMP, " %08x", s->x_gc[i]);
    }
    qemu_log_mask(LOG_UNIMP, "\n");
    return false;
}

static bool sgi_kona_x_decode(SGIKonaState *s, SGIXGC *gc)
{
    return sgi_kona_x_decode_opaque(s, gc, false);
}

/*
 * Whether a stipple is loaded for the fills to use: the entry the last
 * 0x13c named, inside the cache, with a pattern stored.
 */
static bool sgi_kona_x_stipple_loaded(SGIKonaState *s)
{
    uint32_t entry = s->x_stipple_loaded;

    return entry < SGI_KONA_X_STIPPLE_ENTRIES && s->x_stipple_width[entry];
}

/*
 * The X paths reach a row's pixels through one run (sgi_plane_run()) when
 * the row allows it, and pixel by pixel otherwise: a run resolves the
 * plane's format, bounds and host address once per row instead of once per
 * pixel, and loads and stores the same bytes. A NULL plane has no run.
 */
static void sgi_kona_x_row(SGIBoardMemory *m, const SGIPlane *p, int64_t x1,
                           int64_t x2, int row, SGIPlaneRun *run)
{
    if (!p || x1 < INT_MIN || x2 > INT_MAX ||
        !sgi_plane_run(m, p, x1, row, x2 - x1, run)) {
        run->host = NULL;
    }
}

static inline uint32_t sgi_kona_x_row_load(SGIBoardMemory *m,
                                           const SGIPlane *p,
                                           const SGIPlaneRun *run,
                                           int64_t x1, int x, int row)
{
    return run->host ? sgi_plane_run_load(run, x - x1) :
                       sgi_plane_load(m, p, x, row);
}

static inline void sgi_kona_x_row_store(SGIBoardMemory *m, const SGIPlane *p,
                                        const SGIPlaneRun *run, int64_t x1,
                                        int x, int row, uint32_t value)
{
    if (run->host) {
        sgi_plane_run_store(run, x - x1, value);
    } else {
        sgi_plane_store(m, p, x, row, value);
    }
}

/*
 * Retain the existing clipped/stippled geometry. Validate every accessed
 * destination before any canonical word/tag store.
 */
static bool sgi_kona_x_fill_source(SGIKonaState *s, int64_t x1, int64_t y1,
                                   int64_t x2, int64_t y2, bool background,
                                   bool commit)
{
    SGIBoardMemory *memory = &s->mem;
    const SGIPlane *front[2] = { sgi_kona_plane(s, s->plane_front) };
    const SGIPlane *tag[2] = { sgi_kona_plane(s, s->plane_front_tag) };
    uint32_t target_tag = SGI_KONA_TAG_X_INDEX16;
    unsigned banks = 1;
    SGIXGC gc;
    SGIKonaRect clip;
    uint32_t foreground = 0, background_value = 0;
    bool constant;

    /*
     * Word 4's bit 31 marks the X server GC parser's FillOpaqueStippled,
     * and FillTiled with a tile the GC's private data holds: the GC then
     * carries both colours and a stipple is loaded. A clear stipple bit
     * paints the background, as X's opaque stipple does (inferred; the pipe
     * was not observed). A tiled fill is drawn the same way, as a
     * two-colour stipple from the loaded pattern and the GC's two colours:
     * the senders seen load an 8x8 stipple before such a fill, and a tile
     * with more than two colours could not be drawn correctly this way
     * (approximated). With no stipple loaded the pattern is unknown, so the
     * fill is still refused.
     */
    if (!sgi_kona_x_decode_opaque(s, &gc, sgi_kona_x_stipple_loaded(s))) {
        return false;
    }
    if (gc.target == SGI_X_GC_DID || gc.target == SGI_X_GC_OVERLAY0 ||
        gc.target == SGI_X_GC_OVERLAY1 || gc.target == SGI_X_GC_POPUP) {
        unsigned plane = gc.target == SGI_X_GC_DID ? 0 :
                         gc.target == SGI_X_GC_OVERLAY0 ? 1 :
                         gc.target == SGI_X_GC_OVERLAY1 ? 2 : 3;

        memory = &s->desktop_mem;
        front[0] = sgi_board_memory_plane(memory, plane);
        tag[0] = NULL;
    } else if (gc.target == SGI_X_GC_OVERLAY_BOTH) {
        /*
         * The model's two overlay planes, each the index's low byte.
         * Which byte of the pipe's word each buffer holds is unknown;
         * the form repeats the index, so either reading stores the same.
         */
        memory = &s->desktop_mem;
        front[0] = sgi_board_memory_plane(memory, 1);
        front[1] = sgi_board_memory_plane(memory, 2);
        tag[0] = tag[1] = NULL;
        banks = 2;
    } else if (gc.target == SGI_X_GC_MAIN_RGB8) {
        target_tag = SGI_KONA_TAG_DIRECT;
        if (gc.raw[6] == 4 && gc.first_selector == 3) {
            /* Selector 3's physical bank meaning is unknown. */
            front[1] = sgi_kona_plane(s, s->plane_back);
            tag[1] = sgi_kona_plane(s, s->plane_back_tag);
            banks = 2;
        }
    } else if (gc.target != SGI_X_GC_MAIN_INDEX16) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: X fill target %u unsupported\n",
                      gc.target);
        return false;
    }
    if (sgi_x_gc_noop(&gc)) {
        return true;
    }
    sgi_kona_x_clip(s, &clip);
    x1 = MAX(x1, clip.x1);
    y1 = MAX(y1, clip.y1);
    x2 = MIN(x2, clip.x2);
    y2 = MIN(y2, clip.y2);
    if (x1 >= x2 || y1 >= y2) {
        return true;
    }
    assert(bql_locked());
    if (sgi_x_gc_needs_destination(&gc)) {
        for (unsigned bank = 0; bank < banks; bank++) {
            for (int y = y1; y < y2; y++) {
                int row = s->fb_height - 1 - y;
                SGIPlaneRun front_run, tag_run;

                sgi_kona_x_row(memory, front[bank], x1, x2, row, &front_run);
                sgi_kona_x_row(memory, tag[bank], x1, x2, row, &tag_run);
                for (int x = x1; x < x2; x++) {
                    if (!sgi_kona_x_clip_contains(s, x, y) ||
                        (!gc.opaque_or_tiled &&
                         !sgi_kona_x_stipple_bit(s, x, y))) {
                        continue;
                    }
                    if ((tag[bank] &&
                         sgi_kona_x_row_load(memory, tag[bank], &tag_run,
                                             x1, x, row) != target_tag) ||
                        sgi_kona_x_row_load(memory, front[bank], &front_run,
                                            x1, x, row) > gc.width_mask) {
                        qemu_log_mask(LOG_UNIMP,
                                      "sgi-kona: X fill incompatible "
                                      "destination at (%d,%d)\n", x, y);
                        return false;
                    }
                }
            }
        }
    }
    if (!commit) {
        return true;
    }
    /*
     * A GC that needs no destination makes each pixel's word from its
     * colour alone, so the two colours' results are made once here.
     */
    constant = !sgi_x_gc_needs_destination(&gc);
    if (constant) {
        foreground = sgi_x_gc_apply(&gc, gc.foreground, 0);
        background_value = sgi_x_gc_apply(&gc, gc.background, 0);
    }
    for (unsigned bank = 0; bank < banks; bank++) {
        for (int y = y1; y < y2; y++) {
            int row = s->fb_height - 1 - y;
            bool changed = false;
            SGIPlaneRun front_run, tag_run;

            sgi_kona_x_row(memory, front[bank], x1, x2, row, &front_run);
            sgi_kona_x_row(memory, tag[bank], x1, x2, row, &tag_run);
            for (int x = x1; x < x2; x++) {
                uint32_t old, value;
                bool set;

                if (!sgi_kona_x_clip_contains(s, x, y)) {
                    continue;
                }
                set = sgi_kona_x_stipple_bit(s, x, y);
                if (!set && !gc.opaque_or_tiled) {
                    continue;
                }
                old = sgi_kona_x_row_load(memory, front[bank], &front_run,
                                          x1, x, row);
                if (constant) {
                    value = background || !set ? background_value :
                                                 foreground;
                } else {
                    value = sgi_x_gc_apply(&gc, background || !set ?
                                           gc.background : gc.foreground,
                                           old);
                }
                if (value != old ||
                    (tag[bank] &&
                     sgi_kona_x_row_load(memory, tag[bank], &tag_run, x1, x,
                                         row) != target_tag)) {
                    sgi_kona_x_row_store(memory, front[bank], &front_run, x1,
                                         x, row, value);
                    if (tag[bank]) {
                        sgi_kona_x_row_store(memory, tag[bank], &tag_run, x1,
                                             x, row, target_tag);
                    } else {
                        s->desktop_state_used = true;
                        if (gc.target == SGI_X_GC_DID) {
                            s->desktop_id_reported = false;
                            s->display_id_generation++;
                        }
                    }
                    changed = true;
                }
            }
            if (changed) {
                sgi_kona_invalidate_plane(s, front[bank],
                                             row, row);
            }
        }
    }
    return true;
}

static bool sgi_kona_x_fill(SGIKonaState *s, int64_t x1, int64_t y1,
                            int64_t x2, int64_t y2, bool commit)
{
    return sgi_kona_x_fill_source(s, x1, y1, x2, y2, false, commit);
}

static bool sgi_kona_x_fill_box(SGIKonaState *s, uint32_t w0, uint32_t w1,
                                 bool commit)
{
    int x1 = (int16_t)(w0 >> 16);
    int y1 = (int16_t)w0;
    int x2 = (int16_t)(w1 >> 16);
    int y2 = (int16_t)w1;

    trace_sgi_kona_x_box(x1, y1, x2, y2, sgi_kona_x_gc_index(s->x_gc));
    return sgi_kona_x_fill(s, x1, y1, x2, y2, commit);
}

/*
 * One X rect, the words x << 16 | y with signed halves and width << 16 |
 * height with unsigned ones, the X protocol's rectangle, relative to the
 * drawable the X load position names (the X server's solid rect fill,
 * established). Inferred: the pipe adds the position's x and y, then clips
 * to the clip list. The guest check is the greeter button's face
 * (244, 299)-(308, 323) taking the GC's colour after a position of
 * 240, 295.
 */
static bool sgi_kona_x_fill_rect(SGIKonaState *s, uint32_t w0, uint32_t w1,
                                  bool commit)
{
    int64_t x = (int16_t)(w0 >> 16) + (int64_t)(int32_t)s->x_position[0];
    int64_t y = (int16_t)w0 + (int64_t)(int32_t)s->x_position[1];
    uint32_t width = w1 >> 16;
    uint32_t height = w1 & 0xffff;

    trace_sgi_kona_x_rect(x, y, width, height,
                          sgi_kona_x_gc_index(s->x_gc));
    return sgi_kona_x_fill(s, x, y, x + width, y + height, commit);
}

/*
 * 0x156, 0x157 and 0x158, X spans, spans2 and spans6: a count of 1, 2 or
 * 6, then that many points x << 16 | y with signed 16-bit halves, then
 * that many widths (the X server's solid span fill, established). Each
 * span is a one-pixel-high run of `width` pixels from its point, the X
 * protocol's span, filled like an X box: clipped to the screen, the clip
 * list and the scissor, masked by the loaded stipple, with the GC's colour
 * index. A count that does not match the token is logged and the spans
 * dropped.
 *
 * Approximated: the points are taken as absolute, with no X load position
 * added, as the senders are read. Every traced span follows a position
 * of 0, 0, so no traced sequence tells the two readings apart; a spans
 * sender that leaves a non-zero position in place would. The glyph qtest
 * spans-ignore-position pins the model's reading. The spans qtests and
 * the scroll bar's down arrow at (305, 143) check the spans
 * themselves.
 */
static void sgi_kona_x_spans(SGIKonaState *s, uint32_t token,
                             const uint32_t *args, unsigned argc)
{
    uint32_t n = (argc - 1) / 2;
    uint32_t index = sgi_kona_x_gc_index(s->x_gc);

    if (args[0] != n) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X spans token 0x%" PRIx32 " with count %"
                      PRIu32 ", not %" PRIu32 "; dropped\n", token, args[0],
                      n);
        return;
    }
    for (unsigned pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < n; i++) {
            int x = (int16_t)(args[1 + i] >> 16);
            int y = (int16_t)args[1 + i];
            uint32_t width = args[1 + n + i];

            trace_sgi_kona_x_span(x, y, width, index);
            if (!sgi_kona_x_fill(s, x, y, (int64_t)x + width,
                                 (int64_t)y + 1, pass != 0)) {
                return;
            }
        }
    }
}

/* Deferred data words use the next supported point, box or rect's shape. */
static void sgi_kona_x_box_data(SGIKonaState *s, uint32_t token,
                                const uint32_t *args, unsigned argc)
{
    trace_sgi_kona_x_box_data(token, argc);
    if (s->x_boxes_have + argc > SGI_KONA_X_BOX_RUN_WORDS) {
        sgi_kona_x_read_invalidate(s);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X data token 0x%" PRIx32 " makes a run of"
                      " more than %u box words; run dropped\n", token,
                      SGI_KONA_X_BOX_RUN_WORDS);
        trace_sgi_kona_x_box_data_dropped(s->x_boxes_have + argc, token);
        s->x_boxes_have = 0;
        return;
    }
    sgi_kona_x_run_begin(s);
    memcpy(s->x_boxes + s->x_boxes_have, args, argc * sizeof(*args));
    s->x_boxes_have += argc;
}

/*
 * A run of data tokens ended by something other than an X box or rect,
 * unless the word leaves it in place (sgi_kona_x_run_held()).
 */
static void sgi_kona_x_box_data_drop(SGIKonaState *s, uint32_t next)
{
    if (s->x_boxes_have && !sgi_kona_x_run_held(s, next)) {
        sgi_kona_x_read_invalidate(s);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X data run of %u words dropped before 0x%x\n",
                      s->x_boxes_have, next);
        trace_sgi_kona_x_box_data_dropped(s->x_boxes_have, next);
        s->x_boxes_have = 0;
    }
}

/*
 * X box, box6, rect or rect6: the waiting run's words, then the token's
 * own, as boxes or as rectangles.
 */
static void sgi_kona_x_boxes(SGIKonaState *s, bool rects, const uint32_t *args,
                             unsigned argc)
{
    bool (*fill)(SGIKonaState *, uint32_t, uint32_t, bool) =
        rects ? sgi_kona_x_fill_rect : sgi_kona_x_fill_box;

    if (s->x_boxes_have & 1) {
        sgi_kona_x_box_data_drop(s, rects ? SGI_KONA_CMD_X_RECT :
                                         SGI_KONA_CMD_X_BOX);
    }
    for (unsigned pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i + 1 < s->x_boxes_have; i += 2) {
            if (!fill(s, s->x_boxes[i], s->x_boxes[i + 1], pass != 0)) {
                goto done;
            }
        }
        for (unsigned i = 0; i + 1 < argc; i += 2) {
            if (!fill(s, args[i], args[i + 1], pass != 0)) {
                goto done;
            }
        }
    }
done:
    s->x_boxes_have = 0;
}

static bool sgi_kona_x_point_words(SGIKonaState *s, const uint32_t *words,
                                   unsigned count, bool background,
                                   bool commit)
{
    for (unsigned i = 0; i < count; i++) {
        int x = (int16_t)(words[i] >> 16);
        int y = (int16_t)words[i];

        if (!sgi_kona_x_fill_source(s, x, y, x + 1, y + 1,
                                      background, commit)) {
            return false;
        }
    }
    return true;
}

static void sgi_kona_x_points(SGIKonaState *s, const uint32_t *args,
                              unsigned argc, bool background)
{
    if (sgi_kona_x_point_words(s, s->x_boxes, s->x_boxes_have,
                                 background, false) &&
        sgi_kona_x_point_words(s, args, argc, background, false)) {
        sgi_kona_x_point_words(s, s->x_boxes, s->x_boxes_have,
                                 background, true);
        sgi_kona_x_point_words(s, args, argc, background, true);
    }
    s->x_boxes_have = 0;
}

static bool sgi_kona_x_relative_coordinates(uint32_t *words, unsigned count)
{
    int x = 0, y = 0;

    /* Relative packets use a fresh absolute anchor in the model. */
    for (unsigned i = 0; i < count; i++) {
        x += (int16_t)(words[i] >> 16);
        y += (int16_t)words[i];
        if (x < INT16_MIN || x > INT16_MAX ||
            y < INT16_MIN || y > INT16_MAX) {
            return false;
        }
        words[i] = ((uint32_t)(uint16_t)x << 16) | (uint16_t)y;
    }
    return true;
}

/* Endpoint and Bresenham tie rules are the documented solid-line policy. */
static bool sgi_kona_x_segment(SGIKonaState *s, uint32_t first,
                               uint32_t last, bool endpoint, bool commit)
{
    int x = (int16_t)(first >> 16), y = (int16_t)first;
    int x2 = (int16_t)(last >> 16), y2 = (int16_t)last;
    int dx = abs(x2 - x), dy = -abs(y2 - y);
    int sx = x < x2 ? 1 : -1, sy = y < y2 ? 1 : -1;
    int error = dx + dy;

    while (x != x2 || y != y2) {
        int twice = 2 * error;

        if (!sgi_kona_x_fill(s, x, y, x + 1, y + 1, commit)) {
            return false;
        }
        if (twice >= dy) {
            error += dy;
            x += sx;
        }
        if (twice <= dx) {
            error += dx;
            y += sy;
        }
    }
    return !endpoint || sgi_kona_x_fill(s, x, y, x + 1, y + 1, commit);
}

static void sgi_kona_x_lines(SGIKonaState *s, uint32_t token,
                             const uint32_t *args, unsigned argc)
{
    uint32_t vertices[SGI_KONA_X_BOX_RUN_WORDS + 12];
    unsigned count = s->x_boxes_have + argc;
    bool segments = token == SGI_KONA_CMD_X_SEGMENT ||
                    token == SGI_KONA_CMD_X_SEGMENT6;
    bool relative = token == SGI_KONA_CMD_X_RELATIVE_LINE ||
                    token == SGI_KONA_CMD_X_RELATIVE_LINE6;
    bool endpoint = token != SGI_KONA_CMD_X_LINE6 &&
                    token != SGI_KONA_CMD_X_RELATIVE_LINE6;
    bool moved = false;
    SGIXGC gc;

    if (count > ARRAY_SIZE(vertices) || count < 2 ||
        (segments && (count & 1))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: invalid X solid-line vertex count; dropped\n");
        goto done;
    }
    if (s->x_gc[10] || !sgi_kona_x_decode(s, &gc) ||
        gc.first_selector != 1 || (s->x_gc[7] & ~0xcU) != 1) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: unsupported X solid-line GC; dropped\n");
        goto done;
    }
    memcpy(vertices, s->x_boxes, s->x_boxes_have * sizeof(*vertices));
    memcpy(vertices + s->x_boxes_have, args, argc * sizeof(*vertices));
    if (relative && !sgi_kona_x_relative_coordinates(vertices, count)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: relative X line coordinate"
                      " overflow; run dropped\n");
        goto done;
    }
    for (unsigned i = 1; i < count; i++) {
        moved |= vertices[i] != vertices[0];
    }
    endpoint &= !moved || vertices[0] != vertices[count - 1];
    for (unsigned pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i + 1 < count; i += segments ? 2 : 1) {
            if (!sgi_kona_x_segment(s, vertices[i], vertices[i + 1],
                                       segments, pass != 0)) {
                goto done;
            }
        }
        if (!segments && endpoint &&
            !sgi_kona_x_point_words(s, vertices + count - 1, 1,
                                       false, pass != 0)) {
            goto done;
        }
    }
done:
    s->x_boxes_have = 0;
}

static void sgi_kona_x_relative_points(SGIKonaState *s,
                                       const uint32_t *args, unsigned argc)
{
    uint32_t points[SGI_KONA_X_BOX_RUN_WORDS + 24];
    unsigned count = s->x_boxes_have + argc;

    if (count > ARRAY_SIZE(points)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: relative X point run too long; dropped\n");
        goto done;
    }
    memcpy(points, s->x_boxes, s->x_boxes_have * sizeof(*points));
    memcpy(points + s->x_boxes_have, args, argc * sizeof(*points));
    if (!sgi_kona_x_relative_coordinates(points, count)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: relative X point coordinate"
                      " overflow; run dropped\n");
        goto done;
    }
    if (sgi_kona_x_point_words(s, points, count, false, false)) {
        sgi_kona_x_point_words(s, points, count, false, true);
    }
done:
    s->x_boxes_have = 0;
}

/* The words of an X pixel setup, in send order. */
enum {
    SGI_KONA_X_SETUP_MODE,
    SGI_KONA_X_SETUP_SRC_PACK,
    SGI_KONA_X_SETUP_DST_PACK,
    SGI_KONA_X_SETUP_SHIFT,
    SGI_KONA_X_SETUP_WIDTH,
    SGI_KONA_X_SETUP_HEIGHT,
    SGI_KONA_X_SETUP_PAD,
};

/* Mode word bit the X image write sets when rows go by ioctl 0x2711. */
#define SGI_KONA_X_SETUP_MODE_DMA 0x10

/*
 * 0x136, X pixel setup: kept for the pixel write or copy after it
 * (established from the X server's pixmap GC loader). The packing and shift
 * words are table entries whose meaning to the pipe is unknown. Source
 * packing frames the complete payload; each operation validates its
 * reviewed setup subset before using the retained words.
 */
static void sgi_kona_x_pixel_setup(SGIKonaState *s, const uint32_t *args)
{
    memcpy(s->x_pixel_setup, args, sizeof(s->x_pixel_setup));
    s->x_read_setup_valid = sgi_kona_x_read_current(s);
    trace_sgi_kona_x_pixel_setup(args[0], args[1], args[2], args[3], args[4],
                                 args[5], args[6]);
}

/* Main-plane reads ignore foreground and write masks. */
static bool sgi_kona_x_read_gc(const uint32_t *gc, bool rgb)
{
    static const uint32_t forms[][7] = {
        { 0x180c, 0x40001, 1, 6, 5, 1, 1 },
        { 0xc, 1, 1, 6, 0, 1, 1 },
    };

    static const uint32_t rgb_form[] = { 0x2804, 0x60001, 1, 6, 8, 0, 0 };

    if (rgb) {
        return !memcmp(gc + 6, rgb_form, sizeof(rgb_form));
    }
    return !memcmp(gc + 6, forms[0], sizeof(forms[0])) ||
           !memcmp(gc + 6, forms[1], sizeof(forms[1]));
}

/*
 * The frame-buffer word read: the KONA X driver module's frame-buffer word GC
 * for plane selector 1 (cfg 0x1008, op 0x68001, addr 1, ifmt 0xa), with setup
 * mode 2, packing 2 and 2 and shift 0, one 32-bit word a pixel. Its caller,
 * under the X server's display read, takes a colour-index main pixel's index
 * from the word's top bits: index = word >> (32 - depth). The model's X index
 * is a left-justified 16-bit value, so the word is that value shifted left 16.
 * What the pipe puts in bits 15:0 is unknown; the caller reads bits 15:8 only
 * for an alpha it is asked for, and the model leaves them zero.
 *
 * Plane selectors 0, 1 and 2 (op (s << 18) | 0x28001, addr s) are
 * answered per pixel by sgi_kona_x_read_fb_word(). For a TrueColor
 * window with a two-word format (bit 0x4000 clear) the reader asks for
 * selectors 1 and 2 and takes red, green and blue from the first word,
 * selector 1's, when a byte of the window's private data is 0;
 * selector 2's word is read and not used there. When that byte is 1 it
 * asks for 2 and 3, takes the alpha from bits 15:8 of selector 2's word
 * and red, green and blue from selector 3's.
 *
 * Known limit: the model answers selector 2 with the RGB word, so a
 * reader in the second case would take blue (bits 15:8) as alpha, and refuses
 * selector 3, answering zeros. What that byte means, and what the pipe
 * keeps as a direct-colour pixel's alpha, are unknown.
 */
static bool sgi_kona_x_read_fb_word_gc(const uint32_t *gc)
{
    /* Selector s: op (s << 18) | 0x28001 and addr s (the X module's GC). */
    for (uint32_t sel = 0; sel <= 3; sel++) {
        const uint32_t form[] = {
            0x1008, (sel << 18) | 0x28001, sel, 6, 0xa, 1, 1,
        };

        if (!memcmp(gc + 6, form, sizeof(form))) {
            return true;
        }
    }
    return false;
}

/*
 * The frame-buffer word for plane selector `sel` over a pixel the model holds
 * with tag `tag` and stored word `word`, or false where the model has no
 * answer. From the frame-buffer word reader in the KONA X driver module, which
 * picks the selector and the decoding from the window's format word
 * (established for the reader, not for the pipe):
 *
 *   selector 1, 8-bit-component formats (format & 0xf == 1): a colour-index
 *   pixel's index from the top bits; a TrueColor pixel's red, green and
 *   blue from bits 31:24, 23:16 and 15:8 (for a one-word format, and for a
 *   two-word one whose window byte is 0). So selector 1 over a
 *   direct-colour pixel is its RGB shifted left 8, as selector 2's word is
 *   answered.
 *
 *   selector 0, 16-bit formats (format & 0xf == 2): a TrueColor pixel's
 *   5-bit red, green and blue from bits 31:27, 26:22 and 21:17, and bit 16
 *   the alpha flag. Selector 0 is also the 2-bit overlay read (format 8,
 *   index in bits 15:14); the model answers selector 0 over direct-colour
 *   pixels only.
 *
 *   selector 3, a two-word TrueColor format whose window byte is 1:
 *   red, green and blue from bits 31:24, 23:16 and 15:8, so
 *   over a direct-colour pixel it is answered as selector 1. The model
 *   keeps one set of RGB a pixel, so what distinguishes the selectors on
 *   the pipe is not modelled.
 *
 * Every selector reads the bank its pixel shows (see
 * sgi_kona_displayed_bank()), the screen as its caller, the X server's
 * display read, names it, rather than refusing a pixel whose
 * double-buffered window shows bank 1 (inferred from the routine's purpose
 * and oglsnoop's use; which bank the pipe reads for each selector is
 * unknown).
 *
 * Approximated: the model stores logical RGB8, so selector 0 answers the
 * top five bits of each component. What the pipe returns in the bits the
 * reader ignores (7:0 for selector 1 and 2, 15:0 for selector 0) is
 * unknown and answered zero; that includes selector 0's alpha bit, which
 * a reader asked for alpha would take as transparent.
 */
static bool sgi_kona_x_read_fb_word(uint32_t sel, uint32_t tag,
                                    uint32_t word, uint32_t *out)
{
    if (tag == SGI_KONA_TAG_X_INDEX16 && word <= 0xffff && sel == 1) {
        *out = word << 16;
        return true;
    }
    if (tag != SGI_KONA_TAG_DIRECT || word > 0xffffff) {
        return false;
    }
    if (sel == 0) {
        *out = ((word >> 19) & 0x1f) << 27 | ((word >> 11) & 0x1f) << 22 |
               ((word >> 3) & 0x1f) << 17;
    } else {
        *out = word << 8;
    }
    return true;
}

/* Public index values use tightly packed rows; source words stay native. */
static bool sgi_kona_x_read_payload(SGIKonaState *s, const uint32_t *args,
                                    uint32_t limit, uint8_t **out_data,
                                    uint32_t *out_count)
{
    const uint32_t *setup = s->x_pixel_setup;
    int64_t x = (int32_t)args[0], y = (int32_t)args[1];
    uint64_t width = setup[SGI_KONA_X_SETUP_WIDTH];
    uint64_t height = setup[SGI_KONA_X_SETUP_HEIGHT];
    unsigned bytes = setup[SGI_KONA_X_SETUP_DST_PACK] == 0 &&
                     setup[SGI_KONA_X_SETUP_SHIFT] == 8 ? 1 :
                     setup[SGI_KONA_X_SETUP_DST_PACK] == 1 &&
                     setup[SGI_KONA_X_SETUP_SHIFT] == 12 ? 2 :
                     setup[SGI_KONA_X_SETUP_DST_PACK] == 2 &&
                     ((int32_t)setup[SGI_KONA_X_SETUP_SHIFT] == -8 ||
                      setup[SGI_KONA_X_SETUP_SHIFT] == 0) ? 4 : 0;
    bool fb_word = bytes == 4 && setup[SGI_KONA_X_SETUP_SHIFT] == 0;
    uint64_t pitch = width * bytes;
    const char *reason;
    g_autofree uint8_t *data = NULL;

    if (!sgi_kona_x_read_current(s)) {
        reason = "no current context-owned return page";
    } else if (s->x_pixel_open || s->x_pixel_remaining ||
               s->gl_data_remaining || s->gl_tex_table_remaining ||
               s->x_boxes_have || s->x_clip_run_have) {
        reason = "an incompatible transfer is open";
    } else if (!s->x_read_gc_valid || !s->x_read_setup_valid) {
        reason = "GC or setup not loaded in this activation";
    } else if (fb_word ? !sgi_kona_x_read_fb_word_gc(s->x_gc) :
               !sgi_kona_x_read_gc(s->x_gc, bytes == 4)) {
        reason = "unsupported main-plane GC";
    } else if (setup[SGI_KONA_X_SETUP_MODE] != 2 ||
               setup[SGI_KONA_X_SETUP_SRC_PACK] != (bytes == 4 ? 2 : 1) ||
               !bytes ||
               setup[SGI_KONA_X_SETUP_PAD] != 0) {
        reason = "unsupported X read setup";
    } else if ((int32_t)width <= 0 || (int32_t)height <= 0 ||
               x < 0 || y < 0 || x + width > s->fb_width ||
               y + height > s->fb_height) {
        reason = "rectangle outside the main plane";
    } else if (pitch > limit || height > limit / pitch) {
        reason = "payload exceeds the transport capacity";
    } else {
        /*
         * Check every source before writing any part of the reply. A pixel
         * the display shows from bank 1 (see sgi_kona_displayed_bank()) is
         * not the bank this read answers from. A window's rectangle alone
         * does not hide bank 0: where another window covers it, or where
         * its client died and the X server repainted its display IDs, bank
         * 0 is what shows.
         */
        for (uint32_t r = 0; r < height; r++) {
            for (uint32_t c = 0; c < width; c++) {
                int64_t py = s->fb_height - 1 - y - r;
                unsigned bank = fb_word ?
                                sgi_kona_displayed_bank(s, x + c, py) : 0;
                uint32_t tag = sgi_plane_load(&s->mem,
                    sgi_kona_plane(s, sgi_kona_bank_tag_plane(s, bank)),
                    x + c, py);
                uint32_t word = sgi_plane_load(&s->mem,
                    sgi_kona_plane(s, sgi_kona_bank_plane(s, bank)),
                    x + c, py);

                bool rgb = bytes == 4 && !fb_word;
                uint32_t fb;

                if (bank != sgi_kona_displayed_bank(s, x + c, py)) {
                    reason = "bank 0 hidden by a displayed bank-1 window";
                    goto refused;
                }
                if (fb_word ?
                    !sgi_kona_x_read_fb_word(s->x_gc[8], tag, word, &fb) :
                    tag != (rgb ? SGI_KONA_TAG_DIRECT :
                                  SGI_KONA_TAG_X_INDEX16) ||
                    word > (rgb ? 0xffffff : 0xffff)) {
                    reason = "source is not supported main-plane colour";
                    goto refused;
                }
            }
        }
        data = g_malloc0(pitch * height);
        for (uint32_t r = 0; r < height; r++) {
            for (uint32_t c = 0; c < width; c++) {
                int64_t py = s->fb_height - 1 - y - r;
                /* A frame-buffer word reads the bank its pixel shows. */
                unsigned bank = fb_word ?
                                sgi_kona_displayed_bank(s, x + c, py) : 0;
                uint32_t word = sgi_plane_load(&s->mem,
                    sgi_kona_plane(s, sgi_kona_bank_plane(s, bank)),
                    x + c, py);

                if (fb_word) {
                    uint32_t tag = sgi_plane_load(&s->mem,
                        sgi_kona_plane(s, sgi_kona_bank_tag_plane(s, bank)),
                        x + c, py);
                    uint32_t fb = 0;

                    /* Checked above: see sgi_kona_x_read_fb_word(). */
                    sgi_kona_x_read_fb_word(s->x_gc[8], tag, word, &fb);
                    stl_be_p(data + r * pitch + c * 4, fb);
                } else if (bytes == 4) {
                    /* The model stores logical RGB, not physical components. */
                    stl_be_p(data + r * pitch + c * 4, word);
                } else if (bytes == 2) {
                    /* Physical halfword extraction remains inferred. */
                    stw_be_p(data + r * pitch + c * 2, (word >> 4) & 0xfff);
                } else {
                    data[r * pitch + c] = word >> 8;
                }
            }
        }
        if (fb_word && s->x_gc[8] == 1 && !s->fb_word_low_bits_reported) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: frame-buffer word read"
                          " answered with bits 15:0 zero, so an alpha the X"
                          " server asks for reads as 0 (reported once)\n");
            s->fb_word_low_bits_reported = true;
        }
        *out_data = g_steal_pointer(&data);
        *out_count = pitch * height;
        return true;
    }

refused:
    qemu_log_mask(LOG_UNIMP, "sgi-kona: X pixel read at (%" PRId64 ",%"
                  PRId64 ") setup %u %u %u %u %u %u %u GC %x %x %x %x %x %x"
                  " %x refused: %s; %s\n",
                  x, y, setup[0], setup[1], setup[2], setup[3], setup[4],
                  setup[5], setup[6], s->x_gc[6], s->x_gc[7], s->x_gc[8],
                  s->x_gc[9], s->x_gc[10], s->x_gc[11], s->x_gc[12], reason,
                  sgi_kona_x_read_current(s) ? "answered with zeros" :
                                               "not answered");
    return false;
}

/*
 * A refused X pixel read still completes. The X server's image read and the
 * frame-buffer word reader wait on word 0 of the return page with no
 * timeout (the command grammar's pixel-read reply), so a read the model
 * cannot answer would stop the X server for good. The reply is the whole
 * usable page zeroed, then the ready word: defined data, not pixels the
 * model has. Model policy; what the pipe answers to a read it cannot
 * perform is unknown. A read with no page this context owns is not
 * answered, so another context's page is never written.
 */
static void sgi_kona_x_read_answer_refused(SGIKonaState *s)
{
    g_autofree uint8_t *zeros = g_malloc0(SGI_KONA_RETURN_PAGE_BYTES - 8);

    if (sgi_kona_x_read_current(s)) {
        sgi_kona_post_return(s, SGI_KONA_CMD_X_PIXEL_READ, 8, zeros,
                             SGI_KONA_RETURN_PAGE_BYTES - 8);
    }
}

static void sgi_kona_x_pixel_read(SGIKonaState *s, const uint32_t *args)
{
    SGIKonaGLReadDMA *d = &s->gl_read_dma;
    const uint32_t *setup = s->x_pixel_setup;
    g_autofree uint8_t *data = NULL;
    uint32_t count;

    if (d->active) {
        uint32_t header[] = { args[0], args[1], setup[4], setup[5],
                              setup[2] == 0 ? 1 : setup[2] == 1 ? 2 :
                              setup[2] == 2 ? 4 : 0, 0, 0 };

        if (d->refused || d->count || d->context != s->context_current ||
        !sgi_kona_context_owner(s, d->context, d->generation) ||
            !sgi_kona_gl_read_dma_layout(d, header,
                                            SGI_KONA_CMD_X_PIXEL_READ)) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: X read DMA refused\n");
            sgi_kona_gl_read_dma_refuse(s);
            return;
        }
        if (!sgi_kona_x_read_payload(s, args, 64 * MiB, &data, &count)) {
            if (!sgi_kona_x_read_current(s)) {
                qemu_log_mask(LOG_UNIMP, "sgi-kona: X read DMA refused\n");
                sgi_kona_gl_read_dma_refuse(s);
                return;
            }
            /*
             * As for the programmed-I/O reply, a refused source still
             * completes: the bracket's descriptor already fixes the
             * byte count, and those bytes are written as zeros.
             */
            count = d->descriptor[2];
            data = g_malloc0(count);
        }
        memcpy(d->header, header, sizeof(d->header));
        d->selector = SGI_KONA_CMD_X_PIXEL_READ;
        d->count = count;
        d->data = g_steal_pointer(&data);
    } else if (sgi_kona_x_read_payload(s, args, SGI_KONA_RETURN_PAGE_BYTES - 8,
                                       &data, &count)) {
        sgi_kona_post_return(s, SGI_KONA_CMD_X_PIXEL_READ, 8, data, count);
    } else {
        sgi_kona_x_read_answer_refused(s);
    }
}

/* The setup's source-packing words for the two one-bit forms. */
#define SGI_KONA_X_PACK_BITMAP        4
#define SGI_KONA_X_PACK_OPAQUE_BITMAP 5

/*
 * The bits per pixel of a write's data, from the setup's source-packing word,
 * or 0 where it is not known. Inferred from the KONA X driver module's 8-, 16-
 * and 32-bit source packing tables, which hold 0, 1, 2 at index 0: 0, 1 and 2
 * are 8, 16 and 32 bits. 4 is the glyph senders' constant, and the greeter's
 * glyph writes are established to be 1-bit rows (20 words for a 32 by 20 image
 * spelling "Help"). 5 is the opaque one-bit image sender's, inferred to be one
 * bit a pixel as well. See sgi_kona_x_bitmap_index() for what their bits paint.
 */
static uint32_t sgi_kona_x_pixel_bpp(uint32_t src_pack)
{
    switch (src_pack) {
    case 0:
        return 8;
    case 1:
        return 16;
    case 2:
        return 32;
    case SGI_KONA_X_PACK_BITMAP:
    case SGI_KONA_X_PACK_OPAQUE_BITMAP:
        return 1;
    default:
        return 0;
    }
}

/*
 * The X colour index a one-bit write's bit paints, or false for a bit
 * that leaves its pixel unchanged. With depth d and
 * v = index << (16 - d):
 *
 *   source packing 4 (the glyph senders and the one-bit image sender):
 *   GC word 1 is
 *   P(v) = ((v >> 4) & 0xfff) | ((v & 0xf) << 20), so a set bit paints
 *   (((w1 & 0xfff) << 4) | ((w1 >> 20) & 0xf)) >> (16 - d), and a clear
 *   bit nothing. Word 0 is the constant 0xfff0 and is not read.
 *
 *   source packing 5 (the opaque one-bit image sender): a set bit paints GC
 *   word 0 read as v, a clear bit GC word 2 read as v.
 *
 * Approximated: which words each sender fills is established, and what
 * the pipe paints from them is inferred from that alone; the pipe was not
 * observed. This trace helper retains its historical eight-bit projection.
 * discriminating tests are the glyph qtests for one-bit glyph colour and
 * opaque mono images, and in the guest the greeter's button labels
 * appearing, black through entry 0x2d00 under the run's foreground of 0.
 */
static bool sgi_kona_x_bitmap_index(SGIKonaState *s, bool bit,
                                    uint32_t *index)
{
    uint32_t shift = 16 - SGI_KONA_X_TRACE_INDEX_DEPTH;

    if (s->x_pixel_setup[SGI_KONA_X_SETUP_SRC_PACK] ==
        SGI_KONA_X_PACK_OPAQUE_BITMAP) {
        *index = (bit ? s->x_gc[0] : s->x_gc[2]) >> shift;
        return true;
    }
    *index = sgi_kona_x_packed_index(s->x_gc[1]);
    return bit;
}

/* Snapshot geometry bounds all staged coordinates, including clipped holes. */
static void sgi_kona_x_write_bounds(SGIKonaState *s, SGIKonaRect *r)
{
    *r = (SGIKonaRect) { s->x_write_x1, s->x_write_y1,
                         s->x_write_x2, s->x_write_y2 };
}

static void sgi_kona_x_write_extend(SGIKonaState *s)
{
    int64_t x1 = CLAMP((int64_t)s->x_pixel_x, 0, s->fb_width);
    int64_t y1 = CLAMP((int64_t)s->x_pixel_y, 0, s->fb_height);
    int64_t x2 = CLAMP((int64_t)s->x_pixel_x +
                       s->x_pixel_setup[SGI_KONA_X_SETUP_WIDTH],
                       0, s->fb_width);
    int64_t y2 = CLAMP((int64_t)s->x_pixel_y +
                       s->x_pixel_setup[SGI_KONA_X_SETUP_HEIGHT],
                       0, s->fb_height);

    if (x1 >= x2 || y1 >= y2) {
        return;
    }
    if (s->x_write_x1 == s->x_write_x2) {
        s->x_write_x1 = x1;
        s->x_write_y1 = y1;
        s->x_write_x2 = x2;
        s->x_write_y2 = y2;
    } else {
        s->x_write_x1 = MIN(s->x_write_x1, x1);
        s->x_write_y1 = MIN(s->x_write_y1, y1);
        s->x_write_x2 = MAX(s->x_write_x2, x2);
        s->x_write_y2 = MAX(s->x_write_y2, y2);
    }
}

static void sgi_kona_x_write_clear(SGIKonaState *s)
{
    if (s->x_write_snapshot) {
        const SGIPlane *source = sgi_board_memory_plane(&s->x_write_mem, 0);
        const SGIPlane *valid = sgi_board_memory_plane(&s->x_write_mem, 1);
        const SGIPlane *ones = sgi_board_memory_plane(&s->x_write_mem, 2);
        SGIKonaRect r;

        sgi_kona_x_write_bounds(s, &r);
        for (int64_t y = r.y1; y < r.y2; y++) {
            const SGIPlane *planes[] = { source, valid, ones };

            /* Zero is all zero bytes in every packing: clear rows whole. */
            for (unsigned i = 0; i < ARRAY_SIZE(planes); i++) {
                SGIPlaneRun run;

                sgi_kona_x_row(&s->x_write_mem, planes[i], r.x1, r.x2, y,
                               &run);
                if (run.host) {
                    memset(run.host, 0, (r.x2 - r.x1) * run.bpp);
                    continue;
                }
                for (int64_t x = r.x1; x < r.x2; x++) {
                    sgi_plane_store(&s->x_write_mem, planes[i], x, y, 0);
                }
            }
        }
    }
    s->x_write_snapshot = false;
    s->x_write_refused = false;
    s->x_write_multi = false;
    s->x_write_x1 = s->x_write_y1 = 0;
    s->x_write_x2 = s->x_write_y2 = 0;
    memset(s->x_write_gc, 0, sizeof(s->x_write_gc));
    memset(s->x_write_setup, 0, sizeof(s->x_write_setup));
    s->x_write_clip_count = 0;
    memset(s->x_write_clip_rects, 0, sizeof(s->x_write_clip_rects));
    s->x_write_scissor_valid = false;
    memset(s->x_write_scissor, 0, sizeof(s->x_write_scissor));
    s->x_write_stipple_width = s->x_write_stipple_height = 0;
    s->x_write_stipple_x = s->x_write_stipple_y = 0;
    memset(s->x_write_stipple_rows, 0, sizeof(s->x_write_stipple_rows));
}

/*
 * A glyph write (source packing 4) into a 24-bit TrueColor window: the KONA X
 * driver module's GC parser, for that format, widens each colour byte to twelve
 * bits as b << 4 | b >> 4 and stores word 0 as 0xfff0 with the blue value's top
 * four bits and word 1 as its low eight bits, then green, then red: blue[11:8]
 * in word 0 bits 3:0, blue[7:0] in word 1 bits 31:24, green in 23:12 and red in
 * 11:0 (established from the code). That a set bit paints that colour and a
 * clear bit nothing, as in a colour-index window, is inferred. Rewrite the pair
 * into the colour-word form the solid fills use, with alpha all ones, so the
 * shared decoder reads it.
 *
 * A 30-bit TrueColor window's glyphs come with cfg 0 and op 0x64001, and the
 * KONA X driver module's GC parser stores the same layout with each 10-bit
 * component widened as c << 2 | c >> 8 (established from the code); the
 * rewritten pair then carries ifmt 9, the 30-bit form.
 */
static bool sgi_kona_x_rgb_glyph_gc(uint32_t *raw)
{
    uint64_t red, green, blue, colour;
    bool rgb30 = raw[6] == 0;

    if ((raw[0] & ~0xfU) != 0xfff0 || raw[2] || raw[3] || raw[10]) {
        return false;
    }
    red = raw[1] & 0xfff;
    green = (raw[1] >> 12) & 0xfff;
    blue = (uint64_t)(raw[0] & 0xf) << 8 | raw[1] >> 24;
    colour = red << 40 | green << 27 | blue << 14 | 0x1ffe;

    raw[0] = colour >> 32;
    raw[1] = (uint32_t)colour;
    raw[10] = rgb30 ? 9 : 8;
    return true;
}

/* Bitmap source colour fields retain their separately reviewed extraction. */
static bool sgi_kona_x_write_decode_words(SGIKonaState *s, SGIXGC *gc)
{
    uint32_t raw[SGI_KONA_X_GC_WORDS];
    const uint32_t *setup = s->x_write_setup;
    uint32_t pack = setup[SGI_KONA_X_SETUP_SRC_PACK];
    bool bitmap = pack == SGI_KONA_X_PACK_BITMAP ||
                  pack == SGI_KONA_X_PACK_OPAQUE_BITMAP;
    bool geometry, popup;

    memcpy(raw, s->x_write_gc, sizeof(raw));
    if (setup[SGI_KONA_X_SETUP_MODE] & ~SGI_KONA_X_SETUP_MODE_DMA) {
        return false;
    }
    if (pack == 1) {
        return setup[SGI_KONA_X_SETUP_DST_PACK] == 1 &&
               setup[SGI_KONA_X_SETUP_SHIFT] == 4 &&
               sgi_x_gc_decode_main_index(raw, gc) == SGI_X_GC_OK &&
               raw[10] == 5 && !gc->opaque_or_tiled;
    }
    if (pack == 2) {
        return setup[SGI_KONA_X_SETUP_DST_PACK] == 2 &&
               setup[SGI_KONA_X_SETUP_SHIFT] == 8 &&
               sgi_x_gc_decode(raw, gc) == SGI_X_GC_OK &&
               gc->target == SGI_X_GC_MAIN_RGB8 && raw[10] == 8 &&
               !gc->opaque_or_tiled;
    }
    /*
     * A second buffer selector of 2 turns cfg 0x2804 into 0x3804 (the
     * command grammar's GC table); the solid fills already take both, and
     * so does the glyph form here, drawn where the fills draw. A file
     * manager's icon labels come this way once its window's X buffer
     * selection moves to 2.
     */
    if (pack == SGI_KONA_X_PACK_BITMAP &&
        (raw[6] == 0x2804 || raw[6] == 0x3804 ||
         (raw[6] == 0 && raw[7] == 0x64001))) {
        return setup[SGI_KONA_X_SETUP_DST_PACK] == 2 &&
               !setup[SGI_KONA_X_SETUP_SHIFT] &&
               sgi_kona_x_rgb_glyph_gc(raw) &&
               sgi_x_gc_decode(raw, gc) == SGI_X_GC_OK &&
               gc->target == SGI_X_GC_MAIN_RGB8 && !gc->opaque_or_tiled;
    }
    /*
     * An 8-bit image into the popup planes, as a Toolchest menu draws its
     * 14 by 14 marks: cfg 0x1020, ifmt 5, destination packing 1 and no
     * shift. The popup colour form takes the pixel unshifted (v = p), so
     * each byte is the popup pixel, written under the GC's two-bit mask
     * (inferred from that form and the zero shift).
     */
    if (pack == 0 && raw[6] == 0x1020) {
        return setup[SGI_KONA_X_SETUP_DST_PACK] == 1 &&
               !setup[SGI_KONA_X_SETUP_SHIFT] &&
               sgi_x_gc_decode(raw, gc) == SGI_X_GC_OK &&
               gc->target == SGI_X_GC_POPUP && !gc->opaque_or_tiled;
    }
    if (setup[SGI_KONA_X_SETUP_DST_PACK] != 1 ||
        setup[SGI_KONA_X_SETUP_SHIFT] != (bitmap ? 0 : 8) ||
        (!bitmap && pack != 0)) {
        return false;
    }
    /*
     * The one-bit image sender's GC loader sends the glyph form's words 0
     * to 3 with the geometry configuration instead: cfg 0xc, op the
     * format's low nibble (1), ifmt 0. The shared decoder reads that as a
     * geometry GC, so its ifmt stays 0.
     */
    geometry = pack == SGI_KONA_X_PACK_BITMAP && raw[6] == 0xc;
    /*
     * A glyph into the popup planes, as the Toolchest's menus draw their
     * entries: cfg 0x1820, the glyph form's words 0 and 1 with the popup
     * pixel unshifted in P(v) (the GC parser's index case without the depth
     * shift), write mask (pm & 3) << 20. It lands in the popup planes as a
     * popup fill does.
     */
    popup = pack == SGI_KONA_X_PACK_BITMAP && raw[6] == 0x1820;
    if (bitmap) {
        if ((raw[6] != 0x180c && !geometry && !popup) ||
            raw[10] != (pack == SGI_KONA_X_PACK_BITMAP ? 0 : 5)) {
            return false;
        }
        if (pack == SGI_KONA_X_PACK_BITMAP) {
            if (raw[0] != 0xfff0 || raw[1] & ~0xf00fffU ||
                raw[2] || raw[3]) {
                return false;
            }
            uint32_t value = ((raw[1] & 0xfff) << 4) | (raw[1] >> 20);

            raw[0] = ((value & 0xfff0) << 4) | ((value & 15) << 3);
            raw[2] = 0;
        } else {
            if ((raw[0] | raw[2]) & ~0xffffU || raw[1] || raw[3]) {
                return false;
            }
            raw[0] = ((raw[0] & 0xfff0) << 4) | ((raw[0] & 15) << 3);
            raw[2] = ((raw[2] & 0xfff0) << 4) | ((raw[2] & 15) << 3);
        }
        raw[1] = raw[3] = 0;
        raw[10] = geometry ? 0 : 5;
    }
    if (popup) {
        return sgi_x_gc_decode(raw, gc) == SGI_X_GC_OK &&
               gc->target == SGI_X_GC_POPUP && !gc->opaque_or_tiled;
    }
    return sgi_x_gc_decode_main_index(raw, gc) == SGI_X_GC_OK &&
           !gc->opaque_or_tiled;
}

/*
 * The decode depends on the two snapshots alone, so a repeat returns what
 * the last one gave, the GC it filled in included.
 */
static bool sgi_kona_x_write_decode(SGIKonaState *s, SGIXGC *gc)
{
    if (!s->x_write_decoded_valid ||
        memcmp(s->x_write_decoded_gc, s->x_write_gc,
               sizeof(s->x_write_gc)) ||
        memcmp(s->x_write_decoded_setup, s->x_write_setup,
               sizeof(s->x_write_setup))) {
        memset(&s->x_write_decoded, 0, sizeof(s->x_write_decoded));
        s->x_write_decoded_ok = sgi_kona_x_write_decode_words(
            s, &s->x_write_decoded);
        memcpy(s->x_write_decoded_gc, s->x_write_gc, sizeof(s->x_write_gc));
        memcpy(s->x_write_decoded_setup, s->x_write_setup,
               sizeof(s->x_write_setup));
        s->x_write_decoded_valid = true;
    }
    *gc = s->x_write_decoded;
    return s->x_write_decoded_ok;
}

static bool sgi_kona_x_write_contains(SGIKonaState *s, int64_t x, int64_t y)
{
    bool clipped = !s->x_write_clip_count;

    if (x < 0 || x >= s->fb_width ||
        y < 0 || y >= s->fb_height) {
        return false;
    }
    for (uint32_t i = 0; i < s->x_write_clip_count; i++) {
        const uint32_t *r = s->x_write_clip_rects + i * 4;

        if (x >= (int32_t)r[0] && y >= (int32_t)r[1] &&
            x < (int32_t)r[2] && y < (int32_t)r[3]) {
            clipped = true;
            break;
        }
    }
    if (!clipped) {
        return false;
    }
    if (s->x_write_scissor_valid) {
        int64_t sx = (int32_t)s->x_write_scissor[0];
        int64_t sy = (int32_t)s->x_write_scissor[1];

        if (x < sx || y < sy || x >= sx + s->x_write_scissor[2] ||
            y >= sy + s->x_write_scissor[3]) {
            return false;
        }
    }
    if (s->x_write_stipple_width) {
        uint32_t width = s->x_write_stipple_width;
        uint32_t height = s->x_write_stipple_height;
        int64_t cx = x - s->x_write_stipple_x;
        int64_t cy = y - s->x_write_stipple_y;
        uint32_t row = s->x_write_stipple_rows[((cy % height) + height) %
                                               height];

        return row & (0x80000000u >> (((cx % width) + width) % width));
    }
    return true;
}

static bool sgi_kona_x_write_same_state(SGIKonaState *s)
{
    if (memcmp(s->x_write_gc, s->x_gc, sizeof(s->x_gc)) ||
        memcmp(s->x_write_setup, s->x_pixel_setup,
               sizeof(s->x_pixel_setup)) ||
        s->x_write_clip_count != s->x_clip_count ||
        memcmp(s->x_write_clip_rects, s->x_clip_rects,
               sizeof(s->x_clip_rects)) ||
        s->x_write_scissor_valid != s->x_scissor_valid ||
        memcmp(s->x_write_scissor, s->x_scissor,
               sizeof(s->x_scissor))) {
        return false;
    }
    if (s->x_stipple_loaded >= SGI_KONA_X_STIPPLE_ENTRIES) {
        return !s->x_write_stipple_width;
    }

    unsigned entry = s->x_stipple_loaded;

    return s->x_write_stipple_width == s->x_stipple_width[entry] &&
           s->x_write_stipple_height == s->x_stipple_height[entry] &&
           s->x_write_stipple_x ==
               (int64_t)(int32_t)s->x_position[0] +
               s->x_stipple_origin_x[entry] &&
           s->x_write_stipple_y ==
               (int64_t)(int32_t)s->x_position[1] +
               s->x_stipple_origin_y[entry] &&
           !memcmp(s->x_write_stipple_rows,
                   s->x_stipple_rows + entry * SGI_KONA_X_STIPPLE_MAX,
                   sizeof(s->x_write_stipple_rows));
}

/* Bank-zero source staging ends at the established X finish boundary. */
static void sgi_kona_x_pixel_write(SGIKonaState *s, const uint32_t *args)
{
    uint32_t src_pack = s->x_pixel_setup[SGI_KONA_X_SETUP_SRC_PACK];
    uint64_t width = s->x_pixel_setup[SGI_KONA_X_SETUP_WIDTH];
    uint64_t height = s->x_pixel_setup[SGI_KONA_X_SETUP_HEIGHT];
    uint64_t pad = s->x_pixel_setup[SGI_KONA_X_SETUP_PAD];
    uint32_t bpp = sgi_kona_x_pixel_bpp(src_pack);
    uint64_t stride = 0, words = 0;
    SGIXGC gc;
    bool refuse_new = false;

    if (s->x_pixel_open && !s->x_pixel_words && !s->x_write_multi) {
        s->x_pixel_open = false;
    }
    if (s->x_pixel_open) {
        if (s->x_pixel_remaining) {
            sgi_kona_x_read_invalidate(s);
            refuse_new = true;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: X pixel write while another still"
                          " needs %u words; batch refused\n",
                          s->x_pixel_remaining);
        } else if (!sgi_kona_x_write_same_state(s)) {
            refuse_new = true;
            qemu_log_mask(LOG_UNIMP,
                          "sgi-kona: X pixel batch changes drawing state;"
                          " batch refused\n");
        }
        refuse_new |= s->x_write_refused;
        if (refuse_new) {
            s->x_pixel_open = false;
        } else {
            s->x_write_multi = true;
        }
    }
    if (!s->x_pixel_open) {
        sgi_kona_x_write_clear(s);
        s->x_write_snapshot = true;
        s->x_write_refused = refuse_new;
        memcpy(s->x_write_gc, s->x_gc, sizeof(s->x_write_gc));
        memcpy(s->x_write_setup, s->x_pixel_setup, sizeof(s->x_write_setup));
        s->x_write_clip_count = s->x_clip_count;
        memcpy(s->x_write_clip_rects, s->x_clip_rects,
               sizeof(s->x_write_clip_rects));
        s->x_write_scissor_valid = s->x_scissor_valid;
        memcpy(s->x_write_scissor, s->x_scissor,
               sizeof(s->x_write_scissor));
        if (s->x_stipple_loaded < SGI_KONA_X_STIPPLE_ENTRIES) {
            unsigned entry = s->x_stipple_loaded;

            s->x_write_stipple_width = s->x_stipple_width[entry];
            s->x_write_stipple_height = s->x_stipple_height[entry];
            s->x_write_stipple_x = (int64_t)(int32_t)s->x_position[0] +
                                   s->x_stipple_origin_x[entry];
            s->x_write_stipple_y = (int64_t)(int32_t)s->x_position[1] +
                                   s->x_stipple_origin_y[entry];
            memcpy(s->x_write_stipple_rows,
                   s->x_stipple_rows + entry * SGI_KONA_X_STIPPLE_MAX,
                   sizeof(s->x_write_stipple_rows));
        }
    }
    if (!sgi_kona_x_write_decode(s, &gc) ||
        (s->x_write_stipple_width && !s->x_write_stipple_height)) {
        s->x_write_refused = true;
        qemu_log_mask(LOG_UNIMP, "sgi-kona: unsupported X streamed write GC:");
        for (unsigned i = 0; i < SGI_KONA_X_GC_WORDS; i++) {
            qemu_log_mask(LOG_UNIMP, " %08x", s->x_write_gc[i]);
        }
        qemu_log_mask(LOG_UNIMP, " setup:");
        for (unsigned i = 0; i < SGI_KONA_X_PIXEL_SETUP_WORDS; i++) {
            qemu_log_mask(LOG_UNIMP, " %08x", s->x_write_setup[i]);
        }
        qemu_log_mask(LOG_UNIMP, "; complete known payload is consumed\n");
    }
    s->x_pixel_open = true;
    s->x_pixel_x = args[0];
    s->x_pixel_y = args[1];
    sgi_kona_x_write_extend(s);
    s->x_pixel_bpp = bpp;
    s->x_pixel_prev = 0;

    if (!bpp) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: X pixel write with source packing %" PRIu32
                      "; its data cannot be framed\n", src_pack);
    } else {
        stride = DIV_ROUND_UP(pad + bpp * width, 32);
        if (stride && height > UINT32_MAX / stride) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: X pixel write geometry exceeds "
                          "the supported word count\n");
            s->x_write_refused = true;
            stride = words = 0;
        } else {
            words = stride * height;
        }
    }
    s->x_pixel_stride = stride;
    s->x_pixel_words = words;
    s->x_pixel_remaining = words;
    s->x_pixel_owner = s->context_current;
    s->x_pixel_owner_generation =
        sgi_kona_context_generation(s, s->context_current);
    trace_sgi_kona_x_pixel_write(s->x_pixel_x, s->x_pixel_y, bpp, words);
    if (bpp == 1) {
        uint32_t set, clear;
        bool opaque = sgi_kona_x_bitmap_index(s, false, &clear);

        sgi_kona_x_bitmap_index(s, true, &set);
        trace_sgi_kona_x_pixel_bitmap(src_pack, set, opaque, clear);
    }
}

/* Retain only visible source pixels, without touching canonical storage. */
static void sgi_kona_x_pixel_store(SGIKonaState *s, const SGIXGC *gc,
                                   uint64_t i, uint64_t row, uint32_t value)
{
    int64_t x = (int64_t)s->x_pixel_x + i;
    int64_t y = (int64_t)s->x_pixel_y + row;
    const SGIPlane *source = sgi_board_memory_plane(&s->x_write_mem, 0);
    const SGIPlane *valid = sgi_board_memory_plane(&s->x_write_mem, 1);
    const SGIPlane *ones = sgi_board_memory_plane(&s->x_write_mem, 2);
    uint32_t pack = s->x_write_setup[SGI_KONA_X_SETUP_SRC_PACK];

    if (!sgi_kona_x_write_contains(s, x, y)) {
        return;
    }
    if (s->x_pixel_bpp == 1) {
        if (!value && pack == SGI_KONA_X_PACK_BITMAP) {
            return;
        }
        value = value ? gc->foreground : gc->background;
    }
    /* X RGB pixels occupy the low 24 bits of each big-endian word. */
    if (gc->target == SGI_X_GC_MAIN_RGB8) {
        value &= 0xffffff;
    } else if (gc->target == SGI_X_GC_POPUP) {
        /* The popup planes hold two bits. */
        value &= gc->width_mask;
    } else if (gc->target == SGI_X_GC_MAIN_INDEX16 &&
               s->x_pixel_bpp != 1) {
        value = (value << s->x_write_setup[SGI_KONA_X_SETUP_SHIFT]) & 0xffff;
    }
    /* Boolean GX operations compose per bit across writes before finish. */
    uint32_t zero = 0, one = gc->width_mask;

    if (sgi_plane_load(&s->x_write_mem, valid, x, y)) {
        zero = sgi_plane_load(&s->x_write_mem, source, x, y);
        one = sgi_plane_load(&s->x_write_mem, ones, x, y);
    }
    sgi_plane_store(&s->x_write_mem, source, x, y,
                    sgi_x_gc_apply(gc, value, zero));
    sgi_plane_store(&s->x_write_mem, ones, x, y,
                    sgi_x_gc_apply(gc, value, one));
    sgi_plane_store(&s->x_write_mem, valid, x, y, 1);
}

/*
 * A data word of an open pixel write: word w of its row holds row bits
 * 32w to 32w + 31, most significant first, and pixel i occupies bits
 * pad + bpp * i onward. Each pixel ending in this word is staged; one
 * starting in the previous word takes its high bits from x_pixel_prev.
 */
static void sgi_kona_x_pixel_word(SGIKonaState *s, uint32_t word)
{
    uint64_t k = s->x_pixel_words - s->x_pixel_remaining;
    uint64_t row = k / s->x_pixel_stride;
    uint64_t w = k % s->x_pixel_stride;
    const uint32_t *setup = s->x_write_snapshot ? s->x_write_setup :
                                                s->x_pixel_setup;
    uint64_t pad = setup[SGI_KONA_X_SETUP_PAD];
    uint64_t width = setup[SGI_KONA_X_SETUP_WIDTH];
    SGIXGC gc;
    uint32_t bpp = s->x_pixel_bpp;

    if (w == 0) {
        s->x_pixel_prev = 0;
    }
    if (bpp && s->x_write_snapshot && !s->x_write_refused &&
        sgi_kona_x_write_decode(s, &gc) && !sgi_x_gc_noop(&gc)) {
        uint64_t window = ((uint64_t)s->x_pixel_prev << 32) | word;
        uint64_t mask = bpp == 32 ? UINT32_MAX : (1u << bpp) - 1;
        uint64_t first = 32 * w;
        uint64_t i = first > pad ? (first - pad) / bpp : 0;
        for (; i < width; i++) {
            uint64_t end = pad + bpp * (i + 1);

            if (end > first + 32) {
                break;
            }
            if (end > first) {
                sgi_kona_x_pixel_store(s, &gc, i, row,
                                       (window >> (first + 32 - end)) & mask);
            }
        }
    }
    s->x_pixel_prev = word;
    if (--s->x_pixel_remaining == 0) {
        trace_sgi_kona_x_pixel_write_done(s->x_pixel_words);
    }
}

/*
 * Whether the current context's words are part of an open X pixel write:
 * the write's owner is current. Another context's words are its own.
 */
bool sgi_kona_x_pixel_owned(SGIKonaState *s)
{
    return s->x_pixel_owner == s->context_current;
}

/*
 * Whether the open X pixel write waits for its rows by DMA: the setup's
 * mode word carries SGI_KONA_X_SETUP_MODE_DMA, the one word that says so
 * (established from the X server's image write), and rows are still to
 * come.
 */
bool sgi_kona_x_pixel_dma_waiting(SGIKonaState *s)
{
    const uint32_t *setup = s->x_write_snapshot ? s->x_write_setup :
                                                s->x_pixel_setup;

    return s->x_pixel_open && s->x_pixel_remaining &&
           (setup[SGI_KONA_X_SETUP_MODE] & SGI_KONA_X_SETUP_MODE_DMA);
}

/*
 * The host address of device address `device`: entry device >> 14 of the
 * XG map RAM maps its 16 KiB page, the entry's frame the host page's 4 KiB
 * frame number. False at an entry that maps nothing, which the caller
 * logs.
 */
bool sgi_kona_device_address(SGIKonaState *s, uint64_t device,
                             uint64_t *host)
{
    const uint64_t page = 1u << SGI_KONA_MAPRAM_PAGE_SHIFT;
    uint64_t index = device >> SGI_KONA_MAPRAM_PAGE_SHIFT;
    uint32_t entry = index < sgi_kona_mapram_entry_count(s) ?
                     s->map_ram[index] : 0;

    if ((entry & SGI_KONA_MAPRAM_KIND_MASK) != SGI_KONA_MAPRAM_KIND_16K) {
        return false;
    }
    *host = ((uint64_t)(entry & SGI_KONA_PAGE_WORD_FRAME) << 12) +
            (device & (page - 1));
    return true;
}

/*
 * len bytes at device address `device`, through the XG map RAM: entry
 * device >> 14 maps the 16 KiB page, its frame the host page's 4 KiB
 * frame number, as the kernel fills the entries before the packet. False,
 * logged, at an entry that maps nothing or a read that fails.
 */
static bool sgi_kona_device_read(SGIKonaState *s, uint64_t device,
                                 uint8_t *buf, uint32_t len)
{
    const uint64_t page = 1u << SGI_KONA_MAPRAM_PAGE_SHIFT;

    while (len) {
        uint64_t index = device >> SGI_KONA_MAPRAM_PAGE_SHIFT;
        uint32_t entry = index < sgi_kona_mapram_entry_count(s) ?
                         s->map_ram[index] : 0;
        uint32_t chunk = MIN(MIN(len, 4096), page - (device & (page - 1)));
        uint64_t host;
        hwaddr where;

        if ((entry & SGI_KONA_MAPRAM_KIND_MASK) != SGI_KONA_MAPRAM_KIND_16K) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: DMA read at device address 0x%" PRIx64
                          " through map RAM entry 0x%08x, which maps"
                          " nothing\n", device, entry);
            s->dma_read_fault = true;
            return false;
        }
        host = ((uint64_t)(entry & SGI_KONA_PAGE_WORD_FRAME) << 12) +
               (device & (page - 1));
        if (!sgi_kona_fabric_compose(s, s->fabric_hub_widget, host,
                                     &where) ||
            dma_memory_read(&s->fabric_as, where, buf, chunk,
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: DMA read of %u bytes at host 0x%" PRIx64
                          " failed\n", chunk, host);
            s->dma_read_fault = true;
            return false;
        }
        if (trace_event_get_state_backends(TRACE_SGI_KONA_DMA_CHUNK)) {
            char hex[4096 * 2 + 1];

            qemu_hexdump_to_buffer(hex, sizeof(hex), buf, chunk);
            trace_sgi_kona_dma_chunk(s->trace_sequence - 1, host, chunk, hex);
        }
        device += chunk;
        buf += chunk;
        len -= chunk;
    }
    return true;
}

/* Use the same map RAM and fabric as input DMA, splitting at page edges. */
static bool sgi_kona_device_write(SGIKonaState *s, uint64_t device,
                                   const uint8_t *buf, uint32_t len,
                                   bool validate)
{
    const uint64_t page = 1u << SGI_KONA_MAPRAM_PAGE_SHIFT;

    while (len) {
        uint64_t host;
        hwaddr where;
        bool mapped;
        uint32_t chunk = MIN(MIN(len, 4096), page - (device & (page - 1)));

        if ((device >> SGI_KONA_MAPRAM_PAGE_SHIFT) >=
            sgi_kona_mapram_entry_count(s) ||
            !sgi_kona_command_page(s, "DMA write", device >> 12,
                                    device & 0xfff, &host, &mapped) ||
            !mapped ||
            !sgi_kona_fabric_compose(s, s->fabric_hub_widget, host, &where) ||
            (validate ? !dma_memory_valid(&s->fabric_as, where, chunk,
                                           DMA_DIRECTION_FROM_DEVICE,
                                           MEMTXATTRS_UNSPECIFIED) :
             !sgi_kona_host_write(s, host, buf, chunk))) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: DMA write of %u bytes"
                          " at device address 0x%" PRIx64 " failed\n",
                          chunk, device);
            return false;
        }
        device += chunk;
        if (!validate) {
            buf += chunk;
        }
        len -= chunk;
    }
    return true;
}

bool sgi_kona_pixels_dma_waiting(SGIKonaState *s)
{
    const SGIKonaGLImageUpload *u = &s->gl_image_upload;

    return u->count && u->draw_pixels && u->header[5] == 1;
}

bool sgi_kona_pixels_image_dma(SGIKonaState *s, uint64_t io, uint32_t lines,
                                uint32_t per_line, uint64_t pitch)
{
    SGIKonaGLImageUpload *u = &s->gl_image_upload;
    SGIKonaGLPixelDMALayout d;
    uint8_t chunk[4096];

    if (!sgi_kona_pixels_dma_waiting(s) || u->refused ||
        u->context != s->context_current ||
        !sgi_kona_context_owner(s, u->context, u->generation) ||
        !sgi_kona_gl_pixels_dma_layout(u->header, u->row_length,
                                         u->skip_rows, u->skip_pixels,
                                         u->alignment, &d) ||
        (io & 3) != d.source_phase || lines != d.lines ||
        per_line != d.line_bytes || pitch != d.pitch ||
        d.span > UINT64_MAX - io) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: DrawPixels DMA source framing mismatch;"
                      " image refused\n");
        sgi_kona_gl_image_clear(s);
        return false;
    }
    for (uint32_t line = 0; line < lines; line++) {
        for (uint32_t offset = 0; offset < per_line;) {
            uint32_t n = MIN(sizeof(chunk), per_line - offset);

            if (!sgi_kona_device_read(s, io + line * pitch + offset,
                                       chunk, n)) {
                sgi_kona_gl_image_clear(s);
                return false;
            }
            if (u->header[3] == 7 && !d.pixels.strided) {
                sgi_kona_gl_words_or_bytes(u->words, offset, chunk, n);
                offset += n;
                continue;
            }
            /*
             * Each run is the useful part of one row, at consecutive
             * staging offsets: when its last byte has a place, all do.
             */
            for (uint32_t k = 0; k < n;) {
                uint32_t row = d.pixels.strided ? line :
                               (offset + k) / d.pixels.stride;
                uint32_t col = d.pixels.strided ? offset + k :
                               (offset + k) % d.pixels.stride;
                uint32_t run = d.pixels.strided ? n - k :
                               MIN(n - k, d.pixels.stride - col);
                uint64_t at, last, useful;

                if (col < d.pixels.row_bytes) {
                    useful = MIN(run, d.pixels.row_bytes - col);
                    if (!sgi_kona_gl_pixels_pio_offset(&d.pixels, row,
                                                        col + useful - 1,
                                                        &last) ||
                        last / 4 >= u->count ||
                        !sgi_kona_gl_pixels_pio_offset(&d.pixels, row, col,
                                                        &at)) {
                        sgi_kona_gl_image_clear(s);
                        return false;
                    }
                    sgi_kona_gl_words_or_bytes(u->words, at, chunk + k,
                                               useful);
                }
                k += run;
            }
            offset += n;
        }
    }
    return sgi_kona_gl_pixels_done(s);
}

bool sgi_kona_bitmap_dma_waiting(SGIKonaState *s)
{
    const SGIKonaGLImageUpload *u = &s->gl_image_upload;

    return u->count && !u->draw_pixels && u->header[3] == 1;
}

bool sgi_kona_bitmap_image_dma(SGIKonaState *s, uint64_t io, uint32_t lines,
                                uint32_t per_line, uint64_t pitch)
{
    SGIKonaGLImageUpload *u = &s->gl_image_upload;
    SGIKonaGLBitmapDMALayout d;
    uint8_t chunk[4096];

    if (!sgi_kona_bitmap_dma_waiting(s) || u->refused ||
        u->context != s->context_current ||
        !sgi_kona_context_owner(s, u->context, u->generation) ||
        !sgi_kona_gl_bitmap_dma_layout(u->header, u->row_length,
                                         u->skip_rows, u->skip_pixels,
                                         u->alignment, &d) ||
        (io & 3) != d.source_phase || lines != d.lines ||
        per_line != d.line_bytes || pitch != d.pitch ||
        d.span > UINT64_MAX - io) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: raw bitmap DMA source framing mismatch;"
                      " image refused\n");
        sgi_kona_gl_image_clear(s);
        return false;
    }
    for (uint32_t line = 0; line < lines; line++) {
        for (uint32_t offset = 0; offset < per_line;) {
            uint32_t n = MIN(sizeof(chunk), per_line - offset);

            if (!sgi_kona_device_read(s, io + line * pitch + offset,
                                       chunk, n)) {
                sgi_kona_gl_image_clear(s);
                return false;
            }
            if (!d.bitmap.strided) {
                for (uint32_t k = 0; k < n; k += 4) {
                    u->words[(offset + k) / 4] = ldl_be_p(chunk + k);
                }
            } else {
                for (uint32_t k = 0; k < n; k++) {
                    uint64_t at;

                    if (!sgi_kona_gl_bitmap_pio_offset(&d.bitmap, line,
                                                        offset + k, &at) ||
                        at / 4 >= u->count) {
                        sgi_kona_gl_image_clear(s);
                        return false;
                    }
                    u->words[at / 4] |= (uint32_t)chunk[k] <<
                                       (24 - 8 * (at % 4));
                }
            }
            offset += n;
        }
    }
    return sgi_kona_gl_raw_bitmap_done(s);
}

/*
 * The DMA packet that carries a refused native texture load's source:
 * true, once, when one is due. The kernel waits for the packet's
 * completion and kills the pipe when none comes (an IRIX 6.5.30 run of
 * pfCity: "idma timed out", death reason 4), while the load is a
 * command the pipe accepts. So the model completes the transfer and
 * keeps none of its bytes, as it keeps no image for the refused header.
 * Approximated: what the pipe stores for such a load is the model's
 * unsupported case; that the transfer itself completes is what any
 * accepted DMA does.
 */
bool sgi_kona_texture_dma_discard(SGIKonaState *s)
{
    bool due = s->texture_dma_discard;

    s->texture_dma_discard = false;
    return due;
}

/*
 * The DMA packet that carries a DrawPixels or bitmap image the model does
 * not stage (too large, unframed or unallocated): true, once, as for a
 * refused texture load's source (sgi_kona_texture_dma_discard()), so the
 * packet completes unread.
 */
bool sgi_kona_image_dma_discard(SGIKonaState *s)
{
    bool due = s->image_dma_discard;

    s->image_dma_discard = false;
    return due;
}

bool sgi_kona_texture_dma_waiting(SGIKonaState *s)
{
    const SGIKonaTextureUpload *u = &s->texture_upload;

    return u->count && u->header[6] == 1 && !u->complete;
}

/* Native component transfers start at the exact mapped source address. */
bool sgi_kona_texture_image_dma(SGIKonaState *s, uint64_t io, uint32_t lines,
                                 uint32_t per_line, uint64_t pitch)
{
    SGIKonaTextureUpload *u = &s->texture_upload;
    uint8_t chunk[4096];
    uint64_t row_bytes = sgi_kona_texture_row_bytes(u);
    uint32_t prefix = sgi_kona_texture_bitmap(u->header) && !u->header[7] ?
                      u->bitmap_phase : 0;
    uint32_t height = u->header[2];
    uint64_t span = (uint64_t)(height - 1) * u->source_stride + row_bytes;

    if (sgi_kona_texture_bitmap(u->header) && !u->header[7]) {
        span = DIV_ROUND_UP(prefix + span, 4) * 4;
    }
    if (!sgi_kona_texture_dma_waiting(s) ||
        (io & 3) != (u->dma_layout ? u->dma_phase : u->header[5]) ||
        (u->header[7] ? (lines != height || per_line != row_bytes ||
                         pitch != u->source_stride) :
                        (lines != 1 || per_line != span || pitch != span)) ||
        span > UINT64_MAX - io) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: native texture DMA source framing mismatch;"
                      " upload refused\n");
        sgi_kona_texture_upload_clear(s);
        return false;
    }
    for (uint32_t line = 0; line < lines; line++) {
        for (uint32_t offset = 0; offset < per_line;) {
            uint32_t n = MIN(sizeof(chunk), per_line - offset);

            if (!sgi_kona_device_read(s, io + line * pitch + offset,
                                       chunk, n)) {
                sgi_kona_texture_upload_clear(s);
                return false;
            }
            /* Each run is one row's bytes; the upload drops any padding. */
            for (uint32_t k = 0; k < n;) {
                uint32_t source = offset + k;
                uint32_t row, col, run;

                if (u->header[7]) {
                    row = line;
                    col = source;
                    run = n - k;
                } else if (source < prefix) {
                    k += MIN(n - k, prefix - source);
                    continue;
                } else {
                    row = (source - prefix) / u->source_stride;
                    col = (source - prefix) % u->source_stride;
                    run = MIN(n - k, u->source_stride - col);
                }
                sgi_kona_texture_upload_bytes(u, row, col, chunk + k, run);
                k += run;
            }
            offset += n;
        }
    }
    if (!sgi_kona_texture_upload_normalize(s, u)) {
        sgi_kona_texture_upload_clear(s);
        return false;
    }
    u->complete = true;
    if (u->draw_pixels) {
        sgi_kona_gl_draw_pixels_done(s);
    }
    return true;
}

/*
 * The X server's image write by DMA: a DMA packet that
 * arrives while an X pixel write opened under the DMA mode bit waits for
 * its rows. The packet's lines of per_line bytes, pitch bytes apart from
 * device address io, are the rows the programmed-I/O path would have sent
 * as data words, big-endian, top row first (inferred: the byte count is
 * exactly that path's words times four, built from the same setup). The
 * per-line count and gap only say how the bytes lie in host memory; the
 * image's shape is the setup's. So the bytes go four at a time into the
 * open write, which stages them as it would its page words. Bytes beyond
 * the write's rows are logged and dropped; a write the packet leaves
 * short waits for its finish.
 *
 * Returns true once every byte has been read, which is when the kernel
 * may unpin the buffer, so the caller's completion follows it. A page
 * the map RAM does not map ends the transfer with false and, the model's
 * choice, no completion: the kernel then times the transfer out.
 */
bool sgi_kona_x_image_dma(SGIKonaState *s, uint64_t io, uint32_t lines,
                          uint32_t per_line, uint64_t pitch)
{
    uint8_t chunk[4096];
    uint64_t extra = 0;
    uint64_t last;

    trace_sgi_kona_x_image_dma(io, lines, per_line, s->x_pixel_remaining);
    if (per_line & 3) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X image DMA line is not whole words\n");
        s->x_write_refused = true;
        return false;
    }
    if (!lines || !per_line) {
        return true;
    }
    if (pitch && lines - 1 > (UINT64_MAX - io) / pitch) {
        goto overflow;
    }
    last = io + (uint64_t)(lines - 1) * pitch;
    if (per_line > UINT64_MAX - last) {
        goto overflow;
    }
    for (uint32_t l = 0; l < lines; l++) {
        uint64_t address = io + (uint64_t)l * pitch;

        for (uint64_t offset = 0; offset < per_line; offset += sizeof(chunk)) {
            uint32_t bytes = MIN(sizeof(chunk), per_line - offset);

            if (!sgi_kona_device_read(s, address + offset, chunk, bytes)) {
                s->x_write_refused = true;
                return false;
            }
            for (uint32_t b = 0; b < bytes; b += 4) {
                if (!s->x_pixel_remaining) {
                    extra += 4;
                    continue;
                }
                sgi_kona_x_pixel_word(s, ldl_be_p(chunk + b));
            }
        }
    }
    if (extra) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X image DMA carried %" PRIu64 " bytes past"
                      " its rows; not drawn\n", extra);
    }
    return true;

overflow:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-kona: X image DMA device address wraps\n");
    s->x_write_refused = true;
    return false;
}

typedef struct SGIKonaXPreparedPixel {
    uint32_t value;
    uint8_t tag;
    bool write;
} SGIKonaXPreparedPixel;

/* Model the observed pair as separate colour-bank copies. */
static int sgi_kona_x_window_copy_bank(SGIKonaState *s)
{
    static const uint32_t bank_gc[2][SGI_KONA_X_GC_WORDS] = {
        { 0, 0, 0, 0, 0xffffff, 0xffffff,
          0x2804, 0x7c001, 1, 6, 0x11, 0, 0 },
        { 0, 0, 0, 0, 0xffffff, 0xffffff,
          0x3804, 0xbc002, 1, 6, 0x11, 0, 0 },
    };
    const uint32_t *setup = s->x_pixel_setup;
    const uint32_t *position = s->x_position;

    if (setup[SGI_KONA_X_SETUP_MODE] != 1 ||
        setup[SGI_KONA_X_SETUP_SHIFT] ||
        setup[SGI_KONA_X_SETUP_PAD] ||
        !setup[SGI_KONA_X_SETUP_WIDTH] ||
        !setup[SGI_KONA_X_SETUP_HEIGHT] ||
        position[0] || position[1] ||
        position[2] != s->fb_width ||
        position[3] != s->fb_height) {
        return -1;
    }
    for (unsigned bank = 0; bank < ARRAY_SIZE(bank_gc); bank++) {
        if (!memcmp(s->x_gc, bank_gc[bank], sizeof(bank_gc[bank]))) {
            return bank;
        }
    }
    return -1;
}

/*
 * Ordinary copies use bank zero and the loaded clip. The exact window-copy
 * pair selects a bank and uses screen bounds. Preparing every result before
 * stores preserves the original source on overlap.
 */
static void sgi_kona_x_pixel_copy(SGIKonaState *s, const uint32_t *args)
{
    const SGIPlane *front;
    const SGIPlane *tag;
    const uint32_t *setup = s->x_pixel_setup;
    int64_t sx = (int32_t)args[0], sy = (int32_t)args[1];
    int64_t dx = (int32_t)args[2], dy = (int32_t)args[3];
    int64_t w = setup[SGI_KONA_X_SETUP_WIDTH];
    int64_t h = setup[SGI_KONA_X_SETUP_HEIGHT];
    int64_t i0, i1, j0, j1;
    SGIKonaRect clip;
    SGIXGC gc;
    bool rgb = false;
    bool window_copy = setup[SGI_KONA_X_SETUP_SRC_PACK] == 3 &&
                       setup[SGI_KONA_X_SETUP_DST_PACK] == 3;
    int bank = 0;
    uint8_t target_tag;
    size_t width, height;
    g_autofree SGIKonaXPreparedPixel *prepared = NULL;

    trace_sgi_kona_x_pixel_copy(sx, sy, dx, dy, w, h);
    if (window_copy) {
        bank = sgi_kona_x_window_copy_bank(s);
        if (bank < 0) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-kona: unsupported X window copy 3,3 GC/setup\n");
            return;
        }
    } else {
        if (!sgi_kona_x_decode(s, &gc)) {
            return;
        }
        rgb = gc.target == SGI_X_GC_MAIN_RGB8;
        /*
         * A copy moves stored pixels; the model keeps 30-bit TrueColor
         * (ifmt 9) as the same logical RGB as 24-bit, so both copy alike.
         */
        if ((!rgb && gc.target != SGI_X_GC_MAIN_INDEX16) ||
            (rgb ? gc.raw[10] != 8 && gc.raw[10] != 9 : gc.raw[10] != 5) ||
            setup[SGI_KONA_X_SETUP_MODE] != 1 ||
            setup[SGI_KONA_X_SETUP_SRC_PACK] != (rgb ? 2 : 1) ||
            setup[SGI_KONA_X_SETUP_DST_PACK] != (rgb ? 2 : 1) ||
            setup[SGI_KONA_X_SETUP_SHIFT] || setup[SGI_KONA_X_SETUP_PAD]) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: unsupported X copy target %u "
                          "setup %08x %08x %08x %08x %08x %08x %08x\n",
                          gc.target, setup[0], setup[1], setup[2], setup[3],
                          setup[4], setup[5], setup[6]);
            return;
        }
        if (sgi_x_gc_noop(&gc)) {
            return;
        }
    }
    front = sgi_kona_plane(s, sgi_kona_bank_plane(s, bank));
    tag = sgi_kona_plane(s, sgi_kona_bank_tag_plane(s, bank));
    target_tag = rgb ? SGI_KONA_TAG_DIRECT : SGI_KONA_TAG_X_INDEX16;
    if (window_copy) {
        clip = (SGIKonaRect) { 0, 0, s->fb_width, s->fb_height };
    } else {
        sgi_kona_x_clip(s, &clip);
    }
    i0 = MAX(MAX(0, -sx), clip.x1 - dx);
    i1 = MIN(MIN(w, s->fb_width - sx), clip.x2 - dx);
    j0 = MAX(MAX(0, -sy), clip.y1 - dy);
    j1 = MIN(MIN(h, s->fb_height - sy), clip.y2 - dy);
    if (i0 >= i1 || j0 >= j1) {
        return;
    }
    width = i1 - i0;
    height = j1 - j0;
    assert(width <= s->fb_width && height <= s->fb_height);
    /* The checked g_try_new0 multiplication refuses before any store. */
    prepared = g_try_new0(SGIKonaXPreparedPixel, width * height);
    if (!prepared) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X copy preparation allocation failed\n");
        return;
    }
    assert(bql_locked());
    for (size_t row = 0; row < height; row++) {
        int64_t j = j0 + row;
        int src_row = s->fb_height - 1 - (sy + j);
        int dst_row = s->fb_height - 1 - (dy + j);
        SGIPlaneRun src_run, src_tag_run, dst_run, dst_tag_run;

        sgi_kona_x_row(&s->mem, front, sx + i0, sx + i1, src_row, &src_run);
        sgi_kona_x_row(&s->mem, tag, sx + i0, sx + i1, src_row,
                       &src_tag_run);
        sgi_kona_x_row(&s->mem, front, dx + i0, dx + i1, dst_row, &dst_run);
        sgi_kona_x_row(&s->mem, tag, dx + i0, dx + i1, dst_row,
                       &dst_tag_run);
        for (size_t col = 0; col < width; col++) {
            int64_t i = i0 + col;
            SGIKonaXPreparedPixel *pixel = &prepared[row * width + col];
            uint32_t source, dest, source_tag, dest_tag;

            if (!window_copy && !sgi_kona_x_clip_contains(s, dx + i, dy + j)) {
                continue;
            }
            source = sgi_kona_x_row_load(&s->mem, front, &src_run, sx + i0,
                                         sx + i, src_row);
            source_tag = sgi_kona_x_row_load(&s->mem, tag, &src_tag_run,
                                             sx + i0, sx + i, src_row);
            dest = sgi_kona_x_row_load(&s->mem, front, &dst_run, dx + i0,
                                       dx + i, dst_row);
            dest_tag = sgi_kona_x_row_load(&s->mem, tag, &dst_tag_run,
                                           dx + i0, dx + i, dst_row);
            if (!window_copy &&
                (source_tag != target_tag || source > gc.width_mask ||
                 (sgi_x_gc_needs_destination(&gc) &&
                  (dest_tag != target_tag || dest > gc.width_mask)))) {
                qemu_log_mask(LOG_UNIMP, "sgi-kona: X copy incompatible "
                              "pixel source (%" PRId64 ",%" PRId64 ") "
                              "destination (%" PRId64 ",%" PRId64 ")\n",
                              sx + i, sy + j, dx + i, dy + j);
                return;
            }
            pixel->value = window_copy ? source :
                           sgi_x_gc_apply(&gc, source, dest);
            pixel->tag = source_tag;
            pixel->write = pixel->value != dest || pixel->tag != dest_tag;
        }
    }
    for (size_t row = 0; row < height; row++) {
        int64_t j = j0 + row;
        int dst_row = s->fb_height - 1 - (dy + j);
        bool changed = false;
        SGIPlaneRun dst_run, dst_tag_run;

        sgi_kona_x_row(&s->mem, front, dx + i0, dx + i1, dst_row, &dst_run);
        sgi_kona_x_row(&s->mem, tag, dx + i0, dx + i1, dst_row,
                       &dst_tag_run);
        for (size_t col = 0; col < width; col++) {
            int64_t i = i0 + col;
            const SGIKonaXPreparedPixel *pixel = &prepared[row * width + col];

            if (!pixel->write) {
                continue;
            }
            sgi_kona_x_row_store(&s->mem, front, &dst_run, dx + i0, dx + i,
                                 dst_row, pixel->value);
            sgi_kona_x_row_store(&s->mem, tag, &dst_tag_run, dx + i0, dx + i,
                                 dst_row, pixel->tag);
            changed = true;
        }
        if (changed) {
            sgi_kona_invalidate_plane(s, front, dst_row, dst_row);
        }
    }
}

/*
 * Prepare every destination result before any canonical store. A write
 * to the popup planes lands in the desktop planes, untagged, as a popup
 * fill does (sgi_kona_x_fill_source()).
 */
static void sgi_kona_x_write_commit(SGIKonaState *s)
{
    SGIBoardMemory *memory = &s->mem;
    const SGIPlane *front = sgi_kona_plane(s, s->plane_front);
    const SGIPlane *tag = sgi_kona_plane(s, s->plane_front_tag);
    const SGIPlane *source = sgi_board_memory_plane(&s->x_write_mem, 0);
    const SGIPlane *valid = sgi_board_memory_plane(&s->x_write_mem, 1);
    const SGIPlane *ones = sgi_board_memory_plane(&s->x_write_mem, 2);
    size_t width, pixels;
    SGIKonaRect r;
    g_autofree SGIKonaXPreparedPixel *prepared = NULL;
    SGIXGC gc;
    uint8_t target_tag;

    if (!s->x_write_snapshot || s->x_write_refused ||
        s->x_pixel_remaining || !sgi_kona_x_write_decode(s, &gc) ||
        sgi_x_gc_noop(&gc)) {
        return;
    }
    target_tag = gc.target == SGI_X_GC_MAIN_RGB8 ? SGI_KONA_TAG_DIRECT :
                 SGI_KONA_TAG_X_INDEX16;
    if (gc.target == SGI_X_GC_POPUP) {
        memory = &s->desktop_mem;
        front = sgi_board_memory_plane(memory, 3);
        tag = NULL;
    }
    sgi_kona_x_write_bounds(s, &r);
    if (r.x1 >= r.x2 || r.y1 >= r.y2) {
        return;
    }
    width = r.x2 - r.x1;
    pixels = width * (r.y2 - r.y1);
    assert(bql_locked());
    prepared = g_try_new0(SGIKonaXPreparedPixel, pixels);
    if (!prepared) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X write preparation allocation failed\n");
        return;
    }
    for (unsigned y = r.y1; y < r.y2; y++) {
        unsigned py = s->fb_height - 1 - y;
        SGIPlaneRun valid_run, source_run, ones_run, front_run, tag_run;

        sgi_kona_x_row(&s->x_write_mem, valid, r.x1, r.x2, y, &valid_run);
        sgi_kona_x_row(&s->x_write_mem, source, r.x1, r.x2, y, &source_run);
        sgi_kona_x_row(&s->x_write_mem, ones, r.x1, r.x2, y, &ones_run);
        sgi_kona_x_row(memory, front, r.x1, r.x2, py, &front_run);
        sgi_kona_x_row(memory, tag, r.x1, r.x2, py, &tag_run);
        for (unsigned x = r.x1; x < r.x2; x++) {
            SGIKonaXPreparedPixel *pixel =
                &prepared[(y - r.y1) * width + x - r.x1];
            uint32_t dest, dest_tag;

            if (!sgi_kona_x_row_load(&s->x_write_mem, valid, &valid_run,
                                     r.x1, x, y)) {
                continue;
            }
            dest = sgi_kona_x_row_load(memory, front, &front_run, r.x1, x,
                                       py);
            dest_tag = tag ? sgi_kona_x_row_load(memory, tag, &tag_run, r.x1,
                                                 x, py) : target_tag;
            if (sgi_x_gc_needs_destination(&gc) &&
                (dest_tag != target_tag || dest > gc.width_mask)) {
                qemu_log_mask(LOG_UNIMP, "sgi-kona: X streamed write "
                              "incompatible destination (%u,%u)\n", x, y);
                return;
            }
            uint32_t zero = sgi_kona_x_row_load(&s->x_write_mem, source,
                                                &source_run, r.x1, x, y);
            uint32_t one = sgi_kona_x_row_load(&s->x_write_mem, ones,
                                               &ones_run, r.x1, x, y);

            pixel->value = (zero & ~dest) | (one & dest);
            pixel->tag = target_tag;
            pixel->write = pixel->value != dest || pixel->tag != dest_tag;
        }
    }
    for (unsigned y = r.y1; y < r.y2; y++) {
        unsigned py = s->fb_height - 1 - y;
        bool changed = false;
        SGIPlaneRun front_run, tag_run;

        sgi_kona_x_row(memory, front, r.x1, r.x2, py, &front_run);
        sgi_kona_x_row(memory, tag, r.x1, r.x2, py, &tag_run);
        for (unsigned x = r.x1; x < r.x2; x++) {
            const SGIKonaXPreparedPixel *pixel =
                &prepared[(y - r.y1) * width + x - r.x1];

            if (pixel->write) {
                sgi_kona_x_row_store(memory, front, &front_run, r.x1, x, py,
                                     pixel->value);
                if (tag) {
                    sgi_kona_x_row_store(memory, tag, &tag_run, r.x1, x, py,
                                         pixel->tag);
                } else {
                    s->desktop_state_used = true;
                }
                changed = true;
            }
        }
        if (changed) {
            sgi_kona_invalidate_plane(s, front, py, py);
        }
    }
}

/* 0x13a, X pixel finish: always 0, ending the write ("Pixel writes"). */
static void sgi_kona_x_pixel_finish(SGIKonaState *s)
{
    trace_sgi_kona_x_pixel_finish(s->x_pixel_open);
    if (!s->x_pixel_open) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X pixel finish with no pixel write open\n");
    }
    sgi_kona_x_write_commit(s);
    if (s->x_pixel_remaining) {
        sgi_kona_x_read_invalidate(s);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X pixel finish with %" PRIu32 " words of"
                      " its DMA rows not arrived\n", s->x_pixel_remaining);
        s->x_pixel_remaining = 0;
    }
    s->x_pixel_open = false;
    sgi_kona_x_write_clear(s);
}

/*
 * The kernel's pipe-memory swaps (established from the IRIX kernel driver's
 * pool code). When a pool lacks room, it evicts other contexts' regions
 * with 0x1e3: class, offset and size in granules, the granule in bytes,
 * the copy count, the host buffer's device address as page >> 12 and
 * (address & 0xfff) | 0x9000, and the completion reason 0x102. It does not
 * wait: it queues the region and arms a timer of 120 half-second ticks,
 * and one reason-page 0x102 completes the oldest swap-out still queued; a
 * timer that runs out logs "mem_swapout timeout" and kills the pipe with
 * death reason 16. Swap-in, 0x1e2, carries the same six words and then
 * either the tagged offset, for the pipe to read the buffer back by DMA,
 * or 0xffffffff and the buffer's words inline, then 0x102; it completes
 * through the mailbox with 0x102, one per swap-in, in order, or the pipe
 * dies with reason 15. An allocation under pressure may first send 0x1df
 * 0x104, a suspend whose reply on the reason page is 0x104, a count n and
 * n words. The kernel's resume reads its suspend count before waiting: if
 * the interrupt handler has already taken the reply, it sends nothing
 * when n is below 2; if not, it waits for the reply and then sends 0x1de
 * and the reply's n words, whatever n is.
 *
 * The kernel keeps the swapped-out buffer untouched and only hands it back
 * on swap-in, so the bytes are the pipe's own. Approximated: the model
 * keeps every texture and list it draws in its own storage, keyed by
 * object, whatever the pool placement, so a swap moves none of it. A
 * swap-out writes zeros of the full length into the buffer, a swap-in
 * reads the buffer and discards it, and both complete at once. A suspend
 * replies n = 0 (SGI_KONA_SUSPEND_REPLY_WORDS), and a resume, when one
 * comes, is a bare 0x1de; what a real pipe saves is unknown.
 * A pool-pressure qtest checks the completions as stated; the guest check
 * is two texture programs sharing a 64 MB pipe.
 */
#define SGI_KONA_SWAP_MAX_BYTES (256 * MiB)

static uint64_t sgi_kona_swap_bytes(const uint32_t *a)
{
    return (uint64_t)a[2] * a[3] * a[4];
}

static uint64_t sgi_kona_swap_device(uint32_t page, uint32_t offset)
{
    return ((uint64_t)page << 12) | (offset & 0xfff);
}

static void sgi_kona_mem_swap_out(SGIKonaState *s, const uint32_t *a)
{
    static const uint8_t zeros[4096];
    uint64_t bytes = sgi_kona_swap_bytes(a);
    uint64_t device = sgi_kona_swap_device(a[5], a[6]);
    bool ok = (a[6] & ~0xfffu) == 0x9000 &&
              a[7] == SGI_KONA_REASON_SWAP && bytes <= SGI_KONA_SWAP_MAX_BYTES;

    for (int pass = 0; ok && pass < 2; pass++) {
        for (uint64_t done = 0; ok && done < bytes; done += sizeof(zeros)) {
            ok = sgi_kona_device_write(s, device + done, pass ? zeros : NULL,
                                       MIN(sizeof(zeros), bytes - done),
                                       !pass);
        }
    }
    if (!ok) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: swap-out of class %u "
                      "offset 0x%x size 0x%x to 0x%08x 0x%08x reason 0x%x "
                      "not completed\n", a[0], a[1], a[2], a[5], a[6], a[7]);
        return;
    }
    sgi_kona_post_reason(s, SGI_KONA_CMD_MEM_SWAP_OUT, SGI_KONA_REASON_SWAP);
}

static void sgi_kona_mem_swap_in_done(SGIKonaState *s)
{
    uint64_t bytes = sgi_kona_swap_bytes(s->swapin_header);
    uint8_t chunk[4096];
    bool ok = bytes <= SGI_KONA_SWAP_MAX_BYTES;

    if (s->swapin_mode != UINT32_MAX) {
        uint64_t device = sgi_kona_swap_device(s->swapin_header[5],
                                               s->swapin_mode);

        for (uint64_t done = 0; ok && done < bytes; done += sizeof(chunk)) {
            ok = sgi_kona_device_read(s, device + done, chunk,
                                      MIN(sizeof(chunk), bytes - done));
        }
    }
    if (!ok) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: swap-in of class %u "
                      "offset 0x%x size 0x%x not completed\n",
                      s->swapin_header[0], s->swapin_header[1],
                      s->swapin_header[2]);
        return;
    }
    sgi_kona_mailbox_post(s, SGI_KONA_REASON_SWAP);
    sgi_kona_raise_vector(s, true);
}

/*
 * A word after a swap-in's six. Returns false for a word that does not
 * belong to it, which ends the swap-in, logged, and is taken afresh.
 */
enum {
    SGI_KONA_SWAPIN_IDLE,
    SGI_KONA_SWAPIN_MODE,
    SGI_KONA_SWAPIN_DATA,
    SGI_KONA_SWAPIN_REASON,
};

static bool sgi_kona_mem_swap_in_word(SGIKonaState *s, uint32_t word)
{
    switch (s->swapin_stage) {
    case SGI_KONA_SWAPIN_MODE:
        if (word == UINT32_MAX) {
            uint64_t bytes = sgi_kona_swap_bytes(s->swapin_header);

            s->swapin_mode = word;
            if (bytes % 4 || bytes > SGI_KONA_SWAP_MAX_BYTES) {
                break;
            }
            s->swapin_data_left = bytes / 4;
            s->swapin_stage = s->swapin_data_left ? SGI_KONA_SWAPIN_DATA :
                                                    SGI_KONA_SWAPIN_REASON;
            return true;
        }
        if ((word & ~0xfffu) != 0x9000) {
            break;
        }
        s->swapin_mode = word;
        s->swapin_stage = SGI_KONA_SWAPIN_REASON;
        return true;
    case SGI_KONA_SWAPIN_DATA:
        if (!--s->swapin_data_left) {
            s->swapin_stage = SGI_KONA_SWAPIN_REASON;
        }
        return true;
    case SGI_KONA_SWAPIN_REASON:
        s->swapin_stage = SGI_KONA_SWAPIN_IDLE;
        if (word != SGI_KONA_REASON_SWAP) {
            break;
        }
        sgi_kona_mem_swap_in_done(s);
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: swap-in of class %u ended "
                  "by word 0x%" PRIx32 "; not completed\n",
                  s->swapin_header[0], word);
    s->swapin_stage = SGI_KONA_SWAPIN_IDLE;
    return false;
}

/*
 * 0x1e7 tag: the kernel's retrace activation request, sent for a swap after
 * its display-table rewrite and for any other retrace wait. It becomes a
 * swap record on the shared scanout, which honours one per retrace in order
 * and hands it back through sgi_kona_publish(). The record names the
 * context current when the request arrived, the one the kernel's swap
 * validated first; a request with none current still completes its tag,
 * since the driver waits on the tag either way.
 */
static void sgi_kona_retrace_request(SGIKonaState *s, uint32_t tag)
{
    uint64_t seq = s->dma.seq++;
    int slot = s->context_current;

    trace_sgi_kona_retrace_request(tag, slot);
    if (s->scanout.swap_count < SGI_SWAP_MAX) {
        for (unsigned i = 0; i < SGI_SWAP_MAX; i++) {
            if (!s->pending_swaps[i].valid) {
                s->pending_swaps[i] = (SGIKonaPendingSwap) {
                    .valid = true,
                    .seq = seq,
                    .context = slot,
                    .generation = slot == SGI_KONA_CONTEXT_NONE ? 0 :
                                  s->contexts[slot].generation,
                };
                break;
            }
        }
    }
    sgi_scanout_queue_swap(&s->scanout, seq, SGI_KONA_SWAP_COOKIE(slot, tag));

}

static void sgi_kona_kernel_packet(void *opaque, unsigned port_id,
                                   uint32_t selector, const uint32_t *args,
                                   unsigned argc)
{
    SGIKonaState *s = opaque;

    if (s->context_failed) {
        return;
    }
    sgi_kona_port_consumed(s, 1 + argc);
    trace_sgi_kona_kernel_command(selector, args[0],
                                  argc > 1 ? args[1] : 0,
                                  argc > 2 ? args[2] : 0, argc);
    switch (selector) {
    case SGI_KONA_CMD_X_DATA1:
    case SGI_KONA_CMD_X_DATA3:
    case SGI_KONA_CMD_X_DATA5:
    case SGI_KONA_CMD_X_POINT:
    case SGI_KONA_CMD_X_POINT6:
    case SGI_KONA_CMD_X_POINT24:
    case SGI_KONA_CMD_X_RELATIVE_POINT:
    case SGI_KONA_CMD_X_RELATIVE_POINT6:
    case SGI_KONA_CMD_X_RELATIVE_POINT24:
    case SGI_KONA_CMD_X_SEGMENT:
    case SGI_KONA_CMD_X_SEGMENT6:
    case SGI_KONA_CMD_X_LINE:
    case SGI_KONA_CMD_X_LINE6:
    case SGI_KONA_CMD_X_RELATIVE_LINE:
    case SGI_KONA_CMD_X_RELATIVE_LINE6:
    case SGI_KONA_CMD_X_BACKGROUND_POINT:
    case SGI_KONA_CMD_X_BACKGROUND_POINT6:
    case SGI_KONA_CMD_X_BACKGROUND_POINT24:
    case SGI_KONA_CMD_X_DATA2:
    case SGI_KONA_CMD_X_DATA4:
    case SGI_KONA_CMD_X_DATA6:
    case SGI_KONA_CMD_X_DATA8:
    case SGI_KONA_CMD_X_DATA10:
    case SGI_KONA_CMD_X_BOX:
    case SGI_KONA_CMD_X_BOX6:
    case SGI_KONA_CMD_X_RECT:
    case SGI_KONA_CMD_X_RECT6:
        sgi_kona_x_run_foreign(s, selector);
        break;
    default:
        sgi_kona_x_box_data_drop(s, selector);
    }
    if (selector != SGI_KONA_CMD_X_BDATA4 && selector != SGI_KONA_CMD_X_CLIP) {
        sgi_kona_x_clip_run_drop(s, selector);
    } else {
        sgi_kona_x_run_foreign(s, selector);
    }
    switch (selector) {
    case SGI_KONA_CMD_REASON_PAGE:
        sgi_kona_register_reason_page(s, args[0], args[1]);
        break;
    case SGI_KONA_CMD_POST_REASON:
        /*
         * The destroy sends 0x103 only while its context is still the
         * one the board holds. No context identifier accompanies it.
         * Approximate removal of that window from the display table by
         * releasing its bank selection, so X repaints in bank 0 show.
         * Ordinary deactivation preserves the window for later restores.
         */
        if (args[0] == 0x103 && s->context_current != SGI_KONA_CONTEXT_NONE) {
            unsigned i = s->context_current;

            sgi_kona_gl_read_dma_refuse(s);
            memset(&s->state[i]->window, 0, sizeof(s->state[i]->window));
            sgi_kona_gl_set_drawable(s, i);
            sgi_kona_invalidate_all(s);
        }
        sgi_kona_post_reason(s, selector, args[0]);
        break;
    case SGI_KONA_CMD_CHECK_PIPE:
        sgi_kona_post_reason(s, selector, args[0]);
        break;
    case SGI_KONA_CMD_CONTEXT_DEACTIVATE:
        sgi_kona_context_deactivate(s, args[0]);
        break;
    case SGI_KONA_CMD_RETURN_PAGE:
        sgi_kona_register_return_page(s, args[0], args[1]);
        break;
    case SGI_KONA_CMD_GET_CONFIG:
        sgi_kona_answer_config(s);
        break;
    case SGI_KONA_CMD_EEPROM:
        sgi_kona_flash_open(s, args[0]);
        break;
    case SGI_KONA_CMD_X_INIT:
        sgi_kona_x_initialise(s, args);
        break;
    case SGI_KONA_CMD_CONTEXT_MAP:
        /* The save area only; its length, 64 KiB, changes nothing. */
        sgi_kona_context_activate(s, selector, args[0], args[1]);
        break;
    case SGI_KONA_CMD_CONTEXT_RESTORE:
        sgi_kona_context_activate(s, selector, args[1], args[2]);
        if (args[0] != SGI_KONA_SAVE_AREA_MARKER) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: context restore carries save-area word"
                          " 0x%" PRIx32 ", not the 0x%x the model leaves\n",
                          args[0], SGI_KONA_SAVE_AREA_MARKER);
        }
        break;
    case SGI_KONA_CMD_CONTEXT_RECORDS:
    case SGI_KONA_CMD_CONTEXT_WORDS:
        sgi_kona_gl_window(s, selector, args);
        break;
    case SGI_KONA_CMD_SWAP_PAIR:
        /*
         * The last of the state the kernel's context activation sends with
         * a restore after 0x1f3 and 0x1ee, consumed whole so the GL
         * client's words after it frame. What its two words carry is
         * unknown, and nothing waits for an answer. The pool 0x1ed that the
         * kernel's DMA pool update sends with them goes to the DMA engine.
         */
        trace_sgi_kona_command_unmodelled(selector, argc);
        break;
    case SGI_KONA_CMD_SELECT_FEED_START:
        sgi_kona_gl_read_dma_start(s, args);
        break;
    case SGI_KONA_CMD_SELECT_FEED_FINISH:
        sgi_kona_gl_read_dma_finish(s, args);
        break;
    case SGI_KONA_CMD_DMA_PACKET:
    case SGI_KONA_CMD_DMA_LIST_WRITE:
    case SGI_KONA_CMD_DMA_POOL:
    case SGI_KONA_CMD_LIST_CALL:
    case SGI_KONA_CMD_LIST_CALL_POOL:
    case SGI_KONA_CMD_LIST_CALLS:
    case SGI_KONA_CMD_LIST_CALLS_POOL:
    case SGI_KONA_CMD_LIST_PULL:
    case SGI_KONA_CMD_LIST_MOVE:
        sgi_kona_dma_command(s, selector, args);
        break;
    case SGI_KONA_CMD_RETRACE:
        sgi_kona_retrace_request(s, args[0]);
        break;
    case SGI_KONA_CMD_VIDEO:
        sgi_kona_video_open(s, args[0]);
        break;
    case SGI_KONA_CMD_X_COLOR_MAP:
        sgi_kona_color_map_open(s, args[0], args[1]);
        break;
    case SGI_KONA_CMD_X_GAMMA:
        sgi_kona_gamma_open(s, args[0], args[1]);
        break;
    case SGI_KONA_CMD_X_CURSOR:
        sgi_kona_cursor_open(s, args[0], false);
        break;
    case 0x131:
        sgi_kona_cursor_open(s, args[0], true);
        break;
    case SGI_KONA_CMD_X_GC:
        sgi_kona_x_load_gc(s, args);
        break;
    case SGI_KONA_CMD_X_POSITION:
        sgi_kona_x_load_position(s, args);
        break;
    case SGI_KONA_CMD_X_BDATA4:
        sgi_kona_x_clip_data(s, args);
        break;
    case SGI_KONA_CMD_X_CLIP:
        sgi_kona_x_load_clip(s, args);
        break;
    case SGI_KONA_CMD_X_SCISSOR:
        sgi_kona_x_load_scissor(s, args);
        break;
    case SGI_KONA_CMD_X_STORE_STIPPLE:
        sgi_kona_x_store_stipple(s, args);
        break;
    case SGI_KONA_CMD_X_ROTATE_STIPPLE:
        sgi_kona_x_rotate_stipple(s, args);
        break;
    case SGI_KONA_CMD_X_LOAD_STIPPLE:
        sgi_kona_x_load_stipple(s, args[0]);
        break;
    case SGI_KONA_CMD_X_PIXEL_SETUP:
        sgi_kona_x_pixel_setup(s, args);
        break;
    case SGI_KONA_CMD_X_PIXEL_READ:
        sgi_kona_x_pixel_read(s, args);
        break;
    case SGI_KONA_CMD_X_PIXEL_WRITE:
        sgi_kona_x_pixel_write(s, args);
        break;
    case SGI_KONA_CMD_X_PIXEL_FINISH:
        sgi_kona_x_pixel_finish(s);
        break;
    case SGI_KONA_CMD_X_PIXEL_COPY:
        sgi_kona_x_pixel_copy(s, args);
        break;
    case SGI_KONA_CMD_X_DATA1:
    case SGI_KONA_CMD_X_DATA3:
    case SGI_KONA_CMD_X_DATA5:
    case SGI_KONA_CMD_X_DATA2:
    case SGI_KONA_CMD_X_DATA4:
    case SGI_KONA_CMD_X_DATA6:
    case SGI_KONA_CMD_X_DATA8:
    case SGI_KONA_CMD_X_DATA10:
        sgi_kona_x_box_data(s, selector, args, argc);
        break;
    case SGI_KONA_CMD_X_BOX:
    case SGI_KONA_CMD_X_BOX6:
        sgi_kona_x_boxes(s, false, args, argc);
        break;
    case SGI_KONA_CMD_X_RECT:
    case SGI_KONA_CMD_X_RECT6:
        sgi_kona_x_boxes(s, true, args, argc);
        break;
    case SGI_KONA_CMD_X_SPANS:
    case SGI_KONA_CMD_X_SPANS2:
    case SGI_KONA_CMD_X_SPANS6:
        sgi_kona_x_spans(s, selector, args, argc);
        break;
    case SGI_KONA_CMD_X_POINT:
    case SGI_KONA_CMD_X_POINT6:
    case SGI_KONA_CMD_X_POINT24:
        sgi_kona_x_points(s, args, argc, false);
        break;
    case SGI_KONA_CMD_X_BACKGROUND_POINT:
    case SGI_KONA_CMD_X_BACKGROUND_POINT6:
    case SGI_KONA_CMD_X_BACKGROUND_POINT24:
        sgi_kona_x_points(s, args, argc, true);
        break;
    case SGI_KONA_CMD_X_RELATIVE_POINT:
    case SGI_KONA_CMD_X_RELATIVE_POINT6:
    case SGI_KONA_CMD_X_RELATIVE_POINT24:
        sgi_kona_x_relative_points(s, args, argc);
        break;
    case SGI_KONA_CMD_X_SEGMENT:
    case SGI_KONA_CMD_X_SEGMENT6:
    case SGI_KONA_CMD_X_LINE:
    case SGI_KONA_CMD_X_LINE6:
    case SGI_KONA_CMD_X_RELATIVE_LINE:
    case SGI_KONA_CMD_X_RELATIVE_LINE6:
        sgi_kona_x_lines(s, selector, args, argc);
        break;
    case SGI_KONA_CMD_X_DID_MAP:
        if (args[0] < ARRAY_SIZE(s->desktop_map)) {
            s->desktop_map[args[0]] = args[1];
            s->desktop_map_valid[args[0]] = true;
            s->desktop_map_reported[args[0]] = false;
            s->desktop_state_used = true;
            sgi_kona_invalidate_all(s);
        } else {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: unsupported desktop map "
                          "ID %u raw=0x%08x\n", args[0], args[1]);
        }
        break;
    case SGI_KONA_CMD_DISPLAY_MODE:
    case SGI_KONA_CMD_X_DID_CONFIG:
    case SGI_KONA_CMD_WG_FLUSH:
        /*
         * Consumed whole so the stream stays in step. The display modes,
         * display-ID maps and configuration these carry are not modelled
         * yet: the framebuffer
         * does not change. Each word count is approximated from the one
         * caller read (see sgi_kona_command_tokens[]); a
         * wrong one would misframe the words after it, which the X
         * server's fills reaching the model as whole commands would show.
         */
        trace_sgi_kona_command_unmodelled(selector, argc);
        break;
    case SGI_KONA_CMD_MEM_SWAP_OUT:
        sgi_kona_mem_swap_out(s, args);
        break;
    case SGI_KONA_CMD_MEM_SWAP_IN:
        memcpy(s->swapin_header, args, sizeof(s->swapin_header));
        s->swapin_stage = SGI_KONA_SWAPIN_MODE;
        break;
    case SGI_KONA_CMD_MEM_SUSPEND:
        if (args[0] != SGI_KONA_REASON_SUSPEND) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: suspend with reason"
                          " 0x%" PRIx32 "; answered as 0x104\n", args[0]);
        }
        sgi_kona_post_reason_words(s, selector,
                                   (uint32_t[]){ SGI_KONA_REASON_SUSPEND,
                                                 SGI_KONA_SUSPEND_REPLY_WORDS },
                                   2);
        break;
    case SGI_KONA_CMD_MEM_RESUME:
        /* The reply's words come back, and the model saved nothing. */
        break;
    case SGI_KONA_CMD_MEM_REGION:
        trace_sgi_kona_kernel_mem_region(args[0], args[1], args[2], args[3]);
        sgi_kona_dma_list_region(s, args);
        /* fall through */
    case SGI_KONA_CMD_MEM_COPY_DOWN:
    case SGI_KONA_CMD_RESOURCE_SYNC:
        /*
         * The kernel's pool packets, consumed whole so the stream stays in
         * step. A display-list region's placement relocates the context's
         * list addresses (sgi_kona_dma_list_region()); no other placement
         * changes anything the model draws, and a copy-down moves no data.
         * The guest test is the minimal GLX program's trace showing
         * 0x1e4 2 9 0 1 and 0x172 0 with no word dropped.
         */
        trace_sgi_kona_command_unmodelled(selector, argc);
        break;
    default:
        g_assert_not_reached(); /* not in sgi_kona_command_tokens[] */
    }
}

static const SGICommandPortOps sgi_kona_kernel_ops = {
    .packet = sgi_kona_kernel_packet,
};

/* The level word of an overlay visual's buffer configuration. */
#define SGI_KONA_GL_OVERLAY_LEVEL 1

/*
 * The index sizes a level-1 visual's buffer configuration can carry in its
 * fourth word: the KONA X driver module's level-1 visuals are PseudoColor of
 * depth 2, 4 and 8 and TrueColor of depth 15, whose index size is 0.
 */
static bool sgi_kona_gl_level1_index_size(uint32_t word)
{
    return word == 0 || word == 2 || word == 4 || word == 8;
}

/*
 * The textport's own bulk-transfer framing, layered in front of the
 * generic command-port framer: strip the wire protocol's leading sync
 * word, then either forward a word to sgi_command_port_write() or, mid
 * bitmap-blit (command 0xa, excluded from the selector table for
 * exactly this reason), accumulate it into the header or payload.
 *
 * The GL client's commands 0x1 to 0xa and 0xc have the numbers of
 * textport commands: its buffer configuration opens with 1, the sync
 * word, its pack and read-buffer configuration are 2 and 4, and glRect,
 * glBitmap, glLight, glLightModel and glScissor are 3, 5 and 6, 7 and 8,
 * 9 and 0xa, and 0xc. A textport command always follows the sync word,
 * so a GL command meets a textport number only in the word after a 1.
 * How the pipe tells the grammars apart is unknown.
 *
 * Approximated: before the first context activation the page speaks
 * only the textport grammar, as the PROM and the kernel's early textport
 * use it, and a bare word opens a GL command only when it is a whole
 * selector the model framed before the whole GL grammar. After it, every
 * word between packets whose low 16 bits are a GL selector opens that
 * command (see sgi_kona_gl_token_word()), so a bare 0xa is
 * glLightModeli, never the textport's bitmap blit, and a 1 is held until
 * the next word: a textport command (1 to 9, 0xa or 0xc) makes it the
 * sync word, and anything else, such as the level 0 every traced
 * configuration sent, makes it the buffer configuration with that word
 * first. So a kernel textport write after the X server starts still
 * reaches the textport.
 * The rule misreads a buffer configuration whose level is a textport
 * command number, and takes a stray word whose low half is a GL
 * selector as that command.
 *
 * Level 1, an overlay visual, is told apart by looking two words further on.
 * Both readings of 1 1 occur: SceneViewer's overlay context sent the
 * configuration 1 1 0 8 and seven more words,
 * and the IRIX 6.5.30 kernel's unblank routine sends textport command 1 as 1 1
 * 0 (in the kernel textport code), which it may do once a graphics client
 * detaches, perhaps with a context still current in the model. The
 * configuration's fourth word is the visual's index size: the KONA X driver
 * module's visual tables hold level-1 visuals of depth 2, 4 and 8 (PseudoColor)
 * and 15 (TrueColor, index size 0), and nothing else at level 1. What follows
 * the kernel's 1 1 0 is not so bounded: its next textport packet opens with the
 * sync word, but kernel commands such as 0x1f8 share the same port. So while a
 * context is current, 1 1 a b is the configuration 1 a b ... only when b is 0,
 * 2, 4 or 8; any other b ends the textport's command 1 with argument a and is
 * then read afresh. With no context current 1 1 is the textport's at once, as
 * before. Approximated: a command 1 with nothing after it waits for the next
 * word; the model draws nothing for command 1, so only its credit is late; and
 * a kernel 1 1 0 followed, while a context is current, by a word of 0, 2, 4 or
 * 8 from outside the textport would be misread as a configuration. How the pipe
 * itself tells the grammars apart is unknown. Level 2 stays read as the
 * textport's command 2, which the kernel's textport colour setter sends.
 */
static void sgi_kona_textport_push(SGIKonaState *s, uint32_t word)
{
    uint32_t selector;

    if (s->gamma_remaining) {
        sgi_kona_gamma_word(s, word);
        return;
    }
    if (s->cursor_remaining) {
        sgi_kona_cursor_word(s, word);
        return;
    }
    if (s->gl_tex_table_remaining) {
        sgi_kona_gl_tex_table_word(s, word);
        return;
    }
    if (s->gl_data_remaining) {
        sgi_kona_gl_data_word(s, word);
        return;
    }
    if (sgi_kona_dma_calls_open(s)) {
        sgi_kona_dma_calls_word(s, word);
        return;
    }
    if (s->x_pixel_remaining && sgi_kona_x_pixel_owned(s) &&
        !sgi_kona_x_pixel_dma_waiting(s)) {
        sgi_kona_x_pixel_word(s, word);
        return;
    }
    if (s->x_stipple_store_remaining) {
        sgi_kona_x_stipple_word(s, word);
        return;
    }
    if (s->color_map_remaining) {
        sgi_kona_color_map_word(s, word);
        return;
    }
    if (s->video_open) {
        sgi_kona_video_word(s, word);
        return;
    }
    if (s->flash_op.op) {
        sgi_kona_flash_word(s, word);
        return;
    }
    /*
     * Kernel commands only start where a textport command could, between
     * packets, so the two framings never interleave.
     */
    if (s->kernel_port.framing) {
        sgi_command_port_write(&s->kernel_port, word, 4);
        return;
    }
    if (s->gl_port.framing) {
        sgi_command_port_write(&s->gl_port, word, 4);
        return;
    }
    if (s->textport_expect_sync) {
        if (s->gl_grammar && word == SGI_KONA_TEXTPORT_SYNC_WORD) {
            sgi_kona_x_box_data_drop(s, word);
            sgi_kona_x_clip_run_drop(s, word);
            s->gl_sync_held = true;
            s->textport_expect_sync = false;
            return;
        }
        if (word != SGI_KONA_TEXTPORT_SYNC_WORD &&
            sgi_kona_kernel_token_known(s, word)) {
            sgi_kona_kernel_port_word(s, word);
            return;
        }
        if (sgi_kona_gl_token_word(s, word, &selector)) {
            sgi_kona_gl_open(s, word, selector);
            return;
        }
        if (word != SGI_KONA_TEXTPORT_SYNC_WORD) {
            /*
             * Among these are the words of a command whose token the
             * model has no length for: they arrive one by one as fresh
             * words, and a 1 among them opens a textport packet that can
             * take the next command's words. The lengths are unknown, so
             * the behaviour stays; the log and trace make it visible.
             */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: textport expected a sync word, got "
                          "0x%" PRIx32 "\n", word);
            trace_sgi_kona_word_dropped(0, word, "not a token or sync word");
            /* X rect's token among them, for one: see sgi_kona_x_box_data(). */
            sgi_kona_x_box_data_drop(s, word);
            sgi_kona_x_clip_run_drop(s, word);
            return;
        }
        sgi_kona_x_box_data_drop(s, word);
        sgi_kona_x_clip_run_drop(s, word);
        s->textport_expect_sync = false;
        return;
    }
    if (s->gl_level1_held == 1) {
        s->gl_level1_arg = word;
        s->gl_level1_held = 2;
        return;
    }
    if (s->gl_level1_held == 2) {
        s->gl_level1_held = 0;
        if (!sgi_kona_gl_level1_index_size(word)) {
            /*
             * Textport command 1 and its argument, then the word again:
             * the next packet's sync word, a kernel token or anything
             * else, read as on a stream with no 1 1 before it.
             */
            sgi_command_port_write(&s->textport, SGI_KONA_GL_OVERLAY_LEVEL,
                                   4);
            sgi_command_port_write(&s->textport, s->gl_level1_arg, 4);
            sgi_kona_textport_push(s, word);
            return;
        }
        s->textport_expect_sync = true;
        s->gl_token_high = 0;
        sgi_command_port_write(&s->gl_port, SGI_KONA_CMD_GL_BUFFER_CONFIG, 4);
        sgi_command_port_write(&s->gl_port, SGI_KONA_GL_OVERLAY_LEVEL, 4);
        sgi_command_port_write(&s->gl_port, s->gl_level1_arg, 4);
        sgi_command_port_write(&s->gl_port, word, 4);
        return;
    }
    if (s->gl_sync_held) {
        s->gl_sync_held = false;
        if (word == SGI_KONA_GL_OVERLAY_LEVEL &&
            s->context_current != SGI_KONA_CONTEXT_NONE) {
            s->gl_level1_held = 1;
            return;
        }
        if (!sgi_kona_textport_selector_known(word)) {
            s->textport_expect_sync = true;
            s->gl_token_high = 0;
            sgi_command_port_write(&s->gl_port, SGI_KONA_CMD_GL_BUFFER_CONFIG,
                                   4);
            sgi_command_port_write(&s->gl_port, word, 4);
            return;
        }
    }

    switch (s->bitmap_state) {
    case SGI_KONA_BITMAP_IDLE:
        /*
         * The bitmap selector only starts a new blit between packets: a
         * word equal to SGI_KONA_BITMAP_SELECTOR is also a valid argument
         * value for other selectors, and misreading one mid-packet as a
         * fresh blit start desynchronizes every word the generic framer
         * still expects. Found by tracing a real boot; fixed by deferring
         * to the generic framer's own framing state and only intercepting
         * 0xa when the port is between packets.
         */
        if (!s->textport.framing && word == SGI_KONA_BITMAP_SELECTOR) {
            s->bitmap_state = SGI_KONA_BITMAP_HEADER;
            s->bitmap_header_have = 0;
            return;
        }
        sgi_command_port_write(&s->textport, word, 4);
        return;
    case SGI_KONA_BITMAP_HEADER:
        s->bitmap_header[s->bitmap_header_have++] = word;
        if (s->bitmap_header_have < SGI_KONA_BITMAP_HEADER_WORDS) {
            return;
        }
        /*
         * All six header words are established: h0/h1 = width/height, as
         * the bitmap sender's own code shows its payload-size
         * computation multiplying its first two header fields, then
         * applying this same DIV_ROUND_UP(...,32) below; h2..h5 = x
         * origin, y origin, x advance, y advance, from the IO6 PROM's
         * textport bitmap routine, which sends six header words from
         * consecutive halfwords of the caller's descriptor in that order.
         * sgi_kona_bitmap_blit() consumes all six; see its own comment
         * for how they combine with the raster position (command 4).
         * This corrects the previous x, y, width, height, ?, ? guess,
         * which swapped position with size and scrambled every unpacked
         * glyph into diagonal strokes, and the absolute-coordinate guess
         * after it, which piled every blit into one screen corner for
         * want of a tracked raster position.
         */
        s->bitmap_width = s->bitmap_header[0];
        s->bitmap_height = s->bitmap_header[1];
        s->bitmap_payload_expected =
            DIV_ROUND_UP((uint64_t)s->bitmap_width * s->bitmap_height, 32);
        s->bitmap_payload_have = 0;
        if (s->bitmap_payload_expected == 0) {
            sgi_kona_bitmap_reset(s);
            s->textport_expect_sync = true;
            return;
        }
        if (s->bitmap_payload_expected > SGI_KONA_BITMAP_MAX_PAYLOAD_WORDS) {
            sgi_kona_x_read_invalidate(s);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: bitmap blit %" PRIu32 "x%" PRIu32
                          " exceeds this model's payload bound; dropping\n",
                          s->bitmap_width, s->bitmap_height);
            sgi_kona_bitmap_reset(s);
            s->textport_expect_sync = true;
            return;
        }
        s->bitmap_payload = g_new0(uint32_t, s->bitmap_payload_expected);
        s->bitmap_state = SGI_KONA_BITMAP_PAYLOAD;
        return;
    case SGI_KONA_BITMAP_PAYLOAD:
        s->bitmap_payload[s->bitmap_payload_have++] = word;
        if (s->bitmap_payload_have < s->bitmap_payload_expected) {
            return;
        }
        sgi_kona_bitmap_blit(s);
        sgi_kona_bitmap_reset(s);
        s->textport_expect_sync = true;
        return;
    }
    g_assert_not_reached();
}

/*
 * Whether a user command is part-way through its words: any framer or
 * data run of the textport, GL and X grammars still waiting for more,
 * including a command the kernel framer opened without the privileged
 * bit (the X server's tokens and the GL client's list calls share its
 * table), live or set aside by a forced store.
 */
static bool sgi_kona_user_command_open(SGIKonaState *s)
{
    return (s->kernel_port.framing && !s->kernel_port_privileged) ||
           s->kernel_port_suspended.framing ||
           s->textport.framing || s->gl_port.framing || s->gl_sync_held ||
           s->gl_level1_held ||
           s->video_open || s->flash_op.op || s->color_map_remaining ||
           s->gamma_remaining ||
           s->cursor_remaining || s->gl_tex_table_remaining ||
           s->gl_data_remaining || sgi_kona_dma_calls_open(s) ||
           (s->x_pixel_remaining && sgi_kona_x_pixel_owned(s)) ||
           s->x_stipple_store_remaining ||
           s->bitmap_state != SGI_KONA_BITMAP_IDLE;
}

/*
 * The uploads the open GL data run fills, by its token: a texel load's or
 * a DrawPixels' texture upload, a raw bitmap's or a DrawPixels' image, an
 * inline bitmap, a pixel map. A run that holds none, a polygon stipple's
 * or a pop-lights', keeps its words in gl_data alone.
 */
static unsigned sgi_kona_gl_run_uploads(SGIKonaState *s)
{
    uint32_t token = s->gl_data_token;
    unsigned uploads = 0;

    if (!s->gl_data_remaining) {
        return 0;
    }
    if ((token == SGI_KONA_CMD_GL_TEX_LOAD ||
         token == SGI_KONA_CMD_GL_DRAW_PIXELS) && s->texture_upload.count) {
        uploads |= SGI_KONA_RUN_TEXTURE;
    }
    if ((token == SGI_KONA_CMD_GL_BITMAP ||
         (token == SGI_KONA_CMD_GL_DRAW_PIXELS &&
          s->gl_image_upload.draw_pixels)) && s->gl_image_upload.count) {
        uploads |= SGI_KONA_RUN_IMAGE;
    }
    if (token == SGI_KONA_CMD_GL_BITMAP_INLINE && s->gl_bitmap_upload.count) {
        uploads |= SGI_KONA_RUN_BITMAP;
    }
    if (sgi_kona_gl_pixel_map_token(token) && s->gl_map_upload.header[1]) {
        uploads |= SGI_KONA_RUN_MAP;
    }
    return uploads;
}


static void sgi_kona_port_save(SGIKonaSavedPort *d, const SGICommandPort *p)
{
    d->framing = p->framing;
    d->selector = p->selector;
    d->argc_expected = p->argc_expected;
    d->argc_have = p->argc_have;
    memcpy(d->args, p->args, sizeof(d->args));
}

static void sgi_kona_port_restore(SGICommandPort *p, const SGIKonaSavedPort *d)
{
    p->framing = d->framing;
    p->selector = d->selector;
    p->argc_expected = d->argc_expected;
    p->argc_have = d->argc_have;
    memcpy(p->args, d->args, sizeof(p->args));
}

/*
 * Put a user command the kernel framer had open back on it, from the copy
 * a forced store set aside or a context's saved copy, and clear the copy.
 * The kernel framer should be idle by then: the kernel's bracketed
 * packets were never seen to stay open past the bracket. If one is, it
 * ends here, logged, as it would have at the next forced store.
 */
static void sgi_kona_kernel_port_resume(SGIKonaState *s, SGIKonaSavedPort *d)
{
    if (!d->framing) {
        return;
    }
    if (s->kernel_port.framing) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: kernel command 0x%" PRIx32
                      " still open when a user command resumes; it ends"
                      " short of its words\n", s->kernel_port.selector);
        sgi_command_port_reset(&s->kernel_port);
    }
    sgi_kona_port_restore(&s->kernel_port, d);
    s->kernel_port_privileged = false;
    memset(d, 0, sizeof(*d));
}

/*
 * Copy the open user command out to `owner`'s saved slot and leave the
 * input idle for the context now sending words: the inferred pipe keeps a
 * context's open command with the context. The copy is exact, so the
 * context's next words continue the same command when it is restored.
 * Saved words leave the framers, so the credit register's held-word count
 * (SGI_KONA_CREDIT) no longer includes them; their processor-interface
 * credits went back when the kernel's packet ended. Limitation, as for the
 * in-place resume: the kernel also writes unbracketed words, such as the
 * 0x172 0 that follows a memory allocation, and after a switch those
 * restore the current context's copy and can be taken as its arguments.
 */
static void sgi_kona_input_save(SGIKonaState *s, int owner)
{
    SGIKonaSavedInput *v = &s->saved_input[owner];

    v->valid = true;
    sgi_kona_port_save(&v->textport, &s->textport);
    sgi_kona_port_save(&v->gl_port, &s->gl_port);
    v->kernel_port = s->kernel_port_suspended;
    v->textport_expect_sync = s->textport_expect_sync;
    v->gl_sync_held = s->gl_sync_held;
    v->gl_level1_held = s->gl_level1_held;
    v->gl_level1_arg = s->gl_level1_arg;
    v->gl_token_high = s->gl_token_high;
    v->video_open = s->video_open;
    v->video_command = s->video_command;
    v->video_have = s->video_have;
    v->video_first = s->video_first;
    v->video_tail_sum = s->video_tail_sum;
    v->flash_op = s->flash_op;
    memcpy(v->video_data, s->video_data, sizeof(v->video_data));
    v->color_map_start = s->color_map_start;
    v->color_map_count = s->color_map_count;
    v->color_map_remaining = s->color_map_remaining;
    v->color_map_outside = s->color_map_outside;
    v->gamma_start = s->gamma_start;
    v->gamma_count = s->gamma_count;
    v->gamma_remaining = s->gamma_remaining;
    v->gamma_outside = s->gamma_outside;
    v->cursor_remaining = s->cursor_remaining;
    v->cursor_stage_words = s->cursor_stage_words;
    v->cursor_stage_entry = s->cursor_stage_entry;
    memcpy(v->cursor_stage, s->cursor_stage, sizeof(v->cursor_stage));
    v->gl_tex_table_remaining = s->gl_tex_table_remaining;
    v->gl_tex_table_slot = s->gl_tex_table_slot;
    memcpy(v->gl_tex_table_words, s->gl_tex_table_words,
           sizeof(v->gl_tex_table_words));
    v->gl_tex_table_refused = s->gl_tex_table_refused;
    v->calls_token = s->dma.calls_token;
    v->calls_need = s->dma.calls_need;
    v->calls_have = s->dma.calls_have;
    memcpy(v->calls_words, s->dma.calls_words, sizeof(v->calls_words));
    v->x_stipple_store_entry = s->x_stipple_store_entry;
    v->x_stipple_store_width = s->x_stipple_store_width;
    v->x_stipple_store_height = s->x_stipple_store_height;
    v->x_stipple_store_remaining = s->x_stipple_store_remaining;
    /* The bitmap's payload buffer moves with it rather than being copied. */
    v->bitmap_state = s->bitmap_state;
    memcpy(v->bitmap_header, s->bitmap_header, sizeof(v->bitmap_header));
    v->bitmap_header_have = s->bitmap_header_have;
    v->bitmap_width = s->bitmap_width;
    v->bitmap_height = s->bitmap_height;
    v->bitmap_payload = s->bitmap_payload;
    v->bitmap_payload_have = s->bitmap_payload_have;
    v->bitmap_payload_expected = s->bitmap_payload_expected;
    s->bitmap_payload = NULL;
    sgi_kona_bitmap_reset(s);
    /* So does a GL data run's, with the uploads its token fills. */
    if (s->gl_data_remaining) {
        unsigned uploads = sgi_kona_gl_run_uploads(s);

        v->gl_data_token = s->gl_data_token;
        v->gl_data_remaining = s->gl_data_remaining;
        v->gl_data_have = s->gl_data_have;
        memcpy(v->gl_data, s->gl_data, sizeof(v->gl_data));
        if (uploads & SGI_KONA_RUN_TEXTURE) {
            v->texture_upload = s->texture_upload;
            s->texture_upload = (SGIKonaTextureUpload) { 0 };
        }
        if (uploads & SGI_KONA_RUN_IMAGE) {
            v->gl_image_upload = s->gl_image_upload;
            memset(&s->gl_image_upload, 0, sizeof(s->gl_image_upload));
        }
        if (uploads & SGI_KONA_RUN_BITMAP) {
            v->gl_bitmap_upload = s->gl_bitmap_upload;
            memset(&s->gl_bitmap_upload, 0, sizeof(s->gl_bitmap_upload));
        }
        if (uploads & SGI_KONA_RUN_MAP) {
            v->gl_map_upload = s->gl_map_upload;
            memset(&s->gl_map_upload, 0, sizeof(s->gl_map_upload));
        }
        s->gl_data_token = 0;
        s->gl_data_remaining = 0;
        s->gl_data_have = 0;
    }

    sgi_command_port_reset(&s->textport);
    sgi_command_port_reset(&s->gl_port);
    memset(&s->kernel_port_suspended, 0, sizeof(s->kernel_port_suspended));
    s->textport_expect_sync = true;
    s->gl_sync_held = false;
    s->gl_level1_held = 0;
    s->gl_token_high = 0;
    s->video_open = false;
    memset(&s->flash_op, 0, sizeof(s->flash_op));
    s->color_map_remaining = 0;
    s->gamma_remaining = 0;
    s->cursor_remaining = 0;
    s->cursor_stage_words = 0;
    s->gl_tex_table_remaining = 0;
    sgi_kona_dma_calls_drop(s);
    s->x_stipple_store_remaining = 0;
    trace_sgi_kona_input_save(owner);
}

/*
 * Move a saved GL data run back. Its uploads replace the live ones, which
 * hold nothing of an open command; one that still holds a finished upload
 * of another context, waiting for its load identifier or its DMA, loses
 * it, logged for each kind.
 */
static void sgi_kona_gl_run_displaces(SGIKonaState *s, const char *what)
{
    qemu_log_mask(LOG_UNIMP, "sgi-kona: a finished %s upload gives way to"
                  " context %d's restored run\n", what, s->context_current);
}

static void sgi_kona_gl_run_restore(SGIKonaState *s, SGIKonaSavedInput *v)
{
    s->gl_data_token = v->gl_data_token;
    s->gl_data_remaining = v->gl_data_remaining;
    s->gl_data_have = v->gl_data_have;
    memcpy(s->gl_data, v->gl_data, sizeof(s->gl_data));
    if (v->texture_upload.count) {
        if (s->texture_upload.count) {
            sgi_kona_gl_run_displaces(s, "texture");
        }
        sgi_kona_texture_upload_clear(s);
        s->texture_upload = v->texture_upload;
        v->texture_upload = (SGIKonaTextureUpload) { 0 };
    }
    if (v->gl_image_upload.count) {
        if (s->gl_image_upload.count) {
            sgi_kona_gl_run_displaces(s, "image");
        }
        sgi_kona_gl_image_clear(s);
        s->gl_image_upload = v->gl_image_upload;
        memset(&v->gl_image_upload, 0, sizeof(v->gl_image_upload));
    }
    if (v->gl_bitmap_upload.count) {
        if (s->gl_bitmap_upload.count) {
            sgi_kona_gl_run_displaces(s, "inline bitmap");
        }
        s->gl_bitmap_upload = v->gl_bitmap_upload;
    }
    if (v->gl_map_upload.header[1]) {
        if (s->gl_map_upload.header[1]) {
            sgi_kona_gl_run_displaces(s, "pixel map");
        }
        s->gl_map_upload = v->gl_map_upload;
    }
}

/* Copy `owner`'s saved command back into the input, which is idle. */
static void sgi_kona_input_restore(SGIKonaState *s, int owner)
{
    SGIKonaSavedInput *v = &s->saved_input[owner];

    sgi_kona_port_restore(&s->textport, &v->textport);
    sgi_kona_port_restore(&s->gl_port, &v->gl_port);
    sgi_kona_kernel_port_resume(s, &v->kernel_port);
    s->textport_expect_sync = v->textport_expect_sync;
    s->gl_sync_held = v->gl_sync_held;
    s->gl_level1_held = v->gl_level1_held;
    s->gl_level1_arg = v->gl_level1_arg;
    s->gl_token_high = v->gl_token_high;
    s->video_open = v->video_open;
    s->video_command = v->video_command;
    s->video_have = v->video_have;
    s->video_first = v->video_first;
    s->video_tail_sum = v->video_tail_sum;
    s->flash_op = v->flash_op;
    memcpy(s->video_data, v->video_data, sizeof(s->video_data));
    s->color_map_start = v->color_map_start;
    s->color_map_count = v->color_map_count;
    s->color_map_remaining = v->color_map_remaining;
    s->color_map_outside = v->color_map_outside;
    s->gamma_start = v->gamma_start;
    s->gamma_count = v->gamma_count;
    s->gamma_remaining = v->gamma_remaining;
    s->gamma_outside = v->gamma_outside;
    s->cursor_remaining = v->cursor_remaining;
    s->cursor_stage_words = v->cursor_stage_words;
    s->cursor_stage_entry = v->cursor_stage_entry;
    memcpy(s->cursor_stage, v->cursor_stage, sizeof(s->cursor_stage));
    s->gl_tex_table_remaining = v->gl_tex_table_remaining;
    s->gl_tex_table_slot = v->gl_tex_table_slot;
    memcpy(s->gl_tex_table_words, v->gl_tex_table_words,
           sizeof(s->gl_tex_table_words));
    s->gl_tex_table_refused = v->gl_tex_table_refused;
    s->dma.calls_token = v->calls_token;
    s->dma.calls_need = v->calls_need;
    s->dma.calls_have = v->calls_have;
    memcpy(s->dma.calls_words, v->calls_words, sizeof(s->dma.calls_words));
    s->x_stipple_store_entry = v->x_stipple_store_entry;
    s->x_stipple_store_width = v->x_stipple_store_width;
    s->x_stipple_store_height = v->x_stipple_store_height;
    s->x_stipple_store_remaining = v->x_stipple_store_remaining;
    sgi_kona_bitmap_reset(s);
    s->bitmap_state = v->bitmap_state;
    memcpy(s->bitmap_header, v->bitmap_header, sizeof(s->bitmap_header));
    s->bitmap_header_have = v->bitmap_header_have;
    s->bitmap_width = v->bitmap_width;
    s->bitmap_height = v->bitmap_height;
    s->bitmap_payload = v->bitmap_payload;
    s->bitmap_payload_have = v->bitmap_payload_have;
    s->bitmap_payload_expected = v->bitmap_payload_expected;
    v->bitmap_payload = NULL;
    if (v->gl_data_remaining) {
        sgi_kona_gl_run_restore(s, v);
    }
    sgi_kona_saved_input_clear(v);
    trace_sgi_kona_input_restore(owner);
}

/* The tokens only the kernel sends: the context switch and teardown. */
static bool sgi_kona_kernel_only_token(uint32_t word)
{
    return word == SGI_KONA_CMD_CONTEXT_DEACTIVATE ||
           word == SGI_KONA_CMD_CONTEXT_RESTORE ||
           word == SGI_KONA_CMD_CONTEXT_MAP ||
           word == SGI_KONA_CMD_POST_REASON;
}

/*
 * The current context's user input, before its word or slot store is
 * taken: a suspended command resumes in place if its context is current
 * again (a kernel-framer command set aside goes back on the framer), or
 * is saved with its owner if another context sends first; then a command
 * the current context had saved comes back.
 */
static void sgi_kona_user_input(SGIKonaState *s)
{
    int owner = s->suspended_owner;
    int current = s->context_current;

    if (owner != SGI_KONA_CONTEXT_NONE) {
        if (owner == current || !sgi_kona_user_command_open(s)) {
            trace_sgi_kona_bracket_resume(owner);
            sgi_kona_kernel_port_resume(s, &s->kernel_port_suspended);
        } else {
            sgi_kona_input_save(s, owner);
        }
        s->suspended_owner = SGI_KONA_CONTEXT_NONE;
    }
    if (current != SGI_KONA_CONTEXT_NONE && s->saved_input[current].valid &&
        !sgi_kona_user_command_open(s)) {
        sgi_kona_input_restore(s, current);
    }
}

/*
 * A word on the command page. While the kernel's privileged bit is set
 * the word belongs to a kernel command and goes to the kernel framer
 * alone, leaving any suspended user command untouched. A privileged word
 * that opens no kernel command is logged and ignored while a user command
 * is suspended; with none suspended it takes the ordinary path, as the
 * kernel's own textport commands did before. Otherwise a suspended
 * command resumes if its context is current again. If another context
 * sends words first, the suspended command is saved with its owner
 * (sgi_kona_input_save()), except an X pixel write, which stays in place
 * and takes only its owner's words; and a context whose saved command
 * waits gets it back before its first word. The word then takes the
 * ordinary path. That a user word naming a kernel-only token is logged,
 * not refused, is the model's choice: what the pipe does then is unknown.
 */
static void sgi_kona_command_input(SGIKonaState *s, uint32_t word)
{
    if (s->swapin_stage && sgi_kona_mem_swap_in_word(s, word)) {
        return;
    }
    if (s->kernel_input) {
        if (s->kernel_port.framing || sgi_kona_kernel_token_known(s, word)) {
            sgi_kona_kernel_port_word(s, word);
            return;
        }
        if (s->suspended_owner != SGI_KONA_CONTEXT_NONE) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: privileged word 0x%"
                          PRIx32 " opens no kernel command; ignored\n",
                          word);
            return;
        }
        sgi_kona_textport_push(s, word);
        return;
    }
    sgi_kona_user_input(s);
    if (s->gl_grammar && sgi_kona_kernel_only_token(word) &&
        !sgi_kona_user_command_open(s) && !s->kernel_port.framing) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: kernel-only token 0x%"
                      PRIx32 " without the privileged bit\n", word);
    }
    sgi_kona_textport_push(s, word);
}

/*
 * A store to the bracket register. Force ends argument collection: a
 * kernel command still open is abandoned, logged, and an open user
 * command is suspended with the current context. A command the kernel
 * framer opened without the privileged bit is a user command, the X
 * server's or a GL client's list call: it is set aside so the kernel's
 * packet frames on an idle framer, and is suspended like the others.
 * Privileged, with or without force, makes the following words a kernel
 * command's; 0 returns the input to user commands. Force with no command
 * open does nothing more (assumed). The low sixteen bits are written zero
 * by the only sender; other values are logged.
 */
static void sgi_kona_bracket_write(SGIKonaState *s, uint32_t value)
{
    if (value & ~(SGI_KONA_BRACKET_FORCE | SGI_KONA_BRACKET_PRIVILEGED)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: bracket store 0x%" PRIx32
                      " sets bits the kernel never sets\n", value);
    }
    if (value & SGI_KONA_BRACKET_FORCE) {
        if (s->kernel_port.framing && !s->kernel_port_privileged &&
            !s->kernel_port_suspended.framing) {
            sgi_kona_port_save(&s->kernel_port_suspended, &s->kernel_port);
            sgi_command_port_reset(&s->kernel_port);
        } else if (s->kernel_port.framing) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: forced store ends"
                          " kernel command 0x%" PRIx32 " short of its"
                          " words\n", s->kernel_port.selector);
            sgi_command_port_reset(&s->kernel_port);
        }
        if (s->suspended_owner == SGI_KONA_CONTEXT_NONE &&
            sgi_kona_user_command_open(s)) {
            s->suspended_owner = s->context_current;
            trace_sgi_kona_bracket_suspend(s->suspended_owner);
        }
    }
    s->kernel_input = value & SGI_KONA_BRACKET_PRIVILEGED;
}

/*
 * A plain word on the command page, as offset 0 takes it: the decoder a
 * fetched display list's words go through (hw/misc/sgi-kona-dma.c).
 */
void sgi_kona_command_word(SGIKonaState *s, uint32_t word)
{
    sgi_kona_textport_push(s, word);
}

/*
 * A store to the command page at `offset`. Offset 0 and the data slot, offset
 * 0xc, both carry a plain word: the next argument of a command still collecting
 * arguments, otherwise a token (or the textport's sync word). The kernel and
 * gfxinit send every token that way, and so does the X server the stock guest
 * runs, whose KONA X driver module writes each token as a plain word (0x129,
 * 0x12a, 0x136 and 0x138 among them). A store at 0xc + 0x20 * t opens token t
 * with the stored value as its first argument, the way the alternative KONA X
 * driver module sends its commands; the stock guest does not load it. Without
 * the privileged bit a slot store first settles a suspended or saved user
 * command, as a plain word does (sgi_kona_user_input()).
 *
 * Approximated: the decoding is inferred from the stores the senders
 * make. What distinguishes offset 0 from 0xc, and what the
 * low five address bits of a slot store do, are unknown, so the model
 * accepts slot stores at 0xc only and logs any other offset as
 * unexpected. It also logs a slot store naming a token it has no word
 * count for, and one arriving while a command is still open, the one
 * framing error the stores make detectable; the partial command is
 * dropped and the new one opened. The address-decode and data-slot
 * qtests check the rule as stated; nothing here is checked
 * against a real pipe.
 */
static void sgi_kona_command_page_write(SGIKonaState *s, uint32_t offset,
                                        uint32_t value)
{
    uint32_t token = (offset >> SGI_KONA_COMMAND_SLOT_SHIFT) &
                     SGI_KONA_COMMAND_TOKEN_MASK;

    if (offset == 0 || offset == SGI_KONA_COMMAND_DATA_SLOT) {
        s->activity = true;
        sgi_kona_command_input(s, value);
        return;
    }
    if (token == 0 ||
        (offset & SGI_KONA_COMMAND_SLOT_MASK) != SGI_KONA_COMMAND_DATA_SLOT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: unexpected store of 0x%" PRIx32
                      " at command page offset 0x%" PRIx32 "; dropped\n",
                      value, offset);
        trace_sgi_kona_word_dropped(offset, value, "unexpected offset");
        sgi_kona_x_box_data_drop(s, token);
        sgi_kona_x_clip_run_drop(s, token);
        return;
    }
    if (!sgi_kona_kernel_token_known(s, token)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: unknown token 0x%" PRIx32 " stored at"
                      " command page offset 0x%" PRIx32 "; value 0x%" PRIx32
                      " dropped\n", token, offset, value);
        trace_sgi_kona_word_dropped(offset, value, "unknown token");
        sgi_kona_x_box_data_drop(s, token);
        sgi_kona_x_clip_run_drop(s, token);
        return;
    }
    if (!s->kernel_input) {
        sgi_kona_user_input(s);
    }
    if (s->kernel_port.framing || s->gl_port.framing || s->gl_sync_held ||
        s->gl_level1_held ||
        s->video_open || s->flash_op.op || s->color_map_remaining ||
        s->gamma_remaining ||
        s->cursor_remaining || s->gl_tex_table_remaining ||
        s->gl_data_remaining || sgi_kona_dma_calls_open(s) ||
        s->x_pixel_remaining || s->x_stipple_store_remaining) {
        sgi_kona_x_read_invalidate(s);
        sgi_kona_gl_read_dma_refuse(s);
    }
    if (s->kernel_port.framing) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " while token 0x%" PRIx32
                      " still needs %" PRIu32 " words; partial command"
                      " dropped\n", token, offset, s->kernel_port.selector,
                      s->kernel_port.argc_expected -
                      s->kernel_port.argc_have);
        sgi_command_port_reset(&s->kernel_port);
        sgi_kona_x_box_data_drop(s, token);
    }
    if (s->gl_port.framing) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " while token 0x%" PRIx32
                      " still needs %" PRIu32 " words; partial command"
                      " dropped\n", token, offset, s->gl_port.selector,
                      s->gl_port.argc_expected - s->gl_port.argc_have);
        sgi_command_port_reset(&s->gl_port);
    }
    if (s->gl_sync_held) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " after a held 1; the 1 is"
                      " dropped\n", token, offset);
        s->gl_sync_held = false;
        s->textport_expect_sync = true;
    }
    if (s->gl_level1_held) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " after a held 1 1; the words"
                      " are dropped\n", token, offset);
        s->gl_level1_held = 0;
        s->textport_expect_sync = true;
    }
    if (s->video_open) {
        uint32_t length = sgi_kona_video_data_words(s->video_command);

        if (length == SGI_KONA_VIDEO_DATA_UNKNOWN) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: token 0x%" PRIx32 " stored at command"
                          " page offset 0x%" PRIx32 " while video command"
                          " 0x%" PRIx32 " awaits its checksum after %"
                          PRIu32 " data words; partial command dropped\n",
                          token, offset, s->video_command, s->video_have);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: token 0x%" PRIx32 " stored at command"
                          " page offset 0x%" PRIx32 " while video command"
                          " 0x%" PRIx32 " still needs %" PRIu32 " words;"
                          " partial command dropped\n", token, offset,
                          s->video_command, length + 1 - s->video_have);
        }
        s->video_open = false;
    }
    if (s->flash_op.op) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " while EEPROM operation %"
                      PRIu32 " is still taking words; partial operation"
                      " dropped\n", token, offset, s->flash_op.op);
        memset(&s->flash_op, 0, sizeof(s->flash_op));
    }
    if (s->color_map_remaining) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " while token 0x%x still needs %"
                      PRIu32 " entries; partial command dropped\n", token,
                      offset, SGI_KONA_CMD_X_COLOR_MAP,
                      s->color_map_remaining);
        s->color_map_remaining = 0;
    }
    if (s->gamma_remaining) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " while token 0x%x still needs %"
                      PRIu32 " entries; partial command dropped\n", token,
                      offset, SGI_KONA_CMD_X_GAMMA, s->gamma_remaining);
        s->gamma_remaining = 0;
    }
    if (s->cursor_remaining) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " while token 0x%x still needs %"
                      PRIu32 " words; partial command dropped\n", token,
                      offset, SGI_KONA_CMD_X_CURSOR, s->cursor_remaining);
        s->cursor_remaining = 0;
        s->cursor_stage_words = 0;
        memset(s->cursor_stage, 0, sizeof(s->cursor_stage));
    }
    if (s->gl_tex_table_remaining) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " while token 0x%x still needs %"
                      PRIu32 " words; partial command dropped\n", token,
                      offset, SGI_KONA_CMD_GL_TEX_TABLE,
                      s->gl_tex_table_remaining);
        sgi_kona_gl_tex_table_clear(s);
    }
    if (s->gl_data_remaining) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " while token 0x%x still needs %"
                      PRIu32 " words; partial command dropped\n", token,
                      offset, s->gl_data_token, s->gl_data_remaining);
        sgi_kona_texture_upload_clear(s);
        sgi_kona_gl_image_clear(s);
        memset(&s->gl_bitmap_upload, 0, sizeof(s->gl_bitmap_upload));
        memset(&s->gl_map_upload, 0, sizeof(s->gl_map_upload));
        s->gl_data_remaining = 0;
        s->gl_data_token = 0;
        s->gl_data_have = 0;
    }
    if (sgi_kona_dma_calls_open(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " while a list invocation still"
                      " takes its ranges; partial command dropped\n", token,
                      offset);
        sgi_kona_dma_calls_drop(s);
    }
    if (s->x_pixel_remaining) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " while token 0x%x still needs %"
                      PRIu32 " words; partial command dropped\n", token,
                      offset, SGI_KONA_CMD_X_PIXEL_WRITE,
                      s->x_pixel_remaining);
        s->x_pixel_remaining = 0;
        s->x_write_refused = true;
    }
    if (s->x_stipple_store_remaining) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " while token 0x%x still needs %"
                      PRIu32 " rows; partial command dropped\n", token,
                      offset, SGI_KONA_CMD_X_STORE_STIPPLE,
                      s->x_stipple_store_remaining);
        s->x_stipple_store_remaining = 0;
    }
    s->activity = true;
    sgi_kona_kernel_port_word(s, token);
    sgi_command_port_write(&s->kernel_port, value, 4);
}

/*
 * Traces this window at a volume a human can read: one line for the
 * first access, then one per power-of-two access-count milestone after
 * it, instead of one per entry -- the driver's own attach-time mapram
 * sweep alone touches every entry the active count selects twice (a
 * write, then a read-back), up to SGI_KONA_MAPRAM_ENTRIES entries.
 */
static void sgi_kona_mapram_trace_access(SGIKonaState *s, int cpu,
                                         hwaddr addr, bool write)
{
    s->map_ram_accesses++;
    if ((s->map_ram_accesses & (s->map_ram_accesses - 1)) == 0) {
        trace_sgi_kona_mapram_access(cpu, addr, write, s->map_ram_accesses);
    }
}

/*
 * One word of the pipe-internal space the indirect port reaches, or NULL
 * outside the two ranges the loader writes. See SGI_KONA_ARM_LOW_BASE.
 */
static uint32_t *sgi_kona_internal_word(SGIKonaState *s, uint32_t addr)
{
    if (addr & 3) {
        return NULL;
    }
    if (addr - SGI_KONA_ARM_LOW_BASE < SGI_KONA_ARM_LOW_WORDS * 4) {
        return &s->arm_low[(addr - SGI_KONA_ARM_LOW_BASE) / 4];
    }
    if (addr >= SGI_KONA_ARM_HIGH_BASE &&
        addr - SGI_KONA_ARM_HIGH_BASE < SGI_KONA_ARM_HIGH_WORDS * 4) {
        return &s->arm_high[(addr - SGI_KONA_ARM_HIGH_BASE) / 4];
    }
    return NULL;
}

/*
 * Trace every recorded section of the load with the SHA-256 of its words
 * as they are stored now, in big-endian byte order so the digest matches
 * the same bytes in the shipped microcode files. A section that a later
 * one overwrote hashes to the later contents; the stock loader's
 * sections do not overlap. The record then starts afresh for the next
 * load.
 */
static void sgi_kona_trace_load_record(SGIKonaState *s)
{
    uint32_t recorded = MIN(s->load_sections, SGI_KONA_LOAD_SECTIONS_MAX);

    trace_sgi_kona_load_record(s->load_sections, recorded);
    if (trace_event_get_state_backends(TRACE_SGI_KONA_LOAD_SECTION)) {
        for (uint32_t i = 0; i < recorded; i++) {
            uint32_t addr = s->load_section_addr[i];
            uint32_t words = s->load_section_words[i];
            g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
            bool backed = true;

            for (uint32_t w = 0; w < words; w++) {
                uint32_t *word = sgi_kona_internal_word(s, addr + 4 * w);
                uint8_t bytes[4];

                if (!word) {
                    backed = false;
                    break;
                }
                stl_be_p(bytes, *word);
                g_checksum_update(sum, bytes, sizeof(bytes));
            }
            trace_sgi_kona_load_section(i, addr, words,
                                        backed ? g_checksum_get_string(sum)
                                               : "unbacked");
        }
    }
    s->load_sections = 0;
    s->load_section_open = false;
    memset(s->load_section_addr, 0, sizeof(s->load_section_addr));
    memset(s->load_section_words, 0, sizeof(s->load_section_words));
}

static uint32_t sgi_kona_internal_read(SGIKonaState *s, uint32_t addr)
{
    uint32_t *word;

    if (addr - SGI_KONA_FLASH_BASE < SGI_KONA_FLASH_SIZE && !(addr & 3)) {
        return sgi_kona_flash_load(s, addr - SGI_KONA_FLASH_BASE);
    }
    if (addr == SGI_KONA_ICU_CONTROL) {
        return s->icu_control;
    }
    word = sgi_kona_internal_word(s, addr);
    return word ? *word : 0;
}

static void sgi_kona_internal_write(SGIKonaState *s, uint32_t addr,
                                    uint32_t value)
{
    uint32_t *word;

    if (addr == SGI_KONA_ICU_CONTROL) {
        uint32_t old = s->icu_control;

        s->icu_control = value;
        trace_sgi_kona_icu_control(old, value);
        if (old == 1 && value == 0) {
            sgi_kona_trace_load_record(s);
        }
        return;
    }
    word = sgi_kona_internal_word(s, addr);
    if (!word) {
        trace_sgi_kona_internal_write_dropped(addr, value);
        return;
    }
    *word = value;
}

/*
 * The advancing write: a write after a select opens a new section of the
 * load record, and each write extends the open one. The address wraps
 * at 32 bits like the port register it lives in.
 */
static void sgi_kona_port_write_next(SGIKonaState *s, uint32_t value)
{
    uint32_t index;

    if (!s->load_section_open) {
        if (s->load_sections < SGI_KONA_LOAD_SECTIONS_MAX) {
            s->load_section_addr[s->load_sections] = s->eeprom_addr;
            s->load_section_words[s->load_sections] = 0;
        }
        s->load_sections++;
        s->load_section_open = true;
    }
    index = s->load_sections - 1;
    if (index < SGI_KONA_LOAD_SECTIONS_MAX) {
        s->load_section_words[index]++;
    }
    sgi_kona_internal_write(s, s->eeprom_addr, value);
    s->eeprom_addr += 4;
}

/*
 * The two words the driver's attach reads through the indirect port to
 * class the pipe (see "Configurations" above): the product's low bits after
 * SGI_KONA_SELECT_PRODUCT, and no Reality bit after
 * SGI_KONA_SELECT_REDUCED. An unconfigured pipe answers neither, so its
 * reads stay the logged zero they always were.
 */
static bool sgi_kona_class_word(SGIKonaState *s, uint64_t *value)
{
    if (!s->config.product) {
        return false;
    }
    switch (s->cursor_selector) {
    case SGI_KONA_SELECT_PRODUCT:
        *value = s->config.product->inventory_select;
        return true;
    case SGI_KONA_SELECT_REDUCED:
        *value = 0;
        return true;
    default:
        return false;
    }
}

/* Whether display-list work is in progress or words wait behind it. */
static bool sgi_kona_work_pending(SGIKonaState *s)
{
    return s->dma.list_busy ||
           (s->work_queue && s->work_head < s->work_queue->len);
}

static uint64_t sgi_kona_read_word(void *opaque, hwaddr addr,
                                   unsigned size)
{
    SGIKonaState *s = opaque;
    int cpu = current_cpu ? current_cpu->cpu_index : -1;
    uint64_t value;

    switch (addr) {
    case SGI_KONA_WIDGET_ID:
        value = ((uint64_t)s->revision << WIDGET_ID_REVISION_SHIFT) |
                ((uint64_t)s->part << WIDGET_ID_PART_SHIFT) |
                ((uint64_t)s->manufacturer << WIDGET_ID_MANUFACTURER_SHIFT) |
                WIDGET_ID_ALWAYS_ONE;
        break;
    case SGI_KONA_WIDGET_CONTROL:
        value = s->widget_control;
        break;
    case SGI_KONA_PROBE:
        value = s->probe_value;
        break;
    case SGI_KONA_ONEWIRE:
    case SGI_KONA_NIC:
        /*
         * Two register offsets onto one chain: see SGI_KONA_NIC's own
         * header comment for the established/inferred evidence and the
         * open discriminating test.
         */
        value = s->onewire_reg;
        break;
    case SGI_KONA_LOW_WINDOW_WRITE:
        value = s->low_window_write;
        break;
    case SGI_KONA_INT_DEST_UPPER:
        value = s->int_dest_upper;
        break;
    case SGI_KONA_INT_DEST_LOWER:
        value = s->int_dest_lower;
        break;
    case SGI_KONA_INT_VECTORS:
        value = s->int_vectors;
        break;
    case SGI_KONA_CREDIT:
        /*
         * A pipe with work waiting reads as a full FIFO. The IRIX driver's
         * flow-control handler only compares this register with 0x300:
         * above it, it enables the low-watermark cause and puts the storing
         * thread to sleep until that cause; at or below it, it waits a
         * millisecond for the credits to come back and then gives up its
         * time slice. One queued word can stand for seconds of the model's
         * work, so a level counted in words would understate the wait; a
         * fixed full reading gives the driver the one answer that fits.
         * Model choice: how full a real pipe's FIFO reads while it works is
         * unknown.
         */
        value = sgi_kona_work_pending(s) ? SGI_KONA_CREDIT_MASK :
                                           sgi_kona_credit(s);
        break;
    case SGI_KONA_CREDIT_CONFIG:
        value = s->credit_config;
        break;
    case SGI_KONA_RESET_ARG:
        value = s->reset_arg;
        break;
    case SGI_KONA_INT_ENABLE:
        /* Approximation: reads zero. See the header comment. */
        value = 0;
        break;
    case SGI_KONA_INT_CAUSE:
        if (s->reason_queue_len) {
            sgi_kona_reason_drain(s);
        }
        value = s->int_cause;
        break;
    case SGI_KONA_RETRACE_TAG:
        value = s->retrace_tag;
        break;
    case SGI_KONA_MAILBOX:
        value = s->mailbox_pos < s->mailbox_len ?
                s->mailbox_queue[s->mailbox_pos] : 0;
        break;
    case SGI_KONA_RESET_STROBE:
        value = s->reset_strobe;
        break;
    case SGI_KONA_EEPROM_ENABLE:
        value = s->eeprom_enable;
        break;
    case SGI_KONA_EEPROM_ADDR:
        value = s->eeprom_addr;
        break;
    case SGI_KONA_EEPROM_DATA:
        /* Does not advance; see the header comment on the port. */
        value = sgi_kona_internal_read(s, s->eeprom_addr);
        break;
    case SGI_KONA_LOADER_3B0:
        value = s->loader_3b0;
        break;
    case SGI_KONA_LOADER_328:
        value = s->loader_328;
        break;
    case SGI_KONA_ACTIVITY:
        value = s->activity || sgi_kona_work_pending(s) ?
                SGI_KONA_ACTIVITY_BUS : 0;
        break;
    case 0x8003d0:
        if (sgi_kona_class_word(s, &value)) {
            break;
        }
        /* fall through */
    default:
        if (addr >= SGI_KONA_MAPRAM_BASE &&
            addr < SGI_KONA_MAPRAM_BASE + SGI_KONA_MAPRAM_ENTRIES * 4) {
            uint32_t index = (addr - SGI_KONA_MAPRAM_BASE) / 4;
            uint32_t count = sgi_kona_mapram_entry_count(s);

            if (index < count) {
                sgi_kona_mapram_trace_access(s, cpu, addr, false);
                value = s->map_ram[index];
                break;
            }
            trace_sgi_kona_mapram_beyond_count(cpu, addr, index, count);
        }
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: unimplemented read of offset "
                      "0x%" HWADDR_PRIx "\n", addr);
        value = 0;
        break;
    }

    return value;
}

/* The host-interface reset bit: see SGI_KONA_RESET_STROBE. */
static void sgi_kona_hip_reset(SGIKonaState *s)
{
    s->int_enable = 0;
    s->reset_arg = 0;
    s->credit_config = 0;
    s->low_window_write = 0;
    memset(s->map_ram, 0, s->map_ram_count * sizeof(*s->map_ram));
}

static void sgi_kona_write_word(void *opaque, hwaddr addr,
                                uint64_t value, unsigned size)
{
    SGIKonaState *s = opaque;
    int cpu;

    /*
     * Nearly every store is a command word: the command page comes first,
     * ahead of the register decode, which has no register inside it.
     */
    if (addr - SGI_KONA_TEXTPORT_PORT < SGI_KONA_COMMAND_PAGE_SIZE) {
        sgi_kona_command_page_write(s, addr - SGI_KONA_TEXTPORT_PORT, value);
        return;
    }
    cpu = current_cpu ? current_cpu->cpu_index : -1;
    switch (addr) {
    case SGI_KONA_WIDGET_CONTROL:
        /* Unknown control fields are stored without interpreted effects. */
        s->widget_control = value;
        break;
    case SGI_KONA_ONEWIRE:
    case SGI_KONA_NIC: {
        /*
         * Two register offsets onto one chain: see SGI_KONA_NIC's own
         * header comment for the established/inferred evidence and the
         * open discriminating test. Both offsets drive and observe the
         * identical onewire_reg/onewire_bus state.
         */
        uint32_t pulse = (value & SGI_KONA_ONEWIRE_PULSE_MASK) >>
                         SGI_KONA_ONEWIRE_PULSE_SHIFT;
        uint32_t sample = (value & SGI_KONA_ONEWIRE_SAMPLE_MASK) >>
                          SGI_KONA_ONEWIRE_SAMPLE_SHIFT;
        bool level = sgi_onewire_slot(s->parent_obj.onewire_bus, pulse,
                                      sample);

        /*
         * Matches the Bridge NIC register's own two-step behaviour: the
         * pulse/sample bits the guest wrote persist, and completion is
         * reported synchronously, inside this same write, with DONE
         * always set and LEVEL carrying this slot's sampled result.
         */
        s->onewire_reg = ((uint32_t)value & SGI_KONA_ONEWIRE_WRITABLE) |
                         SGI_KONA_ONEWIRE_DONE |
                         (level ? SGI_KONA_ONEWIRE_LEVEL : 0);
        break;
    }
    case SGI_KONA_LOW_WINDOW_WRITE: {
        /*
         * The credit target (see sgi_kona_pi_return()): the IO6 firmware
         * writes 0x23 once, the driver 1 to park it and its transaction
         * number shifted left one to restore it.
         */
        uint32_t parked = s->pi_parked;

        s->low_window_write = value;
        if (!(value & 1) && parked) {
            s->pi_parked = 0;
            sgi_kona_pi_return(s, parked);
        }
        break;
    }
    case SGI_KONA_INT_DEST_UPPER:
        s->int_dest_upper = value;
        break;
    case SGI_KONA_INT_DEST_LOWER:
        s->int_dest_lower = value;
        break;
    case SGI_KONA_INT_VECTORS:
        s->int_vectors = value;
        break;
    case SGI_KONA_RESET_ARG:
        s->reset_arg = value;
        break;
    case SGI_KONA_CREDIT_CONFIG:
        s->credit_config = value;
        break;
    case SGI_KONA_INT_ENABLE:
        s->int_enable = value;
        sgi_kona_video_deliver(s);
        break;
    case SGI_KONA_INT_CAUSE:
        /* Write-1-to-clear, SGI_KONA_INT_CAUSE_GE included. */
        s->int_cause &= ~(uint32_t)value;
        if (s->reason_queue_len) {
            sgi_kona_reason_drain(s);
        }
        if (value & SGI_KONA_INT_CAUSE_VIDEO) {
            s->video_delivered = false;
        }
        /*
         * A completion word is acknowledged by this write alone: the
         * driver's DMA-write wait reads the mailbox once and writes
         * nothing back to it before its handler clears the cause bit
         * (established from the driver's interrupt handlers), where
         * the reply protocol consumes each word with a mailbox write
         * first. So clearing the bit consumes an unread completion word
         * at the head; left queued, completions would fill the queue.
         */
        if ((value & SGI_KONA_INT_CAUSE_MAILBOX) &&
            s->mailbox_pos < s->mailbox_len &&
            (s->mailbox_completions & (1u << s->mailbox_pos))) {
            s->mailbox_pos++;
        }
        /*
         * Re-arm MAILBOX synchronously if a next reply word is already
         * queued: this model has no separate device-side timing to
         * simulate the real interrupt's re-assertion latency.
         */
        if ((value & SGI_KONA_INT_CAUSE_MAILBOX) &&
            s->mailbox_pos < s->mailbox_len) {
            s->int_cause |= SGI_KONA_INT_CAUSE_MAILBOX;
        }
        break;
    case SGI_KONA_MAILBOX:
        /*
         * Any write value (evidence always shows 0) consumes the current
         * reply word; it does not touch SGI_KONA_INT_CAUSE.
         */
        if (s->mailbox_pos < s->mailbox_len) {
            s->mailbox_pos++;
        }
        break;
    case SGI_KONA_RESET_STROBE:
        s->reset_strobe = value;
        trace_sgi_kona_reset_register(value);
        if (value & 1) {
            sgi_kona_hip_reset(s);
        }
        break;
    case SGI_KONA_EEPROM_ENABLE:
        s->eeprom_enable = value;
        break;
    case 0x8003c0:
        s->cursor_selector = value;
        break;
    case 0x8003d0:
        sgi_kona_cursor_register(s, value);
        break;
    case SGI_KONA_EEPROM_ADDR:
        s->eeprom_addr = value;
        s->load_section_open = false;
        break;
    case SGI_KONA_EEPROM_DATA:
        /*
         * The firmware and driver write 0 here to prime an EEPROM read;
         * outside the backed ranges that write is traced and dropped.
         */
        sgi_kona_internal_write(s, s->eeprom_addr, value);
        break;
    case SGI_KONA_PORT_WRITE_NEXT:
        s->activity = true;
        sgi_kona_port_write_next(s, value);
        break;
    case SGI_KONA_ACTIVITY:
        if (value & SGI_KONA_ACTIVITY_BUS) {
            s->activity = false;
        }
        if (value & ~(uint64_t)SGI_KONA_ACTIVITY_BUS) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-kona: unimplemented write of offset "
                          "0x%" HWADDR_PRIx " (value 0x%" PRIx64 ")\n",
                          addr, value);
        }
        break;
    case SGI_KONA_LOADER_3B0:
        s->loader_3b0 = value;
        trace_sgi_kona_loader_register(addr, value);
        break;
    case SGI_KONA_LOADER_328:
        s->loader_328 = value;
        trace_sgi_kona_loader_register(addr, value);
        if (value == 1) {
            sgi_kona_dg_start(s);
        }
        break;
    case SGI_KONA_DMA_FIFO_PUSH:
        s->activity = true;
        sgi_command_port_write(&s->dma.fifo_port, value, 4);
        break;
    case SGI_KONA_DMA_FIFO_KICK:
        sgi_command_port_kick_write(&s->dma.fifo_port, value);
        sgi_kona_bracket_write(s, value);
        break;
    default:
        if (addr >= SGI_KONA_MAPRAM_BASE &&
            addr < SGI_KONA_MAPRAM_BASE + SGI_KONA_MAPRAM_ENTRIES * 4) {
            uint32_t index = (addr - SGI_KONA_MAPRAM_BASE) / 4;
            uint32_t count = sgi_kona_mapram_entry_count(s);

            if (index < count) {
                sgi_kona_mapram_trace_access(s, cpu, addr, true);
                s->map_ram[index] = value;
                break;
            }
            trace_sgi_kona_mapram_beyond_count(cpu, addr, index, count);
        }
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: unimplemented write of offset "
                      "0x%" HWADDR_PRIx " (value 0x%" PRIx64 ")\n",
                      addr, value);
        break;
    }
}

/*
 * An 8-byte access. The KONA X driver module's bulk data helpers store aligned
 * pairs of data words as one doubleword at the command page's offset 0, most
 * significant word first, and odd words as 4-byte stores at the data slot (the
 * stores are established, that the pipe takes them as two data words inferred).
 * Such a store delivers the high word, then the low word, each exactly as a
 * 4-byte store at offset 0.
 *
 * The IRIX GL client library's vector senders store the same way at any
 * stream position: glRasterPos2sv sends its selector and then x << 32 | y
 * as one doubleword where glRasterPos2s sends x and y as two words, and
 * glColor3fv sends its selector, red and green as one doubleword, then
 * blue. The two forms of each call must mean the same, so a doubleword is
 * two words, high word first, wherever it falls (inferred from those
 * senders; how the host interface carries the wide store is unknown).
 *
 * Approximated: a doubleword whose high word opens a command opens it as a
 * plain word would; no sender read does that, and no evidence says how the
 * pipe treats one. The doubleword data qtest (A:B inside an open load
 * counts as A then B) and the 3D qtest of the vector forms check the split.
 *
 * The register file itself is word-sized. Any other 8-byte access is
 * split into two 4-byte accesses at the address and the address plus 4,
 * high word at the lower address, which is how the core already split a
 * wide access from a bulk qtest or DMA read or write before this widget
 * took 8 bytes, and it is logged: no evidence says what a real pipe does
 * with one.
 */
static bool sgi_kona_doubleword_data(hwaddr addr)
{
    return addr == SGI_KONA_TEXTPORT_PORT;
}

static void sgi_kona_log_split(hwaddr addr, bool write)
{
    qemu_log_mask(LOG_UNIMP,
                  "sgi-kona: 8-byte %s at offset 0x%" HWADDR_PRIx
                  " split into two 4-byte accesses\n",
                  write ? "store" : "load", addr);
}

/*
 * Display-list work and the accesses that meet it.
 *
 * A list a processor invokes runs in slices (hw/misc/sgi-kona-dma.c), so
 * the store that invoked it can return while the pipe still works, and
 * the processor takes its timer interrupts meanwhile. IRIX on IP27 counts
 * one scheduler tick per Count/Compare interrupt and does not make up lost
 * ones, so a list run whole inside the store stopped its clock for as long
 * as the list took: a GLperf fill on an Onyx2 lost all but 1.5% of them.
 *
 * Meanwhile an access either goes behind the work, waits for it, or acts
 * on the board as it stands:
 *
 * - command-stream stores queue, in the order they arrive, and run once
 *   the list has finished: the command page, the DMA FIFO port, the
 *   bracket register, the internal port write and the map RAM, which the
 *   list's DMA translates through. Each store's credit goes back when the
 *   work takes the store, as the board returns one when it frees the FIFO
 *   slot. The driver's credits so bound the backlog to the FIFO, and the
 *   processor interface's flow control puts the storing thread to sleep
 *   (hw/mips/sgi/pi-gfx.c, sgi_kona_work_lowater()).
 * - the interrupt cause, interrupt enable and mailbox registers, which the
 *   kernel's interrupt handler reads and acknowledges while the pipe
 *   works, act at once, as do the retrace tag and widget identity reads.
 *   A cause bit or mailbox word queued work raises appears when that work
 *   runs, as on the board.
 * - every other access waits for the work to finish: it reads what the
 *   work writes or changes what it reads, and the model's results must
 *   not depend on how far the work has got.
 *
 * Inferred from the driver's use, not measured: which registers the board
 * serves while its pipe is busy. Stores the processor interface did not
 * count (its graphics page disabled, or a qtest's) are not bounded by
 * credits: past SGI_KONA_WORK_QUEUE_MAX waiting words such a store waits
 * for the work, which keeps the queue's memory bounded.
 */
typedef struct SGIKonaQueuedWord {
    uint32_t addr;
    uint32_t value;
    bool counted;               /* the store's credit goes back with it */
    Object *node;               /* the Hub that sent the store */
} SGIKonaQueuedWord;

#define SGI_KONA_WORK_QUEUE_MAX (1024 * 1024)

typedef enum SGIKonaAccess {
    SGI_KONA_ACCESS_WAITS,
    SGI_KONA_ACCESS_NOW,
    SGI_KONA_ACCESS_QUEUES,
} SGIKonaAccess;

static SGIKonaAccess sgi_kona_access_kind(hwaddr addr, bool write)
{
    SGIKonaAccess stream = write ? SGI_KONA_ACCESS_QUEUES :
                                   SGI_KONA_ACCESS_WAITS;

    if (addr - SGI_KONA_TEXTPORT_PORT < SGI_KONA_COMMAND_PAGE_SIZE) {
        return stream;
    }
    switch (addr) {
    case SGI_KONA_DMA_FIFO_PUSH:
    case SGI_KONA_DMA_FIFO_KICK:
    case SGI_KONA_PORT_WRITE_NEXT:
        return stream;
    case SGI_KONA_INT_CAUSE:
    case SGI_KONA_INT_ENABLE:
    case SGI_KONA_MAILBOX:
        return SGI_KONA_ACCESS_NOW;
    case SGI_KONA_CREDIT:
        return write ? SGI_KONA_ACCESS_WAITS : SGI_KONA_ACCESS_NOW;
    case SGI_KONA_RETRACE_TAG:
    case SGI_KONA_WIDGET_ID:
        return write ? SGI_KONA_ACCESS_WAITS : SGI_KONA_ACCESS_NOW;
    /*
     * The kernel reads and writes the low-window word while the pipe
     * works, its video interrupt handler writes the other two at every
     * retrace (0x800228 is unmodelled and logged), and its timeout check
     * reads the activity latch every 500 ms while it waits on the pipe;
     * the command stream reaches none of them. The latch reads set while
     * work waits, the pipe being busy, and the work sets it again as it
     * consumes words.
     */
    case SGI_KONA_LOW_WINDOW_WRITE:
    case SGI_KONA_ACTIVITY:
        return SGI_KONA_ACCESS_NOW;
    case 0x800228:
        return write ? SGI_KONA_ACCESS_NOW : SGI_KONA_ACCESS_WAITS;
    default:
        if (addr >= SGI_KONA_MAPRAM_BASE &&
            addr < SGI_KONA_MAPRAM_BASE + SGI_KONA_MAPRAM_ENTRIES * 4) {
            return stream;
        }
        return SGI_KONA_ACCESS_WAITS;
    }
}

/* Finish the work now, on this thread, waiting out a slice on another. */
static void sgi_kona_work_drain(SGIKonaState *s, hwaddr addr)
{
    int64_t start;

    if (!s->work_queue) {
        return;
    }
    sgi_head_work_wait_slice(&s->work);
    if (!sgi_kona_work_pending(s)) {
        return;
    }
    start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    sgi_head_work_drain(&s->work);
    trace_sgi_kona_work("drained", addr,
                        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - start);
}

/*
 * Whether a plain word stored on the command page now would end a command
 * that can keep the pipe busy for milliseconds: a GL CopyPixels, or the
 * last word of a data run (a DrawPixels strip, a texture load, a bitmap,
 * a vertex array), whose command then runs. Such a word becomes work of
 * its own, so the store returns before the command runs, as a list's
 * does. It only peeks at the framers, which the word itself may still
 * change on its way in (a context switch's settling of a suspended
 * command, say): a word wrongly taken for one only runs a little later,
 * in order, so the guess need not be exact.
 */
static bool sgi_kona_word_ends_long_command(SGIKonaState *s, hwaddr addr)
{
    hwaddr offset = addr - SGI_KONA_TEXTPORT_PORT;

    if ((offset != 0 && offset != SGI_KONA_COMMAND_DATA_SLOT) ||
        s->kernel_input || s->swapin_stage || s->gamma_remaining ||
        s->cursor_remaining || s->gl_tex_table_remaining) {
        return false;
    }
    if (s->gl_data_remaining) {
        return s->gl_data_remaining == 1;
    }
    if (sgi_kona_dma_calls_open(s) || s->x_pixel_remaining ||
        s->x_stipple_store_remaining || s->color_map_remaining ||
        s->video_open || s->flash_op.op || s->kernel_port.framing) {
        return false;
    }
    return s->gl_port.framing &&
           s->gl_port.selector == SGI_KONA_CMD_GL_COPY_PIXELS &&
           s->gl_port.argc_have + 1 == s->gl_port.argc_expected;
}

/*
 * Whether a command-stream word stored now has to wait, and if so keep
 * it. A word that would start long work on a processor becomes the work
 * (sgi_kona_word_ends_long_command()). Only another processor can store
 * while a slice's draw has let go of the big lock, and its word goes
 * behind everything, even when the slice is running a word that had
 * waited and nothing else is left.
 */
static bool sgi_kona_work_defer(SGIKonaState *s, hwaddr addr,
                                uint32_t value, bool counted)
{
    SGIKonaQueuedWord w = {
        .addr = addr, .value = value, .counted = counted,
        .node = sgi_pi_gfx_store_source(),
    };

    /* qtest, which has no processor, stands in for one with the knob. */
    if (!s->work.drawing && !sgi_kona_work_pending(s) &&
        (current_cpu || s->work.test_words) && !s->work_replaying &&
        sgi_kona_word_ends_long_command(s, addr)) {
        trace_sgi_kona_work("word-deferred", addr, value);
    } else if (!s->work.drawing) {
        if (!sgi_kona_work_pending(s)) {
            return false;
        }
        if (s->work_queue->len - s->work_head >= SGI_KONA_WORK_QUEUE_MAX) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: %u words wait behind"
                          " the pipe's work, more than graphics credits"
                          " allow; waiting for the work\n",
                          s->work_queue->len - s->work_head);
            sgi_kona_work_drain(s, addr);
            return false;
        }
    }
    if (s->work_head == s->work_queue->len) {
        g_array_set_size(s->work_queue, 0);
        s->work_head = 0;
    }
    g_array_append_val(s->work_queue, w);
    sgi_head_work_schedule(&s->work);
    return true;
}

/*
 * The pipe has drained: the FIFO low-watermark cause, when the driver has
 * enabled it. The driver enables it only while a thread waits for the
 * pipe to drain below its threshold (its flow-control handler), and that
 * threshold is the credit register's level 0x300, which the model reads
 * while work waits as a full FIFO (sgi_kona_read_word()). Model choice:
 * the cause is raised when the work and the words behind it are all done;
 * when a real pipe raises it, against which register, is unknown.
 */
static void sgi_kona_work_lowater(SGIKonaState *s)
{
    if (s->int_enable & SGI_KONA_INT_CAUSE_LOWATER) {
        s->int_cause |= SGI_KONA_INT_CAUSE_LOWATER;
        trace_sgi_kona_work("lowater", 0, s->int_enable);
        sgi_kona_raise_vector(s, true);
    }
}

static void sgi_kona_work_replay_one(SGIKonaState *s)
{
    SGIKonaQueuedWord w = g_array_index(s->work_queue, SGIKonaQueuedWord,
                                        s->work_head);

    uint64_t packets = s->pi_packets;

    s->work_head++;
    s->work_replaying = true;
    s->work_replay_node = w.node;
    if (!s->context_failed) {
        sgi_kona_write_word(s, w.addr, w.value, 4);
    }
    /* The pipe takes the store now, and its credit goes back. */
    sgi_kona_pi_credits(s, w.counted, packets);
    s->work_replay_node = NULL;
    s->work_replaying = false;
}

/*
 * Go on with the list in progress and then the words behind it, in
 * order, until the budget is spent or nothing is left. A waiting word may
 * invoke another list, which the loop then runs in turn.
 */
static void sgi_kona_work_run(SGIKonaState *s, SGIHeadWorkBudget *b)
{
    for (;;) {
        if (s->dma.list_busy && !sgi_kona_dma_list_step(s, b)) {
            return;
        }
        if (s->work_head == s->work_queue->len) {
            return;
        }
        sgi_kona_work_replay_one(s);
        if (!s->dma.list_busy && sgi_head_work_spent(b)) {
            return;
        }
    }
}

static void sgi_kona_work_step(void *opaque, SGIHeadWorkBudget *b)
{
    SGIKonaState *s = opaque;

    sgi_kona_work_run(s, b);
    if (sgi_kona_work_pending(s)) {
        return;
    }
    g_array_set_size(s->work_queue, 0);
    s->work_head = 0;
    /*
     * The stores a framer still held when the list began go back once
     * nothing holds them: no store may arrive to return them.
     */
    sgi_kona_pi_credits(s, false, s->pi_packets);
    sgi_kona_work_lowater(s);
}

static bool sgi_kona_work_pending_cb(void *opaque)
{
    return sgi_kona_work_pending(opaque);
}

static const SGIHeadWorkOps sgi_kona_work_ops = {
    .step = sgi_kona_work_step,
    .pending = sgi_kona_work_pending_cb,
};

/* Abandon the list in progress and the words behind it. */
static void sgi_kona_work_abandon(SGIKonaState *s)
{
    if (!s->work_queue) {
        return;
    }
    sgi_head_work_wait_slice(&s->work);
    sgi_kona_dma_list_abandon(s);
    g_array_set_size(s->work_queue, 0);
    s->work_head = 0;
    sgi_head_work_idle(&s->work);
}

/*
 * Display-list work is finished before the machine stops, so a stopped
 * machine, and any state saved from it, has none in progress. Finishing
 * may write guest memory, which a migration then still sends.
 */
static void sgi_kona_work_vm_state(void *opaque, bool running,
                                   RunState state)
{
    if (!running) {
        sgi_head_work_enter_work();
        sgi_kona_work_drain(opaque, UINT64_MAX);
        sgi_head_work_leave();
    }
}

static void sgi_kona_work_init(SGIKonaState *s)
{
    s->work_queue = g_array_new(false, false, sizeof(SGIKonaQueuedWord));
    sgi_head_work_init(&s->work, "sgi-kona-work", &sgi_kona_work_ops, s);
    s->work_vm_state = qemu_add_vm_change_state_handler(
        sgi_kona_work_vm_state, s);
}

static void sgi_kona_work_cleanup(SGIKonaState *s)
{
    if (!s->work_queue) {
        return;
    }
    qemu_del_vm_change_state_handler(s->work_vm_state);
    s->work_vm_state = NULL;
    sgi_head_work_cleanup(&s->work);
    sgi_kona_dma_list_abandon(s);
    g_array_free(s->work_queue, true);
    s->work_queue = NULL;
    s->work_head = 0;
}

/*
 * An access from inside any SGI head's access or work on this thread: a
 * board's DMA aimed through the Crossbow at these registers. Refused and
 * logged, as QEMU's re-entrancy guard, which these registers opt out of,
 * would refuse it.
 */
static MemTxResult sgi_kona_refuse(hwaddr addr, bool write)
{
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: %s at 0x%" HWADDR_PRIx
                  " from inside a graphics head's work refused\n",
                  write ? "write" : "read", addr);
    return MEMTX_ACCESS_ERROR;
}

static MemTxResult sgi_kona_read(void *opaque, hwaddr addr,
                                 uint64_t *data, unsigned size,
                                 MemTxAttrs attrs)
{
    SGIKonaState *s = opaque;
    int cpu = current_cpu ? current_cpu->cpu_index : -1;
    uint64_t sequence;
    uint64_t value;

    if (!sgi_head_work_enter()) {
        *data = 0;
        return sgi_kona_refuse(addr, false);
    }
    if (sgi_kona_access_kind(addr, false) != SGI_KONA_ACCESS_NOW ||
        (size == 8 &&
         sgi_kona_access_kind(addr + 4, false) != SGI_KONA_ACCESS_NOW)) {
        sgi_kona_work_drain(s, addr);
    }
    sequence = s->trace_sequence++;
    if (size != 8) {
        value = sgi_kona_read_word(opaque, addr, size);
    } else {
        sgi_kona_log_split(addr, false);
        value = (sgi_kona_read_word(opaque, addr, 4) << 32) |
                sgi_kona_read_word(opaque, addr + 4, 4);
    }
    trace_sgi_kona_pio_read(sequence, cpu, addr, value, size);
    sgi_head_work_leave();
    *data = value;
    return MEMTX_OK;
}

/*
 * One word of a store, behind the work, after it, or at once
 * (sgi_kona_access_kind()). True when the word was kept to run later.
 */
static bool sgi_kona_store_word(SGIKonaState *s, hwaddr addr,
                                uint32_t value, bool counted)
{
    switch (sgi_kona_access_kind(addr, true)) {
    case SGI_KONA_ACCESS_QUEUES:
        if (sgi_kona_work_defer(s, addr, value, counted)) {
            return true;
        }
        break;
    case SGI_KONA_ACCESS_WAITS:
        sgi_kona_work_drain(s, addr);
        break;
    default:
        break;
    }
    sgi_kona_write_word(s, addr, value, 4);
    return false;
}

/*
 * One processor store, whatever its width: the graphics credit it was
 * charged is sampled here, once, before anything it carries is acted on.
 */
static MemTxResult sgi_kona_write(void *opaque, hwaddr addr,
                                  uint64_t value, unsigned size,
                                  MemTxAttrs attrs)
{
    SGIKonaState *s = opaque;
    bool counted = sgi_kona_store_counted();
    uint64_t packets = s->pi_packets;
    int cpu = current_cpu ? current_cpu->cpu_index : -1;
    bool queued;
    Object *source = sgi_pi_gfx_store_source();

    if (!sgi_head_work_enter()) {
        return sgi_kona_refuse(addr, true);
    }
    if (source) {
        s->pi_store_node = source;
    }
    if (s->context_failed) {
        sgi_head_work_leave();
        return MEMTX_OK;
    }
    trace_sgi_kona_pio_write(s->trace_sequence++, cpu, addr, value, size);

    /*
     * A store whose words wait behind the work keeps its credit until the
     * work takes them: its last word carries it (sgi_kona_work_replay_one()).
     * The graphics credits so bound the words that can wait to the
     * pipe's FIFO, as on the board; the processor interface's flow control
     * then holds the storing thread back (hw/mips/sgi/pi-gfx.c).
     */
    if (size != 8) {
        queued = sgi_kona_store_word(s, addr, value, counted);
    } else if (sgi_kona_doubleword_data(addr)) {
        queued = sgi_kona_store_word(s, addr, value >> 32, false);
        queued |= sgi_kona_store_word(s, addr, (uint32_t)value, counted);
    } else {
        sgi_kona_log_split(addr, true);
        queued = sgi_kona_store_word(s, addr, value >> 32, false);
        queued |= sgi_kona_store_word(s, addr + 4, (uint32_t)value,
                                      counted);
    }
    if (!queued) {
        sgi_kona_pi_credits(s, counted, packets);
    }
    sgi_head_work_leave();
    return MEMTX_OK;
}

/*
 * Crosstalk widget registers are word-sized; both bounds are declared so
 * the core rejects an access outside them instead of reshaping it,
 * matching hw/pci-host/sgi-bridge.c's own register file. The maximum is 8
 * so an 8-byte access reaches sgi_kona_write() or sgi_kona_read() whole
 * and is split there, where the command page's doubleword store can be
 * told apart.
 */
static const MemoryRegionOps sgi_kona_ops = {
    .read_with_attrs = sgi_kona_read,
    .write_with_attrs = sgi_kona_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static bool sgi_kona_build_widget(SGIXioBoardState *board,
                                  Error **errp)
{
    SGIKonaState *s = SGI_KONA(board);

    if (!s->fabric) {
        error_setg(errp, "sgi-kona requires the fabric its "
                  "packets travel");
        return false;
    }
    if (s->identity_index >= SGI_KONA_IDENTITY_SLOTS ||
        (s->shared_identity_index != SGI_KONA_OWN_IDENTITY &&
         s->shared_identity_index >= SGI_KONA_IDENTITY_SLOTS)) {
        error_setg(errp, "sgi-kona: identity-index and shared-identity-index "
                   "take 0 to %u: each pipe slot's records use sixteen "
                   "graphics serial instances, and boards placed by slot "
                   "name start theirs at 64", SGI_KONA_IDENTITY_SLOTS - 1);
        return false;
    }
    if (!sgi_kona_configure(s, errp)) {
        return false;
    }

    memory_region_init_io(&s->regs, OBJECT(s), &sgi_kona_ops, s,
                          "sgi-kona.regs",
                          SGI_KONA_APERTURE_SIZE);
    /*
     * An access that must wait for display-list work sleeps inside its
     * handler with the big lock let go (sgi_head_work_wait_slice()), and a
     * second processor's access may then arrive while the first is still
     * in the device. QEMU's re-entrancy guard is per device, not per
     * thread, and would refuse that access. The handlers keep a guard per
     * thread instead (sgi_head_work_enter()), which still refuses a
     * board's own DMA arriving here from inside its work or another
     * head's.
     */
    s->regs.disable_reentrancy_guard = true;
    s->reason_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sgi_kona_reason_timer,
                                   s);
    board->cpu_face = &s->regs;
    board->packet_face = &s->regs;
    return true;
}

static bool sgi_kona_list_size_valid(SGIKonaState *s, Error **errp)
{
    if (!s->list_memory_size ||
        s->list_memory_size % SGI_KONA_LIST_ROW_BYTES ||
        s->list_memory_size > SGI_KONA_LIST_MAX) {
        error_setg(errp, "sgi-kona list-memory-size %u is not a nonzero "
                   "multiple of %u no larger than %u", s->list_memory_size,
                   SGI_KONA_LIST_ROW_BYTES, (unsigned)SGI_KONA_LIST_MAX);
        return false;
    }
    return true;
}

/*
 * Board memory and its planes, laid out as the header comment on
 * SGI_KONA_TAG_DIRECT gives them, in the order that fixes their ids. The
 * list size was checked by sgi_kona_list_size_valid() first.
 */
static void sgi_kona_board_memory_init(SGIKonaState *s)
{
    const uint64_t pixels = s->fb_capacity;
    const SGIPlane colour = {
        .len = pixels * 4,
        .width = s->fb_width,
        .height = s->fb_height,
        .stride = s->fb_width * 4,
        .format = SGI_PLANE_RGB24,
        .packing = SGI_PACK_LE_RED_HIGH,
        .bottom_origin = true,
    };
    const SGIPlane tag = {
        .len = pixels,
        .width = s->fb_width,
        .height = s->fb_height,
        .stride = s->fb_width,
        .format = SGI_PLANE_TAG,
        .packing = SGI_PACK_LE_RED_LOW,
        .bottom_origin = true,
    };
    const SGIPlane list = {
        .len = s->list_memory_size,
        .width = SGI_KONA_LIST_ROW_BYTES / 4,
        .height = s->list_memory_size / SGI_KONA_LIST_ROW_BYTES,
        .stride = SGI_KONA_LIST_ROW_BYTES,
        .format = SGI_PLANE_RAW,
        .packing = SGI_PACK_BE_RED_LOW,
    };
    /*
     * One depth buffer for the screen, as GL gives a window one shared by
     * its front and back buffers, addressed like the colour planes. The
     * pipe's depth layout is unknown; 24 bits is the model's rules'.
     */
    const SGIPlane depth = {
        .len = pixels * 4,
        .width = s->fb_width,
        .height = s->fb_height,
        .stride = s->fb_width * 4,
        .format = SGI_PLANE_Z24,
        .packing = SGI_PACK_LE_RED_LOW,
        .bottom_origin = true,
    };
    SGIPlane p;
    uint64_t base = 0;

    sgi_board_memory_init(&s->mem, 2 * colour.len + 2 * tag.len + list.len +
                          depth.len + pixels);

    p = colour;
    p.base = base;
    base += p.len;
    s->plane_front = sgi_board_memory_add_plane(&s->mem, &p, &error_abort);
    p.base = base;
    base += p.len;
    s->plane_back = sgi_board_memory_add_plane(&s->mem, &p, &error_abort);
    p = tag;
    p.base = base;
    base += p.len;
    s->plane_front_tag = sgi_board_memory_add_plane(&s->mem, &p,
                                                    &error_abort);
    p.base = base;
    base += p.len;
    s->plane_back_tag = sgi_board_memory_add_plane(&s->mem, &p,
                                                   &error_abort);
    p = list;
    p.base = base;
    base += p.len;
    /* The sizes fit by construction, so a failure is a model bug. */
    s->plane_list = sgi_board_memory_add_plane(&s->mem, &p, &error_abort);
    p = depth;
    p.base = base;
    s->plane_depth = sgi_board_memory_add_plane(&s->mem, &p, &error_abort);
    base += p.len;
    p = (SGIPlane) {
        .base = base,
        .len = pixels,
        .width = s->fb_width,
        .height = s->fb_height,
        .stride = s->fb_width,
        .format = SGI_PLANE_S8,
        .packing = SGI_PACK_LE_RED_LOW,
        .bottom_origin = true,
    };
    s->plane_stencil = sgi_board_memory_add_plane(&s->mem, &p, &error_abort);
}

/*
 * The console and the textport command port start here rather than in
 * build_widget(): qemu_graphic_console_create() registers this device's
 * QOM path with the UI layer, and that path is exactly what the base
 * class's re-parent under the module changes, so creating it any earlier
 * risks registering the console under a path the board no longer has once
 * realize finishes. The textport command port itself has no such
 * constraint (it is a plain struct, not tied to QOM identity), but is
 * initialized alongside for one increment-2 setup site. The one-wire
 * identity chain is the base class's, built before this runs.
 */
static bool sgi_kona_finish_realize(SGIXioBoardState *board, Error **errp)
{
    SGIKonaState *s = SGI_KONA(board);
    SGIKonaMesaPlanes planes;
    /*
     * Before anything this function sets up: finalize relies on a board
     * whose realize failed never having reached address_space_init().
     */
    if (!sgi_kona_list_size_valid(s, errp)) {
        return false;
    }
    if (!sgi_kona_flash_init(s, errp)) {
        return false;
    }

    sgi_command_port_init(&s->textport, &sgi_kona_textport_ops, s,
                          SGI_KONA_PORT_TEXTPORT, sgi_kona_textport_selectors,
                          sgi_kona_textport_num_selectors);
    s->textport_expect_sync = true;
    sgi_command_port_init(&s->kernel_port, &sgi_kona_kernel_ops, s,
                          SGI_KONA_PORT_FIFO, sgi_kona_command_tokens,
                          sgi_kona_command_num_tokens);
    sgi_command_port_init(&s->gl_port, &sgi_kona_gl_ops, s,
                          SGI_KONA_PORT_DOORBELL, sgi_kona_gl_tokens,
                          sgi_kona_gl_num_tokens);

    s->map_ram = g_new0(uint32_t, SGI_KONA_MAPRAM_ENTRIES);
    s->map_ram_count = SGI_KONA_MAPRAM_ENTRIES;

    s->arm_high = g_new0(uint32_t, SGI_KONA_ARM_HIGH_WORDS);
    s->arm_high_count = SGI_KONA_ARM_HIGH_WORDS;

    s->fb_capacity = sgi_kona_fb_capacity(s);
    s->fb_width = SGI_KONA_FB_WIDTH;
    s->fb_height = SGI_KONA_FB_HEIGHT;
    s->cfg_width = SGI_KONA_FB_WIDTH;
    s->cfg_height = SGI_KONA_FB_HEIGHT;
    sgi_kona_board_memory_init(s);
    planes = (SGIKonaMesaPlanes) {
        .front = s->plane_front,
        .back = s->plane_back,
        .front_tag = s->plane_front_tag,
        .back_tag = s->plane_back_tag,
        .depth = s->plane_depth,
        .stencil = s->plane_stencil,
        .direct_tag = SGI_KONA_TAG_DIRECT,
    };
    s->mesa_provider = sgi_kona_mesa_provider_new(
        &s->mem, &planes, &sgi_kona_mesa_dirty_ops, s, errp);
    if (!s->mesa_provider) {
        return false;
    }
    /*
     * The provider can fail construction; initialise the fabric address
     * space only after that last fallible step. Finalize owns its teardown.
     */
    address_space_init(&s->fabric_as, s->fabric, "sgi-kona.fabric");
    if (s->flash_blk) {
        s->flash_vm_change =
            qemu_add_vm_change_state_handler(sgi_kona_flash_vm_change, s);
    }
    sgi_kona_dma_init(&s->dma, s);
    /* RAW32 is model storage for DID16, two overlay16 planes and popup2. */
    sgi_board_memory_init(&s->desktop_mem, s->fb_capacity * 16);
    for (unsigned i = 0; i < 4; i++) {
        SGIPlane plane = {
            .base = (uint64_t)i * s->fb_capacity * 4,
            .len = s->fb_capacity * 4,
            .width = s->fb_width,
            .height = s->fb_height,
            .stride = s->fb_width * 4,
            .format = SGI_PLANE_RAW,
            .packing = SGI_PACK_LE_RED_LOW,
            .bottom_origin = true,
        };

        sgi_board_memory_add_plane(&s->desktop_mem, &plane, &error_abort);
    }
    sgi_board_memory_init(&s->x_write_mem, s->fb_capacity * 9);
    for (unsigned i = 0; i < 3; i++) {
        unsigned bytes = i == 1 ? 1 : 4;
        SGIPlane plane = {
            .base = s->fb_capacity * (i == 0 ? 0 : i == 1 ? 4 : 5),
            .len = s->fb_capacity * bytes,
            .width = s->fb_width,
            .height = s->fb_height,
            .stride = s->fb_width * bytes,
            .format = i == 1 ? SGI_PLANE_TAG : SGI_PLANE_RAW,
            .packing = SGI_PACK_LE_RED_LOW,
        };

        sgi_board_memory_add_plane(&s->x_write_mem, &plane, &error_abort);
    }
    for (unsigned c = 0; c < SGI_KONA_MAX_CHANNELS; c++) {
        s->channel_hook[c] = (SGIKonaChannelHook) { .kona = s, .index = c };
    }
    sgi_scanout_init(&s->scanout, DEVICE(s), &sgi_kona_head_ops, s);
    s->scanout.composite_cursor = true;
    s->channel_scanouts = sgi_kona_channel_count(s) - 1;
    for (unsigned c = 1; c < sgi_kona_channel_count(s); c++) {
        SGIScanout *so = sgi_kona_channel_scanout(s, c);

        sgi_scanout_init_head(so, DEVICE(s), c, &sgi_kona_channel_head_ops,
                              &s->channel_hook[c]);
        so->composite_cursor = true;
    }
    sgi_kona_channels_reset(s);
    sgi_kona_channels_apply(s);
    sgi_kona_work_init(s);
    return true;
}

/*
 * Host-only qtest access to the front bank's pixels: resolved by tag as
 * sgi_kona_fb_read() does, top byte of a direct word included, not the
 * masked line the scanout presents.
 *
 * test_pixel_x/test_pixel_y are wire coordinates, the same convention
 * sgi_kona_draw_point() accepts, not raw framebuffer row/column: this
 * applies that function's own bottom-up row flip so a test reading back
 * (x, y) here sees the pixel a drawing command actually placed at wire
 * coordinate (x, y), independent of framebuffer storage order.
 */
static void sgi_kona_test_get_pixel(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    uint32_t value = 0;

    if (DEVICE(obj)->realized && s->test_pixel_x < s->fb_width &&
        s->test_pixel_y < s->fb_height) {
        value = sgi_kona_fb_read(s, s->test_pixel_x, s->test_pixel_y);
    }
    visit_type_uint32(v, name, &value, errp);
}

/*
 * Host-only qtest bulk form of x-test-pixel: the x-test-pixel-count
 * pixels along the line from (x-test-pixel-x, x-test-pixel-y), each what
 * x-test-pixel returns there, as consecutive eight-digit lower-case hex
 * words in one string. No guest-visible state.
 */
static void sgi_kona_test_get_pixel_row(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    g_autoptr(GString) str = g_string_new(NULL);
    char *out;

    for (uint32_t i = 0; i < s->test_pixel_count; i++) {
        uint32_t x = s->test_pixel_x + i;
        uint32_t value = 0;

        if (DEVICE(obj)->realized && x < s->fb_width &&
            s->test_pixel_y < s->fb_height) {
            value = sgi_kona_fb_read(s, x, s->test_pixel_y);
        }
        g_string_append_printf(str, "%08x", value);
    }
    out = str->str;
    visit_type_str(v, name, &out, errp);
}

/*
 * Host-only qtest access to board memory through the shared plane load:
 * the word of plane x-test-plane at (x-test-pixel-x, x-test-pixel-y),
 * in plane coordinates. A plane id the table does not hold reads 0.
 */
static void sgi_kona_test_get_plane_word(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    SGIBoardMemory *memory = s->test_plane < s->mem.nplanes ?
                             &s->mem : &s->desktop_mem;
    unsigned id = s->test_plane < s->mem.nplanes ? s->test_plane :
                  s->test_plane - s->mem.nplanes;
    const SGIPlane *p = NULL;
    uint32_t value = 0;

    if (DEVICE(obj)->realized) {
        p = sgi_board_memory_plane(memory, id);
    }
    if (p) {
        value = sgi_plane_load(memory, p, s->test_pixel_x, s->test_pixel_y);
    }
    visit_type_uint32(v, name, &value, errp);
}

/*
 * Host-only qtest bulk form of x-test-plane-word: the x-test-pixel-count
 * words of plane x-test-plane along the line starting at (x-test-pixel-x,
 * x-test-pixel-y), each what x-test-plane-word returns there, as
 * consecutive eight-digit lower-case hex words in one string. One QMP
 * command replaces a line of per-pixel round trips; no guest-visible state.
 */
static void sgi_kona_test_get_plane_row(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    SGIBoardMemory *memory = s->test_plane < s->mem.nplanes ?
                             &s->mem : &s->desktop_mem;
    unsigned id = s->test_plane < s->mem.nplanes ? s->test_plane :
                  s->test_plane - s->mem.nplanes;
    const SGIPlane *p = DEVICE(obj)->realized ?
                        sgi_board_memory_plane(memory, id) : NULL;
    g_autoptr(GString) str = g_string_new(NULL);
    char *out;

    for (uint32_t i = 0; i < s->test_pixel_count; i++) {
        uint32_t value = 0;

        if (p) {
            value = sgi_plane_load(memory, p, s->test_pixel_x + i,
                                   s->test_pixel_y);
        }
        g_string_append_printf(str, "%08x", value);
    }
    out = str->str;
    visit_type_str(v, name, &out, errp);
}

/*
 * Host-only qtest write of plane, x and y together, so a test selecting a
 * point for x-test-plane-word needs one QMP command and not three. The
 * value is plane << 48 | y << 24 | x, with x and y each below 2^24; it
 * sets x-test-plane, x-test-pixel-y and x-test-pixel-x and nothing else.
 */
static void sgi_kona_test_set_plane_select(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    s->test_plane = value >> 48;
    s->test_pixel_y = (value >> 24) & 0xffffff;
    s->test_pixel_x = value & 0xffffff;
}

/*
 * SHA-256 of the bytes of plane `plane`, numbered as x-test-plane numbers
 * them, or NULL when it has no device-backed storage.
 */
static char *sgi_kona_test_plane_digest(SGIKonaState *s, unsigned plane)
{
    SGIBoardMemory *memory = plane < s->mem.nplanes ? &s->mem :
                             &s->desktop_mem;
    unsigned id = plane < s->mem.nplanes ? plane : plane - s->mem.nplanes;
    const SGIPlane *p = DEVICE(s)->realized ?
                        sgi_board_memory_plane(memory, id) : NULL;

    if (!p || !memory->bytes || p->base > memory->bytes_len ||
        p->len > memory->bytes_len - p->base) {
        return NULL;
    }
    /* The rows the plane is laid out in, not the room past them. */
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                       memory->bytes + p->base,
                                       MIN(p->len,
                                           (uint64_t)p->stride * p->height));
}

/*
 * The plane's laid-out bytes, for preservation and migration
 * comparisons.
 */
static void sgi_kona_test_get_plane_digest(Object *obj, Visitor *v,
                                           const char *name, void *opaque,
                                           Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    g_autofree char *digest = sgi_kona_test_plane_digest(s, s->test_plane);

    if (!digest) {
        error_setg(errp, "Selected KONA plane has no device-backed storage");
        return;
    }
    visit_type_str(v, name, &digest, errp);
}

/*
 * Host-only qtest bulk form of x-test-plane-digest: the digests of the
 * x-test-plane-count planes from x-test-plane on, as consecutive
 * sixty-four-digit hex strings in one string. A plane with no device-backed
 * storage reads as sixty-four dashes where the per-plane property fails.
 * No guest-visible state.
 */
static void sgi_kona_test_get_plane_digests(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    g_autoptr(GString) str = g_string_new(NULL);
    char *out;

    for (uint32_t i = 0; i < s->test_plane_count; i++) {
        g_autofree char *digest =
            sgi_kona_test_plane_digest(s, s->test_plane + i);

        if (digest) {
            g_string_append(str, digest);
        } else {
            for (unsigned k = 0; k < 64; k++) {
                g_string_append_c(str, '-');
            }
        }
    }
    out = str->str;
    visit_type_str(v, name, &out, errp);
}

static void sgi_kona_test_get_mesa_front_generation(Object *obj, Visitor *v,
                                                     const char *name,
                                                     void *opaque,
                                                     Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    const SGIRenderResource *resources;
    size_t count = 0;
    uint64_t generation = 0;

    if (s->mesa_provider) {
        resources = sgi_kona_mesa_resources(s->mesa_provider, &count);
        if (count) {
            generation = resources[0].backing_generation;
        }
    }
    visit_type_uint64(v, name, &generation, errp);
}

static bool sgi_kona_test_get_mesa_failed(Object *obj, Error **errp)
{
    return SGI_KONA(obj)->mesa_failed;
}

static void sgi_kona_test_set_mesa_failed(Object *obj, bool failed,
                                          Error **errp)
{
    SGI_KONA(obj)->mesa_failed = failed;
}

/* Host-only access to the selected gamma-table entry. */
static void sgi_kona_test_get_gamma(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    uint32_t value = 0;

    if (s->test_gamma_addr < SGI_KONA_GAMMA_ENTRIES) {
        value = s->gamma[s->test_gamma_addr];
    }
    visit_type_uint32(v, name, &value, errp);
}

/*
 * Host-only qtest access to the X graphics context: word test_gc_index,
 * or 0 past the last.
 */
static void sgi_kona_test_get_gc(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    uint32_t value = 0;

    if (s->test_gc_index < SGI_KONA_X_GC_WORDS) {
        value = s->x_gc[s->test_gc_index];
    }
    visit_type_uint32(v, name, &value, errp);
}

/* Host-only staging inspection uses the existing x-test-gc-index cursor. */
static void sgi_kona_test_get_gl_data_word(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    uint32_t value;

    if (s->test_gc_index >= SGI_KONA_GL_DATA_MAX) {
        error_setg(errp, "GL staging index exceeds model capacity");
        return;
    }
    value = s->gl_data[s->test_gc_index];
    visit_type_uint32(v, name, &value, errp);
}

/*
 * Host-only qtest bulk form of x-test-gl-data-word: all
 * SGI_KONA_GL_DATA_MAX staging words, word 0 first, as consecutive
 * eight-digit lower-case hex words in one string. No guest-visible state.
 */
static void sgi_kona_test_get_gl_data_words(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    g_autoptr(GString) str = g_string_new(NULL);
    char *out;

    for (unsigned i = 0; i < SGI_KONA_GL_DATA_MAX; i++) {
        g_string_append_printf(str, "%08x", s->gl_data[i]);
    }
    out = str->str;
    visit_type_str(v, name, &out, errp);
}

/* Inspect the same immutable texture view supplied to software draws. */
static void sgi_kona_test_get_texture_pixel(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    const SGIGLTexImage *image = sgi_kona_texture_resolve(&s->textures);
    uint32_t value = 0;

    if (image && image->pyramid &&
        image->pyramid->count <= SGI_GL_TEX_LEVELS_MAX) {
        const SGIGLTexPyramid *p = image->pyramid;

        for (unsigned k = 0; k < p->count; k++) {
            const SGIGLTexLevel *l = &p->levels[k];

            if (l->level == p->base_level) {
                l->read_texel(l->opaque, l->level, s->test_pixel_x,
                              s->test_pixel_y, &value);
                break;
            }
        }
    }
    visit_type_uint32(v, name, &value, errp);
}

/* The existing test coordinates select a filter slot and coefficient. */
static void sgi_kona_test_get_texture_curve(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    const double *curve = sgi_kona_texture_curve(&s->textures, s->test_pixel_x);
    double value = curve && s->test_pixel_y < 64 ? curve[s->test_pixel_y] : 0;

    visit_type_number(v, name, &value, errp);
}

static void sgi_kona_test_get_texture_curve_valid(Object *obj, Visitor *v,
                                                  const char *name,
                                                  void *opaque, Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    bool valid = sgi_kona_texture_curve(&s->textures, s->test_pixel_x) != NULL;

    visit_type_bool(v, name, &valid, errp);
}

/* Every draw a host-gpu board drew scalar, whatever its reason. */
static void sgi_kona_test_get_gpu_fallbacks(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    uint64_t total = 0;

    for (unsigned i = 0; i < SGI_KONA_FALLBACK__MAX; i++) {
        total += s->renderer_fallbacks[i];
    }
    visit_type_uint64(v, name, &total, errp);
}

static void sgi_kona_test_set_renderer_failure(Object *obj, Visitor *v,
                                               const char *name, void *opaque,
                                               Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    uint32_t nth_draw;

    if (!qtest_enabled() || !s->host_gpu) {
        error_setg(errp, "KONA renderer fault requires qtest and host-gpu");
        return;
    }
    if (!visit_type_uint32(v, name, &nth_draw, errp)) {
        return;
    }
    if (s->renderer) {
        sgi_renderer_test_fail_draw(s->renderer, nth_draw);
        s->test_renderer_fail_draw = 0;
    } else {
        s->test_renderer_fail_draw = nth_draw;
    }
}

static void sgi_kona_test_set_renderer_fail_hold(Object *obj, Visitor *v,
                                                 const char *name,
                                                 void *opaque, Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    uint32_t nth;

    if (!qtest_enabled() || !s->host_gpu) {
        error_setg(errp, "KONA held-write fault requires qtest and host-gpu");
        return;
    }
    if (!visit_type_uint32(v, name, &nth, errp)) {
        return;
    }
    s->test_renderer_fail_hold = nth;
}

static void sgi_kona_test_set_renderer_skew(Object *obj, bool skew,
                                            Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);

    if (!qtest_enabled() || !s->host_gpu) {
        error_setg(errp, "KONA renderer skew requires qtest and host-gpu");
        return;
    }
    s->test_renderer_skew = skew;
}

static void sgi_kona_test_set_renderer_create_failure(Object *obj, Visitor *v,
                                                      const char *name,
                                                      void *opaque,
                                                      Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    uint32_t result;

    if (!qtest_enabled() || !s->host_gpu || s->renderer) {
        error_setg(errp, "KONA renderer creation fault requires an uncreated "
                   "host-gpu renderer in qtest");
        return;
    }
    if (!visit_type_uint32(v, name, &result, errp)) {
        return;
    }
    if (result > SGI_RENDER_CREATE_HOST_ERROR) {
        error_setg(errp, "Invalid SGI renderer creation result %u", result);
        return;
    }
    s->test_renderer_fail_create = result;
}

static bool sgi_kona_test_get_context_alloc_fail(Object *obj, Error **errp)
{
    return SGI_KONA(obj)->test_context_alloc_fail;
}

static void sgi_kona_test_set_context_alloc_fail(Object *obj, bool fail,
                                               Error **errp)
{
    SGI_KONA(obj)->test_context_alloc_fail = fail;
}

static bool sgi_kona_test_get_context_failed(Object *obj, Error **errp)
{
    return SGI_KONA(obj)->context_failed;
}

static void sgi_kona_test_get_context_current(Object *obj, Visitor *v,
                                              const char *name, void *opaque,
                                              Error **errp)
{
    int32_t current = SGI_KONA(obj)->context_current;

    visit_type_int32(v, name, &current, errp);
}

static void sgi_kona_test_get_context_error(Object *obj, Visitor *v,
                                            const char *name, void *opaque,
                                            Error **errp)
{
    g_autofree char *message = g_strdup(SGI_KONA(obj)->context_error);

    visit_type_str(v, name, &message, errp);
}

static void sgi_kona_test_get_context_digest(Object *obj, Visitor *v,
                                             const char *name, void *opaque,
                                             Error **errp)
{
    SGIKonaState *s = SGI_KONA(obj);
    g_autoptr(GChecksum) sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_autofree char *digest = NULL;

    g_checksum_update(sum, (uint8_t *)s->contexts, sizeof(s->contexts));
    g_checksum_update(sum, (uint8_t *)&s->context_clock,
                       sizeof(s->context_clock));
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        if (s->state[i]) {
            g_checksum_update(sum, (uint8_t *)s->state[i],
                               sizeof(*s->state[i]));
        }
    }
    digest = g_strdup(g_checksum_get_string(sum));
    visit_type_str(v, name, &digest, errp);
}

static bool sgi_kona_test_get_work_pending(Object *obj, Error **errp)
{
    return sgi_kona_work_pending(SGI_KONA(obj));
}

static void sgi_kona_instance_init(Object *obj)
{
    SGIKonaState *s = SGI_KONA(obj);

    sgi_kona_texture_init(&s->textures);
    s->context_current = SGI_KONA_CONTEXT_NONE;
    object_property_add(obj, "x-test-context-digest", "str",
                        sgi_kona_test_get_context_digest, NULL, NULL, NULL);
    object_property_add(obj, "x-test-context-current", "int32",
                        sgi_kona_test_get_context_current, NULL, NULL, NULL);
    object_property_add(obj, "x-test-context-error", "str",
                        sgi_kona_test_get_context_error, NULL, NULL, NULL);
    object_property_add_bool(obj, "x-test-context-alloc-fail",
                             sgi_kona_test_get_context_alloc_fail,
                             sgi_kona_test_set_context_alloc_fail);
    object_property_add_uint32_ptr(obj, "x-test-context-count",
                                   &s->context_count, OBJ_PROP_FLAG_READ);
    object_property_add_bool(obj, "x-test-context-failed",
                             sgi_kona_test_get_context_failed, NULL);

    object_property_add_uint32_ptr(obj, "x-test-gl-data-have", &s->gl_data_have,
                                   OBJ_PROP_FLAG_READ);
    object_property_add(obj, "x-test-gl-data-word", "uint32",
                        sgi_kona_test_get_gl_data_word, NULL, NULL, NULL);
    object_property_add(obj, "x-test-gl-data-words", "str",
                        sgi_kona_test_get_gl_data_words, NULL, NULL, NULL);
    /*
     * Display-list work in slices of x-test-slice-words list words rather
     * than of host time, and whether any is still waiting to run.
     */
    object_property_add_uint32_ptr(obj, "x-test-slice-words",
                                   &s->work.test_words,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_bool(obj, "x-test-work-pending",
                             sgi_kona_test_get_work_pending, NULL);
    object_property_add_uint32_ptr(obj, "x-test-pixel-x", &s->test_pixel_x,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "x-test-pixel-y", &s->test_pixel_y,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-texture-pixel", "uint32",
                        sgi_kona_test_get_texture_pixel, NULL, NULL, NULL);
    object_property_add(obj, "x-test-texture-curve", "number",
                        sgi_kona_test_get_texture_curve, NULL, NULL, NULL);
    object_property_add(obj, "x-test-texture-curve-valid", "bool",
                        sgi_kona_test_get_texture_curve_valid,
                        NULL, NULL, NULL);
    object_property_add(obj, "x-test-pixel", "uint32",
                        sgi_kona_test_get_pixel, NULL, NULL, NULL);
    object_property_add(obj, "x-test-pixel-row", "str",
                        sgi_kona_test_get_pixel_row, NULL, NULL, NULL);
    object_property_add_uint32_ptr(obj, "x-test-plane", &s->test_plane,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-plane-word", "uint32",
                        sgi_kona_test_get_plane_word, NULL, NULL, NULL);
    object_property_add(obj, "x-test-config-block", "str",
                        sgi_kona_test_get_config_block, NULL, NULL, NULL);
    object_property_add_uint32_ptr(obj, "x-test-pixel-count",
                                   &s->test_pixel_count,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-plane-row", "str",
                        sgi_kona_test_get_plane_row, NULL, NULL, NULL);
    object_property_add(obj, "x-test-plane-select", "uint64",
                        NULL, sgi_kona_test_set_plane_select, NULL, NULL);
    object_property_add(obj, "x-test-plane-digest", "str",
                        sgi_kona_test_get_plane_digest, NULL, NULL, NULL);
    object_property_add_uint32_ptr(obj, "x-test-plane-count",
                                   &s->test_plane_count,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-plane-digests", "str",
                        sgi_kona_test_get_plane_digests, NULL, NULL, NULL);
    object_property_add(obj, "x-test-mesa-front-generation", "uint64",
                        sgi_kona_test_get_mesa_front_generation,
                        NULL, NULL, NULL);
    object_property_add_bool(obj, "x-test-mesa-failed",
                             sgi_kona_test_get_mesa_failed,
                             sgi_kona_test_set_mesa_failed);
    object_property_add_uint32_ptr(obj, "x-test-gamma-addr",
                                   &s->test_gamma_addr,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-gamma", "uint32",
                        sgi_kona_test_get_gamma, NULL, NULL, NULL);
    object_property_add_uint32_ptr(obj, "x-test-gc-index", &s->test_gc_index,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-gc", "uint32", sgi_kona_test_get_gc,
                        NULL, NULL, NULL);
    object_property_add(obj, "x-test-gpu-fail-draw", "uint32", NULL,
                        sgi_kona_test_set_renderer_failure, NULL, NULL);
    object_property_add(obj, "x-test-gpu-fail-create", "uint32", NULL,
                        sgi_kona_test_set_renderer_create_failure,
                        NULL, NULL);
    object_property_add_bool(obj, "x-test-gpu-skew-legacy", NULL,
                             sgi_kona_test_set_renderer_skew);
    object_property_add(obj, "x-test-gpu-fail-hold", "uint32", NULL,
                        sgi_kona_test_set_renderer_fail_hold, NULL, NULL);
    object_property_add_uint64_ptr(obj, "x-test-gpu-requests",
                                   &s->renderer_requests, OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "x-test-gpu-draws", &s->renderer_draws,
                                   OBJ_PROP_FLAG_READ);
    object_property_add(obj, "x-test-gpu-fallbacks", "uint64",
                        sgi_kona_test_get_gpu_fallbacks, NULL, NULL, NULL);
    for (unsigned i = SGI_RENDER_REFUSAL_NONE + 1; i < SGI_KONA_FALLBACK__MAX;
         i++) {
        g_autofree char *name = g_strdup_printf("x-gpu-fallbacks-%s",
                                                sgi_kona_fallback_name(i));

        object_property_add_uint64_ptr(obj, name, &s->renderer_fallbacks[i],
                                       OBJ_PROP_FLAG_READ);
    }
    sgi_kona_dma_test_props_init(obj, &s->dma);
}

/*
 * Increment 2's own host-only buffers (framebuffer, an in-flight bitmap
 * payload) and console have no unrealize hook to release them from:
 * sgi-xio-board fixes dc->unrealize to sgi_xio_board_unrealize(), which
 * only removes a board from the pending-attach list (see hw/mips/sgi/
 * xio-board.c), and SGIXioBoardClass declares no finish_unrealize
 * counterpart to finish_realize(). instance_finalize runs unconditionally
 * on object destruction regardless of that realize/unrealize contract, so
 * it is the right place instead -- self-contained here, with no change to
 * the shared xio-board base class. Every field is guarded for the unset
 * case (a board that never realized, or whose realize failed before
 * reaching finish_realize()).
 */
static void sgi_kona_instance_finalize(Object *obj)
{
    SGIKonaState *s = SGI_KONA(obj);

    sgi_kona_texture_cleanup(&s->textures);
    sgi_kona_gl_read_dma_clear(s);
    sgi_kona_gl_image_clear(s);
    sgi_kona_texture_upload_clear(s);
    sgi_kona_renderer_stop(s);
    sgi_kona_mesa_provider_free(s->mesa_provider);
    if (s->scanout.con) {
        sgi_scanout_cleanup(&s->scanout);
    }
    for (unsigned c = 1; c < SGI_KONA_MAX_CHANNELS; c++) {
        if (s->channel_scanout[c - 1].con) {
            sgi_scanout_cleanup(&s->channel_scanout[c - 1]);
        }
    }
    /*
     * fabric_as is an embedded AddressSpace, not a pointer: a board that
     * never reached finish_realize() never called address_space_init()
     * on it, so there is no zeroed-pointer check to guard this the way
     * s->scanout/s->bitmap_payload are guarded. Nothing after that call
     * in finish_realize() can fail: every refusal, the list size's
     * included, comes before it, so a board that failed to realize never
     * initialised the address space either.
     * DEVICE(obj)->realized is the same "did finish_realize() actually
     * run" signal sgi_kona_test_get_pixel() already trusts for board
     * memory, set up in that same function.
     */
    if (DEVICE(obj)->realized) {
        address_space_destroy(&s->fabric_as);
    }
    sgi_kona_contexts_clear(s);
    sgi_board_memory_cleanup(&s->mem);
    sgi_board_memory_cleanup(&s->desktop_mem);
    sgi_board_memory_cleanup(&s->x_write_mem);
    sgi_board_memory_cleanup(&s->accum_mem);
    if (s->reason_timer) {
        timer_free(s->reason_timer);
        s->reason_timer = NULL;
    }
    g_free(s->bitmap_payload);
    g_free(s->map_ram);
    g_free(s->arm_high);
}

/*
 * Every guest-writable field this widget carries returns to its
 * post-realize value: widget_control and onewire_reg (the original,
 * identity-only register window), plus increment 2's own register
 * window, mailbox queue, textport command port and framebuffer.
 * manufacturer/part/revision/probe_value are qdev properties fixed at
 * construction, not guest state, so reset leaves them alone -- matching
 * how sgi-bridge.c's own sgi_bridge_reset_core() recomputes its
 * identification register from properties rather than clearing them.
 *
 * test_pixel_x/test_pixel_y are excluded: per their own header comment,
 * they are host-only qtest introspection coordinates, not guest state,
 * so a guest-triggered reset has no more claim on them than it does on
 * any other out-of-band debug property.
 */
static void sgi_kona_reset_hold(Object *obj, ResetType type)
{
    SGIKonaState *s = SGI_KONA(obj);

    /* Reset abandons the display list in progress and the words behind. */
    sgi_kona_work_abandon(s);

    sgi_kona_texture_reset(&s->textures);
    sgi_kona_texture_upload_clear(s);
    sgi_kona_gl_image_clear(s);
    memset(&s->gl_bitmap_upload, 0, sizeof(s->gl_bitmap_upload));
    memset(&s->gl_map_upload, 0, sizeof(s->gl_map_upload));
    sgi_kona_texture_regs_load(s, &(SGIKonaTextureRegs) { 0 });
    s->texture_regs_context = SGI_KONA_CONTEXT_NONE;
    s->texture_state_used = false;
    s->texture_bind_context = SGI_KONA_CONTEXT_NONE;
    s->texture_bind_generation = 0;
    s->texture_associate_pending = false;
    s->texture_associate_context = SGI_KONA_CONTEXT_NONE;
    s->texture_associate_generation = 0;
    s->texture_associate_slot = 0;
    s->texture_retire_pending = false;
    s->image_dma_discard = false;
    sgi_kona_renderer_stop(s);
    s->renderer_failed = false;
    s->mesa_failed = false;
    s->test_renderer_fail_draw = 0;
    s->test_renderer_fail_create = 0;
    s->test_renderer_skew = false;
    s->test_renderer_fail_hold = 0;

    s->widget_control = 0;
    s->onewire_reg = 0;

    /* Increment 2: register-window plain storage and the mailbox queue. */
    s->low_window_write = 0;
    s->pi_parked = 0;
    s->int_dest_upper = 0;
    s->int_dest_lower = 0;
    s->int_vectors = 0;
    s->reset_arg = 0;
    s->int_enable = 0;
    s->video_delivered = false;
    s->int_cause = 0;
    memset(s->mailbox_queue, 0, sizeof(s->mailbox_queue));
    s->mailbox_len = 0;
    s->mailbox_pos = 0;
    s->mailbox_completions = 0;
    s->reset_strobe = 0;
    s->eeprom_enable = 0;
    s->eeprom_addr = 0;

    /* The firmware load: see the header comment on SGI_KONA_ARM_LOW_BASE. */
    memset(s->arm_low, 0, sizeof(s->arm_low));
    memset(s->arm_high, 0, s->arm_high_count * sizeof(uint32_t));
    s->icu_control = 0;
    s->load_section_open = false;
    s->load_sections = 0;
    memset(s->load_section_addr, 0, sizeof(s->load_section_addr));
    memset(s->load_section_words, 0, sizeof(s->load_section_words));
    s->loader_3b0 = 0;
    s->loader_328 = 0;
    s->activity = false;

    /* The XG map RAM: see the header comment on SGI_KONA_MAPRAM_BASE. */
    memset(s->map_ram, 0, s->map_ram_count * sizeof(uint32_t));
    s->map_ram_accesses = 0;

    /*
     * The textport command port's own reset entry point, rather than
     * poking its fields directly: it re-runs sgi_command_port_init()
     * with the same ops/opaque/port_id/selectors this device passed at
     * finish_realize(), so identity is preserved and only the framing
     * state (kick, framing, selector, argc_expected/have, args[])
     * clears. Post-realize, the textport immediately expects a fresh
     * sync word, so that flag is set again rather than cleared.
     */
    sgi_command_port_reset(&s->textport);
    s->textport_expect_sync = true;
    sgi_command_port_reset(&s->kernel_port);
    sgi_command_port_reset(&s->gl_port);
    s->gl_grammar = false;
    s->gl_sync_held = false;
    s->gl_level1_held = 0;
    s->kernel_input = false;
    s->suspended_owner = SGI_KONA_CONTEXT_NONE;
    s->kernel_port_privileged = false;
    memset(&s->kernel_port_suspended, 0, sizeof(s->kernel_port_suspended));
    s->reason_page_valid = false;
    s->reason_page = 0;
    s->reason_queue_len = 0;
    s->reason_unread = false;
    s->reason_posted = 0;
    if (s->reason_timer) {
        timer_del(s->reason_timer);
    }
    s->swapin_stage = SGI_KONA_SWAPIN_IDLE;
    s->swapin_data_left = 0;
    s->return_page_valid = false;
    s->return_page = 0;
    s->credit_config = 0;
    s->credit_pending = 0;
    s->pi_held = 0;
    s->retrace_tag = 0;
    s->retrace_completed = false;
    sgi_kona_contexts_clear(s);
    s->context_clock = 0;
    s->context_failed = false;
    memset(s->context_error, 0, sizeof(s->context_error));
    s->video_open = false;
    s->video_command = 0;
    s->video_have = 0;
    s->video_first = 0;
    s->video_tail_sum = 0;
    s->video_blanked = false;
    /* The flash keeps its contents: only an operation in progress ends. */
    memset(&s->flash_op, 0, sizeof(s->flash_op));
    s->color_map_start = 0;
    s->color_map_count = 0;
    s->color_map_remaining = 0;
    s->color_map_outside = 0;
    memset(s->x_color_map, 0, sizeof(s->x_color_map));
    memset(s->gamma, 0, sizeof(s->gamma));
    memset(s->gamma_loaded, 0, sizeof(s->gamma_loaded));
    s->gamma_start = 0;
    s->gamma_count = 0;
    s->gamma_remaining = 0;
    s->gamma_outside = 0;
    s->cursor_remaining = 0;
    s->cursor_stage_words = 0;
    s->cursor_stage_entry = SGI_KONA_CURSOR_ENTRIES;
    memset(s->cursor_stage, 0, sizeof(s->cursor_stage));
    memset(s->cursor_images, 0, sizeof(s->cursor_images));
    memset(s->cursor_valid, 0, sizeof(s->cursor_valid));
    s->cursor_selector = 0;
    s->cursor_channel = 0;
    s->cursor_offset = 0;
    s->cursor_position = 0;
    s->cursor_mode = 0;
    sgi_kona_gl_tex_table_clear(s);
    s->gl_token_high = 0;
    s->gl_data_token = 0;
    s->gl_data_remaining = 0;
    s->gl_data_have = 0;
    memset(s->gl_data, 0, sizeof(s->gl_data));
    memset(s->x_gc, 0, sizeof(s->x_gc));
    memset(s->x_boxes, 0, sizeof(s->x_boxes));
    s->x_boxes_have = 0;
    memset(s->x_position, 0, sizeof(s->x_position));
    s->x_clip_count = 0;
    memset(s->x_clip_rects, 0, sizeof(s->x_clip_rects));
    memset(s->x_clip_run, 0, sizeof(s->x_clip_run));
    s->x_clip_run_have = 0;
    s->x_run_owner = SGI_KONA_CONTEXT_NONE;
    s->x_run_owner_generation = 0;
    s->x_scissor_valid = false;
    memset(s->x_scissor, 0, sizeof(s->x_scissor));
    memset(s->x_stipple_width, 0, sizeof(s->x_stipple_width));
    memset(s->x_stipple_height, 0, sizeof(s->x_stipple_height));
    memset(s->x_stipple_origin_x, 0, sizeof(s->x_stipple_origin_x));
    memset(s->x_stipple_origin_y, 0, sizeof(s->x_stipple_origin_y));
    memset(s->x_stipple_rows, 0, sizeof(s->x_stipple_rows));
    s->x_stipple_loaded = SGI_KONA_X_STIPPLE_NONE;
    s->x_stipple_store_entry = 0;
    s->x_stipple_store_width = 0;
    s->x_stipple_store_height = 0;
    s->x_stipple_store_remaining = 0;
    sgi_kona_x_read_invalidate(s);
    sgi_kona_gl_read_dma_clear(s);
    memset(s->x_write_mem.bytes, 0, s->x_write_mem.bytes_len);
    sgi_kona_x_write_clear(s);
    memset(s->x_pixel_setup, 0, sizeof(s->x_pixel_setup));
    s->x_pixel_open = false;
    s->x_pixel_x = 0;
    s->x_pixel_y = 0;
    s->x_pixel_bpp = 0;
    s->x_pixel_stride = 0;
    s->x_pixel_words = 0;
    s->x_pixel_remaining = 0;
    s->x_pixel_owner = SGI_KONA_CONTEXT_NONE;
    s->x_pixel_owner_generation = 0;
    s->x_pixel_prev = 0;

    /*
     * The DMA engine's own FIFO command port, the same entry point as
     * the textport's own reset above: re-runs sgi_command_port_init()
     * and clears the kick-primed word accumulator.
     */
    sgi_kona_dma_reset(&s->dma);

    s->draw_color = 0;
    memset(s->colormap, 0, sizeof(s->colormap));
    sgi_kona_bitmap_reset(s);
    s->raster_x = 0;
    s->raster_y = 0;

    /*
     * Board memory returns to its post-realize all-zero contents, cleared
     * here directly because the shared layer has no reset call. The
     * scanout reset repaints every line, since a mid-run reset can follow
     * draws the console has already shown, and restarts the retrace
     * phase.
     */
    s->fb_width = SGI_KONA_FB_WIDTH;
    s->fb_height = SGI_KONA_FB_HEIGHT;
    s->cfg_width = SGI_KONA_FB_WIDTH;
    s->cfg_height = SGI_KONA_FB_HEIGHT;
    sgi_kona_managed_layout(s);
    memset(s->mem.bytes, 0, s->mem.bytes_len);
    memset(s->desktop_mem.bytes, 0, s->desktop_mem.bytes_len);
    /* Made again, cleared, on the next accumulation operation. */
    sgi_board_memory_cleanup(&s->accum_mem);
    s->desktop_state_used = false;
    s->display_id_generation++;
    memset(s->desktop_map, 0, sizeof(s->desktop_map));
    memset(s->desktop_map_valid, 0, sizeof(s->desktop_map_valid));
    memset(s->desktop_map_reported, 0, sizeof(s->desktop_map_reported));
    s->desktop_id_reported = false;
    s->packed_index_short_reported = false;
    s->fb_word_low_bits_reported = false;
    sgi_kona_channels_reset(s);
    sgi_kona_channels_apply(s);
    for (unsigned c = 0; c < sgi_kona_channel_count(s); c++) {
        sgi_scanout_reset(sgi_kona_channel_scanout(s, c));
    }
}

static const Property sgi_kona_properties[] = {
    DEFINE_PROP_BOOL("host-gpu", SGIKonaState, host_gpu, false),
    DEFINE_PROP_LINK("fabric", SGIKonaState, fabric,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    /* The node whose processor interfaces count the board's credits. */
    DEFINE_PROP_LINK("pi", SGIKonaState, pi, TYPE_SGI_PI_GFX, Object *),
    /* See the struct field comment on fabric_hub_widget. */
    DEFINE_PROP_UINT32("fabric-hub-widget", SGIKonaState,
                       fabric_hub_widget, 0),
    DEFINE_PROP_UINT32("manufacturer", SGIKonaState, manufacturer,
                       0x02aa),
    DEFINE_PROP_UINT32("part", SGIKonaState, part, 0xc102),
    DEFINE_PROP_UINT32("revision", SGIKonaState, revision, 0),
    /*
     * Selects the PROM's part-based classification path (bits 2:0 == 0);
     * see the header comment on SGI_KONA_PROBE.
     */
    DEFINE_PROP_UINT32("probe-value", SGIKonaState, probe_value, 0),
    /*
     * Bytes of board memory for resident display lists: the model's own
     * choice, since no capture holds the pipe's size for it. See the
     * header comment on SGI_KONA_TAG_DIRECT. Migration refuses a stream
     * from a board of another size.
     */
    DEFINE_PROP_UINT32("list-memory-size", SGIKonaState, list_memory_size,
                       SGI_KONA_LIST_DEFAULT),
    /*
     * The configuration the pipe presents; see sgi_kona_configure().
     * Without "product" the pipe answers as it always has, and setting
     * any of the others is refused. Zero and empty values take the
     * product's own board, one raster manager, and the channel count of
     * the display generator.
     */
    DEFINE_PROP_BOOL("ktown2", SGIKonaState, ktown2, false),
    DEFINE_PROP_UINT8("identity-index", SGIKonaState, identity_index, 0),
    DEFINE_PROP_UINT8("shared-identity-index", SGIKonaState,
                      shared_identity_index, SGI_KONA_OWN_IDENTITY),
    DEFINE_PROP_UINT64("geometry-serial", SGIKonaState, onewire_serial, 0),
    DEFINE_PROP_STRING("product", SGIKonaState, product),
    DEFINE_PROP_STRING("geometry", SGIKonaState, geometry),
    DEFINE_PROP_STRING("rm-type", SGIKonaState, rm_type),
    DEFINE_PROP_UINT32("rm-count", SGIKonaState, rm_count, 0),
    /* The flash EEPROM's raw image; see SGI_KONA_FLASH_SIZE. */
    DEFINE_PROP_DRIVE("drive", SGIKonaState, flash_blk),
    DEFINE_PROP_UINT32("texture-mb", SGIKonaState, texture_mb, 0),
    DEFINE_PROP_UINT32("channels", SGIKonaState, channels, 0),
    DEFINE_PROP_STRING("display", SGIKonaState, display),
    DEFINE_PROP_STRING("pixel-depth", SGIKonaState, pixel_depth),
};

/* The machine stopped first (sgi_kona_work_vm_state()); this makes sure. */
static int sgi_kona_pre_save(void *opaque)
{
    SGIKonaState *s = opaque;

    sgi_head_work_enter_work();
    sgi_kona_work_drain(s, UINT64_MAX);
    sgi_head_work_leave();
    sgi_kona_renderer_stop(s);
    return 0;
}

static bool sgi_kona_mapram_count_valid(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    return s->map_ram_count == SGI_KONA_MAPRAM_ENTRIES;
}

static bool sgi_kona_arm_high_count_valid(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    return s->arm_high_count == SGI_KONA_ARM_HIGH_WORDS;
}

/*
 * The loaded pixels replaced board memory by a path other than the
 * drawing commands, so every line is repainted.
 */
static int sgi_kona_cursor_pre_load(void *opaque)
{
    SGIKonaState *s = opaque;

    sgi_kona_renderer_stop(s);
    sgi_kona_contexts_clear(s);
    sgi_kona_gl_read_dma_clear(s);

    sgi_kona_texture_reset(&s->textures);
    sgi_kona_texture_upload_clear(s);
    sgi_kona_gl_image_clear(s);
    memset(&s->gl_bitmap_upload, 0, sizeof(s->gl_bitmap_upload));
    memset(&s->gl_map_upload, 0, sizeof(s->gl_map_upload));
    memset(s->texture_bind, 0, sizeof(s->texture_bind));
    memset(s->texture_record, 0, sizeof(s->texture_record));
    s->texture_record_token = 0;
    memset(s->texture_transform, 0, sizeof(s->texture_transform));
    s->texture_bind_valid = false;
    s->texture_record_valid = false;
    s->texture_state_used = false;
    s->texture_bind_context = SGI_KONA_CONTEXT_NONE;
    s->texture_bind_generation = 0;
    s->texture_associate_pending = false;
    s->texture_associate_context = SGI_KONA_CONTEXT_NONE;
    s->texture_associate_generation = 0;
    s->texture_associate_slot = 0;
    s->texture_retire_pending = false;
    s->image_dma_discard = false;
    memset(s->x_write_mem.bytes, 0, s->x_write_mem.bytes_len);
    sgi_kona_x_write_clear(s);
    memset(s->desktop_mem.bytes, 0, s->desktop_mem.bytes_len);
    s->desktop_state_used = false;
    s->display_id_generation++;
    memset(s->desktop_map, 0, sizeof(s->desktop_map));
    memset(s->desktop_map_valid, 0, sizeof(s->desktop_map_valid));
    memset(s->desktop_map_reported, 0, sizeof(s->desktop_map_reported));
    s->desktop_id_reported = false;
    s->packed_index_short_reported = false;
    s->fb_word_low_bits_reported = false;
    s->cursor_state_loaded = false;
    s->cursor_stage_words = 0;
    s->cursor_stage_entry = SGI_KONA_CURSOR_ENTRIES;
    memset(s->cursor_stage, 0, sizeof(s->cursor_stage));
    memset(s->cursor_images, 0, sizeof(s->cursor_images));
    memset(s->cursor_valid, 0, sizeof(s->cursor_valid));
    s->cursor_selector = 0;
    s->cursor_channel = 0;
    s->cursor_offset = 0;
    s->cursor_position = 0;
    s->cursor_mode = 0;
    /* The video-blank subsection is sent only while the source is blank. */
    s->video_blanked = false;
    /* The same for a held 1 1. */
    s->gl_level1_held = 0;
    s->gl_level1_arg = 0;
    /*
     * The channels subsection only when a channel left its reset state,
     * and the video-data one only while a command takes its words.
     */
    sgi_kona_channels_reset(s);
    memset(s->video_data, 0, sizeof(s->video_data));
    /* And the managed-area one only once a combination resized it. */
    s->fb_width = SGI_KONA_FB_WIDTH;
    s->fb_height = SGI_KONA_FB_HEIGHT;
    s->cfg_width = SGI_KONA_FB_WIDTH;
    s->cfg_height = SGI_KONA_FB_HEIGHT;
    sgi_kona_managed_layout(s);
    return 0;
}

/* Check source coordinates and progress against the retained operation. */
static bool sgi_kona_x_write_state_valid(SGIKonaState *s)
{
    const uint32_t *setup = s->x_write_setup;
    const SGIPlane *source = sgi_board_memory_plane(&s->x_write_mem, 0);
    const SGIPlane *valid = sgi_board_memory_plane(&s->x_write_mem, 1);
    const SGIPlane *ones = sgi_board_memory_plane(&s->x_write_mem, 2);
    uint64_t width = setup[SGI_KONA_X_SETUP_WIDTH];
    uint64_t height = setup[SGI_KONA_X_SETUP_HEIGHT];
    uint64_t pad = setup[SGI_KONA_X_SETUP_PAD];
    uint32_t bpp = sgi_kona_x_pixel_bpp(setup[SGI_KONA_X_SETUP_SRC_PACK]);
    uint64_t stride = bpp ? DIV_ROUND_UP(pad + bpp * width, 32) : 0;
    uint64_t words = 0;
    uint64_t consumed;
    SGIXGC gc;
    bool decoded = sgi_kona_x_write_decode(s, &gc);

    if (stride && height > UINT32_MAX / stride) {
        stride = 0;
    } else {
        words = stride * height;
    }
    if (!s->x_pixel_open || s->x_pixel_bpp != bpp ||
        s->x_pixel_stride != stride || s->x_pixel_words != words ||
        s->x_pixel_remaining > words ||
        s->x_write_x1 < 0 || s->x_write_x1 > s->x_write_x2 ||
        s->x_write_x2 > s->fb_width ||
        s->x_write_y1 < 0 || s->x_write_y1 > s->x_write_y2 ||
        s->x_write_y2 > s->fb_height ||
        s->x_write_clip_count > SGI_KONA_X_CLIP_MAX_RECTS ||
        s->x_write_stipple_width > SGI_KONA_X_STIPPLE_MAX ||
        s->x_write_stipple_height > SGI_KONA_X_STIPPLE_MAX ||
        (s->x_write_stipple_width && !s->x_write_stipple_height) ||
        s->x_write_stipple_x < -INT64_C(4294967296) ||
        s->x_write_stipple_x > INT64_C(4294967294) ||
        s->x_write_stipple_y < -INT64_C(4294967296) ||
        s->x_write_stipple_y > INT64_C(4294967294) ||
        ((!decoded || !bpp) && !s->x_write_refused)) {
        return false;
    }
    consumed = words - s->x_pixel_remaining;
    if (!consumed && s->x_pixel_prev) {
        return false;
    }
    for (unsigned y = 0; y < s->fb_height; y++) {
        for (unsigned x = 0; x < s->fb_width; x++) {
            uint32_t present = sgi_plane_load(&s->x_write_mem, valid, x, y);
            uint32_t value = sgi_plane_load(&s->x_write_mem, source, x, y);
            uint32_t one = sgi_plane_load(&s->x_write_mem, ones, x, y);
            int64_t col = (int64_t)x - s->x_pixel_x;
            int64_t row = (int64_t)y - s->x_pixel_y;
            bool candidate = decoded && col >= 0 && row >= 0 &&
                             col < width && row < height &&
                             sgi_kona_x_write_contains(s, x, y);
            bool in_bounds = x >= s->x_write_x1 && x < s->x_write_x2 &&
                             y >= s->x_write_y1 && y < s->x_write_y2;
            uint64_t end = candidate ? (uint64_t)row * stride +
                DIV_ROUND_UP(pad + bpp * (col + 1), 32) : UINT64_MAX;
            bool arrived = candidate && end <= consumed;
            if (present > 1 || value > (decoded ? gc.width_mask : 0xff) ||
                one > (decoded ? gc.width_mask : 0xff) ||
                (!present && (value || one)) ||
                (present && (!in_bounds || sgi_x_gc_noop(&gc) ||
                             (!s->x_write_multi && !arrived))) ||
                (!s->x_write_refused && !s->x_write_multi && arrived &&
                 !sgi_x_gc_noop(&gc) &&
                 setup[SGI_KONA_X_SETUP_SRC_PACK] != SGI_KONA_X_PACK_BITMAP &&
                 !present)) {
                return false;
            }
        }
    }
    return true;
}

static int sgi_kona_post_load_validate(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;
    unsigned pending = 0, queued = 0;

    sgi_kona_renderer_stop(s);
    s->renderer_failed = false;
    s->mesa_failed = false;
    s->test_renderer_fail_draw = 0;
    s->test_renderer_fail_create = 0;
    s->test_renderer_skew = false;
    s->test_renderer_fail_hold = 0;

    /*
     * An open video command can hold no more data words than it takes, or
     * than the checksum search accepts.
     */
    if (s->video_open) {
        uint32_t length = sgi_kona_video_data_words(s->video_command);

        if (length == SGI_KONA_VIDEO_DATA_UNKNOWN ?
            s->video_have >= SGI_KONA_VIDEO_SEARCH_MAX_WORDS :
            s->video_have > length) {
            return -EINVAL;
        }
    }
    /* A bitmap's next word must land in its header or its payload. */
    if (!sgi_kona_bitmap_fields_valid(s->bitmap_state, s->bitmap_header,
                                      s->bitmap_header_have, s->bitmap_width,
                                      s->bitmap_height, s->bitmap_payload,
                                      s->bitmap_payload_have,
                                      s->bitmap_payload_expected)) {
        return -EINVAL;
    }
    /* A gamma load can have no more entries to come than it counted. */
    if (s->gamma_remaining > s->gamma_count ||
        s->gamma_outside > s->gamma_count - s->gamma_remaining) {
        return -EINVAL;
    }
    if (s->color_map_remaining > s->color_map_count ||
        s->color_map_outside > s->color_map_count - s->color_map_remaining) {
        return -EINVAL;
    }
    if (!s->cursor_state_loaded) {
        s->cursor_remaining = 0;
    }
    if (s->cursor_stage_words != 0 && s->cursor_stage_words != 4 &&
        s->cursor_stage_words != SGI_KONA_X_CURSOR_DATA_WORDS) {
        return -EINVAL;
    }
    if (s->cursor_stage_entry > SGI_KONA_CURSOR_ENTRIES ||
        s->cursor_remaining > s->cursor_stage_words ||
        (!s->cursor_remaining != !s->cursor_stage_words)) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < SGI_KONA_CURSOR_ENTRIES; i++) {
        if (s->cursor_valid[i] > 1) {
            return -EINVAL;
        }
    }
    if (s->cursor_remaining > SGI_KONA_X_CURSOR_DATA_WORDS ||
        s->gl_tex_table_remaining > SGI_KONA_GL_TEX_TABLE_WORDS) {
        return -EINVAL;
    }
    if (s->x_boxes_have > SGI_KONA_X_BOX_RUN_WORDS ||
        s->x_clip_count > SGI_KONA_X_CLIP_MAX_RECTS ||
        s->x_clip_run_have + SGI_KONA_X_CLIP_WORDS >
        SGI_KONA_X_CLIP_LIST_WORDS) {
        return -EINVAL;
    }
    /* New snapshots own geometry even when live setup changed meanwhile. */
    if (s->x_write_snapshot) {
        if (!sgi_kona_x_write_state_valid(s)) {
            return -EINVAL;
        }
    } else if (s->x_pixel_open || s->x_pixel_remaining) {
        /* Opening a write always takes its snapshot. */
        return -EINVAL;
    }
    /* The stipple cache's patterns and a store's rows stay in bounds. */
    for (unsigned e = 0; e < SGI_KONA_X_STIPPLE_ENTRIES; e++) {
        if (s->x_stipple_width[e] > SGI_KONA_X_STIPPLE_MAX ||
            s->x_stipple_height[e] > SGI_KONA_X_STIPPLE_MAX ||
            !s->x_stipple_width[e] != !s->x_stipple_height[e]) {
            return -EINVAL;
        }
    }
    if ((s->x_stipple_loaded >= SGI_KONA_X_STIPPLE_ENTRIES &&
         s->x_stipple_loaded != SGI_KONA_X_STIPPLE_NONE) ||
        s->x_stipple_store_remaining > s->x_stipple_store_height) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < SGI_SWAP_MAX; i++) {
        const SGIKonaPendingSwap *owner = &s->pending_swaps[i];
        bool matched = false;

        if (!owner->valid) {
            continue;
        }
        pending++;
        if (owner->context < SGI_KONA_CONTEXT_NONE ||
            owner->context >= SGI_KONA_CONTEXTS ||
            (owner->context == SGI_KONA_CONTEXT_NONE ? owner->generation != 0 :
             (!s->state[owner->context] || !owner->generation ||
              owner->generation > s->contexts[owner->context].generation))) {
            return -EINVAL;
        }
        for (unsigned j = 0; j < i; j++) {
            if (s->pending_swaps[j].valid &&
                s->pending_swaps[j].seq == owner->seq) {
                return -EINVAL;
            }
        }
        for (unsigned j = 0; j < s->scanout.swap_count; j++) {
            const SGISwapRecord *swap = &s->scanout.swaps[
                (s->scanout.swap_head + j) % SGI_SWAP_MAX];

            if ((swap->cookie & SGI_KONA_SWAP_COOKIE_FLAG) &&
                swap->seq == owner->seq &&
                SGI_KONA_SWAP_COOKIE_SLOT(swap->cookie) == owner->context) {
                matched = true;
            }
        }
        if (!matched) {
            return -EINVAL;
        }
    }
    for (unsigned i = 0; i < s->scanout.swap_count; i++) {
        if (s->scanout.swaps[(s->scanout.swap_head + i) % SGI_SWAP_MAX].cookie &
            SGI_KONA_SWAP_COOKIE_FLAG) {
            queued++;
        }
    }
    if (queued != pending ||
        s->context_error[sizeof(s->context_error) - 1]) {
        return -EINVAL;
    }
    /* The current context is a live entry, and only it waits for a page. */
    if (s->context_current < SGI_KONA_CONTEXT_NONE ||
        s->context_current >= SGI_KONA_CONTEXTS ||
        (s->context_current != SGI_KONA_CONTEXT_NONE &&
         (!s->contexts[s->context_current].valid ||
          !s->state[s->context_current])) ||
        (s->context_page_pending &&
         (s->context_current == SGI_KONA_CONTEXT_NONE ||
          s->context_current == SGI_KONA_CONTEXT_TRANSIENT ||
          s->contexts[s->context_current].return_page_valid ||
          s->return_page_valid))) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        const SGIKonaGLPipe *p;

        if (!s->state[i]) {
            continue;
        }
        p = &s->state[i]->pipe;
        if (s->contexts[i].save_offset > 0xfff) {
            return -EINVAL;
        }
        for (unsigned slot = 0; slot < 2; slot++) {
            if (p->texture_assoc_valid & (1u << slot)) {
                SGIKonaTextureObject *object = sgi_kona_texture_lookup(
                    &s->textures, sgi_kona_texture_owner(i),
                    p->texture_assoc_handle[slot]);

                if (!s->contexts[i].valid || !object || !object->native_count) {
                    return -EINVAL;
                }
            }
        }
    }
    /*
     * A GL packet part-way through is one of the GL tokens with its own
     * count, a GL data run belongs to a command that has one, and a 1 is
     * held only in the GL grammar and between packets.
     */
    if (s->gl_port.framing &&
        (!sgi_kona_gl_token(s, s->gl_port.selector) ||
         s->gl_port.argc_expected !=
         sgi_kona_gl_token(s, s->gl_port.selector)->argc ||
         s->gl_port.argc_have >= s->gl_port.argc_expected)) {
        return -EINVAL;
    }
    if (s->gl_data_remaining &&
        (s->gl_port.framing || s->gl_tex_table_remaining ||
         (s->gl_data_token != SGI_KONA_CMD_GL_POLYGON_STIPPLE &&
          s->gl_data_token != SGI_KONA_CMD_GL_POP_LIGHTS &&
          s->gl_data_token != SGI_KONA_CMD_GL_TEX_LOAD &&
          s->gl_data_token != SGI_KONA_CMD_GL_DRAW_PIXELS &&
          s->gl_data_token != SGI_KONA_CMD_GL_BITMAP_INLINE &&
          s->gl_data_token != SGI_KONA_CMD_GL_BITMAP &&
          s->gl_data_token != SGI_KONA_CMD_GL_PIXEL_MAP_F &&
          s->gl_data_token != SGI_KONA_CMD_GL_PIXEL_MAP_UI &&
          s->gl_data_token != SGI_KONA_CMD_GL_PIXEL_MAP_US &&
          s->gl_data_token != SGI_KONA_CMD_GL_CONVOLUTION_1D &&
          s->gl_data_token != SGI_KONA_CMD_GL_CONVOLUTION_2D &&
          s->gl_data_token != SGI_KONA_CMD_GL_SEPARABLE &&
          s->gl_data_token != SGI_KONA_CMD_GL_ARRAY_BULK))) {
        return -EINVAL;
    }
    if (s->gl_sync_held &&
        (!s->gl_grammar || s->gl_port.framing || s->textport_expect_sync)) {
        return -EINVAL;
    }
    if (s->gl_level1_held > 2 ||
        (s->gl_level1_held &&
         (s->gl_sync_held || !s->gl_grammar || s->gl_port.framing ||
          s->textport_expect_sync))) {
        return -EINVAL;
    }
    sgi_kona_invalidate_all(s);
    sgi_kona_cursor_update(s);
    return 0;
}

/*
 * Every channel's scanout presents the mode its channel state gives: the
 * channel's rows are resolved at its format's width into a frame
 * allocated for the scanout's mode, so a stream whose two disagree is
 * refused rather than let a row run past its frame.
 */
static int sgi_kona_channels_match_modes(SGIKonaState *s)
{
    for (unsigned c = 0; c < sgi_kona_channel_count(s); c++) {
        const SGIScanout *so = sgi_kona_channel_scanout(s, c);
        SGIKonaChannelView v;
        uint32_t w = 0, h = 0;

        if (sgi_kona_channel_view(s, c, &v)) {
            w = v.out_width;
            h = v.out_height;
        }
        if (so->mode.width != w || so->mode.height != h ||
            so->frame_width != w || so->frame_height != h) {
            return -EINVAL;
        }
    }
    return 0;
}

static int sgi_kona_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;
    int result = sgi_kona_post_load_validate(opaque, version_id);

    if (!result) {
        result = sgi_kona_channels_match_modes(s);
    }

    sgi_kona_gamma_update(s);
    if (result) {
        sgi_kona_renderer_stop(s);
        sgi_kona_contexts_clear(s);
    }
    return result;
}

/*
 * Board memory, and the scanout's mode, retrace phase, swap queue and
 * cursor, as the subsections the graphics low-level design's migration
 * rule asks for, so the family's own version stays 6.
 */
DEFINE_VMSTATE_SGI_BOARD_MEMORY(vmstate_sgi_kona_board_memory,
                                TYPE_SGI_KONA "/board-memory",
                                SGIKonaState, mem);
static bool sgi_kona_desktop_needed(void *opaque)
{
    SGIKonaState *s = SGI_KONA(opaque);

    return s->desktop_state_used || s->x_write_snapshot;
}

static int sgi_kona_desktop_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    for (unsigned plane = 0; plane < s->desktop_mem.nplanes; plane++) {
        const SGIPlane *p = sgi_board_memory_plane(&s->desktop_mem, plane);
        uint32_t mask = plane == 3 ? 3 : 0xffff;

        for (unsigned y = 0; y < p->height; y++) {
            for (unsigned x = 0; x < p->width; x++) {
                if (sgi_plane_load(&s->desktop_mem, p, x, y) & ~mask) {
                    return -EINVAL;
                }
            }
        }
    }
    s->desktop_state_used = true;
    s->display_id_generation++;
    sgi_kona_invalidate_all(s);
    return 0;
}

static bool sgi_kona_x_write_needed(void *opaque)
{
    return SGI_KONA(opaque)->x_write_snapshot;
}

static int sgi_kona_x_write_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    s->x_write_snapshot = true;
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_x_write = {
    .name = TYPE_SGI_KONA "/desktop/write",
    .version_id = 2,
    .minimum_version_id = 2,
    .needed = sgi_kona_x_write_needed,
    .post_load = sgi_kona_x_write_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(x_write_mem, SGIKonaState, 0,
                       vmstate_sgi_board_memory, SGIBoardMemory),
        VMSTATE_BOOL(x_write_refused, SGIKonaState),
        VMSTATE_BOOL(x_write_multi, SGIKonaState),
        VMSTATE_INT32(x_write_x1, SGIKonaState),
        VMSTATE_INT32(x_write_y1, SGIKonaState),
        VMSTATE_INT32(x_write_x2, SGIKonaState),
        VMSTATE_INT32(x_write_y2, SGIKonaState),
        VMSTATE_UINT32_ARRAY(x_write_gc, SGIKonaState, SGI_KONA_X_GC_WORDS),
        VMSTATE_UINT32_ARRAY(x_write_setup, SGIKonaState,
                             SGI_KONA_X_PIXEL_SETUP_WORDS),
        VMSTATE_UINT32(x_write_clip_count, SGIKonaState),
        VMSTATE_UINT32_ARRAY(x_write_clip_rects, SGIKonaState,
                             4 * SGI_KONA_X_CLIP_MAX_RECTS),
        VMSTATE_BOOL(x_write_scissor_valid, SGIKonaState),
        VMSTATE_UINT32_ARRAY(x_write_scissor, SGIKonaState,
                             SGI_KONA_X_SCISSOR_WORDS),
        VMSTATE_UINT32(x_write_stipple_width, SGIKonaState),
        VMSTATE_UINT32(x_write_stipple_height, SGIKonaState),
        VMSTATE_INT64(x_write_stipple_x, SGIKonaState),
        VMSTATE_INT64(x_write_stipple_y, SGIKonaState),
        VMSTATE_UINT32_ARRAY(x_write_stipple_rows, SGIKonaState,
                             SGI_KONA_X_STIPPLE_MAX),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_desktop = {
    .name = TYPE_SGI_KONA "/desktop",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_desktop_needed,
    .post_load = sgi_kona_desktop_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(desktop_mem, SGIKonaState, 0,
                         vmstate_sgi_board_memory, SGIBoardMemory),
        VMSTATE_UINT32_ARRAY(desktop_map, SGIKonaState, 256),
        VMSTATE_BOOL_ARRAY(desktop_map_valid, SGIKonaState, 256),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_kona_x_write,
        NULL
    },
};

DEFINE_VMSTATE_SGI_SCANOUT(vmstate_sgi_kona_scanout, TYPE_SGI_KONA "/scanout",
                           SGIKonaState, scanout);

/*
 * The display channels, and the consoles of channels past 0, whose
 * scanouts carry their own modes and cursors. Needed on a pipe with more
 * than one channel, or once channel 0 left the state reset gives it.
 */
static bool sgi_kona_channels_needed(void *opaque)
{
    SGIKonaState *s = opaque;
    SGIKonaChannel reset[SGI_KONA_MAX_CHANNELS];

    sgi_kona_channels_default(reset);
    return s->channel_scanouts ||
           memcmp(reset, s->channel, sizeof(reset)) != 0;
}

static int sgi_kona_channels_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    for (unsigned c = 0; c < SGI_KONA_MAX_CHANNELS; c++) {
        const SGIKonaChannel *ch = &s->channel[c];

        if (ch->format_width > SGI_SCANOUT_MAX_DIM ||
            ch->format_height > SGI_SCANOUT_MAX_DIM ||
            ch->input_width > SGI_SCANOUT_MAX_DIM ||
            ch->input_height > SGI_SCANOUT_MAX_DIM ||
            ch->input_x > SGI_SCANOUT_MAX_DIM ||
            ch->input_y > SGI_SCANOUT_MAX_DIM) {
            return -EINVAL;
        }
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_channel = {
    .name = TYPE_SGI_KONA "/channel-entry",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(enabled, SGIKonaChannel),
        VMSTATE_BOOL(loaded, SGIKonaChannel),
        VMSTATE_UINT32(format_width, SGIKonaChannel),
        VMSTATE_UINT32(format_height, SGIKonaChannel),
        VMSTATE_UINT32(input_x, SGIKonaChannel),
        VMSTATE_UINT32(input_y, SGIKonaChannel),
        VMSTATE_UINT32(input_width, SGIKonaChannel),
        VMSTATE_UINT32(input_height, SGIKonaChannel),
        VMSTATE_BOOL(blanked, SGIKonaChannel),
        VMSTATE_UINT32(gamma_map, SGIKonaChannel),
        VMSTATE_UINT32(cursor_priority, SGIKonaChannel),
        VMSTATE_UINT32(source, SGIKonaChannel),
        VMSTATE_UINT32_ARRAY(format_name, SGIKonaChannel,
                             SGI_KONA_FORMAT_NAME_WORDS),
        VMSTATE_UINT32_ARRAY(pan, SGIKonaChannel, 4),
        VMSTATE_UINT32_ARRAY(placement, SGIKonaChannel, 8),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_channels = {
    .name = TYPE_SGI_KONA "/channels",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_channels_needed,
    .post_load = sgi_kona_channels_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(channel, SGIKonaState, SGI_KONA_MAX_CHANNELS, 1,
                             vmstate_sgi_kona_channel, SGIKonaChannel),
        VMSTATE_UINT32_EQUAL(channel_scanouts, SGIKonaState),
        VMSTATE_STRUCT_VARRAY_UINT32(channel_scanout, SGIKonaState,
                                     channel_scanouts, 1, vmstate_sgi_scanout,
                                     SGIScanout),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * The managed area and the size configuration block +0x84 and +0x88
 * latched at the last start, once either left the reset size.
 */
static bool sgi_kona_managed_area_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->fb_width != SGI_KONA_FB_WIDTH ||
           s->fb_height != SGI_KONA_FB_HEIGHT ||
           s->cfg_width != SGI_KONA_FB_WIDTH ||
           s->cfg_height != SGI_KONA_FB_HEIGHT;
}

static int sgi_kona_managed_area_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    if (!sgi_kona_managed_fits(s, s->fb_width, s->fb_height) ||
        !sgi_kona_managed_fits(s, s->cfg_width, s->cfg_height)) {
        return -EINVAL;
    }
    sgi_kona_managed_layout(s);
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_managed_area = {
    .name = TYPE_SGI_KONA "/managed-area",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_managed_area_needed,
    .post_load = sgi_kona_managed_area_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(fb_width, SGIKonaState),
        VMSTATE_INT32(fb_height, SGIKonaState),
        VMSTATE_INT32(cfg_width, SGIKonaState),
        VMSTATE_INT32(cfg_height, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

/* The data words of a video command still open, for its action. */
static bool sgi_kona_video_data_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->video_open && s->video_have &&
           sgi_kona_video_data_words(s->video_command) !=
           SGI_KONA_VIDEO_DATA_UNKNOWN;
}

/* The accumulation buffer, once a context has made it. */
static bool sgi_kona_accum_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->accum_mem.bytes != NULL;
}

static int sgi_kona_accum_pre_load(void *opaque)
{
    SGIKonaState *s = opaque;

    return sgi_kona_accum_ready(s) ? 0 : -ENOMEM;
}

static const VMStateDescription vmstate_sgi_kona_accum = {
    .name = TYPE_SGI_KONA "/accumulation",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_accum_needed,
    .pre_load = sgi_kona_accum_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(accum_mem, SGIKonaState, 0, vmstate_sgi_board_memory,
                       SGIBoardMemory),
        VMSTATE_END_OF_LIST()
    },
};

/* Reasons waiting for the reason page, and a swap-in still taking words. */
static bool sgi_kona_pool_swap_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->reason_queue_len || s->reason_unread || s->swapin_stage;
}

static int sgi_kona_pool_swap_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    if (s->reason_queue_len > SGI_KONA_REASON_QUEUE ||
        s->swapin_stage > SGI_KONA_SWAPIN_REASON) {
        return -EINVAL;
    }
    for (uint32_t i = 0; i < s->reason_queue_len; i++) {
        if (!s->reason_queue_words[i] ||
            s->reason_queue_words[i] > SGI_KONA_REASON_WORDS) {
            return -EINVAL;
        }
    }
    if (s->reason_queue_len) {
        timer_mod(s->reason_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  SGI_KONA_REASON_POLL_NS);
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_pool_swap = {
    .name = TYPE_SGI_KONA "/pool-swap",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_pool_swap_needed,
    .post_load = sgi_kona_pool_swap_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_2DARRAY(reason_queue, SGIKonaState,
                               SGI_KONA_REASON_QUEUE, SGI_KONA_REASON_WORDS),
        VMSTATE_UINT32_ARRAY(reason_queue_token, SGIKonaState,
                             SGI_KONA_REASON_QUEUE),
        VMSTATE_UINT32_ARRAY(reason_queue_words, SGIKonaState,
                             SGI_KONA_REASON_QUEUE),
        VMSTATE_UINT32(reason_queue_len, SGIKonaState),
        VMSTATE_BOOL(reason_unread, SGIKonaState),
        VMSTATE_UINT32(reason_posted, SGIKonaState),
        VMSTATE_UINT32_ARRAY(swapin_header, SGIKonaState, 6),
        VMSTATE_UINT32(swapin_mode, SGIKonaState),
        VMSTATE_UINT32(swapin_stage, SGIKonaState),
        VMSTATE_UINT32(swapin_data_left, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_video_data = {
    .name = TYPE_SGI_KONA "/video-data",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_video_data_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(video_data, SGIKonaState,
                             SGI_KONA_VIDEO_MAX_DATA_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * State for the X server's commands: the sum of an open video command's
 * data words past its first, the graphics context, a run of box words
 * waiting for its X box, the X colour map, an open colour-map load's
 * start, the X position and clip, the pixel setup and an open pixel
 * write, and the one-bit drawing state (the scissor, the stipple cache,
 * the loaded stipple and a stipple store still taking its rows).
 */
static bool sgi_kona_x_server_needed(void *opaque)
{
    return true;
}

static const VMStateDescription vmstate_sgi_kona_x_server = {
    .name = TYPE_SGI_KONA "/x-server",
    .version_id = 5,
    .minimum_version_id = 5,
    .needed = sgi_kona_x_server_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(video_tail_sum, SGIKonaState),
        VMSTATE_UINT32_ARRAY(x_gc, SGIKonaState, SGI_KONA_X_GC_WORDS),
        VMSTATE_UINT32_ARRAY(x_boxes, SGIKonaState, SGI_KONA_X_BOX_RUN_WORDS),
        VMSTATE_UINT32(x_boxes_have, SGIKonaState),
        VMSTATE_UINT32(color_map_start, SGIKonaState),
        VMSTATE_UINT32(color_map_outside, SGIKonaState),
        VMSTATE_SUB_ARRAY(x_color_map, SGIKonaState, 0, 0x4000, 0,
                          vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32_ARRAY(x_position, SGIKonaState,
                             SGI_KONA_X_POSITION_WORDS),
        VMSTATE_UINT32(x_clip_count, SGIKonaState),
        VMSTATE_UINT32_ARRAY(x_clip_rects, SGIKonaState,
                             4 * SGI_KONA_X_CLIP_MAX_RECTS),
        VMSTATE_UINT32_ARRAY(x_clip_run, SGIKonaState,
                             SGI_KONA_X_CLIP_LIST_WORDS),
        VMSTATE_UINT32(x_clip_run_have, SGIKonaState),
        VMSTATE_UINT32_ARRAY(x_pixel_setup, SGIKonaState,
                             SGI_KONA_X_PIXEL_SETUP_WORDS),
        VMSTATE_BOOL(x_pixel_open, SGIKonaState),
        VMSTATE_INT32(x_pixel_x, SGIKonaState),
        VMSTATE_INT32(x_pixel_y, SGIKonaState),
        VMSTATE_UINT32(x_pixel_bpp, SGIKonaState),
        VMSTATE_UINT32(x_pixel_stride, SGIKonaState),
        VMSTATE_UINT32(x_pixel_words, SGIKonaState),
        VMSTATE_UINT32(x_pixel_remaining, SGIKonaState),
        VMSTATE_UINT32(x_pixel_prev, SGIKonaState),
        VMSTATE_BOOL(x_scissor_valid, SGIKonaState),
        VMSTATE_UINT32_ARRAY(x_scissor, SGIKonaState, SGI_KONA_X_SCISSOR_WORDS),
        VMSTATE_UINT32_ARRAY(x_stipple_width, SGIKonaState,
                             SGI_KONA_X_STIPPLE_ENTRIES),
        VMSTATE_UINT32_ARRAY(x_stipple_height, SGIKonaState,
                             SGI_KONA_X_STIPPLE_ENTRIES),
        VMSTATE_INT32_ARRAY(x_stipple_origin_x, SGIKonaState,
                            SGI_KONA_X_STIPPLE_ENTRIES),
        VMSTATE_INT32_ARRAY(x_stipple_origin_y, SGIKonaState,
                            SGI_KONA_X_STIPPLE_ENTRIES),
        VMSTATE_UINT32_ARRAY(x_stipple_rows, SGIKonaState,
                             SGI_KONA_X_STIPPLE_ENTRIES *
                               SGI_KONA_X_STIPPLE_MAX),
        VMSTATE_UINT32(x_stipple_loaded, SGIKonaState),
        VMSTATE_UINT32(x_stipple_store_entry, SGIKonaState),
        VMSTATE_UINT32(x_stipple_store_width, SGIKonaState),
        VMSTATE_UINT32(x_stipple_store_height, SGIKonaState),
        VMSTATE_UINT32(x_stipple_store_remaining, SGIKonaState),
        VMSTATE_SUB_ARRAY(x_color_map, SGIKonaState, 0x4000, 0x4000, 0,
                          vmstate_info_uint32, uint32_t),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * State for graphics contexts and their clients: a GL texture table still
 * taking its data words, the context table, the current context and
 * whether it waits for its return page, the page's GL grammar (the
 * buffer-configuration framer, whether a context has been activated and
 * whether a 1 is held), each entry's GL context, window and buffers, and
 * the GL framing of the whole 3D grammar (the top half of the GL command
 * word being framed, a GL data run still taking its words, and each
 * entry's pixel-store state).
 */
static bool sgi_kona_contexts_needed(void *opaque)
{
    return true;
}

static const VMStateDescription vmstate_sgi_kona_context = {
    .name = TYPE_SGI_KONA "/context",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(valid, SGIKonaContext),
        VMSTATE_UINT32(save_page, SGIKonaContext),
        VMSTATE_UINT32(save_offset, SGIKonaContext),
        VMSTATE_BOOL(return_page_valid, SGIKonaContext),
        VMSTATE_UINT64(return_page, SGIKonaContext),
        VMSTATE_UINT64(generation, SGIKonaContext),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * The initial credit configuration the driver writes, and the processor
 * stores whose graphics credits wait for their packet's last word, which
 * a packet part-way through a migration still owes.
 */
static bool sgi_kona_credit_needed(void *opaque)
{
    return true;
}

/* The open X pixel write's owner, while its rows have words to come. */
static bool sgi_kona_x_pixel_owner_needed(void *opaque)
{
    return ((SGIKonaState *)opaque)->x_pixel_remaining != 0;
}

static int sgi_kona_x_pixel_owner_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    if (s->x_pixel_owner != SGI_KONA_CONTEXT_NONE &&
        (s->x_pixel_owner < 0 || s->x_pixel_owner >= SGI_KONA_CONTEXTS ||
         !sgi_kona_context_owner(s, s->x_pixel_owner,
                                 s->x_pixel_owner_generation))) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_x_pixel_owner = {
    .name = TYPE_SGI_KONA "/x-pixel-owner",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_x_pixel_owner_needed,
    .post_load = sgi_kona_x_pixel_owner_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(x_pixel_owner, SGIKonaState),
        VMSTATE_UINT64(x_pixel_owner_generation, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

/* The owner of an X data or clip-list run, while its words wait. */
static bool sgi_kona_x_run_owner_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->x_boxes_have || s->x_clip_run_have;
}

static int sgi_kona_x_run_owner_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    if (s->x_run_owner != SGI_KONA_CONTEXT_NONE &&
        (s->x_run_owner < 0 || s->x_run_owner >= SGI_KONA_CONTEXTS ||
         !sgi_kona_context_owner(s, s->x_run_owner,
                                 s->x_run_owner_generation))) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_x_run_owner = {
    .name = TYPE_SGI_KONA "/x-run-owner",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_x_run_owner_needed,
    .post_load = sgi_kona_x_run_owner_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(x_run_owner, SGIKonaState),
        VMSTATE_UINT64(x_run_owner_generation, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_saved_port = {
    .name = TYPE_SGI_KONA "/saved-port",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(framing, SGIKonaSavedPort),
        VMSTATE_UINT32(selector, SGIKonaSavedPort),
        VMSTATE_UINT32(argc_expected, SGIKonaSavedPort),
        VMSTATE_UINT32(argc_have, SGIKonaSavedPort),
        VMSTATE_UINT32_ARRAY(args, SGIKonaSavedPort,
                             SGI_COMMAND_PORT_MAX_ARGS),
        VMSTATE_END_OF_LIST()
    },
};

/* Defined with the live uploads they also migrate. */
static const VMStateInfo vmstate_sgi_kona_texture_upload;
static const VMStateInfo vmstate_sgi_kona_gl_image_upload;

static bool sgi_kona_saved_texture_needed(void *opaque)
{
    return ((SGIKonaSavedInput *)opaque)->texture_upload.count != 0;
}

static const VMStateDescription vmstate_sgi_kona_saved_texture = {
    .name = TYPE_SGI_KONA "/saved-input-entry/gl-run/texture",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_saved_texture_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_SINGLE(texture_upload, SGIKonaSavedInput, 1,
                       vmstate_sgi_kona_texture_upload, SGIKonaTextureUpload),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_saved_image_needed(void *opaque)
{
    return ((SGIKonaSavedInput *)opaque)->gl_image_upload.count != 0;
}

static const VMStateDescription vmstate_sgi_kona_saved_image = {
    .name = TYPE_SGI_KONA "/saved-input-entry/gl-run/image",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_saved_image_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_SINGLE(gl_image_upload, SGIKonaSavedInput, 0,
                       vmstate_sgi_kona_gl_image_upload, SGIKonaGLImageUpload),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_saved_gl_bitmap_needed(void *opaque)
{
    return ((SGIKonaSavedInput *)opaque)->gl_bitmap_upload.count != 0;
}

static const VMStateDescription vmstate_sgi_kona_saved_gl_bitmap = {
    .name = TYPE_SGI_KONA "/saved-input-entry/gl-run/bitmap",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_saved_gl_bitmap_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(gl_bitmap_upload.header, SGIKonaSavedInput, 8),
        VMSTATE_UINT32(gl_bitmap_upload.context, SGIKonaSavedInput),
        VMSTATE_UINT64(gl_bitmap_upload.generation, SGIKonaSavedInput),
        VMSTATE_UINT32(gl_bitmap_upload.count, SGIKonaSavedInput),
        VMSTATE_BOOL(gl_bitmap_upload.refused, SGIKonaSavedInput),
        VMSTATE_UINT8_ARRAY(gl_bitmap_upload.bytes, SGIKonaSavedInput,
                            SGI_KONA_GL_BITMAP_BYTES),
        VMSTATE_END_OF_LIST()
    },
};

/* A held 1 1 and the word after it, only while the decision is open. */
static bool sgi_kona_saved_level1_needed(void *opaque)
{
    return ((SGIKonaSavedInput *)opaque)->gl_level1_held != 0;
}

static const VMStateDescription vmstate_sgi_kona_saved_level1 = {
    .name = TYPE_SGI_KONA "/saved-input-entry/gl-level1",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_saved_level1_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(gl_level1_held, SGIKonaSavedInput),
        VMSTATE_UINT32(gl_level1_arg, SGIKonaSavedInput),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_saved_map_needed(void *opaque)
{
    return ((SGIKonaSavedInput *)opaque)->gl_map_upload.header[1] != 0;
}

static const VMStateDescription vmstate_sgi_kona_saved_map = {
    .name = TYPE_SGI_KONA "/saved-input-entry/gl-run/pixel-map",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_saved_map_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(gl_map_upload.header, SGIKonaSavedInput, 4),
        VMSTATE_UINT32(gl_map_upload.token, SGIKonaSavedInput),
        VMSTATE_UINT32(gl_map_upload.context, SGIKonaSavedInput),
        VMSTATE_UINT64(gl_map_upload.generation, SGIKonaSavedInput),
        VMSTATE_BOOL(gl_map_upload.refused, SGIKonaSavedInput),
        VMSTATE_UINT32_ARRAY(gl_map_upload.values, SGIKonaSavedInput,
                             SGI_KONA_GL_PIXEL_MAP_MAX),
        VMSTATE_END_OF_LIST()
    },
};

/* A saved GL data run, only while one is saved; its uploads follow. */
static bool sgi_kona_saved_gl_run_needed(void *opaque)
{
    return ((SGIKonaSavedInput *)opaque)->gl_data_remaining != 0;
}

static const VMStateDescription vmstate_sgi_kona_saved_gl_run = {
    .name = TYPE_SGI_KONA "/saved-input-entry/gl-run",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_saved_gl_run_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gl_data_token, SGIKonaSavedInput),
        VMSTATE_UINT32(gl_data_remaining, SGIKonaSavedInput),
        VMSTATE_UINT32(gl_data_have, SGIKonaSavedInput),
        VMSTATE_UINT32_ARRAY(gl_data, SGIKonaSavedInput,
                             SGI_KONA_GL_DATA_MAX),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_kona_saved_texture,
        &vmstate_sgi_kona_saved_image,
        &vmstate_sgi_kona_saved_gl_bitmap,
        &vmstate_sgi_kona_saved_map,
        NULL
    },
};

static const VMStateDescription vmstate_sgi_kona_flash_op = {
    .name = TYPE_SGI_KONA "/flash-operation",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(op, SGIKonaFlashOp),
        VMSTATE_UINT32(need, SGIKonaFlashOp),
        VMSTATE_UINT32(have, SGIKonaFlashOp),
        VMSTATE_UINT32_ARRAY(args, SGIKonaFlashOp, 3),
        VMSTATE_UINT32(data, SGIKonaFlashOp),
        VMSTATE_UINT64(flash_addr, SGIKonaFlashOp),
        VMSTATE_BOOL(store, SGIKonaFlashOp),
        VMSTATE_BOOL(outside, SGIKonaFlashOp),
        VMSTATE_END_OF_LIST()
    },
};

/* A saved context's EEPROM operation, only while one is open. */
static bool sgi_kona_saved_flash_op_needed(void *opaque)
{
    SGIKonaSavedInput *v = opaque;

    return v->flash_op.op;
}

static const VMStateDescription vmstate_sgi_kona_saved_flash_op = {
    .name = TYPE_SGI_KONA "/saved-input-entry/flash-op",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_saved_flash_op_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(flash_op, SGIKonaSavedInput, 1,
                       vmstate_sgi_kona_flash_op, SGIKonaFlashOp),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_saved_video_data_needed(void *opaque)
{
    SGIKonaSavedInput *v = opaque;

    return v->video_open && v->video_have &&
           sgi_kona_video_data_words(v->video_command) !=
           SGI_KONA_VIDEO_DATA_UNKNOWN;
}

static const VMStateDescription vmstate_sgi_kona_saved_video_data = {
    .name = TYPE_SGI_KONA "/saved-input-entry/video-data",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_saved_video_data_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(video_data, SGIKonaSavedInput,
                             SGI_KONA_VIDEO_MAX_DATA_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_saved_one = {
    .name = TYPE_SGI_KONA "/saved-input-entry",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(valid, SGIKonaSavedInput),
        VMSTATE_STRUCT(textport, SGIKonaSavedInput, 1,
                       vmstate_sgi_kona_saved_port, SGIKonaSavedPort),
        VMSTATE_STRUCT(gl_port, SGIKonaSavedInput, 1,
                       vmstate_sgi_kona_saved_port, SGIKonaSavedPort),
        VMSTATE_BOOL(textport_expect_sync, SGIKonaSavedInput),
        VMSTATE_BOOL(gl_sync_held, SGIKonaSavedInput),
        VMSTATE_UINT32(gl_token_high, SGIKonaSavedInput),
        VMSTATE_BOOL(video_open, SGIKonaSavedInput),
        VMSTATE_UINT32(video_command, SGIKonaSavedInput),
        VMSTATE_UINT32(video_have, SGIKonaSavedInput),
        VMSTATE_UINT32(video_first, SGIKonaSavedInput),
        VMSTATE_UINT32(video_tail_sum, SGIKonaSavedInput),
        VMSTATE_UINT32(color_map_start, SGIKonaSavedInput),
        VMSTATE_UINT32(color_map_count, SGIKonaSavedInput),
        VMSTATE_UINT32(color_map_remaining, SGIKonaSavedInput),
        VMSTATE_UINT32(color_map_outside, SGIKonaSavedInput),
        VMSTATE_UINT32(gamma_start, SGIKonaSavedInput),
        VMSTATE_UINT32(gamma_count, SGIKonaSavedInput),
        VMSTATE_UINT32(gamma_remaining, SGIKonaSavedInput),
        VMSTATE_UINT32(gamma_outside, SGIKonaSavedInput),
        VMSTATE_UINT32(cursor_remaining, SGIKonaSavedInput),
        VMSTATE_UINT32(cursor_stage_words, SGIKonaSavedInput),
        VMSTATE_UINT32(cursor_stage_entry, SGIKonaSavedInput),
        VMSTATE_UINT32_ARRAY(cursor_stage, SGIKonaSavedInput, 260),
        VMSTATE_UINT32(gl_tex_table_remaining, SGIKonaSavedInput),
        VMSTATE_UINT32(gl_tex_table_slot, SGIKonaSavedInput),
        VMSTATE_UINT32_ARRAY(gl_tex_table_words, SGIKonaSavedInput,
                             SGI_KONA_GL_TEX_TABLE_WORDS),
        VMSTATE_BOOL(gl_tex_table_refused, SGIKonaSavedInput),
        VMSTATE_UINT32(calls_token, SGIKonaSavedInput),
        VMSTATE_UINT32(calls_need, SGIKonaSavedInput),
        VMSTATE_UINT32(calls_have, SGIKonaSavedInput),
        VMSTATE_UINT32_ARRAY(calls_words, SGIKonaSavedInput,
                             2 * SGI_KONA_DMA_LIST_MAX_RANGES),
        VMSTATE_UINT32(x_stipple_store_entry, SGIKonaSavedInput),
        VMSTATE_UINT32(x_stipple_store_width, SGIKonaSavedInput),
        VMSTATE_UINT32(x_stipple_store_height, SGIKonaSavedInput),
        VMSTATE_UINT32(x_stipple_store_remaining, SGIKonaSavedInput),
        VMSTATE_STRUCT(kernel_port, SGIKonaSavedInput, 1,
                       vmstate_sgi_kona_saved_port, SGIKonaSavedPort),
        VMSTATE_UINT32(bitmap_state, SGIKonaSavedInput),
        VMSTATE_UINT32_ARRAY(bitmap_header, SGIKonaSavedInput,
                             SGI_KONA_BITMAP_HEADER_WORDS),
        VMSTATE_UINT32(bitmap_header_have, SGIKonaSavedInput),
        VMSTATE_UINT32(bitmap_width, SGIKonaSavedInput),
        VMSTATE_UINT32(bitmap_height, SGIKonaSavedInput),
        VMSTATE_UINT32(bitmap_payload_have, SGIKonaSavedInput),
        VMSTATE_UINT32(bitmap_payload_expected, SGIKonaSavedInput),
        VMSTATE_VALIDATE("saved bitmap payload within bound",
                         sgi_kona_saved_bitmap_count_valid),
        VMSTATE_VARRAY_UINT32_ALLOC(bitmap_payload, SGIKonaSavedInput,
                                   bitmap_payload_expected, 0,
                                   vmstate_info_uint32, uint32_t),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_kona_saved_gl_run,
        &vmstate_sgi_kona_saved_flash_op,
        &vmstate_sgi_kona_saved_level1,
        &vmstate_sgi_kona_saved_video_data,
        NULL
    },
};

/* The saved open commands, only while one is saved. */
static bool sgi_kona_saved_input_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        if (s->saved_input[i].valid) {
            return true;
        }
    }
    return false;
}

/*
 * A saved framer as the live one could have left it: idle and empty, or
 * part-way through a packet of a selector in its own table, with the
 * table's word count and fewer words than that.
 */
static bool sgi_kona_saved_port_valid(const SGIKonaSavedPort *p,
                                      const SGICommandPortSelector *table,
                                      unsigned n)
{
    if (!p->framing) {
        return !p->argc_have && !p->argc_expected;
    }
    for (unsigned i = 0; i < n; i++) {
        if (table[i].selector == p->selector) {
            return p->argc_expected == table[i].argc &&
                   p->argc_expected <= SGI_COMMAND_PORT_MAX_ARGS &&
                   p->argc_have < p->argc_expected;
        }
    }
    return false;
}

/* Defined with the live uploads' own load paths. */
static bool sgi_kona_texture_upload_valid(const SGIKonaTextureUpload *u);
static bool sgi_kona_texture_upload_run_valid(SGIKonaState *s, int owner,
                                              const SGIKonaTextureUpload *u,
                                              uint32_t token, uint32_t have,
                                              uint32_t remaining);
static bool sgi_kona_gl_image_run_valid(SGIKonaState *s, int owner,
                                        const SGIKonaGLImageUpload *u,
                                        uint32_t token, uint32_t have,
                                        uint32_t remaining);
static bool sgi_kona_gl_bitmap_run_valid(SGIKonaState *s, int owner,
                                         const SGIKonaGLBitmapUpload *u,
                                         uint32_t token, uint32_t have,
                                         uint32_t remaining);
static bool sgi_kona_gl_pixel_map_run_valid(SGIKonaState *s, int owner,
                                            const SGIKonaGLPixelMapUpload *u,
                                            uint32_t token, uint32_t have,
                                            uint32_t remaining);

/*
 * A saved GL data run as the live one could be left by its owner: a
 * token that has a data run, no GL packet or texture table framing beside
 * it, and only the uploads its token fills, each passing the check its
 * live load applies, with the owner in place of the current context.
 */
static bool sgi_kona_saved_gl_run_valid(SGIKonaState *s, int owner,
                                        const SGIKonaSavedInput *v)
{
    uint32_t token = v->gl_data_token, have = v->gl_data_have;
    uint32_t remaining = v->gl_data_remaining;
    bool texel = token == SGI_KONA_CMD_GL_TEX_LOAD ||
                 token == SGI_KONA_CMD_GL_DRAW_PIXELS;

    if (!remaining || v->gl_port.framing || v->gl_tex_table_remaining ||
        (!texel && token != SGI_KONA_CMD_GL_POLYGON_STIPPLE &&
         token != SGI_KONA_CMD_GL_POP_LIGHTS &&
         token != SGI_KONA_CMD_GL_BITMAP_INLINE &&
         token != SGI_KONA_CMD_GL_BITMAP &&
         token != SGI_KONA_CMD_GL_ARRAY_BULK &&
         !sgi_kona_gl_pixel_map_token(token))) {
        return false;
    }
    if (v->texture_upload.count &&
        (!texel || !sgi_kona_texture_upload_valid(&v->texture_upload) ||
         !sgi_kona_texture_upload_run_valid(s, owner, &v->texture_upload,
                                            token, have, remaining))) {
        return false;
    }
    if (v->gl_image_upload.count &&
        ((token != SGI_KONA_CMD_GL_BITMAP &&
          !(token == SGI_KONA_CMD_GL_DRAW_PIXELS &&
            v->gl_image_upload.draw_pixels)) ||
         !sgi_kona_gl_image_run_valid(s, owner, &v->gl_image_upload, token,
                                      have, remaining))) {
        return false;
    }
    if (v->gl_bitmap_upload.count &&
        !sgi_kona_gl_bitmap_run_valid(s, owner, &v->gl_bitmap_upload, token,
                                      have, remaining)) {
        return false;
    }
    if (v->gl_map_upload.header[1] &&
        !sgi_kona_gl_pixel_map_run_valid(s, owner, &v->gl_map_upload, token,
                                         have, remaining)) {
        return false;
    }
    return true;
}

/*
 * A saved command must be one the live input could have held. Each check
 * is the live field's own, from sgi_kona_post_load_validate(), the DMA
 * engine's post_load and the contexts' post_load: framers by their
 * tables, a list invocation by its token, even count and bound, the
 * texture-table slot, and the counted runs. The slot must be a context in
 * use, the transient one only while it holds state.
 */
static int sgi_kona_saved_input_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        const SGIKonaSavedInput *v = &s->saved_input[i];
        uint32_t calls_max = v->calls_token == SGI_KONA_CMD_LIST_CALLS ?
                             2 * SGI_KONA_LIST_CALLS_MAX :
                             2 * SGI_KONA_LIST_CALLS_POOL_MAX;

        if (!v->valid) {
            /* An empty slot owns no buffer. */
            if (v->bitmap_state != SGI_KONA_BITMAP_IDLE ||
                v->bitmap_payload_expected || v->gl_data_remaining ||
                v->flash_op.op) {
                return -EINVAL;
            }
            continue;
        }
        if (!sgi_kona_flash_op_valid(&v->flash_op)) {
            return -EINVAL;
        }
        if (i == SGI_KONA_CONTEXT_TRANSIENT ?
            !s->state[SGI_KONA_CONTEXT_TRANSIENT] : i >= s->context_count) {
            return -EINVAL;
        }
        if (!sgi_kona_saved_port_valid(&v->textport,
                                       sgi_kona_textport_selectors,
                                       sgi_kona_textport_num_selectors) ||
            !sgi_kona_saved_port_valid(&v->gl_port, sgi_kona_gl_tokens,
                                       sgi_kona_gl_num_tokens) ||
            !sgi_kona_saved_port_valid(&v->kernel_port,
                                       sgi_kona_command_tokens,
                                       sgi_kona_command_num_tokens) ||
            (v->gl_sync_held &&
             (v->gl_port.framing || v->textport_expect_sync)) ||
            v->gl_level1_held > 2 ||
            (v->gl_level1_held &&
             (v->gl_sync_held || v->gl_port.framing ||
              v->textport_expect_sync))) {
            return -EINVAL;
        }
        if (v->calls_need &&
            ((v->calls_token != SGI_KONA_CMD_LIST_CALLS &&
              v->calls_token != SGI_KONA_CMD_LIST_CALLS_POOL) ||
             v->calls_need > calls_max || v->calls_need % 2 ||
             v->calls_have >= v->calls_need)) {
            return -EINVAL;
        }
        if (!v->calls_need && v->calls_have) {
            return -EINVAL;
        }
        if (v->gl_tex_table_slot >= SGI_KONA_TEXTURE_CURVE_SLOTS &&
            !v->gl_tex_table_refused) {
            return -EINVAL;
        }
        if (v->video_open) {
            uint32_t length = sgi_kona_video_data_words(v->video_command);

            if (length == SGI_KONA_VIDEO_DATA_UNKNOWN ?
                v->video_have >= SGI_KONA_VIDEO_SEARCH_MAX_WORDS :
                v->video_have > length) {
                return -EINVAL;
            }
        }
        if ((v->cursor_stage_words != 0 && v->cursor_stage_words != 4 &&
             v->cursor_stage_words != SGI_KONA_X_CURSOR_DATA_WORDS) ||
            v->cursor_stage_entry > SGI_KONA_CURSOR_ENTRIES ||
            v->cursor_remaining > v->cursor_stage_words ||
            !v->cursor_remaining != !v->cursor_stage_words ||
            v->gamma_remaining > v->gamma_count ||
            v->gamma_outside > v->gamma_count - v->gamma_remaining ||
            v->color_map_remaining > v->color_map_count ||
            v->color_map_outside >
            v->color_map_count - v->color_map_remaining ||
            v->gl_tex_table_remaining > SGI_KONA_GL_TEX_TABLE_WORDS ||
            v->x_stipple_store_height > SGI_KONA_X_STIPPLE_MAX ||
            v->x_stipple_store_remaining > v->x_stipple_store_height ||
            !sgi_kona_bitmap_fields_valid(v->bitmap_state, v->bitmap_header,
                                          v->bitmap_header_have,
                                          v->bitmap_width, v->bitmap_height,
                                          v->bitmap_payload,
                                          v->bitmap_payload_have,
                                          v->bitmap_payload_expected) ||
            ((v->gl_data_remaining || v->gl_data_token || v->gl_data_have ||
              v->texture_upload.count || v->gl_image_upload.count ||
              v->gl_bitmap_upload.count || v->gl_map_upload.header[1]) &&
             !sgi_kona_saved_gl_run_valid(s, i, v))) {
            return -EINVAL;
        }
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_saved_input = {
    .name = TYPE_SGI_KONA "/saved-input",
    .version_id = 3,
    .minimum_version_id = 3,
    .needed = sgi_kona_saved_input_needed,
    .post_load = sgi_kona_saved_input_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(saved_input, SGIKonaState, SGI_KONA_CONTEXTS, 1,
                             vmstate_sgi_kona_saved_one, SGIKonaSavedInput),
        VMSTATE_END_OF_LIST()
    },
};

/* The bracket's input state, only while it differs from reset. */
static bool sgi_kona_bracket_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->kernel_input || s->suspended_owner != SGI_KONA_CONTEXT_NONE ||
           (s->kernel_port.framing && s->kernel_port_privileged);
}

/*
 * A kernel-framer command set aside by a forced store is a packet of the
 * kernel framer's own table that the live framer could have held, and it
 * belongs to the suspended command's owner.
 */
static int sgi_kona_bracket_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    if (s->suspended_owner != SGI_KONA_CONTEXT_NONE &&
        (s->suspended_owner < 0 || s->suspended_owner >= SGI_KONA_CONTEXTS)) {
        return -EINVAL;
    }
    if (!sgi_kona_saved_port_valid(&s->kernel_port_suspended,
                                   sgi_kona_command_tokens,
                                   sgi_kona_command_num_tokens) ||
        (s->kernel_port_suspended.framing &&
         s->suspended_owner == SGI_KONA_CONTEXT_NONE)) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_bracket = {
    .name = TYPE_SGI_KONA "/bracket",
    .version_id = 2,
    .minimum_version_id = 2,
    .needed = sgi_kona_bracket_needed,
    .post_load = sgi_kona_bracket_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(kernel_input, SGIKonaState),
        VMSTATE_INT32(suspended_owner, SGIKonaState),
        VMSTATE_BOOL(kernel_port_privileged, SGIKonaState),
        VMSTATE_STRUCT(kernel_port_suspended, SGIKonaState, 1,
                       vmstate_sgi_kona_saved_port, SGIKonaSavedPort),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_credit = {
    .name = TYPE_SGI_KONA "/credit",
    .version_id = 3,
    .minimum_version_id = 3,
    .needed = sgi_kona_credit_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(credit_config, SGIKonaState),
        VMSTATE_UINT32(pi_held, SGIKonaState),
        VMSTATE_UINT32(pi_parked, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * The retrace tag register. The requests still waiting are the scanout's
 * swap records.
 */
static bool sgi_kona_retrace_needed(void *opaque)
{
    return true;
}

static const VMStateDescription vmstate_sgi_kona_pending_swap = {
    .name = TYPE_SGI_KONA "/pending-swap",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(valid, SGIKonaPendingSwap),
        VMSTATE_UINT64(seq, SGIKonaPendingSwap),
        VMSTATE_UINT64(generation, SGIKonaPendingSwap),
        VMSTATE_INT32(context, SGIKonaPendingSwap),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_retrace = {
    .name = TYPE_SGI_KONA "/retrace",
    .version_id = 2,
    .minimum_version_id = 2,
    .needed = sgi_kona_retrace_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(retrace_tag, SGIKonaState),
        VMSTATE_STRUCT_ARRAY(pending_swaps, SGIKonaState, SGI_SWAP_MAX, 0,
                             vmstate_sgi_kona_pending_swap, SGIKonaPendingSwap),
        VMSTATE_BOOL(video_delivered, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_window = {
    .name = TYPE_SGI_KONA "/gl-window",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(placed, SGIKonaGLWindow),
        VMSTATE_INT32(x, SGIKonaGLWindow),
        VMSTATE_INT32(y, SGIKonaGLWindow),
        VMSTATE_UINT32(width, SGIKonaGLWindow),
        VMSTATE_UINT32(height, SGIKonaGLWindow),
        VMSTATE_UINT32(double_buffered, SGIKonaGLWindow),
        VMSTATE_UINT32(displayed_bank, SGIKonaGLWindow),
        VMSTATE_UINT32(display_id, SGIKonaGLWindow),
        VMSTATE_UINT32(partial, SGIKonaGLWindow),
        VMSTATE_UINT64(place_serial, SGIKonaGLWindow),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_gl_native_depth_valid(uint64_t bits)
{
    double d;

    if ((bits & UINT64_C(0x7ff0000000000000)) ==
        UINT64_C(0x7ff0000000000000)) {
        return false;
    }
    memcpy(&d, &bits, sizeof(d));
    return d >= 0 && d <= 1;
}

static int sgi_kona_gl_returns_post_load(void *opaque, int version_id)
{
    SGIKonaGLReturnState *r = opaque;
    uint32_t mask;

    if (!sgi_kona_gl_native_depth_valid(r->depth_range[0]) ||
        !sgi_kona_gl_native_depth_valid(r->depth_range[1]) ||
        !sgi_kona_gl_native_depth_valid(r->clear_depth) ||
        (r->read_buffer_unsupported ? sgi_kona_gl_read_enum(r->read_buffer) :
         r->read_buffer && !sgi_kona_gl_read_enum(r->read_buffer)) ||
        (r->draw_buffer && !sgi_kona_gl_draw_buffers(r->draw_buffer, &mask))) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_returns = {
    .name = TYPE_SGI_KONA "/gl-return-state",
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = sgi_kona_gl_returns_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(depth_range, SGIKonaGLReturnState, 2),
        VMSTATE_UINT64(clear_depth, SGIKonaGLReturnState),
        VMSTATE_UINT32(read_buffer, SGIKonaGLReturnState),
        VMSTATE_BOOL(read_buffer_unsupported, SGIKonaGLReturnState),
        VMSTATE_UINT32(draw_buffer, SGIKonaGLReturnState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_gl_config_valid(const uint32_t *v, bool received,
                                     bool valid)
{
    static const uint32_t empty[10];

    return received ? valid == sgi_kona_gl_colour_config(v) :
                      !valid && !memcmp(v, empty, sizeof(empty));
}

static int sgi_kona_gl_pipe_post_load(void *opaque, int version_id)
{
    SGIKonaGLPipe *p = opaque;

    if (!sgi_kona_gl_config_valid(p->draw_config, p->draw_config_received,
                                   p->draw_config_valid) ||
        !sgi_kona_gl_config_valid(p->read_config, p->read_config_received,
                                   p->read_config_valid) ||
        p->read_compatible != sgi_kona_gl_read_compatible(p) ||
        (!p->read_binding_unknown && p->read_binding_word) ||
        (p->texture_selector_valid && p->texture_selector >= 2) ||
        (p->texture_assoc_valid & ~3u) ||
        ((p->texture_assoc_valid & 1) &&
         p->texture_assoc_handle[0] == UINT32_MAX) ||
        ((p->texture_assoc_valid & 2) &&
         p->texture_assoc_handle[1] == UINT32_MAX)) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_unpack = {
    .name = TYPE_SGI_KONA "/gl-unpack",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(unpack_swap_bytes, SGIKonaGLPipe),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_error = {
    .name = TYPE_SGI_KONA "/gl-error",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pending_error, SGIKonaGLPipe),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_pixels = {
    .name = TYPE_SGI_KONA "/gl-pixels",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(pixel_zoom_unsupported, SGIKonaGLPipe),
        VMSTATE_BOOL(pixel_transfer_unsupported, SGIKonaGLPipe),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_zoom = {
    .name = TYPE_SGI_KONA "/gl-zoom",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(pixel_zoom, SGIKonaGLPipe, 2),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_zoom_attrib = {
    .name = TYPE_SGI_KONA "/gl-zoom-attrib",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(factors, SGIKonaGLPixelZoom, 2),
        VMSTATE_BOOL(unsupported, SGIKonaGLPixelZoom),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_pack = {
    .name = TYPE_SGI_KONA "/gl-pack",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(pack_swap_bytes, SGIKonaGLPipe),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_bitmap_pack = {
    .name = TYPE_SGI_KONA "/gl-bitmap-pack",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(pack_lsb_first, SGIKonaGLPipe),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_bitmap_unpack = {
    .name = TYPE_SGI_KONA "/gl-bitmap-unpack",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(unpack_lsb_first, SGIKonaGLPipe),
        VMSTATE_UINT32(unpack_skip_rows, SGIKonaGLPipe),
        VMSTATE_UINT32(unpack_skip_pixels, SGIKonaGLPipe),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_gl_pixel_map_valid(const SGIKonaGLPixelMap *map,
                                       bool colour, bool indexed)
{
    if (map->count > SGI_KONA_GL_PIXEL_MAP_MAX ||
        (indexed && map->count && !is_power_of_2(map->count))) {
        return false;
    }
    for (unsigned k = 0; k < SGI_KONA_GL_PIXEL_MAP_MAX; k++) {
        if (k < map->count ?
            colour && !isfinite(sgi_kona_gl_float(map->values[k])) :
            map->values[k] != 0) {
            return false;
        }
    }
    return true;
}

static const VMStateDescription vmstate_sgi_kona_gl_pixel_map_table = {
    .name = TYPE_SGI_KONA "/gl-pixel-map-table",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(count, SGIKonaGLPixelMap),
        VMSTATE_UINT32_ARRAY(values, SGIKonaGLPixelMap,
                             SGI_KONA_GL_PIXEL_MAP_MAX),
        VMSTATE_END_OF_LIST()
    },
};

static int sgi_kona_gl_transfer_post_load(void *opaque, int version_id)
{
    SGIKonaGLPixelTransfer *t = opaque;

    if (t->map_colour > 1 ||
        !sgi_kona_gl_pixel_map_valid(&t->index_map, false, true) ||
        !sgi_kona_gl_pixel_map_valid(&t->stencil_map, false, true)) {
        return -EINVAL;
    }
    for (unsigned k = 0; k < 4; k++) {
        if (!sgi_kona_gl_pixel_map_valid(&t->index_rgba[k], true, true) ||
            !sgi_kona_gl_pixel_map_valid(&t->colour_rgba[k], true, false)) {
            return -EINVAL;
        }
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_transfer = {
    .name = TYPE_SGI_KONA "/gl-transfer",
    .version_id = 3,
    .minimum_version_id = 3,
    .post_load = sgi_kona_gl_transfer_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(scale, SGIKonaGLPixelTransfer, 4),
        VMSTATE_UINT32_ARRAY(bias, SGIKonaGLPixelTransfer, 4),
        VMSTATE_UINT32(depth_scale, SGIKonaGLPixelTransfer),
        VMSTATE_UINT32(depth_bias, SGIKonaGLPixelTransfer),
        VMSTATE_UINT32(index_shift, SGIKonaGLPixelTransfer),
        VMSTATE_UINT32(index_offset, SGIKonaGLPixelTransfer),
        VMSTATE_UINT32(map_stencil, SGIKonaGLPixelTransfer),
        VMSTATE_UINT32(stencil_map.count, SGIKonaGLPixelTransfer),
        VMSTATE_UINT32_ARRAY(stencil_map.values, SGIKonaGLPixelTransfer,
                             SGI_KONA_GL_PIXEL_MAP_MAX),
        VMSTATE_STRUCT_ARRAY(index_rgba, SGIKonaGLPixelTransfer, 4, 0,
                             vmstate_sgi_kona_gl_pixel_map_table,
                             SGIKonaGLPixelMap),
        VMSTATE_STRUCT(index_map, SGIKonaGLPixelTransfer, 0,
                           vmstate_sgi_kona_gl_pixel_map_table,
                           SGIKonaGLPixelMap),
        VMSTATE_UINT32(map_colour, SGIKonaGLPixelTransfer),
        VMSTATE_STRUCT_ARRAY(colour_rgba, SGIKonaGLPixelTransfer, 4, 0,
                             vmstate_sgi_kona_gl_pixel_map_table,
                             SGIKonaGLPixelMap),
        VMSTATE_UINT32_ARRAY(post_conv_scale, SGIKonaGLPixelTransfer, 4),
        VMSTATE_UINT32_ARRAY(post_conv_bias, SGIKonaGLPixelTransfer, 4),
        VMSTATE_UINT32_ARRAY(post_matrix_scale, SGIKonaGLPixelTransfer, 4),
        VMSTATE_UINT32_ARRAY(post_matrix_bias, SGIKonaGLPixelTransfer, 4),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_transfer_pipe = {
    .name = TYPE_SGI_KONA "/gl-transfer-pipe",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(pixel_transfer, SGIKonaGLPipe, 0,
                        vmstate_sgi_kona_gl_transfer, SGIKonaGLPixelTransfer),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_pipe = {
    .name = TYPE_SGI_KONA "/gl-pipe",
    .version_id = 4,
    .minimum_version_id = 4,
    .post_load = sgi_kona_gl_pipe_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(unpack_alignment, SGIKonaGLPipe),
        VMSTATE_UINT32(unpack_row_length, SGIKonaGLPipe),
        VMSTATE_UINT32(pack_alignment, SGIKonaGLPipe),
        VMSTATE_UINT32(pack_row_length, SGIKonaGLPipe),
        VMSTATE_UINT32(depth_bits, SGIKonaGLPipe),
        VMSTATE_STRUCT(returns, SGIKonaGLPipe, 0,
                       vmstate_sgi_kona_gl_returns, SGIKonaGLReturnState),
        VMSTATE_UINT32_ARRAY(draw_config, SGIKonaGLPipe, 10),
        VMSTATE_UINT32_ARRAY(read_config, SGIKonaGLPipe, 10),
        VMSTATE_BOOL(draw_config_received, SGIKonaGLPipe),
        VMSTATE_BOOL(draw_config_valid, SGIKonaGLPipe),
        VMSTATE_BOOL(read_config_received, SGIKonaGLPipe),
        VMSTATE_BOOL(read_config_valid, SGIKonaGLPipe),
        VMSTATE_BOOL(read_compatible, SGIKonaGLPipe),
        VMSTATE_BOOL(read_binding_unknown, SGIKonaGLPipe),
        VMSTATE_UINT32(read_binding_word, SGIKonaGLPipe),
        VMSTATE_UINT32(texture_selector, SGIKonaGLPipe),
        VMSTATE_BOOL(texture_selector_valid, SGIKonaGLPipe),
        VMSTATE_UINT32_ARRAY(texture_assoc_handle, SGIKonaGLPipe, 2),
        VMSTATE_UINT8(texture_assoc_valid, SGIKonaGLPipe),
        VMSTATE_BOOL(texture_rebind_refused, SGIKonaGLPipe),
        VMSTATE_UINT32(array_state, SGIKonaGLPipe),
        VMSTATE_BOOL(array_state_valid, SGIKonaGLPipe),
        VMSTATE_BOOL(array_open, SGIKonaGLPipe),
        VMSTATE_UINT32_ARRAY(accum_clear, SGIKonaGLPipe, 4),
        VMSTATE_UINT32(multisample_enables, SGIKonaGLPipe),
        VMSTATE_UINT32(sample_mask_value, SGIKonaGLPipe),
        VMSTATE_UINT32(sample_mask_invert, SGIKonaGLPipe),
        VMSTATE_UINT32(sample_pattern, SGIKonaGLPipe),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * Each GL context's drawable is the window's and the banks' (see
 * sgi_kona_gl_set_drawable()), so it is set again from them, after the
 * shared description has refused a context its own commands could not
 * have left.
 */
static int sgi_kona_contexts_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    if (s->gl_tex_table_slot >= SGI_KONA_TEXTURE_CURVE_SLOTS &&
        !s->gl_tex_table_refused) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        const SGIKonaGLWindow *w;
        const SGIKonaGLPipe *p;

        if (!s->state[i]) {
            continue;
        }
        w = &s->state[i]->window;
        p = &s->state[i]->pipe;

        if (!p->pixel_zoom_unsupported &&
            (!isfinite(sgi_kona_gl_float(p->pixel_zoom[0])) ||
             !isfinite(sgi_kona_gl_float(p->pixel_zoom[1])))) {
            return -EINVAL;
        }
        if (!sgi_kona_gl_error_valid(p->pending_error) ||
            w->placed > 1 || w->double_buffered > 1 ||
            w->displayed_bank > 1 ||
            (p->draw_config_received &&
             ((w->placed &&
               w->double_buffered != (p->draw_config[3] != 0)) ||
              p->depth_bits != p->draw_config[6]))) {
            return -EINVAL;
        }
        s->state[i]->gl.hw_id = i;
        sgi_kona_gl_set_drawable(s, i);
    }
    return 0;
}

static const VMStateInfo vmstate_sgi_kona_logical_records;

static const VMStateDescription vmstate_sgi_kona_contexts = {
    .name = TYPE_SGI_KONA "/contexts",
    .version_id = 7,
    .minimum_version_id = 7,
    .needed = sgi_kona_contexts_needed,
    .post_load = sgi_kona_contexts_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gl_tex_table_remaining, SGIKonaState),
        VMSTATE_UINT64(context_clock, SGIKonaState),
        {
            .name = "logical-records",
            .info = &vmstate_sgi_kona_logical_records,
            .flags = VMS_SINGLE,
            .size = sizeof(SGIKonaState),
            .offset = 0,
        },
        VMSTATE_BOOL(context_failed, SGIKonaState),
        VMSTATE_BUFFER_UNSAFE(context_error, SGIKonaState, 0, 256),
        VMSTATE_INT32(context_current, SGIKonaState),
        VMSTATE_BOOL(context_page_pending, SGIKonaState),
        VMSTATE_UINT32(gl_port.kick, SGIKonaState),
        VMSTATE_BOOL(gl_port.framing, SGIKonaState),
        VMSTATE_UINT32(gl_port.selector, SGIKonaState),
        VMSTATE_UINT32(gl_port.argc_expected, SGIKonaState),
        VMSTATE_UINT32(gl_port.argc_have, SGIKonaState),
        VMSTATE_UINT32_ARRAY(gl_port.args, SGIKonaState,
                             SGI_COMMAND_PORT_MAX_ARGS),
        VMSTATE_BOOL(gl_grammar, SGIKonaState),
        VMSTATE_BOOL(gl_sync_held, SGIKonaState),
        VMSTATE_UINT32(gl_token_high, SGIKonaState),
        VMSTATE_UINT32(gl_data_token, SGIKonaState),
        VMSTATE_UINT32(gl_data_remaining, SGIKonaState),
        VMSTATE_UINT32(gl_data_have, SGIKonaState),
        VMSTATE_UINT32_ARRAY(gl_data, SGIKonaState, SGI_KONA_GL_DATA_MAX),
        VMSTATE_UINT32(gl_tex_table_slot, SGIKonaState),
        VMSTATE_UINT32_ARRAY(gl_tex_table_words, SGIKonaState,
                             SGI_KONA_GL_TEX_TABLE_WORDS),
        VMSTATE_BOOL(gl_tex_table_refused, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_gl_image_valid(const SGIKonaGLImageUpload *u)
{
    SGIKonaGLBitmapLayout layout;

    if (u->draw_pixels) {
        SGIKonaGLPixelLayout pixels;

        return u->count && u->header[8] <= 1 &&
               (!u->lsb_first || u->header[3] == 7) &&
               u->count <= SGI_KONA_TEXTURE_BYTES / sizeof(uint32_t) &&
               u->header[0] <= SGI_KONA_GL_IMAGE_MAX &&
               u->header[1] <= SGI_KONA_GL_IMAGE_MAX &&
               (uint64_t)u->header[0] * u->header[1] <=
               SGI_KONA_TEXTURE_BYTES /
                   (u->header[3] == 1 ? sizeof(uint32_t) :
                                        4 * sizeof(double)) &&
               !u->header[7] &&
               sgi_kona_gl_pixels_layout(u, &pixels) &&
               pixels.payload_words == u->count &&
               (u->refused || u->context < SGI_KONA_CONTEXTS);
    }
    return u->count &&
           u->count <= SGI_KONA_TEXTURE_BYTES / sizeof(uint32_t) &&
           u->header[0] <= SGI_KONA_GL_IMAGE_MAX &&
           u->header[1] <= SGI_KONA_GL_IMAGE_MAX &&
           sgi_kona_gl_raw_bitmap_layout(u, &layout) &&
           layout.payload_words == u->count &&
           (u->refused || (u->context < SGI_KONA_CONTEXTS &&
                          sgi_kona_gl_bitmap_position_valid(u->header + 5,
                                                             false)));
}

static bool sgi_kona_gl_image_save(QEMUFile *f, void *pv, size_t size,
                                        const VMStateField *field,
                                        JSONWriter *vmdesc, Error **errp)
{
    SGIKonaGLImageUpload *u = pv;

    for (unsigned k = 0; k < 9; k++) {
        qemu_put_be32(f, u->header[k]);
    }
    qemu_put_be32(f, u->row_length);
    qemu_put_be32(f, u->skip_rows);
    qemu_put_be32(f, u->skip_pixels);
    qemu_put_be32(f, u->alignment);
    qemu_put_be32(f, u->context);
    qemu_put_be64(f, u->generation);
    qemu_put_be32(f, u->count);
    qemu_put_byte(f, u->lsb_first);
    qemu_put_byte(f, u->refused);
    for (uint32_t k = 0; k < u->count; k++) {
        qemu_put_be32(f, u->words[k]);
    }
    if (qemu_file_get_error(f)) {
        error_setg(errp, "KONA raw bitmap could not be saved");
        return false;
    }
    return true;
}

static bool sgi_kona_gl_image_load(QEMUFile *f, void *pv, size_t size,
                                        const VMStateField *field, Error **errp)
{
    SGIKonaGLImageUpload *u = pv, tmp = { .draw_pixels = u->draw_pixels };
    unsigned lsb, refused;

    for (unsigned k = 0; k < 9; k++) {
        tmp.header[k] = qemu_get_be32(f);
    }
    tmp.row_length = qemu_get_be32(f);
    tmp.skip_rows = qemu_get_be32(f);
    tmp.skip_pixels = qemu_get_be32(f);
    tmp.alignment = qemu_get_be32(f);
    tmp.context = qemu_get_be32(f);
    tmp.generation = qemu_get_be64(f);
    tmp.count = qemu_get_be32(f);
    lsb = qemu_get_byte(f);
    refused = qemu_get_byte(f);
    tmp.lsb_first = lsb;
    tmp.refused = refused;
    if (lsb > 1 || refused > 1 || !sgi_kona_gl_image_valid(&tmp)) {
        goto invalid;
    }
    tmp.words = g_try_new(uint32_t, tmp.count);
    if (!tmp.words) {
        goto invalid;
    }
    for (uint32_t k = 0; k < tmp.count; k++) {
        tmp.words[k] = qemu_get_be32(f);
    }
    if (qemu_file_get_error(f)) {
        goto invalid;
    }
    g_free(u->words);
    *u = tmp;
    return true;
invalid:
    g_free(tmp.words);
    error_setg(errp, "KONA raw bitmap has invalid dimensions or framing");
    return false;
}

static const VMStateInfo vmstate_sgi_kona_gl_image_upload = {
    .name = "sgi-kona-gl-raw-bitmap-upload",
    .load = sgi_kona_gl_image_load,
    .save = sgi_kona_gl_image_save,
};

static bool sgi_kona_gl_raw_bitmap_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->gl_image_upload.count && !s->gl_image_upload.draw_pixels;
}

/* A raw bitmap or DrawPixels image as its run can leave it, for its owner. */
static bool sgi_kona_gl_image_run_valid(SGIKonaState *s, int owner,
                                        const SGIKonaGLImageUpload *u,
                                        uint32_t token, uint32_t have,
                                        uint32_t remaining)
{
    bool dma = u->draw_pixels ? u->header[5] == 1 : u->header[3] == 1;

    if (!sgi_kona_gl_image_valid(u) ||
        (dma ? (token || have || remaining) :
         (token != (u->draw_pixels ? SGI_KONA_CMD_GL_DRAW_PIXELS :
                                               SGI_KONA_CMD_GL_BITMAP) ||
          have >= u->count ||
          remaining != u->count - have)) ||
        (!u->refused && (u->context != owner ||
        !sgi_kona_context_owner(s, u->context, u->generation) ||
                        !s->contexts[u->context].valid))) {
        return false;
    }
    for (uint32_t k = have; k < u->count; k++) {
        if (u->words[k]) {
            return false;
        }
    }
    return true;
}

static int sgi_kona_gl_image_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    if (!sgi_kona_gl_image_run_valid(s, s->context_current,
                                     &s->gl_image_upload, s->gl_data_token,
                                     s->gl_data_have, s->gl_data_remaining)) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_raw_bitmap = {
    .name = TYPE_SGI_KONA "/gl-raw-bitmap",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_gl_raw_bitmap_needed,
    .post_load = sgi_kona_gl_image_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SINGLE(gl_image_upload, SGIKonaState, 0,
                        vmstate_sgi_kona_gl_image_upload,
                        SGIKonaGLImageUpload),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_gl_pixels_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->gl_image_upload.count && s->gl_image_upload.draw_pixels;
}

static int sgi_kona_gl_pixels_pre_load(void *opaque)
{
    SGIKonaState *s = opaque;

    s->gl_image_upload.draw_pixels = true;
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_raw_pixels = {
    .name = TYPE_SGI_KONA "/gl-raw-pixels",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_gl_pixels_needed,
    .pre_load = sgi_kona_gl_pixels_pre_load,
    .post_load = sgi_kona_gl_image_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SINGLE(gl_image_upload, SGIKonaState, 0,
                        vmstate_sgi_kona_gl_image_upload,
                        SGIKonaGLImageUpload),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_gl_bitmap_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->gl_bitmap_upload.count != 0;
}

/* An inline bitmap as its run can leave it, for the run's owner. */
static bool sgi_kona_gl_bitmap_run_valid(SGIKonaState *s, int owner,
                                         const SGIKonaGLBitmapUpload *u,
                                         uint32_t token, uint32_t have,
                                         uint32_t remaining)
{
    uint32_t words = DIV_ROUND_UP(u->count, 4);

    if (!u->count || u->count > SGI_KONA_GL_BITMAP_BYTES ||
        u->count != u->header[7] ||
        token != SGI_KONA_CMD_GL_BITMAP_INLINE ||
        have >= words ||
        remaining != words - have ||
        (!u->refused && (u->context >= SGI_KONA_CONTEXTS ||
                        owner != u->context ||
                        !sgi_kona_context_owner(s, u->context, u->generation) ||
                        !s->contexts[u->context].valid ||
                        !u->header[0] || (u->header[0] & 7) ||
                        !u->header[1] ||
                        u->header[0] > SGI_KONA_GL_IMAGE_MAX ||
                        u->header[1] > SGI_KONA_GL_IMAGE_MAX ||
                        (uint64_t)(u->header[0] / 8) * u->header[1] !=
                        u->count ||
                        u->header[6] != words + 1 ||
                        !sgi_kona_gl_bitmap_position_valid(u->header + 2,
                                                           true)))) {
        return false;
    }
    for (unsigned at = MIN(have * 4, u->count);
         at < sizeof(u->bytes); at++) {
        if (u->bytes[at]) {
            return false;
        }
    }
    return true;
}

static int sgi_kona_gl_bitmap_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    if (!sgi_kona_gl_bitmap_run_valid(s, s->context_current,
                                      &s->gl_bitmap_upload, s->gl_data_token,
                                      s->gl_data_have, s->gl_data_remaining)) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_bitmap = {
    .name = TYPE_SGI_KONA "/gl-bitmap",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_gl_bitmap_needed,
    .post_load = sgi_kona_gl_bitmap_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(gl_bitmap_upload.header, SGIKonaState, 8),
        VMSTATE_UINT32(gl_bitmap_upload.context, SGIKonaState),
        VMSTATE_UINT64(gl_bitmap_upload.generation, SGIKonaState),
        VMSTATE_UINT32(gl_bitmap_upload.count, SGIKonaState),
        VMSTATE_BOOL(gl_bitmap_upload.refused, SGIKonaState),
        VMSTATE_UINT8_ARRAY(gl_bitmap_upload.bytes, SGIKonaState,
                            SGI_KONA_GL_BITMAP_BYTES),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_gl_pixel_map_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->gl_map_upload.header[1] != 0;
}

/* A pixel map as its run can leave it, for the run's owner. */
static bool sgi_kona_gl_pixel_map_run_valid(SGIKonaState *s, int owner,
                                            const SGIKonaGLPixelMapUpload *u,
                                            uint32_t token, uint32_t have,
                                            uint32_t remaining)
{

    if (!u->header[1] || u->token != token ||
        (u->token != SGI_KONA_CMD_GL_PIXEL_MAP_F &&
         u->token != SGI_KONA_CMD_GL_PIXEL_MAP_UI &&
         u->token != SGI_KONA_CMD_GL_PIXEL_MAP_US) ||
        have >= u->header[1] ||
        remaining != u->header[1] - have ||
        (!u->refused && (u->context >= SGI_KONA_CONTEXTS ||
                        u->context != owner ||
        !sgi_kona_context_owner(s, u->context, u->generation) ||
                        !s->contexts[u->context].valid ||
                        s->state[u->context]->gl.in_begin ||
                        !sgi_kona_gl_pixel_map_header(u->header)))) {
        return false;
    }
    for (unsigned k = MIN(have, SGI_KONA_GL_PIXEL_MAP_MAX);
         k < SGI_KONA_GL_PIXEL_MAP_MAX; k++) {
        if (u->values[k]) {
            return false;
        }
    }
    return true;
}

static int sgi_kona_gl_pixel_map_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    if (!sgi_kona_gl_pixel_map_run_valid(s, s->context_current,
                                         &s->gl_map_upload, s->gl_data_token,
                                         s->gl_data_have,
                                         s->gl_data_remaining)) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_pixel_map = {
    .name = TYPE_SGI_KONA "/gl-pixel-map",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_gl_pixel_map_needed,
    .post_load = sgi_kona_gl_pixel_map_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(gl_map_upload.header, SGIKonaState, 4),
        VMSTATE_UINT32(gl_map_upload.token, SGIKonaState),
        VMSTATE_UINT32(gl_map_upload.context, SGIKonaState),
        VMSTATE_UINT64(gl_map_upload.generation, SGIKonaState),
        VMSTATE_BOOL(gl_map_upload.refused, SGIKonaState),
        VMSTATE_UINT32_ARRAY(gl_map_upload.values, SGIKonaState,
                             SGI_KONA_GL_PIXEL_MAP_MAX),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * The GL attribute stacks glPushAttrib filled, only while one holds a
 * copy. Each copy is a whole shared context, refused on load as the
 * contexts are when its own commands could not have left it.
 */
static int sgi_kona_gl_state_post_load(void *opaque, int version_id)
{
    SGIKonaGLState *state = opaque;

    if (state->attrib_depth > SGI_KONA_GL_ATTRIB_DEPTH) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < state->attrib_depth; i++) {
        const SGIKonaGLPixelZoom *z = &state->zoom_attrib[i];

        if (!z->unsupported &&
            (!isfinite(sgi_kona_gl_float(z->factors[0])) ||
             !isfinite(sgi_kona_gl_float(z->factors[1])))) {
            return -EINVAL;
        }
    }
    return 0;
}

/* Sent once the context has had IRIS GL's rule words or a colour. */
static bool sgi_kona_gl_iris_needed(void *opaque)
{
    SGIKonaGLState *state = opaque;

    return state->iris.colour_normal || state->iris.lmc ||
           state->iris.colour_last;
}

static const VMStateDescription vmstate_sgi_kona_gl_iris = {
    .name = TYPE_SGI_KONA "/logical-state/iris-rule",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_gl_iris_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(iris.colour_normal, SGIKonaGLState),
        VMSTATE_UINT32(iris.lmc, SGIKonaGLState),
        VMSTATE_BOOL(iris.colour_last, SGIKonaGLState),
        VMSTATE_END_OF_LIST()
    },
};

/* Only while the context has a texture load set aside. */
static bool sgi_kona_gl_parked_upload_needed(void *opaque)
{
    return sgi_kona_texture_upload_parkable(
        &((SGIKonaGLState *)opaque)->parked_upload);
}

static int sgi_kona_gl_parked_upload_post_load(void *opaque, int version_id)
{
    const SGIKonaTextureUpload *u = &((SGIKonaGLState *)opaque)->parked_upload;

    return sgi_kona_texture_upload_parkable(u) ? 0 : -EINVAL;
}

static const VMStateDescription vmstate_sgi_kona_gl_parked_upload = {
    .name = TYPE_SGI_KONA "/logical-state/parked-texture-load",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_gl_parked_upload_needed,
    .post_load = sgi_kona_gl_parked_upload_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SINGLE(parked_upload, SGIKonaGLState, 1,
                       vmstate_sgi_kona_texture_upload, SGIKonaTextureUpload),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_gl_texture_regs_needed(void *opaque)
{
    return ((SGIKonaGLState *)opaque)->texture_regs_saved;
}

static int sgi_kona_gl_texture_regs_post_load(void *opaque, int version_id)
{
    const SGIKonaTextureRegs *r = &((SGIKonaGLState *)opaque)->texture_regs;

    return (!r->associate_pending || r->associate_slot < 2) &&
           (!r->record_valid ||
            r->record_token == SGI_KONA_CMD_GL_TEX_BIND_2D ||
            r->record_token == SGI_KONA_CMD_GL_TEX_BIND_VOLUME ||
            r->record_token == SGI_KONA_CMD_GL_TEX_BIND_VOLUME_SHORT) ?
           0 : -EINVAL;
}

static const VMStateDescription vmstate_sgi_kona_gl_texture_regs = {
    .name = TYPE_SGI_KONA "/logical-state/texture-binding",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_gl_texture_regs_needed,
    .post_load = sgi_kona_gl_texture_regs_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(texture_regs_saved, SGIKonaGLState),
        VMSTATE_UINT32_ARRAY(texture_regs.bind, SGIKonaGLState, 5),
        VMSTATE_UINT32_ARRAY(texture_regs.record, SGIKonaGLState, 22),
        VMSTATE_UINT32(texture_regs.record_token, SGIKonaGLState),
        VMSTATE_UINT32_ARRAY(texture_regs.transform, SGIKonaGLState, 8),
        VMSTATE_BOOL(texture_regs.bind_valid, SGIKonaGLState),
        VMSTATE_BOOL(texture_regs.record_valid, SGIKonaGLState),
        VMSTATE_BOOL(texture_regs.bind_pending, SGIKonaGLState),
        VMSTATE_BOOL(texture_regs.associate_pending, SGIKonaGLState),
        VMSTATE_UINT8(texture_regs.associate_slot, SGIKonaGLState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_filter = {
    .name = TYPE_SGI_KONA "/logical-state/imaging/filter",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(format, SGIKonaGLFilter),
        VMSTATE_UINT32(width, SGIKonaGLFilter),
        VMSTATE_UINT32(height, SGIKonaGLFilter),
        VMSTATE_UINT32_2DARRAY(values, SGIKonaGLFilter,
                               SGI_KONA_GL_FILTER_MAX * SGI_KONA_GL_FILTER_MAX,
                               4),
        VMSTATE_UINT32_ARRAY(scale, SGIKonaGLFilter, 4),
        VMSTATE_UINT32_ARRAY(bias, SGIKonaGLFilter, 4),
        VMSTATE_END_OF_LIST()
    },
};

/* Only once a context has used the imaging subset. */
static bool sgi_kona_gl_imaging_needed(void *opaque)
{
    const SGIKonaGLState *st = opaque;
    SGIKonaGLImaging defaults;

    sgi_kona_gl_imaging_init(&defaults);
    if (memcmp(&st->imaging, &defaults, sizeof(defaults))) {
        return true;
    }
    for (unsigned k = 0; k < SGI_KONA_GL_ATTRIB_DEPTH; k++) {
        if (st->imaging_enabled_attrib[k]) {
            return true;
        }
    }
    return false;
}

static int sgi_kona_gl_imaging_post_load(void *opaque, int version_id)
{
    const SGIKonaGLImaging *im = &((SGIKonaGLState *)opaque)->imaging;
    uint32_t token = im->pending_token;

    for (unsigned f = 0; f < SGI_KONA_GL_FILTERS; f++) {
        const SGIKonaGLFilter *filter = &im->filters[f];

        if (filter->format > 5 || filter->width > SGI_KONA_GL_FILTER_MAX ||
            filter->height > SGI_KONA_GL_FILTER_MAX ||
            (f == 0 && filter->height > 1) ||
            !filter->width != !filter->height) {
            return -EINVAL;
        }
    }
    if (token && token != SGI_KONA_CMD_GL_CONVOLUTION_1D &&
        token != SGI_KONA_CMD_GL_CONVOLUTION_2D &&
        token != SGI_KONA_CMD_GL_SEPARABLE) {
        return -EINVAL;
    }
    if (token) {
        uint32_t h[8];
        unsigned filter;

        sgi_kona_gl_filter_header(token, im->pending, &filter, h);
        if (!h[0] || !h[1] || h[0] > SGI_KONA_GL_FILTER_MAX ||
            h[1] > SGI_KONA_GL_FILTER_MAX || im->pending[0] > 5 ||
            h[5] > 1 || h[6] > 1) {
            return -EINVAL;
        }
    }
    for (unsigned k = 0; k < SGI_KONA_GL_ATTRIB_DEPTH; k++) {
        if (((SGIKonaGLState *)opaque)->imaging_enabled_attrib[k] > 0x1ff) {
            return -EINVAL;
        }
    }
    return im->enabled > 7 || im->stages > 0x3f ||
           im->colour_depth > SGI_KONA_GL_COLOUR_STACK - 1 ? -EINVAL : 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_imaging = {
    .name = TYPE_SGI_KONA "/logical-state/imaging",
    .version_id = 2,
    .minimum_version_id = 2,
    .needed = sgi_kona_gl_imaging_needed,
    .post_load = sgi_kona_gl_imaging_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(imaging.filters, SGIKonaGLState,
                             SGI_KONA_GL_FILTERS, 0,
                             vmstate_sgi_kona_gl_filter, SGIKonaGLFilter),
        VMSTATE_UINT32(imaging.enabled, SGIKonaGLState),
        VMSTATE_UINT32(imaging.pending_token, SGIKonaGLState),
        VMSTATE_UINT32_ARRAY(imaging.pending, SGIKonaGLState, 9),
        VMSTATE_UINT32_ARRAY(imaging.colour_matrix, SGIKonaGLState, 16),
        VMSTATE_UINT32_2DARRAY(imaging.colour_stack, SGIKonaGLState,
                               SGI_KONA_GL_COLOUR_STACK - 1, 16),
        VMSTATE_UINT32(imaging.colour_depth, SGIKonaGLState),
        VMSTATE_UINT32(imaging.stages, SGIKonaGLState),
        VMSTATE_UINT32_ARRAY(imaging_enabled_attrib, SGIKonaGLState,
                             SGI_KONA_GL_ATTRIB_DEPTH),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_colour_table = {
    .name = TYPE_SGI_KONA "/colour-table",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(format, SGIKonaGLColourTable),
        VMSTATE_UINT32(width, SGIKonaGLColourTable),
        VMSTATE_UINT32_ARRAY(scale, SGIKonaGLColourTable, 4),
        VMSTATE_UINT32_ARRAY(bias, SGIKonaGLColourTable, 4),
        VMSTATE_UINT32_2DARRAY(values, SGIKonaGLColourTable,
                               SGI_KONA_GL_TABLE_MAX, 4),
        VMSTATE_END_OF_LIST()
    },
};

/* Only once a context has used SGI_color_table. */
static bool sgi_kona_gl_tables_needed(void *opaque)
{
    const SGIKonaGLState *st = opaque;
    g_autofree SGIKonaGLColourTables *defaults =
        g_new(SGIKonaGLColourTables, 1);

    sgi_kona_gl_tables_init(defaults);
    return memcmp(&st->tables, defaults, sizeof(*defaults)) != 0;
}

static int sgi_kona_gl_tables_post_load(void *opaque, int version_id)
{
    const SGIKonaGLColourTables *tb = &((SGIKonaGLState *)opaque)->tables;
    bool proxy;

    for (unsigned n = 0; n < SGI_KONA_GL_TABLES; n++) {
        if (tb->table[n].width > SGI_KONA_GL_TABLE_MAX ||
            (tb->table[n].width &&
             !sgi_kona_gl_table_base(tb->table[n].format))) {
            return -EINVAL;
        }
    }
    if (tb->staged_have > SGI_KONA_GL_TABLE_WORDS) {
        return -EINVAL;
    }
    if (tb->pending[0] &&
        (sgi_kona_gl_table_index(tb->pending[0], &proxy) < 0 || proxy ||
         !tb->pending[2] || tb->pending[2] > SGI_KONA_GL_TABLE_MAX ||
         tb->pending[7] > 1 || tb->pending[8] > 1)) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_tables = {
    .name = TYPE_SGI_KONA "/logical-state/colour-tables",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_gl_tables_needed,
    .post_load = sgi_kona_gl_tables_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(tables.table, SGIKonaGLState, SGI_KONA_GL_TABLES,
                             0, vmstate_sgi_kona_gl_colour_table,
                             SGIKonaGLColourTable),
        VMSTATE_UINT32_ARRAY(tables.proxy_format, SGIKonaGLState,
                             SGI_KONA_GL_TABLES),
        VMSTATE_UINT32_ARRAY(tables.proxy_width, SGIKonaGLState,
                             SGI_KONA_GL_TABLES),
        VMSTATE_UINT32_ARRAY(tables.pending, SGIKonaGLState, 9),
        VMSTATE_UINT32(tables.staged_have, SGIKonaGLState),
        VMSTATE_UINT32_ARRAY(tables.staged, SGIKonaGLState,
                             SGI_KONA_GL_TABLE_WORDS),
        VMSTATE_END_OF_LIST()
    },
};

/* Only once a context has defined a histogram or minmax table. */
static bool sgi_kona_gl_histogram_needed(void *opaque)
{
    const SGIKonaGLState *st = opaque;
    g_autofree SGIKonaGLHistogram *defaults = g_new(SGIKonaGLHistogram, 1);

    sgi_kona_gl_histogram_init(defaults);
    return memcmp(&st->histogram, defaults, sizeof(*defaults)) != 0;
}

static int sgi_kona_gl_histogram_post_load(void *opaque, int version_id)
{
    const SGIKonaGLHistogram *h = &((SGIKonaGLState *)opaque)->histogram;
    unsigned c[4];

    if (h->width > SGI_KONA_GL_HISTOGRAM_MAX ||
        (h->width && !sgi_kona_gl_histogram_slots(h->format, c)) ||
        (h->minmax_format &&
         !sgi_kona_gl_histogram_slots(h->minmax_format, c))) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_histogram = {
    .name = TYPE_SGI_KONA "/logical-state/histogram",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_gl_histogram_needed,
    .post_load = sgi_kona_gl_histogram_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(histogram.format, SGIKonaGLState),
        VMSTATE_UINT32(histogram.width, SGIKonaGLState),
        VMSTATE_BOOL(histogram.sink, SGIKonaGLState),
        VMSTATE_UINT32(histogram.proxy_format, SGIKonaGLState),
        VMSTATE_UINT32(histogram.proxy_width, SGIKonaGLState),
        VMSTATE_UINT32_2DARRAY(histogram.counts, SGIKonaGLState,
                               SGI_KONA_GL_HISTOGRAM_MAX, 4),
        VMSTATE_UINT32(histogram.minmax_format, SGIKonaGLState),
        VMSTATE_BOOL(histogram.minmax_sink, SGIKonaGLState),
        VMSTATE_UINT32_ARRAY(histogram.min, SGIKonaGLState, 4),
        VMSTATE_UINT32_ARRAY(histogram.max, SGIKonaGLState, 4),
        VMSTATE_END_OF_LIST()
    },
};

/* Only once a context has left GL_RENDER, named a feedback type or a name. */
static bool sgi_kona_gl_render_needed(void *opaque)
{
    const SGIKonaGLRenderMode *r = &((SGIKonaGLState *)opaque)->render;

    return r->mode != SGI_KONA_GL_RENDER || r->feedback_type ||
           r->name_count || r->hit || r->next_tag;
}

static bool sgi_kona_gl_render_mode_valid(uint32_t mode)
{
    return mode == SGI_KONA_GL_RENDER || mode == SGI_KONA_GL_FEEDBACK ||
           mode == SGI_KONA_GL_SELECT;
}

static int sgi_kona_gl_render_post_load(void *opaque, int version_id)
{
    const SGIKonaGLRenderMode *r = &((SGIKonaGLState *)opaque)->render;

    if (!sgi_kona_gl_render_mode_valid(r->mode) ||
        r->name_count > SGI_KONA_GL_NAME_STACK_DEPTH ||
        (r->hit && r->hit_min > r->hit_max) ||
        (r->feedback_type &&
         (r->feedback_type < 0x600 || r->feedback_type > 0x604))) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_render = {
    .name = TYPE_SGI_KONA "/logical-state/render-mode",
    .version_id = 2,
    .minimum_version_id = 2,
    .needed = sgi_kona_gl_render_needed,
    .post_load = sgi_kona_gl_render_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(render.mode, SGIKonaGLState),
        VMSTATE_UINT32(render.feedback_type, SGIKonaGLState),
        VMSTATE_UINT32(render.name_count, SGIKonaGLState),
        VMSTATE_UINT32_ARRAY(render.names, SGIKonaGLState,
                             SGI_KONA_GL_NAME_STACK_DEPTH),
        VMSTATE_BOOL(render.hit, SGIKonaGLState),
        VMSTATE_UINT32(render.hit_min, SGIKonaGLState),
        VMSTATE_UINT32(render.hit_max, SGIKonaGLState),
        VMSTATE_UINT32(render.next_tag, SGIKonaGLState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_state = {
    .name = TYPE_SGI_KONA "/logical-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sgi_kona_gl_state_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(gl, SGIKonaGLState, 0,
                       vmstate_sgi_gl_context, SGIGLContext),
        VMSTATE_STRUCT(window, SGIKonaGLState, 0,
                       vmstate_sgi_kona_gl_window, SGIKonaGLWindow),
        VMSTATE_STRUCT(pipe, SGIKonaGLState, 0,
                       vmstate_sgi_kona_gl_pipe, SGIKonaGLPipe),
        VMSTATE_STRUCT(pipe, SGIKonaGLState, 0,
                       vmstate_sgi_kona_gl_unpack, SGIKonaGLPipe),
        VMSTATE_STRUCT(pipe, SGIKonaGLState, 0,
                       vmstate_sgi_kona_gl_error, SGIKonaGLPipe),
        VMSTATE_STRUCT(pipe, SGIKonaGLState, 0,
                       vmstate_sgi_kona_gl_pixels, SGIKonaGLPipe),
        VMSTATE_STRUCT(pipe, SGIKonaGLState, 0,
                       vmstate_sgi_kona_gl_zoom, SGIKonaGLPipe),
        VMSTATE_STRUCT(pipe, SGIKonaGLState, 0,
                       vmstate_sgi_kona_gl_transfer_pipe, SGIKonaGLPipe),
        VMSTATE_STRUCT(pipe, SGIKonaGLState, 0,
                       vmstate_sgi_kona_gl_bitmap_unpack, SGIKonaGLPipe),
        VMSTATE_STRUCT(pipe, SGIKonaGLState, 0,
                       vmstate_sgi_kona_gl_pack, SGIKonaGLPipe),
        VMSTATE_STRUCT(pipe, SGIKonaGLState, 0,
                       vmstate_sgi_kona_gl_bitmap_pack, SGIKonaGLPipe),
        VMSTATE_UINT32(attrib_depth, SGIKonaGLState),
        VMSTATE_UINT32_ARRAY(attrib_mask, SGIKonaGLState,
                             SGI_KONA_GL_ATTRIB_DEPTH),
        VMSTATE_STRUCT_ARRAY(attrib, SGIKonaGLState,
                             SGI_KONA_GL_ATTRIB_DEPTH, 0,
                             vmstate_sgi_gl_context, SGIGLContext),
        VMSTATE_STRUCT_ARRAY(return_attrib, SGIKonaGLState,
                             SGI_KONA_GL_ATTRIB_DEPTH, 0,
                             vmstate_sgi_kona_gl_returns, SGIKonaGLReturnState),
        VMSTATE_STRUCT_ARRAY(zoom_attrib, SGIKonaGLState,
                             SGI_KONA_GL_ATTRIB_DEPTH, 0,
                             vmstate_sgi_kona_gl_zoom_attrib,
                             SGIKonaGLPixelZoom),
        VMSTATE_STRUCT_ARRAY(transfer_attrib, SGIKonaGLState,
                             SGI_KONA_GL_ATTRIB_DEPTH, 0,
                             vmstate_sgi_kona_gl_transfer,
                             SGIKonaGLPixelTransfer),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_kona_gl_iris,
        &vmstate_sgi_kona_gl_parked_upload,
        &vmstate_sgi_kona_gl_texture_regs,
        &vmstate_sgi_kona_gl_imaging,
        &vmstate_sgi_kona_gl_tables,
        &vmstate_sgi_kona_gl_histogram,
        &vmstate_sgi_kona_gl_render,
        NULL
    },
};

static bool sgi_kona_logical_records_save(QEMUFile *f, void *pv, size_t size,
                                         const VMStateField *field,
                                         JSONWriter *vmdesc, Error **errp)
{
    SGIKonaState *s = pv;
    bool transient = s->state[SGI_KONA_CONTEXT_TRANSIENT] != NULL;
    int ret;

    qemu_put_be32(f, s->context_count);
    qemu_put_byte(f, transient);
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        if (i >= s->context_count &&
            (i != SGI_KONA_CONTEXT_TRANSIENT || !transient)) {
            continue;
        }
        if (!vmstate_save_vmsd(f, &vmstate_sgi_kona_context,
                               &s->contexts[i], NULL, errp) ||
            !vmstate_save_vmsd(f, &vmstate_sgi_kona_gl_state,
                               s->state[i], NULL, errp)) {
            return false;
        }
    }
    ret = qemu_file_get_error(f);
    if (ret) {
        error_setg_errno(errp, -ret, "KONA logical context save failed");
        return false;
    }
    return true;
}

static bool sgi_kona_logical_records_load(QEMUFile *f, void *pv, size_t size,
                                         const VMStateField *field,
                                         Error **errp)
{
    SGIKonaState *s = pv;
    uint32_t count = qemu_get_be32(f);
    unsigned transient = qemu_get_byte(f);

    if (count > SGI_KONA_CONTEXT_LIMIT || transient > 1) {
        error_setg(errp, "KONA logical context count is invalid");
        return false;
    }
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        SGIKonaContext *c = &s->contexts[i];

        if (i >= count && (i != SGI_KONA_CONTEXT_TRANSIENT || !transient)) {
            continue;
        }
        s->state[i] = sgi_kona_state_new(s, i);
        if (!s->state[i]) {
            error_setg(errp, "KONA logical context load allocation failed");
            goto failed;
        }
        if (!vmstate_load_vmsd(f, &vmstate_sgi_kona_context, c, 1, errp) ||
            !c->valid || !c->generation || c->generation > s->context_clock ||
            c->save_offset > 0xfff ||
            (i == SGI_KONA_CONTEXT_TRANSIENT && c->return_page_valid) ||
            !vmstate_load_vmsd(f, &vmstate_sgi_kona_gl_state,
                               s->state[i], 1, errp)) {
            if (!errp || !*errp) {
                error_setg(errp, "KONA logical context metadata is invalid");
            }
            goto failed;
        }
        for (unsigned j = 0; j < i && j < count; j++) {
            const SGIKonaContext *other = &s->contexts[j];

            if (i != SGI_KONA_CONTEXT_TRANSIENT &&
                ((c->save_page == other->save_page &&
                  c->save_offset == other->save_offset) ||
                 c->generation == other->generation)) {
                error_setg(errp, "KONA duplicate logical context key "
                           "or generation");
                goto failed;
            }
        }
    }
    if (qemu_file_get_error(f)) {
        error_setg(errp, "KONA logical context stream is truncated");
        goto failed;
    }
    s->context_count = count;
    return true;

failed:
    sgi_kona_contexts_clear(s);
    return false;
}

static const VMStateInfo vmstate_sgi_kona_logical_records = {
    .name = "sgi-kona-logical-records",
    .save = sgi_kona_logical_records_save,
    .load = sgi_kona_logical_records_load,
};

/*
 * Which queued mailbox words are DMA completions, which a cause-bit
 * write consumes. A source without the subsection had none queued that
 * way, or held them as ordinary words.
 */
static bool sgi_kona_mailbox_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->mailbox_completions != 0;
}

static int sgi_kona_mailbox_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    return s->mailbox_completions >> SGI_KONA_MAILBOX_MAX_WORDS ? -EINVAL :
                                                                 0;
}

static const VMStateDescription vmstate_sgi_kona_mailbox = {
    .name = TYPE_SGI_KONA "/mailbox-completions",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_mailbox_needed,
    .post_load = sgi_kona_mailbox_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mailbox_completions, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_cursor_needed(void *opaque)
{
    return true;
}

static int sgi_kona_cursor_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    s->cursor_state_loaded = true;
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_cursor = {
    .name = TYPE_SGI_KONA "/loaded-cursor",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_cursor_needed,
    .post_load = sgi_kona_cursor_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cursor_stage_words, SGIKonaState),
        VMSTATE_UINT32(cursor_stage_entry, SGIKonaState),
        VMSTATE_UINT32_ARRAY(cursor_stage, SGIKonaState, 260),
        VMSTATE_UINT32_ARRAY(cursor_images, SGIKonaState, 64 * 260),
        VMSTATE_UINT8_ARRAY(cursor_valid, SGIKonaState, 64),
        VMSTATE_UINT32(cursor_selector, SGIKonaState),
        VMSTATE_UINT32(cursor_channel, SGIKonaState),
        VMSTATE_UINT32(cursor_offset, SGIKonaState),
        VMSTATE_UINT32(cursor_position, SGIKonaState),
        VMSTATE_UINT32(cursor_mode, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_gl_read_dma_valid(const SGIKonaGLReadDMA *d)
{
    uint64_t io, pitch;
    uint32_t lines;

    if (!sgi_kona_gl_render_mode_valid(d->render_mode) ||
        d->written % 4 || d->written > d->descriptor[2] ||
        ((d->written || d->overflow) &&
         d->render_mode != SGI_KONA_GL_SELECT)) {
        return false;
    }
    if (d->refused) {
        return !d->count && !d->selector;
    }
    if (d->context < 0 || d->context >= SGI_KONA_CONTEXTS ||
        !sgi_kona_gl_read_dma_descriptor(d, &io, &pitch, &lines)) {
        return false;
    }
    return d->count ? (d->count == d->descriptor[2] &&
                      sgi_kona_gl_read_dma_layout(d, d->header, d->selector)) :
                     !d->selector;
}

static bool sgi_kona_gl_read_dma_save(QEMUFile *f, void *pv, size_t size,
                                      const VMStateField *field,
                                      JSONWriter *vmdesc, Error **errp)
{
    SGIKonaGLReadDMA *d = pv;

    for (unsigned k = 0; k < 6; k++) {
        qemu_put_be32(f, d->descriptor[k]);
    }
    for (unsigned k = 0; k < 7; k++) {
        qemu_put_be32(f, d->header[k]);
    }
    qemu_put_be32(f, d->selector);
    qemu_put_be32(f, d->context);
    qemu_put_be64(f, d->generation);
    qemu_put_byte(f, d->refused);
    qemu_put_be32(f, d->render_mode);
    qemu_put_be32(f, d->written);
    qemu_put_byte(f, d->overflow);
    qemu_put_be32(f, d->count);
    if (d->count) {
        qemu_put_buffer(f, d->data, d->count);
    }
    if (qemu_file_get_error(f)) {
        error_setg(errp, "KONA read DMA could not be saved");
        return false;
    }
    return true;
}

static bool sgi_kona_gl_read_dma_load(QEMUFile *f, void *pv, size_t size,
                                      const VMStateField *field, Error **errp)
{
    SGIKonaGLReadDMA *d = pv, tmp = { .active = true };
    unsigned refused, overflow;

    for (unsigned k = 0; k < 6; k++) {
        tmp.descriptor[k] = qemu_get_be32(f);
    }
    for (unsigned k = 0; k < 7; k++) {
        tmp.header[k] = qemu_get_be32(f);
    }
    tmp.selector = qemu_get_be32(f);
    tmp.context = qemu_get_be32(f);
    tmp.generation = qemu_get_be64(f);
    refused = qemu_get_byte(f);
    tmp.refused = refused;
    tmp.render_mode = qemu_get_be32(f);
    tmp.written = qemu_get_be32(f);
    overflow = qemu_get_byte(f);
    tmp.overflow = overflow;
    tmp.count = qemu_get_be32(f);
    if (refused > 1 || overflow > 1 || !sgi_kona_gl_read_dma_valid(&tmp) ||
        qemu_file_get_error(f)) {
        goto invalid;
    }
    if (tmp.count) {
        tmp.data = g_malloc(tmp.count);
        if (qemu_get_buffer(f, tmp.data, tmp.count) != tmp.count ||
            qemu_file_get_error(f)) {
            goto invalid;
        }
    }
    g_free(d->data);
    *d = tmp;
    return true;

invalid:
    g_free(tmp.data);
    error_setg(errp, "KONA read DMA has invalid descriptor or staged data");
    return false;
}

static const VMStateInfo vmstate_sgi_kona_gl_read_dma_data = {
    .name = "sgi-kona-gl-read-dma-data",
    .load = sgi_kona_gl_read_dma_load,
    .save = sgi_kona_gl_read_dma_save,
};

static bool sgi_kona_gl_read_dma_needed(void *opaque)
{
    return ((SGIKonaState *)opaque)->gl_read_dma.active;
}

static int sgi_kona_gl_read_dma_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;
    const SGIKonaGLReadDMA *d = &s->gl_read_dma;

    return !sgi_kona_gl_read_dma_valid(d) ||
           (!d->refused && (d->context != s->context_current ||
        !sgi_kona_context_owner(s, d->context, d->generation) ||
                            !s->contexts[d->context].valid)) ? -EINVAL : 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_read_dma = {
    .name = TYPE_SGI_KONA "/gl-read-dma",
    .version_id = 3,
    .minimum_version_id = 3,
    .needed = sgi_kona_gl_read_dma_needed,
    .post_load = sgi_kona_gl_read_dma_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SINGLE(gl_read_dma, SGIKonaState, 0,
                        vmstate_sgi_kona_gl_read_dma_data, SGIKonaGLReadDMA),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_x_read_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->x_read_gc_valid || s->x_read_setup_valid;
}

static int sgi_kona_x_read_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    return sgi_kona_x_read_needed(s) && !sgi_kona_x_read_current(s) ?
           -EINVAL : 0;
}

/* An absent subsection leaves both flags at their reset defaults. */
static const VMStateDescription vmstate_sgi_kona_x_read = {
    .name = TYPE_SGI_KONA "/x-read-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_x_read_needed,
    .post_load = sgi_kona_x_read_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(x_read_gc_valid, SGIKonaState),
        VMSTATE_BOOL(x_read_setup_valid, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_texture_upload_valid(const SGIKonaTextureUpload *u)
{
    const uint32_t *h = u->header;
    static const uint32_t empty[20];
    uint64_t row_bytes = sgi_kona_texture_row_bytes(u);
    bool bitmap = sgi_kona_texture_bitmap(h);

    if (u->bitmap_skip > 7 || u->bitmap_phase > 3 ||
        (bitmap ?
         (!u->count || u->draw_pixels || (!u->pio_layout && !u->dma_layout) ||
          (!h[7] && u->bitmap_skip) ||
          (u->pio_layout && u->bitmap_phase != u->pio_phase) ||
          (u->dma_layout && u->dma_phase != (h[7] ? u->bitmap_phase : 0))) :
         (u->bitmap_lsb || u->bitmap_skip || u->bitmap_phase))) {
        return false;
    }

    if (!u->draw_pixels && !u->pio_layout && !h[6] && h[7]) {
        return false;
    }
    if (u->pio_layout ?
        (sgi_kona_texture_words(u) >
             SGI_KONA_TEXTURE_BYTES / sizeof(uint32_t) || !u->count ||
         u->draw_pixels || h[6] || u->pio_phase > 3 ||
         (!h[7] && (u->source_stride < row_bytes ||
                    u->source_stride - row_bytes >= 4))) :
        u->pio_phase != 0) {
        return false;
    }
    if (u->dma_layout ?
        (!u->count || u->draw_pixels || u->pio_layout || h[6] != 1 ||
         u->dma_phase > 3 || h[8] ||
         (!h[7] && (u->source_stride < row_bytes ||
                    u->source_stride - row_bytes >= 4))) :
        u->dma_phase != 0) {
        return false;
    }
    if (u->transfer_captured && h[3] == 13 &&
        (!isfinite(sgi_kona_gl_float(u->transfer.depth_scale)) ||
         !isfinite(sgi_kona_gl_float(u->transfer.depth_bias)))) {
        return false;
    }
    if (!u->transfer_captured &&
        (u->transfer.depth_scale || u->transfer.depth_bias)) {
        return false;
    }
    if (u->transfer_captured && (!u->count || u->draw_pixels)) {
        return false;
    }
    if (!u->transfer_captured &&
        (u->transfer.index_shift || u->transfer.index_offset)) {
        return false;
    }
    if (u->transfer.map_colour > 1 ||
        (!u->transfer_captured && u->transfer.map_colour)) {
        return false;
    }
    for (unsigned k = 0; k < 4; k++) {
        if (!sgi_kona_gl_pixel_map_valid(&u->transfer.colour_rgba[k],
                                         true, false) ||
            !sgi_kona_gl_pixel_map_valid(&u->transfer.index_rgba[k],
                                         true, true) ||
            (!u->transfer_captured && (u->transfer.colour_rgba[k].count ||
                                       u->transfer.index_rgba[k].count))) {
            return false;
        }
        if (u->transfer_captured ?
            (h[3] != 11 && h[3] != 13 &&
             (!isfinite(sgi_kona_gl_float(u->transfer.scale[k])) ||
              !isfinite(sgi_kona_gl_float(u->transfer.bias[k])))) :
            (u->transfer.scale[k] || u->transfer.bias[k])) {
            return false;
        }
    }
    if (!u->count) {
        return !u->complete && !u->source_stride && !u->swap_bytes &&
               !u->draw_pixels && !u->draw_context &&
               !memcmp(h, empty, sizeof(empty));
    }
    return h[1] && h[2] &&
           (u->draw_pixels ? (h[3] == 5 && h[4] == 1 && h[6] <= 1 &&
                             h[7] <= 1 && !u->complete &&
                             u->draw_context < SGI_KONA_CONTEXTS) :
                            sgi_kona_texture_native_header(h)) &&
           h[1] <= SGI_KONA_GL_IMAGE_MAX && h[2] <= SGI_KONA_GL_IMAGE_MAX &&
           sgi_kona_texture_source_valid(h) && h[5] < 4 &&
           u->source_stride &&
           (!h[6] || u->source_stride >= row_bytes) &&
           (uint64_t)h[1] * h[2] == u->count &&
           (uint64_t)u->count * sgi_kona_texture_source_words(u) <=
           SGI_KONA_TEXTURE_BYTES / sizeof(uint32_t);
}

/*
 * The record's own marker and layout. Migration is between identical
 * builds only, so the loader takes this layout alone.
 */
#define SGI_KONA_TEXTURE_UPLOAD_STREAM_VERSION 3

static bool sgi_kona_texture_upload_save(QEMUFile *f, void *pv, size_t size,
                                         const VMStateField *field,
                                         JSONWriter *vmdesc, Error **errp)
{
    SGIKonaTextureUpload *u = pv;

    qemu_put_be32(f, UINT32_MAX);
    qemu_put_be32(f, SGI_KONA_TEXTURE_UPLOAD_STREAM_VERSION);
    for (unsigned k = 0; k < 20; k++) {
        qemu_put_be32(f, u->header[k]);
    }
    qemu_put_be32(f, u->count);
    qemu_put_be32(f, u->source_stride);
    qemu_put_byte(f, u->complete);
    qemu_put_byte(f, u->swap_bytes);
    qemu_put_byte(f, u->draw_pixels);
    qemu_put_be32(f, u->draw_context);
    qemu_put_byte(f, u->transfer_captured);
    for (unsigned k = 0; k < 4; k++) {
        qemu_put_be32(f, u->transfer.scale[k]);
        qemu_put_be32(f, u->transfer.bias[k]);
    }
    qemu_put_byte(f, u->pio_layout);
    qemu_put_be32(f, u->pio_phase);
    qemu_put_byte(f, u->dma_layout);
    qemu_put_be32(f, u->dma_phase);
    qemu_put_byte(f, u->transfer.map_colour);
    for (unsigned k = 0; k < 4; k++) {
        const SGIKonaGLPixelMap *map = &u->transfer.colour_rgba[k];

        qemu_put_be32(f, map->count);
        for (unsigned at = 0; at < map->count; at++) {
            qemu_put_be32(f, map->values[at]);
        }
    }
    qemu_put_be32(f, u->transfer.index_shift);
    qemu_put_be32(f, u->transfer.index_offset);
    for (unsigned k = 0; k < 4; k++) {
        const SGIKonaGLPixelMap *map = &u->transfer.index_rgba[k];

        qemu_put_be32(f, map->count);
        for (unsigned at = 0; at < map->count; at++) {
            qemu_put_be32(f, map->values[at]);
        }
    }
    qemu_put_byte(f, u->bitmap_lsb);
    qemu_put_be32(f, u->bitmap_skip);
    qemu_put_be32(f, u->bitmap_phase);
    qemu_put_be32(f, u->transfer.depth_scale);
    qemu_put_be32(f, u->transfer.depth_bias);
    for (unsigned k = 0; k < 4; k++) {
        qemu_put_be32(f, u->transfer.post_conv_scale[k]);
        qemu_put_be32(f, u->transfer.post_conv_bias[k]);
        qemu_put_be32(f, u->transfer.post_matrix_scale[k]);
        qemu_put_be32(f, u->transfer.post_matrix_bias[k]);
    }
    for (unsigned k = 0; k < 16; k++) {
        qemu_put_be32(f, u->colour_matrix[k]);
    }
    for (uint32_t k = 0; k < sgi_kona_texture_buffer_words(u); k++) {
        qemu_put_be32(f, u->pixels[k]);
    }
    if (qemu_file_get_error(f)) {
        error_setg(errp, "KONA texture upload could not be saved");
        return false;
    }
    return true;
}

static bool sgi_kona_texture_upload_load(QEMUFile *f, void *pv, size_t size,
                                         const VMStateField *field,
                                         Error **errp)
{
    SGIKonaTextureUpload *u = pv;
    SGIKonaTextureUpload tmp = { 0 };
    unsigned complete, swap, draw, transfer, pio, dma, bitmap_lsb;

    uint32_t marker = qemu_get_be32(f);
    uint32_t version = qemu_get_be32(f);

    if (marker != UINT32_MAX ||
        version != SGI_KONA_TEXTURE_UPLOAD_STREAM_VERSION) {
        goto invalid;
    }
    for (unsigned k = 0; k < 20; k++) {
        tmp.header[k] = qemu_get_be32(f);
    }
    tmp.count = qemu_get_be32(f);
    tmp.source_stride = qemu_get_be32(f);
    complete = qemu_get_byte(f);
    swap = qemu_get_byte(f);
    draw = qemu_get_byte(f);
    tmp.draw_pixels = draw;
    tmp.draw_context = qemu_get_be32(f);
    transfer = qemu_get_byte(f);
    tmp.transfer_captured = transfer;
    for (unsigned k = 0; k < 4; k++) {
        tmp.transfer.scale[k] = qemu_get_be32(f);
        tmp.transfer.bias[k] = qemu_get_be32(f);
    }
    pio = qemu_get_byte(f);
    tmp.pio_layout = pio;
    tmp.pio_phase = qemu_get_be32(f);
    dma = qemu_get_byte(f);
    tmp.dma_layout = dma;
    tmp.dma_phase = qemu_get_be32(f);
    tmp.transfer.map_colour = qemu_get_byte(f);
    for (unsigned k = 0; k < 4; k++) {
        SGIKonaGLPixelMap *map = &tmp.transfer.colour_rgba[k];

        map->count = qemu_get_be32(f);
        if (map->count > SGI_KONA_GL_PIXEL_MAP_MAX) {
            goto invalid;
        }
        for (unsigned at = 0; at < map->count; at++) {
            map->values[at] = qemu_get_be32(f);
        }
    }
    tmp.transfer.index_shift = qemu_get_be32(f);
    tmp.transfer.index_offset = qemu_get_be32(f);
    for (unsigned k = 0; k < 4; k++) {
        SGIKonaGLPixelMap *map = &tmp.transfer.index_rgba[k];

        map->count = qemu_get_be32(f);
        if (map->count > SGI_KONA_GL_PIXEL_MAP_MAX) {
            goto invalid;
        }
        for (unsigned at = 0; at < map->count; at++) {
            map->values[at] = qemu_get_be32(f);
        }
    }
    bitmap_lsb = qemu_get_byte(f);
    tmp.bitmap_lsb = bitmap_lsb;
    tmp.bitmap_skip = qemu_get_be32(f);
    tmp.bitmap_phase = qemu_get_be32(f);
    tmp.transfer.depth_scale = qemu_get_be32(f);
    tmp.transfer.depth_bias = qemu_get_be32(f);
    for (unsigned k = 0; k < 4; k++) {
        tmp.transfer.post_conv_scale[k] = qemu_get_be32(f);
        tmp.transfer.post_conv_bias[k] = qemu_get_be32(f);
        tmp.transfer.post_matrix_scale[k] = qemu_get_be32(f);
        tmp.transfer.post_matrix_bias[k] = qemu_get_be32(f);
    }
    for (unsigned k = 0; k < 16; k++) {
        tmp.colour_matrix[k] = qemu_get_be32(f);
    }
    tmp.complete = complete;
    tmp.swap_bytes = swap;
    if (complete > 1 || swap > 1 || draw > 1 || transfer > 1 || pio > 1 ||
        dma > 1 || bitmap_lsb > 1 ||
        !sgi_kona_texture_upload_valid(&tmp)) {
        goto invalid;
    }
    if (tmp.count) {
        tmp.pixels = g_try_new(uint32_t, sgi_kona_texture_buffer_words(&tmp));
        if (!tmp.pixels) {
            goto invalid;
        }
        for (uint32_t k = 0; k < sgi_kona_texture_buffer_words(&tmp); k++) {
            tmp.pixels[k] = qemu_get_be32(f);
        }
    }
    if (qemu_file_get_error(f)) {
        goto invalid;
    }
    g_free(u->pixels);
    *u = tmp;
    return true;

invalid:
    g_free(tmp.pixels);
    error_setg(errp, "KONA texture upload has invalid dimensions or progress");
    return false;
}

static const VMStateInfo vmstate_sgi_kona_texture_upload = {
    .name = "sgi-kona-texture-upload",
    .load = sgi_kona_texture_upload_load,
    .save = sgi_kona_texture_upload_save,
};

/*
 * A texture or DrawPixels upload as its run can leave it, for the context
 * that owns the run: the current one live, or the slot a saved run is in.
 */
static bool sgi_kona_texture_upload_run_valid(SGIKonaState *s, int owner,
                                              const SGIKonaTextureUpload *u,
                                              uint32_t token, uint32_t have,
                                              uint32_t remaining)
{
    if (u->count) {
        uint64_t words = sgi_kona_texture_words(u);

        if (u->draw_pixels &&
            (owner != u->draw_context ||
             !s->contexts[u->draw_context].valid)) {
            return false;
        }

        if (!u->complete && u->header[6]) {
            if (remaining &&
                !sgi_kona_gl_pixel_map_token(token)) {
                return false;
            }
            for (uint32_t at = 0; at < sgi_kona_texture_buffer_words(u); at++) {
                if (u->pixels[at]) {
                    return false;
                }
            }
            return true;
        }
        if (u->complete ?
            (remaining &&
             !sgi_kona_gl_pixel_map_token(token)) :
            (token != (u->draw_pixels ? SGI_KONA_CMD_GL_DRAW_PIXELS :
                                                  SGI_KONA_CMD_GL_TEX_LOAD) ||
             !remaining || have >= words ||
             remaining != words - have)) {
            return false;
        }
        if (!u->complete) {
            uint32_t width = u->header[1];
            uint32_t bytes = sgi_kona_texture_source_bytes(u->header[3],
                                                      u->header[4]);
            uint32_t consumed_row, consumed_word;
            uint32_t source_words = sgi_kona_texture_source_words(u);

            sgi_kona_texture_source_position(u, have,
                                             &consumed_row, &consumed_word);
            for (uint32_t at = 0; at < u->count; at++) {
                uint32_t row = at / width, x = at % width;
                uint32_t pad = (sgi_kona_texture_source_phase(u) +
                                (uint64_t)row * u->source_stride) & 3;

                if (sgi_kona_texture_bitmap(u->header)) {
                    uint32_t byte = (x + u->bitmap_skip) / 8;
                    uint64_t word = u->pio_layout && !u->header[7] ?
                        (u->pio_phase + (uint64_t)row * u->source_stride +
                         byte) / 4 : (pad + byte) / 4;
                    bool future = u->pio_layout && !u->header[7] ?
                        word >= have :
                        row > consumed_row || (row == consumed_row &&
                                                word >= consumed_word);

                    if (u->pixels[at] > 1 || (future && u->pixels[at])) {
                        return false;
                    }
                    continue;
                }
                for (unsigned k = 0; k < source_words * 4; k++) {
                    uint32_t word = (pad + x * bytes + k) / 4;
                    bool future = u->pio_layout && !u->header[7] ?
                                  (u->pio_phase +
                                   (uint64_t)row * u->source_stride +
                                   (uint64_t)x * bytes + k) / 4 >=
                                  have :
                                  u->draw_pixels && !u->header[7] ?
                                  (u->header[5] + (uint64_t)at * bytes + k) / 4
                                  >= have :
                                  row > consumed_row ||
                                  (row == consumed_row &&
                                   word >= consumed_word);

                    if ((k >= bytes || future) &&
                        ((u->pixels[at * source_words + k / 4] >>
                          (24 - (k % 4) * 8)) & 0xff)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

static int sgi_kona_textures_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;
    const SGIKonaTextureUpload *u = &s->texture_upload;

    if (!s->texture_state_used || !sgi_kona_texture_upload_valid(u) ||
        (s->texture_upload_context != SGI_KONA_CONTEXT_NONE &&
         (s->texture_upload_context < 0 ||
          s->texture_upload_context >= SGI_KONA_CONTEXTS)) ||
        (s->texture_regs_context != SGI_KONA_CONTEXT_NONE &&
         (s->texture_regs_context < 0 ||
          s->texture_regs_context >= SGI_KONA_CONTEXTS)) ||
        (s->texture_record_valid &&
         s->texture_record_token != SGI_KONA_CMD_GL_TEX_BIND_2D &&
         s->texture_record_token != SGI_KONA_CMD_GL_TEX_BIND_VOLUME &&
         s->texture_record_token != SGI_KONA_CMD_GL_TEX_BIND_VOLUME_SHORT) ||
        (s->texture_bind_context != SGI_KONA_CONTEXT_NONE &&
         (s->texture_bind_context < 0 ||
          s->texture_bind_context >= SGI_KONA_CONTEXTS ||
          (s->texture_bind_context != s->context_current &&
           s->texture_bind_context != s->texture_regs_context) ||
          !sgi_kona_context_owner(s, s->texture_bind_context,
                                   s->texture_bind_generation))) ||
        (s->texture_associate_pending &&
         (s->texture_associate_context < 0 ||
          s->texture_associate_context >= SGI_KONA_CONTEXTS ||
          s->texture_associate_slot >= 2 ||
          (s->texture_associate_context != s->context_current &&
           s->texture_associate_context != s->texture_regs_context) ||
          !sgi_kona_context_owner(s, s->texture_associate_context,
                                   s->texture_associate_generation)))) {
        return -EINVAL;
    }
    if (!sgi_kona_texture_upload_run_valid(s, s->context_current, u,
                                           s->gl_data_token, s->gl_data_have,
                                           s->gl_data_remaining)) {
        return -EINVAL;
    }
    return 0;
}

/* Only between 0x1c4 and the command after it. */
static bool sgi_kona_texture_retire_needed(void *opaque)
{
    return ((SGIKonaState *)opaque)->texture_retire_pending;
}

static bool sgi_kona_texture_discard_needed(void *opaque)
{
    return ((SGIKonaState *)opaque)->texture_dma_discard;
}

static bool sgi_kona_image_discard_needed(void *opaque)
{
    return ((SGIKonaState *)opaque)->image_dma_discard;
}

static const VMStateDescription vmstate_sgi_kona_image_discard = {
    .name = TYPE_SGI_KONA "/image-dma-discard",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_image_discard_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(image_dma_discard, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_texture_discard = {
    .name = TYPE_SGI_KONA "/textures/dma-discard",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_texture_discard_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(texture_dma_discard, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_texture_upload_context_needed(void *opaque)
{
    return ((SGIKonaState *)opaque)->texture_upload_context !=
           SGI_KONA_CONTEXT_NONE;
}

static const VMStateDescription vmstate_sgi_kona_texture_upload_context = {
    .name = TYPE_SGI_KONA "/textures/upload-context",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_texture_upload_context_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(texture_upload_context, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_texture_regs_context_needed(void *opaque)
{
    return ((SGIKonaState *)opaque)->texture_regs_context !=
           SGI_KONA_CONTEXT_NONE;
}

static const VMStateDescription vmstate_sgi_kona_texture_regs_context = {
    .name = TYPE_SGI_KONA "/textures/binding-context",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_texture_regs_context_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(texture_regs_context, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_texture_retire = {
    .name = TYPE_SGI_KONA "/textures/retire",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_texture_retire_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(texture_retire_pending, SGIKonaState),
        VMSTATE_UINT32(texture_retire_handle, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_kona_textures_needed(void *opaque)
{
    return ((SGIKonaState *)opaque)->texture_state_used;
}

static const VMStateDescription vmstate_sgi_kona_textures = {
    .name = TYPE_SGI_KONA "/textures",
    .version_id = 2,
    .minimum_version_id = 2,
    .needed = sgi_kona_textures_needed,
    .post_load = sgi_kona_textures_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SGI_KONA_TEXTURE(textures, SGIKonaState),
        VMSTATE_SINGLE(texture_upload, SGIKonaState, 1,
                       vmstate_sgi_kona_texture_upload, SGIKonaTextureUpload),
        VMSTATE_UINT32_ARRAY(texture_bind, SGIKonaState, 5),
        VMSTATE_UINT32_ARRAY(texture_record, SGIKonaState, 22),
        VMSTATE_UINT32_ARRAY(texture_transform, SGIKonaState, 8),
        VMSTATE_BOOL(texture_bind_valid, SGIKonaState),
        VMSTATE_BOOL(texture_record_valid, SGIKonaState),
        VMSTATE_BOOL(texture_state_used, SGIKonaState),
        VMSTATE_INT32(texture_bind_context, SGIKonaState),
        VMSTATE_UINT64(texture_bind_generation, SGIKonaState),
        VMSTATE_UINT32(texture_record_token, SGIKonaState),
        VMSTATE_BOOL(texture_associate_pending, SGIKonaState),
        VMSTATE_INT32(texture_associate_context, SGIKonaState),
        VMSTATE_UINT64(texture_associate_generation, SGIKonaState),
        VMSTATE_UINT8(texture_associate_slot, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_kona_texture_retire,
        &vmstate_sgi_kona_texture_discard,
        &vmstate_sgi_kona_texture_upload_context,
        &vmstate_sgi_kona_texture_regs_context,
        NULL
    },
};

/* A held 1 1 and the word after it, only while the decision is open. */
static bool sgi_kona_gl_level1_needed(void *opaque)
{
    return ((SGIKonaState *)opaque)->gl_level1_held != 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_level1 = {
    .name = TYPE_SGI_KONA "/gl-level1",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_gl_level1_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(gl_level1_held, SGIKonaState),
        VMSTATE_UINT32(gl_level1_arg, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

/* A display blanked by TempBlankAllChannels, only while it is blanked. */
static bool sgi_kona_video_blank_needed(void *opaque)
{
    return ((SGIKonaState *)opaque)->video_blanked;
}

static const VMStateDescription vmstate_sgi_kona_video_blank = {
    .name = TYPE_SGI_KONA "/video-blank",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_video_blank_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(video_blanked, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

/* The flash, only once something may have changed it. */
static bool sgi_kona_flash_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->flash_written;
}

static int sgi_kona_flash_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    s->flash_written = true;
    /* The destination's image takes them when the VM runs. */
    s->flash_sync_pending = s->flash_blk != NULL;
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_flash = {
    .name = TYPE_SGI_KONA "/flash-contents",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_flash_needed,
    .post_load = sgi_kona_flash_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(flash, SGIKonaState, SGI_KONA_FLASH_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

/* An EEPROM operation still taking words. */
static bool sgi_kona_flash_op_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    return s->flash_op.op;
}

static int sgi_kona_flash_op_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    return sgi_kona_flash_op_valid(&s->flash_op) ? 0 : -EINVAL;
}

static const VMStateDescription vmstate_sgi_kona_flash_op_open = {
    .name = TYPE_SGI_KONA "/flash-op",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_flash_op_needed,
    .post_load = sgi_kona_flash_op_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(flash_op, SGIKonaState, 1, vmstate_sgi_kona_flash_op,
                       SGIKonaFlashOp),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona = {
    .name = TYPE_SGI_KONA,
    .version_id = 6,
    .minimum_version_id = 6,
    .pre_save = sgi_kona_pre_save,
    .pre_load = sgi_kona_cursor_pre_load,
    .post_load = sgi_kona_post_load,
    .fields = (const VMStateField[]) {
        /* The slot, which the destination board's must match. */
        VMSTATE_STRUCT(parent_obj, SGIKonaState, 0,
                       vmstate_sgi_xio_board_slot, SGIXioBoardState),
        VMSTATE_UINT32(widget_control, SGIKonaState),
        VMSTATE_UINT32(onewire_reg, SGIKonaState),

        /* Increment 2: register-window plain storage and the mailbox queue. */
        VMSTATE_UINT32(low_window_write, SGIKonaState),
        VMSTATE_UINT32(reset_arg, SGIKonaState),
        /* SGI_KONA_INT_ENABLE (see its own header comment). */
        VMSTATE_UINT32(int_enable, SGIKonaState),
        VMSTATE_UINT32(int_cause, SGIKonaState),
        VMSTATE_UINT32_ARRAY(mailbox_queue, SGIKonaState,
                             SGI_KONA_MAILBOX_MAX_WORDS),
        VMSTATE_UINT32(mailbox_len, SGIKonaState),
        VMSTATE_UINT32(mailbox_pos, SGIKonaState),
        VMSTATE_UINT32(reset_strobe, SGIKonaState),
        VMSTATE_UINT32(eeprom_enable, SGIKonaState),
        VMSTATE_UINT32(eeprom_addr, SGIKonaState),

        /*
         * Increment 2: the textport command port's migrated fields, per
         * include/hw/misc/sgi-command-port.h's own doc comment ("the
         * owner's VMStateDescription lists these"). ops/opaque/port_id/
         * selectors/num_selectors are not listed: sgi_kona_finish_realize()
         * sets them fresh on every realize, including the one an
         * identical-build migration replays before this state loads.
         */
        VMSTATE_UINT32(textport.kick, SGIKonaState),
        VMSTATE_BOOL(textport.framing, SGIKonaState),
        VMSTATE_UINT32(textport.selector, SGIKonaState),
        VMSTATE_UINT32(textport.argc_expected, SGIKonaState),
        VMSTATE_UINT32(textport.argc_have, SGIKonaState),
        VMSTATE_UINT32_ARRAY(textport.args, SGIKonaState,
                             SGI_COMMAND_PORT_MAX_ARGS),

        /* Increment 2: the textport's own sync/bitmap framing state. */
        VMSTATE_BOOL(textport_expect_sync, SGIKonaState),
        VMSTATE_UINT32(draw_color, SGIKonaState),
        VMSTATE_UINT32_ARRAY(colormap, SGIKonaState, SGI_KONA_COLORMAP_SIZE),
        VMSTATE_UINT32(bitmap_state, SGIKonaState),
        VMSTATE_UINT32_ARRAY(bitmap_header, SGIKonaState,
                             SGI_KONA_BITMAP_HEADER_WORDS),
        VMSTATE_UINT32(bitmap_header_have, SGIKonaState),
        VMSTATE_UINT32(bitmap_width, SGIKonaState),
        VMSTATE_UINT32(bitmap_height, SGIKonaState),
        VMSTATE_UINT32(bitmap_payload_have, SGIKonaState),
        VMSTATE_UINT32(bitmap_payload_expected, SGIKonaState),
        VMSTATE_VALIDATE("bitmap payload within bound",
                         sgi_kona_bitmap_count_valid),
        /*
         * A migration mid-blit must carry the in-flight payload itself,
         * not just its counters: matches hw/misc/sgi-mgras.c's own
         * ge[]/front/back precedent of migrating bulk transfer buffers,
         * not only their lengths. bitmap_payload_expected (already listed
         * above) sizes the allocation on load; outside a transfer it is
         * 0 and this reads/writes as an empty array regardless of the
         * pointer's NULL/non-NULL state.
         */
        VMSTATE_VARRAY_UINT32_ALLOC(bitmap_payload, SGIKonaState,
                                   bitmap_payload_expected, 0,
                                   vmstate_info_uint32, uint32_t),
        /*
         * The raster position (see the struct field's own comment):
         * persists across bitmap_state/bitmap_header/etc, which the
         * fields above already cover, so it is listed on its own here
         * rather than beside them.
         */
        VMSTATE_INT32(raster_x, SGIKonaState),
        VMSTATE_INT32(raster_y, SGIKonaState),

        /*
         * Stage 4: the DMA engine's FIFO command port (same field set as
         * textport's own above). The engine's own state is the
         * sgi-kona/dma subsection.
         */
        VMSTATE_UINT32(dma.fifo_port.kick, SGIKonaState),
        VMSTATE_BOOL(dma.fifo_port.framing, SGIKonaState),
        VMSTATE_UINT32(dma.fifo_port.selector, SGIKonaState),
        VMSTATE_UINT32(dma.fifo_port.argc_expected, SGIKonaState),
        VMSTATE_UINT32(dma.fifo_port.argc_have, SGIKonaState),
        VMSTATE_UINT32_ARRAY(dma.fifo_port.args, SGIKonaState,
                             SGI_COMMAND_PORT_MAX_ARGS),

        /*
         * The XG map RAM (see the header comment on SGI_KONA_MAPRAM_BASE):
         * canonical guest-visible state, migrated the same non-allocating
         * way arm_high is below (map_ram_count is fixed at
         * SGI_KONA_MAPRAM_ENTRIES, never guest-shrunk, so only its fixed
         * length needs to travel through VMSTATE_VARRAY_UINT32()).
         */
        VMSTATE_UINT32(map_ram_count, SGIKonaState),
        VMSTATE_VALIDATE("map RAM length is fixed",
                         sgi_kona_mapram_count_valid),
        VMSTATE_VARRAY_UINT32(map_ram, SGIKonaState, map_ram_count, 0,
                              vmstate_info_uint32, uint32_t),

        /*
         * The firmware load and its record (see the header comment on
         * SGI_KONA_ARM_LOW_BASE).
         */
        VMSTATE_UINT32_ARRAY(arm_low, SGIKonaState, SGI_KONA_ARM_LOW_WORDS),
        VMSTATE_UINT32(arm_high_count, SGIKonaState),
        VMSTATE_VALIDATE("ARM memory length is fixed",
                         sgi_kona_arm_high_count_valid),
        VMSTATE_VARRAY_UINT32(arm_high, SGIKonaState, arm_high_count, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32(icu_control, SGIKonaState),
        VMSTATE_BOOL(load_section_open, SGIKonaState),
        VMSTATE_UINT32(load_sections, SGIKonaState),
        VMSTATE_UINT32_ARRAY(load_section_addr, SGIKonaState,
                             SGI_KONA_LOAD_SECTIONS_MAX),
        VMSTATE_UINT32_ARRAY(load_section_words, SGIKonaState,
                             SGI_KONA_LOAD_SECTIONS_MAX),
        VMSTATE_UINT32(loader_3b0, SGIKonaState),
        VMSTATE_UINT32(loader_328, SGIKonaState),

        /*
         * The kernel command framing, the reason and return pages and the
         * interrupt destination reasons are posted to.
         */
        VMSTATE_UINT32(kernel_port.kick, SGIKonaState),
        VMSTATE_BOOL(kernel_port.framing, SGIKonaState),
        VMSTATE_UINT32(kernel_port.selector, SGIKonaState),
        VMSTATE_UINT32(kernel_port.argc_expected, SGIKonaState),
        VMSTATE_UINT32(kernel_port.argc_have, SGIKonaState),
        VMSTATE_UINT32_ARRAY(kernel_port.args, SGIKonaState,
                             SGI_COMMAND_PORT_MAX_ARGS),
        VMSTATE_UINT32(int_dest_upper, SGIKonaState),
        VMSTATE_UINT32(int_dest_lower, SGIKonaState),
        VMSTATE_UINT32(int_vectors, SGIKonaState),
        VMSTATE_BOOL(reason_page_valid, SGIKonaState),
        VMSTATE_UINT64(reason_page, SGIKonaState),
        VMSTATE_BOOL(return_page_valid, SGIKonaState),
        VMSTATE_UINT64(return_page, SGIKonaState),
        VMSTATE_BOOL(activity, SGIKonaState),

        /* A video command still collecting its words. */
        VMSTATE_BOOL(video_open, SGIKonaState),
        VMSTATE_UINT32(video_command, SGIKonaState),
        VMSTATE_UINT32(video_have, SGIKonaState),
        VMSTATE_UINT32(video_first, SGIKonaState),
        /* A colour-map load still taking its entries. */
        VMSTATE_UINT32(color_map_count, SGIKonaState),
        VMSTATE_UINT32(color_map_remaining, SGIKonaState),
        /* The gamma table and a gamma load still taking its entries. */
        VMSTATE_UINT16_ARRAY(gamma, SGIKonaState, SGI_KONA_GAMMA_ENTRIES),
        VMSTATE_UINT32(gamma_start, SGIKonaState),
        VMSTATE_UINT32(gamma_count, SGIKonaState),
        VMSTATE_UINT32(gamma_remaining, SGIKonaState),
        VMSTATE_UINT32(gamma_outside, SGIKonaState),
        /* A cursor load still taking its data words. */
        VMSTATE_UINT32(cursor_remaining, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_kona_board_memory,
        &vmstate_sgi_kona_scanout,
        &vmstate_sgi_kona_desktop,
        &vmstate_sgi_kona_x_server,
        &vmstate_sgi_kona_contexts,
        &vmstate_sgi_kona_dma,
        &vmstate_sgi_kona_credit,
        &vmstate_sgi_kona_bracket,
        &vmstate_sgi_kona_saved_input,
        &vmstate_sgi_kona_x_pixel_owner,
        &vmstate_sgi_kona_x_run_owner,
        &vmstate_sgi_kona_retrace,
        &vmstate_sgi_kona_cursor,
        &vmstate_sgi_kona_mailbox,
        &vmstate_sgi_kona_textures,
        &vmstate_sgi_kona_image_discard,
        &vmstate_sgi_kona_gl_bitmap,
        &vmstate_sgi_kona_gl_pixel_map,
        &vmstate_sgi_kona_gl_raw_bitmap,
        &vmstate_sgi_kona_gl_raw_pixels,
        &vmstate_sgi_kona_x_read,
        &vmstate_sgi_kona_gl_read_dma,
        &vmstate_sgi_kona_video_blank,
        &vmstate_sgi_kona_flash,
        &vmstate_sgi_kona_flash_op_open,
        &vmstate_sgi_kona_gl_level1,
        &vmstate_sgi_kona_managed_area,
        &vmstate_sgi_kona_channels,
        &vmstate_sgi_kona_video_data,
        &vmstate_sgi_kona_accum,
        &vmstate_sgi_kona_pool_swap,
        NULL
    },
};

static DeviceUnrealize sgi_kona_parent_unrealize;

static void sgi_kona_unrealize(DeviceState *dev)
{
    SGIKonaState *s = SGI_KONA(dev);

    if (s->flash_vm_change) {
        qemu_del_vm_change_state_handler(s->flash_vm_change);
        s->flash_vm_change = NULL;
    }
    sgi_kona_work_cleanup(SGI_KONA(dev));
    sgi_kona_renderer_stop(SGI_KONA(dev));
    sgi_kona_parent_unrealize(dev);
}

static void sgi_kona_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI KONA: InfiniteReality graphics pipe";
    dc->vmsd = &vmstate_sgi_kona;
    device_class_set_parent_unrealize(dc, sgi_kona_unrealize,
                                      &sgi_kona_parent_unrealize);
    /*
     * Ordinarily fitted by the machine at a free XIO slot, but also
     * `-device`-creatable: a late board's own "fabric" can resolve
     * against the spare fabric proxy a Crossbow machine builder holds
     * back for it, the same outbound-only attach path xio-baseio.c's own
     * class_init documents.
     */
    dc->user_creatable = true;
    device_class_set_props(dc, sgi_kona_properties);
    bc->build_widget = sgi_kona_build_widget;
    bc->finish_realize = sgi_kona_finish_realize;
    bc->identity_record = sgi_kona_identity_record;
    rc->phases.hold = sgi_kona_reset_hold;

    sgi_xio_board_class_set_identity(oc, &sgi_kona_identity);
}

static const TypeInfo sgi_kona_type = {
    .name = TYPE_SGI_KONA,
    .parent = TYPE_SGI_XIO_BOARD,
    .instance_size = sizeof(SGIKonaState),
    .instance_init = sgi_kona_instance_init,
    .instance_finalize = sgi_kona_instance_finalize,
    .class_init = sgi_kona_class_init,
};

static void sgi_kona_register_types(void)
{
    type_register_static(&sgi_kona_type);
}

type_init(sgi_kona_register_types)
