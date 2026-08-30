/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the Broadcom NetXtreme BCM570x adapter behind the Origin 200
 * Bridge.
 *
 * The device is a generic PCI NIC that the SGI IRIS Release 2 Gigabit board
 * instantiates with a particular PCI four-tuple. IRIX's tg driver registers on
 * the raw Broadcom primary vendor/device and then, at attach, insists the card
 * also carry SGI's subsystem identity; a card with the Broadcom IDs but the
 * wrong or absent SGI subsystem half passes the primary filter and is rejected
 * only at attach, a failure a boot cannot show cheaply. So this test asserts the
 * whole four-tuple, plus the two indirect config-space windows the driver uses
 * before it ever touches the BAR (docs/datasheets/bcm570x.md, "three access
 * paths" and the qtest checklist).
 *
 * The card is placed at a free slot behind the origin200 Bridge, the same way
 * tests/qtest/sn0-bridge-test.c and tests/qtest/sn0-qlogic-isp-test.c reach an
 * add-in card: config space through the Bridge's type-0 window, and the device
 * BAR through a per-slot memory window the test aims by hand. Byte order across
 * the Bridge is settled by the transmit round-trip qtest in a later task, not
 * here; every access in this file is a 32-bit dword, for which the Bridge's
 * byte-lane address correction is the identity, so nothing here depends on the
 * unsettled register endianness.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "hw/pci/pci_regs.h"
#include <sys/socket.h>
#include <netinet/in.h>

/* Crosstalk space and the Bridge's own widget window inside it. */
#define SPACE_XTALK             0x20000000000ULL
#define BRIDGE_WIDGET           8
#define BRIDGE_BASE             (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

#define MACHINE_WITH_BRIDGE     "-M origin200,io-bridge=on"

/*
 * PCI type-0 configuration space through the Bridge, one 4 KB stride per
 * device and 0x100 per function, exactly as sn0-bridge-test.c addresses it.
 */
#define BRIDGE_CFG0             0x020000
#define CFG0_DEVICE_STRIDE      0x1000
#define CFG0_FUNCTION_STRIDE    0x100
#define CFG0(device, function)  (BRIDGE_BASE + BRIDGE_CFG0 + \
                                 (device) * CFG0_DEVICE_STRIDE + \
                                 (function) * CFG0_FUNCTION_STRIDE)

/*
 * The Bridge's own per-slot device register, used to aim a slot's memory
 * window: REG() reaches the 32-bit datum in the low half of the eight-byte
 * slot, and DEV_MEMORY_SPACE plus the window base (bits 31:20 of the card's
 * PCI address) point the window at the card.
 */
#define REG_DATA                4
#define REG(offset)             (BRIDGE_BASE + (offset) + REG_DATA)
#define BRIDGE_DEVICE(n)        (0x200 + 8 * (n))
#define DEV_MEMORY_SPACE        (1u << 12)

/*
 * Where each slot's memory window sits in the widget's address space. The
 * first two slots get 2 MB windows and the rest 1 MB, laid end to end from
 * 0x200000 -- written out rather than recomputed from the model's own rule.
 */
static const uint64_t window_offset[8] = {
    0x200000, 0x400000, 0x600000, 0x700000,
    0x800000, 0x900000, 0xa00000, 0xb00000,
};
#define WINDOW(slot)            (BRIDGE_BASE + window_offset[slot])

/* Where the card's BAR0 is placed in PCI memory space; 16 MB per slot. */
#define CARD_MMIO(slot)         (0x10000000u + (slot) * 0x1000000u)

/*
 * Base I/O board devices sit at 0, 1 (SCSI) and 2 (IOC3); 3..7 are free. The
 * card goes at slot 4, matching sn0-bridge-test.c's own probe slot.
 */
#define BCM_SLOT                4

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

/* Register-file offsets and bits Task 2 exercises. */
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

/* The MII management registers and the PHY-access command bits (Task 3). */
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

/* Transmit ring, status block, and DMA plumbing (Task 4). */
#define BCM_MBX_TX_PROD_LO      0x0304u
#define BCM_HCC_MODE            0x3c00u
#define BCM_HCC_STATUSBLK_HI    0x3c38u
#define BCM_HCC_STATUSBLK_LO    0x3c3cu
#define BCM_MODE_CTL            0x6800u
#define BCM_DMA_SWAP_OPTIONS    0x36u          /* the big-endian MODE_CTL arm */
#define BCM_SRAM_SEND_RING_RCB  0x0100u
#define BCM_TXBDFLAG_END        0x0004u
#define BCM_STATFLAG_UPDATED    0x00000001u
#define BCM_STATUS_BLK_SIZE     0x50
#define BCM_STATUS_TX_CONS_OFF  16
#define BCM_STATUS_RX_PROD_OFF  18

/* Receive ring (Task 5). */
#define BCM_MBX_RX_STD_PROD_LO  0x026cu
#define BCM_MBX_RX_CONS0_LO     0x0284u
#define BCM_RX_STD_RCB_HI       0x2450u
#define BCM_RX_STD_RCB_LO       0x2454u
#define BCM_RX_STD_RCB_MAXLEN   0x2458u
#define BCM_SRAM_RX_RETURN_RCB  0x0200u
#define BCM_RXBD_SIZE           32
#define BCM_RXBD_IDX            0x08
#define BCM_RXBD_LEN            0x0a
#define BCM_RXBD_FLAGS          0x0e
#define BCM_RXBDFLAG_END        0x0004u

/* Interrupts (Task 6). */
#define BCM_MBX_IRQ0_LO         0x0204u
#define BCM_PCI_PCISTATE        0x70
#define BCM_PCISTATE_INTR_NA    0x00000002u   /* INTR_NOT_ACTIVE */
#define BCM_MISCCTL_MASK_INTR   0x00000002u   /* MISC_CTL.MASK_PCI_INTR */

/*
 * The Bridge's own interrupt status register lives at 0x100 in its widget
 * window, and its low eight bits report each slot's pin live -- it is composed,
 * never latched, so it follows the card's INTA level. The card's pin follows
 * sgi_bridge_map_irq(): for INTA (an even pin) the bit is the slot number, so
 * slot BCM_SLOT lands on bit BCM_SLOT. The same address sn0-ioc3-test.c reads
 * it at, and like that test we never program the Bridge interrupt enable -- the
 * status register reflects the level regardless, and leaving the enable clear
 * keeps the Bridge from trying to send a packet to an unprogrammed destination.
 */
#define BRIDGE_INT_STATUS       REG(0x100)
#define BCM_INT_PIN             (1u << BCM_SLOT)

/* Dual-port BCM5704 (Task 7): copper four-tuple and the multifunction bit. */
#define BCM_DEVICE_ID_BCM5704   0x1648u
#define BCM_SUBSYS_ID_BCM5704   0x8013u
#define BCM_HEADER_MULTIFUNC    0x80u          /* PCI_HEADER_TYPE multi bit */
/* A second status-block host page, so function 1 DMAs somewhere function 0
 * does not, proving the two engines are independent. */
#define STATUS2_PHYS            0x00260000u
#define STATUS2_IOVA            (DMA_PREFIX | STATUS2_PHYS)

/* The Bridge's per-slot DMA swapper (direct window) and the direct map. */
#define DEV_SWAP_DIRECT         (1u << 19)
#define BRIDGE_DIRECT_MAP_REG   REG(0x080)

/*
 * Guest-physical DMA scratch and the 64-bit IOVAs the device is handed for it.
 * The Bridge's direct map (programmed to 0) keeps the low bits, so an IOVA with
 * this widget prefix lands at the matching physical address, the same way
 * sn0-qlogic-isp-test.c reaches guest memory. The scratch sits well above the
 * first megabyte, in RAM nothing else uses under qtest.
 */
#define DMA_PREFIX              0xa500000000000000ULL
#define TX_RING_PHYS            0x00200000u
#define TX_BUF_PHYS             0x00210000u
#define STATUS_PHYS             0x00220000u
#define TX_RING_IOVA            (DMA_PREFIX | TX_RING_PHYS)
#define TX_BUF_IOVA             (DMA_PREFIX | TX_BUF_PHYS)
#define STATUS_IOVA             (DMA_PREFIX | STATUS_PHYS)
#define TX_RING_LEN             4u             /* descriptors in the test ring */
#define STD_RING_PHYS           0x00230000u
#define RET_RING_PHYS           0x00240000u
#define RX_BUF_PHYS             0x00250000u
#define STD_RING_IOVA           (DMA_PREFIX | STD_RING_PHYS)
#define RET_RING_IOVA           (DMA_PREFIX | RET_RING_PHYS)
#define RX_BUF_IOVA             (DMA_PREFIX | RX_BUF_PHYS)
#define RX_RING_LEN             4u

/*
 * The Bridge corrects a narrow (1- or 2-byte) device access by moving only the
 * low two address bits, `off ^ (4 - size)`, the same formula OpenBSD's
 * xbridge(4) and Linux's swizzle both compute. A 16-bit access at natural
 * offset `n` therefore lands at `(n & ~3) | ((n & 3) ^ 2)`. A 4-byte access is
 * unaffected (`^ 0`), which is why the dword reads below need no correction.
 */
#define GUEST_HALFWORD(n)       (((n) & ~3u) | (((n) & 3u) ^ 2u))

static QTestState *bcm570x_start(void)
{
    /*
     * An explicit netdev, referenced by id, so the card's frontend has a
     * backend without competing for the machine's default network device the
     * onboard IOC3 already claims.
     */
    return qtest_initf(MACHINE_WITH_BRIDGE
                       " -netdev user,id=n0"
                       " -device bcm570x,addr=%02x.0,netdev=n0", BCM_SLOT);
}

/* Aim the card's slot window at its BAR0, so the BAR is reachable. */
static void bcm570x_aim_window(QTestState *qts)
{
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + PCI_BASE_ADDRESS_0, CARD_MMIO(BCM_SLOT));
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, REG(BRIDGE_DEVICE(BCM_SLOT)),
                 DEV_MEMORY_SPACE | (CARD_MMIO(BCM_SLOT) >> 20));
}

static uint32_t bar_readl(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, WINDOW(BCM_SLOT) + off);
}

static void bar_writel(QTestState *qts, uint32_t off, uint32_t val)
{
    qtest_writel(qts, WINDOW(BCM_SLOT) + off, val);
}

/* The indirect register window (config 0x78/0x80) into the register file. */
static void ind_reg_writel(QTestState *qts, uint32_t off, uint32_t val)
{
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_REG_BASEADDR, off);
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_REG_DATA, val);
}

/* Read a register-file word back through the indirect window (no BAR needed). */
static uint32_t ind_reg_readl(QTestState *qts, uint32_t off)
{
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_REG_BASEADDR, off);
    return qtest_readl(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_REG_DATA);
}

/* The indirect memory window (config 0x7C/0x84) into on-chip SRAM. */
static void ind_mem_writel(QTestState *qts, uint32_t off, uint32_t val)
{
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MEMWIN_BASEADDR, off);
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MEMWIN_DATA, val);
}

static uint32_t ind_mem_readl(QTestState *qts, uint32_t off)
{
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MEMWIN_BASEADDR, off);
    return qtest_readl(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MEMWIN_DATA);
}

/*
 * The same indirect windows but addressed at a chosen PCI function, so the
 * dual-port test reaches each BCM5704 MAC function's own register file and SRAM
 * through its own config space -- no BAR mapping needed.
 */
static void ind_reg_writel_fn(QTestState *qts, unsigned fn, uint32_t off,
                              uint32_t val)
{
    qtest_writel(qts, CFG0(BCM_SLOT, fn) + BCM_PCI_REG_BASEADDR, off);
    qtest_writel(qts, CFG0(BCM_SLOT, fn) + BCM_PCI_REG_DATA, val);
}

static uint32_t ind_mem_readl_fn(QTestState *qts, unsigned fn, uint32_t off)
{
    qtest_writel(qts, CFG0(BCM_SLOT, fn) + BCM_PCI_MEMWIN_BASEADDR, off);
    return qtest_readl(qts, CFG0(BCM_SLOT, fn) + BCM_PCI_MEMWIN_DATA);
}

static void ind_mem_writel_fn(QTestState *qts, unsigned fn, uint32_t off,
                              uint32_t val)
{
    qtest_writel(qts, CFG0(BCM_SLOT, fn) + BCM_PCI_MEMWIN_BASEADDR, off);
    qtest_writel(qts, CFG0(BCM_SLOT, fn) + BCM_PCI_MEMWIN_DATA, val);
}

/* A PHY register read/write through the MAC's MI_COMM register (needs BAR0). */
static uint16_t mii_read(QTestState *qts, unsigned phy, unsigned reg)
{
    bar_writel(qts, BCM_MI_COMM,
               BCM_MICOMM_BUSY | BCM_MICMD_READ |
               (phy << BCM_MICOMM_PHY_SHIFT) | (reg << BCM_MICOMM_REG_SHIFT));
    return bar_readl(qts, BCM_MI_COMM) & 0xffff;
}

static void mii_write(QTestState *qts, unsigned phy, unsigned reg, uint16_t val)
{
    bar_writel(qts, BCM_MI_COMM,
               BCM_MICOMM_BUSY | BCM_MICMD_WRITE |
               (phy << BCM_MICOMM_PHY_SHIFT) | (reg << BCM_MICOMM_REG_SHIFT) |
               val);
}

/* Big-endian field access, since the device's DMA is defined big-endian. */
static void put32be(uint8_t *b, uint32_t v)
{
    b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v;
}

static void put16be(uint8_t *b, uint16_t v)
{
    b[0] = v >> 8; b[1] = v;
}

static uint32_t get32be(const uint8_t *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

static uint16_t get16be(const uint8_t *b)
{
    return (b[0] << 8) | b[1];
}

/*
 * Enable bus mastering, aim the slot window, and set the slot's DMA swapper so
 * the device delivers big-endian and the Bridge is a no-op (DEV_SWAP set). The
 * direct map keeps the low IOVA bits, so the device's DMA reaches guest memory.
 */
static void bcm570x_setup_dma(QTestState *qts)
{
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + PCI_BASE_ADDRESS_0,
                 CARD_MMIO(BCM_SLOT));
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, REG(BRIDGE_DEVICE(BCM_SLOT)),
                 DEV_MEMORY_SPACE | DEV_SWAP_DIRECT |
                 (CARD_MMIO(BCM_SLOT) >> 20));
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);
}

/*
 * The minimum plumbing for the chip to DMA a status block: bus mastering and
 * the slot's DMA path, a send-ring control block with a ring length so a
 * doorbell passes the range guard, and the status block's host address. The
 * host-coalescing block is left disabled -- each test enables it when it wants
 * a status update -- and the status region is zeroed. A doorbell of producer 0
 * then drives a status-block DMA with no ring walk, which is all the interrupt
 * and gating tests need (no descriptors, no frame).
 */
static void bcm570x_arm_status(QTestState *qts)
{
    bcm570x_setup_dma(qts);
    ind_mem_writel(qts, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    bar_writel(qts, BCM_HCC_STATUSBLK_HI, STATUS_IOVA >> 32);
    bar_writel(qts, BCM_HCC_STATUSBLK_LO, STATUS_IOVA & 0xffffffff);
    qtest_memset(qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
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
static void wait_for_rx_prod(QTestState *qts, uint16_t want)
{
    uint8_t idx[2];
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        qtest_memread(qts, STATUS_PHYS + BCM_STATUS_RX_PROD_OFF, idx, 2);
        if (get16be(idx) == want) {
            return;
        }
        g_usleep(5000);
    }
    qtest_memread(qts, STATUS_PHYS + BCM_STATUS_RX_PROD_OFF, idx, 2);
    g_assert_cmphex(get16be(idx), ==, want);
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
 * board id 0x8010 at 0x2E. The datasheet table/checklist need the same
 * correction.
 */
static void test_pci_four_tuple(const void *data)
{
    QTestState *qts = bcm570x_start();

    /*
     * Dword reads first: a 4-byte config read crosses the Bridge with no
     * byte-lane correction, so vendor sits in the low half and device in the
     * high half, exactly as sn0-bridge-test.c reads a probe card's identity.
     */
    g_assert_cmphex(qtest_readl(qts, CFG0(BCM_SLOT, 0) + PCI_VENDOR_ID), ==,
                    BCM_VENDOR_ID | ((uint32_t)BCM_DEVICE_ID << 16));
    g_assert_cmphex(qtest_readl(qts, CFG0(BCM_SLOT, 0) + PCI_SUBSYSTEM_VENDOR_ID),
                    ==, BCM_SUBSYS_VENDOR_ID | ((uint32_t)BCM_SUBSYS_ID << 16));

    /*
     * And each field on its own as a corrected halfword, so a hidden swap in
     * one half cannot hide behind the other. The vendor at natural offset 0 is
     * read at guest offset 2, and so on.
     */
    g_assert_cmphex(qtest_readw(qts, CFG0(BCM_SLOT, 0) + GUEST_HALFWORD(0x00)),
                    ==, BCM_VENDOR_ID);
    g_assert_cmphex(qtest_readw(qts, CFG0(BCM_SLOT, 0) + GUEST_HALFWORD(0x02)),
                    ==, BCM_DEVICE_ID);
    g_assert_cmphex(qtest_readw(qts, CFG0(BCM_SLOT, 0) + GUEST_HALFWORD(0x2c)),
                    ==, BCM_SUBSYS_VENDOR_ID);
    g_assert_cmphex(qtest_readw(qts, CFG0(BCM_SLOT, 0) + GUEST_HALFWORD(0x2e)),
                    ==, BCM_SUBSYS_ID);

    qtest_quit(qts);
}

/*
 * The indirect register window round-trips. Writing a register offset to config
 * 0x78 and a value to config 0x80 must reach the same register file the BAR
 * exposes at 0x0000-0x7FFF: read the register back through BAR0 at that offset
 * and it must match, proving 0x78/0x80 alias the file.
 */
static void test_indirect_register_window(const void *data)
{
    QTestState *qts = bcm570x_start();
    const uint32_t reg_off = 0x0410;    /* in-range, dword-aligned */
    const uint32_t value = 0x12345678;

    bcm570x_aim_window(qts);

    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_REG_BASEADDR, reg_off);
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_REG_DATA, value);

    /* Through the BAR at the same offset: the aliasing proof. */
    g_assert_cmphex(bar_readl(qts, reg_off), ==, value);

    /* And back through the indirect window itself. */
    g_assert_cmphex(qtest_readl(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_REG_DATA), ==,
                    value);

    qtest_quit(qts);
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
    QTestState *qts = bcm570x_start();
    const uint32_t sram_off = 0x0c14;   /* the MAC-address mailbox, aligned */
    const uint32_t value = 0xdeadbeef;

    bcm570x_aim_window(qts);

    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MEMWIN_BASEADDR, sram_off);
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MEMWIN_DATA, value);

    /* Back through the indirect window. */
    g_assert_cmphex(qtest_readl(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MEMWIN_DATA),
                    ==, value);

    /* And through the BAR0 SRAM window at 0x8000 + (offset & 0x7FFF). */
    g_assert_cmphex(bar_readl(qts, BCM_SRAM_WINDOW + (sram_off & 0x7fffu)), ==,
                    value);

    qtest_quit(qts);
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
    QTestState *qts = bcm570x_start();

    bcm570x_aim_window(qts);

    ind_mem_writel(qts, BCM_SRAM_FW_MBOX, BCM_FW_MAGIC);
    g_assert_cmphex(ind_mem_readl(qts, BCM_SRAM_FW_MBOX), ==,
                    BCM_FW_HANDSHAKE_DONE);

    bar_writel(qts, BCM_MISC_CFG, BCM_MISCCFG_RESET);

    /* The reset trigger self-clears; the driver polls MISC_CFG bit 0 for it. */
    g_assert_cmphex(bar_readl(qts, BCM_MISC_CFG) & BCM_MISCCFG_RESET, ==, 0);

    /* The reset leaves the mailbox reading ready for the post-reset poll. */
    g_assert_cmphex(ind_mem_readl(qts, BCM_SRAM_FW_MBOX), ==,
                    BCM_FW_HANDSHAKE_DONE);

    qtest_quit(qts);
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
    QTestState *qts = bcm570x_start();

    bcm570x_aim_window(qts);

    bar_writel(qts, BCM_MISC_CFG, BCM_MISCCFG_RESET);

    ind_mem_writel(qts, BCM_SRAM_FW_MBOX, BCM_FW_MAGIC);
    g_assert_cmphex(ind_mem_readl(qts, BCM_SRAM_FW_MBOX), ==,
                    BCM_FW_HANDSHAKE_DONE);

    qtest_quit(qts);
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
    QTestState *qts = bcm570x_start();

    bcm570x_aim_window(qts);

    g_assert_cmphex(bar_readl(qts, BCM_FW_PROGRESS), ==, BCM_FW_PROGRESS_INIT);
    g_assert_cmphex(ind_reg_readl(qts, BCM_FW_PROGRESS), ==,
                    BCM_FW_PROGRESS_INIT);

    bar_writel(qts, BCM_MISC_CFG, BCM_MISCCFG_RESET);
    g_assert_cmphex(bar_readl(qts, BCM_FW_PROGRESS), ==, BCM_FW_PROGRESS_INIT);

    qtest_quit(qts);
}

/*
 * The same handshake, but the reset is triggered through the indirect register
 * window (config 0x78/0x80) rather than the BAR -- the path the 5700/5701 take.
 * No BAR mapping is needed: every step here is config space.
 */
static void test_reset_handshake_via_indirect(const void *data)
{
    QTestState *qts = bcm570x_start();

    ind_mem_writel(qts, BCM_SRAM_FW_MBOX, BCM_FW_MAGIC);

    ind_reg_writel(qts, BCM_MISC_CFG, BCM_MISCCFG_RESET);

    g_assert_cmphex(ind_mem_readl(qts, BCM_SRAM_FW_MBOX), ==,
                    BCM_FW_HANDSHAKE_DONE);

    qtest_quit(qts);
}

/*
 * A *_MODE register's reset bit self-clears while the rest of the write
 * persists -- the driver sets the bit and polls for it to read back clear.
 */
static void test_self_clearing_reset_bits(const void *data)
{
    QTestState *qts = bcm570x_start();

    bcm570x_aim_window(qts);

    /* MAC_MODE: reset bit set alongside a port-mode bit that must survive. */
    bar_writel(qts, BCM_MAC_MODE, BCM_MODE_RESET | 0x4);
    g_assert_cmphex(bar_readl(qts, BCM_MAC_MODE) & BCM_MODE_RESET, ==, 0);
    g_assert_cmphex(bar_readl(qts, BCM_MAC_MODE) & 0x4, ==, 0x4);

    /* HCC_MODE reset bit self-clears too. */
    bar_writel(qts, BCM_HCC_MODE, BCM_MODE_RESET);
    g_assert_cmphex(bar_readl(qts, BCM_HCC_MODE) & BCM_MODE_RESET, ==, 0);

    qtest_quit(qts);
}

/*
 * HCC_MODE stores and reads back faithfully: the driver's idle sequence writes
 * ENABLE, then writes 0 and polls up to ~2000 times for ENABLE to read clear,
 * aborting the attach if it never does.
 */
static void test_hcc_mode_idle_poll(const void *data)
{
    QTestState *qts = bcm570x_start();

    bcm570x_aim_window(qts);

    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);
    g_assert_cmphex(bar_readl(qts, BCM_HCC_MODE) & BCM_HCC_ENABLE, ==,
                    BCM_HCC_ENABLE);

    bar_writel(qts, BCM_HCC_MODE, 0);
    g_assert_cmphex(bar_readl(qts, BCM_HCC_MODE) & BCM_HCC_ENABLE, ==, 0);

    qtest_quit(qts);
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
    QTestState *qts = bcm570x_start();

    bcm570x_aim_window(qts);

    bar_writel(qts, BCM_BUFMGR_MODE, BCM_BUFMGR_ENABLE | BCM_BUFMGR_ATTN);

    /* ENABLE reads back clear for the driver's poll... */
    g_assert_cmphex(bar_readl(qts, BCM_BUFMGR_MODE) & BCM_BUFMGR_ENABLE, ==, 0);
    /* ...while ATTN_ENABLE, which the driver did not poll, persists. */
    g_assert_cmphex(bar_readl(qts, BCM_BUFMGR_MODE) & BCM_BUFMGR_ATTN, ==,
                    BCM_BUFMGR_ATTN);

    qtest_quit(qts);
}

/*
 * The host-coalescing idle poll reads the WHOLE register for zero, not just the
 * ENABLE bit ("Host Coalescing Mode Rgtr did not clear" on timeout). Write a
 * value with several bits set, then write 0 and confirm every bit reads back
 * clear, the state the driver waits for before it reprograms the block.
 */
static void test_hcc_mode_clears_to_zero(const void *data)
{
    QTestState *qts = bcm570x_start();

    bcm570x_aim_window(qts);

    bar_writel(qts, BCM_HCC_MODE, 0x0000013e);
    bar_writel(qts, BCM_HCC_MODE, 0);
    g_assert_cmphex(bar_readl(qts, BCM_HCC_MODE), ==, 0);

    qtest_quit(qts);
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
    QTestState *qts = bcm570x_start();
    uint32_t comm;

    bcm570x_aim_window(qts);

    /* A read of the PHY identity: BUSY clears and the low 16 carry the value. */
    bar_writel(qts, BCM_MI_COMM,
               BCM_MICOMM_BUSY | BCM_MICMD_READ |
               (BCM_PHY_ADDR << BCM_MICOMM_PHY_SHIFT) |
               (MII_PHYIDR1 << BCM_MICOMM_REG_SHIFT));
    comm = bar_readl(qts, BCM_MI_COMM);
    g_assert_cmphex(comm & BCM_MICOMM_BUSY, ==, 0);
    g_assert_cmphex(comm & 0xffff, ==, BCM_PHY_IDR1_VALUE);

    /* A write command: BUSY clears too, so the driver's poll returns. */
    bar_writel(qts, BCM_MI_COMM,
               BCM_MICOMM_BUSY | BCM_MICMD_WRITE |
               (BCM_PHY_ADDR << BCM_MICOMM_PHY_SHIFT) |
               (MII_BMCR << BCM_MICOMM_REG_SHIFT) |
               (MII_BMCR_AUTOEN | MII_BMCR_STARTNEG));
    g_assert_cmphex(bar_readl(qts, BCM_MI_COMM) & BCM_MICOMM_BUSY, ==, 0);

    qtest_quit(qts);
}

/*
 * MISC_CTL[31:16] presents the chip id the driver reads its whole identity
 * from. For the single-port default it must decode to BCM5701 (chip id 0x0105,
 * ASIC revision chip-id >> 12 == 0), not merely non-zero.
 */
static void test_misc_ctl_asic_revision(const void *data)
{
    QTestState *qts = bcm570x_start();
    uint32_t misc_ctl = qtest_readl(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MISC_CTL);

    g_assert_cmphex(misc_ctl >> 16, ==, BCM_CHIPID_BCM5701);
    g_assert_cmphex((misc_ctl >> 16) >> 12, ==, BCM_ASICREV_BCM5701);

    qtest_quit(qts);
}

/*
 * The MAC-address mailbox is honoured. The model presents the six bytes of the
 * -device mac= in SRAM 0x0C14/0x0C18 with the tag 0x484B in the high half of
 * 0x0C14, the path bge_attach reads before it falls back to an EEPROM.
 */
static void test_mac_address_mailbox(const void *data)
{
    QTestState *qts = qtest_initf(MACHINE_WITH_BRIDGE
                                  " -netdev user,id=n0"
                                  " -device bcm570x,addr=%02x.0,netdev=n0,"
                                  "mac=52:54:00:ab:cd:ef", BCM_SLOT);

    /* Tag plus the first two address bytes in the high mailbox word. */
    g_assert_cmphex(ind_mem_readl(qts, 0x0c14), ==, 0x484b5254u);
    /* The remaining four address bytes in the low word. */
    g_assert_cmphex(ind_mem_readl(qts, 0x0c18), ==, 0x00abcdefu);

    qtest_quit(qts);
}

/*
 * The PHY answers through MI_COMM with the identity brgphy binds on, and its
 * link comes up when the guest configures the interface -- the load-bearing
 * link/autoneg result. MI_STS, the auto-poll link latch, follows the carrier.
 */
static void test_phy_readback(const void *data)
{
    QTestState *qts = bcm570x_start();

    bcm570x_aim_window(qts);

    /* The Broadcom BCM5701 identity. */
    g_assert_cmphex(mii_read(qts, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);
    g_assert_cmphex(mii_read(qts, BCM_PHY_ADDR, MII_PHYIDR2), ==,
                    BCM_PHY_IDR2_BCM5701);

    /* Out of reset the link is still negotiating: down, incomplete. */
    g_assert_cmphex(mii_read(qts, BCM_PHY_ADDR, MII_BMSR) & MII_BMSR_LINK,
                    ==, 0);
    g_assert_cmphex(bar_readl(qts, BCM_MI_STS) & BCM_MISTS_LINK, ==, 0);

    /* Configure the interface (restart autonegotiation): link comes up. */
    mii_write(qts, BCM_PHY_ADDR, MII_BMCR,
              MII_BMCR_AUTOEN | MII_BMCR_STARTNEG);

    g_assert_cmphex(mii_read(qts, BCM_PHY_ADDR, MII_BMSR) & MII_BMSR_LINK,
                    ==, MII_BMSR_LINK);
    g_assert_cmphex(mii_read(qts, BCM_PHY_ADDR, MII_BMSR) & MII_BMSR_ANEGDONE,
                    ==, MII_BMSR_ANEGDONE);

    /* The auto-poll link latch now reads the carrier too. */
    g_assert_cmphex(bar_readl(qts, BCM_MI_STS) & BCM_MISTS_LINK, ==,
                    BCM_MISTS_LINK);

    qtest_quit(qts);
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
    QTestState *qts = bcm570x_start();
    uint16_t aux;

    bcm570x_aim_window(qts);

    /* Still negotiating: ACOMP clear, but a real value so it is not "no PHY". */
    aux = mii_read(qts, BCM_PHY_ADDR, BRGPHY_AUXSTS);
    g_assert_cmphex(aux & BRGPHY_AUXSTS_ACOMP, ==, 0);
    g_assert_cmphex(aux, !=, 0xffff);

    /* Configure the interface (restart autonegotiation): link comes up. */
    mii_write(qts, BCM_PHY_ADDR, MII_BMCR,
              MII_BMCR_AUTOEN | MII_BMCR_STARTNEG);

    /* The gate the up-path waits on: ACOMP set, a valid HCD, not all-ones. */
    aux = mii_read(qts, BCM_PHY_ADDR, BRGPHY_AUXSTS);
    g_assert_cmphex(aux & BRGPHY_AUXSTS_ACOMP, ==, BRGPHY_AUXSTS_ACOMP);
    g_assert_cmphex(aux & BRGPHY_AUXSTS_AN_RES, ==, BRGPHY_AUXSTS_1000TFDX);
    g_assert_cmphex(aux, !=, 0xffff);

    qtest_quit(qts);
}

/*
 * The load-bearing byte-order test. With the slot's DMA swapper set as the real
 * system programs it and the driver's swap options written, a transmit
 * descriptor and its buffer are DMA'd through the Bridge, the frame reaches the
 * backend byte-for-byte, and the status block the chip writes back lands
 * big-endian in guest memory with its transmit consumer advanced. The frame
 * pattern has every byte distinct so a swap anywhere on the path shows.
 */
static void test_transmit_round_trip(const void *data)
{
    int sv[2];
    QTestState *qts;
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
    qts = qtest_initf(MACHINE_WITH_BRIDGE
                      " -netdev socket,fd=%d,id=n0"
                      " -device bcm570x,addr=%02x.0,netdev=n0", sv[1], BCM_SLOT);

    bcm570x_setup_dma(qts);

    /* The driver's byte-swap programming: stored for read-back, no live swap. */
    ind_reg_writel(qts, BCM_MODE_CTL, BCM_DMA_SWAP_OPTIONS);

    /* The send-ring control block in on-chip SRAM: host base and ring length. */
    ind_mem_writel(qts, BCM_SRAM_SEND_RING_RCB + 0, TX_RING_IOVA >> 32);
    ind_mem_writel(qts, BCM_SRAM_SEND_RING_RCB + 4, TX_RING_IOVA & 0xffffffff);
    ind_mem_writel(qts, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);

    /* The status block's host address, and enable the coalescing block. */
    bar_writel(qts, BCM_HCC_STATUSBLK_HI, STATUS_IOVA >> 32);
    bar_writel(qts, BCM_HCC_STATUSBLK_LO, STATUS_IOVA & 0xffffffff);
    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);

    /* One transmit descriptor at ring index 0, built big-endian. */
    memset(bd, 0, sizeof(bd));
    put32be(bd + 0, TX_BUF_IOVA >> 32);
    put32be(bd + 4, TX_BUF_IOVA & 0xffffffff);
    put16be(bd + 8, sizeof(frame));
    put16be(bd + 10, BCM_TXBDFLAG_END);
    qtest_memwrite(qts, TX_RING_PHYS, bd, sizeof(bd));
    qtest_memwrite(qts, TX_BUF_PHYS, frame, sizeof(frame));
    qtest_memset(qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);

    /* Ring the doorbell: one descriptor produced. */
    bar_writel(qts, BCM_MBX_TX_PROD_LO, 1);

    /* The frame reached the backend unchanged. */
    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));

    /* The status block landed big-endian with the transmit consumer advanced. */
    qtest_memread(qts, STATUS_PHYS, sb, sizeof(sb));
    g_assert_cmphex(get32be(sb + 0), ==, BCM_STATFLAG_UPDATED);
    g_assert_cmphex(get16be(sb + BCM_STATUS_TX_CONS_OFF), ==, 1);
    /* Raw bytes prove the order: big-endian 1 is {0x00, 0x01}, not {0x01, 0x00}. */
    g_assert_cmphex(sb[BCM_STATUS_TX_CONS_OFF], ==, 0x00);
    g_assert_cmphex(sb[BCM_STATUS_TX_CONS_OFF + 1], ==, 0x01);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A producer index past the ring must not hang the mailbox write. The ring
 * holds TX_RING_LEN descriptors, so an index at or beyond it is out of range;
 * the write logs a guest error and returns rather than chasing a consumer it
 * never reaches (the IOC3 ethernet review's first finding). The MMIO access
 * after each proves the write returned.
 */
static void test_tx_producer_out_of_range(const void *data)
{
    QTestState *qts = bcm570x_start();

    bcm570x_setup_dma(qts);

    ind_mem_writel(qts, BCM_SRAM_SEND_RING_RCB + 0, TX_RING_IOVA >> 32);
    ind_mem_writel(qts, BCM_SRAM_SEND_RING_RCB + 4, TX_RING_IOVA & 0xffffffff);
    ind_mem_writel(qts, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);

    /* Just past the ring, and far past it: each write must return. */
    bar_writel(qts, BCM_MBX_TX_PROD_LO, TX_RING_LEN);
    g_assert_cmphex(mii_read(qts, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);

    bar_writel(qts, BCM_MBX_TX_PROD_LO, 0xffff);
    g_assert_cmphex(mii_read(qts, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);

    qtest_quit(qts);
}

/*
 * The receive round trip. A standard-ring buffer is posted, a broadcast frame
 * injected from the backend, and the chip DMAs it into the buffer, writes a
 * completed descriptor into the return ring big-endian, and advances the status
 * block's receive producer. The driver's return-ring acknowledgement is
 * accepted. Broadcast so the address filter passes it regardless of the
 * station address.
 */
static void test_receive_round_trip(const void *data)
{
    int sv[2];
    QTestState *qts;
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
    qts = qtest_initf(MACHINE_WITH_BRIDGE
                      " -netdev socket,fd=%d,id=n0"
                      " -device bcm570x,addr=%02x.0,netdev=n0", sv[1], BCM_SLOT);

    bcm570x_setup_dma(qts);

    /* Standard-ring RCB in CSRs, return-ring RCB in SRAM, status block on. */
    bar_writel(qts, BCM_RX_STD_RCB_HI, STD_RING_IOVA >> 32);
    bar_writel(qts, BCM_RX_STD_RCB_LO, STD_RING_IOVA & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_MAXLEN, RX_RING_LEN << 16);
    ind_mem_writel(qts, BCM_SRAM_RX_RETURN_RCB + 0, RET_RING_IOVA >> 32);
    ind_mem_writel(qts, BCM_SRAM_RX_RETURN_RCB + 4, RET_RING_IOVA & 0xffffffff);
    ind_mem_writel(qts, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);
    bar_writel(qts, BCM_HCC_STATUSBLK_HI, STATUS_IOVA >> 32);
    bar_writel(qts, BCM_HCC_STATUSBLK_LO, STATUS_IOVA & 0xffffffff);
    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);

    /* Post one standard-ring buffer at index 0, built big-endian. */
    memset(bd, 0, sizeof(bd));
    put32be(bd + 0, RX_BUF_IOVA >> 32);
    put32be(bd + 4, RX_BUF_IOVA & 0xffffffff);
    qtest_memwrite(qts, STD_RING_PHYS, bd, sizeof(bd));
    qtest_memset(qts, RX_BUF_PHYS, 0, sizeof(frame));
    qtest_memset(qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);

    bar_writel(qts, BCM_MBX_RX_STD_PROD_LO, 1);
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    wait_for_rx_prod(qts, 1);

    /* The completed descriptor landed in the return ring, big-endian. */
    qtest_memread(qts, RET_RING_PHYS, ret, sizeof(ret));
    g_assert_cmphex(get16be(ret + BCM_RXBD_IDX), ==, 0);
    g_assert_cmphex(get16be(ret + BCM_RXBD_LEN), ==, sizeof(frame));
    g_assert_cmphex(get16be(ret + BCM_RXBD_FLAGS) & BCM_RXBDFLAG_END, ==,
                    BCM_RXBDFLAG_END);

    /* The status block's receive producer advanced, big-endian. */
    qtest_memread(qts, STATUS_PHYS, sb, sizeof(sb));
    g_assert_cmphex(get16be(sb + BCM_STATUS_RX_PROD_OFF), ==, 1);
    g_assert_cmphex(sb[BCM_STATUS_RX_PROD_OFF], ==, 0x00);
    g_assert_cmphex(sb[BCM_STATUS_RX_PROD_OFF + 1], ==, 0x01);

    /* And the frame reached the posted buffer byte-for-byte. */
    qtest_memread(qts, RX_BUF_PHYS, got, sizeof(frame));
    g_assert_cmpmem(got, sizeof(frame), frame, sizeof(frame));

    /* The driver's return-ring acknowledgement is accepted (write returns). */
    bar_writel(qts, BCM_MBX_RX_CONS0_LO, 1);
    g_assert_cmphex(mii_read(qts, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);

    qtest_quit(qts);
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
    QTestState *qts = bcm570x_start();

    bcm570x_setup_dma(qts);
    bar_writel(qts, BCM_RX_STD_RCB_MAXLEN, RX_RING_LEN << 16);
    ind_mem_writel(qts, BCM_SRAM_RX_RETURN_RCB + 8, RX_RING_LEN << 16);

    bar_writel(qts, BCM_MBX_RX_STD_PROD_LO, RX_RING_LEN);
    g_assert_cmphex(mii_read(qts, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);
    bar_writel(qts, BCM_MBX_RX_STD_PROD_LO, 0xffff);
    g_assert_cmphex(mii_read(qts, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);

    bar_writel(qts, BCM_MBX_RX_CONS0_LO, RX_RING_LEN);
    g_assert_cmphex(mii_read(qts, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);
    bar_writel(qts, BCM_MBX_RX_CONS0_LO, 0xffff);
    g_assert_cmphex(mii_read(qts, BCM_PHY_ADDR, MII_PHYIDR1), ==,
                    BCM_PHY_IDR1_VALUE);

    qtest_quit(qts);
}

/*
 * The interrupt mailbox masks and re-enables the INTA line, and the global
 * MISC_CTL mask suppresses it. The chip drives the line as a level the Bridge
 * follows: it goes up when a status-block update is posted with interrupts
 * unmasked and down when the driver acknowledges. Producer-0 doorbells stand in
 * for real completions -- each drives a status-block DMA with no ring walk, so
 * the test needs no descriptors or frame.
 */
static void test_interrupt_mailbox(const void *data)
{
    QTestState *qts = bcm570x_start();
    uint32_t misc;

    bcm570x_arm_status(qts);
    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);

    /* Masked out of reset: an update posts, but the line stays down. */
    bar_writel(qts, BCM_MBX_TX_PROD_LO, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BCM_INT_PIN, ==, 0);

    /* Enable interrupts (mailbox 0); the next update asserts the line. */
    bar_writel(qts, BCM_MBX_IRQ0_LO, 0);
    bar_writel(qts, BCM_MBX_TX_PROD_LO, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BCM_INT_PIN, ==,
                    BCM_INT_PIN);
    g_assert_cmphex(qtest_readl(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_PCISTATE) &
                    BCM_PCISTATE_INTR_NA, ==, 0);

    /* Write 1: masks and acknowledges. The line drops; INTR_NOT_ACTIVE reads. */
    bar_writel(qts, BCM_MBX_IRQ0_LO, 1);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BCM_INT_PIN, ==, 0);
    g_assert_cmphex(qtest_readl(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_PCISTATE) &
                    BCM_PCISTATE_INTR_NA, ==, BCM_PCISTATE_INTR_NA);

    /* Write 0 re-enables; the acknowledge cleared the latch, so the line stays
     * down until a fresh update re-asserts it. */
    bar_writel(qts, BCM_MBX_IRQ0_LO, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BCM_INT_PIN, ==, 0);
    bar_writel(qts, BCM_MBX_TX_PROD_LO, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BCM_INT_PIN, ==,
                    BCM_INT_PIN);

    /* The global mask in MISC_CTL suppresses the line while it is set, and the
     * line returns when it clears -- the pending update was never taken. */
    misc = qtest_readl(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MISC_CTL);
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MISC_CTL,
                 misc | BCM_MISCCTL_MASK_INTR);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BCM_INT_PIN, ==, 0);
    qtest_writel(qts, CFG0(BCM_SLOT, 0) + BCM_PCI_MISC_CTL, misc);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BCM_INT_PIN, ==,
                    BCM_INT_PIN);

    qtest_quit(qts);
}

/*
 * The status-block DMA is gated by HCC_MODE.ENABLE. With the coalescing block
 * disabled a doorbell posts nothing to host memory and raises no interrupt;
 * enabling it lets the same doorbell DMA the status block out.
 */
static void test_status_block_gated_by_hcc(const void *data)
{
    QTestState *qts = bcm570x_start();
    uint8_t sb[BCM_STATUS_BLK_SIZE];

    bcm570x_arm_status(qts);

    /* Disabled: the status region stays zero and the line stays down. */
    bar_writel(qts, BCM_MBX_TX_PROD_LO, 0);
    qtest_memread(qts, STATUS_PHYS, sb, sizeof(sb));
    g_assert_cmphex(get32be(sb), ==, 0);
    g_assert_cmphex(qtest_readl(qts, BRIDGE_INT_STATUS) & BCM_INT_PIN, ==, 0);

    /* Enabled: the same doorbell now DMAs the status block, UPDATED set. */
    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(qts, BCM_MBX_TX_PROD_LO, 0);
    qtest_memread(qts, STATUS_PHYS, sb, sizeof(sb));
    g_assert_cmphex(get32be(sb) & BCM_STATFLAG_UPDATED, ==, BCM_STATFLAG_UPDATED);

    qtest_quit(qts);
}

/* Drive a migration between two connected qtest instances and block until it
 * completes; `to` is already listening on `uri` via its -incoming. Modelled on
 * tests/qtest/sn0-migration-test.c's migrate_stopped_vm. */
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
 * crossed the migration -- the fields this task added to the VMState.
 */
static void test_state_survives_migration(const void *data)
{
    g_autofree gchar *tmpfs = NULL;
    g_autofree gchar *uri = NULL;
    g_autofree gchar *dest = NULL;
    GError *err = NULL;
    QTestState *from, *to;
    const uint32_t sram_sentinel = 0x1a2b3c4d;
    uint32_t misc;

    tmpfs = g_dir_make_tmp("bcm570x-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_initf(MACHINE_WITH_BRIDGE
                       " -netdev user,id=n0"
                       " -device bcm570x,addr=%02x.0,netdev=n0,"
                       "mac=52:54:00:11:22:33", BCM_SLOT);
    bcm570x_arm_status(from);
    bar_writel(from, BCM_HCC_MODE, BCM_HCC_ENABLE);

    /* An SRAM word reset does not touch, so its survival is unambiguous. */
    ind_mem_writel(from, 0x0c1c, sram_sentinel);

    /* Enable interrupts and leave the line asserted at the migration point. */
    bar_writel(from, BCM_MBX_IRQ0_LO, 0);
    bar_writel(from, BCM_MBX_TX_PROD_LO, 0);
    g_assert_cmphex(qtest_readl(from, BRIDGE_INT_STATUS) & BCM_INT_PIN, ==,
                    BCM_INT_PIN);
    g_assert_cmphex(qtest_readl(from, CFG0(BCM_SLOT, 0) + BCM_PCI_PCISTATE) &
                    BCM_PCISTATE_INTR_NA, ==, 0);

    dest = g_strdup_printf(MACHINE_WITH_BRIDGE
                           " -netdev user,id=n0"
                           " -device bcm570x,addr=%02x.0,netdev=n0,"
                           "mac=52:54:00:11:22:33 -incoming %s", BCM_SLOT, uri);
    to = qtest_init(dest);
    bcm570x_migrate(from, uri);

    /* The register file (status-block host address) and SRAM word came across. */
    g_assert_cmphex(ind_reg_readl(to, BCM_HCC_STATUSBLK_LO), ==,
                    STATUS_IOVA & 0xffffffff);
    g_assert_cmphex(ind_mem_readl(to, 0x0c1c), ==, sram_sentinel);

    /* The interrupt config came across, line still asserted. */
    g_assert_cmphex(qtest_readl(to, CFG0(BCM_SLOT, 0) + BCM_PCI_PCISTATE) &
                    BCM_PCISTATE_INTR_NA, ==, 0);

    /* The migrated latch and mask are live: mask then unmask the global mask
     * and the line follows, without any fresh update to re-establish it. */
    misc = qtest_readl(to, CFG0(BCM_SLOT, 0) + BCM_PCI_MISC_CTL);
    qtest_writel(to, CFG0(BCM_SLOT, 0) + BCM_PCI_MISC_CTL,
                 misc | BCM_MISCCTL_MASK_INTR);
    g_assert_cmphex(qtest_readl(to, CFG0(BCM_SLOT, 0) + BCM_PCI_PCISTATE) &
                    BCM_PCISTATE_INTR_NA, ==, BCM_PCISTATE_INTR_NA);
    qtest_writel(to, CFG0(BCM_SLOT, 0) + BCM_PCI_MISC_CTL, misc);
    g_assert_cmphex(qtest_readl(to, CFG0(BCM_SLOT, 0) + BCM_PCI_PCISTATE) &
                    BCM_PCISTATE_INTR_NA, ==, 0);

    qtest_quit(to);
    qtest_quit(from);
}

/*
 * Arm a BCM5704 function's status-block DMA entirely through its own config
 * space: enable bus mastering, give its transmit ring a length so a doorbell
 * passes the range guard, program the status block's host page, and enable the
 * host-coalescing block. No BAR mapping is needed -- the indirect windows reach
 * each function's register file and SRAM, so the two functions are configured
 * independently at one slot.
 */
static void dual5704_arm(QTestState *qts, unsigned fn, uint64_t status_iova)
{
    qtest_writel(qts, CFG0(BCM_SLOT, fn) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    ind_mem_writel_fn(qts, fn, BCM_SRAM_SEND_RING_RCB + 8, TX_RING_LEN << 16);
    ind_reg_writel_fn(qts, fn, BCM_HCC_STATUSBLK_HI, status_iova >> 32);
    ind_reg_writel_fn(qts, fn, BCM_HCC_STATUSBLK_LO, status_iova & 0xffffffff);
    ind_reg_writel_fn(qts, fn, BCM_HCC_MODE, BCM_HCC_ENABLE);
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
    QTestState *qts = qtest_initf(MACHINE_WITH_BRIDGE
                                  " -netdev user,id=n0"
                                  " -device bcm570x,model=bcm5704,addr=%02x.0,"
                                  "netdev=n0", BCM_SLOT);
    uint8_t sb0[BCM_STATUS_BLK_SIZE], sb1[BCM_STATUS_BLK_SIZE];
    unsigned fn;

    /* Both functions present, each carrying the copper BCM5704 four-tuple. */
    for (fn = 0; fn < 2; fn++) {
        g_assert_cmphex(qtest_readl(qts, CFG0(BCM_SLOT, fn) + PCI_VENDOR_ID), ==,
                        BCM_VENDOR_ID |
                        ((uint32_t)BCM_DEVICE_ID_BCM5704 << 16));
        g_assert_cmphex(qtest_readl(qts, CFG0(BCM_SLOT, fn) +
                                    PCI_SUBSYSTEM_VENDOR_ID), ==,
                        BCM_SUBSYS_VENDOR_ID |
                        ((uint32_t)BCM_SUBSYS_ID_BCM5704 << 16));
    }

    /*
     * Function 0 sets the PCI_HEADER_TYPE multifunction bit, so a guest
     * enumerates function 1 at all. Read the dword at 0x0C (a 4-byte config
     * access needs no byte-lane correction) and take the header-type byte.
     */
    g_assert_cmphex((qtest_readl(qts, CFG0(BCM_SLOT, 0) + 0x0c) >> 16) &
                    BCM_HEADER_MULTIFUNC, ==, BCM_HEADER_MULTIFUNC);

    /*
     * Distinct MAC identity per function: both mailboxes carry the 0x484B tag,
     * and the address bytes differ (each function took its own default MAC).
     */
    g_assert_cmphex(ind_mem_readl_fn(qts, 0, 0x0c14) >> 16, ==, 0x484b);
    g_assert_cmphex(ind_mem_readl_fn(qts, 1, 0x0c14) >> 16, ==, 0x484b);
    g_assert_cmphex(ind_mem_readl_fn(qts, 0, 0x0c18), !=,
                    ind_mem_readl_fn(qts, 1, 0x0c18));

    /* The per-slot Bridge DMA path (shared), then arm each function's engine. */
    qtest_writel(qts, REG(BRIDGE_DEVICE(BCM_SLOT)),
                 DEV_MEMORY_SPACE | DEV_SWAP_DIRECT | (CARD_MMIO(BCM_SLOT) >> 20));
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);
    dual5704_arm(qts, 0, STATUS_IOVA);
    dual5704_arm(qts, 1, STATUS2_IOVA);
    qtest_memset(qts, STATUS_PHYS, 0, BCM_STATUS_BLK_SIZE);
    qtest_memset(qts, STATUS2_PHYS, 0, BCM_STATUS_BLK_SIZE);

    /* Drive function 0's doorbell only: its status block updates, function 1's
     * stays zero -- the engines are independent. */
    ind_reg_writel_fn(qts, 0, BCM_MBX_TX_PROD_LO, 0);
    qtest_memread(qts, STATUS_PHYS, sb0, sizeof(sb0));
    qtest_memread(qts, STATUS2_PHYS, sb1, sizeof(sb1));
    g_assert_cmphex(get32be(sb0) & BCM_STATFLAG_UPDATED, ==, BCM_STATFLAG_UPDATED);
    g_assert_cmphex(get32be(sb1), ==, 0);

    /* And driving function 1 moves its own status block -- both work, apart. */
    ind_reg_writel_fn(qts, 1, BCM_MBX_TX_PROD_LO, 0);
    qtest_memread(qts, STATUS2_PHYS, sb1, sizeof(sb1));
    g_assert_cmphex(get32be(sb1) & BCM_STATFLAG_UPDATED, ==, BCM_STATFLAG_UPDATED);

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

    tmpfs = g_dir_make_tmp("bcm570x-test-XXXXXX", &err);
    g_assert(tmpfs);
    uri = g_strdup_printf("unix:%s/migrate.sock", tmpfs);

    from = qtest_initf(MACHINE_WITH_BRIDGE
                       " -netdev user,id=n0"
                       " -device bcm570x,model=bcm5704,addr=%02x.0,netdev=n0",
                       BCM_SLOT);
    ind_mem_writel_fn(from, 0, 0x0c1c, 0xaaaa0000);
    ind_mem_writel_fn(from, 1, 0x0c1c, 0xbbbb1111);

    dest = g_strdup_printf(MACHINE_WITH_BRIDGE
                           " -netdev user,id=n0"
                           " -device bcm570x,model=bcm5704,addr=%02x.0,netdev=n0"
                           " -incoming %s", BCM_SLOT, uri);
    to = qtest_init(dest);
    bcm570x_migrate(from, uri);

    g_assert_cmphex(ind_mem_readl_fn(to, 0, 0x0c1c), ==, 0xaaaa0000);
    g_assert_cmphex(ind_mem_readl_fn(to, 1, 0x0c1c), ==, 0xbbbb1111);

    qtest_quit(to);
    qtest_quit(from);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

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
    qtest_add_data_func("/bcm570x/phy-readback", NULL, test_phy_readback);
    qtest_add_data_func("/bcm570x/phy-aux-status-link-up", NULL,
                        test_phy_aux_status_link_up);
    qtest_add_data_func("/bcm570x/transmit-round-trip", NULL,
                        test_transmit_round_trip);
    qtest_add_data_func("/bcm570x/tx-producer-out-of-range", NULL,
                        test_tx_producer_out_of_range);
    qtest_add_data_func("/bcm570x/receive-round-trip", NULL,
                        test_receive_round_trip);
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
