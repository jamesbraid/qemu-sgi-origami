/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 300 L1 system controller: the controller itself, and the node-side
 * UART window through which the Bedrock speaks its packet protocol.
 */
#include "qemu/osdep.h"
#include "hw/misc/sgi-l1.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "system/cpus.h"
#include "system/reset.h"
#include "system/rtc.h"
#include "system/runstate.h"

/* The UART window: registers, interrupt and the byte queue toward the node. */

static uint8_t l1_uart_irq_cause(SGIL1UARTState *u)
{
    if ((u->ier & 1) && u->queue_len) {
        return 4;
    }
    if ((u->ier & 2) && u->thre_pending) {
        return 2;
    }
    return 1;
}

static void l1_uart_update_irq(SGIL1UARTState *u)
{
    qemu_set_irq(u->irq, l1_uart_irq_cause(u) != 1);
}

static unsigned l1_uart_room(SGIL1UARTState *u)
{
    return SGI_L1_QUEUE_SIZE - u->queue_len;
}

static bool l1_uart_queue(SGIL1UARTState *u, const uint8_t *bytes, unsigned n)
{
    if (n > l1_uart_room(u)) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-l1: receive queue full\n");
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        u->queue[(u->queue_head + u->queue_len++) % SGI_L1_QUEUE_SIZE] =
            bytes[i];
    }
    l1_uart_update_irq(u);
    return true;
}

/* The controller side: operator console text, display and log. */

static void G_GNUC_PRINTF(2, 3) l1_printf(SGIL1State *s, const char *fmt, ...)
{
    g_autofree char *text = NULL;
    va_list ap;

    va_start(ap, fmt);
    text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (s->capture) {
        g_string_append(s->capture, text);
        return;
    }
    qemu_chr_fe_write_all(&s->chr, (const uint8_t *)text, strlen(text));
}

static void l1_prompt(SGIL1State *s)
{
    l1_printf(s, "%03uc%02u-L1> ", s->rack, s->bay);
}

static const char *l1_designation(SGIL1State *s)
{
    static const char *const names[] = {
        "CPU0", "CPU1", "CPU2", "CPU3", "console"
    };
    unsigned subchannel = s->port_field & 0x1f;

    return names[subchannel <= 4 ? subchannel : 4];
}

static void l1_mode_message(SGIL1State *s, bool returning)
{
    s->console_mode = true;
    s->escaped = false;
    s->warned_off = false;
    l1_printf(s, "%s console mode  %03uc%02u %s, <CTRL_T> to escape to L1"
              "\r\n", returning ? "returning to" : "entering", s->rack,
              s->bay, l1_designation(s));
}

static void G_GNUC_PRINTF(2, 3) l1_log(SGIL1State *s, const char *fmt, ...)
{
    unsigned slot = (s->log_head + s->log_len) % SGI_L1_LOG_ENTRIES;
    struct tm tm;
    va_list ap;

    if (s->log_len == SGI_L1_LOG_ENTRIES) {
        s->log_head = (s->log_head + 1) % SGI_L1_LOG_ENTRIES;
    } else {
        s->log_len++;
    }
    qemu_get_timedate(&tm, s->clock_offset);
    s->log_time[slot] = mktimegm(&tm);
    va_start(ap, fmt);
    vsnprintf((char *)s->log_text[slot], SGI_L1_LOG_TEXT, fmt, ap);
    va_end(ap);
}

static void l1_log_print(SGIL1State *s)
{
    if (!s->log_len) {
        l1_printf(s, "log is empty.\r\n");
        return;
    }
    for (unsigned i = 0; i < s->log_len; i++) {
        unsigned slot = (s->log_head + i) % SGI_L1_LOG_ENTRIES;
        time_t when = s->log_time[slot];
        struct tm tm;

        gmtime_r(&when, &tm);
        l1_printf(s, "%02d/%02d/%02d %02d:%02d:%02d %s\r\n", tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_year % 100, tm.tm_hour, tm.tm_min,
                  tm.tm_sec, s->log_text[slot]);
    }
}

static void l1_display_set(SGIL1State *s, unsigned line, const char *text)
{
    pstrcpy((char *)s->display[line], sizeof(s->display[line]), text);
}

static void l1_display_id(SGIL1State *s)
{
    g_autofree char *id = g_strdup_printf("%03uc%02u     /", s->rack, s->bay);

    l1_display_set(s, 0, id);
}

static void l1_display_print(SGIL1State *s)
{
    l1_printf(s, "revision: 2.0\r\nline 1: \"%-12.12s\"\r\n"
              "line 2: \"%-12.12s\"\r\n"
              "leds:  PWR=%-6sSERVICE=off     FAULT=off\r\n",
              (char *)s->display[0], (char *)s->display[1],
              s->node_power ? "on" : "off");
}

/* Node power and reset as the L1 drives them. */

static const char *const l1_reasons[] = { "", "COMMAND", "PANEL", "OS" };

/* The stored brick-type character the identity request returns. */
static uint32_t l1_brick_type_character(SGIL1State *s)
{
    if (!strcmp(s->brick_type, "ip35")) {
        return 0x43;
    }
    if (!strcmp(s->brick_type, "ip37")) {
        return 0x33;
    }
    return 0x34;
}

static void l1_emit(SGIL1State *s, const uint8_t *payload, unsigned length);

static void l1_reason(SGIL1State *s, uint8_t reason)
{
    s->last_reason[1] = s->last_reason[0];
    s->last_reason[0] = reason;
}

/* The 0x8000 event: the first power-off request while up is deferred. */
static void l1_send_event(SGIL1State *s, uint32_t code, uint32_t number,
                          const char *text)
{
    uint8_t payload[127];
    g_autofree char *line = g_strdup_printf("\r\n%03uc%02u ATTN: %s\r\n",
                                            s->rack, s->bay, text);
    unsigned n = 0, length = strlen(line) + 1;

    if (!s->events_enabled || length > sizeof(payload) - 16) {
        return;
    }
    payload[n++] = 0x40 | (s->event_subchannel & 0x1f);
    stl_be_p(payload + n, (s->rack << 18) | (s->bay << 12) | 4);
    n += 4;
    stl_be_p(payload + n, code);
    n += 4;
    payload[n++] = 2;
    payload[n++] = 0;
    stl_be_p(payload + n, number);
    n += 4;
    payload[n++] = 1;
    memcpy(payload + n, line, length);
    l1_emit(s, payload, n + length);
}

static void l1_request_action(bool shutdown)
{
    if (shutdown) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    } else {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

/*
 * Ask for the action and stop every processor, from queued work on the
 * processor that asked for it -- mirroring sgi_hub_request_machine_reset()
 * in hw/misc/sgi-hub.c and elsc_deferred_action_cpu() in hw/misc/sgi-elsc.c.
 * The work runs at that processor's next translation-block boundary,
 * outside the execution loop, which is where a processor may be stopped.
 */
static void l1_request_action_cpu(CPUState *cpu, run_on_cpu_data data)
{
    CPUState *peer;

    l1_request_action(data.host_int);
    CPU_FOREACH(peer) {
        cpu_pause(peer);
    }
}

static void l1_schedule_action(bool shutdown)
{
    if (current_cpu) {
        async_run_on_cpu(current_cpu, l1_request_action_cpu,
                         RUN_ON_CPU_HOST_INT(shutdown));
    } else {
        /* Chardev input runs on the main thread with the BQL held. */
        pause_all_vcpus();
        l1_request_action(shutdown);
    }
}

static void l1_power_down(SGIL1State *s, uint8_t reason, bool shutdown)
{
    const char *source = l1_reasons[reason];

    if (!s->node_power) {
        return;
    }
    if (reason == 1 && !s->power_down_pending && !s->capture) {
        s->power_down_pending = true;
        l1_send_event(s, 0x8000, 0, "Power Down: issue 'pwr d' again to "
                      "immediately power down.");
        return;
    }
    s->power_down_pending = false;
    l1_reason(s, reason);
    s->node_power = false;
    l1_display_set(s, 1, "Powered Down");
    l1_log(s, "power down (%s)", source);
    qemu_set_irq(s->node_power_out, 0);
    if (shutdown) {
        /*
         * A synchronous shutdown request only wakes the main loop, which
         * carries it out whenever it next runs; nothing stops a peer
         * processor from executing meanwhile. current_cpu names the
         * processor whose own guest write reached this MMIO handler (an
         * L1 protocol frame or an operator's "pwr d" over the L1 console),
         * so queuing the request on that processor's own work stops every
         * processor no later than the writer's own next translation-block
         * boundary, before any peer is scheduled again in round robin.
         * Without a processor to queue it on, the L1 console callback runs
         * the same action directly from the main thread while holding the
         * BQL.
         */
        l1_schedule_action(true);
    }
}

static void l1_power_up(SGIL1State *s, uint8_t reason, bool restart)
{
    const char *source = l1_reasons[reason];
    bool returning = s->escaped;

    l1_reason(s, reason);
    s->node_power = true;
    s->power_down_pending = false;
    /* The selection returns to CPU 0 at every power-up. */
    s->port_field &= 0x60;
    l1_display_set(s, 1, "Powered Up");
    l1_log(s, "power up (%s)", source);
    qemu_set_irq(s->node_power_out, 1);
    if (restart) {
        s->power_up_pending = true;
        l1_schedule_action(false);
    }
    if (!returning) {
        /* From the locked prompt the L1 prints a fresh prompt line first. */
        l1_prompt(s);
        l1_printf(s, "\r\n");
    }
    l1_mode_message(s, returning);
}

static void l1_node_reset(SGIL1State *s, uint8_t reason)
{
    if (!s->node_power) {
        return;
    }
    l1_reason(s, reason);
    l1_display_set(s, 1, "Reset done");
    l1_log(s, "reset (%s)", l1_reasons[reason]);
    /* The hold time is not modeled: the line pulses. */
    qemu_irq_pulse(s->node_reset_out);
    l1_schedule_action(false);
}

/* The machine resumes only once the reset that power-up asked for has run. */
static void l1_after_node_reset(void *opaque)
{
    SGIL1State *s = opaque;

    if (s->power_up_pending) {
        s->power_up_pending = false;
        qemu_bh_schedule(s->power_up_bh);
    }
}

static void l1_power_up_bh(void *opaque)
{
    if (!runstate_is_running()) {
        vm_start();
    }
}

/* The controller's own start: mains applied, or the reboot_l1 command. */
static void l1_boot(SGIL1State *s)
{
    s->rack = s->pending_rack;
    s->bay = s->pending_bay;
    s->console_mode = s->escaped = s->warned_off = false;
    s->command_len = 0;
    s->frame_len = 0;
    s->in_frame = s->escape = s->overflow = false;
    s->port_field = s->node_position << 5;
    s->promreq_debug = false;
    s->power_down_pending = false;
    /* A registered event subchannel of 4 or more is non-volatile. */
    if (s->event_subchannel < 4) {
        s->events_enabled = false;
        s->event_subchannel = 0;
    }
    l1_display_id(s);
    l1_display_set(s, 1, "L1 running");
    if (s->revision_major >= 0) {
        l1_log(s, "L1 booting %d.%d.%d", s->revision_major, s->revision_minor,
               s->revision_patch);
    } else {
        l1_log(s, "L1 booting");
    }
}

/* The packet protocol. */

static uint16_t l1_fcs(uint16_t value, uint8_t byte)
{
    value ^= byte;
    for (unsigned i = 0; i < 8; i++) {
        value = (value >> 1) ^ ((value & 1) ? 0x8408 : 0);
    }
    return value;
}

static void l1_emit(SGIL1State *s, const uint8_t *payload, unsigned length)
{
    uint8_t encoded[2 * SGI_L1_FRAME_SIZE + 2];
    uint16_t crc = 0xffff;
    unsigned n = 0;

    /* The firmware's frame is 128 payload bytes including the header. */
    assert(length <= 128);
    if (!s->uart) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: no UART window attached\n");
        return;
    }
    for (unsigned i = 0; i < length; i++) {
        crc = l1_fcs(crc, payload[i]);
    }
    crc = ~crc;
    encoded[n++] = 0x7e;
    for (unsigned i = 0; i < length + 2; i++) {
        uint8_t byte = i < length ? payload[i] :
                       i == length ? crc : crc >> 8;
        if (byte == 0x7e || byte == 0x7d) {
            encoded[n++] = 0x7d;
            byte ^= 0x20;
        }
        encoded[n++] = byte;
    }
    encoded[n++] = 0x7e;
    l1_uart_queue(s->uart, encoded, n);
}

static void l1_reply(SGIL1State *s, uint8_t channel, int32_t code,
                     unsigned count, const uint8_t *fields, unsigned length)
{
    uint8_t payload[128];

    assert(length <= sizeof(payload) - 6);
    payload[0] = 0x20 | channel;
    stl_be_p(payload + 1, code);
    payload[5] = count;
    if (length) {
        memcpy(payload + 6, fields, length);
    }
    l1_emit(s, payload, length + 6);
}

/* Reject malformed typed arguments before any command changes state. */
static bool l1_arguments(const uint8_t *p, unsigned length, unsigned count)
{
    unsigned offset = 0;

    for (unsigned i = 0; i < count; i++) {
        uint8_t tag;
        if (offset == length) {
            return false;
        }
        tag = p[offset++];
        if (tag == 0) {
            if (length - offset < 4) {
                return false;
            }
            offset += 4;
        } else if (tag == 1) {
            const uint8_t *end = memchr(p + offset, 0, length - offset);
            if (!end) {
                return false;
            }
            offset = end - p + 1;
        } else if (tag & 0x80) {
            if (length - offset < (tag & 0x7f)) {
                return false;
            }
            offset += tag & 0x7f;
        } else {
            return false;
        }
    }
    return offset == length;
}

static void l1_selected_read(SGIL1State *s, uint8_t channel,
                             unsigned component, uint32_t area,
                             uint32_t offset, uint32_t requested)
{
    SGIL1SelectedEEPROM *record = &s->selected[component];
    uint8_t fields[122];
    uint32_t length;

    if (area > 4) {
        l1_reply(s, channel, -105, 0, NULL, 0);
        return;
    }
    if ((record->kind == 1 && area == 4) ||
        (record->kind == 2 && area != 4)) {
        l1_reply(s, channel, -104, 0, NULL, 0);
        return;
    }
    if (record->status[area]) {
        l1_reply(s, channel, -107, 0, NULL, 0);
        return;
    }
    if (offset >= record->length[area]) {
        l1_reply(s, channel, -105, 0, NULL, 0);
        return;
    }
    length = MIN(requested, record->length[area] - offset);
    fields[0] = 0;
    stl_be_p(fields + 1, record->length[area] - offset - length);
    if (!requested) {
        l1_reply(s, channel, 0, 1, fields, 5);
        return;
    }
    if (length > 116) {
        /* The selected serializer counts the field it cannot fit. */
        l1_reply(s, channel, 0, 2, fields, 5);
        return;
    }
    fields[5] = 0x80 | length;
    memcpy(fields + 6, record->image + record->start[area] + offset,
           length);
    l1_reply(s, channel, 0, 2, fields, 6 + length);
}

static void l1_selected_format(SGIL1State *s, uint8_t channel,
                               uint32_t component)
{
    uint8_t fields[45];
    SGIL1SelectedEEPROM *record;
    uint32_t values[9];
    unsigned count;

    if ((component & 0xff) >= 6 ||
        !s->selected[component & 0xff].present) {
        l1_reply(s, channel, -104, 0, NULL, 0);
        return;
    }
    if (component >= 6) {
        l1_reply(s, channel, -105, 0, NULL, 0);
        return;
    }
    record = &s->selected[component];
    values[0] = record->kind;
    if (record->kind == 1) {
        values[1] = record->length[1];
        values[2] = record->length[2];
        values[3] = record->length[3];
        values[4] = record->length[0];
        values[5] = record->status[1];
        values[6] = record->status[2];
        values[7] = record->status[3];
        values[8] = record->status[0];
        count = 9;
    } else {
        values[1] = record->length[4];
        values[2] = record->status[4];
        count = 3;
    }
    for (unsigned i = 0; i < count; i++) {
        fields[i * 5] = 0;
        stl_be_p(fields + i * 5 + 1, values[i]);
    }
    l1_reply(s, channel, 0, count, fields, count * 5);
}

static void l1_eeprom(SGIL1State *s, uint8_t channel, const uint8_t *args)
{
    uint32_t component = ldl_be_p(args + 1);
    uint32_t area = ldl_be_p(args + 6);
    uint32_t offset = ldl_be_p(args + 11);
    uint32_t requested = ldl_be_p(args + 16);
    unsigned index, length;
    SGIL1EEPROM *record;
    uint8_t fields[122];

    if (args[0] || args[5] || args[10] || args[15]) {
        l1_reply(s, channel, -100, 0, NULL, 0);
        return;
    }
    if (component < 6 && s->selected[component].present) {
        l1_selected_read(s, channel, component, area, offset, requested);
        return;
    }
    /* The selected IP45 six-entry bound precedes area and offset checks. */
    if (component >= 6 || (!s->records[component].length &&
                            !s->records[16 + component].length &&
                            !s->records[32 + component].length)) {
        l1_reply(s, channel, -104, 0, NULL, 0);
        return;
    }
    if (area > 4) {
        l1_reply(s, channel, -105, 0, NULL, 0);
        return;
    }
    if (area != 1 && area != 2 && area != 4) {
        /* Areas 0 and 3 carry no file-backed record in this model. */
        l1_reply(s, channel, -104, 0, NULL, 0);
        return;
    }
    index = area == 1 ? 0 : area == 2 ? 1 : 2;
    record = &s->records[index * 16 + component];
    if (!record->length) {
        l1_reply(s, channel, -104, 0, NULL, 0);
        return;
    }
    if (record->invalid) {
        l1_reply(s, channel, -107, 0, NULL, 0);
        return;
    }
    /* An offset at or past the area is out of range. */
    if (offset >= record->length) {
        l1_reply(s, channel, -105, 0, NULL, 0);
        return;
    }
    length = MIN(requested, record->length - offset);
    if (length > 116) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: EEPROM reply exceeds frame capacity\n");
        l1_reply(s, channel, -105, 0, NULL, 0);
        return;
    }
    fields[0] = 0;
    stl_be_p(fields + 1, record->length - offset - length);
    if (requested) {
        fields[5] = 0x80 | length;
        memcpy(fields + 6, record->data + offset, length);
        l1_reply(s, channel, 0, 2, fields, 6 + length);
    } else {
        l1_reply(s, channel, 0, 1, fields, 5);
    }
}

/* The operator's commands. */

static bool l1_word(const char *word, const char *name, const char *alias)
{
    return !strcmp(word, name) || (alias && !strcmp(word, alias));
}

static void l1_invalid(SGIL1State *s, const char *command)
{
    l1_printf(s, "ERROR: invalid arguments for '%s' command, "
              "try \"help %s\"\r\n", command, command);
}

static bool l1_debug_value(const char *text, unsigned int *value)
{
    const char *end;
    int error;

    if (!strcmp(text, "0x")) {
        *value = 0;
        return true;
    }
    error = qemu_strtoui(text, &end, 0, value);
    return end != text && !*end && (error == 0 || error == -ERANGE);
}

static bool l1_select(SGIL1State *s, const char *word)
{
    unsigned subchannel;

    if (!strcmp(word, "console")) {
        subchannel = 4;
    } else if (strlen(word) == 1 && word[0] >= '0' && word[0] <= '4') {
        subchannel = word[0] - '0';
    } else if (strlen(word) == 1 && word[0] >= 'a' && word[0] <= 'd') {
        subchannel = word[0] - 'a';
    } else {
        return false;
    }
    s->port_field = (s->port_field & 0x60) | subchannel;
    return true;
}

static void l1_version(SGIL1State *s)
{
    if (s->revision_major < 0) {
        l1_printf(s, "L1 protocol responder; "
                  "firmware revision unavailable\r\n");
    } else {
        l1_printf(s, "L1 %d.%d.%d (Image A) [Fuel/PE/O300 1MB image]\r\n",
                  s->revision_major, s->revision_minor, s->revision_patch);
    }
}

/*
 * Run one command line. Return the task-3 result code. When @console is set,
 * the prompt or the return to console mode follows; the request interpreter
 * collects the output instead.
 */
static int l1_run_line(SGIL1State *s, const char *text, bool console)
{
    char line[sizeof(s->command)];
    char original[sizeof(s->command)];
    char *argv[8];
    const char *rest = NULL;
    int argc = 0;
    char *p;
    bool own_prompt = false;
    int result = 0;
    unsigned int debug_value;

    pstrcpy(line, sizeof(line), text);
    pstrcpy(original, sizeof(original), text);
    for (p = line; argc < ARRAY_SIZE(argv); ) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        argv[argc++] = p;
        if (argc == 3) {
            /* The third word may be a text argument with spaces in it. */
            rest = original + (p - line);
        }
        while (*p && *p != ' ') {
            p++;
        }
        if (*p) {
            *p++ = 0;
        }
    }
    if (argc && !strcmp(argv[0], "*")) {
        if (argc > 1 && !strcmp(argv[1], "debug")) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: wildcard debug needs connected modules\n");
            l1_printf(s, "INFO: wildcard debug is unsupported without "
                      "connected modules.%s", console ? "\r\n" : "\n");
            result = -1;
            goto complete;
        }
        argc--;
        memmove(argv, argv + 1, argc * sizeof(argv[0]));
        rest = NULL;
    }
    if (!argc) {
        if (console) {
            l1_prompt(s);
        }
        return 0;
    }
    if (!strcmp(argv[0], "ctc") && argc > 1 &&
        !strcmp(argv[1], "debug")) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: peer debug is unsupported\n");
        l1_printf(s, "INFO: peer debug is unsupported.%s",
                  console ? "\r\n" : "\n");
        result = -1;
        goto complete;
    }
    if (l1_word(argv[0], "power", "pwr")) {
        if (argc == 2 && l1_word(argv[1], "up", "u")) {
            if (s->node_power) {
                l1_printf(s, "Power is already on\r\n");
            } else {
                l1_power_up(s, 1, true);
                own_prompt = true;
            }
        } else if (argc == 2 && l1_word(argv[1], "down", "d")) {
            l1_power_down(s, 1, true);
        } else if (argc == 2 && !strcmp(argv[1], "check")) {
            l1_printf(s, "power appears %s\r\n", s->node_power ? "on" : "off");
        } else {
            l1_invalid(s, "power");
        }
    } else if (l1_word(argv[0], "reset", "rst")) {
        l1_node_reset(s, 1);
    } else if (l1_word(argv[0], "softreset", "softrst")) {
        if (s->node_power) {
            l1_display_set(s, 1, "Sft rst done");
            l1_log(s, "soft reset (COMMAND)");
            qemu_irq_pulse(s->soft_reset_out);
        }
    } else if (!strcmp(argv[0], "nmi")) {
        if (s->node_power) {
            l1_display_set(s, 1, "NMI done");
            l1_log(s, "NMI (COMMAND)");
            qemu_irq_pulse(s->nmi_out);
        }
    } else if (!strcmp(argv[0], "reboot_l1")) {
        l1_boot(s);
        return 0;
    } else if (!strcmp(argv[0], "l1dbg")) {
        if (argc == 1) {
            l1_printf(s, "L1 PROM request debugging is %s\r\n",
                      s->promreq_debug ? "on" : "off");
        } else if (argc == 3 && l1_word(argv[1], "promreq", "req") &&
                   (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
            s->promreq_debug = !strcmp(argv[2], "on");
        } else {
            l1_invalid(s, "l1dbg");
        }
    } else if (!strcmp(argv[0], "debug")) {
        if (argc == 1) {
            l1_printf(s, "debug switches set to 0x%04x%s",
                      s->debug_switches, console ? "\r\n" : "\n");
        } else if (argc == 2 && l1_debug_value(argv[1], &debug_value)) {
            s->debug_switches = debug_value & 0xffff;
            l1_printf(s, "debug switches set to 0x%04x%s",
                      s->debug_switches, console ? "\r\n" : "\n");
        } else {
            if (console) {
                l1_invalid(s, "debug");
            } else {
                l1_printf(s, "ERROR: command not found.\n");
            }
            result = -1200;
        }
    } else if (l1_word(argv[0], "version", "ver")) {
        l1_version(s);
    } else if (!strcmp(argv[0], "l1")) {
        s->escaped = false;
        l1_printf(s, "L1 command processor engaged, <CTRL-D> to exit.\r\n");
    } else if (l1_word(argv[0], "select", "sel")) {
        if (argc == 1) {
            l1_printf(s, "console input: %03uc%02u %s\r\n"
                      "console output: not filtered.\r\n", s->rack, s->bay,
                      l1_designation(s));
        } else if (!l1_select(s, argv[argc - 1]) ||
                   (argc == 3 && !l1_word(argv[1], "subchannel", "sub") &&
                    strcmp(argv[1], "s")) || argc > 3) {
            l1_printf(s, "ERROR: bad subchanel selection.\r\n");
        }
    } else if (l1_word(argv[0], "display", "dsp")) {
        if (argc == 1) {
            l1_display_print(s);
        } else if (argc >= 3 && rest && argv[1][1] == 0 &&
                   (argv[1][0] == '1' || argv[1][0] == '2')) {
            l1_display_set(s, argv[1][0] - '1', rest);
        } else {
            l1_invalid(s, "display");
        }
    } else if (!strcmp(argv[0], "log")) {
        if (argc == 1) {
            l1_log_print(s);
        } else if (argc == 2 && (!strcmp(argv[1], "clear") ||
                                 !strcmp(argv[1], "reset"))) {
            s->log_head = s->log_len = 0;
            l1_printf(s, "log reset.\r\n");
        } else if (argc >= 3 && rest && !strcmp(argv[1], "insert")) {
            l1_log(s, "%s", rest);
            l1_printf(s, "log entry made.\r\n");
        } else {
            l1_invalid(s, "log");
        }
    } else if (!strcmp(argv[0], "brick")) {
        unsigned long value = 0;

        if (argc == 1) {
            l1_printf(s, "rack: %03u, slot: %02u, partition: none, "
                      "type: C (IP45) [1MB flash], serial: unavailable, "
                      "source: EEPROM\r\n", s->rack, s->bay);
        } else if (argc == 3 && !strcmp(argv[1], "slot") &&
                   !qemu_strtoul(argv[2], NULL, 10, &value)) {
            if (value < 1 || value > 39) {
                l1_printf(s, "Invalid slot specified.  "
                          "Valid slot numbers are 1-39.\r\n");
            } else {
                s->pending_bay = value;
                l1_printf(s, "brick slot set to %02lu (takes effect on next "
                          "L1 reboot/power cycle)\r\n", value);
            }
        } else if (argc == 3 && !strcmp(argv[1], "rack") &&
                   !qemu_strtoul(argv[2], NULL, 10, &value) && value < 0x3ff) {
            s->pending_rack = value;
            l1_printf(s, "brick rack set to %03lu (takes effect on next "
                      "L1 reboot/power cycle)\r\n", value);
        } else {
            l1_invalid(s, "brick");
        }
    } else if (l1_word(argv[0], "help", "hlp")) {
        l1_printf(s, "Commands are:\r\n"
                  "     brick        display      help|hlp     l1\r\n"
                  "     log          nmi          power|pwr    reboot_l1\r\n"
                  "     reset|rst    select|sel   softreset|softrst\r\n"
                  "     version|ver\r\n"
                  "enter 'hlp <cmd>' for more help on a single command.\r\n");
    } else {
        l1_printf(s, "ERROR: command not found.\r\n");
        qemu_log_mask(LOG_UNIMP, "sgi-l1: unsupported operator command\n");
        result = -1;
    }
complete:
    if (!console || own_prompt) {
        return result;
    }
    if (s->escaped && s->node_power) {
        l1_mode_message(s, true);
    } else {
        s->escaped = false;
        l1_prompt(s);
    }
    return result;
}

static void l1_command(SGIL1State *s)
{
    s->command[s->command_len] = 0;
    s->command_len = 0;
    l1_run_line(s, (const char *)s->command, true);
}

/* Task 3: any code with one text argument runs it and returns its lines. */
static void l1_interpret(SGIL1State *s, uint8_t channel, const uint8_t *args,
                         unsigned count)
{
    g_autofree char *output = NULL;
    uint8_t fields[121];
    unsigned length = 0, fields_count = 0;
    int result;
    char **lines;

    if (count != 1 || args[0] != 1) {
        l1_reply(s, channel, -100, 0, NULL, 0);
        return;
    }
    s->capture = g_string_new(NULL);
    result = l1_run_line(s, (const char *)args + 1, false);
    output = g_string_free(s->capture, FALSE);
    s->capture = NULL;
    lines = g_strsplit(output, "\r\n", -1);
    for (char **line = lines; *line; line++) {
        unsigned n = strlen(*line);

        if (!n) {
            continue;
        }
        if (length + 2 + n > sizeof(fields)) {
            break;
        }
        fields[length++] = 1;
        memcpy(fields + length, *line, n + 1);
        length += n + 1;
        fields_count++;
    }
    g_strfreev(lines);
    /* A status of minus 1 for an unknown command is a model choice. */
    l1_reply(s, channel, result, fields_count, fields, length);
}

static int l1_console_can_receive(void *opaque)
{
    SGIL1State *s = opaque;

    /* One input byte can expand to an eight-byte escaped console frame. */
    return s->uart && l1_uart_room(s->uart) >= 8;
}

static void l1_console_receive(void *opaque, const uint8_t *buf, int size)
{
    SGIL1State *s = opaque;

    for (int i = 0; i < size; i++) {
        uint8_t byte = buf[i];
        if (s->console_mode) {
            if (byte == 0x14) {
                s->console_mode = false;
                s->escaped = true;
                l1_printf(s, "escaping to L1 system controller\r\n");
                l1_prompt(s);
            } else if (!s->node_power) {
                /* Once per entry into console mode: a model choice. */
                if (!s->warned_off) {
                    s->warned_off = true;
                    l1_printf(s, "WARNING: power appears off, "
                              "console unavailable\r\n");
                }
            } else {
                uint8_t payload[] = { s->port_field & 0x1f, byte };
                l1_emit(s, payload, sizeof(payload));
            }
        } else if (byte == 4) {
            s->command_len = 0;
            l1_mode_message(s, false);
        } else if (byte == '\r' || byte == '\n') {
            l1_command(s);
        } else if (byte == 8 || byte == 0x7f) {
            if (s->command_len) {
                s->command_len--;
            }
        } else if (s->command_len < sizeof(s->command) - 1) {
            s->command[s->command_len++] = byte;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-l1: operator command too long\n");
        }
    }
}

static void l1_console_event(void *opaque, QEMUChrEvent event)
{
}

/* The router matches bits 31:12 against the identity or the all-local key. */
static bool l1_local(SGIL1State *s, uint32_t address)
{
    uint32_t key = address >> 12;

    return key == ((s->rack << 6) | s->bay) || key == 0xffff;
}

/* A destination nobody serves gets the router's own error reply. */
static void l1_router_error(SGIL1State *s, uint8_t channel)
{
    uint8_t payload[] = { 0x20 | channel, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 8 };

    l1_emit(s, payload, sizeof(payload));
}

/* Task 18 echoes the request: its code as the reply code, up to three args. */
static void l1_loopback(SGIL1State *s, uint8_t channel, uint16_t command,
                        const uint8_t *args, unsigned args_len, unsigned count)
{
    unsigned offset = 0, echoed = MIN(count, 3);

    for (unsigned i = 0; i < echoed; i++) {
        uint8_t tag = args[offset++];

        if (tag == 0) {
            offset += 4;
        } else if (tag == 1) {
            offset += strlen((const char *)args + offset) + 1;
        } else {
            offset += tag & 0x7f;
        }
    }
    l1_reply(s, channel, command, echoed, args, MIN(offset, 121));
}

static bool l1_task_command(uint16_t command)
{
    switch (command) {
    case 1:
    case 9:
    case 0xa:
    case 2:
    case 3:
    case 5:
    case 6:
    case 4:
    case 7:
    case 8:
    case 0xb:
    case 0xc:
    case 0xd:
    case 0xe:
    case 0xf:
    case 0x10:
    case 0x11:
    case 0x1001:
    case 0x1002:
    case 0x1003:
    case 0x1004:
    case 0x1005:
    case 0x1006:
    case 0x1007:
    case 0x1008:
    case 0x1009:
    case 0x100a:
    case 0x100b:
    case 0x100c:
    case 0x2001:
    case 0x2002:
    case 0x2003:
    case 0x2004:
    case 0x2005:
    case 0x2006:
    case 0x2007:
    case 0x2008:
    case 0x4000:
    case 0x4002:
        return true;
    default:
        return false;
    }
}

static void l1_request(SGIL1State *s)
{
    const uint8_t *p = s->frame;
    unsigned length = s->frame_len - 2;
    uint8_t channel = p[0] & 0x1f;
    uint32_t address;
    uint16_t command;
    unsigned count, args_len;
    const uint8_t *args;
    int32_t error = 0;
    uint8_t fields[48];
    unsigned field_count = 0, field_length = 0;

    if ((p[0] & 0xe0) == 0x40) {
        /* Console output on any subchannel retargets the operator's input. */
        s->port_field = (s->port_field & 0x60) | channel;
        if (s->console_mode && s->node_power) {
            qemu_chr_fe_write_all(&s->chr, p + 1, length - 1);
        } else {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: console output while operator is in L1 mode\n");
        }
        return;
    }
    if ((p[0] & 0xe0) != 0 || length < 7) {
        return;
    }
    address = ldl_be_p(p + 1);
    command = lduw_be_p(p + 5);
    if (!l1_local(s, address)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: no receiver for destination 0x%08x\n", address);
        l1_router_error(s, channel);
        return;
    }
    count = length == 7 ? 0 : p[7];
    args = p + MIN(length, 8);
    args_len = length > 8 ? length - 8 : 0;
    switch (address & 0x1f) {
    case 6:
        break;
    case 3:
        if (l1_arguments(args, args_len, count)) {
            l1_interpret(s, channel, args, count);
        } else {
            l1_reply(s, channel, -100, 0, NULL, 0);
        }
        return;
    case 5:
    case 21: {
        /* The Bedrock console tasks hand the first argument back as input. */
        uint8_t payload[] = { (address >> 5) & 0x1f,
                              args_len >= 5 ? args[4] : 0 };

        if (!s->node_power) {
            uint8_t warning[40] = { 1 };
            g_autofree char *text = g_strdup_printf(
                "WARNING: power on %03uc%02u appears off!", s->rack, s->bay);

            pstrcpy((char *)warning + 1, sizeof(warning) - 1, text);
            l1_reply(s, channel, -1, 1, warning, 2 + strlen(text));
            return;
        }
        l1_emit(s, payload, sizeof(payload));
        return;
    }
    case 18:
        if (l1_arguments(args, args_len, count)) {
            l1_loopback(s, channel, command, args, args_len, count);
        }
        return;
    case 2:
    case 14:
    case 15:
    case 16:
    case 19:
        qemu_log_mask(LOG_UNIMP, "sgi-l1: task %u not modeled\n",
                      address & 0x1f);
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-l1: no receiver for task %u\n",
                      address & 0x1f);
        return;
    }
    if (s->promreq_debug) {
        l1_log(s, "L1REQ: %08x - %04x", (s->rack << 18) | (s->bay << 12) |
               ((((s->port_field >> 5) << 5) | channel) << 5) | 5, command);
    }
    if (!l1_task_command(command)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: request 0x%04x unavailable at 0x%08x\n",
                      command, address);
        l1_reply(s, channel, -101, 0, NULL, 0);
        return;
    }
    if (!l1_arguments(args, args_len, count)) {
        l1_reply(s, channel, -100, 0, NULL, 0);
        return;
    }
    switch (command) {
    case 1:
    case 9:
    case 0xb:
        fields[0] = 0;
        stl_be_p(fields + 1, command == 1 ? s->debug_switches :
                 command == 9 ? s->partition : s->two_bit_setting);
        field_count = 1;
        field_length = 5;
        break;
    case 0xa:
        /* Port speeds: the model's two zero integers. */
        field_count = 2;
        field_length = 10;
        memset(fields, 0, field_length);
        break;
    case 0xc:
    case 0xd:
        /* No attached bricks: rack 0, bay 0, type 0 per port. */
        field_count = command == 0xc ? 8 : 4;
        field_length = 5 * field_count;
        memset(fields, 0, field_length);
        break;
    case 0xf:
        fields[0] = fields[5] = 0;
        stl_be_p(fields + 1, s->last_reason[0]);
        stl_be_p(fields + 6, s->last_reason[1]);
        field_count = 2;
        field_length = 10;
        break;
    case 0x10:
        if (!s->processors) {
            error = -108;
            break;
        }
        /* One mask word; every known processor present and enabled. */
        fields[0] = fields[5] = fields[10] = fields[15] = 0;
        stl_be_p(fields + 1, 1);
        stl_be_p(fields + 6, s->processors);
        stl_be_p(fields + 11, (1u << s->processors) - 1);
        stl_be_p(fields + 16, (1u << s->processors) - 1);
        field_count = 4;
        field_length = 20;
        break;
    case 0x11:
        /* The scan master is not modeled: minus 106 with a status. */
        fields[0] = 0;
        stl_be_p(fields + 1, 1);
        l1_reply(s, channel, -106, 1, fields, 5);
        return;
    case 4:
    case 8: {
        const char *serial = command == 4 ? s->brick_serial : s->system_serial;

        if (!serial || !serial[0]) {
            error = -104;
            break;
        }
        fields[0] = 1;
        pstrcpy((char *)fields + 1, sizeof(fields) - 1, serial);
        field_count = 1;
        field_length = 2 + strlen((char *)fields + 1);
        break;
    }
    case 7:
        if (count < 1) {
            error = -100;
            break;
        }
        l1_selected_format(s, channel, ldl_be_p(args + 1));
        return;
    case 0x1001:
    case 0x1006:
    case 0x1008:
        if (count < 1) {
            error = -100;
            break;
        }
        if (args[0] != 0) {
            error = -105;
            break;
        }
        if (command == 0x1001) {
            s->debug_switches = ldl_be_p(args + 1) & 0xffff;
        } else if (command == 0x1006) {
            s->partition = ldl_be_p(args + 1);
        } else {
            s->two_bit_setting = ldl_be_p(args + 1) & 3;
        }
        break;
    case 0x1009:
        if (count < 2) {
            error = -100;
            break;
        }
        if (args[0] != 0 || args[5] != 0 || (ldl_be_p(args + 6) & 0xf) > 2) {
            error = -105;
            break;
        }
        /* The panel action is not modeled beyond its acceptance. */
        break;
    case 0x100a:
        s->events_enabled = false;
        s->event_subchannel = 0;
        break;
    case 0x100b:
        if (count != 3) {
            error = -100;
            break;
        }
        if (args[0] == 0 && args[5] == 0 && args[10] == 0) {
            l1_log(s, "N0: PROM %x.%x (PR %u)", ldl_be_p(args + 1),
                   ldl_be_p(args + 6), ldl_be_p(args + 11));
        }
        s->events_enabled = false;
        s->event_subchannel = 0;
        break;
    case 0x100c:
        if (count != 2) {
            error = -100;
        }
        /* The per-processor entry is stored by the firmware; not read back. */
        break;
    case 0x2001:
        l1_reply(s, channel, 0, 0, NULL, 0);
        l1_node_reset(s, 3);
        return;
    case 0x2002:
    case 0x2003:
    case 0x2004:
        /* This board installs no PCI slot handler: minus 100. */
        error = -100;
        break;
    case 0x2005:
        l1_reply(s, channel, 0, 0, NULL, 0);
        l1_power_down(s, 3, true);
        return;
    case 0x2006:
        l1_reply(s, channel, 0, 0, NULL, 0);
        if (s->node_power) {
            s->power_down_pending = true;
            l1_power_down(s, 3, false);
        }
        l1_power_up(s, 3, true);
        return;
    case 0x2007:
    case 0x2008:
        error = -108;
        break;
    case 0x4000:
        /* The inventory is empty in this model. */
        error = -104;
        break;
    case 0x4002:
        l1_reply(s, channel, 0, 0, NULL, 0);
        l1_log(s, "L1REQ_REBOOT requested...");
        l1_boot(s);
        return;
    case 0xe:
        /* The firmware reads no argument here, so extra ones are answered. */
        fields[0] = 0;
        stl_be_p(fields + 1, (s->port_field >> 5) & 3);
        field_count = 1;
        field_length = 5;
        break;
    case 0x1002:
    case 0x1007:
        if (count < 1) {
            error = -100;
            break;
        }
        if (args[0] != 0) {
            error = -105;
            break;
        }
        if (command == 0x1002) {
            s->port_field = ((s->port_field & 0x60) | ldl_be_p(args + 1)) &
                            0x7f;
        } else {
            s->event_subchannel = ldl_be_p(args + 1);
            s->events_enabled = true;
        }
        break;
    case 0x1003:
        /* Volunteering the only node changes nothing visible. */
        break;
    case 0x1004:
    case 0x1005: {
        char *line = (char *)s->display[1];

        if (count < 1) {
            error = -100;
            break;
        }
        if (args[0] != 1) {
            error = -102;
            break;
        }
        /* Both codes write the software line; unprintable bytes become ?. */
        pstrcpy(line, sizeof(s->display[1]), (const char *)args + 1);
        for (char *c = line; *c; c++) {
            if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7e) {
                *c = '?';
            }
        }
        break;
    }
    case 2:
    case 3:
        fields[0] = 0;
        stl_be_p(fields + 1, (s->rack << 6) | s->bay);
        field_count = 1;
        field_length = 5;
        if (command == 3) {
            fields[5] = 0;
            stl_be_p(fields + 6, l1_brick_type_character(s));
            field_count = 2;
            field_length = 10;
        }
        break;
    case 5:
        if (s->revision_major < 0) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: firmware revision metadata not configured\n");
            error = -104;
            break;
        }
        fields[0] = fields[5] = fields[10] = 0;
        stl_be_p(fields + 1, s->revision_major);
        stl_be_p(fields + 6, s->revision_minor);
        stl_be_p(fields + 11, s->revision_patch);
        field_count = 3;
        field_length = 15;
        break;
    case 6:
        if (count < 4 || args_len < 20) {
            error = -100;
            break;
        }
        l1_eeprom(s, channel, args);
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-l1: request 0x%04x unavailable\n",
                      command);
        error = -101;
        break;
    }
    l1_reply(s, channel, error, field_count, fields, field_length);
}

static void l1_parse(SGIL1State *s, uint8_t byte)
{
    if (byte == 0x7e) {
        if (s->in_frame && !s->overflow && !s->escape && s->frame_len >= 5) {
            uint16_t crc = 0xffff;
            for (unsigned i = 0; i < s->frame_len; i++) {
                crc = l1_fcs(crc, s->frame[i]);
            }
            if (crc == 0xf0b8) {
                l1_request(s);
            }
        }
        s->in_frame = true;
        s->frame_len = 0;
        s->escape = false;
        s->overflow = false;
        return;
    }
    if (!s->in_frame || s->overflow) {
        return;
    }
    if (byte == 0x7d) {
        /* A second escape only re-arms the first. */
        s->escape = true;
        return;
    }
    if (s->escape) {
        byte ^= 0x20;
        s->escape = false;
    }
    if (s->frame_len == SGI_L1_FRAME_SIZE) {
        s->overflow = true;
        return;
    }
    s->frame[s->frame_len++] = byte;
}

/* The UART window's register file. */

static uint64_t l1_uart_read(void *opaque, hwaddr offset, unsigned size)
{
    SGIL1UARTState *u = opaque;
    uint8_t value;

    switch (offset) {
    case 0:
        if (u->lcr & 0x80) {
            return u->divisor[0];
        }
        if (!u->queue_len) {
            return 0;
        }
        value = u->queue[u->queue_head];
        u->queue_head = (u->queue_head + 1) % SGI_L1_QUEUE_SIZE;
        u->queue_len--;
        l1_uart_update_irq(u);
        qemu_chr_fe_accept_input(&u->controller->chr);
        return value;
    case 8:
        return u->lcr & 0x80 ? u->divisor[1] : u->ier;
    case 0x10:
        value = l1_uart_irq_cause(u);
        if (value == 2) {
            u->thre_pending = false;
            l1_uart_update_irq(u);
        }
        return value;
    case 0x18:
        return u->lcr;
    case 0x20:
        return u->mcr;
    case 0x28:
        return 0x60 | (u->queue_len != 0);
    case 0x30:
        return 0x10;
    case 0x38:
        return u->scratch;
    default:
        g_assert_not_reached();
    }
}

static void l1_uart_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    SGIL1UARTState *u = opaque;

    switch (offset) {
    case 0:
        if (u->lcr & 0x80) {
            u->divisor[0] = value;
        } else {
            u->thre_pending = false;
            l1_uart_update_irq(u);
            l1_parse(u->controller, value);
            u->thre_pending = (u->ier & 2) != 0;
        }
        break;
    case 8:
        if (u->lcr & 0x80) {
            u->divisor[1] = value;
        } else {
            if ((value & 2) && !(u->ier & 2)) {
                u->thre_pending = true;
            }
            u->ier = value;
            if (value & 0xfc) {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-l1: unsupported UART interrupt enables\n");
            }
        }
        break;
    case 0x10:
        if (value & 2) {
            u->queue_head = u->queue_len = 0;
        }
        break;
    case 0x18:
        u->lcr = value;
        break;
    case 0x20:
        u->mcr = value;
        break;
    case 0x38:
        u->scratch = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-l1: write to read-only UART register\n");
        break;
    }
    l1_uart_update_irq(u);
}

static const MemoryRegionOps l1_uart_ops = {
    .read = l1_uart_read,
    .write = l1_uart_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 8, .max_access_size = 8 },
    .impl = { .min_access_size = 8, .max_access_size = 8 },
};

/* Controller reset, migration and construction. */

static void l1_reset_enter(Object *obj, ResetType type)
{
    l1_boot(SGI_L1(obj));
}

static bool l1_record_invalid(const SGIL1EEPROM *record, unsigned area)
{
    uint8_t sum = 0;
    size_t checked = area == 2 ? 63 : record->length;

    for (size_t i = 0; i < checked; i++) {
        sum += record->data[i];
    }
    if (area == 2) {
        return record->data[63] != sum;
    }
    /* Raw area files have no common header or encoded EEPROM capacity. */
    return sum != 0;
}

static bool l1_checksum_ok(const uint8_t *data, unsigned length)
{
    uint8_t sum = 0;

    for (unsigned i = 0; i < length; i++) {
        sum += data[i];
    }
    return sum == 0;
}

static void l1_selected_board_area(SGIL1SelectedEEPROM *record,
                                   unsigned area, unsigned header_byte)
{
    uint32_t start = record->image[header_byte] * 8;
    uint32_t length;
    const uint8_t *prefix;

    if (!start || start + 8 > record->capacity) {
        return;
    }
    prefix = record->image + start;
    if (area == 3) {
        length = prefix[0] ? prefix[1] * 8 :
                 ROUND_UP(prefix[2] + 5, 8);
    } else {
        length = prefix[1] * 8;
    }
    if (!length || start + length >= record->capacity) {
        return;
    }
    if (!l1_checksum_ok(prefix, length)) {
        record->status[area] = 3;
        return;
    }
    record->start[area] = start;
    record->length[area] = length;
    record->status[area] = 0;
}

static void l1_selected_board_scan(SGIL1SelectedEEPROM *record)
{
    memset(record->status, 1, 4);
    record->length[0] = 8;
    if (!l1_checksum_ok(record->image, 8)) {
        record->status[0] = 3;
        return;
    }
    record->status[0] = 0;
    l1_selected_board_area(record, 1, 2);
    l1_selected_board_area(record, 2, 3);
    l1_selected_board_area(record, 3, 1);
}

static void l1_selected_spd_scan(SGIL1SelectedEEPROM *record)
{
    uint8_t sum = 0;

    for (unsigned i = 0; i < 63; i++) {
        sum += record->image[i];
    }
    if (record->image[63] != sum) {
        record->status[4] = 3;
        g_clear_pointer(&record->image, g_free);
        record->capacity = 0;
        return;
    }
    record->length[4] = 160;
}

static bool l1_selected_valid(const SGIL1SelectedEEPROM *record,
                              unsigned component)
{
    if (!record->present) {
        return !record->kind && !record->capacity && !record->image;
    }
    if (record->kind != (component < 2 ? 1 : 2) ||
        record->capacity > 65535 ||
        !!record->capacity != !!record->image) {
        return false;
    }
    if (record->kind == 1 && record->capacity < 8) {
        return false;
    }
    if (record->kind == 2 && record->capacity &&
        record->capacity != 160) {
        return false;
    }
    for (unsigned area = 0; area < 5; area++) {
        if (record->status[area] > 4 ||
            record->start[area] > record->capacity ||
            record->length[area] > record->capacity - record->start[area]) {
            return false;
        }
        if (record->status[area] && record->length[area] &&
            !(record->kind == 1 && area == 0 &&
              record->status[0] == 3 && record->length[0] == 8)) {
            return false;
        }
    }
    if (record->kind == 2 &&
        ((record->status[4] == 0) != (record->length[4] == 160))) {
        return false;
    }
    return true;
}

static int l1_post_load(void *opaque, int version_id)
{
    SGIL1State *s = opaque;

    if (s->command_len >= sizeof(s->command) ||
        s->frame_len > SGI_L1_FRAME_SIZE ||
        s->port_field > 0x7f ||
        s->two_bit_setting > 3 || s->debug_switches > 0xffff ||
        s->log_head >= SGI_L1_LOG_ENTRIES ||
        s->log_len > SGI_L1_LOG_ENTRIES ||
        s->rack >= 0x3ff || s->bay >= 0x3f ||
        s->pending_rack >= 0x3ff || s->pending_bay >= 0x3f ||
        s->display[0][127] || s->display[1][127]) {
        return -EINVAL;
    }
    for (unsigned i = 32; i < 48; i++) {
        if (s->records[i].length && s->records[i].length < 64) {
            return -EINVAL;
        }
    }
    for (unsigned i = 0; i < 48; i++) {
        SGIL1EEPROM *record = &s->records[i];

        record->invalid = record->length &&
                          l1_record_invalid(record, i / 16);
    }
    for (unsigned component = 0; component < 6; component++) {
        if (!l1_selected_valid(&s->selected[component], component)) {
            return -EINVAL;
        }
        if (s->selected[component].present &&
            (s->records[component].length ||
             s->records[16 + component].length ||
             s->records[32 + component].length)) {
            return -EINVAL;
        }
    }
    for (unsigned i = 0; i < SGI_L1_LOG_ENTRIES; i++) {
        s->log_text[i][SGI_L1_LOG_TEXT - 1] = 0;
    }
    qemu_set_irq(s->node_power_out, s->node_power);
    return 0;
}

static int l1_record_pre_load(void *opaque)
{
    SGIL1EEPROM *record = opaque;
    g_clear_pointer(&record->data, g_free);
    return 0;
}

static bool l1_record_size_valid(void *opaque, int version_id)
{
    return ((SGIL1EEPROM *)opaque)->length <= 65535;
}

static const VMStateDescription vmstate_l1_record = {
    .name = "sgi-l1/eeprom",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = l1_record_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(length, SGIL1EEPROM),
        VMSTATE_VALIDATE("record-size", l1_record_size_valid),
        VMSTATE_VBUFFER_ALLOC_UINT32(data, SGIL1EEPROM, 0, NULL, length),
        VMSTATE_END_OF_LIST()
    },
};

static int l1_selected_pre_load(void *opaque)
{
    SGIL1SelectedEEPROM *record = opaque;

    g_clear_pointer(&record->image, g_free);
    return 0;
}

static bool l1_selected_size_valid(void *opaque, int version_id)
{
    return ((SGIL1SelectedEEPROM *)opaque)->capacity <= 65535;
}

static const VMStateDescription vmstate_l1_selected = {
    .name = "sgi-l1/selected-eeprom",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = l1_selected_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(present, SGIL1SelectedEEPROM),
        VMSTATE_UINT8(kind, SGIL1SelectedEEPROM),
        VMSTATE_UINT32(capacity, SGIL1SelectedEEPROM),
        VMSTATE_VALIDATE("selected-record-size", l1_selected_size_valid),
        VMSTATE_VBUFFER_ALLOC_UINT32(image, SGIL1SelectedEEPROM, 0,
                                     NULL, capacity),
        VMSTATE_UINT32_ARRAY(start, SGIL1SelectedEEPROM, 5),
        VMSTATE_UINT32_ARRAY(length, SGIL1SelectedEEPROM, 5),
        VMSTATE_UINT8_ARRAY(status, SGIL1SelectedEEPROM, 5),
        VMSTATE_END_OF_LIST()
    },
};

/* Version 8 also carries the selected record cache. */
static const VMStateDescription vmstate_l1 = {
    .name = TYPE_SGI_L1,
    .version_id = 8,
    .minimum_version_id = 8,
    .post_load = l1_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(records, SGIL1State, 48, 0,
                             vmstate_l1_record, SGIL1EEPROM),
        VMSTATE_STRUCT_ARRAY(selected, SGIL1State, 6, 0,
                             vmstate_l1_selected, SGIL1SelectedEEPROM),
        VMSTATE_BOOL(node_power, SGIL1State),
        VMSTATE_BOOL(power_up_pending, SGIL1State),
        VMSTATE_BOOL(console_mode, SGIL1State),
        VMSTATE_BOOL(escaped, SGIL1State),
        VMSTATE_BOOL(warned_off, SGIL1State),
        VMSTATE_BOOL(promreq_debug, SGIL1State),
        VMSTATE_BOOL(power_down_pending, SGIL1State),
        VMSTATE_UINT32(debug_switches, SGIL1State),
        VMSTATE_UINT32(partition, SGIL1State),
        VMSTATE_UINT8(two_bit_setting, SGIL1State),
        VMSTATE_UINT8_ARRAY(last_reason, SGIL1State, 2),
        VMSTATE_UINT8_ARRAY(command, SGIL1State, 128),
        VMSTATE_UINT32(command_len, SGIL1State),
        VMSTATE_UINT32(rack, SGIL1State),
        VMSTATE_UINT32(bay, SGIL1State),
        VMSTATE_UINT32(pending_rack, SGIL1State),
        VMSTATE_UINT32(pending_bay, SGIL1State),
        VMSTATE_UINT8(port_field, SGIL1State),
        VMSTATE_UINT8(event_subchannel, SGIL1State),
        VMSTATE_BOOL(events_enabled, SGIL1State),
        VMSTATE_INT64(clock_offset, SGIL1State),
        VMSTATE_INT64_ARRAY(log_time, SGIL1State, SGI_L1_LOG_ENTRIES),
        VMSTATE_UINT8_2DARRAY(log_text, SGIL1State, SGI_L1_LOG_ENTRIES,
                              SGI_L1_LOG_TEXT),
        VMSTATE_UINT32(log_head, SGIL1State),
        VMSTATE_UINT32(log_len, SGIL1State),
        VMSTATE_UINT8_ARRAY(frame, SGIL1State, SGI_L1_FRAME_SIZE),
        VMSTATE_UINT32(frame_len, SGIL1State),
        VMSTATE_BOOL(in_frame, SGIL1State),
        VMSTATE_BOOL(escape, SGIL1State),
        VMSTATE_BOOL(overflow, SGIL1State),
        VMSTATE_UINT8_2DARRAY(display, SGIL1State, 2, 128),
        VMSTATE_END_OF_LIST()
    },
};

static void l1_realize(DeviceState *dev, Error **errp)
{
    SGIL1State *s = SGI_L1(dev);

    char **files[] = { s->chassis_files, s->board_files, s->spd_files };
    uint32_t counts[] = { s->chassis_count, s->board_count, s->spd_count };

    if (s->revision_major < -1 || s->revision_minor < -1 ||
        s->revision_patch < -1 ||
        ((s->revision_major < 0 || s->revision_minor < 0 ||
          s->revision_patch < 0) &&
         (s->revision_major >= 0 || s->revision_minor >= 0 ||
          s->revision_patch >= 0))) {
        error_setg(errp, "L1 revision metadata needs all three components");
        return;
    }
    if (s->rack >= 0x3ff || s->bay >= 0x3f) {
        error_setg(errp, "L1 rack or bay must be a concrete destination");
        return;
    }
    if (s->node_position > 3) {
        error_setg(errp, "L1 node-position must be 0 to 3");
        return;
    }
    if (!s->brick_type) {
        s->brick_type = g_strdup("ip45");
    }
    if (strcmp(s->brick_type, "ip45") && strcmp(s->brick_type, "ip35") &&
        strcmp(s->brick_type, "ip37")) {
        error_setg(errp, "L1 brick-type must be ip45, ip35 or ip37");
        return;
    }
    if (s->debug_switches > 0xffff || s->processors > 32 ||
        (s->system_serial && strlen(s->system_serial) > 32) ||
        (s->brick_serial && strlen(s->brick_serial) > 32)) {
        error_setg(errp, "L1 setting out of range");
        return;
    }
    for (unsigned area = 0; area < 3; area++) {
        if (counts[area] > 16) {
            error_setg(errp, "L1 EEPROM area has more than 16 components");
            return;
        }
        for (unsigned component = 0; component < counts[area]; component++) {
            SGIL1EEPROM *record = &s->records[area * 16 + component];
            gsize length;
            GError *local_error = NULL;
            char *data;

            if (!files[area][component][0]) {
                continue;
            }
            if (!g_file_get_contents(files[area][component], &data,
                                     &length, &local_error)) {
                error_setg(errp, "L1 EEPROM: %s", local_error->message);
                g_error_free(local_error);
                return;
            }
            if (!length || length > 65535 || (area == 2 && length < 64)) {
                error_setg(errp, "L1 EEPROM record length is invalid");
                g_free(data);
                return;
            }
            record->length = length;
            record->data = (uint8_t *)data;
            record->invalid = l1_record_invalid(record, area);
        }
    }
    if (s->board_record_count > 6 || s->spd_record_count > 6) {
        error_setg(errp, "L1 selected EEPROM has more than six components");
        return;
    }
    for (unsigned component = 0; component < 6; component++) {
        char *board_path = component < s->board_record_count ?
                           s->board_record_files[component] : NULL;
        char *spd_path = component < s->spd_record_count ?
                         s->spd_record_files[component] : NULL;
        bool board = board_path && board_path[0];
        bool spd = spd_path && spd_path[0];
        SGIL1SelectedEEPROM *record = &s->selected[component];
        gsize length;
        GError *local_error = NULL;
        char *data;

        if (!board && !spd) {
            continue;
        }
        if (board == spd || (board && component >= 2) ||
            (spd && component < 2)) {
            error_setg(errp, "L1 selected EEPROM component kind is invalid");
            return;
        }
        if (s->records[component].length ||
            s->records[16 + component].length ||
            s->records[32 + component].length) {
            error_setg(errp, "L1 selected EEPROM conflicts with raw area file");
            return;
        }
        if (!g_file_get_contents(board ? board_path : spd_path, &data,
                                 &length, &local_error)) {
            error_setg(errp, "L1 selected EEPROM: %s", local_error->message);
            g_error_free(local_error);
            return;
        }
        if (length > 65535 || (board && length < 8) ||
            (spd && length != 160)) {
            error_setg(errp, "L1 selected EEPROM record length is invalid");
            g_free(data);
            return;
        }
        record->present = true;
        record->kind = board ? 1 : 2;
        record->capacity = length;
        record->image = (uint8_t *)data;
        if (board) {
            l1_selected_board_scan(record);
        } else {
            l1_selected_spd_scan(record);
        }
    }
    s->pending_rack = s->rack;
    s->pending_bay = s->bay;
    s->power_up_bh = qemu_bh_new(l1_power_up_bh, s);
    qemu_register_reset(l1_after_node_reset, s);
    qemu_chr_fe_set_handlers(&s->chr, l1_console_can_receive,
                             l1_console_receive, l1_console_event, NULL,
                             s, NULL, true);
    l1_boot(s);
    if (s->auto_power) {
        /* The real countdown is skipped: a model choice. */
        l1_log(s, "auto power up initiated");
        l1_printf(s, "INFO: %03uc%02u powering up the system.\r\n", s->rack,
                  s->bay);
        l1_power_up(s, 1, false);
    }
}

static char *l1_display_get(Object *obj, Error **errp)
{
    return g_strdup((char *)SGI_L1(obj)->display[1]);
}

static char *l1_display_first_get(Object *obj, Error **errp)
{
    return g_strdup((char *)SGI_L1(obj)->display[0]);
}

static const Property l1_properties[] = {
    DEFINE_PROP_CHR("chardev", SGIL1State, chr),
    DEFINE_PROP_INT32("revision-major", SGIL1State, revision_major, -1),
    DEFINE_PROP_INT32("revision-minor", SGIL1State, revision_minor, -1),
    DEFINE_PROP_INT32("revision-patch", SGIL1State, revision_patch, -1),
    DEFINE_PROP_ARRAY("chassis-eeprom", SGIL1State, chassis_count,
                      chassis_files, qdev_prop_string, char *),
    DEFINE_PROP_ARRAY("board-eeprom", SGIL1State, board_count,
                      board_files, qdev_prop_string, char *),
    DEFINE_PROP_ARRAY("spd-eeprom", SGIL1State, spd_count,
                      spd_files, qdev_prop_string, char *),
    DEFINE_PROP_ARRAY("board-record-eeprom", SGIL1State, board_record_count,
                      board_record_files, qdev_prop_string, char *),
    DEFINE_PROP_ARRAY("spd-record-eeprom", SGIL1State, spd_record_count,
                      spd_record_files, qdev_prop_string, char *),
    DEFINE_PROP_UINT32("rack", SGIL1State, rack, 1),
    DEFINE_PROP_UINT32("bay", SGIL1State, bay, 1),
    DEFINE_PROP_UINT32("node-position", SGIL1State, node_position, 0),
    DEFINE_PROP_BOOL("auto-power", SGIL1State, auto_power, true),
    DEFINE_PROP_UINT32("debug-switches", SGIL1State, debug_switches, 0),
    DEFINE_PROP_UINT32("partition", SGIL1State, partition, 0),
    DEFINE_PROP_STRING("system-serial", SGIL1State, system_serial),
    DEFINE_PROP_STRING("brick-serial", SGIL1State, brick_serial),
    DEFINE_PROP_UINT32("processors", SGIL1State, processors, 2),
    DEFINE_PROP_STRING("brick-type", SGIL1State, brick_type),
};

static void l1_init(Object *obj)
{
    SGIL1State *s = SGI_L1(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->node_power_out, "node-power", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->node_reset_out, "node-reset", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->soft_reset_out, "soft-reset", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->nmi_out, "nmi", 1);
    object_property_add_uint8_ptr(obj, "event-subchannel", &s->event_subchannel,
                                  OBJ_PROP_FLAG_READ);
}

static void l1_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI L1 system controller";
    dc->realize = l1_realize;
    dc->vmsd = &vmstate_l1;
    dc->user_creatable = true;
    dc->hotpluggable = false;
    device_class_set_props(dc, l1_properties);
    rc->phases.enter = l1_reset_enter;
    object_class_property_add_str(klass, "display-line-1", l1_display_first_get,
                                  NULL);
    object_class_property_add_str(klass, "display-line-2",
                                  l1_display_get, NULL);
}

static void l1_finalize(Object *obj)
{
    SGIL1State *s = SGI_L1(obj);
    for (unsigned i = 0; i < 48; i++) {
        g_free(s->records[i].data);
    }
    for (unsigned i = 0; i < 6; i++) {
        g_free(s->selected[i].image);
    }
    g_free(s->system_serial);
    g_free(s->brick_serial);
    g_free(s->brick_type);
}

/* UART window reset, migration and construction. */

static void l1_uart_reset_enter(Object *obj, ResetType type)
{
    SGIL1UARTState *u = SGI_L1_UART(obj);

    u->lcr = u->ier = u->mcr = u->scratch = 0;
    u->thre_pending = false;
    memset(u->divisor, 0, sizeof(u->divisor));
    u->queue_head = u->queue_len = 0;
}

static void l1_uart_reset_hold(Object *obj, ResetType type)
{
    l1_uart_update_irq(SGI_L1_UART(obj));
}

static int l1_uart_post_load(void *opaque, int version_id)
{
    SGIL1UARTState *u = opaque;

    if (u->queue_len > SGI_L1_QUEUE_SIZE ||
        u->queue_head >= SGI_L1_QUEUE_SIZE) {
        return -EINVAL;
    }
    l1_uart_update_irq(u);
    return 0;
}

static const VMStateDescription vmstate_l1_uart = {
    .name = TYPE_SGI_L1_UART,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = l1_uart_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(lcr, SGIL1UARTState),
        VMSTATE_UINT8(ier, SGIL1UARTState),
        VMSTATE_BOOL(thre_pending, SGIL1UARTState),
        VMSTATE_UINT8(mcr, SGIL1UARTState),
        VMSTATE_UINT8(scratch, SGIL1UARTState),
        VMSTATE_UINT8_ARRAY(divisor, SGIL1UARTState, 2),
        VMSTATE_UINT8_ARRAY(queue, SGIL1UARTState, SGI_L1_QUEUE_SIZE),
        VMSTATE_UINT32(queue_head, SGIL1UARTState),
        VMSTATE_UINT32(queue_len, SGIL1UARTState),
        VMSTATE_END_OF_LIST()
    },
};

static void l1_uart_controller_check(const Object *obj, const char *name,
                                     Object *value, Error **errp)
{
    SGIL1State *controller = SGI_L1(value);

    if (!controller) {
        error_setg(errp, "%s cannot be set to NULL", name);
        return;
    }
    if (controller->uart) {
        error_setg(errp, "L1 controller already has a UART window");
    }
}

static void l1_uart_realize(DeviceState *dev, Error **errp)
{
    SGIL1UARTState *u = SGI_L1_UART(dev);

    if (!u->controller) {
        error_setg(errp, "L1 UART window needs a controller link");
        return;
    }
    u->controller->uart = u;
}

static void l1_uart_init(Object *obj)
{
    SGIL1UARTState *u = SGI_L1_UART(obj);

    memory_region_init_io(&u->mmio, obj, &l1_uart_ops, u, TYPE_SGI_L1_UART,
                          SGI_L1_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &u->mmio);
    qdev_init_gpio_out_named(DEVICE(obj), &u->irq, "uart", 1);
    object_property_add_link(obj, "controller", TYPE_SGI_L1,
                             (Object **)&u->controller,
                             l1_uart_controller_check, OBJ_PROP_LINK_STRONG);
}

static void l1_uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI L1 processor-side UART window";
    dc->realize = l1_uart_realize;
    dc->vmsd = &vmstate_l1_uart;
    dc->user_creatable = true;
    rc->phases.enter = l1_uart_reset_enter;
    rc->phases.hold = l1_uart_reset_hold;
}

static const TypeInfo l1_types[] = {
    {
        .name = TYPE_SGI_L1,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(SGIL1State),
        .instance_init = l1_init,
        .instance_finalize = l1_finalize,
        .class_init = l1_class_init,
    },
    {
        .name = TYPE_SGI_L1_UART,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIL1UARTState),
        .instance_init = l1_uart_init,
        .class_init = l1_uart_class_init,
    },
};

DEFINE_TYPES(l1_types)
