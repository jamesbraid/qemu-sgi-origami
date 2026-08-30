/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Neterion/S2io Xframe 10-Gigabit Ethernet controller.
 *
 * See include/hw/net/xframe.h for what this device is and what this slice
 * covers. In short: PCI identity, the two BAR windows, and the netdev
 * frontend are real; the 64-bit PIF/TxP register file behind the windows
 * is not modelled yet, and every access to either window logs LOG_UNIMP
 * and returns 0. QOM/PCI-realize/BAR shape and the ResettablePhases reset
 * wiring follow hw/scsi/qlogic-isp.c, the fork's PCI-endpoint reference --
 * not hw/pci-host/astro.c, which is a PCI host bridge with a different
 * TypeInfo/realize/BAR shape and which calls the forbidden
 * device_class_set_legacy_reset(). The netdev frontend follows
 * hw/net/sgi-ioc3-eth.c and upstream hw/net/dp8393x.c.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/net/xframe.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "qapi/error.h"

/*
 * Both BAR windows are placeholder-only in this slice: nothing behind them
 * has any state to read, and a write changes nothing. Declared 4-byte
 * min/max to match what the Origin 200 Bridge's own per-slot device window
 * currently serves (its impl.max_access_size = 4 splits any wider guest
 * access into two 4-byte device cycles before it reaches here regardless of
 * what this device declares -- docs/library/nic-on-bridge-conventions.md's
 * "Access width" note). The real PIF/TxP registers are 64-bit with
 * full-value side effects on a single access, which this narrow stub
 * cannot yet serve correctly either way; that is the follow-on slice,
 * gated on the Bridge owner's resolution of Task P3.
 *
 * DEVICE_LITTLE_ENDIAN is the ISP1040 precedent
 * (docs/library/nic-on-bridge-conventions.md's Layer A) and the starting
 * point named in the datasheet; which declared endianness actually
 * reproduces the driver's expected read-back through the Bridge is settled
 * by the byte-order qtest the register-file slice adds, not asserted here.
 */
static uint64_t xframe_pif_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "xframe: PIF register read at 0x%" PRIx64 " (size %u) -- "
                  "the register file is not modelled in this slice\n",
                  addr, size);
    return 0;
}

static void xframe_pif_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "xframe: PIF register write at 0x%" PRIx64
                  " = 0x%" PRIx64 " (size %u) -- the register file is not "
                  "modelled in this slice\n", addr, val, size);
}

static const MemoryRegionOps xframe_pif_ops = {
    .read = xframe_pif_read,
    .write = xframe_pif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static uint64_t xframe_txp_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "xframe: TxP window read at 0x%" PRIx64 " (size %u) -- "
                  "the transmit doorbell is not modelled in this slice\n",
                  addr, size);
    return 0;
}

static void xframe_txp_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "xframe: TxP window write at 0x%" PRIx64
                  " = 0x%" PRIx64 " (size %u) -- the transmit doorbell is "
                  "not modelled in this slice\n", addr, val, size);
}

static const MemoryRegionOps xframe_txp_ops = {
    .read = xframe_txp_read,
    .write = xframe_txp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/*
 * The receive path is a follow-on slice (Task 3): there is no ring to
 * accept a frame into yet, so declining every frame here is the genuine
 * "no buffer available" case the can_receive/receive contract asks for,
 * not a frame this device inspects and drops -- see
 * docs/superpowers/plans/2026-08-29-xframe-ethernet-adapter.md's
 * can_receive/receive note. can_receive() always returning false means
 * receive() below is never called; it exists only so NetClientInfo has a
 * valid pointer.
 */
static bool xframe_can_receive(NetClientState *nc)
{
    return false;
}

static ssize_t xframe_receive(NetClientState *nc, const uint8_t *buf,
                              size_t size)
{
    return 0;
}

static NetClientInfo xframe_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = xframe_can_receive,
    .receive = xframe_receive,
};

static void xframe_reset_hold(Object *obj, ResetType type)
{
    XframeState *s = XFRAME(obj);

    /*
     * The register file this slice defers has no state of its own yet to
     * put back to its documented reset values. Reset still owns the
     * interrupt line, so a freshly reset (or migrated-in) device starts
     * deasserted rather than however qdev happened to leave the pin.
     */
    pci_set_irq(PCI_DEVICE(s), 0);
}

static void xframe_realize(PCIDevice *dev, Error **errp)
{
    XframeState *s = XFRAME(dev);
    DeviceState *d = DEVICE(dev);

    /*
     * The class-level defaults set vendor/device id (and, absent a
     * subsystem property, QEMU's own generic subsystem default) before
     * this realize runs; every field here is a property, so restate all
     * four explicitly rather than let the generic default leak into a
     * field this device does expose.
     */
    pci_config_set_vendor_id(dev->config, s->vendor_id);
    pci_config_set_device_id(dev->config, s->device_id);
    pci_set_word(dev->config + PCI_SUBSYSTEM_VENDOR_ID, s->subsystem_vendor_id);
    pci_set_word(dev->config + PCI_SUBSYSTEM_ID, s->subsystem_id);

    /*
     * One interrupt line, INTx -- see docs/datasheets/xframe.md's
     * "Interrupts": legacy PCI INTx is the era-correct choice for the
     * Origin 200; MSI/MSI-X is optional future work.
     */
    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    memory_region_init_io(&s->pif_mmio, OBJECT(s), &xframe_pif_ops, s,
                          "xframe.pif", XFRAME_PIF_WINDOW_SIZE);
    memory_region_init_io(&s->txp_mmio, OBJECT(s), &xframe_txp_ops, s,
                          "xframe.txp", XFRAME_TXP_WINDOW_SIZE);

    /*
     * BAR0 is a 64-bit memory BAR (config 0x10/0x14), so the next BAR
     * lands at 0x18 -- PCI BAR index 2, not 1, since index 1 is BAR0's own
     * upper half. docs/datasheets/xframe.md's "PCI identity" table.
     */
    pci_register_bar(dev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->pif_mmio);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->txp_mmio);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&xframe_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), d->id,
                          &d->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void xframe_exit(PCIDevice *dev)
{
    XframeState *s = XFRAME(dev);

    qemu_del_nic(s->nic);
}

/*
 * Nothing beyond the standard pci_device fields exists to migrate in this
 * slice -- both windows are stateless stubs and the netdev frontend's own
 * state lives in NICConf, which every NIC model leaves out of its own
 * VMState (see hw/net/sgi-ioc3-eth.c and hw/net/rtl8139.c). Present from
 * day one, per the shared convention, so later slices add fields rather
 * than introduce the description: the register file, ring state and
 * XMSI_ADDRESS scratch value docs/datasheets/xframe.md's "VMState /
 * migration fields" lists.
 */
static const VMStateDescription vmstate_xframe = {
    .name = "xframe",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, XframeState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property xframe_properties[] = {
    DEFINE_PROP_UINT16("vendor-id", XframeState, vendor_id,
                       XFRAME_PCI_VENDOR_ID),
    DEFINE_PROP_UINT16("device-id", XframeState, device_id,
                       XFRAME_PCI_DEVICE_ID_XENA),
    /*
     * Generic default (0): the chip has no subsystem branding of its own.
     * The SGI board sets subsystem-vendor-id to 0x10a9; its
     * subsystem-device-id is not established from any source read for the
     * datasheet, so subsystem-id has no SGI-specific default to give it --
     * see include/hw/net/xframe.h.
     */
    DEFINE_PROP_UINT16("subsystem-vendor-id", XframeState,
                       subsystem_vendor_id, 0),
    DEFINE_PROP_UINT16("subsystem-id", XframeState, subsystem_id, 0),
    DEFINE_NIC_PROPERTIES(XframeState, conf),
};

static void xframe_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    k->realize = xframe_realize;
    k->exit = xframe_exit;
    k->vendor_id = XFRAME_PCI_VENDOR_ID;
    k->device_id = XFRAME_PCI_DEVICE_ID_XENA;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    rc->phases.hold = xframe_reset_hold;
    dc->desc = "Neterion/S2io Xframe 10-Gigabit Ethernet controller";
    dc->vmsd = &vmstate_xframe;
    device_class_set_props(dc, xframe_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo xframe_types[] = {
    {
        .name = TYPE_XFRAME,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(XframeState),
        .class_init = xframe_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(xframe_types)
