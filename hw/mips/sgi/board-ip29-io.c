/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-ip29-io: the Origin 200 System Board's onboard I/O, as a crosstalk
 * board on the sgi-xtalk-board base class.
 *
 * A Bridge sits at crosstalk widget 8 (both the machine's discovery widget
 * and its enumerated widget on a one-module Origin 200: the direct-link
 * Bridge carries no separate Crossbow, and the board's own printed slot is
 * this same widget 8). Behind Bridge's 32-bit parent PCI bus: two ISP1040
 * SCSI controllers at devices 0 and 1, and one IOC3 at device 2. Facts and
 * sources:
 *
 *   - Bridge revision 4 and board part "030-1244-001", and each PCI
 *     function's device number and revision: observed on real Origin 200
 *     hardware, carried by the reviewed origin200 catalogue entries
 *     (docs/machines/layers/modules/ip29.yaml) into the emulator's existing
 *     generated model (hw/mips/sn0-origin200-model.c, base_io and pci[]).
 *     docs/datasheets/bridge.md and docs/datasheets/isp1040.md own the
 *     chips' own register and DMA contracts.
 *   - IOC3 at device 2, PCI subsystem device id 0xc303 (the class's own
 *     "IP29 System Board" default -- docs/datasheets/ioc3.md's
 *     board-variant table and PCI subsystem-register contract): this board
 *     is the one the emulator's default identifies, so this class leaves
 *     the chip's own default rather than repeating it.
 *   - Full population -- Ethernet, both serial ports, keyboard/mouse,
 *     parallel, and the one-wire identity part -- and an M48T35-compatible
 *     Timekeeper on byte-bus select 0: the same origin200 catalogue entry
 *     and generated model, matching the Owner's Guide's onboard I/O list.
 *   - IOC3's own interrupt output reaches Bridge pin 3 directly rather than
 *     the pin matching its device number: observed on real hardware,
 *     carried the same way. Both ISP1040s use Bridge's ordinary rule (a
 *     slot's PCI INTA is wired to the pin of the same number), which
 *     Bridge applies on its own; this board wires nothing extra for them.
 *
 * "fabric", "fabric-base", "fabric-node-shift" and "fabric-widget-shift"
 * pass straight through to the internal Bridge: which Hub or fabric proxy
 * receives this board's interrupt packets is a fact about the machine this
 * board is built into, not about the board itself, so the machine sets
 * them the same way it would on a bare Bridge. "fabric" is required:
 * Bridge refuses to realize without it. "pci-bus-name" also passes
 * through, unset by default (Bridge's own default bus name): the machine
 * building more than one instance of this board -- the two-chassis
 * Origin 200's own case -- has to give each its own name, since two PCI
 * buses answering to the same name is a migration-identity collision on
 * every device behind them, not merely a cosmetic one. "onewire-serial"
 * is Bridge's own NIC ROM identity (widget discovery); "ioc3-onewire-
 * serial" is the onboard IOC3's separate one-wire identity part, a
 * distinct physical serial the topology's io_boards[] entry's own PCI
 * function record carries -- unset, it keeps the IOC3 class's own
 * default, matching a bare -device. "nvram-drive", "chardev-a",
 * "chardev-b", "record-a", "record-b" and "parallel-chardev" name the
 * onboard IOC3's own Timekeeper storage and serial/parallel backends by
 * block or chardev id, unset by default, rather than carrying either
 * kind of backend directly the way "fabric" does: a "drive" or
 * "chardev" property claims its backend the moment it is set, and the
 * IOC3 this board is about to build is the one device that may claim
 * each of these, once, before it realizes -- a link property here would
 * claim it first and leave the IOC3's own identically-named property
 * nothing to claim, and setting either one after this board has
 * realized is refused outright (qdev_prop_set_after_realize()). The
 * deleted inline builder resolved these same backends and the IOC3's
 * own onewire-serial from a `-drive if=mtd` or a bare `-drive if=none,
 * id=sgi-nvram<N>`, the topology's console field, the command line by
 * node index, and the PCI function record; that is now the machine's
 * own job, handed to this board only as an id or a value.
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

#define TYPE_SGI_IP29_IO "sgi-ip29-io"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIp29IoState, SGI_IP29_IO)

/* The board's one printed slot: also Bridge's discovery widget. */
#define IP29_IO_WIDGET 8

#define IP29_IO_BRIDGE_REVISION 4
#define IP29_IO_BRIDGE_BOARD_PART "030-1244-001"

#define IP29_IO_ISP1040_DEVICE_0 0
#define IP29_IO_ISP1040_DEVICE_1 1
#define IP29_IO_ISP1040_REVISION 5

#define IP29_IO_IOC3_DEVICE   2
#define IP29_IO_IOC3_REVISION 1
#define IP29_IO_IOC3_IRQ_PIN  3
#define IP29_IO_IOC3_POPULATION (SGI_IOC3_POPULATION_ETHERNET | \
                                 SGI_IOC3_POPULATION_SERIAL_A | \
                                 SGI_IOC3_POPULATION_SERIAL_B | \
                                 SGI_IOC3_POPULATION_KEYBOARD_MOUSE | \
                                 SGI_IOC3_POPULATION_PARALLEL | \
                                 SGI_IOC3_POPULATION_ONEWIRE_IDENTITY)

/*
 * A placeholder, not an observed part serial: no reviewed source assigns a
 * specific one-wire serial number to this class of board, only to a real
 * fitted unit (docs/machines/layers/modules/ip29.yaml's per-node instance
 * layer, which is instance data, not a board fact). Matches Bridge's own
 * unset default (hw/pci-host/sgi-bridge.c, NIC_ROM_SERIAL_DEFAULT).
 */
#define IP29_IO_SERIAL_PLACEHOLDER 0x060504030201ULL

struct SGIIp29IoState {
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
    char *parallel_chardev_id;

    SGIBridgeState *bridge;
};

/*
 * Resolve a chardev id to its backend and set it on dev's own
 * same-named property, before dev realizes: setting a "chardev"
 * property claims the backend, so this board never holds one of its
 * own (see the header comment on "chardev-a" and its siblings above) --
 * it only carries the id long enough to hand it to the one child that
 * will actually claim it.
 */
static void ip29_io_pass_chardev(DeviceState *dev, const char *prop,
                                 const char *id)
{
    Chardev *chr;

    if (!id) {
        return;
    }
    chr = qemu_chr_find(id);
    if (chr) {
        qdev_prop_set_chr(dev, prop, chr);
    }
}

/*
 * Resolve a block backend id to its backend and set it on dev's own
 * "nvram-drive" property, before dev realizes: the same claim-timing
 * reason ip29_io_pass_chardev exists for, and the same reason this
 * board holds an id rather than a BlockBackend of its own.
 */
static void ip29_io_pass_drive(DeviceState *dev, const char *id)
{
    BlockBackend *blk;

    if (!id) {
        return;
    }
    blk = blk_by_name(id);
    if (blk) {
        qdev_prop_set_drive(dev, "nvram-drive", blk);
    }
}

static bool ip29_io_build_widget(SGIXtalkBoardState *board, Error **errp)
{
    SGIIp29IoState *s = SGI_IP29_IO(board);
    DeviceState *bridge;
    PCIBus *bus;
    PCIDevice *isp0, *isp1, *ioc3;

    if (!s->fabric) {
        error_setg(errp, "sgi-ip29-io requires the fabric its interrupt "
                  "packets travel");
        return false;
    }
    if (board->slot != IP29_IO_WIDGET) {
        error_setg(errp, "sgi-ip29-io is fixed at crosstalk widget %u",
                  IP29_IO_WIDGET);
        return false;
    }

    bridge = qdev_new(TYPE_SGI_BRIDGE);
    object_property_add_child(OBJECT(board), "bridge", OBJECT(bridge));
    s->bridge = SGI_BRIDGE(bridge);
    qdev_prop_set_uint32(bridge, "revision", IP29_IO_BRIDGE_REVISION);
    qdev_prop_set_string(bridge, "board-part", IP29_IO_BRIDGE_BOARD_PART);
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

    isp0 = pci_new(PCI_DEVFN(IP29_IO_ISP1040_DEVICE_0, 0), "qlogic-isp1040");
    object_property_add_child(OBJECT(board), "isp0", OBJECT(isp0));
    pci_realize_and_unref(isp0, bus, &error_abort);
    pci_set_byte(isp0->config + PCI_REVISION_ID, IP29_IO_ISP1040_REVISION);
    qlogic_isp_handle_legacy_cmdline(DEVICE(isp0));

    isp1 = pci_new(PCI_DEVFN(IP29_IO_ISP1040_DEVICE_1, 0), "qlogic-isp1040");
    object_property_add_child(OBJECT(board), "isp1", OBJECT(isp1));
    pci_realize_and_unref(isp1, bus, &error_abort);
    pci_set_byte(isp1->config + PCI_REVISION_ID, IP29_IO_ISP1040_REVISION);
    qlogic_isp_handle_legacy_cmdline(DEVICE(isp1));

    ioc3 = pci_new(PCI_DEVFN(IP29_IO_IOC3_DEVICE, 0), TYPE_SGI_IOC3);
    object_property_add_child(OBJECT(board), "ioc3", OBJECT(ioc3));
    qdev_prop_set_uint32(DEVICE(ioc3), "population", IP29_IO_IOC3_POPULATION);
    qdev_prop_set_uint8(DEVICE(ioc3), "timekeeper-kind",
                        SGI_IOC3_TIMEKEEPER_M48T35);
    qdev_prop_set_uint64(DEVICE(ioc3), "onewire-serial",
                         s->ioc3_onewire_serial);
    ip29_io_pass_drive(DEVICE(ioc3), s->nvram_drive_id);
    ip29_io_pass_chardev(DEVICE(ioc3), "chardev-a", s->chardev_a_id);
    ip29_io_pass_chardev(DEVICE(ioc3), "chardev-b", s->chardev_b_id);
    ip29_io_pass_chardev(DEVICE(ioc3), "record-a", s->record_a_id);
    ip29_io_pass_chardev(DEVICE(ioc3), "record-b", s->record_b_id);
    ip29_io_pass_chardev(DEVICE(ioc3), "parallel-chardev",
                         s->parallel_chardev_id);
    pci_realize_and_unref(ioc3, bus, &error_abort);
    pci_set_byte(ioc3->config + PCI_REVISION_ID, IP29_IO_IOC3_REVISION);
    /* Direct board wiring, not the bus's own per-slot PCI INTx rule. */
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in(bridge, IP29_IO_IOC3_IRQ_PIN));

    board->cpu_face = sysbus_mmio_get_region(SYS_BUS_DEVICE(bridge), 0);
    board->packet_face = sgi_bridge_packet_face(s->bridge);
    return true;
}

static const Property ip29_io_properties[] = {
    DEFINE_PROP_LINK("fabric", SGIIp29IoState, fabric, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT64("fabric-base", SGIIp29IoState, fabric_base, 0),
    DEFINE_PROP_UINT32("fabric-node-shift", SGIIp29IoState,
                       fabric_node_shift, 0),
    DEFINE_PROP_UINT32("fabric-widget-shift", SGIIp29IoState,
                       fabric_widget_shift, 0),
    DEFINE_PROP_UINT64("onewire-serial", SGIIp29IoState, onewire_serial,
                       IP29_IO_SERIAL_PLACEHOLDER),
    DEFINE_PROP_UINT64("ioc3-onewire-serial", SGIIp29IoState,
                       ioc3_onewire_serial, SGI_IOC3_ONEWIRE_SERIAL),
    DEFINE_PROP_STRING("pci-bus-name", SGIIp29IoState, pci_bus_name),
    DEFINE_PROP_STRING("nvram-drive", SGIIp29IoState, nvram_drive_id),
    DEFINE_PROP_STRING("chardev-a", SGIIp29IoState, chardev_a_id),
    DEFINE_PROP_STRING("chardev-b", SGIIp29IoState, chardev_b_id),
    DEFINE_PROP_STRING("record-a", SGIIp29IoState, record_a_id),
    DEFINE_PROP_STRING("record-b", SGIIp29IoState, record_b_id),
    DEFINE_PROP_STRING("parallel-chardev", SGIIp29IoState,
                       parallel_chardev_id),
};

/* No state of its own outside the children Bridge, the ISPs and IOC3 own. */
static const VMStateDescription vmstate_ip29_io = {
    .name = TYPE_SGI_IP29_IO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void ip29_io_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXtalkBoardClass *bc = SGI_XTALK_BOARD_CLASS(oc);

    dc->desc = "Origin 200 System Board onboard I/O";
    dc->vmsd = &vmstate_ip29_io;
    /* Every Origin 200 has exactly one; nothing else builds this class. */
    dc->user_creatable = false;
    device_class_set_props(dc, ip29_io_properties);
    bc->build_widget = ip29_io_build_widget;
}

static const TypeInfo ip29_io_type = {
    .name = TYPE_SGI_IP29_IO,
    .parent = TYPE_SGI_XTALK_BOARD,
    .instance_size = sizeof(SGIIp29IoState),
    .class_init = ip29_io_class_init,
};

static void ip29_io_register_types(void)
{
    type_register_static(&ip29_io_type);
}

type_init(ip29_io_register_types)
