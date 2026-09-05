/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"

static void bedrock_forward_irq(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    qemu_set_irq(s->hardware_irq[n], level);
}

static void bedrock_forward_nmi(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    qemu_set_irq(s->nmi[n], level);
}

static void bedrock_lb_error(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    sgi_bedrock_pi_set_lb_error(&s->pi[0], level);
    sgi_bedrock_pi_set_lb_error(&s->pi[1], level);
}

static void bedrock_ni_error(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    sgi_bedrock_pi_set_ni_error(&s->pi[0], level);
    sgi_bedrock_pi_set_ni_error(&s->pi[1], level);
}

static void bedrock_local_reset(void *opaque, int n, int level)
{
    SGIBedrockState *s = opaque;

    if (level) {
        device_cold_reset(DEVICE(&s->pi[0]));
        device_cold_reset(DEVICE(&s->pi[1]));
        device_cold_reset(DEVICE(&s->lb));
        device_cold_reset(DEVICE(&s->ni));
    }
}

static void bedrock_realize(DeviceState *dev, Error **errp)
{
    SGIBedrockState *s = SGI_BEDROCK(dev);

    if (s->processor_count > 4) {
        error_setg(errp, "Bedrock processor count exceeds four slots");
        return;
    }
    for (unsigned i = 0; i < 2; i++) {
        qdev_prop_set_uint32(DEVICE(&s->pi[i]), "interface", i);
        qdev_prop_set_uint32(DEVICE(&s->pi[i]), "processor-count",
                            s->processor_count);
        if (!qdev_realize(DEVICE(&s->pi[i]), sysbus_get_default(), errp)) {
            return;
        }
        memory_region_add_subregion(&s->mmio, i * 0x200000,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->pi[i]), 0));
        for (unsigned line = 0;
             line < SGI_BEDROCK_PI_PROCESSORS * SGI_BEDROCK_PI_HW_IRQS;
             line++) {
            qdev_connect_gpio_out_named(DEVICE(&s->pi[i]), "hardware-irq",
                                        line,
                qdev_get_gpio_in_named(dev, "pi-hardware-irq",
                    i * SGI_BEDROCK_PI_PROCESSORS *
                    SGI_BEDROCK_PI_HW_IRQS + line));
        }
        for (unsigned cpu = 0; cpu < SGI_BEDROCK_PI_PROCESSORS; cpu++) {
            qdev_connect_gpio_out_named(DEVICE(&s->pi[i]), "nmi", cpu,
                qdev_get_gpio_in_named(dev, "pi-nmi",
                    i * SGI_BEDROCK_PI_PROCESSORS + cpu));
        }
    }
    if (!qdev_realize(DEVICE(&s->lb), sysbus_get_default(), errp)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->ni), sysbus_get_default(), errp)) {
        return;
    }
    memory_region_add_subregion(&s->mmio, 0x600000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->lb), 0));
    memory_region_add_subregion(&s->mmio, 0x680000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->ni), 0));
    qdev_connect_gpio_out_named(DEVICE(&s->lb), "error", 0,
        qdev_get_gpio_in_named(dev, "lb-error", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->lb), "soft-reset", 0,
        qdev_get_gpio_in_named(dev, "local-reset", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->ni), "error", 0,
        qdev_get_gpio_in_named(dev, "ni-error", 0));
    qdev_connect_gpio_out_named(DEVICE(&s->ni), "local-reset", 0,
        qdev_get_gpio_in_named(dev, "local-reset", 0));
}

static const Property bedrock_properties[] = {
    DEFINE_PROP_UINT32("processor-count", SGIBedrockState,
                       processor_count, 1),
};

static void bedrock_init(Object *obj)
{
    SGIBedrockState *s = SGI_BEDROCK(obj);
    DeviceState *dev = DEVICE(obj);

    object_initialize_child(obj, "pi0", &s->pi[0], TYPE_SGI_BEDROCK_PI);
    object_initialize_child(obj, "pi1", &s->pi[1], TYPE_SGI_BEDROCK_PI);
    object_initialize_child(obj, "lb", &s->lb, TYPE_SGI_BEDROCK_LB);
    object_initialize_child(obj, "ni", &s->ni, TYPE_SGI_BEDROCK_NI);
    memory_region_init(&s->mmio, obj, TYPE_SGI_BEDROCK, SGI_BEDROCK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    qdev_init_gpio_out_named(dev, s->hardware_irq, "hardware-irq",
                             ARRAY_SIZE(s->hardware_irq));
    qdev_init_gpio_out_named(dev, s->nmi, "nmi", ARRAY_SIZE(s->nmi));
    qdev_init_gpio_in_named(dev, bedrock_forward_irq, "pi-hardware-irq",
                            ARRAY_SIZE(s->hardware_irq));
    qdev_init_gpio_in_named(dev, bedrock_forward_nmi, "pi-nmi",
                            ARRAY_SIZE(s->nmi));
    qdev_init_gpio_in_named(dev, bedrock_lb_error, "lb-error", 1);
    qdev_init_gpio_in_named(dev, bedrock_ni_error, "ni-error", 1);
    qdev_init_gpio_in_named(dev, bedrock_local_reset, "local-reset", 1);
}

static void bedrock_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "SGI Bedrock node controller";
    dc->realize = bedrock_realize;
    dc->user_creatable = true;
    device_class_set_props(dc, bedrock_properties);
}

static const TypeInfo bedrock_info = {
    .name = TYPE_SGI_BEDROCK,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBedrockState),
    .instance_init = bedrock_init,
    .class_init = bedrock_class_init,
};

static void bedrock_register_types(void)
{
    type_register_static(&bedrock_info);
}

type_init(bedrock_register_types)
