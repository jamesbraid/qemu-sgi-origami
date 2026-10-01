/*
 * Neterion/S2io Xframe register and traffic tests on a Bridge head.
 * Covers PCI setup, PIF controls, transmit, receive, and multicast.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * The "none" machine holds one Bridge and adjacent transfer memory. The card
 * uses its native head; no Origin 200 node, crossbar, or fabric is modeled.
 * tests/qtest/sn0-xframe-test.c covers populated-machine per-slot byte-lane
 * correction and DMA routing. This test formerly ran on x86, before the fork
 * stopped building SGI devices there. Bridge's inbound translated PCI window
 * has no per-slot register to declare a 64-bit device: a doubleword access
 * becomes two 32-bit device cycles, as on a real slot with DEV_64BIT_DEVICE
 * clear. A trigger fires once, on the cycle covering its half. The single
 * eight-byte cycle for DEV_64BIT_DEVICE slots is tested on the populated
 * machine in sn0-xframe-test.c.
 *
 * PCI checks cover vendor/device identity (including the Herc device-id
 * property), default unbranded and optional SGI subsystem pairs, and both BAR
 * types and sizes. PIF checks cover the byte-swapper's fixed diagnostic
 * value, XMSI_ADDRESS scratch, SW_RESET key, RMAC_CFG_KEY lock, immediately
 * quiescent ADAPTER_STATUS, ADAPTER_CONTROL/ADAPTER_EN, and indirect
 * factory-MAC read from the configured netdev MAC. TxP checks cover a golden
 * doorbell frame and out-of-range pointer hang guard. RxDMA and guest-memory
 * RxD checks cover a golden frame with status and interrupt, out-of-range
 * ring-pointer and filtered-frame-consumption hang guards, short-buffer
 * truncation without overrun, and exhaustion/refill in place. Refill follows
 * driver order: interrupt ACK, then refill; recovery uses the device's poll
 * timer. Multicast CAM checks joined-group delivery with another group
 * filtered, IFF_ALLMULTI matching an arbitrary group, and continued unicast
 * delivery.
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "libqos/libqos-sgi-bridge.h"
#include "libqtest.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_regs.h"
#include "net/eth.h"
#include "standard-headers/linux/virtio_net.h"
#include <sys/socket.h>
#include <netinet/in.h>
#ifdef CONFIG_LINUX
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#endif

/*
 * The card is instantiated with an explicit MAC so the factory-MAC read-back
 * assertion has a known value to check. The value is packed into
 * RMAC_ADDR_DATA0_MEM bits 63:16, most-significant byte first (the order both
 * drivers unpack it in). See XFRAME_TEST_MAC_DATA0.
 */
#define XFRAME_TEST_MAC          "52:54:00:12:34:56"
#define XFRAME_TEST_MAC_DATA0    0x5254001234560000ULL
#define XFRAME_CARD              "-device xframe,mac=" XFRAME_TEST_MAC

/*
 * Identity a plain card (no SGI properties) presents: vendor 0x17d5, device
 * 0x5831 (Xframe I / Xena, SGI Release 1); 0x5832 is Xframe II / Herc.
 */
#define XFRAME_VENDOR                0x17d5
#define XFRAME_DEVICE_XENA           0x5831
#define XFRAME_DEVICE_HERC           0x5832
#define XFRAME_SGI_SUBSYSTEM_VENDOR  0x10a9

/*
 * BAR windows, restated from the Xframe documentation rather than derived
 * here: a test that recomputes the implementation's arithmetic checks that
 * the arithmetic ran, not that it is right.
 */
#define XFRAME_PIF_WINDOW_SIZE   0x4000    /* BAR0, spans >= 0x0000-0x33ff */
#define XFRAME_TXP_WINDOW_SIZE   0x100000  /* second BAR, 8 * 0x00020000 */

/*
 * PIF register offsets within BAR0 (public OpenBSD if_xgereg.h / Linux
 * s2io-regs.h names). Restated here rather than shared with the device so the
 * test pins the contract independently.
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
 * 63-loc). Clear is the big-endian reset default; setting a type's SE
 * bit switches that type's descriptors to little-endian. See
 * hw/net/xframe.c's xframe_desc_ldq/xframe_desc_stq.
 */
#define SWAPPER_CTRL_TXD_R_SE    (1ULL << 44)
#define SWAPPER_CTRL_RXD_R_SE    (1ULL << 30)

/*
 * ADAPTER_STATUS quiescent/ready value the driver's single non-retrying
 * attach read requires (if_xgereg.h QUIESCENT): TDMA/RDMA/PFC ready, TMAC
 * buffer empty, PIC quiescent, memory-controller DRAM+queues ready, both PLLs
 * locked, subsystem-quiescence/ready bits 8-15 of the high word, and the
 * fault bits clear.
 */
#define ADAPTER_STATUS_QUIESCENT 0xf400ffc300000000ULL

#define ADAPTER_EN               (1ULL << 56)  /* ADAPTER_CONTROL en */

/*
 * Interrupt aggregates use public OpenBSD if_xgereg.h / Linux s2io-regs.h
 * names and MSB-first s2BIT(loc) = bit 63-loc. Link change rolls into
 * GENERAL_INT_STATUS receive-MAC (GEN_INTR_RXMAC = s2BIT(34) = bit 29);
 * receive traffic uses s2BIT(40) = bit 23. The leaf is MAC_RMAC_ERR_REG
 * (OpenBSD RMAC_LINK_STATE_CHANGE_INT = 1ULL<<32; Linux s2BIT(31)).
 */
#define GEN_INTR_RXMAC             (1ULL << 29)
#define GEN_INTR_RXTRAFFIC         (1ULL << 23)
#define RMAC_LINK_STATE_CHANGE_INT (1ULL << 32)

/*
 * Second-level masks use public OpenBSD if_xgereg.h offsets: PCIXB(x) =
 * 0x0800 + x, MACRB(x) = 0x2000 + x. Link change passes through
 * MAC_RMAC_ERR_MASK (leaf) and MAC_INT_MASK (RMAC block). Active-high masks
 * use DISABLE_ALL_INTRS_64 to mask all sources. GENERAL_INT_MASK only retains
 * IRIX xg writes for readback.
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
 * IRIX xg uses the public Linux s2io-regs.h PIC/GPIO link path with MSB-first
 * bits. GPIO_INT_REG (0x890) holds LINK_UP = s2BIT(2) = bit 61 and LINK_DOWN
 * = s2BIT(1) = bit 62; GPIO_INT_MASK (0x898) masks those bits. PIC_INT_STATUS
 * (0x800) rolls them into PIC_INT_GPIO = s2BIT(4) = bit 59, under
 * PIC_INT_MASK (0x808); GENERAL_INT_STATUS uses GEN_INTR_TXPIC = s2BIT(0) =
 * bit 63. This source differs from the MAC RMAC path: xg opens PIC_INT_MASK
 * with 0, leaves MAC masks closed, and waits for this path at bring-up.
 */
#define PIC_INT_STATUS           0x0800
#define TXPIC_INT_REG            0x0810   /* PCI-X tx errors + sched IRQ */
#define TXPIC_INT_MASK           0x0818   /* xg opens this leaf */
#define GPIO_INT_REG             0x0890
#define GPIO_INT_MASK            0x0898
#define PIC_INT_GPIO             (1ULL << 59)
#define PIC_INT_TX               (1ULL << 63)   /* PIC_INT_TX = s2BIT(0) */
#define GPIO_INT_REG_LINK_UP     (1ULL << 61)
#define GPIO_INT_REG_LINK_DOWN   (1ULL << 62)
#define GPIO_INT_MASK_LINK_UP    (1ULL << 61)
#define GPIO_INT_MASK_LINK_DOWN  (1ULL << 62)
#define GEN_INTR_TXPIC           (1ULL << 63)

/*
 * Linux s2io-regs.h scheduled_int_ctrl at 0x918 uses SCHED_INT_CTRL_TIMER_EN
 * = s2BIT(0) = bit 63. IRIX xg enables a periodic, traffic-independent
 * watchdog interrupt through PIC_INT_MASK's PIC_INT_TX and TXPIC_INT_MASK.
 * The source runs TXPIC_INT_REG -> PIC_INT_STATUS PIC_INT_TX ->
 * GENERAL_INT_STATUS GEN_INTR_TXPIC, independent of link. Public
 * SCHED_INT_PERIOD is TBD; any nonzero test period suffices. The model uses a
 * fixed virtual-time period shorter than the watchdog interval.
 */
#define SCHED_INT_CTRL           0x0918
#define SCHED_INT_CTRL_TIMER_EN  (1ULL << 63)
#define SCHED_INT_TEST_PERIOD    0x00000000000186a0ULL  /* nonzero period */

#define SW_RESET_KEY             0xa5a5a50000000000ULL  /* 0xa5a5a5, 63:40 */
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
 * Use TMAC_EN|RMAC_EN as the MAC_CFG payload for the lock test. Any non-zero
 * value
 * proves whether the write took.
 */
#define MAC_CFG_PAYLOAD          0xc000000000000000ULL

/*
 * Multicast CAM uses the public drivers' 64-entry unicast/factory-MAC table.
 * RMAC_ADDR_DATA1_MEM mask bits align with DATA0 address bits 63:16,
 * MSB-first; a clear mask bit requires a match. Linux
 * drivers/net/ethernet/neterion/s2io.c s2io_set_multicast writes
 * DATA1_MEM_MASK(0) for an exact join. Its IFF_ALLMULTI entry uses address
 * 0x010203040506 and mask 0xfeffffffffff: only byte 0 bit 0, the IEEE group
 * bit, must equal 1. OpenBSD if_xge.c xge_setmulti instead writes
 * 0xFFFFFFFFFFFFFFFF for a join and 0x8000...000/0xF000...000 for
 * IFF_ALLMULTI; those literal values do not mean all multicast under either
 * mask polarity with this packing. The model follows Linux; xframe_rx_filter
 * in xframe.c explains the full derivation.
 */
#define MCAST_GROUP_A_DATA0      0x01005e0000010000ULL  /* 01:00:5e:00:00:01*/
#define MCAST_GROUP_B_DATA0      0x01005e0000020000ULL  /* 01:00:5e:00:00:02*/
#define MCAST_EXACT_MASK         0ULL                   /* all bits match */
#define MCAST_ALLMULTI_DATA0     0x0102030405060000ULL  /* Linux's sentinel */
#define MCAST_ALLMULTI_MASK      0xfeffffffffff0000ULL  /* group bit alone */

/*
 * Transmit path (BAR2 TxP window + the TxD ring in guest memory), from public
 * OpenBSD if_xgereg.h / Linux s2io.h. The doorbell is a per-FIFO TxDL slot of
 * 16 bytes: the list pointer (PAR) at +0, the list control (LCR) at +8.
 * Writing LCR launches the list.
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

/*
 * The card's jumbo frame limit; an unsegmented buffer arrives at this size.
 */
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
 * public OpenBSD if_xgereg.h. The ring base register takes the first
 * descriptor page; the ring controller is put in service with RC_IN_SVC. A
 * completed frame raises RX_TRAFFIC_INT.
 */
#define PRC_RXD0_0               0x1930   /* receive ring base register */
#define PRC_CTRL_0               0x1970   /* receive ring controller */
#define RC_IN_SVC                (1ULL << 56)

/*
 * Receive ring mode, PRC_CTRL_0 bits 49:48. IRIX arms mode 2 when
 * the MTU is raised to a jumbo value. The public s2io header defines
 * this five-buffer header-split layout with 64-byte descriptors
 * (three control words plus five pointers).
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
 * STAT_ADDR holds the guest address; STAT_CFG's top bit enables periodic DMA
 * (public s2io stat_cfg/stat_addr and stat_block offsets). Each 64-bit
 * counter word puts frames in the high half. SWAPPER_CTRL_STATS_SE selects
 * whole-word byte order: reset big-endian gives frames first (dma_ldl_be
 * here); STATS_FE|STATS_SE gives little-endian octets first, as declared
 * publicly.
 */
#define STAT_CFG                 0x09d0
#define STAT_ADDR                0x09d8
#define STAT_CFG_EN              (1ULL << 63)
#define STAT_CFG_ONE_SHOT        (1ULL << 62)
#define SWAPPER_CTRL_STATS_FE    (1ULL << 15)
#define SWAPPER_CTRL_STATS_SE    (1ULL << 14)
#define STAT_TMAC_WORD           0x000
#define STAT_RMAC_WORD           0x060
#define STAT_TMAC_FRMS           STAT_TMAC_WORD          /* big-endian */
#define STAT_TMAC_DATA_OCTETS    (STAT_TMAC_WORD + 4)    /* big-endian */
/*
 * The card writes the hardware section [0, 0x320); the driver's tail follows.
 */
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
 * The modelled minimum between a frame arriving and its completion, in
 * nanoseconds: the device default. Delivery is published only once this much
 * virtual time has passed, so every wait for a completion has to advance the
 * clock as well as give the main loop a pass.
 */
#define RX_ARRIVAL_LATENCY_NS  200000

/*
 * How far to step QEMU_CLOCK_VIRTUAL past the model's delayed link event.
 * Tests do not assert the exact delay, which remains outside the contract.
 */
#define LINK_UP_STEP_NS          (20 * 1000 * 1000)

/*
 * A larger step used by existing tests that only need the delayed link event
 * to have settled, without making its exact timing part of their assertions.
 */
#define LINK_SETTLE_STEP_NS      (700LL * 1000 * 1000)

/*
 * How far to step QEMU_CLOCK_VIRTUAL to cross one period of the scheduled
 * interrupt (XFRAME_SCHED_INT_PERIOD_MS in xframe.c, 2000 ms). Step
 * past one period, but short of a second one. Also used to step well past the
 * point a periodic link re-raise would have fired, to prove the bring-up
 * LINK_UP is delivered exactly once.
 */
#define SCHED_INT_STEP_NS        (3000LL * 1000 * 1000)

/*
 * A 1-buffer-mode RxD is four 64-bit words: host control, control1, control2,
 * buffer pointer. control1 carries ownership (bit 56, 1=card/0=host) and the
 * status code (XCODE, bits 51:48); control2 carries the buffer size in bits
 * 63:48. The received length overwrites the posted capacity.
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
#define RXD_PROTO_IP_FRAG        (1ULL << 34)
#define RXD_PROTO_TCP            (1ULL << 33)
#define RXD_PROTO_UDP            (1ULL << 32)
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
 * The station address set by -device mac=, in wire order. The receive
 * filter compares it with an incoming frame's destination.
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

    t->qs = qtest_sgi_bridge_boot("%s", XFRAME_CARD);
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
 * Real drivers use 32-bit cycles for 64-bit registers: IRIX xg reads its
 * swapper that way, Linux s2io splits Xena MAC_CFG, and Origin 200 Bridge
 * splits DEV_64BIT_DEVICE-clear slots. Low bits are at off+0; high at off+4.
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
 * TxD/RxD words default to big-endian: Linux s2io_set_swapper states this;
 * OpenBSD xge_attach skips SWAPPER_CTRL on big-endian hosts. Big-endian IRIX
 * on Origin 200 also leaves the default. Descriptor words use explicit
 * big-endian bytes, independent of little-endian register MMIO
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
 * that bit clear (the reset state used by these tests and IRIX), the
 * block is big-endian, exactly as descriptors are. Do not use qtest_readl: it
 * applies the x86_64 test target's little-endian correction and would read
 * the block byte-reversed.
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
 * guest running Linux s2io or OpenBSD xge sets a descriptor type's
 * _SE bit and writes its descriptors this way; see
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
 * writing the other direction. The socket backend uses the same
 * length-prefixed framing both ways.
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

    t->qs = qtest_sgi_bridge_boot("-netdev socket,fd=%d,id=n0 "
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

#ifdef CONFIG_LINUX
static bool xframe_tap_start(XframeTx *t, int *tap_fd, int *inject_fd,
                             bool vnet_hdr)
{
    struct ifreq ifr = { };
    const char *vnet_hdr_support = vnet_hdr ? "on" : "off";
    uint64_t barsize;
    int mirror[2], inject[2];

    *tap_fd = open("/dev/net/tun", O_RDWR);
    if (*tap_fd < 0) {
        g_test_skip("/dev/net/tun is unavailable");
        return false;
    }

    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (vnet_hdr) {
        ifr.ifr_flags |= IFF_VNET_HDR;
    }
    if (ioctl(*tap_fd, TUNSETIFF, &ifr) < 0) {
        close(*tap_fd);
        *tap_fd = -1;
        g_test_skip("creating a TAP requires CAP_NET_ADMIN");
        return false;
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, mirror), ==, 0);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, inject), ==, 0);
    net_socket_set_recv_timeout(mirror[0], 5);
    t->host_fd = mirror[0];
    *inject_fd = inject[0];
    t->qs = qtest_sgi_bridge_boot(
        "-netdev tap,fd=%d,id=n0 "
        "-device xframe,netdev=n0,mac=" XFRAME_TEST_MAC " "
        "-chardev socket,id=mirror0,fd=%d "
        "-object filter-mirror,id=f0,netdev=n0,queue=rx,outdev=mirror0,"
        "vnet_hdr_support=%s "
        "-chardev socket,id=inject0,fd=%d "
        "-object filter-redirector,id=f1,netdev=n0,queue=tx,indev=inject0,"
        "vnet_hdr_support=%s",
        *tap_fd, mirror[1], vnet_hdr_support, inject[1], vnet_hdr_support);
    t->dev = find_xframe(t->qs->pcibus, XFRAME_DEVICE_XENA);
    g_assert(t->dev != NULL);
    t->pif = qpci_iomap(t->dev, 0, &barsize);
    g_assert_cmphex(barsize, ==, XFRAME_PIF_WINDOW_SIZE);
    t->txp = qpci_iomap(t->dev, 2, &barsize);
    g_assert_cmphex(barsize, ==, XFRAME_TXP_WINDOW_SIZE);
    qpci_device_enable(t->dev);
    qtest_qmp_assert_success(t->qs->qts, "{ 'execute': 'query-status' }");
    return true;
}

static bool xframe_vnet_tap_start(XframeTx *t, int *tap_fd, int *inject_fd)
{
    return xframe_tap_start(t, tap_fd, inject_fd, true);
}
#endif

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

/*
 * rx-arrival-latency-us=0 isolates transmit-state publication. A synchronous
 * backend reply must remain invisible when the doorbell write returns. With
 * an arrival floor, zero elapsed virtual time would hide a missing deferral.
 */
static void xframe_user_start(XframeTx *t)
{
    uint64_t barsize;

    t->host_fd = -1;
    t->qs = qtest_sgi_bridge_boot("-netdev user,id=n0 "
                          "-device xframe,netdev=n0,rx-arrival-latency-us=0,"
                          "mac=" XFRAME_TEST_MAC);
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
 * pointer in if_xgereg.h's struct rxd1 field order (host control, control1,
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
 * indirect command/strobe interface, with writes enabled. The
 * factory-MAC read-back test uses the same interface without
 * RMAC_ADDR_CMD_WE.
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
 * fixed time: the socket backend's delivery is not synchronous with the write
 * that injects it, the same reason sgi-ioc3-eth-test.c's wait_for_rx_consume
 * polls instead of guessing a delay.
 */
static bool xframe_wait_rx_traffic(XframeTx *t, int attempts)
{
    int i;

    for (i = 0; i < attempts; i++) {
        if (sock_pif_readq(t, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0) {
            return true;
        }
        g_usleep(1000);
        qtest_clock_step(t->qs->qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    return false;
}

/*
 * Poll one RxD's ownership bit rather than RX_TRAFFIC_INT: the status bit is
 * sticky across every delivery until acknowledged, so once more than one
 * descriptor is in play it cannot tell which one landed. The descriptor's own
 * OWN bit can.
 */
static bool xframe_wait_rxd_consumed(XframeTx *t, uint64_t desc, int attempts)
{
    int i;

    for (i = 0; i < attempts; i++) {
        if (!(dma_ldq_be(t->qs->qts, desc + 8) & RXD_OWN)) {
            return true;
        }
        g_usleep(1000);
        qtest_clock_step(t->qs->qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    return false;
}

/*
 * Receive: post a card-owned RxD, inject a frame addressed to the station MAC
 * over the socket backend, and assert it lands in the guest buffer with the
 * documented status and that the receive interrupt is raised. A golden
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
 * The card writes a received frame and descriptor byte counts immediately,
 * but ownership release, traffic status, and INTx wait for the virtual-clock
 * arrival deadline. Without that latency floor, a transmit and its reply can
 * complete at the same virtual instant, unlike wire, peer, or DMA latency.
 */
static void test_receive_waits_for_its_arrival_deadline(void)
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
    /* Unmask, so the delivered line is a real assertion and not a mask. */
    sock_pif_writeq(&t, GENERAL_INT_MASK, 0);
    sock_pif_writeq(&t, RX_TRAFFIC_MASK, 0);
    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF0, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);
    qtest_memset(t.qs->qts, RX_BUF0, 0, sizeof(frame));

    net_socket_send_frame(t.host_fd, frame, sizeof(frame));

    /*
     * Wait for the card to take the frame, which it shows by writing it into
     * the buffer. Nothing here advances the virtual clock, so the arrival
     * deadline cannot have passed.
     */
    for (i = 0; i < 5000; i++) {
        qtest_memread(t.qs->qts, RX_BUF0, landed, sizeof(landed));
        if (memcmp(landed, frame, sizeof(frame)) == 0) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));

    /*
     * The byte count is written; the ownership release and the line are not.
     */
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 16) >>
                    RXD_BUFSIZ_SHIFT, ==, sizeof(frame));
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) & RXD_OWN,
                    ==, RXD_OWN);
    g_assert_cmphex(sock_pif_readq(&t, RX_TRAFFIC_INT), ==, 0);
    g_assert_false(xframe_intx_asserted(t.dev));

    /* Still nothing one nanosecond short of the deadline. */
    qtest_clock_step(t.qs->qts, RX_ARRIVAL_LATENCY_NS - 1);
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) & RXD_OWN,
                    ==, RXD_OWN);
    g_assert_cmphex(sock_pif_readq(&t, RX_TRAFFIC_INT), ==, 0);
    g_assert_false(xframe_intx_asserted(t.dev));

    /* And all three at once past it. */
    qtest_clock_step(t.qs->qts, 1);
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) & RXD_OWN, ==, 0);
    g_assert_cmphex(sock_pif_readq(&t, RX_TRAFFIC_INT), ==, RX_TRAFFIC_RING0);
    g_assert_true(xframe_intx_asserted(t.dev));

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
    sock_pif_writeq(&t, GENERAL_INT_MASK, ~GEN_INTR_RXTRAFFIC);
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
 * Guest-driven hang class: a receive ring base far out of range must not walk
 * off into nothing, spin, or wedge the device. No frame lands and the
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
 * A hang case: a frame the device's own filter drops (a unicast destination
 * that is not the station address, not promiscuous) must still count as
 * consumed, not report 0. A 0 return wedges the netdev queue, since
 * nothing about a filtered frame changes on a retry. A legitimate frame right
 * after must still be delivered, proving the queue never wedged.
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
     * Give the filtered frame time to reach and be dropped by the device, and
     * confirm it did not spuriously land. The next step tests whether the
     * queue still works.
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
 * Ring recovery requires polling after refill. Both public drivers ACK
 * RX_TRAFFIC_INT before refilling (OpenBSD xge_intr/xge_add_rxbuf; Linux
 * s2io_isr/fill_rx_buffers). They set OWN in guest RAM without rewriting
 * PRC_RXD0_0/PRC_CTRL_0, so no register write follows refill to trigger a
 * flush. This test ACKs, refills, then advances the recovery poll. Two posted
 * descriptors are consumed; a third frame stays in QEMU's disabled netdev
 * queue until the poll flushes it. An ACK-triggered flush cannot recover it.
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
     * Slot 2 was never posted; the frame remains in QEMU's netdev queue. An
     * untouched OWN=0 descriptor looks like a completed one, so memory cannot
     * prove the queue state. Wall-clock waiting lets the socket backend reach
     * the no-buffer path; successful delivery after refill proves recovery.
     */
    net_socket_send_frame(t.host_fd, frame3, sizeof(frame3));
    g_usleep(200000);

    /*
     * First, read the pending interrupt bits and write them back. This
     * matches the order both drivers use for interrupt acknowledgement.
     * It does not itself recover anything here: the ring is still empty at
     * this point, exactly as it is for the real drivers.
     */
    pending = sock_pif_readq(&t, RX_TRAFFIC_INT);
    g_assert_cmphex(pending & RX_TRAFFIC_RING0, ==, RX_TRAFFIC_RING0);
    sock_pif_writeq(&t, RX_TRAFFIC_INT, pending);

    /*
     * Refill in place, after the ack: guest RAM only, exactly what a driver's
     * own refill touches (OpenBSD xge_add_rxbuf, Linux fill_rx_buffers),
     * never PRC_RXD0_0/PRC_CTRL_0.
     */
    xframe_program_rxd(&t, slot2, RX_BUF0, RX_BUF_CAPACITY);

    /*
     * Nothing register-visible follows the refill for either driver, so only
     * the recovery poll's own timer can notice it. QEMU_CLOCK_VIRTUAL is
     * frozen under qtest until stepped explicitly, so advance it past the
     * timer's period rather than waiting on wall time, which would never make
     * it fire at all.
     */
    qtest_clock_step(t.qs->qts, RX_RECOVERY_STEP_NS);

    g_assert_true(xframe_wait_rxd_consumed(&t, slot2, 5000));
    qtest_memread(t.qs->qts, RX_BUF0, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(frame3), frame3, sizeof(frame3));

    xframe_tx_stop(&t);
}

/*
 * A joined multicast group is a masked CAM entry with an exact-match mask
 * (every bit compared). See MCAST_EXACT_MASK's comment for the derivation.
 * A frame to a different group finds no matching entry and is filtered (the
 * descriptor stays available, not consumed); the joined group's own frame is
 * delivered into that same descriptor right after. The filter, rather
 * than timing, distinguishes them.
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
     * The unjoined group is filtered, not queued. Unlike the no-buffer
     * case, a filtered frame leaves no ambiguity: the descriptor's OWN bit is
     * a real, immediate, device-visible signal that nothing landed.
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
 * The IFF_ALLMULTI sentinel (Linux s2io_set_multicast's address/mask pair;
 * see MCAST_ALLMULTI_DATA0/MASK's comment) matches an
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
 * (entry 0, an exact-match mask because reset zeroes mac_addr_mask[]) is
 * still delivered, and a random unicast address, matching
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
 * The generic device must enumerate with no SGI-board properties set.
 * The model name follows the silicon.
 */
static void test_identity(void)
{
    QOSState *qs = qtest_sgi_bridge_boot("%s", XFRAME_CARD);
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
    QOSState *qs = qtest_sgi_bridge_boot("-device xframe,device-id=0x5832");
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
    QOSState *qs = qtest_sgi_bridge_boot("%s", XFRAME_CARD);
    QPCIDevice *dev = find_xframe(qs->pcibus, XFRAME_DEVICE_XENA);

    g_assert(dev != NULL);
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_VENDOR_ID), ==, 0);
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_ID), ==, 0);

    g_free(dev);
    qtest_shutdown(qs);
}

/*
 * The SGI instantiation sets subsystem-vendor-id 0x10a9. Its subsystem device
 * ID is unknown and must not be invented, so the other half of the same dword
 * must still read the unbranded default. No device ID is assumed.
 */
static void test_subsystem_vendor_when_sgi_board(void)
{
    QOSState *qs = qtest_sgi_bridge_boot(
        "-device xframe,subsystem-vendor-id=0x10a9");
    QPCIDevice *dev = find_xframe(qs->pcibus, XFRAME_DEVICE_XENA);

    g_assert(dev != NULL);
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_VENDOR_ID), ==,
                    XFRAME_SGI_SUBSYSTEM_VENDOR);
    g_assert_cmphex(qpci_config_readw(dev, PCI_SUBSYSTEM_ID), ==, 0);

    g_free(dev);
    qtest_shutdown(qs);
}

/*
 * BAR0: a 64-bit memory BAR (config 0x10/0x14), sized to cover the PIF map.
 */
static void test_bar0_is_a_64bit_pif_window(void)
{
    QOSState *qs = qtest_sgi_bridge_boot("%s", XFRAME_CARD);
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
 * The second BAR (config 0x18, BAR index 2, since index 1 is BAR0's own
 * upper half): a 32-bit memory window for the TxP transmit doorbell.
 */
static void test_bar2_is_the_txp_window(void)
{
    QOSState *qs = qtest_sgi_bridge_boot("%s", XFRAME_CARD);
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
 * constant. This golden assertion pins the value and catches a
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
 * ADAPTER_STATUS presents its quiescent/ready bits immediately. The
 * attach-gating check reads once, without retrying or polling.
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
 * Idle link change needs no frame. OpenBSD xge_init unmasks RMAC via
 * MAC_INT_MASK = MAC_TMAC_INT and MAC_RMAC_ERR_MASK =
 * ~RMAC_LINK_STATE_CHANGE_INT; Linux en_dis_err_alarms also unmasks RX-MAC.
 * Both drivers service it in their ISR. After ADAPTER_EN rises, a short
 * virtual-time delay latches MAC_RMAC_ERR_REG, passes both MAC masks, sets
 * the GENERAL_INT_STATUS receive-MAC bit, and asserts INTx. It clears on
 * write 1; this test opens GENERAL and block masks and observes the line.
 */
static void test_link_up_raises_interrupt(void)
{
    XframeTest t;

    xframe_start(&t);

    /* Open the GENERAL and both MAC masks for the link source. */
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

    /*
     * The MAC block mask gates the line; unmasking the link bit restores it.
     */
    pif_writeq(&t, MAC_RMAC_ERR_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(t.dev));
    pif_writeq(&t, MAC_RMAC_ERR_MASK, ~RMAC_LINK_STATE_CHANGE_INT);
    g_assert_true(xframe_intx_asserted(t.dev));

    /* Writing the bit back acknowledges the interrupt and drops the line. */
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
 * MAC_CFG is write-locked: a write without first writing RMAC_CFG_KEY =
 * 0x4c0d does not take; with the key, it does.
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
 * One RMAC_CFG_KEY unlock covers a full MAC_CFG write, even across two 32-bit
 * bus cycles. Linux s2io on Xena and an Origin 200 Bridge slot with
 * DEV_64BIT_DEVICE clear use split cycles; IRIX leaves that slot setting
 * clear. Re-locking after the first half loses the second. MAC_CFG holds
 * receive enables: after re-bring-up, broadcast remained disabled, ARP
 * dropped, and one ignored-write log line was the only clue. This payload
 * sets both halves; a whole-register value in one half would miss the bug.
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
 * Real drivers use 32-bit cycles for 64-bit registers: IRIX xg reads
 * PIF_RD_SWAPPER_FB that way, Linux s2io on Xena splits MAC_CFG, and Origin
 * 200 Bridge splits DEV_64BIT_DEVICE-clear slots. The low half is at off+0,
 * high at off+4; either write must preserve the other half. Three remaining
 * cases test registers whose high-half triggers differ and must ignore
 * low-half writes. Plain readback over three other registers duplicated one
 * check; the MAC-table strobe still checks both halves.
 */

/*
 * SW_RESET's key sits in bits 63:40, the register's high 32 bits, so a
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

    /*
     * High half carrying the key (0xa5a5a5 in bits 63:40): reset fires once.
     */
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
 * once and loads the factory MAC into DATA0, which reads back correctly both
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
    uint32_t after_poll;
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
     * request, but the receive is deferred to a bottom half that the main
     * loop runs once the doorbell write returns. After it, the RxD hands
     * ownership back to the guest: its big-endian bit-56 ownership byte reads
     * as 0 through a little-endian word read of the descriptor.
     */
    qpci_io_writel(t.dev, t.txp, TXP_TXDL_LCR + 4, (uint32_t)(lcr >> 32));
    after_poll = qtest_readl(t.qs->qts, RX_RING_BASE + 8);
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
 * the device keeps working. A valid transmit right after still goes out.
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
 * A transmit doorbell sets FIFO TX_TRAFFIC_INT, rolls into the general
 * transmit-traffic bit under TX_TRAFFIC_MASK, and asserts INTx until write-1
 * clear. OpenBSD xge_intr and Linux s2io_isr reclaim descriptors there;
 * without it, a completion-waiting driver stalls after its first burst.
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

    /* Open the GENERAL and transmit-traffic masks for completion. */
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

    /*
     * The transmit-traffic block mask gates the line; unmasking restores it.
     */
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(t.dev));
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, 0);
    g_assert_true(xframe_intx_asserted(t.dev));

    /* Writing pending bits back clears the interrupt and drops INTx. */
    sock_pif_writeq(&t, TX_TRAFFIC_INT, pending);
    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);
    g_assert_cmphex(sock_pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXTRAFFIC,
                    ==, 0);

    xframe_tx_stop(&t);
}

/*
 * Repeated doorbell, completion, and ACK cycles catch one-shot latches and
 * missing reassertion. Each pass must emit a frame, raise a fresh completion,
 * and clear INTx with no pending source. The line must be low before every
 * doorbell; a stuck high line hides later completions and stops descriptor
 * reclamation during sustained traffic.
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

        /*
         * The descriptor comes back to the host on every cycle, not just one.
         */
        g_assert_cmphex(dma_ldq_be(t.qs->qts, par) & TXD_OWN, ==, 0);

        sock_pif_writeq(&t, TX_TRAFFIC_INT, pending);
        g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT), ==, 0);
    }

    xframe_tx_stop(&t);
}

/*
 * Little-endian Linux s2io and OpenBSD xge set TxD/RxD _SE and use
 * little-endian descriptors; reset SWAPPER_CTRL=0 tests cover only
 * big-endian. Here a little-endian TxD emits a frame and releases OWN in that
 * order; a little-endian RxD receives and returns status in that order.
 * Unconditional big-endian decoding reads both OWN bits clear: transmit emits
 * nothing and receive reports no buffer.
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
 * A MAC link source reaches INTx only when its leaf, block, and GENERAL
 * aggregate masks are all open. GENERAL_INT_STATUS remains visible while the
 * source is delivered; masked-summary visibility is intentionally not tested.
 */
static void test_xg_enable_path_link_via_block_masks(void)
{
    XframeTest t;

    xframe_start(&t);

    /*
     * Confirm reset starts masked, then open only the receive-MAC aggregate.
     */
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_MASK), ==, DISABLE_ALL_INTRS_64);
    pif_writeq(&t, GENERAL_INT_MASK, ~GEN_INTR_RXMAC);

    /* Open the receive-MAC block and its link-state-change leaf. */
    pif_writeq(&t, MAC_INT_MASK, MAC_TMAC_INT);
    pif_writeq(&t, MAC_RMAC_ERR_MASK, ~RMAC_LINK_STATE_CHANGE_INT);

    /* Idle, adapter not yet enabled: nothing to raise the line. */
    g_assert_false(xframe_intx_asserted(t.dev));

    /* Enable the adapter; the link comes up a short virtual-time later. */
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    g_assert_false(xframe_intx_asserted(t.dev));
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);

    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_REG) &
                    RMAC_LINK_STATE_CHANGE_INT, ==, RMAC_LINK_STATE_CHANGE_INT);

    /* The MAC link source rolls up as GEN_INTR_RXMAC. */
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

    /* Writing the bit back acknowledges the interrupt and drops the line. */
    pif_writeq(&t, MAC_RMAC_ERR_REG, RMAC_LINK_STATE_CHANGE_INT);
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_stop(&t);
}

/*
 * IRIX xg opens only PIC_INT_MASK and TXPIC_INT_MASK during bring-up. It
 * leaves GENERAL_INT_MASK, GPIO_INT_MASK, and the MAC masks at their all-ones
 * reset values. The GPIO link edge must nevertheless reach INTx through the
 * PIC block, appear in the raw GENERAL_INT_STATUS summary, and clear when xg
 * reads GPIO_INT_REG.
 */
static void test_xg_bringup_pic_summary_serviced(void)
{
    XframeTest t;

    xframe_start(&t);

    g_assert_cmphex(pif_readq(&t, GENERAL_INT_MASK), ==,
                    DISABLE_ALL_INTRS_64);
    g_assert_cmphex(pif_readq(&t, GPIO_INT_MASK), ==,
                    DISABLE_ALL_INTRS_64);
    g_assert_cmphex(pif_readq(&t, MAC_INT_MASK), ==,
                    DISABLE_ALL_INTRS_64);
    g_assert_cmphex(pif_readq(&t, MAC_RMAC_ERR_MASK), ==,
                    DISABLE_ALL_INTRS_64);

    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, TXPIC_INT_MASK, 0);

    g_assert_false(xframe_intx_asserted(t.dev));
    pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    g_assert_false(xframe_intx_asserted(t.dev));
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);

    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==,
                    GEN_INTR_TXPIC);
    g_assert_cmphex(pif_readq(&t, PIC_INT_STATUS) & PIC_INT_GPIO, ==,
                    PIC_INT_GPIO);

    pif_writeq(&t, PIC_INT_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(t.dev));
    pif_writeq(&t, PIC_INT_MASK, 0);
    g_assert_true(xframe_intx_asserted(t.dev));

    g_assert_cmphex(pif_readq(&t, GPIO_INT_REG) & GPIO_INT_REG_LINK_UP, ==,
                    GPIO_INT_REG_LINK_UP);
    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==, 0);

    xframe_stop(&t);
}

/*
 * IRIX xg's watchdog resets an idle adapter about every ten-plus seconds
 * without an interrupt, dropping link each time. It needs any interrupt
 * within its interval, so xg enables scheduled_int_ctrl at 0x918 and unmasks
 * PIC_INT_TX through PIC_INT_MASK and TXPIC_INT_MASK. The periodic PIC/TXPIC
 * source is independent of traffic and link; the ISR services and clears it.
 * Each period asserts INTx and GEN_INTR_TXPIC/PIC_INT_TX/TXPIC_INT_REG, with
 * no GPIO link bit or MAC link latch. Both masks control it; reading
 * TXPIC_INT_REG clears it. One serviced interrupt recurs per period without a
 * storm.
 */
static void test_scheduled_interrupt_feeds_watchdog(void)
{
    XframeTest t;
    uint64_t txpic;

    xframe_start(&t);

    /* Open the two masks xg programs; GENERAL_INT_MASK stays at reset. */
    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, TXPIC_INT_MASK, 0);

    /*
     * Arm the scheduled interrupt as xg does: write enable and a period.
     * The adapter is deliberately NOT enabled, so no bring-up link edge is
     * ever presented: any interrupt seen here is the scheduled timer's alone,
     * and any GPIO link bit set would be a carrier-path contamination this
     * must not have.
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
     * the PIC_INT_TX sub-block. Both are non-destructive derived reads.
     * Crucially, NO GPIO link bit is set and the MAC link latch is untouched:
     * the watchdog feed is entirely off the link/carrier path.
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
     * self-clearing source. INTx drops and the summary bit clears.
     */
    txpic = pif_readq(&t, TXPIC_INT_REG);
    g_assert_cmphex(txpic, !=, 0);
    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, GENERAL_INT_STATUS) & GEN_INTR_TXPIC, ==, 0);

    /*
     * It recurs: another period brings a fresh interrupt, serviced the same.
     */
    qtest_clock_step(t.qs->qts, SCHED_INT_STEP_NS);
    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(pif_readq(&t, TXPIC_INT_REG), !=, 0);
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_stop(&t);
}

/* GENERAL_INT_MASK retains readback state but does not mask xg's source. */
static void test_general_mask_is_readback_only(void)
{
    XframeTest t;

    xframe_start(&t);

    pif_writeq(&t, PIC_INT_MASK, 0);
    pif_writeq(&t, TXPIC_INT_MASK, 0);

    /* Exercise the register's two supported 32-bit halves explicitly. */
    pif_writel(&t, GENERAL_INT_MASK + 0, UINT32_MAX);
    pif_writel(&t, GENERAL_INT_MASK + 4, UINT32_MAX);

    pif_writeq(&t, SCHED_INT_CTRL,
               SCHED_INT_CTRL_TIMER_EN | SCHED_INT_TEST_PERIOD);
    qtest_clock_step(t.qs->qts, SCHED_INT_STEP_NS);

    g_assert_true(xframe_intx_asserted(t.dev));

    pif_writel(&t, GENERAL_INT_MASK + 0, 0);
    g_assert_true(xframe_intx_asserted(t.dev));
    pif_writel(&t, GENERAL_INT_MASK + 4, 0);
    g_assert_true(xframe_intx_asserted(t.dev));

    pif_writel(&t, GENERAL_INT_MASK + 0, UINT32_MAX);
    g_assert_true(xframe_intx_asserted(t.dev));
    pif_writel(&t, GENERAL_INT_MASK + 4, UINT32_MAX);
    g_assert_true(xframe_intx_asserted(t.dev));

    pif_writel(&t, GENERAL_INT_MASK + 0, 0);
    g_assert_true(xframe_intx_asserted(t.dev));
    pif_writel(&t, GENERAL_INT_MASK + 4, 0);
    g_assert_true(xframe_intx_asserted(t.dev));

    pif_writel(&t, TXPIC_INT_REG + 4, 0);
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_stop(&t);
}

static void xframe_backend_link_up_with_gpio_mask(uint64_t gpio_mask,
                                                  bool expect_irq)
{
    XframeTx t;

    xframe_tx_start(&t);

    /*
     * Begin down and consume any model-specific raw event before unmasking.
     */
    qtest_qmp_assert_success(t.qs->qts,
                             "{'execute': 'set_link', 'arguments': "
                             "{'name': 'n0', 'up': false}}");
    sock_pif_readq(&t, GPIO_INT_REG);

    sock_pif_writeq(&t, GENERAL_INT_MASK, ~GEN_INTR_TXPIC);
    sock_pif_writeq(&t, PIC_INT_MASK, ~PIC_INT_GPIO);
    sock_pif_writeq(&t, GPIO_INT_MASK, gpio_mask);
    sock_pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);

    qtest_qmp_assert_success(t.qs->qts,
                             "{'execute': 'set_link', 'arguments': "
                             "{'name': 'n0', 'up': true}}");
    qtest_clock_step_next(t.qs->qts);

    g_assert_cmpint(xframe_intx_asserted(t.dev), ==, expect_irq);
    xframe_tx_stop(&t);
}

/* GPIO_INT_MASK is retained for readback; PIC_INT_MASK gates LINK_UP. */
static void test_gpio_link_up_mask_is_readback_only(void)
{
    xframe_backend_link_up_with_gpio_mask(~GPIO_INT_MASK_LINK_UP, true);
    xframe_backend_link_up_with_gpio_mask(DISABLE_ALL_INTRS_64, true);
}

static void xframe_backend_link_down_with_gpio_mask(uint64_t gpio_mask,
                                                    bool expect_irq)
{
    XframeTx t;

    xframe_tx_start(&t);

    sock_pif_writeq(&t, GENERAL_INT_MASK, ~GEN_INTR_TXPIC);
    sock_pif_writeq(&t, PIC_INT_MASK, ~PIC_INT_GPIO);
    sock_pif_writeq(&t, GPIO_INT_MASK, gpio_mask);

    qtest_qmp_assert_success(t.qs->qts,
                             "{'execute': 'set_link', 'arguments': "
                             "{'name': 'n0', 'up': false}}");

    g_assert_cmpint(xframe_intx_asserted(t.dev), ==, expect_irq);
    xframe_tx_stop(&t);
}

/* GPIO_INT_MASK is likewise readback-only for a LINK_DOWN edge. */
static void test_gpio_link_down_mask_is_readback_only(void)
{
    xframe_backend_link_down_with_gpio_mask(~GPIO_INT_MASK_LINK_DOWN, true);
    xframe_backend_link_down_with_gpio_mask(DISABLE_ALL_INTRS_64, true);
}

/* A masked MAC link source retains its existing read-clear behavior. */
static void test_masked_mac_link_latch_clears_on_read(void)
{
    XframeTx t;

    xframe_tx_start(&t);

    g_assert_cmphex(sock_pif_readq(&t, MAC_INT_MASK), ==,
                    DISABLE_ALL_INTRS_64);
    g_assert_cmphex(sock_pif_readq(&t, MAC_RMAC_ERR_MASK), ==,
                    DISABLE_ALL_INTRS_64);

    qtest_qmp_assert_success(t.qs->qts,
                             "{'execute': 'set_link', 'arguments': "
                             "{'name': 'n0', 'up': false}}");

    g_assert_cmphex(sock_pif_readq(&t, MAC_RMAC_ERR_REG) &
                    RMAC_LINK_STATE_CHANGE_INT, ==,
                    RMAC_LINK_STATE_CHANGE_INT);
    g_assert_cmphex(sock_pif_readq(&t, MAC_RMAC_ERR_REG) &
                    RMAC_LINK_STATE_CHANGE_INT, ==, 0);

    xframe_tx_stop(&t);
}

/* Establish and service an explicit backend UP event on the MAC path. */
static void xframe_service_explicit_link_up(XframeTx *t)
{
    xframe_tx_start(t);

    qtest_qmp_assert_success(t->qs->qts,
                             "{'execute': 'set_link', 'arguments': "
                             "{'name': 'n0', 'up': false}}");
    sock_pif_readq(t, MAC_RMAC_ERR_REG);

    sock_pif_writeq(t, GENERAL_INT_MASK,
                    ~(GEN_INTR_RXMAC | GEN_INTR_TXPIC));
    sock_pif_writeq(t, MAC_INT_MASK, MAC_TMAC_INT);
    sock_pif_writeq(t, MAC_RMAC_ERR_MASK,
                    ~RMAC_LINK_STATE_CHANGE_INT);
    sock_pif_writeq(t, ADAPTER_CONTROL, ADAPTER_EN);

    qtest_qmp_assert_success(t->qs->qts,
                             "{'execute': 'set_link', 'arguments': "
                             "{'name': 'n0', 'up': true}}");
    qtest_clock_step_next(t->qs->qts);
    g_assert_true(xframe_intx_asserted(t->dev));

    sock_pif_writeq(t, MAC_RMAC_ERR_REG, RMAC_LINK_STATE_CHANGE_INT);
    sock_pif_readq(t, GPIO_INT_REG);
    g_assert_false(xframe_intx_asserted(t->dev));
}

/* Rewriting an already-set ADAPTER_EN bit must not fabricate a link event. */
static void test_redundant_adapter_en_does_not_relink(void)
{
    XframeTx t;

    xframe_service_explicit_link_up(&t);
    sock_pif_writeq(&t, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_clock_step(t.qs->qts, LINK_SETTLE_STEP_NS);

    g_assert_false(xframe_intx_asserted(t.dev));
    g_assert_cmphex(sock_pif_readq(&t, MAC_RMAC_ERR_REG) &
                    RMAC_LINK_STATE_CHANGE_INT, ==, 0);

    xframe_tx_stop(&t);
}

/*
 * Scheduled service after a real link event must leave carrier undisturbed.
 */
static void test_scheduled_interrupt_holds_carrier_steady(void)
{
    XframeTx t;
    int period;

    xframe_service_explicit_link_up(&t);
    sock_pif_writeq(&t, PIC_INT_MASK, 0);
    sock_pif_writeq(&t, TXPIC_INT_MASK, 0);
    sock_pif_writeq(&t, SCHED_INT_CTRL,
                    SCHED_INT_CTRL_TIMER_EN | SCHED_INT_TEST_PERIOD);

    for (period = 0; period < 3; period++) {
        qtest_clock_step(t.qs->qts, SCHED_INT_STEP_NS);
        g_assert_true(xframe_intx_asserted(t.dev));
        g_assert_cmphex(sock_pif_readq(&t, MAC_RMAC_ERR_REG) &
                        RMAC_LINK_STATE_CHANGE_INT, ==, 0);
        g_assert_cmphex(sock_pif_readq(&t, TXPIC_INT_REG), !=, 0);
        g_assert_false(xframe_intx_asserted(t.dev));
    }

    xframe_tx_stop(&t);
}

/*
 * IRIX xg ACKs GEN_INTR_TXPIC by writing TXPIC_INT_REG, without reading its
 * self-clearing latch. Without write ACK, the latch and INTx stay set and ISR
 * re-entry hangs bring-up. XFRAME_TXPIC_INT_SCHED is bit 59; only a high-half
 * write consumes it. PIC_INT_STATUS PIC_INT_TX is a side-effect-free pending
 * probe; reading the TXPIC leaf's high half would itself ACK the source. Read
 * the leaf only after write ACK to confirm zero.
 */
static void test_xg_scheduled_interrupt_acks_via_write(void)
{
    XframeTest t;

    xframe_start(&t);

    /* Open the two masks xg programs; GENERAL_INT_MASK stays at reset. */
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
     * xg acknowledges with a write covering the high half, rather than the
     * read expected by the other acknowledgement path. This alone must
     * consume the latch:
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
 * A transmit completion reaches INTx when its traffic block mask is open,
 * even while GENERAL_INT_MASK remains at reset as in the xg sequence.
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

    /* Leave GENERAL_INT_MASK at reset and open only the traffic block. */
    g_assert_cmphex(sock_pif_readq(&t, GENERAL_INT_MASK), ==,
                    DISABLE_ALL_INTRS_64);
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, 0);
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_program_txd(&t, par, buf, frame, sizeof(frame));
    xframe_ring_doorbell(&t, par);
    len = net_socket_recv_frame(t.host_fd, got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));

    /* The open block is sufficient for the pending completion to assert. */
    g_assert_true(xframe_intx_asserted(t.dev));
    g_assert_cmphex(sock_pif_readq(&t, TX_TRAFFIC_INT) & TX_TRAFFIC_FIFO0, ==,
                    TX_TRAFFIC_FIFO0);

    /* The block mask gates the line. */
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(t.dev));
    sock_pif_writeq(&t, TX_TRAFFIC_MASK, 0);
    g_assert_true(xframe_intx_asserted(t.dev));

    /* Writing pending bits back clears the interrupt and drops the line. */
    sock_pif_writeq(&t, TX_TRAFFIC_INT, TX_TRAFFIC_FIFO0);
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_tx_stop(&t);
}

/*
 * Device traffic and interrupt-rate checks. Xframe passes Ethernet II, IPv4,
 * TCP, and UDP bytes unchanged: no checksum offload, segmentation, or header
 * inspection. L3/L4 checksums, even deliberately wrong ones, must survive
 * transmit and receive. IOC3 once replaced an L4 checksum with a partial
 * value, causing slirp to drop frames. Frames use public Ethernet II, RFC 791
 * IPv4, RFC 793 TCP, and RFC 768 UDP layouts with the standard internet
 * checksum. Sizes span padded sub-64-byte frames, 1500-MTU frames, and
 * XFRAME_MAX_FRAME jumbo frames. Unconfigured fallback raises one completion
 * per frame; ACK between frames exposes each new INTx assertion.
 */

/*
 * Guest RAM for the traffic tests: well clear of the transmit/receive scratch
 * the register tests above use, and spaced for a full XFRAME_MAX_FRAME
 * (9600-byte) jumbo frame in each buffer. All four sit inside the sixteen
 * megabytes the head hands a card, which begins where the register tests'
 * scratch does.
 */
#define TRAFFIC_TXD_PAR    (TX_RAM_SCRATCH + 0x400000u) /* descriptor list */
#define TRAFFIC_TX_BUF     (TX_RAM_SCRATCH + 0x410000u) /* transmit buffer */
#define TRAFFIC_RX_RING    (TX_RAM_SCRATCH + 0x500000u) /* page ring base */
#define TRAFFIC_RX_BUF     (TX_RAM_SCRATCH + 0x600000u) /* receive buffer */

/*
 * Header sizes and the frame max, from the public RFC layouts and the
 * device's own XFRAME_MAX_FRAME (9600).
 */
#define ETH_HDR_LEN            14
#define IPV4_HDR_LEN          20
#define TCP_HDR_LEN          20
#define UDP_HDR_LEN           8
#define XFRAME_MAX_FRAME_BYTES  9600

/*
 * Offsets of checksum fields in the assembled frame, read back by the
 * pass-through assertions. IPV4_CSUM at the IPv4 header's bytes
 * 10-11, TCP_CSUM at the TCP header's bytes 16-17, UDP_CSUM at the UDP
 * header's bytes 6-7.
 */
#define IPV4_CSUM_OFF   (ETH_HDR_LEN + 10)
#define TCP_CSUM_OFF    (ETH_HDR_LEN + IPV4_HDR_LEN + 16)
#define UDP_CSUM_OFF    (ETH_HDR_LEN + IPV4_HDR_LEN + 6)

/* How many frames the interrupt-rate tests push back to back. */
#define TRAFFIC_BURST   64

/*
 * The builder and checksum validators use these IPv4 endpoints in every
 * frame so they agree on the pseudo-header.
 */
static const uint8_t traffic_src_ip[4] = { 10, 0, 0, 1 };
static const uint8_t traffic_dst_ip[4] = { 10, 0, 0, 2 };

/*
 * The remote unicast destination for transmitted frames (transmit does not
 * filter, so any address serves).
 */
static const uint8_t traffic_remote_mac[6] = {
    0x02, 0x00, 0x53, 0x47, 0x49, 0x02
};

/*
 * The internet checksum (RFC 1071): a 16-bit ones-complement sum. _add folds
 * bytes into a running 32-bit accumulator (big-endian 16-bit words, an odd
 * trailing byte in the high half); _fold carries the accumulator down and
 * complements it. The sum over the covered bytes, including a correct
 * checksum field, folds to zero. The validators below check this.
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

/*
 * Read a big-endian 16-bit field out of a frame. All checksum and length
 * fields use this wire order.
 */
static uint16_t frame_be16(const uint8_t *frame, size_t off)
{
    return ((uint16_t)frame[off] << 8) | frame[off + 1];
}

/*
 * Assemble a complete Ethernet II / IPv4 / (TCP|UDP) frame into buf with
 * correct L3 and L4 checksums, zero-padded to total_len (>= the natural
 * header+payload length; the excess is Ethernet padding, as a runt gets).
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
    /*
     * A UDP checksum result of zero is transmitted as 0xffff (0 means
     * "none"); these endpoints never produce zero, but honour the rule.
     */
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

static void frame_set_ipv4_fragment(uint8_t *frame, size_t payload_len,
                                    uint16_t flags_offset)
{
    uint8_t *ip = frame + ETH_HDR_LEN;
    size_t ip_total = IPV4_HDR_LEN + payload_len;
    uint16_t csum;

    ip[2] = (uint8_t)(ip_total >> 8);
    ip[3] = (uint8_t)ip_total;
    ip[6] = (uint8_t)(flags_offset >> 8);
    ip[7] = (uint8_t)flags_offset;
    ip[10] = 0;
    ip[11] = 0;
    csum = frame_csum_fold(frame_csum_add(0, ip, IPV4_HDR_LEN));
    ip[10] = (uint8_t)(csum >> 8);
    ip[11] = (uint8_t)csum;
}

/*
 * A correct IPv4 header checksum folds the header (its own field included) to
 * zero.
 */
static bool ipv4_checksum_valid(const uint8_t *frame)
{
    return frame_csum_fold(frame_csum_add(0, frame + ETH_HDR_LEN,
                                          IPV4_HDR_LEN)) == 0;
}

/*
 * A correct L4 checksum folds the pseudo-header plus the whole L4 segment
 * (its own field included) to zero. l4_total is the L4 header plus payload
 * length.
 */
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

static void frame_set_ipv4_tcp_checksums(uint8_t *frame, size_t payload_len)
{
    uint8_t pseudo[12];
    uint8_t *ip = frame + ETH_HDR_LEN;
    uint8_t *tcp = ip + IPV4_HDR_LEN;
    uint16_t csum;
    uint32_t sum;

    ip[10] = 0;
    ip[11] = 0;
    tcp[16] = 0;
    tcp[17] = 0;

    csum = frame_csum_fold(frame_csum_add(0, ip, IPV4_HDR_LEN));
    ip[10] = (uint8_t)(csum >> 8);
    ip[11] = (uint8_t)csum;

    memcpy(pseudo, traffic_src_ip, 4);
    memcpy(pseudo + 4, traffic_dst_ip, 4);
    pseudo[8] = 0;
    pseudo[9] = IPPROTO_TCP;
    pseudo[10] = (uint8_t)((TCP_HDR_LEN + payload_len) >> 8);
    pseudo[11] = (uint8_t)(TCP_HDR_LEN + payload_len);
    sum = frame_csum_add(0, pseudo, sizeof(pseudo));
    sum = frame_csum_add(sum, tcp, TCP_HDR_LEN + payload_len);
    csum = frame_csum_fold(sum);
    tcp[16] = (uint8_t)(csum >> 8);
    tcp[17] = (uint8_t)csum;
}

/*
 * Read exactly n bytes from the stream socket, looping over short reads. A
 * single recv() on a stream socket may return fewer bytes than asked once a
 * frame is jumbo-sized, unlike the small frames net_socket_recv_frame reads
 * in one call.
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

static size_t mirror_recv_packet(int fd, uint8_t *buf, size_t buf_size,
                                 uint32_t *vnet_hdr_len)
{
    uint8_t word[4];
    size_t len;

    sock_read_full(fd, word, sizeof(word));
    len = ((uint32_t)word[0] << 24) | ((uint32_t)word[1] << 16) |
          ((uint32_t)word[2] << 8) | word[3];
    sock_read_full(fd, word, sizeof(word));
    *vnet_hdr_len = ((uint32_t)word[0] << 24) |
                    ((uint32_t)word[1] << 16) |
                    ((uint32_t)word[2] << 8) | word[3];
    g_assert_cmpuint(len, <=, buf_size);
    sock_read_full(fd, buf, len);
    return len;
}

static void redirector_send_packet(int fd, const uint8_t *buf, size_t len,
                                   uint32_t vnet_hdr_len)
{
    uint32_t packet_len = htonl(len);
    uint32_t header_len = htonl(vnet_hdr_len);
    struct iovec iov[] = {
        {
            .iov_base = &packet_len,
            .iov_len = sizeof(packet_len),
        }, {
            .iov_base = &header_len,
            .iov_len = sizeof(header_len),
        }, {
            .iov_base = (void *)buf,
            .iov_len = len,
        },
    };
    size_t total = sizeof(packet_len) + sizeof(header_len) + len;

    g_assert_cmpint(iov_send(fd, iov, G_N_ELEMENTS(iov), 0, total), ==,
                    total);
}

/*
 * The length-prefixed socket-backend frame read, robust to short reads (see
 * sock_read_full) so it can capture a jumbo frame.
 */
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
 * Transmit a complete TCP/IPv4 frame and assert every L2/L3/L4 byte the
 * device emits is byte-for-byte what was in the buffer, with the L3 and L4
 * checksum fields surviving both as the exact bytes built and as valid
 * checksums.
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
 * recomputed or "corrected" them, as the IOC3 model did, would overwrite
 * these sentinels; their verbatim survival proves xframe never touches them.
 */
static void test_transmit_preserves_checksums_verbatim(void)
{
    XframeTx t;
    uint8_t frame[128];
    uint8_t got[128];
    size_t len, emitted;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_TCP, 20, 74);
    /*
     * Corrupt both checksum fields to sentinels no correct checksum equals.
     */
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
 * hands the card a frame whose checksum fields are still placeholders and
 * sets the request in control2; the card owes it the finished checksums.
 * IRIX's xg driver transmits every TCP and UDP segment this way, and a
 * placeholder left on the wire is dropped by the receiver with no diagnostic
 * anywhere.
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
 * TX_TRAFFIC_MASK resets to all ones, masking completion until the guest
 * unmasks it. A driver omitting that write sees no interrupt and cannot
 * reclaim descriptors. This checks reset behavior separately from tests that
 * open the mask themselves.
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

    /*
     * Deliberately touch NEITHER mask: this is the reset state a driver
     * meets.
     */
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_program_txd(&t, par, buf, frame, len);
    xframe_ring_doorbell(&t, par);
    (void)traffic_recv_frame(t.host_fd, got, sizeof(got));

    /*
     * The frame goes out either way. Egress is not gated on the masks. What
     * is pinned here is the INTERRUPT: with the masks as reset, none is
     * delivered, so a driver that reclaims on completion is never told.
     */
    g_assert_cmphex(dma_ldq_be(t.qs->qts, par) & TXD_OWN, ==, 0);
    g_assert_false(xframe_intx_asserted(t.dev));

    xframe_tx_stop(&t);
}

/*
 * The largest receive payload the MAC accepts. A guest raises this to run
 * jumbo. The card must retain and report the requested value; otherwise
 * the driver cannot confirm the size it asked for.
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
 * descriptor it walks, so it owes ownership on every one of them. Returning
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
 * IRIX sets L2TCPSEG and gives one LSO descriptor several maximum segments'
 * payload plus MSS. The card must emit one rebuilt frame per segment. Sending
 * it whole produced 2920-byte TCP segments with peer-disagreed checksum
 * length; peers discarded them and accepted later correctly sized
 * retransmissions of the same sequence number. Bulk traffic then stalled on
 * retransmission timeouts after about a dozen frames. A four-segment send
 * checks frame sizes, rebuilt headers, MSS-sized sequence advances, and
 * payload reassembly.
 */
static void test_transmit_segments_a_large_send(void)
{
    XframeTx t;
    uint8_t frame[ETH_HDR_LEN + IPV4_HDR_LEN + TCP_HDR_LEN + 1736];
    uint8_t got[2048];
    uint8_t expected[ETH_HDR_LEN + IPV4_HDR_LEN + TCP_HDR_LEN + 512];
    uint8_t rebuilt[1736];
    const size_t mss = 512;
    const size_t payload = 1736;              /* 512 + 512 + 512 + 200 */
    const size_t hdr = ETH_HDR_LEN + IPV4_HDR_LEN + TCP_HDR_LEN;
    const size_t expect[4] = { 512, 512, 512, 200 };
    const uint8_t *sent_payload;
    const uint8_t *sent_tcp;
    uint32_t base_seq;
    size_t len, off, i;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_TCP, payload,
                           sizeof(frame));
    sent_tcp = frame + ETH_HDR_LEN + IPV4_HDR_LEN;
    sent_payload = sent_tcp + TCP_HDR_LEN;
    frame[ETH_HDR_LEN + IPV4_HDR_LEN + 13] = 0x19; /* FIN|PSH|ACK */
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

        memcpy(expected, frame, hdr);
        memcpy(expected + hdr, sent_payload + off, seg);
        expected[ETH_HDR_LEN + 2] = (uint8_t)((IPV4_HDR_LEN + TCP_HDR_LEN +
                                               seg) >> 8);
        expected[ETH_HDR_LEN + 3] =
            (uint8_t)(IPV4_HDR_LEN + TCP_HDR_LEN + seg);
        expected[ETH_HDR_LEN + 4] = (uint8_t)((0x1234 + i) >> 8);
        expected[ETH_HDR_LEN + 5] = (uint8_t)(0x1234 + i);
        expected[ETH_HDR_LEN + IPV4_HDR_LEN + 4] =
            (uint8_t)((base_seq + off) >> 24);
        expected[ETH_HDR_LEN + IPV4_HDR_LEN + 5] =
            (uint8_t)((base_seq + off) >> 16);
        expected[ETH_HDR_LEN + IPV4_HDR_LEN + 6] =
            (uint8_t)((base_seq + off) >> 8);
        expected[ETH_HDR_LEN + IPV4_HDR_LEN + 7] =
            (uint8_t)(base_seq + off);
        if (i + 1 < G_N_ELEMENTS(expect)) {
            expected[ETH_HDR_LEN + IPV4_HDR_LEN + 13] &= ~0x09;
        }
        frame_set_ipv4_tcp_checksums(expected, seg);

        /*
         * Every emitted byte, including flags and checksums, is contractual.
         */
        g_assert_cmpuint(len, ==, want);
        g_assert_cmpmem(got, len, expected, want);

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

#ifdef CONFIG_LINUX
static uint16_t frame_ipv4_pseudo_checksum_seed(const uint8_t *frame,
                                                size_t l4_len)
{
    uint8_t pseudo[12];
    const uint8_t *ip = frame + ETH_HDR_LEN;
    uint32_t sum;

    memcpy(pseudo, ip + 12, 8);
    pseudo[8] = 0;
    pseudo[9] = IPPROTO_TCP;
    pseudo[10] = (uint8_t)(l4_len >> 8);
    pseudo[11] = (uint8_t)l4_len;
    sum = frame_csum_add(0, pseudo, sizeof(pseudo));
    return (uint16_t)~frame_csum_fold(sum);
}

static void test_transmit_offloads_lso_to_vnet_backend(void)
{
    XframeTx t;
    const size_t mss = 512;
    const size_t payload = 1736;
    const size_t hdr_len = ETH_HDR_LEN + IPV4_HDR_LEN + TCP_HDR_LEN;
    const size_t frame_len = hdr_len + payload;
    g_autofree uint8_t *frame = g_malloc0(frame_len);
    g_autofree uint8_t *mirrored = g_malloc0(sizeof(struct virtio_net_hdr) +
                                             frame_len);
    struct virtio_net_hdr *vhdr = (struct virtio_net_hdr *)mirrored;
    const uint8_t *got = mirrored + sizeof(*vhdr);
    uint32_t vnet_hdr_len;
    size_t len;
    int tap_fd, inject_fd;

    build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_TCP, payload,
                     frame_len);
    frame[ETH_HDR_LEN + IPV4_HDR_LEN + 13] = 0x19; /* FIN|PSH|ACK */

    if (!xframe_vnet_tap_start(&t, &tap_fd, &inject_fd)) {
        return;
    }

    qtest_memwrite(t.qs->qts, TRAFFIC_TX_BUF, frame, frame_len);
    dma_stq_be(t.qs->qts, TRAFFIC_TXD_PAR,
               (uint64_t)frame_len | TXD_OWN | TXD_GCF | TXD_GCL |
               TXD_LSO_EN | TXD_LSO_MSS(mss));
    dma_stq_be(t.qs->qts, TRAFFIC_TXD_PAR + 8, 0);
    dma_stq_be(t.qs->qts, TRAFFIC_TXD_PAR + 16, TRAFFIC_TX_BUF);
    dma_stq_be(t.qs->qts, TRAFFIC_TXD_PAR + 24, 0);
    qpci_io_writeq(t.dev, t.txp, TXP_TXDL_PAR, TRAFFIC_TXD_PAR);
    qpci_io_writeq(t.dev, t.txp, TXP_TXDL_LCR,
                   TXDL_NUMTXD(0) | TXDL_LGC_FIRST | TXDL_LGC_LAST);

    len = mirror_recv_packet(t.host_fd, mirrored,
                             sizeof(struct virtio_net_hdr) + frame_len,
                             &vnet_hdr_len);
    g_assert_cmpuint(vnet_hdr_len, ==, sizeof(*vhdr));
    g_assert_cmpuint(len, ==, sizeof(*vhdr) + frame_len);
    g_assert_cmphex(vhdr->flags, ==, VIRTIO_NET_HDR_F_NEEDS_CSUM);
    g_assert_cmphex(vhdr->gso_type, ==, VIRTIO_NET_HDR_GSO_TCPV4);
    g_assert_cmpuint(vhdr->hdr_len, ==, hdr_len);
    g_assert_cmpuint(vhdr->gso_size, ==, mss);
    g_assert_cmpuint(vhdr->csum_start, ==,
                     ETH_HDR_LEN + IPV4_HDR_LEN);
    g_assert_cmpuint(vhdr->csum_offset, ==, 16);

    g_assert_cmpmem(got, TCP_CSUM_OFF, frame, TCP_CSUM_OFF);
    g_assert_cmphex(frame_be16(got, TCP_CSUM_OFF), ==,
                    frame_ipv4_pseudo_checksum_seed(frame,
                                                    TCP_HDR_LEN + payload));
    g_assert_cmpmem(got + TCP_CSUM_OFF + 2,
                    frame_len - TCP_CSUM_OFF - 2,
                    frame + TCP_CSUM_OFF + 2,
                    frame_len - TCP_CSUM_OFF - 2);
    g_assert_cmphex(got[ETH_HDR_LEN + IPV4_HDR_LEN + 13], ==, 0x19);
    g_assert_cmphex(dma_ldq_be(t.qs->qts, TRAFFIC_TXD_PAR) & TXD_OWN, ==, 0);
    net_socket_expect_no_frame(t.host_fd);

    sock_pif_writeq(&t, STAT_ADDR, STAT_BLOCK_ADDR);
    sock_pif_writeq(&t, STAT_CFG, STAT_CFG_EN | STAT_CFG_ONE_SHOT);
    g_assert_cmpuint(dma_ldl_be(t.qs->qts,
                                STAT_BLOCK_ADDR + STAT_TMAC_FRMS), ==, 4);
    g_assert_cmpuint(dma_ldl_be(t.qs->qts,
                                STAT_BLOCK_ADDR + STAT_TMAC_DATA_OCTETS), ==,
                     payload + 4 * hdr_len);

    xframe_tx_stop(&t);
    close(tap_fd);
    close(inject_fd);
}

static void test_transmit_adds_vnet_header_to_ordinary_frame(void)
{
    XframeTx t;
    uint8_t frame[64];
    uint8_t mirrored[sizeof(struct virtio_net_hdr) + sizeof(frame)];
    const struct virtio_net_hdr empty_vhdr = { };
    uint32_t vnet_hdr_len;
    size_t len;
    int tap_fd, inject_fd;

    build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_UDP, 10,
                     sizeof(frame));

    if (!xframe_vnet_tap_start(&t, &tap_fd, &inject_fd)) {
        return;
    }

    xframe_program_txd(&t, TRAFFIC_TXD_PAR, TRAFFIC_TX_BUF,
                       frame, sizeof(frame));
    xframe_ring_doorbell(&t, TRAFFIC_TXD_PAR);

    len = mirror_recv_packet(t.host_fd, mirrored, sizeof(mirrored),
                             &vnet_hdr_len);
    g_assert_cmpuint(vnet_hdr_len, ==, sizeof(empty_vhdr));
    g_assert_cmpuint(len, ==, sizeof(mirrored));
    g_assert_cmpmem(mirrored, sizeof(empty_vhdr),
                    &empty_vhdr, sizeof(empty_vhdr));
    g_assert_cmpmem(mirrored + sizeof(empty_vhdr), sizeof(frame),
                    frame, sizeof(frame));
    g_assert_cmphex(dma_ldq_be(t.qs->qts, TRAFFIC_TXD_PAR) & TXD_OWN, ==, 0);
    net_socket_expect_no_frame(t.host_fd);

    xframe_tx_stop(&t);
    close(tap_fd);
    close(inject_fd);
}

static void test_transmit_ordinary_frame_to_non_vnet_tap(void)
{
    XframeTx t;
    uint8_t frame[64];
    uint8_t got[sizeof(frame)];
    size_t len;
    int tap_fd, inject_fd;

    build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_UDP, 10,
                     sizeof(frame));

    if (!xframe_tap_start(&t, &tap_fd, &inject_fd, false)) {
        return;
    }

    xframe_program_txd(&t, TRAFFIC_TXD_PAR, TRAFFIC_TX_BUF,
                       frame, sizeof(frame));
    xframe_ring_doorbell(&t, TRAFFIC_TXD_PAR);

    len = traffic_recv_frame(t.host_fd, got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));
    g_assert_cmphex(dma_ldq_be(t.qs->qts, TRAFFIC_TXD_PAR) & TXD_OWN, ==, 0);
    net_socket_expect_no_frame(t.host_fd);

    xframe_tx_stop(&t);
    close(tap_fd);
    close(inject_fd);
}

static void test_receive_pads_after_vnet_header(void)
{
    XframeTx t;
    uint8_t packet[ETH_ZLEN] = { };
    uint8_t *frame = packet + sizeof(struct virtio_net_hdr);
    uint8_t landed[ETH_ZLEN];
    size_t frame_len = sizeof(packet) - sizeof(struct virtio_net_hdr);
    int tap_fd, inject_fd;
    unsigned i;

    /*
     * TAP pads this exact shape as header plus frame. Once the device removes
     * the negotiated header, the Ethernet packet still needs its own padding.
     */
    for (i = 0; i < frame_len; i++) {
        frame[i] = (uint8_t)(0x80 + i);
    }
    memcpy(frame, xframe_station_mac, sizeof(xframe_station_mac));

    if (!xframe_vnet_tap_start(&t, &tap_fd, &inject_fd)) {
        return;
    }

    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF0, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);
    memset(landed, 0xa5, sizeof(landed));
    qtest_memwrite(t.qs->qts, RX_BUF0, landed, sizeof(landed));

    redirector_send_packet(inject_fd, packet,
                           sizeof(struct virtio_net_hdr),
                           sizeof(struct virtio_net_hdr));
    redirector_send_packet(inject_fd, packet, sizeof(packet),
                           sizeof(struct virtio_net_hdr));
    g_assert_true(xframe_wait_rxd_consumed(&t, RX_RING_BASE, 5000));
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 8) & RXD_OWN, ==, 0);
    g_assert_cmphex(dma_ldq_be(t.qs->qts, RX_RING_BASE + 16) >>
                    RXD_BUFSIZ_SHIFT, ==, ETH_ZLEN);
    qtest_memread(t.qs->qts, RX_BUF0, landed, sizeof(landed));
    g_assert_cmpmem(landed, frame_len, frame, frame_len);
    for (i = frame_len; i < sizeof(landed); i++) {
        g_assert_cmphex(landed[i], ==, 0);
    }

    xframe_tx_stop(&t);
    close(tap_fd);
    close(inject_fd);
}
#endif

/*
 * An LSO buffer is a whole IP datagram, often about 32 KB from IRIX, above
 * the 9600-byte jumbo frame limit. Clamping it truncates the datagram; its IP
 * total length then exceeds the remaining buffer, so segmentation fails and a
 * malformed 9600-byte frame escapes. Four such frames claimed about 32 KB and
 * had bad checksums. This test sends an oversized datagram and requires every
 * byte in correctly sized segments.
 */
static void test_transmit_segments_a_send_larger_than_a_frame(void)
{
    XframeTx t;
    const size_t mss = 1460;
    const size_t payload = 20000;         /* well past XFRAME_MAX_FRAME */
    const size_t hdr = ETH_HDR_LEN + IPV4_HDR_LEN + TCP_HDR_LEN;
    g_autofree uint8_t *frame = g_malloc0(hdr + payload);
    /*
     * Large enough to hold an unsegmented buffer, so an oversized frame is
     * caught by this test's own size assertion rather than by the receive
     * helper running out of room.
     */
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

        /*
         * Nothing oversized: every frame is headers plus at most one segment.
         */
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
 * Receive control word 0 reports classification and verified checksums.
 * Drivers may trust it and skip software checking. Zero checksum flags mean
 * unverified. A valid frame must report verified; corrupt L4 checksum must
 * remain classified, report L4 unverified, and keep the intact IP header
 * verified.
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
    /*
     * On the descriptor, not on RX_TRAFFIC_INT: that bit is sticky across
     * deliveries until acknowledged, so the second frame below would find it
     * already set and read a descriptor whose arrival deadline has not
     * passed.
     */
    g_assert_true(xframe_wait_rxd_consumed(&t, RX_RING_BASE, 5000));

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
    g_assert_true(xframe_wait_rxd_consumed(&t, RX_RING_BASE, 5000));

    c1 = dma_ldq_be(t.qs->qts, RX_RING_BASE + 8);
    g_assert_cmphex(c1 & RXD_PROTO_TCP, ==, RXD_PROTO_TCP);
    g_assert_cmphex((c1 >> 16) & 0xffff, ==, RXD_CKSUM_OK);
    g_assert_cmphex(c1 & 0xffff, !=, RXD_CKSUM_OK);

    xframe_tx_stop(&t);
}

/*
 * At MTU 9000 IRIX uses mode-2 RxDs: three buffers, split headers, and a
 * 64-byte stride instead of 32. A one-buffer walker reads wrong words, finds
 * no owned descriptor, and silently stops receive while the interface remains
 * UP/RUNNING. A frame larger than the first two buffers must split in order
 * across Ethernet-header 14, upper-header 128, and payload buffers, with each
 * completion count correct.
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

    /*
     * A five-buffer descriptor armed the minimal way: three sizes, three
     * pointers, overflow buffers unposted (zero).
     */
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
     * This frame is not IPv4 at all, so the card recognises no L3 header to
     * place: the Ethernet header goes to buffer 0 and the whole remainder to
     * buffer 2, leaving buffer 1 empty. An IPv4 frame is split differently;
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
 * For classified IPv4/TCP, buffer 0 gets Ethernet, buffer 1 IP/transport
 * headers, and buffer 2 payload. IRIX checksum offload reads headers from
 * buffer 1. A Linux-style empty dummy buffer 1 reassembles bytes but makes
 * IRIX drop every segment. Assert header positions, payload, and offload
 * status.
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
    const size_t l34 = IPV4_HDR_LEN + TCP_HDR_LEN;   /* belongs in buffer 1 */
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

    /*
     * The classification and checksum results are reported (offload is on in
     * this mode, unlike the earlier check that disabled it).
     */
    ctrl1 = dma_ldq_be(t.qs->qts, RX_RING_BASE + 8);
    g_assert_cmphex(ctrl1 & RXD_PROTO_TCP, ==, RXD_PROTO_TCP);
    g_assert_cmphex((ctrl1 >> 16) & 0xffff, ==, RXD_CKSUM_OK);   /* IP hdr */
    g_assert_cmphex(ctrl1 & 0xffff, ==, RXD_CKSUM_OK);           /* TCP */

    xframe_tx_stop(&t);
}

/*
 * An IPv4 frame whose TCP header the IP total length cuts short. The card
 * cannot classify the transport, so it reports IPv4 and the L3 checksum
 * verdict alone, and buffer 1 gets only the IP header. Other IPv4 frames
 * follow the same rule; no guest run or manual covers this case. This pins
 * the model's choice, and it is a choice: real hardware may
 * do something else with a header it cannot parse.
 */
static void test_receive_five_buffer_splits_truncated_transport(void)
{
    XframeTx t;
    uint8_t frame[128];
    uint8_t landed[128];
    const uint64_t b0 = RX_BUF0;
    const uint64_t b1 = RX_BUF0 + 0x1000;
    const uint64_t b2 = RX_BUF0 + 0x2000;
    const size_t c0 = 14, c1 = 128, c2 = 4096;   /* as IRIX posts them */
    const size_t ip_total = IPV4_HDR_LEN + 16;   /* 16 < a TCP header */
    uint16_t csum;
    uint64_t ctrl1, ctrl2;
    uint8_t *ip = frame + ETH_HDR_LEN;
    size_t len = 74;

    /*
     * A whole TCP frame, then an IP total length that stops inside the TCP
     * header. The bytes past it are Ethernet padding as far as IP is
     * concerned.
     */
    (void)build_ipv4_frame(frame, xframe_station_mac, IPPROTO_TCP, 20, len);
    ip[2] = (uint8_t)(ip_total >> 8);
    ip[3] = (uint8_t)ip_total;
    ip[10] = 0x00;
    ip[11] = 0x00;
    csum = frame_csum_fold(frame_csum_add(0, ip, IPV4_HDR_LEN));
    ip[10] = (uint8_t)(csum >> 8);
    ip[11] = (uint8_t)csum;

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

    ctrl2 = dma_ldq_be(t.qs->qts, RX_RING_BASE + 16);
    g_assert_cmpuint(RXD5_GET_B0(ctrl2), ==, c0);
    g_assert_cmpuint(RXD5_GET_B1(ctrl2), ==, IPV4_HDR_LEN);
    g_assert_cmpuint(RXD5_GET_B2(ctrl2), ==, len - c0 - IPV4_HDR_LEN);

    qtest_memread(t.qs->qts, b1, landed, IPV4_HDR_LEN);
    g_assert_cmpmem(landed, IPV4_HDR_LEN, ip, IPV4_HDR_LEN);
    qtest_memread(t.qs->qts, b2, landed, len - c0 - IPV4_HDR_LEN);
    g_assert_cmpmem(landed, len - c0 - IPV4_HDR_LEN,
                    frame + c0 + IPV4_HDR_LEN, len - c0 - IPV4_HDR_LEN);

    ctrl1 = dma_ldq_be(t.qs->qts, RX_RING_BASE + 8);
    g_assert_cmphex(ctrl1 & (RXD_PROTO_IPV4 | RXD_PROTO_TCP | RXD_PROTO_UDP),
                    ==, RXD_PROTO_IPV4);
    g_assert_cmphex((ctrl1 >> 16) & 0xffff, ==, RXD_CKSUM_OK);
    g_assert_cmphex(ctrl1 & 0xffff, ==, 0);

    xframe_tx_stop(&t);
}

/*
 * IPv4 fragments still have a complete IP header even though they do not have
 * a complete transport segment to classify or verify. Five-buffer mode must
 * put that header in buffer 1 for every fragment, leaving each fragment's IP
 * payload in buffer 2. Consecutive descriptors and distinct buffers make this
 * cover both the initial fragment (which starts with the UDP header) and the
 * later fragment without allowing one completion to hide the other's layout.
 */
static void test_receive_five_buffer_splits_ipv4_fragments(void)
{
    enum {
        UDP_PAYLOAD_LEN = 2012,
        FIRST_IP_PAYLOAD_LEN = 1480,
        SECOND_IP_PAYLOAD_LEN = UDP_HDR_LEN + UDP_PAYLOAD_LEN -
                                FIRST_IP_PAYLOAD_LEN,
        FIRST_FRAME_LEN = ETH_HDR_LEN + IPV4_HDR_LEN + FIRST_IP_PAYLOAD_LEN,
        SECOND_FRAME_LEN = ETH_HDR_LEN + IPV4_HDR_LEN + SECOND_IP_PAYLOAD_LEN,
    };
    XframeTx t;
    uint8_t datagram[ETH_HDR_LEN + IPV4_HDR_LEN + UDP_HDR_LEN +
                     UDP_PAYLOAD_LEN];
    uint8_t fragments[2][FIRST_FRAME_LEN];
    uint8_t landed[FIRST_IP_PAYLOAD_LEN];
    const uint64_t buffers[2][3] = {
        { RX_BUF0, RX_BUF0 + 0x1000, RX_BUF0 + 0x2000 },
        { RX_BUF0 + 0x5000, RX_BUF0 + 0x6000, RX_BUF0 + 0x7000 },
    };
    const size_t frame_len[2] = { FIRST_FRAME_LEN, SECOND_FRAME_LEN };
    const size_t ip_payload_len[2] = {
        FIRST_IP_PAYLOAD_LEN, SECOND_IP_PAYLOAD_LEN,
    };
    const size_t c0 = ETH_HDR_LEN, c1 = 128, c2 = 9738;
    uint64_t ctrl1, ctrl2, desc;
    unsigned i;

    build_ipv4_frame(datagram, xframe_station_mac, IPPROTO_UDP,
                     UDP_PAYLOAD_LEN, sizeof(datagram));

    memcpy(fragments[0], datagram, FIRST_FRAME_LEN);
    frame_set_ipv4_fragment(fragments[0], FIRST_IP_PAYLOAD_LEN, 0x2000);

    memcpy(fragments[1], datagram, ETH_HDR_LEN + IPV4_HDR_LEN);
    memcpy(fragments[1] + ETH_HDR_LEN + IPV4_HDR_LEN,
           datagram + ETH_HDR_LEN + IPV4_HDR_LEN + FIRST_IP_PAYLOAD_LEN,
           SECOND_IP_PAYLOAD_LEN);
    frame_set_ipv4_fragment(fragments[1], SECOND_IP_PAYLOAD_LEN,
                            FIRST_IP_PAYLOAD_LEN / 8);

    g_assert_cmpuint(frame_len[0], ==, 1514);
    g_assert_cmpuint(frame_len[1], ==, 574);
    g_assert_cmpuint(frame_be16(fragments[0], ETH_HDR_LEN + 2), ==, 1500);
    g_assert_cmphex(frame_be16(fragments[0], ETH_HDR_LEN + 6), ==, 0x2000);
    g_assert_cmpuint(frame_be16(fragments[1], ETH_HDR_LEN + 2), ==, 560);
    g_assert_cmphex(frame_be16(fragments[1], ETH_HDR_LEN + 6), ==,
                    FIRST_IP_PAYLOAD_LEN / 8);
    g_assert_true(ipv4_checksum_valid(fragments[0]));
    g_assert_true(ipv4_checksum_valid(fragments[1]));

    xframe_tx_start(&t);
    for (i = 0; i < 2; i++) {
        desc = RX_RING_BASE + i * RXD5_SIZE;
        dma_stq_be(t.qs->qts, desc + 0, 0);
        dma_stq_be(t.qs->qts, desc + 8, RXD_OWN);
        dma_stq_be(t.qs->qts, desc + 16, RXD5_SET_SIZES(c0, c1, c2));
        dma_stq_be(t.qs->qts, desc + 24, buffers[i][0]);
        dma_stq_be(t.qs->qts, desc + 32, buffers[i][1]);
        dma_stq_be(t.qs->qts, desc + 40, buffers[i][2]);
    }
    sock_pif_writeq(&t, PRC_RXD0_0, RX_RING_BASE);
    sock_pif_writeq(&t, PRC_CTRL_0, RC_IN_SVC | RC_RING_MODE_5BUF);

    for (i = 0; i < 2; i++) {
        desc = RX_RING_BASE + i * RXD5_SIZE;
        net_socket_send_frame(t.host_fd, fragments[i], frame_len[i]);
        g_assert_true(xframe_wait_rxd_consumed(&t, desc, 5000));

        ctrl2 = dma_ldq_be(t.qs->qts, desc + 16);
        g_assert_cmpuint(RXD5_GET_B0(ctrl2), ==, ETH_HDR_LEN);
        g_assert_cmpuint(RXD5_GET_B1(ctrl2), ==, IPV4_HDR_LEN);
        g_assert_cmpuint(RXD5_GET_B2(ctrl2), ==, ip_payload_len[i]);

        qtest_memread(t.qs->qts, buffers[i][0], landed, ETH_HDR_LEN);
        g_assert_cmpmem(landed, ETH_HDR_LEN, fragments[i], ETH_HDR_LEN);
        qtest_memread(t.qs->qts, buffers[i][1], landed, IPV4_HDR_LEN);
        g_assert_cmpmem(landed, IPV4_HDR_LEN,
                        fragments[i] + ETH_HDR_LEN, IPV4_HDR_LEN);
        qtest_memread(t.qs->qts, buffers[i][2], landed, ip_payload_len[i]);
        g_assert_cmpmem(landed, ip_payload_len[i],
                        fragments[i] + ETH_HDR_LEN + IPV4_HDR_LEN,
                        ip_payload_len[i]);

        ctrl1 = dma_ldq_be(t.qs->qts, desc + 8);
        g_assert_cmphex(ctrl1 & RXD_PROTO_IPV4, ==, RXD_PROTO_IPV4);
        g_assert_cmphex(ctrl1 & RXD_PROTO_IP_FRAG, ==, RXD_PROTO_IP_FRAG);
        g_assert_cmphex(ctrl1 & (RXD_PROTO_TCP | RXD_PROTO_UDP), ==, 0);
        g_assert_cmphex((ctrl1 >> 16) & 0xffff, ==, RXD_CKSUM_OK);
        g_assert_cmphex(ctrl1 & 0xffff, ==, 0);
    }

    xframe_tx_stop(&t);
}

/*
 * The classified header split at jumbo size: a 9014-byte TCP frame, the exact
 * shape the peer sends during a bulk transfer at MTU 9000 and the first frame
 * size that genuinely needs buffer 2's room (the handshake fits buffers 0 and
 * 1). In the live guest failure, data frames were retransmitted forever
 * without acknowledgement while a small classified frame passed, so
 * the small test alone is not sufficient coverage.
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
     * Completion counts for unused buffers 3/4 must be zero; driver
     * bookkeeping above them stays intact. Leaving posted sizes adds 8 bytes
     * to the frame length, making IRIX reject every jumbo frame as an input
     * error despite correct payload bytes.
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
 * IRIX derives interface counters from successive statistics DMA snapshots.
 * The block must exist, advance with traffic, and never regress. Without DMA,
 * netstat -in showed Opkts 4294967284 and falling: the driver subtracted
 * stale memory from its prior snapshot.
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
     * whole-32-bit read decodes to the true count, and a small value keeps
     * its three high-order bytes zero. Reading it any other way byte-reverses
     * it.
     */
    g_assert_cmpuint(dma_ldl_be(t.qs->qts,
                                STAT_BLOCK_ADDR + STAT_TMAC_FRMS) >> 8, ==, 0);

    xframe_tx_stop(&t);
}

/*
 * The DMA covers exactly the hardware span [0, 0x320) and nothing past it.
 * Fill the block area and one guard byte with 0xff, drive traffic so the four
 * counters are non-zero, and require the whole span to come back zero apart
 * from those four words while the byte at 0x320 stays 0xff. This fails red if
 * the write is ever shortened again (the overflow offsets and size were once
 * 16 bytes early, leaving the block's tail unwritten) or if it runs long.
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
        bool counter = (i < STAT_TMAC_WORD + 8) ||
                       (i >= STAT_RMAC_WORD && i < STAT_RMAC_WORD + 8);
        if (!counter) {
            g_assert_cmphex(block[i], ==, 0);
        }
    }

    xframe_tx_stop(&t);
}

/*
 * Each 64-bit statistics word stores frames in its high half; the swapper
 * orders the whole word. Public s2io declares little-endian tmac_data_octets
 * then tmac_frms under STATS_FE|STATS_SE. IRIX leaves both clear and reads
 * big-endian frames first. QEMU #558 exposed swapped Opkts and byte counts.
 * Check both byte orders for transmit and receive.
 */
static void test_statistics_block_word_order_follows_swapper(void)
{
    XframeTx t;
    uint8_t frame[128];
    uint8_t rx_frame[64];
    uint8_t got[256];
    size_t len;
    unsigned i;

    len = build_ipv4_frame(frame, traffic_remote_mac, IPPROTO_UDP, 64, 106);
    for (i = 0; i < sizeof(rx_frame); i++) {
        rx_frame[i] = (uint8_t)(0x80 + i);
    }
    memcpy(rx_frame, xframe_station_mac, sizeof(xframe_station_mac));

    xframe_tx_start(&t);

    for (i = 0; i < 3; i++) {
        xframe_program_txd(&t, TX_RAM_SCRATCH, TX_RAM_SCRATCH + 0x1000,
                           frame, len);
        xframe_ring_doorbell(&t, TX_RAM_SCRATCH);
        (void)traffic_recv_frame(t.host_fd, got, sizeof(got));
    }
    xframe_program_rxd(&t, RX_RING_BASE, RX_BUF0, RX_BUF_CAPACITY);
    xframe_rx_ring_arm(&t, RX_RING_BASE);
    net_socket_send_frame(t.host_fd, rx_frame, sizeof(rx_frame));
    g_assert_true(xframe_wait_rx_traffic(&t, 5000));

    sock_pif_writeq(&t, STAT_ADDR, STAT_BLOCK_ADDR);
    sock_pif_writeq(&t, STAT_CFG, STAT_CFG_EN | STAT_CFG_ONE_SHOT);
    g_assert_cmphex(dma_ldq_be(t.qs->qts, STAT_BLOCK_ADDR + STAT_TMAC_WORD), ==,
                    3ULL << 32 | (3 * len));
    g_assert_cmphex(dma_ldq_be(t.qs->qts, STAT_BLOCK_ADDR + STAT_RMAC_WORD), ==,
                    1ULL << 32 | sizeof(rx_frame));

    sock_pif_writeq(&t, SWAPPER_CTRL,
                    SWAPPER_CTRL_STATS_FE | SWAPPER_CTRL_STATS_SE);
    sock_pif_writeq(&t, STAT_CFG, STAT_CFG_EN | STAT_CFG_ONE_SHOT);
    g_assert_cmphex(dma_ldq_le(t.qs->qts, STAT_BLOCK_ADDR + STAT_TMAC_WORD), ==,
                    3ULL << 32 | (3 * len));
    g_assert_cmphex(dma_ldq_le(t.qs->qts, STAT_BLOCK_ADDR + STAT_RMAC_WORD), ==,
                    1ULL << 32 | sizeof(rx_frame));

    xframe_tx_stop(&t);
}

/*
 * A one-shot transfer clears the enable bit when its single DMA completes.
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

/*
 * Checksum pass-through on receive: a deliberately wrong L3/L4 checksum (UDP
 * here) must land in the buffer unchanged. The receive path must not
 * recompute it.
 */
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
 * Version 7 gated PIC delivery through GENERAL_INT_MASK. Keep that behavior
 * after loading an old stream, including across a second migration between
 * version-8 destinations.
 */
static void test_interrupt_masking_migration_from_v7(void)
{
    const char *v7_binary = g_getenv("XFRAME_QEMU_V7_BINARY");
    g_autofree char *first_dir = NULL;
    g_autofree char *first_socket = NULL;
    g_autofree char *first_uri = NULL;
    g_autofree char *first_incoming = NULL;
    g_autofree char *second_dir = NULL;
    g_autofree char *second_socket = NULL;
    g_autofree char *second_uri = NULL;
    g_autofree char *second_incoming = NULL;
    XframeTx from, middle, to;

    if (!v7_binary) {
        g_test_skip("XFRAME_QEMU_V7_BINARY is not set");
        return;
    }

    first_dir = xframe_migration_directory();
    first_socket = g_build_filename(first_dir, "migration.sock", NULL);
    first_uri = g_strdup_printf("unix:%s", first_socket);
    first_incoming = g_strdup_printf("-incoming %s", first_uri);

    xframe_tx_start_binary_args(&from, v7_binary, NULL);
    sock_pif_writeq(&from, PIC_INT_MASK, 0);
    sock_pif_writeq(&from, TXPIC_INT_MASK, 0);
    sock_pif_writeq(&from, SCHED_INT_CTRL,
                    SCHED_INT_CTRL_TIMER_EN | SCHED_INT_TEST_PERIOD);
    qtest_clock_step(from.qs->qts, SCHED_INT_STEP_NS);
    g_assert_false(xframe_intx_asserted(from.dev));

    xframe_tx_start_args(&middle, first_incoming);
    xframe_migrate(from.qs->qts, first_uri);
    g_assert_false(xframe_intx_asserted(middle.dev));

    sock_pif_writeq(&middle, GENERAL_INT_MASK, ~GEN_INTR_TXPIC);
    g_assert_true(xframe_intx_asserted(middle.dev));
    sock_pif_writeq(&middle, GENERAL_INT_MASK, DISABLE_ALL_INTRS_64);
    g_assert_false(xframe_intx_asserted(middle.dev));
    g_assert_cmphex(sock_pif_readq(&middle, GENERAL_INT_STATUS) &
                    GEN_INTR_TXPIC, ==, GEN_INTR_TXPIC);

    second_dir = xframe_migration_directory();
    second_socket = g_build_filename(second_dir, "migration.sock", NULL);
    second_uri = g_strdup_printf("unix:%s", second_socket);
    second_incoming = g_strdup_printf("-incoming %s", second_uri);
    xframe_tx_start_args(&to, second_incoming);
    xframe_migrate(middle.qs->qts, second_uri);
    g_assert_false(xframe_intx_asserted(to.dev));
    g_assert_cmphex(sock_pif_readq(&to, GENERAL_INT_STATUS) &
                    GEN_INTR_TXPIC, ==, GEN_INTR_TXPIC);

    sock_pif_writeq(&to, GENERAL_INT_MASK, ~GEN_INTR_TXPIC);
    g_assert_true(xframe_intx_asserted(to.dev));
    sock_pif_writeq(&to, TXPIC_INT_REG, 0);
    g_assert_false(xframe_intx_asserted(to.dev));

    /* Device reset must not discard the migrated compatibility behavior. */
    sock_pif_writeq(&to, SW_RESET, SW_RESET_KEY);
    sock_pif_writeq(&to, PIC_INT_MASK, 0);
    sock_pif_writeq(&to, TXPIC_INT_MASK, 0);
    sock_pif_writeq(&to, SCHED_INT_CTRL,
                    SCHED_INT_CTRL_TIMER_EN | SCHED_INT_TEST_PERIOD);
    qtest_clock_step(to.qs->qts, SCHED_INT_STEP_NS);
    g_assert_false(xframe_intx_asserted(to.dev));
    sock_pif_writeq(&to, GENERAL_INT_MASK, ~GEN_INTR_TXPIC);
    g_assert_true(xframe_intx_asserted(to.dev));
    sock_pif_writeq(&to, TXPIC_INT_REG, 0);
    g_assert_false(xframe_intx_asserted(to.dev));

    xframe_tx_stop(&to);
    xframe_tx_stop(&middle);
    xframe_tx_stop(&from);
    xframe_migration_cleanup(second_dir, second_socket);
    xframe_migration_cleanup(first_dir, first_socket);
}

/*
 * A version-7 GPIO link source remains gated by GENERAL and GPIO after load.
 */
static void test_gpio_interrupt_masking_migration_from_v7(void)
{
    const char *v7_binary = g_getenv("XFRAME_QEMU_V7_BINARY");
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *incoming = NULL;
    XframeTx from, to;

    if (!v7_binary) {
        g_test_skip("XFRAME_QEMU_V7_BINARY is not set");
        return;
    }

    directory = xframe_migration_directory();
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    incoming = g_strdup_printf("-incoming %s", uri);

    xframe_tx_start_binary_args(&from, v7_binary, NULL);
    qtest_qmp_assert_success(from.qs->qts,
                             "{'execute': 'set_link', 'arguments': "
                             "{'name': 'n0', 'up': false}}");
    sock_pif_readq(&from, GPIO_INT_REG);
    sock_pif_writeq(&from, PIC_INT_MASK, ~PIC_INT_GPIO);
    sock_pif_writeq(&from, ADAPTER_CONTROL, ADAPTER_EN);
    qtest_qmp_assert_success(from.qs->qts,
                             "{'execute': 'set_link', 'arguments': "
                             "{'name': 'n0', 'up': true}}");
    qtest_clock_step_next(from.qs->qts);
    g_assert_false(xframe_intx_asserted(from.dev));

    xframe_tx_start_args(&to, incoming);
    xframe_migrate(from.qs->qts, uri);
    g_assert_false(xframe_intx_asserted(to.dev));

    sock_pif_writeq(&to, GENERAL_INT_MASK, ~GEN_INTR_TXPIC);
    g_assert_false(xframe_intx_asserted(to.dev));
    sock_pif_writeq(&to, GPIO_INT_MASK, ~GPIO_INT_MASK_LINK_UP);
    g_assert_true(xframe_intx_asserted(to.dev));
    g_assert_cmphex(sock_pif_readq(&to, GPIO_INT_REG) &
                    GPIO_INT_REG_LINK_UP, ==, GPIO_INT_REG_LINK_UP);
    g_assert_false(xframe_intx_asserted(to.dev));

    xframe_tx_stop(&to);
    xframe_tx_stop(&from);
    xframe_migration_cleanup(directory, socket);
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

/*
 * A migrated continuous timer retains the source's partially elapsed phase.
 */
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
 * transmit-completion interrupt and the raise count equals the frame count.
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

    /* Open the GENERAL and transmit-traffic masks for completion. */
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
#ifdef CONFIG_LINUX
    qtest_add_func("xframe/transmit-offloads-lso-to-vnet-backend",
                   test_transmit_offloads_lso_to_vnet_backend);
    qtest_add_func("xframe/transmit-adds-vnet-header-to-ordinary-frame",
                   test_transmit_adds_vnet_header_to_ordinary_frame);
    qtest_add_func("xframe/transmit-ordinary-frame-to-non-vnet-tap",
                   test_transmit_ordinary_frame_to_non_vnet_tap);
    qtest_add_func("xframe/receive-pads-after-vnet-header",
                   test_receive_pads_after_vnet_header);
#endif
    qtest_add_func("xframe/transmit-segments-a-send-larger-than-a-frame",
                   test_transmit_segments_a_send_larger_than_a_frame);
    qtest_add_func("xframe/receive-reports-checksum-results",
                   test_receive_reports_checksum_results);
    qtest_add_func("xframe/receive-five-buffer-unclassified-frame",
                   test_receive_five_buffer_unclassified_frame);
    qtest_add_func("xframe/receive-five-buffer-splits-classified-headers",
                   test_receive_five_buffer_splits_classified_headers);
    qtest_add_func("xframe/receive-five-buffer-splits-truncated-transport",
                   test_receive_five_buffer_splits_truncated_transport);
    qtest_add_func("xframe/receive-five-buffer-splits-ipv4-fragments",
                   test_receive_five_buffer_splits_ipv4_fragments);
    qtest_add_func("xframe/receive-five-buffer-splits-a-jumbo-frame",
                   test_receive_five_buffer_splits_a_jumbo_frame);
    qtest_add_func("xframe/statistics-block-counts-traffic",
                   test_statistics_block_counts_traffic);
    qtest_add_func("xframe/statistics-block-write-span",
                   test_statistics_block_write_span);
    qtest_add_func("xframe/statistics-block-word-order-follows-swapper",
                   test_statistics_block_word_order_follows_swapper);
    qtest_add_func("xframe/statistics-one-shot-clears-enable",
                   test_statistics_one_shot_clears_enable);
    qtest_add_func("xframe/descriptors-honor-swapper-ctrl-little-endian",
                   test_descriptors_honor_swapper_ctrl_little_endian);
    qtest_add_func("xframe/xg-enable-path-link-via-block-masks",
                   test_xg_enable_path_link_via_block_masks);
    qtest_add_func("xframe/xg-bringup-pic-summary-serviced",
                   test_xg_bringup_pic_summary_serviced);
    qtest_add_func("xframe/scheduled-interrupt-feeds-watchdog",
                   test_scheduled_interrupt_feeds_watchdog);
    qtest_add_func("xframe/gpio-link-up-mask-is-readback-only",
                   test_gpio_link_up_mask_is_readback_only);
    qtest_add_func("xframe/gpio-link-down-mask-is-readback-only",
                   test_gpio_link_down_mask_is_readback_only);
    qtest_add_func("xframe/masked-mac-link-latch-clears-on-read",
                   test_masked_mac_link_latch_clears_on_read);
    qtest_add_func("xframe/redundant-adapter-en-does-not-relink",
                   test_redundant_adapter_en_does_not_relink);
    qtest_add_func("xframe/scheduled-interrupt-holds-carrier-steady",
                   test_scheduled_interrupt_holds_carrier_steady);
    qtest_add_func("xframe/general-mask-is-readback-only",
                   test_general_mask_is_readback_only);
    qtest_add_func("xframe/xg-scheduled-interrupt-acks-via-write",
                   test_xg_scheduled_interrupt_acks_via_write);
    qtest_add_func("xframe/xg-enable-path-transmit-via-block-masks",
                   test_xg_enable_path_transmit_via_block_masks);
    qtest_add_func("xframe/receive-delivers-a-frame",
                   test_receive_delivers_a_frame);
    qtest_add_func("xframe/receive-waits-for-arrival-deadline",
                   test_receive_waits_for_its_arrival_deadline);
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
    qtest_add_func("xframe/interrupt-masking-migration-from-v7",
                   test_interrupt_masking_migration_from_v7);
    qtest_add_func("xframe/gpio-interrupt-masking-migration-from-v7",
                   test_gpio_interrupt_masking_migration_from_v7);
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
