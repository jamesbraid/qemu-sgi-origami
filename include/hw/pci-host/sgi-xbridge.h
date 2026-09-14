/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_PCI_HOST_SGI_XBRIDGE_H
#define HW_PCI_HOST_SGI_XBRIDGE_H

#include "hw/1wire/onewire.h"
#include "hw/pci/pci_host.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SGI_XBRIDGE_HEAD "sgi-xbridge-head"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXBridgeHeadState, SGI_XBRIDGE_HEAD)

#define TYPE_SGI_XBRIDGE "sgi-xbridge"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXBridgeState, SGI_XBRIDGE)

#define SGI_XBRIDGE_HEAD_PINS 8
#define SGI_XBRIDGE_HEAD_RAW_PCI_IRQS 32
#define SGI_XBRIDGE_HEAD_REG_COUNT (0x300 / 8)
#define SGI_XBRIDGE_HEAD_ATE_COUNT 512

/*
 * A head's "board-part"/"onewire-serial" properties (and the machine's
 * "board-part-e"/"board-part-f" etc. on the chip) place a MicroLAN
 * board-identity record on that head's BRIDGE_NIC register (offset 0xb0).
 * No IP45 IO-8 identity capture exists (docs/datasheets/xbridge.md, "Origin
 * 300 IO-8 population supplement"), so a machine that needs the stock PROM's
 * bus-walk to attach the IO-8's bridge object -- without asserting real
 * hardware bytes it doesn't have -- uses this explicit placeholder pair
 * rather than an Origin 200/2000 part number, which would read as an
 * observed IP45 fact.
 */
#define SGI_XBRIDGE_NIC_PLACEHOLDER_PART   "NOT-OBSERVED"
#define SGI_XBRIDGE_NIC_PLACEHOLDER_SERIAL 0x060504030201ULL

/* The crossbar ports a machine can attach to: 8 to 0xd, the heads excluded. */
#define SGI_XBRIDGE_EXTERNAL_PORTS 6

/*
 * What a machine wires, keyed by crosstalk widget number: 0 is the
 * crossbar as the host's port sees it, 0xe and 0xf are the heads.
 *
 * A widget view covers the widget's whole 16 TB slot, so an access past
 * a head's 16 MB face reaches nothing there too. The views exist from
 * instance init, so a host can link them before either side is realized,
 * and are also SysBus MMIO regions 1 to 3 in that order; region 0 is the
 * whole crosstalk space. A head's PCI bus exists once the chip is realized.
 *
 * GPIO groups, all on the chip:
 *   "head-e-pci-intx", "head-f-pci-intx": 32 outputs each, slot * 4 + pin,
 *       the head's raw INTx levels; slot-to-pin routing is board wiring
 *   "head-e-pin", "head-f-pin": 8 inputs each, the head's interrupt pins
 *   "warm-reset", "warm-reset-out": one line per external port, index
 *       port - 8; the heads' lines are wired inside the chip
 */
MemoryRegion *sgi_xbridge_widget_view(SGIXBridgeState *s, unsigned widget);
PCIBus *sgi_xbridge_pci_bus(SGIXBridgeState *s, unsigned widget);

/* The PCI ranges above the face: two memory aliases and I/O space. */
#define SGI_XBRIDGE_HEAD_RANGES 3

struct SGIXBridgeHeadState {
    PCIHostState parent_obj;
    MemoryRegion widget;
    MemoryRegion face;
    struct XBridgeRange {
        SGIXBridgeHeadState *head;
        AddressSpace *as;
        MemoryRegion mr;
        unsigned width;
        uint32_t swap;
    } range[SGI_XBRIDGE_HEAD_RANGES];
    MemoryRegion undefined;
    MemoryRegion pci_mem;
    MemoryRegion pci_io;
    AddressSpace pci_mem_as;
    AddressSpace pci_io_as;
    uint32_t reg[SGI_XBRIDGE_HEAD_REG_COUNT];
    uint64_t ate[SGI_XBRIDGE_HEAD_ATE_COUNT];
    uint8_t speed;
    uint8_t pins;
    /*
     * The head's own MicroLAN board-identity part, present only for a head
     * whose widget carries a board -- board_part unset (NULL) leaves the
     * bus unbuilt and the register at BRIDGE_NIC inert, matching a head
     * with no board behind it.
     */
    OneWireBus *onewire_bus;
    char *board_part;
    uint64_t onewire_serial;
    qemu_irq pci_irq[SGI_XBRIDGE_HEAD_RAW_PCI_IRQS];
    uint32_t pci_levels;
    /* Rebuilt from the fixed PCI population before migration state loads. */
    uint32_t self_ordered_slots;
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
