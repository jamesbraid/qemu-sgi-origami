/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sgi-baseio: see the header for what this device owns and what its
 * carrier owns instead.
 *
 * Facts and sources, consolidated from the three classes this device
 * replaces (sgi-ip29-io, sgi-baseio's own predecessor board-baseio.c,
 * and sgi-io8):
 *
 *   - ip29: Bridge revision 4, board part "030-1244-001". ISP1040 at
 *     devices 0 and 1, revision 5. IOC3 at device 2, revision 1, PCI
 *     subsystem device id left at the chip's own "IP29 System Board"
 *     default (docs/datasheets/ioc3.md's board-variant table). Full
 *     population -- Ethernet, both serial ports, keyboard/mouse,
 *     parallel, the one-wire identity part -- and an M48T35-compatible
 *     Timekeeper. IOC3's own interrupt output reaches the chip's pin 3
 *     directly rather than the pin matching its device number; both
 *     ISP1040s use the chip's own ordinary per-slot INTA rule.
 *     (docs/machines/layers/modules/ip29.yaml; hw/mips/sn0-origin200-model.c
 *     base_io and pci[], carried into the deleted board-ip29-io.c.)
 *   - io6: Bridge revision 4, board part "030-1124-002". ISP1040 at
 *     devices 0 and 1, revision 5. IOC3 at device 2, revision 1,
 *     population Ethernet, both serial ports and the one-wire identity
 *     part (no keyboard/mouse or parallel), an M48T35-compatible
 *     Timekeeper, and the explicit "not observed" subsystem id 0xffff
 *     (docs/datasheets/baseio.md, "Facts this reference leaves open").
 *     IOC3's own interrupt output reaches pin 4 directly; both ISP1040s
 *     use the ordinary per-slot rule.
 *     (docs/machines/layers/modules/ip27-origin2000.yaml;
 *     hw/mips/sn0-origin2000-model.c, carried into the deleted
 *     board-baseio.c.)
 *   - io8: XBridge head F, board part "030-1673-003" (this device's own
 *     carrier default; the head's fitted onewire-serial is a per-unit
 *     fact the carrier passes down separately). QLogic ISP12160 at
 *     device 1, revision 6, subsystem vendor and device unobserved
 *     (0xffff:0xffff). IOC3 at device 4, revision 1, subsystem vendor
 *     0x10a9 with an explicit unobserved device placeholder (0xffff),
 *     population Ethernet, both serial ports and parallel (no
 *     keyboard/mouse, no one-wire identity -- the board's Ethernet
 *     address comes from the brick EEPROM through the L1, not from
 *     IOC3's own part), an M48T35-compatible Timekeeper, and the DP83843
 *     transceiver at MII address 1 (docs/datasheets/dp83840.md), unlike
 *     the Origin 200/2000 boards' DP83840A at address 31. The AT&T/Lucent
 *     OHCI USB controller at device 5, revision 0x10, subsystem vendor
 *     and device unobserved. A head has no automatic per-slot INTx
 *     routing of its own, so every function's interrupt is wired
 *     explicitly, and IOC3's own internal SuperIO real-time event (not a
 *     PCI pin) reaches the head's own pin 0.
 *     (docs/datasheets/xbridge.md; docs/machines/layers/modules/
 *     origin300-base.yaml; hw/mips/sn1-origin300-model.c
 *     sn1_origin300_pci_devices[], carried into the deleted
 *     board-io8.c.)
 *
 * "onewire-serial" is the onboard IOC3's own one-wire identity part, a
 * fact about this specific fitted unit distinct from the carrier's own
 * chip identity (which the carrier sets directly on the Bridge or
 * XBridge it owns, before this device exists to hand a bus from). It
 * forwards straight to the IOC3 child's own same-named property.
 * "nvram-drive", "chardev-a", "chardev-b", "record-a", "record-b" and
 * "parallel-chardev" name the onboard IOC3's own Timekeeper storage and
 * serial/parallel backends by block or chardev id, unset by default,
 * held only as an id rather than a claimed backend: a "drive" or
 * "chardev" property claims its backend the moment it is set, and the
 * IOC3 this device is about to build is the one device that may claim
 * each of these, once, before it realizes -- a link property here would
 * claim it first and leave the IOC3's own identically-named property
 * nothing to claim. "mio-chardev-a" and "mio-chardev-b" name the same
 * kind of backend for the io6g preset's second IOC3 (the MediaIO
 * daughtercard's device 6), whose two serial ports carry no console or
 * record role of their own -- they are peripheral connections (a serial
 * tablet, an RS-422 deck), so this device offers only a plain chardev
 * id for each, no record backend and no board-level default.
 */

#include "qemu/osdep.h"
#include "hw/mips/sgi/baseio.h"
#include "chardev/char.h"
#include "hw/audio/sgi-rad1-audio.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci-host/sgi-bridge.h"
#include "hw/scsi/qlogic-isp.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "system/block-backend.h"

struct SGIBaseioState {
    DeviceState parent_obj;

    char *preset;
    PCIBus *pci_bus;
    char *part_override;
    bool revision_set;
    uint32_t revision_override;
    uint64_t onewire_serial;
    char *nvram_drive_id;
    char *chardev_a_id;
    char *chardev_b_id;
    char *record_a_id;
    char *record_b_id;
    char *parallel_chardev_id;
    char *mio_chardev_a_id;
    char *mio_chardev_b_id;
};

typedef bool (*SGIBaseioBuildFn)(SGIBaseioState *s, Error **errp);

typedef struct SGIBaseioPreset {
    const char *name;
    SGIBaseioPresetIdentity identity;
    SGIBaseioBuildFn build;
} SGIBaseioPreset;

/* Every board's own carrier resolves this from the PCI bus it hands us. */
static DeviceState *baseio_bus_host(PCIBus *bus)
{
    return DEVICE(BUS(bus)->parent);
}

/*
 * The io8 preset's chip, from the head-F bus sgi_xbridge_pci_bus()
 * handed us: the bus's own parent is the head (SGIXBridgeHeadState,
 * itself a PCIHostState), not the outer XBridge -- the "head-f-pci-intx"
 * and "head-f-pin" GPIO groups this preset wires live on that outer
 * chip (hw/pci-host/sgi-xbridge.h: "GPIO groups, all on the chip"),
 * one QOM level further up than baseio_bus_host() reaches.
 */
static DeviceState *io8_chip_from_bus(PCIBus *bus)
{
    Object *head = OBJECT(BUS(bus)->parent);

    return DEVICE(head->parent);
}

/*
 * Resolve a chardev id to its backend and set it on dev's own same-named
 * property, before dev realizes; see the header comment on
 * "chardev-a" and its siblings for why this device never holds a
 * "chardev" property of its own, and why an id naming no real backend
 * is refused here rather than left to build the IOC3 with that function
 * silently unbound.
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
        error_setg(errp, "sgi-baseio: no chardev backend named '%s' for %s",
                  id, prop);
        return false;
    }
    qdev_prop_set_chr(dev, prop, chr);
    return true;
}

static bool baseio_pass_drive(DeviceState *dev, const char *id, Error **errp)
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

/*
 * The IOC3 child every preset creates, differing only by population.
 * has_subsystem_id is false for ip29 and io8 (whose own PCI subsystem
 * fields are class defaults or, for io8, set afterward through raw
 * config space -- device identity, not a qdev property claim); baseio
 * sets it here, before realize, since "subsystem-id" is a qdev property
 * and setting one after realize is refused outright. has_phy is true
 * only for io8, whose IOC3 answers through a DP83843 at a fixed MII
 * address rather than the Origin 200/2000 boards' own DP83840A default,
 * for the same before-realize reason. timekeeper names the chip's own
 * Timekeeper part, SGI_IOC3_TIMEKEEPER_NONE for a chip that carries
 * none; nvram-drive is only ever forwarded to a chip that has one, so
 * an unrelated Timekeeper backend cannot be claimed by a chip that will
 * unparent its own rtc child unrealized. chardev_a_id/chardev_b_id/
 * record_a_id/record_b_id name the backend ids this chip's own two
 * serial ports claim, NULL for a chip whose ports claim none -- a
 * second IOC3 sharing a board with one that already claims this
 * device's own "chardev-a"/"chardev-b"/"record-a"/"record-b" (or a
 * chip whose ports are given a distinct property of their own instead,
 * such as the io6g preset's MediaIO chip and "mio-chardev-a"/
 * "mio-chardev-b"). child_name is the QOM child property this chip is
 * added under; every preset that builds a single IOC3 keeps the
 * established "ioc3", so its migration path is unchanged, while a
 * preset fitting a second chip (io6g's MediaIO daughtercard) must give
 * it a distinct name -- QOM refuses two children of the same name
 * under one parent.
 *
 * child_name also gives each chip's own nested kbc/kbd/mouse
 * (hw/input/sgi-ioc3-kbc.c) a distinct QOM canonical path today, e.g.
 * ".../mio-ioc3/kbc/kbd" beside ".../ioc3/kbc/kbd", inspectable now
 * through qom-list/qom-get. QMP's own input-send-event cannot yet key off
 * that path: its device= argument needs a QemuConsole bound to the
 * device (ui/console.c's qemu_console_lookup_by_device()), which no
 * headless BaseIO board creates, and separately, a device built through
 * object_initialize_child() like every kbd/mouse here never gets the
 * settable dev->id that lookup also requires (system/qdev-monitor.c's
 * qdev_set_id() refuses a device that already has a QOM parent). Binding
 * a console to one of these controllers is later console-wiring work;
 * this distinct QOM path is the seam it would key off, not something
 * that change still has to invent.
 */
static PCIDevice *baseio_build_ioc3(SGIBaseioState *s, const char *child_name,
                                    unsigned int device, uint8_t revision,
                                    uint32_t population, uint8_t timekeeper,
                                    bool has_subsystem_id,
                                    uint16_t subsystem_id, bool has_phy,
                                    uint8_t phy_addr,
                                    const char *chardev_a_id,
                                    const char *chardev_b_id,
                                    const char *record_a_id,
                                    const char *record_b_id,
                                    Error **errp)
{
    PCIDevice *ioc3 = pci_new(PCI_DEVFN(device, 0), TYPE_SGI_IOC3);

    object_property_add_child(OBJECT(s), child_name, OBJECT(ioc3));
    qdev_prop_set_uint32(DEVICE(ioc3), "population", population);
    qdev_prop_set_uint8(DEVICE(ioc3), "timekeeper-kind", timekeeper);
    qdev_prop_set_uint64(DEVICE(ioc3), "onewire-serial", s->onewire_serial);
    if (has_subsystem_id) {
        qdev_prop_set_uint16(DEVICE(ioc3), "subsystem-id", subsystem_id);
    }
    if (has_phy) {
        qdev_prop_set_uint8(DEVICE(ioc3), "phy-addr", phy_addr);
        object_property_set_bool(OBJECT(ioc3), "phy-dp83843", true,
                                 &error_abort);
    }
    if (timekeeper != SGI_IOC3_TIMEKEEPER_NONE &&
        !baseio_pass_drive(DEVICE(ioc3), s->nvram_drive_id, errp)) {
        object_unparent(OBJECT(ioc3));
        return NULL;
    }
    if (!baseio_pass_chardev(DEVICE(ioc3), "chardev-a", chardev_a_id,
                             errp) ||
        !baseio_pass_chardev(DEVICE(ioc3), "chardev-b", chardev_b_id,
                             errp) ||
        !baseio_pass_chardev(DEVICE(ioc3), "record-a", record_a_id,
                             errp) ||
        !baseio_pass_chardev(DEVICE(ioc3), "record-b", record_b_id,
                             errp)) {
        object_unparent(OBJECT(ioc3));
        return NULL;
    }
    if ((population & SGI_IOC3_POPULATION_PARALLEL) &&
        !baseio_pass_chardev(DEVICE(ioc3), "parallel-chardev",
                             s->parallel_chardev_id, errp)) {
        object_unparent(OBJECT(ioc3));
        return NULL;
    }
    pci_realize_and_unref(ioc3, s->pci_bus, &error_abort);
    pci_set_byte(ioc3->config + PCI_REVISION_ID, revision);
    return ioc3;
}

#define IP29_ISP_DEVICE_0 0
#define IP29_ISP_DEVICE_1 1
#define IP29_ISP_REVISION 5
#define IP29_IOC3_DEVICE   2
#define IP29_IOC3_REVISION 1
#define IP29_IOC3_IRQ_PIN  3
#define IP29_IOC3_POPULATION (SGI_IOC3_POPULATION_ETHERNET | \
                              SGI_IOC3_POPULATION_SERIAL_A | \
                              SGI_IOC3_POPULATION_SERIAL_B | \
                              SGI_IOC3_POPULATION_KEYBOARD_MOUSE | \
                              SGI_IOC3_POPULATION_PARALLEL | \
                              SGI_IOC3_POPULATION_ONEWIRE_IDENTITY)

static bool baseio_build_ip29(SGIBaseioState *s, Error **errp)
{
    PCIDevice *isp0, *isp1, *ioc3;

    isp0 = pci_new(PCI_DEVFN(IP29_ISP_DEVICE_0, 0), "qlogic-isp1040");
    object_property_add_child(OBJECT(s), "isp0", OBJECT(isp0));
    pci_realize_and_unref(isp0, s->pci_bus, &error_abort);
    pci_set_byte(isp0->config + PCI_REVISION_ID, IP29_ISP_REVISION);
    qlogic_isp_handle_legacy_cmdline(DEVICE(isp0));

    isp1 = pci_new(PCI_DEVFN(IP29_ISP_DEVICE_1, 0), "qlogic-isp1040");
    object_property_add_child(OBJECT(s), "isp1", OBJECT(isp1));
    pci_realize_and_unref(isp1, s->pci_bus, &error_abort);
    pci_set_byte(isp1->config + PCI_REVISION_ID, IP29_ISP_REVISION);
    qlogic_isp_handle_legacy_cmdline(DEVICE(isp1));

    ioc3 = baseio_build_ioc3(s, "ioc3", IP29_IOC3_DEVICE, IP29_IOC3_REVISION,
                             IP29_IOC3_POPULATION,
                             SGI_IOC3_TIMEKEEPER_M48T35, false, 0, false, 0,
                             s->chardev_a_id, s->chardev_b_id,
                             s->record_a_id, s->record_b_id, errp);
    if (!ioc3) {
        return false;
    }
    /* Direct board wiring, not the bus's own per-slot PCI INTx rule. */
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in(baseio_bus_host(s->pci_bus),
                                           IP29_IOC3_IRQ_PIN));
    return true;
}

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

/*
 * Both SCSI controllers and the device-2 IOC3 every BaseIO board carries.
 * The device-2 IOC3's population is a caller-supplied argument: the io6g
 * variant's own board IOC3 carries a keyboard/mouse pair beside plain
 * io6's ethernet, serial and one-wire population (see
 * BASEIO_G_IOC3_POPULATION below), while everything else about the chip
 * and both SCSI controllers is identical between the two boards.
 */
static bool baseio_build_baseio_core(SGIBaseioState *s,
                                     uint32_t ioc3_population, Error **errp)
{
    PCIDevice *scsi0, *scsi1, *ioc3;

    scsi0 = pci_new(PCI_DEVFN(BASEIO_SCSI0_DEVICE, 0), "qlogic-isp1040");
    object_property_add_child(OBJECT(s), "scsi0", OBJECT(scsi0));
    pci_realize_and_unref(scsi0, s->pci_bus, &error_abort);
    pci_set_byte(scsi0->config + PCI_REVISION_ID, BASEIO_SCSI_REVISION);
    qlogic_isp_handle_legacy_cmdline(DEVICE(scsi0));

    scsi1 = pci_new(PCI_DEVFN(BASEIO_SCSI1_DEVICE, 0), "qlogic-isp1040");
    object_property_add_child(OBJECT(s), "scsi1", OBJECT(scsi1));
    pci_realize_and_unref(scsi1, s->pci_bus, &error_abort);
    pci_set_byte(scsi1->config + PCI_REVISION_ID, BASEIO_SCSI_REVISION);
    qlogic_isp_handle_legacy_cmdline(DEVICE(scsi1));

    ioc3 = baseio_build_ioc3(s, "ioc3", BASEIO_IOC3_DEVICE,
                             BASEIO_IOC3_REVISION, ioc3_population,
                             SGI_IOC3_TIMEKEEPER_M48T35, true,
                             BASEIO_IOC3_SUBSYSTEM_ID_NOT_OBSERVED, false, 0,
                             s->chardev_a_id, s->chardev_b_id,
                             s->record_a_id, s->record_b_id, errp);
    if (!ioc3) {
        return false;
    }
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in(baseio_bus_host(s->pci_bus),
                                           BASEIO_IOC3_IRQ_PIN));
    return true;
}

static bool baseio_build_io6(SGIBaseioState *s, Error **errp)
{
    return baseio_build_baseio_core(s, BASEIO_IOC3_POPULATION, errp);
}

#define BASEIO_G_MIO_IOC3_DEVICE   6
#define BASEIO_G_MIO_IOC3_REVISION 1
/*
 * Device 6's SuperIO interrupt reaches Bridge input 6: with no ethernet
 * this IOC3 is single-connection, so its shared interrupt lands on the
 * chip's own PCI INTA, which the Bridge's favoured per-slot mapping sends
 * to input 6. The board IOC3 at device 2 rides input 4 instead. Confirmed
 * by 6.5.30 disassembly of the IOC3 interrupt-allocation path and a live
 * IRIX 6.5.30 io6g boot whose keyboard and mouse enumerate at pci/6.
 */
#define BASEIO_G_MIO_IOC3_IRQ_PIN  6
#define BASEIO_G_MIO_IOC3_SUBSYSTEM_ID SGI_IOC3_SUBSYS_IP27_MIO
#define BASEIO_G_MIO_IOC3_POPULATION (SGI_IOC3_POPULATION_SERIAL_A | \
                                      SGI_IOC3_POPULATION_SERIAL_B | \
                                      SGI_IOC3_POPULATION_KEYBOARD_MOUSE | \
                                      SGI_IOC3_POPULATION_PARALLEL)

/*
 * The io6g's own board IOC3 at device 2 carries a keyboard/mouse pair
 * beside plain io6's ethernet, serial and one-wire population -- half
 * of the board's four PS/2 ports, the MediaIO IOC3 above supplying the
 * other half. IRIX only enables this chip's keyboard/mouse when the
 * Bridge's own board-part record reads a "030-0734-" (io6g) prefix rather
 * than plain io6's "030-1124-" (docs/datasheets/baseio.md's
 * board-variant table); sgi_baseio_presets[]'s "io6g" entry below
 * carries that part.
 */
#define BASEIO_G_IOC3_POPULATION (BASEIO_IOC3_POPULATION | \
                                  SGI_IOC3_POPULATION_KEYBOARD_MOUSE)

/*
 * The MediaIO daughtercard's own board-part record on the Bridge's
 * one-wire bus (docs/datasheets/baseio.md's board-variant table). Without
 * it, device 6's IOC3 has no board record of its own on that bus, and
 * IRIX's one-wire dispatch falls back to a default population that turns
 * ethernet on for a chip that was never wired to one, driving pci/6 to
 * probe a PHY that does not exist. The serial only has to read back
 * distinct from the Bridge's own board record so a one-wire search can
 * tell the two apart; it names no real part, so it continues the same
 * placeholder descending-byte idiom as NIC_ROM_SERIAL_DEFAULT
 * (hw/pci-host/sgi-bridge.c) and SGI_XBRIDGE_NIC_PLACEHOLDER_SERIAL
 * (hw/pci-host/sgi-xbridge.c) one step further along it.
 */
#define BASEIO_G_MIO_BOARD_PART     "030-0880-003"
#define BASEIO_G_MIO_ONEWIRE_SERIAL 0x0c0b0a090807ULL

/*
 * The MediaIO daughtercard's RAD1 audio ASIC. No source establishes which
 * Bridge slot the daughtercard wires it to, so device 5 is this model's
 * own placement, chosen only because it is the sole slot devices 0-2 and
 * 6 above leave free on this board's Bridge -- an inference flagged here
 * rather than a captured fact, per docs/datasheets/mediaio-rad1-audio.md.
 * It carries no interrupt wiring: the datasheet establishes no interrupt
 * behavior for this device, and no guest install this model was built
 * against installs a handler or enables one for it.
 */
#define BASEIO_G_RAD1_DEVICE 5

/*
 * io6g: the io6 core with its device-2 IOC3 carrying a
 * keyboard/mouse pair of its own (BASEIO_G_IOC3_POPULATION), plus the
 * MediaIO daughtercard's own board-part record and second IOC3 at
 * device 6 -- PS/2 keyboard/mouse, two more serial ports and a parallel
 * port; no ethernet, no IOC3-level one-wire identity, no Timekeeper
 * (docs/datasheets/baseio.md, docs/datasheets/ioc3.md's board-variant
 * table, subsystem id 0xc301). Between the two IOC3s io6g exposes four
 * PS/2 ports total. This chip's own two serial ports are peripheral
 * connections, not a console or record backend -- device 2's IOC3
 * already owns "chardev-a"/"chardev-b"/"record-a"/"record-b" for this
 * board's chosen console and record backends -- so they claim their
 * backend through this device's own "mio-chardev-a"/"mio-chardev-b"
 * instead, with no record counterpart. The daughtercard's RAD1 audio
 * ASIC rides the same Bridge at device 5 (BASEIO_G_RAD1_DEVICE above).
 */
static bool baseio_build_io6g(SGIBaseioState *s, Error **errp)
{
    PCIDevice *ioc3, *rad1;

    if (!baseio_build_baseio_core(s, BASEIO_G_IOC3_POPULATION, errp)) {
        return false;
    }

    if (!sgi_bridge_add_onewire_board_part(baseio_bus_host(s->pci_bus),
                                           BASEIO_G_MIO_BOARD_PART,
                                           BASEIO_G_MIO_ONEWIRE_SERIAL,
                                           errp)) {
        return false;
    }

    ioc3 = baseio_build_ioc3(s, "mio-ioc3", BASEIO_G_MIO_IOC3_DEVICE,
                             BASEIO_G_MIO_IOC3_REVISION,
                             BASEIO_G_MIO_IOC3_POPULATION,
                             SGI_IOC3_TIMEKEEPER_NONE, true,
                             BASEIO_G_MIO_IOC3_SUBSYSTEM_ID, false, 0,
                             s->mio_chardev_a_id, s->mio_chardev_b_id,
                             NULL, NULL, errp);
    if (!ioc3) {
        return false;
    }
    /*
     * Only an IOC3's ethernet child drives its PCI INTA
     * (hw/misc/sgi-ioc3.c); this chip has none, so its serial ports,
     * keyboard/mouse, parallel port and real-time interrupt all ride
     * its SuperIO GPIO-out index 0 alone. Left unwired, those
     * interrupts are simply dead.
     */
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in(baseio_bus_host(s->pci_bus),
                                           BASEIO_G_MIO_IOC3_IRQ_PIN));

    rad1 = pci_new(PCI_DEVFN(BASEIO_G_RAD1_DEVICE, 0), TYPE_SGI_RAD1_AUDIO);
    object_property_add_child(OBJECT(s), "rad1", OBJECT(rad1));
    pci_realize_and_unref(rad1, s->pci_bus, &error_abort);

    return true;
}

#define IO8_SCSI_DEVICE           1
#define IO8_SCSI_REVISION         6
#define IO8_SCSI_INTERRUPT_PIN    1
#define IO8_SCSI_SUBSYSTEM_VENDOR 0xffff
#define IO8_SCSI_SUBSYSTEM_ID     0xffff

#define IO8_IOC3_DEVICE           4
#define IO8_IOC3_REVISION         1
#define IO8_IOC3_INTERRUPT_PIN    4
#define IO8_IOC3_SUPERIO_PIN      0
#define IO8_IOC3_SUBSYSTEM_VENDOR 0x10a9
#define IO8_IOC3_SUBSYSTEM_ID_NOT_OBSERVED 0xffff
#define IO8_IOC3_POPULATION (SGI_IOC3_POPULATION_ETHERNET | \
                            SGI_IOC3_POPULATION_SERIAL_A | \
                            SGI_IOC3_POPULATION_SERIAL_B | \
                            SGI_IOC3_POPULATION_PARALLEL)
/* The IO-8's own transceiver: DP83843 at MII address 1 (dp83840.md). */
#define IO8_IOC3_PHY_ADDR 1

#define IO8_USB_DEVICE           5
#define IO8_USB_REVISION         0x10
#define IO8_USB_INTERRUPT_PIN    5
#define IO8_USB_SUBSYSTEM_VENDOR 0xffff
#define IO8_USB_SUBSYSTEM_ID     0xffff

/*
 * A head has no automatic per-slot INTx routing (unlike Bridge), so
 * every function's own PCI INTA is wired explicitly onto the chip's
 * "head-f-pci-intx" raw group, itself re-driven at the chip's own
 * "head-f-pin" input the board wiring names.
 */
static void io8_wire_intx(DeviceState *chip, unsigned int device,
                          unsigned int pin)
{
    qdev_connect_gpio_out_named(chip, "head-f-pci-intx",
                                device * PCI_NUM_PINS,
                                qdev_get_gpio_in_named(chip, "head-f-pin",
                                                       pin));
}

static bool baseio_build_io8(SGIBaseioState *s, Error **errp)
{
    DeviceState *chip = io8_chip_from_bus(s->pci_bus);
    PCIDevice *scsi, *ioc3, *usb;

    scsi = pci_new(PCI_DEVFN(IO8_SCSI_DEVICE, 0), "qlogic-isp12160");
    object_property_add_child(OBJECT(s), "scsi", OBJECT(scsi));
    pci_realize_and_unref(scsi, s->pci_bus, &error_abort);
    pci_set_byte(scsi->config + PCI_REVISION_ID, IO8_SCSI_REVISION);
    pci_set_word(scsi->config + PCI_SUBSYSTEM_VENDOR_ID,
                IO8_SCSI_SUBSYSTEM_VENDOR);
    pci_set_word(scsi->config + PCI_SUBSYSTEM_ID, IO8_SCSI_SUBSYSTEM_ID);
    io8_wire_intx(chip, IO8_SCSI_DEVICE, IO8_SCSI_INTERRUPT_PIN);

    ioc3 = baseio_build_ioc3(s, "ioc3", IO8_IOC3_DEVICE, IO8_IOC3_REVISION,
                             IO8_IOC3_POPULATION,
                             SGI_IOC3_TIMEKEEPER_M48T35, false, 0, true,
                             IO8_IOC3_PHY_ADDR,
                             s->chardev_a_id, s->chardev_b_id,
                             s->record_a_id, s->record_b_id, errp);
    if (!ioc3) {
        return false;
    }
    pci_set_word(ioc3->config + PCI_SUBSYSTEM_VENDOR_ID,
                IO8_IOC3_SUBSYSTEM_VENDOR);
    pci_set_word(ioc3->config + PCI_SUBSYSTEM_ID,
                IO8_IOC3_SUBSYSTEM_ID_NOT_OBSERVED);
    io8_wire_intx(chip, IO8_IOC3_DEVICE, IO8_IOC3_INTERRUPT_PIN);
    /* The chip's own internal SuperIO real-time event, not a PCI pin. */
    qdev_connect_gpio_out(DEVICE(ioc3), 0,
                          qdev_get_gpio_in_named(chip, "head-f-pin",
                                                 IO8_IOC3_SUPERIO_PIN));

    usb = pci_new(PCI_DEVFN(IO8_USB_DEVICE, 0), "lucent-ohci");
    object_property_add_child(OBJECT(s), "usb", OBJECT(usb));
    pci_realize_and_unref(usb, s->pci_bus, &error_abort);
    pci_set_byte(usb->config + PCI_REVISION_ID, IO8_USB_REVISION);
    pci_set_word(usb->config + PCI_SUBSYSTEM_VENDOR_ID,
                IO8_USB_SUBSYSTEM_VENDOR);
    pci_set_word(usb->config + PCI_SUBSYSTEM_ID, IO8_USB_SUBSYSTEM_ID);
    io8_wire_intx(chip, IO8_USB_DEVICE, IO8_USB_INTERRUPT_PIN);
    return true;
}

static const SGIBaseioPreset sgi_baseio_presets[] = {
    { "ip29", { "030-1244-001", 4 }, baseio_build_ip29 },
    { "io6", { "030-1124-002", 4 }, baseio_build_io6 },
    { "io6g", { "030-0734-002", 4 }, baseio_build_io6g },
    { "io8", { "030-1673-003", 0 }, baseio_build_io8 },
};

static const SGIBaseioPreset *baseio_preset_find(const char *name)
{
    size_t i;

    if (!name) {
        return NULL;
    }
    for (i = 0; i < ARRAY_SIZE(sgi_baseio_presets); i++) {
        if (g_str_equal(name, sgi_baseio_presets[i].name)) {
            return &sgi_baseio_presets[i];
        }
    }
    return NULL;
}

const SGIBaseioPresetIdentity *sgi_baseio_preset_lookup(const char *name)
{
    const SGIBaseioPreset *preset = baseio_preset_find(name);

    return preset ? &preset->identity : NULL;
}

static char *baseio_get_part(Object *obj, Error **errp)
{
    SGIBaseioState *s = SGI_BASEIO(obj);
    const SGIBaseioPreset *preset = baseio_preset_find(s->preset);

    if (s->part_override) {
        return g_strdup(s->part_override);
    }
    return g_strdup(preset ? preset->identity.board_part : "");
}

static void baseio_set_part(Object *obj, const char *value, Error **errp)
{
    SGIBaseioState *s = SGI_BASEIO(obj);

    g_free(s->part_override);
    s->part_override = g_strdup(value);
}

static void baseio_get_revision(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    SGIBaseioState *s = SGI_BASEIO(obj);
    const SGIBaseioPreset *preset = baseio_preset_find(s->preset);
    uint32_t value = s->revision_set ? s->revision_override :
        (preset ? preset->identity.bridge_revision : 0);

    visit_type_uint32(v, name, &value, errp);
}

static void baseio_set_revision(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    SGIBaseioState *s = SGI_BASEIO(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    s->revision_override = value;
    s->revision_set = true;
}

static void baseio_realize(DeviceState *dev, Error **errp)
{
    SGIBaseioState *s = SGI_BASEIO(dev);
    const SGIBaseioPreset *preset = baseio_preset_find(s->preset);

    if (!preset) {
        error_setg(errp, "sgi-baseio: '%s' names no known preset; expected "
                  "ip29, io6, io6g or io8",
                  s->preset ? s->preset : "(unset)");
        return;
    }
    if (!s->pci_bus) {
        error_setg(errp, "sgi-baseio requires the PCI bus its preset "
                  "populates");
        return;
    }
    preset->build(s, errp);
}

static const Property baseio_properties[] = {
    DEFINE_PROP_STRING("preset", SGIBaseioState, preset),
    DEFINE_PROP_LINK("pci-bus", SGIBaseioState, pci_bus, TYPE_PCI_BUS,
                     PCIBus *),
    DEFINE_PROP_UINT64("onewire-serial", SGIBaseioState, onewire_serial,
                       SGI_IOC3_ONEWIRE_SERIAL),
    DEFINE_PROP_STRING("nvram-drive", SGIBaseioState, nvram_drive_id),
    DEFINE_PROP_STRING("chardev-a", SGIBaseioState, chardev_a_id),
    DEFINE_PROP_STRING("chardev-b", SGIBaseioState, chardev_b_id),
    DEFINE_PROP_STRING("record-a", SGIBaseioState, record_a_id),
    DEFINE_PROP_STRING("record-b", SGIBaseioState, record_b_id),
    DEFINE_PROP_STRING("parallel-chardev", SGIBaseioState,
                       parallel_chardev_id),
    DEFINE_PROP_STRING("mio-chardev-a", SGIBaseioState, mio_chardev_a_id),
    DEFINE_PROP_STRING("mio-chardev-b", SGIBaseioState, mio_chardev_b_id),
};

/* No state of its own outside the children the preset's build() owns. */
static const VMStateDescription vmstate_baseio = {
    .name = TYPE_SGI_BASEIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void baseio_instance_init(Object *obj)
{
    /*
     * "part" and "revision" have a preset-dependent default no static
     * Property table entry can express, so they are hand-added QOM
     * properties rather than DEFINE_PROP_* table entries: a carrier
     * building its own Bridge or XBridge reads these -- on this device,
     * unrealized, before its own chip exists for "pci-bus" to link to --
     * to learn what design part number and silicon revision that chip's
     * own identity properties should carry (see the header comment).
     */
    object_property_add_str(obj, "part", baseio_get_part, baseio_set_part);
    object_property_add(obj, "revision", "uint32", baseio_get_revision,
                        baseio_set_revision, NULL, NULL);
}

static void baseio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "SGI base I/O (IOC3, SCSI and, on the IO-8, USB)";
    dc->realize = baseio_realize;
    dc->vmsd = &vmstate_baseio;
    /* Every carrier builds this itself; a bare instance needs a chip's bus. */
    dc->user_creatable = true;
    device_class_set_props(dc, baseio_properties);
}

static const TypeInfo baseio_type = {
    .name = TYPE_SGI_BASEIO,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SGIBaseioState),
    .instance_init = baseio_instance_init,
    .class_init = baseio_class_init,
};

static void baseio_register_types(void)
{
    type_register_static(&baseio_type);
}

type_init(baseio_register_types)
