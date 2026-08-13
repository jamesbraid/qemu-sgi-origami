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
 * "ver", "get", "acp" (both directions) and "pas" are answered for real;
 * everything else in the ASCII vocabulary answers "err cmd", which is
 * load-bearing rather than a placeholder -- the console-selection routine
 * issues a DIP-switch read before it ever probes this device as a
 * console, and it tolerates a negative reply but not a stalled bus.
 *
 * This device also drives the arbitration broadcast that lets it and every
 * node's Hub share one bus with no real I2C multi-master mechanism: a
 * free-running rotation, one processor's signal per cycle, that presents an
 * observable gap before each signal and holds it once presented -- see
 * elsc_arb_step(). And it is the far end of the AUX serial port: characters
 * typed there queue for "get" unless they are an escaped controller command,
 * and "acp" output commands write straight to it -- see elsc_receive() and
 * elsc_handle_acp_command().
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
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
 * The framed packet starts at txbuf[0]. An earlier version of this device
 * put a throwaway byte ahead of it, reasoning that the PCF8584's receive
 * pipeline discarded a byte from whichever slave answered. It does not:
 * the discard docs/datasheets/pcf8584-elsc.md describes is the driver's
 * first read returning the address-and-direction byte the chip's own data
 * register already held, before any response byte has arrived -- the
 * Philips product specification attaches the transfer to the read, not
 * the start (hw/i2c/pcf8584.c). A responder's own first byte was never at
 * risk, and this device does not need to protect it.
 */
static void elsc_frame_response(SGIElscState *s, const char *text,
                                unsigned text_len)
{
    g_assert(text_len <= SGI_ELSC_MAX_TEXT);

    s->txbuf[0] = text_len;
    memcpy(&s->txbuf[1], text, text_len);
    s->txbuf[text_len + 1] =
        (uint8_t)(-(int)elsc_checksum(s->txbuf, text_len + 1));
    s->txlen = text_len + 2;
    s->txpos = 0;
}

static void elsc_frame_response_str(SGIElscState *s, const char *text)
{
    elsc_frame_response(s, text, strlen(text));
}

/*
 * "get": the console-input poll. The response's first six bytes are a
 * fixed "acp " plus a processor-selector digit plus a space -- firmware
 * checks only those first four and copies the payload starting at byte
 * six, discarding bytes four and five unread, so the shape has to be
 * exactly that even though only four of the six are ever inspected. This
 * device reaches only processor 0, so the selector is always "0".
 *
 * A "get" this device answers is a "get" the driver was always going to
 * send: the arbitration wait withholds signal2 -- see elsc_arb_step() --
 * whenever the queue is empty, and the driver's own transaction helper
 * never puts "get" on the bus at all unless it saw signal2. Answering an
 * empty queue with an empty payload here is therefore for a case real
 * firmware does not create, not the queue-empty response firmware reads.
 */
static void elsc_handle_get(SGIElscState *s)
{
    char buf[SGI_ELSC_MAX_TEXT];
    unsigned payload_max = SGI_ELSC_MAX_TEXT - 6;
    unsigned n = s->rx_queue_len < payload_max ? s->rx_queue_len : payload_max;

    memcpy(buf, "acp 0 ", 6);
    memcpy(&buf[6], s->rx_queue, n);
    elsc_frame_response(s, buf, 6 + n);

    memmove(s->rx_queue, &s->rx_queue[n], s->rx_queue_len - n);
    s->rx_queue_len -= n;
}

/*
 * "acp <selector> <characters>": the console-output flush, the same
 * mnemonic as the "get" response but now as a command. Answered
 * "ok <count>", count in hexadecimal and never zero for a non-empty
 * write -- a bare "ok" parses as zero accepted and livelocks the
 * firmware's output queue, which is why this always answers the count
 * form rather than the bare success elsewhere in this file. Accepting
 * every character submitted is the simple correct answer, and this
 * device has no reason to accept fewer.
 */
static void elsc_handle_acp_command(SGIElscState *s, const char *text,
                                    unsigned len)
{
    char resp[8];
    unsigned n;

    if (len < 6) {
        elsc_frame_response_str(s, "err arg");
        return;
    }

    n = len - 6;
    if (n > 86) {
        n = 86;   /* "at most 86 characters per transaction" */
    }
    qemu_chr_fe_write_all(&s->chr, (const uint8_t *)&text[6], n);

    snprintf(resp, sizeof(resp), "ok %x", n);
    elsc_frame_response_str(s, resp);
}

/*
 * "pas <4 characters>": enter supervisor mode if the password matches the
 * one configured (factory default "none", and this model implements no
 * way to change it), which gates the destructive commands below.
 */
static void elsc_handle_pas(SGIElscState *s, const char *text, unsigned len)
{
    if (len != 8 || text[3] != ' ') {
        elsc_frame_response_str(s, "err arg");
        return;
    }
    if (memcmp(&text[4], "none", 4) == 0) {
        s->supervisor = true;
        elsc_frame_response_str(s, "ok");
    } else {
        elsc_frame_response_str(s, "err perm");
    }
}

/* Dispatch one fully-received, checksum- and length-checked request, and
 * frame its reply. */
static void elsc_dispatch(SGIElscState *s, const char *text, unsigned len)
{
    if (len == 3 && memcmp(text, "ver", 3) == 0) {
        elsc_frame_response_str(s, "ok " SGI_ELSC_VERSION);
        return;
    }
    if (len == 3 && memcmp(text, "get", 3) == 0) {
        elsc_handle_get(s);
        return;
    }
    if (len >= 4 && memcmp(text, "acp ", 4) == 0) {
        elsc_handle_acp_command(s, text, len);
        return;
    }
    if (len >= 3 && memcmp(text, "pas", 3) == 0) {
        elsc_handle_pas(s, text, len);
        return;
    }
    if (len == 3 && memcmp(text, "rst", 3) == 0) {
        /* A minimal protected command, to exercise supervisor gating --
         * not a claim that "rst" is the real vocabulary's reset mnemonic,
         * which is not established by anything read for the datasheet. */
        if (!s->supervisor) {
            elsc_frame_response_str(s, "err perm");
            return;
        }
        qemu_log_mask(LOG_UNIMP,
                      "sgi-elsc: reset requested, not modelled\n");
        elsc_frame_response_str(s, "ok");
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
 * processor-number register, and signal2 computed as signal1 + 8. The
 * resulting processor code has to fit in three bits: signal2 is signal1
 * with bit 3 set, so a code of eight or more would make one processor's
 * "message waiting" numerically identical to a different, lower-numbered
 * processor's "nothing waiting" -- ds-hub's answer to the width this file
 * originally left open. Eight is also exactly what the protocol's
 * addressable space supplies, four node slots of two processors each, and
 * the firmware's own configuration for the family's larger machines
 * confirms it by name, excluding any node slot whose code would exceed
 * seven from ever using the controller at all. See
 * docs/datasheets/pcf8584-elsc.md.
 *
 * A real controller cannot know in advance which of the eight codes are
 * actually populated, so it offers all eight in turn regardless -- what a
 * real controller doling out time slices does anyway -- independent of how
 * many of those codes this specific model backs with a real input queue.
 */
#define ELSC_SIGNAL_BASE       0xe0   /* 0x70 * 2 */
#define ELSC_SIGNAL_CODE_COUNT 8      /* three-bit processor code */

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
     * Whether this code has console input queued decides signal1 versus
     * signal2 -- signal2 is what tells the driver's transaction helper a
     * "get" will find something, and withholding it for an empty queue is
     * the correct behaviour, not a missing feature. Only processor code 0
     * has a real queue: the ASCII protocol's "get"/"acp" selector is
     * hardcoded to it, and a second processor's own input path is not
     * modelled, so every other code always presents signal1.
     */
    elsc_broadcast(s, elsc_signal_byte(s->arb_code,
                   s->arb_code == 0 && s->rx_queue_len > 0));
    s->arb_present = false;
    s->arb_code = (s->arb_code + 1) % ELSC_SIGNAL_CODE_COUNT;
    timer_mod(s->arb_timer, now + ELSC_ARB_HOLD_US);
}

/*
 * The AUX line's one multiplexing rule (Origin 200 Owner's Guide, page
 * 132): a byte sequence beginning 0x14 is the controller's own command and
 * is never queued for "get"; everything else is. What ends an escaped
 * command is not stated anywhere read for the datasheet -- a human types
 * commands and presumably terminates them with Enter -- so this discards
 * through the next line terminator as a model choice, not an observed
 * behaviour, and it is the only part of this rule that is.
 */
static void elsc_receive(void *opaque, const uint8_t *buf, int size)
{
    SGIElscState *s = opaque;
    int i;

    for (i = 0; i < size; i++) {
        uint8_t c = buf[i];

        if (s->in_escape) {
            if (c == '\r' || c == '\n') {
                s->in_escape = false;
            }
            continue;
        }
        if (c == 0x14) {
            s->in_escape = true;
            continue;
        }
        if (s->rx_queue_len < sizeof(s->rx_queue)) {
            s->rx_queue[s->rx_queue_len++] = c;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-elsc: console input queue full, byte "
                          "dropped\n");
        }
    }
}

static int elsc_can_receive(void *opaque)
{
    SGIElscState *s = opaque;
    uint32_t room = sizeof(s->rx_queue) - s->rx_queue_len;

    return room < 32 ? (int)room : 32;
}

static void elsc_reset_hold(Object *obj, ResetType type)
{
    SGIElscState *s = SGI_ELSC(obj);

    s->rxlen = 0;
    s->txlen = 0;
    s->txpos = 0;
    s->arb_code = 0;
    s->arb_present = false;
    s->rx_queue_len = 0;
    s->in_escape = false;
    s->supervisor = false;
}

static void elsc_realize(DeviceState *dev, Error **errp)
{
    SGIElscState *s = SGI_ELSC(dev);

    s->bus = I2C_BUS(qdev_get_parent_bus(dev));
    s->arb_timer = timer_new_us(QEMU_CLOCK_VIRTUAL, elsc_arb_step, s);
    timer_mod(s->arb_timer, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));

    qemu_chr_fe_set_handlers(&s->chr, elsc_can_receive, elsc_receive,
                             NULL, NULL, s, NULL, true);
}

static const VMStateDescription vmstate_sgi_elsc = {
    .name = "sgi-elsc",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, SGIElscState),
        VMSTATE_UINT8_ARRAY(rxbuf, SGIElscState, SGI_ELSC_MAX_FRAME + 1),
        VMSTATE_UINT32(rxlen, SGIElscState),
        VMSTATE_UINT8_ARRAY(txbuf, SGIElscState, SGI_ELSC_MAX_FRAME),
        VMSTATE_UINT32(txlen, SGIElscState),
        VMSTATE_UINT32(txpos, SGIElscState),
        VMSTATE_TIMER_PTR(arb_timer, SGIElscState),
        VMSTATE_UINT32(arb_code, SGIElscState),
        VMSTATE_BOOL(arb_present, SGIElscState),
        VMSTATE_UINT8_ARRAY(rx_queue, SGIElscState, SGI_ELSC_RX_QUEUE_SIZE),
        VMSTATE_UINT32(rx_queue_len, SGIElscState),
        VMSTATE_BOOL(in_escape, SGIElscState),
        VMSTATE_BOOL(supervisor, SGIElscState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sgi_elsc_properties[] = {
    DEFINE_PROP_UINT32("num-cpus", SGIElscState, num_cpus, 1),
    DEFINE_PROP_CHR("chardev", SGIElscState, chr),
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
