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
#include "hw/core/device-diagnostic.h"
#include "qemu/timer.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_SGI_ELSC "sgi-elsc"
OBJECT_DECLARE_SIMPLE_TYPE(SGIElscState, SGI_ELSC)

/* The ELSC's own I2C slave address on the system-controller bus. */
#define SGI_ELSC_ADDR 0x08

/*
 * Every command and response shares one frame: a length byte, that many
 * bytes of ASCII text, and a checksum byte, the whole packet summing to
 * zero modulo 256. The firmware's own packet buffers are 96 bytes and a
 * length byte above 94 reads past them, so a model must never emit one --
 * see docs/datasheets/pcf8584-elsc.md.
 */
#define SGI_ELSC_MAX_TEXT   94
#define SGI_ELSC_MAX_FRAME  (SGI_ELSC_MAX_TEXT + 2)

/*
 * Console input queued from the AUX serial port, waiting for the next
 * "get". Sized well past one frame's worth so a burst of typed input
 * ahead of a slow poller does not lose characters immediately -- a model
 * choice, not a hardware fact; nothing read for the datasheet says how
 * deep the real queue is.
 */
#define SGI_ELSC_RX_QUEUE_SIZE 256

struct SGIElscState {
    I2CSlave parent_obj;
    DeviceDiagnosticSink *diagnostic;
    bool diagnostic_arbitration_broadcast;

    /*
     * The request being written to us, accumulated one byte per I2C send
     * until the master switches direction to read the response. Sized one
     * over the legal maximum so a guest that ignores the 94-byte limit
     * over-fills this buffer rather than corrupting adjacent state --
     * elsc_send() still refuses to write past it.
     */
    uint8_t rxbuf[SGI_ELSC_MAX_FRAME + 1];
    uint32_t rxlen;

    /* The response being read back, built once the request is complete. */
    uint8_t txbuf[SGI_ELSC_MAX_FRAME];
    uint32_t txlen;
    uint32_t txpos;

    /*
     * The arbitration broadcast: the mechanism that lets this super-master
     * and every node's Hub share one bus with no real I2C multi-master
     * support. Free-running from realize for as long as the machine runs,
     * independent of whether any guest is currently polling for it -- see
     * elsc_arb_step() in hw/misc/sgi-elsc.c.
     */
    I2CBus *bus;
    QEMUTimer *arb_timer;
    /*
     * Kept as a Hub-supplied property, but no longer sizes the rotation --
     * the rotation always covers all eight architectural processor codes
     * regardless of how many this machine populates (see elsc_arb_step()
     * in hw/misc/sgi-elsc.c for why). This device implements one real
     * console-input queue, for processor code 0, matching the ASCII
     * protocol's own hardcoded selector; num_cpus has no further effect
     * until a second real queue exists to answer for a second code.
     */
    uint32_t num_cpus;
    uint8_t dip_switches;
    uint32_t arb_code;    /* which of the eight processor codes this cycle presents */
    bool arb_present;    /* false: showing the gap; true: holding the signal */

    /*
     * The AUX serial port: a real RS-232 connector, cabled with only
     * transmit, receive and ground (docs/datasheets/pcf8584-elsc.md), and
     * multiplexed with the checksummed command channel above by one rule
     * on the input side -- a byte sequence beginning 0x14 is a controller
     * command and is never queued; everything else is. Console selection
     * currently reaches only processor 0, so all queued input answers
     * "get" with selector 0; a second processor's own input path is not
     * modelled.
     */
    CharFrontend chr;
    uint8_t rx_queue[SGI_ELSC_RX_QUEUE_SIZE];
    uint32_t rx_queue_len;
    bool in_escape;   /* discarding a controller command typed on the line */

    /* Supervisor mode gates the destructive commands (page 124 of the
     * Origin 200 Owner's Guide); "pas" sets it, factory default "none". */
    bool supervisor;

    /*
     * Latched the first time a console flush is dropped because no character
     * device is attached to the AUX port, so the diagnostic below fires one
     * line rather than one per write for the rest of a dark-console boot.
     */
    bool aux_no_backend_warned;
};

#endif /* HW_MISC_SGI_ELSC_H */
