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
 * them the same way it would on a bare Bridge (see
 * hw/mips/sgi/sn0-model-consumer.c, sn0_model_create_bridge()). "fabric" is
 * required: Bridge refuses to realize without it.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/xtalk-board.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/scsi/qlogic-isp.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

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

    SGIBridgeState *bridge;
};

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
