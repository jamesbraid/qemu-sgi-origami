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

struct PCF8584State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    I2CBus *bus;
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

    /*
     * The matched 7-bit address, valid while transfer is set: how a
     * repeated start tells a direction change from a new target, which
     * the I2C core cannot (pcf8584_start()).
     */
    uint8_t cur_addr7;
};

#endif /* HW_I2C_PCF8584_H */
