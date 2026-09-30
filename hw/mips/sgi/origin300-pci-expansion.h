/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SGI_ORIGIN300_PCI_EXPANSION_H
#define HW_MIPS_SGI_ORIGIN300_PCI_EXPANSION_H

#include "hw/pci/pci.h"
#include "qom/object.h"

#define TYPE_SGI_ORIGIN300_PCI_EXPANSION "sgi-origin300-pci-expansion-module"
OBJECT_DECLARE_SIMPLE_TYPE(SGIOrigin300PciExpansionState,
                           SGI_ORIGIN300_PCI_EXPANSION)

#define SGI_ORIGIN300_PCI_HEADS 6

/* ASIC/head order: U0 E/F, U1 E/F, U2 E/F; chassis slot mapping is unknown. */
PCIBus *sgi_origin300_pci_expansion_bus(SGIOrigin300PciExpansionState *s,
                                       unsigned head);

#endif
