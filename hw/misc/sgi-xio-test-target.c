/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only widget-local RAM with a separate CPU observation alias. */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/units.h"

#define TYPE_SGI_XIO_TEST_TARGET "sgi-xio-test-target"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXioTestTargetState, SGI_XIO_TEST_TARGET)

struct SGIXioTestTargetState {
    SysBusDevice parent_obj;
    MemoryRegion ram;
    MemoryRegion alias;
    uint64_t size;
};

static void target_realize(DeviceState *dev, Error **errp)
{
    SGIXioTestTargetState *s = SGI_XIO_TEST_TARGET(dev);
    g_autofree char *name = g_strdup_printf("%s.ram",
        object_get_canonical_path_component(OBJECT(dev)));

    if (!s->size || s->size > 16 * MiB) {
        error_setg(errp, "Crosstalk test RAM size must be 1 byte to 16 MiB");
        return;
    }
    if (!memory_region_init_ram(&s->ram, OBJECT(s), name, s->size, errp)) {
        return;
    }
    memory_region_init_alias(&s->alias, OBJECT(s), "cpu-alias", &s->ram,
                             0, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->alias);
}

static const Property target_properties[] = {
    DEFINE_PROP_SIZE("size", SGIXioTestTargetState, size, 64 * KiB),
};

static const VMStateDescription vmstate_target = {
    .name = TYPE_SGI_XIO_TEST_TARGET,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void target_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = target_realize;
    dc->user_creatable = true;
    dc->vmsd = &vmstate_target;
    device_class_set_props(dc, target_properties);
}

static const TypeInfo target_type = {
    .name = TYPE_SGI_XIO_TEST_TARGET,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIXioTestTargetState),
    .class_init = target_class_init,
};

static void target_register_types(void)
{
    type_register_static(&target_type);
}

type_init(target_register_types)
