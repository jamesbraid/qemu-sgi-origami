/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Neterion/S2io Xframe 10-Gigabit Ethernet controller.
 *
 * See include/hw/net/xframe.h for what this device is. This models the
 * generic PCI card's own contract: PCI identity, the two BAR windows, the
 * netdev frontend, and the PIF (BAR0) register file the probe/attach path
 * binds on -- the byte-swapper diagnostic, SW_RESET, the RMAC_CFG_KEY
 * MAC-config lock, ADAPTER_STATUS/ADAPTER_CONTROL and the MAC-address table.
 * The registers are 64-bit and native little-endian; a host bridge that
 * corrects register byte order or access width (the Origin 200 Bridge) is that
 * bridge's concern, not this device's. The transmit descriptor/doorbell path
 * behind the TxP window is a follow-on slice and stays a LOG_UNIMP stub.
 *
 * QOM/PCI-realize/BAR shape and the ResettablePhases reset wiring follow
 * hw/scsi/qlogic-isp.c, the fork's PCI-endpoint reference -- not
 * hw/pci-host/astro.c, which is a PCI host bridge with a different
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
 * PIF register offsets within BAR0, and the bit fields the probe/attach path
 * touches. Public OpenBSD if_xgereg.h / Linux s2io-regs.h names, taken from
 * docs/datasheets/xframe.md's register map. All registers are 64-bit.
 */
#define XFRAME_SW_RESET             0x0100
#define XFRAME_ADAPTER_STATUS       0x0108
#define XFRAME_ADAPTER_CONTROL      0x0110
#define XFRAME_SWAPPER_CTRL         0x0908
#define XFRAME_PIF_RD_SWAPPER_FB    0x0910
#define XFRAME_XMSI_ADDRESS         0x0960
#define XFRAME_MAC_CFG              0x2100
#define XFRAME_RMAC_CFG_KEY         0x2120
#define XFRAME_RMAC_ADDR_CMD_MEM    0x2128
#define XFRAME_RMAC_ADDR_DATA0_MEM  0x2130
#define XFRAME_RMAC_ADDR_DATA1_MEM  0x2138

/*
 * The byte-swapper diagnostic returns this fixed constant at a correctly
 * ordered 64-bit read; the driver reads it back to confirm endianness. The
 * model returns it unconditionally, since it presents registers in native
 * order and never scrambles a read.
 */
#define XFRAME_SWAPPER_MAGIC        0x0123456789abcdefULL

/*
 * ADAPTER_STATUS quiescent/ready value the driver's single non-retrying attach
 * read requires (if_xgereg.h QUIESCENT): TDMA/RDMA/PFC ready, TMAC buffer
 * empty, PIC quiescent, memory-controller DRAM+queues ready, both PLLs locked
 * -- and the fault bits clear. The model is always ready, so it presents this
 * immediately.
 */
#define XFRAME_ADAPTER_STATUS_QUIESCENT  0xf40000c300000000ULL

/*
 * SW_RESET: writing the 0xa5a5a5 key in bits 63:40 (OpenBSD 0xa5a5a5<<40,
 * Linux SW_RESET_ALL) triggers a soft reset. The key is a write-only trigger
 * and does not persist; the readable content is the reset-lane control bits
 * below it.
 */
#define XFRAME_SW_RESET_KEY         0xa5a5a5ULL

/* RMAC_CFG_KEY: 0x4c0d in bits 63:48 unlocks the next MAC_CFG write. */
#define XFRAME_RMAC_CFG_KEY_VALUE   0x4c0dULL

/*
 * RMAC_ADDR_CMD_MEM: write-enable selects write vs read, the strobe is
 * self-clearing on completion, and the 6-bit offset selects a table entry.
 */
#define XFRAME_RMAC_ADDR_CMD_WE       (1ULL << 56)
#define XFRAME_RMAC_ADDR_CMD_STROBE   (1ULL << 48)
#define XFRAME_RMAC_ADDR_CMD_OFF_MASK 0x3fULL

/*
 * Pack the netdev's configured MAC into MAC-table entry 0 the way the factory
 * programs it: the six bytes in RMAC_ADDR_DATA0_MEM bits 63:16, most
 * significant byte first (the order both drivers unpack it in).
 */
static void xframe_load_factory_mac(XframeState *s)
{
    uint64_t mac = 0;
    size_t i;

    for (i = 0; i < sizeof(s->conf.macaddr.a); i++) {
        mac |= (uint64_t)s->conf.macaddr.a[i] << (56 - 8 * i);
    }
    s->mac_addr[0] = mac;
}

/*
 * Return the register file to its reset values. Shared by cold reset and the
 * SW_RESET key path. It touches only device register state, never PCI config
 * or BAR state: real hardware's SW_RESET can perturb config space, but this
 * model must leave that to the PCI core (docs/datasheets/xframe.md's VMState
 * note).
 */
static void xframe_soft_reset(XframeState *s)
{
    s->swapper_ctrl = 0;
    s->xmsi_address = 0;
    s->sw_reset = 0;
    s->adapter_control = 0;
    s->mac_cfg = 0;
    s->rmac_cfg_unlocked = false;
    s->rmac_addr_cmd = 0;
    s->rmac_addr_data0 = 0;
    s->rmac_addr_data1 = 0;
    memset(s->mac_addr, 0, sizeof(s->mac_addr));
    xframe_load_factory_mac(s);
}

/*
 * The MAC-address table's indirect command: a strobe carries out a read or a
 * write of one entry and then self-clears, so a driver polling the strobe bit
 * sees the operation complete at once.
 */
static void xframe_rmac_addr_cmd(XframeState *s, uint64_t val)
{
    unsigned offset = (val >> 32) & XFRAME_RMAC_ADDR_CMD_OFF_MASK;

    if (val & XFRAME_RMAC_ADDR_CMD_STROBE) {
        if (val & XFRAME_RMAC_ADDR_CMD_WE) {
            s->mac_addr[offset] = s->rmac_addr_data0;
        } else {
            s->rmac_addr_data0 = s->mac_addr[offset];
        }
        val &= ~XFRAME_RMAC_ADDR_CMD_STROBE;
    }
    s->rmac_addr_cmd = val;
}

/*
 * The PIF register window (BAR0). The registers are 64-bit with full-value
 * side effects on a single access (the swapper diagnostic, SW_RESET's key,
 * RMAC_CFG_KEY), so xframe_pif_ops declares an 8-byte impl and valid width:
 * every access reaches here as one 64-bit call. DEVICE_LITTLE_ENDIAN -- these
 * are little-endian-native parts; values are delivered in native order and a
 * host bridge composes whatever byte order the guest needs.
 */
static uint64_t xframe_pif_read(void *opaque, hwaddr addr, unsigned size)
{
    XframeState *s = opaque;

    switch (addr) {
    case XFRAME_ADAPTER_STATUS:
        return XFRAME_ADAPTER_STATUS_QUIESCENT;
    case XFRAME_PIF_RD_SWAPPER_FB:
        return XFRAME_SWAPPER_MAGIC;
    case XFRAME_SW_RESET:
        return s->sw_reset;
    case XFRAME_ADAPTER_CONTROL:
        return s->adapter_control;
    case XFRAME_SWAPPER_CTRL:
        return s->swapper_ctrl;
    case XFRAME_XMSI_ADDRESS:
        return s->xmsi_address;
    case XFRAME_MAC_CFG:
        return s->mac_cfg;
    case XFRAME_RMAC_CFG_KEY:
        return 0;  /* write-only unlock key */
    case XFRAME_RMAC_ADDR_CMD_MEM:
        return s->rmac_addr_cmd;
    case XFRAME_RMAC_ADDR_DATA0_MEM:
        return s->rmac_addr_data0;
    case XFRAME_RMAC_ADDR_DATA1_MEM:
        return s->rmac_addr_data1;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "xframe: PIF read at 0x%" PRIx64 " (size %u) is not "
                      "modelled\n", addr, size);
        return 0;
    }
}

static void xframe_pif_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    XframeState *s = opaque;

    switch (addr) {
    case XFRAME_SW_RESET:
        if ((val >> 40) == XFRAME_SW_RESET_KEY) {
            xframe_soft_reset(s);
        } else {
            s->sw_reset = val;
        }
        return;
    case XFRAME_ADAPTER_CONTROL:
        s->adapter_control = val;
        return;
    case XFRAME_SWAPPER_CTRL:
        s->swapper_ctrl = val;
        return;
    case XFRAME_XMSI_ADDRESS:
        s->xmsi_address = val;
        return;
    case XFRAME_RMAC_CFG_KEY:
        s->rmac_cfg_unlocked = (val >> 48) == XFRAME_RMAC_CFG_KEY_VALUE;
        return;
    case XFRAME_MAC_CFG:
        /* Write-locked: takes only with the config key freshly written. */
        if (s->rmac_cfg_unlocked) {
            s->mac_cfg = val;
            s->rmac_cfg_unlocked = false;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xframe: MAC_CFG write without RMAC_CFG_KEY, "
                          "ignored\n");
        }
        return;
    case XFRAME_RMAC_ADDR_DATA0_MEM:
        s->rmac_addr_data0 = val;
        return;
    case XFRAME_RMAC_ADDR_DATA1_MEM:
        s->rmac_addr_data1 = val;
        return;
    case XFRAME_RMAC_ADDR_CMD_MEM:
        xframe_rmac_addr_cmd(s, val);
        return;
    case XFRAME_ADAPTER_STATUS:
    case XFRAME_PIF_RD_SWAPPER_FB:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xframe: write to read-only PIF register 0x%" PRIx64
                      ", ignored\n", addr);
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "xframe: PIF write at 0x%" PRIx64 " = 0x%" PRIx64
                      " (size %u) is not modelled\n", addr, val, size);
        return;
    }
}

static const MemoryRegionOps xframe_pif_ops = {
    .read = xframe_pif_read,
    .write = xframe_pif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 8,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8,
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

/*
 * The TxP transmit descriptor/doorbell registers are 64-bit like the PIF
 * window's, so the widths match even though the window itself is still a stub:
 * the transmit path is a follow-on slice.
 */
static const MemoryRegionOps xframe_txp_ops = {
    .read = xframe_txp_read,
    .write = xframe_txp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 8,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8,
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
     * Put the register file back to its documented reset values (including
     * reloading the factory MAC into table entry 0), then deassert the
     * interrupt line so a freshly reset or migrated-in device starts with the
     * pin low rather than however qdev left it.
     */
    xframe_soft_reset(s);
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
 * The migratable register state, per docs/datasheets/xframe.md's "VMState /
 * migration fields": the byte-swap and scratch registers, ADAPTER_CONTROL,
 * MAC_CFG and its unlock latch, and the whole MAC-address table (entry 0's
 * factory address plus any programmed entry -- whatever the guest wrote must
 * survive migration exactly). The netdev frontend's own state lives in NICConf,
 * which every NIC model leaves out of its VMState (see hw/net/sgi-ioc3-eth.c
 * and hw/net/rtl8139.c); PCI config space is covered by VMSTATE_PCI_DEVICE. The
 * transmit/receive ring bookkeeping joins this when that slice lands.
 */
static const VMStateDescription vmstate_xframe = {
    .name = "xframe",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, XframeState),
        VMSTATE_UINT64(swapper_ctrl, XframeState),
        VMSTATE_UINT64(xmsi_address, XframeState),
        VMSTATE_UINT64(sw_reset, XframeState),
        VMSTATE_UINT64(adapter_control, XframeState),
        VMSTATE_UINT64(mac_cfg, XframeState),
        VMSTATE_BOOL(rmac_cfg_unlocked, XframeState),
        VMSTATE_UINT64(rmac_addr_cmd, XframeState),
        VMSTATE_UINT64(rmac_addr_data0, XframeState),
        VMSTATE_UINT64(rmac_addr_data1, XframeState),
        VMSTATE_UINT64_ARRAY(mac_addr, XframeState, XFRAME_MAC_TABLE_ENTRIES),
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
