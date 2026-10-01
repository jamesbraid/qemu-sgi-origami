/* SPDX-License-Identifier: BSD-3-Clause */
/* Synthetic six-head assembly; the caller chooses every external cable. */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/mips/sgi/xio-link.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "hw/misc/sgi-bedrock-io.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define TYPE_XBRIDGE_ASSEMBLY "sgi-xbridge-assembly-fixture"
OBJECT_DECLARE_SIMPLE_TYPE(XBridgeAssemblyState, XBRIDGE_ASSEMBLY)

struct XBridgeAssemblyState {
    SysBusDevice parent_obj;
    DeviceState *chip[3];
    SGIXioLink link[4];
    MemoryRegion host;
    MemoryRegion host_view;
    uint8_t port[4];
    bool source_aware_host;
    bool context_loop;
    DeviceState *ii;
    SGIXioLink host_link;
};

static void assembly_realize(DeviceState *dev, Error **errp)
{
    XBridgeAssemblyState *s = XBRIDGE_ASSEMBLY(dev);
    unsigned i, j;

    for (i = 0; i < 4; i++) {
        if (s->port[i] < 8 || s->port[i] > 0xd || s->port[i] == 0xa) {
            error_setg(errp, "fixture cable %u needs a U0 port from 8 to d "
                       "other than host port a", i);
            return;
        }
        for (j = 0; j < i; j++) {
            if (s->port[i] == s->port[j]) {
                error_setg(errp, "fixture cables share U0 port %x", s->port[i]);
                return;
            }
        }
    }
    if (!memory_region_init_ram(&s->host, OBJECT(s), "xbridge-fixture.host",
                                0x100000, errp)) {
        return;
    }
    memory_region_init_alias(&s->host_view, OBJECT(s), "host-view",
                             &s->host, 0, memory_region_size(&s->host));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->host_view);
    if (s->source_aware_host) {
        s->ii = qdev_new(TYPE_SGI_BEDROCK_IO);
        object_property_add_child(OBJECT(s), "ii", OBJECT(s->ii));
        if (!object_property_set_link(OBJECT(s->ii), "dma-memory",
                                      OBJECT(&s->host), errp) ||
            !object_property_set_link(OBJECT(s->ii), "hub-registers",
                                      OBJECT(&s->host), errp) ||
            !sysbus_realize(SYS_BUS_DEVICE(s->ii), errp)) {
            return;
        }
    }
    for (i = 0; i < 3; i++) {
        g_autofree char *name = g_strdup_printf("chip%u", i);
        g_autofree char *prefix = g_strdup_printf("fixture%u-", i);

        s->chip[i] = qdev_new(TYPE_SGI_XBRIDGE);
        object_property_add_child(OBJECT(s), name, OBJECT(s->chip[i]));
        if (s->source_aware_host &&
            !sgi_xbridge_enable_packet_context(SGI_XBRIDGE(s->chip[i]), errp)) {
            return;
        }
        qdev_prop_set_uint8(s->chip[i], "host-port", 0xa);
        qdev_prop_set_string(s->chip[i], "bus-prefix", prefix);
        qdev_prop_set_bit(s->chip[i], "superbridge", i != 0);
        if (!object_property_set_link(OBJECT(s->chip[i]), "fabric",
                                      OBJECT(&s->host), errp)) {
            return;
        }
    }
    if (s->source_aware_host) {
        sgi_xio_link_init(&s->host_link, OBJECT(s), "fixture-host",
                          OBJECT(s->ii), 0, OBJECT(s->chip[0]), 0xa);
        if (!sgi_xio_link_connect(&s->host_link, errp)) {
            return;
        }
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->chip[0]), errp)) {
        return;
    }
    for (i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf("fixture-cable%u", i);

        sgi_xio_link_init(&s->link[i], OBJECT(s), name,
                          OBJECT(s->chip[0]), s->port[i],
                          OBJECT(s->chip[1 + i / 2]), 0xe + i % 2);
        if (!sgi_xio_link_connect(&s->link[i], errp)) {
            return;
        }
    }
    for (i = 1; i < 3; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(s->chip[i]), errp)) {
            return;
        }
    }
    if (s->context_loop) {
        SGIXioPeer peer = {
            .port = OBJECT(s->chip[0]),
            .index = 8,
            .switched = true,
            .cpu_face = sgi_xio_port_cpu_face(OBJECT(s->chip[0]), 8),
            .packet_face = sgi_xio_port_packet_face(OBJECT(s->chip[0]), 8),
        };

        if (!sgi_xio_port_connect(OBJECT(s->chip[0]), 0xb, &peer, errp)) {
            return;
        }
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(s),
                     sysbus_mmio_get_region(SYS_BUS_DEVICE(s->chip[0]), 0));
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 2; j++) {
            sysbus_init_mmio(SYS_BUS_DEVICE(s),
                sgi_xbridge_widget_view(SGI_XBRIDGE(s->chip[i]), 0xe + j));
        }
    }
    for (i = 1; i < 3; i++) {
        sysbus_init_mmio(SYS_BUS_DEVICE(s),
            sysbus_mmio_get_region(SYS_BUS_DEVICE(s->chip[i]), 1));
    }
    for (i = 0; i < 4; i++) {
        sysbus_init_mmio(SYS_BUS_DEVICE(s),
            sgi_xio_port_packet_face(OBJECT(s->chip[0]), s->port[i]));
    }
    if (s->source_aware_host) {
        sysbus_init_mmio(SYS_BUS_DEVICE(s),
                         sysbus_mmio_get_region(SYS_BUS_DEVICE(s->ii), 0));
    }
}

static void assembly_finalize(Object *obj)
{
    XBridgeAssemblyState *s = XBRIDGE_ASSEMBLY(obj);
    unsigned i;

    g_free(s->host_link.name);
    for (i = 0; i < 4; i++) {
        g_free(s->link[i].name);
    }
}

static const Property assembly_properties[] = {
    DEFINE_PROP_BOOL("context-loop", XBridgeAssemblyState, context_loop, false),
    DEFINE_PROP_BOOL("source-aware-host", XBridgeAssemblyState,
                      source_aware_host, false),
    DEFINE_PROP_UINT8("chip1-e-port", XBridgeAssemblyState, port[0], 0),
    DEFINE_PROP_UINT8("chip1-f-port", XBridgeAssemblyState, port[1], 0),
    DEFINE_PROP_UINT8("chip2-e-port", XBridgeAssemblyState, port[2], 0),
    DEFINE_PROP_UINT8("chip2-f-port", XBridgeAssemblyState, port[3], 0),
};

static void assembly_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Synthetic six-head XBridge assembly";
    dc->realize = assembly_realize;
    dc->user_creatable = true;
    device_class_set_props(dc, assembly_properties);
}

static const TypeInfo assembly_type = {
    .name = TYPE_XBRIDGE_ASSEMBLY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(XBridgeAssemblyState),
    .instance_finalize = assembly_finalize,
    .class_init = assembly_class_init,
};

static void assembly_register_types(void)
{
    type_register_static(&assembly_type);
}

type_init(assembly_register_types)
