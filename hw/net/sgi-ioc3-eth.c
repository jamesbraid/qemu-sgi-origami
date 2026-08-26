/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller: the ethernet MAC engine.
 *
 * Two descriptor rings and the register block that drives them: transmit
 * descriptors carried inline in the ring itself, and a receive ring of
 * buffer addresses a guest supplies. See docs/datasheets/ioc3.md's
 * "Ethernet operation" and "Who advances what, and when" for the sequence
 * this implements, and its "DMA" section for why every transfer here goes
 * through this device's own address space rather than the plain system one.
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
 * Whether the guest has asked for the buffer RAM's parity to be checked.
 *
 * The RAM itself is decoded by the parent chip, but the bit that turns
 * checking on is this block's, so the parent asks rather than reaching into a
 * register it does not own. SGI's power-on diagnostic sets this bit before it
 * touches the window at all and leaves it set through the size probe that
 * follows.
 */
bool sgi_ioc3_eth_ram_parity_enabled(SGIIOC3EthState *s)
{
    return sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL) &
           SGI_IOC3_ENET_MCR_RAM_PARITY;
}

/*
 * Read the collision and deferral counters and zero them in the same step,
 * which is what the hardware does and what makes reading them a way to clear
 * them. Separate from the read handler because the clearing is the part worth
 * naming: a caller that only wanted the value would be using the wrong verb.
 */
static uint32_t sgi_ioc3_take_counters(SGIIOC3EthState *s)
{
    unsigned int index = (SGI_IOC3_ENET_COUNTERS - SGI_IOC3_ENET_BASE) / 4;
    uint32_t counters = s->enet_reg[index];

    s->enet_reg[index] = 0;
    return counters;
}

/*
 * This block's own interrupt line. It is level-driven from the intersection
 * of its status and enable registers, the same shape as the parent chip's
 * SuperIO line -- but unlike that one, this line is the chip's ordinary PCI
 * interrupt pin, reached through the GPIO output the parent connects to a
 * handler that raises it, rather than a wire this device owns end to end.
 */
static void sgi_ioc3_enet_update_irq(SGIIOC3EthState *s)
{
    uint32_t status = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_INT_STATUS);
    uint32_t enable = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_INT_ENABLE);

    trace_sgi_ioc3_eth_irq(status, enable, (status & enable) != 0);
    qemu_set_irq(s->irq, (status & enable) != 0);
}

/*
 * Clear the block. `keep_rings` distinguishes the two resets that reach here,
 * which clear different amounts.
 *
 * The device's own reset clears everything: nothing has been programmed yet
 * and there is nothing to preserve.
 *
 * The MAC control register's reset bit resets the engine, not the addresses
 * the engine was given -- so the two ring base registers survive it. That is
 * SGI's power-on loopback diagnostic's requirement rather than a reading of
 * the datasheet: it programs both ring bases, then asserts the reset bit, then
 * programs only the four indices and starts the engine. It never writes either
 * base again, so a reset that cleared them leaves the engine fetching
 * descriptors from address zero, which is what it did here -- the diagnostic
 * reported ten packets sent and none received. Both guest drivers program the
 * bases after their reset rather than before, which is why no boot found this.
 *
 * What the reset does to everything else is unobserved. The diagnostic
 * rewrites the indices itself, so nothing here says whether hardware cleared
 * them, and clearing them is kept because it is what this model already did.
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
 * Whether the receive engine has anywhere to put a frame: the two enable
 * bits set and a buffer supplied that hardware has not already consumed.
 * Narrowed to the twelve bits the index registers own, because a guest is
 * not required to keep the bits above them clear and the arm-timer bit
 * shares a register with the produce index without being part of it.
 */
/*
 * Whether a frame turns round inside the machine instead of reaching the wire.
 * Two independent switches select it -- the controller's own loopback bit, and
 * the transceiver's -- and SGI's power-on diagnostic exercises both in turn.
 * Either one disconnects the block from the backend in both directions.
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

static bool sgi_ioc3_eth_can_receive(NetClientState *nc)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(qemu_get_nic_opaque(nc));
    uint32_t mcr = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL);
    uint32_t need = SGI_IOC3_ENET_MCR_RX_DMA_EN | SGI_IOC3_ENET_MCR_RX_EN;

    if ((mcr & need) != need) {
        return false;
    }

    /*
     * Loopback disconnects the block from the wire in both directions.
     * Refusing here keeps the backend's traffic out of a diagnostic's ring:
     * SGI's own loopback tests count and compare the frames they find there,
     * so one arriving from outside shifts every comparison after it and
     * reports a data miscompare on a frame the diagnostic never sent.
     */
    if (sgi_ioc3_eth_looping_back(s)) {
        return false;
    }

    return (sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_PRODUCE) &
           SGI_IOC3_ENET_RX_INDEX_MASK) !=
          (sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_CONSUME) &
           SGI_IOC3_ENET_RX_INDEX_MASK);
}

/*
 * Which receive sources a delivered frame raises, given the consume index it
 * has just advanced to. Two independent questions, neither of them "a frame
 * arrived": whether the timer is in service, and whether the buffers still
 * available to the engine have fallen to the watermark a driver asked to
 * hear about.
 *
 * The timer's reload is an idle timeout and the only value either driver
 * writes is zero, which is the one that needs no time to pass. A non-zero
 * reload would need a real timer, and nothing here has one, so it is
 * reported rather than silently treated as immediate.
 */
static ssize_t sgi_ioc3_eth_deliver(SGIIOC3EthState *s, const uint8_t *buf,
                                    size_t size, bool looped);

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

    return sgi_ioc3_eth_deliver(s, buf, size, false);
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
    /*
     * At-or-below rather than strictly-below, because that is what the only
     * driver programming this source asks for. The argument, its evidence
     * class and what stays unverified are set out in
     * docs/library/iec-receive-contract.md's item on which interrupt sources
     * the drivers enable, which is where the reasoning belongs and where it
     * can be checked against the driver line it comes from.
     *
     * Deliberately not restated here. This comparison has been called
     * settled, then unobserved, then settled again, and each round the code
     * and the pages took turns at being the more confident of the two -- a
     * comment that makes the claim can drift out of step with the document
     * that owns it, and a comment that names the document cannot.
     */
    if (available <= threshold) {
        sources |= SGI_IOC3_ENET_INT_RX_THRESHOLD;
    }
    return sources;
}

/*
 * Take the next buffer address off the receive ring, write the frame into it
 * at the offset the MAC control register selects, and advance the consume
 * index -- the sequence the datasheet's behavioral contract gives, in the
 * order it gives it. The ring holds addresses, not frames: what changes size
 * is not the ring, always eight bytes an entry, but the guest-allocated
 * buffer at the far end of the pointer it holds.
 */
/*
 * Put one frame into the receive ring. `short_circuit` says the frame turned
 * round inside the controller itself, ahead of the logic that appends a frame
 * check sequence and builds the receive status vector, so neither is present.
 *
 * The two loopbacks differ here and the firmware's own expectations are what
 * separate them. The controller's own loopback turns the frame round early:
 * SGI's diagnostic sends a byte count and requires exactly that count back,
 * with a status word of zero. The transceiver's loopback sends the frame the
 * whole way through the controller and back, so the same diagnostic requires
 * the count four higher -- the check sequence is really there -- and a status
 * word built as usual. Only the first is a short circuit.
 *
 * Split from the network client's receive callback because either loopback has
 * to reach it without going through that callback's refusal of wire traffic:
 * while looping back the block accepts nothing from the backend, and the frame
 * it accepts instead is its own.
 */
static ssize_t sgi_ioc3_eth_deliver(SGIIOC3EthState *s, const uint8_t *buf,
                                    size_t size, bool short_circuit)
{
    uint32_t consume, offset, mcr, word0, word1, lenlo, lenhi, wire_len;
    uint64_t ring_base, buf_addr;

    if (size > SGI_IOC3_ENET_FRAME_MAX - 8) {
        size = SGI_IOC3_ENET_FRAME_MAX - 8;
    }

    consume = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_CONSUME) &
             SGI_IOC3_ENET_RX_INDEX_MASK;
    ring_base = ((uint64_t)sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_RING_HI)
                << 32) |
               (sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_RING_LO) &
                SGI_IOC3_ENET_RX_BASE_MASK);

    ldq_be_dma(s->dma_as, ring_base + consume, &buf_addr,
              MEMTXATTRS_UNSPECIFIED);

    mcr = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL);
    offset = ((mcr & SGI_IOC3_ENET_MCR_RXOFF_MASK) >>
             SGI_IOC3_ENET_MCR_RXOFF_SHIFT) * 2;

    /*
     * Both guests read this length field as the wire length including the
     * four-byte FCS a real MAC captures alongside the frame -- OpenBSD's
     * iec_rxintr() checks it against ETHER_MIN_LEN before ever subtracting
     * ETHER_CRC_LEN, and IRIX's own ethernet driver subtracts the same
     * four-byte CRC length from the same field, independently. QEMU's net
     * layer never delivers a trailing CRC -- there is no physical layer here
     * to produce one -- so the length reported has to add the phantom bytes
     * back to match what real hardware would say, even though only `size` bytes
     * of real payload follow. Without this, any frame at or under the wire
     * minimum reports four bytes short, fails the guest's own minimum-length
     * check, and is silently dropped -- exactly the frames a DHCP exchange's
     * short broadcast packets tend to be.
     */
    /*
     * The four phantom bytes are compensation for something the wire path
     * loses, so the loopback path must not add them. A frame off the backend
     * arrives with the trailing FCS already stripped -- QEMU's net layer has
     * no physical layer to have produced one -- and both guests read this
     * field as the wire length including it, so the count has to put it back.
     * A frame short-circuited inside the controller lost nothing, because the
     * check sequence was never appended to it: SGI's loopback diagnostic
     * transmits a byte count and requires the same number back, so adding four
     * there reports a frame four bytes longer than the one it sent. A frame
     * looped by the transceiver did go through the whole controller, and the
     * same diagnostic requires the four counted.
     */
    wire_len = (uint32_t)size + (short_circuit ? 0 : 4);

    word0 = SGI_IOC3_ENET_RXH_VALID |
           ((wire_len << SGI_IOC3_ENET_RXH_BYTECNT_SHIFT) &
            SGI_IOC3_ENET_RXH_BYTECNT_MASK);

    lenlo = wire_len & 0x7u;
    lenhi = (wire_len >> 3) & 0x3ffu;
    word1 = SGI_IOC3_ENET_RXE_GOODPKT |
           (lenlo << SGI_IOC3_ENET_RXE_LEN_LO_SHIFT) |
           (lenhi << SGI_IOC3_ENET_RXE_LEN_HI_SHIFT);
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

    stl_be_dma(s->dma_as, buf_addr + 0, word0, MEMTXATTRS_UNSPECIFIED);
    stl_be_dma(s->dma_as, buf_addr + 4, short_circuit ? 0 : word1,
               MEMTXATTRS_UNSPECIFIED);
    dma_memory_write(s->dma_as, buf_addr + offset, buf, size,
                     MEMTXATTRS_UNSPECIFIED);

    consume = (consume + (1u << SGI_IOC3_ENET_RX_RING_SHIFT)) &
             SGI_IOC3_ENET_RX_INDEX_MASK;
    sgi_ioc3_enet_set(s, SGI_IOC3_ENET_RX_CONSUME, consume);

    sgi_ioc3_enet_set(s, SGI_IOC3_ENET_INT_STATUS,
                      sgi_ioc3_enet_get(s, SGI_IOC3_ENET_INT_STATUS) |
                      sgi_ioc3_eth_rx_sources(s, consume));
    sgi_ioc3_enet_update_irq(s);

    return size;
}

static void sgi_ioc3_eth_cleanup(NetClientState *nc)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(qemu_get_nic_opaque(nc));

    s->nic = NULL;
}

static NetClientInfo sgi_ioc3_eth_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = sgi_ioc3_eth_can_receive,
    .receive = sgi_ioc3_eth_receive,
    .cleanup = sgi_ioc3_eth_cleanup,
};

/*
 * Gather one frame from a transmit descriptor's inline data and up to two
 * out-of-line buffers, in that order -- the order the descriptor's own three
 * validity flags list them, and the order that lets a short frame go out
 * from the inline area alone. Returns how many bytes landed in the frame
 * buffer, capped at its size so a guest's own miscount cannot overrun it.
 */
static size_t sgi_ioc3_eth_tx_gather(SGIIOC3EthState *s, uint32_t cmd,
                                     uint32_t bufcnt, uint64_t buf1,
                                     uint64_t buf2, const uint8_t *inline_data)
{
    size_t len = 0;
    unsigned int n;

    if (cmd & SGI_IOC3_ENET_TXD_DATA_VALID) {
        n = bufcnt & SGI_IOC3_ENET_TXD_DATA_LEN_MASK;
        /*
         * The length field is seven bits wide and the area it measures holds
         * 104 bytes, so a guest can name more inline data than exists. The
         * cap below does not catch that -- the frame buffer it guards is
         * twenty times larger than the area -- and without this the copy
         * reads past the end of the caller's descriptor image.
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
        dma_memory_read(s->dma_as, buf1, s->frame + len, n,
                        MEMTXATTRS_UNSPECIFIED);
        len += n;
    }
    if (cmd & SGI_IOC3_ENET_TXD_BUF2_VALID) {
        n = (bufcnt & SGI_IOC3_ENET_TXD_BUF2_LEN_MASK) >>
           SGI_IOC3_ENET_TXD_BUF2_LEN_SHIFT;
        if (n > sizeof(s->frame) - len) {
            n = sizeof(s->frame) - len;
        }
        dma_memory_read(s->dma_as, buf2, s->frame + len, n,
                        MEMTXATTRS_UNSPECIFIED);
        len += n;
    }
    return len;
}

/*
 * Walk the transmit ring from the consume index up to the produce index a
 * guest just wrote, sending one frame per descriptor and advancing the
 * consume index as each is sent -- "Transmitting, in order" in the
 * datasheet's behavioral contract. The consume index carries an idle flag in
 * the same word as its byte offset; it is set once the ring is drained to
 * the produce index this call was given, and cleared for as long as more
 * than one descriptor remains queued behind it (there never is more than
 * one, since this walks the whole batch before returning, but the flag is
 * still derived rather than assumed for the one descriptor in flight).
 */
/*
 * Whether the transmit engine is running. Both of its enables have to be set:
 * the DMA enable lets it fetch descriptors and the transmit enable lets it put
 * a frame on the wire, and the datasheet gives them as separate bits.
 */
static bool sgi_ioc3_eth_tx_enabled(SGIIOC3EthState *s)
{
    return (sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL) &
            (SGI_IOC3_ENET_MCR_TX_DMA_EN | SGI_IOC3_ENET_MCR_TX_EN)) ==
           (SGI_IOC3_ENET_MCR_TX_DMA_EN | SGI_IOC3_ENET_MCR_TX_EN);
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
    /*
     * The sources this walk raises, accumulated on their own rather than
     * onto a copy of the interrupt status register taken here. Sending a
     * frame below re-enters this device: a backend that answers -- QEMU's
     * user-mode networking answers a request from inside the same call that
     * sent it -- delivers the reply through this engine's own receive path
     * several frames down the stack, and that path raises the receive
     * source. A copy taken before the walk does not have it, so writing the
     * copy back afterwards erases it, and a driver dispatching on the
     * sources it can see never collects the frame that is already in its
     * ring. Read the register once at the end and add to what it holds.
     */
    bool loopback = sgi_ioc3_eth_looping_back(s);
    uint32_t raised = 0;
    bool completed_one = false;

    /*
     * Both indices are byte offsets into the ring, and the datasheet gives
     * both as a descriptor number shifted left by seven -- so the low seven
     * bits are not part of either index and no value the engine can be given
     * has them set. A guest can still write them, and the walk below steps by
     * a whole descriptor and stops on equality, so a produce index the steps
     * can never land on is a loop that never ends. That is reachable from a
     * single unaligned register write, which makes it a guest hanging the
     * emulator rather than a fidelity question. Reduce both into the ring and
     * drop the bits that are not part of an index, so the walk terminates
     * whatever it is handed.
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
        size_t len;

        ldl_be_dma(s->dma_as, desc + 0, &cmd, MEMTXATTRS_UNSPECIFIED);
        ldl_be_dma(s->dma_as, desc + 4, &bufcnt, MEMTXATTRS_UNSPECIFIED);
        ldq_be_dma(s->dma_as, desc + 8, &buf1, MEMTXATTRS_UNSPECIFIED);
        ldq_be_dma(s->dma_as, desc + 16, &buf2, MEMTXATTRS_UNSPECIFIED);
        dma_memory_read(s->dma_as, desc + SGI_IOC3_ENET_TXD_DATA_OFFSET,
                        inline_data, sizeof(inline_data),
                        MEMTXATTRS_UNSPECIFIED);

        len = sgi_ioc3_eth_tx_gather(s, cmd, bufcnt, buf1, buf2, inline_data);
        len = MIN(len, cmd & SGI_IOC3_ENET_TXD_BYTECNT_MASK);

        /*
         * The descriptor can ask the engine to finish the frame's transport
         * checksum: the driver pre-loads the pseudo-header partial and the
         * hardware sums the whole frame and writes the fold at the descriptor's
         * offset. Compute the finished TCP or UDP checksum directly -- the IP
         * header checksum is the driver's own, so only the transport sum is
         * inserted. Ignoring the bit ships the driver's partial value, and the
         * far end (slirp, under user-mode networking) drops the frame.
         */
        if (cmd & SGI_IOC3_ENET_TXD_DOCHECKSUM) {
            net_checksum_calculate(s->frame, len, CSUM_TCP | CSUM_UDP);
        }

        /*
         * The engine pads any frame it assembles below the 802.3 sixty-byte
         * minimum (before the frame check sequence) when the MAC control
         * register's pad-enable bit is set -- "Padding a short frame" in the
         * datasheet. Two of the three guest drivers pad in their own transmit
         * path and leave the bit inert; the one that doesn't -- IRIX, in both
         * its kernel and its netboot PROM -- relies on the engine to do it,
         * so an unimplemented pad ships an ARP request or a bare
         * acknowledgement at its true short length. This has to apply before
         * both the loopback and wire paths below, since the engine pads
         * regardless of where the frame is going. s->frame is far larger
         * than sixty bytes, so the pad always fits.
         */
        if ((sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL) &
             SGI_IOC3_ENET_MCR_PAD_EN) && len < 60) {
            memset(s->frame + len, 0, 60 - len);
            len = 60;
        }

        if (len > 0) {
            if (loopback) {
                /*
                 * Loopback: the frame turns round inside the machine and
                 * never reaches the wire. Delivered straight into this
                 * device's own receive path, which is what the bit selects --
                 * a model that sent it to the backend instead would let a
                 * diagnostic's traffic out onto a real network and would wait
                 * for a reply that only a far end could send.
                 */
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

    sgi_ioc3_enet_set(s, SGI_IOC3_ENET_TX_CONSUME,
                      consume | SGI_IOC3_ENET_TX_CONSUME_IDLE);
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
        /*
         * Collisions and deferrals, cleared by the act of reading them. Both
         * guests read this once while resetting the block for no reason other
         * than to zero it, so a model that returned the count without clearing
         * would leave a driver's own counters climbing on a machine that has
         * never sent a frame.
         */
        return sgi_ioc3_take_counters(s);

    case SGI_IOC3_ENET_BUFFER_INDEX:
        /*
         * Where the block's own engines sit in the buffer memory. Nothing is
         * moving, so every one of the four positions is zero, and this is read
         * only rather than storage because software cannot place them.
         */
        return 0;

    case SGI_IOC3_ENET_MII_CONTROL:
        /*
         * Never busy. The read a guest asked for finished before it could
         * look, so the poll after it stops on the first pass.
         */
        return s->enet_reg[index] & ~SGI_IOC3_ENET_MII_BUSY;

    case SGI_IOC3_ENET_MII_DATA_R:
        /*
         * The word the last read request fetched. Which transceiver answered
         * is decided when the request is made rather than here, because that
         * is when the guest names an address: a driver scanning for a
         * transceiver writes each address in turn and reads this register
         * after each, and an address with nothing on it has to read as
         * all-ones or the scan stops at the first one it tries.
         */
        return s->mii_data;

    default:
        return s->enet_reg[index];
    }
}

/*
 * A wrapper rather than a trace call at each of the read handler's returns:
 * several of them return something other than the stored register, and a
 * trace that misses one of those is worse than none, since a reader takes
 * the absence of a line as the absence of an access.
 */
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
         * Written to acknowledge: a one clears the source it names rather than
         * setting it. This is the same contract as the parent chip's SuperIO
         * interrupt register, and the reason to state it twice is that
         * modelling this one as ordinary storage produces the same failure on
         * the ethernet line that it produced on the serial one -- a driver
         * writes back the bits it just read, the write sets them instead of
         * clearing them, and the source it was acknowledging never goes away.
         *
         * Clearing the register alone is not enough: the line this block
         * drives is level-triggered from status & enable, computed in
         * sgi_ioc3_enet_update_irq, and nothing else re-derives it. Skipping
         * that call here would leave the line exactly as high as it was
         * before the write -- correct for every register that does not gate
         * the interrupt, and silently wrong for the one that does, since a
         * guest's own acknowledge write would clear the bits it just read
         * without ever lowering the line it read them because of. The next
         * level-triggered assertion then finds an already-asserted line and
         * the guest's interrupt dispatcher reports it unclaimed, forever,
         * because by the time the handler reads the register again there is
         * nothing left in it to explain the line still being high.
         */
        s->enet_reg[index] &= ~(uint32_t)val;
        sgi_ioc3_enet_update_irq(s);
        return;

    case SGI_IOC3_ENET_INT_ENABLE:
        /*
         * The line is level-driven from status & enable, so the enable
         * register decides it as much as the status register does and has to
         * re-derive it on every write. Plain storage here leaves a source
         * that set while the enable was clear unable to raise the line when
         * a guest later enables it, and the window is reachable on every
         * bring-up: OpenBSD stops the interface by writing zero here, then
         * writes the receive enables in the control register -- which is
         * where this model flushes whatever the backend already had waiting
         * -- and only then writes this register. A frame delivered in
         * between sets its source against a clear enable and the line never
         * follows.
         */
        s->enet_reg[index] = val;
        sgi_ioc3_enet_update_irq(s);
        return;

    case SGI_IOC3_ENET_MII_CONTROL: {
        unsigned int phy = (val >> SGI_IOC3_ENET_MII_PHY_SHIFT) &
                           SGI_IOC3_ENET_MII_PHY_MASK;
        unsigned int reg = val & SGI_IOC3_ENET_MII_REG_MASK;

        /*
         * A management request. The read bit says which direction, and the
         * data register on the write side has already been loaded by the time
         * a write request arrives -- that is the order both drivers use.
         *
         * Only the one address the board wires answers. Every other address
         * reads all-ones, which is what a scan takes as an empty address, and
         * is why a model answering everywhere would be found at address zero
         * and print a line no real machine prints.
         */
        if (val & SGI_IOC3_ENET_MII_READ) {
            s->mii_data = phy == DP83840_ADDRESS
                          ? dp83840_read(&s->phy, reg)
                          : SGI_IOC3_ENET_MII_ABSENT;
        } else if (phy == DP83840_ADDRESS) {
            dp83840_write(&s->phy, reg, s->mii_data_w);
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
         * The doorbell: software has filled one or more descriptors and this
         * write says how far. Walk the ring now rather than waiting to be
         * asked -- nothing else polls an index without an interrupt to prompt
         * it, so the interrupt this walk raises is the only prompt there is.
         *
         * It is not the only thing that starts a walk, though. Software may
         * ring the doorbell while transmit is still switched off, and the
         * write that switches it on is what starts the engine then. The MAC
         * control register's own handler covers that.
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
         * Switching transmit on starts the engine on whatever the produce
         * index already names. A real engine fetches descriptors whenever it
         * is running and has not caught up with that index, so an enable that
         * arrives after the doorbell still sends -- and SGI's power-on
         * loopback diagnostic orders it exactly that way, queueing its
         * descriptors and then enabling transmit. Neither guest driver does,
         * which is why the doorbell alone looked like the whole trigger.
         *
         * Only on the transition, so that the further writes to this register
         * that follow -- the diagnostic adds the receive enables in two more
         * writes -- do not walk a ring that has already been walked. The walk
         * itself would be harmless, since it stops immediately once the
         * consume index has caught up, but a walk that found anything would
         * send it twice.
         */
        if (!was_enabled && sgi_ioc3_eth_tx_enabled(s)) {
            sgi_ioc3_eth_do_tx(s, sgi_ioc3_enet_get(s,
                                                    SGI_IOC3_ENET_TX_PRODUCE));
        }
        return;

    case SGI_IOC3_ENET_RX_PRODUCE:
        /*
         * Bit 31 puts the receive timer in service and the rest is the
         * index. The two are kept apart because only one of them is
         * storage: SGI's own driver writes the index bare from its
         * interrupt path, and disarming the timer there would silence the
         * source every driver dispatches its receive handler on.
         */
        if (val & SGI_IOC3_ENET_RPIR_ARM) {
            s->rx_timer_armed = true;
        }
        /*
         * Either write can be the one that makes a buffer available: a fresh
         * produce index, or the enable bits in the control register turning
         * receive on after buffers were already queued. Flushing here lets a
         * frame the backend already tried and was refused go out on the next
         * attempt instead of waiting for one that happens to arrive after.
         */
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
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void sgi_ioc3_eth_realize(DeviceState *dev, Error **errp)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sgi_ioc3_enet_ops, s,
                          "sgi-ioc3.eth", SGI_IOC3_ENET_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);

    /*
     * Realized as a system-bus device rather than a plain one because a
     * plain device realized with no bus is never reset -- see the parent
     * chip's own realize for the fuller account of why that matters.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->phy), errp)) {
        return;
    }

    /*
     * The network backend. Its address has no relation to what a guest's
     * driver believes its own station address is -- that comes from the
     * one-wire bus behind the parent chip's MicroLAN engine -- so a default
     * here costs nothing real hardware would have supplied differently:
     * nothing on this path reads it back.
     */
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&sgi_ioc3_eth_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void sgi_ioc3_eth_reset_hold(Object *obj, ResetType type)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(obj);

    /*
     * The block resets to zero throughout, which the datasheet gives for
     * every register in it. That matters most for the media access control
     * register, whose transmit and receive enables sit in it: a block that
     * came up enabled would be one no driver had asked to run.
     */
    sgi_ioc3_eth_reset_registers(s, false);
}

static void sgi_ioc3_eth_init(Object *obj)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(obj);

    object_initialize_child(obj, "phy", &s->phy, TYPE_DP83840);
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
};

static void sgi_ioc3_eth_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI IOC3 ethernet MAC engine";
    dc->realize = sgi_ioc3_eth_realize;
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
