/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-pci-shoehorn: the SGI "PCI-XTALK ADAPTER" (the "PCI shoehorn"), a
 * single-XIO-slot XIO->PCI converter board on the sgi-xio-board base class,
 * optionally fitted with an SGI Cad Duo input card.
 *
 * The product is a single crosstalk (XIO) slot adapter that carries one PCI
 * card. Its silkscreen reads "PCI-XTALK ADAPTER"; SGI assembly 030-1275-002
 * (REV B) / 030-1275-004 (rev A), (c) 1997, made in USA, tested at
 * Celestica (ingested product photographs, Library docs/library/cache, the
 * ntc-tech and eBay listings). On the board is an LSI L2A0710 "Bridge Rev D"
 * ASIC, SGI part 099-0125-007 -- the XIO-to-PCI Bridge chip, the same Bridge
 * D revision (widget revision 4) sgi-bridge already models -- and one PCI
 * daughter/riser position on a Burndy "COMPUTERBUS" card-edge connector. The
 * photographed unit carried a QLogic Fibre Channel card; the adapter itself
 * is generic and holds any single PCI card, here the Cad Duo IOC3.
 *
 * SGI's own IP27/SN board-type namespace names it distinctly from the
 * multi-slot expansion products: OpenBSD sys/arch/sgi/include/mnode.h has
 * IP27_BRD_PCI3 "PCI shoehorn" (against IP27_BRD_PCI1 "PCI shoebox"), and
 * Linux include/asm-ia64/sn/klconfig.h has KLTYPE_SHOEHORN.
 *
 * Attribution is an open question this model does not resolve: the project's
 * own docs/library/origin200-nic-inventory.md attributes the 030-1275-00x
 * PCI-XTALK ADAPTER to Origin 2000 (recording the Origin 200's own Bridge as
 * 030-1244-001), while both reseller listings present 030-1275-00x boards
 * pulled from Origin 200 chassis. Reseller attribution is not hardware-graph
 * evidence; the user's target is Origin 2000 and our inventory places it
 * there, so this models an Origin 2000 XIO adapter. See the datasheet.
 *
 * Like sgi-pci-expansion this board attaches through crossbow's spare fabric
 * proxy when created with `-device`, or through the ordinary startup path
 * when a topology declares it at a real XIO slot's widget (Origin 2000 IO2 is
 * crosstalk widget 12, docs/machines/layers/modules/origin2000-module.yaml).
 * Either way #720's spare-widget change and sgi_machine_done() make its
 * widget a full two-way crosstalk endpoint.
 *
 * ---- The fitted Cad Duo card and its keyboard/mouse interrupt ----
 *
 * "cad-duo-slot" fits the existing SGI Cad Duo PCI Card -- a standalone
 * sgi-ioc3 carrying ethernet, one-wire identity and a PS/2 keyboard/mouse
 * pair (docs/datasheets/sgi-cad-duo-pci-card.md; SGI option board
 * 030-1155-002) -- into the adapter's PCI position and wires its SuperIO
 * interrupt so the guest actually receives keystrokes.
 *
 * Only an sgi-ioc3's ethernet child drives its PCI INTA (hw/misc/sgi-ioc3.c);
 * the chip's serial ports, keyboard/mouse, parallel port and real-time
 * interrupt instead ride a separate SuperIO interrupt output (GPIO-out index
 * 0). Left unwired -- as it is for the bare add-in card on Origin 200
 * (docs/datasheets/sgi-cad-duo-pci-card.md) -- those interrupts are dead, so
 * a keystroke never reaches the guest. A real Cad Duo is a plug-in PCI card,
 * not a soldered board, so its single interrupt can only leave on the card
 * edge's INTA..INTD; a single-function card uses INTA, which the SGI Bridge
 * maps to the interrupt pin of the card's own slot number (slot N INTA ->
 * pin N, SGI_BRIDGE_PINS in hw/pci-host/sgi-bridge.h, sgi_bridge_map_irq()).
 * This board therefore wires the fitted card's SuperIO output to that same
 * pin, matching the landed BaseIO-G precedent for a no-ethernet IOC3
 * (hw/mips/sgi/baseio.c, the io6g MediaIO at device 6 -> Bridge pin 6). The
 * exact SuperIO-output-to-Bridge-pin wire is an inferred approximation, not a
 * captured measurement: see the reviewed datasheet for the stated limitation
 * and the discriminating test.
 *
 * Once wired, the SuperIO interrupt takes the same Bridge->Crossbow->Hub
 * path any card interrupt does: the Bridge composes an interrupt packet for
 * the host node's Hub, the Crossbow routes it, and the Hub latches it.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xtalk-link.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci-host/sgi-bridge.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_SGI_PCI_SHOEHORN "sgi-pci-shoehorn"
OBJECT_DECLARE_SIMPLE_TYPE(SGIPciShoehornState, SGI_PCI_SHOEHORN)

/* No card fitted; a real PCI device number is 0 through SGI_BRIDGE_PINS-1. */
#define SGI_PCI_SHOEHORN_NO_CARD UINT32_MAX

/*
 * The Bridge's board-part is its NIC ROM identity register, exactly twelve
 * characters (hw/pci-host/sgi-bridge.h, sgi_bridge_board_part_error()). The
 * adapter's own SGI assembly number, from the ingested product photographs.
 */
#define SGI_SHOEHORN_BRIDGE_BOARD_PART "030-1275-002"

/* Placeholder; see sgi-ip29-io's own identical constant. */
#define PCI_SHOEHORN_SERIAL_PLACEHOLDER 0x060504030201ULL

/*
 * The fitted Cad Duo's own one-wire identity part serial. No real value is
 * captured, so this is the same descending-byte placeholder idiom used for
 * an unobserved NIC ROM serial elsewhere.
 */
#define PCI_SHOEHORN_CAD_DUO_ONEWIRE 0x0d0c0b0a0908ULL

struct SGIPciShoehornState {
    SGIXioBoardState parent_obj;

    MemoryRegion *fabric;
    uint64_t fabric_base;
    uint32_t fabric_node_shift;
    uint32_t fabric_widget_shift;
    uint64_t onewire_serial;
    char *pci_bus_name;
    uint32_t cad_duo_slot;

    SGIBridgeState *bridge;
};

/*
 * Fit an SGI Cad Duo card at cad_duo_slot on the internal Bridge's bus and
 * wire its SuperIO interrupt to the Bridge pin its own slot's INTA maps to.
 * The subsystem id is the "not observed" placeholder the reviewed datasheet
 * establishes; left at the class default the chip resolves through
 * sgi_ioc3_board()'s fall-through to the CAD DUO population.
 */
static bool pci_shoehorn_fit_cad_duo(SGIPciShoehornState *s, Error **errp)
{
    PCIBus *bus = PCI_HOST_BRIDGE(s->bridge)->bus;
    PCIDevice *ioc3;

    ioc3 = pci_new(PCI_DEVFN(s->cad_duo_slot, 0), TYPE_SGI_IOC3);
    object_property_add_child(OBJECT(s), "cad-duo", OBJECT(ioc3));
    qdev_prop_set_uint16(DEVICE(ioc3), "subsystem-id",
                         SGI_IOC3_SUBSYS_CAD_DUO_PCI_CARD_UNCAPTURED);
    qdev_prop_set_uint64(DEVICE(ioc3), "onewire-serial",
                         PCI_SHOEHORN_CAD_DUO_ONEWIRE);
    if (!pci_realize_and_unref(ioc3, bus, errp)) {
        return false;
    }

    /*
     * SuperIO GPIO-out index 0 to the Bridge interrupt pin of the card's own
     * slot: a plug-in card's single interrupt is its INTA, and the Bridge
     * maps slot N's INTA to pin N (sgi_bridge_map_irq()). Inferred, not
     * captured -- see this file's header and the datasheet.
     */
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in(DEVICE(s->bridge), s->cad_duo_slot));
    return true;
}

static bool pci_shoehorn_build_widget(SGIXioBoardState *board, Error **errp)
{
    SGIPciShoehornState *s = SGI_PCI_SHOEHORN(board);
    DeviceState *bridge;

    if (!s->fabric) {
        error_setg(errp, "sgi-pci-shoehorn requires the fabric its "
                  "interrupt packets travel");
        return false;
    }
    if (s->cad_duo_slot != SGI_PCI_SHOEHORN_NO_CARD &&
        s->cad_duo_slot >= SGI_BRIDGE_PINS) {
        error_setg(errp, "sgi-pci-shoehorn cad-duo-slot must name a Bridge "
                   "device slot, 0 to %u", SGI_BRIDGE_PINS - 1);
        return false;
    }

    bridge = qdev_new(TYPE_SGI_BRIDGE);
    object_property_add_child(OBJECT(board), "bridge", OBJECT(bridge));
    s->bridge = SGI_BRIDGE(bridge);
    qdev_prop_set_string(bridge, "board-part",
                         SGI_SHOEHORN_BRIDGE_BOARD_PART);
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

    if (s->cad_duo_slot != SGI_PCI_SHOEHORN_NO_CARD &&
        !pci_shoehorn_fit_cad_duo(s, errp)) {
        return false;
    }

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);

    /* Same spare-fabric claim as sgi-pci-expansion; see its header. */
    sgi_xtalk_crossbow_link_claim_fabric(s->fabric, board->cpu_face,
                                         board->packet_face);
    return true;
}

static const Property pci_shoehorn_properties[] = {
    DEFINE_PROP_LINK("fabric", SGIPciShoehornState, fabric,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT64("fabric-base", SGIPciShoehornState, fabric_base, 0),
    DEFINE_PROP_UINT32("fabric-node-shift", SGIPciShoehornState,
                       fabric_node_shift, 0),
    DEFINE_PROP_UINT32("fabric-widget-shift", SGIPciShoehornState,
                       fabric_widget_shift, 0),
    DEFINE_PROP_UINT64("onewire-serial", SGIPciShoehornState,
                       onewire_serial, PCI_SHOEHORN_SERIAL_PLACEHOLDER),
    DEFINE_PROP_STRING("pci-bus-name", SGIPciShoehornState, pci_bus_name),
    DEFINE_PROP_UINT32("cad-duo-slot", SGIPciShoehornState, cad_duo_slot,
                       SGI_PCI_SHOEHORN_NO_CARD),
};

/* No state of its own outside the internal Bridge and IOC3, which carry it. */
static const VMStateDescription vmstate_pci_shoehorn = {
    .name = TYPE_SGI_PCI_SHOEHORN,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void pci_shoehorn_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);

    dc->desc = "SGI PCI-XTALK ADAPTER (PCI shoehorn)";
    dc->vmsd = &vmstate_pci_shoehorn;
    dc->user_creatable = true;
    device_class_set_props(dc, pci_shoehorn_properties);
    bc->build_widget = pci_shoehorn_build_widget;
}

static const TypeInfo pci_shoehorn_type = {
    .name = TYPE_SGI_PCI_SHOEHORN,
    .parent = TYPE_SGI_XIO_BOARD,
    .instance_size = sizeof(SGIPciShoehornState),
    .class_init = pci_shoehorn_class_init,
};

static void pci_shoehorn_register_types(void)
{
    type_register_static(&pci_shoehorn_type);
}

type_init(pci_shoehorn_register_types)
