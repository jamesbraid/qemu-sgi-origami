/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Fallback for the SGI Bridge plain DMA route.
 * BCM570x is built on targets without Bridge; it keeps ordinary bus-master DMA.
 */

#include "qemu/osdep.h"
#include "hw/pci-host/sgi-bridge.h"

AddressSpace *sgi_bridge_plain_dma_as(PCIDevice *dev)
{
    return NULL;
}
