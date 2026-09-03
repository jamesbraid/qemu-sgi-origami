/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Stand-in for the SGI Bridge's plain DMA route on targets without the
 * Bridge. A device that asks (the BCM570x is built wherever PCI is) learns
 * there is no Bridge to skip and keeps its ordinary bus-master path.
 */

#include "qemu/osdep.h"
#include "hw/pci-host/sgi-bridge.h"

AddressSpace *sgi_bridge_plain_dma_as(PCIDevice *dev)
{
    return NULL;
}
