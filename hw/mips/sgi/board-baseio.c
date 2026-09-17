/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-baseio: the Origin 2000 and Onyx2 standard XIO I/O board (BaseIO), as
 * a crosstalk board on the sgi-xtalk-board base class.
 *
 * A Bridge connects the board to its module's Crossbow; behind Bridge's
 * 32-bit parent PCI bus sit one IOC3 and two SCSI controllers
 * (docs/datasheets/baseio.md, "System position and board composition").
 * Facts and sources:
 *
 *   - Bridge revision 4 and board part "030-1124-002", and each PCI
 *     function's device number and revision: the reviewed origin2000
 *     catalogue entries (docs/machines/layers/modules/ip27-origin2000.yaml)
 *     carried into the emulator's existing generated model
 *     (hw/mips/sn0-origin2000-model.c). BaseIO's own public diagrams do not
 *     establish a PCI device number for either SCSI controller
 *     (docs/datasheets/baseio.md, "Facts this reference leaves open"); the
 *     catalogue's choice of devices 0 and 1 is a modelling decision cleared
 *     in docs/reviews/origin2000-scsi-component-selection-review-20260905.md,
 *     not an independently observed placement, so this class keeps it as
 *     an inference rather than a hardware citation.
 *   - IOC3 at device 2: the public board diagram does not establish a
 *     universal PCI device number either, but device 2 is confirmed
 *     independently by IRIX's own device path convention
 *     (docs/datasheets/baseio.md, "Interrupt and real-time paths":
 *     `/hw/module/<module>/slot/io1/baseio/pci/2/ei`).
 *   - Ethernet, both serial ports, and the one-wire identity part, with an
 *     M48T35-compatible Timekeeper on byte-bus select 0: the same
 *     catalogue entry and generated model. No keyboard/mouse or parallel
 *     function, unlike the Origin 200's onboard IOC3 -- BaseIO carries
 *     neither connector.
 *   - IOC3's own interrupt output reaches Bridge pin 4 directly rather than
 *     the pin matching its device number, the same direct-wiring rule
 *     docs/datasheets/baseio.md states for the Origin 200's onboard IOC3
 *     ("IOC3's INT_OUT reaches Bridge directly"), at the pin number the
 *     existing generated model uses. Both SCSI controllers use Bridge's
 *     ordinary rule (a slot's PCI INTA is wired to the pin of the same
 *     number), which Bridge applies on its own.
 *   - IOC3's PCI subsystem device id: not established by any reviewed
 *     source (docs/datasheets/baseio.md's open list includes "PCI device
 *     and function numbers for either SCSI controller" and leaves the
 *     subsystem field unmentioned entirely). Rather than inherit the
 *     IOC3 class's own default -- the Origin 200 System Board's observed
 *     0xc303 -- or repeat the existing generated model's unexplained zero,
 *     this class exposes the explicit "not observed" placeholder 0xffff,
 *     the same compatibility policy origin300-base.yaml's IO-8 review
 *     adopted for the same open question on that board.
 *
 * "fabric", "fabric-base", "fabric-node-shift" and "fabric-widget-shift"
 * pass straight through to the internal Bridge; see sgi-ip29-io's header
 * for why this board does not fix them itself. "pci-bus-name" also
 * passes through, unset by default; see sgi-ip29-io's header for why a
 * machine building more than one instance of a board class must give
 * each its own. "ioc3-onewire-serial" is the onboard IOC3's own
 * one-wire identity part, distinct from this board's own "onewire-
 * serial" (Bridge's NIC ROM identity); see sgi-ip29-io's header for
 * both. "nvram-drive" names the onboard IOC3's own Timekeeper storage
 * by block id, and "chardev-a", "chardev-b", "record-a" and "record-b"
 * name its two serial functions by chardev id, all unset by default
 * and all held only as an id rather than a claimed backend, for the
 * claim-timing reason sgi-ip29-io's header gives; BaseIO has no
 * parallel function, so this class exposes no "parallel-chardev".
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/xtalk-board.h"
#include "chardev/char.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/scsi/qlogic-isp.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/block-backend.h"

#define TYPE_SGI_BASEIO "sgi-baseio"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBaseioState, SGI_BASEIO)

/* BaseIO is fixed at IO1 (ip27-origin2000.yaml, baseio-bridge widget 8). */
#define BASEIO_WIDGET 8

#define BASEIO_BRIDGE_REVISION 4
#define BASEIO_BRIDGE_BOARD_PART "030-1124-002"

#define BASEIO_SCSI0_DEVICE 0
#define BASEIO_SCSI1_DEVICE 1
#define BASEIO_SCSI_REVISION 5

#define BASEIO_IOC3_DEVICE   2
#define BASEIO_IOC3_REVISION 1
#define BASEIO_IOC3_IRQ_PIN  4
#define BASEIO_IOC3_SUBSYSTEM_ID_NOT_OBSERVED 0xffff
#define BASEIO_IOC3_POPULATION (SGI_IOC3_POPULATION_ETHERNET | \
                                SGI_IOC3_POPULATION_SERIAL_A | \
                                SGI_IOC3_POPULATION_SERIAL_B | \
                                SGI_IOC3_POPULATION_ONEWIRE_IDENTITY)

/* Placeholder; see sgi-ip29-io's own identical constant. */
#define BASEIO_SERIAL_PLACEHOLDER 0x060504030201ULL

struct SGIBaseioState {
    SGIXtalkBoardState parent_obj;

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
};

/*
 * Resolve a chardev id to its backend and set it on dev's own
 * same-named property, before dev realizes; see the header comment on
 * "chardev-a" and its siblings above for why this board never holds a
 * "chardev" property of its own. An id that names no real backend is
 * refused here; see sgi-ip29-io's identical helper for why.
 */
static bool baseio_pass_chardev(DeviceState *dev, const char *prop,
                                const char *id, Error **errp)
{
    Chardev *chr;

    if (!id) {
        return true;
    }
    chr = qemu_chr_find(id);
    if (!chr) {
        error_setg(errp, "sgi-baseio: no chardev backend named '%s' for "
                  "%s", id, prop);
        return false;
    }
    qdev_prop_set_chr(dev, prop, chr);
    return true;
}

/*
 * Resolve a block backend id to its backend and set it on dev's own
 * "nvram-drive" property, before dev realizes; see baseio_pass_chardev
 * and sgi-ip29-io's header for why this board holds only an id.
 */
static bool baseio_pass_drive(DeviceState *dev, const char *id,
                              Error **errp)
{
    BlockBackend *blk;

    if (!id) {
        return true;
    }
    blk = blk_by_name(id);
    if (!blk) {
        error_setg(errp, "sgi-baseio: no drive backend named '%s' for "
                  "nvram-drive", id);
        return false;
    }
    qdev_prop_set_drive(dev, "nvram-drive", blk);
    return true;
}

static bool baseio_build_widget(SGIXtalkBoardState *board, Error **errp)
{
    SGIBaseioState *s = SGI_BASEIO(board);
    DeviceState *bridge;
    PCIBus *bus;
    PCIDevice *scsi0, *scsi1, *ioc3;

    if (!s->fabric) {
        error_setg(errp, "sgi-baseio requires the fabric its interrupt "
                  "packets travel");
        return false;
    }
    if (board->slot != BASEIO_WIDGET) {
        error_setg(errp, "sgi-baseio is fixed at crosstalk widget %u",
                  BASEIO_WIDGET);
        return false;
    }

    bridge = qdev_new(TYPE_SGI_BRIDGE);
    object_property_add_child(OBJECT(board), "bridge", OBJECT(bridge));
    s->bridge = SGI_BRIDGE(bridge);
    qdev_prop_set_uint32(bridge, "revision", BASEIO_BRIDGE_REVISION);
    qdev_prop_set_string(bridge, "board-part", BASEIO_BRIDGE_BOARD_PART);
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

    scsi0 = pci_new(PCI_DEVFN(BASEIO_SCSI0_DEVICE, 0), "qlogic-isp1040");
    object_property_add_child(OBJECT(board), "scsi0", OBJECT(scsi0));
    pci_realize_and_unref(scsi0, bus, &error_abort);
    pci_set_byte(scsi0->config + PCI_REVISION_ID, BASEIO_SCSI_REVISION);
    qlogic_isp_handle_legacy_cmdline(DEVICE(scsi0));

    scsi1 = pci_new(PCI_DEVFN(BASEIO_SCSI1_DEVICE, 0), "qlogic-isp1040");
    object_property_add_child(OBJECT(board), "scsi1", OBJECT(scsi1));
    pci_realize_and_unref(scsi1, bus, &error_abort);
    pci_set_byte(scsi1->config + PCI_REVISION_ID, BASEIO_SCSI_REVISION);
    qlogic_isp_handle_legacy_cmdline(DEVICE(scsi1));

    ioc3 = pci_new(PCI_DEVFN(BASEIO_IOC3_DEVICE, 0), TYPE_SGI_IOC3);
    object_property_add_child(OBJECT(board), "ioc3", OBJECT(ioc3));
    qdev_prop_set_uint32(DEVICE(ioc3), "population", BASEIO_IOC3_POPULATION);
    qdev_prop_set_uint16(DEVICE(ioc3), "subsystem-id",
                         BASEIO_IOC3_SUBSYSTEM_ID_NOT_OBSERVED);
    qdev_prop_set_uint8(DEVICE(ioc3), "timekeeper-kind",
                        SGI_IOC3_TIMEKEEPER_M48T35);
    qdev_prop_set_uint64(DEVICE(ioc3), "onewire-serial",
                         s->ioc3_onewire_serial);
    if (!baseio_pass_drive(DEVICE(ioc3), s->nvram_drive_id, errp) ||
        !baseio_pass_chardev(DEVICE(ioc3), "chardev-a", s->chardev_a_id,
                             errp) ||
        !baseio_pass_chardev(DEVICE(ioc3), "chardev-b", s->chardev_b_id,
                             errp) ||
        !baseio_pass_chardev(DEVICE(ioc3), "record-a", s->record_a_id,
                             errp) ||
        !baseio_pass_chardev(DEVICE(ioc3), "record-b", s->record_b_id,
                             errp)) {
        return false;
    }
    pci_realize_and_unref(ioc3, bus, &error_abort);
    pci_set_byte(ioc3->config + PCI_REVISION_ID, BASEIO_IOC3_REVISION);
    /* Direct board wiring, not the bus's own per-slot PCI INTx rule. */
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in(bridge, BASEIO_IOC3_IRQ_PIN));

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);
    return true;
}

static const Property baseio_properties[] = {
    DEFINE_PROP_LINK("fabric", SGIBaseioState, fabric, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT64("fabric-base", SGIBaseioState, fabric_base, 0),
    DEFINE_PROP_UINT32("fabric-node-shift", SGIBaseioState,
                       fabric_node_shift, 0),
    DEFINE_PROP_UINT32("fabric-widget-shift", SGIBaseioState,
                       fabric_widget_shift, 0),
    DEFINE_PROP_UINT64("onewire-serial", SGIBaseioState, onewire_serial,
                       BASEIO_SERIAL_PLACEHOLDER),
    DEFINE_PROP_UINT64("ioc3-onewire-serial", SGIBaseioState,
                       ioc3_onewire_serial, SGI_IOC3_ONEWIRE_SERIAL),
    DEFINE_PROP_STRING("pci-bus-name", SGIBaseioState, pci_bus_name),
    DEFINE_PROP_STRING("nvram-drive", SGIBaseioState, nvram_drive_id),
    DEFINE_PROP_STRING("chardev-a", SGIBaseioState, chardev_a_id),
    DEFINE_PROP_STRING("chardev-b", SGIBaseioState, chardev_b_id),
    DEFINE_PROP_STRING("record-a", SGIBaseioState, record_a_id),
    DEFINE_PROP_STRING("record-b", SGIBaseioState, record_b_id),
};

/*
 * No state of its own outside the children Bridge, the SCSI HBAs and IOC3
 * own.
 */
static const VMStateDescription vmstate_baseio = {
    .name = TYPE_SGI_BASEIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void baseio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXtalkBoardClass *bc = SGI_XTALK_BOARD_CLASS(oc);

    dc->desc = "Origin 2000 / Onyx2 BaseIO board";
    dc->vmsd = &vmstate_baseio;
    /* Fitted per module by the machine, at most one per deskside enclosure. */
    dc->user_creatable = false;
    device_class_set_props(dc, baseio_properties);
    bc->build_widget = baseio_build_widget;
}

static const TypeInfo baseio_type = {
    .name = TYPE_SGI_BASEIO,
    .parent = TYPE_SGI_XTALK_BOARD,
    .instance_size = sizeof(SGIBaseioState),
    .class_init = baseio_class_init,
};

static void baseio_register_types(void)
{
    type_register_static(&baseio_type);
}

type_init(baseio_register_types)
