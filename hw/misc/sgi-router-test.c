/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host-only qtest fixture for a Router without a machine fabric. QOM controls
 * call the real vector target. They are not guest registers or fabric replies.
 */
#include "qemu/osdep.h"
#include "hw/misc/sgi-router.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"

#define TYPE_SGI_ROUTER_TEST "sgi-router-test"
OBJECT_DECLARE_SIMPLE_TYPE(SGIRouterTestState, SGI_ROUTER_TEST)

struct SGIRouterTestState {
    DeviceState parent_obj;
    DeviceState *router;
    uint64_t address;
    uint64_t port;
    uint64_t exchange;
};

static bool test_port(SGIRouterTestState *s, Error **errp)
{
    if (s->port < 1 || s->port > SGI_ROUTER_PORTS) {
        error_setg(errp, "test arrival port must be 1 through 6");
        return false;
    }
    return true;
}

static bool test_access(SGIRouterTestState *s, SN0FabricVectorOp op,
                         uint64_t data, uint64_t *value, Error **errp)
{
    SN0FabricVectorAccess access = {
        .op = op,
        .addr = s->address,
        .data = data,
    };

    if (!test_port(s, errp)) {
        return false;
    }
    if (!sgi_router_access(SGI_ROUTER(s->router), s->port, &access)) {
        error_setg(errp, "Router target refused the test request");
        return false;
    }
    *value = access.value;
    return true;
}

static void test_get(Object *obj, Visitor *v, const char *name,
                      void *opaque, Error **errp)
{
    SGIRouterTestState *s = SGI_ROUTER_TEST(obj);
    uint64_t value;

    if (!strcmp(name, "register")) {
        if (!test_access(s, SN0_FABRIC_VECTOR_READ, 0, &value, errp)) {
            return;
        }
    } else if (!strcmp(name, "exchange")) {
        value = s->exchange;
    } else {
        if (!test_port(s, errp)) {
            return;
        }
        value = sgi_router_port_enabled(SGI_ROUTER(s->router), s->port) |
            (sgi_router_port_eight_bit_forced(SGI_ROUTER(s->router), s->port)
             << 1);
    }
    visit_type_uint64(v, name, &value, errp);
}

static void test_set(Object *obj, Visitor *v, const char *name,
                      void *opaque, Error **errp)
{
    SGIRouterTestState *s = SGI_ROUTER_TEST(obj);
    uint64_t value;
    uint64_t ignored;

    if (!visit_type_uint64(v, name, &value, errp) || !test_port(s, errp)) {
        return;
    }
    if (!strcmp(name, "register")) {
        test_access(s, SN0_FABRIC_VECTOR_WRITE, value, &ignored, errp);
    } else if (!strcmp(name, "exchange")) {
        test_access(s, SN0_FABRIC_VECTOR_EXCHANGE, value, &s->exchange, errp);
    } else if (!strcmp(name, "link")) {
        sgi_router_set_port_link(SGI_ROUTER(s->router), s->port,
                                  value & 1, value & 2);
    } else if (!strcmp(name, "histogram-receive")) {
        sgi_router_histogram_receive(SGI_ROUTER(s->router), s->port,
                                     value & 1, value & 2, value & 4);
    } else if (!strcmp(name, "histogram-receive-repeat")) {
        for (uint64_t count = value >> 3; count; count--) {
            sgi_router_histogram_receive(SGI_ROUTER(s->router), s->port,
                                         value & 1, value & 2, value & 4);
        }
    } else if (!strcmp(name, "histogram-send")) {
        sgi_router_histogram_send(SGI_ROUTER(s->router), s->port, value & 1);
    } else if (!strcmp(name, "histogram-send-repeat")) {
        for (uint64_t count = value >> 1; count; count--) {
            sgi_router_histogram_send(SGI_ROUTER(s->router), s->port,
                                      value & 1);
        }
    } else {
        sgi_router_latch_port_error(SGI_ROUTER(s->router), s->port, value);
    }
}

static void test_realize(DeviceState *dev, Error **errp)
{
    qdev_realize(SGI_ROUTER_TEST(dev)->router, NULL, errp);
}

static void test_unrealize(DeviceState *dev)
{
    qdev_unrealize(SGI_ROUTER_TEST(dev)->router);
}

static void test_init(Object *obj)
{
    SGIRouterTestState *s = SGI_ROUTER_TEST(obj);

    s->port = 1;
    s->router = qdev_new(TYPE_SGI_ROUTER);
    object_property_add_child(obj, "router", OBJECT(s->router));
    object_unref(OBJECT(s->router));
    object_property_add_alias(obj, "nic-serial", OBJECT(s->router),
                              "nic-serial");
    object_property_add_uint64_ptr(obj, "address", &s->address,
                                    OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "port", &s->port,
                                    OBJ_PROP_FLAG_READWRITE);
    object_property_add(obj, "register", "uint64", test_get, test_set,
                         NULL, NULL);
    object_property_add(obj, "exchange", "uint64", test_get, test_set,
                         NULL, NULL);
    object_property_add(obj, "link", "uint64", NULL, test_set, NULL, NULL);
    object_property_add(obj, "histogram-receive", "uint64", NULL, test_set,
                        NULL, NULL);
    object_property_add(obj, "histogram-receive-repeat", "uint64", NULL,
                        test_set, NULL, NULL);
    object_property_add(obj, "histogram-send", "uint64", NULL, test_set,
                        NULL, NULL);
    object_property_add(obj, "histogram-send-repeat", "uint64", NULL,
                        test_set, NULL, NULL);
    object_property_add(obj, "port-errors", "uint64", NULL, test_set,
                         NULL, NULL);
    object_property_add(obj, "diagnostic", "uint64", test_get, NULL,
                         NULL, NULL);
}

/* Request arguments are host test inputs; the Router child owns VMState. */
static const VMStateDescription vmstate_test = {
    .name = TYPE_SGI_ROUTER_TEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void test_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = test_realize;
    dc->unrealize = test_unrealize;
    dc->vmsd = &vmstate_test;
}

static const TypeInfo test_type = {
    .name = TYPE_SGI_ROUTER_TEST,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SGIRouterTestState),
    .instance_init = test_init,
    .class_init = test_class_init,
};

static void test_register_types(void)
{
    type_register_static(&test_type);
}

type_init(test_register_types)
