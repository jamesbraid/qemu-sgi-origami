/*
 * SGI Origin 200 module system controller (MSC)
 *
 * Copyright (c) 2026 Origami contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Checksummed ASCII command/response channel, bus-arbitration broadcaster
 * and AUX serial endpoint. The protocol, vocabulary and arbitration rules
 * are specified in docs/specs/sgi-elsc.rst.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "monitor/monitor.h"
#include "qapi/error.h"
#include "qapi/type-helpers.h"
#include "trace.h"

/* Firmware ignores the token but its first eight bytes must fit a buffer. */
#define SGI_ELSC_VERSION "origami"
QEMU_BUILD_BUG_ON(sizeof(SGI_ELSC_VERSION) > 8);

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
 * A frame above SGI_ELSC_MAX_TEXT overruns the firmware's own buffer; a
 * model invariant, not guest-triggerable, so asserted.
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
 * "get": firmware checks four of the six header bytes and copies from byte
 * six, so the shape must be exact; the selector is always "0".
 */
static void elsc_handle_get(SGIElscState *s)
{
    char buf[SGI_ELSC_MAX_TEXT];
    unsigned payload_max = SGI_ELSC_MAX_TEXT - 6;
    unsigned n = s->rx_queue_len < payload_max ? s->rx_queue_len : payload_max;
    uint32_t before = s->rx_queue_len;

    memcpy(buf, "acp 0 ", 6);
    memcpy(&buf[6], s->rx_queue, n);
    elsc_frame_response(s, buf, 6 + n);

    memmove(s->rx_queue, &s->rx_queue[n], s->rx_queue_len - n);
    s->rx_queue_len -= n;
    trace_sgi_elsc_get(before, s->rx_queue_len, n);
    if (n) {
        trace_sgi_elsc_rx_dequeue(n, s->rx_queue_len);
    }
    if (s->diagnostic && n) {
        device_diagnostic_record(s->diagnostic, "elsc", "console-dequeue",
                                 before, n, 1, true, before,
                                 s->rx_queue_len);
        if (!s->rx_queue_len) {
            device_diagnostic_record(s->diagnostic, "elsc",
                                     "arbitration-end", before, n, 1, true,
                                     before, 0);
        }
    }
}

/* "acp": a bare "ok" parses as zero accepted and livelocks the firmware. */
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
    trace_sgi_elsc_acp(n, text[4]);
    /* Post-diagnostic console output lands here; warn once if discarded. */
    if (n > 0 && !qemu_chr_fe_backend_connected(&s->chr) &&
        !s->aux_no_backend_warned) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-elsc: guest wrote console output to the ELSC AUX "
                      "port but no character device is attached, so it is "
                      "being discarded. The firmware redirects its console "
                      "here after a failed power-on diagnostic, so this is "
                      "diagnostic output -- attach a backend on serial_hd(2) "
                      "to capture it. The IRIX console (console=d) is IOC3 "
                      "serial A (serial_hd(0)), not this port.\n");
        s->aux_no_backend_warned = true;
    }
    qemu_chr_fe_write_all(&s->chr, (const uint8_t *)&text[6], n);

    snprintf(resp, sizeof(resp), "ok %x", n);
    elsc_frame_response_str(s, resp);
}

/* "pas": supervisor gate; the factory default password is "none". */
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
    if (len == 3 && memcmp(text, "rsw", 3) == 0) {
        char resp[6];

        /* The controller reports its physical switches active-low. */
        snprintf(resp, sizeof(resp), "ok %02x", 0xff ^ s->dip_switches);
        elsc_frame_response_str(s, resp);
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
        /* Whether "rst" is the real mnemonic is not established. */
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

/* The direction switch to read marks the request complete. */
static void elsc_request_complete(SGIElscState *s)
{
    if (s->rxlen < 2 || s->rxlen != (uint32_t)s->rxbuf[0] + 2 ||
        elsc_checksum(s->rxbuf, s->rxlen) != 0) {
        /* The protocol names no framing error distinct from "err csum". */
        elsc_frame_response_str(s, "err csum");
        return;
    }

    elsc_dispatch(s, (const char *)&s->rxbuf[1], s->rxbuf[0]);
}

static int elsc_event_internal(SGIElscState *s, enum i2c_event event)
{
    switch (event) {
    case I2C_START_SEND:
        /* A request is arriving. */
        s->rxlen = 0;
        break;
    case I2C_START_RECV:
        /* Direction switch: the request just written is complete. */
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

static int elsc_event(I2CSlave *i2c, enum i2c_event event)
{
    SGIElscState *s = SGI_ELSC(i2c);
    uint64_t before;
    const char *operation;
    int ret;

    if (likely(!s->diagnostic)) {
        return elsc_event_internal(s, event);
    }
    if (s->diagnostic_arbitration_broadcast) {
        return elsc_event_internal(s, event);
    }

    switch (event) {
    case I2C_START_SEND:
        operation = "start-send";
        break;
    case I2C_START_RECV:
        operation = "start-receive";
        break;
    case I2C_FINISH:
        operation = "finish";
        break;
    default:
        return elsc_event_internal(s, event);
    }

    before = ((uint64_t)s->rxlen << 32) |
             ((uint64_t)s->txlen << 16) | s->txpos;
    ret = elsc_event_internal(s, event);
    device_diagnostic_record(
        s->diagnostic, "elsc", operation, event, 0, 1, true, before,
        ((uint64_t)s->rxlen << 32) | ((uint64_t)s->txlen << 16) | s->txpos);
    return ret;
}

static int elsc_send_internal(SGIElscState *s, uint8_t data)
{

    if (s->rxlen >= sizeof(s->rxbuf)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-elsc: request longer than the protocol allows, "
                      "byte dropped\n");
        return 0;
    }

    s->rxbuf[s->rxlen++] = data;
    return 0;
}

static int elsc_send(I2CSlave *i2c, uint8_t data)
{
    SGIElscState *s = SGI_ELSC(i2c);
    uint64_t before;
    int ret;

    if (likely(!s->diagnostic)) {
        return elsc_send_internal(s, data);
    }
    if (s->diagnostic_arbitration_broadcast) {
        return elsc_send_internal(s, data);
    }

    before = s->rxlen;
    ret = elsc_send_internal(s, data);
    if (s->rxlen != before) {
        device_diagnostic_record(s->diagnostic, "elsc", "send", before,
                                 data, 1, true, before, s->rxlen);
    }
    return ret;
}

static uint8_t elsc_recv_internal(SGIElscState *s)
{

    if (s->txpos < s->txlen) {
        return s->txbuf[s->txpos++];
    }

    /* The driver's own length byte said stop; this is over-reading. */
    return 0xff;
}

static uint8_t elsc_recv(I2CSlave *i2c)
{
    SGIElscState *s = SGI_ELSC(i2c);
    uint64_t before;
    uint8_t value;

    if (likely(!s->diagnostic)) {
        return elsc_recv_internal(s);
    }
    if (s->diagnostic_arbitration_broadcast) {
        return elsc_recv_internal(s);
    }

    before = s->txpos;
    value = elsc_recv_internal(s);
    if (s->txpos != before) {
        device_diagnostic_record(s->diagnostic, "elsc", "receive", before,
                                 value, 1, true, before, s->txpos);
    }
    return value;
}

/*
 * Three-bit code: signal2 is signal1 with bit 3 set, so eight or more
 * collides with another processor's signal1.
 */
#define ELSC_SIGNAL_BASE       0xe0   /* 0x70 * 2 */
#define ELSC_SIGNAL_CODE_COUNT 8      /* three-bit processor code */

static uint8_t elsc_signal_byte(unsigned processor_code, bool signal2)
{
    uint8_t signal1 = ELSC_SIGNAL_BASE + processor_code;

    return signal2 ? signal1 + 8 : signal1;
}

/* One broadcast of value; an empty bus is a no-op. */
static void elsc_broadcast(SGIElscState *s, uint8_t value)
{
    bool diagnostic = s->diagnostic != NULL;

    if (diagnostic) {
        s->diagnostic_arbitration_broadcast = true;
    }
    if (i2c_start_transfer(s->bus, I2C_BROADCAST, false)) {
        if (diagnostic) {
            s->diagnostic_arbitration_broadcast = false;
        }
        return;
    }
    i2c_send(s->bus, value);
    i2c_end_transfer(s->bus);
    if (diagnostic) {
        s->diagnostic_arbitration_broadcast = false;
    }
}

/*
 * Model choices: a gap a polling guest can observe; a held signal
 * re-presented for its whole slot, a duration no plausible poll cadence
 * can miss. The rotation is purely timer-driven -- on the bus a waiting
 * processor's own command retries are indistinguishable from its
 * answer, so no traffic ends a hold early. The real controller's
 * pacing is unobserved; see the bus arbitration section of
 * docs/specs/sgi-elsc.rst.
 */
#define ELSC_ARB_GAP_US        50
#define ELSC_ARB_REPRESENT_US  50
#define ELSC_ARB_SLOT_US       (10 * 1000)
#define ELSC_ARB_RETRY_US      20

/*
 * The gap is required: firmware must see its signal absent before it
 * trusts it. The hold is required too: guest poll and rotation share
 * one deterministic virtual clock, so a one-shot presentation can
 * phase-lock with the poll and a processor then never samples its own
 * slot. Deferral is by retry -- the PCF8584 never calls
 * i2c_bus_release(), so queueing would wait forever.
 */
static void elsc_arb_step(void *opaque)
{
    SGIElscState *s = opaque;
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);

    if (i2c_bus_busy(s->bus)) {
        /* Never mid-transaction: hold and rotation wait out the guest. */
        timer_mod(s->arb_timer, now + ELSC_ARB_RETRY_US);
        return;
    }

    if (s->arb_held) {
        if (now < s->arb_hold_until) {
            /* Re-present, so a poll after other traffic still finds it. */
            elsc_broadcast(s, s->arb_signal);
            timer_mod(s->arb_timer, now + ELSC_ARB_REPRESENT_US);
            return;
        }
        /* The slot has run its length; the next code follows a gap. */
        s->arb_held = false;
        s->arb_code = (s->arb_code + 1) % ELSC_SIGNAL_CODE_COUNT;
    }

    if (!s->arb_present) {
        elsc_broadcast(s, 0);
        s->arb_present = true;
        timer_mod(s->arb_timer, now + ELSC_ARB_GAP_US);
        return;
    }

    /* signal2 promises a "get" will find something; withheld when empty. */
    {
        bool signal2 = s->arb_code == 0 && s->rx_queue_len > 0;

        s->arb_signal = elsc_signal_byte(s->arb_code, signal2);
        trace_sgi_elsc_arb_step(s->cpu_base, s->arb_code, s->arb_signal,
                                signal2);
        elsc_broadcast(s, s->arb_signal);
    }
    s->arb_present = false;
    s->arb_held = true;
    s->arb_hold_until = now + ELSC_ARB_SLOT_US;
    timer_mod(s->arb_timer, now + ELSC_ARB_REPRESENT_US);
}

/*
 * Bytes after 0x14 are the controller's own command (Owner's Guide p.132);
 * discarding to the line terminator is a model choice.
 */
static void elsc_receive(void *opaque, const uint8_t *buf, int size)
{
    SGIElscState *s = opaque;
    bool diagnostic = unlikely(s->diagnostic);
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
            uint32_t before = s->rx_queue_len;

            s->rx_queue[s->rx_queue_len++] = c;
            trace_sgi_elsc_rx_enqueue(c, s->rx_queue_len);
            if (diagnostic) {
                if (!before) {
                    device_diagnostic_record(
                        s->diagnostic, "elsc", "arbitration-start", 0, c,
                        1, true, 0, 1);
                }
                device_diagnostic_record(
                    s->diagnostic, "elsc", "console-enqueue", before, c,
                    1, true, before, s->rx_queue_len);
            }
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
    s->arb_held = false;
    s->arb_signal = 0;
    s->arb_hold_until = 0;
    s->rx_queue_len = 0;
    s->diagnostic_arbitration_broadcast = false;
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

static void elsc_unrealize(DeviceState *dev)
{
    SGIElscState *s = SGI_ELSC(dev);

    /* timer_free() also cancels a pending expiry before freeing. */
    timer_free(s->arb_timer);
    s->arb_timer = NULL;
    qemu_chr_fe_set_handlers(&s->chr, NULL, NULL, NULL, NULL, NULL, NULL,
                             true);
}

static const VMStateDescription vmstate_sgi_elsc = {
    .name = "sgi-elsc",
    .version_id = 4,
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
        VMSTATE_UINT8_V(dip_switches, SGIElscState, 3),
        /*
         * A version-2 or -3 stream loads with these zeroed: no hold in
         * progress, and the rotation resumes from the pending gap.
         */
        VMSTATE_BOOL_V(arb_held, SGIElscState, 4),
        VMSTATE_UINT8_V(arb_signal, SGIElscState, 4),
        VMSTATE_INT64_V(arb_hold_until, SGIElscState, 4),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sgi_elsc_properties[] = {
    DEFINE_PROP_UINT32("num-cpus", SGIElscState, num_cpus, 1),
    DEFINE_PROP_UINT32("cpu-base", SGIElscState, cpu_base, 0),
    DEFINE_PROP_UINT8("dip-switches", SGIElscState, dip_switches, 0),
    DEFINE_PROP_CHR("chardev", SGIElscState, chr),
};

static void sgi_elsc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    dc->desc = "SGI Origin 200 module system controller (MSC)";
    dc->realize = elsc_realize;
    dc->unrealize = elsc_unrealize;
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

/* "info elsc": queue and arbitration state without any trace backend. */
static void elsc_format_state(SGIElscState *s, GString *buf)
{
    bool console_input = s->rx_queue_len > 0;
    uint8_t console_signal = elsc_signal_byte(0, console_input);
    g_autofree char *path = object_get_canonical_path(OBJECT(s));
    uint32_t preview = MIN(s->rx_queue_len, 64);
    uint32_t i;

    g_string_append_printf(buf, "%s\n", path ? path : TYPE_SGI_ELSC);
    g_string_append_printf(buf,
                           "  rx queue: %u byte(s) waiting for \"get\"\n",
                           s->rx_queue_len);
    if (preview) {
        g_string_append(buf, "    input: \"");
        for (i = 0; i < preview; i++) {
            uint8_t c = s->rx_queue[i];

            if (c >= 0x20 && c < 0x7f) {
                g_string_append_c(buf, c);
            } else {
                g_string_append_printf(buf, "\\x%02x", c);
            }
        }
        g_string_append(buf, preview < s->rx_queue_len ? "\"...\n" : "\"\n");
    }
    g_string_append_printf(buf,
                           "  arbitration rotation: code %u of %u, %s\n",
                           s->arb_code, ELSC_SIGNAL_CODE_COUNT,
                           s->arb_held ? "holding signal"
                           : s->arb_present ? "showing gap" : "advancing");
    g_string_append_printf(buf,
                           "  console signal (code 0): 0x%02x = signal%d"
                           " (console input %s)\n",
                           console_signal, console_input ? 2 : 1,
                           console_input ? "waiting" : "idle");
}

static int elsc_info_iter(Object *obj, void *opaque)
{
    SGIElscState *s = (SGIElscState *)object_dynamic_cast(obj, TYPE_SGI_ELSC);

    if (s) {
        elsc_format_state(s, opaque);
    }
    return 0;
}

static HumanReadableText *elsc_info(Error **errp)
{
    g_autoptr(GString) buf = g_string_new("");

    object_child_foreach_recursive(object_get_root(), elsc_info_iter, buf);
    if (buf->len == 0) {
        g_string_append(buf, "no sgi-elsc controller present\n");
    }
    return human_readable_text_from_str(buf);
}

static void elsc_hmp_register(void)
{
    monitor_register_hmp_info_hrt("elsc", elsc_info);
}

type_init(elsc_hmp_register);
