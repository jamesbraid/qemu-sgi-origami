/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock-pi.h"
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
#define PI_RT_COUNTER        0x30100

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
    uint64_t reflected = (s->lb_error ? 1ULL << 55 : 0) |
                         (s->ni_error ? 1ULL << 62 : 0);

    return (s->pending[1] & ~PI_REFLECT_MASK) | reflected;
}

void sgi_bedrock_pi_recompute(SGIBedrockPIState *s)
{
    uint64_t pending1 = pi_pending1(s);

    for (unsigned cpu = 0; cpu < 2; cpu++) {
        unsigned base = cpu * SGI_BEDROCK_PI_HW_IRQS;
        uint64_t bank0 = s->pending[0] & s->mask[cpu][0];
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

static uint64_t pi_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIBedrockPIState *s = opaque;

    switch (offset) {
    case PI_CPU_PROTECT: return s->cpu_protect;
    case PI_IO_PROTECT: return s->io_protect;
    case PI_REGION_PRESENT: return s->region_present;
    case PI_CPU_NUMBER: return s->interface << 1;
    case PI_CACHED_ALIAS: return s->cached_alias;
    case PI_CPU_PRESENT_A:
        return s->interface * 2 < s->processor_count;
    case PI_CPU_PRESENT_B:
        return s->interface * 2 + 1 < s->processor_count;
    case PI_CPU_ENABLE_A: return s->cpu_enable[0];
    case PI_CPU_ENABLE_B: return s->cpu_enable[1];
    case PI_PENDING_0: return s->pending[0];
    case PI_PENDING_1: return pi_pending1(s);
    case PI_MASK_0_A: return s->mask[0][0];
    case PI_MASK_1_A: return s->mask[0][1];
    case PI_MASK_0_B: return s->mask[1][0];
    case PI_MASK_1_B: return s->mask[1][1];
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
    case PI_CPU_ENABLE_A:
        s->cpu_enable[0] = value & 1;
        return;
    case PI_CPU_ENABLE_B:
        s->cpu_enable[1] = value & 1;
        return;
    case PI_NMI_A:
        qemu_irq_pulse(s->nmi[0]);
        return;
    case PI_NMI_B:
        qemu_irq_pulse(s->nmi[1]);
        return;
    case PI_PENDING_MODIFY:
    case PI_PENDING_ALIAS:
        pi_modify(s, value);
        return;
    case PI_PENDING_0:
    case PI_PENDING_1:
    case PI_CPU_NUMBER:
    case PI_CPU_PRESENT_A:
    case PI_CPU_PRESENT_B:
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
    memset(s->mask, 0, sizeof(s->mask));
    s->remap = 0;
    memset(s->error_mask, 0, sizeof(s->error_mask));
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
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = pi_pre_save,
    .post_load = pi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(cpu_protect, SGIBedrockPIState),
        VMSTATE_UINT64(io_protect, SGIBedrockPIState),
        VMSTATE_UINT64(region_present, SGIBedrockPIState),
        VMSTATE_UINT8(cached_alias, SGIBedrockPIState),
        VMSTATE_UINT8_ARRAY(cpu_enable, SGIBedrockPIState, 2),
        VMSTATE_UINT64_ARRAY(pending, SGIBedrockPIState, 2),
        VMSTATE_UINT64_2DARRAY(mask, SGIBedrockPIState, 2, 2),
        VMSTATE_UINT8(remap, SGIBedrockPIState),
        VMSTATE_UINT64(error_pending, SGIBedrockPIState),
        VMSTATE_UINT64_ARRAY(error_mask, SGIBedrockPIState, 2),
        VMSTATE_UINT64_ARRAY(error_stack, SGIBedrockPIState, 2),
        VMSTATE_UINT8(error_stack_size, SGIBedrockPIState),
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
    DEFINE_PROP_UINT32("processor-count", SGIBedrockPIState,
                       processor_count, 1),
    DEFINE_PROP_UINT32("request-region", SGIBedrockPIState, request_region, 0),
};

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
