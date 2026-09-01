/*
 * Neterion/S2io Xframe: the register-contract qtest, on a generic PCI machine.
 *
 * The Xframe is a plain PCI add-in card: its register contract and DMA are
 * machine-independent, so this test drives the device on a generic x86_64
 * host (q35/pc, the off-fabric suite), NOT behind the Origin 200 Bridge. On a
 * little-endian host an 8-byte register access is a single native cycle, so
 * the device's own 64-bit contract is exercised directly, with no Bridge
 * byte-swap or access-width composition in the way -- those are a separate
 * machine-level concern tested elsewhere (docs/library/
 * nic-on-bridge-conventions.md). A device that passes here is correct as a
 * general-purpose PCI endpoint; how it composes with the Bridge on the Origin
 * 200 is asserted by the on-Bridge harness, not here.
 *
 * What this asserts:
 *  - PCI enumeration: identity (vendor/device, the device-id property
 *    selecting Herc), the subsystem pair (unbranded by default, SGI's vendor
 *    when the board property is set), and the two BAR windows' type and size.
 *  - The PIF (BAR0) register file the probe/attach path binds on: the
 *    byte-swapper diagnostic returning its fixed constant, the XMSI_ADDRESS
 *    scratch, SW_RESET's key, the RMAC_CFG_KEY MAC-config lock, ADAPTER_STATUS
 *    presenting its quiescent bits immediately, ADAPTER_CONTROL/ADAPTER_EN,
 *    and the factory-MAC indirect read returning the configured netdev MAC.
 *  - The transmit descriptor list (the TxP window): a doorbell emitting a
 *    golden frame, and the out-of-range-pointer hang guard.
 *  - The receive descriptor ring (the RxDMA registers plus the RxD ring in
 *    guest memory): a golden delivered frame with its completion status and
 *    interrupt, the out-of-range ring-pointer hang guard, the
 *    filtered-frame-still-counts-as-consumed hang guard, a buffer smaller
 *    than the frame truncating rather than overrunning, a ring exhausted
 *    then refilled in place (real driver order: the interrupt-ACK write
 *    first, then the refill) recovering through the device's own
 *    recovery-poll timer, and the multicast CAM: a joined group delivered
 *    while a different group is filtered, the IFF_ALLMULTI wildcard
 *    matching an arbitrary group, and unicast still working unregressed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqos/libqos-pc.h"
#include "libqtest.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_regs.h"
#include <sys/socket.h>
#include <netinet/in.h>

/*
 * The card is instantiated with an explicit MAC so the factory-MAC read-back
 * assertion has a known value to check. The value is packed into
 * RMAC_ADDR_DATA0_MEM bits 63:16, most-significant byte first (the order both
 * drivers unpack it in) -- see XFRAME_TEST_MAC_DATA0.
 */
#define XFRAME_TEST_MAC          "52:54:00:12:34:56"
#define XFRAME_TEST_MAC_DATA0    0x5254001234560000ULL
#define XFRAME_CARD              "-device xframe,mac=" XFRAME_TEST_MAC

/*
 * Identity a plain (no SGI properties) card presents --
 * docs/datasheets/xframe.md's "PCI identity": vendor 0x17d5, device 0x5831
 * (Xframe I / Xena, SGI Release 1); 0x5832 is Xframe II / Herc.
 */
#define XFRAME_VENDOR                0x17d5
#define XFRAME_DEVICE_XENA           0x5831
#define XFRAME_DEVICE_HERC           0x5832
#define XFRAME_SGI_SUBSYSTEM_VENDOR  0x10a9

/*
 * BAR windows, restated from docs/datasheets/xframe.md rather than derived
 * here: a test that recomputes the implementation's arithmetic checks that
 * the arithmetic ran, not that it is right.
 */
#define XFRAME_PIF_WINDOW_SIZE   0x4000    /* BAR0, spans >= 0x0000-0x33ff */
#define XFRAME_TXP_WINDOW_SIZE   0x100000  /* second BAR, 8 * 0x00020000 */

/*
 * PIF register offsets within BAR0 (docs/datasheets/xframe.md's register map;
 * public OpenBSD if_xgereg.h / Linux s2io-regs.h names). Restated here rather
 * than shared with the device so the test pins the contract independently.
 */
#define GENERAL_INT_STATUS       0x0000
#define GENERAL_INT_MASK         0x0008
#define SW_RESET                 0x0100
#define ADAPTER_STATUS           0x0108
#define ADAPTER_CONTROL          0x0110
#define SWAPPER_CTRL             0x0908
#define PIF_RD_SWAPPER_FB        0x0910
#define XMSI_ADDRESS             0x0960
#define MAC_RMAC_ERR_REG         0x2028
#define MAC_CFG                  0x2100
#define RMAC_CFG_KEY             0x2120
#define RMAC_ADDR_CMD_MEM        0x2128
#define RMAC_ADDR_DATA0_MEM      0x2130
#define RMAC_ADDR_DATA1_MEM      0x2138

/*
 * The fixed constant the byte-swapper diagnostic returns at a correctly
 * ordered 64-bit read (if_xgereg.h SWAPPER_MAGIC).
 */
#define SWAPPER_MAGIC            0x0123456789abcdefULL

/*
 * SWAPPER_CTRL's per-DMA-path swap-enable (SE) bits that select descriptor
 * byte order (Linux s2io-regs.h SWAPPER_CTRL_TXD_R_SE = s2BIT(19) = bit 44,
 * SWAPPER_CTRL_RXD_R_SE = s2BIT(33) = bit 30, s2io.h's s2BIT(loc) = bit
 * 63-loc). Clear -- the reset default -- is big-endian; setting a type's SE
 * bit switches that type's descriptors to little-endian. See
 * hw/net/xframe.c's xframe_desc_ldq/xframe_desc_stq.
 */
#define SWAPPER_CTRL_TXD_R_SE    (1ULL << 44)
#define SWAPPER_CTRL_RXD_R_SE    (1ULL << 30)

/*
 * ADAPTER_STATUS quiescent/ready value the driver's single non-retrying
 * attach read requires (if_xgereg.h QUIESCENT): TDMA/RDMA/PFC ready, TMAC
 * buffer empty, PIC quiescent, memory-controller DRAM+queues ready, both PLLs
 * locked, subsystem-quiescence/ready bits 8-15 of the high word -- and the
 * fault bits clear.
 */
#define ADAPTER_STATUS_QUIESCENT 0xf400ffc300000000ULL

#define ADAPTER_EN               (1ULL << 56)  /* ADAPTER_CONTROL enable bit */

/*
 * Interrupt aggregates and the link-state-change source
 * (docs/datasheets/xframe.md's "Interrupts"; public OpenBSD if_xgereg.h /
 * Linux s2io-regs.h names and MSB-first bit numbering, s2BIT(loc) = bit
 * 63-loc). The link-state-change interrupt rolls up into GENERAL_INT_STATUS's
 * receive-MAC aggregate (GEN_INTR_RXMAC = s2BIT(34) = bit 29); the
 * receive-traffic aggregate the RX tests exercise is s2BIT(40) = bit 23. The
 * link bit itself lives in MAC_RMAC_ERR_REG (OpenBSD RMAC_LINK_STATE_CHANGE_INT
 * = 1ULL<<32; Linux s2BIT(31), the same bit).
 */
#define GEN_INTR_RXMAC             (1ULL << 29)
#define RMAC_LINK_STATE_CHANGE_INT (1ULL << 32)

/*
 * Per-block (second-level) interrupt masks (public OpenBSD if_xgereg.h offsets:
 * PCIXB(x) = 0x0800 + x, MACRB(x) = 0x2000 + x). These are what the drivers
 * program to select interrupt sources, and -- as the IRIX xg enable path shows
 * -- what actually gates INTx: xg unmasks here and leaves GENERAL_INT_MASK at
 * its all-ones reset, yet still takes interrupts on real hardware, so
 * GENERAL_INT_MASK gates only the GENERAL_INT_STATUS summary read, not INTx.
 * The link source rolls up through two MAC masks: MAC_RMAC_ERR_MASK (the leaf
 * link-state-change bit) and MAC_INT_MASK (the RMAC block bit); both are
 * bit-set-means-masked, and DISABLE_ALL_INTRS_64 masks everything.
 */
#define PIC_INT_MASK             0x0808
#define TX_TRAFFIC_MASK          0x08e8
#define RX_TRAFFIC_MASK          0x08f8
#define MAC_INT_MASK             0x2008
#define MAC_TMAC_INT             (1ULL << 63)  /* transmit-MAC roll-up bit */
#define MAC_RMAC_INT             (1ULL << 62)  /* receive-MAC roll-up bit */
#define MAC_RMAC_ERR_MASK        0x2030
#define DISABLE_ALL_INTRS_64     0xffffffffffffffffULL  /* all masked */

/*
 * The PIC/GPIO-block link path IRIX xg enables (Linux s2io-regs.h names and
 * MSB-first bit numbering). GPIO_INT_REG (0x890) carries the link-state bits
 * (GPIO_INT_REG_LINK_UP = s2BIT(2) = bit 61, GPIO_INT_REG_LINK_DOWN = s2BIT(1)
 * = bit 62); GPIO_INT_MASK (0x898) is its leaf mask (same bit positions);
 * PIC_INT_STATUS (0x800) rolls the sub-blocks up (PIC_INT_GPIO = s2BIT(4) =
 * bit 59), gated by PIC_INT_MASK (0x808); and the whole PIC block rolls up into
 * GENERAL_INT_STATUS's GEN_INTR_TXPIC = s2BIT(0) = bit 63. This is a different
 * link source from the MAC RMAC path above: xg opens the PIC block
 * (PIC_INT_MASK <- 0) and leaves the MAC masks masked, so this is the path its
 * bring-up waits on.
 */
#define PIC_INT_STATUS           0x0800
#define TXPIC_INT_REG            0x0810   /* PCI-X transmit errors + sched int */
#define TXPIC_INT_MASK           0x0818   /* xg opens this leaf */
#define GPIO_INT_REG             0x0890
#define GPIO_INT_MASK            0x0898
#define PIC_INT_GPIO             (1ULL << 59)
#define PIC_INT_TX               (1ULL << 63)   /* PIC_INT_TX = s2BIT(0) */
#define GPIO_INT_REG_LINK_UP     (1ULL << 61)
#define GPIO_INT_REG_LINK_DOWN   (1ULL << 62)
#define GPIO_INT_MASK_LINK_UP    (1ULL << 61)
#define GEN_INTR_TXPIC           (1ULL << 63)

/*
 * The scheduled/periodic-interrupt timer (Linux s2io-regs.h scheduled_int_ctrl
 * at 0x918; SCHED_INT_CTRL_TIMER_EN = s2BIT(0) = bit 63 arms the timer). IRIX
 * xg programs this at bring-up to feed its "no interrupts" watchdog with a
 * traffic-independent periodic interrupt, unmasked through the PIC block
 * (PIC_INT_MASK's PIC_INT_TX bit) and the TXPIC leaf (TXPIC_INT_MASK). Its
 * source rolls up through TXPIC_INT_REG -> PIC_INT_STATUS's PIC_INT_TX ->
 * GENERAL_INT_STATUS's GEN_INTR_TXPIC, on the link-independent PIC/TXPIC path.
 * The public header marks the period field (SCHED_INT_PERIOD) TBD, so the test
 * writes any nonzero period alongside the enable bit; the model uses a fixed
 * virtual-time period under the watchdog interval.
 */
#define SCHED_INT_CTRL           0x0918
#define SCHED_INT_CTRL_TIMER_EN  (1ULL << 63)
#define SCHED_INT_TEST_PERIOD    0x00000000000186a0ULL  /* any nonzero period */

#define SW_RESET_KEY             0xa5a5a50000000000ULL  /* 0xa5a5a5 in 63:40 */
#define RMAC_CFG_KEY_VALUE       (0x4c0dULL << 48)      /* unlock key */
#define RMAC_ADDR_CMD_STROBE     (1ULL << 48)           /* self-clearing */
#define RMAC_ADDR_CMD_OFF(n)     ((uint64_t)(n) << 32)  /* table offset */
#define RMAC_ADDR_CMD_WE         (1ULL << 56)           /* write, not read */

/*
 * A MAC_CFG payload for the lock test -- TMAC_EN|RMAC_EN; any non-zero value
 * proves whether the write took.
 */
#define MAC_CFG_PAYLOAD          0xc000000000000000ULL

/*
 * Multicast CAM matching (docs/datasheets/xframe.md's "Known model gaps"):
 * both public drivers program a joined group as a masked entry in the same
 * 64-entry table the unicast/factory-MAC path already uses. The mask
 * (RMAC_ADDR_DATA1_MEM) lines up bit-for-bit with RMAC_ADDR_DATA0_MEM's
 * address field (bits 63:16, MSB-first, the same packing
 * XFRAME_TEST_MAC_DATA0 above uses) -- and a CLEAR mask bit means "must
 * match", not a set one. Confirmed from Linux s2io_set_multicast's own two
 * independent uses (drivers/net/ethernet/neterion/s2io.c): an individual
 * join writes DATA1_MEM_MASK(0) -- every bit compared, i.e. an exact
 * match -- and its IFF_ALLMULTI entry writes address 0x010203040506 with
 * mask 0xfeffffffffff, which under this polarity compares exactly one bit,
 * byte 0's bit 0 (the IEEE group/individual bit), against 1: "any multicast
 * destination, nothing else", with no slop. OpenBSD's own xge_setmulti
 * writes the opposite mask (0xFFFFFFFFFFFFFFFF) for an individual join and a
 * different, narrower IFF_ALLMULTI sentinel (0x8000...000/0xF000...000)
 * that does not reduce to "every multicast frame" under either mask
 * polarity with this packing -- checked both ways against the literal
 * values in if_xge.c, not against Linux's paraphrase. This device follows
 * Linux's convention; see xframe_rx_filter's own comment in xframe.c for
 * the full derivation.
 */
#define MCAST_GROUP_A_DATA0      0x01005e0000010000ULL  /* 01:00:5e:00:00:01 */
#define MCAST_GROUP_B_DATA0      0x01005e0000020000ULL  /* 01:00:5e:00:00:02 */
#define MCAST_EXACT_MASK         0ULL                   /* every bit compared */
#define MCAST_ALLMULTI_DATA0     0x0102030405060000ULL  /* Linux's sentinel */
#define MCAST_ALLMULTI_MASK      0xfeffffffffff0000ULL  /* only the group bit */

/*
 * Transmit path (BAR2 TxP window + the TxD ring in guest memory), from
 * docs/datasheets/xframe.md and public OpenBSD if_xgereg.h / Linux s2io.h.
 * The doorbell is a per-FIFO TxDL slot of 16 bytes: the list pointer (PAR) at
 * +0, the list control (LCR) at +8. Writing LCR launches the list.
 */
#define TXP_TXDL_PAR             0x00
#define TXP_TXDL_LCR             0x08

/*
 * LCR fields: NUMTXD (descriptor count minus one) in bits 63:56, plus the
 * first/last-list-gather flags.
 */
#define TXDL_NUMTXD(n)           ((uint64_t)(n) << 56)
#define TXDL_LGC_FIRST           (1ULL << 49)
#define TXDL_LGC_LAST            (1ULL << 48)

/*
 * Transmit completion interrupt (public OpenBSD if_xgereg.h / Linux
 * s2io-regs.h): a finished descriptor list raises TX_TRAFFIC_INT, whose
 * per-FIFO completion bit for FIFO 0 is bit 63 (TX_TRAFFIC_INT_n(0) =
 * s2BIT(0)), rolled up into GENERAL_INT_STATUS's transmit-traffic aggregate
 * (GEN_INTR_TXTRAFFIC = s2BIT(8) = bit 55). The transmit mirror of
 * RX_TRAFFIC_INT / RX_TRAFFIC_RING0 below.
 */
#define TX_TRAFFIC_INT           0x08e0
#define TX_TRAFFIC_FIFO0         (1ULL << 63)
#define GEN_INTR_TXTRAFFIC       (1ULL << 55)

/*
 * A TxD is four 64-bit words: control1, control2, buffer pointer, host
 * control. control1 carries the ownership bit (56, 0=host/1=card), the
 * first/last gather-frame flags (41/40), and the 16-bit buffer length in bits
 * 15:0 (OpenBSD stores the segment length there; Linux TXD_BUFFER0_SIZE).
 */
#define TXD_SIZE                 32
#define TXD_OWN                  (1ULL << 56)
#define TXD_GCF                  (1ULL << 41)
#define TXD_GCL                  (1ULL << 40)

/*
 * Guest RAM the transmit tests use for descriptors and frame buffers: well
 * above the first megabyte, in RAM nothing under qtest is using.
 */
#define TX_RAM_SCRATCH           0x02000000u

/*
 * Receive path (RxDMA registers in BAR0 + the RxD ring in guest memory), from
 * docs/datasheets/xframe.md and public OpenBSD if_xgereg.h. The ring base
 * register takes the first descriptor page; the ring controller is put in
 * service with RC_IN_SVC. A completed frame raises RX_TRAFFIC_INT.
 */
#define PRC_RXD0_0               0x1930   /* receive ring base register */
#define PRC_CTRL_0               0x1970   /* receive ring controller */
#define RC_IN_SVC                (1ULL << 56)
#define RX_TRAFFIC_INT           0x08f0

/*
 * RX_TRAFFIC_INT's ring-0 completion bit (Linux s2io-regs.h
 * RX_TRAFFIC_INT_n(0), the driver's own MSB-first bit numbering: bit 63).
 * Only ring 0 is modelled.
 */
#define RX_TRAFFIC_RING0         (1ULL << 63)

/*
 * How far to step QEMU_CLOCK_VIRTUAL (frozen under qtest until stepped
 * explicitly) to cross the device's own recovery-poll period
 * (XFRAME_RX_RECOVERY_PERIOD_MS in xframe.c, 5 ms) with comfortable margin.
 */
#define RX_RECOVERY_STEP_NS      (20 * 1000 * 1000)

/*
 * How far to step QEMU_CLOCK_VIRTUAL to cross the delay to the FIRST bring-up
 * link edge (XFRAME_LINK_UP_DELAY_MS in xframe.c, 5 ms) with comfortable
 * margin. The model presents the link as a DOWN->UP transition after the
 * adapter is enabled; this crosses to the link-DOWN edge, the first of the two.
 */
#define LINK_UP_STEP_NS          (20 * 1000 * 1000)

/*
 * How far to step past the link-DOWN edge to the link-UP edge that follows it
 * (XFRAME_LINK_DOWN_TO_UP_MS in xframe.c, 500 ms) with comfortable margin,
 * still short of the 2 s heartbeat. Stepped from the adapter-enable it also
 * carries the link all the way through the DOWN->UP transition and settles it
 * up -- what the tests that only care about a settled-up link step across.
 */
#define LINK_SETTLE_STEP_NS      (700LL * 1000 * 1000)

/*
 * How far to step QEMU_CLOCK_VIRTUAL to cross one period of the scheduled
 * interrupt (XFRAME_SCHED_INT_PERIOD_MS in xframe.c, 2000 ms) -- comfortably
 * past one period, short of a second one. Also used to step well past the point
 * a periodic link re-raise would have fired, to prove the bring-up LINK_UP is
 * delivered exactly once.
 */
#define SCHED_INT_STEP_NS        (3000LL * 1000 * 1000)

/*
 * A 1-buffer-mode RxD is four 64-bit words: host control, control1, control2,
 * buffer pointer. control1 carries ownership (bit 56, 1=card/0=host) and the
 * status code (XCODE, bits 51:48); control2 carries the buffer size in bits
 * 63:48 -- the posted capacity, overwritten by the received length.
 */
#define RXD_SIZE                 32
#define RXD_OWN                  (1ULL << 56)
#define RXD_XCODE_SHIFT          48
#define RXD_XCODE_MASK           0xf
#define RXD_XCODE_BSZ            7   /* buffer too small for the frame */
#define RXD_BUFSIZ_SHIFT         48

/*
 * Guest RAM for the receive ring and its buffers, page-aligned and clear of
 * the transmit scratch.
 */
#define RX_RING_BASE             (TX_RAM_SCRATCH + 0x10000)
#define RX_BUF0                  (TX_RAM_SCRATCH + 0x20000)
#define RX_BUF1                  (TX_RAM_SCRATCH + 0x21000)
#define RX_BUF_CAPACITY          128
#define RX_BUF_SMALL_CAPACITY    32   /* smaller than the test frames */

/*
 * The station address the -device mac= sets, in wire order -- what the receive
 * filter matches an incoming frame's destination against.
 */
static const uint8_t xframe_station_mac[6] = {
    0x52, 0x54, 0x00, 0x12, 0x34, 0x56
};

static void save_fn(QPCIDevice *dev, int devfn, void *data)
{
    QPCIDevice **pdev = (QPCIDevice **)data;

    *pdev = dev;
}

static QPCIDevice *find_xframe(QPCIBus *bus, uint16_t device_id)
{
    QPCIDevice *dev = NULL;

    qpci_device_foreach(bus, XFRAME_VENDOR, device_id, save_fn, &dev);
    return dev;
}

/*
 * A booted VM with the default (Xena) card, its BAR0 PIF window mapped and
 * decoding enabled, ready for register access.
 */
typedef struct {
    QOSState *qs;
    QPCIDevice *dev;
    QPCIBar pif;
} XframeTest;

static void xframe_start(XframeTest *t)
{
    uint64_t barsize;

    t->qs = qtest_pc_boot("%s", XFRAME_CARD);
    t->dev = find_xframe(t->qs->pcibus, XFRAME_DEVICE_XENA);
    g_assert(t->dev != NULL);

    t->pif = qpci_iomap(t->dev, 0, &barsize);
    g_assert_cmphex(barsize, ==, XFRAME_PIF_WINDOW_SIZE);

    qpci_device_enable(t->dev);
}

static void xframe_stop(XframeTest *t)
{
    g_free(t->dev);
    qtest_shutdown(t->qs);
}

static uint64_t pif_readq(XframeTest *t, uint64_t off)
{
    return qpci_io_readq(t->dev, t->pif, off);
}

static void pif_writeq(XframeTest *t, uint64_t off, uint64_t val)
{
    qpci_io_writeq(t->dev, t->pif, off, val);
}

/*
 * Observe the device's INTx line directly: QEMU reflects an asserted legacy
 * interrupt in the config-space status register's interrupt bit
 * (pci_update_irq_status), independently of the command register's interrupt
 * disable. This reads the actual delivered line, not just a status register
 * the device happens to latch.
 */
static bool xframe_intx_asserted(QPCIDevice *dev)
{
    return (qpci_config_readw(dev, PCI_STATUS) & PCI_STATUS_INTERRUPT) != 0;
}

/*
 * 32-bit register access: the width real drivers use for these 64-bit
 * registers (IRIX xg's byte-swapper handshake reads them 32-bit; Linux s2io on
 * Xena writes MAC_CFG as two 32-bit halves), and the width the Origin 200
 * Bridge delivers when a slot leaves DEV_64BIT_DEVICE clear. A register at
 * off+0 is its low 32 bits, off+4 its high 32 bits.
 */
static uint32_t pif_readl(XframeTest *t, uint64_t off)
{
    return qpci_io_readl(t->dev, t->pif, off);
}

static void pif_writel(XframeTest *t, uint64_t off, uint32_t val)
{
    qpci_io_writel(t->dev, t->pif, off, val);
}

/*
 * TxD/RxD control words are the Xframe's hardware-default big-endian format:
 * Linux's own comment is "the device by default set to a big endian format,
 * so a big endian driver need not set anything" (s2io_set_swapper), and
 * OpenBSD's xge_attach likewise skips the SWAPPER_CTRL write on a big-endian
 * host. The Origin 200's IRIX guest is always big-endian, so it never
 * programs the swapper away from that default. A 64-bit descriptor word is
 * therefore written and read back as explicit big-endian bytes here,
 * independent of the register-MMIO windows below, which stay little-endian
 * (DEVICE_LITTLE_ENDIAN).
 */
static void dma_stq_be(QTestState *qts, uint64_t addr, uint64_t val)
{
    uint8_t b[8];
    int i;

    for (i = 0; i < 8; i++) {
        b[7 - i] = val >> (8 * i);
    }
    qtest_memwrite(qts, addr, b, sizeof(b));
}

static uint64_t dma_ldq_be(QTestState *qts, uint64_t addr)
{
    uint8_t b[8];
    uint64_t val = 0;
    int i;

    qtest_memread(qts, addr, b, sizeof(b));
    for (i = 0; i < 8; i++) {
        val = (val << 8) | b[i];
    }
    return val;
}

/*
 * Little-endian counterparts of dma_stq_be/dma_ldq_be above, for the
 * SWAPPER_CTRL-programmed little-endian descriptor variant (a little-endian
 * guest -- Linux s2io / OpenBSD xge on an LE host -- sets a descriptor
 * type's _SE bit and writes its descriptors this way; see
 * test_descriptors_honor_swapper_ctrl_little_endian).
 */
static void dma_stq_le(QTestState *qts, uint64_t addr, uint64_t val)
{
    uint8_t b[8];
    int i;

    for (i = 0; i < 8; i++) {
        b[i] = val >> (8 * i);
    }
    qtest_memwrite(qts, addr, b, sizeof(b));
}

static uint64_t dma_ldq_le(QTestState *qts, uint64_t addr)
{
    uint8_t b[8];
    uint64_t val = 0;
    int i;

    qtest_memread(qts, addr, b, sizeof(b));
    for (i = 0; i < 8; i++) {
        val |= (uint64_t)b[i] << (8 * i);
    }
    return val;
}

/*
 * The socket backend frames each packet with a four-byte network-order length
 * ahead of it, in both directions.
 */
static void net_socket_set_recv_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

static void net_socket_send_frame(int fd, const uint8_t *buf, size_t len)
{
    uint32_t hdr = htonl((uint32_t)len);

    g_assert_cmpint(send(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    g_assert_cmpint(send(fd, buf, len, 0), ==, (int)len);
}

static size_t net_socket_recv_frame(int fd, uint8_t *buf, size_t buf_size)
{
    uint32_t hdr;
    size_t len;

    g_assert_cmpint(recv(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    len = ntohl(hdr);
    g_assert_cmpuint(len, <=, buf_size);
    g_assert_cmpint(recv(fd, buf, len, 0), ==, (int)len);
    return len;
}

static void net_socket_expect_no_frame(int fd)
{
    uint32_t hdr;

    net_socket_set_recv_timeout(fd, 1);
    errno = 0;
    g_assert_cmpint(recv(fd, &hdr, sizeof(hdr), 0), ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
}

/*
 * A booted VM with the card wired to a socket backend, both BARs mapped and
 * bus-mastering enabled. host_fd is the test's end of the pair: the device
 * emits transmitted frames onto it, and the test injects received frames by
 * writing the other direction -- the same length-prefixed framing, since the
 * socket backend uses it both ways.
 */
typedef struct {
    QOSState *qs;
    QPCIDevice *dev;
    QPCIBar pif;
    QPCIBar txp;
    int host_fd;
} XframeTx;

static void xframe_tx_start(XframeTx *t)
{
    uint64_t barsize;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 5);
    t->host_fd = sv[0];

    t->qs = qtest_pc_boot("-netdev socket,fd=%d,id=n0 "
                          "-device xframe,netdev=n0,mac=" XFRAME_TEST_MAC,
                          sv[1]);
    t->dev = find_xframe(t->qs->pcibus, XFRAME_DEVICE_XENA);
    g_assert(t->dev != NULL);

    t->pif = qpci_iomap(t->dev, 0, &barsize);
    g_assert_cmphex(barsize, ==, XFRAME_PIF_WINDOW_SIZE);

    t->txp = qpci_iomap(t->dev, 2, &barsize);
    g_assert_cmphex(barsize, ==, XFRAME_TXP_WINDOW_SIZE);

    qpci_device_enable(t->dev);
}

static uint64_t sock_pif_readq(XframeTx *t, uint64_t off)
{
    return qpci_io_readq(t->dev, t->pif, off);
}

static void sock_pif_writeq(XframeTx *t, uint64_t off, uint64_t val)
{
    qpci_io_writeq(t->dev, t->pif, off, val);
}

static void xframe_tx_stop(XframeTx *t)
{
    g_free(t->dev);
    qtest_shutdown(t->qs);
    close(t->host_fd);
}

/*
 * Program a single-segment TxD (first and last of a one-frame list) owned by
 * the card, its buffer holding the frame.
 */
static void xframe_program_txd(XframeTx *t, uint64_t par, uint64_t buf,
                               const uint8_t *frame, size_t len)
{
    qtest_memwrite(t->qs->qts, buf, frame, len);
    dma_stq_be(t->qs->qts, par + 0,
               (uint64_t)len | TXD_OWN | TXD_GCF | TXD_GCL);
    dma_stq_be(t->qs->qts, par + 8, 0);
    dma_stq_be(t->qs->qts, par + 16, buf);
    dma_stq_be(t->qs->qts, par + 24, 0);
}

/* Ring the doorbell for a one-descriptor list at par. */
static void xframe_ring_doorbell(XframeTx *t, uint64_t par)
{
    qpci_io_writeq(t->dev, t->txp, TXP_TXDL_PAR, par);
    qpci_io_writeq(t->dev, t->txp, TXP_TXDL_LCR,
                   TXDL_NUMTXD(0) | TXDL_LGC_FIRST | TXDL_LGC_LAST);
}

/*
 * Program a fresh, card-owned RxD in 1-buffer mode: host control cleared,
 * ownership to the card, the buffer's capacity in control2, and the buffer
 * pointer -- if_xgereg.h's struct rxd1 field order (host control, control1,
 * control2, buffer pointer).
 */
static void xframe_program_rxd(XframeTx *t, uint64_t desc, uint64_t buf,
                               uint64_t capacity)
{
    dma_stq_be(t->qs->qts, desc + 0, 0);
    dma_stq_be(t->qs->qts, desc + 8, RXD_OWN);
    dma_stq_be(t->qs->qts, desc + 16, capacity << RXD_BUFSIZ_SHIFT);
    dma_stq_be(t->qs->qts, desc + 24, buf);
}

/* Point receive ring 0 at ring_base and put it in service. */
static void xframe_rx_ring_arm(XframeTx *t, uint64_t ring_base)
{
    sock_pif_writeq(t, PRC_RXD0_0, ring_base);
    sock_pif_writeq(t, PRC_CTRL_0, RC_IN_SVC);
}

/*
 * Program one MAC-address-table entry (data0, data1) at offset via the
 * indirect command/strobe interface, write-enabled -- the same interface
 * the factory-MAC read-back test uses, just with RMAC_ADDR_CMD_WE set.
 */
static void xframe_program_mac_entry(XframeTx *t, unsigned offset,
                                     uint64_t data0, uint64_t data1)
{
    sock_pif_writeq(t, RMAC_ADDR_DATA0_MEM, data0);
    sock_pif_writeq(t, RMAC_ADDR_DATA1_MEM, data1);
    sock_pif_writeq(t, RMAC_ADDR_CMD_MEM,
                    RMAC_ADDR_CMD_WE | RMAC_ADDR_CMD_STROBE |
                    RMAC_ADDR_CMD_OFF(offset));
}

/*
 * Poll RX_TRAFFIC_INT for the ring-0 completion bit rather than sleeping a
 * fixed time: the socket backend's delivery is not synchronous with the
 * write that injects it, the same reason sgi-ioc3-eth-test.c's
 * wait_for_rx_consume polls instead of guessing a delay.
 */
static bool xframe_wait_rx_traffic(XframeTx *t, int attempts)
{
    int i;

    for (i = 0; i < attempts; i++) {
        if (sock_pif_readq(t, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0) {
            return true;
        }
        g_usleep(1000);
    }
    return false;
}

/*
 * Poll one RxD's ownership bit rather than RX_TRAFFIC_INT: the status bit
 * is sticky across every delivery until acknowledged, so once more than one
 * descriptor is in play it cannot tell which one landed. The descriptor's
 * own OWN bit can.
 */
static bool xframe_wait_rxd_consumed(XframeTx *t, uint64_t desc, int attempts)
{
    int i;

    for (i = 0; i < attempts; i++) {
        if (!(dma_ldq_be(t->qs->qts, desc + 8) & RXD_OWN)) {
            return true;
        }
        g_usleep(1000);
    }
    return false;
}

/*
 * Receive: post a card-owned RxD, inject a frame addressed to the station
 * MAC over the socket backend, and assert it lands in the guest buffer with
 * the documented status and that the receive interrupt is raised. A golden
 * byte-for-byte assertion on what the card deposited, not a round trip.
 */
static void test_receive_delivers_a_frame(void)
{
    XframeTx t;
    uint8_t frame[64];
    uint8_t landed[64];
    unsigned i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(0x80 + i);
    }
    memcpy(frame, xframe_station_mac, sizeof(xframe_station_mac));

    xframe_tx_start(&t);
    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF0, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);

    net_socket_send_frame(t.host_fd, frame, sizeof(frame));
    g_assert_true(xframe_wait_rx_traffic(&t, 5000));

    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) & RXD_OWN, ==, 0);
    g_assert_cmphex((dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) >>
                     RXD_XCODE_SHIFT) & RXD_XCODE_MASK, ==, 0);
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 16) >>
                    RXD_BUFSIZ_SHIFT, ==, sizeof(frame));

    qtest_memread(t.qs->qts, RX_BUF0, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(frame), frame, sizeof(frame));

    xframe_tx_stop(&t);
}

/*
 * Guest-driven hang class: a receive ring base far out of range must not
 * walk off into nothing, spin, or wedge the device -- no frame lands and the
 * device stays alive, the receive-side counterpart of the transmit slice's
 * out-of-range doorbell test. Reprogramming a valid ring afterwards and
 * seeing a frame land proves recovery, not a fluke.
 */
static void test_receive_out_of_range_ring_pointer_is_safe(void)
{
    XframeTx t;
    uint8_t frame[64];
    uint8_t landed[64];
    unsigned i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(0xa0 + i);
    }
    memcpy(frame, xframe_station_mac, sizeof(xframe_station_mac));

    xframe_tx_start(&t);

    xframe_rx_ring_arm(&t, 0xffffffff00000000ULL);
    net_socket_send_frame(t.host_fd, frame, sizeof(frame));
    g_assert_false(xframe_wait_rx_traffic(&t, 500));

    /* The device is still alive: reprogramming a valid ring recovers. */
    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF1, RX_BUF_CAPACITY);
    sock_pif_writeq(&t, PRC_RXD0_0, RX_RING_BASE);

    net_socket_send_frame(t.host_fd, frame, sizeof(frame));
    g_assert_true(xframe_wait_rx_traffic(&t, 5000));

    qtest_memread(t.qs->qts, RX_BUF1, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(frame), frame, sizeof(frame));

    xframe_tx_stop(&t);
}

/*
 * The load-bearing hang class: a frame the device's own filter drops (a
 * unicast destination that is not the station address, not promiscuous)
 * must still count as consumed, not report 0 -- a 0 return here wedges the
 * netdev queue, since nothing about a filtered frame changes on a retry. A
 * legitimate frame right after must still be delivered, proving the queue
 * never wedged.
 */
static void test_receive_filtered_frame_does_not_wedge(void)
{
    XframeTx t;
    uint8_t filtered[64];
    uint8_t legit[64];
    uint8_t landed[64];
    static const uint8_t not_station[6] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55
    };
    unsigned i;

    for (i = 0; i < sizeof(filtered); i++) {
        filtered[i] = (uint8_t)(0xc0 + i);
        legit[i] = (uint8_t)(0xd0 + i);
    }
    memcpy(filtered, not_station, sizeof(not_station));
    memcpy(legit, xframe_station_mac, sizeof(xframe_station_mac));

    xframe_tx_start(&t);
    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF0, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);

    net_socket_send_frame(t.host_fd, filtered, sizeof(filtered));
    /*
     * Give the filtered frame time to reach and be dropped by the device,
     * and confirm it did not spuriously land -- what carries the test is
     * what happens next, not this.
     */
    g_usleep(200000);
    g_assert_cmphex(sock_pif_readq(&t, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0,
                    ==, 0);

    net_socket_send_frame(t.host_fd, legit, sizeof(legit));
    g_assert_true(xframe_wait_rx_traffic(&t, 5000));

    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) & RXD_OWN, ==, 0);
    qtest_memread(t.qs->qts, RX_BUF0, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(legit), legit, sizeof(legit));

    xframe_tx_stop(&t);
}

/*
 * A buffer smaller than the frame: the deposit truncates to the posted
 * capacity and the descriptor reports XCODE=X_BSZ, rather than overrunning
 * the buffer or silently reporting success on a partial frame.
 */
static void test_receive_buffer_too_small_truncates(void)
{
    XframeTx t;
    uint8_t frame[64];
    uint8_t landed[RX_BUF_SMALL_CAPACITY];
    unsigned i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(0xe0 + i);
    }
    memcpy(frame, xframe_station_mac, sizeof(xframe_station_mac));

    xframe_tx_start(&t);
    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF0, RX_BUF_SMALL_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);

    net_socket_send_frame(t.host_fd, frame, sizeof(frame));
    g_assert_true(xframe_wait_rx_traffic(&t, 5000));

    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) & RXD_OWN, ==, 0);
    g_assert_cmphex((dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) >>
                     RXD_XCODE_SHIFT) & RXD_XCODE_MASK, ==, RXD_XCODE_BSZ);
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 16) >>
                    RXD_BUFSIZ_SHIFT, ==, RX_BUF_SMALL_CAPACITY);

    qtest_memread(t.qs->qts, RX_BUF0, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(landed));

    xframe_tx_stop(&t);
}

/*
 * Recovery after ring exhaustion, through the real path: a recovery poll,
 * not the driver's interrupt-ACK write. Both public drivers acknowledge the
 * receive interrupt (read RX_TRAFFIC_INT, write the pending bits back)
 * BEFORE refilling any descriptor -- OpenBSD's xge_intr acks and only then
 * walks the ring calling xge_add_rxbuf per descriptor; Linux's s2io_isr is
 * the same order -- and a live ring is never refilled by rewriting
 * PRC_RXD0_0/PRC_CTRL_0 either: both drivers flip a consumed RxD's OWN bit
 * directly in guest RAM instead (OpenBSD xge_add_rxbuf, Linux
 * fill_rx_buffers), no register write at all. So nothing the guest ever
 * writes to a register lands after the refill for a register-write-triggered
 * flush to hook onto; only a poll can notice it. This test reproduces the
 * real order -- ack, then refill, then let the recovery poll's next tick
 * run -- rather than the reverse, which would pass for the wrong reason.
 *
 * Two descriptors are posted and consumed to run the ring dry; a third frame
 * then finds no owned descriptor and is left queued by QEMU's netdev core,
 * which disables further delivery until something flushes it. The queued
 * frame must land once the recovery poll's timer fires after the refill:
 * this is the mechanism this whole slice adds, and without it (or with the
 * ack write wrongly modelled as what recovers it) this assertion is exactly
 * what fails.
 */
static void test_receive_recovers_after_exhaustion_via_recovery_poll(void)
{
    XframeTx t;
    uint8_t frame1[64], frame2[64], frame3[64];
    uint8_t landed[64];
    uint64_t slot0 = RX_RING_BASE + 0 * RXD_SIZE;
    uint64_t slot1 = RX_RING_BASE + 1 * RXD_SIZE;
    uint64_t slot2 = RX_RING_BASE + 2 * RXD_SIZE;
    uint64_t pending;
    unsigned i;

    for (i = 0; i < sizeof(frame1); i++) {
        frame1[i] = (uint8_t)(0x10 + i);
        frame2[i] = (uint8_t)(0x30 + i);
        frame3[i] = (uint8_t)(0x50 + i);
    }
    memcpy(frame1, xframe_station_mac, sizeof(xframe_station_mac));
    memcpy(frame2, xframe_station_mac, sizeof(xframe_station_mac));
    memcpy(frame3, xframe_station_mac, sizeof(xframe_station_mac));

    xframe_tx_start(&t);

    /* A two-descriptor ring: just enough capacity to run dry on frame 3. */
    xframe_program_rxd(&t, slot0, RX_BUF0, RX_BUF_CAPACITY);
    xframe_program_rxd(&t, slot1, RX_BUF1, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);

    net_socket_send_frame(t.host_fd, frame1, sizeof(frame1));
    g_assert_true(xframe_wait_rxd_consumed(&t, slot0, 5000));

    net_socket_send_frame(t.host_fd, frame2, sizeof(frame2));
    g_assert_true(xframe_wait_rxd_consumed(&t, slot1, 5000));

    /*
     * The ring is dry: slot 2 was never posted, so this frame finds no
     * owned descriptor and is left in QEMU's netdev queue rather than
     * delivered. There is no register or memory state that distinguishes
     * "queued, waiting for recovery" from "never reached the device at
     * all" -- an untouched descriptor's OWN bit reads exactly like a
     * completed one's, 0, so asserting that here would not test anything.
     * The sleep only gives the socket backend real wall-clock time to
     * actually read the frame off the wire and hit the no-buffer path; the
     * real proof this frame was genuinely stuck behind exhaustion is that
     * this test fails at the final assertion without the fix in this
     * commit.
     */
    net_socket_send_frame(t.host_fd, frame3, sizeof(frame3));
    g_usleep(200000);

    /*
     * The driver's own interrupt-ACK idiom -- read the pending bits, write
     * them back -- performed FIRST, matching the real order both drivers
     * use. It does not itself recover anything here: the ring is still
     * empty at this point, exactly as it is for the real drivers.
     */
    pending = sock_pif_readq(&t, RX_TRAFFIC_INT);
    g_assert_cmphex(pending & RX_TRAFFIC_RING0, ==, RX_TRAFFIC_RING0);
    sock_pif_writeq(&t, RX_TRAFFIC_INT, pending);

    /*
     * Refill in place, after the ack: guest RAM only, exactly what a
     * driver's own refill touches (OpenBSD xge_add_rxbuf, Linux
     * fill_rx_buffers), never PRC_RXD0_0/PRC_CTRL_0.
     */
    xframe_program_rxd(&t, slot2, RX_BUF0, RX_BUF_CAPACITY);

    /*
     * Nothing register-visible follows the refill for either driver, so
     * only the recovery poll's own timer can notice it. QEMU_CLOCK_VIRTUAL
     * is frozen under qtest until stepped explicitly, so advance it past
     * the timer's period rather than waiting on wall time, which would
     * never make it fire at all.
     */
    qtest_clock_step(t.qs->qts, RX_RECOVERY_STEP_NS);

    g_assert_true(xframe_wait_rxd_consumed(&t, slot2, 5000));
    qtest_memread(t.qs->qts, RX_BUF0, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(frame3), frame3, sizeof(frame3));

    xframe_tx_stop(&t);
}

/*
 * A joined multicast group is a masked CAM entry with an exact-match mask
 * (every bit compared) -- see MCAST_EXACT_MASK's comment for the derivation.
 * A frame to a different group finds no matching entry and is filtered (the
 * descriptor stays available, not consumed); the joined group's own frame is
 * delivered into that same descriptor right after, proving the filter -- not
 * a coincidence of timing -- is what told them apart.
 */
static void test_receive_multicast_exact_match(void)
{
    XframeTx t;
    uint8_t wrong_group[64];
    uint8_t right_group[64];
    uint8_t landed[64];
    static const uint8_t group_a[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x01 };
    static const uint8_t group_b[6] = { 0x01, 0x00, 0x5e, 0x00, 0x00, 0x02 };
    unsigned i;

    for (i = 0; i < sizeof(wrong_group); i++) {
        wrong_group[i] = (uint8_t)(0x60 + i);
        right_group[i] = (uint8_t)(0x70 + i);
    }
    memcpy(wrong_group, group_b, sizeof(group_b));
    memcpy(right_group, group_a, sizeof(group_a));

    xframe_tx_start(&t);
    xframe_program_mac_entry(&t, 4, MCAST_GROUP_A_DATA0, MCAST_EXACT_MASK);
    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF0, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);

    /*
     * The unjoined group is filtered, not queued -- unlike the no-buffer
     * case, a filtered frame leaves no ambiguity: the descriptor's OWN bit
     * is a real, immediate, device-visible signal that nothing landed.
     */
    net_socket_send_frame(t.host_fd, wrong_group, sizeof(wrong_group));
    g_usleep(200000);
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) & RXD_OWN, ==,
                    RXD_OWN);

    net_socket_send_frame(t.host_fd, right_group, sizeof(right_group));
    g_assert_true(xframe_wait_rxd_consumed(&t, RX_RING_BASE, 5000));
    qtest_memread(t.qs->qts, RX_BUF0, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(right_group), right_group,
                    sizeof(right_group));

    xframe_tx_stop(&t);
}

/*
 * The IFF_ALLMULTI sentinel (Linux s2io_set_multicast's own address/mask
 * pair -- MCAST_ALLMULTI_DATA0/MASK's comment has the derivation) matches an
 * arbitrary multicast destination that was never individually programmed,
 * proving the wildcard mask genuinely tests only the group bit rather than
 * happening to match one specific address.
 */
static void test_receive_multicast_allmulti_wildcard(void)
{
    XframeTx t;
    uint8_t frame[64];
    uint8_t landed[64];
    static const uint8_t arbitrary_group[6] = {
        0x33, 0x33, 0x00, 0x00, 0x00, 0x01
    };
    unsigned i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(0x90 + i);
    }
    memcpy(frame, arbitrary_group, sizeof(arbitrary_group));

    xframe_tx_start(&t);
    xframe_program_mac_entry(&t, 4, MCAST_ALLMULTI_DATA0, MCAST_ALLMULTI_MASK);
    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF0, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);

    net_socket_send_frame(t.host_fd, frame, sizeof(frame));
    g_assert_true(xframe_wait_rxd_consumed(&t, RX_RING_BASE, 5000));
    qtest_memread(t.qs->qts, RX_BUF0, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(frame), frame, sizeof(frame));

    xframe_tx_stop(&t);
}

/*
 * Unicast regression guard for the masked-match rework: the station address
 * (entry 0, an exact-match mask by construction -- reset zeroes
 * mac_addr_mask[]) is still delivered, and a random unicast address, matching
 * no entry, is still filtered rather than accidentally swept in by the new
 * masked-compare loop.
 */
static void test_receive_unicast_still_matches(void)
{
    XframeTx t;
    uint8_t station_frame[64];
    uint8_t random_frame[64];
    uint8_t landed[64];
    static const uint8_t random_unicast[6] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x66
    };
    unsigned i;

    for (i = 0; i < sizeof(station_frame); i++) {
        station_frame[i] = (uint8_t)(0xa0 + i);
        random_frame[i] = (uint8_t)(0xb0 + i);
    }
    memcpy(station_frame, xframe_station_mac, sizeof(xframe_station_mac));
    memcpy(random_frame, random_unicast, sizeof(random_unicast));

    xframe_tx_start(&t);
    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF0, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);

    net_socket_send_frame(t.host_fd, random_frame, sizeof(random_frame));
    g_usleep(200000);
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) & RXD_OWN, ==,
                    RXD_OWN);

    net_socket_send_frame(t.host_fd, station_frame, sizeof(station_frame));
    g_assert_true(xframe_wait_rxd_consumed(&t, RX_RING_BASE, 5000));
    qtest_memread(t.qs->qts, RX_BUF0, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(station_frame), station_frame,
                    sizeof(station_frame));

    xframe_tx_stop(&t);
}

/*
 * The generic device, no SGI-board properties set, must enumerate on its own
 * -- the payoff of naming the model after the silicon.
 */
static void test_identity(void)
{
    QOSState *qs = qtest_pc_boot("%s", XFRAME_CARD);
    QPCIDevice *dev = find_xframe(qs->pcibus, XFRAME_DEVICE_XENA);

    g_assert(dev != NULL);
    g_assert_cmphex(qpci_config_readw(dev, PCI_VENDOR_ID), ==, XFRAME_VENDOR);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==,
                    XFRAME_DEVICE_XENA);

    g_free(dev);
    qtest_shutdown(qs);
}

/*
 * The device-id property selects the other ASIC generation (Herc), not a
 * separate device type.
 */
static void test_device_id_selects_herc(void)
{
    QOSState *qs = qtest_pc_boot("-device xframe,device-id=0x5832");
    QPCIDevice *dev = find_xframe(qs->pcibus, XFRAME_DEVICE_HERC);

    g_assert(dev != NULL);
    g_assert_cmphex(qpci_config_readw(dev, PCI_DEVICE_ID), ==,
                    XFRAME_DEVICE_HERC);

    g_free(dev);
    qtest_shutdown(qs);
}

/*
 * The generic device carries no subsystem branding of its own: realize() must
 * positively overwrite QEMU's generic subsystem default rather than leave it
 * to leak through.
 */
static void test_subsystem_is_unbranded_by_default(void)
{
    QOSState *qs = qtest_pc_boot("%s", XFRAME_CARD);
    QPCIDevice *dev = find_xframe(qs->pcibus, XFRAME_DEVICE_XENA);

    g_assert(dev != NULL);
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_VENDOR_ID), ==, 0);
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_ID), ==, 0);

    g_free(dev);
    qtest_shutdown(qs);
}

/*
 * The SGI instantiation sets subsystem-vendor-id 0x10a9. Its subsystem device
 * ID is OPEN in the datasheet and must not be invented, so the other half of
 * the same dword must still read the unbranded default -- proving nothing was
 * invented to fill it.
 */
static void test_subsystem_vendor_when_sgi_board(void)
{
    QOSState *qs = qtest_pc_boot("-device xframe,subsystem-vendor-id=0x10a9");
    QPCIDevice *dev = find_xframe(qs->pcibus, XFRAME_DEVICE_XENA);

    g_assert(dev != NULL);
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_VENDOR_ID), ==,
                    XFRAME_SGI_SUBSYSTEM_VENDOR);
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_ID), ==, 0);

    g_free(dev);
    qtest_shutdown(qs);
}

/* BAR0: a 64-bit memory BAR (config 0x10/0x14), sized to cover the PIF map. */
static void test_bar0_is_a_64bit_pif_window(void)
{
    QOSState *qs = qtest_pc_boot("%s", XFRAME_CARD);
    QPCIDevice *dev = find_xframe(qs->pcibus, XFRAME_DEVICE_XENA);
    uint32_t bar0;
    uint64_t barsize;

    g_assert(dev != NULL);

    /*
     * Memory space (bit 0 clear), 64-bit type (bits 2:1 = 0b10), not
     * prefetchable (bit 3 clear): the low nibble is 0b0100. Read before
     * mapping, while the address bits are still zero.
     */
    bar0 = qpci_config_readl(dev, PCI_BASE_ADDRESS_0);
    g_assert_cmphex(bar0 & 0xf, ==, 0x4);

    qpci_iomap(dev, 0, &barsize);
    g_assert_cmphex(barsize, ==, XFRAME_PIF_WINDOW_SIZE);

    g_free(dev);
    qtest_shutdown(qs);
}

/*
 * The second BAR (config 0x18 -- BAR index 2, since index 1 is BAR0's own
 * upper half): a 32-bit memory window for the TxP transmit doorbell.
 */
static void test_bar2_is_the_txp_window(void)
{
    QOSState *qs = qtest_pc_boot("%s", XFRAME_CARD);
    QPCIDevice *dev = find_xframe(qs->pcibus, XFRAME_DEVICE_XENA);
    uint32_t bar2;
    uint64_t barsize;

    g_assert(dev != NULL);

    /* Memory space, 32-bit type, not prefetchable: every flag bit clear. */
    bar2 = qpci_config_readl(dev, PCI_BASE_ADDRESS_2);
    g_assert_cmphex(bar2 & 0xf, ==, 0x0);

    qpci_iomap(dev, 2, &barsize);
    g_assert_cmphex(barsize, ==, XFRAME_TXP_WINDOW_SIZE);

    g_free(dev);
    qtest_shutdown(qs);
}

/*
 * The byte-swapper diagnostic: a 64-bit read returns exactly the fixed
 * constant. This is a golden assertion -- it pins the value, so it catches a
 * self-consistent-but-wrong byte order that a write/read round trip would
 * miss. It is the make-or-break the driver's endian check reads back.
 */
static void test_swapper_diagnostic_returns_magic(void)
{
    XframeTest t;

    xframe_start(&t);
    g_assert_cmphex(pif_readq(&t, PIF_RD_SWAPPER_FB), ==, SWAPPER_MAGIC);
    xframe_stop(&t);
}

/* XMSI_ADDRESS is a 64-bit scratch: a value written reads back unchanged. */
static void test_xmsi_address_roundtrips(void)
{
    XframeTest t;

    xframe_start(&t);
    pif_writeq(&t, XMSI_ADDRESS, SWAPPER_MAGIC);
    g_assert_cmphex(pif_readq(&t, XMSI_ADDRESS), ==, SWAPPER_MAGIC);

    pif_writeq(&t, XMSI_ADDRESS, 0xdeadbeefcafef00dULL);
    g_assert_cmphex(pif_readq(&t, XMSI_ADDRESS), ==, 0xdeadbeefcafef00dULL);
    xframe_stop(&t);
}

/*
 * ADAPTER_STATUS presents its quiescent/ready bits immediately -- the
 * attach-gating check is a single non-retrying read with no polling budget.
 */
static void test_adapter_status_is_quiescent(void)
{
    XframeTest t;

    xframe_start(&t);
    g_assert_cmphex(pif_readq(&t, ADAPTER_STATUS), ==,
                    ADAPTER_STATUS_QUIESCENT);
    xframe_stop(&t);
}

/* ADAPTER_EN in ADAPTER_CONTROL is settable and reads back. */
static void test_adapter_en_is_settable(void)
{
    XframeTest t;

    xframe_start(&t);
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    g_assert_cmphex(pif_readq(&t, ADAPTER_CONTROL) & ADAPTER_EN, ==,
                    ADAPTER_EN);
    xframe_stop(&t);
}

/*
 * The link-state-change interrupt: the one source an idle interface actually
 * sees. Both public drivers unmask it at bring-up through the MAC block masks
 * (OpenBSD xge_init: MAC_INT_MASK = MAC_TMAC_INT leaving only RMAC unmasked,
 * MAC_RMAC_ERR_MASK = ~RMAC_LINK_STATE_CHANGE_INT; Linux en_dis_err_alarms adds
 * it to the RX-MAC unmask set) and service it in their ISR, and it needs no
 * frame to arrive: it fires when the link comes up after the driver enables the
 * adapter. The model raises it a short virtual-time delay after ADAPTER_EN goes
 * 0->1 -- latched in MAC_RMAC_ERR_REG, gated by the two MAC block masks,
 * aggregated into GENERAL_INT_STATUS's receive-MAC bit, and write-1-to-cleared
 * -- observed here on the INTx line itself. Interrupts here are enabled the way
 * the drivers do it: GENERAL_INT_MASK left at 0 (the OpenBSD value) and the
 * block masks selecting the source. Without this source an idle interface
 * raises nothing, which is what makes the IRIX xg watchdog reset the device in
 * a loop.
 */
static void test_link_up_raises_interrupt(void)
{
    XframeTest t;

    xframe_start(&t);

    /*
     * Enable the link source the way OpenBSD xge_init does: general mask 0 (its
     * value), and the RMAC source unmasked at both MAC block masks.
     */
    pif_writeq(&t, GENERAL_INT_MASK, 0);
    pif_writeq(&t, MAC_INT_MASK, MAC_TMAC_INT);
    pif_writeq(&t, MAC_RMAC_ERR_MASK, ~RMAC_LINK_STATE_CHANGE_INT);

    /* Idle, adapter not yet enabled: nothing to raise the line. */
    g_assert_false(xframe_intx_asserted(t.dev));

    /*
     * Enabling the adapter starts the link negotiating; it comes up
     * asynchronously. QEMU_CLOCK_VIRTUAL is frozen under qtest until stepped,
     * so nothing has fired in the instant right after the write.
     */
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_REG) &
                    RMAC_LINK_STATE_CHANGE_INT, ==, 0);

    /* Step past the link-up delay: the link comes up, the interrupt fires. */
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);
    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_REG) &
                    RMAC_LINK_STATE_CHANGE_INT, ==,
                    RMAC_LINK_STATE_CHANGE_INT);
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_RXMAC, ==,
                    GEN_INTR_RXMAC);

    /* The MAC block mask gates the line; unmasking the link bit restores it. */
    pif_writeq(&t, MAC_RMAC_ERR_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(t.dev));
    pif_writeq(&t, MAC_RMAC_ERR_MASK, ~RMAC_LINK_STATE_CHANGE_INT);
    g_assert_true(xframe_intx_asserted(t.dev));

    /* The ISR ack -- write the bit back -- clears it and drops the line. */
    pif_writeq(&t, MAC_RMAC_ERR_REG, RMAC_LINK_STATE_CHANGE_INT);
    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_REG), ==, 0);
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_RXMAC, ==, 0);

    xframe_stop(&t);
}

/*
 * The SW_RESET key (0xa5a5a5...) returns the register file to reset: state
 * written before the key is gone afterwards.
 */
static void test_sw_reset_key_clears_state(void)
{
    XframeTest t;

    xframe_start(&t);
    pif_writeq(&t, XMSI_ADDRESS, SWAPPER_MAGIC);
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    g_assert_cmphex(pif_readq(&t, XMSI_ADDRESS), ==, SWAPPER_MAGIC);

    pif_writeq(&t, SW_RESET, SW_RESET_KEY);

    g_assert_cmphex(pif_readq(&t, XMSI_ADDRESS), ==, 0);
    g_assert_cmphex(pif_readq(&t, ADAPTER_CONTROL), ==, 0);
    xframe_stop(&t);
}

/*
 * MAC_CFG is write-locked: a write without first writing RMAC_CFG_KEY = 0x4c0d
 * does not take; with the key, it does.
 */
static void test_mac_config_write_lock(void)
{
    XframeTest t;

    xframe_start(&t);

    /* Locked: the write is dropped. */
    pif_writeq(&t, MAC_CFG, MAC_CFG_PAYLOAD);
    g_assert_cmphex(pif_readq(&t, MAC_CFG), ==, 0);

    /* Unlocked by the key: the write takes. */
    pif_writeq(&t, RMAC_CFG_KEY, RMAC_CFG_KEY_VALUE);
    pif_writeq(&t, MAC_CFG, MAC_CFG_PAYLOAD);
    g_assert_cmphex(pif_readq(&t, MAC_CFG), ==, MAC_CFG_PAYLOAD);

    xframe_stop(&t);
}

/*
 * The factory MAC read-back: the indirect read of MAC-address-table entry 0
 * (strobe, no write-enable) returns the configured netdev MAC in
 * RMAC_ADDR_DATA0_MEM bits 63:16, and the command strobe self-clears.
 */
static void test_factory_mac_readback(void)
{
    XframeTest t;

    xframe_start(&t);

    pif_writeq(&t, RMAC_ADDR_CMD_MEM,
               RMAC_ADDR_CMD_STROBE | RMAC_ADDR_CMD_OFF(0));
    g_assert_cmphex(pif_readq(&t, RMAC_ADDR_CMD_MEM) & RMAC_ADDR_CMD_STROBE,
                    ==, 0);
    g_assert_cmphex(pif_readq(&t, RMAC_ADDR_DATA0_MEM), ==,
                    XFRAME_TEST_MAC_DATA0);

    xframe_stop(&t);
}

/*
 * 32-bit register access. The registers are 64-bit, but real drivers read and
 * write them with 32-bit cycles (IRIX xg's swapper handshake reads
 * PIF_RD_SWAPPER_FB 32-bit; Linux s2io on Xena writes MAC_CFG as two 32-bit
 * halves), and behind the Origin 200 Bridge a slot with DEV_64BIT_DEVICE clear
 * gets every 64-bit access split into two 32-bit device cycles. So the device
 * must serve a register's low half at off+0 and its high half at off+4, and a
 * write to one half must not disturb the other. These are the subtests that
 * failed before the window's valid.min_access_size dropped to 4.
 */

/*
 * The byte-swapper diagnostic read the way IRIX xg reads it: 32-bit, one half
 * at a time. Each half must be the corresponding half of the magic constant --
 * this exact read is what xg's endianness handshake checks, and a 0 here (a
 * rejected sub-64-bit access) is what aborted attach before the fix.
 */
static void test_swapper_diagnostic_32bit_halves(void)
{
    XframeTest t;

    xframe_start(&t);
    g_assert_cmphex(pif_readl(&t, PIF_RD_SWAPPER_FB + 0), ==,
                    (uint32_t)SWAPPER_MAGIC);
    g_assert_cmphex(pif_readl(&t, PIF_RD_SWAPPER_FB + 4), ==,
                    (uint32_t)(SWAPPER_MAGIC >> 32));
    xframe_stop(&t);
}

/*
 * ADAPTER_STATUS read 32-bit: its two halves differ (0x00000000 low,
 * 0xf400ffc3 high), so a half read that returned the wrong 32 bits -- or the
 * whole 64-bit value truncated -- shows up here.
 */
static void test_adapter_status_32bit_halves(void)
{
    XframeTest t;

    xframe_start(&t);
    g_assert_cmphex(pif_readl(&t, ADAPTER_STATUS + 0), ==,
                    (uint32_t)ADAPTER_STATUS_QUIESCENT);
    g_assert_cmphex(pif_readl(&t, ADAPTER_STATUS + 4), ==,
                    (uint32_t)(ADAPTER_STATUS_QUIESCENT >> 32));
    xframe_stop(&t);
}

/*
 * The XMSI_ADDRESS scratch via 32-bit halves. Reading back each half of a
 * 64-bit-written value pins the read split; writing each half separately and
 * reading the whole back pins the write split -- and that a low-half write
 * leaves the high half alone, which a naive "widen to 64-bit" write would
 * clobber to zero.
 */
static void test_xmsi_address_32bit_halves(void)
{
    XframeTest t;

    xframe_start(&t);
    pif_writeq(&t, XMSI_ADDRESS, 0xdeadbeefcafef00dULL);
    g_assert_cmphex(pif_readl(&t, XMSI_ADDRESS + 0), ==, 0xcafef00d);
    g_assert_cmphex(pif_readl(&t, XMSI_ADDRESS + 4), ==, 0xdeadbeef);

    pif_writel(&t, XMSI_ADDRESS + 0, 0x11223344);
    pif_writel(&t, XMSI_ADDRESS + 4, 0x55667788);
    g_assert_cmphex(pif_readq(&t, XMSI_ADDRESS), ==, 0x5566778811223344ULL);
    xframe_stop(&t);
}

/*
 * SW_RESET's key sits in bits 63:40 -- the register's high 32 bits -- so a
 * 32-bit write to the low half must not reset, and the 32-bit write that
 * carries the key to the high half must reset exactly once. This is the
 * no-double-fire contract for the trigger the memory core would otherwise let
 * a split write mishandle.
 */
static void test_sw_reset_key_via_32bit(void)
{
    XframeTest t;

    xframe_start(&t);
    pif_writeq(&t, XMSI_ADDRESS, SWAPPER_MAGIC);
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);

    /* Low half: no key here, must not reset. */
    pif_writel(&t, SW_RESET + 0, 0x00000001);
    g_assert_cmphex(pif_readq(&t, XMSI_ADDRESS), ==, SWAPPER_MAGIC);
    g_assert_cmphex(pif_readq(&t, ADAPTER_CONTROL) & ADAPTER_EN, ==,
                    ADAPTER_EN);

    /* High half carrying the key (0xa5a5a5 in bits 63:40): reset fires once. */
    pif_writel(&t, SW_RESET + 4, (uint32_t)(SW_RESET_KEY >> 32));
    g_assert_cmphex(pif_readq(&t, XMSI_ADDRESS), ==, 0);
    g_assert_cmphex(pif_readq(&t, ADAPTER_CONTROL), ==, 0);
    xframe_stop(&t);
}

/*
 * MAC_CFG's write lock unlocked and driven entirely by 32-bit writes, the
 * Linux s2io Xena path: RMAC_CFG_KEY's key is in bits 63:48 (the high half),
 * re-written before each MAC_CFG half. A 32-bit write of the key to the wrong
 * (low) half must not unlock, and a MAC_CFG payload with both halves non-zero
 * proves each half latches.
 */
static void test_rmac_cfg_key_unlock_via_32bit(void)
{
    XframeTest t;

    xframe_start(&t);

    /* Locked: the 32-bit-halved write is dropped. */
    pif_writel(&t, MAC_CFG + 0, 0x0000ffff);
    pif_writel(&t, MAC_CFG + 4, 0xc0000000);
    g_assert_cmphex(pif_readq(&t, MAC_CFG), ==, 0);

    /* The key in the wrong half does not unlock. */
    pif_writel(&t, RMAC_CFG_KEY + 0, 0x00004c0d);
    pif_writel(&t, MAC_CFG + 4, 0xc0000000);
    g_assert_cmphex(pif_readq(&t, MAC_CFG), ==, 0);

    /* Key to the high half unlocks; re-keyed before each half, both take. */
    pif_writel(&t, RMAC_CFG_KEY + 4, (uint32_t)(RMAC_CFG_KEY_VALUE >> 32));
    pif_writel(&t, MAC_CFG + 0, 0x0000ffff);
    pif_writel(&t, RMAC_CFG_KEY + 4, (uint32_t)(RMAC_CFG_KEY_VALUE >> 32));
    pif_writel(&t, MAC_CFG + 4, 0xc0000000);
    g_assert_cmphex(pif_readq(&t, MAC_CFG), ==, 0xc00000000000ffffULL);
    xframe_stop(&t);
}

/*
 * The MAC-address-table strobe via 32-bit. The command (write-enable, strobe
 * bit 48, offset) lives in the register's high 32 bits, so a 32-bit write to
 * the low half must not strobe, and the high-half write fires the read strobe
 * once -- loading the factory MAC into DATA0, which reads back correctly both
 * 64-bit and 32-bit-halved.
 */
static void test_mac_table_strobe_via_32bit(void)
{
    XframeTest t;

    xframe_start(&t);

    /* A low-half write carries no strobe (bit 48 is in the high half). */
    pif_writeq(&t, RMAC_ADDR_DATA0_MEM, 0);
    pif_writel(&t, RMAC_ADDR_CMD_MEM + 0, 0x00000000);
    g_assert_cmphex(pif_readq(&t, RMAC_ADDR_DATA0_MEM), ==, 0);

    /* The high half carries the read strobe for entry 0: it fires once. */
    pif_writel(&t, RMAC_ADDR_CMD_MEM + 4,
               (uint32_t)(RMAC_ADDR_CMD_STROBE >> 32));
    g_assert_cmphex(pif_readq(&t, RMAC_ADDR_CMD_MEM) & RMAC_ADDR_CMD_STROBE,
                    ==, 0);
    g_assert_cmphex(pif_readq(&t, RMAC_ADDR_DATA0_MEM), ==,
                    XFRAME_TEST_MAC_DATA0);
    g_assert_cmphex(pif_readl(&t, RMAC_ADDR_DATA0_MEM + 0), ==,
                    (uint32_t)XFRAME_TEST_MAC_DATA0);
    g_assert_cmphex(pif_readl(&t, RMAC_ADDR_DATA0_MEM + 4), ==,
                    (uint32_t)(XFRAME_TEST_MAC_DATA0 >> 32));
    xframe_stop(&t);
}

/*
 * Transmit: program a single-descriptor TxD list in guest memory, ring the
 * doorbell, and assert the device consumes the descriptor (ownership bit
 * cleared) and emits the exact frame onto the backend. A golden byte-for-byte
 * assertion on what left the card, not a register round trip.
 */
static void test_transmit_emits_a_frame(void)
{
    XframeTx t;
    uint8_t frame[64];
    uint8_t got[128];
    uint64_t par = TX_RAM_SCRATCH;
    uint64_t buf = TX_RAM_SCRATCH + 0x1000;
    size_t len;
    unsigned i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 1);
    }
    memset(frame, 0xff, 6);  /* broadcast destination */

    xframe_tx_start(&t);
    xframe_program_txd(&t, par, buf, frame, sizeof(frame));
    xframe_ring_doorbell(&t, par);

    len = net_socket_recv_frame(t.host_fd, got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));

    /* The card released the descriptor: ownership handed back to the host. */
    g_assert_cmphex(dma_ldq_be(t.qs->qts, par) & TXD_OWN, ==, 0);

    xframe_tx_stop(&t);
}

/*
 * Guest-driven hang class: a doorbell whose list pointer is far out of range
 * must not walk off into nothing or wedge the device. It emits no frame and
 * the device keeps working -- a valid transmit right after still goes out.
 */
static void test_transmit_out_of_range_pointer_is_safe(void)
{
    XframeTx t;
    uint8_t frame[64];
    uint8_t got[128];
    uint64_t good_par = TX_RAM_SCRATCH;
    uint64_t buf = TX_RAM_SCRATCH + 0x1000;
    size_t len;
    unsigned i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(0x40 + i);
    }
    memset(frame, 0xff, 6);

    xframe_tx_start(&t);

    /* Doorbell at a wildly out-of-range list pointer: no frame, no wedge. */
    qpci_io_writeq(t.dev, t.txp, TXP_TXDL_PAR, 0xffffffff00000000ULL);
    qpci_io_writeq(t.dev, t.txp, TXP_TXDL_LCR,
                   TXDL_NUMTXD(0) | TXDL_LGC_FIRST | TXDL_LGC_LAST);
    net_socket_expect_no_frame(t.host_fd);

    /* The device is still alive: a well-formed list transmits normally. */
    net_socket_set_recv_timeout(t.host_fd, 5);
    xframe_program_txd(&t, good_par, buf, frame, sizeof(frame));
    xframe_ring_doorbell(&t, good_par);

    len = net_socket_recv_frame(t.host_fd, got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));

    xframe_tx_stop(&t);
}

/*
 * Transmit completion raises an interrupt: a rung doorbell that emits a frame
 * sets TX_TRAFFIC_INT for the FIFO, aggregated into GENERAL_INT_STATUS's
 * transmit-traffic bit, gated by GENERAL_INT_MASK, and write-1-to-cleared --
 * the transmit mirror of the receive interrupt, observed on the INTx line.
 * Both drivers reclaim sent descriptors on this interrupt (OpenBSD xge_intr,
 * Linux s2io_isr), so without it a driver that waits on transmit completion
 * stalls after the first burst.
 */
static void test_transmit_completion_raises_interrupt(void)
{
    XframeTx t;
    uint8_t frame[64];
    uint8_t got[128];
    uint64_t par = TX_RAM_SCRATCH;
    uint64_t buf = TX_RAM_SCRATCH + 0x1000;
    uint64_t pending;
    size_t len;
    unsigned i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 1);
    }
    memset(frame, 0xff, 6);  /* broadcast destination */

    xframe_tx_start(&t);

    /*
     * Enable transmit completion the way the drivers do: general mask 0 (their
     * value) and the transmit-traffic block mask unmasked.
     */
    sock_pif_writeq(&t, GENERAL_INT_MASK, 0);
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, 0);
    g_assert_false(xframe_intx_asserted(t.dev));

    /* Ring a one-descriptor list: the frame egresses and completion fires. */
    xframe_program_txd(&t, par, buf, frame, sizeof(frame));
    xframe_ring_doorbell(&t, par);

    len = net_socket_recv_frame(t.host_fd, got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));

    g_assert_true(xframe_intx_asserted(t.dev));
    pending = sock_pif_readq(&t, TX_TRAFFIC_INT);
    g_assert_cmphex(pending & TX_TRAFFIC_FIFO0, ==, TX_TRAFFIC_FIFO0);
    g_assert_cmphex(sock_pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXTRAFFIC,
                    ==, GEN_INTR_TXTRAFFIC);

    /* The transmit-traffic block mask gates the line; unmasking restores it. */
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(t.dev));
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, 0);
    g_assert_true(xframe_intx_asserted(t.dev));

    /* The ISR ack -- write the pending bits back -- clears it, drops INTx. */
    sock_pif_writeq(&t, TX_TRAFFIC_INT, pending);
    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);
    g_assert_cmphex(sock_pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXTRAFFIC,
                    ==, 0);

    xframe_tx_stop(&t);
}

/*
 * SWAPPER_CTRL-conditional descriptor byte order: a little-endian guest
 * (Linux s2io / OpenBSD xge on an LE host) sets the TxD/RxD _SE bits and
 * writes little-endian descriptors, and the device must honor that -- not
 * decode big-endian unconditionally, which is all the rest of this suite's
 * descriptor tests exercise (SWAPPER_CTRL left at its reset 0). Exercises
 * both directions in one test, the same XframeTx socket-backed harness
 * test_receive_delivers_a_frame already shares with the transmit tests: a
 * doorbell with a little-endian TxD still emits the frame and releases
 * ownership little-endian, and a posted little-endian RxD still receives a
 * frame and writes its little-endian completion status back.
 *
 * RED/GREEN: against the pre-fix unconditional-big-endian decode, this test
 * fails both ways -- transmit sees TXD_OWN misread clear
 * ("descriptor the card does not own; ignored", no frame egresses) and
 * receive sees the RxD's OWN bit misread clear too (can_receive reports no
 * buffer, the frame is never delivered). It passes once the decode honors
 * SWAPPER_CTRL's per-path SE bit.
 */
static void test_descriptors_honor_swapper_ctrl_little_endian(void)
{
    XframeTx t;
    uint8_t tx_frame[64];
    uint8_t rx_frame[64];
    uint8_t got[128];
    uint8_t landed[64];
    uint64_t par = TX_RAM_SCRATCH;
    uint64_t buf = TX_RAM_SCRATCH + 0x1000;
    size_t len;
    unsigned i;

    for (i = 0; i < sizeof(tx_frame); i++) {
        tx_frame[i] = (uint8_t)(i + 1);
    }
    memset(tx_frame, 0xff, 6);  /* broadcast destination */

    for (i = 0; i < sizeof(rx_frame); i++) {
        rx_frame[i] = (uint8_t)(0xc0 + i);
    }
    memcpy(rx_frame, xframe_station_mac, sizeof(xframe_station_mac));

    xframe_tx_start(&t);

    /* Program the little-endian swap for both descriptor types. */
    sock_pif_writeq(&t, SWAPPER_CTRL,
                    SWAPPER_CTRL_TXD_R_SE | SWAPPER_CTRL_RXD_R_SE);

    /* Transmit: a little-endian TxD must still be consumed and sent. */
    qtest_memwrite(t.qs->qts, buf, tx_frame, sizeof(tx_frame));
    dma_stq_le(t.qs->qts, par + 0,
              (uint64_t)sizeof(tx_frame) | TXD_OWN | TXD_GCF | TXD_GCL);
    dma_stq_le(t.qs->qts, par + 8, 0);
    dma_stq_le(t.qs->qts, par + 16, buf);
    dma_stq_le(t.qs->qts, par + 24, 0);
    xframe_ring_doorbell(&t, par);

    len = net_socket_recv_frame(t.host_fd, got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(tx_frame));
    g_assert_cmpmem(got, len, tx_frame, sizeof(tx_frame));
    /* The card released the descriptor: ownership handed back, still LE. */
    g_assert_cmphex(dma_ldq_le(t.qs->qts, par) & TXD_OWN, ==, 0);

    /* Receive: a little-endian RxD must still be recognised and filled. */
    dma_stq_le(t.qs->qts, RX_RING_BASE + 0, 0);
    dma_stq_le(t.qs->qts, RX_RING_BASE + 8, RXD_OWN);
    dma_stq_le(t.qs->qts, RX_RING_BASE + 16,
              (uint64_t)RX_BUF_CAPACITY << RXD_BUFSIZ_SHIFT);
    dma_stq_le(t.qs->qts, RX_RING_BASE + 24, RX_BUF0);
    xframe_rx_ring_arm(&t, RX_RING_BASE);

    net_socket_send_frame(t.host_fd, rx_frame, sizeof(rx_frame));
    g_assert_true(xframe_wait_rx_traffic(&t, 5000));

    g_assert_cmphex(dma_ldq_le(t.qs->qts, RX_RING_BASE + 8) & RXD_OWN, ==, 0);
    g_assert_cmphex(dma_ldq_le(t.qs->qts, RX_RING_BASE + 16) >>
                    RXD_BUFSIZ_SHIFT, ==, sizeof(rx_frame));

    qtest_memread(t.qs->qts, RX_BUF0, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(rx_frame), rx_frame, sizeof(rx_frame));

    xframe_tx_stop(&t);
}

/*
 * The MAC-block link interrupt with GENERAL_INT_MASK left at its all-ones reset
 * -- the OpenBSD xge_init enable path (MAC_INT_MASK = MAC_TMAC_INT to unmask
 * the RMAC block bit, MAC_RMAC_ERR_MASK = ~RMAC_LINK_STATE_CHANGE_INT to unmask
 * the leaf link bit), which never touches GENERAL_INT_MASK. INTx must assert
 * with GENERAL_INT_MASK all-ones, because the block masks gate the line and the
 * general mask gates nothing. This pins the gating: the general mask is not on
 * the line's path, and the summary (checked below) is the raw aggregate rather
 * than a general-mask-blanked view. (IRIX xg takes its link interrupt through
 * the PIC/GPIO block, not this MAC one -- see the PIC-path test below.)
 */
static void test_xg_enable_path_link_via_block_masks(void)
{
    XframeTest t;

    xframe_start(&t);

    /* xg leaves the top-level mask exactly as reset: fully masked. */
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_MASK), ==, DISABLE_ALL_INTRS_64);

    /* Unmask the link source at the block level only, as the drivers do. */
    pif_writeq(&t, MAC_INT_MASK, MAC_TMAC_INT);
    pif_writeq(&t, MAC_RMAC_ERR_MASK, ~RMAC_LINK_STATE_CHANGE_INT);

    /* Idle, adapter not yet enabled: nothing to raise the line. */
    g_assert_false(xframe_intx_asserted(t.dev));

    /* Enable the adapter; the link comes up a short virtual-time later. */
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    g_assert_false(xframe_intx_asserted(t.dev));
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);

    /*
     * INTx asserts although GENERAL_INT_MASK is all-ones: the block masks gate
     * the line. This is the whole xg fix -- it fails before the gating change.
     */
    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_REG) &
                    RMAC_LINK_STATE_CHANGE_INT, ==, RMAC_LINK_STATE_CHANGE_INT);

    /*
     * The summary shows the source even though GENERAL_INT_MASK is all-ones:
     * GENERAL_INT_STATUS is the raw block-pending aggregate, not gated by the
     * general mask. The live xg diagnostic read it 379,815x and needs it
     * non-zero to dispatch; blanking it by the all-ones general mask is the
     * refuted "summary-visibility" model. Here the MAC link source rolls up as
     * GEN_INTR_RXMAC.
     */
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_RXMAC, ==,
                    GEN_INTR_RXMAC);

    /* The leaf block mask gates the line: masking the link bit drops INTx. */
    pif_writeq(&t, MAC_RMAC_ERR_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(t.dev));
    pif_writeq(&t, MAC_RMAC_ERR_MASK, ~RMAC_LINK_STATE_CHANGE_INT);
    g_assert_true(xframe_intx_asserted(t.dev));

    /* The RMAC roll-up mask gates it too: masking RMAC at MAC_INT_MASK. */
    pif_writeq(&t, MAC_INT_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(t.dev));
    pif_writeq(&t, MAC_INT_MASK, MAC_TMAC_INT);
    g_assert_true(xframe_intx_asserted(t.dev));

    /* The ISR ack -- write the bit back -- clears it and drops the line. */
    pif_writeq(&t, MAC_RMAC_ERR_REG, RMAC_LINK_STATE_CHANGE_INT);
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_stop(&t);
}

/*
 * The link-state interrupt through the PIC/GPIO block -- the source IRIX xg's
 * bring-up waits on, distinct from the MAC RMAC path above. The model raises
 * the GPIO LINK_UP event on adapter-enable and rolls it up through
 * PIC_INT_STATUS's PIC_INT_GPIO into GENERAL_INT_STATUS's GEN_INTR_TXPIC (Linux
 * s2io_isr dispatches GEN_INTR_TXPIC to s2io_txpic_intr_handle, which reads
 * pic_int_status -> PIC_INT_GPIO -> gpio_int_reg -> LINK_UP). This variant
 * opens the PIC block AND the GPIO leaf, and asserts the source is gated by
 * PIC_INT_MASK's PIC_INT_GPIO bit and NOT by the GPIO leaf mask: masking the
 * leaf leaves INTx asserted. (xg's exact path, which leaves the leaf masked, is
 * the separate PIC-summary test below.)
 */
static void test_xg_enable_path_link_via_gpio_block(void)
{
    XframeTest t;
    uint64_t mac_latched;

    xframe_start(&t);

    /* xg leaves the top-level and MAC/traffic masks at their masked reset. */
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_MASK), ==, DISABLE_ALL_INTRS_64);
    g_assert_cmphex(pif_readq(&t, MAC_INT_MASK), ==, DISABLE_ALL_INTRS_64);
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_MASK), ==, DISABLE_ALL_INTRS_64);

    /*
     * Open the PIC/GPIO path: the PIC block wholesale and the GPIO link-up
     * leaf. The MAC masks stay masked -- so if the line ever asserts, it is the
     * GPIO source doing it, not the MAC one.
     */
    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, GPIO_INT_MASK, ~GPIO_INT_MASK_LINK_UP);

    /* Idle, adapter not yet enabled: nothing to raise the line. */
    g_assert_false(xframe_intx_asserted(t.dev));

    /* Enable the adapter; the link comes up a short virtual-time later. */
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    g_assert_false(xframe_intx_asserted(t.dev));
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);

    /*
     * INTx asserts although GENERAL_INT_MASK and the MAC masks are all-ones:
     * the PIC block mask gates the line.
     */
    g_assert_true(xframe_intx_asserted(t.dev));

    /*
     * The ISR's descent path reads, both non-destructive derived summaries:
     * PIC_INT_STATUS shows the GPIO sub-block, and GENERAL_INT_STATUS shows the
     * source with GENERAL_INT_MASK all-ones (the raw block-pending aggregate,
     * so xg -- which never touches the general mask -- sees GEN_INTR_TXPIC and
     * dispatches). Checked before the acking read of GPIO_INT_REG below, which
     * consumes the edge.
     */
    g_assert_cmphex(pif_readq(&t, PIC_INT_STATUS) & PIC_INT_GPIO, ==,
                    PIC_INT_GPIO);
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==,
                    GEN_INTR_TXPIC);

    /*
     * The GPIO leaf mask does NOT gate the line: masking it leaves INTx
     * asserted. Delivery is gated by PIC_INT_MASK's PIC_INT_GPIO bit alone --
     * the measured xg behaviour (xg leaves the leaf masked yet takes the
     * interrupt).
     */
    pif_writeq(&t, GPIO_INT_MASK, DISABLE_ALL_INTRS_64);
    g_assert_true(xframe_intx_asserted(t.dev));

    /* The PIC block mask gates it: masking the whole PIC block drops it. */
    pif_writeq(&t, PIC_INT_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(t.dev));
    pif_writeq(&t, PIC_INT_MASK, 0);
    g_assert_true(xframe_intx_asserted(t.dev));

    /*
     * The ISR ack IRIX xg actually performs -- READ GPIO_INT_REG, no
     * write-1-to-clear, no mask (the round-6 trace). The read returns the
     * latched LINK_UP bit and consumes the self-clearing edge, dropping INTx.
     * The MAC path latched its own link bit off the same link-up event, but the
     * MAC masks keep it off the line -- and its read-back is non-destructive --
     * so it stays latched: proof the two link sources are independent and this
     * one came through the PIC/GPIO block.
     */
    g_assert_cmphex(pif_readq(&t, GPIO_INT_REG) & GPIO_INT_REG_LINK_UP, ==,
                    GPIO_INT_REG_LINK_UP);
    g_assert_false(xframe_intx_asserted(t.dev));
    mac_latched = pif_readq(&t, MAC_RMAC_ERR_REG) & RMAC_LINK_STATE_CHANGE_INT;
    g_assert_cmphex(mac_latched, ==, RMAC_LINK_STATE_CHANGE_INT);

    xframe_stop(&t);
}

/*
 * IRIX xg's EXACT measured bring-up path, end to end -- the reproduction of
 * the live-diagnostic wall. From the trace, xg opens ONLY PIC_INT_MASK (0x808)
 * and TXPIC_INT_MASK (0x818) and leaves GENERAL_INT_MASK, the GPIO leaf
 * (GPIO_INT_MASK 0x898), every MAC mask and both traffic masks at their
 * all-ones reset. On link-up, INTx must assert (the GPIO link source is gated
 * by the PIC block mask alone, not the GPIO leaf xg never opens); its aggregate
 * bit must be visible in GENERAL_INT_STATUS despite the all-ones general mask
 * (the raw summary xg's ISR read 379,815x and got 0 from before the fix); and
 * xg's read of GPIO_INT_REG must ack the self-clearing link-up edge and drop
 * the line so it fires once per event rather than storming (the round-6 trace:
 * xg reads GPIO_INT_REG and issues no write-1-to-clear and no mask). Fails
 * before the block-mask/raw-summary change on two counts -- the leaf-gated
 * source never asserts with the leaf masked, and the summary reads 0 -- and
 * before the read-ack change on the post-read de-assert; passes after.
 */
static void test_xg_bringup_pic_summary_serviced(void)
{
    XframeTest t;

    xframe_start(&t);

    /*
     * Everything except the two PIC masks stays at its masked reset -- the
     * exact mask state the live xg trace leaves.
     */
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_MASK), ==, DISABLE_ALL_INTRS_64);
    g_assert_cmphex(pif_readq(&t, GPIO_INT_MASK), ==, DISABLE_ALL_INTRS_64);
    g_assert_cmphex(pif_readq(&t, MAC_INT_MASK), ==, DISABLE_ALL_INTRS_64);
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_MASK), ==, DISABLE_ALL_INTRS_64);

    /* xg opens exactly these two, and nothing else. */
    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, TXPIC_INT_MASK, 0);

    /* Idle, adapter not yet enabled: nothing to raise the line. */
    g_assert_false(xframe_intx_asserted(t.dev));

    /* Enable the adapter; the link comes up a short virtual-time later. */
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    g_assert_false(xframe_intx_asserted(t.dev));
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);

    /*
     * INTx asserts with the GPIO leaf still masked -- the whole point: the
     * source reaches xg through the PIC block it opens, not the leaf it leaves
     * masked.
     */
    g_assert_true(xframe_intx_asserted(t.dev));

    /*
     * The summary xg's ISR actually reads is non-zero and shows GEN_INTR_TXPIC,
     * although GENERAL_INT_MASK is all-ones -- and PIC_INT_STATUS below it
     * shows the GPIO sub-block. Both are non-destructive derived reads, checked
     * before the acking read of GPIO_INT_REG. This raw summary is the fix that
     * lets xg identify the source at all -- before it, this read returned 0 and
     * xg stormed without ever dispatching.
     */
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS), !=, 0);
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==,
                    GEN_INTR_TXPIC);
    g_assert_cmphex(pif_readq(&t, PIC_INT_STATUS) & PIC_INT_GPIO, ==,
                    PIC_INT_GPIO);

    /* The PIC block mask gates the line: masking it drops INTx. */
    pif_writeq(&t, PIC_INT_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(t.dev));
    pif_writeq(&t, PIC_INT_MASK, 0);
    g_assert_true(xframe_intx_asserted(t.dev));

    /*
     * The ISR ack xg actually performs, from the round-6 trace: it READS
     * GPIO_INT_REG (sees LINK_UP) and issues no write-1-to-clear and no mask.
     * That read consumes the self-clearing link-up edge, de-asserting INTx and
     * clearing the summary bit, so the source fires once per event instead of
     * storming.
     */
    g_assert_cmphex(pif_readq(&t, GPIO_INT_REG) & GPIO_INT_REG_LINK_UP, ==,
                    GPIO_INT_REG_LINK_UP);
    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==, 0);

    xframe_stop(&t);
}

/*
 * IRIX xg's measured ISR ack of the link-up interrupt, from the round-6 live
 * boot. On the interrupt firing, xg reads GENERAL_INT_STATUS (sees
 * GEN_INTR_TXPIC), reads TXPIC_INT_REG (0x810 -- the PCI-X transmit-error
 * sub-source, which is empty, so it walks on), reads GPIO_INT_REG (sees
 * LINK_UP) -- and then does NOT write GPIO_INT_REG back (no write-1-to-clear)
 * and does NOT mask GPIO_INT_MASK or PIC_INT_MASK. The only thing that
 * acknowledges the link-up edge is that read of GPIO_INT_REG. So the source is
 * a self-clearing edge consumed by the ISR's read: INTx must de-assert after
 * xg reads GPIO_INT_REG and stay down -- one serviced interrupt per link-up,
 * not a storm. Before this change the GPIO status stayed latched and INTx
 * stayed asserted after the read (xg re-entered ~183,869x in ~12s and never
 * transmitted); it fails here on the post-read de-assert and passes after.
 */
static void test_xg_link_up_gpio_read_acks(void)
{
    XframeTest t;

    xframe_start(&t);

    /* xg's exact mask state: open the two PIC leaves it opens, nothing else. */
    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, TXPIC_INT_MASK, 0);

    /* Enable the adapter; the link comes up a short virtual-time later. */
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);
    g_assert_true(xframe_intx_asserted(t.dev));

    /* xg's ISR dispatch read: the raw summary shows the PIC block source. */
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==,
                    GEN_INTR_TXPIC);

    /*
     * xg reads the txpic transmit-error sub-source (0x810) while walking the
     * PIC block. It is empty -- no PCI-X transmit error is modelled -- so this
     * read is non-destructive and leaves the line asserted: the source is the
     * GPIO link, not txpic.
     */
    g_assert_cmphex(pif_readq(&t, TXPIC_INT_REG), ==, 0);
    g_assert_true(xframe_intx_asserted(t.dev));

    /*
     * xg's actual ack: it reads GPIO_INT_REG, sees LINK_UP, and does nothing
     * else -- no write-1-to-clear, no mask. That read consumes the
     * self-clearing link-up edge.
     */
    g_assert_cmphex(pif_readq(&t, GPIO_INT_REG) & GPIO_INT_REG_LINK_UP, ==,
                    GPIO_INT_REG_LINK_UP);

    /* INTx de-asserts after the acking read: the storm the wall was. */
    g_assert_false(xframe_intx_asserted(t.dev));

    /*
     * And it stays down: the source is gone from the summary and the latch, so
     * a re-dispatch finds nothing and xg does not re-enter. A genuinely new
     * link event would latch afresh -- this only consumes the delivered edge.
     */
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==, 0);
    g_assert_cmphex(pif_readq(&t, GPIO_INT_REG) & GPIO_INT_REG_LINK_UP, ==, 0);

    xframe_stop(&t);
}

/*
 * A single link-up event is not enough: a live IRIX xg boot shows its driver
 * watchdog resetting the adapter roughly once every ten-plus seconds after the
 * one link-up interrupt ("xframe_watchdog: ERROR: No interrupts occuring, reset
 * device..."), each reset re-knocking the link down. The watchdog is a pure
 * liveness timer -- it does not care what interrupt arrives, only that one
 * arrives inside its interval. The driver feeds it not by re-raising the link,
 * but by programming a periodic hardware SCHEDULED interrupt at bring-up: it
 * writes an enable+period into scheduled_int_ctrl (0x918) and unmasks that
 * source through the PIC block (PIC_INT_MASK, PIC_INT_TX) and the TXPIC leaf
 * (TXPIC_INT_MASK). That timer raises a traffic- and link-independent interrupt
 * on the PIC/TXPIC path that the ISR services and self-clears.
 *
 * Writing the enable+period arms the timer; stepping past one period asserts
 * INTx, visible in GENERAL_INT_STATUS as GEN_INTR_TXPIC and in PIC_INT_STATUS
 * as PIC_INT_TX, and readable in the TXPIC block status (TXPIC_INT_REG) -- with
 * NO GPIO link bit set and no MAC link latch touched, so the watchdog feed
 * never disturbs carrier. It is gated by both PIC_INT_MASK and TXPIC_INT_MASK,
 * self-clears on the ISR's read of TXPIC_INT_REG, and recurs each period -- one
 * serviced interrupt per period, not a storm. Fails before the timer exists
 * (INTx never asserts after arming); passes after.
 */
static void test_scheduled_interrupt_feeds_watchdog(void)
{
    XframeTest t;
    uint64_t txpic;

    xframe_start(&t);

    /* xg's exact mask state: open the two PIC leaves it opens, nothing else. */
    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, TXPIC_INT_MASK, 0);

    /*
     * Arm the scheduled interrupt the way xg does -- write enable + a period.
     * The adapter is deliberately NOT enabled, so no bring-up link edge is ever
     * presented: any interrupt seen here is the scheduled timer's alone, and any
     * GPIO link bit set would be a carrier-path contamination this must not have.
     */
    g_assert_false(xframe_intx_asserted(t.dev));
    pif_writeq(&t, SCHED_INT_CTRL, SCHED_INT_CTRL_TIMER_EN | SCHED_INT_TEST_PERIOD);
    /* The virtual clock is frozen under qtest: nothing has fired yet. */
    g_assert_false(xframe_intx_asserted(t.dev));

    /* Step past one period: the scheduled timer fires and asserts INTx. */
    qtest_clock_step(t.qs->qts, SCHED_INT_STEP_NS);
    g_assert_true(xframe_intx_asserted(t.dev));

    /*
     * The raw summary shows the PIC block (GEN_INTR_TXPIC) and PIC_INT_STATUS
     * the PIC_INT_TX sub-block -- both non-destructive derived reads. Crucially,
     * NO GPIO link bit is set and the MAC link latch is untouched: the watchdog
     * feed is entirely off the link/carrier path.
     */
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==,
                    GEN_INTR_TXPIC);
    g_assert_cmphex(pif_readq(&t, PIC_INT_STATUS) & PIC_INT_TX, ==, PIC_INT_TX);
    g_assert_cmphex(pif_readq(&t, GPIO_INT_REG) &
                    (GPIO_INT_REG_LINK_UP | GPIO_INT_REG_LINK_DOWN), ==, 0);
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_REG) &
                    RMAC_LINK_STATE_CHANGE_INT, ==, 0);

    /* The PIC block mask gates the line: masking it drops INTx. */
    pif_writeq(&t, PIC_INT_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(t.dev));
    pif_writeq(&t, PIC_INT_MASK, 0);
    g_assert_true(xframe_intx_asserted(t.dev));

    /* The TXPIC leaf mask gates it too: masking it drops INTx. */
    pif_writeq(&t, TXPIC_INT_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(t.dev));
    pif_writeq(&t, TXPIC_INT_MASK, 0);
    g_assert_true(xframe_intx_asserted(t.dev));

    /*
     * The ISR's ack: it reads TXPIC_INT_REG while walking the PIC block, sees
     * the scheduled-interrupt latch (non-zero), and that read consumes the
     * self-clearing source -- dropping INTx and clearing the summary bit.
     */
    txpic = pif_readq(&t, TXPIC_INT_REG);
    g_assert_cmphex(txpic, !=, 0);
    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==, 0);

    /* It recurs: another period brings a fresh interrupt, serviced the same. */
    qtest_clock_step(t.qs->qts, SCHED_INT_STEP_NS);
    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, TXPIC_INT_REG), !=, 0);
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_stop(&t);
}

/*
 * IRIX xg's ISR does not take the read-based ack above: it dispatches off
 * GENERAL_INT_STATUS's GEN_INTR_TXPIC and, instead of reading TXPIC_INT_REG,
 * WRITES it as its acknowledgement -- it never issues the read that would
 * self-clear the scheduled-interrupt latch. Before the write-path ack in
 * xframe_pif_write_reg's XFRAME_TXPIC_INT_REG case, that left the latch
 * permanently set: GENERAL_INT_STATUS kept reporting GEN_INTR_TXPIC and the
 * ISR re-entered without end, hanging the driver's bring-up. Fails before
 * that case exists (the write leaves the latch and INTx set); passes after.
 *
 * The latch bit (XFRAME_TXPIC_INT_SCHED, bit 59) sits in the high 32 bits, so
 * only a write reaching that half may consume it -- a low-half-only write to
 * this register must leave the source untouched.
 *
 * Pending state is probed through PIC_INT_STATUS's PIC_INT_TX, a derived,
 * side-effect-free read (see xframe_pif_read_reg), rather than by reading
 * TXPIC_INT_REG itself: a read of that leaf covering the high half is the
 * OTHER, already-working ack path (xframe_pif_read), and would consume the
 * very latch this test means to leave pending for the write-ack under test.
 * TXPIC_INT_REG is read directly only to confirm it settles at 0 once the
 * write-ack has actually fired.
 */
static void test_xg_scheduled_interrupt_acks_via_write(void)
{
    XframeTest t;

    xframe_start(&t);

    /* xg's exact mask state: open the two PIC leaves it opens, nothing else. */
    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, TXPIC_INT_MASK, 0);

    /* Arm the scheduled interrupt and step past one period, as xg does. */
    pif_writeq(&t, SCHED_INT_CTRL,
               SCHED_INT_CTRL_TIMER_EN | SCHED_INT_TEST_PERIOD);
    qtest_clock_step(t.qs->qts, SCHED_INT_STEP_NS);
    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==,
                    GEN_INTR_TXPIC);
    g_assert_cmphex(pif_readq(&t, PIC_INT_STATUS) & PIC_INT_TX, ==, PIC_INT_TX);

    /* A low-half-only write does not reach the latch bit: no ack. */
    pif_writel(&t, TXPIC_INT_REG, 0);
    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, PIC_INT_STATUS) & PIC_INT_TX, ==, PIC_INT_TX);

    /*
     * xg's actual ack: a WRITE covering the high half -- never the read the
     * other ack path expects. This alone must consume the latch:
     * GENERAL_INT_STATUS drops GEN_INTR_TXPIC, the leaf reads back clean, and
     * INTx deasserts.
     */
    pif_writeq(&t, TXPIC_INT_REG, 0);
    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==, 0);
    g_assert_cmphex(pif_readq(&t, TXPIC_INT_REG), ==, 0);

    /*
     * One serviced interrupt per period, not a storm: the timer is still
     * armed, so the next period raises again and the write-ack (this time as
     * the 0x814 high-half 32-bit write) services it the same way.
     */
    qtest_clock_step(t.qs->qts, SCHED_INT_STEP_NS);
    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, PIC_INT_STATUS) & PIC_INT_TX, ==, PIC_INT_TX);
    pif_writel(&t, TXPIC_INT_REG + 4, 0);
    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, TXPIC_INT_REG), ==, 0);

    xframe_stop(&t);
}

/*
 * The bring-up LINK_UP is delivered exactly ONCE. The watchdog is fed by the
 * separate scheduled interrupt above, NOT by re-raising the link, so once the
 * bring-up DOWN->UP transition has been serviced the GPIO link source must stay
 * quiescent: a re-raised LINK_UP with no intervening LINK_DOWN would be a
 * spurious carrier event to a driver whose link belief is already up. After
 * servicing the bring-up UP, stepping the clock well past the point any
 * periodic link re-raise would fire must leave INTx deasserted and no link bit
 * latched. Fails while a link heartbeat re-raises LINK_UP (INTx re-asserts);
 * passes once the heartbeat is gone.
 */
static void test_link_up_delivered_once(void)
{
    XframeTest t;

    xframe_start(&t);

    /* xg's exact mask state: open the two PIC leaves it opens, nothing else. */
    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, TXPIC_INT_MASK, 0);

    /* Bring the link up and service the bring-up edge exactly as xg does. */
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);
    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, GPIO_INT_REG) & GPIO_INT_REG_LINK_UP, ==,
                    GPIO_INT_REG_LINK_UP);
    g_assert_false(xframe_intx_asserted(t.dev));

    /*
     * Step well past where a periodic link re-raise would have fired: the GPIO
     * link source stays quiescent -- LINK_UP was delivered once, and nothing
     * re-raises it. (No scheduled interrupt was armed here, so nothing on the
     * PIC/TXPIC path fires either.)
     */
    qtest_clock_step(t.qs->qts, SCHED_INT_STEP_NS);
    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, GPIO_INT_REG) &
                    (GPIO_INT_REG_LINK_UP | GPIO_INT_REG_LINK_DOWN), ==, 0);

    xframe_stop(&t);
}

/*
 * The bring-up link edge IRIX xg gates transmit on: a genuine link DOWN->UP
 * transition, delivered as TWO separate, separately-serviced GPIO interrupts,
 * not a bare unchanging UP. The Neterion family propagates carrier and wakes
 * the transmit queue only on a link-state CHANGE (Linux s2io_link's
 * "if (link != last_link_state)" guard), so a model that only ever asserts UP
 * presents no transition to a driver whose belief is already "up" and leaves
 * transmit gated shut. On ADAPTER_EN 0->1 the model presents a link-DOWN first,
 * then the link-UP one interval later -- and the two must never arrive as both
 * GPIO link bits set at once, which s2io treats as an unstable state it clears
 * and re-waits on. Each edge is a self-clearing GPIO interrupt serviced by the
 * ISR's read of GPIO_INT_REG, exactly as the single bring-up event was. The UP
 * is then delivered ONCE and not re-raised -- the watchdog is fed by the
 * separate scheduled interrupt, not by the link. Fails before this change --
 * the model never asserts LINK_DOWN, so the first serviced edge is UP, not
 * DOWN -- and passes after.
 */
static void test_link_down_up_transition_at_bringup(void)
{
    XframeTest t;
    uint64_t gpio;

    xframe_start(&t);

    /* xg's exact mask state: open the two PIC leaves it opens, nothing else. */
    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, TXPIC_INT_MASK, 0);

    /* Idle, adapter not yet enabled: nothing to raise the line. */
    g_assert_false(xframe_intx_asserted(t.dev));

    /*
     * Enable the adapter. The first link edge -- a link-DOWN -- lands a short
     * virtual-time later, after the driver's synchronous bring-up has unmasked
     * (the same delay the one-shot used), so it is delivered, not acked away.
     */
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    g_assert_false(xframe_intx_asserted(t.dev));
    qtest_clock_step(t.qs->qts, LINK_UP_STEP_NS);

    /*
     * First edge: link-DOWN. It reaches INTx through the same PIC/GPIO roll-up
     * the UP does -- the raw summary shows GEN_INTR_TXPIC and PIC_INT_STATUS
     * the GPIO sub-block (both non-destructive derived reads, checked before
     * the acking read of GPIO_INT_REG below).
     */
    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==,
                    GEN_INTR_TXPIC);
    g_assert_cmphex(pif_readq(&t, PIC_INT_STATUS) & PIC_INT_GPIO, ==,
                    PIC_INT_GPIO);

    /*
     * The ISR's acking read returns the latched DOWN bit and NOT the UP bit --
     * a single clean edge, never both together -- and consumes it, dropping
     * INTx: one link-DOWN delivered and serviced.
     */
    gpio = pif_readq(&t, GPIO_INT_REG);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_DOWN, ==, GPIO_INT_REG_LINK_DOWN);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_UP, ==, 0);
    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==, 0);

    /*
     * Second edge, one interval later: link-UP. A separate interrupt with the
     * UP bit set and the DOWN bit clear -- the completed DOWN->UP transition,
     * delivered and serviced independently of the DOWN above.
     */
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);
    g_assert_true(xframe_intx_asserted(t.dev));
    gpio = pif_readq(&t, GPIO_INT_REG);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_UP, ==, GPIO_INT_REG_LINK_UP);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_DOWN, ==, 0);
    g_assert_false(xframe_intx_asserted(t.dev));

    /*
     * The UP is delivered once: stepping well past where a periodic link
     * re-raise would have fired leaves the GPIO source quiescent -- INTx down
     * and no link bit latched. Nothing re-asserts LINK_UP; the watchdog is fed
     * by the scheduled interrupt, exercised separately.
     */
    qtest_clock_step(t.qs->qts, SCHED_INT_STEP_NS);
    g_assert_false(xframe_intx_asserted(t.dev));
    gpio = pif_readq(&t, GPIO_INT_REG);
    g_assert_cmphex(gpio & (GPIO_INT_REG_LINK_UP | GPIO_INT_REG_LINK_DOWN),
                    ==, 0);

    xframe_stop(&t);
}

/*
 * Carrier stays steady across the watchdog: a redundant ADAPTER_CONTROL write
 * that sets ADAPTER_EN while it is already set -- the idempotent re-poke IRIX
 * xg's link handler does on every serviced pass (a read-modify-OR-write that
 * never clears ADAPTER_EN first) -- must NOT re-arm the bring-up link sequence.
 * The link DOWN->UP edge arms only on a genuine ADAPTER_EN 0->1 transition, so
 * a re-poke while it is already 1 presents no fresh edge: no new GPIO link
 * event latches and INTx stays down. Were the arm keyed on the written level
 * rather than the 0->1 transition, this re-poke would re-run the DOWN->UP
 * sequence and flap carrier once per serviced pass.
 */
static void test_xg_redundant_adapter_en_does_not_relink(void)
{
    XframeTest t;

    xframe_start(&t);

    /* xg's exact mask state: open the two PIC leaves it opens, nothing else. */
    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, TXPIC_INT_MASK, 0);

    /* Bring the link up and service the bring-up edge exactly as xg does. */
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);
    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, GPIO_INT_REG) & GPIO_INT_REG_LINK_UP, ==,
                    GPIO_INT_REG_LINK_UP);
    g_assert_false(xframe_intx_asserted(t.dev));

    /*
     * The re-poke: ADAPTER_EN set again while already set. It must be a no-op
     * for the link sequence -- no re-armed edge, no re-delivered LINK_UP.
     */
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);

    /*
     * Step well past both bring-up link delays: nothing re-arms, so no GPIO
     * link event is presented and INTx stays down.
     */
    qtest_clock_step(t.qs->qts, SCHED_INT_STEP_NS);
    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, GPIO_INT_REG) &
                    (GPIO_INT_REG_LINK_UP | GPIO_INT_REG_LINK_DOWN), ==, 0);

    xframe_stop(&t);
}

/*
 * The MAC-block link-state-change latch (MAC_RMAC_ERR_REG bit 32) self-clears
 * on the driver's READ when that source is masked -- IRIX xg's case. xg takes
 * its link interrupt through the PIC/GPIO block and leaves the MAC link path
 * masked, yet its alarm handler still reads MAC_RMAC_ERR_REG each pass to test
 * the link-state-change bit. The bring-up link-up event latches that bit on
 * both paths; xg never write-1-to-clears it (it services the link through
 * GPIO), so a plain W1C level would read set on every pass and xg would treat
 * the settled link as changing again and again -- flapping carrier. So a read
 * of the masked source consumes the latch: the alarm handler sees the one real
 * change once, and the next pass finds nothing to act on. (When the source is
 * UNMASKED -- OpenBSD's path -- the read stays non-destructive and W1C is the
 * ack; that contract is test_link_up_raises_interrupt, kept green.)
 */
static void test_xg_masked_mac_link_latch_self_clears_on_read(void)
{
    XframeTest t;

    xframe_start(&t);

    /* xg's mask state: the MAC link path stays masked at its all-ones reset. */
    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, TXPIC_INT_MASK, 0);
    g_assert_cmphex(pif_readq(&t, MAC_INT_MASK), ==, DISABLE_ALL_INTRS_64);
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_MASK), ==, DISABLE_ALL_INTRS_64);

    /* Bring the link up and service the GPIO bring-up edge as xg does. */
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);
    g_assert_cmphex(pif_readq(&t, GPIO_INT_REG) & GPIO_INT_REG_LINK_UP, ==,
                    GPIO_INT_REG_LINK_UP);

    /*
     * xg's alarm-handler read of MAC_RMAC_ERR_REG sees the one real link-state
     * change (the bring-up event latched the bit on both paths).
     */
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_REG) &
                    RMAC_LINK_STATE_CHANGE_INT, ==, RMAC_LINK_STATE_CHANGE_INT);

    /*
     * That read consumed the latch (the source is masked, so the read is the
     * only ack xg gives it): the next pass's read finds nothing. Before the
     * fix the masked W1C level stayed set here and xg re-ran its link handler
     * every scheduled pass, driving carrier down and up between passes.
     */
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_REG) &
                    RMAC_LINK_STATE_CHANGE_INT, ==, 0);

    xframe_stop(&t);
}

/*
 * Disabling the adapter (ADAPTER_EN 1->0) must clear any link source already
 * latched but not yet acked, not just cancel the timers: xframe_update_irq
 * gates the two link sources on their own masks alone, never on ADAPTER_EN,
 * so a stale unacked latch would keep INTx asserted for a disabled adapter.
 * Left in place, a stale GPIO_INT_REG LINK_UP would also let a later
 * re-enable's fresh LINK_DOWN edge OR onto it (xframe_link_up_timer_cb clears
 * only LINK_DOWN when it sets LINK_UP, never the reverse) -- both bits
 * latched together, the unstable state this model otherwise never presents.
 */
static void test_xg_adapter_disable_clears_pending_link_latches(void)
{
    XframeTest t;
    uint64_t gpio;

    xframe_start(&t);

    /* xg's exact mask state: open the two PIC leaves it opens, nothing else. */
    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, TXPIC_INT_MASK, 0);

    /* Bring the link all the way up, unserviced: both link latches pending. */
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);
    g_assert_true(xframe_intx_asserted(t.dev));

    /*
     * Disable without acking anything. INTx must drop from this write alone
     * -- before any register read could ack a latch -- which is only true if
     * the falling edge itself cleared the link sources.
     */
    pif_writeq(&t, ADAPTER_CONTROL, 0);
    g_assert_false(xframe_intx_asserted(t.dev));
    gpio = pif_readq(&t, GPIO_INT_REG);
    g_assert_cmphex(gpio & (GPIO_INT_REG_LINK_UP | GPIO_INT_REG_LINK_DOWN),
                    ==, 0);
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_REG) &
                    RMAC_LINK_STATE_CHANGE_INT, ==, 0);

    /*
     * Re-enable: the fresh bring-up edge presents a clean DOWN-only latch.
     * Before the fix, the disable left LINK_UP set, and this edge would OR
     * LINK_DOWN onto it -- both bits together instead of a clean DOWN.
     */
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_clock_step(t.qs->qts, LINK_UP_STEP_NS);
    g_assert_true(xframe_intx_asserted(t.dev));
    gpio = pif_readq(&t, GPIO_INT_REG);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_DOWN, ==, GPIO_INT_REG_LINK_DOWN);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_UP, ==, 0);

    xframe_stop(&t);
}

/*
 * The end-to-end carrier-steady guard: with the link up and the scheduled
 * interrupt feeding the watchdog, servicing that periodic interrupt must not
 * disturb carrier. Across several scheduled-interrupt periods -- each serviced
 * the way xg does, reading TXPIC_INT_REG for the scheduled source and
 * MAC_RMAC_ERR_REG in its alarm handler -- the scheduled interrupt keeps firing
 * and self-clearing while both link latches stay CLEAR: no GPIO link bit is
 * re-raised (the bring-up edge is delivered once), and the MAC link-change bit,
 * consumed on the first alarm read, never re-asserts. This is the flap the live
 * IRIX boot showed: before the fix the MAC link-change bit stayed latched and
 * xg re-ran its link handler on every scheduled pass, flapping carrier so its
 * transmit routine (which gates on carrier) never rang the doorbell.
 */
static void test_xg_scheduled_interrupt_holds_carrier_steady(void)
{
    XframeTest t;
    int period;

    xframe_start(&t);

    /* xg opens exactly the two PIC leaves; the MAC link path stays masked. */
    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, TXPIC_INT_MASK, 0);

    /* Arm the scheduled interrupt (the watchdog feed) and bring the link up. */
    pif_writeq(&t, SCHED_INT_CTRL,
               SCHED_INT_CTRL_TIMER_EN | SCHED_INT_TEST_PERIOD);
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);

    /* Service the bring-up link edge: the GPIO read acks it and INTx drops. */
    g_assert_cmphex(pif_readq(&t, GPIO_INT_REG) & GPIO_INT_REG_LINK_UP, ==,
                    GPIO_INT_REG_LINK_UP);
    /* xg's alarm read sees the one real link-state change and consumes it. */
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_REG) &
                    RMAC_LINK_STATE_CHANGE_INT, ==, RMAC_LINK_STATE_CHANGE_INT);
    g_assert_false(xframe_intx_asserted(t.dev));

    /*
     * Several watchdog periods. Each pass: the scheduled interrupt fires and
     * self-clears on the TXPIC read, while the alarm read of MAC_RMAC_ERR_REG
     * and the GPIO read both find their link latches clear -- carrier never
     * flaps. The MAC-latch assertion is the one that fails before the fix.
     */
    for (period = 0; period < 3; period++) {
        qtest_clock_step(t.qs->qts, SCHED_INT_STEP_NS);

        /* The scheduled interrupt fired, on the PIC/TXPIC path. */
        g_assert_true(xframe_intx_asserted(t.dev));
        g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==,
                        GEN_INTR_TXPIC);

        /* No link bit is set: the watchdog feed never touches carrier. */
        g_assert_cmphex(pif_readq(&t, GPIO_INT_REG) &
                        (GPIO_INT_REG_LINK_UP | GPIO_INT_REG_LINK_DOWN), ==, 0);
        g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_REG) &
                        RMAC_LINK_STATE_CHANGE_INT, ==, 0);

        /* The scheduled source self-clears on the TXPIC read; INTx drops. */
        g_assert_cmphex(pif_readq(&t, TXPIC_INT_REG), !=, 0);
        g_assert_false(xframe_intx_asserted(t.dev));
    }

    xframe_stop(&t);
}

/*
 * The same xg enable path for a traffic source: GENERAL_INT_MASK left at its
 * all-ones reset, transmit completion enabled through the block mask alone
 * (TX_TRAFFIC_MASK = 0, the way OpenBSD xge_init and Linux
 * en_dis_able_nic_intrs unmask it). A rung doorbell's completion must assert
 * INTx with the general mask untouched.
 */
static void test_xg_enable_path_transmit_via_block_masks(void)
{
    XframeTx t;
    uint8_t frame[64];
    uint8_t got[128];
    uint64_t par = TX_RAM_SCRATCH;
    uint64_t buf = TX_RAM_SCRATCH + 0x1000;
    size_t len;
    unsigned i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 1);
    }
    memset(frame, 0xff, 6);  /* broadcast destination */

    xframe_tx_start(&t);

    /* Top-level mask untouched (all-ones reset); unmask transmit at block. */
    g_assert_cmphex(sock_pif_readq(&t, GENERAL_INT_MASK), ==,
                    DISABLE_ALL_INTRS_64);
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, 0);
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_program_txd(&t, par, buf, frame, sizeof(frame));
    xframe_ring_doorbell(&t, par);
    len = net_socket_recv_frame(t.host_fd, got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));

    /* INTx asserts with the general mask all-ones: the block mask gates it. */
    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0, ==,
                    TX_TRAFFIC_FIFO0);

    /* The block mask gates the line. */
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(t.dev));
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, 0);
    g_assert_true(xframe_intx_asserted(t.dev));

    /* The ISR ack -- write the pending bits back -- drops the line. */
    sock_pif_writeq(&t, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_tx_stop(&t);
}

/*
 * QEMU's own backend link (the netdev's own carrier, toggled here by QMP
 * `set_link` the way an operator or a management layer would), not the
 * modelled bring-up sequence exercised above: the device had no
 * link_status_changed callback at all, so a `set_link off` left GPIO_INT_REG
 * showing whatever the bring-up sequence had already latched and the guest
 * kept believing it had carrier. A `set_link off` must present LINK_DOWN
 * (clearing any latched LINK_UP), and a following `set_link on` while the
 * adapter is still enabled must re-run the bring-up DOWN->UP transition from
 * its first edge -- a driver already running needs a genuine transition to
 * notice the link is back, the same reason the bring-up sequence itself is
 * an edge and not a level (see test_link_down_up_transition_at_bringup).
 * Fails before this change -- GPIO_INT_REG keeps reporting the settled
 * bring-up LINK_UP straight through the backend going down -- and passes
 * after.
 */
static void test_backend_link_down_presents_link_down(void)
{
    XframeTx t;
    uint64_t gpio;

    xframe_tx_start(&t);

    /* xg's exact mask state: open the two PIC leaves it opens, nothing else. */
    sock_pif_writeq(&t, PIC_INT_MASK, 0);
    sock_pif_writeq(&t, TXPIC_INT_MASK, 0);

    /* Bring the link up and service the bring-up edge, exactly as xg does. */
    sock_pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);
    g_assert_cmphex(sock_pif_readq(&t, GPIO_INT_REG) & GPIO_INT_REG_LINK_UP,
                    ==, GPIO_INT_REG_LINK_UP);
    g_assert_false(xframe_intx_asserted(t.dev));

    /*
     * The backend goes down: LINK_DOWN must be presented, not the settled
     * LINK_UP the bring-up sequence already delivered.
     */
    qtest_qmp_assert_success(t.qs->qts, "{'execute': 'set_link', 'arguments': "
                             "{'name': 'n0', 'up': false}}");
    g_assert_true(xframe_intx_asserted(t.dev));
    gpio = sock_pif_readq(&t, GPIO_INT_REG);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_DOWN, ==, GPIO_INT_REG_LINK_DOWN);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_UP, ==, 0);
    g_assert_false(xframe_intx_asserted(t.dev));

    /* No spurious re-raise while the backend just stays down. */
    qtest_clock_step(t.qs->qts, SCHED_INT_STEP_NS);
    g_assert_false(xframe_intx_asserted(t.dev));
    gpio = sock_pif_readq(&t, GPIO_INT_REG);
    g_assert_cmphex(gpio & (GPIO_INT_REG_LINK_UP | GPIO_INT_REG_LINK_DOWN),
                    ==, 0);

    /*
     * Backend recovers with the adapter still enabled: a fresh DOWN->UP
     * transition runs, the two edges arriving one interval apart as usual.
     */
    qtest_qmp_assert_success(t.qs->qts, "{'execute': 'set_link', 'arguments': "
                             "{'name': 'n0', 'up': true}}");
    qtest_clock_step(t.qs->qts, LINK_UP_STEP_NS);
    gpio = sock_pif_readq(&t, GPIO_INT_REG);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_DOWN, ==, GPIO_INT_REG_LINK_DOWN);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_UP, ==, 0);

    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);
    gpio = sock_pif_readq(&t, GPIO_INT_REG);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_UP, ==, GPIO_INT_REG_LINK_UP);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_DOWN, ==, 0);

    xframe_tx_stop(&t);
}

/*
 * xframe_link_up_timer_cb's own guard against the backend link -- reachable
 * even though xframe_set_link_status cancels an already-armed bring-up
 * timer the moment the backend drops, because ADAPTER_CONTROL's own write
 * path arms the FIRST edge (xframe_link_down_timer_cb, which arms this one
 * in turn) without ever consulting the backend. A backend already down
 * before the guest enables the adapter is exactly that case: the bring-up
 * sequence arms and runs both one-shots regardless, and the second edge must
 * not announce LINK_UP for a backend that never came up. Fails before this
 * change -- xframe_link_up_timer_cb set LINK_UP unconditionally -- and
 * passes after.
 */
static void test_link_up_timer_respects_backend_already_down(void)
{
    XframeTx t;
    uint64_t gpio;

    xframe_tx_start(&t);

    sock_pif_writeq(&t, PIC_INT_MASK, 0);
    sock_pif_writeq(&t, TXPIC_INT_MASK, 0);

    /* The backend is down before the guest ever enables the adapter. */
    qtest_qmp_assert_success(t.qs->qts, "{'execute': 'set_link', 'arguments': "
                             "{'name': 'n0', 'up': false}}");

    /* Enable the adapter and step through both bring-up edges in one go. */
    sock_pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);

    /*
     * The second edge has fired. With the backend still down, only
     * LINK_DOWN is latched -- never a spurious LINK_UP.
     */
    g_assert_true(xframe_intx_asserted(t.dev));
    gpio = sock_pif_readq(&t, GPIO_INT_REG);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_UP, ==, 0);
    g_assert_cmphex(gpio & GPIO_INT_REG_LINK_DOWN, ==, GPIO_INT_REG_LINK_DOWN);
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_tx_stop(&t);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_device("xframe")) {
        return 0;
    }

    qtest_add_func("xframe/identity", test_identity);
    qtest_add_func("xframe/device-id-selects-herc",
                   test_device_id_selects_herc);
    qtest_add_func("xframe/subsystem-is-unbranded-by-default",
                   test_subsystem_is_unbranded_by_default);
    qtest_add_func("xframe/subsystem-vendor-when-sgi-board",
                   test_subsystem_vendor_when_sgi_board);
    qtest_add_func("xframe/bar0-is-a-64bit-pif-window",
                   test_bar0_is_a_64bit_pif_window);
    qtest_add_func("xframe/bar2-is-the-txp-window",
                   test_bar2_is_the_txp_window);
    qtest_add_func("xframe/swapper-diagnostic-returns-magic",
                   test_swapper_diagnostic_returns_magic);
    qtest_add_func("xframe/xmsi-address-roundtrips",
                   test_xmsi_address_roundtrips);
    qtest_add_func("xframe/adapter-status-is-quiescent",
                   test_adapter_status_is_quiescent);
    qtest_add_func("xframe/adapter-en-is-settable",
                   test_adapter_en_is_settable);
    qtest_add_func("xframe/link-up-raises-interrupt",
                   test_link_up_raises_interrupt);
    qtest_add_func("xframe/sw-reset-key-clears-state",
                   test_sw_reset_key_clears_state);
    qtest_add_func("xframe/mac-config-write-lock", test_mac_config_write_lock);
    qtest_add_func("xframe/factory-mac-readback", test_factory_mac_readback);
    qtest_add_func("xframe/swapper-diagnostic-32bit-halves",
                   test_swapper_diagnostic_32bit_halves);
    qtest_add_func("xframe/adapter-status-32bit-halves",
                   test_adapter_status_32bit_halves);
    qtest_add_func("xframe/xmsi-address-32bit-halves",
                   test_xmsi_address_32bit_halves);
    qtest_add_func("xframe/sw-reset-key-via-32bit",
                   test_sw_reset_key_via_32bit);
    qtest_add_func("xframe/rmac-cfg-key-unlock-via-32bit",
                   test_rmac_cfg_key_unlock_via_32bit);
    qtest_add_func("xframe/mac-table-strobe-via-32bit",
                   test_mac_table_strobe_via_32bit);
    qtest_add_func("xframe/transmit-emits-a-frame",
                   test_transmit_emits_a_frame);
    qtest_add_func("xframe/transmit-out-of-range-pointer-is-safe",
                   test_transmit_out_of_range_pointer_is_safe);
    qtest_add_func("xframe/transmit-completion-raises-interrupt",
                   test_transmit_completion_raises_interrupt);
    qtest_add_func("xframe/descriptors-honor-swapper-ctrl-little-endian",
                   test_descriptors_honor_swapper_ctrl_little_endian);
    qtest_add_func("xframe/xg-enable-path-link-via-block-masks",
                   test_xg_enable_path_link_via_block_masks);
    qtest_add_func("xframe/xg-enable-path-link-via-gpio-block",
                   test_xg_enable_path_link_via_gpio_block);
    qtest_add_func("xframe/xg-bringup-pic-summary-serviced",
                   test_xg_bringup_pic_summary_serviced);
    qtest_add_func("xframe/xg-link-up-gpio-read-acks",
                   test_xg_link_up_gpio_read_acks);
    qtest_add_func("xframe/scheduled-interrupt-feeds-watchdog",
                   test_scheduled_interrupt_feeds_watchdog);
    qtest_add_func("xframe/xg-scheduled-interrupt-acks-via-write",
                   test_xg_scheduled_interrupt_acks_via_write);
    qtest_add_func("xframe/link-up-delivered-once",
                   test_link_up_delivered_once);
    qtest_add_func("xframe/link-down-up-transition-at-bringup",
                   test_link_down_up_transition_at_bringup);
    qtest_add_func("xframe/xg-redundant-adapter-en-does-not-relink",
                   test_xg_redundant_adapter_en_does_not_relink);
    qtest_add_func("xframe/xg-masked-mac-link-latch-self-clears-on-read",
                   test_xg_masked_mac_link_latch_self_clears_on_read);
    qtest_add_func("xframe/xg-adapter-disable-clears-pending-link-latches",
                   test_xg_adapter_disable_clears_pending_link_latches);
    qtest_add_func("xframe/xg-scheduled-interrupt-holds-carrier-steady",
                   test_xg_scheduled_interrupt_holds_carrier_steady);
    qtest_add_func("xframe/xg-enable-path-transmit-via-block-masks",
                   test_xg_enable_path_transmit_via_block_masks);
    qtest_add_func("xframe/backend-link-down-presents-link-down",
                   test_backend_link_down_presents_link_down);
    qtest_add_func("xframe/link-up-timer-respects-backend-already-down",
                   test_link_up_timer_respects_backend_already_down);
    qtest_add_func("xframe/receive-delivers-a-frame",
                   test_receive_delivers_a_frame);
    qtest_add_func("xframe/receive-out-of-range-ring-pointer-is-safe",
                   test_receive_out_of_range_ring_pointer_is_safe);
    qtest_add_func("xframe/receive-filtered-frame-does-not-wedge",
                   test_receive_filtered_frame_does_not_wedge);
    qtest_add_func("xframe/receive-buffer-too-small-truncates",
                   test_receive_buffer_too_small_truncates);
    qtest_add_func("xframe/receive-recovers-after-exhaustion-via-recovery-poll",
                   test_receive_recovers_after_exhaustion_via_recovery_poll);
    qtest_add_func("xframe/receive-multicast-exact-match",
                   test_receive_multicast_exact_match);
    qtest_add_func("xframe/receive-multicast-allmulti-wildcard",
                   test_receive_multicast_allmulti_wildcard);
    qtest_add_func("xframe/receive-unicast-still-matches",
                   test_receive_unicast_still_matches);

    return g_test_run();
}
