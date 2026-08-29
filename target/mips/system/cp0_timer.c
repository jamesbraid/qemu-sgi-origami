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
static uint32_t cpu_mips_count_at(CPUMIPSState *env, int64_t now_ns)
{
    MIPSCPU *cpu = env_archcpu(env);

    return env->CP0_Count +
            (uint32_t)clock_ns_to_ticks(cpu->count_clock, now_ns);
}

static uint32_t cpu_mips_get_count_val(CPUMIPSState *env)
{
    return cpu_mips_count_at(env, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
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

    if (env->r10k_perf_timer) {
        level = level || cpu_mips_r10k_perf_ip7(env);
    }
    qemu_set_irq(env->irq[irq], level);
}

/*
 * Deadline and count must come from the same clock sample. Two separate
 * reads leave a window in which the counter crosses a just-written
 * Compare: classified as armed, then armed a full wrap out.
 */
static void cpu_mips_timer_update(CPUMIPSState *env, int64_t now_ns)
{
    MIPSCPU *cpu = env_archcpu(env);
    uint64_t next_ns;
    uint32_t wait;

    wait = env->CP0_Compare - cpu_mips_count_at(env, now_ns);
    /* Clamp interval to overflow if virtual time had not progressed */
    if (!wait) {
        wait = UINT32_MAX;
    }
    next_ns = now_ns + clock_ticks_to_ns(cpu->count_clock, wait);
    timer_mod(env->timer, next_ns);
}

/* Expire the timer.  */
static void cpu_mips_timer_expire(CPUMIPSState *env, int64_t now_ns)
{
    cpu_mips_timer_update(env, now_ns);
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
            cpu_mips_timer_expire(env, now_ns);
        }
    }

    return cpu_mips_get_count_val(env);
}

/*
 * A write that leaves Count at or past Compare. The manual (14.8) makes the
 * comparison continuous - "Whenever their values equal, the interrupt bit
 * IP[7] in the Cause register is set" - so equality standing at the moment
 * of the write raises the interrupt at once. The expiry path must not come
 * here: at expiry the registers are also equal, but that match has just been
 * delivered, and cpu_mips_timer_update()'s zero-interval clamp correctly
 * defers the next one a full wrap - which is also the deadline both write
 * paths have already programmed for the match after this one.
 *
 * The window treats a Compare write up to half the counter space behind
 * Count as the match it just missed. That corner needs a stall between
 * adjacent instructions longer than the arming distance - unreachable on
 * silicon, routine on a loaded host - and deferring it costs the guest a
 * full counter wrap of its clock. Count writes keep strict equality.
 */
static bool cpu_mips_write_matches(CPUMIPSState *env, uint32_t count,
                                   uint32_t compare, uint32_t window)
{
    if (!env->cfg.compare_match_is_continuous ||
        (uint32_t)(count - compare) > window) {
        return false;
    }
    if (env->insn_flags & ISA_MIPS_R2) {
        cp0_cause_update(env, 0, 1 << CP0Ca_TI);
    }
    env->cp0_timer_ip7 = 1;
    return true;
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
        int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        /* Store new count register */
        env->CP0_Count = count - (uint32_t)clock_ns_to_ticks(cpu->count_clock,
                                                             now_ns);
        /* Update timer timer */
        cpu_mips_timer_update(env, now_ns);
        if (cpu_mips_write_matches(env, count, env->CP0_Compare, 0)) {
            cpu_mips_update_ip7(env);
        }
    }
}

void cpu_mips_store_compare(CPUMIPSState *env, uint32_t value)
{
    bool matched = false;

    BQL_LOCK_GUARD();

    env->CP0_Compare = value;
    if (!(env->CP0_Cause & (1 << CP0Ca_DC))) {
        int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        /* Classify and arm against one instant, or a stall between the
         * two reads turns an imminent match into a full-wrap deferral. */
        matched = cpu_mips_write_matches(env, cpu_mips_count_at(env, now_ns),
                                         value, UINT32_MAX >> 1);
        cpu_mips_timer_update(env, now_ns);
    }
    if (matched) {
        /* The write's own reset of IP[7] loses to the standing match. */
        cpu_mips_update_ip7(env);
        return;
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

    cpu_mips_timer_expire(env, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

void cpu_mips_clock_init(MIPSCPU *cpu)
{
    CPUMIPSState *env = &cpu->env;

    env->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &mips_timer_cb, env);
    cpu_mips_r10k_perf_clock_init(cpu);
}
