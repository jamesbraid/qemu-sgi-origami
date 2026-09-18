/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-xio-baseio: the Origin 2000 and Onyx2 standard XIO I/O board
 * (BaseIO), an sgi-xio-board subclass that creates its own Bridge and an
 * sgi-baseio (the "baseio" preset) behind it -- the removable-card
 * carrier docs/designs/sn-machine-model-architecture.md's "Base I/O"
 * table names. See sgi-baseio's own header and hw/mips/sgi/baseio.c for
 * the population this board's preset builds; this file owns only what
 * is specific to the card: its own Bridge and the card's fitted-part
 * identity.
 *
 * "fabric", "fabric-base", "fabric-node-shift" and "fabric-widget-shift"
 * pass straight through to the internal Bridge: which Hub or fabric
 * proxy receives this board's interrupt packets is a fact about the
 * machine this board is built into, not about the board itself, so the
 * machine sets them the same way it would on a bare Bridge. "fabric" is
 * required: Bridge refuses to realize without it. "pci-bus-name" also
 * passes through, unset by default: the machine building more than one
 * instance of this board -- a multi-enclosure Origin 2000's own case --
 * has to give each its own name, since two PCI buses answering to the
 * same name is a migration-identity collision on every device behind
 * them. "onewire-serial" is Bridge's own NIC ROM identity (widget
 * discovery); "ioc3-onewire-serial" is the onboard IOC3's separate
 * one-wire identity, forwarded to the internal sgi-baseio's own
 * "onewire-serial" property. "nvram-drive", "chardev-a", "chardev-b",
 * "record-a" and "record-b" forward the same way, unset by default;
 * BaseIO has no parallel function, so this class exposes no
 * "parallel-chardev" (the internal sgi-baseio's "baseio" preset never
 * wires one either).
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/baseio.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/pci-host/sgi-bridge.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_SGI_XIO_BASEIO "sgi-xio-baseio"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXioBaseioState, SGI_XIO_BASEIO)

/* BaseIO is fixed at IO1 (ip27-origin2000.yaml, baseio-bridge widget 8). */
#define XIO_BASEIO_WIDGET 8

/* Placeholder; no reviewed source assigns a serial to this board class. */
#define XIO_BASEIO_SERIAL_PLACEHOLDER 0x060504030201ULL

struct SGIXioBaseioState {
    SGIXioBoardState parent_obj;

    MemoryRegion *fabric;
    uint64_t fabric_base;
    uint32_t fabric_node_shift;
    uint32_t fabric_widget_shift;
    uint64_t onewire_serial;
    uint64_t ioc3_onewire_serial;
    char *pci_bus_name;
    char *nvram_drive_id;
    char *chardev_a_id;
    char *chardev_b_id;
    char *record_a_id;
    char *record_b_id;

    SGIBridgeState *bridge;
    SGIBaseioState *baseio;
};

static bool xio_baseio_build_widget(SGIXioBoardState *board, Error **errp)
{
    SGIXioBaseioState *s = SGI_XIO_BASEIO(board);
    const SGIBaseioPresetIdentity *identity = sgi_baseio_preset_lookup(
        "baseio");
    DeviceState *bridge, *baseio;
    PCIBus *bus;

    if (!s->fabric) {
        error_setg(errp, "sgi-xio-baseio requires the fabric its interrupt "
                  "packets travel");
        return false;
    }
    if (board->slot != XIO_BASEIO_WIDGET) {
        error_setg(errp, "sgi-xio-baseio is fixed at crosstalk widget %u",
                  XIO_BASEIO_WIDGET);
        return false;
    }

    bridge = qdev_new(TYPE_SGI_BRIDGE);
    object_property_add_child(OBJECT(board), "bridge", OBJECT(bridge));
    s->bridge = SGI_BRIDGE(bridge);
    qdev_prop_set_uint32(bridge, "revision", identity->bridge_revision);
    qdev_prop_set_string(bridge, "board-part", identity->board_part);
    qdev_prop_set_uint64(bridge, "onewire-serial", s->onewire_serial);
    if (s->pci_bus_name) {
        qdev_prop_set_string(bridge, "pci-bus-name", s->pci_bus_name);
    }
    object_property_set_link(OBJECT(bridge), "fabric", OBJECT(s->fabric),
                             &error_abort);
    qdev_prop_set_uint64(bridge, "fabric-base", s->fabric_base);
    qdev_prop_set_uint32(bridge, "fabric-node-shift", s->fabric_node_shift);
    qdev_prop_set_uint32(bridge, "fabric-widget-shift",
                         s->fabric_widget_shift);
    if (!sysbus_realize(SYS_BUS_DEVICE(bridge), errp)) {
        return false;
    }
    bus = PCI_HOST_BRIDGE(bridge)->bus;

    baseio = qdev_new(TYPE_SGI_BASEIO);
    object_property_add_child(OBJECT(board), "baseio", OBJECT(baseio));
    s->baseio = SGI_BASEIO(baseio);
    qdev_prop_set_string(baseio, "preset", "baseio");
    object_property_set_link(OBJECT(baseio), "pci-bus", OBJECT(bus),
                             &error_abort);
    qdev_prop_set_uint64(baseio, "onewire-serial", s->ioc3_onewire_serial);
    if (s->nvram_drive_id) {
        qdev_prop_set_string(baseio, "nvram-drive", s->nvram_drive_id);
    }
    if (s->chardev_a_id) {
        qdev_prop_set_string(baseio, "chardev-a", s->chardev_a_id);
    }
    if (s->chardev_b_id) {
        qdev_prop_set_string(baseio, "chardev-b", s->chardev_b_id);
    }
    if (s->record_a_id) {
        qdev_prop_set_string(baseio, "record-a", s->record_a_id);
    }
    if (s->record_b_id) {
        qdev_prop_set_string(baseio, "record-b", s->record_b_id);
    }
    if (!qdev_realize(DEVICE(baseio), NULL, errp)) {
        return false;
    }

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);
    return true;
}

static const Property xio_baseio_properties[] = {
    DEFINE_PROP_LINK("fabric", SGIXioBaseioState, fabric, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT64("fabric-base", SGIXioBaseioState, fabric_base, 0),
    DEFINE_PROP_UINT32("fabric-node-shift", SGIXioBaseioState,
                       fabric_node_shift, 0),
    DEFINE_PROP_UINT32("fabric-widget-shift", SGIXioBaseioState,
                       fabric_widget_shift, 0),
    DEFINE_PROP_UINT64("onewire-serial", SGIXioBaseioState, onewire_serial,
                       XIO_BASEIO_SERIAL_PLACEHOLDER),
    DEFINE_PROP_UINT64("ioc3-onewire-serial", SGIXioBaseioState,
                       ioc3_onewire_serial, SGI_IOC3_ONEWIRE_SERIAL),
    DEFINE_PROP_STRING("pci-bus-name", SGIXioBaseioState, pci_bus_name),
    DEFINE_PROP_STRING("nvram-drive", SGIXioBaseioState, nvram_drive_id),
    DEFINE_PROP_STRING("chardev-a", SGIXioBaseioState, chardev_a_id),
    DEFINE_PROP_STRING("chardev-b", SGIXioBaseioState, chardev_b_id),
    DEFINE_PROP_STRING("record-a", SGIXioBaseioState, record_a_id),
    DEFINE_PROP_STRING("record-b", SGIXioBaseioState, record_b_id),
};

/* No state of its own outside the children the Bridge and baseio own. */
static const VMStateDescription vmstate_xio_baseio = {
    .name = TYPE_SGI_XIO_BASEIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void xio_baseio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);

    dc->desc = "Origin 2000 / Onyx2 BaseIO board";
    dc->vmsd = &vmstate_xio_baseio;
    /* Fitted per module by the machine, at most one per deskside enclosure. */
    dc->user_creatable = false;
    device_class_set_props(dc, xio_baseio_properties);
    bc->build_widget = xio_baseio_build_widget;
}

static const TypeInfo xio_baseio_type = {
    .name = TYPE_SGI_XIO_BASEIO,
    .parent = TYPE_SGI_XIO_BOARD,
    .instance_size = sizeof(SGIXioBaseioState),
    .class_init = xio_baseio_class_init,
};

static void xio_baseio_register_types(void)
{
    type_register_static(&xio_baseio_type);
}

type_init(xio_baseio_register_types)
