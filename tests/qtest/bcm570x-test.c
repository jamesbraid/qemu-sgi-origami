/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the Broadcom NetXtreme BCM570x adapter.
 *
 * The bcm570x is a general-purpose little-endian PCI NIC. This test drives it
 * on a generic x86_64/i386 PC machine through the standard PCI config and BAR
 * mechanisms (libqos qpci) -- the device must work identically on any PCI host,
 * so its own register contract, ring engines, and DMA byte order are proven
 * here with no host-bridge assumption. The device's native little-endian DMA
 * composed with the SGI Origin 200 Bridge's per-slot byte-swap correction is a
 * separate machine-level concern, validated by sn0-bcm570x-test.c (see
 * docs/library/nic-on-bridge-conventions.md).
 *
 * The card carries the SGI IRIS Release 2 Gigabit board's PCI four-tuple:
 * IRIX's tg driver registers on the raw Broadcom primary vendor/device and then,
 * at attach, insists the card also carry SGI's subsystem identity, so the test
 * asserts the whole four-tuple, plus the two indirect config-space windows the
 * driver uses before it ever touches the BAR (docs/datasheets/bcm570x.md,
 * "three access paths" and the qtest checklist).
 *
 * Byte order is asserted as a golden fixed-byte-layout: the device delivers its
 * DMA in native little-endian, so a status flag of 0x00000001 lands in guest
 * memory as bytes 01 00 00 00. A self-round-trip through the model cannot see
 * the swap direction (swap-on-write and swap-on-read cancel), so every DMA
 * byte-order check reads the raw bytes and asserts the exact little-endian
 * placement.
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "libqtest.h"
#include "libqos/pci-pc.h"
#include "qapi/error.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "hw/pci/pci_regs.h"
#include "standard-headers/linux/virtio_net.h"
#include <sys/socket.h>
#include <netinet/in.h>
#ifdef CONFIG_LINUX
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#endif

/*
 * The card is placed at a free PCI slot on the PC root bus. Slots 0-2 hold the
 * host bridge, ISA bridge and VGA; slot 4 is free, matching the device's own
 * default probe slot in the machine.
 */
#define BCM_SLOT                4
#define BCM_DEVFN               QPCI_DEVFN(BCM_SLOT, 0)

/* The base machine and network backend shared by most tests. */
#define BCM_MACHINE             "-nodefaults -machine pc"

/* The single-port SGI IRIS Release 2 board's four-tuple (bcm5701, copper). */
#define BCM_VENDOR_ID           0x14e4
#define BCM_DEVICE_ID           0x1645
#define BCM_SUBSYS_VENDOR_ID    0x10a9
#define BCM_SUBSYS_ID           0x8010

/* The indirect-access config registers (docs/datasheets/bcm570x.md). */
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
 * The buffer-manager mode register (BUFMGR_MODE in tg3, BGE_BMAN_MODE in bge).
 * ENABLE is the enable-request bit; ATTN_ENABLE arms its attention. The public
 * drivers set ENABLE and poll for it to stay SET, but IRIX tg sets ENABLE and
 * polls for it to CLEAR -- the chip acknowledges the request by dropping the
 * bit -- so the model self-clears ENABLE while ATTN_ENABLE persists.
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
 * The Broadcom auxiliary status summary register (BRGPHY_MII_AUXSTS, 0x19) the
 * copper link-up path reads for speed and duplex. Its AN_RES field (0x0700)
 * carries the negotiated result -- 0x0700 is 1000baseT full duplex -- alongside
 * the autoneg-complete and link bits.
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
#define BCM_HCC_STATS_TICKS     0x3c28u
#define BCM_STATUS_TICK_MAX_NS  (250 * 1000 * 1000)
#define BCM_HCC_RX_COAL_TICKS   0x3c08u        /* receive coalescing: microseconds */
#define BCM_HCC_TX_COAL_TICKS   0x3c0cu
#define BCM_HCC_TX_MAX_COAL_BDS 0x3c14u
#define BCM_HCC_RX_MAX_COAL_BDS 0x3c10u        /* receive coalescing: descriptors */
#define BCM_MODE_CTL            0x6800u
#define BCM_DMA_SWAP_OPTIONS    0x36u          /* the driver's MODE_CTL swap arm */
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
/* Status-block idx[0]: rx_producer at 0x10, tx_consumer at 0x12 (tg3 layout). */
#define BCM_STATUS_RX_PROD_OFF  16
#define BCM_STATUS_TX_CONS_OFF  18
#define BCM_STATUS_TAG_OFF       4      /* the device bumps this once per status DMA */

/* Receive ring. */
#define BCM_MBX_RX_STD_PROD_LO  0x026cu
#define BCM_MBX_RX_CONS0_LO     0x0284u
#define BCM_RX_STD_RCB_HI       0x2450u
#define BCM_RX_STD_RCB_LO       0x2454u
#define BCM_RX_STD_RCB_MAXLEN   0x2458u
#define BCM_SRAM_RX_RETURN_RCB  0x0200u
#define BCM_TXBD_SIZE           16
#define BCM_RXBD_SIZE           32
#define BCM_RXBD_LEN            0x08   /* idx_len word: length in the low half */
#define BCM_RXBD_IDX            0x0a   /* idx_len word: ring index in the high half */
#define BCM_RXBD_FLAGS          0x0c   /* type_flags word: flags in the low half */
#define BCM_RX_JUMBO_RCB_HI     0x2440u
#define BCM_RX_JUMBO_RCB_LO     0x2444u
#define BCM_RX_JUMBO_RCB_MAXLEN 0x2448u
#define BCM_MBX_RX_JUMBO_PROD_LO 0x0274u
#define BCM_RXBDFLAG_JUMBO_RING 0x0020u
#define BCM_RXBD_TCP_CSUM       0x10   /* csum word: TCP/UDP result in the low half */
#define BCM_RXBD_IP_CSUM        0x12   /* csum word: IP result in the high half */
#define BCM_RXBDFLAG_TCP_UDP_CKSUM 0x0001u
#define BCM_RXBDFLAG_IP_CKSUM   0x0002u
#define BCM_RXBDFLAG_END        0x0004u
#define BCM_CSUM_GOOD           0xffffu /* a one's-complement sum that checks out */
#define BCM_ETH_FCS_LEN         4      /* length field carries the FCS the driver strips */

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
 * Guest-physical DMA scratch. There is no host IOMMU on the PC machine, so the
 * device DMAs to exactly the guest-physical address the driver programs; the
 * scratch sits above the first megabyte, in RAM nothing else uses under qtest.
 */
#define TX_RING_PHYS            0x00200000ULL
#define TX_BUF_PHYS             0x00210000ULL
#define STATUS_PHYS             0x00220000ULL
#define STD_RING_PHYS           0x00230000ULL
#define RET_RING_PHYS           0x00240000ULL
#define RX_BUF_PHYS             0x00250000ULL
#define STATUS2_PHYS            0x00260000ULL
#define TX_RING_LEN            4u             /* descriptors in the test ring */
#define RX_RING_LEN           4u
/*
 * The standard ring's control block carries the size of the buffer behind each
 * descriptor, not a count of descriptors. A driver at the standard MTU posts
 * something like this; a frame that does not fit belongs on the jumbo ring.
 */
#define STD_BUF_SIZE          1536u
#define JUMBO_RING_PHYS       0x00260000ULL
#define JUMBO_BUF_PHYS        0x00270000ULL
#define STD_STALE_BUF_PHYS    0x00280000ULL
#define JUMBO_STALE_BUF_PHYS  0x00290000ULL
#define JUMBO_BUF_SIZE        9216u
#define UNMAPPED_PHYS         0x02000000ULL
#define RAM_END_16M           0x01000000ULL
#define COAL_RING_LEN         64u            /* a ring long enough for a coalescing burst */

/* A ring and buffer area big enough to time a burst rather than a packet. */
#define BENCH_RING_PHYS       0x00300000ULL
#define BENCH_BUF_PHYS        0x00400000ULL
#define BENCH_RING_LEN        256u
#define BENCH_FRAMES          255u
#define BENCH_FRAME_LEN       1472u
#define BENCH_BUF_STRIDE      1536u
#define BENCH_ROUNDS          40u    /* enough frames to be a rate, not a sample */

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
    b->bus = qpci_new_pc(qts, NULL);
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
    qpci_free_pc(b->bus);
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

/* A PHY register read/write through the MAC's MI_COMM register (needs BAR0). */
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
 * Little-endian field access, matching the device's native DMA order. These
 * helpers build and decode the descriptors the test exchanges with the device;
 * the golden byte-layout assertions below read the raw bytes to pin the
 * direction the round trip alone cannot see.
 */
static void put32le(uint8_t *b, uint32_t v)
{
    b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
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

static void put16le(uint8_t *b, uint16_t v)
{
    b[0] = v; b[1] = v >> 8;
}

static uint32_t get32le(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t get16le(const uint8_t *b)
{
    return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

/* True when the card is driving its INTA line, as the driver reads it: the
 * chip clears PCISTATE.INTR_NOT_ACTIVE while the line is asserted. */
static bool intr_line_up(QPCIDevice *dev)
{
    return (qpci_config_readl(dev, BCM_PCI_PCISTATE) & BCM_PCISTATE_INTR_NA)
           == 0;
}

/*
 * The minimum plumbing for the chip to DMA a status block: a send-ring control
 * block with a ring length so a doorbell passes the range guard, and the status
 * block's host address. The host-coalescing block is left disabled -- each test
 * enables it when it wants a status update -- and the status region is zeroed. A
 * doorbell of producer 0 then drives a status-block DMA with no ring walk, which
 * is all the interrupt and gating tests need (no descriptors, no frame).
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
 * A socket backend delivers a received frame on a later pass of the main loop,
 * so the status block's receive producer is polled rather than read once.
 */
static void wait_for_rx_prod(Bcm *b, uint16_t want)
{
    uint8_t idx[2];
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        qtest_memread(b->qts, STATUS_PHYS + BCM_STATUS_RX_PROD_OFF, idx, 2);
        if (get16le(idx) == want) {
            return;
        }
        g_usleep(5000);
    }
    qtest_memread(b->qts, STATUS_PHYS + BCM_STATUS_RX_PROD_OFF, idx, 2);
    g_assert_cmphex(get16le(idx), ==, want);
}

/* The status tag the device bumps once per status-block DMA, so its delta over
 * a burst counts how many interrupts the coalescing block actually raised. */
static uint32_t read_status_tag(Bcm *b)
{
    uint8_t tag[4];

    qtest_memread(b->qts, STATUS_PHYS + BCM_STATUS_TAG_OFF, tag, sizeof(tag));
    return get32le(tag);
}

/* Poll the return ring until the descriptor at idx is marked end-of-frame -- the
 * per-frame completion the device writes even while it coalesces the status
 * block, so a test can tell a frame was received before any interrupt fired. */
static void wait_for_ret_end(Bcm *b, unsigned int idx)
{
    uint8_t flags[2];
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        qtest_memread(b->qts, RET_RING_PHYS + (uint64_t)idx * BCM_RXBD_SIZE +
                      BCM_RXBD_FLAGS, flags, 2);
        if (get16le(flags) & BCM_RXBDFLAG_END) {
            return;
        }
        g_usleep(5000);
    }
    qtest_memread(b->qts, RET_RING_PHYS + (uint64_t)idx * BCM_RXBD_SIZE +
                  BCM_RXBD_FLAGS, flags, 2);
    g_assert_cmphex(get16le(flags) & BCM_RXBDFLAG_END, ==, BCM_RXBDFLAG_END);
}

/*
 * The four-tuple IRIX tg requires. The Broadcom primary vendor/device are
 * standard, and the SGI subsystem identity sits in the standard PCI subsystem
 * fields: subsystem vendor at config 0x2C, subsystem id at 0x2E.
 *
 * Note on offsets: docs/datasheets/bcm570x.md's config table and qtest
 * checklist place the subsystem *device* at 0x2C and the subsystem *vendor* at
 * 0x2E, taking OpenBSD bge's header macro names (BGE_PCI_SUBSYS 0x2C /
 * BGE_PCI_SUBVID 0x2E) at face value. Those macro names are backwards: bge's
 * own driver (if_bge.c) reads the subsystem through the standard
 * PCI_SUBSYS_ID_REG (0x2C) dword and takes PCI_VENDOR() -- the low half at
 * 0x2C -- as the subsystem vendor and PCI_PRODUCT() -- the high half at 0x2E --
 * as the device. That is plain standard-PCI layout, which is what real silicon
 * and IRIX both use, so the card presents SGI vendor 0x10a9 at 0x2C and the
 * board id 0x8010 at 0x2E.
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
 * The indirect register window round-trips. Writing a register offset to config
 * 0x78 and a value to config 0x80 must reach the same register file the BAR
 * exposes at 0x0000-0x7FFF: read the register back through BAR0 at that offset
 * and it must match, proving 0x78/0x80 alias the file.
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
 * the word back both through config 0x7C/0x84 and through the BAR0 window, and
 * the two views must agree.
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
 * ready value at once. This covers the bge ordering, where the magic is written
 * before the reset; the reset re-arms the mailbox and self-clears its trigger.
 */
static void test_reset_handshake_via_bar(const void *data)
{
    Bcm b;

    bcm_open(&b, bcm_init());

    ind_mem_writel(&b, BCM_SRAM_FW_MBOX, BCM_FW_MAGIC);
    g_assert_cmphex(ind_mem_readl(&b, BCM_SRAM_FW_MBOX), ==,
                    BCM_FW_HANDSHAKE_DONE);

    bar_writel(&b, BCM_MISC_CFG, BCM_MISCCFG_RESET);

    /* The reset trigger self-clears; the driver polls MISC_CFG bit 0 for it. */
    g_assert_cmphex(bar_readl(&b, BCM_MISC_CFG) & BCM_MISCCFG_RESET, ==, 0);

    /* The reset leaves the mailbox reading ready for the post-reset poll. */
    g_assert_cmphex(ind_mem_readl(&b, BCM_SRAM_FW_MBOX), ==,
                    BCM_FW_HANDSHAKE_DONE);

    bcm_close(&b);
}

/*
 * The IRIX ordering: the driver resets the chip first, then writes the magic to
 * the firmware mailbox and polls it. Because the mailbox returns the one's-
 * complement of the last value written, the magic reads back ready without any
 * static value having been arranged during the reset -- the case a mailbox that
 * only latched a complement on reset would time out on, the driver's magic
 * write having overwritten it.
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
 * The firmware-progress register (0x5034) the IRIX driver polls after reset. It
 * aborts the moment the register changes from its all-ones init to a value it
 * does not recognise, and a bare zero reads as exactly such a value (the boot
 * failure "Progress code=0x0"). The chip holds it at the all-ones "no progress
 * yet" sentinel -- out of reset and after a global reset -- so the driver reads
 * "unchanged" and keeps polling the mailbox instead of failing.
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
 * The same handshake, but the reset is triggered through the indirect register
 * window (config 0x78/0x80) rather than the BAR -- the path the 5700/5701 take.
 * No BAR mapping is needed: every step here is config space.
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
 * persists -- the driver sets the bit and polls for it to read back clear.
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
     * through it -- reset on its own, carrying no ENABLE, which leaves the
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
 * HCC_MODE stores and reads back faithfully: the driver's idle sequence writes
 * ENABLE, then writes 0 and polls up to ~2000 times for ENABLE to read clear,
 * aborting the attach if it never does.
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
 * polls ENABLE for CLEAR ("TG3_BM_ENA bit did not clear" on timeout) -- the
 * opposite of tg3/bge, which poll it for SET. So a plain store-and-read-back
 * would leave ENABLE set and hang the driver. The chip acknowledges the request
 * by dropping ENABLE at once, while ATTN_ENABLE persists.
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
 * The host-coalescing idle poll reads the WHOLE register for zero, not just the
 * ENABLE bit ("Host Coalescing Mode Rgtr did not clear" on timeout). Write a
 * value with several bits set, then write 0 and confirm every bit reads back
 * clear, the state the driver waits for before it reprograms the block.
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

    /* A read of the PHY identity: BUSY clears and the low 16 carry the value. */
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
 * from. For the single-port default it must decode to BCM5701 (chip id 0x0105,
 * ASIC revision chip-id >> 12 == 0), not merely non-zero.
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
 * The MAC-address mailbox is honoured. The model presents the six bytes of the
 * -device mac= in SRAM 0x0C14/0x0C18 with the tag 0x484B in the high half of
 * 0x0C14, the path bge_attach reads before it falls back to an EEPROM.
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
 * from the serial EEPROM at power-on. A driver that reads the address straight
 * from these MAC control registers -- not the SRAM mailbox -- still sees the
 * -device mac=.
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
 * link comes up a settle after the part starts negotiating -- the load-bearing
 * link/autoneg result. MI_STS, the auto-poll link latch, follows the carrier.
 *
 * The settle is the contract, not a detail to assert away: a restart negotiates
 * for a while and completes afterwards, so a driver watching for the completion
 * it asked for sees the transition. Completing the moment the guest writes the
 * control register hides that transition and hangs IRIX tg's bring-up.
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

    /* Configure the interface (restart autonegotiation): still negotiating. */
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
 * only when the brgphy auxiliary status summary (PHY register 0x19) reads with
 * ACOMP (0x8000) set and a valid highest-common-denominator speed/duplex in the
 * AN_RES field (bits 10:8), and it treats an all-ones read as a PHY that is not
 * responding. So before the link is up the register must answer with ACOMP
 * clear and a real (non-0xffff) value, and once up it must present ACOMP with a
 * settled resolution -- here 1000baseT full duplex, matching the gigabit answer
 * the 1000baseT status register already gives.
 */
static void test_phy_aux_status_link_up(const void *data)
{
    Bcm b;
    uint16_t aux;

    bcm_open(&b, bcm_init());

    /* Still negotiating: ACOMP clear, but a real value so it is not "no PHY". */
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
 * The load-bearing byte-order test, as a golden fixed-byte-layout assertion.
 * The chip DMAs the status block to guest memory; because the device is a
 * little-endian PCI part, a status flag of 0x00000001 must occupy bytes
 * 01 00 00 00 in guest memory, and a producer index of 1 must read {01, 00} --
 * NOT the big-endian {00, 00, 00, 01} / {00, 01}. A self-round-trip could not
 * tell the two apart, so the raw bytes are read and the exact placement pinned.
 * This runs on the generic PC host with no bridge in the path, so it asserts
 * the device's native delivery directly.
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

    /* The status flag, byte for byte: little-endian 1 is 01 00 00 00. */
    g_assert_cmphex(sb[0], ==, 0x01);
    g_assert_cmphex(sb[1], ==, 0x00);
    g_assert_cmphex(sb[2], ==, 0x00);
    g_assert_cmphex(sb[3], ==, 0x00);
    g_assert_cmphex(get32le(sb + 0), ==, BCM_STATFLAG_UPDATED);

    bcm_close(&b);
}

/*
 * Host-address word-swap. The big-endian IRIX driver sets ENDIAN_WORDSWAP and
 * writes each 64-bit host address as two 32-bit halves word-swapped -- the
 * intended low word into ADDR_HI, the intended high word into ADDR_LO -- so the
 * chip must swap them back when it composes the DMA target. Program a
 * status-block address of 0x01000000 word-swapped (HI <- 0x01000000, LO <- 0)
 * and confirm the block DMAs there. Composing (HI<<32)|LO raw would target
 * 0x0100000000000000, off the end of RAM, and land nothing.
 */
static void test_status_block_hostaddr_wordswap(const void *data)
{
    const uint64_t intended = 0x01000000;   /* 16 MiB, within the 128 MiB RAM */
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
    g_assert_cmphex(get32le(sb + 0), ==, BCM_STATFLAG_UPDATED);

    bcm_close(&b);
}

/*
 * The transmit round trip. With the driver's swap options written, a transmit
 * descriptor and its buffer are DMA'd, the frame reaches the backend
 * byte-for-byte, and the status block the chip writes back lands native
 * little-endian with its transmit consumer advanced. The frame pattern has
 * every byte distinct so a swap anywhere on the path shows. The status-block
 * bytes are checked as a golden little-endian layout, the direction a round
 * trip alone cannot see.
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

    /* The driver's byte-swap programming: stored for read-back, no live swap. */
    ind_reg_writel(&b, BCM_MODE_CTL, BCM_DMA_SWAP_OPTIONS);

    /* The send-ring control block in on-chip SRAM: host base and ring length. */
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 0, TX_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4, TX_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);

    /* The status block's host address, and enable the coalescing block. */
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    /* One transmit descriptor at ring index 0, built little-endian. */
    memset(bd, 0, sizeof(bd));
    put32le(bd + 0, TX_BUF_PHYS >> 32);
    put32le(bd + 4, TX_BUF_PHYS & 0xffffffff);
    /* len_flags: flags in the low 16 bits (0x08), length in the high (0x0a). */
    put16le(bd + 8, BCM_TXBDFLAG_END);
    put16le(bd + 10, sizeof(frame));
    qtest_memwrite(b.qts, TX_RING_PHYS, bd, sizeof(bd));
    qtest_memwrite(b.qts, TX_BUF_PHYS, frame, sizeof(frame));
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);

    /* Ring the doorbell: one descriptor produced. */
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 1);

    /* The frame reached the backend unchanged. */
    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));

    /* The status block landed little-endian with the transmit consumer
     * advanced. */
    qtest_memread(b.qts, STATUS_PHYS, sb, sizeof(sb));
    g_assert_cmphex(get32le(sb + 0), ==, BCM_STATFLAG_UPDATED);
    g_assert_cmphex(get16le(sb + BCM_STATUS_TX_CONS_OFF), ==, 1);
    /* Raw bytes prove the order: little-endian 1 is {0x01, 0x00}. */
    g_assert_cmphex(sb[BCM_STATUS_TX_CONS_OFF], ==, 0x01);
    g_assert_cmphex(sb[BCM_STATUS_TX_CONS_OFF + 1], ==, 0x00);

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
    Bcm b;
    uint32_t before_poll, after_poll;

    bcm_open(&b, bcm_init());

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

    put32le(rx_bd + 0, RX_BUF_PHYS >> 32);
    put32le(rx_bd + 4, RX_BUF_PHYS & 0xffffffff);
    qtest_memwrite(b.qts, STD_RING_PHYS, rx_bd, sizeof(rx_bd));
    qtest_memset(b.qts, RX_BUF_PHYS, 0, sizeof(landed));
    qtest_memset(b.qts, RET_RING_PHYS, 0, sizeof(ret));
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);

    bar_writel(&b, BCM_HCC_TX_MAX_COAL_BDS, 2);
    bar_writel(&b, BCM_HCC_TX_COAL_TICKS, 1000000);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_false(intr_line_up(b.dev));

    put32le(tx_bd + 0, TX_BUF_PHYS >> 32);
    put32le(tx_bd + 4, TX_BUF_PHYS & 0xffffffff);
    put16le(tx_bd + 8, BCM_TXBDFLAG_END);
    put16le(tx_bd + 10, sizeof(arp_request));
    qtest_memwrite(b.qts, TX_RING_PHYS, tx_bd, sizeof(tx_bd));
    qtest_memwrite(b.qts, TX_BUF_PHYS, arp_request, sizeof(arp_request));

    /*
     * User networking answers the ARP request during qemu_send_packet(). The
     * first read observes the transmit callback boundary; the second follows
     * exactly one AIO poll, when the deferred reply must be fully published.
     */
    before_poll = qtest_writel_then_readl_poll(
        b.qts, b.bar.addr + BCM_MBX_TX_PROD_LO, 1,
        STATUS_PHYS + BCM_STATUS_RX_PROD_OFF, &after_poll);
    g_assert_cmphex(before_poll, ==, 0);
    g_assert_cmphex(after_poll, ==, 0x00010001);
    g_assert_true(intr_line_up(b.dev));

    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_IDX), ==, 0);
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
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
                             " -m 16M"
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

    put32le(bd + 0, TX_BUF_PHYS >> 32);
    put32le(bd + 4, TX_BUF_PHYS & 0xffffffff);
    put16le(bd + 8, BCM_TXBDFLAG_END);
    put16le(bd + 10, sizeof(frame));
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
                             " -m 16M"
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0"
                             " -d guest_errors -D %s",
                             sv[1], BCM_SLOT, logfile));

    put32le(ring, TX_BUF_PHYS >> 32);
    put32le(ring + 4, TX_BUF_PHYS & 0xffffffff);
    put16le(ring + 10, 28);
    put32le(ring + BCM_TXBD_SIZE, UNMAPPED_PHYS >> 32);
    put32le(ring + BCM_TXBD_SIZE + 4, UNMAPPED_PHYS & 0xffffffff);
    put16le(ring + BCM_TXBD_SIZE + 8, BCM_TXBDFLAG_END);
    put16le(ring + BCM_TXBD_SIZE + 10, 32);
    qtest_memwrite(b.qts, TX_RING_PHYS, ring, sizeof(ring));
    qtest_memwrite(b.qts, TX_BUF_PHYS, frame, 28);
    qtest_memset(b.qts, STATUS_PHYS, 0, sizeof(status));
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB, TX_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4,
                   TX_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 2);
    wait_for_log_marker(b.qts, logfile,
                        "bcm570x: transmit payload read at 0x2000000 failed; "
                        "frame left ready to retry");
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 0);
    g_assert_cmphex(read_status_tag(&b), ==, tag);
    g_assert_false(intr_line_up(b.dev));
    g_assert_cmpint(recv(sv[0], &byte, sizeof(byte), MSG_DONTWAIT), ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);

    /* Repair only fragment two; producer 2 must replay fragment one. */
    put32le(ring + BCM_TXBD_SIZE, (TX_BUF_PHYS + 0x100) >> 32);
    put32le(ring + BCM_TXBD_SIZE + 4,
            (TX_BUF_PHYS + 0x100) & 0xffffffff);
    qtest_memwrite(b.qts, TX_RING_PHYS + BCM_TXBD_SIZE,
                   ring + BCM_TXBD_SIZE, BCM_TXBD_SIZE);
    qtest_memwrite(b.qts, TX_BUF_PHYS + 0x100, frame + 28, 32);
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 2);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 2);
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
                             " -m 16M"
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0"
                             " -d guest_errors -D %s",
                             sv[1], BCM_SLOT, logfile));

    put32le(ring, TX_BUF_PHYS >> 32);
    put32le(ring + 4, TX_BUF_PHYS & 0xffffffff);
    put16le(ring + 8, BCM_TXBDFLAG_END);
    put16le(ring + 10, empty_first ? 0 : sizeof(first));
    put32le(ring + 16, (TX_BUF_PHYS + 0x100) >> 32);
    put32le(ring + 20, (TX_BUF_PHYS + 0x100) & 0xffffffff);
    put16le(ring + 26, FIRST_FRAGMENT_LEN);
    put32le(ring + 32, UNMAPPED_PHYS >> 32);
    put32le(ring + 36, UNMAPPED_PHYS & 0xffffffff);
    put16le(ring + 40, BCM_TXBDFLAG_END);
    put16le(ring + 42, sizeof(second) - FIRST_FRAGMENT_LEN);
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
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 3);
    wait_for_log_marker(b.qts, logfile,
                        "bcm570x: transmit payload read at 0x2000000 failed; "
                        "frame left ready to retry");
    if (empty_first) {
        net_socket_expect_no_frame(sv[0]);
    } else {
        len = net_socket_recv_frame(sv[0], got, sizeof(got));
        g_assert_cmpuint(len, ==, sizeof(first));
        g_assert_cmpmem(got, len, first, sizeof(first));
    }
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 1);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_true(intr_line_up(b.dev));
    net_socket_expect_no_frame(sv[0]);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);

    put32le(ring + 32,
            (TX_BUF_PHYS + 0x100 + FIRST_FRAGMENT_LEN) >> 32);
    put32le(ring + 36,
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
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 3);
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
    enum { RAM_END_PHYS = 0x01000000 };
    const uint64_t initial_ring = RAM_END_PHYS - 16;
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
                             " -m 16M"
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0"
                             " -d guest_errors -D %s",
                             sv[1], BCM_SLOT, logfile));

    put32le(bd, TX_BUF_PHYS >> 32);
    put32le(bd + 4, TX_BUF_PHYS & 0xffffffff);
    put16le(bd + 8, BCM_TXBDFLAG_END);
    put16le(bd + 10, sizeof(first));
    qtest_memwrite(b.qts, initial_ring, bd, sizeof(bd));
    qtest_memwrite(b.qts, TX_BUF_PHYS, first, sizeof(first));
    qtest_memset(b.qts, STATUS_PHYS, 0, sizeof(status));
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB, initial_ring >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4,
                   initial_ring & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
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
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 1);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_true(intr_line_up(b.dev));
    net_socket_expect_no_frame(sv[0]);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);

    qtest_memwrite(b.qts, repaired_ring, bd, sizeof(bd));
    memset(bd, 0, sizeof(bd));
    put32le(bd, (TX_BUF_PHYS + 0x100) >> 32);
    put32le(bd + 4, (TX_BUF_PHYS + 0x100) & 0xffffffff);
    put16le(bd + 8, BCM_TXBDFLAG_END);
    put16le(bd + 10, sizeof(second));
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
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 2);
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
    const unsigned int initial_ticks = 0x10000000;
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
                             " -m 16M"
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0"
                             " -d guest_errors -D %s",
                             sv[1], BCM_SLOT, logfile));

    put32le(ring, TX_BUF_PHYS >> 32);
    put32le(ring + 4, TX_BUF_PHYS & 0xffffffff);
    put16le(ring + 8, BCM_TXBDFLAG_END);
    put16le(ring + 10, sizeof(first));
    put16le(ring + 24, BCM_TXBDFLAG_END);
    put32le(ring + 32, UNMAPPED_PHYS >> 32);
    put32le(ring + 36, UNMAPPED_PHYS & 0xffffffff);
    put16le(ring + 40, BCM_TXBDFLAG_END);
    put16le(ring + 42, sizeof(second));
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
    bar_writel(&b, BCM_HCC_TX_COAL_TICKS, initial_ticks);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 3);
    wait_for_log_marker(b.qts, logfile,
                        "bcm570x: transmit payload read at 0x2000000 failed; "
                        "frame left ready to retry");
    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(first));
    g_assert_cmpmem(got, len, first, sizeof(first));
    net_socket_expect_no_frame(sv[0]);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 2);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_true(intr_line_up(b.dev));
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    bar_writel(&b, BCM_HCC_TX_COAL_TICKS, retry_ticks);

    put32le(ring + 32, (TX_BUF_PHYS + 0x100) >> 32);
    put32le(ring + 36, (TX_BUF_PHYS + 0x100) & 0xffffffff);
    qtest_memwrite(b.qts, TX_RING_PHYS + 32, ring + 32, 16);
    qtest_memwrite(b.qts, TX_BUF_PHYS + 0x100, second, sizeof(second));
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 3);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(second));
    g_assert_cmpmem(got, len, second, sizeof(second));
    net_socket_expect_no_frame(sv[0]);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 2);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_false(intr_line_up(b.dev));

    qtest_clock_step(b.qts, (int64_t)retry_ticks * 1000 + 1000);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 3);
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
 * selector alone must not rewrite the frame. Packet attributes remain the same
 * on every fragment through the BD carrying END, so the split case keeps them
 * on both descriptors and cuts the source field itself across their boundary.
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
        put32le(bd + 0, buf >> 32);
        put32le(bd + 4, buf & 0xffffffff);
        put16le(bd + 8, flags);
        put16le(bd + 10, sizeof(frame));
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

    /* A nonzero selector without INSERT leaves the complete frame untouched. */
    qtest_memwrite(b.qts, TX_BUF_PHYS + 4 * 128, frame, sizeof(frame));
    memset(bd, 0, sizeof(bd));
    put32le(bd + 0, (TX_BUF_PHYS + 4 * 128) >> 32);
    put32le(bd + 4, (TX_BUF_PHYS + 4 * 128) & 0xffffffff);
    put16le(bd + 8, BCM_TXBDFLAG_CHOOSE_SRC_ADDR(3) |
                       BCM_TXBDFLAG_END);
    put16le(bd + 10, sizeof(frame));
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
    put32le(bd + 0, (TX_BUF_PHYS + 5 * 128) >> 32);
    put32le(bd + 4, (TX_BUF_PHYS + 5 * 128) & 0xffffffff);
    put16le(bd + 8, BCM_TXBDFLAG_INSERT_SRC_ADDR |
                       BCM_TXBDFLAG_CHOOSE_SRC_ADDR(2));
    put16le(bd + 10, 8);
    qtest_memwrite(b.qts, TX_RING_PHYS + 5 * sizeof(bd), bd, sizeof(bd));
    memset(bd, 0, sizeof(bd));
    put32le(bd + 0, (TX_BUF_PHYS + 6 * 128) >> 32);
    put32le(bd + 4, (TX_BUF_PHYS + 6 * 128) & 0xffffffff);
    put16le(bd + 8, BCM_TXBDFLAG_INSERT_SRC_ADDR |
                       BCM_TXBDFLAG_CHOOSE_SRC_ADDR(2) |
                       BCM_TXBDFLAG_END);
    put16le(bd + 10, sizeof(frame) - 8);
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
 * segments' worth of payload and names the segment size in the descriptor; the
 * card cuts it up. Sending it whole puts a frame on the wire no peer accepts,
 * and the sender then waits out a retransmission timeout for each one, which
 * reads as a link stalling rather than as a malformed frame.
 */
#define LSO_MSS         512u
#define LSO_SEGMENTS    3u
#define LSO_HDR         54u     /* ethernet + IPv4 + TCP, all minimum length */

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
    put32le(bd + 0, TX_BUF_PHYS >> 32);
    put32le(bd + 4, TX_BUF_PHYS & 0xffffffff);
    put16le(bd + 8, flags | BCM_TXBDFLAG_END);
    put16le(bd + 10, (uint16_t)len);
    put16le(bd + 14, (uint16_t)mss);
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
 * Transmit interrupt coalescing. The driver says how many completed descriptors
 * it will wait for; reporting each one as it happens spends an interrupt per
 * frame on a saving nobody asked for.
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
    put32le(bd + 0, buf >> 32);
    put32le(bd + 4, buf & 0xffffffff);
    put16le(bd + 8, flags);
    put16le(bd + 10, len);
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
 * That is what makes coalescing worth anything -- a driver that handed the card
 * a whole burst on one doorbell would get one completion either way.
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
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 4);
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
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 2);
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
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 0);
    g_assert_cmphex(read_status_tag(&b), ==, tag);
    g_assert_false(intr_line_up(b.dev));

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 2);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));
    net_socket_expect_no_frame(sv[0]);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 2);
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
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 1);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 1);
    g_assert_true(intr_line_up(b.dev));
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 3);

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpuint(len, ==, sizeof(second));
    g_assert_cmpmem(got, len, second, sizeof(second));
    net_socket_expect_no_frame(sv[0]);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 3);
    g_assert_cmphex(read_status_tag(&b), ==, tag + 2);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
    close(sv[1]);
}

/*
 * The other half, and the one that matters more: a burst that does not fill a
 * group must still be reported. A count with no clock behind it holds the tail
 * of every transfer back for an interrupt that never comes.
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
 * A zero tick budget means no delay, so a short transmit group must be
 * reported by the doorbell that completed it.
 */
static void test_transmit_coalescing_zero_ticks(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t got[128];
    uint32_t before;

    memset(frame, 0x8d, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    txc_setup(&b, 8, 0);

    before = read_status_tag(&b);
    txc_post(&b, frame, sizeof(frame), 1);
    g_assert_cmpint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==,
                    sizeof(frame));
    g_assert_cmpuint(read_status_tag(&b) - before, ==, 1);

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
                             " -m 16M"
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
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 0);
    g_assert_false(intr_line_up(b.dev));

    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    qtest_clock_step(b.qts, (int64_t)ticks * 1000 + 1000);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16le(status + BCM_STATUS_TX_CONS_OFF), ==, 1);
    g_assert_cmphex(read_status_tag(&b), ==, 1);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
}

/*
 * The MAC status word's change bits are latches, and the driver clears one by
 * writing it back. Storing what it wrote instead leaves every bit it tried to
 * clear reading as set, so the driver sees the condition it just acknowledged
 * still standing and handles it again -- on IRIX that was the carrier being
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
    /* checksum at 24..25 stays zero -- the card is asked to fill it */
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
    put32le(bd + 0, TX_BUF_PHYS >> 32);
    put32le(bd + 4, TX_BUF_PHYS & 0xffffffff);
    put16le(bd + 8, flags);
    put16le(bd + 10, frame_len);
    qtest_memwrite(b->qts, TX_RING_PHYS, bd, sizeof(bd));
    qtest_memwrite(b->qts, TX_BUF_PHYS, frame, frame_len);

    bar_writel(b, BCM_MBX_TX_PROD_LO, 1);
    return net_socket_recv_frame(rx_fd, got, got_size);
}

/*
 * The checksums the driver asks the card to finish. IRIX tg hands a frame over
 * with the IP and TCP checksum fields still empty and the offload bits set in
 * the descriptor, so a model that transmits them untouched puts a frame on the
 * wire that every receiver discards in silence -- and the failure hides, because
 * ICMP and ARP carry checksums the guest computed itself and keep working.
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
 * The lengths in the frame are the guest's bytes, so they are not to be trusted:
 * a header claiming to be shorter than an IPv4 header can be would run the
 * checksum arithmetic off the front of the buffer. The offload is declined and
 * the frame goes out as it came in.
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
 * How fast the send path moves a burst. The driver hands the card a run of
 * descriptors and rings the doorbell once, so this times exactly that: the whole
 * burst leaves on one doorbell, and the frames are counted off the backend. It
 * reports the rate rather than asserting one -- what a host manages depends on
 * the host -- but it fails if a frame is lost or malformed, so the number is
 * never quoted for a run that did not actually carry the traffic.
 */
static void test_transmit_throughput(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t *ring, *bufs, *frame;
    uint8_t got[2048];
    unsigned int i, round;
    gint64 t0, t1;
    guint64 bytes = 0;
    double secs, mbps;

    ring = g_malloc0(BENCH_RING_LEN * 16);
    bufs = g_malloc0(BENCH_FRAMES * BENCH_BUF_STRIDE);
    frame = g_malloc0(BENCH_FRAME_LEN);

    for (i = 0; i < BENCH_FRAME_LEN; i++) {
        frame[i] = (uint8_t)(i * 5 + 3);
    }
    for (i = 0; i < BENCH_FRAMES; i++) {
        uint64_t buf = BENCH_BUF_PHYS + (uint64_t)i * BENCH_BUF_STRIDE;
        uint8_t *bd = ring + i * 16;

        memcpy(bufs + i * BENCH_BUF_STRIDE, frame, BENCH_FRAME_LEN);
        put32le(bd + 0, buf >> 32);
        put32le(bd + 4, buf & 0xffffffff);
        put16le(bd + 8, BCM_TXBDFLAG_END);
        put16le(bd + 10, BENCH_FRAME_LEN);
    }
    /* Complete an empty frame when the consumer wraps back to slot zero. */
    put16le(ring + BENCH_FRAMES * 16 + 8, BCM_TXBDFLAG_END);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 30);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    ind_reg_writel(&b, BCM_MODE_CTL, BCM_DMA_SWAP_OPTIONS);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 0, BENCH_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 4,
                   BENCH_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_SEND_RING_RCB + 8, BENCH_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    qtest_memwrite(b.qts, BENCH_RING_PHYS, ring, BENCH_RING_LEN * 16);
    qtest_memwrite(b.qts, BENCH_BUF_PHYS, bufs,
                   BENCH_FRAMES * BENCH_BUF_STRIDE);

    /*
     * Repeat the burst so the figure is a sustained rate rather than one
     * doorbell's worth. A single burst can flatter a device: the ring is cold,
     * nothing has wrapped, and the sample is too short for the cost of the
     * steady state to show.
     */
    t0 = g_get_monotonic_time();
    for (round = 0; round < BENCH_ROUNDS; round++) {
        unsigned int j;

        bar_writel(&b, BCM_MBX_TX_PROD_LO, BENCH_FRAMES);
        for (j = 0; j < BENCH_FRAMES; j++) {
            size_t len = net_socket_recv_frame(sv[0], got, sizeof(got));

            g_assert_cmpint(len, ==, BENCH_FRAME_LEN);
            g_assert_cmpmem(got, len, frame, BENCH_FRAME_LEN);
            bytes += len;
        }
        /* Hand the ring back to the start for the next round. */
        bar_writel(&b, BCM_MBX_TX_PROD_LO, 0);
    }
    t1 = g_get_monotonic_time();

    secs = (double)(t1 - t0) / G_USEC_PER_SEC;
    mbps = secs > 0 ? ((double)bytes * 8) / (secs * 1000000.0) : 0;
    g_test_message("transmit sustained: %u frames, %" G_GUINT64_FORMAT " bytes "
                   "in %.4f s = %.1f Mbit/s, %.0f frames/s",
                   BENCH_FRAMES * BENCH_ROUNDS, bytes, secs, mbps,
                   secs > 0 ? (BENCH_FRAMES * BENCH_ROUNDS) / secs : 0);

    bcm_close(&b);
    close(sv[0]);
    g_free(ring);
    g_free(bufs);
    g_free(frame);
}

/*
 * What coalescing is worth on the receive side. The same burst is taken twice,
 * once with an update per frame and once with a group per K frames, and both the
 * interrupt count and the wall time are reported. The interrupt count is the
 * honest measure -- the model's own cost per update -- and the timing says
 * whether that saving shows up in how fast the burst clears.
 */
static void receive_burst_cost(unsigned int coal, unsigned int frames,
                               uint32_t *updates, double *secs)
{
    int sv[2];
    Bcm b;
    uint8_t frame[128];
    uint8_t bd[BCM_RXBD_SIZE];
    uint32_t tag_before, tag_after;
    unsigned int i;
    gint64 t0, t1;

    memset(frame, 0xa5, sizeof(frame));
    memset(frame, 0xff, 6);          /* broadcast: always accepted */

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
    bar_writel(&b, BCM_HCC_RX_COAL_TICKS, 0x10000000);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    memset(bd, 0, sizeof(bd));
    put32le(bd + 0, RX_BUF_PHYS >> 32);
    put32le(bd + 4, RX_BUF_PHYS & 0xffffffff);
    for (i = 0; i < frames; i++) {
        qtest_memwrite(b.qts, STD_RING_PHYS + (uint64_t)i * BCM_RXBD_SIZE,
                       bd, sizeof(bd));
    }
    qtest_memset(b.qts, RET_RING_PHYS, 0,
                 (uint64_t)COAL_RING_LEN * BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, frames);

    tag_before = read_status_tag(&b);
    t0 = g_get_monotonic_time();
    for (i = 0; i < frames; i++) {
        net_socket_send_frame(sv[0], frame, sizeof(frame));
    }
    wait_for_rx_prod(&b, frames);
    t1 = g_get_monotonic_time();
    tag_after = read_status_tag(&b);

    *updates = tag_after - tag_before;
    *secs = (double)(t1 - t0) / G_USEC_PER_SEC;

    bcm_close(&b);
    close(sv[0]);
}

static void test_receive_coalescing_cost(const void *data)
{
    const unsigned int frames = 48;   /* whole groups at either setting */
    uint32_t per_frame, grouped;
    double t_per_frame, t_grouped;

    receive_burst_cost(1, frames, &per_frame, &t_per_frame);
    receive_burst_cost(16, frames, &grouped, &t_grouped);

    g_test_message("receive burst of %u frames: per-frame updates %u in %.4f s; "
                   "coalesced-by-16 updates %u in %.4f s (%.1fx fewer updates)",
                   frames, per_frame, t_per_frame, grouped, t_grouped,
                   grouped ? (double)per_frame / grouped : 0);

    /* Whatever the host timing, the saving in updates is the contract. */
    g_assert_cmpuint(per_frame, ==, frames);
    g_assert_cmpuint(grouped, <, per_frame);
}

/*
 * Frame payload byte order under the endian word-swap. With ENDIAN_WORDSWAP set,
 * the big-endian driver lays each 32-bit DMA word down byte-reversed -- the same
 * reversal that lets it write descriptors little-endian -- so a payload it stores
 * reversed must reach the wire in order. Descriptors are read through the
 * little-endian accessors already; a payload is a raw byte stream, so the device
 * has to undo the reversal itself. Store the frame reversed per 32-bit word and
 * confirm the backend receives it straight.
 */
static void test_transmit_frame_wordswap(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[60];       /* what must reach the wire */
    uint8_t swapped[60];     /* what the driver lays into the buffer */
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

    /* The driver arms ENDIAN_WORDSWAP for the big-endian host. It gates the
     * frame swap and the control-block address composition alike, so the
     * register-programmed host addresses are written word-swapped too: the low
     * word into the HI slot, the high word into the LO slot. The descriptor's
     * own buffer address is not register-programmed -- it is read from the
     * descriptor through the little-endian accessors -- so it stays in order. */
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
    put32le(bd + 0, TX_BUF_PHYS >> 32);
    put32le(bd + 4, TX_BUF_PHYS & 0xffffffff);
    put16le(bd + 8, BCM_TXBDFLAG_END);
    put16le(bd + 10, sizeof(frame));
    qtest_memwrite(b.qts, TX_RING_PHYS, bd, sizeof(bd));
    qtest_memwrite(b.qts, TX_BUF_PHYS, swapped, sizeof(swapped));
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);

    bar_writel(&b, BCM_MBX_TX_PROD_LO, 1);

    /* The chip undid the swap: the wire carries the frame in order. */
    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));

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
 * completed descriptor into the return ring little-endian, and advances the
 * status block's receive producer. The device-written descriptor's length field
 * is checked as a golden little-endian layout -- it proves the device's
 * descriptor-write direction, which the round trip alone cannot. Broadcast so
 * the address filter passes it regardless of the station address.
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
    bar_writel(b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    memset(bd, 0, sizeof(bd));
    put32le(bd + 0, RX_BUF_PHYS >> 32);
    put32le(bd + 4, RX_BUF_PHYS & 0xffffffff);
    qtest_memwrite(b->qts, STD_RING_PHYS, bd, sizeof(bd));
    qtest_memset(b->qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(b->qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(b, BCM_MBX_RX_STD_PROD_LO, 1);
    (void)fd;
}

#ifdef CONFIG_LINUX
/* TAP's negotiated vnet header is transport metadata, not Ethernet payload. */
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
 * own Max_Len (0x2448) is unused by hardware (PG105 page 91, Table 341),
 * so the model reads each buffer's size from the descriptor the driver
 * posts, the same field IRIX fills in for its jumbo descriptors.
 */
static void write_jumbo_rx_bd(Bcm *b, uint64_t ring, unsigned int idx,
                              uint64_t buffer, uint16_t buf_len)
{
    uint8_t bd[BCM_RXBD_SIZE] = { 0 };

    put32le(bd + 0, buffer >> 32);
    put32le(bd + 4, buffer & 0xffffffff);
    put16le(bd + BCM_RXBD_LEN, buf_len);
    put16le(bd + BCM_RXBD_FLAGS, BCM_RXBDFLAG_JUMBO_RING);
    qtest_memwrite(b->qts, ring + (uint64_t)idx * BCM_RXBD_SIZE,
                   bd, sizeof(bd));
}

/* Post one jumbo-ring buffer alongside the standard ring rx_open_one set up. */
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

    put32le(bd + 0, buffer >> 32);
    put32le(bd + 4, buffer & 0xffffffff);
    qtest_memwrite(b->qts, ring + (uint64_t)idx * BCM_RXBD_SIZE,
                   bd, sizeof(bd));
}

static uint16_t read_rx_prod(Bcm *b)
{
    uint8_t idx[2];

    qtest_memread(b->qts, STATUS_PHYS + BCM_STATUS_RX_PROD_OFF,
                  idx, sizeof(idx));
    return get16le(idx);
}

/*
 * A frame too big for the buffer behind a standard-ring descriptor must not be
 * written into it. The buffer size is the driver's statement of what it set
 * aside; writing past it corrupts whatever the guest put next in memory, and
 * the damage surfaces as a fault somewhere with no connection to this device.
 * Observed for real: at MTU 9000 with no jumbo ring posted, the guest kernel
 * panicked.
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
    rx_open_one(&b, sv[0]);             /* standard ring only, no jumbo ring */

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
 * guide gives this adapter two MTUs, 1500 and 9000; the part carries the larger
 * one on a separate receive ring whose buffers are sized for it, and marks the
 * completion so the driver recycles from the right ring.
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

    /* Reported whole, marked as the jumbo ring's, and landed byte for byte. */
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_LEN), ==,
                    BCM_JUMBO_FRAME_LEN + BCM_ETH_FCS_LEN);
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_JUMBO_RING,
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
 * available. If that selected ring is empty, the frame must stay queued: an
 * unposted descriptor is stale guest memory, not permission to DMA through it.
 */
static void test_receive_waits_for_standard_ring(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t first[64], second[64], got[64];
    uint8_t ret[BCM_RXBD_SIZE];

    memset(first, 0x31, sizeof(first));
    memset(second, 0x42, sizeof(second));
    memset(first, 0xff, 6);
    memset(second, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_open_one(&b, sv[0]);
    rx_open_jumbo(&b);
    write_rx_bd(&b, STD_RING_PHYS, 1, STD_STALE_BUF_PHYS);

    net_socket_send_frame(sv[0], first, sizeof(first));
    wait_for_rx_prod(&b, 1);
    bar_writel(&b, BCM_MBX_RX_CONS0_LO, 1);

    qtest_memset(b.qts, RET_RING_PHYS + BCM_RXBD_SIZE, 0, BCM_RXBD_SIZE);
    qtest_memset(b.qts, STD_STALE_BUF_PHYS, 0xa5, sizeof(second));
    qtest_memset(b.qts, JUMBO_BUF_PHYS, 0x5a, sizeof(second));
    net_socket_send_frame(sv[0], second, sizeof(second));
    g_usleep(100000);

    g_assert_cmpuint(read_rx_prod(&b), ==, 1);
    qtest_memread(b.qts, STD_STALE_BUF_PHYS, got, sizeof(got));
    memset(first, 0xa5, sizeof(first));
    g_assert_cmpmem(got, sizeof(got), first, sizeof(first));
    qtest_memread(b.qts, JUMBO_BUF_PHYS, got, sizeof(got));
    memset(first, 0x5a, sizeof(first));
    g_assert_cmpmem(got, sizeof(got), first, sizeof(first));

    /* Unmask the existing update, then acknowledge it. */
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_true(intr_line_up(b.dev));
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_false(intr_line_up(b.dev));
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 2);
    wait_for_rx_prod(&b, 2);
    qtest_memread(b.qts, STD_STALE_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), second, sizeof(second));
    qtest_memread(b.qts, RET_RING_PHYS + BCM_RXBD_SIZE, ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_IDX), ==, 1);
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) &
                    BCM_RXBDFLAG_JUMBO_RING, ==, 0);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * The inverse case has the same contract. A jumbo frame cannot borrow a
 * posted standard descriptor while its own selected ring is empty.
 */
static void test_receive_waits_for_jumbo_ring(const void *data)
{
    int sv[2];
    Bcm b;
    g_autofree uint8_t *first = g_malloc(BCM_JUMBO_FRAME_LEN);
    g_autofree uint8_t *second = g_malloc(BCM_JUMBO_FRAME_LEN);
    g_autofree uint8_t *got = g_malloc(BCM_JUMBO_FRAME_LEN);
    uint8_t ret[BCM_RXBD_SIZE];

    memset(first, 0x53, BCM_JUMBO_FRAME_LEN);
    memset(second, 0x64, BCM_JUMBO_FRAME_LEN);
    memset(first, 0xff, 6);
    memset(second, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_open_one(&b, sv[0]);
    rx_open_jumbo(&b);
    write_jumbo_rx_bd(&b, JUMBO_RING_PHYS, 1, JUMBO_STALE_BUF_PHYS,
                      JUMBO_BUF_SIZE);

    net_socket_send_frame(sv[0], first, BCM_JUMBO_FRAME_LEN);
    wait_for_rx_prod(&b, 1);
    bar_writel(&b, BCM_MBX_RX_CONS0_LO, 1);

    qtest_memset(b.qts, RET_RING_PHYS + BCM_RXBD_SIZE, 0, BCM_RXBD_SIZE);
    qtest_memset(b.qts, JUMBO_STALE_BUF_PHYS, 0xa5,
                 BCM_JUMBO_FRAME_LEN);
    net_socket_send_frame(sv[0], second, BCM_JUMBO_FRAME_LEN);
    g_usleep(100000);

    g_assert_cmpuint(read_rx_prod(&b), ==, 1);
    qtest_memread(b.qts, JUMBO_STALE_BUF_PHYS, got, BCM_JUMBO_FRAME_LEN);
    memset(first, 0xa5, BCM_JUMBO_FRAME_LEN);
    g_assert_cmpmem(got, BCM_JUMBO_FRAME_LEN,
                    first, BCM_JUMBO_FRAME_LEN);

    /*
     * Re-enable while only the standard ring can take a frame. Retrying the
     * queued jumbo makes no progress and must preserve this authorization for
     * the jumbo producer that actually makes its selected ring ready.
     */
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_true(intr_line_up(b.dev));
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_false(intr_line_up(b.dev));
    g_usleep(100000);
    g_assert_cmpuint(read_rx_prod(&b), ==, 1);
    bar_writel(&b, BCM_MBX_RX_JUMBO_PROD_LO, 2);
    wait_for_rx_prod(&b, 2);
    qtest_memread(b.qts, JUMBO_STALE_BUF_PHYS, got, BCM_JUMBO_FRAME_LEN);
    g_assert_cmpmem(got, BCM_JUMBO_FRAME_LEN,
                    second, BCM_JUMBO_FRAME_LEN);
    qtest_memread(b.qts, RET_RING_PHYS + BCM_RXBD_SIZE, ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_IDX), ==, 1);
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) &
                    BCM_RXBDFLAG_JUMBO_RING, ==,
                    BCM_RXBDFLAG_JUMBO_RING);

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
 * descriptor. The part verifies both on the way in and reports the sums, which
 * is what lets a driver hand the frame up without reading the payload again;
 * with nothing reported the stack has to make its own pass over every byte.
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
    flags = get16le(ret + BCM_RXBD_FLAGS);

    /* Both results are reported... */
    g_assert_cmphex(flags & BCM_RXBDFLAG_IP_CKSUM, ==, BCM_RXBDFLAG_IP_CKSUM);
    g_assert_cmphex(flags & BCM_RXBDFLAG_TCP_UDP_CKSUM, ==,
                    BCM_RXBDFLAG_TCP_UDP_CKSUM);
    /* ...and both check out, which is what the sums come to when they do. */
    g_assert_cmphex(get16le(ret + BCM_RXBD_IP_CSUM), ==, BCM_CSUM_GOOD);
    g_assert_cmphex(get16le(ret + BCM_RXBD_TCP_CSUM), ==, BCM_CSUM_GOOD);

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
    flags = get16le(ret + BCM_RXBD_FLAGS);

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
     * arrives and takes descriptor zero -- so the ring was left usable rather
     * than the oversized frame half-consuming it.
     */
    net_socket_send_frame(sv[0], small, sizeof(small));
    wait_for_rx_prod(&b, 1);

    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_LEN), ==,
                    sizeof(small) + BCM_ETH_FCS_LEN);

    bcm_close(&b);
    close(sv[0]);
    g_free(frame);
}

/*
 * Many laps of the return ring, driven the way the driver drives it: take each
 * completion, then publish the consumer index so the card knows the entry is
 * free again. A ring short enough to wrap several times over a modest burst is
 * the point -- the faults this catches only appear once the producer has been
 * round at least once, which is why a transfer that fits in one lap passes
 * happily and a longer one stops dead.
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
    put32le(bd + 0, RX_BUF_PHYS >> 32);
    put32le(bd + 4, RX_BUF_PHYS & 0xffffffff);
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
            if (get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END) {
                got = true;
                break;
            }
            g_usleep(5000);
        }
        g_assert_true(got);
        g_assert_cmphex(get16le(ret + BCM_RXBD_LEN), ==,
                        sizeof(frame) + BCM_ETH_FCS_LEN);

        /* Release the entry, as the driver does once it has taken it. */
        bar_writel(&b, BCM_MBX_RX_CONS0_LO, (idx + 1) % LAP_RET_LEN);
        /* And keep buffers posted, the way a driver refills as it consumes. */
        bar_writel(&b, BCM_MBX_RX_STD_PROD_LO,
                   (LAP_STD_POSTED + i + 1) % 512u);
    }

    bcm_close(&b);
    close(sv[0]);
}

/*
 * The ring fills, and then it recovers. Refusing a frame while the driver still
 * owes itself every entry is right; staying refused after the driver has caught
 * up is the failure that looks like a link going dead mid-transfer, and it is
 * the one worth pinning, because a full ring is reached on any sustained
 * transfer and a card that never comes back from it is a card that works until
 * it is used.
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
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    memset(bd, 0, sizeof(bd));
    put32le(bd + 0, RX_BUF_PHYS >> 32);
    put32le(bd + 4, RX_BUF_PHYS & 0xffffffff);
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
    g_usleep(200000);

    /* The last entry is still free: a full ring keeps one, so the producer
     * cannot be mistaken for the consumer. */
    qtest_memread(b.qts, RET_RING_PHYS +
                  (uint64_t)(LAP_RET_LEN - 1) * BCM_RXBD_SIZE,
                  ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==, 0);

    /*
     * An interrupt cycle with the ring still full changes nothing: the frames
     * are waiting on return-ring space, not on the driver's acknowledgement.
     */
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_true(intr_line_up(b.dev));
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    g_assert_false(intr_line_up(b.dev));
    g_usleep(100000);
    qtest_memread(b.qts, RET_RING_PHYS +
                  (uint64_t)(LAP_RET_LEN - 1) * BCM_RXBD_SIZE,
                  ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==, 0);

    /*
     * The driver now takes everything and says so. That consumer write is the
     * readiness transition, and it retries immediately -- no second interrupt
     * acknowledgement, no virtual-clock step.
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
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==, 0);

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
 * while it is clear. The bit "controls whether the Receive MAC state machine is
 * active or not" and resets to 0 [57XX-PG105-R, printed page 391, Table 268],
 * so a halted receiver takes nothing and keeps nothing: setting the bit later
 * must not produce frames from before it was set. That is the whole of the
 * wedge this pins -- a back end holds what the device refuses, and IRIX was
 * handed traffic from before the interface existed, mid-bring-up, in two of
 * five boots.
 *
 * A model that simply never delivered would pass half of this, so each halted
 * stretch is followed by a fresh frame that has to land, and the bit is cleared
 * again afterwards to show the halt is not a one-time bring-up special case.
 *
 * The manual counts what a running receiver drops for want of descriptors
 * (nicNoMoreRXBDs, printed page 110, Table 49). This model carries no
 * statistics block, so there is no counter here to assert.
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
    put32le(bd + 0, RX_BUF_PHYS >> 32);
    put32le(bd + 4, RX_BUF_PHYS & 0xffffffff);
    for (i = 0; i < 8; i++) {
        qtest_memwrite(b.qts, STD_RING_PHYS + (uint64_t)i * BCM_RXBD_SIZE,
                       bd, sizeof(bd));
    }
    qtest_memset(b.qts, RET_RING_PHYS, 0,
                 (uint64_t)LAP_RET_LEN * BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 8);

    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
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
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==, 0);

    /* Running now, so a frame sent now lands. */
    memset(frame, 0x5e, sizeof(frame));
    memset(frame, 0xff, 6);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_rx_prod(&b, 1);
    g_assert_true(intr_line_up(b.dev));
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
                    BCM_RXBDFLAG_END);
    g_assert_cmphex(get16le(ret + BCM_RXBD_LEN), ==,
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
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
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
 * from a timeout with no interrupt in sight, and Linux tg3 refills inside NAPI
 * poll while its interrupt is still masked.
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

    /* Bring-up order: interrupts enabled, engine enabled, no buffer posted. */
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    g_usleep(100000);
    g_assert_cmpuint(read_rx_prod(&b), ==, 0);
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==, 0);

    /* The one event the guest makes: a buffer posted through the mailbox. */
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);

    wait_for_rx_prod(&b, 1);
    qtest_memread(b.qts, RX_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_IDX), ==, 0);
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
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

/*
 * Ask for removal with a refused frame queued against the device, then let
 * reset finish the unplug. Queue teardown must release the backend and leave
 * the main loop responsive after the device state has gone away.
 */
static void test_queued_receive_hot_unplug(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];

    memset(frame, 0xff, sizeof(frame));
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);

    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,id=tg0,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    g_usleep(100000);

    qtest_qmp_device_del_send(b.qts, "tg0");
    qtest_system_reset_nowait(b.qts);
    qtest_qmp_eventwait(b.qts, "DEVICE_DELETED");
    qtest_clock_step(b.qts, 1000);
    qtest_qmp_assert_success(b.qts, "{ 'execute': 'query-status' }");
    qtest_qmp_assert_success(b.qts,
                             "{ 'execute': 'netdev_del',"
                             "  'arguments': { 'id': 'n0' } }");

    bcm_close(&b);
    close(sv[0]);
}

/*
 * Removing the card must release every embedded helper with it. A child left
 * attached to a global bus outlives the card's allocation and makes QEMU's
 * final object-tree teardown access freed parent storage under ASan.
 */
static void test_hot_unplug_exits_cleanly(const void *data)
{
    QTestState *qts = qtest_initf(BCM_MACHINE
                                  " -device bcm570x,id=tg0,addr=%02x.0",
                                  BCM_SLOT);

    qtest_qmp_device_del_send(qts, "tg0");
    qtest_system_reset_nowait(qts);
    qtest_qmp_eventwait(qts, "DEVICE_DELETED");
    qtest_quit(qts);
}

/*
 * Function 0 creates function 1 while it realizes. Occupy function 1 so that
 * the final step fails after the parent has otherwise initialized, and repeat
 * the failed hotplug. Neither attempt may retain the requested network backend.
 */
static void test_failed_realize_releases_backend(const void *data)
{
    QTestState *qts = qtest_initf(BCM_MACHINE
                                  " -netdev user,id=n0"
                                  " -device pci-testdev,addr=%02x.1",
                                  BCM_SLOT);
    g_autofree char *addr = g_strdup_printf("%02x.0", BCM_SLOT);
    unsigned int i;

    for (i = 0; i < 2; i++) {
        g_autofree char *id = g_strdup_printf("tg%u", i);
        QDict *response;

        response = qtest_qmp_assert_failure_ref(
            qts,
            "{ 'execute': 'device_add', 'arguments': {"
            "  'driver': 'bcm570x', 'id': %s, 'addr': %s,"
            "  'model': 'bcm5704', 'multifunction': true,"
            "  'netdev': 'n0' } }",
            id, addr);
        g_assert_nonnull(strstr(qdict_get_str(response, "desc"),
                                "in use by pci-testdev"));
        qobject_unref(response);
    }

    qtest_qmp_assert_success(qts,
                             "{ 'execute': 'netdev_del',"
                             "  'arguments': { 'id': 'n0' } }");
    qtest_qmp_assert_success(qts, "{ 'execute': 'query-status' }");
    qtest_quit(qts);
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
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    /* Post one standard-ring buffer at index 0, built little-endian. */
    memset(bd, 0, sizeof(bd));
    put32le(bd + 0, RX_BUF_PHYS >> 32);
    put32le(bd + 4, RX_BUF_PHYS & 0xffffffff);
    qtest_memwrite(b.qts, STD_RING_PHYS, bd, sizeof(bd));
    qtest_memset(b.qts, RX_BUF_PHYS, 0, sizeof(frame));
    qtest_memset(b.qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);

    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_rx_prod(&b, 1);

    /* The completed descriptor landed in the return ring, little-endian. */
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_IDX), ==, 0);
    g_assert_cmphex(get16le(ret + BCM_RXBD_LEN), ==,
                    sizeof(frame) + BCM_ETH_FCS_LEN);
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
                    BCM_RXBDFLAG_END);
    /* Golden bytes on the length field: 64 payload + 4-byte FCS = 68 is
     * {0x44, 0x00}, little-endian. */
    g_assert_cmphex(ret[BCM_RXBD_LEN], ==, 0x44);
    g_assert_cmphex(ret[BCM_RXBD_LEN + 1], ==, 0x00);

    /* The status block's receive producer advanced, little-endian. */
    qtest_memread(b.qts, STATUS_PHYS, sb, sizeof(sb));
    g_assert_cmphex(get16le(sb + BCM_STATUS_RX_PROD_OFF), ==, 1);
    g_assert_cmphex(sb[BCM_STATUS_RX_PROD_OFF], ==, 0x01);
    g_assert_cmphex(sb[BCM_STATUS_RX_PROD_OFF + 1], ==, 0x00);

    /* And the frame reached the posted buffer byte-for-byte. */
    qtest_memread(b.qts, RX_BUF_PHYS, got, sizeof(frame));
    g_assert_cmpmem(got, sizeof(frame), frame, sizeof(frame));

    /* The driver's return-ring acknowledgement is accepted (write returns). */
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
                             " -m 16M"
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
                             " -m 16M"
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
    g_assert_cmphex(get16le(ret + BCM_RXBD_IDX), ==, 0);

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
                             " -m 16M"
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
    put32le(bd, UNMAPPED_PHYS >> 32);
    put32le(bd + 4, UNMAPPED_PHYS & 0xffffffff);
    qtest_memwrite(b.qts, STD_RING_PHYS, bd, sizeof(bd));
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);
    bar_writel(&b, BCM_MBX_IRQ0_LO, 0);
    tag = read_status_tag(&b);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_log_marker(b.qts, logfile,
                        "bcm570x: receive payload write at 0x2000000 failed");
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmpmem(ret, sizeof(ret), zero, sizeof(zero));
    g_assert_cmphex(read_status_tag(&b), ==, tag);
    g_assert_false(intr_line_up(b.dev));

    /* Repair only the buffer address; the same ring entry remains posted. */
    put32le(bd, RX_BUF_PHYS >> 32);
    put32le(bd + 4, RX_BUF_PHYS & 0xffffffff);
    qtest_memwrite(b.qts, STD_RING_PHYS, bd, sizeof(bd));
    qtest_memset(b.qts, RX_BUF_PHYS, 0, sizeof(landed));
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);
    wait_for_rx_prod(&b, 1);
    qtest_memread(b.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_IDX), ==, 0);
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
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
 * guest repairs what faulted -- a descriptor ring, a buffer address, the return
 * ring -- and the receive-ready write that follows retries it. A completion
 * whose interrupt the driver has not acknowledged yet does not hold it back:
 * the receive path is driven by posted buffers, not by the interrupt mailbox.
 */
static void test_receive_dma_error_after_progress(const void *data)
{
    RxDmaFault fault = GPOINTER_TO_INT(data);
    const char *fault_marker =
        fault == RX_DMA_FAULT_DESCRIPTOR ?
        "bcm570x: receive descriptor 1 read failed" :
        fault == RX_DMA_FAULT_PAYLOAD ?
        "bcm570x: receive payload write at 0x2000000 failed" :
        "bcm570x: receive completion 1 write failed";
    uint64_t std_ring = fault == RX_DMA_FAULT_DESCRIPTOR ?
                        RAM_END_16M - BCM_RXBD_SIZE : STD_RING_PHYS;
    uint64_t ret_ring = fault == RX_DMA_FAULT_COMPLETION ?
                        RAM_END_16M - BCM_RXBD_SIZE : RET_RING_PHYS;
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
                             " -m 16M"
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
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    net_socket_send_frame(sv[0], first, sizeof(first));
    wait_for_rx_prod(&b, 1);

    qtest_memread(b.qts, RX_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), first, sizeof(first));
    qtest_memread(b.qts, first_ret, ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_IDX), ==, 0);
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
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
    g_assert_cmphex(get16le(ret + BCM_RXBD_IDX), ==, 1);
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
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
 * Interrupt coalescing on receive. The host-coalescing block batches completed
 * receive descriptors into one status-block update and one interrupt instead of
 * one per frame: with RX_MAX_COAL_BDS set to K, K received frames raise a single
 * status-block DMA. The status tag, bumped once per status DMA, therefore
 * advances by N/K over N frames, not by N. This is the receive path's per-packet
 * interrupt cost; honouring the value the driver programs is both the fidelity
 * contract (the datasheet's host-coalescing registers) and the performance win.
 *
 * The coalescing timer is parked far out so the descriptor count, not the timer,
 * closes each group -- the qtest virtual clock does not advance on its own, so
 * the timer never fires here and the group boundary is deterministic.
 */
static void test_receive_interrupt_coalescing(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t bd[BCM_RXBD_SIZE];
    unsigned int i;
    const unsigned int coal = 4;     /* RX_MAX_COAL_BDS: frames per interrupt */
    const unsigned int frames = 16;  /* a whole number of coalescing groups */
    uint32_t tag_before, tag_after;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 7 + 1);
    }
    memset(frame, 0xff, 6);          /* broadcast destination, always accepted */

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    /* Standard-ring RCB in CSRs, return-ring RCB in SRAM, a ring long enough for
     * the whole burst, and the status block's host address. */
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

    /* Post COAL_RING_LEN worth of standard-ring buffers -- one scratch buffer is
     * enough, the payload is irrelevant here, only the interrupt count is. */
    memset(bd, 0, sizeof(bd));
    put32le(bd + 0, RX_BUF_PHYS >> 32);
    put32le(bd + 4, RX_BUF_PHYS & 0xffffffff);
    for (i = 0; i < frames; i++) {
        qtest_memwrite(b.qts, STD_RING_PHYS + (uint64_t)i * BCM_RXBD_SIZE,
                       bd, sizeof(bd));
    }
    qtest_memset(b.qts, RET_RING_PHYS, 0,
                 (uint64_t)COAL_RING_LEN * BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, frames);

    /* The one-shot bring-up status update fires on that first producer write;
     * sample the tag after it, so the delta below counts only the coalesced,
     * frame-driven updates. */
    tag_before = read_status_tag(&b);

    /* Send the whole burst, then wait for the receive producer to reach it. */
    for (i = 0; i < frames; i++) {
        net_socket_send_frame(sv[0], frame, sizeof(frame));
    }
    wait_for_rx_prod(&b, frames);

    tag_after = read_status_tag(&b);

    /* N frames coalesced K-at-a-time is exactly N/K status-block updates -- one
     * interrupt per group, not one per frame. */
    g_assert_cmpuint(tag_after - tag_before, ==, frames / coal);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * Coalescing's tick timer flushes a partial group. With RX_MAX_COAL_BDS above
 * the burst size the descriptor count never triggers, so a handful of frames
 * raise no interrupt until the RX_COAL_TICKS budget elapses, at which point the
 * timer flushes them in one update. This is the no-stall guarantee: the tail of
 * a burst is delivered on the tick, never left waiting for a frame that never
 * comes.
 */
static void test_receive_coalescing_timer(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint8_t bd[BCM_RXBD_SIZE];
    unsigned int i;
    const unsigned int coal = 8;     /* larger than the burst: count never fires */
    const unsigned int ticks = 50;   /* RX_COAL_TICKS, microseconds */
    const unsigned int frames = 3;   /* only the timer can flush these */
    uint32_t tag_before;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 3 + 9);
    }
    memset(frame, 0xff, 6);          /* broadcast destination, always accepted */

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
    put32le(bd + 0, RX_BUF_PHYS >> 32);
    put32le(bd + 4, RX_BUF_PHYS & 0xffffffff);
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
    /* Every frame lands in the return ring, but under coalescing none raises a
     * status update yet: the count is below the group size and no tick has
     * elapsed. */
    wait_for_ret_end(&b, frames - 1);
    g_assert_cmpuint(read_status_tag(&b) - tag_before, ==, 0);

    /* Advance past the tick budget; the timer flushes the held group in one
     * update. */
    qtest_clock_step(b.qts, (int64_t)ticks * 1000 + 1000);
    wait_for_rx_prod(&b, frames);
    g_assert_cmpuint(read_status_tag(&b) - tag_before, ==, 1);

    bcm_close(&b);
    close(sv[0]);
}

/*
 * Receive and transmit use the same zero-tick contract: a partial group is
 * posted immediately instead of waiting forever for a timer that was not
 * armed.
 */
static void test_receive_coalescing_zero_ticks(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];
    uint32_t before;

    memset(frame, 0x41, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));
    rx_open_one(&b, sv[0]);
    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, 8);
    bar_writel(&b, BCM_HCC_RX_COAL_TICKS, 0);

    before = read_status_tag(&b);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_ret_end(&b, 0);
    g_assert_cmpuint(read_status_tag(&b) - before, ==, 1);

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
                             " -m 16M"
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
    put32le(bd, RX_BUF_PHYS >> 32);
    put32le(bd + 4, RX_BUF_PHYS & 0xffffffff);
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
    g_assert_cmphex(get16le(status + BCM_STATUS_RX_PROD_OFF), ==, 0);
    g_assert_false(intr_line_up(b.dev));

    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    qtest_clock_step(b.qts, (int64_t)ticks * 1000 + 1000);
    qtest_memread(b.qts, STATUS_PHYS, status, sizeof(status));
    g_assert_cmphex(get16le(status + BCM_STATUS_RX_PROD_OFF), ==, 1);
    g_assert_cmphex(read_status_tag(&b), ==, 1);
    g_assert_true(intr_line_up(b.dev));

    bcm_close(&b);
    close(sv[0]);
}

/*
 * Rebuild only the status-DMA plumbing after a reset. The coalescing controls
 * stay at their reset values, so any update before the default one-second
 * periodic deadline came from work a reset failed to discard.
 */
static void bcm_rearm_status_after_reset(Bcm *b)
{
    qpci_device_enable(b->dev);
    ind_mem_writel(b, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    ind_reg_writel(b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    ind_reg_writel(b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
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
    bcm_rearm_status_after_reset(&b);
    qtest_clock_step(b.qts, (int64_t)ticks * 1000 + 1000);
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
    bcm_rearm_status_after_reset(&b);
    qtest_clock_step(b.qts, (int64_t)ticks * 1000 + 1000);
    g_assert_cmpuint(read_status_tag(&b), ==, 0);

    bcm_close(&b);
    close(sv[0]);
}

static void periodic_status_reset(bool system_reset)
{
    Bcm b;
    const unsigned int ticks = 50;

    bcm_open(&b, bcm_init());
    bcm_arm_status(&b);
    bar_writel(&b, BCM_HCC_STATS_TICKS, ticks);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);

    if (system_reset) {
        qtest_qmp_assert_success(b.qts, "{ 'execute': 'system_reset' }");
        qtest_qmp_eventwait(b.qts, "RESET");
    } else {
        bar_writel(&b, BCM_MISC_CFG, BCM_MISCCFG_RESET);
    }
    bcm_rearm_status_after_reset(&b);
    qtest_clock_step(b.qts, (int64_t)ticks * 1000 + 1000);
    g_assert_cmpuint(read_status_tag(&b), ==, 0);

    bcm_close(&b);
}

static void test_global_reset_cancels_periodic_status(const void *data)
{
    periodic_status_reset(false);
}

static void test_system_reset_cancels_periodic_status(const void *data)
{
    periodic_status_reset(true);
}

/*
 * Frame payload byte order on receive, the inverse of the transmit swap. With
 * ENDIAN_WORDSWAP set the chip lays the received frame into the buffer reversed
 * per 32-bit word, the layout the big-endian driver reverses back when it reads
 * it. Send a frame and confirm the buffer holds the word-reversed image.
 */
static void test_receive_frame_wordswap(const void *data)
{
    int sv[2];
    Bcm b;
    uint8_t frame[64];       /* what arrives on the wire */
    uint8_t swapped[64];     /* what the driver must find in the buffer */
    uint8_t bd[BCM_RXBD_SIZE];
    uint8_t got[64];
    uint32_t misc;
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 5 + 3);
    }
    memset(frame, 0xff, 6);     /* broadcast destination */
    for (i = 0; i < sizeof(frame); i += 4) {
        swapped[i + 0] = frame[i + 3];
        swapped[i + 1] = frame[i + 2];
        swapped[i + 2] = frame[i + 1];
        swapped[i + 3] = frame[i + 0];
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -netdev socket,fd=%d,id=n0"
                             " -device bcm570x,addr=%02x.0,netdev=n0",
                             sv[1], BCM_SLOT));

    /* ENDIAN_WORDSWAP gates the frame swap and the control-block address
     * composition alike, so the register-programmed host addresses are written
     * word-swapped: the low word into the HI slot, the high word into the LO
     * slot. The posted buffer's own address rides in the descriptor and is read
     * through the little-endian accessors, so it stays in order. */
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
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    memset(bd, 0, sizeof(bd));
    put32le(bd + 0, RX_BUF_PHYS >> 32);
    put32le(bd + 4, RX_BUF_PHYS & 0xffffffff);
    qtest_memwrite(b.qts, STD_RING_PHYS, bd, sizeof(bd));
    qtest_memset(b.qts, RX_BUF_PHYS, 0, sizeof(frame));
    qtest_memset(b.qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(b.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);

    bar_writel(&b, BCM_MBX_RX_STD_PROD_LO, 1);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_rx_prod(&b, 1);

    qtest_memread(b.qts, RX_BUF_PHYS, got, sizeof(frame));
    g_assert_cmpmem(got, sizeof(frame), swapped, sizeof(swapped));

    bcm_close(&b);
    close(sv[0]);
}

/*
 * A receive producer or consumer index past its ring must not hang the mailbox
 * write. The rings hold RX_RING_LEN descriptors, so an index at or beyond that
 * is out of range; each write logs a guest error and returns, proven by the
 * MMIO access after it.
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
 * MISC_CTL mask suppresses it. The chip drives the line as a level: it goes up
 * when a status-block update is posted with interrupts unmasked and down when
 * the driver acknowledges. The driver reads the line through
 * PCISTATE.INTR_NOT_ACTIVE, which the chip clears while the line is asserted.
 * Producer-0 doorbells stand in for real completions -- each drives a
 * status-block DMA with no ring walk, so the test needs no descriptors or frame.
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

    /* Write 1: masks and acknowledges. The line drops; INTR_NOT_ACTIVE reads. */
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

    /* The global mask in MISC_CTL suppresses the line while it is set, and the
     * line returns when it clears -- the pending update was never taken. */
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
    g_assert_cmphex(get32le(sb), ==, 0);
    g_assert_false(intr_line_up(b.dev));

    /* Enabled: the same doorbell now DMAs the status block, UPDATED set. */
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&b, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 0);
    qtest_memread(b.qts, STATUS_PHYS, sb, sizeof(sb));
    g_assert_cmphex(get32le(sb) & BCM_STATFLAG_UPDATED, ==, BCM_STATFLAG_UPDATED);

    bcm_close(&b);
}

/* A failed status write advances neither its tag nor the interrupt state. */
static void test_status_dma_error(const void *data)
{
    Bcm b;

    bcm_open(&b, qtest_initf(BCM_MACHINE
                             " -m 16M"
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

/* Drive a migration between two connected qtest instances and block until it
 * completes; `to` is already listening on `uri` via its -incoming. */
static void bcm570x_migrate(QTestState *from, const char *uri)
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
        g_assert(g_test_timer_elapsed() < 30);
        g_usleep(1000);
    }
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
 * nested state must therefore start at a new parent version: reusing version 6
 * makes old and new binaries assign different layouts to the same stream.
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
        const char *args[] = {
            qtest_qemu_binary(NULL), "-nodefaults", "-machine", "pc",
            "-device", "bcm570x", "-dump-vmstate", dump_path, NULL
        };

        g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
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
    g_assert_cmpint(qdict_get_int(section, "version_id"), ==, 9);
    description = qdict_get_qdict(section, "Description");
    g_assert_cmpint(qdict_get_int(description, "version_id"), ==, 9);
    fields = qdict_get_qlist(description, "Fields");
    phy = bcm570x_vmstate_field(fields, "phy");
    g_assert_nonnull(phy);
    g_assert_cmpint(qdict_get_int(phy, "version_id"), ==, 7);

    qobject_unref(root_obj);
    unlink(dump_path);
}

/* A version-6 standalone PHY section must load into the nested PHY state. */
static void test_v6_phy_state_migrates(const void *data)
{
    const uint16_t advertise = 0x5a5a;
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *source = NULL;
    g_autofree gchar *dest = NULL;
    GError *err = NULL;
    QTestState *from_qts, *to_qts;
    Bcm from, to;

    if (!g_getenv("QTEST_QEMU_BINARY_V6")) {
        g_test_skip("QTEST_QEMU_BINARY_V6 is not set");
        return;
    }

    tmpfs = g_dir_make_tmp("bcm570x-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    source = g_strdup_printf(BCM_MACHINE
                             " -device bcm570x,addr=%02x.0", BCM_SLOT);
    from_qts = qtest_init_ext("QTEST_QEMU_BINARY_V6", source, NULL, true);
    bcm_open(&from, from_qts);
    mii_write(&from, BCM_PHY_ADDR, MII_ANAR, advertise);
    g_assert_cmphex(mii_read(&from, BCM_PHY_ADDR, MII_ANAR), ==, advertise);

    dest = g_strdup_printf(BCM_MACHINE
                           " -device bcm570x,addr=%02x.0 -incoming %s",
                           BCM_SLOT, uri);
    to_qts = qtest_init(dest);
    bcm_open(&to, to_qts);
    bcm570x_migrate(from_qts, uri);
    g_assert_cmphex(mii_read(&to, BCM_PHY_ADDR, MII_ANAR), ==, advertise);

    bcm_close(&to);
    bcm_close(&from);
    g_rmdir(tmpfs);
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
    bcm570x_migrate(from.qts, uri);

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
 * A partial transmit group owns both a pending completion count and its
 * deadline. Migration must carry both so the destination reports the tail at
 * the same timer boundary instead of losing it.
 */
static void test_transmit_coalescing_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest = NULL;
    GError *err = NULL;
    QTestState *from_qts, *to_qts;
    Bcm from, to;
    int sv[2];
    uint8_t frame[64];
    uint8_t got[128];
    const unsigned int ticks = 50;

    tmpfs = g_dir_make_tmp("bcm570x-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    memset(frame, 0x56, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    from_qts = qtest_initf(BCM_MACHINE
                           " -netdev socket,fd=%d,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0",
                           sv[1], BCM_SLOT);
    bcm_open(&from, from_qts);
    txc_setup(&from, 8, ticks);
    txc_post(&from, frame, sizeof(frame), 1);
    g_assert_cmpint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==,
                    sizeof(frame));
    g_assert_cmpuint(read_status_tag(&from), ==, 0);

    dest = g_strdup_printf(BCM_MACHINE
                           " -netdev user,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0"
                           " -incoming %s", BCM_SLOT, uri);
    to_qts = qtest_init(dest);
    bcm_open(&to, to_qts);
    bcm570x_migrate(from_qts, uri);

    g_assert_cmpuint(read_status_tag(&to), ==, 0);
    qtest_clock_step(to.qts, (int64_t)ticks * 1000 + 1000);
    g_assert_cmpuint(read_status_tag(&to), ==, 1);

    bcm_close(&to);
    bcm_close(&from);
    close(sv[0]);
}

/*
 * The receive side uses the same migration contract. This pins the existing
 * pending-count field to a real timer expiry on the destination.
 */
static void test_receive_coalescing_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest = NULL;
    GError *err = NULL;
    QTestState *from_qts, *to_qts;
    Bcm from, to;
    int sv[2];
    uint8_t frame[64];
    const unsigned int ticks = 50;

    tmpfs = g_dir_make_tmp("bcm570x-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);
    memset(frame, 0x67, sizeof(frame));
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    from_qts = qtest_initf(BCM_MACHINE
                           " -netdev socket,fd=%d,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0",
                           sv[1], BCM_SLOT);
    bcm_open(&from, from_qts);
    rx_open_one(&from, sv[0]);
    bar_writel(&from, BCM_HCC_RX_MAX_COAL_BDS, 8);
    bar_writel(&from, BCM_HCC_RX_COAL_TICKS, ticks);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_ret_end(&from, 0);
    g_assert_cmpuint(read_status_tag(&from), ==, 0);

    dest = g_strdup_printf(BCM_MACHINE
                           " -netdev user,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0"
                           " -incoming %s", BCM_SLOT, uri);
    to_qts = qtest_init(dest);
    bcm_open(&to, to_qts);
    bcm570x_migrate(from_qts, uri);

    qtest_clock_step(to.qts, (int64_t)ticks * 1000 + 1000);
    g_assert_cmpuint(read_status_tag(&to), ==, 1);

    bcm_close(&to);
    bcm_close(&from);
    close(sv[0]);
}

/*
 * HCC periodic status is part of the live device state too. An idle guest must
 * keep receiving watchdog updates after migration without rewriting HCC.
 */
static void test_periodic_status_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest = NULL;
    GError *err = NULL;
    QTestState *from_qts, *to_qts;
    Bcm from, to;
    const unsigned int ticks = 50;

    tmpfs = g_dir_make_tmp("bcm570x-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from_qts = bcm_init();
    bcm_open(&from, from_qts);
    bcm_arm_status(&from);
    bar_writel(&from, BCM_HCC_STATS_TICKS, ticks);
    bar_writel(&from, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(&from, BCM_RX_MODE, BCM_RXMODE_ENABLE);
    qtest_memset(from.qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);

    dest = g_strdup_printf(BCM_MACHINE
                           " -netdev user,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0"
                           " -incoming %s", BCM_SLOT, uri);
    to_qts = qtest_init(dest);
    bcm_open(&to, to_qts);
    bcm570x_migrate(from_qts, uri);

    g_assert_cmpuint(read_status_tag(&to), ==, 0);
    qtest_clock_step(to.qts, (int64_t)ticks * 1000 + 1000);
    g_assert_cmpuint(read_status_tag(&to), ==, 1);

    bcm_close(&to);
    bcm_close(&from);
}

/*
 * The device's state round-trips across a migration. On the source, a
 * register-file word (the status-block host address), an SRAM word, and an
 * asserted interrupt are established, then the machine is migrated stopped. On
 * the destination the register and SRAM words read back, and the interrupt is
 * still live: toggling the global mask drops and re-raises the line with no new
 * update, which holds only if the pending latch and the mailbox mask both
 * crossed the migration.
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

    /* Enable interrupts and leave the line asserted at the migration point. */
    bar_writel(&from, BCM_MBX_IRQ0_LO, 0);
    bar_writel(&from, BCM_MBX_TX_PROD_LO, 0);
    g_assert_true(intr_line_up(from.dev));

    dest = g_strdup_printf(BCM_MACHINE
                           " -netdev user,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0,"
                           "mac=52:54:00:11:22:33 -incoming %s", BCM_SLOT, uri);
    to_qts = qtest_init(dest);
    bcm_open(&to, to_qts);
    bcm570x_migrate(from_qts, uri);

    /* The register file (status-block host address) and SRAM word came across. */
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
 * A write-one mailbox acknowledgement can precede the driver's matching unmask.
 * The mask itself has to survive migration: a frame the destination places goes
 * on raising no line until the driver writes zero. Receiving is not part of
 * that handshake -- the posted buffer releases the frame either way.
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
    put32le(bd, RX_BUF_PHYS >> 32);
    put32le(bd + 4, RX_BUF_PHYS & 0xffffffff);
    qtest_memwrite(from.qts, STD_RING_PHYS, bd, sizeof(bd));
    qtest_memset(from.qts, RET_RING_PHYS, 0, sizeof(ret));
    qtest_memset(from.qts, RX_BUF_PHYS, 0, sizeof(frame));
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
    bcm570x_migrate(from.qts, uri);

    net_socket_send_frame(dest_sv[0], frame, sizeof(frame));
    g_usleep(100000);
    qtest_memread(to.qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16le(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==, 0);

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
 * passes the range guard, program the status block's host page, and enable the
 * host-coalescing block. No BAR mapping is needed -- the indirect windows reach
 * each function's register file and SRAM, so the two functions are configured
 * independently at one slot.
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
 * status block. Driving one function's transmit doorbell moves that function's
 * status block and leaves the other's untouched.
 */
static void test_dual_port_present_and_independent(const void *data)
{
    QTestState *qts = qtest_initf(BCM_MACHINE
                                  " -netdev user,id=n0"
                                  " -device bcm570x,model=bcm5704,addr=%02x.0,"
                                  "netdev=n0", BCM_SLOT);
    QPCIBus *bus = qpci_new_pc(qts, NULL);
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

    /* Distinct MAC identity per function: both mailboxes carry the 0x484B tag,
     * and the address bytes differ (each function took its own default MAC). */
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

    /* Drive function 0's doorbell only: its status block updates, function 1's
     * stays zero -- the engines are independent. */
    ind_reg_writel_dev(fn0, BCM_MBX_TX_PROD_LO, 0);
    qtest_memread(qts, STATUS_PHYS, sb0, sizeof(sb0));
    qtest_memread(qts, STATUS2_PHYS, sb1, sizeof(sb1));
    g_assert_cmphex(get32le(sb0) & BCM_STATFLAG_UPDATED, ==, BCM_STATFLAG_UPDATED);
    g_assert_cmphex(get32le(sb1), ==, 0);

    /* And driving function 1 moves its own status block -- both work, apart. */
    ind_reg_writel_dev(fn1, BCM_MBX_TX_PROD_LO, 0);
    qtest_memread(qts, STATUS2_PHYS, sb1, sizeof(sb1));
    g_assert_cmphex(get32le(sb1) & BCM_STATFLAG_UPDATED, ==, BCM_STATFLAG_UPDATED);

    qpci_iounmap(fn0, bars[0]);
    qpci_iounmap(fn1, bars[1]);
    g_free(fn0);
    g_free(fn1);
    qpci_free_pc(bus);
    qtest_quit(qts);
}

/*
 * Both functions' per-function state migrates independently. Each function is a
 * full PCI device with its own VMStateDescription at its own devfn, so no
 * VMState change was needed for the dual-port device; this proves it by writing
 * a distinct SRAM word to each function, migrating stopped, and reading both
 * back on the destination.
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
    from_bus = qpci_new_pc(from, NULL);
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
    to_bus = qpci_new_pc(to, NULL);
    to0 = qpci_device_find(to_bus, QPCI_DEVFN(BCM_SLOT, 0));
    to1 = qpci_device_find(to_bus, QPCI_DEVFN(BCM_SLOT, 1));
    g_assert_nonnull(to0);
    g_assert_nonnull(to1);
    bcm570x_migrate(from, uri);

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
    qpci_free_pc(from_bus);
    qpci_free_pc(to_bus);
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
    qtest_add_data_func("/bcm570x/transmit-throughput", NULL,
                        test_transmit_throughput);
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
    qtest_add_data_func("/bcm570x/receive-coalescing-cost", NULL,
                        test_receive_coalescing_cost);
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
    qtest_add_data_func("/bcm570x/queued-receive-hot-unplug", NULL,
                        test_queued_receive_hot_unplug);
    qtest_add_data_func("/bcm570x/hot-unplug-exits-cleanly", NULL,
                        test_hot_unplug_exits_cleanly);
    qtest_add_data_func("/bcm570x/failed-realize-releases-backend", NULL,
                        test_failed_realize_releases_backend);
    qtest_add_data_func("/bcm570x/receive-checksum-offload", NULL,
                        test_receive_checksum_offload);
    qtest_add_data_func("/bcm570x/receive-checksum-not-ip", NULL,
                        test_receive_checksum_not_ip);
    qtest_add_data_func("/bcm570x/receive-overruns-nothing", NULL,
                        test_receive_overruns_nothing);
    qtest_add_data_func("/bcm570x/receive-jumbo-frame", NULL,
                        test_receive_jumbo_frame);
    qtest_add_data_func("/bcm570x/receive-waits-for-standard-ring", NULL,
                        test_receive_waits_for_standard_ring);
    qtest_add_data_func("/bcm570x/receive-waits-for-jumbo-ring", NULL,
                        test_receive_waits_for_jumbo_ring);
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
    qtest_add_data_func("/bcm570x/receive-coalescing-status-dma-retry", NULL,
                        test_receive_coalescing_status_dma_retry);
    qtest_add_data_func("/bcm570x/global-reset-cancels-rx-coalescing", NULL,
                        test_global_reset_cancels_receive_coalescing);
    qtest_add_data_func("/bcm570x/system-reset-cancels-tx-coalescing", NULL,
                        test_system_reset_cancels_transmit_coalescing);
    qtest_add_data_func("/bcm570x/global-reset-cancels-periodic-status", NULL,
                        test_global_reset_cancels_periodic_status);
    qtest_add_data_func("/bcm570x/system-reset-cancels-periodic-status", NULL,
                        test_system_reset_cancels_periodic_status);
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
    qtest_add_data_func("/bcm570x/v6-phy-state-migrates", NULL,
                        test_v6_phy_state_migrates);
    qtest_add_data_func("/bcm570x/state-survives-migration", NULL,
                        test_state_survives_migration);
    qtest_add_data_func(
        "/bcm570x/masked-interrupt-survives-migration", NULL,
        test_masked_interrupt_survives_migration);
    qtest_add_data_func("/bcm570x/migration/link-down", GINT_TO_POINTER(0),
                        test_migrated_link_controls_egress);
    qtest_add_data_func("/bcm570x/migration/link-up", GINT_TO_POINTER(1),
                        test_migrated_link_controls_egress);
    qtest_add_data_func("/bcm570x/transmit-coalescing-survives-migration", NULL,
                        test_transmit_coalescing_survives_migration);
    qtest_add_data_func("/bcm570x/receive-coalescing-survives-migration", NULL,
                        test_receive_coalescing_survives_migration);
    qtest_add_data_func("/bcm570x/periodic-status-survives-migration", NULL,
                        test_periodic_status_survives_migration);
    qtest_add_data_func("/bcm570x/dual-port-present-and-independent", NULL,
                        test_dual_port_present_and_independent);
    qtest_add_data_func("/bcm570x/dual-port-survives-migration", NULL,
                        test_dual_port_survives_migration);

    return g_test_run();
}
