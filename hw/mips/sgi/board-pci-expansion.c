/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-pci-expansion: an Origin 2000 module's optional XIO PCI expansion
 * board, as a crosstalk board on the sgi-xtalk-board base class.
 *
 * The Origin 2000 module descriptor reserves crosstalk position IO2 for an
 * optional PCI expansion, but does not establish whether a given system has
 * one fitted, nor its numeric Crossbow widget identity or any onboard
 * population beyond a bare Bridge and PCI bus
 * (docs/machines/layers/modules/ip27-origin2000.yaml, the "pci-expansion"
 * connector entry: "Occupancy and numeric Crossbow widget identity are
 * unresolved"). Unlike sgi-ip29-io and sgi-baseio, no reviewed source gives
 * this board a fixed onboard device population or a real board part
 * number, so the plan names this class its one `-device` demonstrator
 * (docs/plans/sn-machine-model.md, "SGI boards (new)"): a plain Bridge, its
 * PCI bus named so a following `-device ...,bus=<name>` can populate it.
 *
 * Not user-creatable yet, though: build_widget() below requires "fabric"
 * before this board's own realize, and every candidate fabric (a
 * Crossbow's own packet-ingress region) lives under a Crossbow the family
 * builder (sn-machine.c) only QOM-parents at the machine-done hook, after
 * every command-line device has already realized. Reaching this class from
 * `-device` needs a machine-owned spare fabric proxy per Crossbar,
 * parented at machine-init time the way the builder's own io_board_packet_
 * proxy[] already is for a board it builds itself, aliased onto the real
 * packet-ingress region at that same machine-done point -- not yet built.
 * Until it is, this class stays reachable only by the same fixture
 * tests/qtest/sgi-board-test.c uses for the other three onboard-only board
 * classes.
 *
 * Bridge's own "board-part" is a required, exactly-12-character identity
 * register value (hw/pci-host/sgi-bridge.h,
 * sgi_bridge_board_part_error()); with no observed part number for this
 * board, the class exposes the same explicit "not observed" placeholder
 * already established for an XBridge head with no board behind it
 * (hw/pci-host/sgi-xbridge.h, SGI_XBRIDGE_NIC_PLACEHOLDER_PART).
 *
 * "fabric", "fabric-base", "fabric-node-shift" and "fabric-widget-shift"
 * pass straight through to the internal Bridge; see sgi-ip29-io's header
 * for why this board does not fix them itself.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/xtalk-board.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_SGI_PCI_EXPANSION "sgi-pci-expansion"
OBJECT_DECLARE_SIMPLE_TYPE(SGIPciExpansionState, SGI_PCI_EXPANSION)

/* Placeholder; see sgi-ip29-io's own identical constant. */
#define PCI_EXPANSION_SERIAL_PLACEHOLDER 0x060504030201ULL

struct SGIPciExpansionState {
    SGIXtalkBoardState parent_obj;

    MemoryRegion *fabric;
    uint64_t fabric_base;
    uint32_t fabric_node_shift;
    uint32_t fabric_widget_shift;
    uint64_t onewire_serial;
    char *pci_bus_name;

    SGIBridgeState *bridge;
};

static bool pci_expansion_build_widget(SGIXtalkBoardState *board,
                                       Error **errp)
{
    SGIPciExpansionState *s = SGI_PCI_EXPANSION(board);
    DeviceState *bridge;

    if (!s->fabric) {
        error_setg(errp, "sgi-pci-expansion requires the fabric its "
                  "interrupt packets travel");
        return false;
    }

    bridge = qdev_new(TYPE_SGI_BRIDGE);
    object_property_add_child(OBJECT(board), "bridge", OBJECT(bridge));
    s->bridge = SGI_BRIDGE(bridge);
    qdev_prop_set_string(bridge, "board-part",
                         SGI_XBRIDGE_NIC_PLACEHOLDER_PART);
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

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);
    return true;
}

static const Property pci_expansion_properties[] = {
    DEFINE_PROP_LINK("fabric", SGIPciExpansionState, fabric,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT64("fabric-base", SGIPciExpansionState, fabric_base, 0),
    DEFINE_PROP_UINT32("fabric-node-shift", SGIPciExpansionState,
                       fabric_node_shift, 0),
    DEFINE_PROP_UINT32("fabric-widget-shift", SGIPciExpansionState,
                       fabric_widget_shift, 0),
    DEFINE_PROP_UINT64("onewire-serial", SGIPciExpansionState,
                       onewire_serial, PCI_EXPANSION_SERIAL_PLACEHOLDER),
    DEFINE_PROP_STRING("pci-bus-name", SGIPciExpansionState, pci_bus_name),
};

/* No state of its own outside the internal Bridge, which carries its own. */
static const VMStateDescription vmstate_pci_expansion = {
    .name = TYPE_SGI_PCI_EXPANSION,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void pci_expansion_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXtalkBoardClass *bc = SGI_XTALK_BOARD_CLASS(oc);

    dc->desc = "Origin 2000 XIO PCI expansion board";
    dc->vmsd = &vmstate_pci_expansion;
    /*
     * Not yet: see the header comment above. Flip this once a
     * machine-owned spare fabric proxy exists for -device to resolve
     * "fabric" against.
     */
    dc->user_creatable = false;
    device_class_set_props(dc, pci_expansion_properties);
    bc->build_widget = pci_expansion_build_widget;
}

static const TypeInfo pci_expansion_type = {
    .name = TYPE_SGI_PCI_EXPANSION,
    .parent = TYPE_SGI_XTALK_BOARD,
    .instance_size = sizeof(SGIPciExpansionState),
    .class_init = pci_expansion_class_init,
};

static void pci_expansion_register_types(void)
{
    type_register_static(&pci_expansion_type);
}

type_init(pci_expansion_register_types)
