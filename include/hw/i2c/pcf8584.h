/*
 * Philips PCF8584 I2C-bus controller
 *
 * Copyright (c) 2026 Origami contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_PCF8584_H
#define HW_I2C_PCF8584_H

#include "hw/core/sysbus.h"
#include "hw/misc/sgi-diagnostic.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_PCF8584 "pcf8584"
OBJECT_DECLARE_SIMPLE_TYPE(PCF8584State, PCF8584)

/*
 * Two addresses selected by the A0 pin, spaced eight bytes apart the way the
 * Origin 200's Hub wires it.
 */
#define PCF8584_A0_STRIDE   8
#define PCF8584_MMIO_SIZE   (2 * PCF8584_A0_STRIDE)

/* Control register, write side. */
#define PCF8584_CTL_PIN     0x80    /* reset, and generate repeated starts */
#define PCF8584_CTL_ESO     0x40    /* enable serial output */
#define PCF8584_CTL_ES1     0x20    /* register select, high bit */
#define PCF8584_CTL_ES2     0x10    /* register select, low bit */
#define PCF8584_CTL_ENI     0x08    /* enable interrupt output */
#define PCF8584_CTL_STA     0x04    /* generate a start condition */
#define PCF8584_CTL_STO     0x02    /* generate a stop condition */
#define PCF8584_CTL_ACK     0x01    /* acknowledge received bytes */

/* Status register, read side. Shares the address with the control register. */
#define PCF8584_STS_PIN     0x80    /* pending interrupt NOT: set means busy */
#define PCF8584_STS_STS     0x20    /* stop detected, slave receive mode */
#define PCF8584_STS_BER     0x10    /* bus error */
#define PCF8584_STS_LRB     0x08    /* last received acknowledge bit */
#define PCF8584_STS_AAS     0x04    /* addressed as slave */
#define PCF8584_STS_LAB     0x02    /* lost arbitration */
#define PCF8584_STS_BB      0x01    /* bus not busy */

/* "wire-master" default: this adapter drives its own untimed private bus. */
#define PCF8584_NO_WIRE     UINT32_MAX

/* Where the adapter is in the byte it is clocking onto the shared wire. */
typedef enum {
    PCF8584_WIRE_IDLE,          /* driving nothing */
    PCF8584_WIRE_START_SDA,     /* SDA fall staged, awaiting it on the line */
    PCF8584_WIRE_START_HOLD,    /* SDA low and SCL high, awaiting the clock */
    PCF8584_WIRE_BITS,          /* clocking the eight address or data bits */
    PCF8584_WIRE_ACK,           /* the ninth, acknowledgement, clock */
    PCF8584_WIRE_PAUSED,        /* SCL held low, awaiting the host */
    PCF8584_WIRE_RESTART_SCL,   /* clock resumed to place a repeated START */
    PCF8584_WIRE_STOP_SCL,      /* clock resumed to place a STOP */
    PCF8584_WIRE_STOP_SDA,      /* SDA rise staged, awaiting STOP on the line */
    PCF8584_WIRE_LOST,          /* arbitration lost, waiting the winner out */
    PCF8584_WIRE_PHASE_COUNT,
} PCF8584WirePhase;

/* Which host access releases an addressed adapter's own SCL hold. */
typedef enum {
    PCF8584_HOLD_NONE,
    PCF8584_HOLD_READ,          /* receiver: a data-register read */
    PCF8584_HOLD_WRITE,         /* transmitter: a data-register write */
    PCF8584_HOLD_COUNT,
} PCF8584Hold;

struct PCF8584State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    I2CBus *bus;
    I2CSlave *rx;               /* this adapter's own endpoint on that bus */
    qemu_irq irq;
    DeviceDiagnosticSink *diagnostic;

    /*
     * Four registers behind one address, chosen by ESO/ES1/ES2. Separate
     * fields so a wrong select cannot read a plausible neighbour.
     */
    uint8_t own_address;        /* S0', seven bits, right-justified */
    uint8_t clock;              /* S2, oscillator divider and bus speed */
    uint8_t vector;             /* S3, interrupt vector */
    uint8_t data;               /* S0, the byte shifted in or out */

    uint8_t control;            /* S1 write side */
    uint8_t status;             /* S1 read side */

    /*
     * The core accepted the start and the open transfer must be ended.
     * Chip-level open-transaction state is the BB status bit, which is
     * what tells a first start from a repeated one.
     */
    bool transfer;
    bool recv;                  /* current direction is slave-to-master */
    bool start_pending;         /* a repeated start awaits its address byte */
    /* An idle data-register write awaits an immediate first START. */
    bool start_address_staged;
    uint8_t start_address;      /* outbound byte kept apart from readable S0 */

    /*
     * The matched 7-bit address, valid while transfer is set: how a
     * repeated start tells a direction change from a new target, which
     * the I2C core cannot (pcf8584_start()).
     */
    uint8_t cur_addr7;

    /*
     * Participant id on the shared timed wire, or PCF8584_NO_WIRE for the
     * untimed single-owner path. Several of these sit on one Origin 2000
     * module bus, so the transfer runs as staged SDA/SCL transitions rather
     * than as one indivisible core call.
     */
    uint32_t wire_master;

    uint8_t wire_phase;         /* PCF8584WirePhase */
    uint8_t wire_bit;           /* 0-7 data bit, 8 the acknowledgement clock */
    uint8_t wire_shift;         /* byte being clocked out */
    uint8_t wire_recv_byte;     /* byte being clocked in */
    uint8_t wire_hold_release;  /* PCF8584Hold */
    bool wire_active;           /* this adapter owns an open transfer */
    bool wire_address_byte;     /* the byte in flight is the address */
    bool wire_ack_nak;          /* acknowledgement bit sampled on clock nine */
    bool wire_grant_pending;    /* publish the observed module grant at STOP */
    uint8_t wire_grant;         /* unmatched module grant awaiting STOP */
    bool wire_start_waiting;    /* compatibility START held until bus free */
    bool wire_scl, wire_sda;    /* line levels as this adapter last saw them */
    int64_t wire_free_at;       /* earliest fresh START after the last STOP */
};

/*
 * The registers as a guest access reaches them, for an in-process driver that
 * would otherwise have to stand up the whole memory-dispatch path. The machine
 * still goes through the MMIO ops.
 */
uint64_t pcf8584_read_register(PCF8584State *s, hwaddr addr, unsigned size);
void pcf8584_write_register(PCF8584State *s, hwaddr addr, uint64_t value,
                            unsigned size);

#endif /* HW_I2C_PCF8584_H */
