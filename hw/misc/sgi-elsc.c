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
 * negative reply but not a stalled bus. The console pass-through comes
 * later.
 *
 * This device also drives the arbitration broadcast that lets it and every
 * node's Hub share one bus with no real I2C multi-master mechanism: a
 * free-running rotation, one processor's signal per cycle, that presents an
 * observable gap before each signal and holds it once presented -- see
 * elsc_arb_step().
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/core/qdev-properties.h"
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

/*
 * QEMU's own reserved broadcast/general-call address, matching
 * hw/i2c/core.c's private I2C_BROADCAST -- not exported by the public
 * header, so restated here rather than reached into the core for.
 */
#define ELSC_I2C_BROADCAST 0x00

/*
 * The signal pair a processor watches for is derived, not fixed: 0x70 * 2
 * plus twice the node's own slot index plus a live read of the Hub's
 * processor-number register, and signal2 is signal1 with bit 3 set. This
 * machine is single-node, so the slot term is 0, and PI_CPU_NUM is not
 * modelled -- see docs/datasheets/pcf8584-elsc.md. Computing one value from
 * that chain would be one wrong term away from a silent hang; rotating
 * through every populated processor's signal in turn is correct whatever
 * the chain would have produced, which is what a real super-master does
 * anyway.
 */
#define ELSC_SIGNAL_BASE 0xe0   /* 0x70 * 2 */

static uint8_t elsc_signal_byte(unsigned processor_code, bool signal2)
{
    uint8_t signal1 = ELSC_SIGNAL_BASE + processor_code;

    return signal2 ? signal1 + 8 : signal1;
}

/*
 * A one-shot broadcast write: address nobody, send one byte, release the
 * bus. Nothing about this is an ordinary transaction from any Hub's point
 * of view -- the byte simply appears in every PCF8584's data register,
 * which is the whole of what the arbitration protocol needs (see
 * docs/notebooks/ds-hub.md's construction note and hw/i2c/pcf8584.c's
 * broadcast receiver). If nothing is listening this is a silent no-op:
 * broadcasting to an empty bus is not an error, it just has no effect, and
 * the rotation still advances next time the timer fires.
 */
static void elsc_broadcast(SGIElscState *s, uint8_t value)
{
    if (i2c_start_transfer(s->bus, ELSC_I2C_BROADCAST, false)) {
        return;
    }
    i2c_send(s->bus, value);
    i2c_end_transfer(s->bus);
}

/*
 * Model choices, not measured values: the gap only needs to be long enough
 * that a poll can observe it, and the hold only needs to clear the 250us
 * threshold docs/datasheets/pcf8584-elsc.md specifies with margin. Neither
 * number comes from real hardware.
 */
#define ELSC_ARB_GAP_US    50
#define ELSC_ARB_HOLD_US   300
#define ELSC_ARB_RETRY_US  20

/*
 * The free-running arbitration cycle: for the current processor slot,
 * present a gap (neither signal value), then present and hold that slot's
 * signal, then advance to the next slot. Presenting a gap before every
 * signal, rather than moving straight from one slot's signal to the next,
 * is what the firmware's first wait loop depends on -- it requires its own
 * signal to be *absent* before it will trust a subsequent appearance of it,
 * so a model that never presents an absence never satisfies that loop, no
 * matter how correctly it eventually presents the signal itself.
 *
 * Deferred rather than driven through when the bus is already in a real
 * transaction: broadcasting here would corrupt whatever the PCF8584 and
 * another slave are in the middle of doing, since QEMU's I2C core has no
 * notion of a second master waiting politely. Retried shortly rather than
 * queued through i2c_bus_master()/i2c_bus_release(), because nothing on
 * this bus's master side (hw/i2c/pcf8584.c) ever calls i2c_bus_release() --
 * it drives transactions directly rather than through that pending-master
 * queue -- so a deferral that waited for a release notification would wait
 * forever.
 */
static void elsc_arb_step(void *opaque)
{
    SGIElscState *s = opaque;
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);

    if (i2c_bus_busy(s->bus)) {
        timer_mod(s->arb_timer, now + ELSC_ARB_RETRY_US);
        return;
    }

    if (!s->arb_present) {
        elsc_broadcast(s, 0);
        s->arb_present = true;
        timer_mod(s->arb_timer, now + ELSC_ARB_GAP_US);
        return;
    }

    /*
     * Whether this slot has console input queued decides signal1 versus
     * signal2 -- signal2 is what tells the driver's transaction helper a
     * "get" will find something, and withholding it for an empty queue is
     * the correct behaviour, not a missing feature. No queue exists yet at
     * this milestone, so every slot always presents signal1; the console
     * path adds the real condition here.
     */
    elsc_broadcast(s, elsc_signal_byte(s->arb_slot, false));
    s->arb_present = false;
    s->arb_slot = (s->arb_slot + 1) % s->num_cpus;
    timer_mod(s->arb_timer, now + ELSC_ARB_HOLD_US);
}

static void elsc_reset_hold(Object *obj, ResetType type)
{
    SGIElscState *s = SGI_ELSC(obj);

    s->rxlen = 0;
    s->txlen = 0;
    s->txpos = 0;
    s->arb_slot = 0;
    s->arb_present = false;
}

static void elsc_realize(DeviceState *dev, Error **errp)
{
    SGIElscState *s = SGI_ELSC(dev);

    s->bus = I2C_BUS(qdev_get_parent_bus(dev));
    s->arb_timer = timer_new_us(QEMU_CLOCK_VIRTUAL, elsc_arb_step, s);
    timer_mod(s->arb_timer, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));
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
        VMSTATE_TIMER_PTR(arb_timer, SGIElscState),
        VMSTATE_UINT32(arb_slot, SGIElscState),
        VMSTATE_BOOL(arb_present, SGIElscState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sgi_elsc_properties[] = {
    DEFINE_PROP_UINT32("num-cpus", SGIElscState, num_cpus, 1),
};

static void sgi_elsc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    dc->desc = "SGI Origin 200 entry-level system controller";
    dc->realize = elsc_realize;
    dc->vmsd = &vmstate_sgi_elsc;
    device_class_set_props(dc, sgi_elsc_properties);
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
