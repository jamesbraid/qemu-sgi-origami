/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bedrock local block: bounded register storage and atomic scratch aliases. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock-lb.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define LB_REVISION        0x0000
#define LB_CPU_PERMISSION  0x4000
#define LB_CPU_OVERRIDE    0x4008
#define LB_IO_PERMISSION   0x4010
#define LB_SOFT_RESET      0x4018
#define LB_REGION_PRESENT  0x4020
#define LB_NODES_ABSENT    0x4028
#define LB_MICROLAN        0x4030
#define LB_ERROR_BITS      0x4040
#define LB_ERROR_CLEAR     0x4048
#define LB_ERROR_HEADER_1  0x4050
#define LB_ERROR_HEADER_2  0x4058
#define LB_ERROR_DATA      0x4060
#define LB_DEBUG_SELECT    0x4100
#define LB_DEBUG_PINS      0x4108
#define LB_RTC_LOCAL       0x4200
#define LB_RTC_FILTER      0x4208
#define LB_SCRATCH_BASE    0x8000
#define LB_SCRATCH_WZ_BASE 0x8040
#define LB_SCRATCH_RS_BASE 0x8058
#define LB_VECTOR_ROUTE    0xc008
#define LB_VECTOR_DATA     0xc010

#define LB_PASSWORD 0x53474972756c6573ULL
#define LB_ERROR_MASK 0x7ffULL
#define LB_NODES_ABSENT_MASK 0x80ff80ff80ff80ffULL
#define LB_RTC_LOCAL_MASK (1ULL | (0x3ffULL << 4) | (0x3ffULL << 16) | \
                           (1ULL << 28) | (1ULL << 32))
#define LB_RTC_FILTER_MASK (0x1fULL | (0xfffULL << 8) | (1ULL << 20) | \
                            (0x3ffULL << 24) | (0x3ffULL << 36) | \
                            (0x3ffULL << 48))
#define LB_ERROR_HEADER_1_MASK (0x7fULL | (0x7ffULL << 8) | \
                                (0x7ffULL << 20) | (0xfULL << 32) | \
                                (1ULL << 40) | (1ULL << 44))
#define LB_ERROR_HEADER_2_MASK ((1ULL << 38) - 1)

static uint64_t lb_identity(SGIBedrockLBState *s)
{
    return ((uint64_t)s->manufacturer << 1) |
           ((uint64_t)s->part_number << 12) |
           ((uint64_t)s->revision << 28) |
           ((uint64_t)s->node_number << 32) |
           ((uint64_t)s->region_size << 46);
}

static void lb_update_irq(SGIBedrockLBState *s)
{
    qemu_set_irq(s->error, s->errors != 0);
}

static uint64_t lb_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIBedrockLBState *s = opaque;

    if (offset >= LB_SCRATCH_BASE && offset < LB_SCRATCH_BASE + 5 * 8) {
        return s->scratch[(offset - LB_SCRATCH_BASE) / 8];
    }
    if (offset >= LB_SCRATCH_RS_BASE && offset < LB_SCRATCH_RS_BASE + 2 * 8) {
        unsigned index = 3 + (offset - LB_SCRATCH_RS_BASE) / 8;
        uint64_t old = s->scratch[index];

        s->scratch[index] = 1;
        return old;
    }

    switch (offset) {
    case LB_REVISION:
        return lb_identity(s);
    case LB_CPU_PERMISSION:
        return s->cpu_permission;
    case LB_IO_PERMISSION:
        return s->io_permission;
    case LB_REGION_PRESENT:
        return s->region_present;
    case LB_NODES_ABSENT:
        return s->nodes_absent;
    case LB_ERROR_BITS:
        return s->errors;
    case LB_ERROR_HEADER_1:
        return s->error_header[0];
    case LB_ERROR_HEADER_2:
        return s->error_header[1];
    case LB_ERROR_DATA:
        return s->error_data;
    case LB_DEBUG_SELECT:
        return s->debug_select;
    case LB_RTC_LOCAL:
        return s->rtc_local;
    case LB_RTC_FILTER:
        return s->rtc_filter;
    case LB_VECTOR_ROUTE:
        return s->vector_route;
    case LB_VECTOR_DATA:
        return s->vector_data;
    case LB_CPU_OVERRIDE:
    case LB_SOFT_RESET:
    case LB_ERROR_CLEAR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-bedrock-lb: read from write-only register 0x%"
                      HWADDR_PRIx "\n", offset);
        return 0;
    case LB_MICROLAN:
    case LB_DEBUG_PINS:
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-lb: unsupported read at 0x%"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void lb_write(void *opaque, hwaddr offset, uint64_t value,
                     unsigned size)
{
    SGIBedrockLBState *s = opaque;

    if (offset >= LB_SCRATCH_BASE && offset < LB_SCRATCH_BASE + 5 * 8) {
        unsigned index = (offset - LB_SCRATCH_BASE) / 8;

        s->scratch[index] = index < 3 ? value : value & 1;
        return;
    }
    if (offset >= LB_SCRATCH_WZ_BASE && offset < LB_SCRATCH_WZ_BASE + 3 * 8) {
        unsigned index = (offset - LB_SCRATCH_WZ_BASE) / 8;

        if (s->scratch[index] == 0) {
            s->scratch[index] = value;
        }
        return;
    }

    switch (offset) {
    case LB_REVISION:
        return;
    case LB_CPU_PERMISSION:
        s->cpu_permission = value;
        if (value != UINT64_MAX) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-lb: CPU permission "
                          "enforcement requires requester context\n");
        }
        return;
    case LB_CPU_OVERRIDE:
        if (value == LB_PASSWORD) {
            s->cpu_permission |= 1ULL << s->request_region;
        }
        return;
    case LB_IO_PERMISSION:
        s->io_permission = value;
        if (value != UINT64_MAX) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-lb: I/O permission "
                          "enforcement requires requester context\n");
        }
        return;
    case LB_SOFT_RESET:
        if (value & 1) {
            device_cold_reset(DEVICE(s));
            qemu_irq_pulse(s->soft_reset);
        }
        return;
    case LB_REGION_PRESENT:
        s->region_present = value;
        return;
    case LB_NODES_ABSENT:
        s->nodes_absent = value & LB_NODES_ABSENT_MASK;
        return;
    case LB_ERROR_BITS:
        s->errors |= value & LB_ERROR_MASK;
        lb_update_irq(s);
        return;
    case LB_ERROR_CLEAR:
        s->errors &= ~(value & LB_ERROR_MASK);
        lb_update_irq(s);
        return;
    case LB_ERROR_HEADER_1:
        s->error_header[0] = value & LB_ERROR_HEADER_1_MASK;
        return;
    case LB_ERROR_HEADER_2:
        s->error_header[1] = value & LB_ERROR_HEADER_2_MASK;
        return;
    case LB_ERROR_DATA:
        s->error_data = value;
        return;
    case LB_DEBUG_SELECT:
        s->debug_select = value & 0x3f;
        return;
    case LB_RTC_LOCAL:
        s->rtc_local = value & LB_RTC_LOCAL_MASK;
        return;
    case LB_RTC_FILTER:
        s->rtc_filter = value & LB_RTC_FILTER_MASK;
        return;
    case LB_VECTOR_ROUTE:
        s->vector_route = value;
        return;
    case LB_VECTOR_DATA:
        s->vector_data = value;
        return;
    case LB_MICROLAN:
    case LB_DEBUG_PINS:
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bedrock-lb: unsupported write at 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n", offset, value);
        return;
    }
}

static const MemoryRegionOps lb_ops = {
    .read = lb_read,
    .write = lb_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void lb_reset_enter(Object *obj, ResetType type)
{
    SGIBedrockLBState *s = SGI_BEDROCK_LB(obj);

    s->cpu_permission = UINT64_MAX;
    s->io_permission = UINT64_MAX;
    s->region_present = 0;
    s->nodes_absent = 0;
    s->error_header[0] = 0;
    s->error_header[1] = 0;
    s->error_data = 0;
    s->debug_select = 0;
    s->rtc_local = 0;
    s->rtc_filter = 0;
    s->scratch[0] = 0;
    s->scratch[3] = 0;
    s->scratch[4] = 0;
    s->vector_route = 0;
    s->vector_data = 0;
}

static void lb_reset_hold(Object *obj, ResetType type)
{
    lb_update_irq(SGI_BEDROCK_LB(obj));
}

static int lb_post_load(void *opaque, int version_id)
{
    lb_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_lb = {
    .name = TYPE_SGI_BEDROCK_LB,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = lb_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(cpu_permission, SGIBedrockLBState),
        VMSTATE_UINT64(io_permission, SGIBedrockLBState),
        VMSTATE_UINT64(region_present, SGIBedrockLBState),
        VMSTATE_UINT64(nodes_absent, SGIBedrockLBState),
        VMSTATE_UINT64(errors, SGIBedrockLBState),
        VMSTATE_UINT64_ARRAY(error_header, SGIBedrockLBState, 2),
        VMSTATE_UINT64(error_data, SGIBedrockLBState),
        VMSTATE_UINT64(debug_select, SGIBedrockLBState),
        VMSTATE_UINT64(rtc_local, SGIBedrockLBState),
        VMSTATE_UINT64(rtc_filter, SGIBedrockLBState),
        VMSTATE_UINT64_ARRAY(scratch, SGIBedrockLBState, 5),
        VMSTATE_UINT64(vector_route, SGIBedrockLBState),
        VMSTATE_UINT64(vector_data, SGIBedrockLBState),
        VMSTATE_END_OF_LIST()
    },
};

static void lb_realize(DeviceState *dev, Error **errp)
{
    SGIBedrockLBState *s = SGI_BEDROCK_LB(dev);

    if (s->manufacturer > 0x7ff || s->part_number > 0xffff ||
        s->revision > 0xf || s->node_number > 0xff ||
        s->region_size > 2 || s->request_region > 63) {
        error_setg(errp, "Bedrock local-block identity or region property "
                   "is outside its documented field");
    }
}

static const Property lb_properties[] = {
    DEFINE_PROP_UINT32("manufacturer", SGIBedrockLBState, manufacturer, 0x36),
    DEFINE_PROP_UINT32("part-number", SGIBedrockLBState, part_number, 0x3012),
    DEFINE_PROP_UINT32("revision", SGIBedrockLBState, revision, 2),
    DEFINE_PROP_UINT32("node-number", SGIBedrockLBState, node_number, 0),
    DEFINE_PROP_UINT32("region-size", SGIBedrockLBState, region_size, 0),
    DEFINE_PROP_UINT32("request-region", SGIBedrockLBState, request_region, 0),
};

static void lb_init(Object *obj)
{
    SGIBedrockLBState *s = SGI_BEDROCK_LB(obj);

    memory_region_init_io(&s->mmio, obj, &lb_ops, s, TYPE_SGI_BEDROCK_LB,
                          SGI_BEDROCK_LB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    qdev_init_gpio_out_named(DEVICE(obj), &s->soft_reset, "soft-reset", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->error, "error", 1);
}

static void lb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI Bedrock local block";
    dc->realize = lb_realize;
    dc->vmsd = &vmstate_lb;
    dc->user_creatable = true;
    device_class_set_props(dc, lb_properties);
    rc->phases.enter = lb_reset_enter;
    rc->phases.hold = lb_reset_hold;
}

static const TypeInfo lb_info = {
    .name = TYPE_SGI_BEDROCK_LB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBedrockLBState),
    .instance_init = lb_init,
    .class_init = lb_class_init,
};

static void lb_register_types(void)
{
    type_register_static(&lb_info);
}

type_init(lb_register_types)
