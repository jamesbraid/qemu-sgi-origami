/*
 * Alteon Tigon 2 (SGI "eg") PCI card on the Origin 200 Bridge.
 * A free -device slot, as in sn0-bridge-test.c, exercises Bridge decode,
 * register access, firmware ring processing and descriptor byte order over
 * the machine's crosstalk-to-PCI path; base I/O adapters are soldered down.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"

/*
 * The modelled minimum between a frame arriving and its completion, in
 * nanoseconds: the adapters' device default. Delivery is published only once
 * this much virtual time has passed, so every wait for a completion has to
 * advance the clock as well as give the main loop a pass.
 */
#define RX_ARRIVAL_LATENCY_NS  200000

#include <sys/socket.h>
#include <netinet/in.h>
#include "sgi-test.h"
#include "sgi-migrate.h"

/* Crosstalk space and the Bridge's widget window inside it. */
#define BRIDGE_WIDGET   8
#define BRIDGE_BASE     (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))

/* A fixed station address, so the frame filters have a known target. */
#define TIGON_MAC       "52:54:00:12:34:56"
static const uint8_t tigon_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

#define MACHINE_WITH_BRIDGE  \
    "-M origin200 -device tigon,addr=3,mac=" TIGON_MAC

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
#define PCI_REV_CLASS       0x08
#define PCI_INTR_LINE       0x3c

/* The Bridge's own per-slot device register, low half of its 8-byte slot. */
#define REG_DATA            4
#define BRIDGE_DEVICE(n)    (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)
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
#define DIRECT_IOVA(phys)   (0x80000000u | (uint32_t)(phys))

/* Where the card's BAR is parked in PCI memory space (1 MB aligned). */
#define ADAPTER_MMIO        0x10000000u

/* The card's identity. */
#define TIGON_VENDOR_ID     0x10a9
#define TIGON_DEVICE_ID     0x0009
#define TIGON_CLASS_CODE    0x020000
#define TIGON_CHIP_REV      0x6         /* Tigon 2 rev nibble */

/* Register offsets within the card's own window (if_tireg.h). */
#define TI_MISC_HOST_CTL    0x040
#define TI_GCR_PAR0         0x600
#define TI_GCR_PAR1         0x604
#define TI_MHC_RESET        0x00000008
#define TI_MHC_CHIP_REV_MASK 0xf0000000

/* Interrupt and CPU control. */
#define TI_MHC_INTSTATE     0x00000001
#define TI_WINBASE          0x068
#define TI_CPU_STATE        0x140
#define TI_CPU_PROGRAM_COUNTER 0x144
#define TI_SRAM_WINDOW      0x800

/* Mailbox doorbells and descriptor flags. */
#define TI_MB_HOSTINTR      0x504
#define TI_MB_SENDPROD_IDX  0x514
#define TI_MB_STDRXPROD_IDX 0x51c
#define TI_MB_CMDPROD_IDX   0x50c
#define TI_GCR_CMDRING      0x700
#define TI_CMD_SET_PROMISC_MODE 0x0a
#define TI_CMD_CODE_PROMISC_ENB 0x01
#define TI_CMD_CODE_PROMISC_DIS 0x02
#define TI_CMD_SET_MAC_ADDR 0x0c
#define TI_CMD_SET_RX_JUMBO_PROD_IDX 0x10
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
#define TI_GCR_RX_COAL_TICKS 0x630
#define TI_GCR_TX_COAL_TICKS 0x634
#define TI_GCR_TX_MAX_COAL_BD 0x63c
#define TI_GCR_RX_MAX_COAL_BD 0x640
#define TI_GCR_MASK_INTRS  0x664
#define TI_GCR_RXRETURNCONS_IDX 0x680
#define TI_MHC_BYTE_SWAP_ENB 0x00000010
#define TI_MHC_WORD_SWAP_ENB 0x00000020
#define TI_OPMODE_BYTESWAP_BD   0x00000002
#define TI_OPMODE_WORDSWAP_BD   0x00000004
#define TI_OPMODE_BYTESWAP_DATA 0x00000010

/*
 * A general information block and its rings, laid out in low guest RAM. The
 * Bridge carries a DMA address below its translated window straight through
 * to system memory, so the card reaches these at the same address the test
 * wrote them, with no address-translation setup.
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
/* Misaligned to exercise the payload copy's buf & 3 != 0 path. */
#define TX_BUF_DIRECT_PHYS  (TX_BUF_PHYS + 2)
#define RX_BUF_DIRECT_PHYS  (RX_BUF_PHYS + 2)
/* Deliberately outside the 128 MiB machine used by the DMA-error tests. */
#define UNMAPPED_PAYLOAD_PHYS 0x7ff00000u

#define JUMBO_RX_RING_PHYS  0x002a0000u
#define JUMBO_BUF_PHYS      0x002c0000u

#define TI_RCB_FLAG_HOST_RING   0x0040
#define TI_EV_FIRMWARE_UP       0x01
#define TI_BDTYPE_RECV_BD       0x0002
#define TI_BDTYPE_RECV_JUMBO_BD 0x0003
#define TI_BDFLAG_JUMBO_RING    0x0010
#define TI_MB_JUMBORXPROD_IDX   0x524
#define GIB_JUMBO_RX_RCB        0x440

/*
 * The statistics contract. The block's own head holds a 1 KB struct ti_stats,
 * and its last field names a second host address the firmware refreshes a
 * copy into. Both public drivers make those one address; this test gives them
 * separate buffers, the way the IRIX guest does, so a card that filled only
 * one of them fails here.
 */
#define GIB_REFRESH_STATS_PTR   0x488
#define STATS_PHYS              0x002d0000u
#define STATS_SIZE              0x400

/* Word offsets of the fields this contract defines inside struct ti_stats. */
#define STAT_IF_INDEX           (12 * 4)
#define STAT_IF_MTU             (14 * 4)
#define STAT_IF_SPEED           (15 * 4)
#define STAT_IF_ADMIN_STATUS    (16 * 4)
#define STAT_IF_OPER_STATUS     (17 * 4)
#define STAT_IF_IN_DISCARDS     (19 * 4)
#define STAT_IF_PHYS_ADDRESS    (25 * 4)
#define STAT_HC_IN_OCTETS       (36 * 4)
#define STAT_HC_IN_UCAST        (38 * 4)
#define STAT_HC_IN_MULTICAST    (40 * 4)
#define STAT_HC_IN_BROADCAST    (42 * 4)
#define STAT_HC_OUT_OCTETS      (44 * 4)
#define STAT_HC_OUT_UCAST       (46 * 4)
#define STAT_HC_OUT_BROADCAST   (50 * 4)

#define TI_GCR_STAT_TICKS       0x638
#define TI_GCR_IFINDEX          0x65c
#define TI_GCR_GLINK_STAT       0x668
#define TI_GCR_IFMTU            0x660
#define TI_CMD_HOST_STATE       0x01
#define TI_CMD_CODE_STACK_UP    0x01
#define TI_CMD_LINK_NEGOTIATION 0x0b
#define TI_CMD_CLR_STATS        0x0f
#define TI_CMD_RFRSH_STATS      0x11
#define TI_EV_STATS_UPDATED     0x04
#define IF_STATUS_UP            1
#define IF_STATUS_DOWN          2
#define GIGABIT_BITS_PER_SECOND 1000000000u
/* The model's stand-in delay between a negotiation request and the answer. */
#define LINK_DELAY_NS           50000000
/* How often the model rewrites the block at the information block's head. */
#define STATS_LIVE_NS           100000000

/*
 * Write a ring control block's 64-bit host address (two words, hi then lo).
 */
static void write_rcb_dma_addr(QTestState *qts, uint32_t rcb, uint64_t addr)
{
    qtest_writel(qts, rcb + 0, addr >> 32);
    qtest_writel(qts, rcb + 4, addr);
}

static void write_rcb_addr(QTestState *qts, uint32_t rcb, uint32_t phys)
{
    write_rcb_dma_addr(qts, rcb, phys);
}

/*
 * A 4-byte access needs no byte-lane correction.
 * sgi_bridge_window_correct() moves only the low two bits and leaves a
 * full-word access at its natural offset, so a 32-bit register is reached
 * at WINDOW + offset directly.
 */
static void aim_window_at_bar(QTestState *qts)
{
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_BAR0, ADAPTER_MMIO);
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(TIGON_SLOT),
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
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

    /*
     * The RESET bit is accepted and leaves the read-only rev nibble intact.
     */
    qtest_writel(qts, WINDOW_TIGON + TI_MISC_HOST_CTL, TI_MHC_RESET);
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_MISC_HOST_CTL) &
                    TI_MHC_CHIP_REV_MASK, ==, (uint32_t)TIGON_CHIP_REV << 28);

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

    qtest_memset(qts, STATS_PHYS, 0, STATS_SIZE);
    write_rcb_addr(qts, GIB_PHYS + GIB_REFRESH_STATS_PTR, STATS_PHYS);
}

/*
 * Hand the card a general-information-block address. The register window is
 * little-endian, so the half at the lower offset, the one the public header
 * calls GENINFO_HI, is the least significant one, which is where a guest
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
 * The socket backend frames each packet with a four-byte network-order
 * length, the same way the other NIC qtests drive a socketpair-backed netdev.
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
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    return qtest_readl(qts, RETURN_PROD_PHYS);
}

/*
 * The same wait for a caller that already has completions behind it: a bare
 * "not zero" would return the previous frame's index before this one's
 * arrival deadline has passed.
 */
static uint32_t wait_return_prod_reaches(QTestState *qts, uint32_t want)
{
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        if (qtest_readl(qts, RETURN_PROD_PHYS) == want) {
            break;
        }
        g_usleep(5000);
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    return qtest_readl(qts, RETURN_PROD_PHYS);
}

static void assert_return_prod_stays(QTestState *qts, uint32_t expected)
{
    int attempt;

    for (attempt = 0; attempt < 20; attempt++) {
        g_assert_cmphex(qtest_readl(qts, RETURN_PROD_PHYS), ==, expected);
        g_usleep(5000);
    }
}

static QTestState *tigon_init_netdev(int guest_fd)
{
    return qtest_initf("-M origin200 "
                       "-netdev socket,fd=%d,id=n0 "
                       "-device tigon,addr=3,mac=" TIGON_MAC ",netdev=n0",
                       guest_fd);
}

static QTestState *tigon_init_netdev_binary(int guest_fd, const char *binary)
{
    g_autofree char *saved = g_strdup(g_getenv("QTEST_QEMU_BINARY"));
    QTestState *qts;

    g_assert_true(g_setenv("QTEST_QEMU_BINARY", binary, true));
    qts = tigon_init_netdev(guest_fd);
    if (saved) {
        g_assert_true(g_setenv("QTEST_QEMU_BINARY", saved, true));
    } else {
        g_unsetenv("QTEST_QEMU_BINARY");
    }
    return qts;
}

static QTestState *tigon_init_netdev_no_arrival_floor(int guest_fd)
{
    return qtest_initf("-M origin200 "
                       "-netdev socket,fd=%d,id=n0 "
                       "-device tigon,addr=3,mac=" TIGON_MAC ",netdev=n0"
                       ",rx-arrival-latency-us=0",
                       guest_fd);
}

static QTestState *tigon_init_netdev_128m(int guest_fd)
{
    return qtest_initf("-M origin200 -m 128M "
                       "-netdev socket,fd=%d,id=n0 "
                       "-device tigon,addr=3,mac=" TIGON_MAC ",netdev=n0",
                       guest_fd);
}

static QTestState *tigon_init_netdev_128m_dma_traced(int guest_fd,
                                                      const char *logfile)
{
    return qtest_initf("-M origin200 -m 128M "
                       "-netdev socket,fd=%d,id=n0 "
                       "-device tigon,addr=3,mac=" TIGON_MAC ",netdev=n0 "
                       "-d guest_errors,trace:tigon_dma -D %s",
                       guest_fd, logfile);
}

static QTestState *tigon_init_netdev_dma_routes_traced(
    int guest_fd, const char *logfile)
{
    return qtest_initf("-M origin200 "
                       "-netdev socket,fd=%d,id=n0 "
                       "-device tigon,addr=3,mac=" TIGON_MAC ",netdev=n0 "
                       "-d trace:tigon_dma,trace:tigon_payload_dma_route,"
                       "trace:tigon_descriptor_dma_route -D %s",
                       guest_fd, logfile);
}

static char *tigon_test_log_new(void)
{
    char *logfile = NULL;
    int fd = g_file_open_tmp("tigon-dma-XXXXXX", &logfile, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    return logfile;
}

static void wait_log_contains(QTestState *qts, const char *logfile,
                              const char *needle)
{
    g_autofree char *log = NULL;
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        g_clear_pointer(&log, g_free);
        g_assert_true(g_file_get_contents(logfile, &log, NULL, NULL));
        if (strstr(log, needle)) {
            return;
        }
        g_usleep(5000);
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS / 4);
    }

    g_error("log did not contain '%s'", needle);
}

static const char *find_tigon_dma_once(const char *log,
                                       const char *direction,
                                       uint64_t addr, uint32_t bytes,
                                       unsigned result)
{
    g_autofree char *needle = g_strdup_printf(
        "tigon_dma %s 0x%" PRIx64 " %u bytes result %u",
        direction, addr, bytes, result);
    const char *match = strstr(log, needle);

    g_assert_nonnull(match);
    g_assert_null(strstr(match + strlen(needle), needle));
    return match;
}

static unsigned count_tigon_dma_in_range(const char *log, uint64_t start,
                                         uint64_t bytes)
{
    static const char event[] = "tigon_dma ";
    const char *cursor = log;
    unsigned count = 0;

    while ((cursor = strstr(cursor, event)) != NULL) {
        char direction[6];
        uint64_t addr;
        uint32_t len;
        unsigned result;

        if (sscanf(cursor, "tigon_dma %5s 0x%" SCNx64
                   " %" SCNu32 " bytes result %u",
                   direction, &addr, &len, &result) == 4 &&
            addr < start + bytes && start < addr + len) {
            count++;
        }
        cursor += sizeof(event) - 1;
    }
    return count;
}

static unsigned count_log_matches(const char *log, const char *needle)
{
    const char *cursor = log;
    unsigned count = 0;

    while ((cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += strlen(needle);
    }
    return count;
}

/* The same machine, with the Bridge's DMA-route trace points logged. */
static QTestState *tigon_init_netdev_traced(int guest_fd, const char *logfile)
{
    return qtest_initf("-M origin200 "
                       "-netdev socket,fd=%d,id=n0 "
                       "-device tigon,addr=3,mac=" TIGON_MAC ",netdev=n0 "
                       "-d trace:sgi_bridge_lane_swap_read,"
                       "trace:sgi_bridge_lane_swap_write,"
                       "trace:sgi_bridge_dma_xlate_plain -D %s",
                       guest_fd, logfile);
}

static bool irq_asserted(QTestState *qts)
{
    return (qtest_readl(qts, WINDOW_TIGON + TI_MISC_HOST_CTL) &
            TI_MHC_INTSTATE) != 0;
}

/*
 * An IRIX-style GIB address has nonzero upper bits: widget in the high half,
 * offset in the low half. Reversed halves name the wrong widget and miss
 * memory, so rings read zero while the interface appears configured.
 * Addresses below 4 GiB cannot distinguish the order because their high
 * halves are zero.
 */
static void test_geninfo_upper_half(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);
    uint64_t gib = 0xa300000000000000ULL | GIB_PHYS;

    enable_dma_big_endian(qts);
    /*
     * Unlike the block addresses the other tests publish, this one is above 4
     * GiB, so it leaves local space and crosses the bridge's byte swapper.
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

/*
 * A firmware download reaches SRAM through the movable 2 KB BAR window. The
 * mixed-width writes share one word so widening either narrow access corrupts
 * a neighbouring byte. A second WINBASE position proves that the window is
 * not fixed at SRAM offset zero. Only after that public upload sequence do we
 * publish the ring set and release CPU A.
 */
static void test_firmware_upload_window_posts_event(const void *data)
{
    static const uint8_t upload_pattern[][4] = {
        { 0xa5, 0x22, 0xb6, 0xc7 },
        { 0xd8, 0xe9, 0xfa, 0x0b },
    };
    static const uint8_t empty_pattern[4] = { 0 };
    const uint32_t upload_base = 0x2000;
    const uint32_t other_base = 0x2800;
    const uint32_t upload_off = 0x20;
    const uint64_t window = WINDOW_TIGON + TI_SRAM_WINDOW + upload_off;
    uint8_t observed[4];
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    enable_dma_big_endian(qts);
    build_gib(qts);

    qtest_writel(qts, WINDOW_TIGON + TI_WINBASE, upload_base);
    qtest_writel(qts, window, 0x11223344);
    qtest_writeb(qts, window, 0xa5);
    qtest_writew(qts, window + 2, 0xb6c7);
    qtest_writel(qts, window + 4, 0xd8e9fa0b);

    qtest_memread(qts, window, observed, sizeof(observed));
    g_assert_cmpmem(observed, sizeof(observed), upload_pattern[0],
                    sizeof(upload_pattern[0]));
    qtest_memread(qts, window + 4, observed, sizeof(observed));
    g_assert_cmpmem(observed, sizeof(observed), upload_pattern[1],
                    sizeof(upload_pattern[1]));

    qtest_writel(qts, WINDOW_TIGON + TI_WINBASE, other_base);
    qtest_memread(qts, window, observed, sizeof(observed));
    g_assert_cmpmem(observed, sizeof(observed), empty_pattern,
                    sizeof(empty_pattern));
    qtest_memread(qts, window + 4, observed, sizeof(observed));
    g_assert_cmpmem(observed, sizeof(observed), empty_pattern,
                    sizeof(empty_pattern));
    qtest_writel(qts, window, 0x01020304);

    qtest_writel(qts, WINDOW_TIGON + TI_WINBASE, upload_base);
    qtest_memread(qts, window, observed, sizeof(observed));
    g_assert_cmpmem(observed, sizeof(observed), upload_pattern[0],
                    sizeof(upload_pattern[0]));
    qtest_memread(qts, window + 4, observed, sizeof(observed));
    g_assert_cmpmem(observed, sizeof(observed), upload_pattern[1],
                    sizeof(upload_pattern[1]));

    qtest_writel(qts, WINDOW_TIGON + TI_CPU_PROGRAM_COUNTER, upload_base);
    publish_gib(qts);
    release_cpu(qts);

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

static void post_std_rx_buffer(QTestState *qts, unsigned slot, uint32_t phys,
                               uint16_t buf_idx, uint32_t buf_len);

/* Descriptor DMA is batched, while completion stays a separate last write. */
static void test_descriptor_dma_batches_producer_last(const void *data)
{
    int sv[2];
    QTestState *qts;
    g_autofree char *logfile = tigon_test_log_new();
    g_autofree char *log = NULL;
    const char *event_desc, *event_prod, *tx_desc, *tx_cons;
    const char *rx_desc, *rx_return, *rx_prod;
    uint8_t got[sizeof(test_frame)];
    uint32_t word2;
    size_t len;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev_128m_dma_traced(sv[1], logfile);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    enable_dma_big_endian(qts);
    qtest_writel(qts, BRIDGE_DEVICE(TIGON_SLOT),
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20) | DEV_SWAP_DIRECT);
    build_gib(qts);
    write_rcb_dma_addr(qts, GIB_PHYS + GIB_RETURN_PROD_PTR,
                       DIRECT_IOVA(RETURN_PROD_PHYS));
    publish_gib(qts);
    release_cpu(qts);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);

    qtest_memwrite(qts, TX_BUF_PHYS, test_frame, sizeof(test_frame));
    write_rcb_addr(qts, TX_RING_PHYS, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(test_frame) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);

    len = net_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(test_frame));
    g_assert_cmpmem(got, len, test_frame, sizeof(test_frame));
    g_assert_cmphex(qtest_readl(qts, TX_CONS_PHYS), ==, 1);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);

    write_rcb_addr(qts, STD_RX_RING_PHYS, RX_BUF_PHYS);
    qtest_writel(qts, STD_RX_RING_PHYS + 8, (7u << 16) | 2048);
    qtest_writel(qts, STD_RX_RING_PHYS + 12,
                 (uint32_t)TI_BDTYPE_RECV_BD << 16);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);
    net_send_frame(sv[0], test_frame, sizeof(test_frame));

    wait_log_contains(qts, logfile,
                      "tigon: receive return producer write at "
                      "0x80250000 failed");
    g_assert_cmphex(qtest_readl(qts, RETURN_PROD_PHYS), ==, 0);
    word2 = qtest_readl(qts, RETURN_RING_PHYS + 8);
    g_assert_cmphex(word2 >> 16, ==, 7);
    g_assert_false(irq_asserted(qts));

    g_assert_true(g_file_get_contents(logfile, &log, NULL, NULL));
    event_desc = find_tigon_dma_once(log, "write", EVENT_RING_PHYS, 8, 0);
    event_prod = find_tigon_dma_once(log, "write", EV_PROD_PHYS, 4, 0);
    tx_desc = find_tigon_dma_once(log, "read", TX_RING_PHYS, 12, 0);
    tx_cons = find_tigon_dma_once(log, "write", TX_CONS_PHYS, 4, 0);
    rx_desc = find_tigon_dma_once(log, "read", STD_RX_RING_PHYS, 12, 0);
    rx_return = find_tigon_dma_once(log, "write", RETURN_RING_PHYS, 32, 0);
    rx_prod = find_tigon_dma_once(log, "write",
                                  DIRECT_IOVA(RETURN_PROD_PHYS), 4, 2);
    g_assert_cmpuint(count_tigon_dma_in_range(log, EVENT_RING_PHYS, 8), ==,
                     1);
    g_assert_cmpuint(count_tigon_dma_in_range(log, TX_RING_PHYS, 16), ==, 1);
    g_assert_cmpuint(count_tigon_dma_in_range(log, STD_RX_RING_PHYS, 32), ==,
                     1);
    g_assert_cmpuint(count_tigon_dma_in_range(log, RETURN_RING_PHYS, 32), ==,
                     1);
    g_assert_cmpuint(count_tigon_dma_in_range(log, EV_PROD_PHYS, 4), ==, 1);
    g_assert_cmpuint(count_tigon_dma_in_range(log, TX_CONS_PHYS, 4), ==, 1);
    g_assert_cmpuint(count_tigon_dma_in_range(
                         log, DIRECT_IOVA(RETURN_PROD_PHYS), 4), ==, 1);
    g_assert_true(event_desc < event_prod);
    g_assert_true(event_prod < tx_desc);
    g_assert_true(tx_desc < tx_cons);
    g_assert_true(tx_cons < rx_desc);
    g_assert_true(rx_desc < rx_return);
    g_assert_true(rx_return < rx_prod);

    /*
     * The guest repairs the mapping and posts a buffer. Posting is what
     * restarts placement, so the completion held since its producer write was
     * refused lands on the next tick rather than when its retry backoff
     * runs out. That backoff is at least one arrival floor from the refusal.
     * One nanosecond of
     * virtual time is far short of that backoff, and only enough for a timer
     * already due to fire.
     */
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readl(qts, RETURN_PROD_PHYS), ==, 1);

    qtest_quit(qts);
    close(sv[0]);
    unlink(logfile);
}

/* Repeated traffic uses the cached descriptor route for every ordered DMA. */
static void test_descriptor_dma_route_used_for_traffic(const void *data)
{
    int sv[2];
    QTestState *qts;
    g_autofree char *logfile = tigon_test_log_new();
    g_autofree char *log = NULL;
    uint8_t got[sizeof(test_frame)];
    gsize traffic_start;
    unsigned i;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev_dma_routes_traced(sv[1], logfile);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);
    go_operational(qts);
    g_assert_true(g_file_get_contents(logfile, &log, &traffic_start, NULL));
    g_clear_pointer(&log, g_free);

    /* One doorbell drains two transmit payloads. */
    for (i = 0; i < 2; i++) {
        uint64_t desc = TX_RING_PHYS + (uint64_t)i * 16;
        uint64_t payload = TX_BUF_PHYS + (uint64_t)i * 0x100;

        qtest_memwrite(qts, payload, test_frame, sizeof(test_frame));
        write_rcb_addr(qts, desc, payload);
        qtest_writel(qts, desc + 8,
                     ((uint32_t)sizeof(test_frame) << 16) | TI_BDFLAG_END);
        qtest_writel(qts, desc + 12, 0);
    }
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 2);
    for (i = 0; i < 2; i++) {
        g_assert_cmpint(net_recv_frame(sv[0], got, sizeof(got)), ==,
                        sizeof(test_frame));
        g_assert_cmpmem(got, sizeof(got), test_frame, sizeof(test_frame));
    }

    /* Two incoming packets exercise the receive payload route too. */
    for (i = 0; i < 2; i++) {
        post_std_rx_buffer(qts, i, RX_BUF_PHYS + (uint64_t)i * 0x100,
                           i, 2048);
    }
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 2);
    net_send_frame(sv[0], test_frame, sizeof(test_frame));
    net_send_frame(sv[0], test_frame, sizeof(test_frame));
    for (i = 0; i < 200; i++) {
        if (qtest_readl(qts, RETURN_PROD_PHYS) == 2) {
            break;
        }
        g_usleep(5000);
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    g_assert_cmphex(qtest_readl(qts, RETURN_PROD_PHYS), ==, 2);

    qtest_quit(qts);
    close(sv[0]);
    g_assert_true(g_file_get_contents(logfile, &log, NULL, NULL));
    g_assert_cmpuint(count_log_matches(log, "tigon_payload_dma_route "), ==,
                     1);
    g_assert_cmpuint(count_log_matches(log + traffic_start, "tigon_dma "),
                     ==, 10);
    g_assert_cmpuint(count_log_matches(log + traffic_start,
                                       "tigon_descriptor_dma_route "), ==,
                     10);
    unlink(logfile);
}

/* A refused payload fetch must not send or consume its descriptor. */
static void test_transmit_payload_dma_error(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t byte;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev_128m(sv[1]);
    close(sv[1]);

    go_operational(qts);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);

    write_rcb_addr(qts, TX_RING_PHYS + 0, UNMAPPED_PAYLOAD_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(test_frame) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);

    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);

    g_assert_cmphex(qtest_readl(qts, TX_CONS_PHYS), ==, 0);
    g_assert_cmpint(recv(sv[0], &byte, sizeof(byte), MSG_DONTWAIT), ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
    g_assert_false(irq_asserted(qts));

    qtest_quit(qts);
    close(sv[0]);
}

/* A later fragment fault must leave the whole frame ready to retry. */
static void test_transmit_fragment_dma_error_retry(const void *data)
{
    enum { FIRST_LEN = 24 };
    int sv[2];
    QTestState *qts;
    uint8_t got[sizeof(test_frame)];
    uint8_t byte;
    size_t len;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev_128m(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);

    qtest_memwrite(qts, TX_BUF_PHYS, test_frame, sizeof(test_frame));
    write_rcb_addr(qts, TX_RING_PHYS + 0, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8, (uint32_t)FIRST_LEN << 16);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);
    write_rcb_addr(qts, TX_RING_PHYS + 16, UNMAPPED_PAYLOAD_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 24,
                 ((uint32_t)(sizeof(test_frame) - FIRST_LEN) << 16) |
                 TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 28, 0);

    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 2);

    g_assert_cmphex(qtest_readl(qts, TX_CONS_PHYS), ==, 0);
    g_assert_cmpint(recv(sv[0], &byte, sizeof(byte), MSG_DONTWAIT), ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
    g_assert_false(irq_asserted(qts));

    /* Repair only the failed fragment and retry the same producer. */
    write_rcb_addr(qts, TX_RING_PHYS + 16, TX_BUF_PHYS + FIRST_LEN);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 2);

    len = net_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(test_frame));
    g_assert_cmpmem(got, len, test_frame, sizeof(test_frame));
    g_assert_cmphex(qtest_readl(qts, TX_CONS_PHYS), ==, 2);
    g_assert_true(irq_asserted(qts));
    g_assert_cmpint(recv(sv[0], &byte, sizeof(byte), MSG_DONTWAIT), ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A 60-byte IPv4 TCP SYN with its TCP checksum field left zero, the way
 * IRIX's eg hands a checksum-offloaded segment to the card. Offsets: ethernet
 * 0..13, IP header 14..33 (protocol 6, total length 40), TCP header 34..53
 * (checksum at 50..51), then ethernet padding to the 60-byte minimum.
 */
static const uint8_t tcp_csum_frame[60] = {
    0x52, 0x54, 0x00, 0x12, 0x34, 0x99,      /* dest = a peer unicast */
    0x52, 0x54, 0x00, 0x12, 0x34, 0x56,      /* source = our MAC */
    0x08, 0x00,                              /* ethertype IP */
    0x45, 0x00, 0x00, 0x28,                  /* v4, IHL 5, total length 40 */
    0x00, 0x00, 0x40, 0x00,                  /* id 0, flags DF */
    0x40, 0x06, 0x00, 0x00,                  /* ttl 64, TCP, IP cksum 0 */
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
 * header as the frame stands. A correct TCP checksum makes this 0xffff; a
 * zero checksum field makes it the complement of the real checksum instead.
 */
static uint16_t tcp_csum_fold(const uint8_t *f)
{
    uint32_t sum = 0;
    int i;

    sum += (f[26] << 8) | f[27];   /* src ip high */
    sum += (f[28] << 8) | f[29];   /* src ip low */
    sum += (f[30] << 8) | f[31];   /* dst ip high */
    sum += (f[32] << 8) | f[33];   /* dst ip low */
    sum += f[23];                  /* pseudo-header zero + protocol */
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
 * Transmit checksum offload: a descriptor flagged TI_BDFLAG_TCP_UDP_CKSUM
 * asks the card to fill the L4 checksum the driver left zero. Without it
 * every TCP segment leaves with a zero checksum and no peer accepts it.
 * An eg TCP handshake then never completes, though UDP (optional checksum)
 * survives.
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
 * A real 61-byte dst/src/ethertype frame is not a multiple of 4, so the
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
 * Transmit a driver-style DMA-mapped sk_buff through Bridge direct-map DMA.
 * TX_BUF_PHYS is below the translated window and bypasses Bridge DMA. As in
 * byte_order_check(), descriptors read as words under either programmed
 * pairing; payload bytes must match exactly. A start with buf & 3 == 2 and a
 * non-multiple-of-four length exercise both partial-word edges.
 */
static void transmit_direct_map_check(bool bridge_transparent,
                                      const char *tracefile)
{
    int sv[2];
    QTestState *qts;
    uint8_t got[2048];
    size_t len;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tracefile ? tigon_init_netdev_traced(sv[1], tracefile)
                    : tigon_init_netdev(sv[1]);
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
    transmit_direct_map_check(true, NULL);
}

static void test_transmit_direct_map_corrects(const void *data)
{
    transmit_direct_map_check(false, NULL);
}

static unsigned count_lines_with(const char *text, const char *needle)
{
    unsigned n = 0;
    const char *p = text;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }
    return n;
}

/*
 * Payload must use plain DMA, not the byte-lane swap window. Both routes
 * produce identical wire bytes: per-word reversal cancels the card's word
 * consumption, but the swap path dispatches once per word and limits
 * throughput. The trace distinguishes them. Nonzero descriptor swap counts
 * prove tracing is active; payload counts on the plain route prove the
 * address matches. Both addresses derive from the buffer constant, so moving
 * it updates both. A trace format change fails the positive check first. Swap
 * logs a window offset; plain DMA logs the device's raw PCI address.
 */
static void test_payload_skips_lane_swap(const void *data)
{
    g_autofree char *swap_needle =
        g_strdup_printf("addr 0x%x", TX_BUF_PHYS >> 16);
    g_autofree char *plain_needle =
        g_strdup_printf("plain addr 0x%" PRIx64,
                        (uint64_t)(IOVA(TX_BUF_PHYS) >> 8));
    char *logfile = NULL;
    char *log = NULL;
    int fd;

    fd = g_file_open_tmp("tigon-lane-swap-XXXXXX", &logfile, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    transmit_direct_map_check(false, logfile);

    g_assert_true(g_file_get_contents(logfile, &log, NULL, NULL));
    /* Control: the swap window saw the descriptor and ring traffic. */
    g_assert_cmpuint(count_lines_with(log, "lane swap "), >, 0);
    /* Control: the payload buffer's address showed up on the plain route. */
    g_assert_cmpuint(count_lines_with(log, plain_needle), >, 0);
    /* The payload buffer never crossed the swap window. */
    g_assert_cmpuint(count_lines_with(log, swap_needle), ==, 0);

    g_free(log);
    unlink(logfile);
    g_free(logfile);
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
    g_autofree char *logfile = tigon_test_log_new();
    g_autofree char *log = NULL;
    QTestState *qts = qtest_initf(
        "-M origin200 -netdev user,id=n0 "
        "-device tigon,id=tigon0,addr=3,mac=" TIGON_MAC ",netdev=n0 "
        "-d trace:tigon_rx_flush -D %s", logfile);
    uint32_t after_poll;
    uint8_t landed[sizeof(arp_request)];

    go_operational(qts);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_RX_MAX_COAL_BD, 2);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_RX_COAL_TICKS, 1000000);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    g_assert_false(irq_asserted(qts));

    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    qtest_memwrite(qts, TX_BUF_PHYS, arp_request, sizeof(arp_request));
    write_rcb_addr(qts, TX_RING_PHYS, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(arp_request) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);

    /*
     * User networking answers the ARP request from qemu_send_packet(), which
     * queues the receive on a bottom half. The doorbell write returns to the
     * main loop, which runs that bottom half, so the receive-return
     * descriptor then shows published ownership.
     */
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);
    after_poll = qtest_readl(qts, RETURN_RING_PHYS + 12);
    g_assert_cmphex(after_poll & TI_BDFLAG_END, ==, TI_BDFLAG_END);
    g_assert_cmphex(qtest_readl(qts, TX_CONS_PHYS), ==, 1);
    g_assert_true(irq_asserted(qts));

    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, 6, tigon_mac, 6);
    g_assert_cmphex(landed[20], ==, 0x00);
    g_assert_cmphex(landed[21], ==, 0x02);
    g_assert_cmpmem(landed + 28, 4, gateway_ip, 4);
    g_assert_cmpmem(landed + 38, 4, guest_ip, 4);

    /* RX is coalesced, so this interrupt can only be the TX completion. */
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    g_assert_false(irq_asserted(qts));

    /* Prove the trace records the ordinary deferred flush above. */
    qtest_clock_step(qts, 1);
    g_assert_true(g_file_get_contents(logfile, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "tigon_rx_flush"));
    g_clear_pointer(&log, g_free);

    /* Queue another reply, then reset the chip; the reset clears the IRQ. */
    write_rcb_addr(qts, TX_RING_PHYS + 16, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 24,
                 ((uint32_t)sizeof(arp_request) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 28, 0);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 2);
    qtest_writel(qts, WINDOW_TIGON + TI_MISC_HOST_CTL, TI_MHC_RESET);
    qtest_clock_step(qts, 1);
    g_assert_false(irq_asserted(qts));

    /*
     * Origin's fixed PCI bus rejects hot-unplug. Forced unrealize exercises
     * the device teardown path after the reset.
     */
    qtest_qmp_assert_success(
        qts, "{'execute': 'qom-set', 'arguments': {"
             "'path': '/machine/peripheral/tigon0', "
             "'property': 'realized', 'value': false}}");
    qtest_clock_step(qts, 1);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    qtest_qmp_assert_success(
        qts, "{'execute': 'netdev_del', 'arguments': {'id': 'n0'}}");

    qtest_quit(qts);
    unlink(logfile);
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

/* A refused payload write must leave the posted buffer ready to retry. */
static void test_receive_payload_dma_error(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t landed[sizeof(rx_frame_ours)];
    uint8_t returned[32], return_sentinel[sizeof(returned)];
    uint32_t word2;

    memset(return_sentinel, 0x5a, sizeof(return_sentinel));
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev_128m(sv[1]);
    close(sv[1]);

    go_operational(qts);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);

    qtest_memwrite(qts, RETURN_RING_PHYS, return_sentinel,
                   sizeof(return_sentinel));
    post_std_rx_buffer(qts, 0, UNMAPPED_PAYLOAD_PHYS, 7, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));

    assert_return_prod_stays(qts, 0);
    qtest_memread(qts, RETURN_RING_PHYS, returned, sizeof(returned));
    g_assert_cmpmem(returned, sizeof(returned), return_sentinel,
                    sizeof(return_sentinel));
    g_assert_false(irq_asserted(qts));

    /* Repair slot 0 in place without posting another descriptor. */
    qtest_memset(qts, RX_BUF_PHYS, 0, sizeof(landed));
    write_rcb_addr(qts, STD_RX_RING_PHYS, RX_BUF_PHYS);
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));

    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    word2 = qtest_readl(qts, RETURN_RING_PHYS + 8);
    g_assert_cmphex(word2 >> 16, ==, 7);
    g_assert_cmphex(word2 & 0xffff, ==, sizeof(rx_frame_ours));
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed),
                    rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_true(irq_asserted(qts));
    assert_return_prod_stays(qts, 1);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A frame larger than the standard ring's buffers has to come back through
 * the jumbo ring and say that it did. A card that only ever serves the
 * standard ring cannot carry such a frame at all, and a driver will not offer
 * a frame size the card cannot take, so the guest stays at the standard
 * size however it is configured, which then looks like a limitation of the
 * guest.
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
 * The firmware command is the other way a jumbo buffer gets posted, and the
 * IRIX eg driver uses it rather than the mailbox. A frame arriving after one
 * must land in that buffer with its payload intact and report the buffer's
 * host address, ring index and full frame length. The negative is sharp: a
 * card that ignored the command would have an empty jumbo ring and would
 * discard the frame instead.
 */
static void test_receive_jumbo_posted_by_command(const void *data)
{
    enum { JUMBO_LEN = 4000 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[JUMBO_LEN], landed[JUMBO_LEN];
    unsigned n;
    uint32_t word2, word3;

    memcpy(frame, rx_frame_ours, 14);
    for (n = 14; n < JUMBO_LEN; n++) {
        frame[n] = (uint8_t)(n * 3 + (n >> 5));
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /* A standard buffer too, so the choice is size, not availability. */
    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    /*
     * One jumbo buffer, pushed by the firmware command, never the mailbox.
     * The capacity is the one the IRIX driver actually posts, a 16 KB
     * allocation, far larger than any frame the card assembles. A buffer with
     * room to spare is a normal posting, not a corrupt descriptor.
     */
    qtest_memset(qts, JUMBO_BUF_PHYS, 0, 16384);
    write_rcb_addr(qts, JUMBO_RX_RING_PHYS + 0, JUMBO_BUF_PHYS);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 8, (5u << 16) | 16222);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 12,
                 (uint32_t)TI_BDTYPE_RECV_JUMBO_BD << 16);

    /* The command posts it; the jumbo producer mailbox is never written. */
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_CMDRING,
                 ((uint32_t)TI_CMD_SET_RX_JUMBO_PROD_IDX << 24) | 1);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_CMDPROD_IDX, 1);

    net_send_frame(sv[0], frame, sizeof(frame));
    g_assert_cmphex(wait_return_prod(qts), ==, 1);

    /*
     * The completion names the jumbo buffer by its own host address, the
     * driver's only way to tell the rings apart, with the posted index and
     * the whole frame's length.
     */
    g_assert_cmphex(qtest_readl(qts, RETURN_RING_PHYS + 0), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RETURN_RING_PHYS + 4), ==,
                    JUMBO_BUF_PHYS);
    word2 = qtest_readl(qts, RETURN_RING_PHYS + 8);
    g_assert_cmphex(word2 >> 16, ==, 5);
    g_assert_cmphex(word2 & 0xffff, ==, JUMBO_LEN);
    word3 = qtest_readl(qts, RETURN_RING_PHYS + 12);
    g_assert_cmphex(word3 & TI_BDFLAG_JUMBO_RING, ==, TI_BDFLAG_JUMBO_RING);

    qtest_memread(qts, JUMBO_BUF_PHYS, landed, sizeof(frame));
    g_assert_cmpmem(landed, sizeof(frame), frame, sizeof(frame));

    /* The standard buffer was left untouched. */
    g_assert_cmphex(qtest_readl(qts, RX_BUF_PHYS), ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Poll for a completion without advancing the virtual clock. Every qtest
 * command runs a pass of the main loop, so the back end still gets to read
 * its socket; what does not happen is any timer firing.
 */
static uint32_t return_prod_without_clock(QTestState *qts, uint32_t want)
{
    int attempt;

    for (attempt = 0; attempt < 100; attempt++) {
        if (qtest_readl(qts, RETURN_PROD_PHYS) >= want) {
            break;
        }
        g_usleep(5000);
    }
    return qtest_readl(qts, RETURN_PROD_PHYS);
}

/*
 * Freeze virtual time to distinguish immediate discard from bounded hold.
 * Other checks step past either outcome. A held frame stops backend reads, so
 * the following standard frame cannot arrive; immediate discard delivers it
 * on the next main-loop pass. The immediate-discard policy is an
 * approximation; a bounded hold reduced BCM570x throughput when measured and
 * would evade the other checks. Set the arrival floor to zero so success
 * needs no virtual-time advance.
 */
static void test_jumbo_shortage_discards_without_waiting(const void *data)
{
    enum { JUMBO_LEN = 4000 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[JUMBO_LEN], landed[sizeof(rx_frame_ours)];
    unsigned n;
    uint32_t word2;

    memcpy(frame, rx_frame_ours, 14);
    for (n = 14; n < JUMBO_LEN; n++) {
        frame[n] = (uint8_t)(n * 17 + (n >> 2));
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev_no_arrival_floor(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /* A standard buffer, and a jumbo control block with nothing posted. */
    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    net_send_frame(sv[0], frame, sizeof(frame));
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));

    /*
     * No qtest_clock_step anywhere: only a discard gets the second frame in.
     */
    g_assert_cmphex(return_prod_without_clock(qts, 1), ==, 1);
    word2 = qtest_readl(qts, RETURN_RING_PHYS + 8);
    g_assert_cmphex(word2 & 0xffff, ==, sizeof(rx_frame_ours));
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), rx_frame_ours,
                    sizeof(rx_frame_ours));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Send the firmware command after a frame is held with neither ring posted.
 * The jumbo producer mailbox retries it; the IRIX eg command handler must
 * too. The earlier case posts a buffer before the command and cannot detect a
 * missing command flush. This case tests that flush.
 */
static void test_jumbo_command_releases_a_refused_frame(const void *data)
{
    enum { JUMBO_LEN = 4000 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[JUMBO_LEN], landed[JUMBO_LEN];
    unsigned n;
    uint32_t word2, word3;

    memcpy(frame, rx_frame_ours, 14);
    for (n = 14; n < JUMBO_LEN; n++) {
        frame[n] = (uint8_t)(n * 11 + (n >> 4));
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /*
     * The jumbo descriptor is written into the ring but not posted, and no
     * standard buffer is posted either. Both rings empty is what makes the
     * card refuse rather than discard, so the frame is still there to
     * release.
     */
    qtest_memset(qts, JUMBO_BUF_PHYS, 0, 16384);
    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    write_rcb_addr(qts, JUMBO_RX_RING_PHYS + 0, JUMBO_BUF_PHYS);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 8, (9u << 16) | 16222);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 12,
                 (uint32_t)TI_BDTYPE_RECV_JUMBO_BD << 16);

    net_send_frame(sv[0], frame, sizeof(frame));
    assert_return_prod_stays(qts, 0);
    g_assert_cmphex(qtest_readl(qts, JUMBO_BUF_PHYS), ==, 0);

    /* The only thing that changes. The jumbo mailbox is never written. */
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_CMDRING,
                 ((uint32_t)TI_CMD_SET_RX_JUMBO_PROD_IDX << 24) | 1);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_CMDPROD_IDX, 1);

    g_assert_cmphex(wait_return_prod(qts), ==, 1);

    g_assert_cmphex(qtest_readl(qts, RETURN_RING_PHYS + 4), ==,
                    JUMBO_BUF_PHYS);
    word2 = qtest_readl(qts, RETURN_RING_PHYS + 8);
    g_assert_cmphex(word2 >> 16, ==, 9);
    g_assert_cmphex(word2 & 0xffff, ==, JUMBO_LEN);
    word3 = qtest_readl(qts, RETURN_RING_PHYS + 12);
    g_assert_cmphex(word3 & TI_BDFLAG_JUMBO_RING, ==, TI_BDFLAG_JUMBO_RING);

    qtest_memread(qts, JUMBO_BUF_PHYS, landed, sizeof(frame));
    g_assert_cmpmem(landed, sizeof(frame), frame, sizeof(frame));

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


/*
 * Interrupt coalescing delays the IRQ until a completion count or time limit,
 * whichever comes first; IRIX requests 130 descriptors or 900 microseconds.
 * Per-frame IRQs can cost up to two orders of magnitude more handler entries.
 * Completions remain visible in the ring immediately, so polling does not
 * stall.
 */
static void test_interrupt_coalesce_by_count(const void *data)
{
    int sv[2];
    QTestState *qts;
    unsigned i;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);
    go_operational(qts);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);

    /* Four completions per interrupt; the timer set far enough out not to. */
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_RX_MAX_COAL_BD, 4);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_RX_COAL_TICKS, 1000000);

    for (i = 0; i < 4; i++) {
        qtest_memset(qts, RX_BUF_PHYS + i * 2048, 0, 2048);
        post_std_rx_buffer(qts, i, RX_BUF_PHYS + i * 2048, i, 2048);
    }
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 4);

    for (i = 0; i < 3; i++) {
        net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
        g_assert_cmphex(wait_return_prod_reaches(qts, i + 1), ==, i + 1);
        /* Posted, but not yet worth an interrupt. */
        g_assert_false(irq_asserted(qts));
    }

    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_cmphex(wait_return_prod_reaches(qts, 4), ==, 4);
    g_assert_true(irq_asserted(qts));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The other half of the same contract: a frame that never reaches the count
 * must still be delivered when the time expires, or a transfer that ends
 * mid-batch waits for an interrupt that never comes.
 */
static void test_interrupt_coalesce_by_time(const void *data)
{
    int sv[2];
    QTestState *qts;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);
    go_operational(qts);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);

    /* A count this frame will never reach, and a millisecond of patience. */
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_RX_MAX_COAL_BD, 100);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_RX_COAL_TICKS, 1000);

    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    g_assert_false(irq_asserted(qts));

    qtest_clock_step(qts, 2000000);   /* two milliseconds */
    g_assert_true(irq_asserted(qts));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * TX_MAX_COAL_BD counts completed buffer descriptors, not producer-doorbell
 * writes. A driver normally publishes several ready descriptors with one
 * mailbox write, so counting the whole ring walk as one completion leaves a
 * sustained transfer waiting for the timer even after it reaches its
 * requested interrupt threshold.
 */
static void test_transmit_coalesce_counts_batched_descriptors(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t got[sizeof(test_frame)];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);
    go_operational(qts);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);

    qtest_writel(qts, WINDOW_TIGON + TI_GCR_TX_MAX_COAL_BD, 2);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_TX_COAL_TICKS, 1000000);

    qtest_memwrite(qts, TX_BUF_PHYS, test_frame, sizeof(test_frame) / 2);
    qtest_memwrite(qts, TX_BUF_PHYS + 0x100,
                   test_frame + sizeof(test_frame) / 2,
                   sizeof(test_frame) / 2);
    write_rcb_addr(qts, TX_RING_PHYS, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 (uint32_t)(sizeof(test_frame) / 2) << 16);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);
    write_rcb_addr(qts, TX_RING_PHYS + 16, TX_BUF_PHYS + 0x100);
    qtest_writel(qts, TX_RING_PHYS + 24,
                 ((uint32_t)(sizeof(test_frame) / 2) << 16) |
                 TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 28, 0);

    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 2);

    g_assert_cmpuint(net_recv_frame(sv[0], got, sizeof(got)), ==,
                     sizeof(test_frame));
    g_assert_cmpmem(got, sizeof(got), test_frame, sizeof(test_frame));
    g_assert_cmphex(qtest_readl(qts, TX_CONS_PHYS), ==, 2);
    g_assert_true(irq_asserted(qts));

    qtest_quit(qts);
    close(sv[0]);
}

/* An empty producer doorbell must not invent a completed descriptor. */
static void test_transmit_coalesce_ignores_empty_doorbell(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t got[sizeof(test_frame)];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);
    go_operational(qts);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);

    qtest_writel(qts, WINDOW_TIGON + TI_GCR_TX_MAX_COAL_BD, 2);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_TX_COAL_TICKS, 1000);

    qtest_memwrite(qts, TX_BUF_PHYS, test_frame, sizeof(test_frame));
    write_rcb_addr(qts, TX_RING_PHYS, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(test_frame) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);
    g_assert_cmpuint(net_recv_frame(sv[0], got, sizeof(got)), ==,
                     sizeof(test_frame));
    g_assert_false(irq_asserted(qts));

    qtest_clock_step(qts, 500000);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);
    g_assert_false(irq_asserted(qts));
    qtest_clock_step(qts, 600000);
    g_assert_true(irq_asserted(qts));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Only complete frames contribute their descriptors. A completed prefix may
 * contain multiple frames, while an unterminated tail stays at the consumer
 * and joins the descriptors supplied by the next doorbell.
 */
static void test_transmit_coalesce_counts_complete_frames(const void *data)
{
    enum { FIRST_LEN = 24 };
    int sv[2];
    QTestState *qts;
    uint8_t got[sizeof(test_frame)];
    unsigned i;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);
    go_operational(qts);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_TX_MAX_COAL_BD, 4);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_TX_COAL_TICKS, 1000000);

    for (i = 0; i < 5; i++) {
        write_rcb_addr(qts, TX_RING_PHYS + i * 16,
                       TX_BUF_PHYS + i * 0x100);
        qtest_writel(qts, TX_RING_PHYS + i * 16 + 12, 0);
    }
    qtest_memwrite(qts, TX_BUF_PHYS, test_frame, sizeof(test_frame));
    qtest_memwrite(qts, TX_BUF_PHYS + 0x100, test_frame,
                   sizeof(test_frame) / 2);
    qtest_memwrite(qts, TX_BUF_PHYS + 0x200,
                   test_frame + sizeof(test_frame) / 2,
                   sizeof(test_frame) / 2);
    qtest_memwrite(qts, TX_BUF_PHYS + 0x300, test_frame, FIRST_LEN);
    qtest_memwrite(qts, TX_BUF_PHYS + 0x400, test_frame + FIRST_LEN,
                   sizeof(test_frame) - FIRST_LEN);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(test_frame) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 24,
                 (uint32_t)(sizeof(test_frame) / 2) << 16);
    qtest_writel(qts, TX_RING_PHYS + 40,
                 ((uint32_t)(sizeof(test_frame) / 2) << 16) |
                 TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 56, (uint32_t)FIRST_LEN << 16);

    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 4);
    g_assert_cmpuint(net_recv_frame(sv[0], got, sizeof(got)), ==,
                     sizeof(test_frame));
    g_assert_cmpmem(got, sizeof(got), test_frame, sizeof(test_frame));
    g_assert_cmpuint(net_recv_frame(sv[0], got, sizeof(got)), ==,
                     sizeof(test_frame));
    g_assert_cmpmem(got, sizeof(got), test_frame, sizeof(test_frame));
    g_assert_cmphex(qtest_readl(qts, TX_CONS_PHYS), ==, 3);
    g_assert_false(irq_asserted(qts));

    qtest_writel(qts, TX_RING_PHYS + 72,
                 ((uint32_t)(sizeof(test_frame) - FIRST_LEN) << 16) |
                 TI_BDFLAG_END);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 5);
    g_assert_cmpuint(net_recv_frame(sv[0], got, sizeof(got)), ==,
                     sizeof(test_frame));
    g_assert_cmpmem(got, sizeof(got), test_frame, sizeof(test_frame));
    g_assert_cmphex(qtest_readl(qts, TX_CONS_PHYS), ==, 5);
    g_assert_true(irq_asserted(qts));

    qtest_quit(qts);
    close(sv[0]);
}

/* A failed frame must not join completed frames ahead of it in the batch. */
static void test_transmit_coalesce_stops_before_dma_error(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t got[sizeof(test_frame)];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev_128m(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);
    go_operational(qts);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_TX_MAX_COAL_BD, 2);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_TX_COAL_TICKS, 1000000);

    qtest_memwrite(qts, TX_BUF_PHYS, test_frame, sizeof(test_frame));
    write_rcb_addr(qts, TX_RING_PHYS, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(test_frame) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);
    write_rcb_addr(qts, TX_RING_PHYS + 16, UNMAPPED_PAYLOAD_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 24,
                 ((uint32_t)sizeof(test_frame) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 28, 0);

    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 2);
    g_assert_cmpuint(net_recv_frame(sv[0], got, sizeof(got)), ==,
                     sizeof(test_frame));
    g_assert_cmphex(qtest_readl(qts, TX_CONS_PHYS), ==, 1);
    g_assert_false(irq_asserted(qts));

    write_rcb_addr(qts, TX_RING_PHYS + 16, TX_BUF_PHYS);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 2);
    g_assert_cmpuint(net_recv_frame(sv[0], got, sizeof(got)), ==,
                     sizeof(test_frame));
    g_assert_cmphex(qtest_readl(qts, TX_CONS_PHYS), ==, 2);
    g_assert_true(irq_asserted(qts));

    qtest_quit(qts);
    close(sv[0]);
}

/* Reset cancels a pending TX batch and its coalescing deadline. */
static void test_transmit_coalesce_reset(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t got[sizeof(test_frame)];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);
    go_operational(qts);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_TX_MAX_COAL_BD, 2);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_TX_COAL_TICKS, 1000);

    qtest_memwrite(qts, TX_BUF_PHYS, test_frame, sizeof(test_frame));
    write_rcb_addr(qts, TX_RING_PHYS, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(test_frame) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);
    g_assert_cmpuint(net_recv_frame(sv[0], got, sizeof(got)), ==,
                     sizeof(test_frame));
    g_assert_false(irq_asserted(qts));

    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    g_assert_false(irq_asserted(qts));
    qtest_clock_step(qts, 2000000);
    g_assert_false(irq_asserted(qts));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Receive and transmit have distinct coalescing tick/count registers. A long
 * receive batch must not replace a shorter transmit deadline, and publishing
 * that transmit completion must leave the receive tail pending for its own
 * deadline. Sharing one timer between the directions loses both properties.
 */
static void test_interrupt_coalesce_directions_are_independent(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t got[sizeof(test_frame)];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);
    go_operational(qts);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);

    qtest_writel(qts, WINDOW_TIGON + TI_GCR_RX_MAX_COAL_BD, 100);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_RX_COAL_TICKS, 1000000);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_TX_MAX_COAL_BD, 100);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_TX_COAL_TICKS, 1000);

    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    g_assert_false(irq_asserted(qts));

    qtest_memwrite(qts, TX_BUF_PHYS, test_frame, sizeof(test_frame));
    write_rcb_addr(qts, TX_RING_PHYS, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(test_frame) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);
    g_assert_cmpuint(net_recv_frame(sv[0], got, sizeof(got)), ==,
                     sizeof(test_frame));
    g_assert_false(irq_asserted(qts));

    qtest_clock_step(qts, 2000000);
    g_assert_true(irq_asserted(qts));

    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    g_assert_false(irq_asserted(qts));
    qtest_clock_step(qts, 1000000000);
    g_assert_true(irq_asserted(qts));

    qtest_quit(qts);
    close(sv[0]);
}


/*
 * The SRAM timing configuration register. The driver sets its
 * synchronous-SRAM bit with a read-modify-write, so a register that answered
 * zero would make it read back something it had not written. Nothing acts on
 * the value: this model's SRAM is an array and has no timing to configure.
 */
static void test_misc_conf_reads_back(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    /*
     * Point the window at the BAR and let the device decode memory. Without
     * this the register block answers nothing at all, and a read-back test
     * reads the zero of an undecoded access rather than the register.
     */
    enable_dma_big_endian(qts);

    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + 0x050), ==, 0);
    qtest_writel(qts, WINDOW_TIGON + 0x050, 0x00100000);
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + 0x050), ==, 0x00100000);

    qtest_quit(qts);
}


/*
 * A ones-complement sum, written out here rather than shared with the device
 * so the test does not confirm the implementation against itself.
 */
static uint16_t ones_sum(const uint8_t *p, size_t len)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
    }
    if (i < len) {
        sum += (uint32_t)p[i] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)sum;
}

/*
 * The card reports what it summed over a received frame, and the driver uses
 * it instead of summing the bytes again on the processor. The public driver
 * reads both fields for any IP frame with no flag to gate them and inverts
 * them itself: "the Tigon firmware does not invert the checksum for us".
 * A well-formed header must arrive here as 0xffff, and a card reporting
 * zero is telling every driver of that shape that every packet is corrupt.
 */
static void test_receive_reports_checksums(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[74];
    uint32_t word4;
    uint16_t want_ip, want_l4;
    unsigned i;

    /* Ethernet + IPv4 + TCP, with a correct header checksum. */
    memcpy(frame, rx_frame_ours, 14);
    frame[12] = 0x08; frame[13] = 0x00;              /* IPv4 */
    memset(frame + 14, 0, sizeof(frame) - 14);
    frame[14] = 0x45;                                 /* v4, 5-word header */
    frame[16] = 0x00; frame[17] = sizeof(frame) - 14; /* total length */
    frame[22] = 64;                                   /* ttl */
    frame[23] = 6;                                    /* TCP */
    frame[26] = 10; frame[27] = 0; frame[28] = 2; frame[29] = 2;
    frame[30] = 10; frame[31] = 0; frame[32] = 2; frame[33] = 15;
    for (i = 34; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 7);
    }
    /* Complete the header checksum so the header is genuinely well-formed. */
    frame[24] = 0; frame[25] = 0;
    {
        uint16_t hs = ones_sum(frame + 14, 20) ^ 0xffff;
        frame[24] = hs >> 8; frame[25] = hs & 0xff;
    }
    want_ip = ones_sum(frame + 14, 20);
    want_l4 = ones_sum(frame + 34, sizeof(frame) - 34);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);
    go_operational(qts);

    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    net_send_frame(sv[0], frame, sizeof(frame));
    g_assert_cmphex(wait_return_prod(qts), ==, 1);

    /*
     * Big-endian word: header sum in the high half, payload sum in the low.
     */
    word4 = qtest_readl(qts, RETURN_RING_PHYS + 16);
    g_assert_cmphex(word4 >> 16, ==, 0xffff);
    g_assert_cmphex(word4 >> 16, ==, want_ip);
    g_assert_cmphex(word4 & 0xffff, ==, want_l4);

    qtest_quit(qts);
    close(sv[0]);
}

static void set_station_address(QTestState *qts, const uint8_t mac[6])
{
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_PAR0,
                 ((uint32_t)mac[0] << 8) | mac[1]);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_PAR1,
                 ((uint32_t)mac[2] << 24) |
                 ((uint32_t)mac[3] << 16) |
                 ((uint32_t)mac[4] << 8) | mac[5]);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_CMDRING,
                 (uint32_t)TI_CMD_SET_MAC_ADDR << 24);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_CMDPROD_IDX, 1);
}

/* The SET_MAC_ADDR command makes the address in PAR0/PAR1 active. */
static void test_set_mac_command_updates_filter(const void *data)
{
    static const uint8_t new_mac[6] = {
        0x52, 0x54, 0x00, 0x65, 0x43, 0x21,
    };
    int sv[2];
    QTestState *qts;
    uint8_t frame[sizeof(rx_frame_ours)];
    uint8_t landed[sizeof(rx_frame_ours)];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);
    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    set_station_address(qts, new_mac);

    /* Only the frame for the new address may consume the posted buffer. */
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    memcpy(frame, rx_frame_ours, sizeof(frame));
    memcpy(frame, new_mac, sizeof(new_mac));
    net_send_frame(sv[0], frame, sizeof(frame));

    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));

    qtest_quit(qts);
    close(sv[0]);
}

/* A chip reset makes the configured address active again. */
static void test_reset_restores_configured_mac(const void *data)
{
    static const uint8_t new_mac[6] = {
        0x52, 0x54, 0x00, 0x65, 0x43, 0x21,
    };
    int sv[2];
    QTestState *qts;
    uint8_t frame[sizeof(rx_frame_ours)];
    uint8_t landed[sizeof(rx_frame_ours)];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);
    set_station_address(qts, new_mac);
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");

    go_operational(qts);
    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    /* The pre-reset programmed address must no longer consume the buffer. */
    memcpy(frame, rx_frame_ours, sizeof(frame));
    memcpy(frame, new_mac, sizeof(new_mac));
    net_send_frame(sv[0], frame, sizeof(frame));
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));

    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed),
                    rx_frame_ours, sizeof(rx_frame_ours));

    qtest_quit(qts);
    close(sv[0]);
}

/* Migration preserves the station address currently filtering traffic. */
static void test_programmed_mac_survives_migration(const void *data)
{
    static const uint8_t new_mac[6] = {
        0x52, 0x54, 0x00, 0x65, 0x43, 0x21,
    };
    g_autofree char *tmpdir = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    int from_sv[2], to_sv[2];
    QTestState *from, *to;
    uint8_t frame[sizeof(rx_frame_ours)];
    uint8_t landed[sizeof(rx_frame_ours)];

    tmpdir = g_dir_make_tmp("sn0-tigon-migration-XXXXXX", &err);
    g_assert_no_error(err);
    socket = g_build_filename(tmpdir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, from_sv), ==, 0);
    from = tigon_init_netdev(from_sv[1]);
    close(from_sv[1]);
    go_operational(from);
    qtest_memset(from, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(from, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(from, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);
    set_station_address(from, new_mac);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, to_sv), ==, 0);
    dest_args = g_strdup_printf(
        "-M origin200 -netdev socket,fd=%d,id=n0 "
        "-device tigon,addr=3,mac=" TIGON_MAC ",netdev=n0 -incoming %s",
        to_sv[1], uri);
    to = qtest_init(dest_args);
    close(to_sv[1]);
    sgi_migrate_stopped(from, uri);

    /* The configured address must remain inactive after migration. */
    net_send_frame(to_sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    memcpy(frame, rx_frame_ours, sizeof(frame));
    memcpy(frame, new_mac, sizeof(new_mac));
    net_send_frame(to_sv[0], frame, sizeof(frame));
    g_assert_cmphex(wait_return_prod(to), ==, 1);
    qtest_memread(to, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));

    qtest_quit(to);
    qtest_quit(from);
    close(to_sv[0]);
    close(from_sv[0]);
    unlink(socket);
    g_assert_cmpint(rmdir(tmpdir), ==, 0);
}

static void write_split_tx_frame_at(QTestState *qts, unsigned first)
{
    qtest_memwrite(qts, TX_BUF_PHYS, test_frame, sizeof(test_frame) / 2);
    qtest_memwrite(qts, TX_BUF_PHYS + 0x100,
                   test_frame + sizeof(test_frame) / 2,
                   sizeof(test_frame) / 2);
    write_rcb_addr(qts, TX_RING_PHYS + first * 16, TX_BUF_PHYS);
    qtest_writel(qts, TX_RING_PHYS + first * 16 + 8,
                 (uint32_t)(sizeof(test_frame) / 2) << 16);
    qtest_writel(qts, TX_RING_PHYS + first * 16 + 12, 0);
    write_rcb_addr(qts, TX_RING_PHYS + (first + 1) * 16,
                   TX_BUF_PHYS + 0x100);
    qtest_writel(qts, TX_RING_PHYS + (first + 1) * 16 + 8,
                 ((uint32_t)(sizeof(test_frame) / 2) << 16) |
                 TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + (first + 1) * 16 + 12, 0);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, first + 2);
}

static void write_fragmented_tx_frame(QTestState *qts, unsigned first,
                                      unsigned count)
{
    enum { FRAGMENT_LEN = 20 };
    unsigned i;

    g_assert_cmpuint(first + count, <=, 512);
    qtest_memset(qts, TX_BUF_PHYS, 0x5a, count * FRAGMENT_LEN);
    for (i = 0; i < count; i++) {
        uint32_t desc = TX_RING_PHYS + (first + i) * 16;

        write_rcb_addr(qts, desc, TX_BUF_PHYS + i * FRAGMENT_LEN);
        qtest_writel(qts, desc + 8,
                     (FRAGMENT_LEN << 16) |
                     (i + 1 == count ? TI_BDFLAG_END : 0));
        qtest_writel(qts, desc + 12, 0);
    }
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, first + count);
}

/*
 * Locate the actual adjacent RX and TX coalescing counts in a migration
 * stream. The counts are deliberately awkward values produced through the
 * device interfaces, and the following one-millisecond TX deadline
 * distinguishes them from the adjacent ring indices.
 */
static void seed_near_limit_tx_count(const char *path)
{
    /*
     * The two counts, then the upper half of the transmit deadline that
     * follows them. Only the upper half: the deadline is the virtual clock
     * plus a millisecond, and waiting for the receives above runs that clock
     * on, so its low bytes are not a fixed value. Four zero bytes still
     * separate this from the adjacent ring indices, which the single-match
     * assertion below checks.
     */
    static const uint8_t pending_counts[] = {
        0x00, 0x00, 0x00, 0x25,
        0x00, 0x00, 0x00, 0x3d,
        0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t near_limit[] = { 0xff, 0xff, 0xff, 0xfd };
    g_autofree char *stream = NULL;
    GError *err = NULL;
    gsize stream_len;
    gsize match = 0;
    unsigned matches = 0;
    gsize i;

    g_assert_true(g_file_get_contents(path, &stream, &stream_len, &err));
    g_assert_no_error(err);
    for (i = 0; i + sizeof(pending_counts) <= stream_len; i++) {
        if (memcmp(stream + i, pending_counts, sizeof(pending_counts)) == 0) {
            match = i;
            matches++;
        }
    }
    g_assert_cmpuint(matches, ==, 1);
    memcpy(stream + match + 4, near_limit, sizeof(near_limit));
    g_assert_true(g_file_set_contents(path, stream, stream_len, &err));
    g_assert_no_error(err);
}

/*
 * A restored near-limit count must flush through the device without wrapping.
 * The interrupt must come from completing descriptors, and acknowledging it
 * before the retained deadline proves the same flush cancelled its timer.
 */
static void test_interrupt_coalesce_count_does_not_wrap(const void *data)
{
    enum {
        RX_PENDING = 37,
        TX_PENDING = 61,
        TX_COMPLETED = 3,
    };
    g_autofree char *tmpdir = NULL;
    g_autofree char *stream_path = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    int from_sv[2], to_sv[2];
    QTestState *from, *to;
    uint8_t got[TX_PENDING * 20];
    unsigned attempt;
    unsigned i;

    tmpdir = g_dir_make_tmp("sn0-tigon-overflow-migration-XXXXXX", &err);
    g_assert_no_error(err);
    stream_path = g_build_filename(tmpdir, "migration", NULL);
    uri = g_strdup_printf("file:%s", stream_path);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, from_sv), ==, 0);
    from = tigon_init_netdev(from_sv[1]);
    close(from_sv[1]);
    net_recv_timeout(from_sv[0], 5);
    go_operational(from);
    qtest_writel(from, WINDOW_TIGON + TI_MB_HOSTINTR, 0);

    qtest_writel(from, WINDOW_TIGON + TI_GCR_RX_MAX_COAL_BD, UINT32_MAX);
    qtest_writel(from, WINDOW_TIGON + TI_GCR_RX_COAL_TICKS, UINT32_MAX);
    for (i = 0; i < RX_PENDING; i++) {
        post_std_rx_buffer(from, i, RX_BUF_PHYS, i, 2048);
    }
    qtest_writel(from, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, RX_PENDING);
    for (i = 0; i < RX_PENDING; i++) {
        net_send_frame(from_sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    }
    for (attempt = 0; attempt < 200; attempt++) {
        if (qtest_readl(from, RETURN_PROD_PHYS) == RX_PENDING) {
            break;
        }
        g_usleep(5000);
        qtest_clock_step(from, RX_ARRIVAL_LATENCY_NS / 4);
    }
    g_assert_cmphex(qtest_readl(from, RETURN_PROD_PHYS), ==, RX_PENDING);

    qtest_writel(from, WINDOW_TIGON + TI_GCR_TX_MAX_COAL_BD, UINT32_MAX);
    qtest_writel(from, WINDOW_TIGON + TI_GCR_TX_COAL_TICKS, 1000);
    write_fragmented_tx_frame(from, 0, TX_PENDING);
    g_assert_cmpuint(net_recv_frame(from_sv[0], got, sizeof(got)), ==,
                     TX_PENDING * 20);
    g_assert_cmphex(qtest_readl(from, TX_CONS_PHYS), ==, TX_PENDING);
    g_assert_false(irq_asserted(from));

    sgi_migrate_stopped(from, uri);
    qtest_quit(from);
    close(from_sv[0]);
    seed_near_limit_tx_count(stream_path);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, to_sv), ==, 0);
    dest_args = g_strdup_printf(
        "-M origin200 -netdev socket,fd=%d,id=n0 "
        "-device tigon,addr=3,mac=" TIGON_MAC ",netdev=n0 -incoming %s",
        to_sv[1], uri);
    to = qtest_init(dest_args);
    close(to_sv[1]);
    net_recv_timeout(to_sv[0], 5);

    g_assert_false(irq_asserted(to));
    write_fragmented_tx_frame(to, TX_PENDING, TX_COMPLETED);
    g_assert_cmpuint(net_recv_frame(to_sv[0], got, sizeof(got)), ==,
                     TX_COMPLETED * 20);
    g_assert_cmphex(qtest_readl(to, TX_CONS_PHYS), ==,
                    TX_PENDING + TX_COMPLETED);
    g_assert_true(irq_asserted(to));

    qtest_writel(to, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    g_assert_false(irq_asserted(to));
    qtest_clock_step(to, 2000000);
    g_assert_false(irq_asserted(to));

    qtest_quit(to);
    close(to_sv[0]);
    unlink(stream_path);
    g_assert_cmpint(rmdir(tmpdir), ==, 0);
}

/*
 * Version 3 counted completed ring walks rather than descriptors. Its partial
 * TX count cannot be translated, so loading it must publish the completed
 * work instead of silently changing its distance from the threshold.
 */
static void test_transmit_coalesce_migration_from_v3(const void *data)
{
    const char *old_binary = g_getenv("TIGON_QEMU_V3_BINARY");
    g_autofree char *tmpdir = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    int from_sv[2], to_sv[2];
    QTestState *from, *to;
    uint8_t got[sizeof(test_frame)];

    if (!old_binary) {
        g_test_skip("TIGON_QEMU_V3_BINARY is not set");
        return;
    }

    tmpdir = g_dir_make_tmp("sn0-tigon-v3-migration-XXXXXX", &err);
    g_assert_no_error(err);
    socket = g_build_filename(tmpdir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, from_sv), ==, 0);
    from = tigon_init_netdev_binary(from_sv[1], old_binary);
    close(from_sv[1]);
    net_recv_timeout(from_sv[0], 5);
    go_operational(from);
    qtest_writel(from, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    qtest_writel(from, WINDOW_TIGON + TI_GCR_TX_MAX_COAL_BD, 4);
    qtest_writel(from, WINDOW_TIGON + TI_GCR_TX_COAL_TICKS, 1000);
    write_split_tx_frame_at(from, 0);
    g_assert_cmpuint(net_recv_frame(from_sv[0], got, sizeof(got)), ==,
                     sizeof(test_frame));
    g_assert_false(irq_asserted(from));

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, to_sv), ==, 0);
    dest_args = g_strdup_printf(
        "-M origin200 -netdev socket,fd=%d,id=n0 "
        "-device tigon,addr=3,mac=" TIGON_MAC ",netdev=n0 -incoming %s",
        to_sv[1], uri);
    to = qtest_init(dest_args);
    close(to_sv[1]);
    sgi_migrate_stopped(from, uri);

    g_assert_true(irq_asserted(to));
    qtest_writel(to, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    g_assert_false(irq_asserted(to));
    qtest_clock_step(to, 2000000);
    g_assert_false(irq_asserted(to));

    qtest_quit(to);
    qtest_quit(from);
    close(to_sv[0]);
    close(from_sv[0]);
    unlink(socket);
    g_assert_cmpint(rmdir(tmpdir), ==, 0);
}

/* A current stream retains the count accumulated before migration. */
static void test_transmit_coalesce_count_migration_current(const void *data)
{
    g_autofree char *tmpdir = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    int from_sv[2], to_sv[2];
    QTestState *from, *to;
    uint8_t got[sizeof(test_frame)];

    tmpdir = g_dir_make_tmp("sn0-tigon-current-migration-XXXXXX", &err);
    g_assert_no_error(err);
    socket = g_build_filename(tmpdir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, from_sv), ==, 0);
    from = tigon_init_netdev(from_sv[1]);
    close(from_sv[1]);
    net_recv_timeout(from_sv[0], 5);
    go_operational(from);
    qtest_writel(from, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    qtest_writel(from, WINDOW_TIGON + TI_GCR_TX_MAX_COAL_BD, 4);
    qtest_writel(from, WINDOW_TIGON + TI_GCR_TX_COAL_TICKS, 1000000);
    write_split_tx_frame_at(from, 0);
    g_assert_cmpuint(net_recv_frame(from_sv[0], got, sizeof(got)), ==,
                     sizeof(test_frame));
    g_assert_false(irq_asserted(from));

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, to_sv), ==, 0);
    dest_args = g_strdup_printf(
        "-M origin200 -netdev socket,fd=%d,id=n0 "
        "-device tigon,addr=3,mac=" TIGON_MAC ",netdev=n0 -incoming %s",
        to_sv[1], uri);
    to = qtest_init(dest_args);
    close(to_sv[1]);
    sgi_migrate_stopped(from, uri);

    g_assert_false(irq_asserted(to));
    net_recv_timeout(to_sv[0], 5);
    write_split_tx_frame_at(to, 2);
    g_assert_cmpuint(net_recv_frame(to_sv[0], got, sizeof(got)), ==,
                     sizeof(test_frame));
    g_assert_cmphex(qtest_readl(to, TX_CONS_PHYS), ==, 4);
    g_assert_true(irq_asserted(to));

    qtest_quit(to);
    qtest_quit(from);
    close(to_sv[0]);
    close(from_sv[0]);
    unlink(socket);
    g_assert_cmpint(rmdir(tmpdir), ==, 0);
}

/* A current stream retains the deadline independently of its count. */
static void test_transmit_coalesce_deadline_migration_current(const void *data)
{
    g_autofree char *tmpdir = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *dest_args = NULL;
    GError *err = NULL;
    int from_sv[2], to_sv[2];
    QTestState *from, *to;
    uint8_t got[sizeof(test_frame)];

    tmpdir = g_dir_make_tmp("sn0-tigon-current-migration-XXXXXX", &err);
    g_assert_no_error(err);
    socket = g_build_filename(tmpdir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, from_sv), ==, 0);
    from = tigon_init_netdev(from_sv[1]);
    close(from_sv[1]);
    net_recv_timeout(from_sv[0], 5);
    go_operational(from);
    qtest_writel(from, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    qtest_writel(from, WINDOW_TIGON + TI_GCR_TX_MAX_COAL_BD, 4);
    qtest_writel(from, WINDOW_TIGON + TI_GCR_TX_COAL_TICKS, 1000);
    write_split_tx_frame_at(from, 0);
    g_assert_cmpuint(net_recv_frame(from_sv[0], got, sizeof(got)), ==,
                     sizeof(test_frame));
    g_assert_false(irq_asserted(from));
    qtest_clock_step(from, 400000);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, to_sv), ==, 0);
    dest_args = g_strdup_printf(
        "-M origin200 -netdev socket,fd=%d,id=n0 "
        "-device tigon,addr=3,mac=" TIGON_MAC ",netdev=n0 -incoming %s",
        to_sv[1], uri);
    to = qtest_init(dest_args);
    close(to_sv[1]);
    sgi_migrate_stopped(from, uri);

    g_assert_false(irq_asserted(to));
    qtest_clock_step(to, 900000);
    g_assert_false(irq_asserted(to));
    qtest_clock_step(to, 200000);
    g_assert_true(irq_asserted(to));

    qtest_quit(to);
    qtest_quit(from);
    close(to_sv[0]);
    close(from_sv[0]);
    unlink(socket);
    g_assert_cmpint(rmdir(tmpdir), ==, 0);
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
 * Send and receive byte-exact IPv4 TCP, UDP and bare-protocol datagrams
 * through the rings. Other cases use ARP-like or hand-written frames. This
 * catches IP header damage or protocol-dependent handling on paths that
 * should ignore it.
 */
static void test_ip_carriage_both_directions(const void *data)
{
    static const struct { uint8_t proto; size_t l4_len; const char *what; }
    cases[] = {
        { 6,   20,  "TCP" },       /* a bare TCP header */
        { 17,  8,   "UDP" },       /* a bare UDP header */
        { 17,  512, "UDP bulk" },  /* a datagram past one word-copy chunk */
        { 6,   1400, "TCP bulk" }, /* close to a full-size frame */
        { 89,  40,  "OSPF" },      /* not TCP/UDP; carried */
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

        /*
         * Transmit: the frame leaves the card exactly as the guest wrote it.
         */
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




/*
 * A multicast frame: an IPv6 neighbour-discovery solicited-node address,
 * which is the case that matters. Group bit set in the first octet.
 */
static const uint8_t rx_frame_mcast[64] = {
    0x33, 0x33, 0xff, 0x12, 0x34, 0x56,      /* dest = IPv6 solicited-node */
    0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    0x86, 0xdd,                              /* ethertype IPv6 */
    0x60, 0x00, 0x00, 0x00,
};

/*
 * A real 63-byte dst/src/ethertype frame is not a multiple of 4, so the
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
 * path, the mirror of transmit_direct_map_check() for
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

    /*
     * GIB and the standard-receive/return rings reached through the window.
     */
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

/*
 * A multicast frame is delivered, not dropped. The exact group filter is not
 * modelled, so the card keeps every multicast frame and lets the stack sort
 * them out. This preserves multicast traffic when the filter is absent.
 * Dropping them all
 * unless the interface is promiscuous is not: a driver that has joined a
 * group gets nothing and nothing anywhere says why. IPv6 neighbour discovery
 * is carried entirely on solicited-node multicast, so it fails silently.
 */
static void test_receive_accepts_multicast(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t landed[sizeof(rx_frame_mcast)];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);

    /*
     * Not promiscuous: go_operational leaves the filter in its normal state.
     */
    net_send_frame(sv[0], rx_frame_mcast, sizeof(rx_frame_mcast));

    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed),
                    rx_frame_mcast, sizeof(rx_frame_mcast));

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
 * transparent: the chip delivers big-endian (OPMODE byte-swap bits set) and
 * the Bridge swap bit is set so the Bridge is a no-op;
 * corrected: the chip stays little-endian (OPMODE native) and the Bridge
 * swap bit is clear so the Bridge performs the single correction, as
 * the ISP1040 does behind this Bridge.
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
 * emulator. The mailbox values are ring indices, so the walk reduces them
 * into range rather than looping over an index it can never reach; if the
 * guard were missing, an index of four billion would spin. The test writes
 * such values and then proves the machine still answers, which it could not
 * do from inside a runaway walk.
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
 * The interrupt mask. The public driver writes 0 here under the comment "Turn
 * interrupts on", so zero permits and a non-zero value holds the line down.
 * Modelled because a register that is stored and never consulted answers
 * reads faithfully while ignoring what it was written for. The card would
 * deliver an interrupt into a driver that had just asked for silence.
 */
static void test_interrupt_mask_holds_the_line(const void *data)
{
    QTestState *qts = qtest_init(MACHINE_WITH_BRIDGE);

    go_operational(qts);
    g_assert_true(irq_asserted(qts));

    qtest_writel(qts, WINDOW_TIGON + TI_GCR_MASK_INTRS, 1);
    g_assert_false(irq_asserted(qts));

    /* Reads back what was written whether or not it is acted on. */
    g_assert_cmphex(qtest_readl(qts, WINDOW_TIGON + TI_GCR_MASK_INTRS), ==, 1);

    /* The pending cause is held, not lost: unmasking delivers it. */
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_MASK_INTRS, 0);
    g_assert_true(irq_asserted(qts));

    qtest_quit(qts);
}

/*
 * IRIX eg acknowledges with TI_MB_HOSTINTR=0; it never writes
 * TI_GCR_EVENTCONS_IDX or TI_GCR_RXRETURNCONS_IDX. NetBSD ti instead writes
 * mask value 1 at ISR entry (if_ti.c ti_intr()). Consumer-index catch-up left
 * INTA asserted, eg spinning on the write and ifconfig hung. The mailbox
 * write must drop INTA; a later completion must raise it again.
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

    /*
     * The driver's ISR: eg never touches the GCR consumers, only the mailbox.
     */
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);

    /* The line must drop from that write alone. */
    g_assert_false(irq_asserted(qts));

    /* A second, later completion still has to raise the line. */
    post_std_rx_buffer(qts, 1, RX_BUF_PHYS + 0x1000, 1, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 2);
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_cmphex(wait_return_prod_reaches(qts, 2), ==, 2);
    g_assert_true(irq_asserted(qts));

    /* And the same ack-with-zero protocol quiets it again. */
    qtest_writel(qts, WINDOW_TIGON + TI_MB_HOSTINTR, 0);
    g_assert_false(irq_asserted(qts));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * SET_PROMISC_MODE goes through the command ring and doorbell, not a
 * register. Once enabled, receive-filters-other accepts a frame for another
 * station. Public drivers pack the enable code in bits 23:12; another field
 * leaves the filter unchanged and the frame dropped.
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
                 (TI_CMD_CODE_PROMISC_ENB << 12));
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
 * The disable code, from the same field. A model that ignored the code and
 * toggled on the opcode alone, or read the field from the wrong bits, would
 * pass the enable test above and fail here: the second other-station frame
 * would land instead of being dropped.
 */
static void test_receive_promisc_disable_restores_filter(const void *data)
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
    qtest_memset(qts, RX_BUF_PHYS + 0x1000, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 1, RX_BUF_PHYS + 0x1000, 1, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 2);

    qtest_writel(qts, WINDOW_TIGON + TI_GCR_CMDRING,
                 (TI_CMD_SET_PROMISC_MODE << 24) |
                 (TI_CMD_CODE_PROMISC_ENB << 12));
    qtest_writel(qts, WINDOW_TIGON + TI_MB_CMDPROD_IDX, 1);

    net_send_frame(sv[0], rx_frame_other, sizeof(rx_frame_other));
    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(rx_frame_other));
    g_assert_cmpmem(landed, sizeof(rx_frame_other),
                    rx_frame_other, sizeof(rx_frame_other));

    qtest_writel(qts, WINDOW_TIGON + TI_GCR_CMDRING + 4,
                 (TI_CMD_SET_PROMISC_MODE << 24) |
                 (TI_CMD_CODE_PROMISC_DIS << 12));
    qtest_writel(qts, WINDOW_TIGON + TI_MB_CMDPROD_IDX, 2);

    /*
     * Another frame for the other station, then one for us. Both are read
     * from the backend in order; only ours may reach the ring, so the return
     * producer stops at 2 and the second buffer holds our frame.
     */
    net_send_frame(sv[0], rx_frame_other, sizeof(rx_frame_other));
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));

    g_assert_cmphex(wait_return_prod_reaches(qts, 2), ==, 2);
    qtest_memread(qts, RX_BUF_PHYS + 0x1000, landed, sizeof(rx_frame_ours));
    g_assert_cmpmem(landed, sizeof(rx_frame_ours),
                    rx_frame_ours, sizeof(rx_frame_ours));

    qtest_quit(qts);
    close(sv[0]);
}

/* Post one command word into the NIC-resident ring and ring its doorbell. */
static void post_command(QTestState *qts, unsigned slot, uint32_t word)
{
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_CMDRING + slot * 4, word);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_CMDPROD_IDX, slot + 1);
}

/* A 64-bit counter in a published block: most significant word first. */
static uint64_t read_stat64_at(QTestState *qts, uint32_t base, uint32_t offset)
{
    return ((uint64_t)qtest_readl(qts, base + offset) << 32) |
           qtest_readl(qts, base + offset + 4);
}

static uint64_t read_stat64(QTestState *qts, uint32_t offset)
{
    return read_stat64_at(qts, STATS_PHYS, offset);
}

/* Queue one transmit descriptor holding a whole frame. */
static void write_tx_frame(QTestState *qts, unsigned slot, uint32_t buf,
                           const uint8_t *frame, size_t len)
{
    qtest_memwrite(qts, buf, frame, len);
    write_rcb_addr(qts, TX_RING_PHYS + slot * 16, buf);
    qtest_writel(qts, TX_RING_PHYS + slot * 16 + 8,
                 ((uint32_t)len << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + slot * 16 + 12, 0);
}

/*
 * The frame and octet counters the firmware maintains. Three unicast and one
 * broadcast transmit, two unicast receives, all of sizes the test chose: the
 * refreshed block must report each direction's frames and the octets they
 * carried, with the broadcast frame counted apart from the unicast ones.
 */
static void test_statistics_traffic_counters(const void *data)
{
    static const unsigned tx_unicast = 3;
    static const unsigned rx_unicast = 2;
    uint8_t got[2048];
    QTestState *qts;
    unsigned i;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    for (i = 0; i < tx_unicast; i++) {
        write_tx_frame(qts, i, TX_BUF_PHYS, test_frame_direct,
                       sizeof(test_frame_direct));
    }
    write_tx_frame(qts, tx_unicast, TX_BUF_PHYS + 0x100, test_frame,
                   sizeof(test_frame));
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, tx_unicast + 1);

    for (i = 0; i < tx_unicast + 1; i++) {
        net_recv_frame(sv[0], got, sizeof(got));
    }

    for (i = 0; i < rx_unicast; i++) {
        post_std_rx_buffer(qts, i, RX_BUF_PHYS + i * 2048, i, 2048);
    }
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, rx_unicast);
    for (i = 0; i < rx_unicast; i++) {
        net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    }
    g_assert_cmphex(wait_return_prod_reaches(qts, rx_unicast), ==, rx_unicast);

    post_command(qts, 0, (uint32_t)TI_CMD_RFRSH_STATS << 24);

    g_assert_cmpuint(read_stat64(qts, STAT_HC_OUT_UCAST), ==, tx_unicast);
    g_assert_cmpuint(read_stat64(qts, STAT_HC_OUT_BROADCAST), ==, 1);
    g_assert_cmpuint(read_stat64(qts, STAT_HC_OUT_OCTETS), ==,
                     tx_unicast * sizeof(test_frame_direct) +
                     sizeof(test_frame));
    g_assert_cmpuint(read_stat64(qts, STAT_HC_IN_UCAST), ==, rx_unicast);
    g_assert_cmpuint(read_stat64(qts, STAT_HC_IN_BROADCAST), ==, 0);
    g_assert_cmpuint(read_stat64(qts, STAT_HC_IN_OCTETS), ==,
                     rx_unicast * sizeof(rx_frame_ours));

    /*
     * The same counters reach the block's own head, which is the copy the
     * IRIX board utility reads. A card that filled only the address the last
     * field names reported nothing to it.
     */
    g_assert_cmpuint(read_stat64_at(qts, GIB_PHYS, STAT_HC_OUT_UCAST), ==,
                     tx_unicast);
    g_assert_cmpuint(read_stat64_at(qts, GIB_PHYS, STAT_HC_IN_UCAST), ==,
                     rx_unicast);

    /*
     * A refresh announces itself on the event ring, the way the firmware-up
     * event did: without it a driver that waits for the answer to its refresh
     * request waits forever.
     */
    g_assert_cmphex(qtest_readl(qts, EV_PROD_PHYS), ==, 2);
    g_assert_cmphex(qtest_readl(qts, EVENT_RING_PHYS + 8) >> 24, ==,
                    TI_EV_STATS_UPDATED);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The interface section: the index and MTU the host published through the
 * register pair, the speed and operational state of the link, and the station
 * address. Read once before the link is negotiated and once after, so a model
 * that reported a gigabit link before there was one would fail the first
 * read.
 */
static void test_statistics_interface_identity(const void *data)
{
    static const uint32_t host_mtu = 1500;
    uint8_t phys_address[8];
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    qtest_writel(qts, WINDOW_TIGON + TI_GCR_IFINDEX, 1);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_IFMTU, host_mtu);

    post_command(qts, 0, (uint32_t)TI_CMD_RFRSH_STATS << 24);
    g_assert_cmpuint(qtest_readl(qts, STATS_PHYS + STAT_IF_SPEED), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, STATS_PHYS + STAT_IF_OPER_STATUS), ==,
                     IF_STATUS_DOWN);

    post_command(qts, 1, ((uint32_t)TI_CMD_HOST_STATE << 24) |
                 (TI_CMD_CODE_STACK_UP << 12));
    post_command(qts, 2, (uint32_t)TI_CMD_LINK_NEGOTIATION << 24);
    qtest_clock_step(qts, LINK_DELAY_NS);

    post_command(qts, 3, (uint32_t)TI_CMD_RFRSH_STATS << 24);
    g_assert_cmpuint(qtest_readl(qts, STATS_PHYS + STAT_IF_INDEX), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, STATS_PHYS + STAT_IF_MTU), ==, host_mtu);
    g_assert_cmpuint(qtest_readl(qts, STATS_PHYS + STAT_IF_SPEED), ==,
                     GIGABIT_BITS_PER_SECOND);
    g_assert_cmpuint(qtest_readl(qts, STATS_PHYS + STAT_IF_OPER_STATUS), ==,
                     IF_STATUS_UP);
    g_assert_cmpuint(qtest_readl(qts, STATS_PHYS + STAT_IF_ADMIN_STATUS), ==,
                     IF_STATUS_UP);

    /*
     * The station address occupies the last six bytes of the eight-byte
     * field, behind two zero bytes, the same padding the serial EEPROM puts
     * in front of it.
     */
    qtest_memread(qts, STATS_PHYS + STAT_IF_PHYS_ADDRESS, phys_address,
                  sizeof(phys_address));
    g_assert_cmphex(phys_address[0], ==, 0);
    g_assert_cmphex(phys_address[1], ==, 0);
    g_assert_cmpmem(phys_address + 2, sizeof(tigon_mac),
                    tigon_mac, sizeof(tigon_mac));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The copy at the information block's own head stays current without the host
 * asking for anything. The IRIX driver reads that copy and never sends a
 * refresh command, and its own refresh period is five seconds, so a card that
 * rewrote the head only when asked would answer a board utility with counters
 * from before the traffic it is being asked about.
 */
static void test_statistics_head_stays_current(const void *data)
{
    uint8_t got[2048];
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /*
     * Let the publish the device owes from reset happen before either half.
     */
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(read_stat64_at(qts, GIB_PHYS, STAT_HC_OUT_UCAST), ==, 0);
    g_assert_cmpuint(read_stat64_at(qts, GIB_PHYS, STAT_HC_IN_UCAST), ==, 0);

    write_tx_frame(qts, 0, TX_BUF_PHYS, test_frame_direct,
                   sizeof(test_frame_direct));
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);
    net_recv_frame(sv[0], got, sizeof(got));

    /*
     * No command, no refresh request, no statistics-tick period programmed.
     */
    qtest_clock_step(qts, STATS_LIVE_NS * 2);

    g_assert_cmpuint(read_stat64_at(qts, GIB_PHYS, STAT_HC_OUT_UCAST), ==, 1);
    g_assert_cmpuint(read_stat64_at(qts, GIB_PHYS, STAT_HC_OUT_OCTETS), ==,
                     sizeof(test_frame_direct));
    g_assert_cmpuint(read_stat64_at(qts, GIB_PHYS, STAT_HC_IN_UCAST), ==, 0);

    /* The receive side counts in a quiet period of its own. */
    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    qtest_clock_step(qts, STATS_LIVE_NS * 2);

    g_assert_cmpuint(read_stat64_at(qts, GIB_PHYS, STAT_HC_IN_UCAST), ==, 1);
    g_assert_cmpuint(read_stat64_at(qts, GIB_PHYS, STAT_HC_IN_OCTETS), ==,
                     sizeof(rx_frame_ours));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Update the head's interface fields on change, not only its counters. IRIX
 * sends no refresh command; counter-only publication can leave link state
 * down for five seconds. Separate quiet periods expose stale fields. A
 * refresh would republish all fields and hide a missed change.
 */
static void test_statistics_head_tracks_interface_state(const void *data)
{
    static const uint32_t host_mtu = 1518;
    static const uint8_t new_mac[6] = {
        0x52, 0x54, 0x00, 0x65, 0x43, 0x21,
    };
    uint8_t phys_address[8];
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /*
     * Let the publish the device owes from its own reset happen first, so the
     * register writes below are the only change in their quiet period.
     */
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_INDEX), ==, 0);

    /*
     * The index and the size are separate registers with separate marks, so
     * they get separate quiet periods: written together, either one's mark
     * would carry the other to the head and neither would be pinned.
     */
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_IFINDEX, 1);
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_INDEX), ==, 1);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_MTU), ==, 0);

    qtest_writel(qts, WINDOW_TIGON + TI_GCR_IFMTU, host_mtu);
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_MTU), ==, host_mtu);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_ADMIN_STATUS), ==,
                     IF_STATUS_DOWN);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_SPEED), ==, 0);

    /* The stack coming up, which only the host-state command reports. */
    post_command(qts, 0, ((uint32_t)TI_CMD_HOST_STATE << 24) |
                 (TI_CMD_CODE_STACK_UP << 12));
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_ADMIN_STATUS), ==,
                     IF_STATUS_UP);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_SPEED), ==, 0);

    /* The link, which arrives later on the firmware's own timer. */
    post_command(qts, 1, (uint32_t)TI_CMD_LINK_NEGOTIATION << 24);
    qtest_clock_step(qts, LINK_DELAY_NS);
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_SPEED), ==,
                     GIGABIT_BITS_PER_SECOND);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_OPER_STATUS), ==,
                     IF_STATUS_UP);

    /*
     * The block reads its speed and operational state out of the gigabit
     * link-status register, whichever side last wrote it. The contract calls
     * that register firmware-owned, so this is the model staying consistent
     * with itself. It does not claim a card accepts the write, but it is the
     * only check of the register's own mark.
     */
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_GLINK_STAT, 0);
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_SPEED), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_OPER_STATUS), ==,
                     IF_STATUS_DOWN);

    /* A new station address, made active by its own command. */
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_PAR0,
                 ((uint32_t)new_mac[0] << 8) | new_mac[1]);
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_PAR1,
                 ((uint32_t)new_mac[2] << 24) | ((uint32_t)new_mac[3] << 16) |
                 ((uint32_t)new_mac[4] << 8) | new_mac[5]);
    post_command(qts, 2, (uint32_t)TI_CMD_SET_MAC_ADDR << 24);
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    qtest_memread(qts, GIB_PHYS + STAT_IF_PHYS_ADDRESS, phys_address,
                  sizeof(phys_address));
    g_assert_cmphex(phys_address[0], ==, 0);
    g_assert_cmphex(phys_address[1], ==, 0);
    g_assert_cmpmem(phys_address + 2, sizeof(new_mac),
                    new_mac, sizeof(new_mac));

    qtest_quit(qts);
    close(sv[0]);
}

/* Step the live interval until the head's discard count reaches want. */
static uint32_t wait_head_discards_reaches(QTestState *qts, uint32_t want)
{
    int attempt;

    for (attempt = 0; attempt < 200; attempt++) {
        if (qtest_readl(qts, GIB_PHYS + STAT_IF_IN_DISCARDS) == want) {
            break;
        }
        g_usleep(5000);
        qtest_clock_step(qts, STATS_LIVE_NS);
    }
    return qtest_readl(qts, GIB_PHYS + STAT_IF_IN_DISCARDS);
}

/*
 * A frame dropped after the address filter accepted it is a published field
 * of its own, and reaches the head on the same terms as the counters. The
 * model has two such drops and they are marked separately, so both run here:
 * a descriptor offering less room than a minimum-length frame, and one
 * pointing at an address the payload write cannot reach. Neither consumes the
 * descriptor, so the second reuses the first's slot without a new doorbell.
 */
static void test_statistics_head_tracks_discards(const void *data)
{
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev_128m(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /* Same quiet period as above: let reset's own publish happen first. */
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_IN_DISCARDS), ==, 0);

    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 8);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_cmpuint(wait_head_discards_reaches(qts, 1), ==, 1);

    post_std_rx_buffer(qts, 0, UNMAPPED_PAYLOAD_PHYS, 0, 2048);
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_cmpuint(wait_head_discards_reaches(qts, 2), ==, 2);

    g_assert_cmphex(qtest_readl(qts, RETURN_PROD_PHYS), ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A clear reaches the head on its own. The reader that needs this never sends
 * a refresh, so a clear the host cannot see until it asks leaves the utility
 * printing the counts the operator just cleared. No refresh command is posted
 * after the clear here, for the same reason as above.
 */
static void test_statistics_clear_reaches_head(const void *data)
{
    uint8_t got[2048];
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    write_tx_frame(qts, 0, TX_BUF_PHYS, test_frame_direct,
                   sizeof(test_frame_direct));
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);
    net_recv_frame(sv[0], got, sizeof(got));
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(read_stat64_at(qts, GIB_PHYS, STAT_HC_OUT_UCAST), ==, 1);

    post_command(qts, 0, (uint32_t)TI_CMD_CLR_STATS << 24);
    qtest_clock_step(qts, STATS_LIVE_NS * 2);

    g_assert_cmpuint(read_stat64_at(qts, GIB_PHYS, STAT_HC_OUT_UCAST), ==, 0);
    g_assert_cmpuint(read_stat64_at(qts, GIB_PHYS, STAT_HC_OUT_OCTETS), ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/* A chip reset zeroes the counters, the way the clear command does. */
static void test_reset_clears_statistics(const void *data)
{
    uint8_t got[2048];
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    write_tx_frame(qts, 0, TX_BUF_PHYS, test_frame_direct,
                   sizeof(test_frame_direct));
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);
    net_recv_frame(sv[0], got, sizeof(got));

    post_command(qts, 0, (uint32_t)TI_CMD_RFRSH_STATS << 24);
    g_assert_cmpuint(read_stat64(qts, STAT_HC_OUT_UCAST), ==, 1);

    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");

    /* The block is rebuilt, so the count can only be the model's own. */
    go_operational(qts);
    post_command(qts, 0, (uint32_t)TI_CMD_RFRSH_STATS << 24);
    g_assert_cmpuint(read_stat64(qts, STAT_HC_OUT_UCAST), ==, 0);
    g_assert_cmpuint(read_stat64(qts, STAT_HC_OUT_OCTETS), ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Byte order of the published block, on the pairing the IRIX guest actually
 * programs: the chip's descriptor byte swap off and the Bridge slot doing the
 * single correction. Every field has to read big-endian in guest memory, and
 * a 64-bit counter's two words have to arrive most significant first. Written
 * as one quantity in the chip's own order instead, each count arrives
 * multiplied by 2^32, which the transparent pairing above cannot see,
 * because there the two spellings are the same bytes.
 */
static void test_statistics_byte_order(const void *data)
{
    uint8_t seen[8];
    QTestState *qts;
    uint8_t got[2048];
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    aim_window_at_bar(qts);
    qtest_writel(qts, CFG0(TIGON_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    qtest_writel(qts, BRIDGE_DIRECT_MAP_REG, 0);
    qtest_writel(qts, BRIDGE_DEVICE(TIGON_SLOT),
                 DEV_MEMORY_SPACE | (ADAPTER_MMIO >> 20));
    qtest_writel(qts, WINDOW_TIGON + TI_GCR_OPMODE, 0);

    qtest_memset(qts, GIB_PHYS, 0, GIB_SIZE);
    qtest_memset(qts, TX_RING_PHYS, 0, 0x800);
    qtest_memset(qts, TX_CONS_PHYS, 0, 8);
    qtest_memset(qts, EVENT_RING_PHYS, 0, 0x800);
    qtest_memset(qts, EV_PROD_PHYS, 0, 8);
    qtest_memset(qts, STATS_PHYS, 0, STATS_SIZE);
    qtest_writeq(qts, GIB_PHYS + GIB_EV_RCB, IOVA(EVENT_RING_PHYS));
    qtest_writeq(qts, GIB_PHYS + GIB_EV_PROD_PTR, IOVA(EV_PROD_PHYS));
    qtest_writeq(qts, GIB_PHYS + GIB_TX_RCB, IOVA(TX_RING_PHYS));
    qtest_writel(qts, GIB_PHYS + GIB_TX_RCB + 8,
                 (512u << 16) | TI_RCB_FLAG_HOST_RING);
    qtest_writeq(qts, GIB_PHYS + GIB_TX_CONS_PTR, IOVA(TX_CONS_PHYS));
    qtest_writeq(qts, GIB_PHYS + GIB_REFRESH_STATS_PTR, IOVA(STATS_PHYS));

    publish_gib_at(qts, IOVA(GIB_PHYS));
    release_cpu(qts);

    qtest_memwrite(qts, TX_BUF_DIRECT_PHYS, test_frame_direct,
                   sizeof(test_frame_direct));
    qtest_writeq(qts, TX_RING_PHYS + 0, IOVA(TX_BUF_DIRECT_PHYS));
    qtest_writel(qts, TX_RING_PHYS + 8,
                 ((uint32_t)sizeof(test_frame_direct) << 16) | TI_BDFLAG_END);
    qtest_writel(qts, TX_RING_PHYS + 12, 0);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);
    g_assert_cmpint(net_recv_frame(sv[0], got, sizeof(got)), ==,
                    (int)sizeof(test_frame_direct));

    post_command(qts, 0, (uint32_t)TI_CMD_LINK_NEGOTIATION << 24);
    qtest_clock_step(qts, LINK_DELAY_NS);
    post_command(qts, 1, (uint32_t)TI_CMD_RFRSH_STATS << 24);

    /*
     * One transmitted unicast frame: the count's 1 belongs in the last byte.
     */
    qtest_memread(qts, STATS_PHYS + STAT_HC_OUT_UCAST, seen, sizeof(seen));
    g_assert_cmphex(seen[0], ==, 0x00);
    g_assert_cmphex(seen[1], ==, 0x00);
    g_assert_cmphex(seen[2], ==, 0x00);
    g_assert_cmphex(seen[3], ==, 0x00);
    g_assert_cmphex(seen[4], ==, 0x00);
    g_assert_cmphex(seen[5], ==, 0x00);
    g_assert_cmphex(seen[6], ==, 0x00);
    g_assert_cmphex(seen[7], ==, 0x01);

    /* A gigabit in bits a second, big-endian: 1000000000 is 0x3b9aca00. */
    qtest_memread(qts, STATS_PHYS + STAT_IF_SPEED, seen, 4);
    g_assert_cmphex(seen[0], ==, 0x3b);
    g_assert_cmphex(seen[1], ==, 0x9a);
    g_assert_cmphex(seen[2], ==, 0xca);
    g_assert_cmphex(seen[3], ==, 0x00);

    /* The station address, behind its two bytes of padding. */
    qtest_memread(qts, STATS_PHYS + STAT_IF_PHYS_ADDRESS, seen, sizeof(seen));
    g_assert_cmphex(seen[0], ==, 0x00);
    g_assert_cmphex(seen[1], ==, 0x00);
    g_assert_cmpmem(seen + 2, sizeof(tigon_mac), tigon_mac, sizeof(tigon_mac));

    qtest_quit(qts);
    close(sv[0]);
}

/* The clear command zeroes the counters the next refresh publishes. */
static void test_statistics_clear(const void *data)
{
    uint8_t got[2048];
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    write_tx_frame(qts, 0, TX_BUF_PHYS, test_frame_direct,
                   sizeof(test_frame_direct));
    qtest_writel(qts, WINDOW_TIGON + TI_MB_SENDPROD_IDX, 1);
    net_recv_frame(sv[0], got, sizeof(got));

    post_command(qts, 0, (uint32_t)TI_CMD_RFRSH_STATS << 24);
    g_assert_cmpuint(read_stat64(qts, STAT_HC_OUT_UCAST), ==, 1);

    post_command(qts, 1, (uint32_t)TI_CMD_CLR_STATS << 24);
    post_command(qts, 2, (uint32_t)TI_CMD_RFRSH_STATS << 24);
    g_assert_cmpuint(read_stat64(qts, STAT_HC_OUT_UCAST), ==, 0);
    g_assert_cmpuint(read_stat64(qts, STAT_HC_OUT_OCTETS), ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A completion that arrives while the host interrupt is masked must still
 * raise the line when the driver re-enables it. The Tigon 2 driver masks by
 * writing 1 to the interrupt mailbox and re-enables by writing 0; a frame
 * delivered in between sets the pending latch, and the re-enable, distinct
 * from IRIX's zero-only acknowledge, must not drop it.
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

/*
 * With the jumbo ring configured but empty, a frame between standard and
 * jumbo sizes has no descriptor. Discard it, increment RFC 1213 ifInDiscards
 * and nicNoMoreRxBDs (ti_stats word 100), and keep serving standard frames.
 * The independent ring control blocks and index pairs preclude truncating
 * into a standard buffer or blocking its queue. Public sources do not
 * establish whether real firmware holds a frame for bounded refill. The model
 * cannot retain it without blocking the standard ring; posting a jumbo buffer
 * must not revive it.
 */
static void test_receive_jumbo_ring_unprovisioned(const void *data)
{
    enum { JUMBO_LEN = 4000 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[JUMBO_LEN], landed[JUMBO_LEN], zeros[JUMBO_LEN];
    unsigned n;
    uint32_t word2, word3;

    memcpy(frame, rx_frame_ours, 14);
    for (n = 14; n < JUMBO_LEN; n++) {
        frame[n] = (uint8_t)(n * 9 + (n >> 6));
    }
    memset(zeros, 0, sizeof(zeros));

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /*
     * Two standard buffers, so the ring outlives the frame that is dropped.
     */
    qtest_memset(qts, RX_BUF_PHYS, 0, 4096);
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    post_std_rx_buffer(qts, 1, RX_BUF_PHYS + 2048, 1, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 2);

    /*
     * The jumbo ring's control block is published by the general information
     * block, the way a driver publishes it at attach; no buffer is posted and
     * neither doorbell that would post one is written.
     */
    qtest_memset(qts, JUMBO_BUF_PHYS, 0, 16384);
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_IN_DISCARDS), ==, 0);

    net_send_frame(sv[0], frame, sizeof(frame));
    g_assert_cmpuint(wait_head_discards_reaches(qts, 1), ==, 1);

    /* No completion, and neither buffer written. */
    assert_return_prod_stays(qts, 0);
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), zeros, sizeof(landed));

    /* The receiver is not latched: the frame behind the dropped one lands. */
    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    word2 = qtest_readl(qts, RETURN_RING_PHYS + 8);
    g_assert_cmphex(word2 >> 16, ==, 0);
    g_assert_cmphex(word2 & 0xffff, ==, sizeof(rx_frame_ours));
    word3 = qtest_readl(qts, RETURN_RING_PHYS + 12);
    g_assert_cmphex(word3 >> 16, ==, TI_BDTYPE_RECV_BD);
    g_assert_cmphex(word3 & TI_BDFLAG_JUMBO_RING, ==, 0);
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(rx_frame_ours));
    g_assert_cmpmem(landed, sizeof(rx_frame_ours),
                    rx_frame_ours, sizeof(rx_frame_ours));

    /* Posting a jumbo buffer now brings nothing back: that frame is gone. */
    write_rcb_addr(qts, JUMBO_RX_RING_PHYS + 0, JUMBO_BUF_PHYS);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 8, (3u << 16) | 16222);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 12,
                 (uint32_t)TI_BDTYPE_RECV_JUMBO_BD << 16);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_JUMBORXPROD_IDX, 1);
    assert_return_prod_stays(qts, 1);
    qtest_memread(qts, JUMBO_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), zeros, sizeof(landed));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Discard a standard frame only if the jumbo ring could receive it;
 * otherwise refuse it so the caller can retry instead of losing it silently.
 * Cover a published but empty jumbo ring with neither ring posted, and a
 * doorbell rung while its control block is zero. In the latter case, posted
 * buffers remain unreachable: tigon_can_receive checks neither base, and
 * tigon_parse_gib reads the jumbo base only at firmware start. Both states
 * must hold the frame until a standard buffer arrives. Other tests post
 * jumbo buffers and cannot detect removal of this guard.
 */
static void test_standard_shortage_guard_holds(const void *data)
{
    enum { JUMBO_LEN = 4000 };
    int sv[2];
    QTestState *qts;
    uint8_t landed[sizeof(rx_frame_ours)];
    uint8_t big[JUMBO_LEN];
    unsigned n;

    /* Neither ring posted: the jumbo control block is published and empty. */
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);
    go_operational(qts);
    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);

    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    assert_return_prod_stays(qts, 0);
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_IN_DISCARDS), ==, 0);

    /* Held, not discarded: posting the standard ring delivers it. */
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);
    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), rx_frame_ours, sizeof(landed));
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_IN_DISCARDS), ==, 0);
    qtest_quit(qts);
    close(sv[0]);

    /* The jumbo doorbell rung with the ring's control block left at zero. */
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    enable_dma_big_endian(qts);
    build_gib(qts);
    write_rcb_addr(qts, GIB_PHYS + GIB_JUMBO_RX_RCB, 0);
    publish_gib(qts);
    release_cpu(qts);
    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_JUMBORXPROD_IDX, 1);

    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    assert_return_prod_stays(qts, 0);
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_IN_DISCARDS), ==, 0);

    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);
    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), rx_frame_ours, sizeof(landed));
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_IN_DISCARDS), ==, 0);
    qtest_quit(qts);
    close(sv[0]);

    /*
     * The third state reaches the index check: a queued jumbo frame consumes
     * its buffer, then the backend calls receive() for the standard frame
     * without rechecking can_receive(). Both rings are empty but the jumbo
     * base remains published. The first case never enters receive(); the
     * second stops at the missing-base check.
     * Send both frames in one socket write. The backend finalizes both in
     * one read while can_receive() is false, then doorbell flush delivers
     * them back to back. Separate writes can let qemu_net_queue_send recheck
     * can_receive() and queue the second frame, bypassing the index check
     * while all assertions still pass.
     */
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);
    go_operational(qts);
    qtest_memset(qts, RX_BUF_PHYS, 0, 2048);
    qtest_memset(qts, JUMBO_BUF_PHYS, 0, 16384);

    for (n = 14; n < JUMBO_LEN; n++) {
        big[n] = (uint8_t)(n * 3 + (n >> 5));
    }
    memcpy(big, rx_frame_ours, 14);
    {
        uint8_t both[8 + JUMBO_LEN + sizeof(rx_frame_ours)];
        size_t len = 0;
        uint32_t hdr;

        hdr = htonl((uint32_t)JUMBO_LEN);
        memcpy(both + len, &hdr, 4);
        len += 4;
        memcpy(both + len, big, JUMBO_LEN);
        len += JUMBO_LEN;
        hdr = htonl((uint32_t)sizeof(rx_frame_ours));
        memcpy(both + len, &hdr, 4);
        len += 4;
        memcpy(both + len, rx_frame_ours, sizeof(rx_frame_ours));
        len += sizeof(rx_frame_ours);
        g_assert_cmpint(send(sv[0], both, len, 0), ==, (int)len);
    }

    /* Exactly one jumbo buffer: the frame in front consumes it. */
    write_rcb_addr(qts, JUMBO_RX_RING_PHYS + 0, JUMBO_BUF_PHYS);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 8, (5u << 16) | 16222);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 12,
                 (uint32_t)TI_BDTYPE_RECV_JUMBO_BD << 16);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_JUMBORXPROD_IDX, 1);

    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    assert_return_prod_stays(qts, 1);
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_IN_DISCARDS), ==, 0);

    /* Held, not discarded: the standard ring delivers it. */
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);
    g_assert_cmphex(wait_return_prod_reaches(qts, 2), ==, 2);
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), rx_frame_ours, sizeof(landed));
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_IN_DISCARDS), ==, 0);
    qtest_quit(qts);
    close(sv[0]);
}

/*
 * With a jumbo buffer but no standard buffer, discard and count a 64-byte
 * frame without consuming either buffer. The next jumbo frame completes;
 * posting a standard buffer cannot revive the discarded frame. Completion
 * rejects a hold; the later post rejects deferred delivery. The frozen-clock
 * case checks discard timing.
 */
static void test_receive_standard_ring_unprovisioned(const void *data)
{
    enum { JUMBO_LEN = 4000 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[JUMBO_LEN], landed[JUMBO_LEN], zeros[JUMBO_LEN];
    unsigned n;
    uint32_t word2, word3;

    memcpy(frame, rx_frame_ours, 14);
    for (n = 14; n < JUMBO_LEN; n++) {
        frame[n] = (uint8_t)(n * 9 + (n >> 6));
    }
    memset(zeros, 0, sizeof(zeros));

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /* One jumbo buffer, and no standard buffer at all. */
    qtest_memset(qts, RX_BUF_PHYS, 0, 4096);
    qtest_memset(qts, JUMBO_BUF_PHYS, 0, 16384);
    write_rcb_addr(qts, JUMBO_RX_RING_PHYS + 0, JUMBO_BUF_PHYS);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 8, (3u << 16) | 16222);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 12,
                 (uint32_t)TI_BDTYPE_RECV_JUMBO_BD << 16);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_JUMBORXPROD_IDX, 1);

    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(qtest_readl(qts, GIB_PHYS + STAT_IF_IN_DISCARDS), ==, 0);

    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    g_assert_cmpuint(wait_head_discards_reaches(qts, 1), ==, 1);

    /* No completion, and neither ring's buffer written. */
    assert_return_prod_stays(qts, 0);
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(rx_frame_ours));
    g_assert_cmpmem(landed, sizeof(rx_frame_ours), zeros,
                    sizeof(rx_frame_ours));
    qtest_memread(qts, JUMBO_BUF_PHYS, landed, sizeof(rx_frame_ours));
    g_assert_cmpmem(landed, sizeof(rx_frame_ours), zeros,
                    sizeof(rx_frame_ours));

    /* The receiver is not latched: the frame behind the dropped one lands. */
    net_send_frame(sv[0], frame, sizeof(frame));
    g_assert_cmphex(wait_return_prod(qts), ==, 1);
    word2 = qtest_readl(qts, RETURN_RING_PHYS + 8);
    g_assert_cmphex(word2 >> 16, ==, 3);
    g_assert_cmphex(word2 & 0xffff, ==, sizeof(frame));
    word3 = qtest_readl(qts, RETURN_RING_PHYS + 12);
    g_assert_cmphex(word3 >> 16, ==, TI_BDTYPE_RECV_JUMBO_BD);
    g_assert_cmphex(word3 & TI_BDFLAG_JUMBO_RING, ==, TI_BDFLAG_JUMBO_RING);
    qtest_memread(qts, JUMBO_BUF_PHYS, landed, sizeof(frame));
    g_assert_cmpmem(landed, sizeof(frame), frame, sizeof(frame));

    /*
     * Posting a standard buffer now brings nothing back: that frame is gone.
     */
    post_std_rx_buffer(qts, 0, RX_BUF_PHYS, 0, 2048);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_STDRXPROD_IDX, 1);
    assert_return_prod_stays(qts, 1);
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(rx_frame_ours));
    g_assert_cmpmem(landed, sizeof(rx_frame_ours), zeros,
                    sizeof(rx_frame_ours));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * With only a jumbo buffer, discard a standard-size frame immediately and
 * count an input discard. Size selects the ring; standard max_len only routes
 * large frames to jumbo. With virtual time frozen, the queued jumbo frame
 * passes only after immediate discard. A bounded hold previously measured on
 * this MTU 1500 path cost throughput; this assertion would catch its return.
 */
static void test_standard_shortage_discards_without_waiting(const void *data)
{
    enum { JUMBO_LEN = 4000 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[JUMBO_LEN], landed[JUMBO_LEN], zeros[JUMBO_LEN];
    unsigned n;
    uint32_t word2, word3;

    memcpy(frame, rx_frame_ours, 14);
    for (n = 14; n < JUMBO_LEN; n++) {
        frame[n] = (uint8_t)(n * 9 + (n >> 6));
    }
    memset(zeros, 0, sizeof(zeros));

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = tigon_init_netdev_no_arrival_floor(sv[1]);
    close(sv[1]);
    net_recv_timeout(sv[0], 5);

    go_operational(qts);

    /* One jumbo buffer, and no standard buffer at all. */
    qtest_memset(qts, RX_BUF_PHYS, 0, 4096);
    qtest_memset(qts, JUMBO_BUF_PHYS, 0, 16384);
    write_rcb_addr(qts, JUMBO_RX_RING_PHYS + 0, JUMBO_BUF_PHYS);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 8, (3u << 16) | 16222);
    qtest_writel(qts, JUMBO_RX_RING_PHYS + 12,
                 (uint32_t)TI_BDTYPE_RECV_JUMBO_BD << 16);
    qtest_writel(qts, WINDOW_TIGON + TI_MB_JUMBORXPROD_IDX, 1);

    net_send_frame(sv[0], rx_frame_ours, sizeof(rx_frame_ours));
    net_send_frame(sv[0], frame, sizeof(frame));

    /*
     * No qtest_clock_step anywhere: only a discard gets the second frame in.
     */
    g_assert_cmphex(return_prod_without_clock(qts, 1), ==, 1);
    word2 = qtest_readl(qts, RETURN_RING_PHYS + 8);
    g_assert_cmphex(word2 >> 16, ==, 3);
    g_assert_cmphex(word2 & 0xffff, ==, sizeof(frame));
    word3 = qtest_readl(qts, RETURN_RING_PHYS + 12);
    g_assert_cmphex(word3 >> 16, ==, TI_BDTYPE_RECV_JUMBO_BD);
    g_assert_cmphex(word3 & TI_BDFLAG_JUMBO_RING, ==, TI_BDFLAG_JUMBO_RING);
    qtest_memread(qts, JUMBO_BUF_PHYS, landed, sizeof(frame));
    g_assert_cmpmem(landed, sizeof(frame), frame, sizeof(frame));

    /* The standard buffer was never written: that frame is gone. */
    qtest_memread(qts, RX_BUF_PHYS, landed, sizeof(rx_frame_ours));
    g_assert_cmpmem(landed, sizeof(rx_frame_ours), zeros,
                    sizeof(rx_frame_ours));

    /* The discard is counted where the card publishes its input discards. */
    qtest_clock_step(qts, STATS_LIVE_NS * 2);
    g_assert_cmpuint(wait_head_discards_reaches(qts, 1), ==, 1);

    qtest_quit(qts);
    close(sv[0]);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/sn0/tigon/one-memory-bar", NULL,
                        test_one_memory_bar);
    qtest_add_data_func("/sn0/tigon/misc-host-ctl-chip-rev", NULL,
                        test_misc_host_ctl_chip_rev);
    qtest_add_data_func("/sn0/tigon/geninfo-upper-half", NULL,
                        test_geninfo_upper_half);
    qtest_add_data_func("/sn0/tigon/firmware-upload-window-posts-event", NULL,
                        test_firmware_upload_window_posts_event);
    qtest_add_data_func("/sn0/tigon/user-backend-reply-waits-for-transmit",
                        NULL, test_user_backend_reply_waits_for_transmit);
    qtest_add_data_func("/sn0/tigon/descriptor-dma-batches-producer-last",
                        NULL, test_descriptor_dma_batches_producer_last);
    qtest_add_data_func("/sn0/tigon/descriptor-dma-route-used-for-traffic",
                        NULL, test_descriptor_dma_route_used_for_traffic);
    qtest_add_data_func("/sn0/tigon/transmit-payload-dma-error", NULL,
                        test_transmit_payload_dma_error);
    qtest_add_data_func("/sn0/tigon/transmit-fragment-dma-error-retry", NULL,
                        test_transmit_fragment_dma_error_retry);
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
    qtest_add_data_func("/sn0/tigon/interrupt-coalesce-by-count", NULL,
                        test_interrupt_coalesce_by_count);
    qtest_add_data_func("/sn0/tigon/interrupt-coalesce-by-time", NULL,
                        test_interrupt_coalesce_by_time);
    qtest_add_data_func("/sn0/tigon/interrupt-coalesce-count-no-wrap", NULL,
                        test_interrupt_coalesce_count_does_not_wrap);
    qtest_add_data_func("/sn0/tigon/transmit-coalesce-counts-batched-bds",
                        NULL,
                        test_transmit_coalesce_counts_batched_descriptors);
    qtest_add_data_func("/sn0/tigon/transmit-coalesce-ignores-empty-doorbell",
                        NULL, test_transmit_coalesce_ignores_empty_doorbell);
    qtest_add_data_func("/sn0/tigon/transmit-coalesce-complete-frames", NULL,
                        test_transmit_coalesce_counts_complete_frames);
    qtest_add_data_func("/sn0/tigon/transmit-coalesce-stops-before-dma-error",
                        NULL, test_transmit_coalesce_stops_before_dma_error);
    qtest_add_data_func("/sn0/tigon/transmit-coalesce-reset", NULL,
                        test_transmit_coalesce_reset);
    qtest_add_data_func("/sn0/tigon/interrupt-coalesce-directions-independent",
                        NULL,
                        test_interrupt_coalesce_directions_are_independent);
    qtest_add_data_func("/sn0/tigon/misc-conf-reads-back", NULL,
                        test_misc_conf_reads_back);
    qtest_add_data_func("/sn0/tigon/receive-reports-checksums", NULL,
                        test_receive_reports_checksums);
    qtest_add_data_func("/sn0/tigon/payload-skips-lane-swap", NULL,
                        test_payload_skips_lane_swap);
    qtest_add_data_func("/sn0/tigon/set-mac-command-updates-filter", NULL,
                        test_set_mac_command_updates_filter);
    qtest_add_data_func("/sn0/tigon/reset-restores-configured-mac", NULL,
                        test_reset_restores_configured_mac);
    qtest_add_data_func("/sn0/tigon/programmed-mac-survives-migration", NULL,
                        test_programmed_mac_survives_migration);
    qtest_add_data_func("/sn0/tigon/transmit-coalesce-migration-from-v3",
                        NULL, test_transmit_coalesce_migration_from_v3);
    qtest_add_data_func("/sn0/tigon/transmit-coalesce-count-migration-current",
                        NULL,
                        test_transmit_coalesce_count_migration_current);
    qtest_add_data_func(
        "/sn0/tigon/transmit-coalesce-deadline-migration-current", NULL,
        test_transmit_coalesce_deadline_migration_current);
    qtest_add_data_func("/sn0/tigon/receive-payload-dma-error", NULL,
                        test_receive_payload_dma_error);
    qtest_add_data_func("/sn0/tigon/receive-jumbo-frame", NULL,
                        test_receive_jumbo_frame);
    qtest_add_data_func("/sn0/tigon/receive-jumbo-posted-by-command", NULL,
                        test_receive_jumbo_posted_by_command);
    qtest_add_data_func("/sn0/tigon/standard-shortage-guard-holds", NULL,
                        test_standard_shortage_guard_holds);
    qtest_add_data_func("/sn0/tigon/receive-standard-ring-unprovisioned", NULL,
                        test_receive_standard_ring_unprovisioned);
    qtest_add_data_func(
        "/sn0/tigon/standard-shortage-discards-without-waiting", NULL,
        test_standard_shortage_discards_without_waiting);
    qtest_add_data_func("/sn0/tigon/receive-jumbo-ring-unprovisioned", NULL,
                        test_receive_jumbo_ring_unprovisioned);
    qtest_add_data_func(
        "/sn0/tigon/jumbo-shortage-discards-without-waiting", NULL,
        test_jumbo_shortage_discards_without_waiting);
    qtest_add_data_func("/sn0/tigon/jumbo-command-releases-a-refused-frame",
                        NULL, test_jumbo_command_releases_a_refused_frame);
    qtest_add_data_func("/sn0/tigon/firmware-up-releases-a-held-frame", NULL,
                        test_firmware_up_releases_a_held_frame);
    qtest_add_data_func("/sn0/tigon/receive-accepts-multicast", NULL,
                        test_receive_accepts_multicast);
    qtest_add_data_func("/sn0/tigon/receive-direct-map-transparent", NULL,
                        test_receive_direct_map_transparent);
    qtest_add_data_func("/sn0/tigon/receive-direct-map-corrects", NULL,
                        test_receive_direct_map_corrects);
    qtest_add_data_func("/sn0/tigon/byte-order-bridge-transparent", NULL,
                        test_byte_order_bridge_transparent);
    qtest_add_data_func("/sn0/tigon/byte-order-bridge-corrects", NULL,
                        test_byte_order_bridge_corrects);
    qtest_add_data_func("/sn0/tigon/hang-guard", NULL, test_hang_guard);
    qtest_add_data_func("/sn0/tigon/interrupt-mask-holds-the-line", NULL,
                        test_interrupt_mask_holds_the_line);
    qtest_add_data_func("/sn0/tigon/interrupt-irix-ack-protocol", NULL,
                        test_interrupt_irix_ack_protocol);

    qtest_add_data_func("/sn0/tigon/statistics-head-tracks-interface-state",
                        NULL, test_statistics_head_tracks_interface_state);
    qtest_add_data_func("/sn0/tigon/statistics-head-tracks-discards", NULL,
                        test_statistics_head_tracks_discards);
    qtest_add_data_func("/sn0/tigon/statistics-clear-reaches-head", NULL,
                        test_statistics_clear_reaches_head);
    qtest_add_data_func("/sn0/tigon/reset-clears-statistics", NULL,
                        test_reset_clears_statistics);
    qtest_add_data_func("/sn0/tigon/receive-promisc-accepts-other", NULL,
                        test_receive_promisc_accepts_other);
    qtest_add_data_func("/sn0/tigon/receive-promisc-disable-restores-filter",
                        NULL, test_receive_promisc_disable_restores_filter);

    qtest_add_data_func("/sn0/tigon/interrupt-reenable-preserves-completion",
                        NULL, test_interrupt_reenable_preserves_completion);

    qtest_add_data_func("/sn0/tigon/statistics-traffic-counters", NULL,
                        test_statistics_traffic_counters);
    qtest_add_data_func("/sn0/tigon/statistics-interface-identity", NULL,
                        test_statistics_interface_identity);
    qtest_add_data_func("/sn0/tigon/statistics-clear", NULL,
                        test_statistics_clear);
    qtest_add_data_func("/sn0/tigon/statistics-byte-order", NULL,
                        test_statistics_byte_order);
    qtest_add_data_func("/sn0/tigon/statistics-head-stays-current", NULL,
                        test_statistics_head_stays_current);

    return g_test_run();
}
