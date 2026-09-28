/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-pci-expansion: an Origin 2000 module's optional XIO PCI expansion
 * board on the sgi-xio-board base class.
 *
 * The Origin 2000 module descriptor reserves XIO position IO2 for an
 * optional PCI expansion, but does not establish whether a given system has
 * one fitted, nor its numeric Crossbow widget identity or any onboard
 * population beyond a bare Bridge and PCI bus. Unlike sgi-ip29-io and
 * sgi-baseio, no reviewed source gives this board a fixed onboard device
 * population or a real board part number, so this class is the family's one
 * `-device` demonstrator: a plain Bridge, its PCI bus named so a following
 * `-device ...,bus=<name>` can populate it.
 *
 * `-device`-creatable at an empty XIO slot it names (slot=io2): the
 * machine hands it the link to that slot's Crossbow port, which gives
 * the board its fabric before build_widget() runs and attaches the
 * board's Bridge to the Crossbow once it has (xio-board.c). A processor
 * or peer widget then reaches this board's Bridge through its own
 * Crossbow, and the board's Bridge sends packets and interrupts out to
 * any populated widget -- the same both-way path an onboard widget has.
 * A slot the topology does not name, or one already filled, is refused.
 *
 * Bridge's "board-part" is a required, exactly-12-character identity register
 * value (hw/pci-host/sgi-bridge.h, sgi_bridge_board_part_error()); with no
 * observed part number for the bare board, the class defaults to the same
 * explicit "not observed" placeholder already established for an XBridge head
 * with no board behind it (hw/pci-host/sgi-xbridge.h,
 * SGI_XBRIDGE_NIC_PLACEHOLDER_PART). A product built on this class sets
 * "board-part", "board-barcode", "board-revision" and "board-name" to its
 * board-identity record and "bridge-revision" to its Bridge chip's revision;
 * they pass straight through to the internal Bridge. With "board-name" unset
 * the Bridge keeps its older text page, and with "bridge-revision" 0 its
 * default revision.
 *
 * "slots" limits the PCI device positions a card may take to 0 .. slots-1,
 * for a carrier with fewer physical slots than the Bridge has device
 * positions:
 * the Origin 2000 PCI Carrier Assembly has three (itser's guide, 007-3456-003
 * printed page 34). Which device number each physical slot is wired to is not
 * established, so the first three are an approximation. 0, the default,
 * leaves every Bridge device position open.
 *
 * "fabric" passes straight through to the internal Bridge; the link to the
 * board's slot sets it.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xio-link.h"
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

/* Placeholder; see sgi-ip29-io's identical constant. */
#define PCI_EXPANSION_SERIAL_PLACEHOLDER 0x060504030201ULL

struct SGIPciExpansionState {
    SGIXioBoardState parent_obj;

    MemoryRegion *fabric;
    uint64_t onewire_serial;
    char *pci_bus_name;
    uint32_t slots;
    char *board_part;
    char *board_barcode;
    char *board_revision;
    char *board_name;
    uint32_t bridge_revision;

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
    if (s->bridge_revision) {
        qdev_prop_set_uint32(bridge, "revision", s->bridge_revision);
    }
    qdev_prop_set_string(bridge, "board-part",
                         s->board_part ? s->board_part :
                         SGI_XBRIDGE_NIC_PLACEHOLDER_PART);
    if (s->board_barcode) {
        qdev_prop_set_string(bridge, "board-barcode", s->board_barcode);
    }
    if (s->board_revision) {
        qdev_prop_set_string(bridge, "board-revision", s->board_revision);
    }
    if (s->board_name) {
        qdev_prop_set_string(bridge, "board-name", s->board_name);
    }
    qdev_prop_set_uint64(bridge, "onewire-serial", s->onewire_serial);
    sgi_xio_board_name_pci_bus(SGI_XIO_BOARD(s), bridge, s->pci_bus_name);
    object_property_set_link(OBJECT(bridge), "fabric", OBJECT(s->fabric),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(bridge), errp)) {
        return false;
    }
    if (s->slots) {
        pci_bus_set_slot_reserved_mask(PCI_HOST_BRIDGE(bridge)->bus,
                                       ~MAKE_64BIT_MASK(0, s->slots));
    }

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);

    return true;
}

static const Property pci_expansion_properties[] = {
    DEFINE_PROP_LINK("fabric", SGIPciExpansionState, fabric,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT64("onewire-serial", SGIPciExpansionState,
                       onewire_serial, PCI_EXPANSION_SERIAL_PLACEHOLDER),
    DEFINE_PROP_STRING("pci-bus-name", SGIPciExpansionState, pci_bus_name),
    DEFINE_PROP_UINT32("slots", SGIPciExpansionState, slots, 0),
    DEFINE_PROP_STRING("board-part", SGIPciExpansionState, board_part),
    DEFINE_PROP_STRING("board-barcode", SGIPciExpansionState, board_barcode),
    DEFINE_PROP_STRING("board-revision", SGIPciExpansionState,
                       board_revision),
    DEFINE_PROP_STRING("board-name", SGIPciExpansionState, board_name),
    DEFINE_PROP_UINT32("bridge-revision", SGIPciExpansionState,
                       bridge_revision, 0),
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
    /* See the header above for what "fabric" resolves against. */
    dc->user_creatable = true;
    device_class_set_props(dc, pci_expansion_properties);
    bc->build_widget = pci_expansion_build_widget;
}

/*
 * The Origin 2000 PCI Carrier Assembly: three PCI slots for third-party
 * cards (007-3456-003 printed page 34), fitted in XIO slot IO2. The slots
 * take PCI devices 0 to 2, an approximation: no source gives the
 * carrier's device wiring.
 *
 * Its board-identity record and Bridge revision
 * (docs/datasheets/sgi-pci-shoebox.md in the Origami repository,
 * "Board-identity record"): a real Origin 2000's PCI XIO board in slot 2
 * prints part 030-1062-002, revision C and name PCI-XIO, and SGI's part
 * table names 030-1062-002 the Origin 2000 PCI cardcage. That this
 * carrier is that cardcage is inferred: the owner's guide names no part.
 * Both real PCI_XIO boards observed report Bridge revision 3. The
 * barcode is a placeholder.
 */
#define TYPE_SGI_PCI_CARRIER "sgi-pci-carrier"

static void pci_carrier_instance_init(Object *obj)
{
    SGIPciExpansionState *s = SGI_PCI_EXPANSION(obj);

    s->slots = 3;
    s->board_part = g_strdup("030-1062-002");
    s->board_barcode = g_strdup("OPCIXIO001");
    s->board_revision = g_strdup("C");
    s->board_name = g_strdup("PCI-XIO");
    s->bridge_revision = 3;
}

static void pci_carrier_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Origin 2000 PCI Carrier Assembly (three PCI slots)";
}

static const TypeInfo pci_expansion_types[] = {
    {
        .name = TYPE_SGI_PCI_EXPANSION,
        .parent = TYPE_SGI_XIO_BOARD,
        .instance_size = sizeof(SGIPciExpansionState),
        .class_init = pci_expansion_class_init,
    },
    {
        .name = TYPE_SGI_PCI_CARRIER,
        .parent = TYPE_SGI_PCI_EXPANSION,
        .instance_init = pci_carrier_instance_init,
        .class_init = pci_carrier_class_init,
    },
};

static void pci_expansion_register_types(void)
{
    type_register_static_array(pci_expansion_types,
                               ARRAY_SIZE(pci_expansion_types));
}

type_init(pci_expansion_register_types)
