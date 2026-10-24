/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI L1 system controller: the controller itself, and the node-side
 * UART window through which the Bedrock speaks its packet protocol.
 */
#include "qemu/osdep.h"
#include "hw/misc/sgi-l1.h"
#include "hw/mips/sgi/sgi-synthetic-serial.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "net/util.h"
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
#include "system/qtest.h"
#include "trace.h"

/* Product metadata does not describe physical UART wiring or power control. */
struct SGIL1ProductProfile {
    const char *name;
    uint32_t identity;
    unsigned board_count;
    uint32_t board_capacity;
    bool compute_services;
};

/*
 * No Fuel L1 identity reply is captured, so ip34 has no default type; the
 * machine passes the catalogue's code through brick-type-code.
 */
static const SGIL1ProductProfile l1_profiles[] = {
    { "ip34", 0, 2, 0, true },
    { "unknown", 0x34, 2, 0, true },
    { "ip45", 0x34, 2, 0, true },
    { "ip35", 0x43, 2, 0, true },
    { "ip37", 0x33, 2, 0, true },
    { "pci-expansion", 0x25, 4, 0x0200, false },
    { "router", 0x52, 0, 0, false },
    { "vbrick-discovery-reference", 0, 0, 0, false },
};

/*
 * The UART window: registers, interrupt and the byte queue toward the node.
 */

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

/* The RS-422 pair is usable: a peer is cabled and both run network 422. */
static SGIL1State *l1_linked_peer(SGIL1State *s)
{
    return s->profile->compute_services && s->peer &&
           s->peer->profile->compute_services &&
           s->network_422 && s->peer->network_422 ? s->peer : NULL;
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

static int l1_compute_unsupported(SGIL1State *s)
{
    const char *profile = !strcmp(s->profile->name, "pci-expansion") ?
                          "expansion" : s->profile->name;

    qemu_log_mask(LOG_UNIMP,
                  "sgi-l1: compute service unsupported on %s profile\n",
                  profile);
    l1_printf(s, "INFO: compute service is unsupported on %s profile.\r\n",
              profile);
    return -1;
}

static void l1_mode_message(SGIL1State *s, bool returning)
{
    if (!s->profile->compute_services) {
        l1_compute_unsupported(s);
        l1_prompt(s);
        return;
    }
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
    if (s->brick_type_code) {
        return s->brick_type_code;
    }
    if (!strcmp(s->brick_type, "ip35")) {
        return 0x43;
    }
    if (!strcmp(s->brick_type, "ip37")) {
        return 0x33;
    }
    return s->profile->identity;
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
 * processor that asked for it, mirroring sgi_hub_request_machine_reset() in
 * hw/misc/sgi-hub.c and elsc_deferred_action_cpu() in hw/misc/sgi-elsc.c. The
 * work runs at that processor's next translation-block boundary, outside the
 * execution loop, which is where a processor may be stopped.
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

    if (!s->profile->compute_services) {
        return;
    }

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
         * processor from executing meanwhile. current_cpu names the processor
         * whose own guest write reached this MMIO handler (an L1 protocol
         * frame or an operator's "pwr d" over the L1 console), so queuing the
         * request on that processor's own work stops every processor no later
         * than the writer's own next translation-block boundary, before any
         * peer is scheduled again in round robin. Without a processor to
         * queue it on, the L1 console callback runs the same action directly
         * from the main thread while holding the BQL.
        */
        l1_schedule_action(true);
    }
}

static void l1_power_up(SGIL1State *s, uint8_t reason, bool restart)
{
    const char *source = l1_reasons[reason];
    bool returning = s->escaped;

    if (!s->profile->compute_services) {
        return;
    }

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
    if (!s->profile->compute_services || !s->node_power) {
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

    s->usb_test_state.set = false;
    g_strlcpy(s->usb_test_result, "none", sizeof(s->usb_test_result));
    g_strlcpy(s->usb_test_descriptor_result, "none",
              sizeof(s->usb_test_descriptor_result));

    if (s->profile->compute_services && s->power_up_pending) {
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

bool sgi_l1_bind_bic_startup(SGIL1State *s, SGIL1BICStartup selection,
                           const SGIL1BICEndpoint endpoints[2],
                           SGIL1BICOperation operation, Error **errp)
{
    if (DEVICE(s)->realized || s->bic_startup ||
        selection != SGI_L1_BIC_STARTUP_EXPERIMENT || !operation ||
        !s->brick_type || strcmp(s->brick_type, "pci-expansion") ||
        !endpoints[0].receiver || !endpoints[0].identity ||
        (!!endpoints[1].receiver != !!endpoints[1].identity) ||
        (endpoints[1].receiver &&
         (endpoints[1].receiver == endpoints[0].receiver ||
          endpoints[1].identity == endpoints[0].identity))) {
        error_setg(errp, "L1 BIC startup needs an unbound expansion controller "
                   "and distinct immutable endpoints before realization");
        return false;
    }
    s->bic_startup = selection;
    s->bic_operation = operation;
    for (unsigned i = 0; i < 2; i++) {
        s->bic_endpoints[i] = endpoints[i];
        if (endpoints[i].receiver) {
            object_ref(endpoints[i].receiver);
        }
    }
    return true;
}

static void l1_bic_startup(SGIL1State *s)
{
    uint16_t coordinates = (s->rack << 6) | s->bay;

    if (!s->bic_startup_needed) {
        return;
    }
    s->bic_startup_needed = false;
    s->bic_operation_active = true;
    for (unsigned i = 0; i < 2; i++) {
        if (!s->bic_endpoints[i].receiver) {
            continue;
        }
        /* Independent operations: a failed high byte does not skip low. */
        for (unsigned byte = 0; byte < 2; byte++) {
            uint8_t command = byte ? 0x12 : 0x13;
            uint8_t value = byte ? coordinates : coordinates >> 8;
            uint8_t address = 0x8c + 2 * i;

            bool completed = s->bic_operation(s->bic_endpoints[i].receiver,
                                              i, 0, address, command, value);

            trace_sgi_l1_bic_startup_operation(i, 0, address, command, value,
                                               completed);
            if (!completed) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "sgi-l1: BIC startup operation failed: "
                              "position=%u bus=0 address=0x%02x "
                              "command=0x%02x value=0x%02x\n",
                              i, address, command, value);
            }
        }
    }
    s->bic_operation_active = false;
}

/* The controller's own start: mains applied, or the reboot_l1 command. */
static void l1_boot_local(SGIL1State *s)
{
    s->bic_startup_needed = s->bic_startup != SGI_L1_BIC_STARTUP_NONE;
    s->rack = s->pending_rack;
    s->bay = s->pending_bay;
    s->console_mode = s->escaped = s->warned_off = false;
    s->command_len = 0;
    memset(s->frame, 0, sizeof(s->frame));
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

static void l1_boot(SGIL1State *s)
{
    l1_boot_local(s);
    l1_bic_startup(s);
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

static void l1_emit_transport(SGIL1State *s, unsigned transport,
                              const uint8_t *payload, unsigned length)
{
    uint8_t encoded[2 * SGI_L1_FRAME_SIZE + 2];
    uint16_t crc = 0xffff;
    unsigned n = 0;

    /* The firmware's frame is 128 payload bytes including the header. */
    assert(length <= 128);
    if (transport >= SGI_L1_IROUTER_TRANSPORTS || !s->transport[transport]) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: no UART window attached for transport %u\n",
                      transport);
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
        /* Selected serial-multiplex startup escapes CR, but not XON/XOFF. */
        if (byte == 0x7e || byte == 0x7d || byte == 0x0d) {
            encoded[n++] = 0x7d;
            byte ^= 0x20;
        }
        encoded[n++] = byte;
    }
    encoded[n++] = 0x7e;
    l1_uart_queue(s->transport[transport], encoded, n);
}

static void l1_emit_direct(SGIL1State *s, const uint8_t *payload,
                           unsigned length)
{
    l1_emit_transport(s, 0, payload, length);
}

static void l1_emit(SGIL1State *s, const uint8_t *payload, unsigned length)
{
    l1_emit_direct(s->reply_to ? s->reply_to : s, payload, length);
}

static void l1_reply_direct(SGIL1State *s, uint8_t channel, int32_t code,
                            unsigned count, const uint8_t *fields,
                            unsigned length)
{
    uint8_t payload[128];

    assert(length <= sizeof(payload) - 6);
    payload[0] = 0x20 | channel;
    stl_be_p(payload + 1, code);
    payload[5] = count;
    if (length) {
        memcpy(payload + 6, fields, length);
    }
    l1_emit_direct(s, payload, length + 6);
}

static void l1_reply(SGIL1State *s, uint8_t channel, int32_t code,
                     unsigned count, const uint8_t *fields, unsigned length)
{
    l1_reply_direct(s->reply_to ? s->reply_to : s, channel, code, count,
                    fields, length);
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

static void l1_selected_read(SGIL1State *s, SGIL1State *recipient,
                             uint8_t channel,
                             unsigned component, uint32_t area,
                             uint32_t offset, uint32_t requested)
{
    SGIL1SelectedEEPROM *record = &s->selected[component];
    uint8_t fields[122];
    uint32_t length;

    if (area > 4) {
        l1_reply_direct(recipient, channel, -105, 0, NULL, 0);
        return;
    }
    if ((record->kind == 1 && area == 4) ||
        (record->kind == 2 && area != 4)) {
        l1_reply_direct(recipient, channel, -104, 0, NULL, 0);
        return;
    }
    if (record->status[area]) {
        l1_reply_direct(recipient, channel, -107, 0, NULL, 0);
        return;
    }
    if (offset >= record->length[area]) {
        l1_reply_direct(recipient, channel, -105, 0, NULL, 0);
        return;
    }
    length = MIN(requested, record->length[area] - offset);
    fields[0] = 0;
    stl_be_p(fields + 1, record->length[area] - offset - length);
    if (!requested) {
        l1_reply_direct(recipient, channel, 0, 1, fields, 5);
        return;
    }
    if (length > 116) {
        /* The selected serializer counts the field it cannot fit. */
        l1_reply_direct(recipient, channel, 0, 2, fields, 5);
        return;
    }
    fields[5] = 0x80 | length;
    memcpy(fields + 6, record->image + record->start[area] + offset,
           length);
    l1_reply_direct(recipient, channel, 0, 2, fields, 6 + length);
}

static void l1_selected_format(SGIL1State *s, SGIL1State *recipient,
                               uint8_t channel,
                               uint32_t component)
{
    uint8_t fields[45];
    SGIL1SelectedEEPROM *record;
    uint32_t values[9];
    unsigned count;

    if ((component & 0xff) >= 6 ||
        !s->selected[component & 0xff].present) {
        l1_reply_direct(recipient, channel, -104, 0, NULL, 0);
        return;
    }
    if (component >= 6) {
        l1_reply_direct(recipient, channel, -105, 0, NULL, 0);
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
    l1_reply_direct(recipient, channel, 0, count, fields, count * 5);
}

static void l1_eeprom(SGIL1State *s, SGIL1State *recipient, uint8_t channel,
                      const uint8_t *args)
{
    uint32_t component = ldl_be_p(args + 1);
    uint32_t area = ldl_be_p(args + 6);
    uint32_t offset = ldl_be_p(args + 11);
    uint32_t requested = ldl_be_p(args + 16);
    unsigned index, length;
    SGIL1EEPROM *record;
    uint8_t fields[122];

    if (args[0] || args[5] || args[10] || args[15]) {
        l1_reply_direct(recipient, channel, -100, 0, NULL, 0);
        return;
    }
    if (component < 6 && s->selected[component].present) {
        l1_selected_read(s, recipient, channel, component, area, offset,
                          requested);
        return;
    }
    if (component >= 16 || (!s->records[component].length &&
                            !s->records[16 + component].length &&
                            !s->records[32 + component].length)) {
        l1_reply_direct(recipient, channel, -104, 0, NULL, 0);
        return;
    }
    if (area > 4) {
        l1_reply_direct(recipient, channel, -105, 0, NULL, 0);
        return;
    }
    if (area != 1 && area != 2 && area != 4) {
        /* Areas 0 and 3 carry no file-backed record in this model. */
        l1_reply_direct(recipient, channel, -104, 0, NULL, 0);
        return;
    }
    index = area == 1 ? 0 : area == 2 ? 1 : 2;
    record = &s->records[index * 16 + component];
    if (!record->length) {
        l1_reply_direct(recipient, channel, -104, 0, NULL, 0);
        return;
    }
    if (record->invalid) {
        l1_reply_direct(recipient, channel, -107, 0, NULL, 0);
        return;
    }
    /* An offset at or past the area is out of range. */
    if (offset >= record->length) {
        l1_reply_direct(recipient, channel, -105, 0, NULL, 0);
        return;
    }
    length = MIN(requested, record->length - offset);
    if (length > 116) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: EEPROM reply exceeds frame capacity\n");
        l1_reply_direct(recipient, channel, -105, 0, NULL, 0);
        return;
    }
    fields[0] = 0;
    stl_be_p(fields + 1, record->length - offset - length);
    if (requested) {
        fields[5] = 0x80 | length;
        memcpy(fields + 6, record->data + offset, length);
        l1_reply_direct(recipient, channel, 0, 2, fields, 6 + length);
    } else {
        l1_reply_direct(recipient, channel, 0, 1, fields, 5);
    }
}

static bool l1_discovery_reference(const SGIL1State *s)
{
    return !g_strcmp0(s->brick_type, "vbrick-discovery-reference");
}

/* This experimental owner supplies no physical identity or product data. */
static void l1_reference_refuse(SGIL1State *s, SGIL1State *recipient,
                                uint8_t channel, unsigned task)
{
    qemu_log_mask(LOG_UNIMP, "sgi-l1: task %u unavailable on %s\n",
                  task, s->brick_type);
    l1_reply_direct(recipient, channel, task == 6 ? -104 : -101, 0, NULL, 0);
}

static bool l1_read_query(SGIL1State *s, SGIL1State *recipient,
                          uint8_t channel, uint16_t command,
                          const uint8_t *args, unsigned args_len,
                          unsigned count)
{
    uint8_t fields[40] = { 0 };

    if (l1_discovery_reference(s)) {
        l1_reference_refuse(s, recipient, channel, 6);
        return true;
    }
    switch (command) {
    case 3:
        if (!l1_brick_type_character(s) ||
            (!s->brick_type_code && !strcmp(s->brick_type, "unknown"))) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: brick identity not configured\n");
            l1_reply_direct(recipient, channel, -104, 0, NULL, 0);
            return true;
        }
        stl_be_p(fields + 1, (s->rack << 6) | s->bay);
        stl_be_p(fields + 6, l1_brick_type_character(s));
        l1_reply_direct(recipient, channel, 0, 2, fields, 10);
        return true;
    case 4:
        if (!s->brick_serial || !s->brick_serial[0]) {
            l1_reply_direct(recipient, channel, -104, 0, NULL, 0);
        } else {
            fields[0] = 1;
            pstrcpy((char *)fields + 1, sizeof(fields) - 1, s->brick_serial);
            l1_reply_direct(recipient, channel, 0, 1, fields,
                             2 + strlen((char *)fields + 1));
        }
        return true;
    case 6:
        if (count < 4 || args_len < 20) {
            l1_reply_direct(recipient, channel, -100, 0, NULL, 0);
        } else {
            l1_eeprom(s, recipient, channel, args);
        }
        return true;
    case 7:
        if (count < 1 || args_len < 5) {
            l1_reply_direct(recipient, channel, -100, 0, NULL, 0);
        } else {
            l1_selected_format(s, recipient, channel, ldl_be_p(args + 1));
        }
        return true;
    case 0xc:
        l1_reply_direct(recipient, channel, 0, 8, fields, sizeof(fields));
        return true;
    default:
        return false;
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
 * The peer's side of a broadcast power or reset command. Both chassis are one
 * QEMU machine here, so the requesting L1's own action already powers or
 * resets every node; the peer only records the state and log entry a real
 * peer would, and does not request a second machine action. What a real pair
 * exchanges to propagate it is unknown.
*/
static void l1_peer_power(SGIL1State *peer, bool up)
{
    if (up == peer->node_power) {
        return;
    }
    peer->node_power = up;
    peer->power_down_pending = false;
    l1_display_set(peer, 1, up ? "Powered Up" : "Powered Down");
    l1_log(peer, "power %s (COMMAND)", up ? "up" : "down");
    qemu_set_irq(peer->node_power_out, up);
}

static void l1_peer_reset(SGIL1State *peer)
{
    if (!peer->node_power) {
        return;
    }
    l1_display_set(peer, 1, "Reset done");
    l1_log(peer, "reset (COMMAND)");
}

/*
 * Run one command line. Return the task-3 result code. When @console is set,
 * the prompt or the return to console mode follows; the request interpreter
 * collects the output instead.
*/
bool sgi_l1_bind_expansion_power(SGIL1State *s, Object *receiver,
    uint64_t identity, SGIL1PowerRequest request, Error **errp)
{
    if (DEVICE(s)->realized || s->power_receiver || !receiver || !identity ||
        !request || !s->brick_type || strcmp(s->brick_type, "pci-expansion")) {
        error_setg(errp, "L1 power needs one pre-realization "
                   "expansion binding");
        return false;
    }
    s->power_receiver = receiver;
    s->power_identity = identity;
    s->power_request = request;
    return true;
}

bool sgi_l1_bind_power_command(SGIL1State *issuer, SGIL1State *owner,
                               uint64_t identity, Error **errp)
{
    if (!owner || issuer == owner || issuer->power_command_owner ||
        !owner->power_receiver || owner->power_identity != identity ||
        !issuer->profile || !issuer->profile->compute_services) {
        error_setg(errp, "L1 power command needs a distinct bound owner");
        return false;
    }
    issuer->power_command_owner = owner;
    issuer->power_command_identity = identity;
    issuer->power_command_reachable = true;
    return true;
}

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
    bool broadcast = false;
    SGIL1State *peer = l1_linked_peer(s);

    if (l1_discovery_reference(s)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: operator commands unavailable on %s\n",
                      s->brick_type);
        l1_printf(s, "ERROR: commands unsupported "
                  "on this reference owner.\r\n");
        return -1;
    }
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
        broadcast = true;
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
    if (s->power_request && argc == 2 && !broadcast &&
        !strcmp(argv[0], "pwr") &&
        (!strcmp(argv[1], "u") || !strcmp(argv[1], "d"))) {
        Error *error = NULL;
        bool on = !strcmp(argv[1], "u");

        result = s->power_request(s->power_receiver, s, s->power_identity,
                                  on, &error) ? 0 : -1;
        if (result) {
            l1_printf(s, "ERROR: expansion main-power operation failed.%s",
                      console ? "\r\n" : "\n");
        } else {
            l1_printf(s, "Expansion main power %s.%s", on ? "on" : "off",
                      console ? "\r\n" : "\n");
        }
        if (error) {
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-l1: %s\n",
                          error_get_pretty(error));
            error_free(error);
        }
        goto complete;
    }
    if (!strcmp(argv[0], "ctc") && argc > 1 &&
        !strcmp(argv[1], "pwr") && s->power_command_owner) {
        SGIL1State *owner = s->power_command_owner;
        GString *output;

        if (argc != 3 || strcmp(argv[1], "pwr") ||
            (strcmp(argv[2], "u") && strcmp(argv[2], "d")) ||
            !s->power_command_reachable || s->power_command_active ||
            owner->power_command_active || owner->capture) {
            l1_printf(s, "ERROR: expansion power command route refused.%s",
                      console ? "\r\n" : "\n");
            result = -1;
            goto complete;
        }
        /* This synchronous request owns its output until the issuer replies. */
        output = g_string_new(NULL);
        s->power_command_active = true;
        owner->power_command_active = true;
        owner->capture = output;
        result = l1_run_line(owner, original + (argv[1] - line), false);
        owner->capture = NULL;
        owner->power_command_active = false;
        s->power_command_active = false;
        l1_printf(s, "%s", output->str);
        g_string_free(output, TRUE);
        goto complete;
    }
    if (!s->profile->compute_services &&
        (l1_word(argv[0], "power", "pwr") ||
         l1_word(argv[0], "reset", "rst") ||
         l1_word(argv[0], "softreset", "softrst") ||
         !strcmp(argv[0], "nmi") || l1_word(argv[0], "select", "sel"))) {
        result = l1_compute_unsupported(s);
        goto complete;
    }
    if (!strcmp(argv[0], "ctc") && argc > 1 && s->serving_peer) {
        /*
         * A command the peer relayed here names its own chassis as the peer
         * again. Whether a real L1 relays it back is unknown; the model
         * refuses it rather than capture into the relaying L1's collection
         * while that L1 is still collecting (a model choice).
        */
        qemu_log_mask(LOG_UNIMP, "sgi-l1: nested ctc is unsupported\n");
        l1_printf(s, "INFO: nested ctc is unsupported.%s",
                  console ? "\r\n" : "\n");
        result = -1;
        goto complete;
    }
    if (!strcmp(argv[0], "ctc") && argc > 1 && peer) {
        g_autofree char *output = NULL;

        /* The peer runs it; its brick id line comes first. */
        peer->capture = g_string_new(NULL);
        peer->serving_peer = true;
        result = l1_run_line(peer, original + (argv[1] - line), false);
        peer->serving_peer = false;
        output = g_string_free(peer->capture, FALSE);
        peer->capture = NULL;
        /* One line of its own, so a task-3 reply carries it as a field. */
        l1_printf(s, "%03uc%02u:\r\n%s", peer->rack, peer->bay, output);
        goto complete;
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
            if (broadcast && peer) {
                l1_peer_power(peer, true);
            }
            if (s->node_power) {
                l1_printf(s, "Power is already on\r\n");
            } else {
                l1_power_up(s, 1, true);
                own_prompt = true;
            }
        } else if (argc == 2 && l1_word(argv[1], "down", "d")) {
            l1_power_down(s, 1, true);
            /*
             * The peer follows only once this chassis has gone down: a first
             * request that is waiting for its confirmation leaves both
             * chassis up.
            */
            if (broadcast && peer && !s->node_power) {
                l1_peer_power(peer, false);
            }
        } else if (argc == 2 && !strcmp(argv[1], "check")) {
            l1_printf(s, "power appears %s\r\n", s->node_power ? "on" : "off");
        } else {
            l1_invalid(s, "power");
        }
    } else if (l1_word(argv[0], "reset", "rst")) {
        if (broadcast && peer) {
            l1_peer_reset(peer);
        }
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

void sgi_l1_query_router_init(SGIL1QueryRouter *router,
                             SGIL1QueryOwner *owners, unsigned owner_count,
                             SGIL1QueryAssociation *associations,
                             unsigned association_count, Error **errp)
{
    if (!owners || !owner_count || (association_count && !associations)) {
        error_setg(errp, "L1 query registry needs explicit owners "
                   "and associations");
        return;
    }
    for (unsigned i = 0; i < owner_count; i++) {
        if (!owners[i].controller || !owners[i].identity ||
            owners[i].controller->query_router) {
            error_setg(errp, "Invalid or already registered L1 query owner %u",
                       i);
            return;
        }
        for (unsigned j = 0; j < i; j++) {
            if (owners[i].controller == owners[j].controller ||
                owners[i].identity == owners[j].identity) {
                error_setg(errp, "Duplicate L1 query owner %u", i);
                return;
            }
        }
    }
    for (unsigned i = 0; i < association_count; i++) {
        SGIL1QueryAssociation *a = &associations[i];
        if (a->from >= owner_count || a->to >= owner_count ||
            a->from == a->to) {
            error_setg(errp, "Invalid L1 query association %u", i);
            return;
        }
        for (unsigned j = 0; j < i; j++) {
            if (a->from == associations[j].from &&
                a->to == associations[j].to) {
                error_setg(errp, "Duplicate L1 query association %u", i);
                return;
            }
        }
    }
    router->owners = owners;
    router->owner_count = owner_count;
    router->associations = associations;
    router->association_count = association_count;
    router->active = false;
    for (unsigned i = 0; i < owner_count; i++) {
        owners[i].controller->query_router = router;
        owners[i].controller->query_owner_identity = owners[i].identity;
    }
}

bool sgi_l1_query_router_set_attached(SGIL1QueryRouter *router,
                                     SGIL1QueryAttachment *attachments,
                                     unsigned count, Error **errp)
{
    if (router->active || (count && !attachments)) {
        error_setg(errp, "Invalid L1 attached-controller configuration");
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        SGIL1QueryAttachment *a = &attachments[i];
        bool associated = false;

        if (a->issuer >= router->owner_count ||
            a->owner >= router->owner_count || a->issuer == a->owner) {
            error_setg(errp, "Invalid L1 attached owner %u", i);
            return false;
        }
        for (unsigned j = 0; j < i; j++) {
            if (a->issuer == attachments[j].issuer) {
                error_setg(errp, "Duplicate L1 attached issuer %u", a->issuer);
                return false;
            }
        }
        for (unsigned j = 0; j < router->association_count; j++) {
            SGIL1QueryAssociation *edge = &router->associations[j];
            if (edge->from == a->issuer && edge->to == a->owner) {
                associated = true;
                break;
            }
        }
        if (!associated) {
            error_setg(errp, "L1 attached owner has no registry association");
            return false;
        }
    }
    router->attachments = attachments;
    router->attachment_count = count;
    return true;
}

void sgi_l1_query_router_destroy(SGIL1QueryRouter *router)
{
    for (unsigned i = 0; i < router->owner_count; i++) {
        SGIL1State *s = router->owners[i].controller;
        if (s->query_router == router) {
            s->query_router = NULL;
            s->query_owner_identity = 0;
        }
    }
    memset(router, 0, sizeof(*router));
}

static void l1_serve_query(SGIL1State *serving,
                           const SGIL1QueryRequest *request)
{
    const uint8_t *p = request->payload;
    unsigned count = request->length == 7 ? 0 : p[7];
    unsigned args_len = request->length > 8 ? request->length - 8 : 0;
    const uint8_t *args = p + MIN(request->length, 8);

    assert(l1_local(serving, ldl_be_p(p + 1)));
    if (!l1_arguments(args, args_len, count)) {
        l1_reply_direct(request->recipient, request->channel, -100, 0, NULL, 0);
    } else if (l1_discovery_reference(serving)) {
        l1_reference_refuse(serving, request->recipient, request->channel,
                            request->destination & 0x1f);
    } else {
        l1_read_query(serving, request->recipient, request->channel,
                      lduw_be_p(p + 5), args, args_len, count);
    }
}

void sgi_l1_query_route(SGIL1QueryRouter *router,
                       const SGIL1QueryRequest *request)
{
    SGIL1State *serving = NULL;
    unsigned issuer = router->owner_count;
    unsigned matches = 0;
    bool reachable = false;
    bool attached = request->destination == UINT32_C(0x4ffff006);
    uint32_t key = request->destination >> 12;
    const uint8_t *p = request->payload;
    unsigned length = request->length;
    uint16_t command;
    SGIL1QueryRequest forwarded = *request;
    uint8_t copy[SGI_L1_FRAME_SIZE];
    uint8_t failure[] = { 0x20 | request->channel,
                         0xff, 0xff, 0xff, 0xff, 0, 0, 0, 8 };

    if (router->active || length < 7 || length > sizeof(copy) ||
        (p[0] & 0xe0) || ldl_be_p(p + 1) != request->destination ||
        (p[0] & 0x1f) != request->channel) {
        goto refused;
    }
    if (!attached && (request->destination >> 28 ||
                     (key >> 6) == 0x3ff || (key & 0x3f) == 0x3f)) {
        goto refused;
    }
    command = lduw_be_p(p + 5);
    for (unsigned i = 0; i < router->owner_count; i++) {
        if (router->owners[i].controller == request->issuer) {
            issuer = i;
            break;
        }
    }
    if (attached) {
        unsigned owner = router->owner_count;

        for (unsigned i = 0; i < router->attachment_count; i++) {
            if (router->attachments[i].issuer == issuer) {
                owner = router->attachments[i].owner;
                break;
            }
        }
        if (owner == router->owner_count) {
            goto refused;
        }
        serving = router->owners[owner].controller;
        for (unsigned i = 0; i < router->association_count; i++) {
            SGIL1QueryAssociation *a = &router->associations[i];
            if (a->from == issuer && a->to == owner) {
                reachable = a->reachable;
                break;
            }
        }
        memcpy(copy, p, length);
        forwarded.destination = request->destination & UINT32_C(0x0fffffff);
        stl_be_p(copy + 1, forwarded.destination);
        forwarded.payload = copy;
    } else {
        for (unsigned i = 0; i < router->association_count; i++) {
            SGIL1QueryAssociation *a = &router->associations[i];
            SGIL1State *candidate = router->owners[a->to].controller;
            if (a->from == issuer &&
                key == ((candidate->rack << 6) | candidate->bay)) {
                matches++;
                serving = candidate;
                reachable = a->reachable;
            }
        }
        if (matches != 1) {
            goto refused;
        }
    }
    if (!reachable) {
        goto refused;
    }
    if (!l1_discovery_reference(serving) &&
        ((request->destination & 0x1f) != 6 ||
         (command != 3 && command != 6 && command != 7 &&
          !(!attached && command == 4) && !(attached && command == 0xc)))) {
        goto refused;
    }
    router->active = true;
    l1_serve_query(serving, &forwarded);
    router->active = false;
    return;

refused:
    qemu_log_mask(LOG_UNIMP, "sgi-l1: isolated query route refused 0x%08x%s\n",
                  request->destination, router->active ? " (nested)" : "");
    l1_emit_direct(request->recipient, failure, sizeof(failure));
}

/*
 * Task 18 echoes the request: its code as the reply code, up to three args.
 */
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

static void l1_request_frame(SGIL1State *s, const uint8_t *p,
                             unsigned length, unsigned transport);

/*
 * A request the local router cannot deliver: the catch-all tries the cabled
 * peer. A type-0 request for the peer's own rack and bay goes there as it is;
 * a type-3 (attached C-brick) request is rewritten to type 0 and tried on the
 * link (the addressing section's routing table). The peer answers as itself,
 * on the requester's subchannel, through the requester's UART. Returns false
 * when nothing across the link takes it.
*/
static bool l1_forward_to_peer(SGIL1State *s, const uint8_t *p,
                               unsigned length, uint32_t address)
{
    SGIL1State *peer = l1_linked_peer(s);
    unsigned type = address >> 28;
    uint32_t key = (address >> 12) & 0xffff;
    uint8_t copy[SGI_L1_FRAME_SIZE];

    if (!peer || length > sizeof(copy)) {
        return false;
    }
    if (type == 3) {
        address &= ~(UINT32_C(0xf) << 28);
    } else if (type != 0 || key != ((peer->rack << 6) | peer->bay)) {
        return false;
    }
    memcpy(copy, p, length);
    stl_be_p(copy + 1, address);
    peer->reply_to = s;
    l1_request_frame(peer, copy, length, 0);
    peer->reply_to = NULL;
    return true;
}

static void l1_irouter_chunk(SGIL1State *s, const uint8_t *p,
                            unsigned length, unsigned transport);

static bool l1_irouter_enabled(const SGIL1State *s)
{
    return !l1_discovery_reference(s) &&
           (s->irouter_compat || s->o300_l1cmd_compat);
}

static void l1_request(SGIL1State *s, unsigned transport)
{
    SGIL1FrameState *frame = &s->frame[transport];

    l1_request_frame(s, frame->data, frame->length - 2, transport);
}

static void l1_o300_trace_frame(const SGIL1State *s, bool reply,
                                 unsigned transport, const uint8_t *frame,
                                 unsigned length)
{
    static const char digits[] = "0123456789abcdef";
    char hex[SGI_L1_FRAME_SIZE * 2 + 1];

    if (!s->o300_l1cmd_compat || s->brick_type_code != 0x34) {
        return;
    }
    g_assert(length <= SGI_L1_FRAME_SIZE);
    for (unsigned i = 0; i < length; i++) {
        hex[i * 2] = digits[frame[i] >> 4];
        hex[i * 2 + 1] = digits[frame[i] & 0xf];
    }
    hex[length * 2] = 0;
    trace_sgi_l1_o300_frame(reply, transport, length, hex);
}

static bool l1_o300_ascii_field(const uint8_t **cursor, const uint8_t *end,
                                char field[64])
{
    unsigned length;

    if (*cursor >= end || (**cursor >> 6) != 3) {
        return false;
    }
    length = *(*cursor)++ & 0x3f;
    if (!length || length >= 64 || length > end - *cursor) {
        return false;
    }
    for (unsigned i = 0; i < length; i++) {
        if (!g_ascii_isprint((*cursor)[i])) {
            return false;
        }
    }
    memcpy(field, *cursor, length);
    field[length] = 0;
    *cursor += length;
    return true;
}

static void l1_o300_board_fields(SGIL1State *s, unsigned component,
                                 char fields[5][64])
{
    const uint8_t *cursor, *end;
    const SGIL1SelectedEEPROM *selected = &s->selected[component];
    const SGIL1EEPROM *raw = &s->records[16 + component];
    char manufacturer[64];
    uint8_t tag;

    if (selected->present && selected->kind == 1 &&
        selected->status[1] == 0 && selected->length[1]) {
        cursor = selected->image + selected->start[1];
        end = cursor + selected->length[1];
    } else if (!selected->present && raw->length && !raw->invalid) {
        cursor = raw->data;
        end = cursor + raw->length;
    } else {
        return;
    }
    if (end - cursor < 7) {
        return;
    }
    cursor += 6;
    if (!l1_o300_ascii_field(&cursor, end, manufacturer) ||
        !l1_o300_ascii_field(&cursor, end, fields[0]) ||
        !l1_o300_ascii_field(&cursor, end, fields[1]) ||
        !l1_o300_ascii_field(&cursor, end, fields[2]) ||
        cursor >= end) {
        return;
    }
    cursor++; /* Board area's FRU byte separates part from revision. */
    if (!l1_o300_ascii_field(&cursor, end, fields[3]) || cursor >= end) {
        return;
    }
    tag = *cursor++;
    if ((tag >> 6) != 0 || (tag & 0x3f) > end - cursor) {
        return;
    }
    cursor += tag & 0x3f;
    l1_o300_ascii_field(&cursor, end, fields[4]);
}

static char *l1_o300_command_text(SGIL1State *s, const char *command)
{
    const char *brick_serial = s->brick_serial && s->brick_serial[0] ?
                               s->brick_serial : "EMU-UNKNOWN-BSN";
    const char *system_serial = s->system_serial && s->system_serial[0] ?
                                s->system_serial : "EMU-UNKNOWN-SSN";
    const char *brick_location = s->brick_serial && s->brick_serial[0] ?
                                 "CONFIGURED" : "EMULATED";
    const char *system_location = s->system_serial && s->system_serial[0] ?
                                  "CONFIGURED" : "EMULATED";
    g_autofree char *partition = NULL;
    g_autoptr(GString) text = NULL;
    char node[5][64] = {
        "EMU-UNKNOWN-NODE-PRODUCT", "EMU-UNKNOWN-NODE-SERIAL",
        "EMU-UNKNOWN-NODE-PART", "EMU-UNKNOWN-NODE-REV",
        "EMU-UNKNOWN-NODE-TW",
    };
    char io8[5][64] = {
        "EMU-UNKNOWN-IO8-PRODUCT", "EMU-UNKNOWN-IO8-SERIAL",
        "EMU-UNKNOWN-IO8-PART", "EMU-UNKNOWN-IO8-REV",
        "EMU-UNKNOWN-IO8-TW",
    };

    if (!strcmp(command, "brick ")) {
        partition = s->partition ? g_strdup_printf("%u", s->partition) :
                                   g_strdup("none");
        return g_strdup_printf(
            "rack: %03u, slot: %02u, partition: %s, type: C (IP45) "
            "[EMU-UNKNOWN flash], serial:%s, source: %s\n\n",
            s->rack, s->bay, partition, brick_serial, brick_location);
    }
    if (!strcmp(command, "version ")) {
        if (s->revision_major < 0) {
            return g_strdup("L1 EMU-UNKNOWN (Image EMU-UNKNOWN), Built "
                            "EMU-UNKNOWN [EMU-ORIGIN300-L1]\n\n");
        }
        return g_strdup_printf(
            "L1 %d.%d.%d (Image EMU-UNKNOWN), Built EMU-UNKNOWN "
            "[EMU-ORIGIN300-L1]\n\n",
            s->revision_major, s->revision_minor, s->revision_patch);
    }
    if (strcmp(command, "serial all ")) {
        return NULL;
    }
    l1_o300_board_fields(s, 0, node);
    l1_o300_board_fields(s, 1, io8);
    text = g_string_new(
        "Data                            Location      Value\n"
        "------------------------------  ------------  --------\n");
    g_string_append_printf(text,
        "Local System Serial Number      %-12s  %s\n"
        "Reference System Serial Number  EMULATED      EMU-UNKNOWN-REF-SSN\n"
        "Local Brick Serial Number       %-12s  %s\n"
        "Reference Brick Serial Number   EMULATED      EMU-UNKNOWN-REF-BSN\n"
        "\n"
        "EEPROM      Product Name    Serial        Part Number           Rev  T/W\n"
        "----------  --------------  -------------  --------------------  ---  ------\n",
        system_location, system_serial, brick_location, brick_serial);
    g_string_append_printf(text, "%-10s  %-14s  %-12s  %-20s  %-3s  %s\n",
                           "NODE", node[0], node[1], node[2], node[3], node[4]);
    g_string_append_printf(text, "%-10s  %-14s  %-12s  %-20s  %-3s  %s\n",
                           "IO8", io8[0], io8[1], io8[2], io8[3], io8[4]);
    g_string_append(text,
        "\nAdditional EEPROM and DIMM inventory: EMU-UNKNOWN\n\n");
    return g_string_free(g_steal_pointer(&text), false);
}

static void l1_irouter_reply(SGIL1State *s, unsigned transport,
                             uint8_t channel, const uint8_t *request,
                             unsigned request_length,
                             unsigned fragment_count)
{
    static const char *const type_names[256] = {
        [0x37] = "Chimera Server",
        [0x39] = "Chimera WS",
        [0x51] = "Rackmount WS",
    };
    const uint8_t *scalar = request + 20;
    const uint8_t *string = scalar + 8;
    uint32_t string_length = lduw_be_p(string + 2);
    uint32_t encoded_offset = ldl_be_p(string + 4);
    bool o300 = s->o300_l1cmd_compat && s->brick_type_code == 0x34;
    bool ip53 = s->irouter_compat && type_names[s->brick_type_code];
    /* Selected IP53 compatibility input, not a physical identity claim. */
    uint32_t node_id = (s->rack << 6) | s->bay;
    uint32_t packed_channel = (transport << 5) | channel;
    uint32_t return_address = (node_id << 12) |
                              (packed_channel << 5) | 0x15;
    uint32_t local_address = (node_id << 12) | 3;
    const char *partition = s->partition == 0 ? "none" :
                            s->partition < 255 ? NULL : "?";
    g_autofree char *partition_text = NULL;
    const char *serial = s->brick_serial ? s->brick_serial : "(null)";
    g_autofree char *text = NULL;
    g_autofree uint8_t *response = NULL;
    unsigned text_length, response_length, offset;

    if ((!o300 && !ip53) ||
        request_length < 36 || lduw_be_p(request) != request_length ||
        request[2] != 2 || request[3] != 0 ||
        request[5] != (0x80 | fragment_count) ||
        request[6] != 0 ||
        request[7] != 0 || ldl_be_p(request + 8) != 0x0ffff003 ||
        request[16] != 2 || memcmp(scalar, "\0\0\0\0\0\0\0\0", 8) ||
        string[0] != 0x0a || string[1] != 1 ||
        encoded_offset < 16 || encoded_offset - 16 != 36 ||
        string_length != request_length - 36 || string_length < 1 ||
        request[request_length - 1] != 0) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: unsupported or malformed IRouter request\n");
        return;
    }
    if (o300 &&
        ((string_length == 7 && !memcmp(request + 36, "brick \0", 7)) ||
         (string_length == 9 && !memcmp(request + 36, "version \0", 9)) ||
         (string_length == 12 &&
          !memcmp(request + 36, "serial all \0", 12)))) {
        text = l1_o300_command_text(s, (const char *)request + 36);
    } else if (!o300 && string_length == 6 &&
               !memcmp(request + 36, "brick\0", 6)) {
        /* The plain command spelling. */
    } else if (!o300 && string_length == 7 &&
               !memcmp(request + 36, "brick \0", 7)) {
        /* The captured interpreter form includes one trailing space. */
    } else {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: IRouter command interpreter operation refused\n");
        return;
    }

    if (!o300) {
        if (!partition) {
            partition_text = g_strdup_printf("%u", s->partition);
            partition = partition_text;
        }
        text = g_strdup_printf(
            "rack: %03u, slot: %02u, partition: %s, type: %s [?MB flash], "
            "serial:%s, source: OTHER\n\n",
            s->rack, s->bay, partition, type_names[s->brick_type_code],
            serial);
    }
    text_length = strlen(text) + 1;
    response_length = 36 + text_length;
    if (response_length > SGI_L1_IROUTER_MAX_FRAGMENT) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-l1: IRouter brick response exceeds fragment limit\n");
        return;
    }
    response = g_malloc0(response_length);
    stw_be_p(response, response_length);
    response[2] = 2;
    response[4] = request[4];
    response[5] = 0x81;
    response[6] = 1;
    stl_be_p(response + 8, return_address);
    stl_be_p(response + 12, local_address);
    response[16] = 2;
    response[17] = request[17];
    memcpy(response + 18, request + 18, 2);
    response[20] = 0;
    stl_be_p(response + 24, 0);
    response[28] = 0x0a;
    response[29] = 1;
    stw_be_p(response + 30, text_length);
    stl_be_p(response + 32, 0x34);
    memcpy(response + 36, text, text_length);

    for (offset = 0; offset < response_length; ) {
        uint8_t chunk[128] = { 0 };
        unsigned bytes = MIN(113, response_length - offset);
        unsigned chunk_length = 10 + bytes;

        chunk[0] = 0x20 | channel;
        stw_be_p(chunk + 5, offset ? 0x3003 : 0x3001);
        chunk[7] = offset ? 0 : 1;
        stw_be_p(chunk + 8, offset ? 0 : response_length);
        memcpy(chunk + 10, response + offset, bytes);
        if (o300) {
            l1_o300_trace_frame(s, true, transport, chunk, chunk_length);
        }
        l1_emit_transport(s, transport, chunk, chunk_length);
        offset += bytes;
    }
}

static void l1_irouter_complete_fragment(SGIL1State *s, unsigned transport,
                                         uint8_t channel,
                                         SGIL1IRouterAssembly *assembly)
{
    if (assembly->received != assembly->expected) {
        return;
    }
    if (!assembly->first_length) {
        assembly->first_length = assembly->total;
    }
    assembly->fragments_left--;
    if (!assembly->fragments_left) {
        unsigned first_length = assembly->first_length;
        unsigned fragment_count = assembly->fragment_count;

        if (fragment_count == 1) {
            l1_irouter_reply(s, transport, channel, assembly->data,
                             first_length, fragment_count);
        } else {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: multi-fragment IRouter dispatch is unsupported\n");
        }
        memset(assembly, 0, sizeof(*assembly));
    }
}

static void l1_irouter_chunk(SGIL1State *s, const uint8_t *p,
                             unsigned length, unsigned transport)
{
    uint16_t command = lduw_be_p(p + 5);
    uint8_t channel = p[0] & 0x1f;
    SGIL1IRouterAssembly *assembly;
    unsigned payload_length;

    if (transport >= SGI_L1_IROUTER_TRANSPORTS || length < 10 ||
        (p[0] & 0xe0) || ldl_be_p(p + 1) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-l1: malformed IRouter chunk transport\n");
        return;
    }
    l1_o300_trace_frame(s, false, transport, p, length);
    assembly = &s->irouter[transport * SGI_L1_IROUTER_CHANNELS + channel];
    payload_length = length - 10;
    if (command == 0x3001) {
        uint8_t fragments = p[7];
        uint16_t fragment_length = lduw_be_p(p + 8);

        if (assembly->active) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-l1: replacing pending IRouter request\n");
        }
        memset(assembly, 0, sizeof(*assembly));
        if (!l1_irouter_enabled(s) || !fragments || fragments >= 128 ||
            !fragment_length ||
            fragment_length > sizeof(assembly->data) ||
            payload_length > fragment_length) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: unsupported IRouter message start\n");
            return;
        }
        assembly->active = true;
        assembly->fragment_count = fragments;
        assembly->fragments_left = fragments;
        assembly->expected = fragment_length;
        memcpy(assembly->data, p + 10, payload_length);
        assembly->received = payload_length;
        assembly->total = payload_length;
        l1_irouter_complete_fragment(s, transport, channel, assembly);
    } else if (command == 0x3002) {
        uint16_t fragment_length = lduw_be_p(p + 8);

        if (!assembly->active || !assembly->fragments_left ||
            assembly->received != assembly->expected || !fragment_length ||
            fragment_length > sizeof(assembly->data) - assembly->total ||
            payload_length > fragment_length) {
            memset(assembly, 0, sizeof(*assembly));
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-l1: invalid IRouter next fragment\n");
            return;
        }
        assembly->expected = fragment_length;
        assembly->received = payload_length;
        memcpy(assembly->data + assembly->total, p + 10, payload_length);
        assembly->total += payload_length;
        l1_irouter_complete_fragment(s, transport, channel, assembly);
    } else if (command == 0x3003) {
        if (!assembly->active ||
            assembly->received == assembly->expected ||
            payload_length > assembly->expected - assembly->received ||
            payload_length > sizeof(assembly->data) - assembly->total) {
            memset(assembly, 0, sizeof(*assembly));
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-l1: IRouter continuation without valid message\n");
            return;
        }
        memcpy(assembly->data + assembly->total, p + 10, payload_length);
        assembly->received += payload_length;
        assembly->total += payload_length;
        l1_irouter_complete_fragment(s, transport, channel, assembly);
    } else {
        memset(assembly, 0, sizeof(*assembly));
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: unsupported IRouter fragment command 0x%04x\n",
                      command);
        return;
    }
}

static void l1_request_frame(SGIL1State *s, const uint8_t *p,
                             unsigned length, unsigned transport)
{
    uint8_t channel = p[0] & 0x1f;
    uint32_t address;
    uint16_t command;
    unsigned count, args_len;
    const uint8_t *args;
    int32_t error = 0;
    uint8_t fields[48];
    unsigned field_count = 0, field_length = 0;

    if (l1_irouter_enabled(s) && length >= 7 && !(p[0] & 0xe0) &&
        ldl_be_p(p + 1) == 0 &&
        p[5] == 0x30 && (p[6] == 1 || p[6] == 2 || p[6] == 3)) {
        l1_irouter_chunk(s, p, length, transport);
        return;
    }
    if ((p[0] & 0xe0) == 0x40) {
        if (!s->profile->compute_services) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: processor console unsupported on %s profile\n",
                          s->profile->name);
            return;
        }
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
        if (s->query_router && ((address >> 28) == 0 ||
            (address == UINT32_C(0x4ffff006) &&
             s->query_router->attachment_count))) {
            SGIL1QueryRequest request = {
                .issuer = s,
                .recipient = s,
                .destination = address,
                .payload = p,
                .length = length,
                .channel = channel,
            };
            sgi_l1_query_route(s->query_router, &request);
            return;
        }
        if (!s->reply_to && l1_forward_to_peer(s, p, length, address)) {
            return;
        }
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: no receiver for destination 0x%08x\n", address);
        l1_router_error(s, channel);
        return;
    }
    count = length == 7 ? 0 : p[7];
    args = p + MIN(length, 8);
    args_len = length > 8 ? length - 8 : 0;
    if (l1_discovery_reference(s)) {
        SGIL1State *recipient = s->reply_to ? s->reply_to : s;

        if (!l1_arguments(args, args_len, count)) {
            l1_reply_direct(recipient, channel, -100, 0, NULL, 0);
        } else {
            l1_reference_refuse(s, recipient, channel, address & 0x1f);
        }
        return;
    }
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

        if (!s->profile->compute_services) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: console task unsupported on %s profile\n",
                          s->profile->name);
            l1_reply(s, channel, -101, 0, NULL, 0);
            return;
        }
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
    if (!s->profile->compute_services &&
        (command == 0x1002 || command == 0x1003 || command == 0x100b ||
         command == 0x100c || command == 0x2001 || command == 0x2005 ||
         command == 0x2006 || command == 0x2007 || command == 0x2008)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: compute request 0x%04x unsupported on "
                      "%s profile\n",
                      command, s->profile->name);
        l1_reply(s, channel, -101, 0, NULL, 0);
        return;
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
    if (l1_read_query(s, s->reply_to ? s->reply_to : s, channel,
                      command, args, args_len, count)) {
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
        /* The first integer selects a PROM display branch, not link speed. */
        field_count = 2;
        field_length = 10;
        memset(fields, 0, field_length);
        stl_be_p(fields + 1, s->port_speed_first);
        break;
    case 0xd:
        /* No attached bricks: rack 0, bay 0, type 0 per port. */
        field_count = 4;
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
    case 8: {
        const char *serial = s->system_serial;

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
        /*
         * The per-processor entry is stored by the firmware; not read back.
         */
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
        fields[0] = 0;
        stl_be_p(fields + 1, (s->rack << 6) | s->bay);
        field_count = 1;
        field_length = 5;
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
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-l1: request 0x%04x unavailable\n",
                      command);
        error = -101;
        break;
    }
    l1_reply(s, channel, error, field_count, fields, field_length);
}

static void l1_parse(SGIL1State *s, unsigned transport, uint8_t byte)
{
    SGIL1FrameState *frame = &s->frame[transport];

    if (byte == 0x7e) {
        /* A header and its check: the PROM's one-character echo is four. */
        if (frame->in_frame && !frame->overflow && !frame->escape &&
            frame->length >= 3) {
            uint16_t crc = 0xffff;
            for (unsigned i = 0; i < frame->length; i++) {
                crc = l1_fcs(crc, frame->data[i]);
            }
            if (crc == 0xf0b8) {
                l1_request(s, transport);
            }
        }
        frame->in_frame = true;
        frame->length = 0;
        frame->escape = false;
        frame->overflow = false;
        return;
    }
    if (!frame->in_frame || frame->overflow) {
        return;
    }
    if (byte == 0x7d) {
        /* A second escape only re-arms the first. */
        frame->escape = true;
        return;
    }
    if (frame->escape) {
        byte ^= 0x20;
        frame->escape = false;
    }
    if (frame->length == SGI_L1_FRAME_SIZE) {
        frame->overflow = true;
        return;
    }
    frame->data[frame->length++] = byte;
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
            l1_parse(u->controller, u->transport_index, value);
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
    SGIL1State *s = SGI_L1(obj);

    memset(s->irouter, 0, sizeof(s->irouter));
    s->usb_test_state.set = false;
    g_strlcpy(s->usb_test_result, "none", sizeof(s->usb_test_result));
    g_strlcpy(s->usb_test_descriptor_result, "none",
              sizeof(s->usb_test_descriptor_result));
    l1_boot_local(s);
}

static void l1_reset_hold(Object *obj, ResetType type)
{
    l1_bic_startup(SGI_L1(obj));
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
                                   unsigned area, unsigned header_byte,
                                   uint32_t configured_capacity)
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
    if (!length || start + length >= configured_capacity ||
        start + length > record->capacity) {
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

static void l1_selected_board_scan(SGIL1SelectedEEPROM *record,
                                   uint32_t configured_capacity)
{
    memset(record->status, 1, 4);
    record->length[0] = 8;
    if (!l1_checksum_ok(record->image, 8)) {
        record->status[0] = 3;
        return;
    }
    record->status[0] = 0;
    l1_selected_board_area(record, 1, 2, configured_capacity);
    l1_selected_board_area(record, 2, 3, configured_capacity);
    l1_selected_board_area(record, 3, 1, configured_capacity);
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
                              unsigned component,
                              const SGIL1ProductProfile *profile)
{
    if (!record->present) {
        return !record->kind && !record->capacity && !record->image;
    }
    if (record->kind != (component < profile->board_count ? 1 : 2) ||
        (profile->board_capacity &&
         (component >= profile->board_count ||
          record->capacity > profile->board_capacity)) ||
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

static bool l1_frame_state_valid(void *opaque, int version_id);

static int l1_post_load(void *opaque, int version_id)
{
    SGIL1State *s = opaque;

    if ((!s->profile->compute_services &&
         (s->node_power || s->power_up_pending || s->power_down_pending ||
          s->console_mode || s->escaped)) ||
        s->command_len >= sizeof(s->command) ||
        s->port_field > 0x7f ||
        s->revision_major < -1 || s->revision_minor < -1 ||
        s->revision_patch < -1 ||
        ((s->revision_major < 0 || s->revision_minor < 0 ||
          s->revision_patch < 0) &&
         (s->revision_major >= 0 || s->revision_minor >= 0 ||
          s->revision_patch >= 0)) ||
        s->two_bit_setting > 3 || s->debug_switches > 0xffff ||
        s->brick_type_code > 0xff ||
        s->log_head >= SGI_L1_LOG_ENTRIES ||
        s->log_len > SGI_L1_LOG_ENTRIES ||
        s->rack >= 0x3ff || s->bay >= 0x3f ||
        s->pending_rack >= 0x3ff || s->pending_bay >= 0x3f ||
        s->display[0][127] || s->display[1][127]) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < SGI_L1_IROUTER_TRANSPORTS; i++) {
        if (!l1_frame_state_valid(&s->frame[i], 0)) {
            return -EINVAL;
        }
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
        if (!l1_selected_valid(&s->selected[component], component,
                               s->profile)) {
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

static bool l1_irouter_assembly_valid(void *opaque, int version_id)
{
    SGIL1IRouterAssembly *assembly = opaque;

    return assembly->expected <= SGI_L1_IROUTER_MAX_FRAGMENT &&
           assembly->received <= assembly->expected &&
           assembly->total <= SGI_L1_IROUTER_MAX_FRAGMENT &&
           assembly->first_length <= assembly->total &&
           assembly->fragments_left <= assembly->fragment_count &&
           assembly->fragment_count < 128 &&
           (!assembly->active || (assembly->expected &&
                                  assembly->fragment_count));
}

static const VMStateDescription vmstate_l1_irouter_assembly = {
    .name = "sgi-l1/irouter-assembly",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(active, SGIL1IRouterAssembly),
        VMSTATE_UINT8(fragment_count, SGIL1IRouterAssembly),
        VMSTATE_UINT8(fragments_left, SGIL1IRouterAssembly),
        VMSTATE_UINT16(expected, SGIL1IRouterAssembly),
        VMSTATE_UINT16(received, SGIL1IRouterAssembly),
        VMSTATE_UINT16(total, SGIL1IRouterAssembly),
        VMSTATE_UINT16(first_length, SGIL1IRouterAssembly),
        VMSTATE_VALIDATE("assembly-bounds", l1_irouter_assembly_valid),
        VMSTATE_UINT8_ARRAY(data, SGIL1IRouterAssembly,
                            SGI_L1_IROUTER_MAX_FRAGMENT),
        VMSTATE_END_OF_LIST()
    },
};

static bool l1_frame_state_valid(void *opaque, int version_id)
{
    SGIL1FrameState *frame = opaque;

    return frame->length <= SGI_L1_FRAME_SIZE;
}

static const VMStateDescription vmstate_l1_frame = {
    .name = "sgi-l1/frame",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(data, SGIL1FrameState, SGI_L1_FRAME_SIZE),
        VMSTATE_UINT32(length, SGIL1FrameState),
        VMSTATE_BOOL(in_frame, SGIL1FrameState),
        VMSTATE_BOOL(escape, SGIL1FrameState),
        VMSTATE_BOOL(overflow, SGIL1FrameState),
        VMSTATE_VALIDATE("frame-bounds", l1_frame_state_valid),
        VMSTATE_END_OF_LIST()
    },
};

static int l1_pre_save(void *opaque)
{
    SGIL1State *s = opaque;

    return s->power_command_active || s->bic_operation_active ||
           (s->query_router && s->query_router->active) ? -EBUSY : 0;
}

/* Product identity must match before loading the selected record cache. */
static const VMStateDescription vmstate_l1 = {
    .name = TYPE_SGI_L1,
    .version_id = 13,
    .minimum_version_id = 13,
    .pre_save = l1_pre_save,
    .post_load = l1_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(product_identity, SGIL1State),
        VMSTATE_UINT64_EQUAL(irouter_profile_identity, SGIL1State),
        VMSTATE_STRUCT_ARRAY(irouter, SGIL1State,
                             SGI_L1_IROUTER_TRANSPORTS *
                             SGI_L1_IROUTER_CHANNELS, 0,
                             vmstate_l1_irouter_assembly,
                             SGIL1IRouterAssembly),
        VMSTATE_UINT32_EQUAL(port_speed_first, SGIL1State),
        VMSTATE_UINT64_EQUAL(power_identity, SGIL1State),
        VMSTATE_UINT64_EQUAL(power_command_identity, SGIL1State),
        VMSTATE_BOOL(power_command_reachable, SGIL1State),
        VMSTATE_UINT32_EQUAL(bic_startup, SGIL1State),
        VMSTATE_UINT64_EQUAL(bic_endpoints[0].identity, SGIL1State),
        VMSTATE_UINT64_EQUAL(bic_endpoints[1].identity, SGIL1State),
        VMSTATE_UINT64_EQUAL(query_owner_identity, SGIL1State),
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
        VMSTATE_UINT32(brick_type_code, SGIL1State),
        VMSTATE_INT32(revision_major, SGIL1State),
        VMSTATE_INT32(revision_minor, SGIL1State),
        VMSTATE_INT32(revision_patch, SGIL1State),
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
        VMSTATE_STRUCT_ARRAY(frame, SGIL1State,
                             SGI_L1_IROUTER_TRANSPORTS, 0,
                             vmstate_l1_frame, SGIL1FrameState),
        VMSTATE_UINT8_2DARRAY(display, SGIL1State, 2, 128),
        VMSTATE_END_OF_LIST()
    },
};

/*
 * Synthetic identity records: what the L1 hands out when no raw record file
 * names a component. Each area uses the Information-Area encoding OpenBSD's
 * l1.c reads: a type/length byte (top two bits the type, low six the length),
 * then the bytes. SGI 007-4498-002 printed pages 181-182 and 007-4365-001
 * printed page 75 give the IO8 areas' 16- and 80-byte field boundaries.
 * The contents are not factory data: dates, keys and the IO8 part are
 * placeholders, the serials are marked synthetic ones, and only the node
 * board's name, part and revision letter come from real inventories (the
 * caller supplies them). Every area
 * sums to zero modulo 256. A Fuel's single system board answers as both
 * the node board and the I/O logic board, the second area carrying the
 * station address in the IO8 position.
 */
#define L1_IA_BINARY 0
#define L1_IA_ASCII 3
#define L1_IA_EOF 0xc1

static void l1_ia_field(GByteArray *area, unsigned type, const char *data,
                        size_t length)
{
    uint8_t tag = type << 6 | length;

    g_assert(length <= 0x3f);
    g_byte_array_append(area, &tag, 1);
    g_byte_array_append(area, (const uint8_t *)data, length);
}

static void l1_ia_text(GByteArray *area, const char *text)
{
    l1_ia_field(area, L1_IA_ASCII, text, strlen(text));
}

static void l1_ia_u32(GByteArray *area, uint32_t value)
{
    uint8_t data[4];

    stl_be_p(data, value);
    l1_ia_field(area, L1_IA_BINARY, (const char *)data, sizeof(data));
}

/* Pads to a multiple of eight bytes and appends the zero-sum checksum. */
static void l1_ia_finish(SGIL1EEPROM *record, GByteArray *area)
{
    uint8_t zero = 0;
    uint8_t sum = 0;

    while ((area->len + 1) % 8) {
        g_byte_array_append(area, &zero, 1);
    }
    for (unsigned i = 0; i < area->len; i++) {
        sum += area->data[i];
    }
    sum = -sum;
    g_byte_array_append(area, &sum, 1);
    record->length = area->len;
    record->data = g_byte_array_free(area, false);
    record->invalid = false;
}

typedef struct SGIL1CPUContext {
    uint32_t mode_word;
    uint32_t cpu_mhz;
    uint32_t hub_mhz;
} SGIL1CPUContext;

/* The chassis area: both identity strings the placeholder "NA". */
static void l1_build_chassis_area(SGIL1EEPROM *record)
{
    static const uint8_t header[] = { 0x00, 0x02, 0x00 };
    GByteArray *area = g_byte_array_new();
    uint8_t eof = L1_IA_EOF;

    g_byte_array_append(area, header, sizeof(header));
    l1_ia_text(area, "NA");
    l1_ia_text(area, "NA");
    g_byte_array_append(area, &eof, 1);
    l1_ia_finish(record, area);
}

/*
 * A board area: placeholder manufacturer "SYNTHETIC", a marked synthetic
 * serial and a zero date, the given name, part and revision, and zeroed
 * placeholder fields. An optional IP53 CPU context is separate from the
 * identity prefix. An I/O board also carries its station address. The serial
 * is component @letter's, instance
 * @index: "EMUN00" for node 0's board. On the model, with "000000" here
 * the stock IP35 PROM reported after every reset that the NVRAM did not
 * belong to this system and reinitialized it, and with the marked serials
 * it does not. Which property of "000000" the PROM rejects is unknown.
 */
static void l1_build_board_area(SGIL1EEPROM *record, const char *name,
                                const char *part, const char *revision,
                                const uint8_t *mac, char letter,
                                unsigned int index,
                                const SGIL1CPUContext *cpu_context)
{
    static const char zeros[4];
    GByteArray *area = g_byte_array_new();
    uint8_t header[6] = { 0 };
    uint8_t fru_file = 0, eof = L1_IA_EOF, type_three = 0x03;
    char serial[SGI_SYNTHETIC_TEXT_MIN_WIDTH + 1];

    sgi_synthetic_text_serial(serial, SGI_SYNTHETIC_TEXT_MIN_WIDTH, letter,
                              index);
    g_byte_array_append(area, header, sizeof(header));
    if (cpu_context) {
        area->data[0] = 0x02;
    }
    l1_ia_text(area, "SYNTHETIC");
    l1_ia_text(area, name);
    l1_ia_text(area, serial);
    l1_ia_text(area, part);
    g_byte_array_append(area, &fru_file, 1);
    l1_ia_text(area, revision);
    l1_ia_field(area, L1_IA_BINARY, zeros, 1);
    l1_ia_text(area, "00");
    if (cpu_context) {
        l1_ia_field(area, L1_IA_BINARY, (const char *)&type_three, 1);
        static const char empty;

        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
        l1_ia_u32(area, cpu_context->mode_word);
        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
        l1_ia_u32(area, cpu_context->hub_mhz);
        l1_ia_u32(area, cpu_context->cpu_mhz);
        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
        l1_ia_field(area, L1_IA_BINARY, &empty, 0);
    }
    for (unsigned i = 0; i < 3; i++) {
        l1_ia_field(area, L1_IA_BINARY, zeros, sizeof(zeros));
    }
    if (mac) {
        char digits[13];

        snprintf(digits, sizeof(digits), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        l1_ia_text(area, digits);
    }
    g_byte_array_append(area, &eof, 1);
    /* Byte 1 counts the area's eight-byte units, checksum included. */
    area->data[1] = (area->len + 1 + 7) / 8;
    l1_ia_finish(record, area);
}

/*
 * The station address in an 80-byte IO8 board area with the published
 * layout: three binary tags at 49, 54 and 59, the twelve-digit ASCII field
 * tagged at 64, end-of-fields at 77. Another layout yields no address.
 */
static bool l1_io8_board_mac(const SGIL1EEPROM *record, uint8_t mac[6])
{
    const uint8_t *data = record->data;

    if (record->length != 80 || record->invalid ||
        data[0] || data[1] != 0x0a || data[2] ||
        data[49] != 0x04 || data[54] != 0x04 || data[59] != 0x04 ||
        data[64] != 0xcc || data[77] != L1_IA_EOF || data[78]) {
        return false;
    }
    for (unsigned i = 0; i < 12; i++) {
        if (!g_ascii_isxdigit(data[65 + i])) {
            return false;
        }
    }
    for (unsigned i = 0; i < 6; i++) {
        mac[i] = g_ascii_xdigit_value(data[65 + 2 * i]) << 4 |
                 g_ascii_xdigit_value(data[66 + 2 * i]);
    }
    return memcmp(mac, "\0\0\0\0\0\0", 6) && !(mac[0] & 1);
}

static bool l1_component_selected(SGIL1State *s, unsigned component)
{
    return component < ARRAY_SIZE(s->selected) &&
           s->selected[component].present;
}

/*
 * Fills each identity area no raw file supplied. Component 0 is the node
 * board, component 1 the IO8 or, for a system board, the node board's
 * identity again with the station address; a selected record on a
 * component leaves it alone. The Fuel-family MAC text record has no modeled
 * EEPROM bytes: only the RAM copy the firmware serves through request
 * 0x0008 is seeded. A system board seeds that text too, and its component 1
 * area carries the same address, so every reader sees one MAC.
 */
static bool l1_build_identity(SGIL1State *s, Error **errp)
{
    bool io8 = false, mac_text = false, system_board = false;
    SGIL1CPUContext cpu_context;
    const SGIL1CPUContext *cpu_context_ptr = NULL;
    uint8_t mac[6];

    if (s->mac_record && s->mac_record[0]) {
        io8 = !strcmp(s->mac_record, "io8-board");
        mac_text = !strcmp(s->mac_record, "mac-text");
        system_board = !strcmp(s->mac_record, "system-board");
        if (!io8 && !mac_text && !system_board) {
            error_setg(errp, "L1 mac-record must be io8-board, mac-text or "
                       "system-board");
            return false;
        }
    }
    if (system_board && !s->node_board_name) {
        error_setg(errp, "L1 system-board mac-record needs a node board "
                   "identity");
        return false;
    }
    mac_text |= system_board;
    if (s->mac) {
        if (!io8 && !mac_text) {
            error_setg(errp, "L1 mac needs a mac-record");
            return false;
        }
        if (net_parse_macaddr(mac, s->mac) < 0 ||
            !memcmp(mac, "\0\0\0\0\0\0", 6) || (mac[0] & 1)) {
            error_setg(errp, "L1 mac must be a nonzero unicast address");
            return false;
        }
    } else if (io8 || (mac_text && !s->board_mac_valid)) {
        error_setg(errp, "L1 mac-record needs a mac");
        return false;
    }
    if ((s->node_board_name || s->node_board_part ||
         s->node_board_revision) &&
        (!s->node_board_name || !s->node_board_part ||
         !s->node_board_revision)) {
        error_setg(errp, "L1 node board identity needs a name, part and "
                   "revision");
        return false;
    }
    if ((s->cpu_mode_word || s->cpu_clock_hz || s->hub_clock_hz) &&
        !s->cpu_context_present) {
        error_setg(errp, "L1 CPU context values need cpu-context-present");
        return false;
    }
    if (s->cpu_context_present &&
        (!s->cpu_clock_hz || !s->hub_clock_hz ||
         s->cpu_clock_hz % 1000000 || s->hub_clock_hz % 1000000 ||
         s->cpu_clock_hz / 1000000 > UINT32_MAX ||
         s->hub_clock_hz / 1000000 > UINT32_MAX)) {
        error_setg(errp, "L1 CPU context clocks must be positive integral "
                   "MHz values that fit 32 bits");
        return false;
    }
    if (s->cpu_context_present) {
        cpu_context = (SGIL1CPUContext) {
            .mode_word = s->cpu_mode_word,
            .cpu_mhz = s->cpu_clock_hz / 1000000,
            .hub_mhz = s->hub_clock_hz / 1000000,
        };
        cpu_context_ptr = &cpu_context;
    }
    if (s->node_board_name &&
        (strlen(s->node_board_name) > 0x3f ||
         strlen(s->node_board_part) > 0x3f ||
         strlen(s->node_board_revision) > 0x3f)) {
        error_setg(errp, "L1 node board identity field is too long");
        return false;
    }
    if ((s->node_board_name || io8 || mac_text) &&
        !s->profile->compute_services) {
        error_setg(errp, "L1 expansion profile has no node identity");
        return false;
    }

    if ((s->node_board_name || s->cpu_context_present) &&
        !l1_component_selected(s, 0)) {
        if (!s->records[0].length) {
            l1_build_chassis_area(&s->records[0]);
        }
        if (!s->records[16].length) {
            l1_build_board_area(&s->records[16],
                                s->node_board_name ? s->node_board_name : "",
                                s->node_board_part ? s->node_board_part : "",
                                s->node_board_revision ?
                                    s->node_board_revision : "",
                                NULL, 'N', s->identity_index,
                                cpu_context_ptr);
        }
    }
    if (io8 && !l1_component_selected(s, 1)) {
        if (!s->records[1].length) {
            l1_build_chassis_area(&s->records[1]);
        }
        if (!s->records[17].length) {
            l1_build_board_area(&s->records[17], "IO8", "NOTOBSERVED0", "00",
                                mac, 'I', s->identity_index, NULL);
        }
        /* A supplied board area carries its own address, if it has one. */
        s->board_mac_valid = l1_io8_board_mac(&s->records[17], s->board_mac);
    }
    if (mac_text && !s->board_mac_valid) {
        if (s->system_serial) {
            error_setg(errp, "L1 MAC text and system-serial conflict");
            return false;
        }
        s->system_serial = g_strdup_printf("%02X%02X%02X%02X%02X%02X",
                                           mac[0], mac[1], mac[2], mac[3],
                                           mac[4], mac[5]);
        memcpy(s->board_mac, mac, sizeof(s->board_mac));
        s->board_mac_valid = true;
    }
    if (system_board && !l1_component_selected(s, 1)) {
        if (!s->records[1].length) {
            l1_build_chassis_area(&s->records[1]);
        }
        /* The same one board as component 0, so the same serial. */
        if (!s->records[17].length) {
            l1_build_board_area(&s->records[17], s->node_board_name,
                                s->node_board_part, s->node_board_revision,
                                s->board_mac, 'N', s->identity_index, NULL);
        }
    }
    return true;
}

static uint64_t l1_irouter_profile_identity(const SGIL1State *s)
{
    uint64_t hash = UINT64_C(14695981039346656037);

#define L1_IROUTER_HASH_BYTE(byte) \
    do { \
        hash = (hash ^ (uint8_t)(byte)) * UINT64_C(1099511628211); \
    } while (0)

    L1_IROUTER_HASH_BYTE(s->irouter_compat);
    if (s->o300_l1cmd_compat) {
        L1_IROUTER_HASH_BYTE(1);
    }
    if (!l1_irouter_enabled(s)) {
        return hash;
    }
    L1_IROUTER_HASH_BYTE(s->brick_type_code >> 24);
    L1_IROUTER_HASH_BYTE(s->brick_type_code >> 16);
    L1_IROUTER_HASH_BYTE(s->brick_type_code >> 8);
    L1_IROUTER_HASH_BYTE(s->brick_type_code);
    L1_IROUTER_HASH_BYTE(s->brick_serial ? 1 : 0);
    if (s->brick_serial) {
        for (const uint8_t *p = (const uint8_t *)s->brick_serial; *p; p++) {
            L1_IROUTER_HASH_BYTE(*p);
        }
    }
    if (s->o300_l1cmd_compat && s->system_serial) {
        for (const uint8_t *p = (const uint8_t *)s->system_serial; *p; p++) {
            L1_IROUTER_HASH_BYTE(*p);
        }
    }
#undef L1_IROUTER_HASH_BYTE
    return hash;
}

static void l1_realize(DeviceState *dev, Error **errp)
{
    SGIL1State *s = SGI_L1(dev);

    char **files[] = { s->chassis_files, s->board_files, s->spd_files };
    uint32_t counts[] = { s->chassis_count, s->board_count, s->spd_count };

    s->irouter_profile_identity = l1_irouter_profile_identity(s);

    if (s->fuel_mac_eeprom) {
        char data[513];
        gsize length = 0;
        const uint8_t *bytes;
        size_t start, area_length, cursor;
        unsigned sum = 0;
        bool mac_field = false;
        int fd;

        if (s->system_serial) {
            error_setg(errp, "Fuel MAC EEPROM and system-serial conflict");
            return;
        }
        fd = qemu_open(s->fuel_mac_eeprom, O_RDONLY | O_BINARY, errp);
        if (fd < 0) {
            error_prepend(errp, "Fuel MAC EEPROM: ");
            return;
        }
        /* One extra byte distinguishes the fixed image from oversized input. */
        while (length < sizeof(data)) {
            ssize_t n = read(fd, data + length, sizeof(data) - length);

            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                error_setg_errno(errp, errno, "Fuel MAC EEPROM: read '%s'",
                                 s->fuel_mac_eeprom);
                qemu_close(fd);
                return;
            }
            if (!n) {
                break;
            }
            length += n;
        }
        qemu_close(fd);
        bytes = (const uint8_t *)data;
        if (length != 512) {
            goto invalid_fuel_mac;
        }
        for (size_t i = 0; i < 8; i++) {
            sum += bytes[i];
        }
        start = (size_t)bytes[3] * 8;
        if ((sum & 0xff) || start < 8 || start + 7 >= length) {
            goto invalid_fuel_mac;
        }
        area_length = (size_t)bytes[start + 1] * 8;
        if (!area_length || start + area_length >= length) {
            goto invalid_fuel_mac;
        }
        sum = 0;
        for (size_t i = start; i < start + area_length; i++) {
            sum += bytes[i];
        }
        if (sum & 0xff) {
            goto invalid_fuel_mac;
        }
        cursor = 7 + (bytes[start + 6] & 63);
        for (unsigned field = 0; field < 5; field++) {
            size_t field_length, advance;

            if (cursor >= area_length) {
                goto invalid_fuel_mac;
            }
            field_length = bytes[start + cursor] & 63;
            if (cursor + 1 + field_length > area_length) {
                goto invalid_fuel_mac;
            }
            if (field == 0) {
                mac_field = field_length >= 4 &&
                    !memcmp(bytes + start + cursor + 1, "MAC ", 4);
            }
            if (field == 1 && field_length &&
                cursor + 1 + field_length <= area_length &&
                bytes[start + cursor + field_length] == '\t') {
                field_length--;
            }
            advance = field_length + (field < 2 ? 1 : field == 3 ? 3 : 2);
            if (cursor + advance > area_length ||
                cursor + 1 + field_length > area_length) {
                goto invalid_fuel_mac;
            }
            cursor += advance;
        }
        if (!mac_field || cursor + 14 >= area_length) {
            goto invalid_fuel_mac;
        }
        cursor += 14;
        if ((bytes[start + cursor] & 63) != 12 ||
            cursor + 13 > area_length) {
            goto invalid_fuel_mac;
        }
        for (size_t i = 0; i < 12; i++) {
            if (!g_ascii_isxdigit(bytes[start + cursor + 1 + i])) {
                goto invalid_fuel_mac;
            }
        }
        s->system_serial = g_strndup(data + start + cursor + 1, 12);
        for (size_t i = 0; i < sizeof(s->board_mac); i++) {
            s->board_mac[i] =
                (g_ascii_xdigit_value(s->system_serial[2 * i]) << 4) |
                g_ascii_xdigit_value(s->system_serial[2 * i + 1]);
        }
        if (!memcmp(s->board_mac, "\0\0\0\0\0\0",
                    sizeof(s->board_mac)) || (s->board_mac[0] & 1)) {
            goto invalid_fuel_mac;
        }
        s->board_mac_valid = true;
        goto fuel_mac_done;

invalid_fuel_mac:
        error_setg(errp, "Fuel MAC EEPROM record is malformed");
        return;
fuel_mac_done:
        ;
    }

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
    if (s->identity_index > 99) {
        error_setg(errp, "L1 identity-index must be 0 to 99");
        return;
    }
    if (s->node_position > 3) {
        error_setg(errp, "L1 node-position must be 0 to 3");
        return;
    }
    if (!s->brick_type) {
        s->brick_type = g_strdup("ip45");
    }
    if (s->brick_type_code > 0xff) {
        error_setg(errp, "L1 brick-type-code must be one byte");
        return;
    }
    if (s->port_speed_first > 1) {
        error_setg(errp, "L1 first port-speed value must be zero or one");
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(l1_profiles); i++) {
        if (!strcmp(s->brick_type, l1_profiles[i].name)) {
            s->profile = &l1_profiles[i];
            break;
        }
    }
    if (!s->profile) {
        error_setg(errp, "L1 brick-type must be ip34, ip45, ip35, ip37, "
                   "unknown, pci-expansion, router or vbrick-discovery-reference");
        return;
    }
    if (s->bic_startup && strcmp(s->profile->name, "pci-expansion")) {
        error_setg(errp, "L1 BIC startup requires the expansion profile");
        return;
    }
    s->product_identity = (s->profile - l1_profiles) + 1;
    if (!s->profile->compute_services) {
        s->auto_power = false;
        s->processors = 0;
    }
    if (s->profile->board_capacity &&
        (s->chassis_count || s->board_count || s->spd_count ||
         s->spd_record_count ||
         s->board_record_count > s->profile->board_count)) {
        error_setg(errp, "L1 expansion profile accepts only four selected "
                   "board records");
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
        if (board == spd || (board && component >= s->profile->board_count) ||
            (spd && (s->profile->board_capacity ||
                     component < s->profile->board_count))) {
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
        if (length > 65535 ||
            (s->profile->board_capacity &&
             length > s->profile->board_capacity) ||
            (board && length < 8) ||
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
            l1_selected_board_scan(record, s->profile->board_capacity ?
                                    s->profile->board_capacity : length);
        } else {
            l1_selected_spd_scan(record);
        }
    }
    if (!l1_build_identity(s, errp)) {
        return;
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

bool sgi_l1_board_mac(DeviceState *dev, uint8_t mac[6])
{
    SGIL1State *s = SGI_L1(dev);

    if (!s->board_mac_valid) {
        return false;
    }
    memcpy(mac, s->board_mac, sizeof(s->board_mac));
    return true;
}

void sgi_l1_eeprom_digest(DeviceState *dev, uint8_t digest[32])
{
    static const uint8_t domain[] = "sgi-l1-effective-eeprom";
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
    SGIL1State *s = SGI_L1(dev);
    gsize digest_length = 32;

    g_assert(qdev_is_realized(dev));
    g_checksum_update(checksum, domain, sizeof(domain));
    for (unsigned area = 0; area < 3; area++) {
        for (unsigned component = 0; component < 16; component++) {
            const SGIL1EEPROM *record =
                &s->records[area * 16 + component];
            uint8_t metadata[7] = { area, component, record->invalid };

            stl_be_p(metadata + 3, record->length);
            g_checksum_update(checksum, metadata, sizeof(metadata));
            if (record->length) {
                g_checksum_update(checksum, record->data, record->length);
            }
        }
    }
    for (unsigned component = 0; component < ARRAY_SIZE(s->selected);
         component++) {
        const SGIL1SelectedEEPROM *record = &s->selected[component];
        uint8_t metadata[4] = { 3, component, record->present, record->kind };

        g_checksum_update(checksum, metadata, sizeof(metadata));
        if (record->present) {
            for (unsigned area = 0; area < ARRAY_SIZE(record->start); area++) {
                uint8_t selected_area[10] = {
                    area, record->status[area],
                };

                stl_be_p(selected_area + 2, record->start[area]);
                stl_be_p(selected_area + 6, record->length[area]);
                g_checksum_update(checksum, selected_area,
                                  sizeof(selected_area));
                if (record->status[area] == 0 && record->length[area]) {
                    g_assert(record->start[area] <= record->capacity);
                    g_assert(record->length[area] <=
                             record->capacity - record->start[area]);
                    g_checksum_update(checksum,
                                      record->image + record->start[area],
                                      record->length[area]);
                }
            }
        }
    }
    g_checksum_get_digest(checksum, digest, &digest_length);
    g_assert(digest_length == 32);
}

static char *l1_display_get(Object *obj, Error **errp)
{
    return g_strdup((char *)SGI_L1(obj)->display[1]);
}

static char *l1_display_first_get(Object *obj, Error **errp)
{
    return g_strdup((char *)SGI_L1(obj)->display[0]);
}

typedef struct L1USBPreparedStatus {
    bool has_value;
    uint8_t bytes[2];
    unsigned exposed;
} L1USBPreparedStatus;

/* Selected-image software calculation only; no USB transfer is prepared. */
static L1USBPreparedStatus l1_usb_status(const uint8_t setup[8],
                                          const SGIL1USBTestState *state)
{
    L1USBPreparedStatus result = { 0 };
    uint8_t recipient = setup[0] & 3;
    uint8_t endpoint = setup[4];

    result.exposed = MIN((unsigned)(setup[6] | setup[7] << 8), 2u);
    switch (recipient) {
    case 0:
        result.has_value = true;
        result.bytes[0] = 1;
        break;
    case 1:
        result.has_value = true;
        break;
    case 2:
        switch (endpoint) {
        case 0x00:
        case 0x80:
            result.bytes[0] = state->ep0 == 1;
            break;
        case 0x01:
            result.bytes[0] = state->out1 == 1;
            break;
        case 0x82:
            result.bytes[0] = state->in2 == 1;
            break;
        default:
            return result;
        }
        result.has_value = true;
        break;
    }
    return result;
}

static int l1_usb_hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void l1_usb_setup_set(Object *obj, const char *value, Error **errp)
{
    SGIL1State *s = SGI_L1(obj);
    uint8_t setup[8];
    L1USBPreparedStatus prepared;

    if (strlen(value) != 16) {
        error_setg(errp, "usb-test-setup requires exactly eight hex bytes");
        return;
    }
    for (unsigned i = 0; i < 8; i++) {
        int hi = l1_usb_hex(value[2 * i]);
        int lo = l1_usb_hex(value[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            error_setg(errp, "usb-test-setup contains a non-hex digit");
            return;
        }
        setup[i] = hi << 4 | lo;
    }
    if (setup[1] != 0 || (setup[0] != 0x80 && setup[0] != 0x81 &&
                          setup[0] != 0x82) || setup[2] || setup[3] ||
        setup[5] || (setup[0] != 0x82 && setup[4])) {
        error_setg(errp, "unsupported usb-test-setup request or index");
        return;
    }
    if (!s->usb_test_state.set) {
        error_setg(errp, "usb-test-cached-state must be injected first");
        return;
    }
    prepared = l1_usb_status(setup, &s->usb_test_state);
    if (!prepared.has_value) {
        g_strlcpy(s->usb_test_result, "none", sizeof(s->usb_test_result));
        return;
    }
    switch (prepared.exposed) {
    case 0:
        g_strlcpy(s->usb_test_result, "prepared:",
                  sizeof(s->usb_test_result));
        break;
    case 1:
        snprintf(s->usb_test_result, sizeof(s->usb_test_result),
                 "prepared:%02x", prepared.bytes[0]);
        break;
    default:
        snprintf(s->usb_test_result, sizeof(s->usb_test_result),
                 "prepared:%02x%02x", prepared.bytes[0], prepared.bytes[1]);
        break;
    }
}

static char *l1_usb_result_get(Object *obj, Error **errp)
{
    return g_strdup(SGI_L1(obj)->usb_test_result);
}

static void l1_usb_state_set(Object *obj, const char *value, Error **errp)
{
    SGIL1State *s = SGI_L1(obj);
    unsigned config, ep0, out1, in2;
    char canonical[32];

    if (sscanf(value, "config=%u,ep0=%u,out1=%u,in2=%u",
               &config, &ep0, &out1, &in2) != 4 ||
        config > 1 || ep0 < 1 || ep0 > 2 || out1 < 1 || out1 > 2 ||
        in2 < 1 || in2 > 2) {
        error_setg(errp, "invalid usb-test-cached-state");
        return;
    }
    snprintf(canonical, sizeof(canonical),
             "config=%u,ep0=%u,out1=%u,in2=%u", config, ep0, out1, in2);
    if (strcmp(value, canonical)) {
        error_setg(errp, "invalid usb-test-cached-state syntax");
        return;
    }
    s->usb_test_state = (SGIL1USBTestState) {
        .set = true, .config = config, .ep0 = ep0, .out1 = out1, .in2 = in2,
    };
}

static char *l1_usb_state_get(Object *obj, Error **errp)
{
    SGIL1USBTestState *state = &SGI_L1(obj)->usb_test_state;

    if (!state->set) {
        return g_strdup("unset");
    }
    return g_strdup_printf("config=%u,ep0=%u,out1=%u,in2=%u",
                           state->config, state->ep0, state->out1, state->in2);
}

static const uint8_t l1_usb_device_table[] = {
    0x12, 0x01, 0x00, 0x01, 0xff, 0xff, 0x00, 0x40, 0x5e,
    0x06, 0x34, 0x12, 0x00, 0x01, 0x01, 0x02, 0x03, 0x01,
};

static const uint8_t l1_usb_configuration_tree[] = {
    0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x04, 0x60, 0x00,
    0x09, 0x04, 0x00, 0x00, 0x02, 0x00, 0x00, 0xff, 0x05,
    0x07, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00,
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00,
};

typedef struct L1USBPreparedDescriptor {
    const uint8_t *table;
    size_t exposed;
} L1USBPreparedDescriptor;

/* Select internal software bytes without creating a USB transfer. */
static L1USBPreparedDescriptor l1_usb_descriptor(const uint8_t setup[8])
{
    L1USBPreparedDescriptor result;
    size_t length;

    if (setup[3] == 1) {
        result.table = l1_usb_device_table;
        length = sizeof(l1_usb_device_table);
    } else {
        result.table = l1_usb_configuration_tree;
        length = sizeof(l1_usb_configuration_tree);
    }
    result.exposed = MIN((size_t)(setup[6] | setup[7] << 8), length);
    return result;
}

static void l1_usb_descriptor_setup_set(Object *obj, const char *value,
                                         Error **errp)
{
    SGIL1State *s = SGI_L1(obj);
    uint8_t setup[8];
    L1USBPreparedDescriptor prepared;
    char result[sizeof(s->usb_test_descriptor_result)];
    static const char hex[] = "0123456789abcdef";

    if (strlen(value) != 16) {
        error_setg(errp, "usb-test-descriptor-setup requires eight hex bytes");
        return;
    }
    for (unsigned i = 0; i < 8; i++) {
        int hi = l1_usb_hex(value[2 * i]);
        int lo = l1_usb_hex(value[2 * i + 1]);

        if (hi < 0 || lo < 0) {
            error_setg(errp, "usb-test-descriptor-setup has a non-hex digit");
            return;
        }
        setup[i] = hi << 4 | lo;
    }
    if (setup[0] != 0x80 || setup[1] != 0x06 || setup[2] ||
        (setup[3] != 1 && setup[3] != 2) || setup[4] || setup[5]) {
        error_setg(errp, "unsupported usb-test-descriptor-setup selector");
        return;
    }
    prepared = l1_usb_descriptor(setup);
    memcpy(result, "prepared:", 9);
    for (size_t i = 0; i < prepared.exposed; i++) {
        result[9 + 2 * i] = hex[prepared.table[i] >> 4];
        result[10 + 2 * i] = hex[prepared.table[i] & 0xf];
    }
    result[9 + 2 * prepared.exposed] = 0;
    g_strlcpy(s->usb_test_descriptor_result, result,
              sizeof(s->usb_test_descriptor_result));
}

static char *l1_usb_descriptor_result_get(Object *obj, Error **errp)
{
    return g_strdup(SGI_L1(obj)->usb_test_descriptor_result);
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
    DEFINE_PROP_STRING("fuel-mac-eeprom", SGIL1State, fuel_mac_eeprom),
    DEFINE_PROP_STRING("mac", SGIL1State, mac),
    DEFINE_PROP_STRING("mac-record", SGIL1State, mac_record),
    DEFINE_PROP_STRING("node-board-name", SGIL1State, node_board_name),
    DEFINE_PROP_STRING("node-board-part", SGIL1State, node_board_part),
    DEFINE_PROP_BOOL("cpu-context-present", SGIL1State,
                     cpu_context_present, false),
    DEFINE_PROP_UINT32("cpu-mode-word", SGIL1State, cpu_mode_word, 0),
    DEFINE_PROP_UINT64("cpu-clock-hz", SGIL1State, cpu_clock_hz, 0),
    DEFINE_PROP_UINT64("hub-clock-hz", SGIL1State, hub_clock_hz, 0),
    DEFINE_PROP_UINT8("identity-index", SGIL1State, identity_index, 0),
    DEFINE_PROP_STRING("node-board-revision", SGIL1State,
                       node_board_revision),
    DEFINE_PROP_STRING("brick-serial", SGIL1State, brick_serial),
    DEFINE_PROP_UINT32("processors", SGIL1State, processors, 2),
    DEFINE_PROP_STRING("brick-type", SGIL1State, brick_type),
    DEFINE_PROP_UINT32("brick-type-code", SGIL1State, brick_type_code, 0),
    DEFINE_PROP_BOOL("irouter-compat", SGIL1State, irouter_compat, false),
    DEFINE_PROP_BOOL("o300-l1cmd-compat", SGIL1State, o300_l1cmd_compat,
                     false),
    DEFINE_PROP_UINT32("port-speed-first", SGIL1State, port_speed_first, 0),
    /*
     * The network mode the L1 keeps in non-volatile memory. 422 is the mode
     * the Origin 300 User's Guide requires for a cabled pair; the factory
     * default is unknown.
    */
    DEFINE_PROP_BOOL("network-422", SGIL1State, network_422, true),
};

static bool l1_power_command_reachable_get(Object *obj, Error **errp)
{
    SGIL1State *s = SGI_L1(obj);

    return s->power_command_owner && s->power_command_reachable;
}

static void l1_power_command_reachable_set(Object *obj, bool reachable,
                                           Error **errp)
{
    SGIL1State *s = SGI_L1(obj);

    if (!s->power_command_owner || s->power_command_active) {
        error_setg(errp, "power command association is absent or active");
        return;
    }
    s->power_command_reachable = reachable;
}

static void l1_init(Object *obj)
{
    SGIL1State *s = SGI_L1(obj);

    object_property_add_bool(obj, "power-command-reachable",
        l1_power_command_reachable_get, l1_power_command_reachable_set);

    g_strlcpy(s->usb_test_result, "none", sizeof(s->usb_test_result));
    g_strlcpy(s->usb_test_descriptor_result, "none",
              sizeof(s->usb_test_descriptor_result));
    if (qtest_enabled()) {
        object_property_add_str(obj, "usb-test-setup", NULL,
                                l1_usb_setup_set);
        object_property_add_str(obj, "usb-test-result", l1_usb_result_get,
                                NULL);
        object_property_add_str(obj, "usb-test-cached-state", NULL,
                                l1_usb_state_set);
        object_property_add_str(obj, "usb-test-cached-state-readback",
                                l1_usb_state_get, NULL);
        object_property_add_str(obj, "usb-test-descriptor-setup", NULL,
                                l1_usb_descriptor_setup_set);
        object_property_add_str(obj, "usb-test-descriptor-result",
                                l1_usb_descriptor_result_get, NULL);
    }

    qdev_init_gpio_out_named(DEVICE(obj), &s->node_power_out, "node-power", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->node_reset_out, "node-reset", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->soft_reset_out, "soft-reset", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->nmi_out, "nmi", 1);
    object_property_add_uint8_ptr(obj, "event-subchannel", &s->event_subchannel,
                                  OBJ_PROP_FLAG_READ);
    /*
     * Settable after realize: each chassis's node realizes its own L1, so the
     * machine can only join the two once both exist.
    */
    object_property_add_link(obj, "peer", TYPE_SGI_L1, (Object **)&s->peer,
                             object_property_allow_set_link, 0);
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
    rc->phases.hold = l1_reset_hold;
    object_class_property_add_str(klass, "display-line-1", l1_display_first_get,
                                  NULL);
    object_class_property_add_str(klass, "display-line-2",
                                  l1_display_get, NULL);
}

static void l1_finalize(Object *obj)
{
    SGIL1State *s = SGI_L1(obj);

    if (s->power_up_bh) {
        qemu_unregister_reset(l1_after_node_reset, s);
        qemu_bh_delete(s->power_up_bh);
    }
    for (unsigned i = 0; i < 2; i++) {
        if (s->bic_endpoints[i].receiver) {
            object_unref(s->bic_endpoints[i].receiver);
        }
    }

    for (unsigned i = 0; i < 48; i++) {
        g_free(s->records[i].data);
    }
    for (unsigned i = 0; i < 6; i++) {
        g_free(s->selected[i].image);
    }
    g_free(s->system_serial);
    g_free(s->fuel_mac_eeprom);
    g_free(s->brick_serial);
    g_free(s->brick_type);
}

/* UART window reset, migration and construction. */

static void l1_uart_reset_enter(Object *obj, ResetType type)
{
    SGIL1UARTState *u = SGI_L1_UART(obj);

    if (u->controller && u->transport_index < SGI_L1_IROUTER_TRANSPORTS) {
        memset(&u->controller->frame[u->transport_index], 0,
               sizeof(u->controller->frame[u->transport_index]));
        memset(&u->controller->irouter[
                   u->transport_index * SGI_L1_IROUTER_CHANNELS], 0,
               SGI_L1_IROUTER_CHANNELS * sizeof(u->controller->irouter[0]));
    }

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
        VMSTATE_UINT8_EQUAL(transport_index, SGIL1UARTState),
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

static const Property l1_uart_properties[] = {
    DEFINE_PROP_UINT8("transport-index", SGIL1UARTState, transport_index, 0),
};

static void l1_uart_controller_check(const Object *obj, const char *name,
                                     Object *value, Error **errp)
{
    SGIL1State *controller = SGI_L1(value);

    (void)obj;
    if (!controller) {
        error_setg(errp, "%s cannot be set to NULL", name);
    }
}

static void l1_uart_realize(DeviceState *dev, Error **errp)
{
    SGIL1UARTState *u = SGI_L1_UART(dev);

    if (!u->controller) {
        error_setg(errp, "L1 UART window needs a controller link");
        return;
    }
    if (u->transport_index >= SGI_L1_IROUTER_TRANSPORTS ||
        u->controller->transport[u->transport_index]) {
        if (u->transport_index == 0 && u->controller->transport[0]) {
            error_setg(errp, "L1 controller already has a UART window");
        } else {
            error_setg(errp,
                       "L1 UART transport index is invalid or already used");
        }
        return;
    }
    u->controller->transport[u->transport_index] = u;
    if (u->transport_index == 0) {
        u->controller->uart = u;
    }
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
    device_class_set_props(dc, l1_uart_properties);
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
