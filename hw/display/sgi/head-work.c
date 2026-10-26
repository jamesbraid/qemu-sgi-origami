/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI graphics head work that outlasts the store that started it. See the
 * header for the contract.
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "hw/display/sgi/head-work.h"

SGIHeadWorkBudget sgi_head_work_budget(SGIHeadWork *w, bool timed)
{
    return (SGIHeadWorkBudget) {
        .deadline = timed ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                            SGI_HEAD_WORK_SLICE_NS : INT64_MAX,
        .words = w->test_words ? w->test_words : UINT32_MAX,
    };
}

bool sgi_head_work_spent(SGIHeadWorkBudget *b)
{
    if (b->words != UINT32_MAX && !--b->words) {
        return true;
    }
    return b->deadline != INT64_MAX &&
           qemu_clock_get_ns(QEMU_CLOCK_REALTIME) >= b->deadline;
}

/*
 * The next slice runs a short gap later on the machine clock. The gap is
 * for the vCPUs: outside its draws a slice holds the big lock, and a
 * thread that drops the lock and takes it straight back usually wins it
 * again.
 */
#define SGI_HEAD_WORK_GAP_NS    (100 * SCALE_US)

void sgi_head_work_schedule(SGIHeadWork *w)
{
    if (!w->pending_since) {
        w->pending_since = MAX(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), 1);
    }
    if (!timer_pending(w->timer)) {
        timer_mod(w->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SGI_HEAD_WORK_GAP_NS);
    }
}

void sgi_head_work_idle(SGIHeadWork *w)
{
    w->pending_since = 0;
    timer_del(w->timer);
}

bool sgi_head_work_overdue(SGIHeadWork *w, int64_t ns)
{
    return w->pending_since &&
           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - w->pending_since > ns;
}

void sgi_head_work_wait_slice(SGIHeadWork *w)
{
    while (w->slice) {
        qemu_cond_wait_bql(&w->done);
    }
}

void sgi_head_work_drain(SGIHeadWork *w)
{
    SGIHeadWorkBudget budget = { .deadline = INT64_MAX, .words = UINT32_MAX };

    sgi_head_work_wait_slice(w);
    if (w->ops->pending(w->opaque)) {
        w->ops->step(w->opaque, &budget);
    }
    sgi_head_work_idle(w);
}

bool sgi_head_work_may_unlock(SGIHeadWork *w)
{
    return w->slice && qemu_thread_is_self(&w->thread);
}

void sgi_head_work_unlock(SGIHeadWork *w)
{
    w->drawing = true;
    bql_unlock();
}

void sgi_head_work_relock(SGIHeadWork *w)
{
    bql_lock();
    w->drawing = false;
}

/*
 * A slice runs on the head's own thread, not the main loop's: the main
 * loop raises the processors' timer interrupts, and a draw there would
 * hold it for as long as the draw takes, delaying the scheduler tick past
 * what IRIX makes up.
 */
static void sgi_head_work_timer(void *opaque)
{
    SGIHeadWork *w = opaque;

    w->go = true;
    qemu_cond_signal(&w->kick);
}

static void *sgi_head_work_thread(void *opaque)
{
    SGIHeadWork *w = opaque;

    rcu_register_thread();
    bql_lock();
    for (;;) {
        SGIHeadWorkBudget budget;

        while (!w->go && !w->exit) {
            qemu_cond_wait_bql(&w->kick);
        }
        if (w->exit) {
            break;
        }
        w->go = false;
        budget = sgi_head_work_budget(w, true);
        sgi_head_work_enter_work();
        w->slice = true;
        w->ops->step(w->opaque, &budget);
        w->slice = false;
        sgi_head_work_leave();
        qemu_cond_broadcast(&w->done);
        if (w->ops->pending(w->opaque)) {
            sgi_head_work_schedule(w);
        } else {
            sgi_head_work_idle(w);
        }
        if (w->ops->slice_done) {
            w->ops->slice_done(w->opaque);
        }
    }
    bql_unlock();
    rcu_unregister_thread();
    return NULL;
}

void sgi_head_work_init(SGIHeadWork *w, const char *name,
                        const SGIHeadWorkOps *ops, void *opaque)
{
    w->ops = ops;
    w->opaque = opaque;
    w->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sgi_head_work_timer, w);
    qemu_cond_init(&w->kick);
    qemu_cond_init(&w->done);
    qemu_thread_create(&w->thread, name, sgi_head_work_thread, w,
                       QEMU_THREAD_JOINABLE);
    w->started = true;
}

void sgi_head_work_cleanup(SGIHeadWork *w)
{
    if (!w->started) {
        return;
    }
    /* The thread needs the big lock to see that it should stop. */
    w->exit = true;
    qemu_cond_signal(&w->kick);
    bql_unlock();
    qemu_thread_join(&w->thread);
    bql_lock();
    timer_free(w->timer);
    w->timer = NULL;
    qemu_cond_destroy(&w->kick);
    qemu_cond_destroy(&w->done);
    w->started = false;
}
