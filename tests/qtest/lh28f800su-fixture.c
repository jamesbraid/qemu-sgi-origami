/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Map the x16 chip without choosing an IP30 board or Bridge wiring. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "system/qtest.h"

#define TYPE_FLASH_TEST "lh28f800su-test-device"
OBJECT_DECLARE_SIMPLE_TYPE(FlashTestState, FLASH_TEST)

struct FlashTestState {
    DeviceState parent_obj;
};

static void flash_test_realize(DeviceState *dev, Error **errp)
{
    DeviceState *flash;

    if (!qtest_enabled()) {
        error_setg(errp, TYPE_FLASH_TEST " requires qtest");
        return;
    }
    flash = qdev_new("lh28f800su");
    object_property_add_child(OBJECT(dev), "chip", OBJECT(flash));
    /* Block 1 starts locked; the other blocks start writable. */
    qdev_prop_set_uint16(flash, "initial-lock-mask", 1u << 1);
    if (!sysbus_realize(SYS_BUS_DEVICE(flash), errp)) {
        object_unref(OBJECT(flash));
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(flash), 0, 0x50000000);
    qemu_set_irq(qdev_get_gpio_in_named(flash, "pins", 0), 1);
    qemu_set_irq(qdev_get_gpio_in_named(flash, "pins", 1), 1);
    qemu_set_irq(qdev_get_gpio_in_named(flash, "pins", 2), 1);
    object_unref(OBJECT(flash));
}

static void flash_test_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = flash_test_realize;
    dc->hotpluggable = false;
}

static const TypeInfo flash_test_type = {
    .name = TYPE_FLASH_TEST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(FlashTestState),
    .class_init = flash_test_class_init,
};

static void flash_test_register_types(void)
{
    type_register_static(&flash_test_type);
}

type_init(flash_test_register_types)
