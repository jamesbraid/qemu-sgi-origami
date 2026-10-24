/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI GBE 1.1 architectural display model. The functional-display option
 * selects the reviewed functional policy in Origami's o2-gbe-platform.md.
 * Pipeline phase, byte placement, FIFO reset and PLL validity are model
 * approximations. FIFO reset policy is in o2-gbe-dma-reset.md.
 */
#include "qemu/osdep.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/display/sgi/scanout.h"
#include "hw/i2c/bitbang_i2c.h"
#include "migration/blocker.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "system/dma.h"

#define TYPE_SGI_O2_GBE "sgi-o2-gbe"
OBJECT_DECLARE_SIMPLE_TYPE(SGIO2GBEState, SGI_O2_GBE)
enum { GBE_DMA_CRIME_15, GBE_DMA_LEGACY };

static const QEnumLookup dma_view_lookup = {
    .array = (const char *const[]) { "crime-1.5", "legacy-software" },
    .size = 2,
};
#define FREEZE BIT(31)
#define FIELDS 0xffffffu
#define RGB 0xffffff00u
#define POINTER 0xffffffe1u
#define DID_MASK 0x1ffffu
#define CMAP_COUNT 4608
#define LIST_ENTRIES 4096
#define TIMING_COUNT 10

/* SysBus IRQs 0..3 are one-shot CRIME cause events, not held levels. */
struct SGIO2GBEState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion *ram;
    AddressSpace dma_as;
    SGIScanout scanout;
    bool functional;
    int dma_view;
    uint8_t model_config;
    uint64_t reference_hz, ttl_hz, differential_hz;
    uint32_t ctrl, dot, sysclk;
    uint32_t vt[20], ovr[3], frm[4], did[2];
    uint32_t invalid_timing_reported;
    uint32_t wid[32], cmap[CMAP_COUNT], gmap[256], crs[5], glyph[64];
    uint8_t wid_written[32], cmap_written[CMAP_COUNT], gmap_written[256];
    uint8_t crs_written[3], glyph_written[64];
    uint32_t palette_pending;
    uint16_t lists[2][LIST_ENTRIES];
    uint32_t list_count[2];
    bool plane_fault[2], did_fault;
    uint8_t did_cache[65536], did_valid[65536];
    uint32_t fetched_did;
    uint64_t frames;
    int64_t last_ns;
    uint64_t fraction;
    QEMUTimer *timer;
    qemu_irq events[4], timing[TIMING_COUNT], aux_out[20];
    uint32_t aux_in;
    bool sense, frame_lock, frame_lock_pending, stereo;
    I2CBus *buses[2];
    bitbang_i2c_interface i2c[2];
    uint8_t i2c_latch[2];
    Error *i2c_blocker;
};

static unsigned lo(uint32_t value)
{
    return value & 4095;
}

static unsigned hi(uint32_t value)
{
    return (value >> 12) & 4095;
}

static void unsupported(const char *what)
{
    qemu_log_mask(LOG_UNIMP, "sgi-o2-gbe: unsupported %s\n", what);
}

/* Endpoints equal to the period alias zero; larger comparators never match. */
static void table_input(uint8_t *flags, const char *name)
{
    /* Bit zero records initialization; bit one suppresses repeated reports. */
    if (!(*flags & 3)) {
        unsupported(name);
        *flags |= 2;
    }
}

static unsigned interval(SGIO2GBEState *s, unsigned index, unsigned start,
                         unsigned end, unsigned period)
{
    if (!period || start > period || end > period) {
        if (!(s->invalid_timing_reported & BIT(index))) {
            unsupported("timing interval");
            s->invalid_timing_reported |= BIT(index);
        }
        return 0;
    }
    return (end % period + period - start % period) % period;
}

static unsigned distance(unsigned point, unsigned start, unsigned period)
{
    return (point + period - start % period) % period;
}

static bool in_signal(SGIO2GBEState *s, unsigned index, unsigned p,
                      unsigned period)
{
    uint32_t reg = s->vt[index];
    unsigned n = interval(s, index, hi(reg), lo(reg), period);
    return n && distance(p, hi(reg), period) < n;
}

static bool dma_read(SGIO2GBEState *s, uint64_t addr, void *buf, size_t len)
{
    uint64_t base = s->dma_view == GBE_DMA_LEGACY ? 0 : 0x40000000;
    uint64_t limit = s->dma_view == GBE_DMA_LEGACY ? 0x10000000 : 0x80000000;
    uint64_t size = s->ram ? memory_region_size(s->ram) : 0;
    uint64_t offset;

    if (!len || addr > UINT32_MAX || len > (1ULL << 32) - addr ||
        addr < base || addr + len > limit) {
        goto invalid;
    }
    offset = addr - base;
    if (offset > size || len > size - offset ||
        dma_memory_read(&s->dma_as, offset, buf, len,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        goto invalid;
    }
    return true;

invalid:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-o2-gbe: DMA outside linked RAM at 0x%" PRIx64
                  " (%zu bytes)\n", addr, len);
    return false;
}

static bool plane_held(SGIO2GBEState *s, unsigned plane)
{
    return plane ? s->ovr[0] & BIT(13) : s->frm[0] & BIT(15);
}

static void fetch_list(SGIO2GBEState *s, unsigned plane)
{
    uint32_t shape = plane ? s->ovr[0] : s->frm[0];
    uint32_t control = plane ? s->ovr[1] : s->frm[2];
    unsigned rhs = shape & 31, whole = (shape >> 5) & 255;
    unsigned height = s->frm[1] >> 16;
    uint32_t addr = control & ~31u;
    unsigned count = (whole + !!rhs) * DIV_ROUND_UP(height, 128);
    uint8_t bytes[LIST_ENTRIES * 2];

    s->list_count[plane] = 0;
    s->plane_fault[plane] = false;
    if (plane_held(s, plane) || !(control & 1)) {
        return;
    }
    if (rhs > 15 || !(whole || rhs) || !height ||
        (!plane && ((shape >> 13) & 3) == 3) ||
        count > LIST_ENTRIES || (addr & 8191) + count * 2 > 8192) {
        unsupported("plane geometry or descriptor extent");
        s->plane_fault[plane] = true;
        return;
    }
    if (!dma_read(s, addr, bytes, count * 2)) {
        s->plane_fault[plane] = true;
        return;
    }
    for (unsigned i = 0; i < count; i++) {
        s->lists[plane][i] = lduw_be_p(bytes + i * 2);
    }
    s->list_count[plane] = count;
}

static void vertical_blank(SGIO2GBEState *s)
{
    s->ovr[1] = s->ovr[2];
    s->frm[2] = s->frm[3];
    s->did[0] = s->did[1];
    fetch_list(s, 0);
    fetch_list(s, 1);
    s->fetched_did = s->did[0];
    s->did_fault = false;
    memset(s->did_valid, 0, sizeof(s->did_valid));
    s->frames++;
    sgi_scanout_invalidate_all(&s->scanout);
}

/* Rational clocks retain sub-dot phase without accumulating rounded periods. */
static uint64_t clock_rate(SGIO2GBEState *s, uint64_t *den)
{
    unsigned source = s->ctrl >> 28 & 3;
    uint64_t num;

    *den = 1;
    if (source != 3) {
        return source == 0 ? s->ttl_hz : source == 1 ?
               s->differential_hz : 0;
    }
    *den = (((s->dot >> 8) & 63) + 1) << ((s->dot >> 14) & 3);
    num = s->reference_hz * ((s->dot & 255) + 1);
    if (!(s->dot & BIT(20)) || (s->dot & (3u << 22))) {
        *den = 1;
        return 0;
    }
    /* Equal frequencies share a phase denominator despite PLL encodings. */
    uint64_t a = num, b = *den;
    while (b) {
        uint64_t remainder = a % b;
        a = b;
        b = remainder;
    }
    *den /= a;
    return num / a;
}

static void pll_status(SGIO2GBEState *s)
{
    uint64_t num = s->reference_hz * ((s->dot & 255) + 1);
    unsigned den = ((s->dot >> 8) & 63) + 1;

    s->dot &= ~(3u << 22);
    if (num < 80000000ull * den || num > 220000000ull * den) {
        s->dot |= 3u << 22;
    } else if (!(s->dot & BIT(20))) {
        s->dot |= BIT(23);
    }
}

static void outputs(SGIO2GBEState *s)
{
    unsigned x = lo(s->vt[0]), y = hi(s->vt[0]);
    unsigned xp = lo(s->vt[1]), yp = hi(s->vt[1]);
    unsigned control = s->vt[6];
    bool levels[TIMING_COUNT] = { 0 };

    if (xp && yp) {
        levels[0] = in_signal(s, 2, y, yp);
        levels[1] = in_signal(s, 3, x, xp);
        levels[2] = in_signal(s, 4, y, yp);
        levels[3] = in_signal(s, 5, x, xp);
        levels[4] = in_signal(s, 10, x, xp);
        levels[5] = in_signal(s, 11, y, yp);
        levels[6] = in_signal(s, 12, x, xp);
    }
    levels[0] = !(control & 2) && (levels[0] ^ !!(control & 1));
    levels[1] = !(control & 8) && (levels[1] ^ !!(control & 4));
    levels[7] = s->stereo || (control & 64);
    levels[8] = !!(s->ctrl & BIT(26));
    /* Composite sync uses its independently programmed force controls. */
    levels[9] = (control & 16) || (!(control & 32) &&
                                  (levels[0] ^ levels[1]));
    for (unsigned i = 0; i < TIMING_COUNT; i++) {
        qemu_set_irq(s->timing[i], levels[i]);
    }
}

static void line_start(SGIO2GBEState *s)
{
    unsigned y = hi(s->vt[0]), yp = hi(s->vt[1]);
    unsigned matches[] = { hi(s->vt[8]), lo(s->vt[8]),
                           hi(s->vt[9]), lo(s->vt[9]) };

    if (hi(s->vt[4]) <= yp && y == hi(s->vt[4]) % yp) {
        vertical_blank(s);
    }
    if (lo(s->vt[7]) <= yp && y == lo(s->vt[7]) % yp) {
        s->stereo = !s->stereo;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (matches[i] <= yp && y == matches[i] % yp) {
            qemu_irq_pulse(s->events[i]);
        }
    }
}

static void sync_clock(SGIO2GBEState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t den, num = clock_rate(s, &den);
    uint64_t elapsed = MAX(now - s->last_ns, 0);
    unsigned xp = lo(s->vt[1]), yp = hi(s->vt[1]);
    uint64_t ticks;
    __uint128_t scaled;

    s->last_ns = now;
    if (!s->functional || !num || !xp || !yp || (s->vt[0] & FREEZE)) {
        return;
    }
    den *= NANOSECONDS_PER_SECOND;
    scaled = (__uint128_t)elapsed * num + s->fraction;
    ticks = scaled / den;
    s->fraction = scaled % den;
    while (ticks) {
        unsigned x = lo(s->vt[0]) % xp, y = hi(s->vt[0]) % yp;
        unsigned step = MIN(ticks, xp - x);
        x += step;
        ticks -= step;
        if (x == xp) {
            x = 0;
            y = (y + 1) % yp;
            if (s->frame_lock_pending) {
                y = hi(s->vt[7]) % yp;
                s->frame_lock_pending = false;
            }
            s->vt[0] = y << 12;
            line_start(s);
        } else {
            s->vt[0] = y << 12 | x;
        }
    }
    outputs(s);
}

static void arm(SGIO2GBEState *s)
{
    uint64_t den, num = clock_rate(s, &den);
    unsigned xp = lo(s->vt[1]), yp = hi(s->vt[1]);
    unsigned x = lo(s->vt[0]), next;
    static const unsigned horizontal[] = { 3, 5, 10, 12, 13, 15, 17, 18 };
    __uint128_t delta;

    timer_del(s->timer);
    if (!s->functional || !num || !xp || !yp || (s->vt[0] & FREEZE)) {
        return;
    }
    next = xp - x;
    for (unsigned i = 0; i < ARRAY_SIZE(horizontal); i++) {
        unsigned r = s->vt[horizontal[i]];
        unsigned a = lo(r), b = hi(r);
        if (a > x && a < xp) {
            next = MIN(next, a - x);
        }
        if (b > x && b < xp) {
            next = MIN(next, b - x);
        }
    }
    delta = (__uint128_t)next * den * NANOSECONDS_PER_SECOND;
    delta -= MIN(delta, s->fraction);
    timer_mod(s->timer, s->last_ns + MAX((delta + num - 1) / num, 1));
}

static void timer_tick(void *opaque)
{
    SGIO2GBEState *s = opaque;
    sync_clock(s);
    arm(s);
}

static void geometry(SGIO2GBEState *s)
{
    if (!s->functional) {
        return;
    }
    SGIScanoutMode mode = {
        .width = interval(s, 15, lo(s->vt[15]), hi(s->vt[15]), lo(s->vt[1])),
        .height = interval(s, 16, lo(s->vt[16]), hi(s->vt[16]), hi(s->vt[1])),
        /* The GBE counter timer owns retrace and descriptor fetch ordering. */
        .clock_hz = 0,
    };
    if (mode.width != s->scanout.mode.width ||
        mode.height != s->scanout.mode.height) {
        sgi_scanout_set_mode(&s->scanout, &mode);
    }
    sgi_scanout_invalidate_all(&s->scanout);
}

typedef struct GBEPlaneRow {
    uint64_t address;
    bool valid;
    uint8_t bytes[512];
} GBEPlaneRow;

static uint32_t plane_pixel(SGIO2GBEState *s, unsigned plane,
                             uint64_t stream, GBEPlaneRow *cache)
{
    uint32_t shape = plane ? s->ovr[0] : s->frm[0];
    uint32_t ctl = plane ? s->ovr[1] : s->frm[2];
    unsigned depth = plane ? 0 : (shape >> 13) & 3;
    unsigned bpp = 1u << depth;
    unsigned width = (((shape >> 5) & 255) * 512 + (shape & 31) * 32) / bpp;
    unsigned tiles = ((shape >> 5) & 255) + !!(shape & 31);
    uint64_t x, y, index, addr;
    uint16_t entry;

    if (plane_held(s, plane) || !(ctl & 1) || s->plane_fault[plane] ||
        !s->list_count[plane]) {
        return 0;
    }
    if (!width || depth == 3) {
        s->plane_fault[plane] = true;
        unsupported("plane depth or storage width");
        return 0;
    }
    x = stream % width;
    y = stream / width;
    index = (y / 128) * tiles + x / (512 / bpp);
    if (y >= (s->frm[1] >> 16) || index >= s->list_count[plane]) {
        unsupported("pixel stream beyond storage height");
        s->plane_fault[plane] = true;
        return 0;
    }
    entry = s->lists[plane][index];
    if (!entry) {
        return 0;
    }
    addr = ((uint64_t)entry << 16) + (y % 128) * 512;
    if (!cache->valid || cache->address != addr) {
        if (!dma_read(s, addr, cache->bytes, sizeof(cache->bytes))) {
            s->plane_fault[plane] = true;
            return 0;
        }
        cache->address = addr;
        cache->valid = true;
    }
    unsigned offset = (x % (512 / bpp)) * bpp;
    return bpp == 1 ? cache->bytes[offset] : bpp == 2 ?
           lduw_be_p(cache->bytes + offset) : ldl_be_p(cache->bytes + offset);
}

static bool did_read(SGIO2GBEState *s, unsigned offset, unsigned size,
                      uint32_t *result)
{
    if (offset > 65536 - size) {
        goto fail;
    }
    for (unsigned pos = offset; pos < offset + size;) {
        unsigned first = pos;
        if (s->did_valid[pos]) {
            pos++;
            continue;
        }
        while (pos < offset + size && !s->did_valid[pos]) {
            pos++;
        }
        if (!dma_read(s, ((uint64_t)(s->fetched_did & 65535) << 16) + first,
                      s->did_cache + first, pos - first)) {
            goto fail;
        }
        memset(s->did_valid + first, 1, pos - first);
    }
    *result = size == 4 ? ldl_be_p(s->did_cache + offset) :
                        lduw_be_p(s->did_cache + offset);
    return true;
fail:
    s->did_fault = true;
    unsupported("DID table extent");
    return false;
}

static unsigned display_id(SGIO2GBEState *s, unsigned raster_x,
                            unsigned raster_y)
{
    unsigned xp = lo(s->vt[1]), yp = hi(s->vt[1]);
    int x, y, last = -1;
    uint32_t entry = 0;
    unsigned offset = 0;

    if (!(s->fetched_did & BIT(16)) || s->did_fault) {
        return 0;
    }
    if (xp <= 20) {
        unsupported("DID horizontal alignment");
        return 0;
    }
    x = (int)distance(raster_x, lo(s->vt[17]), xp) - 20;
    y = (sextract32(s->vt[17], 12, 12) +
         distance(raster_y, hi(s->vt[4]), yp) + 1) & 4095;
    if (x < 0 || x > 2047 || y > 2047) {
        unsupported("DID coordinate");
        return 0;
    }
    for (unsigned i = 0; i < 16384; i++) {
        if (!did_read(s, i * 4, 4, &entry)) {
            return 0;
        }
        int end = entry & 2047;
        if (end <= last) {
            goto malformed;
        }
        if (y <= end) {
            offset = ((entry >> 19) & 127) * 512 +
                     ((entry >> 11) & 255) * 2;
            break;
        }
        last = end;
        if (i == 16383) {
            goto malformed;
        }
    }
    last = -1;
    unsigned limit = (offset & ~511u) + 512;
    unsigned found = 0;
    bool selected = false;
    /* Validate the terminator even if the requested X precedes it. */
    for (; offset + 2 <= limit; offset += 2) {
        if (!did_read(s, offset, 2, &entry)) {
            return 0;
        }
        int end = entry & 2047;
        if (end <= last) {
            goto malformed;
        }
        if (!selected && x <= end) {
            found = entry >> 11;
            selected = true;
        }
        if (end == 2047) {
            return found;
        }
        last = end;
    }
malformed:
    s->did_fault = true;
    unsupported("malformed DID runs");
    return 0;
}

static uint32_t color_map(SGIO2GBEState *s, unsigned index)
{
    if (index >= CMAP_COUNT) {
        unsupported("color-map index");
        return 0;
    }
    table_input(&s->cmap_written[index], "uninitialized color-map entry");
    return s->cmap[index] >> 8;
}

static uint32_t gamma_map(SGIO2GBEState *s, uint32_t color)
{
    unsigned r = color >> 16 & 255, g = color >> 8 & 255, b = color & 255;
    table_input(&s->gmap_written[r], "uninitialized gamma entry");
    table_input(&s->gmap_written[g], "uninitialized gamma entry");
    table_input(&s->gmap_written[b], "uninitialized gamma entry");
    return (s->gmap[r] >> 8 & 0xff0000) |
           (s->gmap[g] >> 8 & 0x00ff00) |
           (s->gmap[b] >> 8 & 0x0000ff);
}

static unsigned expand(unsigned value, unsigned bits)
{
    switch (bits) {
    case 2: return value * 85;
    case 3: return value << 5 | value << 2 | value >> 1;
    case 4: return value * 17;
    case 5: return value << 3 | value >> 2;
    default:
        g_assert_not_reached();
    }
}

static uint32_t normal_color(SGIO2GBEState *s, uint32_t pixel, unsigned id)
{
    unsigned wid = s->wid[id], buffer = wid & 3, mode = wid >> 2 & 7;
    unsigned depth = s->frm[0] >> 13 & 3;
    unsigned bits = 8u << depth, cm = wid >> 5 & 31;
    unsigned need[] = { 8, 16, 8, 16, 16, 32 };
    unsigned r, g, b;
    uint32_t color;

    table_input(&s->wid_written[id], "uninitialized WID");
    if (mode >= 6 || !buffer || depth == 3) {
        goto invalid;
    }
    if (buffer != 3) {
        bits /= 2;
        pixel = buffer == 2 ? pixel >> bits : pixel & MAKE_64BIT_MASK(0, bits);
    }
    if (bits < need[mode]) {
        goto invalid;
    }
    switch (mode) {
    case 0:
        color = color_map(s, cm << 8 | (pixel & 255));
        break;
    case 1:
        color = color_map(s, pixel & 4095);
        break;
    case 2:
        r = expand(pixel >> 5 & 7, 3);
        g = expand(pixel >> 2 & 7, 3);
        b = expand(pixel & 3, 2);
        goto rgb;
    case 3:
        r = expand(pixel >> 12 & 15, 4);
        g = expand(pixel >> 8 & 15, 4);
        b = expand(pixel >> 4 & 15, 4);
        goto rgb;
    case 4:
        r = expand(pixel >> 10 & 31, 5);
        g = expand(pixel >> 5 & 31, 5);
        b = expand(pixel & 31, 5);
        goto rgb;
    case 5:
        r = pixel >> 24;
        g = pixel >> 16 & 255;
        b = pixel >> 8 & 255;
    rgb:
        color = cm ? (color_map(s, cm << 8 | r) & 0xff0000) |
                     (color_map(s, cm << 8 | g) & 0x00ff00) |
                     (color_map(s, cm << 8 | b) & 0x0000ff) :
                     r << 16 | g << 8 | b;
        break;
    default:
        g_assert_not_reached();
    }
    return wid & BIT(10) ? color : gamma_map(s, color);
invalid:
    unsupported("normal pixel mode or buffer depth");
    return 0;
}

static uint32_t cursor_color(SGIO2GBEState *s, unsigned x, unsigned y,
                              uint32_t under)
{
    unsigned xp = lo(s->vt[1]), yp = hi(s->vt[1]);
    int cx = (int16_t)s->crs[0], cy = (int16_t)(s->crs[0] >> 16);
    unsigned index;

    if (!(s->crs[1] & 1)) {
        return under;
    }
    if (xp <= 54) {
        unsupported("cursor horizontal alignment");
        return under;
    }
    cx += (int)distance(lo(s->vt[15]) % xp, lo(s->vt[18]), xp) - 54;
    cy += sextract32(s->vt[18], 12, 12) +
          distance(lo(s->vt[16]) % yp, hi(s->vt[4]), yp);
    if (s->crs[1] & 2) {
        return (x == cx || y == cy) ? s->crs[2] >> 8 : under;
    }
    int gx = (int)x - (cx - 31), gy = (int)y - (cy - 31);
    if (gx < 0 || gy < 0 || gx >= 32 || gy >= 32) {
        return under;
    }
    index = s->glyph[gy * 2 + gx / 16] >> (30 - (gx % 16) * 2) & 3;
    return index ? s->crs[index + 1] >> 8 : under;
}

static void resolve_row(void *opaque, unsigned y, uint32_t *dst)
{
    SGIO2GBEState *s = opaque;
    unsigned xp = lo(s->vt[1]), yp = hi(s->vt[1]);
    unsigned w = s->scanout.mode.width;
    unsigned producer = interval(s, 13, hi(s->vt[13]), lo(s->vt[13]), xp);
    unsigned ph = interval(s, 14, hi(s->vt[14]), lo(s->vt[14]), yp);
    GBEPlaneRow cache[2] = { 0 };
    unsigned ry = yp ? (lo(s->vt[16]) + y) % yp : 0;
    unsigned py = yp ? distance(ry, hi(s->vt[14]), yp) : 0;

    if (producer && producer + 2 <= xp) {
        producer += 2;
    } else {
        producer = 0;
    }
    for (unsigned x = 0; x < w; x++) {
        uint32_t color = 0;
        unsigned rx = xp ? (lo(s->vt[15]) + x) % xp : 0;
        unsigned px = xp ? distance(rx, (hi(s->vt[13]) + 19) % xp, xp) : 0;
        if (producer >= 3 && px < producer && py < ph) {
            uint64_t stream = (uint64_t)py * producer + px;
            uint32_t normal = plane_pixel(s, 0, stream, &cache[0]);
            uint32_t overlay = plane_pixel(s, 1, stream, &cache[1]);
            color = normal_color(s, normal, display_id(s, rx, ry));
            if (overlay) {
                color = gamma_map(s, color_map(s, 4352 + overlay));
            }
        }
        if (!(producer >= 3 && px < producer && py < ph)) {
            color = normal_color(s, 0, display_id(s, rx, ry));
        }
        dst[x] = cursor_color(s, x, y, color);
    }
}

static const SGIHeadOps scanout_ops = { .resolve_row = resolve_row };

static void i2c_migration(SGIO2GBEState *s)
{
    bool busy = s->i2c[0].state != STOPPED || s->i2c[1].state != STOPPED ||
                s->i2c[0].current_addr >= 0 || s->i2c[1].current_addr >= 0;
    if (busy && !s->i2c_blocker) {
        Error *err = NULL;

        error_setg(&s->i2c_blocker, "GBE I2C transfer is active");
        if (migrate_add_blocker_internal(&s->i2c_blocker, &err) < 0) {
            /* pre_save rejects a transfer started while migration is busy. */
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-o2-gbe: %s\n",
                          error_get_pretty(err));
            error_free(err);
        }
    } else if (!busy && s->i2c_blocker) {
        migrate_del_blocker(&s->i2c_blocker);
    }
}

static void i2c_write(SGIO2GBEState *s, unsigned bus, uint8_t value)
{
    bitbang_i2c_interface *b = &s->i2c[bus];
    unsigned lines = ~value & 3;
    if (!(lines & 2)) {
        bitbang_i2c_set(b, BITBANG_I2C_SCL, 0);
    }
    bitbang_i2c_set(b, BITBANG_I2C_SDA, lines & 1);
    if (lines & 2) {
        bitbang_i2c_set(b, BITBANG_I2C_SCL, 1);
    }
    s->i2c_latch[bus] = value & 3;
    i2c_migration(s);
}

static void aux_outputs(SGIO2GBEState *s)
{
    for (unsigned i = 0; i < 10; i++) {
        qemu_set_irq(s->aux_out[i * 2], !!(s->ctrl & BIT(6 + 2 * i)));
        qemu_set_irq(s->aux_out[i * 2 + 1],
                     !(s->ctrl & BIT(7 + 2 * i)));
    }
}

static bool accepts(void *opaque, hwaddr addr, unsigned size,
                      bool write, MemTxAttrs attrs)
{
    if (size == 4 && !(addr & 3)) {
        return true;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-o2-gbe: unsupported access %s at 0x%06" HWADDR_PRIx
                  " (%u bytes)\n", write ? "write" : "read", addr, size);
    return false;
}

static uint64_t read_reg(void *opaque, hwaddr addr, unsigned size)
{
    SGIO2GBEState *s = opaque;
    unsigned index;
    sync_clock(s);
    arm(s);
    if (addr >= 0x10000 && addr < 0x10050) {
        return s->vt[(addr - 0x10000) / 4];
    }
    if (addr >= 0x20000 && addr < 0x2000c) {
        return s->ovr[(addr - 0x20000) / 4];
    }
    if (addr >= 0x30000 && addr < 0x30010) {
        return s->frm[(addr - 0x30000) / 4];
    }
    if (addr >= 0x40000 && addr < 0x40008) {
        return s->did[(addr - 0x40000) / 4];
    }
    if (addr >= 0x48000 && addr < 0x48080) {
        index = (addr - 0x48000) / 4;
        table_input(&s->wid_written[index], "uninitialized WID");
        return s->wid[index];
    }
    if (addr >= 0x50000 && addr < 0x54800) {
        index = (addr - 0x50000) / 4;
        table_input(&s->cmap_written[index], "uninitialized color-map entry");
        return s->cmap[index];
    }
    if (addr >= 0x60000 && addr < 0x60400) {
        index = (addr - 0x60000) / 4;
        table_input(&s->gmap_written[index], "uninitialized gamma entry");
        return s->gmap[index];
    }
    if (addr >= 0x70000 && addr < 0x70014) {
        return s->crs[(addr - 0x70000) / 4];
    }
    if (addr >= 0x78000 && addr < 0x78100) {
        return s->glyph[(addr - 0x78000) / 4];
    }
    switch (addr) {
    case 0: {
        uint32_t result = s->ctrl | (s->sense ? BIT(4) : 0);
        for (unsigned i = 0; i < 10; i++) {
            if (s->ctrl & BIT(7 + 2 * i)) {
                result &= ~BIT(6 + 2 * i);
                result |= !!(s->aux_in & BIT(i)) << (6 + 2 * i);
            }
        }
        return result;
    }
    case 4: return s->dot;
    case 8:
    case 0x10: {
        unsigned bus = addr == 8 ? 0 : 1;
        bitbang_i2c_interface *b = &s->i2c[bus];
        return (~((b->last_clock << 1) | (b->last_data & b->device_out))) & 3;
    }
    case 0xc: return s->sysclk;
    case 0x14: return 0x666;
    case 0x58000: return MIN(s->palette_pending, 63);
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-o2-gbe: unsupported access read at 0x%06"
                      HWADDR_PRIx "\n", addr);
        return UINT32_MAX;
    }
}

static void write_shape(SGIO2GBEState *s, unsigned plane, uint32_t value)
{
    uint32_t *shape = plane ? &s->ovr[0] : &s->frm[0];
    uint32_t bit = plane ? BIT(13) : BIT(15);

    if ((value & bit) && !s->functional) {
        unsupported(plane ? "overlay diagnostic reset" :
                            "frame diagnostic reset");
        return;
    }
    if ((value & bit) && !(*shape & bit)) {
        s->list_count[plane] = 0;
        s->plane_fault[plane] = false;
        memset(s->lists[plane], 0, sizeof(s->lists[plane]));
    }
    *shape = value & ((bit - 1) | (s->functional ? bit : 0));
}

static void write_reg(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    SGIO2GBEState *s = opaque;
    uint32_t v = value;
    unsigned index;
    uint64_t old_den, old_num = clock_rate(s, &old_den);
    sync_clock(s);
    if (addr >= 0x10000 && addr < 0x10050) {
        index = (addr - 0x10000) / 4;
        if (index == 19 && (v & FIELDS)) {
            unsupported("capture alignment");
            goto done;
        }
        v &= index == 0 ? FREEZE | FIELDS : index == 6 ? 127 : FIELDS;
        if (s->vt[index] != v) {
            /* Repeated evaluation of unchanged invalid timing is noisy. */
            s->invalid_timing_reported &= ~BIT(index);
            if (index == 1) {
                if (lo(s->vt[1]) != lo(v)) {
                    s->invalid_timing_reported &= ~(BIT(3) | BIT(5) |
                        BIT(10) | BIT(12) | BIT(13) | BIT(15));
                }
                if (hi(s->vt[1]) != hi(v)) {
                    s->invalid_timing_reported &= ~(BIT(2) | BIT(4) |
                        BIT(11) | BIT(14) | BIT(16));
                }
            }
        }
        s->vt[index] = v;
        if (index <= 1 && !(s->vt[0] & FREEZE) &&
            lo(s->vt[1]) && hi(s->vt[1])) {
            s->vt[0] = (lo(s->vt[0]) % lo(s->vt[1])) |
                       (hi(s->vt[0]) % hi(s->vt[1])) << 12;
        }
        if (index == 1 || index == 13) {
            unsigned xp = lo(s->vt[1]);
            unsigned raw = interval(s, 13, hi(s->vt[13]), lo(s->vt[13]), xp);
            if (raw && raw + 2 > xp) {
                unsupported("horizontal producer length");
            }
        }
        if (index == 1 || index == 15 || index == 16) {
            geometry(s);
        }
        if (!(s->vt[0] & FREEZE) &&
            (!lo(s->vt[1]) || !hi(s->vt[1]))) {
            unsupported("zero raster period");
        }
        goto done;
    }
    if (addr >= 0x20000 && addr < 0x2000c) {
        index = (addr - 0x20000) / 4;
        if (!index) {
            write_shape(s, 1, v);
        } else {
            s->ovr[index] = v & POINTER;
        }
        goto done;
    }
    if (addr >= 0x30000 && addr < 0x30010) {
        index = (addr - 0x30000) / 4;
        if (!index) {
            write_shape(s, 0, v);
        } else {
            s->frm[index] = v & (index >= 2 ? POINTER : 0xffff0000u);
        }
        goto done;
    }
    if (addr >= 0x40000 && addr < 0x40008) {
        s->did[(addr - 0x40000) / 4] = v & DID_MASK;
        goto done;
    }
    if (addr >= 0x48000 && addr < 0x48080) {
        index = (addr - 0x48000) / 4;
        s->wid[index] = v & 8191;
        s->wid_written[index] = 1;
        goto done;
    }
    if (addr >= 0x50000 && addr < 0x54800) {
        index = (addr - 0x50000) / 4;
        s->palette_pending++;
        s->cmap[index] = v & RGB;
        s->cmap_written[index] = 1;
        s->palette_pending--;
        goto done;
    }
    if (addr >= 0x60000 && addr < 0x60400) {
        index = (addr - 0x60000) / 4;
        s->gmap[index] = v & RGB;
        s->gmap_written[index] = 1;
        goto done;
    }
    if (addr >= 0x70000 && addr < 0x70014) {
        index = (addr - 0x70000) / 4;
        s->crs[index] = v & (!index ? UINT32_MAX : index == 1 ? 3 : RGB);
        if (index >= 2) {
            s->crs_written[index - 2] = 1;
        }
        goto done;
    }
    if (addr >= 0x78000 && addr < 0x78100) {
        index = (addr - 0x78000) / 4;
        s->glyph[index] = v;
        s->glyph_written[index] = 1;
        goto done;
    }
    switch (addr) {
    case 0:
        if ((v >> 28 & 3) == 2) {
            unsupported("clock source 2");
        }
        s->ctrl = (v & 0x3fffffc0u) | 1;
        aux_outputs(s);
        break;
    case 4:
        if (v & (3u << 24)) {
            unsupported("PLL test controls");
            break;
        }
        s->dot = v & 0x10ffffu;
        pll_status(s);
        break;
    case 8:
        i2c_write(s, 0, v);
        break;
    case 0x10:
        i2c_write(s, 1, v);
        break;
    case 0xc:
        if ((v & 0x7f0f) != 8) {
            unsupported("system-clock test or analog controls");
        }
        break;
    case 0x14:
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-o2-gbe: unsupported access write at 0x%06"
                      HWADDR_PRIx "\n", addr);
        break;
    }
done:
    if (addr == 0 || addr == 4) {
        uint64_t new_den, new_num = clock_rate(s, &new_den);
        if (new_num != old_num || new_den != old_den) {
            s->fraction = 0;
        }
    }
    if (s->functional) {
        sgi_scanout_invalidate_all(&s->scanout);
    }
    outputs(s);
    arm(s);
}

static const MemoryRegionOps gbe_ops = {
    .read = read_reg,
    .write = write_reg,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8,
               .unaligned = true, .accepts = accepts },
    .impl = { .min_access_size = 4, .max_access_size = 8, .unaligned = true },
};

static void input(void *opaque, int n, int level)
{
    SGIO2GBEState *s = opaque;
    sync_clock(s);
    if (n < 10) {
        s->aux_in = deposit32(s->aux_in, n, 1, !!level);
    } else if (n == 10) {
        s->sense = !!level;
    } else {
        if (level && !s->frame_lock) {
            unsigned yp = hi(s->vt[1]);
            if (yp && hi(s->vt[7]) <= yp) {
                s->frame_lock_pending = true;
            } else {
                unsupported("frame-lock preset");
            }
        }
        s->frame_lock = !!level;
    }
    arm(s);
}

static void reset_enter(Object *obj, ResetType type)
{
    SGIO2GBEState *s = SGI_O2_GBE(obj);
    timer_del(s->timer);
    s->ctrl = 0x03ffffc1;
    s->dot = 0;
    pll_status(s);
    s->sysclk = 8;
    s->invalid_timing_reported = 0;
    memset(s->vt, 0, sizeof(s->vt));
    s->vt[0] = FREEZE;
    memset(s->ovr, 0, sizeof(s->ovr));
    memset(s->frm, 0, sizeof(s->frm));
    memset(s->did, 0, sizeof(s->did));
    memset(s->wid, 0, sizeof(s->wid));
    memset(s->cmap, 0, sizeof(s->cmap));
    memset(s->gmap, 0, sizeof(s->gmap));
    memset(s->crs, 0, sizeof(s->crs));
    memset(s->glyph, 0, sizeof(s->glyph));
    memset(s->wid_written, 0, sizeof(s->wid_written));
    memset(s->cmap_written, 0, sizeof(s->cmap_written));
    memset(s->gmap_written, 0, sizeof(s->gmap_written));
    memset(s->crs_written, 0, sizeof(s->crs_written));
    memset(s->glyph_written, 0, sizeof(s->glyph_written));
    memset(s->lists, 0, sizeof(s->lists));
    memset(s->list_count, 0, sizeof(s->list_count));
    memset(s->plane_fault, 0, sizeof(s->plane_fault));
    memset(s->did_cache, 0, sizeof(s->did_cache));
    memset(s->did_valid, 0, sizeof(s->did_valid));
    s->fetched_did = s->frames = s->fraction = s->palette_pending = 0;
    s->did_fault = s->frame_lock_pending = s->stereo = false;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned i = 0; i < 2; i++) {
        bitbang_i2c_interface *b = &s->i2c[i];
        if (i2c_bus_busy(b->bus)) {
            i2c_end_transfer(b->bus);
        }
        b->state = STOPPED;
        b->buffer = 0;
        b->current_addr = -1;
        bitbang_i2c_init(b, s->buses[i]);
        i2c_write(s, i, 3);
    }
    i2c_migration(s);
    aux_outputs(s);
    outputs(s);
    if (s->functional) {
        geometry(s);
    }
}

static int pre_save(void *opaque)
{
    SGIO2GBEState *s = opaque;
    sync_clock(s);
    return (s->i2c[0].state != STOPPED || s->i2c[1].state != STOPPED ||
            s->i2c[0].current_addr >= 0 || s->i2c[1].current_addr >= 0) ?
            -EBUSY : 0;
}

static int post_load(void *opaque, int version)
{
    SGIO2GBEState *s = opaque;
    s->invalid_timing_reported = 0;
    if (s->list_count[0] > LIST_ENTRIES ||
        s->list_count[1] > LIST_ENTRIES ||
        (plane_held(s, 0) && s->list_count[0]) ||
        (plane_held(s, 1) && s->list_count[1])) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < 2; i++) {
        bitbang_i2c_interface *b = &s->i2c[i];
        b->state = STOPPED;
        b->buffer = 0;
        b->current_addr = -1;
        b->last_data = ~s->i2c_latch[i] & 1;
        b->last_clock = (~s->i2c_latch[i] >> 1) & 1;
        b->device_out = 1;
    }
    if (s->functional) {
        geometry(s);
    }
    outputs(s);
    aux_outputs(s);
    arm(s);
    return 0;
}

static const VMStateDescription vmstate_gbe = {
    .name = TYPE_SGI_O2_GBE,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = pre_save,
    .post_load = post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(model_config, SGIO2GBEState),
        VMSTATE_UINT64_EQUAL(reference_hz, SGIO2GBEState),
        VMSTATE_UINT64_EQUAL(ttl_hz, SGIO2GBEState),
        VMSTATE_UINT64_EQUAL(differential_hz, SGIO2GBEState),
        VMSTATE_UINT32(ctrl, SGIO2GBEState),
        VMSTATE_UINT32(dot, SGIO2GBEState),
        VMSTATE_UINT32(sysclk, SGIO2GBEState),
        VMSTATE_UINT32_ARRAY(vt, SGIO2GBEState, 20),
        VMSTATE_UINT32_ARRAY(ovr, SGIO2GBEState, 3),
        VMSTATE_UINT32_ARRAY(frm, SGIO2GBEState, 4),
        VMSTATE_UINT32_ARRAY(did, SGIO2GBEState, 2),
        VMSTATE_UINT32_ARRAY(wid, SGIO2GBEState, 32),
        VMSTATE_UINT32_ARRAY(cmap, SGIO2GBEState, CMAP_COUNT),
        VMSTATE_UINT32_ARRAY(gmap, SGIO2GBEState, 256),
        VMSTATE_UINT32_ARRAY(crs, SGIO2GBEState, 5),
        VMSTATE_UINT32_ARRAY(glyph, SGIO2GBEState, 64),
        VMSTATE_UINT8_ARRAY(wid_written, SGIO2GBEState, 32),
        VMSTATE_UINT8_ARRAY(cmap_written, SGIO2GBEState, CMAP_COUNT),
        VMSTATE_UINT8_ARRAY(gmap_written, SGIO2GBEState, 256),
        VMSTATE_UINT8_ARRAY(crs_written, SGIO2GBEState, 3),
        VMSTATE_UINT8_ARRAY(glyph_written, SGIO2GBEState, 64),
        VMSTATE_UINT16_2DARRAY(lists, SGIO2GBEState, 2, LIST_ENTRIES),
        VMSTATE_UINT32_ARRAY(list_count, SGIO2GBEState, 2),
        VMSTATE_BOOL_ARRAY(plane_fault, SGIO2GBEState, 2),
        VMSTATE_BOOL(did_fault, SGIO2GBEState),
        VMSTATE_UINT8_ARRAY(did_cache, SGIO2GBEState, 65536),
        VMSTATE_UINT8_ARRAY(did_valid, SGIO2GBEState, 65536),
        VMSTATE_UINT32(fetched_did, SGIO2GBEState),
        VMSTATE_UINT64(frames, SGIO2GBEState),
        VMSTATE_INT64(last_ns, SGIO2GBEState),
        VMSTATE_UINT64(fraction, SGIO2GBEState),
        VMSTATE_UINT32(aux_in, SGIO2GBEState),
        VMSTATE_BOOL(sense, SGIO2GBEState),
        VMSTATE_BOOL(frame_lock, SGIO2GBEState),
        VMSTATE_BOOL(frame_lock_pending, SGIO2GBEState),
        VMSTATE_BOOL(stereo, SGIO2GBEState),
        VMSTATE_UINT8_ARRAY(i2c_latch, SGIO2GBEState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void realize(DeviceState *dev, Error **errp)
{
    SGIO2GBEState *s = SGI_O2_GBE(dev);
    s->model_config = s->functional | s->dma_view << 1;
    if (s->reference_hz > 1000000000 || !s->reference_hz ||
        s->ttl_hz > 1000000000 || s->differential_hz > 1000000000) {
        error_setg(errp, "GBE clock inputs must be at most 1 GHz, with a "
                         "nonzero reference");
        return;
    }
    if (!s->functional) {
        return;
    }
    if (!s->ram || !memory_region_size(s->ram) ||
        memory_region_size(s->ram) > 3 * GiB) {
        error_setg(errp, "GBE functional-display requires populated DMA RAM");
        return;
    }
    address_space_init(&s->dma_as, s->ram, "sgi-o2-gbe.dma");
    sgi_scanout_init(&s->scanout, dev, &scanout_ops, s);
}

static void unrealize(DeviceState *dev)
{
    SGIO2GBEState *s = SGI_O2_GBE(dev);
    timer_del(s->timer);
    migrate_del_blocker(&s->i2c_blocker);
    if (s->functional) {
        sgi_scanout_cleanup(&s->scanout);
        address_space_destroy(&s->dma_as);
    }
}

static void dma_view_property(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    SGIO2GBEState *s = SGI_O2_GBE(obj);

    visit_type_enum(v, name, &s->dma_view, &dma_view_lookup, errp);
}

static const PropertyInfo prop_dma_view = {
    .type = "GBEDMAView",
    .description = "GBE read-DMA address domain",
    .enum_table = &dma_view_lookup,
    .get = dma_view_property,
    .set = dma_view_property,
};

static const Property properties[] = {
    DEFINE_PROP("dma-view", SGIO2GBEState, dma_view, prop_dma_view, int),
    DEFINE_PROP_BOOL("functional-display", SGIO2GBEState, functional, false),
    DEFINE_PROP_UINT64("reference-hz", SGIO2GBEState, reference_hz, 20000000),
    DEFINE_PROP_UINT64("external-ttl-hz", SGIO2GBEState, ttl_hz, 0),
    DEFINE_PROP_UINT64("external-differential-hz", SGIO2GBEState,
                       differential_hz, 0),
    DEFINE_PROP_LINK("ram", SGIO2GBEState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static void init(Object *obj)
{
    SGIO2GBEState *s = SGI_O2_GBE(obj);
    DeviceState *dev = DEVICE(obj);
    memory_region_init_io(&s->mmio, obj, &gbe_ops, s, TYPE_SGI_O2_GBE,
                          16 * MiB);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    for (unsigned i = 0; i < 4; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->events[i]);
    }
    qdev_init_gpio_in(dev, input, 12);
    qdev_init_gpio_out_named(dev, s->timing, "timing", TIMING_COUNT);
    qdev_init_gpio_out_named(dev, s->aux_out, "auxiliary", 20);
    s->aux_in = 1023;
    s->sense = true;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, timer_tick, s);
    s->buses[0] = i2c_init_bus(dev, "i2c");
    s->buses[1] = i2c_init_bus(dev, "i2cfp");
    for (unsigned i = 0; i < 2; i++) {
        bitbang_i2c_init(&s->i2c[i], s->buses[i]);
        s->i2c[i].current_addr = -1;
    }
    object_property_add_uint64_ptr(obj, "x-test-frame-count", &s->frames,
                                   OBJ_PROP_FLAG_READ);
}

static void finalize(Object *obj)
{
    timer_free(SGI_O2_GBE(obj)->timer);
}

static void class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);
    dc->vmsd = &vmstate_gbe;
    dc->desc = "SGI GBE 1.1 architectural display";
    dc->user_creatable = true;
    dc->realize = realize;
    dc->unrealize = unrealize;
    device_class_set_props(dc, properties);
    rc->phases.enter = reset_enter;
}

static const TypeInfo info = {
    .name = TYPE_SGI_O2_GBE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIO2GBEState),
    .instance_init = init,
    .instance_finalize = finalize,
    .class_init = class_init,
};

static void register_types(void)
{
    type_register_static(&info);
}
type_init(register_types)
