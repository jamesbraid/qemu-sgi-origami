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
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_PCF8584 "pcf8584"
OBJECT_DECLARE_SIMPLE_TYPE(PCF8584State, PCF8584)

/*
 * The chip presents two addresses, selected by its A0 pin. Hosts wire A0 to
 * some address bit of their own; this model spaces the two eight bytes apart,
 * which is how the SGI Origin 200's Hub ASIC wires it, and answers accesses of
 * any width up to that spacing.
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
#define I2C_PCF_PIN     0x80    /* pending interrupt NOT: set means busy */
#define I2C_PCF_STS     0x20    /* stop detected, slave receive mode */
#define I2C_PCF_BER     0x10    /* bus error */
#define I2C_PCF_LRB     0x08    /* last received acknowledge bit */
#define I2C_PCF_AAS     0x04    /* addressed as slave */
#define I2C_PCF_LAB     0x02    /* lost arbitration */
#define I2C_PCF_BB      0x01    /* bus not busy */

struct PCF8584State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;

    /*
     * One address selects one of four registers, chosen by ESO together with
     * ES1 and ES2. Kept as four fields rather than an array indexed by the
     * select bits, so that a wrong select cannot silently read a plausible
     * value out of a neighbouring slot.
     */
    uint8_t own_address;        /* S0', seven bits, right-justified */
    uint8_t clock;              /* S2, oscillator divider and bus speed */
    uint8_t vector;             /* S3, interrupt vector */
    uint8_t data;               /* S0, the byte shifted in or out */

    uint8_t control;            /* S1 write side */
    uint8_t status;             /* S1 read side */

    /*
     * A transfer is in progress between a start condition and a stop. The
     * chip cannot see the difference between a first start and a repeated
     * one from the register writes alone, so this is what distinguishes them:
     * the firmware writes the address before the control register for a first
     * start, and after it for a repeated start.
     */
    bool transfer;
    bool recv;                  /* current direction is slave-to-master */
    bool start_pending;         /* a repeated start awaits its address byte */
};

#endif /* HW_I2C_PCF8584_H */
