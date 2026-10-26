/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI graphics head work that outlasts the store that started it.
 *
 * A head that runs a long command stream (a display list, say) inside the
 * processor store that started it keeps that processor from its
 * interrupts for as long as the stream takes. IRIX on IP27 counts one
 * scheduler tick per Count/Compare interrupt and does not make up lost
 * ticks, so its clock stalls for the length of the stream. This helper
 * lets a family run such work in slices instead: a little inside the
 * store, the rest on a thread of the head's own, paced by the machine
 * clock so that nothing runs while the machine is stopped and qtest steps
 * it. The family decides what the work is, which accesses must wait for
 * it, and what waits behind it.
 */
#ifndef HW_DISPLAY_SGI_HEAD_WORK_H
#define HW_DISPLAY_SGI_HEAD_WORK_H

#include "qemu/thread.h"
#include "qemu/timer.h"
#include "hw/display/sgi/head-guard.h"

/*
 * How much one slice may do: host time, because a slice exists to bound
 * how long its thread keeps the device, and for qtest a count of words.
 * A slice checks the budget between words, so it always does at least one
 * and stops at the first word boundary past the deadline.
 */
typedef struct SGIHeadWorkBudget {
    int64_t deadline;           /* QEMU_CLOCK_REALTIME; INT64_MAX: none */
    uint32_t words;             /* UINT32_MAX: none */
} SGIHeadWorkBudget;

/*
 * One slice of host time. The IRIX scheduler clock ticks every 10 ms, so a
 * slice stays well below that.
 */
#define SGI_HEAD_WORK_SLICE_NS  (1 * SCALE_MS)

typedef struct SGIHeadWorkOps {
    /* Go on with the work until the budget is spent or nothing is left. */
    void (*step)(void *opaque, SGIHeadWorkBudget *b);
    /* Whether any work is left. */
    bool (*pending)(void *opaque);
    /* After a slice on the head's thread, under the big lock. */
    void (*slice_done)(void *opaque);
} SGIHeadWorkOps;

typedef struct SGIHeadWork {
    const SGIHeadWorkOps *ops;
    void *opaque;
    /* Host-only: words a slice for qtest; 0 means host time. */
    uint32_t test_words;
    /* When the work last became pending, on QEMU_CLOCK_VIRTUAL; 0: idle. */
    int64_t pending_since;
    QEMUTimer *timer;
    QemuThread thread;
    QemuCond kick;
    QemuCond done;
    bool started;
    bool go;
    bool exit;
    bool slice;                 /* a slice runs on the head's thread */
    bool drawing;               /* ... and has let go of the big lock */
} SGIHeadWork;

void sgi_head_work_init(SGIHeadWork *w, const char *name,
                        const SGIHeadWorkOps *ops, void *opaque);
/* Stops the thread; the family abandons any work left afterwards. */
void sgi_head_work_cleanup(SGIHeadWork *w);

/* The budget for a slice; @timed bounds it by host time. */
SGIHeadWorkBudget sgi_head_work_budget(SGIHeadWork *w, bool timed);
/* Count one word against the budget; true when the slice must stop. */
bool sgi_head_work_spent(SGIHeadWorkBudget *b);

/* Work has been left pending: run the next slice soon. */
void sgi_head_work_schedule(SGIHeadWork *w);
/* Wait out a slice that has let go of the big lock on another thread. */
void sgi_head_work_wait_slice(SGIHeadWork *w);
/* Finish all the work now, on this thread. */
void sgi_head_work_drain(SGIHeadWork *w);
/* Forget the work's timing once the family has abandoned it. */
void sgi_head_work_idle(SGIHeadWork *w);
/* Whether work has been pending for longer than @ns of machine time. */
bool sgi_head_work_overdue(SGIHeadWork *w, int64_t ns);

/*
 * Whether a draw on this thread may let go of the big lock: inside a slice
 * on the head's own thread, which every other thread that decodes the
 * work waits out. A family brackets such a draw with
 * sgi_head_work_unlock() and sgi_head_work_relock(), and must arrange
 * that nothing else touches what the draw reads or writes meanwhile.
 */
bool sgi_head_work_may_unlock(SGIHeadWork *w);
void sgi_head_work_unlock(SGIHeadWork *w);
void sgi_head_work_relock(SGIHeadWork *w);

#endif
