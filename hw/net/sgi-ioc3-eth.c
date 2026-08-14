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
 * Whether the receive engine has anywhere to put a frame: the two enable
 * bits set and a buffer supplied that hardware has not already consumed.
 * Narrowed to the twelve bits the index registers own, because a guest is
 * not required to keep the bits above them clear and the arm-timer bit
 * shares a register with the produce index without being part of it.
 */
static bool sgi_ioc3_eth_can_receive(NetClientState *nc)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(qemu_get_nic_opaque(nc));
    uint32_t mcr = sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL);
    uint32_t need = SGI_IOC3_ENET_MCR_RX_DMA_EN | SGI_IOC3_ENET_MCR_RX_EN;

    if ((mcr & need) != need) {
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
static ssize_t sgi_ioc3_eth_receive(NetClientState *nc, const uint8_t *buf,
                                    size_t size)
{
    SGIIOC3EthState *s = SGI_IOC3_ETH(qemu_get_nic_opaque(nc));
    uint32_t consume, offset, mcr, word0, word1, lenlo, lenhi, wire_len;
    uint64_t ring_base, buf_addr;

    if (!sgi_ioc3_eth_can_receive(nc)) {
        trace_sgi_ioc3_eth_rx_refused(
            sgi_ioc3_enet_get(s, SGI_IOC3_ENET_MAC_CONTROL),
            sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_PRODUCE),
            sgi_ioc3_enet_get(s, SGI_IOC3_ENET_RX_CONSUME));
        return 0;
    }
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
    wire_len = (uint32_t)size + 4;

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
    stl_be_dma(s->dma_as, buf_addr + 4, word1, MEMTXATTRS_UNSPECIFIED);
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
    uint32_t raised = 0;
    bool completed_one = false;

    produce %= ring_bytes;

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
        if (len > 0) {
            qemu_send_packet(qemu_get_queue(s->nic), s->frame, len);
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
         * asked, since nothing else here ever will be -- neither guest polls
         * an index without an interrupt to prompt it, so the interrupt this
         * walk raises is the only prompt there is.
         */
        s->enet_reg[index] = val;
        if ((s->enet_reg[(SGI_IOC3_ENET_MAC_CONTROL - SGI_IOC3_ENET_BASE) / 4]
            & (SGI_IOC3_ENET_MCR_TX_DMA_EN | SGI_IOC3_ENET_MCR_TX_EN)) ==
           (SGI_IOC3_ENET_MCR_TX_DMA_EN | SGI_IOC3_ENET_MCR_TX_EN)) {
            sgi_ioc3_eth_do_tx(s, val);
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
        /* fall through */
    case SGI_IOC3_ENET_MAC_CONTROL:
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
    memset(s->enet_reg, 0, sizeof(s->enet_reg));
    s->mii_data = SGI_IOC3_ENET_MII_ABSENT;
    s->mii_data_w = 0;
    s->rx_timer_armed = false;
    sgi_ioc3_enet_update_irq(s);
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
