/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA: the InfiniteReality graphics pipe.
 *
 * This sgi-xio-board subclass models the widget at an Origin 2000 XIO slot.
 * It provides the widget-header and subsystem-classification registers,
 * one-wire identity records, an additional register window, and a textport
 * command port. The command port uses the shared QEMU framer and adds local
 * handling for the variable-length bitmap transfer. The native 1280x1024
 * framebuffer is shown through the shared scanout, which converts only the
 * display lines drawing marked dirty. Beyond the textport, the board
 * decodes the X server's command pages, keeps per-context state, moves
 * data by DMA (hw/misc/sgi-kona-dma.c) and runs resident display lists.
 *
 * Values beyond the widget-header and classification fields are synthetic
 * or approximated. Identity serials and several board-record fields are
 * placeholders. Where a value rests on a captured boot trace, the comment
 * at that site says so; the rest of the command and register model has no
 * real-device capture behind it. Unimplemented offsets read zero and
 * accept writes.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/1wire/ds2502.h"
#include "hw/display/sgi/rasterizer.h"
#include "hw/mips/sgi/pi-gfx.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xio-link.h"
#include "hw/mips/sgi/xio-widget.h"
#include "hw/misc/sgi-kona.h"
#include "hw/misc/sgi-kona-ports.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/qapi-visit-common.h"
#include "qapi/util.h"
#include "qapi/visitor.h"
#include "qom/object.h"
#include "system/dma.h"
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

/* Placeholder serials; these are not hardware identifiers. */
#define SGI_KONA_GEOMETRY_SERIAL 0x000000000001ULL
#define SGI_KONA_KTOWN_SERIAL    0x000000000002ULL

/*
 * These records use the shared fixed-offset board-record representation.
 * Barcode, part, revision, and name are stored fields. Group, capability,
 * and variety bytes use the unprogrammed value. The displayed serial is the
 * one-wire ROM number, not a separate stored field. Configured part numbers
 * are retained; the other identity fields are synthetic placeholders.
 */
static const SGIBridgeBoardRecord sgi_kona_geometry_record = {
    .barcode = "00000001",
    .part = "030-1398-001",
    .revision = "01",
    .name = "GEOM-BD",
};
static const SGIBridgeBoardRecord sgi_kona_ktown_record = {
    .barcode = "00000002",
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
static uint32_t sgi_kona_x_color(SGIKonaState *s, uint32_t index)
{
    uint64_t addr = (uint64_t)SGI_KONA_X_COLOR_MAP_WINDOW + index;
    uint32_t entry, rgb = 0;

    if (addr >= SGI_KONA_X_COLOR_MAP_ENTRIES) {
        return 0;
    }
    entry = s->x_color_map[addr];
    for (int shift = 20; shift >= 0; shift -= 10) {
        uint32_t c = (entry >> shift) & 0x3ff;

        rgb = (rgb << 8) | ((c * 255 + 511) / 1023);
    }
    return rgb;
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
static bool sgi_kona_gl_window_rect(const SGIKonaGLWindow *w, SGIClipRect *r)
{
    int64_t x0 = w->x, y0 = w->y;
    int64_t x1 = x0 + w->width - 1, y1 = y0 + w->height - 1;

    if (!w->placed || !w->width || !w->height) {
        return false;
    }
    x0 = MAX(x0, 0);
    y0 = MAX(y0, 0);
    x1 = MIN(x1, SGI_KONA_FB_WIDTH - 1);
    y1 = MIN(y1, SGI_KONA_FB_HEIGHT - 1);
    if (x0 > x1 || y0 > y1) {
        return false;
    }
    *r = (SGIClipRect) { x0, y0, x1, y1 };
    return true;
}

/*
 * The window of context table entry i if it shows colour bank 1, the
 * bank a double-buffered context's window shows after an odd number of
 * swaps; false otherwise.
 */
static bool sgi_kona_bank1_window(SGIKonaState *s, unsigned i,
                                  SGIClipRect *r)
{
    const SGIKonaGLWindow *w = &s->gl_window[i];

    return s->contexts[i].valid && w->double_buffered &&
           w->displayed_bank && sgi_kona_gl_window_rect(w, r);
}

/*
 * The colour bank the display shows at plane (x, y): bank 1 inside the
 * window of a double-buffered context showing it, bank 0 everywhere else.
 *
 * Approximated: which bank a pixel shows is decided by window rectangles,
 * standing in for the display table the swap rewrites, whose layout is
 * unknown. A pixel inside any window showing bank 1 shows bank 1, even
 * where another context's window lies over it: a window's stacking order
 * reaches the pipe in no packet the model decodes.
 */
static unsigned sgi_kona_displayed_bank(SGIKonaState *s, int x, int y)
{
    SGIClipRect r;

    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        if (sgi_kona_bank1_window(s, i, &r) &&
            x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) {
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
        return sgi_kona_x_color(s, word);
    default:
        return word;
    }
}

/* The pixel the display shows at plane (x, y), from the bank it shows. */
uint32_t sgi_kona_fb_read(SGIKonaState *s, int x, int y)
{
    return sgi_kona_bank_read(s, sgi_kona_displayed_bank(s, x, y), x, y);
}

/* A direct-colour word into the front bank at plane (x, y). */
void sgi_kona_fb_store_direct(SGIKonaState *s, int x, int y, uint32_t word)
{
    const SGIPlane *front = sgi_kona_plane(s, s->plane_front);

    sgi_plane_store(&s->mem, front, x, y, word);
    sgi_plane_store(&s->mem, sgi_kona_plane(s, s->plane_front_tag), x, y,
                    SGI_KONA_TAG_DIRECT);
    sgi_scanout_invalidate_plane(&s->scanout, front, y, y);
}

/*
 * Display line y for the shared scanout: each pixel resolved by its tag
 * from the bank shown there (see sgi_kona_displayed_bank()), with the
 * unused top byte of an x8r8g8b8 word cleared. No gamma or cursor stage
 * is modelled (the gamma table the X server loads is stored, not
 * applied), and the cursor stays absent: the scanout's host cursor is
 * never defined.
 */
static void sgi_kona_resolve_row(void *opaque, unsigned y, uint32_t *dst)
{
    SGIKonaState *s = opaque;
    int row = SGI_KONA_FB_HEIGHT - 1 - y;
    SGIClipRect r;

    for (unsigned x = 0; x < SGI_KONA_FB_WIDTH; x++) {
        dst[x] = sgi_kona_bank_read(s, 0, x, row) & 0x00ffffff;
    }
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        if (!sgi_kona_bank1_window(s, i, &r) || row < r.y0 ||
            row > r.y1) {
            continue;
        }
        for (int x = r.x0; x <= r.x1; x++) {
            dst[x] = sgi_kona_bank_read(s, 1, x, row) & 0x00ffffff;
        }
    }
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

/*
 * The one mode the model presents: the framebuffer's size, with the
 * retrace at SGI_KONA_FIELD_HZ (see that macro for why it is the model's
 * choice). A 60 Hz clock counting one per frame expresses it exactly.
 */
static const SGIScanoutMode sgi_kona_scanout_mode = {
    .width = SGI_KONA_FB_WIDTH,
    .height = SGI_KONA_FB_HEIGHT,
    .frame_clocks = 1,
    .clock_hz = SGI_KONA_FIELD_HZ,
};

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
     * SGI_KONA_FB_HEIGHT - 1 - y. The pixel keeps its colour-map index,
     * resolved at scanout (see sgi_kona_fb_read()).
     */
    sgi_plane_store(&s->mem, front, x, y, s->draw_color & 0xff);
    sgi_plane_store(&s->mem, sgi_kona_plane(s, s->plane_front_tag), x, y,
                    SGI_KONA_TAG_COLOR_INDEX);
    sgi_scanout_invalidate_plane(&s->scanout, front, y, y);
}

/*
 * Filled rectangle (command 5): x0, y0, x1, y1, inclusive on every side.
 * Established from two independent sources: the sender's own code
 * (IO6 PROM, command 5's sender routine) pushes wire words
 * `max(a1-1,0), max(a2-1,0), a3, a4` with a3/a4 passed through unmodified,
 * and a real captured boot trace shows those same four wire words holding
 * a constant x0/x1 pair (0/1279, the full screen width) while y0/y1 vary
 * such that each call's y0 equals the previous call's y1 -- the chaining an
 * inclusive-both-ends rectangle produces, not an exclusive one (which would
 * leave a one-row gap between adjacent fills instead of the observed
 * shared boundary row).
 */
static void sgi_kona_fill_rect(SGIKonaState *s, int32_t x0, int32_t y0,
                               int32_t x1, int32_t y1)
{
    int32_t x, y;

    for (y = y0; y <= y1; y++) {
        for (x = x0; x <= x1; x++) {
            sgi_kona_draw_point(s, x, y);
        }
    }
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

/*
 * Unpack the completed payload into a rectangle anchored on the
 * textport's own raster position, one bit per pixel, MSB-first,
 * row-major. The header's field order -- width, height, x origin, y
 * origin, x advance, y advance -- is established from the IO6 PROM's
 * kona_tp_bitmap routine, which sends six header words from halfwords
 * +0x8..+0x12 of the caller's descriptor in that order. The PROM also
 * places every bitmap relative to the raster position (command 4): every
 * bitmap it sends is preceded by one (icons) or starts a run of glyphs
 * that advance from it (labels). The PROM's font_glyph_draw shows the
 * origin fields are a glyph's signed x/y offsets negated into the
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
 * kona_attach()'s own entry-count formula: N == 0 selects 8, otherwise N,
 * then shifted left 15 (established). At this model's default
 * probe-value (0), N is 0, so the count is SGI_KONA_MAPRAM_ENTRIES, the
 * maximum the fixed-size backing array below holds; a nonzero
 * probe-value can only shrink it, as SGI_KONA_PROBE's header comment
 * says.
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
 * One unsolicited mailbox word, after any reply words still unread, the
 * way kona_arm_intr takes them one at a time. A queue with no room left
 * is logged and the word is lost.
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

static void sgi_kona_raise_vector(SGIKonaState *s, bool general);

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

    switch (c->kind) {
    case SGI_DONE_SWAPPED:
        s->retrace_tag = tag;
        s->retrace_completed = true;
        trace_sgi_kona_retrace_done(tag);
        if (slot >= 0 && slot < SGI_KONA_CONTEXTS &&
            s->contexts[slot].valid && s->gl_window[slot].double_buffered) {
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
}

/*
 * The scanout reached a retrace. If it honoured a retrace request, raise
 * the video bit, which kona_video_intr takes, and send the general
 * vector.
 */
static void sgi_kona_retrace(void *opaque)
{
    SGIKonaState *s = opaque;

    if (!s->retrace_completed) {
        return;
    }
    s->retrace_completed = false;
    s->int_cause |= SGI_KONA_INT_CAUSE_VIDEO;
    sgi_kona_raise_vector(s, true);
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
 *   under cause bit 0x10: kona_arm_intr reads +0x800240 and posts the
 *   semaphore kona_dmawrite sleeps on. A cookie naming the video bit
 *   0x8 sets it with no mailbox word.
 *   FAILED for an item whose sender waits sets the fatal cause bit 0x100.
 *   Approximated: what the pipe does when a transfer fails is unknown;
 *   the model makes the failure visible to the driver's handler, which
 *   dies on that bit, rather than post a completion for work not done.
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
         * kona_intr_handler and kona_arm_intr); nothing polls them.
         */
        if (cause & SGI_KONA_INT_CAUSE_MAILBOX) {
            sgi_kona_raise_vector(s, true);
        }
        break;
    case SGI_DONE_FAILED:
        if (c->cookie != SGI_KONA_DMA_COOKIE_NONE &&
            c->cookie != SGI_KONA_DMA_COOKIE_PORT) {
            s->int_cause |= SGI_KONA_INT_CAUSE_FATAL;
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
 * The graphics credits of the processor stores the board takes. The
 * driver's flow control charges one credit in the storing processor's
 * interface for each uncached store into the page it enabled for the
 * pipe, and kona_flow_await_returned_credits waits for the count to come
 * back to the total it wrote before it lets the next process attach,
 * killing the pipe (death reason 25) when it does not. The board returns
 * one credit for each charged store once the words the store carried
 * have been taken: at once for a word a framer consumed or a data run
 * took, and with the packet's last word for one a framer still holds, so
 * the credits of a packet come back together. A doubleword store carries
 * two words and earns one credit, as the interface charged one. Words a
 * display list fetched from memory were never stored and earn none.
 *
 * Inferred: that the pipe returns a credit when its front end takes a
 * store's words, and holds the credits of a packet still arriving, which
 * is the same reading SGI_KONA_CREDIT gives the board's own credit
 * register. That register stays: the PROM tests it for zero before every
 * textport command, and it and these credits both count the words a
 * framer holds. The credits go to the board's node through the map the
 * driver programs for its widget; with no "pi" link the board logs once
 * and returns nothing.
 */
static void sgi_kona_pi_credits(SGIKonaState *s, bool counted)
{
    uint32_t credits;

    if (counted) {
        s->pi_held++;
    }
    if (!s->pi_held || sgi_kona_port_held(&s->textport) ||
        sgi_kona_port_held(&s->kernel_port) ||
        sgi_kona_port_held(&s->gl_port) ||
        sgi_kona_port_held(&s->dma.fifo_port)) {
        return;
    }
    credits = s->pi_held;
    s->pi_held = 0;
    if (!s->pi) {
        if (!s->pi_unlinked_logged) {
            s->pi_unlinked_logged = true;
            qemu_log_mask(LOG_UNIMP, "sgi-kona: no \"pi\" link; graphics"
                          " credits are not returned\n");
        }
        return;
    }
    trace_sgi_kona_pi_credits(SGI_XIO_BOARD(s)->slot, credits);
    sgi_pi_gfx_return_credits(s->pi, SGI_XIO_BOARD(s)->slot, credits);
}

static void sgi_kona_textport_packet(void *opaque, unsigned port_id,
                                     uint32_t selector, const uint32_t *args,
                                     unsigned argc)
{
    SGIKonaState *s = opaque;

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
            sgi_scanout_invalidate_all(&s->scanout);
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
         * Screen size: accepted and traced only; the framebuffer stays
         * fixed at SGI_KONA_FB_WIDTH x SGI_KONA_FB_HEIGHT.
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
            s, (uint32_t[]){ 0, 0, 0, SGI_KONA_FB_WIDTH, SGI_KONA_FB_HEIGHT },
            5);
        break;
    case 0x4:
        /*
         * Raster position: established from the IO6 PROM's
         * kona_tp_raster_pos routine as the position every subsequent
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

static bool sgi_kona_kernel_token_known(uint32_t word)
{
    for (unsigned i = 0; i < sgi_kona_command_num_tokens; i++) {
        if (sgi_kona_command_tokens[i].selector == word) {
            return true;
        }
    }
    return false;
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
        trace_sgi_kona_reason_page(s->reason_page, mapped);
    }
}

/*
 * 0x1f2 a b len: remember the return-data page, where the pipe answers
 * a process's requests and the process polls word 0. KONApcxswap sends
 * 0x1f2 straight after a context's first-activation 0x1fd and never on a
 * restore, so that page becomes the context's own, restored with it by
 * 0x1f9 (see sgi_kona_context_activate()). That the pipe keeps a return
 * page per context is inferred from those two paths. A 0x1f2 with no
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
}

static void sgi_kona_gl_reset_context(SGIKonaState *s, unsigned i);

/*
 * 0x1fd page offset len, or 0x1f9 word0 page offset sentinel: make the
 * context whose save area the two words name current, and answer on its
 * return page from here on. A 0x1fd starts the context afresh, its GL
 * state and window included (see sgi_kona_gl_reset_context()), with no
 * return page until the 0x1f2 that follows it. A 0x1f9 restores the page
 * that 0x1f2 gave it; one naming a save area no 0x1fd named is logged and
 * leaves the pipe with no return page, so an answer goes nowhere rather
 * than to another context. The key is the two words as sent, which the
 * kernel derives from one fixed allocation per context. See
 * SGIKonaContext for the table's size and eviction.
 */
static void sgi_kona_context_activate(SGIKonaState *s, uint32_t token,
                                      uint32_t page, uint32_t offset)
{
    bool first = token == SGI_KONA_CMD_CONTEXT_MAP;
    bool fresh = first;
    uint64_t device = ((uint64_t)page << 12) | (offset & 0xfff);
    SGIKonaContext *c;
    int slot = SGI_KONA_CONTEXT_NONE;

    offset &= 0xfff;
    for (int i = 0; i < SGI_KONA_CONTEXTS; i++) {
        c = &s->contexts[i];
        if (c->valid && c->save_page == page && c->save_offset == offset) {
            slot = i;
            break;
        }
    }
    if (slot == SGI_KONA_CONTEXT_NONE) {
        if (!first) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: context restore names save area 0x%"
                          PRIx64 ", which no 0x%x set up; no return page\n",
                          device, SGI_KONA_CMD_CONTEXT_MAP);
        }
        for (int i = 0; i < SGI_KONA_CONTEXTS; i++) {
            if (!s->contexts[i].valid) {
                slot = i;
                break;
            }
        }
        if (slot == SGI_KONA_CONTEXT_NONE) {
            slot = 0;
            for (int i = 1; i < SGI_KONA_CONTEXTS; i++) {
                if (s->contexts[i].last_used < s->contexts[slot].last_used) {
                    slot = i;
                }
            }
            c = &s->contexts[slot];
            qemu_log_mask(LOG_UNIMP,
                          "sgi-kona: context table full (%d entries);"
                          " forgetting the context with save area 0x%"
                          PRIx64 "\n", SGI_KONA_CONTEXTS,
                          ((uint64_t)c->save_page << 12) | c->save_offset);
        }
        c = &s->contexts[slot];
        memset(c, 0, sizeof(*c));
        c->valid = true;
        c->save_page = page;
        c->save_offset = offset;
        fresh = true;
    }
    c = &s->contexts[slot];
    if (first) {
        c->return_page_valid = false;
        c->return_page = 0;
    }
    if (fresh) {
        sgi_kona_gl_reset_context(s, slot);
    }
    c->last_used = ++s->context_clock;
    s->context_current = slot;
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
 * the driver programs is inferred: kona_reset_adaptor writes one
 * destination and three vectors. Not gated on SGI_KONA_INT_ENABLE, which
 * the model has never interpreted: KONAstart writes 0xf7c there, which
 * includes the geometry-engine and video bits, and the driver applies it
 * to the general vector's cause in software.
 */
static void sgi_kona_raise_vector(SGIKonaState *s, bool general)
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

    if (!s->int_dest_upper && !s->int_dest_lower) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: %s interrupt with no"
                      " destination programmed; no packet sent\n", what);
        return;
    }
    if (!sgi_kona_fabric_compose(s, widget, address, &where)) {
        return;
    }
    if (general) {
        trace_sgi_kona_general_interrupt(where, vector);
    } else {
        trace_sgi_kona_ge_interrupt(where, vector);
    }
    sgi_xio_send_interrupt(&s->fabric_as, where, vector, true);
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
 * 0x1f8 R and 0x1dd R, and 0x1fc with its fixed reason 0x101: write R
 * big-endian to word 0 of the reason page, set the geometry-engine cause
 * bit and raise its interrupt, which is what kona_ge_intr reads and
 * dispatches on. Approximated: hardware is
 * inferred to post only once its queued work is done, and the model has
 * no queued work, so it posts at once. With no page registered it logs,
 * traces and posts nothing.
 */
static void sgi_kona_post_reason(SGIKonaState *s, uint32_t token,
                                 uint32_t reason)
{
    uint8_t bytes[4];

    if (!s->reason_page_valid) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%x reason 0x%x with no reason page"
                      " registered; nothing posted\n", token, reason);
        trace_sgi_kona_reason_no_page(token, reason);
        return;
    }
    stl_be_p(bytes, reason);
    if (!sgi_kona_host_write(s, s->reason_page, bytes, sizeof(bytes))) {
        return;
    }
    trace_sgi_kona_reason_post(token, reason, s->reason_page);
    s->int_cause |= SGI_KONA_INT_CAUSE_GE;
    sgi_kona_raise_vector(s, false);
}

/*
 * 0x1fc index: switch the current context out. KONApcxswap then sleeps
 * until the pipe posts reason 0x101, and kona_ge_intr wakes it and
 * cancels its 90 s deactivate timeout. When the context is restored
 * later, KONApcxswap reads words 0 and 2 of its save area back (see
 * SGI_KONA_SAVE_AREA_MARKER), so they are written before the post. The
 * index names the context's entry in the kernel's table, not the save
 * area, so the save area is the one the last 0x1fd or 0x1f9 named.
 *
 * Approximated: hardware writes a real context record into the save area,
 * whose contents are unknown; the model writes only the two words the
 * kernel reads back. Hardware presumably posts once the outgoing
 * context's queued work has drained and its state is saved; the model
 * keeps no queued work, so it posts at once. The guest checks are the
 * minimal GLX program's first make-current going on to 0x1fd and 0x1f2
 * rather than the timeout, and a later swap back sending
 * 0x1f9 0x4b4f4e41 and 0x172 0.
 */
static void sgi_kona_context_deactivate(SGIKonaState *s, uint32_t index)
{
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
    s->context_current = SGI_KONA_CONTEXT_NONE;
    s->context_page_pending = false;
    sgi_kona_post_reason(s, SGI_KONA_CMD_CONTEXT_DEACTIVATE,
                         SGI_KONA_REASON_CONTEXT_DEACTIVATED);
}

/*
 * The board configuration the pipe answers 0x1e9 with. gfxinit hands the
 * 0x778 bytes from return page + 8 to the kernel, which copies them over
 * its board information; every later process reads them back from there.
 * Offsets are from page + 8.
 *
 * Approximated: every value is the model's own choice for the one
 * configuration it emulates, one pipe with one raster manager driving one
 * 1280x1024 channel, and not read from a real pipe. Readers are known
 * for these seven fields and the pool descriptors below only;
 * every other byte is unknown and written zero. Each field is written as
 * a 32-bit big-endian word, which the readers' widths do not establish.
 * The guest test that tells a working block from a wrong one is gfxinfo
 * or gfxinit -v reporting one channel and occupancy mask 0xf, with no
 * "return area did not go valid".
 */
#define SGI_KONA_CONFIG_OFFSET 8
#define SGI_KONA_CONFIG_SIZE   0x778

/*
 * The four pipe-memory pool descriptors at block offset 0x90 + 0x1c *
 * class. kona_mem_init points class i's pool at them
 * and kona_mem_alloc checks every request against them; nothing in the
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
      .copy_down = 1, .granule = 0x400, .capacity = 0x10000 },
};

static const struct {
    uint32_t offset;
    uint32_t value;
} sgi_kona_config_fields[] = {
    { 0x00, 0 },                   /* board set, 0 to 6 */
    { 0x14, 1 },                   /* display channel count */
    { 0x58, 0 },                   /* count of 0x8c-byte records */
    { 0x5c, 0xf },                 /* occupancy mask */
    { 0x84, SGI_KONA_FB_WIDTH },   /* width */
    { 0x88, SGI_KONA_FB_HEIGHT },  /* height */
    { 0x8c, 0 },                   /* pixel-depth code, 0 to 3 */
};

/*
 * Post a return-page answer: `len` bytes at `offset`, none when `len` is
 * 0, then a non-zero word 0, the only thing the waiting process polls. The
 * order is inferred: gfxinit hands the page to the kernel as soon as word
 * 0 goes non-zero, so the data must already be there. Approximated like the
 * reason post: the model answers at once rather than after queued work.
 * With no return page registered it logs and writes nothing.
 */
static void sgi_kona_post_return(SGIKonaState *s, uint32_t token,
                                 uint32_t offset, const void *data,
                                 uint32_t len)
{
    uint8_t ready[4];

    if (!s->return_page_valid) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%x with no return page registered;"
                      " nothing posted\n", token);
        return;
    }
    if (len && !sgi_kona_host_write(s, s->return_page + offset, data, len)) {
        return;
    }
    stl_be_p(ready, 1);
    if (sgi_kona_host_write(s, s->return_page, ready, sizeof(ready))) {
        trace_sgi_kona_return_post(token, s->return_page);
    }
}

static void sgi_kona_answer_config(SGIKonaState *s)
{
    uint8_t block[SGI_KONA_CONFIG_SIZE] = { 0 };

    for (size_t i = 0; i < ARRAY_SIZE(sgi_kona_config_fields); i++) {
        stl_be_p(block + sgi_kona_config_fields[i].offset,
                 sgi_kona_config_fields[i].value);
    }
    for (size_t i = 0; i < ARRAY_SIZE(sgi_kona_pools); i++) {
        const SGIKonaPoolDescriptor *pool = &sgi_kona_pools[i];
        uint8_t *p = block + SGI_KONA_POOL_OFFSET + sizeof(*pool) * i;

        stl_be_p(p + 0x00, pool->max_key);
        stl_be_p(p + 0x04, pool->max_align);
        stl_be_p(p + 0x08, pool->copies);
        stl_be_p(p + 0x0c, pool->shared);
        stl_be_p(p + 0x10, pool->copy_down);
        stl_be_p(p + 0x14, pool->granule);
        stl_be_p(p + 0x18, pool->capacity);
    }
    sgi_kona_post_return(s, SGI_KONA_CMD_GET_CONFIG, SGI_KONA_CONFIG_OFFSET,
                         block, sizeof(block));
}

/*
 * 0x128 W H D, X initialise: write W, H and D to words 2, 3 and 4 of the
 * return page, then word 0 = 1. kon_InitHW waits for word 0, then
 * compares words 2 to 4 with the width, height and depth code it sent
 * and logs a mismatch. Approximated: the model echoes the
 * request, so it can never report a mode other than the one asked for,
 * where a real pipe presumably reports the mode it set up. The guest
 * check is an X server log with no "arm says" mismatch and no pipe
 * health-check message from kon_InitHW.
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
 * next word as the checksum; a checksum that does not match is traced and
 * the command still acts, because what the pipe does with a bad checksum is
 * unknown. Falling back to the checksum search on a mismatch would cover the
 * pass-through sender at 0x4b808c, which can send other lengths; the model
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
 * What the model answers each video Get with, as data words from word 3
 * of the return page; words past those listed, up to the command's reply
 * size in sgi_kona_video_commands[], are zero.
 *
 * Approximated: the values describe the one configuration the model
 * emulates, not a real pipe's answer. GetBoardConfig: display-generator
 * version 1, board mask 1 (channel 0 only), option mask 0, options code
 * 0 and flag 0; the X server treats an options code of 0x41 or more, or
 * the option bits 0x2 and 0x4, as fatal. GetChannelConfig: bit 0 of the
 * first word set, channel present, whose absence is fatal. Every other
 * reply of the table, whichever its basis, is answered zero at its
 * size: a generic answer, unsafe for these two alone, the others' fields
 * being unexamined against their readers.
 * So GetDGInterruptState's three zero words set neither bit 21 nor bit 20
 * of the second, and the X server takes its genlock flag from its own
 * default; QueryMonitorName's zero word 0 and zero name bytes give the
 * channel an empty monitor name and a zero word, which is what the X
 * server's copy does with a zero reply. Which values a real display
 * generator sends is unknown. The guest checks are a trace
 * with board mask 1 showing exactly one GetChannelConfig, with data 0,
 * after XServerVideoInit, one QueryMonitorName per channel, and an X
 * server log with no "Unexpected reply" or "Checksum mismatch" line.
 */
static const struct {
    uint32_t command;
    uint32_t words[5];
} sgi_kona_video_replies[] = {
    { SGI_KONA_VIDEO_GET_BOARD_CONFIG, { 1, 1, 0, 0, 0 } },
    { SGI_KONA_VIDEO_GET_CHANNEL_CONFIG, { 1 } },
};

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
    uint32_t words = v->reply_bytes / 4;
    uint32_t sum = v->reply_id;

    assert(v->reply_bytes <= SGI_KONA_VIDEO_MAX_REPLY_BYTES);
    for (size_t i = 0; i < ARRAY_SIZE(sgi_kona_video_replies); i++) {
        if (sgi_kona_video_replies[i].command != v->command) {
            continue;
        }
        for (uint32_t w = 0;
             w < words && w < ARRAY_SIZE(sgi_kona_video_replies[i].words);
             w++) {
            stl_be_p(reply + 4 + 4 * w, sgi_kona_video_replies[i].words[w]);
        }
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
    }
    if (!v) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: unmodelled video command 0x%" PRIx32
                      " (data 0x%" PRIx32 "); consumed, nothing posted\n",
                      s->video_command, s->video_first);
        return;
    }
    if (v->reply_id != SGI_KONA_VIDEO_NO_REPLY) {
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
    if (s->video_have++ == 0) {
        s->video_first = word;
    } else {
        s->video_tail_sum += word;
    }
    if (length == SGI_KONA_VIDEO_DATA_UNKNOWN &&
        s->video_have == SGI_KONA_VIDEO_SEARCH_MAX_WORDS) {
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
 * 0x12e, X load colour map: a start address, a count n, then n entries
 * (established from konInstallColormap and konStoreColors). The command
 * port frames the first two words; entry i lands at start + i in
 * the X colour map. An entry past the table is counted and logged, not
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
        sgi_scanout_invalidate_all(&s->scanout);
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
 * kona.so's sender). The command port frames the first two words; the
 * entries follow here and land in s->gamma, one address per entry from the
 * start address. The table is stored for the scanout to apply later; the
 * framebuffer is still shown as stored.
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
static void sgi_kona_gamma_complete(SGIKonaState *s)
{
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

/*
 * 0x130, X load cursor: a slot word, then 260 data words. kon_cursorLogo
 * sends slot 0; the second
 * sender, the routine at 0x526140, sends 260 times a cursor-private word,
 * inferred to be an offset into a cursor table (0x2698 in the run). The
 * model accepts any slot and traces it. The command port frames the slot
 * word; the data words are consumed here. The cursor image is not
 * modelled: nothing is drawn or stored.
 *
 * Approximated: the length is fixed at the senders' 260, because nothing
 * on the wire carries it; a cursor load of another size would misframe
 * every word after it. The discriminating guest test is the X server's
 * cursor loads framing as whole commands with nothing dropped after them.
 */
static void sgi_kona_cursor_open(SGIKonaState *s, uint32_t slot)
{
    trace_sgi_kona_x_cursor(slot);
    s->cursor_remaining = SGI_KONA_X_CURSOR_DATA_WORDS;
}

/* A word on the command page while a cursor load takes its data. */
static void sgi_kona_cursor_word(SGIKonaState *s)
{
    if (--s->cursor_remaining == 0) {
        trace_sgi_kona_command_unmodelled(SGI_KONA_CMD_X_CURSOR,
                                          1 + SGI_KONA_X_CURSOR_DATA_WORDS);
    }
}

/*
 * The GL client's 0xee 0 (get line-mode distance, paraphrasing the pipe's
 * command name): __glkon_get_special waits for word
 * 0 of the return page to go non-zero, and its caller then copies 32 words
 * from byte 8, eight records of four, words 0 and 1 of each into one
 * 16-word context block and words 2 and 3 into another, which the
 * library's pixel-transfer code reads later (established from the
 * reader). The answer goes on the current context's return page.
 *
 * Approximated: what the 32 words mean is unknown, and zeros are the
 * model's choice. The guest check is the minimal GLX program's probe
 * going on from 0xee to its viewport 0xe3 and scissor 0xc.
 */
#define SGI_KONA_GL_LINE_DISTANCE_WORDS 32

static void sgi_kona_gl_line_distance(SGIKonaState *s)
{
    uint8_t words[SGI_KONA_GL_LINE_DISTANCE_WORDS * 4] = { 0 };

    sgi_kona_post_return(s, SGI_KONA_CMD_GL_LINE_DISTANCE, 8, words,
                         sizeof(words));
}

/*
 * The GL client's glGetError, 0x3e and a word the pipe ignores: the library
 * waits without limit for word 0 of the context's return page and then reads
 * the error code from byte 8 (established from the reader). The library
 * does keep the first error it raised itself, but only its generic
 * glGetError reads that field; the KONA glGetError never does and asks
 * the pipe, so the code the client sees comes from the pipe alone. The
 * library sends its own errors to the pipe as 0xe4 and the code.
 *
 * Approximated: the model keeps no error state, drops 0xe4's code, and
 * always answers 0, no error. Returning the first error recorded by 0xe4
 * and clearing it, as glGetError requires, is inferred and not modelled.
 * The guest check is the minimal GLX probe's clear case getting past its
 * glGetError to the next command.
 */
static void sgi_kona_gl_get_error(SGIKonaState *s)
{
    uint8_t error[4];

    stl_be_p(error, 0);
    sgi_kona_post_return(s, SGI_KONA_CMD_GL_GET_ERROR, 8, error,
                         sizeof(error));
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
    .evidence = "chosen, pending capture: the shared rasterizer's first "
                "implemented set, eight bits a channel for the model's "
                "planes, no dither",
};

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

static void sgi_kona_gl_store_colour(void *opaque, SGIColourTarget target,
                                     int x, int y, uint32_t colour)
{
    SGIKonaState *s = opaque;
    int bank = sgi_kona_target_bank(s, target);
    const SGIPlane *p;

    if (bank < 0) {
        return;
    }
    p = sgi_kona_plane(s, sgi_kona_bank_plane(s, bank));
    sgi_plane_store(&s->mem, p, x, y, colour & 0x00ffffff);
    sgi_plane_store(&s->mem, sgi_kona_plane(s, sgi_kona_bank_tag_plane(s,
                                                                  bank)),
                    x, y, SGI_KONA_TAG_DIRECT);
    sgi_scanout_invalidate_plane(&s->scanout, p, y, y);
}

static uint32_t sgi_kona_gl_load_colour(void *opaque, SGIColourTarget target,
                                        int x, int y)
{
    SGIKonaState *s = opaque;
    int bank = sgi_kona_target_bank(s, target);

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

    sgi_plane_store(&s->mem, sgi_kona_plane(s, s->plane_depth), x, y, depth);
}

/*
 * The model's planes as the shared rasterizer reaches them: the colour
 * banks and the depth plane. No stencil plane is modelled, so the
 * stencil test passes and changes nothing (the shared rasterizer's rule
 * for planes without one).
 */
static void sgi_kona_gl_target(SGIKonaState *s, SGIRasterTarget *t)
{
    *t = (SGIRasterTarget) {
        .store_colour = sgi_kona_gl_store_colour,
        .load_colour = sgi_kona_gl_load_colour,
        .load_depth = sgi_kona_gl_load_depth,
        .store_depth = sgi_kona_gl_store_depth,
        .opaque = s,
        .screen_top = SGI_KONA_FB_HEIGHT - 1,
        .screen_rows_down = false,
    };
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
    return i < SGI_KONA_CONTEXTS && s->gl_pipe[i].depth_bits;
}

static SGIGLDrawResult sgi_kona_gl_draw_checked(void *opaque,
                                                const SGIGLDraw *d)
{
    SGIKonaState *s = opaque;
    SGIRasterTarget t;
    SGIGLDrawState st;
    SGIGLDraw copy;
    SGIGLDrawResult ret;

    sgi_kona_gl_target(s, &t);
    if (d->state->depth_test && !sgi_kona_gl_has_depth(s, d->context)) {
        st = *d->state;
        st.depth_test = 0;
        copy = *d;
        copy.state = &st;
        d = &copy;
    }
    ret = sgi_soft_draw(&sgi_kona_gl_rules, &t, d);
    if (ret != SGI_GL_DRAW_OK) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: GL draw of context %" PRIu32
                      " not completed by the rasterizer\n", d->context);
    }
    return ret;
}

static void sgi_kona_gl_draw(void *opaque, const SGIGLDraw *d)
{
    sgi_kona_gl_draw_checked(opaque, d);
}

static void sgi_kona_gl_raster(void *opaque, const SGIRasterOp *op)
{
    SGIKonaState *s = opaque;
    SGIRasterTarget t;
    SGIRasterOp copy;

    sgi_kona_gl_target(s, &t);
    if ((op->clear_buffers & SGI_GL_DEPTH_BUFFER_BIT) &&
        (s->context_current == SGI_KONA_CONTEXT_NONE ||
         !sgi_kona_gl_has_depth(s, s->context_current))) {
        copy = *op;
        copy.clear_buffers &= ~SGI_GL_DEPTH_BUFFER_BIT;
        op = &copy;
    }
    sgi_soft_raster(&sgi_kona_gl_rules, &t, op);
}

/*
 * glFinish's fence. Every draw has been stored by the time the context
 * sends it, so the finish completes at once: word 0 of the return page,
 * the one thing the client waits on.
 */
static void sgi_kona_gl_fence(void *opaque, uint32_t context)
{
    sgi_kona_post_return(opaque, SGI_KONA_CMD_GL_FINISH, 0, NULL, 0);
}

static void sgi_kona_gl_sink(SGIKonaState *s, SGIGLSink *sink)
{
    *sink = (SGIGLSink) {
        .draw = sgi_kona_gl_draw,
        .raster = sgi_kona_gl_raster,
        .fence = sgi_kona_gl_fence,
        .opaque = s,
        .draw_checked = sgi_kona_gl_draw_checked,
    };
}

/*
 * Point context table entry i's GL context at its window and banks: see
 * SGIKonaGLWindow. A single-buffered context's front is bank 0 and it has
 * no back; a double-buffered one's front is the bank its window shows and
 * its back the other.
 */
static void sgi_kona_gl_set_drawable(SGIKonaState *s, unsigned i)
{
    const SGIKonaGLWindow *w = &s->gl_window[i];
    unsigned front = w->double_buffered ? w->displayed_bank : 0;
    SGIGLDrawable d = {
        .bound = w->placed,
        .origin = { w->x, w->y },
        .front = sgi_kona_bank_plane(s, front),
        .back = w->double_buffered ? sgi_kona_bank_plane(s, !front) :
                                     SGI_PLANE_NONE,
    };

    if (sgi_kona_gl_window_rect(w, &d.clip.r[0])) {
        d.clip.n = 1;
    } else {
        d.clip.empty = 1;
    }
    sgi_gl_set_drawable(&s->gl[i], &d);
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
    s->gl_window[i].displayed_bank ^= 1;
    trace_sgi_kona_gl_swap(i, s->gl_window[i].displayed_bank);
    sgi_kona_gl_set_drawable(s, i);
}

/* Context table entry i starts afresh: GL's initial state, no window. */
static void sgi_kona_gl_reset_context(SGIKonaState *s, unsigned i)
{
    SGIGLSink sink;

    sgi_gl_reset(&s->gl[i], i);
    /*
     * glPolygonMode's outlines and points and stippled lines go through
     * the shared context's checked draw path, which this sink has.
     */
    sgi_kona_gl_sink(s, &sink);
    sgi_gl_enable_polygon_modes(&s->gl[i], &sink);
    sgi_gl_enable_line_stipple(&s->gl[i], &sink);
    memset(&s->gl_window[i], 0, sizeof(s->gl_window[i]));
    s->gl_pipe[i] = (SGIKonaGLPipe) {
        .unpack_alignment = 4,
        .pack_alignment = 4,
    };
    s->gl_attrib_depth[i] = 0;
    sgi_kona_gl_set_drawable(s, i);
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

/*
 * 0x1f3 and 0x1ee: the current context's window, as SGIKonaGLWindow
 * reads their words. The context keeps it until the next one, since
 * kona_act_pcx sends neither on a restore it does not mark stale.
 */
static void sgi_kona_gl_window(SGIKonaState *s, uint32_t selector,
                               const uint32_t *args)
{
    int i = sgi_kona_gl_current(s, selector);
    SGIKonaGLWindow *w;

    if (i == SGI_KONA_CONTEXT_NONE) {
        return;
    }
    w = &s->gl_window[i];
    w->x = args[0];
    w->y = args[1];
    if (selector == SGI_KONA_CMD_CONTEXT_RECORDS) {
        w->width = args[6];
        w->height = args[7];
        w->placed = 1;
    }
    trace_sgi_kona_gl_window(selector, i, w->x, w->y, w->width, w->height);
    sgi_kona_gl_set_drawable(s, i);
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
    s->gl_window[i].double_buffered =
        args[SGI_KONA_GL_BUFFER_CONFIG_DOUBLE] != 0;
    s->gl_pipe[i].depth_bits = args[SGI_KONA_GL_BUFFER_CONFIG_DEPTH];
    buffers = s->gl_window[i].double_buffered ? SGI_GL_DRAW_BACK :
                                                SGI_GL_DRAW_FRONT;
    sgi_kona_gl_sink(s, &sink);
    sgi_gl_command(&s->gl[i], &sink,
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
        sgi_gl_command(&s->gl[s->context_current], &sink,
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
        word = args[0] & (SGI_GL_COLOR_BUFFER_BIT | SGI_GL_DEPTH_BUFFER_BIT);
        if (args[0] != word) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: glClear of buffers 0x%"
                          PRIx32 "; only the colour and depth buffers are"
                          " modelled\n", args[0]);
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
    sgi_gl_command(&s->gl[i], &sink, &cmd);
}

/*
 * A GL texture table: 0x17e, a slot word, then SGI_KONA_GL_TEX_TABLE_WORDS
 * table words (established from bindTexTable and __glkon_datavn). The data
 * words arrive as sixteen doubleword stores and one word store; each
 * doubleword reaches the page as two words, high word first (see
 * sgi_kona_write()). The command port frames the slot word; the data words
 * are consumed here, since the table words are arbitrary bit patterns
 * that would otherwise open commands of their own. What the tables control
 * is unknown, so nothing is stored.
 */
static void sgi_kona_gl_tex_table_open(SGIKonaState *s, uint32_t slot)
{
    trace_sgi_kona_gl_tex_table(slot);
    s->gl_tex_table_remaining = SGI_KONA_GL_TEX_TABLE_WORDS;
}

/* A word on the command page while a texture table takes its data. */
static void sgi_kona_gl_tex_table_word(SGIKonaState *s)
{
    if (--s->gl_tex_table_remaining == 0) {
        trace_sgi_kona_command_unmodelled(SGI_KONA_CMD_GL_TEX_TABLE,
                                          1 + SGI_KONA_GL_TEX_TABLE_WORDS);
    }
}

/* A float's bits, as the shared context takes a float argument. */
static uint32_t sgi_kona_gl_bits(float f)
{
    uint32_t bits;

    memcpy(&bits, &f, sizeof(bits));
    return bits;
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

    switch (selector) {
    case SGI_KONA_CMD_GL_MATRIX_MODE:
        if (args[0] == 0x1800) {
            qemu_log_mask(LOG_UNIMP, "sgi-kona: the colour matrix stack is"
                          " not modelled; glMatrixMode ignored\n");
            return true;
        }
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
    case SGI_KONA_CMD_GL_COLOR3D:
    case SGI_KONA_CMD_GL_COLOR4D:
    case SGI_KONA_CMD_GL_NORMAL3D:
        for (int k = 0; k < cmd.nargs; k++) {
            a[k] = sgi_kona_gl_double(&args[2 * k]);
        }
        break;
    }
    trace_sgi_kona_gl_command(selector, i);
    sgi_kona_gl_sink(s, &sink);
    sgi_gl_command(&s->gl[i], &sink, &cmd);
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
    sgi_gl_polygon_stipple(&s->gl[i], rows);
}

/*
 * The GL client's raster state, onto the current context's draw state:
 * depth, blending, alpha, stencil, masks, logic op, culling and facing,
 * polygon modes and offset, lines and points, fog, clip planes, draw
 * buffer and texture environment, each carrying the caller's enumerants
 * and values (established from the senders). The integer forms of glFog
 * and glTexEnv convert by OpenGL's rules. glClearDepth's double in 0 to 1
 * is scaled to the 24-bit depth plane, rounded to nearest, the model's
 * choice. The stencil state is kept, but no stencil plane is modelled, so
 * its test passes. glHint and glClearAccum have no shared state and are
 * absorbed. Returns false for a selector this does not carry out.
 */
static bool sgi_kona_gl_raster_state(SGIKonaState *s, int i,
                                     uint32_t selector, const uint32_t *args)
{
    uint32_t a[5];
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
        cmd = (SGIGLCmd) { SGI_GL_OP_DRAW_BUFFERS, 1, a };
        break;
    case SGI_KONA_CMD_GL_HINT:
    case SGI_KONA_CMD_GL_CLEAR_ACCUM:
        trace_sgi_kona_command_unmodelled(selector, 0);
        return true;
    default:
        return false;
    }
    trace_sgi_kona_gl_command(selector, i);
    sgi_kona_gl_sink(s, &sink);
    sgi_gl_command(&s->gl[i], &sink, &cmd);
    return true;
}

/*
 * The return page's size: the length every traced 0x1f2 registered, and
 * so the most a reply can carry (inferred).
 */
#define SGI_KONA_RETURN_PAGE_BYTES 0x4000

/*
 * The colour bank context i's read buffer names: glReadBuffer's 0x37, or
 * by default the back buffer of a double-buffered visual and the front of
 * a single-buffered one (OpenGL 1.2.1, 4.3.2). -1 for none.
 */
static int sgi_kona_gl_read_bank(SGIKonaState *s, int i)
{
    const SGIGLDrawable *d = &s->gl[i].cur.drawable;
    uint32_t buffer = s->gl_pipe[i].read_buffer;
    bool back = buffer ? buffer == SGI_GL_BACK || buffer == 0x0402 :
                         s->gl_window[i].double_buffered;

    return sgi_kona_target_bank(s, back ? d->back : d->front);
}

/*
 * 0x39 and seven words, glReadPixels: the client waits on word 0 of the
 * return page and copies the pixels from byte 8 (established from the
 * reader). Which word is what is inferred from the sender's stores: the
 * window x and y, the width and height, the format and type (the record
 * fields the texel load also sends) and the destination address's low
 * two bits, which the model takes as where in the first word the pixels
 * start. The pixels are the read buffer's from the window's lower-left
 * corner, rows bottom first, each row packed by the pack row length and
 * alignment 0x33 set (4.3.2). A pixel outside the window or the screen
 * reads 0; no alpha plane is modelled, so alpha is 1.0. The model packs
 * GL_RGBA, GL_RGB and GL_ABGR_EXT in GL_UNSIGNED_BYTE and logs any other
 * pair, answering with word 0 alone so the client does not wait forever.
 * A read larger than the page goes by DMA request 0x2710, which is not
 * modelled; what the page cannot hold is logged and dropped.
 */
static void sgi_kona_gl_read_pixels(SGIKonaState *s, const uint32_t *args)
{
    int i = s->context_current;
    int32_t x0 = args[0], y0 = args[1];
    uint32_t w = args[2], h = args[3], format = args[4], type = args[5];
    uint32_t start = 8 + (args[6] & 3);
    uint32_t comps = format == SGI_GL_RGB ? 3 : 4;
    const SGIKonaGLPipe *p;
    uint64_t row, stride, len;
    g_autofree uint8_t *data = NULL;
    int bank;

    if (i == SGI_KONA_CONTEXT_NONE) {
        sgi_kona_post_return(s, SGI_KONA_CMD_GL_READ_PIXELS, 0, NULL, 0);
        return;
    }
    p = &s->gl_pipe[i];
    if (type != 0x1401 || (format != SGI_GL_RGBA && format != SGI_GL_RGB &&
                           format != 0x8000)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels of format 0x%"
                      PRIx32 " type 0x%" PRIx32 " is not modelled; no"
                      " pixels returned\n", format, type);
        sgi_kona_post_return(s, SGI_KONA_CMD_GL_READ_PIXELS, 0, NULL, 0);
        return;
    }
    row = (uint64_t)(p->pack_row_length ? p->pack_row_length : w) * comps;
    stride = ROUND_UP(row, MAX(p->pack_alignment, 1));
    len = h ? stride * (h - 1) + (uint64_t)w * comps : 0;
    if (len > SGI_KONA_RETURN_PAGE_BYTES - start) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glReadPixels of %" PRIu32 "x%"
                      PRIu32 " needs 0x%" PRIx64 " bytes, more than the"
                      " return page holds; the rest is dropped\n", w, h,
                      len);
        len = SGI_KONA_RETURN_PAGE_BYTES - start;
    }
    data = g_malloc0(len);
    bank = sgi_kona_gl_read_bank(s, i);
    for (uint64_t r = 0; r < h; r++) {
        for (uint64_t c = 0; c < w; c++) {
            uint64_t at = r * stride + c * comps;
            int64_t px = (int64_t)s->gl_window[i].x + x0 + c;
            int64_t py = (int64_t)s->gl_window[i].y + y0 + r;
            uint32_t rgb = 0;
            uint8_t *out;

            if (at + comps > len) {
                break;
            }
            if (bank >= 0 && px >= 0 && py >= 0 &&
                px < SGI_KONA_FB_WIDTH && py < SGI_KONA_FB_HEIGHT &&
                (uint64_t)(x0 + c) < s->gl_window[i].width &&
                (uint64_t)(y0 + r) < s->gl_window[i].height) {
                rgb = sgi_kona_bank_read(s, bank, px, py);
            }
            out = data + at;
            if (format == 0x8000) {
                out[0] = 0xff;
                out[1] = rgb;
                out[2] = rgb >> 8;
                out[3] = rgb >> 16;
                continue;
            }
            out[0] = rgb >> 16;
            out[1] = rgb >> 8;
            out[2] = rgb;
            if (comps == 4) {
                out[3] = 0xff;
            }
        }
    }
    trace_sgi_kona_gl_reply(SGI_KONA_CMD_GL_READ_PIXELS, format, len);
    sgi_kona_post_return(s, SGI_KONA_CMD_GL_READ_PIXELS, start, data, len);
}

/*
 * 0x4c and a name, glGet: the client waits on word 0 of the return page
 * and reads the values from byte 8, converting them by a type its own
 * table holds (established from the reader). The model answers from the
 * shared context, a float as its bits and an integer or enumerant as
 * itself, big-endian words; that each name's table type matches is
 * inferred, and settled for a name only by a trace of its reply. A name
 * the shared context does not keep is logged and answered with word 0
 * alone, so the client reads what the page held before.
 */
static void sgi_kona_gl_get(SGIKonaState *s, uint32_t name)
{
    uint32_t out[SGI_GL_GET_WORDS];
    uint8_t bytes[SGI_GL_GET_WORDS * 4];
    unsigned n = 0;

    if (s->context_current == SGI_KONA_CONTEXT_NONE ||
        !sgi_gl_get(&s->gl[s->context_current], name, out, &n)) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glGet of 0x%" PRIx32 " is not"
                      " answered; word 0 posted alone\n", name);
        sgi_kona_post_return(s, SGI_KONA_CMD_GL_GET, 0, NULL, 0);
        return;
    }
    for (unsigned k = 0; k < n; k++) {
        stl_be_p(bytes + 4 * k, out[k]);
    }
    trace_sgi_kona_gl_reply(SGI_KONA_CMD_GL_GET, name, 4 * n);
    sgi_kona_post_return(s, SGI_KONA_CMD_GL_GET, 8, bytes, 4 * n);
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
#define SGI_KONA_PUSH_TEXTURE_WORDS  121

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
 *   0x40000 the texture state, which no model state fills: zeros
 *
 * The 0x1000 section's length depends on a library word the model cannot
 * see, so it is not sent and is logged. Returns the reply's bytes.
 */
static uint32_t sgi_kona_gl_push_reply(SGIKonaState *s, int i, uint32_t mask,
                                       uint8_t *w, uint32_t room)
{
    const SGIGLContext *c = &s->gl[i];
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
    if (mask & 0x40000) {
        used += 4 * SGI_KONA_PUSH_TEXTURE_WORDS;
    }
    if (mask & 0x1000) {
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glPushAttrib's transform"
                      " section is not sent; its length is unknown\n");
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
                       SGI_KONA_PUSH_OFFSET_WORDS + SGI_KONA_GL_STIPPLE_ROWS +
                       SGI_KONA_PUSH_TEXTURE_WORDS)];
    uint32_t len;
    uint32_t *depth;

    if (i == SGI_KONA_CONTEXT_NONE) {
        sgi_kona_post_return(s, SGI_KONA_CMD_GL_PUSH_ATTRIB, 0, NULL, 0);
        return;
    }
    len = sgi_kona_gl_push_reply(s, i, mask, reply, sizeof(reply));
    depth = &s->gl_attrib_depth[i];
    if (*depth >= SGI_KONA_GL_ATTRIB_DEPTH) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: glPushAttrib on a full"
                      " attribute stack; nothing kept\n");
    } else if (sgi_gl_clone(&s->gl_attrib[i * SGI_KONA_GL_ATTRIB_DEPTH +
                                          *depth], &s->gl[i])) {
        s->gl_attrib_mask[i * SGI_KONA_GL_ATTRIB_DEPTH + *depth] = mask;
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
        SGI_KONA_RESTORE(cur.texture_2d);
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
    if (!s->gl_attrib_depth[i] || s->gl[i].in_begin) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-kona: glPopAttrib with %s;"
                      " ignored\n", s->gl[i].in_begin ? "a primitive open" :
                      "an empty attribute stack");
        return;
    }
    k = i * SGI_KONA_GL_ATTRIB_DEPTH + --s->gl_attrib_depth[i];
    sgi_kona_gl_restore(&s->gl[i], &s->gl_attrib[k], s->gl_attrib_mask[k]);
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
            sgi_gl_command(&s->gl[i], &sink,
                           &(SGIGLCmd) { SGI_GL_OP_LIGHT, 6, a });
        }
    }
}

static void sgi_kona_x_box_data_drop(SGIKonaState *s, uint32_t next);
static void sgi_kona_x_clip_run_drop(SGIKonaState *s, uint32_t next);

/* The GL command table's entry for a 16-bit selector, or NULL. */
static const SGICommandPortSelector *sgi_kona_gl_token(uint32_t selector)
{
    for (unsigned i = 0; i < sgi_kona_gl_num_tokens; i++) {
        if (sgi_kona_gl_tokens[i].selector == selector) {
            return &sgi_kona_gl_tokens[i];
        }
    }
    return NULL;
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
    return sgi_kona_gl_token(low) != NULL;
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
static void sgi_kona_gl_data_open(SGIKonaState *s, uint32_t token,
                                  uint32_t words)
{
    s->gl_data_token = token;
    s->gl_data_remaining = words;
    s->gl_data_have = 0;
    trace_sgi_kona_gl_data(token, words);
}

static void sgi_kona_gl_data_done(SGIKonaState *s);

static void sgi_kona_gl_data_word(SGIKonaState *s, uint32_t word)
{
    if (s->gl_data_have < SGI_KONA_GL_DATA_MAX) {
        s->gl_data[s->gl_data_have] = word;
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
 * The words of a texel load's image rows on the page, from its header, or
 * 0 with a log when the header does not say.
 *
 * Established: the rows follow the 20 header words, each row as the whole
 * words covering it from its word-aligned start. Inferred: header words 1
 * and 2 are the image's width and height, words 3 and 4 its format and
 * type (the record fields the readback also sends), word 5 the low two
 * bits of the source address, and word 6 nonzero when the image goes by
 * DMA instead, with no rows on the page. Every row then starts at the
 * same two low bits only when the unpack row stride is a whole number of
 * words, which the model requires. A side above SGI_KONA_GL_IMAGE_MAX,
 * the model's own bound and no known limit of the pipe, is taken as a
 * misread header.
 */
#define SGI_KONA_GL_IMAGE_MAX 8192

static uint32_t sgi_kona_gl_tex_load_words(SGIKonaState *s, int i,
                                           const uint32_t *h)
{
    const SGIKonaGLPipe *p = &s->gl_pipe[i];
    uint32_t bytes = sgi_kona_gl_pixel_bytes(h[3], h[4]);
    uint64_t row = (uint64_t)(p->unpack_row_length ? p->unpack_row_length :
                              h[1]) * bytes;
    uint64_t stride = ROUND_UP(row, MAX(p->unpack_alignment, 1));
    uint64_t words;

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
 * uses for the images it takes and returns (GL_UNPACK_ROW_LENGTH,
 * GL_UNPACK_ALIGNMENT and their pack forms). The library sends the state
 * and converts no image, so the pipe holds it (inferred). Other
 * parameters are kept by no part of the model and logged.
 */
static void sgi_kona_gl_pixel_store(SGIKonaState *s, int i, uint32_t pname,
                                    uint32_t value)
{
    SGIKonaGLPipe *p = &s->gl_pipe[i];

    switch (pname) {
    case 0x0cf2:                /* GL_UNPACK_ROW_LENGTH */
        p->unpack_row_length = value;
        break;
    case 0x0cf5:                /* GL_UNPACK_ALIGNMENT */
        p->unpack_alignment = value;
        break;
    case 0x0d02:                /* GL_PACK_ROW_LENGTH */
        p->pack_row_length = value;
        break;
    case 0x0d05:                /* GL_PACK_ALIGNMENT */
        p->pack_alignment = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glPixelStore of 0x%" PRIx32
                      " to %" PRIu32 " is not modelled\n", pname, value);
    }
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
static void sgi_kona_gl_packet(void *opaque, unsigned port_id,
                               uint32_t selector, const uint32_t *args,
                               unsigned argc)
{
    SGIKonaState *s = opaque;
    int i = s->context_current;
    uint32_t words;

    sgi_kona_port_consumed(s, 1 + argc);
    trace_sgi_kona_kernel_command(selector, args[0], argc > 1 ? args[1] : 0,
                                  argc > 2 ? args[2] : 0, argc);
    switch (selector) {
    case SGI_KONA_CMD_GL_BUFFER_CONFIG:
    case SGI_KONA_CMD_GL_READ_CONFIG:
        trace_sgi_kona_gl_buffer_config(selector, args[0], args[1], args[2],
                                        args[SGI_KONA_GL_BUFFER_CONFIG_DOUBLE]);
        if (selector == SGI_KONA_CMD_GL_BUFFER_CONFIG) {
            sgi_kona_gl_buffer_config(s, args);
        } else {
            trace_sgi_kona_command_unmodelled(selector, argc);
        }
        return;
    case SGI_KONA_CMD_GL_TEX_TABLE:
        sgi_kona_gl_tex_table_open(s, args[0]);
        return;
    case SGI_KONA_CMD_GL_LINE_DISTANCE:
        sgi_kona_gl_line_distance(s);
        return;
    case SGI_KONA_CMD_GL_GET_ERROR:
        sgi_kona_gl_get_error(s);
        return;
    case SGI_KONA_CMD_GL_FINISH:
        sgi_kona_gl_finish(s, args[0]);
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
    case SGI_KONA_CMD_GL_TEX_LOAD:
        words = i == SGI_KONA_CONTEXT_NONE ? 0 :
                sgi_kona_gl_tex_load_words(s, i, args);
        if (words) {
            sgi_kona_gl_data_open(s, selector, words);
        }
        return;
    case SGI_KONA_CMD_GL_DRAW_PIXELS:
        /*
         * Which of the eight header words give the image's size, format
         * and type is unknown, so its rows cannot be counted: they reach
         * the page as words of their own.
         */
        qemu_log_mask(LOG_UNIMP, "sgi-kona: glDrawPixels header framed;"
                      " its rows are not\n");
        return;
    case SGI_KONA_CMD_GL_PIXEL_STORE:
        if (i != SGI_KONA_CONTEXT_NONE) {
            sgi_kona_gl_pixel_store(s, i, args[0], args[1]);
        }
        return;
    case SGI_KONA_CMD_GL_READ_BUFFER:
        if (i != SGI_KONA_CONTEXT_NONE) {
            s->gl_pipe[i].read_buffer = args[0];
        }
        return;
    case SGI_KONA_CMD_GL_READ_PIXELS:
        sgi_kona_gl_read_pixels(s, args);
        return;
    case SGI_KONA_CMD_GL_GET:
        sgi_kona_gl_get(s, args[0]);
        return;
    case SGI_KONA_CMD_GL_PUSH_ATTRIB:
        sgi_kona_gl_push_attrib(s, args[0]);
        return;
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
    trace_sgi_kona_command_unmodelled(s->gl_data_token, s->gl_data_have);
    s->gl_data_token = 0;
    s->gl_data_have = 0;
}

static const SGICommandPortOps sgi_kona_gl_ops = {
    .packet = sgi_kona_gl_packet,
};

/*
 * fg_h in the one-bit form kon_loadBitmapGC sends: kon_parseGC sets it to
 * this constant in the colour-index case, and the foreground travels in
 * word 1 instead (established). No solid fg_h below can equal
 * it: that packing never sets bit 7.
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

    return v >> (16 - SGI_KONA_X_INDEX_DEPTH);
}

/*
 * The colour index a colour-index window's GC fills with. kon_loadGeomGC
 * packs v = index << (16 - depth) into fg_h as ((v & 0xf) << 3) |
 * ((v & 0xfff0) << 4) (established); at depth 8 that is index << 12. A GC
 * in the one-bit form carries its foreground in word 1, see
 * sgi_kona_x_packed_index().
 *
 * Approximated: no traced fill follows a one-bit-form GC, so what such a
 * fill paints is unknown. The model paints the foreground word 1 carries,
 * rather than decoding 0xfff0 as the solid form, which would give index
 * 0xf.
 */
static uint32_t sgi_kona_x_gc_index(const uint32_t *gc)
{
    uint32_t fg_h = gc[0];
    uint32_t v = ((fg_h >> 3) & 0xf) | ((fg_h >> 4) & 0xfff0);

    if (fg_h == SGI_KONA_X_GC_BITMAP_FG_H) {
        return sgi_kona_x_packed_index(gc[1]);
    }
    return v >> (16 - SGI_KONA_X_INDEX_DEPTH);
}

/*
 * 0x129, X load GC: fg_h, fg_l, bg_h, bg_l, wmask0, wmask1, cfg, op, addr,
 * bl, ifmt, colfmt, pixfmt, in that order (kon_loadGC's debug strings).
 * The model keeps all 13 and fills with the colour index
 * sgi_kona_x_gc_index() reads from them.
 *
 * Approximated: the model always reads fg_h in the colour-index form at
 * SGI_KONA_X_INDEX_DEPTH, or the one-bit form when fg_h is
 * SGI_KONA_X_GC_BITMAP_FG_H. The window's format word, which picks the
 * colour-index form over the two RGB forms, reaches no command; the stock
 * greeter's fills are colour-index at depth 8. fg_l is kept and not read:
 * the colour-index form's is 0x1ffe, and what the pipe makes of any other
 * value (the run also sent 0, 0x800 and 0x9000) is unknown, so a fill
 * with another fg_l still takes fg_h's index. Write masks, cfg and op,
 * whose bits are unknown, are not applied. The discriminating test is a
 * guest fill whose colour-map entry appears on the scanout: (340, 20) of
 * the greeter resolving to 213, 213, 213 grey and (270, 310) to 153, 153,
 * 153 grey.
 */
static void sgi_kona_x_load_gc(SGIKonaState *s, const uint32_t *args)
{
    memcpy(s->x_gc, args, sizeof(s->x_gc));
    trace_sgi_kona_x_gc(s->x_gc[0], sgi_kona_x_gc_index(s->x_gc));
}

/*
 * 0x12a, X load position: the target drawable's x, y, width and height
 * (kon_loadGC). It is the origin of X rect coordinates, see
 * sgi_kona_x_fill_rect(). Box and pixel-write coordinates arrive absolute,
 * every sender of them setting the position to 0, 0 first, so the model
 * never adds it to those.
 */
static void sgi_kona_x_load_position(SGIKonaState *s, const uint32_t *args)
{
    memcpy(s->x_position, args, sizeof(s->x_position));
    trace_sgi_kona_x_position(args[0], args[1], args[2], args[3]);
}

/* A clip list's words ended by something other than X load clip. */
static void sgi_kona_x_clip_run_drop(SGIKonaState *s, uint32_t next)
{
    if (s->x_clip_run_have) {
        trace_sgi_kona_x_clip_run_dropped(s->x_clip_run_have, next);
        s->x_clip_run_have = 0;
    }
}

/*
 * 0x145, X bdata4: the leading words of a clip list longer than one
 * rectangle (kon_loadClipRects), kept until the X load clip ending it.
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
    memcpy(s->x_clip_run + s->x_clip_run_have, args,
           SGI_KONA_X_BDATA4_WORDS * sizeof(*args));
    s->x_clip_run_have += SGI_KONA_X_BDATA4_WORDS;
}

/*
 * 0x12b, X load clip: the end of a clip list, a rectangle count n and n
 * rectangles x1, y1, x2, y2, the first words carried by any X bdata4 before
 * it (kon_loadClipRects). Inferred: each rectangle is absolute with an
 * exclusive far corner, and drawing is clipped to their union. A list whose
 * length does not match its count, including a count of 0, is logged and
 * leaves drawing clipped only to the screen, since what the pipe makes of
 * one is unknown.
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
 * 0x12d, X load scissor: the clip box's x1, y1, width and height, which
 * konPolyGlyphBltSolid, konImageGlyphBlt and the fixed-width glyph senders
 * send right after a one-rectangle X load clip with the same box
 * (established). The model applies it as a second rectangle
 * with the clip, to the pixel writes, copies and fills after it, see
 * sgi_kona_x_clip(). A scissor of zero width or height clips everything,
 * logged.
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
 * height, then `height` rows, one word each, most significant bit
 * leftmost (kon_loadCachedStipple, established for the senders; the bit
 * order is inferred from a traced run's 6 by 20 I-beam). The
 * rows arrive through sgi_kona_x_stipple_word().
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
 * and span fills that follow (konPolyFillRectStippledC,
 * konFillSpansStippledC), or SGI_KONA_X_STIPPLE_NONE, which ends the masking
 * (kon_loadStipple, kon_unloadStipple; both uses are inferred). An entry
 * outside the cache is logged and unloads the stipple; an entry holding no
 * pattern is logged, and the fills are not masked while it holds none.
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
    r->x2 = SGI_KONA_FB_WIDTH;
    r->y2 = SGI_KONA_FB_HEIGHT;
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
 * Fill [x1, x2) by [y1, y2), clipped to the screen and the X clip and
 * masked by the loaded stipple, into the front colour bank with the GC's
 * colour index, tagged as an X pixel, and mark the lines.
 *
 * Approximated: y counts down from the top display line, the X server's
 * convention, where the textport's wire y counts up; the stock guest's
 * first box covers the screen either way, and a guest fill of a box away
 * from the centre would tell the orientation. The model draws into the
 * front bank only, with no logic op or write mask.
 */
static void sgi_kona_x_fill(SGIKonaState *s, int64_t x1, int64_t y1,
                            int64_t x2, int64_t y2)
{
    const SGIPlane *front = sgi_kona_plane(s, s->plane_front);
    const SGIPlane *tag = sgi_kona_plane(s, s->plane_front_tag);
    uint32_t index = sgi_kona_x_gc_index(s->x_gc);
    SGIKonaRect clip;

    sgi_kona_x_clip(s, &clip);
    x1 = MAX(x1, clip.x1);
    y1 = MAX(y1, clip.y1);
    x2 = MIN(x2, clip.x2);
    y2 = MIN(y2, clip.y2);
    if (x1 >= x2 || y1 >= y2) {
        return;
    }
    for (int y = y1; y < y2; y++) {
        /* The front plane is bottom-origin: display line y is this row. */
        int row = SGI_KONA_FB_HEIGHT - 1 - y;

        for (int x = x1; x < x2; x++) {
            if (!sgi_kona_x_clip_contains(s, x, y) ||
                !sgi_kona_x_stipple_bit(s, x, y)) {
                continue;
            }
            sgi_plane_store(&s->mem, front, x, row, index);
            sgi_plane_store(&s->mem, tag, x, row, SGI_KONA_TAG_X_INDEX);
        }
    }
    sgi_scanout_invalidate_plane(&s->scanout, front,
                                 SGI_KONA_FB_HEIGHT - y2,
                                 SGI_KONA_FB_HEIGHT - 1 - y1);
}

/*
 * One X box, the words w0 = x1 << 16 | y1 and w1 = x2 << 16 | y2 with signed
 * 16-bit halves and an exclusive far corner, in absolute screen coordinates
 * (established from the senders). The X load position is not added: every
 * box sender sets it to 0, 0 first.
 */
static void sgi_kona_x_fill_box(SGIKonaState *s, uint32_t w0, uint32_t w1)
{
    int x1 = (int16_t)(w0 >> 16);
    int y1 = (int16_t)w0;
    int x2 = (int16_t)(w1 >> 16);
    int y2 = (int16_t)w1;

    trace_sgi_kona_x_box(x1, y1, x2, y2, sgi_kona_x_gc_index(s->x_gc));
    sgi_kona_x_fill(s, x1, y1, x2, y2);
}

/*
 * One X rect, the words x << 16 | y with signed halves and width << 16 |
 * height with unsigned ones, the X protocol's rectangle, relative to the
 * drawable the X load position names (konPolyFillRectSolid, established).
 * Inferred: the pipe adds the position's x and y, then clips to the clip
 * list. The guest check is the greeter button's face (244, 299)-(308, 323)
 * taking the GC's colour after a position of 240, 295.
 */
static void sgi_kona_x_fill_rect(SGIKonaState *s, uint32_t w0, uint32_t w1)
{
    int64_t x = (int16_t)(w0 >> 16) + (int64_t)(int32_t)s->x_position[0];
    int64_t y = (int16_t)w0 + (int64_t)(int32_t)s->x_position[1];
    uint32_t width = w1 >> 16;
    uint32_t height = w1 & 0xffff;

    trace_sgi_kona_x_rect(x, y, width, height,
                          sgi_kona_x_gc_index(s->x_gc));
    sgi_kona_x_fill(s, x, y, x + width, y + height);
}

/*
 * 0x156, 0x157 and 0x158, X spans, spans2 and spans6: a count of 1, 2 or
 * 6, then that many points x << 16 | y with signed 16-bit halves, then
 * that many widths (konFillSpansSolid, established). Each
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
    for (uint32_t i = 0; i < n; i++) {
        int x = (int16_t)(args[1 + i] >> 16);
        int y = (int16_t)args[1 + i];
        uint32_t width = args[1 + n + i];

        trace_sgi_kona_x_span(x, y, width, index);
        sgi_kona_x_fill(s, x, y, (int64_t)x + width, (int64_t)y + 1);
    }
}

/*
 * The words of a run of data tokens (0x142, 0x144, 0x147, 0x149, 0x14b),
 * kept until the token that ends the run. The senders send the last box of
 * a run as X box 0x154, or the last rectangle as X rect 0x152, after one
 * data token carrying the ones before it, so the ending token says how
 * the run's words read. The model draws a run when an X box, box6, rect or
 * rect6 ends it, and drops it, traced, when anything else does, a kernel
 * packet the kernel interleaves with the X server's included. Whether a
 * real pipe draws a data token's words on their own, or keeps a run across
 * a kernel packet, is unknown; every box or rectangle of a run gives the
 * same pixels either way.
 */
static void sgi_kona_x_box_data(SGIKonaState *s, uint32_t token,
                                const uint32_t *args, unsigned argc)
{
    trace_sgi_kona_x_box_data(token, argc);
    if (s->x_boxes_have + argc > SGI_KONA_X_BOX_RUN_WORDS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X data token 0x%" PRIx32 " makes a run of"
                      " more than %u box words; run dropped\n", token,
                      SGI_KONA_X_BOX_RUN_WORDS);
        trace_sgi_kona_x_box_data_dropped(s->x_boxes_have + argc, token);
        s->x_boxes_have = 0;
        return;
    }
    memcpy(s->x_boxes + s->x_boxes_have, args, argc * sizeof(*args));
    s->x_boxes_have += argc;
}

/* A run of data tokens ended by something other than an X box or rect. */
static void sgi_kona_x_box_data_drop(SGIKonaState *s, uint32_t next)
{
    if (s->x_boxes_have) {
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
    void (*fill)(SGIKonaState *, uint32_t, uint32_t) =
        rects ? sgi_kona_x_fill_rect : sgi_kona_x_fill_box;

    for (uint32_t i = 0; i + 1 < s->x_boxes_have; i += 2) {
        fill(s, s->x_boxes[i], s->x_boxes[i + 1]);
    }
    s->x_boxes_have = 0;
    for (unsigned i = 0; i + 1 < argc; i += 2) {
        fill(s, args[i], args[i + 1]);
    }
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

/* Mode word bit konDrawImage sets when the rows go by ioctl 0x2711. */
#define SGI_KONA_X_SETUP_MODE_DMA 0x10

/*
 * 0x136, X pixel setup: kept for the pixel write or copy after it
 * (established from kon_loadPixmapGC). The packing and shift words are table
 * entries whose meaning to the pipe is unknown; the model reads only the
 * source packing, for the bits per pixel, and applies no shift or
 * destination packing.
 */
static void sgi_kona_x_pixel_setup(SGIKonaState *s, const uint32_t *args)
{
    memcpy(s->x_pixel_setup, args, sizeof(s->x_pixel_setup));
    trace_sgi_kona_x_pixel_setup(args[0], args[1], args[2], args[3], args[4],
                                 args[5], args[6]);
}

/* The setup's source-packing words for the two one-bit forms. */
#define SGI_KONA_X_PACK_BITMAP        4
#define SGI_KONA_X_PACK_OPAQUE_BITMAP 5

/*
 * The bits per pixel of a write's data, from the setup's source-packing
 * word, or 0 where it is not known. Inferred from kona.so's table names
 * (kon_pack8src, kon_pack16src, kon_pack32src hold 0, 1, 2 at index 0):
 * 0, 1 and 2 are 8, 16 and 32 bits. 4 is the glyph senders' constant, and
 * the greeter's glyph writes are established to be 1-bit rows (20 words
 * for a 32 by 20 image spelling "Help"). 5 is konDrawOpaqueMonoImage's,
 * inferred to be one bit a pixel as well. See sgi_kona_x_bitmap_index()
 * for what their bits paint.
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
 *   source packing 4 (the glyph senders, konDrawMonoImage): GC word 1 is
 *   P(v) = ((v >> 4) & 0xfff) | ((v & 0xf) << 20), so a set bit paints
 *   (((w1 & 0xfff) << 4) | ((w1 >> 20) & 0xf)) >> (16 - d), and a clear
 *   bit nothing. Word 0 is the constant 0xfff0 and is not read.
 *
 *   source packing 5 (konDrawOpaqueMonoImage): a set bit paints GC word
 *   0 read as v, a clear bit GC word 2 read as v.
 *
 * Approximated: which words each sender fills is established, and what
 * the pipe paints from them is inferred from that alone; the pipe was not
 * observed. The depth is SGI_KONA_X_INDEX_DEPTH, as for the fills. The
 * discriminating tests are the glyph qtests for one-bit glyph colour and
 * opaque mono images, and in the guest the greeter's button labels
 * appearing, black through entry 0x2d00 under the run's foreground of 0.
 */
static bool sgi_kona_x_bitmap_index(SGIKonaState *s, bool bit,
                                    uint32_t *index)
{
    uint32_t shift = 16 - SGI_KONA_X_INDEX_DEPTH;

    if (s->x_pixel_setup[SGI_KONA_X_SETUP_SRC_PACK] ==
        SGI_KONA_X_PACK_OPAQUE_BITMAP) {
        *index = (bit ? s->x_gc[0] : s->x_gc[2]) >> shift;
        return true;
    }
    *index = sgi_kona_x_packed_index(s->x_gc[1]);
    return bit;
}

/*
 * 0x139, X pixel write: destination x and y, then, unless the setup's mode
 * word marks the DMA path, height rows of ceil((pad + bpp * width) / 32)
 * big-endian words, the first pad bits of each row skipped ("Pixel writes
 * and copies", established from konDrawImage). Both paths end with X pixel
 * finish. The words arrive through sgi_kona_x_pixel_word().
 *
 * Approximated: every pixel lands in the front bank at (x + column,
 * y + row), y counting down as for boxes, clipped to the screen and the X
 * clip. An 8-bit pixel is an X colour-map index; a 32-bit pixel is direct
 * colour; a 16-bit pixel's packing is unknown and the model keeps its low
 * byte as an index. The bank a write lands in and whether the shift words
 * apply are unknown. Under the DMA mode bit the same rows arrive by the
 * kernel's DMA packet instead (sgi_kona_x_image_dma()), and command-page
 * words do not feed the write meanwhile. The discriminating
 * guest test is the X server's root weave appearing with its pixels at
 * x = 3, 1, 2, 0 of rows 0 to 3. A one-bit pixel paints the colour index
 * sgi_kona_x_bitmap_index() gives, or leaves its pixel alone.
 */
static void sgi_kona_x_pixel_write(SGIKonaState *s, const uint32_t *args)
{
    uint32_t src_pack = s->x_pixel_setup[SGI_KONA_X_SETUP_SRC_PACK];
    uint64_t width = s->x_pixel_setup[SGI_KONA_X_SETUP_WIDTH];
    uint64_t height = s->x_pixel_setup[SGI_KONA_X_SETUP_HEIGHT];
    uint64_t pad = s->x_pixel_setup[SGI_KONA_X_SETUP_PAD];
    uint32_t bpp = sgi_kona_x_pixel_bpp(src_pack);
    uint64_t stride = 0, words = 0;

    if (s->x_pixel_open) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X pixel write while another is open;"
                      " the open one ends\n");
    }
    s->x_pixel_open = true;
    s->x_pixel_x = args[0];
    s->x_pixel_y = args[1];
    s->x_pixel_bpp = bpp;
    s->x_pixel_prev = 0;

    if (!bpp) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-kona: X pixel write with source packing %" PRIu32
                      "; its data cannot be framed\n", src_pack);
    } else {
        stride = DIV_ROUND_UP(pad + bpp * width, 32);
        words = stride * height;
        if (words > UINT32_MAX) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: X pixel write of %" PRIu64 " words;"
                          " its data cannot be framed\n", words);
            stride = words = 0;
        }
    }
    s->x_pixel_stride = stride;
    s->x_pixel_words = words;
    s->x_pixel_remaining = words;
    trace_sgi_kona_x_pixel_write(s->x_pixel_x, s->x_pixel_y, bpp, words);
    if (bpp == 1) {
        uint32_t set, clear;
        bool opaque = sgi_kona_x_bitmap_index(s, false, &clear);

        sgi_kona_x_bitmap_index(s, true, &set);
        trace_sgi_kona_x_pixel_bitmap(src_pack, set, opaque, clear);
    }
}

/* One pixel of a write, at column i of data row `row`. */
static void sgi_kona_x_pixel_store(SGIKonaState *s, const SGIKonaRect *clip,
                                   uint64_t i, uint64_t row, uint32_t value)
{
    int64_t x = (int64_t)s->x_pixel_x + i;
    int64_t y = (int64_t)s->x_pixel_y + row;
    int plane_row = SGI_KONA_FB_HEIGHT - 1 - y;
    uint32_t tag = SGI_KONA_TAG_X_INDEX;

    if (x < clip->x1 || x >= clip->x2 || y < clip->y1 || y >= clip->y2 ||
        !sgi_kona_x_clip_contains(s, x, y)) {
        return;
    }
    switch (s->x_pixel_bpp) {
    case 1:
        if (!sgi_kona_x_bitmap_index(s, value, &value)) {
            return;
        }
        break;
    case 16:
        value &= 0xff;
        break;
    case 32:
        tag = SGI_KONA_TAG_DIRECT;
        break;
    }
    sgi_plane_store(&s->mem, sgi_kona_plane(s, s->plane_front), x, plane_row,
                    value);
    sgi_plane_store(&s->mem, sgi_kona_plane(s, s->plane_front_tag), x,
                    plane_row, tag);
    sgi_scanout_invalidate_plane(&s->scanout,
                                 sgi_kona_plane(s, s->plane_front),
                                 plane_row, plane_row);
}

/*
 * A data word of an open pixel write: word w of its row holds row bits
 * 32w to 32w + 31, most significant first, and pixel i occupies bits
 * pad + bpp * i onward. Each pixel ending in this word is stored; one
 * starting in the previous word takes its high bits from x_pixel_prev.
 */
static void sgi_kona_x_pixel_word(SGIKonaState *s, uint32_t word)
{
    uint64_t k = s->x_pixel_words - s->x_pixel_remaining;
    uint64_t row = k / s->x_pixel_stride;
    uint64_t w = k % s->x_pixel_stride;
    uint64_t pad = s->x_pixel_setup[SGI_KONA_X_SETUP_PAD];
    uint64_t width = s->x_pixel_setup[SGI_KONA_X_SETUP_WIDTH];
    uint32_t bpp = s->x_pixel_bpp;

    if (w == 0) {
        s->x_pixel_prev = 0;
    }
    if (bpp) {
        uint64_t window = ((uint64_t)s->x_pixel_prev << 32) | word;
        uint64_t mask = bpp == 32 ? UINT32_MAX : (1u << bpp) - 1;
        uint64_t first = 32 * w;
        uint64_t i = first > pad ? (first - pad) / bpp : 0;
        SGIKonaRect clip;

        sgi_kona_x_clip(s, &clip);
        for (; i < width; i++) {
            uint64_t end = pad + bpp * (i + 1);

            if (end > first + 32) {
                break;
            }
            if (end > first) {
                sgi_kona_x_pixel_store(s, &clip, i, row,
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
 * Whether the open X pixel write waits for its rows by DMA: the setup's
 * mode word carries SGI_KONA_X_SETUP_MODE_DMA, the one word that says so
 * (established from konDrawImage), and rows are still to come.
 */
bool sgi_kona_x_pixel_dma_waiting(SGIKonaState *s)
{
    return s->x_pixel_open && s->x_pixel_remaining &&
           (s->x_pixel_setup[SGI_KONA_X_SETUP_MODE] &
            SGI_KONA_X_SETUP_MODE_DMA);
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
        uint32_t chunk = MIN(len, page - (device & (page - 1)));
        uint64_t host;
        hwaddr where;

        if ((entry & SGI_KONA_MAPRAM_KIND_MASK) != SGI_KONA_MAPRAM_KIND_16K) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-kona: DMA read at device address 0x%" PRIx64
                          " through map RAM entry 0x%08x, which maps"
                          " nothing\n", device, entry);
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
            return false;
        }
        device += chunk;
        buf += chunk;
        len -= chunk;
    }
    return true;
}

/*
 * The X server's image write by DMA (konDrawImage): a DMA packet that
 * arrives while an X pixel write opened under the DMA mode bit waits for
 * its rows. The packet's lines of per_line bytes, pitch bytes apart from
 * device address io, are the rows the programmed-I/O path would have sent
 * as data words, big-endian, top row first (inferred: the byte count is
 * exactly that path's words times four, built from the same setup). The
 * per-line count and gap only say how the bytes lie in host memory; the
 * image's shape is the setup's. So the bytes go four at a time into the
 * open write, which draws them as it would its page words. Bytes beyond
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
    g_autofree uint8_t *line = g_malloc(per_line);
    uint64_t extra = 0;

    trace_sgi_kona_x_image_dma(io, lines, per_line, s->x_pixel_remaining);
    for (uint32_t l = 0; l < lines; l++) {
        if (!sgi_kona_device_read(s, io + l * pitch, line, per_line)) {
            return false;
        }
        for (uint32_t b = 0; b < per_line; b += 4) {
            if (!s->x_pixel_remaining) {
                extra += 4;
                continue;
            }
            sgi_kona_x_pixel_word(s, ldl_be_p(line + b));
        }
    }
    if (extra) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X image DMA carried %" PRIu64 " bytes past"
                      " its rows; not drawn\n", extra);
    }
    return true;
}

/*
 * 0x137, X pixel copy: source x, y, then destination x, y, copying the
 * setup's width by height (the direction is established by konCopyRect's
 * debug string).
 * The model copies within the front bank, each pixel's word and tag
 * together, so a pixel keeps how it resolves. The destination is clipped
 * to the screen, the clip list and the scissor, and a pixel whose source
 * is off the screen is not copied. Both points are taken as absolute: the
 * X load position is not added. konCopyRect sets it to 0, 0 first, so
 * its copies cannot tell. The unnamed routine at 0x4817f0 sends a
 * position and never a clip before its copies; a trace of it with a
 * non-zero position would tell whether the pipe adds the position to a
 * copy.
 *
 * Approximated: copying as if through a temporary, so an overlapping copy
 * reads every source pixel before it is overwritten, is the model's
 * choice; no copy in the traced runs overlaps its source. The shift and
 * packing words are not applied. The discriminating guest test is the
 * weave tiled across the screen after the run's 17 copies.
 */
static void sgi_kona_x_pixel_copy(SGIKonaState *s, const uint32_t *args)
{
    const SGIPlane *front = sgi_kona_plane(s, s->plane_front);
    const SGIPlane *tag = sgi_kona_plane(s, s->plane_front_tag);
    int64_t sx = (int32_t)args[0], sy = (int32_t)args[1];
    int64_t dx = (int32_t)args[2], dy = (int32_t)args[3];
    int64_t w = s->x_pixel_setup[SGI_KONA_X_SETUP_WIDTH];
    int64_t h = s->x_pixel_setup[SGI_KONA_X_SETUP_HEIGHT];
    int64_t i0, i1, j0, j1;
    SGIKonaRect clip;

    trace_sgi_kona_x_pixel_copy(sx, sy, dx, dy, w, h);
    sgi_kona_x_clip(s, &clip);
    i0 = MAX(MAX(0, -sx), clip.x1 - dx);
    i1 = MIN(MIN(w, SGI_KONA_FB_WIDTH - sx), clip.x2 - dx);
    j0 = MAX(MAX(0, -sy), clip.y1 - dy);
    j1 = MIN(MIN(h, SGI_KONA_FB_HEIGHT - sy), clip.y2 - dy);
    if (i0 >= i1 || j0 >= j1) {
        return;
    }
    /* Walk away from the destination's overlap with the source. */
    for (int64_t n = 0; n < j1 - j0; n++) {
        int64_t j = dy > sy ? j1 - 1 - n : j0 + n;
        int src_row = SGI_KONA_FB_HEIGHT - 1 - (sy + j);
        int dst_row = SGI_KONA_FB_HEIGHT - 1 - (dy + j);

        for (int64_t m = 0; m < i1 - i0; m++) {
            int64_t i = dx > sx ? i1 - 1 - m : i0 + m;

            if (!sgi_kona_x_clip_contains(s, dx + i, dy + j)) {
                continue;
            }
            sgi_plane_store(&s->mem, front, dx + i, dst_row,
                            sgi_plane_load(&s->mem, front, sx + i, src_row));
            sgi_plane_store(&s->mem, tag, dx + i, dst_row,
                            sgi_plane_load(&s->mem, tag, sx + i, src_row));
        }
    }
    sgi_scanout_invalidate_plane(&s->scanout, front,
                                 SGI_KONA_FB_HEIGHT - (dy + j1),
                                 SGI_KONA_FB_HEIGHT - 1 - (dy + j0));
}

/* 0x13a, X pixel finish: always 0, ending the write ("Pixel writes"). */
static void sgi_kona_x_pixel_finish(SGIKonaState *s)
{
    trace_sgi_kona_x_pixel_finish(s->x_pixel_open);
    if (!s->x_pixel_open) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X pixel finish with no pixel write open\n");
    }
    if (sgi_kona_x_pixel_dma_waiting(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: X pixel finish with %" PRIu32 " words of"
                      " its DMA rows not arrived\n", s->x_pixel_remaining);
        s->x_pixel_remaining = 0;
    }
    s->x_pixel_open = false;
}

/*
 * 0x1e7 tag: kona_retrace_activate's request, sent for a swap after its
 * display-table rewrite and for any other retrace wait. It becomes a
 * swap record on the shared scanout, which honours one per retrace in
 * order and hands it back through sgi_kona_publish(). The record names
 * the context current when the request arrived, the one the kernel's
 * swap validated first; a request with none current still completes its
 * tag, since the driver waits on the tag either way.
 */
static void sgi_kona_retrace_request(SGIKonaState *s, uint32_t tag)
{
    trace_sgi_kona_retrace_request(tag, s->context_current);
    sgi_scanout_queue_swap(&s->scanout, s->dma.seq++,
                           SGI_KONA_SWAP_COOKIE(s->context_current, tag));
}

static void sgi_kona_kernel_packet(void *opaque, unsigned port_id,
                                   uint32_t selector, const uint32_t *args,
                                   unsigned argc)
{
    SGIKonaState *s = opaque;

    sgi_kona_port_consumed(s, 1 + argc);
    trace_sgi_kona_kernel_command(selector, args[0],
                                  argc > 1 ? args[1] : 0,
                                  argc > 2 ? args[2] : 0, argc);
    switch (selector) {
    case SGI_KONA_CMD_X_DATA2:
    case SGI_KONA_CMD_X_DATA4:
    case SGI_KONA_CMD_X_DATA6:
    case SGI_KONA_CMD_X_DATA8:
    case SGI_KONA_CMD_X_DATA10:
    case SGI_KONA_CMD_X_BOX:
    case SGI_KONA_CMD_X_BOX6:
    case SGI_KONA_CMD_X_RECT:
    case SGI_KONA_CMD_X_RECT6:
        break;
    default:
        sgi_kona_x_box_data_drop(s, selector);
    }
    if (selector != SGI_KONA_CMD_X_BDATA4 && selector != SGI_KONA_CMD_X_CLIP) {
        sgi_kona_x_clip_run_drop(s, selector);
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

            memset(&s->gl_window[i], 0, sizeof(s->gl_window[i]));
            sgi_kona_gl_set_drawable(s, i);
            sgi_scanout_invalidate_all(&s->scanout);
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
         * The last of the state kona_act_pcx sends with a restore after
         * 0x1f3 and 0x1ee, consumed whole so the GL client's words after
         * it frame. What its two words carry is unknown, and nothing
         * waits for an answer. The pool 0x1ed that
         * kona_dmapool_update_pipe sends with them goes to the DMA engine.
         */
        trace_sgi_kona_command_unmodelled(selector, argc);
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
        sgi_kona_cursor_open(s, args[0]);
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
    case SGI_KONA_CMD_X_PIXEL_WRITE:
        sgi_kona_x_pixel_write(s, args);
        break;
    case SGI_KONA_CMD_X_PIXEL_FINISH:
        sgi_kona_x_pixel_finish(s);
        break;
    case SGI_KONA_CMD_X_PIXEL_COPY:
        sgi_kona_x_pixel_copy(s, args);
        break;
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
    case SGI_KONA_CMD_DISPLAY_MODE:
    case SGI_KONA_CMD_X_DID_MAP:
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
    case SGI_KONA_CMD_MEM_REGION:
        trace_sgi_kona_kernel_mem_region(args[0], args[1], args[2], args[3]);
        /* fall through */
    case SGI_KONA_CMD_MEM_COPY_DOWN:
    case SGI_KONA_CMD_MEM_SWAP_OUT:
    case SGI_KONA_CMD_RESOURCE_SYNC:
        /*
         * The kernel's pool packets, consumed whole so the stream stays in
         * step. The model keeps no pipe memory
         * behind the pools: a region's placement changes nothing it
         * draws, and a copy-down or swap-out moves no data. A swap-out
         * names completion reason 0x102, which the model does not post;
         * how the kernel waits for it is unknown, and only a full pool
         * sends one. The guest test is the minimal GLX program's trace
         * showing 0x1e4 2 9 0 1 and 0x172 0 with no word dropped.
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
 * command number, an overlay level of 1 or 2 for example, and takes a
 * stray word whose low half is a GL selector as that command. A trace
 * of a client choosing an overlay visual, or of the PROM's textport
 * after an X session, would constrain it.
 */
static void sgi_kona_textport_push(SGIKonaState *s, uint32_t word)
{
    uint32_t selector;

    if (s->gamma_remaining) {
        sgi_kona_gamma_word(s, word);
        return;
    }
    if (s->cursor_remaining) {
        sgi_kona_cursor_word(s);
        return;
    }
    if (s->gl_tex_table_remaining) {
        sgi_kona_gl_tex_table_word(s);
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
    if (s->x_pixel_remaining && !sgi_kona_x_pixel_dma_waiting(s)) {
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
            sgi_kona_kernel_token_known(word)) {
            sgi_command_port_write(&s->kernel_port, word, 4);
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
    if (s->gl_sync_held) {
        s->gl_sync_held = false;
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
         * computation multiplying struct+0x8 by struct+0xa, then applying
         * this same DIV_ROUND_UP(...,32) below; h2..h5 = x origin, y
         * origin, x advance, y advance, from the IO6 PROM's
         * kona_tp_bitmap, which sends six header words from halfwords
         * +0x8..+0x12 of the caller's descriptor in that order.
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
 * A plain word on the command page, as offset 0 takes it: the decoder a
 * fetched display list's words go through (hw/misc/sgi-kona-dma.c).
 */
void sgi_kona_command_word(SGIKonaState *s, uint32_t word)
{
    sgi_kona_textport_push(s, word);
}

/*
 * A store to the command page at `offset`. Offset 0 and the data slot,
 * offset 0xc, both carry a plain word: the next argument of a command
 * still collecting arguments, otherwise a token (or the textport's sync
 * word). The kernel and gfxinit send every token that way. A store at
 * 0xc + 0x20 * t opens token t with the stored value as its first
 * argument, the way the X server's kona.so sends its commands.
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
        sgi_kona_textport_push(s, value);
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
    if (!sgi_kona_kernel_token_known(token)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: unknown token 0x%" PRIx32 " stored at"
                      " command page offset 0x%" PRIx32 "; value 0x%" PRIx32
                      " dropped\n", token, offset, value);
        trace_sgi_kona_word_dropped(offset, value, "unknown token");
        sgi_kona_x_box_data_drop(s, token);
        sgi_kona_x_clip_run_drop(s, token);
        return;
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
    }
    if (s->gl_tex_table_remaining) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " while token 0x%x still needs %"
                      PRIu32 " words; partial command dropped\n", token,
                      offset, SGI_KONA_CMD_GL_TEX_TABLE,
                      s->gl_tex_table_remaining);
        s->gl_tex_table_remaining = 0;
    }
    if (s->gl_data_remaining) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-kona: token 0x%" PRIx32 " stored at command page"
                      " offset 0x%" PRIx32 " while token 0x%x still needs %"
                      PRIu32 " words; partial command dropped\n", token,
                      offset, s->gl_data_token, s->gl_data_remaining);
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
    sgi_command_port_write(&s->kernel_port, token, 4);
    sgi_command_port_write(&s->kernel_port, value, 4);
}

/*
 * Traces this window at a volume a human can read: one line for the
 * first access, then one per power-of-two access-count milestone after
 * it, instead of one per entry -- kona_mapram_init()'s own attach-time
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

    if (addr == SGI_KONA_EEPROM_CHECKED_ADDR) {
        return SGI_KONA_EEPROM_CHECKED_VALUE;
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
        value = sgi_kona_credit(s);
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
        value = s->activity ? SGI_KONA_ACTIVITY_BUS : 0;
        break;
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

    trace_sgi_kona_pio_read(cpu, addr, value, size);
    return value;
}

static void sgi_kona_write_word(void *opaque, hwaddr addr,
                                uint64_t value, unsigned size)
{
    SGIKonaState *s = opaque;
    int cpu = current_cpu ? current_cpu->cpu_index : -1;

    trace_sgi_kona_pio_write(cpu, addr, value, size);

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
    case SGI_KONA_LOW_WINDOW_WRITE:
        /* Observed value 0x23 during install; meaning unknown. */
        s->low_window_write = value;
        break;
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
        /* Stored for migration/introspection; never read back as-is. */
        s->int_enable = value;
        break;
    case SGI_KONA_INT_CAUSE:
        /* Write-1-to-clear, SGI_KONA_INT_CAUSE_GE included. */
        s->int_cause &= ~(uint32_t)value;
        /*
         * A completion word is acknowledged by this write alone: the
         * driver's DMA-write wait reads the mailbox once and writes
         * nothing back to it before its handler clears the cause bit
         * (established from kona_arm_intr and kona_intr_handler), where
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
        break;
    case SGI_KONA_EEPROM_ENABLE:
        s->eeprom_enable = value;
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
        break;
    case SGI_KONA_DMA_FIFO_PUSH:
        s->activity = true;
        sgi_command_port_write(&s->dma.fifo_port, value, 4);
        break;
    case SGI_KONA_DMA_FIFO_KICK:
        sgi_command_port_kick_write(&s->dma.fifo_port, value);
        break;
    default:
        if (addr >= SGI_KONA_TEXTPORT_PORT &&
            addr < SGI_KONA_TEXTPORT_PORT + SGI_KONA_COMMAND_PAGE_SIZE) {
            sgi_kona_command_page_write(s, addr - SGI_KONA_TEXTPORT_PORT,
                                        value);
            break;
        }
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
 * An 8-byte access. kona.so's bulk data helpers (KonSeqData4_ip19 and its
 * siblings) store aligned pairs of data words as one doubleword at the
 * command page's offset 0, most significant word first, and odd words as
 * 4-byte stores at the data slot (the stores are established, that the pipe
 * takes them as two data words inferred). Such a store delivers the high
 * word, then the low word, each exactly as a 4-byte store at offset 0.
 *
 * Approximated: a doubleword at offset 0 can also open a command with its
 * high word, which the helpers never make it do, and no evidence says how
 * the pipe treats one. The doubleword data qtest (A:B inside an open
 * load counts as A then B) checks the split; a guest run whose X
 * server reaches those helpers would check it in use.
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

static uint64_t sgi_kona_read(void *opaque, hwaddr addr, unsigned size)
{
    if (size != 8) {
        return sgi_kona_read_word(opaque, addr, size);
    }
    sgi_kona_log_split(addr, false);
    return (sgi_kona_read_word(opaque, addr, 4) << 32) |
           sgi_kona_read_word(opaque, addr + 4, 4);
}

/*
 * One processor store, whatever its width: the graphics credit it was
 * charged is sampled here, once, before anything it carries is acted on.
 */
static void sgi_kona_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    SGIKonaState *s = opaque;
    bool counted = sgi_kona_store_counted();
    int cpu = current_cpu ? current_cpu->cpu_index : -1;

    if (size != 8) {
        sgi_kona_write_word(opaque, addr, value, size);
    } else if (sgi_kona_doubleword_data(addr)) {
        /* The raw width; each half then traces as a 4-byte store. */
        trace_sgi_kona_pio_write(cpu, addr, value, size);
        sgi_kona_write_word(opaque, addr, value >> 32, 4);
        sgi_kona_write_word(opaque, addr, (uint32_t)value, 4);
    } else {
        trace_sgi_kona_pio_write(cpu, addr, value, size);
        sgi_kona_log_split(addr, true);
        sgi_kona_write_word(opaque, addr, value >> 32, 4);
        sgi_kona_write_word(opaque, addr + 4, (uint32_t)value, 4);
    }
    sgi_kona_pi_credits(s, counted);
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
    .read = sgi_kona_read,
    .write = sgi_kona_write,
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

    memory_region_init_io(&s->regs, OBJECT(s), &sgi_kona_ops, s,
                          "sgi-kona.regs",
                          SGI_KONA_APERTURE_SIZE);
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
    const uint64_t pixels = (uint64_t)SGI_KONA_FB_WIDTH * SGI_KONA_FB_HEIGHT;
    const SGIPlane colour = {
        .len = pixels * 4,
        .width = SGI_KONA_FB_WIDTH,
        .height = SGI_KONA_FB_HEIGHT,
        .stride = SGI_KONA_FB_WIDTH * 4,
        .format = SGI_PLANE_RGB24,
        .packing = SGI_PACK_LE_RED_HIGH,
        .bottom_origin = true,
        .evidence = "model's choice: the one mode it presents, "
                    "little-endian x8r8g8b8; no capture of the pipe's "
                    "pixel layout",
    };
    const SGIPlane tag = {
        .len = pixels,
        .width = SGI_KONA_FB_WIDTH,
        .height = SGI_KONA_FB_HEIGHT,
        .stride = SGI_KONA_FB_WIDTH,
        .format = SGI_PLANE_TAG,
        .packing = SGI_PACK_LE_RED_LOW,
        .bottom_origin = true,
        .evidence = "model's choice: colour-map index or direct RGB per "
                    "pixel",
    };
    const SGIPlane list = {
        .len = s->list_memory_size,
        .width = SGI_KONA_LIST_ROW_BYTES / 4,
        .height = s->list_memory_size / SGI_KONA_LIST_ROW_BYTES,
        .stride = SGI_KONA_LIST_ROW_BYTES,
        .format = SGI_PLANE_RAW,
        .packing = SGI_PACK_BE_RED_LOW,
        .evidence = "model's choice: words of resident display lists; "
                    "size not held from any capture",
    };
    /*
     * One depth buffer for the screen, as GL gives a window one shared by
     * its front and back buffers, addressed like the colour planes. The
     * pipe's depth layout is unknown; 24 bits is the model's rules'.
     */
    const SGIPlane depth = {
        .len = pixels * 4,
        .width = SGI_KONA_FB_WIDTH,
        .height = SGI_KONA_FB_HEIGHT,
        .stride = SGI_KONA_FB_WIDTH * 4,
        .format = SGI_PLANE_Z24,
        .packing = SGI_PACK_LE_RED_LOW,
        .bottom_origin = true,
        .evidence = "model's choice: one 24-bit depth word a pixel; no "
                    "capture of the pipe's depth layout",
    };
    SGIPlane p;
    uint64_t base = 0;

    sgi_board_memory_init(&s->mem, 2 * colour.len + 2 * tag.len + list.len +
                          depth.len);

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
    /*
     * Before anything this function sets up: finalize relies on a board
     * whose realize failed never having reached address_space_init().
     */
    if (!sgi_kona_list_size_valid(s, errp)) {
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

    /*
     * fabric's own address space: this board's first feature needing
     * dma_memory_read()/dma_memory_write() rather than the register
     * file's MMIO dispatch. Destroyed in instance_finalize(), matching
     * hw/misc/sgi-mgras.c's own fabric_as lifecycle.
     */
    address_space_init(&s->fabric_as, s->fabric, "sgi-kona.fabric");
    sgi_kona_dma_init(&s->dma, s);

    s->map_ram = g_new0(uint32_t, SGI_KONA_MAPRAM_ENTRIES);
    s->map_ram_count = SGI_KONA_MAPRAM_ENTRIES;

    s->arm_high = g_new0(uint32_t, SGI_KONA_ARM_HIGH_WORDS);
    s->arm_high_count = SGI_KONA_ARM_HIGH_WORDS;

    sgi_kona_board_memory_init(s);
    sgi_scanout_init(&s->scanout, DEVICE(s), &sgi_kona_head_ops, s);
    sgi_scanout_set_mode(&s->scanout, &sgi_kona_scanout_mode);
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

    if (DEVICE(obj)->realized && s->test_pixel_x < SGI_KONA_FB_WIDTH &&
        s->test_pixel_y < SGI_KONA_FB_HEIGHT) {
        value = sgi_kona_fb_read(s, s->test_pixel_x, s->test_pixel_y);
    }
    visit_type_uint32(v, name, &value, errp);
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
    const SGIPlane *p = NULL;
    uint32_t value = 0;

    if (DEVICE(obj)->realized) {
        p = sgi_kona_plane(s, s->test_plane);
    }
    if (p) {
        value = sgi_plane_load(&s->mem, p, s->test_pixel_x, s->test_pixel_y);
    }
    visit_type_uint32(v, name, &value, errp);
}

/*
 * Host-only qtest access to the gamma table: the entry at gamma-RAM
 * address test_gamma_addr, or 0 for an address the table does not cover.
 */
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

static void sgi_kona_instance_init(Object *obj)
{
    SGIKonaState *s = SGI_KONA(obj);

    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        sgi_gl_init(&s->gl[i], i);
    }
    for (unsigned k = 0; k < ARRAY_SIZE(s->gl_attrib); k++) {
        sgi_gl_init(&s->gl_attrib[k], k / SGI_KONA_GL_ATTRIB_DEPTH);
    }

    object_property_add_uint32_ptr(obj, "x-test-pixel-x", &s->test_pixel_x,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint32_ptr(obj, "x-test-pixel-y", &s->test_pixel_y,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-pixel", "uint32",
                        sgi_kona_test_get_pixel, NULL, NULL, NULL);
    object_property_add_uint32_ptr(obj, "x-test-plane", &s->test_plane,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-plane-word", "uint32",
                        sgi_kona_test_get_plane_word, NULL, NULL, NULL);
    object_property_add_uint32_ptr(obj, "x-test-gamma-addr",
                                   &s->test_gamma_addr,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-gamma", "uint32",
                        sgi_kona_test_get_gamma, NULL, NULL, NULL);
    object_property_add_uint32_ptr(obj, "x-test-gc-index", &s->test_gc_index,
                                   OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "x-test-gc", "uint32", sgi_kona_test_get_gc,
                        NULL, NULL, NULL);
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

    if (s->scanout.con) {
        sgi_scanout_cleanup(&s->scanout);
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
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        sgi_gl_cleanup(&s->gl[i]);
    }
    for (unsigned k = 0; k < ARRAY_SIZE(s->gl_attrib); k++) {
        sgi_gl_cleanup(&s->gl_attrib[k]);
    }
    sgi_board_memory_cleanup(&s->mem);
    g_free(s->legacy_framebuffer);
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

    s->widget_control = 0;
    s->onewire_reg = 0;

    /* Increment 2: register-window plain storage and the mailbox queue. */
    s->low_window_write = 0;
    s->int_dest_upper = 0;
    s->int_dest_lower = 0;
    s->int_vectors = 0;
    s->reset_arg = 0;
    s->int_enable = 0;
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
    s->reason_page_valid = false;
    s->reason_page = 0;
    s->return_page_valid = false;
    s->return_page = 0;
    s->credit_config = 0;
    s->credit_pending = 0;
    s->pi_held = 0;
    s->retrace_tag = 0;
    s->retrace_completed = false;
    memset(s->contexts, 0, sizeof(s->contexts));
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        sgi_kona_gl_reset_context(s, i);
    }
    s->context_current = SGI_KONA_CONTEXT_NONE;
    s->context_page_pending = false;
    s->context_clock = 0;
    s->video_open = false;
    s->video_command = 0;
    s->video_have = 0;
    s->video_first = 0;
    s->video_tail_sum = 0;
    s->color_map_start = 0;
    s->color_map_count = 0;
    s->color_map_remaining = 0;
    s->color_map_outside = 0;
    memset(s->x_color_map, 0, sizeof(s->x_color_map));
    memset(s->gamma, 0, sizeof(s->gamma));
    s->gamma_start = 0;
    s->gamma_count = 0;
    s->gamma_remaining = 0;
    s->gamma_outside = 0;
    s->cursor_remaining = 0;
    s->gl_tex_table_remaining = 0;
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
    memset(s->x_pixel_setup, 0, sizeof(s->x_pixel_setup));
    s->x_pixel_open = false;
    s->x_pixel_x = 0;
    s->x_pixel_y = 0;
    s->x_pixel_bpp = 0;
    s->x_pixel_stride = 0;
    s->x_pixel_words = 0;
    s->x_pixel_remaining = 0;
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
    memset(s->mem.bytes, 0, s->mem.bytes_len);
    sgi_scanout_reset(&s->scanout);
}

static const Property sgi_kona_properties[] = {
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
};

/* A version 6 stream from before the board memory, or none. */
static bool sgi_kona_legacy_framebuffer_count_valid(void *opaque,
                                                    int version_id)
{
    SGIKonaState *s = opaque;

    return s->legacy_framebuffer_count == 0 ||
           s->legacy_framebuffer_count ==
           SGI_KONA_FB_WIDTH * SGI_KONA_FB_HEIGHT;
}

static int sgi_kona_pre_save(void *opaque)
{
    SGIKonaState *s = opaque;

    s->legacy_framebuffer_count = 0;
    s->legacy_framebuffer_dirty = false;
    return 0;
}

/*
 * A framebuffer from a version 6 stream saved before the board memory:
 * resolved words, top display line first. They land in the front bank
 * as direct colour, which shows exactly what the source showed; the
 * colour-map indices behind its textport pixels were never saved.
 */
static void sgi_kona_load_legacy_framebuffer(SGIKonaState *s)
{
    for (uint32_t line = 0; line < SGI_KONA_FB_HEIGHT; line++) {
        for (uint32_t x = 0; x < SGI_KONA_FB_WIDTH; x++) {
            sgi_kona_fb_store_direct(
                s, x, SGI_KONA_FB_HEIGHT - 1 - line,
                s->legacy_framebuffer[line * SGI_KONA_FB_WIDTH + x]);
        }
    }
    g_free(s->legacy_framebuffer);
    s->legacy_framebuffer = NULL;
    s->legacy_framebuffer_count = 0;
    s->legacy_framebuffer_dirty = false;
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
 * drawing commands, so every line is repainted. A version 6 stream from
 * before the scanout carries no scanout subsection; the destination then
 * keeps the retrace phase it started with. One from before the board
 * memory carries its framebuffer in the main section instead of the
 * board memory subsection.
 */
static int sgi_kona_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

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
    /* A gamma load can have no more entries to come than it counted. */
    if (s->gamma_remaining > s->gamma_count ||
        s->gamma_outside > s->gamma_count - s->gamma_remaining) {
        return -EINVAL;
    }
    if (s->color_map_remaining > s->color_map_count ||
        s->color_map_outside > s->color_map_count - s->color_map_remaining) {
        return -EINVAL;
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
    /* Pixel data in flight needs the row geometry its write computed. */
    if (s->x_pixel_remaining > s->x_pixel_words ||
        (s->x_pixel_remaining &&
         (!s->x_pixel_stride ||
          s->x_pixel_bpp != sgi_kona_x_pixel_bpp(
              s->x_pixel_setup[SGI_KONA_X_SETUP_SRC_PACK])))) {
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
    /* The current context is a live entry, and only it waits for a page. */
    if (s->context_current < SGI_KONA_CONTEXT_NONE ||
        s->context_current >= SGI_KONA_CONTEXTS ||
        (s->context_current != SGI_KONA_CONTEXT_NONE &&
         !s->contexts[s->context_current].valid) ||
        (s->context_page_pending &&
         s->context_current == SGI_KONA_CONTEXT_NONE)) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        if (s->contexts[i].save_offset > 0xfff) {
            return -EINVAL;
        }
    }
    /*
     * A GL packet part-way through is one of the GL tokens with its own
     * count, a GL data run belongs to a command that has one, and a 1 is
     * held only in the GL grammar and between packets.
     */
    if (s->gl_port.framing &&
        (!sgi_kona_gl_token(s->gl_port.selector) ||
         s->gl_port.argc_expected !=
         sgi_kona_gl_token(s->gl_port.selector)->argc ||
         s->gl_port.argc_have >= s->gl_port.argc_expected)) {
        return -EINVAL;
    }
    if (s->gl_data_remaining &&
        (s->gl_port.framing || s->gl_tex_table_remaining ||
         (s->gl_data_token != SGI_KONA_CMD_GL_POLYGON_STIPPLE &&
          s->gl_data_token != SGI_KONA_CMD_GL_POP_LIGHTS &&
          s->gl_data_token != SGI_KONA_CMD_GL_TEX_LOAD))) {
        return -EINVAL;
    }
    if (s->gl_sync_held &&
        (!s->gl_grammar || s->gl_port.framing || s->textport_expect_sync)) {
        return -EINVAL;
    }
    if (s->legacy_framebuffer) {
        sgi_kona_load_legacy_framebuffer(s);
    }
    sgi_scanout_invalidate_all(&s->scanout);
    return 0;
}

/*
 * Board memory, and the scanout's mode, retrace phase, swap queue and
 * cursor, as the subsections the graphics low-level design's migration
 * rule asks for, so the family's own version stays 6.
 */
DEFINE_VMSTATE_SGI_BOARD_MEMORY(vmstate_sgi_kona_board_memory,
                                TYPE_SGI_KONA "/board-memory",
                                SGIKonaState, mem);
DEFINE_VMSTATE_SGI_SCANOUT(vmstate_sgi_kona_scanout, TYPE_SGI_KONA "/scanout",
                           SGIKonaState, scanout);

/*
 * State for the X server's commands that arrived after version 6 did: the
 * sum of an open video command's data words past its first, the graphics
 * context and a run of box words waiting for its X box. A version 6
 * stream has none of it and loads with the reset default, which is exact
 * for any video command such a stream can hold open, since none took more
 * than one data word, and for the rest, which that source never kept.
 *
 * Version 2 adds the X colour map, an open colour-map load's start, the X
 * position and clip, and the pixel setup and an open pixel write. A
 * version 1 source kept none of them: its destination has an empty map,
 * no clip and no write open, and one caught mid colour-map load resumes
 * at start 0. The subsection is always sent, so a binary from before it
 * (before d84ac60ae3) refuses every stream this one saves, and one with
 * version 1 (up to 057d09cc95) refuses version 2.
 *
 * Version 3 adds the one-bit drawing state: the scissor, the stipple
 * cache, the loaded stipple and a stipple store still taking its rows. A
 * version 2 source kept none of it, and its destination starts with no
 * scissor and an empty cache with no stipple loaded, as that source drew.
 * A binary with version 2 (up to 61b7e6c4e2) refuses version 3.
 */
static bool sgi_kona_x_server_needed(void *opaque)
{
    return true;
}

static const VMStateDescription vmstate_sgi_kona_x_server = {
    .name = TYPE_SGI_KONA "/x-server",
    .version_id = 3,
    .minimum_version_id = 1,
    .needed = sgi_kona_x_server_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(video_tail_sum, SGIKonaState),
        VMSTATE_UINT32_ARRAY(x_gc, SGIKonaState, SGI_KONA_X_GC_WORDS),
        VMSTATE_UINT32_ARRAY(x_boxes, SGIKonaState, SGI_KONA_X_BOX_RUN_WORDS),
        VMSTATE_UINT32(x_boxes_have, SGIKonaState),
        VMSTATE_UINT32_V(color_map_start, SGIKonaState, 2),
        VMSTATE_UINT32_V(color_map_outside, SGIKonaState, 2),
        VMSTATE_UINT32_ARRAY_V(x_color_map, SGIKonaState,
                               SGI_KONA_X_COLOR_MAP_ENTRIES, 2),
        VMSTATE_UINT32_ARRAY_V(x_position, SGIKonaState,
                               SGI_KONA_X_POSITION_WORDS, 2),
        VMSTATE_UINT32_V(x_clip_count, SGIKonaState, 2),
        VMSTATE_UINT32_ARRAY_V(x_clip_rects, SGIKonaState,
                               4 * SGI_KONA_X_CLIP_MAX_RECTS, 2),
        VMSTATE_UINT32_ARRAY_V(x_clip_run, SGIKonaState,
                               SGI_KONA_X_CLIP_LIST_WORDS, 2),
        VMSTATE_UINT32_V(x_clip_run_have, SGIKonaState, 2),
        VMSTATE_UINT32_ARRAY_V(x_pixel_setup, SGIKonaState,
                               SGI_KONA_X_PIXEL_SETUP_WORDS, 2),
        VMSTATE_BOOL_V(x_pixel_open, SGIKonaState, 2),
        VMSTATE_INT32_V(x_pixel_x, SGIKonaState, 2),
        VMSTATE_INT32_V(x_pixel_y, SGIKonaState, 2),
        VMSTATE_UINT32_V(x_pixel_bpp, SGIKonaState, 2),
        VMSTATE_UINT32_V(x_pixel_stride, SGIKonaState, 2),
        VMSTATE_UINT32_V(x_pixel_words, SGIKonaState, 2),
        VMSTATE_UINT32_V(x_pixel_remaining, SGIKonaState, 2),
        VMSTATE_UINT32_V(x_pixel_prev, SGIKonaState, 2),
        VMSTATE_BOOL_V(x_scissor_valid, SGIKonaState, 3),
        VMSTATE_UINT32_ARRAY_V(x_scissor, SGIKonaState,
                               SGI_KONA_X_SCISSOR_WORDS, 3),
        VMSTATE_UINT32_ARRAY_V(x_stipple_width, SGIKonaState,
                               SGI_KONA_X_STIPPLE_ENTRIES, 3),
        VMSTATE_UINT32_ARRAY_V(x_stipple_height, SGIKonaState,
                               SGI_KONA_X_STIPPLE_ENTRIES, 3),
        VMSTATE_INT32_ARRAY_V(x_stipple_origin_x, SGIKonaState,
                              SGI_KONA_X_STIPPLE_ENTRIES, 3),
        VMSTATE_INT32_ARRAY_V(x_stipple_origin_y, SGIKonaState,
                              SGI_KONA_X_STIPPLE_ENTRIES, 3),
        VMSTATE_UINT32_ARRAY_V(x_stipple_rows, SGIKonaState,
                               SGI_KONA_X_STIPPLE_ENTRIES *
                               SGI_KONA_X_STIPPLE_MAX, 3),
        VMSTATE_UINT32_V(x_stipple_loaded, SGIKonaState, 3),
        VMSTATE_UINT32_V(x_stipple_store_entry, SGIKonaState, 3),
        VMSTATE_UINT32_V(x_stipple_store_width, SGIKonaState, 3),
        VMSTATE_UINT32_V(x_stipple_store_height, SGIKonaState, 3),
        VMSTATE_UINT32_V(x_stipple_store_remaining, SGIKonaState, 3),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * State for graphics contexts and their clients, which arrived after the
 * device's version 6 and the X server subsection's version 3: a GL
 * texture table still taking its data words. A source without the
 * subsection kept none of it and loads with no table open. The
 * subsection is always sent, so a binary from before it (up to
 * 3432a6a7ef) refuses every stream this one saves.
 *
 * Version 2 adds the context table, the current context and whether it
 * waits for its return page. A version 1 source kept none of them: its
 * destination knows no context, so its first restore is logged and
 * answers nowhere until a context is set up again. A binary with version
 * 1 refuses version 2.
 *
 * Version 3 adds the page's GL grammar: the buffer-configuration framer,
 * whether a context has been activated and whether a 1 is held. A
 * version 2 source kept none of them, and its destination reads the page
 * in the textport grammar until the next activation. A binary with
 * version 2 refuses version 3.
 *
 * Version 4 adds each entry's GL context and its window and buffers. A
 * version 3 source drew no GL, and its destination's contexts start from
 * GL's initial state with no window, so they draw nothing until the
 * kernel next sends one. A binary with version 3 refuses version 4.
 *
 * Version 5 adds the GL framing of the whole 3D grammar: the top half of
 * the GL command word being framed, a GL data run still taking its
 * words, and each entry's pixel-store state. A version 4 source framed
 * none of the commands that use them, so the destination's reset values
 * stand.
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
        VMSTATE_UINT64(last_used, SGIKonaContext),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * The initial credit configuration the driver writes, stored since the
 * credit register began following consumption. A source without the
 * subsection loads with none written.
 *
 * Version 2 adds the processor stores whose graphics credits wait for
 * their packet's last word, which a packet part-way through a migration
 * still owes. A version 1 source returned no credits and owes none.
 */
static bool sgi_kona_credit_needed(void *opaque)
{
    return true;
}

static const VMStateDescription vmstate_sgi_kona_credit = {
    .name = TYPE_SGI_KONA "/credit",
    .version_id = 2,
    .minimum_version_id = 1,
    .needed = sgi_kona_credit_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(credit_config, SGIKonaState),
        VMSTATE_UINT32_V(pi_held, SGIKonaState, 2),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * The retrace tag register, since retrace requests began completing. The
 * requests still waiting are the scanout's swap records. A source without
 * the subsection loads with the register at 0.
 */
static bool sgi_kona_retrace_needed(void *opaque)
{
    return true;
}

static const VMStateDescription vmstate_sgi_kona_retrace = {
    .name = TYPE_SGI_KONA "/retrace",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_retrace_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(retrace_tag, SGIKonaState),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_window = {
    .name = TYPE_SGI_KONA "/gl-window",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(placed, SGIKonaGLWindow),
        VMSTATE_INT32(x, SGIKonaGLWindow),
        VMSTATE_INT32(y, SGIKonaGLWindow),
        VMSTATE_UINT32(width, SGIKonaGLWindow),
        VMSTATE_UINT32(height, SGIKonaGLWindow),
        VMSTATE_UINT32(double_buffered, SGIKonaGLWindow),
        VMSTATE_UINT32(displayed_bank, SGIKonaGLWindow),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_kona_gl_pipe = {
    .name = TYPE_SGI_KONA "/gl-pipe",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(unpack_alignment, SGIKonaGLPipe),
        VMSTATE_UINT32(unpack_row_length, SGIKonaGLPipe),
        VMSTATE_UINT32(pack_alignment, SGIKonaGLPipe),
        VMSTATE_UINT32(pack_row_length, SGIKonaGLPipe),
        VMSTATE_UINT32(depth_bits, SGIKonaGLPipe),
        VMSTATE_UINT32(read_buffer, SGIKonaGLPipe),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * Each GL context's drawable is the window's and the banks' (see
 * sgi_kona_gl_set_drawable()), so it is set again from them, after the
 * shared description has refused a context its own commands could not
 * have left. A version 3 source drew no GL: its contexts keep the
 * destination's reset state and no window.
 */
static int sgi_kona_contexts_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        const SGIKonaGLWindow *w = &s->gl_window[i];

        if (w->placed > 1 || w->double_buffered > 1 ||
            w->displayed_bank > 1) {
            return -EINVAL;
        }
        s->gl[i].hw_id = i;
        sgi_kona_gl_set_drawable(s, i);
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_contexts = {
    .name = TYPE_SGI_KONA "/contexts",
    .version_id = 5,
    .minimum_version_id = 1,
    .needed = sgi_kona_contexts_needed,
    .post_load = sgi_kona_contexts_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gl_tex_table_remaining, SGIKonaState),
        VMSTATE_STRUCT_ARRAY(contexts, SGIKonaState, SGI_KONA_CONTEXTS, 2,
                             vmstate_sgi_kona_context, SGIKonaContext),
        VMSTATE_INT32_V(context_current, SGIKonaState, 2),
        VMSTATE_BOOL_V(context_page_pending, SGIKonaState, 2),
        VMSTATE_UINT64_V(context_clock, SGIKonaState, 2),
        VMSTATE_UINT32_V(gl_port.kick, SGIKonaState, 3),
        VMSTATE_BOOL_V(gl_port.framing, SGIKonaState, 3),
        VMSTATE_UINT32_V(gl_port.selector, SGIKonaState, 3),
        VMSTATE_UINT32_V(gl_port.argc_expected, SGIKonaState, 3),
        VMSTATE_UINT32_V(gl_port.argc_have, SGIKonaState, 3),
        VMSTATE_UINT32_ARRAY_V(gl_port.args, SGIKonaState,
                               SGI_COMMAND_PORT_MAX_ARGS, 3),
        VMSTATE_BOOL_V(gl_grammar, SGIKonaState, 3),
        VMSTATE_BOOL_V(gl_sync_held, SGIKonaState, 3),
        VMSTATE_STRUCT_ARRAY(gl_window, SGIKonaState, SGI_KONA_CONTEXTS, 4,
                             vmstate_sgi_kona_gl_window, SGIKonaGLWindow),
        VMSTATE_STRUCT_ARRAY(gl, SGIKonaState, SGI_KONA_CONTEXTS, 4,
                             vmstate_sgi_gl_context, SGIGLContext),
        VMSTATE_UINT32_V(gl_token_high, SGIKonaState, 5),
        VMSTATE_UINT32_V(gl_data_token, SGIKonaState, 5),
        VMSTATE_UINT32_V(gl_data_remaining, SGIKonaState, 5),
        VMSTATE_UINT32_V(gl_data_have, SGIKonaState, 5),
        VMSTATE_UINT32_ARRAY_V(gl_data, SGIKonaState, SGI_KONA_GL_DATA_MAX,
                               5),
        VMSTATE_STRUCT_ARRAY(gl_pipe, SGIKonaState, SGI_KONA_CONTEXTS, 5,
                             vmstate_sgi_kona_gl_pipe, SGIKonaGLPipe),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * The GL attribute stacks glPushAttrib filled, only while one holds a
 * copy. Each copy is a whole shared context, refused on load as the
 * contexts are when its own commands could not have left it.
 */
static bool sgi_kona_gl_attrib_needed(void *opaque)
{
    SGIKonaState *s = opaque;

    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        if (s->gl_attrib_depth[i]) {
            return true;
        }
    }
    return false;
}

static int sgi_kona_gl_attrib_post_load(void *opaque, int version_id)
{
    SGIKonaState *s = opaque;

    for (unsigned i = 0; i < SGI_KONA_CONTEXTS; i++) {
        if (s->gl_attrib_depth[i] > SGI_KONA_GL_ATTRIB_DEPTH) {
            return -EINVAL;
        }
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_kona_gl_attrib = {
    .name = TYPE_SGI_KONA "/gl-attrib",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_kona_gl_attrib_needed,
    .post_load = sgi_kona_gl_attrib_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(gl_attrib_depth, SGIKonaState,
                             SGI_KONA_CONTEXTS),
        VMSTATE_UINT32_ARRAY(gl_attrib_mask, SGIKonaState,
                             SGI_KONA_CONTEXTS * SGI_KONA_GL_ATTRIB_DEPTH),
        VMSTATE_STRUCT_ARRAY(gl_attrib, SGIKonaState,
                             SGI_KONA_CONTEXTS * SGI_KONA_GL_ATTRIB_DEPTH, 0,
                             vmstate_sgi_gl_context, SGIGLContext),
        VMSTATE_END_OF_LIST()
    },
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

static const VMStateDescription vmstate_sgi_kona = {
    .name = TYPE_SGI_KONA,
    .version_id = 6,
    .minimum_version_id = 1,
    .pre_save = sgi_kona_pre_save,
    .post_load = sgi_kona_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(widget_control, SGIKonaState),
        VMSTATE_UINT32(onewire_reg, SGIKonaState),

        /* Increment 2: register-window plain storage and the mailbox queue. */
        VMSTATE_UINT32(low_window_write, SGIKonaState),
        VMSTATE_UINT32(reset_arg, SGIKonaState),
        /*
         * SGI_KONA_INT_ENABLE (see its own header comment): new alongside
         * the XG map RAM below, so it is gated the same way, at version 3.
         */
        VMSTATE_UINT32_V(int_enable, SGIKonaState, 3),
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
         * rather than beside them. Gated at version 2: vmstate_sgi_kona
         * has carried version_id 1 since the device's introduction, and
         * an actual v1 stream has no bytes for either field. Reading them
         * unconditionally would misparse a genuine v1 stream's next
         * section content as raster_x/raster_y; gating leaves both at
         * sgi_kona_reset_hold()'s own (0, 0) default when an old stream
         * loads, matching what a v1 source's absence of the concept
         * always implied.
         */
        VMSTATE_INT32_V(raster_x, SGIKonaState, 2),
        VMSTATE_INT32_V(raster_y, SGIKonaState, 2),

        /*
         * Where a version 6 stream from before the board memory carried
         * the framebuffer. The pixels now travel in the board memory
         * subsection, so a stream saved now has a zero count here and no
         * words; one saved before has the full framebuffer, which
         * post_load moves into the front plane. See the struct field
         * comment on legacy_framebuffer. The other direction does not
         * work: a binary from before the board memory refuses a stream
         * saved now, failing its "sgi-kona/framebuffer length is fixed"
         * check on the zero count.
         */
        VMSTATE_UINT32(legacy_framebuffer_count, SGIKonaState),
        VMSTATE_VALIDATE("framebuffer length is zero or fixed",
                         sgi_kona_legacy_framebuffer_count_valid),
        VMSTATE_VARRAY_UINT32_ALLOC(legacy_framebuffer, SGIKonaState,
                                    legacy_framebuffer_count, 0,
                                    vmstate_info_uint32, uint32_t),
        VMSTATE_BOOL(legacy_framebuffer_dirty, SGIKonaState),

        /*
         * Stage 4: the DMA engine's FIFO command port (same field set as
         * textport's own above) and its kick-primed word accumulator.
         * Gated at version 3 for the same reason raster_x/raster_y are
         * gated at version 2: a genuine v1/v2 stream has no bytes for
         * these, so reading them unconditionally would misparse whatever
         * follows. An old stream loads with sgi_kona_reset_hold()'s own
         * all-zero default, matching a v1/v2 source's absence of the
         * concept.
         */
        VMSTATE_UINT32_V(dma.fifo_port.kick, SGIKonaState, 3),
        VMSTATE_BOOL_V(dma.fifo_port.framing, SGIKonaState, 3),
        VMSTATE_UINT32_V(dma.fifo_port.selector, SGIKonaState, 3),
        VMSTATE_UINT32_V(dma.fifo_port.argc_expected, SGIKonaState, 3),
        VMSTATE_UINT32_V(dma.fifo_port.argc_have, SGIKonaState, 3),
        VMSTATE_UINT32_ARRAY_V(dma.fifo_port.args, SGIKonaState,
                               SGI_COMMAND_PORT_MAX_ARGS, 3),
        /*
         * The accumulator of the engine the shared pixel DMA replaced:
         * a hole, so a version 6 stream still loads. The engine's own
         * state is the sgi-kona/dma subsection.
         */
        VMSTATE_UNUSED_V(3, SGI_KONA_DMA_RETIRED_BYTES),

        /*
         * The XG map RAM (see the header comment on SGI_KONA_MAPRAM_BASE):
         * canonical guest-visible state, migrated the same non-allocating
         * way arm_high is below (map_ram_count is fixed at
         * SGI_KONA_MAPRAM_ENTRIES, never guest-shrunk, so only its fixed
         * length needs to travel through VMSTATE_VARRAY_UINT32()). Gated
         * at version 3, alongside int_enable above: vmstate_sgi_kona
         * reached version_id 2 for raster_x/raster_y only, and an actual
         * v1 or v2 stream has no bytes for map_ram_count or map_ram.
         * Reading them unconditionally would misparse a genuine older
         * stream's next section as this table; gating leaves map_ram at
         * sgi_kona_reset_hold()'s own all-zero default when an older
         * stream loads, matching what that stream's absence of the
         * concept always implied.
         */
        VMSTATE_UINT32_V(map_ram_count, SGIKonaState, 3),
        VMSTATE_VALIDATE("map RAM length is fixed",
                         sgi_kona_mapram_count_valid),
        VMSTATE_VARRAY_UINT32(map_ram, SGIKonaState, map_ram_count, 3,
                              vmstate_info_uint32, uint32_t),

        /*
         * The firmware load and its record (see the header comment on
         * SGI_KONA_ARM_LOW_BASE). Gated at version 4 the same way the
         * map RAM is gated at version 3: a version 1-3 stream has no
         * bytes for any of it, so an older stream loads with
         * sgi_kona_reset_hold()'s all-zero default, matching a source
         * that never had the port.
         */
        VMSTATE_UINT32_ARRAY_V(arm_low, SGIKonaState, SGI_KONA_ARM_LOW_WORDS,
                               4),
        VMSTATE_UINT32_V(arm_high_count, SGIKonaState, 4),
        VMSTATE_VALIDATE("ARM memory length is fixed",
                         sgi_kona_arm_high_count_valid),
        VMSTATE_VARRAY_UINT32(arm_high, SGIKonaState, arm_high_count, 4,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32_V(icu_control, SGIKonaState, 4),
        VMSTATE_BOOL_V(load_section_open, SGIKonaState, 4),
        VMSTATE_UINT32_V(load_sections, SGIKonaState, 4),
        VMSTATE_UINT32_ARRAY_V(load_section_addr, SGIKonaState,
                               SGI_KONA_LOAD_SECTIONS_MAX, 4),
        VMSTATE_UINT32_ARRAY_V(load_section_words, SGIKonaState,
                               SGI_KONA_LOAD_SECTIONS_MAX, 4),
        VMSTATE_UINT32_V(loader_3b0, SGIKonaState, 4),
        VMSTATE_UINT32_V(loader_328, SGIKonaState, 4),

        /*
         * The kernel command framing, the reason and return pages and the
         * interrupt destination reasons are posted to, also version 4.
         */
        VMSTATE_UINT32_V(kernel_port.kick, SGIKonaState, 4),
        VMSTATE_BOOL_V(kernel_port.framing, SGIKonaState, 4),
        VMSTATE_UINT32_V(kernel_port.selector, SGIKonaState, 4),
        VMSTATE_UINT32_V(kernel_port.argc_expected, SGIKonaState, 4),
        VMSTATE_UINT32_V(kernel_port.argc_have, SGIKonaState, 4),
        VMSTATE_UINT32_ARRAY_V(kernel_port.args, SGIKonaState,
                               SGI_COMMAND_PORT_MAX_ARGS, 4),
        VMSTATE_UINT32_V(int_dest_upper, SGIKonaState, 4),
        VMSTATE_UINT32_V(int_dest_lower, SGIKonaState, 4),
        VMSTATE_UINT32_V(int_vectors, SGIKonaState, 4),
        VMSTATE_BOOL_V(reason_page_valid, SGIKonaState, 4),
        VMSTATE_UINT64_V(reason_page, SGIKonaState, 4),
        VMSTATE_BOOL_V(return_page_valid, SGIKonaState, 4),
        VMSTATE_UINT64_V(return_page, SGIKonaState, 4),
        VMSTATE_BOOL_V(activity, SGIKonaState, 4),

        /* A video command still collecting its words, version 5. */
        VMSTATE_BOOL_V(video_open, SGIKonaState, 5),
        VMSTATE_UINT32_V(video_command, SGIKonaState, 5),
        VMSTATE_UINT32_V(video_have, SGIKonaState, 5),
        /* A one-word array until the table grew: the same bytes. */
        VMSTATE_UINT32_V(video_first, SGIKonaState, 5),
        /* A colour-map load still taking its entries, also version 5. */
        VMSTATE_UINT32_V(color_map_count, SGIKonaState, 5),
        VMSTATE_UINT32_V(color_map_remaining, SGIKonaState, 5),
        /*
         * The gamma table and a gamma load still taking its entries,
         * version 6. A version 5 stream has none of them and loads with
         * the reset default, an empty table and no load open.
         */
        VMSTATE_UINT16_ARRAY_V(gamma, SGIKonaState, SGI_KONA_GAMMA_ENTRIES,
                               6),
        VMSTATE_UINT32_V(gamma_start, SGIKonaState, 6),
        VMSTATE_UINT32_V(gamma_count, SGIKonaState, 6),
        VMSTATE_UINT32_V(gamma_remaining, SGIKonaState, 6),
        VMSTATE_UINT32_V(gamma_outside, SGIKonaState, 6),
        /* A cursor load still taking its data words, also version 6. */
        VMSTATE_UINT32_V(cursor_remaining, SGIKonaState, 6),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_kona_board_memory,
        &vmstate_sgi_kona_scanout,
        &vmstate_sgi_kona_x_server,
        &vmstate_sgi_kona_contexts,
        &vmstate_sgi_kona_dma,
        &vmstate_sgi_kona_credit,
        &vmstate_sgi_kona_retrace,
        &vmstate_sgi_kona_mailbox,
        &vmstate_sgi_kona_gl_attrib,
        NULL
    },
};

static void sgi_kona_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI KONA: InfiniteReality graphics pipe";
    dc->vmsd = &vmstate_sgi_kona;
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
