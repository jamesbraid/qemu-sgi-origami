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
#include "chardev/char-fe.h"
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
static void sdma_update_high_water(SGIIOCSerialDMAPort *p);

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

/*
 * Transmit-ring-empty is a level: set while ring DMA is on and nothing is
 * left to send, neither in the ring nor held by the engine. Linux's IOC3 ring
 * driver says a write can clear it without the handler, re-reads it after a
 * write, and stops it by masking the source rather than acknowledging it.
 * Requiring ring DMA keeps the status register's reset value of zero.
 */
static void sdma_update_tx_empty(SGIIOCSerialDMAPort *p)
{
    uint32_t mask = sdma_pointer_mask(p);
    bool empty = (sdma_control(p) & SGI_IOC_SDMA_ENABLE) &&
                 !((*sdma_reg(p, SGI_IOC_SDMA_TX_PRODUCE) ^
                    *sdma_reg(p, SGI_IOC_SDMA_TX_CONSUME)) & mask) &&
                 !p->tx_fetched && !p->tx_delay_due;

    sdma_set_level(p, SGI_IOC_SDMA_SRC_TX_EMPTY, empty);
}

static bool sdma_running(SGIIOCSerialDMAPort *p)
{
    uint32_t control = sdma_control(p);

    return !p->halted && (control & SGI_IOC_SDMA_ENABLE) &&
           !(control & SGI_IOC_SDMA_PAUSE);
}

/* A transfer the fabric refused: the chip reports it, and may halt. */
static void sdma_dma_failed(SGIIOCSerialDMAPort *p, bool rx, hwaddr addr)
{
    SGIIOCSerialDMA *d = p->engine;

    if (d->params->halt_on_dma_error) {
        p->halted = true;
    }
    if (d->ops->dma_error) {
        d->ops->dma_error(d->opaque, p->index, rx, addr);
    } else {
        sdma_raise(p, SGI_IOC_SDMA_SRC_DMA_ERROR);
    }
}

static MemTxResult sdma_dma_rw(SGIIOCSerialDMA *d, hwaddr addr, void *buf,
                               dma_addr_t len, DMADirection dir)
{
    MemTxResult result;

    d->in_flight++;
    result = dma_memory_rw(d->dma_as, addr, buf, len, dir,
                           MEMTXATTRS_UNSPECIFIED);
    d->in_flight--;
    return result;
}

bool sgi_ioc_sdma_idle(SGIIOCSerialDMA *d)
{
    return !d->in_flight;
}

/* Post the packer as one entry; unfilled slots carry no validity flags. */
static bool sdma_post(SGIIOCSerialDMAPort *p, uint32_t *produce,
                      uint32_t *consume, uint32_t mask)
{
    uint64_t ring = sdma_ring(p, SGI_IOC_SDMA_RING_RX);
    uint32_t next = (*produce + SGI_IOC_SDMA_ENTRY_SIZE) & mask;

    /* Full: refuse; the characters keep in the packer and the FIFO. */
    if (next == (*consume & mask)) {
        p->post_refused = true;
        return false;
    }

    /* A refused write must not advance the index past a missing entry. */
    if (sdma_dma_rw(p->engine, ring + (*produce & mask), p->packer,
                    SGI_IOC_SDMA_ENTRY_SIZE, DMA_DIRECTION_FROM_DEVICE) !=
        MEMTX_OK) {
        sdma_dma_failed(p, true, ring + (*produce & mask));
        return false;
    }
    *produce = next;
    p->post_refused = false;
    p->packed = 0;
    memset(p->packer, 0, SGI_IOC_SDMA_ENTRY_SIZE);
    return true;
}

/*
 * The reload counts 16 kHz ticks from the character that started the timer,
 * which `since` places: a loopback arrival serviced late still counts from
 * when it reached the receiver.
 */
static void sdma_rx_timer_start(SGIIOCSerialDMAPort *p, int64_t since)
{
    uint32_t reload = *sdma_reg(p, SGI_IOC_SDMA_RX_TIMER) &
                      SGI_IOC_SDMA_TIMER_RELOAD;

    /* Once it has run, the count it held from the reload write is spent. */
    p->rx_idle_count = 0;

    /* Started while paused, it waits frozen at its full count. */
    if (p->paused) {
        p->rx_frozen_due = p->paused_at +
                           (int64_t)reload * SGI_IOC_SDMA_TIMER_TICK_NS;
        return;
    }
    timer_mod(p->rx_timer,
              since + (int64_t)reload * SGI_IOC_SDMA_TIMER_TICK_NS);
}

/*
 * Expiry posts what the packer holds, raises its source and spends the arm:
 * both Linux ring drivers re-arm as each read starts, and their comment's
 * account of characters left in the packer only holds if the arm is gone
 * once the timer has run, "after the interrupt was generated". It raises
 * even when software has already drained the ring: spending the arm with no
 * interrupt leaves later characters waiting with no timer and no reader,
 * and IRIX 6.5.30's ring console stalls on exactly that.
 */
static void sdma_rx_timer_expired(void *opaque)
{
    SGIIOCSerialDMAPort *p = opaque;
    uint32_t *produce = sdma_reg(p, SGI_IOC_SDMA_RX_PRODUCE);
    uint32_t *consume = sdma_reg(p, SGI_IOC_SDMA_RX_CONSUME);
    uint32_t mask = sdma_pointer_mask(p);

    p->rx_idle_count = 0;

    /* A still-packed entry means the ring was full; retry now. */
    if (p->packed) {
        sdma_post(p, produce, consume, mask);
    }

    if (p->armed) {
        sdma_raise(p, SGI_IOC_SDMA_SRC_RX_TIMER);
    }
    p->armed = false;
    sdma_update_high_water(p);
}

/* A transmit delay has run out; the walk goes on from when it ended. */
static void sdma_tx_delay_ended(SGIIOCSerialDMAPort *p)
{
    int64_t when = p->tx_delay_due;

    p->tx_delay_due = 0;
    timer_del(p->tx_delay_timer);
    if (p->tx_delay_irq) {
        p->tx_delay_irq = false;
        sdma_raise(p, SGI_IOC_SDMA_SRC_TX_EXPLICIT);
    }
    sgi_ioc_sdma_run_from(p, when);
}

static void sdma_tx_delay_expired(void *opaque)
{
    SGIIOCSerialDMAPort *p = opaque;

    if (p->tx_delay_due) {
        sdma_tx_delay_ended(p);
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

        if (p->tx_delay_due && !p->paused && p->tx_delay_due <= now) {
            sdma_tx_delay_ended(p);
        }

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

    /*
     * Receive-ring-full is a level too: set while ring DMA is on and an
     * entry waits, turned away for lack of ring room. It does not come back
     * with every refused retry after software has acknowledged it, and it
     * falls once the consumer makes room, whether or not the held entry has
     * posted yet. A ring filled exactly, with nothing waiting, does not
     * raise it. A model choice; what the part does is not known.
     */
    if (((*produce + SGI_IOC_SDMA_ENTRY_SIZE) & mask) != (*consume & mask)) {
        p->post_refused = false;
    }
    sdma_set_level(p, SGI_IOC_SDMA_SRC_RX_FULL,
                   (sdma_control(p) & SGI_IOC_SDMA_ENABLE) && p->post_refused);
}

/*
 * The receive timer starts at the first character received after it is
 * armed, as both Linux ring drivers' comment on the part says. A running
 * timer is not started again by later characters; whether the part restarts
 * it is not known.
 */
static void sdma_receive(SGIIOCSerialDMAPort *p, int64_t since)
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

        if (p->armed && !timer_pending(p->rx_timer) && !p->rx_frozen_due) {
            sdma_rx_timer_start(p, since);
        }

        /*
         * A fourth character completes the entry. Fewer stay in the packer
         * until the timer runs out or a drain: both Linux ring drivers'
         * comment on the part says held bytes are not written "until
         * either there are 4 bytes available or the rx_timer expires".
         */
        if (p->packed == SGI_IOC_SDMA_ENTRY_CHARS) {
            sdma_post(p, produce, consume, mask);
        }
    }
}

/*
 * The type field and bit 5 decide what a slot does; the low five bits have
 * no defined meaning and are reported, not obeyed. Report them once, when
 * the entry is fetched, for the slots up to the one that ends it.
 */
static void sdma_check_entry(SGIIOCSerialDMAPort *p, const uint8_t *entry)
{
    uint8_t undefined = 0;
    unsigned i;

    for (i = 0; i < SGI_IOC_SDMA_ENTRY_CHARS; i++) {
        uint8_t control = entry[SGI_IOC_SDMA_ENTRY_CHARS + i];

        undefined |= control & SGI_IOC_SDMA_TX_UNDEFINED;
        if ((control & SGI_IOC_SDMA_TX_TYPE) == SGI_IOC_SDMA_TX_NOTHING) {
            break;
        }
    }
    if (undefined) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc-serial-dma: undefined TX control bits 0x%02x "
                      "in an entry on port %u\n", undefined, p->index);
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

    /* A delay in progress holds every slot behind it. */
    if (p->tx_delay_due) {
        return;
    }

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
            if (sdma_dma_rw(d, ring + (*consume & mask), entry,
                            SGI_IOC_SDMA_ENTRY_SIZE,
                            DMA_DIRECTION_TO_DEVICE) != MEMTX_OK) {
                sdma_dma_failed(p, false, ring + (*consume & mask));
                break;
            }
            p->tx_fetched = true;
            sdma_check_entry(p, entry);
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
                    sdma_receive(p, since);
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
                sdma_receive(p, since);
                break;
            case SGI_IOC_SDMA_TX_MODEM:
                serial_io_ops.write(serial, UART_MCR, entry[i], 1);
                break;
            case SGI_IOC_SDMA_TX_DELAY:
                /*
                 * The data byte is milliseconds of quiet; an explicit
                 * interrupt asked of it comes when it ends.
                 */
                if (entry[i]) {
                    p->tx_slot = i + 1;
                    p->tx_delay_due = since + entry[i] * SCALE_MS;
                    p->tx_delay_irq = control & SGI_IOC_SDMA_TX_INTERRUPT;
                    timer_mod(p->tx_delay_timer, p->tx_delay_due);
                    return;
                }
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
    }
}

/*
 * The drain posts whatever the packer holds and reads back set until it
 * has: both Linux ring drivers poll it to clear, enable ring DMA first
 * because "DMA must be enabled for this to work", and skip the wait without
 * ring room because "otherwise the DRAIN will never complete". So it waits
 * while ring DMA is off or paused and while the ring is full.
 */
static void sdma_drain(SGIIOCSerialDMAPort *p)
{
    if (!p->drain_pending || !sdma_running(p)) {
        return;
    }
    if (p->packed &&
        !sdma_post(p, sdma_reg(p, SGI_IOC_SDMA_RX_PRODUCE),
                   sdma_reg(p, SGI_IOC_SDMA_RX_CONSUME),
                   sdma_pointer_mask(p))) {
        return;
    }
    p->drain_pending = false;
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
    sdma_receive(p, since);
    sdma_drain(p);
    sdma_update_high_water(p);
    sdma_update_tx_empty(p);

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

/* Resets the indices and the work in progress; the settings survive. */
static void sdma_reset_port(SGIIOCSerialDMAPort *p)
{
    hwaddr reg;

    /* Recovering a port after a failed transfer is what this is for. */
    p->halted = false;

    for (reg = SGI_IOC_SDMA_TX_PRODUCE; reg <= SGI_IOC_SDMA_RX_CONSUME;
         reg += 4) {
        *sdma_reg(p, reg) = 0;
    }

    p->armed = false;
    p->drain_pending = false;
    p->packed = 0;
    p->post_refused = false;
    memset(p->packer, 0, SGI_IOC_SDMA_ENTRY_SIZE);
    p->tx_slot = 0;
    p->tx_fetched = false;
    timer_del(p->tx_delay_timer);
    p->tx_delay_due = 0;
    p->tx_delay_irq = false;

    timer_del(p->rx_timer);
    p->rx_frozen_due = 0;
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
    if (offset == SGI_IOC_SDMA_CONTROL && p->tx_delay_due) {
        val |= SGI_IOC_SDMA_TX_TIMER_RUN;
    }
    if (offset == SGI_IOC_SDMA_CONTROL && p->drain_pending) {
        val |= SGI_IOC_SDMA_RX_TO_MEMORY;
    }

    /* The consume index reports the arm, which a write can only set. */
    if (offset == SGI_IOC_SDMA_RX_CONSUME && p->armed) {
        val |= SGI_IOC_SDMA_ARM_RX_TIMER;
    }

    /* The produce index also reports the packer's held count. */
    if (offset == SGI_IOC_SDMA_RX_PRODUCE && d->params->held_counts) {
        val |= (uint32_t)p->packed << SGI_IOC_SDMA_HELD_SHIFT;
    }

    /*
     * Ticks left while running, or when a pause froze it. Idle, the count is
     * whatever a reload write last loaded, until the timer runs or the
     * channel resets.
     */
    if (offset == SGI_IOC_SDMA_RX_TIMER) {
        uint32_t ticks = p->rx_idle_count;

        if (p->paused && p->rx_frozen_due) {
            ticks = MIN((p->rx_frozen_due - p->paused_at) /
                        SGI_IOC_SDMA_TIMER_TICK_NS,
                        SGI_IOC_SDMA_TIMER_RELOAD);
        } else if (timer_pending(p->rx_timer)) {
            int64_t left = timer_expire_time_ns(p->rx_timer) -
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

            ticks = MIN(MAX(left, 0) / SGI_IOC_SDMA_TIMER_TICK_NS,
                        SGI_IOC_SDMA_TIMER_RELOAD);
        }
        val |= ticks << SGI_IOC_SDMA_TIMER_COUNT_SHIFT;
    }

    return val;
}

/*
 * Pausing freezes the receive timer and a transmit delay where they stand;
 * resuming lets each finish the time it had left. No source settles what
 * the part does: this is the model's choice.
 */
static void sdma_freeze(SGIIOCSerialDMAPort *p, int64_t now)
{
    p->paused = true;
    p->paused_at = now;
    p->rx_frozen_due = 0;
    if (timer_pending(p->rx_timer)) {
        p->rx_frozen_due = MAX(timer_expire_time_ns(p->rx_timer), now);
        timer_del(p->rx_timer);
    }
    timer_del(p->tx_delay_timer);
}

static void sdma_thaw(SGIIOCSerialDMAPort *p, int64_t now)
{
    int64_t held = now - p->paused_at;

    p->paused = false;
    p->paused_at = 0;
    if (p->rx_frozen_due) {
        timer_mod(p->rx_timer, p->rx_frozen_due + held);
        p->rx_frozen_due = 0;
    }
    if (p->tx_delay_due) {
        p->tx_delay_due += held;
        timer_mod(p->tx_delay_timer, p->tx_delay_due);
    }
}

static void sdma_port_write(SGIIOCSerialDMA *d, unsigned port,
                             hwaddr offset, uint32_t val)
{
    SGIIOCSerialDMAPort *p = &d->port[port];
    uint32_t mask = sdma_pointer_mask(p);

    /* A write that stops or rewinds the timer comes after an expiry due. */
    sgi_ioc_sdma_catch_up(d);

    switch (offset) {
    case SGI_IOC_SDMA_CONTROL: {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        *sdma_reg(p, offset) = val & ~SGI_IOC_SDMA_CTRL_UNSTORED;
        if (val & SGI_IOC_SDMA_RESET) {
            sdma_reset_port(p);
        }
        if (val & SGI_IOC_SDMA_RX_TO_MEMORY) {
            p->drain_pending = true;
        }
        if ((val & SGI_IOC_SDMA_PAUSE) && !p->paused) {
            sdma_freeze(p, now);
        } else if (!(val & SGI_IOC_SDMA_PAUSE) && p->paused) {
            sdma_thaw(p, now);
        }
        if (!(val & SGI_IOC_SDMA_ENABLE)) {
            timer_del(p->rx_timer);
            p->rx_frozen_due = 0;
        }
        /* Enabling is a start: the firmware hands the ring over first. */
        sgi_ioc_sdma_kick(d, port);
        return;
    }

    case SGI_IOC_SDMA_TX_PRODUCE:
        *sdma_reg(p, offset) = val & mask;
        sgi_ioc_sdma_kick(d, port);
        return;

    case SGI_IOC_SDMA_RX_CONSUME: {
        uint32_t *consume = sdma_reg(p, offset);

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
        /* Arming starts nothing: the next character received does. */
        sgi_ioc_sdma_kick(d, port);
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

void sgi_ioc_sdma_port_write(SGIIOCSerialDMA *d, unsigned port,
                             hwaddr offset, uint32_t val)
{
    sdma_port_write(d, port, offset, val);
    /* Enabling, a doorbell or a reset can change it without a walk. */
    sdma_update_tx_empty(&d->port[port]);
    sdma_update_high_water(&d->port[port]);
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
                       const SGIIOCSerialDMAParams *params,
                       const SGIIOCSerialDMAOps *ops, void *opaque,
                       AddressSpace *dma_as)
{
    unsigned i;

    assert(nports <= SGI_IOC_SDMA_MAX_PORTS);
    d->params = params;
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
        p->tx_delay_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                         sdma_tx_delay_expired, p);
    }
}

/*
 * With modem-control bit 4 set a PC16550D disconnects its serial input, so
 * a character arriving from outside in loopback goes nowhere; QEMU's 16550
 * core would take it. The engine sits between the port's character backend
 * and its UART and discards such input on arrival, leaving looped-back
 * characters alone. The 16550 core's own handlers still do the rest. When
 * the discard happens is an approximation: on arrival from the host, not a
 * character time on the line later.
 */
static bool sdma_uart_looped(SGIIOCSerialDMAPort *p)
{
    return p->uart->mcr & UART_MCR_LOOP;
}

static int sdma_chr_can_read(void *opaque)
{
    SGIIOCSerialDMAPort *p = opaque;

    if (sdma_uart_looped(p)) {
        return 1;
    }
    return p->saved_can_read ? p->saved_can_read(p->saved_opaque) : 0;
}

static void sdma_chr_read(void *opaque, const uint8_t *buf, int size)
{
    SGIIOCSerialDMAPort *p = opaque;

    if (sdma_uart_looped(p)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc-serial-dma: port %u discarded %d character(s) "
                      "from outside while in loopback\n", p->index, size);
        return;
    }
    if (p->saved_read) {
        p->saved_read(p->saved_opaque, buf, size);
    }
}

static void sdma_chr_event(void *opaque, QEMUChrEvent event)
{
    SGIIOCSerialDMAPort *p = opaque;

    if (p->saved_event) {
        p->saved_event(p->saved_opaque, event);
    }
}

static int sdma_chr_be_change(void *opaque);

static void sdma_interpose_chr(SGIIOCSerialDMAPort *p)
{
    CharFrontend *chr = &p->uart->chr;

    p->saved_can_read = chr->chr_can_read;
    p->saved_read = chr->chr_read;
    p->saved_event = chr->chr_event;
    p->saved_be_change = chr->chr_be_change;
    p->saved_opaque = chr->opaque;
    qemu_chr_fe_set_handlers(chr, sdma_chr_can_read, sdma_chr_read,
                             sdma_chr_event, sdma_chr_be_change, p, NULL,
                             true);
}

/* A new backend: the 16550 core installs its handlers again, then us. */
static int sdma_chr_be_change(void *opaque)
{
    SGIIOCSerialDMAPort *p = opaque;

    if (p->saved_be_change && p->saved_be_change(p->saved_opaque) < 0) {
        return -1;
    }
    sdma_interpose_chr(p);
    return 0;
}

void sgi_ioc_sdma_attach_uart(SGIIOCSerialDMA *d, unsigned port,
                              SerialState *uart)
{
    SGIIOCSerialDMAPort *p = &d->port[port];

    p->uart = uart;
    /*
     * A port realized without a backend has no chardev for chardev-change
     * to replace, so it never gains one later and needs no wrapper. A
     * backend that chardev-change swaps out keeps the wrapper through
     * sdma_chr_be_change.
     */
    if (qemu_chr_fe_backend_connected(&uart->chr)) {
        sdma_interpose_chr(p);
    }
}

void sgi_ioc_sdma_finalize(SGIIOCSerialDMA *d)
{
    unsigned i;

    for (i = 0; i < d->nports; i++) {
        timer_free(d->port[i].rx_timer);
        timer_free(d->port[i].loop_timer);
        timer_free(d->port[i].tx_delay_timer);
        d->port[i].rx_timer = NULL;
        d->port[i].loop_timer = NULL;
        d->port[i].tx_delay_timer = NULL;
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
        timer_del(p->tx_delay_timer);
        p->tx_delay_due = 0;
        p->tx_delay_irq = false;
        p->paused = false;
        p->paused_at = 0;
        p->rx_frozen_due = 0;
        p->packed = 0;
        p->post_refused = false;
        memset(p->packer, 0, SGI_IOC_SDMA_ENTRY_SIZE);
        p->armed = false;
        p->drain_pending = false;
        p->halted = false;
    }
}

static int sdma_port_pre_load(void *opaque)
{
    SGIIOCSerialDMAPort *p = opaque;

    /* A stream without the loopback subsection has nothing on the line. */
    p->loop_due = 0;
    return 0;
}

/* The due time is the state; the timer is only its wake-up. */
static int sdma_port_post_load(void *opaque, int version_id)
{
    SGIIOCSerialDMAPort *p = opaque;

    if (p->tx_delay_due && !p->paused) {
        timer_mod(p->tx_delay_timer, p->tx_delay_due);
    } else {
        timer_del(p->tx_delay_timer);
    }
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
    .post_load = sdma_port_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, SGIIOCSerialDMAPort, SGI_IOC_SDMA_PORT_REGS),
        VMSTATE_UINT8(tx_slot, SGIIOCSerialDMAPort),
        VMSTATE_BOOL(tx_fetched, SGIIOCSerialDMAPort),
        VMSTATE_UINT8_ARRAY(tx_entry, SGIIOCSerialDMAPort,
                            SGI_IOC_SDMA_ENTRY_SIZE),
        VMSTATE_UINT8_ARRAY(packer, SGIIOCSerialDMAPort,
                            SGI_IOC_SDMA_ENTRY_SIZE),
        VMSTATE_UINT8(packed, SGIIOCSerialDMAPort),
        VMSTATE_BOOL(post_refused, SGIIOCSerialDMAPort),
        VMSTATE_TIMER_PTR(rx_timer, SGIIOCSerialDMAPort),
        VMSTATE_UINT16(rx_idle_count, SGIIOCSerialDMAPort),
        VMSTATE_BOOL(armed, SGIIOCSerialDMAPort),
        VMSTATE_BOOL(drain_pending, SGIIOCSerialDMAPort),
        VMSTATE_BOOL(halted, SGIIOCSerialDMAPort),
        VMSTATE_INT64(tx_delay_due, SGIIOCSerialDMAPort),
        VMSTATE_BOOL(tx_delay_irq, SGIIOCSerialDMAPort),
        VMSTATE_BOOL(paused, SGIIOCSerialDMAPort),
        VMSTATE_INT64(paused_at, SGIIOCSerialDMAPort),
        VMSTATE_INT64(rx_frozen_due, SGIIOCSerialDMAPort),
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
