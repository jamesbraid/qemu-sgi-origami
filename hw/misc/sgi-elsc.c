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
#include "qemu/main-loop.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "monitor/monitor.h"
#include "system/runstate.h"
#include "system/replay.h"
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

/* Name a committed deferred action for a guest-error log. */
static const char *elsc_action_name(uint8_t action)
{
    switch (action) {
    case ELSC_ACTION_RESET:
        return "reset";
    case ELSC_ACTION_CYCLE:
        return "power-cycle";
    case ELSC_ACTION_SHUTDOWN:
        return "shutdown";
    default:
        return "deferred action";
    }
}

/*
 * The module hooks. Each is optional, and with no module installed the
 * answers are the Origin 200's: this controller is the whole module, so it
 * can carry out its own actions and there is nobody to report the rest to.
 */
static bool elsc_module_can_execute(SGIElscState *s, SGIElscAction action)
{
    if (!s->module_ops || !s->module_ops->can_execute) {
        return true;
    }
    return s->module_ops->can_execute(s->module_opaque, action);
}

static void elsc_module_slave_active(SGIElscState *s, bool active)
{
    if (s->module_ops && s->module_ops->slave_active) {
        s->module_ops->slave_active(s->module_opaque, active);
    }
}

static void elsc_module_console_owner(SGIElscState *s, unsigned processor)
{
    if (s->module_ops && s->module_ops->console_owner) {
        s->module_ops->console_owner(s->module_opaque, processor);
    }
}

static void elsc_module_console_input(SGIElscState *s, bool pending)
{
    if (s->module_ops && s->module_ops->console_input) {
        s->module_ops->console_input(s->module_opaque, pending);
    }
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
    if (before && !s->rx_queue_len) {
        elsc_module_console_input(s, false);
    }
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
    /*
     * Writing console output claims the module's one alternate console: the
     * module's automatic input selection follows the processor that most
     * recently sent output to it.
     */
    if (n > 0 && s->resp_owner >= 0) {
        elsc_module_console_owner(s, (unsigned)s->resp_owner);
    }
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

/*
 * "dsp": overwrite the display from position 0, leaving the remaining
 * positions unaltered. What an over-length message does is unobserved;
 * rejecting it whole is the conservative choice.
 */
static void elsc_handle_dsp(SGIElscState *s, const char *text, unsigned len)
{
    unsigned n = len - 4;

    if (n > SGI_ELSC_DISPLAY_CHARS || memchr(&text[4], '\0', n)) {
        elsc_frame_response_str(s, "err arg");
        return;
    }
    memcpy(s->display, &text[4], n);
    elsc_frame_response_str(s, "ok");
}

/*
 * "dsc N C": one display character. Parsed positionally -- the firmware
 * sends "dsc %x %c", and C may be any ASCII character but nul, a space
 * included, so splitting on whitespace would reject a legal command.
 */
static void elsc_handle_dsc(SGIElscState *s, const char *text, unsigned len)
{
    if (len != 7 || text[3] != ' ' || text[5] != ' ' ||
        text[4] < '0' || text[4] > '7' || text[6] == '\0') {
        elsc_frame_response_str(s, "err arg");
        return;
    }
    s->display[text[4] - '0'] = text[6];
    elsc_frame_response_str(s, "ok");
}

/*
 * Parse a run of ASCII hex digits into *out; the text is not NUL-terminated.
 * Rejects an empty run and anything wider than 32 bits, so a caller can range
 * check the result. Most arguments in this protocol are hex.
 */
static bool elsc_parse_hex(const char *p, unsigned n, uint32_t *out)
{
    uint32_t v = 0;
    unsigned i;

    if (n == 0 || n > 8) {
        return false;
    }
    for (i = 0; i < n; i++) {
        char c = p[i];

        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v |= c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v |= c - 'A' + 10;
        } else {
            return false;
        }
    }
    *out = v;
    return true;
}

/*
 * "pwr <sub>": the power subcommands share the "pwr" mnemonic and select on
 * the letter after the space. Only the power-on query is boot-driven; the
 * datasheet lists the rest (delayed power-down, power-cycle, redundant
 * supply) as shutdown and reset-recovery paths. An unknown subcommand letter
 * is "err cmd"; a known letter with a bad argument is "err arg".
 */
static void elsc_handle_pwr(SGIElscState *s, const char *text, unsigned len)
{
    if (len < 5 || text[3] != ' ') {
        /* "pwr", "pwrx", "pwr ": no subcommand letter. */
        elsc_frame_response_str(s, "err cmd");
        return;
    }

    switch (text[4]) {
    case 'q':
        if (len != 5) {
            elsc_frame_response_str(s, "err arg");
            return;
        }
        /*
         * Power-on query: the PROM issues it once at power-on to detect a
         * cold start and seed a boot-status bit. Answered "ok 1" on the
         * first read after a cold power-on and "ok 0" thereafter. The model
         * clears the latch on read, so only the first caller in a power
         * cycle sees the 1; the datasheet records this clear-on-read as
         * 6.5.7-derived and unconfirmed for the 6.5.30 firmware we boot.
         *
         * The latch is per-controller. On the single-module Origin 200 this
         * model targets that is the whole machine. A multi-module machine
         * has one controller per module, each genuinely cold-powered; the
         * datasheet's design has the boot master read its own module's latch
         * once and propagate the result, so the others are not read. Whether
         * a multi-module cold-power latch is per-module or machine-wide is
         * not settled by the datasheet.
         */
        {
            char resp[6];

            snprintf(resp, sizeof(resp), "ok %x", s->cold_power_on ? 1 : 0);
            s->cold_power_on = false;
            elsc_frame_response_str(s, resp);
        }
        return;
    case 'd': {
        /*
         * "pwr d <sec>": delayed power-down, a destructive command --
         * supervisor-gated like "rst". The delay is hex seconds: 0 is
         * immediate, otherwise the guide's range 0x5 to 0x258 (5 to 600
         * seconds). A malformed form, a non-hex delay or an out-of-range
         * delay is "err arg", and all three are checked before the supervisor
         * gate: a bad argument is a bad argument whether or not the caller has
         * authenticated, and must not be reported as "err perm". Answer "ok",
         * then shut the machine down; like "rst" the action is deferred until
         * the ack is read (the deferred-action state machine). The scheduled
         * delay is not modelled -- the shutdown follows the ack.
         */
        uint32_t sec;

        if (len < 6 || text[5] != ' ' ||
            !elsc_parse_hex(&text[6], len - 6, &sec)) {
            elsc_frame_response_str(s, "err arg");
            return;
        }
        if (sec != 0 && (sec < 5 || sec > 0x258)) {
            elsc_frame_response_str(s, "err arg");
            return;
        }
        if (!s->supervisor) {
            elsc_frame_response_str(s, "err perm");
            return;
        }
        if (!elsc_module_can_execute(s, ELSC_ACTION_SHUTDOWN)) {
            elsc_frame_response_str(s, "err cmd");
            return;
        }
        elsc_frame_response_str(s, "ok");
        /*
         * Arm only if nothing is committed yet. A committed action is past
         * the point of no return; clearing deferred_committed here would
         * demote it to ARMED and the fetch-seam commit would be lost. The
         * committed action fires regardless; when it is weaker than this
         * request the returned "ok" does not describe the action performed,
         * and that mismatch is logged rather than passed over silently.
         */
        if (!s->deferred_committed) {
            s->deferred_action = ELSC_ACTION_SHUTDOWN;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-elsc: \"pwr d\" while a %s is already "
                          "committed; that action fires and this \"ok\" may "
                          "not match it\n",
                          elsc_action_name(s->deferred_action));
        }
        return;
    }
    case 'c':
        /*
         * "pwr c": power-cycle, destructive -- supervisor-gated like "rst".
         * Answer "ok", then reset the machine, deferred until the ack is read
         * (the deferred-action state machine). A power-cycle is a cold
         * restart, not a warm reset, so its bottom half also re-arms the
         * cold-power latch -- distinguished from "rst" by the CYCLE action.
         * Doing it when the action fires, not here, means a superseded
         * "pwr c" leaves no trace: no reset and no false cold-power latch.
         */
        if (len != 5) {
            elsc_frame_response_str(s, "err arg");
            return;
        }
        if (!s->supervisor) {
            elsc_frame_response_str(s, "err perm");
            return;
        }
        if (!elsc_module_can_execute(s, ELSC_ACTION_CYCLE)) {
            elsc_frame_response_str(s, "err cmd");
            return;
        }
        elsc_frame_response_str(s, "ok");
        /*
         * Arm only if nothing is committed yet. A committed action is past
         * the point of no return; clearing deferred_committed here would
         * demote it to ARMED and the fetch-seam commit would be lost. The
         * committed action fires regardless; when it is weaker than this
         * request the returned "ok" does not describe the action performed,
         * and that mismatch is logged rather than passed over silently.
         */
        if (!s->deferred_committed) {
            s->deferred_action = ELSC_ACTION_CYCLE;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-elsc: \"pwr c\" while a %s is already "
                          "committed; that action fires and this \"ok\" may "
                          "not match it\n",
                          elsc_action_name(s->deferred_action));
        }
        return;
    case 'r':
        /*
         * "pwr r": redundant-supply query, read-only and ungated. The wire
         * response is a per-module record for each supply, tagged
         * tag:present:health -- "m:y:ok"/"m:y:fl"/"m:n" for the master and
         * "s:y:ok"/"s:y:fl"/"s:n" for the slave. The present flag (y/n)
         * separates an absent supply ("m:n") from a failed one ("m:y:fl"),
         * and health is "ok" or "fl". Both supplies are modelled present and
         * healthy. The response payload is identical on the operator and wire
         * interfaces -- only the command framing differs -- so this is what
         * the Origin 200 diagnostics guide documents (datasheet PR #602). It
         * supersedes an earlier one-colon "ok m:ok s:ok" that came from the
         * driver's test-fixture mock rather than the firmware wire format.
         */
        if (len != 5) {
            elsc_frame_response_str(s, "err arg");
            return;
        }
        elsc_frame_response_str(s, "ok m:y:ok s:y:ok");
        return;
    default:
        /* An unrecognised subcommand letter is an unknown command. */
        elsc_frame_response_str(s, "err cmd");
        return;
    }
}

/* A non-empty run of ASCII decimal digits; the text is not NUL-terminated. */
static bool elsc_all_digits(const char *p, unsigned n)
{
    unsigned i;

    if (n == 0) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
    }
    return true;
}

/*
 * "hbt [<interval> <delay>]": heartbeat / NMI-watchdog control. "hbt" alone
 * sends one heartbeat, "hbt 0" disables monitoring, and "hbt <interval>
 * <delay>" enables it so a missed heartbeat raises a non-maskable interrupt
 * after the delay. The two arguments are decimal, unlike the hex arguments
 * elsewhere in this protocol. The kernel only enables the watchdog during
 * controller init, so validating the request and answering "ok" without
 * arming a timer carries a boot; no missed-heartbeat interrupt is modelled.
 */
static void elsc_handle_hbt(SGIElscState *s, const char *text, unsigned len)
{
    const char *args;
    const char *sp;
    unsigned rem;

    if (len == 3) {
        elsc_frame_response_str(s, "ok");   /* a single heartbeat */
        return;
    }
    if (text[3] != ' ') {
        /* "hbtx": junk after the mnemonic with no delimiter is not "hbt". */
        elsc_frame_response_str(s, "err cmd");
        return;
    }

    args = &text[4];
    rem = len - 4;
    sp = memchr(args, ' ', rem);
    if (sp == NULL) {
        /* The only valid one-argument form is "hbt 0" (disable). */
        if (rem != 1 || args[0] != '0') {
            elsc_frame_response_str(s, "err arg");
            return;
        }
    } else {
        /* "hbt <interval> <delay>": two args split on the space. */
        unsigned first = sp - args;

        if (!elsc_all_digits(args, first) ||
            !elsc_all_digits(sp + 1, rem - first - 1)) {
            elsc_frame_response_str(s, "err arg");
            return;
        }
        /*
         * The enable form arms an NMI watchdog the model does not
         * implement. Answering "ok" without arming a timer carries a boot
         * (the datasheet's own note), but flag that it is unmodelled.
         */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-elsc: heartbeat/NMI watchdog enable is not "
                      "modelled; answering ok without arming it\n");
    }

    elsc_frame_response_str(s, "ok");
}

/*
 * "tas <ch>" / "tas 0": the test-and-set lock cell. A distinct service
 * carried over the bus -- not the arbitration, which every command passes
 * through, this one included. "tas <ch>" takes the lock if it is free;
 * "tas 0" releases it.
 *
 * <ch> is a LITERAL character, not a hex value: the client stores the owner
 * and compares it byte for byte, so "tas A" acquires with owner 'A' and a
 * retry reads back "ok A" -- not "ok a" -- and any non-'0' byte is a valid
 * channel, with nothing to reject as a bad argument. Classic test-and-set:
 * "ok <held>" reports the byte the cell held BEFORE this request, so an
 * acquire of a free cell reads back "ok 0" and a caller polls until it reads
 * 0 (was free) or its own <ch> (already holds it). ASCII '0' is reserved as
 * the release channel, so it is never an owner and the free cell is
 * unambiguous.
 *
 * Occupancy (tas_held) is tracked apart from the owner byte (tas_lock): any
 * byte is a legal owner, NUL (0x00) included, so the owner value alone cannot
 * mean "free". A NUL owner must round-trip in the response, so the reply is
 * framed with an explicit length rather than as a C string, whose strlen
 * would truncate "ok \0" back to "ok ".
 *
 * The literal-byte round-trip is the test-and-set semantic the client's byte
 * comparison needs, regardless of the datasheet's "hex" wording, which is
 * flagged for the source owner. Read from the 6.5.7 driver's lock wrappers,
 * dead code there; confirm against 6.5.30.
 */
static void elsc_handle_tas(SGIElscState *s, const char *text, unsigned len)
{
    uint8_t ch;
    bool was_held;
    uint8_t held;
    char buf[4];

    if (len >= 4 && text[3] != ' ') {
        /* "tasx": junk after the mnemonic with no delimiter is not "tas". */
        elsc_frame_response_str(s, "err cmd");
        return;
    }
    if (len != 5) {
        /* "tas" takes exactly one channel character. */
        elsc_frame_response_str(s, "err arg");
        return;
    }
    ch = (uint8_t)text[4];

    was_held = s->tas_held;             /* occupancy before this request */
    held = s->tas_lock;                 /* owner before this request, if held */
    if (ch == '0') {
        s->tas_held = false;            /* release */
        s->tas_lock = 0;
    } else if (!s->tas_held) {
        s->tas_held = true;             /* free: this caller takes it */
        s->tas_lock = ch;
    }
    /* else: already held (by this caller or another); the cell is unchanged. */

    /*
     * "ok <held>": the owner byte before this request, or '0' if it was
     * free.
     */
    buf[0] = 'o';
    buf[1] = 'k';
    buf[2] = ' ';
    buf[3] = was_held ? held : '0';
    elsc_frame_response(s, buf, sizeof(buf));
}

/*
 * "nic <p> <s> <d>": one-wire identity passthrough. Three hex arguments
 * (pulse, sample, delay) bit-bang the midplane one-wire line; answers
 * "ok <hex>" with the sampled bit. No one-wire device is modelled, so the
 * open-drain line idles high and every sample reads 1. Interactive-monitor
 * command, not boot-driven.
 */
static void elsc_handle_nic(SGIElscState *s, const char *text, unsigned len)
{
    const char *end = &text[len];
    const char *arg, *sp1, *sp2;
    uint32_t v;

    if (len < 4 || text[3] != ' ') {
        /* "nic", "nicx": no space-delimited argument list -- not "nic". */
        elsc_frame_response_str(s, "err cmd");
        return;
    }
    arg = &text[4];
    sp1 = memchr(arg, ' ', end - arg);
    if (sp1 == NULL) {
        elsc_frame_response_str(s, "err arg");
        return;
    }
    sp2 = memchr(sp1 + 1, ' ', end - (sp1 + 1));
    if (sp2 == NULL) {
        elsc_frame_response_str(s, "err arg");
        return;
    }
    /* Parse all three as hex to validate; the sampled value is fixed. */
    if (!elsc_parse_hex(arg, sp1 - arg, &v) ||
        !elsc_parse_hex(sp1 + 1, sp2 - (sp1 + 1), &v) ||
        !elsc_parse_hex(sp2 + 1, end - (sp2 + 1), &v)) {
        elsc_frame_response_str(s, "err arg");
        return;
    }

    elsc_frame_response_str(s, "ok 1");
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
    if (len >= 4 && memcmp(text, "dsp ", 4) == 0) {
        elsc_handle_dsp(s, text, len);
        return;
    }
    if (len >= 4 && memcmp(text, "dsc ", 4) == 0) {
        elsc_handle_dsc(s, text, len);
        return;
    }
    if (len >= 3 && memcmp(text, "pas", 3) == 0) {
        elsc_handle_pas(s, text, len);
        return;
    }
    if (len >= 3 && memcmp(text, "pwr", 3) == 0) {
        elsc_handle_pwr(s, text, len);
        return;
    }
    if (len >= 3 && memcmp(text, "hbt", 3) == 0) {
        elsc_handle_hbt(s, text, len);
        return;
    }
    if (len >= 3 && memcmp(text, "tas", 3) == 0) {
        elsc_handle_tas(s, text, len);
        return;
    }
    if (len >= 3 && memcmp(text, "nic", 3) == 0) {
        elsc_handle_nic(s, text, len);
        return;
    }
    if (len == 3 && memcmp(text, "rst", 3) == 0) {
        /*
         * Module reset -- panic reboot and fatal-error recovery only, never
         * a normal boot. No delayed-reset form; the hex delay belongs to
         * "pwr d". Supervisor-gated. A caller treats "ok" as proof the reset
         * happened and does not fall back to its own hardware reset, so the
         * reset must fire. It is deferred until the guest has read "ok" --
         * committed at the fetch seam that completes the ack
         * (elsc_response_fully_read) and then run from a bottom half.
         *
         * The datasheet's reset is module-scoped. On the single-module
         * Origin 200 the module is the whole machine, so qemu_system_reset
         * is exact. A multi-node machine (the two-node bring-up is active)
         * has one controller per module, and this resets the whole VM --
         * broader than the hardware's per-module reset. QEMU has no
         * per-module reset, and the controller cannot even tell from its
         * own properties whether the machine is multi-node, so the model
         * keeps the whole-machine reset rather than refuse; a true
         * module-scoped reset would need a machine-level signal.
         */
        if (!s->supervisor) {
            elsc_frame_response_str(s, "err perm");
            return;
        }
        /*
         * A module owner decides what a reset reaches, and it is asked
         * before the acknowledgement is framed rather than after. Answering
         * "ok" for an action that will not happen is undetectable to the
         * caller, which takes the "ok" as proof and will not fall back to
         * its own hardware reset; "err cmd" leaves it that fallback.
         */
        if (!elsc_module_can_execute(s, ELSC_ACTION_RESET)) {
            elsc_frame_response_str(s, "err cmd");
            return;
        }
        elsc_frame_response_str(s, "ok");
        /*
         * Arm only if nothing is committed yet. A committed action is past
         * the point of no return; clearing deferred_committed here would
         * demote it to ARMED and the fetch-seam commit would be lost. The
         * committed action fires regardless; when it is weaker than this
         * request the returned "ok" does not describe the action performed,
         * and that mismatch is logged rather than passed over silently.
         */
        if (!s->deferred_committed) {
            s->deferred_action = ELSC_ACTION_RESET;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-elsc: \"rst\" while a %s is already committed; "
                          "that action fires and this \"ok\" may not match "
                          "it\n",
                          elsc_action_name(s->deferred_action));
        }
        return;
    }

    elsc_frame_response_str(s, "err cmd");
}

/* The direction switch to read marks the request complete. */
static void elsc_request_complete(SGIElscState *s)
{
    if (s->rxlen == 0 && s->txpos < s->txlen) {
        /* No request this transaction: a fetch of the pending response. */
        return;
    }

    /*
     * A new request supersedes any pending response. An ARMED action -- its
     * ack not fully read -- is canceled: consuming this new, unrelated response
     * must not fire it. A COMMITTED action is past the point of no return and
     * survives; the new request's own I2C_START_SEND already scheduled it (a
     * NACK-on-last-text-byte response reaches here that way, without a STOP or
     * a last-byte NACK). If this request is itself "rst", the dispatch below
     * sees the committed action and logs rather than re-arming.
     */
    if (!s->deferred_committed) {
        s->deferred_action = ELSC_ACTION_NONE;
    }

    /*
     * The response belongs to whoever asked: the CPU driving the bus
     * is current_cpu at the slave event, and its index is the global
     * processor code. A monitor or qtest access carries no processor
     * and attributes as the console processor.
     */
    s->resp_owner = current_cpu ? current_cpu->cpu_index : 0;

    if (s->rxlen < 2 || s->rxlen != (uint32_t)s->rxbuf[0] + 2 ||
        elsc_checksum(s->rxbuf, s->rxlen) != 0) {
        /* The protocol names no framing error distinct from "err csum". */
        elsc_frame_response_str(s, "err csum");
        return;
    }

    elsc_dispatch(s, (const char *)&s->rxbuf[1], s->rxbuf[0]);
}

/*
 * Carry out a COMMITTED deferred action once its "ok" has been read. Runs
 * from a bottom half, scheduled at the transaction boundary that follows the
 * fetch-seam commit (elsc_fire_committed_action, from a NACK, a STOP or a
 * following request) or, after a migration, by elsc_post_load.
 *
 * Every destructive action releases the I2C bus first. A response the master
 * ends by NACKing the last byte with no STOP -- it may fetch the byte in a
 * later transaction, or issue a same-address repeated START -- leaves this
 * controller matched on the bus: the STOP that would call i2c_end_transfer
 * never comes. The reset, cycle or shutdown resets the PCF8584 but not the
 * bus, so the stale match would misroute the next transaction -- an
 * identity-EEPROM read answered by this controller. A shutdown that
 * -no-shutdown or -action shutdown=pause turns into machine survival keeps
 * that wedged bus alive, so it needs the release just as a reset does.
 * i2c_end_transfer walks the bus's slave list and cannot run from inside an
 * I2C callback, and the reset must not run while the guest is still reading
 * the frame, which is why the whole action is deferred to this bottom half
 * rather than run when it is committed.
 *
 * The gate makes a stale schedule a no-op: only a committed action fires, and
 * firing clears the state, so a second scheduling -- another boundary of the
 * same response, or a post-migration re-arm racing the original -- does
 * nothing.
 */
static void elsc_deferred_action_bh(void *opaque)
{
    SGIElscState *s = opaque;
    uint8_t action = s->deferred_action;

    if (!s->deferred_committed) {
        return;
    }
    s->deferred_action = ELSC_ACTION_NONE;
    s->deferred_committed = false;
    s->deferred_scheduled = false;

    /*
     * Release the bus for every destructive action, before it fires. All
     * three leave this controller matched (the NACK-without-STOP path never
     * ended the transfer) and none of them ends it themselves, so a survived
     * shutdown wedges the bus exactly as a reset would.
     *
     * Which call releases it depends on the bus. i2c_end_transfer() is the
     * anonymous path and asserts that no participant ids are configured, so
     * a shared module bus needs its owning master named instead. Neither is
     * called on an idle bus: there is nothing matched to release, and no
     * owner is resolved between a stop and the next start.
     */
    if (i2c_bus_busy(s->bus)) {
        if (s->bus->master_count) {
            i2c_master_end_transfer(s->bus, s->bus->owner);
        } else {
            i2c_end_transfer(s->bus);
        }
    }

    switch (action) {
    case ELSC_ACTION_CYCLE:
        /*
         * A power-cycle is a cold restart, so re-arm the cold-power latch
         * for the post-reset "pwr q". elsc_reset_hold leaves the latch alone
         * (a warm "rst" must not re-arm it), so setting it here, before the
         * action fires, carries it through whoever carries it out.
         */
        s->cold_power_on = true;
        break;
    case ELSC_ACTION_RESET:
    case ELSC_ACTION_SHUTDOWN:
        break;
    default:
        /*
         * Only the three actions above can be committed; post_load rejects
         * any other value, so reaching here is a model bug.
         */
        g_assert_not_reached();
    }

    /*
     * A module owner decides what the action reaches -- the datasheet's
     * reset is module-scoped, and only the single-module Origin 200 can
     * read that as the whole machine.
     */
    if (s->module_ops && s->module_ops->execute) {
        s->module_ops->execute(s->module_opaque, action);
        return;
    }

    if (action == ELSC_ACTION_SHUTDOWN) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    } else {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

/*
 * The response has been fully handed to the guest: end the announcement and
 * mark any deferred action COMMITTED. Called from the fetch seam in
 * elsc_recv_internal -- the read that advances txpos to txlen.
 *
 * That seam is the exact point at which the ack is provably delivered. The
 * PCF8584 prefetches one byte ahead (pcf8584_read_data returns the held byte
 * and then fetches the next), so txpos reaches txlen precisely as the chip
 * pulls the frame's last byte -- the trailing checksum -- by which time the
 * guest has already been handed everything through the last text byte, i.e.
 * the entire "ok"/"err" ack. The one byte still in the read-ahead is only that
 * checksum, which carries no ack information.
 *
 * Committing at the fetch, not at a terminal event, is what closes the
 * last-text-byte NACK race: a master that clears ACK on the last text byte
 * fires I2C_NACK one byte early (txpos < txlen) and, if it then issues a
 * same-address repeated START with no STOP, produces no I2C_FINISH either --
 * so a commit tied to those events would miss the action and
 * elsc_request_complete would cancel it as still-ARMED. The fetch that follows
 * the NACK still runs (pcf8584_read_data fetches regardless of ACK), so the
 * seam catches it. An action whose ack was never fully read stays ARMED and a
 * superseding request cancels it; past this seam it is COMMITTED and survives
 * supersession and migration.
 *
 * Committing is only a flag -- no bus walk, no schedule -- so it is safe from
 * inside the recv callback. Carrying the action out is separate
 * (elsc_fire_committed_action, from the transaction's terminal events), so the
 * bottom half's i2c_end_transfer and reset run only once the guest has taken
 * the whole frame -- the checksum still sits one byte behind in the read-ahead
 * at this instant -- rather than resetting the chip mid-read and corrupting
 * that last byte.
 */
static void elsc_response_fully_read(SGIElscState *s)
{
    /* The announcement ends: nothing waits for this owner any more. */
    s->resp_owner = -1;

    /* COMMITTED: the ack is delivered, so the action can no longer cancel. */
    if (s->deferred_action != ELSC_ACTION_NONE) {
        s->deferred_committed = true;
    }
}

/*
 * Schedule a COMMITTED deferred action's bottom half. Called at a transaction
 * boundary -- the master's NACK, the STOP, or a following request -- so it runs
 * after the guest has taken the whole frame, not from inside the read that
 * completed the ack. deferred_scheduled records that the bottom half is queued,
 * so a migration re-arms exactly it (elsc_post_load); it is idempotent, and the
 * bottom half's own committed-flag guard makes all but the first run a no-op.
 * Scheduled through the replay-aware helper so a reset lands at the same
 * instruction under record/replay (docs/devel/replay.rst).
 */
static void elsc_fire_committed_action(SGIElscState *s)
{
    if (s->deferred_action != ELSC_ACTION_NONE && s->deferred_committed) {
        s->deferred_scheduled = true;
        replay_bh_schedule_oneshot_event(qemu_get_aio_context(),
                                         elsc_deferred_action_bh, s);
    }
}

static int elsc_event_internal(SGIElscState *s, enum i2c_event event)
{
    /*
     * A committed deferred action is carried out at the first bus event after
     * the seam that is not part of the recv data path. Every such event
     * schedules it (the bottom half's own committed-flag guard keeps that
     * idempotent), so whichever boundary a given response reaches -- a NACK, a
     * STOP, a repeated START in either direction -- fires it, and none fire it
     * mid-read. Only the recv data path itself does not schedule, because that
     * is where the guest is still taking the frame's bytes.
     */
    switch (event) {
    case I2C_START_SEND:
        elsc_module_slave_active(s, true);
        /* A new request is arriving; its START is one such boundary. */
        elsc_fire_committed_action(s);
        s->rxlen = 0;
        break;
    case I2C_START_RECV:
        elsc_module_slave_active(s, true);
        /*
         * Direction switch: the request just written is complete. A committed
         * action is not canceled here, so schedule it -- this covers a
         * same-address repeated START back into the receive direction after a
         * fully-fetched response, which reaches no other boundary.
         */
        elsc_request_complete(s);
        elsc_fire_committed_action(s);
        break;
    case I2C_NACK:
        /*
         * The master NACKed a byte to end the read; if an action committed at
         * the seam, the guest has taken the whole frame, so carry it out.
         * Scheduling at NACK is safe only because pcf8584_read_data captures
         * the held byte before it raises I2C_NACK, so the checksum reaches the
         * guest before anything schedules; were that read path reordered, this
         * boundary would move back inside the frame.
         */
        elsc_fire_committed_action(s);
        break;
    case I2C_FINISH:
        elsc_module_slave_active(s, false);
        s->rxlen = 0;
        /*
         * An unconsumed response stays pending for its requester --
         * announced by its signal 2 -- until read to its end or
         * superseded. The controller's own broadcasts deliver this
         * event too; discarding here would lose the response before
         * its owner could poll. A committed action fires here too, for a
         * response the master ends with a STOP rather than a bare NACK.
         */
        elsc_fire_committed_action(s);
        if (s->txpos >= s->txlen) {
            s->txlen = 0;
            s->txpos = 0;
            s->resp_owner = -1;
        }
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
        uint8_t data = s->txbuf[s->txpos++];

        if (s->txpos == s->txlen) {
            /*
             * The fetch seam: the last byte has just been handed out, so the
             * whole ack is delivered. End the announcement and commit any
             * deferred action here (see elsc_response_fully_read).
             */
            elsc_response_fully_read(s);
        }
        return data;
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
#define ELSC_ARB_SLOT_BASE_US  (10 * 1000)
#define ELSC_ARB_RETRY_US      20

/*
 * Slot lengths vary, 10.0 to 11.6ms, as a pure function of (code,
 * revolution) -- deterministic, so record/replay and migration stay
 * exact. A metronomic ring admits a stable phase lock from a guest
 * polling loop with a commensurate period, both clocks being the one
 * virtual clock; real controller and processor oscillators are not
 * synchronized. 17 is prime and coprime to the ring's eight codes and
 * to both multipliers, so each code walks all seventeen lengths across
 * revolutions and consecutive revolutions never repeat a ring period.
 */
static int64_t elsc_arb_slot_us(uint32_t code, uint32_t rev)
{
    uint32_t vary = (code * 7 + (rev % 17) * 13) % 17;

    return ELSC_ARB_SLOT_BASE_US + (int64_t)vary * 100;
}

/*
 * Whether something waits for the given code's processor: console
 * input for the console processor, or a response its requester has
 * not yet consumed.
 */
static bool elsc_arb_signal2(SGIElscState *s, uint32_t code)
{
    return (code == 0 && s->rx_queue_len > 0) ||
           (s->resp_owner >= 0 && code == (uint32_t)s->resp_owner &&
            s->txpos < s->txlen);
}

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
            /*
             * Re-present, so a poll after other traffic still finds
             * it -- recomputed, so signal 2 stands only while its
             * promise does: a slot whose response was consumed or
             * whose queue drained downgrades to signal 1.
             */
            bool signal2 = elsc_arb_signal2(s, s->arb_code);

            s->arb_signal = elsc_signal_byte(s->arb_code, signal2);
            elsc_broadcast(s, s->arb_signal);
            timer_mod(s->arb_timer, now + ELSC_ARB_REPRESENT_US);
            return;
        }
        /* The slot has run its length; the next code follows a gap. */
        s->arb_held = false;
        s->arb_code = (s->arb_code + 1) % ELSC_SIGNAL_CODE_COUNT;
        if (s->arb_code == 0) {
            s->arb_rev++;
        }
    }

    if (!s->arb_present) {
        elsc_broadcast(s, 0);
        s->arb_present = true;
        timer_mod(s->arb_timer, now + ELSC_ARB_GAP_US);
        return;
    }

    /* signal2 promises something waits; withheld when nothing does. */
    {
        bool signal2 = elsc_arb_signal2(s, s->arb_code);

        s->arb_signal = elsc_signal_byte(s->arb_code, signal2);
        trace_sgi_elsc_arb_step(s->cpu_base, s->arb_code, s->arb_signal,
                                signal2);
        elsc_broadcast(s, s->arb_signal);
    }
    s->arb_present = false;
    s->arb_held = true;
    s->arb_hold_until = now + elsc_arb_slot_us(s->arb_code, s->arb_rev);
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
    bool was_pending = s->rx_queue_len > 0;
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

    if (!was_pending && s->rx_queue_len) {
        elsc_module_console_input(s, true);
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
    s->resp_owner = -1;
    s->arb_code = 0;
    s->arb_rev = 0;
    s->arb_present = false;
    s->arb_held = false;
    s->arb_signal = 0;
    s->arb_hold_until = 0;
    s->rx_queue_len = 0;
    s->diagnostic_arbitration_broadcast = false;
    s->in_escape = false;
    s->supervisor = false;
    s->deferred_action = ELSC_ACTION_NONE;
    s->deferred_committed = false;
    s->deferred_scheduled = false;
    s->tas_held = false;
    s->tas_lock = 0;
    /* The power-on content is unstated; blank spaces are a model choice. */
    memset(s->display, ' ', sizeof(s->display));

    /* Tell the module what reset just made true of this controller. */
    elsc_module_slave_active(s, false);
    elsc_module_console_input(s, false);
}

void sgi_elsc_set_module_ops(SGIElscState *s, const SGIElscModuleOps *ops,
                             void *opaque)
{
    /* Wiring, not runtime state: reset and the bus already run off it. */
    assert(!DEVICE(s)->realized);
    s->module_ops = ops;
    s->module_opaque = opaque;
}

static void elsc_realize(DeviceState *dev, Error **errp)
{
    SGIElscState *s = SGI_ELSC(dev);

    s->bus = I2C_BUS(qdev_get_parent_bus(dev));
    /*
     * The cold power-on latch is set here rather than in reset because a
     * warm reset -- a guest "rst", or any system reset -- does not cycle
     * power and so must not re-arm it; only powering the controller up
     * does, which realize models. A migrated stream overwrites it on load.
     */
    s->cold_power_on = true;
    /*
     * The timer exists either way so that unrealize and the migration
     * stream keep one shape; only starting it makes the rotation run.
     */
    s->arb_timer = timer_new_us(QEMU_CLOCK_VIRTUAL, elsc_arb_step, s);
    if (s->arbitration) {
        timer_mod(s->arb_timer, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL));
    }

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

/*
 * The bottom half that carries out a committed action is not migrated. On
 * load, re-arm exactly the one the source had scheduled and not yet run --
 * deferred_scheduled records precisely that, and post_load re-arms iff it is
 * set. No reasoning about the frame is needed: the bit is set where a boundary
 * schedules the bottom half and cleared when the bottom half runs, so a stream
 * taken with it set is a bottom half lost in flight and one taken with it clear
 * is not.
 *
 * That leaves an ARMED action (ack unread, never scheduled) and a committed
 * action still waiting for its boundary (fully read but not yet closed, so not
 * scheduled) both alone, which is right: the ARMED one is committed and fired
 * when the guest reads its ack on the destination, and the waiting one is
 * scheduled by the destination's own closing boundary. Re-arming either here
 * would fire before its frame is finished.
 */
static int elsc_post_load(void *opaque, int version_id)
{
    SGIElscState *s = opaque;

    /*
     * Reject an inconsistent stream before it can drive the bottom half. A
     * pairing the model never produces -- an action past ELSC_ACTION_SHUTDOWN,
     * the committed flag set with no action, or the scheduled flag set without
     * the committed flag (scheduled is only ever set while committed) -- means
     * a corrupt or foreign stream, so fail the load rather than schedule a
     * bottom half that would hit g_assert_not_reached() or fire a phantom
     * action.
     */
    if (s->deferred_action > ELSC_ACTION_SHUTDOWN ||
        (s->deferred_committed && s->deferred_action == ELSC_ACTION_NONE) ||
        (s->deferred_scheduled && !s->deferred_committed)) {
        return -EINVAL;
    }

    if (s->deferred_scheduled) {
        replay_bh_schedule_oneshot_event(qemu_get_aio_context(),
                                         elsc_deferred_action_bh, s);
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_elsc = {
    .name = "sgi-elsc",
    .version_id = 11,
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
        /* An older stream restarts the slot-length sequence at zero. */
        VMSTATE_UINT32_V(arb_rev, SGIElscState, 5),
        /*
         * An older stream loads with no owner: a response in flight
         * stays passively served, unannounced, as the model that wrote
         * the stream behaved.
         */
        VMSTATE_INT32_V(resp_owner, SGIElscState, 6),
        /* An older stream loads the display zeroed, not blank. */
        VMSTATE_UINT8_ARRAY_V(display, SGIElscState,
                              SGI_ELSC_DISPLAY_CHARS, 7),
        /*
         * A pre-version-8 stream carried no latch and never answered
         * "pwr q"; it keeps realize's value (freshly powered), so a query
         * after such a migration reads the cold-power-on once.
         */
        VMSTATE_BOOL_V(cold_power_on, SGIElscState, 8),
        /*
         * A deferred destructive action, whether its ack has been read, and
         * whether its bottom half has been scheduled -- migrated so an action
         * accepted before a migration behaves correctly on the destination:
         * ARMED waits for the ack read, COMMITTED waits for its boundary, and a
         * SCHEDULED-but-unrun bottom half is re-armed on load (elsc_post_load).
         * A pre-version-9 stream loads all three zeroed -- no action pending.
         */
        VMSTATE_UINT8_V(deferred_action, SGIElscState, 9),
        VMSTATE_BOOL_V(deferred_committed, SGIElscState, 9),
        VMSTATE_BOOL_V(deferred_scheduled, SGIElscState, 9),
        /*
         * The lock cell. tas_held is the occupancy and tas_lock the owner
         * byte; both are needed because any byte -- NUL included -- is a
         * legal owner, so the owner value alone cannot mean "free". A
         * pre-version-11 stream loads tas_held false (the cell free),
         * regardless of any tas_lock byte a version-10 stream carried.
         */
        VMSTATE_UINT8_V(tas_lock, SGIElscState, 10),
        VMSTATE_BOOL_V(tas_held, SGIElscState, 11),
        VMSTATE_END_OF_LIST()
    },
    .post_load = elsc_post_load,
};

static const Property sgi_elsc_properties[] = {
    DEFINE_PROP_UINT32("num-cpus", SGIElscState, num_cpus, 1),
    DEFINE_PROP_UINT32("cpu-base", SGIElscState, cpu_base, 0),
    DEFINE_PROP_UINT8("dip-switches", SGIElscState, dip_switches, 0),
    /*
     * The synthesized arbitration rotation, on by default. An Origin 2000
     * module publishes real address-only grants from its module controller,
     * onto a bus with identified participants, which the broadcast's own
     * anonymous transfer calls assert against, so a module switches it off.
     */
    DEFINE_PROP_BOOL("arbitration", SGIElscState, arbitration, true),
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

    sc->supports_master_ids = true;
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
    bool resp_pending = s->resp_owner >= 0 && s->txpos < s->txlen;
    bool code0_signal2 = console_input ||
                         (resp_pending && s->resp_owner == 0);
    uint8_t console_signal = elsc_signal_byte(0, code0_signal2);
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
    g_string_append(buf, "  display: \"");
    for (i = 0; i < SGI_ELSC_DISPLAY_CHARS; i++) {
        uint8_t c = s->display[i];

        if (c >= 0x20 && c < 0x7f) {
            g_string_append_c(buf, c);
        } else {
            g_string_append_printf(buf, "\\x%02x", c);
        }
    }
    g_string_append(buf, "\"\n");
    if (resp_pending) {
        g_string_append_printf(buf,
                               "  pending response: %u byte(s) for "
                               "processor code %d\n",
                               s->txlen - s->txpos, s->resp_owner);
    } else {
        g_string_append(buf, "  pending response: none\n");
    }
    g_string_append_printf(buf,
                           "  arbitration rotation: code %u of %u, %s\n",
                           s->arb_code, ELSC_SIGNAL_CODE_COUNT,
                           s->arb_held ? "holding signal"
                           : s->arb_present ? "showing gap" : "advancing");
    g_string_append_printf(buf,
                           "  console signal (code 0): 0x%02x = signal%d"
                           " (console input %s)\n",
                           console_signal, code0_signal2 ? 2 : 1,
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
