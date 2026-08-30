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

/* The firmware-ready handshake in on-chip SRAM. */
#define BCM_SRAM_FW_MBOX        0x0b50u
#define BCM_FW_MAGIC            0x4b657654u
#define BCM_FW_HANDSHAKE_DONE   0xb49a89abu     /* ~BCM_FW_MAGIC */

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
 * The driver writes the magic to SRAM 0x0B50, triggers a global reset via
 * MISC_CFG bit 0, and polls 0x0B50 for the complement the on-chip bootcode
 * leaves once it is up.
 */
static void test_reset_handshake_via_bar(const void *data)
{
    QTestState *qts = bcm570x_start();

    bcm570x_aim_window(qts);

    ind_mem_writel(qts, BCM_SRAM_FW_MBOX, BCM_FW_MAGIC);
    g_assert_cmphex(ind_mem_readl(qts, BCM_SRAM_FW_MBOX), ==, BCM_FW_MAGIC);

    bar_writel(qts, BCM_MISC_CFG, BCM_MISCCFG_RESET);

    g_assert_cmphex(ind_mem_readl(qts, BCM_SRAM_FW_MBOX), ==,
                    BCM_FW_HANDSHAKE_DONE);

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
    qtest_add_data_func("/bcm570x/reset-handshake-via-indirect", NULL,
                        test_reset_handshake_via_indirect);
    qtest_add_data_func("/bcm570x/self-clearing-reset-bits", NULL,
                        test_self_clearing_reset_bits);
    qtest_add_data_func("/bcm570x/hcc-mode-idle-poll", NULL,
                        test_hcc_mode_idle_poll);
    qtest_add_data_func("/bcm570x/misc-ctl-asic-revision", NULL,
                        test_misc_ctl_asic_revision);
    qtest_add_data_func("/bcm570x/mac-address-mailbox", NULL,
                        test_mac_address_mailbox);
    qtest_add_data_func("/bcm570x/phy-readback", NULL, test_phy_readback);
    qtest_add_data_func("/bcm570x/transmit-round-trip", NULL,
                        test_transmit_round_trip);
    qtest_add_data_func("/bcm570x/tx-producer-out-of-range", NULL,
                        test_tx_producer_out_of_range);
    qtest_add_data_func("/bcm570x/receive-round-trip", NULL,
                        test_receive_round_trip);
    qtest_add_data_func("/bcm570x/rx-producer-out-of-range", NULL,
                        test_rx_producer_out_of_range);

    return g_test_run();
}
