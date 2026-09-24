/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-pci-expansion: an Origin 2000 module's optional XIO PCI expansion
 * board, as an XIO board on the sgi-xio-board base class.
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
 * `-device`-creatable: build_widget() below requires "fabric" before this
 * board's own realize, and every candidate fabric (a Crossbow's own
 * packet-ingress region) lives under a Crossbow the family builder
 * (sn-machine.c) realizes during machine init, before a command-line
 * device exists to reach it. sgi_machine_build_crossbars() there holds
 * one populated-at-startup widget back per Crossbow and prepares a spare
 * fabric proxy against it; a machine-init-done notifier
 * (sgi_machine_done()) finishes that proxy, aliasing it onto the
 * Crossbow's own real packet ingress, once every Crossbow the machine
 * builds has realized. "fabric" on a `-device` board resolves against
 * that proxy.
 *
 * This gives a `-device` board a two-way widget. build_widget() below
 * records its Bridge's own faces and hands them to
 * sgi_xtalk_crossbow_link_claim_fabric(), which tells sgi_machine_done()
 * to finish the attachment once it finishes the proxy -- without that
 * call the spare widget stays not-alive and the Crossbow disables the
 * proxy's whole target region, same as any other not-alive port
 * (hw/misc/sgi-xbow.c, sgi_xbow_sync_routes()). sgi_machine_done()
 * registers the board's faces as the widget's inbound destination
 * (sgi_xbow_attach_endpoint_live(), the post-realize counterpart of the
 * sgi_xbow_attach_endpoint() an onboard board uses before its Crossbow
 * realizes) and brings its link alive (sgi_xbow_mark_widget_alive()). A
 * processor or peer widget then reaches this board's Bridge through its
 * own Crossbow, and the board's Bridge sends packets and interrupts out to
 * any populated widget -- the same both-way path an onboard widget has. A
 * topology with no widget to spare (every one already populated at
 * startup) leaves no proxy to resolve "fabric" against, and this board's
 * own realize refuses.
 *
 * Bridge's own "board-part" is a required, exactly-12-character identity
 * register value (hw/pci-host/sgi-bridge.h,
 * sgi_bridge_board_part_error()); with no observed part number for this
 * board, the class exposes the same explicit "not observed" placeholder
 * already established for an XBridge head with no board behind it
 * (hw/pci-host/sgi-xbridge.h, SGI_XBRIDGE_NIC_PLACEHOLDER_PART).
 *
 * "slots" limits the PCI device positions a card may take to 0 .. slots-1,
 * for a carrier with fewer physical slots than the Bridge has device
 * positions: the Origin 2000 PCI Carrier Assembly has three (its owner's
 * guide, 007-3456-003 printed page 34). Which device number each physical
 * slot is wired to is not captured, so the first three are an
 * approximation. 0, the default, leaves every Bridge device position open.
 *
 * "fabric", "fabric-base", "fabric-node-shift" and "fabric-widget-shift"
 * pass straight through to the internal Bridge; see sgi-ip29-io's header
 * for why this board does not fix them itself.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xtalk-link.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
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
    SGIXioBoardState parent_obj;

    MemoryRegion *fabric;
    uint64_t fabric_base;
    uint32_t fabric_node_shift;
    uint32_t fabric_widget_shift;
    uint64_t onewire_serial;
    char *pci_bus_name;
    uint32_t slots;

    SGIBridgeState *bridge;
};

static bool pci_expansion_build_widget(SGIXioBoardState *board,
                                       Error **errp)
{
    SGIPciExpansionState *s = SGI_PCI_EXPANSION(board);
    DeviceState *bridge;

    if (!s->fabric) {
        error_setg(errp, "sgi-pci-expansion requires the fabric its "
                  "interrupt packets travel");
        return false;
    }
    if (s->slots > SGI_BRIDGE_PINS) {
        error_setg(errp, "sgi-pci-expansion slots must be 0 to %u",
                   SGI_BRIDGE_PINS);
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
    if (s->slots) {
        pci_bus_set_slot_reserved_mask(PCI_HOST_BRIDGE(bridge)->bus,
                                       ~MAKE_64BIT_MASK(0, s->slots));
    }

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);

    /*
     * Claim the spare fabric proxy now the Bridge's own faces exist, so
     * sgi_machine_done() both brings the spare widget's link alive for this
     * board's outbound packets and registers those faces as the widget's
     * inbound destination -- a processor or peer widget reaches this board's
     * Bridge through its own Crossbow the same way it reaches any onboard
     * widget. A fixture wiring "fabric" to a plain sink is a no-op here.
     */
    sgi_xtalk_crossbow_link_claim_fabric(s->fabric, board->cpu_face,
                                         board->packet_face);
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
    DEFINE_PROP_UINT32("slots", SGIPciExpansionState, slots, 0),
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
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);

    dc->desc = "Origin 2000 XIO PCI expansion board";
    dc->vmsd = &vmstate_pci_expansion;
    /* See the header comment above for what "fabric" resolves against. */
    dc->user_creatable = true;
    device_class_set_props(dc, pci_expansion_properties);
    bc->build_widget = pci_expansion_build_widget;
}

static const TypeInfo pci_expansion_type = {
    .name = TYPE_SGI_PCI_EXPANSION,
    .parent = TYPE_SGI_XIO_BOARD,
    .instance_size = sizeof(SGIPciExpansionState),
    .class_init = pci_expansion_class_init,
};

static void pci_expansion_register_types(void)
{
    type_register_static(&pci_expansion_type);
}

type_init(pci_expansion_register_types)
