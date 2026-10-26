/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The per-thread re-entry guard SGI graphics heads share
 * (hw/display/sgi/head-guard.c). It has no other dependencies, so a host
 * unit test links it alone.
 */
#ifndef HW_DISPLAY_SGI_HEAD_GUARD_H
#define HW_DISPLAY_SGI_HEAD_GUARD_H

/*
 * Whether this thread is inside an SGI head's register access or work.
 * A head's DMA can be aimed through the Crossbow at another head's
 * registers, and that head's access can let go of the big lock to wait
 * for its own work, so an access that arrives from inside any head's
 * access or work is refused, as QEMU's re-entrancy guard would refuse it.
 * Bracket every register access and every slice with enter and leave;
 * enter returns false for an access that must be refused.
 */
bool sgi_head_work_enter(void);
void sgi_head_work_leave(void);
/* Bracket a slice or drain that runs outside any register access. */
void sgi_head_work_enter_work(void);

#endif
