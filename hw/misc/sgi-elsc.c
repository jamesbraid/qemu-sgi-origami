/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Origin 200 MSC: checksummed ASCII command/reply, arbitration grants and AUX
 * serial.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/i2c/i2c-wire.h"
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

/*
 * Each build's version field, all eight characters of it. Firmware ignores
 * the token but its first eight bytes must fit a buffer.
 */
#define SGI_ELSC_VERSION_ORIGIN200  "V 3.1S  "
#define SGI_ELSC_VERSION_ORIGIN2000 "VER 3.2 "
QEMU_BUILD_BUG_ON(sizeof(SGI_ELSC_VERSION_ORIGIN200) != 9);
QEMU_BUILD_BUG_ON(sizeof(SGI_ELSC_VERSION_ORIGIN2000) != 9);

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
 * A frame above SGI_ELSC_MAX_TEXT overruns the firmware's own buffer; a model
 * invariant, not guest-triggerable, so asserted.
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
 * A success with no value: the firmware's three bytes "ok ", trailing space
 * included. IRIX accepts "ok" followed by a space or the end of the text.
 */
static void elsc_frame_ok(SGIElscState *s)
{
    elsc_frame_response_str(s, "ok ");
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

/* The "tas" cell's free value. */
#define ELSC_TAS_FREE '0'

/*
 * One request as the controller parses it. The firmware keeps the text and
 * puts a carriage return where the checksum byte was, so every command ends
 * in one; text[len - 1] is always that carriage return. pos is the next
 * unread byte, and the readers below never move it past the end.
 */
typedef struct ElscRequest {
    const char *text;
    unsigned len;
    unsigned pos;
} ElscRequest;

static char elsc_peek(const ElscRequest *r)
{
    return r->pos < r->len ? r->text[r->pos] : '\r';
}

static void elsc_skip(ElscRequest *r)
{
    if (r->pos < r->len) {
        r->pos++;
    }
}

/*
 * The character reader most commands use: skip spaces and take the next
 * character. A carriage return or line feed is no argument, and is left
 * where it is.
 */
static bool elsc_read_char(ElscRequest *r, char *c)
{
    while (elsc_peek(r) == ' ') {
        elsc_skip(r);
    }
    *c = elsc_peek(r);
    if (*c == '\r' || *c == '\n') {
        return false;
    }
    elsc_skip(r);
    return true;
}

/*
 * The hex reader: up to four digits, either case, with no leading spaces
 * skipped. A number shorter than four digits consumes the character that
 * ends it, so a following number must come after exactly one space; a
 * four-digit number consumes nothing after it, so the next number follows
 * with no space at all, and a fifth digit starts it. No digits at all reads
 * as no value, and the character that ended the attempt is still consumed.
 */
static bool elsc_read_hex(ElscRequest *r, uint32_t *value)
{
    unsigned digits = 0;

    *value = 0;
    while (digits < 4 && g_ascii_isxdigit(elsc_peek(r))) {
        *value = *value << 4 | g_ascii_xdigit_value(elsc_peek(r));
        elsc_skip(r);
        digits++;
    }
    if (digits < 4) {
        elsc_skip(r);
    }
    return digits > 0;
}

/*
 * "get": firmware checks four of the six header bytes and copies from byte
 * six, so the shape must be exact; the selector is always "0".
 */
static void elsc_handle_get(SGIElscState *s, ElscRequest *r)
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
}

/*
 * "acp": a bare "ok" parses as zero accepted and livelocks the firmware. The
 * selector and payload are taken from fixed offsets, as the firmware's output
 * parser does, with no validation of either.
 */
static void elsc_handle_acp(SGIElscState *s, ElscRequest *r)
{
    const char *text = r->text;
    unsigned len = r->len - 1;
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

static void elsc_handle_ver(SGIElscState *s, ElscRequest *r)
{
    elsc_frame_response_str(s, s->origin2000 ?
                            "ok " SGI_ELSC_VERSION_ORIGIN2000 :
                            "ok " SGI_ELSC_VERSION_ORIGIN200);
}

/* The physical switches, active-low, as hex with no leading zero. */
static void elsc_handle_rsw(SGIElscState *s, ElscRequest *r)
{
    char resp[6];

    snprintf(resp, sizeof(resp), "ok %x", 0xff ^ s->dip_switches);
    elsc_frame_response_str(s, resp);
}

/*
 * "dsp <text>": the Origin 2000 build's eight-character front-panel message.
 * The text starts right after the space that ends the mnemonic, so spaces in
 * it count. Up to eight characters overwrite the display from its first
 * position and leave the rest alone. The text stops at the first unprintable
 * character, the carriage return included, and anything past eight is
 * dropped. An empty message is a bad argument.
 */
static void elsc_handle_dsp(SGIElscState *s, ElscRequest *r)
{
    unsigned n = 0;

    while (r->pos + n < r->len && n < SGI_ELSC_DISPLAY_CHARS &&
           g_ascii_isprint(r->text[r->pos + n])) {
        n++;
    }
    if (n == 0) {
        elsc_frame_response_str(s, "err arg");
        return;
    }
    memcpy(s->display, &r->text[r->pos], n);
    elsc_frame_ok(s);
}

/*
 * "dsc <pos> <ch>": one display character on the Origin 2000 build. Both
 * arguments come from the character reader, so the position is the digit 1
 * to 8, for display positions 0 to 7, and the character is printable and
 * never a space. The PROM sends "dsc %x %c" with positions 2s-1 and 2s for
 * slot s.
 */
static void elsc_handle_dsc(SGIElscState *s, ElscRequest *r)
{
    char pos, ch;

    if (!elsc_read_char(r, &pos) || pos < '1' || pos > '8' ||
        !elsc_read_char(r, &ch) || !g_ascii_isprint(ch)) {
        elsc_frame_response_str(s, "err arg");
        return;
    }
    s->display[pos - '1'] = ch;
    elsc_frame_ok(s);
}

/*
 * Arm a destructive action whose "ok" has just been framed. Clearing
 * deferred_committed would demote an acknowledged action to ARMED and lose
 * the commit after the last text byte is fetched. The committed action
 * still fires; log if it is weaker than this request and the returned "ok"
 * therefore describes another action.
 */
static void elsc_arm_action(SGIElscState *s, SGIElscAction action,
                            const char *command)
{
    if (!s->deferred_committed) {
        s->deferred_action = action;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-elsc: \"%s\" while a %s is already committed; "
                      "that action fires and this \"ok\" may not match it\n",
                      command, elsc_action_name(s->deferred_action));
    }
}

/*
 * "pwr d [<sec>]": power down after a hex delay. Exactly one character after
 * the "d" is skipped, then the hex reader runs, so no digit at all is no
 * argument. 1 to 0x258 (re)loads the countdown from this request, so a
 * repeated request moves the deadline either way. 0 powers down at once, as
 * does no argument while a countdown is pending; no argument otherwise
 * starts a three-second countdown. Above 0x258 is "err arg" and changes
 * nothing.
 *
 * An immediate power-down replies "ok" first and acts once the ack is read,
 * as "rst" does. A countdown's expiry needs no ack.
 */
static void elsc_handle_pwr_down(SGIElscState *s, ElscRequest *r)
{
    uint32_t sec;
    bool has_delay, immediate;

    elsc_skip(r);
    has_delay = elsc_read_hex(r, &sec);
    if (has_delay && sec > SGI_ELSC_DELAY_MAX) {
        elsc_frame_response_str(s, "err arg");
        return;
    }
    if (!elsc_module_can_execute(s, ELSC_ACTION_SHUTDOWN)) {
        elsc_frame_response_str(s, "err cmd");
        return;
    }
    elsc_frame_ok(s);

    immediate = has_delay ? sec == 0 : timer_pending(s->pwr_down_timer);
    if (!immediate) {
        timer_mod(s->pwr_down_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)(has_delay ? sec : 3) * 1000);
        return;
    }
    /*
     * A pending countdown keeps running: the firmware removes power whether
     * or not this ack is read, so if a new request supersedes the ack before
     * it commits, the countdown still powers down.
     */
    elsc_arm_action(s, ELSC_ACTION_SHUTDOWN, "pwr d");
}

/*
 * "pwr <sub>": the power subcommands share the "pwr" mnemonic and select on
 * a letter from the character reader, so "pwr q" and "pwr  q junk" are the
 * same request. Only the power-on query is boot-driven; the rest are
 * shutdown and reset-recovery paths. A letter the controller does not know
 * is a bad argument, "pwr r" included: the redundant-supply query belongs to
 * controller firmware 3.3S and later, and neither build modelled here has
 * it. IRIX treats the error as "no supply report".
 */
static void elsc_handle_pwr(SGIElscState *s, ElscRequest *r)
{
    char sub;

    if (!elsc_read_char(r, &sub)) {
        elsc_frame_response_str(s, "err arg");
        return;
    }

    switch (sub) {
    case 'q':
        /*
         * Power-on query: the PROM issues it once at power-on to detect a
         * cold start and seed a boot-status bit. Answered "ok 1" on the first
         * read after a cold power-on and "ok 0" thereafter. The model clears
         * the latch on read, so only the first caller in a power cycle sees
         * the 1; this clear-on-read behavior is 6.5.7-derived and unconfirmed
         * for the 6.5.30 firmware we boot.
         *
         * The latch is per-controller. On the single-module Origin 200 this
         * model targets that is the whole machine. A multi-module machine has
         * one controller per module, each genuinely cold-powered; by design
         * the boot master reads its own module's latch once and propagates
         * the result, so the others are not read. Whether a multi-module
         * cold-power latch is per-module or machine-wide is not settled.
         */
        {
            char resp[6];

            snprintf(resp, sizeof(resp), "ok %x", s->cold_power_on ? 1 : 0);
            s->cold_power_on = false;
            elsc_frame_response_str(s, resp);
        }
        return;
    case 'd':
        elsc_handle_pwr_down(s, r);
        return;
    case 'c': {
        /*
         * "pwr c [<sec>]": the optional off interval is read like the
         * "pwr d" delay and runs from 5 to 0x258 seconds. The model restarts
         * at once whatever it is (see the specification's power section).
         *
         * "pwr c" replies "ok" before the reset, as "rst" does. CYCLE is a
         * cold restart: its bottom half re-arms the cold-power latch when
         * the action fires after the ack. A superseded request causes
         * neither reset nor false cold-power bit.
         */
        uint32_t sec;

        elsc_skip(r);
        if (elsc_read_hex(r, &sec) &&
            (sec < 5 || sec > SGI_ELSC_DELAY_MAX)) {
            elsc_frame_response_str(s, "err arg");
            return;
        }
        if (!elsc_module_can_execute(s, ELSC_ACTION_CYCLE)) {
            elsc_frame_response_str(s, "err cmd");
            return;
        }
        elsc_frame_ok(s);
        elsc_arm_action(s, ELSC_ACTION_CYCLE, "pwr c");
        return;
    }
    default:
        elsc_frame_response_str(s, "err arg");
        return;
    }
}

/*
 * "hbt [<interval> [<cycle>]]": heartbeat / NMI-watchdog control, both
 * arguments read by the hex reader. "hbt" with no readable value sends one
 * heartbeat and "hbt 0" disables monitoring. Otherwise the interval is 5 to
 * 0x258 seconds, and the optional second value sets how long after the NMI
 * the controller waits before power-cycling the module. The kernel sends
 * "hbt 30 20" only when its heartbeat tunable is set, which IRIX 6.5.30
 * leaves off, so the request is validated and answered without arming a
 * timer; no missed-heartbeat NMI or power cycle is modelled.
 */
static void elsc_handle_hbt(SGIElscState *s, ElscRequest *r)
{
    uint32_t interval;

    if (!elsc_read_hex(r, &interval) || interval == 0) {
        elsc_frame_ok(s);   /* a heartbeat, or monitoring disabled */
        return;
    }
    if (interval < 5 || interval > SGI_ELSC_DELAY_MAX) {
        elsc_frame_response_str(s, "err arg");
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "sgi-elsc: heartbeat/NMI watchdog enable is not "
                  "modelled; answering ok without arming it\n");
    elsc_frame_ok(s);
}

/*
 * "tas [<ch>]": the test-and-set lock cell. A distinct service carried over
 * the bus; not the arbitration, which every command passes through, this
 * one included.
 *
 * The cell is one byte, free when it holds ASCII '0'. The argument is the
 * next non-space character, a LITERAL byte rather than a hex value, so "tas
 * A" and "tas  A" both offer 'A' and a space can never be an owner. "tas 0"
 * frees the cell; any other byte, NUL included, takes it only if it is free
 * and otherwise leaves the holder in place. With no argument the command
 * only reports. Every reply is "ok " and the byte the cell held before the
 * request, so a caller polls until it reads '0' (it was free and is now
 * taken) or its own byte (it already held it).
 *
 * The reply is framed with an explicit length because a NUL owner must
 * round-trip; strlen would cut "ok \0" back to "ok ". No IRIX 6.5.30 binary
 * is known to send "tas".
 */
static void elsc_handle_tas(SGIElscState *s, ElscRequest *r)
{
    char ch;
    char buf[4] = { 'o', 'k', ' ', (char)s->tas_lock };

    if (elsc_read_char(r, &ch)) {
        if (ch == ELSC_TAS_FREE) {
            s->tas_lock = ELSC_TAS_FREE;
        } else if (s->tas_lock == ELSC_TAS_FREE) {
            s->tas_lock = (uint8_t)ch;
        }
    }
    elsc_frame_response(s, buf, sizeof(buf));
}

/*
 * "nic <p> <s> <d>": one-wire identity passthrough. Three values from the
 * hex reader (pulse, sample, delay), any of them missing a bad argument,
 * bit-bang the midplane one-wire line; answers "ok <hex>" with the sampled
 * bit. A controller without a module-provided line retains the absent-part
 * result: the open-drain line idles high.
 */
static void elsc_handle_nic(SGIElscState *s, ElscRequest *r)
{
    uint32_t pulse, sample, delay;
    bool level = true;
    char response[] = "ok 1";

    if (!elsc_read_hex(r, &pulse) || !elsc_read_hex(r, &sample) ||
        !elsc_read_hex(r, &delay)) {
        elsc_frame_response_str(s, "err arg");
        return;
    }

    if (s->module_ops && s->module_ops->onewire_slot) {
        level = s->module_ops->onewire_slot(s->module_opaque, pulse, sample);
    }
    trace_sgi_elsc_nic(pulse, sample, delay,
                       s->module_ops && s->module_ops->onewire_slot, level);
    response[3] = level ? '1' : '0';
    elsc_frame_response_str(s, response);
}

/*
 * Module reset: panic reboot and fatal-error recovery only, never a normal
 * boot. A caller treats "ok" as proof the reset happened and does not fall
 * back to its own hardware reset, so the reset must fire. It is deferred
 * until the guest has read "ok": committed at the fetch of the last frame
 * byte that completes the ack (elsc_response_fully_read) and then run from a
 * bottom half.
 *
 * The hardware reset is module-scoped. On the single-module Origin 200 the
 * module is the whole machine, so qemu_system_reset is exact. A multi-node
 * machine has one controller per module, and this resets the whole VM;
 * QEMU has no per-module reset, so the model keeps the whole-machine reset
 * rather than refuse.
 *
 * A module owner decides what a reset reaches, and it is asked before the
 * acknowledgement is framed rather than after. Answering "ok" for an action
 * that will not happen is undetectable to the caller, which takes the "ok"
 * as proof and will not fall back to its own hardware reset; "err cmd"
 * leaves it that fallback.
 */
static void elsc_handle_rst(SGIElscState *s, ElscRequest *r)
{
    if (!elsc_module_can_execute(s, ELSC_ACTION_RESET)) {
        elsc_frame_response_str(s, "err cmd");
        return;
    }
    elsc_frame_ok(s);
    elsc_arm_action(s, ELSC_ACTION_RESET, "rst");
}

/* Which builds have a command in their table. */
#define ELSC_BUILD_ORIGIN200    (1u << 0)
#define ELSC_BUILD_ORIGIN2000   (1u << 1)
#define ELSC_BUILD_BOTH         (ELSC_BUILD_ORIGIN200 | ELSC_BUILD_ORIGIN2000)

typedef struct ElscCommand {
    char mnemonic[4];
    unsigned builds;
    void (*handler)(SGIElscState *s, ElscRequest *r);
} ElscCommand;

/*
 * What each build answers over I2C. A mnemonic a build lacks answers
 * "err cmd" with no other effect, and so do the serial-only commands
 * ("cfg", "dbg", "ech", "fre", "mod", "nvm", "pas"), which never appear here.
 */
static const ElscCommand elsc_commands[] = {
    { "acp", ELSC_BUILD_BOTH,       elsc_handle_acp },
    { "dsc", ELSC_BUILD_ORIGIN2000, elsc_handle_dsc },
    { "dsp", ELSC_BUILD_ORIGIN2000, elsc_handle_dsp },
    { "get", ELSC_BUILD_BOTH,       elsc_handle_get },
    { "hbt", ELSC_BUILD_BOTH,       elsc_handle_hbt },
    { "nic", ELSC_BUILD_BOTH,       elsc_handle_nic },
    { "pwr", ELSC_BUILD_BOTH,       elsc_handle_pwr },
    { "rst", ELSC_BUILD_BOTH,       elsc_handle_rst },
    { "rsw", ELSC_BUILD_BOTH,       elsc_handle_rsw },
    { "tas", ELSC_BUILD_BOTH,       elsc_handle_tas },
    { "ver", ELSC_BUILD_BOTH,       elsc_handle_ver },
};

/*
 * The first four bytes are a three-letter lowercase mnemonic and its
 * separator, which must be a space or the carriage return. Matching is
 * case-sensitive, and a request that is only the terminator is no command.
 * Whatever a handler leaves unread is discarded.
 */
static void elsc_dispatch(SGIElscState *s, const char *text, unsigned len)
{
    char line[SGI_ELSC_MAX_FRAME];
    unsigned build = s->origin2000 ? ELSC_BUILD_ORIGIN2000 :
                                     ELSC_BUILD_ORIGIN200;
    ElscRequest r;
    unsigned i;

    memcpy(line, text, len);
    line[len] = '\r';
    r.text = line;
    r.len = len + 1;
    r.pos = 4;

    if (len < 3 || (line[3] != ' ' && line[3] != '\r')) {
        elsc_frame_response_str(s, "err cmd");
        return;
    }
    if (line[3] == '\r') {
        r.pos = 3;
    }
    for (i = 0; i < ARRAY_SIZE(elsc_commands); i++) {
        const ElscCommand *c = &elsc_commands[i];

        if (memcmp(line, c->mnemonic, 3) == 0 && (c->builds & build)) {
            c->handler(s, &r);
            return;
        }
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
     * A new request supersedes any pending response. An ARMED action (its
     * ack not fully read) is canceled: consuming this new, unrelated
     * response must not fire it. A COMMITTED action is past the point of no
     * return and survives; the new request's own I2C_START_SEND already
     * scheduled it (a NACK-on-last-text-byte response reaches here that way,
     * without a STOP or a last-byte NACK). If this request is itself "rst",
     * the dispatch below sees the committed action and logs rather than
     * re-arming.
     */
    if (!s->deferred_committed) {
        s->deferred_action = ELSC_ACTION_NONE;
    }

    /*
     * The response belongs to whoever asked: the CPU driving the bus is
     * current_cpu at the slave event, and its index is the global processor
     * code. A monitor or qtest access carries no processor, so attribute it
     * to slice A of this controller's module.
     */
    s->resp_owner = current_cpu ? current_cpu->cpu_index : s->cpu_base;

    if (s->rxlen < 2 || s->rxlen != (uint32_t)s->rxbuf[0] + 2 ||
        elsc_checksum(s->rxbuf, s->rxlen) != 0) {
        /* The protocol names no framing error distinct from "err csum". */
        elsc_frame_response_str(s, "err csum");
        return;
    }

    elsc_dispatch(s, (const char *)&s->rxbuf[1], s->rxbuf[0]);
}

/*
 * elsc_fire_committed_action schedules this after the ack's last text byte is
 * fetched, at NACK, STOP or the next request. It runs as requesting-CPU work
 * through elsc_deferred_action_cpu, or a bottom half after elsc_post_load or
 * qtest access without a running guest CPU.
 *
 * NACK without STOP, including a later byte fetch or same-address repeated
 * START, leaves this slave matched: PCF8584 reset does not reset the I2C bus.
 * A later identity-EEPROM read could reach this controller. Even shutdown may
 * leave the machine running under -no-shutdown or -action shutdown=pause, so
 * reset, cycle and shutdown all release the bus first. i2c_end_transfer walks
 * slaves and cannot run inside an I2C callback; reset also must wait until
 * the guest finishes the frame.
 *
 * Only COMMITTED actions fire. Firing clears state, so another boundary or
 * post-migration re-arm cannot execute the same action twice.
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
     * Only the anonymous bus is released here. i2c_end_transfer() asserts
     * that no participant ids are configured, and the named alternative
     * cannot help: i2c_master_owns() is false for every wire-mode bus, and a
     * wire bus has no resolved owner between a stop and the next start. Such
     * a bus ends its transfer at the STOP its master drives.
     */
    if (!i2c_bus_master_count(s->bus) && i2c_bus_busy(s->bus)) {
        i2c_end_transfer(s->bus);
    }

    switch (action) {
    case ELSC_ACTION_CYCLE:
        /*
         * A power-cycle is a cold restart, so re-arm the cold-power latch for
         * the post-reset "pwr q". elsc_reset_hold leaves the latch alone (a
         * warm "rst" must not re-arm it), so setting it here, before the
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
     * A module owner decides what the action reaches; the hardware reset is
     * module-scoped, and only the single-module Origin 200 can read that as
     * the whole machine.
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
 * elsc_recv_internal calls this when txpos reaches txlen. PCF8584 returns the
 * held byte before fetching the next, so the guest has the last text byte and
 * full "ok"/"err" ack when it prefetches the trailing checksum. That checksum
 * carries no ack information. End the announcement and commit here.
 *
 * NACK on the last text byte arrives early (txpos < txlen). A same-address
 * repeated START without STOP then raises no I2C_FINISH. PCF8584 still
 * fetches after NACK, so this commit survives; committing only at NACK/FINISH
 * would leave the action ARMED for elsc_request_complete to cancel. Unread
 * acks stay ARMED and can be superseded; committed actions survive
 * supersession and migration.
 *
 * This callback sets a flag only. elsc_fire_committed_action runs at a
 * terminal event; its bottom half ends the I2C transfer and resets after the
 * guest takes the checksum, avoiding a reset during the read.
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
 * Run a COMMITTED action from the requesting processor's own queued work,
 * then stop every processor, mirroring sgi_hub_request_machine_reset() in
 * hw/misc/sgi-hub.c for the Hub's port-reset write (#606). Queued work runs
 * at the writer's next translation-block boundary, outside the execution
 * loop, which is where a processor may be stopped. Round-robin execution
 * reaches it before any peer is scheduled again, so nothing else executes
 * past the write that committed the action; multi-threaded execution cannot
 * promise that, since the peers run on their own host threads, and this only
 * bounds them to their own next block boundary rather than to whenever the
 * main loop would otherwise have performed the reset.
 *
 * Only the no-module-owner case pauses every processor here. Where a module
 * decides what the action reaches (module_ops->execute for the
 * Origin 2000 module complex), stopping processors is that module's call, not
 * this controller's.
 */
static void elsc_deferred_action_cpu(CPUState *cpu, run_on_cpu_data data)
{
    SGIElscState *s = data.host_ptr;
    bool module_owned = s->module_ops != NULL;
    CPUState *peer;

    elsc_deferred_action_bh(s);

    if (!module_owned) {
        CPU_FOREACH(peer) {
            cpu_pause(peer);
        }
    }
}

/*
 * Fire a COMMITTED deferred action. Called at a transaction boundary (the
 * master's NACK, the STOP, or a following request) so it runs after the
 * guest has taken the whole frame, not from inside the read that completed
 * the ack. deferred_scheduled records that firing is queued, so a migration
 * re-arms exactly it (elsc_post_load); the action's own committed-flag guard
 * makes all but the first run a no-op.
 *
 * A live transaction reaches this device through a guest access, so
 * current_cpu names the processor that drove it: queue the action on that
 * processor's own work (elsc_deferred_action_cpu) rather than a main-loop
 * bottom half, so every processor the action must stop is still running when
 * the work runs and none of them can get ahead of it. A bottom half left the
 * moment of the reset to whenever the main loop next ran, with every other
 * processor free to keep executing until then. This caused #607.
 *
 * Without a current processor there is no writer to queue the work on: the
 * bottom half re-armed after a migration (elsc_post_load), or a qtest access
 * with no processor running guest code. Fall back to the bottom half
 * there, scheduled through the replay-aware helper so it lands at a
 * reproducible point under record/replay.
 */
static void elsc_fire_committed_action(SGIElscState *s)
{
    if (s->deferred_action != ELSC_ACTION_NONE && s->deferred_committed) {
        s->deferred_scheduled = true;
        if (current_cpu) {
            async_run_on_cpu(current_cpu, elsc_deferred_action_cpu,
                             RUN_ON_CPU_HOST_PTR(s));
        } else {
            replay_bh_schedule_oneshot_event(qemu_get_aio_context(),
                                             elsc_deferred_action_bh, s);
        }
    }
}

/*
 * The "pwr d" countdown ran out: remove power. Its "ok" was read long ago,
 * so the shutdown commits at once and runs from a bottom half. It
 * overrides an action whose ack is still unread; one already committed
 * fires instead, and the shutdown is logged as lost.
 */
static void elsc_pwr_down_expired(void *opaque)
{
    SGIElscState *s = opaque;

    if (s->deferred_committed) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-elsc: \"pwr d\" countdown expired while a %s is "
                      "committed; that action fires instead\n",
                      elsc_action_name(s->deferred_action));
        return;
    }
    s->deferred_action = ELSC_ACTION_SHUTDOWN;
    s->deferred_committed = true;
    elsc_fire_committed_action(s);
}

static int elsc_event_internal(SGIElscState *s, enum i2c_event event)
{
    /*
     * A committed deferred action is carried out at the first bus event after
     * that fetch that is not part of the recv data path. Every such event
     * schedules it (the bottom half's own committed-flag guard keeps that
     * idempotent), so whichever boundary a given response reaches (a NACK,
     * a STOP, a repeated START back to this controller or away to another
     * address) fires it, and none fire it mid-read. Only the recv data path
     * itself does not schedule, because that is where the guest is still
     * taking the frame's bytes.
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
         * action is not canceled here, so schedule it; this covers a
         * same-address repeated START back into the receive direction after a
         * fully-fetched response, which reaches no other boundary.
         */
        elsc_request_complete(s);
        elsc_fire_committed_action(s);
        break;
    case I2C_RESTART:
        /*
         * A repeated START to somebody else's address. The controller is no
         * longer addressed and will see no STOP, so this is its last event in
         * the transaction: it has to report itself idle here or a module
         * scheduler waiting on that would wait forever, and it has to carry
         * out a committed action here or the closing resync to the invalid
         * address would swallow one whose "ok" the guest already has.
         */
        elsc_module_slave_active(s, false);
        elsc_fire_committed_action(s);
        break;
    case I2C_NACK:
        /*
         * The master NACKed a byte to end the read; if an action committed at
         * that fetch, the guest has taken the whole frame, so carry it out.
         * Scheduling at NACK is safe only because pcf8584_read_data captures
         * the held byte before it raises I2C_NACK, so the checksum reaches
         * the guest before anything schedules; were that read path reordered,
         * this boundary would move back inside the frame.
         */
        elsc_fire_committed_action(s);
        break;
    case I2C_FINISH:
        elsc_module_slave_active(s, false);
        s->rxlen = 0;
        /*
         * An unconsumed response stays pending for its requester (announced
         * by its signal 2) until read to its end or superseded. The
         * controller's own broadcasts deliver this event too; discarding here
         * would lose the response before its owner could poll. A committed
         * action fires here too, for a response the master ends with a STOP
         * rather than a bare NACK.
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

    return elsc_event_internal(s, event);
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

    return elsc_send_internal(s, data);
}

static uint8_t elsc_recv_internal(SGIElscState *s)
{

    if (s->txpos < s->txlen) {
        uint8_t data = s->txbuf[s->txpos++];

        if (s->txpos == s->txlen) {
            /*
             * The fetch of the last frame byte: the last byte has just been
             * handed out, so the whole ack is delivered. End the announcement
             * and commit any deferred action here (see
             * elsc_response_fully_read).
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

    return elsc_recv_internal(s);
}

/*
 * Three-bit code: signal2 is signal1 with bit 3 set, so eight or more
 * collides with another processor's signal1.
 */
#define ELSC_SIGNAL_BASE         0xe0   /* 0x70 * 2 */
#define ELSC_RING_POSITION_COUNT 8      /* three-bit position field */
/*
 * The Origin 200 controller's ring: processor codes 0 to 3, then the
 * slave-controller position. That is stated for two modules; a single
 * module using the same one counter is an inference.
 */
#define ELSC_ORIGIN200_POSITIONS 5

static uint8_t elsc_signal_byte(unsigned ring_position, bool signal2)
{
    uint8_t signal1 = ELSC_SIGNAL_BASE + ring_position;

    return signal2 ? signal1 + 8 : signal1;
}

/* Publish one address-only grant; the grant address is not acknowledged. */
static void elsc_publish_grant_on(I2CBus *bus, uint8_t value)
{
    i2c_start_transfer(bus, value >> 1, value & 1);
    i2c_end_transfer(bus);
}

static void elsc_publish_grant(SGIElscState *s, uint8_t value)
{
    elsc_publish_grant_on(s->bus, value);
    if (s->arb_peer) {
        elsc_publish_grant_on(s->arb_peer->bus, value);
    }
}

/*
 * The controller attempts one grant per one-millisecond periodic tick, so each
 * position stands for one tick and a ring of n positions turns in n
 * milliseconds.
 */
#define ELSC_ARB_TICK_US 1000

/*
 * Whether something waits for the given code's processor: console input for
 * the console processor, or a response its requester has not yet consumed.
 */
static bool elsc_arb_signal2(SGIElscState *s, uint32_t ring_position)
{
    return (ring_position == 0 && s->rx_queue_len > 0) ||
           (s->resp_owner >= 0 &&
            ring_position == (uint32_t)s->resp_owner &&
            s->txpos < s->txlen);
}

static bool elsc_shared_arb_signal2(SGIElscState *s,
                                    uint32_t ring_position)
{
    SGIElscState *owner = s;

    if (s->arb_peer && ring_position >= s->arb_peer->cpu_base &&
        ring_position < s->arb_peer->cpu_base + s->arb_peer->num_cpus) {
        owner = s->arb_peer;
    }
    return elsc_arb_signal2(owner, ring_position);
}

/*
 * One periodic tick: publish the next position's grant, which stays readable
 * until the next tick replaces it. A tick that finds either bus busy publishes
 * nothing and does not advance, and the controller waits for its next tick
 * rather than publishing as soon as the bus frees.
 */
static void elsc_arb_step(void *opaque)
{
    SGIElscState *s = opaque;
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);
    uint32_t code = s->arb_code;
    bool signal2;

    /*
     * Arm from now rather than from the missed deadline: a tick the host runs
     * late must not be followed at once by another that cuts the processor's
     * hold short.
     */
    timer_mod(s->arb_timer, now + ELSC_ARB_TICK_US);

    if (i2c_bus_busy(s->bus) ||
        (s->arb_peer && i2c_bus_busy(s->arb_peer->bus))) {
        return;
    }

    signal2 = elsc_shared_arb_signal2(s, code);
    s->arb_signal = elsc_signal_byte(code, signal2);
    trace_sgi_elsc_arb_step(s->cpu_base, code, s->arb_signal, signal2);
    elsc_publish_grant(s, s->arb_signal);

    s->arb_code = (code + 1) % s->arb_positions;
}

/*
 * Bytes after 0x14 are the controller's own command (Owner's Guide p.132);
 * discarding to the line terminator is a model choice.
 */
static void elsc_receive(void *opaque, const uint8_t *buf, int size)
{
    SGIElscState *s = opaque;
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
            s->rx_queue[s->rx_queue_len++] = c;
            trace_sgi_elsc_rx_enqueue(c, s->rx_queue_len);
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

/*
 * The controller's own reinitialisation, which its recovery timeout and its
 * reset and power paths reach. Reinitialisation clears the protocol queues
 * and nothing else, so the framing state goes and the rest stays: the
 * display, the lock and any committed destructive action are not queues,
 * and no source says a reinitialisation disturbs them. An in-flight
 * request is abandoned. An action already acknowledged to a guest still
 * happens.
 *
 * Two of the fields cleared here are a model choice rather than a contract.
 * Queued alternate-console input goes with the framing, and so does the
 * message indication that announces it, but nothing establishes that a
 * five-second I2C recovery drops characters a terminal already sent.
 * Retaining them would be as defensible. They are named here because a test
 * now asserts that they go.
 */
void sgi_elsc_reinitialize(SGIElscState *s)
{
    s->rxlen = 0;
    s->txlen = 0;
    s->txpos = 0;
    s->resp_owner = -1;
    s->rx_queue_len = 0;
    s->in_escape = false;
    elsc_module_slave_active(s, false);
    elsc_module_console_input(s, false);
}

static void elsc_reset_hold(Object *obj, ResetType type)
{
    SGIElscState *s = SGI_ELSC(obj);

    s->rxlen = 0;
    s->txlen = 0;
    s->txpos = 0;
    s->resp_owner = -1;
    s->arb_code = 0;
    s->arb_signal = 0;
    s->rx_queue_len = 0;
    s->in_escape = false;
    s->deferred_action = ELSC_ACTION_NONE;
    s->deferred_committed = false;
    s->deferred_scheduled = false;
    timer_del(s->pwr_down_timer);
    s->tas_lock = ELSC_TAS_FREE;
    /* The power-on content is unstated; blank spaces are a model choice. */
    memset(s->display, ' ', sizeof(s->display));

    /* Tell the module what reset just made true of this controller. */
    elsc_module_slave_active(s, false);
    elsc_module_console_input(s, false);
}

/*
 * The rotation starts with code 0 at the first reset, and a later reset
 * leaves a running rotation on its schedule. The first step runs here
 * rather than from a timer armed at the current time. Such a timer is
 * already expired when replay's initial snapshot is taken, and when
 * QEMU services an expired virtual timer depends on host scheduling, so
 * replays from that snapshot could diverge from the recording. Every
 * step after this one is armed in the future.
 *
 * This is the exit phase so that both buses the step publishes on, and the
 * shared rotation's peer, have finished their own reset.
 */
static void elsc_reset_exit(Object *obj, ResetType type)
{
    SGIElscState *s = SGI_ELSC(obj);

    if (s->arbitration && !timer_pending(s->arb_timer)) {
        elsc_arb_step(s);
    }
}

void sgi_elsc_set_module_ops(SGIElscState *s, const SGIElscModuleOps *ops,
                             void *opaque)
{
    /* Wiring, not runtime state: reset and the bus already run off it. */
    assert(!DEVICE(s)->realized);
    s->module_ops = ops;
    s->module_opaque = opaque;
}

void sgi_elsc_share_arbitration(SGIElscState *master, SGIElscState *slave,
                                unsigned int positions)
{
    assert(DEVICE(master)->realized);
    assert(DEVICE(slave)->realized);
    assert(master->arbitration);
    assert(!slave->arbitration);
    assert(master != slave);
    assert(!master->arb_peer);
    assert(positions > 0 && positions <= ELSC_RING_POSITION_COUNT);

    master->arb_peer = slave;
    master->arb_positions = positions;
}

static void elsc_realize(DeviceState *dev, Error **errp)
{
    SGIElscState *s = SGI_ELSC(dev);

    if (!g_strcmp0(s->build, "origin2000")) {
        s->origin2000 = true;
    } else if (!s->build || !g_strcmp0(s->build, "origin200")) {
        s->origin2000 = false;
    } else {
        error_setg(errp, "sgi-elsc: build must be origin200 or origin2000, "
                   "not '%s'", s->build);
        return;
    }
    s->bus = I2C_BUS(qdev_get_parent_bus(dev));
    s->arb_positions = ELSC_ORIGIN200_POSITIONS;
    /*
     * The cold power-on latch is set here rather than in reset because a warm
     * reset (a guest "rst", or any system reset) does not cycle power and
     * so must not re-arm it; only powering the controller up does, which
     * realize models. A migrated stream overwrites it on load.
     */
    s->cold_power_on = true;
    s->tas_lock = ELSC_TAS_FREE;
    /*
     * The timer exists either way so that unrealize and the migration stream
     * keep one shape; only starting it makes the rotation run, which the
     * first reset does (elsc_reset_exit).
     */
    s->arb_timer = timer_new_us(QEMU_CLOCK_VIRTUAL, elsc_arb_step, s);
    s->pwr_down_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                     elsc_pwr_down_expired, s);

    qemu_chr_fe_set_handlers(&s->chr, elsc_can_receive, elsc_receive,
                             NULL, NULL, s, NULL, true);
}

static void elsc_unrealize(DeviceState *dev)
{
    SGIElscState *s = SGI_ELSC(dev);

    /* timer_free() also cancels a pending expiry before freeing. */
    timer_free(s->arb_timer);
    s->arb_timer = NULL;
    timer_free(s->pwr_down_timer);
    s->pwr_down_timer = NULL;
    qemu_chr_fe_set_handlers(&s->chr, NULL, NULL, NULL, NULL, NULL, NULL,
                             true);
}

/*
 * The bottom half that carries out a committed action is not migrated. On
 * load, re-arm exactly the one the source had scheduled and not yet run.
 * deferred_scheduled records precisely that, and post_load re-arms iff it is
 * set. No reasoning about the frame is needed: the bit is set where a
 * boundary schedules the bottom half and cleared when the bottom half runs,
 * so a stream taken with it set is a bottom half lost in flight and one taken
 * with it clear is not.
 *
 * That leaves an ARMED action (ack unread, never scheduled) and a committed
 * action still waiting for its boundary (fully read but not yet closed, so
 * not scheduled) both alone, which is right: the ARMED one is committed and
 * fired when the guest reads its ack on the destination, and the waiting one
 * is scheduled by the destination's own closing boundary. Re-arming either
 * here would fire before its frame is finished.
 */
static int elsc_post_load(void *opaque, int version_id)
{
    SGIElscState *s = opaque;

    /* The framing indexes address fixed buffers; a corrupt stream stops. */
    if (s->rxlen > sizeof(s->rxbuf) || s->txlen > sizeof(s->txbuf) ||
        s->txpos > s->txlen) {
        return -EINVAL;
    }
    if (s->arb_code >= ELSC_RING_POSITION_COUNT) {
        return -EINVAL;
    }
    if (s->arb_code >= s->arb_positions) {
        return -EINVAL;
    }

    /*
     * Reject an inconsistent stream before it can drive the bottom half. A
     * pairing the model never produces (an action past
     * ELSC_ACTION_SHUTDOWN, the committed flag set with no action, or the
     * scheduled flag set without the committed flag (scheduled is only ever
     * set while committed)) means a corrupt or foreign stream, so fail the
     * load rather than schedule a bottom half that would hit
     * g_assert_not_reached() or fire a phantom action.
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
    .version_id = 16,
    .minimum_version_id = 16,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, SGIElscState),
        VMSTATE_UINT8_ARRAY(rxbuf, SGIElscState, SGI_ELSC_MAX_FRAME + 1),
        VMSTATE_UINT32(rxlen, SGIElscState),
        VMSTATE_UINT8_ARRAY(txbuf, SGIElscState, SGI_ELSC_MAX_FRAME),
        VMSTATE_UINT32(txlen, SGIElscState),
        VMSTATE_UINT32(txpos, SGIElscState),
        VMSTATE_TIMER_PTR(arb_timer, SGIElscState),
        VMSTATE_UINT32(arb_code, SGIElscState),
        VMSTATE_UINT8_ARRAY(rx_queue, SGIElscState, SGI_ELSC_RX_QUEUE_SIZE),
        VMSTATE_UINT32(rx_queue_len, SGIElscState),
        VMSTATE_BOOL(in_escape, SGIElscState),
        VMSTATE_UINT8(dip_switches, SGIElscState),
        VMSTATE_UINT8(arb_signal, SGIElscState),
        VMSTATE_INT32(resp_owner, SGIElscState),
        VMSTATE_UINT8_ARRAY(display, SGIElscState, SGI_ELSC_DISPLAY_CHARS),
        VMSTATE_BOOL(cold_power_on, SGIElscState),
        /*
         * A deferred destructive action, whether its ack has been read, and
         * whether its bottom half has been scheduled. Migrate these so an
         * action accepted before a migration behaves correctly on the
         * destination:
         * ARMED waits for the ack read, COMMITTED waits for its boundary, and
         * a SCHEDULED-but-unrun bottom half is re-armed on load
         * (elsc_post_load).
         */
        VMSTATE_UINT8(deferred_action, SGIElscState),
        VMSTATE_BOOL(deferred_committed, SGIElscState),
        VMSTATE_BOOL(deferred_scheduled, SGIElscState),
        /* The lock cell, '0' when free. */
        VMSTATE_UINT8(tas_lock, SGIElscState),
        VMSTATE_TIMER_PTR(pwr_down_timer, SGIElscState),
        VMSTATE_END_OF_LIST()
    },
    .post_load = elsc_post_load,
};

static const Property sgi_elsc_properties[] = {
    DEFINE_PROP_UINT32("num-cpus", SGIElscState, num_cpus, 1),
    DEFINE_PROP_UINT32("cpu-base", SGIElscState, cpu_base, 0),
    DEFINE_PROP_UINT8("dip-switches", SGIElscState, dip_switches, 0),
    /* Only the master module controller publishes arbitration grants. */
    DEFINE_PROP_BOOL("arbitration", SGIElscState, arbitration, true),
    DEFINE_PROP_STRING("build", SGIElscState, build),
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
    rc->phases.exit = elsc_reset_exit;

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
        .interfaces    = (const InterfaceInfo[]) {
            { TYPE_I2C_WIRE_ENDPOINT },
            { }
        },
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
    if (s->origin2000) {
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
    }
    if (resp_pending) {
        g_string_append_printf(buf,
                               "  pending response: %u byte(s) for "
                               "processor code %d\n",
                               s->txlen - s->txpos, s->resp_owner);
    } else {
        g_string_append(buf, "  pending response: none\n");
    }
    g_string_append_printf(buf,
                           "  arbitration rotation: next position %u of %u\n",
                           s->arb_code, s->arb_positions);
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
