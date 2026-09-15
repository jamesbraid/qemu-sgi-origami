/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-io8: the Origin 300 IO-8 assembly, as a crosstalk board on the
 * sgi-xtalk-board base class.
 *
 * The IO-8 assembly is the fused XBridge chip itself: a crossbar section
 * plus two independent PCI bridge heads, ports 0xe and 0xf
 * (docs/datasheets/xbridge.md; docs/machines/layers/modules/
 * origin300-base.yaml, the "xbridge" component). Head F (port 0xf, 33 MHz)
 * carries the IO-8's fixed devices; head E (port 0xe, 66 MHz) carries the
 * board's two general-purpose option slots, both left empty here since no
 * reviewed source fixes what -- if anything -- ships in them, so this class
 * does not populate head E at all.
 *
 * Head F's population, device numbers, interrupt pins and identity are the
 * origin300-base.yaml catalogue entries (ioc3, scsi, usb) already carried
 * into the emulator's existing generated model
 * (hw/mips/sn1-origin300-model.c, sn1_origin300_pci_devices[]):
 *
 *   - QLogic ISP12160 at device 1, PCI revision 6, physical interrupt pin
 *     1; subsystem vendor and device unobserved (0xffff:0xffff).
 *   - IOC3 at device 4, PCI revision 1, physical interrupt pin 4;
 *     subsystem vendor 0x10a9 (the class's own public SGI vendor) with an
 *     explicit unobserved device placeholder (0xffff), rather than
 *     inheriting the Origin 200 System Board's 0xc303 default -- IO-8 is a
 *     different board. Population 0x17 (Ethernet, both serial ports and
 *     the parallel function; no keyboard/mouse, and no one-wire identity --
 *     the board's Ethernet address comes from the brick EEPROM through the
 *     L1, not from IOC3's own part), with an M48T35-compatible Timekeeper.
 *     A second, internal interrupt source (the chip's own SuperIO real-time
 *     event, not a PCI pin) reaches physical pin 0.
 *   - The AT&T/Lucent OHCI USB controller at device 5, PCI revision 0x10,
 *     physical interrupt pin 5; subsystem vendor and device unobserved.
 *   - The IOC3's transceiver answers at MII address 1 as a DP83843, not the
 *     Origin 200 BaseIO's address 31 DP83840A (docs/datasheets/dp83840.md).
 *
 * docs/datasheets/xbridge.md and docs/datasheets/ioc3.md own the chips' own
 * register and DMA contracts; docs/datasheets/isp12160.md and
 * docs/datasheets/lucent-ohci.md own the other two functions'.
 *
 * Head F's own MicroLAN board-identity part ("board-part-f" on the chip)
 * has no IP45 IO-8 identity capture (docs/datasheets/xbridge.md's own
 * placeholder note), so this class exposes the same explicit "not
 * observed" pair the chip itself already defines for exactly this case
 * (SGI_XBRIDGE_NIC_PLACEHOLDER_PART/SERIAL) as overridable properties,
 * rather than baking them in, since a real configuration-space capture is
 * expected to replace them.
 *
 * Widget identity: XBridge has no separate outer Crossbow chip on the
 * Origin 300 -- the whole chip sits directly on the Bedrock's own
 * crosstalk port (host-port 0xa; docs/machines/layers/modules/
 * origin300-base.yaml, xbridge_host_port). This board's own `slot=`
 * property (the base class's module-Crossbow widget number) has no
 * matching Origin 300 hardware fact, since there is no module Crossbow
 * for it to name a position on; it exists only to satisfy the base
 * class's contract. The eventual Origin 300 machine wiring is expected to
 * attach this board's fabric link directly, the way hw/mips/sgi/sn1.c
 * does today, rather than through sgi_xtalk_board_attach_pending().
 * "fabric" passes straight through to the internal chip and is required:
 * XBridge refuses to realize without it.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/xtalk-board.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/pci-host/sgi-xbridge.h"
#include "hw/pci/pci.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_SGI_IO8 "sgi-io8"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIo8State, SGI_IO8)

/* The Bedrock's own crosstalk port the whole chip sits on (fixed wiring). */
#define IO8_HOST_PORT 0xa

/* Head F: the IO-8's fixed devices; head E's two option slots stay empty. */
#define IO8_HEAD_F 0xf

#define IO8_SCSI_DEVICE       1
#define IO8_SCSI_REVISION     6
#define IO8_SCSI_INTERRUPT_PIN 1
#define IO8_SCSI_SUBSYSTEM_VENDOR 0xffff
#define IO8_SCSI_SUBSYSTEM_ID     0xffff

#define IO8_IOC3_DEVICE       4
#define IO8_IOC3_REVISION     1
#define IO8_IOC3_INTERRUPT_PIN 4
#define IO8_IOC3_SUPERIO_PIN   0
#define IO8_IOC3_SUBSYSTEM_VENDOR 0x10a9
#define IO8_IOC3_SUBSYSTEM_ID_NOT_OBSERVED 0xffff
#define IO8_IOC3_POPULATION (SGI_IOC3_POPULATION_ETHERNET | \
                            SGI_IOC3_POPULATION_SERIAL_A | \
                            SGI_IOC3_POPULATION_SERIAL_B | \
                            SGI_IOC3_POPULATION_PARALLEL)
/* The IO-8's own transceiver: DP83843 at MII address 1 (dp83840.md). */
#define IO8_IOC3_PHY_ADDR 1

#define IO8_USB_DEVICE       5
#define IO8_USB_REVISION     0x10
#define IO8_USB_INTERRUPT_PIN 5
#define IO8_USB_SUBSYSTEM_VENDOR 0xffff
#define IO8_USB_SUBSYSTEM_ID     0xffff

struct SGIIo8State {
    SGIXtalkBoardState parent_obj;

    MemoryRegion *fabric;
    char *board_part;
    uint64_t onewire_serial;

    SGIXBridgeState *chip;
};

/* Wires one head F function's PCI INTx into the chip's own pin group. */
static void io8_wire_intx(DeviceState *chip, unsigned int device,
                          unsigned int pin)
{
    g_autofree char *intx = g_strdup_printf("head-%x-pci-intx", IO8_HEAD_F);
    g_autofree char *pin_name = g_strdup_printf("head-%x-pin", IO8_HEAD_F);

    qdev_connect_gpio_out_named(chip, intx, device * PCI_NUM_PINS,
                                qdev_get_gpio_in_named(chip, pin_name, pin));
}

static bool io8_build_widget(SGIXtalkBoardState *board, Error **errp)
{
    SGIIo8State *s = SGI_IO8(board);
    DeviceState *chip;
    PCIBus *bus;
    PCIDevice *scsi, *ioc3, *usb;
    g_autofree char *pin_name = g_strdup_printf("head-%x-pin", IO8_HEAD_F);

    if (!s->fabric) {
        error_setg(errp, "sgi-io8 requires the fabric its interrupt "
                  "packets travel");
        return false;
    }

    chip = qdev_new(TYPE_SGI_XBRIDGE);
    object_property_add_child(OBJECT(board), "chip", OBJECT(chip));
    s->chip = SGI_XBRIDGE(chip);
    qdev_prop_set_uint8(chip, "host-port", IO8_HOST_PORT);
    qdev_prop_set_string(chip, "board-part-f",
                         s->board_part ?: SGI_XBRIDGE_NIC_PLACEHOLDER_PART);
    qdev_prop_set_uint64(chip, "onewire-serial-f", s->onewire_serial);
    object_property_set_link(OBJECT(chip), "fabric", OBJECT(s->fabric),
                             &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(chip), errp)) {
        return false;
    }
    bus = sgi_xbridge_pci_bus(s->chip, IO8_HEAD_F);

    scsi = pci_new(PCI_DEVFN(IO8_SCSI_DEVICE, 0), "qlogic-isp12160");
    object_property_add_child(OBJECT(board), "scsi", OBJECT(scsi));
    pci_realize_and_unref(scsi, bus, &error_abort);
    pci_set_byte(scsi->config + PCI_REVISION_ID, IO8_SCSI_REVISION);
    pci_set_word(scsi->config + PCI_SUBSYSTEM_VENDOR_ID,
                IO8_SCSI_SUBSYSTEM_VENDOR);
    pci_set_word(scsi->config + PCI_SUBSYSTEM_ID, IO8_SCSI_SUBSYSTEM_ID);
    io8_wire_intx(chip, IO8_SCSI_DEVICE, IO8_SCSI_INTERRUPT_PIN);

    ioc3 = pci_new(PCI_DEVFN(IO8_IOC3_DEVICE, 0), TYPE_SGI_IOC3);
    object_property_add_child(OBJECT(board), "ioc3", OBJECT(ioc3));
    qdev_prop_set_uint32(DEVICE(ioc3), "population", IO8_IOC3_POPULATION);
    qdev_prop_set_uint8(DEVICE(ioc3), "timekeeper-kind",
                        SGI_IOC3_TIMEKEEPER_M48T35);
    qdev_prop_set_uint8(DEVICE(ioc3), "phy-addr", IO8_IOC3_PHY_ADDR);
    object_property_set_bool(OBJECT(ioc3), "phy-dp83843", true, &error_abort);
    pci_realize_and_unref(ioc3, bus, &error_abort);
    pci_set_byte(ioc3->config + PCI_REVISION_ID, IO8_IOC3_REVISION);
    pci_set_word(ioc3->config + PCI_SUBSYSTEM_VENDOR_ID,
                IO8_IOC3_SUBSYSTEM_VENDOR);
    pci_set_word(ioc3->config + PCI_SUBSYSTEM_ID,
                IO8_IOC3_SUBSYSTEM_ID_NOT_OBSERVED);
    io8_wire_intx(chip, IO8_IOC3_DEVICE, IO8_IOC3_INTERRUPT_PIN);
    /* The chip's own internal SuperIO real-time event, not a PCI pin. */
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in_named(chip, pin_name,
                                                 IO8_IOC3_SUPERIO_PIN));

    usb = pci_new(PCI_DEVFN(IO8_USB_DEVICE, 0), "lucent-ohci");
    object_property_add_child(OBJECT(board), "usb", OBJECT(usb));
    pci_realize_and_unref(usb, bus, &error_abort);
    pci_set_byte(usb->config + PCI_REVISION_ID, IO8_USB_REVISION);
    pci_set_word(usb->config + PCI_SUBSYSTEM_VENDOR_ID,
                IO8_USB_SUBSYSTEM_VENDOR);
    pci_set_word(usb->config + PCI_SUBSYSTEM_ID, IO8_USB_SUBSYSTEM_ID);
    io8_wire_intx(chip, IO8_USB_DEVICE, IO8_USB_INTERRUPT_PIN);

    /* The whole chip answers as one widget: the crossbar's own view. */
    board->cpu_face = sgi_xbridge_widget_view(s->chip, 0);
    board->packet_face = board->cpu_face;
    return true;
}

static const Property io8_properties[] = {
    DEFINE_PROP_LINK("fabric", SGIIo8State, fabric, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_STRING("board-part", SGIIo8State, board_part),
    DEFINE_PROP_UINT64("onewire-serial", SGIIo8State, onewire_serial,
                       SGI_XBRIDGE_NIC_PLACEHOLDER_SERIAL),
};

/* No state of its own outside the internal chip, which carries its own. */
static const VMStateDescription vmstate_io8 = {
    .name = TYPE_SGI_IO8,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void io8_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    SGIXtalkBoardClass *bc = SGI_XTALK_BOARD_CLASS(oc);

    dc->desc = "Origin 300 IO-8 assembly";
    dc->vmsd = &vmstate_io8;
    /* Every Origin 300 has exactly one; nothing else builds this class. */
    dc->user_creatable = false;
    device_class_set_props(dc, io8_properties);
    bc->build_widget = io8_build_widget;
}

static const TypeInfo io8_type = {
    .name = TYPE_SGI_IO8,
    .parent = TYPE_SGI_XTALK_BOARD,
    .instance_size = sizeof(SGIIo8State),
    .class_init = io8_class_init,
};

static void io8_register_types(void)
{
    type_register_static(&io8_type);
}

type_init(io8_register_types)
