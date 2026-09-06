/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bedrock internal crossbar: register storage and software error injection. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock-xb.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define XB_PARAMETERS           0x000
#define XB_SLOW_GRANT           0x008
#define XB_SPEW_CONTROL         0x010
#define XB_ARBITRATION_TRIGGER  0x018
#define XB_QUEUE_ERROR_BASE     0x020
#define XB_FIRST_ERROR          0x090
#define XB_DEBUG_DATA           0x098
#define XB_DEBUG_ARBITRATION    0x0a0
#define XB_QUEUE_CLEAR_BASE     0x120
#define XB_FIRST_ERROR_CLEAR    0x190

#define XB_PARAMETERS_RESET         0x0008000080000021ULL
#define XB_PARAMETERS_MASK          0x000ffffffffffff1ULL
#define XB_SLOW_GRANT_MASK          0xffffffffULL
#define XB_SPEW_CONTROL_MASK        0x7ffULL
#define XB_ARBITRATION_TRIGGER_MASK 0xfULL
#define XB_DEBUG_DATA_MASK          0xffffULL
#define XB_DEBUG_ARBITRATION_MASK   0x7ULL

#define XB_FIRST_ERROR_SOURCE_SHIFT 8
#define XB_FIRST_ERROR_SOURCE_MASK  0xfULL
#define XB_FIRST_ERROR_VALID        (1ULL << 12)

/*
 * Queue n lives at XB_QUEUE_ERROR_BASE + 8n with its clear alias at
 * XB_QUEUE_CLEAR_BASE + 8n and reports source 0xf - n. The datasheet gives
 * 0xf for PI0 output and 0x2 for LB input; the values between follow address
 * order, unconfirmed. A zero mask marks a queue whose error layout is
 * unpublished.
 */
static const uint64_t xb_queue_mask[SGI_BEDROCK_XB_QUEUES] = {
    [0]  = 0x3333,  /* PI0 output */
    [1]  = 0x0033,  /* PI0 input */
    [4]  = 0x3330,  /* MD to PI0 output */
    [7]  = 0x00f0,  /* MD input */
    [8]  = 0xfff0,  /* NI output */
    [12] = 0x3303,  /* LB output */
    [13] = 0x0011,  /* LB input */
};

static bool xb_queue_at(hwaddr offset, hwaddr base, unsigned *queue)
{
    if (offset < base || offset >= base + SGI_BEDROCK_XB_QUEUES * 8) {
        return false;
    }
    *queue = (offset - base) / 8;
    return true;
}

static unsigned xb_first_error_queue(SGIBedrockXBState *s)
{
    return 0xf - ((s->first_error >> XB_FIRST_ERROR_SOURCE_SHIFT) &
                  XB_FIRST_ERROR_SOURCE_MASK);
}

static void xb_update_irq(SGIBedrockXBState *s)
{
    qemu_set_irq(s->error, !!(s->first_error & XB_FIRST_ERROR_VALID));
}

static void xb_latch(SGIBedrockXBState *s, unsigned queue)
{
    /* Type and channel are unmeasured; the model records zero. */
    s->first_error = XB_FIRST_ERROR_VALID |
                     ((uint64_t)(0xf - queue) << XB_FIRST_ERROR_SOURCE_SHIFT);
}

static void xb_first_error_clear(SGIBedrockXBState *s)
{
    if (!(s->first_error & XB_FIRST_ERROR_VALID)) {
        return;
    }
    if (s->unserviced & BIT(xb_first_error_queue(s))) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-xb: first error cleared "
                      "before its queue error\n");
        return;
    }
    s->first_error = 0;
    for (unsigned n = 0; n < SGI_BEDROCK_XB_QUEUES; n++) {
        if (s->queue_errors[n]) {
            xb_latch(s, n);
            break;
        }
    }
}

static uint64_t xb_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIBedrockXBState *s = opaque;
    unsigned queue;

    if (xb_queue_at(offset, XB_QUEUE_ERROR_BASE, &queue)) {
        if (!xb_queue_mask[queue]) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-xb: unsupported read of "
                          "queue error register 0x%" HWADDR_PRIx "\n",
                          offset);
            return 0;
        }
        return s->queue_errors[queue];
    }
    if (xb_queue_at(offset, XB_QUEUE_CLEAR_BASE, &queue) ||
        offset == XB_FIRST_ERROR_CLEAR) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-xb: unsupported read of clear "
                      "register 0x%" HWADDR_PRIx "\n", offset);
        return 0;
    }

    switch (offset) {
    case XB_PARAMETERS:
        return s->parameters;
    case XB_SLOW_GRANT:
        return s->slow_grant;
    case XB_SPEW_CONTROL:
        return s->spew_control;
    case XB_ARBITRATION_TRIGGER:
        return s->arbitration_trigger;
    case XB_FIRST_ERROR:
        return s->first_error;
    case XB_DEBUG_DATA:
        return s->debug_data;
    case XB_DEBUG_ARBITRATION:
        return s->debug_arbitration;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-xb: unsupported read at 0x%"
                      HWADDR_PRIx "\n", offset);
        return 0;
    }
}

static void xb_write(void *opaque, hwaddr offset, uint64_t value,
                     unsigned size)
{
    SGIBedrockXBState *s = opaque;
    unsigned queue;

    if (xb_queue_at(offset, XB_QUEUE_ERROR_BASE, &queue)) {
        if (!xb_queue_mask[queue]) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-xb: unsupported write to "
                          "queue error register 0x%" HWADDR_PRIx " value 0x%"
                          PRIx64 "\n", offset, value);
            return;
        }
        value &= xb_queue_mask[queue];
        if (!value) {
            return;
        }
        s->queue_errors[queue] |= value;
        s->unserviced |= BIT(queue);
        if (!(s->first_error & XB_FIRST_ERROR_VALID)) {
            xb_latch(s, queue);
            xb_update_irq(s);
        }
        return;
    }
    if (xb_queue_at(offset, XB_QUEUE_CLEAR_BASE, &queue)) {
        if (!xb_queue_mask[queue]) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-xb: unsupported clear of "
                          "queue error register 0x%" HWADDR_PRIx "\n",
                          offset);
            return;
        }
        s->queue_errors[queue] = 0;
        s->unserviced &= ~BIT(queue);
        return;
    }

    switch (offset) {
    case XB_PARAMETERS:
        s->parameters = value & XB_PARAMETERS_MASK;
        return;
    case XB_SLOW_GRANT:
        s->slow_grant = value & XB_SLOW_GRANT_MASK;
        return;
    case XB_SPEW_CONTROL:
        s->spew_control = value & XB_SPEW_CONTROL_MASK;
        if (s->spew_control) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-xb: traffic observation "
                          "is not modeled\n");
        }
        return;
    case XB_ARBITRATION_TRIGGER:
        s->arbitration_trigger = value & XB_ARBITRATION_TRIGGER_MASK;
        return;
    case XB_FIRST_ERROR:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-xb: write to read-only "
                      "first error register\n");
        return;
    case XB_DEBUG_DATA:
        s->debug_data = value & XB_DEBUG_DATA_MASK;
        if (s->debug_data) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-xb: debug data output is "
                          "not modeled\n");
        }
        return;
    case XB_DEBUG_ARBITRATION:
        s->debug_arbitration = value & XB_DEBUG_ARBITRATION_MASK;
        if (s->debug_arbitration) {
            qemu_log_mask(LOG_UNIMP, "sgi-bedrock-xb: debug arbitration "
                          "output is not modeled\n");
        }
        return;
    case XB_FIRST_ERROR_CLEAR:
        xb_first_error_clear(s);
        xb_update_irq(s);
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-bedrock-xb: unsupported write at 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n", offset, value);
        return;
    }
}

static const MemoryRegionOps xb_ops = {
    .read = xb_read,
    .write = xb_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void xb_reset_enter(Object *obj, ResetType type)
{
    SGIBedrockXBState *s = SGI_BEDROCK_XB(obj);

    s->parameters = XB_PARAMETERS_RESET;
    s->slow_grant = 0;
    s->spew_control = 0;
    s->arbitration_trigger = 0;
    memset(s->queue_errors, 0, sizeof(s->queue_errors));
    s->first_error = 0;
    s->debug_data = 0;
    s->debug_arbitration = 0;
    s->unserviced = 0;
}

static void xb_reset_hold(Object *obj, ResetType type)
{
    xb_update_irq(SGI_BEDROCK_XB(obj));
}

static int xb_post_load(void *opaque, int version_id)
{
    xb_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_xb = {
    .name = TYPE_SGI_BEDROCK_XB,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = xb_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(parameters, SGIBedrockXBState),
        VMSTATE_UINT64(slow_grant, SGIBedrockXBState),
        VMSTATE_UINT64(spew_control, SGIBedrockXBState),
        VMSTATE_UINT64(arbitration_trigger, SGIBedrockXBState),
        VMSTATE_UINT64_ARRAY(queue_errors, SGIBedrockXBState,
                             SGI_BEDROCK_XB_QUEUES),
        VMSTATE_UINT64(first_error, SGIBedrockXBState),
        VMSTATE_UINT64(debug_data, SGIBedrockXBState),
        VMSTATE_UINT64(debug_arbitration, SGIBedrockXBState),
        VMSTATE_UINT16(unserviced, SGIBedrockXBState),
        VMSTATE_END_OF_LIST()
    },
};

static void xb_init(Object *obj)
{
    SGIBedrockXBState *s = SGI_BEDROCK_XB(obj);

    memory_region_init_io(&s->mmio, obj, &xb_ops, s, TYPE_SGI_BEDROCK_XB,
                          SGI_BEDROCK_XB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    qdev_init_gpio_out_named(DEVICE(obj), &s->error, "error", 1);
}

static void xb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI Bedrock internal crossbar";
    dc->vmsd = &vmstate_xb;
    dc->user_creatable = true;
    rc->phases.enter = xb_reset_enter;
    rc->phases.hold = xb_reset_hold;
}

static const TypeInfo xb_info = {
    .name = TYPE_SGI_BEDROCK_XB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBedrockXBState),
    .instance_init = xb_init,
    .class_init = xb_class_init,
};

static void xb_register_types(void)
{
    type_register_static(&xb_info);
}

type_init(xb_register_types)
