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
 * separate machine-level concern, validated by the IRIX boot, not by a qtest
 * (see docs/library/nic-on-bridge-conventions.md).
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
#include "libqtest.h"
#include "libqos/pci-pc.h"
#include "qobject/qdict.h"
#include "hw/pci/pci_regs.h"
#include <sys/socket.h>
#include <netinet/in.h>

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
#define BCM_HCC_RX_COAL_TICKS   0x3c08u        /* receive coalescing: microseconds */
#define BCM_HCC_RX_MAX_COAL_BDS 0x3c10u        /* receive coalescing: descriptors */
#define BCM_MODE_CTL            0x6800u
#define BCM_DMA_SWAP_OPTIONS    0x36u          /* the driver's MODE_CTL swap arm */
#define BCM_SRAM_SEND_RING_RCB  0x0100u
#define BCM_TXBDFLAG_END        0x0004u
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
#define BCM_RXBD_SIZE           32
#define BCM_RXBD_LEN            0x08   /* idx_len word: length in the low half */
#define BCM_RXBD_IDX            0x0a   /* idx_len word: ring index in the high half */
#define BCM_RXBD_FLAGS          0x0c   /* type_flags word: flags in the low half */
#define BCM_RXBDFLAG_END        0x0004u
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
#define COAL_RING_LEN         64u            /* a ring long enough for a coalescing burst */

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

static void bar_writel(Bcm *b, uint32_t off, uint32_t val)
{
    qpci_io_writel(b->dev, b->bar, off, val);
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

static void net_socket_send_frame(int fd, const uint8_t *buf, size_t len)
{
    uint32_t hdr = htonl((uint32_t)len);

    g_assert_cmpint(send(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    g_assert_cmpint(send(fd, buf, len, 0), ==, (int)len);
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
    Bcm b;
    QTestState *qts = qtest_initf(BCM_MACHINE
                                  " -netdev user,id=n0"
                                  " -device bcm570x,addr=%02x.0,netdev=n0,"
                                  "mac=52:54:00:ab:cd:ef", BCM_SLOT);

    bcm_open(&b, qts);

    /* MAC_ADDR_0_HIGH: the first two address bytes, in the low half. */
    g_assert_cmphex(bar_readl(&b, 0x0410), ==, 0x00005254u);
    /* MAC_ADDR_0_LOW: the remaining four address bytes. */
    g_assert_cmphex(bar_readl(&b, 0x0414), ==, 0x00abcdefu);

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
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, RX_RING_LEN << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4, RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);

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
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, COAL_RING_LEN << 16);
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
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, COAL_RING_LEN << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4, RET_RING_PHYS & 0xffffffff);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, COAL_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS & 0xffffffff);
    bar_writel(&b, BCM_HCC_RX_MAX_COAL_BDS, coal);
    bar_writel(&b, BCM_HCC_RX_COAL_TICKS, ticks);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);

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
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, RX_RING_LEN << 16);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 0, (uint32_t)RET_RING_PHYS);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 4, RET_RING_PHYS >> 32);
    ind_mem_writel(&b, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);
    bar_writel(&b, BCM_HCC_STATUSBLK_HI, (uint32_t)STATUS_PHYS);
    bar_writel(&b, BCM_HCC_STATUSBLK_LO, STATUS_PHYS >> 32);
    bar_writel(&b, BCM_HCC_MODE, BCM_HCC_ENABLE);

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
    bar_writel(&b, BCM_RX_STD_RCB_MAXLEN, RX_RING_LEN << 16);
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

    /* Write 0 re-enables; the acknowledge cleared the latch, so the line stays
     * down until a fresh update re-asserts it. */
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
    bar_writel(&b, BCM_MBX_TX_PROD_LO, 0);
    qtest_memread(b.qts, STATUS_PHYS, sb, sizeof(sb));
    g_assert_cmphex(get32le(sb) & BCM_STATFLAG_UPDATED, ==, BCM_STATFLAG_UPDATED);

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

    /* An SRAM word reset does not touch, so its survival is unambiguous. */
    ind_mem_writel(&from, 0x0c1c, sram_sentinel);

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

    /* The interrupt config came across, line still asserted. */
    g_assert_true(intr_line_up(to.dev));

    /* The migrated latch and mask are live: mask then unmask the global mask
     * and the line follows, without any fresh update to re-establish it. */
    misc = qpci_config_readl(to.dev, BCM_PCI_MISC_CTL);
    qpci_config_writel(to.dev, BCM_PCI_MISC_CTL, misc | BCM_MISCCTL_MASK_INTR);
    g_assert_false(intr_line_up(to.dev));
    qpci_config_writel(to.dev, BCM_PCI_MISC_CTL, misc);
    g_assert_true(intr_line_up(to.dev));

    bcm_close(&to);
    bcm_close(&from);
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
    ind_mem_writel_dev(from0, 0x0c1c, 0xaaaa0000);
    ind_mem_writel_dev(from1, 0x0c1c, 0xbbbb1111);

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

    g_assert_cmphex(ind_mem_readl_dev(to0, 0x0c1c), ==, 0xaaaa0000);
    g_assert_cmphex(ind_mem_readl_dev(to1, 0x0c1c), ==, 0xbbbb1111);

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
    qtest_add_data_func("/bcm570x/phy-aux-status-link-up", NULL,
                        test_phy_aux_status_link_up);
    qtest_add_data_func("/bcm570x/status-block-byte-order", NULL,
                        test_status_block_byte_order);
    qtest_add_data_func("/bcm570x/status-block-hostaddr-wordswap", NULL,
                        test_status_block_hostaddr_wordswap);
    qtest_add_data_func("/bcm570x/transmit-round-trip", NULL,
                        test_transmit_round_trip);
    qtest_add_data_func("/bcm570x/transmit-frame-wordswap", NULL,
                        test_transmit_frame_wordswap);
    qtest_add_data_func("/bcm570x/tx-producer-out-of-range", NULL,
                        test_tx_producer_out_of_range);
    qtest_add_data_func("/bcm570x/receive-round-trip", NULL,
                        test_receive_round_trip);
    qtest_add_data_func("/bcm570x/receive-frame-wordswap", NULL,
                        test_receive_frame_wordswap);
    qtest_add_data_func("/bcm570x/receive-interrupt-coalescing", NULL,
                        test_receive_interrupt_coalescing);
    qtest_add_data_func("/bcm570x/receive-coalescing-timer", NULL,
                        test_receive_coalescing_timer);
    qtest_add_data_func("/bcm570x/rx-producer-out-of-range", NULL,
                        test_rx_producer_out_of_range);
    qtest_add_data_func("/bcm570x/interrupt-mailbox", NULL,
                        test_interrupt_mailbox);
    qtest_add_data_func("/bcm570x/status-block-gated-by-hcc", NULL,
                        test_status_block_gated_by_hcc);
    qtest_add_data_func("/bcm570x/state-survives-migration", NULL,
                        test_state_survives_migration);
    qtest_add_data_func("/bcm570x/dual-port-present-and-independent", NULL,
                        test_dual_port_present_and_independent);
    qtest_add_data_func("/bcm570x/dual-port-survives-migration", NULL,
                        test_dual_port_survives_migration);

    return g_test_run();
}
