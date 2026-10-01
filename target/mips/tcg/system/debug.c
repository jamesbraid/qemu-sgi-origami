/*
 * ASID- and ExcCode-filtered breakpoints for the MIPS debug commands.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "chardev/char.h"
#include "exec/gdbstub.h"
#include "exec/tb-flush.h"
#include "hw/core/cpu.h"
#include "system/runstate.h"
#include "accel/tcg/cpu-loop.h"
#include "cpu.h"
#include "internal.h"
#include "../tcg-internal.h"

/*
 * The TCG breakpoint check for a BP_CPU breakpoint at the current PC.
 * Nothing else plants BP_CPU breakpoints on a MIPS CPU, so an unknown
 * PC is never a match.
 */
bool mips_debug_check_breakpoint(CPUState *cs)
{
    MIPSCPU *cpu = MIPS_CPU(cs);
    CPUMIPSState *env = &cpu->env;
    uint64_t pc = env->active_tc.PC;
    uint16_t asid = env->CP0_EntryHi & env->CP0_EntryHi_ASID_mask;
    uint8_t exccode = extract32(env->CP0_Cause, CP0Ca_EC, 5);
    int i;

    /* The resume after a stop is a check at the stop PC, not a new hit. */
    if (cpu->debug_bp_skip) {
        cpu->debug_bp_skip = false;
        if (cpu->debug_bp_skip_pc == pc) {
            return false;
        }
    }
    for (i = 0; i < MIPS_DEBUG_BREAKPOINTS; i++) {
        const MIPSDebugBreakpoint *bp = &cpu->debug_bp[i];

        if (bp->used && bp->pc == pc &&
            (!bp->has_asid || bp->asid == asid) &&
            (!bp->has_exccode || bp->exccode == exccode)) {
            return true;
        }
    }
    return false;
}

/*
 * cpu_exec is about to return EXCP_DEBUG for a breakpoint at the current
 * PC.  The generic handling of that return asks the gdb stub which CPU
 * stopped, and the stub indexes a process table that exists only once a
 * gdb server has been started, so a planted breakpoint stops the machine
 * from here instead: vm_stop() on a vCPU thread requests the debug run
 * state and parks this CPU, and leaving the loop with EXCP_INTERRUPT
 * keeps the generic path out of it.  A gdb server started with -s owns
 * the chardev named "gdb" and is told which CPU stopped, as the generic
 * path would tell it.  Any other debug stop clears the resume skip, so a
 * single-step past the stop PC never hides the next real hit.
 */
void mips_debug_breakpoint_hit(CPUState *cs)
{
    MIPSCPU *cpu = MIPS_CPU(cs);
    uint64_t pc = cpu->env.active_tc.PC;

    cpu->debug_bp_skip = false;
    if (!cpu_breakpoint_test(cs, pc, BP_CPU) ||
        !mips_debug_check_breakpoint(cs)) {
        return;
    }
    cpu->debug_bp_skip = true;
    cpu->debug_bp_skip_pc = pc;

    bql_lock();
    if (qemu_chr_find("gdb")) {
        gdb_set_stop_cpu(cs);
    }
    vm_stop(RUN_STATE_DEBUG);
    bql_unlock();

    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cs);
}

static bool mips_debug_breakpoint_same(const MIPSDebugBreakpoint *bp,
                                       const MIPSDebugBreakpoint *key)
{
    return bp->used && bp->pc == key->pc &&
           bp->has_asid == key->has_asid &&
           (!key->has_asid || bp->asid == key->asid) &&
           bp->has_exccode == key->has_exccode &&
           (!key->has_exccode || bp->exccode == key->exccode);
}

static bool mips_debug_breakpoint_at(const MIPSCPU *cpu, uint64_t pc)
{
    int i;

    for (i = 0; i < MIPS_DEBUG_BREAKPOINTS; i++) {
        if (cpu->debug_bp[i].used && cpu->debug_bp[i].pc == pc) {
            return true;
        }
    }
    return false;
}

typedef struct MIPSDebugBreakpointWork {
    const MIPSDebugBreakpoint *bp;
    bool remove;
    Error *err;
} MIPSDebugBreakpointWork;

/* Runs on the CPU's own thread, between translation blocks. */
static void mips_debug_breakpoint_work(CPUState *cs, run_on_cpu_data data)
{
    MIPSDebugBreakpointWork *w = data.host_ptr;
    MIPSCPU *cpu = MIPS_CPU(cs);
    MIPSDebugBreakpoint *found = NULL, *slot = NULL;
    int i;

    cpu->debug_bp_skip = false;
    for (i = 0; i < MIPS_DEBUG_BREAKPOINTS; i++) {
        MIPSDebugBreakpoint *bp = &cpu->debug_bp[i];

        if (mips_debug_breakpoint_same(bp, w->bp)) {
            found = bp;
        } else if (!bp->used && !slot) {
            slot = bp;
        }
    }

    if (w->remove) {
        if (!found) {
            error_setg(&w->err, "no such breakpoint on CPU %d",
                       cs->cpu_index);
            return;
        }
        found->used = false;
        if (!mips_debug_breakpoint_at(cpu, w->bp->pc)) {
            cpu_breakpoint_remove(cs, w->bp->pc, BP_CPU);
        }
        return;
    }

    if (found) {
        error_setg(&w->err, "breakpoint already planted on CPU %d",
                   cs->cpu_index);
        return;
    }
    if (!slot) {
        error_setg(&w->err, "CPU %d has no free breakpoint slot",
                   cs->cpu_index);
        return;
    }
    if (!mips_debug_breakpoint_at(cpu, w->bp->pc)) {
        cpu_breakpoint_insert(cs, w->bp->pc, BP_CPU, NULL);
    }
    *slot = *w->bp;
    slot->used = true;
    /* Blocks already chained through this PC would skip the check. */
    queue_tb_flush(cs);
}

void mips_debug_breakpoint_update(MIPSCPU *cpu, const MIPSDebugBreakpoint *bp,
                                  bool remove, Error **errp)
{
    MIPSDebugBreakpointWork w = { .bp = bp, .remove = remove };

    run_on_cpu(CPU(cpu), mips_debug_breakpoint_work, RUN_ON_CPU_HOST_PTR(&w));
    error_propagate(errp, w.err);
}
