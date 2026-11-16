/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Binary input for the SGI display reader. See ui/sgi-display-shm.c for the
 * socket and docs/system/mips/sgi/display-shm.rst for the contract.
 *
 * An SGI machine has one PS/2 keyboard and one PS/2 mouse, so keys and
 * buttons name no head and go to the machine's handlers. Every pointer here
 * is relative. An absolute position is therefore turned into relative
 * motion by closing a loop on the guest cursor: send a step, wait until the
 * guest has moved its cursor for it, and size the next step from what that
 * one did.
 *
 * IRIX accelerates relative motion, per axis and per PS/2 packet: a packet
 * of up to 4 counts moves the cursor that many pixels, a larger one 2n - 4
 * (observed on IRIX 6.5.30 with the VPro). The pixels a count buys
 * therefore depend on the size of the step, so a gain learned on one step
 * is wrong for a step of another size. The loop keeps it converging anyway:
 * one packet in flight at a time, a gain measured afresh for every step of
 * every target, and on each axis a step never as large as one that already
 * overshot the target, so the steps shrink toward the end.
 *
 * A pointer that keeps moving gives a new target faster than the guest
 * answers. A target that arrives with a step in flight only moves the target:
 * the next step is sized when the answer comes. QEMU also aims ahead of the
 * pointer by the distance it covers while a step lands, so the cursor
 * follows closely instead of behind.
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "ui/input.h"
#include "ui/sgi-display-priv.h"
#include "trace.h"

/* Steps a target gets before QEMU gives up on reaching it exactly. */
#define ABS_MAX_STEPS 32
/* A target not reached in this long of guest time is stale. */
#define ABS_TTL_MS 2000
/* A step the guest has not answered in this long moved nothing. */
#define ABS_ANSWER_MS 250
/*
 * A cursor quiet this long after its first move has finished the step.
 * IRIX takes up to 30 ms to act on a packet, and a change of cursor shape
 * can move the reported corner before that.
 */
#define ABS_SETTLE_MS 40
/* Steps in a row that leave an axis where it was before it gives up. */
#define ABS_HELD_STEPS 2
#define GAIN_MIN (1.0 / 16)
#define GAIN_MAX 16.0
/*
 * QEMU's PS/2 mouse carries at most 127 counts in a packet and splits a
 * larger move into several; the guest moves its cursor once for each. One
 * step is one packet, so one step has one answer.
 */
#define STEP_MAX 127

#define KEY_CODE_MAX 0x2ff

/* A pointer that sent no target for this long has stopped. */
#define LEAD_QUIET_MS 50
#define LEAD_MAX 200
/*
 * Of the distance the pointer covers in one step's time, how much to aim
 * ahead. Measured with the pointer moving at 600 to 3000 px/s on IRIX: 0.5 to
 * 0.7 cut the error to 60 to 70 percent of aiming at the target, and 0.85
 * started to overshoot on turns.
 */
#define LEAD_GAIN 0.6

typedef struct AbsAxis {
    int32_t target;
    int32_t from;               /* the cursor when the last step went out */
    int32_t err;                /* the target less that */
    int32_t sent;               /* this step, in counts */
    int32_t last;               /* the last step that was not 0 */
    double gain;                /* pixels a count moved on the last step */
    int32_t limit;              /* below every step that overshot */
    unsigned held;              /* steps in a row that moved nothing */
    bool moved_target;          /* the target moved since the step went out */
} AbsAxis;

static struct {
    bool pending;
    bool answered;              /* the cursor moved since the step went out */
    unsigned head;
    unsigned steps;
    AbsAxis ax[2];
    int64_t deadline;
    QEMUTimer *timer;
    /* The host pointer's recent motion, in guest milliseconds. */
    int64_t ev_ms;              /* when the last target arrived */
    int32_t ev_x, ev_y;         /* ... and what it was */
    double vel[2];              /* px per ms, smoothed */
    int64_t step_ms;            /* when the step in flight went out */
    double cycle_ms;            /* how long a step takes to land */
    /* Where the host pointer was last put on each head (host pixels). */
    bool have_host[SGI_SHM_MAX_HEADS];
    int32_t hx[SGI_SHM_MAX_HEADS], hy[SGI_SHM_MAX_HEADS];
} loop;

/* What the reader holds down, so losing it cannot leave a key stuck. */
static uint8_t keys_down[(KEY_CODE_MAX + 8) / 8];
static uint64_t buttons_down;

/*
 * Buttons the reader sent while a move was still closing on its target: the
 * click belongs where the pointer is, not where the guest cursor has got to,
 * so each waits for the move to end. In order, and one packet each. A key
 * that arrives behind a waiting button waits too, so a modifier cannot
 * overtake the click it belongs to.
 */
#define BTN_DEFER_MAX 16
/* A press does not wait longer than this for a pointer that keeps moving. */
#define BTN_DEFER_MS 150
static struct {
    unsigned n;
    int64_t since;
    struct {
        bool key;               /* a key, not a button */
        unsigned code;
        bool down;
    } q[BTN_DEFER_MAX];
} deferred;

static int64_t guest_ms(void)
{
    return qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
}

/*
 * Where to aim for one axis: the target, and a little beyond it in the
 * direction the host pointer is moving, by the distance it covers while a
 * step lands. A pointer that has stopped (no target for LEAD_QUIET_MS) is aimed
 * at exactly.
 */
static int32_t axis_aim(int i, bool lead_on)
{
    double lead;

    /*
     * An axis that moved nothing on the last step may be pinned at the edge
     * of the screen by a lead that points past it: aim at the target.
     */
    if (!lead_on || loop.ax[i].held ||
        guest_ms() - loop.ev_ms > LEAD_QUIET_MS) {
        return loop.ax[i].target;
    }
    lead = loop.vel[i] * loop.cycle_ms * LEAD_GAIN;
    lead = MAX(-LEAD_MAX, MIN(LEAD_MAX, lead));
    return loop.ax[i].target + (int32_t)lround(lead);
}

/* A new target from the host: update the pointer's velocity, or restart it. */
static void note_target(int32_t x, int32_t y, bool first)
{
    int64_t now = guest_ms(), dt = now - loop.ev_ms;
    const int32_t t[2] = { x, y }, p[2] = { loop.ev_x, loop.ev_y };

    if (first || dt > LEAD_QUIET_MS) {
        loop.vel[0] = loop.vel[1] = 0;
    } else if (dt >= 4) {
        for (int i = 0; i < 2; i++) {
            loop.vel[i] = 0.5 * loop.vel[i] + 0.5 * (t[i] - p[i]) / (double)dt;
        }
    } else {
        return;                 /* a burst: keep the older sample */
    }
    loop.ev_ms = now;
    loop.ev_x = x;
    loop.ev_y = y;
}

static void button_apply(unsigned button, bool down)
{
    if (down) {
        buttons_down |= 1ull << button;
    } else {
        buttons_down &= ~(1ull << button);
    }
    qemu_input_queue_btn(NULL, button, down);
}

static void key_apply(unsigned code, bool down)
{
    QemuInputEvent evt = {
        .type = INPUT_EVENT_KIND_KEY,
        .key = { .key = code, .down = down },
    };

    if (down) {
        keys_down[code / 8] |= 1 << (code % 8);
    } else {
        keys_down[code / 8] &= ~(1 << (code % 8));
    }
    qemu_input_event_send(NULL, &evt);
}

/* Send what waited for the move, a packet each so none is merged away. */
static void buttons_flush(void)
{
    for (unsigned i = 0; i < deferred.n; i++) {
        if (deferred.q[i].key) {
            key_apply(deferred.q[i].code, deferred.q[i].down);
        } else {
            button_apply(deferred.q[i].code, deferred.q[i].down);
        }
        qemu_input_event_sync();
    }
    deferred.n = 0;
}

static void defer_push(bool key, unsigned code, bool down)
{
    if (deferred.n == BTN_DEFER_MAX) {
        buttons_flush();
    }
    if (!deferred.n) {
        deferred.since = guest_ms();
    }
    deferred.q[deferred.n].key = key;
    deferred.q[deferred.n].code = code;
    deferred.q[deferred.n].down = down;
    deferred.n++;
}

static void abs_stop(const char *why)
{
    int32_t cx = 0, cy = 0;
    bool on;

    sgi_display_head_cursor(loop.head, &cx, &cy, &on);
    trace_sgi_display_abs_stop(cx, cy, why);
    loop.pending = false;
    timer_del(loop.timer);
    buttons_flush();
}

/*
 * The counts for one axis's next step: the error over the gain, halves
 * rounded toward zero, at most STEP_MAX and below any step that overshot.
 */
static int32_t axis_step(AbsAxis *a)
{
    long v;

    if (!a->err || a->held >= ABS_HELD_STEPS) {
        return 0;
    }
    v = (long)ceil(abs(a->err) / a->gain - 0.5);
    if (v < 1) {
        if (abs(a->last) == 1) {
            /* One count moves more than twice the error: this is as close. */
            return 0;
        }
        /* The gain came from a larger step; measure one count. */
        v = 1;
    }
    v = MIN(v, a->limit);
    return a->err > 0 ? v : -v;
}

/* What the last step did on one axis: moved pixels for its counts. */
static void axis_learn(AbsAxis *a, int32_t moved)
{
    /*
     * A move of nothing or against the step is not its answer: the axis is
     * held at the edge of the screen, or the guest changed its cursor's
     * shape and so the glyph's corner the VPro reports. It teaches nothing,
     * and an axis that moves nothing ABS_HELD_STEPS times in a row is held.
     */
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

/* One step toward the target. Returns whether an event was queued. */
static bool abs_step(void)
{
    int32_t c[2];
    bool on, queued = false;

    if (!sgi_display_head_cursor(loop.head, &c[0], &c[1], &on)) {
        abs_stop("no head");
        return false;
    }
    if (loop.steps >= ABS_MAX_STEPS) {
        abs_stop("out of steps");
        return false;
    }
    if (guest_ms() >= loop.deadline) {
        abs_stop("out of time");
        return false;
    }
    /*
     * A step that has nowhere to go ahead of the pointer is tried again at
     * the target itself: the move is not over while the cursor is at the
     * lead and the pointer is not.
     */
    for (int pass = 0; pass < 2 && !queued; pass++) {
        for (int i = 0; i < 2; i++) {
            AbsAxis *a = &loop.ax[i];

            a->from = c[i];
            a->err = axis_aim(i, pass == 0) - c[i];
            a->sent = axis_step(a);
            if (a->sent) {
                a->last = a->sent;
                qemu_input_queue_rel(NULL, i ? INPUT_AXIS_Y : INPUT_AXIS_X,
                                     a->sent);
                queued = true;
            }
        }
    }
    for (int i = 0; i < 2; i++) {
        loop.ax[i].moved_target = false;
    }
    if (!queued) {
        abs_stop(loop.ax[0].err || loop.ax[1].err ? "as close as it gets"
                                                  : "on target");
        return false;
    }
    trace_sgi_display_abs_step(c[0], c[1], loop.ax[0].sent, loop.ax[1].sent,
                               (int)(loop.ax[0].gain * 1000),
                               (int)(loop.ax[1].gain * 1000));
    loop.steps++;
    loop.answered = false;
    if (loop.step_ms) {
        double took = guest_ms() - loop.step_ms;
        loop.cycle_ms = 0.7 * loop.cycle_ms + 0.3 * MIN(took, 150.0);
    }
    loop.step_ms = guest_ms();
    timer_mod(loop.timer, guest_ms() + ABS_ANSWER_MS);
    return true;
}

static void abs_timer(void *opaque)
{
    int32_t cx, cy;
    bool on;

    if (!loop.pending) {
        return;
    }
    if (!loop.answered) {
        abs_stop("no answer");
        return;
    }
    if (!sgi_display_head_cursor(loop.head, &cx, &cy, &on)) {
        abs_stop("no head");
        return;
    }
    trace_sgi_display_abs_answer(cx, cy, cx - loop.ax[0].from,
                                 cy - loop.ax[1].from);
    axis_learn(&loop.ax[0], cx - loop.ax[0].from);
    axis_learn(&loop.ax[1], cy - loop.ax[1].from);
    if (deferred.n && guest_ms() - deferred.since >= BTN_DEFER_MS) {
        buttons_flush();
    }
    if (abs_step()) {
        qemu_input_event_sync();
    }
}

/*
 * The guest moved its cursor. The step is answered once the cursor has left
 * where it was when the step went out; the answer is complete once it has
 * been still for ABS_SETTLE_MS, so a guest that reports a move in parts,
 * or late, is measured whole.
 */
void sgi_display_input_cursor_moved(unsigned head)
{
    int32_t cx, cy;
    bool on;

    if (!loop.pending || head != loop.head ||
        !sgi_display_head_cursor(head, &cx, &cy, &on)) {
        return;
    }
    if (!loop.answered) {
        if (cx == loop.ax[0].from && cy == loop.ax[1].from) {
            /* Not the answer: the cursor has not moved yet. */
            return;
        }
        loop.answered = true;
    }
    /* A cursor that never keeps still still ends at the deadline. */
    timer_mod(loop.timer, MIN(guest_ms() + ABS_SETTLE_MS, loop.deadline));
}

void sgi_display_input_reader_gone(void)
{
    bool queued = false;

    loop.pending = false;
    deferred.n = 0;
    if (loop.timer) {
        timer_del(loop.timer);
    }
    memset(loop.have_host, 0, sizeof(loop.have_host));
    /* A reader that crashes or is replaced mid-press releases what it held. */
    for (unsigned code = 1; code <= KEY_CODE_MAX; code++) {
        if (keys_down[code / 8] & (1 << (code % 8))) {
            QemuInputEvent evt = {
                .type = INPUT_EVENT_KIND_KEY,
                .key = { .key = code, .down = false },
            };

            qemu_input_event_send(NULL, &evt);
            queued = true;
        }
    }
    for (unsigned b = 0; b < INPUT_BUTTON__MAX; b++) {
        if (buttons_down & (1ull << b)) {
            qemu_input_queue_btn(NULL, b, false);
            queued = true;
        }
    }
    memset(keys_down, 0, sizeof(keys_down));
    buttons_down = 0;
    if (queued) {
        qemu_input_event_sync();
    }
}

/*
 * A new target while a step is in flight. Each axis keeps its measured gain
 * but forgets what overshot the old target, and the budget of steps and time
 * starts again: a pointer that keeps moving is a new target every time, not a
 * loop that fails to converge.
 */
static void retarget(int32_t x, int32_t y)
{
    const int32_t t[2] = { x, y };

    for (int i = 0; i < 2; i++) {
        AbsAxis *a = &loop.ax[i];

        if (a->target != t[i]) {
            a->target = t[i];
            a->limit = STEP_MAX;
            a->held = 0;
            a->moved_target = true;
        }
    }
    loop.steps = 0;
    loop.deadline = guest_ms() + ABS_TTL_MS;
}

static bool input_absolute(const SGIShmMsgInput *in)
{
    unsigned head = in->head;
    int cur = sgi_display_cursor_head();
    bool queued = false;

    if (head >= sgi_display_head_count()) {
        warn_report_once("sgi-display-shm: absolute input names head %u, "
                         "which does not exist; dropped", head);
        return false;
    }
    if (cur >= 0 && (unsigned)cur != head) {
        /*
         * The guest cursor is on another head and QEMU does not know how
         * the guest arranged its screens, so there is no target to close
         * on: move by what the host pointer moved on this head.
         */
        if (loop.pending) {
            abs_stop("other head");
        }
        if (loop.have_host[head]) {
            int32_t dx = in->x - loop.hx[head], dy = in->y - loop.hy[head];

            if (dx) {
                qemu_input_queue_rel(NULL, INPUT_AXIS_X, dx);
                queued = true;
            }
            if (dy) {
                qemu_input_queue_rel(NULL, INPUT_AXIS_Y, dy);
                queued = true;
            }
        }
    } else {
        int32_t cx = 0, cy = 0;
        bool on;

        if (!loop.timer) {
            loop.timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, abs_timer, NULL);
        }
        if (loop.pending && loop.head == head) {
            /*
             * The host pointer is still moving. A step is in flight, and
             * until the guest answers it QEMU does not know where its cursor
             * will be, so a second packet would be sized from a stale
             * position and pile onto the first. Move the target; the answer
             * to that step sizes the next one toward it.
             */
            note_target(in->x, in->y, false);
            retarget(in->x, in->y);
        } else {
            sgi_display_head_cursor(head, &cx, &cy, &on);
            trace_sgi_display_abs_target(head, in->x, in->y, cx, cy);
            note_target(in->x, in->y, true);
            loop.step_ms = 0;
            if (loop.cycle_ms < 20) {
                loop.cycle_ms = 60;
            }
            loop.pending = true;
            loop.head = head;
            loop.steps = 0;
            /* The gain depends on the step, so each target relearns it. */
            loop.ax[0] = (AbsAxis) { .target = in->x, .gain = 1.0,
                                     .limit = STEP_MAX };
            loop.ax[1] = (AbsAxis) { .target = in->y, .gain = 1.0,
                                     .limit = STEP_MAX };
            loop.deadline = guest_ms() + ABS_TTL_MS;
            queued = abs_step();
        }
    }
    loop.have_host[head] = true;
    loop.hx[head] = in->x;
    loop.hy[head] = in->y;
    return queued;
}

bool sgi_display_input_apply(const SGIShmMsgInput *in)
{
    switch (in->kind) {
    case SGI_SHM_INPUT_KEY:
        if (in->code < 1 || in->code > KEY_CODE_MAX) {
            warn_report_once("sgi-display-shm: key code 0x%x is outside "
                             "1..0x%x; dropped", in->code, KEY_CODE_MAX);
            return false;
        } else {
            if (deferred.n) {
                defer_push(true, in->code, in->value != 0);
                return false;
            }
            key_apply(in->code, in->value != 0);
            return true;
        }
    case SGI_SHM_INPUT_BUTTON:
        if (in->code >= INPUT_BUTTON__MAX) {
            warn_report_once("sgi-display-shm: button %u is not an "
                             "InputButton; dropped", in->code);
            return false;
        }
        if (loop.pending) {
            defer_push(false, in->code, in->value != 0);
            return false;
        }
        button_apply(in->code, in->value != 0);
        return true;
    case SGI_SHM_INPUT_RELATIVE:
        /* The user moved the pointer: an absolute target is stale. */
        if (loop.pending) {
            abs_stop("relative input");
        }
        if (in->x) {
            qemu_input_queue_rel(NULL, INPUT_AXIS_X, in->x);
        }
        if (in->y) {
            qemu_input_queue_rel(NULL, INPUT_AXIS_Y, in->y);
        }
        return in->x || in->y;
    case SGI_SHM_INPUT_ABSOLUTE:
        return input_absolute(in);
    default:
        warn_report_once("sgi-display-shm: input kind %u is unknown; "
                         "dropped", in->kind);
        return false;
    }
}
