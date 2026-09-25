/* SPDX-License-Identifier: GPL-2.0-or-later */
/* sgi-net-cable: joins two network ports; see the header for the contract. */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/mips/sgi/net-cable.h"
#include "hw/mips/sgi-net-port.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

typedef struct SGINetCableEnd {
    Object *port;
    unsigned int index;
} SGINetCableEnd;

struct SGINetCableState {
    DeviceState parent_obj;

    SGINetCableEnd end[2];
    uint32_t kind;
    bool down;
    NotifierList changed;
};

void sgi_net_cable_add_change_notifier(SGINetCableState *cable,
                                       Notifier *notifier)
{
    notifier_list_add(&cable->changed, notifier);
}

void sgi_net_cable_set_end(SGINetCableState *cable, unsigned int side,
                           Object *port, unsigned int index)
{
    g_assert(side < 2);
    g_assert(!DEVICE(cable)->realized);
    cable->end[side] = (SGINetCableEnd) { .port = port, .index = index };
}

static bool sgi_net_cable_end_equal(SGINetCableEnd a, Object *port,
                                    unsigned int index)
{
    return a.port == port && a.index == index;
}

bool sgi_net_cable_other_end(SGINetCableState *cable, Object *port,
                             unsigned int index, Object **peer_port,
                             unsigned int *peer_index)
{
    unsigned int side;

    for (side = 0; side < 2; side++) {
        if (sgi_net_cable_end_equal(cable->end[side], port, index)) {
            *peer_port = cable->end[side ^ 1].port;
            *peer_index = cable->end[side ^ 1].index;
            return true;
        }
    }
    return false;
}

bool sgi_net_cable_up(SGINetCableState *cable)
{
    return DEVICE(cable)->realized && !cable->down;
}

bool sgi_net_cable_neighbor(SGINetCableState *cable, Object *port,
                            unsigned int index, Object **peer_port,
                            unsigned int *peer_index)
{
    if (!sgi_net_cable_up(cable)) {
        return false;
    }
    if (!sgi_net_cable_other_end(cable, port, index, peer_port, peer_index)) {
        return false;
    }
    if (!sgi_net_port_link_state(port, index).enabled) {
        return false;
    }
    if (!sgi_net_port_link_state(*peer_port, *peer_index).enabled) {
        return false;
    }
    return true;
}

static void sgi_net_cable_set_down(Object *obj, bool value, Error **errp)
{
    SGINetCableState *cable = SGI_NET_CABLE(obj);
    unsigned int side;

    cable->down = value;
    if (!DEVICE(cable)->realized) {
        return;
    }
    sgi_net_port_flood_start();
    for (side = 0; side < 2; side++) {
        SGINetCableEnd *end = &cable->end[side];

        if (end->port) {
            sgi_net_port_peer_changed(end->port, end->index);
        }
    }
    notifier_list_notify(&cable->changed, cable);
}

static bool sgi_net_cable_get_down(Object *obj, Error **errp)
{
    return SGI_NET_CABLE(obj)->down;
}

static void sgi_net_cable_realize(DeviceState *dev, Error **errp)
{
    SGINetCableState *cable = SGI_NET_CABLE(dev);
    unsigned int side;

    for (side = 0; side < 2; side++) {
        if (!cable->end[side].port) {
            error_setg(errp, "a network cable needs both ends connected "
                       "before realize");
            return;
        }
        if (!object_dynamic_cast(cable->end[side].port, TYPE_SGI_NET_PORT)) {
            error_setg(errp, "a network cable's end %u is not a network "
                       "port", side);
            return;
        }
    }
}

static int sgi_net_cable_post_load(void *opaque, int version_id)
{
    SGINetCableState *cable = opaque;
    unsigned int side;

    sgi_net_port_flood_start();
    for (side = 0; side < 2; side++) {
        sgi_net_port_peer_changed(cable->end[side].port,
                                  cable->end[side].index);
    }
    notifier_list_notify(&cable->changed, cable);
    return 0;
}

static const VMStateDescription vmstate_sgi_net_cable = {
    .name = "sgi-net-cable",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sgi_net_cable_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(down, SGINetCableState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sgi_net_cable_properties[] = {
    DEFINE_PROP_UINT32("kind", SGINetCableState, kind,
                       SGI_NET_CABLE_KIND_CABLE),
    DEFINE_PROP_UINT32("end0-index", SGINetCableState, end[0].index, 0),
    DEFINE_PROP_UINT32("end1-index", SGINetCableState, end[1].index, 0),
};

static void sgi_net_cable_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "SGI network cable";
    dc->realize = sgi_net_cable_realize;
    dc->vmsd = &vmstate_sgi_net_cable;
    dc->user_creatable = true;
    device_class_set_props(dc, sgi_net_cable_properties);
    /*
     * The ends as properties, for a register test that joins two standalone
     * devices from the command line; a machine sets them with
     * sgi_net_cable_set_end() instead. Either way each end device's own
     * cable reference is wired separately. Weak, as the C setter's plain
     * pointer is: each end usually refers back to this cable.
     */
    object_class_property_add_link(klass, "end0", TYPE_SGI_NET_PORT,
                                   offsetof(SGINetCableState, end[0].port),
                                   qdev_prop_allow_set_link_before_realize,
                                   0);
    object_class_property_add_link(klass, "end1", TYPE_SGI_NET_PORT,
                                   offsetof(SGINetCableState, end[1].port),
                                   qdev_prop_allow_set_link_before_realize,
                                   0);
    object_class_property_add_bool(klass, "down", sgi_net_cable_get_down,
                                   sgi_net_cable_set_down);
    object_class_property_set_description(klass, "down",
        "Whether this cable is marked down, refusing every access across "
        "it as the physical link would");
}

static void sgi_net_cable_init(Object *obj)
{
    notifier_list_init(&SGI_NET_CABLE(obj)->changed);
}

static const TypeInfo sgi_net_cable_types[] = {
    {
        .name = TYPE_SGI_NET_CABLE,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(SGINetCableState),
        .instance_init = sgi_net_cable_init,
        .class_init = sgi_net_cable_class_init,
    },
};

DEFINE_TYPES(sgi_net_cable_types)
