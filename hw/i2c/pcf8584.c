/*
 * Philips PCF8584 I2C-bus controller
 *
 * Copyright (c) 2026 Origami contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Modelled from the register contract in the Origami project tree's
 * controller datasheet; see also docs/specs/sgi-elsc.rst.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/cpu.h"
#include "hw/i2c/pcf8584.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "trace.h"

/*
 * Which register the A0-low address selects. ES2 keeps selecting even with
 * the serial interface enabled; only ES1 stops meaning anything.
 */
typedef enum {
    PCF8584_REG_OWN_ADDRESS,
    PCF8584_REG_CLOCK,
    PCF8584_REG_VECTOR,
    PCF8584_REG_DATA,
} PCF8584Reg;

static uint64_t pcf8584_diagnostic_state(PCF8584State *s)
{
    return ((uint64_t)s->control << 56) |
           ((uint64_t)s->status << 48) |
           ((uint64_t)s->data << 40) |
           ((uint64_t)s->cur_addr7 << 32) |
           ((uint64_t)s->transfer << 3) |
           ((uint64_t)s->recv << 2) |
           ((uint64_t)s->start_pending << 1) |
           s->start_address_staged;
}

static PCF8584Reg pcf8584_selected(PCF8584State *s)
{
    if ((s->control & PCF8584_CTL_ES2) && !(s->control & PCF8584_CTL_ES1)) {
        return PCF8584_REG_VECTOR;
    }
    if (s->control & PCF8584_CTL_ESO) {
        return (s->control & PCF8584_CTL_ES2) ? PCF8584_REG_VECTOR
                                              : PCF8584_REG_DATA;
    }
    if (s->control & PCF8584_CTL_ES1) {
        return PCF8584_REG_CLOCK;
    }
    return PCF8584_REG_OWN_ADDRESS;
}

static void pcf8584_update_irq(PCF8584State *s)
{
    /* PIN is active low in the chip's naming: clear means completed. */
    bool level = (s->control & PCF8584_CTL_ENI) &&
                 !(s->status & PCF8584_STS_PIN);

    qemu_set_irq(s->irq, level);
}

/* Mark the byte transfer finished, recording whether the far end acked. */
static void pcf8584_complete(PCF8584State *s, bool nacked)
{
    s->status &= ~PCF8584_STS_PIN;
    if (nacked) {
        s->status |= PCF8584_STS_LRB;
    } else {
        s->status &= ~PCF8584_STS_LRB;
    }
    pcf8584_update_irq(s);
}

/*
 * The wire's timing. Firmware programs a 12 MHz input with the 90 kHz
 * bus-speed select and nothing else, so that is the only rate modelled. The
 * period is split evenly: a deterministic approximation from the published
 * nominal rate, not a measured duty cycle, since the specification calls the
 * selected rates approximate and fixes no duty cycle. Both halves clear the
 * published 4.7 us low and 4.0 us high minimums, and one high interval also
 * serves as the START setup and hold.
 */
#define PCF8584_SCL_LOW_NS      5556
#define PCF8584_SCL_HIGH_NS     5556
/* Published minimum bus-free interval ahead of a fresh START. */
#define PCF8584_BUS_FREE_NS     4700

static bool pcf8584_on_wire(PCF8584State *s)
{
    return s->wire_master != PCF8584_NO_WIRE;
}

static int64_t pcf8584_now(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static int pcf8584_current_cpu_index(void)
{
    return current_cpu ? current_cpu->cpu_index : -1;
}

/*
 * Stage one line transition. The virtual clock keeps running while an event
 * is being delivered, so a deadline derived from that event's own timestamp
 * can already have passed by the time it is requested. The wire applies the
 * floor from the same clock sample it validates against.
 */
static void pcf8584_wire_stage(PCF8584State *s, int64_t at, bool sda,
                               bool clock)
{
    int rc = i2c_wire_stage_at_or_after(s->bus, s->wire_master, at, 1,
                                        sda, clock);

    if (rc) {
        qemu_log_mask(LOG_UNIMP, "pcf8584: the shared wire refused a line "
                      "transition (%d); this transfer is abandoned\n", rc);
        s->wire_phase = PCF8584_WIRE_IDLE;
        s->wire_active = false;
    }
}

/* What this adapter drives on SDA for the clock it is about to run. */
static bool pcf8584_wire_bit(PCF8584State *s)
{
    if (s->wire_bit >= 8) {
        /* Clock nine: a transmitter releases SDA, a receiver drives ACK. */
        return s->wire_address_byte || !s->recv ||
               !(s->control & PCF8584_CTL_ACK);
    }
    if (s->recv && !s->wire_address_byte) {
        return true;            /* the target drives the data bits */
    }
    return (s->wire_shift >> (7 - s->wire_bit)) & 1;
}

/* A fresh START: SDA falls with both lines released, after the free time. */
static void pcf8584_wire_begin_start_at(PCF8584State *s, int64_t at)
{
    uint8_t addr = s->start_address_staged ? s->start_address : s->data;

    s->start_address_staged = false;
    s->wire_shift = addr;
    s->recv = addr & 1;
    s->cur_addr7 = addr >> 1;
    s->wire_address_byte = true;
    s->wire_active = true;
    s->wire_phase = PCF8584_WIRE_START_SDA;
    /* Staging floors this at the present, so an elapsed free time is now. */
    pcf8584_wire_stage(s, at, false, false);
}

static void pcf8584_wire_begin_start(PCF8584State *s)
{
    pcf8584_wire_begin_start_at(s, s->wire_free_at);
}

/*
 * A repeated START keeps the bus: the clock resumes so SCL rises with SDA
 * released, and the SDA fall that follows is placed while it is still high.
 */
static void pcf8584_wire_begin_restart(PCF8584State *s, uint8_t addr)
{
    if (s->wire_phase != PCF8584_WIRE_PAUSED) {
        qemu_log_mask(LOG_UNIMP, "pcf8584: repeated START part way through a "
                      "byte is unsupported\n");
        return;
    }
    s->wire_shift = addr;
    s->recv = addr & 1;
    s->cur_addr7 = addr >> 1;
    s->wire_address_byte = true;
    s->wire_phase = PCF8584_WIRE_RESTART_SCL;
    pcf8584_wire_stage(s, pcf8584_now() + 1, true, true);
}

/*
 * Resume the paused clock into the next byte. The pause left SCL low, so the
 * resume is that bit's rising edge and its SDA is sampled there.
 */
static void pcf8584_wire_next_byte(PCF8584State *s, uint8_t shift)
{
    s->wire_shift = shift;
    s->wire_recv_byte = 0;
    s->wire_address_byte = false;
    s->wire_bit = 0;
    s->wire_phase = PCF8584_WIRE_BITS;
    s->status |= PCF8584_STS_PIN;
    pcf8584_wire_stage(s, pcf8584_now() + 1, pcf8584_wire_bit(s), true);
}

static void pcf8584_wire_request_stop(PCF8584State *s)
{
    if (!s->wire_active) {
        /*
         * Not the owner. A losing or reset non-owner must not end the
         * winner's transfer, so only local state moves.
         */
        s->start_pending = false;
        s->start_address_staged = false;
        s->wire_start_waiting = false;
        return;
    }
    if (s->wire_phase != PCF8584_WIRE_PAUSED) {
        qemu_log_mask(LOG_UNIMP, "pcf8584: STOP requested part way through a "
                      "byte is unsupported\n");
        return;
    }
    trace_pcf8584_stop();
    s->start_pending = false;
    s->wire_phase = PCF8584_WIRE_STOP_SCL;
    pcf8584_wire_stage(s, pcf8584_now() + 1, false, true);
}

static void pcf8584_wire_release_hold(PCF8584State *s)
{
    s->wire_hold_release = PCF8584_HOLD_NONE;
    if (i2c_wire_hold_after(s->rx, 1, false)) {
        qemu_log_mask(LOG_UNIMP, "pcf8584: the shared wire refused this "
                      "adapter's clock release\n");
    }
}

/*
 * BER: the datasheet makes a misplaced START or STOP report PIN active and
 * BB free, so BB stops following the line until a PIN write clears BER.
 */
static void pcf8584_wire_bus_error(PCF8584State *s)
{
    s->status |= PCF8584_STS_BER | PCF8584_STS_BB;
    s->status &= ~PCF8584_STS_PIN;
    s->wire_active = false;
    s->wire_phase = PCF8584_WIRE_IDLE;
    s->start_pending = false;
    if (i2c_wire_reset_master(s->bus, s->wire_master) == -ENOTSUP) {
        qemu_log_mask(LOG_UNIMP, "pcf8584: recovery from a misplaced START or "
                      "STOP while driving is unmodelled\n");
    }
    pcf8584_update_irq(s);
}

/* The ninth clock has fallen: the byte, its acknowledgement included, is in. */
static void pcf8584_wire_byte_done(PCF8584State *s)
{
    bool nacked = s->wire_ack_nak;

    if (s->wire_address_byte) {
        trace_pcf8584_start(s->cur_addr7, s->recv, !nacked);
    } else if (s->recv) {
        /* This adapter drove clock nine, so nothing was received on it. */
        s->data = s->wire_recv_byte;
        nacked = false;
        trace_pcf8584_recv(s->data);
    } else {
        trace_pcf8584_send(s->wire_shift, !nacked);
    }
    /*
     * Between bytes the chip keeps SCL low and waits for its host rather
     * than releasing the line, so the clock stops without a rising edge.
     */
    if (i2c_wire_pause_master(s->bus, s->wire_master)) {
        qemu_log_mask(LOG_UNIMP, "pcf8584: the shared wire refused the "
                      "between-byte clock hold\n");
    }
    s->wire_phase = PCF8584_WIRE_PAUSED;
    pcf8584_complete(s, nacked);
}

static void pcf8584_wire_event(void *opaque, const I2CWireEvent *event)
{
    PCF8584State *s = opaque;
    bool was_scl = s->wire_scl, was_sda = s->wire_sda;
    bool start = was_scl && event->scl && was_sda && !event->sda;
    bool stop = was_scl && event->scl && !was_sda && event->sda;
    bool start_overtaken = stop && s->wire_start_waiting &&
                           s->wire_grant_pending;
    bool rise = !was_scl && event->scl;
    bool fall = was_scl && !event->scl;
    int64_t now = event->time;

    s->wire_scl = event->scl;
    s->wire_sda = event->sda;

    if (event->error) {
        qemu_log_mask(LOG_UNIMP, "pcf8584: the shared wire stopped with %d; "
                      "no further transfers are represented\n", event->error);
        s->wire_phase = PCF8584_WIRE_IDLE;
        s->wire_active = false;
        s->wire_grant_pending = false;
        s->wire_start_waiting = false;
        return;
    }
    if (event->lost & (1ULL << s->wire_master)) {
        /*
         * LAB reports the loss at the mismatching bit. PIN stays inactive
         * until the winning byte finishes after its acknowledgement clock,
         * so the loser keeps counting the winner's clocks out.
         */
        s->status |= PCF8584_STS_LAB;
        s->wire_active = false;
        s->start_pending = false;
        s->wire_phase = PCF8584_WIRE_LOST;
    }

    if ((start || stop) && (s->wire_phase == PCF8584_WIRE_BITS ||
                            s->wire_phase == PCF8584_WIRE_ACK)) {
        pcf8584_wire_bus_error(s);
        return;
    }
    if (stop && s->wire_grant_pending) {
        trace_pcf8584_grant_publish(s->wire_master,
                                    pcf8584_current_cpu_index(),
                                    s->wire_grant, s->status, s->wire_phase,
                                    s->bus->owner,
                                    i2c_wire_contenders(s->bus));
        s->data = s->wire_grant;
        s->wire_grant_pending = false;
    }
    if (stop) {
        s->wire_free_at = now + PCF8584_BUS_FREE_NS;
    }

    switch (s->wire_phase) {
    case PCF8584_WIRE_START_SDA:
        if (!start) {
            break;
        }
        if (!(i2c_wire_contenders(s->bus) & (1ULL << s->wire_master))) {
            /*
             * A peer opened the bus in the interval between the host's
             * control write and this adapter's own line transition. That is
             * the busy-START case again, which the chip sources leave
             * undefined. Preserve the request as the busy-START path does,
             * including its address before a module grant replaces S0 at
             * STOP.
             */
            s->start_address = s->wire_shift;
            s->start_address_staged = true;
            s->wire_start_waiting = true;
            s->wire_phase = PCF8584_WIRE_IDLE;
            s->wire_active = false;
            s->wire_address_byte = false;
            i2c_wire_reset_master(s->bus, s->wire_master);
            break;
        }
        s->wire_bit = 0;
        s->wire_phase = PCF8584_WIRE_START_HOLD;
        pcf8584_wire_stage(s, now + PCF8584_SCL_HIGH_NS,
                           pcf8584_wire_bit(s), true);
        break;
    case PCF8584_WIRE_START_HOLD:
        if (fall) {
            s->wire_phase = PCF8584_WIRE_BITS;
        }
        break;
    case PCF8584_WIRE_BITS:
        if (rise && s->recv && !s->wire_address_byte) {
            s->wire_recv_byte = (s->wire_recv_byte << 1) | event->sda;
        } else if (fall) {
            s->wire_bit++;
            if (s->wire_bit >= 8) {
                s->wire_phase = PCF8584_WIRE_ACK;
            }
            pcf8584_wire_stage(s, now + 1, pcf8584_wire_bit(s), true);
        }
        break;
    case PCF8584_WIRE_ACK:
        if (rise) {
            s->wire_ack_nak = event->sda;
        } else if (fall) {
            pcf8584_wire_byte_done(s);
        }
        break;
    case PCF8584_WIRE_RESTART_SCL:
        if (rise) {
            s->wire_phase = PCF8584_WIRE_START_SDA;
            pcf8584_wire_stage(s, now + PCF8584_SCL_HIGH_NS, false, false);
        }
        break;
    case PCF8584_WIRE_STOP_SCL:
        if (rise) {
            s->wire_phase = PCF8584_WIRE_STOP_SDA;
            pcf8584_wire_stage(s, now + PCF8584_SCL_HIGH_NS, true, false);
        }
        break;
    case PCF8584_WIRE_STOP_SDA:
        if (stop) {
            s->wire_phase = PCF8584_WIRE_IDLE;
            s->wire_active = false;
            s->wire_address_byte = false;
        }
        break;
    case PCF8584_WIRE_LOST:
        if (rise && s->wire_bit >= 8) {
            s->wire_ack_nak = event->sda;
        } else if (fall && s->wire_bit >= 8) {
            s->wire_phase = PCF8584_WIRE_IDLE;
            s->wire_address_byte = false;
            pcf8584_complete(s, s->wire_ack_nak);
        } else if (fall) {
            s->wire_bit++;
        }
        break;
    default:
        break;
    }

    /* Status follows the line: START clears BB and STOP sets it. */
    if (!(s->status & PCF8584_STS_BER)) {
        if (i2c_bus_busy(s->bus)) {
            s->status &= ~PCF8584_STS_BB;
        } else {
            s->status |= PCF8584_STS_BB;
        }
    }
    if (stop && s->wire_start_waiting) {
        s->wire_start_waiting = false;
        if (start_overtaken) {
            /*
             * A later grant overtook this queued START.  Report arbitration
             * loss so software repeats its grant wait instead of running the
             * transaction in another processor's slot.
             */
            s->start_address_staged = false;
            s->status |= PCF8584_STS_LAB;
            pcf8584_complete(s, true);
            return;
        }
        /*
         * Several adapters can have queued the same controller address.
         * Let one lower-numbered adapter open the bus first; the peer START
         * event returns every later adapter to this queue for the next STOP.
         */
        pcf8584_wire_begin_start_at(s, s->wire_free_at + s->wire_master);
    }
}

static void pcf8584_wire_write_data(PCF8584State *s, uint8_t val)
{
    if (s->wire_hold_release == PCF8584_HOLD_WRITE) {
        /* Addressed as a transmitter: the byte written releases our hold. */
        s->data = val;
        pcf8584_wire_release_hold(s);
        return;
    }
    if (s->start_pending) {
        /* A repeated start's address arrives after its control write. */
        s->start_pending = false;
        s->data = val;
        pcf8584_wire_begin_restart(s, val);
        return;
    }
    if (!s->wire_active) {
        /*
         * No transfer of ours is open, so this is the address staged ahead
         * of a first start. A peer occupying the bus is not our transfer.
         */
        s->data = val;
        s->start_address = val;
        s->start_address_staged = true;
        return;
    }
    if (s->recv) {
        qemu_log_mask(LOG_GUEST_ERROR, "pcf8584: data written during a "
                      "receive transfer, ignored\n");
        return;
    }
    if (s->wire_phase != PCF8584_WIRE_PAUSED) {
        qemu_log_mask(LOG_GUEST_ERROR, "pcf8584: data written while a byte "
                      "is still on the bus, ignored\n");
        return;
    }
    s->data = val;
    pcf8584_wire_next_byte(s, val);
}

static uint8_t pcf8584_wire_read_data(PCF8584State *s)
{
    uint8_t val = s->data;

    trace_pcf8584_data_read(s->wire_master, pcf8584_current_cpu_index(),
                            val, s->wire_grant, s->status, s->wire_phase,
                            s->wire_active,
                            i2c_bus_busy(s->bus), s->bus->owner,
                            i2c_wire_contenders(s->bus));

    if (s->wire_hold_release == PCF8584_HOLD_READ) {
        /* Addressed as a receiver: the read releases our own clock hold. */
        pcf8584_wire_release_hold(s);
        return val;
    }
    if (s->wire_active && s->recv && !s->wire_ack_nak &&
        s->wire_phase == PCF8584_WIRE_PAUSED) {
        /* An acknowledged read advances the pipeline to the next byte. */
        if (!(s->control & PCF8584_CTL_ACK)) {
            trace_pcf8584_nack();
        }
        pcf8584_wire_next_byte(s, 0);
    }
    return val;
}

static void pcf8584_wire_request_start(PCF8584State *s)
{
    trace_pcf8584_start_request(s->wire_master,
                                pcf8584_current_cpu_index(), s->wire_grant,
                                s->wire_grant_pending, s->status,
                                s->wire_phase, s->wire_active,
                                i2c_bus_busy(s->bus), s->bus->owner,
                                i2c_wire_contenders(s->bus));
    if (s->wire_active) {
        /* Repeated start: the address byte has not been written yet. */
        s->start_pending = true;
        s->status |= PCF8584_STS_PIN;
        return;
    }
    s->status |= PCF8584_STS_PIN;
    if (i2c_bus_busy(s->bus)) {
        /*
         * Compatibility approximation: silicon behaviour is unknown when
         * software requests an initial START after its free-bus observation
         * has gone stale. Preserve the request until STOP so host scheduling
         * cannot break the module-controller grant rendezvous.
         */
        if (!s->start_address_staged) {
            s->start_address = s->data;
            s->start_address_staged = true;
        }
        s->wire_start_waiting = true;
        return;
    }
    pcf8584_wire_begin_start(s);
}

static void pcf8584_stop(PCF8584State *s)
{
    trace_pcf8584_stop();
    if (s->transfer) {
        i2c_end_transfer(s->bus);
    }
    s->transfer = false;
    s->recv = false;
    s->start_pending = false;
    s->start_address_staged = false;
    s->status |= PCF8584_STS_BB;
}

/* Issue a start with addr as the address-and-direction byte. */
static void pcf8584_start(PCF8584State *s, uint8_t addr)
{
    bool recv = addr & 1;
    uint8_t addr7 = addr >> 1;
    int rc;

    s->start_address_staged = false;

    /*
     * The core rescans only when nothing is matched, so an address change
     * must end the old match here; unconditionally would deliver I2C_FINISH
     * to a slave still mid-command.
     */
    if (s->transfer && addr7 != s->cur_addr7) {
        i2c_end_transfer(s->bus);
        s->transfer = false;
    }
    s->cur_addr7 = addr7;

    s->recv = recv;
    s->status &= ~PCF8584_STS_BB;

    rc = i2c_start_transfer(s->bus, addr7, recv);
    if (rc && s->transfer) {
        /*
         * A refused same-address repeated start is left open by the core for
         * the caller to end; held open it keeps i2c_bus_busy() true forever.
         */
        i2c_end_transfer(s->bus);
    }
    s->transfer = rc == 0;
    trace_pcf8584_start(addr7, recv, rc == 0);

    if (rc) {
        /* Normal: firmware probes an invalid address in order to be refused. */
        pcf8584_complete(s, true);
        return;
    }

    /* No fetch for recv: the transfer attaches to the read, not the start. */
    pcf8584_complete(s, false);
}

static void pcf8584_write_data(PCF8584State *s, uint8_t val)
{
    int rc;

    if (pcf8584_on_wire(s)) {
        pcf8584_wire_write_data(s, val);
        return;
    }

    if (s->start_pending) {
        s->start_pending = false;
        pcf8584_start(s, val);
        return;
    }

    if (s->status & PCF8584_STS_BB) {
        /* Bus free: the address byte staged ahead of the control write. */
        s->data = val;
        s->start_address = val;
        s->start_address_staged = true;
        return;
    }

    if (s->recv) {
        qemu_log_mask(LOG_GUEST_ERROR, "pcf8584: data written during a "
                      "receive transfer, ignored\n");
        return;
    }

    s->status |= PCF8584_STS_PIN;
    if (s->transfer) {
        rc = i2c_send(s->bus, val);
    } else {
        /*
         * The start was refused. i2c_send() over the core's empty matched
         * list would report success, so the refusal is remembered instead.
         */
        rc = -1;
    }
    trace_pcf8584_send(val, rc == 0);
    s->data = val;
    pcf8584_complete(s, rc != 0);
}

static uint8_t pcf8584_read_data(PCF8584State *s)
{
    uint8_t val;

    if (pcf8584_on_wire(s)) {
        return pcf8584_wire_read_data(s);
    }

    val = s->data;
    if (s->transfer && s->recv) {
        /* Held byte first, then fetch: fetch-first is off by one all packet. */
        if (!(s->control & PCF8584_CTL_ACK)) {
            /* ACK cleared means the driver wants this to be the last byte. */
            trace_pcf8584_nack();
            i2c_nack(s->bus);
        }
        s->data = i2c_recv(s->bus);
        trace_pcf8584_recv(s->data);
        pcf8584_complete(s, false);
    }

    return val;
}

static void pcf8584_write_control(PCF8584State *s, uint8_t val)
{
    uint8_t prev = s->control;

    s->control = val;

    if (val & PCF8584_CTL_PIN) {
        /*
         * PIN from the host is the idle request. It is also written alone to
         * select a configuration register, so it must not disturb a transfer.
         */
        s->status |= PCF8584_STS_PIN;
        s->status &= ~(PCF8584_STS_BER | PCF8584_STS_LAB | PCF8584_STS_AAS);
    }

    if (val & PCF8584_CTL_STA) {
        if (pcf8584_on_wire(s)) {
            pcf8584_wire_request_start(s);
        } else if (!(s->status & PCF8584_STS_BB)) {
            /* Repeated start: the address byte has not been written yet. */
            s->start_pending = true;
            s->status |= PCF8584_STS_PIN;
        } else {
            uint8_t addr = s->start_address_staged ? s->start_address :
                                                        s->data;

            /* First start: the address is already staged in the register. */
            s->status |= PCF8584_STS_PIN;
            pcf8584_start(s, addr);
        }
        return;
    }

    if (val & PCF8584_CTL_STO) {
        if (pcf8584_on_wire(s)) {
            pcf8584_wire_request_stop(s);
        } else {
            pcf8584_stop(s);
        }
        s->status |= PCF8584_STS_PIN;
        pcf8584_update_irq(s);
        return;
    }

    if (pcf8584_on_wire(s) && s->wire_start_waiting &&
        (prev & PCF8584_CTL_ESO) && !(val & PCF8584_CTL_ESO)) {
        s->wire_start_waiting = false;
        s->start_address_staged = false;
    }

    if ((prev ^ val) & PCF8584_CTL_ENI) {
        pcf8584_update_irq(s);
    }
}

static uint64_t pcf8584_read_internal(void *opaque, hwaddr addr,
                                      unsigned size)
{
    PCF8584State *s = PCF8584(opaque);

    if (addr >= PCF8584_A0_STRIDE) {
        /*
         * Write-only in the documentation with the serial interface off;
         * real parts answer with the control bits plus the PIN state.
         */
        if (s->control & PCF8584_CTL_ESO) {
            return s->status;
        }
        return (s->control & ~PCF8584_STS_PIN) | (s->status & PCF8584_STS_PIN);
    }

    switch (pcf8584_selected(s)) {
    case PCF8584_REG_OWN_ADDRESS:
        return s->own_address;
    case PCF8584_REG_CLOCK:
        return s->clock;
    case PCF8584_REG_VECTOR:
        return s->vector;
    case PCF8584_REG_DATA:
        return pcf8584_read_data(s);
    default:
        g_assert_not_reached();
    }
}

static void pcf8584_write_internal(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    PCF8584State *s = PCF8584(opaque);

    if (addr >= PCF8584_A0_STRIDE) {
        pcf8584_write_control(s, val & 0xff);
        return;
    }

    switch (pcf8584_selected(s)) {
    case PCF8584_REG_OWN_ADDRESS:
        /*
         * Seven bits, right-justified; stored as written so the firmware's
         * read-back liveness check sees what it wrote.
         */
        s->own_address = val & 0x7f;
        break;
    case PCF8584_REG_CLOCK:
        s->clock = val & 0x1f;
        break;
    case PCF8584_REG_VECTOR:
        s->vector = val & 0xff;
        break;
    case PCF8584_REG_DATA:
        pcf8584_write_data(s, val & 0xff);
        break;
    default:
        g_assert_not_reached();
    }
}

static uint64_t pcf8584_read(void *opaque, hwaddr addr, unsigned size)
{
    PCF8584State *s = PCF8584(opaque);
    uint64_t before;
    uint64_t value;
    uint64_t after;

    if (likely(!s->diagnostic)) {
        value = pcf8584_read_internal(opaque, addr, size);
        trace_pcf8584_read(addr, value, size);
        return value;
    }

    before = pcf8584_diagnostic_state(s);
    value = pcf8584_read_internal(opaque, addr, size);
    after = pcf8584_diagnostic_state(s);

    trace_pcf8584_read(addr, value, size);
    device_diagnostic_record(s->diagnostic, "pcf8584", "mmio-read",
                             addr, value, size, true, before, after);
    return value;
}

static void pcf8584_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    PCF8584State *s = PCF8584(opaque);
    uint64_t before;
    uint64_t after;

    trace_pcf8584_write(addr, val, size);

    if (likely(!s->diagnostic)) {
        pcf8584_write_internal(opaque, addr, val, size);
        return;
    }

    before = pcf8584_diagnostic_state(s);
    pcf8584_write_internal(opaque, addr, val, size);
    after = pcf8584_diagnostic_state(s);
    device_diagnostic_record(s->diagnostic, "pcf8584", "mmio-write",
                             addr, val, size, true, before, after);
}

uint64_t pcf8584_read_register(PCF8584State *s, hwaddr addr, unsigned size)
{
    return pcf8584_read(s, addr, size);
}

void pcf8584_write_register(PCF8584State *s, hwaddr addr, uint64_t value,
                            unsigned size)
{
    pcf8584_write(s, addr, value, size);
}

/*
 * impl.max is 8: the default of 4 splits the firmware's eight-byte accesses
 * and runs the data register's read side effect twice.
 */
static const MemoryRegionOps pcf8584_ops = {
    .read = pcf8584_read,
    .write = pcf8584_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
};

/*
 * The chip's slave half: catches broadcasts and directed hits on S0',
 * reported through AAS. It adds nothing of its own to what the I2C slave
 * underneath it already migrates.
 */
#define TYPE_PCF8584_BCAST_RX "pcf8584-broadcast-rx"
OBJECT_DECLARE_SIMPLE_TYPE(PCF8584BcastRxState, PCF8584_BCAST_RX)

struct PCF8584BcastRxState {
    I2CSlave parent_obj;
    PCF8584State *chip;
    /*
     * Set at match time: the current match is a directed hit on the
     * programmed address, not the general call. Consulted synchronously by
     * the event that follows the same start, so it is transient and never
     * migrated.
     */
    bool directed;
};

/* Consulted at match time, so reset and migration never copy it here. */
static bool pcf8584_bcast_rx_match_and_add(I2CSlave *candidate,
                                           uint8_t address, bool broadcast,
                                           I2CNodeList *current_devs)
{
    PCF8584BcastRxState *rx = PCF8584_BCAST_RX(candidate);
    I2CNode *node;

    if (!broadcast && address != rx->chip->own_address) {
        return false;
    }

    rx->directed = !broadcast;
    node = g_new(struct I2CNode, 1);
    node->elt = candidate;
    QLIST_INSERT_HEAD(current_devs, node, next);
    return true;
}

static int pcf8584_bcast_rx_event(I2CSlave *dev, enum i2c_event event)
{
    PCF8584BcastRxState *rx = PCF8584_BCAST_RX(dev);

    /* Only a directed hit on the programmed address raises AAS. */
    if (rx->directed && (event == I2C_START_SEND || event == I2C_START_RECV)) {
        rx->chip->status |= PCF8584_STS_AAS;
    }
    return 0;
}

static int pcf8584_bcast_rx_send(I2CSlave *dev, uint8_t data)
{
    PCF8584BcastRxState *rx = PCF8584_BCAST_RX(dev);

    rx->chip->data = data;
    return 0;
}

static uint8_t pcf8584_bcast_rx_recv(I2CSlave *dev)
{
    return PCF8584_BCAST_RX(dev)->chip->data;
}

static void pcf8584_observe_address(PCF8584State *s, uint8_t address_byte)
{
    if (!(s->control & PCF8584_CTL_ESO) || !s->own_address ||
        (address_byte >> 1) == s->own_address ||
        address_byte == I2C_BROADCAST) {
        return;
    }
    s->data = address_byte;
}

static void pcf8584_bcast_rx_unmatched_address(I2CSlave *dev,
                                                uint8_t address, bool recv)
{
    PCF8584State *s = PCF8584_BCAST_RX(dev)->chip;
    uint8_t address_byte = (address << 1) | recv;

    if (address_byte >= 0xe0 && address_byte <= 0xef) {
        pcf8584_observe_address(s, address_byte);
    }
}

/* PIN goes active only after all nine clocks, the acknowledgement included. */
static void pcf8584_service_pin(PCF8584State *s)
{
    s->status &= ~PCF8584_STS_PIN;
    pcf8584_update_irq(s);
}

/*
 * Addressed service on the shared wire. Each adapter owns its own clock hold
 * and keeps it until its own host moves the byte, so servicing one adapter
 * cannot release another's.
 */
static bool pcf8584_bcast_rx_wire_service(I2CSlave *dev,
                                          enum i2c_wire_service service)
{
    PCF8584State *s = PCF8584_BCAST_RX(dev)->chip;

    if (s->wire_active) {
        /*
         * This adapter is driving the transfer. Its master half already owns
         * the status and the clock, and stretching SCL against itself would
         * stall the transfer it is in the middle of.
         */
        return false;
    }

    switch (service) {
    case I2C_WIRE_ADDRESS_SEND:
    case I2C_WIRE_WRITE:
        s->wire_hold_release = PCF8584_HOLD_READ;
        pcf8584_service_pin(s);
        return true;
    case I2C_WIRE_ADDRESS_RECV:
        s->wire_hold_release = PCF8584_HOLD_WRITE;
        pcf8584_service_pin(s);
        return true;
    case I2C_WIRE_READ_ACK:
        /* As a transmitter the acknowledgement bit is a received one. */
        s->wire_hold_release = PCF8584_HOLD_WRITE;
        pcf8584_complete(s, false);
        return true;
    case I2C_WIRE_READ_NACK:
        /* The master is done with us and drives STOP or a repeated START. */
        s->wire_hold_release = PCF8584_HOLD_NONE;
        pcf8584_complete(s, true);
        return false;
    default:
        return false;
    }
}

/*
 * A software-required approximation, not measured PCF8584 behaviour: the chip
 * sources guarantee passive buffering only with own address zero, while the
 * module grant protocol needs the last unmatched address readable from a
 * nonzero-address adapter. A timed module grant becomes readable at its STOP,
 * once firmware can safely act on it, and remains through idle until a later
 * completed address phase replaces it. Observing is not selection -- no
 * acknowledgement, no addressed-as-slave, no PIN, no interrupt and no clock
 * hold -- and only address phases are copied here, never payload.
 */
static void pcf8584_bcast_rx_wire_address(I2CSlave *dev, uint8_t address_byte)
{
    PCF8584State *s = PCF8584_BCAST_RX(dev)->chip;

    if (address_byte >= 0xe0 && address_byte <= 0xef &&
        (s->control & PCF8584_CTL_ESO) && s->own_address &&
        (address_byte >> 1) != s->own_address) {
        s->wire_grant = address_byte;
        s->wire_grant_pending = true;
        return;
    }
    s->wire_grant_pending = false;
    pcf8584_observe_address(s, address_byte);
}

/*
 * The endpoint carries no state of its own worth moving; what it must move
 * is the I2C slave underneath it. A shared wire finishes loading only once
 * every slave on the bus has restored, counted by their post-load hooks, and
 * a slave with no migration state runs none: the count would never complete,
 * and the wire would come up on the destination with its clock stopped.
 */
static const VMStateDescription vmstate_pcf8584_bcast_rx = {
    .name = "pcf8584-broadcast-rx",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, PCF8584BcastRxState),
        VMSTATE_END_OF_LIST()
    },
};

static void pcf8584_bcast_rx_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_pcf8584_bcast_rx;
    sc->supports_master_ids = true;
    sc->match_and_add = pcf8584_bcast_rx_match_and_add;
    sc->event = pcf8584_bcast_rx_event;
    sc->send = pcf8584_bcast_rx_send;
    sc->recv = pcf8584_bcast_rx_recv;
    sc->wire_service = pcf8584_bcast_rx_wire_service;
    sc->wire_address = pcf8584_bcast_rx_wire_address;
    sc->unmatched_address = pcf8584_bcast_rx_unmatched_address;
}

static void pcf8584_reset_hold(Object *obj, ResetType type)
{
    PCF8584State *s = PCF8584(obj);

    /* Reset releases an untimed transfer before forgetting that it owns it. */
    if (s->transfer && s->bus && !pcf8584_on_wire(s) &&
        i2c_bus_busy(s->bus)) {
        i2c_end_transfer(s->bus);
    }

    s->own_address = 0;
    s->clock = 0;
    s->vector = 0;
    s->data = 0;
    s->control = 0;
    /* Out of reset the chip is idle and the bus is free. */
    s->status = PCF8584_STS_PIN | PCF8584_STS_BB;
    s->transfer = false;
    s->recv = false;
    s->start_pending = false;
    s->start_address_staged = false;
    s->start_address = 0;
    s->cur_addr7 = 0;

    s->wire_phase = PCF8584_WIRE_IDLE;
    s->wire_bit = 0;
    s->wire_shift = 0;
    s->wire_recv_byte = 0;
    s->wire_hold_release = PCF8584_HOLD_NONE;
    s->wire_active = false;
    s->wire_address_byte = false;
    s->wire_ack_nak = false;
    s->wire_grant_pending = false;
    s->wire_grant = 0;
    s->wire_start_waiting = false;
    s->wire_free_at = 0;
    s->wire_scl = s->wire_sda = true;
    if (pcf8584_on_wire(s) && s->bus) {
        /*
         * Reset reaches this adapter alone. An owning adapter's reset
         * waveform is unestablished, and a losing or reset non-owner must
         * not end the winner's transfer, so the peer transfer is left alone.
         */
        if (i2c_wire_reset_master(s->bus, s->wire_master) == -ENOTSUP) {
            qemu_log_mask(LOG_UNIMP, "pcf8584: reset while this adapter owns "
                          "the shared bus is unsupported\n");
        }
        s->wire_scl = i2c_wire_scl(s->bus);
        s->wire_sda = i2c_wire_sda(s->bus);
    }
    pcf8584_update_irq(s);
}

static void pcf8584_realize(DeviceState *dev, Error **errp)
{
    PCF8584State *s = PCF8584(dev);
    PCF8584BcastRxState *rx;

    memory_region_init_io(&s->iomem, OBJECT(dev), &pcf8584_ops, s,
                          "pcf8584", PCF8584_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    if (!s->bus) {
        if (pcf8584_on_wire(s)) {
            error_setg(errp, "wire-master needs a bus link to share");
            return;
        }
        s->bus = i2c_init_bus(dev, "i2c");
    } else if (pcf8584_on_wire(s)) {
        /*
         * clock_after_loss is false: a loser stops contributing to the
         * clock at once. The chip sources do not distinguish that from
         * clocking the rest of the winning byte, so it is a model policy.
         */
        int rc = i2c_wire_configure(s->bus, s->wire_master,
                                    PCF8584_SCL_LOW_NS, PCF8584_SCL_HIGH_NS,
                                    false);

        if (!rc) {
            rc = i2c_wire_set_observer(s->bus, s->wire_master,
                                       pcf8584_wire_event, s);
        }
        if (rc) {
            error_setg(errp, "wire-master %" PRIu32
                       " is not available on the linked bus (%d)",
                       s->wire_master, rc);
            return;
        }
        /*
         * Both lines low is not a state an idle open-drain wire can be in,
         * and it is what zero-initialisation leaves behind. Seed the
         * remembered levels from the wire the way reset does: a START is
         * classified against them, so an adapter that starts from the
         * impossible pair never recognises its own and waits for ever.
         */
        s->wire_scl = i2c_wire_scl(s->bus);
        s->wire_sda = i2c_wire_sda(s->bus);
    }

    rx = PCF8584_BCAST_RX(i2c_slave_create_simple(s->bus,
                                                   TYPE_PCF8584_BCAST_RX, 0));
    rx->chip = s;
    s->rx = I2C_SLAVE(rx);
}

static void pcf8584_unrealize(DeviceState *dev)
{
    PCF8584State *s = PCF8584(dev);

    if (pcf8584_on_wire(s) && s->bus) {
        i2c_wire_clear_observer(s->bus, s->wire_master,
                                pcf8584_wire_event, s);
    }
}

/* Combinations the state machine cannot produce must not load. */
static int pcf8584_post_load(void *opaque, int version_id)
{
    PCF8584State *s = opaque;

    if (version_id < 4 && s->start_address_staged) {
        s->start_address = s->data;
    }
    if (version_id < 5) {
        s->wire_grant_pending = false;
        s->wire_grant = 0;
    }
    if (version_id < 6) {
        s->wire_start_waiting = false;
    }

    if (s->wire_phase >= PCF8584_WIRE_PHASE_COUNT ||
        s->wire_hold_release >= PCF8584_HOLD_COUNT ||
        s->wire_bit > 8 || s->wire_free_at < 0 ||
        (s->start_pending && s->start_address_staged) ||
        (s->wire_start_waiting &&
         (s->wire_active || s->wire_phase != PCF8584_WIRE_IDLE ||
          !s->start_address_staged)) ||
        (s->wire_grant_pending &&
         (s->wire_grant < 0xe0 || s->wire_grant > 0xef))) {
        return -EINVAL;
    }
    if (!pcf8584_on_wire(s)) {
        /* The untimed path drives none of this. */
        if (s->wire_phase != PCF8584_WIRE_IDLE || s->wire_active ||
            s->wire_address_byte || s->wire_bit ||
            s->wire_hold_release != PCF8584_HOLD_NONE ||
            s->wire_grant_pending || s->wire_start_waiting) {
            return -EINVAL;
        }
        return 0;
    }
    /* The wire path never opens an untimed core transfer. */
    if (s->transfer) {
        return -EINVAL;
    }
    if (s->wire_active != (s->wire_phase != PCF8584_WIRE_IDLE &&
                           s->wire_phase != PCF8584_WIRE_LOST)) {
        return -EINVAL;
    }
    if (s->wire_phase == PCF8584_WIRE_IDLE &&
        (s->wire_address_byte || s->start_pending)) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_pcf8584 = {
    .name = "pcf8584",
    .version_id = 6,
    .minimum_version_id = 1,
    .post_load = pcf8584_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(own_address, PCF8584State),
        VMSTATE_UINT8(clock, PCF8584State),
        VMSTATE_UINT8(vector, PCF8584State),
        VMSTATE_UINT8(data, PCF8584State),
        VMSTATE_UINT8(control, PCF8584State),
        VMSTATE_UINT8(status, PCF8584State),
        VMSTATE_BOOL(transfer, PCF8584State),
        VMSTATE_BOOL(recv, PCF8584State),
        VMSTATE_BOOL(start_pending, PCF8584State),
        VMSTATE_BOOL_V(start_address_staged, PCF8584State, 2),
        VMSTATE_UINT8_V(start_address, PCF8584State, 4),
        VMSTATE_UINT8(cur_addr7, PCF8584State),
        VMSTATE_UINT8_V(wire_phase, PCF8584State, 3),
        VMSTATE_UINT8_V(wire_bit, PCF8584State, 3),
        VMSTATE_UINT8_V(wire_shift, PCF8584State, 3),
        VMSTATE_UINT8_V(wire_recv_byte, PCF8584State, 3),
        VMSTATE_UINT8_V(wire_hold_release, PCF8584State, 3),
        VMSTATE_BOOL_V(wire_active, PCF8584State, 3),
        VMSTATE_BOOL_V(wire_address_byte, PCF8584State, 3),
        VMSTATE_BOOL_V(wire_ack_nak, PCF8584State, 3),
        VMSTATE_BOOL_V(wire_grant_pending, PCF8584State, 5),
        VMSTATE_UINT8_V(wire_grant, PCF8584State, 5),
        VMSTATE_BOOL_V(wire_start_waiting, PCF8584State, 6),
        VMSTATE_BOOL_V(wire_scl, PCF8584State, 3),
        VMSTATE_BOOL_V(wire_sda, PCF8584State, 3),
        VMSTATE_INT64_V(wire_free_at, PCF8584State, 3),
        VMSTATE_END_OF_LIST()
    },
};

static const Property pcf8584_properties[] = {
    /*
     * Left unset the adapter makes its own private bus, which is the
     * one-controller Origin 200 wiring. An Origin 2000 module links four
     * adapters to one bus and gives each its own wire participant id.
     */
    DEFINE_PROP_LINK("bus", PCF8584State, bus, TYPE_I2C_BUS, I2CBus *),
    DEFINE_PROP_UINT32("wire-master", PCF8584State, wire_master,
                       PCF8584_NO_WIRE),
};

static void pcf8584_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Philips PCF8584 I2C-bus controller";
    dc->realize = pcf8584_realize;
    dc->unrealize = pcf8584_unrealize;
    dc->vmsd = &vmstate_pcf8584;
    device_class_set_props(dc, pcf8584_properties);
    rc->phases.hold = pcf8584_reset_hold;
}

static const TypeInfo pcf8584_types[] = {
    {
        .name          = TYPE_PCF8584,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PCF8584State),
        .class_init    = pcf8584_class_init,
    },
    {
        .name          = TYPE_PCF8584_BCAST_RX,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(PCF8584BcastRxState),
        .class_init    = pcf8584_bcast_rx_class_init,
    },
};

DEFINE_TYPES(pcf8584_types)
