/* SPDX-License-Identifier: MIT */
/*
 * The R10000 family's performance counters: the counting model, their
 * share of the IP[7] interrupt line, and the deadline timer that delivers
 * a counter's overflow interrupt.
 *
 * Built for every MIPS system target, not only mips64: cp0.c and the
 * shared helpers call in without a TARGET_MIPS64 guard, so the symbols
 * must link everywhere even though only mips64 can create an R10000.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "internal.h"

/*
 * The R10000 family's performance counters, the other half of register
 * 25 beside the control registers: each counter advances once per its
 * control register's event and holds IP[7] up while its bit 31 is set and
 * the control register's interrupt enable is on. Of the manual's event
 * table only Cycles, event 0, has an honest analogue here - the processor
 * clock the machine drives, the same basis CP0_Count is derived from.
 * Every other event names microarchitecture this model does not have
 * (issue, graduation, caches, branches, external interventions), so a
 * counter selecting one holds its written value rather than inventing
 * increments; instructions graduated could only be derived from icount,
 * which this machine does not run with.
 *
 * A counter counting Cycles stores its value the way CP0_Count does:
 * value minus the current cycle count, reassembled on read. The
 * representation must change whenever the counting predicate does, so
 * every write to Status, every exception entry and every ERET folds the
 * live counters to plain values before the mode changes and re-anchors
 * them after - the freeze and thaw pair below.
 */

static uint32_t mips_r10k_perf_cycles(CPUMIPSState *env)
{
    MIPSCPU *cpu = env_archcpu(env);

    return (uint32_t)clock_ns_to_ticks(cpu->clock,
                                       qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

/*
 * The event a control register selects. The field is four bits on the
 * R10000 and five on the R12000 family; each model's writable mask keeps
 * the bits its field lacks at zero, so reading five serves both.
 */
static unsigned mips_r10k_perf_event(uint32_t ctl)
{
    return extract32(ctl, CP0PC_EVENT, 5);
}

/*
 * Whether counter @ctr advances right now: its event is Cycles and its
 * control register enables the current mode. The manual's qualifiers: U,
 * S and K each pair their mode with EXL and ERL clear, the EXL enable
 * wants EXL set with ERL clear, and ERL disables all counting.
 */
static bool mips_r10k_perf_counting(CPUMIPSState *env, unsigned ctr)
{
    uint32_t ctl = env->CP0_Performance[ctr * 2];
    uint32_t st = env->CP0_Status;

    if (mips_r10k_perf_event(ctl) != 0) {
        return false;
    }
    if (st & (1 << CP0St_ERL)) {
        return false;
    }
    if (st & (1 << CP0St_EXL)) {
        return ctl & CP0PC_EXL;
    }
    switch (extract32(st, CP0St_KSU, 2)) {
    case 0:
        return ctl & CP0PC_K;
    case 1:
        return ctl & CP0PC_S;
    case 2:
    case 3:
        /* KSU=3 is implemented as User mode on the R10000 (manual 14.10). */
        return ctl & CP0PC_U;
    default:
        return false;
    }
}

/*
 * True while a mode change can start or stop a counter, which is when the
 * status-write paths must pay for the freeze/thaw pair and the lock
 * around it; with the counters idle - every guest outside a profiler -
 * those paths stay as they were.
 */
bool cpu_mips_r10k_perf_mode_sensitive(CPUMIPSState *env)
{
    unsigned ctr;

    /* No deadline timer, no counters: cpu_mips_r10k_perf_clock_init(). */
    if (!env->r10k_perf_timer) {
        return false;
    }
    for (ctr = 0; ctr < env->cfg.performance_counter_pairs; ctr++) {
        uint32_t ctl = env->CP0_Performance[ctr * 2];

        if (mips_r10k_perf_event(ctl) == 0 &&
            (ctl & (CP0PC_U | CP0PC_S | CP0PC_K | CP0PC_EXL))) {
            return true;
        }
    }
    return false;
}

uint32_t cpu_mips_get_r10k_perf(CPUMIPSState *env, unsigned sel)
{
    uint32_t v = env->CP0_Performance[sel];

    if ((sel & 1) && mips_r10k_perf_counting(env, sel >> 1)) {
        v += mips_r10k_perf_cycles(env);
    }
    return v;
}

/* The counters' contribution to IP[7]: bit 31 up with the enable on. */
bool cpu_mips_r10k_perf_ip7(CPUMIPSState *env)
{
    unsigned ctr;

    if (!env->cfg.performance_counter_interrupts) {
        return false;
    }
    for (ctr = 0; ctr < env->cfg.performance_counter_pairs; ctr++) {
        if ((env->CP0_Performance[ctr * 2] & CP0PC_IE) &&
            (cpu_mips_get_r10k_perf(env, ctr * 2 + 1) & 0x80000000u)) {
            return true;
        }
    }
    return false;
}

/*
 * Settle IP[7] and the deadline together, from one reading of the clock.
 * The level and the next edge must come from the same counter value: read
 * twice, a stall between the reads can straddle bit 31 rising, and the
 * deadline then waits for the far edge of a match whose level was never
 * raised. Edges wait only on a counter that is counting with its
 * interrupt enable on: bit 31 rising as the count crosses the top half,
 * falling as it wraps back to zero.
 */
static void mips_r10k_perf_update(CPUMIPSState *env)
{
    MIPSCPU *cpu = env_archcpu(env);
    unsigned irq = extract32(env->CP0_IntCtl, CP0IntCtl_IPTI, 3);
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t cycles = (uint32_t)clock_ns_to_ticks(cpu->clock, now_ns);
    uint64_t next_ns = UINT64_MAX;
    bool level = env->cp0_timer_ip7;
    unsigned ctr;

    /* R14000 counter interrupt behavior remains outside the cleared model. */
    if (!env->cfg.performance_counter_interrupts) {
        timer_del(env->r10k_perf_timer);
        qemu_set_irq(env->irq[irq], level);
        return;
    }

    for (ctr = 0; ctr < env->cfg.performance_counter_pairs; ctr++) {
        bool counting = mips_r10k_perf_counting(env, ctr);
        uint32_t v = env->CP0_Performance[ctr * 2 + 1];
        uint64_t wait;

        if (counting) {
            v += cycles;
        }
        if (!(env->CP0_Performance[ctr * 2] & CP0PC_IE)) {
            continue;
        }
        if (v & 0x80000000u) {
            level = true;
            wait = (1ULL << 32) - v;
        } else {
            wait = 0x80000000u - v;
        }
        if (counting) {
            next_ns = MIN(next_ns,
                          now_ns + clock_ticks_to_ns(cpu->clock, wait));
        }
    }
    if (env->r10k_perf_timer) {
        if (next_ns == UINT64_MAX) {
            timer_del(env->r10k_perf_timer);
        } else {
            timer_mod(env->r10k_perf_timer, next_ns);
        }
    }
    qemu_set_irq(env->irq[irq], level);
}

void cpu_mips_store_r10k_perf(CPUMIPSState *env, uint32_t val, unsigned sel)
{
    BQL_LOCK_GUARD();

    if (sel & 1) {
        /* A counter, stored as a bias while it is counting. */
        if (mips_r10k_perf_counting(env, sel >> 1)) {
            env->CP0_Performance[sel] = val - mips_r10k_perf_cycles(env);
        } else {
            env->CP0_Performance[sel] = val;
        }
    } else {
        /*
         * A control register: its counter's representation follows the
         * counting predicate across the change.
         */
        if (mips_r10k_perf_counting(env, sel >> 1)) {
            env->CP0_Performance[sel + 1] =
                (uint32_t)env->CP0_Performance[sel + 1] +
                mips_r10k_perf_cycles(env);
        }
        env->CP0_Performance[sel] =
            val & env->cfg.performance_control_rw_bitmask;
        if (mips_r10k_perf_counting(env, sel >> 1)) {
            env->CP0_Performance[sel + 1] =
                (uint32_t)env->CP0_Performance[sel + 1] -
                mips_r10k_perf_cycles(env);
        }
    }
    mips_r10k_perf_update(env);
}

/*
 * The pair around every mode change. Freeze folds each live counter to a
 * plain value before the change; thaw re-anchors whichever counters count
 * in the new mode and refreshes the interrupt line and the deadline.
 * Callers hold the big lock across the pair whenever
 * cpu_mips_r10k_perf_mode_sensitive() said the pair is needed, so the
 * deadline callback cannot read a counter mid-fold.
 */
void cpu_mips_r10k_perf_freeze(CPUMIPSState *env)
{
    unsigned ctr;

    if (!env->r10k_perf_timer) {
        return;
    }
    for (ctr = 0; ctr < env->cfg.performance_counter_pairs; ctr++) {
        if (mips_r10k_perf_counting(env, ctr)) {
            env->CP0_Performance[ctr * 2 + 1] =
                (uint32_t)env->CP0_Performance[ctr * 2 + 1] +
                mips_r10k_perf_cycles(env);
        }
    }
}

void cpu_mips_r10k_perf_thaw(CPUMIPSState *env)
{
    unsigned ctr;

    if (!env->r10k_perf_timer) {
        return;
    }
    for (ctr = 0; ctr < env->cfg.performance_counter_pairs; ctr++) {
        if (mips_r10k_perf_counting(env, ctr)) {
            env->CP0_Performance[ctr * 2 + 1] =
                (uint32_t)env->CP0_Performance[ctr * 2 + 1] -
                mips_r10k_perf_cycles(env);
        }
    }
    mips_r10k_perf_update(env);
}

static void mips_r10k_perf_cb(void *opaque)
{
    CPUMIPSState *env = opaque;

    mips_r10k_perf_update(env);
}

void cpu_mips_r10k_perf_post_load(CPUMIPSState *env)
{
    if (!env->r10k_perf_timer) {
        return;
    }
    if (env->cfg.performance_counter_pairs == 4) {
        /* Re-anchor migrated R14000 logical values to the destination clock. */
        cpu_mips_r10k_perf_thaw(env);
    } else {
        /* The established R10000 stream stores its running clock biases. */
        mips_r10k_perf_update(env);
    }
}

void cpu_mips_r10k_perf_clock_init(MIPSCPU *cpu)
{
    CPUMIPSState *env = &cpu->env;

    if (!env->cfg.performance_counter_pairs) {
        return;
    }
    env->r10k_perf_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        &mips_r10k_perf_cb, env);
}
