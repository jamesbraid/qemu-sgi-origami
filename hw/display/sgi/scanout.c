/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI graphics scanout. See the header for the model.
 */
#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "ui/surface.h"
#include "hw/display/sgi/scanout.h"
#include "trace.h"

int64_t sgi_scanout_retrace_time(const SGIScanout *s, uint64_t n)
{
    uint64_t hz = s->mode.clock_hz, fc = s->mode.frame_clocks;

    if (!hz || !fc) {
        return -1;
    }
    /*
     * n * fc / hz seconds, exactly, without overflowing for any n a run
     * reaches: whole seconds' worth of frames first, then the rest, whose
     * product fits because both factors are below 2^32.
     */
    return s->epoch + (n / hz) * fc * NANOSECONDS_PER_SECOND +
           muldiv64((n % hz) * fc, NANOSECONDS_PER_SECOND, hz);
}

/*
 * A mode may be derived from guest-programmed timing tables, so it is
 * checked before anything is allocated or armed from it. A size past
 * SGI_SCANOUT_MAX_DIM presents nothing, and a period shorter than
 * SGI_SCANOUT_MIN_PERIOD_NS, which would re-arm the timer faster than the
 * main loop can run it, or a frame too long to count, has no retrace.
 * Returns whether the mode was used as given.
 */
static bool sgi_scanout_sanitize(SGIScanoutMode *mode)
{
    bool ok = true;

    if (mode->width > SGI_SCANOUT_MAX_DIM ||
        mode->height > SGI_SCANOUT_MAX_DIM) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-scanout: %ux%u exceeds the "
                      "largest mode, %ux%u\n", mode->width, mode->height,
                      SGI_SCANOUT_MAX_DIM, SGI_SCANOUT_MAX_DIM);
        mode->width = mode->height = 0;
        ok = false;
    }
    if (mode->clock_hz &&
        (mode->frame_clocks > UINT32_MAX ||
         muldiv64(mode->frame_clocks, NANOSECONDS_PER_SECOND,
                  mode->clock_hz) < SGI_SCANOUT_MIN_PERIOD_NS)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-scanout: a frame of %" PRIu64
                      " clocks at %u Hz is outside the retrace periods "
                      "modelled\n", mode->frame_clocks, mode->clock_hz);
        mode->clock_hz = 0;
        ok = false;
    }
    return ok;
}

static void sgi_scanout_arm(SGIScanout *s)
{
    int64_t when = sgi_scanout_retrace_time(s, s->frames + 1);

    if (when < 0) {
        timer_del(s->retrace_timer);
    } else {
        timer_mod(s->retrace_timer, when);
    }
}

static void sgi_scanout_restart(SGIScanout *s)
{
    s->epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->frames = 0;
    sgi_scanout_arm(s);
}

void sgi_scanout_set_next_retrace(SGIScanout *s, int64_t when)
{
    int64_t next = sgi_scanout_retrace_time(s, s->frames + 1);

    if (next < 0) {
        return;
    }
    /*
     * Move the epoch rather than restart the count, so retrace n stays
     * epoch plus n periods and the next one, frames + 1, lands on when.
     */
    s->epoch = when - (next - s->epoch);
    sgi_scanout_arm(s);
}

static void sgi_scanout_retrace(void *opaque)
{
    SGIScanout *s = opaque;
    bool swapped = false;

    s->frames++;
    trace_sgi_scanout_retrace(s->frames, s->swap_count);
    if (s->swap_count) {
        SGISwapRecord rec = s->swaps[s->swap_head];

        s->swap_head = (s->swap_head + 1) % SGI_SWAP_MAX;
        s->swap_count--;
        trace_sgi_scanout_swap(rec.seq, rec.cookie, s->frames);
        sgi_head_publish(s->ops, s->opaque, rec.seq, SGI_DONE_SWAPPED,
                         rec.cookie, 0);
        swapped = true;
    }
    if (s->ops->retrace) {
        s->ops->retrace(s->opaque);
    }
    /* The family flipped what the lines resolve to: repaint them all. */
    if (swapped) {
        sgi_scanout_invalidate_all(s);
    }
    sgi_scanout_arm(s);
}

void sgi_scanout_invalidate(SGIScanout *s, int first, int count)
{
    int h = s->frame ? s->mode.height : 0;
    int last = first + count - 1;

    first = MAX(first, 0);
    last = MIN(last, h - 1);
    if (first <= last) {
        bitmap_set(s->dirty, first, last - first + 1);
    }
}

void sgi_scanout_invalidate_all(SGIScanout *s)
{
    sgi_scanout_invalidate(s, 0, s->mode.height);
}

void sgi_scanout_invalidate_plane(SGIScanout *s, const SGIPlane *p,
                                  int y0, int y1)
{
    if (p->bottom_origin) {
        int first = (int)p->height - 1 - MAX(y0, y1);
        int last = (int)p->height - 1 - MIN(y0, y1);

        sgi_scanout_invalidate(s, first, last - first + 1);
    } else {
        sgi_scanout_invalidate(s, MIN(y0, y1), ABS(y1 - y0) + 1);
    }
}

/* The display lines the cursor image covers now; first > last if none. */
static void sgi_scanout_cursor_span(const SGIScanout *s, int *first,
                                    int *last)
{
    int top = s->cursor_y - s->cursor_hot_y;

    if (!s->cursor_on || !s->cursor_w || !s->cursor_h) {
        *first = 0;
        *last = -1;
        return;
    }
    *first = top;
    *last = top + (int)s->cursor_h - 1;
}

/* A composited cursor changed: repaint where it was and where it is. */
static void sgi_scanout_cursor_invalidate(SGIScanout *s)
{
    int first, last;

    sgi_scanout_invalidate(s, s->cursor_drawn_first,
                           s->cursor_drawn_last - s->cursor_drawn_first + 1);
    sgi_scanout_cursor_span(s, &first, &last);
    sgi_scanout_invalidate(s, first, last - first + 1);
}

static uint32_t sgi_scanout_blend(uint32_t src, uint32_t dst)
{
    unsigned a = src >> 24;
    uint32_t out = 0;

    if (a == 0xff) {
        return src & 0xffffff;
    }
    for (int sh = 0; sh < 24; sh += 8) {
        unsigned s8 = (src >> sh) & 0xff, d8 = (dst >> sh) & 0xff;

        out |= ((s8 * a + d8 * (255 - a) + 127) / 255) << sh;
    }
    return out;
}

static void sgi_scanout_draw_cursor(SGIScanout *s, int first, int last)
{
    int left = s->cursor_x - s->cursor_hot_x;
    int top = s->cursor_y - s->cursor_hot_y;
    int w = s->mode.width;

    if (!s->cursor_on) {
        return;
    }
    first = MAX(first, top);
    last = MIN(last, top + (int)s->cursor_h - 1);
    for (int y = first; y <= last; y++) {
        const uint32_t *src = &s->cursor_image[(y - top) * s->cursor_w];
        uint32_t *dst = &s->frame[(size_t)y * w];

        for (int i = 0; i < (int)s->cursor_w; i++) {
            int x = left + i;

            if (x < 0 || x >= w || !(src[i] >> 24)) {
                continue;
            }
            dst[x] = sgi_scanout_blend(src[i], dst[x]);
        }
    }
}

void sgi_scanout_update(SGIScanout *s)
{
    unsigned long h, y, end;

    if (s->ops->pre_update) {
        s->ops->pre_update(s->opaque);
    }
    if (!s->frame) {
        return;
    }
    h = s->mode.height;
    for (y = find_first_bit(s->dirty, h); y < h;
         y = find_next_bit(s->dirty, h, end)) {
        end = find_next_zero_bit(s->dirty, h, y);
        for (unsigned long line = y; line < end; line++) {
            s->ops->resolve_row(s->opaque, line,
                                &s->frame[line * s->mode.width]);
        }
        bitmap_clear(s->dirty, y, end - y);
        if (s->composite_cursor) {
            sgi_scanout_draw_cursor(s, y, end - 1);
        }
        trace_sgi_scanout_update(y, end - 1);
        qemu_console_update(s->con, 0, y, s->mode.width, end - y);
    }
    if (s->composite_cursor) {
        int first, last;

        /*
         * The whole span the cursor covers now, not just the lines this
         * pass repainted: they are what the next change must repaint.
         */
        sgi_scanout_cursor_span(s, &first, &last);
        s->cursor_drawn_first = first;
        s->cursor_drawn_last = last;
    }
}

static void sgi_scanout_host_cursor(SGIScanout *s)
{
    /* An empty image is one transparent pixel, replacing the old image. */
    QEMUCursor *c = cursor_alloc(MAX(s->cursor_w, 1), MAX(s->cursor_h, 1));

    c->hot_x = s->cursor_hot_x;
    c->hot_y = s->cursor_hot_y;
    memcpy(c->data, s->cursor_image,
           sizeof(uint32_t) * s->cursor_w * s->cursor_h);
    qemu_console_set_cursor(s->con, c);
    cursor_unref(c);
}

void sgi_scanout_cursor_define(SGIScanout *s, uint32_t w, uint32_t h,
                               int hot_x, int hot_y, const uint32_t *argb)
{
    uint32_t cw = MIN(w, SGI_CURSOR_MAX), ch = MIN(h, SGI_CURSOR_MAX);

    if (cw != w || ch != h) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-scanout: a %ux%u cursor is "
                      "clipped to %ux%u\n", w, h, cw, ch);
    }
    s->cursor_w = cw;
    s->cursor_h = ch;
    s->cursor_hot_x = hot_x;
    s->cursor_hot_y = hot_y;
    memset(s->cursor_image, 0, sizeof(s->cursor_image));
    for (uint32_t y = 0; y < ch; y++) {
        memcpy(&s->cursor_image[y * cw], &argb[(size_t)y * w],
               sizeof(uint32_t) * cw);
    }
    if (s->composite_cursor) {
        sgi_scanout_cursor_invalidate(s);
    } else {
        sgi_scanout_host_cursor(s);
    }
}

void sgi_scanout_cursor_move(SGIScanout *s, int x, int y, bool on)
{
    s->cursor_x = x;
    s->cursor_y = y;
    s->cursor_on = on;
    if (s->composite_cursor) {
        sgi_scanout_cursor_invalidate(s);
    } else {
        qemu_console_set_mouse(s->con, x, y, on);
    }
}

void sgi_scanout_queue_swap(SGIScanout *s, uint64_t seq, uint64_t cookie)
{
    if (s->swap_count == SGI_SWAP_MAX) {
        sgi_head_publish(s->ops, s->opaque, seq, SGI_DONE_FAILED, cookie,
                         SGI_FAIL_FULL);
        return;
    }
    s->swaps[(s->swap_head + s->swap_count) % SGI_SWAP_MAX] =
        (SGISwapRecord) { .seq = seq, .cookie = cookie };
    s->swap_count++;
}

/* A frame, dirty map and surface for the mode, when its size changed. */
static void sgi_scanout_alloc(SGIScanout *s)
{
    uint32_t w = s->mode.width, h = s->mode.height;
    uint32_t *old = s->frame;

    if (old && w == s->frame_width && h == s->frame_height) {
        return;
    }
    g_free(s->dirty);
    s->frame = NULL;
    s->dirty = NULL;
    s->frame_width = s->frame_height = 0;
    /* The size is sanitized, so w * 4 fits an int. */
    QEMU_BUILD_BUG_ON(SGI_SCANOUT_MAX_DIM > INT_MAX / 4);
    if (w && h) {
        s->frame = g_new0(uint32_t, (size_t)w * h);
        s->dirty = bitmap_new(h);
        s->frame_width = w;
        s->frame_height = h;
        qemu_console_set_surface(s->con,
            qemu_create_displaysurface_from(w, h, PIXMAN_x8r8g8b8, w * 4,
                                            (uint8_t *)s->frame));
    } else if (old) {
        /* Nothing to show, and the old surface must not outlive its frame. */
        qemu_console_set_surface(s->con,
            qemu_create_placeholder_surface(640, 480, "No display mode"));
    }
    /* Only now: the console's previous surface pointed into it. */
    g_free(old);
}

void sgi_scanout_set_mode(SGIScanout *s, const SGIScanoutMode *mode)
{
    s->mode = *mode;
    sgi_scanout_sanitize(&s->mode);
    trace_sgi_scanout_mode(s->mode.width, s->mode.height,
                           s->mode.frame_clocks, s->mode.clock_hz);
    sgi_scanout_alloc(s);
    s->cursor_drawn_first = 0;
    s->cursor_drawn_last = -1;
    sgi_scanout_invalidate_all(s);
    sgi_scanout_restart(s);
}

void sgi_scanout_reset(SGIScanout *s)
{
    s->swap_head = 0;
    s->swap_count = 0;
    s->cursor_on = false;
    if (!s->composite_cursor) {
        qemu_console_set_mouse(s->con, s->cursor_x, s->cursor_y, false);
    }
    sgi_scanout_invalidate_all(s);
    sgi_scanout_restart(s);
}

static void sgi_scanout_gfx_invalidate(void *opaque)
{
    sgi_scanout_invalidate_all(opaque);
}

static bool sgi_scanout_gfx_update(void *opaque)
{
    sgi_scanout_update(opaque);
    return true;
}

static const GraphicHwOps sgi_scanout_gfx_ops = {
    .invalidate = sgi_scanout_gfx_invalidate,
    .gfx_update = sgi_scanout_gfx_update,
};

void sgi_scanout_init(SGIScanout *s, DeviceState *dev,
                      const SGIHeadOps *ops, void *opaque)
{
    assert(ops->resolve_row);
    s->ops = ops;
    s->opaque = opaque;
    s->cursor_drawn_first = 0;
    s->cursor_drawn_last = -1;
    s->retrace_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sgi_scanout_retrace,
                                    s);
    s->con = qemu_graphic_console_create(dev, 0, &sgi_scanout_gfx_ops, s);
}

void sgi_scanout_cleanup(SGIScanout *s)
{
    timer_free(s->retrace_timer);
    s->retrace_timer = NULL;
    qemu_graphic_console_close(s->con);
    s->con = NULL;
    g_free(s->frame);
    g_free(s->dirty);
    s->frame = NULL;
    s->dirty = NULL;
}

static int sgi_scanout_post_load(void *opaque, int version_id)
{
    SGIScanout *s = opaque;

    if (s->swap_head >= SGI_SWAP_MAX || s->swap_count > SGI_SWAP_MAX ||
        s->cursor_w > SGI_CURSOR_MAX || s->cursor_h > SGI_CURSOR_MAX ||
        !sgi_scanout_sanitize(&s->mode)) {
        return -EINVAL;
    }
    sgi_scanout_alloc(s);
    s->cursor_drawn_first = 0;
    s->cursor_drawn_last = -1;
    sgi_scanout_invalidate_all(s);
    if (!s->composite_cursor) {
        sgi_scanout_host_cursor(s);
        qemu_console_set_mouse(s->con, s->cursor_x, s->cursor_y,
                               s->cursor_on);
    }
    sgi_scanout_arm(s);
    return 0;
}

static const VMStateDescription vmstate_sgi_swap_record = {
    .name = "sgi-swap-record",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(seq, SGISwapRecord),
        VMSTATE_UINT64(cookie, SGISwapRecord),
        VMSTATE_END_OF_LIST()
    },
};

const VMStateDescription vmstate_sgi_scanout = {
    .name = "sgi-scanout",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sgi_scanout_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mode.width, SGIScanout),
        VMSTATE_UINT32(mode.height, SGIScanout),
        VMSTATE_UINT64(mode.frame_clocks, SGIScanout),
        VMSTATE_UINT32(mode.clock_hz, SGIScanout),
        VMSTATE_INT64(epoch, SGIScanout),
        VMSTATE_UINT64(frames, SGIScanout),
        VMSTATE_STRUCT_ARRAY(swaps, SGIScanout, SGI_SWAP_MAX, 1,
                             vmstate_sgi_swap_record, SGISwapRecord),
        VMSTATE_UINT32(swap_head, SGIScanout),
        VMSTATE_UINT32(swap_count, SGIScanout),
        VMSTATE_UINT32_ARRAY(cursor_image, SGIScanout,
                             SGI_CURSOR_MAX * SGI_CURSOR_MAX),
        VMSTATE_UINT32(cursor_w, SGIScanout),
        VMSTATE_UINT32(cursor_h, SGIScanout),
        VMSTATE_INT32(cursor_hot_x, SGIScanout),
        VMSTATE_INT32(cursor_hot_y, SGIScanout),
        VMSTATE_INT32(cursor_x, SGIScanout),
        VMSTATE_INT32(cursor_y, SGIScanout),
        VMSTATE_BOOL(cursor_on, SGIScanout),
        VMSTATE_END_OF_LIST()
    },
};
