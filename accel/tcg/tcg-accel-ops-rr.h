/*
 * QEMU TCG Single Threaded vCPUs implementation
 *
 * Copyright 2020 SUSE LLC
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef TCG_ACCEL_OPS_RR_H
#define TCG_ACCEL_OPS_RR_H

/* Kick all RR vCPUs. */
void rr_kick_vcpu_thread(CPUState *requested);

/* Give one runnable RR slice to the requested vCPU. */
void rr_handoff_vcpu_thread(CPUState *requested);

/* start the round robin vcpu thread */
void rr_start_vcpu_thread(CPUState *cpu);

#endif /* TCG_ACCEL_OPS_RR_H */
