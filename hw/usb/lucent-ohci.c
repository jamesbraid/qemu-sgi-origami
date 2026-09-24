/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT&T/Lucent OHCI USB controller, the two-port part SGI's I/O boards
 * carry. It is a plain pci-ohci with fixed PCI identifiers; the
 * behavioural differences are two root-hub ports rather than the pci-ohci
 * default of three, set through the inherited num-ports qdev property, a
 * fixed medium DEVSEL timing in the PCI Status word, set after the shared
 * OHCI wrapper realizes, and HcInterruptEnable's OpenHCI reset value.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
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
#define LUCENT_OHCI_COMMAND_HCR     0x01

/*
 * OpenHCI 1.0a's reset tables give every HcInterruptEnable field,
 * MasterInterruptEnable included, a reset value of 0 (sections 7.1.5 and
 * 7.1.6), and a HostControllerReset resets the operational registers it
 * does not except (section 7.1.3). One sentence in 7.1.6 says the master
 * bit is set after a reset instead; the Lucent OHCI datasheet records why
 * zero is taken, which SGI's IO-8 board test also expects. The shared core
 * leaves the bit set after either reset, so this part clears it.
 */
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
        ohci->intr = 0;
    }
}

/*
 * Only HcCommandStatus is overlaid; every other register is the core's.
 * Access sizes pass through as the core's own region would take them.
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

static void lucent_ohci_realize(PCIDevice *dev, Error **errp)
{
    LucentOHCIClass *lc = LUCENT_OHCI_GET_CLASS(dev);
    Error *err = NULL;
    MemoryRegion *command;
    OHCIState *ohci;

    lc->parent_realize(dev, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    pci_set_word(dev->config + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);

    /*
     * The parent's instance struct is private to its file, so this part's
     * one region is allocated rather than embedded; the device lives as
     * long as the machine.
     */
    ohci = lucent_ohci_core(dev);
    command = g_new0(MemoryRegion, 1);
    memory_region_init_io(command, OBJECT(dev), &lucent_ohci_command_ops,
                          ohci, "lucent-ohci.command-status", 4);
    memory_region_add_subregion_overlap(&ohci->mem, LUCENT_OHCI_COMMAND_STATUS,
                                        command, 1);
    ohci->intr = 0;
}

static void lucent_ohci_reset(DeviceState *d)
{
    LucentOHCIClass *lc = LUCENT_OHCI_GET_CLASS(d);

    lc->parent_reset(d);
    lucent_ohci_core(PCI_DEVICE(d))->intr = 0;
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
