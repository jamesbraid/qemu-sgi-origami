/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MIPS_SGI_PCI_INTX_WIRING_H
#define HW_MIPS_SGI_PCI_INTX_WIRING_H

#include "hw/core/irq.h"
#include "hw/pci/pci.h"

#define SGI_PCI_INTX_MAP_SIZE (2 * PCI_NUM_PINS)
#define SGI_PCI_INTX_INPUTS 8

/* Explicit board map for device indexes 1/2, pins A/B/C/D in order. */
uint8_t sgi_pci_intx_levels(const uint8_t map[SGI_PCI_INTX_MAP_SIZE],
                           uint32_t raw);
void sgi_pci_intx_drive(qemu_irq outputs[SGI_PCI_INTX_INPUTS],
                        uint8_t previous, uint8_t levels);

#endif
