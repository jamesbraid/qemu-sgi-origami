/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-xio-baseio: the Origin 2000 and Onyx2 standard XIO I/O board
 * (BaseIO), an sgi-xio-board subclass that creates its own Bridge and an
 * sgi-baseio (the "io6" preset) behind it -- the removable-card
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
 * "parallel-chardev" (the internal sgi-baseio's "io6" preset never
 * wires one either). "preset" selects which internal sgi-baseio preset
 * this card builds, "io6" by default; "io6g" builds the same
 * card with the MediaIO daughtercard's second IOC3 fitted. Both name
 * the same physical board part and Bridge revision, so this class's own
 * fitted-part identity is looked up from the same preset. "flash-drive"
 * names the image behind the card's own flash part on the Bridge's flash
 * port, the BASEIO PROM the IP27 PROM loads from it; this file owns that
 * part too, since it is a fitted part of the card and not of the
 * sgi-baseio population.
 */

#include "qemu/osdep.h"
#include "hw/block/flash.h"
#include "hw/mips/sgi/baseio.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xtalk-link.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/pci-host/sgi-bridge.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "system/block-backend.h"

#define TYPE_SGI_XIO_BASEIO "sgi-xio-baseio"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXioBaseioState, SGI_XIO_BASEIO)

/* BaseIO is fixed at IO1 (ip27-origin2000.yaml, baseio-bridge widget 8). */
#define XIO_BASEIO_WIDGET 8

/* Placeholder; no reviewed source assigns a serial to this board class. */
#define XIO_BASEIO_SERIAL_PLACEHOLDER 0x060504030201ULL

/*
 * The card's 1 MB flash EPROM on the Bridge's flash port, holding the
 * BASEIO PROM (baseio.md, "the 1 MiB flash EPROM attaches on the
 * Bridge"): a 512K x16 top-boot part answering the AMD command set with
 * manufacturer 0x01 and device 0x22da, an identity the IP27 PROM's
 * loader accepts and Linux's JEDEC table names AM29LV800BT. The port's
 * unlock addresses are the x16 word addresses.
 */
#define XIO_BASEIO_FLASH_MANUFACTURER 0x01
#define XIO_BASEIO_FLASH_DEVICE       0x22da
#define XIO_BASEIO_FLASH_UNLOCK0      0x555
#define XIO_BASEIO_FLASH_UNLOCK1      0x2aa

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

    SGIBridgeState *bridge;
    SGIBaseioState *baseio;
    PFlashCFI02 *flash;
};

/*
 * The flash part, its contents from "flash-drive" when one is named: the
 * image must be the part's own 1 MB. Without a drive the part is fitted
 * blank and reads as zeros, not the erased 0xff a real unprogrammed chip
 * would show; the firmware's loader rejects either and falls back to its
 * internal copy.
 */
static bool xio_baseio_build_flash(SGIXioBaseioState *s, Error **errp)
{
    SGIXioBoardState *board = SGI_XIO_BOARD(s);
    g_autofree char *name = g_strdup_printf("sgi-xio-baseio.module%u.flash",
                                            board->module);
    DeviceState *flash = qdev_new(TYPE_PFLASH_CFI02);

    object_property_add_child(OBJECT(s), "flash", OBJECT(flash));
    if (s->flash_drive_id) {
        BlockBackend *blk = blk_by_name(s->flash_drive_id);

        if (!blk) {
            error_setg(errp, "sgi-xio-baseio: no drive named '%s' for "
                      "flash-drive", s->flash_drive_id);
            return false;
        }
        if (!qdev_prop_set_drive_err(flash, "drive", blk, errp)) {
            return false;
        }
    }
    /* Top boot: fifteen 64 KB sectors, then 32 KB, two 8 KB and 16 KB. */
    qdev_prop_set_uint32(flash, "num-blocks0", 15);
    qdev_prop_set_uint32(flash, "sector-length0", 64 * KiB);
    qdev_prop_set_uint32(flash, "num-blocks1", 1);
    qdev_prop_set_uint32(flash, "sector-length1", 32 * KiB);
    qdev_prop_set_uint32(flash, "num-blocks2", 2);
    qdev_prop_set_uint32(flash, "sector-length2", 8 * KiB);
    qdev_prop_set_uint32(flash, "num-blocks3", 1);
    qdev_prop_set_uint32(flash, "sector-length3", 16 * KiB);
    qdev_prop_set_uint8(flash, "width", 2);
    qdev_prop_set_uint8(flash, "big-endian", 1);
    qdev_prop_set_uint16(flash, "id0", XIO_BASEIO_FLASH_MANUFACTURER);
    qdev_prop_set_uint16(flash, "id1", XIO_BASEIO_FLASH_DEVICE);
    qdev_prop_set_uint16(flash, "unlock-addr0", XIO_BASEIO_FLASH_UNLOCK0);
    qdev_prop_set_uint16(flash, "unlock-addr1", XIO_BASEIO_FLASH_UNLOCK1);
    qdev_prop_set_string(flash, "name", name);
    if (!sysbus_realize(SYS_BUS_DEVICE(flash), errp)) {
        return false;
    }
    s->flash = PFLASH_CFI02(flash);
    return true;
}

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
    sgi_xtalk_crossbow_link_claim_fabric(s->fabric);
    if (board->slot != XIO_BASEIO_WIDGET) {
        error_setg(errp, "sgi-xio-baseio is fixed at crosstalk widget %u",
                  XIO_BASEIO_WIDGET);
        return false;
    }
    if (!identity) {
        error_setg(errp, "sgi-xio-baseio: '%s' names no known baseio "
                  "preset; expected io6 or io6g",
                  s->preset ? s->preset : "(unset)");
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
    if (!xio_baseio_build_flash(s, errp)) {
        return false;
    }
    object_property_set_link(
        OBJECT(bridge), "flash",
        OBJECT(sysbus_mmio_get_region(SYS_BUS_DEVICE(s->flash), 0)),
        &error_abort);
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
    if (!qdev_realize(DEVICE(baseio), NULL, errp)) {
        return false;
    }

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);
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
    /*
     * Ordinarily fitted per module by the machine, at most one per
     * deskside enclosure, but also `-device`-creatable: a late board's
     * own "fabric" can resolve against the spare fabric proxy a Crossbow
     * machine builder now holds back for it (sgi_machine_build_crossbars()
     * in sn-machine.c), claiming it (build_widget()'s own
     * sgi_xtalk_crossbow_link_claim_fabric() call) so sgi_machine_done()
     * brings the underlying widget alive once it finishes the proxy --
     * the same outbound-only path onto a real Crossbow that
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
