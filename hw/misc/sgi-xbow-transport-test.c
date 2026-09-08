/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Sparse endpoints distinguish CPU offsets from full packet addresses. */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/sgi-xbow.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define TYPE_XBOW_TRANSPORT_TEST "sgi-xbow-transport-test"
OBJECT_DECLARE_SIMPLE_TYPE(XbowTransportTest, XBOW_TRANSPORT_TEST)

typedef struct Endpoint {
    MemoryRegion mr;
    XbowTransportTest *test;
    unsigned index;
} Endpoint;

struct XbowTransportTest {
    SysBusDevice parent_obj;
    SGIXbowState xbow;
    MemoryRegion control;
    MemoryRegion cpu_alias;
    MemoryRegion packet_alias;
    AddressSpace packets;
    Endpoint endpoint[4];
    uint64_t count[4];
    uint64_t address[4];
    uint64_t value[4];
    uint64_t attributes[4];
    uint64_t probe;
    uint64_t result;
    uint64_t guards;
};

static MemTxResult endpoint_read(void *opaque, hwaddr addr, uint64_t *data,
                                 unsigned size, MemTxAttrs attrs)
{
    Endpoint *e = opaque;
    XbowTransportTest *s = e->test;

    s->count[e->index]++;
    s->address[e->index] = addr;
    s->attributes[e->index] = attrs.requester_id;
    *data = s->value[e->index];
    return addr == 0xdead0 ? MEMTX_ERROR : MEMTX_OK;
}

static MemTxResult endpoint_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size, MemTxAttrs attrs)
{
    Endpoint *e = opaque;
    XbowTransportTest *s = e->test;
    uint64_t ignored;
    MemTxResult result = endpoint_read(opaque, addr, &ignored, size, attrs);

    if (result == MEMTX_OK) {
        s->value[e->index] = data;
    }
    return result;
}

static const MemoryRegionOps endpoint_ops = {
    .read_with_attrs = endpoint_read,
    .write_with_attrs = endpoint_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static uint64_t control_read(void *opaque, hwaddr addr, unsigned size)
{
    XbowTransportTest *s = opaque;
    unsigned i = addr / 32;

    if (i < 4) {
        switch (addr % 32) {
        case 0: return s->count[i];
        case 8: return s->address[i];
        case 16: return s->value[i];
        case 24: return s->attributes[i];
        }
    }
    return addr == 0x80 ? s->result : s->guards;
}

static void control_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    XbowTransportTest *s = opaque;
    MemTxAttrs attrs = { .requester_id = 0x1234 };
    uint8_t bytes[8] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe };

    if (addr == 0x80) {
        s->probe = value;
    } else if (addr == 0x88) {
        s->result = address_space_rw(&s->packets, s->probe, attrs,
                                     bytes, sizeof(bytes), value == 2);
    }
}

static const MemoryRegionOps control_ops = {
    .read = control_read,
    .write = control_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

static void record_refusal(XbowTransportTest *s, bool accepted,
                            Error **errp, unsigned bit)
{
    if (!accepted && *errp) {
        s->guards |= 1ULL << bit;
    }
    error_free(*errp);
    *errp = NULL;
}

static void transport_realize(DeviceState *dev, Error **errp)
{
    XbowTransportTest *s = XBOW_TRANSPORT_TEST(dev);
    unsigned i;
    Error *err = NULL;
    MemoryRegion uninitialized = { 0 };

    qdev_prop_set_uint64(DEVICE(&s->xbow), "span", 1ULL << 34);
    qdev_prop_set_uint16(DEVICE(&s->xbow), "links-present", 0x700);
    qdev_prop_set_uint16(DEVICE(&s->xbow), "startup-links-alive", 0x700);
    for (i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf("endpoint%u", i);
        s->endpoint[i].test = s;
        s->endpoint[i].index = i;
        s->value[i] = 0x1122334400000000ULL + i;
        memory_region_init_io(&s->endpoint[i].mr, OBJECT(s), &endpoint_ops,
                              &s->endpoint[i], name,
                              i < 2 ? (4096ULL << i) :
                              1ULL << (i % 2 ? 48 : 34));
    }
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 7,
                   &s->endpoint[0].mr, &s->endpoint[1].mr, &err), &err, 0);
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 8,
                   NULL, &s->endpoint[1].mr, &err), &err, 1);
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 8,
                   &s->endpoint[0].mr, NULL, &err), &err, 2);
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 8,
                   &uninitialized, &s->endpoint[1].mr, &err), &err, 3);
    record_refusal(s, sgi_xbow_attach_widget(&s->xbow, 14, NULL, &err),
                   &err, 5);
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 16,
                   &s->endpoint[0].mr, &s->endpoint[1].mr, &err), &err, 7);
    for (i = 0; i < 2; i++) {
        if (!sgi_xbow_attach_endpoint(&s->xbow, 8 + 2 * i,
                                      &s->endpoint[2 * i].mr,
                                      &s->endpoint[2 * i + 1].mr, errp)) {
            return;
        }
    }
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 8,
                   &s->endpoint[2].mr, &s->endpoint[3].mr, &err), &err, 4);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->xbow), errp)) {
        return;
    }
    record_refusal(s, sgi_xbow_attach_endpoint(&s->xbow, 11,
                   &s->endpoint[0].mr, &s->endpoint[1].mr, &err), &err, 6);
    memory_region_init_io(&s->control, OBJECT(s), &control_ops, s,
                          "transport.control", 0x100);
    /* The control callback deliberately makes a nested test transaction. */
    s->control.disable_reentrancy_guard = true;
    memory_region_init_alias(&s->cpu_alias, OBJECT(s), "transport.cpu",
                             sgi_xbow_ingress(&s->xbow, 9), 0, 1ULL << 38);
    memory_region_init_alias(&s->packet_alias, OBJECT(s), "transport.packet",
                             sgi_xbow_packet_ingress(&s->xbow, 9),
                             0, 1ULL << 52);
    address_space_init(&s->packets, &s->packet_alias, "transport.probes");
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->control);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->cpu_alias);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->packet_alias);
}

static void transport_unrealize(DeviceState *dev)
{
    XbowTransportTest *s = XBOW_TRANSPORT_TEST(dev);

    address_space_destroy(&s->packets);
    qdev_unrealize(DEVICE(&s->xbow));
}

static void transport_init(Object *obj)
{
    XbowTransportTest *s = XBOW_TRANSPORT_TEST(obj);

    object_initialize_child(obj, "xbow", &s->xbow, TYPE_SGI_XBOW);
}

static const VMStateDescription transport_vmstate = {
    .name = TYPE_XBOW_TRANSPORT_TEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(count, XbowTransportTest, 4),
        VMSTATE_UINT64_ARRAY(address, XbowTransportTest, 4),
        VMSTATE_UINT64_ARRAY(value, XbowTransportTest, 4),
        VMSTATE_UINT64_ARRAY(attributes, XbowTransportTest, 4),
        VMSTATE_END_OF_LIST()
    },
};

static void transport_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = transport_realize;
    dc->unrealize = transport_unrealize;
    dc->vmsd = &transport_vmstate;
    dc->user_creatable = true;
}

static const TypeInfo transport_type = {
    .name = TYPE_XBOW_TRANSPORT_TEST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XbowTransportTest),
    .instance_init = transport_init,
    .class_init = transport_class_init,
};

static void transport_register(void)
{
    type_register_static(&transport_type);
}

type_init(transport_register)
