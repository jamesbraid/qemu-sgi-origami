/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Bedrock processor interface: CPU registers, interrupts and real-time
 * counter.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/misc/sgi-bedrock-pi.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "trace.h"

#define PI_CPU_PROTECT       0x000
#define PI_CPU_OVERRIDE      0x008
#define PI_IO_PROTECT        0x010
#define PI_REGION_PRESENT    0x018
#define PI_CPU_NUMBER        0x020
#define PI_CACHED_ALIAS      0x028
#define PI_RQ_TIMEOUT_MAX    0x030
#define PI_RQ_TIMEOUT_SCALE  0x038
#define PI_CPU_PRESENT_A     0x040
#define PI_CPU_PRESENT_B     0x048
#define PI_CPU_ENABLE_A      0x050
#define PI_CPU_ENABLE_B      0x058
#define PI_NMI_A             0x070
#define PI_NMI_B             0x078
#define PI_PENDING_MODIFY    0x090
#define PI_PENDING_0         0x098
#define PI_PENDING_1         0x0a0
#define PI_MASK_0_A          0x0a8
#define PI_MASK_1_A          0x0b0
#define PI_MASK_0_B          0x0b8
#define PI_MASK_1_B          0x0c0
#define PI_CC_PEND_SET_A     0x0c8
#define PI_CC_PEND_SET_B     0x0d0
#define PI_CC_PEND_CLR_A     0x0d8
#define PI_CC_PEND_CLR_B     0x0e0
#define PI_CC_PEND_MASK      0x0e8
#define PI_REMAP             0x0f0
#define PI_RT_COMPARE_A      0x108
#define PI_RT_COMPARE_B      0x110
#define PI_PROFILE_COMPARE   0x118
#define PI_RT_PENDING_A      0x120
#define PI_RT_PENDING_B      0x128
#define PI_PROFILE_PENDING_A 0x130
#define PI_PROFILE_PENDING_B 0x138
#define PI_RT_ENABLE_A       0x140
#define PI_RT_ENABLE_B       0x148
#define PI_PROFILE_ENABLE_A  0x150
#define PI_PROFILE_ENABLE_B  0x158
#define PI_PENDING_ALIAS     0x180
#define PI_ERR_INT_PEND_WR   0x3f8
#define PI_ERROR_PENDING     0x400
#define PI_ERROR_MASK_A      0x408
#define PI_ERROR_MASK_B      0x410
#define PI_ERROR_STACK_A     0x418
#define PI_ERROR_STACK_B     0x420
#define PI_ERROR_STACK_SIZE  0x428
#define PI_ERROR_STATUS_0_A  0x430
#define PI_ERROR_STATUS_0_A_CLEAR 0x438
#define PI_ERROR_STATUS_1_A  0x440
#define PI_ERROR_STATUS_1_A_CLEAR 0x448
#define PI_ERROR_STATUS_0_B  0x450
#define PI_ERROR_STATUS_0_B_CLEAR 0x458
#define PI_ERROR_STATUS_1_B  0x460
#define PI_ERROR_STATUS_1_B_CLEAR 0x468
#define PI_ERR_CHECK_ENABLE  0x490
#define PI_NACK_COUNT_A      0x4a8
#define PI_NACK_COUNT_B      0x4b0
#define PI_NACK_COMPARE      0x4b8
#define PI_SPOOL_MASK        0x4c0
#define PI_SPURIOUS_HEADER_0 0x4c8
#define PI_SPURIOUS_HEADER_1 0x4d0
#define PI_RT_COUNTER        0x30100
/*
 * Graphics flow control: ten registers from 0x300, five per slot, whose
 * rules are the shared block's (hw/mips/sgi/pi-gfx.c), and the credit-mode
 * bit in its own page. The count and the return reach the block through
 * the owning node, which knows the issuing slot and reads the node-widget
 * map.
 */
#define PI_GFX_END           (SGI_PI_GFX_BASE + 8 * SGI_PI_GFX_REGS)
#define PI_GFX_CREDIT_MODE   0x20068

#define PI_NACK_COUNT_MASK   ((1ULL << 21) - 1)
#define PI_NACK_COMPARE_MASK ((1ULL << 20) - 1)
#define PI_SPOOL_MASK_MASK   0x3f
#define PI_ERR_CHECK_MASK    0x3f
#define PI_RQ_TIMEOUT_MAX_MASK   0xff
#define PI_RQ_TIMEOUT_SCALE_MASK 0xffffff

#define PI_PASSWORD 0x53474972756c6573ULL
#define PI_ERROR_MASK ((1ULL << 35) - 1)
#define PI_ERROR_MEMORY_UNCORRECTABLE (1ULL << 24)
#define PI_REFLECT_MASK ((1ULL << 54) | (1ULL << 55) | \
                         (1ULL << 62) | (1ULL << 63))
#define PI_RT_MASK ((1ULL << 55) - 1)
#define PI_RT_HZ 1250000
#define PI_RT_TICK_NS (NANOSECONDS_PER_SECOND / PI_RT_HZ)

static uint64_t pi_rt_count(SGIBedrockPIState *s)
{
    int64_t delta = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->rt_base_ns;

    if (delta < 0) {
        return s->rt_base;
    }
    return (s->rt_base + delta / PI_RT_TICK_NS) & PI_RT_MASK;
}

static void pi_arm_rt(SGIBedrockPIState *s, unsigned cpu)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed = MAX(now - s->rt_base_ns, 0);
    uint64_t phase = elapsed % PI_RT_TICK_NS;
    uint64_t ticks = ((s->rt_compare[cpu] - pi_rt_count(s) - 1) &
                      PI_RT_MASK) + 1;
    uint64_t max_ticks = ((uint64_t)(INT64_MAX - now) + phase) /
                         PI_RT_TICK_NS;
    uint64_t chunk;

    /*
     * A full 55-bit revolution exceeds an int64 nanosecond deadline. Wake at
     * the largest safe intermediate point and recompute.
    */
    if (max_ticks == 0) {
        s->rt_timer_match[cpu] = false;
        timer_del(s->rt_timer[cpu]);
        return;
    }
    chunk = MIN(ticks, max_ticks);
    s->rt_timer_match[cpu] = chunk == ticks;
    timer_mod_ns(s->rt_timer[cpu],
                 now - phase + chunk * PI_RT_TICK_NS);
}

static void pi_arm_profile(SGIBedrockPIState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed = MAX(now - s->rt_base_ns, 0);
    uint64_t phase = elapsed % PI_RT_TICK_NS;
    uint32_t count = pi_rt_count(s);
    uint64_t ticks = (uint32_t)(s->profile_compare - count);
    uint64_t max_ticks = ((uint64_t)(INT64_MAX - now) + phase) /
                         PI_RT_TICK_NS;

    if (ticks == 0) {
        ticks = UINT64_C(1) << 32;
    }
    if (ticks > max_ticks) {
        timer_del(s->profile_timer);
        return;
    }
    timer_mod_ns(s->profile_timer,
                 now - phase + ticks * PI_RT_TICK_NS);
}

static uint64_t pi_pending1(SGIBedrockPIState *s)
{
    uint64_t reflected = (s->xb_error ? 1ULL << 54 : 0) |
                         (s->lb_error ? 1ULL << 55 : 0) |
                         (s->ni_error ? 1ULL << 62 : 0);

    return (s->pending[1] & ~PI_REFLECT_MASK) | reflected;
}

/*
 * Bank-0 bits 5 and 6 are each the coherency-pending result for A or B (set
 * while any masked bit of that region word is) OR'd with the pending bit
 * itself, so a directly-set source is not overwritten. An XBridge head routes
 * its own error interrupt onto bit 5 or 6 through the modify register, and
 * only bank-1's reflect bits are immune to a modify write, not these. Bit 4
 * is the L1 controller's UART line, OR'd in the same way.
*/
static uint64_t pi_pending0(SGIBedrockPIState *s)
{
    uint64_t coherency =
        ((s->cc_pend[0] & s->cc_pend_mask) ? 1ULL << 5 : 0) |
        ((s->cc_pend[1] & s->cc_pend_mask) ? 1ULL << 6 : 0);

    return s->pending[0] | coherency | (s->uart_irq ? 1ULL << 4 : 0);
}

/*
 * A pending register is a polled register, and the next change answers that
 * poll without the BQL. Its value is composed from several fields that only
 * change under the BQL: the bank, the coherency-pending words and their mask,
 * the L1 UART line and the reflected error lines. A reader loading them one
 * by one without the lock could combine fields from either side of a change,
 * such as a bank bit already cleared with a coherency bit not yet set, a
 * value the register never held. So every change composes the whole word
 * here and publishes it with one release store, and a read is one acquire
 * load (pi_pending_word()). The PROM passes messages through these bits, so a
 * processor that sees a bit must also see the memory its sender wrote before
 * setting it, which the release and acquire pair provides, as the BQL used
 * to. Every input change is followed by this function under the BQL, reset
 * and an incoming migration included.
 */
static uint64_t pi_pending_word(SGIBedrockPIState *s, unsigned bank)
{
    return qatomic_load_acquire(&s->pending_word[bank]);
}

void sgi_bedrock_pi_recompute(SGIBedrockPIState *s)
{
    uint64_t pending0 = pi_pending0(s);
    uint64_t pending1 = pi_pending1(s);

    qatomic_store_release(&s->pending_word[0], pending0);
    qatomic_store_release(&s->pending_word[1], pending1);

    for (unsigned cpu = 0; cpu < 2; cpu++) {
        unsigned base = cpu * SGI_BEDROCK_PI_HW_IRQS;
        uint64_t bank0 = pending0 & s->mask[cpu][0];
        uint64_t bank1 = pending1 & s->mask[cpu][1];

        qemu_set_irq(s->hardware_irq[base], !!bank0);
        qemu_set_irq(s->hardware_irq[base + 1], !!bank1);
        qemu_set_irq(s->hardware_irq[base + 2],
                     s->rt_pending[cpu] && s->rt_enable[cpu]);
        qemu_set_irq(s->hardware_irq[base + 3],
                     s->profile_pending[cpu] && s->profile_enable[cpu]);
        qemu_set_irq(s->hardware_irq[base + 4],
                     !!(s->error_pending & s->error_mask[cpu]));
    }
}

static void pi_rt_expire(void *opaque)
{
    SGIBedrockPITimerLine *line = opaque;
    SGIBedrockPIState *s = line->pi;

    if (s->rt_timer_match[line->cpu]) {
        s->rt_pending[line->cpu] = 1;
        sgi_bedrock_pi_recompute(s);
    }
    pi_arm_rt(s, line->cpu);
}

static void pi_profile_expire(void *opaque)
{
    SGIBedrockPIState *s = opaque;

    s->profile_pending[0] = 1;
    s->profile_pending[1] = 1;
    sgi_bedrock_pi_recompute(s);
    pi_arm_profile(s);
}

static void pi_rearm_timers(SGIBedrockPIState *s)
{
    pi_arm_rt(s, 0);
    pi_arm_rt(s, 1);
    pi_arm_profile(s);
}

static void pi_drive_cpu_enable(SGIBedrockPIState *s)
{
    for (unsigned cpu = 0; cpu < SGI_BEDROCK_PI_PROCESSORS; cpu++) {
        qemu_set_irq(s->cpu_enable_out[cpu], s->cpu_enable[cpu]);
    }
}

void sgi_bedrock_pi_set_lb_error(SGIBedrockPIState *s, bool level)
{
    s->lb_error = level;
    sgi_bedrock_pi_recompute(s);
}

void sgi_bedrock_pi_set_ni_error(SGIBedrockPIState *s, bool level)
{
    s->ni_error = level;
    sgi_bedrock_pi_recompute(s);
}

void sgi_bedrock_pi_set_xb_error(SGIBedrockPIState *s, bool level)
{
    s->xb_error = level;
    sgi_bedrock_pi_recompute(s);
}

void sgi_bedrock_pi_set_uart_irq(SGIBedrockPIState *s, bool level)
{
    s->uart_irq = level;
    sgi_bedrock_pi_recompute(s);
}

void sgi_bedrock_pi_set_pending(SGIBedrockPIState *s, unsigned bank,
                                unsigned bit)
{
    s->pending[bank] |= 1ULL << bit;
    sgi_bedrock_pi_recompute(s);
}

#define PI_LEVEL_REARM_NS 1000000

static void pi_level_rearm(void *opaque)
{
    SGIBedrockPIState *s = opaque;
    uint64_t active[2] = { 0 };
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t next = INT64_MAX;

    for (unsigned source = 0; source < SGI_BEDROCK_PI_LEVEL_SOURCES; source++) {
        unsigned encoded = s->level_source[source];

        if (encoded) {
            unsigned level = encoded - 1;

            active[level / 64] |= UINT64_C(1) << (level % 64);
        }
    }
    for (unsigned level = 7; level < SGI_BEDROCK_PI_INTERRUPT_LEVELS;
         level++) {
        int64_t due = s->level_rearm_due[level];
        uint64_t bit = UINT64_C(1) << (level % 64);

        if (!due) {
            continue;
        }
        if (due > now) {
            next = MIN(next, due);
            continue;
        }
        s->level_rearm_due[level] = 0;
        s->pending[level / 64] |= active[level / 64] & bit;
    }
    if (next != INT64_MAX) {
        timer_mod_ns(s->level_rearm_timer, next);
    }
    sgi_bedrock_pi_recompute(s);
}

void sgi_bedrock_pi_set_level_source(SGIBedrockPIState *s, unsigned source,
                                     unsigned level, bool active)
{
    uint8_t encoded;

    if (source >= SGI_BEDROCK_PI_LEVEL_SOURCES ||
        (active && (level < 7 || level > 127))) {
        return;
    }
    encoded = active ? level + 1 : 0;
    if (s->level_source[source] != encoded) {
        s->level_source[source] = encoded;
    }
    sgi_bedrock_pi_recompute(s);
}

void sgi_bedrock_pi_set_memory_uncorrectable(SGIBedrockPIState *s)
{
    s->error_pending |= PI_ERROR_MEMORY_UNCORRECTABLE;
    sgi_bedrock_pi_recompute(s);
}

/*
 * ---- Graphics flow control: the stall below the bias ----
 *
 * The SN1 header's credit-counter comment: when the counter is below the
 * bias the processor's write-ready is withheld, a graphics interrupt is
 * sent, and write-ready is then allowed again. Which pending bit that
 * interrupt reaches and how long the processor is held are unknown. The
 * model's choices, inferred:
 *
 * - Every counted store that leaves the counter below the bias raises
 *   first-bank pending bit 1 + slot. The bit is a latch software clears,
 *   so a raise while it is pending changes nothing; a raise after the
 *   processor has cleared it while the counter is still low sends a fresh
 *   interrupt.
 * - That store completes, and the slot is stopped: its next counted store
 *   waits until the processor has taken the interrupt or the counter has
 *   risen above the bias. A processor holding its interrupts off, as a
 *   kernel command-issue site at raised priority does, therefore waits for
 *   the board to return credits. One that takes interrupts takes this one
 *   as the crossing store ends its block, before its next store; a posted
 *   store in the middle of a block waits for credits instead, since the
 *   processor cannot take an interrupt while it is held.
 * - The processor has taken the interrupt when software clears the
 *   pending bit through the pending-modify register or writes the slot's
 *   interrupt counter, which the IRIX 6.5.30 IP35 kernel's graphics
 *   interrupt handler zeroes.
 *
 * The wait releases the big lock, so the board can go on returning
 * credits. It ends early when the machine stops, so that a stop or reset
 * never waits for a board that is stopped with it, and after
 * PI_GFX_STALL_MAX_NS, a bound the model sets because the chip's hold is
 * not known to be unbounded.
 */
#define PI_GFX_STALL_MAX_NS     (1000 * SCALE_MS)
#define PI_GFX_STALL_POLL_MS    5

/* The stop ends: taken, or the counter above the bias. */
static void pi_gfx_release(SGIBedrockPIState *s, unsigned slot,
                           const char *why)
{
    if (s->gfx.stopped[slot]) {
        s->gfx.stopped[slot] = false;
        trace_sgi_bedrock_pi_gfx_release(s->interface, slot, why,
                                         s->gfx.credit[slot]);
        qemu_cond_broadcast(&s->gfx_cond);
    }
}

static void pi_gfx_release_if_above(SGIBedrockPIState *s, unsigned slot)
{
    if (s->gfx.credit[slot] > s->gfx.bias[slot]) {
        pi_gfx_release(s, slot, "credits");
    }
}

/* A counted store waits out the stop of its slot. Called with the BQL. */
static void pi_gfx_stall(SGIBedrockPIState *s, unsigned slot)
{
    int64_t start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    const char *why = "released";

    trace_sgi_bedrock_pi_gfx_stall(s->interface, slot, s->gfx.credit[slot]);
    while (s->gfx.stopped[slot]) {
        if (!current_cpu || qatomic_read(&current_cpu->stop)) {
            why = "machine stop";
            break;
        }
        if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - start >=
            PI_GFX_STALL_MAX_NS) {
            why = "time limit";
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-pi: graphics slot %c "
                          "held for %d ms with the counter at 0x%x and "
                          "the interrupt not taken; letting the store go\n",
                          'A' + slot, (int)(PI_GFX_STALL_MAX_NS / SCALE_MS),
                          s->gfx.credit[slot]);
            s->gfx.stopped[slot] = false;
            break;
        }
        qemu_cond_timedwait_bql(&s->gfx_cond, PI_GFX_STALL_POLL_MS);
    }
    trace_sgi_bedrock_pi_gfx_stalled(s->interface, slot, why,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - start, s->gfx.credit[slot]);
}

/*
 * ---- Graphics flow control: the interrupt counter ----
 *
 * The SN1 header's rules (the shared block, sgi_pi_gfx_int_advance()):
 * one count per clock while the credit counter is at zero (and below a
 * nonzero bias, the model's choice, so an idle slot sends nothing); at
 * the compare
 * the graphics interrupt, and the compare loaded into the counter;
 * zeroed when the credit counter rises above the bias or software writes
 * it. Which pending bit the counter's interrupt reaches is unknown; the
 * datasheet's position, inferred from the IRIX handler that zeroes the
 * counter, is the same bit 1 + slot as the stall's. Which clock it counts
 * is unknown too: the model counts at the real-time counter's 1.25 MHz,
 * its choice, so IRIX's compare of 20000 is 16 ms of machine time.
 */
#define PI_GFX_INT_TICK_NS      PI_RT_TICK_NS

static void pi_gfx_int_sync(SGIBedrockPIState *s, unsigned slot)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / PI_GFX_INT_TICK_NS;
    uint64_t due;

    if (sgi_pi_gfx_int_advance(&s->gfx, slot, now)) {
        trace_sgi_bedrock_pi_gfx_int_compare(s->interface, slot,
                                             s->gfx.int_compare[slot]);
        sgi_bedrock_pi_set_pending(s, 0, 1 + slot);
    }
    due = sgi_pi_gfx_int_due(&s->gfx, slot);
    if (due == UINT64_MAX) {
        timer_del(s->gfx_int_timer[slot]);
    } else {
        timer_mod_ns(s->gfx_int_timer[slot],
                     MIN(due, INT64_MAX / PI_GFX_INT_TICK_NS) *
                     PI_GFX_INT_TICK_NS);
    }
}

static void pi_gfx_int_expire(void *opaque)
{
    SGIBedrockPITimerLine *line = opaque;

    pi_gfx_int_sync(line->pi, line->cpu);
}

/*
 * Around any change to a slot's credit counter, bias or interrupt
 * registers: bring the interrupt counter up to now under the old values,
 * then apply the zero rule and restart it under the new ones.
 */
static bool pi_gfx_before(SGIBedrockPIState *s, unsigned slot)
{
    pi_gfx_int_sync(s, slot);
    return s->gfx.credit[slot] > s->gfx.bias[slot];
}

static void pi_gfx_after(SGIBedrockPIState *s, unsigned slot, bool was_above)
{
    if (!was_above && s->gfx.credit[slot] > s->gfx.bias[slot]) {
        s->gfx.int_count[slot] = 0;
    }
    pi_gfx_int_sync(s, slot);
}

bool sgi_bedrock_pi_gfx_store(SGIBedrockPIState *s, unsigned slot,
                              hwaddr addr)
{
    SGIPIGfx *g = &s->gfx;
    bool above;

    g_assert(slot < SGI_BEDROCK_PI_PROCESSORS);
    if (!sgi_pi_gfx_covers(g, slot, addr)) {
        return false;
    }
    if (g->stopped[slot]) {
        pi_gfx_stall(s, slot);
        /* The page may have moved while the big lock was let go. */
        if (!sgi_pi_gfx_covers(g, slot, addr)) {
            return false;
        }
    }
    above = pi_gfx_before(s, slot);
    sgi_pi_gfx_count_store(g, slot, addr, "sgi-bedrock-pi");
    pi_gfx_after(s, slot, above);
    if (g->credit[slot] < g->bias[slot]) {
        trace_sgi_bedrock_pi_gfx_below_bias(s->interface, slot,
                                            g->credit[slot], g->bias[slot]);
        g->stopped[slot] = true;
        sgi_bedrock_pi_set_pending(s, 0, 1 + slot);
    }
    return true;
}

void sgi_bedrock_pi_gfx_return(SGIBedrockPIState *s, unsigned slot,
                               uint32_t credits)
{
    bool above;

    g_assert(slot < SGI_BEDROCK_PI_PROCESSORS);
    above = pi_gfx_before(s, slot);
    sgi_pi_gfx_add_credits(&s->gfx, slot, credits, "sgi-bedrock-pi");
    pi_gfx_after(s, slot, above);
    pi_gfx_release_if_above(s, slot);
}

static void pi_gfx_write(SGIBedrockPIState *s, unsigned n, uint64_t value)
{
    unsigned slot = n / SGI_PI_GFX_SLOT_REGS;
    bool above = pi_gfx_before(s, slot);

    sgi_pi_gfx_write(&s->gfx, n, value);
    pi_gfx_after(s, slot, above);
    switch (n % SGI_PI_GFX_SLOT_REGS) {
    case SGI_PI_GFX_CREDIT:
    case SGI_PI_GFX_BIAS:
        pi_gfx_release_if_above(s, slot);
        break;
    case SGI_PI_GFX_INT_COUNT:
        pi_gfx_release(s, slot, "counter write");
        break;
    }
}

static void pi_modify(SGIBedrockPIState *s, uint64_t value, bool reissue)
{
    unsigned bank = (value >> 6) & 1;
    unsigned bit = value & 0x3f;
    uint64_t bitmask = 1ULL << bit;

    if (bank && (bitmask & PI_REFLECT_MASK)) {
        return;
    }
    if (value & (1ULL << 8)) {
        s->pending[bank] |= bitmask;
    } else {
        s->pending[bank] &= ~bitmask;
        if (reissue) {
            for (unsigned source = 0; source < SGI_BEDROCK_PI_LEVEL_SOURCES;
                 source++) {
                unsigned source_level = s->level_source[source];

                if (source_level && source_level - 1 == bank * 64 + bit) {
                    unsigned level = bank * 64 + bit;
                    int64_t due = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                  PI_LEVEL_REARM_NS;

                    s->level_rearm_due[level] = due;
                    if (!timer_pending(s->level_rearm_timer) ||
                        timer_expire_time_ns(s->level_rearm_timer) > due) {
                        timer_mod_ns(s->level_rearm_timer, due);
                    }
                    break;
                }
            }
        }
    }
    sgi_bedrock_pi_recompute(s);
}

bool sgi_bedrock_pi_io_modify(SGIBedrockPIState *s, uint64_t value,
                              unsigned region)
{
    if (region >= 64 || !(s->io_protect & (1ULL << region))) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-pi: I/O interrupt write "
                      "0x%" PRIx64 " from region %u denied by I/O protect\n",
                      value, region);
        return false;
    }
    pi_modify(s, value, false);
    if (trace_event_get_state_backends(TRACE_SGI_BEDROCK_PI_IO_MODIFY)) {
        trace_sgi_bedrock_pi_io_modify(
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), s->interface, region,
            value, (value >> 6) & 1, s->pending[(value >> 6) & 1]);
    }
    return true;
}

/*
 * Bit 0 is the reading processor's slot within this interface and bit 1 is
 * the interface itself, so a processor learns both from one read. The slot
 * has to come from whoever issued the access: a shared value cannot tell A
 * from B, and the firmware's rendezvous depends on the difference. With no
 * issuing processor (a qtest, whose accesses carry none, or an access from
 * outside this node's processors), the model answers slot A. That is model
 * behaviour rather than a hardware claim: real silicon always has a
 * requester. Which processor is in which slot comes from the board through
 * slot-a-cpu-index and slot-b-cpu-index. It used to be derived here from the
 * population mask by walking the fitted slots in ascending order and handing
 * them consecutive vCPU indices. That agrees with the board only while at
 * most one slot per interface is fitted; with all four fitted the board puts
 * its second processor on interface 1, the derivation put it in interface 0's
 * second slot, and the processor actually sitting there read its own number
 * as interface 0 slot A.
*/
static unsigned pi_population_mask(SGIBedrockPIState *s)
{
    return s->processor_mask ? s->processor_mask :
        (1U << MIN(s->processor_count, 4U)) - 1;
}

static uint64_t pi_cpu_number(SGIBedrockPIState *s)
{
    if (current_cpu) {
        for (unsigned slot = 0; slot < SGI_BEDROCK_PI_PROCESSORS; slot++) {
            if (s->slot_cpu_index[slot] >= 0 &&
                current_cpu->cpu_index == s->slot_cpu_index[slot]) {
                return slot | ((uint64_t)s->interface << 1);
            }
        }
    }
    return (uint64_t)s->interface << 1;
}

static uint64_t pi_read_locked(void *opaque, hwaddr offset, unsigned size)
{
    SGIBedrockPIState *s = opaque;
    unsigned mask = pi_population_mask(s);

    if (offset >= SGI_PI_GFX_BASE && offset < PI_GFX_END) {
        unsigned n = (offset - SGI_PI_GFX_BASE) / 8;

        /* The interrupt counter reads live. */
        pi_gfx_int_sync(s, n / SGI_PI_GFX_SLOT_REGS);
        return sgi_pi_gfx_read(&s->gfx, n);
    }
    switch (offset) {
    case PI_CPU_PROTECT: return s->cpu_protect;
    case PI_IO_PROTECT: return s->io_protect;
    case PI_REGION_PRESENT: return s->region_present;
    case PI_CPU_NUMBER: return pi_cpu_number(s);
    case PI_CACHED_ALIAS: return s->cached_alias;
    case PI_RQ_TIMEOUT_MAX: return s->rq_timeout_max;
    case PI_RQ_TIMEOUT_SCALE: return s->rq_timeout_scale;
    case PI_CPU_PRESENT_A:
        return !!(mask & (1U << (s->interface * 2)));
    case PI_CPU_PRESENT_B:
        return !!(mask & (1U << (s->interface * 2 + 1)));
    case PI_CPU_ENABLE_A: return s->cpu_enable[0];
    case PI_CPU_ENABLE_B: return s->cpu_enable[1];
    case PI_PENDING_0:
    case PI_PENDING_1: {
        uint64_t value = pi_pending_word(s, offset == PI_PENDING_1);

        trace_sgi_bedrock_pi_pending_read(s->interface,
                                          current_cpu ? current_cpu->cpu_index
                                                      : -1,
                                          offset == PI_PENDING_1, value);
        return value;
    }
    case PI_MASK_0_A: return s->mask[0][0];
    case PI_MASK_1_A: return s->mask[0][1];
    case PI_MASK_0_B: return s->mask[1][0];
    case PI_MASK_1_B: return s->mask[1][1];
    case PI_CC_PEND_SET_A:
    case PI_CC_PEND_CLR_A: return s->cc_pend[0];
    case PI_CC_PEND_SET_B:
    case PI_CC_PEND_CLR_B: return s->cc_pend[1];
    case PI_CC_PEND_MASK: return s->cc_pend_mask;
    case PI_REMAP: return s->remap;
    case PI_RT_COMPARE_A: return s->rt_compare[0];
    case PI_RT_COMPARE_B: return s->rt_compare[1];
    case PI_PROFILE_COMPARE: return s->profile_compare;
    case PI_RT_PENDING_A: return s->rt_pending[0];
    case PI_RT_PENDING_B: return s->rt_pending[1];
    case PI_PROFILE_PENDING_A: return s->profile_pending[0];
    case PI_PROFILE_PENDING_B: return s->profile_pending[1];
    case PI_RT_ENABLE_A: return s->rt_enable[0];
    case PI_RT_ENABLE_B: return s->rt_enable[1];
    case PI_PROFILE_ENABLE_A: return s->profile_enable[0];
    case PI_PROFILE_ENABLE_B: return s->profile_enable[1];
    case PI_ERR_INT_PEND_WR:
    case PI_ERROR_PENDING: return s->error_pending;
    case PI_ERROR_MASK_A: return s->error_mask[0];
    case PI_ERROR_MASK_B: return s->error_mask[1];
    case PI_ERROR_STACK_A: return s->error_stack[0];
    case PI_ERROR_STACK_B: return s->error_stack[1];
    case PI_ERROR_STACK_SIZE: return s->error_stack_size;
    case PI_ERROR_STATUS_0_A:
    case PI_ERROR_STATUS_0_A_CLEAR: return s->error_status[0][0];
    case PI_ERROR_STATUS_1_A:
    case PI_ERROR_STATUS_1_A_CLEAR: return s->error_status[0][1];
    case PI_ERROR_STATUS_0_B:
    case PI_ERROR_STATUS_0_B_CLEAR: return s->error_status[1][0];
    case PI_ERROR_STATUS_1_B:
    case PI_ERROR_STATUS_1_B_CLEAR: return s->error_status[1][1];
    case PI_ERR_CHECK_ENABLE: return s->err_check_enable;
    case PI_NACK_COUNT_A: return s->nack_count[0];
    case PI_NACK_COUNT_B: return s->nack_count[1];
    case PI_NACK_COMPARE: return s->nack_compare;
    case PI_SPOOL_MASK: return s->spool_mask;
    case PI_SPURIOUS_HEADER_0: return s->spurious_header[0];
    case PI_SPURIOUS_HEADER_1: return s->spurious_header[1];
    case PI_GFX_CREDIT_MODE: return s->gfx_credit_mode;
    case PI_RT_COUNTER:
    case PI_RT_COUNTER + 4: {
        uint64_t count = pi_rt_count(s);

        /* Sampled first: a park the report starts cannot change it. */
        if (s->bedrock) {
            sgi_bedrock_rt_counter_read(s->bedrock);
        }
        if (size == 4) {
            return (offset & 4) ? (uint32_t)count : count >> 32;
        }
        return count;
    }
    case PI_CPU_OVERRIDE:
    case PI_NMI_A:
    case PI_NMI_B:
    case PI_PENDING_MODIFY:
    case PI_PENDING_ALIAS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-pi: read from write-only register 0x%"
                      HWADDR_PRIx "\n", offset);
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-pi: unsupported read at 0x%"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void pi_write_locked(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    SGIBedrockPIState *s = opaque;
    int cpu = current_cpu ? current_cpu->cpu_index : -1;

    if (offset >= SGI_PI_GFX_BASE && offset < PI_GFX_END) {
        pi_gfx_write(s, (offset - SGI_PI_GFX_BASE) / 8, value);
        return;
    }
    switch (offset) {
    case PI_CPU_PROTECT:
        s->cpu_protect = value;
        if (value != UINT64_MAX) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-pi: CPU protection "
                          "enforcement requires requester context\n");
        }
        return;
    case PI_CPU_OVERRIDE:
        if (value == PI_PASSWORD) {
            s->cpu_protect |= 1ULL << s->request_region;
        }
        return;
    case PI_IO_PROTECT:
        s->io_protect = value;
        if (value != UINT64_MAX) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-pi: I/O protection "
                          "enforcement requires requester context\n");
        }
        return;
    case PI_REGION_PRESENT:
        s->region_present = value;
        return;
    case PI_CACHED_ALIAS:
        if (s->cached_alias != (value & 0xf)) {
            s->cached_alias = value & 0xf;
            notifier_list_notify(&s->cached_alias_notifiers, s);
        }
        return;
    /*
     * The coherency request-buffer timeout: the maximum count a valid buffer
     * may reach, and the scale that sets how often that count advances. The
     * model runs no request-buffer timeout, so both are stored control words
     * read back with their reserved bits zero.
    */
    case PI_RQ_TIMEOUT_MAX:
        s->rq_timeout_max = value & PI_RQ_TIMEOUT_MAX_MASK;
        return;
    case PI_RQ_TIMEOUT_SCALE:
        s->rq_timeout_scale = value & PI_RQ_TIMEOUT_SCALE_MASK;
        return;
    case PI_CPU_ENABLE_A:
        s->cpu_enable[0] = value & 1;
        qemu_set_irq(s->cpu_enable_out[0], s->cpu_enable[0]);
        return;
    case PI_CPU_ENABLE_B:
        s->cpu_enable[1] = value & 1;
        qemu_set_irq(s->cpu_enable_out[1], s->cpu_enable[1]);
        return;
    /*
     * A store to NMI A or B sends that slot's processor a non-maskable
     * interrupt. The header ties the register to a per-processor NMI but
     * gives no trigger predicate. Linux's SEND_NMI, mainline for the Hub
     * and in the public IP35 series (patch 14/38) for this PI, stores 1 to
     * interrupt another CPU for its crash dump; its one caller builds only
     * with REAL_NMI_SIGNAL, which no tree defines. The model takes bit 0 as
     * the trigger and fires once per store, an approximation: what other
     * bits do, whether a store of 0 retracts anything, and whether a
     * disabled slot takes the NMI are unknown, so a store without bit 0 is
     * logged and otherwise inert.
     */
    case PI_NMI_A:
    case PI_NMI_B:
        if (!(value & 1)) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-pi: NMI register 0x%"
                          HWADDR_PRIx " store of 0x%" PRIx64 " has bit 0 "
                          "clear and delivers no NMI\n", offset, value);
            return;
        }
        trace_sgi_bedrock_pi_nmi(s->interface, offset == PI_NMI_B, value);
        qemu_irq_pulse(s->nmi[offset == PI_NMI_B]);
        return;
    case PI_PENDING_MODIFY:
    case PI_PENDING_ALIAS:
        pi_modify(s, value, true);
        /* A clear of a graphics bit: the processor took the interrupt. */
        if (!(value & 0x140) && (value & 0x3f) >= 1 &&
            (value & 0x3f) <= SGI_BEDROCK_PI_PROCESSORS) {
            pi_gfx_release(s, (value & 0x3f) - 1, "taken");
        }
        if (trace_event_get_state_backends(
                TRACE_SGI_BEDROCK_PI_PENDING_MODIFY)) {
            trace_sgi_bedrock_pi_pending_modify(
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), s->interface, cpu,
                value, (value >> 6) & 1, s->pending[(value >> 6) & 1]);
        }
        return;
    case PI_PENDING_0:
    case PI_PENDING_1:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-pi: write of 0x%"
                      PRIx64 " to read-only pending register 0x%"
                      HWADDR_PRIx "\n", value, offset);
        return;
    case PI_CPU_NUMBER:
    case PI_CPU_PRESENT_A:
    case PI_CPU_PRESENT_B:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-pi: write of 0x%" PRIx64
                      " to hardware-set register 0x%" HWADDR_PRIx
                      " ignored\n", value, offset);
        return;
    case PI_MASK_0_A:
        trace_sgi_bedrock_pi_mask_write(s->interface, offset, value, cpu);
        s->mask[0][0] = value;
        break;
    case PI_MASK_1_A:
        trace_sgi_bedrock_pi_mask_write(s->interface, offset, value, cpu);
        s->mask[0][1] = value;
        break;
    case PI_MASK_0_B:
        trace_sgi_bedrock_pi_mask_write(s->interface, offset, value, cpu);
        s->mask[1][0] = value;
        break;
    case PI_MASK_1_B:
        trace_sgi_bedrock_pi_mask_write(s->interface, offset, value, cpu);
        s->mask[1][1] = value;
        break;
    /*
     * A set store raises the storing processor's region bit, not the data
     * written; the model stands in this interface's request region for that
     * source, as the protection override does. The clear is data-bit
     * write-one-to-clear.
    */
    case PI_CC_PEND_SET_A:
        s->cc_pend[0] |= 1ULL << s->request_region;
        break;
    case PI_CC_PEND_SET_B:
        s->cc_pend[1] |= 1ULL << s->request_region;
        break;
    case PI_CC_PEND_CLR_A:
        s->cc_pend[0] &= ~value;
        break;
    case PI_CC_PEND_CLR_B:
        s->cc_pend[1] &= ~value;
        break;
    case PI_CC_PEND_MASK:
        s->cc_pend_mask = value;
        break;
    case PI_REMAP:
        s->remap = value & 0x1f;
        if (s->remap & 0xf) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-pi: pending bank 1 remap "
                          "0x%x is not routed\n", s->remap & 0xf);
        }
        if (s->remap & 0x10) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-pi: error forwarding to "
                          "the I/O interface is not implemented\n");
        }
        break;
    case PI_RT_COMPARE_A:
    case PI_RT_COMPARE_B: {
        unsigned slot = offset == PI_RT_COMPARE_B;

        s->rt_compare[slot] = value & PI_RT_MASK;
        pi_arm_rt(s, slot);
        if (s->bedrock) {
            sgi_bedrock_rt_compare_written(s->bedrock, s->interface, slot);
        }
        return;
    }
    case PI_PROFILE_COMPARE:
        s->profile_compare = value;
        pi_arm_profile(s);
        return;
    case PI_RT_PENDING_A:
        s->rt_pending[0] = 0;
        break;
    case PI_RT_PENDING_B:
        s->rt_pending[1] = 0;
        break;
    case PI_PROFILE_PENDING_A:
        s->profile_pending[0] = 0;
        break;
    case PI_PROFILE_PENDING_B:
        s->profile_pending[1] = 0;
        break;
    case PI_RT_ENABLE_A:
        s->rt_enable[0] = value & 1;
        break;
    case PI_RT_ENABLE_B:
        s->rt_enable[1] = value & 1;
        break;
    case PI_PROFILE_ENABLE_A:
        s->profile_enable[0] = value & 1;
        break;
    case PI_PROFILE_ENABLE_B:
        s->profile_enable[1] = value & 1;
        break;
    case PI_ERR_INT_PEND_WR:
        s->error_pending =
            (s->error_pending & PI_ERROR_MEMORY_UNCORRECTABLE) |
            (value & PI_ERROR_MASK & ~PI_ERROR_MEMORY_UNCORRECTABLE);
        break;
    case PI_ERROR_PENDING:
        s->error_pending &= ~(value & PI_ERROR_MASK);
        break;
    case PI_ERROR_MASK_A:
        s->error_mask[0] = value & PI_ERROR_MASK;
        break;
    case PI_ERROR_MASK_B:
        s->error_mask[1] = value & PI_ERROR_MASK;
        break;
    case PI_ERROR_STACK_A:
        s->error_stack[0] = value & 0x1fffffff8ULL;
        return;
    case PI_ERROR_STACK_B:
        s->error_stack[1] = value & 0x1fffffff8ULL;
        return;
    case PI_ERROR_STACK_SIZE:
        s->error_stack_size = value & 0xf;
        if (s->error_stack_size) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-pi: error spooling "
                          "(stack size %u) is not implemented\n",
                          s->error_stack_size);
        }
        return;
    case PI_ERROR_STATUS_0_A:
    case PI_ERROR_STATUS_1_A:
    case PI_ERROR_STATUS_0_B:
    case PI_ERROR_STATUS_1_B:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-pi: write of 0x%"
                      PRIx64 " to error status register 0x%" HWADDR_PRIx
                      " ignored\n", value, offset);
        return;
    case PI_ERROR_STATUS_0_A_CLEAR:
    case PI_ERROR_STATUS_1_A_CLEAR:
        s->error_status[0][0] = 0;
        s->error_status[0][1] = 0;
        return;
    case PI_ERROR_STATUS_0_B_CLEAR:
    case PI_ERROR_STATUS_1_B_CLEAR:
        s->error_status[1][0] = 0;
        s->error_status[1][1] = 0;
        return;
    /*
     * System-bus error checking and forwarding: six enable bits (ECC and
     * data-quality generation, SysAD/command parity, state and data-quality
     * checks) over reserved high bits. The model has no bus fabric to apply
     * them to, so it stores the control word and reads the reserved bits back
     * as zero; nothing selects checking off a modelled transaction.
    */
    case PI_ERR_CHECK_ENABLE:
        s->err_check_enable = value & PI_ERR_CHECK_MASK;
        return;
    /*
     * The negative-acknowledge counter holds a count in bits 19:0 and an
     * enable in bit 20; its compare holds a threshold in 19:0. The model's
     * system bus never negatively acknowledges a processor transaction, so an
     * enabled counter has nothing to count: it holds its programmed value,
     * never reaches the compare, and never raises the processor-nack pending
     * bit. That is the correct result for a bus that never retries, not a
     * missing feature, so the write is stored rather than logged unsupported.
    */
    case PI_NACK_COUNT_A:
    case PI_NACK_COUNT_B:
        s->nack_count[offset == PI_NACK_COUNT_B] = value & PI_NACK_COUNT_MASK;
        return;
    case PI_NACK_COMPARE:
        s->nack_compare = value & PI_NACK_COMPARE_MASK;
        return;
    case PI_SPOOL_MASK:
        s->spool_mask = value & PI_SPOOL_MASK_MASK;
        return;
    case PI_SPURIOUS_HEADER_0:
        s->spurious_header[0] = value;
        return;
    case PI_SPURIOUS_HEADER_1:
        s->spurious_header[1] = value;
        return;
    case PI_GFX_CREDIT_MODE:
        /* Kept and read back; the model counts one per store either way. */
        s->gfx_credit_mode = value & 1;
        return;
    case PI_RT_COUNTER:
        s->rt_base = value & PI_RT_MASK;
        s->rt_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        pi_rearm_timers(s);
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bedrock-pi: unsupported write at 0x%"
                      HWADDR_PRIx " value 0x%" PRIx64 "\n", offset, value);
        return;
    }
    sgi_bedrock_pi_recompute(s);
}

/*
 * SGI Performer maps PI_RT_COUNTER into user space as a fast clock and
 * reads it 4 bytes at a time. Allow 4-byte reads for each 32-bit half of the
 * counter, while retaining the 8-byte access contract for writes and all
 * other registers.
 */
static bool pi_accepts(void *opaque, hwaddr addr, unsigned size,
                       bool is_write, MemTxAttrs attrs)
{
    if (is_write) {
        return size == 8;
    }
    if (size == 8) {
        return true;
    }
    if (size == 4 && (addr == PI_RT_COUNTER || addr == PI_RT_COUNTER + 4)) {
        return true;
    }
    return false;
}

/*
 * The register window is lockless (pi_init()), so each access decides
 * whether it needs the BQL. A doubleword read of a pending bank does not:
 * that is the register a waiting processor polls. The stock IP35 PROM's
 * secondary processors spin on both banks for as long as the first one
 * works, and under the lock every poll held up that processor and the main
 * loop. With three processors waiting, the first one's graphics console ran
 * at a fifth of its one-processor speed. A pending read is one acquire
 * load of the word sgi_bedrock_pi_recompute() published, and its remaining
 * state is the reading processor's own (current_cpu, for the trace). Every
 * other access, a pending read made with the BQL already held included,
 * takes the lock as before.
 */
static bool pi_read_is_lockless(hwaddr offset, unsigned size)
{
    return size == 8 && (offset == PI_PENDING_0 || offset == PI_PENDING_1);
}

static uint64_t pi_read(void *opaque, hwaddr offset, unsigned size)
{
    if (!bql_locked() && pi_read_is_lockless(offset, size)) {
        return pi_read_locked(opaque, offset, size);
    }

    BQL_LOCK_GUARD();
    return pi_read_locked(opaque, offset, size);
}

static void pi_write(void *opaque, hwaddr offset, uint64_t value,
                     unsigned size)
{
    BQL_LOCK_GUARD();
    pi_write_locked(opaque, offset, value, size);
}

static const MemoryRegionOps pi_ops = {
    .read = pi_read,
    .write = pi_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
        .accepts = pi_accepts,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void pi_reset_enter(Object *obj, ResetType type)
{
    SGIBedrockPIState *s = SGI_BEDROCK_PI(obj);

    s->pending[0] = 0;
    s->pending[1] = 0;
    memset(s->level_source, 0, sizeof(s->level_source));
    memset(s->level_rearm_due, 0, sizeof(s->level_rearm_due));
    sgi_pi_gfx_reset(&s->gfx);
    timer_del(s->gfx_int_timer[0]);
    timer_del(s->gfx_int_timer[1]);
    s->gfx_credit_mode = 0;
    memset(s->cc_pend, 0, sizeof(s->cc_pend));
    s->cc_pend_mask = 0;
    memset(s->mask, 0, sizeof(s->mask));
    s->remap = 0;
    memset(s->error_mask, 0, sizeof(s->error_mask));
    memset(s->nack_count, 0, sizeof(s->nack_count));
    s->nack_compare = 0;
    s->spool_mask = 0;
    s->err_check_enable = 0;
    s->rq_timeout_max = 0;
    s->rq_timeout_scale = 0;
    s->rt_base = 0;
    s->rt_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    memset(s->rt_compare, 0, sizeof(s->rt_compare));
    s->profile_compare = 0;
    memset(s->rt_pending, 0, sizeof(s->rt_pending));
    memset(s->profile_pending, 0, sizeof(s->profile_pending));
    memset(s->rt_enable, 0, sizeof(s->rt_enable));
    memset(s->profile_enable, 0, sizeof(s->profile_enable));
    timer_del(s->rt_timer[0]);
    timer_del(s->rt_timer[1]);
    timer_del(s->profile_timer);
    timer_del(s->level_rearm_timer);
    memset(s->rt_timer_match, 0, sizeof(s->rt_timer_match));
}

static void pi_reset_hold(Object *obj, ResetType type)
{
    SGIBedrockPIState *s = SGI_BEDROCK_PI(obj);

    sgi_bedrock_pi_recompute(s);
    pi_drive_cpu_enable(s);
    pi_rearm_timers(s);
}

static int pi_post_load(void *opaque, int version_id)
{
    SGIBedrockPIState *s = opaque;

    s->rt_base = s->rt_migrate_count;
    s->rt_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                    s->rt_migrate_phase_ns;
    pi_rearm_timers(s);
    sgi_bedrock_pi_recompute(s);
    for (unsigned slot = 0; slot < SGI_BEDROCK_PI_PROCESSORS; slot++) {
        pi_gfx_int_sync(s, slot);
    }
    pi_drive_cpu_enable(s);
    notifier_list_notify(&s->cached_alias_notifiers, s);
    return 0;
}

void sgi_bedrock_pi_add_cached_alias_notifier(SGIBedrockPIState *s,
                                              Notifier *notifier)
{
    notifier_list_add(&s->cached_alias_notifiers, notifier);
}

static int pi_pre_save(void *opaque)
{
    SGIBedrockPIState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed = MAX(now - s->rt_base_ns, 0);

    s->rt_migrate_count = pi_rt_count(s);
    s->rt_migrate_phase_ns = elapsed % PI_RT_TICK_NS;
    return 0;
}

bool sgi_bedrock_pi_select_pending_route(SGIBedrockPIState *s,
                                         uint32_t origin_count, Error **errp)
{
    if (DEVICE(s)->realized || s->pending_route_selected ||
        (origin_count != 2 && origin_count != 4)) {
        error_setg(errp, "PI pending route requires two/four origins "
                   "and selection before realization");
        return false;
    }
    s->pending_route_selected = 1;
    s->pending_route_origins = origin_count;
    return true;
}

static const VMStateDescription vmstate_pi = {
    .name = TYPE_SGI_BEDROCK_PI,
    .version_id = 7,
    .minimum_version_id = 7,
    .pre_save = pi_pre_save,
    .post_load = pi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(pending_route_selected, SGIBedrockPIState),
        VMSTATE_UINT32_EQUAL(pending_route_origins, SGIBedrockPIState),
        VMSTATE_UINT64(cpu_protect, SGIBedrockPIState),
        VMSTATE_UINT64(io_protect, SGIBedrockPIState),
        VMSTATE_UINT64(region_present, SGIBedrockPIState),
        VMSTATE_UINT8(cached_alias, SGIBedrockPIState),
        VMSTATE_UINT8(rq_timeout_max, SGIBedrockPIState),
        VMSTATE_UINT32(rq_timeout_scale, SGIBedrockPIState),
        VMSTATE_UINT8_ARRAY(cpu_enable, SGIBedrockPIState, 2),
        VMSTATE_UINT64_ARRAY(pending, SGIBedrockPIState, 2),
        VMSTATE_UINT8_ARRAY(level_source, SGIBedrockPIState,
                            SGI_BEDROCK_PI_LEVEL_SOURCES),
        VMSTATE_INT64_ARRAY(level_rearm_due, SGIBedrockPIState,
                            SGI_BEDROCK_PI_INTERRUPT_LEVELS),
        VMSTATE_UINT64_ARRAY(cc_pend, SGIBedrockPIState, 2),
        VMSTATE_UINT64(cc_pend_mask, SGIBedrockPIState),
        VMSTATE_UINT64_2DARRAY(mask, SGIBedrockPIState, 2, 2),
        VMSTATE_UINT8(remap, SGIBedrockPIState),
        VMSTATE_UINT64(error_pending, SGIBedrockPIState),
        VMSTATE_UINT64_ARRAY(error_mask, SGIBedrockPIState, 2),
        VMSTATE_UINT64_ARRAY(error_stack, SGIBedrockPIState, 2),
        VMSTATE_UINT8(error_stack_size, SGIBedrockPIState),
        VMSTATE_UINT64_2DARRAY(error_status, SGIBedrockPIState, 2, 2),
        VMSTATE_UINT64_ARRAY(nack_count, SGIBedrockPIState, 2),
        VMSTATE_UINT32(nack_compare, SGIBedrockPIState),
        VMSTATE_UINT8(spool_mask, SGIBedrockPIState),
        VMSTATE_UINT8(err_check_enable, SGIBedrockPIState),
        VMSTATE_UINT64_ARRAY(spurious_header, SGIBedrockPIState, 2),
        VMSTATE_UINT64(rt_base, SGIBedrockPIState),
        VMSTATE_INT64(rt_base_ns, SGIBedrockPIState),
        VMSTATE_UINT64(rt_migrate_count, SGIBedrockPIState),
        VMSTATE_UINT16(rt_migrate_phase_ns, SGIBedrockPIState),
        VMSTATE_UINT64_ARRAY(rt_compare, SGIBedrockPIState, 2),
        VMSTATE_UINT32(profile_compare, SGIBedrockPIState),
        VMSTATE_UINT8_ARRAY(rt_pending, SGIBedrockPIState, 2),
        VMSTATE_UINT8_ARRAY(profile_pending, SGIBedrockPIState, 2),
        VMSTATE_UINT8_ARRAY(rt_enable, SGIBedrockPIState, 2),
        VMSTATE_UINT8_ARRAY(profile_enable, SGIBedrockPIState, 2),
        VMSTATE_TIMER_PTR_ARRAY(rt_timer, SGIBedrockPIState, 2),
        VMSTATE_TIMER_PTR(profile_timer, SGIBedrockPIState),
        VMSTATE_TIMER_PTR(level_rearm_timer, SGIBedrockPIState),
        VMSTATE_BOOL_ARRAY(rt_timer_match, SGIBedrockPIState, 2),
        VMSTATE_STRUCT(gfx, SGIBedrockPIState, 0, vmstate_sgi_pi_gfx, SGIPIGfx),
        VMSTATE_UINT8(gfx_credit_mode, SGIBedrockPIState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property pi_properties[] = {
    DEFINE_PROP_UINT32("interface", SGIBedrockPIState, interface, 0),
    DEFINE_PROP_INT32("slot-a-cpu-index", SGIBedrockPIState,
                      slot_cpu_index[0], -1),
    DEFINE_PROP_INT32("slot-b-cpu-index", SGIBedrockPIState,
                      slot_cpu_index[1], -1),
    DEFINE_PROP_UINT32("processor-count", SGIBedrockPIState,
                       processor_count, 1),
    DEFINE_PROP_UINT32("processor-mask", SGIBedrockPIState,
                       processor_mask, 0),
    DEFINE_PROP_UINT8("cpu-enable-initial", SGIBedrockPIState,
                      cpu_enable_initial, 0),
    DEFINE_PROP_UINT32("request-region", SGIBedrockPIState, request_region, 0),
};

static void pi_realize(DeviceState *dev, Error **errp)
{
    SGIBedrockPIState *s = SGI_BEDROCK_PI(dev);

    (void)errp;
    s->cpu_enable[0] = s->cpu_enable_initial & 1;
    s->cpu_enable[1] = (s->cpu_enable_initial >> 1) & 1;
}

static void pi_init(Object *obj)
{
    SGIBedrockPIState *s = SGI_BEDROCK_PI(obj);

    s->cpu_protect = UINT64_MAX;
    s->io_protect = UINT64_MAX;
    s->region_present = UINT64_MAX;
    notifier_list_init(&s->cached_alias_notifiers);
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        s->rt_line[cpu].pi = s;
        s->rt_line[cpu].cpu = cpu;
        s->rt_timer[cpu] = timer_new_ns(QEMU_CLOCK_VIRTUAL, pi_rt_expire,
                                        &s->rt_line[cpu]);
    }
    s->profile_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pi_profile_expire, s);
    s->level_rearm_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pi_level_rearm, s);
    qemu_cond_init(&s->gfx_cond);
    for (unsigned slot = 0; slot < SGI_BEDROCK_PI_PROCESSORS; slot++) {
        s->gfx_int_line[slot].pi = s;
        s->gfx_int_line[slot].cpu = slot;
        s->gfx_int_timer[slot] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                              pi_gfx_int_expire,
                                              &s->gfx_int_line[slot]);
    }
    memory_region_init_io(&s->mmio, obj, &pi_ops, s, TYPE_SGI_BEDROCK_PI,
                          SGI_BEDROCK_PI_SIZE);
    /*
     * Only a polled pending read goes without the BQL (pi_read()). This also
     * drops the memory core's reentrancy guard, which only matters for a
     * device whose access can start one that comes back into it. No access
     * here does: a write changes registers, timers and interrupt lines and
     * starts no transfer.
     */
    memory_region_enable_lockless_io(&s->mmio);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    qdev_init_gpio_out_named(DEVICE(obj), s->hardware_irq, "hardware-irq",
                             ARRAY_SIZE(s->hardware_irq));
    qdev_init_gpio_out_named(DEVICE(obj), s->nmi, "nmi",
                             ARRAY_SIZE(s->nmi));
    qdev_init_gpio_out_named(DEVICE(obj), s->cpu_enable_out, "cpu-enable",
                             ARRAY_SIZE(s->cpu_enable_out));
}

static void pi_finalize(Object *obj)
{
    SGIBedrockPIState *s = SGI_BEDROCK_PI(obj);

    timer_free(s->rt_timer[0]);
    timer_free(s->rt_timer[1]);
    timer_free(s->profile_timer);
    timer_free(s->level_rearm_timer);
    qemu_cond_destroy(&s->gfx_cond);
    timer_free(s->gfx_int_timer[0]);
    timer_free(s->gfx_int_timer[1]);
}

static void pi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI Bedrock processor interface";
    dc->realize = pi_realize;
    dc->vmsd = &vmstate_pi;
    dc->user_creatable = false;
    device_class_set_props(dc, pi_properties);
    rc->phases.enter = pi_reset_enter;
    rc->phases.hold = pi_reset_hold;
}

static const TypeInfo pi_info = {
    .name = TYPE_SGI_BEDROCK_PI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBedrockPIState),
    .instance_init = pi_init,
    .instance_finalize = pi_finalize,
    .class_init = pi_class_init,
};

static void pi_register_types(void)
{
    type_register_static(&pi_info);
}

type_init(pi_register_types)
