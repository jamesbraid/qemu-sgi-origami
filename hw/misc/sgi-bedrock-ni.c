/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bedrock NI, composed by the parent without owning node reset policy. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock-ni.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void ni_update_irq(SGIBedrockNIState *s)
{
    qemu_set_irq(s->error, !!(s->regs.errors & 0x1fffffff000000ULL));
}

static uint64_t ni_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIBedrockNIState *s = opaque;
    uint64_t value = 0;

    if (!sgi_bedrock_ni_regs_read(&s->regs, offset, &value)) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-ni: unimplemented read at "
                      "0x%" HWADDR_PRIx "\n", offset);
    }
    ni_update_irq(s);
    return value;
}

static void ni_write(void *opaque, hwaddr offset, uint64_t value,
                     unsigned size)
{
    SGIBedrockNIState *s = opaque;

    switch (sgi_bedrock_ni_regs_write(&s->regs, offset, value)) {
    case SGI_BEDROCK_NI_LOCAL_RESET:
        qemu_irq_pulse(s->local_reset);
        break;
    case SGI_BEDROCK_NI_UNIMPLEMENTED:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-ni: unimplemented write at "
                      "0x%" HWADDR_PRIx " value 0x%" PRIx64 "\n",
                      offset, value);
        break;
    case SGI_BEDROCK_NI_READ_ONLY:
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-ni: write to read-only "
                      "register 0x%" HWADDR_PRIx "\n", offset);
        break;
    case SGI_BEDROCK_NI_WRITTEN:
        break;
    }
}

static const MemoryRegionOps ni_ops = {
    .read = ni_read,
    .write = ni_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void ni_reset_enter(Object *obj, ResetType type)
{
    sgi_bedrock_ni_regs_reset(&SGI_BEDROCK_NI(obj)->regs);
}

static void ni_reset_hold(Object *obj, ResetType type)
{
    ni_update_irq(SGI_BEDROCK_NI(obj));
}

static int ni_post_load(void *opaque, int version_id)
{
    ni_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_ni_regs = {
    .name = "sgi-bedrock-ni/registers",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(control, SGIBedrockNIRegs, 8),
        VMSTATE_UINT64(errors, SGIBedrockNIRegs),
        VMSTATE_UINT64_ARRAY(header, SGIBedrockNIRegs, 2),
        VMSTATE_UINT64(sideband, SGIBedrockNIRegs),
        VMSTATE_UINT8_ARRAY(local_route, SGIBedrockNIRegs, 128),
        VMSTATE_UINT8(global_route, SGIBedrockNIRegs),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_ni = {
    .name = TYPE_SGI_BEDROCK_NI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ni_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(regs, SGIBedrockNIState, 1, vmstate_ni_regs,
                       SGIBedrockNIRegs),
        VMSTATE_END_OF_LIST()
    },
};

static void ni_init(Object *obj)
{
    SGIBedrockNIState *s = SGI_BEDROCK_NI(obj);

    memory_region_init_io(&s->mmio, obj, &ni_ops, s, TYPE_SGI_BEDROCK_NI,
                          SGI_BEDROCK_NI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    qdev_init_gpio_out_named(DEVICE(obj), &s->local_reset, "local-reset", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->error, "error", 1);
}

static void ni_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI Bedrock disconnected network interface";
    dc->vmsd = &vmstate_ni;
    dc->user_creatable = true;
    rc->phases.enter = ni_reset_enter;
    rc->phases.hold = ni_reset_hold;
}

static const TypeInfo ni_info = {
    .name = TYPE_SGI_BEDROCK_NI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBedrockNIState),
    .instance_init = ni_init,
    .class_init = ni_class_init,
};

static void ni_register_types(void)
{
    type_register_static(&ni_info);
}

type_init(ni_register_types)
