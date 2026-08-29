/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * QTest for the SGI IOC3's ethernet engine on a machine with none of the SGI
 * fabric under it.
 *
 * Every other result this device has is entangled with the Bridge, the
 * crosstalk fabric, the Hub and our own address map, so a defect found there
 * has to be argued into place. This plugs the same chip into a plain PCI
 * machine, where none of that exists, and drives the rings and the DMA from
 * outside. What it buys is attribution: a behaviour reproduced here belongs
 * to the device, and one that does not reproduce here belongs to the fabric.
 * It is the discipline that settled the SCSI adapter's defects, which were
 * reproduced on this same machine with no guest running at all.
 *
 * What this cannot prove, stated plainly because the test is ours and so is
 * the device: a test we wrote exercising a model we wrote is weaker evidence
 * than a real driver. The honest claim is that the fabric is excluded, not
 * that the engine is correct. The one assertion here that does not depend on
 * our own understanding is the backend's reply -- user-mode networking parses
 * what the engine transmitted and answers it, or does not, and it has no
 * opinion about what we meant.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"
#include <sys/socket.h>
#include <netinet/in.h>

/*
 * Where this test puts the chip's register window and its own scratch memory.
 * The window sits in the machine's PCI hole and the scratch well above the
 * first megabyte, in RAM nothing on this machine is using: no firmware runs
 * under qtest, so there is nothing to collide with.
 */
#define IOC3_BAR_ADDR   0xc0000000u
#define RAM_SCRATCH     0x02000000u

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

#define ENET_INT_RX_TIMER (1u << 0)
#define ENET_INT_RX_THRESHOLD (1u << 1)
#define ENET_INT_TX_COMPLETE  (1u << 22)
#define RPIR_ARM          (1u << 31)

/*
 * A management request: the register number in the low five bits, the
 * transceiver address above it, and a bit that asks for a read. The board
 * wires its one transceiver at address 31 -- every real Origin 200 capture
 * in the boot-log library prints it there.
 */
#define MII_CTL_READ      (1u << 10)
#define MII_PHY_SHIFT     5
#define PHY_ADDRESS       31

/*
 * The transceiver registers and bits this file drives, numbered by the
 * 802.3 management register set rather than by our own header.
 */
#define PHY_BMCR          0
#define PHY_BMSR          1
#define BMCR_LOOPBACK     0x4000
#define BMCR_SPEED100     0x2000
#define BMCR_DUPLEX       0x0100
#define BMSR_AN_COMP      0x0020
#define BMSR_LINK         0x0004

#define TXD_INT_DONE      (1u << 12)
#define TXD_DATA_VALID    (1u << 16)
#define TXD_BUF1_VALID    (1u << 17)
#define TXD_BUF2_VALID    (1u << 18)
#define TXD_BUF1_LEN_SHIFT 8
#define TXD_BUF2_LEN_SHIFT 20

/*
 * The descriptor's inline data area, from the drivers rather than from our
 * own header: OpenBSD's struct iec_txdesc puts its buffer after two words and
 * two eight-byte pointers, and Linux's struct ioc3_etxd declares data[104]
 * after the same four fields. A defect lived in this offset for a whole
 * session while every test that could have caught it used our number.
 */
#define TXD_DATA_OFFSET   24

/*
 * How much of the descriptor the inline area actually is, and how much the
 * field that measures it can name. OpenBSD computes IEC_TXD_BUFSIZE as
 * IEC_TXDESCSIZE - IEC_TXD_BUFOFFSET = 104, and Linux declares data[104]
 * outright. The length field beside it is seven bits in both -- OpenBSD's
 * IECTX_BUF0_LEN_MASK and Linux's ETXD_D0CNT_MASK are each 0x7f -- so a guest
 * can legally name 127 bytes of an area that holds 104. The two numbers
 * disagreeing is the boundary, not either number alone.
 */
#define TXD_DATA_SIZE     104
#define TXD_DATA_LEN_MAX  127

/*
 * The assembled frame's own limit, and it disagrees with the buffer by one.
 * The command word's total byte count is eleven bits in both drivers --
 * OpenBSD's IEC_TXCMD_DATALEN and Linux's ETXD_BYTECNT_MASK, each 0x7ff, so
 * 2047 -- while the engine assembles into 2048 bytes. A test that only ever
 * used round numbers would never see which of the two governs.
 */
#define TXD_BYTECNT_MAX   2047

/*
 * The sixteen-kilobyte boundary a transfer may not cross, which is why a
 * descriptor carries two buffer pointers at all rather than one. OpenBSD names
 * it IEC_DMA_BOUNDARY and splits at it explicitly: "The chip DMA engine can
 * not cross 16KB boundaries. If our mbuf doesn't fit, use the second pointer."
 * This is the only boundary here that comes from a hardware constraint rather
 * than from the width of a field.
 */
#define TX_DMA_BOUNDARY   0x4000u

/*
 * Where a receive buffer's frame starts, past the two header words the
 * engine writes and the alignment padding OpenBSD's own descriptor keeps.
 */
#define RXD_BUF_OFFSET    66
#define RXH_VALID         0x80000000u
#define RXH_BYTECNT_SHIFT 16

/*
 * The received byte count's field, eleven bits in both drivers -- OpenBSD's
 * IEC_RXSTAT_LEN_MASK and Linux's ERXBUF_BYTECNT_MASK are each 0x07ff0000 --
 * so the largest count the engine can report at all is 2047, and a frame of
 * more than 2043 bytes could not be described.
 */
#define RXH_BYTECNT_MASK  0x07ff0000u
#define RXH_BYTECNT_MAX   2047

/*
 * The split length field in the error word, which the same count arrives in a
 * second time: three low bits at 12 and ten high bits at 16, reassembled as
 * the high part shifted left three. Both drivers carry it and neither reads
 * it, so it is the field most likely to rot unnoticed.
 */
#define RXE_LEN_LO_SHIFT  12
#define RXE_LEN_HI_SHIFT  16

/*
 * What a real MAC captures alongside the frame and QEMU's network layer never
 * produces, so the engine adds it back into the count it reports. OpenBSD's
 * ETHER_CRC_LEN, and Linux subtracts the same four in its own receive path.
 */
#define ETHER_FCS_LEN     4

/*
 * Where the guests' own length gates fall, from OpenBSD's if_ether.h: a frame
 * is dropped below ETHER_MIN_LEN or above ETHER_MAX_LEN, both counted with the
 * CRC included -- which is the count this engine reports rather than the
 * payload it delivered. So the interesting delivered sizes are four less.
 */
#define ETHER_MIN_LEN     64
#define ETHER_MAX_LEN     1518

/*
 * The engine's own truncation point, and this one is ours rather than the
 * drivers'. It assembles into 2048 bytes and reserves eight, so a delivery is
 * cut at 2040 -- three below what the count field could still describe. No
 * legal Ethernet frame comes near either number, so the two limits have never
 * had to be reconciled; the test asserts the model's own and says which it is.
 */
#define RX_DELIVER_MAX    2040

/*
 * The good-packet bit in a receive buffer's error word, from the drivers
 * rather than from our header, on the same footing as the offsets above.
 * Linux declares it as ERXBUF_GOODPKT 0x40000000 and gates every delivered
 * frame on it, dropping the frame without it; OpenBSD declares the same value
 * as IEC_RXERR_GOODPACKET and never reads it, testing the error bits beside it
 * instead. A test carrying an untraceable copy of a constant is how the
 * transmit data offset stayed wrong in two files at once, each agreeing with
 * the other and neither with the hardware.
 */
#define RXE_GOODPKT       0x40000000u

/*
 * The long-event and bad-packet bits beside it, again from a driver rather
 * than from our header: OpenBSD's if_iecreg.h names them IEC_RXERR_LONGEVENT
 * 0x10000000 and IEC_RXERR_BADPACKET 0x20000000, and its iec_rxintr drops
 * any frame wearing either. The engine sets both when it truncates a
 * delivery, so what a guest sees of a frame too long to represent is a drop
 * with a printed reason rather than a silently shortened frame.
 */
#define RXE_LONGEVENT     0x10000000u
#define RXE_BADPKT        0x20000000u

/*
 * The register window is declared big-endian and this machine is not, so the
 * memory system swaps between the value a test hands it and the value the
 * device sees. Every access here goes through these two rather than through
 * qtest_readl and qtest_writel directly, because getting that swap wrong
 * produces registers that read back whatever was written and a device that
 * never does anything -- which looks like a model that ignores its inputs
 * rather than like a test with its bytes reversed.
 */
static void ioc3_writel(QTestState *qts, uint64_t addr, uint32_t val)
{
    qtest_writel(qts, addr, bswap32(val));
}

static uint32_t ioc3_readl(QTestState *qts, uint64_t addr)
{
    return bswap32(qtest_readl(qts, addr));
}

/*
 * The rings and descriptors are read by the device with explicit big-endian
 * accessors wherever it runs, so they are written here as bytes rather than
 * through a target-endian store.
 */
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

/*
 * Configuration space through the machine's own address and data ports,
 * which is all a machine with no firmware run offers.
 */
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
 * Bring the chip up on this machine: place its window, let it decode and
 * master, and put the ethernet engine in the state a driver leaves it in --
 * both directions enabled, the frame offset OpenBSD programs, four receive
 * buffers supplied, and the receive timer armed with the immediate reload
 * both drivers write.
 */
/*
 * Everything after the machine exists: place the window, let the chip decode
 * and master, and leave the engine in the state a driver leaves it in. Shared
 * by both attachments below, because which backend is on the other side must
 * be the only thing that differs between them.
 *
 * Promiscuous, unlike a real driver: this file's own address-filter
 * consideration is out of its scope (the IOC3 datasheet in the Origami
 * project tree, docs/datasheets/ioc3.md, covers the filter contract, and
 * tests/qtest/sn0-ioc3-test.c is where it is exercised). Several tests here
 * address a frame with an arbitrary byte pattern to probe gather and
 * DMA-boundary mechanics rather than a station address at all, so without
 * this bit the filter this device now applies would drop them for a reason
 * that has nothing to do with what they test.
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

    /*
     * A read that proves the window is decoding and the byte order above is
     * right in one go. The management data register answers all-ones for the
     * absent transceiver at reset, which is the one reset value in this block
     * that is not the same forwards and backwards.
     */
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

    /*
     * An explicit netdev rather than -nic, because this machine creates its
     * own default network device during machine init and would claim a -nic
     * before a -device is realized at all.
     */
    qts = qtest_initf("-M pc -display none "
                      "-netdev user,id=n0 "
                      "-device sgi-ioc3,addr=%02x.0,netdev=n0", IOC3_PCI_SLOT);
    ioc3_bring_up(qts, ring, rxbuf, buffers);
    return qts;
}

/*
 * The other attachment: a connected socket pair rather than user-mode
 * networking. What it buys is that the test chooses every byte in both
 * directions, which user-mode networking cannot give -- it answers what it
 * understands and nothing else, so it cannot carry an arbitrary length or an
 * arbitrary payload. Size boundaries need exactly that, so they use this and
 * the interleaving tests use the other.
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
 * A bound on how long the test's end of the pair will block for a frame that
 * is late rather than never coming: delivery to a socket backend is not
 * guaranteed to have reached it by the time the register write that caused it
 * returns.
 */
static void net_socket_set_recv_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

/*
 * The framing the socket backend uses: a four-byte network-order length ahead
 * of each frame, in both directions.
 */
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

/*
 * Nothing may arrive. The engine transmits during the produce write, so a
 * frame it did send is already buffered by the time this runs; a short
 * timeout only bounds the wait for the expected silence.
 */
static void net_socket_expect_no_frame(int fd)
{
    uint32_t hdr;

    net_socket_set_recv_timeout(fd, 1);
    errno = 0;
    g_assert_cmpint(recv(fd, &hdr, sizeof(hdr), 0), ==, -1);
    g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
}

/*
 * An ARP request for the address user-mode networking answers on. Padded to
 * whatever length the caller wants, because a longer frame is still a valid
 * request -- the backend reads the first forty-two bytes and ignores the
 * rest -- and length is what decides which of the two transmit paths a real
 * driver would use.
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

/*
 * What a transmitted frame earns: the backend parses it and replies, the
 * engine delivers the reply into the ring, and the receive timer source is
 * raised. Every one of those is a step the SGI fabric plays no part in on
 * this machine.
 */
static void assert_reply_landed(QTestState *qts, uint32_t rxbuf)
{
    uint8_t landed[42];

    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 8);
    g_assert_cmphex(dma_ldl_be(qts, rxbuf) & RXH_VALID, ==, RXH_VALID);

    /*
     * Reported good. Linux gates every delivered frame on this bit and drops
     * the frame without it, while OpenBSD reads the error bits beside it and
     * never looks at this one -- a field only one of the two guests can miss.
     */
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

/*
 * A frame small enough for the descriptor's own inline area, which is the
 * path a driver takes for anything at or under 104 bytes. This is the path
 * that shipped broken for a session while every passing test used the other
 * one, so it is the one worth having on a machine that cannot be blamed.
 */
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

/*
 * A frame too large for the inline area, carried through the two out-of-line
 * pointers and split across both of them -- the shape a driver produces when
 * its buffer would cross the sixteen-kilobyte boundary the engine cannot.
 */
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
 * Send one frame and return what the backend received, so a size boundary is
 * asserted on the bytes that left the machine rather than on a register the
 * engine wrote for itself. The caller picks the path: inline for anything the
 * area can hold, the out-of-line pointers otherwise, split across both if it
 * is told where.
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
        /*
         * Buffer one ends exactly on the boundary and buffer two starts on
         * it, which is the split OpenBSD's own transmit path produces when an
         * mbuf would cross it. Nothing else in this suite makes that shape.
         */
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

/*
 * The inline area holds 104 bytes and the field that measures it holds seven
 * bits, so a guest can name 127 of an area that has 104. Either side of the
 * area's own size, then the two lengths that exceed it, all on the wire.
 *
 * These are the sizes nothing else here uses: the existing tests pick round
 * numbers well clear of the edge, which is how a defect lived in this path for
 * a session while every one of them passed.
 */
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
         * At or under the area, every byte named arrives. Over it, the frame
         * stops at the area's end rather than at the field's value -- an
         * engine trusting the field sends bytes from past its own copy of the
         * descriptor, and they would be whatever followed it in memory.
         */
        g_assert_cmpint(len, ==, expect);
        g_assert_cmpmem(got, len, frame, expect);

        qtest_quit(qts);
        close(sv[0]);
    }
}

/*
 * The command word's byte count is eleven bits and is what the engine sends:
 * the segment lengths say where the bytes live, the count says how many
 * leave. Either side of the count's own maximum and totals past it, built
 * from both pointers so the total is what is being tested rather than any
 * one buffer's length. A total past the field wraps in the descriptor the
 * guest actually wrote -- one past the maximum carries a count of zero and
 * puts nothing on the wire, and 3000 carries its low eleven bits, 952.
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

/*
 * A frame split exactly on the sixteen-kilobyte boundary the engine may not
 * cross. This is the shape a real driver produces -- OpenBSD computes the
 * distance to the next boundary and puts the remainder in the second pointer
 * for that reason and no other -- and it is the only boundary in this file
 * that comes from a hardware constraint rather than a field width.
 */
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

    /*
     * 250 bytes before the boundary and 350 after it, so buffer one ends on
     * it exactly. A gather that ignored the second pointer, or read both from
     * one region, produces a frame of the wrong length or the wrong bytes and
     * either shows here.
     */
    len = send_and_capture(qts, sv[0], frame, sizeof(frame), sizeof(frame),
                           250, got, sizeof(got));

    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Deliver one frame from the backend and read back everything the engine wrote
 * about it. Returns the reported byte count; the caller checks the rest.
 */
static uint32_t receive_and_inspect(QTestState *qts, int host_fd,
                                    const uint8_t *frame, size_t len,
                                    uint32_t rxbuf, uint32_t slot,
                                    uint8_t *landed, size_t landed_size)
{
    uint32_t base = rxbuf + slot * 0x1000;
    uint32_t word0, word1, count, split;
    int attempt;

    net_socket_send_frame(host_fd, frame, len);

    /*
     * A socket backend delivers on a later pass of the main loop, so the
     * consume index is polled rather than read once. The interleaving tests
     * elsewhere need the opposite and use a backend that answers inside the
     * call; here the delay is harmless and the size control is what matters.
     */
    for (attempt = 0; attempt < 200; attempt++) {
        if (ioc3_readl(qts, ENET_RX_CONSUME) != slot * 8) {
            break;
        }
        g_usleep(5000);
    }
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, (slot + 1) * 8);

    word0 = dma_ldl_be(qts, base);
    word1 = dma_ldl_be(qts, base + 4);

    g_assert_cmphex(word0 & RXH_VALID, ==, RXH_VALID);
    g_assert_cmphex(word1 & RXE_GOODPKT, ==, RXE_GOODPKT);

    count = (word0 & RXH_BYTECNT_MASK) >> RXH_BYTECNT_SHIFT;

    /*
     * The same count arrives a second time in the split field, and the two
     * must agree. Neither driver reads the split one, so nothing else in the
     * system would notice it drifting.
     */
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
 * The reported count against the delivered size, at the sizes where something
 * changes. Two of these are the guests' own gates -- a frame is dropped below
 * ETHER_MIN_LEN and above ETHER_MAX_LEN, both counted with the CRC the engine
 * adds back -- so 60 bytes delivered reports exactly the minimum and 59
 * reports one short of it, while 1514 reports exactly the maximum and 1515
 * reports one over. A model that dropped the four-byte addition would put
 * every one of those on the wrong side of the guest's comparison.
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

/*
 * Where the engine stops. It assembles into 2048 bytes and reserves eight, so
 * a delivery is cut at 2040 -- and the count field could still describe three
 * more, which is the tell that this limit is the model's own rather than the
 * hardware's. Either side of it and one past, with the truncated case required
 * to report the truncated length rather than what arrived.
 */
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

        /*
         * The count describes what was kept, not what was offered. Reporting
         * the offered length would send a driver reading past the bytes that
         * actually arrived.
         */
        g_assert_cmpuint(count, ==, kept + ETHER_FCS_LEN);
        g_assert_cmpmem(landed, kept, frame, kept);

        /*
         * And a truncated delivery says so where a driver looks: the long
         * and bad bits OpenBSD drops on, on the one size that was cut and
         * on neither of its neighbours.
         */
        g_assert_cmphex(word1 & (RXE_LONGEVENT | RXE_BADPKT), ==,
                        kept == sizes[n] ? 0
                                         : RXE_LONGEVENT | RXE_BADPKT);
    }

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The receive ring's size and the index that walks it, from the drivers:
 * OpenBSD's IEC_NRXDESC_MAX is 512 and Linux sizes its own ring the same way,
 * and both indices are byte offsets rather than entry numbers -- twelve bits,
 * stepping eight per entry, so they run 0 to 4088 and wrap.
 */
#define RX_RING_ENTRIES   512
#define RX_RING_STRIDE    8
#define RX_INDEX_WRAP     (RX_RING_ENTRIES * RX_RING_STRIDE)

/*
 * The transmit ring, which comes in two sizes chosen by the low bit of the
 * base register. OpenBSD's IEC_NTXDESC_MAX is 128 descriptors of
 * IEC_TXDESCSIZE, and the larger arrangement is four times that.
 */
#define TX_DESC_SIZE      128
#define TX_RING_SMALL     128
#define TX_RING_LARGE     512
#define TX_RING_SEL       1u
#define TX_CONSUME_IDLE   (1u << 31)
#define TX_CONSUME_MASK   0xffffu

/*
 * Wait for the consume index to reach a value, since a socket backend
 * delivers on a later pass of the main loop rather than inside the write.
 */
static void wait_for_rx_consume(QTestState *qts, uint32_t want)
{
    int attempt;

    for (attempt = 0; attempt < 2000; attempt++) {
        if (ioc3_readl(qts, ENET_RX_CONSUME) == want) {
            return;
        }
        g_usleep(1000);
    }
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, want);
}

/*
 * Drive the receive ring right round three times. One lap proves the index
 * returns to zero; three prove it keeps doing it, which is a different claim
 * and the one a wrap defect off by a single entry survives.
 *
 * The ring holds 512 entries and the buffers behind them are reused, exactly
 * as OpenBSD does -- it fills all 512 pointer slots from a smaller pool with
 * `i & (sc_nrxdesc - 1)`. So this exercises the index arithmetic rather than
 * anyone's memory footprint.
 */
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
        /*
         * Keep the engine supplied the way a driver does, refilling well
         * ahead of where it is consuming rather than one entry at a time.
         */
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
     * Three whole laps land exactly back on zero. On its own that is a weak
     * thing to assert -- an engine that delivered nothing at all would also
     * finish at zero -- so what carries this test is the check inside the
     * loop, which names the exact expected index after every one of the
     * 1536 deliveries. The two together say the index visited every value
     * and came home, rather than merely ending where it started.
     */
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);

    /*
     * And the last lap really wrote a frame, rather than the ring turning
     * over an engine that had stopped delivering.
     */
    g_assert_cmphex(dma_ldl_be(qts, rxbuf + ((RX_RING_ENTRIES - 1) % BUFFERS) *
                               0x1000) & RXH_VALID, ==, RXH_VALID);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The receive ring running out, and coming back. When the engine has consumed
 * everything software supplied it must stop taking frames rather than
 * overwrite a buffer it has already filled -- and when software supplies more,
 * the frames the backend has been holding must go in without another arriving
 * to prompt it.
 *
 * This case was found by accident: a six-frame test against a four-buffer ring
 * stopped dead at four. An accident that reproduces on purpose is a better
 * starting point than a case somebody imagined, so it is asserted here.
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
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==,
                    BUFFERS * RX_RING_STRIDE);

    /*
     * Supplying buffers is what releases them. Nothing else happens here --
     * no further frame arrives -- so an engine that only looked at its ring
     * when something new turned up would sit on these forever.
     */
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

/*
 * Fill a run of transmit descriptors, each carrying its own index so the
 * frames can be told apart on the wire, and ring the doorbell once.
 */
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
 * A whole batch behind one doorbell. Neither driver rings once per descriptor
 * -- OpenBSD queues everything ifq_dequeue gives it and writes the produce
 * index once at the end -- so an engine that sent only the first descriptor of
 * a batch would stall every driver we have, and would look fine to any test
 * that queued one frame at a time.
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
        /*
         * In order, and all of them: the index each descriptor carries says
         * which it was.
         */
        g_assert_cmphex(got[14], ==, (uint8_t)(n >> 8));
        g_assert_cmphex(got[15], ==, (uint8_t)n);
    }

    g_assert_cmphex(ioc3_readl(qts, ENET_TX_CONSUME), ==,
                    (BURST * TX_DESC_SIZE) | TX_CONSUME_IDLE);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A transmit descriptor that cannot be read. Every field the walk needs comes
 * out of that fetch -- the command bits, the byte counts, both buffer
 * addresses and the inline area -- so an engine that reads it without looking
 * at the result assembles a frame out of whatever the failed reads left on its
 * stack, sends it, and steps the consume index over a descriptor it never saw.
 * The frame is what makes this worth a test: the wire carries something no
 * driver asked for.
 *
 * The walk stops on the descriptor instead, and the index stays on it: the
 * fetch is what failed, so there is nothing to say the descriptor was
 * processed. A guest that repairs the address and rings again gets it sent,
 * which is the second half below.
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
 * The same for the frame's own bytes rather than its descriptor. A pointer
 * descriptor names memory the engine gathers the frame from, and a gather that
 * fails leaves the frame buffer holding whatever it held before -- the frame
 * before this one, or nothing at all. Sending that is the worst version of
 * this fault, because it puts bytes on the wire that no guest ever wrote and
 * tells the driver the transmit succeeded.
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

/*
 * The transmit ring right round three times, on the smaller of the two
 * arrangements. As with the receive ring, one lap proves the index returns and
 * three prove it keeps doing so.
 */
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

    /*
     * Three whole laps, so the index is back where it started. The frames
     * checked above are what makes that mean something: an engine that
     * stopped sending would also finish at zero.
     */
    g_assert_cmphex(ioc3_readl(qts, ENET_TX_CONSUME) & TX_CONSUME_MASK,
                    ==, 0);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * Which arrangement the low bit of the base register selects, asserted where
 * the two differ: at descriptor 128. On the small ring that index is the
 * start of the second lap; on the large one it is an ordinary descriptor a
 * long way from the end. Sending 129 and reading where the consume index came
 * to rest tells the two apart, and nothing else in this file does -- every
 * other transmit test stays inside the first 128 either way.
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

    /*
     * On the large ring the index is simply 129 descriptors in. An engine
     * ignoring the select bit would have wrapped at 128 and be sitting on
     * one.
     */
    g_assert_cmphex(ioc3_readl(qts, ENET_TX_CONSUME) & TX_CONSUME_MASK,
                    ==, COUNT * TX_DESC_SIZE);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * The interleaving the whole engine was rewritten for: a frame arriving while
 * the transmit walk is still running. One test covers it, at one size, on a
 * frame the backend happens to answer -- so the size at which a reply can be
 * lost is untested everywhere except that one point.
 *
 * User-mode networking answers a hand-built echo request synchronously, inside
 * the same call that sent it, and its reply is the same length as the request.
 * That gives the receive size freely, which the ARP the other test uses cannot:
 * an ARP reply is one fixed length.
 *
 * A TRAP THAT LOOKS LIKE A PASSING TEST. The backend learns a station address
 * from ARP input only, so an IP conversation with no ARP in front of it makes
 * it ask who we are first. What comes back then is its own ARP request -- a
 * delivered frame, a valid buffer header, a plausible byte count, every field
 * a test would check looking right, and none of it the reply being tested.
 * The exchange below is completed before any size is measured, for that reason
 * and no other.
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

/*
 * Our own address and the one user-mode networking answers on, which are its
 * documented defaults rather than anything this model chooses.
 */
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

/*
 * Queue one frame and ring the doorbell, taking the next descriptor each
 * time: the walk runs from the consume index to the produce index, so a
 * second doorbell against the first slot sends a descriptor nobody wrote.
 */
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

/*
 * One size per test case rather than a loop over all of them, and that is not
 * a stylistic choice. The mutation these exist to fail against is the original
 * defect -- the transmit walk snapshotting the interrupt status at entry and
 * writing it back at exit -- and a single case aborts on the first size that
 * fails, which says nothing about the rest. Split, every size reports its own
 * verdict, so "it fails" becomes "it fails at all seven" and a size that
 * quietly does not exercise the interleaving cannot hide behind one that does.
 */
static void test_a_reply_inside_the_walk_at_one_size(const void *data)
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

    /*
     * The prelude. Its own reply is the backend's ARP answer, and taking it
     * here is what stops it being mistaken for the first size's echo.
     */
    build_arp_request(frame, 42);
    tx_one(qts, base, 0, frame, 42);
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, RX_RING_STRIDE);
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
         * No polling anywhere in this loop, deliberately. The reply is
         * delivered inside the doorbell write, and a test that waited for it
         * would also pass on a delivery arriving afterwards -- which is the
         * case the engine has always got right and not the one that broke.
         */
        g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==,
                        (slot + 1) * RX_RING_STRIDE);

        word0 = dma_ldl_be(qts, base_addr);
        word1 = dma_ldl_be(qts, base_addr + 4);
        g_assert_cmphex(word0 & RXH_VALID, ==, RXH_VALID);
        g_assert_cmphex(word1 & RXE_GOODPKT, ==, RXE_GOODPKT);

        /*
         * The reply mirrors the request, so the count is the size under test
         * plus the CRC the engine adds back.
         */
        g_assert_cmpuint((word0 & RXH_BYTECNT_MASK) >> RXH_BYTECNT_SHIFT,
                         ==, size + ETHER_FCS_LEN);

        /* An echo reply rather than the backend's own ARP question. */
        qtest_memread(qts, base_addr + RXD_BUF_OFFSET, landed, sizeof(landed));
        g_assert_cmphex(landed[12], ==, 0x08);
        g_assert_cmphex(landed[13], ==, 0x00);
        g_assert_cmphex(landed[34], ==, 0x00);

        /*
         * And the receive source survived the walk that carried it. This is
         * the assertion the milestone turns on, now made at seven sizes
         * rather than one.
         */
        status = ioc3_readl(qts, ENET_INT_STATUS);
        g_assert_cmphex(status & ENET_INT_RX_TIMER, ==, ENET_INT_RX_TIMER);
        ioc3_writel(qts, ENET_INT_STATUS, 0xffffffffu);

    }

    qtest_quit(qts);
}

static const uint32_t walk_sizes[] = { 74, 104, 105, 170, 554, 1042,
                                       ETHER_MAX_LEN - ETHER_FCS_LEN };

/*
 * The two halves of a management transfer, in the order both drivers use:
 * a read is a request naming an address and register, answered in the data
 * register; a write loads the data register first.
 */
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
 * The backend's link state, read back through the transceiver's status
 * word. Out of the box the backend's link is up, so the status register
 * reports a link once negotiation is forced closed -- and set_link is the
 * cable being pulled: the link and completion bits must drop while it is
 * out and return when it is back, or a guest that watches its carrier
 * keeps trusting an interface the operator disconnected.
 */
static void test_the_backend_link_reaches_the_status_word(const void *data)
{
    QTestState *qts;
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;

    qts = ioc3_start(ring, rxbuf, 2);

    /*
     * Force the link up the way the PROM's network boot does: a control
     * write that is not a reset closes negotiation at once.
     */
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
 * A frame held back by transceiver loopback goes in when the loopback
 * clears. While either loopback is selected the block refuses the backend's
 * traffic, and the refusal queues the frame rather than dropping it -- so
 * clearing the loopback must deliver what was queued without another frame
 * arriving to prompt it. SGI's power-on diagnostic is the reason this edge
 * exists at all: it walks the transceiver's loopback selects while the
 * backend is free to have traffic waiting.
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
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);

    /*
     * Nothing arrives after this write. Only the flush it triggers can
     * deliver the queued frame.
     */
    mii_write(qts, PHY_BMCR, BMCR_SPEED100 | BMCR_DUPLEX);
    wait_for_rx_consume(qts, RX_RING_STRIDE);
    g_assert_cmphex(dma_ldl_be(qts, rxbuf) & RXH_VALID, ==, RXH_VALID);

    qtest_quit(qts);
    close(sv[0]);
}

/*
 * A frame whose receive ring entry cannot be read. That entry carries the
 * address every write the delivery makes is aimed at -- both header words,
 * the payload, and the phantom check sequence -- so a fetch that fails
 * leaves the engine with no destination at all. Continuing past it writes
 * those five kilobytes wherever the failed read left the address variable
 * pointing, advances the consume index and raises the receive interrupt:
 * the driver is told a good packet is waiting in a buffer nothing was
 * written to, and takes whatever the buffer held before. Dropping the frame
 * is the honest outcome, since the transfer never happened and this block
 * has no status a driver reads for one the fabric refused.
 *
 * The ring is moved above every address the machine answers rather than
 * unmapped underneath it, so the failure is the memory system's own. MAC
 * loopback turns the transmit round inside the block, which puts the whole
 * delivery inside the doorbell write and leaves nothing to wait for.
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

    /*
     * The drop costs the engine nothing else. With the ring back where
     * memory answers, the next frame takes the slot the dropped one left
     * alone, which is also what says the index never moved for it.
     */
    ioc3_writel(qts, ENET_RX_RING_HI, 0);
    tx_one(qts, base, 1, frame, sizeof(frame));

    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, RX_RING_STRIDE);
    g_assert_cmphex(dma_ldl_be(qts, rxbuf) & RXH_VALID, ==, RXH_VALID);
    qtest_memread(qts, rxbuf + RXD_BUF_OFFSET, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));

    qtest_quit(qts);
}

/*
 * The complement, standing alone for the reason set out in the origin200
 * version of this case: an assertion about an absence should not inherit a
 * wait. Nothing is asked for here, so nothing can come back, and the absence
 * holds by construction rather than because a window expired.
 *
 * Two routes could bring a frame back and only one is the backend. Loopback
 * returns a transmit into this device's own receive path whenever either
 * selection is set. Both are clear out of reset and neither is set here, so
 * the silence holds -- but on a state this case would otherwise never
 * mention, which is why the MAC control write is spelled out in full.
 */
static void test_a_transmit_with_no_reply_raises_nothing(const void *data)
{
    QTestState *qts;
    uint8_t quiet[64];
    uint32_t base = RAM_SCRATCH;
    uint32_t ring = RAM_SCRATCH + 0x8000;
    uint32_t rxbuf = RAM_SCRATCH + 0xc000;
    uint32_t status;

    qts = ioc3_start(ring, rxbuf, 2);
    ioc3_writel(qts, ENET_TX_RING_HI, 0);
    ioc3_writel(qts, ENET_TX_RING_LO, base & 0xffffc000u);

    memset(quiet, 0, sizeof(quiet));
    memset(quiet, 0xff, 6);
    quiet[12] = 0x88;
    quiet[13] = 0xb5;
    tx_one(qts, base, 0, quiet, sizeof(quiet));

    /*
     * A buffer is free and the index has not moved, so nothing arrived --
     * which is what makes the next line a statement about the source.
     */
    g_assert_cmphex(ioc3_readl(qts, ENET_RX_CONSUME), ==, 0);
    status = ioc3_readl(qts, ENET_INT_STATUS);
    g_assert_cmphex(status & (ENET_INT_RX_TIMER | ENET_INT_RX_THRESHOLD),
                    ==, 0);
    g_assert_cmphex(status & ENET_INT_TX_COMPLETE, ==, ENET_INT_TX_COMPLETE);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    unsigned int i;

    g_test_init(&argc, &argv, NULL);

    /*
     * Either side of the descriptor's inline area, then sizes that can only
     * go through the pointers, up to the largest frame a guest accepts. The
     * transmit path changes at 104 and the receive size changes at each.
     */
    qtest_add_data_func("sgi-ioc3-eth/no-reply-raises-nothing", NULL,
                        test_a_transmit_with_no_reply_raises_nothing);
    qtest_add_data_func("sgi-ioc3-eth/backend-link-reaches-status", NULL,
                        test_the_backend_link_reaches_the_status_word);
    qtest_add_data_func("sgi-ioc3-eth/queued-frame-lands-after-loopback", NULL,
                        test_a_queued_frame_lands_when_phy_loopback_clears);
    qtest_add_data_func("sgi-ioc3-eth/unreadable-ring-entry-drops-the-frame",
                        NULL, test_a_frame_whose_ring_entry_cannot_be_read);
    qtest_add_data_func("sgi-ioc3-eth/unreadable-transmit-descriptor", NULL,
                        test_a_transmit_descriptor_that_cannot_be_read);
    qtest_add_data_func("sgi-ioc3-eth/unreadable-transmit-buffer", NULL,
                        test_a_transmit_buffer_that_cannot_be_read);

    for (i = 0; i < ARRAY_SIZE(walk_sizes); i++) {
        char *name = g_strdup_printf("sgi-ioc3-eth/reply-inside-the-walk/%u",
                                     walk_sizes[i]);

        qtest_add_data_func(name, GUINT_TO_POINTER(walk_sizes[i]),
                            test_a_reply_inside_the_walk_at_one_size);
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

    return g_test_run();
}
