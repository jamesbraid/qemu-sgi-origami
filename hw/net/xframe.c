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
#include "net/checksum.h"

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
#define XFRAME_PIC_INT_STATUS       0x0800
#define XFRAME_PIC_INT_MASK         0x0808
#define XFRAME_TXPIC_INT_REG        0x0810
#define XFRAME_TXPIC_INT_MASK       0x0818
#define XFRAME_RXPIC_INT_MASK       0x0830
#define XFRAME_GPIO_INT_REG         0x0890
#define XFRAME_GPIO_INT_MASK        0x0898
#define XFRAME_TX_TRAFFIC_INT       0x08e0
#define XFRAME_TX_TRAFFIC_MASK      0x08e8
#define XFRAME_RX_TRAFFIC_INT       0x08f0
#define XFRAME_RX_TRAFFIC_MASK      0x08f8
#define XFRAME_SWAPPER_CTRL         0x0908
#define XFRAME_PIF_RD_SWAPPER_FB    0x0910
#define XFRAME_SCHED_INT_CTRL       0x0918
#define XFRAME_XMSI_ADDRESS         0x0960
#define XFRAME_PRC_RXD0_0           0x1930
#define XFRAME_PRC_CTRL_0           0x1970
#define XFRAME_MAC_INT_MASK         0x2008
#define XFRAME_MAC_RMAC_ERR_REG     0x2028
#define XFRAME_MAC_RMAC_ERR_MASK    0x2030
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
 * GENERAL_INT_STATUS's transmit-traffic aggregate bit (Linux s2io-regs.h
 * GEN_INTR_TXTRAFFIC = s2BIT(8), i.e. bit 63-8 = 55), and the per-FIFO
 * transmit-completion bit within TX_TRAFFIC_INT (TX_TRAFFIC_INT_n(n) =
 * s2BIT(n), FIFO 0 = bit 63) -- the same MSB-first numbering and the
 * transmit mirror of RX_TRAFFIC_INT. Both drivers unmask transmit traffic at
 * bring-up (writing TX_TRAFFIC_MASK = 0) and ack TX_TRAFFIC_INT in their ISR.
 */
#define XFRAME_GEN_INTR_TXTRAFFIC   (1ULL << 55)
#define XFRAME_TX_TRAFFIC_FIFO(n)   (1ULL << (63 - (n)))

/*
 * GENERAL_INT_STATUS's receive-MAC aggregate bit (Linux s2io-regs.h
 * GEN_INTR_RXMAC = s2BIT(34), i.e. bit 63-34 = 29 in the same MSB-first
 * numbering as GEN_INTR_RXTRAFFIC above). The RMAC link-state-change
 * interrupt (below) is a receive-MAC-block source, so it rolls up into this
 * top-level bit -- the aggregate both drivers read before descending to the
 * per-block MAC_RMAC_ERR_REG (OpenBSD xge_intr, Linux s2io_isr).
 */
#define XFRAME_GEN_INTR_RXMAC       (1ULL << 29)

/*
 * MAC_RMAC_ERR_REG's link-state-change interrupt (OpenBSD if_xgereg.h
 * RMAC_LINK_STATE_CHANGE_INT = 1ULL<<32; Linux s2io-regs.h s2BIT(31), the
 * same bit 32). Both drivers unmask it at bring-up -- OpenBSD's xge_init
 * writes MAC_RMAC_ERR_MASK = ~RMAC_LINK_STATE_CHANGE_INT and MAC_INT_MASK to
 * leave only RMAC unmasked; Linux's en_dis_err_alarms adds it to the RX-MAC
 * unmask set -- and both service it in their ISR (OpenBSD reads
 * MAC_RMAC_ERR_REG and, if link restored, re-enables the adapter). It is the
 * one interrupt that fires on an idle interface with no traffic: the link
 * coming up. Write-1-to-clear, like RX_TRAFFIC_INT.
 */
#define XFRAME_RMAC_LINK_STATE_CHANGE_INT  (1ULL << 32)

/*
 * MAC_INT_STATUS/MAC_INT_MASK's receive-MAC roll-up bit (OpenBSD if_xgereg.h
 * MAC_RMAC_INT = 1ULL<<62; MAC_TMAC_INT = 1ULL<<63 is the transmit-MAC sibling
 * both drivers mask). The RMAC error/link block only rolls up into
 * GEN_INTR_RXMAC when this bit is unmasked in MAC_INT_MASK -- OpenBSD xge_init
 * writes MAC_INT_MASK = MAC_TMAC_INT (only RMAC unmasked); Linux
 * en_dis_err_alarms unmasks MAC_INT_STATUS_RMAC_INT. So the link source is
 * gated by two MAC masks: this block roll-up mask and MAC_RMAC_ERR_MASK's leaf
 * link bit.
 */
#define XFRAME_MAC_RMAC_INT         (1ULL << 62)

/*
 * GENERAL_INT_STATUS's PCI-X-block aggregate bit (Linux s2io-regs.h
 * GEN_INTR_TXPIC = s2BIT(0), i.e. bit 63 in the same MSB-first numbering as the
 * other GEN_INTR bits above). The whole PIC block -- the GPIO link-state source
 * below included -- rolls up into this one top-level bit, the one s2io_isr
 * dispatches to s2io_txpic_intr_handle. (The chip's own name for the PIC
 * block's general-status bit; nothing transmit-specific about it.)
 */
#define XFRAME_GEN_INTR_TXPIC       (1ULL << 63)

/*
 * PIC_INT_STATUS's GPIO sub-block aggregate bit (Linux s2io-regs.h PIC_INT_GPIO
 * = s2BIT(4), i.e. bit 59). The GPIO leaf below rolls up into PIC_INT_STATUS
 * here, and PIC_INT_MASK (0x808) gates this bit's contribution to
 * GEN_INTR_TXPIC. IRIX xg opens the whole PIC block (writes PIC_INT_MASK = 0),
 * so this bit is unmasked -- the block xg's bring-up actually enables.
 */
#define XFRAME_PIC_INT_GPIO         (1ULL << 59)

/*
 * PIC_INT_STATUS's TXPIC (PCI-X transmit) sub-block aggregate bit (Linux
 * s2io-regs.h PIC_INT_TX = s2BIT(0), i.e. bit 63). The TXPIC leaf (TXPIC_INT_REG
 * / TXPIC_INT_MASK) rolls up into PIC_INT_STATUS here, gated by PIC_INT_MASK
 * (0x808). The scheduled interrupt below is delivered on this sub-block, the
 * one xg unmasks (PIC_INT_MASK = 0, TXPIC_INT_MASK = 0) alongside the GPIO one.
 */
#define XFRAME_PIC_INT_TX           (1ULL << 63)

/*
 * The scheduled/periodic-interrupt timer control (Linux s2io-regs.h
 * scheduled_int_ctrl at 0x918; SCHED_INT_CTRL_TIMER_EN = s2BIT(0) = bit 63 arms
 * the timer, SCHED_INT_CTRL_ONE_SHOT = s2BIT(1) = bit 62 fires once rather than
 * repeating). IRIX xg programs this at bring-up to feed its "no interrupts"
 * watchdog with a traffic-independent periodic interrupt.
 */
#define XFRAME_SCHED_INT_CTRL_TIMER_EN  (1ULL << 63)
#define XFRAME_SCHED_INT_CTRL_ONE_SHOT  (1ULL << 62)

/*
 * The scheduled interrupt's leaf latch in TXPIC_INT_REG (0x810), gated by
 * TXPIC_INT_MASK (0x818). The public header (s2io-regs.h) names the TXPIC leaf
 * bits for PCI-X transmit errors and marks the scheduled-interrupt routing and
 * period (SCHED_INT_PERIOD) TBD, so this is a model-chosen bit in the same leaf
 * register the source rolls up through -- placed in the high 32 bits so the
 * ISR's high-half read acks it, like the GPIO link bits. Distinct from every
 * PCIX_INT_REG_* error bit.
 */
#define XFRAME_TXPIC_INT_SCHED      (1ULL << 59)

/*
 * GPIO_INT_REG's link-state bits and GPIO_INT_MASK's matching mask bits (Linux
 * s2io-regs.h GPIO_INT_REG_LINK_UP = s2BIT(2) = bit 61, GPIO_INT_REG_LINK_DOWN
 * = s2BIT(1) = bit 62; GPIO_INT_MASK_LINK_UP/DOWN the same bit positions). The
 * link-state interrupt an idle interface sees on the PIC/GPIO path -- a
 * DIFFERENT source from the MAC RMAC_LINK_STATE_CHANGE path above. Linux's ISR
 * reads PIC_INT_STATUS for PIC_INT_GPIO, then GPIO_INT_REG for LINK_UP, then
 * enables the adapter and marks the link up (s2io_txpic_intr_handle); its
 * Hercules enable path unmasks PIC_INT_GPIO in PIC_INT_MASK and
 * GPIO_INT_MASK_LINK_UP in GPIO_INT_MASK (en_dis_able_nic_intrs). All the GPIO
 * bits sit in the high 32 bits. Write-1-to-clear on the status, like the other
 * status registers here.
 */
#define XFRAME_GPIO_INT_REG_LINK_UP    (1ULL << 61)
#define XFRAME_GPIO_INT_REG_LINK_DOWN  (1ULL << 62)
#define XFRAME_GPIO_INT_LINK_BITS      (XFRAME_GPIO_INT_REG_LINK_UP | \
                                        XFRAME_GPIO_INT_REG_LINK_DOWN)

/*
 * ADAPTER_CONTROL's enable bit (OpenBSD if_xgereg.h ADAPTER_EN = 1ULL<<56):
 * xge_enable sets it to bring the adapter up. The model arms the link-up
 * timer on the 0->1 transition, modelling the link coming up shortly after.
 */
#define XFRAME_ADAPTER_EN           (1ULL << 56)

/*
 * How long after the driver enables the adapter the modelled link presents its
 * FIRST edge -- a link-DOWN (see xframe_link_down_timer_cb). Real hardware
 * raises the link event asynchronously, milliseconds after enable and after the
 * driver's synchronous bring-up (which acks any stale link event before
 * unmasking, so a same-instant latch would be wiped). A short virtual-time
 * delay reproduces that ordering without a modelled PHY. Not a hardware timer;
 * sized like the receive recovery poll, short enough that a guest never
 * notices.
 */
#define XFRAME_LINK_UP_DELAY_MS     5

/*
 * How long after the link-DOWN edge the modelled link comes UP -- the second
 * edge of the bring-up DOWN->UP transition (see xframe_link_up_timer_cb). The
 * Neterion family gates carrier and transmit on a link-state CHANGE, never a
 * level, and treats both link bits set at once as an "unstable" state to clear
 * and re-wait on; so the model presents the DOWN and the UP as two separate,
 * separately-serviced interrupts an interval apart, giving a driver whose link
 * belief is already "up" a genuine edge to flip it. Long enough that the guest
 * services the DOWN before the UP arrives. Not pinned by the public drivers
 * (they treat the link event as a one-shot edge), so this is a best-supported
 * interval, not a value read from a reference. Not a hardware timer.
 */
#define XFRAME_LINK_DOWN_TO_UP_MS   500

/*
 * The scheduled interrupt's period. A live IRIX xg boot pinned the watchdog as
 * a pure liveness timer -- it does not care what interrupt arrives, only that
 * one arrives inside its interval (~15 s), and it resets the adapter
 * ("xframe_watchdog: ERROR: No interrupts occuring, reset device...") after the
 * single bring-up link event, each reset re-knocking the link down. xg feeds it
 * by arming a periodic hardware scheduled interrupt at bring-up (writing
 * SCHED_INT_CTRL, unmasking PIC_INT_MASK and TXPIC_INT_MASK), independent of
 * traffic. The hardware period is computed at runtime from PCI bus speed and is
 * not a public constant (s2io-regs.h marks SCHED_INT_PERIOD TBD); the model only
 * needs "more often than the ~15 s watchdog", so it uses a fixed virtual-time
 * period derived from that behaviour. Two seconds leaves comfortable margin.
 * Not a hardware timer's exact tick.
 */
#define XFRAME_SCHED_INT_PERIOD_MS  2000

/*
 * Every interrupt mask resets to all-ones = fully masked. Reset values are
 * marked OPEN in the datasheet ("OPEN (masked)"); all-ones is the concrete
 * bit pattern the public drivers use for "masked" (Linux s2io.h's
 * DISABLE_ALL_INTRS / S2IO_MINUS_ONE, OpenBSD's all-ones convention on the
 * sibling registers), and it is the spurious-safe default: no source can
 * assert INTx until a driver unmasks its block. Both the top-level
 * GENERAL_INT_MASK and the per-block masks share this reset -- see
 * xframe_update_irq for which one actually gates the line.
 */
#define XFRAME_INT_MASK_RESET       0xffffffffffffffffULL

/*
 * ADAPTER_STATUS quiescent/ready value the driver's single non-retrying attach
 * read requires (if_xgereg.h QUIESCENT): TDMA/RDMA/PFC ready, TMAC buffer
 * empty, PIC quiescent, memory-controller DRAM+queues ready, both PLLs locked
 * -- and the fault bits clear. It also includes the subsystem-quiescence/
 * ready bits 8-15 of the high word: the driver's adapter-start (transmit-
 * enable) path requires every one of those bits set before it marks the
 * adapter ready and rings the transmit doorbell. The model is always ready,
 * so it presents this immediately.
 */
#define XFRAME_ADAPTER_STATUS_QUIESCENT  0xf400ffc300000000ULL

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
 * Whether the link-state-change source is delivering an interrupt right now:
 * the bit is latched in MAC_RMAC_ERR_REG and unmasked at both MAC levels --
 * MAC_RMAC_ERR_MASK's leaf link bit and MAC_INT_MASK's RMAC block bit. Both
 * drivers unmask both (OpenBSD xge_init: MAC_INT_MASK = MAC_TMAC_INT,
 * MAC_RMAC_ERR_MASK = ~RMAC_LINK_STATE_CHANGE_INT; Linux en_dis_err_alarms
 * unmasks MAC_INT_STATUS_RMAC_INT and, on Xena, RMAC_LINK_STATE_CHANGE_INT).
 */
static bool xframe_link_pending(XframeState *s)
{
    return (s->mac_rmac_err & XFRAME_RMAC_LINK_STATE_CHANGE_INT) &&
           !(s->mac_rmac_err_mask & XFRAME_RMAC_LINK_STATE_CHANGE_INT) &&
           !(s->mac_int_mask & XFRAME_MAC_RMAC_INT);
}

/*
 * Whether the GPIO link-state source is delivering an interrupt right now: a
 * link bit is latched in GPIO_INT_REG and the PIC block's PIC_INT_GPIO roll-up
 * is unmasked in PIC_INT_MASK. This is the PIC/GPIO-block link path IRIX xg
 * enables, distinct from the MAC-block xframe_link_pending above that OpenBSD
 * xge uses. Linux services it the same way (s2io_txpic_intr_handle reads
 * pic_int_status for PIC_INT_GPIO, then gpio_int_reg for LINK_UP).
 *
 * Gated by PIC_INT_MASK's PIC_INT_GPIO bit ALONE, deliberately NOT by the GPIO
 * leaf mask (gpio_int_mask): the live IRIX xg diagnostic is decisive. xg opens
 * PIC_INT_MASK (clearing PIC_INT_GPIO) and TXPIC_INT_MASK but leaves
 * gpio_int_mask at its all-ones reset, and its watchdog is nonetheless
 * satisfied once this source reaches its ISR -- so the leaf mask cannot gate
 * delivery for xg. An earlier model that required the leaf too (Linux's
 * Hercules enable path in en_dis_able_nic_intrs unmasks GPIO_INT_MASK_LINK_UP)
 * left the source unreachable and xg's watchdog resetting the adapter in a
 * loop; the measurement outranks that roll-up theory. gpio_int_mask is held for
 * read-back only.
 */
static bool xframe_gpio_link_pending(XframeState *s)
{
    return ((s->gpio_int_reg & XFRAME_GPIO_INT_LINK_BITS) != 0) &&
           !(s->pic_int_mask & XFRAME_PIC_INT_GPIO);
}

/*
 * Whether the scheduled interrupt is delivering right now: the periodic timer
 * has fired and latched sched_int_pending, and the source is unmasked at BOTH
 * levels xg opens -- PIC_INT_MASK's PIC_INT_TX roll-up bit and TXPIC_INT_MASK's
 * leaf bit. This is the traffic- and link-independent watchdog feed on the
 * PIC/TXPIC path, distinct from the GPIO link source above (gated by
 * PIC_INT_GPIO) and never touching the GPIO/link bits, so it never disturbs
 * carrier. Both masks gate it because xg unmasks both (PIC_INT_MASK = 0,
 * TXPIC_INT_MASK = 0) -- unlike the GPIO source, whose leaf mask xg leaves set.
 */
static bool xframe_sched_int_pending(XframeState *s)
{
    return s->sched_int_pending &&
           !(s->pic_int_mask & XFRAME_PIC_INT_TX) &&
           !(s->txpic_int_mask & XFRAME_TXPIC_INT_SCHED);
}

/*
 * Recompute the INTx line. Each source is gated by its own PER-BLOCK
 * (second-level) mask -- NOT by the top-level GENERAL_INT_MASK, which the model
 * does not use to gate anything (see xframe_pif_read_reg: the summary is the
 * raw block-pending aggregate). This is what the drivers actually rely on: IRIX
 * xg enables interrupts purely through the block masks (PIC_INT_MASK,
 * TXPIC_INT_MASK) and leaves GENERAL_INT_MASK at its all-ones reset, yet takes
 * interrupts on real hardware -- impossible if the general mask gated the line.
 * OpenBSD xge_init and Linux en_dis_able_nic_intrs also unmask at the block
 * level (TX/RX_TRAFFIC_MASK = 0, the MAC masks); they additionally write
 * GENERAL_INT_MASK (OpenBSD to 0, Linux clearing the aggregate bits), but the
 * model's summary is the raw aggregate regardless -- see the read handler.
 * Gating on the block masks makes all three drivers work; gating on the general
 * mask is what left xg's watchdog resetting in a loop.
 *
 * Four sources so far: receive completion (RX_TRAFFIC_INT gated by
 * RX_TRAFFIC_MASK), transmit completion (TX_TRAFFIC_INT gated by
 * TX_TRAFFIC_MASK), and two independent link-state paths -- the MAC-block one
 * (MAC_RMAC_ERR_REG gated by the two MAC masks, what OpenBSD xge uses) and the
 * PIC/GPIO-block one (GPIO_INT_REG gated by PIC_INT_MASK's PIC_INT_GPIO bit
 * alone, what IRIX xg uses). Either fires on an idle interface -- no frame need
 * arrive for the link to come up -- so a driver watchdog that resets on "no
 * interrupts" is satisfied through whichever block that driver unmasked.
 */
static void xframe_update_irq(XframeState *s)
{
    bool rx_pending = (s->rx_traffic_int & ~s->rx_traffic_mask) != 0;
    bool tx_pending = (s->tx_traffic_int & ~s->tx_traffic_mask) != 0;
    bool link_pending = xframe_link_pending(s);
    bool gpio_pending = xframe_gpio_link_pending(s);
    bool sched_pending = xframe_sched_int_pending(s);

    pci_set_irq(PCI_DEVICE(s),
                rx_pending || tx_pending || link_pending || gpio_pending ||
                sched_pending);
}

/*
 * The FIRST bring-up edge: the modelled link goes DOWN on the PIC/GPIO path,
 * then arms the UP edge one interval later. IRIX xg (and the Neterion family
 * generally) propagates carrier and wakes transmit only on a link-state CHANGE,
 * so the model must present a genuine DOWN->UP transition -- a bare, unchanging
 * UP is a no-op to a driver whose link belief is already "up", and transmit
 * stays gated. This latches only GPIO_INT_REG LINK_DOWN (the PIC/GPIO path xg
 * uses); the MAC-path ADAPTER_STATUS stays quiescent with its fault bits clear,
 * so a MAC-block "down" has nowhere to show and the DOWN is scoped to the GPIO
 * path. Armed by the ADAPTER_EN 0->1 transition (see XFRAME_LINK_UP_DELAY_MS),
 * this fires after the driver's synchronous bring-up has unmasked interrupts,
 * so the pending bit is delivered rather than acked away. Its own callback arms
 * the UP edge (XFRAME_LINK_DOWN_TO_UP_MS out) so the two are separate,
 * separately-serviced interrupts -- never both link bits set at once, which
 * s2io treats as an unstable state it clears and re-waits on.
 */
static void xframe_link_down_timer_cb(void *opaque)
{
    XframeState *s = opaque;

    s->gpio_int_reg |= XFRAME_GPIO_INT_REG_LINK_DOWN;
    xframe_update_irq(s);
    timer_mod(s->link_up_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              XFRAME_LINK_DOWN_TO_UP_MS);
}

/*
 * The SECOND bring-up edge: the modelled link comes up. Latch the
 * link-state-change interrupt on BOTH link paths -- the MAC-block
 * RMAC_LINK_STATE_CHANGE_INT and the PIC/GPIO-block GPIO_INT_REG LINK_UP -- and
 * recompute the line, clearing any still-latched LINK_DOWN so the UP is a clean
 * up-only edge even if the guest never serviced the DOWN. A driver arms
 * whichever block it uses, so latching both and letting each source's own masks
 * decide delivery serves OpenBSD (MAC path) and IRIX xg (PIC/GPIO path) from
 * one event without either seeing the other's. Armed one interval after the
 * DOWN edge (see xframe_link_down_timer_cb), this completes the DOWN->UP
 * transition. This is the settled up-link interrupt an idle interface sees --
 * the reason a watchdog that resets on "no interrupts" is satisfied without any
 * frame having to arrive.
 *
 * Guarded against the QEMU backend link, not just the guest-facing bits: the
 * bring-up sequence arms on the ADAPTER_EN edge alone, with no regard for
 * whether the netdev backend is actually up, so a `set_link off` issued (or
 * already in effect) before this edge fires must not announce a spurious
 * LINK_UP for a backend that is down. xframe_set_link_status below cancels
 * both bring-up timers the moment the backend drops, so this guard is
 * ordinarily never reached with the backend down mid-transition -- it is a
 * second line of defence against a driver bring-up racing a backend that was
 * already down before ADAPTER_EN's rising edge armed this timer in the first
 * place, since that edge does not itself consult the backend. LINK_DOWN was
 * already latched by xframe_link_down_timer_cb (and/or by
 * xframe_set_link_status), so this leaves it in place and returns rather than
 * completing the transition.
 */
static void xframe_link_up_timer_cb(void *opaque)
{
    XframeState *s = opaque;

    if (qemu_get_queue(s->nic)->link_down) {
        s->gpio_int_reg = (s->gpio_int_reg & ~XFRAME_GPIO_INT_REG_LINK_UP) |
                          XFRAME_GPIO_INT_REG_LINK_DOWN;
        xframe_update_irq(s);
        return;
    }

    s->gpio_int_reg = (s->gpio_int_reg & ~XFRAME_GPIO_INT_REG_LINK_DOWN) |
                      XFRAME_GPIO_INT_REG_LINK_UP;
    s->mac_rmac_err |= XFRAME_RMAC_LINK_STATE_CHANGE_INT;
    xframe_update_irq(s);
}

/*
 * Arm the scheduled interrupt one period out, and record that it is armed --
 * migrated (sched_int_armed) so a target that loads mid-flight knows whether
 * a completed one-shot needs re-arming or not (see xframe_post_load).
 */
static void xframe_sched_int_arm(XframeState *s)
{
    s->sched_int_armed = true;
    timer_mod(s->sched_int_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + XFRAME_SCHED_INT_PERIOD_MS);
}

/*
 * The scheduled interrupt fires: latch it and recompute INTx. This is the
 * traffic- and link-independent watchdog feed the driver programs at bring-up
 * (SCHED_INT_CTRL) -- it rolls up through the TXPIC leaf and PIC_INT_TX into
 * GEN_INTR_TXPIC and is serviced by the ISR's read of TXPIC_INT_REG, which
 * self-clears it (see xframe_pif_read), so it delivers one serviced interrupt
 * per period rather than storming. It NEVER touches GPIO_INT_REG or the MAC
 * link latch, so it never re-runs carrier -- the whole point of feeding the
 * watchdog through a separate source rather than re-raising the link. Re-arms
 * each period while TIMER_EN stays set, unless ONE_SHOT is set -- a fired
 * one-shot clears sched_int_armed rather than re-arming, so a migrated-in
 * target does not wrongly re-fire it (xframe_post_load). A disable
 * (SCHED_INT_CTRL TIMER_EN cleared), soft reset, or ADAPTER_EN 1->0 cancels the
 * timer outright; this TIMER_EN guard covers a tick already in flight when that
 * happens.
 */
static void xframe_sched_int_cb(void *opaque)
{
    XframeState *s = opaque;

    if (!(s->sched_int_ctrl & XFRAME_SCHED_INT_CTRL_TIMER_EN)) {
        return;
    }
    s->sched_int_pending = true;
    xframe_update_irq(s);
    if (!(s->sched_int_ctrl & XFRAME_SCHED_INT_CTRL_ONE_SHOT)) {
        xframe_sched_int_arm(s);
    } else {
        s->sched_int_armed = false;
    }
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
    s->mac_rmac_err = 0;
    s->gpio_int_reg = 0;
    s->sched_int_ctrl = 0;
    s->sched_int_pending = false;
    s->sched_int_armed = false;
    s->mac_cfg = 0;
    s->rmac_cfg_unlocked = false;
    s->rmac_addr_cmd = 0;
    s->rmac_addr_data0 = 0;
    s->rmac_addr_data1 = 0;
    memset(s->mac_addr, 0, sizeof(s->mac_addr));
    memset(s->mac_addr_mask, 0, sizeof(s->mac_addr_mask));
    memset(s->txdl_par, 0, sizeof(s->txdl_par));
    s->tx_traffic_int = 0;
    s->prc_rxd0_0 = 0;
    s->prc_ctrl_0 = 0;
    s->rx_traffic_int = 0;
    s->rx_page_addr = 0;
    s->rx_desc_idx = 0;
    s->general_int_mask = XFRAME_INT_MASK_RESET;
    s->pic_int_mask = XFRAME_INT_MASK_RESET;
    s->txpic_int_mask = XFRAME_INT_MASK_RESET;
    s->rxpic_int_mask = XFRAME_INT_MASK_RESET;
    s->gpio_int_mask = XFRAME_INT_MASK_RESET;
    s->tx_traffic_mask = XFRAME_INT_MASK_RESET;
    s->rx_traffic_mask = XFRAME_INT_MASK_RESET;
    s->mac_int_mask = XFRAME_INT_MASK_RESET;
    s->mac_rmac_err_mask = XFRAME_INT_MASK_RESET;
    /*
     * Reset tears the ring down, so a recovery poll left running from before
     * has nothing left to recover -- stop it rather than let it keep firing
     * against a ring that no longer exists in any guest-meaningful sense.
     */
    timer_del(s->rx_recovery_timer);
    /*
     * A reset drops the adapter (ADAPTER_EN cleared above), so any pending
     * link event is void -- the driver re-enables and the link comes up
     * afresh. Cancel both bring-up one-shots (the DOWN edge and the UP edge it
     * arms) and the scheduled interrupt rather than let a stale event fire
     * against a device the guest just took down.
     */
    timer_del(s->link_down_timer);
    timer_del(s->link_up_timer);
    timer_del(s->sched_int_timer);
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
    case XFRAME_TX_TRAFFIC_INT:
        return s->tx_traffic_int;
    case XFRAME_RX_TRAFFIC_INT:
        return s->rx_traffic_int;
    case XFRAME_MAC_RMAC_ERR_REG:
        return s->mac_rmac_err;
    case XFRAME_PRC_RXD0_0:
        return s->prc_rxd0_0;
    case XFRAME_PRC_CTRL_0:
        return s->prc_ctrl_0;
    case XFRAME_GENERAL_INT_STATUS: {
        /*
         * The top-level summary both public drivers -- and IRIX xg -- read
         * before touching any per-block register (OpenBSD xge_intr, Linux
         * s2io_isr: "reason = readq(&bar0->general_int_status)"). Two
         * load-bearing properties:
         *
         * Derived, never stored: each aggregate bit rolls up from the same
         * block-gated pending signal that drives INTx (a block shows in the
         * summary exactly when it has an unmasked pending source), so the
         * summary can never disagree with the block registers or with the line.
         *
         * Raw -- NOT masked by GENERAL_INT_MASK. This is the corrected
         * semantics: GENERAL_INT_STATUS is the raw block-pending aggregate. The
         * live IRIX xg diagnostic refuted the earlier "summary-visibility mask"
         * model: with INTx force-asserted, xg's ISR read this register 379,815
         * times and got 0 every time, because the model returned
         * "status & ~general_int_mask" and xg leaves general_int_mask at its
         * all-ones reset -- so xg could never identify the source and stormed.
         * xg reads this summary to dispatch and must see the pending block
         * through it. OpenBSD and Linux write general_int_mask = 0 (or clear
         * the aggregate bits they service), so "& ~mask" was already a no-op
         * for them: dropping it does not regress them; it only stops blanking
         * the summary for a driver that, correctly, leaves the general mask
         * alone.
         */
        uint64_t status = 0;

        if ((s->rx_traffic_int & ~s->rx_traffic_mask) != 0) {
            status |= XFRAME_GEN_INTR_RXTRAFFIC;
        }
        if ((s->tx_traffic_int & ~s->tx_traffic_mask) != 0) {
            status |= XFRAME_GEN_INTR_TXTRAFFIC;
        }
        if (xframe_link_pending(s)) {
            status |= XFRAME_GEN_INTR_RXMAC;
        }
        if (xframe_gpio_link_pending(s) || xframe_sched_int_pending(s)) {
            status |= XFRAME_GEN_INTR_TXPIC;
        }
        return status;
    }
    case XFRAME_GENERAL_INT_MASK:
        return s->general_int_mask;
    case XFRAME_PIC_INT_STATUS: {
        /*
         * The PIC block's status summary, one level below GENERAL_INT_STATUS:
         * the ISR reads it to find which PIC sub-block is asserting before
         * descending to that sub-block's own register (Linux
         * s2io_txpic_intr_handle reads pic_int_status, then, on PIC_INT_GPIO,
         * gpio_int_reg). Derived, never stored -- like GENERAL_INT_STATUS: it
         * shows PIC_INT_GPIO exactly when the GPIO link source is pending and
         * unmasked, and PIC_INT_TX when the scheduled interrupt is, so it cannot
         * drift from the registers that clear them. The GPIO sub-block and the
         * TXPIC sub-block (the scheduled interrupt) are the two modelled PIC
         * sources; the others (rxpic/flash/mdio/iic) raise nothing here.
         */
        uint64_t status = 0;

        if (xframe_gpio_link_pending(s)) {
            status |= XFRAME_PIC_INT_GPIO;
        }
        if (xframe_sched_int_pending(s)) {
            status |= XFRAME_PIC_INT_TX;
        }
        return status;
    }
    case XFRAME_TXPIC_INT_REG:
        /*
         * The PCI-X transmit-error status register (ECC/FSM error bits,
         * Linux s2io-regs.h PCIX_INT_REG_*), the first PIC sub-source. No PCI-X
         * transmit error is raised, but this leaf is also where the scheduled
         * interrupt surfaces: when the periodic timer has fired, it reads the
         * scheduled-interrupt latch bit. IRIX xg reads this register while
         * walking the PIC block on GEN_INTR_TXPIC (the round-6 trace's 0x810
         * read), and that read acks the scheduled interrupt (see the clear on
         * read in xframe_pif_read) -- the same self-clearing service the GPIO
         * link source gets. It reads a clean 0 when nothing is pending, so xg's
         * per-ISR probe on an idle link is not a log line.
         */
        return s->sched_int_pending ? XFRAME_TXPIC_INT_SCHED : 0;
    case XFRAME_SCHED_INT_CTRL:
        return s->sched_int_ctrl;
    case XFRAME_GPIO_INT_REG:
        return s->gpio_int_reg;
    case XFRAME_GPIO_INT_MASK:
        return s->gpio_int_mask;
    case XFRAME_PIC_INT_MASK:
        return s->pic_int_mask;
    case XFRAME_TXPIC_INT_MASK:
        return s->txpic_int_mask;
    case XFRAME_RXPIC_INT_MASK:
        return s->rxpic_int_mask;
    case XFRAME_TX_TRAFFIC_MASK:
        return s->tx_traffic_mask;
    case XFRAME_RX_TRAFFIC_MASK:
        return s->rx_traffic_mask;
    case XFRAME_MAC_INT_MASK:
        return s->mac_int_mask;
    case XFRAME_MAC_RMAC_ERR_MASK:
        return s->mac_rmac_err_mask;
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
    hwaddr reg = addr & ~7ULL;
    uint64_t val = xframe_pif_read_reg(s, reg);

    if (size == 4 && (addr & 4)) {
        val >>= 32;
    }

    /*
     * The PIC/GPIO link-up interrupt is a self-clearing edge, acknowledged by
     * the ISR's READ of GPIO_INT_REG -- the measured IRIX xg clear. The round-6
     * live boot is decisive: xg's ISR reads GENERAL_INT_STATUS (seeing
     * GEN_INTR_TXPIC), reads the txpic sub-source (0x810, empty), reads
     * GPIO_INT_REG (LINK_UP), and then issues NO write-1-to-clear of
     * GPIO_INT_REG and NO per-interrupt write to PIC_INT_MASK/GPIO_INT_MASK --
     * only a GENERAL_INT_MASK mask/restore toggle, which gates nothing here.
     * Its sole acknowledgement of the link event is that read. Modelling
     * GPIO_INT_REG as a plain W1C latch left the source asserted after the
     * read, so xg re-entered its ISR ~183,869 times in ~12 s and never
     * transmitted. So the read consumes the edge: it returns the latched link
     * bits (the driver sees LINK_UP), then clears them and recomputes the line,
     * dropping INTx. One link-up delivers one serviced interrupt that quiesces;
     * a genuinely new link event latches afresh. Write-1-to-clear
     * (xframe_pif_write_reg) remains a valid clear too -- OpenBSD-style W1C and
     * any explicit ack still drop the source.
     *
     * The link bits (LINK_UP/LINK_DOWN) sit in the high 32 bits, so only a read
     * that covers the high half consumes the edge: the low-half read xg issues
     * for the byte-swapped register returns 0 and clears nothing, so a
     * low-then-high half pair still lets the high-half read see LINK_UP before
     * clearing it. This runs only on a genuine MMIO read, never on the
     * read-modify-write compose path (xframe_pif_write goes through
     * xframe_pif_read_reg), so composing a write onto GPIO_INT_REG cannot
     * spuriously ack.
     */
    if (reg == XFRAME_GPIO_INT_REG && (size == 8 || (addr & 4)) &&
        (s->gpio_int_reg & XFRAME_GPIO_INT_LINK_BITS)) {
        s->gpio_int_reg &= ~XFRAME_GPIO_INT_LINK_BITS;
        xframe_update_irq(s);
    }

    /*
     * The scheduled interrupt is the same kind of self-clearing edge: xg reads
     * TXPIC_INT_REG (0x810) while walking the PIC block and issues no explicit
     * clear, so the read consumes the latch and drops INTx -- one serviced
     * interrupt per period, not a storm. Its latch bit (XFRAME_TXPIC_INT_SCHED)
     * sits in the high 32 bits, so only a read covering the high half consumes
     * it, exactly as the GPIO link-bit ack above; and like it, this runs only on
     * a real MMIO read, never on the write-compose path.
     */
    if (reg == XFRAME_TXPIC_INT_REG && (size == 8 || (addr & 4)) &&
        s->sched_int_pending) {
        s->sched_int_pending = false;
        xframe_update_irq(s);
    }

    /*
     * The MAC-block link-state-change latch is a self-clearing edge too when
     * the source is MASKED -- the case IRIX xg leaves it in. OpenBSD unmasks
     * the MAC link path and acks the latch by write-1-to-clear, so while it is
     * unmasked the read stays non-destructive and W1C is the ack (see the W1C
     * path in xframe_pif_write_reg, and test_link_up_raises_interrupt). But xg
     * takes its link interrupt through the PIC/GPIO block and leaves the MAC
     * path masked, yet its alarm handler still READS MAC_RMAC_ERR_REG (0x2028)
     * on every scheduled-interrupt pass to test bit 32. A W1C level xg never
     * writes would read set on every pass, so xg would see the settled link
     * "change" again and again and flap carrier between passes. The driver will
     * not W1C a source it masked, so its status read is the only ack it gives:
     * the read returns the bit (the alarm handler sees the one real change) and
     * then consumes the latch, so the next pass finds nothing. Only a read
     * covering the high 32 bits (where bit 32 sits) consumes it, like the GPIO
     * and scheduled-interrupt acks above; and like them this runs only on a
     * real MMIO read, never on the write-compose path (which uses
     * xframe_pif_read_reg). The source being masked means clearing it never
     * moves INTx, so xframe_update_irq only keeps the recompute uniform.
     */
    if (reg == XFRAME_MAC_RMAC_ERR_REG && (size == 8 || (addr & 4)) &&
        (s->mac_rmac_err & XFRAME_RMAC_LINK_STATE_CHANGE_INT) &&
        !xframe_link_pending(s)) {
        s->mac_rmac_err &= ~XFRAME_RMAC_LINK_STATE_CHANGE_INT;
        xframe_update_irq(s);
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
        /*
         * The driver bringing the adapter up (ADAPTER_EN 0->1) is what starts
         * the link negotiating; model the link as a DOWN->UP transition a short
         * time later by arming the link-DOWN one-shot below (which arms the UP
         * edge in turn). Only the rising edge arms it, so the ISR's own re-enable
         * on link-restore (OpenBSD xge_intr calls xge_enable, which rewrites
         * ADAPTER_EN while it is already set) does not re-arm and cannot storm.
         * The scheduled interrupt that keeps the watchdog fed is NOT armed here
         * -- the driver arms it explicitly by writing SCHED_INT_CTRL during
         * bring-up. The falling edge cancels both bring-up one-shots and the
         * scheduled interrupt -- the driver disabling the adapter without a full
         * reset must not leave a stale link edge or a periodic interrupt firing
         * for a device it just took down. It also clears any link source
         * already latched but not yet acked (GPIO_INT_REG's LINK_UP/LINK_DOWN
         * bits, mac_rmac_err's RMAC_LINK_STATE_CHANGE_INT), matching how the
         * read-ack paths clear them (XFRAME_GPIO_INT_REG and
         * XFRAME_MAC_RMAC_ERR_REG read handling above): delivery of these
         * sources is not gated by ADAPTER_EN, so leaving one latched would
         * keep INTx asserted for a disabled adapter, and leaving LINK_UP
         * latched would let a later re-enable's DOWN edge OR it onto a fresh
         * LINK_DOWN -- the unstable both-bits state this model otherwise never
         * presents (see xframe_link_up_timer_cb).
         */
        if ((full & XFRAME_ADAPTER_EN) &&
            !(s->adapter_control & XFRAME_ADAPTER_EN)) {
            timer_mod(s->link_down_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      XFRAME_LINK_UP_DELAY_MS);
        } else if (!(full & XFRAME_ADAPTER_EN) &&
                   (s->adapter_control & XFRAME_ADAPTER_EN)) {
            timer_del(s->link_down_timer);
            timer_del(s->link_up_timer);
            timer_del(s->sched_int_timer);
            s->sched_int_pending = false;
            s->sched_int_armed = false;
            s->gpio_int_reg &= ~XFRAME_GPIO_INT_LINK_BITS;
            s->mac_rmac_err &= ~XFRAME_RMAC_LINK_STATE_CHANGE_INT;
        }
        s->adapter_control = full;
        xframe_update_irq(s);
        return;
    case XFRAME_SWAPPER_CTRL:
        s->swapper_ctrl = full;
        return;
    case XFRAME_XMSI_ADDRESS:
        s->xmsi_address = full;
        return;
    case XFRAME_SCHED_INT_CTRL:
        /*
         * The scheduled/periodic-interrupt timer control. The driver arms the
         * watchdog feed here at bring-up: writing TIMER_EN (with a period the
         * model does not read, since the hardware tick is TBD publicly) starts
         * the periodic timer; clearing TIMER_EN stops it and drops any pending
         * scheduled interrupt. TIMER_EN sits in the high half, so act on it only
         * when the write reaches that half (a low-half period-only write just
         * updates the stored value). Not gated on ADAPTER_EN: the bring-up
         * sequence enables the adapter before programming this, and a live tick
         * on a disabled adapter is cancelled by the ADAPTER_EN 1->0 path above.
         */
        s->sched_int_ctrl = full;
        if (commit_high) {
            if (full & XFRAME_SCHED_INT_CTRL_TIMER_EN) {
                xframe_sched_int_arm(s);
            } else {
                timer_del(s->sched_int_timer);
                s->sched_int_pending = false;
                s->sched_int_armed = false;
                xframe_update_irq(s);
            }
        }
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
    case XFRAME_TX_TRAFFIC_INT:
        /*
         * Write-1-to-clear, the transmit mirror of the receive ACK below:
         * the ISR reads the pending completions and writes them back
         * (OpenBSD xge_intr: "if ((val = PIF_RCSR(TX_TRAFFIC_INT)))
         * PIF_WCSR(TX_TRAFFIC_INT, val);"; Linux s2io_isr writes
         * S2IO_MINUS_ONE to tx_traffic_int). Clear on the bits this access
         * carries, not the composed value, so a 32-bit half-write cannot
         * clear the untouched half's pending completions.
         */
        s->tx_traffic_int &= ~written;
        xframe_update_irq(s);
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
    case XFRAME_MAC_RMAC_ERR_REG:
        /*
         * Write-1-to-clear, the same ACK idiom as RX_TRAFFIC_INT: the ISR
         * reads the link-state-change bit and writes it back to clear it
         * (OpenBSD xge_intr: "PIF_WCSR(MAC_RMAC_ERR_REG,
         * RMAC_LINK_STATE_CHANGE_INT)"). Clear on the bits this access
         * carries, not the composed value, so a 32-bit half-write cannot
         * clear the untouched half's bits.
         */
        s->mac_rmac_err &= ~written;
        xframe_update_irq(s);
        return;
    case XFRAME_GPIO_INT_REG:
        /*
         * Write-1-to-clear, the PIC/GPIO-block link status -- a secondary clear
         * path. IRIX xg acks the link-up edge by READING this register, not by
         * writing it (see xframe_pif_read's clear-on-read); Linux
         * s2io_txpic_intr_handle acks a plain LINK_UP by masking GPIO_INT_MASK,
         * and writes the bits back here only in its unstable both-up-and-down
         * case. This W1C keeps that explicit-write path and any OpenBSD-style
         * ack working. Clear on the bits this access carries, not the composed
         * value, so a 32-bit half-write cannot clear the untouched half's bits.
         */
        s->gpio_int_reg &= ~written;
        xframe_update_irq(s);
        return;
    case XFRAME_TXPIC_INT_REG:
        /*
         * The IRIX xg ISR acks the scheduled interrupt by WRITING this leaf
         * (it dispatches off GENERAL_INT_STATUS's GEN_INTR_TXPIC and never
         * reads the leaf), so a write covering the high half consumes the
         * latch -- the write-path counterpart of the read-based self-clear
         * in xframe_pif_read. One serviced interrupt per period, not a storm.
         */
        if (commit_high && s->sched_int_pending) {
            s->sched_int_pending = false;
            xframe_update_irq(s);
        }
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
        /*
         * Stored for read-back and migration only: it gates neither INTx nor
         * the GENERAL_INT_STATUS summary read (that summary is the raw
         * block-pending aggregate -- see the read handler). Drivers write and
         * read it back (OpenBSD writes 0; Linux blanks it while servicing), but
         * nothing the model computes depends on it. No xframe_update_irq.
         */
        s->general_int_mask = full;
        return;
    case XFRAME_TX_TRAFFIC_MASK:
        /* Second-level mask: gates TX_TRAFFIC_INT -> INTx. Recompute. */
        s->tx_traffic_mask = full;
        xframe_update_irq(s);
        return;
    case XFRAME_RX_TRAFFIC_MASK:
        /* Second-level mask: gates RX_TRAFFIC_INT -> INTx. Recompute. */
        s->rx_traffic_mask = full;
        xframe_update_irq(s);
        return;
    case XFRAME_MAC_INT_MASK:
        /* Gates the RMAC block roll-up for the link source. Recompute. */
        s->mac_int_mask = full;
        xframe_update_irq(s);
        return;
    case XFRAME_MAC_RMAC_ERR_MASK:
        /* Gates the leaf link-state-change bit. Recompute. */
        s->mac_rmac_err_mask = full;
        xframe_update_irq(s);
        return;
    case XFRAME_PIC_INT_MASK:
        /*
         * The PIC block mask. Its PIC_INT_GPIO bit gates the GPIO link source's
         * roll-up to the line (see xframe_gpio_link_pending) -- this is the
         * block IRIX xg opens (writes PIC_INT_MASK = 0) -- so recompute. Its
         * other sub-block bits (txpic/rxpic/flash/mdio/iic) gate no modelled
         * source; they are held for read-back only.
         */
        s->pic_int_mask = full;
        xframe_update_irq(s);
        return;
    case XFRAME_GPIO_INT_MASK:
        /*
         * The GPIO leaf mask. Real hardware gates the GPIO link leaf with it,
         * but this model delivers the GPIO link source through PIC_INT_MASK's
         * PIC_INT_GPIO roll-up alone (see xframe_gpio_link_pending), because
         * IRIX xg leaves this leaf masked yet must still take the interrupt. So
         * it gates no modelled source and is held for read-back only; no
         * xframe_update_irq.
         */
        s->gpio_int_mask = full;
        return;
    case XFRAME_TXPIC_INT_MASK:
        /*
         * The TXPIC (PCI-X transmit) sub-block leaf mask. Its PCI-X-transmit-
         * error bits gate no modelled error source, but its XFRAME_TXPIC_INT_SCHED
         * bit gates the scheduled interrupt's leaf (see xframe_sched_int_pending)
         * -- xg unmasks this leaf (writes 0) precisely to open that source -- so
         * recompute. Its other bits are held for read-back.
         */
        s->txpic_int_mask = full;
        xframe_update_irq(s);
        return;
    case XFRAME_RXPIC_INT_MASK:
        s->rxpic_int_mask = full;
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
 * Transmit checksum offload, in the second control word. The driver hands the
 * card a frame whose IPv4, TCP or UDP checksum field is not yet valid and asks
 * for it to be inserted here (Linux s2io TXD_TX_CKO_IPV4_EN, _TCP_EN, _UDP_EN,
 * the driver's own MSB-first bit numbering: bits 5, 6 and 7).
 */
#define XFRAME_TXD_CKO_IPV4         (1ULL << 58)
#define XFRAME_TXD_CKO_TCP          (1ULL << 57)
#define XFRAME_TXD_CKO_UDP          (1ULL << 56)

#define XFRAME_ETH_HDR_LEN          14
#define XFRAME_ETHERTYPE_IPV4       0x0800
#define XFRAME_IPV4_MIN_HDR_LEN     20

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
 *
 * Descriptor control words are decoded through xframe_desc_ldq/xframe_desc_stq
 * below, which honour SWAPPER_CTRL's per-path _SE bit: big-endian while it is
 * clear (the reset default -- Linux's own comment: "the device by default set
 * to a big endian format, so a big endian driver need not set anything" --
 * s2io_set_swapper; OpenBSD's xge_attach likewise skips the SWAPPER_CTRL write
 * on a BE host), little-endian once a guest programs it. The Origin 200's
 * IRIX/OpenBSD-mips guest is always big-endian and never programs the swapper
 * away from that default, so decode stays effectively unconditional on this
 * machine; a little-endian guest (Linux s2io / OpenBSD xge on an LE host)
 * that does set the swap bit gets the order it asked for. Frame payload DMA
 * below stays a raw byte copy -- packet data carries no word order to get
 * wrong.
 */

/*
 * SWAPPER_CTRL's per-DMA-path swap-enable (SE) bits that pick descriptor byte
 * order (Linux s2io-regs.h SWAPPER_CTRL_TXD_R_SE = s2BIT(19) = bit 44,
 * SWAPPER_CTRL_RXD_R_SE = s2BIT(33) = bit 30; s2io.h's s2BIT(loc) is
 * 0x8000000000000000ULL >> loc, the same MSB-first numbering as the interrupt
 * bits above). s2io_set_swapper's __BIG_ENDIAN branch sets the TxD/RxD _FE
 * bits with the _SE bits CLEAR; its little-endian (#else) branch sets both
 * _FE and _SE for the same paths -- so the _SE bit alone selects order, and
 * it agrees with SWAPPER_CTRL's all-zero reset value (big-endian), which a
 * big-endian driver never disturbs. Only the _R_SE bit is checked, not the
 * separate _W_SE bit the same driver also sets: both branches of
 * s2io_set_swapper OR a descriptor type's _R_SE and _W_SE into the same
 * writeq together, so one stored bit per type tracks reads and write-backs
 * alike.
 */
#define XFRAME_SWAPPER_CTRL_TXD_R_SE  (1ULL << 44)
#define XFRAME_SWAPPER_CTRL_RXD_R_SE  (1ULL << 30)

/*
 * Decode/encode one descriptor control word in the byte order SWAPPER_CTRL
 * selects for that descriptor type. Every TxD/RxD control-word accessor in
 * xframe_tx_doorbell, xframe_rx_advance_page, xframe_rx_head and
 * xframe_receive routes through these; frame-payload DMA does not.
 */
static uint64_t xframe_desc_ldq(XframeState *s, const uint8_t *p, bool rxd)
{
    uint64_t se = rxd ? XFRAME_SWAPPER_CTRL_RXD_R_SE : XFRAME_SWAPPER_CTRL_TXD_R_SE;

    return (s->swapper_ctrl & se) ? ldq_le_p(p) : ldq_be_p(p);
}

static void xframe_desc_stq(XframeState *s, uint8_t *p, uint64_t val, bool rxd)
{
    uint64_t se = rxd ? XFRAME_SWAPPER_CTRL_RXD_R_SE : XFRAME_SWAPPER_CTRL_TXD_R_SE;

    if (s->swapper_ctrl & se) {
        stq_le_p(p, val);
    } else {
        stq_be_p(p, val);
    }
}

/*
 * net_checksum_calculate believes the lengths in the frame it is handed, and
 * the guest composed them: a header length under the twenty-byte minimum, or
 * lengths reaching past what was gathered, walks it off the end of the buffer.
 * A descriptor asking for insertion is not proof the frame is the IPv4 packet
 * the request claims, so check before offering it one.
 */
static bool xframe_tx_frame_is_ipv4(const uint8_t *frame, size_t len)
{
    unsigned ihl, total;

    if (len < XFRAME_ETH_HDR_LEN + XFRAME_IPV4_MIN_HDR_LEN) {
        return false;
    }
    if (lduw_be_p(frame + 12) != XFRAME_ETHERTYPE_IPV4) {
        return false;
    }
    if ((frame[XFRAME_ETH_HDR_LEN] >> 4) != 4) {
        return false;
    }
    ihl = (frame[XFRAME_ETH_HDR_LEN] & 0x0f) * 4;
    total = lduw_be_p(frame + XFRAME_ETH_HDR_LEN + 2);

    return ihl >= XFRAME_IPV4_MIN_HDR_LEN && total >= ihl &&
           XFRAME_ETH_HDR_LEN + (size_t)total <= len;
}

/* Not gated on ADAPTER_EN: drivers enable the adapter before any doorbell. */
static void xframe_tx_doorbell(XframeState *s, unsigned fifo, uint64_t lcr)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint64_t par = s->txdl_par[fifo];
    unsigned count = ((lcr >> 56) & 0xff) + 1;   /* NUMTXD + 1, 1..256 */
    uint64_t first_control2 = 0;
    size_t frame_len = 0;
    bool have_first = false;
    unsigned i;

    for (i = 0; i < count; i++) {
        uint64_t txd_addr = par + (uint64_t)i * XFRAME_TXD_SIZE;
        uint8_t raw[XFRAME_TXD_SIZE];
        uint8_t word[8];
        uint64_t control1, bufaddr;
        unsigned seg_len;

        if (pci_dma_read(pdev, txd_addr, raw, sizeof(raw)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xframe: transmit descriptor read at 0x%" PRIx64
                          " failed; list abandoned\n", txd_addr);
            return;
        }
        control1 = xframe_desc_ldq(s, raw, false);
        bufaddr = xframe_desc_ldq(s, raw + 16, false);
        seg_len = control1 & XFRAME_TXD_BUF_LEN_MASK;

        if (i == 0) {
            if (!(control1 & XFRAME_TXD_OWN)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "xframe: transmit doorbell with a descriptor the "
                              "card does not own; ignored\n");
                return;
            }
            first_control2 = xframe_desc_ldq(s, raw + 8, false);
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

        /*
         * Give this descriptor back before the frame goes out, and give back
         * EVERY descriptor the card walks rather than only the list's first.
         * A driver reclaims transmit descriptors by their ownership bit, so
         * one left owned by the card is one its ring never recovers: holding
         * the gathered descriptors drained IRIX's xg ring after about a dozen
         * frames and stopped it transmitting altogether.
         */
        xframe_desc_stq(s, word, control1 & ~XFRAME_TXD_OWN, false);
        pci_dma_write(pdev, txd_addr, word, sizeof(word));

        if ((control1 & XFRAME_TXD_GCL) || frame_len >= XFRAME_MAX_FRAME) {
            break;  /* last segment of this frame */
        }
    }

    if (frame_len > 0) {
        int csum = 0;

        if (first_control2 & XFRAME_TXD_CKO_IPV4) {
            csum |= CSUM_IP;
        }
        if (first_control2 & XFRAME_TXD_CKO_TCP) {
            csum |= CSUM_TCP;
        }
        if (first_control2 & XFRAME_TXD_CKO_UDP) {
            csum |= CSUM_UDP;
        }
        if (csum && !xframe_tx_frame_is_ipv4(s->tx_frame, frame_len)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xframe: checksum insertion asked for a frame that "
                          "is not a well-formed IPv4 packet; sent unchanged\n");
            csum = 0;
        }
        if (csum) {
            net_checksum_calculate(s->tx_frame, frame_len, csum);
        }
        qemu_send_packet(qemu_get_queue(s->nic), s->tx_frame, frame_len);
    }

    /*
     * Signal the transmit completion for this FIFO once the list is done and
     * the frame is on the wire: set the FIFO's TX_TRAFFIC_INT bit and
     * recompute the line. Raised only when a descriptor was actually
     * processed -- the early returns above (unowned or unreadable descriptor)
     * complete nothing. Both drivers depend on this to reclaim transmit
     * descriptors: OpenBSD's xge_intr and Linux's s2io_isr walk the sent
     * descriptors on TX_TRAFFIC_INT. Setting device state after the send
     * keeps a synchronous loopback receive from observing a half-updated
     * doorbell, the same ordering the descriptor release above uses.
     */
    if (have_first) {
        s->tx_traffic_int |= XFRAME_TX_TRAFFIC_FIFO(fifo);
        xframe_update_irq(s);
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
    s->rx_page_addr = xframe_desc_ldq(s, raw, true);
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

    return (xframe_desc_ldq(s, raw + 8, true) & XFRAME_RXD_OWN) != 0;
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
 * own descriptor. RxD control words decode big-endian for the same reason
 * TxD's do -- see xframe_tx_doorbell's comment.
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

    control2 = xframe_desc_ldq(s, raw + 16, true);
    bufaddr = xframe_desc_ldq(s, raw + 24, true);
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

    xframe_desc_stq(s, word, (uint64_t)copy_len << XFRAME_RXD_BUFSIZ_SHIFT, true);
    pci_dma_write(pdev, desc_addr + 16, word, sizeof(word));

    xframe_desc_stq(s, word, (uint64_t)xcode << XFRAME_RXD_XCODE_SHIFT, true);
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

/*
 * QEMU's own netdev backend link, not the modelled bring-up sequence above:
 * this is the standard NetClientInfo hook a `set_link` QMP command (or any
 * other backend disconnect) invokes on NetClientState::link_down changing.
 * Without it the device never looked at that flag at all -- it announced
 * LINK_UP on the ADAPTER_EN bring-up edge regardless of whether the backend
 * was actually connected, so an operator-downed netdev still showed carrier
 * to the guest.
 *
 * Backend down: present LINK_DOWN on the same PIC/GPIO path the bring-up
 * sequence uses, and cancel a pending bring-up edge -- link_down_timer if the
 * DOWN hasn't latched yet, link_up_timer if it has and the UP edge is still
 * armed -- so neither one-shot completes a transition to UP behind a backend
 * that just went away. (xframe_link_up_timer_cb's own link_down check above
 * covers the one case this cancellation cannot reach: a backend already down
 * before ADAPTER_EN's rising edge armed the sequence in the first place.)
 * This also raises the MAC-block link-state-change latch the way a genuine
 * link event does, for a driver (OpenBSD xge) that takes its link interrupt
 * through that path instead of the PIC/GPIO one.
 *
 * Backend back up while the adapter is enabled: re-run the bring-up
 * DOWN->UP transition from its first edge, the same one-shot ADAPTER_EN's
 * 0->1 edge arms (xframe_pif_write_reg's XFRAME_ADAPTER_CONTROL case) -- a
 * driver already running needs a genuine transition to notice the link is
 * back, not a level. Only while the adapter is enabled: a backend recovering
 * while the guest has not enabled the adapter has nothing to notice yet, and
 * the driver's own bring-up arms the sequence when it does.
 */
static void xframe_set_link_status(NetClientState *nc)
{
    XframeState *s = XFRAME(qemu_get_nic_opaque(nc));

    if (nc->link_down) {
        timer_del(s->link_down_timer);
        timer_del(s->link_up_timer);
        s->gpio_int_reg = (s->gpio_int_reg & ~XFRAME_GPIO_INT_REG_LINK_UP) |
                          XFRAME_GPIO_INT_REG_LINK_DOWN;
        s->mac_rmac_err |= XFRAME_RMAC_LINK_STATE_CHANGE_INT;
        xframe_update_irq(s);
    } else if (s->adapter_control & XFRAME_ADAPTER_EN) {
        timer_mod(s->link_down_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  XFRAME_LINK_UP_DELAY_MS);
    }
}

static NetClientInfo xframe_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = xframe_can_receive,
    .receive = xframe_receive,
    .link_status_changed = xframe_set_link_status,
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
    s->link_down_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                      xframe_link_down_timer_cb, s);
    s->link_up_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                    xframe_link_up_timer_cb, s);
    s->sched_int_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                      xframe_sched_int_cb, s);

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
    timer_free(s->link_down_timer);
    timer_free(s->link_up_timer);
    timer_free(s->sched_int_timer);
    qemu_del_nic(s->nic);
}

/*
 * A target that loads with the scheduled interrupt armed must keep it coming,
 * or a driver watchdog resumes resetting the device as if this migrated in
 * stale. VMSTATE_TIMER_PTR_V above restores the timer's own pending expiry
 * exactly when the source had it mid-period at save time; re-arm only covers
 * the gap that restore cannot -- a save taken with nothing pending on the
 * timer (between periods, or any other case where it was armed but not
 * outstanding) -- by starting it fresh whenever the driver had it armed and
 * the restore left nothing running.
 *
 * Gated on sched_int_armed, not merely SCHED_INT_CTRL's TIMER_EN bit: TIMER_EN
 * stays set across a ONE_SHOT interrupt firing (xframe_sched_int_cb's ONE_SHOT
 * branch does not clear it), so a TIMER_EN-only check would wrongly re-arm an
 * already-completed one-shot on every future load. sched_int_armed instead
 * tracks whether an arm is actually outstanding: xframe_sched_int_arm sets it
 * on every arm (bring-up and periodic re-arm alike, so it stays true across
 * periods), the ONE_SHOT branch clears it exactly when it declines to re-arm,
 * and every path that cancels the timer (SCHED_INT_CTRL clearing TIMER_EN,
 * ADAPTER_EN 1->0, soft reset) clears it too -- so a migrated target re-arms
 * only when the source device actually still had one pending.
 *
 * Also gated on !timer_pending(s->sched_int_timer): VMSTATE_TIMER_PTR_V has
 * already restored that exact pending expiry by the time post_load runs, so
 * re-arming unconditionally would discard it for now + period, shifting the
 * next interrupt later than the source's. Re-arming only when nothing is
 * pending leaves a migrated-in pending timer on its restored deadline and
 * still starts one for the armed-but-not-pending gap above.
 */
static int xframe_post_load(void *opaque, int version_id)
{
    XframeState *s = opaque;

    /*
     * A version-1 stream predates this branch's per-block interrupt masks
     * (VMSTATE_UINT64_V at version 2 below): v1 gated every source through
     * the single GENERAL_INT_MASK, so a v1 source never programmed -- and
     * this VMState never carried -- any of the eight masks that
     * xframe_update_irq (via xframe_link_pending, xframe_gpio_link_pending
     * and xframe_sched_int_pending) actually gates on: pic_int_mask,
     * txpic_int_mask, rxpic_int_mask, gpio_int_mask, tx_traffic_mask,
     * rx_traffic_mask, mac_int_mask and mac_rmac_err_mask. Left unset, a v1
     * load leaves them at this struct's reset value (XFRAME_INT_MASK_RESET,
     * all-ones/masked), and xframe_update_irq now gates every source through
     * exactly these masks -- so a migrated v1 guest would stop seeing
     * interrupts even though its general mask still had them open. The
     * compatible v2 state for a v1 source is all eight open (0): the
     * guest's driver is already past bring-up and does not revisit its
     * masks afterward, so the model must present them open itself rather
     * than wait for a rewrite that never comes.
     */
    if (version_id < 2) {
        s->pic_int_mask = 0;
        s->txpic_int_mask = 0;
        s->rxpic_int_mask = 0;
        s->gpio_int_mask = 0;
        s->tx_traffic_mask = 0;
        s->rx_traffic_mask = 0;
        s->mac_int_mask = 0;
        s->mac_rmac_err_mask = 0;
    }

    if (s->sched_int_armed && !timer_pending(s->sched_int_timer)) {
        xframe_sched_int_arm(s);
    }
    return 0;
}

/*
 * The migratable register state, per docs/datasheets/xframe.md's "VMState /
 * migration fields". Version 1 (the base this branch started from) covers the
 * byte-swap and scratch registers, ADAPTER_CONTROL, GENERAL_INT_MASK, MAC_CFG
 * and its unlock latch, the whole MAC-address table and its parallel
 * per-entry mask (entry 0's factory address plus any programmed unicast or
 * multicast entry -- whatever the guest wrote must survive migration
 * exactly), the per-FIFO pending transmit list pointer, and the receive
 * ring's own bookkeeping (PRC_RXD0_0/PRC_CTRL_0, the pending RX_TRAFFIC_INT
 * status, and the card's current walk position through the ring).
 *
 * Version 2 is this branch's interrupt-block model, added as one logical
 * bump over that base (VMSTATE_*_V at version 2 below): the per-block
 * interrupt masks (the PCI-X/PIC masks, the transmit- and receive-traffic
 * masks, and the two MAC masks) and the pending TX_TRAFFIC_INT completion
 * status -- these feed xframe_update_irq, so a migrated target that lost them
 * would recompute a wrong INTx line; the PIC/GPIO-block link path IRIX xg uses
 * (gpio_int_reg, the link-state status, and gpio_int_mask, held for read-back
 * only -- the GPIO link source is gated by pic_int_mask's PIC_INT_GPIO bit,
 * migrated alongside it); the bring-up link edge's two one-shots
 * (link_down_timer, link_up_timer) -- a save taken mid-transition must still
 * deliver whichever edge was pending on the target, or a latched DOWN would
 * never be cleared; and the scheduled interrupt that replaced the earlier
 * sustained-link heartbeat -- SCHED_INT_CTRL (the register the driver arms it
 * with), the sched_int_pending latch (which also feeds the INTx recompute),
 * sched_int_armed (whether an arm is actually outstanding -- see
 * xframe_post_load), and the timer's own pending expiry (sched_int_timer),
 * migrated for the same reason link_up_timer is: a target that lost a pending
 * scheduled interrupt would starve the driver watchdog after migration.
 *
 * A version-1 stream loads every version-2 field at this struct's reset
 * value: zero for the latches, pending flags and timers -- nothing pending,
 * nothing armed, the correct state for a target that migrated in from a
 * build with no interrupt model at all -- but XFRAME_INT_MASK_RESET
 * (all-ones/masked) for the eight gating masks, since the struct resets
 * masks closed. xframe_post_load re-opens exactly those eight for a
 * version_id < 2 load (see its comment): the compatible v2 state for a v1
 * source, whose general_int_mask alone gated its interrupts, is these masks
 * open.
 *
 * The two derived summaries (GENERAL_INT_STATUS and PIC_INT_STATUS) need no
 * field of their own: both are recomputed from the RX/TX traffic status, the
 * two link-state sources, the scheduled-interrupt latch and these masks on
 * every read, so migrating those is enough. The netdev frontend's own state
 * lives in NICConf, which every NIC model leaves out of its VMState (see
 * hw/net/sgi-ioc3-eth.c and hw/net/rtl8139.c); PCI config space is covered by
 * VMSTATE_PCI_DEVICE.
 */
static const VMStateDescription vmstate_xframe = {
    .name = "xframe",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = xframe_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, XframeState),
        VMSTATE_UINT64(swapper_ctrl, XframeState),
        VMSTATE_UINT64(xmsi_address, XframeState),
        VMSTATE_UINT64(sw_reset, XframeState),
        VMSTATE_UINT64(adapter_control, XframeState),
        VMSTATE_UINT64(general_int_mask, XframeState),
        VMSTATE_UINT64_V(pic_int_mask, XframeState, 2),
        VMSTATE_UINT64_V(txpic_int_mask, XframeState, 2),
        VMSTATE_UINT64_V(rxpic_int_mask, XframeState, 2),
        VMSTATE_UINT64_V(gpio_int_mask, XframeState, 2),
        VMSTATE_UINT64_V(tx_traffic_mask, XframeState, 2),
        VMSTATE_UINT64_V(rx_traffic_mask, XframeState, 2),
        VMSTATE_UINT64_V(mac_int_mask, XframeState, 2),
        VMSTATE_UINT64_V(mac_rmac_err_mask, XframeState, 2),
        VMSTATE_UINT64_V(mac_rmac_err, XframeState, 2),
        VMSTATE_UINT64_V(gpio_int_reg, XframeState, 2),
        VMSTATE_UINT64(mac_cfg, XframeState),
        VMSTATE_BOOL(rmac_cfg_unlocked, XframeState),
        VMSTATE_UINT64(rmac_addr_cmd, XframeState),
        VMSTATE_UINT64(rmac_addr_data0, XframeState),
        VMSTATE_UINT64(rmac_addr_data1, XframeState),
        VMSTATE_UINT64_ARRAY(mac_addr, XframeState, XFRAME_MAC_TABLE_ENTRIES),
        VMSTATE_UINT64_ARRAY(mac_addr_mask, XframeState,
                             XFRAME_MAC_TABLE_ENTRIES),
        VMSTATE_UINT64_ARRAY(txdl_par, XframeState, XFRAME_TXP_FIFOS),
        VMSTATE_UINT64_V(tx_traffic_int, XframeState, 2),
        VMSTATE_UINT64(prc_rxd0_0, XframeState),
        VMSTATE_UINT64(prc_ctrl_0, XframeState),
        VMSTATE_UINT64(rx_traffic_int, XframeState),
        VMSTATE_UINT64(rx_page_addr, XframeState),
        VMSTATE_UINT32(rx_desc_idx, XframeState),
        VMSTATE_UINT64_V(sched_int_ctrl, XframeState, 2),
        VMSTATE_BOOL_V(sched_int_pending, XframeState, 2),
        VMSTATE_BOOL_V(sched_int_armed, XframeState, 2),
        VMSTATE_TIMER_PTR_V(sched_int_timer, XframeState, 2),
        VMSTATE_TIMER_PTR_V(link_down_timer, XframeState, 2),
        VMSTATE_TIMER_PTR_V(link_up_timer, XframeState, 2),
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
