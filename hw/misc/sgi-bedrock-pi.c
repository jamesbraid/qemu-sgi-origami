/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock-pi.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

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
#define PI_ERROR_SET         0x3f8
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
     * A full 55-bit revolution exceeds an int64 nanosecond deadline.
     * Wake at the largest safe intermediate point and recompute.
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
 * Bank-0 bits 5 and 6 are each the coherency-pending result for A or B --
 * set while any masked bit of that region word is -- OR'd with the pending
 * bit itself, so a directly-set source is not overwritten. An XBridge head
 * routes its own error interrupt onto bit 5 or 6 through the modify register,
 * and only bank-1's reflect bits are immune to a modify write, not these.
 */
static uint64_t pi_pending0(SGIBedrockPIState *s)
{
    uint64_t coherency =
        ((s->cc_pend[0] & s->cc_pend_mask) ? 1ULL << 5 : 0) |
        ((s->cc_pend[1] & s->cc_pend_mask) ? 1ULL << 6 : 0);

    return s->pending[0] | coherency;
}

void sgi_bedrock_pi_recompute(SGIBedrockPIState *s)
{
    uint64_t pending0 = pi_pending0(s);
    uint64_t pending1 = pi_pending1(s);

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

static void pi_modify(SGIBedrockPIState *s, uint64_t value)
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
    pi_modify(s, value);
    return true;
}

/*
 * Bit 0 is the reading processor's slot within this interface and bit 1 is
 * the interface itself, so a processor learns both from one read. The slot
 * has to come from whoever issued the access: a shared value cannot tell A
 * from B, and the firmware's rendezvous depends on the difference.
 *
 * With no issuing processor -- a qtest, whose accesses carry none, or an
 * access from outside this node's processors -- the model answers slot A.
 * That is model behaviour rather than a hardware claim: real silicon always
 * has a requester.
 */
static unsigned pi_population_mask(SGIBedrockPIState *s)
{
    return s->processor_mask ? s->processor_mask :
        (1U << MIN(s->processor_count, 4U)) - 1;
}

static uint64_t pi_cpu_number(SGIBedrockPIState *s)
{
    unsigned logical = s->cpu_base;
    unsigned mask = pi_population_mask(s);

    for (unsigned physical = 0; physical < 4; physical++) {
        if (!(mask & (1U << physical))) {
            continue;
        }
        if (current_cpu && physical / 2 == s->interface &&
            current_cpu->cpu_index == logical) {
            return (physical & 1) | ((uint64_t)s->interface << 1);
        }
        logical++;
    }
    return (uint64_t)s->interface << 1;
}

static uint64_t pi_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIBedrockPIState *s = opaque;
    unsigned mask = pi_population_mask(s);

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
    case PI_PENDING_0: return pi_pending0(s);
    case PI_PENDING_1: return pi_pending1(s);
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
    case PI_ERROR_SET:
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
    case PI_RT_COUNTER: return pi_rt_count(s);
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

static void pi_write(void *opaque, hwaddr offset, uint64_t value,
                     unsigned size)
{
    SGIBedrockPIState *s = opaque;

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
        s->cached_alias = value & 0xf;
        return;
    /*
     * The coherency request-buffer timeout: the maximum count a valid
     * buffer may reach, and the scale that sets how often that count
     * advances. The model runs no request-buffer timeout, so both are
     * stored control words read back with their reserved bits zero.
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
     * The header ties a store here to a per-processor NMI, but no public
     * source gives the trigger predicate, and the value the stock PROM writes
     * is demonstrably non-triggering: it stores 0x100 to both offsets,
     * fire-and-forget, through the remote register window during per-node
     * bring-up and never reads them back. A MIPS NMI vectors to the reset
     * vector, so a store that self-NMI'd the running processor would reset-loop
     * the single-node PROM, which instead boots. The model therefore delivers
     * no CPU NMI from an ordinary MMIO store; a modelled NMI reaches the
     * processor over the s->nmi line from the external L1 controller (its nmi
     * command). The store is kept observable and otherwise inert.
     */
    case PI_NMI_A:
    case PI_NMI_B:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-pi: NMI register 0x%" HWADDR_PRIx
                      " store of 0x%" PRIx64 " delivers no NMI; the L1 "
                      "controller drives the processor's NMI line\n",
                      offset, value);
        return;
    case PI_PENDING_MODIFY:
    case PI_PENDING_ALIAS:
        pi_modify(s, value);
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
        s->mask[0][0] = value;
        break;
    case PI_MASK_1_A:
        s->mask[0][1] = value;
        break;
    case PI_MASK_0_B:
        s->mask[1][0] = value;
        break;
    case PI_MASK_1_B:
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
        s->rt_compare[0] = value & PI_RT_MASK;
        pi_arm_rt(s, 0);
        return;
    case PI_RT_COMPARE_B:
        s->rt_compare[1] = value & PI_RT_MASK;
        pi_arm_rt(s, 1);
        return;
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
    case PI_ERROR_SET:
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
     * system bus never negatively acknowledges a processor transaction, so
     * an enabled counter has nothing to count: it holds its programmed value,
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

static const MemoryRegionOps pi_ops = {
    .read = pi_read,
    .write = pi_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void pi_reset_enter(Object *obj, ResetType type)
{
    SGIBedrockPIState *s = SGI_BEDROCK_PI(obj);

    s->pending[0] = 0;
    s->pending[1] = 0;
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
    pi_drive_cpu_enable(s);
    return 0;
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

static const VMStateDescription vmstate_pi = {
    .name = TYPE_SGI_BEDROCK_PI,
    .version_id = 5,
    .minimum_version_id = 5,
    .pre_save = pi_pre_save,
    .post_load = pi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(cpu_protect, SGIBedrockPIState),
        VMSTATE_UINT64(io_protect, SGIBedrockPIState),
        VMSTATE_UINT64(region_present, SGIBedrockPIState),
        VMSTATE_UINT8(cached_alias, SGIBedrockPIState),
        VMSTATE_UINT8(rq_timeout_max, SGIBedrockPIState),
        VMSTATE_UINT32(rq_timeout_scale, SGIBedrockPIState),
        VMSTATE_UINT8_ARRAY(cpu_enable, SGIBedrockPIState, 2),
        VMSTATE_UINT64_ARRAY(pending, SGIBedrockPIState, 2),
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
        VMSTATE_BOOL_ARRAY(rt_timer_match, SGIBedrockPIState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static const Property pi_properties[] = {
    DEFINE_PROP_UINT32("interface", SGIBedrockPIState, interface, 0),
    DEFINE_PROP_UINT32("cpu-base", SGIBedrockPIState, cpu_base, 0),
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
    for (unsigned cpu = 0; cpu < 2; cpu++) {
        s->rt_line[cpu].pi = s;
        s->rt_line[cpu].cpu = cpu;
        s->rt_timer[cpu] = timer_new_ns(QEMU_CLOCK_VIRTUAL, pi_rt_expire,
                                        &s->rt_line[cpu]);
    }
    s->profile_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pi_profile_expire, s);
    memory_region_init_io(&s->mmio, obj, &pi_ops, s, TYPE_SGI_BEDROCK_PI,
                          SGI_BEDROCK_PI_SIZE);
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
