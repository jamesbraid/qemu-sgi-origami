/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT&T/Lucent OHCI USB controller, the two-port part SGI's I/O boards
 * carry. It is a plain pci-ohci with fixed PCI identifiers; the
 * behavioural differences are two root-hub ports rather than the pci-ohci
 * default of three, set through the inherited num-ports qdev property, a
 * fixed medium DEVSEL timing in the PCI Status word, set after the shared
 * OHCI wrapper realizes, and HcInterruptEnable's OpenHCI reset value.
 * Register overlays below keep what the driver writes where the part's
 * board tests require it and the shared core discards it, and switch
 * root-hub port power as the root-hub descriptors select.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "qom/object.h"
#include "hcd-ohci.h"

#define TYPE_LUCENT_OHCI "lucent-ohci"

typedef struct LucentOHCIClass {
    PCIDeviceClass parent_class;
    void (*parent_realize)(PCIDevice *dev, Error **errp);
    DeviceReset parent_reset;
} LucentOHCIClass;

DECLARE_CLASS_CHECKERS(LucentOHCIClass, LUCENT_OHCI, TYPE_LUCENT_OHCI)

/* The operational register a HostControllerReset is requested through. */
#define LUCENT_OHCI_COMMAND_STATUS  0x08
/* Root-hub power and over-current configuration. */
#define LUCENT_OHCI_RH_DESCRIPTOR_A 0x48
/* NPS, PSM, OCPM, NOCP and POTPGT: the driver-written fields. */
#define LUCENT_OHCI_RHA_WRITABLE    0xff001b00u
/* Root-hub DeviceRemovable and PortPowerControlMask. */
#define LUCENT_OHCI_RH_DESCRIPTOR_B 0x4c
/* HostControllerFunctionalState, which a write can take to USBRESET. */
#define LUCENT_OHCI_CONTROL         0x04
#define LUCENT_OHCI_COMMAND_HCR     0x01
/* FrameNumber, and HcControl's HostControllerFunctionalState. */
#define LUCENT_OHCI_FM_NUMBER       0x3c
#define LUCENT_OHCI_CONTROL_HCFS    0xc0
#define LUCENT_OHCI_HCFS_OPERATIONAL 0x80
/* HcRhDescriptorA's PowerSwitchingMode and NoPowerSwitching. */
#define LUCENT_OHCI_RHA_PSM         (1u << 8)
#define LUCENT_OHCI_RHA_NPS         (1u << 9)
/* HcRhStatus, and its ClearGlobalPower and SetGlobalPower write bits. */
#define LUCENT_OHCI_RH_STATUS       0x50
#define LUCENT_OHCI_RHS_LPS         (1u << 0)
#define LUCENT_OHCI_RHS_LPSC        (1u << 16)
/*
 * HcRhPortStatus[1], the first of one register per port, and its
 * PortPowerStatus (SetPortPower when written) and LowSpeedDeviceAttached
 * (ClearPortPower when written) bits.
 */
#define LUCENT_OHCI_RH_PORT_STATUS  0x54
#define LUCENT_OHCI_PORT_PPS        (1u << 8)
#define LUCENT_OHCI_PORT_LSDA       (1u << 9)
/* What the core's own power-off clears: PPS, CCS, PSS and PRS. */
#define LUCENT_OHCI_PORT_POWER_OFF  0x115u

/*
 * The reset values this part holds where the shared core's differ, after
 * the device's reset and after a HostControllerReset, which resets the
 * operational registers it does not except (OpenHCI 1.0a section 7.1.3):
 *
 * - HcInterruptEnable: every field resets to 0 in the reset tables of
 *   sections 7.1.5 and 7.1.6, MasterInterruptEnable included. One sentence
 *   in 7.1.6 says the master bit is set after a reset instead; the Lucent
 *   OHCI datasheet records why zero is taken, which SGI's IO-8 board test
 *   also expects.
 * - HcFmInterval's FSLargestDataPacket: the specification leaves it to the
 *   part ("TBD", section 7.3.1); zero rests on the board test alone.
 */
static void lucent_ohci_reset_values(OHCIState *ohci)
{
    ohci->intr = 0;
    ohci->fsmps = 0;
}

static OHCIState *lucent_ohci_core(PCIDevice *dev)
{
    return container_of(dev->io_regions[0].memory, OHCIState, mem);
}

/*
 * Straight to the core's handlers: both are little-endian four-byte
 * registers, and a dispatch through the memory API would be refused as the
 * same device re-entering itself.
 */
static uint64_t lucent_ohci_command_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    OHCIState *ohci = opaque;

    return ohci->mem.ops->read(ohci->mem.opaque,
                               LUCENT_OHCI_COMMAND_STATUS + addr, size);
}

static void lucent_ohci_command_write(void *opaque, hwaddr addr,
                                      uint64_t val, unsigned size)
{
    OHCIState *ohci = opaque;

    ohci->mem.ops->write(ohci->mem.opaque, LUCENT_OHCI_COMMAND_STATUS + addr,
                         val, size);
    if (addr == 0 && (val & LUCENT_OHCI_COMMAND_HCR)) {
        lucent_ohci_reset_values(ohci);
    }
}

/*
 * Access sizes pass through as the core's own region would take them; the
 * other overlays below follow the same pattern.
 */
static const MemoryRegionOps lucent_ohci_command_ops = {
    .read = lucent_ohci_command_read,
    .write = lucent_ohci_command_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lucent_ohci_instance_init(Object *obj)
{
    qdev_prop_set_uint32(DEVICE(obj), "num-ports", 2);
}

/*
 * HcRhDescriptorB: OpenHCI 1.0a makes DeviceRemovable (bits 15:0) and
 * PortPowerControlMask (bits 31:16) writable by the driver, one bit per
 * root-hub port from bit 1 of each half (section 7.4.2), and a
 * HostControllerReset leaves the root hub alone (section 7.1.3). The shared
 * core discards writes; this part keeps the bits for the ports it has.
 * Reads, and the power-on value, stay the core's.
 */
static uint64_t lucent_ohci_rhdesc_b_read(void *opaque, hwaddr addr,
                                          unsigned size)
{
    OHCIState *ohci = opaque;

    return ohci->mem.ops->read(ohci->mem.opaque,
                               LUCENT_OHCI_RH_DESCRIPTOR_B + addr, size);
}

static void lucent_ohci_rhdesc_b_write(void *opaque, hwaddr addr,
                                       uint64_t val, unsigned size)
{
    OHCIState *ohci = opaque;
    uint32_t ports = MAKE_64BIT_MASK(1, ohci->num_ports);

    /* The core's handler still sees it, for its trace and checks. */
    ohci->mem.ops->write(ohci->mem.opaque, LUCENT_OHCI_RH_DESCRIPTOR_B + addr,
                         val, size);
    if (addr == 0 && size == 4) {
        ohci->rhdesc_b = val & (ports | ports << 16);
    }
}

static const MemoryRegionOps lucent_ohci_rhdesc_b_ops = {
    .read = lucent_ohci_rhdesc_b_read,
    .write = lucent_ohci_rhdesc_b_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * HcRhDescriptorA: OpenHCI 1.0a makes NoPowerSwitching, PowerSwitchingMode,
 * OverCurrentProtectionMode, NoOverCurrentProtection and
 * PowerOnToPowerGoodTime writable by the driver (section 7.4.1); the shared
 * core discards writes to them. The port power overlays below act on the
 * two power-switching fields. Reads, the port count and the power-on value
 * stay the core's.
 */
static uint64_t lucent_ohci_rhdesc_a_read(void *opaque, hwaddr addr,
                                          unsigned size)
{
    OHCIState *ohci = opaque;

    return ohci->mem.ops->read(ohci->mem.opaque,
                               LUCENT_OHCI_RH_DESCRIPTOR_A + addr, size);
}

static void lucent_ohci_rhdesc_a_write(void *opaque, hwaddr addr,
                                       uint64_t val, unsigned size)
{
    OHCIState *ohci = opaque;

    ohci->mem.ops->write(ohci->mem.opaque, LUCENT_OHCI_RH_DESCRIPTOR_A + addr,
                         val, size);
    if (addr == 0 && size == 4) {
        ohci->rhdesc_a = (ohci->rhdesc_a & ~LUCENT_OHCI_RHA_WRITABLE) |
                         (val & LUCENT_OHCI_RHA_WRITABLE);
    }
}

static const MemoryRegionOps lucent_ohci_rhdesc_a_ops = {
    .read = lucent_ohci_rhdesc_a_read,
    .write = lucent_ohci_rhdesc_a_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * Entering USBRESET resets the root hub, and the shared core takes
 * HcRhDescriptorB back to its power-on zero with it. OpenHCI 1.0a leaves
 * the root-hub registers out of those USBRESET preserves and says the root
 * hub "is being reset" there (section 5.1.2.1), but gives the descriptor
 * registers' reset column as "Power-On Reset" and says they are written
 * during initialization (section 7.4). The IO-8 board test requires the
 * written port bits back afterwards, so this part keeps them; the Lucent
 * OHCI datasheet marks that inferred from the test. Everything else about
 * the write, the port resets included, is the core's.
 */
static uint64_t lucent_ohci_control_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    OHCIState *ohci = opaque;

    return ohci->mem.ops->read(ohci->mem.opaque, LUCENT_OHCI_CONTROL + addr,
                               size);
}

static void lucent_ohci_control_write(void *opaque, hwaddr addr,
                                      uint64_t val, unsigned size)
{
    OHCIState *ohci = opaque;
    uint32_t rhdesc_b = ohci->rhdesc_b;

    ohci->mem.ops->write(ohci->mem.opaque, LUCENT_OHCI_CONTROL + addr, val,
                         size);
    ohci->rhdesc_b = rhdesc_b;
}

static const MemoryRegionOps lucent_ohci_control_ops = {
    .read = lucent_ohci_control_read,
    .write = lucent_ohci_control_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * HcFmNumber: OpenHCI 1.0a, and the Lucent USS-312 datasheet's Table 42
 * after it, give FrameNumber as read-only to the driver and read/write to
 * the controller, and say nothing of what a driver write does. SGI's IO-8
 * and IO-7 board tests for this part write it while the controller is not
 * operational and require the value back, and the IO-8 test was retuned to
 * drop four other fields this part does not keep while keeping this one;
 * so this part is taken to latch a write while it is not counting frames,
 * as the Lucent OHCI datasheet records (inferred, not measured). A write
 * while operational is still discarded, as the core does: what the part
 * does then is unknown.
 */
static uint64_t lucent_ohci_fm_number_read(void *opaque, hwaddr addr,
                                           unsigned size)
{
    OHCIState *ohci = opaque;

    return ohci->mem.ops->read(ohci->mem.opaque, LUCENT_OHCI_FM_NUMBER + addr,
                               size);
}

static void lucent_ohci_fm_number_write(void *opaque, hwaddr addr,
                                        uint64_t val, unsigned size)
{
    OHCIState *ohci = opaque;

    ohci->mem.ops->write(ohci->mem.opaque, LUCENT_OHCI_FM_NUMBER + addr, val,
                         size);
    if (addr != 0 || size != 4) {
        return;
    }
    if ((ohci->ctl & LUCENT_OHCI_CONTROL_HCFS) ==
        LUCENT_OHCI_HCFS_OPERATIONAL) {
        qemu_log_mask(LOG_UNIMP, "lucent-ohci: HcFmNumber write 0x%" PRIx64
                      " while operational discarded\n", val);
        return;
    }
    ohci->frame_number = val & 0xffff;
}

static const MemoryRegionOps lucent_ohci_fm_number_ops = {
    .read = lucent_ohci_fm_number_read,
    .write = lucent_ohci_fm_number_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/*
 * Root-hub port power, as OpenHCI 1.0a sections 7.4.1 to 7.4.4 switch it.
 * With NoPowerSwitching set the ports are always powered and no command
 * reaches them. Otherwise PowerSwitchingMode clear lets only the global
 * commands (HcRhStatus) switch every port; set, it lets each port answer
 * only the per-port commands (HcRhPortStatus) when its PortPowerControlMask
 * bit in HcRhDescriptorB is set, and only the global ones when it is clear.
 * The shared core acts on both kinds for every port whatever the
 * descriptors say, and reports every port powered; these overlays filter
 * the commands and report PortPowerStatus as the port holds it.
 */
static bool lucent_ohci_port_switched(OHCIState *ohci, unsigned port,
                                      bool per_port)
{
    bool masked = ohci->rhdesc_b & (1u << (16 + 1 + port));

    if (ohci->rhdesc_a & LUCENT_OHCI_RHA_NPS) {
        return false;
    }
    if (!(ohci->rhdesc_a & LUCENT_OHCI_RHA_PSM)) {
        return !per_port;
    }
    return per_port == masked;
}

static uint64_t lucent_ohci_port_status_read(void *opaque, hwaddr addr,
                                             unsigned size)
{
    OHCIState *ohci = opaque;
    uint64_t val;

    val = ohci->mem.ops->read(ohci->mem.opaque,
                              LUCENT_OHCI_RH_PORT_STATUS + addr, size);
    /* The core reads every port as powered; only NoPowerSwitching may. */
    if (!(addr & 3) && !(ohci->rhdesc_a & LUCENT_OHCI_RHA_NPS)) {
        val &= ~(uint64_t)LUCENT_OHCI_PORT_PPS;
        val |= ohci->rhport[addr >> 2].ctrl & LUCENT_OHCI_PORT_PPS;
    }
    return val;
}

static void lucent_ohci_port_status_write(void *opaque, hwaddr addr,
                                          uint64_t val, unsigned size)
{
    OHCIState *ohci = opaque;

    if (!(addr & 3) && !lucent_ohci_port_switched(ohci, addr >> 2, true)) {
        val &= ~(uint64_t)(LUCENT_OHCI_PORT_PPS | LUCENT_OHCI_PORT_LSDA);
    }
    ohci->mem.ops->write(ohci->mem.opaque, LUCENT_OHCI_RH_PORT_STATUS + addr,
                         val, size);
}

static const MemoryRegionOps lucent_ohci_port_status_ops = {
    .read = lucent_ohci_port_status_read,
    .write = lucent_ohci_port_status_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static uint64_t lucent_ohci_rh_status_read(void *opaque, hwaddr addr,
                                           unsigned size)
{
    OHCIState *ohci = opaque;

    return ohci->mem.ops->read(ohci->mem.opaque, LUCENT_OHCI_RH_STATUS + addr,
                               size);
}

/*
 * The global commands go to the ports they may switch here rather than to
 * the core, which would switch them all. A power-off clears what the
 * core's own does, and neither changes an interrupt, as in the core.
 */
static void lucent_ohci_rh_status_write(void *opaque, hwaddr addr,
                                        uint64_t val, unsigned size)
{
    OHCIState *ohci = opaque;
    uint32_t power = 0;
    unsigned i;

    if (addr == 0) {
        power = val & (LUCENT_OHCI_RHS_LPS | LUCENT_OHCI_RHS_LPSC);
        val &= ~(uint64_t)power;
    }
    ohci->mem.ops->write(ohci->mem.opaque, LUCENT_OHCI_RH_STATUS + addr, val,
                         size);
    for (i = 0; power && i < ohci->num_ports; i++) {
        if (!lucent_ohci_port_switched(ohci, i, false)) {
            continue;
        }
        if (power & LUCENT_OHCI_RHS_LPS) {
            ohci->rhport[i].ctrl &= ~LUCENT_OHCI_PORT_POWER_OFF;
        }
        if (power & LUCENT_OHCI_RHS_LPSC) {
            ohci->rhport[i].ctrl |= LUCENT_OHCI_PORT_PPS;
        }
    }
}

static const MemoryRegionOps lucent_ohci_rh_status_ops = {
    .read = lucent_ohci_rh_status_read,
    .write = lucent_ohci_rh_status_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* Some of the core's registers, overlaid for as long as the device lives. */
static void lucent_ohci_overlay(PCIDevice *dev, OHCIState *ohci, hwaddr at,
                                uint64_t size, const MemoryRegionOps *ops,
                                const char *name)
{
    MemoryRegion *mr = g_new0(MemoryRegion, 1);

    memory_region_init_io(mr, OBJECT(dev), ops, ohci, name, size);
    memory_region_add_subregion_overlap(&ohci->mem, at, mr, 1);
}

static void lucent_ohci_realize(PCIDevice *dev, Error **errp)
{
    LucentOHCIClass *lc = LUCENT_OHCI_GET_CLASS(dev);
    Error *err = NULL;
    OHCIState *ohci;

    lc->parent_realize(dev, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    pci_set_word(dev->config + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);

    /*
     * The parent's instance struct is private to its file, so the overlay
     * regions are allocated rather than embedded.
     */
    ohci = lucent_ohci_core(dev);
    lucent_ohci_overlay(dev, ohci, LUCENT_OHCI_COMMAND_STATUS, 4,
                        &lucent_ohci_command_ops,
                        "lucent-ohci.command-status");
    lucent_ohci_overlay(dev, ohci, LUCENT_OHCI_RH_DESCRIPTOR_B, 4,
                        &lucent_ohci_rhdesc_b_ops,
                        "lucent-ohci.rh-descriptor-b");
    lucent_ohci_overlay(dev, ohci, LUCENT_OHCI_RH_DESCRIPTOR_A, 4,
                        &lucent_ohci_rhdesc_a_ops,
                        "lucent-ohci.rh-descriptor-a");
    lucent_ohci_overlay(dev, ohci, LUCENT_OHCI_CONTROL, 4,
                        &lucent_ohci_control_ops, "lucent-ohci.control");
    lucent_ohci_overlay(dev, ohci, LUCENT_OHCI_FM_NUMBER, 4,
                        &lucent_ohci_fm_number_ops, "lucent-ohci.fm-number");
    lucent_ohci_overlay(dev, ohci, LUCENT_OHCI_RH_STATUS, 4,
                        &lucent_ohci_rh_status_ops, "lucent-ohci.rh-status");
    lucent_ohci_overlay(dev, ohci, LUCENT_OHCI_RH_PORT_STATUS,
                        4 * ohci->num_ports, &lucent_ohci_port_status_ops,
                        "lucent-ohci.rh-port-status");
    lucent_ohci_reset_values(ohci);
}

static void lucent_ohci_reset(DeviceState *d)
{
    LucentOHCIClass *lc = LUCENT_OHCI_GET_CLASS(d);

    lc->parent_reset(d);
    lucent_ohci_reset_values(lucent_ohci_core(PCI_DEVICE(d)));
}

static void lucent_ohci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    LucentOHCIClass *lc = LUCENT_OHCI_CLASS(klass);

    k->vendor_id = 0x11c1;
    k->device_id = 0x5802;
    k->revision = 0x10;
    /* The hardware values are unknown; these are emulator placeholders. */
    k->subsystem_vendor_id = 0xffff;
    k->subsystem_id = 0xffff;
    lc->parent_realize = k->realize;
    k->realize = lucent_ohci_realize;
    lc->parent_reset = dc->legacy_reset;
    device_class_set_legacy_reset(dc, lucent_ohci_reset);
    dc->desc = "AT&T/Lucent USB Controller";
}

static const TypeInfo lucent_ohci_info = {
    .name          = TYPE_LUCENT_OHCI,
    .parent        = "pci-ohci",
    .instance_init = lucent_ohci_instance_init,
    .class_init    = lucent_ohci_class_init,
    .class_size    = sizeof(LucentOHCIClass),
};

static void lucent_ohci_register_types(void)
{
    type_register_static(&lucent_ohci_info);
}

type_init(lucent_ohci_register_types)
