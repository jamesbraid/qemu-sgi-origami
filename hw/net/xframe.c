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
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/net/xframe.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "standard-headers/linux/virtio_net.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "hw/net/net_tx_pkt.h"

#include "qapi/error.h"
#include "trace.h"

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
#define XFRAME_TTI_COMMAND_MEM      0x1150
#define XFRAME_TTI_DATA1_MEM        0x1158
#define XFRAME_TTI_DATA2_MEM        0x1160
#define XFRAME_RTI_COMMAND_MEM      0x19b8
#define XFRAME_RTI_DATA1_MEM        0x19c0
#define XFRAME_RTI_DATA2_MEM        0x19c8
/*
 * The statistics block (public s2io: stat_cfg / stat_addr in the DCSR bank).
 * STAT_CFG's enable and one-shot sit in the top bits (s2BIT(0)/s2BIT(1), i.e.
 * bits 63 and 62); the update period occupies the low 32 bits in bus clicks,
 * PER_SEC = 0x208d5 clicks to the second.
 */
#define XFRAME_STAT_CFG             0x09d0
#define XFRAME_STAT_ADDR            0x09d8
#define XFRAME_STAT_CFG_EN          (1ULL << 63)
#define XFRAME_STAT_CFG_ONE_SHOT    (1ULL << 62)
#define XFRAME_STAT_PER_SEC         0x208d5ULL

/*
 * The statistics block has its own byte-swap enables, SWAPPER_CTRL_STATS_FE
 * and _SE (public s2io s2BIT(48)/s2BIT(49); s2BIT(n) is 1<<(63-n), so bits 15
 * and 14 of the low word). The two combinations that occur on real drivers are
 * modelled, and only those. Linux sets FE and SE together on both host
 * endiannesses and declares every field __le in place: FE+SE means each field
 * little-endian at its own offset. IRIX leaves both clear, and the block was
 * measured big-endian in place (a little-endian write is read back
 * byte-reversed here, a count of 50 arriving as 0x32000000). FE alone or SE
 * alone -- which nobody sets -- would transpose the halves of each field on the
 * hardware; that is not modelled, and the STAT_CFG enable path logs it. A
 * 64-bit field,
 * were one ever filled, follows the same selector in place (whole-64-bit
 * little- or big-endian), not a half-swap.
 */
#define XFRAME_SWAPPER_CTRL_STATS_FE  (1ULL << 15)
#define XFRAME_SWAPPER_CTRL_STATS_SE  (1ULL << 14)

static void xframe_stat_dma(XframeState *s);
static int64_t xframe_stat_period_ms(XframeState *s);

static void xframe_stat_stl(XframeState *s, uint8_t *p, uint32_t val)
{
    if (s->swapper_ctrl & XFRAME_SWAPPER_CTRL_STATS_SE) {
        stl_le_p(p, val);
    } else {
        stl_be_p(p, val);
    }
}

#define XFRAME_PRC_RXD0_0           0x1930
#define XFRAME_PRC_CTRL_0           0x1970
#define XFRAME_MAC_INT_MASK         0x2008
#define XFRAME_MAC_RMAC_ERR_REG     0x2028
#define XFRAME_MAC_RMAC_ERR_MASK    0x2030
#define XFRAME_MAC_CFG              0x2100
/*
 * Largest receive payload the MAC will accept, in the field the driver writes
 * (Linux s2io RMAC_MAX_PYLD_LEN(val) = vBIT(val,2,14), so the value sits in
 * bits 61:48; its reset default is 1500). A guest raises this to run jumbo.
 */
#define XFRAME_RMAC_MAX_PYLD_LEN    0x2110
#define XFRAME_MAX_PYLD_SHIFT       48
#define XFRAME_MAX_PYLD_MASK        0x3fffULL
#define XFRAME_MAX_PYLD_DEFAULT     1500

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
 * bits sit in the high 32 bits. Raw status and acknowledgement semantics remain
 * model compatibility behavior rather than an established hardware contract.
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
 * Model delay before a link-up event. Public sources establish neither the
 * hardware delay nor a mandatory event caused by ADAPTER_EN, so this remains a
 * model timing choice rather than a hardware register contract.
 */
#define XFRAME_LINK_UP_DELAY_MS     5

/*
 * Compatibility delay used only when an incoming migration stream contains a
 * pending instance of the former synthetic link-down timer. New device
 * activity never arms that timer.
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

#define XFRAME_TRAFFIC_MEM_CMD_WE       (1ULL << 56)
#define XFRAME_TRAFFIC_MEM_CMD_STROBE   (1ULL << 48)
#define XFRAME_TTI_MEM_CMD_OFF_MASK     0x3fULL
#define XFRAME_RTI_MEM_CMD_OFF_MASK     0x7ULL
#define XFRAME_TRAFFIC_DATA1_AC          (1ULL << 25)
#define XFRAME_TRAFFIC_DATA1_CI          (1ULL << 24)
#define XFRAME_TTI_TIMER_MASK             0x3ffffffULL
#define XFRAME_RTI_TIMER_MASK             0x1fffffffULL

/*
 * PRC_CTRL_0: RC_IN_SVC arms receive ring 0, and bits 49:48 pick the
 * descriptor buffer-count mode. Only 1-buffer mode (the value both public
 * drivers use) is modelled.
 */
#define XFRAME_RC_IN_SVC                 (1ULL << 56)
#define XFRAME_PRC_CTRL_RING_MODE_MASK   (3ULL << 48)
#define XFRAME_RXD_MODE_1BUF             0
#define XFRAME_RXD_MODE_5BUF            2

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
 * IRIX xg opens PIC_INT_MASK but leaves GPIO_INT_MASK at its all-ones reset,
 * yet the link source reaches its ISR. Gate the source on PIC_INT_GPIO alone
 * and retain GPIO_INT_MASK for readback. An incoming version-7 device keeps
 * the former GPIO leaf gate until it is replaced rather than changing
 * interrupt visibility during migration.
 */
static bool xframe_gpio_link_pending(XframeState *s)
{
    return ((s->gpio_int_reg & XFRAME_GPIO_INT_LINK_BITS) != 0) &&
           !(s->pic_int_mask & XFRAME_PIC_INT_GPIO) &&
           (!s->legacy_interrupt_masking ||
            ((s->gpio_int_reg & XFRAME_GPIO_INT_LINK_BITS &
              ~s->gpio_int_mask) != 0));
}

/*
 * Whether the scheduled interrupt is delivering right now: the periodic timer
 * has fired and latched sched_int_pending, and the source is unmasked at BOTH
 * levels xg opens -- PIC_INT_MASK's PIC_INT_TX roll-up bit and TXPIC_INT_MASK's
 * leaf bit. This is the traffic- and link-independent watchdog feed on the
 * PIC/TXPIC path, distinct from the GPIO link source above (gated by
 * PIC_INT_GPIO) and never touching the GPIO/link bits, so it never disturbs
 * carrier. Both masks gate it independently of the sibling GPIO leaf masks.
 */
static bool xframe_sched_int_pending(XframeState *s)
{
    return s->sched_int_pending &&
           !(s->pic_int_mask & XFRAME_PIC_INT_TX) &&
           !(s->txpic_int_mask & XFRAME_TXPIC_INT_SCHED);
}

/*
 * Recompute the level-sensitive INTx line from each source's block masks.
 * GENERAL_INT_MASK does not gate delivery: IRIX xg leaves it at its all-ones
 * reset while enabling interrupts through PIC_INT_MASK and TXPIC_INT_MASK. A
 * device loaded from a version-7 stream retains the former GENERAL aggregate
 * gates as migration compatibility state.
 *
 * Four sources so far: receive completion (RX_TRAFFIC_INT gated by
 * RX_TRAFFIC_MASK), transmit completion (TX_TRAFFIC_INT gated by
 * TX_TRAFFIC_MASK), and two independent link-state paths -- the MAC-block one
 * (MAC_RMAC_ERR_REG gated by the two MAC masks, what OpenBSD xge uses) and the
 * PIC/GPIO-block one (GPIO_INT_REG gated by PIC_INT_MASK's PIC_INT_GPIO bit).
 * Either fires on an idle interface -- no frame need arrive for the link to
 * come up -- so a driver watchdog that resets on "no interrupts" is satisfied
 * through whichever block that driver unmasked.
 */
static void xframe_update_irq(XframeState *s)
{
    bool rx_pending = (s->rx_traffic_int & ~s->rx_traffic_mask) != 0;
    bool tx_pending = (s->tx_traffic_int & ~s->tx_traffic_mask) != 0;
    bool link_pending = xframe_link_pending(s);
    bool pic_pending = xframe_gpio_link_pending(s) ||
                       xframe_sched_int_pending(s);

    if (s->legacy_interrupt_masking) {
        rx_pending &= !(s->general_int_mask & XFRAME_GEN_INTR_RXTRAFFIC);
        tx_pending &= !(s->general_int_mask & XFRAME_GEN_INTR_TXTRAFFIC);
        link_pending &= !(s->general_int_mask & XFRAME_GEN_INTR_RXMAC);
        pic_pending &= !(s->general_int_mask & XFRAME_GEN_INTR_TXPIC);
    }

    pci_set_irq(PCI_DEVICE(s),
                rx_pending || tx_pending || link_pending || pic_pending);
}

static uint64_t xframe_traffic_bit(unsigned entry)
{
    return 1ULL << (63 - entry);
}

static void xframe_traffic_publish(XframeState *s, bool rx, unsigned entry)
{
    XframeTrafficBatch *batch = rx ? &s->rti_batch[entry] :
                                     &s->tti_batch[entry];
    uint64_t *status = rx ? &s->rx_traffic_int : &s->tx_traffic_int;
    uint64_t bit = xframe_traffic_bit(entry);

    batch->completions = 0;
    timer_del(batch->timer);
    if (*status & bit) {
        batch->deferred = true;
    } else {
        *status |= bit;
        if (rx) {
            trace_xframe_rx_completion_irq_recompute();
        }
        xframe_update_irq(s);
    }
}

static void xframe_traffic_timer_cb(void *opaque)
{
    XframeTrafficBatch *batch = opaque;

    if (batch->completions) {
        xframe_traffic_publish(batch->owner, batch->rx, batch->entry);
    }
}

static void xframe_traffic_batch_reset(XframeTrafficBatch *batch)
{
    timer_del(batch->timer);
    batch->completions = 0;
    batch->deferred = false;
}

static int64_t xframe_traffic_interval_ns(uint32_t ticks)
{
    return ((int64_t)ticks * 64000 + 132) / 133;
}

static uint32_t xframe_traffic_ticks(XframeState *s, bool rx, unsigned entry)
{
    uint64_t data1 = rx ? s->rti_entry_data1[entry] :
                          s->tti_entry_data1[entry];
    uint32_t mask = rx ? XFRAME_RTI_TIMER_MASK : XFRAME_TTI_TIMER_MASK;

    return (data1 >> 32) & mask;
}

static void xframe_traffic_continuous_arm(XframeState *s, bool rx,
                                          unsigned entry)
{
    XframeTrafficContinuous *continuous = rx ? &s->rti_continuous[entry] :
                                               &s->tti_continuous[entry];
    uint64_t data1 = rx ? s->rti_entry_data1[entry] :
                          s->tti_entry_data1[entry];
    bool valid = rx ? s->rti_entry_valid[entry] :
                      s->tti_entry_valid[entry];
    uint32_t ticks = xframe_traffic_ticks(s, rx, entry);

    timer_del(continuous->timer);
    continuous->deadline_ns = 0;
    if (valid && (s->adapter_control & XFRAME_ADAPTER_EN) && ticks &&
        (data1 & XFRAME_TRAFFIC_DATA1_CI)) {
        continuous->deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                  xframe_traffic_interval_ns(ticks);
        timer_mod(continuous->timer, continuous->deadline_ns);
    }
}

static void xframe_traffic_continuous_cb(void *opaque)
{
    XframeTrafficContinuous *continuous = opaque;
    XframeState *s = continuous->owner;
    uint32_t ticks = xframe_traffic_ticks(s, continuous->rx,
                                          continuous->entry);
    int64_t interval_ns = xframe_traffic_interval_ns(ticks);
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t expirations = 1 + (now_ns - continuous->deadline_ns) /
                               interval_ns;

    xframe_traffic_publish(s, continuous->rx, continuous->entry);
    if (expirations > 1) {
        xframe_traffic_publish(s, continuous->rx, continuous->entry);
    }
    continuous->deadline_ns += expirations * interval_ns;
    timer_mod(continuous->timer, continuous->deadline_ns);
}

static void xframe_traffic_complete(XframeState *s, bool rx, unsigned entry)
{
    XframeTrafficBatch *batch = rx ? &s->rti_batch[entry] :
                                     &s->tti_batch[entry];
    uint64_t data1 = rx ? s->rti_entry_data1[entry] :
                          s->tti_entry_data1[entry];
    uint64_t data2 = rx ? s->rti_entry_data2[entry] :
                          s->tti_entry_data2[entry];
    bool valid = rx ? s->rti_entry_valid[entry] :
                      s->tti_entry_valid[entry];
    uint32_t timer_mask = rx ? XFRAME_RTI_TIMER_MASK :
                               XFRAME_TTI_TIMER_MASK;
    uint32_t ticks = (data1 >> 32) & timer_mask;
    uint32_t ceiling = data2 >> 48;

    if (!valid || !(data1 & XFRAME_TRAFFIC_DATA1_AC) || ticks == 0 ||
        ceiling <= 1) {
        xframe_traffic_publish(s, rx, entry);
        return;
    }

    batch->completions++;
    if (batch->completions >= ceiling) {
        xframe_traffic_publish(s, rx, entry);
    } else if (!timer_pending(batch->timer)) {
        timer_mod(batch->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  xframe_traffic_interval_ns(ticks));
    }
}

static void xframe_traffic_ack(XframeState *s, bool rx, uint64_t written)
{
    XframeTrafficBatch *batches = rx ? s->rti_batch : s->tti_batch;
    uint64_t *status = rx ? &s->rx_traffic_int : &s->tx_traffic_int;
    unsigned entry;

    *status &= ~written;
    for (entry = 0; entry < XFRAME_TRAFFIC_MEM_ENTRIES; entry++) {
        uint64_t bit = xframe_traffic_bit(entry);

        if ((written & bit) && !(*status & bit) && batches[entry].deferred) {
            batches[entry].deferred = false;
            *status |= bit;
        }
    }
    xframe_update_irq(s);
}

/*
 * Retain the former synthetic link-down timer callback for incoming migration
 * streams that saved it while pending. New device activity never arms it.
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
 * Present link up on both interrupt paths. A first UP event is sufficient; no
 * synthetic delivered DOWN is required. Recheck the backend when the timer
 * fires so an intervening disconnect cannot announce stale carrier.
 */
static void xframe_link_up_timer_cb(void *opaque)
{
    XframeState *s = opaque;

    if (qemu_get_queue(s->nic)->link_down) {
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

static void xframe_rx_cache_invalidate(XframeState *s)
{
    s->rx_desc_cache_valid = false;
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
    unsigned i;

    qemu_bh_cancel(s->rx_flush_bh);
    xframe_rx_cache_invalidate(s);
    s->defer_backend_rx = false;
    s->backend_rx_deferred = false;
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
    s->tti_command = 0;
    s->tti_data1 = 0;
    s->tti_data2 = 0;
    memset(s->tti_entry_data1, 0, sizeof(s->tti_entry_data1));
    memset(s->tti_entry_data2, 0, sizeof(s->tti_entry_data2));
    memset(s->tti_entry_valid, 0, sizeof(s->tti_entry_valid));
    s->rti_command = 0;
    s->rti_data1 = 0;
    s->rti_data2 = 0;
    memset(s->rti_entry_data1, 0, sizeof(s->rti_entry_data1));
    memset(s->rti_entry_data2, 0, sizeof(s->rti_entry_data2));
    memset(s->rti_entry_valid, 0, sizeof(s->rti_entry_valid));
    for (i = 0; i < XFRAME_TRAFFIC_MEM_ENTRIES; i++) {
        xframe_traffic_batch_reset(&s->tti_batch[i]);
        xframe_traffic_batch_reset(&s->rti_batch[i]);
        timer_del(s->tti_continuous[i].timer);
        timer_del(s->rti_continuous[i].timer);
        s->tti_continuous[i].deadline_ns = 0;
        s->rti_continuous[i].deadline_ns = 0;
    }
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
    s->rmac_max_pyld_len =
        (uint64_t)XFRAME_MAX_PYLD_DEFAULT << XFRAME_MAX_PYLD_SHIFT;
    s->mac_rmac_err_mask = XFRAME_INT_MASK_RESET;
    /*
     * Reset tears the ring down, so a recovery poll left running from before
     * has nothing left to recover -- stop it rather than let it keep firing
     * against a ring that no longer exists in any guest-meaningful sense.
     */
    timer_del(s->rx_recovery_timer);
    timer_del(s->stat_timer);
    s->stat_cfg = 0;
    s->stat_addr = 0;
    s->stat_tx_frms = 0;
    s->stat_tx_octets = 0;
    s->stat_rx_frms = 0;
    s->stat_rx_octets = 0;
    /*
     * A reset drops the adapter (ADAPTER_EN cleared above), so any pending
     * link event is void. Cancel the active UP one-shot, the legacy DOWN
     * one-shot retained for migration, and the scheduled interrupt.
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

static void xframe_traffic_mem_cmd(XframeState *s, uint64_t val, bool rx)
{
    uint64_t mask = rx ? XFRAME_RTI_MEM_CMD_OFF_MASK :
                         XFRAME_TTI_MEM_CMD_OFF_MASK;
    unsigned entry = (val >> 32) & mask;
    uint64_t *command = rx ? &s->rti_command : &s->tti_command;
    uint64_t *data1 = rx ? &s->rti_data1 : &s->tti_data1;
    uint64_t *data2 = rx ? &s->rti_data2 : &s->tti_data2;
    uint64_t *entry_data1 = rx ? s->rti_entry_data1 : s->tti_entry_data1;
    uint64_t *entry_data2 = rx ? s->rti_entry_data2 : s->tti_entry_data2;
    bool *entry_valid = rx ? s->rti_entry_valid : s->tti_entry_valid;

    if (val & XFRAME_TRAFFIC_MEM_CMD_STROBE) {
        if (!rx && entry >= XFRAME_TRAFFIC_MEM_ENTRIES) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xframe: invalid TTI memory entry %u\n", entry);
        } else if (val & XFRAME_TRAFFIC_MEM_CMD_WE) {
            entry_data1[entry] = *data1;
            entry_data2[entry] = *data2;
            entry_valid[entry] = true;
            xframe_traffic_batch_reset(rx ? &s->rti_batch[entry] :
                                            &s->tti_batch[entry]);
            xframe_traffic_continuous_arm(s, rx, entry);
        }
        val &= ~XFRAME_TRAFFIC_MEM_CMD_STROBE;
    }
    *command = val;
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
    case XFRAME_TTI_COMMAND_MEM:
        return s->tti_command;
    case XFRAME_TTI_DATA1_MEM:
        return s->tti_data1;
    case XFRAME_TTI_DATA2_MEM:
        return s->tti_data2;
    case XFRAME_RTI_COMMAND_MEM:
        return s->rti_command;
    case XFRAME_RTI_DATA1_MEM:
        return s->rti_data1;
    case XFRAME_RTI_DATA2_MEM:
        return s->rti_data2;
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
    case XFRAME_STAT_CFG:
        return s->stat_cfg;
    case XFRAME_STAT_ADDR:
        return s->stat_addr;
    case XFRAME_PRC_RXD0_0:
        return s->prc_rxd0_0;
    case XFRAME_PRC_CTRL_0:
        return s->prc_ctrl_0;
    case XFRAME_GENERAL_INT_STATUS: {
        /*
         * Derived from lower-level delivered sources and independent of the
         * GENERAL mask. This preserves the model's existing summary behavior;
         * hardware visibility while a GENERAL bit is masked remains open.
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
    case XFRAME_RMAC_MAX_PYLD_LEN:
        return s->rmac_max_pyld_len;
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
     * Preserve the model's existing read-clear behavior without making it part
     * of the hardware contract. GPIO acknowledgement remains open. Only a read
     * covering the high half consumes the high-half link bits, and write
     * composition through xframe_pif_read_reg never reaches this side effect.
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
    bool adapter_enable_edge = addr == XFRAME_ADAPTER_CONTROL &&
        (full & XFRAME_ADAPTER_EN) &&
        !(s->adapter_control & XFRAME_ADAPTER_EN);
    bool adapter_disable_edge = addr == XFRAME_ADAPTER_CONTROL &&
        !(full & XFRAME_ADAPTER_EN) &&
        (s->adapter_control & XFRAME_ADAPTER_EN);
    unsigned i;

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
         * A rising ADAPTER_EN edge schedules a single link-up event when the
         * backend is up. It never synthesizes a preceding delivered LINK_DOWN.
         * Rewriting an already-set enable bit does not re-arm the timer.
         * The scheduled interrupt that keeps the watchdog fed is NOT armed here
         * -- the driver arms it explicitly by writing SCHED_INT_CTRL during
         * bring-up. The falling edge cancels both link timers and the
         * scheduled interrupt -- the driver disabling the adapter without a full
         * reset must not leave a stale link edge or a periodic interrupt firing
         * for a device it just took down. It also clears any link source
         * already latched but not yet acked (GPIO_INT_REG's LINK_UP/LINK_DOWN
         * bits, mac_rmac_err's RMAC_LINK_STATE_CHANGE_INT), matching how the
         * read-ack paths clear them (XFRAME_GPIO_INT_REG and
         * XFRAME_MAC_RMAC_ERR_REG read handling above): delivery of these
         * sources is not gated by ADAPTER_EN, so leaving one latched would
         * keep INTx asserted for a disabled adapter.
         */
        if (adapter_enable_edge &&
            !qemu_get_queue(s->nic)->link_down) {
            timer_mod(s->link_up_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                      XFRAME_LINK_UP_DELAY_MS);
        } else if (adapter_disable_edge) {
            timer_del(s->link_down_timer);
            timer_del(s->link_up_timer);
            timer_del(s->sched_int_timer);
            s->sched_int_pending = false;
            s->sched_int_armed = false;
            s->gpio_int_reg &= ~XFRAME_GPIO_INT_LINK_BITS;
            s->mac_rmac_err &= ~XFRAME_RMAC_LINK_STATE_CHANGE_INT;
        }
        s->adapter_control = full;
        for (i = 0; i < XFRAME_TRAFFIC_MEM_ENTRIES; i++) {
            if (adapter_enable_edge) {
                xframe_traffic_continuous_arm(s, false, i);
                xframe_traffic_continuous_arm(s, true, i);
            } else if (adapter_disable_edge) {
                timer_del(s->tti_continuous[i].timer);
                timer_del(s->rti_continuous[i].timer);
                s->tti_continuous[i].deadline_ns = 0;
                s->rti_continuous[i].deadline_ns = 0;
            }
        }
        xframe_update_irq(s);
        return;
    case XFRAME_SWAPPER_CTRL:
        xframe_rx_cache_invalidate(s);
        s->swapper_ctrl = full;
        return;
    case XFRAME_XMSI_ADDRESS:
        s->xmsi_address = full;
        return;
    case XFRAME_TTI_DATA1_MEM:
        s->tti_data1 = full;
        return;
    case XFRAME_TTI_DATA2_MEM:
        s->tti_data2 = full;
        return;
    case XFRAME_TTI_COMMAND_MEM:
        if (commit_high) {
            xframe_traffic_mem_cmd(s, full, false);
        } else {
            s->tti_command = full;
        }
        return;
    case XFRAME_RTI_DATA1_MEM:
        s->rti_data1 = full;
        return;
    case XFRAME_RTI_DATA2_MEM:
        s->rti_data2 = full;
        return;
    case XFRAME_RTI_COMMAND_MEM:
        if (commit_high) {
            xframe_traffic_mem_cmd(s, full, true);
        } else {
            s->rti_command = full;
        }
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
            /*
             * Release the unlock on the access covering the HIGH half -- the
             * same half the key itself arrives through -- not on whichever
             * access happens to come first. A driver reaching this 64-bit
             * register through 32-bit cycles writes it as two halves (Linux
             * s2io does on the Xena part, and the Origin 200 Bridge splits
             * every 64-bit access that way for a slot with DEV_64BIT_DEVICE
             * clear), so releasing it on the first half refuses the second and
             * leaves the register holding half of what the guest wrote. One key
             * unlocks one register write, however many bus cycles it takes.
             */
            if (commit_high) {
                s->rmac_cfg_unlocked = false;
            }
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
        xframe_traffic_ack(s, false, written);
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
        xframe_traffic_ack(s, true, written);
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
         * Preserve the model's existing W1C path. Hardware acknowledgement
         * semantics remain open. Clear only the bits carried by this access so
         * a 32-bit half-write cannot clear bits in the untouched half.
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
    case XFRAME_STAT_ADDR:
        s->stat_addr = full;
        return;
    case XFRAME_STAT_CFG:
        /*
         * Enable and one-shot are in the high half; act once the write covers
         * it. Enabling DMAs a first snapshot immediately -- the driver's
         * initial read must not see stale guest memory -- and arms the
         * periodic refresh unless one-shot asked for exactly one.
         */
        s->stat_cfg = full;
        if (commit_high) {
            if (full & XFRAME_STAT_CFG_EN) {
                /*
                 * Only FE==SE is modelled (both set -> little-endian in place,
                 * both clear -> big-endian). A guest that sets exactly one asks
                 * for a half-transpose this model does not do; warn once here at
                 * enable rather than on every periodic DMA.
                 */
                if (!(s->swapper_ctrl & XFRAME_SWAPPER_CTRL_STATS_FE) !=
                    !(s->swapper_ctrl & XFRAME_SWAPPER_CTRL_STATS_SE)) {
                    qemu_log_mask(LOG_UNIMP,
                                  "xframe: statistics swapper FE != SE (0x%"
                                  PRIx64 ") not modelled; writing in place\n",
                                  s->swapper_ctrl);
                }
                xframe_stat_dma(s);
                if (full & XFRAME_STAT_CFG_ONE_SHOT) {
                    /*
                     * The hardware clears enable when the single transfer
                     * completes; the driver's one-shot path (s2io_updt_stats)
                     * polls STAT_CFG's enable bit for exactly that and gives up
                     * as "Updt failed" if it never clears. The DMA above is
                     * synchronous, so clear it now, and cancel any periodic
                     * timer a prior enable armed so one-shot means exactly one.
                     */
                    timer_del(s->stat_timer);
                    s->stat_cfg &= ~XFRAME_STAT_CFG_EN;
                } else {
                    /*
                     * The interval is taken from the period present now, at the
                     * enable (high-half) write; a guest that rewrites the period
                     * low half afterwards gets one stale interval before the
                     * next re-arm picks the new value up. Log a clamped period
                     * once here rather than on every timer fire.
                     */
                    int64_t raw = (int64_t)((full & 0xffffffffULL) * 1000 /
                                            XFRAME_STAT_PER_SEC);
                    if (raw < 100 || raw > 10000) {
                        qemu_log_mask(LOG_UNIMP,
                                      "xframe: statistics period %" PRId64
                                      " ms clamped to [100, 10000]\n", raw);
                    }
                    timer_mod(s->stat_timer,
                              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                              xframe_stat_period_ms(s));
                }
            } else {
                timer_del(s->stat_timer);
            }
        }
        return;
    case XFRAME_PRC_RXD0_0:
        xframe_rx_cache_invalidate(s);
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
        xframe_rx_cache_invalidate(s);
        if ((full & XFRAME_RC_IN_SVC) && !(s->prc_ctrl_0 & XFRAME_RC_IN_SVC)) {
            /*
             * Arming the ring restarts the walk at its base -- the sequence
             * both drivers use writes PRC_RXD0_0 once, before this.
             */
            s->rx_page_addr = s->prc_rxd0_0;
            s->rx_desc_idx = 0;
        }
        switch ((unsigned)((full >> 48) & 3)) {
        case XFRAME_RXD_MODE_1BUF:
        case XFRAME_RXD_MODE_5BUF:
            break;      /* both implemented */
        default:
            qemu_log_mask(LOG_UNIMP,
                          "xframe: receive ring mode %u is not implemented "
                          "(only one-buffer mode 0 and five-buffer mode 2)\n",
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
         * Retained for readback. IRIX xg leaves this at reset and enables the
         * interrupt sources it uses through their block masks. An incoming
         * version-7 device retains its former final aggregate gate.
         */
        s->general_int_mask = full;
        xframe_update_irq(s);
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
    case XFRAME_RMAC_MAX_PYLD_LEN:
        /* The guest raises this to run jumbo; the receive path honours it. */
        s->rmac_max_pyld_len = full;
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
         * roll-up to the line, while PIC_INT_TX gates the scheduled source.
         * Recompute for both; the remaining sub-block bits are held for
         * read-back only.
         */
        s->pic_int_mask = full;
        xframe_update_irq(s);
        return;
    case XFRAME_GPIO_INT_MASK:
        /* Only an incoming version-7 device retains this link-source gate. */
        s->gpio_int_mask = full;
        xframe_update_irq(s);
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
 * The statistics block. The card DMAs a structure of hardware counters to the
 * guest address in STAT_ADDR, periodically while STAT_CFG's enable is set, and
 * the driver maintains its interface counters as DELTAS against successive
 * snapshots of that memory. So the block must exist and must advance: left
 * un-DMA'd, the driver differences against a block that never moves and its
 * transmit packet count runs backwards (observed as netstat -in reporting
 * Opkts of 4294967284 and falling, about one step per transmit handed over).
 *
 * Offsets and the hardware span come from compiling the public s2io stat_block
 * (offsetof, both the mainline and history copies of the struct agree). The
 * card writes the hardware section, [0, 0x320); at 0x320 the driver's own
 * software tail (u8 buffer[20], then swStat) begins and the card never touches
 * it. The block is a VALUE structure -- counts, not a byte stream -- so its
 * byte order follows the guest's own SWAPPER_CTRL_STATS_SE setting, the way
 * descriptors follow their swap-enables (see xframe_stat_stl); a plain
 * little-endian write is read back byte-reversed on this big-endian guest, a
 * count of 50 arriving as 0x32000000. Each 64-bit count is split as a low
 * 32-bit field near the top of the block and a high 32-bit overflow field in
 * a later section -- the two overflow fields sit past the eight __le16
 * rmac_full_q0..q7 counters, which is why each overflow offset is 16 bytes
 * above its Tx/Rx neighbour. Only the transmit and receive frame and byte
 * counters are filled -- the rest of the block reads zero, which for counters
 * of events this model never generates (errors, pauses, discards) is also the
 * truth.
 */
#define XFRAME_STAT_BLOCK_SIZE          0x320
#define XFRAME_STAT_TMAC_DATA_OCTETS    0x000
#define XFRAME_STAT_TMAC_FRMS           0x004
#define XFRAME_STAT_RMAC_DATA_OCTETS    0x060
#define XFRAME_STAT_RMAC_VLD_FRMS       0x064
#define XFRAME_STAT_TMAC_OCTETS_OFLOW   0x218
#define XFRAME_STAT_TMAC_FRMS_OFLOW     0x21c
#define XFRAME_STAT_RMAC_OCTETS_OFLOW   0x260
#define XFRAME_STAT_RMAC_FRMS_OFLOW     0x264

static void xframe_stat_dma(XframeState *s)
{
    uint8_t block[XFRAME_STAT_BLOCK_SIZE] = { 0 };

    xframe_stat_stl(s, block + XFRAME_STAT_TMAC_DATA_OCTETS,
                    (uint32_t)s->stat_tx_octets);
    xframe_stat_stl(s, block + XFRAME_STAT_TMAC_OCTETS_OFLOW,
                    (uint32_t)(s->stat_tx_octets >> 32));
    xframe_stat_stl(s, block + XFRAME_STAT_TMAC_FRMS, (uint32_t)s->stat_tx_frms);
    xframe_stat_stl(s, block + XFRAME_STAT_TMAC_FRMS_OFLOW,
                    (uint32_t)(s->stat_tx_frms >> 32));
    xframe_stat_stl(s, block + XFRAME_STAT_RMAC_DATA_OCTETS,
                    (uint32_t)s->stat_rx_octets);
    xframe_stat_stl(s, block + XFRAME_STAT_RMAC_OCTETS_OFLOW,
                    (uint32_t)(s->stat_rx_octets >> 32));
    xframe_stat_stl(s, block + XFRAME_STAT_RMAC_VLD_FRMS,
                    (uint32_t)s->stat_rx_frms);
    xframe_stat_stl(s, block + XFRAME_STAT_RMAC_FRMS_OFLOW,
                    (uint32_t)(s->stat_rx_frms >> 32));

    if (pci_dma_write(PCI_DEVICE(s), s->stat_addr, block,
                      sizeof(block)) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xframe: statistics block write at 0x%" PRIx64
                      " failed\n", s->stat_addr);
    }
}

/*
 * STAT_CFG's low 32 bits are the period in bus clicks (public s2io
 * SET_UPDT_PERIOD / SET_UPDT_CLICKS, s2io-regs.h:264-266); PER_SEC clicks make
 * a second. Clamp to a host floor and ceiling so a pathological guest value
 * cannot spin or freeze the periodic DMA. The floor only ever bites periodic
 * mode -- the driver's one-shot uses 10 clicks and never reaches the timer --
 * and the enable path logs when it engages, so the silent change of a
 * guest-visible period is visible once.
 */
static int64_t xframe_stat_period_ms(XframeState *s)
{
    uint64_t clicks = s->stat_cfg & 0xffffffffULL;
    int64_t ms = clicks * 1000 / XFRAME_STAT_PER_SEC;

    return MIN(MAX(ms, 100), 10000);
}

static void xframe_stat_timer_cb(void *opaque)
{
    XframeState *s = opaque;

    /* Only periodic mode arms this timer; a one-shot cleared enable already. */
    if (!(s->stat_cfg & XFRAME_STAT_CFG_EN)) {
        return;
    }
    xframe_stat_dma(s);
    timer_mod(s->stat_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              xframe_stat_period_ms(s));
}

/*
 * Transmit checksum offload, in the second control word. The driver hands the
 * card a frame whose IPv4, TCP or UDP checksum field is not yet valid and asks
 * for it to be inserted here (Linux s2io TXD_TX_CKO_IPV4_EN, _TCP_EN, _UDP_EN,
 * the driver's own MSB-first bit numbering: bits 5, 6 and 7).
 */
#define XFRAME_TXD_CKO_IPV4         (1ULL << 58)
#define XFRAME_TXD_CKO_TCP          (1ULL << 57)
#define XFRAME_TXD_CKO_UDP          (1ULL << 56)

/*
 * Segmentation offload, in the FIRST control word alongside the buffer length
 * and the gather code (Linux s2io TXD_TCP_LSO_EN is s2BIT(30), i.e. bit 33,
 * and TXD_TCP_LSO_MSS is vBIT(val,34,14), i.e. bits 29:16).
 */
#define XFRAME_TXD_LSO_EN           (1ULL << 33)
#define XFRAME_TXD_LSO_MSS_SHIFT    16
#define XFRAME_TXD_LSO_MSS_MASK     0x3fffULL

#define XFRAME_ETH_HDR_LEN          14
#define XFRAME_ETHERTYPE_IPV4       0x0800
#define XFRAME_IPV4_MIN_HDR_LEN     20
#define XFRAME_TCP_MIN_HDR_LEN      20
#define XFRAME_IPPROTO_TCP          6
#define XFRAME_IPPROTO_UDP          17
#define XFRAME_TCP_FLAG_FIN         0x01
#define XFRAME_TCP_FLAG_PSH         0x08

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

static void xframe_send_packet(XframeState *s, const uint8_t *frame,
                               size_t frame_len)
{
    struct virtio_net_hdr vhdr = { };
    struct iovec iov[] = {
        {
            .iov_base = &vhdr,
            .iov_len = sizeof(vhdr),
        }, {
            .iov_base = (void *)frame,
            .iov_len = frame_len,
        },
    };

    if (s->has_vnet_hdr) {
        qemu_sendv_packet(qemu_get_queue(s->nic), iov, G_N_ELEMENTS(iov));
    } else {
        qemu_send_packet(qemu_get_queue(s->nic), frame, frame_len);
    }
}

/*
 * Segmentation offload: send one oversized TCP buffer as a run of MSS-sized
 * frames. IRIX enables this on the adapter -- xg1's interface flags carry
 * L2TCPSEG -- and then hands the card a single buffer holding several segments'
 * worth of payload with the segment size in the descriptor, expecting the card
 * to do the cutting. Each frame gets its own copy of the headers, its own
 * sequence number and its own checksums, and only the last one keeps the
 * sender's FIN and PSH: those mark the end of the data, which the earlier
 * segments are not.
 *
 * Returns false for anything this cannot legally split -- not IPv4/TCP, headers
 * that do not fit, or a payload no larger than one segment -- leaving the caller
 * to send the frame unchanged, which is the right answer in every one of those
 * cases.
 */
static bool xframe_tx_segment(XframeState *s, size_t frame_len, uint16_t mss)
{
    const uint8_t *frame = s->tx_frame;
    const uint8_t *ip, *tcp;
    size_t ihl, thl, hdr_len, payload_len, off;
    uint32_t seq;
    uint16_t ip_id;
    unsigned index;

    if (mss == 0 || !xframe_tx_frame_is_ipv4(frame, frame_len)) {
        return false;
    }
    ip = frame + XFRAME_ETH_HDR_LEN;
    if (ip[9] != XFRAME_IPPROTO_TCP) {
        return false;
    }
    ihl = (ip[0] & 0x0f) * 4;
    if (frame_len < XFRAME_ETH_HDR_LEN + ihl + XFRAME_TCP_MIN_HDR_LEN) {
        return false;
    }
    tcp = ip + ihl;
    thl = (tcp[12] >> 4) * 4;
    if (thl < XFRAME_TCP_MIN_HDR_LEN) {
        return false;
    }
    hdr_len = XFRAME_ETH_HDR_LEN + ihl + thl;
    if (frame_len <= hdr_len) {
        return false;
    }
    payload_len = frame_len - hdr_len;
    if (payload_len <= mss) {
        return false;
    }

    seq = ldl_be_p(tcp + 4);
    ip_id = lduw_be_p(ip + 4);

    for (off = 0, index = 0; off < payload_len; off += mss, index++) {
        size_t chunk = MIN((size_t)mss, payload_len - off);
        uint8_t *seg = s->tx_segment;
        uint8_t *seg_ip = seg + XFRAME_ETH_HDR_LEN;
        uint8_t *seg_tcp = seg_ip + ihl;

        memcpy(seg, frame, hdr_len);
        memcpy(seg + hdr_len, frame + hdr_len + off, chunk);

        /* Each segment's headers describe that segment, not the whole send. */
        stw_be_p(seg_ip + 2, (uint16_t)(ihl + thl + chunk));
        stw_be_p(seg_ip + 4, (uint16_t)(ip_id + index));
        stl_be_p(seg_tcp + 4, seq + (uint32_t)off);

        if (off + chunk < payload_len) {
            seg_tcp[13] &= (uint8_t)~(XFRAME_TCP_FLAG_FIN | XFRAME_TCP_FLAG_PSH);
        }

        /*
         * The guest cannot have checksummed segments it never built, so these
         * are computed here whatever the descriptor's checksum bits asked for.
         */
        net_checksum_calculate(seg, hdr_len + chunk, CSUM_IP | CSUM_TCP);
        xframe_send_packet(s, seg, hdr_len + chunk);
        s->stat_tx_frms++;
        s->stat_tx_octets += hdr_len + chunk;
    }

    return true;
}

static void xframe_tx_pkt_release(void *context, void *base, size_t len)
{
    /* The fragment points at XframeState::tx_frame, not allocated memory. */
}

static bool xframe_tx_offload(XframeState *s, size_t frame_len, uint16_t mss)
{
    const uint8_t *frame = s->tx_frame;
    struct virtio_net_hdr *vhdr;
    size_t hdr_len, payload_len, segments;
    bool sent = false;

    if (!s->has_vnet_hdr || mss == 0 ||
        !xframe_tx_frame_is_ipv4(frame, frame_len)) {
        return false;
    }
    if (frame[XFRAME_ETH_HDR_LEN + 9] != XFRAME_IPPROTO_TCP) {
        return false;
    }

    net_tx_pkt_reset(s->tx_pkt, xframe_tx_pkt_release, NULL);
    if (!net_tx_pkt_add_raw_fragment(s->tx_pkt, s->tx_frame, frame_len) ||
        !net_tx_pkt_parse(s->tx_pkt) ||
        !net_tx_pkt_build_vheader(s->tx_pkt, true, true, mss)) {
        goto out;
    }
    vhdr = net_tx_pkt_get_vhdr(s->tx_pkt);
    hdr_len = vhdr->hdr_len;
    if ((vhdr->gso_type & ~VIRTIO_NET_HDR_GSO_ECN) !=
            VIRTIO_NET_HDR_GSO_TCPV4 ||
        hdr_len >= frame_len || frame_len - hdr_len <= mss) {
        goto out;
    }
    payload_len = frame_len - hdr_len;
    net_tx_pkt_update_ip_checksums(s->tx_pkt);
    sent = net_tx_pkt_send(s->tx_pkt, qemu_get_queue(s->nic));
    if (sent) {
        segments = DIV_ROUND_UP(payload_len, mss);
        s->stat_tx_frms += segments;
        s->stat_tx_octets += payload_len + segments * hdr_len;
    }

out:
    net_tx_pkt_reset(s->tx_pkt, xframe_tx_pkt_release, NULL);
    return sent;
}

/* Not gated on ADAPTER_EN: drivers enable the adapter before any doorbell. */
static void xframe_tx_doorbell(XframeState *s, unsigned fifo, uint64_t lcr)
{
    PCIDevice *pdev = PCI_DEVICE(s);
    uint64_t par = s->txdl_par[fifo];
    unsigned count = ((lcr >> 56) & 0xff) + 1;   /* NUMTXD + 1, 1..256 */
    uint64_t txd_addr[256];
    uint64_t txd_control1[256];
    uint64_t first_control1 = 0;
    uint64_t first_control2 = 0;
    size_t frame_len = 0;
    size_t cap = XFRAME_MAX_FRAME;
    bool have_first = false;
    unsigned i, walked = 0;

    for (i = 0; i < count; i++) {
        uint64_t addr = par + (uint64_t)i * XFRAME_TXD_SIZE;
        uint8_t raw[XFRAME_TXD_SIZE];
        uint64_t control1, bufaddr;
        unsigned seg_len;

        if (pci_dma_read(pdev, addr, raw, sizeof(raw)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xframe: transmit descriptor read at 0x%" PRIx64
                          " failed; list abandoned\n", addr);
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
            first_control1 = control1;
            first_control2 = xframe_desc_ldq(s, raw + 8, false);
            have_first = true;
            /*
             * A segmentation-offload buffer is one IP datagram's worth of
             * payload waiting to be cut up, so it is bounded by the datagram
             * the guest is describing rather than by what one frame may carry.
             * IRIX hands this adapter buffers of about 32 KB.
             */
            if (control1 & XFRAME_TXD_LSO_EN) {
                cap = XFRAME_MAX_TSO_FRAME;
            }
        }

        if (seg_len > cap - frame_len) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xframe: transmit frame exceeds %zu bytes; "
                          "truncated\n", cap);
            seg_len = cap - frame_len;
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
        txd_addr[walked] = addr;
        txd_control1[walked++] = control1;

        if ((control1 & XFRAME_TXD_GCL) || frame_len >= cap) {
            break;  /* last segment of this frame */
        }
    }

    /*
     * Give every walked descriptor back before the frame goes out. The first
     * descriptor is the list's publication point, so release it last: a fault
     * while releasing any later descriptor leaves the list owned and safe to
     * retry. A driver reclaims transmit descriptors by their ownership bit;
     * every write must succeed before it can be told that the list completed.
     */
    for (i = 1; i < walked; i++) {
        uint8_t word[8];

        xframe_desc_stq(s, word, txd_control1[i] & ~XFRAME_TXD_OWN, false);
        if (pci_dma_write(pdev, txd_addr[i], word, sizeof(word)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xframe: transmit ownership write at 0x%" PRIx64
                          " failed; completion not published\n", txd_addr[i]);
            return;
        }
    }
    if (walked > 0) {
        uint8_t word[8];

        xframe_desc_stq(s, word, txd_control1[0] & ~XFRAME_TXD_OWN, false);
        if (pci_dma_write(pdev, txd_addr[0], word, sizeof(word)) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xframe: transmit ownership write at 0x%" PRIx64
                          " failed; completion not published\n", txd_addr[0]);
            return;
        }
    }

    if (frame_len > 0) {
        uint16_t mss = (first_control1 >> XFRAME_TXD_LSO_MSS_SHIFT) &
                       XFRAME_TXD_LSO_MSS_MASK;
        bool segmented = false;
        int csum = 0;

        /*
         * A backend may answer synchronously from a network send. Hold that
         * reply in QEMU's net queue until this doorbell has published its
         * transmit completion below. This covers every segment of one
         * offloaded send as a single transmit operation.
         */
        s->defer_backend_rx = true;

        /*
         * Segmentation offload is asked for in the first control word, and it
         * rebuilds and rechecksums every frame it sends -- so it runs instead
         * of the whole-buffer checksum insertion below, which would otherwise
         * compute over a frame that never reaches the wire.
         */
        if (first_control1 & XFRAME_TXD_LSO_EN) {
            segmented = xframe_tx_offload(s, frame_len, mss) ||
                        xframe_tx_segment(s, frame_len, mss);
        }

        if (!segmented) {
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
                              "xframe: checksum insertion asked for a frame "
                              "that is not a well-formed IPv4 packet; sent "
                              "unchanged\n");
                csum = 0;
            }
            if (csum) {
                net_checksum_calculate(s->tx_frame, frame_len, csum);
            }
            xframe_send_packet(s, s->tx_frame, frame_len);
            s->stat_tx_frms++;
            s->stat_tx_octets += frame_len;
        }
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
        xframe_traffic_complete(s, false, fifo);
    }

    s->defer_backend_rx = false;
    if (s->backend_rx_deferred) {
        s->backend_rx_deferred = false;
        qemu_bh_schedule(s->rx_flush_bh);
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
 * What the card reports about a received frame, in the same first control word
 * as the transfer code: how it classified the frame, and whether its checksums
 * verified (Linux s2io.h RXD_FRAME_PROTO_IPV4 s2BIT(27), _IP_FRAG s2BIT(29),
 * _PROTO_TCP s2BIT(30) and _PROTO_UDP s2BIT(31) -- bits 36, 34, 33 and 32
 * counting from the low end -- with RXD_GET_L3_CKSUM reading the half at bits
 * 31:16 and RXD_GET_L4_CKSUM the half at 15:0, each 0xFFFF when the checksum
 * verified.
 *
 * Leaving these zero is not the same as leaving them unimplemented: zero is a
 * value, and it is the one that says "this frame's checksums did not verify".
 */
#define XFRAME_RXD_PROTO_IPV4        (1ULL << 36)
#define XFRAME_RXD_PROTO_IP_FRAG     (1ULL << 34)
#define XFRAME_RXD_PROTO_TCP         (1ULL << 33)
#define XFRAME_RXD_PROTO_UDP         (1ULL << 32)
#define XFRAME_RXD_L3_CKSUM_SHIFT    16
#define XFRAME_RXD_CKSUM_OK          0xffffULL

/*
 * Classify a received frame and check its checksums, in the form the first
 * control word reports them. A driver that trusts this skips verifying in
 * software, which is the point: on this machine the guest CPU is emulated and
 * the check here is not, so every byte checked here is a byte the guest does
 * not have to walk.

 *
 * Report a checksum verified ONLY when it actually verified against the bytes
 * that arrived. Anything this cannot vouch for -- a frame that is not IPv4, a
 * truncated one, a fragment (which carries no complete transport header), a
 * transport this does not parse, or a UDP datagram whose checksum field is zero
 * and therefore means "not computed" -- leaves the corresponding half zero, and
 * a driver reading zero verifies for itself.
 */
static uint64_t xframe_rx_offload_status(const uint8_t *frame, size_t len,
                                         size_t *l34_len)
{
    const uint8_t *ip, *l4;
    size_t ihl, ip_total, l4_len, thl;
    uint64_t status = 0;
    uint8_t pseudo[12];
    uint32_t sum;
    uint8_t proto;

    if (l34_len) {
        *l34_len = 0;
    }

    if (len < XFRAME_ETH_HDR_LEN + XFRAME_IPV4_MIN_HDR_LEN ||
        lduw_be_p(frame + 12) != XFRAME_ETHERTYPE_IPV4 ||
        (frame[XFRAME_ETH_HDR_LEN] >> 4) != 4) {
        return 0;
    }
    ip = frame + XFRAME_ETH_HDR_LEN;
    ihl = (ip[0] & 0x0f) * 4;
    ip_total = lduw_be_p(ip + 2);
    if (ihl < XFRAME_IPV4_MIN_HDR_LEN || ip_total < ihl ||
        XFRAME_ETH_HDR_LEN + ip_total > len) {
        return 0;
    }

    status |= XFRAME_RXD_PROTO_IPV4;
    if (net_raw_checksum((uint8_t *)ip, ihl) == 0) {
        status |= XFRAME_RXD_CKSUM_OK << XFRAME_RXD_L3_CKSUM_SHIFT;
    }

    /* More-fragments set or a non-zero offset: no complete transport header. */
    if (lduw_be_p(ip + 6) & 0x3fff) {
        if (l34_len) {
            *l34_len = ihl;
        }
        return status | XFRAME_RXD_PROTO_IP_FRAG;
    }

    proto = ip[9];
    l4 = ip + ihl;
    l4_len = ip_total - ihl;
    if (proto == XFRAME_IPPROTO_TCP) {
        if (l4_len < XFRAME_TCP_MIN_HDR_LEN) {
            return status;
        }
        thl = ((l4[12] >> 4) & 0x0f) * 4;
        if (thl < XFRAME_TCP_MIN_HDR_LEN || thl > l4_len) {
            return status;
        }
        status |= XFRAME_RXD_PROTO_TCP;
    } else if (proto == XFRAME_IPPROTO_UDP) {
        if (l4_len < 8) {
            return status;
        }
        thl = 8;
        status |= XFRAME_RXD_PROTO_UDP;
    } else {
        return status;
    }

    /*
     * Complete TCP/UDP frames split the IP and transport headers into buffer 1.
     * Fragments report only the validated IP header above because no complete
     * transport header exists.
     */
    if (l34_len) {
        *l34_len = ihl + thl;
    }

    /* A zero UDP checksum means "none sent", not "verified". */
    if (proto == XFRAME_IPPROTO_UDP && lduw_be_p(l4 + 6) == 0) {
        return status;
    }

    memcpy(pseudo, ip + 12, 8);          /* source and destination addresses */
    pseudo[8] = 0;
    pseudo[9] = proto;
    stw_be_p(pseudo + 10, (uint16_t)l4_len);
    sum = net_checksum_add(sizeof(pseudo), pseudo);
    sum += net_checksum_add(l4_len, (uint8_t *)l4);
    if (net_checksum_finish(sum) == 0) {
        status |= XFRAME_RXD_CKSUM_OK;
    }

    return status;
}

/*
 * RxDs are laid out 127 to a 4 KB page (if_xgereg.h's NDESC_1BUFMODE),
 * chained by a next-page pointer in the page's last eight bytes. Both public
 * drivers allocate several such pages and link them into a ring, so the walk
 * below follows that same chain rather than assuming one page is the whole
 * ring.
 */
#define XFRAME_RXD1_PER_PAGE         127

/*
 * Receive ring modes, PRC_CTRL_0 bits 49:48. IRIX arms mode 0 at a standard MTU
 * and mode 2 -- which the public s2io header (RING_MODE_5, vBIT(2,14,2)) names
 * the FIVE-buffer header-split layout -- the moment the MTU is raised to a jumbo
 * value, and stays in it. Its descriptor is 64 bytes: three control words and
 * five buffer pointers, where one-buffer mode's is 32 bytes with one pointer. A
 * model walking the ring at the one-buffer stride reads the wrong words, never
 * finds an owned descriptor again, and receive stops for good -- an interface
 * still reading UP while silently receiving nothing, not recovering when the
 * MTU is put back.
 *
 * The card splits a received frame across the buffers by protocol layer: the
 * Ethernet header into buffer 0, the IP and transport headers into buffer 1, and
 * the payload from buffer 2 on. Observed on IRIX 6.5.30, the descriptors the
 * driver posts carry buffer sizes 14, 128 and 9738 -- exactly an Ethernet
 * header, room for the upper headers, and a jumbo payload -- in the field
 * positions the public RxD three-buffer size macros give (vBIT(val,8,8),
 * vBIT(val,16,16), vBIT(val,32,16)). A frame that fits buffer 2 never needs
 * buffers 3 and 4, so those pointers are read but not filled here.
 *
 * NOT the same as Linux's RXD_MODE_3B ("two buffer mode", ring-mode value 1),
 * which posts a one-byte dummy for its middle buffer and never reads it -- an
 * earlier version of this code took that dummy for the rule and left buffer 1
 * empty, which corrupted every classified frame because the driver's checksum-
 * offload path reads the transport headers from buffer 1 where the card puts
 * them. Only modes 0 and 2 are implemented; 1 and 3 keep the LOG_UNIMP below.
 */
#define XFRAME_RXD_MODE(ctrl0)       (((ctrl0) >> 48) & 3)

#define XFRAME_RXD5_SIZE             64
#define XFRAME_RXD5_PER_PAGE         63

#define XFRAME_RXD5_BUF0_SIZE(c2)    (((c2) >> 48) & 0xff)
#define XFRAME_RXD5_BUF1_SIZE(c2)    (((c2) >> 32) & 0xffff)
#define XFRAME_RXD5_BUF2_SIZE(c2)    (((c2) >> 16) & 0xffff)
#define XFRAME_RXD5_SET_SIZES(b0, b1, b2)                                   \
    ((((uint64_t)(b0) & 0xff) << 48) | (((uint64_t)(b1) & 0xffff) << 32) |  \
     (((uint64_t)(b2) & 0xffff) << 16))

/* Geometry of the ring the guest has actually armed. */
static bool xframe_rx_header_split(XframeState *s)
{
    return XFRAME_RXD_MODE(s->prc_ctrl_0) == XFRAME_RXD_MODE_5BUF;
}

static unsigned xframe_rxd_size(XframeState *s)
{
    return xframe_rx_header_split(s) ? XFRAME_RXD5_SIZE : XFRAME_RXD_SIZE;
}

static unsigned xframe_rxd_per_page(XframeState *s)
{
    return xframe_rx_header_split(s) ? XFRAME_RXD5_PER_PAGE
                                     : XFRAME_RXD1_PER_PAGE;
}
#define XFRAME_RXD1_PAGE_SIZE        4096
#define XFRAME_RXD1_PAGE_NEXT_OFF    (XFRAME_RXD1_PAGE_SIZE - 8)

/*
 * RX_TRAFFIC_INT's ring-0 completion bit (Linux s2io-regs.h
 * RX_TRAFFIC_INT_n(0), the driver's own MSB-first bit numbering: bit 63).
 * Only ring 0 is modelled.
 */

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

    xframe_rx_cache_invalidate(s);
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
 * A successful fetch is retained while this head remains card-owned. QEMU's
 * normal delivery path calls can_receive immediately before receive, and the
 * ownership contract means the guest cannot rewrite that descriptor between
 * those callbacks. A queued retry bypasses can_receive and fetches here once.
 * Every device-side input to the address, layout, or byte order is part of the
 * match key and its write path invalidates the cache as well.
 *
 * Ring-index hygiene: the in-page position is reduced modulo the page's
 * descriptor count before any walk, the sgi-ioc3-eth/qlogic-isp pattern --
 * so a stuck or corrupted position can never read past the page.
 */
static bool xframe_rx_head(XframeState *s, uint64_t *desc_addr,
                           uint8_t raw[XFRAME_RXD5_SIZE])
{
    PCIDevice *pdev = PCI_DEVICE(s);
    unsigned desc_size;

    if (!(s->prc_ctrl_0 & XFRAME_RC_IN_SVC)) {
        xframe_rx_cache_invalidate(s);
        return false;
    }

    s->rx_desc_idx %= xframe_rxd_per_page(s);
    desc_size = xframe_rxd_size(s);
    *desc_addr = s->rx_page_addr +
                 (uint64_t)s->rx_desc_idx * desc_size;

    if (s->rx_desc_cache_valid &&
        s->rx_desc_cache_page_addr == s->rx_page_addr &&
        s->rx_desc_cache_addr == *desc_addr &&
        s->rx_desc_cache_prc_ctrl == s->prc_ctrl_0 &&
        s->rx_desc_cache_swapper ==
            (s->swapper_ctrl & XFRAME_SWAPPER_CTRL_RXD_R_SE) &&
        s->rx_desc_cache_idx == s->rx_desc_idx &&
        s->rx_desc_cache_size == desc_size) {
        memcpy(raw, s->rx_desc_cache, desc_size);
        return true;
    }

    xframe_rx_cache_invalidate(s);

    trace_xframe_rx_descriptor_fetch(*desc_addr, desc_size);
    if (pci_dma_read(pdev, *desc_addr, raw, desc_size) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xframe: receive descriptor read at 0x%" PRIx64
                      " failed\n", *desc_addr);
        return false;
    }

    if (!(xframe_desc_ldq(s, raw + 8, true) & XFRAME_RXD_OWN)) {
        return false;
    }

    memcpy(s->rx_desc_cache, raw, desc_size);
    s->rx_desc_cache_page_addr = s->rx_page_addr;
    s->rx_desc_cache_addr = *desc_addr;
    s->rx_desc_cache_prc_ctrl = s->prc_ctrl_0;
    s->rx_desc_cache_swapper =
        s->swapper_ctrl & XFRAME_SWAPPER_CTRL_RXD_R_SE;
    s->rx_desc_cache_idx = s->rx_desc_idx;
    s->rx_desc_cache_size = desc_size;
    s->rx_desc_cache_valid = true;
    return true;
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
    uint8_t raw[XFRAME_RXD5_SIZE];
    bool owned;

    if (s->defer_backend_rx) {
        s->backend_rx_deferred = true;
        return false;
    }

    owned = xframe_rx_head(s, &desc_addr, raw);

    if (!owned) {
        xframe_rx_arm_recovery(s);
    }

    return owned;
}

static void xframe_rx_flush(void *opaque)
{
    XframeState *s = opaque;

    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

static bool xframe_rx_completion_write(XframeState *s, uint64_t addr,
                                       const uint8_t word[8])
{
    if (pci_dma_write(PCI_DEVICE(s), addr, word, 8) == MEMTX_OK) {
        return true;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "xframe: receive completion write at 0x%" PRIx64
                  " failed; frame retained\n", addr);
    xframe_rx_arm_recovery(s);
    return false;
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
    size_t orig_size = size;
    uint8_t min_pkt[ETH_ZLEN];
    size_t min_pktsz = sizeof(min_pkt);
    uint64_t desc_addr, control2;
    uint64_t bufaddr3[3] = { 0, 0, 0 };
    size_t bufcap[3] = { 0, 0, 0 };
    size_t bufgot[3] = { 0, 0, 0 };
    size_t seg[3] = { 0, 0, 0 };
    uint64_t offload = 0;
    size_t l34 = 0;
    bool split_headers;
    size_t eth;
    unsigned nbuf, i;
    size_t placed;
    uint8_t raw[XFRAME_RXD5_SIZE];
    size_t capacity, copy_len;
    unsigned xcode;
    uint8_t word[8];

    if (s->has_vnet_hdr) {
        if (size <= sizeof(struct virtio_net_hdr)) {
            return orig_size;
        }
        buf += sizeof(struct virtio_net_hdr);
        size -= sizeof(struct virtio_net_hdr);
    }

    /* The backend may have counted a vnet header toward Ethernet padding. */
    if (eth_pad_short_frame(min_pkt, &min_pktsz, buf, size)) {
        buf = min_pkt;
        size = min_pktsz;
    }

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
        return orig_size;
    }

    control2 = xframe_desc_ldq(s, raw + 16, true);

    copy_len = size;
    if (copy_len > XFRAME_MAX_FRAME) {
        /*
         * Past what the card can hold at all. Say so rather than quietly
         * handing the guest a short frame -- a truncation nothing reports is
         * indistinguishable downstream from data that was never sent.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "xframe: received frame of %zu bytes exceeds the %u-byte "
                      "maximum; truncated\n", size, XFRAME_MAX_FRAME);
        copy_len = XFRAME_MAX_FRAME;
    }

    /*
     * Parse the frame once. Complete TCP/UDP frames split their IP and
     * transport headers into buffer 1; IPv4 fragments split only their IP
     * header because their transport segment is incomplete.
     */
    offload = xframe_rx_offload_status(buf, copy_len, &l34);
    split_headers = l34 > 0;

    if (xframe_rx_header_split(s)) {
        /*
         * Five-buffer header-split mode. The card splits a frame by protocol
         * layer: the Ethernet header into buffer 0, the IP and transport headers
         * into buffer 1, the payload from buffer 2 on. Buffers 3 and 4 exist
         * (five pointers) but a frame that fits buffer 2 never needs them, so
         * they are read for stride but not filled.
         */
        nbuf = 3;
        bufcap[0] = XFRAME_RXD5_BUF0_SIZE(control2);
        bufcap[1] = XFRAME_RXD5_BUF1_SIZE(control2);
        bufcap[2] = XFRAME_RXD5_BUF2_SIZE(control2);
        bufaddr3[0] = xframe_desc_ldq(s, raw + 24, true);
        bufaddr3[1] = xframe_desc_ldq(s, raw + 32, true);
        bufaddr3[2] = xframe_desc_ldq(s, raw + 40, true);

        eth = MIN((size_t)XFRAME_ETH_HDR_LEN, copy_len);
        if (split_headers && eth == XFRAME_ETH_HDR_LEN &&
            eth <= bufcap[0] && l34 <= bufcap[1] && eth + l34 <= copy_len) {
            /*
             * Header split: L2 -> buf0, L3+L4 -> buf1, payload -> buf2. The
             * room this layout really offers is what its own three legs hold,
             * not the sum of the posted sizes -- the unclassified layout below
             * uses none of buffer 1, and this one uses exactly l34 of it.
             */
            capacity = eth + l34 + bufcap[2];
            xcode = XFRAME_RXD_XCODE_OK;
            if (copy_len > capacity) {
                /*
                 * A frame past buffer 2's room would continue into buffers 3
                 * and 4 on the real card. IRIX posts a buffer 2 larger than
                 * any frame its own MTU allows, so nothing reaches this; say
                 * so rather than silently truncating if something ever does.
                 */
                qemu_log_mask(LOG_GUEST_ERROR,
                              "xframe: %zu-byte receive frame needs the "
                              "overflow buffers (room %zu); truncated\n",
                              copy_len, capacity);
                xcode = XFRAME_RXD_XCODE_BSZ;
                copy_len = capacity;
            }
            seg[0] = eth;
            seg[1] = l34;
            seg[2] = copy_len - eth - l34;
        } else {
            /*
             * Unclassified (no IP or transport headers to split -- ARP), or a
             * parsed frame whose headers do not fit buffer 1. Deliver it
             * unsplit -- L2 header to buffer 0, the remainder to buffer 2 -- and
             * do NOT report offload, since reporting it would claim a placement
             * in buffer 1 the card did not make. IRIX never posts a buffer 1
             * too small for a real header, so the second case is a guest-error,
             * not a normal path.
             */
            if (split_headers) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "xframe: header-split receive: %zu-byte headers do "
                              "not fit buffer 1 (%zu); delivered unsplit\n",
                              l34, bufcap[1]);
                offload = 0;
            }
            seg[0] = MIN(eth, bufcap[0]);
            seg[1] = 0;
            capacity = seg[0] + bufcap[2];
            xcode = XFRAME_RXD_XCODE_OK;
            if (copy_len > capacity) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "xframe: %zu-byte receive frame needs the "
                              "overflow buffers (room %zu); truncated\n",
                              copy_len, capacity);
                xcode = XFRAME_RXD_XCODE_BSZ;
                copy_len = capacity;
            }
            seg[2] = copy_len - seg[0];
        }
    } else {
        nbuf = 1;
        bufcap[0] = (control2 >> XFRAME_RXD_BUFSIZ_SHIFT) & 0xffff;
        bufaddr3[0] = xframe_desc_ldq(s, raw + 24, true);
        capacity = bufcap[0];

        xcode = XFRAME_RXD_XCODE_OK;
        if (copy_len > capacity) {
            xcode = XFRAME_RXD_XCODE_BSZ;
            copy_len = capacity;
        }
        seg[0] = copy_len;
    }

    for (i = 0, placed = 0; i < nbuf; i++) {
        if (seg[i] == 0) {
            bufgot[i] = 0;
            continue;
        }
        if (pci_dma_write(pdev, bufaddr3[i], buf + placed, seg[i]) != MEMTX_OK) {
            /*
             * The descriptor itself was fine; a buffer pointer it named was
             * not. Release the descriptor and advance past it below rather
             * than leaving it parked here -- the same bad pointer would fail
             * identically on every future frame, permanently stalling the ring
             * behind one bad buffer.
             */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "xframe: receive buffer write at 0x%" PRIx64
                          " failed; frame dropped\n", bufaddr3[i]);
            placed = 0;
            xcode = XFRAME_RXD_XCODE_ABORT;
            offload = 0;
            break;
        }
        bufgot[i] = seg[i];
        placed += seg[i];
    }
    copy_len = placed;

    if (xframe_rx_header_split(s)) {
        /*
         * Five-buffer completion also reports the byte counts of buffers 3 and
         * 4, in the low two 16-bit fields of the first descriptor word -- the
         * same fields the guest posts their sizes in (observed: IRIX posts two
         * real 4-byte buffers there, sizes 0x0004/0x0004, below an incrementing
         * index in the upper half). A frame that fits buffer 2 puts nothing in
         * them, and the count must SAY so: left unwritten, the driver reads the
         * posted sizes back as counts, sums a frame 8 bytes longer than the
         * wire's, and rejects every frame as an input error -- receive dead at
         * jumbo MTU with the bytes themselves delivered perfectly.
         * The upper half is the guest's own bookkeeping and is preserved.
         *
         * RULE, not an optimisation: this word is written ONLY in five-buffer
         * mode. In the other ring modes it is host-private -- the public Linux
         * driver keeps its skb pointer there and the card never touches it --
         * so generalising this writeback to another mode would corrupt a
         * driver pointer. (Control_2's own low 16 bits are posted as zero by
         * IRIX in this mode -- observed 0x000e008026120000 -- so the size
         * writeback below zeroing them rewrites the posted value unchanged.)
         */
        uint64_t hc = xframe_desc_ldq(s, raw, true);

        hc &= ~0xffffffffULL;   /* buffers 3 and 4 received nothing */
        xframe_desc_stq(s, word, hc, true);
        if (!xframe_rx_completion_write(s, desc_addr, word)) {
            return 0;
        }

        xframe_desc_stq(s, word,
                        XFRAME_RXD5_SET_SIZES(bufgot[0], bufgot[1], bufgot[2]),
                        true);
    } else {
        xframe_desc_stq(s, word,
                        (uint64_t)copy_len << XFRAME_RXD_BUFSIZ_SHIFT, true);
    }
    if (!xframe_rx_completion_write(s, desc_addr + 16, word)) {
        return 0;
    }

    xframe_desc_stq(s, word,
                    ((uint64_t)xcode << XFRAME_RXD_XCODE_SHIFT) | offload, true);
    if (!xframe_rx_completion_write(s, desc_addr + 8, word)) {
        return 0;
    }
    xframe_rx_cache_invalidate(s);

    /*
     * Count only delivered frames. A frame the receive path drops earlier for
     * a bad buffer pointer is not counted here; on the hardware it would land
     * in a discard counter this model leaves zero, so netstat's received total
     * omits it rather than miscounting it.
     */
    if (copy_len > 0) {
        s->stat_rx_frms++;
        s->stat_rx_octets += copy_len;
    }

    s->rx_desc_idx++;
    if (s->rx_desc_idx >= xframe_rxd_per_page(s)) {
        xframe_rx_advance_page(s);
    }

    xframe_traffic_complete(s, true, 0);
    timer_del(s->rx_recovery_timer);

    return orig_size;
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
 * QEMU's own netdev backend link:
 * this is the standard NetClientInfo hook a `set_link` QMP command (or any
 * other backend disconnect) invokes on NetClientState::link_down changing.
 * Without it the device never looked at that flag at all -- it announced
 * LINK_UP on the ADAPTER_EN bring-up edge regardless of whether the backend
 * was actually connected, so an operator-downed netdev still showed carrier
 * to the guest.
 *
 * Backend down: present LINK_DOWN on the PIC/GPIO path and cancel any pending
 * link timer so it cannot announce UP after the backend disconnected.
 * This also raises the MAC-block link-state-change latch the way a genuine
 * link event does, for a driver (OpenBSD xge) that takes its link interrupt
 * through that path instead of the PIC/GPIO one.
 *
 * Backend back up while the adapter is enabled: schedule the same delayed UP
 * event as a rising ADAPTER_EN edge. If the guest has not enabled the adapter,
 * its later enable schedules the event instead.
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
        timer_mod(s->link_up_timer,
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
    if (s->tx_pkt) {
        net_tx_pkt_reset(s->tx_pkt, xframe_tx_pkt_release, NULL);
    }
    pci_set_irq(PCI_DEVICE(s), 0);
}

static void xframe_realize(PCIDevice *dev, Error **errp)
{
    XframeState *s = XFRAME(dev);
    DeviceState *d = DEVICE(dev);
    NetClientState *nc;
    unsigned i;

    s->rx_recovery_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                        xframe_rx_recovery_timer_cb, s);
    s->stat_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, xframe_stat_timer_cb, s);
    s->link_down_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                      xframe_link_down_timer_cb, s);
    s->link_up_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                    xframe_link_up_timer_cb, s);
    s->sched_int_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                      xframe_sched_int_cb, s);
    for (i = 0; i < XFRAME_TRAFFIC_MEM_ENTRIES; i++) {
        s->tti_batch[i].owner = s;
        s->tti_batch[i].entry = i;
        s->tti_batch[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                             xframe_traffic_timer_cb,
                                             &s->tti_batch[i]);
        s->rti_batch[i].owner = s;
        s->rti_batch[i].entry = i;
        s->rti_batch[i].rx = true;
        s->rti_batch[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                             xframe_traffic_timer_cb,
                                             &s->rti_batch[i]);
        s->tti_continuous[i].owner = s;
        s->tti_continuous[i].entry = i;
        s->tti_continuous[i].timer = timer_new_ns(
            QEMU_CLOCK_VIRTUAL, xframe_traffic_continuous_cb,
            &s->tti_continuous[i]);
        s->rti_continuous[i].owner = s;
        s->rti_continuous[i].entry = i;
        s->rti_continuous[i].rx = true;
        s->rti_continuous[i].timer = timer_new_ns(
            QEMU_CLOCK_VIRTUAL, xframe_traffic_continuous_cb,
            &s->rti_continuous[i]);
    }

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
    nc = qemu_get_queue(s->nic);
    s->has_vnet_hdr = qemu_has_vnet_hdr(nc->peer);
    if (s->has_vnet_hdr) {
        qemu_set_vnet_hdr_len(nc->peer, sizeof(struct virtio_net_hdr));
    }
    net_tx_pkt_init(&s->tx_pkt, 1);
    s->rx_flush_bh = qemu_bh_new_guarded(xframe_rx_flush, s,
                                         &d->mem_reentrancy_guard);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void xframe_exit(PCIDevice *dev)
{
    XframeState *s = XFRAME(dev);
    unsigned i;

    xframe_rx_cache_invalidate(s);
    qemu_bh_delete(s->rx_flush_bh);
    timer_free(s->rx_recovery_timer);
    timer_free(s->stat_timer);
    timer_free(s->link_down_timer);
    timer_free(s->link_up_timer);
    timer_free(s->sched_int_timer);
    for (i = 0; i < XFRAME_TRAFFIC_MEM_ENTRIES; i++) {
        timer_free(s->tti_batch[i].timer);
        timer_free(s->rti_batch[i].timer);
        timer_free(s->tti_continuous[i].timer);
        timer_free(s->rti_continuous[i].timer);
    }
    net_tx_pkt_reset(s->tx_pkt, xframe_tx_pkt_release, NULL);
    net_tx_pkt_uninit(s->tx_pkt);
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
    unsigned i;

    /* The cache is local prefetch state, never part of the migration stream. */
    xframe_rx_cache_invalidate(s);

    if (version_id < 8) {
        s->legacy_interrupt_masking = true;
    }

    /*
     * A stream older than version 3 never carried RMAC_MAX_PYLD_LEN, so it
     * arrives zero -- which would read back as "accept a payload of nothing".
     * Present the reset default instead: a source that predates the field was
     * running the 1500-byte default anyway, since nothing could have raised it.
     */
    if (version_id < 3) {
        s->rmac_max_pyld_len =
            (uint64_t)XFRAME_MAX_PYLD_DEFAULT << XFRAME_MAX_PYLD_SHIFT;
    }

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

    /*
     * Version 5 saved the traffic memories before the model used them for
     * interrupt moderation. Retaining those entries would make future
     * completions on a version 6 destination behave differently from the
     * immediate interrupts produced by the source.
     */
    if (version_id < 6) {
        s->tti_command = 0;
        s->tti_data1 = 0;
        s->tti_data2 = 0;
        s->rti_command = 0;
        s->rti_data1 = 0;
        s->rti_data2 = 0;
        memset(s->tti_entry_data1, 0, sizeof(s->tti_entry_data1));
        memset(s->tti_entry_data2, 0, sizeof(s->tti_entry_data2));
        memset(s->tti_entry_valid, 0, sizeof(s->tti_entry_valid));
        memset(s->rti_entry_data1, 0, sizeof(s->rti_entry_data1));
        memset(s->rti_entry_data2, 0, sizeof(s->rti_entry_data2));
        memset(s->rti_entry_valid, 0, sizeof(s->rti_entry_valid));
        for (i = 0; i < XFRAME_TRAFFIC_MEM_ENTRIES; i++) {
            xframe_traffic_batch_reset(&s->tti_batch[i]);
            xframe_traffic_batch_reset(&s->rti_batch[i]);
        }
    }

    if (s->sched_int_armed && !timer_pending(s->sched_int_timer)) {
        xframe_sched_int_arm(s);
    }
    /*
     * Periodic statistics DMA resumes on the target if the source had it
     * enabled. Only periodic mode leaves enable set to migrate -- a one-shot
     * clears it when its single transfer completes -- so the enable bit alone
     * is the condition. A v3 or older source never programmed it, and the
     * zero-value fields load as disabled, so this arms nothing there.
     */
    if ((s->stat_cfg & XFRAME_STAT_CFG_EN) && !timer_pending(s->stat_timer)) {
        timer_mod(s->stat_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  xframe_stat_period_ms(s));
    }
    /* Replay the restored level through the destination PCI bus. */
    pci_device_deassert_intx(PCI_DEVICE(s));
    xframe_update_irq(s);
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
 * (gpio_int_reg, the link-state status, and gpio_int_mask, retained for
 * readback; pic_int_mask's PIC_INT_GPIO bit gates delivery); the current
 * link-up timer and the
 * legacy link-down timer retained for existing migration streams; and the
 * scheduled interrupt that replaced the earlier
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
 *
 * Version 8 records whether the device was loaded from a version-7 or older
 * stream. Those sources gated INTx through GENERAL_INT_MASK and gated GPIO
 * link leaves through GPIO_INT_MASK. Retaining that behavior avoids changing
 * an already-running guest's interrupt visibility during migration; a fresh
 * version-8 device uses the readback-only behavior required by IRIX xg.
 */
static const VMStateDescription vmstate_xframe_traffic_batch = {
    .name = "xframe/traffic-batch",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, XframeTrafficBatch),
        VMSTATE_UINT32(completions, XframeTrafficBatch),
        VMSTATE_BOOL(deferred, XframeTrafficBatch),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_xframe_traffic_continuous = {
    .name = "xframe/traffic-continuous",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, XframeTrafficContinuous),
        VMSTATE_INT64(deadline_ns, XframeTrafficContinuous),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_xframe = {
    .name = "xframe",
    .version_id = 8,
    .minimum_version_id = 1,
    .post_load = xframe_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, XframeState),
        VMSTATE_UINT64(swapper_ctrl, XframeState),
        VMSTATE_UINT64(xmsi_address, XframeState),
        VMSTATE_UINT64(sw_reset, XframeState),
        VMSTATE_UINT64(adapter_control, XframeState),
        VMSTATE_UINT64(general_int_mask, XframeState),
        VMSTATE_UINT64_V(rmac_max_pyld_len, XframeState, 3),
        VMSTATE_UINT64_V(stat_cfg, XframeState, 4),
        VMSTATE_UINT64_V(stat_addr, XframeState, 4),
        VMSTATE_UINT64_V(stat_tx_frms, XframeState, 4),
        VMSTATE_UINT64_V(stat_tx_octets, XframeState, 4),
        VMSTATE_UINT64_V(stat_rx_frms, XframeState, 4),
        VMSTATE_UINT64_V(stat_rx_octets, XframeState, 4),
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
        VMSTATE_UINT64_V(tti_command, XframeState, 5),
        VMSTATE_UINT64_V(tti_data1, XframeState, 5),
        VMSTATE_UINT64_V(tti_data2, XframeState, 5),
        VMSTATE_UINT64_ARRAY_V(tti_entry_data1, XframeState,
                               XFRAME_TRAFFIC_MEM_ENTRIES, 5),
        VMSTATE_UINT64_ARRAY_V(tti_entry_data2, XframeState,
                               XFRAME_TRAFFIC_MEM_ENTRIES, 5),
        VMSTATE_BOOL_ARRAY_V(tti_entry_valid, XframeState,
                             XFRAME_TRAFFIC_MEM_ENTRIES, 5),
        VMSTATE_UINT64_V(rti_command, XframeState, 5),
        VMSTATE_UINT64_V(rti_data1, XframeState, 5),
        VMSTATE_UINT64_V(rti_data2, XframeState, 5),
        VMSTATE_UINT64_ARRAY_V(rti_entry_data1, XframeState,
                               XFRAME_TRAFFIC_MEM_ENTRIES, 5),
        VMSTATE_UINT64_ARRAY_V(rti_entry_data2, XframeState,
                               XFRAME_TRAFFIC_MEM_ENTRIES, 5),
        VMSTATE_BOOL_ARRAY_V(rti_entry_valid, XframeState,
                             XFRAME_TRAFFIC_MEM_ENTRIES, 5),
        VMSTATE_STRUCT_ARRAY(tti_batch, XframeState,
                             XFRAME_TRAFFIC_MEM_ENTRIES, 6,
                             vmstate_xframe_traffic_batch,
                             XframeTrafficBatch),
        VMSTATE_STRUCT_ARRAY(rti_batch, XframeState,
                             XFRAME_TRAFFIC_MEM_ENTRIES, 6,
                             vmstate_xframe_traffic_batch,
                             XframeTrafficBatch),
        VMSTATE_STRUCT_ARRAY(tti_continuous, XframeState,
                             XFRAME_TRAFFIC_MEM_ENTRIES, 7,
                             vmstate_xframe_traffic_continuous,
                             XframeTrafficContinuous),
        VMSTATE_STRUCT_ARRAY(rti_continuous, XframeState,
                             XFRAME_TRAFFIC_MEM_ENTRIES, 7,
                             vmstate_xframe_traffic_continuous,
                             XframeTrafficContinuous),
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
        VMSTATE_BOOL_V(legacy_interrupt_masking, XframeState, 8),
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
