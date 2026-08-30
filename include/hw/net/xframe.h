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
 * This models the generic PCI card's own machine-independent contract. The
 * PIF register window (BAR0) carries the probe/attach register file -- the
 * byte-swapper diagnostic, SW_RESET, the RMAC_CFG_KEY MAC-config lock,
 * ADAPTER_STATUS/ADAPTER_CONTROL and the MAC-address table -- as 64-bit
 * registers in native little-endian order. The transmit descriptor/doorbell
 * path behind the TxP window (BAR2) is modelled, and so is the receive path:
 * the RxDMA registers (also in BAR0) plus the RxD descriptor ring the card
 * walks in guest memory. A host bridge that corrects register byte order or
 * access width (the Origin 200 Bridge) is that bridge's concern, not this
 * device's -- see docs/library/nic-on-bridge-conventions.md.
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

/*
 * The station/multicast MAC-address table has 64 entries, addressed by the
 * 6-bit offset field of RMAC_ADDR_CMD_MEM. Entry 0 holds the factory MAC read
 * back at attach (docs/datasheets/xframe.md's "Behavioral contract").
 */
#define XFRAME_MAC_TABLE_ENTRIES        64

/*
 * The TxP window is eight per-FIFO transmit doorbells at a 0x20000 stride
 * (docs/datasheets/xframe.md's register map).
 */
#define XFRAME_TXP_FIFOS                8

/*
 * The largest frame the transmit path assembles from a descriptor list -- the
 * card's 9600-byte jumbo limit (docs/datasheets/xframe.md's "Role"). A
 * guest-supplied length beyond this is clamped rather than copied unbounded.
 */
#define XFRAME_MAX_FRAME                9600

struct XframeState {
    PCIDevice parent_obj;

    /* BAR0: the PIF register window (the probe/attach register file). */
    MemoryRegion pif_mmio;
    /* The second BAR: the TxP transmit descriptor/doorbell window. */
    MemoryRegion txp_mmio;

    /* PCI-identity properties -- see "PCI identity" above. */
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;

    /*
     * PIF register state, all 64-bit and in native little-endian order --
     * docs/datasheets/xframe.md's "Register map" and "VMState / migration
     * fields".
     */
    uint64_t swapper_ctrl;      /* byte-swap control (stored; not scrambled) */
    uint64_t xmsi_address;      /* MSI address / byte-swap scratch */
    uint64_t sw_reset;          /* reset-lane control (key is write-only) */
    uint64_t adapter_control;   /* ADAPTER_EN, LED, EOI, wait-int, ECC */
    uint64_t mac_cfg;           /* TMAC/RMAC config, key-locked */
    bool rmac_cfg_unlocked;     /* RMAC_CFG_KEY unlocks a MAC_CFG write */
    uint64_t rmac_addr_cmd;     /* MAC-table command/strobe/offset */
    uint64_t rmac_addr_data0;   /* MAC-table data word 0 (the address) */
    uint64_t rmac_addr_data1;   /* MAC-table data word 1 (write-only mask) */
    uint64_t mac_addr[XFRAME_MAC_TABLE_ENTRIES]; /* the 64-entry MAC table */

    /*
     * Transmit doorbell: the pending TxDL list pointer per FIFO, written to the
     * TxP window before the control word that launches the list.
     */
    uint64_t txdl_par[XFRAME_TXP_FIFOS];

    /*
     * Scratch for assembling one outgoing frame from a descriptor list.
     * Transient within a doorbell; not migrated.
     */
    uint8_t tx_frame[XFRAME_MAX_FRAME];

    /*
     * RxDMA ring state -- docs/datasheets/xframe.md's RxDMA register bank and
     * "Behavioral contract" Receive note. Only ring 0 is modelled: both
     * public drivers use exactly one of the chip's eight receive rings
     * (OpenBSD if_xge.c's own comment on PRC_RXD0_0: "PRC_RXD0_[1-7] are not
     * used").
     */
    uint64_t prc_rxd0_0;        /* PRC_RXD0_0: ring 0's descriptor-page base */
    uint64_t prc_ctrl_0;        /* PRC_CTRL_0: ring 0 control (RC_IN_SVC...) */
    uint64_t rx_traffic_int;    /* RX_TRAFFIC_INT: receive completion status */

    /*
     * The card's own walk position through the RxD ring: not itself a
     * guest-visible register, mirroring how real hardware tracks its current
     * descriptor internally. Reset to the ring base whenever the guest
     * (re)writes it or (re)arms RC_IN_SVC.
     */
    uint64_t rx_page_addr;
    uint32_t rx_desc_idx;

    /* The netdev frontend. */
    NICConf conf;
    NICState *nic;
};

#endif
