/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "hw/misc/sgi-dma-buffer.h"
#include "hw/pci/pci_device.h"

static bool sgi_dma_buffer_pci_enabled(PCIDevice *dev)
{
    return dev->is_master;
}

void sgi_dma_buffer_init_pci(SGIDMABufferContext *ctx, PCIDevice *dev)
{
    AddressSpace *selected = pci_device_iommu_address_space(dev);

    ctx->as = pci_get_address_space(dev);
    ctx->pci_dev = dev;
    ctx->enabled = sgi_dma_buffer_pci_enabled;
    ctx->provider = sgi_dma_buffer_provider(selected->root);
}
