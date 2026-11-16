/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The display-shm producer (docs/interop/display-shm.rst): heads, slot sets,
 * sampling and flow control. Main loop only, except dshm_head_marked().
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/atomic.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "qemu/guest-random.h"
#include "system/runstate.h"
#include "system/system.h"
#include "ui/input.h"
#include "ui/display-shm-priv.h"

static DisplayShm *the_display;
/* Console indices are scanned up to here, gaps included. */
#define CONSOLE_SCAN 256

bool dshm_producer_present;

int64_t dshm_now_ns(void)
{
    return get_clock();
}

DShmHead *dshm_head(DisplayShm *d, unsigned idx)
{
    return idx < d->n_heads ? d->heads[idx] : NULL;
}

static DShmStateHead *state_head(DisplayShm *d, unsigned idx)
{
    return (DShmStateHead *)(d->state->ptr + sizeof(DShmStateHeader) +
                             (size_t)idx * sizeof(DShmStateHead));
}

static uint32_t stride_for(uint32_t width)
{
    return ROUND_UP(width * 4, DSHM_STRIDE_ALIGN);
}

/* ---- Slots and sets --------------------------------------------------- */

static void slot_free(DShmHead *h, DShmSlot *s)
{
    DisplayShm *d = h->d;

    for (unsigned i = 0; i < d->n_viewers; i++) {
        dshm_region_forget(d->viewers[i], s->r);
    }
    dshm_region_unref(d, s->r);
    g_free(s->stale);
    g_free(s);
}

static DShmSlot *slot_new(DShmHead *h, DShmSet *set)
{
    DShmSlot *s;
    DShmRegion *r;
    Error *err = NULL;

    r = dshm_region_new(h->d, DSHM_KIND_SLOT, set->slot_bytes,
                        set->writable, &err);
    if (!r) {
        error_report_err(err);
        return NULL;
    }
    s = g_new0(DShmSlot, 1);
    s->r = r;
    s->set = set;
    s->stale = bitmap_new(set->height);
    bitmap_fill(s->stale, set->height);
    return s;
}

static void set_destroy(DShmSet *set)
{
    DShmHead *h = set->h;

    for (unsigned i = 0; i < set->n; i++) {
        slot_free(h, set->slot[i]);
    }
    QTAILQ_REMOVE(&h->sets, set, next);
    g_free(set);
}

static bool set_idle(const DShmSet *set)
{
    if (set->viewers) {
        return false;
    }
    for (unsigned i = 0; i < set->n; i++) {
        if (set->slot[i]->lent) {
            return false;
        }
    }
    return true;
}

static void set_maybe_free(DShmSet *set)
{
    if (set && set_idle(set)) {
        set_destroy(set);
    }
}

static unsigned set_want(const DShmSet *set)
{
    return MIN(MAX(set->credits + 1, 2u), DSHM_MAX_SLOTS);
}

static void vh_send_head(DShmVH *vh);

static DShmVH *viewer_vh(DShmViewer *v, DShmHead *h)
{
    return !v->closing && h->idx < v->n_vh ? v->vh[h->idx] : NULL;
}

/* Tell every viewer on the set about a change of its slot list. */
static void set_announce(DShmSet *set, const DShmVH *skip)
{
    DisplayShm *d = set->h->d;

    if (!set->current) {
        return;
    }
    for (unsigned i = 0; i < d->n_viewers; i++) {
        DShmVH *vh = viewer_vh(d->viewers[i], set->h);

        if (vh && vh != skip && vh->set == set && d->viewers[i]->ready) {
            vh_send_head(vh);
        }
    }
}

/* Grow or shrink to 1 plus the credits. False if memory ran out. */
static bool set_fit(DShmSet *set, const DShmVH *skip, bool announce)
{
    unsigned want = set_want(set);
    bool ok = true, changed = set->list_changed;

    set->list_changed = false;
    while (set->n < want) {
        DShmSlot *s = slot_new(set->h, set);

        if (!s) {
            ok = false;
            break;
        }
        set->slot[set->n++] = s;
        changed = true;
    }
    for (unsigned i = set->n; i-- > 0 && set->n > want;) {
        if (!set->slot[i]->lent) {
            DShmSlot *s = set->slot[i];

            memmove(&set->slot[i], &set->slot[i + 1],
                    (set->n - i - 1) * sizeof(set->slot[0]));
            set->n--;
            slot_free(set->h, s);
            changed = true;
        }
    }
    if (changed && announce) {
        set_announce(set, skip);
    }
    return ok;
}

/* Take a slot out of the set for good: nothing will write it again. */
static void slot_retire(DShmHead *h, DShmSlot *s)
{
    DShmSet *set = s->set;

    if (!set) {
        return;
    }
    for (unsigned i = 0; i < set->n; i++) {
        if (set->slot[i] == s) {
            memmove(&set->slot[i], &set->slot[i + 1],
                    (set->n - i - 1) * sizeof(set->slot[0]));
            set->n--;
            set->list_changed = true;
            break;
        }
    }
    s->set = NULL;
    s->retired = true;
    if (!s->lent) {
        slot_free(h, s);
    }
}

/* ---- Leases ----------------------------------------------------------- */

static int vh_free_index(const DShmVH *vh)
{
    for (unsigned i = 0; i < vh->credit && i < DSHM_MAX_LEASES; i++) {
        if (!vh->lease[i].used) {
            return i;
        }
    }
    return -1;
}

static void lease_free(DShmVH *vh, unsigned i)
{
    DShmSlot *s = vh->lease[i].slot;
    DShmSet *set = s->set;

    vh->lease[i].used = false;
    vh->lease[i].slot = NULL;
    assert(s->lent);
    s->lent--;
    if (s->retired && !s->lent) {
        slot_free(vh->h, s);
    } else {
        set_maybe_free(set);
    }
}

/* Read the viewer's release table: what it has finished with comes free. */
static void vh_refresh_leases(DShmVH *vh)
{
    DShmViewer *v = vh->v;
    DShmFeedbackHead *fb;

    if (v->closing || !v->feedback || vh->h->idx >= DSHM_FEEDBACK_HEADS) {
        return;
    }
    fb = (DShmFeedbackHead *)((uint8_t *)v->feedback->ptr +
                              sizeof(DShmFeedbackHeader)) + vh->h->idx;
    for (unsigned i = 0; i < DSHM_MAX_LEASES; i++) {
        uint64_t r;

        if (!vh->lease[i].used || !vh->lease[i].started) {
            continue;
        }
        r = qatomic_load_acquire(&fb->released[i]);
        if (r >= vh->lease[i].seq) {
            if (r > vh->lease[i].seq && !vh->over_logged[i]) {
                vh->over_logged[i] = true;
                warn_report("display-shm: viewer %s released lease %u past "
                            "what it was lent", v->name, i);
            }
            lease_free(vh, i);
        }
    }
}

static void head_refresh_leases(DShmHead *h)
{
    DisplayShm *d = h->d;

    for (unsigned i = 0; i < d->n_viewers; i++) {
        DShmVH *vh = viewer_vh(d->viewers[i], h);

        if (vh) {
            vh_refresh_leases(vh);
        }
    }
}

/* ---- HEAD ------------------------------------------------------------- */

uint32_t dshm_head_pointer_mode(DShmHead *h, DShmViewer *v)
{
    if (h->con && qemu_input_is_absolute(h->con)) {
        return DSHM_POINTER_ABSOLUTE;
    }
    if (v->ext_steer && h->steerable) {
        return DSHM_POINTER_STEERED;
    }
    return DSHM_POINTER_RELATIVE;
}

static void vh_build_head(DShmVH *vh, DShmHeadMsg *m)
{
    DShmHead *h = vh->h;
    DShmSet *set = vh->set;
    bool shows = h->present && h->width && h->height && !vh->nothing;

    memset(m, 0, sizeof(*m));
    m->hdr.type = DSHM_MSG_HEAD;
    m->hdr.flags = DSHM_F_MUST;
    m->head = h->idx;
    m->flags = (h->present ? DSHM_HEAD_PRESENT : 0) |
               (vh->too_large ? DSHM_HEAD_TOO_LARGE : 0) |
               (h->unsupported ? DSHM_HEAD_UNSUPPORTED : 0);
    m->mode_serial = vh->mode_serial;
    m->source_index = h->con ? qemu_console_get_index(h->con) : h->idx;
    if (shows) {
        m->width = h->width;
        m->height = h->height;
        m->refresh_mhz = h->mhz;
        m->fourcc = DSHM_FOURCC_XR24;
        m->stride = stride_for(h->width);
    }
    if (shows && set) {
        m->n_slots = set->n;
        for (unsigned i = 0; i < set->n; i++) {
            m->slot_region[i] = set->slot[i]->r->id;
        }
    }
    m->pointer_mode = dshm_head_pointer_mode(h, vh->v);
    m->credit = vh->credit;
    memcpy(m->label, h->label, sizeof(m->label));
}

/* The regions of the set first, then the HEAD that lists them. */
static void vh_send_head(DShmVH *vh)
{
    DShmHeadMsg m;
    DShmSet *set = vh->set;

    if (set && !vh->nothing) {
        for (unsigned i = 0; i < set->n; i++) {
            DShmRegion *r = set->slot[i]->r;

            if (!g_hash_table_contains(vh->v->announced,
                                       GUINT_TO_POINTER(r->id))) {
                dshm_region_announce(vh->v, r, r->access);
            }
        }
    }
    vh_build_head(vh, &m);
    dshm_queue(vh->v, &m, sizeof(m), -1, false);
    dshm_flush(vh->v);
}

void dshm_head_send_to(DShmViewer *v, DShmHead *h)
{
    DShmVH *vh = viewer_vh(v, h);

    if (vh) {
        vh_send_head(vh);
    }
}

/* ---- Joining and leaving sets ----------------------------------------- */

static DShmSet *head_find_set(DShmHead *h, bool writable, unsigned credit)
{
    DShmSet *set;

    QTAILQ_FOREACH(set, &h->sets, next) {
        if (set->current && set->writable == writable &&
            set->credits + credit + 1 <= DSHM_MAX_SLOTS) {
            return set;
        }
    }
    return NULL;
}

static unsigned head_live_sets(DShmHead *h)
{
    DShmSet *set;
    unsigned n = 0;

    QTAILQ_FOREACH(set, &h->sets, next) {
        n += set->viewers != 0;
    }
    return n;
}

static DShmSet *set_new(DShmHead *h, bool writable)
{
    DShmSet *set = g_new0(DShmSet, 1);

    set->h = h;
    set->writable = writable;
    set->current = true;
    set->width = h->width;
    set->height = h->height;
    set->stride = stride_for(h->width);
    set->slot_bytes = (size_t)set->stride * h->height;
    QTAILQ_INSERT_TAIL(&h->sets, set, next);
    return set;
}

static void vh_leave_set(DShmVH *vh)
{
    DShmSet *set = vh->set;

    if (!set) {
        return;
    }
    vh->set = NULL;
    set->viewers--;
    set->credits -= vh->credit;
    if (set->viewers && set->current) {
        set_fit(set, NULL, true);
    } else {
        set_maybe_free(set);
    }
}

/* Join (or create) the set for the head's current mode. */
static bool vh_join_set(DShmVH *vh)
{
    DShmHead *h = vh->h;
    DShmViewer *v = vh->v;
    bool writable = v->features & DSHM_FEAT_RW_PIXELS;
    unsigned credit = v->credit;
    DShmSet *set;

    vh->nothing = false;
    vh->too_large = false;
    if (!h->present || !h->width || !h->height || !h->ref) {
        return true;
    }
    if (h->width > v->max_width || h->height > v->max_height) {
        vh->too_large = true;
        return true;
    }
    if ((uint64_t)stride_for(h->width) * h->height > h->d->max_head_bytes) {
        vh->too_large = true;
        return true;
    }
    set = head_find_set(h, writable, credit);
    if (!set && head_live_sets(h) < h->d->max_sets) {
        set = set_new(h, writable);
    }
    if (!set && writable) {
        /* Over the bound: the core read-only set, if it fits. */
        set = head_find_set(h, false, credit);
        if (!set) {
            set = set_new(h, false);
        }
    }
    if (!set) {
        return false;
    }
    vh->credit = MIN(credit, DSHM_MAX_SLOTS - 1 - set->credits);
    if (!vh->credit) {
        return false;
    }
    vh->set = set;
    set->viewers++;
    set->credits += vh->credit;
    if (!set_fit(set, vh, true) || set->n < 2) {
        vh->set = NULL;
        set->viewers--;
        set->credits -= vh->credit;
        set_maybe_free(set);
        vh->nothing = true;
        return true;
    }
    return true;
}

static void vh_reset_damage(DShmVH *vh)
{
    g_free(vh->damage);
    vh->damage = bitmap_new(MAX(vh->h->height, 1));
    vh->damage_all = true;
}

/* Cancel the viewer's one unsent FRAME, if any, undoing it. */
static void vh_drop_queued(DShmVH *vh)
{
    DShmOut *o = vh->queued;

    if (o) {
        dshm_frame_cancel(vh->v, o);
        dshm_out_remove(vh->v, o);
    }
}

/* Put the viewer on a set for the head's mode now; send what it needs. */
static void vh_rehome(DShmVH *vh, bool new_mode)
{
    DShmViewer *v = vh->v;

    vh_drop_queued(vh);
    vh_leave_set(vh);
    vh->set_gen++;
    vh->reset = true;
    if (new_mode) {
        vh->mode_serial++;
        vh_reset_damage(vh);
    } else {
        vh->damage_all = true;
    }
    if (!vh_join_set(vh)) {
        dshm_viewer_doom(v, DSHM_ERR_LIMIT, "no slot set within limits");
        return;
    }
    vh_send_head(vh);
    /* A request recorded across the re-home is due again. */
    dshm_vh_eval(vh);
}

static DShmVH *vh_new(DShmViewer *v, DShmHead *h)
{
    DShmVH *vh = g_new0(DShmVH, 1);

    vh->v = v;
    vh->h = h;
    vh->credit = v->credit;
    vh->next_seq = 1;
    vh->mode_serial = 1;
    vh->reset = true;
    vh_reset_damage(vh);
    return vh;
}

static void vh_free(DShmVH *vh)
{
    g_free(vh->damage);
    g_free(vh);
}

static void viewer_attach_head(DShmViewer *v, DShmHead *h)
{
    DShmVH *vh;

    if (h->idx >= v->n_vh) {
        v->vh = g_renew(DShmVH *, v->vh, h->idx + 1);
        memset(v->vh + v->n_vh, 0, (h->idx + 1 - v->n_vh) * sizeof(*v->vh));
        v->n_vh = h->idx + 1;
    }
    vh = vh_new(v, h);
    v->vh[h->idx] = vh;
    if (!vh_join_set(vh)) {
        dshm_viewer_doom(v, DSHM_ERR_LIMIT, "no slot set within limits");
        return;
    }
    vh_send_head(vh);
}

static void send_cursor_image(DShmViewer *v, DShmHead *h);

void dshm_viewer_join(DShmViewer *v)
{
    DisplayShm *d = v->d;

    dshm_rescan(d);
    v->vh = g_new0(DShmVH *, d->head_capacity);
    v->n_vh = d->head_capacity;
    for (unsigned i = 0; i < d->n_heads && !v->closing; i++) {
        DShmHead *h = d->heads[i];

        if (h && h->present) {
            viewer_attach_head(v, h);
        }
    }
    for (unsigned i = 0; i < d->n_heads && !v->closing; i++) {
        if (d->heads[i] && d->heads[i]->present &&
            d->heads[i]->cursor_image_serial) {
            send_cursor_image(v, d->heads[i]);
        }
    }
    if (v->closing) {
        return;
    }
    dshm_input_claim_on_join(v);
    dshm_input_ack_state(v);
    {
        DShmHdr ready = { .type = DSHM_MSG_READY, .flags = DSHM_F_MUST };

        dshm_queue(v, &ready, sizeof(ready), -1, false);
    }
    v->ready = true;
    dshm_flush(v);
}

void dshm_viewer_leave(DShmViewer *v)
{
    DisplayShm *d = v->d;

    dshm_input_release(v);
    dshm_test_viewer_gone(v);
    for (unsigned i = 0; i < v->n_vh; i++) {
        DShmVH *vh = v->vh[i];

        if (!vh) {
            continue;
        }
        /* Slots sent may still be read by the viewer: retire them. */
        for (unsigned l = 0; l < DSHM_MAX_LEASES; l++) {
            if (vh->lease[l].used) {
                DShmSlot *s = vh->lease[l].slot;

                if (vh->lease[l].started) {
                    slot_retire(vh->h, s);
                    /* slot_retire may free it; the lease holds it alive */
                }
            }
        }
        for (unsigned l = 0; l < DSHM_MAX_LEASES; l++) {
            if (vh->lease[l].used) {
                lease_free(vh, l);
            }
        }
        vh_leave_set(vh);
        vh_free(vh);
        v->vh[i] = NULL;
    }
    if (v->feedback) {
        dshm_region_unref(d, v->feedback);
        v->feedback = NULL;
    }
    dshm_eval_flagged_all(d);
}

/* ---- Sampling --------------------------------------------------------- */

bool dshm_head_demand(DShmHead *h)
{
    DisplayShm *d = h->d;

    for (unsigned i = 0; i < d->n_viewers; i++) {
        DShmVH *vh = viewer_vh(d->viewers[i], h);

        if (vh && vh->req.frame && vh->set && d->viewers[i]->ready &&
            (vh_free_index(vh) >= 0 || vh->queued)) {
            return true;
        }
    }
    return false;
}

static void state_frame_write(DShmHead *h)
{
    DShmStateHead *sh = state_head(h->d, h->idx);
    uint32_t seq = sh->frame_seq;

    qatomic_set(&sh->frame_seq, seq + 1);
    /* the odd count is visible before any field changes */
    smp_wmb();
    qatomic_set(&sh->frame_serial, h->n);
    qatomic_set(&sh->sample_ns, h->sample_ns);
    qatomic_set(&sh->publish_ns, h->publish_ns);
    qatomic_set(&sh->guest_ns, h->guest_ns);
    qatomic_set(&sh->skipped, h->skipped);
    /* the fields are written before the count turns even */
    smp_wmb();
    qatomic_set(&sh->frame_seq, seq + 2);
}

static void head_eval_all(DShmHead *h)
{
    DisplayShm *d = h->d;

    for (unsigned i = 0; i < d->n_viewers; i++) {
        DShmVH *vh = viewer_vh(d->viewers[i], h);

        if (vh && !d->viewers[i]->closing) {
            dshm_vh_eval(vh);
        }
    }
}

static void finish_sample(DShmHead *h)
{
    DisplayShm *d = h->d;
    bool first = h->need_first && h->ref;

    if (h->in_sample) {
        h->completed_seq = h->sample_seq;
    }
    if (first) {
        bitmap_fill(h->changed, h->height);
        h->changed_any = true;
        h->need_first = false;
        h->ref_valid = true;
    }
    if (h->changed_any) {
        DShmSet *set;

        h->n++;
        h->sample_ns = h->cur_ns;
        h->publish_ns = dshm_now_ns();
        h->guest_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        d->stats.publishes++;
        for (unsigned i = 0; i < d->n_viewers; i++) {
            DShmVH *vh = viewer_vh(d->viewers[i], h);

            if (!vh) {
                continue;
            }
            if (first) {
                vh->damage_all = true;
            }
            bitmap_or(vh->damage, vh->damage, h->changed, h->height);
        }
        QTAILQ_FOREACH(set, &h->sets, next) {
            if (!set->current) {
                continue;
            }
            for (unsigned i = 0; i < set->n; i++) {
                bitmap_or(set->slot[i]->stale, set->slot[i]->stale,
                          h->changed, set->height);
            }
        }
        bitmap_zero(h->changed, h->height);
        h->changed_any = false;
        state_frame_write(h);
    }
    head_eval_all(h);
}

void dshm_head_sample_start(DShmHead *h)
{
    if (!h) {
        return;
    }
    if (!h->d) {
        return;
    }
    qatomic_set(&h->unsampled, false);
    /* the flag clears before the marks are read */
    smp_mb();
    if (!h->sample_open) {
        h->sample_open = true;
        if (!h->in_sample) {
            h->cur_ns = dshm_now_ns();
            h->d->stats.samples++;
        }
    }
}

void dshm_head_changed(DShmHead *h, int first, int count)
{
    if (!h) {
        return;
    }
    int last = MIN(first + count, (int)h->height);

    if (!h->d) {
        return;
    }
    first = MAX(first, 0);
    if (first >= last || !h->changed) {
        return;
    }
    bitmap_set(h->changed, first, last - first);
    h->changed_any = true;
    h->d->stats.lines_stored += last - first;
}

void dshm_head_produced(DShmHead *h, unsigned count)
{
    if (h && h->d) {
        h->d->stats.lines_produced += count;
    }
}

void dshm_head_sample_end(DShmHead *h)
{
    if (!h) {
        return;
    }
    if (!h->d || !h->sample_open) {
        return;
    }
    h->sample_open = false;
    finish_sample(h);
}

static void run_sample(DShmHead *h, bool numbered)
{
    if (!numbered) {
        h->sample_seq++;
        h->d->stats.samples++;
    }
    h->in_sample = true;
    h->cur_ns = dshm_now_ns();
    h->sample_start_ns = h->cur_ns;
    if (h->ops && h->ops->sample && h->ref) {
        h->ops->sample(h->opaque);
    }
    h->sample_open = false;
    if (h->completed_seq != h->sample_seq) {
        finish_sample(h);
    }
    h->in_sample = false;
}

void dshm_head_do_sample(DShmHead *h)
{
    /* a sample may drop the big lock to wait: do not start one inside it */
    if (!h->present || h->held || h->in_sample) {
        return;
    }
    if (h->hold_armed) {
        h->hold_armed = false;
        h->held = true;
        h->sample_seq++;
        return;
    }
    run_sample(h, false);
}

void dshm_head_hold_release(DShmHead *h)
{
    if (h->held) {
        h->held = false;
        run_sample(h, true);
    }
}

static void head_bh(void *opaque)
{
    DShmHead *h = opaque;

    h->d->stats.wakeups++;
    if (!h->present || !qatomic_read(&h->unsampled) || !dshm_head_demand(h)) {
        /* The flag stays set, so later marks schedule nothing. */
        return;
    }
    if (h->ops && h->ops->retrace_armed && runstate_is_running() &&
        h->ops->retrace_armed(h->opaque)) {
        h->waiting_retrace = true;
        return;
    }
    dshm_head_do_sample(h);
}

void dshm_head_marked(DShmHead *h)
{
    if (!h) {
        return;
    }
    /* the mark is visible before the flag that announces it */
    smp_mb();
    if (!qatomic_xchg(&h->unsampled, true) && h->bh) {
        qemu_bh_schedule(h->bh);
    }
}

void dshm_head_retrace(DShmHead *h)
{
    if (!h) {
        return;
    }
    if (h->d && h->waiting_retrace) {
        h->waiting_retrace = false;
        dshm_head_do_sample(h);
    }
}

void dshm_head_retrace_stopped(DShmHead *h)
{
    if (!h) {
        return;
    }
    dshm_head_retrace(h);
}

static void run_state_change(void *opaque, bool running, RunState state)
{
    DisplayShm *d = opaque;

    if (running) {
        return;
    }
    /* A sample waiting for a retrace the stopped guest will not raise. */
    for (unsigned i = 0; i < d->n_heads; i++) {
        if (d->heads[i] && d->heads[i]->waiting_retrace) {
            dshm_head_retrace(d->heads[i]);
        }
    }
}

/* Mark lines of a core-owned source: set the bits, then tell the head. */
void dshm_head_mark_bits(DShmHead *h, int first, int count)
{
    int last = MIN(first + count, (int)h->height);

    first = MAX(first, 0);
    while (first < last && h->marks) {
        int word = first / BITS_PER_LONG, bit = first % BITS_PER_LONG;
        int n = MIN(BITS_PER_LONG - bit, last - first);
        unsigned long mask = (n == BITS_PER_LONG ? ~0ul :
                              (1ul << n) - 1) << bit;

        qatomic_or(&h->marks[word], mask);
        first += n;
    }
}

void dshm_head_mark(DShmHead *h, int first, int count)
{
    dshm_head_mark_bits(h, first, count);
    dshm_head_marked(h);
}

/* Sampling steps 2 to 4 for a source whose marks the producer holds. */
void dshm_head_sample_marks(DShmHead *h, DShmResolveRow resolve, void *opaque)
{
    g_autofree uint32_t *row = g_malloc(MAX(h->width, 1) * 4);
    size_t words = BITS_TO_LONGS(h->height);

    dshm_head_sample_start(h);
    for (size_t w = 0; w < words; w++) {
        unsigned long bits = qatomic_xchg(&h->marks[w], 0);

        while (bits) {
            unsigned y = w * BITS_PER_LONG + ctzl(bits);
            uint32_t *dst;

            bits &= bits - 1;
            if (y >= h->height) {
                continue;
            }
            dst = (uint32_t *)((uint8_t *)h->ref + y * h->ref_stride);
            resolve(opaque, y, row);
            h->d->stats.lines_produced++;
            if (memcmp(dst, row, h->width * 4)) {
                memcpy(dst, row, h->width * 4);
                dshm_head_changed(h, y, 1);
            }
        }
    }
    dshm_head_sample_end(h);
}

/* ---- Delivery --------------------------------------------------------- */

static unsigned stale_count(const DShmSlot *s)
{
    return bitmap_count_one(s->stale, s->set->height);
}

static DShmSlot *pick_slot(DShmVH *vh)
{
    DShmSet *set = vh->set;
    DShmSlot *best = NULL;
    unsigned best_n = UINT_MAX;

    head_refresh_leases(vh->h);
    for (unsigned i = 0; i < set->n; i++) {
        DShmSlot *s = set->slot[i];
        unsigned n;

        if (s->lent) {
            continue;
        }
        n = stale_count(s);
        if (n < best_n) {
            best = s;
            best_n = n;
        }
    }
    return best;
}

static void fill_slot(DShmHead *h, DShmSlot *s)
{
    DisplayShm *d = h->d;
    DShmSet *set = s->set;
    size_t wbytes = (size_t)h->width * 4;
    unsigned long y;
    uint64_t n = 0;

    for (y = find_first_bit(s->stale, set->height); y < set->height;
         y = find_next_bit(s->stale, set->height, y + 1)) {
        memcpy(s->r->ptr + y * set->stride,
               (uint8_t *)h->ref + y * h->ref_stride, wbytes);
        n++;
    }
    bitmap_zero(s->stale, set->height);
    if (n) {
        d->stats.fills++;
        d->stats.lines_copied += n;
        d->stats.bytes_copied += n * wbytes;
    }
}

/* Encode the viewer's accumulated damage after the FRAME. */
static size_t damage_encode(DShmVH *vh, uint8_t *out, uint16_t *kind,
                            uint16_t *count)
{
    DShmHead *h = vh->h;
    unsigned nrect = 0;
    struct { uint32_t x, y, w, h; } rect[DSHM_MAX_RECTS + 1];
    unsigned long y;

    if (vh->damage_all) {
        *kind = DSHM_DAMAGE_WHOLE;
        *count = 0;
        return 0;
    }
    for (y = find_first_bit(vh->damage, h->height); y < h->height;) {
        unsigned long end = find_next_zero_bit(vh->damage, h->height, y);

        if (nrect == DSHM_MAX_RECTS) {
            goto lines;
        }
        rect[nrect].x = 0;
        rect[nrect].y = y;
        rect[nrect].w = h->width;
        rect[nrect].h = end - y;
        nrect++;
        y = find_next_bit(vh->damage, h->height, end);
    }
    *kind = DSHM_DAMAGE_RECTS;
    *count = nrect;
    memcpy(out, rect, nrect * 16);
    return nrect * 16;
lines:
    {
        size_t words = DIV_ROUND_UP(h->height, 64);

        *kind = DSHM_DAMAGE_LINES;
        *count = words;
        memset(out, 0, words * 8);
        for (y = find_first_bit(vh->damage, h->height); y < h->height;
             y = find_next_bit(vh->damage, h->height, y + 1)) {
            out[y / 8] |= 1 << (y % 8);
        }
        return words * 8;
    }
}

static void vh_deliver(DShmVH *vh)
{
    DShmHead *h = vh->h;
    DShmViewer *v = vh->v;
    DShmOut *old = vh->queued, *o;
    uint64_t saved_last = vh->last_lent;
    DShmReq saved_req = vh->req;
    unsigned lease;
    DShmSlot *slot;
    uint8_t buf[sizeof(DShmFrame) + DSHM_MAX_RECTS * 16 + DSHM_MAX_DIM / 8];
    DShmFrame *f = (DShmFrame *)buf;
    uint16_t kind, count;
    size_t dlen, total;
    int idx;

    if (old) {
        /* Replace the unsent FRAME: its lease, its damage, its history. */
        lease = old->lease;
        saved_last = old->saved_last_lent;
        saved_req = old->saved_req;
        bitmap_or(vh->damage, vh->damage, old->saved_damage, h->height);
        vh->damage_all |= old->saved_damage_all;
        lease_free(vh, old->lease);
        vh->queued = NULL;
        dshm_out_remove(v, old);
    }
    slot = pick_slot(vh);
    if (!slot) {
        h->skipped++;
        state_frame_write(h);
        return;
    }
    if (stale_count(slot)) {
        fill_slot(h, slot);
    }
    idx = old ? (int)lease : vh_free_index(vh);
    assert(idx >= 0);
    lease = idx;
    memset(buf, 0, sizeof(*f));
    f->hdr.type = DSHM_MSG_FRAME;
    f->hdr.flags = DSHM_F_MUST;
    f->head = h->idx;
    f->region = slot->r->id;
    f->serial = h->n;
    f->mode_serial = vh->mode_serial;
    f->sample_ns = h->sample_ns;
    f->publish_ns = dshm_now_ns();
    f->guest_ns = h->guest_ns;
    f->lease = lease;
    f->frame_flags = h->cursor_embedded ? DSHM_FRAME_CURSOR_EMBEDDED : 0;
    f->lease_seq = vh->next_seq++;
    dlen = damage_encode(vh, buf + sizeof(*f), &kind, &count);
    f->damage_kind = kind;
    f->damage_count = count;
    total = ROUND_UP(sizeof(*f) + dlen, 8);
    o = dshm_queue(v, buf, total, -1, false);
    if (!o) {
        return;
    }
    o->region = slot->r->id;
    o->vh = vh;
    o->lease = lease;
    o->lease_seq = f->lease_seq;
    o->gen = vh->set_gen;
    o->slot = slot;
    o->saved_last_lent = saved_last;
    o->saved_req = saved_req;
    o->saved_damage = bitmap_new(MAX(h->height, 1));
    bitmap_copy(o->saved_damage, vh->damage, h->height);
    o->saved_damage_all = vh->damage_all;
    vh->lease[lease].used = true;
    vh->lease[lease].started = false;
    vh->lease[lease].seq = f->lease_seq;
    vh->lease[lease].slot = slot;
    slot->lent++;
    vh->queued = o;
    vh->last_lent = h->n;
    vh->req.frame = false;
    vh->req.sample_now = false;
    vh->req.bound = 0;
    bitmap_zero(vh->damage, h->height);
    vh->damage_all = false;
    h->d->stats.frames_sent++;
    {
        unsigned q = 0;

        for (unsigned i = 0; i < v->n_vh; i++) {
            q += v->vh[i] && v->vh[i]->queued;
        }
        v->wholly_queued_max = MAX(v->wholly_queued_max, q);
    }
    dshm_flush(v);
}

void dshm_frame_started(DShmViewer *v, DShmOut *o)
{
    DShmVH *vh = o->vh;

    if (!vh) {
        return;
    }
    vh->lease[o->lease].started = true;
    if (vh->queued == o) {
        vh->queued = NULL;
    }
    if (o->gen == vh->set_gen) {
        vh->reset = false;
    }
}

/* An unsent FRAME is being removed: undo it as a unit. */
void dshm_frame_cancel(DShmViewer *v, DShmOut *o)
{
    DShmVH *vh = o->vh;

    if (!vh) {
        return;
    }
    assert(vh->queued == o);
    vh->queued = NULL;
    lease_free(vh, o->lease);
    bitmap_or(vh->damage, vh->damage, o->saved_damage, vh->h->height);
    vh->damage_all |= o->saved_damage_all;
    vh->last_lent = o->saved_last_lent;
    if (!vh->req.frame) {
        vh->req = o->saved_req;
        vh->req.sample_now = false;
        vh->req.bound = 0;
    }
    vh->needs_eval = true;
    o->vh = NULL;
}

void dshm_viewer_eval_flagged(DShmViewer *v)
{
    for (unsigned i = 0; i < v->n_vh && !v->closing; i++) {
        if (v->vh[i] && v->vh[i]->needs_eval) {
            v->vh[i]->needs_eval = false;
            dshm_vh_eval(v->vh[i]);
        }
    }
}

static void vh_bind(DShmVH *vh)
{
    DShmHead *h = vh->h;
    int64_t quarter = h->mhz ? 250000000000ll / h->mhz : 4000000;

    if (h->sample_seq > h->completed_seq) {
        vh->req.bound = h->sample_seq;
    } else if (h->sample_seq &&
               dshm_now_ns() - h->sample_start_ns <= quarter) {
        vh->req.bound = h->sample_seq;
    } else {
        vh->req.bound = h->sample_seq + 1;
        dshm_head_do_sample(h);
    }
}

void dshm_vh_eval(DShmVH *vh)
{
    DShmHead *h = vh->h;
    uint64_t seen, last;

    if (vh->v->closing || !vh->v->ready || !vh->req.frame) {
        return;
    }
    if (!h->present || !h->ref_valid || !vh->set || !h->n) {
        /* A recorded request waits for the mode's first sample. */
        if (vh->req.sample_now && !vh->req.bound && h->present && vh->set &&
            vh_free_index(vh) >= 0) {
            if (h->sample_seq > h->completed_seq) {
                vh->req.bound = h->sample_seq;
            } else {
                vh->req.bound = h->sample_seq + 1;
                dshm_head_do_sample(h);
            }
        }
        return;
    }
    vh_refresh_leases(vh);
    if (vh_free_index(vh) < 0 && !vh->queued) {
        return;
    }
    if (vh->req.sample_now && !vh->req.bound) {
        vh_bind(vh);
        if (vh->req.bound == vh->h->sample_seq &&
            vh->req.bound > h->completed_seq) {
            return;
        }
        if (!vh->req.frame) {
            return;
        }
    }
    if (vh->req.bound && vh->req.bound > h->completed_seq) {
        return;
    }
    seen = vh->reset ? 0 : vh->req.frame_seen;
    last = vh->reset ? 0 : vh->last_lent;
    if (h->n > MAX(seen, last)) {
        vh_deliver(vh);
    }
}

void dshm_vh_request(DShmVH *vh, const DShmRequest *m)
{
    DShmHead *h = vh->h;

    if (m->kinds & DSHM_REQ_FRAME) {
        bool now = m->flags & DSHM_REQ_SAMPLE_NOW;

        if (now) {
            vh->req.sample_now = true;
            vh->req.bound = 0;
        } else if (!(vh->req.frame &&
                     (vh->req.bound || vh->req.sample_now))) {
            vh->req.sample_now = false;
            vh->req.bound = 0;
        }
        vh->req.frame = true;
        vh->req.frame_seen = m->frame_seen;
        vh_refresh_leases(vh);
        if (!now && qatomic_read(&h->unsampled)) {
            qemu_bh_schedule(h->bh);
        }
        dshm_vh_eval(vh);
    }
    if (m->kinds & DSHM_REQ_CURSOR) {
        vh->req.cursor = true;
        vh->req.cursor_seen = m->cursor_seen;
        dshm_head_state_cursor_notify(h);
    }
}

/* ---- Re-homing -------------------------------------------------------- */

void dshm_import_failed(DShmViewer *v, uint32_t region, uint32_t reason)
{
    /* Every lease the viewer holds on that region is released at once. */
    for (unsigned i = 0; i < v->n_vh; i++) {
        DShmVH *vh = v->vh[i];
        bool found = false;

        if (!vh) {
            continue;
        }
        for (unsigned l = 0; l < DSHM_MAX_LEASES; l++) {
            if (vh->lease[l].used && vh->lease[l].started &&
                vh->lease[l].slot->r->id == region) {
                lease_free(vh, l);
                found = true;
            }
        }
        if (!vh->set) {
            continue;
        }
        for (unsigned s = 0; s < vh->set->n; s++) {
            found |= vh->set->slot[s]->r->id == region;
        }
        if (!found) {
            continue;
        }
        switch (reason) {
        case DSHM_IMPORT_DEVICE_LOST:
            if (vh->lost_once) {
                dshm_viewer_close(v, DSHM_ERR_UNSUPPORTED, "no storage class");
                return;
            }
            vh->lost_once = true;
            vh_rehome(vh, false);
            break;
        case DSHM_IMPORT_TOO_LARGE:
            vh_leave_set(vh);
            vh->too_large = true;
            vh->set_gen++;
            vh_send_head(vh);
            break;
        case DSHM_IMPORT_NEEDS_WRITE:
            if (v->features & DSHM_FEAT_RW_PIXELS) {
                vh_rehome(vh, false);
            } else {
                dshm_viewer_close(v, DSHM_ERR_UNSUPPORTED, "no storage class");
                return;
            }
            break;
        case DSHM_IMPORT_REPRESENTATION:
            vh_rehome(vh, false);
            break;
        case DSHM_IMPORT_CLASS:
        case DSHM_IMPORT_ADAPTER:
        case DSHM_IMPORT_OOM:
            /* shm is the only class and the viewer cannot take it */
            dshm_viewer_close(v, DSHM_ERR_UNSUPPORTED, "no storage class");
            return;
        default:
            dshm_viewer_close(v, DSHM_ERR_PROTOCOL, "IMPORT_FAILED reason");
            return;
        }
    }
}

void dshm_vh_set_storage(DShmVH *vh, const uint32_t *storage)
{
    bool has_shm = false, term = false;

    for (unsigned i = 0; i < 8 && !term; i++) {
        term = !storage[i];
        has_shm |= storage[i] == DSHM_STORAGE_SHM;
    }
    if (!has_shm || !term) {
        dshm_viewer_close(vh->v, DSHM_ERR_PROTOCOL, "SET_STORAGE");
        return;
    }
    memcpy(vh->v->storage, storage, sizeof(vh->v->storage));
    vh->lost_once = false;
    vh_rehome(vh, false);
}

/* ---- Cursor ----------------------------------------------------------- */

static void state_cursor_write(DShmHead *h)
{
    DShmStateHead *sh = state_head(h->d, h->idx);

    h->cursor_seq += 2;
    qatomic_set(&sh->cursor_seq, h->cursor_seq - 1);
    /* the odd count is visible before any field changes */
    smp_wmb();
    qatomic_set(&sh->cursor_flags, h->cursor_visible);
    qatomic_set(&sh->cursor_x, h->cursor_x);
    qatomic_set(&sh->cursor_y, h->cursor_y);
    qatomic_set(&sh->cursor_image_serial, h->cursor_image_serial);
    /* the fields are written before the count turns even */
    smp_wmb();
    qatomic_set(&sh->cursor_seq, h->cursor_seq);
}

void dshm_head_state_cursor_notify(DShmHead *h)
{
    DisplayShm *d = h->d;

    for (unsigned i = 0; i < d->n_viewers; i++) {
        DShmVH *vh = viewer_vh(d->viewers[i], h);

        if (vh && vh->req.cursor && vh->req.cursor_seen != h->cursor_seq) {
            DShmCursorNotify m = { .hdr = { .type = DSHM_MSG_CURSOR_NOTIFY },
                                   .head = h->idx,
                                   .cursor_seq = h->cursor_seq };

            vh->req.cursor = false;
            dshm_queue(d->viewers[i], &m, sizeof(m), -1, false);
            dshm_flush(d->viewers[i]);
        }
    }
}

static void send_cursor_image(DShmViewer *v, DShmHead *h)
{
    size_t px = (size_t)h->cursor_w * h->cursor_h * 4;
    size_t len = ROUND_UP(sizeof(DShmCursorImage) + px, 8);
    g_autofree uint8_t *buf = g_malloc0(len);
    DShmCursorImage *m = (DShmCursorImage *)buf;
    DShmOut *o, *next;

    QTAILQ_FOREACH_SAFE(o, &v->out, next, next) {
        if (o->type == DSHM_MSG_CURSOR_IMAGE && !o->off &&
            ((DShmCursorImage *)o->buf)->head == h->idx) {
            dshm_out_remove(v, o);
        }
    }
    m->hdr.type = DSHM_MSG_CURSOR_IMAGE;
    m->head = h->idx;
    m->image_serial = h->cursor_image_serial;
    m->width = h->cursor_w;
    m->height = h->cursor_h;
    m->hot_x = h->cursor_hot_x;
    m->hot_y = h->cursor_hot_y;
    m->fourcc = DSHM_FOURCC_AR24;
    m->stride = h->cursor_w * 4;
    if (px) {
        memcpy(buf + sizeof(*m), h->cursor_pixels, px);
    }
    dshm_queue(v, buf, len, -1, false);
    dshm_flush(v);
}

void dshm_head_cursor_move(DShmHead *h, int x, int y, bool on)
{
    if (!h) {
        return;
    }
    DisplayShm *d = h->d;

    if (!d) {
        return;
    }
    if (h->cursor_visible == on && h->cursor_x == x && h->cursor_y == y) {
        return;
    }
    h->cursor_visible = on;
    h->cursor_x = x;
    h->cursor_y = y;
    h->fb_kind = h->steerable ? 1 : 0;
    h->fb_x = x;
    h->fb_y = y;
    if (!d->state || !h->present) {
        return;
    }
    state_cursor_write(h);
    dshm_head_state_cursor_notify(h);
    dshm_steer_cursor_moved(h);
}

void dshm_head_guest_cursor(DShmHead *h, unsigned kind, int x, int y)
{
    if (!h) {
        return;
    }
    if (!h->d) {
        return;
    }
    h->fb_kind = kind;
    h->fb_x = x;
    h->fb_y = y;
    if (h->d->state && h->present) {
        dshm_steer_cursor_moved(h);
    }
}

void dshm_head_cursor_define(DShmHead *h, QEMUCursor *c)
{
    DisplayShm *d = h->d;
    uint32_t w = c ? MIN(c->width, DSHM_CURSOR_MAX) : 0;
    uint32_t ht = c ? MIN(c->height, DSHM_CURSOR_MAX) : 0;

    if (!d) {
        return;
    }
    g_free(h->cursor_pixels);
    h->cursor_pixels = g_malloc0(MAX(w * ht * 4, 4));
    for (uint32_t y = 0; y < ht; y++) {
        memcpy(h->cursor_pixels + (size_t)y * w, c->data + (size_t)y * c->width,
               (size_t)w * 4);
    }
    h->cursor_w = w;
    h->cursor_h = ht;
    h->cursor_hot_x = c ? c->hot_x : 0;
    h->cursor_hot_y = c ? c->hot_y : 0;
    h->cursor_image_serial++;
    if (!d->state || !h->present) {
        return;
    }
    state_cursor_write(h);
    for (unsigned i = 0; i < d->n_viewers; i++) {
        if (d->viewers[i]->ready && viewer_vh(d->viewers[i], h)) {
            send_cursor_image(d->viewers[i], h);
        }
    }
    dshm_head_state_cursor_notify(h);
}

void dshm_dcl_mouse_set(DisplayChangeListener *dcl, int x, int y, bool on)
{
    DShmHead *h = container_of(dcl, DShmHead, dcl);

    dshm_head_cursor_move(h, x, y, on);
}

void dshm_dcl_cursor_define(DisplayChangeListener *dcl, QEMUCursor *c)
{
    DShmHead *h = container_of(dcl, DShmHead, dcl);

    dshm_head_cursor_define(h, c);
}

static const DisplayChangeListenerOps head_dcl_ops = {
    .dpy_name = "display-shm",
    .dpy_mouse_set = dshm_dcl_mouse_set,
    .dpy_cursor_define = dshm_dcl_cursor_define,
};

void dshm_head_set_steerable(DShmHead *h, bool steerable)
{
    if (!h) {
        return;
    }
    h->steerable = steerable;
}

void dshm_head_set_cursor_embedded(DShmHead *h, bool embedded)
{
    if (!h) {
        return;
    }
    h->cursor_embedded = embedded;
}

/* ---- Heads ------------------------------------------------------------ */

static void head_alloc_lines(DShmHead *h);

/* Slot damage buffers are sized for DSHM_MAX_DIM; a larger mode is nothing. */
static bool dim_ok(uint32_t width, uint32_t height)
{
    if (width > DSHM_MAX_DIM || height > DSHM_MAX_DIM) {
        warn_report_once("display-shm: a %ux%u mode is past %d; not published",
                         width, height, DSHM_MAX_DIM);
        return false;
    }
    return true;
}

/* A head belongs to the producer once there is one; a source may come first. */
static void head_adopt(DShmHead *h, DisplayShm *d)
{
    h->d = d;
    head_alloc_lines(h);
    h->bh = qemu_bh_new(head_bh, h);
    if (qatomic_read(&h->unsampled)) {
        qemu_bh_schedule(h->bh);
    }
}

DShmHead *dshm_head_new(DisplayShm *d, QemuConsole *con,
                        const DShmSourceOps *ops, void *opaque,
                        const char *label)
{
    DShmHead *h = g_new0(DShmHead, 1);

    h->idx = UINT_MAX;
    h->con = con;
    h->ops = ops;
    h->opaque = opaque;
    snprintf(h->label, sizeof(h->label), "%s", label ? label : "");
    QTAILQ_INIT(&h->sets);
    h->need_first = true;
    h->unsampled = true;
    h->dcl.update_interval = GUI_REFRESH_INTERVAL_IDLE;
    if (d) {
        head_adopt(h, d);
    }
    return h;
}

/* Sources come up before the device exists and wait here to be adopted. */
static GPtrArray *registered;

DShmHead *dshm_head_add(QemuConsole *con, const DShmSourceOps *ops,
                        void *opaque, const char *label)
{
    DisplayShm *d = the_display && the_display->ready ? the_display : NULL;
    DShmHead *h = dshm_head_new(d, con, ops, opaque, label);

    if (d) {
        dshm_head_announce(h);
    } else {
        if (!registered) {
            registered = g_ptr_array_new();
        }
        g_ptr_array_add(registered, h);
    }
    return h;
}

/* The head gets its number, its state record and its viewers. */
void dshm_head_announce(DShmHead *h)
{
    DisplayShm *d = h->d;

    if (h->idx != UINT_MAX) {
        return;
    }
    if (d->n_heads >= d->head_capacity) {
        warn_report("display-shm: more heads than the %u it has room for; "
                    "%s is not published", d->head_capacity, h->label);
        qemu_bh_delete(h->bh);
        h->bh = NULL;
        h->d = NULL;
        g_hash_table_add(d->seen_cons,
                         h->con ? (void *)h->con : (void *)h->seen_con);
        return;
    }
    h->idx = d->n_heads++;
    d->heads[h->idx] = h;
    g_hash_table_add(d->seen_cons,
                     h->con ? (void *)h->con : (void *)h->seen_con);
    h->present = true;
    if (h->con && !h->dcl_on) {
        h->dcl_on = true;
        qemu_console_register_listener(h->con, &h->dcl,
                                       h->dcl_ops ? h->dcl_ops :
                                                    &head_dcl_ops);
    }
    for (unsigned i = 0; i < d->n_viewers; i++) {
        DShmViewer *v = d->viewers[i];

        if (v->ready && !v->closing) {
            viewer_attach_head(v, h);
            if (h->cursor_image_serial) {
                send_cursor_image(v, h);
            }
            dshm_flush(v);
        }
    }
}

static void head_alloc_lines(DShmHead *h)
{
    g_free(h->marks);
    g_free(h->changed);
    h->marks = bitmap_new(MAX(h->height, 1));
    h->changed = bitmap_new(MAX(h->height, 1));
    h->changed_any = false;
}

void dshm_head_set_mode(DShmHead *h, uint32_t width, uint32_t height,
                        uint32_t refresh_mhz, uint32_t *ref, size_t stride)
{
    if (!h) {
        return;
    }
    DisplayShm *d = h->d;
    bool nothing = !width || !height || !ref || !dim_ok(width, height);
    bool geometry;

    if (nothing) {
        width = height = 0;
        ref = NULL;
    }
    geometry = width != h->width || height != h->height || ref != h->ref;
    h->mhz = refresh_mhz;
    if (!geometry) {
        if (h->idx != UINT_MAX && h->present) {
            for (unsigned i = 0; i < d->n_viewers; i++) {
                dshm_head_send_to(d->viewers[i], h);
            }
        }
        return;
    }
    h->width = width;
    h->height = height;
    h->ref = ref;
    h->ref_stride = stride;
    h->ref_valid = false;
    h->need_first = true;
    qatomic_set(&h->unsampled, true);
    if (h->d) {
        head_alloc_lines(h);
    }
    /* Samples in flight are discarded; the new mode is sampled instead. */
    h->held = false;
    h->completed_seq = h->sample_seq;
    h->waiting_retrace = false;
    if (h->idx == UINT_MAX) {
        return;
    }
    dshm_head_changed_geometry(h);
}

/* The producer-side half of a mode change: every set and viewer follows. */
void dshm_head_changed_geometry(DShmHead *h)
{
    DisplayShm *d = h->d;
    DShmSet *set, *next;

    /* The old sets give up their unlent slots first, to make room. */
    QTAILQ_FOREACH(set, &h->sets, next) {
        set->current = false;
        for (unsigned i = set->n; i-- > 0;) {
            if (!set->slot[i]->lent) {
                DShmSlot *s = set->slot[i];

                memmove(&set->slot[i], &set->slot[i + 1],
                        (set->n - i - 1) * sizeof(set->slot[0]));
                set->n--;
                slot_free(h, s);
            }
        }
    }
    for (unsigned i = 0; i < d->n_viewers; i++) {
        DShmViewer *v = d->viewers[i];
        DShmVH *vh = viewer_vh(v, h);

        if (!vh) {
            continue;
        }
        if (v->ready) {
            vh->req.bound = 0;
            vh_rehome(vh, true);
            dshm_steer_head_changed(h);
        }
    }
    QTAILQ_FOREACH_SAFE(set, &h->sets, next, next) {
        set_maybe_free(set);
    }
    if (h->unsampled && dshm_head_demand(h)) {
        qemu_bh_schedule(h->bh);
    }
}

void dshm_head_set_mode_owned(DShmHead *h, uint32_t width, uint32_t height,
                              uint32_t refresh_mhz)
{
    uint32_t *old = h->own;
    size_t stride;

    if (!dim_ok(width, height)) {
        width = height = 0;
    }
    stride = (size_t)width * 4;

    h->own = width && height ? g_malloc0(stride * height) : NULL;
    dshm_head_set_mode(h, width, height, refresh_mhz, h->own, stride);
    g_free(old);
    if (h->own) {
        dshm_head_mark(h, 0, height);
    }
}

void dshm_head_remove(DShmHead *h)
{
    if (!h) {
        return;
    }
    DisplayShm *d = h->d;

    if (!h->d) {
        /* Never adopted: nothing may find it, and its source is going. */
        if (registered) {
            g_ptr_array_remove(registered, h);
        }
        g_free(h->marks);
        g_free(h->changed);
        g_free(h->own);
        g_free(h);
        return;
    }
    h->ops = NULL;
    h->opaque = NULL;
    if (h->idx == UINT_MAX || !h->present) {
        return;
    }
    dshm_steer_head_changed(h);
    h->present = false;
    g_hash_table_remove(d->seen_cons, h->con);
    h->con = NULL;
    h->width = h->height = 0;
    h->ref = NULL;
    h->held = false;
    h->completed_seq = h->sample_seq;
    for (unsigned i = 0; i < d->n_viewers; i++) {
        DShmVH *vh = viewer_vh(d->viewers[i], h);

        if (vh) {
            vh->req.frame = vh->req.cursor = false;
            vh_drop_queued(vh);
            vh_leave_set(vh);
            vh_send_head(vh);
        }
    }
    if (h->dcl_on) {
        qemu_console_unregister_listener(&h->dcl);
        h->dcl_on = false;
    }
    /* The shell stays (viewers point at it, numbers are not reused). */
    qemu_bh_delete(h->bh);
    h->bh = NULL;
    g_free(h->marks);
    g_free(h->changed);
    g_free(h->own);
    g_free(h->cursor_pixels);
    h->marks = h->changed = NULL;
    h->own = NULL;
    h->cursor_pixels = NULL;
}

/*
 * Test control: retire the slot of the viewer's wholly queued FRAME as a
 * disconnect would. The FRAME is cancelled with it and undone.
 */
unsigned dshm_vh_retire_queued(DShmVH *vh)
{
    DShmOut *o = vh->queued;
    DShmRegion *r;
    DShmSlot *s;
    DShmSet *set;

    if (!o) {
        return 0;
    }
    s = o->slot;
    set = s->set;
    r = s->r;
    dshm_region_ref(r);
    /*
     * Out of the set first, so the HEAD that replaces the cancelled ones no
     * longer lists it; the cancelled FRAME's lease then frees it.
     */
    slot_retire(vh->h, s);
    dshm_region_forget(vh->v, r);
    dshm_region_unref(vh->h->d, r);
    if (set) {
        set_fit(set, NULL, true);
    }
    return 1;
}

/* Leases on a removed head's slots come free when the viewer next speaks. */
void dshm_viewer_refresh_gone(DShmViewer *v)
{
    for (unsigned i = 0; i < v->n_vh && !v->closing; i++) {
        DShmVH *vh = v->vh[i];

        if (vh && !vh->h->present) {
            vh_refresh_leases(vh);
        }
    }
}

void dshm_eval_flagged_all(DisplayShm *d)
{
    for (unsigned i = 0; i < d->n_viewers; i++) {
        dshm_viewer_eval_flagged(d->viewers[i]);
    }
}

/* ---- Stats ------------------------------------------------------------ */

void dshm_stats_fill(DisplayShm *d, DShmViewer *v, DShmStats *s)
{
    unsigned nsets = 0;

    for (unsigned i = 0; i < d->n_heads; i++) {
        DShmSet *set;

        if (!d->heads[i]) {
            continue;
        }
        QTAILQ_FOREACH(set, &d->heads[i]->sets, next) {
            nsets++;
        }
    }
    memset(s, 0, sizeof(*s));
    s->hdr.type = DSHM_EXT_TYPE(v->test_idx, DSHM_TEST_STATS);
    s->samples = d->stats.samples;
    s->fills = d->stats.fills;
    s->lines_produced = d->stats.lines_produced;
    s->lines_stored = d->stats.lines_stored;
    s->lines_copied = d->stats.lines_copied;
    s->bytes_copied = d->stats.bytes_copied;
    s->publishes = d->stats.publishes;
    s->frames_sent = d->stats.frames_sent;
    s->wakeups = d->stats.wakeups;
    s->sets = nsets;
    s->max_queued = v->wholly_queued_max;
    s->retained_bytes = d->retained;
}

/* ---- The object ------------------------------------------------------- */

static void display_shm_done(Notifier *n, void *data)
{
    DisplayShm *d = container_of(n, DisplayShm, done);
    Error *err = NULL;
    unsigned ncons = 0, cap;
    size_t state_bytes, off;
    DShmStateHeader *sh;
    QemuConsole *con;

    for (unsigned i = 0; i < CONSOLE_SCAN; i++) {
        con = qemu_console_lookup_by_index(i);
        ncons = con ? i + 1 : ncons;
    }
    if (registered) {
        d->n_pre = registered->len;
        d->pre = g_new0(DShmHead *, MAX(d->n_pre, 1));
        for (unsigned i = 0; i < d->n_pre; i++) {
            d->pre[i] = g_ptr_array_index(registered, i);
            head_adopt(d->pre[i], d);
        }
        g_ptr_array_free(registered, true);
        registered = NULL;
    }
    cap = d->head_capacity ? d->head_capacity :
          ncons + d->n_pre + (d->x_test ? 4 : 0) + 1;
    cap = MIN(cap, DSHM_FEEDBACK_HEADS);
    d->head_capacity = cap;
    d->heads = g_new0(DShmHead *, cap);

    off = ROUND_UP(sizeof(DShmStateHeader) +
                   (size_t)cap * sizeof(DShmStateHead), 64);
    d->steer_block_offset = off;
    state_bytes = off + (size_t)cap * DSHM_STEER_BLOCK_BYTES;
    d->state = dshm_region_new(d, DSHM_KIND_STATE, state_bytes, false, &err);
    if (!d->state) {
        error_report_err(err);
        exit(1);
    }
    sh = (DShmStateHeader *)d->state->ptr;
    sh->major = DSHM_MAJOR;
    sh->minor = DSHM_MINOR;
    sh->session_id = d->session_id;
    sh->head_offset = sizeof(DShmStateHeader);
    sh->head_stride = sizeof(DShmStateHead);
    sh->head_capacity = cap;
    d->seen_cons = g_hash_table_new(NULL, NULL);
    d->ready = true;

    /* Heads in console order: a registered source, a 2D device, or neither. */
    for (unsigned i = 0; i < ncons; i++) {
        QemuConsole *c = qemu_console_lookup_by_index(i);
        bool taken = false;

        if (!c || !qemu_console_is_graphic(c)) {
            continue;
        }
        for (unsigned p = 0; p < d->n_pre; p++) {
            if (d->pre[p] && d->pre[p]->con == c) {
                dshm_head_announce(d->pre[p]);
                d->pre[p] = NULL;
                taken = true;
            }
        }
        if (!taken) {
            dshm_surface_attach(d, c);
        }
    }
    for (unsigned p = 0; p < d->n_pre; p++) {
        if (d->pre[p]) {
            dshm_head_announce(d->pre[p]);
        }
    }
    g_free(d->pre);
    d->pre = NULL;
    d->n_pre = 0;
    dshm_test_head_add(d);
    dshm_transport_start(d, &err);
    if (err) {
        error_report_err(err);
        exit(1);
    }
    if (!d->vm_handler) {
        d->vm_handler = true;
        d->vm_entry = qemu_add_vm_change_state_handler(run_state_change, d);
    }
    /* everything is in place before the magic that says so */
    smp_wmb();
    qatomic_set(&sh->magic, DSHM_STATE_MAGIC);
}

/* Consoles that appeared after the machine was done get a head too. */
void dshm_rescan(DisplayShm *d)
{
    QemuConsole *c;

    for (unsigned i = 0; i < CONSOLE_SCAN; i++) {
        c = qemu_console_lookup_by_index(i);
        if (c && qemu_console_is_graphic(c) &&
            !g_hash_table_contains(d->seen_cons, c)) {
            dshm_surface_attach(d, c);
        }
    }
}

static void display_shm_exit(Notifier *n, void *data)
{
    DisplayShm *d = container_of(n, DisplayShm, exit);

    dshm_transport_stop(d);
}

static void display_shm_realize(DeviceState *dev, Error **errp)
{
    DisplayShm *d = DISPLAY_SHM(dev);
    uint64_t sid;

    if (HOST_BIG_ENDIAN) {
        error_setg(errp, "display-shm needs a little-endian host");
        return;
    }
    if (the_display) {
        error_setg(errp, "there is one display-shm device");
        return;
    }
    if ((d->vulkan && strcmp(d->vulkan, "off")) ||
        (d->udmabuf && strcmp(d->udmabuf, "off"))) {
        error_setg(errp, "display-shm: vulkan and udmabuf are not "
                   "implemented; leave them out or set them to off");
        return;
    }
    if (d->retire && *d->retire && strcmp(d->retire, "held")) {
        error_setg(errp, "retire-on-disconnect=%s is not implemented",
                   d->retire);
        return;
    }
    if ((d->listen_fd_prop < 0) == (!d->path || !*d->path)) {
        error_setg(errp, "display-shm needs exactly one of fd and path");
        return;
    }
    if (d->max_viewers < 1 || d->max_viewers > DSHM_VIEWERS_MAX) {
        error_setg(errp, "max-viewers is 1 to %d", DSHM_VIEWERS_MAX);
        return;
    }
    if (!d->steering) {
        d->steering = g_strdup("auto");
    }
    qemu_guest_getrandom_nofail(&sid, sizeof(sid));
    d->session_id = sid ? sid : 1;
    d->listen_fd = -1;
    d->next_region_id = 1;
    d->next_viewer_id = 1;
    the_display = d;
    dshm_producer_present = true;
    dshm_steer_init(d);
    dshm_test_init(d);
    d->done.notify = display_shm_done;
    qemu_add_machine_init_done_notifier(&d->done);
    d->exit.notify = display_shm_exit;
    qemu_add_exit_notifier(&d->exit);
}

static void display_shm_unrealize(DeviceState *dev)
{
    DisplayShm *d = DISPLAY_SHM(dev);

    notifier_remove(&d->done);
    notifier_remove(&d->exit);
    if (d->vm_entry) {
        qemu_del_vm_change_state_handler(d->vm_entry);
        d->vm_entry = NULL;
    }
    dshm_transport_stop(d);
    /* Sources go back to being inert; their heads stay for them to remove. */
    for (unsigned i = 0; i < d->n_heads; i++) {
        DShmHead *h = d->heads[i];

        if (h && h->dcl_on) {
            qemu_console_unregister_listener(&h->dcl);
            h->dcl_on = false;
        }
        if (h && h->bh) {
            qemu_bh_delete(h->bh);
            h->bh = NULL;
        }
        if (h) {
            h->d = NULL;
        }
    }
    dshm_producer_present = false;
    the_display = NULL;
}

static const Property display_shm_props[] = {
    DEFINE_PROP_INT32("fd", DisplayShm, listen_fd_prop, -1),
    DEFINE_PROP_STRING("path", DisplayShm, path),
    DEFINE_PROP_UINT32("max-viewers", DisplayShm, max_viewers, 4),
    DEFINE_PROP_UINT32("heads", DisplayShm, head_capacity, 0),
    DEFINE_PROP_STRING("allow-uid", DisplayShm, allow_uid),
    DEFINE_PROP_STRING("vulkan", DisplayShm, vulkan),
    DEFINE_PROP_STRING("udmabuf", DisplayShm, udmabuf),
    DEFINE_PROP_STRING("steering", DisplayShm, steering),
    DEFINE_PROP_SIZE("max-head-bytes", DisplayShm, max_head_bytes,
                     256 * MiB),
    DEFINE_PROP_UINT32("max-sets-per-head", DisplayShm, max_sets, 3),
    DEFINE_PROP_SIZE("max-retained-bytes", DisplayShm, max_retained, 1 * GiB),
    DEFINE_PROP_STRING("retire-on-disconnect", DisplayShm, retire),
    DEFINE_PROP_STRING("shm-prefix", DisplayShm, shm_prefix),
    DEFINE_PROP_BOOL("x-test", DisplayShm, x_test, false),
};

static void display_shm_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = display_shm_realize;
    dc->unrealize = display_shm_unrealize;
    dc->hotpluggable = false;
    dc->desc = "display protocol version 4 producer";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_props(dc, display_shm_props);
}

static const TypeInfo display_shm_info = {
    .name = TYPE_DISPLAY_SHM,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(DisplayShm),
    .class_init = display_shm_class_init,
};

static void display_shm_register(void)
{
    type_register_static(&display_shm_info);
}

type_init(display_shm_register);
