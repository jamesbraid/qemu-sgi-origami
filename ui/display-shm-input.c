/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Input for the display-shm producer: reports, ownership, bounds and
 * release. Events reach the guest through the head's console. The pointer
 * steering loop is in display-shm-steer.c.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "ui/input.h"
#include "ui/display-shm-priv.h"

#define KEY_MAX             0x2ff
#define REL_CLAMP           32767
/* Motion forwarded per axis, and wheel notches, per 10 ms of guest time. */
#define MOTION_WINDOW_NS    (10 * SCALE_MS)
#define MOTION_BUDGET       4096
#define WHEEL_BUDGET        64

static const InputButton button_map[] = {
    INPUT_BUTTON_LEFT, INPUT_BUTTON_MIDDLE, INPUT_BUTTON_RIGHT,
    INPUT_BUTTON_WHEEL_UP, INPUT_BUTTON_WHEEL_DOWN, INPUT_BUTTON_SIDE,
    INPUT_BUTTON_EXTRA, INPUT_BUTTON_WHEEL_LEFT, INPUT_BUTTON_WHEEL_RIGHT,
};
#define BUTTONS (sizeof(button_map) / sizeof(button_map[0]))

static bool is_wheel(unsigned b)
{
    return b == 3 || b == 4 || b == 7 || b == 8;
}

/* ---- Echo and injection ----------------------------------------------- */

void dshm_echo(DisplayShm *d, uint32_t head, uint32_t kind, uint32_t code,
               int32_t a, int32_t b)
{
    d->echo_seq++;
    for (unsigned i = 0; i < d->n_viewers; i++) {
        DShmViewer *v = d->viewers[i];
        DShmInputEcho m = {
            .hdr = { .type = DSHM_EXT_TYPE(v->test_idx, DSHM_TEST_INPUT_ECHO) },
            .head = head, .kind = kind, .code = code, .a = a, .b = b,
            .seq = d->echo_seq,
        };

        if (v->ext_test) {
            dshm_queue(v, &m, sizeof(m), DSHM_NO_HANDLE, false);
            dshm_flush(v);
        }
    }
}

static void budget_refill(DisplayShm *d)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (now - d->window_ns >= MOTION_WINDOW_NS || now < d->window_ns) {
        d->window_ns = now;
        d->rel_used[0] = d->rel_used[1] = 0;
        d->wheel_used = 0;
    }
}

static int limit_axis(DisplayShm *d, int axis, int v)
{
    int64_t room = MOTION_BUDGET - d->rel_used[axis];
    int64_t take = MIN(llabs(v), MAX(room, 0));

    d->rel_used[axis] += take;
    return v < 0 ? -take : take;
}

/* kind 3 for the viewer's motion, 6 for a steering packet. */
static void inject_rel_kind(DShmHead *h, int dx, int dy, uint32_t kind)
{
    DisplayShm *d = h->d;

    budget_refill(d);
    dx = limit_axis(d, 0, dx);
    dy = limit_axis(d, 1, dy);
    if (dx) {
        qemu_input_queue_rel(h->con, INPUT_AXIS_X, dx);
    }
    if (dy) {
        qemu_input_queue_rel(h->con, INPUT_AXIS_Y, dy);
    }
    if (dx || dy) {
        dshm_echo(d, h->idx, kind, 0, dx, dy);
    }
}

void dshm_inject_rel(DShmHead *h, int dx, int dy)
{
    inject_rel_kind(h, dx, dy, 3);
}

void dshm_inject_steer(DShmHead *h, int dx, int dy)
{
    inject_rel_kind(h, dx, dy, 6);
}

void dshm_inject_key(DShmHead *h, unsigned code, bool down)
{
    QemuInputEvent evt = {
        .type = INPUT_EVENT_KIND_KEY,
        .key = { .key = code, .down = down },
    };

    qemu_input_event_send(h->con, &evt);
    dshm_echo(h->d, h->idx, 1, code, down, 0);
}

void dshm_inject_button(DShmHead *h, unsigned button, bool down)
{
    qemu_input_queue_btn(h->con, button_map[button], down);
    dshm_echo(h->d, h->idx, 2, button, down, 0);
}

void dshm_inject_abs(DShmHead *h, int x, int y)
{
    x = MAX(0, MIN(x, (int)h->width - 1));
    y = MAX(0, MIN(y, (int)h->height - 1));
    qemu_input_queue_abs(h->con, INPUT_AXIS_X, x, 0, h->width);
    qemu_input_queue_abs(h->con, INPUT_AXIS_Y, y, 0, h->height);
    dshm_echo(h->d, h->idx, 4, 0, x, y);
}

void dshm_inject_sync(DShmHead *h)
{
    qemu_input_event_sync();
    dshm_echo(h->d, h->idx, 5, 0, 0, 0);
}

/* ---- Reports ----------------------------------------------------------- */

static void rel_flush(DisplayShm *d)
{
    DShmHead *h;

    if (!d->acc_on) {
        return;
    }
    d->acc_on = false;
    h = dshm_head(d, d->acc_head);
    if (h && h->present) {
        int dx = MAX(-REL_CLAMP, MIN(REL_CLAMP, d->acc_x));
        int dy = MAX(-REL_CLAMP, MIN(REL_CLAMP, d->acc_y));

        if (dx || dy) {
            dshm_inject_rel(h, dx, dy);
        }
    }
    d->acc_x = d->acc_y = 0;
}

void dshm_input_end_report(DisplayShm *d)
{
    rel_flush(d);
    if (d->report_n) {
        d->report_n = 0;
        qemu_input_event_sync();
        dshm_echo(d, d->report_head, 5, 0, 0, 0);
    }
}

/* One more event in the report; 256 end it. */
static void report_count(DisplayShm *d, unsigned head)
{
    d->report_head = head;
    if (++d->report_n >= DSHM_REPORT_MAX) {
        warn_report_once("display-shm: a report reached %d events and was "
                         "ended", DSHM_REPORT_MAX);
        dshm_input_end_report(d);
    }
}

static void keys_note(DShmViewer *v, unsigned code, bool down,
                      unsigned head)
{
    v->key_head[code] = head;
    if (down) {
        v->keys_down[code / 8] |= 1 << (code % 8);
    } else {
        v->keys_down[code / 8] &= ~(1 << (code % 8));
    }
}

static void key_apply(DShmViewer *v, DShmHead *h, unsigned code, bool down)
{
    DisplayShm *d = v->d;

    rel_flush(d);
    keys_note(v, code, down, h->idx);
    dshm_inject_key(h, code, down);
    report_count(d, h->idx);
}

static void button_apply(DShmViewer *v, DShmHead *h, unsigned button,
                         bool down)
{
    DisplayShm *d = v->d;

    /* Each button change is a report of its own. */
    dshm_input_end_report(d);
    if (is_wheel(button)) {
        budget_refill(d);
        if (down && d->wheel_used >= WHEEL_BUDGET) {
            v->wheel_dropped |= 1u << button;
            return;
        }
        /* a press that was dropped takes its release with it */
        if (!down && (v->wheel_dropped & (1u << button))) {
            v->wheel_dropped &= ~(1u << button);
            return;
        }
        if (down) {
            v->wheel_dropped &= ~(1u << button);
        }
        d->wheel_used += down;
    }
    v->button_head[button] = h->idx;
    if (down) {
        v->buttons_down |= 1ull << button;
    } else {
        v->buttons_down &= ~(1ull << button);
    }
    dshm_inject_button(h, button, down);
    d->report_n = 1;
    d->report_head = h->idx;
    dshm_input_end_report(d);
}

/* A key or button the steering queue held back, now let go. */
void dshm_input_apply_deferred(DShmViewer *v, unsigned head, bool key,
                               unsigned code, bool down)
{
    DShmHead *h = dshm_head(v->d, head);

    if (!h || !h->present) {
        return;
    }
    if (key) {
        key_apply(v, h, code, down);
    } else {
        button_apply(v, h, code, down);
    }
}

static void held_release(DShmViewer *v)
{
    DisplayShm *d = v->d;

    v->wheel_dropped = 0;

    for (unsigned code = 1; code <= KEY_MAX; code++) {
        DShmHead *h = dshm_head(d, v->key_head[code]);

        if ((v->keys_down[code / 8] & (1 << (code % 8))) && h && h->present) {
            dshm_inject_key(h, code, false);
            report_count(d, h->idx);
        }
    }
    for (unsigned b = 0; b < BUTTONS; b++) {
        DShmHead *h = dshm_head(d, v->button_head[b]);

        if ((v->buttons_down & (1ull << b)) && h && h->present) {
            dshm_input_end_report(d);
            dshm_inject_button(h, b, false);
            d->report_n = 1;
            d->report_head = h->idx;
        }
    }
    memset(v->keys_down, 0, sizeof(v->keys_down));
    v->buttons_down = 0;
    dshm_input_end_report(d);
}

/* Owner change, reset or disconnect: nothing the viewer started goes on. */
static void owner_release(DShmViewer *v)
{
    dshm_steer_cancel(v->d, true);
    dshm_input_end_report(v->d);
    held_release(v);
}

static void state_broadcast(DisplayShm *d)
{
    for (unsigned i = 0; i < d->n_viewers; i++) {
        if (d->viewers[i]->ready) {
            dshm_input_ack_state(d->viewers[i]);
        }
    }
}

static void claim(DShmViewer *v, bool take)
{
    DisplayShm *d = v->d;

    if (take && (v->features & DSHM_FEAT_INPUT)) {
        if (d->owner == v) {
            return;
        }
        if (d->owner) {
            owner_release(d->owner);
        }
        d->owner = v;
    } else if (!take && d->owner == v) {
        owner_release(v);
        d->owner = NULL;
    } else {
        return;
    }
    state_broadcast(d);
}

void dshm_input_claim_on_join(DShmViewer *v)
{
    if ((v->features & DSHM_FEAT_INPUT) && !v->d->owner) {
        v->d->owner = v;
    }
}

void dshm_input_ack_state(DShmViewer *v)
{
    DShmInputState m = { .hdr = { .type = DSHM_MSG_INPUT_STATE },
                         .flags = v->d->owner == v,
                         .leds = v->d->leds };

    dshm_queue(v, &m, sizeof(m), DSHM_NO_HANDLE, false);
    dshm_flush(v);
}

void dshm_input_release(DShmViewer *v)
{
    if (v->d->owner == v) {
        owner_release(v);
        v->d->owner = NULL;
        state_broadcast(v->d);
    }
}

/* ---- Messages --------------------------------------------------------- */

static DShmHead *event_head(DShmViewer *v, uint32_t head, const char *what)
{
    DShmHead *h = dshm_head(v->d, head);

    if (!h || !h->present || !h->width) {
        warn_report_once("display-shm: %s names head %u, which does not "
                         "exist; dropped", what, head);
        return NULL;
    }
    return h;
}

static void do_rel(DShmViewer *v, DShmHead *h, int32_t x, int32_t y)
{
    DisplayShm *d = v->d;

    /* Motion that follows a steered move is the user's: the move ends. */
    dshm_steer_cancel(d, false);
    if (d->acc_on && d->acc_head != h->idx) {
        rel_flush(d);
    }
    d->acc_on = true;
    d->acc_head = h->idx;
    d->acc_x += x;
    d->acc_y += y;
    report_count(d, h->idx);
}

static void do_abs(DShmViewer *v, DShmHead *h, int32_t x, int32_t y)
{
    DisplayShm *d = v->d;

    switch (dshm_head_pointer_mode(h, v)) {
    case DSHM_POINTER_ABSOLUTE:
        dshm_steer_cancel(d, false);
        rel_flush(d);
        dshm_inject_abs(h, x, y);
        report_count(d, h->idx);
        break;
    case DSHM_POINTER_STEERED:
        rel_flush(d);
        dshm_steer_abs(v, h, x, y);
        break;
    default:
        warn_report_once("display-shm: absolute input for a relative "
                         "pointer; dropped");
        break;
    }
}

void dshm_input_message(DShmViewer *v, const DShmHdr *hdr, const uint8_t *msg)
{
    DisplayShm *d = v->d;
    bool owner = d->owner == v;

    switch (hdr->type) {
    case DSHM_MSG_INPUT_CLAIM: {
        DShmInputClaim m;

        memcpy(&m, msg, sizeof(m));
        claim(v, m.flags & 1);
        return;
    }
    case DSHM_MSG_INPUT_RESET:
        if (owner) {
            owner_release(v);
        }
        return;
    case DSHM_MSG_SYNC: {
        DShmSync m;

        memcpy(&m, msg, sizeof(m));
        if (owner) {
            dshm_input_end_report(d);
        }
        if (m.flags & 1) {
            DShmInputAck a = { .hdr = { .type = DSHM_MSG_INPUT_ACK },
                               .serial = m.serial };

            dshm_queue(v, &a, sizeof(a), DSHM_NO_HANDLE, false);
            dshm_flush(v);
        }
        return;
    }
    case DSHM_MSG_KEY: {
        DShmKey m;
        DShmHead *h;

        memcpy(&m, msg, sizeof(m));
        h = owner ? event_head(v, m.head, "KEY") : NULL;
        if (!h) {
            return;
        }
        if (m.down > 1 || m.code < 1 || m.code > KEY_MAX) {
            warn_report_once("display-shm: a key outside 1..0x%x or with "
                             "down > 1; dropped", KEY_MAX);
            return;
        }
        if (!dshm_steer_defer(v, m.head, true, m.code, m.down)) {
            key_apply(v, h, m.code, m.down);
        }
        return;
    }
    case DSHM_MSG_BUTTON: {
        DShmKey m;
        DShmHead *h;

        memcpy(&m, msg, sizeof(m));
        h = owner ? event_head(v, m.head, "BUTTON") : NULL;
        if (!h) {
            return;
        }
        if (m.down > 1 || m.code >= BUTTONS) {
            warn_report_once("display-shm: a button outside 0..%zu or with "
                             "down > 1; dropped", BUTTONS - 1);
            return;
        }
        if (!dshm_steer_defer(v, m.head, false, m.code, m.down)) {
            button_apply(v, h, m.code, m.down);
        }
        return;
    }
    case DSHM_MSG_REL: {
        DShmMotion m;
        DShmHead *h;

        memcpy(&m, msg, sizeof(m));
        h = owner ? event_head(v, m.head, "REL") : NULL;
        if (h) {
            do_rel(v, h, m.x, m.y);
        }
        return;
    }
    case DSHM_MSG_ABS: {
        DShmMotion m;
        DShmHead *h;

        memcpy(&m, msg, sizeof(m));
        h = owner ? event_head(v, m.head, "ABS") : NULL;
        if (h) {
            do_abs(v, h, m.x, m.y);
        }
        return;
    }
    default:
        if (hdr->flags & DSHM_F_MUST) {
            dshm_viewer_close(v, DSHM_ERR_UNSUPPORTED, "unsupported message");
        }
        return;
    }
}
