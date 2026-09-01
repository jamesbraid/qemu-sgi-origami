/*
 * SGI Origin 200 module system controller (MSC)
 *
 * Copyright (c) 2026 Origami contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SGI_ELSC_H
#define HW_MISC_SGI_ELSC_H

#include "hw/i2c/i2c.h"
#include "hw/misc/sgi-diagnostic.h"
#include "qemu/timer.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_SGI_ELSC "sgi-elsc"
OBJECT_DECLARE_SIMPLE_TYPE(SGIElscState, SGI_ELSC)

/* The ELSC's own I2C slave address on the system-controller bus. */
#define SGI_ELSC_ADDR 0x08

/*
 * Length, text, checksum summing to zero mod 256. A length above 94
 * overruns the firmware's own 96-byte buffers; never emit one.
 */
#define SGI_ELSC_MAX_TEXT   94
#define SGI_ELSC_MAX_FRAME  (SGI_ELSC_MAX_TEXT + 2)

/*
 * AUX input queued for "get". The depth is a model choice; no source
 * states the real queue's.
 */
#define SGI_ELSC_RX_QUEUE_SIZE 256

struct SGIElscState {
    I2CSlave parent_obj;
    DeviceDiagnosticSink *diagnostic;
    bool diagnostic_arbitration_broadcast;

    /*
     * The request, one byte per I2C send; sized one over the legal maximum
     * so an over-long guest frame lands here, not in adjacent state.
     */
    uint8_t rxbuf[SGI_ELSC_MAX_FRAME + 1];
    uint32_t rxlen;

    /* The response being read back, built once the request is complete. */
    uint8_t txbuf[SGI_ELSC_MAX_FRAME];
    uint32_t txlen;
    uint32_t txpos;

    /*
     * The arbitration broadcast rotation, free-running from realize
     * whether or not anything polls (elsc_arb_step()).
     */
    I2CBus *bus;
    QEMUTimer *arb_timer;
    /*
     * Hub-supplied and currently inert: the rotation always covers all
     * eight codes and only code 0 has a real input queue.
     */
    uint32_t num_cpus;
    /*
     * Hub-supplied: the global index of this node's slice A processor,
     * naming this controller's instance in the arbitration trace. The
     * code space is global processor indexes, so it also says which
     * codes are local.
     */
    uint32_t cpu_base;
    uint8_t dip_switches;
    uint32_t arb_code;    /* which of the eight codes this cycle presents */
    uint32_t arb_rev;    /* full rotations completed; varies the slot lengths */
    bool arb_present;    /* the gap has been shown; the signal comes next */
    bool arb_held;       /* the signal is on the wire, held for its processor */
    uint8_t arb_signal;  /* the held byte, latched when the hold begins */
    int64_t arb_hold_until;   /* virtual-clock us end of the current slot */

    /*
     * The AUX serial port, multiplexed with the command channel by one
     * input rule: bytes after 0x14 are a controller command, never queued.
     */
    CharFrontend chr;
    uint8_t rx_queue[SGI_ELSC_RX_QUEUE_SIZE];
    uint32_t rx_queue_len;
    bool in_escape;   /* discarding a controller command typed on the line */

    /*
     * Supervisor mode gates the destructive commands (page 124 of the
     * Origin 200 Owner's Guide); "pas" sets it, factory default "none".
     */
    bool supervisor;

    /* Warn once per dark-console boot, not once per dropped flush. */
    bool aux_no_backend_warned;
};

#endif /* HW_MISC_SGI_ELSC_H */
