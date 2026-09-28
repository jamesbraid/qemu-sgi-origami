/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the Broadcom NetXtreme BCM570x adapter, on the Bridge head.
 *
 * The head is one Bridge on the "none" machine with the memory a card
 * transfers into beside it, under -M none. The card is reached over the chip
 * it ships behind (an Origin has it nowhere else), but
 * nothing here is an Origin 200 claim: no node, no crossbar, no fabric. How
 * the device composes with a populated machine's DMA routes is a separate
 * integration test, sn0-bcm570x-test.c.
 *
 * This ran on an x86 PC until the fork stopped building SGI devices for that
 * target, and the move changes what the golden byte assertions below say. The
 * model resolves the Bridge's plain DMA address space at realize and, behind
 * any Bridge, delivers its DMA integer fields in the driver's own big-endian
 * layout instead of native little-endian, with a 16-bit field in the other
 * half of its 32-bit word (offset xor 2) because the word swap relocates
 * halfwords. The packet payload is copied verbatim. That is the device's
 * documented byte-order contract, and it is the only byte order the product
 * ever presents.
 *
 * The card carries the SGI IRIS Release 2 Gigabit board's PCI four-tuple:
 * IRIX's tg driver registers on the raw Broadcom primary vendor/device and
 * then, at attach, insists the card also carry SGI's subsystem identity, so
 * the test asserts the whole four-tuple, plus the two indirect config-space
 * windows the driver uses before it ever touches the BAR (the device's three
 * config-space access paths).
 *
 * Byte order is asserted as a golden fixed-byte-layout: on this host a status
 * flag of 0x00000001 lands in guest memory as bytes 00 00 00 01. A
 * self-round-trip through the model cannot see the swap direction
 * (swap-on-write and swap-on-read cancel), so every DMA byte-order check
 * reads the raw bytes and asserts the exact placement.
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "libqtest.h"
#include "libqos/pci-sgi-bridge.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "hw/pci/pci_regs.h"
#include "standard-headers/linux/virtio_net.h"
#include "sgi-migrate.h"
#include <sys/socket.h>
#include <netinet/in.h>
#ifdef CONFIG_LINUX
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#endif

/*
 * The Bridge decodes eight slots and the head places nothing else on them, so
 * the card sits where it does on the base I/O board's first free slot.
 */
#define BCM_SLOT                4
#define BCM_DEVFN               QPCI_DEVFN(BCM_SLOT, 0)

/* The base machine and network backend shared by most tests. */
#define BCM_MACHINE             "-nodefaults -M none " QPCI_SGI_BRIDGE_HEAD

/* The single-port SGI IRIS Release 2 board's four-tuple (bcm5701, copper). */
#define BCM_VENDOR_ID           0x14e4
#define BCM_DEVICE_ID           0x1645
#define BCM_SUBSYS_VENDOR_ID    0x10a9
#define BCM_SUBSYS_ID           0x8010

/* The indirect-access config registers. */
#define BCM_PCI_REG_BASEADDR    0x78
#define BCM_PCI_MEMWIN_BASEADDR 0x7c
#define BCM_PCI_REG_DATA        0x80
#define BCM_PCI_MEMWIN_DATA     0x84

/* BAR0 layout: register file low, on-chip SRAM window at 0x8000. */
#define BCM_SRAM_WINDOW         0x8000u

/* Register-file offsets and bits the bring-up polls exercise. */
#define BCM_MAC_MODE            0x0400u
#define BCM_MAC_ADDR_0_HIGH     0x0410u
#define BCM_MAC_ADDR_0_LOW      0x0414u
#define BCM_RX_MODE             0x0468u
#define BCM_RXMODE_ENABLE       0x00000002u
#define BCM_HCC_MODE            0x3c00u
#define BCM_MISC_CFG            0x6804u
#define BCM_MODE_RESET          0x00000001u
#define BCM_HCC_ENABLE          0x00000002u
#define BCM_MISCCFG_RESET       0x00000001u

/*
 * The buffer-manager mode register (BUFMGR_MODE in tg3, BGE_BMAN_MODE in
 * bge). ENABLE is the enable-request bit; ATTN_ENABLE arms its attention. The
 * public drivers set ENABLE and poll for it to stay SET, but IRIX tg sets
 * ENABLE and polls for it to CLEAR. The chip acknowledges the request by
 * dropping the bit, so the model self-clears ENABLE while ATTN_ENABLE
 * persists.
 */
#define BCM_BUFMGR_MODE         0x4400u
#define BCM_BUFMGR_ENABLE       0x00000002u
#define BCM_BUFMGR_ATTN         0x00000004u

/* The firmware-ready handshake in on-chip SRAM. */
#define BCM_SRAM_FW_MBOX        0x0b50u
#define BCM_FW_MAGIC            0x4b657654u
#define BCM_FW_HANDSHAKE_DONE   0xb49a89abu     /* ~BCM_FW_MAGIC */

/* The firmware-progress register the IRIX driver polls after reset. */
#define BCM_FW_PROGRESS         0x5034u
#define BCM_FW_PROGRESS_INIT    0xffffffffu

/* Config 0x68 (misc host control): bits 31:16 hold the chip id. */
#define BCM_PCI_MISC_CTL        0x68
#define BCM_CHIPID_BCM5701      0x0105u
#define BCM_ASICREV_BCM5701     0x0u            /* chip id >> 12 */

/* The MII management registers and the PHY-access command bits. */
#define BCM_MI_COMM             0x044cu
#define BCM_MI_STS              0x0450u
#define BCM_MICOMM_BUSY         0x20000000u
#define BCM_MICMD_READ          0x08000000u
#define BCM_MICMD_WRITE         0x04000000u
#define BCM_MICOMM_REG_SHIFT    16
#define BCM_MICOMM_PHY_SHIFT    21
#define BCM_MISTS_LINK          0x00000001u

/* Standard MII register numbers and the bits this test reads. */
#define MII_BMCR                0x00u
#define MII_BMSR                0x01u
#define MII_PHYIDR1             0x02u
#define MII_PHYIDR2             0x03u
#define MII_ANAR                0x04u
#define MII_BMCR_AUTOEN         0x1000u
#define MII_BMCR_STARTNEG       0x0200u
#define MII_BMSR_ANEGDONE       0x0020u
#define MII_BMSR_LINK           0x0004u
#define BCM_PHY_IDR1_VALUE      0x0020u
#define BCM_PHY_IDR2_BCM5701    0x6110u

/*
 * The Broadcom auxiliary status summary register (BRGPHY_MII_AUXSTS, 0x19)
 * the copper link-up path reads for speed and duplex. Its AN_RES field
 * (0x0700) carries the negotiated result alongside the autoneg-complete
 * and link bits. 0x0700 is 1000baseT full duplex.
 */
#define BRGPHY_AUXSTS           0x19u
#define BRGPHY_AUXSTS_ACOMP     0x8000u
#define BRGPHY_AUXSTS_AN_RES    0x0700u
#define BRGPHY_AUXSTS_1000TFDX  0x0700u

/* Any PHY address answers: the integrated PHY is the only one on the bus. */
#define BCM_PHY_ADDR            1u

/*
 * How long the modelled part negotiates for, from bcm570x_phy.h. A test that
 * wants the settled link steps the clock past it.
 */
#define BCM_PHY_SETTLE_NS       (100 * 1000 * 1000)

/* Transmit ring, status block, and DMA plumbing. */
#define BCM_MBX_TX_PROD_LO      0x0304u
#define BCM_HCC_STATUSBLK_HI    0x3c38u
#define BCM_HCC_STATUSBLK_LO    0x3c3cu

/*
 * The modelled minimum between a frame arriving and its completion, in
 * nanoseconds: the device default. Delivery is published only once this much
 * virtual time has passed, so every wait for a completion has to advance the
 * clock as well as give the main loop a pass.
 */
#define RX_ARRIVAL_LATENCY_NS  200000
#define BCM_HCC_RX_COAL_TICKS   0x3c08u        /* RX coalescing: us */
#define BCM_HCC_TX_COAL_TICKS   0x3c0cu
#define BCM_HCC_TX_MAX_COAL_BDS 0x3c14u
#define BCM_HCC_RX_MAX_COAL_BDS 0x3c10u        /* RX coalescing: BDs */
#define BCM_MODE_CTL            0x6800u
#define BCM_DMA_SWAP_OPTIONS    0x36u          /* MODE_CTL swap arm */
#define BCM_MODE_HOST_SEND_BDS  (1u << 17)
#define BCM_SRAM_SEND_RING_RCB  0x0100u
#define BCM_TXBDFLAG_TCP_UDP_CKSUM 0x0001u
#define BCM_TXBDFLAG_IP_CKSUM   0x0002u
#define BCM_TXBDFLAG_END        0x0004u
#define BCM_TXBDFLAG_INSERT_SRC_ADDR 0x1000u
#define BCM_TXBDFLAG_CHOOSE_SRC_ADDR(n) ((n) << 13)
#define BCM_TXBDFLAG_LEGACY_SEGMENTATION_BITS 0x7000u
#define BCM_MAC_ADDR_STRIDE     8u
#define BCM_SOURCE_TX_RING_LEN  8u
#define BCM_STATFLAG_UPDATED    0x00000001u
#define BCM_STATUS_BLK_SIZE     0x50
/*
 * Status-block idx[0]: rx_producer at 0x10, tx_consumer at 0x12 (tg3 layout).
 */
#define BCM_STATUS_RX_PROD_OFF  16
#define BCM_STATUS_TX_CONS_OFF  18
#define BCM_STATUS_TAG_OFF       4      /* incremented per status DMA */

/* Receive ring. */
#define BCM_MBX_RX_STD_PROD_LO  0x026cu
#define BCM_MBX_RX_CONS0_LO     0x0284u
#define BCM_RX_STD_RCB_HI       0x2450u
#define BCM_RX_STD_RCB_LO       0x2454u
#define BCM_RX_STD_RCB_MAXLEN   0x2458u
#define BCM_SRAM_RX_RETURN_RCB  0x0200u
/* nicNoMoreRXBDs, PG105 page 110 Table 49 / page 114 Table 54. */
#define BCM_SRAM_STATS_NO_MORE_RXBDS 0x0898u
#define BCM_TXBD_SIZE           16
#define BCM_RXBD_SIZE           32
#define BCM_RXBD_LEN            0x08   /* idx_len: length in low half */
#define BCM_RXBD_IDX            0x0a   /* idx_len: ring index in high half */
#define BCM_RXBD_FLAGS          0x0c   /* type_flags: flags in low half */
#define BCM_RX_JUMBO_RCB_HI     0x2440u
#define BCM_RX_JUMBO_RCB_LO     0x2444u
#define BCM_RX_JUMBO_RCB_MAXLEN 0x2448u
#define BCM_MBX_RX_JUMBO_PROD_LO 0x0274u
#define BCM_RXBDFLAG_JUMBO_RING 0x0020u
#define BCM_RXBD_TCP_CSUM       0x10   /* csum: TCP/UDP result in low half */
#define BCM_RXBD_IP_CSUM        0x12   /* csum: IP result in high half */
#define BCM_RXBDFLAG_TCP_UDP_CKSUM 0x0001u
#define BCM_RXBDFLAG_IP_CKSUM   0x0002u
#define BCM_RXBDFLAG_END        0x0004u
#define BCM_CSUM_GOOD           0xffffu /* valid one's-complement sum */
#define BCM_ETH_FCS_LEN         4      /* length includes stripped FCS */

/* Interrupts. */
#define BCM_MBX_IRQ0_LO         0x0204u
#define BCM_PCI_PCISTATE        0x70
#define BCM_PCISTATE_INTR_NA    0x00000002u   /* INTR_NOT_ACTIVE */
#define BCM_MISCCTL_MASK_INTR   0x00000002u   /* MISC_CTL.MASK_PCI_INTR */
#define BCM_MISCCTL_WORDSWAP    0x00000008u   /* MISC_CTL.ENDIAN_WORDSWAP */

/* Dual-port BCM5704: copper four-tuple and the multifunction bit. */
#define BCM_DEVICE_ID_BCM5704   0x1648u
#define BCM_SUBSYS_ID_BCM5704   0x8013u
#define BCM_HEADER_MULTIFUNC    0x80u          /* PCI_HEADER_TYPE multi bit */

/*
 * DMA scratch, inside the sixteen megabytes the head hands a card. A transfer
 * below the Bridge's translated window reaches the fabric at the address it
 * names, so each of these is both a PCI address the card writes to and an
 * address qtest reads back.
 */
#define DMA_BASE                0x02000000ULL
#define DMA_END                 (DMA_BASE + 0x01000000ULL)
#define TX_RING_PHYS            (DMA_BASE + 0x00200000ULL)
#define TX_BUF_PHYS             (DMA_BASE + 0x00210000ULL)
#define STATUS_PHYS             (DMA_BASE + 0x00220000ULL)
#define STD_RING_PHYS           (DMA_BASE + 0x00230000ULL)
#define RET_RING_PHYS           (DMA_BASE + 0x00240000ULL)
#define RX_BUF_PHYS             (DMA_BASE + 0x00250000ULL)
#define STATUS2_PHYS            (DMA_BASE + 0x00260000ULL)
#define TX_RING_LEN            4u             /* test ring descriptors */
#define RX_RING_LEN           4u
/*
 * The standard ring's control block carries the size of the buffer behind
 * each descriptor, not a count of descriptors. A driver at the standard MTU
 * posts something like this; a frame that does not fit belongs on the jumbo
 * ring.
 */
#define STD_BUF_SIZE          1536u
#define JUMBO_RING_PHYS       (DMA_BASE + 0x00260000ULL)
#define JUMBO_BUF_PHYS        (DMA_BASE + 0x00270000ULL)
#define STD_STALE_BUF_PHYS    (DMA_BASE + 0x00280000ULL)
#define JUMBO_STALE_BUF_PHYS  (DMA_BASE + 0x00290000ULL)
#define JUMBO_BUF_SIZE        9216u
/* Past the transfer memory, short of the chip's face: nothing backs it. */
#define UNMAPPED_PHYS         0x04000000ULL
#define COAL_RING_LEN         64u            /* holds a coalescing burst */

/* A ring and buffer area big enough to time a burst rather than a packet. */
#define BENCH_RING_PHYS       (DMA_BASE + 0x00300000ULL)
#define BENCH_BUF_PHYS        (DMA_BASE + 0x00400000ULL)
#define BENCH_RING_LEN        256u
#define BENCH_FRAMES          255u
#define BENCH_FRAME_LEN       1472u
#define BENCH_BUF_STRIDE      1536u
#define BENCH_ROUNDS          40u    /* enough frames to measure a rate */

/*
 * A live card: the PCI bus, the device at its slot, and BAR0 mapped and
 * enabled for MMIO and bus-master DMA. Config-space-only tests still map the
 * BAR (harmless); it is required for the register-file MMIO path.
 */
typedef struct {
    QTestState *qts;
    QPCIBus *bus;
    QPCIDevice *dev;
    QPCIBar bar;
} Bcm;

static void bcm_open_at(Bcm *b, QTestState *qts, unsigned devfn)
{
    b->qts = qts;
    b->bus = qpci_init_sgi_bridge(qts, NULL);
    b->dev = qpci_device_find(b->bus, devfn);
    g_assert_nonnull(b->dev);
    qpci_device_enable(b->dev);
    b->bar = qpci_iomap(b->dev, 0, NULL);
}

static void bcm_open(Bcm *b, QTestState *qts)
{
    bcm_open_at(b, qts, BCM_DEVFN);
}

static void bcm_close(Bcm *b)
{
    qpci_iounmap(b->dev, b->bar);
    g_free(b->dev);
    qpci_free_sgi_bridge(b->bus);
    qtest_quit(b->qts);
}

static QTestState *bcm_init(void)
{
    /*
     * An explicit netdev, referenced by id, so the card's frontend has a
     * backend. -nodefaults keeps any default NIC or display off the bus.
     */
    return qtest_initf(BCM_MACHINE
                       " -netdev user,id=n0"
                       " -device bcm570x,addr=%02x.0,netdev=n0", BCM_SLOT);
}

/* BAR0 register-file MMIO. */
static uint32_t bar_readl(Bcm *b, uint32_t off)
{
    return qpci_io_readl(b->dev, b->bar, off);
}

static uint8_t bar_readb(Bcm *b, uint32_t off)
{
    return qpci_io_readb(b->dev, b->bar, off);
}

static void bar_writel(Bcm *b, uint32_t off, uint32_t val)
{
    qpci_io_writel(b->dev, b->bar, off, val);
}

static void assert_station_bytes(Bcm *b, const uint8_t expected[8])
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        g_assert_cmphex(bar_readb(b, BCM_MAC_ADDR_0_HIGH + i), ==,
                        expected[i]);
    }
}

static void assert_station_bytes_dev(QPCIDevice *dev, QPCIBar bar,
                                     uint32_t high, uint32_t low)
{
    const uint8_t expected[] = {
        (uint8_t)high, (uint8_t)(high >> 8),
        (uint8_t)(high >> 16), (uint8_t)(high >> 24),
        (uint8_t)low, (uint8_t)(low >> 8),
        (uint8_t)(low >> 16), (uint8_t)(low >> 24),
    };
    unsigned int i;

    for (i = 0; i < 8; i++) {
        g_assert_cmphex(qpci_io_readb(dev, bar,
                                     BCM_MAC_ADDR_0_HIGH + i), ==,
                        expected[i]);
    }
}

/* The indirect register window (config 0x78/0x80) into the register file. */
static void ind_reg_writel_dev(QPCIDevice *dev, uint32_t off, uint32_t val)
{
    qpci_config_writel(dev, BCM_PCI_REG_BASEADDR, off);
    qpci_config_writel(dev, BCM_PCI_REG_DATA, val);
}

static uint32_t ind_reg_readl_dev(QPCIDevice *dev, uint32_t off)
{
    qpci_config_writel(dev, BCM_PCI_REG_BASEADDR, off);
    return qpci_config_readl(dev, BCM_PCI_REG_DATA);
}

/* The indirect memory window (config 0x7C/0x84) into on-chip SRAM. */
static void ind_mem_writel_dev(QPCIDevice *dev, uint32_t off, uint32_t val)
{
    qpci_config_writel(dev, BCM_PCI_MEMWIN_BASEADDR, off);
    qpci_config_writel(dev, BCM_PCI_MEMWIN_DATA, val);
}

static uint32_t ind_mem_readl_dev(QPCIDevice *dev, uint32_t off)
{
    qpci_config_writel(dev, BCM_PCI_MEMWIN_BASEADDR, off);
    return qpci_config_readl(dev, BCM_PCI_MEMWIN_DATA);
}

static void ind_reg_writel(Bcm *b, uint32_t off, uint32_t val)
{
    ind_reg_writel_dev(b->dev, off, val);
}

static uint32_t ind_reg_readl(Bcm *b, uint32_t off)
{
    return ind_reg_readl_dev(b->dev, off);
}

static void ind_mem_writel(Bcm *b, uint32_t off, uint32_t val)
{
    ind_mem_writel_dev(b->dev, off, val);
}

static uint32_t ind_mem_readl(Bcm *b, uint32_t off)
{
    return ind_mem_readl_dev(b->dev, off);
}

/*
 * A PHY register read/write through the MAC's MI_COMM register (needs BAR0).
 */
static uint16_t mii_read(Bcm *b, unsigned phy, unsigned reg)
{
    bar_writel(b, BCM_MI_COMM,
               BCM_MICOMM_BUSY | BCM_MICMD_READ |
               (phy << BCM_MICOMM_PHY_SHIFT) | (reg << BCM_MICOMM_REG_SHIFT));
    return bar_readl(b, BCM_MI_COMM) & 0xffff;
}

static void mii_write(Bcm *b, unsigned phy, unsigned reg, uint16_t val)
{
    bar_writel(b, BCM_MI_COMM,
               BCM_MICOMM_BUSY | BCM_MICMD_WRITE |
               (phy << BCM_MICOMM_PHY_SHIFT) | (reg << BCM_MICOMM_REG_SHIFT) |
               val);
}

/*
 * DMA field access in the order the model delivers behind a Bridge: every
 * 32-bit word big-endian, the driver's own layout. Off a Bridge the same
 * fields are little-endian, which is the whole difference the host makes.
 * These helpers build and decode the descriptors the test exchanges with the
 * device; the golden byte-layout assertions below read the raw bytes to pin
 * the direction the round trip alone cannot see.
 */
static void put32dma(uint8_t *b, uint32_t v)
{
    b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v;
}

/* Big-endian helpers: frame headers are big-endian wherever they appear. */
static void stw_be_p_test(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get16be_test(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get32dma(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

/*
 * A 16-bit DMA field. The word swap puts it in the other half of its 32-bit
 * word from where the little-endian layout does, so its byte offset within
 * the structure is the model's offset xor 2 and the two bytes there are
 * big-endian. Off a Bridge the same field is little-endian at the plain
 * offset.
 */
#define DMA16(off)              ((off) ^ 2u)

static uint16_t get16be(const uint8_t *b)
{
    return ((uint16_t)b[0] << 8) | (uint16_t)b[1];
}

static void put16dma(uint8_t *base, unsigned int off, uint16_t v)
{
    base[DMA16(off)] = v >> 8;
    base[DMA16(off) + 1] = (uint8_t)v;
}

static uint16_t get16dma(const uint8_t *base, unsigned int off)
{
    return get16be(base + DMA16(off));
}

/* True when the card is driving its INTA line, as the driver reads it: the
 * chip clears PCISTATE.INTR_NOT_ACTIVE while the line is asserted. */
static bool intr_line_up(QPCIDevice *dev)
{
    return (qpci_config_readl(dev, BCM_PCI_PCISTATE) & BCM_PCISTATE_INTR_NA)
           == 0;
}

/*
 * The minimum plumbing for the chip to DMA a status block: a send-ring
 * control block with a ring length so a doorbell passes the range guard, and
 * the status block's host address. Host coalescing starts disabled; each
 * test enables it when it wants a status update. The status region is
 * zeroed. A doorbell of producer 0 drives a status-block DMA with no ring
 * walk, which is all the interrupt and gating tests need (no
 * descriptors, no frame).
 */
static void bcm_arm_status(Bcm *b)
{
    ind_mem_writel(b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    qtest_memset(b->qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
}

/* A connected socket backend, so the test reads the frame the engine sent. */
static void net_socket_set_recv_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

static size_t net_socket_recv_frame(int fd, uint8_t *buf, size_t buf_size)
{
    uint32_t hdr;
    size_t len;

    g_assert_cmpint(recv(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    len = ntohl(hdr);
    g_assert_cmpint(len, <=, buf_size);
    g_assert_cmpint(recv(fd, buf, len, 0), ==, (int)len);
    return len;
}

/* A sent frame is already buffered by now; the timeout bounds silence. */
static void net_socket_expect_no_frame(int fd)
{
    uint32_t hdr;

    net_socket_set_recv_timeout(fd, 1);
    errno = 0;
    g_assert_cmpint(recv(fd, &hdr, sizeof(hdr), 0), ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
}

static void net_socket_send_frame(int fd, const uint8_t *buf, size_t len)
{
    uint32_t hdr = htonl((uint32_t)len);

    g_assert_cmpint(send(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    g_assert_cmpint(send(fd, buf, len, 0), ==, (int)len);
}

#ifdef CONFIG_LINUX
static void sock_read_full(int fd, uint8_t *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t got = recv(fd, buf + off, len - off, 0);

        g_assert_cmpint(got, >, 0);
        off += got;
    }
}

static size_t mirror_recv_packet(int fd, uint8_t *buf, size_t buf_size,
                                 uint32_t *vnet_hdr_len)
{
    uint32_t word;
    size_t len;

    sock_read_full(fd, (uint8_t *)&word, sizeof(word));
    len = ntohl(word);
    sock_read_full(fd, (uint8_t *)&word, sizeof(word));
    *vnet_hdr_len = ntohl(word);
    g_assert_cmpuint(len, <=, buf_size);
    sock_read_full(fd, buf, len);
    return len;
}

static bool bcm_vnet_tap_open(Bcm *b, int *tap_fd, int *mirror_fd,
                              int *inject_fd)
{
    struct ifreq ifr = { };
    int mirror[2], inject[2];

    *tap_fd = open("/dev/net/tun", O_RDWR);
    if (*tap_fd < 0) {
        g_test_skip("/dev/net/tun is unavailable");
        return false;
    }

    ifr.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR;
    if (ioctl(*tap_fd, TUNSETIFF, &ifr) < 0) {
        close(*tap_fd);
        *tap_fd = -1;
        g_test_skip("creating a TAP requires CAP_NET_ADMIN");
        return false;
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, mirror), ==, 0);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, inject), ==, 0);
    net_socket_set_recv_timeout(mirror[0], 10);
    *mirror_fd = mirror[0];
    bcm_open(b, qtest_initf(
        BCM_MACHINE " -netdev tap,fd=%d,id=n0"
        " -device bcm570x,addr=%02x.0,netdev=n0"
        " -chardev socket,id=mirror0,fd=%d"
        " -object filter-mirror,id=f0,netdev=n0,queue=rx,outdev=mirror0,"
        "vnet_hdr_support=on"
        " -chardev socket,id=inject0,fd=%d"
        " -object filter-redirector,id=f1,netdev=n0,queue=tx,indev=inject0,"
        "vnet_hdr_support=on",
        *tap_fd, BCM_SLOT, mirror[1], inject[1]));
    *inject_fd = inject[0];
    return true;
}

static void redirector_send_packet(int fd, const uint8_t *buf, size_t len,
                                   uint32_t vnet_hdr_len)
{
    uint32_t packet_len = htonl(len);
    uint32_t header_len = htonl(vnet_hdr_len);
    struct iovec iov[] = {
        { .iov_base = &packet_len, .iov_len = sizeof(packet_len) },
        { .iov_base = &header_len, .iov_len = sizeof(header_len) },
        { .iov_base = (void *)buf, .iov_len = len },
    };
    size_t total = sizeof(packet_len) + sizeof(header_len) + len;

    g_assert_cmpint(iov_send(fd, iov, G_N_ELEMENTS(iov), 0, total), ==,
                    total);
}
#endif

static void wait_for_log_marker(QTestState *qts, const char *logfile,
                                const char *marker)
{
    unsigned int i;

    for (i = 0; i < 100; i++) {
        g_autofree char *log = NULL;

        qtest_clock_step(qts, 1000 * 1000);
        g_usleep(2000);
        if (g_file_get_contents(logfile, &log, NULL, NULL) &&
            strstr(log, marker)) {
            return;
        }
    }
    g_error("timed out waiting for QEMU log marker: %s", marker);
}

/*
 * A socket backend delivers a received frame on a later pass of the main
 * loop, so the status block's receive producer is polled rather than read
 * once.
 */
static void wait_for_rx_prod(Bcm *b, uint16_t want)
{
    uint8_t idx[2];
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        qtest_memread(b->qts, STATUS_PHYS + DMA16(BCM_STATUS_RX_PROD_OFF),
                      idx, 2);
        if (get16be(idx) == want) {
            return;
        }
        g_usleep(5000);
        qtest_clock_step(b->qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    qtest_memread(b->qts, STATUS_PHYS + DMA16(BCM_STATUS_RX_PROD_OFF), idx, 2);
    g_assert_cmphex(get16be(idx), ==, want);
}

/*
 * The status tag the device bumps once per status-block DMA, so its delta
 * over a burst counts how many interrupts the coalescing block actually
 * raised.
 */
static uint32_t read_status_tag(Bcm *b)
{
    uint8_t tag[4];

    qtest_memread(b->qts, STATUS_PHYS + BCM_STATUS_TAG_OFF, tag, sizeof(tag));
    return get32dma(tag);
}

/*
 * per-frame completion the device writes even while it coalesces the status
 */
static void wait_for_ret_end(Bcm *b, unsigned int idx)
{
    uint8_t flags[2];
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        qtest_memread(b->qts, RET_RING_PHYS + (uint64_t)idx * BCM_RXBD_SIZE +
                      DMA16(BCM_RXBD_FLAGS), flags, 2);
        if (get16be(flags) & BCM_RXBDFLAG_END) {
            return;
        }
        g_usleep(5000);
        qtest_clock_step(b->qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    qtest_memread(b->qts, RET_RING_PHYS + (uint64_t)idx * BCM_RXBD_SIZE +
                  DMA16(BCM_RXBD_FLAGS), flags, 2);
    g_assert_cmphex(get16be(flags) & BCM_RXBDFLAG_END, ==, BCM_RXBDFLAG_END);
}

/*
 * The four-tuple IRIX tg requires. The Broadcom primary vendor/device are
 * standard, and the SGI subsystem identity sits in the standard PCI subsystem
 * fields: subsystem vendor at config 0x2C, subsystem id at 0x2E.
 *
 * Note on offsets: the BCM570x datasheet's config table places the subsystem
 * device* at 0x2C and the subsystem *vendor* at 0x2E, taking OpenBSD bge's
 * header macro names (BGE_PCI_SUBSYS 0x2C / BGE_PCI_SUBVID 0x2E) at face
 * value. Those macro names are backwards: bge's own driver (if_bge.c) reads
 * the subsystem through the standard PCI_SUBSYS_ID_REG (0x2C) dword and takes
 * PCI_VENDOR() (the low half at 0x2C) as the subsystem vendor and
 * PCI_PRODUCT() (the high half at 0x2E) as the device. That is plain
 * standard-PCI layout, which is what real silicon and IRIX both use, so the
 * card presents SGI vendor 0x10a9 at 0x2C and the board id 0x8010 at 0x2E.
 */
static void test_pci_four_tuple(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());

    /* Dword reads: vendor in the low half, device in the high half. */
    g_assert_cmphex(qpci_config_readl(b.dev, PCI_VENDOR_ID), ==,
                    BCM_VENDOR_ID | ((uint32_t)BCM_DEVICE_ID << 16));
    g_assert_cmphex(qpci_config_readl(b.dev, PCI_SUBSYSTEM_VENDOR_ID), ==,
                    BCM_SUBSYS_VENDOR_ID | ((uint32_t)BCM_SUBSYS_ID << 16));

    /* And each field on its own, so a hidden swap in one half cannot hide
     * behind the other. */
    g_assert_cmphex(qpci_config_readw(b.dev, 0x00), ==, BCM_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(b.dev, 0x02), ==, BCM_DEVICE_ID);
    g_assert_cmphex(qpci_config_readw(b.dev, 0x2c), ==, BCM_SUBSYS_VENDOR_ID);
    g_assert_cmphex(qpci_config_readw(b.dev, 0x2e), ==, BCM_SUBSYS_ID);

    bcm_close(&b);
}

/*
 * The indirect register window round-trips. Writing a register offset to
 * config 0x78 and a value to config 0x80 must reach the same register file
 * the BAR exposes at 0x0000-0x7FFF: read the register back through BAR0 at
 * that offset and it must match, proving 0x78/0x80 alias the file.
 */
static void test_indirect_register_window(const void *data)
{
    Bcm b;
    const uint32_t reg_off = 0x0410;    /* in-range, dword-aligned */
    const uint32_t value = 0x12345678;

    bcm_open(&b, bcm_init());

    ind_reg_writel(&b, reg_off, value);

    /* Through the BAR at the same offset: the aliasing proof. */
    g_assert_cmphex(bar_readl(&b, reg_off), ==, value);

    /* And back through the indirect window itself. */
    g_assert_cmphex(ind_reg_readl(&b, reg_off), ==, value);

    bcm_close(&b);
}

/*
 * The indirect memory window round-trips. Writing an on-chip SRAM offset to
 * config 0x7C and a value to config 0x84 reaches one word of SRAM; the same
 * base, masked to a 32 KB boundary, positions the BAR0 window at 0x8000. Read
 * the word back both through config 0x7C/0x84 and through the BAR0 window,
 * and the two views must agree.
 */
static void test_indirect_memory_window(const void *data)
{
    Bcm b;
    const uint32_t sram_off = 0x0c14;   /* the MAC-address mailbox, aligned */
    const uint32_t value = 0xdeadbeef;

    bcm_open(&b, bcm_init());

    ind_mem_writel(&b, sram_off, value);

    /* Back through the indirect window. */
    g_assert_cmphex(ind_mem_readl(&b, sram_off), ==, value);

    /* And through the BAR0 SRAM window at 0x8000 + (offset & 0x7FFF). */
    g_assert_cmphex(bar_readl(&b, BCM_SRAM_WINDOW + (sram_off & 0x7fffu)), ==,
                    value);

    bcm_close(&b);
}

/*
 * The reset handshake completes when the reset is triggered through the BAR.
 * The mailbox stands in for the on-chip bootcode: it reads back the one's-
 * complement of the last value written, so the driver's magic reads as the
 * ready value at once. This covers the bge ordering, where the magic is
 * written before the reset; the reset re-arms the mailbox and self-clears its
 * trigger.
 */
static void test_reset_handshake_via_bar(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());

    ind_mem_writel(&b, BCM_SRAM_FW_MBOX, BCM_FW_MAGIC);
    g_assert_cmphex(ind_mem_readl(&b, BCM_SRAM_FW_MBOX), ==,
                    BCM_FW_HANDSHAKE_DONE);

    bar_writel(&b, BCM_MISC_CFG, BCM_MISCCFG_RESET);

    /*
     * The reset trigger self-clears; the driver polls MISC_CFG bit 0 for it.
     */
    g_assert_cmphex(bar_readl(&b, BCM_MISC_CFG) & BCM_MISCCFG_RESET, ==, 0);

    /* The reset leaves the mailbox reading ready for the post-reset poll. */
    g_assert_cmphex(ind_mem_readl(&b, BCM_SRAM_FW_MBOX), ==,
                    BCM_FW_HANDSHAKE_DONE);

    bcm_close(&b);
}

/*
 * The IRIX ordering: the driver resets the chip first, then writes the magic
 * to the firmware mailbox and polls it. Because the mailbox returns the
 * one's- complement of the last value written, the magic reads back ready
 * without any static value having been arranged during the reset. A mailbox
 * that only latched a complement on reset would time out because the
 * driver's magic write overwrote it.
 */
static void test_reset_handshake_magic_after_reset(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());

    bar_writel(&b, BCM_MISC_CFG, BCM_MISCCFG_RESET);

    ind_mem_writel(&b, BCM_SRAM_FW_MBOX, BCM_FW_MAGIC);
    g_assert_cmphex(ind_mem_readl(&b, BCM_SRAM_FW_MBOX), ==,
                    BCM_FW_HANDSHAKE_DONE);

    bcm_close(&b);
}

/*
 * The firmware-progress register (0x5034) the IRIX driver polls after reset.
 * It aborts the moment the register changes from its all-ones init to a value
 * it does not recognise, and a bare zero reads as exactly such a value (the
 * boot failure "Progress code=0x0"). The chip holds it at the all-ones "no
 * progress yet" sentinel both out of reset and after a global reset, so the
 * driver reads "unchanged" and keeps polling the mailbox instead of failing.
 */
static void test_firmware_progress_register(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());

    g_assert_cmphex(bar_readl(&b, BCM_FW_PROGRESS), ==, BCM_FW_PROGRESS_INIT);
    g_assert_cmphex(ind_reg_readl(&b, BCM_FW_PROGRESS), ==,
                    BCM_FW_PROGRESS_INIT);

    bar_writel(&b, BCM_MISC_CFG, BCM_MISCCFG_RESET);
    g_assert_cmphex(bar_readl(&b, BCM_FW_PROGRESS), ==, BCM_FW_PROGRESS_INIT);

    bcm_close(&b);
}

/*
 * The same handshake, but the reset is triggered through the indirect
 * register window (config 0x78/0x80) rather than the BAR, as the
 * 5700/5701 take. No BAR mapping is needed: every step here is config space.
 */
static void test_reset_handshake_via_indirect(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());

    ind_mem_writel(&b, BCM_SRAM_FW_MBOX, BCM_FW_MAGIC);

    ind_reg_writel(&b, BCM_MISC_CFG, BCM_MISCCFG_RESET);

    g_assert_cmphex(ind_mem_readl(&b, BCM_SRAM_FW_MBOX), ==,
                    BCM_FW_HANDSHAKE_DONE);

    bcm_close(&b);
}

/*
 * A *_MODE register's reset bit self-clears while the rest of the write
 * persists. The driver sets the bit and polls for it to read back clear.
 */
static void test_self_clearing_reset_bits(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());

    /* MAC_MODE: reset bit set alongside a port-mode bit that must survive. */
    bar_writel(&b, BCM_MAC_MODE, BCM_MODE_RESET | 0x4);
    g_assert_cmphex(bar_readl(&b, BCM_MAC_MODE) & BCM_MODE_RESET, ==, 0);
    g_assert_cmphex(bar_readl(&b, BCM_MAC_MODE) & 0x4, ==, 0x4);

    /* HCC_MODE reset bit self-clears too. */
    bar_writel(&b, BCM_HCC_MODE, BCM_MODE_RESET);
    g_assert_cmphex(bar_readl(&b, BCM_HCC_MODE) & BCM_MODE_RESET, ==, 0);

    /*
     * RX_MODE the same. Then the sequence the IRIX driver actually runs
     * through it: reset on its own, carrying no ENABLE. This leaves the
     * receiver halted rather than restarting it.
     */
    bar_writel(&b, BCM_RX_MODE, BCM_MODE_RESET | BCM_RXMODE_ENABLE);
    g_assert_cmphex(bar_readl(&b, BCM_RX_MODE) & BCM_MODE_RESET, ==, 0);
    g_assert_cmphex(bar_readl(&b, BCM_RX_MODE) & BCM_RXMODE_ENABLE, ==,
                    BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_MODE_RESET);
    g_assert_cmphex(bar_readl(&b, BCM_RX_MODE), ==, 0);

    bcm_close(&b);
}

/*
 * HCC_MODE stores and reads back faithfully: the driver's idle sequence
 * writes ENABLE, then writes 0 and polls up to ~2000 times for ENABLE to read
 * clear, aborting the attach if it never does.
 */
static void test_hcc_mode_idle_poll(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());

    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    g_assert_cmphex(bar_readl(&b, BCM_HCC_MODE) & BCM_HCC_ENABLE, ==,
                    BCM_HCC_ENABLE);

    bar_writel(&b, BCM_HCC_MODE, 0);
    g_assert_cmphex(bar_readl(&b, BCM_HCC_MODE) & BCM_HCC_ENABLE, ==, 0);

    bcm_close(&b);
}

/*
 * The buffer-manager enable handshake, where IRIX tg diverges from the public
 * drivers. The driver sets ENABLE (0x2) alongside ATTN_ENABLE (0x4) and then
 * polls ENABLE for CLEAR ("TG3_BM_ENA bit did not clear" on timeout),
 * unlike tg3/bge, which poll it for SET. A plain store-and-read-back
 * would leave ENABLE set and hang the driver. The chip acknowledges the
 * request by dropping ENABLE at once, while ATTN_ENABLE persists.
 */
static void test_bufmgr_enable_self_clears(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());

    bar_writel(&b, BCM_BUFMGR_MODE, BCM_BUFMGR_ENABLE | BCM_BUFMGR_ATTN);

    /* ENABLE reads back clear for the driver's poll... */
    g_assert_cmphex(bar_readl(&b, BCM_BUFMGR_MODE) & BCM_BUFMGR_ENABLE, ==, 0);
    /* ...while ATTN_ENABLE, which the driver did not poll, persists. */
    g_assert_cmphex(bar_readl(&b, BCM_BUFMGR_MODE) & BCM_BUFMGR_ATTN, ==,
                    BCM_BUFMGR_ATTN);

    bcm_close(&b);
}

/*
 * The host-coalescing idle poll reads the WHOLE register for zero, not just
 * the ENABLE bit ("Host Coalescing Mode Rgtr did not clear" on timeout).
 * Write a value with several bits set, then write 0 and confirm every bit
 * reads back clear, the state the driver waits for before it reprograms the
 * block.
 */
static void test_hcc_mode_clears_to_zero(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());

    bar_writel(&b, BCM_HCC_MODE, 0x0000013e);
    bar_writel(&b, BCM_HCC_MODE, 0);
    g_assert_cmphex(bar_readl(&b, BCM_HCC_MODE), ==, 0);

    bcm_close(&b);
}

/*
 * The MII BUSY handshake. The driver issues an MI command with START/BUSY
 * (0x20000000) set and polls BUSY for CLEAR ("BUSY bit never cleared" on
 * timeout); a read command must also leave the addressed PHY register's value
 * in the low 16 bits. The chip completes the transaction at once. Reading the
 * FULL command word back (not just its low half) proves BUSY is gone for the
 * poll, for both a read and a write command.
 */
static void test_mi_busy_clears(const void *data)
{
    Bcm b;
    uint32_t comm;

    bcm_open(&b, bcm_init());

    /*
     * A read of the PHY identity: BUSY clears and the low 16 carry the value.
     */
    bar_writel(&b, BCM_MI_COMM,
               BCM_MICOMM_BUSY | BCM_MICMD_READ |
               (BCM_PHY_ADDR << BCM_MICOMM_PHY_SHIFT) |
               (MII_PHYIDR1 << BCM_MICOMM_REG_SHIFT));
    comm = bar_readl(&b, BCM_MI_COMM);
    g_assert_cmphex(comm & BCM_MICOMM_BUSY, ==, 0);
    g_assert_cmphex(comm & 0xffff, ==, BCM_PHY_IDR1_VALUE);

    /* A write command: BUSY clears too, so the driver's poll returns. */
    bar_writel(&b, BCM_MI_COMM,
               BCM_MICOMM_BUSY | BCM_MICMD_WRITE |
               (BCM_PHY_ADDR << BCM_MICOMM_PHY_SHIFT) |
               (MII_BMCR << BCM_MICOMM_REG_SHIFT) |
               (MII_BMCR_AUTOEN | MII_BMCR_STARTNEG));
    g_assert_cmphex(bar_readl(&b, BCM_MI_COMM) & BCM_MICOMM_BUSY, ==, 0);

    bcm_close(&b);
}

/*
 * MISC_CTL[31:16] presents the chip id the driver reads its whole identity
 * from. For the single-port default it must decode to BCM5701 (chip id
 * 0x0105, ASIC revision chip-id >> 12 == 0), not merely non-zero.
 */
static void test_misc_ctl_asic_revision(const void *data)
{
    Bcm b;
    uint32_t misc_ctl;

    bcm_open(&b, bcm_init());
    misc_ctl = qpci_config_readl(b.dev, BCM_PCI_MISC_CTL);

    g_assert_cmphex(misc_ctl >> 16, ==, BCM_CHIPID_BCM5701);
    g_assert_cmphex((misc_ctl >> 16) >> 12, ==, BCM_ASICREV_BCM5701);

    bcm_close(&b);
}

/*
 * The MAC-address mailbox is honoured. The model presents the six bytes of
 * the -device mac= in SRAM 0x0C14/0x0C18 with the tag 0x484B in the high half
 * of 0x0C14, the path bge_attach reads before it falls back to an EEPROM.
 */
static void test_mac_address_mailbox(const void *data)
{
    Bcm b;
    QTestState *qts = qtest_initf(BCM_MACHINE
                                  " -netdev user,id=n0"
                                  " -device bcm570x,addr=%02x.0,netdev=n0,"
                                  "mac=52:54:00:ab:cd:ef", BCM_SLOT);

    bcm_open(&b, qts);

    /* Tag plus the first two address bytes in the high mailbox word. */
    g_assert_cmphex(ind_mem_readl(&b, 0x0c14), ==, 0x484b5254u);
    /* The remaining four address bytes in the low word. */
    g_assert_cmphex(ind_mem_readl(&b, 0x0c18), ==, 0x00abcdefu);

    bcm_close(&b);
}

/*
 * The station address also appears in MAC address filter slot 0
 * (MAC_ADDR_0_HIGH at 0x0410, MAC_ADDR_0_LOW at 0x0414), which the chip loads
 * from the serial EEPROM at power-on. A driver that reads the address
 * straight from these MAC control registers (not the SRAM mailbox) still
 * sees the -device mac=.
 */
static void test_mac_address_registers(const void *data)
{
    static const uint8_t expected[] = {
        0x54, 0x52, 0x00, 0x00, 0xef, 0xcd, 0xab, 0x00,
    };
    Bcm b;
    QTestState *qts = qtest_initf(BCM_MACHINE
                                  " -netdev user,id=n0"
                                  " -device bcm570x,addr=%02x.0,netdev=n0,"
                                  "mac=52:54:00:ab:cd:ef", BCM_SLOT);

    bcm_open(&b, qts);

    /* MAC_ADDR_0_HIGH: the first two address bytes, in the low half. */
    g_assert_cmphex(bar_readl(&b, BCM_MAC_ADDR_0_HIGH), ==, 0x00005254u);
    /* MAC_ADDR_0_LOW: the remaining four address bytes. */
    g_assert_cmphex(bar_readl(&b, BCM_MAC_ADDR_0_LOW), ==, 0x00abcdefu);
    assert_station_bytes(&b, expected);

    bar_writel(&b, BCM_MAC_ADDR_0_HIGH - 4, 0x55667788);
    bar_writel(&b, BCM_MAC_ADDR_0_LOW + 4, 0x11223344);
    /*
     * Only this pair permits narrow reads. Adjacent byte reads and halfword
     * accesses remain rejected by the register file.
     */
    g_assert_cmphex(bar_readb(&b, BCM_MAC_ADDR_0_HIGH - 1), ==, 0);
    g_assert_cmphex(bar_readb(&b, BCM_MAC_ADDR_0_LOW + 4), ==, 0);
    g_assert_cmphex(qpci_io_readw(b.dev, b.bar, BCM_MAC_ADDR_0_HIGH), ==,
                    0);

    /* Narrow writes are not part of the byte-read contract. */
    qpci_io_writeb(b.dev, b.bar, BCM_MAC_ADDR_0_HIGH, 0);
    qpci_io_writew(b.dev, b.bar, BCM_MAC_ADDR_0_LOW, 0);
    qpci_io_writeb(b.dev, b.bar, BCM_MAC_ADDR_0_LOW + 4, 0);
    g_assert_cmphex(bar_readl(&b, BCM_MAC_ADDR_0_HIGH), ==, 0x00005254u);
    g_assert_cmphex(bar_readl(&b, BCM_MAC_ADDR_0_LOW), ==, 0x00abcdefu);
    g_assert_cmphex(bar_readl(&b, BCM_MAC_ADDR_0_HIGH - 4), ==, 0x55667788);
    g_assert_cmphex(bar_readl(&b, BCM_MAC_ADDR_0_LOW + 4), ==, 0x11223344);

    bar_writel(&b, BCM_MAC_ADDR_0_HIGH, 0x0000a1a2);
    bar_writel(&b, BCM_MAC_ADDR_0_LOW, 0xa3a4a5a6);
    bar_writel(&b, BCM_MISC_CFG, BCM_MISCCFG_RESET);
    g_assert_cmphex(bar_readl(&b, BCM_MAC_ADDR_0_HIGH), ==, 0x00005254u);
    g_assert_cmphex(bar_readl(&b, BCM_MAC_ADDR_0_LOW), ==, 0x00abcdefu);
    assert_station_bytes(&b, expected);

    bcm_close(&b);
}

/*
 * The PHY answers through MI_COMM with the identity brgphy binds on, and its
 * link comes up after negotiation settles; link/autoneg completion drives
 * bring-up. MI_STS, the auto-poll link latch, follows the carrier.
 *
 * The settle is the contract, not a detail to assert away: a restart
 * negotiates for a while and completes afterwards, so a driver watching for
 * the completion it asked for sees the transition. Completing the moment the
 * guest writes the control register hides that transition and hangs IRIX tg's
 * bring-up.
 */
static void test_phy_readback(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());

    /* The Broadcom BCM5701 identity. */
    g_assert_cmphex(mii_read(&b, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);
    g_assert_cmphex(mii_read(&b, BCM_PHY_ADDR, MII_PHYIDR2), ==,
                    BCM_PHY_IDR2_BCM5701);

    /* Out of reset the link is still negotiating: down, incomplete. */
    g_assert_cmphex(mii_read(&b, BCM_PHY_ADDR, MII_BMSR) & MII_BMSR_LINK,
                    ==, 0);
    g_assert_cmphex(bar_readl(&b, BCM_MI_STS) & BCM_MISTS_LINK, ==, 0);

    /*
     * Configure the interface (restart autonegotiation): still negotiating.
     */
    mii_write(&b, BCM_PHY_ADDR, MII_BMCR,
              MII_BMCR_AUTOEN | MII_BMCR_STARTNEG);

    g_assert_cmphex(mii_read(&b, BCM_PHY_ADDR, MII_BMSR) & MII_BMSR_LINK,
                    ==, 0);

    /* The settle elapses and the link comes up. */
    qtest_clock_step(b.qts, BCM_PHY_SETTLE_NS + 1000);

    g_assert_cmphex(mii_read(&b, BCM_PHY_ADDR, MII_BMSR) & MII_BMSR_LINK,
                    ==, MII_BMSR_LINK);
    g_assert_cmphex(mii_read(&b, BCM_PHY_ADDR, MII_BMSR) & MII_BMSR_ANEGDONE,
                    ==, MII_BMSR_ANEGDONE);

    /* The auto-poll link latch now reads the carrier too. */
    g_assert_cmphex(bar_readl(&b, BCM_MI_STS) & BCM_MISTS_LINK, ==,
                    BCM_MISTS_LINK);

    bcm_close(&b);
}

/*
 * The copper link-up gate. IRIX tg's negotiation (tg_neg_copper_link) exits
 * only when the brgphy auxiliary status summary (PHY register 0x19) reads
 * with ACOMP (0x8000) set and a valid highest-common-denominator speed/duplex
 * in the AN_RES field (bits 10:8), and it treats an all-ones read as a PHY
 * that is not responding. So before the link is up the register must answer
 * with ACOMP clear and a real (non-0xffff) value, and once up it must present
 * ACOMP with a settled resolution of 1000baseT full duplex, matching the
 * gigabit answer the 1000baseT status register already gives.
 */
static void test_phy_aux_status_link_up(const void *data)
{
    Bcm b;
    uint16_t aux;

    bcm_open(&b, bcm_init());

    /*
     * Still negotiating: ACOMP clear, but a real value so it is not "no PHY".
     */
    aux = mii_read(&b, BCM_PHY_ADDR, BRGPHY_AUXSTS);
    g_assert_cmphex(aux & BRGPHY_AUXSTS_ACOMP, ==, 0);
    g_assert_cmphex(aux, !=, 0xffff);

    /* Configure the interface, then let the negotiation settle. */
    mii_write(&b, BCM_PHY_ADDR, MII_BMCR,
              MII_BMCR_AUTOEN | MII_BMCR_STARTNEG);
    qtest_clock_step(b.qts, BCM_PHY_SETTLE_NS + 1000);

    /* The gate the up-path waits on: ACOMP set, a valid HCD, not all-ones. */
    aux = mii_read(&b, BCM_PHY_ADDR, BRGPHY_AUXSTS);
    g_assert_cmphex(aux & BRGPHY_AUXSTS_ACOMP, ==, BRGPHY_AUXSTS_ACOMP);
    g_assert_cmphex(aux & BRGPHY_AUXSTS_AN_RES, ==, BRGPHY_AUXSTS_1000TFDX);
    g_assert_cmphex(aux, !=, 0xffff);

    bcm_close(&b);
}

/*
 * The byte-order test the rest of the file rests on, as a golden
 * fixed-byte-layout assertion. The chip DMAs the status block to guest
 * memory; behind a Bridge the model delivers the driver's own big-endian
 * layout, so a status flag of 0x00000001 must occupy bytes 00 00 00 01. A
 * self-round-trip could not tell that from the other direction, so the raw
 * bytes are read and the exact placement pinned.
 */
static void test_status_block_byte_order(const void *data)
{
    Bcm b;
    uint8_t sb[BCM_STATUS_BLK_SIZE];

    bcm_open(&b, bcm_init());
    bcm_arm_status(&b);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    /* A producer-0 doorbell posts the status block with UPDATED set. */
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 0);
    qtest_memread(b.qts, STATUS_PHYS, sb, sizeof(sb));

    /*
     * The status flag, byte for byte. Off a Bridge the same flag lands
     * little-endian, 01 00 00 00; on this host the word is big-endian.
     */
    g_assert_cmphex(sb[0], ==, 0x00);
    g_assert_cmphex(sb[1], ==, 0x00);
    g_assert_cmphex(sb[2], ==, 0x00);
    g_assert_cmphex(sb[3], ==, 0x01);
    g_assert_cmphex(get32dma(sb + 0), ==, BCM_STATFLAG_UPDATED);

    bcm_close(&b);
}

/*
 * Host-address word-swap. The big-endian IRIX driver sets ENDIAN_WORDSWAP and
 * writes each 64-bit host address as two 32-bit halves word-swapped: the
 * intended low word into ADDR_HI, the intended high word into ADDR_LO.
 * The chip must swap them back when it composes the DMA target. Program a
 * status-block address word-swapped (HI <- its low word, LO <- 0) and confirm
 * the block DMAs there. Composing (HI<<32)|LO raw would target that low word
 * shifted into the top half, off the end of the transfer memory, and land
 * nothing.
 */
static void test_status_block_hostaddr_wordswap(const void *data)
{
    /* Eight megabytes into the sixteen the head hands a card. */
    const uint64_t intended = DMA_BASE + 0x00800000ULL;
    Bcm b;
    uint32_t misc;
    uint8_t sb[BCM_STATUS_BLK_SIZE];

    bcm_open(&b, bcm_init());

    /* Send-ring length so a producer-0 doorbell passes the range guard. */
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);

    /* The driver arms ENDIAN_WORDSWAP in MISC_CTL for the big-endian host. */
    misc = qpci_config_readl(b.dev, BCM_PCI_MISC_CTL);
    qpci_config_writel(b.dev, BCM_PCI_MISC_CTL, misc | BCM_MISCCTL_WORDSWAP);

    /* Word-swapped: low word into ADDR_HI, high word into ADDR_LO. */
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, (uint32_t)intended);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, (uint32_t)(intended >> 32));

    qtest_memset(b.qts, intended, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    /* A producer-0 doorbell drives the status-block DMA. */
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 0);

    /* The swap honoured, the block lands at the intended address. */
    qtest_memread(b.qts, intended, sb, sizeof(sb));
    g_assert_cmphex(get32dma(sb + 0), ==, BCM_STATFLAG_UPDATED);

    bcm_close(&b);
}

/*
 * The transmit round trip. With the driver's swap options written, a transmit
 * descriptor and its buffer are DMA'd, the frame reaches the backend
 * byte-for-byte, and the status block the chip writes back carries its
 * transmit consumer advanced. The frame pattern has every byte distinct so a
 * swap anywhere on the path shows. The status-block bytes are checked as a
 * golden layout, the direction a round trip alone cannot see.
 */
static void test_transmit_round_trip(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[60];
    uint8_t bd[16];
    uint8_t sb[BCM_STATUS_BLK_SIZE];
    uint8_t got[128];
    unsigned int i;
    size_t len;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 7 + 1);
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    /*
     * The driver's byte-swap programming: stored for read-back, no live swap.
     */
    ind_reg_writel(&b, BCM_MODE_CTL, BCM_DMA_SWAP_OPTIONS);

    /*
     * The send-ring control block in on-chip SRAM: host base and ring length.
     */
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 0, TX_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4, TX_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);

    /* The status block's host address, and enable the coalescing block. */
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    /*
     * One completed descriptor per update [57XX-PG105-R, printed page 454].
     */
    bar_writel(&b, BCM_HCC_TX_MAX_COAL_BDS, 1);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    /* One transmit descriptor at ring index 0, in the Bridge's order. */
    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, TX_BUF_PHYS >> 32);
    put32dma(bd + 4, TX_BUF_PHYS & 0xffffffff);
    /*
     * len_flags: flags in the low 16 bits (0x08), length in the high (0x0a).
     */
    put16dma(bd, 8, BCM_TXBDFLAG_END);
    put16dma(bd, 10, sizeof(frame));
    qtest_memwrite(b.qts, TX_RING_PHYS, bd, sizeof(bd));
    qtest_memwrite(b.qts, TX_BUF_PHYS, frame, sizeof(frame));
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);

    /* Ring the doorbell: one descriptor produced. */
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 1);

    /* The frame reached the backend unchanged. */
    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));

    /* The status block landed with the transmit consumer advanced. */
    qtest_memread(b.qts, STATUS_PHYS, sb, sizeof(sb));
    g_assert_cmphex(get32dma(sb + 0), ==, BCM_STATFLAG_UPDATED);
    g_assert_cmphex(get16dma(sb, BCM_STATUS_TX_CONS_OFF), ==, 1);
    /*
     * Raw bytes prove the order. Off a Bridge a consumer of 1 is {0x01, 0x00}
     * at the field's own offset; here the word swap moves the halfword into
     * the other half of its word and writes it big-endian.
     */
    g_assert_cmphex(sb[DMA16(BCM_STATUS_TX_CONS_OFF)], ==, 0x00);
    g_assert_cmphex(sb[DMA16(BCM_STATUS_TX_CONS_OFF) + 1], ==, 0x01);

    bcm_close(&b);
    close(sv[0]);
}

static void test_user_backend_reply_waits_for_transmit(const void *data)
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
    uint8_t tx_bd[BCM_TXBD_SIZE] = { 0 };
    uint8_t rx_bd[BCM_RXBD_SIZE] = { 0 };
    uint8_t ret[BCM_RXBD_SIZE];
    uint8_t landed[sizeof(arp_request)];
    uint8_t sb[BCM_STATUS_BLK_SIZE];
    Bcm b;

    /*
     * rx-arrival-latency-us=0 on purpose. This test asserts that a
     * synchronous backend reply is invisible at the instant the transmit
     * doorbell write returns, which is what pins the model's rule about
     * deferring a backend reply until the transmit walk has published its
     * state. Under the arrival floor no virtual time has passed at that
     * instant either, so the assertion would hold whether or not that rule
     * existed. Taking the floor out leaves the rule as the only thing holding
     * the reply back.
     */
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev user,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0,"
                             "rx-arrival-latency-us=0", BCM_SLOT));

    bar_writel(&b, BCM_MAC_ADDR_0_HIGH, 0x00005254);
    bar_writel(&b, BCM_MAC_ADDR_0_LOW, 0x00123456);

    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 0, TX_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4,
                   TX_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(&b, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4,
                   RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);

    put32dma(rx_bd + 0, RX_BUF_PHYS >> 32);
    put32dma(rx_bd + 4, RX_BUF_PHYS & 0xffffffff);
    qtest_memwrite(b.qts, STD_RING_PHYS, rx_bd, sizeof(rx_bd));
    qtest_memset(b.qts, RX_BUF_PHYS, 0, sizeof(landed));
    qtest_memset(b.qts, RET_RING_PHYS, 0, sizeof(ret));
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);

    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(&b, BCM_HCC_TX_MAX_COAL_BDS, 2);
    bar_writel(&b, BCM_HCC_TX_COAL_TICKS, 1000000);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_false(intr_line_up(b.dev));

    put32dma(tx_bd + 0, TX_BUF_PHYS >> 32);
    put32dma(tx_bd + 4, TX_BUF_PHYS & 0xffffffff);
    put16dma(tx_bd, 8, BCM_TXBDFLAG_END);
    put16dma(tx_bd, 10, sizeof(arp_request));
    qtest_memwrite(b.qts, TX_RING_PHYS, tx_bd, sizeof(tx_bd));
    qtest_memwrite(b.qts, TX_BUF_PHYS, arp_request, sizeof(arp_request));

    /*
     * User networking answers the ARP request during qemu_send_packet(),
     * deferring the receive to a bottom half. The doorbell write returns to
     * the main loop, which runs that bottom half, so the status block then
     * reports the delivered frame and the interrupt is raised.
     */
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 1);
    /* Both indices, read the instant the doorbell write returns. */
    qtest_memread(b.qts, STATUS_PHYS, sb, sizeof(sb));
    g_assert_cmphex(get16dma(sb, BCM_STATUS_RX_PROD_OFF), ==, 1);
    g_assert_cmphex(get16dma(sb, BCM_STATUS_TX_CONS_OFF), ==, 1);
    g_assert_true(intr_line_up(b.dev));

    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_IDX), ==, 0);
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
                    BCM_RXBDFLAG_END);
    qtest_memread(b.qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, 6, arp_request + 6, 6);
    g_assert_cmphex(landed[21], ==, 0x02);
    g_assert_cmpmem(landed + 28, 4, gateway_ip, 4);
    g_assert_cmpmem(landed + 38, 4, guest_ip, 4);

    bcm_close(&b);
}

/* A refused transmit-descriptor read must leave the doorbell retryable. */
static void test_transmit_descriptor_dma_error(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[60];
    uint8_t bd[BCM_TXBD_SIZE] = { 0 };
    uint8_t got[sizeof(frame)];
    unsigned int i;
    size_t len;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 7 + 1);
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 0,
                   UNMAPPED_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4,
                   UNMAPPED_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 1);
    net_socket_expect_no_frame(sv[0]);
    g_assert_cmphex(read_status_tag(&b), ==, 0);

    put32dma(bd + 0, TX_BUF_PHYS >> 32);
    put32dma(bd + 4, TX_BUF_PHYS & 0xffffffff);
    put16dma(bd, 8, BCM_TXBDFLAG_END);
    put16dma(bd, 10, sizeof(frame));
    qtest_memwrite(b.qts, TX_RING_PHYS, bd, sizeof(bd));
    qtest_memwrite(b.qts, TX_BUF_PHYS, frame, sizeof(frame));
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 0, TX_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4,
                   TX_RING_PHYS & 0xffffffff);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 1);
    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));

    bcm_close(&b);
    close(sv[0]);
}

/* A later fragment's failed payload read rolls the whole frame back. */
static void test_transmit_payload_dma_error(const void *data)
{
    g_autofree char *logfile = NULL;
    uint8_t frame[60], got[sizeof(frame)];
    uint8_t ring[2 * BCM_TXBD_SIZE] = { 0 };
    uint8_t status[BCM_STATUS_BLK_SIZE];
    uint8_t byte;
    Bcm b;
    unsigned int i;
    uint32_t tag;
    size_t len;
    int logfd;
    int sv[2];

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 7 + 1);
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    logfd = g_file_open_tmp("bcm570x-tx-payload-error-XXXXXX", &logfile,
                            NULL);
    g_assert_cmpint(logfd, >=, 0);
    close(logfd);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0"
                             " -d guest_errors -D %s",
                             sv[1], BCM_SLOT, logfile));

    put32dma(ring, TX_BUF_PHYS >> 32);
    put32dma(ring + 4, TX_BUF_PHYS & 0xffffffff);
    put16dma(ring, 10, 28);
    put32dma(ring + BCM_TXBD_SIZE, UNMAPPED_PHYS >> 32);
    put32dma(ring + BCM_TXBD_SIZE + 4, UNMAPPED_PHYS & 0xffffffff);
    put16dma(ring + BCM_TXBD_SIZE, 8, BCM_TXBDFLAG_END);
    put16dma(ring + BCM_TXBD_SIZE, 10, 32);
    qtest_memwrite(b.qts, TX_RING_PHYS, ring, sizeof(ring));
    qtest_memwrite(b.qts, TX_BUF_PHYS, frame, 28);
    qtest_memset(b.qts, STATUS_PHYS, 0, sizeof(status));
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB, TX_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4,
                   TX_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    /*
     * One completed descriptor per update [57XX-PG105-R, printed page 454].
     */
    bar_writel(&b, BCM_HCC_TX_MAX_COAL_BDS, 1);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 2);
    wait_for_log_marker(b.qts, logfile,
                        "bcm570x: transmit payload read at 0x4000000 failed; "
                        "frame left ready to retry");
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 0);
    g_assert_cmphex(read_status_tag(&b), ==, tag);
    g_assert_false(intr_line_up(b.dev));
    g_assert_cmpint(recv(sv[0], &byte, sizeof(byte), MSG_DONTWAIT), ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);

    /* Repair only fragment two; producer 2 must replay fragment one. */
    put32dma(ring + BCM_TXBD_SIZE, (TX_BUF_PHYS + 0x100) >> 32);
    put32dma(ring + BCM_TXBD_SIZE + 4,
            (TX_BUF_PHYS + 0x100) & 0xffffffff);
    qtest_memwrite(b.qts, TX_RING_PHYS + BCM_TXBD_SIZE,
                   ring + BCM_TXBD_SIZE, BCM_TXBD_SIZE);
    qtest_memwrite(b.qts, TX_BUF_PHYS + 0x100, frame + 28, 32);
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 2);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 2);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
    close(sv[1]);
    unlink(logfile);
}

/* A completed descriptor stays consumed when the trailing frame faults. */
static void test_transmit_complete_then_fragment_dma_error_retry(
    const void *data)
{
    enum { FIRST_FRAGMENT_LEN = 28 };
    g_autofree char *logfile = NULL;
    bool empty_first = GPOINTER_TO_UINT(data);
    uint8_t first[60], second[60], got[sizeof(first)];
    uint8_t ring[3 * 16] = { 0 };
    uint8_t status[BCM_STATUS_BLK_SIZE];
    Bcm b;
    unsigned int i;
    uint32_t tag;
    size_t len;
    int logfd;
    int sv[2];

    for (i = 0; i < sizeof(first); i++) {
        first[i] = (uint8_t)(i * 7 + 1);
        second[i] = (uint8_t)(i * 5 + 3);
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    logfd = g_file_open_tmp("bcm570x-tx-partial-progress-XXXXXX", &logfile,
                            NULL);
    g_assert_cmpint(logfd, >=, 0);
    close(logfd);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0"
                             " -d guest_errors -D %s",
                             sv[1], BCM_SLOT, logfile));

    put32dma(ring, TX_BUF_PHYS >> 32);
    put32dma(ring + 4, TX_BUF_PHYS & 0xffffffff);
    put16dma(ring, 8, BCM_TXBDFLAG_END);
    put16dma(ring, 10, empty_first ? 0 : sizeof(first));
    put32dma(ring + 16, (TX_BUF_PHYS + 0x100) >> 32);
    put32dma(ring + 20, (TX_BUF_PHYS + 0x100) & 0xffffffff);
    put16dma(ring, 26, FIRST_FRAGMENT_LEN);
    put32dma(ring + 32, UNMAPPED_PHYS >> 32);
    put32dma(ring + 36, UNMAPPED_PHYS & 0xffffffff);
    put16dma(ring, 40, BCM_TXBDFLAG_END);
    put16dma(ring, 42, sizeof(second) - FIRST_FRAGMENT_LEN);
    qtest_memwrite(b.qts, TX_RING_PHYS, ring, sizeof(ring));
    qtest_memwrite(b.qts, TX_BUF_PHYS, first, sizeof(first));
    qtest_memwrite(b.qts, TX_BUF_PHYS + 0x100, second,
                   FIRST_FRAGMENT_LEN);
    qtest_memset(b.qts, STATUS_PHYS, 0, sizeof(status));
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB, TX_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4,
                   TX_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    /*
     * One completed descriptor per update [57XX-PG105-R, printed page 454].
     */
    bar_writel(&b, BCM_HCC_TX_MAX_COAL_BDS, 1);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 3);
    wait_for_log_marker(b.qts, logfile,
                        "bcm570x: transmit payload read at 0x4000000 failed; "
                        "frame left ready to retry");
    if (empty_first) {
        net_socket_expect_no_frame(sv[0]);
    } else {
        len = net_socket_recv_frame(sv[0], got, sizeof(got));
        g_assert_cmpuint(len, ==, sizeof(first));
        g_assert_cmpmem(got, len, first, sizeof(first));
    }
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 1);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_true(intr_line_up(b.dev));
    net_socket_expect_no_frame(sv[0]);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);

    put32dma(ring + 32,
            (TX_BUF_PHYS + 0x100 + FIRST_FRAGMENT_LEN) >> 32);
    put32dma(ring + 36,
            (TX_BUF_PHYS + 0x100 + FIRST_FRAGMENT_LEN) & 0xffffffff);
    qtest_memwrite(b.qts, TX_RING_PHYS + 32, ring + 32, 16);
    qtest_memwrite(b.qts, TX_BUF_PHYS + 0x100 + FIRST_FRAGMENT_LEN,
                   second + FIRST_FRAGMENT_LEN,
                   sizeof(second) - FIRST_FRAGMENT_LEN);
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 3);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(second));
    g_assert_cmpmem(got, len, second, sizeof(second));
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 3);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 2);
    g_assert_true(intr_line_up(b.dev));
    net_socket_expect_no_frame(sv[0]);

    bcm_close(&b);
    close(sv[0]);
    close(sv[1]);
    unlink(logfile);
}

/* A completed frame stays consumed when the next descriptor is unreadable. */
static void test_transmit_complete_then_descriptor_dma_error_retry(
    const void *data)
{
    /* Straddling the top of the transfer memory: descriptor 1 is past it. */
    const uint64_t initial_ring = DMA_END - 16;
    const uint64_t repaired_ring = TX_RING_PHYS - 16;
    g_autofree char *logfile = NULL;
    uint8_t first[60], second[60], got[sizeof(first)];
    uint8_t bd[16] = { 0 };
    uint8_t status[BCM_STATUS_BLK_SIZE];
    Bcm b;
    unsigned int i;
    uint32_t tag;
    size_t len;
    int logfd;
    int sv[2];

    for (i = 0; i < sizeof(first); i++) {
        first[i] = (uint8_t)(i * 7 + 1);
        second[i] = (uint8_t)(i * 5 + 3);
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    logfd = g_file_open_tmp("bcm570x-tx-descriptor-progress-XXXXXX",
                            &logfile, NULL);
    g_assert_cmpint(logfd, >=, 0);
    close(logfd);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0"
                             " -d guest_errors -D %s",
                             sv[1], BCM_SLOT, logfile));

    put32dma(bd, TX_BUF_PHYS >> 32);
    put32dma(bd + 4, TX_BUF_PHYS & 0xffffffff);
    put16dma(bd, 8, BCM_TXBDFLAG_END);
    put16dma(bd, 10, sizeof(first));
    qtest_memwrite(b.qts, initial_ring, bd, sizeof(bd));
    qtest_memwrite(b.qts, TX_BUF_PHYS, first, sizeof(first));
    qtest_memset(b.qts, STATUS_PHYS, 0, sizeof(status));
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB, initial_ring >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4,
                   initial_ring & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    /*
     * One completed descriptor per update [57XX-PG105-R, printed page 454].
     */
    bar_writel(&b, BCM_HCC_TX_MAX_COAL_BDS, 1);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 2);
    wait_for_log_marker(b.qts, logfile,
                        "bcm570x: transmit descriptor 1 read failed; "
                        "ring walk stopped");
    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(first));
    g_assert_cmpmem(got, len, first, sizeof(first));
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 1);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_true(intr_line_up(b.dev));
    net_socket_expect_no_frame(sv[0]);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);

    qtest_memwrite(b.qts, repaired_ring, bd, sizeof(bd));
    memset(bd, 0, sizeof(bd));
    put32dma(bd, (TX_BUF_PHYS + 0x100) >> 32);
    put32dma(bd + 4, (TX_BUF_PHYS + 0x100) & 0xffffffff);
    put16dma(bd, 8, BCM_TXBDFLAG_END);
    put16dma(bd, 10, sizeof(second));
    qtest_memwrite(b.qts, TX_RING_PHYS, bd, sizeof(bd));
    qtest_memwrite(b.qts, TX_BUF_PHYS + 0x100, second, sizeof(second));
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB, repaired_ring >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4,
                   repaired_ring & 0xffffffff);
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 2);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(second));
    g_assert_cmpmem(got, len, second, sizeof(second));
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 2);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 2);
    g_assert_true(intr_line_up(b.dev));
    net_socket_expect_no_frame(sv[0]);

    bcm_close(&b);
    close(sv[0]);
    close(sv[1]);
    unlink(logfile);
}

/* Partial-error coalescing counts every completed descriptor. */
static void test_transmit_partial_error_descriptor_coalescing(
    const void *data)
{
    const unsigned int retry_ticks = 50;
    g_autofree char *logfile = NULL;
    uint8_t first[60], second[60], got[sizeof(first)];
    uint8_t ring[3 * 16] = { 0 };
    uint8_t status[BCM_STATUS_BLK_SIZE];
    Bcm b;
    unsigned int i;
    uint32_t tag;
    size_t len;
    int logfd;
    int sv[2];

    for (i = 0; i < sizeof(first); i++) {
        first[i] = (uint8_t)(i * 7 + 1);
        second[i] = (uint8_t)(i * 5 + 3);
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    logfd = g_file_open_tmp("bcm570x-tx-partial-coalescing-XXXXXX",
                            &logfile, NULL);
    g_assert_cmpint(logfd, >=, 0);
    close(logfd);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0"
                             " -d guest_errors -D %s",
                             sv[1], BCM_SLOT, logfile));

    put32dma(ring, TX_BUF_PHYS >> 32);
    put32dma(ring + 4, TX_BUF_PHYS & 0xffffffff);
    put16dma(ring, 8, BCM_TXBDFLAG_END);
    put16dma(ring, 10, sizeof(first));
    put16dma(ring, 24, BCM_TXBDFLAG_END);
    put32dma(ring + 32, UNMAPPED_PHYS >> 32);
    put32dma(ring + 36, UNMAPPED_PHYS & 0xffffffff);
    put16dma(ring, 40, BCM_TXBDFLAG_END);
    put16dma(ring, 42, sizeof(second));
    qtest_memwrite(b.qts, TX_RING_PHYS, ring, sizeof(ring));
    qtest_memwrite(b.qts, TX_BUF_PHYS, first, sizeof(first));
    qtest_memset(b.qts, STATUS_PHYS, 0, sizeof(status));
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB, TX_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4,
                   TX_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(&b, BCM_HCC_TX_MAX_COAL_BDS, 2);
    bar_writel(&b, BCM_HCC_TX_COAL_TICKS, retry_ticks);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 3);
    wait_for_log_marker(b.qts, logfile,
                        "bcm570x: transmit payload read at 0x4000000 failed; "
                        "frame left ready to retry");
    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(first));
    g_assert_cmpmem(got, len, first, sizeof(first));
    net_socket_expect_no_frame(sv[0]);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 2);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_true(intr_line_up(b.dev));
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);

    put32dma(ring + 32, (TX_BUF_PHYS + 0x100) >> 32);
    put32dma(ring + 36, (TX_BUF_PHYS + 0x100) & 0xffffffff);
    qtest_memwrite(b.qts, TX_RING_PHYS + 32, ring + 32, 16);
    qtest_memwrite(b.qts, TX_BUF_PHYS + 0x100, second, sizeof(second));
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 3);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(second));
    g_assert_cmpmem(got, len, second, sizeof(second));
    net_socket_expect_no_frame(sv[0]);
    /*
     * The send budget ran out with nothing to report while the first frame's
     * failure was being logged, and a budget that has run out stays run out
     * until an update reloads it, so the retry's completion is reported as it
     * happens rather than held for a fresh one.
     */
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 3);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 2);
    g_assert_true(intr_line_up(b.dev));

    /* One update for the group, and no clock of its own behind it. */
    qtest_clock_step(b.qts, (int64_t)retry_ticks * 1000 + 1000);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 3);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 2);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
    close(sv[1]);
    unlink(logfile);
}

/*
 * Source-address insertion is a per-packet send-BD attribute. Bit 12 enables
 * replacement and bits 14:13 select one of the four programmed MAC slots; the
 * selector alone must not rewrite the frame. Packet attributes remain the
 * same on every fragment through the BD carrying END, so the split case keeps
 * them on both descriptors and cuts the source field itself across their
 * boundary.
 */
static void test_transmit_source_address(const void *data)
{
    static const uint32_t slot_regs[4][2] = {
        { 0x00000210, 0x20304050 },
        { 0x00000211, 0x21314151 },
        { 0x00000212, 0x22324252 },
        { 0x00000213, 0x23334353 },
    };
    static const uint8_t expected_src[4][6] = {
        { 0x02, 0x10, 0x20, 0x30, 0x40, 0x50 },
        { 0x02, 0x11, 0x21, 0x31, 0x41, 0x51 },
        { 0x02, 0x12, 0x22, 0x32, 0x42, 0x52 },
        { 0x02, 0x13, 0x23, 0x33, 0x43, 0x53 },
    };
    static const unsigned int selector_order[4] = { 2, 0, 3, 1 };
    static const uint8_t sentinel_src[6] = {
        0xde, 0xad, 0xbe, 0xef, 0x00, 0x01,
    };
    int sv[2];
    Bcm b;
    uint8_t frame[60];
    uint8_t expected[sizeof(frame)];
    uint8_t got[128];
    uint8_t bd[16];
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(0x80 + i);
    }
    memcpy(frame + 6, sentinel_src, sizeof(sentinel_src));

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    ind_reg_writel(&b, BCM_MODE_CTL,
                   BCM_DMA_SWAP_OPTIONS | BCM_MODE_HOST_SEND_BDS);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 0, TX_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4,
                   TX_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8,
                   BCM_SOURCE_TX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    for (i = 0; i < 4; i++) {
        bar_writel(&b, BCM_MAC_ADDR_0_HIGH + i * BCM_MAC_ADDR_STRIDE,
                   slot_regs[i][0]);
        bar_writel(&b, BCM_MAC_ADDR_0_LOW + i * BCM_MAC_ADDR_STRIDE,
                   slot_regs[i][1]);
    }

    for (i = 0; i < 4; i++) {
        unsigned int selector = selector_order[i];
        uint64_t buf = TX_BUF_PHYS + (uint64_t)i * 128;
        uint16_t flags = BCM_TXBDFLAG_INSERT_SRC_ADDR |
                         BCM_TXBDFLAG_CHOOSE_SRC_ADDR(selector) |
                         BCM_TXBDFLAG_END;
        size_t len;

        qtest_memwrite(b.qts, buf, frame, sizeof(frame));
        memset(bd, 0, sizeof(bd));
        put32dma(bd + 0, buf >> 32);
        put32dma(bd + 4, buf & 0xffffffff);
        put16dma(bd, 8, flags);
        put16dma(bd, 10, sizeof(frame));
        qtest_memwrite(b.qts, TX_RING_PHYS + (uint64_t)i * sizeof(bd),
                       bd, sizeof(bd));

        bar_writel(&b, BCM_MBX_TX_PROD_LO, i + 1);
        len = net_socket_recv_frame(sv[0], got, sizeof(got));
        g_assert_cmpuint(len, ==, sizeof(frame));
        memcpy(expected, frame, sizeof(expected));
        memcpy(expected + 6, expected_src[selector],
               sizeof(expected_src[selector]));
        g_assert_cmpmem(got, len, expected, sizeof(expected));
    }

    /*
     * A nonzero selector without INSERT leaves the complete frame untouched.
     */
    qtest_memwrite(b.qts, TX_BUF_PHYS + 4 * 128, frame, sizeof(frame));
    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, (TX_BUF_PHYS + 4 * 128) >> 32);
    put32dma(bd + 4, (TX_BUF_PHYS + 4 * 128) & 0xffffffff);
    put16dma(bd, 8, BCM_TXBDFLAG_CHOOSE_SRC_ADDR(3) |
                       BCM_TXBDFLAG_END);
    put16dma(bd, 10, sizeof(frame));
    qtest_memwrite(b.qts, TX_RING_PHYS + 4 * sizeof(bd), bd, sizeof(bd));
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 5);
    g_assert_cmpuint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==,
                     sizeof(frame));
    g_assert_cmpmem(got, sizeof(frame), frame, sizeof(frame));

    /* Split after source bytes 0-1; both fragments carry the packet flags. */
    qtest_memwrite(b.qts, TX_BUF_PHYS + 5 * 128, frame, 8);
    qtest_memwrite(b.qts, TX_BUF_PHYS + 6 * 128, frame + 8,
                   sizeof(frame) - 8);
    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, (TX_BUF_PHYS + 5 * 128) >> 32);
    put32dma(bd + 4, (TX_BUF_PHYS + 5 * 128) & 0xffffffff);
    put16dma(bd, 8, BCM_TXBDFLAG_INSERT_SRC_ADDR |
                       BCM_TXBDFLAG_CHOOSE_SRC_ADDR(2));
    put16dma(bd, 10, 8);
    qtest_memwrite(b.qts, TX_RING_PHYS + 5 * sizeof(bd), bd, sizeof(bd));
    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, (TX_BUF_PHYS + 6 * 128) >> 32);
    put32dma(bd + 4, (TX_BUF_PHYS + 6 * 128) & 0xffffffff);
    put16dma(bd, 8, BCM_TXBDFLAG_INSERT_SRC_ADDR |
                       BCM_TXBDFLAG_CHOOSE_SRC_ADDR(2) |
                       BCM_TXBDFLAG_END);
    put16dma(bd, 10, sizeof(frame) - 8);
    qtest_memwrite(b.qts, TX_RING_PHYS + 6 * sizeof(bd), bd, sizeof(bd));
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 7);
    g_assert_cmpuint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==,
                     sizeof(frame));
    memcpy(expected, frame, sizeof(expected));
    memcpy(expected + 6, expected_src[2], sizeof(expected_src[2]));
    g_assert_cmpmem(got, sizeof(frame), expected, sizeof(expected));

    bcm_close(&b);
    close(sv[0]);
}

/*
 * Segmentation offload. The driver hands down one buffer holding several
 * segments' worth of payload and names the segment size in the descriptor;
 * the card cuts it up. Sending it whole puts a frame on the wire no peer
 * accepts, and the sender then waits out a retransmission timeout for each
 * one, which reads as a link stalling rather than as a malformed frame.
 */
#define LSO_MSS         512u
#define LSO_SEGMENTS    3u
#define LSO_HDR         54u     /* min Ethernet + IPv4 + TCP headers */

static void build_lso_frame(uint8_t *f, unsigned int payload)
{
    static const uint8_t head[] = {
        0x52, 0x54, 0x00, 0x11, 0x22, 0x33,
        0x52, 0x54, 0x00, 0xbc, 0x57, 0x01,
        0x08, 0x00,
    };
    unsigned int i;

    memset(f, 0, LSO_HDR + payload);
    memcpy(f, head, sizeof(head));
    f[14] = 0x45;
    stw_be_p_test(f + 16, (uint16_t)(20 + 20 + payload));
    stw_be_p_test(f + 18, 0x1234);
    f[22] = 64;
    f[23] = 6;
    f[26] = 10; f[27] = 0; f[28] = 2; f[29] = 15;
    f[30] = 10; f[31] = 0; f[32] = 2; f[33] = 2;
    stw_be_p_test(f + 34, 0x3039);
    stw_be_p_test(f + 36, 0x0017);
    f[38] = 0x00; f[39] = 0x00; f[40] = 0x10; f[41] = 0x00;
    f[46] = 0x50;
    f[47] = 0x19;               /* FIN, PSH and ACK */
    stw_be_p_test(f + 48, 0x2000);
    for (i = 0; i < payload; i++) {
        f[LSO_HDR + i] = (uint8_t)(i & 0xff);
    }
}

static void lso_open(Bcm *b, int fd, uint8_t *frame, unsigned int len,
                     unsigned int mss, uint16_t flags)
{
    uint8_t bd[16];

    ind_reg_writel(b, BCM_MODE_CTL, BCM_DMA_SWAP_OPTIONS);
    ind_mem_writel(b, BCM_SRAM_SEND_RING_RCB + 0, TX_RING_PHYS >> 32);
    ind_mem_writel(b, BCM_SRAM_SEND_RING_RCB + 4, TX_RING_PHYS & 0xffffffff);
    ind_mem_writel(b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, TX_BUF_PHYS >> 32);
    put32dma(bd + 4, TX_BUF_PHYS & 0xffffffff);
    put16dma(bd, 8, flags | BCM_TXBDFLAG_END);
    put16dma(bd, 10, (uint16_t)len);
    put16dma(bd, 14, (uint16_t)mss);
    qtest_memwrite(b->qts, TX_RING_PHYS, bd, sizeof(bd));
    qtest_memwrite(b->qts, TX_BUF_PHYS, frame, len);
    bar_writel(b, BCM_MBX_TX_PROD_LO, 1);
    (void)fd;
}

static uint32_t lso_csum_add(uint32_t sum, const uint8_t *buf, size_t len);
static uint16_t lso_csum_finish(uint32_t sum);
static uint32_t lso_tcp_pseudo_sum(const uint8_t *frame, size_t tcp_len);

static void test_transmit_segmentation(const void *data)
{
    int sv[2];
    Bcm b;
    unsigned int payload = LSO_MSS * LSO_SEGMENTS;
    uint8_t *frame = g_malloc(LSO_HDR + payload);
    uint8_t got[2048];
    unsigned int i;

    build_lso_frame(frame, payload);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    lso_open(&b, sv[0], frame, LSO_HDR + payload, LSO_MSS,
             BCM_TXBDFLAG_LEGACY_SEGMENTATION_BITS);

    for (i = 0; i < LSO_SEGMENTS; i++) {
        size_t len = net_socket_recv_frame(sv[0], got, sizeof(got));
        uint8_t expected[LSO_HDR + LSO_MSS];
        uint32_t seq;

        g_assert_cmpint(len, ==, LSO_HDR + LSO_MSS);
        memcpy(expected, frame, LSO_HDR);
        memcpy(expected + LSO_HDR, frame + LSO_HDR + i * LSO_MSS,
               LSO_MSS);
        stw_be_p_test(expected + 16, 20 + 20 + LSO_MSS);
        stw_be_p_test(expected + 18, 0x1234 + i);
        expected[38] = (0x1000 + i * LSO_MSS) >> 24;
        expected[39] = (0x1000 + i * LSO_MSS) >> 16;
        expected[40] = (0x1000 + i * LSO_MSS) >> 8;
        expected[41] = 0x1000 + i * LSO_MSS;
        if (i < LSO_SEGMENTS - 1) {
            expected[47] &= ~0x09;
        }
        stw_be_p_test(expected + 24, 0);
        stw_be_p_test(expected + 50, 0);
        stw_be_p_test(expected + 24,
                      lso_csum_finish(lso_csum_add(0, expected + 14, 20)));
        stw_be_p_test(expected + 50,
                      lso_csum_finish(lso_csum_add(
                          lso_tcp_pseudo_sum(expected, 20 + LSO_MSS),
                          expected + 34, 20 + LSO_MSS)));
        g_assert_cmpmem(got, len, expected, sizeof(expected));
        seq = ((uint32_t)got[38] << 24) | ((uint32_t)got[39] << 16) |
              ((uint32_t)got[40] << 8) | got[41];
        g_assert_cmphex(seq, ==, 0x1000 + i * LSO_MSS);
        if (i < LSO_SEGMENTS - 1) {
            g_assert_cmphex(got[47] & 0x09, ==, 0);
        } else {
            g_assert_cmphex(got[47] & 0x09, ==, 0x09);
        }
    }

    bcm_close(&b);
    close(sv[0]);
    g_free(frame);
}

static uint32_t lso_csum_add(uint32_t sum, const uint8_t *buf, size_t len)
{
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)buf[i] << 8) | buf[i + 1];
    }
    if (len & 1) {
        sum += (uint32_t)buf[len - 1] << 8;
    }
    return sum;
}

static uint16_t lso_csum_finish(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint32_t lso_tcp_pseudo_sum(const uint8_t *frame, size_t tcp_len)
{
    uint8_t pseudo[12];

    memcpy(pseudo, frame + 26, 8);
    pseudo[8] = 0;
    pseudo[9] = IPPROTO_TCP;
    pseudo[10] = tcp_len >> 8;
    pseudo[11] = tcp_len;
    return lso_csum_add(0, pseudo, sizeof(pseudo));
}

#ifdef CONFIG_LINUX
/* A plain frame still needs an empty vnet header on a negotiated TAP. */
static void test_transmit_vnet_plain(const void *data)
{
    static const uint8_t arp[60] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x52, 0x54, 0x00, 0xbc, 0x57, 0x01,
        0x08, 0x06,
        0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
        0x52, 0x54, 0x00, 0xbc, 0x57, 0x01,
        10, 77, 0, 2,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        10, 77, 0, 1,
    };
    uint8_t mirrored[sizeof(struct virtio_net_hdr) + sizeof(arp)];
    struct virtio_net_hdr *vhdr = (struct virtio_net_hdr *)mirrored;
    uint32_t vnet_hdr_len;
    Bcm b;
    size_t len;
    int tap_fd, mirror_fd, inject_fd;

    if (!bcm_vnet_tap_open(&b, &tap_fd, &mirror_fd, &inject_fd)) {
        return;
    }

    lso_open(&b, mirror_fd, (uint8_t *)arp, sizeof(arp), 0, 0);
    len = mirror_recv_packet(mirror_fd, mirrored, sizeof(mirrored),
                             &vnet_hdr_len);

    g_assert_cmpuint(vnet_hdr_len, ==, sizeof(*vhdr));
    g_assert_cmpuint(len, ==, sizeof(mirrored));
    g_assert_cmpmem(vhdr, sizeof(*vhdr), &(struct virtio_net_hdr) { },
                    sizeof(*vhdr));
    g_assert_cmpmem(mirrored + sizeof(*vhdr), sizeof(arp), arp, sizeof(arp));

    bcm_close(&b);
    close(tap_fd);
    close(mirror_fd);
    close(inject_fd);
}

/* A vnet-capable backend receives one GSO packet, not three host copies. */
static void test_transmit_segmentation_vnet(const void *data)
{
    const unsigned int payload = LSO_MSS * LSO_SEGMENTS;
    const unsigned int frame_len = LSO_HDR + payload;
    g_autofree uint8_t *frame = g_malloc(frame_len);
    g_autofree uint8_t *mirrored = g_malloc(sizeof(struct virtio_net_hdr) +
                                            frame_len);
    struct virtio_net_hdr *vhdr = (struct virtio_net_hdr *)mirrored;
    const uint8_t *got = mirrored + sizeof(*vhdr);
    uint32_t vnet_hdr_len;
    Bcm b;
    size_t len;
    int tap_fd, mirror_fd, inject_fd;

    build_lso_frame(frame, payload);
    if (!bcm_vnet_tap_open(&b, &tap_fd, &mirror_fd, &inject_fd)) {
        return;
    }

    lso_open(&b, mirror_fd, frame, frame_len, LSO_MSS,
             BCM_TXBDFLAG_LEGACY_SEGMENTATION_BITS);
    len = mirror_recv_packet(mirror_fd, mirrored,
                             sizeof(struct virtio_net_hdr) + frame_len,
                             &vnet_hdr_len);

    g_assert_cmpuint(vnet_hdr_len, ==, sizeof(*vhdr));
    g_assert_cmpuint(len, ==, sizeof(*vhdr) + frame_len);
    g_assert_cmphex(vhdr->flags, ==, VIRTIO_NET_HDR_F_NEEDS_CSUM);
    g_assert_cmphex(vhdr->gso_type, ==, VIRTIO_NET_HDR_GSO_TCPV4);
    g_assert_cmpuint(vhdr->hdr_len, ==, LSO_HDR);
    g_assert_cmpuint(vhdr->gso_size, ==, LSO_MSS);
    g_assert_cmpuint(vhdr->csum_start, ==, 14 + 20);
    g_assert_cmpuint(vhdr->csum_offset, ==, 16);
    g_assert_cmpmem(got, 24, frame, 24);
    g_assert_cmphex(get16be_test(got + 24), !=, 0);
    g_assert_cmpmem(got + 26, 24, frame + 26, 24);
    g_assert_cmphex(get16be_test(got + 50), ==,
                    (uint16_t)~lso_csum_finish(
                        lso_tcp_pseudo_sum(frame, 20 + payload)));
    g_assert_cmpmem(got + 52, frame_len - 52,
                    frame + 52, frame_len - 52);
    g_assert_cmphex(got[47] & 0x09, ==, 0x09);
    net_socket_expect_no_frame(mirror_fd);

    bcm_close(&b);
    close(tap_fd);
    close(mirror_fd);
    close(inject_fd);
}

typedef enum VnetLsoFallbackCase {
    VNET_LSO_FALLBACK_VLAN,
    VNET_LSO_FALLBACK_MF,
    VNET_LSO_FALLBACK_OFFSET,
    VNET_LSO_FALLBACK_LENGTH,
    VNET_LSO_FALLBACK_UDP,
} VnetLsoFallbackCase;

/* Unsupported LSO inputs retain the exact whole-frame fallback on TAP. */
static void test_transmit_segmentation_vnet_fallback(const void *data)
{
    VnetLsoFallbackCase test_case = GPOINTER_TO_INT(data);
    const unsigned int payload = LSO_MSS * LSO_SEGMENTS;
    unsigned int frame_len = LSO_HDR + payload;
    g_autofree uint8_t *frame = g_malloc(frame_len + 4);
    g_autofree uint8_t *mirrored = g_malloc(sizeof(struct virtio_net_hdr) +
                                            frame_len + 4);
    uint32_t vnet_hdr_len;
    Bcm b;
    size_t len;
    int tap_fd, mirror_fd, inject_fd;

    build_lso_frame(frame, payload);
    switch (test_case) {
    case VNET_LSO_FALLBACK_VLAN:
        memmove(frame + 16, frame + 12, frame_len - 12);
        frame[12] = 0x81;
        frame[13] = 0x00;
        frame[14] = 0x00;
        frame[15] = 0x2a;
        frame_len += 4;
        break;
    case VNET_LSO_FALLBACK_MF:
        frame[20] = 0x20;
        break;
    case VNET_LSO_FALLBACK_OFFSET:
        frame[21] = 0x01;
        break;
    case VNET_LSO_FALLBACK_LENGTH:
        stw_be_p_test(frame + 16, 40);
        break;
    case VNET_LSO_FALLBACK_UDP:
        frame[23] = IPPROTO_UDP;
        break;
    default:
        g_assert_not_reached();
    }

    if (!bcm_vnet_tap_open(&b, &tap_fd, &mirror_fd, &inject_fd)) {
        return;
    }
    lso_open(&b, mirror_fd, frame, frame_len, LSO_MSS,
             BCM_TXBDFLAG_LEGACY_SEGMENTATION_BITS);
    len = mirror_recv_packet(mirror_fd, mirrored,
                             sizeof(struct virtio_net_hdr) + frame_len,
                             &vnet_hdr_len);

    g_assert_cmpuint(vnet_hdr_len, ==, sizeof(struct virtio_net_hdr));
    g_assert_cmpuint(len, ==, sizeof(struct virtio_net_hdr) + frame_len);
    g_assert_cmpmem(mirrored, sizeof(struct virtio_net_hdr),
                    &(struct virtio_net_hdr) { },
                    sizeof(struct virtio_net_hdr));
    g_assert_cmpmem(mirrored + sizeof(struct virtio_net_hdr), frame_len,
                    frame, frame_len);
    net_socket_expect_no_frame(mirror_fd);

    bcm_close(&b);
    close(tap_fd);
    close(mirror_fd);
    close(inject_fd);
}
#endif

/* Tagged packets retain the pre-existing whole-frame fallback semantics. */
static void test_transmit_segmentation_vlan_fallback(const void *data)
{
    const unsigned int payload = LSO_MSS * LSO_SEGMENTS;
    const unsigned int frame_len = LSO_HDR + 4 + payload;
    g_autofree uint8_t *frame = g_malloc(frame_len);
    uint8_t got[2048];
    int sv[2];
    Bcm b;
    size_t len;

    build_lso_frame(frame, payload);
    memmove(frame + 16, frame + 12, LSO_HDR + payload - 12);
    frame[12] = 0x81;
    frame[13] = 0x00;
    frame[14] = 0x00;
    frame[15] = 0x2a;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    lso_open(&b, sv[0], frame, frame_len, LSO_MSS,
             BCM_TXBDFLAG_LEGACY_SEGMENTATION_BITS);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, frame_len);
    g_assert_cmpmem(got, len, frame, frame_len);
    net_socket_expect_no_frame(sv[0]);

    bcm_close(&b);
    close(sv[0]);
}

/* A descriptor naming no segment size is the ordinary path, untouched. */
static void test_transmit_no_segmentation(const void *data)
{
    int sv[2];
    Bcm b;
    unsigned int payload = 200;
    uint8_t *frame = g_malloc(LSO_HDR + payload);
    uint8_t got[2048];
    size_t len;

    build_lso_frame(frame, payload);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    lso_open(&b, sv[0], frame, LSO_HDR + payload, 0, 0);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, LSO_HDR + payload);
    g_assert_cmpmem(got, 34, frame, 34);

    bcm_close(&b);
    close(sv[0]);
    g_free(frame);
}

/*
 * Transmit interrupt coalescing. The driver says how many completed
 * descriptors it will wait for; reporting each one as it happens spends an
 * interrupt per frame on a saving nobody asked for.
 */
#define TXC_FRAMES      16u
#define TXC_COAL        4u
#define TXC_RING        64u

static void txc_setup(Bcm *b, unsigned int coal, unsigned int ticks)
{
    ind_reg_writel(b, BCM_MODE_CTL, BCM_DMA_SWAP_OPTIONS);
    ind_mem_writel(b, BCM_SRAM_SEND_RING_RCB + 0, TX_RING_PHYS >> 32);
    ind_mem_writel(b, BCM_SRAM_SEND_RING_RCB + 4, TX_RING_PHYS & 0xffffffff);
    ind_mem_writel(b, BCM_SRAM_SEND_RING_RCB + 8, TXC_RING << 16);
    bar_writel(b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(b, BCM_HCC_TX_MAX_COAL_BDS, coal);
    bar_writel(b, BCM_HCC_TX_COAL_TICKS, ticks);
    bar_writel(b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    qtest_memset(b->qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
}

static void txc_write_bd_at(Bcm *b, unsigned int slot, uint64_t buf,
                            unsigned int len, uint16_t flags)
{
    uint8_t bd[16] = { 0 };

    g_assert_cmpuint(slot, <, TXC_RING);
    g_assert_cmpuint(len, <=, UINT16_MAX);
    put32dma(bd + 0, buf >> 32);
    put32dma(bd + 4, buf & 0xffffffff);
    put16dma(bd, 8, flags);
    put16dma(bd, 10, len);
    qtest_memwrite(b->qts, TX_RING_PHYS + (uint64_t)slot * 16,
                   bd, sizeof(bd));
}

static void txc_post_one_at(Bcm *b, const uint8_t *frame, unsigned int len,
                            unsigned int slot)
{
    uint64_t buf = TX_BUF_PHYS + (uint64_t)slot * 2048;

    qtest_memwrite(b->qts, buf, frame, len);
    txc_write_bd_at(b, slot, buf, len, BCM_TXBDFLAG_END);
    bar_writel(b, BCM_MBX_TX_PROD_LO, slot + 1);
}

/*
 * Post frames the way a driver does: one descriptor, one doorbell, per frame.
 * That is what makes coalescing useful. A driver that handed the
 * card a whole burst on one doorbell would get one completion either way.
 */
static void txc_post(Bcm *b, const uint8_t *frame, unsigned int len,
                     unsigned int frames)
{
    unsigned int i;

    for (i = 0; i < frames; i++) {
        txc_post_one_at(b, frame, len, i);
    }
}

static void test_transmit_coalescing(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t got[128];
    uint32_t before, after;
    unsigned int i;

    memset(frame, 0x5a, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    /* Tick budget parked far out, so the frame count is what triggers. */
    txc_setup(&b, TXC_COAL, 0x10000000);

    before = read_status_tag(&b);
    txc_post(&b, frame, sizeof(frame), TXC_FRAMES);

    /* Every frame still reaches the wire. */
    for (i = 0; i < TXC_FRAMES; i++) {
        g_assert_cmpint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==,
                        sizeof(frame));
    }
    after = read_status_tag(&b);

    /* N frames grouped K at a time is N/K updates, not N. */
    g_assert_cmpuint(after - before, ==, TXC_FRAMES / TXC_COAL);

    bcm_close(&b);
    close(sv[0]);
}

/* Empty and dropped frames still complete every descriptor they consume. */
static void test_transmit_coalescing_empty_and_dropped(const void *data)
{
    enum { OVERSIZE_FRAGMENT_LEN = 9600 };
    uint8_t frame[64], got[sizeof(frame)];
    uint8_t status[BCM_STATUS_BLK_SIZE];
    Bcm b;
    uint32_t tag;
    size_t len;
    int sv[2];

    memset(frame, 0x69, sizeof(frame));
    memset(frame, 0xff, 6);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    txc_setup(&b, 4, 0x10000000);
    qtest_memwrite(b.qts, TX_BUF_PHYS, frame, sizeof(frame));
    qtest_memset(b.qts, TX_BUF_PHYS + 0x1000, 0xa5,
                 OVERSIZE_FRAGMENT_LEN);
    txc_write_bd_at(&b, 0, TX_BUF_PHYS, sizeof(frame), BCM_TXBDFLAG_END);
    txc_write_bd_at(&b, 1, 0, 0, BCM_TXBDFLAG_END);
    txc_write_bd_at(&b, 2, TX_BUF_PHYS + 0x1000,
                    OVERSIZE_FRAGMENT_LEN, 0);
    txc_write_bd_at(&b, 3, TX_BUF_PHYS + 0x4000, 1,
                    BCM_TXBDFLAG_END);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 4);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));
    net_socket_expect_no_frame(sv[0]);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 4);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
    close(sv[1]);
}

/* One completed frame may consume enough descriptors to fill a group. */
static void test_transmit_coalescing_multidescriptor(const void *data)
{
    enum { FIRST_FRAGMENT_LEN = 28 };
    uint8_t frame[60], got[sizeof(frame)];
    uint8_t status[BCM_STATUS_BLK_SIZE];
    Bcm b;
    unsigned int i;
    uint32_t tag;
    size_t len;
    int sv[2];

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 7 + 1);
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    txc_setup(&b, 2, 0x10000000);
    qtest_memwrite(b.qts, TX_BUF_PHYS, frame, FIRST_FRAGMENT_LEN);
    qtest_memwrite(b.qts, TX_BUF_PHYS + 0x100,
                   frame + FIRST_FRAGMENT_LEN,
                   sizeof(frame) - FIRST_FRAGMENT_LEN);
    txc_write_bd_at(&b, 0, TX_BUF_PHYS, FIRST_FRAGMENT_LEN, 0);
    txc_write_bd_at(&b, 1, TX_BUF_PHYS + 0x100,
                    sizeof(frame) - FIRST_FRAGMENT_LEN,
                    BCM_TXBDFLAG_END);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 2);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));
    net_socket_expect_no_frame(sv[0]);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 2);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
    close(sv[1]);
}

/* A doorbell ending within a frame leaves every fragment ready to retry. */
static void test_transmit_split_doorbell(const void *data)
{
    enum { FIRST_FRAGMENT_LEN = 28 };
    bool empty_first = GPOINTER_TO_UINT(data);
    unsigned int first_len = empty_first ? 0 : FIRST_FRAGMENT_LEN;
    uint8_t frame[60], got[sizeof(frame)];
    uint8_t status[BCM_STATUS_BLK_SIZE];
    Bcm b;
    unsigned int i;
    uint32_t tag;
    size_t len;
    int sv[2];

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 7 + 1);
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    txc_setup(&b, 1, 0x10000000);
    qtest_memwrite(b.qts, TX_BUF_PHYS, frame, first_len);
    qtest_memwrite(b.qts, TX_BUF_PHYS + 0x100, frame + first_len,
                   sizeof(frame) - first_len);
    txc_write_bd_at(&b, 0, TX_BUF_PHYS, first_len, 0);
    txc_write_bd_at(&b, 1, TX_BUF_PHYS + 0x100,
                    sizeof(frame) - first_len, BCM_TXBDFLAG_END);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 1);

    net_socket_expect_no_frame(sv[0]);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 0);
    g_assert_cmphex(read_status_tag(&b), ==, tag);
    g_assert_false(intr_line_up(b.dev));

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 2);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));
    net_socket_expect_no_frame(sv[0]);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 2);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
    close(sv[1]);
}

/* A trailing partial frame does not roll back an earlier completion. */
static void test_transmit_complete_then_split_doorbell(const void *data)
{
    enum { FIRST_FRAGMENT_LEN = 28 };
    uint8_t first[60], second[60], got[sizeof(first)];
    uint8_t status[BCM_STATUS_BLK_SIZE];
    Bcm b;
    unsigned int i;
    uint32_t tag;
    size_t len;
    int sv[2];

    for (i = 0; i < sizeof(first); i++) {
        first[i] = (uint8_t)(i * 7 + 1);
        second[i] = (uint8_t)(i * 5 + 3);
    }
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    txc_setup(&b, 1, 0x10000000);
    qtest_memwrite(b.qts, TX_BUF_PHYS, first, sizeof(first));
    qtest_memwrite(b.qts, TX_BUF_PHYS + 0x100, second,
                   FIRST_FRAGMENT_LEN);
    qtest_memwrite(b.qts, TX_BUF_PHYS + 0x200,
                   second + FIRST_FRAGMENT_LEN,
                   sizeof(second) - FIRST_FRAGMENT_LEN);
    txc_write_bd_at(&b, 0, TX_BUF_PHYS, sizeof(first), BCM_TXBDFLAG_END);
    txc_write_bd_at(&b, 1, TX_BUF_PHYS + 0x100, FIRST_FRAGMENT_LEN, 0);
    txc_write_bd_at(&b, 2, TX_BUF_PHYS + 0x200,
                    sizeof(second) - FIRST_FRAGMENT_LEN,
                    BCM_TXBDFLAG_END);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 2);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(first));
    g_assert_cmpmem(got, len, first, sizeof(first));
    net_socket_expect_no_frame(sv[0]);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 1);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_true(intr_line_up(b.dev));
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 3);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(second));
    g_assert_cmpmem(got, len, second, sizeof(second));
    net_socket_expect_no_frame(sv[0]);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 3);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 2);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
    close(sv[1]);
}

/*
 * The other half, and the one that matters more: a burst that does not fill a
 * group must still be reported. A count with no clock behind it holds the
 * tail of every transfer back for an interrupt that never comes.
 */
static void test_transmit_coalescing_timer(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t got[128];
    uint32_t before, after;
    unsigned int i;
    const unsigned int ticks = 50;

    memset(frame, 0x3c, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    /* A group of eight asked for, but only three frames sent. */
    txc_setup(&b, 8, ticks);

    before = read_status_tag(&b);
    txc_post(&b, frame, sizeof(frame), 3);
    for (i = 0; i < 3; i++) {
        g_assert_cmpint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==,
                        sizeof(frame));
    }

    /* Nothing reported yet: the group is short. */
    g_assert_cmpuint(read_status_tag(&b) - before, ==, 0);

    /* The tick budget elapses and the short group is reported anyway. */
    qtest_clock_step(b.qts, (int64_t)ticks * 1000 + 1000);
    after = read_status_tag(&b);
    g_assert_cmpuint(after - before, ==, 1);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * A zero in a coalescing ticks register disables that timer: "A value of 0 in
 * this register disables the transmit tick coalescing logic. In this case,
 * status block updates will occur for transmit events only if the Send Max
 * Coalesced BD value is reached" [57XX-PG105-R, printed page 453]. A short
 * group therefore waits for the count however long the ring stays idle.
 */
static void test_transmit_coalescing_zero_ticks(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t got[128];
    uint8_t status[BCM_STATUS_BLK_SIZE];

    memset(frame, 0x8d, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    txc_setup(&b, 2, 0);

    txc_post_one_at(&b, frame, sizeof(frame), 0);
    g_assert_cmpint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==,
                    sizeof(frame));
    /* One descriptor of the two, and no timer to give up waiting. */
    qtest_clock_step(b.qts, 1000 * 1000 * 1000);
    g_assert_cmpuint(read_status_tag(&b), ==, 0);

    txc_post_one_at(&b, frame, sizeof(frame), 1);
    g_assert_cmpint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==,
                    sizeof(frame));
    g_assert_cmpuint(read_status_tag(&b), ==, 1);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 2);

    bcm_close(&b);
    close(sv[0]);
}

/* A failed status DMA must leave a completed transmit group retryable. */
static void test_transmit_coalescing_status_dma_retry(const void *data)
{
    const unsigned int ticks = 50;
    uint8_t frame[64], got[sizeof(frame)];
    uint8_t status[BCM_STATUS_BLK_SIZE];
    Bcm b;
    int sv[2];

    memset(frame, 0x9b, sizeof(frame));
    memset(frame, 0xff, 6);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    txc_setup(&b, 1, ticks);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, UNMAPPED_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, UNMAPPED_PHYS & 0xffffffff);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);

    txc_post(&b, frame, sizeof(frame), 1);
    g_assert_cmpint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==,
                    sizeof(frame));
    g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 0);
    g_assert_false(intr_line_up(b.dev));

    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    qtest_clock_step(b.qts, (int64_t)ticks * 1000 + 1000);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 1);
    g_assert_cmphex(read_status_tag(&b), ==, 1);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
}

/*
 * The MAC status word's change bits are latches, and the driver clears one by
 * writing it back. Storing what it wrote instead leaves every bit it tried to
 * clear reading as set, so the driver sees the condition it just acknowledged
 * still standing and handles it again. On IRIX, carrier was
 * re-announced once a second for the life of the interface.
 */
#define BCM_MAC_STATUS          0x0404u
#define BCM_MACSTAT_CFG_CHANGED  0x00000008u
#define BCM_MACSTAT_SYNC_CHANGED 0x00000010u

static void test_mac_status_write_one_clears(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());

    /* Nothing latched to begin with. */
    g_assert_cmphex(bar_readl(&b, BCM_MAC_STATUS), ==, 0);

    /* Acknowledging bits that are not set leaves it clear rather than
     * recording the acknowledgement as the state. */
    bar_writel(&b, BCM_MAC_STATUS,
               BCM_MACSTAT_CFG_CHANGED | BCM_MACSTAT_SYNC_CHANGED);
    g_assert_cmphex(bar_readl(&b, BCM_MAC_STATUS), ==, 0);

    /* And again, the way a driver polls: the write must never be readable
     * back as the register's value. */
    bar_writel(&b, BCM_MAC_STATUS, 0xffffffffu);
    g_assert_cmphex(bar_readl(&b, BCM_MAC_STATUS), ==, 0);

    bcm_close(&b);
}

/* The one's-complement sum an IP or TCP checksum field is meant to hold. */
static uint16_t inet_csum(const uint8_t *b, size_t len, uint32_t sum)
{
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)b[i] << 8) | b[i + 1];
    }
    if (len & 1) {
        sum += (uint32_t)b[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* An Ethernet/IPv4/TCP frame with both checksum fields left for the card. */
#define CKSUM_FRAME_LEN 62
static void build_cksum_frame(uint8_t *f, uint8_t ihl_byte)
{
    static const uint8_t head[] = {
        0x52, 0x54, 0x00, 0x11, 0x22, 0x33,     /* destination */
        0x52, 0x54, 0x00, 0xbc, 0x57, 0x01,     /* source */
        0x08, 0x00,                             /* IPv4 */
    };
    unsigned int i;

    memset(f, 0, CKSUM_FRAME_LEN);
    memcpy(f, head, sizeof(head));

    f[14] = ihl_byte;               /* version and header length */
    f[16] = 0x00; f[17] = 48;       /* total length: 20 + 20 + 8 */
    f[22] = 64;                     /* time to live */
    f[23] = 6;                      /* TCP */
    /* checksum at 24..25 stays zero; the card fills it */
    f[26] = 10; f[27] = 0; f[28] = 2; f[29] = 15;       /* source address */
    f[30] = 10; f[31] = 0; f[32] = 2; f[33] = 2;        /* destination */

    f[34] = 0x30; f[35] = 0x39;     /* source port */
    f[36] = 0x00; f[37] = 0x17;     /* destination port */
    f[46] = 0x50;                   /* data offset: five words */
    f[47] = 0x02;                   /* SYN */
    f[48] = 0x20; f[49] = 0x00;     /* window */
    /* checksum at 50..51 stays zero, likewise the card's to fill */
    for (i = 54; i < CKSUM_FRAME_LEN; i++) {
        f[i] = (uint8_t)(i);
    }
}

/* Put one frame on the wire through the send ring and read it back. */
static size_t transmit_one(Bcm *b, int rx_fd, const uint8_t *frame,
                           size_t frame_len, uint16_t flags,
                           uint8_t *got, size_t got_size)
{
    uint8_t bd[16];

    ind_reg_writel(b, BCM_MODE_CTL, BCM_DMA_SWAP_OPTIONS);
    ind_mem_writel(b, BCM_SRAM_SEND_RING_RCB + 0, TX_RING_PHYS >> 32);
    ind_mem_writel(b, BCM_SRAM_SEND_RING_RCB + 4, TX_RING_PHYS & 0xffffffff);
    ind_mem_writel(b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, TX_BUF_PHYS >> 32);
    put32dma(bd + 4, TX_BUF_PHYS & 0xffffffff);
    put16dma(bd, 8, flags);
    put16dma(bd, 10, frame_len);
    qtest_memwrite(b->qts, TX_RING_PHYS, bd, sizeof(bd));
    qtest_memwrite(b->qts, TX_BUF_PHYS, frame, frame_len);

    bar_writel(b, BCM_MBX_TX_PROD_LO, 1);
    return net_socket_recv_frame(rx_fd, got, got_size);
}

/*
 * The checksums the driver asks the card to finish. IRIX tg hands a frame
 * over with the IP and TCP checksum fields still empty and the offload bits
 * set in the descriptor, so a model that transmits them untouched puts a
 * frame on the wire that every receiver discards in silence. The
 * failure hides, because ICMP and ARP carry checksums the guest computed
 * itself and keep working.
 */
static void test_transmit_checksum_offload(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[CKSUM_FRAME_LEN];
    uint8_t got[128];
    uint8_t bare[CKSUM_FRAME_LEN];
    uint16_t ip_expect, tcp_expect;
    uint32_t pseudo;
    size_t len;

    build_cksum_frame(frame, 0x45);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    len = transmit_one(&b, sv[0], frame, sizeof(frame),
                       BCM_TXBDFLAG_END | BCM_TXBDFLAG_IP_CKSUM |
                       BCM_TXBDFLAG_TCP_UDP_CKSUM, got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));

    /* What each field should hold, computed over the frame as sent. */
    memcpy(bare, frame, sizeof(frame));
    ip_expect = inet_csum(bare + 14, 20, 0);

    pseudo = 0;
    pseudo += ((uint32_t)bare[26] << 8) | bare[27];
    pseudo += ((uint32_t)bare[28] << 8) | bare[29];
    pseudo += ((uint32_t)bare[30] << 8) | bare[31];
    pseudo += ((uint32_t)bare[32] << 8) | bare[33];
    pseudo += 6;        /* protocol */
    pseudo += 28;       /* TCP header and payload */
    tcp_expect = inet_csum(bare + 34, 28, pseudo);

    /* The card filled both in, and with something, not zero. */
    g_assert_cmphex((got[24] << 8) | got[25], ==, ip_expect);
    g_assert_cmphex((got[50] << 8) | got[51], ==, tcp_expect);
    g_assert_cmpuint((got[24] << 8) | got[25], !=, 0);
    g_assert_cmpuint((got[50] << 8) | got[51], !=, 0);

    /* Everything else went out untouched. */
    g_assert_cmpmem(got, 24, frame, 24);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * The lengths in the frame are the guest's bytes, so they are not to be
 * trusted: a header claiming to be shorter than an IPv4 header can be would
 * run the checksum arithmetic off the front of the buffer. The offload is
 * declined and the frame goes out as it came in.
 */
static void test_transmit_checksum_bad_header(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[CKSUM_FRAME_LEN];
    uint8_t got[128];
    size_t len;

    /* Header length four words: below the twenty bytes IPv4 requires. */
    build_cksum_frame(frame, 0x44);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    len = transmit_one(&b, sv[0], frame, sizeof(frame),
                       BCM_TXBDFLAG_END | BCM_TXBDFLAG_IP_CKSUM |
                       BCM_TXBDFLAG_TCP_UDP_CKSUM, got, sizeof(got));

    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));

    bcm_close(&b);
    close(sv[0]);
}

/*
 * What ENDIAN_WORDSWAP reaches, and what it does not. The bit decides how the
 * chip composes the register-programmed host addresses. The driver writes
 * each 64-bit address as two halves word-swapped, the low word into the HI
 * slot. The DMA must still land where it was aimed.
 *
 * It does not reach the payload here. Behind a Bridge the byte-lane
 * correction is the chip's job and the model copies packet payload verbatim
 * on that route, which is the bounded Bridge-machine behavior this model
 * documents: what the card's own clear-data transformation does with these
 * bits set is not known. Off a Bridge the same bits do reverse each 32-bit
 * word of the payload, which is the regime this file asserted when it ran on
 * a PC. So lay a word-reversed buffer down and require the wire to carry it
 * unchanged.
 */
static void test_transmit_frame_wordswap(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[60];       /* the driver's own byte order */
    uint8_t swapped[60];     /* laid into the buffer word-reversed */
    uint8_t bd[16];
    uint8_t got[128];
    uint32_t misc;
    unsigned int i;
    size_t len;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 7 + 1);
    }
    for (i = 0; i < sizeof(frame); i += 4) {
        swapped[i + 0] = frame[i + 3];
        swapped[i + 1] = frame[i + 2];
        swapped[i + 2] = frame[i + 1];
        swapped[i + 3] = frame[i + 0];
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    /*
     * The driver arms ENDIAN_WORDSWAP for the big-endian host, so the
     * register-programmed host addresses are written word-swapped: the low
     * word into the HI slot, the high word into the LO slot. The descriptor's
     * own buffer address is not register-programmed. DMA accessors read it
     * from the descriptor, so it stays in order.
     */
    misc = qpci_config_readl(b.dev, BCM_PCI_MISC_CTL);
    qpci_config_writel(b.dev, BCM_PCI_MISC_CTL, misc | BCM_MISCCTL_WORDSWAP);

    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 0, TX_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4, TX_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, (uint32_t)STATUS_PHYS);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, TX_BUF_PHYS >> 32);
    put32dma(bd + 4, TX_BUF_PHYS & 0xffffffff);
    put16dma(bd, 8, BCM_TXBDFLAG_END);
    put16dma(bd, 10, sizeof(frame));
    qtest_memwrite(b.qts, TX_RING_PHYS, bd, sizeof(bd));
    qtest_memwrite(b.qts, TX_BUF_PHYS, swapped, sizeof(swapped));
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 1);

    /*
     * The addresses composed, so the descriptor and its buffer were found.
     * The payload went out exactly as it lay in memory: the swap bit did not
     * touch it. Off a Bridge this same buffer would arrive as `frame`.
     */
    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, swapped, sizeof(swapped));

    bcm_close(&b);
    close(sv[0]);
}

/*
 * A producer index past the ring must not hang the mailbox write. The ring
 * holds TX_RING_LEN descriptors, so an index at or beyond it is out of range;
 * the write logs a guest error and returns rather than chasing a consumer it
 * never reaches. The MMIO access after each proves the write returned.
 */
static void test_tx_producer_out_of_range(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());

    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 0, TX_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4, TX_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);

    /* Just past the ring, and far past it: each write must return. */
    bar_writel(&b, BCM_MBX_TX_PROD_LO, TX_RING_LEN);
    g_assert_cmphex(mii_read(&b, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 0xffff);
    g_assert_cmphex(mii_read(&b, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);

    bcm_close(&b);
}

/*
 * The receive round trip. A standard-ring buffer is posted, a broadcast frame
 * injected from the backend, and the chip DMAs it into the buffer, writes a
 * completed descriptor into the return ring, and advances the status block's
 * receive producer. The device-written descriptor's length field is checked
 * as a golden byte layout. It proves the device's descriptor-write
 * direction, which the round trip alone cannot. Broadcast so the address
 * filter passes it regardless of the station address.
 */
/* SGI documents two MTUs for this adapter, 1500 and 9000. */
#define BCM_JUMBO_FRAME_LEN     9000u
/* Larger than the model carries, so it has to be refused rather than cut. */
#define BCM_OVERSIZE_FRAME_LEN  9800u

/*
 * Bring the interface up far enough to receive, with a single posted buffer.
 * Returns with the standard-ring producer rung for one frame.
 */
static void rx_open_one(Bcm *b, int fd)
{
    uint8_t bd[BCM_RXBD_SIZE];

    bar_writel(b, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(b, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(b, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(b, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_PHYS >> 32);
    ind_mem_writel(b, BCM_SRAM_RX_RETURN_RCB + 4, RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(b, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);
    bar_writel(b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    /*
     * One completed descriptor per update is the guide's "for simplicity"
     * setting [57XX-PG105-R, printed page 454]. Left at the reset value of
     * zero the receive count criterion is off, and these tests want to be
     * told about every frame.
     */
    bar_writel(b, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, RX_BUF_PHYS >> 32);
    put32dma(bd + 4, RX_BUF_PHYS & 0xffffffff);
    qtest_memwrite(b->qts, STD_RING_PHYS, bd, sizeof(bd));
    qtest_memset(b->qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(b->qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(b, BCM_MBX_RX_STD_PROD_LO, 1);
    (void)fd;
}

/*
 * The same plumbing with the receive coalescing registers spelled out and as
 * many buffers posted as the test needs, so a test says exactly which update
 * criterion it arms. Both zero means neither is armed.
 */
static void rx_criteria_open(Bcm *b, unsigned int buffers,
                             unsigned int max_bds, unsigned int ticks)
{
    uint8_t bd[BCM_RXBD_SIZE];
    unsigned int i;

    g_assert_cmpuint(buffers, <, COAL_RING_LEN);
    bar_writel(b, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(b, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(b, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(b, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_PHYS >> 32);
    ind_mem_writel(b, BCM_SRAM_RX_RETURN_RCB + 4, RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(b, BCM_SRAM_RX_RETURN_RCB + 8, COAL_RING_LEN << 16);
    bar_writel(b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(b, BCM_HCC_RX_MAX_COAL_BDS, max_bds);
    bar_writel(b, BCM_HCC_RX_COAL_TICKS, ticks);
    bar_writel(b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(b, BCM_MBX_IRQ0_LO, 0);

    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, RX_BUF_PHYS >> 32);
    put32dma(bd + 4, RX_BUF_PHYS & 0xffffffff);
    for (i = 0; i < buffers; i++) {
        qtest_memwrite(b->qts, STD_RING_PHYS + (uint64_t)i * BCM_RXBD_SIZE,
                       bd, sizeof(bd));
    }
    qtest_memset(b->qts, RET_RING_PHYS, 0,
                 (uint64_t)COAL_RING_LEN * BCM_RXBD_SIZE);
    qtest_memset(b->qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(b, BCM_MBX_RX_STD_PROD_LO, buffers);
}

#ifdef CONFIG_LINUX
/*
 * TAP's negotiated vnet header is transport metadata, not Ethernet payload.
 */
static void test_receive_vnet_plain(const void *data)
{
    uint8_t packet[sizeof(struct virtio_net_hdr) + 64] = { };
    uint8_t *frame = packet + sizeof(struct virtio_net_hdr);
    uint8_t got[64];
    Bcm b;
    int tap_fd, mirror_fd, inject_fd;

    memset(frame, 0xa5, 64);
    memset(frame, 0xff, 6);
    if (!bcm_vnet_tap_open(&b, &tap_fd, &mirror_fd, &inject_fd)) {
        return;
    }

    rx_open_one(&b, tap_fd);
    redirector_send_packet(inject_fd, packet, sizeof(packet),
                           sizeof(struct virtio_net_hdr));
    wait_for_rx_prod(&b, 1);
    qtest_memread(b.qts, RX_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), frame, 64);

    bcm_close(&b);
    close(tap_fd);
    close(mirror_fd);
    close(inject_fd);
}
#endif

/*
 * Post one jumbo-ring descriptor carrying a buffer length: the jumbo RCB's
 * own Max_Len (0x2448) is unused by hardware (PG105 page 91, Table 341), so
 * the model reads each buffer's size from the descriptor the driver posts,
 * the same field IRIX fills in for its jumbo descriptors.
 */
static void write_jumbo_rx_bd(Bcm *b, uint64_t ring, unsigned int idx,
                              uint64_t buffer, uint16_t buf_len)
{
    uint8_t bd[BCM_RXBD_SIZE] = { 0 };

    put32dma(bd + 0, buffer >> 32);
    put32dma(bd + 4, buffer & 0xffffffff);
    put16dma(bd, BCM_RXBD_LEN, buf_len);
    put16dma(bd, BCM_RXBD_FLAGS, BCM_RXBDFLAG_JUMBO_RING);
    qtest_memwrite(b->qts, ring + (uint64_t)idx * BCM_RXBD_SIZE,
                   bd, sizeof(bd));
}

/*
 * Post one jumbo-ring buffer alongside the standard ring rx_open_one set up.
 */
static void rx_open_jumbo(Bcm *b)
{
    bar_writel(b, BCM_RX_JUMBO_RCB_HI, JUMBO_RING_PHYS >> 32);
    bar_writel(b, BCM_RX_JUMBO_RCB_LO, JUMBO_RING_PHYS & 0xffffffff);
    bar_writel(b, BCM_RX_JUMBO_RCB_MAXLEN, JUMBO_BUF_SIZE << 16);

    write_jumbo_rx_bd(b, JUMBO_RING_PHYS, 0, JUMBO_BUF_PHYS, JUMBO_BUF_SIZE);
    bar_writel(b, BCM_MBX_RX_JUMBO_PROD_LO, 1);
}

static void write_rx_bd(Bcm *b, uint64_t ring, unsigned int idx,
                        uint64_t buffer)
{
    uint8_t bd[BCM_RXBD_SIZE] = { 0 };

    put32dma(bd + 0, buffer >> 32);
    put32dma(bd + 4, buffer & 0xffffffff);
    qtest_memwrite(b->qts, ring + (uint64_t)idx * BCM_RXBD_SIZE,
                   bd, sizeof(bd));
}

static uint16_t read_rx_prod(Bcm *b)
{
    uint8_t idx[2];

    qtest_memread(b->qts, STATUS_PHYS + DMA16(BCM_STATUS_RX_PROD_OFF),
                  idx, sizeof(idx));
    return get16be(idx);
}

/*
 * A frame does not complete the instant the backend hands it over. The chip
 * writes it into the posted buffer and fills in its return descriptor
 * straight away, and the two things the driver can read (the status block's
 * receive producer and the interrupt) appear only once the arrival deadline
 * has passed on the virtual clock. The return descriptor is invisible until
 * the producer names it, so this checks that it is already written while the
 * producer still is not.
 *
 * Without the floor the model published a reply within tens of microseconds
 * of the transmit that provoked it, faster than the wire alone; the model
 * floors it with an arrival-latency approximation.
 */
static void test_receive_waits_for_its_arrival_deadline(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t landed[64];
    uint8_t ret[BCM_RXBD_SIZE];
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 0x40);
    }
    memset(frame, 0xff, 6);      /* broadcast: no address filter in the way */

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    rx_open_one(&b, sv[1]);
    qtest_memset(b.qts, RX_BUF_PHYS, 0, sizeof(frame));

    /* Unmask, so the line below is a real assertion and not a mask. */
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_false(intr_line_up(b.dev));

    net_socket_send_frame(sv[0], frame, sizeof(frame));

    /*
     * Wait for the chip to take the frame, which it shows by writing it into
     * the buffer. Nothing here advances the virtual clock, so the arrival
     * deadline cannot have passed.
     */
    for (i = 0; i < 200; i++) {
        qtest_memread(b.qts, RX_BUF_PHYS, landed, sizeof(landed));
        if (memcmp(landed, frame, sizeof(frame)) == 0) {
            break;
        }
        g_usleep(5000);
    }
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));

    /* The return descriptor is written; the producer and the line are not. */
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
                    BCM_RXBDFLAG_END);
    g_assert_cmphex(read_rx_prod(&b), ==, 0);
    g_assert_false(intr_line_up(b.dev));

    /* Still nothing one nanosecond short of the deadline. */
    qtest_clock_step(b.qts, RX_ARRIVAL_LATENCY_NS - 1);
    g_assert_cmphex(read_rx_prod(&b), ==, 0);
    g_assert_false(intr_line_up(b.dev));

    /* And both at once past it. */
    qtest_clock_step(b.qts, 1);
    g_assert_cmphex(read_rx_prod(&b), ==, 1);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
}


/*
 * A frame too big for the buffer behind a standard-ring descriptor must not
 * be written into it. The buffer size is the driver's statement of what it
 * set aside; writing past it corrupts whatever the guest put next in memory,
 * and the damage surfaces as a fault somewhere with no connection to this
 * device. Observed for real: at MTU 9000 with no jumbo ring posted, the guest
 * kernel panicked.
 */
static void test_receive_overruns_nothing(const void *data)
{
    int sv[2];
    Bcm b;
    const unsigned int len = 4096;      /* larger than STD_BUF_SIZE */
    uint8_t *frame = g_malloc(len);
    uint8_t *guard = g_malloc(len);
    unsigned int i;

    for (i = 0; i < len; i++) {
        frame[i] = (uint8_t)(i * 7 + 1);
    }
    memset(frame, 0xff, 6);             /* broadcast destination */

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_open_one(&b, sv[0]);             /* standard ring; no jumbo ring */

    /* Lay a pattern over the buffer and well past its end. */
    memset(guard, 0xa5, len);
    qtest_memwrite(b.qts, RX_BUF_PHYS, guard, len);

    net_socket_send_frame(sv[0], frame, len);

    /*
     * Nothing beyond the posted buffer may have moved. Whether the frame is
     * refused outright or cut to fit is the model's business; overwriting the
     * guest's memory past the buffer is not.
     */
    qtest_memread(b.qts, RX_BUF_PHYS + STD_BUF_SIZE, guard, len - STD_BUF_SIZE);
    for (i = 0; i < len - STD_BUF_SIZE; i++) {
        g_assert_cmphex(guard[i], ==, 0xa5);
    }

    bcm_close(&b);
    close(sv[0]);
    g_free(frame);
    g_free(guard);
}

/*
 * A jumbo frame arrives whole, on the ring that exists for it. SGI's board
 * guide gives this adapter two MTUs, 1500 and 9000; the part carries the
 * larger one on a separate receive ring whose buffers are sized for it, and
 * marks the completion so the driver recycles from the right ring.
 */
static void test_receive_jumbo_frame(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t *frame = g_malloc(BCM_JUMBO_FRAME_LEN);
    uint8_t *got = g_malloc(BCM_JUMBO_FRAME_LEN);
    uint8_t ret[BCM_RXBD_SIZE];
    unsigned int i;

    for (i = 0; i < BCM_JUMBO_FRAME_LEN; i++) {
        frame[i] = (uint8_t)(i * 7 + 1);
    }
    memset(frame, 0xff, 6);     /* broadcast destination */

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_open_one(&b, sv[0]);
    rx_open_jumbo(&b);
    qtest_memset(b.qts, JUMBO_BUF_PHYS, 0, BCM_JUMBO_FRAME_LEN);

    net_socket_send_frame(sv[0], frame, BCM_JUMBO_FRAME_LEN);
    wait_for_rx_prod(&b, 1);

    /*
     * Reported whole, marked as the jumbo ring's, and landed byte for byte.
     */
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_LEN), ==,
                    BCM_JUMBO_FRAME_LEN + BCM_ETH_FCS_LEN);
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_JUMBO_RING,
                    ==, BCM_RXBDFLAG_JUMBO_RING);
    qtest_memread(b.qts, JUMBO_BUF_PHYS, got, BCM_JUMBO_FRAME_LEN);
    g_assert_cmpmem(got, BCM_JUMBO_FRAME_LEN, frame, BCM_JUMBO_FRAME_LEN);

    bcm_close(&b);
    close(sv[0]);
    g_free(frame);
    g_free(got);
}

/*
 * A small frame belongs to the standard ring even when a jumbo buffer is
 * available, and it cannot borrow that jumbo descriptor: an unposted
 * descriptor is stale guest memory, not permission to DMA through it. With
 * the standard ring empty it has nowhere to go, and the chip discards it --
 * the same disposition the jumbo shortage gets, for the same reason. Refusing
 * it stops the back end reading and strands the jumbo frame behind it, on a
 * ring that does have a buffer.
 *
 * The last step proves the frame was discarded rather than parked: posting a
 * standard buffer afterwards brings nothing back.
 *
 * The manual's counter for this, `nicNoMoreRXBDs`, names no ring (PG105 page
 * 110, Table 49; page 114, Table 54 gives it one offset with no per-ring
 * companion), so this discard counts at the same offset the jumbo case bumps.
 * Read before the drop, immediately after, and once more at the end, the same
 * three points the jumbo test checks.
 */
static void test_receive_drops_standard_with_empty_standard_ring(
    const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t first[64], second[64], got[64], untouched[64];
    g_autofree uint8_t *big = g_malloc(BCM_JUMBO_FRAME_LEN);
    g_autofree uint8_t *big_got = g_malloc(BCM_JUMBO_FRAME_LEN);
    uint8_t ret[BCM_RXBD_SIZE];

    memset(first, 0x31, sizeof(first));
    memset(second, 0x42, sizeof(second));
    memset(big, 0x53, BCM_JUMBO_FRAME_LEN);
    memset(untouched, 0xa5, sizeof(untouched));
    memset(first, 0xff, 6);
    memset(second, 0xff, 6);
    memset(big, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_open_one(&b, sv[0]);
    rx_open_jumbo(&b);
    write_rx_bd(&b, STD_RING_PHYS, 1, STD_STALE_BUF_PHYS);

    g_assert_cmpuint(bar_readl(&b, BCM_SRAM_WINDOW +
                                BCM_SRAM_STATS_NO_MORE_RXBDS), ==, 0);

    net_socket_send_frame(sv[0], first, sizeof(first));
    wait_for_rx_prod(&b, 1);
    bar_writel(&b, BCM_MBX_RX_CONS0_LO, 1);

    /*
     * The standard ring is now empty; the jumbo ring still holds its buffer.
     */
    qtest_memset(b.qts, RET_RING_PHYS + BCM_RXBD_SIZE, 0, BCM_RXBD_SIZE);
    qtest_memset(b.qts, STD_STALE_BUF_PHYS, 0xa5, sizeof(second));
    qtest_memset(b.qts, JUMBO_BUF_PHYS, 0xa5, sizeof(second));
    net_socket_send_frame(sv[0], second, sizeof(second));
    g_usleep(100000);

    /* Neither buffer written, and no completion. */
    g_assert_cmpuint(read_rx_prod(&b), ==, 1);
    qtest_memread(b.qts, STD_STALE_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), untouched, sizeof(untouched));
    qtest_memread(b.qts, JUMBO_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), untouched, sizeof(untouched));

    /*
     * The chip counts this rather than latching it (PG105 page 110): one
     * frame dropped for want of a posted standard descriptor, one count.
     */
    g_assert_cmpuint(bar_readl(&b, BCM_SRAM_WINDOW +
                                BCM_SRAM_STATS_NO_MORE_RXBDS), ==, 1);

    /* Unmask the existing update, then acknowledge it. */
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_true(intr_line_up(b.dev));
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_false(intr_line_up(b.dev));

    /* The receiver is not latched: a frame for the other ring still lands. */
    qtest_memset(b.qts, JUMBO_BUF_PHYS, 0, BCM_JUMBO_FRAME_LEN);
    net_socket_send_frame(sv[0], big, BCM_JUMBO_FRAME_LEN);
    wait_for_rx_prod(&b, 2);
    qtest_memread(b.qts, JUMBO_BUF_PHYS, big_got, BCM_JUMBO_FRAME_LEN);
    g_assert_cmpmem(big_got, BCM_JUMBO_FRAME_LEN, big, BCM_JUMBO_FRAME_LEN);
    qtest_memread(b.qts, RET_RING_PHYS + BCM_RXBD_SIZE, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_LEN), ==,
                    BCM_JUMBO_FRAME_LEN + BCM_ETH_FCS_LEN);
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) &
                    BCM_RXBDFLAG_JUMBO_RING, ==, BCM_RXBDFLAG_JUMBO_RING);

    /* Posting a standard buffer brings nothing back: that frame is gone. */
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 2);
    g_usleep(100000);
    g_assert_cmpuint(read_rx_prod(&b), ==, 2);
    qtest_memread(b.qts, STD_STALE_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), untouched, sizeof(untouched));

    /*
     * Neither the delivered jumbo frame nor the late post is this condition.
     */
    g_assert_cmpuint(bar_readl(&b, BCM_SRAM_WINDOW +
                                BCM_SRAM_STATS_NO_MORE_RXBDS), ==, 1);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * The inverse case is not the same contract. A jumbo frame still cannot
 * borrow a posted standard descriptor. A frame "greater than the Max_Len
 * field of Receive Standard Producer Ring will consume a BD from Receive
 * Jumbo Producer Ring" [57XX-PG105-R, printed page 92]. An empty jumbo
 * ring does not park it either, because parking it would stop the back end
 * and strand the standard-size frames behind it.
 *
 * What the manual does not say is what becomes of the frame itself: it never
 * addresses a host producer ring with no descriptor at all. The discard the
 * last step pins is the model's own approximation of that unspecified case;
 * holding the frame briefly instead was tried and rejected by measurement.
 *
 * The manual does say the chip counts this condition rather than latching it
 * (`nicNoMoreRXBDs`, PG105 page 110), so the discard has to reach the guest
 * there too: the test reads BCM_SRAM_STATS_NO_MORE_RXBDS before and after the
 * dropped frame and asserts it moved by exactly one, then stays there through
 * every step that follows. Neither the delivered standard frame nor the
 * belated jumbo post is the condition this counter names.
 */
static void test_receive_drops_oversize_with_empty_jumbo_ring(const void *data)
{
    int sv[2];
    Bcm b;
    g_autofree uint8_t *first = g_malloc(BCM_JUMBO_FRAME_LEN);
    g_autofree uint8_t *second = g_malloc(BCM_JUMBO_FRAME_LEN);
    g_autofree uint8_t *got = g_malloc(BCM_JUMBO_FRAME_LEN);
    uint8_t small[64], small_got[64];
    uint8_t ret[BCM_RXBD_SIZE];

    memset(first, 0x53, BCM_JUMBO_FRAME_LEN);
    memset(second, 0x64, BCM_JUMBO_FRAME_LEN);
    memset(small, 0x75, sizeof(small));
    memset(first, 0xff, 6);
    memset(second, 0xff, 6);
    memset(small, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_open_one(&b, sv[0]);
    rx_open_jumbo(&b);
    write_jumbo_rx_bd(&b, JUMBO_RING_PHYS, 1, JUMBO_STALE_BUF_PHYS,
                      JUMBO_BUF_SIZE);

    g_assert_cmpuint(bar_readl(&b, BCM_SRAM_WINDOW +
                                BCM_SRAM_STATS_NO_MORE_RXBDS), ==, 0);

    net_socket_send_frame(sv[0], first, BCM_JUMBO_FRAME_LEN);
    wait_for_rx_prod(&b, 1);
    bar_writel(&b, BCM_MBX_RX_CONS0_LO, 1);

    /*
     * The jumbo ring is now empty; the standard ring still holds its buffer.
     */
    qtest_memset(b.qts, RET_RING_PHYS + BCM_RXBD_SIZE, 0, BCM_RXBD_SIZE);
    qtest_memset(b.qts, JUMBO_STALE_BUF_PHYS, 0xa5,
                 BCM_JUMBO_FRAME_LEN);
    qtest_memset(b.qts, RX_BUF_PHYS, 0xa5, sizeof(small));
    net_socket_send_frame(sv[0], second, BCM_JUMBO_FRAME_LEN);
    g_usleep(100000);

    g_assert_cmpuint(read_rx_prod(&b), ==, 1);
    qtest_memread(b.qts, JUMBO_STALE_BUF_PHYS, got, BCM_JUMBO_FRAME_LEN);
    memset(first, 0xa5, BCM_JUMBO_FRAME_LEN);
    g_assert_cmpmem(got, BCM_JUMBO_FRAME_LEN,
                    first, BCM_JUMBO_FRAME_LEN);

    /*
     * The chip counts this rather than latching it (PG105 page 110): one
     * frame dropped for want of a posted jumbo descriptor, one count.
     */
    g_assert_cmpuint(bar_readl(&b, BCM_SRAM_WINDOW +
                                BCM_SRAM_STATS_NO_MORE_RXBDS), ==, 1);

    /* Acknowledge the first frame's update, so the next one stands alone. */
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_true(intr_line_up(b.dev));
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_false(intr_line_up(b.dev));

    /*
     * The receiver is not latched: the frame behind the dropped one arrives.
     */
    net_socket_send_frame(sv[0], small, sizeof(small));
    wait_for_rx_prod(&b, 2);
    qtest_memread(b.qts, RX_BUF_PHYS, small_got, sizeof(small_got));
    g_assert_cmpmem(small_got, sizeof(small_got), small, sizeof(small));
    qtest_memread(b.qts, RET_RING_PHYS + BCM_RXBD_SIZE, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_LEN), ==,
                    sizeof(small) + BCM_ETH_FCS_LEN);
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) &
                    BCM_RXBDFLAG_JUMBO_RING, ==, 0);

    /* Posting a jumbo buffer brings nothing back: that frame is gone. */
    bar_writel(&b, BCM_MBX_RX_JUMBO_PROD_LO, 2);
    g_usleep(100000);
    g_assert_cmpuint(read_rx_prod(&b), ==, 2);
    qtest_memread(b.qts, JUMBO_STALE_BUF_PHYS, got, BCM_JUMBO_FRAME_LEN);
    g_assert_cmpmem(got, BCM_JUMBO_FRAME_LEN,
                    first, BCM_JUMBO_FRAME_LEN);

    /* Neither the delivered frame nor the late post is this condition. */
    g_assert_cmpuint(bar_readl(&b, BCM_SRAM_WINDOW +
                                BCM_SRAM_STATS_NO_MORE_RXBDS), ==, 1);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * Fill in a frame's real IP and TCP checksums, so the card is asked to verify
 * rather than to compute.
 */
static void seal_cksum_frame(uint8_t *f)
{
    uint16_t sum;
    uint32_t pseudo;

    f[24] = f[25] = 0;
    sum = inet_csum(f + 14, 20, 0);
    f[24] = sum >> 8;
    f[25] = sum & 0xff;

    f[50] = f[51] = 0;
    pseudo = 0;
    pseudo += ((uint32_t)f[26] << 8) | f[27];
    pseudo += ((uint32_t)f[28] << 8) | f[29];
    pseudo += ((uint32_t)f[30] << 8) | f[31];
    pseudo += ((uint32_t)f[32] << 8) | f[33];
    pseudo += 6;        /* protocol */
    pseudo += 28;       /* TCP header and payload */
    sum = inet_csum(f + 34, 28, pseudo);
    f[50] = sum >> 8;
    f[51] = sum & 0xff;
}

/*
 * A received IPv4/TCP frame carries its checksum results in the completed
 * descriptor. The part verifies both on the way in and reports the sums,
 * which is what lets a driver hand the frame up without reading the payload
 * again; with nothing reported the stack has to make its own pass over every
 * byte.
 */
static void test_receive_checksum_offload(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[CKSUM_FRAME_LEN];
    uint8_t ret[BCM_RXBD_SIZE];
    uint16_t flags;

    build_cksum_frame(frame, 0x45);
    memset(frame, 0xff, 6);         /* broadcast destination */
    seal_cksum_frame(frame);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_open_one(&b, sv[0]);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_rx_prod(&b, 1);

    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    flags = get16dma(ret, BCM_RXBD_FLAGS);

    /* Both results are reported... */
    g_assert_cmphex(flags & BCM_RXBDFLAG_IP_CKSUM, ==, BCM_RXBDFLAG_IP_CKSUM);
    g_assert_cmphex(flags & BCM_RXBDFLAG_TCP_UDP_CKSUM, ==,
                    BCM_RXBDFLAG_TCP_UDP_CKSUM);
    /* ...and both check out, which is what the sums come to when they do. */
    g_assert_cmphex(get16dma(ret, BCM_RXBD_IP_CSUM), ==, BCM_CSUM_GOOD);
    g_assert_cmphex(get16dma(ret, BCM_RXBD_TCP_CSUM), ==, BCM_CSUM_GOOD);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * A frame the card cannot check is reported as unchecked rather than as good.
 * Claiming a sum for something never summed would have the stack skip its own
 * pass on the strength of it.
 */
static void test_receive_checksum_not_ip(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[CKSUM_FRAME_LEN];
    uint8_t ret[BCM_RXBD_SIZE];
    uint16_t flags;

    build_cksum_frame(frame, 0x45);
    memset(frame, 0xff, 6);         /* broadcast destination */
    seal_cksum_frame(frame);
    frame[12] = 0x08;               /* ARP, not IPv4 */
    frame[13] = 0x06;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_open_one(&b, sv[0]);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_rx_prod(&b, 1);

    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    flags = get16dma(ret, BCM_RXBD_FLAGS);

    g_assert_cmphex(flags & BCM_RXBDFLAG_IP_CKSUM, ==, 0);
    g_assert_cmphex(flags & BCM_RXBDFLAG_TCP_UDP_CKSUM, ==, 0);
    /* The frame still arrives; only the checksum claim is withheld. */
    g_assert_cmphex(flags & BCM_RXBDFLAG_END, ==, BCM_RXBDFLAG_END);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * A frame larger than the model carries is refused, not cut down. Delivering
 * part of it would hand the guest something that looks whole and is not, and
 * the damage would surface wherever the payload is finally read rather than
 * here.
 */
static void test_receive_oversize_dropped(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t *frame = g_malloc0(BCM_OVERSIZE_FRAME_LEN);
    uint8_t ret[BCM_RXBD_SIZE];
    uint8_t small[64];
    unsigned int i;

    memset(frame, 0xff, 6);
    for (i = 0; i < sizeof(small); i++) {
        small[i] = (uint8_t)(i + 9);
    }
    memset(small, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_open_one(&b, sv[0]);

    net_socket_send_frame(sv[0], frame, BCM_OVERSIZE_FRAME_LEN);

    /*
     * Nothing completes for it. A frame that does fit, sent after, still
     * arrives and takes descriptor zero. The ring was left usable rather
     * than the oversized frame half-consuming it.
     */
    net_socket_send_frame(sv[0], small, sizeof(small));
    wait_for_rx_prod(&b, 1);

    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_LEN), ==,
                    sizeof(small) + BCM_ETH_FCS_LEN);

    bcm_close(&b);
    close(sv[0]);
    g_free(frame);
}

/*
 * Many laps of the return ring, driven the way the driver drives it: take
 * each completion, then publish the consumer index so the card knows the
 * entry is free again. A ring short enough to wrap several times over a
 * modest burst is the point. These faults only appear once the
 * producer has been round at least once, which is why a transfer that fits in
 * one lap passes happily and a longer one stops dead.
 */
#define LAP_RET_LEN     8u
#define LAP_FRAMES      40u     /* five times round */
#define LAP_STD_POSTED  64u

static void test_receive_ring_laps(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t bd[BCM_RXBD_SIZE];
    uint8_t ret[BCM_RXBD_SIZE];
    unsigned int i, j;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 2);
    }
    memset(frame, 0xff, 6);     /* broadcast: always accepted */

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    bar_writel(&b, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, 1518u << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4, RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, LAP_RET_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    /* Post standard-ring buffers, all pointing at one scratch buffer. */
    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, RX_BUF_PHYS >> 32);
    put32dma(bd + 4, RX_BUF_PHYS & 0xffffffff);
    for (i = 0; i < LAP_STD_POSTED; i++) {
        qtest_memwrite(b.qts, STD_RING_PHYS + (uint64_t)i * BCM_RXBD_SIZE,
                       bd, sizeof(bd));
    }
    qtest_memset(b.qts, RET_RING_PHYS, 0,
                 (uint64_t)LAP_RET_LEN * BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, LAP_STD_POSTED);

    for (i = 0; i < LAP_FRAMES; i++) {
        unsigned int idx = i % LAP_RET_LEN;
        uint64_t slot = RET_RING_PHYS + (uint64_t)idx * BCM_RXBD_SIZE;
        bool got = false;

        /* Clear the entry so its completion cannot be mistaken for an older
         * one still sitting there from the previous lap. */
        qtest_memset(b.qts, slot, 0, BCM_RXBD_SIZE);
        net_socket_send_frame(sv[0], frame, sizeof(frame));

        for (j = 0; j < 200 && !got; j++) {
            qtest_memread(b.qts, slot, ret, sizeof(ret));
            if (get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END) {
                got = true;
                break;
            }
            g_usleep(5000);
            qtest_clock_step(b.qts, RX_ARRIVAL_LATENCY_NS / 4);
        }
        g_assert_true(got);
        g_assert_cmphex(get16dma(ret, BCM_RXBD_LEN), ==,
                        sizeof(frame) + BCM_ETH_FCS_LEN);

        /* Release the entry, as the driver does once it has taken it. */
        bar_writel(&b, BCM_MBX_RX_CONS0_LO, (idx + 1) % LAP_RET_LEN);
        /*
         * And keep buffers posted, the way a driver refills as it consumes.
         */
        bar_writel(&b, BCM_MBX_RX_STD_PROD_LO,
                   (LAP_STD_POSTED + i + 1) % 512u);
    }

    bcm_close(&b);
    close(sv[0]);
}

/*
 * The ring fills, and then it recovers. Refusing a frame while the driver
 * still owes itself every entry is right; staying refused after the driver
 * has caught up is the failure that looks like a link going dead
 * mid-transfer, and it is the one worth pinning, because a full ring is
 * reached on any sustained transfer and a card that never comes back from it
 * is a card that works until it is used.
 */
static void test_receive_ring_full_recovers(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t bd[BCM_RXBD_SIZE];
    uint8_t ret[BCM_RXBD_SIZE];
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 3);
    }
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    bar_writel(&b, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, 1518u << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4, RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, LAP_RET_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    /*
     * One completed descriptor per update [57XX-PG105-R, printed page 454].
     */
    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, RX_BUF_PHYS >> 32);
    put32dma(bd + 4, RX_BUF_PHYS & 0xffffffff);
    for (i = 0; i < LAP_STD_POSTED; i++) {
        qtest_memwrite(b.qts, STD_RING_PHYS + (uint64_t)i * BCM_RXBD_SIZE,
                       bd, sizeof(bd));
    }
    qtest_memset(b.qts, RET_RING_PHYS, 0,
                 (uint64_t)LAP_RET_LEN * BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, LAP_STD_POSTED);

    /* Send a whole ring's worth without ever taking one, so it fills. */
    for (i = 0; i < LAP_RET_LEN * 2 + 4; i++) {
        net_socket_send_frame(sv[0], frame, sizeof(frame));
    }
    /*
     * Each completion waits for its own arrival deadline, so run the clock on
     * as well as the main loop until the ring has taken everything it can.
     */
    for (i = 0; i < 40; i++) {
        g_usleep(5000);
        qtest_clock_step(b.qts, RX_ARRIVAL_LATENCY_NS);
    }

    /* The last entry is still free: a full ring keeps one, so the producer
     * cannot be mistaken for the consumer. */
    qtest_memread(b.qts, RET_RING_PHYS +
                  (uint64_t)(LAP_RET_LEN - 1) * BCM_RXBD_SIZE,
                  ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==, 0);

    /*
     * An interrupt cycle with the ring still full changes nothing: the frames
     * are waiting on return-ring space, not on the driver's acknowledgement.
     */
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_true(intr_line_up(b.dev));
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_false(intr_line_up(b.dev));
    g_usleep(100000);
    qtest_clock_step(b.qts, RX_ARRIVAL_LATENCY_NS);
    qtest_memread(b.qts, RET_RING_PHYS +
                  (uint64_t)(LAP_RET_LEN - 1) * BCM_RXBD_SIZE,
                  ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==, 0);

    /*
     * The driver now takes everything and says so. That consumer write is the
     * readiness transition. It retries without another interrupt
     * acknowledgement, after the arrival line's delay.
     */
    for (i = 0; i < LAP_RET_LEN; i++) {
        qtest_memset(b.qts, RET_RING_PHYS + (uint64_t)i * BCM_RXBD_SIZE, 0,
                     BCM_RXBD_SIZE);
    }
    bar_writel(&b, BCM_MBX_RX_CONS0_LO, LAP_RET_LEN - 1);

    /* The retry fills the ring a second time. */
    wait_for_rx_prod(&b, LAP_RET_LEN - 2);
    qtest_memread(b.qts, RET_RING_PHYS +
                  (uint64_t)(LAP_RET_LEN - 2) * BCM_RXBD_SIZE,
                  ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==, 0);

    /*
     * And again for the second ring's worth. Freeing return-ring space is the
     * whole condition; the interrupt from the first refill is still pending
     * and unacknowledged.
     */
    for (i = 0; i < LAP_RET_LEN; i++) {
        qtest_memset(b.qts, RET_RING_PHYS + (uint64_t)i * BCM_RXBD_SIZE, 0,
                     BCM_RXBD_SIZE);
    }
    bar_writel(&b, BCM_MBX_RX_CONS0_LO, LAP_RET_LEN - 2);
    wait_for_rx_prod(&b, LAP_RET_LEN - 4);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * The receive MAC's ENABLE bit, and what it means for a frame that arrives
 * while it is clear. The bit "controls whether the Receive MAC state machine
 * is active or not" and resets to 0 [57XX-PG105-R, printed page 391, Table
 * 268], so a halted receiver takes nothing and keeps nothing: setting the bit
 * later must not produce frames from before it was set. That is the whole of
 * the wedge this pins. A back end holds what the device refuses, and IRIX
 * was handed traffic from before the interface existed, mid-bring-up, in two
 * of five boots.
 *
 * A model that simply never delivered would pass half of this, so each halted
 * stretch is followed by a fresh frame that has to land, and the bit is
 * cleared again afterwards to show the halt is not a one-time bring-up
 * special case.
 *
 * The manual counts what a running receiver drops for want of descriptors
 * (nicNoMoreRXBDs, printed page 110, Table 49), but a halted receiver is a
 * different condition: nothing is placed anywhere to run short of. That
 * counter does not apply here, and there is no other one to assert.
 */
static void test_receive_mac_enable(const void *data)
{
    enum { FRAMES = 4 };
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t bd[BCM_RXBD_SIZE];
    uint8_t ret[BCM_RXBD_SIZE];
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 11);
    }
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    /* Everything ready and the doorbell rung, but the receive MAC halted. */
    bar_writel(&b, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, 1518u << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4, RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, LAP_RET_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);

    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, RX_BUF_PHYS >> 32);
    put32dma(bd + 4, RX_BUF_PHYS & 0xffffffff);
    for (i = 0; i < 8; i++) {
        qtest_memwrite(b.qts, STD_RING_PHYS + (uint64_t)i * BCM_RXBD_SIZE,
                       bd, sizeof(bd));
    }
    qtest_memset(b.qts, RET_RING_PHYS, 0,
                 (uint64_t)LAP_RET_LEN * BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 8);

    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    /*
     * One completed descriptor per update [57XX-PG105-R, printed page 454].
     */
    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    g_assert_cmphex(bar_readl(&b, BCM_RX_MODE) & BCM_RXMODE_ENABLE, ==, 0);

    for (i = 0; i < FRAMES; i++) {
        net_socket_send_frame(sv[0], frame, sizeof(frame));
    }
    g_usleep(100000);
    g_assert_cmpuint(read_rx_prod(&b), ==, 0);

    /* Starting the state machine must not produce any of them. */
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    g_assert_cmphex(bar_readl(&b, BCM_RX_MODE) & BCM_RXMODE_ENABLE, ==,
                    BCM_RXMODE_ENABLE);
    g_usleep(100000);
    g_assert_cmpuint(read_rx_prod(&b), ==, 0);
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==, 0);

    /* Running now, so a frame sent now lands. */
    memset(frame, 0x5e, sizeof(frame));
    memset(frame, 0xff, 6);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_rx_prod(&b, 1);
    g_assert_true(intr_line_up(b.dev));
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
                    BCM_RXBDFLAG_END);
    g_assert_cmphex(get16dma(ret, BCM_RXBD_LEN), ==,
                    sizeof(frame) + BCM_ETH_FCS_LEN);

    /*
     * Halting again takes the receiver back out, mid-life and not only at
     * reset: what arrives while it is clear is gone when it is set again.
     */
    bar_writel(&b, BCM_RX_MODE, 0);
    memset(frame, 0x7a, sizeof(frame));
    memset(frame, 0xff, 6);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    g_usleep(100000);
    g_assert_cmpuint(read_rx_prod(&b), ==, 1);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    g_usleep(100000);
    g_assert_cmpuint(read_rx_prod(&b), ==, 1);

    memset(frame, 0x3c, sizeof(frame));
    memset(frame, 0xff, 6);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_rx_prod(&b, 2);
    qtest_memread(b.qts, RET_RING_PHYS + BCM_RXBD_SIZE, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
                    BCM_RXBDFLAG_END);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * A frame the chip could not place waits for a buffer, not for an interrupt.
 * The programmer's guide gives the producer mailbox as the resumption event:
 * "When the host software updates one of these indices, the BCM57XX family is
 * automatically signaled that a new BD is waiting for DMA" (57XX-PG105-R,
 * Receive Producer Ring, page 124), and the RX List Initiator "activates
 * whenever the receive producer index for any of receive Buffer Descriptor
 * (BD) rings is written" (page 51). Drivers rely on it: OpenBSD bge refills
 * from a timeout with no interrupt in sight, and Linux tg3 refills inside
 * NAPI poll while its interrupt is still masked.
 *
 * A false pass here is a frame delivered because something else unmasked the
 * interrupt, so nothing in this test writes the interrupt mailbox after the
 * frame is queued, and the producer write is the only event between the
 * refusal and the delivery.
 */
static void test_queued_receive_released_by_producer(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64], got[64];
    uint8_t ret[BCM_RXBD_SIZE];

    memset(frame, 0x7c, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    bar_writel(&b, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4,
                   RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    write_rx_bd(&b, STD_RING_PHYS, 0, RX_BUF_PHYS);
    qtest_memset(b.qts, RX_BUF_PHYS, 0, sizeof(frame));
    qtest_memset(b.qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);

    /*
     * Bring-up order: interrupts enabled, engine enabled, no buffer posted.
     */
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    /*
     * One completed descriptor per update [57XX-PG105-R, printed page 454].
     */
    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    g_usleep(100000);
    g_assert_cmpuint(read_rx_prod(&b), ==, 0);
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==, 0);

    /* The one event the guest makes: a buffer posted through the mailbox. */
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);

    wait_for_rx_prod(&b, 1);
    qtest_memread(b.qts, RX_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_IDX), ==, 0);
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
                    BCM_RXBDFLAG_END);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * A reset discards receive work queued against the old ring configuration and
 * re-enables backend delivery. Queue one frame while the device is unready,
 * reset it, build fresh rings, then prove a distinct post-reset frame reaches
 * the new buffer. This exercises both the driver-triggered global reset and a
 * machine reset because they share the same network-queue boundary.
 */
static void test_receive_queue_reset(const void *data)
{
    bool system_reset = GPOINTER_TO_INT(data);
    int sv[2];
    Bcm b;
    uint8_t stale[64];
    uint8_t fresh[64];
    uint8_t got[64];

    memset(stale, 0x51, sizeof(stale));
    memset(stale, 0xff, 6);
    memset(fresh, 0xa7, sizeof(fresh));
    memset(fresh, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    net_socket_send_frame(sv[0], stale, sizeof(stale));
    g_usleep(100000);

    if (system_reset) {
        qtest_qmp_assert_success(b.qts, "{ 'execute': 'system_reset' }");
        qtest_qmp_eventwait(b.qts, "RESET");
        qpci_device_enable(b.dev);
        b.bar = qpci_iomap(b.dev, 0, NULL);
    } else {
        bar_writel(&b, BCM_MISC_CFG, BCM_MISCCFG_RESET);
    }

    rx_open_one(&b, sv[0]);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    net_socket_send_frame(sv[0], fresh, sizeof(fresh));
    wait_for_rx_prod(&b, 1);
    qtest_memread(b.qts, RX_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), fresh, sizeof(fresh));

    bcm_close(&b);
    close(sv[0]);
}

static void test_receive_round_trip(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t bd[BCM_RXBD_SIZE];
    uint8_t ret[BCM_RXBD_SIZE];
    uint8_t got[64];
    uint8_t sb[BCM_STATUS_BLK_SIZE];
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 5 + 3);
    }
    memset(frame, 0xff, 6);     /* broadcast destination */

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    /* Standard-ring RCB in CSRs, return-ring RCB in SRAM, status block on. */
    bar_writel(&b, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4, RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    /*
     * One completed descriptor per update [57XX-PG105-R, printed page 454].
     */
    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    /* Post one standard-ring buffer at index 0, in the Bridge's order. */
    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, RX_BUF_PHYS >> 32);
    put32dma(bd + 4, RX_BUF_PHYS & 0xffffffff);
    qtest_memwrite(b.qts, STD_RING_PHYS, bd, sizeof(bd));
    qtest_memset(b.qts, RX_BUF_PHYS, 0, sizeof(frame));
    qtest_memset(b.qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);

    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_rx_prod(&b, 1);

    /* The completed descriptor landed in the return ring. */
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_IDX), ==, 0);
    g_assert_cmphex(get16dma(ret, BCM_RXBD_LEN), ==,
                    sizeof(frame) + BCM_ETH_FCS_LEN);
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
                    BCM_RXBDFLAG_END);
    /*
     * Golden bytes on the length field: 64 payload + a 4-byte FCS is 68. Off
     * a Bridge those bytes are {0x44, 0x00} at the field's own offset; here
     * the word swap puts the halfword in the other half of its word and
     * writes it big-endian.
     */
    g_assert_cmphex(ret[DMA16(BCM_RXBD_LEN)], ==, 0x00);
    g_assert_cmphex(ret[DMA16(BCM_RXBD_LEN) + 1], ==, 0x44);

    /* The status block's receive producer advanced, in the same order. */
    qtest_memread(b.qts, STATUS_PHYS, sb, sizeof(sb));
    g_assert_cmphex(get16dma(sb, BCM_STATUS_RX_PROD_OFF), ==, 1);
    g_assert_cmphex(sb[DMA16(BCM_STATUS_RX_PROD_OFF)], ==, 0x00);
    g_assert_cmphex(sb[DMA16(BCM_STATUS_RX_PROD_OFF) + 1], ==, 0x01);

    /* And the frame reached the posted buffer byte-for-byte. */
    qtest_memread(b.qts, RX_BUF_PHYS, got, sizeof(frame));
    g_assert_cmpmem(got, sizeof(frame), frame, sizeof(frame));

    /*
     * The driver's return-ring acknowledgement is accepted (write returns).
     */
    bar_writel(&b, BCM_MBX_RX_CONS0_LO, 1);
    g_assert_cmphex(mii_read(&b, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);

    bcm_close(&b);
    close(sv[0]);
}

/* A refused descriptor read leaves the queued frame and slot retryable. */
static void test_receive_descriptor_dma_error(const void *data)
{
    g_autofree char *logfile = NULL;
    uint8_t frame[64], landed[sizeof(frame)] = { 0 };
    uint8_t zero_landed[sizeof(landed)] = { 0 };
    uint8_t ret[BCM_RXBD_SIZE], zero[BCM_RXBD_SIZE] = { 0 };
    Bcm b;
    unsigned int i;
    uint32_t tag;
    int logfd;
    int sv[2];

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 5 + 3);
    }
    memset(frame, 0xff, 6);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    logfd = g_file_open_tmp("bcm570x-rx-descriptor-error-XXXXXX", &logfile,
                            NULL);
    g_assert_cmpint(logfd, >=, 0);
    close(logfd);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0"
                             " -d guest_errors -D %s",
                             sv[1], BCM_SLOT, logfile));
    rx_open_one(&b, sv[0]);
    bar_writel(&b, BCM_RX_STD_RCB_HI, UNMAPPED_PHYS >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_LO, UNMAPPED_PHYS & 0xffffffff);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_log_marker(b.qts, logfile,
                        "bcm570x: receive descriptor 0 read failed");
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmpmem(ret, sizeof(ret), zero, sizeof(zero));
    qtest_memread(b.qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), zero_landed,
                    sizeof(zero_landed));
    g_assert_cmphex(read_status_tag(&b), ==, tag);
    g_assert_false(intr_line_up(b.dev));

    /* Repair only the descriptor ring; retry the already queued frame. */
    bar_writel(&b, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);
    wait_for_rx_prod(&b, 1);
    qtest_memread(b.qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));

    bcm_close(&b);
    close(sv[0]);
    close(sv[1]);
    unlink(logfile);
}

/* A refused completion write leaves the queued frame and slot retryable. */
static void test_receive_completion_dma_error(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t landed[sizeof(frame)];
    uint8_t ret[BCM_RXBD_SIZE];
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 5 + 3);
    }
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_open_one(&b, sv[0]);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 0,
                   UNMAPPED_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4,
                   UNMAPPED_PHYS & 0xffffffff);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    for (i = 0; i < 200; i++) {
        qtest_memread(b.qts, RX_BUF_PHYS, landed, sizeof(landed));
        if (memcmp(landed, frame, sizeof(frame)) == 0) {
            break;
        }
        g_usleep(5000);
    }
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));
    g_assert_cmphex(read_rx_prod(&b), ==, 0);

    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4,
                   RET_RING_PHYS & 0xffffffff);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);
    wait_for_rx_prod(&b, 1);
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_IDX), ==, 0);

    bcm_close(&b);
    close(sv[0]);
}

/* A refused payload write must not publish or consume the receive entry. */
static void test_receive_payload_dma_error(const void *data)
{
    g_autofree char *logfile = NULL;
    uint8_t frame[64], landed[sizeof(frame)];
    uint8_t bd[BCM_RXBD_SIZE] = { 0 };
    uint8_t ret[BCM_RXBD_SIZE], zero[BCM_RXBD_SIZE] = { 0 };
    Bcm b;
    unsigned int i;
    uint32_t tag;
    int logfd;
    int sv[2];

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 5 + 3);
    }
    memset(frame, 0xff, 6);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    logfd = g_file_open_tmp("bcm570x-rx-payload-error-XXXXXX", &logfile,
                            NULL);
    g_assert_cmpint(logfd, >=, 0);
    close(logfd);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0"
                             " -d guest_errors -D %s",
                             sv[1], BCM_SLOT, logfile));

    bar_writel(&b, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4,
                   RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    qtest_memset(b.qts, RET_RING_PHYS, 0, sizeof(ret));
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    put32dma(bd, UNMAPPED_PHYS >> 32);
    put32dma(bd + 4, UNMAPPED_PHYS & 0xffffffff);
    qtest_memwrite(b.qts, STD_RING_PHYS, bd, sizeof(bd));
    /*
     * One completed descriptor per update [57XX-PG105-R, printed page 454].
     */
    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_log_marker(b.qts, logfile,
                        "bcm570x: receive payload write at 0x4000000 failed");
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmpmem(ret, sizeof(ret), zero, sizeof(zero));
    g_assert_cmphex(read_status_tag(&b), ==, tag);
    g_assert_false(intr_line_up(b.dev));

    /* Repair only the buffer address; the same ring entry remains posted. */
    put32dma(bd, RX_BUF_PHYS >> 32);
    put32dma(bd + 4, RX_BUF_PHYS & 0xffffffff);
    qtest_memwrite(b.qts, STD_RING_PHYS, bd, sizeof(bd));
    qtest_memset(b.qts, RX_BUF_PHYS, 0, sizeof(landed));
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);
    wait_for_rx_prod(&b, 1);
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_IDX), ==, 0);
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
                    BCM_RXBDFLAG_END);
    qtest_memread(b.qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
    close(sv[1]);
    unlink(logfile);
}

typedef enum RxDmaFault {
    RX_DMA_FAULT_DESCRIPTOR,
    RX_DMA_FAULT_PAYLOAD,
    RX_DMA_FAULT_COMPLETION,
} RxDmaFault;

/*
 * A failed receive DMA consumes nothing and leaves the frame at the peer. The
 * guest repairs the descriptor ring, buffer address, or return ring that
 * faulted. The following receive-ready write retries the frame. A
 * completion whose interrupt the driver has not acknowledged yet does not
 * hold it back: the receive path is driven by posted buffers, not by the
 * interrupt mailbox.
 */
static void test_receive_dma_error_after_progress(const void *data)
{
    RxDmaFault fault = GPOINTER_TO_INT(data);
    const char *fault_marker =
        fault == RX_DMA_FAULT_DESCRIPTOR ?
        "bcm570x: receive descriptor 1 read failed" :
        fault == RX_DMA_FAULT_PAYLOAD ?
        "bcm570x: receive payload write at 0x4000000 failed" :
        "bcm570x: receive completion 1 write failed";
    uint64_t std_ring = fault == RX_DMA_FAULT_DESCRIPTOR ?
                        DMA_END - BCM_RXBD_SIZE : STD_RING_PHYS;
    uint64_t ret_ring = fault == RX_DMA_FAULT_COMPLETION ?
                        DMA_END - BCM_RXBD_SIZE : RET_RING_PHYS;
    uint64_t first_ret = ret_ring;
    uint64_t second_ret;
    uint8_t first[64], second[64], got[64];
    uint8_t ret[BCM_RXBD_SIZE];
    uint32_t tag;
    unsigned int i;
    g_autofree char *logfile = NULL;
    Bcm b;
    int logfd;
    int sv[2];

    for (i = 0; i < sizeof(first); i++) {
        first[i] = (uint8_t)(i + 0x21);
        second[i] = (uint8_t)(i + 0x61);
    }
    memset(first, 0xff, 6);
    memset(second, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    logfd = g_file_open_tmp("bcm570x-rx-progress-error-XXXXXX", &logfile,
                            NULL);
    g_assert_cmpint(logfd, >=, 0);
    close(logfd);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0"
                             " -d guest_errors -D %s",
                             sv[1], BCM_SLOT, logfile));

    bar_writel(&b, BCM_RX_STD_RCB_HI, std_ring >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_LO, std_ring & 0xffffffff);
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB, ret_ring >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4,
                   ret_ring & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);

    write_rx_bd(&b, std_ring, 0, RX_BUF_PHYS);
    if (fault != RX_DMA_FAULT_DESCRIPTOR) {
        write_rx_bd(&b, std_ring, 1,
                    fault == RX_DMA_FAULT_PAYLOAD ?
                    UNMAPPED_PHYS : RX_BUF_PHYS + 0x100);
    }
    qtest_memset(b.qts, RX_BUF_PHYS, 0, 0x200);
    qtest_memset(b.qts, ret_ring, 0, BCM_RXBD_SIZE);
    if (fault != RX_DMA_FAULT_COMPLETION) {
        qtest_memset(b.qts, ret_ring + BCM_RXBD_SIZE, 0, BCM_RXBD_SIZE);
    }
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 2);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    /*
     * One completed descriptor per update [57XX-PG105-R, printed page 454].
     */
    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    net_socket_send_frame(sv[0], first, sizeof(first));
    wait_for_rx_prod(&b, 1);

    qtest_memread(b.qts, RX_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), first, sizeof(first));
    qtest_memread(b.qts, first_ret, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_IDX), ==, 0);
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
                    BCM_RXBDFLAG_END);
    tag = read_status_tag(&b);
    g_assert_true(intr_line_up(b.dev));

    net_socket_send_frame(sv[0], second, sizeof(second));
    wait_for_log_marker(b.qts, logfile, fault_marker);
    g_assert_cmpuint(read_rx_prod(&b), ==, 1);
    g_assert_cmphex(read_status_tag(&b), ==, tag);

    switch (fault) {
    case RX_DMA_FAULT_DESCRIPTOR:
        write_rx_bd(&b, STD_RING_PHYS, 0, RX_BUF_PHYS + 0x100);
        std_ring = STD_RING_PHYS - BCM_RXBD_SIZE;
        bar_writel(&b, BCM_RX_STD_RCB_HI, std_ring >> 32);
        bar_writel(&b, BCM_RX_STD_RCB_LO, std_ring & 0xffffffff);
        second_ret = RET_RING_PHYS + BCM_RXBD_SIZE;
        break;
    case RX_DMA_FAULT_PAYLOAD:
        write_rx_bd(&b, STD_RING_PHYS, 1, RX_BUF_PHYS + 0x100);
        second_ret = RET_RING_PHYS + BCM_RXBD_SIZE;
        break;
    case RX_DMA_FAULT_COMPLETION:
        ret_ring = RET_RING_PHYS - BCM_RXBD_SIZE;
        ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB, ret_ring >> 32);
        ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4,
                       ret_ring & 0xffffffff);
        qtest_memset(b.qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
        second_ret = RET_RING_PHYS;
        break;
    default:
        g_assert_not_reached();
    }

    /* The repair, then the receive-ready write that retries the frame. */
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 2);
    wait_for_rx_prod(&b, 2);
    qtest_memread(b.qts, RX_BUF_PHYS + 0x100, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), second, sizeof(second));
    qtest_memread(b.qts, second_ret, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_IDX), ==, 1);
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
                    BCM_RXBDFLAG_END);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);

    /* No queued duplicate survives the successful retry. */
    tag = read_status_tag(&b);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 2);
    g_usleep(100000);
    g_assert_cmpuint(read_rx_prod(&b), ==, 2);
    g_assert_cmphex(read_status_tag(&b), ==, tag);

    bcm_close(&b);
    close(sv[0]);
    close(sv[1]);
    unlink(logfile);
}

/*
 * Interrupt coalescing on receive. The host-coalescing block batches
 * completed receive descriptors into one status-block update and one
 * interrupt instead of one per frame: with RX_MAX_COAL_BDS set to K, K
 * received frames raise a single status-block DMA. The status tag, bumped
 * once per status DMA, therefore advances by N/K over N frames, not by N.
 * This is the receive path's per-packet interrupt cost; honouring the value
 * the driver programs is both the fidelity contract (the datasheet's
 * host-coalescing registers) and the performance win.
 *
 * The coalescing timer is parked far out so the descriptor count, not the
 * timer, closes each group. The qtest virtual clock does not advance on its
 * own, so the timer never fires here and the group boundary is deterministic.
 */
static void test_receive_interrupt_coalescing(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t bd[BCM_RXBD_SIZE];
    unsigned int i;
    const unsigned int coal = 4;     /* RX_MAX_COAL_BDS: frames/IRQ */
    const unsigned int frames = 16;  /* a whole number of coalescing groups */
    uint32_t tag_before, tag_after;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 7 + 1);
    }
    memset(frame, 0xff, 6);          /* broadcast; always accepted */

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    /*
     * Standard-ring RCB in CSRs, return-ring RCB in SRAM, a ring long enough
     * for the whole burst, and the status block's host address.
     */
    bar_writel(&b, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4, RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, COAL_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);

    /* Program receive coalescing before enabling the block: K descriptors per
     * update, timer parked far out so the count is the trigger. */
    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, coal);
    bar_writel(&b, BCM_HCC_RX_COAL_TICKS, 0x10000000);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    /*
     * Post COAL_RING_LEN worth of standard-ring buffers. One scratch buffer
     * is enough; only the interrupt count matters here.
     */
    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, RX_BUF_PHYS >> 32);
    put32dma(bd + 4, RX_BUF_PHYS & 0xffffffff);
    for (i = 0; i < frames; i++) {
        qtest_memwrite(b.qts, STD_RING_PHYS + (uint64_t)i * BCM_RXBD_SIZE,
                       bd, sizeof(bd));
    }
    qtest_memset(b.qts, RET_RING_PHYS, 0,
                 (uint64_t)COAL_RING_LEN * BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, frames);

    /*
 * sample the tag after it, so the delta below counts only the coalesced,
 */
    tag_before = read_status_tag(&b);

    /*
     * Send the whole burst, then wait for the receive producer to reach it.
     */
    for (i = 0; i < frames; i++) {
        net_socket_send_frame(sv[0], frame, sizeof(frame));
    }
    wait_for_rx_prod(&b, frames);

    tag_after = read_status_tag(&b);

    /*
     * N frames coalesced K-at-a-time is exactly N/K status-block updates --
     * one interrupt per group, not one per frame.
     */
    g_assert_cmpuint(tag_after - tag_before, ==, frames / coal);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * Coalescing's tick timer flushes a partial group. With RX_MAX_COAL_BDS above
 * the burst size the descriptor count never triggers, so a handful of frames
 * raise no interrupt until the RX_COAL_TICKS budget elapses, at which point
 * the timer flushes them in one update. This is the no-stall guarantee: the
 * tail of a burst is delivered on the tick, never left waiting for a frame
 * that never comes.
 */
static void test_receive_coalescing_timer(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t bd[BCM_RXBD_SIZE];
    unsigned int i;
    const unsigned int coal = 8;     /* exceeds burst; count never fires */
    /*
     * RX_COAL_TICKS in microseconds, and it has to outlast the arrival floor
     * for a burst to coalesce at all. A completion reaches the coalescing
     * block only at its arrival deadline, and the timer runs from the last
     * status-block update rather than from the frame waiting to be reported,
     * so a budget shorter than the floor is always expired by the time the
     * first completion lands: that frame is then reported on its own and the
     * rest wait out a reloaded budget, which is two updates and not the one
     * group flush this case is about. Also above the worst case of the
     * clock-stepping the wait helpers do while polling.
     */
    const unsigned int ticks = 20000;
    const unsigned int frames = 3;   /* only the timer can flush these */
    uint32_t tag_before;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 3 + 9);
    }
    memset(frame, 0xff, 6);          /* broadcast; always accepted */

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    bar_writel(&b, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4, RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, COAL_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, coal);
    bar_writel(&b, BCM_HCC_RX_COAL_TICKS, ticks);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, RX_BUF_PHYS >> 32);
    put32dma(bd + 4, RX_BUF_PHYS & 0xffffffff);
    for (i = 0; i < frames; i++) {
        qtest_memwrite(b.qts, STD_RING_PHYS + (uint64_t)i * BCM_RXBD_SIZE,
                       bd, sizeof(bd));
    }
    qtest_memset(b.qts, RET_RING_PHYS, 0,
                 (uint64_t)COAL_RING_LEN * BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, frames);

    tag_before = read_status_tag(&b);

    for (i = 0; i < frames; i++) {
        net_socket_send_frame(sv[0], frame, sizeof(frame));
    }
    /*
 * status update yet: the count is below the group size and no tick has
 */
    wait_for_ret_end(&b, frames - 1);
    g_assert_cmpuint(read_status_tag(&b) - tag_before, ==, 0);

    /* Advance past the tick budget; the timer flushes the held group in one
     * update. */
    /*
     * Past the arrival floor as well: the coalescing timer only starts
     * counting once the completion is reported to it.
     */
    qtest_clock_step(b.qts,
                     RX_ARRIVAL_LATENCY_NS + (int64_t)ticks * 1000 + 1000);
    wait_for_rx_prod(&b, frames);
    g_assert_cmpuint(read_status_tag(&b) - tag_before, ==, 1);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * Receive reads the same zero-tick rule off the same page: the count is then
 * the only thing that can report a frame. The first frame is completed into
 * the return ring and left unreported for as long as the test cares to wait.
 */
static void test_receive_coalescing_zero_ticks(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t status[BCM_STATUS_BLK_SIZE];

    memset(frame, 0x41, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_criteria_open(&b, 2, 2, 0);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_ret_end(&b, 0);
    qtest_clock_step(b.qts, 1000 * 1000 * 1000);
    g_assert_cmpuint(read_status_tag(&b), ==, 0);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_rx_prod(&b, 2);
    g_assert_cmpuint(read_status_tag(&b), ==, 1);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_RX_PROD_OFF), ==, 2);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * With neither receive criterion armed (no max-BD count or tick budget,
 * as after reset), the chip completes the frame into the
 * return ring and tells the host nothing. Nothing else reports it either: the
 * guide gives the status block four update criteria and no clock of its own
 * [57XX-PG105-R, printed page 63].
 */
static void test_receive_without_a_criterion_is_silent(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t status[BCM_STATUS_BLK_SIZE];

    memset(frame, 0x21, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_criteria_open(&b, 1, 0, 0);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_ret_end(&b, 0);

    qtest_clock_step(b.qts, 1000 * 1000 * 1000);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get32dma(status), ==, 0);
    g_assert_cmphex(get16dma(status, BCM_STATUS_RX_PROD_OFF), ==, 0);
    g_assert_cmpuint(read_status_tag(&b), ==, 0);
    g_assert_false(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
}

/* The transmit side of the same silence. */
static void test_transmit_without_a_criterion_is_silent(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t got[128];
    uint8_t status[BCM_STATUS_BLK_SIZE];

    memset(frame, 0x2f, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    txc_setup(&b, 0, 0);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);

    txc_post_one_at(&b, frame, sizeof(frame), 0);
    g_assert_cmpint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==,
                    sizeof(frame));

    qtest_clock_step(b.qts, 1000 * 1000 * 1000);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get32dma(status), ==, 0);
    g_assert_cmphex(get16dma(status, BCM_STATUS_TX_CONS_OFF), ==, 0);
    g_assert_cmpuint(read_status_tag(&b), ==, 0);
    g_assert_false(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
}

/*
 * A coalescing timer runs from the last status-block update, not from the
 * event that is waiting: it "will be reset to the value of this register and
 * will start counting down, after every status block update", and "once the
 * counter is in the expired state, a status block update will occur if a
 * packet had been received ... since the last status block update"
 * [57XX-PG105-R, printed page 453]. So on an idle ring the timer is already
 * expired when a frame arrives, and that frame is reported at once rather
 * than held for a fresh budget. This sets the latency of a lone
 * request-reply exchange.
 */
static void test_receive_expired_timer_reports_at_once(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    const unsigned int ticks = 50;

    memset(frame, 0x33, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    /* A count too large to fire, so only the timer can report anything. */
    rx_criteria_open(&b, 2, 8, ticks);

    /* The budget elapses with an empty ring: nothing to report, no update. */
    qtest_clock_step(b.qts, (int64_t)ticks * 1000 * 4);
    g_assert_cmpuint(read_status_tag(&b), ==, 0);

    /* The frame lands on an expired timer and is reported without waiting. */
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_rx_prod(&b, 1);
    g_assert_cmpuint(read_status_tag(&b), ==, 1);

    bcm_close(&b);
    close(sv[0]);
}

/* The transmit timer expires on the same terms. */
static void test_transmit_expired_timer_reports_at_once(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t got[128];
    const unsigned int ticks = 50;

    memset(frame, 0x44, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    txc_setup(&b, 8, ticks);

    qtest_clock_step(b.qts, (int64_t)ticks * 1000 * 4);
    g_assert_cmpuint(read_status_tag(&b), ==, 0);

    txc_post_one_at(&b, frame, sizeof(frame), 0);
    g_assert_cmpint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==,
                    sizeof(frame));
    g_assert_cmpuint(read_status_tag(&b), ==, 1);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * The receive producer the driver reads out of the status block always names
 * a completion the chip has already written: "When the controller receives a
 * packet and writes that packet data into host memory via DMA, it will
 * increment the Producer Index" [57XX-PG105-R, printed page 107]. A driver
 * that trusted a producer running ahead of the ring would read a descriptor
 * that is still zero.
 */
static void test_receive_producer_follows_its_completion(const void *data)
{
    const unsigned int frames = 3;
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t bd[BCM_RXBD_SIZE];
    unsigned int i;

    memset(frame, 0x55, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_criteria_open(&b, frames, 1, 0);

    for (i = 0; i < frames; i++) {
        net_socket_send_frame(sv[0], frame, sizeof(frame));
        wait_for_rx_prod(&b, i + 1);

        /* The descriptor the producer just passed carries the frame... */
        qtest_memread(b.qts, RET_RING_PHYS + (uint64_t)i * BCM_RXBD_SIZE,
                      bd, sizeof(bd));
        g_assert_cmphex(get16dma(bd, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
                        BCM_RXBDFLAG_END);
        g_assert_cmphex(get16dma(bd, BCM_RXBD_LEN), ==,
                        sizeof(frame) + BCM_ETH_FCS_LEN);

        /* ...and the one it points at is still untouched. */
        qtest_memread(b.qts,
                      RET_RING_PHYS + (uint64_t)(i + 1) * BCM_RXBD_SIZE,
                      bd, sizeof(bd));
        g_assert_cmphex(get16dma(bd, BCM_RXBD_LEN), ==, 0);
        g_assert_cmphex(get16dma(bd, BCM_RXBD_FLAGS), ==, 0);
    }

    bcm_close(&b);
    close(sv[0]);
}

/* A failed status DMA must leave a completed receive group retryable. */
static void test_receive_coalescing_status_dma_retry(const void *data)
{
    const unsigned int ticks = 50;
    uint8_t frame[64];
    uint8_t bd[BCM_RXBD_SIZE] = { 0 };
    uint8_t status[BCM_STATUS_BLK_SIZE];
    Bcm b;
    int sv[2];

    memset(frame, 0x6d, sizeof(frame));
    memset(frame, 0xff, 6);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    bar_writel(&b, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4,
                   RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);
    put32dma(bd, RX_BUF_PHYS >> 32);
    put32dma(bd + 4, RX_BUF_PHYS & 0xffffffff);
    qtest_memwrite(b.qts, STD_RING_PHYS, bd, sizeof(bd));
    qtest_memset(b.qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, UNMAPPED_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, UNMAPPED_PHYS & 0xffffffff);
    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(&b, BCM_HCC_RX_COAL_TICKS, ticks);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_ret_end(&b, 0);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_RX_PROD_OFF), ==, 0);
    g_assert_false(intr_line_up(b.dev));

    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    /*
     * Past the arrival floor as well: the coalescing timer only starts
     * counting once the completion is reported to it.
     */
    qtest_clock_step(b.qts,
                     RX_ARRIVAL_LATENCY_NS + (int64_t)ticks * 1000 + 1000);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16dma(status, BCM_STATUS_RX_PROD_OFF), ==, 1);
    g_assert_cmphex(read_status_tag(&b), ==, 1);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
}

/*
 * Rebuild the status-DMA plumbing after a reset and arm both coalescing
 * timers, with descriptor counts too high to fire. The reset left nothing for
 * either timer to report, so an update after the budget elapses is work the
 * reset failed to discard.
 */
static void bcm_rearm_status_after_reset(Bcm *b, unsigned int ticks)
{
    qpci_device_enable(b->dev);
    ind_mem_writel(b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    ind_reg_writel(b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    ind_reg_writel(b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    ind_reg_writel(b, BCM_HCC_RX_MAX_COAL_BDS, 8);
    ind_reg_writel(b, BCM_HCC_TX_MAX_COAL_BDS, 8);
    ind_reg_writel(b, BCM_HCC_RX_COAL_TICKS, ticks);
    ind_reg_writel(b, BCM_HCC_TX_COAL_TICKS, ticks);
    qtest_memset(b->qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    ind_reg_writel(b, BCM_HCC_MODE, BCM_HCC_ENABLE);
}

/*
 * The guest-driven core reset must discard a partial receive group and its
 * timer. Otherwise re-enabling HCC lets pre-reset work update the new status
 * block after the old deadline.
 */
static void test_global_reset_cancels_receive_coalescing(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    const unsigned int ticks = 50;

    memset(frame, 0x72, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_open_one(&b, sv[0]);
    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, 8);
    bar_writel(&b, BCM_HCC_RX_COAL_TICKS, ticks);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_ret_end(&b, 0);
    g_assert_cmpuint(read_status_tag(&b), ==, 0);

    bar_writel(&b, BCM_MISC_CFG, BCM_MISCCFG_RESET);
    bcm_rearm_status_after_reset(&b, ticks);
    /*
     * Past the arrival floor as well: the coalescing timer only starts
     * counting once the completion is reported to it.
     */
    qtest_clock_step(b.qts,
                     RX_ARRIVAL_LATENCY_NS + (int64_t)ticks * 1000 + 1000);
    g_assert_cmpuint(read_status_tag(&b), ==, 0);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * A machine reset has the same boundary for transmit work. A pending
 * completion from the old machine state must not be delivered after reset.
 */
static void test_system_reset_cancels_transmit_coalescing(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t got[128];
    const unsigned int ticks = 50;

    memset(frame, 0x93, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    txc_setup(&b, 8, ticks);
    txc_post(&b, frame, sizeof(frame), 1);
    g_assert_cmpint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==,
                    sizeof(frame));
    g_assert_cmpuint(read_status_tag(&b), ==, 0);

    qtest_qmp_assert_success(b.qts, "{ 'execute': 'system_reset' }");
    qtest_qmp_eventwait(b.qts, "RESET");
    bcm_rearm_status_after_reset(&b, ticks);
    qtest_clock_step(b.qts, (int64_t)ticks * 1000 + 1000);
    g_assert_cmpuint(read_status_tag(&b), ==, 0);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * The receive half of the same claim. ENDIAN_WORDSWAP composes the
 * register-programmed ring and status addresses, and the frame lands in the
 * posted buffer byte for byte as it arrived on the wire: on this route the
 * model does not transform payload, whatever the swap bits say. Off a Bridge
 * the same bits would leave a word-reversed image in the buffer instead.
 */
static void test_receive_frame_wordswap(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];       /* what arrives on the wire */
    uint8_t bd[BCM_RXBD_SIZE];
    uint8_t got[64];
    uint32_t misc;
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 5 + 3);
    }
    memset(frame, 0xff, 6);     /* broadcast destination */

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    /*
     * ENDIAN_WORDSWAP decides the control-block address composition, so the
     * register-programmed host addresses are written word-swapped: the low
     * word into the HI slot, the high word into the LO slot. The posted
     * buffer's own address rides in the descriptor and is read through the
     * DMA accessors, so it stays in order.
     */
    misc = qpci_config_readl(b.dev, BCM_PCI_MISC_CTL);
    qpci_config_writel(b.dev, BCM_PCI_MISC_CTL, misc | BCM_MISCCTL_WORDSWAP);

    bar_writel(&b, BCM_RX_STD_RCB_HI, (uint32_t)STD_RING_PHYS);
    bar_writel(&b, BCM_RX_STD_RCB_LO, STD_RING_PHYS >> 32);
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 0, (uint32_t)RET_RING_PHYS);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, (uint32_t)STATUS_PHYS);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS >> 32);
    /*
     * One completed descriptor per update [57XX-PG105-R, printed page 454].
     */
    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, RX_BUF_PHYS >> 32);
    put32dma(bd + 4, RX_BUF_PHYS & 0xffffffff);
    qtest_memwrite(b.qts, STD_RING_PHYS, bd, sizeof(bd));
    qtest_memset(b.qts, RX_BUF_PHYS, 0, sizeof(frame));
    qtest_memset(b.qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);

    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_rx_prod(&b, 1);

    qtest_memread(b.qts, RX_BUF_PHYS, got, sizeof(frame));
    g_assert_cmpmem(got, sizeof(frame), frame, sizeof(frame));

    bcm_close(&b);
    close(sv[0]);
}

/*
 * A receive producer or consumer index past its ring must not hang the
 * mailbox write. The rings hold RX_RING_LEN descriptors, so an index at or
 * beyond that is out of range; each write logs a guest error and returns,
 * proven by the MMIO access after it.
 */
static void test_rx_producer_out_of_range(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);

    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, RX_RING_LEN);
    g_assert_cmphex(mii_read(&b, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 0xffff);
    g_assert_cmphex(mii_read(&b, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);

    bar_writel(&b, BCM_MBX_RX_CONS0_LO, RX_RING_LEN);
    g_assert_cmphex(mii_read(&b, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);
    bar_writel(&b, BCM_MBX_RX_CONS0_LO, 0xffff);
    g_assert_cmphex(mii_read(&b, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);

    bcm_close(&b);
}

/*
 * The interrupt mailbox masks and re-enables the INTA line, and the global
 * MISC_CTL mask suppresses it. The chip drives the line as a level: it goes
 * up when a status-block update is posted with interrupts unmasked and down
 * when the driver acknowledges. The driver reads the line through
 * PCISTATE.INTR_NOT_ACTIVE, which the chip clears while the line is asserted.
 * Producer-0 doorbells stand in for real completions. Each drives a
 * status-block DMA with no ring walk, so the test needs no descriptors or
 * frame.
 */
static void test_interrupt_mailbox(const void *data)
{
    Bcm b;
    uint32_t misc;

    bcm_open(&b, bcm_init());
    bcm_arm_status(&b);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    /* Masked out of reset: an update posts, but the line stays down. */
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 0);
    g_assert_false(intr_line_up(b.dev));

    /* Enable interrupts (mailbox 0); the next update asserts the line. */
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 0);
    g_assert_true(intr_line_up(b.dev));

    /*
     * Write 1: masks and acknowledges. The line drops; INTR_NOT_ACTIVE reads.
     */
    bar_writel(&b, BCM_MBX_IRQ0_LO, 1);
    g_assert_false(intr_line_up(b.dev));

    /* A completion posted while masked remains pending across the unmask. */
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 0);
    g_assert_false(intr_line_up(b.dev));
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_true(intr_line_up(b.dev));

    /* The IRIX write-0-only acknowledgement clears that pending update. */
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_false(intr_line_up(b.dev));
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 0);
    g_assert_true(intr_line_up(b.dev));

    /*
     * The global mask in MISC_CTL suppresses the line while it is set, and
     * the line returns when it clears. The pending update was never taken.
     */
    misc = qpci_config_readl(b.dev, BCM_PCI_MISC_CTL);
    qpci_config_writel(b.dev, BCM_PCI_MISC_CTL, misc | BCM_MISCCTL_MASK_INTR);
    g_assert_false(intr_line_up(b.dev));
    qpci_config_writel(b.dev, BCM_PCI_MISC_CTL, misc);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
}

/*
 * The status-block DMA is gated by HCC_MODE.ENABLE. With the coalescing block
 * disabled a doorbell posts nothing to host memory and raises no interrupt;
 * enabling it lets the same doorbell DMA the status block out.
 */
static void test_status_block_gated_by_hcc(const void *data)
{
    Bcm b;
    uint8_t sb[BCM_STATUS_BLK_SIZE];

    bcm_open(&b, bcm_init());
    bcm_arm_status(&b);

    /* Disabled: the status region stays zero and the line stays down. */
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 0);
    qtest_memread(b.qts, STATUS_PHYS, sb, sizeof(sb));
    g_assert_cmphex(get32dma(sb), ==, 0);
    g_assert_false(intr_line_up(b.dev));

    /* Enabled: the same doorbell now DMAs the status block, UPDATED set. */
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 0);
    qtest_memread(b.qts, STATUS_PHYS, sb, sizeof(sb));
    g_assert_cmphex(get32dma(sb) & BCM_STATFLAG_UPDATED, ==,
                    BCM_STATFLAG_UPDATED);

    bcm_close(&b);
}

/* A failed status write advances neither its tag nor the interrupt state. */
static void test_status_dma_error(const void *data)
{
    Bcm b;

    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev user,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             BCM_SLOT));
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, UNMAPPED_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, UNMAPPED_PHYS & 0xffffffff);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 0);
    g_assert_false(intr_line_up(b.dev));

    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 0);
    g_assert_cmphex(read_status_tag(&b), ==, 1);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
}

/*
 * The source reports completion once it has sent the stream, which can be
 * before the destination has loaded it. A test that steps the destination's
 * clock before then steps it under a device whose timers have not arrived.
 */
static void bcm570x_migrate(QTestState *from, QTestState *to, const char *uri)
{
    qtest_qmp_assert_success(from,
        "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
}

static QDict *bcm570x_vmstate_field(QList *fields, const char *name)
{
    const QListEntry *entry;

    QLIST_FOREACH_ENTRY(fields, entry) {
        QDict *field = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(field, "field"), name)) {
            return field;
        }
    }
    return NULL;
}

/*
 * Version 6 predates moving the PHY into the MAC's migration section. The
 * nested state must therefore start at a new parent version: reusing version
 * 6 makes old and new binaries assign different layouts to the same stream.
 */
static void test_vmstate_versions_nested_phy(const void *data)
{
    g_autofree gchar *dump_path = NULL;
    g_autofree gchar *dump = NULL;
    g_autofree gchar *error_output = NULL;
    QObject *root_obj;
    QDict *root, *section, *description, *phy;
    QList *fields;
    GError *err = NULL;
    int fd, wait_status;

    fd = g_file_open_tmp("bcm570x-vmstate-XXXXXX", &dump_path, &err);
    g_assert_no_error(err);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    {
        g_autofree char *command = g_strdup_printf(
            "%s " BCM_MACHINE " -device bcm570x -dump-vmstate %s",
            qtest_qemu_binary(NULL), dump_path);
        g_auto(GStrv) args = NULL;

        g_assert_true(g_shell_parse_argv(command, NULL, &args, &err));
        g_assert_no_error(err);
        g_assert_true(g_spawn_sync(NULL, args, NULL, 0, NULL, NULL,
                                   NULL, &error_output, &wait_status, &err));
    }
    g_assert_no_error(err);
    g_assert_true(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_true(g_file_get_contents(dump_path, &dump, NULL, &err));
    g_assert_no_error(err);

    root_obj = qobject_from_json(dump, &error_abort);
    root = qobject_to(QDict, root_obj);
    g_assert_false(qdict_haskey(root, "bcm570x-phy"));
    section = qdict_get_qdict(root, "bcm570x");
    g_assert_cmpint(qdict_get_int(section, "version_id"), ==, 10);
    description = qdict_get_qdict(section, "Description");
    g_assert_cmpint(qdict_get_int(description, "version_id"), ==, 10);
    fields = qdict_get_qlist(description, "Fields");
    phy = bcm570x_vmstate_field(fields, "phy");
    g_assert_nonnull(phy);
    g_assert_cmpint(qdict_get_int(phy, "version_id"), ==, 7);

    qobject_unref(root_obj);
    unlink(dump_path);
}

/*
 * The PHY is a migrated child while the net frontend is host state. Both must
 * still describe the same cable on the destination, including after a fresh
 * set_link event changes that level.
 */
static void test_migrated_link_controls_egress(const void *data)
{
    bool migrate_up = GPOINTER_TO_INT(data);
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest = NULL;
    GError *err = NULL;
    QTestState *from_qts, *to_qts;
    Bcm from, to;
    int source_sv[2], dest_sv[2];
    uint8_t frame[60];
    uint8_t got[sizeof(frame)];

    memset(frame, 0x5a, sizeof(frame));
    memset(frame, 0xff, 6);

    tmpfs = g_dir_make_tmp("bcm570x-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, source_sv), ==, 0);
    from_qts = qtest_initf(BCM_MACHINE
                           " -netdev socket,fd=%d,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0,"
                           "mac=52:54:00:11:22:33",
                           source_sv[1], BCM_SLOT);
    bcm_open(&from, from_qts);
    txc_setup(&from, 1, 0);

    mii_write(&from, BCM_PHY_ADDR, MII_BMCR,
              MII_BMCR_AUTOEN | MII_BMCR_STARTNEG);
    qtest_clock_step(from.qts, BCM_PHY_SETTLE_NS + 1000);
    g_assert_cmphex(mii_read(&from, BCM_PHY_ADDR, MII_BMSR) & MII_BMSR_LINK,
                    ==, MII_BMSR_LINK);
    if (!migrate_up) {
        qtest_qmp_assert_success(from.qts,
            "{'execute': 'set_link', 'arguments': "
            "{'name': 'n0', 'up': false}}");
        g_assert_cmphex(mii_read(&from, BCM_PHY_ADDR, MII_BMSR) &
                        MII_BMSR_LINK, ==, 0);
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, dest_sv), ==, 0);
    dest = g_strdup_printf(BCM_MACHINE
                           " -netdev socket,fd=%d,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0,"
                           "mac=52:54:00:11:22:33 -incoming %s",
                           dest_sv[1], BCM_SLOT, uri);
    to_qts = qtest_init(dest);
    bcm_open(&to, to_qts);
    bcm570x_migrate(from.qts, to_qts, uri);

    g_assert_cmphex(mii_read(&to, BCM_PHY_ADDR, MII_BMSR) & MII_BMSR_LINK,
                    ==, migrate_up ? MII_BMSR_LINK : 0);
    txc_post_one_at(&to, frame, sizeof(frame), 0);
    if (migrate_up) {
        net_socket_set_recv_timeout(dest_sv[0], 10);
        g_assert_cmpint(net_socket_recv_frame(dest_sv[0], got, sizeof(got)),
                        ==, sizeof(frame));
    } else {
        net_socket_expect_no_frame(dest_sv[0]);

        qtest_qmp_assert_success(to.qts,
            "{'execute': 'set_link', 'arguments': "
            "{'name': 'n0', 'up': true}}");
        g_assert_cmphex(mii_read(&to, BCM_PHY_ADDR, MII_BMSR) & MII_BMSR_LINK,
                        ==, MII_BMSR_LINK);

        net_socket_set_recv_timeout(dest_sv[0], 10);
        txc_post_one_at(&to, frame, sizeof(frame), 1);
        g_assert_cmpint(net_socket_recv_frame(dest_sv[0], got, sizeof(got)),
                        ==, sizeof(frame));
    }

    bcm_close(&to);
    bcm_close(&from);
    close(source_sv[0]);
    close(dest_sv[0]);
    g_rmdir(tmpfs);
}

/*
 * F12: the arrival line's vmstate migrates only what it holds, not a fixed
 * 256 slots. Drain one completion clear of the ring's start so the line's
 * head is not still sitting at zero, hold two more behind a floor small
 * polling drift cannot cross by accident, then migrate and confirm the
 * destination still reports all three in order: the one already-published
 * completion untouched, the two held ones on their own deadline. Kept under
 * the return ring's own length (RX_RING_LEN) throughout, so the status
 * block's producer index (a value mod that length) never wraps and
 * can be read as a plain count.
 */
static void test_arrival_line_migrates_only_what_it_holds(const void *data)
{
    /*
     * Ten times the default floor: comfortably past the small drift the held
     * frames' own polling below can add, and still well inside what
     * wait_for_rx_prod's clock-stepping retries can cross when draining.
     */
    enum { FLOOR_US = 2000 };
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest = NULL;
    GError *err = NULL;
    QTestState *from_qts, *to_qts;
    Bcm from, to;
    uint8_t bd[BCM_RXBD_SIZE];
    uint8_t frame[64];
    int sv[2];
    unsigned int i;
    const unsigned int drained = 1, held = 2, total = drained + held;

    memset(frame, 0x71, sizeof(frame));
    memset(frame, 0xff, 6);

    tmpfs = g_dir_make_tmp("bcm570x-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    from_qts = qtest_initf(BCM_MACHINE
                           " -netdev socket,fd=%d,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0,"
                           "rx-arrival-latency-us=%u",
                           sv[1], BCM_SLOT, FLOOR_US);
    bcm_open(&from, from_qts);

    bar_writel(&from, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(&from, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(&from, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(&from, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_PHYS >> 32);
    ind_mem_writel(&from, BCM_SRAM_RX_RETURN_RCB + 4,
                   RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&from, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);
    bar_writel(&from, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&from, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    /* One completion, one update: this test is not about coalescing. */
    bar_writel(&from, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(&from, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&from, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    memset(bd, 0, sizeof(bd));
    put32dma(bd + 0, RX_BUF_PHYS >> 32);
    put32dma(bd + 4, RX_BUF_PHYS & 0xffffffff);
    for (i = 0; i < total; i++) {
        qtest_memwrite(from.qts, STD_RING_PHYS + (uint64_t)i * BCM_RXBD_SIZE,
                       bd, sizeof(bd));
    }
    qtest_memset(from.qts, RET_RING_PHYS, 0,
                (uint64_t)RX_RING_LEN * BCM_RXBD_SIZE);
    qtest_memset(from.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&from, BCM_MBX_RX_STD_PROD_LO, total);

    /* One lands and publishes, moving the ring's head off its start index. */
    for (i = 0; i < drained; i++) {
        net_socket_send_frame(sv[0], frame, sizeof(frame));
    }
    wait_for_rx_prod(&from, drained);
    bar_writel(&from, BCM_MBX_RX_CONS0_LO, drained % RX_RING_LEN);

    /* Two more, held behind a floor this test's own polling never reaches. */
    for (i = 0; i < held; i++) {
        net_socket_send_frame(sv[0], frame, sizeof(frame));
    }
    for (i = 0; i < held; i++) {
        wait_for_ret_end(&from, (drained + i) % RX_RING_LEN);
    }
    g_assert_cmphex(read_rx_prod(&from), ==, drained);

    dest = g_strdup_printf(BCM_MACHINE
                           " -netdev user,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0,"
                           "rx-arrival-latency-us=%u"
                           " -incoming %s", BCM_SLOT, FLOOR_US, uri);
    to_qts = qtest_init(dest);
    bcm_open(&to, to_qts);
    bcm570x_migrate(from.qts, to_qts, uri);

    /* The two already published travelled as ordinary device state. */
    g_assert_cmphex(read_rx_prod(&to), ==, drained);

    /*
     * The three held ones publish together, on their own deadline, at an
     * absolute QEMU_CLOCK_VIRTUAL value carried over from a source that was
     * not at time zero, so the destination (which starts fresh at zero) is
     * polled up to it rather than stepped by a single guessed amount.
     */
    wait_for_rx_prod(&to, total);

    bcm_close(&to);
    bcm_close(&from);
    close(sv[0]);
    g_rmdir(tmpfs);
}

/*
 * The device's state round-trips across a migration. On the source, a
 * register-file word (the status-block host address), an SRAM word, and an
 * asserted interrupt are established, then the machine is migrated stopped.
 * On the destination the register and SRAM words read back, and the interrupt
 * is still live: toggling the global mask drops and re-raises the line with
 * no new update, which holds only if the pending latch and the mailbox mask
 * both crossed the migration.
 */
static void test_state_survives_migration(const void *data)
{
    static const uint8_t migrated_station[] = {
        0xa2, 0xa1, 0x00, 0x00, 0xa6, 0xa5, 0xa4, 0xa3,
    };
    static const uint8_t reset_station[] = {
        0x54, 0x52, 0x00, 0x00, 0x33, 0x22, 0x11, 0x00,
    };
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest = NULL;
    GError *err = NULL;
    Bcm from, to;
    QTestState *from_qts, *to_qts;
    const uint32_t sram_sentinel = 0x1a2b3c4d;
    uint32_t misc;

    tmpfs = g_dir_make_tmp("bcm570x-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from_qts = qtest_initf(BCM_MACHINE
                           " -netdev user,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0,"
                           "mac=52:54:00:11:22:33", BCM_SLOT);
    bcm_open(&from, from_qts);
    bcm_arm_status(&from);
    bar_writel(&from, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&from, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    /* An SRAM word reset does not touch, so its survival is unambiguous. */
    ind_mem_writel(&from, 0x0c1c, sram_sentinel);
    bar_writel(&from, BCM_MAC_ADDR_0_HIGH, 0x0000a1a2);
    bar_writel(&from, BCM_MAC_ADDR_0_LOW, 0xa3a4a5a6);

    /*
     * Enable interrupts and leave the line asserted at the migration point.
     */
    bar_writel(&from, BCM_MBX_IRQ0_LO, 0);
    bar_writel(&from, BCM_MBX_TX_PROD_LO, 0);
    g_assert_true(intr_line_up(from.dev));

    dest = g_strdup_printf(BCM_MACHINE
                           " -netdev user,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0,"
                           "mac=52:54:00:11:22:33 -incoming %s", BCM_SLOT, uri);
    to_qts = qtest_init(dest);
    bcm_open(&to, to_qts);
    bcm570x_migrate(from_qts, to_qts, uri);

    /*
     * The register file (status-block host address) and SRAM word came
     * across.
     */
    g_assert_cmphex(ind_reg_readl(&to, BCM_HCC_STATUSBLK_LO), ==,
                    STATUS_PHYS & 0xffffffff);
    g_assert_cmphex(ind_mem_readl(&to, 0x0c1c), ==, sram_sentinel);
    g_assert_cmphex(bar_readl(&to, BCM_MAC_ADDR_0_HIGH), ==, 0x0000a1a2);
    g_assert_cmphex(bar_readl(&to, BCM_MAC_ADDR_0_LOW), ==, 0xa3a4a5a6);
    assert_station_bytes(&to, migrated_station);

    /* The interrupt config came across, line still asserted. */
    g_assert_true(intr_line_up(to.dev));

    /* The migrated latch and mask are live: mask then unmask the global mask
     * and the line follows, without any fresh update to re-establish it. */
    misc = qpci_config_readl(to.dev, BCM_PCI_MISC_CTL);
    qpci_config_writel(to.dev, BCM_PCI_MISC_CTL, misc | BCM_MISCCTL_MASK_INTR);
    g_assert_false(intr_line_up(to.dev));
    qpci_config_writel(to.dev, BCM_PCI_MISC_CTL, misc);
    g_assert_true(intr_line_up(to.dev));

    bar_writel(&to, BCM_MISC_CFG, BCM_MISCCFG_RESET);
    g_assert_cmphex(bar_readl(&to, BCM_MAC_ADDR_0_HIGH), ==, 0x00005254u);
    g_assert_cmphex(bar_readl(&to, BCM_MAC_ADDR_0_LOW), ==, 0x00112233u);
    assert_station_bytes(&to, reset_station);

    bcm_close(&to);
    bcm_close(&from);
}

/*
 * A write-one mailbox acknowledgement can precede the driver's matching
 * unmask. The mask itself has to survive migration: a frame the destination
 * places goes on raising no line until the driver writes zero. Receiving is
 * not part of that handshake. The posted buffer releases the frame either
 * way.
 */
static void test_masked_interrupt_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest = NULL;
    GError *err = NULL;
    QTestState *from_qts, *to_qts;
    Bcm from, to;
    int source_sv[2], dest_sv[2];
    uint8_t frame[64];
    uint8_t bd[BCM_RXBD_SIZE] = { 0 };
    uint8_t ret[BCM_RXBD_SIZE];

    tmpfs = g_dir_make_tmp("bcm570x-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    memset(frame, 0xa6, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, source_sv), ==, 0);
    from_qts = qtest_initf(BCM_MACHINE
                           " -netdev socket,fd=%d,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0",
                           source_sv[1], BCM_SLOT);
    bcm_open(&from, from_qts);
    bcm_arm_status(&from);
    bar_writel(&from, BCM_RX_STD_RCB_HI, STD_RING_PHYS >> 32);
    bar_writel(&from, BCM_RX_STD_RCB_LO, STD_RING_PHYS & 0xffffffff);
    bar_writel(&from, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    ind_mem_writel(&from, BCM_SRAM_RX_RETURN_RCB, RET_RING_PHYS >> 32);
    ind_mem_writel(&from, BCM_SRAM_RX_RETURN_RCB + 4,
                   RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&from, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);
    put32dma(bd, RX_BUF_PHYS >> 32);
    put32dma(bd + 4, RX_BUF_PHYS & 0xffffffff);
    qtest_memwrite(from.qts, STD_RING_PHYS, bd, sizeof(bd));
    qtest_memset(from.qts, RET_RING_PHYS, 0, sizeof(ret));
    qtest_memset(from.qts, RX_BUF_PHYS, 0, sizeof(frame));
    /*
     * One completed descriptor per update [57XX-PG105-R, printed page 454].
     */
    bar_writel(&from, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(&from, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&from, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    bar_writel(&from, BCM_MBX_IRQ0_LO, 0);
    bar_writel(&from, BCM_MBX_TX_PROD_LO, 0);
    g_assert_true(intr_line_up(from.dev));
    bar_writel(&from, BCM_MBX_IRQ0_LO, 1);
    g_assert_false(intr_line_up(from.dev));

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, dest_sv), ==, 0);
    dest = g_strdup_printf(BCM_MACHINE
                           " -netdev socket,fd=%d,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0"
                           " -incoming %s", dest_sv[1], BCM_SLOT, uri);
    to_qts = qtest_init(dest);
    bcm_open(&to, to_qts);
    bcm570x_migrate(from.qts, to_qts, uri);

    net_socket_send_frame(dest_sv[0], frame, sizeof(frame));
    g_usleep(100000);
    qtest_memread(to.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16dma(ret, BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==, 0);

    bar_writel(&to, BCM_MBX_RX_STD_PROD_LO, 1);
    wait_for_rx_prod(&to, 1);
    g_assert_false(intr_line_up(to.dev));
    bar_writel(&to, BCM_MBX_IRQ0_LO, 0);
    g_assert_true(intr_line_up(to.dev));

    bcm_close(&to);
    bcm_close(&from);
    close(source_sv[0]);
    close(dest_sv[0]);
    g_rmdir(tmpfs);
}

/*
 * Arm a BCM5704 function's status-block DMA entirely through its own config
 * space: enable bus mastering, give its transmit ring a length so a doorbell
 * passes the range guard, program the status block's host page, and enable
 * the host-coalescing block. No BAR mapping is needed. The indirect windows
 * reach each function's register file and SRAM, so the two functions are
 * configured independently at one slot.
 */
static void dual5704_arm(QPCIDevice *dev, uint64_t status_phys)
{
    qpci_config_writew(dev, PCI_COMMAND,
                       PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    ind_mem_writel_dev(dev, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    ind_reg_writel_dev(dev, BCM_HCC_STATUSBLK_HI, status_phys >> 32);
    ind_reg_writel_dev(dev, BCM_HCC_STATUSBLK_LO, status_phys & 0xffffffff);
    ind_reg_writel_dev(dev, BCM_HCC_MODE, BCM_HCC_ENABLE);
}

/*
 * The dual-port BCM5704 presents as one multifunction PCI device: function 0
 * and function 1 at the same slot, each carrying the copper four-tuple, each
 * with its own MAC identity, and each with an independent ring engine and
 * status block. Driving one function's transmit doorbell moves that
 * function's status block and leaves the other's untouched.
 */
static void test_dual_port_present_and_independent(const void *data)
{
    QTestState *qts = qtest_initf(BCM_MACHINE
                                  " -netdev user,id=n0"
                                  " -device bcm570x,model=bcm5704,addr=%02x.0,"
                                  "netdev=n0", BCM_SLOT);
    QPCIBus *bus = qpci_init_sgi_bridge(qts, NULL);
    QPCIDevice *fn0 = qpci_device_find(bus, QPCI_DEVFN(BCM_SLOT, 0));
    QPCIDevice *fn1 = qpci_device_find(bus, QPCI_DEVFN(BCM_SLOT, 1));
    uint8_t sb0[BCM_STATUS_BLK_SIZE], sb1[BCM_STATUS_BLK_SIZE];
    QPCIDevice *fns[2];
    QPCIBar bars[2];
    uint8_t station[2][8];
    unsigned i;

    g_assert_nonnull(fn0);
    g_assert_nonnull(fn1);
    fns[0] = fn0;
    fns[1] = fn1;

    /* Both functions present, each carrying the copper BCM5704 four-tuple. */
    for (i = 0; i < 2; i++) {
        g_assert_cmphex(qpci_config_readl(fns[i], PCI_VENDOR_ID), ==,
                        BCM_VENDOR_ID |
                        ((uint32_t)BCM_DEVICE_ID_BCM5704 << 16));
        g_assert_cmphex(qpci_config_readl(fns[i], PCI_SUBSYSTEM_VENDOR_ID), ==,
                        BCM_SUBSYS_VENDOR_ID |
                        ((uint32_t)BCM_SUBSYS_ID_BCM5704 << 16));
        qpci_device_enable(fns[i]);
        bars[i] = qpci_iomap(fns[i], 0, NULL);
    }

    /* Function 0 sets the PCI_HEADER_TYPE multifunction bit, so a guest
     * enumerates function 1 at all. */
    g_assert_cmphex(qpci_config_readb(fn0, PCI_HEADER_TYPE) &
                    BCM_HEADER_MULTIFUNC, ==, BCM_HEADER_MULTIFUNC);

    /*
     * Distinct MAC identity per function: both mailboxes carry the 0x484B
     * tag, and the address bytes differ (each function took its own default
     * MAC).
     */
    g_assert_cmphex(ind_mem_readl_dev(fn0, 0x0c14) >> 16, ==, 0x484b);
    g_assert_cmphex(ind_mem_readl_dev(fn1, 0x0c14) >> 16, ==, 0x484b);
    g_assert_cmphex(ind_mem_readl_dev(fn0, 0x0c18), !=,
                    ind_mem_readl_dev(fn1, 0x0c18));
    for (i = 0; i < 8; i++) {
        station[0][i] = qpci_io_readb(fn0, bars[0],
                                     BCM_MAC_ADDR_0_HIGH + i);
        station[1][i] = qpci_io_readb(fn1, bars[1],
                                     BCM_MAC_ADDR_0_HIGH + i);
    }
    g_assert_cmpint(memcmp(station[0], station[1], sizeof(station[0])), !=, 0);

    /* Arm each function's engine through its own config space. */
    dual5704_arm(fn0, STATUS_PHYS);
    dual5704_arm(fn1, STATUS2_PHYS);
    qtest_memset(qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    qtest_memset(qts, STATUS2_PHYS, 0, BCM_STATUS_BLK_SIZE);

    /*
     * Drive function 0's doorbell only: its status block updates, function
     * 1's stays zero. The engines are independent.
     */
    ind_reg_writel_dev(fn0, BCM_MBX_TX_PROD_LO, 0);
    qtest_memread(qts, STATUS_PHYS, sb0, sizeof(sb0));
    qtest_memread(qts, STATUS2_PHYS, sb1, sizeof(sb1));
    g_assert_cmphex(get32dma(sb0) & BCM_STATFLAG_UPDATED, ==,
                    BCM_STATFLAG_UPDATED);
    g_assert_cmphex(get32dma(sb1), ==, 0);

    /*
     * Driving function 1 moves its own status block. The engines work
     * independently.
     */
    ind_reg_writel_dev(fn1, BCM_MBX_TX_PROD_LO, 0);
    qtest_memread(qts, STATUS2_PHYS, sb1, sizeof(sb1));
    g_assert_cmphex(get32dma(sb1) & BCM_STATFLAG_UPDATED, ==,
                    BCM_STATFLAG_UPDATED);

    qpci_iounmap(fn0, bars[0]);
    qpci_iounmap(fn1, bars[1]);
    g_free(fn0);
    g_free(fn1);
    qpci_free_sgi_bridge(bus);
    qtest_quit(qts);
}

/*
 * Both functions' per-function state migrates independently. Each function is
 * a full PCI device with its own VMStateDescription at its own devfn, so no
 * VMState change was needed for the dual-port device; this proves it by
 * writing a distinct SRAM word to each function, migrating stopped, and
 * reading both back on the destination.
 */
static void test_dual_port_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    QPCIBus *from_bus, *to_bus;
    QPCIDevice *from0, *from1, *to0, *to1;
    QPCIBar to_bar0, to_bar1;
    uint32_t configured_high[2], configured_low[2];

    tmpfs = g_dir_make_tmp("bcm570x-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_initf(BCM_MACHINE
                       " -netdev user,id=n0"
                       " -device bcm570x,model=bcm5704,addr=%02x.0,netdev=n0",
                       BCM_SLOT);
    from_bus = qpci_init_sgi_bridge(from, NULL);
    from0 = qpci_device_find(from_bus, QPCI_DEVFN(BCM_SLOT, 0));
    from1 = qpci_device_find(from_bus, QPCI_DEVFN(BCM_SLOT, 1));
    g_assert_nonnull(from0);
    g_assert_nonnull(from1);
    configured_high[0] = ind_reg_readl_dev(from0, BCM_MAC_ADDR_0_HIGH);
    configured_high[1] = ind_reg_readl_dev(from1, BCM_MAC_ADDR_0_HIGH);
    configured_low[0] = ind_reg_readl_dev(from0, BCM_MAC_ADDR_0_LOW);
    configured_low[1] = ind_reg_readl_dev(from1, BCM_MAC_ADDR_0_LOW);
    ind_mem_writel_dev(from0, 0x0c1c, 0xaaaa0000);
    ind_mem_writel_dev(from1, 0x0c1c, 0xbbbb1111);
    ind_reg_writel_dev(from0, BCM_MAC_ADDR_0_HIGH, 0x0000a1a2);
    ind_reg_writel_dev(from0, BCM_MAC_ADDR_0_LOW, 0xa3a4a5a6);
    ind_reg_writel_dev(from1, BCM_MAC_ADDR_0_HIGH, 0x0000b1b2);
    ind_reg_writel_dev(from1, BCM_MAC_ADDR_0_LOW, 0xb3b4b5b6);

    dest = g_strdup_printf(BCM_MACHINE
                           " -netdev user,id=n0"
                           " -device bcm570x,model=bcm5704,addr=%02x.0,netdev=n0"
                           " -incoming %s", BCM_SLOT, uri);
    to = qtest_init(dest);
    to_bus = qpci_init_sgi_bridge(to, NULL);
    to0 = qpci_device_find(to_bus, QPCI_DEVFN(BCM_SLOT, 0));
    to1 = qpci_device_find(to_bus, QPCI_DEVFN(BCM_SLOT, 1));
    g_assert_nonnull(to0);
    g_assert_nonnull(to1);
    bcm570x_migrate(from, to, uri);

    qpci_device_enable(to0);
    qpci_device_enable(to1);
    to_bar0 = qpci_iomap(to0, 0, NULL);
    to_bar1 = qpci_iomap(to1, 0, NULL);

    g_assert_cmphex(ind_mem_readl_dev(to0, 0x0c1c), ==, 0xaaaa0000);
    g_assert_cmphex(ind_mem_readl_dev(to1, 0x0c1c), ==, 0xbbbb1111);
    assert_station_bytes_dev(to0, to_bar0, 0x0000a1a2, 0xa3a4a5a6);
    assert_station_bytes_dev(to1, to_bar1, 0x0000b1b2, 0xb3b4b5b6);

    ind_reg_writel_dev(to0, BCM_MISC_CFG, BCM_MISCCFG_RESET);
    g_assert_cmphex(ind_reg_readl_dev(to0, BCM_MAC_ADDR_0_HIGH), ==,
                    configured_high[0]);
    g_assert_cmphex(ind_reg_readl_dev(to0, BCM_MAC_ADDR_0_LOW), ==,
                    configured_low[0]);
    g_assert_cmphex(ind_reg_readl_dev(to1, BCM_MAC_ADDR_0_HIGH), ==,
                    0x0000b1b2);
    g_assert_cmphex(ind_reg_readl_dev(to1, BCM_MAC_ADDR_0_LOW), ==,
                    0xb3b4b5b6);

    ind_reg_writel_dev(to1, BCM_MISC_CFG, BCM_MISCCFG_RESET);
    g_assert_cmphex(ind_reg_readl_dev(to1, BCM_MAC_ADDR_0_HIGH), ==,
                    configured_high[1]);
    g_assert_cmphex(ind_reg_readl_dev(to1, BCM_MAC_ADDR_0_LOW), ==,
                    configured_low[1]);
    assert_station_bytes_dev(to0, to_bar0,
                             configured_high[0], configured_low[0]);
    assert_station_bytes_dev(to1, to_bar1,
                             configured_high[1], configured_low[1]);
    g_assert_cmphex(configured_low[0], !=, configured_low[1]);

    qpci_iounmap(to0, to_bar0);
    qpci_iounmap(to1, to_bar1);
    g_free(from0);
    g_free(from1);
    g_free(to0);
    g_free(to1);
    qpci_free_sgi_bridge(from_bus);
    qpci_free_sgi_bridge(to_bus);
    qtest_quit(to);
    qtest_quit(from);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_device("bcm570x")) {
        return 0;
    }

    qtest_add_data_func("/bcm570x/pci-four-tuple", NULL, test_pci_four_tuple);
    qtest_add_data_func("/bcm570x/indirect-register-window", NULL,
                        test_indirect_register_window);
    qtest_add_data_func("/bcm570x/indirect-memory-window", NULL,
                        test_indirect_memory_window);
    qtest_add_data_func("/bcm570x/reset-handshake-via-bar", NULL,
                        test_reset_handshake_via_bar);
    qtest_add_data_func("/bcm570x/reset-handshake-magic-after-reset", NULL,
                        test_reset_handshake_magic_after_reset);
    qtest_add_data_func("/bcm570x/firmware-progress-register", NULL,
                        test_firmware_progress_register);
    qtest_add_data_func("/bcm570x/reset-handshake-via-indirect", NULL,
                        test_reset_handshake_via_indirect);
    qtest_add_data_func("/bcm570x/self-clearing-reset-bits", NULL,
                        test_self_clearing_reset_bits);
    qtest_add_data_func("/bcm570x/hcc-mode-idle-poll", NULL,
                        test_hcc_mode_idle_poll);
    qtest_add_data_func("/bcm570x/bufmgr-enable-self-clears", NULL,
                        test_bufmgr_enable_self_clears);
    qtest_add_data_func("/bcm570x/hcc-mode-clears-to-zero", NULL,
                        test_hcc_mode_clears_to_zero);
    qtest_add_data_func("/bcm570x/mi-busy-clears", NULL,
                        test_mi_busy_clears);
    qtest_add_data_func("/bcm570x/misc-ctl-asic-revision", NULL,
                        test_misc_ctl_asic_revision);
    qtest_add_data_func("/bcm570x/mac-address-mailbox", NULL,
                        test_mac_address_mailbox);
    qtest_add_data_func("/bcm570x/mac-address-registers", NULL,
                        test_mac_address_registers);
    qtest_add_data_func("/bcm570x/phy-readback", NULL, test_phy_readback);
    qtest_add_data_func("/bcm570x/mac-status-write-one-clears", NULL,
                        test_mac_status_write_one_clears);
    qtest_add_data_func("/bcm570x/phy-aux-status-link-up", NULL,
                        test_phy_aux_status_link_up);
    qtest_add_data_func("/bcm570x/status-block-byte-order", NULL,
                        test_status_block_byte_order);
    qtest_add_data_func("/bcm570x/status-block-hostaddr-wordswap", NULL,
                        test_status_block_hostaddr_wordswap);
    qtest_add_data_func("/bcm570x/transmit-round-trip", NULL,
                        test_transmit_round_trip);
    qtest_add_data_func("/bcm570x/user-backend-reply-waits-for-transmit", NULL,
                        test_user_backend_reply_waits_for_transmit);
    qtest_add_data_func("/bcm570x/transmit-source-address", NULL,
                        test_transmit_source_address);
    qtest_add_data_func("/bcm570x/transmit-descriptor-dma-error", NULL,
                        test_transmit_descriptor_dma_error);
    qtest_add_data_func("/bcm570x/transmit-payload-dma-error", NULL,
                        test_transmit_payload_dma_error);
    qtest_add_data_func("/bcm570x/transmit-complete-then-fragment-"
                        "dma-error-retry", NULL,
                        test_transmit_complete_then_fragment_dma_error_retry);
    qtest_add_data_func("/bcm570x/transmit-empty-complete-then-fragment-"
                        "dma-error-retry", GUINT_TO_POINTER(1),
                        test_transmit_complete_then_fragment_dma_error_retry);
    qtest_add_data_func("/bcm570x/transmit-complete-then-descriptor-"
                        "dma-error-retry", NULL,
                        test_transmit_complete_then_descriptor_dma_error_retry);
    qtest_add_data_func("/bcm570x/transmit-partial-error-descriptor-"
                        "coalescing", NULL,
                        test_transmit_partial_error_descriptor_coalescing);
    qtest_add_data_func("/bcm570x/transmit-coalescing", NULL,
                        test_transmit_coalescing);
    qtest_add_data_func("/bcm570x/transmit-coalescing-empty-and-dropped", NULL,
                        test_transmit_coalescing_empty_and_dropped);
    qtest_add_data_func("/bcm570x/transmit-coalescing-multidescriptor", NULL,
                        test_transmit_coalescing_multidescriptor);
    qtest_add_data_func("/bcm570x/transmit-split-doorbell", NULL,
                        test_transmit_split_doorbell);
    qtest_add_data_func("/bcm570x/transmit-zero-length-split-doorbell",
                        GUINT_TO_POINTER(1), test_transmit_split_doorbell);
    qtest_add_data_func("/bcm570x/transmit-complete-then-split-doorbell", NULL,
                        test_transmit_complete_then_split_doorbell);
    qtest_add_data_func("/bcm570x/transmit-coalescing-timer", NULL,
                        test_transmit_coalescing_timer);
    qtest_add_data_func("/bcm570x/transmit-coalescing-zero-ticks", NULL,
                        test_transmit_coalescing_zero_ticks);
    qtest_add_data_func("/bcm570x/transmit-coalescing-status-dma-retry", NULL,
                        test_transmit_coalescing_status_dma_retry);
    qtest_add_data_func("/bcm570x/transmit-checksum-offload", NULL,
                        test_transmit_checksum_offload);
    qtest_add_data_func("/bcm570x/transmit-segmentation", NULL,
                        test_transmit_segmentation);
#ifdef CONFIG_LINUX
    qtest_add_data_func("/bcm570x/transmit-vnet-plain", NULL,
                        test_transmit_vnet_plain);
    qtest_add_data_func("/bcm570x/transmit-segmentation-vnet", NULL,
                        test_transmit_segmentation_vnet);
    qtest_add_data_func("/bcm570x/transmit-segmentation-vnet-fallback/vlan",
                        GINT_TO_POINTER(VNET_LSO_FALLBACK_VLAN),
                        test_transmit_segmentation_vnet_fallback);
    qtest_add_data_func("/bcm570x/transmit-segmentation-vnet-fallback/mf",
                        GINT_TO_POINTER(VNET_LSO_FALLBACK_MF),
                        test_transmit_segmentation_vnet_fallback);
    qtest_add_data_func("/bcm570x/transmit-segmentation-vnet-fallback/offset",
                        GINT_TO_POINTER(VNET_LSO_FALLBACK_OFFSET),
                        test_transmit_segmentation_vnet_fallback);
    qtest_add_data_func("/bcm570x/transmit-segmentation-vnet-fallback/length",
                        GINT_TO_POINTER(VNET_LSO_FALLBACK_LENGTH),
                        test_transmit_segmentation_vnet_fallback);
    qtest_add_data_func("/bcm570x/transmit-segmentation-vnet-fallback/udp",
                        GINT_TO_POINTER(VNET_LSO_FALLBACK_UDP),
                        test_transmit_segmentation_vnet_fallback);
#endif
    qtest_add_data_func("/bcm570x/transmit-segmentation-vlan-fallback", NULL,
                        test_transmit_segmentation_vlan_fallback);
    qtest_add_data_func("/bcm570x/transmit-no-segmentation", NULL,
                        test_transmit_no_segmentation);
    qtest_add_data_func("/bcm570x/transmit-checksum-bad-header", NULL,
                        test_transmit_checksum_bad_header);
    qtest_add_data_func("/bcm570x/transmit-frame-wordswap", NULL,
                        test_transmit_frame_wordswap);
    qtest_add_data_func("/bcm570x/tx-producer-out-of-range", NULL,
                        test_tx_producer_out_of_range);
    qtest_add_data_func("/bcm570x/receive-round-trip", NULL,
                        test_receive_round_trip);
#ifdef CONFIG_LINUX
    qtest_add_data_func("/bcm570x/receive-vnet-plain", NULL,
                        test_receive_vnet_plain);
#endif
    qtest_add_data_func("/bcm570x/receive-descriptor-dma-error", NULL,
                        test_receive_descriptor_dma_error);
    qtest_add_data_func("/bcm570x/receive-completion-dma-error", NULL,
                        test_receive_completion_dma_error);
    qtest_add_data_func("/bcm570x/receive-payload-dma-error", NULL,
                        test_receive_payload_dma_error);
    qtest_add_data_func("/bcm570x/receive-descriptor-dma-error-after-progress",
                        GINT_TO_POINTER(RX_DMA_FAULT_DESCRIPTOR),
                        test_receive_dma_error_after_progress);
    qtest_add_data_func("/bcm570x/receive-payload-dma-error-after-progress",
                        GINT_TO_POINTER(RX_DMA_FAULT_PAYLOAD),
                        test_receive_dma_error_after_progress);
    qtest_add_data_func("/bcm570x/receive-completion-dma-error-after-progress",
                        GINT_TO_POINTER(RX_DMA_FAULT_COMPLETION),
                        test_receive_dma_error_after_progress);
    qtest_add_data_func("/bcm570x/receive-ring-laps", NULL,
                        test_receive_ring_laps);
    qtest_add_data_func("/bcm570x/receive-ring-full-recovers", NULL,
                        test_receive_ring_full_recovers);
    qtest_add_data_func("/bcm570x/receive-mac-enable", NULL,
                        test_receive_mac_enable);
    qtest_add_data_func("/bcm570x/queued-receive-released-by-producer", NULL,
                        test_queued_receive_released_by_producer);
    qtest_add_data_func("/bcm570x/receive-queue-global-reset",
                        GINT_TO_POINTER(0), test_receive_queue_reset);
    qtest_add_data_func("/bcm570x/receive-queue-system-reset",
                        GINT_TO_POINTER(1), test_receive_queue_reset);
    qtest_add_data_func("/bcm570x/receive-checksum-offload", NULL,
                        test_receive_checksum_offload);
    qtest_add_data_func("/bcm570x/receive-checksum-not-ip", NULL,
                        test_receive_checksum_not_ip);
    qtest_add_data_func("/bcm570x/receive-overruns-nothing", NULL,
                        test_receive_overruns_nothing);
    qtest_add_data_func("bcm570x/receive-waits-for-arrival-deadline",
                        NULL,
                        test_receive_waits_for_its_arrival_deadline);
    qtest_add_data_func("/bcm570x/receive-jumbo-frame", NULL,
                        test_receive_jumbo_frame);
    qtest_add_data_func(
        "/bcm570x/receive-drops-standard-with-empty-standard-ring", NULL,
        test_receive_drops_standard_with_empty_standard_ring);
    qtest_add_data_func("/bcm570x/receive-drops-oversize-with-empty-jumbo-ring",
                        NULL,
                        test_receive_drops_oversize_with_empty_jumbo_ring);
    qtest_add_data_func("/bcm570x/receive-oversize-dropped", NULL,
                        test_receive_oversize_dropped);
    qtest_add_data_func("/bcm570x/receive-frame-wordswap", NULL,
                        test_receive_frame_wordswap);
    qtest_add_data_func("/bcm570x/receive-interrupt-coalescing", NULL,
                        test_receive_interrupt_coalescing);
    qtest_add_data_func("/bcm570x/receive-coalescing-timer", NULL,
                        test_receive_coalescing_timer);
    qtest_add_data_func("/bcm570x/receive-coalescing-zero-ticks", NULL,
                        test_receive_coalescing_zero_ticks);
    qtest_add_data_func("/bcm570x/receive-without-a-criterion-is-silent", NULL,
                        test_receive_without_a_criterion_is_silent);
    qtest_add_data_func("/bcm570x/transmit-without-a-criterion-is-silent", NULL,
                        test_transmit_without_a_criterion_is_silent);
    qtest_add_data_func("/bcm570x/receive-expired-timer-reports-at-once", NULL,
                        test_receive_expired_timer_reports_at_once);
    qtest_add_data_func("/bcm570x/transmit-expired-timer-reports-at-once", NULL,
                        test_transmit_expired_timer_reports_at_once);
    qtest_add_data_func("/bcm570x/receive-producer-follows-its-completion",
                        NULL, test_receive_producer_follows_its_completion);
    qtest_add_data_func("/bcm570x/receive-coalescing-status-dma-retry", NULL,
                        test_receive_coalescing_status_dma_retry);
    qtest_add_data_func("/bcm570x/global-reset-cancels-rx-coalescing", NULL,
                        test_global_reset_cancels_receive_coalescing);
    qtest_add_data_func("/bcm570x/system-reset-cancels-tx-coalescing", NULL,
                        test_system_reset_cancels_transmit_coalescing);
    qtest_add_data_func("/bcm570x/rx-producer-out-of-range", NULL,
                        test_rx_producer_out_of_range);
    qtest_add_data_func("/bcm570x/interrupt-mailbox", NULL,
                        test_interrupt_mailbox);
    qtest_add_data_func("/bcm570x/status-block-gated-by-hcc", NULL,
                        test_status_block_gated_by_hcc);
    qtest_add_data_func("/bcm570x/status-dma-error", NULL,
                        test_status_dma_error);
    qtest_add_data_func("/bcm570x/vmstate-versions-nested-phy", NULL,
                        test_vmstate_versions_nested_phy);
    qtest_add_data_func("/bcm570x/state-survives-migration", NULL,
                        test_state_survives_migration);
    qtest_add_data_func(
        "/bcm570x/masked-interrupt-survives-migration", NULL,
        test_masked_interrupt_survives_migration);
    qtest_add_data_func("/bcm570x/migration/link-down", GINT_TO_POINTER(0),
                        test_migrated_link_controls_egress);
    qtest_add_data_func("/bcm570x/migration/link-up", GINT_TO_POINTER(1),
                        test_migrated_link_controls_egress);
    qtest_add_data_func("/bcm570x/arrival-line-migrates-only-what-it-holds",
                        NULL,
                        test_arrival_line_migrates_only_what_it_holds);
    qtest_add_data_func("/bcm570x/dual-port-present-and-independent", NULL,
                        test_dual_port_present_and_independent);
    qtest_add_data_func("/bcm570x/dual-port-survives-migration", NULL,
                        test_dual_port_survives_migration);

    return g_test_run();
}
