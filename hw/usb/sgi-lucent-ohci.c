/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AT&T/Lucent OHCI USB controller, the two-port part SGI's I/O boards
 * carry. It is a plain pci-ohci with fixed PCI identifiers; the two
 * behavioural differences are two root-hub ports rather than the pci-ohci
 * default of three, set through the inherited num-ports qdev property, and
 * a fixed medium DEVSEL timing in the PCI Status word, set after the
 * shared OHCI wrapper realizes.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/qdev-core.h"
#include "qom/object.h"

#define TYPE_LUCENT_OHCI "lucent-ohci"

typedef struct LucentOHCIClass {
    PCIDeviceClass parent_class;
    void (*parent_realize)(PCIDevice *dev, Error **errp);
} LucentOHCIClass;

DECLARE_CLASS_CHECKERS(LucentOHCIClass, LUCENT_OHCI, TYPE_LUCENT_OHCI)

static void lucent_ohci_instance_init(Object *obj)
{
    qdev_prop_set_uint32(DEVICE(obj), "num-ports", 2);
}

static void lucent_ohci_realize(PCIDevice *dev, Error **errp)
{
    LucentOHCIClass *lc = LUCENT_OHCI_GET_CLASS(dev);
    Error *err = NULL;

    lc->parent_realize(dev, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    pci_set_word(dev->config + PCI_STATUS, PCI_STATUS_DEVSEL_MEDIUM);
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
