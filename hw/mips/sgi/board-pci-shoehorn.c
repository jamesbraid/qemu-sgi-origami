/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * sgi-pci-shoehorn: the SGI "PCI-XTALK ADAPTER" (the "PCI shoehorn"), a
 * single-XIO-slot XIO->PCI converter board on the sgi-xio-board base class,
 * optionally fitted with an SGI Cad Duo input card.
 *
 * The product is a single XIO slot adapter that carries one PCI
 * card. Its silkscreen reads "PCI-XTALK ADAPTER"; SGI assembly 030-1275-002
 * (REV B) / 030-1275-004 (rev A), (c) 1997, made in USA, tested at Celestica
 * (product photographs and reseller listings, including ntc-tech and eBay).
 * On the board is an LSI L2A0710 "Bridge Rev D" ASIC, SGI part 099-0125-007,
 * the XIO-to-PCI Bridge chip, the same Bridge D revision (widget revision 4)
 * sgi-bridge already models, and one PCI daughter/riser position on a Burndy
 * "COMPUTERBUS" card-edge connector. The photographed unit carried a QLogic
 * Fibre Channel card; the adapter itself is generic and holds any single PCI
 * card, here the Cad Duo IOC3.
 *
 * SGI's IP27/SN board-type namespace names it distinctly from the multi-slot
 * expansion products: OpenBSD sys/arch/sgi/include/mnode.h has IP27_BRD_PCI3
 * "PCI shoehorn" (against IP27_BRD_PCI1 "PCI shoebox"), and Linux
 * include/asm-ia64/sn/klconfig.h has KLTYPE_SHOEHORN.
 *
 * Attribution is an open question this model does not resolve: the
 * 030-1275-00x PCI-XTALK ADAPTER is attributed to Origin 2000, distinct from
 * the Origin 200's Bridge, 030-1244-001, while reseller listings present
 * 030-1275-00x boards pulled from Origin 200 chassis. Reseller attribution is
 * not hardware-graph evidence, so this models an Origin 2000 XIO adapter.
 *
 * Like sgi-pci-expansion this board is linked to the Crossbow port of the
 * empty XIO slot it names when created with `-device` (slot=io2; Origin
 * 2000 IO2 is XIO widget 12), or by the machine builder when a topology
 * declares it. Either way its widget is a full two-way XIO endpoint.
 *
 * ---- The fitted Cad Duo card and its keyboard/mouse interrupt ----
 *
 * "cad-duo-slot" fits the existing SGI Cad Duo PCI Card, a standalone
 * sgi-ioc3 carrying ethernet, one-wire identity and a PS/2 keyboard/mouse
 * pair (SGI option board 030-1155-002), into the adapter's PCI position and
 * wires its SuperIO interrupt so the guest actually receives keystrokes.
 *
 * Only an sgi-ioc3's ethernet child drives its PCI INTA (hw/misc/sgi-ioc3.c);
 * the chip's serial ports, keyboard/mouse, parallel port and real-time
 * interrupt instead ride a separate SuperIO interrupt output (GPIO-out index
 * 0). Left unwired, as it is for the bare add-in card on Origin 200, those
 * interrupts are dead, so a keystroke never reaches the guest. A real Cad Duo
 * is a plug-in PCI card, not a soldered board, so its interrupts can only
 * leave on the card edge's INTA..INTD. The ethernet uses INTA; an add-in IOC3
 * raises SuperIO on INTB (OpenBSD sys/arch/sgi/pci/ioc.c). The Bridge routes
 * a slot's INTA/INTC to pin slot and its INTB/INTD to pin slot ^ 4 (OpenBSD
 * sys/arch/sgi/xbow/xbridge.c, xbridge_intr_map(); Linux
 * arch/mips/pci/pci-xtalk-bridge.c, int_mapping[slot][1]; Linux 2.5.0
 * arch/ia64/sn/io/pcibr.c). This board therefore wires the fitted card's
 * SuperIO output to pin slot ^ 4. IRIX 6.5.30 agrees: with the card at device
 * 1 it enables Bridge pins 1 and 5. Earlier, with the card at device 0 and
 * SuperIO sharing its INTA pin, it looped in the ethernet handler, which
 * never clears a SuperIO source. Which card-edge line the real Cad Duo drives
 * is inferred from those drivers, not independently observed on real
 * hardware.
 *
 * Once wired, the SuperIO interrupt takes the same Bridge->Crossbow->Hub path
 * any card interrupt does: the Bridge composes an interrupt packet for the
 * host node's Hub, the Crossbow routes it, and the Hub latches it.
 *
 * "sio-slot" fits SGI's dual-serial PCI card (030-1657-003, PCI_SIO_UFC)
 * instead: an IOC3 with serial ports A and B and nothing else, its board
 * record on the chip's one-wire bus. IRIX 6.5.30 takes that card's SuperIO
 * interrupt from line B even with no Ethernet on the chip, so it is wired to
 * pin slot ^ 4 like the Cad Duo's. The adapter has one PCI position, so only
 * one of the two slot properties may be set.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xio-link.h"
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
 * The adapter's board-identity record on its Bridge's one-wire bus. The part
 * is the SGI assembly number on the photographed board's label, and the
 * revision the REV B beside it: the label is not the record, so the revision
 * is inferred. Real Origin 2000 inventories name the board XTALK_PCI. The
 * barcode is a placeholder, since a real one names a physical unit. The part
 * is exactly twelve characters (hw/pci-host/sgi-bridge.h,
 * sgi_bridge_board_part_error()).
 */
#define SGI_SHOEHORN_BRIDGE_BOARD_PART     "030-1275-002"
#define SGI_SHOEHORN_BRIDGE_BOARD_REVISION "B"
#define SGI_SHOEHORN_BRIDGE_BOARD_NAME     "XTALK_PCI"
#define SGI_SHOEHORN_BRIDGE_BOARD_BARCODE  "OXTPCI0001"

/*
 * The photographed board's Bridge is marked "Bridge Rev D", widget revision
 * 4,
 * and real Origin 2000 inventories list the Bridge behind an XTALK_PCI board
 * as Rev 4.
 */
#define SGI_SHOEHORN_BRIDGE_REVISION 4

/* Placeholder; see sgi-ip29-io's identical constant. */
#define PCI_SHOEHORN_SERIAL_PLACEHOLDER 0x060504030201ULL

/*
 * The fitted Cad Duo's one-wire identity part serial when nothing sets
 * "cad-duo-onewire-serial". No real value is known, so this is the same
 * descending-byte placeholder idiom used for an unobserved NIC ROM serial
 * elsewhere; a catalogue-placed board sets the topology's synthetic identity
 * instead.
 */
#define PCI_SHOEHORN_CAD_DUO_ONEWIRE 0x0d0c0b0a0908ULL
/* The dual-serial card's record part serial, the next placeholder along. */
#define PCI_SHOEHORN_SIO_ONEWIRE 0x100f0e0d0c0bULL

struct SGIPciShoehornState {
    SGIXioBoardState parent_obj;

    MemoryRegion *fabric;
    uint64_t onewire_serial;
    char *pci_bus_name;
    uint32_t cad_duo_slot;
    uint64_t cad_duo_onewire_serial;
    uint32_t sio_slot;
    uint64_t sio_onewire_serial;

    SGIBridgeState *bridge;
};

/*
 * A card the adapter can carry: an sgi-ioc3 with the card's subsystem id,
 * population and board record. The Cad Duo keeps the class's automatic
 * population, which its placeholder subsystem id resolves to the CAD DUO
 * fall-through. serial_prop names the one-wire part the board's serial goes
 * to: the Cad Duo's station-address part, or, on the dual-serial card, which
 * has no Ethernet and so no station-address part, its record part.
 */
typedef struct PciShoehornCard {
    const char *child_name;
    const char *serial_prop;
    uint16_t subsystem_id;
    uint32_t population;
    const char *part;
    const char *barcode;
    const char *revision;
    const char *name;
} PciShoehornCard;

static const PciShoehornCard pci_shoehorn_cad_duo = {
    "cad-duo", "onewire-serial", SGI_IOC3_SUBSYS_CAD_DUO_PCI_CARD_UNCAPTURED,
    SGI_IOC3_POPULATION_AUTO,
    SGI_IOC3_CAD_DUO_BOARD_PART, SGI_IOC3_CAD_DUO_BOARD_BARCODE,
    SGI_IOC3_CAD_DUO_BOARD_REVISION, SGI_IOC3_CAD_DUO_BOARD_NAME,
};

static const PciShoehornCard pci_shoehorn_sio_ufc = {
    "sio-ufc", "board-onewire-serial", SGI_IOC3_SUBSYS_SIO_UFC_UNCAPTURED,
    SGI_IOC3_POPULATION_SERIAL_A | SGI_IOC3_POPULATION_SERIAL_B,
    SGI_IOC3_SIO_UFC_BOARD_PART, SGI_IOC3_SIO_UFC_BOARD_BARCODE,
    SGI_IOC3_SIO_UFC_BOARD_REVISION, SGI_IOC3_SIO_UFC_BOARD_NAME,
};

/*
 * Fit a card at slot on the internal Bridge's bus, with its board record on
 * the card's one-wire bus, and wire its SuperIO interrupt to the Bridge pin
 * its slot's INTB maps to.
 */
static bool pci_shoehorn_fit_card(SGIPciShoehornState *s,
                                  const PciShoehornCard *card,
                                  uint32_t slot, uint64_t onewire_serial,
                                  Error **errp)
{
    PCIBus *bus = PCI_HOST_BRIDGE(s->bridge)->bus;
    PCIDevice *ioc3;

    ioc3 = pci_new(PCI_DEVFN(slot, 0), TYPE_SGI_IOC3);
    object_property_add_child(OBJECT(s), card->child_name, OBJECT(ioc3));
    qdev_prop_set_uint16(DEVICE(ioc3), "subsystem-id", card->subsystem_id);
    qdev_prop_set_uint32(DEVICE(ioc3), "population", card->population);
    qdev_prop_set_uint64(DEVICE(ioc3), card->serial_prop, onewire_serial);
    qdev_prop_set_string(DEVICE(ioc3), "board-part", card->part);
    qdev_prop_set_string(DEVICE(ioc3), "board-barcode", card->barcode);
    qdev_prop_set_string(DEVICE(ioc3), "board-revision", card->revision);
    qdev_prop_set_string(DEVICE(ioc3), "board-name", card->name);
    if (!pci_realize_and_unref(ioc3, bus, errp)) {
        return false;
    }

    /*
     * SuperIO GPIO-out index 0 leaves on the card's INTB, which the Bridge
     * routes to pin slot ^ 4. Driver behavior implies this wiring; physical
     * wiring is unverified; see this file's header.
     */
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in(DEVICE(s->bridge), slot ^ 4));
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
    if (s->sio_slot != SGI_PCI_SHOEHORN_NO_CARD &&
        s->sio_slot >= SGI_BRIDGE_PINS) {
        error_setg(errp, "sgi-pci-shoehorn sio-slot must name a Bridge "
                   "device slot, 0 to %u", SGI_BRIDGE_PINS - 1);
        return false;
    }
    if (s->cad_duo_slot != SGI_PCI_SHOEHORN_NO_CARD &&
        s->sio_slot != SGI_PCI_SHOEHORN_NO_CARD) {
        error_setg(errp, "sgi-pci-shoehorn has one PCI position: set "
                   "cad-duo-slot or sio-slot, not both");
        return false;
    }

    bridge = qdev_new(TYPE_SGI_BRIDGE);
    object_property_add_child(OBJECT(board), "bridge", OBJECT(bridge));
    s->bridge = SGI_BRIDGE(bridge);
    qdev_prop_set_uint32(bridge, "revision", SGI_SHOEHORN_BRIDGE_REVISION);
    qdev_prop_set_string(bridge, "board-part",
                         SGI_SHOEHORN_BRIDGE_BOARD_PART);
    qdev_prop_set_string(bridge, "board-barcode",
                         SGI_SHOEHORN_BRIDGE_BOARD_BARCODE);
    qdev_prop_set_string(bridge, "board-revision",
                         SGI_SHOEHORN_BRIDGE_BOARD_REVISION);
    qdev_prop_set_string(bridge, "board-name",
                         SGI_SHOEHORN_BRIDGE_BOARD_NAME);
    qdev_prop_set_uint64(bridge, "onewire-serial", s->onewire_serial);
    sgi_xio_board_name_pci_bus(SGI_XIO_BOARD(s), bridge, s->pci_bus_name);
    object_property_set_link(OBJECT(bridge), "fabric", OBJECT(s->fabric),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(bridge), errp)) {
        return false;
    }

    if (s->cad_duo_slot != SGI_PCI_SHOEHORN_NO_CARD &&
        !pci_shoehorn_fit_card(s, &pci_shoehorn_cad_duo, s->cad_duo_slot,
                               s->cad_duo_onewire_serial, errp)) {
        return false;
    }
    if (s->sio_slot != SGI_PCI_SHOEHORN_NO_CARD &&
        !pci_shoehorn_fit_card(s, &pci_shoehorn_sio_ufc, s->sio_slot,
                               s->sio_onewire_serial, errp)) {
        return false;
    }

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);
    board->link_reset = qdev_get_gpio_in_named(DEVICE(s->bridge),
                                                "link-reset", 0);
    board->link_chip = OBJECT(s->bridge);

    /* Same slot fabric claim as sgi-pci-expansion; see its header. */
    return true;
}

static const Property pci_shoehorn_properties[] = {
    DEFINE_PROP_LINK("fabric", SGIPciShoehornState, fabric,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT64("onewire-serial", SGIPciShoehornState,
                       onewire_serial, PCI_SHOEHORN_SERIAL_PLACEHOLDER),
    DEFINE_PROP_STRING("pci-bus-name", SGIPciShoehornState, pci_bus_name),
    DEFINE_PROP_UINT32("cad-duo-slot", SGIPciShoehornState, cad_duo_slot,
                       SGI_PCI_SHOEHORN_NO_CARD),
    DEFINE_PROP_UINT64("cad-duo-onewire-serial", SGIPciShoehornState,
                       cad_duo_onewire_serial, PCI_SHOEHORN_CAD_DUO_ONEWIRE),
    DEFINE_PROP_UINT32("sio-slot", SGIPciShoehornState, sio_slot,
                       SGI_PCI_SHOEHORN_NO_CARD),
    DEFINE_PROP_UINT64("sio-onewire-serial", SGIPciShoehornState,
                       sio_onewire_serial, PCI_SHOEHORN_SIO_ONEWIRE),
};

/* The internal Bridge and IOC3 carry all state. */
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
