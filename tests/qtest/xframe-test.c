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
#define TTI_COMMAND_MEM          0x1150
#define TTI_DATA1_MEM            0x1158
#define TTI_DATA2_MEM            0x1160
#define RTI_COMMAND_MEM          0x19b8
#define RTI_DATA1_MEM            0x19c0
#define RTI_DATA2_MEM            0x19c8
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

#define TRAFFIC_MEM_CMD_WE       (1ULL << 56)
#define TRAFFIC_MEM_CMD_STROBE   (1ULL << 48)
#define TTI_MEM_CMD_OFF(n)       ((uint64_t)(n) << 32)
#define RTI_MEM_CMD_OFF(n)       ((uint64_t)(n) << 32)

/* Linux Xframe-I INTA and OpenBSD entry-0 programming vectors. */
#define LINUX_TTI_DATA1          0x00002078030a1030ULL
#define LINUX_TTI_DATA2          0x0010002000400080ULL
#define LINUX_RTI_DATA1          0x00000fff020a1030ULL
#define LINUX_RTI_DATA2          0x0001000200400080ULL
#define OPENBSD_TTI_DATA1        0x000001ff02051430ULL
#define OPENBSD_TTI_DATA2        0x0019004000800200ULL
#define OPENBSD_RTI_DATA1        0x0000080002051432ULL
#define OPENBSD_RTI_DATA2        0x0040008001000200ULL

#define TRAFFIC_DATA1_TIMER(n)   ((uint64_t)(n) << 32)
#define TRAFFIC_DATA1_AC         (1ULL << 25)
#define TRAFFIC_DATA1_CI         (1ULL << 24)
#define TRAFFIC_DATA2_COUNTS(a, b, c, d) \
    (((uint64_t)(a) << 48) | ((uint64_t)(b) << 32) | \
     ((uint64_t)(c) << 16) | (d))

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
#define TRAFFIC_INT_ENTRY(n)     (1ULL << (63 - (n)))
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
 * Segmentation offload, in the same first control word (Linux s2io.h
 * TXD_TCP_LSO_EN is s2BIT(30), i.e. bit 33, and TXD_TCP_LSO_MSS is
 * vBIT(val,34,14), i.e. bits 29:16).
 */
#define TXD_LSO_EN               (1ULL << 33)
#define TXD_LSO_MSS(v)           (((uint64_t)(v) & 0x3fff) << 16)

/* The card's jumbo frame limit; an unsegmented buffer arrives at this size. */
#define XFRAME_MAX_FRAME_FOR_TEST 9600

/*
 * The checksum-insert requests in the second control word (Linux s2io-regs.h
 * TXD_TX_CKO_IPV4_EN, _TCP_EN and _UDP_EN, that driver's own MSB-first bit
 * numbering: bits 5, 6 and 7).
 */
#define TXD_CKO_IPV4             (1ULL << 58)
#define TXD_CKO_TCP              (1ULL << 57)
#define TXD_CKO_UDP              (1ULL << 56)

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

/*
 * Receive ring mode, PRC_CTRL_0 bits 49:48. IRIX arms mode 2 -- the public
 * s2io header's five-buffer header-split layout, 64-byte descriptors (three
 * control words plus five pointers) -- as soon as the MTU is raised to a jumbo
 * value.
 */
#define RC_RING_MODE_5BUF        (2ULL << 48)
#define RXD5_SIZE                64
#define RXD5_SET_SIZES(b0, b1, b2)                                          \
    ((((uint64_t)(b0) & 0xff) << 48) | (((uint64_t)(b1) & 0xffff) << 32) |  \
     (((uint64_t)(b2) & 0xffff) << 16))
#define RXD5_GET_B0(c2)          (((c2) >> 48) & 0xff)
#define RXD5_GET_B1(c2)          (((c2) >> 32) & 0xffff)
#define RXD5_GET_B2(c2)          (((c2) >> 16) & 0xffff)
#define RX_TRAFFIC_INT           0x08f0

/*
 * The statistics block: STAT_ADDR takes the guest address, STAT_CFG's top bit
 * enables the periodic DMA (public s2io stat_cfg / stat_addr; field offsets
 * from its stat_block struct). The block's byte order follows the guest's
 * SWAPPER_CTRL_STATS_SE bit -- big-endian at reset, as these tests read it
 * with dma_ldl_be below.
 */
#define STAT_CFG                 0x09d0
#define STAT_ADDR                0x09d8
#define STAT_CFG_EN              (1ULL << 63)
#define STAT_CFG_ONE_SHOT        (1ULL << 62)
#define STAT_TMAC_DATA_OCTETS    0x000
#define STAT_TMAC_FRMS           0x004
#define STAT_RMAC_DATA_OCTETS    0x060
#define STAT_RMAC_VLD_FRMS       0x064
/* The card writes the hardware section [0, 0x320); the driver's tail follows. */
#define STAT_BLOCK_SIZE          0x320
#define STAT_BLOCK_ADDR          (TX_RAM_SCRATCH + 0x30000)

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
 * What the card reports about a received frame in the first control word
 * (Linux s2io.h RXD_FRAME_PROTO_IPV4 s2BIT(27) and _PROTO_TCP s2BIT(30), so
 * bits 36 and 33; RXD_GET_L3_CKSUM is the half at 31:16 and RXD_GET_L4_CKSUM
 * the half at 15:0, each 0xFFFF when that checksum verified).
 */
#define RXD_PROTO_IPV4           (1ULL << 36)
#define RXD_PROTO_TCP            (1ULL << 33)
#define RXD_CKSUM_OK             0xffffULL

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
 * A 32-bit big-endian read for the statistics block, whose fields the card
 * lays out in the order the guest's SWAPPER_CTRL_STATS_SE bit selects. With
 * that bit clear -- the reset state these tests run in, matching IRIX -- the
 * block is big-endian, exactly as descriptors are. Do not use qtest_readl: it
 * applies the x86_64 test target's little-endian correction and would read the
 * block byte-reversed.
 */
static uint32_t dma_ldl_be(QTestState *qts, uint64_t addr)
{
    uint8_t b[4];
    uint32_t val = 0;
    int i;

    qtest_memread(qts, addr, b, sizeof(b));
    for (i = 0; i < 4; i++) {
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

static void xframe_tx_start_args(XframeTx *t, const char *args)
{
    uint64_t barsize;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 5);
    t->host_fd = sv[0];

    t->qs = qtest_pc_boot("-netdev socket,fd=%d,id=n0 "
                          "-device xframe,netdev=n0,mac=" XFRAME_TEST_MAC
                          " %s", sv[1], args ? args : "");
    t->dev = find_xframe(t->qs->pcibus, XFRAME_DEVICE_XENA);
    g_assert(t->dev != NULL);

    t->pif = qpci_iomap(t->dev, 0, &barsize);
    g_assert_cmphex(barsize, ==, XFRAME_PIF_WINDOW_SIZE);

    t->txp = qpci_iomap(t->dev, 2, &barsize);
    g_assert_cmphex(barsize, ==, XFRAME_TXP_WINDOW_SIZE);

    qpci_device_enable(t->dev);
}

static void xframe_tx_start(XframeTx *t)
{
    xframe_tx_start_args(t, NULL);
}

static void xframe_tx_start_binary_args(XframeTx *t, const char *binary,
                                        const char *args)
{
    g_autofree char *saved = g_strdup(g_getenv("QTEST_QEMU_BINARY"));

    g_assert_true(g_setenv("QTEST_QEMU_BINARY", binary, true));
    xframe_tx_start_args(t, args);
    if (saved) {
        g_assert_true(g_setenv("QTEST_QEMU_BINARY", saved, true));
    } else {
        g_unsetenv("QTEST_QEMU_BINARY");
    }
}

static void xframe_user_start(XframeTx *t)
{
    uint64_t barsize;

    t->host_fd = -1;
    t->qs = qtest_pc_boot("-netdev user,id=n0 "
                          "-device xframe,netdev=n0,mac=" XFRAME_TEST_MAC);
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
    if (t->host_fd >= 0) {
        close(t->host_fd);
    }
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
 * The net core asks can_receive immediately before delivering a packet.
 * Peeking the head descriptor there and fetching the same immutable,
 * card-owned descriptor again in receive doubles descriptor DMA traffic.
 * Trace the actual PCI-DMA fetch boundary so this remains a performance
 * contract rather than a timing-sensitive wall-clock benchmark.
 */
static void test_receive_fetches_descriptor_once(void)
{
    XframeTx t;
    uint8_t frame[64] = { 0 };
    g_autofree char *trace_path = NULL;
    g_autofree char *trace_args = NULL;
    g_autofree char *trace = NULL;
    const char *p;
    unsigned fetches = 0;
    int fd;

    memcpy(frame, xframe_station_mac, sizeof(xframe_station_mac));
    fd = g_file_open_tmp("xframe-rx-trace-XXXXXX", &trace_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    trace_args = g_strdup_printf(
        "-trace xframe_rx_descriptor_fetch,file=%s", trace_path);
    xframe_tx_start_args(&t, trace_args);
    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF0, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);

    net_socket_send_frame(t.host_fd, frame, sizeof(frame));
    g_assert_true(xframe_wait_rxd_consumed(&t, RX_RING_BASE, 5000));
    xframe_tx_stop(&t);

    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, NULL));
    for (p = trace; (p = strstr(p, "xframe_rx_descriptor_fetch")); p++) {
        fetches++;
    }
    g_assert_cmpuint(fetches, ==, 1);
    g_assert_cmpint(g_unlink(trace_path), ==, 0);
}

/*
 * RX_TRAFFIC_INT is a sticky, level-triggered completion source. Once the
 * first frame has latched ring 0, another completed frame cannot change the
 * delivered IRQ state until the guest acknowledges it. Recomputing that
 * unchanged state on every frame only repeats host work in a bulk receive.
 */
static void test_receive_recomputes_pending_irq_once(void)
{
    const char *event = "xframe_rx_completion_irq_recompute";
    XframeTx t;
    uint8_t frame[64] = { 0 };
    uint64_t slot0 = RX_RING_BASE;
    uint64_t slot1 = RX_RING_BASE + RXD_SIZE;
    g_autofree char *trace_path = NULL;
    g_autofree char *trace_args = NULL;
    g_autofree char *trace = NULL;
    const char *p;
    unsigned recomputes = 0;
    int fd;

    memcpy(frame, xframe_station_mac, sizeof(xframe_station_mac));
    fd = g_file_open_tmp("xframe-rx-trace-XXXXXX", &trace_path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    trace_args = g_strdup_printf(
        "-trace xframe_rx_completion_irq_recompute,file=%s", trace_path);
    xframe_tx_start_args(&t, trace_args);
    sock_pif_writeq(&t, RX_TRAFFIC_MASK, 0);
    xframe_program_rxd(&t, slot0, RX_BUF0, RX_BUF_CAPACITY);
    xframe_program_rxd(&t, slot1, RX_BUF1, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);

    net_socket_send_frame(t.host_fd, frame, sizeof(frame));
    g_assert_true(xframe_wait_rxd_consumed(&t, slot0, 5000));
    net_socket_send_frame(t.host_fd, frame, sizeof(frame));
    g_assert_true(xframe_wait_rxd_consumed(&t, slot1, 5000));
    g_assert_cmphex(sock_pif_readq(&t, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0,
                    ==, RX_TRAFFIC_RING0);
    g_assert_true(xframe_intx_asserted(t.dev));
    xframe_tx_stop(&t);

    g_assert_true(g_file_get_contents(trace_path, &trace, NULL, NULL));
    for (p = trace; (p = strstr(p, event)); p++) {
        recomputes++;
    }
    g_assert_cmpuint(recomputes, ==, 1);
    g_assert_cmpint(g_unlink(trace_path), ==, 0);
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
 * One key unlocks one MAC_CFG write, however many bus cycles the guest spends
 * on it. Drivers reach this 64-bit register through 32-bit cycles -- Linux
 * s2io writes it as two halves on the Xena part, and the Origin 200 Bridge
 * splits every 64-bit access that way for a slot with DEV_64BIT_DEVICE clear,
 * which is the state the live IRIX boot leaves -- so a model that releases the
 * lock on the FIRST access refuses the second and keeps half of what the guest
 * wrote.
 *
 * That is not a cosmetic loss. MAC_CFG carries the receive enables, so on the
 * real guest it left the register reading zero after a re-bring-up, with
 * broadcast reception disabled: every ARP dropped, nothing resolved, and the
 * only sign of it a single log line saying one MAC_CFG write had been ignored.
 *
 * The payload here deliberately has bits in BOTH halves; the whole-register
 * lock test above cannot catch this because its value lives entirely in one.
 */
static void test_mac_config_write_lock_spans_both_halves(void)
{
    XframeTest t;
    const uint64_t payload = MAC_CFG_PAYLOAD | 0x00000000000000c0ULL;

    xframe_start(&t);

    pif_writeq(&t, RMAC_CFG_KEY, RMAC_CFG_KEY_VALUE);
    pif_writel(&t, MAC_CFG + 0, (uint32_t)payload);
    pif_writel(&t, MAC_CFG + 4, (uint32_t)(payload >> 32));

    g_assert_cmphex(pif_readq(&t, MAC_CFG), ==, payload);

    xframe_stop(&t);
}

/*
 * TTI and RTI are indirect configuration memories. DATA1/DATA2 stage a
 * public-driver vector; write-enable plus strobe commits the selected entry
 * and the command completes synchronously. TTI exposes a six-bit selector
 * even though the modelled card has eight FIFOs, so every selector beyond 7
 * must be rejected without aliasing or disturbing the staging words.
 */
static void test_traffic_interrupt_memory_programming(void)
{
    XframeTest t;
    unsigned entry;

    xframe_start(&t);

    pif_writeq(&t, TTI_DATA1_MEM, LINUX_TTI_DATA1);
    pif_writeq(&t, TTI_DATA2_MEM, LINUX_TTI_DATA2);
    g_assert_cmphex(pif_readq(&t, TTI_DATA1_MEM), ==, LINUX_TTI_DATA1);
    g_assert_cmphex(pif_readq(&t, TTI_DATA2_MEM), ==, LINUX_TTI_DATA2);
    pif_writeq(&t, TTI_COMMAND_MEM,
               TRAFFIC_MEM_CMD_WE | TRAFFIC_MEM_CMD_STROBE |
               TTI_MEM_CMD_OFF(0));
    g_assert_cmphex(pif_readq(&t, TTI_COMMAND_MEM) &
                    TRAFFIC_MEM_CMD_STROBE, ==, 0);

    pif_writeq(&t, TTI_DATA1_MEM, OPENBSD_TTI_DATA1);
    pif_writeq(&t, TTI_DATA2_MEM, OPENBSD_TTI_DATA2);
    pif_writeq(&t, TTI_COMMAND_MEM,
               TRAFFIC_MEM_CMD_WE | TRAFFIC_MEM_CMD_STROBE |
               TTI_MEM_CMD_OFF(7));

    /* Keep both valid endpoints distinct across every invalid selector. */
    pif_writeq(&t, TTI_DATA1_MEM,
               TRAFFIC_DATA1_TIMER(0x100) | TRAFFIC_DATA1_CI);
    pif_writeq(&t, TTI_DATA2_MEM, TRAFFIC_DATA2_COUNTS(1, 1, 1, 1));
    pif_writeq(&t, TTI_COMMAND_MEM,
               TRAFFIC_MEM_CMD_WE | TRAFFIC_MEM_CMD_STROBE |
               TTI_MEM_CMD_OFF(0));
    pif_writeq(&t, TTI_DATA1_MEM,
               TRAFFIC_DATA1_TIMER(0x200) | TRAFFIC_DATA1_CI);
    pif_writeq(&t, TTI_COMMAND_MEM,
               TRAFFIC_MEM_CMD_WE | TRAFFIC_MEM_CMD_STROBE |
               TTI_MEM_CMD_OFF(7));

    /* Strobe alone must not commit the staged CI configuration to entry 1. */
    pif_writeq(&t, TTI_DATA1_MEM,
               TRAFFIC_DATA1_TIMER(0x80) | TRAFFIC_DATA1_CI);
    pif_writeq(&t, TTI_COMMAND_MEM,
               TRAFFIC_MEM_CMD_STROBE | TTI_MEM_CMD_OFF(1));
    g_assert_cmphex(pif_readq(&t, TTI_COMMAND_MEM) &
                    TRAFFIC_MEM_CMD_STROBE, ==, 0);

    pif_writeq(&t, TTI_DATA1_MEM, OPENBSD_TTI_DATA1);
    pif_writeq(&t, TTI_DATA2_MEM, OPENBSD_TTI_DATA2);
    for (entry = 8; entry < 64; entry++) {
        pif_writeq(&t, TTI_COMMAND_MEM,
                   TRAFFIC_MEM_CMD_WE | TRAFFIC_MEM_CMD_STROBE |
                   TTI_MEM_CMD_OFF(entry));
        g_assert_cmphex(pif_readq(&t, TTI_COMMAND_MEM) &
                        TRAFFIC_MEM_CMD_STROBE, ==, 0);
        g_assert_cmphex(pif_readq(&t, TTI_DATA1_MEM), ==,
                        OPENBSD_TTI_DATA1);
        g_assert_cmphex(pif_readq(&t, TTI_DATA2_MEM), ==,
                        OPENBSD_TTI_DATA2);
    }

    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_clock_step(t.qs->qts, 130000);
    g_assert_cmphex(pif_readq(&t, TX_TRAFFIC_INT), ==,
                    TRAFFIC_INT_ENTRY(0));
    pif_writeq(&t, TX_TRAFFIC_INT, TRAFFIC_INT_ENTRY(0));
    qtest_clock_step(t.qs->qts, 130000);
    g_assert_cmphex(pif_readq(&t, TX_TRAFFIC_INT), ==,
                    TRAFFIC_INT_ENTRY(0) | TRAFFIC_INT_ENTRY(7));

    pif_writeq(&t, RTI_DATA1_MEM, LINUX_RTI_DATA1);
    pif_writeq(&t, RTI_DATA2_MEM, LINUX_RTI_DATA2);
    pif_writeq(&t, RTI_COMMAND_MEM,
               TRAFFIC_MEM_CMD_WE | TRAFFIC_MEM_CMD_STROBE |
               RTI_MEM_CMD_OFF(0));
    g_assert_cmphex(pif_readq(&t, RTI_COMMAND_MEM) &
                    TRAFFIC_MEM_CMD_STROBE, ==, 0);
    pif_writeq(&t, RTI_DATA1_MEM, OPENBSD_RTI_DATA1);
    pif_writeq(&t, RTI_DATA2_MEM, OPENBSD_RTI_DATA2);
    pif_writeq(&t, RTI_COMMAND_MEM,
               TRAFFIC_MEM_CMD_WE | TRAFFIC_MEM_CMD_STROBE |
               RTI_MEM_CMD_OFF(7));

    pif_writeq(&t, SW_RESET, SW_RESET_KEY);
    g_assert_cmphex(pif_readq(&t, TTI_COMMAND_MEM), ==, 0);
    g_assert_cmphex(pif_readq(&t, TTI_DATA1_MEM), ==, 0);
    g_assert_cmphex(pif_readq(&t, TTI_DATA2_MEM), ==, 0);
    g_assert_cmphex(pif_readq(&t, RTI_COMMAND_MEM), ==, 0);
    g_assert_cmphex(pif_readq(&t, RTI_DATA1_MEM), ==, 0);
    g_assert_cmphex(pif_readq(&t, RTI_DATA2_MEM), ==, 0);

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

static void test_user_backend_reply_waits_for_transmit(void)
{
    static const uint8_t gateway_ip[4] = { 10, 0, 2, 2 };
    static const uint8_t guest_ip[4] = { 10, 0, 2, 15 };
    static const uint8_t arp_request[42] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56,
        0x08, 0x06,
        0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56, 10, 0, 2, 15,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 10, 0, 2, 2,
    };
    XframeTx t;
    uint64_t lcr = TXDL_NUMTXD(0) | TXDL_LGC_FIRST | TXDL_LGC_LAST;
    uint32_t rxd_control1_hi, after_poll;
    uint8_t landed[sizeof(arp_request)];

    xframe_user_start(&t);
    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF0, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);
    xframe_program_txd(&t, TX_RAM_SCRATCH, TX_RAM_SCRATCH + 0x1000,
                       arp_request, sizeof(arp_request));

    qpci_io_writeq(t.dev, t.txp, TXP_TXDL_PAR, TX_RAM_SCRATCH);
    qpci_io_writel(t.dev, t.txp, TXP_TXDL_LCR, (uint32_t)lcr);
    /*
     * The LCR high half rings the doorbell. User networking answers this ARP
     * request synchronously, but receive must wait until the write callback
     * returns and the transmit completion is visible. The RxD is big-endian:
     * its bit-56 ownership byte reads as 1 through x86 qtest's readl.
     */
    rxd_control1_hi = qtest_writel_then_readl_poll(
        t.qs->qts, t.txp.addr + TXP_TXDL_LCR + 4,
        (uint32_t)(lcr >> 32), RX_RING_BASE + 8, &after_poll);
    g_assert_cmphex(rxd_control1_hi, ==, 1);
    g_assert_cmphex(after_poll & 1, ==, 0);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0, ==,
                    TX_TRAFFIC_FIFO0);

    qtest_memread(t.qs->qts, RX_BUF0, landed, sizeof(landed));
    g_assert_cmpmem(landed, 6, xframe_station_mac, 6);
    g_assert_cmphex(landed[20], ==, 0x00);
    g_assert_cmphex(landed[21], ==, 0x02);
    g_assert_cmpmem(landed + 28, 4, gateway_ip, 4);
    g_assert_cmpmem(landed + 38, 4, guest_ip, 4);

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
 * A driver does not transmit once, it transmits for as long as traffic lasts:
 * ring, take the completion, ack it, ring again. The single-cycle test above
 * cannot see a completion that only goes missing on a later pass -- a source
 * that latches once and is never cleared again, or a re-assertion the model
 * never makes -- and a driver that stops being told its frames completed stops
 * transmitting at all. So run the cycle repeatedly and require the same three
 * things of every pass: the frame egresses, the completion is raised on THAT
 * pass, and the ack puts the line back down with nothing latched behind it.
 *
 * The check that earns this test its place is the one at the top of each
 * cycle: the line must be low before the doorbell. A stuck source would leave
 * it high, and every later completion would then land on an already-asserted
 * line where it can no longer be told apart from the one before it.
 */
static void test_transmit_completion_survives_repeated_cycles(void)
{
    XframeTx t;
    uint8_t frame[64];
    uint8_t got[128];
    uint64_t par = TX_RAM_SCRATCH;
    uint64_t buf = TX_RAM_SCRATCH + 0x1000;
    uint64_t pending;
    size_t len;
    unsigned cycle, i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 1);
    }
    memset(frame, 0xff, 6);  /* broadcast destination */

    xframe_tx_start(&t);
    sock_pif_writeq(&t, GENERAL_INT_MASK, 0);
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, 0);

    for (cycle = 0; cycle < 8; cycle++) {
        /* Nothing left asserted by the previous cycle's ack. */
        g_assert_false(xframe_intx_asserted(t.dev));

        xframe_program_txd(&t, par, buf, frame, sizeof(frame));
        xframe_ring_doorbell(&t, par);

        len = net_socket_recv_frame(t.host_fd, got, sizeof(got));
        g_assert_cmpint(len, ==, sizeof(frame));
        g_assert_cmpmem(got, len, frame, sizeof(frame));

        g_assert_true(xframe_intx_asserted(t.dev));
        pending = sock_pif_readq(&t, TX_TRAFFIC_INT);
        g_assert_cmphex(pending & TX_TRAFFIC_FIFO0, ==, TX_TRAFFIC_FIFO0);

        /* The descriptor comes back to the host on every cycle, not just one. */
        g_assert_cmphex(dma_ldq_be(t.qs->qts, par) & TXD_OWN, ==, 0);

        sock_pif_writeq(&t, TX_TRAFFIC_INT, pending);
        g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);
    }

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

/*
 * ==========================================================================
 * Device-level protocol-traffic and interrupt-rate tests.
 *
 * The xframe carries whatever bytes the guest hands it: no checksum offload,
 * no segmentation, no header inspection, so a complete Ethernet II / IPv4 /
 * TCP or UDP frame must cross transmit and receive byte-for-byte -- its L3
 * (IPv4 header) and L4 (TCP/UDP) checksum fields included. That checksum
 * pass-through is the exact class of corruption that once bit the IOC3 model,
 * which overwrote an L4 checksum with a partial value and had slirp silently
 * drop the frames; these tests prove xframe leaves the fields alone, even a
 * deliberately wrong one it would have to rewrite if it had any offload.
 *
 * The frames use public RFC layouts only (Ethernet II, RFC 791 IPv4, RFC 793
 * TCP, RFC 768 UDP) with the standard internet checksum, built here rather
 * than captured. Sizes run from a sub-64-byte padded runt through a full
 * 1500-MTU frame to the device's XFRAME_MAX_FRAME jumbo maximum.
 *
 * The last two tests pin the unconfigured fallback: an entry that the guest
 * has not committed raises one completion interrupt per frame. Acking between
 * frames makes each completion a fresh, countable INTx assertion.
 * ==========================================================================
 */

/*
 * Guest RAM for the traffic tests: well clear of the transmit/receive scratch
 * the register tests above use, and spaced for a full XFRAME_MAX_FRAME
 * (9600-byte) jumbo frame in each buffer.
 */
#define TRAFFIC_TXD_PAR    0x03000000u   /* transmit descriptor list */
#define TRAFFIC_TX_BUF     0x03010000u   /* transmit frame buffer */
#define TRAFFIC_RX_RING    0x03100000u   /* receive ring base (one page) */
#define TRAFFIC_RX_BUF     0x03200000u   /* receive frame buffer */

/* Header sizes and the frame max, from the public RFC layouts and the
 * device's own XFRAME_MAX_FRAME (9600). */
#define ETH_HDR_LEN            14
#define IPV4_HDR_LEN          20
#define TCP_HDR_LEN          20
#define UDP_HDR_LEN           8
#define XFRAME_MAX_FRAME_BYTES  9600

/* Where each checksum field sits in the assembled frame -- what the
 * pass-through assertions read back. IPV4_CSUM at the IPv4 header's bytes
 * 10-11, TCP_CSUM at the TCP header's bytes 16-17, UDP_CSUM at the UDP
 * header's bytes 6-7. */
#define IPV4_CSUM_OFF   (ETH_HDR_LEN + 10)
#define TCP_CSUM_OFF    (ETH_HDR_LEN + IPV4_HDR_LEN + 16)
#define UDP_CSUM_OFF    (ETH_HDR_LEN + IPV4_HDR_LEN + 6)

/* How many frames the interrupt-rate tests push back to back. */
#define TRAFFIC_BURST   64

/* The IPv4 endpoints every built frame uses -- shared so the builder and the
 * checksum validators agree on the pseudo-header. */
static const uint8_t traffic_src_ip[4] = { 10, 0, 0, 1 };
static const uint8_t traffic_dst_ip[4] = { 10, 0, 0, 2 };

/* The remote unicast destination for transmitted frames (transmit does not
 * filter, so any address serves). */
static const uint8_t traffic_remote_mac[6] = {
    0x02, 0x00, 0x53, 0x47, 0x49, 0x02
};

/*
 * The internet checksum (RFC 1071): a 16-bit ones-complement sum. _add folds
 * bytes into a running 32-bit accumulator (big-endian 16-bit words, an odd
 * trailing byte in the high half); _fold carries the accumulator down and
 * complements it. A correct field makes the sum over the covered bytes --
 * that field included -- fold to zero, which the validators below check.
 */
static uint32_t frame_csum_add(uint32_t sum, const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
    }
    if (i < len) {
        sum += (uint32_t)data[i] << 8;
    }
    return sum;
}

static uint16_t frame_csum_fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* Read a big-endian 16-bit field out of a frame -- the wire order of every
 * checksum and length field. */
static uint16_t frame_be16(const uint8_t *frame, size_t off)
{
    return ((uint16_t)frame[off] << 8) | frame[off + 1];
}

/*
 * Assemble a complete Ethernet II / IPv4 / (TCP|UDP) frame into buf with
 * correct L3 and L4 checksums, zero-padded to total_len (>= the natural
 * header+payload length -- the excess is Ethernet padding, as a runt gets).
 * The payload is a byte ramp so a miscompare points at an offset. proto is
 * IPPROTO_TCP or IPPROTO_UDP (from netinet/in.h). Returns total_len.
 */
static size_t build_ipv4_frame(uint8_t *buf, const uint8_t *dst_mac,
                               uint8_t proto, size_t payload_len,
                               size_t total_len)
{
    static const uint8_t src_mac[6] = { 0x02, 0x00, 0x53, 0x47, 0x49, 0x01 };
    size_t l4_hdr = (proto == IPPROTO_TCP) ? TCP_HDR_LEN : UDP_HDR_LEN;
    size_t l4_total = l4_hdr + payload_len;
    size_t ip_total = IPV4_HDR_LEN + l4_total;
    size_t natural = ETH_HDR_LEN + ip_total;
    uint8_t *ip = buf + ETH_HDR_LEN;
    uint8_t *l4 = ip + IPV4_HDR_LEN;
    uint8_t *payload = l4 + l4_hdr;
    uint8_t pseudo[12];
    uint16_t csum;
    uint32_t sum;
    size_t i;

    g_assert_cmpuint(total_len, >=, natural);
    memset(buf, 0, total_len);

    /* Ethernet II: destination, source, ethertype IPv4 (0x0800). */
    memcpy(buf, dst_mac, 6);
    memcpy(buf + 6, src_mac, 6);
    buf[12] = 0x08;
    buf[13] = 0x00;

    /* IPv4 header (RFC 791), checksum field left zero for the sum below. */
    ip[0] = 0x45;                    /* version 4, IHL 5 (no options) */
    ip[1] = 0x00;                    /* DSCP/ECN */
    ip[2] = (uint8_t)(ip_total >> 8);
    ip[3] = (uint8_t)ip_total;
    ip[4] = 0x12;                    /* identification */
    ip[5] = 0x34;
    ip[6] = 0x40;                    /* flags: don't fragment */
    ip[7] = 0x00;                    /* fragment offset 0 */
    ip[8] = 0x40;                    /* TTL 64 */
    ip[9] = proto;
    ip[10] = 0x00;                   /* header checksum, filled below */
    ip[11] = 0x00;
    memcpy(ip + 12, traffic_src_ip, 4);
    memcpy(ip + 16, traffic_dst_ip, 4);

    /* Payload byte ramp. */
    for (i = 0; i < payload_len; i++) {
        payload[i] = (uint8_t)(0x40 + i);
    }

    /* L4 header, checksum field left zero for the sum below. */
    if (proto == IPPROTO_TCP) {
        l4[0] = 0xc0; l4[1] = 0x00;  /* source port 49152 */
        l4[2] = 0x00; l4[3] = 0x50;  /* destination port 80 */
        l4[4] = 0x00; l4[5] = 0x00; l4[6] = 0x00; l4[7] = 0x01;  /* seq */
        l4[8] = 0x00; l4[9] = 0x00; l4[10] = 0x00; l4[11] = 0x00; /* ack */
        l4[12] = 0x50;               /* data offset 5 (no options) */
        l4[13] = 0x18;               /* flags PSH|ACK */
        l4[14] = 0xff; l4[15] = 0xff; /* window */
        l4[16] = 0x00; l4[17] = 0x00; /* checksum, filled below */
        l4[18] = 0x00; l4[19] = 0x00; /* urgent pointer */
    } else {
        l4[0] = 0xc0; l4[1] = 0x00;  /* source port 49152 */
        l4[2] = 0x00; l4[3] = 0x35;  /* destination port 53 */
        l4[4] = (uint8_t)(l4_total >> 8);
        l4[5] = (uint8_t)l4_total;
        l4[6] = 0x00; l4[7] = 0x00;  /* checksum, filled below */
    }

    /* IPv4 header checksum. */
    csum = frame_csum_fold(frame_csum_add(0, ip, IPV4_HDR_LEN));
    ip[10] = (uint8_t)(csum >> 8);
    ip[11] = (uint8_t)csum;

    /* L4 checksum over the pseudo-header plus the L4 segment. */
    memcpy(pseudo, traffic_src_ip, 4);
    memcpy(pseudo + 4, traffic_dst_ip, 4);
    pseudo[8] = 0x00;
    pseudo[9] = proto;
    pseudo[10] = (uint8_t)(l4_total >> 8);
    pseudo[11] = (uint8_t)l4_total;
    sum = frame_csum_add(0, pseudo, sizeof(pseudo));
    sum = frame_csum_add(sum, l4, l4_total);
    csum = frame_csum_fold(sum);
    /* A UDP checksum result of zero is transmitted as 0xffff (0 means "none");
     * these endpoints never produce zero, but honour the rule. */
    if (proto == IPPROTO_UDP && csum == 0) {
        csum = 0xffff;
    }
    if (proto == IPPROTO_TCP) {
        l4[16] = (uint8_t)(csum >> 8);
        l4[17] = (uint8_t)csum;
    } else {
        l4[6] = (uint8_t)(csum >> 8);
        l4[7] = (uint8_t)csum;
    }

    return total_len;
}

/* A correct IPv4 header checksum folds the header (its own field included) to
 * zero. */
static bool ipv4_checksum_valid(const uint8_t *frame)
{
    return frame_csum_fold(frame_csum_add(0, frame + ETH_HDR_LEN,
                                          IPV4_HDR_LEN)) == 0;
}

/* A correct L4 checksum folds the pseudo-header plus the whole L4 segment (its
 * own field included) to zero. l4_total is the L4 header plus payload length. */
static bool l4_checksum_valid(const uint8_t *frame, uint8_t proto,
                              size_t l4_total)
{
    uint8_t pseudo[12];
    uint32_t sum;

    memcpy(pseudo, traffic_src_ip, 4);
    memcpy(pseudo + 4, traffic_dst_ip, 4);
    pseudo[8] = 0x00;
    pseudo[9] = proto;
    pseudo[10] = (uint8_t)(l4_total >> 8);
    pseudo[11] = (uint8_t)l4_total;
    sum = frame_csum_add(0, pseudo, sizeof(pseudo));
    sum = frame_csum_add(sum, frame + ETH_HDR_LEN + IPV4_HDR_LEN, l4_total);
    return frame_csum_fold(sum) == 0;
}

/*
 * Read exactly n bytes from the stream socket, looping over short reads. A
 * single recv() on a stream socket may return fewer bytes than asked once a
 * frame is jumbo-sized, unlike the small frames net_socket_recv_frame reads in
 * one call.
 */
static void sock_read_full(int fd, uint8_t *buf, size_t n)
{
    size_t off = 0;

    while (off < n) {
        ssize_t r = recv(fd, buf + off, n - off, 0);

        g_assert_cmpint(r, >, 0);
        off += (size_t)r;
    }
}

/* The length-prefixed socket-backend frame read, robust to short reads (see
 * sock_read_full) so it can capture a jumbo frame. */
static size_t traffic_recv_frame(int fd, uint8_t *buf, size_t buf_size)
{
    uint8_t hdr[4];
    size_t len;

    sock_read_full(fd, hdr, sizeof(hdr));
    len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
          ((uint32_t)hdr[2] << 8) | hdr[3];
    g_assert_cmpuint(len, <=, buf_size);
    sock_read_full(fd, buf, len);
    return len;
}

/*
 * Transmit one frame through a single-segment TxD and return what the device
 * emitted onto the backend, asserting the descriptor was released.
 */
static size_t traffic_tx_capture(XframeTx *t, const uint8_t *frame, size_t len,
                                 uint8_t *got, size_t got_size)
{
    size_t emitted;

    xframe_program_txd(t, TRAFFIC_TXD_PAR, TRAFFIC_TX_BUF, frame, len);
    xframe_ring_doorbell(t, TRAFFIC_TXD_PAR);
    emitted = traffic_recv_frame(t->host_fd, got, got_size);
    g_assert_cmphex(dma_ldq_be(t->qs->qts, TRAFFIC_TXD_PAR) & TXD_OWN, ==, 0);
    return emitted;
}

/*
 * A gather list: the frame split across two descriptors, first and last, the
 * shape a guest produces whenever its stack hands down a header and a payload
 * in separate buffers. Both descriptors are owned by the card on entry.
 */
static void xframe_program_txd_pair(XframeTx *t, uint64_t par, uint64_t buf,
                                    const uint8_t *frame, size_t len,
                                    size_t split)
{
    qtest_memwrite(t->qs->qts, buf, frame, split);
    qtest_memwrite(t->qs->qts, buf + 0x1000, frame + split, len - split);

    dma_stq_be(t->qs->qts, par + 0, (uint64_t)split | TXD_OWN | TXD_GCF);
    dma_stq_be(t->qs->qts, par + 8, 0);
    dma_stq_be(t->qs->qts, par + 16, buf);
    dma_stq_be(t->qs->qts, par + 24, 0);

    dma_stq_be(t->qs->qts, par + TXD_SIZE + 0,
               (uint64_t)(len - split) | TXD_OWN | TXD_GCL);
    dma_stq_be(t->qs->qts, par + TXD_SIZE + 8, 0);
    dma_stq_be(t->qs->qts, par + TXD_SIZE + 16, buf + 0x1000);
    dma_stq_be(t->qs->qts, par + TXD_SIZE + 24, 0);
}

/* traffic_tx_capture, with checksum insertion asked for in control2. */
static size_t traffic_tx_capture_cko(XframeTx *t, const uint8_t *frame,
                                     size_t len, uint8_t *got,
                                     size_t got_size, uint64_t cko)
{
    size_t emitted;

    xframe_program_txd(t, TRAFFIC_TXD_PAR, TRAFFIC_TX_BUF, frame, len);
    dma_stq_be(t->qs->qts, TRAFFIC_TXD_PAR + 8, cko);
    xframe_ring_doorbell(t, TRAFFIC_TXD_PAR);
    emitted = traffic_recv_frame(t->host_fd, got, got_size);
    g_assert_cmphex(dma_ldq_be(t->qs->qts, TRAFFIC_TXD_PAR) & TXD_OWN, ==, 0);
    return emitted;
}

/*
 * Post one card-owned RxD sized for a jumbo frame, inject the frame, wait for
 * its completion, and read the deposited bytes back into landed. Asserts the
 * descriptor completed with XCODE OK and the reported length equals len.
 */
static void traffic_rx_capture(XframeTx *t, const uint8_t *frame, size_t len,
                               uint8_t *landed)
{
    xframe_program_rxd(t, TRAFFIC_RX_RING, TRAFFIC_RX_BUF,
                       XFRAME_MAX_FRAME_BYTES);
    xframe_rx_ring_arm(t, TRAFFIC_RX_RING);

    net_socket_send_frame(t->host_fd, frame, len);
    g_assert_true(xframe_wait_rx_traffic(t, 5000));

    g_assert_cmphex(dma_ldq_be(t->qs->qts, TRAFFIC_RX_RING + 8) & RXD_OWN,
                    ==, 0);
    g_assert_cmphex((dma_ldq_be(t->qs->qts, TRAFFIC_RX_RING + 8) >>
                     RXD_XCODE_SHIFT) & RXD_XCODE_MASK, ==, 0);
    g_assert_cmphex(dma_ldq_be(t->qs->qts, TRAFFIC_RX_RING + 16) >>
                    RXD_BUFSIZ_SHIFT, ==, len);

    qtest_memread(t->qs->qts, TRAFFIC_RX_BUF, landed, len);
}

/*
 * Transmit a complete TCP/IPv4 frame and assert every L2/L3/L4 byte the device
 * emits is byte-for-byte what was in the buffer, with the L3 and L4 checksum
 * fields surviving both as the exact bytes built and as valid checksums.
 */
static void test_transmit_tcp_frame_byte_exact(void)
{
    XframeTx t;
    uint8_t frame[256];
    uint8_t got[256];
    size_t len, emitted;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_TCP, 40, 94);

    xframe_tx_start(&t);
    emitted = traffic_tx_capture(&t, frame, len, got, sizeof(got));

    g_assert_cmpint(emitted, ==, (int)len);
    g_assert_cmpmem(got, emitted, frame, len);
    g_assert_cmphex(frame_be16(got, IPV4_CSUM_OFF), ==,
                    frame_be16(frame, IPV4_CSUM_OFF));
    g_assert_cmphex(frame_be16(got, TCP_CSUM_OFF), ==,
                    frame_be16(frame, TCP_CSUM_OFF));
    g_assert_true(ipv4_checksum_valid(got));
    g_assert_true(l4_checksum_valid(got, IPPROTO_TCP, TCP_HDR_LEN + 40));

    xframe_tx_stop(&t);
}

/* The UDP/IPv4 counterpart of the transmit byte-exact test. */
static void test_transmit_udp_frame_byte_exact(void)
{
    XframeTx t;
    uint8_t frame[256];
    uint8_t got[256];
    size_t len, emitted;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_UDP, 40, 82);

    xframe_tx_start(&t);
    emitted = traffic_tx_capture(&t, frame, len, got, sizeof(got));

    g_assert_cmpint(emitted, ==, (int)len);
    g_assert_cmpmem(got, emitted, frame, len);
    g_assert_cmphex(frame_be16(got, IPV4_CSUM_OFF), ==,
                    frame_be16(frame, IPV4_CSUM_OFF));
    g_assert_cmphex(frame_be16(got, UDP_CSUM_OFF), ==,
                    frame_be16(frame, UDP_CSUM_OFF));
    g_assert_true(ipv4_checksum_valid(got));
    g_assert_true(l4_checksum_valid(got, IPPROTO_UDP, UDP_HDR_LEN + 40));

    xframe_tx_stop(&t);
}

/*
 * Receive a complete TCP/IPv4 frame and assert the DMA'd buffer matches the
 * injected frame byte-for-byte, checksum fields surviving verbatim and valid.
 */
static void test_receive_tcp_frame_byte_exact(void)
{
    XframeTx t;
    uint8_t frame[256];
    uint8_t landed[256];
    size_t len;

    len = build_ipv4_frame(frame, xframe_station_mac, IPPROTO_TCP, 40, 94);

    xframe_tx_start(&t);
    traffic_rx_capture(&t, frame, len, landed);

    g_assert_cmpmem(landed, len, frame, len);
    g_assert_cmphex(frame_be16(landed, IPV4_CSUM_OFF), ==,
                    frame_be16(frame, IPV4_CSUM_OFF));
    g_assert_cmphex(frame_be16(landed, TCP_CSUM_OFF), ==,
                    frame_be16(frame, TCP_CSUM_OFF));
    g_assert_true(ipv4_checksum_valid(landed));
    g_assert_true(l4_checksum_valid(landed, IPPROTO_TCP, TCP_HDR_LEN + 40));

    xframe_tx_stop(&t);
}

/* The UDP/IPv4 counterpart of the receive byte-exact test. */
static void test_receive_udp_frame_byte_exact(void)
{
    XframeTx t;
    uint8_t frame[256];
    uint8_t landed[256];
    size_t len;

    len = build_ipv4_frame(frame, xframe_station_mac, IPPROTO_UDP, 40, 82);

    xframe_tx_start(&t);
    traffic_rx_capture(&t, frame, len, landed);

    g_assert_cmpmem(landed, len, frame, len);
    g_assert_cmphex(frame_be16(landed, IPV4_CSUM_OFF), ==,
                    frame_be16(frame, IPV4_CSUM_OFF));
    g_assert_cmphex(frame_be16(landed, UDP_CSUM_OFF), ==,
                    frame_be16(frame, UDP_CSUM_OFF));
    g_assert_true(ipv4_checksum_valid(landed));
    g_assert_true(l4_checksum_valid(landed, IPPROTO_UDP, UDP_HDR_LEN + 40));

    xframe_tx_stop(&t);
}

/*
 * Checksum pass-through on transmit: the device has no offload, so a
 * deliberately WRONG L3 and L4 checksum must egress unchanged. A device that
 * recomputed or "corrected" them -- the IOC3 model's bug -- would overwrite
 * these sentinels; their verbatim survival proves xframe never touches them.
 */
static void test_transmit_preserves_checksums_verbatim(void)
{
    XframeTx t;
    uint8_t frame[128];
    uint8_t got[128];
    size_t len, emitted;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_TCP, 20, 74);
    /* Corrupt both checksum fields to sentinels no correct checksum equals. */
    frame[IPV4_CSUM_OFF] = 0xde; frame[IPV4_CSUM_OFF + 1] = 0xad;
    frame[TCP_CSUM_OFF] = 0xbe; frame[TCP_CSUM_OFF + 1] = 0xef;

    xframe_tx_start(&t);
    emitted = traffic_tx_capture(&t, frame, len, got, sizeof(got));

    g_assert_cmpint(emitted, ==, (int)len);
    g_assert_cmpmem(got, emitted, frame, len);
    g_assert_cmphex(frame_be16(got, IPV4_CSUM_OFF), ==, 0xdead);
    g_assert_cmphex(frame_be16(got, TCP_CSUM_OFF), ==, 0xbeef);

    xframe_tx_stop(&t);
}

/*
 * Checksum insertion when the descriptor asks for it. A driver using offload
 * hands the card a frame whose checksum fields are still placeholders and sets
 * the request in control2; the card owes it the finished checksums. IRIX's xg
 * driver transmits every TCP and UDP segment this way, and a placeholder left
 * on the wire is dropped by the receiver with no diagnostic anywhere.
 */
static void test_transmit_inserts_requested_tcp_checksum(void)
{
    XframeTx t;
    uint8_t frame[128];
    uint8_t got[128];
    size_t len, emitted;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_TCP, 20, 74);
    frame[IPV4_CSUM_OFF] = 0xde; frame[IPV4_CSUM_OFF + 1] = 0xad;
    frame[TCP_CSUM_OFF] = 0xbe; frame[TCP_CSUM_OFF + 1] = 0xef;

    xframe_tx_start(&t);
    emitted = traffic_tx_capture_cko(&t, frame, len, got, sizeof(got),
                                     TXD_CKO_IPV4 | TXD_CKO_TCP);

    g_assert_cmpint(emitted, ==, (int)len);
    g_assert_cmphex(frame_be16(got, IPV4_CSUM_OFF), !=, 0xdead);
    g_assert_cmphex(frame_be16(got, TCP_CSUM_OFF), !=, 0xbeef);
    g_assert_true(ipv4_checksum_valid(got));
    g_assert_true(l4_checksum_valid(got, IPPROTO_TCP, TCP_HDR_LEN + 20));

    xframe_tx_stop(&t);
}

/*
 * What the card does with the masks AS RESET, which is the question the
 * completion test above cannot ask because it opens them itself. A driver that
 * never writes TX_TRAFFIC_MASK gets whatever this says: the reset value is
 * all-ones (masked), so the transmit-completion interrupt is gated off until
 * the guest asks for it. Whether that is right depends on the guest, but the
 * model's behaviour should be pinned rather than assumed -- a restrictive
 * reset value turns a missing guest write into silence, and silence is what a
 * driver waiting to reclaim descriptors never recovers from.
 */
static void test_transmit_completion_with_masks_as_reset(void)
{
    XframeTx t;
    uint8_t frame[64];
    uint8_t got[128];
    uint64_t par = TX_RAM_SCRATCH;
    uint64_t buf = TX_RAM_SCRATCH + 0x1000;
    size_t len = sizeof(frame);
    unsigned i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 1);
    }
    memset(frame, 0xff, 6);

    xframe_tx_start(&t);

    /* Deliberately touch NEITHER mask: this is the reset state a driver meets. */
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_program_txd(&t, par, buf, frame, len);
    xframe_ring_doorbell(&t, par);
    (void)traffic_recv_frame(t.host_fd, got, sizeof(got));

    /*
     * The frame goes out either way -- egress is not gated on the masks. What
     * is pinned here is the INTERRUPT: with the masks as reset, none is
     * delivered, so a driver that reclaims on completion is never told.
     */
    g_assert_cmphex(dma_ldq_be(t.qs->qts, par) & TXD_OWN, ==, 0);
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_tx_stop(&t);
}

/*
 * The largest receive payload the MAC accepts. A guest raises this to run
 * jumbo, so the card has to keep what it is told and read it back -- a value
 * the model drops leaves the driver unable to confirm the size it asked for.
 * Reset presents the 1500-byte default (Linux s2io RMAC_MAX_PYLD_LEN_DEF).
 */
#define RMAC_MAX_PYLD_LEN_OFF   0x2110
#define MAX_PYLD_SHIFT          48

static void test_rmac_max_payload_defaults_and_holds(void)
{
    XframeTx t;
    uint64_t v;

    xframe_tx_start(&t);

    v = sock_pif_readq(&t, RMAC_MAX_PYLD_LEN_OFF);
    g_assert_cmpint((v >> MAX_PYLD_SHIFT) & 0x3fff, ==, 1500);

    sock_pif_writeq(&t, RMAC_MAX_PYLD_LEN_OFF,
                    (uint64_t)9000 << MAX_PYLD_SHIFT);
    v = sock_pif_readq(&t, RMAC_MAX_PYLD_LEN_OFF);
    g_assert_cmpint((v >> MAX_PYLD_SHIFT) & 0x3fff, ==, 9000);

    xframe_tx_stop(&t);
}

/*
 * A gather list must come back to the host in full. The card consumes every
 * descriptor it walks, so it owes ownership on every one of them -- returning
 * only the first leaves the rest permanently owned by the card, and a driver
 * that cannot reclaim them runs its transmit ring down to nothing and stops
 * sending. IRIX's xg driver does exactly that after about a dozen frames.
 */
static void test_transmit_gather_list_releases_every_descriptor(void)
{
    XframeTx t;
    uint8_t frame[256];
    uint8_t got[256];
    size_t len, emitted;
    uint64_t d0, d1;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_TCP, 40, 94);

    xframe_tx_start(&t);
    xframe_program_txd_pair(&t, TRAFFIC_TXD_PAR, TRAFFIC_TX_BUF, frame, len, 32);
    qpci_io_writeq(t.dev, t.txp, TXP_TXDL_PAR, TRAFFIC_TXD_PAR);
    qpci_io_writeq(t.dev, t.txp, TXP_TXDL_LCR,
                   TXDL_NUMTXD(1) | TXDL_LGC_FIRST | TXDL_LGC_LAST);
    emitted = traffic_recv_frame(t.host_fd, got, sizeof(got));

    /* The frame is the two buffers joined, in order. */
    g_assert_cmpint(emitted, ==, (int)len);
    g_assert_cmpmem(got, emitted, frame, len);

    /* Both descriptors are back with the host. */
    d0 = dma_ldq_be(t.qs->qts, TRAFFIC_TXD_PAR);
    d1 = dma_ldq_be(t.qs->qts, TRAFFIC_TXD_PAR + TXD_SIZE);
    g_assert_cmphex(d0 & TXD_OWN, ==, 0);
    g_assert_cmphex(d1 & TXD_OWN, ==, 0);

    xframe_tx_stop(&t);
}

/*
 * Segmentation offload. IRIX turns this on for the adapter -- xg1's interface
 * flags carry L2TCPSEG -- and then hands the card ONE buffer holding several
 * maximum segments' worth of payload, with the descriptor's LSO enable set and
 * the segment size it wants in the descriptor's MSS field. The card owes it one
 * frame per segment, each with its own headers.
 *
 * Sending the buffer whole instead puts a single oversized frame on the wire
 * carrying a TCP segment no receiver will accept: measured from inside IRIX,
 * 2920-byte segments (two maximum segments under one header) went out with
 * checksums covering a length the peer disagreed with, were discarded, and were
 * followed by correctly sized retransmissions of the same sequence number. A
 * bulk transfer collapses into retransmission timeouts and crawls to a stop,
 * which is what "the adapter stalls after about a dozen frames" actually was.
 *
 * Drive a four-segment send and require the wire to carry four properly formed
 * frames: right sizes, headers rebuilt per segment, sequence numbers advancing
 * by the segment size, and the payloads reassembling into what was handed over.
 */
static void test_transmit_segments_a_large_send(void)
{
    XframeTx t;
    uint8_t frame[ETH_HDR_LEN + IPV4_HDR_LEN + TCP_HDR_LEN + 1736];
    uint8_t got[2048];
    uint8_t rebuilt[1736];
    const size_t mss = 512;
    const size_t payload = 1736;              /* 512 + 512 + 512 + 200 */
    const size_t expect[4] = { 512, 512, 512, 200 };
    const uint8_t *sent_payload;
    const uint8_t *sent_tcp;
    uint32_t base_seq;
    size_t len, off, i;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_TCP, payload,
                           sizeof(frame));
    sent_tcp = frame + ETH_HDR_LEN + IPV4_HDR_LEN;
    sent_payload = sent_tcp + TCP_HDR_LEN;
    base_seq = ((uint32_t)sent_tcp[4] << 24) | ((uint32_t)sent_tcp[5] << 16) |
               ((uint32_t)sent_tcp[6] << 8) | sent_tcp[7];

    xframe_tx_start(&t);

    qtest_memwrite(t.qs->qts, TRAFFIC_TX_BUF, frame, len);
    dma_stq_be(t.qs->qts, TRAFFIC_TXD_PAR + 0,
               (uint64_t)len | TXD_OWN | TXD_GCF | TXD_GCL |
               TXD_LSO_EN | TXD_LSO_MSS(mss));
    dma_stq_be(t.qs->qts, TRAFFIC_TXD_PAR + 8, 0);
    dma_stq_be(t.qs->qts, TRAFFIC_TXD_PAR + 16, TRAFFIC_TX_BUF);
    dma_stq_be(t.qs->qts, TRAFFIC_TXD_PAR + 24, 0);
    qpci_io_writeq(t.dev, t.txp, TXP_TXDL_PAR, TRAFFIC_TXD_PAR);
    qpci_io_writeq(t.dev, t.txp, TXP_TXDL_LCR,
                   TXDL_NUMTXD(0) | TXDL_LGC_FIRST | TXDL_LGC_LAST);

    off = 0;
    for (i = 0; i < 4; i++) {
        size_t seg = expect[i];
        size_t want = ETH_HDR_LEN + IPV4_HDR_LEN + TCP_HDR_LEN + seg;
        const uint8_t *ip = got + ETH_HDR_LEN;
        const uint8_t *tcp = ip + IPV4_HDR_LEN;
        uint32_t seq;

        len = traffic_recv_frame(t.host_fd, got, sizeof(got));

        /* One frame per segment, none of them oversized. */
        g_assert_cmpuint(len, ==, want);

        /* The IPv4 header describes THIS segment, not the whole send. */
        g_assert_cmpuint((ip[2] << 8) | ip[3], ==,
                         IPV4_HDR_LEN + TCP_HDR_LEN + seg);

        /* Sequence numbers advance from the sender's by what has gone out. */
        seq = ((uint32_t)tcp[4] << 24) | ((uint32_t)tcp[5] << 16) |
              ((uint32_t)tcp[6] << 8) | tcp[7];
        g_assert_cmpuint(seq, ==, base_seq + (uint32_t)off);

        memcpy(rebuilt + off, got + want - seg, seg);
        off += seg;
    }

    /* Nothing added, dropped or reordered on the way out. */
    g_assert_cmpuint(off, ==, payload);
    g_assert_cmpmem(rebuilt, payload, sent_payload, payload);

    xframe_tx_stop(&t);
}

/*
 * A segmentation-offload buffer is not a frame, and must not be clamped like
 * one. It is a whole IP datagram waiting to be cut up, so it is routinely
 * larger than anything the wire can carry: IRIX hands this adapter about 32 KB
 * at a time, well past the card's 9600-byte jumbo limit. Clamping it to that
 * limit truncates the datagram, and the truncated remnant then fails the check
 * that its IP total length fits inside it -- so segmentation declines and a
 * malformed 9600-byte frame goes out whole. Measured on the wire before this
 * was fixed: four such frames, each claiming an IP total length of about 32 KB,
 * and each one of the four bad checksums in the run.
 *
 * Drive a datagram larger than the frame limit and require every byte of it to
 * come out in properly sized frames.
 */
static void test_transmit_segments_a_send_larger_than_a_frame(void)
{
    XframeTx t;
    const size_t mss = 1460;
    const size_t payload = 20000;         /* well past XFRAME_MAX_FRAME */
    const size_t hdr = ETH_HDR_LEN + IPV4_HDR_LEN + TCP_HDR_LEN;
    g_autofree uint8_t *frame = g_malloc0(hdr + payload);
    /* Large enough to hold an unsegmented buffer, so an oversized frame is
     * caught by this test's own size assertion rather than by the receive
     * helper running out of room. */
    uint8_t got[XFRAME_MAX_FRAME_FOR_TEST];
    size_t len, seen = 0, frames = 0;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_TCP, payload,
                           hdr + payload);

    xframe_tx_start(&t);

    qtest_memwrite(t.qs->qts, TRAFFIC_TX_BUF, frame, len);
    dma_stq_be(t.qs->qts, TRAFFIC_TXD_PAR + 0,
               (uint64_t)len | TXD_OWN | TXD_GCF | TXD_GCL |
               TXD_LSO_EN | TXD_LSO_MSS(mss));
    dma_stq_be(t.qs->qts, TRAFFIC_TXD_PAR + 8, 0);
    dma_stq_be(t.qs->qts, TRAFFIC_TXD_PAR + 16, TRAFFIC_TX_BUF);
    dma_stq_be(t.qs->qts, TRAFFIC_TXD_PAR + 24, 0);
    qpci_io_writeq(t.dev, t.txp, TXP_TXDL_PAR, TRAFFIC_TXD_PAR);
    qpci_io_writeq(t.dev, t.txp, TXP_TXDL_LCR,
                   TXDL_NUMTXD(0) | TXDL_LGC_FIRST | TXDL_LGC_LAST);

    while (seen < payload) {
        size_t got_len = traffic_recv_frame(t.host_fd, got, sizeof(got));

        /* Nothing oversized: every frame is headers plus at most one segment. */
        g_assert_cmpuint(got_len, >, hdr);
        g_assert_cmpuint(got_len, <=, hdr + mss);
        seen += got_len - hdr;
        frames++;
    }

    /* The whole datagram came out, and in the number of frames it needs. */
    g_assert_cmpuint(seen, ==, payload);
    g_assert_cmpuint(frames, ==, (payload + mss - 1) / mss);

    xframe_tx_stop(&t);
}

/*
 * The card reports what it made of a received frame in the first control word:
 * how it classified it, and whether its checksums verified. A driver that
 * trusts that skips checking in software, which is real work saved on a machine
 * whose CPU is emulated and whose card is not.
 *
 * Leaving those halves zero is not "unimplemented" -- zero is the value that
 * says the checksums did NOT verify. So both directions are asserted: a good
 * frame reports verified, and a frame whose transport checksum has been
 * corrupted reports layer 4 not verified while still classifying the frame and
 * still vouching for the intact IP header.
 */
static void test_receive_reports_checksum_results(void)
{
    XframeTx t;
    uint8_t frame[128];
    size_t len;
    uint64_t c1;

    len = build_ipv4_frame(frame, xframe_station_mac, IPPROTO_TCP, 40, 94);

    xframe_tx_start(&t);
    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF0, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);
    net_socket_send_frame(t.host_fd, frame, len);
    g_assert_true(xframe_wait_rx_traffic(&t, 5000));

    c1 = dma_ldq_be(t.qs->qts, RX_RING_BASE + 8);
    g_assert_cmphex(c1 & RXD_PROTO_IPV4, ==, RXD_PROTO_IPV4);
    g_assert_cmphex(c1 & RXD_PROTO_TCP, ==, RXD_PROTO_TCP);
    g_assert_cmphex((c1 >> 16) & 0xffff, ==, RXD_CKSUM_OK);
    g_assert_cmphex(c1 & 0xffff, ==, RXD_CKSUM_OK);

    /* Corrupt the transport checksum; the IP header is left intact. */
    frame[ETH_HDR_LEN + IPV4_HDR_LEN + 16] ^= 0xff;

    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF0, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);
    net_socket_send_frame(t.host_fd, frame, len);
    g_assert_true(xframe_wait_rx_traffic(&t, 5000));

    c1 = dma_ldq_be(t.qs->qts, RX_RING_BASE + 8);
    g_assert_cmphex(c1 & RXD_PROTO_TCP, ==, RXD_PROTO_TCP);
    g_assert_cmphex((c1 >> 16) & 0xffff, ==, RXD_CKSUM_OK);
    g_assert_cmphex(c1 & 0xffff, !=, RXD_CKSUM_OK);

    xframe_tx_stop(&t);
}

/*
 * Jumbo receive. IRIX raises the MTU and immediately re-arms the receive ring
 * in mode 2: three buffers per descriptor, header split across them, and
 * descriptors 64 bytes rather than 32. A model that knows only one-buffer mode
 * keeps walking at the 32-byte stride, reads the wrong words, never finds an
 * owned descriptor again, and receive stops for good -- an interface that still
 * reads UP and RUNNING while silently receiving nothing, which is exactly what
 * this adapter did at MTU 9000 before the mode was implemented.
 *
 * Post one three-buffer descriptor with the sizes the guest really uses
 * (14 for the Ethernet header, 128 for the upper headers, the rest for
 * payload), deliver a frame bigger than the first two buffers put together, and
 * require it split across all three in order with each buffer's own count
 * reported back.
 */
static void test_receive_five_buffer_unclassified_frame(void)
{
    XframeTx t;
    uint8_t frame[600];
    uint8_t landed[600];
    const uint64_t b0 = RX_BUF0;
    const uint64_t b1 = RX_BUF0 + 0x1000;
    const uint64_t b2 = RX_BUF0 + 0x2000;
    const size_t c0 = 14, c1 = 128, c2 = 4096;  /* as IRIX posts them */
    uint64_t ctrl2;
    unsigned i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 3);
    }
    memcpy(frame, xframe_station_mac, sizeof(xframe_station_mac));

    xframe_tx_start(&t);

    /* A five-buffer descriptor armed the minimal way: three sizes, three
     * pointers, overflow buffers unposted (zero). */
    dma_stq_be(t.qs->qts, RX_RING_BASE + 0, 0);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 8, RXD_OWN);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 16, RXD5_SET_SIZES(c0, c1, c2));
    dma_stq_be(t.qs->qts, RX_RING_BASE + 24, b0);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 32, b1);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 40, b2);

    sock_pif_writeq(&t, PRC_RXD0_0, RX_RING_BASE);
    sock_pif_writeq(&t, PRC_CTRL_0, RC_IN_SVC | RC_RING_MODE_5BUF);

    net_socket_send_frame(t.host_fd, frame, sizeof(frame));
    g_assert_true(xframe_wait_rx_traffic(&t, 5000));

    /* The card handed the descriptor back. */
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) & RXD_OWN, ==, 0);

    /*
     * This frame is NOT IPv4/TCP -- it has no transport headers to split -- so
     * the card puts the Ethernet header in buffer 0 and the whole remainder in
     * buffer 2, leaving buffer 1 empty. A classified frame is split differently;
     * that is the next test.
     */
    ctrl2 = dma_ldq_be(t.qs->qts, RX_RING_BASE + 16);
    g_assert_cmpuint(RXD5_GET_B0(ctrl2), ==, c0);
    g_assert_cmpuint(RXD5_GET_B1(ctrl2), ==, 0);
    g_assert_cmpuint(RXD5_GET_B2(ctrl2), ==, sizeof(frame) - c0);

    qtest_memread(t.qs->qts, b0, landed, c0);
    g_assert_cmpmem(landed, c0, frame, c0);
    qtest_memread(t.qs->qts, b2, landed, sizeof(frame) - c0);
    g_assert_cmpmem(landed, sizeof(frame) - c0, frame + c0, sizeof(frame) - c0);

    xframe_tx_stop(&t);
}

/*
 * The header split proper: a classified IPv4/TCP frame. The card puts the
 * Ethernet header in buffer 0, the IP and transport headers in buffer 1, and
 * the payload in buffer 2 -- the layout IRIX's checksum-offload path depends on,
 * reading the transport headers from buffer 1. An earlier version of this model
 * left buffer 1 empty (the Linux two-buffer dummy) and put the whole remainder
 * in buffer 2, which reassembled fine but made the offload path find no headers
 * in buffer 1 and drop every segment. So this asserts the headers ARE in
 * buffer 1, the payload IS in buffer 2, and the offload status is reported.
 */
static void test_receive_five_buffer_splits_classified_headers(void)
{
    XframeTx t;
    uint8_t frame[512];
    uint8_t landed[512];
    const uint64_t b0 = RX_BUF0;
    const uint64_t b1 = RX_BUF0 + 0x1000;
    const uint64_t b2 = RX_BUF0 + 0x2000;
    const size_t c0 = 14, c1 = 128, c2 = 4096;   /* as IRIX posts them */
    const size_t l34 = IPV4_HDR_LEN + TCP_HDR_LEN;   /* what belongs in buffer 1 */
    const size_t payload = 200;
    uint64_t ctrl1, ctrl2;
    size_t len;

    len = build_ipv4_frame(frame, xframe_station_mac, IPPROTO_TCP, payload,
                           sizeof(frame));

    xframe_tx_start(&t);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 0, 0);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 8, RXD_OWN);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 16, RXD5_SET_SIZES(c0, c1, c2));
    dma_stq_be(t.qs->qts, RX_RING_BASE + 24, b0);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 32, b1);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 40, b2);
    sock_pif_writeq(&t, PRC_RXD0_0, RX_RING_BASE);
    sock_pif_writeq(&t, PRC_CTRL_0, RC_IN_SVC | RC_RING_MODE_5BUF);

    net_socket_send_frame(t.host_fd, frame, len);
    g_assert_true(xframe_wait_rx_traffic(&t, 5000));
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) & RXD_OWN, ==, 0);

    /* L2 -> buffer 0, L3+L4 -> buffer 1, payload -> buffer 2. */
    ctrl2 = dma_ldq_be(t.qs->qts, RX_RING_BASE + 16);
    g_assert_cmpuint(RXD5_GET_B0(ctrl2), ==, c0);
    g_assert_cmpuint(RXD5_GET_B1(ctrl2), ==, l34);
    g_assert_cmpuint(RXD5_GET_B2(ctrl2), ==, len - c0 - l34);

    qtest_memread(t.qs->qts, b0, landed, c0);
    g_assert_cmpmem(landed, c0, frame, c0);
    qtest_memread(t.qs->qts, b1, landed, l34);
    g_assert_cmpmem(landed, l34, frame + c0, l34);
    qtest_memread(t.qs->qts, b2, landed, len - c0 - l34);
    g_assert_cmpmem(landed, len - c0 - l34, frame + c0 + l34, len - c0 - l34);

    /* The classification and checksum results are reported (offload is on in
     * this mode, unlike the interim gate that disabled it). */
    ctrl1 = dma_ldq_be(t.qs->qts, RX_RING_BASE + 8);
    g_assert_cmphex(ctrl1 & RXD_PROTO_TCP, ==, RXD_PROTO_TCP);
    g_assert_cmphex((ctrl1 >> 16) & 0xffff, ==, RXD_CKSUM_OK);   /* IP header */
    g_assert_cmphex(ctrl1 & 0xffff, ==, RXD_CKSUM_OK);           /* TCP */

    xframe_tx_stop(&t);
}

/*
 * The same split at jumbo size: a 9014-byte TCP frame, the exact shape the peer
 * sends during a bulk transfer at MTU 9000 and the first frame size that
 * genuinely needs buffer 2's room (the handshake fits buffers 0 and 1). This is
 * the case the guest failed on live -- data frames retransmitted forever, never
 * acknowledged -- while the 274-byte classified test above passed, so the small
 * test alone is not sufficient coverage.
 */
static void test_receive_five_buffer_splits_a_jumbo_frame(void)
{
    XframeTx t;
    g_autofree uint8_t *frame = g_malloc0(9014);
    g_autofree uint8_t *landed = g_malloc0(9014);
    const uint64_t b0 = RX_BUF0;
    const uint64_t b1 = RX_BUF0 + 0x1000;
    const uint64_t b2 = RX_BUF0 + 0x2000;
    const size_t c0 = 14, c1 = 128, c2 = 9738;   /* as IRIX posts them */
    const size_t l34 = IPV4_HDR_LEN + TCP_HDR_LEN;
    const size_t payload = 9014 - ETH_HDR_LEN - IPV4_HDR_LEN - TCP_HDR_LEN;
    uint64_t ctrl1, ctrl2;
    size_t len;

    len = build_ipv4_frame(frame, xframe_station_mac, IPPROTO_TCP, payload,
                           9014);
    g_assert_cmpuint(len, ==, 9014);

    xframe_tx_start(&t);
    /*
     * Post the descriptor the way IRIX actually does: five buffers, with the
     * two 4-byte overflow buffers' sizes in the low halves of the first word
     * under the driver's own bookkeeping value in the upper half.
     */
    dma_stq_be(t.qs->qts, RX_RING_BASE + 0,
               (0x1234ULL << 32) | (4 << 16) | 4);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 8, RXD_OWN);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 16, RXD5_SET_SIZES(c0, c1, c2));
    dma_stq_be(t.qs->qts, RX_RING_BASE + 24, b0);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 32, b1);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 40, b2);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 48, b2 + 0x4000);
    dma_stq_be(t.qs->qts, RX_RING_BASE + 56, b2 + 0x4008);
    sock_pif_writeq(&t, PRC_RXD0_0, RX_RING_BASE);
    sock_pif_writeq(&t, PRC_CTRL_0, RC_IN_SVC | RC_RING_MODE_5BUF);

    net_socket_send_frame(t.host_fd, frame, len);
    g_assert_true(xframe_wait_rx_traffic(&t, 5000));
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) & RXD_OWN, ==, 0);

    ctrl2 = dma_ldq_be(t.qs->qts, RX_RING_BASE + 16);
    g_assert_cmpuint(RXD5_GET_B0(ctrl2), ==, c0);
    g_assert_cmpuint(RXD5_GET_B1(ctrl2), ==, l34);
    g_assert_cmpuint(RXD5_GET_B2(ctrl2), ==, len - c0 - l34);

    /*
     * The completion reports buffers 3 and 4 as EMPTY -- their count fields in
     * the first word zeroed, the driver's bookkeeping above them preserved. A
     * model that leaves the posted sizes in place hands the driver a frame sum
     * 8 bytes long, and IRIX rejects every such frame as an input error:
     * receive dead at jumbo MTU with each byte of the frame itself delivered
     * perfectly, which is exactly how this shipped broken once already.
     */
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 0), ==,
                    0x1234ULL << 32);

    qtest_memread(t.qs->qts, b0, landed, c0);
    g_assert_cmpmem(landed, c0, frame, c0);
    qtest_memread(t.qs->qts, b1, landed, l34);
    g_assert_cmpmem(landed, l34, frame + c0, l34);
    qtest_memread(t.qs->qts, b2, landed, len - c0 - l34);
    g_assert_cmpmem(landed, len - c0 - l34, frame + c0 + l34, len - c0 - l34);

    ctrl1 = dma_ldq_be(t.qs->qts, RX_RING_BASE + 8);
    g_assert_cmphex(ctrl1 & RXD_PROTO_TCP, ==, RXD_PROTO_TCP);
    g_assert_cmphex(ctrl1 & 0xffff, ==, RXD_CKSUM_OK);

    xframe_tx_stop(&t);
}

/*
 * The statistics block. The driver keeps its interface counters as deltas
 * against successive DMA'd snapshots of this block, so the block must exist,
 * must advance with traffic, and must never go backwards: unmodelled, the
 * guest's transmit packet count ran BACKWARDS (netstat -in showing Opkts of
 * 4294967284 and falling), because the driver differenced against memory that
 * never moved.
 */
static void test_statistics_block_counts_traffic(void)
{
    XframeTx t;
    uint8_t frame[128];
    uint8_t got[256];
    size_t len;
    uint32_t frms0, frms, octets;
    unsigned i;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_UDP, 64, 106);

    xframe_tx_start(&t);

    /* Enable: the first snapshot lands immediately. */
    sock_pif_writeq(&t, STAT_ADDR, STAT_BLOCK_ADDR);
    sock_pif_writeq(&t, STAT_CFG, STAT_CFG_EN | 0x208d5);
    frms0 = dma_ldl_be(t.qs->qts, STAT_BLOCK_ADDR + STAT_TMAC_FRMS);
    g_assert_cmpuint(frms0, ==, 0);

    /* Transmit three frames and let the periodic refresh publish them. */
    for (i = 0; i < 3; i++) {
        xframe_program_txd(&t, TX_RAM_SCRATCH, TX_RAM_SCRATCH + 0x1000,
                           frame, len);
        xframe_ring_doorbell(&t, TX_RAM_SCRATCH);
        (void)traffic_recv_frame(t.host_fd, got, sizeof(got));
    }
    qtest_clock_step(t.qs->qts, 2LL * 1000 * 1000 * 1000);

    frms = dma_ldl_be(t.qs->qts, STAT_BLOCK_ADDR + STAT_TMAC_FRMS);
    octets = dma_ldl_be(t.qs->qts, STAT_BLOCK_ADDR + STAT_TMAC_DATA_OCTETS);
    g_assert_cmpuint(frms, ==, 3);
    g_assert_cmpuint(octets, ==, 3 * len);

    /* More traffic only moves it forward. */
    xframe_program_txd(&t, TX_RAM_SCRATCH, TX_RAM_SCRATCH + 0x1000, frame, len);
    xframe_ring_doorbell(&t, TX_RAM_SCRATCH);
    (void)traffic_recv_frame(t.host_fd, got, sizeof(got));
    qtest_clock_step(t.qs->qts, 2LL * 1000 * 1000 * 1000);
    g_assert_cmpuint(dma_ldl_be(t.qs->qts,
                                STAT_BLOCK_ADDR + STAT_TMAC_FRMS), ==, 4);

    /*
     * The block's byte order follows the swapper, big-endian at reset: a
     * whole-32-bit read decodes to the true count, and a small value keeps its
     * three high-order bytes zero. Reading it any other way byte-reverses it.
     */
    g_assert_cmpuint(dma_ldl_be(t.qs->qts,
                                STAT_BLOCK_ADDR + STAT_TMAC_FRMS) >> 8, ==, 0);

    xframe_tx_stop(&t);
}

/*
 * The DMA covers exactly the hardware span [0, 0x320) and nothing past it. Fill
 * the block area and one guard byte with 0xff, drive traffic so the four
 * counters are non-zero, and require the whole span to come back zero apart
 * from those four words while the byte at 0x320 stays 0xff. This fails red if
 * the write is ever shortened again (the overflow offsets and size were once 16
 * bytes early, leaving the block's tail unwritten) or if it runs long.
 */
static void test_statistics_block_write_span(void)
{
    XframeTx t;
    uint8_t frame[128];
    uint8_t got[256];
    uint8_t block[STAT_BLOCK_SIZE + 1];
    size_t len, i;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_UDP, 64, 106);

    xframe_tx_start(&t);

    qtest_memset(t.qs->qts, STAT_BLOCK_ADDR, 0xff, sizeof(block));

    sock_pif_writeq(&t, STAT_ADDR, STAT_BLOCK_ADDR);
    for (i = 0; i < 3; i++) {
        xframe_program_txd(&t, TX_RAM_SCRATCH, TX_RAM_SCRATCH + 0x1000,
                           frame, len);
        xframe_ring_doorbell(&t, TX_RAM_SCRATCH);
        (void)traffic_recv_frame(t.host_fd, got, sizeof(got));
    }
    sock_pif_writeq(&t, STAT_CFG, STAT_CFG_EN | 0x208d5);
    qtest_clock_step(t.qs->qts, 2LL * 1000 * 1000 * 1000);

    qtest_memread(t.qs->qts, STAT_BLOCK_ADDR, block, sizeof(block));

    /* The guard byte at 0x320 is untouched. */
    g_assert_cmphex(block[STAT_BLOCK_SIZE], ==, 0xff);

    /* Everything in the span is zero except the four counter words. */
    for (i = 0; i < STAT_BLOCK_SIZE; i++) {
        bool counter = (i < STAT_TMAC_FRMS + 4) ||
                       (i >= STAT_RMAC_DATA_OCTETS && i < STAT_RMAC_VLD_FRMS + 4);
        if (!counter) {
            g_assert_cmphex(block[i], ==, 0);
        }
    }

    xframe_tx_stop(&t);
}

/*
 * A one-shot transfer clears the enable bit when its single DMA completes --
 * the driver's one-shot path polls exactly that and gives up as "Updt failed"
 * if enable stays set. A periodic enable leaves the bit set.
 */
static void test_statistics_one_shot_clears_enable(void)
{
    XframeTx t;

    xframe_tx_start(&t);
    sock_pif_writeq(&t, STAT_ADDR, STAT_BLOCK_ADDR);

    sock_pif_writeq(&t, STAT_CFG, STAT_CFG_ONE_SHOT | STAT_CFG_EN | 0xa);
    g_assert_cmphex(sock_pif_readq(&t, STAT_CFG) & STAT_CFG_EN, ==, 0);

    sock_pif_writeq(&t, STAT_CFG, STAT_CFG_EN | 0x208d5);
    g_assert_cmphex(sock_pif_readq(&t, STAT_CFG) & STAT_CFG_EN, ==, STAT_CFG_EN);

    xframe_tx_stop(&t);
}

/*
 * A descriptor can ask for insertion on a frame that is not IPv4 at all. The
 * lengths a checksum walk would follow are the guest's own bytes, so the
 * request has to be declined and the frame sent as it stands.
 */
static void test_transmit_checksum_request_on_non_ipv4_declined(void)
{
    XframeTx t;
    uint8_t frame[128];
    uint8_t got[128];
    size_t len = 64, emitted;

    memset(frame, 0xa5, sizeof(frame));
    memcpy(frame, traffic_remote_mac, 6);
    frame[12] = 0x08; frame[13] = 0x06;   /* ARP, not IPv4 */

    xframe_tx_start(&t);
    emitted = traffic_tx_capture_cko(&t, frame, len, got, sizeof(got),
                                     TXD_CKO_IPV4 | TXD_CKO_TCP);

    g_assert_cmpint(emitted, ==, (int)len);
    g_assert_cmpmem(got, emitted, frame, len);

    xframe_tx_stop(&t);
}

/* The UDP counterpart of the checksum-insertion test. */
static void test_transmit_inserts_requested_udp_checksum(void)
{
    XframeTx t;
    uint8_t frame[128];
    uint8_t got[128];
    size_t len, emitted;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_UDP, 20, 62);
    frame[IPV4_CSUM_OFF] = 0xde; frame[IPV4_CSUM_OFF + 1] = 0xad;
    frame[UDP_CSUM_OFF] = 0xbe; frame[UDP_CSUM_OFF + 1] = 0xef;

    xframe_tx_start(&t);
    emitted = traffic_tx_capture_cko(&t, frame, len, got, sizeof(got),
                                     TXD_CKO_IPV4 | TXD_CKO_UDP);

    g_assert_cmpint(emitted, ==, (int)len);
    g_assert_cmphex(frame_be16(got, IPV4_CSUM_OFF), !=, 0xdead);
    g_assert_cmphex(frame_be16(got, UDP_CSUM_OFF), !=, 0xbeef);
    g_assert_true(ipv4_checksum_valid(got));
    g_assert_true(l4_checksum_valid(got, IPPROTO_UDP, UDP_HDR_LEN + 20));

    xframe_tx_stop(&t);
}

/* Checksum pass-through on receive: a deliberately wrong L3/L4 checksum (UDP
 * here) must land in the buffer unchanged -- no recompute on the receive path
 * either. */
static void test_receive_preserves_checksums_verbatim(void)
{
    XframeTx t;
    uint8_t frame[128];
    uint8_t landed[128];
    size_t len;

    len = build_ipv4_frame(frame, xframe_station_mac, IPPROTO_UDP, 20, 62);
    frame[IPV4_CSUM_OFF] = 0xde; frame[IPV4_CSUM_OFF + 1] = 0xad;
    frame[UDP_CSUM_OFF] = 0xbe; frame[UDP_CSUM_OFF + 1] = 0xef;

    xframe_tx_start(&t);
    traffic_rx_capture(&t, frame, len, landed);

    g_assert_cmpmem(landed, len, frame, len);
    g_assert_cmphex(frame_be16(landed, IPV4_CSUM_OFF), ==, 0xdead);
    g_assert_cmphex(frame_be16(landed, UDP_CSUM_OFF), ==, 0xbeef);

    xframe_tx_stop(&t);
}

/*
 * Frame-size coverage on transmit: a sub-64-byte padded runt, a full 1500-MTU
 * frame, and the device's XFRAME_MAX_FRAME jumbo maximum must each egress
 * byte-for-byte. The runt is a small UDP datagram with trailing Ethernet
 * padding; the larger two are TCP segments filling the size exactly.
 */
static void test_transmit_frame_sizes_byte_exact(void)
{
    static const struct {
        size_t total;
        uint8_t proto;
        size_t payload;
    } cases[] = {
        { 60, IPPROTO_UDP, 4 },          /* sub-64 runt, Ethernet-padded */
        { 1514, IPPROTO_TCP, 1460 },     /* a full 1500-MTU frame */
        { XFRAME_MAX_FRAME_BYTES, IPPROTO_TCP,
          XFRAME_MAX_FRAME_BYTES - ETH_HDR_LEN - IPV4_HDR_LEN - TCP_HDR_LEN },
    };
    unsigned c;

    for (c = 0; c < G_N_ELEMENTS(cases); c++) {
        XframeTx t;
        uint8_t frame[XFRAME_MAX_FRAME_BYTES];
        uint8_t got[XFRAME_MAX_FRAME_BYTES];
        size_t len, emitted;

        len = build_ipv4_frame(frame, traffic_remote_mac, cases[c].proto,
                               cases[c].payload, cases[c].total);

        xframe_tx_start(&t);
        emitted = traffic_tx_capture(&t, frame, len, got, sizeof(got));
        g_assert_cmpint(emitted, ==, (int)cases[c].total);
        g_assert_cmpmem(got, emitted, frame, cases[c].total);
        xframe_tx_stop(&t);
    }
}

/* The receive counterpart of the frame-size coverage test. */
static void test_receive_frame_sizes_byte_exact(void)
{
    static const struct {
        size_t total;
        uint8_t proto;
        size_t payload;
    } cases[] = {
        { 60, IPPROTO_UDP, 4 },
        { 1514, IPPROTO_TCP, 1460 },
        { XFRAME_MAX_FRAME_BYTES, IPPROTO_TCP,
          XFRAME_MAX_FRAME_BYTES - ETH_HDR_LEN - IPV4_HDR_LEN - TCP_HDR_LEN },
    };
    unsigned c;

    for (c = 0; c < G_N_ELEMENTS(cases); c++) {
        XframeTx t;
        uint8_t frame[XFRAME_MAX_FRAME_BYTES];
        uint8_t landed[XFRAME_MAX_FRAME_BYTES];
        size_t len;

        len = build_ipv4_frame(frame, xframe_station_mac, cases[c].proto,
                               cases[c].payload, cases[c].total);

        xframe_tx_start(&t);
        traffic_rx_capture(&t, frame, len, landed);
        g_assert_cmpmem(landed, cases[c].total, frame, cases[c].total);
        xframe_tx_stop(&t);
    }
}

static void traffic_interrupt_commit(XframeTx *t, bool rx, unsigned entry,
                                     uint64_t data1, uint64_t data2)
{
    uint64_t data1_reg = rx ? RTI_DATA1_MEM : TTI_DATA1_MEM;
    uint64_t data2_reg = rx ? RTI_DATA2_MEM : TTI_DATA2_MEM;
    uint64_t command_reg = rx ? RTI_COMMAND_MEM : TTI_COMMAND_MEM;

    sock_pif_writeq(t, data1_reg, data1);
    sock_pif_writeq(t, data2_reg, data2);
    sock_pif_writeq(t, command_reg,
                    TRAFFIC_MEM_CMD_WE | TRAFFIC_MEM_CMD_STROBE |
                    TTI_MEM_CMD_OFF(entry));
}

static void xframe_migrate(QTestState *from, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);

    g_test_timer_start();
    for (;;) {
        QDict *ret = qtest_qmp_assert_success_ref(from,
            "{ 'execute': 'query-migrate' }");
        g_autofree char *status = g_strdup(qdict_get_str(ret, "status"));

        qobject_unref(ret);
        g_assert_cmpstr(status, !=, "failed");
        if (g_str_equal(status, "completed")) {
            break;
        }
        g_assert_true(g_test_timer_elapsed() < 30);
        g_usleep(1000);
    }
}

static char *xframe_migration_directory(void)
{
    g_autofree char *cwd = g_get_current_dir();
    char *directory = g_build_filename(cwd,
                                       ".xframe-migration-XXXXXX", NULL);

    g_assert_nonnull(g_mkdtemp(directory));
    return directory;
}

static void xframe_migration_cleanup(const char *directory,
                                     const char *socket)
{
    if (g_file_test(socket, G_FILE_TEST_EXISTS)) {
        g_assert_cmpint(g_unlink(socket), ==, 0);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

/*
 * Version 5 carried the traffic-interrupt memories but not moderation state.
 * A destination must retain an already-pending completion and its mask while
 * reverting future completions to the source model's immediate behavior.
 */
static void test_traffic_migration_from_v5(void)
{
    const char *v5_binary = g_getenv("XFRAME_QEMU_V5_BINARY");
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *incoming = NULL;
    XframeTx from, to;
    uint8_t frame[128];
    uint8_t got[128];
    uint8_t landed[128];
    size_t len;

    if (!v5_binary) {
        g_test_skip("XFRAME_QEMU_V5_BINARY is not set");
        return;
    }

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_UDP, 4, 60);
    directory = xframe_migration_directory();
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    incoming = g_strdup_printf("-incoming %s", uri);

    xframe_tx_start_binary_args(&from, v5_binary, NULL);
    sock_pif_writeq(&from, TX_TRAFFIC_MASK, 0);
    sock_pif_writeq(&from, RX_TRAFFIC_MASK, 0);
    traffic_interrupt_commit(&from, false, 0,
                             TRAFFIC_DATA1_TIMER(0x1000) |
                             TRAFFIC_DATA1_AC | TRAFFIC_DATA1_CI,
                             TRAFFIC_DATA2_COUNTS(4, 4, 4, 4));
    traffic_interrupt_commit(&from, true, 0,
                             TRAFFIC_DATA1_TIMER(0x1000) |
                             TRAFFIC_DATA1_AC | TRAFFIC_DATA1_CI,
                             TRAFFIC_DATA2_COUNTS(4, 4, 4, 4));
    g_assert_cmpuint(traffic_tx_capture(&from, frame, len, got, sizeof(got)),
                     ==, len);
    memcpy(frame, xframe_station_mac, sizeof(xframe_station_mac));
    traffic_rx_capture(&from, frame, len, landed);
    g_assert_cmphex(sock_pif_readq(&from, TX_TRAFFIC_INT) &
                    TX_TRAFFIC_FIFO0, ==, TX_TRAFFIC_FIFO0);
    g_assert_cmphex(sock_pif_readq(&from, RX_TRAFFIC_INT) &
                    RX_TRAFFIC_RING0, ==, RX_TRAFFIC_RING0);

    xframe_tx_start_args(&to, incoming);
    xframe_migrate(from.qs->qts, uri);
    g_assert_cmphex(sock_pif_readq(&to, TX_TRAFFIC_MASK), ==, 0);
    g_assert_cmphex(sock_pif_readq(&to, RX_TRAFFIC_MASK), ==, 0);
    g_assert_cmphex(sock_pif_readq(&to, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);
    g_assert_cmphex(sock_pif_readq(&to, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0,
                    ==, RX_TRAFFIC_RING0);
    sock_pif_writeq(&to, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);
    sock_pif_writeq(&to, RX_TRAFFIC_INT, RX_TRAFFIC_RING0);

    memcpy(frame, traffic_remote_mac, sizeof(traffic_remote_mac));
    g_assert_cmpuint(traffic_tx_capture(&to, frame, len, got, sizeof(got)),
                     ==, len);
    g_assert_cmphex(sock_pif_readq(&to, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);
    memcpy(frame, xframe_station_mac, sizeof(xframe_station_mac));
    traffic_rx_capture(&to, frame, len, landed);
    g_assert_cmphex(sock_pif_readq(&to, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0,
                    ==, RX_TRAFFIC_RING0);

    sock_pif_writeq(&to, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);
    sock_pif_writeq(&to, RX_TRAFFIC_INT, RX_TRAFFIC_RING0);
    sock_pif_writeq(&to, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_clock_step(to.qs->qts, 3000000);
    g_assert_cmphex(sock_pif_readq(&to, TX_TRAFFIC_INT), ==, 0);
    g_assert_cmphex(sock_pif_readq(&to, RX_TRAFFIC_INT), ==, 0);

    xframe_tx_stop(&to);
    xframe_tx_stop(&from);
    xframe_migration_cleanup(directory, socket);
}

/* A version 6 stream retains both committed memory and a partial batch. */
static void test_traffic_migration_v6_round_trip(void)
{
    const char *v6_binary = g_getenv("XFRAME_QEMU_V6_BINARY");
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *incoming = NULL;
    XframeTx from, to;
    uint8_t frame[128];
    uint8_t got[128];
    size_t len;
    unsigned i;

    if (!v6_binary) {
        g_test_skip("XFRAME_QEMU_V6_BINARY is not set");
        return;
    }

    directory = xframe_migration_directory();
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    incoming = g_strdup_printf("-incoming %s", uri);
    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_UDP, 4, 60);
    xframe_tx_start_binary_args(&from, v6_binary, NULL);
    sock_pif_writeq(&from, TX_TRAFFIC_MASK, 0);
    traffic_interrupt_commit(&from, false, 0,
                             TRAFFIC_DATA1_TIMER(0x1000) |
                             TRAFFIC_DATA1_AC,
                             TRAFFIC_DATA2_COUNTS(4, 4, 4, 4));
    for (i = 0; i < 2; i++) {
        g_assert_cmpuint(traffic_tx_capture(&from, frame, len, got,
                                            sizeof(got)), ==, len);
    }
    g_assert_cmphex(sock_pif_readq(&from, TX_TRAFFIC_INT), ==, 0);

    xframe_tx_start_args(&to, incoming);
    xframe_migrate(from.qs->qts, uri);
    for (i = 0; i < 2; i++) {
        g_assert_cmpuint(traffic_tx_capture(&to, frame, len, got,
                                            sizeof(got)), ==, len);
        g_assert_cmphex(sock_pif_readq(&to, TX_TRAFFIC_INT) &
                        TX_TRAFFIC_FIFO0, ==,
                        i == 1 ? TX_TRAFFIC_FIFO0 : 0);
    }

    xframe_tx_stop(&to);
    xframe_tx_stop(&from);
    xframe_migration_cleanup(directory, socket);
}

/* Independent TX and RX auto-cancel deadlines retain their remaining time. */
static void test_traffic_migration_v6_deadlines(void)
{
    const char *v6_binary = g_getenv("XFRAME_QEMU_V6_BINARY");
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *incoming = NULL;
    XframeTx from, to;
    uint8_t frame[128];
    uint8_t got[128];
    size_t len;

    if (!v6_binary) {
        g_test_skip("XFRAME_QEMU_V6_BINARY is not set");
        return;
    }

    directory = xframe_migration_directory();
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    incoming = g_strdup_printf("-incoming %s", uri);
    len = build_ipv4_frame(frame, xframe_station_mac, IPPROTO_UDP, 4, 60);
    xframe_tx_start_binary_args(&from, v6_binary, NULL);
    sock_pif_writeq(&from, TX_TRAFFIC_MASK, 0);
    sock_pif_writeq(&from, RX_TRAFFIC_MASK, 0);
    traffic_interrupt_commit(&from, false, 0,
                             TRAFFIC_DATA1_TIMER(0x100) |
                             TRAFFIC_DATA1_AC,
                             TRAFFIC_DATA2_COUNTS(4, 4, 4, 4));
    traffic_interrupt_commit(&from, true, 0,
                             TRAFFIC_DATA1_TIMER(0x1000) |
                             TRAFFIC_DATA1_AC,
                             TRAFFIC_DATA2_COUNTS(4, 4, 4, 4));

    xframe_program_rxd(&from, TRAFFIC_RX_RING, TRAFFIC_RX_BUF,
                       RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&from, TRAFFIC_RX_RING);
    net_socket_send_frame(from.host_fd, frame, len);
    g_assert_true(xframe_wait_rxd_consumed(&from, TRAFFIC_RX_RING, 5000));
    memcpy(frame, traffic_remote_mac, sizeof(traffic_remote_mac));
    g_assert_cmpuint(traffic_tx_capture(&from, frame, len, got, sizeof(got)),
                     ==, len);
    g_assert_cmphex(sock_pif_readq(&from, TX_TRAFFIC_INT), ==, 0);
    g_assert_cmphex(sock_pif_readq(&from, RX_TRAFFIC_INT), ==, 0);

    xframe_tx_start_args(&to, incoming);
    xframe_migrate(from.qs->qts, uri);
    qtest_clock_step(to.qs->qts, 200000);
    g_assert_cmphex(sock_pif_readq(&to, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);
    g_assert_cmphex(sock_pif_readq(&to, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0,
                    ==, 0);
    qtest_clock_step(to.qs->qts, 2000000);
    g_assert_cmphex(sock_pif_readq(&to, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0,
                    ==, RX_TRAFFIC_RING0);

    xframe_tx_stop(&to);
    xframe_tx_stop(&from);
    xframe_migration_cleanup(directory, socket);
}

/* A completion deferred behind a latched status relatches after migration. */
static void test_traffic_migration_v6_deferred(void)
{
    const char *v6_binary = g_getenv("XFRAME_QEMU_V6_BINARY");
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *incoming = NULL;
    XframeTx from, to;
    uint8_t frame[128];
    uint8_t got[128];
    size_t len;
    unsigned i;

    if (!v6_binary) {
        g_test_skip("XFRAME_QEMU_V6_BINARY is not set");
        return;
    }

    directory = xframe_migration_directory();
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    incoming = g_strdup_printf("-incoming %s", uri);
    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_UDP, 4, 60);
    xframe_tx_start_binary_args(&from, v6_binary, NULL);
    sock_pif_writeq(&from, TX_TRAFFIC_MASK, 0);
    traffic_interrupt_commit(&from, false, 0, TRAFFIC_DATA1_AC,
                             TRAFFIC_DATA2_COUNTS(1, 1, 1, 1));
    for (i = 0; i < 2; i++) {
        g_assert_cmpuint(traffic_tx_capture(&from, frame, len, got,
                                            sizeof(got)), ==, len);
    }
    g_assert_cmphex(sock_pif_readq(&from, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);

    xframe_tx_start_args(&to, incoming);
    xframe_migrate(from.qs->qts, uri);
    sock_pif_writeq(&to, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);
    g_assert_cmphex(sock_pif_readq(&to, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);
    sock_pif_writeq(&to, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);
    g_assert_cmphex(sock_pif_readq(&to, TX_TRAFFIC_INT), ==, 0);

    xframe_tx_stop(&to);
    xframe_tx_stop(&from);
    xframe_migration_cleanup(directory, socket);
}

/*
 * Auto-cancel batches each FIFO and ring independently. TX's shorter timer
 * publishes first without disturbing RX's tail; a later entry recommit proves
 * the count ceiling, while Linux's low-utilization receive count of one stays
 * immediate.
 */
static void test_traffic_interrupt_moderation(void)
{
    XframeTx t;
    uint8_t frame[128];
    uint8_t got[128];
    size_t len;
    unsigned i;

    len = build_ipv4_frame(frame, xframe_station_mac, IPPROTO_UDP, 10, 64);
    xframe_tx_start(&t);
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, 0);
    sock_pif_writeq(&t, RX_TRAFFIC_MASK, 0);

    traffic_interrupt_commit(&t, false, 0,
                             TRAFFIC_DATA1_TIMER(0x100) | TRAFFIC_DATA1_AC,
                             TRAFFIC_DATA2_COUNTS(4, 4, 4, 4));
    traffic_interrupt_commit(&t, true, 0,
                             TRAFFIC_DATA1_TIMER(0x1000) | TRAFFIC_DATA1_AC,
                             TRAFFIC_DATA2_COUNTS(4, 4, 4, 4));

    xframe_program_rxd(&t, TRAFFIC_RX_RING, TRAFFIC_RX_BUF,
                       RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, TRAFFIC_RX_RING);
    net_socket_send_frame(t.host_fd, frame, len);
    g_assert_true(xframe_wait_rxd_consumed(&t, TRAFFIC_RX_RING, 5000));

    memcpy(frame, traffic_remote_mac, sizeof(traffic_remote_mac));
    g_assert_cmpuint(traffic_tx_capture(&t, frame, len, got, sizeof(got)), ==,
                     len);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);
    g_assert_cmphex(sock_pif_readq(&t, RX_TRAFFIC_INT), ==, 0);

    qtest_clock_step(t.qs->qts, 200000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);
    g_assert_cmphex(sock_pif_readq(&t, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0,
                    ==, 0);
    sock_pif_writeq(&t, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);

    qtest_clock_step(t.qs->qts, 2000000);
    g_assert_cmphex(sock_pif_readq(&t, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0,
                    ==, RX_TRAFFIC_RING0);
    sock_pif_writeq(&t, RX_TRAFFIC_INT, RX_TRAFFIC_RING0);

    traffic_interrupt_commit(&t, false, 0,
                             TRAFFIC_DATA1_TIMER(0x1000) | TRAFFIC_DATA1_AC,
                             TRAFFIC_DATA2_COUNTS(3, 3, 3, 3));
    for (i = 0; i < 3; i++) {
        g_assert_cmpuint(traffic_tx_capture(&t, frame, len, got, sizeof(got)),
                         ==, len);
        g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) &
                        TX_TRAFFIC_FIFO0, ==,
                        i == 2 ? TX_TRAFFIC_FIFO0 : 0);
    }
    sock_pif_writeq(&t, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);

    traffic_interrupt_commit(&t, true, 0, LINUX_RTI_DATA1,
                             LINUX_RTI_DATA2);
    memcpy(frame, xframe_station_mac, sizeof(xframe_station_mac));
    xframe_program_rxd(&t, TRAFFIC_RX_RING, TRAFFIC_RX_BUF,
                       RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, TRAFFIC_RX_RING);
    net_socket_send_frame(t.host_fd, frame, len);
    g_assert_true(xframe_wait_rxd_consumed(&t, TRAFFIC_RX_RING, 5000));
    g_assert_cmphex(sock_pif_readq(&t, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0,
                    ==, RX_TRAFFIC_RING0);

    xframe_tx_stop(&t);
}

/*
 * Linux enables continuous interrupts on TTI entry 0. The first event is one
 * full programmed interval after commit, repeats without traffic, and an
 * adapter disable abandons the old phase so re-enable starts a fresh one.
 */
static void test_traffic_continuous_interrupt(void)
{
    XframeTx t;

    xframe_tx_start(&t);
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, 0);
    sock_pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    traffic_interrupt_commit(&t, false, 0, LINUX_TTI_DATA1,
                             LINUX_TTI_DATA2);
    traffic_interrupt_commit(&t, true, 0,
                             TRAFFIC_DATA1_TIMER(0x3000) |
                             TRAFFIC_DATA1_CI,
                             TRAFFIC_DATA2_COUNTS(1, 1, 1, 1));

    qtest_clock_step(t.qs->qts, 3900000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);
    g_assert_cmphex(sock_pif_readq(&t, RX_TRAFFIC_INT), ==, 0);
    qtest_clock_step(t.qs->qts, 200000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);
    g_assert_cmphex(sock_pif_readq(&t, RX_TRAFFIC_INT), ==, 0);
    sock_pif_writeq(&t, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);

    qtest_clock_step(t.qs->qts, 2000000);
    g_assert_cmphex(sock_pif_readq(&t, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0,
                    ==, RX_TRAFFIC_RING0);
    sock_pif_writeq(&t, RX_TRAFFIC_INT, RX_TRAFFIC_RING0);
    qtest_clock_step(t.qs->qts, 2000000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);
    sock_pif_writeq(&t, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);

    sock_pif_writeq(&t, ADAPTER_CONTROL, 0);
    qtest_clock_step(t.qs->qts, 5000000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);

    sock_pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_clock_step(t.qs->qts, 3900000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);
    qtest_clock_step(t.qs->qts, 200000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);

    xframe_tx_stop(&t);
}

/*
 * A CI expiry publishes an auto-cancel tail without moving its periodic
 * phase. A later count-triggered publication likewise leaves that phase
 * alone, and multiple expiries behind latched status become one deferred
 * publication for the next W1C.
 */
static void test_traffic_continuous_batch_and_w1c(void)
{
    XframeTx t;
    uint8_t frame[128];
    uint8_t got[128];
    size_t len;
    unsigned i;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_UDP, 4, 60);
    xframe_tx_start(&t);
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, 0);
    sock_pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    traffic_interrupt_commit(&t, false, 0,
                             TRAFFIC_DATA1_TIMER(0x100) |
                             TRAFFIC_DATA1_AC | TRAFFIC_DATA1_CI,
                             TRAFFIC_DATA2_COUNTS(3, 3, 3, 3));

    g_assert_cmpuint(traffic_tx_capture(&t, frame, len, got, sizeof(got)),
                     ==, len);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);
    qtest_clock_step(t.qs->qts, 130000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);
    sock_pif_writeq(&t, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);

    for (i = 0; i < 3; i++) {
        g_assert_cmpuint(traffic_tx_capture(&t, frame, len, got,
                                            sizeof(got)), ==, len);
        g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) &
                        TX_TRAFFIC_FIFO0, ==,
                        i == 2 ? TX_TRAFFIC_FIFO0 : 0);
    }
    sock_pif_writeq(&t, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);

    qtest_clock_step(t.qs->qts, 110000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);
    qtest_clock_step(t.qs->qts, 10000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);

    qtest_clock_step(t.qs->qts, 400000);
    sock_pif_writeq(&t, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);
    sock_pif_writeq(&t, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);

    xframe_tx_stop(&t);
}

/* Recommit, CI clear, a zero timer, and reset each abandon the old phase. */
static void test_traffic_continuous_configuration_lifecycle(void)
{
    XframeTx t;
    uint64_t ci = TRAFFIC_DATA1_TIMER(0x100) | TRAFFIC_DATA1_CI;

    xframe_tx_start(&t);
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, 0);
    sock_pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    traffic_interrupt_commit(&t, false, 0, ci,
                             TRAFFIC_DATA2_COUNTS(1, 1, 1, 1));

    qtest_clock_step(t.qs->qts, 60000);
    traffic_interrupt_commit(&t, false, 0, ci,
                             TRAFFIC_DATA2_COUNTS(1, 1, 1, 1));
    qtest_clock_step(t.qs->qts, 70000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);
    qtest_clock_step(t.qs->qts, 60000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);
    sock_pif_writeq(&t, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);

    traffic_interrupt_commit(&t, false, 0, TRAFFIC_DATA1_TIMER(0x100),
                             TRAFFIC_DATA2_COUNTS(1, 1, 1, 1));
    qtest_clock_step(t.qs->qts, 300000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);

    traffic_interrupt_commit(&t, false, 0, TRAFFIC_DATA1_CI,
                             TRAFFIC_DATA2_COUNTS(1, 1, 1, 1));
    qtest_clock_step(t.qs->qts, 300000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);

    traffic_interrupt_commit(&t, false, 0, ci,
                             TRAFFIC_DATA2_COUNTS(1, 1, 1, 1));
    qtest_clock_step(t.qs->qts, 60000);
    sock_pif_writeq(&t, SW_RESET, SW_RESET_KEY);
    qtest_clock_step(t.qs->qts, 200000);
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);

    xframe_tx_stop(&t);
}

/* A migrated continuous timer retains the source's partially elapsed phase. */
static void test_traffic_continuous_migration(void)
{
    g_autofree char *directory = xframe_migration_directory();
    g_autofree char *socket = g_build_filename(directory, "migration.sock",
                                                NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *incoming = g_strdup_printf("-incoming %s", uri);
    XframeTx from, to;
    int64_t source_time;

    xframe_tx_start(&from);
    sock_pif_writeq(&from, TX_TRAFFIC_MASK, 0);
    sock_pif_writeq(&from, ADAPTER_CONTROL, ADAPTER_EN);
    traffic_interrupt_commit(&from, false, 0, LINUX_TTI_DATA1,
                             LINUX_TTI_DATA2);
    source_time = qtest_clock_step(from.qs->qts, 2000000);

    xframe_tx_start_args(&to, incoming);
    xframe_migrate(from.qs->qts, uri);
    /* Qtest's explicit virtual clock is harness input, not migrated state. */
    qtest_clock_set(to.qs->qts, source_time);
    qtest_clock_step(to.qs->qts, 1900000);
    g_assert_cmphex(sock_pif_readq(&to, TX_TRAFFIC_INT), ==, 0);
    qtest_clock_step(to.qs->qts, 200000);
    g_assert_cmphex(sock_pif_readq(&to, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                    ==, TX_TRAFFIC_FIFO0);

    xframe_tx_stop(&to);
    xframe_tx_stop(&from);
    xframe_migration_cleanup(directory, socket);
}

/*
 * Transmit interrupt rate: push TRAFFIC_BURST frames back to back, acking the
 * completion after each so the next is a fresh, countable INTx assertion. The
 * test leaves the TTI entry uncommitted, so each frame raises its own
 * transmit-completion
 * interrupt and the raise count equals the frame count.
 */
static void test_transmit_interrupt_rate_is_one_per_frame(void)
{
    XframeTx t;
    uint8_t frame[128];
    uint8_t got[128];
    size_t len;
    int raises = 0;
    unsigned i;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_TCP, 10, 64);

    xframe_tx_start(&t);

    /* Unmask transmit completion the way the drivers do: general mask 0, the
     * transmit-traffic block mask 0. */
    sock_pif_writeq(&t, GENERAL_INT_MASK, 0);
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, 0);
    g_assert_false(xframe_intx_asserted(t.dev));

    for (i = 0; i < TRAFFIC_BURST; i++) {
        xframe_program_txd(&t, TRAFFIC_TXD_PAR, TRAFFIC_TX_BUF, frame, len);
        xframe_ring_doorbell(&t, TRAFFIC_TXD_PAR);
        g_assert_cmpint(traffic_recv_frame(t.host_fd, got, sizeof(got)),
                        ==, (int)len);

        /* This frame's completion raised its own interrupt. */
        if (xframe_intx_asserted(t.dev)) {
            raises++;
        }
        g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0,
                        ==, TX_TRAFFIC_FIFO0);

        /* Ack it so the next frame's completion is a fresh raise. */
        sock_pif_writeq(&t, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);
        g_assert_false(xframe_intx_asserted(t.dev));
    }

    /* One transmit-completion interrupt per frame before TTI is committed. */
    g_assert_cmpint(raises, ==, TRAFFIC_BURST);

    xframe_tx_stop(&t);
}

/*
 * Receive interrupt rate: post a ring of TRAFFIC_BURST owned descriptors,
 * inject that many frames one at a time, and ack the receive completion after
 * each so it is a fresh, countable INTx assertion. As on transmit, an
 * uncommitted RTI entry remains immediate. The descriptors share one buffer:
 * this counts completions, not landed bytes.
 */
static void test_receive_interrupt_rate_is_one_per_frame(void)
{
    XframeTx t;
    uint8_t frame[128];
    size_t len;
    int raises = 0;
    unsigned i;

    len = build_ipv4_frame(frame, xframe_station_mac, IPPROTO_UDP, 10, 64);

    xframe_tx_start(&t);

    /* Unmask receive completion on INTx: general mask 0, block mask 0. */
    sock_pif_writeq(&t, GENERAL_INT_MASK, 0);
    sock_pif_writeq(&t, RX_TRAFFIC_MASK, 0);

    for (i = 0; i < TRAFFIC_BURST; i++) {
        xframe_program_rxd(&t, TRAFFIC_RX_RING + (uint64_t)i * RXD_SIZE,
                           TRAFFIC_RX_BUF, RX_BUF_CAPACITY);
    }
    xframe_rx_ring_arm(&t, TRAFFIC_RX_RING);
    g_assert_false(xframe_intx_asserted(t.dev));

    for (i = 0; i < TRAFFIC_BURST; i++) {
        uint64_t slot = TRAFFIC_RX_RING + (uint64_t)i * RXD_SIZE;

        net_socket_send_frame(t.host_fd, frame, len);
        g_assert_true(xframe_wait_rxd_consumed(&t, slot, 5000));

        /* This frame's completion raised its own interrupt. */
        if (xframe_intx_asserted(t.dev)) {
            raises++;
        }
        g_assert_cmphex(sock_pif_readq(&t, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0,
                        ==, RX_TRAFFIC_RING0);

        /* Ack it so the next frame's completion is a fresh raise. */
        sock_pif_writeq(&t, RX_TRAFFIC_INT, RX_TRAFFIC_RING0);
        g_assert_false(xframe_intx_asserted(t.dev));
    }

    /* One receive-completion interrupt per frame before RTI is committed. */
    g_assert_cmpint(raises, ==, TRAFFIC_BURST);

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
    qtest_add_func("xframe/mac-config-write-lock-spans-both-halves",
                   test_mac_config_write_lock_spans_both_halves);
    qtest_add_func("xframe/traffic-interrupt-memory-programming",
                   test_traffic_interrupt_memory_programming);
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
    qtest_add_func("xframe/user-backend-reply-waits-for-transmit",
                   test_user_backend_reply_waits_for_transmit);
    qtest_add_func("xframe/transmit-out-of-range-pointer-is-safe",
                   test_transmit_out_of_range_pointer_is_safe);
    qtest_add_func("xframe/transmit-completion-raises-interrupt",
                   test_transmit_completion_raises_interrupt);
    qtest_add_func("xframe/transmit-completion-survives-repeated-cycles",
                   test_transmit_completion_survives_repeated_cycles);
    qtest_add_func("xframe/transmit-segments-a-large-send",
                   test_transmit_segments_a_large_send);
    qtest_add_func("xframe/transmit-segments-a-send-larger-than-a-frame",
                   test_transmit_segments_a_send_larger_than_a_frame);
    qtest_add_func("xframe/receive-reports-checksum-results",
                   test_receive_reports_checksum_results);
    qtest_add_func("xframe/receive-five-buffer-unclassified-frame",
                   test_receive_five_buffer_unclassified_frame);
    qtest_add_func("xframe/receive-five-buffer-splits-classified-headers",
                   test_receive_five_buffer_splits_classified_headers);
    qtest_add_func("xframe/receive-five-buffer-splits-a-jumbo-frame",
                   test_receive_five_buffer_splits_a_jumbo_frame);
    qtest_add_func("xframe/statistics-block-counts-traffic",
                   test_statistics_block_counts_traffic);
    qtest_add_func("xframe/statistics-block-write-span",
                   test_statistics_block_write_span);
    qtest_add_func("xframe/statistics-one-shot-clears-enable",
                   test_statistics_one_shot_clears_enable);
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
    qtest_add_func("xframe/receive-fetches-descriptor-once",
                   test_receive_fetches_descriptor_once);
    qtest_add_func("xframe/receive-recomputes-pending-irq-once",
                   test_receive_recomputes_pending_irq_once);
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
    qtest_add_func("xframe/transmit-tcp-frame-byte-exact",
                   test_transmit_tcp_frame_byte_exact);
    qtest_add_func("xframe/transmit-udp-frame-byte-exact",
                   test_transmit_udp_frame_byte_exact);
    qtest_add_func("xframe/receive-tcp-frame-byte-exact",
                   test_receive_tcp_frame_byte_exact);
    qtest_add_func("xframe/receive-udp-frame-byte-exact",
                   test_receive_udp_frame_byte_exact);
    qtest_add_func("xframe/transmit-preserves-checksums-verbatim",
                   test_transmit_preserves_checksums_verbatim);
    qtest_add_func("xframe/receive-preserves-checksums-verbatim",
                   test_receive_preserves_checksums_verbatim);
    qtest_add_func("xframe/transmit-inserts-requested-tcp-checksum",
                   test_transmit_inserts_requested_tcp_checksum);
    qtest_add_func("xframe/transmit-inserts-requested-udp-checksum",
                   test_transmit_inserts_requested_udp_checksum);
    qtest_add_func("xframe/transmit-checksum-request-on-non-ipv4-declined",
                   test_transmit_checksum_request_on_non_ipv4_declined);
    qtest_add_func("xframe/transmit-gather-list-releases-every-descriptor",
                   test_transmit_gather_list_releases_every_descriptor);
    qtest_add_func("xframe/rmac-max-payload-defaults-and-holds",
                   test_rmac_max_payload_defaults_and_holds);
    qtest_add_func("xframe/transmit-completion-with-masks-as-reset",
                   test_transmit_completion_with_masks_as_reset);
    qtest_add_func("xframe/transmit-frame-sizes-byte-exact",
                   test_transmit_frame_sizes_byte_exact);
    qtest_add_func("xframe/receive-frame-sizes-byte-exact",
                   test_receive_frame_sizes_byte_exact);
    qtest_add_func("xframe/traffic-interrupt-moderation",
                   test_traffic_interrupt_moderation);
    qtest_add_func("xframe/traffic-migration-from-v5",
                   test_traffic_migration_from_v5);
    qtest_add_func("xframe/traffic-migration-v6-round-trip",
                   test_traffic_migration_v6_round_trip);
    qtest_add_func("xframe/traffic-migration-v6-deadlines",
                   test_traffic_migration_v6_deadlines);
    qtest_add_func("xframe/traffic-migration-v6-deferred",
                   test_traffic_migration_v6_deferred);
    qtest_add_func("xframe/traffic-continuous-interrupt",
                   test_traffic_continuous_interrupt);
    qtest_add_func("xframe/traffic-continuous-batch-and-w1c",
                   test_traffic_continuous_batch_and_w1c);
    qtest_add_func("xframe/traffic-continuous-configuration-lifecycle",
                   test_traffic_continuous_configuration_lifecycle);
    qtest_add_func("xframe/traffic-continuous-migration",
                   test_traffic_continuous_migration);
    qtest_add_func("xframe/transmit-interrupt-rate-is-one-per-frame",
                   test_transmit_interrupt_rate_is_one_per_frame);
    qtest_add_func("xframe/receive-interrupt-rate-is-one-per-frame",
                   test_receive_interrupt_rate_is_one_per_frame);

    return g_test_run();
}
