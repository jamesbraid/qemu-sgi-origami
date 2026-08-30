/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Neterion/S2io Xframe 10-Gigabit Ethernet controller.
 *
 * A public third-party PCI add-in card, not SGI silicon -- see
 * docs/datasheets/xframe.md. The SGI 10-Gigabit Ethernet Network Adapter is
 * one instantiation of this generic device, distinguished by PCI-identity
 * properties (subsystem-vendor-id 0x10a9), the same way upstream e1000 and
 * rtl8139 expose vendor/board identity as properties rather than as
 * separate devices.
 *
 * SCAFFOLDING SLICE: the two BAR windows and the netdev frontend exist, but
 * the 64-bit PIF/TxP register file behind them does not -- both windows
 * answer every access with LOG_UNIMP. The real registers carry full-value
 * side effects on a single 64-bit access (the byte-swapper diagnostic,
 * SW_RESET's key, RMAC_CFG_KEY), and the Bridge currently splits an 8-byte
 * device access into two 4-byte cycles before it reaches any device's
 * MemoryRegionOps regardless of what the device declares -- see the Access
 * width note in docs/library/nic-on-bridge-conventions.md. That is under
 * review by the Bridge owner (docs/superpowers/plans/
 * 2026-08-29-xframe-ethernet-adapter.md, Task P3); the register file is the
 * follow-on slice once it settles.
 */

#ifndef HW_NET_XFRAME_H
#define HW_NET_XFRAME_H

#include "hw/pci/pci_device.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_XFRAME "xframe"
OBJECT_DECLARE_SIMPLE_TYPE(XframeState, XFRAME)

/*
 * PCI identity. Vendor ID is common to every Xframe/Xframe II part; device
 * ID picks the ASIC generation (Xena vs Herc) and is a settable property,
 * not a separate device type -- docs/datasheets/xframe.md's "Device
 * identity as configurable properties".
 */
#define XFRAME_PCI_VENDOR_ID            0x17d5  /* Neterion / S2io */
#define XFRAME_PCI_DEVICE_ID_XENA       0x5831  /* Xframe I, SGI Release 1 */
#define XFRAME_PCI_DEVICE_ID_HERC       0x5832  /* Xframe II */

/*
 * The SGI board's own PCI subsystem vendor ID. Its subsystem device ID is
 * not established from any source read for the datasheet and must not be
 * invented -- the subsystem-id property stays at its generic default (0)
 * until a source settles it.
 */
#define XFRAME_PCI_SUBSYSTEM_VENDOR_ID_SGI  0x10a9

/*
 * BAR0: the PIF register window (config offset 0x10, a 64-bit memory BAR).
 * The banked register map spans at least 0x0000-0x33ff
 * (docs/datasheets/xframe.md's "PIF window banks"); the exact decoded size
 * is OPEN there, so this is the smallest power-of-two window that covers
 * the documented span.
 */
#define XFRAME_PIF_WINDOW_SIZE          0x4000

/*
 * The second BAR (config offset 0x18): the TxP transmit descriptor/doorbell
 * window, eight per-FIFO slots at a 0x00020000 stride
 * (docs/datasheets/xframe.md's register map).
 */
#define XFRAME_TXP_WINDOW_SIZE          0x100000

struct XframeState {
    PCIDevice parent_obj;

    /* BAR0: the PIF register window. Stub-only in this slice. */
    MemoryRegion pif_mmio;
    /* The second BAR: the TxP descriptor/doorbell window. Stub-only. */
    MemoryRegion txp_mmio;

    /* PCI-identity properties -- see "PCI identity" above. */
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;

    /* The netdev frontend. No transmit or receive path yet (Tasks 2-3). */
    NICConf conf;
    NICState *nic;
};

#endif
