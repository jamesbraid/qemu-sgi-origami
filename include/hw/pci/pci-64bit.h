/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Conventional PCI 64-bit data path.
 *
 * On a real bus the data-phase width is settled per transaction: a slot
 * wired to the 64-bit data path drives REQ64# during reset, a 64-bit agent
 * that sampled it may request 64-bit transfers, and the target answers with
 * ACK64# (PCI Local Bus Specification 2.1, sections 3.10 and 4.3.2). QEMU
 * models no data-phase width and MemTxAttrs carries none. Both ends are
 * fixed once reset ends, so a slot owner that needs the width settles it
 * per slot at the end of reset and caches it, standing in for the
 * per-transaction handshake, much as pcie_sync_bridge_lnk() settles a
 * negotiated PCIe link width from the device behind a bridge.
 */
#ifndef HW_PCI_PCI_64BIT_H
#define HW_PCI_PCI_64BIT_H

#include "hw/pci/pci_device.h"

/*
 * Implemented by conventional PCI devices that run 64-bit data phases when
 * their slot drives REQ64# at reset.
 */
#define INTERFACE_PCI_64BIT_DEVICE "pci-64bit-device"

typedef struct PCI64BitDeviceClass PCI64BitDeviceClass;
DECLARE_CLASS_CHECKERS(PCI64BitDeviceClass, PCI_64BIT_DEVICE,
                       INTERFACE_PCI_64BIT_DEVICE)

struct PCI64BitDeviceClass {
    InterfaceClass parent_class;

    /*
     * Optional: told at the end of every reset whether REQ64# was asserted,
     * so a card can show what it sampled in its own status register.
     */
    void (*req64_sampled)(PCIDevice *dev, bool asserted);
};

/*
 * For the slot owner at the end of reset: notify the device of REQ64# and
 * return whether it now runs 64-bit data phases, which is true only for a
 * device implementing the interface in a slot that drives REQ64#.
 */
bool pci_64bit_device_reset_done(PCIDevice *dev, bool req64);

#endif
