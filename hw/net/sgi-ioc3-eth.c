/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 ethernet MAC engine. See docs/specs/sgi-ioc3.rst.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
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

/* The parent's buffer RAM asks this block for its parity-enable bit. */
bool sgi_ioc3_eth_ram_parity_enabled(SGIIOC3EthState *s)
{
    return sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL) &
           SGI_IOC3_ENET_MCR_RAM_PARITY;
}

/* Reading the counters clears them. */
static uint32_t sgi_ioc3_take_counters(SGIIOC3EthState *s)
{
    unsigned int index = (SGI_IOC3_ENET_COUNTERS - SGI_IOC3_ENET_BASE) / 4;
    uint32_t counters = s->enet_reg[index];

    s->enet_reg[index] = 0;
    return counters;
}

static void sgi_ioc3_enet_update_irq(SGIIOC3EthState *s)
{
    uint32_t status = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_INT_STATUS);
    uint32_t enable = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_INT_ENABLE);
    bool level = (status & enable) != 0;

    if (level == s->irq_asserted) {
        return;
    }
    trace_sgi_ioc3_eth_irq(status, enable, level);
    qemu_set_irq(s->irq, level);
    s->irq_asserted = level;
}

/*
 * Raise sources into INT_STATUS. A bit already set here has not been
 * acknowledged yet, so this delivery has no bit of its own to be seen by:
 * mark it deferred so the guest's write-one-to-clear that acknowledges the
 * earlier completion turns into a real edge for this one too, instead of
 * silently clearing both (docs/datasheets/bridge.md, "must never latch and
 * clear on service").
 */
static void sgi_ioc3_eth_raise_int(SGIIOC3EthState *s, uint32_t sources)
{
    uint32_t status = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_INT_STATUS);

    s->int_deferred |= status & sources;
    sgi_ioc3_enet_set(s, SGI_IOC3_ENET_INT_STATUS, status | sources);
    sgi_ioc3_enet_update_irq(s);
}

/*
 * The engine reset keeps the ring bases: SGI's power-on diagnostic
 * programs them once and never rewrites them across its reset.
 */
static void sgi_ioc3_eth_reset_registers(SGIIOC3EthState *s, bool keep_rings)
{
    uint32_t rx_hi = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_RING_HI);
    uint32_t rx_lo = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_RING_LO);
    uint32_t tx_hi = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_TX_RING_HI);
    uint32_t tx_lo = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_TX_RING_LO);

    if (s->rx_flush_bh) {
        qemu_bh_cancel(s->rx_flush_bh);
    }
    sgi_net_rx_latency_reset(&s->rx_latency);
    s->defer_backend_rx = false;
    s->backend_rx_deferred = false;
    s->int_deferred = 0;
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
    s->rx_consume_staged = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_CONSUME) &
                           SGI_IOC3_ENET_RX_INDEX_MASK;
    sgi_ioc3_enet_update_irq(s);
}

static bool sgi_ioc3_eth_mac_loopback(SGIIOC3EthState *s)
{
    return sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL) &
           SGI_IOC3_ENET_MCR_LOOPBACK;
}

static bool sgi_ioc3_eth_looping_back(SGIIOC3EthState *s)
{
    return sgi_ioc3_eth_mac_loopback(s) || dp83840_in_loopback(&s->phy);
}

static bool sgi_ioc3_eth_can_receive(NetClientState *nc)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(qemu_get_nic_opaque(nc));
    uint32_t mcr = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL);
    uint32_t need = SGI_IOC3_ENET_MCR_RX_DMA_EN | SGI_IOC3_ENET_MCR_RX_EN;

    if (s->defer_backend_rx) {
        s->backend_rx_deferred = true;
        return false;
    }
    if ((mcr & need) != need) {
        return false;
    }

    /* In loopback, wire traffic would shift a diagnostic's comparisons. */
    if (sgi_ioc3_eth_looping_back(s)) {
        return false;
    }

    /* No room on the arrival line is the same answer as no buffer. */
    if (sgi_net_rx_latency_full(&s->rx_latency)) {
        return false;
    }

    /*
     * Against the staged index, not the published one: a buffer whose
     * completion is still held has already been used.
     */
    return (sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_PRODUCE) &
           SGI_IOC3_ENET_RX_INDEX_MASK) != s->rx_consume_staged;
}

static ssize_t sgi_ioc3_eth_deliver(SGIIOC3EthState *s, const uint8_t *buf,
                                    size_t size, bool short_circuit,
                                    bool immediate);

/* Linux's ioc3_hash(): low six CRC bits, themselves bit-reversed. */
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

/* Filter against the guest-programmed station address, not the backend's. */
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
        /* Consumed, not 0: a 0 return wedges the queue on this frame. */
        trace_sgi_ioc3_eth_rx_filtered((uint32_t)size);
        return size;
    }

    return sgi_ioc3_eth_deliver(s, buf, size, false, false);
}

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
    /* At-or-below: strictly-below is one frame late for OpenBSD. */
    if (available <= threshold) {
        sources |= SGI_IOC3_ENET_INT_RX_THRESHOLD;
    }
    return sources;
}

/*
 * Publish a receive the engine has already written: the header word whose
 * valid bit hands the buffer over, the consume index, and the receive
 * sources. Held behind the arrival floor by sgi_net_rx_latency_hold().
 */
static bool sgi_ioc3_eth_rx_publish(void *opaque, const void *record)
{
    SGIIOC3EthState *s = opaque;
    const SGIIOC3EthRxDone *done = record;

    if (stl_be_dma(&s->dma_as, done->buf_addr, done->word0,
                   MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-eth: receive header at 0x%" PRIx64
                      " cannot be written; the frame is held\n",
                      done->buf_addr);
        return false;
    }

    sgi_ioc3_enet_set(s, SGI_IOC3_ENET_RX_CONSUME, done->consume);
    sgi_ioc3_eth_raise_int(s, sgi_ioc3_eth_rx_sources(s, done->consume));
    return true;
}

/*
 * short_circuit marks a MAC-loopback frame, turned round ahead of the FCS
 * append and status vector; PHY loopback goes the whole way and gets both.
 * immediate marks a frame the MAC turned round internally, which never
 * crossed the wire and so carries none of the arrival latency the floor
 * models. It is still published in order behind anything the line is holding,
 * or the consume index it moves would pass a held one and then walk backwards
 * when that one lands. It also arrives from the transmit walk rather than
 * receive(), so can_receive() never ran for it -- the line's room is checked
 * here instead, and a full line is the overrun any other receive gets.
 */
static ssize_t sgi_ioc3_eth_deliver(SGIIOC3EthState *s, const uint8_t *buf,
                                    size_t size, bool short_circuit,
                                    bool immediate)
{
    SGIIOC3EthRxDone done;
    uint32_t consume, offset, mcr, word0, word1, lenlo, lenhi, wire_len;
    uint64_t ring_base, buf_addr;
    bool truncated = false;
    MemTxResult res;

    /*
     * A loopback frame reaches here without can_receive()'s gate, so the
     * arrival line's room has to be checked here instead. No different from
     * any other receive with nowhere to go: an overrun drops it.
     */
    if (immediate && sgi_net_rx_latency_full(&s->rx_latency)) {
        trace_sgi_ioc3_eth_rx_refused(
            sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL),
            sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_PRODUCE),
            sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_CONSUME));
        return size;
    }

    if (size > SGI_IOC3_ENET_FRAME_MAX - 8) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-eth: %zu-byte frame truncated to the %d bytes"
                      " a receive delivery can carry\n",
                      size, SGI_IOC3_ENET_FRAME_MAX - 8);
        size = SGI_IOC3_ENET_FRAME_MAX - 8;
        truncated = true;
    }

    consume = s->rx_consume_staged;
    ring_base = ((uint64_t)sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_RING_HI)
                << 32) |
               (sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_RING_LO) &
                SGI_IOC3_ENET_RX_BASE_MASK);

    /* A refused entry fetch leaves no destination; drop the frame. */
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
     * Guests read the wire length FCS included; the backend delivers no
     * FCS, so four phantom bytes are added or minimum frames read short.
     */
    wire_len = (uint32_t)size + (short_circuit ? 0 : 4);

    word0 = SGI_IOC3_ENET_RXH_VALID |
           ((wire_len << SGI_IOC3_ENET_RXH_BYTECNT_SHIFT) &
            SGI_IOC3_ENET_RXH_BYTECNT_MASK);

    /*
     * The checksum-offload partial must be exact: Linux finishes the
     * transport checksum from it and trusts the fold.
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

    res = stl_be_dma(&s->dma_as, buf_addr + 4, short_circuit ? 0 : word1,
                     MEMTXATTRS_UNSPECIFIED);
    res |= dma_memory_write(&s->dma_as, buf_addr + offset, buf, size,
                            MEMTXATTRS_UNSPECIFIED);
    if (!short_circuit) {
        /* The phantom FCS bytes, written as the zero they were summed as. */
        res |= stl_be_dma(&s->dma_as, buf_addr + offset + size, 0,
                          MEMTXATTRS_UNSPECIFIED);
    }
    /* An unreachable buffer is not a delivered frame; the index stays. */
    if (res != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-eth: receive buffer at 0x%" PRIx64
                      " cannot be written; the frame is dropped\n", buf_addr);
        return size;
    }

    consume = (consume + (1u << SGI_IOC3_ENET_RX_RING_SHIFT)) &
             SGI_IOC3_ENET_RX_INDEX_MASK;
    s->rx_consume_staged = consume;

    /*
     * The payload and its status vector are in the buffer; the valid bit,
     * the index and the sources are what the guest sees, and those wait for
     * the frame's arrival deadline.
     */
    done.buf_addr = buf_addr;
    done.word0 = word0;
    done.consume = consume;
    if (immediate) {
        sgi_net_rx_latency_no_wire(&s->rx_latency, &done);
    } else {
        sgi_net_rx_latency_hold(&s->rx_latency, &done);
    }

    return size;
}

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
        /* The 7-bit length can name more than the 104-byte area holds. */
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

static bool sgi_ioc3_eth_tx_enabled(SGIIOC3EthState *s)
{
    return (sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL) &
            (SGI_IOC3_ENET_MCR_TX_DMA_EN | SGI_IOC3_ENET_MCR_TX_EN)) ==
           (SGI_IOC3_ENET_MCR_TX_DMA_EN | SGI_IOC3_ENET_MCR_TX_EN);
}

/*
 * Runs from the main loop, never from the MMIO write that schedules it: a
 * synchronous qemu_flush_queued_packets() there would re-enter receive
 * delivery before the guest's store instruction that posted a buffer, or
 * flipped MAC_CONTROL, or left loopback, has even returned (issue #554).
 */
static void sgi_ioc3_eth_rx_flush(void *opaque)
{
    SGIIOC3EthState *s = opaque;

    trace_sgi_ioc3_eth_rx_flush();
    qemu_flush_queued_packets(qemu_get_queue(s->nic));
}

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
    bool loopback = sgi_ioc3_eth_looping_back(s);
    uint32_t raised = 0;
    bool completed_one = false;

    /*
     * The walk stops on equality: an index the steps can never land on is
     * an emulator hang from one register write, so reduce and align both.
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

        /* A refused fetch stops the walk with the index on the entry. */
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
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc3-eth: the frame for the transmit descriptor"
                          " at 0x%" PRIx64 " cannot be read; it is not sent\n",
                          desc);
            break;
        }
        len = MIN(len, cmd & SGI_IOC3_ENET_TXD_BYTECNT_MASK);

        /* Transport sum only; the IP header checksum is the driver's own. */
        if (cmd & SGI_IOC3_ENET_TXD_DOCHECKSUM) {
            net_checksum_calculate(s->frame, len, CSUM_TCP | CSUM_UDP);
        }

        /* Pad to the 802.3 minimum: IRIX relies on the engine for it. */
        if ((sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL) &
             SGI_IOC3_ENET_MCR_PAD_EN) && len < 60) {
            memset(s->frame + len, 0, 60 - len);
            len = 60;
        }

        trace_sgi_ioc3_eth_tx((uint32_t)len, loopback);

        if (len > 0) {
            if (loopback) {
                sgi_ioc3_eth_deliver(s, s->frame, len,
                                     sgi_ioc3_eth_mac_loopback(s), true);
            } else {
                s->defer_backend_rx = true;
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

    /* Idle is derived from the index, not from the loop returning. */
    sgi_ioc3_enet_set(s, SGI_IOC3_ENET_TX_CONSUME,
                      consume | (consume == produce ?
                                 SGI_IOC3_ENET_TX_CONSUME_IDLE : 0));
    sgi_ioc3_eth_raise_int(s, raised);

    s->defer_backend_rx = false;
    if (s->backend_rx_deferred) {
        s->backend_rx_deferred = false;
        qemu_bh_schedule(s->rx_flush_bh);
    }
}

static uint64_t sgi_ioc3_enet_read_impl(void *opaque, hwaddr addr,
                                        unsigned size)
{
    SGIIOC3EthState *s = opaque;
    unsigned int index = addr / 4;

    switch (SGI_IOC3_ENET_BASE + addr) {
    case SGI_IOC3_ENET_COUNTERS:
        return sgi_ioc3_take_counters(s);

    case SGI_IOC3_ENET_BUFFER_INDEX:
        /* The engines' positions in buffer memory; nothing is moving. */
        return 0;

    case SGI_IOC3_ENET_MII_CONTROL:
        return s->enet_reg[index] & ~SGI_IOC3_ENET_MII_BUSY;

    case SGI_IOC3_ENET_MII_DATA_R:
        return s->mii_data;

    default:
        return s->enet_reg[index];
    }
}

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
    case SGI_IOC3_ENET_INT_STATUS: {
        uint32_t cleared = (uint32_t)val;
        uint32_t deferred = s->int_deferred & cleared;

        s->enet_reg[index] &= ~cleared;
        s->int_deferred &= ~cleared;
        sgi_ioc3_enet_update_irq(s);
        if (deferred) {
            /*
             * A completion landed on one of these bits after the driver's
             * read but before this acknowledge: the line has just dropped
             * for the cleared bits above, so restoring the deferred ones
             * now raises it again as a real edge, rather than the guest's
             * single write silently absorbing both completions.
             *
             * This depends on "cleared" being the whole status word the
             * driver read, so status & enable truly reaches zero above.
             * All three public drivers write back exactly what they read
             * here (docs/datasheets/ioc3.md, "0xF4 Interrupt status").
             */
            s->enet_reg[index] |= deferred;
            sgi_ioc3_enet_update_irq(s);
        }
        return;
    }

    case SGI_IOC3_ENET_INT_ENABLE:
        /* Enabling a set source must raise the line; OpenBSD relies on it. */
        s->enet_reg[index] = val;
        sgi_ioc3_enet_update_irq(s);
        return;

    case SGI_IOC3_ENET_MII_CONTROL: {
        unsigned int phy = (val >> SGI_IOC3_ENET_MII_PHY_SHIFT) &
                           SGI_IOC3_ENET_MII_PHY_MASK;
        unsigned int reg = val & SGI_IOC3_ENET_MII_REG_MASK;

        /* Only the configured address answers, or a scan finds phantoms. */
        if (val & SGI_IOC3_ENET_MII_READ) {
            s->mii_data = phy == s->phy.addr
                          ? dp83840_read(&s->phy, reg)
                          : SGI_IOC3_ENET_MII_ABSENT;
        } else if (phy == s->phy.addr) {
            bool was_looping = sgi_ioc3_eth_looping_back(s);

            dp83840_write(&s->phy, reg, s->mii_data_w);

            /* Leaving loopback re-opens receive; defer what queued. */
            if (was_looping && !sgi_ioc3_eth_looping_back(s)) {
                qemu_bh_schedule(s->rx_flush_bh);
            }
        }

        s->enet_reg[index] = val;
        return;
    }

    case SGI_IOC3_ENET_MII_DATA_W:
        s->mii_data_w = val;
        s->enet_reg[index] = val;
        return;

    case SGI_IOC3_ENET_TX_PRODUCE:
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
        qemu_bh_schedule(s->rx_flush_bh);

        /*
         * Enabling starts the engine (the diagnostic queues first); only
         * on the transition, or a rewrite sends the descriptors twice.
         */
        if (!was_enabled && sgi_ioc3_eth_tx_enabled(s)) {
            sgi_ioc3_eth_do_tx(s, sgi_ioc3_enet_get(s,
                                                    SGI_IOC3_ENET_TX_PRODUCE));
        }
        return;

    case SGI_IOC3_ENET_RX_PRODUCE:
        /* Bit 31 is a command: a bare index write must not disarm. */
        if (val & SGI_IOC3_ENET_RPIR_ARM) {
            s->rx_timer_armed = true;
        }
        s->enet_reg[index] = val;
        /*
         * Posting buffers is what restarts placement: release whatever the
         * backend is holding, and retry a completion the engine could not
         * write, rather than leaving either to wait.
         */
        sgi_net_rx_latency_ready(&s->rx_latency);
        qemu_bh_schedule(s->rx_flush_bh);
        return;

    case SGI_IOC3_ENET_RX_CONSUME:
        /*
         * Moving the engine's own index abandons whatever it had in flight:
         * the completions held behind the arrival floor name buffers the
         * guest has just said the engine is no longer at.
         */
        if (s->rx_latency.count != 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-ioc3-eth: receive consume index rewritten with"
                          " %u frames in flight; they are discarded\n",
                          s->rx_latency.count);
        }
        sgi_net_rx_latency_reset(&s->rx_latency);
        s->enet_reg[index] = val;
        s->rx_consume_staged = val & SGI_IOC3_ENET_RX_INDEX_MASK;
        return;

    case SGI_IOC3_ENET_COUNTERS:
    case SGI_IOC3_ENET_BUFFER_INDEX:
    case SGI_IOC3_ENET_MII_DATA_R:
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

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->phy), errp)) {
        return;
    }

    /*
     * The firmware expects SGI's OUI on a default address; an explicit
     * mac= is never rewritten, which the bytes alone cannot decide.
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
    s->rx_flush_bh = qemu_bh_new_guarded(sgi_ioc3_eth_rx_flush, s,
                                         &dev->mem_reentrancy_guard);
    if (!sgi_net_rx_latency_init(&s->rx_latency, s->rx_arrival_latency_us,
                                 sizeof(SGIIOC3EthRxDone),
                                 sgi_ioc3_eth_rx_publish,
                                 sgi_ioc3_eth_rx_flush, s, errp)) {
        return;
    }
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);

    /* The callback fires on change only; read the initial link state. */
    dp83840_update_link(&s->phy, qemu_get_queue(s->nic)->link_down);
}

static void sgi_ioc3_eth_unrealize(DeviceState *dev)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(dev);

    sgi_net_rx_latency_cleanup(&s->rx_latency);
    qemu_bh_delete(s->rx_flush_bh);
    qemu_del_nic(s->nic);
    address_space_destroy(&s->dma_as);
    /*
     * The transceiver lives inside this object and is realized on the system
     * bus, so it has to leave that bus here. Left on it, it comes off only
     * when this object is finalized, and a bus hands a child's reference back
     * through RCU -- so the release arrives a grace period after the storage
     * it names has been freed with the rest of this object.
     */
    object_unparent(OBJECT(&s->phy));
}

static void sgi_ioc3_eth_reset_hold(Object *obj, ResetType type)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(obj);

    sgi_ioc3_eth_reset_registers(s, false);
}

static void sgi_ioc3_eth_init(Object *obj)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(obj);

    object_initialize_child(obj, "phy", &s->phy, TYPE_DP83840);

    object_property_add_alias(obj, "phy-addr", OBJECT(&s->phy), "addr");
    object_property_add_alias(obj, "phy-dp83843", OBJECT(&s->phy), "dp83843");
}

static int sgi_ioc3_eth_post_load(void *opaque, int version_id)
{
    SGIIOC3EthState *s = opaque;

    s->irq_asserted = (sgi_ioc3_enet_get(s, SGI_IOC3_ENET_INT_STATUS) &
                       sgi_ioc3_enet_get(s, SGI_IOC3_ENET_INT_ENABLE)) != 0;
    if (version_id < 4) {
        /* No arrival line in the stream: the engine is where it published. */
        s->rx_consume_staged = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_CONSUME) &
                               SGI_IOC3_ENET_RX_INDEX_MASK;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_ioc3_eth = {
    .name = "sgi-ioc3-eth",
    .version_id = 4,
    .minimum_version_id = 1,
    .post_load = sgi_ioc3_eth_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(enet_reg, SGIIOC3EthState, SGI_IOC3_ENET_REGS),
        VMSTATE_UINT16(mii_data, SGIIOC3EthState),
        VMSTATE_UINT16(mii_data_w, SGIIOC3EthState),
        VMSTATE_BOOL_V(rx_timer_armed, SGIIOC3EthState, 2),
        VMSTATE_UINT32_V(int_deferred, SGIIOC3EthState, 3),
        VMSTATE_UINT32_V(rx_consume_staged, SGIIOC3EthState, 4),
        VMSTATE_STRUCT(rx_latency, SGIIOC3EthState, 4,
                       vmstate_sgi_net_rx_latency, SGINetRxLatency),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_ioc3_eth_properties[] = {
    DEFINE_NIC_PROPERTIES(SGIIOC3EthState, conf),
    DEFINE_PROP_BOOL("mac-generated", SGIIOC3EthState, mac_generated, false),
    DEFINE_PROP_UINT32("rx-arrival-latency-us", SGIIOC3EthState,
                       rx_arrival_latency_us, SGI_NET_RX_LATENCY_DEFAULT_US),
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
