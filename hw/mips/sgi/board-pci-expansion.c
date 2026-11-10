/* SPDX-License-Identifier: BSD-3-Clause */
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
 * "slots" limits the PCI device positions a card may take to
 * first-slot .. first-slot + slots - 1, for a carrier with fewer physical
 * slots than the Bridge has device positions: the Origin 2000 PCI Carrier
 * Assembly has three (itser's guide, 007-3456-003 printed page 34). Which
 * device number each physical slot is wired to is not established, so the
 * first three are an approximation. 0, the default, leaves every Bridge
 * device position open. "first-slot" defaults to 0.
 *
 * "flash-empty-reads-high" makes a read of a flash chip select with nothing
 * fitted complete with all ones instead of failing, for a board whose
 * empty select is not known to fail. A write to it still fails.
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
#include "hw/mips/sgi/sgi-synthetic-serial.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "hw/misc/sgi-ioc3.h"
#include "system/system.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_SGI_PCI_EXPANSION "sgi-pci-expansion"
OBJECT_DECLARE_SIMPLE_TYPE(SGIPciExpansionState, SGI_PCI_EXPANSION)

/* Marked synthetic; no reviewed source assigns a serial to this board. */
#define PCI_EXPANSION_SERIAL_PLACEHOLDER SGI_SYNTHETIC_ONEWIRE_SERIAL('B', 0)

struct SGIPciExpansionState {
    SGIXioBoardState parent_obj;

    MemoryRegion *fabric;
    uint64_t onewire_serial;
    char *pci_bus_name;
    uint32_t slots;
    uint32_t first_slot;
    bool flash_empty_reads_high;
    char *board_part;
    char *board_barcode;
    char *board_revision;
    char *board_name;
    uint32_t bridge_revision;
    /* The Fibre Channel boards' product, resolved when the board is built. */
    char *fc_product;

    SGIBridgeState *bridge;
    Notifier machine_done;
};

/*
 * A card fitted with -device at one of the board's slots: an IOC3 there
 * sends its SuperIO interrupt out on the card's INTB, which the Bridge
 * takes on pin (device ^ 4), as the shoehorn wires its own fitted card.
 * The cards exist only once the command line's devices are made, so this
 * runs when machine creation is done, and leaves any output already wired.
 */
static void pci_expansion_wire_card(PCIBus *bus, PCIDevice *dev, void *opaque)
{
    SGIPciExpansionState *s = opaque;
    unsigned int pin = PCI_SLOT(dev->devfn) ^ 4;

    if (!object_dynamic_cast(OBJECT(dev), TYPE_SGI_IOC3) ||
        qdev_get_gpio_out_connector(DEVICE(dev), NULL, 0) ||
        pin >= SGI_BRIDGE_PINS) {
        return;
    }
    qdev_connect_gpio_out(DEVICE(dev), 0,
                          qdev_get_gpio_in(DEVICE(s->bridge), pin));
}

static void pci_expansion_machine_done(Notifier *notifier, void *data)
{
    SGIPciExpansionState *s = container_of(notifier, SGIPciExpansionState,
                                           machine_done);

    pci_for_each_device(PCI_HOST_BRIDGE(s->bridge)->bus, 0,
                        pci_expansion_wire_card, s);
}

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
    if (s->slots && (s->first_slot >= SGI_BRIDGE_PINS ||
                     s->slots > SGI_BRIDGE_PINS - s->first_slot)) {
        error_setg(errp, "sgi-pci-expansion first-slot %u with %u slots "
                   "runs past Bridge device %u", s->first_slot, s->slots,
                   SGI_BRIDGE_PINS - 1);
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
    } else if (s->board_name) {
        char barcode[SGI_SYNTHETIC_BARCODE_SIZE];

        sgi_synthetic_barcode(barcode, 'O', s->onewire_serial);
        qdev_prop_set_string(bridge, "board-barcode", barcode);
    }
    if (s->board_revision) {
        qdev_prop_set_string(bridge, "board-revision", s->board_revision);
    }
    if (s->board_name) {
        qdev_prop_set_string(bridge, "board-name", s->board_name);
    }
    qdev_prop_set_uint64(bridge, "onewire-serial", s->onewire_serial);
    qdev_prop_set_bit(bridge, "flash-empty-reads-high",
                      s->flash_empty_reads_high);
    sgi_xio_board_name_pci_bus(SGI_XIO_BOARD(s), bridge, s->pci_bus_name);
    object_property_set_link(OBJECT(bridge), "fabric", OBJECT(s->fabric),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(bridge), errp)) {
        return false;
    }
    if (s->slots) {
        pci_bus_set_slot_reserved_mask(PCI_HOST_BRIDGE(bridge)->bus,
                                       ~MAKE_64BIT_MASK(s->first_slot,
                                                        s->slots));
    }

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);
    s->machine_done.notify = pci_expansion_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
    board->link_reset = qdev_get_gpio_in_named(DEVICE(s->bridge),
                                                "link-reset", 0);
    board->link_chip = OBJECT(s->bridge);

    return true;
}

static const Property pci_expansion_properties[] = {
    DEFINE_PROP_LINK("fabric", SGIPciExpansionState, fabric,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT64("onewire-serial", SGIPciExpansionState,
                       onewire_serial, PCI_EXPANSION_SERIAL_PLACEHOLDER),
    DEFINE_PROP_STRING("pci-bus-name", SGIPciExpansionState, pci_bus_name),
    DEFINE_PROP_UINT32("slots", SGIPciExpansionState, slots, 0),
    DEFINE_PROP_UINT32("first-slot", SGIPciExpansionState, first_slot, 0),
    DEFINE_PROP_BOOL("flash-empty-reads-high", SGIPciExpansionState,
                     flash_empty_reads_high, false),
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

/* A board that realized took a machine-done notifier; it goes with it. */
static void pci_expansion_instance_finalize(Object *obj)
{
    SGIPciExpansionState *s = SGI_PCI_EXPANSION(obj);

    if (s->machine_done.notify) {
        qemu_remove_machine_init_done_notifier(&s->machine_done);
    }
}

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
 * A real Origin 2000's PCI XIO board in slot 2
 * prints part 030-1062-002, revision C and name PCI-XIO, and SGI's part
 * table names 030-1062-002 the Origin 2000 PCI cardcage. That this
 * carrier is that cardcage is inferred: the owner's guide names no part.
 * Both real PCI_XIO boards observed report Bridge revision 3. The
 * barcode is the marked synthetic one the build makes from the serial.
 */
#define TYPE_SGI_PCI_CARRIER "sgi-pci-carrier"

static void pci_carrier_instance_init(Object *obj)
{
    SGIPciExpansionState *s = SGI_PCI_EXPANSION(obj);

    s->slots = 3;
    s->board_part = g_strdup("030-1062-002");
    s->board_revision = g_strdup("C");
    s->board_name = g_strdup("PCI-XIO");
    s->bridge_revision = 3;
}

static void pci_carrier_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Origin 2000 PCI Carrier Assembly (three PCI slots)";
}

/*
 * The Octane's PCI module, SGI's "OCTANE PCI Module" (the PCI card cage): a
 * three-slot PCI board on the Crossbow port of its own, 13, beside the four
 * XIO slots at 9 to 12 (installation guide 007-3547-001 printed pages 3 and
 * 13). Slot N of the module is taken to be PCI device N, so it holds
 * devices 1 to 3; the guide numbers the slots but not their IDSEL wiring, so
 * that is inferred, not established.
 *
 * Real Octane inventories print the board as part 030-0952-005 revision E,
 * name XTALKPCI. Its Bridge revision is unknown; the onboard Octane
 * Bridge's, 3, stands in. A board with no flash fitted is not known to fault
 * when the flash window is read, so both chip selects read high.
 */
#define TYPE_SGI_OCTANE_PCI_CAGE "sgi-octane-pci-cage"

static void octane_pci_cage_instance_init(Object *obj)
{
    SGIPciExpansionState *s = SGI_PCI_EXPANSION(obj);

    s->slots = 3;
    s->first_slot = 1;
    s->board_part = g_strdup("030-0952-005");
    s->board_revision = g_strdup("E");
    s->board_name = g_strdup("XTALKPCI");
    s->bridge_revision = 3;
    s->flash_empty_reads_high = true;
}

static void octane_pci_cage_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "Octane PCI module (three PCI slots, Crossbow port 13)";
}

/*
 * The Fibre Channel XIO boards: a Bridge with an Adaptec AIC-1160 on each of
 * PCI devices 0 and 1, one per port. The two ports are two chips on two
 * Bridge slots, which the published hardware-graph paths show as pci/0 and
 * pci/1 under one node. The board takes no other card, so only those
 * positions are open. Each controller's interrupt reaches the Bridge on the
 * pin of its device number. A port's loop is reached through the controller
 * (see hw/scsi/adaptec-aic1160.c).
 *
 * Which product it is follows the machine. The XT-FC-2P, part 030-0927-00x,
 * is the Origin 2000, Origin 200 and Onyx2 board; the X9-FC-2P, part
 * 013-2119-00x, is the identical Octane board. Both are copper and FC-AL
 * only, with no fabric login. The optical -B boards are separate products
 * whose controller is not established, so they are not modelled. The last
 * digit of either part number is not known. The one observed XT-FC-2P
 * carries assembly 030-0927003E, read here as part 030-0927-003, and the
 * revision E goes unrecorded because the Bridge's record takes a revision
 * only with a board name, which is not known. The X9-FC-2P keeps the bare
 * board's placeholder part. No source gives the Bridge revision, so it keeps
 * the default.
 */
#define TYPE_SGI_FC_BOARD "sgi-fc-board"
#define FC_BOARD_PORTS 2

typedef struct FCProduct {
    const char *name;
    const char *part;
} FCProduct;

static const FCProduct fc_products[] = {
    { "xt-fc-2p", "030-0927-003" },
    { "x9-fc-2p", NULL },
};

static const FCProduct *fc_find_product(const char *name)
{
    for (int i = 0; i < ARRAY_SIZE(fc_products); i++) {
        if (g_str_equal(fc_products[i].name, name)) {
            return &fc_products[i];
        }
    }
    return NULL;
}

static char *fc_get_product(Object *obj, Error **errp)
{
    return g_strdup(SGI_PCI_EXPANSION(obj)->fc_product);
}

static void fc_set_product(Object *obj, const char *value, Error **errp)
{
    SGIPciExpansionState *s = SGI_PCI_EXPANSION(obj);

    if (!fc_find_product(value)) {
        error_setg(errp, "unknown Fibre Channel board product '%s'; the "
                   "products are xt-fc-2p and x9-fc-2p", value);
        return;
    }
    g_free(s->fc_product);
    s->fc_product = g_strdup(value);
}

static bool (*fc_board_parent_build_widget)(SGIXioBoardState *board,
                                            Error **errp);

static bool fc_board_build_widget(SGIXioBoardState *board, Error **errp)
{
    SGIPciExpansionState *s = SGI_PCI_EXPANSION(board);
    const FCProduct *product;
    PCIBus *bus;

    /* The Octane's slots are named widget-N; the racks' are io2 and up. */
    if (!s->fc_product) {
        bool octane = board->slot_name &&
                      g_str_has_prefix(board->slot_name, "widget-");

        s->fc_product = g_strdup(octane ? "x9-fc-2p" : "xt-fc-2p");
    }
    product = fc_find_product(s->fc_product);
    s->slots = FC_BOARD_PORTS;
    /*
     * The IP30 kernel probes the flash window of every PCI-mode Bridge
     * without a fault handler, and a failed read there panics it. No flash
     * is modelled on this board, so an empty select reads high.
     */
    s->flash_empty_reads_high = true;
    if (product->part) {
        g_free(s->board_part);
        s->board_part = g_strdup(product->part);
    }
    if (!fc_board_parent_build_widget(board, errp)) {
        return false;
    }
    bus = PCI_HOST_BRIDGE(s->bridge)->bus;
    for (unsigned port = 0; port < FC_BOARD_PORTS; port++) {
        g_autofree char *name = g_strdup_printf("port%u", port);
        PCIDevice *fc = pci_new(PCI_DEVFN(port, 0), "aic1160");

        /* Each port has a name of its own, a placeholder: none is known. */
        qdev_prop_set_uint64(DEVICE(fc), "wwn",
                             UINT64_C(0x2100000000000001) + port);
        /* A board with an id names each port's loop bus after it. */
        if (DEVICE(board)->id) {
            g_autofree char *scsi = g_strdup_printf("%s-p%u",
                                                   DEVICE(board)->id, port);

            qdev_prop_set_string(DEVICE(fc), "bus-name", scsi);
        }
        object_property_add_child(OBJECT(board), name, OBJECT(fc));
        if (!pci_realize_and_unref(fc, bus, errp)) {
            return false;
        }
    }
    return true;
}

static void fc_board_instance_finalize(Object *obj)
{
    g_free(SGI_PCI_EXPANSION(obj)->fc_product);
}

static void fc_board_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);

    dc->desc = "Fibre Channel XIO board (XT-FC-2P or X9-FC-2P)";
    object_class_property_add_str(oc, "product", fc_get_product,
                                  fc_set_product);
    object_class_property_set_description(oc, "product",
        "xt-fc-2p or x9-fc-2p; by default x9-fc-2p in an Octane slot and "
        "xt-fc-2p elsewhere");
    fc_board_parent_build_widget = bc->build_widget;
    bc->build_widget = fc_board_build_widget;
}

static const TypeInfo pci_expansion_types[] = {
    {
        .name = TYPE_SGI_PCI_EXPANSION,
        .parent = TYPE_SGI_XIO_BOARD,
        .instance_size = sizeof(SGIPciExpansionState),
        .instance_finalize = pci_expansion_instance_finalize,
        .class_init = pci_expansion_class_init,
    },
    {
        .name = TYPE_SGI_PCI_CARRIER,
        .parent = TYPE_SGI_PCI_EXPANSION,
        .instance_init = pci_carrier_instance_init,
        .class_init = pci_carrier_class_init,
    },
    {
        .name = TYPE_SGI_OCTANE_PCI_CAGE,
        .parent = TYPE_SGI_PCI_EXPANSION,
        .instance_init = octane_pci_cage_instance_init,
        .class_init = octane_pci_cage_class_init,
    },
    {
        .name = TYPE_SGI_FC_BOARD,
        .parent = TYPE_SGI_PCI_EXPANSION,
        .instance_finalize = fc_board_instance_finalize,
        .class_init = fc_board_class_init,
    },
};

static void pci_expansion_register_types(void)
{
    type_register_static_array(pci_expansion_types,
                               ARRAY_SIZE(pci_expansion_types));
}

type_init(pci_expansion_register_types)
