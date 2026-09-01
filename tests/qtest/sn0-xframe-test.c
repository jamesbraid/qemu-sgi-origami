/*
 * Neterion/S2io Xframe behind the Origin 200 Bridge: the device<->Bridge
 * byte-order composition its own x86_64 qtest cannot reach.
 *
 * tests/qtest/xframe-test.c proves the device's register and DMA contract on a
 * generic little-endian PC, where an 8-byte access is one native cycle and no
 * host bridge sits in the path. This file drives the same device on the real
 * Origin 200 topology, at a free PCI slot behind the SGI Bridge, so the
 * composition the PC machine has no way to exercise is what is under test: the
 * Bridge's register-window byte-order correction over the device's
 * little-endian delivery, and the Bridge's DMA byte-lane path over the
 * device's native-little-endian byte-buffer DMA.
 *
 * The resolved contract (docs/library/nic-on-bridge-conventions.md): the
 * device declares little-endian and delivers native little-endian; the Bridge
 * corrects register MMIO and DMA per the guest-programmed slot DEV register,
 * which for a NIC slot leaves every swap bit clear. Xframe's registers are
 * 64-bit, so the slot is marked DEV_64BIT_DEVICE and a 64-bit register access
 * is served in one eight-byte cycle -- the single-cycle path the Bridge's
 * 64-bit-device fix added. Under that setting a guest 64-bit register access
 * composes to identity, and a DMA transfer carries each aligned 32-bit word
 * byte-reversed -- the same word-value correction the on-Bridge QLogic ISP
 * suite pins (tests/qtest/sn0-qlogic-isp-test.c's normalise_words), because the
 * NIC's slot is not one the board exempts from the byte swap.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include <sys/socket.h>
#include <netinet/in.h>

/* Crosstalk space and the Bridge's widget window inside it. */
#define SPACE_XTALK             0x20000000000ULL
#define BRIDGE_WIDGET           8
#define BRIDGE_BASE             (SPACE_XTALK + ((uint64_t)BRIDGE_WIDGET << 24))
#define REG_DATA                4

#define MACHINE_WITH_BRIDGE     "-M origin200,io-bridge=on"

/*
 * The base I/O board fills PCI devices 0 and 1 (the SCSI adapters) and 2 (the
 * IOC3, which the board exempts from the byte-lane swap). The first free,
 * non-exempt slot is 3 -- the slot generality is the whole point: the Bridge
 * corrects by slot, so any free slot would do, and a non-exempt one is what
 * puts the byte-lane path in front of the device's DMA.
 */
#define XFRAME_SLOT             3

/* The per-slot device register: 32 bits in the low half of an 8-byte slot. */
#define BRIDGE_DEVICE(n)        (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)
#define DEV_MEMORY_SPACE        (1u << 12)
#define DEV_SWAP_PROGRAMMED     (1u << 13)   /* programmed-access (reg) swap */
#define DEV_64BIT_DEVICE        (1u << 22)   /* doubleword in one cycle */

/* Type 0 configuration space: eight devices 4 KB apart. */
#define BRIDGE_CFG0             0x020000
#define CFG0_DEVICE_STRIDE      0x1000
#define CFG0(dev)               (BRIDGE_BASE + BRIDGE_CFG0 + \
                                 (dev) * CFG0_DEVICE_STRIDE)
#define PCI_VENDOR_ID           0x00
#define PCI_COMMAND             0x04
#define PCI_COMMAND_MEMORY      0x0002
#define PCI_COMMAND_MASTER      0x0004
#define PCI_BAR0                0x10
#define PCI_BAR0_HIGH           0x14
#define PCI_BAR2                0x18

/*
 * The slot's window sits at a fixed widget offset the Bridge derives from the
 * slot number (2 MB for the first two slots, 1 MB thereafter, laid end to
 * end): slot 3 is the fourth window, 0x200000 + 2 * 2 MB + 1 MB.
 */
#define WINDOW_SLOT3            (BRIDGE_BASE + 0x700000)

/*
 * Where each BAR is decoded in the Bridge's PCI space. The slot window points
 * PCI address bits 31:20 at one of these; a 1 MB window reaches BAR0's PIF
 * registers from one base and the TxP doorbell from the other, so the window
 * is aimed per test rather than needing both at once.
 */
#define PIF_BAR_PCI             0x10000000u  /* base field 0x100 */
#define TXP_BAR_PCI             0x10100000u  /* base field 0x101 */

/*
 * DMA translation: one page-table entry maps a translated-window PCI address
 * to the physical page the descriptors and buffers live in. The device issues
 * translated addresses; the entry sends them to TARGET_PAGE.
 */
#define ATE_BASE                0x010000
#define ATE(n)                  (BRIDGE_BASE + ATE_BASE + (n) * 8)
#define ATE_ENTRY(page, widget) ((uint64_t)(page) | \
                                 ((uint64_t)(widget) << 8) | 1)
#define PCI_TRANSLATED          0x40000000u
#define TARGET_PAGE             0x00100000u

/* Descriptors and buffers within the translated page (PCI and physical). */
#define DESC_IOVA               (PCI_TRANSLATED + 0x000)
#define DESC_PHYS               (TARGET_PAGE + 0x000)
#define TX_BUF_IOVA             (PCI_TRANSLATED + 0x100)
#define TX_BUF_PHYS             (TARGET_PAGE + 0x100)
#define RX_BUF_IOVA             (PCI_TRANSLATED + 0x200)
#define RX_BUF_PHYS             (TARGET_PAGE + 0x200)

/*
 * The device's PCI identity and register offsets, restated from the xframe
 * datasheet (docs/datasheets/xframe.md in the origami project tree, not this
 * repository) the way tests/qtest/xframe-test.c restates them -- a test pins
 * the contract independently of the model.
 */
#define XFRAME_VENDOR           0x17d5
#define XFRAME_DEVICE_XENA      0x5831
#define XFRAME_PIF_WINDOW_SIZE  0x4000

#define ADAPTER_STATUS          0x0108
#define PIF_RD_SWAPPER_FB       0x0910
#define PRC_RXD0_0              0x1930
#define PRC_CTRL_0              0x1970
#define RX_TRAFFIC_INT          0x08f0

/* The two fixed 64-bit constants the register composition is checked on. */
#define SWAPPER_MAGIC           0x0123456789abcdefULL
#define ADAPTER_STATUS_QUIESCENT 0xf400ffc300000000ULL

#define RC_IN_SVC               (1ULL << 56)
#define RX_TRAFFIC_RING0        (1ULL << 63)

/* TxP doorbell (BAR2): the list pointer at +0, the launch control at +8. */
#define TXP_TXDL_PAR            0x00
#define TXP_TXDL_LCR            0x08
#define TXDL_NUMTXD(n)          ((uint64_t)(n) << 56)
#define TXDL_LGC_FIRST          (1ULL << 49)
#define TXDL_LGC_LAST           (1ULL << 48)

/* A TxD: control1, control2, buffer pointer, host control. */
#define TXD_OWN                 (1ULL << 56)
#define TXD_GCF                 (1ULL << 41)
#define TXD_GCL                 (1ULL << 40)

/* A 1-buffer RxD: host control, control1, control2, buffer pointer. */
#define RXD_OWN                 (1ULL << 56)
#define RXD_XCODE_SHIFT         48
#define RXD_XCODE_MASK          0xf
#define RXD_BUFSIZ_SHIFT        48
#define RX_BUF_CAPACITY         128

#define TEST_MAC                "52:54:00:12:34:56"
static const uint8_t station_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

/* A multiple of four, so every DMA chunk is one aligned 32-bit word. */
#define FRAME_LEN               64

/*
 * The socket backend frames each packet with a four-byte network-order length,
 * in both directions -- the same helpers tests/qtest/xframe-test.c uses.
 */
static void net_socket_set_recv_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)),
                    ==, 0);
}

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
    g_assert_cmpuint(len, <=, buf_size);
    g_assert_cmpint(recv(fd, buf, len, 0), ==, (int)len);
    return len;
}

/*
 * Big-endian pack/unpack of a 64-bit descriptor word in a byte buffer -- the
 * descriptor format's reset default, which the always-big-endian Origin 200
 * guest never reprograms away from (hw/net/xframe.c's ldq_be_p/stq_be_p
 * descriptor accessors). These bytes are "canonical" from the device's own
 * point of view: dma_write_canonical/dma_read_canonical below apply the
 * Bridge's separate per-word byte-lane correction on top.
 */
static void stq_be_buf(uint8_t *b, uint64_t v)
{
    int i;

    for (i = 0; i < 8; i++) {
        b[i] = v >> (8 * (7 - i));
    }
}

static uint64_t ldq_be_buf(const uint8_t *b)
{
    uint64_t v = 0;
    int i;

    for (i = 0; i < 8; i++) {
        v = (v << 8) | b[i];
    }
    return v;
}

/*
 * Reverse each aligned 32-bit word: the correction the Bridge's byte-lane DMA
 * path needs for this non-exempt slot with its swap bits clear. A device that
 * DMA-reads a byte buffer in its own little-endian order sees the canonical
 * bytes back once the guest has laid them out word-reversed, and a byte buffer
 * the device DMA-writes reads back canonical once reversed again -- the same
 * transform in both directions (sn0-qlogic-isp-test.c's normalise_words). The
 * length is a multiple of four here, so every chunk is one aligned word.
 */
static void normalise_words(uint8_t *buf, size_t len)
{
    size_t i;

    for (i = 0; i + 4 <= len; i += 4) {
        uint8_t t = buf[i];

        buf[i] = buf[i + 3];
        buf[i + 3] = t;
        t = buf[i + 1];
        buf[i + 1] = buf[i + 2];
        buf[i + 2] = t;
    }
}

/*
 * Guest memory as the device sees it: write canonical bytes word-reversed so a
 * device DMA-read recovers them, and read a device DMA-write back word-reversed
 * to recover the canonical bytes it wrote.
 */
static void dma_write_canonical(QTestState *qts, uint64_t phys,
                                const uint8_t *buf, size_t len)
{
    uint8_t tmp[512];

    g_assert_cmpuint(len, <=, sizeof(tmp));
    memcpy(tmp, buf, len);
    normalise_words(tmp, len);
    qtest_memwrite(qts, phys, tmp, len);
}

static void dma_read_canonical(QTestState *qts, uint64_t phys, uint8_t *buf,
                               size_t len)
{
    qtest_memread(qts, phys, buf, len);
    normalise_words(buf, len);
}

/*
 * A 64-bit register access through the slot window at its natural offset.
 * DEV_64BIT_DEVICE serves the doubleword in one cycle and the swap bit is
 * clear, so the composition -- the device's little-endian delivery, the
 * Bridge's window correction, the guest's big-endian load -- is identity: no
 * byte-lane address swizzle applies to a single eight-byte cycle.
 */
static void win_writeq(QTestState *qts, uint64_t off, uint64_t val)
{
    qtest_writeq(qts, WINDOW_SLOT3 + off, val);
}

static uint64_t win_readq(QTestState *qts, uint64_t off)
{
    return qtest_readq(qts, WINDOW_SLOT3 + off);
}

/*
 * A 32-bit register access through the slot window -- the width IRIX xg reads
 * these 64-bit registers with. On a DEV_64BIT_DEVICE-clear slot this is one
 * 32-bit device cycle the Bridge byte-lane-corrects, reaching the register's
 * low half at off+0 and its high half at off+4.
 */
static uint32_t win_readl(QTestState *qts, uint64_t off)
{
    return qtest_readl(qts, WINDOW_SLOT3 + off);
}

/* Aim the slot's window at a BAR, marking the device 64-bit and swap-clear. */
static void aim_window(QTestState *qts, uint32_t pci_base)
{
    qtest_writel(qts, BRIDGE_DEVICE(XFRAME_SLOT),
                 DEV_64BIT_DEVICE | DEV_MEMORY_SPACE | (pci_base >> 20));
}

/*
 * Bring the card up: decode both BARs, enable memory (and bus mastering for
 * DMA), and aim the slot window at the BAR this test drives.
 */
static void bring_up(QTestState *qts, uint32_t window_pci_base, bool master)
{
    uint32_t vendor = qtest_readl(qts, CFG0(XFRAME_SLOT) + PCI_VENDOR_ID);

    g_assert_cmphex(vendor & 0xffff, ==, XFRAME_VENDOR);

    qtest_writel(qts, CFG0(XFRAME_SLOT) + PCI_BAR0, PIF_BAR_PCI);
    qtest_writel(qts, CFG0(XFRAME_SLOT) + PCI_BAR0_HIGH, 0);
    qtest_writel(qts, CFG0(XFRAME_SLOT) + PCI_BAR2, TXP_BAR_PCI);
    qtest_writel(qts, CFG0(XFRAME_SLOT) + PCI_COMMAND,
                 PCI_COMMAND_MEMORY | (master ? PCI_COMMAND_MASTER : 0));

    aim_window(qts, window_pci_base);
}

static QTestState *boot_plain(void)
{
    return qtest_initf("%s -device xframe,addr=%d,mac=%s",
                       MACHINE_WITH_BRIDGE, XFRAME_SLOT, TEST_MAC);
}

static QTestState *boot_with_socket(int *host_fd)
{
    QTestState *qts;
    int sv[2];

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    net_socket_set_recv_timeout(sv[0], 5);
    *host_fd = sv[0];

    qts = qtest_initf("%s -netdev socket,fd=%d,id=n0 "
                      "-device xframe,addr=%d,netdev=n0,mac=%s",
                      MACHINE_WITH_BRIDGE, sv[1], XFRAME_SLOT, TEST_MAC);
    return qts;
}

/*
 * Register composition: a big-endian guest 64-bit read of the byte-swapper
 * diagnostic returns the magic through the Bridge's register-window correction
 * composed with the device's little-endian delivery -- the single-cycle path
 * the 64-bit-device fix enabled. The swap bit is pinned clear (the tripwire for
 * a guest ever setting it) and the device-size bit pinned set (the fix keys the
 * one-cycle path on it). ADAPTER_STATUS's quiescent constant is checked too:
 * both halves of that value differ, so a hidden byte or word swap that the
 * magic alone might not expose returns a visibly wrong number.
 */
static void test_swapper_magic_composes_through_the_bridge(const void *data)
{
    QTestState *qts = boot_plain();
    uint32_t dev;

    bring_up(qts, PIF_BAR_PCI, false);

    dev = qtest_readl(qts, BRIDGE_DEVICE(XFRAME_SLOT));
    g_assert_cmphex(dev & DEV_SWAP_PROGRAMMED, ==, 0);
    g_assert_cmphex(dev & DEV_64BIT_DEVICE, ==, DEV_64BIT_DEVICE);

    g_assert_cmphex(win_readq(qts, PIF_RD_SWAPPER_FB), ==, SWAPPER_MAGIC);
    g_assert_cmphex(win_readq(qts, ADAPTER_STATUS), ==,
                    ADAPTER_STATUS_QUIESCENT);

    qtest_quit(qts);
}

/*
 * The real IRIX xg case: the slot's DEV register left with DEV_64BIT_DEVICE
 * CLEAR, the way the live IRIX boot leaves it -- unlike the test above, which
 * marks the slot 64-bit. With the bit clear the Bridge delivers a 32-bit
 * register access to the device as one 32-bit cycle rather than folding a
 * doubleword into an eight-byte cycle, so this is the register access xg's
 * endianness handshake actually issues on hardware: a 32-bit read, byte-lane
 * corrected by the Bridge, of each half of the swapper diagnostic. It reaches
 * the right bytes only because the device now serves 32-bit register cycles --
 * before its window's valid.min_access_size dropped to 4 the cycle was rejected
 * below the valid minimum and the read came back zero, and xg's endian check
 * aborted attach. Both halves of the swapper magic, and both halves of the
 * quiescent ADAPTER_STATUS (whose halves differ, so a half reaching the wrong
 * lane shows), must compose whole.
 *
 * A single 64-bit read on a DEV_64BIT_DEVICE-clear slot is a different path the
 * Bridge word-orders for a 32-bit-device slot; xg never issues one (it reads
 * 32-bit), so that composition -- a Bridge concern -- is not what this asserts.
 */
static void test_swapper_magic_composes_via_32bit_cycles(const void *data)
{
    QTestState *qts = boot_plain();
    uint32_t dev;

    /* Decode both BARs and enable memory, but leave DEV_64BIT_DEVICE clear. */
    qtest_writel(qts, CFG0(XFRAME_SLOT) + PCI_BAR0, PIF_BAR_PCI);
    qtest_writel(qts, CFG0(XFRAME_SLOT) + PCI_BAR0_HIGH, 0);
    qtest_writel(qts, CFG0(XFRAME_SLOT) + PCI_BAR2, TXP_BAR_PCI);
    qtest_writel(qts, CFG0(XFRAME_SLOT) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(XFRAME_SLOT),
                 DEV_MEMORY_SPACE | (PIF_BAR_PCI >> 20));

    dev = qtest_readl(qts, BRIDGE_DEVICE(XFRAME_SLOT));
    g_assert_cmphex(dev & DEV_64BIT_DEVICE, ==, 0);
    g_assert_cmphex(dev & DEV_SWAP_PROGRAMMED, ==, 0);

    g_assert_cmphex(win_readl(qts, PIF_RD_SWAPPER_FB + 0), ==,
                    (uint32_t)SWAPPER_MAGIC);
    g_assert_cmphex(win_readl(qts, PIF_RD_SWAPPER_FB + 4), ==,
                    (uint32_t)(SWAPPER_MAGIC >> 32));
    g_assert_cmphex(win_readl(qts, ADAPTER_STATUS + 0), ==,
                    (uint32_t)ADAPTER_STATUS_QUIESCENT);
    g_assert_cmphex(win_readl(qts, ADAPTER_STATUS + 4), ==,
                    (uint32_t)(ADAPTER_STATUS_QUIESCENT >> 32));

    qtest_quit(qts);
}

/*
 * Transmit composition, the load-bearing DMA assertion the PC machine cannot
 * see. The guest lays a single-segment TxD list and its frame buffer in memory
 * the way the machine's DMA byte order requires (canonical bytes word-reversed
 * through dma_write_canonical), rings the doorbell through the window, and the
 * exact frame must appear on the socket backend -- proving the device DMA-read
 * the descriptor's fields (the buffer pointer among them) and the frame buffer
 * through the Bridge and reassembled the driver's bytes. The card then hands
 * the descriptor back with its ownership bit cleared; reading that
 * device-written doubleword back word-reversed must recover the control word,
 * proving the device->guest DMA write composes too.
 *
 * If a 64-bit descriptor field came back with its two 32-bit halves reversed,
 * that would be a device-side field-ordering issue, not a Bridge fault -- but
 * the Bridge's byte-lane path preserves each word's position and reverses only
 * the bytes within it, so a field the guest wrote word-reversed is recovered
 * whole, halves in place. This asserts that whole-field recovery.
 */
static void test_transmit_frame_composes_through_the_bridge(const void *data)
{
    int host_fd;
    QTestState *qts = boot_with_socket(&host_fd);
    uint8_t frame[FRAME_LEN];
    uint8_t got[128];
    uint8_t desc[32];
    uint8_t handback[8];
    uint64_t control1;
    size_t len;
    unsigned i;

    bring_up(qts, TXP_BAR_PCI, true);
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE, 0));

    /* Distinct bytes so a byte or word swap anywhere shows as a wrong frame. */
    for (i = 0; i < FRAME_LEN; i++) {
        frame[i] = (uint8_t)(0x80 + i);
    }

    dma_write_canonical(qts, TX_BUF_PHYS, frame, sizeof(frame));

    /* One card-owned first-and-last segment; length in control1 bits 15:0. */
    memset(desc, 0, sizeof(desc));
    stq_be_buf(desc + 0, (uint64_t)FRAME_LEN | TXD_OWN | TXD_GCF | TXD_GCL);
    stq_be_buf(desc + 16, TX_BUF_IOVA);
    dma_write_canonical(qts, DESC_PHYS, desc, sizeof(desc));

    /* Ring the doorbell for a one-descriptor list at the descriptor's IOVA. */
    win_writeq(qts, TXP_TXDL_PAR, DESC_IOVA);
    win_writeq(qts, TXP_TXDL_LCR,
               TXDL_NUMTXD(0) | TXDL_LGC_FIRST | TXDL_LGC_LAST);

    len = net_socket_recv_frame(host_fd, got, sizeof(got));
    g_assert_cmpint(len, ==, sizeof(frame));
    g_assert_cmpmem(got, len, frame, sizeof(frame));

    /* The card released the descriptor: ownership handed back, length kept. */
    dma_read_canonical(qts, DESC_PHYS, handback, sizeof(handback));
    control1 = ldq_be_buf(handback);
    g_assert_cmphex(control1 & TXD_OWN, ==, 0);
    g_assert_cmphex(control1 & 0xffff, ==, FRAME_LEN);

    qtest_quit(qts);
    close(host_fd);
}

static bool wait_rx_traffic(QTestState *qts, int attempts)
{
    int i;

    for (i = 0; i < attempts; i++) {
        if (win_readq(qts, RX_TRAFFIC_INT) & RX_TRAFFIC_RING0) {
            return true;
        }
        g_usleep(1000);
    }
    return false;
}

/*
 * Receive composition: the device's native-little-endian byte-buffer DMA
 * writing a whole frame into the guest, through the Bridge's byte-lane path.
 * The guest posts one card-owned RxD (laid out canonical-word-reversed so the
 * device DMA-reads its buffer pointer and capacity correctly), arms ring 0, and
 * injects a frame addressed to the station MAC. The frame the device deposits
 * must read back -- word-reversed -- as the exact bytes injected, and the
 * completion it writes (length in control2, status and cleared ownership in
 * control1) must compose the same way. This is the frame-data DMA direction the
 * transmit path does not itself pin.
 */
static void test_receive_frame_composes_through_the_bridge(const void *data)
{
    int host_fd;
    QTestState *qts = boot_with_socket(&host_fd);
    uint8_t frame[FRAME_LEN];
    uint8_t landed[FRAME_LEN];
    uint8_t rxd[32];
    uint8_t word[8];
    uint64_t control1, control2;
    unsigned i;

    bring_up(qts, PIF_BAR_PCI, true);
    qtest_writeq(qts, ATE(0), ATE_ENTRY(TARGET_PAGE, 0));

    for (i = 0; i < FRAME_LEN; i++) {
        frame[i] = (uint8_t)(0x80 + i);
    }
    memcpy(frame, station_mac, sizeof(station_mac));

    /* host control 0, card-owned, capacity in control2 bits 63:48, buf ptr. */
    memset(rxd, 0, sizeof(rxd));
    stq_be_buf(rxd + 8, RXD_OWN);
    stq_be_buf(rxd + 16, (uint64_t)RX_BUF_CAPACITY << RXD_BUFSIZ_SHIFT);
    stq_be_buf(rxd + 24, RX_BUF_IOVA);
    dma_write_canonical(qts, DESC_PHYS, rxd, sizeof(rxd));

    win_writeq(qts, PRC_RXD0_0, DESC_IOVA);
    win_writeq(qts, PRC_CTRL_0, RC_IN_SVC);

    net_socket_send_frame(host_fd, frame, sizeof(frame));
    g_assert_true(wait_rx_traffic(qts, 5000));

    /* The frame the device DMA-wrote, recovered word-reversed. */
    dma_read_canonical(qts, RX_BUF_PHYS, landed, sizeof(landed));
    g_assert_cmpmem(landed, sizeof(landed), frame, sizeof(frame));

    /* control1: ownership handed back, status code OK. */
    dma_read_canonical(qts, DESC_PHYS + 8, word, sizeof(word));
    control1 = ldq_be_buf(word);
    g_assert_cmphex(control1 & RXD_OWN, ==, 0);
    g_assert_cmphex((control1 >> RXD_XCODE_SHIFT) & RXD_XCODE_MASK, ==, 0);

    /* control2: the received length overwrote the posted capacity. */
    dma_read_canonical(qts, DESC_PHYS + 16, word, sizeof(word));
    control2 = ldq_be_buf(word);
    g_assert_cmphex(control2 >> RXD_BUFSIZ_SHIFT, ==, FRAME_LEN);

    qtest_quit(qts);
    close(host_fd);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_device("xframe")) {
        return 0;
    }

    qtest_add_data_func("sn0/xframe/swapper-magic-composes-through-the-bridge",
                        NULL, test_swapper_magic_composes_through_the_bridge);
    qtest_add_data_func("sn0/xframe/swapper-magic-composes-via-32bit-cycles",
                        NULL, test_swapper_magic_composes_via_32bit_cycles);
    qtest_add_data_func("sn0/xframe/transmit-frame-composes-through-the-bridge",
                        NULL, test_transmit_frame_composes_through_the_bridge);
    qtest_add_data_func("sn0/xframe/receive-frame-composes-through-the-bridge",
                        NULL, test_receive_frame_composes_through_the_bridge);

    return g_test_run();
}
