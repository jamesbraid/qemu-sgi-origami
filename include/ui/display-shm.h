/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * What a source of pixels calls (docs/interop/display-shm.rst). It owns a
 * reference frame; ops->sample reports the lines that differ with
 * dshm_head_changed() and ends with dshm_head_sample_end().
 */
#ifndef UI_DISPLAY_SHM_H
#define UI_DISPLAY_SHM_H

#include "ui/console.h"

typedef struct DisplayShm DisplayShm;
typedef struct DShmHead DShmHead;

typedef struct DShmSourceOps {
    /* Sampling steps 1 to 3: acquire, take marks, produce, report. */
    void (*sample)(void *opaque);
    /* Whether a retrace will come to pace a sample; NULL: never. */
    bool (*retrace_armed)(void *opaque);
} DShmSourceOps;

/* Register a source; the device adopts it when the machine is done. */
DShmHead *dshm_head_add(QemuConsole *con, const DShmSourceOps *ops,
                        void *opaque, const char *label);
void dshm_head_remove(DShmHead *h);

extern bool dshm_producer_present;

/* Whether a device exists, so a source can skip work only one needs. */
static inline bool dshm_head_active(DShmHead *h)
{
    return dshm_producer_present && h;
}

/*
 * The head's mode and its reference frame, which the source owns until the
 * next call (NULL or 0x0: nothing shown). The source marks every line.
 */
void dshm_head_set_mode(DShmHead *h, uint32_t width, uint32_t height,
                        uint32_t refresh_mhz, uint32_t *ref, size_t stride);

/*
 * A source that has no frame of its own: the producer allocates the
 * reference frame and resolve_row fills a line of it.
 */
typedef void (*DShmResolveRow)(void *opaque, unsigned y, uint32_t *dst);
void dshm_head_set_mode_owned(DShmHead *h, uint32_t width, uint32_t height,
                              uint32_t refresh_mhz);
void dshm_head_mark(DShmHead *h, int first, int count);
void dshm_head_sample_marks(DShmHead *h, DShmResolveRow resolve,
                            void *opaque);

/* A line was marked: any thread, after the mark's store. */
void dshm_head_marked(DShmHead *h);
/* Sampling step 2: the flag clears before the marks are read. */
void dshm_head_sample_start(DShmHead *h);
/* Lines first .. first + count - 1 differed from the reference frame. */
void dshm_head_changed(DShmHead *h, int first, int count);
/* Lines the sample produced, for the statistics. */
void dshm_head_produced(DShmHead *h, unsigned count);
/* Sampling step 4, also for an update someone else asked for. */
void dshm_head_sample_end(DShmHead *h);
/* The source's retrace fired, or its timer stopped. */
void dshm_head_retrace(DShmHead *h);
void dshm_head_retrace_stopped(DShmHead *h);

/* The source reports where the guest drew its cursor, so ABS can steer. */
void dshm_head_set_steerable(DShmHead *h, bool steerable);
/* The source draws its cursor into the pixels it resolves. */
void dshm_head_set_cursor_embedded(DShmHead *h, bool embedded);

void dshm_head_cursor_move(DShmHead *h, int x, int y, bool on);
void dshm_head_cursor_define(DShmHead *h, QEMUCursor *c);
/* Where the guest drew its cursor: kind 1 hot spot, 2 glyph top-left. */
void dshm_head_guest_cursor(DShmHead *h, unsigned kind, int x, int y);

#endif
