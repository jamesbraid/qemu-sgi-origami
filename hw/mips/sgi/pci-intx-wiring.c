/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "pci-intx-wiring.h"

uint8_t sgi_pci_intx_levels(const uint8_t map[SGI_PCI_INTX_MAP_SIZE],
                           uint32_t raw)
{
    uint8_t levels = 0;

    for (unsigned device = 1; device <= 2; device++) {
        for (unsigned pin = 0; pin < PCI_NUM_PINS; pin++) {
            if (raw & (1U << (device * PCI_NUM_PINS + pin))) {
                levels |= 1U << map[(device - 1) * PCI_NUM_PINS + pin];
            }
        }
    }
    return levels;
}

void sgi_pci_intx_drive(qemu_irq outputs[SGI_PCI_INTX_INPUTS],
                        uint8_t previous, uint8_t levels)
{
    uint8_t changed = previous ^ levels;

    for (unsigned pin = 0; pin < SGI_PCI_INTX_INPUTS; pin++) {
        if (changed & (1U << pin)) {
            qemu_set_irq(outputs[pin], !!(levels & (1U << pin)));
        }
    }
}
