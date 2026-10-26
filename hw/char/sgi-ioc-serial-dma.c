/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI IOC3/IOC4 serial ring DMA engine.
 *
 * Each port's engine moves characters between rings in host memory and the
 * port's 16550: the transmit walk writes the UART's transmit register, and the
 * receive side drains its FIFO into a packer that becomes a ring entry. A
 * ring's address is derived from its port and direction; no register reads it
 * back.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/char/sgi-ioc-serial-dma.h"
#include "migration/vmstate.h"
#include "system/dma.h"

/* The 16550's published layout; hw/char/serial.c's macros are private. */
#define UART_RBR_THR    0
#define UART_MCR        4
#define UART_LSR        5
#define UART_MSR        6

#define UART_LSR_DR     0x01
#define UART_LSR_OE     0x02
#define UART_LSR_PE     0x04
#define UART_LSR_FE     0x08
#define UART_LSR_BI     0x10

#define UART_MCR_DTR    0x01
#define UART_MCR_RTS    0x02
#define UART_MCR_OUT1   0x04
#define UART_MCR_OUT2   0x08
#define UART_MCR_LOOP   0x10

#define UART_MSR_DCTS   0x01
#define UART_MSR_DDCD   0x08
#define UART_MSR_CTS    0x10
#define UART_MSR_DCD    0x80

static void sgi_ioc_sdma_run_from(SGIIOCSerialDMAPort *p, int64_t since);

static uint32_t *sdma_reg(SGIIOCSerialDMAPort *p, hwaddr offset)
{
    return &p->reg[offset / 4];
}

static uint32_t sdma_control(SGIIOCSerialDMAPort *p)
{
    return *sdma_reg(p, SGI_IOC_SDMA_CONTROL);
}

static unsigned sdma_pair(SGIIOCSerialDMAPort *p)
{
    return p->index / 2;
}

/* The selected ring size is also every index register's pointer width. */
static uint32_t sdma_ring_size(SGIIOCSerialDMAPort *p)
{
    uint32_t lo = p->engine->base_lo[sdma_pair(p)];

    return (lo & SGI_IOC_SDMA_RING_4K) ? SGI_IOC_SDMA_RING_4K_SIZE
                                       : SGI_IOC_SDMA_RING_1K_SIZE;
}

static uint32_t sdma_pointer_mask(SGIIOCSerialDMAPort *p)
{
    return (sdma_ring_size(p) - 1) & ~(SGI_IOC_SDMA_ENTRY_SIZE - 1);
}

/* The size-select bit is not address: unmasked, every ring is a byte off. */
static uint64_t sdma_ring(SGIIOCSerialDMAPort *p, unsigned direction)
{
    SGIIOCSerialDMA *d = p->engine;
    unsigned pair = sdma_pair(p);
    uint64_t base = ((uint64_t)d->base_hi[pair] << 32) |
                    (d->base_lo[pair] & SGI_IOC_SDMA_RING_ADDRESS);
    unsigned index = (p->index % 2) * SGI_IOC_SDMA_RING_DIRS + direction;

    return base + (uint64_t)index * sdma_ring_size(p);
}

/*
 * The chip ORs into its live register: a loopback transmit walk re-enters the
 * receiver, and writing back a pre-walk snapshot would lose its sources.
 */
static void sdma_raise(SGIIOCSerialDMAPort *p, SGIIOCSerialDMASource src)
{
    p->engine->ops->raise(p->engine->opaque, p->index, src);
}

static void sdma_set_level(SGIIOCSerialDMAPort *p, SGIIOCSerialDMASource src,
                           bool level)
{
    p->engine->ops->set_level(p->engine->opaque, p->index, src, level);
}

bool sgi_ioc_sdma_flow_cts(SGIIOCSerialDMA *d, unsigned port)
{
    SGIIOCSerialDMAPort *p = &d->port[port];

    /*
     * Looped back with the engine's hardware flow control on, the port
     * reports clear-to-send asserted even with request-to-send clear. The
     * board test observes the level, not the mechanism behind it, so this
     * supplies the level alone and leaves the modem-control register as
     * software wrote it.
     */
    return p->uart && (p->uart->mcr & UART_MCR_LOOP) &&
           (sdma_control(p) & SGI_IOC_SDMA_FLOW_CONTROL);
}

static bool sdma_running(SGIIOCSerialDMAPort *p)
{
    uint32_t control = sdma_control(p);

    return (control & SGI_IOC_SDMA_ENABLE) && !(control & SGI_IOC_SDMA_PAUSE);
}

/* Post the packer as one entry; unfilled slots carry no validity flags. */
static bool sdma_post(SGIIOCSerialDMAPort *p, uint32_t *produce,
                      uint32_t *consume, uint32_t mask)
{
    uint64_t ring = sdma_ring(p, SGI_IOC_SDMA_RING_RX);
    uint32_t next = (*produce + SGI_IOC_SDMA_ENTRY_SIZE) & mask;

    /* Full: refuse; the characters keep in the packer and the FIFO. */
    if (next == (*consume & mask)) {
        sdma_raise(p, SGI_IOC_SDMA_SRC_RX_FULL);
        return false;
    }

    /* A refused write must not advance the index past a missing entry. */
    if (dma_memory_write(p->engine->dma_as, ring + (*produce & mask),
                         p->packer, SGI_IOC_SDMA_ENTRY_SIZE,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        sdma_raise(p, SGI_IOC_SDMA_SRC_DMA_ERROR);
        return false;
    }
    *produce = next;
    p->packed = 0;
    memset(p->packer, 0, SGI_IOC_SDMA_ENTRY_SIZE);
    p->delivered = true;
    return true;
}

/*
 * The reload is receive idleness, in 16 kHz ticks from the last delivery,
 * which `since` places: a loopback arrival serviced late still counts from
 * when it reached the receiver.
 */
static void sdma_rx_timer_start(SGIIOCSerialDMAPort *p, int64_t since)
{
    uint32_t reload = *sdma_reg(p, SGI_IOC_SDMA_RX_TIMER) &
                      SGI_IOC_SDMA_TIMER_RELOAD;

    /* Once it has run, the count it held from the reload write is spent. */
    p->rx_idle_count = 0;
    timer_mod(p->rx_timer,
              since + (int64_t)reload * SGI_IOC_SDMA_TIMER_TICK_NS);
}

/* A drained ring expires into silence. */
static void sdma_rx_timer_expired(void *opaque)
{
    SGIIOCSerialDMAPort *p = opaque;
    uint32_t *produce = sdma_reg(p, SGI_IOC_SDMA_RX_PRODUCE);
    uint32_t *consume = sdma_reg(p, SGI_IOC_SDMA_RX_CONSUME);
    uint32_t mask = sdma_pointer_mask(p);

    p->rx_idle_count = 0;

    /* A still-packed entry means the ring was full; retry now. */
    if (p->packed && sdma_post(p, produce, consume, mask)) {
        p->delivered = false;
    }

    if (p->armed && ((*produce ^ *consume) & mask)) {
        sdma_raise(p, SGI_IOC_SDMA_SRC_RX_TIMER);
    }
}

/* A looped-back character reaches the receiver; the walk goes on from then. */
static void sdma_loop_arrived(SGIIOCSerialDMAPort *p)
{
    int64_t when = p->loop_due;

    p->loop_due = 0;
    timer_del(p->loop_timer);
    sgi_ioc_sdma_run_from(p, when);
}

static void sdma_loop_expired(void *opaque)
{
    SGIIOCSerialDMAPort *p = opaque;

    if (p->loop_due) {
        sdma_loop_arrived(p);
    }
}

/*
 * An expiry already due happens before an access, not whenever the main
 * loop next services the timer. The count field is read off the clock, so
 * without this a guest can read a spent count while the source it raises
 * is still clear: the IO-8 board test loads a two-tick reload, waits 10 ms
 * and requires the source set. A loopback arrival already due goes first,
 * since it is what starts the timer.
 */
void sgi_ioc_sdma_catch_up(SGIIOCSerialDMA *d)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned i;

    for (i = 0; i < d->nports; i++) {
        SGIIOCSerialDMAPort *p = &d->port[i];

        /* Each arrival can send the next character; the ring bounds it. */
        while (p->loop_due && p->loop_due <= now) {
            sdma_loop_arrived(p);
        }

        if (timer_pending(p->rx_timer) &&
            timer_expire_time_ns(p->rx_timer) <= now) {
            timer_del(p->rx_timer);
            sdma_rx_timer_expired(p);
        }
    }
}

/*
 * A level, recomputed wherever the waiting count moves: a latched raise
 * storms IRIX's ring console. The mark counts ring entries: both Linux ring
 * drivers set it to three quarters of a ring's entries and compare it, shifted
 * to a byte offset, with the distance between the indices, and IRIX's ring
 * console programs 384 against 4 KiB rings of 512 entries.
 */
static void sdma_update_high_water(SGIIOCSerialDMAPort *p)
{
    uint32_t *produce = sdma_reg(p, SGI_IOC_SDMA_RX_PRODUCE);
    uint32_t *consume = sdma_reg(p, SGI_IOC_SDMA_RX_CONSUME);
    uint32_t mask = sdma_pointer_mask(p);
    uint32_t high_water = sdma_control(p) & SGI_IOC_SDMA_HIGH_WATER;
    uint32_t waiting = ((*produce - *consume) & mask) /
                       SGI_IOC_SDMA_ENTRY_SIZE;

    sdma_set_level(p, SGI_IOC_SDMA_SRC_RX_HIGH,
                   high_water && waiting >= high_water);
}

/*
 * End of burst: a short group posts now rather than on timer expiry; the
 * interrupt still waits for the timer, the timing drivers observe.
 */
static void sdma_flush(SGIIOCSerialDMAPort *p, int64_t since)
{
    uint32_t *produce = sdma_reg(p, SGI_IOC_SDMA_RX_PRODUCE);
    uint32_t *consume = sdma_reg(p, SGI_IOC_SDMA_RX_CONSUME);
    uint32_t mask = sdma_pointer_mask(p);

    /* A looped-back character still on the line continues the burst. */
    if (p->loop_due) {
        sdma_update_high_water(p);
        return;
    }

    if (p->packed) {
        sdma_post(p, produce, consume, mask);
    }

    sdma_update_high_water(p);

    if (!p->delivered) {
        return;
    }
    p->delivered = false;

    if (p->armed) {
        sdma_rx_timer_start(p, since);
    }
}

static void sdma_receive(SGIIOCSerialDMAPort *p)
{
    uint32_t *produce = sdma_reg(p, SGI_IOC_SDMA_RX_PRODUCE);
    uint32_t *consume = sdma_reg(p, SGI_IOC_SDMA_RX_CONSUME);
    uint32_t mask = sdma_pointer_mask(p);
    SerialState *serial = p->uart;

    /*
     * The 16550 core loops a character back the moment it is written; on
     * the part it takes a character time on the line to arrive.
     */
    if (p->loop_due) {
        return;
    }

    while (sdma_running(p)) {
        uint8_t lsr;
        uint8_t msr;
        uint8_t status;

        /* On refusal the drain stops without dropping anything. */
        if (p->packed == SGI_IOC_SDMA_ENTRY_CHARS &&
            !sdma_post(p, produce, consume, mask)) {
            break;
        }

        lsr = serial_io_ops.read(serial, UART_LSR, 1);
        if (!(lsr & UART_LSR_DR)) {
            break;
        }

        /* LSR before RBR: reading LSR clears the latches it describes. */
        p->packer[p->packed] = serial_io_ops.read(serial, UART_RBR_THR, 1);
        msr = serial_io_ops.read(serial, UART_MSR, 1);
        if (sgi_ioc_sdma_flow_cts(p->engine, p->index)) {
            msr |= UART_MSR_CTS;
        }

        status = SGI_IOC_SDMA_RX_DATA_OK | SGI_IOC_SDMA_RX_MODEM_OK;
        if (lsr & UART_LSR_OE) {
            status |= SGI_IOC_SDMA_RX_OVERRUN;
        }
        if (lsr & UART_LSR_PE) {
            status |= SGI_IOC_SDMA_RX_PARITY;
        }
        if (lsr & UART_LSR_FE) {
            status |= SGI_IOC_SDMA_RX_FRAMING;
        }
        if (lsr & UART_LSR_BI) {
            status |= SGI_IOC_SDMA_RX_BREAK;
        }
        if (msr & UART_MSR_CTS) {
            status |= SGI_IOC_SDMA_RX_CTS;
        }
        if (msr & UART_MSR_DCD) {
            status |= SGI_IOC_SDMA_RX_CD;
        }
        p->packer[SGI_IOC_SDMA_ENTRY_CHARS + p->packed] = status;

        ++p->packed;
    }
}

/*
 * Each control byte types its data byte; the empty type ends the entry, and
 * sending past it puts memory garbage on the wire.
 */
static void sdma_transmit(SGIIOCSerialDMAPort *p, int64_t since)
{
    SGIIOCSerialDMA *d = p->engine;
    uint32_t *produce = sdma_reg(p, SGI_IOC_SDMA_TX_PRODUCE);
    uint32_t *consume = sdma_reg(p, SGI_IOC_SDMA_TX_CONSUME);
    uint32_t mask = sdma_pointer_mask(p);
    SerialState *serial = p->uart;
    uint64_t ring = sdma_ring(p, SGI_IOC_SDMA_RING_TX);
    bool walked = false;

    while (sdma_running(p) && (*consume & mask) != (*produce & mask)) {
        uint8_t *entry = p->tx_entry;
        unsigned i;
        bool loop;

        /*
         * An entry once fetched stays with the engine until its last slot
         * goes, as the unpacker count in the consume index implies. A
         * refused fetch stops the walk: the buffer is not an entry.
         */
        if (!p->tx_fetched) {
            if (dma_memory_read(d->dma_as, ring + (*consume & mask), entry,
                                SGI_IOC_SDMA_ENTRY_SIZE,
                                MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                sdma_raise(p, SGI_IOC_SDMA_SRC_DMA_ERROR);
                break;
            }
            p->tx_fetched = true;
        }

        for (i = p->tx_slot; i < SGI_IOC_SDMA_ENTRY_CHARS; i++) {
            uint8_t control = entry[SGI_IOC_SDMA_ENTRY_CHARS + i];
            uint8_t type = control & SGI_IOC_SDMA_TX_TYPE;

            /* Wherever it sits: the slots behind it are not sent. */
            if (type == SGI_IOC_SDMA_TX_NOTHING) {
                break;
            }

            switch (type) {
            case SGI_IOC_SDMA_TX_CHARACTER:
                /* In loopback the line carries one character at a time. */
                if (!p->tx_ready || p->loop_due) {
                    return;
                }
                loop = serial->mcr & UART_MCR_LOOP;
                /* What has already arrived goes ahead of this one. */
                if (loop) {
                    sdma_receive(p);
                }
                serial_io_ops.write(serial, UART_RBR_THR, entry[i], 1);
                if (d->ops->transmitted) {
                    d->ops->transmitted(d->opaque, p->index, entry[i]);
                }
                if (loop) {
                    p->loop_due = since + serial->char_transmit_time;
                    timer_mod(p->loop_timer, p->loop_due);
                }
                /* Drain per character or loopback overruns the 16-byte FIFO. */
                sdma_receive(p);
                break;
            case SGI_IOC_SDMA_TX_MODEM:
                serial_io_ops.write(serial, UART_MCR, entry[i], 1);
                break;
            case SGI_IOC_SDMA_TX_DELAY:
                break;
            }

            p->tx_slot = i + 1;

            if (control & SGI_IOC_SDMA_TX_INTERRUPT) {
                sdma_raise(p, SGI_IOC_SDMA_SRC_TX_EXPLICIT);
            }
        }

        p->tx_slot = 0;
        p->tx_fetched = false;
        *consume = (*consume + SGI_IOC_SDMA_ENTRY_SIZE) & mask;
        walked = true;
    }

    /* On reaching the end of a walk, not on every idle engine. */
    if (walked && (*consume & mask) == (*produce & mask)) {
        sdma_raise(p, SGI_IOC_SDMA_SRC_TX_EMPTY);
    }
}

/* `since` is when the walk starts: now, or when a loopback arrival was due. */
static void sgi_ioc_sdma_run_from(SGIIOCSerialDMAPort *p, int64_t since)
{
    SGIIOCSerialDMA *d = p->engine;

    /* The walk drives a 16550 the board may not have fitted. */
    if (!p->uart || !d->ops->reachable(d->opaque, p->index)) {
        return;
    }

    /* Loopback re-enters; a second walk would post a half-filled entry. */
    if (p->busy) {
        return;
    }
    p->busy = true;

    sdma_transmit(p, since);
    sdma_receive(p);
    sdma_flush(p, since);

    p->busy = false;
}

void sgi_ioc_sdma_kick(SGIIOCSerialDMA *d, unsigned port)
{
    sgi_ioc_sdma_run_from(&d->port[port],
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

void sgi_ioc_sdma_set_tx_ready(SGIIOCSerialDMA *d, unsigned port, bool level)
{
    d->port[port].tx_ready = level;
    if (level) {
        sgi_ioc_sdma_kick(d, port);
    }
}

/* Resets the four indices and nothing else; the settings survive. */
static void sdma_reset_port(SGIIOCSerialDMAPort *p)
{
    hwaddr reg;

    for (reg = SGI_IOC_SDMA_TX_PRODUCE; reg <= SGI_IOC_SDMA_RX_CONSUME;
         reg += 4) {
        *sdma_reg(p, reg) = 0;
    }

    p->armed = false;
    p->packed = 0;
    memset(p->packer, 0, SGI_IOC_SDMA_ENTRY_SIZE);
    p->delivered = false;
    p->tx_slot = 0;
    p->tx_fetched = false;

    timer_del(p->rx_timer);
    p->rx_idle_count = 0;
}

/*
 * A passive mirror read from the register file's fields: the byte-wide read
 * ops clear the latches and this view must not.
 */
static uint32_t sdma_shadow(SGIIOCSerialDMAPort *p)
{
    SerialState *serial = p->uart;
    uint8_t lsr;
    uint8_t mcr;
    uint8_t msr;
    uint32_t shadow;

    if (!serial) {
        return 0;
    }
    lsr = serial->lsr;
    mcr = serial->mcr;

    /* In loopback the modem-control outputs wire back to status. */
    if (mcr & UART_MCR_LOOP) {
        msr = ((mcr & 0x0c) << 4) | ((mcr & 0x02) << 3) | ((mcr & 0x01) << 5);
    } else {
        msr = serial->msr;
    }
    if (sgi_ioc_sdma_flow_cts(p->engine, p->index)) {
        msr |= UART_MSR_CTS;
    }

    shadow = lsr & SGI_IOC_SDMA_SHADOW_LSR_MASK;

    if (msr & UART_MSR_DCTS) {
        shadow |= SGI_IOC_SDMA_SHADOW_DELTA_CTS;
    }
    if (msr & UART_MSR_DDCD) {
        shadow |= SGI_IOC_SDMA_SHADOW_DELTA_CD;
    }
    if (msr & UART_MSR_CTS) {
        shadow |= SGI_IOC_SDMA_SHADOW_CTS;
    }
    if (msr & UART_MSR_DCD) {
        shadow |= SGI_IOC_SDMA_SHADOW_CD;
    }
    if (mcr & UART_MCR_DTR) {
        shadow |= SGI_IOC_SDMA_SHADOW_DTR;
    }
    if (mcr & UART_MCR_RTS) {
        shadow |= SGI_IOC_SDMA_SHADOW_RTS;
    }
    if (mcr & UART_MCR_OUT1) {
        shadow |= SGI_IOC_SDMA_SHADOW_OUT1;
    }
    if (mcr & UART_MCR_OUT2) {
        shadow |= SGI_IOC_SDMA_SHADOW_OUT2;
    }
    if (mcr & UART_MCR_LOOP) {
        shadow |= SGI_IOC_SDMA_SHADOW_LOOPBACK;
    }
    return shadow;
}

uint32_t sgi_ioc_sdma_port_read(SGIIOCSerialDMA *d, unsigned port,
                                hwaddr offset)
{
    SGIIOCSerialDMAPort *p = &d->port[port];
    uint32_t val;

    sgi_ioc_sdma_catch_up(d);

    if (offset == SGI_IOC_SDMA_SHADOW) {
        return sdma_shadow(p);
    }

    val = *sdma_reg(p, offset);

    if (offset == SGI_IOC_SDMA_CONTROL && (val & SGI_IOC_SDMA_PAUSE)) {
        val |= SGI_IOC_SDMA_PAUSED;
    }

    /* The consume index reports the arm, which a write can only set. */
    if (offset == SGI_IOC_SDMA_RX_CONSUME && p->armed) {
        val |= SGI_IOC_SDMA_ARM_RX_TIMER;
    }

    /* The produce index also reports the packer's held count. */
    if (offset == SGI_IOC_SDMA_RX_PRODUCE) {
        val |= (uint32_t)p->packed << SGI_IOC_SDMA_HELD_SHIFT;
    }

    /*
     * Ticks left while running. Idle, the count is whatever a reload write
     * last loaded, until the timer runs or the channel resets.
     */
    if (offset == SGI_IOC_SDMA_RX_TIMER) {
        uint32_t ticks = p->rx_idle_count;

        if (timer_pending(p->rx_timer)) {
            int64_t left = timer_expire_time_ns(p->rx_timer) -
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

            ticks = MIN(MAX(left, 0) / SGI_IOC_SDMA_TIMER_TICK_NS,
                        SGI_IOC_SDMA_TIMER_RELOAD);
        }
        val |= ticks << SGI_IOC_SDMA_TIMER_COUNT_SHIFT;
    }

    return val;
}

void sgi_ioc_sdma_port_write(SGIIOCSerialDMA *d, unsigned port,
                             hwaddr offset, uint32_t val)
{
    SGIIOCSerialDMAPort *p = &d->port[port];
    uint32_t mask = sdma_pointer_mask(p);

    /* A write that stops or rewinds the timer comes after an expiry due. */
    sgi_ioc_sdma_catch_up(d);

    switch (offset) {
    case SGI_IOC_SDMA_CONTROL:
        *sdma_reg(p, offset) = val & ~SGI_IOC_SDMA_CTRL_UNSTORED;
        if (val & SGI_IOC_SDMA_RESET) {
            sdma_reset_port(p);
        }
        if (!(val & SGI_IOC_SDMA_ENABLE)) {
            timer_del(p->rx_timer);
        }
        /*
         * The drain bit is a command IRIX polls to zero, and enabling is a
         * start: the firmware hands the ring over before enabling.
         */
        sgi_ioc_sdma_kick(d, port);
        return;

    case SGI_IOC_SDMA_TX_PRODUCE:
        *sdma_reg(p, offset) = val & mask;
        sgi_ioc_sdma_kick(d, port);
        return;

    case SGI_IOC_SDMA_RX_CONSUME: {
        uint32_t *consume = sdma_reg(p, offset);
        uint32_t *produce;

        /*
         * Bit 31 sets the arm and a write with it clear leaves the arm
         * alone: both Linux ring drivers arm as a read starts and end the
         * read with bit 31 clear, under a comment calling that write a
         * re-arm, and IRIX's ring console does the same.
         */
        *consume = val & mask;
        if (val & SGI_IOC_SDMA_ARM_RX_TIMER) {
            p->armed = true;
        }
        sgi_ioc_sdma_kick(d, port);

        /* Arming over a non-empty ring rewinds the timer. */
        produce = sdma_reg(p, SGI_IOC_SDMA_RX_PRODUCE);
        if ((val & SGI_IOC_SDMA_ARM_RX_TIMER) &&
            ((*produce ^ *consume) & mask)) {
            sdma_rx_timer_start(p, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        }
        return;
    }

    case SGI_IOC_SDMA_RX_TIMER:
        *sdma_reg(p, offset) = val & SGI_IOC_SDMA_TIMER_RELOAD;
        /* The write loads the count as well; a running timer keeps its own. */
        p->rx_idle_count = val & SGI_IOC_SDMA_TIMER_RELOAD;
        return;

    case SGI_IOC_SDMA_SHADOW:
        /* Discarded silently: OpenBSD writes zero here on every boot. */
        return;

    /*
     * The engine advances these two, and software can still place them: the
     * board test writes each and reads it back. Only the pointer field
     * stores, so a write cannot forge the held counts the engine reports in
     * the bits above it.
     */
    case SGI_IOC_SDMA_TX_CONSUME:
    case SGI_IOC_SDMA_RX_PRODUCE:
        *sdma_reg(p, offset) = val & mask;
        sgi_ioc_sdma_kick(d, port);
        return;
    }
}

uint32_t sgi_ioc_sdma_base_read(SGIIOCSerialDMA *d, unsigned pair, bool high)
{
    return high ? d->base_hi[pair] : d->base_lo[pair];
}

void sgi_ioc_sdma_base_write(SGIIOCSerialDMA *d, unsigned pair, bool high,
                             uint32_t val)
{
    if (high) {
        d->base_hi[pair] = val;
    } else {
        d->base_lo[pair] = val;
    }
}

void sgi_ioc_sdma_init(SGIIOCSerialDMA *d, unsigned nports,
                       const SGIIOCSerialDMAOps *ops, void *opaque,
                       AddressSpace *dma_as)
{
    unsigned i;

    assert(nports <= SGI_IOC_SDMA_MAX_PORTS);
    d->ops = ops;
    d->opaque = opaque;
    d->dma_as = dma_as;
    d->nports = nports;

    /*
     * Timers for every port whatever the board fits: reset deletes them all
     * and migration carries them all.
     */
    for (i = 0; i < nports; i++) {
        SGIIOCSerialDMAPort *p = &d->port[i];

        p->engine = d;
        p->index = i;
        p->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sdma_rx_timer_expired,
                                   p);
        p->loop_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sdma_loop_expired, p);
    }
}

void sgi_ioc_sdma_attach_uart(SGIIOCSerialDMA *d, unsigned port,
                              SerialState *uart)
{
    d->port[port].uart = uart;
}

void sgi_ioc_sdma_finalize(SGIIOCSerialDMA *d)
{
    unsigned i;

    for (i = 0; i < d->nports; i++) {
        timer_free(d->port[i].rx_timer);
        timer_free(d->port[i].loop_timer);
        d->port[i].rx_timer = NULL;
        d->port[i].loop_timer = NULL;
    }
}

void sgi_ioc_sdma_reset(SGIIOCSerialDMA *d)
{
    unsigned i;

    memset(d->base_hi, 0, sizeof(d->base_hi));
    memset(d->base_lo, 0, sizeof(d->base_lo));
    for (i = 0; i < d->nports; i++) {
        SGIIOCSerialDMAPort *p = &d->port[i];

        memset(p->reg, 0, sizeof(p->reg));
        timer_del(p->rx_timer);
        p->rx_idle_count = 0;
        /* The 16550's own reset empties the FIFO the character sits in. */
        timer_del(p->loop_timer);
        p->loop_due = 0;
        p->tx_slot = 0;
        p->tx_fetched = false;
        p->packed = 0;
        memset(p->packer, 0, SGI_IOC_SDMA_ENTRY_SIZE);
        p->delivered = false;
        p->armed = false;
    }
}

static int sdma_port_pre_load(void *opaque)
{
    SGIIOCSerialDMAPort *p = opaque;

    /* A stream without the loopback subsection has nothing on the line. */
    p->loop_due = 0;
    return 0;
}

static bool sdma_loop_needed(void *opaque)
{
    SGIIOCSerialDMAPort *p = opaque;

    return p->loop_due != 0;
}

/* The due time is the state; the timer is only its wake-up. */
static int sdma_loop_post_load(void *opaque, int version_id)
{
    SGIIOCSerialDMAPort *p = opaque;

    if (p->loop_due) {
        timer_mod(p->loop_timer, p->loop_due);
    } else {
        timer_del(p->loop_timer);
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_ioc_sdma_loop = {
    .name = "sgi-ioc-sdma-port/loop",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sdma_loop_needed,
    .post_load = sdma_loop_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(loop_due, SGIIOCSerialDMAPort),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sgi_ioc_sdma_port = {
    .name = "sgi-ioc-sdma-port",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = sdma_port_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, SGIIOCSerialDMAPort, SGI_IOC_SDMA_PORT_REGS),
        VMSTATE_UINT8(tx_slot, SGIIOCSerialDMAPort),
        VMSTATE_BOOL(tx_fetched, SGIIOCSerialDMAPort),
        VMSTATE_UINT8_ARRAY(tx_entry, SGIIOCSerialDMAPort,
                            SGI_IOC_SDMA_ENTRY_SIZE),
        VMSTATE_UINT8_ARRAY(packer, SGIIOCSerialDMAPort,
                            SGI_IOC_SDMA_ENTRY_SIZE),
        VMSTATE_UINT8(packed, SGIIOCSerialDMAPort),
        VMSTATE_TIMER_PTR(rx_timer, SGIIOCSerialDMAPort),
        VMSTATE_UINT16(rx_idle_count, SGIIOCSerialDMAPort),
        VMSTATE_BOOL(armed, SGIIOCSerialDMAPort),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_ioc_sdma_loop,
        NULL
    }
};

const VMStateDescription vmstate_sgi_ioc_sdma = {
    .name = "sgi-ioc-sdma",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(base_hi, SGIIOCSerialDMA, SGI_IOC_SDMA_MAX_PAIRS),
        VMSTATE_UINT32_ARRAY(base_lo, SGIIOCSerialDMA, SGI_IOC_SDMA_MAX_PAIRS),
        VMSTATE_STRUCT_VARRAY_UINT32(port, SGIIOCSerialDMA, nports, 0,
                                     vmstate_sgi_ioc_sdma_port,
                                     SGIIOCSerialDMAPort),
        VMSTATE_END_OF_LIST()
    }
};
