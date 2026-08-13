/*
 * SGI Origin 200 entry-level system controller (ELSC)
 *
 * Copyright (c) 2026 Origami contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The system controller's checksummed ASCII command/response channel,
 * modelled from docs/datasheets/pcf8584-elsc.md. Every command is a short
 * mnemonic sent and answered inside one combined write-then-read I2C
 * transaction: a length byte, that many bytes of text, and a checksum byte
 * chosen so the whole packet -- length through checksum, inclusive -- sums
 * to zero modulo 256.
 *
 * Only "ver" is answered for real at this milestone; everything else in
 * the ASCII vocabulary answers "err cmd", which is load-bearing rather than
 * a placeholder -- the console-selection routine issues a DIP-switch read
 * before it ever probes this device as a console, and it tolerates a
 * negative reply but not a stalled bus. The arbitration broadcast this
 * device also needs to drive, and the console pass-through, come later.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/sgi-elsc.h"
#include "migration/vmstate.h"

/*
 * The version string's content is irrelevant to firmware's presence probe
 * -- only the response's shape (it must begin "ok") is checked -- so this
 * names the model rather than claiming a real ELSC firmware revision.
 */
#define SGI_ELSC_VERSION "origami-elsc 1.0"

static uint8_t elsc_checksum(const uint8_t *buf, unsigned len)
{
    uint8_t sum = 0;
    unsigned i;

    for (i = 0; i < len; i++) {
        sum += buf[i];
    }
    return sum;
}

/*
 * Frame text into the response buffer: a length byte, the text, and a
 * checksum chosen so length + text + checksum sums to zero modulo 256.
 * text_len must never exceed SGI_ELSC_MAX_TEXT -- the datasheet's own
 * bound, because the firmware's transfer-length tracking is unclamped and
 * a longer frame makes it read past its own buffer before any check runs.
 * That bound is a model invariant on what this device may emit, not
 * something a guest can trigger, so it is asserted rather than logged.
 *
 * The framed packet starts at txbuf[1], not txbuf[0]. The PCF8584's own
 * receive pipeline (hw/i2c/pcf8584.c) fetches one byte from whichever slave
 * answered before the driver ever reads the data register, and hands that
 * byte back on the driver's first read -- which is the discarded read
 * docs/datasheets/pcf8584-elsc.md describes: "the driver reads the data
 * register once and throws the byte away... a model that answers it with
 * the first response byte shifts the entire packet by one." That discard
 * consumes whatever this device put on the wire first, so txbuf[0] has to
 * be a byte firmware was always going to throw away, and the length byte
 * firmware actually keeps has to be the one after it.
 */
static void elsc_frame_response(SGIElscState *s, const char *text,
                                unsigned text_len)
{
    g_assert(text_len <= SGI_ELSC_MAX_TEXT);

    s->txbuf[0] = 0;    /* consumed by the chip's own pipeline discard */
    s->txbuf[1] = text_len;
    memcpy(&s->txbuf[2], text, text_len);
    s->txbuf[text_len + 2] =
        (uint8_t)(-(int)elsc_checksum(&s->txbuf[1], text_len + 1));
    s->txlen = text_len + 3;
    s->txpos = 0;
}

static void elsc_frame_response_str(SGIElscState *s, const char *text)
{
    elsc_frame_response(s, text, strlen(text));
}

/* Dispatch one fully-received, checksum- and length-checked request, and
 * frame its reply. */
static void elsc_dispatch(SGIElscState *s, const char *text, unsigned len)
{
    if (len == 3 && memcmp(text, "ver", 3) == 0) {
        elsc_frame_response_str(s, "ok " SGI_ELSC_VERSION);
        return;
    }

    elsc_frame_response_str(s, "err cmd");
}

/*
 * The request is complete once the master switches direction to read the
 * response. Validate its framing and checksum, then dispatch it.
 */
static void elsc_request_complete(SGIElscState *s)
{
    if (s->rxlen < 2 || s->rxlen != (uint32_t)s->rxbuf[0] + 2 ||
        elsc_checksum(s->rxbuf, s->rxlen) != 0) {
        /*
         * A short, long, or bad-checksum packet. "csum" is the token real
         * firmware defines for a rejected checksum, and nothing in the
         * protocol names a separate framing error -- a malformed packet
         * fails the identical whole-packet-sums-to-zero test a corrupted
         * checksum does, so answering it the same way is the protocol's
         * own logic, not an approximation of it.
         */
        elsc_frame_response_str(s, "err csum");
        return;
    }

    elsc_dispatch(s, (const char *)&s->rxbuf[1], s->rxbuf[0]);
}

static int elsc_event(I2CSlave *i2c, enum i2c_event event)
{
    SGIElscState *s = SGI_ELSC(i2c);

    switch (event) {
    case I2C_START_SEND:
        /* A request is arriving. */
        s->rxlen = 0;
        break;
    case I2C_START_RECV:
        /* The master switched to read: the request just written is
         * complete. */
        elsc_request_complete(s);
        break;
    case I2C_FINISH:
        s->rxlen = 0;
        s->txlen = 0;
        s->txpos = 0;
        break;
    default:
        break;
    }

    return 0;
}

static int elsc_send(I2CSlave *i2c, uint8_t data)
{
    SGIElscState *s = SGI_ELSC(i2c);

    if (s->rxlen >= sizeof(s->rxbuf)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-elsc: request longer than the protocol allows, "
                      "byte dropped\n");
        return 0;
    }

    s->rxbuf[s->rxlen++] = data;
    return 0;
}

static uint8_t elsc_recv(I2CSlave *i2c)
{
    SGIElscState *s = SGI_ELSC(i2c);

    if (s->txpos < s->txlen) {
        return s->txbuf[s->txpos++];
    }

    /* Past the end of a well-formed response the driver's own length byte
     * already told it to stop reading; this is what it gets if it doesn't. */
    return 0xff;
}

static void elsc_reset_hold(Object *obj, ResetType type)
{
    SGIElscState *s = SGI_ELSC(obj);

    s->rxlen = 0;
    s->txlen = 0;
    s->txpos = 0;
}

static const VMStateDescription vmstate_sgi_elsc = {
    .name = "sgi-elsc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, SGIElscState),
        VMSTATE_UINT8_ARRAY(rxbuf, SGIElscState, SGI_ELSC_MAX_FRAME + 1),
        VMSTATE_UINT32(rxlen, SGIElscState),
        VMSTATE_UINT8_ARRAY(txbuf, SGIElscState, SGI_ELSC_MAX_FRAME + 1),
        VMSTATE_UINT32(txlen, SGIElscState),
        VMSTATE_UINT32(txpos, SGIElscState),
        VMSTATE_END_OF_LIST()
    },
};

static void sgi_elsc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    dc->desc = "SGI Origin 200 entry-level system controller";
    dc->vmsd = &vmstate_sgi_elsc;
    rc->phases.hold = elsc_reset_hold;

    sc->event = elsc_event;
    sc->send = elsc_send;
    sc->recv = elsc_recv;
}

static const TypeInfo sgi_elsc_types[] = {
    {
        .name          = TYPE_SGI_ELSC,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(SGIElscState),
        .class_init    = sgi_elsc_class_init,
    },
};

DEFINE_TYPES(sgi_elsc_types)
