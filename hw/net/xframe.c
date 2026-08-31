/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Neterion/S2io Xframe 10-Gigabit Ethernet controller.
 *
 * See include/hw/net/xframe.h for what this device is. This models the
 * generic PCI card's own contract: PCI identity, the two BAR windows, the
 * netdev frontend, and the PIF (BAR0) register file the probe/attach path
 * binds on -- the byte-swapper diagnostic, SW_RESET, the RMAC_CFG_KEY
 * MAC-config lock, ADAPTER_STATUS/ADAPTER_CONTROL and the MAC-address table.
 * The registers are 64-bit and native little-endian; a host bridge that
 * corrects register byte order or access width (the Origin 200 Bridge) is that
 * bridge's concern, not this device's. The transmit descriptor/doorbell path
 * behind the TxP window (BAR2) is modelled: a doorbell walks the FIFO's
 * descriptor list, gathers the buffers, and emits the frame. The receive path
 * behind the RxDMA registers (also BAR0) is modelled too: the card walks its
 * posted RxD ring, deposits an accepted frame into the guest's buffer, and
 * raises the receive interrupt.
 *
 * QOM/PCI-realize/BAR shape and the ResettablePhases reset wiring follow
 * hw/scsi/qlogic-isp.c, the fork's PCI-endpoint reference -- not
 * hw/pci-host/astro.c, which is a PCI host bridge with a different
 * TypeInfo/realize/BAR shape and which calls the forbidden
 * device_class_set_legacy_reset(). The netdev frontend follows
 * hw/net/sgi-ioc3-eth.c and upstream hw/net/dp8393x.c.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/net/xframe.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "qapi/error.h"

/*
 * PIF register offsets within BAR0, and the bit fields the probe/attach path
 * touches. Public OpenBSD if_xgereg.h / Linux s2io-regs.h names, taken from
 * the register map of the xframe datasheet (docs/datasheets/xframe.md in
 * the origami project tree, not this repository -- every later citation of
 * it here means that file). All registers are 64-bit.
 */
#define XFRAME_GENERAL_INT_STATUS   0x0000
#define XFRAME_GENERAL_INT_MASK     0x0008
#define XFRAME_SW_RESET             0x0100
#define XFRAME_ADAPTER_STATUS       0x0108
#define XFRAME_ADAPTER_CONTROL      0x0110
#define XFRAME_RX_TRAFFIC_INT       0x08f0
#define XFRAME_SWAPPER_CTRL         0x0908
#define XFRAME_PIF_RD_SWAPPER_FB    0x0910
#define XFRAME_XMSI_ADDRESS         0x0960
#define XFRAME_PRC_RXD0_0           0x1930
#define XFRAME_PRC_CTRL_0           0x1970
#define XFRAME_MAC_CFG              0x2100
#define XFRAME_RMAC_CFG_KEY         0x2120
#define XFRAME_RMAC_ADDR_CMD_MEM    0x2128
#define XFRAME_RMAC_ADDR_DATA0_MEM  0x2130
#define XFRAME_RMAC_ADDR_DATA1_MEM  0x2138

/*
 * The byte-swapper diagnostic returns this fixed constant at a correctly
 * ordered 64-bit read; the driver reads it back to confirm endianness. The
 * model returns it unconditionally, since it presents registers in native
 * order and never scrambles a read.
 */
#define XFRAME_SWAPPER_MAGIC        0x0123456789abcdefULL

/*
 * GENERAL_INT_STATUS's receive-traffic aggregate bit (Linux s2io-regs.h
 * GEN_INTR_RXTRAFFIC = s2BIT(40); s2io.h's s2BIT(loc) is
 * 0x8000000000000000ULL >> loc, i.e. bit 63-loc -- the same MSB-first
 * numbering already cross-checked against OpenBSD's ADAPTER_EN for
 * ADAPTER_CONTROL). GENERAL_INT_STATUS itself is not modelled as stored
 * state: it is derived from the same per-block status this device already
 * keeps (currently just RX_TRAFFIC_INT), so it cannot drift out of sync
 * with the register that actually clears on acknowledgement.
 */
#define XFRAME_GEN_INTR_RXTRAFFIC   (1ULL << 23)

/*
 * GENERAL_INT_MASK's reset value is marked OPEN in the datasheet
 * ("OPEN (masked)"): the drivers never assert what they find at power-on,
 * only that they must unmask before interrupts do anything. Both drivers
 * mask by writing all-ones (Linux s2io.h's DISABLE_ALL_INTRS /
 * S2IO_MINUS_ONE, OpenBSD implicitly via the same all-ones convention on
 * the sibling traffic-interrupt registers), so an all-ones reset -- fully
 * masked until a driver unmasks it -- is the concrete bit pattern the
 * public sources actually use for "masked", not an invented default.
 */
#define XFRAME_GENERAL_INT_MASK_RESET  0xffffffffffffffffULL

/*
 * ADAPTER_STATUS quiescent/ready value the driver's single non-retrying attach
 * read requires (if_xgereg.h QUIESCENT): TDMA/RDMA/PFC ready, TMAC buffer
 * empty, PIC quiescent, memory-controller DRAM+queues ready, both PLLs locked
 * -- and the fault bits clear. The model is always ready, so it presents this
 * immediately.
 */
#define XFRAME_ADAPTER_STATUS_QUIESCENT  0xf40000c300000000ULL

/*
 * SW_RESET: writing the 0xa5a5a5 key in bits 63:40 (OpenBSD 0xa5a5a5<<40,
 * Linux SW_RESET_ALL) triggers a soft reset. The key is a write-only trigger
 * and does not persist; the readable content is the reset-lane control bits
 * below it.
 */
#define XFRAME_SW_RESET_KEY         0xa5a5a5ULL

/* RMAC_CFG_KEY: 0x4c0d in bits 63:48 unlocks the next MAC_CFG write. */
#define XFRAME_RMAC_CFG_KEY_VALUE   0x4c0dULL

/*
 * RMAC_ADDR_CMD_MEM: write-enable selects write vs read, the strobe is
 * self-clearing on completion, and the 6-bit offset selects a table entry.
 */
#define XFRAME_RMAC_ADDR_CMD_WE       (1ULL << 56)
#define XFRAME_RMAC_ADDR_CMD_STROBE   (1ULL << 48)
#define XFRAME_RMAC_ADDR_CMD_OFF_MASK 0x3fULL

/*
 * PRC_CTRL_0: RC_IN_SVC arms receive ring 0, and bits 49:48 pick the
 * descriptor buffer-count mode. Only 1-buffer mode (the value both public
 * drivers use) is modelled.
 */
#define XFRAME_RC_IN_SVC                 (1ULL << 56)
#define XFRAME_PRC_CTRL_RING_MODE_MASK   (3ULL << 48)

/*
 * Pack the netdev's configured MAC into MAC-table entry 0 the way the factory
 * programs it: the six bytes in RMAC_ADDR_DATA0_MEM bits 63:16, most
 * significant byte first (the order both drivers unpack it in).
 */
static void xframe_load_factory_mac(XframeState *s)
{
    uint64_t mac = 0;
    size_t i;

    for (i = 0; i < sizeof(s->conf.macaddr.a); i++) {
        mac |= (uint64_t)s->conf.macaddr.a[i] << (56 - 8 * i);
    }
    s->mac_addr[0] = mac;
}

/*
 * Recompute the INTx line from the device's one interrupt source modelled so
 * far: receive completion, gated by GENERAL_INT_MASK the way both drivers
 * gate it. Linux's s2io_isr masks GENERAL_INT_MASK to all-ones on entry and
 * restores the driver's own mask on exit, once per interrupt serviced;
 * OpenBSD's xge_init instead unmasks once at bring-up
 * ("PIF_WCSR(GENERAL_INT_MASK, 0);") and its xge_intr never touches the mask
 * again. Either way it is a real, driver-written mask, not a register the
 * silicon carries but no driver programs. A transmit-completion source, if
 * one is ever added, would OR in here too.
 */
static void xframe_update_irq(XframeState *s)
{
    bool pending = s->rx_traffic_int != 0 &&
                  !(s->general_int_mask & XFRAME_GEN_INTR_RXTRAFFIC);

    pci_set_irq(PCI_DEVICE(s), pending);
}

/*
 * Return the register file to its reset values. Shared by cold reset and the
 * SW_RESET key path. It touches only device register state, never PCI config
 * or BAR state: real hardware's SW_RESET can perturb config space, but this
 * model must leave that to the PCI core (docs/datasheets/xframe.md's VMState
 * note).
 */
static void xframe_soft_reset(XframeState *s)
{
    s->swapper_ctrl = 0;
    s->xmsi_address = 0;
    s->sw_reset = 0;
    s->adapter_control = 0;
    s->mac_cfg = 0;
    s->rmac_cfg_unlocked = false;
    s->rmac_addr_cmd = 0;
    s->rmac_addr_data0 = 0;
    s->rmac_addr_data1 = 0;
    memset(s->mac_addr, 0, sizeof(s->mac_addr));
    memset(s->mac_addr_mask, 0, sizeof(s->mac_addr_mask));
    memset(s->txdl_par, 0, sizeof(s->txdl_par));
    s->prc_rxd0_0 = 0;
    s->prc_ctrl_0 = 0;
    s->rx_traffic_int = 0;
    s->rx_page_addr = 0;
    s->rx_desc_idx = 0;
    s->general_int_mask = XFRAME_GENERAL_INT_MASK_RESET;
    /*
     * Reset tears the ring down, so a recovery poll left running from before
     * has nothing left to recover -- stop it rather than let it keep firing
     * against a ring that no longer exists in any guest-meaningful sense.
     */
    timer_del(s->rx_recovery_timer);
    xframe_load_factory_mac(s);
    xframe_update_irq(s);
}

/*
 * The MAC-address table's indirect command: a strobe carries out a read or a
 * write of one entry and then self-clears, so a driver polling the strobe bit
 * sees the operation complete at once. A write also latches
 * RMAC_ADDR_DATA1_MEM into the entry's mask -- see xframe_rx_filter for what
 * the mask means. DATA1 is write-only in both drivers (never part of any
 * read-back), so a read strobe restores only DATA0.
 */
static void xframe_rmac_addr_cmd(XframeState *s, uint64_t val)
{
    unsigned offset = (val >> 32) & XFRAME_RMAC_ADDR_CMD_OFF_MASK;

    if (val & XFRAME_RMAC_ADDR_CMD_STROBE) {
        if (val & XFRAME_RMAC_ADDR_CMD_WE) {
            s->mac_addr[offset] = s->rmac_addr_data0;
            s->mac_addr_mask[offset] = s->rmac_addr_data1;
        } else {
            s->rmac_addr_data0 = s->mac_addr[offset];
        }
        val &= ~XFRAME_RMAC_ADDR_CMD_STROBE;
    }
    s->rmac_addr_cmd = val;
}

/*
 * The PIF register window (BAR0). The registers are 64-bit and
 * DEVICE_LITTLE_ENDIAN -- little-endian-native parts, delivered in native
 * order, a host bridge composing whatever byte order the guest needs.
 *
 * Real drivers access these 64-bit registers with 32-bit cycles: IRIX xg reads
 * the byte-swapper diagnostic 32-bit during its endianness handshake, and Linux
 * s2io on the Xena part writes MAC_CFG as two 32-bit halves. Behind the Origin
 * 200 Bridge a slot left with DEV_64BIT_DEVICE clear -- as the live IRIX boot
 * leaves it -- has every 64-bit access split into two 32-bit device cycles. So
 * xframe_pif_ops declares valid/impl.min_access_size = 4: the window serves a
 * register's low 32 bits at off+0 and its high 32 bits at off+4.
 *
 * impl.min = 4 (not 8) is deliberate. QEMU's memory core does not read-modify-
 * write a sub-impl access: access_with_adjusted_size() hands the handler the
 * unaligned guest address with the impl size and zero-extends a narrow write
 * (system/memory.c). With impl.min = 8 a 32-bit high-half read would reach the
 * handler at off+4 (undecoded) and a 32-bit low-half write would clobber the
 * upper 32 bits to zero. Dispatching per size in the handler is what serves a
 * 64-bit register through 32-bit halves correctly. A read at any width comes
 * through xframe_pif_read; a write through xframe_pif_write, which composes the
 * accessed half onto the current value and fires a register's side effect only
 * when the write covers the half its trigger sits in (all triggers here -- the
 * SW_RESET key in bits 63:40, RMAC_CFG_KEY in 63:48, the MAC-table strobe in
 * bit 48 -- live in the high half), so a two-halves write fires it once.
 */
static uint64_t xframe_pif_read_reg(XframeState *s, hwaddr addr)
{
    switch (addr) {
    case XFRAME_ADAPTER_STATUS:
        return XFRAME_ADAPTER_STATUS_QUIESCENT;
    case XFRAME_PIF_RD_SWAPPER_FB:
        return XFRAME_SWAPPER_MAGIC;
    case XFRAME_SW_RESET:
        return s->sw_reset;
    case XFRAME_ADAPTER_CONTROL:
        return s->adapter_control;
    case XFRAME_SWAPPER_CTRL:
        return s->swapper_ctrl;
    case XFRAME_XMSI_ADDRESS:
        return s->xmsi_address;
    case XFRAME_MAC_CFG:
        return s->mac_cfg;
    case XFRAME_RMAC_CFG_KEY:
        return 0;  /* write-only unlock key */
    case XFRAME_RMAC_ADDR_CMD_MEM:
        return s->rmac_addr_cmd;
    case XFRAME_RMAC_ADDR_DATA0_MEM:
        return s->rmac_addr_data0;
    case XFRAME_RMAC_ADDR_DATA1_MEM:
        return s->rmac_addr_data1;
    case XFRAME_RX_TRAFFIC_INT:
        return s->rx_traffic_int;
    case XFRAME_PRC_RXD0_0:
        return s->prc_rxd0_0;
    case XFRAME_PRC_CTRL_0:
        return s->prc_ctrl_0;
    case XFRAME_GENERAL_INT_STATUS:
        /*
         * The top-level aggregate both drivers read before touching any
         * per-block register (OpenBSD xge_intr, Linux s2io_isr: "reason =
         * readq(&bar0->general_int_status)"). Derived from the per-block
         * state this device keeps rather than stored on its own, so it can
         * never disagree with RX_TRAFFIC_INT about whether receive traffic
         * is pending.
         */
        return s->rx_traffic_int != 0 ? XFRAME_GEN_INTR_RXTRAFFIC : 0;
    case XFRAME_GENERAL_INT_MASK:
        return s->general_int_mask;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "xframe: PIF read at 0x%" PRIx64 " is not modelled\n",
                      addr);
        return 0;
    }
}

/*
 * Serve a register read at whatever width the memory core delivers. A 32-bit
 * read takes the register's low half at off+0 or -- off+4, the odd word of the
 * 8-aligned register -- its high half. A 64-bit read returns the whole value;
 * the core masks a 32-bit low-half read down to the low word for us.
 */
static uint64_t xframe_pif_read(void *opaque, hwaddr addr, unsigned size)
{
    XframeState *s = opaque;
    uint64_t val = xframe_pif_read_reg(s, addr & ~7ULL);

    if (size == 4 && (addr & 4)) {
        val >>= 32;
    }
    return val;
}

/*
 * Apply a write to the register at addr (8-aligned). full is the whole 64-bit
 * value the register takes -- the accessed half composed onto its current
 * content by xframe_pif_write below; written is just the bits this access
 * carries, positioned (what a write-1-to-clear register must act on, rather
 * than on the untouched half's current bits); commit_high says the access
 * covers the high 32 bits, where every side-effect trigger here sits.
 */
static void xframe_pif_write_reg(XframeState *s, hwaddr addr, uint64_t full,
                                 uint64_t written, bool commit_high)
{
    switch (addr) {
    case XFRAME_SW_RESET:
        if (commit_high && (full >> 40) == XFRAME_SW_RESET_KEY) {
            xframe_soft_reset(s);
        } else {
            s->sw_reset = full;
        }
        return;
    case XFRAME_ADAPTER_CONTROL:
        s->adapter_control = full;
        return;
    case XFRAME_SWAPPER_CTRL:
        s->swapper_ctrl = full;
        return;
    case XFRAME_XMSI_ADDRESS:
        s->xmsi_address = full;
        return;
    case XFRAME_RMAC_CFG_KEY:
        /* The key is in bits 63:48: only a write reaching the high half. */
        if (commit_high) {
            s->rmac_cfg_unlocked = (full >> 48) == XFRAME_RMAC_CFG_KEY_VALUE;
        }
        return;
    case XFRAME_MAC_CFG:
        /* Write-locked: takes only with the config key freshly written. */
        if (s->rmac_cfg_unlocked) {
            s->mac_cfg = full;
            s->rmac_cfg_unlocked = false;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xframe: MAC_CFG write without RMAC_CFG_KEY, "
                          "ignored\n");
        }
        return;
    case XFRAME_RMAC_ADDR_DATA0_MEM:
        s->rmac_addr_data0 = full;
        return;
    case XFRAME_RMAC_ADDR_DATA1_MEM:
        s->rmac_addr_data1 = full;
        return;
    case XFRAME_RMAC_ADDR_CMD_MEM:
        /*
         * The strobe (bit 48), write-enable and offset are all in the high
         * half; only a write reaching it carries out an entry access.
         */
        if (commit_high) {
            xframe_rmac_addr_cmd(s, full);
        } else {
            s->rmac_addr_cmd = full;
        }
        return;
    case XFRAME_RX_TRAFFIC_INT:
        /*
         * Write-1-to-clear: the driver reads the pending bits and writes the
         * same value back to clear exactly those (OpenBSD if_xge.c's
         * xge_intr: "if ((val = PIF_RCSR(RX_TRAFFIC_INT)))
         * PIF_WCSR(RX_TRAFFIC_INT, val);"; Linux s2io.c's s2io_isr writes
         * S2IO_MINUS_ONE, its all-ones write-1-clear-everything sentinel:
         * "if (reason & GEN_INTR_RXTRAFFIC) writeq(S2IO_MINUS_ONE,
         * &bar0->rx_traffic_int);"). This is the real receive-interrupt ACK
         * and is modelled here for its own sake, but it does not recover a
         * ring refilled in place: both drivers perform this exact write
         * BEFORE refilling anything -- OpenBSD's xge_intr acks here and only
         * then walks the ring, calling xge_add_rxbuf per descriptor as it
         * goes; Linux's s2io_isr is the same order. A flush fired from this
         * write would find the ring still empty. xframe_rx_recovery_timer_cb
         * is what actually recovers that case; see its comment.
         *
         * Clear on the bits this access actually carries (written), not on the
         * composed value: a 32-bit write to one half must not clear the other
         * half's pending bits, which its untouched current content would name.
         */
        s->rx_traffic_int &= ~written;
        xframe_update_irq(s);
        return;
    case XFRAME_PRC_RXD0_0:
        s->prc_rxd0_0 = full;
        s->rx_page_addr = full;
        s->rx_desc_idx = 0;
        /*
         * Recovers a netdev queue a prior 0 return left holding a frame, for
         * the case this register write covers: the guest re-arming the ring
         * itself. xframe_rx_recovery_timer_cb is what recovers a ring
         * refilled in place under sustained traffic instead.
         */
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        return;
    case XFRAME_PRC_CTRL_0:
        if ((full & XFRAME_RC_IN_SVC) && !(s->prc_ctrl_0 & XFRAME_RC_IN_SVC)) {
            /*
             * Arming the ring restarts the walk at its base -- the sequence
             * both drivers use writes PRC_RXD0_0 once, before this.
             */
            s->rx_page_addr = s->prc_rxd0_0;
            s->rx_desc_idx = 0;
        }
        if (full & XFRAME_PRC_CTRL_RING_MODE_MASK) {
            qemu_log_mask(LOG_UNIMP,
                          "xframe: only 1-buffer receive mode is modelled; "
                          "ring mode %u ignored\n",
                          (unsigned)((full >> 48) & 3));
        }
        s->prc_ctrl_0 = full;
        /* Same flush, same guest-re-arms-the-ring case as PRC_RXD0_0 above. */
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        return;
    case XFRAME_GENERAL_INT_STATUS:
        /*
         * OpenBSD's xge_intr writes back exactly what it read here
         * ("PIF_WCSR(GENERAL_INT_STATUS, val);"); Linux's s2io_isr never
         * writes it at all, only reads it. Either driver's real per-source
         * acknowledgement is the RX_TRAFFIC_INT write above, and this
         * aggregate is derived rather than stored (see xframe_pif_read), so
         * there is nothing of its own for a write here to clear. Accepted
         * without effect.
         */
        return;
    case XFRAME_GENERAL_INT_MASK:
        s->general_int_mask = full;
        xframe_update_irq(s);
        return;
    case XFRAME_ADAPTER_STATUS:
    case XFRAME_PIF_RD_SWAPPER_FB:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xframe: write to read-only PIF register 0x%" PRIx64
                      ", ignored\n", addr);
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "xframe: PIF write at 0x%" PRIx64 " = 0x%" PRIx64
                      " is not modelled\n", addr, full);
        return;
    }
}

/*
 * A register write at whatever width the memory core delivers. Compose the
 * accessed half onto the register's current content so a 32-bit write to one
 * half leaves the other untouched, and hand xframe_pif_write_reg both the
 * composed value and the raw written bits, plus whether the write reaches the
 * high half (where every side-effect trigger sits).
 */
static void xframe_pif_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    XframeState *s = opaque;
    hwaddr reg = addr & ~7ULL;
    uint64_t written, wmask;
    bool commit_high;

    if (size == 4 && (addr & 4)) {
        wmask = 0xffffffff00000000ULL;
        written = (val & 0xffffffffULL) << 32;
    } else if (size == 4) {
        wmask = 0x00000000ffffffffULL;
        written = val & 0xffffffffULL;
    } else {
        wmask = ~0ULL;
        written = val;
    }
    commit_high = (wmask & 0xffffffff00000000ULL) != 0;

    xframe_pif_write_reg(s, reg,
                         (xframe_pif_read_reg(s, reg) & ~wmask) | written,
                         written, commit_high);
}

static const MemoryRegionOps xframe_pif_ops = {
    .read = xframe_pif_read,
    .write = xframe_pif_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

/*
 * TxP transmit window (BAR2): per-FIFO TxDL doorbell slots. Each slot is 16
 * bytes -- the descriptor-list pointer (PAR) at +0 and the list control (LCR)
 * at +8; writing LCR launches the list. The window is eight FIFOs at a
 * 0x20000 stride, each an 8192-entry TxDL ring. Public OpenBSD if_xgereg.h
 * names.
 */
#define XFRAME_TXP_FIFO_STRIDE      0x00020000
#define XFRAME_TXDL_STRIDE          0x10
#define XFRAME_TXDL_PAR             0x00
#define XFRAME_TXDL_LCR             0x08

/*
 * A TxD is four 64-bit words: control1, control2, buffer pointer, host
 * control. control1 carries ownership (bit 56, 0=host/1=card), the first/last
 * gather-frame flags (41/40), and the 16-bit buffer length in bits 15:0
 * (OpenBSD stores the segment length there; Linux TXD_BUFFER0_SIZE).
 */
#define XFRAME_TXD_SIZE             32
#define XFRAME_TXD_OWN              (1ULL << 56)
#define XFRAME_TXD_GCL              (1ULL << 40)
#define XFRAME_TXD_BUF_LEN_MASK     0xffffULL

/*
 * Run a transmit descriptor list: fetch up to NUMTXD+1 descriptors from the
 * FIFO's pending list pointer, gather their buffers into one frame, hand the
 * first descriptor back to the host, and put the frame on the wire.
 *
 * Guest-driven hang safety: the walk count is the 8-bit NUMTXD field plus one,
 * so at most 256 descriptors -- it cannot spin. A descriptor or buffer read
 * that misses guest memory abandons the list with no progress rather than
 * faulting, and the assembled frame is capped at the jumbo limit rather than
 * copied unbounded. The frame is fully copied into s->tx_frame and the
 * descriptor released before qemu_send_packet, so a receive that a loopback or
 * hub backend re-enters synchronously finds consistent device state.
 */
/* Not gated on ADAPTER_EN: drivers enable the adapter before any doorbell. */
static void xframe_tx_doorbell(XframeState *s, unsigned fifo, uint64_t lcr)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint64_t par = s->txdl_par[fifo];
    unsigned count = ((lcr >> 56) & 0xff) + 1;   /* NUMTXD + 1, 1..256 */
    uint64_t first_control1 = 0;
    size_t frame_len = 0;
    bool have_first = false;
    unsigned i;

    for (i = 0; i < count; i++) {
        uint64_t txd_addr = par + (uint64_t)i * XFRAME_TXD_SIZE;
        uint8_t raw[XFRAME_TXD_SIZE];
        uint64_t control1, bufaddr;
        unsigned seg_len;

        if (pci_dma_read(pdev, txd_addr, raw, sizeof(raw)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xframe: transmit descriptor read at 0x%" PRIx64
                          " failed; list abandoned\n", txd_addr);
            return;
        }
        control1 = ldq_le_p(raw);
        bufaddr = ldq_le_p(raw + 16);
        seg_len = control1 & XFRAME_TXD_BUF_LEN_MASK;

        if (i == 0) {
            if (!(control1 & XFRAME_TXD_OWN)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "xframe: transmit doorbell with a descriptor the "
                              "card does not own; ignored\n");
                return;
            }
            first_control1 = control1;
            have_first = true;
        }

        if (seg_len > XFRAME_MAX_FRAME - frame_len) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xframe: transmit frame exceeds %u bytes; truncated\n",
                          XFRAME_MAX_FRAME);
            seg_len = XFRAME_MAX_FRAME - frame_len;
        }
        if (seg_len > 0 &&
            pci_dma_read(pdev, bufaddr, s->tx_frame + frame_len, seg_len)
                != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xframe: transmit buffer read at 0x%" PRIx64
                          " failed; list abandoned\n", bufaddr);
            return;
        }
        frame_len += seg_len;

        if ((control1 & XFRAME_TXD_GCL) || frame_len >= XFRAME_MAX_FRAME) {
            break;  /* last segment of this frame */
        }
    }

    /* Release the first descriptor to the host (clear its ownership bit). */
    if (have_first) {
        uint8_t word[8];

        stq_le_p(word, first_control1 & ~XFRAME_TXD_OWN);
        pci_dma_write(pdev, par, word, sizeof(word));
    }

    if (frame_len > 0) {
        qemu_send_packet(qemu_get_queue(s->nic), s->tx_frame, frame_len);
    }
}

/* The TxP window is write-only (the doorbell); the drivers never read it. */
static uint64_t xframe_txp_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "xframe: read of the write-only TxP window at 0x%" PRIx64
                  " (size %u)\n", addr, size);
    return 0;
}

static void xframe_txp_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    XframeState *s = opaque;
    /*
     * Ring-index hygiene: the write carries a raw guest-supplied TxDL slot in
     * its address. Reduce the FIFO index modulo the FIFO count before use so a
     * doorbell can never index past the modelled ring (the sgi-ioc3-eth.c /
     * qlogic-isp.c pattern); the BAR decode already bounds both the FIFO and
     * the slot (the slot offset is confined to one FIFO stride). A write that
     * is not on a TxDL pointer or control boundary is a guest error, not a
     * walk.
     */
    unsigned fifo = (addr / XFRAME_TXP_FIFO_STRIDE) % XFRAME_TXP_FIFOS;
    unsigned slot_off = (addr % XFRAME_TXP_FIFO_STRIDE) % XFRAME_TXDL_STRIDE;
    /*
     * The two doorbell fields are 64-bit, so like the PIF window this serves a
     * 32-bit half at off+0/off+4 (both windows must, since the Bridge splits a
     * doubleword into 32-bit device cycles for a DEV_64BIT_DEVICE-clear slot).
     * The list pointer latches per half; ringing the list -- the control
     * field's NUMTXD and gather flags all sit in its high half -- fires only on
     * the committing write reaching that half, so two halves ring it once.
     */
    unsigned field = slot_off & ~7u;
    uint64_t written, wmask;
    bool commit_high;

    if (size == 4 && (slot_off & 4)) {
        wmask = 0xffffffff00000000ULL;
        written = (val & 0xffffffffULL) << 32;
    } else if (size == 4) {
        wmask = 0x00000000ffffffffULL;
        written = val & 0xffffffffULL;
    } else {
        wmask = ~0ULL;
        written = val;
    }
    commit_high = (wmask & 0xffffffff00000000ULL) != 0;

    switch (field) {
    case XFRAME_TXDL_PAR:
        s->txdl_par[fifo] = (s->txdl_par[fifo] & ~wmask) | written;
        return;
    case XFRAME_TXDL_LCR:
        if (commit_high) {
            xframe_tx_doorbell(s, fifo, written);
        }
        return;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xframe: TxP write at 0x%" PRIx64 " is not a TxDL "
                      "pointer or control field; ignored\n", addr);
        return;
    }
}

/*
 * The TxP transmit doorbell registers are 64-bit like the PIF window's, and are
 * served the same 32-bit-half way (see xframe_pif_ops on why impl.min is 4).
 */
static const MemoryRegionOps xframe_txp_ops = {
    .read = xframe_txp_read,
    .write = xframe_txp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

/*
 * MAC_CFG bits the receive filter consults (if_xgereg.h RMAC_PROM_EN,
 * RMAC_BCAST_EN). Everything else in MAC_CFG (TMAC_EN, loopback, pad/FCS
 * strip, RMAC_DISCARD_PFRM, RMAC_ALL_ADDR_EN) has no filtering role here:
 * RMAC_ALL_ADDR_EN doesn't gate multicast any more, since the CAM's own
 * masked-match table now expresses "match every multicast frame" (see
 * xframe_rx_filter) the way both drivers actually configure it, and neither
 * public driver ever sets RMAC_ALL_ADDR_EN itself.
 */
#define XFRAME_MAC_CFG_RMAC_PROM_EN      (1ULL << 56)
#define XFRAME_MAC_CFG_RMAC_BCAST_EN     (1ULL << 54)

/*
 * A 1-buffer-mode RxD is four 64-bit words: host control, control1,
 * control2, buffer pointer (if_xgereg.h's struct rxd1). control1 carries
 * ownership (bit 56, 1=card/0=host) and the status code (XCODE, bits
 * 51:48); control2 carries the buffer size in bits 63:48 -- the posted
 * capacity, overwritten below with the received length.
 */
#define XFRAME_RXD_SIZE              32
#define XFRAME_RXD_OWN               (1ULL << 56)
#define XFRAME_RXD_XCODE_SHIFT       48
#define XFRAME_RXD_XCODE_OK          0
#define XFRAME_RXD_XCODE_ABORT       2   /* abort during transfer */
#define XFRAME_RXD_XCODE_BSZ         7   /* buffer too small for the frame */
#define XFRAME_RXD_BUFSIZ_SHIFT      48

/*
 * RxDs are laid out 127 to a 4 KB page (if_xgereg.h's NDESC_1BUFMODE),
 * chained by a next-page pointer in the page's last eight bytes. Both public
 * drivers allocate several such pages and link them into a ring, so the walk
 * below follows that same chain rather than assuming one page is the whole
 * ring.
 */
#define XFRAME_RXD1_PER_PAGE         127
#define XFRAME_RXD1_PAGE_SIZE        4096
#define XFRAME_RXD1_PAGE_NEXT_OFF    (XFRAME_RXD1_PAGE_SIZE - 8)

/*
 * RX_TRAFFIC_INT's ring-0 completion bit (Linux s2io-regs.h
 * RX_TRAFFIC_INT_n(0), the driver's own MSB-first bit numbering: bit 63).
 * Only ring 0 is modelled.
 */
#define XFRAME_RX_TRAFFIC_RING0      (1ULL << 63)

/*
 * Modest, arbitrary period for the recovery poll below -- not a modelled
 * hardware timer (the real card's own coalescing timers are out of scope
 * here), just short enough that a qtest or a guest driver never notices the
 * delay.
 */
#define XFRAME_RX_RECOVERY_PERIOD_MS  5

/*
 * Follow the ring's next-page pointer once its 127 descriptors are used up.
 * Left in place (index forced back to the page's own start) on a DMA
 * failure: xframe_rx_head's ring-index hygiene then finds that slot already
 * handed back to the host and reports no buffer, rather than walking off
 * the page.
 */
static void xframe_rx_advance_page(XframeState *s)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint8_t raw[8];

    if (pci_dma_read(pdev, s->rx_page_addr + XFRAME_RXD1_PAGE_NEXT_OFF,
                     raw, sizeof(raw)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xframe: receive ring next-page pointer read at "
                      "0x%" PRIx64 " failed\n",
                      s->rx_page_addr + XFRAME_RXD1_PAGE_NEXT_OFF);
        s->rx_desc_idx = 0;
        return;
    }
    s->rx_page_addr = ldq_le_p(raw);
    s->rx_desc_idx = 0;
}

/*
 * Peek the head-of-ring RxD: whether the ring is armed and, if so, whether
 * the card currently owns the next descriptor. can_receive and receive both
 * call this and nothing else to decide "is there a buffer" -- the same
 * question asked the same way, so a filtered frame's drop can never be
 * mistaken for the no-buffer case the 0 return is reserved for.
 *
 * Ring-index hygiene: the in-page position is reduced modulo the page's
 * descriptor count before any walk, the sgi-ioc3-eth/qlogic-isp pattern --
 * so a stuck or corrupted position can never read past the page.
 */
static bool xframe_rx_head(XframeState *s, uint64_t *desc_addr,
                           uint8_t raw[XFRAME_RXD_SIZE])
{
    PCIDevice *pdev = PCI_DEVICE(s);

    if (!(s->prc_ctrl_0 & XFRAME_RC_IN_SVC)) {
        return false;
    }

    s->rx_desc_idx %= XFRAME_RXD1_PER_PAGE;
    *desc_addr = s->rx_page_addr + (uint64_t)s->rx_desc_idx * XFRAME_RXD_SIZE;

    if (pci_dma_read(pdev, *desc_addr, raw, XFRAME_RXD_SIZE) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xframe: receive descriptor read at 0x%" PRIx64
                      " failed\n", *desc_addr);
        return false;
    }

    return (ldq_le_p(raw + 8) & XFRAME_RXD_OWN) != 0;
}

/*
 * Whether a frame off the wire should reach the ring at all -- real hardware
 * filters by destination address in the RMAC block before a frame is ever
 * written to a receive buffer. The 64-entry MAC-address table is both the
 * unicast filter (entry 0 is the factory/station address, but any of the 64
 * is a match) and the multicast filter: both public drivers program a joined
 * multicast group as a masked entry in the very same table
 * (RMAC_ADDR_CMD_MEM/DATA0_MEM/DATA1_MEM -- OpenBSD xge_setmulti, Linux
 * s2io_set_multicast), so one masked-match loop against the table covers
 * both, with no separate multicast path needed.
 *
 * The mask (DATA1_MEM, latched into mac_addr_mask[] by xframe_rmac_addr_cmd)
 * is a per-bit "compare" selector over the same 48 address bits DATA0/DATA0's
 * factory-MAC packing uses (bits 63:16, MSB-first): a mask bit CLEAR means
 * that address bit must match the incoming destination; a mask bit SET means
 * don't care. An all-clear mask (the reset default, and what entry 0's
 * factory MAC gets) is therefore an exact match -- unicast is just the
 * every-bit-compared case of the same rule multicast uses.
 *
 * This polarity is Linux s2io_set_multicast's own, verified two ways that
 * agree with each other: an individual joined group is programmed with
 * DATA1_MEM_MASK(0) -- an all-clear mask, i.e. every bit compared, i.e. an
 * exact match against that one group, which is the only sane reading given
 * per-group multicast join/leave is core, well-exercised functionality, not
 * an edge case. IFF_ALLMULTI is programmed with address 0x010203040506 and
 * mask 0xfeffffffffff: byte 0 of that mask is 0xfe (0b11111110) -- every bit
 * of byte 0 except bit 0 is "don't care", and bytes 1-5 (mask 0xff each) are
 * entirely "don't care" -- so under this polarity the entry compares exactly
 * one bit: byte 0's bit 0, the IEEE group/individual bit, against address
 * 0x01's bit 0, which is 1. That is precisely "any destination with the
 * multicast bit set", with no slop and no special-casing: the general rule
 * falls straight out of Linux's own values.
 *
 * OpenBSD's xge_setmulti does not corroborate this: it writes DATA1_MEM as
 * all-ones (0xFFFFFFFFFFFFFFFF, the opposite of Linux's all-clear) for an
 * individual join, and its own IFF_ALLMULTI sentinel (DATA0=0x8000...000,
 * DATA1=0xF000...000) does not reduce to "match every multicast frame" under
 * either mask polarity with this packing -- worked through both ways, it
 * only ever matches a narrow, specific address pattern, not "every"
 * multicast destination, contradicting its own "receive everything with the
 * multicast bit set" comment. Both readings were checked against the exact
 * literal values in if_xge.c and s2io.c, not against each other's
 * paraphrase. This model follows Linux's convention, since it is the one
 * that is internally consistent on its own account for both the common case
 * (an individual join) and the wildcard case, and does not need a
 * separately-invented special case to explain the wildcard value.
 */
static bool xframe_rx_filter(XframeState *s, const uint8_t *buf, size_t size)
{
    static const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint64_t dest;
    size_t i;

    if (s->mac_cfg & XFRAME_MAC_CFG_RMAC_PROM_EN) {
        return true;
    }
    if (size < 6) {
        return false;
    }
    if (memcmp(buf, broadcast, 6) == 0) {
        return (s->mac_cfg & XFRAME_MAC_CFG_RMAC_BCAST_EN) != 0;
    }

    dest = 0;
    for (i = 0; i < 6; i++) {
        dest = (dest << 8) | buf[i];
    }
    for (i = 0; i < XFRAME_MAC_TABLE_ENTRIES; i++) {
        uint64_t addr = s->mac_addr[i] >> 16;
        uint64_t mask = s->mac_addr_mask[i] >> 16;

        if (((dest ^ addr) & ~mask & 0xffffffffffffULL) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * Arm the recovery poll if it is not already running. Guarded rather than an
 * unconditional timer_mod: both call sites below can run repeatedly while
 * the ring stays genuinely exhausted (can_receive is polled on every packet
 * QEMU's netdev core considers sending), and resetting the deadline on every
 * one of those polls would keep pushing it out and never let the poll fire.
 */
static void xframe_rx_arm_recovery(XframeState *s)
{
    if (!timer_pending(s->rx_recovery_timer)) {
        timer_mod(s->rx_recovery_timer,
                 qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                 XFRAME_RX_RECOVERY_PERIOD_MS);
    }
}

/*
 * Whether the receive engine has anywhere to put a frame right now: the ring
 * armed and its head descriptor owned by the card. Not gated on ADAPTER_EN
 * or MAC_CFG's RMAC_EN, the same simplification the transmit doorbell
 * already makes -- a driver arms the ring and enables the adapter together.
 *
 * QEMU's netdev core checks this BEFORE ever calling receive(): a packet
 * this refuses is queued directly and receive() is never invoked for it, so
 * this -- not receive()'s own 0 return -- is where the genuine no-buffer
 * condition is actually seen in practice, and where the recovery poll has to
 * be armed for a ring the guest will go on to refill in place.
 */
static bool xframe_can_receive(NetClientState *nc)
{
    XframeState *s = XFRAME(qemu_get_nic_opaque(nc));
    uint64_t desc_addr;
    uint8_t raw[XFRAME_RXD_SIZE];
    bool owned = xframe_rx_head(s, &desc_addr, raw);

    if (!owned) {
        xframe_rx_arm_recovery(s);
    }

    return owned;
}

/*
 * Deposit one frame into the head-of-ring RxD: filter it, write it into the
 * posted buffer, and publish the completion -- length and status first, then
 * ownership released last, the same order the transmit doorbell releases its
 * own descriptor.
 *
 * Guest-driven hang safety: a filtered frame and a genuine no-buffer
 * condition return different values on purpose. No buffer at all returns 0,
 * which is QEMU's netdev-queue signal to hold and redeliver this same frame
 * once can_receive reports true again. A filtered frame returns the frame's
 * full size instead, because nothing about it changes on a retry -- treating
 * it as consumed is what keeps the queue from wedging on a frame no future
 * attempt will ever pass.
 */
static ssize_t xframe_receive(NetClientState *nc, const uint8_t *buf,
                              size_t size)
{
    XframeState *s = XFRAME(qemu_get_nic_opaque(nc));
    PCIDevice *pdev = PCI_DEVICE(s);
    uint64_t desc_addr, control2, bufaddr;
    uint8_t raw[XFRAME_RXD_SIZE];
    size_t capacity, copy_len;
    unsigned xcode;
    uint8_t word[8];

    if (!xframe_rx_head(s, &desc_addr, raw)) {
        /*
         * The genuine no-buffer condition -- reached here, rather than only
         * from can_receive's own gate, when a recovery-poll flush retries a
         * queued packet directly and still finds nothing owned.
         */
        xframe_rx_arm_recovery(s);
        return 0;
    }

    if (!xframe_rx_filter(s, buf, size)) {
        timer_del(s->rx_recovery_timer);
        return size;
    }

    control2 = ldq_le_p(raw + 16);
    bufaddr = ldq_le_p(raw + 24);
    capacity = (control2 >> XFRAME_RXD_BUFSIZ_SHIFT) & 0xffff;

    copy_len = size;
    if (copy_len > XFRAME_MAX_FRAME) {
        copy_len = XFRAME_MAX_FRAME;
    }
    xcode = XFRAME_RXD_XCODE_OK;
    if (copy_len > capacity) {
        xcode = XFRAME_RXD_XCODE_BSZ;
        copy_len = capacity;
    }

    if (copy_len > 0 &&
        pci_dma_write(pdev, bufaddr, buf, copy_len) != MEMTX_OK) {
        /*
         * The descriptor itself was fine; the buffer pointer it named was
         * not. Release the descriptor and advance past it below rather than
         * leaving it parked here -- the same bad pointer would fail
         * identically on every future frame, permanently stalling the ring
         * behind one bad buffer.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xframe: receive buffer write at 0x%" PRIx64
                      " failed; frame dropped\n", bufaddr);
        copy_len = 0;
        xcode = XFRAME_RXD_XCODE_ABORT;
    }

    stq_le_p(word, (uint64_t)copy_len << XFRAME_RXD_BUFSIZ_SHIFT);
    pci_dma_write(pdev, desc_addr + 16, word, sizeof(word));

    stq_le_p(word, (uint64_t)xcode << XFRAME_RXD_XCODE_SHIFT);
    pci_dma_write(pdev, desc_addr + 8, word, sizeof(word));

    s->rx_desc_idx++;
    if (s->rx_desc_idx >= XFRAME_RXD1_PER_PAGE) {
        xframe_rx_advance_page(s);
    }

    s->rx_traffic_int |= XFRAME_RX_TRAFFIC_RING0;
    xframe_update_irq(s);
    timer_del(s->rx_recovery_timer);

    return size;
}

/*
 * Re-attempt delivery of whatever the netdev queue is holding. Armed by
 * xframe_rx_arm_recovery -- called from can_receive's own no-buffer case,
 * where QEMU's netdev core actually turns a refused packet away before
 * receive() is ever invoked, and again from receive()'s genuine no-buffer
 * return for a flush-triggered retry that still finds nothing. Disarmed by
 * every other return from receive(), so this only ever runs during the
 * window between "a frame was refused for lack of an owned descriptor" and
 * "the ring has one again".
 *
 * This -- not the RX_TRAFFIC_INT acknowledge modelled above -- is what
 * recovers a ring the guest refills in place (flips an RxD's OWN bit
 * directly in guest RAM, no register write at all): both public drivers
 * acknowledge the receive interrupt BEFORE refilling any descriptor
 * (OpenBSD's xge_intr acks RX_TRAFFIC_INT and only then walks the ring,
 * calling xge_add_rxbuf per descriptor as it goes; Linux's s2io_isr is the
 * same order), so nothing the guest writes to a register ever lands after
 * the refill for this device to hook a flush onto. Polling is the only
 * mechanism left that can still notice the refill once it happens.
 */
static void xframe_rx_recovery_timer_cb(void *opaque)
{
    XframeState *s = opaque;

    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

static NetClientInfo xframe_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = xframe_can_receive,
    .receive = xframe_receive,
};

static void xframe_reset_hold(Object *obj, ResetType type)
{
    XframeState *s = XFRAME(obj);

    /*
     * Put the register file back to its documented reset values (including
     * reloading the factory MAC into table entry 0), then deassert the
     * interrupt line so a freshly reset or migrated-in device starts with the
     * pin low rather than however qdev left it.
     */
    xframe_soft_reset(s);
    pci_set_irq(PCI_DEVICE(s), 0);
}

static void xframe_realize(PCIDevice *dev, Error **errp)
{
    XframeState *s = XFRAME(dev);
    DeviceState *d = DEVICE(dev);

    s->rx_recovery_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                        xframe_rx_recovery_timer_cb, s);

    /*
     * The class-level defaults set vendor/device id (and, absent a
     * subsystem property, QEMU's own generic subsystem default) before
     * this realize runs; every field here is a property, so restate all
     * four explicitly rather than let the generic default leak into a
     * field this device does expose.
     */
    pci_config_set_vendor_id(dev->config, s->vendor_id);
    pci_config_set_device_id(dev->config, s->device_id);
    pci_set_word(dev->config + PCI_SUBSYSTEM_VENDOR_ID, s->subsystem_vendor_id);
    pci_set_word(dev->config + PCI_SUBSYSTEM_ID, s->subsystem_id);

    /*
     * One interrupt line, INTx -- see docs/datasheets/xframe.md's
     * "Interrupts": legacy PCI INTx is the era-correct choice for the
     * Origin 200; MSI/MSI-X is optional future work.
     */
    dev->config[PCI_INTERRUPT_PIN] = 0x01;

    memory_region_init_io(&s->pif_mmio, OBJECT(s), &xframe_pif_ops, s,
                          "xframe.pif", XFRAME_PIF_WINDOW_SIZE);
    memory_region_init_io(&s->txp_mmio, OBJECT(s), &xframe_txp_ops, s,
                          "xframe.txp", XFRAME_TXP_WINDOW_SIZE);

    /*
     * BAR0 is a 64-bit memory BAR (config 0x10/0x14), so the next BAR
     * lands at 0x18 -- PCI BAR index 2, not 1, since index 1 is BAR0's own
     * upper half. docs/datasheets/xframe.md's "PCI identity" table.
     */
    pci_register_bar(dev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->pif_mmio);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->txp_mmio);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&xframe_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), d->id,
                          &d->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void xframe_exit(PCIDevice *dev)
{
    XframeState *s = XFRAME(dev);

    timer_free(s->rx_recovery_timer);
    qemu_del_nic(s->nic);
}

/*
 * The migratable register state, per docs/datasheets/xframe.md's "VMState /
 * migration fields": the byte-swap and scratch registers, ADAPTER_CONTROL,
 * GENERAL_INT_MASK, MAC_CFG and its unlock latch, the whole MAC-address
 * table and its parallel per-entry mask (entry 0's factory address plus any
 * programmed unicast or multicast entry -- whatever the guest wrote must
 * survive migration exactly), the per-FIFO pending transmit list pointer,
 * and the receive ring's own bookkeeping
 * (PRC_RXD0_0/PRC_CTRL_0, the pending RX_TRAFFIC_INT status, and the card's
 * current walk position through the ring). GENERAL_INT_STATUS needs no
 * field of its own: it is derived from RX_TRAFFIC_INT on every read, so
 * migrating that one register is enough to reproduce it. The netdev
 * frontend's own state lives in NICConf, which every NIC model leaves out
 * of its VMState (see hw/net/sgi-ioc3-eth.c and hw/net/rtl8139.c); PCI
 * config space is covered by VMSTATE_PCI_DEVICE.
 */
static const VMStateDescription vmstate_xframe = {
    .name = "xframe",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, XframeState),
        VMSTATE_UINT64(swapper_ctrl, XframeState),
        VMSTATE_UINT64(xmsi_address, XframeState),
        VMSTATE_UINT64(sw_reset, XframeState),
        VMSTATE_UINT64(adapter_control, XframeState),
        VMSTATE_UINT64(general_int_mask, XframeState),
        VMSTATE_UINT64(mac_cfg, XframeState),
        VMSTATE_BOOL(rmac_cfg_unlocked, XframeState),
        VMSTATE_UINT64(rmac_addr_cmd, XframeState),
        VMSTATE_UINT64(rmac_addr_data0, XframeState),
        VMSTATE_UINT64(rmac_addr_data1, XframeState),
        VMSTATE_UINT64_ARRAY(mac_addr, XframeState, XFRAME_MAC_TABLE_ENTRIES),
        VMSTATE_UINT64_ARRAY(mac_addr_mask, XframeState,
                             XFRAME_MAC_TABLE_ENTRIES),
        VMSTATE_UINT64_ARRAY(txdl_par, XframeState, XFRAME_TXP_FIFOS),
        VMSTATE_UINT64(prc_rxd0_0, XframeState),
        VMSTATE_UINT64(prc_ctrl_0, XframeState),
        VMSTATE_UINT64(rx_traffic_int, XframeState),
        VMSTATE_UINT64(rx_page_addr, XframeState),
        VMSTATE_UINT32(rx_desc_idx, XframeState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property xframe_properties[] = {
    DEFINE_PROP_UINT16("vendor-id", XframeState, vendor_id,
                       XFRAME_PCI_VENDOR_ID),
    DEFINE_PROP_UINT16("device-id", XframeState, device_id,
                       XFRAME_PCI_DEVICE_ID_XENA),
    /*
     * Generic default (0): the chip has no subsystem branding of its own.
     * The SGI board sets subsystem-vendor-id to 0x10a9; its
     * subsystem-device-id is not established from any source read for the
     * datasheet, so subsystem-id has no SGI-specific default to give it --
     * see include/hw/net/xframe.h.
     */
    DEFINE_PROP_UINT16("subsystem-vendor-id", XframeState,
                       subsystem_vendor_id, 0),
    DEFINE_PROP_UINT16("subsystem-id", XframeState, subsystem_id, 0),
    DEFINE_NIC_PROPERTIES(XframeState, conf),
};

static void xframe_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    k->realize = xframe_realize;
    k->exit = xframe_exit;
    k->vendor_id = XFRAME_PCI_VENDOR_ID;
    k->device_id = XFRAME_PCI_DEVICE_ID_XENA;
    k->class_id = PCI_CLASS_NETWORK_ETHERNET;
    rc->phases.hold = xframe_reset_hold;
    dc->desc = "Neterion/S2io Xframe 10-Gigabit Ethernet controller";
    dc->vmsd = &vmstate_xframe;
    device_class_set_props(dc, xframe_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo xframe_types[] = {
    {
        .name = TYPE_XFRAME,
        .parent = TYPE_PCI_DEVICE,
        .instance_size = sizeof(XframeState),
        .class_init = xframe_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(xframe_types)
