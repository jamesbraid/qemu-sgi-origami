/*
 * QEMU MIPS CPU
 *
 * Copyright (c) 2012 SUSE LINUX Products GmbH
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <http://www.gnu.org/licenses/lgpl-2.1.html>
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "internal.h"
#include "exec/cputlb.h"
#include "../tcg/trace.h"

/* Called for updates to CP0_Status.  */
void sync_c0_status(CPUMIPSState *env, CPUMIPSState *cpu, int tc)
{
    int32_t tcstatus, *tcst;
    uint32_t v = cpu->CP0_Status;
    uint32_t cu, mx, asid, ksu;
    uint32_t mask = ((1 << CP0TCSt_TCU3)
                       | (1 << CP0TCSt_TCU2)
                       | (1 << CP0TCSt_TCU1)
                       | (1 << CP0TCSt_TCU0)
                       | (1 << CP0TCSt_TMX)
                       | (3 << CP0TCSt_TKSU)
                       | (0xff << CP0TCSt_TASID));

    cu = (v >> CP0St_CU0) & 0xf;
    mx = (v >> CP0St_MX) & 0x1;
    ksu = (v >> CP0St_KSU) & 0x3;
    asid = env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask;

    tcstatus = cu << CP0TCSt_TCU0;
    tcstatus |= mx << CP0TCSt_TMX;
    tcstatus |= ksu << CP0TCSt_TKSU;
    tcstatus |= asid;

    if (tc == cpu->current_tc) {
        tcst = &cpu->active_tc.CP0_TCStatus;
    } else {
        tcst = &cpu->tcs[tc].CP0_TCStatus;
    }

    *tcst &= ~mask;
    *tcst |= tcstatus;
    compute_hflags(cpu);
}

static void mips_store_status(CPUMIPSState *env, target_ulong val)
{
    uint32_t mask = env->CP0_Status_rw_bitmask;
    target_ulong old;

    old = env->CP0_Status;

    if (env->insn_flags & ISA_MIPS_R6) {
        bool has_supervisor = extract32(mask, CP0St_KSU, 2) == 0x3;
#if defined(TARGET_MIPS64)
        uint32_t ksux = (1 << CP0St_KX) & val;
        ksux |= (ksux >> 1) & val; /* KX = 0 forces SX to be 0 */
        ksux |= (ksux >> 1) & val; /* SX = 0 forces UX to be 0 */
        val = (val & ~(7 << CP0St_UX)) | ksux;
#endif
        if (has_supervisor && extract32(val, CP0St_KSU, 2) == 0x3) {
            mask &= ~(3 << CP0St_KSU);
        }
        mask &= ~(((1 << CP0St_SR) | (1 << CP0St_NMI)) & val);
    }

    env->CP0_Status = (old & ~mask) | (val & mask);
#if defined(TARGET_MIPS64)
    if ((env->CP0_Status ^ old) & (old & (7 << CP0St_UX))) {
        /* Access to at least one of the 64-bit segments has been disabled */
        tlb_flush(env_cpu(env));
    }
#endif
    if (ase_mt_available(env)) {
        sync_c0_status(env, env, env->current_tc);
    } else {
        compute_hflags(env);
    }
}

/*
 * Put CPU_INTERRUPT_HARD back up if a pending interrupt has just become
 * deliverable. mips_cpu_exec_interrupt() drops the flag when a pending
 * interrupt can neither be taken nor wake the CPU, so that the exec
 * loop's lock-free skip survives a masked window; every own-CPU change
 * that can end such a window has to call this, or the interrupt waits
 * for the next edge on its line.
 *
 * The test needs no lock. Only the CPU that owns this state calls it,
 * from its own thread, so the mask side is single-writer. The one
 * cross-thread input is Cause.IP, and a device that sets a bit there
 * raises the flag itself, under the lock, as part of setting it -- so
 * a stale read here can only miss a bit somebody else is already
 * raising for. Reading a flag that is about to be raised costs nothing;
 * raising one that is no longer warranted costs a single crossing,
 * which the next rejection clears.
 */
void cpu_mips_reassert_interrupt(CPUMIPSState *env)
{
    CPUState *cs = env_cpu(env);

    /*
     * Nothing to do where the MT ASE is present, and the reads below
     * must not happen there either. mips_cpu_exec_interrupt() excludes
     * MT from the clear, so the flag is never down while an interrupt
     * is pending and this function could only ever return -- but it
     * would return after reading CP0_Status, which helper_mttc0_status()
     * can be writing from another VPE's thread at the same moment.
     * Testing Config3 first, a field fixed at realize, keeps the
     * exclusion on both halves of the invariant rather than on one.
     */
    if (ase_mt_available(env)) {
        return;
    }

    if (!cpu_mips_hw_interrupts_pending(env) ||
        !cpu_mips_hw_interrupts_enabled(env) ||
        cpu_test_interrupt(cs, CPU_INTERRUPT_HARD)) {
        return;
    }

    BQL_LOCK_GUARD();
    cpu_interrupt(cs, CPU_INTERRUPT_HARD);
}

void cpu_mips_store_status(CPUMIPSState *env, target_ulong val)
{
    bool tracing = trace_event_get_state(TRACE_MIPS_CP0_STATUS_WRITE);
    uint32_t old_status = tracing ? env->CP0_Status : 0;

    /*
     * The only writer of a CPU's CP0_Status from another thread is
     * helper_mttc0_status() (cp0_helper.c). It reaches the field through
     * mips_cpu_map_tc(), which returns the caller's own env unless the
     * caller has CP0_VPEConf0.MVP set -- a bit that only a processor
     * implementing the MT ASE ever has. Where MT is absent, this field
     * has exactly one writer, the owning VPE running on its own thread,
     * and the lock has no second party to exclude. Take it only where
     * there is one; a machine built from a single cpu_type is
     * homogeneous, so the local test answers for every CPU that could
     * address this one.
     */
    if (cpu_mips_r10k_perf_mode_sensitive(env)) {
        /*
         * A live R10000 performance counter changes representation with
         * the mode, so the write happens between the freeze/thaw pair,
         * under the lock so the counters' deadline callback cannot read
         * one mid-fold. Only a guest that is actually profiling pays.
         */
        BQL_LOCK_GUARD();

        cpu_mips_r10k_perf_freeze(env);
        mips_store_status(env, val);
        cpu_mips_r10k_perf_thaw(env);
    } else if (ase_mt_available(env)) {
        BQL_LOCK_GUARD();
        mips_store_status(env, val);
    } else {
        mips_store_status(env, val);
    }

    cpu_mips_reassert_interrupt(env);
    if (tracing) {
        trace_mips_cp0_status_write(
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
            qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
            env_cpu(env)->cpu_index, env->active_tc.PC, old_status,
            env->CP0_Status, qatomic_read(&env->CP0_Cause),
            cpu_test_interrupt(env_cpu(env), CPU_INTERRUPT_HARD));
    }
}

static void mips_store_cause(CPUMIPSState *env, target_ulong val)
{
    uint32_t mask = 0x00C00300;
    uint32_t old, new;
    int i;

    /*
     * The store itself needs no lock: cp0_cause_update() composes with
     * the device and timer writers of this same word, which is what the
     * lock used to be here for. What still needs one is the work the
     * store triggers, and each piece takes it where it is needed.
     */
    if (env->insn_flags & ISA_MIPS_R2) {
        mask |= 1 << CP0Ca_DC;
    }
    if (env->insn_flags & ISA_MIPS_R6) {
        mask &= ~((1 << CP0Ca_WP) & val);
    }
    if (env->cfg.cause_rw_bitmask) {
        /*
         * The R10000 has no IV or WP: its Cause bits 27:16 are reserved,
         * written as zeroes and returning zeroes (manual 14.11), and every
         * bit but the two software interrupts is read-only. Letting bit 23
         * latch would also move interrupt dispatch to a vector this
         * processor does not have.
         */
        mask &= env->cfg.cause_rw_bitmask;
    }

    old = cp0_cause_update(env, mask, val & mask);
    new = (old & ~mask) | (val & mask);

    if ((old ^ new) & (1 << CP0Ca_DC)) {
        /*
         * Both directions fold the ticks elapsed so far back into
         * CP0_Count and reprogram the timer. cpu_mips_store_count()
         * guards itself, cpu_mips_stop_count() does not, so hold the
         * lock across either.
         */
        BQL_LOCK_GUARD();

        if (new & (1 << CP0Ca_DC)) {
            cpu_mips_stop_count(env);
        } else {
            cpu_mips_start_count(env);
        }
    }

    /* Set/reset software interrupts. cpu_mips_irq_request() guards. */
    for (i = 0 ; i < 2 ; i++) {
        if ((old ^ new) & (1 << (CP0Ca_IP + i))) {
            cpu_mips_soft_irq(env, i, new & (1 << (CP0Ca_IP + i)));
        }
    }
}

void cpu_mips_store_cause(CPUMIPSState *env, target_ulong val)
{
    /*
     * Where the MT ASE is present, helper_mttc0_cause() runs this same
     * store against this CPU's Cause from another VPE's thread, under the
     * big lock. The compare-and-swap keeps the word itself consistent
     * either way, but the side effects above are computed from one call's
     * own old/new snapshot and applied after its swap: two unserialized
     * stores that both move the DC bit or a software interrupt line could
     * apply their timer and line effects in the opposite order to their
     * swaps, leaving the timer or the line disagreeing with the register.
     * Take the lock where that second writer can exist, exactly as
     * cpu_mips_store_status() does; where MT is absent, every bit this
     * store owns has one writer and the lock-free path stands.
     */
    if (ase_mt_available(env)) {
        BQL_LOCK_GUARD();
        mips_store_cause(env, val);
    } else {
        mips_store_cause(env, val);
    }
}
