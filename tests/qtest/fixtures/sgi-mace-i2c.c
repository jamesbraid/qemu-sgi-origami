/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Synthetic endpoint; no O2 endpoint identity or wiring is implied. */
#include "qemu/osdep.h"
#include "hw/i2c/i2c-wire.h"
#include "hw/mips/sgi/mace-i2c.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/qtest.h"

#define TYPE_MACE_I2C_TEST "sgi-mace-i2c-test-endpoint"
OBJECT_DECLARE_SIMPLE_TYPE(MACEI2CTest, MACE_I2C_TEST)

struct MACEI2CTest {
    I2CSlave parent_obj;
    uint32_t mode;
    uint32_t cursor;
    uint32_t starts;
    uint32_t stops;
    uint32_t nacks;
    uint32_t writes;
    uint32_t payload;
    uint64_t events;
    uint32_t busy_seen;
    uint32_t recursive_events;
    bool callback_active;
    bool profile_available;
};

static int endpoint_event(I2CSlave *dev, enum i2c_event event)
{
    MACEI2CTest *s = MACE_I2C_TEST(dev);
    unsigned code;

    if (s->callback_active) {
        s->recursive_events++;
    }

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_RECV:
        s->starts++;
        code = event == I2C_START_SEND ? 1 : 2;
        break;
    case I2C_FINISH:
        s->stops++;
        code = 5;
        break;
    case I2C_NACK:
        s->nacks++;
        code = 4;
        break;
    case I2C_RESTART:
        code = 6;
        break;
    default:
        g_assert_not_reached();
    }
    s->events = (s->events << 4) | code;
    return s->mode == 5 ? 1 : 0;
}

static uint8_t endpoint_recv(I2CSlave *dev)
{
    MACEI2CTest *s = MACE_I2C_TEST(dev);
    static const uint8_t bytes[] = { 0x96, 0x3d, 0x71, 0xb2 };

    uint8_t byte = bytes[s->cursor++ % ARRAY_SIZE(bytes)];
    MACEI2C *controller = SGI_MACE_I2C(dev->qdev.parent_bus->parent);
    uint64_t status;

    s->events = (s->events << 4) | 3;
    g_assert_cmpint(controller->regs.ops->read_with_attrs(controller, 0x10,
                       &status, 8, MEMTXATTRS_UNSPECIFIED), ==, MEMTX_OK);
    s->busy_seen |= status & 0x10;
    if (s->mode == 7) {
        s->callback_active = true;
        mace_i2c_reset(controller, true);
        s->callback_active = false;
    }
    return byte;
}

static int endpoint_send(I2CSlave *dev, uint8_t byte)
{
    MACEI2CTest *s = MACE_I2C_TEST(dev);

    s->events = (s->events << 4) | 7;
    s->payload = (s->payload << 8) | byte;
    s->writes++;
    if (s->mode == 8) {
        MACEI2C *controller = SGI_MACE_I2C(dev->qdev.parent_bus->parent);

        s->callback_active = true;
        mace_i2c_reset(controller, true);
        s->callback_active = false;
    }
    return s->mode == 6 ? -EIO : s->mode == 4;
}

static const VMStateDescription vmstate_endpoint = {
    .name = TYPE_MACE_I2C_TEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, MACEI2CTest),
        VMSTATE_UINT32_EQUAL(mode, MACEI2CTest),
        VMSTATE_UINT32(cursor, MACEI2CTest),
        VMSTATE_UINT32(starts, MACEI2CTest),
        VMSTATE_UINT32(stops, MACEI2CTest),
        VMSTATE_UINT32(nacks, MACEI2CTest),
        VMSTATE_UINT32(writes, MACEI2CTest),
        VMSTATE_UINT32(payload, MACEI2CTest),
        VMSTATE_UINT64(events, MACEI2CTest),
        VMSTATE_UINT32(busy_seen, MACEI2CTest),
        VMSTATE_UINT32(recursive_events, MACEI2CTest),
        VMSTATE_BOOL(profile_available, MACEI2CTest),
        VMSTATE_END_OF_LIST()
    },
};

static void endpoint_realize(DeviceState *dev, Error **errp)
{
    if (!qtest_enabled()) {
        error_setg(errp, "MACE I2C endpoint requires qtest");
    }
}

static bool endpoint_profile_get(Object *obj, Error **errp)
{
    return MACE_I2C_TEST(obj)->profile_available;
}

static void endpoint_profile_set(Object *obj, bool value, Error **errp)
{
    MACE_I2C_TEST(obj)->profile_available = value;
}

static void endpoint_init(Object *obj)
{
    MACEI2CTest *s = MACE_I2C_TEST(obj);

    s->profile_available = true;
    object_property_add_bool(obj, "profile-available", endpoint_profile_get,
                              endpoint_profile_set);
    object_property_add_uint32_ptr(obj, "busy-seen", &s->busy_seen,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "recursive-events",
                                   &s->recursive_events,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "cursor", &s->cursor,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "starts", &s->starts,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "stops", &s->stops, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "nacks", &s->nacks, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "writes", &s->writes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "payload", &s->payload,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "events", &s->events,
                                   OBJ_PROP_FLAG_READ);
}

static const Property endpoint_props[] = {
    DEFINE_PROP_UINT32("mode", MACEI2CTest, mode, 0),
};

static uint64_t endpoint_profile(I2CSlave *dev)
{
    unsigned mode = MACE_I2C_TEST(dev)->mode;

    return !MACE_I2C_TEST(dev)->profile_available ||
           mode == 1 || mode == 2 || mode == 3 ? 0 : mode + 1;
}

static void endpoint_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);
    I2CWireEndpointClass *ec = I2C_WIRE_ENDPOINT_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    ec->untimed_profile = endpoint_profile;
    sc->event = endpoint_event;
    sc->send = endpoint_send;
    sc->recv = endpoint_recv;
    dc->realize = endpoint_realize;
    dc->vmsd = &vmstate_endpoint;
    device_class_set_props(dc, endpoint_props);
}

static const TypeInfo endpoint_info = {
    .name = TYPE_MACE_I2C_TEST,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(MACEI2CTest),
    .instance_init = endpoint_init,
    .class_init = endpoint_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_I2C_WIRE_ENDPOINT }, { }
    },
};

static void write_only_class_init(ObjectClass *oc, const void *data)
{
    I2C_SLAVE_CLASS(oc)->recv = NULL;
}

static const TypeInfo write_only_info = {
    .name = "sgi-mace-i2c-test-write-only",
    .parent = TYPE_MACE_I2C_TEST,
    .class_init = write_only_class_init,
};

static void read_only_class_init(ObjectClass *oc, const void *data)
{
    I2C_SLAVE_CLASS(oc)->send = NULL;
}

static const TypeInfo read_only_info = {
    .name = "sgi-mace-i2c-test-read-only",
    .parent = TYPE_MACE_I2C_TEST,
    .class_init = read_only_class_init,
};

static void register_endpoint(void)
{
    type_register_static(&endpoint_info);
    type_register_static(&write_only_info);
    type_register_static(&read_only_info);
}
type_init(register_endpoint)
