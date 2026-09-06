/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_PCI_HOST_SGI_XBRIDGE_H
#define HW_PCI_HOST_SGI_XBRIDGE_H

#include "hw/pci/pci_host.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SGI_XBRIDGE_HEAD "sgi-xbridge-head"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXBridgeHeadState, SGI_XBRIDGE_HEAD)

#define TYPE_SGI_XBRIDGE "sgi-xbridge"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXBridgeState, SGI_XBRIDGE)

#define SGI_XBRIDGE_HEAD_RAW_PCI_IRQS 32
#define SGI_XBRIDGE_HEAD_REG_COUNT (0x300 / 8)
#define SGI_XBRIDGE_HEAD_ATE_COUNT 512

struct SGIXBridgeHeadState {
    PCIHostState parent_obj;
    MemoryRegion widget;
    MemoryRegion pci_mem;
    MemoryRegion pci_io;
    AddressSpace pci_mem_as;
    AddressSpace pci_io_as;
    uint32_t reg[SGI_XBRIDGE_HEAD_REG_COUNT];
    uint64_t ate[SGI_XBRIDGE_HEAD_ATE_COUNT];
    uint8_t speed;
    uint8_t pins;
    qemu_irq pci_irq[SGI_XBRIDGE_HEAD_RAW_PCI_IRQS];
    uint32_t pci_levels;
    MemoryRegion *target[16];
    struct XBridgeTarget {
        AddressSpace as;
        MemoryRegion swap;
        AddressSpace swap_as;
    } endpoint[16];
    struct XBridgeDMA {
        IOMMUMemoryRegion iommu;
        AddressSpace as;
        SGIXBridgeHeadState *head;
        unsigned slot;
    } dma[8];
    MemoryRegion denied;
    AddressSpace denied_as;
    MemoryRegion flush;
    AddressSpace flush_as;
};


#endif
