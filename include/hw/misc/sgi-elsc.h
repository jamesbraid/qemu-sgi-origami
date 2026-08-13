/*
 * SGI Origin 200 entry-level system controller (ELSC)
 *
 * Copyright (c) 2026 Origami contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SGI_ELSC_H
#define HW_MISC_SGI_ELSC_H

#include "hw/i2c/i2c.h"
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

struct SGIElscState {
    I2CSlave parent_obj;

    /*
     * The request being written to us, accumulated one byte per I2C send
     * until the master switches direction to read the response. Sized one
     * over the legal maximum so a guest that ignores the 94-byte limit
     * over-fills this buffer rather than corrupting adjacent state --
     * elsc_send() still refuses to write past it.
     */
    uint8_t rxbuf[SGI_ELSC_MAX_FRAME + 1];
    uint32_t rxlen;

    /*
     * The response being read back, built once the request is complete.
     * One byte longer than the frame it holds: byte 0 is not part of the
     * framed packet, it is what the PCF8584's own receive pipeline consumes
     * as its discarded first read (see elsc_frame_response() in
     * hw/misc/sgi-elsc.c), so the real length byte is byte 1.
     */
    uint8_t txbuf[SGI_ELSC_MAX_FRAME + 1];
    uint32_t txlen;
    uint32_t txpos;
};

#endif /* HW_MISC_SGI_ELSC_H */
