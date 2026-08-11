/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Bridge, the crosstalk-to-PCI host bridge.
 *
 * One side of the chip is an ordinary crosstalk widget with the common widget
 * register block at the bottom of its 16 MB of crosstalk address space. The
 * other side is a PCI host bridge with eight device slots, an eight-pin
 * interrupt controller and a translator for transfers a device initiates.
 *
 * On an Origin 200 this is the only path from the crossbar to any peripheral,
 * so nothing a guest does past its first console banner avoids it.
 */

#ifndef HW_PCI_HOST_SGI_BRIDGE_H
#define HW_PCI_HOST_SGI_BRIDGE_H

#include "qemu/units.h"
#include "hw/pci/pci_host.h"
#include "qom/object.h"

#define TYPE_SGI_BRIDGE "sgi-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBridgeState, SGI_BRIDGE)

/* The whole of the widget's crosstalk space belongs to this chip. */
#define SGI_BRIDGE_WIDGET_SIZE  (16 * MiB)

/*
 * The local register block. The chip decodes 64 KB there and defines the first
 * 0x300 bytes of it; every register in that range is 32 bits or narrower and
 * sits in data bits 31:0 of an eight-byte slot, so the block holds 96 slots.
 */
#define SGI_BRIDGE_REGS_SIZE    (64 * KiB)
#define SGI_BRIDGE_REG_SLOT     8
#define SGI_BRIDGE_REGS_DEFINED 0x300
#define SGI_BRIDGE_REG_SLOTS    (SGI_BRIDGE_REGS_DEFINED / SGI_BRIDGE_REG_SLOT)

/*
 * The eight PCI device slots, which are also the eight interrupt pins. A slot's
 * INTA is wired to the pin of the same number, which is the rule OpenBSD relies
 * on and which two SCSI controllers on a real Origin 200 confirm: the device at
 * PCI 0 takes interrupt 0 and the device at PCI 1 takes interrupt 1.
 */
#define SGI_BRIDGE_DEVICES      8
#define SGI_BRIDGE_PINS         8

struct SGIBridgeState {
    PCIHostState parent_obj;

    /*
     * The widget's whole crosstalk space, and the local register block inside
     * it. The chip claims the space whether or not it decodes an address in it,
     * because claiming it is what lets the chip answer for an address it has
     * nothing at rather than leaving the answer to whatever is underneath.
     */
    MemoryRegion widget;
    MemoryRegion regs;

    /*
     * The window a processor reads PCI configuration space through. Type 0
     * only: there is no PCI-to-PCI bridge on this machine, and Linux's own
     * driver says outright that it uses type 0 accesses only.
     */
    MemoryRegion cfg0;

    /* The PCI side's two address spaces, which the device windows look into. */
    MemoryRegion pci_mmio;
    MemoryRegion pci_io;

    /*
     * A window per PCI slot, through which a processor reaches whatever is in
     * that slot. Each slot has two of them, aimed at the same place in the
     * widget's space and looking into the two PCI address spaces, with exactly
     * one enabled: a window selects memory or I/O space by a bit in the slot's
     * own register, and a memory region cannot be re-pointed at a different
     * space once it exists.
     */
    MemoryRegion device_window_mem[SGI_BRIDGE_DEVICES];
    MemoryRegion device_window_io[SGI_BRIDGE_DEVICES];

    /*
     * Every defined register in the local block, one 32-bit word per eight-byte
     * slot, indexed by the slot number. A slot the chip does not define holds
     * nothing and is never read: which slots exist, what each resets to and
     * which of its bits a guest may change all come from one table in the
     * implementation, so a register cannot be given a decode without also being
     * given a reset value.
     */
    uint32_t reg[SGI_BRIDGE_REG_SLOTS];

    /*
     * The level each of the eight interrupt pins is being driven to by whatever
     * is in that slot. This is the state the interrupt status register reports,
     * and it is kept as the devices' levels rather than as a latched word
     * because the status register must follow the level and must never latch:
     * the guest re-arms in software by re-reading that register, so a bit that
     * cleared on service would tell it a device had gone quiet when it had not.
     */
    uint8_t pin_level;

    /* What the identification register answers with. */
    uint32_t revision;
    uint32_t part;
    uint32_t manufacturer;
};

#endif /* HW_PCI_HOST_SGI_BRIDGE_H */
