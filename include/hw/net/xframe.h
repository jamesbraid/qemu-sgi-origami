/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Neterion/S2io Xframe 10-Gigabit Ethernet controller.
 *
 * A public third-party PCI add-in card, not SGI silicon -- see the xframe
 * datasheet (docs/datasheets/xframe.md in the origami project tree, not
 * this repository -- every later citation of it here means that file).
 * The SGI 10-Gigabit Ethernet Network Adapter is
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

/*
 * The largest buffer a descriptor asking for segmentation offload may hand
 * over. That buffer is not a frame: it is one IP datagram's worth of payload
 * for the card to cut into frames, so it is bounded by the largest datagram a
 * guest can describe (a 16-bit IP total length) plus its Ethernet header, not
 * by the wire's frame limit. IRIX hands this adapter about 32 KB at a time;
 * clamping such a buffer to the frame limit truncates the datagram and puts a
 * malformed remnant on the wire.
 */
#define XFRAME_MAX_TSO_FRAME            (65535 + 14)

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
    uint64_t general_int_mask;  /* GENERAL_INT_MASK: read-back only */
    /*
     * Per-block (second-level) interrupt masks. These gate the actual INTx
     * line; GENERAL_INT_MASK above gates neither the line nor the summary read
     * (see xframe_update_irq and the read handler). tx/rx_traffic_mask and the
     * two MAC masks gate the sources this model raises; pic_int_mask's
     * PIC_INT_GPIO bit gates the GPIO link source (the path IRIX xg uses);
     * gpio_int_mask and the other PIC masks are stored for read-back but gate
     * no modelled source. All reset to all-ones (masked).
     */
    uint64_t pic_int_mask;      /* PIC_INT_MASK: gates the GPIO link source */
    uint64_t txpic_int_mask;    /* TXPIC_INT_MASK (no modelled src) */
    uint64_t rxpic_int_mask;    /* RXPIC_INT_MASK (no modelled src) */
    uint64_t gpio_int_mask;     /* GPIO_INT_MASK: read-back only */
    uint64_t tx_traffic_mask;   /* TX_TRAFFIC_MASK: gates TX_TRAFFIC_INT */
    uint64_t rx_traffic_mask;   /* RX_TRAFFIC_MASK: gates RX_TRAFFIC_INT */
    uint64_t mac_int_mask;      /* MAC_INT_MASK: gates the RMAC block roll-up */
    uint64_t rmac_max_pyld_len; /* RMAC_MAX_PYLD_LEN: largest payload accepted */
    uint64_t mac_rmac_err_mask; /* MAC_RMAC_ERR_MASK: gates link-state-change */
    uint64_t mac_rmac_err;      /* MAC_RMAC_ERR_REG: RMAC link-state-change */
    uint64_t gpio_int_reg;      /* GPIO_INT_REG: PIC/GPIO-block link-state */
    uint64_t mac_cfg;           /* TMAC/RMAC config, key-locked */
    bool rmac_cfg_unlocked;     /* RMAC_CFG_KEY unlocks a MAC_CFG write */
    /*
     * The statistics block: the card DMAs a structure of hardware counters to
     * the physical address in STAT_ADDR while STAT_CFG enables it, and the
     * driver reads its interface counters out of that memory as deltas.
     * Unmodelled, the driver differences against a block that never advances
     * and its transmit packet count runs BACKWARDS (netstat -in showing Opkts
     * of 4294967284 and falling). The four counters below are what the block
     * is filled from; they only ever increase.
     */
    uint64_t stat_cfg;          /* STAT_CFG: enable, one-shot, update period */
    uint64_t stat_addr;         /* STAT_ADDR: where the block is DMA'd */
    uint64_t stat_tx_frms;      /* frames put on the wire */
    uint64_t stat_tx_octets;    /* their bytes */
    uint64_t stat_rx_frms;      /* frames delivered to the guest */
    uint64_t stat_rx_octets;    /* their bytes */
    QEMUTimer *stat_timer;      /* periodic DMA while enabled */

    uint64_t rmac_addr_cmd;     /* MAC-table command/strobe/offset */
    uint64_t rmac_addr_data0;   /* MAC-table data word 0 (the address) */
    uint64_t rmac_addr_data1;   /* MAC-table data word 1 (write-only mask) */
    uint64_t mac_addr[XFRAME_MAC_TABLE_ENTRIES]; /* the 64-entry MAC table */
    /*
     * Per-entry match mask, latched from rmac_addr_data1 on a write strobe
     * -- see xframe_rx_filter for the compare-on-clear-bit semantics.
     */
    uint64_t mac_addr_mask[XFRAME_MAC_TABLE_ENTRIES];

    /*
     * Transmit doorbell: the pending TxDL list pointer per FIFO, written to the
     * TxP window before the control word that launches the list.
     */
    uint64_t txdl_par[XFRAME_TXP_FIFOS];
    uint64_t tx_traffic_int;    /* TX_TRAFFIC_INT: transmit completion status */

    /*
     * Scratch for assembling one outgoing frame from a descriptor list, or the
     * whole datagram when the descriptor asks for segmentation offload.
     * Transient within a doorbell; not migrated.
     */
    uint8_t tx_frame[XFRAME_MAX_TSO_FRAME];

    /*
     * Scratch for one segment cut from tx_frame when the descriptor asks for
     * segmentation offload. Transient within a doorbell; not migrated.
     */
    uint8_t tx_segment[XFRAME_MAX_FRAME];

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

    /*
     * Recovery poll for a ring the guest refills in place (see
     * xframe_rx_recovery_timer_cb in xframe.c). Transient netdev-queue
     * recovery machinery, not device state a migrated-in target needs to
     * reproduce: not part of VMState.
     */
    QEMUTimer *rx_recovery_timer;

    /*
     * The bring-up link edge is presented as a DOWN->UP transition, not a bare
     * unchanging UP: a driver in the Neterion family propagates carrier and
     * wakes transmit only on a link-state CHANGE, so an initial belief of "up"
     * needs a real edge to flip it. Two one-shots sequence that transition off
     * the ADAPTER_EN 0->1 edge (see xframe_link_down_timer_cb /
     * xframe_link_up_timer_cb in xframe.c). link_down_timer fires first, a
     * short time after enable (after the driver's synchronous bring-up unmasks,
     * so it is delivered, not acked away), latching GPIO_INT_REG LINK_DOWN; its
     * callback then arms link_up_timer one interval later to latch LINK_UP
     * (clearing LINK_DOWN) and the MAC-path link-state-change bit. The two are
     * SEPARATE interrupts, never both GPIO bits at once. Both are cancelled on
     * soft reset and on ADAPTER_EN 1->0.
     *
     * Both one-shots are migrated (VMSTATE_TIMER_PTR_V): a save taken between
     * the two edges must still deliver the pending UP on the target -- or, for
     * a save taken between ADAPTER_EN and the DOWN edge itself, still deliver
     * the DOWN -- or a latched GPIO_INT_REG bit (migrated above) would never
     * be cleared and the target would never complete the transition.
     */
    QEMUTimer *link_down_timer;
    QEMUTimer *link_up_timer;

    /*
     * The periodic hardware "scheduled interrupt" the driver programs at
     * bring-up to feed its "no interrupts" watchdog (see xframe_sched_int_cb in
     * xframe.c). A live IRIX xg boot's driver watchdog is a pure liveness timer:
     * it resets the adapter unless an interrupt keeps arriving, and the one-shot
     * bring-up link transition above is not enough to satisfy it. The driver
     * arms a periodic scheduled interrupt (scheduled_int_ctrl at 0x918,
     * SCHED_INT_CTRL_TIMER_EN) and unmasks it through the PIC block (PIC_INT_MASK
     * PIC_INT_TX) and the TXPIC leaf (TXPIC_INT_MASK). It is a traffic- and
     * link-independent source on the PIC/TXPIC path -- it never touches the
     * GPIO/link bits, so it never disturbs carrier. Armed by a SCHED_INT_CTRL
     * write with TIMER_EN set, re-armed each period, cancelled on soft reset,
     * on ADAPTER_EN 1->0, and on a SCHED_INT_CTRL write clearing TIMER_EN. Its
     * pending expiry is migrated (VMSTATE_TIMER_PTR_V); the VMState post_load
     * re-arms it if sched_int_armed is still set on load -- not merely
     * TIMER_EN, which stays set across a fired ONE_SHOT interrupt that must
     * NOT be re-armed (see xframe_post_load and xframe_sched_int_cb).
     */
    uint64_t sched_int_ctrl;    /* scheduled_int_ctrl (0x918): read-back + arm */
    bool sched_int_pending;     /* scheduled interrupt latched, cleared on ack */
    /*
     * Whether an arm is actually outstanding, migrated (VMSTATE_BOOL_V) so
     * post_load can tell a still-armed periodic/pending timer from a ONE_SHOT
     * that already fired and must not be re-armed -- see xframe_sched_int_arm,
     * xframe_sched_int_cb and xframe_post_load in xframe.c.
     */
    bool sched_int_armed;
    QEMUTimer *sched_int_timer;

    /* The netdev frontend. */
    NICConf conf;
    NICState *nic;
    QEMUBH *rx_flush_bh;

    /* Backend replies wait until the transmit completion is published. */
    bool defer_backend_rx;
    bool backend_rx_deferred;
};

#endif
