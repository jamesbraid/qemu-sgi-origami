/*
 * Alteon Tigon 2 (the SGI "eg" gigabit board) behind the Origin 200 Bridge.
 *
 * The Tigon is an add-in PCI card, so unlike the base I/O board's soldered
 * adapters it is instantiated with -device at a free slot and reached through
 * that slot's own Bridge decode -- the same way tests/qtest/sn0-bridge-test.c
 * hosts its probe card. Everything here runs on the machine's real
 * crosstalk-to-PCI topology so the register contract, the firmware-mediated
 * ring engine, and the descriptor byte order are all proven on the fabric the
 * card actually sits on rather than a generic PCI bus that has no Bridge.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#include <sys/socket.h>
#include <netinet/in.h>

/* Crosstalk space and the Bridge's widget window inside it. */
#define SPACE_XTALK     0x20000000000ULL
#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

/* A fixed station address, so PAR0/PAR1 and the EEPROM read are known. */
#define TIGON_MAC       "52:54:00:12:34:56"
static const uint8_t tigon_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

#define MACHINE_WITH_BRIDGE  \
    "-M origin200,io-bridge=on -device tigon,addr=3,mac=" TIGON_MAC

/*
 * The card's slot. Devices 0 and 1 are the base I/O board's SCSI adapters and
 * device 2 is IOC3; 3 is the first free slot, and its widget window is the
 * first of the six 1 MB windows (sn0-bridge-test.c's own window table).
 */
#define TIGON_SLOT      3
#define WINDOW_TIGON    (BRIDGE_BASE + 0x700000)

/* PCI type 0 configuration space through the Bridge. */
#define BRIDGE_CFG0     0x020000
#define CFG0_STRIDE     0x1000
#define CFG0(dev)       (BRIDGE_BASE + BRIDGE_CFG0 + (dev) * CFG0_STRIDE)

#define PCI_VENDOR_DEVICE   0x00
#define PCI_COMMAND         0x04
#define PCI_REV_CLASS       0x08
#define PCI_BAR0            0x10
#define PCI_INTR_LINE       0x3c

#define PCI_COMMAND_MEMORY  0x0002
#define PCI_COMMAND_MASTER  0x0004

/* The Bridge's own per-slot device register, low half of its 8-byte slot. */
#define REG_DATA            4
#define BRIDGE_DEVICE(n)    (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)
#define DEV_MEMORY_SPACE    (1u << 12)
#define DEV_SWAP_DIRECT     (1u << 19)   /* direct-map window swap bit */
#define BRIDGE_DIRECT_MAP_REG (BRIDGE_BASE + 0x080 + REG_DATA)

/*
 * A 64-bit direct-map DMA address: the low bits reach system memory and the
 * high half selects the direct-map path whose byte swap is gated by
 * DEV_SWAP_DIRECT, the same base sn0-qlogic-isp-test.c uses to prove DMA runs
 * through the Bridge rather than around it.
 */
#define IOVA_PREFIX         0xa500000000000000ULL
#define IOVA(phys)          (IOVA_PREFIX | (uint32_t)(phys))

/* Where the card's BAR is parked in PCI memory space (1 MB aligned). */
#define ADAPTER_MMIO        0x10000000u

/* The card's identity, from docs/datasheets/tigon.md. */
#define TIGON_VENDOR_ID     0x10a9
#define TIGON_DEVICE_ID     0x0009
#define TIGON_CLASS_CODE    0x020000
#define TIGON_CHIP_REV      0x6         /* Tigon 2 rev nibble */

/* Register offsets within the card's own window (if_tireg.h). */
#define TI_MISC_HOST_CTL    0x040
#define TI_MISC_LOCAL_CTL   0x044
#define TI_GCR_PAR0         0x600
#define TI_GCR_PAR1         0x604
#define TI_MHC_RESET        0x00000008
#define TI_MHC_CHIP_REV_MASK 0xf0000000

/* Serial-EEPROM bit-bang lines in MISC_LOCAL_CTL. */
#define TI_MLC_EE_CLK       0x00100000
#define TI_MLC_EE_TXEN      0x00200000
#define TI_MLC_EE_DOUT      0x00400000
#define TI_MLC_EE_DIN       0x00800000
#define TI_EE_MAC_OFFSET    0x8c

/* Interrupt and CPU control. */
#define TI_MHC_INTSTATE     0x00000001
#define TI_CPU_STATE        0x140

/* Mailbox doorbells and descriptor flags. */
#define TI_MB_HOSTINTR      0x504
#define TI_MB_SENDPROD_IDX  0x514
#define TI_MB_STDRXPROD_IDX 0x51c
#define TI_MB_CMDPROD_IDX   0x50c
#define TI_GCR_CMDRING      0x700
#define TI_CMD_SET_PROMISC_MODE 0x0a
#define TI_CMD_CODE_PROMISC_ENB 0x01
#define TI_MHC_CLEARINT     0x00000002
#define TI_MHC_MASK_INTS    0x00000040
#define TI_BDFLAG_TCP_UDP_CKSUM 0x0001
#define TI_BDFLAG_END       0x0004
#define TI_BDFLAG_UCAST_PKT 0x0020
#define TI_BDFLAG_BCAST_PKT 0x0060

/* GCR registers exercised by the ring model. */
#define TI_GCR_GENINFO_HI   0x608
#define TI_GCR_GENINFO_LO   0x60c
#define TI_GCR_OPMODE       0x618
#define TI_GCR_EVENTCONS_IDX 0x628
#define TI_GCR_RXRETURNCONS_IDX 0x680
#define TI_MHC_BYTE_SWAP_ENB 0x00000010
#define TI_MHC_WORD_SWAP_ENB 0x00000020
#define TI_OPMODE_BYTESWAP_BD   0x00000002
#define TI_OPMODE_WORDSWAP_BD   0x00000004
#define TI_OPMODE_BYTESWAP_DATA 0x00000010

/*
 * A general information block and its rings, laid out in low guest RAM. The
 * Bridge carries a DMA address below its translated window straight through to
 * system memory, so the card reaches these at the same address the test wrote
 * them, with no address-translation setup.
 */
#define GIB_PHYS            0x00200000u
#define GIB_EV_RCB          0x400
#define GIB_TX_RCB          0x420
#define GIB_STD_RX_RCB      0x430
#define GIB_RETURN_RCB      0x460
#define GIB_EV_PROD_PTR     0x470
#define GIB_RETURN_PROD_PTR 0x478
#define GIB_TX_CONS_PTR     0x480
#define GIB_SIZE            0x490

#define EVENT_RING_PHYS     0x00210000u
#define EV_PROD_PHYS        0x00220000u
#define TX_CONS_PHYS        0x00230000u
#define RETURN_RING_PHYS    0x00240000u
#define RETURN_PROD_PHYS    0x00250000u
#define STD_RX_RING_PHYS    0x00260000u
#define TX_RING_PHYS        0x00270000u
#define RX_BUF_PHYS         0x00280000u
#define TX_BUF_PHYS         0x00290000u
/* Misaligned on purpose -- forces the payload copy's buf & 3 != 0 path. */
#define TX_BUF_DIRECT_PHYS  (TX_BUF_PHYS + 2)
#define RX_BUF_DIRECT_PHYS  (RX_BUF_PHYS + 2)

#define JUMBO_RX_RING_PHYS  0x002a0000u
#define JUMBO_BUF_PHYS      0x002c0000u

#define TI_RCB_FLAG_HOST_RING   0x0040
#define TI_EV_FIRMWARE_UP       0x01
#define TI_BDTYPE_RECV_BD       0x0002
#define TI_BDTYPE_RECV_JUMBO_BD 0x0003
#define TI_BDFLAG_JUMBO_RING    0x0010
#define TI_MB_JUMBORXPROD_IDX   0x524
#define GIB_JUMBO_RX_RCB        0x440

/* Write a ring control block's 64-bit host address (two words, hi then lo). */
static void write_rcb_addr(QTestState *qts, uint32_t rcb, uint32_t phys)
{
    qtest_writel(qts, rcb + 0, 0);
    qtest_writel(qts, rcb + 4, phys);
}

/*
 * A 4-byte access needs no byte-lane correction -- sgi_bridge_window_correct()
 * moves only the low two bits and leaves a full-word access at its natural
 * offset -- so a 32-bit register is reached at WINDOW + offset directly.
 */
static void aim_window_at_bar(QTestState *qts)
{
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_BAR0, ADAPTER_MMIO);
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(TIGON_SLOT),
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
}

static void test_config_identity(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /* Vendor and device, the SGI Tigon pair, in one 32-bit config read. */
    g_assert_cmphex(qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_VENDOR_DEVICE), ==,
                    TIGON_VENDOR_ID | (TIGON_DEVICE_ID << 16));

    /* Class code (bytes 0x09-0x0B): Ethernet network controller. */
    g_assert_cmphex(qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_REV_CLASS) >> 8, ==,
                    TIGON_CLASS_CODE);

    /* One interrupt pin, INTA. */
    g_assert_cmphex((qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_INTR_LINE) >> 8) &
                    0xff, ==, 0x01);

    qtest_quit(qts);
}

static void test_one_memory_bar(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /* BAR0 is a 32-bit, non-prefetchable memory BAR: low nibble all clear. */
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_BAR0, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_BAR0), ==,
                    0xffffc000u);

    /* There is no second BAR. */
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_BAR0 + 4, 0xffffffffu);
    g_assert_cmphex(qtest_readl(qts, CFG0(TIGON_SLOT) + PCI_BAR0 + 4), ==, 0);

    qtest_quit(qts);
}

static void test_misc_host_ctl_chip_rev(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    aim_window_at_bar(qts);

    /* The chip-revision nibble is the driver's Tigon 1 vs 2 runtime test. */
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_MISC_HOST_CTL) &
                    TI_MHC_CHIP_REV_MASK, ==, (uint32_t)TIGON_CHIP_REV << 28);

    /* The RESET bit is accepted and leaves the read-only rev nibble intact. */
    qtest_writel(qts, WINDOW_TIGON + TI_MISC_HOST_CTL, TI_MHC_RESET);
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_MISC_HOST_CTL) &
                    TI_MHC_CHIP_REV_MASK, ==, (uint32_t)TIGON_CHIP_REV << 28);

    qtest_quit(qts);
}

/*
 * The station-address paths. The driver reads the MAC before any firmware is
 * downloaded, so both routes must answer independently of firmware state: the
 * bit-banged serial EEPROM (MISC_LOCAL_CTL) that the driver reads at attach,
 * and the GCR PAR0/PAR1 registers real firmware would present once running.
 *
 * The EEPROM helpers below replay the exact 24Cxx I2C sequence the NetBSD "ti"
 * driver's ti_eeprom_getbyte()/EEPROM_START()/EEPROM_STOP() macros drive
 * (if_ti.c, if_tireg.h): a start condition, an 8-bit control byte, two address
 * bytes for a write, a stop/start, an 8-bit read control byte, then eight bits
 * clocked out on EE_DIN.
 */
static uint32_t ee_read(QTestState *qts)
{
    return qtest_readl(qts, WINDOW_TIGON + TI_MISC_LOCAL_CTL);
}

static void ee_write(QTestState *qts, uint32_t v)
{
    qtest_writel(qts, WINDOW_TIGON + TI_MISC_LOCAL_CTL, v);
}

static void ee_setbit(QTestState *qts, uint32_t bit)
{
    ee_write(qts, ee_read(qts) | bit);
}

static void ee_clrbit(QTestState *qts, uint32_t bit)
{
    ee_write(qts, ee_read(qts) & ~bit);
}

static void ee_start(QTestState *qts)
{
    ee_setbit(qts, TI_MLC_EE_CLK);
    ee_setbit(qts, TI_MLC_EE_DOUT);
    ee_setbit(qts, TI_MLC_EE_TXEN);
    ee_clrbit(qts, TI_MLC_EE_DOUT);
    ee_clrbit(qts, TI_MLC_EE_CLK);
}

static void ee_stop(QTestState *qts)
{
    ee_clrbit(qts, TI_MLC_EE_TXEN);
    ee_clrbit(qts, TI_MLC_EE_DOUT);
    ee_setbit(qts, TI_MLC_EE_CLK);
    ee_setbit(qts, TI_MLC_EE_TXEN);
    ee_setbit(qts, TI_MLC_EE_DOUT);
    ee_clrbit(qts, TI_MLC_EE_TXEN);
    ee_clrbit(qts, TI_MLC_EE_CLK);
}

static void ee_putbyte(QTestState *qts, uint8_t byte)
{
    int i;

    ee_setbit(qts, TI_MLC_EE_TXEN);
    for (i = 0x80; i; i >>= 1) {
        if (byte & i) {
            ee_setbit(qts, TI_MLC_EE_DOUT);
        } else {
            ee_clrbit(qts, TI_MLC_EE_DOUT);
        }
        ee_setbit(qts, TI_MLC_EE_CLK);
        ee_clrbit(qts, TI_MLC_EE_CLK);
    }
    ee_clrbit(qts, TI_MLC_EE_TXEN);
    /* The ninth clock is the acknowledge; the driver reads and ignores it. */
    ee_setbit(qts, TI_MLC_EE_CLK);
    (void)ee_read(qts);
    ee_clrbit(qts, TI_MLC_EE_CLK);
}

static uint8_t ee_getbyte(QTestState *qts, uint16_t addr)
{
    uint8_t byte = 0;
    int i;

    ee_start(qts);
    ee_putbyte(qts, 0xa0);              /* write control byte */
    ee_putbyte(qts, (addr >> 8) & 0xff);
    ee_putbyte(qts, addr & 0xff);
    ee_stop(qts);
    ee_start(qts);
    ee_putbyte(qts, 0xa1);             /* read control byte */

    ee_clrbit(qts, TI_MLC_EE_TXEN);
    for (i = 0x80; i; i >>= 1) {
        ee_setbit(qts, TI_MLC_EE_CLK);
        if (ee_read(qts) & TI_MLC_EE_DIN) {
            byte |= i;
        }
        ee_clrbit(qts, TI_MLC_EE_CLK);
    }
    ee_stop(qts);

    return byte;
}

static void test_station_addr_par(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    aim_window_at_bar(qts);

    /* PAR0 = bytes 0-1, PAR1 = bytes 2-5, in the driver's big-endian pair. */
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_GCR_PAR0), ==,
                    ((uint32_t)tigon_mac[0] << 8) | tigon_mac[1]);
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_GCR_PAR1), ==,
                    ((uint32_t)tigon_mac[2] << 24) |
                    ((uint32_t)tigon_mac[3] << 16) |
                    ((uint32_t)tigon_mac[4] << 8) | tigon_mac[5]);

    qtest_quit(qts);
}

static void test_station_addr_eeprom(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    int i;

    aim_window_at_bar(qts);

    /*
     * The six MAC bytes sit at TI_EE_MAC_OFFSET + 2, not at TI_EE_MAC_OFFSET:
     * the manual's stated offset is two zero pad bytes before the address
     * (if_ti.c comment at the read call). Confirm both facts.
     */
    for (i = 0; i < 6; i++) {
        g_assert_cmphex(ee_getbyte(qts, TI_EE_MAC_OFFSET + 2 + i), ==,
                        tigon_mac[i]);
    }
    g_assert_cmphex(ee_getbyte(qts, TI_EE_MAC_OFFSET), ==, 0);
    g_assert_cmphex(ee_getbyte(qts, TI_EE_MAC_OFFSET + 1), ==, 0);

    qtest_quit(qts);
}

/*
 * Put the card in a state where its DMA lands big-endian in guest RAM: bus
 * master enabled, and OPMODE's descriptor/data byte-swap bits set, which is
 * what the driver programs on a big-endian host. The rings sit in low RAM the
 * Bridge carries straight through, so no address-translation setup is needed.
 */
static void enable_dma_big_endian(QTestState *qts)
{
    aim_window_at_bar(qts);
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_OPMODE,
                 TI_OPMODE_BYTESWAP_BD | TI_OPMODE_WORDSWAP_BD |
                 TI_OPMODE_BYTESWAP_DATA);
}

static void build_gib(QTestState *qts)
{
    qtest_memset(qts, GIB_PHYS, 0, GIB_SIZE);
    qtest_memset(qts, EVENT_RING_PHYS, 0, 0x800);
    qtest_memset(qts, EV_PROD_PHYS, 0, 8);
    qtest_memset(qts, TX_CONS_PHYS, 0, 8);
    qtest_memset(qts, RETURN_RING_PHYS, 0, 0x4000);
    qtest_memset(qts, RETURN_PROD_PHYS, 0, 8);
    qtest_memset(qts, STD_RX_RING_PHYS, 0, 0x800);
    qtest_memset(qts, TX_RING_PHYS, 0, 0x800);

    write_rcb_addr(qts, GIB_PHYS + GIB_EV_RCB, EVENT_RING_PHYS);
    /* Tigon 2 puts the transmit ring in host DRAM (the host-ring flag). */
    write_rcb_addr(qts, GIB_PHYS + GIB_TX_RCB, TX_RING_PHYS);
    qtest_writel(qts, GIB_PHYS + GIB_TX_RCB + 8,
                 (512u << 16) | TI_RCB_FLAG_HOST_RING);
    write_rcb_addr(qts, GIB_PHYS + GIB_STD_RX_RCB, STD_RX_RING_PHYS);
    qtest_memset(qts, JUMBO_RX_RING_PHYS, 0, 0x2000);
    write_rcb_addr(qts, GIB_PHYS + GIB_JUMBO_RX_RCB, JUMBO_RX_RING_PHYS);
    write_rcb_addr(qts, GIB_PHYS + GIB_RETURN_RCB, RETURN_RING_PHYS);
    qtest_writel(qts, GIB_PHYS + GIB_RETURN_RCB + 8, (2048u << 16));
    write_rcb_addr(qts, GIB_PHYS + GIB_EV_PROD_PTR, EV_PROD_PHYS);
    write_rcb_addr(qts, GIB_PHYS + GIB_RETURN_PROD_PTR, RETURN_PROD_PHYS);
    write_rcb_addr(qts, GIB_PHYS + GIB_TX_CONS_PTR, TX_CONS_PHYS);
}

/*
 * Hand the card a general-information-block address. The register window is
 * little-endian, so the half at the lower offset -- the one the public header
 * calls GENINFO_HI -- is the least significant one, which is where a guest
 * writing the pair puts the low half, as IRIX does.
 */
static void publish_gib_at(QTestState *qts, uint64_t gib)
{
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_GENINFO_HI, (uint32_t)gib);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_GENINFO_LO, gib >> 32);
}

static void publish_gib(QTestState *qts)
{
    publish_gib_at(qts, GIB_PHYS);
}

static void release_cpu(QTestState *qts)
{
    qtest_writel(qts, WINDOW_TIGON + TI_CPU_STATE, 0);
}

/* Drive the whole download-and-release handshake to the operational state. */
static void go_operational(QTestState *qts)
{
    enable_dma_big_endian(qts);
    build_gib(qts);
    publish_gib(qts);
    release_cpu(qts);
}

/*
 * The socket backend frames each packet with a four-byte network-order length,
 * the same way the other NIC qtests drive a socketpair-backed netdev.
 */
static void net_recv_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

static size_t net_recv_frame(int fd, uint8_t *buf, size_t buf_size)
{
    uint32_t hdr;
    size_t len;

    g_assert_cmpint(recv(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    len = ntohl(hdr);
    g_assert_cmpint(len, <=, buf_size);
    g_assert_cmpint(recv(fd, buf, len, 0), ==, (int)len);
    return len;
}

static void net_send_frame(int fd, const uint8_t *buf, size_t len)
{
    uint32_t hdr = htonl((uint32_t)len);

    g_assert_cmpint(send(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    g_assert_cmpint(send(fd, buf, len, 0), ==, (int)len);
}

/* Give the main loop passes to read the socket and deliver a frame. */
static uint32_t wait_return_prod(QTestState *qts)
{
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        if (qtest_readl(qts, RETURN_PROD_PHYS) != 0) {
            break;
        }
        g_usleep(5000);
    }
    return qtest_readl(qts, RETURN_PROD_PHYS);
}

static QTestState *tigon_init_netdev(int guest_fd)
{
    return qtest_initf("-M origin200,io-bridge=on "
                       "-netdev socket,fd=%d,id=n0 "
                       "-device tigon,addr=3,mac=" TIGON_MAC ",netdev=n0",
                       guest_fd);
}

static bool irq_asserted(QTestState *qts)
{
    return (qtest_readl(qts, WINDOW_TIGON + TI_MISC_HOST_CTL) &
            TI_MHC_INTSTATE) != 0;
}

/*
 * A general-information-block address whose upper half is not zero -- the
 * shape IRIX uses, where the top bits name the target widget and the low bits
 * are the offset. Assembling the two halves the wrong way round produces an
 * address naming the wrong widget and pointing past the end of memory, so
 * every ring the card reads through it comes back zero and the interface
 * carries nothing while looking configured. Every other test here publishes a
 * block below 4 GiB, where the two orderings differ only in bits that are
 * zero, so this is the one that can tell them apart.
 */
static void test_geninfo_upper_half(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    uint64_t gib = 0xa300000000000000ULL | GIB_PHYS;

    enable_dma_big_endian(qts);
    /*
     * Unlike the block addresses the other tests publish, this one is above
     * 4 GiB, so it leaves local space and crosses the bridge's byte swapper.
     * Let it through untouched, so what the test measures is the assembly of
     * the address rather than a swap the card has already been told to do.
     */
    qtest_writel(qts, BRIDGE_DEVICE(TIGON_SLOT),
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20) | DEV_SWAP_DIRECT);
    build_gib(qts);

    /*
     * The block stays where build_gib() put it: this machine's DMA passes the
     * offset through, so an address carrying widget bits on top still reaches
     * it. What has to survive is the assembly of the two halves.
     */
    publish_gib_at(qts, gib);
    release_cpu(qts);

    /* Firmware-up only happens if the card read the block it was given. */
    g_assert_cmphex(qtest_readl(qts, EVENT_RING_PHYS), ==,
                    (uint32_t)TI_EV_FIRMWARE_UP << 24);
    g_assert_cmphex(qtest_readl(qts, EV_PROD_PHYS), ==, 1);

    qtest_quit(qts);
}

static void test_firmware_up_requires_gib(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    enable_dma_big_endian(qts);
    build_gib(qts);

    /* Releasing the CPU with no GIB published must not post firmware-up. */
    release_cpu(qts);
    g_assert_cmphex(qtest_readl(qts, EVENT_RING_PHYS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, EV_PROD_PHYS), ==, 0);
    g_assert_false(irq_asserted(qts));

    qtest_quit(qts);
}

static void test_firmware_up_posts_event(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    enable_dma_big_endian(qts);
    build_gib(qts);
    publish_gib(qts);
    release_cpu(qts);

    /* Both conditions met: a firmware-up event and the interrupt. */
    g_assert_cmphex(qtest_readl(qts, EVENT_RING_PHYS), ==,
                    (uint32_t)TI_EV_FIRMWARE_UP << 24);
    g_assert_cmphex(qtest_readl(qts, EV_PROD_PHYS), ==, 1);
    g_assert_true(irq_asserted(qts));

    qtest_quit(qts);
}

/* A minimal well-formed ethernet frame the test chooses byte for byte. */
static const uint8_t test_frame[60] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,      /* broadcast dest */
    0x52, 0x54, 0x00, 0x12, 0x34, 0x56,      /* source = our MAC */
    0x08, 0x06,                              /* ethertype ARP */
    0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04,
};

static void test_transmit(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t got[2048];
    size_t len;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /* One transmit descriptor: a single fragment marked end-of-frame. */
    qtest_memwrite(qts, TX_BUF_PHYS, test_frame, sizeof(test_frame));
    write_rcb_addr(qts, TX_RING_PHYS + 0, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(test_frame) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);

    /* Ring the send doorbell: producer index 1. */
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);

    len = net_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(test_frame));
    g_assert_cmpmem(got, len, test_frame, sizeof(test_frame));

    /* The transmit consumer index advanced in host memory. */
    g_assert_cmphex(qtest_readl(qts, TX_CONS_PHYS), ==, 1);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A 60-byte IPv4 TCP SYN with its TCP checksum field left zero, the way IRIX's
 * eg hands a checksum-offloaded segment to the card. Offsets: ethernet 0..13,
 * IP header 14..33 (protocol 6, total length 40), TCP header 34..53 (checksum
 * at 50..51), then ethernet padding to the 60-byte minimum.
 */
static const uint8_t tcp_csum_frame[60] = {
    0x52, 0x54, 0x00, 0x12, 0x34, 0x99,      /* dest = a peer unicast */
    0x52, 0x54, 0x00, 0x12, 0x34, 0x56,      /* source = our MAC */
    0x08, 0x00,                              /* ethertype IP */
    0x45, 0x00, 0x00, 0x28,                  /* v4, IHL 5, total length 40 */
    0x00, 0x00, 0x40, 0x00,                  /* id 0, flags DF */
    0x40, 0x06, 0x00, 0x00,                  /* ttl 64, proto TCP, ip cksum 0 */
    0x0a, 0x00, 0x02, 0x0f,                  /* src 10.0.2.15 */
    0x0a, 0x00, 0x02, 0x02,                  /* dst 10.0.2.2 */
    0x13, 0x89, 0x13, 0x8a,                  /* src port 5001, dst 5002 */
    0x00, 0x00, 0x00, 0x01,                  /* seq */
    0x00, 0x00, 0x00, 0x00,                  /* ack */
    0x50, 0x02, 0x20, 0x00,                  /* data offset 5, SYN, window */
    0x00, 0x00, 0x00, 0x00,                  /* TCP checksum 0, urgent 0 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,      /* padding to 60 bytes */
};

/*
 * Fold the ones-complement sum over the TCP pseudo-header and the 20-byte TCP
 * header as the frame stands. A correct TCP checksum makes this 0xffff; a zero
 * checksum field makes it the complement of the real checksum instead.
 */
static uint16_t tcp_csum_fold(const uint8_t *f)
{
    uint32_t sum = 0;
    int i;

    sum += (f[26] << 8) | f[27];   /* src ip high */
    sum += (f[28] << 8) | f[29];   /* src ip low */
    sum += (f[30] << 8) | f[31];   /* dst ip high */
    sum += (f[32] << 8) | f[33];   /* dst ip low */
    sum += f[23];                  /* protocol (pseudo-header zero byte + it) */
    sum += 20;                     /* TCP length */
    for (i = 34; i < 54; i += 2) {
        sum += (f[i] << 8) | f[i + 1];
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)sum;
}

/*
 * Transmit checksum offload: a descriptor flagged TI_BDFLAG_TCP_UDP_CKSUM asks
 * the card to fill the L4 checksum the driver left zero. Without it every TCP
 * segment leaves with a zero checksum and no peer accepts it -- the reason an
 * eg TCP handshake never completes though UDP (optional checksum) survives.
 */
static void test_transmit_checksum_offload(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t got[2048];
    size_t len;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /* The frame carries a zero TCP checksum; the offload flag asks for it. */
    qtest_memwrite(qts, TX_BUF_PHYS, tcp_csum_frame, sizeof(tcp_csum_frame));
    write_rcb_addr(qts, TX_RING_PHYS + 0, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(tcp_csum_frame) << 16) |
                 TI_BDFLAG_END | TI_BDFLAG_TCP_UDP_CKSUM);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);

    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);

    len = net_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(tcp_csum_frame));

    /* The card filled the checksum in, and it verifies. */
    g_assert_cmpuint((got[50] << 8) | got[51], !=, 0);
    g_assert_cmphex(tcp_csum_fold(got), ==, 0xffff);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A real dst/src/ethertype frame, 61 bytes -- not a multiple of 4 -- so the
 * transmit-payload golden test below exercises the trailing partial word as
 * well as the plain aligned words.
 */
static const uint8_t test_frame_direct[61] = {
    0x52, 0x54, 0x00, 0x12, 0x34, 0x99,      /* dest = a peer unicast */
    0x52, 0x54, 0x00, 0x12, 0x34, 0x56,      /* source = our MAC */
    0x08, 0x00,                              /* ethertype IP */
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa,
};

/*
 * The transmit payload on the real (non-bypassed) direct-map DMA path -- a
 * driver's actual DMA-mapped sk_buff, unlike TX_BUF_PHYS above which sits
 * below the Bridge's translated window and so never exercises the swap at
 * all. Same two pairings as byte_order_check(), because the payload is
 * composed out of the identical tigon_dma_ld32() calls the descriptors use.
 * The frame is posted from a misaligned start (buf & 3 == 2) on top of a
 * length that is not a multiple of 4, forcing the leading- and
 * trailing-partial-word paths along with the plain aligned-word path.
 */
static void transmit_direct_map_check(bool bridge_transparent)
{
    int sv[2];
    QTestState *qts;
    uint8_t got[2048];
    size_t len;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    aim_window_at_bar(qts);
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);
    qtest_writel(qts, BRIDGE_DEVICE(TIGON_SLOT),
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20) |
                 (bridge_transparent ? DEV_SWAP_DIRECT : 0));
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_OPMODE,
                 bridge_transparent ?
                 (TI_OPMODE_BYTESWAP_BD | TI_OPMODE_WORDSWAP_BD |
                  TI_OPMODE_BYTESWAP_DATA) : 0);

    /* GIB and TX ring reached through the direct-map window too. */
    qtest_memset(qts, GIB_PHYS, 0, GIB_SIZE);
    qtest_memset(qts, TX_RING_PHYS, 0, 0x800);
    qtest_memset(qts, TX_CONS_PHYS, 0, 8);
    qtest_writeq(qts, GIB_PHYS + GIB_TX_RCB, IOVA(TX_RING_PHYS));
    qtest_writel(qts, GIB_PHYS + GIB_TX_RCB + 8,
                 (512u << 16) | TI_RCB_FLAG_HOST_RING);
    qtest_writeq(qts, GIB_PHYS + GIB_TX_CONS_PTR, IOVA(TX_CONS_PHYS));

    publish_gib_at(qts, IOVA(GIB_PHYS));
    release_cpu(qts);

    qtest_memwrite(qts, TX_BUF_DIRECT_PHYS, test_frame_direct,
                   sizeof(test_frame_direct));
    qtest_writeq(qts, TX_RING_PHYS + 0, IOVA(TX_BUF_DIRECT_PHYS));
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(test_frame_direct) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);

    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);

    len = net_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(test_frame_direct));
    g_assert_cmpmem(got, len, test_frame_direct, sizeof(test_frame_direct));

    g_assert_cmphex(qtest_readl(qts, TX_CONS_PHYS), ==, 1);

    qtest_quit(qts);
    close(sv[0]);
}

static void test_transmit_direct_map_transparent(const void *data)
{
    transmit_direct_map_check(true);
}

static void test_transmit_direct_map_corrects(const void *data)
{
    transmit_direct_map_check(false);
}

/* A standard receive-buffer descriptor: address, index and length. */
static void post_std_rx_buffer(QTestState *qts, unsigned slot, uint32_t phys,
                               uint16_t idx, uint32_t bytes)
{
    uint32_t desc = STD_RX_RING_PHYS + slot * 32;

    write_rcb_addr(qts, desc + 0, phys);
    qtest_writel(qts, desc + 8, ((uint32_t)idx << 16) | (bytes & 0xffff));
    qtest_writel(qts, desc + 12, (uint32_t)TI_BDTYPE_RECV_BD << 16);
}

static const uint8_t rx_frame_ours[64] = {
    0x52, 0x54, 0x00, 0x12, 0x34, 0x56,      /* dest = our MAC */
    0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,      /* source */
    0x08, 0x00,                              /* ethertype IP */
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
};

static const uint8_t rx_frame_other[64] = {
    0x02, 0x00, 0x00, 0x00, 0x00, 0x99,      /* dest = a different unicast */
    0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    0x08, 0x00,
    0x99, 0x98, 0x97, 0x96,
};

/*
 * A frame larger than the standard ring's buffers has to come back through
 * the jumbo ring and say that it did. A card that only ever serves the
 * standard ring cannot carry such a frame at all, and a driver will not offer
 * a frame size the card cannot take -- so the guest stays at the standard size
 * however it is configured, which then looks like a limitation of the guest.
 */
static void test_receive_jumbo_frame(const void *data)
{
    enum { JUMBO_LEN = 4000 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[JUMBO_LEN], landed[JUMBO_LEN];
    unsigned n;
    uint32_t word2, word3;

    memcpy(frame, rx_frame_ours, 14);
    for (n = 14; n < JUMBO_LEN; n++) {
        frame[n] = (uint8_t)(n * 5 + (n >> 7));
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /*
     * A standard buffer is posted as well, so what decides the ring is the
     * size of the frame rather than which ring happens to have room.
     */
    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    qtest_memset(qts, JUMBO_BUF_PHYS, 0, 16384);
    write_rcb_addr(qts, JUMBO_RX_RING_PHYS + 0, JUMBO_BUF_PHYS);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 8, (7u << 16) | 9000);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 12,
                 (uint32_t)TI_BDTYPE_RECV_JUMBO_BD << 16);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_JUMBORXPROD_IDX, 1);

    net_send_frame(sv[0], frame, sizeof(frame));

    g_assert_cmphex(wait_return_prod(qts), ==, 1);

    /* Whole, out of the jumbo buffer, and reported as a jumbo receive. */
    word2 = qtest_readl(qts, RETURN_RING_PHYS + 8);
    g_assert_cmphex(word2 >> 16, ==, 7);
    g_assert_cmphex(word2 & 0xffff, ==, JUMBO_LEN);
    word3 = qtest_readl(qts, RETURN_RING_PHYS + 12);
    g_assert_cmphex(word3 >> 16, ==, TI_BDTYPE_RECV_JUMBO_BD);
    g_assert_cmphex(word3 & TI_BDFLAG_JUMBO_RING, ==, TI_BDFLAG_JUMBO_RING);

    qtest_memread(qts, JUMBO_BUF_PHYS, landed, sizeof(frame));
    g_assert_cmpmem(landed, sizeof(frame), frame, sizeof(frame));

    /* The standard buffer was left untouched. */
    g_assert_cmphex(qtest_readl(qts, RX_BUF_PHYS), ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A frame offered before the firmware is up is refused, and the back end then
 * holds it until the card says it can take another. Bringing the firmware up
 * is one of the things that makes the card able to take one, so it has to say
 * so: a driver that posted its buffers first and rang no further doorbell
 * would otherwise never see the held frame.
 */
static void test_firmware_up_releases_a_held_frame(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t landed[sizeof(rx_frame_ours)];
    uint32_t word2;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    /* Everything except the CPU release, so the firmware stays down. */
    enable_dma_big_endian(qts);
    build_gib(qts);
    publish_gib(qts);

    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    /* Refused: there are buffers, but the card is not operational yet. */
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_cmphex(qtest_readl(qts, RETURN_PROD_PHYS), ==, 0);

    /* The only thing that changes. No further doorbell is written. */
    release_cpu(qts);

    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    word2 = qtest_readl(qts, RETURN_RING_PHYS + 8);
    g_assert_cmphex(word2 & 0xffff, ==, sizeof(rx_frame_ours));
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), rx_frame_ours,
                    sizeof(rx_frame_ours));

    qtest_quit(qts);
    close(sv[0]);
}

static void test_receive(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t landed[64];
    uint32_t word2;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));

    /* The return producer index advances once the frame is delivered. */
    g_assert_cmphex(wait_return_prod(qts), ==, 1);

    /* Return descriptor names the buffer index and the received length. */
    word2 = qtest_readl(qts, RETURN_RING_PHYS + 8);
    g_assert_cmphex(word2 >> 16, ==, 0);
    g_assert_cmphex(word2 & 0xffff, ==, sizeof(rx_frame_ours));

    /*
     * The return flags must carry END: IRIX's eg silently frees any receive
     * whose return descriptor has it clear, so without it no frame reaches IP.
     */
    g_assert_cmphex(qtest_readl(qts, RETURN_RING_PHYS + 12) & TI_BDFLAG_END,
                    ==, TI_BDFLAG_END);

    /* The frame landed in the posted buffer, byte for byte. */
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(rx_frame_ours));
    g_assert_cmpmem(landed, sizeof(rx_frame_ours),
                    rx_frame_ours, sizeof(rx_frame_ours));

    g_assert_true(irq_asserted(qts));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Build an IPv4 frame of a given protocol and total payload length, with the
 * L4 checksum field left zero. Returns the frame length. The caller owns the
 * buffer; it must hold 14 + 20 + l4_len bytes.
 */
static size_t build_ipv4_frame(uint8_t *f, uint8_t proto, size_t l4_len,
                               uint8_t seed)
{
    size_t ip_len = 20 + l4_len;
    size_t i;

    /*
     * Addressed to our own MAC so the same frame serves both directions: the
     * transmit path does not inspect the destination, and the receive filter
     * requires it.
     */
    memcpy(f, rx_frame_ours, 14);          /* dst = us, src, ethertype IPv4 */
    f[14] = 0x45;                          /* version 4, header length 5 */
    f[15] = 0x00;
    f[16] = (uint8_t)(ip_len >> 8);
    f[17] = (uint8_t)ip_len;
    f[18] = 0x00; f[19] = 0x00;            /* identification */
    f[20] = 0x40; f[21] = 0x00;            /* don't fragment */
    f[22] = 0x40;                          /* ttl */
    f[23] = proto;
    f[24] = 0x00; f[25] = 0x00;            /* header checksum, left zero */
    f[26] = 10; f[27] = 0; f[28] = 2; f[29] = 15;    /* 10.0.2.15 */
    f[30] = 10; f[31] = 0; f[32] = 2; f[33] = 2;     /* 10.0.2.2 */
    for (i = 0; i < l4_len; i++) {
        f[34 + i] = (uint8_t)(seed + i);
    }
    return 14 + ip_len;
}

/*
 * IP carriage in both directions, for each transport the guest actually uses.
 * The other tests drive ARP-shaped or hand-written frames; this one puts real
 * IPv4 datagrams -- TCP, UDP, and a bare non-transport protocol -- through the
 * transmit ring and back in through the receive ring, and compares them byte
 * for byte at both ends. A model that mangles the IP header, or that treats
 * one protocol differently from another on a path that should not care, fails
 * here rather than in a guest weeks later.
 */
static void test_ip_carriage_both_directions(const void *data)
{
    static const struct { uint8_t proto; size_t l4_len; const char *what; }
    cases[] = {
        { 6,   20,  "TCP" },       /* a bare TCP header */
        { 17,  8,   "UDP" },       /* a bare UDP header */
        { 17,  512, "UDP bulk" },  /* a datagram past one word-copy chunk */
        { 6,   1400, "TCP bulk" }, /* close to a full-size frame */
        { 89,  40,  "OSPF" },      /* neither TCP nor UDP: still just carried */
    };
    unsigned c;

    for (c = 0; c < ARRAY_SIZE(cases); c++) {
        int sv[2];
        QTestState *qts;
        uint8_t frame[2048];
        uint8_t got[2048];
        uint8_t landed[2048];
        size_t flen;
        uint32_t word2;

        flen = build_ipv4_frame(frame, cases[c].proto, cases[c].l4_len,
                                (uint8_t)(0x30 + c));

        g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
        qts = tigon_init_netdev(sv[1]);
        close(sv[1]);
        net_recv_timeout(sv[0], 5);
        go_operational(qts);

        /* Transmit: the frame leaves the card exactly as the guest wrote it. */
        qtest_memwrite(qts, TX_BUF_PHYS, frame, flen);
        write_rcb_addr(qts, TX_RING_PHYS + 0, TX_BUF_PHYS);
        qtest_writel(qts, TX_RING_PHYS + 8,
                     ((uint32_t)flen << 16) | TI_BDFLAG_END);
        qtest_writel(qts, TX_RING_PHYS + 12, 0);
        qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);

        g_assert_cmpint(net_recv_frame(sv[0], got, sizeof(got)), ==, (int)flen);
        g_assert_cmpmem(got, flen, frame, flen);

        /* Receive: the same datagram lands in the posted buffer intact. */
        qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
        post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
        qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);
        net_send_frame(sv[0], frame, flen);

        g_assert_cmphex(wait_return_prod(qts), ==, 1);
        word2 = qtest_readl(qts, RETURN_RING_PHYS + 8);
        g_assert_cmphex(word2 & 0xffff, ==, flen);
        qtest_memread(qts, RX_BUF_PHYS, landed, flen);
        g_assert_cmpmem(landed, flen, frame, flen);

        qtest_quit(qts);
        close(sv[0]);
    }
}

/*
 * A full-size frame, transmitted and compared byte for byte. The payload copy
 * runs over 1500 bytes rather than the 60 the other transmit tests use, so a
 * copy that is correct only for short frames, or only at the start of one,
 * fails here.
 */
static void test_transmit_full_size_frame(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[1500];
    uint8_t got[2048];
    size_t len;
    unsigned i;

    /* dst/src/ethertype, then a pattern no repeating word can match. */
    memcpy(frame, test_frame_direct, 14);
    for (i = 14; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 7 + (i >> 5));
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    qtest_memwrite(qts, TX_BUF_PHYS, frame, sizeof(frame));
    write_rcb_addr(qts, TX_RING_PHYS + 0, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(frame) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);

    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);

    len = net_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A frame flagged for checksum offload need not be the IPv4 packet the flag
 * claims: the guest composes the bytes. A header length below the 20-byte
 * minimum underflows the checksum helper's remaining-length arithmetic and
 * walks off the end of the frame, so the card must refuse to offload it and
 * send the frame as it stands.
 */
static void test_transmit_checksum_rejects_malformed(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[60];
    uint8_t got[2048];
    size_t len;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /* An IPv4 frame claiming header length 0 and a huge total length. */
    memcpy(frame, tcp_csum_frame, sizeof(frame));
    frame[14] = 0x40;               /* version 4, header length 0 */
    frame[16] = 0xff;               /* total length 65535 */
    frame[17] = 0xff;

    qtest_memwrite(qts, TX_BUF_PHYS, frame, sizeof(frame));
    write_rcb_addr(qts, TX_RING_PHYS + 0, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(frame) << 16) |
                 TI_BDFLAG_END | TI_BDFLAG_TCP_UDP_CKSUM);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);

    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);

    /* The frame goes out untouched rather than being parsed as IPv4. */
    len = net_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The same guard for a frame that is not IP at all: an ARP frame flagged for
 * offload must be sent as it stands, not parsed through the IPv4 path because
 * its first payload byte happens to look like a version nibble.
 */
static void test_transmit_checksum_ignores_non_ip(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t got[2048];
    size_t len;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /* test_frame is ARP (ethertype 0x0806), flagged for offload anyway. */
    qtest_memwrite(qts, TX_BUF_PHYS, test_frame, sizeof(test_frame));
    write_rcb_addr(qts, TX_RING_PHYS + 0, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(test_frame) << 16) |
                 TI_BDFLAG_END | TI_BDFLAG_TCP_UDP_CKSUM);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);

    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);

    len = net_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(test_frame));
    g_assert_cmpmem(got, len, test_frame, sizeof(test_frame));

    qtest_quit(qts);
    close(sv[0]);
}




/* An oversized frame must not be written past the buffer the guest posted. */
static void test_receive_clamps_to_buffer(const void *data)
{
    static const uint8_t big_frame[128] = {
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56,   /* dest = our MAC */
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x08, 0x00, 0xa1, 0xa2, 0xa3, 0xa4,
    };
    int sv[2];
    QTestState *qts;
    uint8_t guard[64];
    uint32_t word2;
    int i;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /* A 64-byte buffer, with the 64 bytes past it seeded to catch a spill. */
    qtest_memset(qts, RX_BUF_PHYS, 0, 128);
    qtest_memset(qts, RX_BUF_PHYS + 64, 0xa5, 64);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 64);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    net_send_frame(sv[0], big_frame, sizeof(big_frame));
    g_assert_cmphex(wait_return_prod(qts), ==, 1);

    /* The reported length is clamped to the buffer, not the frame. */
    word2 = qtest_readl(qts, RETURN_RING_PHYS + 8);
    g_assert_cmphex(word2 & 0xffff, ==, 64);

    /* Nothing landed past the buffer: the seeded bytes are intact. */
    qtest_memread(qts, RX_BUF_PHYS + 64, guard, sizeof(guard));
    for (i = 0; i < (int)sizeof(guard); i++) {
        g_assert_cmphex(guard[i], ==, 0xa5);
    }

    qtest_quit(qts);
    close(sv[0]);
}

static void test_receive_filters_other(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t landed[64];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 1, RX_BUF_PHYS + 0x1000, 1, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 2);

    /*
     * A frame for a different station, then one for us. Both are read from
     * the backend in order; only the second may reach the ring. If the filter
     * let the first through, the return producer would reach 2 and the buffer
     * would hold the wrong frame.
     */
    net_send_frame(sv[0], rx_frame_other, sizeof(rx_frame_other));
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));

    g_assert_cmphex(wait_return_prod(qts), ==, 1);

    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(rx_frame_ours));
    g_assert_cmpmem(landed, sizeof(rx_frame_ours),
                    rx_frame_ours, sizeof(rx_frame_ours));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A real dst/src/ethertype frame, 63 bytes -- not a multiple of 4 -- so the
 * receive-payload golden test below exercises the trailing partial word as
 * well as the plain aligned words.
 */
static const uint8_t rx_frame_direct[63] = {
    0x52, 0x54, 0x00, 0x12, 0x34, 0x56,      /* dest = our MAC */
    0x52, 0x54, 0x00, 0x12, 0x34, 0x99,      /* source = a peer unicast */
    0x08, 0x00,                              /* ethertype IP */
    0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa, 0xfb, 0xfc,
};

/*
 * The receive payload written through the real (non-bypassed) direct-map DMA
 * path -- the mirror of transmit_direct_map_check() for
 * tigon_dma_write_frame(). Same two pairings, same misaligned start (buf & 3
 * == 2) and non-multiple-of-4 length, forcing the leading- and
 * trailing-partial-word paths on the write side too.
 */
static void receive_direct_map_check(bool bridge_transparent, uint32_t opmode)
{
    int sv[2];
    QTestState *qts;
    uint8_t landed[64];
    uint32_t desc;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    aim_window_at_bar(qts);
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);
    qtest_writel(qts, BRIDGE_DEVICE(TIGON_SLOT),
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20) |
                 (bridge_transparent ? DEV_SWAP_DIRECT : 0));
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_OPMODE, opmode);

    /* GIB and the standard-receive/return rings reached through the window. */
    qtest_memset(qts, GIB_PHYS, 0, GIB_SIZE);
    qtest_memset(qts, STD_RX_RING_PHYS, 0, 0x800);
    qtest_memset(qts, RETURN_RING_PHYS, 0, 0x4000);
    qtest_memset(qts, RETURN_PROD_PHYS, 0, 8);
    qtest_writeq(qts, GIB_PHYS + GIB_STD_RX_RCB, IOVA(STD_RX_RING_PHYS));
    qtest_writeq(qts, GIB_PHYS + GIB_RETURN_RCB, IOVA(RETURN_RING_PHYS));
    qtest_writel(qts, GIB_PHYS + GIB_RETURN_RCB + 8, (2048u << 16));
    qtest_writeq(qts, GIB_PHYS + GIB_RETURN_PROD_PTR, IOVA(RETURN_PROD_PHYS));

    publish_gib_at(qts, IOVA(GIB_PHYS));
    release_cpu(qts);

    qtest_memset(qts, RX_BUF_DIRECT_PHYS, 0, 2048);
    desc = STD_RX_RING_PHYS + 0;
    qtest_writeq(qts, desc + 0, IOVA(RX_BUF_DIRECT_PHYS));
    qtest_writel(qts, desc + 8, 2048);
    qtest_writel(qts, desc + 12, (uint32_t)TI_BDTYPE_RECV_BD << 16);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    net_send_frame(sv[0], rx_frame_direct, sizeof(rx_frame_direct));

    g_assert_cmphex(wait_return_prod(qts), ==, 1);

    qtest_memread(qts, RX_BUF_DIRECT_PHYS, landed, sizeof(rx_frame_direct));
    g_assert_cmpmem(landed, sizeof(rx_frame_direct),
                    rx_frame_direct, sizeof(rx_frame_direct));

    /*
     * Through the Bridge, the return descriptor must still report the real
     * frame length and carry the END flag, so the guest driver delivers it.
     */
    {
        uint32_t w8 = qtest_readl(qts, RETURN_RING_PHYS + 8);
        uint32_t w12 = qtest_readl(qts, RETURN_RING_PHYS + 12);
        g_assert_cmphex(w8 & 0xffff, ==, sizeof(rx_frame_direct));
        g_assert_cmphex(w12 & TI_BDFLAG_END, ==, TI_BDFLAG_END);
    }

    qtest_quit(qts);
    close(sv[0]);
}

static void test_receive_direct_map_transparent(const void *data)
{
    receive_direct_map_check(true, TI_OPMODE_BYTESWAP_BD |
                             TI_OPMODE_WORDSWAP_BD | TI_OPMODE_BYTESWAP_DATA);
}

static void test_receive_direct_map_corrects(const void *data)
{
    receive_direct_map_check(false, 0);
}

/*
 * The byte-order arbiter, run on the real Bridge fabric with DMA through the
 * direct-map window whose swap the per-slot DEV_SWAP_DIRECT bit gates. Two
 * mutually exclusive pairings both have to land descriptors big-endian in
 * guest memory, each swapping exactly once:
 *
 *   transparent: the chip delivers big-endian (OPMODE byte-swap bits set) and
 *   the Bridge swap bit is set so the Bridge is a no-op;
 *
 *   corrected: the chip stays little-endian (OPMODE native) and the Bridge
 *   swap bit is clear so the Bridge performs the single correction -- the
 *   ISP1040's own working precedent behind this Bridge.
 *
 * Which pairing the real SGI board and IRIX use is set by platform code the
 * public driver never touches; this proves the emulator implements each
 * correctly, not which one the hardware wired.
 */
static void byte_order_check(bool bridge_transparent)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    uint8_t seen[4];

    aim_window_at_bar(qts);
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);
    qtest_writel(qts, BRIDGE_DEVICE(TIGON_SLOT),
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20) |
                 (bridge_transparent ? DEV_SWAP_DIRECT : 0));
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_OPMODE,
                 bridge_transparent ?
                 (TI_OPMODE_BYTESWAP_BD | TI_OPMODE_WORDSWAP_BD |
                  TI_OPMODE_BYTESWAP_DATA) : 0);

    /* GIB and event ring at direct-map addresses so the swap path is used. */
    qtest_memset(qts, GIB_PHYS, 0, GIB_SIZE);
    qtest_memset(qts, EVENT_RING_PHYS, 0, 0x800);
    qtest_memset(qts, EV_PROD_PHYS, 0, 8);
    qtest_writeq(qts, GIB_PHYS + GIB_EV_RCB, IOVA(EVENT_RING_PHYS));
    qtest_writeq(qts, GIB_PHYS + GIB_EV_PROD_PTR, IOVA(EV_PROD_PHYS));

    publish_gib_at(qts, IOVA(GIB_PHYS));
    release_cpu(qts);

    /*
     * The firmware-up event descriptor (eventx = 0x01000000) must read
     * big-endian in guest memory whichever pairing produced it: byte 0 holds
     * the event type. Any single missing, doubled, or half-width swap moves
     * the 0x01 out of byte 0.
     */
    qtest_memread(qts, EVENT_RING_PHYS, seen, 4);
    g_assert_cmphex(seen[0], ==, 0x01);
    g_assert_cmphex(seen[1], ==, 0x00);
    g_assert_cmphex(seen[2], ==, 0x00);
    g_assert_cmphex(seen[3], ==, 0x00);

    /* And the producer index, 1, in big-endian: the 0x01 lands in byte 3. */
    qtest_memread(qts, EV_PROD_PHYS, seen, 4);
    g_assert_cmphex(seen[0], ==, 0x00);
    g_assert_cmphex(seen[1], ==, 0x00);
    g_assert_cmphex(seen[2], ==, 0x00);
    g_assert_cmphex(seen[3], ==, 0x01);

    qtest_quit(qts);
}

static void test_byte_order_bridge_transparent(const void *data)
{
    byte_order_check(true);
}

static void test_byte_order_bridge_corrects(const void *data)
{
    byte_order_check(false);
}

/*
 * A guest that writes an out-of-range producer index must not hang the
 * emulator. The mailbox values are ring indices, so the walk reduces them into
 * range rather than looping over an index it can never reach; if the guard were
 * missing, an index of four billion would spin. The test writes such values and
 * then proves the machine still answers, which it could not do from inside a
 * runaway walk.
 */
static void test_hang_guard(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    go_operational(qts);

    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 0xffffffffu);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 0x40000000u);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 0xffffffffu);

    /* The walks terminated: the register file still answers. */
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_MISC_HOST_CTL) &
                    TI_MHC_CHIP_REV_MASK, ==, (uint32_t)TIGON_CHIP_REV << 28);

    qtest_quit(qts);
}

/*
 * The interrupt line the model exposes through the MISC_HOST_CTL state bit is
 * gated by the chip-level mask and cleared either by the mailbox write (the
 * driver's acknowledge, see tigon.c's TI_MB_HOSTINTR handler) or the
 * clear-interrupt bit. Firmware-up leaves it asserted; walk it through each
 * control, checking the mask before the ack so it still has a pending latch
 * to gate.
 */
static void test_interrupt_mask_ack(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    go_operational(qts);
    g_assert_true(irq_asserted(qts));

    /* The chip-level mask suppresses delivery without touching the latch. */
    qtest_writel(qts, WINDOW_TIGON + TI_MISC_HOST_CTL, TI_MHC_MASK_INTS);
    g_assert_false(irq_asserted(qts));
    qtest_writel(qts, WINDOW_TIGON + TI_MISC_HOST_CTL, 0);
    g_assert_true(irq_asserted(qts));

    /* The mailbox write is the acknowledge: either value drops the latch. */
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 1);
    g_assert_false(irq_asserted(qts));

    /* Clear-interrupt still drops a still-pending latch directly. */
    qtest_writel(qts, WINDOW_TIGON + TI_MISC_HOST_CTL, TI_MHC_CLEARINT);
    g_assert_false(irq_asserted(qts));

    qtest_quit(qts);
}

/*
 * The real acknowledge is the mailbox write, not GCR consumer-index
 * catch-up: IRIX's "eg" driver, observed live, never writes
 * TI_GCR_EVENTCONS_IDX or TI_GCR_RXRETURNCONS_IDX at all, so a model gated on
 * those consumers left the line stuck asserted forever against it. Pin both
 * halves of that: advancing the consumer index alone does nothing, and the
 * mailbox write -- with the value eg actually uses, 0 -- is what lowers the
 * line.
 */
static void test_interrupt_hostintr_ack(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    go_operational(qts);
    g_assert_true(irq_asserted(qts));

    /* Advancing the consumer index no longer has any effect on the line. */
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_EVENTCONS_IDX, 1);
    g_assert_true(irq_asserted(qts));

    /* IRIX's eg writes only 0 here -- that alone must ack. */
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    g_assert_false(irq_asserted(qts));

    qtest_quit(qts);
}

/*
 * The IRIX "eg" driver's actual acknowledge protocol, observed live across a
 * 33 million line trace: it never writes TI_GCR_EVENTCONS_IDX or
 * TI_GCR_RXRETURNCONS_IDX, and it writes TI_MB_HOSTINTR only with 0, never
 * the public NetBSD "ti" driver's mask-at-ISR-entry value 1 (if_ti.c
 * ti_intr()). Against the old consumer-index-catch-up model that left the
 * line stuck asserted forever: `eg` spins on the write, INTA never drops,
 * and `ifconfig` never returns. The mailbox write alone must ack, and a
 * later genuine completion must still raise the line again.
 */
static void test_interrupt_irix_ack_protocol(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);
    g_assert_true(irq_asserted(qts));   /* the firmware-up event */

    /* One receive completion, landing in the return ring. */
    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    g_assert_true(irq_asserted(qts));

    /* The driver's ISR: eg never touches the GCR consumers, only the mailbox. */
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);

    /* The line must drop from that write alone. */
    g_assert_false(irq_asserted(qts));

    /* A second, later completion still has to raise the line. */
    post_std_rx_buffer(qts, 1, RX_BUF_PHYS + 0x1000, 1, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 2);
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_cmphex(wait_return_prod(qts), ==, 2);
    g_assert_true(irq_asserted(qts));

    /* And the same ack-with-zero protocol quiets it again. */
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    g_assert_false(irq_asserted(qts));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The Layer-A register-swap bits and the descriptor-swap operating mode are
 * ordinary read/write state: the guest programs them and reads them back
 * unchanged. This pins that the model stores them rather than acting on them
 * in some hidden way.
 */
static void test_register_config_roundtrip(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    aim_window_at_bar(qts);

    /* MISC_HOST_CTL byte/word swap-enable bits round-trip (Layer A config). */
    qtest_writel(qts, WINDOW_TIGON + TI_MISC_HOST_CTL,
                 TI_MHC_BYTE_SWAP_ENB | TI_MHC_WORD_SWAP_ENB);
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_MISC_HOST_CTL) &
                    (TI_MHC_BYTE_SWAP_ENB | TI_MHC_WORD_SWAP_ENB), ==,
                    TI_MHC_BYTE_SWAP_ENB | TI_MHC_WORD_SWAP_ENB);

    /* GCR OPMODE reads back exactly what was written. */
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_OPMODE,
                 TI_OPMODE_BYTESWAP_BD | TI_OPMODE_WORDSWAP_BD |
                 TI_OPMODE_BYTESWAP_DATA);
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_GCR_OPMODE), ==,
                    TI_OPMODE_BYTESWAP_BD | TI_OPMODE_WORDSWAP_BD |
                    TI_OPMODE_BYTESWAP_DATA);

    qtest_quit(qts);
}

/*
 * Promiscuous mode is a command, not a register: the driver writes a
 * SET_PROMISC_MODE command into the command ring and rings the doorbell.
 * Once enabled, a frame for a different station -- filtered by
 * receive-filters-other above -- is accepted onto the ring.
 */
static void test_receive_promisc_accepts_other(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t landed[64];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    qtest_writel(qts, WINDOW_TIGON + TI_GCR_CMDRING,
                 (TI_CMD_SET_PROMISC_MODE << 24) |
                 (TI_CMD_CODE_PROMISC_ENB << 16));
    qtest_writel(qts, WINDOW_TIGON + TI_MB_CMDPROD_IDX, 1);

    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    net_send_frame(sv[0], rx_frame_other, sizeof(rx_frame_other));

    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(rx_frame_other));
    g_assert_cmpmem(landed, sizeof(rx_frame_other),
                    rx_frame_other, sizeof(rx_frame_other));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A completion that arrives while the host interrupt is masked must still
 * raise the line when the driver re-enables it. The Tigon 2 driver masks by
 * writing 1 to the interrupt mailbox and re-enables by writing 0; a frame
 * delivered in between sets the pending latch, and the re-enable -- distinct
 * from IRIX's zero-only acknowledge -- must not drop it.
 */
static void test_interrupt_reenable_preserves_completion(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);
    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    /* Mask and acknowledge: the line is held off. */
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 1);

    /* A frame arrives while masked: it sets the pending latch. */
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    g_assert_false(irq_asserted(qts));

    /* Re-enable: the completion that arrived while masked must raise INTA. */
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    g_assert_true(irq_asserted(qts));

    qtest_quit(qts);
    close(sv[0]);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/sn0/tigon/config-identity", NULL,
                        test_config_identity);
    qtest_add_data_func("/sn0/tigon/one-memory-bar", NULL,
                        test_one_memory_bar);
    qtest_add_data_func("/sn0/tigon/misc-host-ctl-chip-rev", NULL,
                        test_misc_host_ctl_chip_rev);
    qtest_add_data_func("/sn0/tigon/station-addr-par", NULL,
                        test_station_addr_par);
    qtest_add_data_func("/sn0/tigon/station-addr-eeprom", NULL,
                        test_station_addr_eeprom);
    qtest_add_data_func("/sn0/tigon/geninfo-upper-half", NULL,
                        test_geninfo_upper_half);
    qtest_add_data_func("/sn0/tigon/firmware-up-requires-gib", NULL,
                        test_firmware_up_requires_gib);
    qtest_add_data_func("/sn0/tigon/firmware-up-posts-event", NULL,
                        test_firmware_up_posts_event);
    qtest_add_data_func("/sn0/tigon/transmit", NULL, test_transmit);
    qtest_add_data_func("/sn0/tigon/ip-carriage-both-directions", NULL,
                        test_ip_carriage_both_directions);
    qtest_add_data_func("/sn0/tigon/transmit-full-size-frame", NULL,
                        test_transmit_full_size_frame);
    qtest_add_data_func("/sn0/tigon/transmit-checksum-offload", NULL,
                        test_transmit_checksum_offload);
    qtest_add_data_func("/sn0/tigon/transmit-checksum-rejects-malformed", NULL,
                        test_transmit_checksum_rejects_malformed);
    qtest_add_data_func("/sn0/tigon/transmit-checksum-ignores-non-ip", NULL,
                        test_transmit_checksum_ignores_non_ip);
    qtest_add_data_func("/sn0/tigon/transmit-direct-map-transparent", NULL,
                        test_transmit_direct_map_transparent);
    qtest_add_data_func("/sn0/tigon/transmit-direct-map-corrects", NULL,
                        test_transmit_direct_map_corrects);
    qtest_add_data_func("/sn0/tigon/receive", NULL, test_receive);
    qtest_add_data_func("/sn0/tigon/receive-jumbo-frame", NULL,
                        test_receive_jumbo_frame);
    qtest_add_data_func("/sn0/tigon/firmware-up-releases-a-held-frame", NULL,
                        test_firmware_up_releases_a_held_frame);
    qtest_add_data_func("/sn0/tigon/receive-filters-other", NULL,
                        test_receive_filters_other);
    qtest_add_data_func("/sn0/tigon/receive-clamps-to-buffer", NULL,
                        test_receive_clamps_to_buffer);
    qtest_add_data_func("/sn0/tigon/receive-direct-map-transparent", NULL,
                        test_receive_direct_map_transparent);
    qtest_add_data_func("/sn0/tigon/receive-direct-map-corrects", NULL,
                        test_receive_direct_map_corrects);
    qtest_add_data_func("/sn0/tigon/byte-order-bridge-transparent", NULL,
                        test_byte_order_bridge_transparent);
    qtest_add_data_func("/sn0/tigon/byte-order-bridge-corrects", NULL,
                        test_byte_order_bridge_corrects);
    qtest_add_data_func("/sn0/tigon/hang-guard", NULL, test_hang_guard);
    qtest_add_data_func("/sn0/tigon/interrupt-mask-ack", NULL,
                        test_interrupt_mask_ack);
    qtest_add_data_func("/sn0/tigon/interrupt-hostintr-ack", NULL,
                        test_interrupt_hostintr_ack);
    qtest_add_data_func("/sn0/tigon/interrupt-irix-ack-protocol", NULL,
                        test_interrupt_irix_ack_protocol);
    qtest_add_data_func("/sn0/tigon/register-config-roundtrip", NULL,
                        test_register_config_roundtrip);

    qtest_add_data_func("/sn0/tigon/receive-promisc-accepts-other", NULL,
                        test_receive_promisc_accepts_other);

    qtest_add_data_func("/sn0/tigon/interrupt-reenable-preserves-completion",
                        NULL, test_interrupt_reenable_preserves_completion);

    return g_test_run();
}
