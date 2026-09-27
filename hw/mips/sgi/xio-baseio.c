/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-xio-baseio: an Origin 2000 and Onyx2 XIO I/O board built from a
 * Bridge and an sgi-baseio preset behind it -- the standard BaseIO
 * ("io6"), the BaseIO-G ("io6g"), the MENET ("menet") or the MSCSI
 * ("mscsi"). An sgi-xio-board subclass: the removable-card carrier
 * docs/designs/sn-machine-model-architecture.md's "Base I/O"
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
 * "record-a" and "record-b" forward the same way, unset by default.
 * "parallel-chardev" forwards the same way too, to the MediaIO
 * daughtercard's parallel port the "io6g" preset fits; the plain BaseIO
 * has no parallel function, and the internal sgi-baseio refuses the id
 * under "io6". "mio-chardev-a" and "mio-chardev-b" forward the
 * same way to the internal sgi-baseio's own same-named properties, the
 * MediaIO daughtercard's two serial ports the "io6g" preset fits --
 * peripheral connections, not a console or record backend, so neither
 * has a record counterpart, and both are unset (the ports enumerate
 * unbacked) under the "io6" preset, which never builds that chip.
 * "preset" selects which internal sgi-baseio preset
 * this card builds, "io6" by default; "io6g" builds the same
 * card with the MediaIO daughtercard's second IOC3 fitted; "menet"
 * builds the four-IOC3 MENET board and "mscsi" the four-ISP1040 MSCSI
 * board. This class's own fitted-part identity, the widget a card must
 * sit at and whether its Bridge carries a flash part are all looked up
 * from the same preset: the BaseIO presets are fixed at IO1 with the
 * flash, the MENET and MSCSI fit any XIO slot and have no flash, and a
 * preset that is not an XIO card is refused. The MENET's chips, and the
 * MSCSI, have no console, record, parallel or clock backend to take; the
 * internal sgi-baseio refuses a parallel id, and this class a flash id,
 * for a preset without the part. "flash-drive"
 * names the image behind the card's own flash part on the Bridge's flash
 * port, the BASEIO PROM the IP27 PROM loads from it. The card fits that
 * part, not the sgi-baseio population, so this file places it; the part
 * itself is the one hw/mips/sgi/baseio-flash.c builds, which the
 * GIGAchannel cabinet fits as well.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/baseio.h"
#include "hw/mips/sgi/baseio-flash.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xtalk-link.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/pci-host/sgi-bridge.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_SGI_XIO_BASEIO "sgi-xio-baseio"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXioBaseioState, SGI_XIO_BASEIO)

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
    char *preset;
    char *nvram_drive_id;
    char *flash_drive_id;
    char *chardev_a_id;
    char *chardev_b_id;
    char *record_a_id;
    char *record_b_id;
    char *mio_chardev_a_id;
    char *mio_chardev_b_id;
    char *parallel_chardev_id;

    SGIBridgeState *bridge;
    SGIBaseioState *baseio;
    PFlashCFI02 *flash;
};

static bool xio_baseio_build_widget(SGIXioBoardState *board, Error **errp)
{
    SGIXioBaseioState *s = SGI_XIO_BASEIO(board);
    const SGIBaseioPresetIdentity *identity =
        sgi_baseio_preset_lookup(s->preset);
    DeviceState *bridge, *baseio;
    PCIBus *bus;

    if (!s->fabric) {
        error_setg(errp, "sgi-xio-baseio requires the fabric its interrupt "
                  "packets travel");
        return false;
    }
    if (!identity || !identity->xio_widgets) {
        error_setg(errp, "sgi-xio-baseio: '%s' names no XIO card "
                  "preset; expected io6, io6g, menet or mscsi",
                  s->preset ? s->preset : "(unset)");
        return false;
    }
    if (board->slot >= 32 ||
        !(identity->xio_widgets & SGI_BASEIO_XIO_WIDGET(board->slot))) {
        error_setg(errp, "sgi-xio-baseio preset '%s' cannot sit at "
                  "crosstalk widget %u", s->preset, board->slot);
        return false;
    }
    if (!identity->bridge_flash && s->flash_drive_id) {
        error_setg(errp, "sgi-xio-baseio preset '%s' fits no flash part for "
                  "flash-drive '%s'", s->preset, s->flash_drive_id);
        return false;
    }

    bridge = qdev_new(TYPE_SGI_BRIDGE);
    object_property_add_child(OBJECT(board), "bridge", OBJECT(bridge));
    s->bridge = SGI_BRIDGE(bridge);
    sgi_baseio_preset_set_bridge_identity(bridge, identity);
    qdev_prop_set_uint64(bridge, "onewire-serial", s->onewire_serial);
    sgi_xio_board_name_pci_bus(SGI_XIO_BOARD(s), bridge, s->pci_bus_name);
    object_property_set_link(OBJECT(bridge), "fabric", OBJECT(s->fabric),
                             &error_abort);
    qdev_prop_set_uint64(bridge, "fabric-base", s->fabric_base);
    qdev_prop_set_uint32(bridge, "fabric-node-shift", s->fabric_node_shift);
    qdev_prop_set_uint32(bridge, "fabric-widget-shift",
                         s->fabric_widget_shift);
    if (identity->bridge_flash) {
        /* Per slot: a module can hold a second BaseIO-preset card. */
        g_autofree char *name = g_strdup_printf(
            "sgi-xio-baseio.module%u.widget%u.flash", board->module,
            board->slot);

        s->flash = sgi_baseio_flash_new(OBJECT(s), name, s->flash_drive_id,
                                        errp);
        if (!s->flash) {
            return false;
        }
        object_property_set_link(
            OBJECT(bridge), "flash",
            OBJECT(sysbus_mmio_get_region(SYS_BUS_DEVICE(s->flash), 0)),
            &error_abort);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(bridge), errp)) {
        return false;
    }
    bus = PCI_HOST_BRIDGE(bridge)->bus;

    baseio = qdev_new(TYPE_SGI_BASEIO);
    object_property_add_child(OBJECT(board), "baseio", OBJECT(baseio));
    s->baseio = SGI_BASEIO(baseio);
    qdev_prop_set_string(baseio, "preset", s->preset);
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
    if (s->mio_chardev_a_id) {
        qdev_prop_set_string(baseio, "mio-chardev-a", s->mio_chardev_a_id);
    }
    if (s->mio_chardev_b_id) {
        qdev_prop_set_string(baseio, "mio-chardev-b", s->mio_chardev_b_id);
    }
    if (s->parallel_chardev_id) {
        qdev_prop_set_string(baseio, "parallel-chardev",
                             s->parallel_chardev_id);
    }
    if (!qdev_realize(DEVICE(baseio), NULL, errp)) {
        return false;
    }

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);

    /*
     * Claim the slot's fabric proxy now the Bridge's own faces exist, so
     * sgi_machine_done() both brings the slot widget's link alive for this
     * board's outbound packets and registers those faces as the widget's
     * inbound destination. A fixture wiring "fabric" to a plain sink is a
     * no-op here.
     */
    sgi_xtalk_crossbow_link_claim_fabric(s->fabric, board->cpu_face,
                                         board->packet_face);
    return true;
}

static void xio_baseio_instance_init(Object *obj)
{
    SGIXioBaseioState *s = SGI_XIO_BASEIO(obj);

    /* The plain board by default; a machine asks for "io6g" instead. */
    s->preset = g_strdup("io6");
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
    DEFINE_PROP_STRING("preset", SGIXioBaseioState, preset),
    DEFINE_PROP_STRING("nvram-drive", SGIXioBaseioState, nvram_drive_id),
    DEFINE_PROP_STRING("flash-drive", SGIXioBaseioState, flash_drive_id),
    DEFINE_PROP_STRING("chardev-a", SGIXioBaseioState, chardev_a_id),
    DEFINE_PROP_STRING("chardev-b", SGIXioBaseioState, chardev_b_id),
    DEFINE_PROP_STRING("record-a", SGIXioBaseioState, record_a_id),
    DEFINE_PROP_STRING("record-b", SGIXioBaseioState, record_b_id),
    DEFINE_PROP_STRING("mio-chardev-a", SGIXioBaseioState, mio_chardev_a_id),
    DEFINE_PROP_STRING("mio-chardev-b", SGIXioBaseioState, mio_chardev_b_id),
    DEFINE_PROP_STRING("parallel-chardev", SGIXioBaseioState,
                       parallel_chardev_id),
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

    dc->desc = "Origin 2000 / Onyx2 Bridge-based XIO I/O board (BaseIO, "
               "MENET)";
    dc->vmsd = &vmstate_xio_baseio;
    /*
     * Ordinarily fitted per module by the machine, at most one per
     * deskside enclosure, but also `-device`-creatable at an empty XIO
     * slot it names (slot=io5): the machine hands it that slot's fabric
     * proxy, and claiming it (build_widget()'s own
     * sgi_xtalk_crossbow_link_claim_fabric() call) makes
     * sgi_machine_done() bring the slot's widget alive both ways, as
     * sgi-pci-expansion's own header comment describes.
     */
    dc->user_creatable = true;
    device_class_set_props(dc, xio_baseio_properties);
    bc->build_widget = xio_baseio_build_widget;
}

static const TypeInfo xio_baseio_type = {
    .name = TYPE_SGI_XIO_BASEIO,
    .parent = TYPE_SGI_XIO_BOARD,
    .instance_size = sizeof(SGIXioBaseioState),
    .instance_init = xio_baseio_instance_init,
    .class_init = xio_baseio_class_init,
};

static void xio_baseio_register_types(void)
{
    type_register_static(&xio_baseio_type);
}

type_init(xio_baseio_register_types)
