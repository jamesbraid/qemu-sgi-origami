/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/*
 * IA-64 Local SAPIC and interval timer architecture operations.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "exec/cpu-common.h"
#include "arch/arch.h"
#include "hw/ia64/sapic.h"
#include "exec-access.h"
#include "trace.h"

/* ---- Local SAPIC helpers ---- */

static int sapic_find_isr(CPUIA64State *env);
static void ia64_sapic_set_special_work(CPUState *cs, run_on_cpu_data data);

CPUState *ia64_cpu_by_sapic_id(uint8_t id, uint8_t eid)
{
    CPUState *cs;
    uint64_t lid = ia64_sapic_lid(id, eid);

    CPU_FOREACH(cs) {
        IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

        if ((qatomic_read(&cpu->env.cr[IA64_CR_SAPIC_LID]) &
             (IA64_SAPIC_LID_ID_MASK | IA64_SAPIC_LID_EID_MASK)) == lid) {
            return cs;
        }
    }
    return NULL;
}

void ia64_cpu_set_sapic_id(CPUState *cs, uint16_t lid_hi)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    cpu->sapic_id = lid_hi;
    qatomic_set(&cpu->env.cr[IA64_CR_SAPIC_LID], (uint64_t)lid_hi << 16);
}

uint16_t ia64_cpu_sapic_id(CPUState *cs)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    return qatomic_read(&cpu->env.cr[IA64_CR_SAPIC_LID]) >> 16;
}

void ia64_sapic_set_xtp(CPUState *cs, uint8_t xtp)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    qatomic_set(&cpu->env.interrupt.sapic_xtp,
                xtp & IA64_SAPIC_XTP_WRITABLE_MASK);
}

uint8_t ia64_sapic_get_xtp(CPUState *cs)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);

    return qatomic_read(&cpu->env.interrupt.sapic_xtp);
}

static CPUState *sapic_redirect_target(CPUState *requested)
{
    CPUState *cs;
    CPUState *selected = NULL;
    IA64CPU *requested_cpu = ia64_cpu_from_cpu_state(requested);
    uint64_t requested_eid =
        qatomic_read(&requested_cpu->env.cr[IA64_CR_SAPIC_LID]) &
        IA64_SAPIC_LID_EID_MASK;
    unsigned int requested_index = MAX(requested->cpu_index, 0);
    unsigned int best_priority = UINT_MAX;
    uint64_t best_order = UINT64_MAX;

    CPU_FOREACH(cs) {
        IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
        uint8_t xtp = ia64_sapic_get_xtp(cs);
        unsigned int priority;
        unsigned int index;
        uint64_t order;

        if ((qatomic_read(&cpu->env.cr[IA64_CR_SAPIC_LID]) &
             IA64_SAPIC_LID_EID_MASK) != requested_eid ||
            (xtp & IA64_SAPIC_XTP_DISABLE)) {
            continue;
        }
        priority = xtp & IA64_SAPIC_XTP_PRIORITY_MASK;
        index = MAX(cs->cpu_index, 0);
        /* Walk indices from the requested CPU, then wrap to smaller ones. */
        order = ((uint64_t)(index < requested_index) << 32) | index;
        if (priority < best_priority ||
            (priority == best_priority && order < best_order)) {
            selected = cs;
            best_priority = priority;
            best_order = order;
        }
    }

    return selected != NULL ? selected : requested;
}

static void sapic_deliver_to(CPUState *cs, IA64SapicDeliveryMode delivery,
                             uint8_t vector)
{
    if (delivery == IA64_SAPIC_DELIVERY_PMI ||
        delivery == IA64_SAPIC_DELIVERY_INIT) {
        run_on_cpu_data data =
            RUN_ON_CPU_HOST_INT(((unsigned int)delivery << 8) | vector);

        if (qemu_cpu_is_self(cs)) {
            ia64_sapic_set_special_work(cs, data);
        } else {
            async_run_on_cpu(cs, ia64_sapic_set_special_work, data);
        }
    } else {
        ia64_sapic_set_irq(cs, vector);
    }
}

bool ia64_sapic_deliver(IA64SapicDestinationMode destination_mode, uint8_t id,
                        uint8_t eid, bool redirect,
                        IA64SapicDeliveryMode delivery, uint8_t vector)
{
    CPUState *cs;

    /* SAPIC interrupt transactions use physical ID/EID addressing. */
    if (destination_mode != IA64_SAPIC_DESTINATION_PHYSICAL) {
        return false;
    }

    switch (delivery) {
    case IA64_SAPIC_DELIVERY_INT_REDIRECT:
        redirect = true;
        /* fall through */
    case IA64_SAPIC_DELIVERY_INT:
        if (!ia64_external_interrupt_vector_valid(vector)) {
            return false;
        }
        break;
    case IA64_SAPIC_DELIVERY_NMI:
        vector = 2;
        break;
    case IA64_SAPIC_DELIVERY_EXTINT:
        vector = 0;
        break;
    case IA64_SAPIC_DELIVERY_PMI:
        if (vector >= 4) {
            return false;
        }
        break;
    case IA64_SAPIC_DELIVERY_INIT:
        vector = 0;
        break;
    default:
        return false;
    }

    cs = ia64_cpu_by_sapic_id(id, eid);
    if (cs == NULL) {
        return false;
    }
    if (redirect && delivery != IA64_SAPIC_DELIVERY_NMI &&
        delivery != IA64_SAPIC_DELIVERY_EXTINT) {
        cs = sapic_redirect_target(cs);
        sapic_deliver_to(cs, delivery, vector);
        return true;
    }

    sapic_deliver_to(cs, delivery, vector);
    return true;
}

static bool sapic_vector_active(const uint64_t bitmap[4], int vector)
{
    return bitmap[vector / 64] & (1ULL << (vector % 64));
}

static int sapic_vector_priority(int vector)
{
    if (vector == 2) {
        return 257;
    }
    if (vector == 0) {
        return 256;
    }
    if (vector >= 16) {
        return vector;
    }
    return -1;
}

static int sapic_vector_priority_class(int vector)
{
    return vector >= 16 ? vector >> 4 : -1;
}

static bool sapic_vector_unmasked(CPUIA64State *env, int vector)
{
    uint64_t tpr = env->cr[IA64_CR_SAPIC_TPR];
    int isr = sapic_find_isr(env);
    int vector_priority = sapic_vector_priority(vector);
    int vector_class = sapic_vector_priority_class(vector);

    if (vector_priority <= sapic_vector_priority(isr)) {
        return false;
    }

    if (vector == 2) {
        return true;
    }
    if (vector == 0) {
        return !(tpr & IA64_TPR_MMI);
    }
    return !(tpr & IA64_TPR_MMI) &&
           ((uint64_t)vector_class > ((tpr & IA64_TPR_MIC_MASK) >> 4));
}

static int sapic_find_highest_normal(const uint64_t bitmap[4])
{
    int word;

    for (word = 3; word >= 0; word--) {
        uint64_t active = bitmap[word];

        if (word == 0) {
            active &= ~0xffffULL;
        }
        if (active) {
            return word * 64 + 63 - clz64(active);
        }
    }
    return IA64_SPURIOUS_VECTOR;
}

static int sapic_find_irr(CPUIA64State *env)
{
    int vector;

    if (sapic_vector_active(env->interrupt.sapic_irr, 2) &&
        sapic_vector_unmasked(env, 2)) {
        return 2;
    }
    if (sapic_vector_active(env->interrupt.sapic_irr, 0) &&
        sapic_vector_unmasked(env, 0)) {
        return 0;
    }
    vector = sapic_find_highest_normal(env->interrupt.sapic_irr);
    if (vector != IA64_SPURIOUS_VECTOR && sapic_vector_unmasked(env, vector)) {
        return vector;
    }
    return IA64_SPURIOUS_VECTOR;
}

static int sapic_find_isr(CPUIA64State *env)
{
    if (sapic_vector_active(env->interrupt.sapic_isr, 2)) {
        return 2;
    }
    if (sapic_vector_active(env->interrupt.sapic_isr, 0)) {
        return 0;
    }
    return sapic_find_highest_normal(env->interrupt.sapic_isr);
}

void ia64_sapic_update_interrupt(CPUIA64State *env)
{
    CPUState *cs = env_cpu(env);
    bool special =
        (env->interrupt.sapic_init_pending && !(env->psr & IA64_PSR_MC)) ||
        (env->interrupt.sapic_pmi_pending && (env->psr & IA64_PSR_IC));

    if (special || sapic_find_irr(env) != IA64_SPURIOUS_VECTOR) {
        cpu_set_interrupt(cs, CPU_INTERRUPT_HARD);
        ia64_exec_recheck_interrupts(cs);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

bool ia64_sapic_has_pending(CPUIA64State *env)
{
    int vector = sapic_find_irr(env);
    return vector != IA64_SPURIOUS_VECTOR;
}

bool ia64_sapic_has_pmi(CPUIA64State *env)
{
    return env->interrupt.sapic_pmi_pending != 0;
}

bool ia64_sapic_has_init(CPUIA64State *env)
{
    return env->interrupt.sapic_init_pending;
}

int ia64_sapic_accept_pmi(CPUIA64State *env)
{
    uint16_t pending = env->interrupt.sapic_pmi_pending;
    int vector;

    if (!pending) {
        return -1;
    }
    vector = 31 - clz32(pending);
    env->interrupt.sapic_pmi_pending &= ~(1U << vector);
    trace_ia64_sapic_vector(env_cpu(env)->cpu_index, "accept-pmi", vector);
    ia64_sapic_update_interrupt(env);
    return vector;
}

bool ia64_sapic_accept_init(CPUIA64State *env)
{
    if (!env->interrupt.sapic_init_pending) {
        return false;
    }
    env->interrupt.sapic_init_pending = false;
    env->interrupt.sapic_init_reason = 0;
    trace_ia64_sapic_vector(env_cpu(env)->cpu_index, "accept-init", 0);
    ia64_sapic_update_interrupt(env);
    return true;
}

static int ia64_itv_vector(CPUIA64State *env)
{
    uint8_t vector = env->cr[IA64_CR_ITV] & 0xFF;

    return ia64_external_interrupt_vector_valid(vector) ? vector : -1;
}

static bool ia64_itv_masked(CPUIA64State *env)
{
    return env->cr[IA64_CR_ITV] & IA64_VECTOR_MASKED;
}

int ia64_sapic_accept(CPUIA64State *env)
{
    int vector = sapic_find_irr(env);
    if (vector != IA64_SPURIOUS_VECTOR) {
        int idx = vector / 64;
        int bit = vector % 64;
        env->interrupt.sapic_irr[idx] &= ~(1ULL << bit);
        env->interrupt.sapic_isr[idx] |= (1ULL << bit);
        trace_ia64_sapic_vector(env_cpu(env)->cpu_index, "accept", vector);
        ia64_sapic_update_interrupt(env);
    }
    return vector;
}

void ia64_sapic_eoi(CPUIA64State *env)
{
    int vector = sapic_find_isr(env);
    if (vector != IA64_SPURIOUS_VECTOR) {
        int idx = vector / 64;
        int bit = vector % 64;
        env->interrupt.sapic_isr[idx] &= ~(1ULL << bit);
        trace_ia64_sapic_vector(env_cpu(env)->cpu_index, "eoi", vector);
    }
    ia64_sapic_update_interrupt(env);
}

int ia64_sapic_get_ivr(CPUIA64State *env)
{
    return ia64_sapic_accept(env);
}

static void ia64_sapic_set_irq_work(CPUState *cs, run_on_cpu_data data)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    uint8_t vector = data.host_int;
    int idx = vector / 64;
    int bit = vector % 64;

    cpu->env.interrupt.sapic_irr[idx] |= (1ULL << bit);
    trace_ia64_sapic_vector(cs->cpu_index, "raise", vector);
    ia64_sapic_update_interrupt(&cpu->env);
}

void ia64_sapic_set_irq(CPUState *cs, uint8_t vector)
{
    run_on_cpu_data data = RUN_ON_CPU_HOST_INT(vector);

    if (qemu_cpu_is_self(cs)) {
        ia64_sapic_set_irq_work(cs, data);
    } else {
        async_run_on_cpu(cs, ia64_sapic_set_irq_work, data);
    }
}

void ia64_sapic_set_init(CPUState *cs, uint8_t reason)
{
    run_on_cpu_data data = RUN_ON_CPU_HOST_INT(
        ((unsigned int)IA64_SAPIC_DELIVERY_INIT << 8) | reason);

    if (qemu_cpu_is_self(cs)) {
        ia64_sapic_set_special_work(cs, data);
    } else {
        async_run_on_cpu(cs, ia64_sapic_set_special_work, data);
    }
}

static void ia64_sapic_set_special_work(CPUState *cs, run_on_cpu_data data)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    unsigned int encoded = data.host_int;
    IA64SapicDeliveryMode delivery = encoded >> 8;
    uint8_t vector = encoded;

    if (delivery == IA64_SAPIC_DELIVERY_PMI) {
        cpu->env.interrupt.sapic_pmi_pending |= 1U << vector;
        trace_ia64_sapic_vector(cs->cpu_index, "raise-pmi", vector);
    } else {
        g_assert(delivery == IA64_SAPIC_DELIVERY_INIT);
        if (!cpu->env.interrupt.sapic_init_pending ||
            vector > cpu->env.interrupt.sapic_init_reason) {
            cpu->env.interrupt.sapic_init_reason = vector;
        }
        cpu->env.interrupt.sapic_init_pending = true;
        trace_ia64_sapic_vector(cs->cpu_index, "raise-init", 0);
    }
    ia64_sapic_update_interrupt(&cpu->env);
}

/* ITC is the virtual clock at the part's frequency plus a per-CPU offset. */
static uint64_t ia64_itc_ticks_now(CPUIA64State *env)
{
    IA64CPUClass *icc = ia64_env_cpu_class(env);

    return muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                    icc->itc_frequency_hz, NANOSECONDS_PER_SECOND) +
           env->interrupt.itc_offset;
}

uint64_t ia64_itc_peek(CPUIA64State *env)
{
    return ia64_itc_ticks_now(env);
}

static void ia64_itm_fire(CPUIA64State *env)
{
    IA64CPU *cpu = env_archcpu(env);
    int vector = ia64_itv_vector(env);

    env->interrupt.itm_armed = false;
    timer_del(cpu->itm_timer);
    if (vector >= 0 && !ia64_itv_masked(env)) {
        trace_ia64_itm(CPU(cpu)->cpu_index, "fire", ia64_itc_ticks_now(env),
                       env->cr_itm, vector);
        ia64_sapic_set_irq(CPU(cpu), vector);
    }
}

/*
 * Arm the match for a future ITM, given the ITC value to count from. A value
 * the ITC has passed arms nothing, as the SDM says.
 */
static void ia64_itm_arm(CPUIA64State *env, uint64_t itc)
{
    IA64CPU *cpu = env_archcpu(env);
    IA64CPUClass *icc = ia64_env_cpu_class(env);
    int64_t delta = (int64_t)(env->cr_itm - itc);
    int64_t now_ns;
    uint64_t ns;

    env->interrupt.itm_armed = false;
    timer_del(cpu->itm_timer);
    if (ia64_itv_vector(env) < 0 || ia64_itv_masked(env) || delta < 0) {
        return;
    }
    if (delta == 0) {
        ia64_itm_fire(env);
        return;
    }
    now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ns = muldiv64(delta, NANOSECONDS_PER_SECOND, icc->itc_frequency_hz) + 1;
    env->interrupt.itm_armed = true;
    timer_mod(cpu->itm_timer, ns > (uint64_t)INT64_MAX - now_ns ? INT64_MAX
                                                                 : now_ns + ns);
    trace_ia64_itm(CPU(cpu)->cpu_index, "arm", itc, env->cr_itm,
                   ia64_itv_vector(env));
}

void ia64_itm_update(CPUIA64State *env)
{
    ia64_itm_arm(env, ia64_itc_ticks_now(env));
}

static void ia64_itc_set(CPUIA64State *env, uint64_t value)
{
    env->interrupt.itc_offset = 0;
    env->interrupt.itc_offset = value - ia64_itc_ticks_now(env);
}

void ia64_itc_reset(CPUIA64State *env)
{
    ia64_itc_set(env, 0);
}

/* The new ITC is the written value; the time this write took does not count. */
void ia64_itc_write(CPUIA64State *env, uint64_t value)
{
    ia64_itc_set(env, value);
    ia64_itm_arm(env, value);
}

/*
 * Approximation: a read that returns a value at or past an armed ITM raises the
 * interrupt before it returns, so a guest spinning on ar.itc sees it pending.
 */
uint64_t ia64_itc_read(CPUIA64State *env)
{
    uint64_t itc = ia64_itc_ticks_now(env);

    if (env->interrupt.itm_armed && (int64_t)(itc - env->cr_itm) >= 0) {
        ia64_itm_fire(env);
    }
    return itc;
}

static void ia64_itm_timer_work(CPUState *cs, run_on_cpu_data data)
{
    CPUIA64State *env = cpu_env(cs);

    if (env->interrupt.itm_armed && (int64_t)(ia64_itc_ticks_now(env) -
                                              env->cr_itm) >= 0) {
        ia64_itm_fire(env);
    } else {
        ia64_itm_update(env);
    }
}

void ia64_itm_timer_cb(void *opaque)
{
    async_run_on_cpu(CPU(opaque), ia64_itm_timer_work, RUN_ON_CPU_NULL);
}

/*
 * A halted processor wakes for any interrupt that would be unmasked by TPR
 * and the vectors in service, whatever psr.i says (SDM Vol. 2, 11.5).
 */
bool ia64_cpu_has_work(CPUState *cs)
{
    IA64CPU *cpu = ia64_cpu_from_cpu_state(cs);
    CPUIA64State *env = &cpu->env;

    return !env->checkstopped && cpu_test_interrupt(cs, CPU_INTERRUPT_HARD) &&
           (ia64_sapic_has_pending(env) ||
            (env->interrupt.sapic_pmi_pending && (env->psr & IA64_PSR_IC)) ||
            (env->interrupt.sapic_init_pending && !(env->psr & IA64_PSR_MC)));
}

void ia64_cpu_halt(CPUIA64State *env)
{
    CPUState *cs = env_cpu(env);

    cs->halted = 1;
    cs->exception_index = EXCP_HLT;
    cpu_exit(cs);
}
