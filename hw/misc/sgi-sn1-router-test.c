/* SPDX-License-Identifier: BSD-3-Clause */
/* Host request adapter for native SN1 router register and routing tests. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-sn1-router.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "system/address-spaces.h"

#define TYPE_SGI_SN1_ROUTER_TEST "sgi-sn1-router-test"
OBJECT_DECLARE_SIMPLE_TYPE(SGISN1RouterTestState, SGI_SN1_ROUTER_TEST)

struct SGISN1RouterTestState {
    SysBusDevice parent_obj;
    SGISN1RouterState *router;
    MemoryRegion mmio;
    uint64_t ingress;
    uint64_t turn;
    uint64_t destination;
    uint64_t vector;
    uint64_t operation;
    uint64_t hops;
    uint64_t result;
    uint64_t failure;
    uint64_t reply;
};

static uint64_t test_access(SGISN1RouterTestState *s, hwaddr addr,
                            uint64_t value, unsigned size, bool write)
{
    SGINetPacket packet = {
        .addr = addr, .data = value, .vector = s->vector,
        .op = s->operation == UINT64_MAX ?
            (write ? SGI_NET_PACKET_WRITE : SGI_NET_PACKET_READ) :
            (s->operation <= SGI_NET_PACKET_INVALID ? s->operation :
             SGI_NET_PACKET_INVALID),
        .hops = MIN(s->hops, UINT_MAX),
    };

    if (size != 8 || s->ingress > SGI_SN1_ROUTER_PORTS) {
        s->failure = SGI_NET_PACKET_FAILURE_ADDRESS;
        s->result = SGI_NET_PACKET_ERROR;
        return 0;
    }
    s->result = sgi_net_port_receive(OBJECT(s->router), s->ingress,
                                    &packet, &error_abort);
    s->failure = packet.failure;
    s->reply = packet.reply_vector;
    return packet.value;
}

static uint64_t test_read(void *opaque, hwaddr addr, unsigned size)
{
    return test_access(opaque, addr, 0, size, false);
}

static void test_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    test_access(opaque, addr, value, size, true);
}

static const MemoryRegionOps test_ops = {
    .read = test_read,
    .write = test_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static uint64_t test_route_value(Object *obj)
{
    SGISN1RouterTestState *s = SGI_SN1_ROUTER_TEST(obj);
    unsigned output;
    uint8_t next;

    if (!s->router ||
        s->ingress > SGI_SN1_ROUTER_PORTS || s->turn > UINT8_MAX ||
        s->destination > UINT8_MAX ||
        !sgi_sn1_router_route(s->router, s->ingress, s->turn,
                              s->destination, &output, &next)) {
        return UINT64_MAX;
    }
    return output | ((uint64_t)next << 8);
}

static void test_route(Object *obj, Visitor *v, const char *name,
                       void *opaque, Error **errp)
{
    uint64_t value = test_route_value(obj);

    visit_type_uint64(v, name, &value, errp);
}

static void test_realize(DeviceState *dev, Error **errp)
{
    SGISN1RouterTestState *s = SGI_SN1_ROUTER_TEST(dev);

    if (!s->router) {
        error_setg(errp, "SN1 router test needs a router link");
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s), 0, 0x10000000);
}

static void test_init(Object *obj)
{
    SGISN1RouterTestState *s = SGI_SN1_ROUTER_TEST(obj);

    s->ingress = 1;
    s->operation = UINT64_MAX;
    memory_region_init_io(&s->mmio, obj, &test_ops, s,
                          "sn1-router.test", 0x1000000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    object_property_add_link(obj, "router", TYPE_SGI_SN1_ROUTER,
                             (Object **)&s->router,
                             qdev_prop_allow_set_link_before_realize, 0);
    object_property_add_uint64_ptr(obj, "ingress", &s->ingress,
                                    OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "turn", &s->turn,
                                    OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "destination", &s->destination,
                                    OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "vector", &s->vector,
                                    OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "operation", &s->operation,
                                    OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "hops", &s->hops,
                                    OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint64_ptr(obj, "result", &s->result,
                                    OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "failure", &s->failure,
                                    OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "reply", &s->reply,
                                    OBJ_PROP_FLAG_READ);
    object_property_add(obj, "route", "uint64", test_route, NULL, NULL, NULL);
}

static void test_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = test_realize;
    dc->user_creatable = true;
}

static const TypeInfo test_info = {
    .name = TYPE_SGI_SN1_ROUTER_TEST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGISN1RouterTestState),
    .instance_init = test_init,
    .class_init = test_class_init,
};

static SGINetPacketResult silent_receive(Object *port, unsigned index,
                                         SGINetPacket *packet, Error **errp)
{
    return SGI_NET_PACKET_NONE;
}

static void silent_class_init(ObjectClass *oc, const void *data)
{
    DEVICE_CLASS(oc)->user_creatable = true;
    SGI_NET_PORT_CLASS(oc)->receive = silent_receive;
}

static const TypeInfo silent_info = {
    .name = "sgi-sn1-router-test-peer",
    .parent = TYPE_DEVICE,
    .class_init = silent_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_SGI_NET_PORT }, { }
    },
};

static void test_register(void)
{
    type_register_static(&test_info);
    type_register_static(&silent_info);
}

type_init(test_register)
