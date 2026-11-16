/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * qemu.pointer-steering, version 1: an absolute position turned into
 * relative motion by closing a loop on the guest cursor, one packet in
 * flight at a time and a gain measured afresh for every step.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/atomic.h"
#include "qemu/error-report.h"
#include "ui/input.h"
#include "ui/display-shm-priv.h"

/* Steps a target gets before the producer gives up on reaching it exactly. */
#define ABS_MAX_STEPS 32
/* A target not reached in this long of guest time is stale. */
#define ABS_TTL_MS 2000
/* A step the guest has not answered in this long moved nothing. */
#define ABS_ANSWER_MS 250
/* A cursor quiet this long after its first move has finished the step. */
#define ABS_SETTLE_MS 40
#define ABS_HELD_STEPS 2
#define GAIN_MIN (1.0 / 16)
#define GAIN_MAX 16.0
/* A PS/2 packet carries at most 127 counts; one step is one packet. */
#define STEP_MAX 127
#define LEAD_QUIET_MS 50
#define LEAD_MAX 200
/* How much of the distance the pointer covers in a step's time to aim ahead. */
#define LEAD_GAIN 0.6
/* A queued click waits no longer than this for a pointer that keeps moving. */
#define DEFER_MS 150

#define REASON_TARGET   1
#define REASON_CLOSE    2
#define REASON_LIMIT    3
#define REASON_CANCEL   4

typedef struct AbsAxis {
    int32_t target;
    int32_t from;               /* the cursor when the last step went out */
    int32_t err;
    int32_t sent;               /* this step, in counts */
    int32_t last;               /* the last step that was not 0 */
    double gain;                /* pixels a count moved on the last step */
    int32_t limit;              /* below every step that overshot */
    unsigned held;
    bool moved_target;
} AbsAxis;

typedef struct Steer {
    DisplayShm *d;
    bool pending, answered;
    DShmViewer *viewer;
    unsigned head;
    unsigned steps;
    AbsAxis ax[2];
    int64_t deadline;
    QEMUTimer *timer;
    int64_t ev_ms;
    int32_t ev_x, ev_y;
    double vel[2];
    int64_t step_ms;
    double cycle_ms;
    int cursor_head;
    bool have_host[DSHM_FEEDBACK_HEADS];
    int32_t hx[DSHM_FEEDBACK_HEADS], hy[DSHM_FEEDBACK_HEADS];
} Steer;

static int64_t guest_ms(void)
{
    return qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
}

static Steer *steer_of(DisplayShm *d)
{
    return d->steer;
}

bool dshm_steer_wanted(DisplayShm *d)
{
    return !d->steering || strcmp(d->steering, "off") != 0;
}

/* The feedback block in the state region: where the guest drew its cursor. */
static void block_write(DShmHead *h)
{
    DisplayShm *d = h->d;
    DShmSteerBlock *b;
    Steer *st = steer_of(d);

    if (!d->state) {
        return;
    }
    b = (DShmSteerBlock *)(d->state->ptr + d->steer_block_offset +
                           (size_t)h->idx * DSHM_STEER_BLOCK_BYTES);
    qatomic_set(&b->seq, b->seq + 1);
    /* the odd count is visible before any field changes */
    smp_wmb();
    qatomic_set(&b->kind, h->steerable ? h->fb_kind : 0);
    qatomic_set(&b->x, h->fb_x);
    qatomic_set(&b->y, h->fb_y);
    qatomic_set(&b->moving, st->pending && st->head == h->idx);
    /* the fields are written before the count turns even */
    smp_wmb();
    qatomic_set(&b->seq, b->seq + 1);
}

static void done_send(Steer *st, unsigned reason)
{
    DShmViewer *v = st->viewer;
    DShmHead *h = dshm_head(st->d, st->head);
    DShmSteerDone m;

    if (!v || v->closing || !v->ext_steer || !h) {
        return;
    }
    m = (DShmSteerDone) {
        .hdr = { .type = DSHM_EXT_TYPE(v->steer_idx, DSHM_STEER_STEER_DONE) },
        .head = st->head, .reason = reason,
        .x = h->fb_x, .y = h->fb_y,
    };
    dshm_queue(v, &m, sizeof(m), DSHM_NO_HANDLE, false);
    dshm_flush(v);
}

/* ---- The click queue --------------------------------------------------- */

static void queue_flush(DisplayShm *d)
{
    DShmViewer *v = d->owner;
    unsigned n = d->steerq_n;

    d->steerq_n = 0;
    timer_del(d->steerq_timer);
    for (unsigned i = 0; i < n && v; i++) {
        dshm_input_apply_deferred(v, d->steerq[i].head, d->steerq[i].key,
                                  d->steerq[i].code, d->steerq[i].down);
    }
}

static void queue_drop(DisplayShm *d)
{
    d->steerq_n = 0;
    timer_del(d->steerq_timer);
}

static void queue_timer(void *opaque)
{
    queue_flush(opaque);
}

/* A button during a move waits, and everything after it: order is kept. */
bool dshm_steer_defer(DShmViewer *v, unsigned head, bool key, unsigned code,
                      bool down)
{
    DisplayShm *d = v->d;
    Steer *st = steer_of(d);

    if (!d->steerq_n && (key || !st->pending)) {
        return false;
    }
    if (d->steerq_n == DSHM_STEER_QUEUE) {
        queue_flush(d);
    }
    if (!d->steerq_n) {
        d->steerq_since = guest_ms();
        timer_mod(d->steerq_timer, d->steerq_since + DEFER_MS);
    }
    d->steerq[d->steerq_n++] = (DShmSteerQ) {
        .key = key, .code = code, .down = down, .head = head,
    };
    return true;
}

/* ---- The loop ---------------------------------------------------------- */

static void abs_stop(Steer *st, unsigned reason, bool flush)
{
    DisplayShm *d = st->d;

    if (!st->pending) {
        return;
    }
    st->pending = false;
    timer_del(st->timer);
    {
        DShmHead *h = dshm_head(d, st->head);

        if (h) {
            block_write(h);
        }
    }
    done_send(st, reason);
    if (flush) {
        queue_flush(d);
    } else {
        queue_drop(d);
    }
}

/* Cancel a move: reason 4. What waited goes in order, or is dropped. */
void dshm_steer_cancel(DisplayShm *d, bool drop)
{
    Steer *st = steer_of(d);

    abs_stop(st, REASON_CANCEL, !drop);
    if (drop) {
        queue_drop(d);
    }
}

void dshm_steer_head_changed(DShmHead *h)
{
    Steer *st = steer_of(h->d);

    if (st->pending && st->head == h->idx) {
        abs_stop(st, REASON_CANCEL, true);
    }
}

static int32_t axis_aim(Steer *st, int i, bool lead_on)
{
    double lead;

    if (!lead_on || st->ax[i].held ||
        guest_ms() - st->ev_ms > LEAD_QUIET_MS) {
        return st->ax[i].target;
    }
    lead = st->vel[i] * st->cycle_ms * LEAD_GAIN;
    lead = MAX(-LEAD_MAX, MIN(LEAD_MAX, lead));
    return st->ax[i].target + (int32_t)lround(lead);
}

static void note_target(Steer *st, int32_t x, int32_t y, bool first)
{
    int64_t now = guest_ms(), dt = now - st->ev_ms;
    const int32_t t[2] = { x, y }, p[2] = { st->ev_x, st->ev_y };

    if (first || dt > LEAD_QUIET_MS) {
        st->vel[0] = st->vel[1] = 0;
    } else if (dt >= 4) {
        for (int i = 0; i < 2; i++) {
            st->vel[i] = 0.5 * st->vel[i] + 0.5 * (t[i] - p[i]) / (double)dt;
        }
    } else {
        return;
    }
    st->ev_ms = now;
    st->ev_x = x;
    st->ev_y = y;
}

static int32_t axis_step(AbsAxis *a)
{
    long v;

    if (!a->err || a->held >= ABS_HELD_STEPS) {
        return 0;
    }
    v = (long)ceil(abs(a->err) / a->gain - 0.5);
    if (v < 1) {
        if (abs(a->last) == 1) {
            /* One count moves over twice the error: as close as it gets. */
            return 0;
        }
        v = 1;
    }
    v = MIN(v, a->limit);
    return a->err > 0 ? v : -v;
}

static void axis_learn(AbsAxis *a, int32_t moved)
{
    /* A move of nothing or against the step teaches nothing. */
    if (!a->sent) {
        return;
    }
    a->held = moved ? 0 : a->held + 1;
    if (!moved || (moved > 0) != (a->sent > 0)) {
        return;
    }
    a->gain = MAX(GAIN_MIN, MIN(GAIN_MAX, (double)moved / a->sent));
    if (!a->moved_target && abs(moved) > abs(a->err)) {
        /* Past the target: never send that many counts again. */
        a->limit = MAX(1, MIN(a->limit, abs(a->sent) - 1));
    }
}

/* One step toward the target. */
static bool abs_step(Steer *st)
{
    DShmHead *h = dshm_head(st->d, st->head);
    int32_t c[2];
    int dx[2] = { 0, 0 };
    bool queued = false;

    if (!h || !h->present) {
        abs_stop(st, REASON_LIMIT, true);
        return false;
    }
    c[0] = h->fb_x;
    c[1] = h->fb_y;
    if (st->steps >= ABS_MAX_STEPS || guest_ms() >= st->deadline) {
        abs_stop(st, REASON_LIMIT, true);
        return false;
    }
    /* A step with nowhere to go ahead of the pointer is tried at the target. */
    for (int pass = 0; pass < 2 && !queued; pass++) {
        for (int i = 0; i < 2; i++) {
            AbsAxis *a = &st->ax[i];

            a->from = c[i];
            a->err = axis_aim(st, i, pass == 0) - c[i];
            a->sent = axis_step(a);
            if (a->sent) {
                a->last = a->sent;
                dx[i] = a->sent;
                queued = true;
            }
        }
    }
    for (int i = 0; i < 2; i++) {
        st->ax[i].moved_target = false;
    }
    if (!queued) {
        abs_stop(st, st->ax[0].err || st->ax[1].err ? REASON_CLOSE :
                                                       REASON_TARGET, true);
        return false;
    }
    dshm_inject_steer(h, dx[0], dx[1]);
    st->steps++;
    st->answered = false;
    if (st->step_ms) {
        double took = guest_ms() - st->step_ms;

        st->cycle_ms = 0.7 * st->cycle_ms + 0.3 * MIN(took, 150.0);
    }
    st->step_ms = guest_ms();
    timer_mod(st->timer, guest_ms() + ABS_ANSWER_MS);
    return true;
}

static void abs_timer(void *opaque)
{
    Steer *st = opaque;
    DShmHead *h = dshm_head(st->d, st->head);

    if (!st->pending) {
        return;
    }
    if (!st->answered) {
        abs_stop(st, REASON_LIMIT, true);
        return;
    }
    if (!h || !h->present) {
        abs_stop(st, REASON_LIMIT, true);
        return;
    }
    axis_learn(&st->ax[0], h->fb_x - st->ax[0].from);
    axis_learn(&st->ax[1], h->fb_y - st->ax[1].from);
    if (st->d->steerq_n && guest_ms() - st->d->steerq_since >= DEFER_MS) {
        queue_flush(st->d);
    }
    if (abs_step(st)) {
        dshm_inject_sync(h);
    }
}

/* A step is answered when the cursor leaves its place, done once still. */
void dshm_steer_cursor_moved(DShmHead *h)
{
    Steer *st = steer_of(h->d);

    if (h->cursor_visible || h->fb_kind == 2) {
        st->cursor_head = h->idx;
    }
    block_write(h);
    if (!st->pending || h->idx != st->head) {
        return;
    }
    if (!st->answered) {
        if (h->fb_x == st->ax[0].from && h->fb_y == st->ax[1].from) {
            return;
        }
        st->answered = true;
    }
    timer_mod(st->timer, MIN(guest_ms() + ABS_SETTLE_MS, st->deadline));
}

static void retarget(Steer *st, int32_t x, int32_t y)
{
    const int32_t t[2] = { x, y };

    for (int i = 0; i < 2; i++) {
        AbsAxis *a = &st->ax[i];

        if (a->target != t[i]) {
            a->target = t[i];
            a->limit = STEP_MAX;
            a->held = 0;
            a->moved_target = true;
        }
    }
    st->steps = 0;
    st->deadline = guest_ms() + ABS_TTL_MS;
}

void dshm_steer_abs(DShmViewer *v, DShmHead *h, int x, int y)
{
    DisplayShm *d = v->d;
    Steer *st = steer_of(d);
    unsigned head = h->idx;

    /* Targets stay on the head. */
    x = MAX(0, MIN(x, (int)h->width - 1));
    y = MAX(0, MIN(y, (int)h->height - 1));
    st->viewer = v;
    if (st->cursor_head >= 0 && (unsigned)st->cursor_head != head) {
        /* Cursor on another head: no target to close on, move by delta. */
        bool queued = false;

        if (st->pending) {
            abs_stop(st, REASON_CANCEL, true);
        }
        if (st->have_host[head]) {
            int dx = x - st->hx[head], dy = y - st->hy[head];

            if (dx || dy) {
                dshm_inject_steer(h, dx, dy);
                queued = true;
            }
        }
        st->have_host[head] = true;
        st->hx[head] = x;
        st->hy[head] = y;
        if (queued) {
            dshm_inject_sync(h);
        }
        return;
    }
    if (st->pending && st->head != head) {
        abs_stop(st, REASON_CANCEL, true);
    }
    if (st->pending) {
        note_target(st, x, y, false);
        retarget(st, x, y);
    } else {
        bool queued;

        note_target(st, x, y, true);
        st->step_ms = 0;
        if (st->cycle_ms < 20) {
            st->cycle_ms = 60;
        }
        st->pending = true;
        st->head = head;
        st->steps = 0;
        st->ax[0] = (AbsAxis) { .target = x, .gain = 1.0, .limit = STEP_MAX };
        st->ax[1] = (AbsAxis) { .target = y, .gain = 1.0, .limit = STEP_MAX };
        st->deadline = guest_ms() + ABS_TTL_MS;
        block_write(h);
        queued = abs_step(st);
        if (queued) {
            dshm_inject_sync(h);
        }
    }
    st->have_host[head] = true;
    st->hx[head] = x;
    st->hy[head] = y;
}

void dshm_steer_init(DisplayShm *d)
{
    Steer *st = g_new0(Steer, 1);

    st->d = d;
    st->cursor_head = -1;
    st->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, abs_timer, st);
    d->steer = st;
    d->steerq_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, queue_timer, d);
}
