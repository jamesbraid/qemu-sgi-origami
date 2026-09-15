/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The IOC3 ethernet engine on a plain PCI machine: a behaviour reproduced
 * here belongs to the device, not the SGI fabric. The backend's reply is
 * the one assertion independent of our own understanding.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"
#include <sys/socket.h>
#include <netinet/in.h>

/* The chip's window in the PCI hole; scratch in RAM nothing uses. */
#define IOC3_BAR_ADDR   0xc0000000u
#define RAM_SCRATCH     0x02000000u
#define PC_RAM_END      0x08000000u

#define IOC3_PCI_SLOT   4

/* The ethernet block's registers, at their offsets in the chip's window. */
#define ENET_MAC_CONTROL  (IOC3_BAR_ADDR + 0x0f0)
#define ENET_INT_STATUS   (IOC3_BAR_ADDR + 0x0f4)
#define ENET_INT_ENABLE   (IOC3_BAR_ADDR + 0x0f8)
#define ENET_RX_CONTROL   (IOC3_BAR_ADDR + 0x0fc)
#define ENET_RX_RING_HI   (IOC3_BAR_ADDR + 0x100)
#define ENET_RX_RING_LO   (IOC3_BAR_ADDR + 0x104)
#define ENET_RX_CONSUME   (IOC3_BAR_ADDR + 0x10c)
#define ENET_RX_PRODUCE   (IOC3_BAR_ADDR + 0x110)
#define ENET_RX_TIMER     (IOC3_BAR_ADDR + 0x114)
#define ENET_TX_RING_HI   (IOC3_BAR_ADDR + 0x128)
#define ENET_TX_RING_LO   (IOC3_BAR_ADDR + 0x12c)
#define ENET_TX_CONSUME   (IOC3_BAR_ADDR + 0x130)
#define ENET_TX_PRODUCE   (IOC3_BAR_ADDR + 0x134)
#define ENET_MII_CONTROL  (IOC3_BAR_ADDR + 0x148)
#define ENET_MII_DATA_R   (IOC3_BAR_ADDR + 0x14c)
#define ENET_MII_DATA_W   (IOC3_BAR_ADDR + 0x150)

#define MCR_PROMISC       (1u << 1)
#define MCR_RXOFF_SHIFT   3
#define MCR_RXOFF(halfwords) ((uint32_t)(halfwords) << MCR_RXOFF_SHIFT)
#define MCR_TX_DMA_EN     (1u << 13)
#define MCR_TX_EN         (1u << 14)
#define MCR_RX_DMA_EN     (1u << 15)
#define MCR_RX_EN         (1u << 16)
#define MCR_LOOPBACK      (1u << 17)
#define MCR_RESET         (1u << 31)

#define ENET_INT_RX_TIMER (1u << 0)
#define ENET_INT_RX_THRESHOLD (1u << 1)
#define ENET_INT_TX_EMPTY     (1u << 16)
#define ENET_INT_TX_COMPLETE  (1u << 22)
#define RPIR_ARM          (1u << 31)

/*
 * The modelled minimum between a frame arriving and its completion, in
 * nanoseconds: the sgi-ioc3-eth device default. Delivery is published only
 * once this much virtual time has passed, so every wait for a completion has
 * to advance the clock as well as give the main loop a pass.
 */
#define RX_ARRIVAL_LATENCY_NS  200000

/* Management requests; the transceiver is at 31 on every real capture. */
#define MII_CTL_READ      (1u << 10)
#define MII_PHY_SHIFT     5
#define PHY_ADDRESS       31

/* Transceiver registers, numbered by the 802.3 management set. */
#define PHY_BMCR          0
#define PHY_BMSR          1
#define BMCR_LOOPBACK     0x4000
#define BMCR_SPEED100     0x2000
#define BMCR_DUPLEX       0x0100
#define BMSR_AN_COMP      0x0020
#define BMSR_LINK         0x0004

/*
 * Register 0x17: plain storage on the default DP83840A part (PCS_CONFIG),
 * the DP83843's Loopback Register (LBR) when the "phy-dp83843" property is
 * set. LBR_TW_LBEN is the DP83843's twister/PMD loopback select, the bit
 * the IP35 BASEIO PROM's Twister diagnostic writes (OpenBSD's
 * sys/dev/mii/nsphyterreg.h; docs/datasheets/dp83840.md).
 */
#define MII_LBR           0x17
#define LBR_TW_LBEN       0x0020

#define TXD_INT_DONE      (1u << 12)
#define TXD_DATA_VALID    (1u << 16)
#define TXD_BUF1_VALID    (1u << 17)
#define TXD_BUF2_VALID    (1u << 18)
#define TXD_BUF1_LEN_SHIFT 8
#define TXD_BUF2_LEN_SHIFT 20

/* The inline area's offset, from both drivers' structs, not our header. */
#define TXD_DATA_OFFSET   24

/*
 * The area holds 104 bytes (both drivers) and its length field seven bits,
 * so a guest can legally name 127 of 104: the disagreement is the boundary.
 */
#define TXD_DATA_SIZE     104
#define TXD_DATA_LEN_MAX  127

/* The command word's total count: eleven bits in both drivers. */
#define TXD_BYTECNT_MAX   2047

/*
 * The 16 KB boundary a transfer may not cross (OpenBSD's IEC_DMA_BOUNDARY)
 * -- the one limit here from hardware rather than a field width.
 */
#define TX_DMA_BOUNDARY   0x4000u

/* Frame start: past the header words and OpenBSD's alignment padding. */
#define RXD_BUF_OFFSET    66
#define RXH_VALID         0x80000000u
#define RXH_BYTECNT_SHIFT 16

/* The received count's field: eleven bits in both drivers. */
#define RXH_BYTECNT_MASK  0x07ff0000u
#define RXH_BYTECNT_MAX   2047

/*
 * The count's second arrival, split across the error word; neither driver
 * reads it, so it is the field most likely to rot unnoticed.
 */
#define RXE_LEN_LO_SHIFT  12
#define RXE_LEN_HI_SHIFT  16

/* The FCS QEMU's net layer never delivers; the engine adds it back. */
#define ETHER_FCS_LEN     4

/*
 * The guests' length gates, counted with the CRC included, so the
 * interesting delivered sizes are four less.
 */
#define ETHER_MIN_LEN     64
#define ETHER_MAX_LEN     1518

/*
 * The truncation point is the model's own (2048 assembled minus eight
 * reserved), three below what the count field could describe.
 */
#define RX_DELIVER_MAX    2040

/* Linux gates every delivered frame on ERXBUF_GOODPKT. */
#define RXE_GOODPKT       0x40000000u
#define RXE_BROADCAST     0x08000000u

/*
 * OpenBSD drops any frame wearing either bit; the engine sets both on a
 * truncated delivery, so an over-long frame is a printed drop, not a
 * silently shortened one.
 */
#define RXE_LONGEVENT     0x10000000u
#define RXE_BADPKT        0x20000000u

/* The window's byte order lives here and nowhere else. */
static void ioc3_writel(QTestState *qts, uint64_t addr, uint32_t val)
{
    qtest_writel(qts, addr, val);
}

static uint32_t ioc3_readl(QTestState *qts, uint64_t addr)
{
    return qtest_readl(qts, addr);
}

/* The device reads rings big-endian everywhere: write them as bytes. */
static void dma_stl_be(QTestState *qts, uint64_t addr, uint32_t val)
{
    uint8_t b[4] = { val >> 24, val >> 16, val >> 8, val };

    qtest_memwrite(qts, addr, b, sizeof(b));
}

static uint32_t dma_ldl_be(QTestState *qts, uint64_t addr)
{
    uint8_t b[4];

    qtest_memread(qts, addr, b, sizeof(b));
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

/* Configuration space through the ports; no firmware ran. */
static uint32_t pci_cfg_addr(uint8_t slot, uint8_t offset)
{
    return 0x80000000u | ((uint32_t)slot << 11) | (offset & 0xfcu);
}

static void pci_cfg_writel(QTestState *qts, uint8_t slot, uint8_t offset,
                           uint32_t val)
{
    qtest_outl(qts, 0xcf8, pci_cfg_addr(slot, offset));
    qtest_outl(qts, 0xcfc, val);
}

/*
 * Driver-shaped bring-up shared by both attachments. Promiscuous, unlike a
 * real driver: the DMA-mechanics frames would otherwise be filtered for a
 * reason unrelated to what they test.
 */
static void ioc3_bring_up(QTestState *qts, uint32_t ring, uint32_t rxbuf,
                          unsigned int buffers)
{
    unsigned int i;

    qtest_outl(qts, 0xcf8, pci_cfg_addr(IOC3_PCI_SLOT, PCI_VENDOR_ID));
    g_assert_cmphex(qtest_inl(qts, 0xcfc), ==, 0x000310a9u);

    pci_cfg_writel(qts, IOC3_PCI_SLOT, PCI_BASE_ADDRESS_0, IOC3_BAR_ADDR);
    pci_cfg_writel(qts, IOC3_PCI_SLOT, PCI_COMMAND,
                   PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    /* The one reset value that is not the same forwards and backwards. */
    g_assert_cmphex(ioc3_readl(qts, ENET_MII_DATA_R), ==, 0xffff);

    ioc3_writel(qts, ENET_MAC_CONTROL, MCR_PROMISC | MCR_TX_DMA_EN | MCR_TX_EN |
                MCR_RX_DMA_EN | MCR_RX_EN | MCR_RXOFF(RXD_BUF_OFFSET / 2));
    ioc3_writel(qts, ENET_RX_RING_HI, 0);
    ioc3_writel(qts, ENET_RX_RING_LO, ring & 0xfffff000u);
    ioc3_writel(qts, ENET_RX_TIMER, 0);
    ioc3_writel(qts, ENET_RX_CONTROL, 0);
    for (i = 0; i < buffers; i++) {
        dma_stl_be(qts, ring + i * 8, 0);
        dma_stl_be(qts, ring + i * 8 + 4, rxbuf + i * 0x1000);
    }
    ioc3_writel(qts, ENET_RX_PRODUCE, (buffers << 3) | RPIR_ARM);
}

static QTestState *ioc3_start(uint32_t ring, uint32_t rxbuf,
                              unsigned int buffers)
{
    QTestState *qts;

    /* -nic would be claimed by the machine's own default device. */
    qts = qtest_initf("-M pc -m 128M -display none "
                      "-netdev user,id=n0 "
                      "-device sgi-ioc3,addr=%02x.0,netdev=n0", IOC3_PCI_SLOT);
    ioc3_bring_up(qts, ring, rxbuf, buffers);
    return qts;
}

/*
 * The socket-pair attachment chooses every byte in both directions, which
 * size boundaries need and user-mode networking cannot give.
 */
static QTestState *ioc3_attach_socket(int guest_fd, unsigned int buffers)
{
    QTestState *qts;

    qts = qtest_initf("-M pc -display none "
                      "-netdev socket,fd=%d,id=n0 "
                      "-device sgi-ioc3,addr=%02x.0,netdev=n0",
                      guest_fd, IOC3_PCI_SLOT);
    ioc3_bring_up(qts, RAM_SCRATCH + 0x8000, RAM_SCRATCH + 0xc000, buffers);
    return qts;
}

/*
 * The same attachment, with the transceiver switched to the Origin 300
 * IO-8 board's DP83843 instead of the default DP83840A.
 */
static QTestState *ioc3_attach_socket_dp83843(int guest_fd,
                                              unsigned int buffers)
{
    QTestState *qts;

    qts = qtest_initf("-M pc -display none "
                      "-netdev socket,fd=%d,id=n0 "
                      "-device sgi-ioc3,addr=%02x.0,netdev=n0,phy-dp83843=on",
                      guest_fd, IOC3_PCI_SLOT);
    ioc3_bring_up(qts, RAM_SCRATCH + 0x8000, RAM_SCRATCH + 0xc000, buffers);
    return qts;
}

/* Bounds the wait for a late frame; delivery may follow the write. */
static void net_socket_set_recv_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

/* The socket backend's framing: a network-order length per frame. */
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

/*
 * An ARP request for the backend's gateway, padded to order: length decides
 * which transmit path a real driver would use.
 */
static void build_arp_request(uint8_t *frame, size_t len)
{
    static const uint8_t head[42] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x08, 0x00, 0x69, 0x12, 0x34, 0x56,
        0x08, 0x06,
        0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
        0x08, 0x00, 0x69, 0x12, 0x34, 0x56, 10, 0, 2, 15,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 10, 0, 2, 2
    };

    g_assert_cmpint(len, >=, sizeof(head));
    memset(frame, 0, len);
    memcpy(frame, head, sizeof(head));
}

/* Backend parses, replies; engine delivers; timer source raises. */
static void assert_reply_landed(QTestState *qts, uint32_t rxbuf)
{
    uint8_t landed[42];
    int attempt;

    /*
     * The reply is delivered from a bottom half after the transmit walk and
     * published at its arrival deadline, so give the main loop passes and
     * advance the virtual clock rather than reading once.
     */
    for (attempt = 0; attempt < 200; attempt++) {
        if (ioc3_readl(qts, ENET_RX_CONSUME) == 8) {
            break;
        }
        g_usleep(1000);
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 8);
    g_assert_cmphex(dma_ldl_be(qts, rxbuf) & RXH_VALID, ==, RXH_VALID);

    /* Linux gates delivery on this bit; OpenBSD never reads it. */
    g_assert_cmphex(dma_ldl_be(qts, rxbuf + 4) & RXE_GOODPKT, ==, RXE_GOODPKT);

    qtest_memread(qts, rxbuf + RXD_BUF_OFFSET, landed, sizeof(landed));

    /* Addressed to us, from the gateway user-mode networking answers as. */
    g_assert_cmphex(landed[0], ==, 0x08);
    g_assert_cmphex(landed[1], ==, 0x00);
    g_assert_cmphex(landed[2], ==, 0x69);

    /* An ARP reply: the ethertype, then the opcode. */
    g_assert_cmphex(landed[12], ==, 0x08);
    g_assert_cmphex(landed[13], ==, 0x06);
    g_assert_cmphex(landed[21], ==, 0x02);

    /* Answering for the address the request asked about. */
    g_assert_cmphex(landed[28], ==, 10);
    g_assert_cmphex(landed[29], ==, 0);
    g_assert_cmphex(landed[30], ==, 2);
    g_assert_cmphex(landed[31], ==, 2);

    g_assert_cmphex(ioc3_readl(qts, ENET_INT_STATUS) & ENET_INT_RX_TIMER,
                    ==, ENET_INT_RX_TIMER);
}

/* The inline path, for anything at or under 104 bytes. */
static void test_the_inline_path_off_the_fabric(const void *data)
{
    QTestState *qts;
    uint8_t frame[60];
    uint32_t desc = RAM_SCRATCH;
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;

    build_arp_request(frame, sizeof(frame));
    qts = ioc3_start(ring, rxbuf, 4);

    dma_stl_be(qts, desc + 0, sizeof(frame) | TXD_DATA_VALID | TXD_INT_DONE);
    dma_stl_be(qts, desc + 4, sizeof(frame));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, sizeof(frame));

    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, desc & 0xffffc000u);
    ioc3_writel(qts, ENET_TX_PRODUCE, 128);

    assert_reply_landed(qts, rxbuf);
    qtest_quit(qts);
}

/* The pointer path, split across both buffers. */
static void test_the_pointer_path_off_the_fabric(const void *data)
{
    QTestState *qts;
    uint8_t frame[120];
    uint32_t desc = RAM_SCRATCH;
    uint32_t buf1 = RAM_SCRATCH + 0x1000;
    uint32_t buf2 = RAM_SCRATCH + 0x2000;
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;
    const uint32_t split = 64;

    build_arp_request(frame, sizeof(frame));
    qts = ioc3_start(ring, rxbuf, 4);

    qtest_memwrite(qts, buf1, frame, split);
    qtest_memwrite(qts, buf2, frame + split, sizeof(frame) - split);

    dma_stl_be(qts, desc + 0,
               sizeof(frame) | TXD_BUF1_VALID | TXD_BUF2_VALID |
               TXD_INT_DONE);
    dma_stl_be(qts, desc + 4,
               (split << TXD_BUF1_LEN_SHIFT) |
               ((sizeof(frame) - split) << TXD_BUF2_LEN_SHIFT));
    dma_stl_be(qts, desc + 8, 0);
    dma_stl_be(qts, desc + 12, buf1);
    dma_stl_be(qts, desc + 16, 0);
    dma_stl_be(qts, desc + 20, buf2);

    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, desc & 0xffffc000u);
    ioc3_writel(qts, ENET_TX_PRODUCE, 128);

    assert_reply_landed(qts, rxbuf);
    qtest_quit(qts);
}

/*
 * Send one frame and return what the backend received: boundaries are
 * asserted on the bytes that left the machine, not on a register.
 */
static void send_descriptor(QTestState *qts, const uint8_t *frame,
                            uint32_t claimed_len, uint32_t actual_len,
                            uint32_t split)
{
    uint32_t desc = RAM_SCRATCH;
    uint32_t buf1 = RAM_SCRATCH + TX_DMA_BOUNDARY - split;
    uint32_t buf2 = RAM_SCRATCH + TX_DMA_BOUNDARY;

    if (split == 0) {
        qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, actual_len);
        dma_stl_be(qts, desc + 0, claimed_len | TXD_DATA_VALID);
        dma_stl_be(qts, desc + 4, claimed_len);
    } else {
        /* The split OpenBSD produces when an mbuf would cross the boundary. */
        qtest_memwrite(qts, buf1, frame, split);
        qtest_memwrite(qts, buf2, frame + split, actual_len - split);
        dma_stl_be(qts, desc + 0,
                   claimed_len | TXD_BUF1_VALID | TXD_BUF2_VALID);
        dma_stl_be(qts, desc + 4,
                   (split << TXD_BUF1_LEN_SHIFT) |
                   ((actual_len - split) << TXD_BUF2_LEN_SHIFT));
        dma_stl_be(qts, desc + 8, 0);
        dma_stl_be(qts, desc + 12, buf1);
        dma_stl_be(qts, desc + 16, 0);
        dma_stl_be(qts, desc + 20, buf2);
    }

    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, desc & 0xffffc000u);
    ioc3_writel(qts, ENET_TX_PRODUCE, 128);
}

static size_t send_and_capture(QTestState *qts, int host_fd,
                               const uint8_t *frame, uint32_t claimed_len,
                               uint32_t actual_len, uint32_t split,
                               uint8_t *got, size_t got_size)
{
    send_descriptor(qts, frame, claimed_len, actual_len, split);
    return net_socket_recv_frame(host_fd, got, got_size);
}

/* Either side of the area's size, then the lengths that exceed it. */
static void test_the_inline_area_boundary(const void *data)
{
    static const uint32_t claimed[] = { 103, 104, 105, TXD_DATA_LEN_MAX };
    int sv[2];
    QTestState *qts;
    uint8_t frame[TXD_DATA_LEN_MAX];
    uint8_t got[512];
    unsigned int i, n;
    size_t len;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 1);
    }

    for (n = 0; n < ARRAY_SIZE(claimed); n++) {
        uint32_t expect = MIN(claimed[n], TXD_DATA_SIZE);

        g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
        net_socket_set_recv_timeout(sv[0], 10);
        qts = ioc3_attach_socket(sv[1], 8);

        len = send_and_capture(qts, sv[0], frame, claimed[n],
                               MIN(claimed[n], (uint32_t)sizeof(frame)), 0,
                               got, sizeof(got));

        /*
         * Over the area the frame stops at the area's end: trusting the
         * field sends bytes from past the descriptor copy.
         */
        g_assert_cmpint(len, ==, expect);
        g_assert_cmpmem(got, len, frame, expect);

        qtest_quit(qts);
        close(sv[0]);
    }
}

/*
 * The count says how many bytes leave; a total past the field wraps in the
 * descriptor as written -- 2048 carries zero, 3000 carries 952.
 */
static void test_the_assembled_frame_limit(const void *data)
{
    static const uint32_t sizes[] = { TXD_BYTECNT_MAX - 1, TXD_BYTECNT_MAX,
                                      TXD_BYTECNT_MAX + 1, 3000 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[3000];
    uint8_t got[4096];
    unsigned int i, n;
    size_t len;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 7 + 3);
    }

    for (n = 0; n < ARRAY_SIZE(sizes); n++) {
        uint32_t total = sizes[n];
        uint32_t part = total / 2;
        uint32_t wire = total & TXD_BYTECNT_MAX;

        g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
        net_socket_set_recv_timeout(sv[0], 10);
        qts = ioc3_attach_socket(sv[1], 8);

        if (wire == 0) {
            send_descriptor(qts, frame, total, total, part);
            net_socket_expect_no_frame(sv[0]);
        } else {
            len = send_and_capture(qts, sv[0], frame, total, total, part,
                                   got, sizeof(got));
            g_assert_cmpint(len, ==, wire);
            g_assert_cmpmem(got, len, frame, wire);
        }

        qtest_quit(qts);
        close(sv[0]);
    }
}

/* Split exactly on the 16 KB boundary, the shape OpenBSD produces. */
static void test_a_frame_split_on_the_dma_boundary(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[600];
    uint8_t got[1024];
    unsigned int i;
    size_t len;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i ^ 0x5a);
    }

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket(sv[1], 8);

    /* Buffer one ends exactly on the boundary. */
    len = send_and_capture(qts, sv[0], frame, sizeof(frame), sizeof(frame),
                           250, got, sizeof(got));

    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));

    qtest_quit(qts);
    close(sv[0]);
}

/* Deliver one frame; return the reported count. */
static uint32_t receive_and_inspect(QTestState *qts, int host_fd,
                                    const uint8_t *frame, size_t len,
                                    uint32_t rxbuf, uint32_t slot,
                                    uint8_t *landed, size_t landed_size)
{
    uint32_t base = rxbuf + slot * 0x1000;
    uint32_t word0, word1, count, split;
    int attempt;

    net_socket_send_frame(host_fd, frame, len);

    /* A socket backend delivers on a later main-loop pass: poll. */
    for (attempt = 0; attempt < 200; attempt++) {
        if (ioc3_readl(qts, ENET_RX_CONSUME) != slot * 8) {
            break;
        }
        g_usleep(5000);
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, (slot + 1) * 8);

    word0 = dma_ldl_be(qts, base);
    word1 = dma_ldl_be(qts, base + 4);

    g_assert_cmphex(word0 & RXH_VALID, ==, RXH_VALID);
    g_assert_cmphex(word1 & RXE_GOODPKT, ==, RXE_GOODPKT);

    count = (word0 & RXH_BYTECNT_MASK) >> RXH_BYTECNT_SHIFT;

    /* The split field's copy must agree; nothing else would notice drift. */
    split = (((word1 >> RXE_LEN_HI_SHIFT) & 0x3ff) << 3) |
            ((word1 >> RXE_LEN_LO_SHIFT) & 0x7);
    g_assert_cmpuint(split, ==, count);

    /* Never more than the field can hold, whatever the backend offered. */
    g_assert_cmpuint(count, <=, RXH_BYTECNT_MAX);

    if (landed_size) {
        qtest_memread(qts, base + RXD_BUF_OFFSET, landed, landed_size);
    }
    return count;
}

/*
 * The reported count at the guests' own gates: dropping the four-byte
 * addition puts every one on the wrong side of the comparison.
 */
static void test_the_reported_count_at_the_guest_gates(const void *data)
{
    static const uint32_t sizes[] = {
        59, 60, 61,
        ETHER_MAX_LEN - ETHER_FCS_LEN - 1,
        ETHER_MAX_LEN - ETHER_FCS_LEN,
        ETHER_MAX_LEN - ETHER_FCS_LEN + 1,
    };
    int sv[2];
    QTestState *qts;
    uint8_t frame[ETHER_MAX_LEN];
    uint8_t landed[ETHER_MAX_LEN];
    unsigned int i, n;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i * 3 + 1);
    }
    /* Broadcast, so the delivery is not conditional on any address filter. */
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket(sv[1], 8);

    for (n = 0; n < ARRAY_SIZE(sizes); n++) {
        uint32_t count = receive_and_inspect(qts, sv[0], frame, sizes[n],
                                             RAM_SCRATCH + 0xc000, n,
                                             landed, sizes[n]);

        g_assert_cmpuint(count, ==, sizes[n] + ETHER_FCS_LEN);
        g_assert_cmpmem(landed, sizes[n], frame, sizes[n]);
    }

    qtest_quit(qts);
    close(sv[0]);
}

/* Either side of the truncation point and one past. */
static void test_the_delivery_truncation_point(const void *data)
{
    static const uint32_t sizes[] = { RX_DELIVER_MAX - 1, RX_DELIVER_MAX,
                                      RX_DELIVER_MAX + 1 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[RX_DELIVER_MAX + 1];
    uint8_t landed[RX_DELIVER_MAX];
    unsigned int i, n;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i ^ 0x33);
    }
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket(sv[1], 8);

    for (n = 0; n < ARRAY_SIZE(sizes); n++) {
        uint32_t kept = MIN(sizes[n], (uint32_t)RX_DELIVER_MAX);
        uint32_t count = receive_and_inspect(qts, sv[0], frame, sizes[n],
                                             RAM_SCRATCH + 0xc000, n,
                                             landed, kept);
        uint32_t word1 = dma_ldl_be(qts, RAM_SCRATCH + 0xc000 + n * 0x1000
                                    + 4);

        /* The count describes what was kept, not what was offered. */
        g_assert_cmpuint(count, ==, kept + ETHER_FCS_LEN);
        g_assert_cmpmem(landed, kept, frame, kept);

        /* The long and bad bits, on the cut size and neither neighbour. */
        g_assert_cmphex(word1 & (RXE_LONGEVENT | RXE_BADPKT), ==,
                        kept == sizes[n] ? 0
                                         : RXE_LONGEVENT | RXE_BADPKT);
    }

    qtest_quit(qts);
    close(sv[0]);
}

/* 512 entries in both drivers; indices are byte offsets and wrap. */
#define RX_RING_ENTRIES   512
#define RX_RING_STRIDE    8
#define RX_INDEX_WRAP     (RX_RING_ENTRIES * RX_RING_STRIDE)

/* Two transmit ring sizes, chosen by the base register's low bit. */
#define TX_DESC_SIZE      128
#define TX_RING_SMALL     128
#define TX_RING_LARGE     512
#define TX_RING_SEL       1u
#define TX_CONSUME_IDLE   (1u << 31)
#define TX_CONSUME_MASK   0xffffu

/* A socket backend delivers on a later main-loop pass. */
static void wait_for_rx_consume(QTestState *qts, uint32_t want)
{
    int attempt;

    for (attempt = 0; attempt < 2000; attempt++) {
        if (ioc3_readl(qts, ENET_RX_CONSUME) == want) {
            return;
        }
        g_usleep(1000);
        qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS / 4);
    }
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, want);
}

/* Three laps: one proves the wrap, three prove it keeps happening. */
static void test_the_receive_ring_wraps_three_times(const void *data)
{
    enum { BUFFERS = 8, LAPS = 3 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[64];
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;
    unsigned int i;
    uint32_t sent;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)i;
    }
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket(sv[1], BUFFERS);

    /* Every slot in the ring, pointing back into the small pool. */
    for (i = 0; i < RX_RING_ENTRIES; i++) {
        dma_stl_be(qts, ring + i * RX_RING_STRIDE, 0);
        dma_stl_be(qts, ring + i * RX_RING_STRIDE + 4,
                   rxbuf + (i % BUFFERS) * 0x1000);
    }

    for (sent = 0; sent < RX_RING_ENTRIES * LAPS; sent++) {
        /* Refill well ahead, the way a driver does. */
        if (sent % 64 == 0) {
            uint32_t ahead = (sent + 256) % RX_RING_ENTRIES;

            ioc3_writel(qts, ENET_RX_PRODUCE,
                        (ahead * RX_RING_STRIDE) | RPIR_ARM);
        }
        net_socket_send_frame(sv[0], frame, sizeof(frame));
        wait_for_rx_consume(qts, ((sent + 1) * RX_RING_STRIDE) %
                                 RX_INDEX_WRAP);
    }

    /*
     * Weak alone -- no deliveries also ends at zero -- but the loop named
     * the exact index after each of the 1536 deliveries.
     */
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);

    /* And the last lap really wrote a frame. */
    g_assert_cmphex(dma_ldl_be(qts, rxbuf + ((RX_RING_ENTRIES - 1) % BUFFERS) *
                               0x1000) & RXH_VALID, ==, RXH_VALID);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A consumed ring must stop, and new buffers must drain what the backend
 * held without another frame to prompt it.
 */
static void test_the_receive_ring_runs_dry_and_recovers(const void *data)
{
    enum { BUFFERS = 4, EXTRA = 2 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[64];
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 0x40);
    }
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket(sv[1], BUFFERS);

    for (i = 0; i < BUFFERS + EXTRA; i++) {
        net_socket_send_frame(sv[0], frame, sizeof(frame));
    }

    /* It fills what it was given and then stops, however many are offered. */
    wait_for_rx_consume(qts, BUFFERS * RX_RING_STRIDE);
    g_usleep(200000);
    qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS);
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==,
                    BUFFERS * RX_RING_STRIDE);

    /* Nothing else happens here: only the new buffers can release these. */
    for (i = 0; i < EXTRA; i++) {
        uint32_t slot = BUFFERS + i;

        dma_stl_be(qts, RAM_SCRATCH + 0x8000 + slot * RX_RING_STRIDE, 0);
        dma_stl_be(qts, RAM_SCRATCH + 0x8000 + slot * RX_RING_STRIDE + 4,
                   rxbuf + slot * 0x1000);
    }
    ioc3_writel(qts, ENET_RX_PRODUCE,
                ((BUFFERS + EXTRA) * RX_RING_STRIDE) | RPIR_ARM);

    wait_for_rx_consume(qts, (BUFFERS + EXTRA) * RX_RING_STRIDE);

    /* And what landed is the frame, not a repeat of an earlier buffer. */
    g_assert_cmphex(dma_ldl_be(qts, rxbuf + BUFFERS * 0x1000) & RXH_VALID,
                    ==, RXH_VALID);

    qtest_quit(qts);
    close(sv[0]);
}

/* Descriptors carrying their own index, one doorbell for the run. */
static void queue_tx_run(QTestState *qts, uint32_t base, uint32_t first,
                         uint32_t count, uint32_t ring_bytes, size_t len)
{
    uint32_t n;

    for (n = 0; n < count; n++) {
        uint32_t slot = (first + n) % (ring_bytes / TX_DESC_SIZE);
        uint32_t desc = base + slot * TX_DESC_SIZE;
        uint8_t frame[64];

        memset(frame, 0, sizeof(frame));
        memset(frame, 0xff, 6);
        frame[12] = 0x08;
        frame[13] = 0x00;
        frame[14] = (uint8_t)((first + n) >> 8);
        frame[15] = (uint8_t)(first + n);

        qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, len);
        dma_stl_be(qts, desc + 0, len | TXD_DATA_VALID);
        dma_stl_be(qts, desc + 4, len);
    }
}

/*
 * Neither driver rings per descriptor, so an engine sending only the first
 * of a batch stalls every driver and passes any one-frame test.
 */
static void test_a_burst_behind_one_doorbell(const void *data)
{
    enum { BURST = 16, LEN = 64 };
    int sv[2];
    QTestState *qts;
    uint8_t got[128];
    uint32_t base = RAM_SCRATCH;
    unsigned int n;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket(sv[1], 8);

    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, base & 0xffffc000u);
    queue_tx_run(qts, base, 0, BURST, TX_RING_SMALL * TX_DESC_SIZE, LEN);

    /* One write, sixteen frames. */
    ioc3_writel(qts, ENET_TX_PRODUCE, BURST * TX_DESC_SIZE);

    for (n = 0; n < BURST; n++) {
        size_t len = net_socket_recv_frame(sv[0], got, sizeof(got));

        g_assert_cmpint(len, ==, LEN);
        g_assert_cmphex(got[14], ==, (uint8_t)(n >> 8));
        g_assert_cmphex(got[15], ==, (uint8_t)n);
    }

    g_assert_cmphex(ioc3_readl(qts, ENET_TX_CONSUME), ==,
                    (BURST * TX_DESC_SIZE) | TX_CONSUME_IDLE);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A fetch the fabric refuses stops the walk with the index on the
 * descriptor; repairing the address and ringing again sends it.
 */
static void test_a_transmit_descriptor_that_cannot_be_read(const void *data)
{
    enum { LEN = 64 };
    int sv[2];
    QTestState *qts;
    uint8_t got[128];
    uint32_t base = RAM_SCRATCH;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = ioc3_attach_socket(sv[1], 4);

    queue_tx_run(qts, base, 0, 1, TX_RING_SMALL * TX_DESC_SIZE, LEN);

    /* Sixty-four gigabytes up, where this machine has nothing at all. */
    ioc3_writel(qts, ENET_TX_RING_HI, 0x10);
    ioc3_writel(qts, ENET_TX_RING_LO, base & 0xffffc000u);
    ioc3_writel(qts, ENET_TX_PRODUCE, TX_DESC_SIZE);

    net_socket_expect_no_frame(sv[0]);
    g_assert_cmphex(ioc3_readl(qts, ENET_TX_CONSUME), ==, 0);

    /* The ring back where memory answers: the same descriptor goes out. */
    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_PRODUCE, TX_DESC_SIZE);

    net_socket_set_recv_timeout(sv[0], 10);
    g_assert_cmpint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==, LEN);
    g_assert_cmphex(ioc3_readl(qts, ENET_TX_CONSUME), ==,
                    TX_DESC_SIZE | TX_CONSUME_IDLE);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The same for the gather: a failed buffer read must not put stale bytes
 * on the wire and call the transmit a success.
 */
static void test_a_transmit_buffer_that_cannot_be_read(const void *data)
{
    enum { LEN = 128 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[LEN];
    uint8_t got[256];
    uint32_t base = RAM_SCRATCH;
    uint32_t buf = RAM_SCRATCH + 0x4000;
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 0x40);
    }
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = ioc3_attach_socket(sv[1], 4);

    qtest_memwrite(qts, buf, frame, sizeof(frame));
    dma_stl_be(qts, base + 0, LEN | TXD_BUF1_VALID | TXD_INT_DONE);
    dma_stl_be(qts, base + 4, LEN << TXD_BUF1_LEN_SHIFT);
    /* The buffer's high word, which is where the frame is not. */
    dma_stl_be(qts, base + 8, 0x10);
    dma_stl_be(qts, base + 12, buf);
    dma_stl_be(qts, base + 16, 0);
    dma_stl_be(qts, base + 20, 0);

    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, base & 0xffffc000u);
    ioc3_writel(qts, ENET_TX_PRODUCE, TX_DESC_SIZE);

    net_socket_expect_no_frame(sv[0]);
    g_assert_cmphex(ioc3_readl(qts, ENET_TX_CONSUME), ==, 0);

    /* The buffer back where the frame is: the same descriptor goes out. */
    dma_stl_be(qts, base + 8, 0);
    ioc3_writel(qts, ENET_TX_PRODUCE, TX_DESC_SIZE);

    net_socket_set_recv_timeout(sv[0], 10);
    g_assert_cmpint(net_socket_recv_frame(sv[0], got, sizeof(got)), ==, LEN);
    g_assert_cmpmem(got, LEN, frame, LEN);

    qtest_quit(qts);
    close(sv[0]);
}

/* Three laps of the small transmit ring. */
static void test_the_transmit_ring_wraps_three_times(const void *data)
{
    enum { LAPS = 3, CHUNK = 16, LEN = 64 };
    int sv[2];
    QTestState *qts;
    uint8_t got[128];
    uint32_t base = RAM_SCRATCH;
    uint32_t ring_bytes = TX_RING_SMALL * TX_DESC_SIZE;
    uint32_t sent = 0;
    unsigned int n;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket(sv[1], 8);

    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, base & 0xffffc000u);

    while (sent < TX_RING_SMALL * LAPS) {
        queue_tx_run(qts, base, sent, CHUNK, ring_bytes, LEN);
        ioc3_writel(qts, ENET_TX_PRODUCE,
                    ((sent + CHUNK) * TX_DESC_SIZE) % ring_bytes);

        for (n = 0; n < CHUNK; n++) {
            size_t len = net_socket_recv_frame(sv[0], got, sizeof(got));
            uint32_t which = sent + n;

            g_assert_cmpint(len, ==, LEN);
            g_assert_cmphex(got[14], ==, (uint8_t)(which >> 8));
            g_assert_cmphex(got[15], ==, (uint8_t)which);
        }
        sent += CHUNK;
    }

    /* The checked frames are what make ending at zero mean something. */
    g_assert_cmphex(ioc3_readl(qts, ENET_TX_CONSUME) & TX_CONSUME_MASK,
                    ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Asserted where the two ring sizes differ: descriptor 128. Nothing else
 * here leaves the first 128 either way.
 */
static void test_the_ring_select_bit_moves_the_wrap(const void *data)
{
    enum { COUNT = TX_RING_SMALL + 1, CHUNK = 16, LEN = 64 };
    int sv[2];
    QTestState *qts;
    uint8_t got[128];
    uint32_t base = RAM_SCRATCH;
    uint32_t ring_bytes = TX_RING_LARGE * TX_DESC_SIZE;
    uint32_t sent = 0;
    unsigned int n;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket(sv[1], 8);

    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, (base & 0xffffc000u) | TX_RING_SEL);

    while (sent < COUNT) {
        uint32_t run = MIN((uint32_t)CHUNK, (uint32_t)COUNT - sent);

        queue_tx_run(qts, base, sent, run, ring_bytes, LEN);
        ioc3_writel(qts, ENET_TX_PRODUCE, (sent + run) * TX_DESC_SIZE);

        for (n = 0; n < run; n++) {
            size_t len = net_socket_recv_frame(sv[0], got, sizeof(got));

            g_assert_cmpint(len, ==, LEN);
            g_assert_cmphex(got[15], ==, (uint8_t)(sent + n));
        }
        sent += run;
    }

    /* Ignoring the select bit wraps at 128 and sits on one. */
    g_assert_cmphex(ioc3_readl(qts, ENET_TX_CONSUME) & TX_CONSUME_MASK,
                    ==, COUNT * TX_DESC_SIZE);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * An echo request sets the receive size freely; the ARP exchange completes
 * first, or the backend's own ARP question masquerades as the reply.
 */
static uint16_t ones_complement_sum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
    }
    if (len & 1) {
        sum += (uint32_t)data[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* User-mode networking's documented defaults. */
static const uint8_t guest_mac[6] = { 0x08, 0x00, 0x69, 0x12, 0x34, 0x56 };
static const uint8_t guest_ip[4] = { 10, 0, 2, 15 };
static const uint8_t gateway_ip[4] = { 10, 0, 2, 2 };

static void build_icmp_echo(uint8_t *f, size_t total, const uint8_t *dst_mac)
{
    size_t ip_total = total - 14;
    size_t icmp_len = ip_total - 20;
    size_t i;
    uint16_t sum;

    g_assert_cmpint(total, >=, 14 + 20 + 8);
    memset(f, 0, total);
    memcpy(f, dst_mac, 6);
    memcpy(f + 6, guest_mac, 6);
    f[12] = 0x08; f[13] = 0x00;

    f[14] = 0x45;
    f[16] = (uint8_t)(ip_total >> 8);
    f[17] = (uint8_t)ip_total;
    f[22] = 64;
    f[23] = 1;
    memcpy(f + 26, guest_ip, 4);
    memcpy(f + 30, gateway_ip, 4);
    sum = ones_complement_sum(f + 14, 20);
    f[24] = (uint8_t)(sum >> 8);
    f[25] = (uint8_t)sum;

    f[34] = 8;
    f[38] = 0x12; f[39] = 0x34;
    f[40] = 0x00; f[41] = 0x01;
    for (i = 42; i < total; i++) {
        f[i] = (uint8_t)(i & 0xff);
    }
    sum = ones_complement_sum(f + 34, icmp_len);
    f[36] = (uint8_t)(sum >> 8);
    f[37] = (uint8_t)sum;
}

/* Next descriptor each time: re-ringing slot 0 sends one nobody wrote. */
static void tx_one(QTestState *qts, uint32_t base, uint32_t slot,
                   const uint8_t *frame, size_t len)
{
    uint32_t desc = base + slot * TX_DESC_SIZE;
    uint32_t buf = RAM_SCRATCH + 0x4000;

    if (len <= TXD_DATA_SIZE) {
        qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, len);
        dma_stl_be(qts, desc + 0, len | TXD_DATA_VALID | TXD_INT_DONE);
        dma_stl_be(qts, desc + 4, len);
    } else {
        qtest_memwrite(qts, buf, frame, len);
        dma_stl_be(qts, desc + 0, len | TXD_BUF1_VALID | TXD_INT_DONE);
        dma_stl_be(qts, desc + 4, len << TXD_BUF1_LEN_SHIFT);
        dma_stl_be(qts, desc + 8, 0);
        dma_stl_be(qts, desc + 12, buf);
        dma_stl_be(qts, desc + 16, 0);
        dma_stl_be(qts, desc + 20, 0);
    }
    ioc3_writel(qts, ENET_TX_PRODUCE, (slot + 1) * TX_DESC_SIZE);
}

/* One size per case so a packet-size boundary reports its own verdict. */
static void test_a_reply_after_the_walk_at_one_size(const void *data)
{
    const uint32_t size = GPOINTER_TO_UINT(data);
    QTestState *qts;
    uint8_t frame[ETHER_MAX_LEN];
    uint8_t landed[64];
    uint8_t gw_mac[6];
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;
    uint32_t base = RAM_SCRATCH;
    qts = ioc3_start(ring, rxbuf, 3);
    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, base & 0xffffc000u);

    /* Take the ARP answer here so it is not mistaken for the echo. */
    build_arp_request(frame, 42);
    tx_one(qts, base, 0, frame, 42);
    wait_for_rx_consume(qts, RX_RING_STRIDE);
    qtest_memread(qts, rxbuf + RXD_BUF_OFFSET, landed, sizeof(landed));
    g_assert_cmphex(landed[12], ==, 0x08);
    g_assert_cmphex(landed[13], ==, 0x06);
    memcpy(gw_mac, landed + 6, 6);

    ioc3_writel(qts, ENET_INT_STATUS, 0xffffffffu);

    {
        uint32_t slot = 1;
        uint32_t base_addr = rxbuf + slot * 0x1000;
        uint32_t word0, word1, status;

        build_icmp_echo(frame, size, gw_mac);
        tx_one(qts, base, slot, frame, size);

        /*
         * The main loop runs the queued receive, and the completion follows
         * its arrival deadline, so wait for the index rather than read once.
         */
        wait_for_rx_consume(qts, (slot + 1) * RX_RING_STRIDE);

        word0 = dma_ldl_be(qts, base_addr);
        word1 = dma_ldl_be(qts, base_addr + 4);
        g_assert_cmphex(word0 & RXH_VALID, ==, RXH_VALID);
        g_assert_cmphex(word1 & RXE_GOODPKT, ==, RXE_GOODPKT);

        /* The reply mirrors the request, plus the CRC added back. */
        g_assert_cmpuint((word0 & RXH_BYTECNT_MASK) >> RXH_BYTECNT_SHIFT,
                         ==, size + ETHER_FCS_LEN);

        /* An echo reply rather than the backend's own ARP question. */
        qtest_memread(qts, base_addr + RXD_BUF_OFFSET, landed, sizeof(landed));
        g_assert_cmphex(landed[12], ==, 0x08);
        g_assert_cmphex(landed[13], ==, 0x00);
        g_assert_cmphex(landed[34], ==, 0x00);

        /* The receive source is combined with the completed transmit. */
        status = ioc3_readl(qts, ENET_INT_STATUS);
        g_assert_cmphex(status & ENET_INT_RX_TIMER, ==, ENET_INT_RX_TIMER);
        ioc3_writel(qts, ENET_INT_STATUS, 0xffffffffu);

    }

    qtest_quit(qts);
}

static const uint32_t walk_sizes[] = { 74, 104, 105, 170, 554, 1042,
                                       ETHER_MAX_LEN - ETHER_FCS_LEN };

/* Management transfers, in the order both drivers use. */
static uint16_t mii_read(QTestState *qts, unsigned int reg)
{
    ioc3_writel(qts, ENET_MII_CONTROL,
                (PHY_ADDRESS << MII_PHY_SHIFT) | reg | MII_CTL_READ);
    return (uint16_t)ioc3_readl(qts, ENET_MII_DATA_R);
}

static void mii_write(QTestState *qts, unsigned int reg, uint16_t val)
{
    ioc3_writel(qts, ENET_MII_DATA_W, val);
    ioc3_writel(qts, ENET_MII_CONTROL, (PHY_ADDRESS << MII_PHY_SHIFT) | reg);
}

/*
 * set_link is the cable being pulled: the link and completion bits must
 * drop and return, or a carrier-watching guest trusts a dead interface.
 */
static void test_the_backend_link_reaches_the_status_word(const void *data)
{
    QTestState *qts;
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;

    qts = ioc3_start(ring, rxbuf, 2);

    /* A control write that is not a reset closes negotiation at once. */
    mii_write(qts, PHY_BMCR, BMCR_SPEED100 | BMCR_DUPLEX);
    g_assert_cmphex(mii_read(qts, PHY_BMSR) & (BMSR_LINK | BMSR_AN_COMP),
                    ==, BMSR_LINK | BMSR_AN_COMP);

    qtest_qmp_assert_success(qts, "{'execute': 'set_link', 'arguments': "
                             "{'name': 'n0', 'up': false}}");
    g_assert_cmphex(mii_read(qts, PHY_BMSR) & (BMSR_LINK | BMSR_AN_COMP),
                    ==, 0);

    qtest_qmp_assert_success(qts, "{'execute': 'set_link', 'arguments': "
                             "{'name': 'n0', 'up': true}}");
    g_assert_cmphex(mii_read(qts, PHY_BMSR) & (BMSR_LINK | BMSR_AN_COMP),
                    ==, BMSR_LINK | BMSR_AN_COMP);

    qtest_quit(qts);
}

/*
 * Loopback queues refused backend traffic rather than dropping it, so
 * clearing it must deliver the queue unprompted -- the diagnostic walks
 * the loopback selects with traffic free to be waiting.
 */
static void test_a_queued_frame_lands_when_phy_loopback_clears(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[64];
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 0x80);
    }
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket(sv[1], 2);

    mii_write(qts, PHY_BMCR, BMCR_LOOPBACK | BMCR_SPEED100 | BMCR_DUPLEX);

    /* Refused while looping back: the index must sit still. */
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    g_usleep(200000);
    qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS);
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);

    /* Only the flush this write triggers can deliver the queued frame. */
    mii_write(qts, PHY_BMCR, BMCR_SPEED100 | BMCR_DUPLEX);
    wait_for_rx_consume(qts, RX_RING_STRIDE);
    g_assert_cmphex(dma_ldl_be(qts, rxbuf) & RXH_VALID, ==, RXH_VALID);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * An unreadable ring entry drops the frame: advancing would point the
 * driver at a buffer nothing wrote.
 */
static void test_a_frame_whose_ring_entry_cannot_be_read(const void *data)
{
    QTestState *qts;
    uint8_t frame[60];
    uint8_t landed[60];
    uint32_t base = RAM_SCRATCH;
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;

    build_arp_request(frame, sizeof(frame));
    qts = ioc3_start(ring, rxbuf, 4);
    ioc3_writel(qts, ENET_MAC_CONTROL,
                MCR_PROMISC | MCR_LOOPBACK | MCR_TX_DMA_EN | MCR_TX_EN |
                MCR_RX_DMA_EN | MCR_RX_EN | MCR_RXOFF(RXD_BUF_OFFSET / 2));
    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, base & 0xffffc000u);

    /* Sixty-four gigabytes up, where this machine has nothing at all. */
    ioc3_writel(qts, ENET_RX_RING_HI, 0x10);
    tx_one(qts, base, 0, frame, sizeof(frame));

    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);
    g_assert_cmphex(ioc3_readl(qts, ENET_INT_STATUS) &
                    (ENET_INT_RX_TIMER | ENET_INT_RX_THRESHOLD), ==, 0);
    g_assert_cmphex(dma_ldl_be(qts, rxbuf) & RXH_VALID, ==, 0);

    /* The next frame takes the slot the dropped one left alone. */
    ioc3_writel(qts, ENET_RX_RING_HI, 0);
    tx_one(qts, base, 1, frame, sizeof(frame));

    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, RX_RING_STRIDE);
    g_assert_cmphex(dma_ldl_be(qts, rxbuf) & RXH_VALID, ==, RXH_VALID);
    qtest_memread(qts, rxbuf + RXD_BUF_OFFSET, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));

    qtest_quit(qts);
}

static void assert_receive_not_published(QTestState *qts, uint32_t rxbuf)
{
    g_assert_cmphex(dma_ldl_be(qts, rxbuf) & RXH_VALID, ==, 0);
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);
    g_assert_cmphex(ioc3_readl(qts, ENET_INT_STATUS) &
                    (ENET_INT_RX_TIMER | ENET_INT_RX_THRESHOLD), ==, 0);
}

/*
 * Only the first header word fits in RAM.  Publishing it before the rest
 * would expose a valid buffer whose status and payload were never written.
 */
static void test_a_status_failure_does_not_publish_valid(const void *data)
{
    QTestState *qts;
    uint8_t frame[60];
    uint32_t base = RAM_SCRATCH;
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = PC_RAM_END - sizeof(uint32_t);

    build_arp_request(frame, sizeof(frame));
    qts = ioc3_start(ring, rxbuf, 1);
    dma_stl_be(qts, rxbuf, 0x11223344);
    ioc3_writel(qts, ENET_MAC_CONTROL,
                MCR_PROMISC | MCR_LOOPBACK | MCR_TX_DMA_EN | MCR_TX_EN |
                MCR_RX_DMA_EN | MCR_RX_EN | MCR_RXOFF(RXD_BUF_OFFSET / 2));
    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, base & 0xffffc000u);

    tx_one(qts, base, 0, frame, sizeof(frame));

    assert_receive_not_published(qts, rxbuf);

    qtest_quit(qts);
}

/* The status fits, but a MAC-looped payload crosses the end of RAM. */
static void test_a_payload_failure_does_not_publish_valid(const void *data)
{
    QTestState *qts;
    uint8_t frame[96];
    uint32_t base = RAM_SCRATCH;
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = PC_RAM_END - 128;
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = i + 1;
    }
    qts = ioc3_start(ring, rxbuf, 1);
    dma_stl_be(qts, rxbuf, 0x11223344);
    dma_stl_be(qts, rxbuf + 4, 0x55667788);
    ioc3_writel(qts, ENET_MAC_CONTROL,
                MCR_PROMISC | MCR_LOOPBACK | MCR_TX_DMA_EN | MCR_TX_EN |
                MCR_RX_DMA_EN | MCR_RX_EN | MCR_RXOFF(RXD_BUF_OFFSET / 2));
    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, base & 0xffffc000u);

    tx_one(qts, base, 0, frame, sizeof(frame));

    g_assert_cmphex(dma_ldl_be(qts, rxbuf + 4), ==, 0);
    assert_receive_not_published(qts, rxbuf);

    qtest_quit(qts);
}

/* The complete payload fits; only the following FCS write crosses RAM. */
static void test_an_fcs_failure_does_not_publish_valid(const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[60];
    uint8_t landed[sizeof(frame)];
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = PC_RAM_END - 128;
    uint32_t status = 0x55667788;
    unsigned int attempt, i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = i + 1;
    }
    memset(frame, 0xff, 6);
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M pc -m 128M -display none "
                      "-netdev socket,fd=%d,id=n0 "
                      "-device sgi-ioc3,addr=%02x.0,netdev=n0",
                      sv[1], IOC3_PCI_SLOT);
    ioc3_bring_up(qts, ring, rxbuf, 1);
    dma_stl_be(qts, rxbuf, 0x11223344);
    dma_stl_be(qts, rxbuf + 4, 0x55667788);

    net_socket_send_frame(sv[0], frame, sizeof(frame));
    for (attempt = 0; attempt < 200; attempt++) {
        status = dma_ldl_be(qts, rxbuf + 4);
        if (status != 0x55667788) {
            break;
        }
        g_usleep(5000);
    }

    g_assert_cmphex(status, ==, RXE_GOODPKT | RXE_BROADCAST | (8u << 16));
    qtest_memread(qts, rxbuf + RXD_BUF_OFFSET, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));
    assert_receive_not_published(qts, rxbuf);

    qtest_quit(qts);
    close(sv[0]);
}

static unsigned int count_trace_events(const char *trace, const char *event)
{
    unsigned int count = 0;

    while ((trace = strstr(trace, event))) {
        count++;
        trace += strlen(event);
    }
    return count;
}

/* An asserted level stays asserted while more completed buffers arrive. */
static void test_a_receive_burst_asserts_the_irq_once(const void *data)
{
    enum { BURST = 8 };
    g_autofree char *path = NULL;
    g_autofree char *trace = NULL;
    QDict *response;
    int sv[2];
    QTestState *qts;
    uint8_t frame[64];
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = i;
    }
    memset(frame, 0xff, 6);
    path = g_strdup_printf("sgi-ioc3-rx-irq-%u-%u.trace",
                           (unsigned int)getpid(), g_random_int());
    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    qts = qtest_initf("-M pc -m 128M -display none "
                      "-netdev socket,fd=%d,id=n0 "
                      "-device sgi-ioc3,addr=%02x.0,netdev=n0 "
                      "-trace file=%s",
                      sv[1], IOC3_PCI_SLOT, path);
    ioc3_bring_up(qts, ring, rxbuf, BURST);
    ioc3_writel(qts, ENET_INT_ENABLE, ENET_INT_RX_TIMER);
    response = qtest_qmp(qts, "{ 'execute': 'trace-event-set-state', "
                              "'arguments': { 'name': 'sgi_ioc3_eth_irq', "
                                             "'enable': true } }");
    g_assert_true(qdict_haskey(response, "return"));
    qobject_unref(response);

    for (i = 0; i < BURST; i++) {
        net_socket_send_frame(sv[0], frame, sizeof(frame));
    }
    wait_for_rx_consume(qts, BURST * RX_RING_STRIDE);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &trace, NULL, NULL));
    g_assert_cmpuint(count_trace_events(trace, "sgi_ioc3_eth_irq"), ==, 1);
    g_assert_cmpint(g_unlink(path), ==, 0);
    close(sv[0]);
}

/*
 * Stands alone so it inherits no wait; the MAC control write is spelled
 * out to pin both loopback selections clear.
 */
static void test_a_transmit_with_no_reply_raises_nothing(const void *data)
{
    g_autofree char *path = NULL;
    g_autofree char *trace = NULL;
    QTestState *qts;
    uint8_t quiet[64];
    uint32_t base = RAM_SCRATCH;
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;
    uint32_t status;

    path = g_strdup_printf("sgi-ioc3-rx-flush-%u-%u.trace",
                           (unsigned int)getpid(), g_random_int());
    qts = qtest_initf("-M pc -m 128M -display none "
                      "-netdev user,id=n0 "
                      "-device sgi-ioc3,addr=%02x.0,netdev=n0 "
                      "-trace sgi_ioc3_eth_rx_flush,file=%s",
                      IOC3_PCI_SLOT, path);
    ioc3_bring_up(qts, ring, rxbuf, 2);
    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, base & 0xffffc000u);

    memset(quiet, 0, sizeof(quiet));
    memset(quiet, 0xff, 6);
    quiet[12] = 0x88;
    quiet[13] = 0xb5;
    tx_one(qts, base, 0, quiet, sizeof(quiet));

    /* Nothing arrived, so the next line is a statement about the source. */
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);
    status = ioc3_readl(qts, ENET_INT_STATUS);
    g_assert_cmphex(status & (ENET_INT_RX_TIMER | ENET_INT_RX_THRESHOLD),
                    ==, 0);
    g_assert_cmphex(status & ENET_INT_TX_COMPLETE, ==, ENET_INT_TX_COMPLETE);

    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &trace, NULL, NULL));
    /*
     * Bring-up's own MAC_CONTROL and RX_PRODUCE writes each schedule one;
     * the quiet transmit that gets no reply must not add a third.
     */
    g_assert_cmpuint(count_trace_events(trace, "sgi_ioc3_eth_rx_flush"), ==, 2);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

/* The reset case needs a control that proves its backend reply was queued. */
static void test_deferred_receive_delivers_without_reset(const void *data)
{
    g_autofree char *path = NULL;
    g_autofree char *trace = NULL;
    QTestState *qts;
    uint8_t frame[60];
    uint32_t desc = RAM_SCRATCH;
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;

    path = g_strdup_printf("sgi-ioc3-rx-deferred-%u-%u.trace",
                           (unsigned int)getpid(), g_random_int());
    qts = qtest_initf("-M pc -m 128M -display none "
                      "-netdev user,id=n0 "
                      "-device sgi-ioc3,addr=%02x.0,netdev=n0 "
                      "-trace sgi_ioc3_eth_rx_flush,file=%s",
                      IOC3_PCI_SLOT, path);
    ioc3_bring_up(qts, ring, rxbuf, 2);
    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, desc & 0xffffc000u);

    build_arp_request(frame, sizeof(frame));
    dma_stl_be(qts, desc, sizeof(frame) | TXD_DATA_VALID | TXD_INT_DONE);
    dma_stl_be(qts, desc + 4, sizeof(frame));
    qtest_memwrite(qts, desc + TXD_DATA_OFFSET, frame, sizeof(frame));

    qtest_writel(qts, ENET_TX_PRODUCE, 128);
    wait_for_rx_consume(qts, RX_RING_STRIDE);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &trace, NULL, NULL));
    g_assert_nonnull(strstr(trace, "sgi_ioc3_eth_rx_flush"));
    g_assert_cmpint(g_unlink(path), ==, 0);
}

/*
 * A frame does not complete the instant the backend hands it over. The
 * engine writes it into the posted buffer straight away, and the three
 * things the guest can see -- the header word's valid bit, the consume
 * index, and the receive source -- appear only once the arrival deadline
 * has passed on the virtual clock.
 *
 * Without the floor the model published a reply in the same virtual instant
 * as the transmit that provoked it, which no wire, peer or DMA can do; see
 * docs/datasheets/ioc3.md's arrival-latency approximation.
 */
static void test_a_receive_waits_for_its_arrival_deadline(const void *data)
{
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;
    uint8_t frame[60];
    uint8_t landed[sizeof(frame)];
    QTestState *qts;
    int sv[2];
    int attempt;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket(sv[1], 4);
    ioc3_writel(qts, ENET_INT_STATUS, 0xffffffffu);
    g_assert_cmphex(ioc3_readl(qts, ENET_INT_STATUS), ==, 0);

    build_arp_request(frame, sizeof(frame));
    net_socket_send_frame(sv[0], frame, sizeof(frame));

    /*
     * Wait for the engine to take the frame, which it shows by writing it
     * into the buffer. Nothing here advances the virtual clock, so the
     * arrival deadline cannot have passed.
     */
    for (attempt = 0; attempt < 200; attempt++) {
        qtest_memread(qts, rxbuf + RXD_BUF_OFFSET, landed, sizeof(landed));
        if (memcmp(landed, frame, sizeof(frame)) == 0) {
            break;
        }
        g_usleep(5000);
    }
    g_assert_cmpint(memcmp(landed, frame, sizeof(frame)), ==, 0);

    /* The payload is there and none of the three visible halves are. */
    g_assert_cmphex(dma_ldl_be(qts, rxbuf) & RXH_VALID, ==, 0);
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);
    g_assert_cmphex(ioc3_readl(qts, ENET_INT_STATUS), ==, 0);

    /* Still nothing one nanosecond short of the deadline. */
    qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS - 1);
    g_assert_cmphex(dma_ldl_be(qts, rxbuf) & RXH_VALID, ==, 0);
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);
    g_assert_cmphex(ioc3_readl(qts, ENET_INT_STATUS), ==, 0);

    /* And all three at once past it. */
    qtest_clock_step(qts, 1);
    g_assert_cmphex(dma_ldl_be(qts, rxbuf) & RXH_VALID, ==, RXH_VALID);
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 8);
    g_assert_cmphex(ioc3_readl(qts, ENET_INT_STATUS) & ENET_INT_RX_TIMER,
                    ==, ENET_INT_RX_TIMER);

    qtest_quit(qts);
    close(sv[0]);
}

/* SGI_NET_RX_LATENCY_DEPTH, hw/net/net-rx-latency.h: the line's cap. */
#define RX_LATENCY_LINE_DEPTH 256

/*
 * MAC loopback delivers straight from the transmit walk, never through
 * receive(), so can_receive()'s check that the arrival line has room never
 * ran for it. Hold one backend frame to occupy the line, then loopback
 * exactly enough frames to fill it: the last one finds the line full without
 * ever having been asked to refuse it.
 */
static void test_loopback_respects_a_full_arrival_line(const void *data)
{
    enum { BUFFERS = 16 };
    int sv[2];
    QTestState *qts;
    uint8_t seed[64];
    uint8_t frame[64];
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;
    uint32_t base = RAM_SCRATCH;
    uint8_t landed[sizeof(seed)];
    unsigned int i;
    int attempt;

    for (i = 0; i < sizeof(seed); i++) {
        seed[i] = (uint8_t)(i + 0x30);
        frame[i] = (uint8_t)(i + 0x60);
    }
    memset(seed, 0xff, 6);
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket(sv[1], BUFFERS);

    /*
     * One backend frame, held behind its arrival deadline: the line now
     * holds one entry, and nothing here steps the virtual clock to let it
     * publish. The payload lands in the posted buffer before the header's
     * valid bit does, so polling for it proves the frame was staged without
     * needing the clock.
     */
    net_socket_send_frame(sv[0], seed, sizeof(seed));
    for (attempt = 0; attempt < 200; attempt++) {
        qtest_memread(qts, rxbuf + RXD_BUF_OFFSET, landed, sizeof(landed));
        if (memcmp(landed, seed, sizeof(seed)) == 0) {
            break;
        }
        g_usleep(5000);
    }
    g_assert_cmpint(memcmp(landed, seed, sizeof(seed)), ==, 0);
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);

    /* Now loop back: can_receive() never runs on this path. */
    ioc3_writel(qts, ENET_MAC_CONTROL,
               MCR_PROMISC | MCR_LOOPBACK | MCR_TX_DMA_EN | MCR_TX_EN |
               MCR_RX_DMA_EN | MCR_RX_EN | MCR_RXOFF(RXD_BUF_OFFSET / 2));
    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, (base & 0xffffc000u) | TX_RING_SEL);

    /*
     * 255 loopback deliveries fill the remaining room exactly; the 256th
     * finds the line already full. Unfixed, that is
     * sgi_net_rx_latency_enqueue()'s "the caller already checked" assertion.
     */
    for (i = 0; i < RX_LATENCY_LINE_DEPTH; i++) {
        tx_one(qts, base, i, frame, sizeof(frame));
    }

    /* Still alive: the transmit side completed all 256 regardless. */
    g_assert_cmphex(ioc3_readl(qts, ENET_TX_CONSUME) & TX_CONSUME_MASK, ==,
                    RX_LATENCY_LINE_DEPTH * TX_DESC_SIZE);

    /*
     * Past every held deadline, exactly the line's worth of completions
     * publish: the seed plus 255 accepted loopback frames, not 256 of them --
     * the 256th loopback delivery found no room and was dropped.
     */
    qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS);
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==,
                    RX_LATENCY_LINE_DEPTH * RX_RING_STRIDE);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The IP35 BASEIO PROM's Twister loopback sub-test (enet_tw_loop): BMCR
 * disables autonegotiation and selects 100 Mb full duplex with no BMCR
 * loopback bit, then LBR's twister/PMD select is OR-ed in. On the DP83843
 * this loops transmitted frames back to the receive ring the way PHY
 * loopback always has -- through the whole controller, four bytes higher
 * and a real status word -- without ever reaching the backend.
 */
static void test_dp83843_twister_loopback_turns_a_frame_round(const void *data)
{
    enum { LEN = 301 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[LEN];
    uint8_t landed[LEN];
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;
    uint32_t base = RAM_SCRATCH;
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 0x11);
    }
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 1);
    qts = ioc3_attach_socket_dp83843(sv[1], 2);

    mii_write(qts, PHY_BMCR, BMCR_SPEED100 | BMCR_DUPLEX);
    mii_write(qts, MII_LBR, mii_read(qts, MII_LBR) | LBR_TW_LBEN);

    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, base & 0xffffc000u);
    tx_one(qts, base, 0, frame, sizeof(frame));

    wait_for_rx_consume(qts, RX_RING_STRIDE);

    g_assert_cmphex(dma_ldl_be(qts, rxbuf) & RXH_VALID, ==, RXH_VALID);
    g_assert_cmpuint((dma_ldl_be(qts, rxbuf) & RXH_BYTECNT_MASK) >>
                     RXH_BYTECNT_SHIFT, ==, sizeof(frame) + ETHER_FCS_LEN);
    g_assert_cmphex(dma_ldl_be(qts, rxbuf + 4) & RXE_GOODPKT, ==,
                    RXE_GOODPKT);

    qtest_memread(qts, rxbuf + RXD_BUF_OFFSET, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));

    g_assert_cmphex(ioc3_readl(qts, ENET_INT_STATUS) & ENET_INT_TX_EMPTY, ==,
                    ENET_INT_TX_EMPTY);

    /* The whole point: nothing of this crossed to the backend. */
    net_socket_expect_no_frame(sv[0]);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The negative control this bug turns on: leaving TW_LBEN clear on the same
 * DP83843 part, same BMCR, same everything else, must not loop -- otherwise
 * the fix would really be "any register 0x17 write loops," not this bit.
 */
static void test_dp83843_loopback_requires_the_tw_lben_bit(const void *data)
{
    enum { LEN = 128 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[LEN];
    uint8_t got[256];
    uint32_t base = RAM_SCRATCH;
    unsigned int i;
    size_t len;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 0x22);
    }
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket_dp83843(sv[1], 2);

    mii_write(qts, PHY_BMCR, BMCR_SPEED100 | BMCR_DUPLEX);
    /* Bit 5 explicitly left clear; the rest of the sequence is unchanged. */
    mii_write(qts, MII_LBR, mii_read(qts, MII_LBR) & ~LBR_TW_LBEN);

    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, base & 0xffffc000u);
    tx_one(qts, base, 0, frame, sizeof(frame));

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The Origin 200 / OpenBSD nsphy non-regression: on the default DP83840A
 * part, register 0x17 bit 5 is PCR.CIM_DIS, which nsphy sets on every media
 * change and sn0-ioc3-test.c exercises as ordinary storage. The identical
 * write that loops a DP83843 must do nothing but store on this part.
 */
static void test_dp83840a_bit5_does_not_select_loopback(const void *data)
{
    enum { LEN = 128 };
    int sv[2];
    QTestState *qts;
    uint8_t frame[LEN];
    uint8_t got[256];
    uint32_t base = RAM_SCRATCH;
    unsigned int i;
    size_t len;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 0x33);
    }
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket(sv[1], 2);

    mii_write(qts, PHY_BMCR, BMCR_SPEED100 | BMCR_DUPLEX);
    mii_write(qts, MII_LBR, mii_read(qts, MII_LBR) | LBR_TW_LBEN);

    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, base & 0xffffc000u);
    tx_one(qts, base, 0, frame, sizeof(frame));

    len = net_socket_recv_frame(sv[0], got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A PHY reset clears LBR along with the rest of the transceiver's state, so
 * leaving loopback that way must re-open the backend exactly as clearing
 * BMCR's own loopback bit already does
 * (test_a_queued_frame_lands_when_phy_loopback_clears).
 */
static void test_dp83843_reset_clears_lbr_and_reopens_the_backend(
    const void *data)
{
    int sv[2];
    QTestState *qts;
    uint8_t frame[64];
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;
    unsigned int i;

    for (i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(i + 0x44);
    }
    memset(frame, 0xff, 6);

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 10);
    qts = ioc3_attach_socket_dp83843(sv[1], 2);

    mii_write(qts, PHY_BMCR, BMCR_SPEED100 | BMCR_DUPLEX);
    mii_write(qts, MII_LBR, mii_read(qts, MII_LBR) | LBR_TW_LBEN);

    /* Refused while looping back: the index must sit still. */
    net_socket_send_frame(sv[0], frame, sizeof(frame));
    g_usleep(200000);
    qtest_clock_step(qts, RX_ARRIVAL_LATENCY_NS);
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);

    /* Only the flush a reset's loopback exit triggers delivers it. */
    mii_write(qts, PHY_BMCR, 0x8000);
    wait_for_rx_consume(qts, RX_RING_STRIDE);
    g_assert_cmphex(dma_ldl_be(qts, rxbuf) & RXH_VALID, ==, RXH_VALID);

    qtest_quit(qts);
    close(sv[0]);
}

int main(int argc, char **argv)
{
    unsigned int i;

    g_test_init(&argc, &argv, NULL);

    /* The transmit path changes at 104; the receive size at each. */
    qtest_add_data_func("sgi-ioc3-eth/no-reply-raises-nothing", NULL,
                        test_a_transmit_with_no_reply_raises_nothing);
    qtest_add_data_func("sgi-ioc3-eth/deferred-receive-delivers-without-reset",
                        NULL,
                        test_deferred_receive_delivers_without_reset);
    qtest_add_data_func("sgi-ioc3-eth/backend-link-reaches-status", NULL,
                        test_the_backend_link_reaches_the_status_word);
    qtest_add_data_func("sgi-ioc3-eth/queued-frame-lands-after-loopback", NULL,
                        test_a_queued_frame_lands_when_phy_loopback_clears);
    qtest_add_data_func("sgi-ioc3-eth/unreadable-ring-entry-drops-the-frame",
                        NULL, test_a_frame_whose_ring_entry_cannot_be_read);
    qtest_add_data_func("sgi-ioc3-eth/status-failure-does-not-publish-valid",
                        NULL, test_a_status_failure_does_not_publish_valid);
    qtest_add_data_func("sgi-ioc3-eth/payload-failure-does-not-publish-valid",
                        NULL, test_a_payload_failure_does_not_publish_valid);
    qtest_add_data_func("sgi-ioc3-eth/fcs-failure-does-not-publish-valid",
                        NULL, test_an_fcs_failure_does_not_publish_valid);
    qtest_add_data_func("sgi-ioc3-eth/receive-burst-asserts-irq-once", NULL,
                        test_a_receive_burst_asserts_the_irq_once);
    qtest_add_data_func("sgi-ioc3-eth/receive-waits-for-arrival-deadline",
                        NULL, test_a_receive_waits_for_its_arrival_deadline);
    qtest_add_data_func("sgi-ioc3-eth/loopback-respects-a-full-arrival-line",
                        NULL, test_loopback_respects_a_full_arrival_line);
    qtest_add_data_func("sgi-ioc3-eth/unreadable-transmit-descriptor", NULL,
                        test_a_transmit_descriptor_that_cannot_be_read);
    qtest_add_data_func("sgi-ioc3-eth/unreadable-transmit-buffer", NULL,
                        test_a_transmit_buffer_that_cannot_be_read);

    for (i = 0; i < ARRAY_SIZE(walk_sizes); i++) {
        char *name = g_strdup_printf("sgi-ioc3-eth/reply-after-the-walk/%u",
                                     walk_sizes[i]);

        qtest_add_data_func(name, GUINT_TO_POINTER(walk_sizes[i]),
                            test_a_reply_after_the_walk_at_one_size);
        g_free(name);
    }
    qtest_add_data_func("sgi-ioc3-eth/burst-behind-one-doorbell", NULL,
                        test_a_burst_behind_one_doorbell);
    qtest_add_data_func("sgi-ioc3-eth/transmit-ring-wraps", NULL,
                        test_the_transmit_ring_wraps_three_times);
    qtest_add_data_func("sgi-ioc3-eth/ring-select-moves-the-wrap", NULL,
                        test_the_ring_select_bit_moves_the_wrap);
    qtest_add_data_func("sgi-ioc3-eth/receive-ring-wraps", NULL,
                        test_the_receive_ring_wraps_three_times);
    qtest_add_data_func("sgi-ioc3-eth/receive-ring-dry-and-recovers", NULL,
                        test_the_receive_ring_runs_dry_and_recovers);
    qtest_add_data_func("sgi-ioc3-eth/reported-count-at-guest-gates", NULL,
                        test_the_reported_count_at_the_guest_gates);
    qtest_add_data_func("sgi-ioc3-eth/delivery-truncation-point", NULL,
                        test_the_delivery_truncation_point);
    qtest_add_data_func("sgi-ioc3-eth/inline-area-boundary", NULL,
                        test_the_inline_area_boundary);
    qtest_add_data_func("sgi-ioc3-eth/assembled-frame-limit", NULL,
                        test_the_assembled_frame_limit);
    qtest_add_data_func("sgi-ioc3-eth/dma-boundary-split", NULL,
                        test_a_frame_split_on_the_dma_boundary);
    qtest_add_data_func("sgi-ioc3-eth/inline-path-off-the-fabric", NULL,
                        test_the_inline_path_off_the_fabric);
    qtest_add_data_func("sgi-ioc3-eth/pointer-path-off-the-fabric", NULL,
                        test_the_pointer_path_off_the_fabric);
    qtest_add_data_func("sgi-ioc3-eth/dp83843-twister-loopback-turns-a-frame"
                        "-round", NULL,
                        test_dp83843_twister_loopback_turns_a_frame_round);
    qtest_add_data_func("sgi-ioc3-eth/dp83843-loopback-requires-tw-lben",
                        NULL,
                        test_dp83843_loopback_requires_the_tw_lben_bit);
    qtest_add_data_func("sgi-ioc3-eth/dp83840a-bit5-does-not-select-loopback",
                        NULL,
                        test_dp83840a_bit5_does_not_select_loopback);
    qtest_add_data_func("sgi-ioc3-eth/dp83843-reset-reopens-the-backend",
                        NULL,
                        test_dp83843_reset_clears_lbr_and_reopens_the_backend);

    return g_test_run();
}
