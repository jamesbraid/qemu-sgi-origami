/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * qemu.test, version 1: a picture source that is a pure function of a
 * stimulus, controls that make internal states reproducible, and the input
 * and statistics a conformance suite reads. Offered only by a producer
 * started with x-test=on, and only to a viewer that sets the TEST feature.
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qemu/thread.h"
#include "ui/display-shm-priv.h"

#define TEST_W 128
#define TEST_H 64
#define HOLDS_MAX 16

typedef struct TestHead {
    DShmHead *h;
    uint32_t pattern;
    uint64_t seed;
    uint32_t step;
    int mark_delay_us;          /* op 4: mark from a second thread */
} TestHead;

typedef struct TestHold {
    uint32_t sender, target;    /* viewer ids */
    uint32_t op;                /* 1 sample hold, 5 write hold */
    unsigned head;
} TestHold;

typedef struct TestState {
    TestHold holds[HOLDS_MAX];
    unsigned n_holds;
} TestState;

/* ---- The pictures ------------------------------------------------------ */

static uint64_t fnv(uint64_t seed, uint32_t y, uint32_t s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    uint8_t b[16];

    stq_le_p(b, seed);
    stl_le_p(b + 8, y);
    stl_le_p(b + 12, s);
    for (unsigned i = 0; i < sizeof(b); i++) {
        h = (h ^ b[i]) * 0x100000001b3ull;
    }
    return h;
}

static uint32_t pixel(const TestHead *t, unsigned y)
{
    unsigned hgt = t->h->height;

    switch (t->pattern) {
    case 1:
        return (t->seed * 2654435761ull + t->step) & 0xffffff;
    case 2:
        return ((y + (uint64_t)t->step * 8) % hgt) < 8 ? 0xffffff : 0;
    case 3: {
        int64_t s = (int64_t)t->step - (((int64_t)t->step - (y % 7)) % 7 +
                                        7) % 7;

        /* the largest s <= step with s mod 7 == y mod 7 */
        if (s < 0) {
            return 0;
        }
        return fnv(t->seed, y, s) & 0xffffff;
    }
    default:
        return 0;
    }
}

static void test_resolve(void *opaque, unsigned y, uint32_t *dst)
{
    TestHead *t = opaque;
    uint32_t p = pixel(t, y);

    for (unsigned x = 0; x < t->h->width; x++) {
        dst[x] = p;
    }
    if (y == 0) {
        for (unsigned i = 0; i < 8 && i < t->h->width; i++) {
            dst[i] = (t->step >> (4 * i)) & 0xf;
        }
    }
}

static void test_sample(void *opaque)
{
    TestHead *t = opaque;

    dshm_head_sample_marks(t->h, test_resolve, t);
}

static const DShmSourceOps test_ops = { .sample = test_sample };

static TestHead *test_head_new(DisplayShm *d, uint32_t w, uint32_t hgt)
{
    TestHead *t = g_new0(TestHead, 1);

    t->pattern = 1;
    t->h = dshm_head_new(d, NULL, &test_ops, t, "test");
    t->h->test = t;
    dshm_head_set_steerable(t->h, true);
    dshm_head_set_mode_owned(t->h, w, hgt, 60000);
    dshm_head_announce(t->h);
    return t;
}

void dshm_test_init(DisplayShm *d)
{
    if (d->x_test) {
        d->test = g_new0(TestState, 1);
    }
}

void dshm_test_head_add(DisplayShm *d)
{
    if (d->x_test) {
        test_head_new(d, TEST_W, TEST_H);
    }
}

/* ---- Holds ------------------------------------------------------------- */

static void hold_add(DisplayShm *d, uint32_t sender, uint32_t target,
                     uint32_t op, unsigned head)
{
    TestState *ts = d->test;

    if (ts->n_holds < HOLDS_MAX) {
        ts->holds[ts->n_holds++] = (TestHold) {
            .sender = sender, .target = target, .op = op, .head = head,
        };
    }
}

static void hold_release(DisplayShm *d, uint32_t op, uint32_t target,
                         unsigned head)
{
    TestState *ts = d->test;

    for (unsigned i = 0; i < ts->n_holds;) {
        if (ts->holds[i].op == op && ts->holds[i].target == target &&
            (op == 5 || ts->holds[i].head == head)) {
            ts->holds[i] = ts->holds[--ts->n_holds];
        } else {
            i++;
        }
    }
}

/* All holds end when the sender or the target goes. */
void dshm_test_viewer_gone(DShmViewer *v)
{
    DisplayShm *d = v->d;
    TestState *ts = d->test;

    if (!ts) {
        return;
    }
    for (unsigned i = 0; i < ts->n_holds;) {
        TestHold hold = ts->holds[i];

        if (hold.sender != v->id && hold.target != v->id) {
            i++;
            continue;
        }
        ts->holds[i] = ts->holds[--ts->n_holds];
        if (hold.op == 5) {
            DShmViewer *t = dshm_viewer_by_id(d, hold.target);

            if (t && t != v) {
                t->hold_writes = false;
                dshm_flush(t);
            }
        } else {
            DShmHead *h = dshm_head(d, hold.head);

            if (h) {
                h->hold_armed = false;
                dshm_head_hold_release(h);
            }
        }
    }
}

/* ---- Stimulus ---------------------------------------------------------- */

static void *mark_thread(void *opaque)
{
    TestHead *t = opaque;
    int delay = t->mark_delay_us;

    /* The bits, a pause, then the flag: a race a single thread cannot show. */
    dshm_head_mark_bits(t->h, 0, t->h->height);
    g_usleep(delay);
    dshm_head_marked(t->h);
    return NULL;
}

static void mark_all(TestHead *t)
{
    if (t->mark_delay_us) {
        QemuThread th;

        qemu_thread_create(&th, "dshm-mark", mark_thread, t,
                           QEMU_THREAD_DETACHED);
        t->mark_delay_us = 0;
    } else {
        dshm_head_mark(t->h, 0, t->h->height);
    }
}

static void stimulus(DShmViewer *v, const DShmStimulus *m)
{
    DisplayShm *d = v->d;
    DShmHead *h = dshm_head(d, m->head);
    TestHead *t = h ? h->test : NULL;

    if (m->kind == 5) {
        if (m->arg) {
            test_head_new(d, TEST_W, TEST_H);
        } else if (h && h->test) {
            dshm_head_remove(h);
            g_free(h->test);
            h->test = NULL;
        }
        return;
    }
    if (!t || !h->present) {
        warn_report_once("display-shm: a stimulus names head %u, which is "
                         "not a test head; dropped", m->head);
        return;
    }
    switch (m->kind) {
    case 1:
        t->pattern = m->arg;
        t->seed = m->seed;
        t->step = m->step;
        mark_all(t);
        break;
    case 2:
        dshm_head_cursor_move(h, m->seed & 0xffff, (m->seed >> 16) & 0xffff,
                              m->arg != 0);
        break;
    case 3: {
        uint32_t n = MIN(MAX(m->arg, 1u), (uint32_t)DSHM_CURSOR_MAX);
        QEMUCursor *c = cursor_alloc(n, n);

        for (uint32_t i = 0; i < n * n; i++) {
            c->data[i] = m->seed;
        }
        dshm_head_cursor_define(h, c);
        cursor_unref(c);
        break;
    }
    case 4: {
        uint32_t w = m->seed & 0xffff, hgt = (m->seed >> 16) & 0xffff;

        t->pattern = 1;
        t->seed = 0;
        t->step = 0;
        dshm_head_set_mode_owned(h, w, hgt, 60000);
        break;
    }
    default:
        break;
    }
}

/* ---- Controls ---------------------------------------------------------- */

static void control_done(DShmViewer *v, uint32_t op, uint32_t result)
{
    DShmControlDone m = {
        .hdr = { .type = DSHM_EXT_TYPE(v->test_idx,
                                       DSHM_TEST_CONTROL_DONE) },
        .op = op, .result = result,
    };

    /* Urgent: a held connection must still hear that it is held. */
    dshm_queue_ext(v, &m, sizeof(m), true);
    dshm_flush(v);
}

static void control(DShmViewer *v, const DShmControl *m)
{
    DisplayShm *d = v->d;
    DShmViewer *target = m->target_viewer ?
        dshm_viewer_by_id(d, m->target_viewer) : v;
    DShmHead *h = dshm_head(d, m->head);
    uint32_t result = 1;

    switch (m->op) {
    case 1:
        if (h && h->present) {
            h->hold_armed = true;
            hold_add(d, v->id, v->id, 1, h->idx);
            result = 0;
        }
        break;
    case 2:
        /* nothing held, nothing to act on */
        if (h && (h->hold_armed || h->held)) {
            hold_release(d, 1, v->id, h->idx);
            for (unsigned i = 0; i < d->n_viewers; i++) {
                hold_release(d, 1, d->viewers[i]->id, h->idx);
            }
            h->hold_armed = false;
            if (h->held) {
                dshm_head_hold_release(h);
            }
            result = 0;
        }
        break;
    case 3:
        if (target && h && h->idx < target->n_vh && target->vh[h->idx]) {
            result = dshm_vh_retire_queued(target->vh[h->idx]) ? 0 : 1;
            dshm_viewer_eval_flagged(target);
        }
        break;
    case 4:
        if (h && h->test) {
            ((TestHead *)h->test)->mark_delay_us = MAX(m->arg, 1u);
            result = 0;
        }
        break;
    case 5:
        if (target) {
            target->hold_writes = true;
            hold_add(d, v->id, target->id, 5, 0);
            result = 0;
        }
        break;
    case 6:
        if (target) {
            target->hold_writes = false;
            hold_release(d, 5, target->id, 0);
            dshm_flush(target);
            result = 0;
        }
        break;
    default:
        break;
    }
    control_done(v, m->op, result);
}

void dshm_test_message(DShmViewer *v, unsigned local, const uint8_t *msg,
                       size_t len)
{
    switch (local) {
    case DSHM_TEST_STIMULUS:
        if (len >= sizeof(DShmStimulus)) {
            DShmStimulus m;

            memcpy(&m, msg, sizeof(m));
            stimulus(v, &m);
            return;
        }
        break;
    case DSHM_TEST_CONTROL:
        if (len >= sizeof(DShmControl)) {
            DShmControl m;

            memcpy(&m, msg, sizeof(m));
            control(v, &m);
            return;
        }
        break;
    case DSHM_TEST_STATS_REQUEST: {
        DShmStats s;

        dshm_stats_fill(v->d, v, &s);
        dshm_queue(v, &s, sizeof(s), DSHM_NO_HANDLE, false);
        dshm_flush(v);
        return;
    }
    default:
        return;
    }
    dshm_viewer_close(v, DSHM_ERR_PROTOCOL, "short test message");
}
