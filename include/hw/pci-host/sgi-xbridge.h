/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SGI XBridge register and PCI head definitions. */
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
/*
 * Revision B's table: 1024 entries of storage, of which the page mapper uses
 * the first 512.
 */
#define SGI_XBRIDGE_HEAD_ATE_COUNT 1024
#define SGI_XBRIDGE_HEAD_ATE_MAPPED 512

/*
 * A head's "board-part"/"onewire-serial" properties (and the machine's
 * "board-part-e"/"board-part-f" etc. on the chip) place a MicroLAN
 * board-identity record on that head's BRIDGE_NIC register (offset 0xb0). No
 * real IP45 IO-8 identity bytes are known, so a machine that needs the stock
 * PROM's bus-walk to attach the IO-8's bridge object uses this explicit
 * placeholder pair. An Origin 200/2000 part number would read as an
 * observed IP45 fact; the placeholder claims no real hardware bytes.
 */
#define SGI_XBRIDGE_NIC_PLACEHOLDER_PART   "NOT-OBSERVED"
#define SGI_XBRIDGE_NIC_PLACEHOLDER_SERIAL 0x060504030201ULL

/*
 * The crossbar ports a machine can attach to: 8 to 0xd, the heads excluded.
 */
#define SGI_XBRIDGE_EXTERNAL_PORTS 6

/*
 * Which of those six the model lets a widget be attached to, as the
 * "xio-port" property's default. No source binds the Origin 300's rear
 * XIO connector to a port number: its own manual records the connector
 * and the XBridge specification records the six ports, and nothing read
 * so far joins the two. 0xb is the
 * lowest external port left once the Bedrock takes 0xa, chosen for that
 * and nothing else. A machine sets the property from its own topology,
 * so an observed number replaces this in one place.
 */
#define SGI_XBRIDGE_XIO_PORT_DEFAULT 0xb

/*
 * Put a widget on the chip's external XIO port. Until this is
 * called the port reports no widget present and its link not alive, and
 * a processor access to that widget number reaches nothing -- what an
 * empty connector answers. Afterwards the widget answers processor
 * traffic at its own widget position inside the chip's XIO space,
 * a peer's packet addressed to that widget reaches packet_face, and the
 * crossbar reports the link populated and alive, which is what firmware
 * walks before it addresses a port at all.
 *
 * cpu_face and packet_face are the widget's own two faces
 * (hw/mips/sgi-xio-port.h); a leaf widget passes the same region
 * twice. Callable before or after the chip realizes: what the two faces
 * are installed in exists from instance init, so a board that only
 * arrives once the machine is built attaches the same way an onboard
 * one does.
 */
bool sgi_xbridge_attach_xio(SGIXBridgeState *s, MemoryRegion *cpu_face,
                            MemoryRegion *packet_face, Error **errp);

/*
 * Where an attached widget's own outbound packets go: the chip's link
 * to its host, the same region both bridge heads send their interrupts
 * and their cards' DMA through. NULL before the chip realizes.
 */
MemoryRegion *sgi_xbridge_xio_host_face(SGIXBridgeState *s);

/* Machine-only projection for the experimental two-chip V-brick profile. */
void sgi_xbridge_project_vbrick_pio(SGIXBridgeState *s,
                                    MemoryRegion *v12_view,
                                    MemoryRegion *crossbar_view,
                                    uint64_t half_size);

/* The widget number the external port answers on. */
unsigned int sgi_xbridge_xio_port(SGIXBridgeState *s);

/*
 * What a machine wires, keyed by XIO widget number: 0 is the
 * crossbar as the host's port sees it, 0xe and 0xf are the heads, and
 * "xio-port" is the one external port a widget can be attached to.
 *
 * A widget view covers the widget's whole 16 TB slot, so an access past
 * a head's 16 MB face reaches nothing there too. The views for 0, 0xe and
 * 0xf exist from instance init, so a host can link them before either
 * side is realized, and are also SysBus MMIO regions 1 to 3 in that
 * order; region 0 is the whole XIO space. The external port's view
 * is region 4 and exists once the chip is realized, because which widget
 * it covers is a property. A head's PCI bus exists once the chip is
 * realized.
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
     * whose widget carries a board. With board_part unset (NULL), the bus
     * unbuilt and the register at BRIDGE_NIC inert, matching a head with no
     * board behind it.
     */
    OneWireBus *onewire_bus;
    char *board_part;
    /* The root PCI bus's name; unset names it after the head. */
    char *bus_name;
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
