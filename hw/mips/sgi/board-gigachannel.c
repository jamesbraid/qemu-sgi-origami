/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-gigachannel: the Origin 200 GIGAchannel expansion cabinet's own I/O,
 * as the cabinet's Crossbow sees it on link 8 (docs/datasheets/
 * sgi-gigachannel.md in the Origami repository): a Bridge with four PCI
 * openings, the board record the firmware names io7 from, and the
 * cabinet's system-controller status word on the Bridge's external-flash
 * port. The cabinet's Crossbow, and the attached module's own IP29 Bridge
 * on link 9, are the machine's to build (hw/mips/sgi/sn-machine.c).
 *
 * The Bridge is widget revision 4 (Bridge rev D) in every captured
 * inventory. Its four PCI openings are labeled 0 to 3 and take Bridge PCI
 * devices 0 to 3: the one captured hardware-graph path names pci/0, and
 * the rest of that mapping is inferred. The record is the XBOX XTALK
 * INTERFACE board's, XBOX_DUALXTOWN, part 030-1252-001 rev F in every
 * capture; which one-wire line carries it is not established, so it rides
 * this Bridge's own line, the one the firmware names io7 from. The barcode
 * is a placeholder, since a real one names a physical unit.
 *
 * ---- The external-flash port ----
 *
 * The IP27 PROM loads its BASEIO PROM from the port's base chip select on a
 * GIGAchannel (window offset 0, widget offset 0xc0_0000), through the same
 * identify and copy sequence it runs at an Origin 2000 BaseIO card's
 * port. The image is established: a real cabinet's BASEIO PROM banner is
 * the build shipped as the BaseIO's flash image. Which part SGI fitted is
 * not, so the model fits the BaseIO card's part (hw/mips/sgi/
 * baseio-flash.c), an identity the loader accepts. "flash-drive" names its
 * image. With no drive named nothing is fitted: the base chip select
 * bus-errors, and the PROM retries with its own internal copy and says so
 * on the console, as it would for a cabinet whose part is missing.
 *
 * The port's other chip select (widget offset 0xe0_0000) carries the
 * cabinet's system-controller status, a 16-bit word both the BASEIO PROM
 * and the IRIX 6.5.30 kernel read on a GIGAchannel and neither writes.
 * The kernel's own event table names its low bits: bit 0 a blower failure
 * and bit 3 a power-supply over-temperature, both of which it shuts the
 * system down for; bit 4 the backup supply taking over; bit 5
 * manufacturing mode; bits 1 and 2 unknown events. Bit 6 it reads once at
 * start as a redundant supply being fitted. Those meanings, and that
 * nothing else in the word matters to software, come from the shipped
 * binaries, not from a hardware description; how the word is decoded
 * across the chip select is not known. The model answers the word as
 * "system-controller-status" -- zero by default, a cabinet with no
 * redundant supply and nothing to report -- at every 16-bit position of
 * the chip select, and logs and drops writes.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/baseio-flash.h"
#include "hw/mips/sgi/xio-board.h"
#include "hw/mips/sgi/xio-link.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_host.h"
#include "hw/pci-host/sgi-bridge.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_SGI_GIGACHANNEL "sgi-gigachannel"
OBJECT_DECLARE_SIMPLE_TYPE(SGIGigachannelState, SGI_GIGACHANNEL)

#define GIGACHANNEL_SLOTS           4
#define GIGACHANNEL_BRIDGE_REVISION 4
#define GIGACHANNEL_BOARD_PART      "030-1252-001"
#define GIGACHANNEL_BOARD_REVISION  "F"
#define GIGACHANNEL_BOARD_NAME      "XBOX_DUALXTOWN"
#define GIGACHANNEL_BOARD_BARCODE   "OXBOXDXT01"

/* Placeholder; no reviewed source assigns a serial to this board. */
#define GIGACHANNEL_SERIAL_PLACEHOLDER 0x060504030201ULL

/* One 16-bit word, repeated across the chip select by the port. */
#define GIGACHANNEL_STATUS_SIZE 2

struct SGIGigachannelState {
    SGIXioBoardState parent_obj;

    MemoryRegion *fabric;
    uint64_t onewire_serial;
    char *pci_bus_name;
    char *flash_drive_id;
    uint16_t status;

    SGIBridgeState *bridge;
    PFlashCFI02 *flash;
    MemoryRegion status_word;
};

static uint64_t gigachannel_status_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    SGIGigachannelState *s = opaque;

    return s->status;
}

static void gigachannel_status_write(void *opaque, hwaddr addr,
                                     uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "sgi-gigachannel: write of 0x%" PRIx64 " to the "
                  "system-controller status word; no source describes one, "
                  "dropped\n", value);
}

static const MemoryRegionOps gigachannel_status_ops = {
    .read = gigachannel_status_read,
    .write = gigachannel_status_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 2,
    .impl.min_access_size = 2,
    .impl.max_access_size = 2,
};

static bool gigachannel_build_widget(SGIXioBoardState *board, Error **errp)
{
    SGIGigachannelState *s = SGI_GIGACHANNEL(board);
    DeviceState *bridge;

    if (!s->fabric) {
        error_setg(errp, "sgi-gigachannel requires the fabric its interrupt "
                   "packets travel");
        return false;
    }

    memory_region_init_io(&s->status_word, OBJECT(s),
                          &gigachannel_status_ops, s,
                          "sgi-gigachannel.status", GIGACHANNEL_STATUS_SIZE);

    bridge = qdev_new(TYPE_SGI_BRIDGE);
    object_property_add_child(OBJECT(board), "bridge", OBJECT(bridge));
    s->bridge = SGI_BRIDGE(bridge);
    qdev_prop_set_uint32(bridge, "revision", GIGACHANNEL_BRIDGE_REVISION);
    qdev_prop_set_string(bridge, "board-part", GIGACHANNEL_BOARD_PART);
    qdev_prop_set_string(bridge, "board-barcode", GIGACHANNEL_BOARD_BARCODE);
    qdev_prop_set_string(bridge, "board-revision",
                         GIGACHANNEL_BOARD_REVISION);
    qdev_prop_set_string(bridge, "board-name", GIGACHANNEL_BOARD_NAME);
    qdev_prop_set_uint64(bridge, "onewire-serial", s->onewire_serial);
    sgi_xio_board_name_pci_bus(SGI_XIO_BOARD(s), bridge, s->pci_bus_name);
    if (s->flash_drive_id) {
        g_autofree char *name =
            g_strdup_printf("sgi-gigachannel.module%u.flash", board->module);

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
    object_property_set_link(OBJECT(bridge), "flash-select1",
                             OBJECT(&s->status_word), &error_abort);
    object_property_set_link(OBJECT(bridge), "fabric", OBJECT(s->fabric),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(bridge), errp)) {
        return false;
    }
    pci_bus_set_slot_reserved_mask(
        PCI_HOST_BRIDGE(bridge)->bus,
        ~(uint32_t)MAKE_64BIT_MASK(0, GIGACHANNEL_SLOTS));

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);
    return true;
}

static const Property gigachannel_properties[] = {
    DEFINE_PROP_LINK("fabric", SGIGigachannelState, fabric,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT64("onewire-serial", SGIGigachannelState, onewire_serial,
                       GIGACHANNEL_SERIAL_PLACEHOLDER),
    DEFINE_PROP_STRING("pci-bus-name", SGIGigachannelState, pci_bus_name),
    DEFINE_PROP_STRING("flash-drive", SGIGigachannelState, flash_drive_id),
    DEFINE_PROP_UINT16("system-controller-status", SGIGigachannelState,
                       status, 0),
};

/*
 * No state of its own: the status word is a construction-time property,
 * and the internal Bridge and the flash part carry their own.
 */
static const VMStateDescription vmstate_gigachannel = {
    .name = TYPE_SGI_GIGACHANNEL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void gigachannel_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXioBoardClass *bc = SGI_XIO_BOARD_CLASS(oc);

    dc->desc = "Origin 200 GIGAchannel expansion cabinet I/O";
    dc->vmsd = &vmstate_gigachannel;
    /* The cabinet is topology, placed by the machine, not a card. */
    dc->user_creatable = false;
    device_class_set_props(dc, gigachannel_properties);
    bc->build_widget = gigachannel_build_widget;
}

static const TypeInfo gigachannel_type = {
    .name = TYPE_SGI_GIGACHANNEL,
    .parent = TYPE_SGI_XIO_BOARD,
    .instance_size = sizeof(SGIGigachannelState),
    .class_init = gigachannel_class_init,
};

static void gigachannel_register_types(void)
{
    type_register_static(&gigachannel_type);
}

type_init(gigachannel_register_types)
