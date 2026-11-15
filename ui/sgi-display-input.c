/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Binary input for the SGI display reader. See ui/sgi-display-shm.c for the
 * socket and docs/system/mips/sgi/display-shm.rst for the contract.
 *
 * An SGI machine has one PS/2 keyboard and one PS/2 mouse, so keys and
 * buttons name no head and go to the machine's handlers. Every pointer here
 * is relative. An absolute position is therefore turned into relative
 * motion by closing a loop on the guest cursor: send the remaining error
 * divided by the gain seen so far, wait for the guest to move its cursor,
 * and repeat. IRIX accelerates relative motion, so the gain is not 1 and
 * is learned from each step's result.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "ui/input.h"
#include "ui/sgi-display-priv.h"

/* Steps a target gets before QEMU gives up on reaching it exactly. */
#define ABS_MAX_STEPS 8
/* A target the guest has not answered in this long is stale. */
#define ABS_TTL_MS 1000
#define GAIN_MIN (1.0 / 16)
#define GAIN_MAX 16.0
/* A PS/2 packet carries at most 255 counts, so a step is one packet. */
#define STEP_MAX 255

#define KEY_CODE_MAX 0x2ff

static struct {
    bool pending;
    unsigned head;
    int32_t tx, ty;             /* the target, in head pixels */
    unsigned steps;
    int32_t px, py;             /* the cursor when the last step went out */
    int32_t sx, sy;             /* the last step */
    double gx, gy;              /* observed motion per unit sent */
    int64_t deadline;
    /* Kept across targets: the guest's acceleration does not change. */
    bool gain_known;
    /* Where the host pointer was last put on each head (host pixels). */
    bool have_host[SGI_SHM_MAX_HEADS];
    int32_t hx[SGI_SHM_MAX_HEADS], hy[SGI_SHM_MAX_HEADS];
} loop;

/* What the reader holds down, so losing it cannot leave a key stuck. */
static uint8_t keys_down[(KEY_CODE_MAX + 8) / 8];
static uint64_t buttons_down;

static int32_t step_for(int32_t err, double gain)
{
    double q = err / gain;
    long v;

    if (!err) {
        return 0;
    }
    v = (long)(q < 0 ? q - 0.5 : q + 0.5);
    if (!v) {
        v = err > 0 ? 1 : -1;
    }
    return MAX(-STEP_MAX, MIN(STEP_MAX, v));
}

/* One step toward the target. Returns whether an event was queued. */
static bool abs_step(void)
{
    int32_t cx, cy, ex, ey;
    bool on, queued = false;

    if (!loop.pending ||
        !sgi_display_head_cursor(loop.head, &cx, &cy, &on) ||
        loop.steps >= ABS_MAX_STEPS ||
        qemu_clock_get_ms(QEMU_CLOCK_REALTIME) > loop.deadline) {
        loop.pending = false;
        return false;
    }
    ex = loop.tx - cx;
    ey = loop.ty - cy;
    if (!ex && !ey) {
        loop.pending = false;
        return false;
    }
    loop.sx = step_for(ex, loop.gx);
    loop.sy = step_for(ey, loop.gy);
    if (loop.sx) {
        qemu_input_queue_rel(NULL, INPUT_AXIS_X, loop.sx);
        queued = true;
    }
    if (loop.sy) {
        qemu_input_queue_rel(NULL, INPUT_AXIS_Y, loop.sy);
        queued = true;
    }
    loop.px = cx;
    loop.py = cy;
    loop.steps++;
    return queued;
}

static double learn(double gain, int32_t moved, int32_t sent)
{
    double g;

    if (!sent || !moved || (moved > 0) != (sent > 0)) {
        return gain;
    }
    g = (double)moved / sent;
    return MAX(GAIN_MIN, MIN(GAIN_MAX, g));
}

void sgi_display_input_cursor_moved(unsigned head)
{
    int32_t cx, cy;
    bool on;

    if (!loop.pending || head != loop.head ||
        !sgi_display_head_cursor(head, &cx, &cy, &on)) {
        return;
    }
    if (cx == loop.px && cy == loop.py) {
        /* Not a reply to the step: the cursor went nowhere. */
        return;
    }
    loop.gx = learn(loop.gx, cx - loop.px, loop.sx);
    loop.gy = learn(loop.gy, cy - loop.py, loop.sy);
    if (abs_step()) {
        qemu_input_event_sync();
    }
}

void sgi_display_input_reader_gone(void)
{
    bool queued = false;

    loop.pending = false;
    loop.gain_known = false;
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
        loop.pending = false;
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
        loop.pending = true;
        loop.head = head;
        loop.tx = in->x;
        loop.ty = in->y;
        loop.steps = 0;
        loop.sx = loop.sy = 0;
        if (!loop.gain_known) {
            loop.gx = loop.gy = 1.0;
            loop.gain_known = true;
        }
        loop.deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + ABS_TTL_MS;
        queued = abs_step();
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
            QemuInputEvent evt = {
                .type = INPUT_EVENT_KIND_KEY,
                .key = { .key = in->code, .down = in->value != 0 },
            };

            if (in->value) {
                keys_down[in->code / 8] |= 1 << (in->code % 8);
            } else {
                keys_down[in->code / 8] &= ~(1 << (in->code % 8));
            }
            qemu_input_event_send(NULL, &evt);
            return true;
        }
    case SGI_SHM_INPUT_BUTTON:
        if (in->code >= INPUT_BUTTON__MAX) {
            warn_report_once("sgi-display-shm: button %u is not an "
                             "InputButton; dropped", in->code);
            return false;
        }
        if (in->value) {
            buttons_down |= 1ull << in->code;
        } else {
            buttons_down &= ~(1ull << in->code);
        }
        qemu_input_queue_btn(NULL, in->code, in->value != 0);
        return true;
    case SGI_SHM_INPUT_RELATIVE:
        /* The user moved the pointer: an absolute target is stale. */
        loop.pending = false;
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
