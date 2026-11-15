/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * What the transport (sgi-display-shm.c) and the input half
 * (sgi-display-input.c) of the SGI display share. Not a model interface:
 * models use include/ui/sgi-display-shm.h.
 */
#ifndef UI_SGI_DISPLAY_PRIV_H
#define UI_SGI_DISPLAY_PRIV_H

#include "ui/sgi-display-shm.h"

/*
 * Queue one INPUT message into the input layer. Returns true when it queued
 * an event, so the caller owes one qemu_input_event_sync() after the batch.
 */
bool sgi_display_input_apply(const SGIShmMsgInput *in);

/* The reader's connection ended: forget what it asked for. */
void sgi_display_input_reader_gone(void);

/* A guest cursor update on head: advance a pending absolute move. */
void sgi_display_input_cursor_moved(unsigned head);

/* The cursor QEMU last saw on a head; false for a head that is not bound. */
bool sgi_display_head_cursor(unsigned head, int32_t *x, int32_t *y,
                             bool *on);
/* The head the guest cursor was last shown on, or -1 for none yet. */
int sgi_display_cursor_head(void);
unsigned sgi_display_head_count(void);

#endif
