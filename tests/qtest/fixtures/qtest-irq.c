/* SPDX-License-Identifier: BSD-3-Clause */
/* Host-only GPIO fixture for qtest interception and pulse accounting. */
#include "qemu/osdep.h"
#include "hw/core/qdev.h"
#include "hw/core/irq.h"
#include "qemu/module.h"

#define TYPE_QTEST_IRQ_FIXTURE "qtest-irq-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(QTestIRQFixture, QTEST_IRQ_FIXTURE)

struct QTestIRQFixture {
    DeviceState parent_obj;
    qemu_irq wide_out[512];
    qemu_irq narrow_out[2];
};

static void wide_input(void *opaque, int n, int level)
{
    QTestIRQFixture *s = opaque;

    qemu_set_irq(s->wide_out[n], level);
}

static void narrow_input(void *opaque, int n, int level)
{
    QTestIRQFixture *s = opaque;

    qemu_set_irq(s->narrow_out[n], level);
}

static void fixture_init(Object *obj)
{
    QTestIRQFixture *s = QTEST_IRQ_FIXTURE(obj);
    DeviceState *dev = DEVICE(obj);

    /* Register the smaller input first so it is traversed last. */
    qdev_init_gpio_in_named(dev, narrow_input, "narrow",
                           ARRAY_SIZE(s->narrow_out));
    qdev_init_gpio_in_named(dev, wide_input, "wide", ARRAY_SIZE(s->wide_out));
    qdev_init_gpio_out_named(dev, s->wide_out, "wide-out",
                            ARRAY_SIZE(s->wide_out));
    qdev_init_gpio_out_named(dev, s->narrow_out, "narrow-out",
                            ARRAY_SIZE(s->narrow_out));
}

static const TypeInfo fixture_type = {
    .name = TYPE_QTEST_IRQ_FIXTURE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(QTestIRQFixture),
    .instance_init = fixture_init,
};

static void register_types(void)
{
    type_register_static(&fixture_type);
}

type_init(register_types)
