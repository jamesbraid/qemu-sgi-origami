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

/* A fixed station address so PAR0/PAR1 and the EEPROM read are deterministic. */
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
#define DEV_SWAP_DIRECT     (1u << 19)   /* swap bit for the direct-map window */
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
#define TI_MB_SENDPROD_IDX  0x514
#define TI_MB_STDRXPROD_IDX 0x51c
#define TI_BDFLAG_END       0x0004
#define TI_BDFLAG_UCAST_PKT 0x0020
#define TI_BDFLAG_BCAST_PKT 0x0060

/* GCR registers exercised by the ring model. */
#define TI_GCR_GENINFO_HI   0x608
#define TI_GCR_GENINFO_LO   0x60c
#define TI_GCR_OPMODE       0x618
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

#define TI_RCB_FLAG_HOST_RING   0x0040
#define TI_EV_FIRMWARE_UP       0x01
#define TI_BDTYPE_RECV_BD       0x0002

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
    write_rcb_addr(qts, GIB_PHYS + GIB_RETURN_RCB, RETURN_RING_PHYS);
    qtest_writel(qts, GIB_PHYS + GIB_RETURN_RCB + 8, (2048u << 16));
    write_rcb_addr(qts, GIB_PHYS + GIB_EV_PROD_PTR, EV_PROD_PHYS);
    write_rcb_addr(qts, GIB_PHYS + GIB_RETURN_PROD_PTR, RETURN_PROD_PHYS);
    write_rcb_addr(qts, GIB_PHYS + GIB_TX_CONS_PTR, TX_CONS_PHYS);
}

static void publish_gib(QTestState *qts)
{
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_GENINFO_HI, 0);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_GENINFO_LO, GIB_PHYS);
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

static void test_firmware_up_requires_gib(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    enable_dma_big_endian(qts);
    build_gib(qts);

    /* Releasing the CPU without publishing the GIB must not post firmware-up. */
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

    /* The frame landed in the posted buffer, byte for byte. */
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(rx_frame_ours));
    g_assert_cmpmem(landed, sizeof(rx_frame_ours),
                    rx_frame_ours, sizeof(rx_frame_ours));

    g_assert_true(irq_asserted(qts));

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

    qtest_writel(qts, WINDOW_TIGON + TI_GCR_GENINFO_HI,
                 (uint32_t)(IOVA_PREFIX >> 32));
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_GENINFO_LO, GIB_PHYS);
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
    qtest_add_data_func("/sn0/tigon/firmware-up-requires-gib", NULL,
                        test_firmware_up_requires_gib);
    qtest_add_data_func("/sn0/tigon/firmware-up-posts-event", NULL,
                        test_firmware_up_posts_event);
    qtest_add_data_func("/sn0/tigon/transmit", NULL, test_transmit);
    qtest_add_data_func("/sn0/tigon/receive", NULL, test_receive);
    qtest_add_data_func("/sn0/tigon/receive-filters-other", NULL,
                        test_receive_filters_other);
    qtest_add_data_func("/sn0/tigon/byte-order-bridge-transparent", NULL,
                        test_byte_order_bridge_transparent);
    qtest_add_data_func("/sn0/tigon/byte-order-bridge-corrects", NULL,
                        test_byte_order_bridge_corrects);

    return g_test_run();
}
