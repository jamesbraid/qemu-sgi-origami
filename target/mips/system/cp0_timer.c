/*
 * QEMU MIPS timer support
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "internal.h"

/* MIPS R4K timer */
static uint32_t cpu_mips_get_count_val(CPUMIPSState *env)
{
    MIPSCPU *cpu = env_archcpu(env);
    int64_t now_ns;
    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return env->CP0_Count +
            (uint32_t)clock_ns_to_ticks(cpu->count_clock, now_ns);
}

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
 * Whether counter @ctr advances right now: its event is Cycles and its
 * control register enables the current mode. The manual's qualifiers: U,
 * S and K each pair their mode with EXL and ERL clear, the EXL enable
 * wants EXL set with ERL clear, and ERL disables all counting.
 */
static bool mips_r10k_perf_counting(CPUMIPSState *env, unsigned ctr)
{
    uint32_t ctl = env->CP0_Performance[ctr * 2];
    uint32_t st = env->CP0_Status;

    if (((ctl >> CP0PC_EVENT) & 0xf) != 0) {
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

    if (!(env->insn_flags & INSN_R10K)) {
        return false;
    }
    for (ctr = 0; ctr < 2; ctr++) {
        uint32_t ctl = env->CP0_Performance[ctr * 2];

        if (((ctl >> CP0PC_EVENT) & 0xf) == 0 &&
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
static bool mips_r10k_perf_ip7(CPUMIPSState *env)
{
    unsigned ctr;

    for (ctr = 0; ctr < 2; ctr++) {
        if ((env->CP0_Performance[ctr * 2] & CP0PC_IE) &&
            (cpu_mips_get_r10k_perf(env, ctr * 2 + 1) & 0x80000000u)) {
            return true;
        }
    }
    return false;
}

/*
 * Drive the line behind Cause.IP7 with the OR of its two sources, so
 * that neither the timer nor a counter can clear what the other
 * asserted: a Compare write may not take down a counter overflow, and a
 * counter write may not take down a pending timer interrupt - the manual
 * calls both out by name. On processors without the R10000's counters
 * the second source is constantly false and this is the raise or lower
 * the timer always did.
 */
static void cpu_mips_update_ip7(CPUMIPSState *env)
{
    unsigned irq = extract32(env->CP0_IntCtl, CP0IntCtl_IPTI, 3);
    bool level = env->cp0_timer_ip7;

    if (env->insn_flags & INSN_R10K) {
        level = level || mips_r10k_perf_ip7(env);
    }
    qemu_set_irq(env->irq[irq], level);
}

/*
 * Program the deadline for the next IP[7] edge a live counter will
 * produce on its own: bit 31 rising as the count crosses the top half,
 * or falling as it wraps back to zero. Nothing to wait for unless the
 * counter is counting with its interrupt enable on.
 */
static void mips_r10k_perf_deadline(CPUMIPSState *env)
{
    MIPSCPU *cpu = env_archcpu(env);
    uint64_t next_ns = UINT64_MAX;
    unsigned ctr;

    if (!env->r10k_perf_timer) {
        return;
    }
    for (ctr = 0; ctr < 2; ctr++) {
        uint32_t v;
        uint64_t wait, ns;

        if (!(env->CP0_Performance[ctr * 2] & CP0PC_IE) ||
            !mips_r10k_perf_counting(env, ctr)) {
            continue;
        }
        v = cpu_mips_get_r10k_perf(env, ctr * 2 + 1);
        if (v & 0x80000000u) {
            wait = (1ULL << 32) - v;
        } else {
            wait = 0x80000000u - v;
        }
        ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
             clock_ticks_to_ns(cpu->clock, wait);
        next_ns = MIN(next_ns, ns);
    }
    if (next_ns == UINT64_MAX) {
        timer_del(env->r10k_perf_timer);
    } else {
        timer_mod(env->r10k_perf_timer, next_ns);
    }
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
        env->CP0_Performance[sel] = val & CP0PC_MASK;
        if (mips_r10k_perf_counting(env, sel >> 1)) {
            env->CP0_Performance[sel + 1] =
                (uint32_t)env->CP0_Performance[sel + 1] -
                mips_r10k_perf_cycles(env);
        }
    }
    cpu_mips_update_ip7(env);
    mips_r10k_perf_deadline(env);
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

    if (!(env->insn_flags & INSN_R10K)) {
        return;
    }
    for (ctr = 0; ctr < 2; ctr++) {
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

    if (!(env->insn_flags & INSN_R10K)) {
        return;
    }
    for (ctr = 0; ctr < 2; ctr++) {
        if (mips_r10k_perf_counting(env, ctr)) {
            env->CP0_Performance[ctr * 2 + 1] =
                (uint32_t)env->CP0_Performance[ctr * 2 + 1] -
                mips_r10k_perf_cycles(env);
        }
    }
    cpu_mips_update_ip7(env);
    mips_r10k_perf_deadline(env);
}

static void mips_r10k_perf_cb(void *opaque)
{
    CPUMIPSState *env = opaque;

    cpu_mips_update_ip7(env);
    mips_r10k_perf_deadline(env);
}

/*
 * After an incoming migration: the counters and the clock crossed intact,
 * but the deadline timer did not, so a counter that was counting toward an
 * IP[7] edge must be re-armed.
 */
void cpu_mips_r10k_perf_post_load(CPUMIPSState *env)
{
    if (!(env->insn_flags & INSN_R10K)) {
        return;
    }
    cpu_mips_update_ip7(env);
    mips_r10k_perf_deadline(env);
}

static void cpu_mips_timer_update(CPUMIPSState *env)
{
    MIPSCPU *cpu = env_archcpu(env);
    uint64_t now_ns, next_ns;
    uint32_t wait;

    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    wait = env->CP0_Compare - cpu_mips_get_count_val(env);
    /* Clamp interval to overflow if virtual time had not progressed */
    if (!wait) {
        wait = UINT32_MAX;
    }
    next_ns = now_ns + clock_ticks_to_ns(cpu->count_clock, wait);
    timer_mod(env->timer, next_ns);
}

/* Expire the timer.  */
static void cpu_mips_timer_expire(CPUMIPSState *env)
{
    cpu_mips_timer_update(env);
    if (env->insn_flags & ISA_MIPS_R2) {
        cp0_cause_update(env, 0, 1 << CP0Ca_TI);
    }
    env->cp0_timer_ip7 = 1;
    cpu_mips_update_ip7(env);
}

uint32_t cpu_mips_get_count(CPUMIPSState *env)
{
    uint64_t now_ns;

    /*
     * Reading needs no lock. CP0_Count and CP0_Compare are written only
     * by the CPU that owns them, from its own thread, and this runs on
     * that thread. What needs one is the expiry below, which reprograms
     * the timer and raises the interrupt line -- the same work
     * mips_timer_cb() does from the iothread.
     */
    if (qatomic_read(&env->CP0_Cause) & (1 << CP0Ca_DC)) {
        return env->CP0_Count;
    }

    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (timer_pending(env->timer) && timer_expired(env->timer, now_ns)) {
        /* The timer has already expired.  */
        BQL_LOCK_GUARD();

        /*
         * Ask again now the lock is held. The callback may have run
         * while we waited for it, in which case the timer is already
         * reprogrammed and expiring it a second time would deliver an
         * interrupt the guest is not owed.
         */
        now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (timer_pending(env->timer) && timer_expired(env->timer, now_ns)) {
            cpu_mips_timer_expire(env);
        }
    }

    return cpu_mips_get_count_val(env);
}

void cpu_mips_store_count(CPUMIPSState *env, uint32_t count)
{
    /*
     * This gets called from cpu_state_reset(), potentially before timer init.
     * So env->timer may be NULL, so treat timer as disabled in that case.
     */
    MIPSCPU *cpu = env_archcpu(env);

    BQL_LOCK_GUARD();

    if (env->CP0_Cause & (1 << CP0Ca_DC) || !env->timer) {
        env->CP0_Count = count;
    } else {
        /* Store new count register */
        env->CP0_Count = count - (uint32_t)clock_ns_to_ticks(cpu->count_clock,
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        /* Update timer timer */
        cpu_mips_timer_update(env);
    }
}

void cpu_mips_store_compare(CPUMIPSState *env, uint32_t value)
{
    BQL_LOCK_GUARD();

    env->CP0_Compare = value;
    if (!(env->CP0_Cause & (1 << CP0Ca_DC))) {
        cpu_mips_timer_update(env);
    }
    if (env->insn_flags & ISA_MIPS_R2) {
        cp0_cause_update(env, 1 << CP0Ca_TI, 0);
    }
    env->cp0_timer_ip7 = 0;
    cpu_mips_update_ip7(env);
}

void cpu_mips_start_count(CPUMIPSState *env)
{
    cpu_mips_store_count(env, env->CP0_Count);
}

void cpu_mips_stop_count(CPUMIPSState *env)
{
    /* Store the current value */
    MIPSCPU *cpu = env_archcpu(env);
    env->CP0_Count += (uint32_t)clock_ns_to_ticks(cpu->count_clock,
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void mips_timer_cb(void *opaque)
{
    CPUMIPSState *env;

    env = opaque;

    if (env->CP0_Cause & (1 << CP0Ca_DC)) {
        return;
    }

    cpu_mips_timer_expire(env);
}

void cpu_mips_clock_init(MIPSCPU *cpu)
{
    CPUMIPSState *env = &cpu->env;

    env->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &mips_timer_cb, env);
    if (cpu->env.cpu_model->insn_flags & INSN_R10K) {
        env->r10k_perf_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                            &mips_r10k_perf_cb, env);
    }
}
