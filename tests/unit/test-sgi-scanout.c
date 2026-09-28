/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI graphics scanout: retrace period and phase, swap records, dirty
 * lines, both cursor paths, and migration.
 *
 * The console is a recorder: the few console entry points the scanout
 * calls are defined here and log what they were given. The virtual clock
 * is a variable the tests advance, and timers are QEMU's own.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "io/channel-buffer.h"
#include "migration/vmstate.h"
#include "../migration/qemu-file.h"
#include "system/cpu-timers.h"
#include "hw/display/sgi/scanout.h"

static int64_t virtual_ns;

int64_t cpus_get_virtual_clock(void)
{
    return virtual_ns;
}

void cpus_set_virtual_clock(int64_t new_time)
{
    virtual_ns = new_time;
}

typedef struct Update {
    int y, h;
} Update;

static struct {
    GArray *updates;            /* of Update */
    DisplaySurface *surface;
    uint8_t *surface_data;
    bool placeholder;
    int mouse_x, mouse_y, mouse_calls;
    bool mouse_on;
    int cursor_calls, cursor_w, cursor_h, cursor_hot_x, cursor_hot_y;
    uint32_t cursor_pixel0;
} con;

static int fake_console;

QemuConsole *qemu_graphic_console_create(DeviceState *dev, uint32_t head,
                                         const GraphicHwOps *ops,
                                         void *opaque)
{
    return (QemuConsole *)&fake_console;
}

void qemu_graphic_console_close(QemuConsole *c)
{
    g_free(con.surface);
    con.surface = NULL;
}

DisplaySurface *qemu_create_displaysurface_from(int width, int height,
                                                pixman_format_code_t format,
                                                int linesize, uint8_t *data)
{
    g_assert_cmpint(linesize, ==, width * 4);
    con.surface_data = data;
    con.placeholder = false;
    return g_new0(DisplaySurface, 1);
}

DisplaySurface *qemu_create_placeholder_surface(int w, int h,
                                                const char *msg)
{
    con.surface_data = NULL;
    con.placeholder = true;
    return g_new0(DisplaySurface, 1);
}

void qemu_console_set_surface(QemuConsole *c, DisplaySurface *surface)
{
    g_free(con.surface);
    con.surface = surface;
}

void qemu_console_update(QemuConsole *c, int x, int y, int w, int h)
{
    Update u = { .y = y, .h = h };

    g_array_append_val(con.updates, u);
}

void qemu_console_set_mouse(QemuConsole *c, int x, int y, bool on)
{
    con.mouse_x = x;
    con.mouse_y = y;
    con.mouse_on = on;
    con.mouse_calls++;
}

void qemu_console_set_cursor(QemuConsole *c, QEMUCursor *cursor)
{
    con.cursor_calls++;
    con.cursor_w = cursor->width;
    con.cursor_h = cursor->height;
    con.cursor_hot_x = cursor->hot_x;
    con.cursor_hot_y = cursor->hot_y;
    con.cursor_pixel0 = cursor->data[0];
}

QEMUCursor *cursor_alloc(uint16_t width, uint16_t height)
{
    QEMUCursor *c = g_malloc0(sizeof(QEMUCursor) +
                              sizeof(uint32_t) * width * height);

    c->width = width;
    c->height = height;
    c->refcount = 1;
    return c;
}

void cursor_unref(QEMUCursor *c)
{
    if (c && --c->refcount == 0) {
        g_free(c);
    }
}

typedef struct Head {
    SGIScanout scanout;
    unsigned resolved[1024];    /* resolve_row() calls per line */
    GString *events;            /* swaps and retraces, in order */
} Head;

/* Line y resolves to a colour naming the line and the column. */
static uint32_t line_colour(unsigned y, unsigned x)
{
    return 0x100000 * (y + 1) + x;
}

static void head_resolve_row(void *opaque, unsigned y, uint32_t *dst)
{
    Head *h = opaque;

    h->resolved[y]++;
    for (unsigned x = 0; x < h->scanout.mode.width; x++) {
        dst[x] = line_colour(y, x);
    }
}

static void head_publish(void *opaque, const SGICompletion *c)
{
    Head *h = opaque;

    g_string_append_printf(h->events, "%s:%" PRIx64 " ",
                           c->kind == SGI_DONE_SWAPPED ? "swap" : "fail",
                           c->cookie);
}

static void head_retrace(void *opaque)
{
    Head *h = opaque;

    g_string_append_printf(h->events, "retrace:%" PRIu64 " ",
                           h->scanout.frames);
}

static const SGIHeadOps head_ops = {
    .publish = head_publish,
    .resolve_row = head_resolve_row,
    .retrace = head_retrace,
};

/* 1280x1024 at 60 on MGRAS: 1680x1065 clocks at the PLL's 107.25 MHz. */
static const SGIScanoutMode mgras_1280x1024_60 = {
    .width = 1280, .height = 1024,
    .frame_clocks = 1680 * 1065,
    .clock_hz = 107250000,
};

static Head *head_new(bool composite)
{
    Head *h = g_new0(Head, 1);

    con.updates = g_array_new(false, true, sizeof(Update));
    h->events = g_string_new("");
    h->scanout.composite_cursor = composite;
    sgi_scanout_init(&h->scanout, NULL, &head_ops, h);
    return h;
}

static void head_free(Head *h)
{
    sgi_scanout_cleanup(&h->scanout);
    g_string_free(h->events, true);
    g_array_free(con.updates, true);
    g_free(h);
    memset(&con, 0, sizeof(con));
}

static void run_to(int64_t ns)
{
    virtual_ns = ns;
    qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
}

/*
 * The period is the mode's clocks over its clock, 59.943 Hz for this one,
 * and retrace n is computed from the epoch, so a thousand frames later the
 * phase is still exact to the nanosecond.
 */
static void test_retrace_period(void)
{
    Head *h = head_new(false);
    SGIScanout *s = &h->scanout;

    virtual_ns = 5000;
    sgi_scanout_set_mode(s, &mgras_1280x1024_60);
    g_assert_cmpint(s->epoch, ==, 5000);
    g_assert_cmpint(sgi_scanout_retrace_time(s, 1), ==, 5000 + 16682517);
    g_assert_cmpint(sgi_scanout_retrace_time(s, 1000), ==,
                    5000 + 16682517482ll);
    g_assert_cmpint(sgi_scanout_retrace_time(s, 60000), ==,
                    5000 + 1000951048951ll);
    g_assert_cmpfloat_with_epsilon(
        1000 * 1e9 / (sgi_scanout_retrace_time(s, 1000) - 5000), 59.943,
        0.0005);

    run_to(5000 + 16682516);
    g_assert_cmpuint(s->frames, ==, 0);
    run_to(5000 + 16682517);
    g_assert_cmpuint(s->frames, ==, 1);
    g_assert_cmpstr(h->events->str, ==, "retrace:1 ");
    for (uint64_t n = 2; n <= 1000; n++) {
        run_to(sgi_scanout_retrace_time(s, n));
    }
    g_assert_cmpuint(s->frames, ==, 1000);
    g_assert_cmpint(timer_expire_time_ns(s->retrace_timer), ==,
                    sgi_scanout_retrace_time(s, 1001));

    /* Exact far out, where n * frame_clocks alone approaches 2^64. */
    g_assert_cmpint(sgi_scanout_retrace_time(s, 10000000000ull), ==,
                    5000 + 166825174825174825ll);

    /* No clock, no retrace. */
    sgi_scanout_set_mode(s, &(SGIScanoutMode) { .width = 8, .height = 8 });
    g_assert_cmpint(sgi_scanout_retrace_time(s, 1), ==, -1);
    g_assert_false(timer_pending(s->retrace_timer));

    /*
     * A period a guest's timing tables could program but no display has:
     * one clock at 1 GHz would re-arm the timer every nanosecond. It gets
     * no retrace, and neither does a frame too long to count.
     */
    sgi_scanout_set_mode(s, &(SGIScanoutMode) {
        .width = 8, .height = 8, .frame_clocks = 1, .clock_hz = 1000000000,
    });
    g_assert_cmpint(sgi_scanout_retrace_time(s, 1), ==, -1);
    g_assert_false(timer_pending(s->retrace_timer));
    sgi_scanout_set_mode(s, &(SGIScanoutMode) {
        .width = 8, .height = 8, .frame_clocks = 1ull << 40, .clock_hz = 1,
    });
    g_assert_false(timer_pending(s->retrace_timer));
    /* The floor itself is allowed: 1 kHz. */
    sgi_scanout_set_mode(s, &(SGIScanoutMode) {
        .width = 8, .height = 8, .frame_clocks = 1000, .clock_hz = 1000000,
    });
    g_assert_true(timer_pending(s->retrace_timer));
    head_free(h);
}

/*
 * One swap takes effect per retrace, in order, published before the
 * family's retrace hook; a seventeenth waiting swap fails at once.
 */
static void test_swaps(void)
{
    Head *h = head_new(false);
    SGIScanout *s = &h->scanout;

    virtual_ns = 0;
    sgi_scanout_set_mode(s, &mgras_1280x1024_60);
    sgi_scanout_queue_swap(s, 1, 0xa);
    sgi_scanout_queue_swap(s, 2, 0xb);
    sgi_scanout_update(s);
    memset(h->resolved, 0, sizeof(h->resolved));
    run_to(sgi_scanout_retrace_time(s, 1));
    /* The swap changed what every line resolves to: all are repainted. */
    sgi_scanout_update(s);
    g_assert_cmpuint(h->resolved[0], ==, 1);
    g_assert_cmpuint(h->resolved[1023], ==, 1);
    g_assert_cmpint(g_array_index(con.updates, Update,
                                  con.updates->len - 1).h, ==, 1024);
    run_to(sgi_scanout_retrace_time(s, 2));
    run_to(sgi_scanout_retrace_time(s, 3));
    /* A retrace with no swap repaints nothing. */
    g_array_set_size(con.updates, 0);
    sgi_scanout_update(s);
    sgi_scanout_update(s);
    g_assert_cmpuint(con.updates->len, ==, 1);
    g_assert_cmpstr(h->events->str, ==,
                    "swap:a retrace:1 swap:b retrace:2 retrace:3 ");

    g_string_truncate(h->events, 0);
    for (unsigned i = 0; i < SGI_SWAP_MAX; i++) {
        sgi_scanout_queue_swap(s, 10 + i, 0x100 + i);
    }
    g_assert_cmpstr(h->events->str, ==, "");
    sgi_scanout_queue_swap(s, 99, 0xdead);
    g_assert_cmpstr(h->events->str, ==, "fail:dead ");
    run_to(sgi_scanout_retrace_time(s, 4));
    g_assert_cmpstr(h->events->str, ==, "fail:dead swap:100 retrace:4 ");

    /* Reset drops what is waiting and restarts the phase. */
    g_string_truncate(h->events, 0);
    virtual_ns += 1234;
    sgi_scanout_reset(s);
    g_assert_cmpuint(s->swap_count, ==, 0);
    g_assert_cmpint(s->epoch, ==, virtual_ns);
    run_to(sgi_scanout_retrace_time(s, 1));
    g_assert_cmpstr(h->events->str, ==, "retrace:1 ");
    head_free(h);
}

/*
 * A destination restarted its phase at its own reset, long before the
 * source's next retrace. Setting the next retrace puts that retrace at the
 * time given and the rest a period apart, counting on from where the count
 * was, with none of the retraces the stale phase says were missed.
 */
static void test_set_next_retrace(void)
{
    Head *h = head_new(false);
    SGIScanout *s = &h->scanout;
    int64_t period, when;

    virtual_ns = 1000;
    sgi_scanout_set_mode(s, &mgras_1280x1024_60);
    run_to(sgi_scanout_retrace_time(s, 1));
    run_to(sgi_scanout_retrace_time(s, 2));
    g_assert_cmpstr(h->events->str, ==, "retrace:1 retrace:2 ");
    g_string_truncate(h->events, 0);
    period = sgi_scanout_retrace_time(s, 3) - sgi_scanout_retrace_time(s, 2);

    /* A hundred periods on, without the timer having run, off phase. */
    virtual_ns = sgi_scanout_retrace_time(s, 102) + 12345;
    when = virtual_ns + 777;
    sgi_scanout_set_next_retrace(s, when);
    g_assert_cmpint(timer_expire_time_ns(s->retrace_timer), ==, when);
    g_assert_cmpint(sgi_scanout_retrace_time(s, 3), ==, when);

    run_to(when - 1);
    g_assert_cmpstr(h->events->str, ==, "");
    run_to(when);
    g_assert_cmpstr(h->events->str, ==, "retrace:3 ");
    g_assert_cmpint(timer_expire_time_ns(s->retrace_timer), ==,
                    sgi_scanout_retrace_time(s, 4));
    run_to(sgi_scanout_retrace_time(s, 4) - 1);
    g_assert_cmpstr(h->events->str, ==, "retrace:3 ");
    run_to(sgi_scanout_retrace_time(s, 4));
    run_to(sgi_scanout_retrace_time(s, 5));
    g_assert_cmpstr(h->events->str, ==, "retrace:3 retrace:4 retrace:5 ");
    g_assert_cmpuint(s->frames, ==, 5);
    /* The period is the mode's, to within the rounding of one frame. */
    g_assert_cmpint(ABS(sgi_scanout_retrace_time(s, 5) - when - 2 * period),
                    <=, 1);

    /* No retrace in the mode: nothing to put anywhere. */
    sgi_scanout_set_mode(s, &(SGIScanoutMode) { .width = 8, .height = 8 });
    sgi_scanout_set_next_retrace(s, virtual_ns + 1000);
    g_assert_false(timer_pending(s->retrace_timer));
    head_free(h);
}

static const SGIScanoutMode small = { .width = 8, .height = 6 };

static void assert_resolved(Head *h, const unsigned *want)
{
    for (unsigned y = 0; y < small.height; y++) {
        g_assert_cmpuint(h->resolved[y], ==, want[y]);
    }
    memset(h->resolved, 0, sizeof(h->resolved));
}

/* Only dirty lines are resolved and presented, in runs. */
static void test_dirty_lines(void)
{
    Head *h = head_new(false);
    SGIScanout *s = &h->scanout;
    SGIPlane bottom = { .width = 8, .height = 6, .bottom_origin = true };
    SGIPlane top = { .width = 8, .height = 6 };
    Update *u;

    sgi_scanout_set_mode(s, &small);
    g_assert_true(con.surface_data == (uint8_t *)s->frame);
    sgi_scanout_update(s);
    assert_resolved(h, (unsigned[]) { 1, 1, 1, 1, 1, 1 });
    g_assert_cmpuint(con.updates->len, ==, 1);
    u = &g_array_index(con.updates, Update, 0);
    g_assert_cmpint(u->y, ==, 0);
    g_assert_cmpint(u->h, ==, 6);
    g_assert_cmphex(s->frame[5 * 8 + 7], ==, line_colour(5, 7));

    sgi_scanout_update(s);
    assert_resolved(h, (unsigned[]) { 0, 0, 0, 0, 0, 0 });
    g_assert_cmpuint(con.updates->len, ==, 1);

    sgi_scanout_invalidate(s, 2, 2);
    sgi_scanout_invalidate(s, 5, 10);   /* clipped to the last line */
    sgi_scanout_update(s);
    assert_resolved(h, (unsigned[]) { 0, 0, 1, 1, 0, 1 });
    g_assert_cmpuint(con.updates->len, ==, 3);
    u = &g_array_index(con.updates, Update, 1);
    g_assert_cmpint(u->y, ==, 2);
    g_assert_cmpint(u->h, ==, 2);

    /* A bottom-origin plane's rows 0 and 1 are the bottom two lines. */
    sgi_scanout_invalidate_plane(s, &bottom, 0, 1);
    sgi_scanout_update(s);
    assert_resolved(h, (unsigned[]) { 0, 0, 0, 0, 1, 1 });
    sgi_scanout_invalidate_plane(s, &top, 1, 0);
    sgi_scanout_update(s);
    assert_resolved(h, (unsigned[]) { 1, 1, 0, 0, 0, 0 });

    /* A new mode of the same size keeps the frame and repaints it. */
    sgi_scanout_set_mode(s, &small);
    sgi_scanout_update(s);
    assert_resolved(h, (unsigned[]) { 1, 1, 1, 1, 1, 1 });

    /*
     * A mode with no pixels frees the frame, so the console must stop
     * showing it first: it gets a placeholder, and nothing is resolved.
     */
    sgi_scanout_set_mode(s, &(SGIScanoutMode) { 0 });
    g_assert_true(con.placeholder);
    g_assert_null(s->frame);
    sgi_scanout_invalidate_all(s);
    sgi_scanout_update(s);
    assert_resolved(h, (unsigned[]) { 0, 0, 0, 0, 0, 0 });

    /* A size past the largest mode is refused the same way. */
    sgi_scanout_set_mode(s, &small);
    g_assert_false(con.placeholder);
    sgi_scanout_set_mode(s, &(SGIScanoutMode) {
        .width = SGI_SCANOUT_MAX_DIM + 1, .height = 4,
    });
    g_assert_true(con.placeholder);
    g_assert_null(s->frame);
    g_assert_cmpuint(s->mode.width, ==, 0);
    head_free(h);
}

typedef struct FlagHead {
    Head head;                  /* first: the ops take either */
    bool drawn;                 /* the family's whole-frame dirty flag */
    unsigned calls;
    unsigned resolved_before;   /* lines resolved this update, at the call */
} FlagHead;

static unsigned total_resolved(Head *h)
{
    unsigned n = 0;

    for (unsigned y = 0; y < ARRAY_SIZE(h->resolved); y++) {
        n += h->resolved[y];
    }
    return n;
}

static void flag_pre_update(void *opaque)
{
    FlagHead *f = opaque;

    f->calls++;
    f->resolved_before = total_resolved(&f->head);
    if (f->drawn) {
        f->drawn = false;
        sgi_scanout_invalidate(&f->head.scanout, 3, 1);
    }
}

static const SGIHeadOps flag_head_ops = {
    .publish = head_publish,
    .resolve_row = head_resolve_row,
    .retrace = head_retrace,
    .pre_update = flag_pre_update,
};

/*
 * The pre-update hook runs once per update, before any line is resolved,
 * and a line it invalidates is resolved and presented by that update.
 */
static void test_pre_update(void)
{
    FlagHead *f = g_new0(FlagHead, 1);
    Head *h = &f->head;
    SGIScanout *s = &h->scanout;

    con.updates = g_array_new(false, true, sizeof(Update));
    h->events = g_string_new("");
    sgi_scanout_init(s, NULL, &flag_head_ops, h);

    /* No frame yet: the hook still runs, and nothing is resolved. */
    sgi_scanout_update(s);
    g_assert_cmpuint(f->calls, ==, 1);

    sgi_scanout_set_mode(s, &small);
    sgi_scanout_update(s);
    g_assert_cmpuint(f->calls, ==, 2);
    g_assert_cmpuint(f->resolved_before, ==, 0);
    assert_resolved(h, (unsigned[]) { 1, 1, 1, 1, 1, 1 });

    /* Nothing drawn: nothing resolved, but the hook ran. */
    sgi_scanout_update(s);
    g_assert_cmpuint(f->calls, ==, 3);
    assert_resolved(h, (unsigned[]) { 0, 0, 0, 0, 0, 0 });

    g_array_set_size(con.updates, 0);
    f->drawn = true;
    sgi_scanout_update(s);
    g_assert_cmpuint(f->calls, ==, 4);
    g_assert_cmpuint(f->resolved_before, ==, 0);
    g_assert_false(f->drawn);
    assert_resolved(h, (unsigned[]) { 0, 0, 0, 1, 0, 0 });
    g_assert_cmpuint(con.updates->len, ==, 1);
    g_assert_cmpint(g_array_index(con.updates, Update, 0).y, ==, 3);

    /* Lines already dirty are resolved after the hook, not before. */
    sgi_scanout_invalidate(s, 0, 2);
    sgi_scanout_update(s);
    g_assert_cmpuint(f->resolved_before, ==, 0);
    assert_resolved(h, (unsigned[]) { 1, 1, 0, 0, 0, 0 });

    sgi_scanout_cleanup(s);
    g_string_free(h->events, true);
    g_array_free(con.updates, true);
    g_free(f);
    memset(&con, 0, sizeof(con));
}

/*
 * A composited cursor is drawn over the lines it covers, with its hot spot
 * at the position given; moving it repaints where it was and where it is,
 * and nothing else.
 */
static void test_cursor_composite(void)
{
    Head *h = head_new(true);
    SGIScanout *s = &h->scanout;
    /* 2x2: opaque red, transparent, half-alpha green, opaque white. */
    static const uint32_t image[4] = {
        0xffff0000, 0x00123456, 0x8000ff00, 0xffffffff,
    };
    uint32_t under = line_colour(2, 3);
    uint32_t half = ((0x00 * 0x80 + ((under >> 16) & 0xff) * 0x7f + 127) /
                     255) << 16 |
                    ((0xff * 0x80 + ((under >> 8) & 0xff) * 0x7f + 127) /
                     255) << 8 |
                    ((0x00 * 0x80 + (under & 0xff) * 0x7f + 127) / 255);

    sgi_scanout_set_mode(s, &small);
    sgi_scanout_update(s);
    memset(h->resolved, 0, sizeof(h->resolved));

    sgi_scanout_cursor_define(s, 2, 2, 1, 1, image);
    sgi_scanout_cursor_move(s, 4, 2, true);
    sgi_scanout_update(s);
    assert_resolved(h, (unsigned[]) { 0, 1, 1, 0, 0, 0 });
    g_assert_cmphex(s->frame[1 * 8 + 3], ==, 0xff0000);
    g_assert_cmphex(s->frame[1 * 8 + 4], ==, line_colour(1, 4));
    g_assert_cmphex(s->frame[2 * 8 + 3], ==, half);
    g_assert_cmphex(s->frame[2 * 8 + 4], ==, 0xffffff);
    g_assert_cmphex(s->frame[2 * 8 + 5], ==, line_colour(2, 5));
    g_assert_cmpint(con.mouse_calls, ==, 0);
    g_assert_cmpint(con.cursor_calls, ==, 0);

    /* Down two lines: the old lines come back clean. */
    sgi_scanout_cursor_move(s, 4, 4, true);
    sgi_scanout_update(s);
    assert_resolved(h, (unsigned[]) { 0, 1, 1, 1, 1, 0 });
    g_assert_cmphex(s->frame[1 * 8 + 3], ==, line_colour(1, 3));
    g_assert_cmphex(s->frame[3 * 8 + 3], ==, 0xff0000);

    /* A repaint elsewhere keeps the cursor where it is drawn. */
    sgi_scanout_invalidate(s, 0, 1);
    sgi_scanout_update(s);
    assert_resolved(h, (unsigned[]) { 1, 0, 0, 0, 0, 0 });
    sgi_scanout_cursor_move(s, 4, 4, false);
    sgi_scanout_update(s);
    assert_resolved(h, (unsigned[]) { 0, 0, 0, 1, 1, 0 });
    g_assert_cmphex(s->frame[3 * 8 + 3], ==, line_colour(3, 3));

    /* Partly off the left and bottom edges. */
    sgi_scanout_cursor_move(s, 0, 6, true);
    sgi_scanout_update(s);
    g_assert_cmphex(s->frame[5 * 8 + 0], ==, line_colour(5, 0));
    sgi_scanout_cursor_move(s, 1, 6, true);
    sgi_scanout_update(s);
    g_assert_cmphex(s->frame[5 * 8 + 0], ==, 0xff0000);
    head_free(h);
}

/* The host cursor: defined and moved through the console, no repaint. */
static void test_cursor_host(void)
{
    Head *h = head_new(false);
    SGIScanout *s = &h->scanout;
    static const uint32_t image[4] = { 0xff00ff00, 0, 0, 0 };

    sgi_scanout_set_mode(s, &small);
    sgi_scanout_update(s);
    memset(h->resolved, 0, sizeof(h->resolved));
    sgi_scanout_cursor_define(s, 2, 2, 1, 0, image);
    g_assert_cmpint(con.cursor_calls, ==, 1);
    g_assert_cmpint(con.cursor_w, ==, 2);
    g_assert_cmpint(con.cursor_hot_x, ==, 1);
    g_assert_cmpint(con.cursor_hot_y, ==, 0);
    g_assert_cmphex(con.cursor_pixel0, ==, 0xff00ff00);
    sgi_scanout_cursor_move(s, 5, 3, true);
    g_assert_cmpint(con.mouse_x, ==, 5);
    g_assert_cmpint(con.mouse_y, ==, 3);
    g_assert_true(con.mouse_on);
    sgi_scanout_update(s);
    assert_resolved(h, (unsigned[]) { 0, 0, 0, 0, 0, 0 });
    g_assert_cmphex(s->frame[3 * 8 + 4], ==, line_colour(3, 4));

    /* An empty image replaces the old one with a transparent pixel. */
    sgi_scanout_cursor_define(s, 0, 0, 0, 0, image);
    g_assert_cmpint(con.cursor_calls, ==, 2);
    g_assert_cmpint(con.cursor_w, ==, 1);
    g_assert_cmphex(con.cursor_pixel0, ==, 0);
    head_free(h);
}

/*
 * Cursor sizes can come from the guest: one past SGI_CURSOR_MAX keeps its
 * top-left SGI_CURSOR_MAX square instead of stopping QEMU.
 */
static void test_cursor_clamp(void)
{
    Head *h = head_new(true);
    SGIScanout *s = &h->scanout;
    g_autofree uint32_t *image = g_new0(uint32_t, 100 * 100);

    image[0] = 0xff000001;
    image[1 * 100 + 63] = 0xff000002;
    image[1 * 100 + 64] = 0xff000003;
    sgi_scanout_set_mode(s, &small);
    sgi_scanout_cursor_define(s, 100, 100, 0, 0, image);
    g_assert_cmpuint(s->cursor_w, ==, SGI_CURSOR_MAX);
    g_assert_cmpuint(s->cursor_h, ==, SGI_CURSOR_MAX);
    g_assert_cmphex(s->cursor_image[0], ==, 0xff000001);
    g_assert_cmphex(s->cursor_image[1 * 64 + 63], ==, 0xff000002);
    g_assert_cmphex(s->cursor_image[2 * 64], ==, 0);
    head_free(h);
}

typedef struct TestHead {
    uint32_t reg;
    SGIScanout scanout;
} TestHead;

DEFINE_VMSTATE_SGI_SCANOUT(vmstate_test_head_scanout, "test-head/scanout",
                           TestHead, scanout);

/* The family's own description, with the scanout as a subsection. */
static const VMStateDescription vmstate_test_head = {
    .name = "test-head",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(reg, TestHead),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_test_head_scanout,
        NULL
    },
};

/*
 * The retrace phase, the waiting swaps and the cursor travel; the loaded
 * scanout resizes to the saved mode, repaints everything and re-arms at
 * the next retrace of the saved phase.
 */
static void test_migration(void)
{
    Head *h = head_new(false);
    TestHead src = { .reg = 1 }, dst = { 0 };
    SGIScanout *s = &src.scanout;
    QIOChannelBuffer *bioc = qio_channel_buffer_new(65536);
    QEMUFile *f = qemu_file_new_output(QIO_CHANNEL(bioc));
    g_autoptr(GByteArray) stream = g_byte_array_new();
    static const uint32_t image[1] = { 0xff123456 };
    int64_t next;

    sgi_scanout_init(s, NULL, &head_ops, h);
    virtual_ns = 100;
    sgi_scanout_set_mode(s, &mgras_1280x1024_60);
    run_to(sgi_scanout_retrace_time(s, 3));
    sgi_scanout_queue_swap(s, 7, 0x77);
    sgi_scanout_cursor_define(s, 1, 1, 0, 0, image);
    sgi_scanout_cursor_move(s, 10, 20, true);
    next = sgi_scanout_retrace_time(s, 4);

    g_assert_cmpint(vmstate_save_state(f, &vmstate_test_head, &src, NULL,
                                       &error_abort), ==, 0);
    g_assert_cmpint(qemu_fflush(f), ==, 0);
    g_byte_array_append(stream, bioc->data, bioc->usage);
    /*
     * What follows a device in a real stream: loading peeks one byte past
     * the device for a subsection, and a stream that ends there is an error.
     */
    g_byte_array_append(stream, (const guint8 *)"", 1);
    qemu_fclose(f);
    object_unref(OBJECT(bioc));
    sgi_scanout_cleanup(s);

    sgi_scanout_init(&dst.scanout, NULL, &head_ops, h);
    sgi_scanout_set_mode(&dst.scanout, &small);
    con.cursor_calls = 0;
    con.mouse_on = false;
    bioc = qio_channel_buffer_new(stream->len);
    memcpy(bioc->data, stream->data, stream->len);
    bioc->usage = stream->len;
    f = qemu_file_new_input(QIO_CHANNEL(bioc));
    g_assert_cmpint(vmstate_load_state(f, &vmstate_test_head, &dst, 1,
                                       &error_abort), ==, 0);
    qemu_fclose(f);
    object_unref(OBJECT(bioc));

    g_assert_cmpuint(dst.reg, ==, 1);
    g_assert_cmpuint(dst.scanout.mode.width, ==, 1280);
    g_assert_cmpuint(dst.scanout.frame_width, ==, 1280);
    g_assert_cmpuint(dst.scanout.frames, ==, 3);
    g_assert_cmpuint(dst.scanout.swap_count, ==, 1);
    g_assert_cmpint(timer_expire_time_ns(dst.scanout.retrace_timer), ==,
                    next);
    g_assert_cmpint(find_first_zero_bit(dst.scanout.dirty, 1024), ==, 1024);
    g_assert_cmpint(con.cursor_calls, ==, 1);
    g_assert_cmpint(con.mouse_x, ==, 10);
    g_assert_true(con.mouse_on);

    sgi_scanout_cleanup(&dst.scanout);
    head_free(h);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    module_call_init(MODULE_INIT_QOM);
    qemu_init_clocks(NULL);
    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
    g_test_add_func("/sgi-scanout/retrace-period", test_retrace_period);
    g_test_add_func("/sgi-scanout/swaps", test_swaps);
    g_test_add_func("/sgi-scanout/set-next-retrace", test_set_next_retrace);
    g_test_add_func("/sgi-scanout/dirty-lines", test_dirty_lines);
    g_test_add_func("/sgi-scanout/pre-update", test_pre_update);
    g_test_add_func("/sgi-scanout/cursor-composite", test_cursor_composite);
    g_test_add_func("/sgi-scanout/cursor-host", test_cursor_host);
    g_test_add_func("/sgi-scanout/cursor-clamp", test_cursor_clamp);
    g_test_add_func("/sgi-scanout/migration", test_migration);
    return g_test_run();
}
