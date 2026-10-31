/* SPDX-License-Identifier: BSD-3-Clause */
/* Conventional PCI 64-bit data path; see include/hw/pci/pci-64bit.h. */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/pci/pci-64bit.h"

bool pci_64bit_device_reset_done(PCIDevice *dev, bool req64)
{
    PCI64BitDeviceClass *pc = (PCI64BitDeviceClass *)
        object_class_dynamic_cast(object_get_class(OBJECT(dev)),
                                  INTERFACE_PCI_64BIT_DEVICE);

    if (!pc) {
        return false;
    }
    if (pc->req64_sampled) {
        pc->req64_sampled(dev, req64);
    }
    return req64;
}

static const TypeInfo pci_64bit_device_info = {
    .name = INTERFACE_PCI_64BIT_DEVICE,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(PCI64BitDeviceClass),
};

static void pci_64bit_register_types(void)
{
    type_register_static(&pci_64bit_device_info);
}

type_init(pci_64bit_register_types)
