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
#define ENET_TX_PRODUCE   (IOC3_BAR_ADDR + 0x134)
#define ENET_MII_DATA_R   (IOC3_BAR_ADDR + 0x14c)

#define MCR_RXOFF_SHIFT   3
#define MCR_RXOFF(halfwords) ((uint32_t)(halfwords) << MCR_RXOFF_SHIFT)
#define MCR_TX_DMA_EN     (1u << 13)
#define MCR_TX_EN         (1u << 14)
#define MCR_RX_DMA_EN     (1u << 15)
#define MCR_RX_EN         (1u << 16)

#define ENET_INT_RX_TIMER (1u << 0)
#define RPIR_ARM          (1u << 31)

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
 * Where a receive buffer's frame starts, past the two header words the
 * engine writes and the alignment padding OpenBSD's own descriptor keeps.
 */
#define RXD_BUF_OFFSET    66
#define RXH_VALID         0x80000000u
#define RXE_GOODPKT       0x40000000u

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
static QTestState *ioc3_start(uint32_t ring, uint32_t rxbuf)
{
    QTestState *qts;
    unsigned int i;

    /*
     * An explicit netdev rather than -nic, because this machine creates its
     * own default network device during machine init and would claim a -nic
     * before a -device is realized at all.
     */
    qts = qtest_initf("-M pc -display none "
                      "-netdev user,id=n0 "
                      "-device sgi-ioc3,addr=%02x.0,netdev=n0", IOC3_PCI_SLOT);

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

    ioc3_writel(qts, ENET_MAC_CONTROL, MCR_TX_DMA_EN | MCR_TX_EN |
                MCR_RX_DMA_EN | MCR_RX_EN | MCR_RXOFF(RXD_BUF_OFFSET / 2));
    ioc3_writel(qts, ENET_RX_RING_HI, 0);
    ioc3_writel(qts, ENET_RX_RING_LO, ring & 0xfffff000u);
    ioc3_writel(qts, ENET_RX_TIMER, 0);
    ioc3_writel(qts, ENET_RX_CONTROL, 0);
    for (i = 0; i < 4; i++) {
        dma_stl_be(qts, ring + i * 8, 0);
        dma_stl_be(qts, ring + i * 8 + 4, rxbuf + i * 0x1000);
    }
    ioc3_writel(qts, ENET_RX_PRODUCE, (4 << 3) | RPIR_ARM);

    return qts;
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
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56,
        0x08, 0x06,
        0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56, 10, 0, 2, 15,
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
    g_assert_cmphex(landed[0], ==, 0x52);
    g_assert_cmphex(landed[1], ==, 0x54);
    g_assert_cmphex(landed[2], ==, 0x00);

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
    qts = ioc3_start(ring, rxbuf);

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
    qts = ioc3_start(ring, rxbuf);

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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("sgi-ioc3-eth/inline-path-off-the-fabric", NULL,
                        test_the_inline_path_off_the_fabric);
    qtest_add_data_func("sgi-ioc3-eth/pointer-path-off-the-fabric", NULL,
                        test_the_pointer_path_off_the_fabric);

    return g_test_run();
}
