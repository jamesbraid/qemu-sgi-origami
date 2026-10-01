/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI graphics scanout: one head's binding to a QEMU console.
 *
 * SGIScanout owns the console, the presented frame, which display lines
 * are dirty, the cursor, the vertical-retrace timer and the queue of swap
 * records. The family owns what a pixel looks like: the scanout asks its
 * resolve_row() hook for each dirty line and presents only those lines.
 * Display line 0 is the top of the monitor.
 *
 * The retrace period is a parameter the family derives from its own mode
 * tables: frame_clocks clocks of a clock_hz clock per frame, blanking
 * included. Retrace n falls at epoch + n * frame_clocks / clock_hz seconds
 * on QEMU_CLOCK_VIRTUAL, computed from the epoch each time so the phase
 * does not drift by a rounded period per frame. A mode with clock_hz 0 has
 * no retrace. The mode may come from guest-programmed tables, so it is
 * checked: a size past SGI_SCANOUT_MAX_DIM presents nothing, and a period
 * under SGI_SCANOUT_MIN_PERIOD_NS has no retrace, each with a guest error.
 *
 * A swap record takes effect at the first retrace after it is queued, one
 * record per retrace, in order: that retrace publishes SWAPPED with the
 * record's cookie before calling the family's retrace() hook, so the
 * family's bank select and its interrupt see the same frame, and then
 * marks every line dirty so the next update shows the new bank. One record
 * per retrace follows MGRAS, whose kernel flips one bank per retrace
 * interrupt; a family that completes many tags at one retrace keeps its
 * own list and does that from retrace().
 *
 * The cursor is by default the host's, through qemu_console_set_cursor()
 * and qemu_console_set_mouse(), so moving it costs no repaint; a screendump
 * then does not show it. With composite_cursor set the scanout draws it
 * into the presented frame instead, repainting the lines it covered and
 * covers.
 *
 * Every field is main-loop owned.
 */
#ifndef HW_DISPLAY_SGI_SCANOUT_H
#define HW_DISPLAY_SGI_SCANOUT_H

#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "hw/display/sgi/board-memory.h"
#include "hw/display/sgi/head-ops.h"

#define SGI_CURSOR_MAX 64
#define SGI_SWAP_MAX 16
#define SGI_SCANOUT_MAX_DIM 16384
/* 1 kHz: well above any SGI display's field rate. */
#define SGI_SCANOUT_MIN_PERIOD_NS 1000000

typedef struct SGIScanoutMode {
    uint32_t width, height;     /* visible pixels and lines */
    uint64_t frame_clocks;      /* clocks per frame, blanking included */
    uint32_t clock_hz;          /* the clock they count; 0: no retrace */
} SGIScanoutMode;

typedef struct SGISwapRecord {
    uint64_t seq;
    uint64_t cookie;
} SGISwapRecord;

typedef struct SGIScanout {
    QemuConsole *con;
    const SGIHeadOps *ops;
    void *opaque;
    bool composite_cursor;      /* draw the cursor into the frame */

    SGIScanoutMode mode;
    uint32_t *frame;            /* width x height of 0x00RRGGBB */
    unsigned long *dirty;       /* one bit per display line */
    uint32_t frame_width, frame_height; /* what frame was allocated for */

    QEMUTimer *retrace_timer;
    int64_t epoch;              /* virtual time of retrace 0 */
    uint64_t frames;            /* retraces since the epoch */

    SGISwapRecord swaps[SGI_SWAP_MAX];
    uint32_t swap_head, swap_count;

    /* 0xAARRGGBB, row-major, cursor_w x cursor_h */
    uint32_t cursor_image[SGI_CURSOR_MAX * SGI_CURSOR_MAX];
    uint32_t cursor_w, cursor_h;
    int32_t cursor_hot_x, cursor_hot_y;
    int32_t cursor_x, cursor_y; /* the hot spot, in display coordinates */
    bool cursor_on;
    /* lines the composited cursor was last drawn over, first > last: none */
    int32_t cursor_drawn_first, cursor_drawn_last;
} SGIScanout;

/*
 * Bind a scanout to a new console of dev. ops->resolve_row is required;
 * ops->publish is required once a swap is queued. No mode yet: nothing
 * is presented and no retrace runs until sgi_scanout_set_mode().
 */
void sgi_scanout_init(SGIScanout *s, DeviceState *dev,
                      const SGIHeadOps *ops, void *opaque);
void sgi_scanout_cleanup(SGIScanout *s);

/* A new mode: new frame, everything dirty, retrace phase restarted now. */
void sgi_scanout_set_mode(SGIScanout *s, const SGIScanoutMode *mode);
/* Forget swaps and the cursor, repaint, restart the retrace phase. */
void sgi_scanout_reset(SGIScanout *s);

/* Virtual time of retrace n of the current mode, or -1 with no retrace. */
int64_t sgi_scanout_retrace_time(const SGIScanout *s, uint64_t n);

/*
 * Put the next retrace at virtual time when, with the ones after it a
 * period apart. The retrace count carries on from where it is. For a
 * family whose own migration stream holds the retrace phase: it calls this
 * from its post_load, after the scanout's, so the destination neither
 * replays every retrace since its own reset nor drifts off the source's
 * phase. A when already past fires at once, and so does every retrace
 * between it and now. A negative when, such as the -1 of an idle timer's
 * expiry, means now: the next retrace fires at once and the rest follow a
 * period apart. With no retrace in the current mode this does nothing.
 */
void sgi_scanout_set_next_retrace(SGIScanout *s, int64_t when);

void sgi_scanout_invalidate(SGIScanout *s, int first, int count);
void sgi_scanout_invalidate_all(SGIScanout *s);
/*
 * Plane rows y0 .. y1 changed. The plane is the displayed one and exactly
 * as tall as the mode: a bottom-origin row maps to display line
 * p->height - 1 - y.
 */
void sgi_scanout_invalidate_plane(SGIScanout *s, const SGIPlane *p,
                                  int y0, int y1);

/* Resolve the dirty lines, draw the cursor if composited, present. */
void sgi_scanout_update(SGIScanout *s);

/*
 * A new cursor image, w x h pixels of 0xAARRGGBB (alpha 0 transparent),
 * with its hot spot at (hot_x, hot_y) in the image. A size past
 * SGI_CURSOR_MAX keeps the image's top-left SGI_CURSOR_MAX square, with a
 * guest error; an empty image hides the cursor.
 */
void sgi_scanout_cursor_define(SGIScanout *s, uint32_t w, uint32_t h,
                               int hot_x, int hot_y, const uint32_t *argb);
/* The hot spot is at display (x, y); on is whether the cursor shows. */
void sgi_scanout_cursor_move(SGIScanout *s, int x, int y, bool on);

/*
 * Queue a swap to take effect at a coming retrace. With SGI_SWAP_MAX
 * already waiting, publishes FAILED with SGI_FAIL_FULL at once.
 */
void sgi_scanout_queue_swap(SGIScanout *s, uint64_t seq, uint64_t cookie);

/*
 * Migration: mode, retrace phase, queued swaps and the cursor, as a
 * subsection of the family's own description, defined the same way as the
 * board memory's (see board-memory.h). Loading marks everything dirty and
 * re-arms the retrace on the saved phase.
 */
extern const VMStateDescription vmstate_sgi_scanout;

#define VMSTATE_SGI_SCANOUT(_field, _state)                              \
    VMSTATE_STRUCT(_field, _state, 0, vmstate_sgi_scanout, SGIScanout)

#define DEFINE_VMSTATE_SGI_SCANOUT(_vmsd, _name, _state, _field)         \
    static const VMStateDescription _vmsd = {                            \
        .name = (_name),                                                 \
        .version_id = 1,                                                 \
        .minimum_version_id = 1,                                         \
        .fields = (const VMStateField[]) {                               \
            VMSTATE_SGI_SCANOUT(_field, _state),                         \
            VMSTATE_END_OF_LIST()                                        \
        },                                                               \
    }

#endif
