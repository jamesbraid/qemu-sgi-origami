/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Origin 200 BCM570x Bridge DMA tests. Big-endian scalar access through plain
 * DMA preserves descriptor and status words without swap-window dispatch;
 * scalar bytes alone cannot identify the route, so the Bridge trace does.
 * Plain-route payloads are copied verbatim for the measured BCM5701 setup;
 * transformation with DMA-swap controls clear remains unknown on hardware.
 * The received TCP descriptor checks big-endian 32-bit checksum-offload
 * words:
 * little-endian access byte-swaps them, causing IRIX to reject offloaded
 * frames as "incomplete ethernet frame" and stall TCP. bcm570x-test.c covers
 * x86 without a Bridge: little-endian words and endian-controlled payload
 * swap. These tests cover the Bridge route absent from that suite.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include <sys/socket.h>
#include "sgi-test.h"

#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_IO + ((uint64_t)BRIDGE_WIDGET << 24))

#define BCM_MAC         "52:54:00:bc:57:01"

#define MACHINE_WITH_BRIDGE  \
    "-M origin200 -device bcm570x,addr=3,mac=" BCM_MAC

/*
 * The card's slot: devices 0 and 1 are the base I/O board's SCSI adapters and
 * device 2 is IOC3; 3 is the first free slot, with the first 1 MB window.
 */
#define BCM_SLOT        3
#define WINDOW_BCM      (BRIDGE_BASE + 0x700000)

/* PCI type 0 configuration space through the Bridge. */
#define BRIDGE_CFG0     0x020000
#define CFG0_STRIDE     0x1000
#define CFG0(dev)       (BRIDGE_BASE + BRIDGE_CFG0 + (dev) * CFG0_STRIDE)

/* The Bridge's own per-slot device register, low half of its 8-byte slot. */
#define REG_DATA            4
#define BRIDGE_DEVICE(n)    (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)

/* Where the card's BAR is parked in PCI memory space (1 MB aligned). */
#define ADAPTER_MMIO        0x10000000u

/*
 * A 64-bit direct-map DMA address. The low bits reach system memory; the
 * prefix selects the direct-map path whose byte swap the Bridge applies for a
 * slot left at the value-preserving default. This is where the
 * swap-window cost lies, and the path plain space must replace. A low
 * bare address
 * would not do: local space never crosses the swapper, so it could not
 * distinguish the routes.
 */
#define IOVA_PREFIX         0xa500000000000000ULL
#define IOVA(phys)          (IOVA_PREFIX | (uint32_t)(phys))

/* Card registers, offsets within the BAR (public bge/tg3 map). */
#define BCM_RX_MODE             0x0468
#define BCM_RXMODE_ENABLE       0x2
#define BCM_HCC_MODE            0x3c00
#define BCM_HCC_STATUSBLK_ADDR_HI 0x3c38
#define BCM_HCC_STATUSBLK_ADDR_LO 0x3c3c
#define BCM_HCC_RX_COAL_TICKS   0x3c08
#define BCM_HCC_RX_MAX_COAL_BDS 0x3c10
#define BCM_HCC_ENABLE          0x2
#define BCM_RX_STD_RCB_HI       0x2450
#define BCM_RX_STD_RCB_LO       0x2454
#define BCM_RX_STD_RCB_MAXLEN   0x2458
#define BCM_MBX_RX_STD_PROD_LO  0x026c
/*
 * The jumbo receive ring's control block (public bge/tg3 map; PG105 Table
 * 341, offset 0x2448, page 438). Max_Len there is unused for jumbo rings,
 * only Disable Ring (bit 1) and Extended RX Enable (bit 0) do anything.
 */
#define BCM_RX_JUMBO_RCB_HI      0x2440
#define BCM_RX_JUMBO_RCB_LO      0x2444
#define BCM_RX_JUMBO_RCB_MAXLEN  0x2448
#define BCM_RX_JUMBO_RCB_NICADDR 0x244c
#define BCM_MBX_RX_JUMBO_PROD_LO 0x0274
#define BCM_RCB_FLAG_DISABLE_RING 0x0002u
#define BCM_SRAM_WINDOW_BASE    0x8000  /* SRAM through the BAR's high half */
#define BCM_SRAM_RX_RETURN_RCB  0x0200
/* nicNoMoreRXBDs, PG105 page 110 Table 49 / page 114 Table 54. */
#define BCM_SRAM_STATS_NO_MORE_RXBDS 0x0898
#define BCM_MAC_ADDR_0_HIGH     0x0410
#define BCM_MAC_ADDR_0_LOW      0x0414
#define BCM_MISC_CFG            0x6804
#define BCM_MISCCFG_RESET       0x00000001u

/* Completed-descriptor layout in the return ring (public bge/tg3 map). */
#define BCM_RXBD_SIZE           32
#define BCM_RXBD_IDX_LEN        0x08    /* index (31:16), length (15:0) */
#define BCM_RXBD_TYPE_FLAGS     0x0c    /* type (31:16), flags (15:0) */
#define BCM_RXBD_CSUM           0x10    /* IP sum 31:16; TCP/UDP sum 15:0 */
#define BCM_RXBDFLAG_TCP_UDP_CKSUM 0x0001u
#define BCM_RXBDFLAG_IP_CKSUM   0x0002u
#define BCM_RXBDFLAG_END        0x0004u
#define BCM_RXBDFLAG_JUMBO_RING 0x0020u
#define BCM_ETH_FCS_LEN         4

/* Status-block idx[0]: rx_producer at byte offset 16 (tg3 layout). */
#define BCM_STATUS_RX_PROD_OFF  16

/* Guest-physical scratch, mirroring the eg test's low-RAM layout. */
#define STATUS_PHYS         0x00220000u
#define STD_RING_PHYS       0x00230000u
#define RET_RING_PHYS       0x00240000u
#define RX_BUF_PHYS         0x00250000u
#define STD_BUF_SIZE        1536u
#define JUMBO_RING_PHYS     0x00260000u
#define JUMBO_BUF_PHYS      0x00270000u
#define JUMBO_BUF_LEN       9146u
#define STD_MAXLEN_1518     1518u
#define BIG_FRAME_LEN       9010u
#define SMALL_JUMBO_BUF_LEN 2000u
#define OVERRUN_FRAME_LEN   3000u
#define FITTING_FRAME_LEN   1800u

static void bar_writel(QTestState *qts, uint32_t off, uint32_t val)
{
    qtest_writel(qts, WINDOW_BCM + off, val);
}

static uint32_t bar_readl(QTestState *qts, uint32_t off)
{
    return qtest_readl(qts, WINDOW_BCM + off);
}

static void assert_station_bytes(QTestState *qts, const uint8_t expected[8])
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readb(qts, WINDOW_BCM +
                                   BCM_MAC_ADDR_0_HIGH + i), ==, expected[i]);
    }
}

static void aim_window_at_bar(QTestState *qts)
{
    qtest_writel(qts, CFG0(BCM_SLOT) + PCI_BAR0, ADAPTER_MMIO);
    qtest_writel(qts, CFG0(BCM_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DEVICE(BCM_SLOT),
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
}

static unsigned count_lines_with(const char *log, const char *needle)
{
    unsigned n = 0;
    const char *p = log;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}

static void test_station_address_byte_reads(const void *data)
{
    static const uint8_t expected[] = {
        0x00, 0x00, 0x52, 0x54, 0x00, 0xbc, 0x57, 0x01,
    };
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    aim_window_at_bar(qts);

    g_assert_cmphex(bar_readl(qts, BCM_MAC_ADDR_0_HIGH), ==, 0x00005254u);
    g_assert_cmphex(bar_readl(qts, BCM_MAC_ADDR_0_LOW), ==, 0x00bc5701u);
    assert_station_bytes(qts, expected);

    bar_writel(qts, BCM_MAC_ADDR_0_HIGH - 4, 0x55667788);
    bar_writel(qts, BCM_MAC_ADDR_0_LOW + 4, 0x11223344);
    g_assert_cmphex(qtest_readb(qts, WINDOW_BCM +
                               BCM_MAC_ADDR_0_HIGH - 1), ==, 0);
    g_assert_cmphex(qtest_readb(qts, WINDOW_BCM +
                               BCM_MAC_ADDR_0_LOW + 4), ==, 0);
    g_assert_cmphex(qtest_readw(qts, WINDOW_BCM +
                               BCM_MAC_ADDR_0_HIGH), ==, 0);

    qtest_writeb(qts, WINDOW_BCM + BCM_MAC_ADDR_0_HIGH, 0);
    qtest_writew(qts, WINDOW_BCM + BCM_MAC_ADDR_0_LOW, 0);
    qtest_writeb(qts, WINDOW_BCM + BCM_MAC_ADDR_0_LOW + 4, 0);
    g_assert_cmphex(bar_readl(qts, BCM_MAC_ADDR_0_HIGH), ==, 0x00005254u);
    g_assert_cmphex(bar_readl(qts, BCM_MAC_ADDR_0_LOW), ==, 0x00bc5701u);
    g_assert_cmphex(bar_readl(qts, BCM_MAC_ADDR_0_HIGH - 4), ==, 0x55667788);
    g_assert_cmphex(bar_readl(qts, BCM_MAC_ADDR_0_LOW + 4), ==, 0x11223344);

    bar_writel(qts, BCM_MAC_ADDR_0_HIGH, 0x0000a1a2);
    bar_writel(qts, BCM_MAC_ADDR_0_LOW, 0xa3a4a5a6);
    bar_writel(qts, BCM_MISC_CFG, BCM_MISCCFG_RESET);
    g_assert_cmphex(bar_readl(qts, BCM_MAC_ADDR_0_HIGH), ==, 0x00005254u);
    g_assert_cmphex(bar_readl(qts, BCM_MAC_ADDR_0_LOW), ==, 0x00bc5701u);
    assert_station_bytes(qts, expected);

    qtest_quit(qts);
}

/*
 * The guest reads and writes 32-bit descriptor words big-endian on this
 * machine, so the test lays them out the same way.
 */
static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static uint32_t bd_word_be(QTestState *qts, uint64_t phys, unsigned int off)
{
    uint8_t w[4];

    qtest_memread(qts, phys + off, w, sizeof(w));
    return ((uint32_t)w[0] << 24) | ((uint32_t)w[1] << 16) |
           ((uint32_t)w[2] << 8) | w[3];
}

/*
 * The socket backend frames each packet with a four-byte network-order
 * length, as in the other NIC qtests using a socketpair-backed netdev.
 */
static void net_send_frame(int fd, const uint8_t *buf, size_t len)
{
    uint32_t hdr = htonl((uint32_t)len);

    g_assert_cmpint(send(fd, &hdr, sizeof(hdr), 0), ==, (int)sizeof(hdr));
    g_assert_cmpint(send(fd, buf, len, 0), ==, (int)len);
}

/*
 * The device writes a completed descriptor into the return ring before it
 * raises any interrupt. Poll for the descriptor length word to become
 * non-zero. That holds in either byte order, so this wait does
 * not itself depend on the layout the test is checking. The caller then reads
 * the descriptor and asserts the byte order. A swapped word fails with
 * a legible value instead of a silent timeout.
 */
static void wait_for_completed_descriptor(QTestState *qts)
{
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        if (bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_IDX_LEN) != 0) {
            return;
        }
        qtest_clock_step(qts, 1 * 1000 * 1000);
        g_usleep(2000);
    }
    g_assert_cmphex(bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_IDX_LEN), !=, 0);
}

/*
 * A minimal Ethernet/IPv4/TCP frame with valid header checksums lets
 * the card's receive checksum offload verify them. It reports the
 * results in the completed descriptor. Broadcom-source to the card's
 * own MAC, arbitrary destination; the addresses do not matter to the
 * descriptor byte order.
 */
#define RX_FRAME_LEN 60
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

static void build_rx_frame(uint8_t *f)
{
    static const uint8_t head[] = {
        0x52, 0x54, 0x00, 0xbc, 0x57, 0x01,     /* destination: the card */
        0x52, 0x54, 0x00, 0x11, 0x22, 0x33,     /* source */
        0x08, 0x00,                             /* IPv4 */
    };
    uint32_t pseudo;
    uint16_t sum;

    memset(f, 0, RX_FRAME_LEN);
    memcpy(f, head, sizeof(head));

    f[14] = 0x45;                   /* version 4, header length 5 words */
    f[16] = 0x00; f[17] = 40;       /* total length: 20 IP + 20 TCP */
    f[22] = 64;                     /* time to live */
    f[23] = 6;                      /* TCP */
    f[26] = 10; f[27] = 0; f[28] = 2; f[29] = 15;   /* source address */
    f[30] = 10; f[31] = 0; f[32] = 2; f[33] = 2;    /* destination */
    sum = inet_csum(f + 14, 20, 0);
    f[24] = sum >> 8; f[25] = sum & 0xff;           /* IP checksum */

    f[34] = 0x30; f[35] = 0x39;     /* source port */
    f[36] = 0x00; f[37] = 0x17;     /* destination port */
    f[46] = 0x50;                   /* data offset: five words */
    f[47] = 0x10;                   /* ACK */
    f[48] = 0x20; f[49] = 0x00;     /* window */
    pseudo = 0;
    pseudo += ((uint32_t)f[26] << 8) | f[27];
    pseudo += ((uint32_t)f[28] << 8) | f[29];
    pseudo += ((uint32_t)f[30] << 8) | f[31];
    pseudo += ((uint32_t)f[32] << 8) | f[33];
    pseudo += 6;                    /* protocol */
    pseudo += 20;                   /* TCP length */
    sum = inet_csum(f + 34, 20, pseudo);
    f[50] = sum >> 8; f[51] = sum & 0xff;           /* TCP checksum */
}

/*
 * A received frame's completed descriptor, read as the guest reads it on this
 * machine: big-endian. Every 32-bit word (length, type/flags, and
 * checksum-offload results) must land in that layout. A descriptor field
 * left on the model's little-endian accessor writes it byte-swapped; the
 * offload fields in particular then reach the driver with the frame-complete
 * and checksum-valid bits in the wrong half, which a full IRIX boot showed as
 * the driver rejecting every offloaded frame and stalling TCP. A byte-exact
 * round trip cannot see this: the payload is a byte stream either way, so
 * only a descriptor whose fields carry meaning in their bit positions exposes
 * it.
 */
static void test_receive_descriptor_big_endian(const void *data)
{
    QTestState *qts;
    uint8_t frame[RX_FRAME_LEN];
    uint8_t got[RX_FRAME_LEN];
    uint8_t bd[BCM_RXBD_SIZE];
    uint32_t type_flags, csum, idx_len;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M origin200 -netdev socket,fd=%d,id=n0 "
                      "-device bcm570x,addr=3,netdev=n0,mac=" BCM_MAC, sv[1]);
    aim_window_at_bar(qts);

    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_HI, IOVA(STATUS_PHYS) >> 32);
    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_LO, IOVA(STATUS_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_HI, IOVA(STD_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_STD_RCB_LO, IOVA(STD_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 0,
               IOVA(RET_RING_PHYS) >> 32);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 4,
               IOVA(RET_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 8,
               64 << 16);
    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(qts, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    /* Post one receive buffer: the guest writes the descriptor big-endian. */
    memset(bd, 0, sizeof(bd));
    put32be(bd + 0, IOVA(RX_BUF_PHYS) >> 32);
    put32be(bd + 4, IOVA(RX_BUF_PHYS) & 0xffffffff);
    qtest_memwrite(qts, STD_RING_PHYS, bd, sizeof(bd));
    qtest_memset(qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(qts, STATUS_PHYS, 0, 0x50);
    bar_writel(qts, BCM_MBX_RX_STD_PROD_LO, 1);

    build_rx_frame(frame);
    net_send_frame(sv[0], frame, sizeof(frame));
    wait_for_completed_descriptor(qts);

    /* The completed descriptor's words, read big-endian as the guest does. */
    idx_len = bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_IDX_LEN);
    type_flags = bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_TYPE_FLAGS);
    csum = bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_CSUM);

    /* Length in the low half; the frame plus its four-byte check sequence. */
    g_assert_cmphex(idx_len & 0xffff, ==, RX_FRAME_LEN + BCM_ETH_FCS_LEN);
    /*
     * Frame-end and both offload-valid marks are in the flags (low) half;
     * the type (high) half stays clear. A byte-swapped word fails both.
     */
    g_assert_cmphex(type_flags & 0xffff, ==,
                    BCM_RXBDFLAG_END | BCM_RXBDFLAG_IP_CKSUM |
                    BCM_RXBDFLAG_TCP_UDP_CKSUM);
    g_assert_cmphex(type_flags >> 16, ==, 0);
    /*
     * The IP sum the card computed sits in the checksum word's high half; a
     * byte-swapped word would strand it in the low half.
     */
    g_assert_cmpuint(csum >> 16, !=, 0);

    /* The payload is a byte stream, delivered verbatim to the buffer. */
    qtest_memread(qts, RX_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));

    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

/*
 * The plain DMA route's byte-lane halfword correction (bcm570x_dma_st16:
 * offset xor 2) lands a 16-bit status field in the other half of its
 * 4-byte-aligned word from its nominal offset, so the receive producer
 * reads back at BCM_STATUS_RX_PROD_OFF ^ 2, not BCM_STATUS_RX_PROD_OFF.
 */
static uint16_t status_rx_prod_be(QTestState *qts)
{
    uint8_t w[2];

    qtest_memread(qts, STATUS_PHYS + (BCM_STATUS_RX_PROD_OFF ^ 2), w,
                  sizeof(w));
    return ((uint16_t)w[0] << 8) | w[1];
}

/*
 * Post one receive descriptor: the buffer's host address, then the ring
 * index and buffer length packed into the upper and lower halves of the
 * third word, then flags in the fourth, as IRIX posts jumbo
 * buffers with. The standard ring never reads the length or flags words
 * back, so posting them harmlessly there too keeps one helper for both
 * rings.
 */
static void post_rx_descriptor(QTestState *qts, uint64_t ring_phys,
                               unsigned int idx, uint64_t buf_phys,
                               uint16_t buf_len, uint16_t flags)
{
    uint8_t bd[BCM_RXBD_SIZE];

    memset(bd, 0, sizeof(bd));
    put32be(bd + 0, IOVA(buf_phys) >> 32);
    put32be(bd + 4, IOVA(buf_phys) & 0xffffffff);
    put32be(bd + BCM_RXBD_IDX_LEN, ((uint32_t)idx << 16) | buf_len);
    put32be(bd + BCM_RXBD_TYPE_FLAGS, flags);
    qtest_memwrite(qts, ring_phys + (uint64_t)idx * BCM_RXBD_SIZE, bd,
                  sizeof(bd));
}

/*
 * A frame long enough to force the jumbo ring, addressed to the card, with
 * an ethertype the checksum offload will not try to parse. These tests
 * check which ring a frame lands on, not the offload fields.
 */
static void build_big_rx_frame(uint8_t *f, size_t len)
{
    static const uint8_t head[] = {
        0x52, 0x54, 0x00, 0xbc, 0x57, 0x01,     /* destination: the card */
        0x52, 0x54, 0x00, 0x11, 0x22, 0x33,     /* source */
        0xff, 0xff,                             /* not IPv4 */
    };
    size_t i;

    memset(f, 0, len);
    memcpy(f, head, sizeof(head));
    for (i = sizeof(head); i < len; i++) {
        f[i] = (uint8_t)i;
    }
}

/*
 * A dropped frame produces no completion to poll for. Pump the same
 * clock-and-sleep cycle wait_for_completed_descriptor uses, for a fixed,
 * generous span, then let the caller assert nothing arrived.
 */
static void pump_settle(QTestState *qts)
{
    int attempt;

    for (attempt = 0; attempt < 40; attempt++) {
        qtest_clock_step(qts, 1 * 1000 * 1000);
        g_usleep(2000);
    }
}

/*
 * The jumbo RCB's own Max_Len field (offset 0x2448, bits 31-16) is unused by
 * hardware (PG105 Table 341, page 438; page 91: "This Max_Len field is
 * ignored in the Receive Jumbo Producer Ring"). Ring selection compares the
 * frame only against the standard ring's Max_Len, and a frame too big for
 * that goes to the jumbo ring whenever it is not disabled and has a host
 * base. IRIX 6.5.30 leaves it this way at MTU 9000: it posts
 * the jumbo RCB's host and NIC addresses but never touches 0x2448.
 *
 * Pins ring selection, not arrival timing, so the floor is zeroed: the
 * completed descriptor is checked the instant it lands.
 */
static void test_jumbo_ring_selected_by_standard_length(const void *data)
{
    QTestState *qts;
    uint8_t frame[BIG_FRAME_LEN];
    uint8_t got[BIG_FRAME_LEN];
    uint32_t idx_len, type_flags, addr_hi, addr_lo;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M origin200 -netdev socket,fd=%d,id=n0 "
                      "-device bcm570x,addr=3,netdev=n0,mac=" BCM_MAC
                      ",rx-arrival-latency-us=0", sv[1]);
    aim_window_at_bar(qts);

    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_HI, IOVA(STATUS_PHYS) >> 32);
    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_LO, IOVA(STATUS_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_HI, IOVA(STD_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_STD_RCB_LO, IOVA(STD_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_MAXLEN, STD_MAXLEN_1518 << 16);
    bar_writel(qts, BCM_RX_JUMBO_RCB_HI, IOVA(JUMBO_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_JUMBO_RCB_LO, IOVA(JUMBO_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_JUMBO_RCB_NICADDR, 0x00001000u);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 0,
               IOVA(RET_RING_PHYS) >> 32);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 4,
               IOVA(RET_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 8,
               64 << 16);
    /* One completed descriptor per update
     * [57XX-PG105-R, printed page 454]. */
    bar_writel(qts, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(qts, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    post_rx_descriptor(qts, STD_RING_PHYS, 0, RX_BUF_PHYS, 0, 0);
    qtest_memset(qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(qts, STATUS_PHYS, 0, 0x50);
    bar_writel(qts, BCM_MBX_RX_STD_PROD_LO, 1);

    /* Leave 0x2448 at its reset value of 0, like IRIX. */
    post_rx_descriptor(qts, JUMBO_RING_PHYS, 0, JUMBO_BUF_PHYS,
                       JUMBO_BUF_LEN, BCM_RXBDFLAG_JUMBO_RING);
    bar_writel(qts, BCM_MBX_RX_JUMBO_PROD_LO, 1);
    g_assert_cmphex(bar_readl(qts, BCM_MBX_RX_JUMBO_PROD_LO), ==, 1);
    g_assert_cmphex(bar_readl(qts, BCM_RX_JUMBO_RCB_MAXLEN), ==, 0);

    build_big_rx_frame(frame, sizeof(frame));
    net_send_frame(sv[0], frame, sizeof(frame));
    wait_for_completed_descriptor(qts);

    idx_len = bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_IDX_LEN);
    type_flags = bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_TYPE_FLAGS);
    addr_hi = bd_word_be(qts, RET_RING_PHYS, 0);
    addr_lo = bd_word_be(qts, RET_RING_PHYS, 4);

    g_assert_cmphex(idx_len & 0xffff, ==, BIG_FRAME_LEN + BCM_ETH_FCS_LEN);
    g_assert_cmphex(type_flags & 0xffff, ==,
                    BCM_RXBDFLAG_END | BCM_RXBDFLAG_JUMBO_RING);
    g_assert_cmphex(((uint64_t)addr_hi << 32) | addr_lo, ==,
                    IOVA(JUMBO_BUF_PHYS));
    g_assert_cmphex(status_rx_prod_be(qts), ==, 1);

    qtest_memread(qts, JUMBO_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), frame, sizeof(frame));

    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

/*
 * Disable Ring (bit 1 of 0x2448) is the only thing that turns the jumbo ring
 * off; a driver sets it "until we're actually ready to start using this
 * ring" (OpenBSD bge, quoted in the jumbo-RCB evidence report) rather than
 * clearing Max_Len, which hardware ignores anyway. A frame too big for the
 * standard ring must be dropped while that bit is set, and the standard
 * ring must keep working afterward.
 *
 * Pins ring selection, not arrival timing, so the floor is zeroed: the
 * completed descriptor is checked the instant it lands.
 */
static void test_jumbo_ring_disabled_drops_oversize(const void *data)
{
    QTestState *qts;
    uint8_t big[BIG_FRAME_LEN];
    uint8_t small[100];
    uint8_t got[100];
    uint32_t idx_len, type_flags;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M origin200 -netdev socket,fd=%d,id=n0 "
                      "-device bcm570x,addr=3,netdev=n0,mac=" BCM_MAC
                      ",rx-arrival-latency-us=0", sv[1]);
    aim_window_at_bar(qts);

    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_HI, IOVA(STATUS_PHYS) >> 32);
    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_LO, IOVA(STATUS_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_HI, IOVA(STD_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_STD_RCB_LO, IOVA(STD_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_MAXLEN, STD_MAXLEN_1518 << 16);
    bar_writel(qts, BCM_RX_JUMBO_RCB_HI, IOVA(JUMBO_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_JUMBO_RCB_LO, IOVA(JUMBO_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_JUMBO_RCB_NICADDR, 0x00001000u);
    bar_writel(qts, BCM_RX_JUMBO_RCB_MAXLEN, BCM_RCB_FLAG_DISABLE_RING);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 0,
               IOVA(RET_RING_PHYS) >> 32);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 4,
               IOVA(RET_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 8,
               64 << 16);
    /* One completed descriptor per update
     * [57XX-PG105-R, printed page 454]. */
    bar_writel(qts, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(qts, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    post_rx_descriptor(qts, STD_RING_PHYS, 0, RX_BUF_PHYS, 0, 0);
    qtest_memset(qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(qts, STATUS_PHYS, 0, 0x50);
    bar_writel(qts, BCM_MBX_RX_STD_PROD_LO, 1);

    post_rx_descriptor(qts, JUMBO_RING_PHYS, 0, JUMBO_BUF_PHYS,
                       JUMBO_BUF_LEN, BCM_RXBDFLAG_JUMBO_RING);
    bar_writel(qts, BCM_MBX_RX_JUMBO_PROD_LO, 1);
    g_assert_cmphex(bar_readl(qts, BCM_MBX_RX_JUMBO_PROD_LO), ==, 1);

    build_big_rx_frame(big, sizeof(big));
    net_send_frame(sv[0], big, sizeof(big));
    pump_settle(qts);

    g_assert_cmphex(bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_IDX_LEN), ==, 0);
    g_assert_cmphex(status_rx_prod_be(qts), ==, 0);

    build_big_rx_frame(small, sizeof(small));
    net_send_frame(sv[0], small, sizeof(small));
    wait_for_completed_descriptor(qts);

    idx_len = bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_IDX_LEN);
    type_flags = bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_TYPE_FLAGS);
    g_assert_cmphex(idx_len & 0xffff, ==, sizeof(small) + BCM_ETH_FCS_LEN);
    g_assert_cmphex(type_flags & 0xffff, ==, BCM_RXBDFLAG_END);
    g_assert_cmphex(status_rx_prod_be(qts), ==, 1);

    qtest_memread(qts, RX_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), small, sizeof(small));

    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

/*
 * Wait for the status block's receive producer to reach a value. Unlike
 * wait_for_completed_descriptor this follows the ring past its first slot,
 * so a test can check a second frame's arrival.
 */
static void wait_for_rx_producer(QTestState *qts, uint16_t want)
{
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        if (status_rx_prod_be(qts) >= want) {
            return;
        }
        qtest_clock_step(qts, 1 * 1000 * 1000);
        g_usleep(2000);
    }
    g_assert_cmphex(status_rx_prod_be(qts), ==, want);
}

/*
 * The jumbo ring enabled and addressed but carrying no posted buffer. An
 * over-length frame arriving in that state has no descriptor to go to, and
 * cannot take the standard ring's: a frame "greater than the Max_Len field
 * of Receive Standard Producer Ring will consume a BD from Receive Jumbo
 * Producer Ring" [57XX-PG105-R, printed page 92]. The standard ring keeps
 * being served all the same, which is the step that fails if the model
 * parks the frame instead: the back end stops reading and the standard
 * frame behind it never arrives.
 *
 * The manual says nothing about what becomes of the frame itself. The
 * pages that look like they answer it are about the chip's own on-chip
 * buffer pool, not a host ring. The discard pinned by the last step is the
 * model's approximation. jumbo-shortage-discards-without-waiting is what
 * separates it from a bounded hold; this case steps the clock, so it cannot.
 *
 * Pins the empty-ring disposition, not arrival timing, so the floor is
 * zeroed: each completion is checked the instant it lands.
 *
 * The manual also says the chip counts this rather than latching it
 * (`nicNoMoreRXBDs`, PG105 page 110); the test reads that counter through the
 * Bridge before and after the dropped frame and asserts it moved by exactly
 * one, then holds through the delivered frame and the belated jumbo post.
 */
static void test_jumbo_ring_unprovisioned_drops_oversize(const void *data)
{
    QTestState *qts;
    uint8_t big[BIG_FRAME_LEN];
    uint8_t small[100];
    uint8_t got[100];
    uint8_t jumbo_got[BIG_FRAME_LEN];
    uint8_t zeros[BIG_FRAME_LEN];
    uint32_t idx_len, type_flags;
    int sv[2];

    memset(zeros, 0, sizeof(zeros));

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M origin200 -netdev socket,fd=%d,id=n0 "
                      "-device bcm570x,addr=3,netdev=n0,mac=" BCM_MAC
                      ",rx-arrival-latency-us=0", sv[1]);
    aim_window_at_bar(qts);

    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_HI, IOVA(STATUS_PHYS) >> 32);
    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_LO, IOVA(STATUS_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_HI, IOVA(STD_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_STD_RCB_LO, IOVA(STD_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_MAXLEN, STD_MAXLEN_1518 << 16);
    /* Addresses only, like IRIX: 0x2448 stays at its reset value of 0. */
    bar_writel(qts, BCM_RX_JUMBO_RCB_HI, IOVA(JUMBO_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_JUMBO_RCB_LO, IOVA(JUMBO_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_JUMBO_RCB_NICADDR, 0x00001000u);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 0,
               IOVA(RET_RING_PHYS) >> 32);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 4,
               IOVA(RET_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 8,
               64 << 16);
    /* One completed descriptor per update
     * [57XX-PG105-R, printed page 454]. */
    bar_writel(qts, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(qts, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    /* Two standard buffers let the ring outlive the dropped frame. */
    post_rx_descriptor(qts, STD_RING_PHYS, 0, RX_BUF_PHYS, 0, 0);
    post_rx_descriptor(qts, STD_RING_PHYS, 1, RX_BUF_PHYS + STD_BUF_SIZE, 0, 0);
    qtest_memset(qts, RX_BUF_PHYS, 0, 2 * STD_BUF_SIZE);
    qtest_memset(qts, RET_RING_PHYS, 0, 2 * BCM_RXBD_SIZE);
    qtest_memset(qts, STATUS_PHYS, 0, 0x50);
    qtest_memset(qts, JUMBO_BUF_PHYS, 0, BIG_FRAME_LEN);
    bar_writel(qts, BCM_MBX_RX_STD_PROD_LO, 2);

    /* The jumbo producer mailbox is never written: the ring stays empty. */
    g_assert_cmphex(bar_readl(qts, BCM_MBX_RX_JUMBO_PROD_LO), ==, 0);
    g_assert_cmphex(bar_readl(qts, BCM_RX_JUMBO_RCB_MAXLEN), ==, 0);
    g_assert_cmphex(bar_readl(qts, BCM_SRAM_WINDOW_BASE +
                               BCM_SRAM_STATS_NO_MORE_RXBDS), ==, 0);

    build_big_rx_frame(big, sizeof(big));
    net_send_frame(sv[0], big, sizeof(big));
    pump_settle(qts);

    /* No completion, no producer movement, and neither buffer written. */
    g_assert_cmphex(bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_IDX_LEN), ==, 0);
    g_assert_cmphex(status_rx_prod_be(qts), ==, 0);
    qtest_memread(qts, RX_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), zeros, sizeof(got));

    /*
     * The chip counts this rather than latching it (PG105 page 110): one
     * frame dropped for want of a posted jumbo descriptor, one count, read
     * back across the Origin 200 Bridge like any other register.
     */
    g_assert_cmphex(bar_readl(qts, BCM_SRAM_WINDOW_BASE +
                               BCM_SRAM_STATS_NO_MORE_RXBDS), ==, 1);

    /* The receiver is not latched: the frame behind it still arrives. */
    build_big_rx_frame(small, sizeof(small));
    net_send_frame(sv[0], small, sizeof(small));
    wait_for_rx_producer(qts, 1);

    idx_len = bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_IDX_LEN);
    type_flags = bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_TYPE_FLAGS);
    g_assert_cmphex(idx_len & 0xffff, ==, sizeof(small) + BCM_ETH_FCS_LEN);
    g_assert_cmphex(idx_len >> 16, ==, 0);
    g_assert_cmphex(type_flags & 0xffff, ==, BCM_RXBDFLAG_END);
    g_assert_cmphex(status_rx_prod_be(qts), ==, 1);
    qtest_memread(qts, RX_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), small, sizeof(small));

    /*
     * Posting a jumbo buffer now brings nothing back. A model that parked
     * the oversize frame would complete it here, minutes of guest time after
     * the wire carried it.
     */
    post_rx_descriptor(qts, JUMBO_RING_PHYS, 0, JUMBO_BUF_PHYS,
                       JUMBO_BUF_LEN, BCM_RXBDFLAG_JUMBO_RING);
    bar_writel(qts, BCM_MBX_RX_JUMBO_PROD_LO, 1);
    pump_settle(qts);

    g_assert_cmphex(status_rx_prod_be(qts), ==, 1);
    g_assert_cmphex(bd_word_be(qts, RET_RING_PHYS + BCM_RXBD_SIZE,
                               BCM_RXBD_IDX_LEN), ==, 0);
    qtest_memread(qts, JUMBO_BUF_PHYS, jumbo_got, sizeof(jumbo_got));
    g_assert_cmpmem(jumbo_got, sizeof(jumbo_got), zeros, sizeof(zeros));

    /* Neither the delivered frame nor the late post is this condition. */
    g_assert_cmphex(bar_readl(qts, BCM_SRAM_WINDOW_BASE +
                               BCM_SRAM_STATS_NO_MORE_RXBDS), ==, 1);

    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

/*
 * A jumbo frame bigger than the buffer behind its own posted descriptor must
 * not be delivered: the descriptor's length field is the driver's statement
 * of what it set aside for that buffer, the same way the standard ring's
 * Max_Len is. The model drops the frame without writing a completion or
 * advancing the return-ring producer. The standard ring has no comparable
 * per-descriptor check. The model also leaves the jumbo ring's
 * consumer where it was rather than stepping past the descriptor: the driver
 * never said the buffer was used, so a later frame that does fit must still
 * be able to land in it. That is asserted here indirectly, by sending one:
 * if the consumer had advanced anyway, the ring would look empty to it and
 * the fitting frame would stall rather than complete.
 *
 * Pins overrun and consumer-position handling, not arrival timing, so the
 * floor is zeroed: the completed descriptor is checked the instant it lands.
 */
static void test_jumbo_frame_overruns_descriptor(const void *data)
{
    QTestState *qts;
    uint8_t big[OVERRUN_FRAME_LEN];
    uint8_t fits[FITTING_FRAME_LEN];
    uint8_t got[FITTING_FRAME_LEN];
    uint32_t idx_len, type_flags, addr_hi, addr_lo;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M origin200 -netdev socket,fd=%d,id=n0 "
                      "-device bcm570x,addr=3,netdev=n0,mac=" BCM_MAC
                      ",rx-arrival-latency-us=0", sv[1]);
    aim_window_at_bar(qts);

    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_HI, IOVA(STATUS_PHYS) >> 32);
    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_LO, IOVA(STATUS_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_HI, IOVA(STD_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_STD_RCB_LO, IOVA(STD_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_MAXLEN, STD_MAXLEN_1518 << 16);
    bar_writel(qts, BCM_RX_JUMBO_RCB_HI, IOVA(JUMBO_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_JUMBO_RCB_LO, IOVA(JUMBO_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_JUMBO_RCB_NICADDR, 0x00001000u);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 0,
               IOVA(RET_RING_PHYS) >> 32);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 4,
               IOVA(RET_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 8,
               64 << 16);
    /* One completed descriptor per update
     * [57XX-PG105-R, printed page 454]. */
    bar_writel(qts, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(qts, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    post_rx_descriptor(qts, STD_RING_PHYS, 0, RX_BUF_PHYS, 0, 0);
    qtest_memset(qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(qts, STATUS_PHYS, 0, 0x50);
    bar_writel(qts, BCM_MBX_RX_STD_PROD_LO, 1);

    /* One jumbo descriptor, its buffer far smaller than the frame to come. */
    post_rx_descriptor(qts, JUMBO_RING_PHYS, 0, JUMBO_BUF_PHYS,
                       SMALL_JUMBO_BUF_LEN, BCM_RXBDFLAG_JUMBO_RING);
    bar_writel(qts, BCM_MBX_RX_JUMBO_PROD_LO, 1);

    build_big_rx_frame(big, sizeof(big));
    net_send_frame(sv[0], big, sizeof(big));
    pump_settle(qts);

    g_assert_cmphex(bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_IDX_LEN), ==, 0);
    g_assert_cmphex(status_rx_prod_be(qts), ==, 0);

    /* The same descriptor, untouched, takes a frame that actually fits it. */
    build_big_rx_frame(fits, sizeof(fits));
    net_send_frame(sv[0], fits, sizeof(fits));
    wait_for_completed_descriptor(qts);

    idx_len = bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_IDX_LEN);
    type_flags = bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_TYPE_FLAGS);
    addr_hi = bd_word_be(qts, RET_RING_PHYS, 0);
    addr_lo = bd_word_be(qts, RET_RING_PHYS, 4);

    g_assert_cmphex(idx_len & 0xffff, ==, FITTING_FRAME_LEN + BCM_ETH_FCS_LEN);
    g_assert_cmphex(type_flags & 0xffff, ==,
                    BCM_RXBDFLAG_END | BCM_RXBDFLAG_JUMBO_RING);
    g_assert_cmphex(((uint64_t)addr_hi << 32) | addr_lo, ==,
                    IOVA(JUMBO_BUF_PHYS));
    g_assert_cmphex(status_rx_prod_be(qts), ==, 1);

    qtest_memread(qts, JUMBO_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), fits, sizeof(fits));

    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

/*
 * Enabling host coalescing arms the periodic status tick, and stepping the
 * one received frame under a max-coalesced count of one makes the model post
 * one status block to the programmed host address. Its address is a
 * direct-map
 * IOVA, so on the swap route it would appear in the Bridge's lane-swap trace.
 * On the plain route it appears in the plain-translate trace instead. The
 * status block's first word checks the byte order. The model writes the
 * driver's big-endian layout, so the UPDATED flag's bytes must read
 * 00 00 00 01 in guest memory.
 *
 * Pins the DMA route and the byte order, not arrival timing, so the floor is
 * zeroed: the completion reaches the coalescing block the instant the frame
 * lands, and the count of one reports it.
 */
static void test_dma_rides_plain_route(const void *data)
{
    char *logfile = NULL;
    char *log = NULL;
    char needle[64];
    uint8_t frame[RX_FRAME_LEN];
    uint8_t bd[BCM_RXBD_SIZE];
    uint8_t word0[4];
    QTestState *qts;
    int sv[2];
    int fd;

    fd = g_file_open_tmp("bcm570x-dma-route-XXXXXX", &logfile, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M origin200 -netdev socket,fd=%d,id=n0 "
                      "-device bcm570x,addr=3,netdev=n0,mac=" BCM_MAC
                      ",rx-arrival-latency-us=0"
                      " -d trace:sgi_bridge_lane_swap_read,"
                      "trace:sgi_bridge_lane_swap_write,"
                      "trace:sgi_bridge_dma_xlate_plain -D %s",
                      sv[1], logfile);
    aim_window_at_bar(qts);

    /* Status block and receive rings at direct-map IOVAs. */
    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_HI, IOVA(STATUS_PHYS) >> 32);
    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_LO, IOVA(STATUS_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_HI, IOVA(STD_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_STD_RCB_LO, IOVA(STD_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 0,
               IOVA(RET_RING_PHYS) >> 32);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 4,
               IOVA(RET_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 8,
               64 << 16);

    qtest_memset(qts, STATUS_PHYS, 0, 0x50);
    qtest_memset(qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    bar_writel(qts, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(qts, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    memset(bd, 0, sizeof(bd));
    put32be(bd + 0, IOVA(RX_BUF_PHYS) >> 32);
    put32be(bd + 4, IOVA(RX_BUF_PHYS) & 0xffffffff);
    qtest_memwrite(qts, STD_RING_PHYS, bd, sizeof(bd));
    bar_writel(qts, BCM_MBX_RX_STD_PROD_LO, 1);

    build_rx_frame(frame);
    net_send_frame(sv[0], frame, sizeof(frame));
    wait_for_completed_descriptor(qts);

    /* Big-endian regime: the UPDATED flag lands as the driver reads it. */
    qtest_memread(qts, STATUS_PHYS, word0, sizeof(word0));
    g_assert_cmphex(word0[0], ==, 0x00);
    g_assert_cmphex(word0[1], ==, 0x00);
    g_assert_cmphex(word0[2], ==, 0x00);
    g_assert_cmphex(word0[3], ==, 0x01);

    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);

    g_assert_true(g_file_get_contents(logfile, &log, NULL, NULL));

    /*
     * The card's transfer took the plain route (this is also the liveness
     * control: a dead trace would fail here, not pass anything vacuously)...
     */
    snprintf(needle, sizeof(needle), "plain addr 0x%" PRIx64,
             (uint64_t)IOVA(STATUS_PHYS));
    g_assert_cmpuint(count_lines_with(log, needle), >, 0);
    /* ...and its address never crossed the swap window. */
    snprintf(needle, sizeof(needle), "lane swap read addr 0x%" PRIx64,
             (uint64_t)IOVA(STATUS_PHYS));
    g_assert_cmpuint(count_lines_with(log, needle), ==, 0);
    snprintf(needle, sizeof(needle), "lane swap write addr 0x%" PRIx64,
             (uint64_t)IOVA(STATUS_PHYS));
    g_assert_cmpuint(count_lines_with(log, needle), ==, 0);

    g_free(log);
    unlink(logfile);
    g_free(logfile);
}

/*
 * Poll the status block's receive producer, stepping the virtual clock
 * as wait_for_completed_descriptor does so a coalescing timer can expire.
 */
static void wait_for_status_rx_prod(QTestState *qts, uint16_t want)
{
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        if (status_rx_prod_be(qts) == want) {
            return;
        }
        qtest_clock_step(qts, 1 * 1000 * 1000);
        g_usleep(2000);
    }
    g_assert_cmphex(status_rx_prod_be(qts), ==, want);
}

/*
 * What makes the chip write the status block, on the machine the card is
 * really in. The guide gives four criteria and no others [57XX-PG105-R,
 * printed page 63], each of them counted in buffer descriptors or measured by
 * a coalescing timer, and a register left at zero turns its criterion off. So
 * a frame received with neither receive criterion armed is completed into the
 * return ring and never announced, however long the guest waits. There is no
 * clock of the chip's own behind the status block. Arming the count
 * makes the next frame post one update, whose producer names a
 * completion already in the ring.
 */
static void test_status_block_update_criteria(const void *data)
{
    uint8_t frame[RX_FRAME_LEN];
    uint8_t word0[4];
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M origin200 -netdev socket,fd=%d,id=n0 "
                      "-device bcm570x,addr=3,netdev=n0,mac=" BCM_MAC, sv[1]);
    aim_window_at_bar(qts);

    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_HI, IOVA(STATUS_PHYS) >> 32);
    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_LO, IOVA(STATUS_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_HI, IOVA(STD_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_STD_RCB_LO, IOVA(STD_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_MAXLEN, STD_BUF_SIZE << 16);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 0,
               IOVA(RET_RING_PHYS) >> 32);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 4,
               IOVA(RET_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 8,
               64 << 16);

    /* Neither receive criterion armed: no descriptor count or tick budget. */
    bar_writel(qts, BCM_HCC_RX_MAX_COAL_BDS, 0);
    bar_writel(qts, BCM_HCC_RX_COAL_TICKS, 0);
    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(qts, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    qtest_memset(qts, RET_RING_PHYS, 0, 2 * BCM_RXBD_SIZE);
    qtest_memset(qts, STATUS_PHYS, 0, 0x50);
    post_rx_descriptor(qts, STD_RING_PHYS, 0, RX_BUF_PHYS, STD_BUF_SIZE, 0);
    bar_writel(qts, BCM_MBX_RX_STD_PROD_LO, 1);

    build_rx_frame(frame);
    net_send_frame(sv[0], frame, sizeof(frame));
    wait_for_completed_descriptor(qts);

    /* The completion is in the ring and the host has not been told. */
    pump_settle(qts);
    qtest_memread(qts, STATUS_PHYS, word0, sizeof(word0));
    g_assert_cmphex(word0[0], ==, 0);
    g_assert_cmphex(word0[1], ==, 0);
    g_assert_cmphex(word0[2], ==, 0);
    g_assert_cmphex(word0[3], ==, 0);
    g_assert_cmphex(status_rx_prod_be(qts), ==, 0);

    /* One completed descriptor per update, and the next frame posts one. */
    bar_writel(qts, BCM_HCC_RX_MAX_COAL_BDS, 1);
    post_rx_descriptor(qts, STD_RING_PHYS, 1, RX_BUF_PHYS, STD_BUF_SIZE, 0);
    bar_writel(qts, BCM_MBX_RX_STD_PROD_LO, 2);
    net_send_frame(sv[0], frame, sizeof(frame));
    wait_for_status_rx_prod(qts, 2);

    qtest_memread(qts, STATUS_PHYS, word0, sizeof(word0));
    g_assert_cmphex(word0[3], ==, 0x01);
    /* The producer names a completion already written. */
    g_assert_cmphex(bd_word_be(qts, RET_RING_PHYS + BCM_RXBD_SIZE,
                               BCM_RXBD_IDX_LEN) & 0xffff, ==,
                    RX_FRAME_LEN + BCM_ETH_FCS_LEN);

    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

/*
 * Poll for a completion without advancing the virtual clock. Every qtest
 * command runs a pass of the main loop, so the back end still gets to read
 * its socket; what does not happen is any timer firing.
 */
static uint16_t rx_producer_without_clock(QTestState *qts, uint16_t want)
{
    int attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        if (status_rx_prod_be(qts) >= want) {
            break;
        }
        g_usleep(5000);
    }
    return status_rx_prod_be(qts);
}

/*
 * Jumbo shortage discards immediately. Advancing the virtual clock past a
 * possible hold gives the same final state, so keep it frozen. Holding the
 * frame refuses it and stops backend reads; the following standard frame
 * cannot arrive. Immediate discard keeps reads active and delivers that
 * frame on the next main-loop pass. This is an approximation: the BCM570x
 * datasheet's jumbo receive contract does not prescribe discard timing.
 * A bounded hold delivered fewer bytes and would fail this test.
 */
static void test_jumbo_shortage_discards_without_waiting(const void *data)
{
    QTestState *qts;
    uint8_t big[BIG_FRAME_LEN];
    uint8_t small[100];
    uint8_t got[100];
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M origin200 -netdev socket,fd=%d,id=n0 "
                      "-device bcm570x,addr=3,netdev=n0,mac=" BCM_MAC
                      ",rx-arrival-latency-us=0", sv[1]);
    aim_window_at_bar(qts);

    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_HI, IOVA(STATUS_PHYS) >> 32);
    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_LO, IOVA(STATUS_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_HI, IOVA(STD_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_STD_RCB_LO, IOVA(STD_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_MAXLEN, STD_MAXLEN_1518 << 16);
    /* Addresses only, like IRIX: 0x2448 stays at its reset value of 0. */
    bar_writel(qts, BCM_RX_JUMBO_RCB_HI, IOVA(JUMBO_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_JUMBO_RCB_LO, IOVA(JUMBO_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_JUMBO_RCB_NICADDR, 0x00001000u);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 0,
               IOVA(RET_RING_PHYS) >> 32);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 4,
               IOVA(RET_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 8,
               64 << 16);
    bar_writel(qts, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(qts, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    post_rx_descriptor(qts, STD_RING_PHYS, 0, RX_BUF_PHYS, 0, 0);
    qtest_memset(qts, RX_BUF_PHYS, 0, STD_BUF_SIZE);
    qtest_memset(qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(qts, STATUS_PHYS, 0, 0x50);
    bar_writel(qts, BCM_MBX_RX_STD_PROD_LO, 1);

    /* The jumbo producer mailbox is never written: the ring stays empty. */
    build_big_rx_frame(big, sizeof(big));
    net_send_frame(sv[0], big, sizeof(big));
    build_big_rx_frame(small, sizeof(small));
    net_send_frame(sv[0], small, sizeof(small));

    /* No qtest_clock_step: only a discard gets the second frame in. */
    g_assert_cmphex(rx_producer_without_clock(qts, 1), ==, 1);
    qtest_memread(qts, RX_BUF_PHYS, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), small, sizeof(small));
    g_assert_cmphex(bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_IDX_LEN) & 0xffff,
                    ==, sizeof(small) + BCM_ETH_FCS_LEN);

    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

/*
 * The standard ring is addressed but has no posted buffer; the jumbo ring is
 * provisioned. Length and standard Max_Len select the standard ring, so a
 * standard frame cannot use a jumbo buffer and is discarded immediately.
 * Freeze virtual time: advancing past a possible hold hides the difference,
 * while only immediate discard lets the queued jumbo frame arrive. A bounded
 * hold delivered fewer bytes on the MTU 1500 path (BCM570x datasheet).
 * nicNoMoreRXBDs has one counter with no per-ring companion (PG105 page 110,
 * Table 49; page 114, Table 54). Check the same offset used by jumbo shortage
 * before and after the drop; the following jumbo delivery adds no count.
 */
static void test_standard_shortage_discards_without_waiting(const void *data)
{
    QTestState *qts;
    uint8_t big[BIG_FRAME_LEN];
    uint8_t small[100];
    uint8_t got[BIG_FRAME_LEN];
    uint8_t zeros[BIG_FRAME_LEN];
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M origin200 -netdev socket,fd=%d,id=n0 "
                      "-device bcm570x,addr=3,netdev=n0,mac=" BCM_MAC
                      ",rx-arrival-latency-us=0", sv[1]);
    aim_window_at_bar(qts);

    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_HI, IOVA(STATUS_PHYS) >> 32);
    bar_writel(qts, BCM_HCC_STATUSBLK_ADDR_LO, IOVA(STATUS_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_HI, IOVA(STD_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_STD_RCB_LO, IOVA(STD_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_STD_RCB_MAXLEN, STD_MAXLEN_1518 << 16);
    /* Addresses only, like IRIX: 0x2448 stays at its reset value of 0. */
    bar_writel(qts, BCM_RX_JUMBO_RCB_HI, IOVA(JUMBO_RING_PHYS) >> 32);
    bar_writel(qts, BCM_RX_JUMBO_RCB_LO, IOVA(JUMBO_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_RX_JUMBO_RCB_NICADDR, 0x00001000u);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 0,
               IOVA(RET_RING_PHYS) >> 32);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 4,
               IOVA(RET_RING_PHYS) & 0xffffffff);
    bar_writel(qts, BCM_SRAM_WINDOW_BASE + BCM_SRAM_RX_RETURN_RCB + 8,
               64 << 16);
    bar_writel(qts, BCM_HCC_RX_MAX_COAL_BDS, 1);
    bar_writel(qts, BCM_HCC_MODE, BCM_HCC_ENABLE);
    bar_writel(qts, BCM_RX_MODE, BCM_RXMODE_ENABLE);

    /* A jumbo buffer only; the standard producer mailbox is never written. */
    post_rx_descriptor(qts, JUMBO_RING_PHYS, 0, JUMBO_BUF_PHYS,
                       JUMBO_BUF_LEN, BCM_RXBDFLAG_JUMBO_RING);
    qtest_memset(qts, JUMBO_BUF_PHYS, 0, BIG_FRAME_LEN);
    qtest_memset(qts, RX_BUF_PHYS, 0, STD_BUF_SIZE);
    qtest_memset(qts, RET_RING_PHYS, 0, BCM_RXBD_SIZE);
    qtest_memset(qts, STATUS_PHYS, 0, 0x50);
    bar_writel(qts, BCM_MBX_RX_JUMBO_PROD_LO, 1);
    g_assert_cmphex(bar_readl(qts, BCM_MBX_RX_STD_PROD_LO), ==, 0);
    g_assert_cmphex(bar_readl(qts, BCM_SRAM_WINDOW_BASE +
                               BCM_SRAM_STATS_NO_MORE_RXBDS), ==, 0);

    build_big_rx_frame(small, sizeof(small));
    net_send_frame(sv[0], small, sizeof(small));
    build_big_rx_frame(big, sizeof(big));
    net_send_frame(sv[0], big, sizeof(big));

    /* No qtest_clock_step: only a discard gets the second frame in. */
    g_assert_cmphex(rx_producer_without_clock(qts, 1), ==, 1);
    qtest_memread(qts, JUMBO_BUF_PHYS, got, sizeof(big));
    g_assert_cmpmem(got, sizeof(big), big, sizeof(big));
    g_assert_cmphex(bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_IDX_LEN) & 0xffff,
                    ==, sizeof(big) + BCM_ETH_FCS_LEN);
    g_assert_cmphex(bd_word_be(qts, RET_RING_PHYS, BCM_RXBD_TYPE_FLAGS) &
                    BCM_RXBDFLAG_JUMBO_RING, ==, BCM_RXBDFLAG_JUMBO_RING);
    /* The standard buffer was never written: that frame is gone. */
    qtest_memread(qts, RX_BUF_PHYS, got, sizeof(small));
    memset(zeros, 0, sizeof(small));
    g_assert_cmpmem(got, sizeof(small), zeros, sizeof(small));

    /*
     * The chip counts this rather than latching it (PG105 page 110), the
     * same as the jumbo case: one frame dropped for want of a posted
     * standard descriptor, one count. The jumbo frame that lands right
     * after adds nothing to it. Delivery is not the condition this
     * counter names.
     */
    g_assert_cmphex(bar_readl(qts, BCM_SRAM_WINDOW_BASE +
                               BCM_SRAM_STATS_NO_MORE_RXBDS), ==, 1);

    qtest_quit(qts);
    close(sv[0]);
    close(sv[1]);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/sn0/bcm570x/dma-rides-plain-route", NULL,
                        test_dma_rides_plain_route);
    qtest_add_data_func("/sn0/bcm570x/status-block-update-criteria", NULL,
                        test_status_block_update_criteria);
    qtest_add_data_func("/sn0/bcm570x/receive-descriptor-big-endian", NULL,
                        test_receive_descriptor_big_endian);
    qtest_add_data_func("/sn0/bcm570x/station-address-byte-reads", NULL,
                        test_station_address_byte_reads);
    qtest_add_data_func("/sn0/bcm570x/jumbo-ring-selected-by-standard-length",
                        NULL, test_jumbo_ring_selected_by_standard_length);
    qtest_add_data_func(
        "/sn0/bcm570x/standard-shortage-discards-without-waiting", NULL,
        test_standard_shortage_discards_without_waiting);
    qtest_add_data_func("/sn0/bcm570x/jumbo-ring-disabled-drops-oversize",
                        NULL, test_jumbo_ring_disabled_drops_oversize);
    qtest_add_data_func("/sn0/bcm570x/jumbo-frame-overruns-descriptor",
                        NULL, test_jumbo_frame_overruns_descriptor);
    qtest_add_data_func(
        "/sn0/bcm570x/jumbo-ring-unprovisioned-drops-oversize", NULL,
        test_jumbo_ring_unprovisioned_drops_oversize);
    qtest_add_data_func(
        "/sn0/bcm570x/jumbo-shortage-discards-without-waiting", NULL,
        test_jumbo_shortage_discards_without_waiting);

    return g_test_run();
}
