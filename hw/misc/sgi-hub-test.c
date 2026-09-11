/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host-only error injection for Hub register-contract qtests. */

#include "qemu/osdep.h"
#include "hw/misc/sgi-hub.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"

#define TYPE_SGI_HUB_TEST "sgi-hub-test"
OBJECT_DECLARE_SIMPLE_TYPE(SGIHubTestState, SGI_HUB_TEST)

struct SGIHubTestState {
    DeviceState parent_obj;
    SGIHubState *hub;
};

static void test_set(Object *obj, Visitor *v, const char *name,
                     void *opaque, Error **errp)
{
    SGIHubTestState *s = SGI_HUB_TEST(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (!s->hub) {
        error_setg(errp, "hub link is not set");
        return;
    }
    sgi_hub_latch_port_error(s->hub, value);
}

static void test_init(Object *obj)
{
    object_property_add(obj, "port-errors", "uint64", NULL, test_set,
                        NULL, NULL);
}

static const Property test_properties[] = {
    DEFINE_PROP_LINK("hub", SGIHubTestState, hub, TYPE_SGI_HUB, SGIHubState *),
    DEFINE_PROP_END_OF_LIST(),
};

static void test_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, test_properties);
}

static const TypeInfo test_type = {
    .name = TYPE_SGI_HUB_TEST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SGIHubTestState),
    .instance_init = test_init,
    .class_init = test_class_init,
};

static void test_register_types(void)
{
    type_register_static(&test_type);
}

type_init(test_register_types)
