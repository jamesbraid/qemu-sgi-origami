/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the real IOMMU notifier registration API without caching DMA. */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"

#define TYPE_SGI_IOMMU_NOTIFIER_TEST "sgi-iommu-notifier-test"
OBJECT_DECLARE_SIMPLE_TYPE(SGINotifierTestState, SGI_IOMMU_NOTIFIER_TEST)

struct SGINotifierTestState {
    SysBusDevice parent_obj;
    MemoryRegion *iommu;
    uint32_t flags;
    int32_t result;
};

static void notifier_event(IOMMUNotifier *notifier, IOMMUTLBEntry *entry)
{
}

static void notifier_realize(DeviceState *dev, Error **errp)
{
    SGINotifierTestState *s = SGI_IOMMU_NOTIFIER_TEST(dev);
    Error *local_err = NULL;
    IOMMUNotifier notifier;

    if (!s->iommu || !s->flags || (s->flags & ~IOMMU_NOTIFIER_ALL)) {
        error_setg(errp, "Notifier test needs an IOMMU and flags 1 through 7");
        return;
    }
    iommu_notifier_init(&notifier, notifier_event, s->flags,
                        0, UINT64_MAX, 0);
    s->result = memory_region_register_iommu_notifier(s->iommu, &notifier,
                                                     &local_err);
    error_free(local_err);
    if (!s->result) {
        memory_region_unregister_iommu_notifier(s->iommu, &notifier);
    }
}

static void notifier_get_result(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    SGINotifierTestState *s = SGI_IOMMU_NOTIFIER_TEST(obj);

    visit_type_int32(v, name, &s->result, errp);
}

static void notifier_init(Object *obj)
{
    SGINotifierTestState *s = SGI_IOMMU_NOTIFIER_TEST(obj);

    object_property_add_link(obj, "iommu", TYPE_IOMMU_MEMORY_REGION,
                             (Object **)&s->iommu,
                             qdev_prop_allow_set_link_before_realize, 0);
    object_property_add(obj, "result", "int32", notifier_get_result,
                        NULL, NULL, NULL);
}

static const Property notifier_properties[] = {
    DEFINE_PROP_UINT32("flags", SGINotifierTestState, flags, 1),
};

static const VMStateDescription vmstate_notifier = {
    .name = TYPE_SGI_IOMMU_NOTIFIER_TEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(result, SGINotifierTestState),
        VMSTATE_END_OF_LIST()
    },
};

static void notifier_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = notifier_realize;
    dc->user_creatable = true;
    dc->vmsd = &vmstate_notifier;
    device_class_set_props(dc, notifier_properties);
}

static const TypeInfo notifier_type = {
    .name = TYPE_SGI_IOMMU_NOTIFIER_TEST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGINotifierTestState),
    .instance_init = notifier_init,
    .class_init = notifier_class_init,
};

static void notifier_register_types(void)
{
    type_register_static(&notifier_type);
}

type_init(notifier_register_types)
