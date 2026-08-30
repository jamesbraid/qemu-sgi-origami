/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller: the ethernet MAC engine.
 *
 * Two descriptor rings and the register block that drives them: transmit
 * descriptors carried inline in the ring, and a receive ring of buffer
 * addresses the guest supplies. Written from the IOC3 datasheet in the
 * origami project tree (docs/datasheets/ioc3.md there, not here).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/net/sgi-ioc3-eth.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "net/checksum.h"
#include "system/dma.h"
#include "trace.h"

static uint32_t sgi_ioc3_enet_get(SGIIOC3EthState *s, hwaddr off)
{
    return s->enet_reg[(off - SGI_IOC3_ENET_BASE) / 4];
}

static void sgi_ioc3_enet_set(SGIIOC3EthState *s, hwaddr off, uint32_t val)
{
    s->enet_reg[(off - SGI_IOC3_ENET_BASE) / 4] = val;
}

/*
 * The buffer RAM is the parent chip's, but the parity-enable bit is this
 * block's, so the parent asks rather than reach into a register it does
 * not own.
 */
bool sgi_ioc3_eth_ram_parity_enabled(SGIIOC3EthState *s)
{
    return sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL) &
           SGI_IOC3_ENET_MCR_RAM_PARITY;
}

/* Reading the collision and deferral counters clears them. */
static uint32_t sgi_ioc3_take_counters(SGIIOC3EthState *s)
{
    unsigned int index = (SGI_IOC3_ENET_COUNTERS - SGI_IOC3_ENET_BASE) / 4;
    uint32_t counters = s->enet_reg[index];

    s->enet_reg[index] = 0;
    return counters;
}

/* Level-driven from status & enable, out through the parent's GPIO. */
static void sgi_ioc3_enet_update_irq(SGIIOC3EthState *s)
{
    uint32_t status = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_INT_STATUS);
    uint32_t enable = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_INT_ENABLE);

    trace_sgi_ioc3_eth_irq(status, enable, (status & enable) != 0);
    qemu_set_irq(s->irq, (status & enable) != 0);
}

/*
 * Clear the block. The MAC control register's reset bit resets the engine
 * but not the ring bases: SGI's power-on loopback diagnostic programs the
 * bases, asserts the bit, then starts the engine without rewriting them.
 * The device's own reset clears everything. What the engine reset does to
 * the other registers is unobserved; clearing them is the kept default.
 */
static void sgi_ioc3_eth_reset_registers(SGIIOC3EthState *s, bool keep_rings)
{
    uint32_t rx_hi = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_RING_HI);
    uint32_t rx_lo = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_RING_LO);
    uint32_t tx_hi = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_TX_RING_HI);
    uint32_t tx_lo = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_TX_RING_LO);

    memset(s->enet_reg, 0, sizeof(s->enet_reg));
    if (keep_rings) {
        sgi_ioc3_enet_set(s, SGI_IOC3_ENET_RX_RING_HI, rx_hi);
        sgi_ioc3_enet_set(s, SGI_IOC3_ENET_RX_RING_LO, rx_lo);
        sgi_ioc3_enet_set(s, SGI_IOC3_ENET_TX_RING_HI, tx_hi);
        sgi_ioc3_enet_set(s, SGI_IOC3_ENET_TX_RING_LO, tx_lo);
    }
    s->mii_data = SGI_IOC3_ENET_MII_ABSENT;
    s->mii_data_w = 0;
    s->rx_timer_armed = false;
    sgi_ioc3_enet_update_irq(s);
}

/*
 * Two independent loopback switches -- the controller's bit and the
 * transceiver's -- and either disconnects the block from the backend.
 */
static bool sgi_ioc3_eth_mac_loopback(SGIIOC3EthState *s)
{
    return sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL) &
           SGI_IOC3_ENET_MCR_LOOPBACK;
}

static bool sgi_ioc3_eth_looping_back(SGIIOC3EthState *s)
{
    return sgi_ioc3_eth_mac_loopback(s) || dp83840_in_loopback(&s->phy);
}

/*
 * Enables set and a buffer available. The indices are narrowed to their
 * twelve bits: the arm-timer bit shares the produce register without
 * being part of the index.
 */
static bool sgi_ioc3_eth_can_receive(NetClientState *nc)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(qemu_get_nic_opaque(nc));
    uint32_t mcr = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL);
    uint32_t need = SGI_IOC3_ENET_MCR_RX_DMA_EN | SGI_IOC3_ENET_MCR_RX_EN;

    if ((mcr & need) != need) {
        return false;
    }

    /*
     * Refusing wire traffic in loopback keeps the backend out of a
     * diagnostic's ring, where an outside frame shifts every comparison.
     */
    if (sgi_ioc3_eth_looping_back(s)) {
        return false;
    }

    return (sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_PRODUCE) &
           SGI_IOC3_ENET_RX_INDEX_MASK) !=
          (sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_CONSUME) &
           SGI_IOC3_ENET_RX_INDEX_MASK);
}

static ssize_t sgi_ioc3_eth_deliver(SGIIOC3EthState *s, const uint8_t *buf,
                                    size_t size, bool looped);

/*
 * The multicast hash bit an address selects, matching Linux's ioc3_hash():
 * the low six bits of the reflected Ethernet CRC, themselves bit-reversed.
 */
static unsigned int sgi_ioc3_eth_mcast_hash(const uint8_t *addr)
{
    uint32_t crc = net_crc32_le(addr, 6) & 0x3f;
    unsigned int index = 0;
    int bit;

    for (bit = 6; --bit >= 0; ) {
        index <<= 1;
        index |= crc & 0x1;
        crc >>= 1;
    }
    return index;
}

/*
 * Destination filtering, against the guest-programmed station address --
 * never the backend NIC's, which has no relation to what the guest's
 * driver believes its address is.
 */
static bool sgi_ioc3_eth_rx_filter(SGIIOC3EthState *s, const uint8_t *buf,
                                   size_t size)
{
    static const uint8_t broadcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint32_t mcr = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL);
    uint32_t addr_hi, addr_lo, hash_hi, hash_lo;
    uint8_t station[6];
    unsigned int index;

    if (mcr & SGI_IOC3_ENET_MCR_PROMISC) {
        return true;
    }

    if (size < 6) {
        return false;
    }

    /*
     * The low register carries the first four octets, byte 0 in its low
     * byte; the high register's low half carries the last two (Linux's
     * __ioc3_set_mac_address()).
     */
    addr_hi = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_ADDR_HI);
    addr_lo = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_ADDR_LO);
    station[0] = addr_lo;
    station[1] = addr_lo >> 8;
    station[2] = addr_lo >> 16;
    station[3] = addr_lo >> 24;
    station[4] = addr_hi;
    station[5] = addr_hi >> 8;
    if (memcmp(buf, station, 6) == 0) {
        return true;
    }

    if (memcmp(buf, broadcast, 6) == 0) {
        return true;
    }

    if (!(buf[0] & 1)) {
        /* Unicast, and it matched neither the station address nor all-ones. */
        return false;
    }

    hash_hi = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_HASH_HI);
    hash_lo = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_HASH_LO);
    index = sgi_ioc3_eth_mcast_hash(buf);
    return ((index < 32 ? hash_lo : hash_hi) >> (index & 31)) & 1;
}

static ssize_t sgi_ioc3_eth_receive(NetClientState *nc, const uint8_t *buf,
                                    size_t size)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(qemu_get_nic_opaque(nc));

    if (!sgi_ioc3_eth_can_receive(nc)) {
        trace_sgi_ioc3_eth_rx_refused(
            sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL),
            sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_PRODUCE),
            sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_CONSUME));
        return 0;
    }

    if (!sgi_ioc3_eth_rx_filter(s, buf, size)) {
        /*
         * Consumed, not 0: a 0 return asks the net queue to redeliver a
         * frame no retry can ever pass, wedging the queue head.
         */
        trace_sgi_ioc3_eth_rx_filtered((uint32_t)size);
        return size;
    }

    return sgi_ioc3_eth_deliver(s, buf, size, false);
}

/*
 * Which receive sources a delivered frame raises: the armed timer, and the
 * available-buffer watermark. The only reload either driver writes is
 * zero; a non-zero one would need a real timer, so it is reported instead.
 */
static uint32_t sgi_ioc3_eth_rx_sources(SGIIOC3EthState *s, uint32_t consume)
{
    uint32_t reload = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_TIMER) &
                      SGI_IOC3_ENET_RTR_RELOAD_MASK;
    uint32_t threshold = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_CONTROL) &
                         SGI_IOC3_ENET_RCSR_THRESH_MASK;
    uint32_t produce = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_PRODUCE) &
                       SGI_IOC3_ENET_RX_INDEX_MASK;
    uint32_t available = ((produce - consume) & SGI_IOC3_ENET_RX_INDEX_MASK) >>
                         SGI_IOC3_ENET_RX_RING_SHIFT;
    uint32_t sources = 0;

    if (s->rx_timer_armed) {
        if (reload == 0) {
            sources |= SGI_IOC3_ENET_INT_RX_TIMER;
        } else {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-ioc3-eth: receive timer reload %u not modelled;"
                          " only an immediate timeout is\n", reload);
        }
    }
    /*
     * At-or-below: OpenBSD writes its whole available count as the
     * threshold and expects the interrupt when the count reaches it --
     * strictly-below would be one frame late.
     */
    if (available <= threshold) {
        sources |= SGI_IOC3_ENET_INT_RX_THRESHOLD;
    }
    return sources;
}

/*
 * Put one frame into the receive ring. `short_circuit` marks a frame the
 * controller's own loopback turned round ahead of the FCS append and the
 * status vector, so neither is present; the transceiver's loopback goes
 * the whole way through and gets both. Split from the receive callback so
 * loopback frames bypass its refusal of wire traffic.
 */
static ssize_t sgi_ioc3_eth_deliver(SGIIOC3EthState *s, const uint8_t *buf,
                                    size_t size, bool short_circuit)
{
    uint32_t consume, offset, mcr, word0, word1, lenlo, lenhi, wire_len;
    uint64_t ring_base, buf_addr;
    bool truncated = false;
    MemTxResult res;

    if (size > SGI_IOC3_ENET_FRAME_MAX - 8) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-eth: %zu-byte frame truncated to the %d bytes"
                      " a receive delivery can carry\n",
                      size, SGI_IOC3_ENET_FRAME_MAX - 8);
        size = SGI_IOC3_ENET_FRAME_MAX - 8;
        truncated = true;
    }

    consume = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_CONSUME) &
             SGI_IOC3_ENET_RX_INDEX_MASK;
    ring_base = ((uint64_t)sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_RING_HI)
                << 32) |
               (sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_RING_LO) &
                SGI_IOC3_ENET_RX_BASE_MASK);

    /*
     * A ring entry the fabric refuses leaves the delivery with no
     * destination; the frame is dropped. The block has no error status for
     * a refused transfer, so the log is where it is reported.
     */
    if (ldq_be_dma(&s->dma_as, ring_base + consume, &buf_addr,
                   MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-eth: receive ring entry at 0x%" PRIx64
                      " cannot be read; the frame is dropped\n",
                      ring_base + consume);
        return size;
    }

    mcr = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL);
    offset = ((mcr & SGI_IOC3_ENET_MCR_RXOFF_MASK) >>
             SGI_IOC3_ENET_MCR_RXOFF_SHIFT) * 2;

    /*
     * Both guests read the length as the wire length including the four
     * FCS bytes the backend never delivers, so they are added back -- or a
     * minimum-length frame reads four short and the guest drops it. A
     * short-circuited frame never had an FCS appended, so nothing is added.
     */
    wire_len = (uint32_t)size + (short_circuit ? 0 : 4);

    word0 = SGI_IOC3_ENET_RXH_VALID |
           ((wire_len << SGI_IOC3_ENET_RXH_BYTECNT_SHIFT) &
            SGI_IOC3_ENET_RXH_BYTECNT_MASK);

    /*
     * The receive checksum-offload partial: the one's-complement sum of the
     * whole packet, uncomplemented. Linux's ioc3_tcpudp_checksum() finishes
     * the transport checksum from it and trusts the packet only when the
     * reconstruction folds to 0xffff, so it must be exact. The phantom FCS
     * bytes are written and summed as zero, which is self-consistent.
     */
    if (!short_circuit) {
        uint32_t sum = net_checksum_add(size, (uint8_t *)buf);

        word0 |= 0xffffu & ~net_checksum_finish(sum);
    }

    lenlo = wire_len & 0x7u;
    lenhi = (wire_len >> 3) & 0x3ffu;
    word1 = SGI_IOC3_ENET_RXE_GOODPKT |
           (lenlo << SGI_IOC3_ENET_RXE_LEN_LO_SHIFT) |
           (lenhi << SGI_IOC3_ENET_RXE_LEN_HI_SHIFT);
    if (truncated) {
        /* A cut frame says so where a driver looks; OpenBSD drops it. */
        word1 |= SGI_IOC3_ENET_RXE_LONGEVENT | SGI_IOC3_ENET_RXE_BADPKT;
    }
    if (size >= 6 && buf[0] == 0xff && buf[1] == 0xff && buf[2] == 0xff &&
        buf[3] == 0xff && buf[4] == 0xff && buf[5] == 0xff) {
        word1 |= SGI_IOC3_ENET_RXE_BROADCAST;
    } else if (size >= 1 && (buf[0] & 1)) {
        word1 |= SGI_IOC3_ENET_RXE_MULTICAST;
    }

    trace_sgi_ioc3_eth_rx((uint32_t)size, mcr,
                          sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_PRODUCE),
                          sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_CONSUME),
                          ring_base, buf_addr, offset, wire_len, word0, word1);

    res = stl_be_dma(&s->dma_as, buf_addr + 0, word0, MEMTXATTRS_UNSPECIFIED);
    res |= stl_be_dma(&s->dma_as, buf_addr + 4, short_circuit ? 0 : word1,
                      MEMTXATTRS_UNSPECIFIED);
    res |= dma_memory_write(&s->dma_as, buf_addr + offset, buf, size,
                            MEMTXATTRS_UNSPECIFIED);
    if (!short_circuit) {
        /*
         * The phantom FCS bytes, written as the defined zero they were
         * summed as.
         */
        res |= stl_be_dma(&s->dma_as, buf_addr + offset + size, 0,
                          MEMTXATTRS_UNSPECIFIED);
    }

    /*
     * A buffer the transfer could not reach is not a delivered frame: the
     * index stays put and no interrupt announces it.
     */
    if (res != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-eth: receive buffer at 0x%" PRIx64
                      " cannot be written; the frame is dropped\n", buf_addr);
        return size;
    }

    consume = (consume + (1u << SGI_IOC3_ENET_RX_RING_SHIFT)) &
             SGI_IOC3_ENET_RX_INDEX_MASK;
    sgi_ioc3_enet_set(s, SGI_IOC3_ENET_RX_CONSUME, consume);

    sgi_ioc3_enet_set(s, SGI_IOC3_ENET_INT_STATUS,
                      sgi_ioc3_enet_get(s, SGI_IOC3_ENET_INT_STATUS) |
                      sgi_ioc3_eth_rx_sources(s, consume));
    sgi_ioc3_enet_update_irq(s);

    return size;
}

/* The link bit a guest watches is the transceiver's; relay to it. */
static void sgi_ioc3_eth_set_link(NetClientState *nc)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(qemu_get_nic_opaque(nc));

    dp83840_update_link(&s->phy, nc->link_down);
}

static NetClientInfo sgi_ioc3_eth_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = sgi_ioc3_eth_can_receive,
    .receive = sgi_ioc3_eth_receive,
    .link_status_changed = sgi_ioc3_eth_set_link,
};

/*
 * Gather one frame: inline data, then up to two out-of-line buffers, the
 * order the validity flags list. Capped at the frame buffer's size so a
 * guest's miscount cannot overrun it.
 */
static MemTxResult sgi_ioc3_eth_tx_gather(SGIIOC3EthState *s, uint32_t cmd,
                                          uint32_t bufcnt, uint64_t buf1,
                                          uint64_t buf2,
                                          const uint8_t *inline_data,
                                          size_t *frame_len)
{
    MemTxResult res = MEMTX_OK;
    size_t len = 0;
    unsigned int n;

    if (cmd & SGI_IOC3_ENET_TXD_DATA_VALID) {
        n = bufcnt & SGI_IOC3_ENET_TXD_DATA_LEN_MASK;
        /*
         * The seven-bit length can name more inline data than the 104-byte
         * area holds; without this the copy reads past the descriptor image.
         */
        if (n > SGI_IOC3_ENET_TXD_DATA_SIZE) {
            n = SGI_IOC3_ENET_TXD_DATA_SIZE;
        }
        if (n > sizeof(s->frame) - len) {
            n = sizeof(s->frame) - len;
        }
        memcpy(s->frame + len, inline_data, n);
        len += n;
    }
    if (cmd & SGI_IOC3_ENET_TXD_BUF1_VALID) {
        n = (bufcnt & SGI_IOC3_ENET_TXD_BUF1_LEN_MASK) >>
           SGI_IOC3_ENET_TXD_BUF1_LEN_SHIFT;
        if (n > sizeof(s->frame) - len) {
            n = sizeof(s->frame) - len;
        }
        res |= dma_memory_read(&s->dma_as, buf1, s->frame + len, n,
                               MEMTXATTRS_UNSPECIFIED);
        len += n;
    }
    if (cmd & SGI_IOC3_ENET_TXD_BUF2_VALID) {
        n = (bufcnt & SGI_IOC3_ENET_TXD_BUF2_LEN_MASK) >>
           SGI_IOC3_ENET_TXD_BUF2_LEN_SHIFT;
        if (n > sizeof(s->frame) - len) {
            n = sizeof(s->frame) - len;
        }
        res |= dma_memory_read(&s->dma_as, buf2, s->frame + len, n,
                               MEMTXATTRS_UNSPECIFIED);
        len += n;
    }

    *frame_len = len;
    return res;
}

/* Both enables: DMA to fetch descriptors, transmit to send. */
static bool sgi_ioc3_eth_tx_enabled(SGIIOC3EthState *s)
{
    return (sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL) &
            (SGI_IOC3_ENET_MCR_TX_DMA_EN | SGI_IOC3_ENET_MCR_TX_EN)) ==
           (SGI_IOC3_ENET_MCR_TX_DMA_EN | SGI_IOC3_ENET_MCR_TX_EN);
}

/*
 * Walk the transmit ring from the consume index to the produce index the
 * guest wrote, one frame per descriptor. The consume word carries an idle
 * flag beside the byte offset, set once the ring is drained.
 */
static void sgi_ioc3_eth_do_tx(SGIIOC3EthState *s, uint32_t produce)
{
    uint32_t tx_lo = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_TX_RING_LO);
    unsigned int ring_entries = (tx_lo & SGI_IOC3_ENET_TX_RING_SEL) ?
                                SGI_IOC3_ENET_TX_RING_LARGE :
                                SGI_IOC3_ENET_TX_RING_SMALL;
    uint32_t ring_bytes = ring_entries * SGI_IOC3_ENET_TX_DESC_SIZE;
    uint64_t tx_base = ((uint64_t)sgi_ioc3_enet_get(s, SGI_IOC3_ENET_TX_RING_HI)
                       << 32) | (tx_lo & SGI_IOC3_ENET_TX_BASE_MASK);
    uint32_t consume = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_TX_CONSUME) &
                       SGI_IOC3_ENET_TX_CONSUME_MASK;
    /*
     * Raised sources accumulate apart from the status register: sending
     * re-enters this device (a backend can answer from inside the send),
     * and writing back a copy taken before the walk would erase the
     * receive source that reply raised.
     */
    bool loopback = sgi_ioc3_eth_looping_back(s);
    uint32_t raised = 0;
    bool completed_one = false;

    /*
     * The walk steps by whole descriptors and stops on equality, so an
     * unaligned or out-of-ring index the steps can never land on is an
     * emulator hang from one register write. Reduce and align both.
     */
    if ((produce | consume) & (SGI_IOC3_ENET_TX_DESC_SIZE - 1)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-eth: transmit index not on a descriptor"
                      " boundary (produce 0x%04x consume 0x%04x)\n",
                      produce, consume);
    }
    produce = QEMU_ALIGN_DOWN(produce % ring_bytes,
                              SGI_IOC3_ENET_TX_DESC_SIZE);
    consume = QEMU_ALIGN_DOWN(consume % ring_bytes,
                              SGI_IOC3_ENET_TX_DESC_SIZE);

    while (consume != produce) {
        uint64_t desc = tx_base + consume;
        uint32_t cmd, bufcnt;
        uint64_t buf1, buf2;
        uint8_t inline_data[SGI_IOC3_ENET_TXD_DATA_SIZE];
        MemTxResult res;
        size_t len;

        res = ldl_be_dma(&s->dma_as, desc + 0, &cmd, MEMTXATTRS_UNSPECIFIED);
        res |= ldl_be_dma(&s->dma_as, desc + 4, &bufcnt,
                          MEMTXATTRS_UNSPECIFIED);
        res |= ldq_be_dma(&s->dma_as, desc + 8, &buf1, MEMTXATTRS_UNSPECIFIED);
        res |= ldq_be_dma(&s->dma_as, desc + 16, &buf2,
                          MEMTXATTRS_UNSPECIFIED);
        res |= dma_memory_read(&s->dma_as,
                               desc + SGI_IOC3_ENET_TXD_DATA_OFFSET,
                               inline_data, sizeof(inline_data),
                               MEMTXATTRS_UNSPECIFIED);

        /*
         * A descriptor the fabric refused must not become a frame on the
         * wire. The walk stops with the index on it, so a guest that
         * repairs the address and rings again gets it sent.
         */
        if (res != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc3-eth: transmit descriptor at 0x%" PRIx64
                          " cannot be read; the walk stops on it\n", desc);
            break;
        }

        trace_sgi_ioc3_eth_tx_desc(consume, cmd, bufcnt, buf1, buf2);

        res = sgi_ioc3_eth_tx_gather(s, cmd, bufcnt, buf1, buf2, inline_data,
                                     &len);
        if (res != MEMTX_OK) {
            /* The same for the frame's own bytes. */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc3-eth: the frame for the transmit descriptor"
                          " at 0x%" PRIx64 " cannot be read; it is not sent\n",
                          desc);
            break;
        }
        len = MIN(len, cmd & SGI_IOC3_ENET_TXD_BYTECNT_MASK);

        /*
         * Transmit checksum offload: only the transport sum is inserted --
         * the IP header checksum is the driver's own. Ignoring the bit
         * ships the driver's partial and the far end drops the frame.
         */
        if (cmd & SGI_IOC3_ENET_TXD_DOCHECKSUM) {
            net_checksum_calculate(s->frame, len, CSUM_TCP | CSUM_UDP);
        }

        /*
         * Pad to the 802.3 minimum when the enable bit asks. IRIX -- kernel
         * and netboot PROM both -- relies on the engine for it; applied
         * before both paths, since the engine pads wherever the frame goes.
         */
        if ((sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL) &
             SGI_IOC3_ENET_MCR_PAD_EN) && len < 60) {
            memset(s->frame + len, 0, 60 - len);
            len = 60;
        }

        trace_sgi_ioc3_eth_tx((uint32_t)len, loopback);

        if (len > 0) {
            if (loopback) {
                /* Straight into the receive path, never to the backend. */
                sgi_ioc3_eth_deliver(s, s->frame, len,
                                     sgi_ioc3_eth_mac_loopback(s));
            } else {
                qemu_send_packet(qemu_get_queue(s->nic), s->frame, len);
            }
        }

        if (cmd & SGI_IOC3_ENET_TXD_INT_DONE) {
            raised |= SGI_IOC3_ENET_INT_TX_COMPLETE;
        }

        consume = (consume + SGI_IOC3_ENET_TX_DESC_SIZE) % ring_bytes;
        completed_one = true;
    }

    if (completed_one) {
        raised |= SGI_IOC3_ENET_INT_TX_EMPTY;
    }

    /*
     * The idle flag is derived from where the index ended, not set because
     * the loop returned: a walk that stopped on a refused descriptor is
     * not a drained ring.
     */
    sgi_ioc3_enet_set(s, SGI_IOC3_ENET_TX_CONSUME,
                      consume | (consume == produce ?
                                 SGI_IOC3_ENET_TX_CONSUME_IDLE : 0));
    sgi_ioc3_enet_set(s, SGI_IOC3_ENET_INT_STATUS,
                      sgi_ioc3_enet_get(s, SGI_IOC3_ENET_INT_STATUS) | raised);
    sgi_ioc3_enet_update_irq(s);
}

static uint64_t sgi_ioc3_enet_read_impl(void *opaque, hwaddr addr,
                                        unsigned size)
{
    SGIIOC3EthState *s = opaque;
    unsigned int index = addr / 4;

    switch (SGI_IOC3_ENET_BASE + addr) {
    case SGI_IOC3_ENET_COUNTERS:
        /* Cleared by the act of reading; both guests read once to zero. */
        return sgi_ioc3_take_counters(s);

    case SGI_IOC3_ENET_BUFFER_INDEX:
        /* The engines' positions in buffer memory; nothing is moving. */
        return 0;

    case SGI_IOC3_ENET_MII_CONTROL:
        /* Never busy: the request finished before the guest could look. */
        return s->enet_reg[index] & ~SGI_IOC3_ENET_MII_BUSY;

    case SGI_IOC3_ENET_MII_DATA_R:
        /* The word the last request fetched; who answered was decided then. */
        return s->mii_data;

    default:
        return s->enet_reg[index];
    }
}

/* One trace point covering every return, the derived ones included. */
static uint64_t sgi_ioc3_enet_read(void *opaque, hwaddr addr, unsigned size)
{
    uint64_t val = sgi_ioc3_enet_read_impl(opaque, addr, size);

    trace_sgi_ioc3_eth_reg_read((uint32_t)(SGI_IOC3_ENET_BASE + addr),
                                (uint32_t)val);
    return val;
}

static void sgi_ioc3_enet_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    SGIIOC3EthState *s = opaque;
    bool was_enabled;
    unsigned int index = addr / 4;

    trace_sgi_ioc3_eth_reg_write((uint32_t)(SGI_IOC3_ENET_BASE + addr),
                                 (uint32_t)val);

    switch (SGI_IOC3_ENET_BASE + addr) {
    case SGI_IOC3_ENET_INT_STATUS:
        /*
         * Write-one-to-clear, the parent's SuperIO contract. The line must
         * be re-derived here too: it is level-triggered from status &
         * enable, and an acknowledge that cleared the bits without lowering
         * the line leaves the next assertion unexplainable to the guest.
         */
        s->enet_reg[index] &= ~(uint32_t)val;
        sgi_ioc3_enet_update_irq(s);
        return;

    case SGI_IOC3_ENET_INT_ENABLE:
        /*
         * The enable decides the level line as much as status does: a
         * source that set while its enable was clear must raise the line
         * when the guest enables it, a window OpenBSD's bring-up order
         * reaches on every boot.
         */
        s->enet_reg[index] = val;
        sgi_ioc3_enet_update_irq(s);
        return;

    case SGI_IOC3_ENET_MII_CONTROL: {
        unsigned int phy = (val >> SGI_IOC3_ENET_MII_PHY_SHIFT) &
                           SGI_IOC3_ENET_MII_PHY_MASK;
        unsigned int reg = val & SGI_IOC3_ENET_MII_REG_MASK;

        /*
         * A management request; the write-side data register is already
         * loaded. Only the configured address answers -- the rest read
         * all-ones, or a scan finds a phantom transceiver at address zero.
         */
        if (val & SGI_IOC3_ENET_MII_READ) {
            s->mii_data = phy == s->phy.addr
                          ? dp83840_read(&s->phy, reg)
                          : SGI_IOC3_ENET_MII_ABSENT;
        } else if (phy == s->phy.addr) {
            bool was_looping = sgi_ioc3_eth_looping_back(s);

            dp83840_write(&s->phy, reg, s->mii_data_w);

            /*
             * Clearing the transceiver's loopback re-opens the receive
             * path; a queued frame waits for this flush.
             */
            if (was_looping && !sgi_ioc3_eth_looping_back(s)) {
                qemu_flush_queued_packets(qemu_get_queue(s->nic));
            }
        }

        s->enet_reg[index] = val;
        return;
    }

    case SGI_IOC3_ENET_MII_DATA_W:
        /* Loaded before the request that sends it. */
        s->mii_data_w = val;
        s->enet_reg[index] = val;
        return;

    case SGI_IOC3_ENET_TX_PRODUCE:
        /*
         * The doorbell. A doorbell rung while transmit is off is handled
         * by the MAC control write that later switches it on.
         */
        s->enet_reg[index] = val;
        if (sgi_ioc3_eth_tx_enabled(s)) {
            sgi_ioc3_eth_do_tx(s, val);
        }
        return;

    case SGI_IOC3_ENET_MAC_CONTROL:
        if (val & SGI_IOC3_ENET_MCR_RESET) {
            sgi_ioc3_eth_reset_registers(s, true);
            s->enet_reg[index] = SGI_IOC3_ENET_MCR_RESET |
                                 SGI_IOC3_ENET_MCR_ARB_DIAG_IDLE;
            return;
        }
        was_enabled = sgi_ioc3_eth_tx_enabled(s);
        s->enet_reg[index] = val;
        qemu_flush_queued_packets(qemu_get_queue(s->nic));

        /*
         * Enabling transmit starts the engine on whatever the produce index
         * already names -- SGI's loopback diagnostic queues first and
         * enables after. Only on the transition, or a later write to this
         * register would send the same descriptors twice.
         */
        if (!was_enabled && sgi_ioc3_eth_tx_enabled(s)) {
            sgi_ioc3_eth_do_tx(s, sgi_ioc3_enet_get(s,
                                                    SGI_IOC3_ENET_TX_PRODUCE));
        }
        return;

    case SGI_IOC3_ENET_RX_PRODUCE:
        /*
         * Bit 31 arms the timer and is a command, not storage: SGI's driver
         * writes the index bare from its interrupt path, which must not
         * disarm.
         */
        if (val & SGI_IOC3_ENET_RPIR_ARM) {
            s->rx_timer_armed = true;
        }
        /* This write can be what makes a buffer available; flush. */
        s->enet_reg[index] = val;
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        return;

    case SGI_IOC3_ENET_COUNTERS:
    case SGI_IOC3_ENET_BUFFER_INDEX:
    case SGI_IOC3_ENET_MII_DATA_R:
        /* Read-only. A guest writing here is not told it worked. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-eth: write to read-only register 0x%03x\n",
                      (unsigned)(SGI_IOC3_ENET_BASE + addr));
        return;

    default:
        s->enet_reg[index] = val;
        return;
    }
}

static const MemoryRegionOps sgi_ioc3_enet_ops = {
    .read = sgi_ioc3_enet_read,
    .write = sgi_ioc3_enet_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static bool sgi_ioc3_eth_mac_unset(const MACAddr *mac)
{
    static const MACAddr zero = { };

    return memcmp(mac, &zero, sizeof(*mac)) == 0;
}

static void sgi_ioc3_eth_realize(DeviceState *dev, Error **errp)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!s->dma_mr) {
        error_setg(errp, TYPE_SGI_IOC3_ETH " 'dma-memory' link not set");
        return;
    }
    address_space_init(&s->dma_as, s->dma_mr, "sgi-ioc3-eth-dma");

    memory_region_init_io(&s->mmio, OBJECT(dev), &sgi_ioc3_enet_ops, s,
                          "sgi-ioc3.eth", SGI_IOC3_ENET_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);

    /* A bus-less plain device is never reset, hence sysbus. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->phy), errp)) {
        return;
    }

    /*
     * The one-wire part answers with this address and the firmware expects
     * SGI's OUI on it, so an unset or generator-filled address is stamped.
     * mac-generated carries the provenance the bytes cannot: mac= may name
     * any value, the generator's range included, and an explicit mac= is a
     * replacement part's programmed identity and stays intact.
     */
    if (sgi_ioc3_eth_mac_unset(&s->conf.macaddr) || s->mac_generated) {
        qemu_macaddr_default_if_unset(&s->conf.macaddr);
        s->conf.macaddr.a[0] = 0x08;
        s->conf.macaddr.a[1] = 0x00;
        s->conf.macaddr.a[2] = 0x69;
    }
    s->nic = qemu_new_nic(&sgi_ioc3_eth_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    /*
     * The callback above only fires on a change, so a backend whose link
     * was taken down before this device existed has to be read once here.
     */
    dp83840_update_link(&s->phy, qemu_get_queue(s->nic)->link_down);
}

static void sgi_ioc3_eth_unrealize(DeviceState *dev)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(dev);

    qemu_del_nic(s->nic);
    /* An address space left behind outlives the device on the global list. */
    address_space_destroy(&s->dma_as);
}

static void sgi_ioc3_eth_reset_hold(Object *obj, ResetType type)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(obj);

    /* Zero throughout; a block that came up enabled ran unasked. */
    sgi_ioc3_eth_reset_registers(s, false);
}

static void sgi_ioc3_eth_init(Object *obj)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(obj);

    object_initialize_child(obj, "phy", &s->phy, TYPE_DP83840);

    /* One copy: the routing address and the reported one cannot differ. */
    object_property_add_alias(obj, "phy-addr", OBJECT(&s->phy), "addr");
}

static const VMStateDescription vmstate_sgi_ioc3_eth = {
    .name = "sgi-ioc3-eth",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(enet_reg, SGIIOC3EthState, SGI_IOC3_ENET_REGS),
        VMSTATE_UINT16(mii_data, SGIIOC3EthState),
        VMSTATE_UINT16(mii_data_w, SGIIOC3EthState),
        VMSTATE_BOOL_V(rx_timer_armed, SGIIOC3EthState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_ioc3_eth_properties[] = {
    DEFINE_NIC_PROPERTIES(SGIIOC3EthState, conf),
    DEFINE_PROP_BOOL("mac-generated", SGIIOC3EthState, mac_generated, false),
    DEFINE_PROP_LINK("dma-memory", SGIIOC3EthState, dma_mr,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void sgi_ioc3_eth_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI IOC3 ethernet MAC engine";
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->realize = sgi_ioc3_eth_realize;
    dc->unrealize = sgi_ioc3_eth_unrealize;
    rc->phases.hold = sgi_ioc3_eth_reset_hold;
    dc->vmsd = &vmstate_sgi_ioc3_eth;
    device_class_set_props(dc, sgi_ioc3_eth_properties);
}

static const TypeInfo sgi_ioc3_eth_types[] = {
    {
        .name = TYPE_SGI_IOC3_ETH,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIIOC3EthState),
        .instance_init = sgi_ioc3_eth_init,
        .class_init = sgi_ioc3_eth_class_init,
    },
};

DEFINE_TYPES(sgi_ioc3_eth_types)
