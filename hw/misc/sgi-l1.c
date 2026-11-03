/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI L1 system controller: the controller itself, and the node-side
 * UART window through which the Bedrock speaks its packet protocol.
 */
#include "qemu/osdep.h"
#include "hw/misc/sgi-l1.h"
#include "hw/misc/sgi-l1-env.h"
#include "hw/misc/sgi-l1-l2-link.h"
#include "hw/misc/sgi-l1-pbay.h"
#include "hw/mips/sgi/sgi-synthetic-serial.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/resettable.h"
#include "migration/blocker.h"
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

/* The Origin 3000 bricks whose L1 the profile models. */
enum {
    L1_O3K_NONE,
    L1_O3K_CBRICK,
    L1_O3K_IBRICK,
    L1_O3K_CXBRICK,
    L1_O3K_VBRICK,
};

/*
 * The attached-brick ports an I-brick lists its C-brick and its graphics
 * brick at. Neither is published; the model's numbering.
 */
#define L1_IBRICK_HOST_PORT 1
#define L1_IBRICK_GRAPHICS_PORT 2

/*
 * How long a controller that has been told to restart answers no peer. The
 * guides print the "no response from" line but give no figure, only "for a
 * while", so the duration alone is the model's choice.
 */
#define L1_O3K_RESTART_NS (1 * NANOSECONDS_PER_SECOND)

/* Product metadata does not describe physical UART wiring or power control. */
struct SGIL1ProductProfile {
    const char *name;
    uint32_t identity;
    unsigned board_count;
    uint32_t board_capacity;
    bool compute_services;
    uint8_t o3k;
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
    /*
     * The Origin 3000 compute brick and system I/O brick. The letters are
     * candidates (see docs/system/mips/sgi/hardware/l1.rst): a C-brick's is
     * valid for its module name only, and the case of an I-brick's is the
     * model's choice. Their records are all synthetic, so board_count and
     * board_capacity, which describe the file-backed record cache, stay zero.
     */
    { "cbrick", 0x43, 0, 0, true, L1_O3K_CBRICK },
    { "ibrick", 0x49, 0, 0, false, L1_O3K_IBRICK },
    /*
     * The N-brick's L1. It answers as an I/O brick does, one for the
     * several C-bricks cabled to it. Its type letter is a candidate only:
     * the L1 command set lists 'n' for the brick (007-4240-002 page 235)
     * but no capture shows what it reports.
     */
    { "nbrick", 0x4e, 0, 0, false, L1_O3K_IBRICK },
    /*
     * The Origin 3900 and Onyx 3000 Cx-brick: one L1 for one to four IP53
     * node boards. The L1 firmware's personality for it is 6, and request
     * 0x0003 returns that letter (router board records datasheet, "Other
     * answers from the same handler"); brick-type-code overrides it.
     */
    { "cxbrick", 0x36, 0, 0, true, L1_O3K_CXBRICK },
    /*
     * The Onyx 3000 V-brick: the X-brick's chassis and midplane XBridge
     * with V12 boards in its slots, and its own L1, cabled to a C-brick
     * like an I-brick. The L1 firmware has no V-brick personality and
     * takes its letter from the logic board's EEPROM name, so a V-brick
     * whose logic board is named as an X-brick's answers X. That is
     * inferred, not captured (docs/system/mips/sgi/hardware/l1.rst).
     */
    { "vbrick", 0x58, 0, 0, false, L1_O3K_VBRICK },
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

static bool l1_is_cbrick(const SGIL1State *s)
{
    return s->profile->o3k == L1_O3K_CBRICK;
}

static bool l1_is_ibrick(const SGIL1State *s)
{
    return s->profile->o3k == L1_O3K_IBRICK;
}

/* The N-brick shares the I/O brick profile and several C-bricks share it. */
static bool l1_is_nbrick(const SGIL1State *s)
{
    return l1_is_ibrick(s) && s->profile->identity == 0x4e;
}

static bool l1_is_vbrick(const SGIL1State *s)
{
    return s->profile && s->profile->o3k == L1_O3K_VBRICK;
}

/* A brick whose L1 is reached from the one C-brick its XIO cable joins. */
static bool l1_is_iobrick(const SGIL1State *s)
{
    return l1_is_ibrick(s) || l1_is_vbrick(s);
}

static bool l1_is_cxbrick(const SGIL1State *s)
{
    return s->profile && s->profile->o3k == L1_O3K_CXBRICK;
}

/*
 * The board link a frame leaves on: the one whose request is being
 * answered, otherwise the console board's. Every other profile has one.
 */
static unsigned l1_link(const SGIL1State *s)
{
    if (!l1_is_cxbrick(s)) {
        return 0;
    }
    return s->cx_serving ? s->cx_link : s->cx_console_board;
}

/*
 * The slot a Cx-brick board link reports. The stock PROM refuses two modules
 * at one rack and slot, so each board answers with the brick's slot plus its
 * board number; no source states how a real Cx-brick numbers its boards. The
 * one lead is a newsgroup post naming processors 1.9.c and 1.17.a of a
 * 64-processor Origin 3900, slots that fit boards numbered from their
 * brick's slot, which does not settle the order (an approximation).
 */
static unsigned l1_cx_board_slot(const SGIL1State *s)
{
    return s->bay + (s->cx_serving ? s->cx_link : 0);
}

static bool l1_cx_compute_board(const SGIL1State *s, unsigned board)
{
    return board < SGI_L1_CX_BOARDS && (s->cx_compute & (1u << board));
}

static bool l1_cx_fitted_board(const SGIL1State *s, unsigned board)
{
    return board < SGI_L1_CX_BOARDS && (s->cx_boards & (1u << board));
}

/* Each board link is one of the UART windows the transports already index. */
QEMU_BUILD_BUG_ON(SGI_L1_CX_BOARDS > SGI_L1_IROUTER_TRANSPORTS);

/* Rack and slot are both set: only a C-brick or I-brick can lack them. */
static bool l1_named(const SGIL1State *s)
{
    return s->rack_valid && s->bay_valid;
}

/*
 * The name the operator sees: rack, type letter and slot, with dashes for
 * what is unset ("---c--" is the factory state).
 */
static char *l1_module_name(const SGIL1State *s)
{
    char rack[8], bay[8];

    if (s->rack_valid) {
        snprintf(rack, sizeof(rack), "%03u", s->rack);
    } else {
        g_strlcpy(rack, "---", sizeof(rack));
    }
    if (s->bay_valid) {
        snprintf(bay, sizeof(bay), "%02u", s->bay);
    } else {
        g_strlcpy(bay, "--", sizeof(bay));
    }
    return g_strdup_printf("%s%c%s", rack,
                           l1_is_nbrick(s) ? 'n' : l1_is_ibrick(s) ? 'i' :
                           l1_is_vbrick(s) ? 'x' : 'c', bay);
}

static bool l1_restarting(const SGIL1State *s)
{
    return s->restart_until &&
           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->restart_until;
}

/*
 * Every controller that exists, so a cable can be found from either end
 * without walking the composition tree. A controller outside the tree is
 * found too. Touched under the BQL only.
 */
static GSList *l1_controllers;

/*
 * The C-brick that names @ibrick as its I-brick, ignoring @except. An
 * I-brick holds no pointer back, so the cable is found from the C-brick's
 * side and the two ends cannot disagree.
 */
static SGIL1State *l1_host_of(const SGIL1State *ibrick,
                              const SGIL1State *except)
{
    for (GSList *l = l1_controllers; l; l = l->next) {
        SGIL1State *candidate = l->data;

        if (candidate != except && candidate->io_peer == ibrick) {
            return candidate;
        }
    }
    return NULL;
}

typedef struct SGIL1ConfigRow {
    unsigned index;
    SGIL1State *brick;
} SGIL1ConfigRow;

/*
 * This brick at index 0 and each cabled brick at its port number, in port
 * order. Returns the number of rows, at most three.
 */
static unsigned l1_config_rows(SGIL1State *s, SGIL1ConfigRow rows[3])
{
    unsigned count = 1;

    rows[0] = (SGIL1ConfigRow) { 0, s };
    if (l1_is_iobrick(s)) {
        /* The first of an I-brick's C-brick ports. */
        SGIL1State *host = l1_is_nbrick(s) ? NULL : l1_host_of(s, NULL);

        if (host) {
            rows[count++] = (SGIL1ConfigRow) { 1, host };
        }
    } else {
        SGIL1State *c_brick = l1_linked_peer(s);

        if (s->io_peer) {
            rows[count++] = (SGIL1ConfigRow) { s->io_port, s->io_peer };
        }
        if (c_brick) {
            rows[count++] = (SGIL1ConfigRow) { s->c_port, c_brick };
        }
    }
    if (count == 3 && rows[1].index > rows[2].index) {
        SGIL1ConfigRow swap = rows[1];

        rows[1] = rows[2];
        rows[2] = swap;
    }
    return count;
}

/*
 * The partition ID this controller reports. PROVISIONAL: an I/O brick takes
 * its C-brick's ID and refuses a set, which is one reading of the L1 sheet's
 * brick command ("it is acquired from attached C-brick"). The partitioning
 * datasheet and the L1 datasheet conflict on whether an I/O brick holds a
 * partition of its own, and a separate research item will settle it. An
 * N-brick serves several C-bricks, which can hold different IDs, so it keeps
 * its own value (a model choice).
 */
static uint32_t l1_partition(const SGIL1State *s)
{
    if (l1_is_iobrick(s) && !l1_is_nbrick(s)) {
        SGIL1State *host = s->reply_to ? s->reply_to : l1_host_of(s, NULL);

        if (host) {
            return host->partition;
        }
    }
    return s->partition;
}

/* The Origin 3000 R-brick's L1: the router profile on the R-brick board. */
static bool l1_is_rbrick(const SGIL1State *s)
{
    return s->profile && !strcmp(s->profile->name, "router") &&
           s->router_board && !strcmp(s->router_board, "rbrick");
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
    g_autofree char *name = l1_module_name(s);

    l1_printf(s, "%s-L1> ", name);
}

static char *l1_designation(SGIL1State *s)
{
    static const char *const names[] = {
        "CPU0", "CPU1", "CPU2", "CPU3", "console"
    };
    unsigned subchannel = s->port_field & 0x1f;

    if (l1_is_cxbrick(s)) {
        /* The Origin 3900 names: 0A to 3D, and console0 to console3. */
        if (s->cx_console_subchannel == 4) {
            return g_strdup_printf("console%u", s->cx_console_board);
        }
        return g_strdup_printf("%u%c", s->cx_console_board,
                               'A' + s->cx_console_subchannel);
    }
    return g_strdup(names[subchannel <= 4 ? subchannel : 4]);
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
    g_autofree char *name = l1_module_name(s);
    g_autofree char *designation = l1_designation(s);

    if (l1_is_cxbrick(s) && !l1_cx_compute_board(s, s->cx_console_board)) {
        /*
         * A board with no processors answers nothing. The two lines are the
         * Origin 3900 guide's for a console and a processor target; using
         * them here, and staying in L1 mode, is the model's choice.
         */
        s->console_mode = false;
        s->escaped = false;
        if (s->cx_console_subchannel == 4) {
            l1_printf(s, "no response from %s junk bus console "
                      "UART:UART_TIMEOUT\r\n", name);
        } else {
            l1_printf(s, "no response from %s Junk bus CPU%s system not "
                      "responding\r\n", name, designation);
        }
        l1_prompt(s);
        return;
    }
    s->console_mode = true;
    s->escaped = false;
    s->warned_off = false;
    l1_printf(s, "%s console mode  %s %s, <CTRL_T> to escape to L1"
              "\r\n", returning ? "returning to" : "entering", name,
              designation);
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
    g_autofree char *name = l1_module_name(s);
    g_autofree char *id = g_strdup_printf("%s     /", name);

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

/*
 * The 0x8000 event, sent by the deferred first power-off request. The
 * firmware reading in the L1 datasheet (established) defers only for the
 * type letters 9, A and Q, and only with an event subchannel registered.
 */
static void l1_send_event(SGIL1State *s, uint32_t code, uint32_t number,
                          const char *text)
{
    uint8_t payload[127];
    g_autofree char *name = l1_module_name(s);
    g_autofree char *line = g_strdup_printf("\r\n%s ATTN: %s\r\n", name,
                                            text);
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

static bool l1_defers_power_down(SGIL1State *s)
{
    uint32_t letter = l1_brick_type_character(s);

    return s->events_enabled &&
           (letter == '9' || letter == 'A' || letter == 'Q');
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
    if (reason == 1 && !s->power_down_pending && !s->capture &&
        l1_defers_power_down(s)) {
        s->power_down_pending = true;
        l1_send_event(s, 0x8000, 0, "Power Down: issue 'pwr d' again to "
                      "immediately power down.");
        return;
    }
    s->power_down_pending = false;
    l1_reason(s, reason);
    s->node_power = false;
    /* The power-down clears the registered event subchannel (established). */
    s->events_enabled = false;
    s->event_subchannel = 0;
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
    /*
     * The selection returns to CPU 0 at every power-up. A Cx-brick applies
     * that to each board link's field; its brick-wide console selection
     * stays where it was.
     */
    s->port_field &= 0x60;
    for (unsigned i = 0; i < SGI_L1_CX_BOARDS; i++) {
        s->cx_port_field[i] &= 0x60;
    }
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
    s->rack_valid = s->pending_rack_valid;
    s->bay_valid = s->pending_bay_valid;
    s->partition = s->pending_partition;
    s->console_mode = s->escaped = s->warned_off = false;
    s->command_len = 0;
    memset(s->frame, 0, sizeof(s->frame));
    s->port_field = s->node_position << 5;
    /*
     * A Cx-brick's link positions are its board numbers, and its console
     * starts on board 0's console subchannel, or on the lowest board with
     * processors when board 0 has none (a model choice).
     */
    for (unsigned i = 0; i < SGI_L1_CX_BOARDS; i++) {
        s->cx_port_field[i] = i << 5;
    }
    s->cx_console_board = 0;
    if (l1_is_cxbrick(s) && !l1_cx_compute_board(s, 0)) {
        for (unsigned i = 1; i < SGI_L1_CX_BOARDS; i++) {
            if (l1_cx_compute_board(s, i)) {
                s->cx_console_board = i;
                break;
            }
        }
    }
    s->cx_console_subchannel = 4;
    s->cx_filter = false;
    /*
     * Monitoring, temperature checking, altitude and the power-monitoring
     * mode are numbered settings and survive a controller start; the rest
     * are held in RAM and are lost (inferred from the datasheet).
     */
    if (s->env.fan_auto_off || s->env.faults_only || s->env.fans_off ||
        s->env.custom_limits || s->env.fan_override_mask) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: a controller start drops the "
                      "RAM-only env settings; which settings persist is "
                      "inferred\n");
    }
    s->env = (SGIL1EnvSettings) {
        .disabled = s->env.disabled,
        .temp_off = s->env.temp_off,
        .power_mode = s->env.power_mode,
        .altitude_high = s->env.altitude_high,
    };
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
    l1_emit_transport(s, l1_link(s), payload, length);
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

/* The chunk of a present record that a read at @offset asks for. */
static void l1_eeprom_reply(SGIL1State *recipient, uint8_t channel,
                            const SGIL1EEPROM *record, uint32_t offset,
                            uint32_t requested)
{
    uint8_t fields[122];
    unsigned length;

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

static void l1_eeprom(SGIL1State *s, SGIL1State *recipient, uint8_t channel,
                      const uint8_t *args)
{
    uint32_t component = ldl_be_p(args + 1);
    uint32_t area = ldl_be_p(args + 6);
    uint32_t offset = ldl_be_p(args + 11);
    uint32_t requested = ldl_be_p(args + 16);
    unsigned index;
    SGIL1EEPROM *record;

    trace_sgi_l1_eeprom(s->profile->name, component, area, offset, requested);
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
    l1_eeprom_reply(recipient, channel, record, offset, requested);
}

static void l1_o3k_dimm(SGIL1EEPROM *record, unsigned board, unsigned dimm,
                        uint8_t density);

/*
 * The Cx-brick's EEPROM components, as the firmware's personality for it
 * numbers them (router board records datasheet, "Personality 6"): 0 the
 * power inlet, 1 the router board, then for each node board a board record
 * followed by its eight DIMM SPDs in the order 0, 2, 4, 6, 1, 3, 5, 7. Board
 * n's record is component 2 + 9 * n. Thirty-eight entries.
 *
 * The firmware numbers a request by the port it arrived on (the datasheet's
 * "Component numbers by requesting port"; the port to board mapping is
 * inferred). A node board's link n asks for component c and gets table entry:
 *   c = 0     2 + 9n, its own node record
 *   c = 1     3 + 9n, its own DIMM 0 (not the router)
 *   c >= 2    c + 9n, so 3 to 10 are its own DIMMs and larger numbers reach
 *             other boards' entries; 38 and up are absent
 * The router's own port is the router-window L1, whose router-profile
 * records answer component 1 and nothing else. This L1 has no router port,
 * so the router record and the power inlet (reachable only from that port)
 * are not served here.
 *
 * Model choices, all approximations:
 *  - The port to board mapping (link n is node board n) is inferred.
 *  - The power inlet is absent: its record contents are unknown, and the
 *    PROM never asks for it.
 *  - SPDs exist only for the DIMMs the catalogue fits: 2048 MiB per node as
 *    two 1 GiB DIMMs in slots 0 and 1, the only fit the Origin 3900 offers.
 *    The population of a real Cx-brick is unknown. An SPD has a valid
 *    checksum, byte 13 zero (the PROM's memory self-test decides the width),
 *    and byte 31 the 1 GiB rank density the PROM takes only if its memory
 *    probe finds nothing. All other type and timing bytes stay zero; the
 *    PROM does not read them.
 *
 * This does not go through l1_eeprom(): the selected-record overlay and the
 * invalid-area answer (minus 107) are unreachable here, since the Origin
 * 3000 profiles take no record files and mark no area invalid.
 */
#define L1_CX_NODE_STRIDE 9
#define L1_CX_COMPONENTS (2 + SGI_L1_CX_BOARDS * L1_CX_NODE_STRIDE)
/* The DIMM slots the catalogue's memory fit populates, and their density. */
#define L1_CX_FITTED_DIMMS 0x03
#define L1_CX_SPD_DENSITY_1GB 0x01

static void l1_cx_eeprom(SGIL1State *s, SGIL1State *recipient,
                         uint8_t channel, const uint8_t *args)
{
    static const uint8_t dimm_order[8] = { 0, 2, 4, 6, 1, 3, 5, 7 };
    uint32_t asked = ldl_be_p(args + 1);
    uint32_t component = asked;
    uint32_t area = ldl_be_p(args + 6);
    uint32_t offset = ldl_be_p(args + 11);
    uint32_t requested = ldl_be_p(args + 16);
    SGIL1EEPROM spd = { 0 };
    const SGIL1EEPROM *record;
    unsigned slot, board, dimm;

    if (asked >= L1_CX_COMPONENTS) {
        /* Absent on any link; the offset below cannot wrap. */
        trace_sgi_l1_cx_eeprom(asked, asked);
        l1_reply_direct(recipient, channel, -104, 0, NULL, 0);
        return;
    }
    if (component < 2) {
        component = (component ? 3 : 2) + L1_CX_NODE_STRIDE * s->cx_link;
    } else {
        component += L1_CX_NODE_STRIDE * s->cx_link;
    }
    trace_sgi_l1_cx_eeprom(asked, component);
    trace_sgi_l1_eeprom(s->profile->name, component, area, offset, requested);
    if (args[0] || args[5] || args[10] || args[15]) {
        l1_reply_direct(recipient, channel, -100, 0, NULL, 0);
        return;
    }
    board = (component - 2) / L1_CX_NODE_STRIDE;
    slot = (component - 2) % L1_CX_NODE_STRIDE;
    dimm = slot ? dimm_order[slot - 1] : 0;
    if (component >= L1_CX_COMPONENTS || !l1_cx_fitted_board(s, board) ||
        (slot && !(L1_CX_FITTED_DIMMS & (1u << dimm)))) {
        l1_reply_direct(recipient, channel, -104, 0, NULL, 0);
        return;
    }
    if (area > 4) {
        l1_reply_direct(recipient, channel, -105, 0, NULL, 0);
        return;
    }
    if (!slot) {
        /* A node board record: areas 1 and 2 only. */
        if (area != 1 && area != 2) {
            l1_reply_direct(recipient, channel, -104, 0, NULL, 0);
            return;
        }
        record = &s->records[(area == 1 ? 0 : 16) + board];
    } else {
        /* A DIMM: an SPD, area 4 only, made when it is asked for. */
        if (area != 4) {
            l1_reply_direct(recipient, channel, -104, 0, NULL, 0);
            return;
        }
        l1_o3k_dimm(&spd, board, dimm, L1_CX_SPD_DENSITY_1GB);
        record = &spd;
    }
    l1_eeprom_reply(recipient, channel, record, offset, requested);
    g_free(spd.data);
}

/* A destination nobody serves gets the router's own error reply. */
static void l1_router_error_to(SGIL1State *recipient, uint8_t channel)
{
    uint8_t payload[] = { 0x20 | channel, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 8 };

    l1_emit_direct(recipient, payload, sizeof(payload));
}

static void l1_router_error(SGIL1State *s, uint8_t channel)
{
    l1_router_error_to(s->reply_to ? s->reply_to : s, channel);
}

/*
 * The optional link to an L2 (docs/system/mips/sgi/hardware/l1.rst, "L2
 * link"). While a peer holds the link, a request for controller type 1 or 2
 * leaves as router segments and the peer's response comes back to whoever
 * asked. With no peer, such a request gets the router error reply.
 */

/* The tag in the source word's bits 11:5 names the requester again. */
#define L1_L2_TASK 0x15
#define L1_L2_PENDING_NONE  0
#define L1_L2_PENDING_PLAIN 1
#define L1_L2_PENDING_CHUNK 2
#define L1_L2_CHAIN_NONE 0xff

static bool l1_l2_linked(const SGIL1State *s)
{
    return s->l2_up;
}

/*
 * What the L1 puts in a forwarded request's source word: the controller's
 * own key, the requester's transport and subchannel packed into bits 11:5
 * as the chunk-transport rows of the L1 sheet pack them, and the return
 * task. The key an unnamed controller uses is the local one: an
 * approximation, the real value is not established.
 */
static uint32_t l1_l2_source(const SGIL1State *s, unsigned tag)
{
    uint32_t key = l1_named(s) ? ((s->rack << 6) | s->bay) : 0xffff;

    return (key << 12) | (tag << 5) | L1_L2_TASK;
}

/* The rack and bay a trace names the controller by; all ones when unset. */
static void l1_trace_name(const SGIL1State *s, unsigned *rack, unsigned *bay)
{
    *rack = l1_named(s) ? s->rack : 0xffff;
    *bay = l1_named(s) ? s->bay : 0xffff;
}

static char *l1_hex_string(const uint8_t *data, unsigned length)
{
    static const char digits[] = "0123456789abcdef";
    char *hex = g_malloc(length * 2 + 1);

    for (unsigned i = 0; i < length; i++) {
        hex[i * 2] = digits[data[i] >> 4];
        hex[i * 2 + 1] = digits[data[i] & 0xf];
    }
    hex[length * 2] = 0;
    return hex;
}

/* A chunk-transport frame: channel byte, result word, code, count, length. */
static void l1_trace_chunk(const SGIL1State *s, bool to_node,
                           unsigned transport, const uint8_t *frame,
                           unsigned length)
{
    g_autofree char *hex = NULL;
    unsigned rack, bay;

    if (!trace_event_get_state_backends(TRACE_SGI_L1_CHUNK) || length < 10) {
        return;
    }
    hex = l1_hex_string(frame + 10, length - 10);
    l1_trace_name(s, &rack, &bay);
    trace_sgi_l1_chunk(rack, bay, to_node, transport,
                       frame[0] & 0x1f, lduw_be_p(frame + 5), frame[7],
                       lduw_be_p(frame + 8), length - 10, hex);
}

static void l1_l2_trace_segment(const SGIL1State *s, bool to_l2,
                                const uint8_t *segment, unsigned length)
{
    static const char digits[] = "0123456789abcdef";
    g_autofree char *hex = NULL;
    unsigned rack, bay;

    if (!trace_event_get_state_backends(TRACE_SGI_L1_L2_SEGMENT)) {
        return;
    }
    hex = g_malloc(length * 2 + 1);
    for (unsigned i = 0; i < length; i++) {
        hex[i * 2] = digits[segment[i] >> 4];
        hex[i * 2 + 1] = digits[segment[i] & 0xf];
    }
    hex[length * 2] = 0;
    l1_trace_name(s, &rack, &bay);
    trace_sgi_l1_l2_segment(s->profile->name, rack, bay, to_l2, length, hex);
}

static void l1_l2_send(SGIL1State *s, const uint8_t *segment, unsigned length)
{
    uint8_t message[1 + SGI_L1_L2_SEGMENT_MAX];

    g_assert(length >= SGI_L1_L2_SEGMENT_MIN &&
             length <= SGI_L1_L2_SEGMENT_MAX);
    l1_l2_trace_segment(s, true, segment, length);
    message[0] = SGI_L1_L2_KIND_SEGMENT;
    memcpy(message + 1, segment, length);
    qemu_chr_fe_write_all(&s->l2_chr, message, length + 1);
}

static void l1_l2_router_error(SGIL1State *s, unsigned transport,
                               uint8_t channel)
{
    uint8_t payload[] = { 0x20 | channel, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 8 };

    l1_emit_transport(s, transport, payload, sizeof(payload));
}

static bool l1_l2_destination(uint32_t address)
{
    return (address >> 28) == 1 || (address >> 28) == 2;
}

/*
 * A request from the node that arrived as chunk frames and was assembled
 * into one inner message, which is already a router segment. Returns false
 * when the message is not for the link.
 */
static bool l1_l2_forward_chunked(SGIL1State *s, unsigned transport,
                                  uint8_t channel, const uint8_t *message,
                                  unsigned length)
{
    uint8_t segment[SGI_L1_L2_SEGMENT_MAX];
    unsigned tag = transport * SGI_L1_IROUTER_CHANNELS + channel;

    if (!l1_l2_linked(s) || length < SGI_L1_L2_SEG_DESC ||
        !l1_l2_destination(ldl_be_p(message + SGI_L1_L2_SEG_DEST))) {
        return false;
    }
    if (length > sizeof(segment) ||
        lduw_be_p(message + SGI_L1_L2_SEG_LENGTH) != length ||
        message[SGI_L1_L2_SEG_COUNT] != (SGI_L1_L2_FIRST_SEGMENT | 1)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: chunked request for the L2 is not one segment\n");
        return true;
    }
    memcpy(segment, message, length);
    stl_be_p(segment + SGI_L1_L2_SEG_SOURCE, l1_l2_source(s, tag));
    s->l2_pending[tag] = L1_L2_PENDING_CHUNK;
    l1_l2_send(s, segment, length);
    return true;
}

/*
 * A plain request from the node for the L2. Only integer arguments have a
 * known descriptor form, so a request with any other argument keeps the
 * router error reply. Returns true when the request left on the link.
 */
static bool l1_l2_forward_plain(SGIL1State *s, uint8_t channel,
                                uint32_t address, uint16_t command,
                                const uint8_t *args, unsigned args_len,
                                unsigned count, unsigned transport)
{
    uint8_t segment[SGI_L1_L2_SEGMENT_MAX] = { 0 };
    unsigned tag = transport * SGI_L1_IROUTER_CHANNELS + channel;
    unsigned length = SGI_L1_L2_SEG_DESC + 8 * count;

    if (!l1_l2_linked(s) || !l1_arguments(args, args_len, count) ||
        length > sizeof(segment)) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        if (args[5 * i]) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: request for the L2 has a non-integer "
                          "argument\n");
            return false;
        }
    }
    stw_be_p(segment + SGI_L1_L2_SEG_LENGTH, length);
    segment[SGI_L1_L2_SEG_FORMAT] = 2;
    segment[SGI_L1_L2_SEG_COUNT] = SGI_L1_L2_FIRST_SEGMENT | 1;
    segment[SGI_L1_L2_SEG_CLASS] = SGI_L1_L2_CLASS_REQUEST;
    stl_be_p(segment + SGI_L1_L2_SEG_DEST, address);
    stl_be_p(segment + SGI_L1_L2_SEG_SOURCE, l1_l2_source(s, tag));
    segment[SGI_L1_L2_SEG_NDESC] = count;
    stw_be_p(segment + SGI_L1_L2_SEG_CODE, command);
    for (unsigned i = 0; i < count; i++) {
        /* A scalar descriptor: type 0, segment byte 0, length 0, value. */
        memcpy(segment + SGI_L1_L2_SEG_DESC + 8 * i + 4, args + 5 * i + 1, 4);
    }
    s->l2_pending[tag] = L1_L2_PENDING_PLAIN;
    l1_l2_send(s, segment, length);
    return true;
}

/*
 * Frames of at most 113 inner bytes carry a segment to the node. The first
 * frame of each segment is 0x3001 and carries the segment's count byte (low
 * seven bits) and this frame's payload length; each later frame is 0x3003
 * while the segment has more, and 0x3002 on the frame that exhausts it. The
 * stock firmware's reader takes a single-frame segment and a three-frame one
 * under this rule, and refuses a 0x3001 frame whose length field is the
 * segment's length when that exceeds the frame's payload. Whether it reads a
 * second segment, and what a later frame's length field holds, is not
 * established: the model leaves it zero.
 */
static void l1_l2_chunk_reply(SGIL1State *s, unsigned transport,
                              uint8_t channel, const uint8_t *segment,
                              unsigned length)
{
    for (unsigned offset = 0; offset < length; ) {
        uint8_t chunk[128] = { 0 };
        unsigned bytes = MIN(113, length - offset);
        bool last = offset + bytes == length;
        uint16_t code = last ? 0x3002 : 0x3003;

        if (!offset) {
            code = 0x3001;
            chunk[7] = segment[SGI_L1_L2_SEG_COUNT] & 0x7f;
            stw_be_p(chunk + 8, bytes);
        }
        chunk[0] = 0x20 | channel;
        stw_be_p(chunk + 5, code);
        memcpy(chunk + 10, segment + offset, bytes);
        l1_trace_chunk(s, true, transport, chunk, 10 + bytes);
        l1_emit_transport(s, transport, chunk, 10 + bytes);
        offset += bytes;
    }
}

/* A response to a plain request: the first descriptor is the result. */
static void l1_l2_plain_reply(SGIL1State *s, unsigned transport,
                              uint8_t channel, const uint8_t *segment,
                              unsigned length)
{
    unsigned count;
    uint8_t payload[128];
    int32_t result;

    /* The descriptor count is past the shortest segment the link carries. */
    count = length >= SGI_L1_L2_SEG_DESC ? segment[SGI_L1_L2_SEG_NDESC] : 0;
    if (segment[SGI_L1_L2_SEG_COUNT] != (SGI_L1_L2_FIRST_SEGMENT | 1) ||
        !count || count > 24 ||
        length < SGI_L1_L2_SEG_DESC + 8 * count) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: reply from the L2 does not fit a plain "
                      "reply\n");
        l1_l2_router_error(s, transport, channel);
        return;
    }
    for (unsigned i = 0; i < count; i++) {
        if (segment[SGI_L1_L2_SEG_DESC + 8 * i]) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: reply from the L2 has a non-integer "
                          "descriptor\n");
            l1_l2_router_error(s, transport, channel);
            return;
        }
    }
    result = ldl_be_p(segment + SGI_L1_L2_SEG_DESC + 4);
    payload[0] = 0x20 | channel;
    stl_be_p(payload + 1, result);
    payload[5] = count - 1;
    for (unsigned i = 1; i < count; i++) {
        payload[6 + 5 * (i - 1)] = 0;
        memcpy(payload + 7 + 5 * (i - 1),
               segment + SGI_L1_L2_SEG_DESC + 8 * i + 4, 4);
    }
    l1_emit_transport(s, transport, payload, 6 + 5 * (count - 1));
}

/*
 * The L2 daemon's recurring configuration request (L2 sheet, "The daemon's
 * recurring request" and "The reply, and the route it creates"; L1 sheet,
 * "Request 0x0ffff00e"). An L1 becomes routable at the L2 only through the
 * reply to this request, which lists the bricks the L1 knows by rack and
 * bay. Grades follow the sheets and the sheets' source is one reading of one
 * L1 package, so none of this is called hardware-accurate:
 *
 * - The request is class 0, selector 6 in the first integer descriptor, the
 *   serial text or an integer 0 second, an integer third (established).
 * - The reply is class 1 to the request's source word, status 0, the
 *   request's code, with the descriptors integer 7, the third descriptor
 *   echoed, a record count, the record size 0x70, a byte array of table
 *   positions and the records (established). A nonzero third descriptor
 *   shortens the reply to the integer 7 (established).
 * - The reply's source word is (identity << 12) | 14 with the identity
 *   being rack in bits 15:6 and bay in 5:0; that those are this brick's
 *   settings is inferred.
 * - Before it builds the reply the L1 sends two requests of its own,
 *   selectors 2 and 13, to the same destination (established); what they
 *   carry beyond the selector, and their consumer, are unknown. One integer
 *   descriptor each is this model's choice.
 * - A record's words at +0x00 (valid), +0x08 (rack) and +0x0c (bay) are the
 *   fields the L2 routes by (established). The word at +0x60 must be nonzero
 *   for the L1 to send the record (established); its value and the rest of
 *   the record are unknown, so they are zero here except +0x60, which is 1,
 *   and the text at +0x64, where the L1 keeps the serial the request
 *   carries (the text's role is inferred). The model's table is the bricks
 *   it lists for "config", which is an approximation: what fills the real
 *   table is unknown.
 * - The type of a binary descriptor is not established by the sheets.
 */
#define L1_L2_RECORD_VALID  0x00
#define L1_L2_RECORD_RACK   0x08
#define L1_L2_RECORD_BAY    0x0c
#define L1_L2_RECORD_PRESENT 0x60
#define L1_L2_RECORD_TEXT   0x64
#define L1_L2_RECORD_TEXT_MAX (SGI_L1_L2_RECORD_SIZE - L1_L2_RECORD_TEXT)

static void l1_l2_put_integer(uint8_t *segment, unsigned i, uint32_t value)
{
    stl_be_p(segment + SGI_L1_L2_SEG_DESC + 8 * i + 4, value);
}

/* A descriptor for bytes at @offset in the segment, counted as the L2 does. */
static void l1_l2_put_data(uint8_t *segment, unsigned i, uint8_t type,
                           unsigned data_length, unsigned offset)
{
    uint8_t *desc = segment + SGI_L1_L2_SEG_DESC + 8 * i;

    desc[0] = type;
    stw_be_p(desc + 2, data_length);
    stl_be_p(desc + 4, offset + SGI_L1_L2_DESC_PREFIX);
}

static void l1_l2_header(uint8_t *segment, unsigned length, uint8_t class,
                         uint32_t dest, uint32_t source, unsigned ndesc,
                         const uint8_t *seq_from, uint16_t code)
{
    stw_be_p(segment + SGI_L1_L2_SEG_LENGTH, length);
    segment[SGI_L1_L2_SEG_FORMAT] = 2;
    segment[SGI_L1_L2_SEG_COUNT] = SGI_L1_L2_FIRST_SEGMENT | 1;
    segment[SGI_L1_L2_SEG_CLASS] = class;
    stl_be_p(segment + SGI_L1_L2_SEG_DEST, dest);
    stl_be_p(segment + SGI_L1_L2_SEG_SOURCE, source);
    segment[SGI_L1_L2_SEG_NDESC] = ndesc;
    stw_be_p(segment + SGI_L1_L2_SEG_CODE, code);
    if (seq_from) {
        segment[SGI_L1_L2_SEG_SEQ_LOW] = seq_from[SGI_L1_L2_SEG_SEQ_LOW];
        segment[SGI_L1_L2_SEG_SEQ_HIGH] = seq_from[SGI_L1_L2_SEG_SEQ_HIGH];
    }
}

/* The serial text of the request's second descriptor, or NULL for none. */
static const char *l1_l2_config_text(const uint8_t *request, unsigned length)
{
    const uint8_t *desc = request + SGI_L1_L2_SEG_DESC + 8;
    unsigned text_length, offset;

    if (desc[0] != SGI_L1_L2_DESC_STRING) {
        return NULL;
    }
    text_length = lduw_be_p(desc + 2);
    offset = ldl_be_p(desc + 4);
    if (!text_length || offset < SGI_L1_L2_DESC_PREFIX ||
        offset - SGI_L1_L2_DESC_PREFIX > length ||
        text_length > length - (offset - SGI_L1_L2_DESC_PREFIX) ||
        request[offset - SGI_L1_L2_DESC_PREFIX + text_length - 1]) {
        return NULL;
    }
    return (const char *)request + offset - SGI_L1_L2_DESC_PREFIX;
}

static void l1_l2_config_request(SGIL1State *s, const uint8_t *request,
                                 unsigned length)
{
    SGIL1ConfigRow rows[3];
    uint8_t segment[SGI_L1_L2_SEGMENT_MAX] = { 0 };
    uint8_t index[3];
    unsigned ndesc = request[SGI_L1_L2_SEG_NDESC];
    uint32_t identity, source, dest, third;
    const char *text;
    unsigned count, records = 0, reply_length;
    static const uint8_t selectors[] = {
        SGI_L1_L2_CONFIG_SELECTOR_A, SGI_L1_L2_CONFIG_SELECTOR_B
    };

    if (!l1_named(s)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: configuration request for an unnamed "
                      "controller is not answered\n");
        return;
    }
    if (ndesc != 3 || length < SGI_L1_L2_SEG_DESC + 8 * ndesc ||
        request[SGI_L1_L2_SEG_DESC] != SGI_L1_L2_DESC_INTEGER ||
        ldl_be_p(request + SGI_L1_L2_SEG_DESC + 4) !=
            SGI_L1_L2_CONFIG_SELECTOR ||
        request[SGI_L1_L2_SEG_DESC + 16] != SGI_L1_L2_DESC_INTEGER) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: configuration request is not selector 6 with "
                      "three descriptors\n");
        return;
    }
    third = ldl_be_p(request + SGI_L1_L2_SEG_DESC + 16 + 4);
    text = l1_l2_config_text(request, length);
    identity = ((s->rack & 0x3ff) << 6) | (s->bay & 0x3f);
    source = (identity << 12) | SGI_L1_L2_CONFIG_TASK;
    dest = ldl_be_p(request + SGI_L1_L2_SEG_SOURCE);

    /* The L1's own requests come first on the link. */
    for (unsigned i = 0; i < ARRAY_SIZE(selectors); i++) {
        memset(segment, 0, sizeof(segment));
        l1_l2_header(segment, SGI_L1_L2_SEG_DESC + 8,
                     SGI_L1_L2_CLASS_REQUEST, SGI_L1_L2_CONFIG_DEST, source,
                     1, NULL, 0);
        l1_l2_put_integer(segment, 0, selectors[i]);
        l1_l2_send(s, segment, SGI_L1_L2_SEG_DESC + 8);
    }

    memset(segment, 0, sizeof(segment));
    if (third) {
        reply_length = SGI_L1_L2_SEG_DESC + 8;
        l1_l2_header(segment, reply_length, SGI_L1_L2_CLASS_RESPONSE, dest,
                     source, 1, request, 0);
        l1_l2_put_integer(segment, 0, SGI_L1_L2_CONFIG_REPLY_KIND);
        l1_l2_send(s, segment, reply_length);
        return;
    }

    count = l1_config_rows(s, rows);
    for (unsigned i = 0; i < count; i++) {
        if (l1_named(rows[i].brick)) {
            index[records++] = i;
        }
    }
    reply_length = SGI_L1_L2_SEG_DESC + 6 * 8 + records +
                   records * SGI_L1_L2_RECORD_SIZE;
    l1_l2_header(segment, reply_length, SGI_L1_L2_CLASS_RESPONSE, dest,
                 source, 6, request, 0);
    l1_l2_put_integer(segment, 0, SGI_L1_L2_CONFIG_REPLY_KIND);
    l1_l2_put_integer(segment, 1, third);
    l1_l2_put_integer(segment, 2, records);
    l1_l2_put_integer(segment, 3, SGI_L1_L2_RECORD_SIZE);
    l1_l2_put_data(segment, 4, SGI_L1_L2_DESC_BLOB, records,
                   SGI_L1_L2_SEG_DESC + 48);
    l1_l2_put_data(segment, 5, SGI_L1_L2_DESC_BLOB,
                   records * SGI_L1_L2_RECORD_SIZE,
                   SGI_L1_L2_SEG_DESC + 48 + records);
    memcpy(segment + SGI_L1_L2_SEG_DESC + 48, index, records);
    for (unsigned i = 0; i < records; i++) {
        const SGIL1State *brick = rows[index[i]].brick;
        uint8_t *record = segment + SGI_L1_L2_SEG_DESC + 48 + records +
                          i * SGI_L1_L2_RECORD_SIZE;

        stl_be_p(record + L1_L2_RECORD_VALID, 1);
        stl_be_p(record + L1_L2_RECORD_RACK, brick->rack);
        stl_be_p(record + L1_L2_RECORD_BAY, brick->bay);
        stl_be_p(record + L1_L2_RECORD_PRESENT, 1);
        if (!i && text) {
            g_strlcpy((char *)record + L1_L2_RECORD_TEXT, text,
                      L1_L2_RECORD_TEXT_MAX);
        }
    }
    l1_l2_send(s, segment, reply_length);
}

static void l1_l2_segment(SGIL1State *s, const uint8_t *segment,
                          unsigned length)
{
    uint8_t count = segment[SGI_L1_L2_SEG_COUNT];
    bool first = count & SGI_L1_L2_FIRST_SEGMENT;
    unsigned tag;

    l1_l2_trace_segment(s, false, segment, length);
    if (first) {
        /* The destination lies beyond the shortest segment the link carries. */
        uint32_t dest = length >= SGI_L1_L2_SEG_DESC ?
                        ldl_be_p(segment + SGI_L1_L2_SEG_DEST) : 0;

        if (s->l2_chain_left) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-l1: L2 segment chain restarted\n");
        }
        s->l2_chain_tag = L1_L2_CHAIN_NONE;
        s->l2_chain_left = 0;
        if (length >= SGI_L1_L2_SEG_DESC &&
            segment[SGI_L1_L2_SEG_CLASS] == SGI_L1_L2_CLASS_REQUEST &&
            dest == SGI_L1_L2_CONFIG_DEST &&
            count == (SGI_L1_L2_FIRST_SEGMENT | 1)) {
            l1_l2_config_request(s, segment, length);
            return;
        }
        if (length < SGI_L1_L2_SEG_DESC ||
            segment[SGI_L1_L2_SEG_CLASS] != SGI_L1_L2_CLASS_RESPONSE) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: L2 sent class %u to 0x%08x, code 0x%04x; "
                          "only responses are served\n",
                          length > SGI_L1_L2_SEG_CLASS ?
                          segment[SGI_L1_L2_SEG_CLASS] : 0, dest,
                          length >= SGI_L1_L2_SEG_DESC ?
                          lduw_be_p(segment + SGI_L1_L2_SEG_CODE) : 0);
            s->l2_chain_left = (count & 0x7f) > 1 ? count & 0x7f : 0;
            return;
        }
        if ((dest & 0x1f) == SGI_L1_L2_CONFIG_TASK) {
            /* An answer to the L1's own selector 2 or 13 request. */
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: L2 answered a configuration request of "
                          "the controller; nothing consumes it\n");
            s->l2_chain_left = (count & 0x7f) > 1 ? count & 0x7f : 0;
            return;
        }
        if ((dest & 0x1f) != L1_L2_TASK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-l1: L2 response to 0x%08x names no requester\n",
                          dest);
            s->l2_chain_left = (count & 0x7f) > 1 ? count & 0x7f : 0;
            return;
        }
        s->l2_chain_tag = (dest >> 5) & 0x7f;
        s->l2_chain_first = true;
    } else {
        if (!s->l2_chain_left || (count & 0x7f) != s->l2_chain_left - 1) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-l1: L2 continuation segment out of order\n");
            s->l2_chain_left = 0;
            s->l2_chain_tag = L1_L2_CHAIN_NONE;
            return;
        }
    }
    s->l2_chain_left = (count & 0x7f) > 1 ? count & 0x7f : 0;
    tag = s->l2_chain_tag;
    if (tag == L1_L2_CHAIN_NONE) {
        return;
    }
    if (s->l2_pending[tag] == L1_L2_PENDING_NONE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-l1: L2 response with no request pending\n");
        return;
    }
    if (s->l2_pending[tag] == L1_L2_PENDING_CHUNK) {
        l1_l2_chunk_reply(s, tag / SGI_L1_IROUTER_CHANNELS,
                          tag % SGI_L1_IROUTER_CHANNELS, segment, length);
    } else if (s->l2_chain_first) {
        l1_l2_plain_reply(s, tag / SGI_L1_IROUTER_CHANNELS,
                          tag % SGI_L1_IROUTER_CHANNELS, segment, length);
    }
    s->l2_chain_first = false;
    if (!s->l2_chain_left) {
        s->l2_pending[tag] = L1_L2_PENDING_NONE;
        s->l2_chain_tag = L1_L2_CHAIN_NONE;
    }
}

static int l1_l2_can_receive(void *opaque)
{
    SGIL1State *s = opaque;

    return sizeof(s->l2_rx) - s->l2_rx_len;
}

/* Messages are a kind byte and a segment; the segment says its own length. */
static void l1_l2_receive(void *opaque, const uint8_t *buf, int size)
{
    SGIL1State *s = opaque;
    unsigned start = 0;

    memcpy(s->l2_rx + s->l2_rx_len, buf, size);
    s->l2_rx_len += size;
    while (s->l2_rx_len - start >= 1) {
        const uint8_t *message = s->l2_rx + start;
        unsigned available = s->l2_rx_len - start;
        unsigned length;

        if (message[0] != SGI_L1_L2_KIND_SEGMENT) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-l1: unknown L2 link message kind %u\n",
                          message[0]);
            start++;
            continue;
        }
        if (available < 3) {
            break;
        }
        length = lduw_be_p(message + 1);
        if (length < SGI_L1_L2_SEGMENT_MIN || length > SGI_L1_L2_SEGMENT_MAX) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-l1: L2 segment length %u is out of range\n",
                          length);
            start++;
            continue;
        }
        if (available < 1 + length) {
            break;
        }
        l1_l2_segment(s, message + 1, length);
        start += 1 + length;
    }
    memmove(s->l2_rx, s->l2_rx + start, s->l2_rx_len - start);
    s->l2_rx_len -= start;
}

static void l1_l2_event(void *opaque, QEMUChrEvent event)
{
    SGIL1State *s = opaque;

    if (event == CHR_EVENT_OPENED) {
        s->l2_up = true;
    } else if (event == CHR_EVENT_CLOSED) {
        s->l2_up = false;
        /* Whoever still waits for the L2 gets the no-route reply. */
        for (unsigned tag = 0; tag < ARRAY_SIZE(s->l2_pending); tag++) {
            if (s->l2_pending[tag] == L1_L2_PENDING_PLAIN) {
                l1_l2_router_error(s, tag / SGI_L1_IROUTER_CHANNELS,
                                   tag % SGI_L1_IROUTER_CHANNELS);
            }
        }
        memset(s->l2_pending, 0, sizeof(s->l2_pending));
        s->l2_rx_len = 0;
        s->l2_chain_left = 0;
        s->l2_chain_tag = L1_L2_CHAIN_NONE;
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

/*
 * Requests 0x000c and 0x000d on a C-brick: a pair of the packed rack and bay
 * and the type letter for each port, ports 1 to 4 or ports 5 and 10. The
 * port each cable appears at is configuration, because the guides disagree
 * on the numbering. An attached brick without a name appears as absent: what
 * a real controller returns for one is unknown.
 */
static void l1_o3k_attached_put(uint8_t *fields, unsigned slot,
                                uint32_t rack, uint32_t bay, uint8_t letter)
{
    fields[10 * slot] = 0;
    stl_be_p(fields + 10 * slot + 1, (rack << 6) | bay);
    fields[10 * slot + 5] = 0;
    stl_be_p(fields + 10 * slot + 6, letter);
}

/*
 * The same request on an I/O brick. The stock IP35 PROM sends it only from
 * its crossbar arbitration, which it enters only when the attached brick's
 * widget 0 reports a crossbow part (0xd000 or 0xd100). The I-brick's and
 * V-brick's widget 0 is an XBridge, so that PROM never asks either one, on
 * hardware or here; the reply is for other callers (see
 * docs/system/mips/sgi/hardware/l1.rst). What a real I/O brick answers is
 * not published (approximation): the model lists the requesting C-brick at
 * port 1 and, when that C-brick's chip has a graphics brick on its other
 * port, the graphics brick at port 2, each as the C-brick's reply lists a
 * brick, and leaves the other ports empty. An I-brick answers 0x000c so
 * with or without a graphics brick, and so does a V-brick: the L1 firmware
 * answers 0x000c from its link table whatever the brick's letter, and a
 * V-brick runs the X-brick personality. Without a graphics brick, 0x000d on
 * either, and both requests on an N-brick, keep the unknown-request code:
 * that PROM never asks an N-brick, and nothing settles the others.
 */
static void l1_iobrick_attached(SGIL1State *s, SGIL1State *recipient,
                                uint8_t channel, bool low_ports)
{
    SGIL1State *host = s->reply_to ? s->reply_to : l1_host_of(s, NULL);
    uint8_t fields[40] = { 0 };
    bool one_host = !l1_is_nbrick(s);

    if (!host || !l1_named(host) ||
        (!host->io_gfx_bay && (!one_host || !low_ports))) {
        /* No source says what an I/O brick answers. */
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: attached-brick request unavailable on the "
                      "%s profile\n", s->profile->name);
        l1_reply_direct(recipient, channel, -101, 0, NULL, 0);
        return;
    }
    if (low_ports) {
        l1_o3k_attached_put(fields, L1_IBRICK_HOST_PORT - 1,
                            host->rack, host->bay, 'C');
        if (host->io_gfx_bay) {
            l1_o3k_attached_put(fields, L1_IBRICK_GRAPHICS_PORT - 1,
                                host->rack, host->io_gfx_bay, 'G');
        }
    }
    l1_reply_direct(recipient, channel, 0, low_ports ? 8 : 4, fields,
                    low_ports ? 40 : 20);
}

static void l1_o3k_attached(SGIL1State *s, SGIL1State *recipient,
                            uint8_t channel, bool low_ports)
{
    static const unsigned low[] = { 1, 2, 3, 4 };
    static const unsigned high[] = { 5, 10 };
    const unsigned *ports = low_ports ? low : high;
    unsigned slots = low_ports ? ARRAY_SIZE(low) : ARRAY_SIZE(high);
    uint8_t fields[40] = { 0 };
    struct {
        SGIL1State *brick;
        unsigned port;
        uint8_t letter;
    } cables[] = {
        { s->io_peer, s->io_port, 'I' },
        { l1_linked_peer(s), s->c_port, 'C' },
    };

    if (l1_is_iobrick(s)) {
        l1_iobrick_attached(s, recipient, channel, low_ports);
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(cables); i++) {
        if (!cables[i].brick) {
            continue;
        }
        if (!l1_named(cables[i].brick)) {
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: attached brick has no rack and slot\n");
            continue;
        }
        for (unsigned slot = 0; slot < slots; slot++) {
            if (ports[slot] == cables[i].port) {
                l1_o3k_attached_put(fields, slot, cables[i].brick->rack,
                                    cables[i].brick->bay, cables[i].letter);
            }
        }
    }
    l1_reply_direct(recipient, channel, 0, 2 * slots, fields, 10 * slots);
}

static bool l1_read_query(SGIL1State *s, SGIL1State *recipient,
                          uint8_t channel, uint16_t command,
                          const uint8_t *args, unsigned args_len,
                          unsigned count)
{
    uint8_t fields[40] = { 0 };

    trace_sgi_l1_read_query(s->profile->name, command);
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
        if (!l1_named(s)) {
            /* The unnamed state's reply is unknown, so the guest gets none. */
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: identity requested of a brick with no "
                          "rack and slot\n");
            l1_router_error_to(recipient, channel);
            return true;
        }
        stl_be_p(fields + 1, (s->rack << 6) |
                 (l1_is_cxbrick(s) ? l1_cx_board_slot(s) : s->bay));
        stl_be_p(fields + 6, l1_brick_type_character(s));
        l1_reply_direct(recipient, channel, 0, 2, fields, 10);
        return true;
    case 4:
        if (l1_is_cxbrick(s) ||
            (s->router_board && !strcmp(s->router_board, "cxbrick"))) {
            /*
             * Letter 6 answers per port with the serial of the board record
             * the port names: NODE n on node link n, ROUTER on the router
             * port, which also serves as the brick's serial. These are the
             * synthetic serials the records carry.
             */
            char serial[SGI_SYNTHETIC_TEXT_MIN_WIDTH + 1];

            if (l1_is_cxbrick(s)) {
                sgi_synthetic_text_serial(serial,
                                          SGI_SYNTHETIC_TEXT_MIN_WIDTH, 'N',
                                          s->identity_index * 4 + s->cx_link);
            } else {
                sgi_synthetic_text_serial(serial,
                                          SGI_SYNTHETIC_TEXT_MIN_WIDTH, 'R',
                                          s->identity_index);
            }
            fields[0] = 1;
            pstrcpy((char *)fields + 1, sizeof(fields) - 1, serial);
            l1_reply_direct(recipient, channel, 0, 1, fields,
                            2 + strlen(serial));
        } else if (!s->brick_serial || !s->brick_serial[0]) {
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
        } else if (l1_is_cxbrick(s)) {
            l1_cx_eeprom(s, recipient, channel, args);
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
        if (s->profile->o3k) {
            l1_o3k_attached(s, recipient, channel, true);
            return true;
        }
        l1_reply_direct(recipient, channel, 0, 8, fields, sizeof(fields));
        return true;
    case 0xd:
        if (!s->profile->o3k) {
            return false;
        }
        l1_o3k_attached(s, recipient, channel, false);
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

/*
 * A Cx-brick's selection: "0A" to "3D", or "console" and a board number.
 * A board with no node fitted is refused. A board without processors is
 * accepted, since the guide lists all twenty names.
 */
static bool l1_cx_select(SGIL1State *s, const char *word)
{
    unsigned board, subchannel;

    if (g_str_has_prefix(word, "console") && strlen(word) == 8 &&
        g_ascii_isdigit(word[7])) {
        board = word[7] - '0';
        subchannel = 4;
    } else if (strlen(word) == 2 && g_ascii_isdigit(word[0]) &&
               g_ascii_toupper(word[1]) >= 'A' &&
               g_ascii_toupper(word[1]) <= 'D') {
        board = word[0] - '0';
        subchannel = g_ascii_toupper(word[1]) - 'A';
    } else {
        return false;
    }
    if (!l1_cx_fitted_board(s, board)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-l1: selection names empty board %u\n", board);
        return false;
    }
    s->cx_console_board = board;
    s->cx_console_subchannel = subchannel;
    return true;
}

/*
 * An Origin 3000 profile matches no particular L1 image: the revision it is
 * given is an approximation made for request 0x0005, so the line names no
 * image bank and no product label.
 */
static void l1_version(SGIL1State *s)
{
    if (s->revision_major < 0) {
        l1_printf(s, "L1 protocol responder; "
                  "firmware revision unavailable\r\n");
    } else if (s->profile->o3k) {
        l1_printf(s, "L1 %d.%d.%d (Image EMU-UNKNOWN)\r\n",
                  s->revision_major, s->revision_minor, s->revision_patch);
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
    if (!up) {
        peer->events_enabled = false;
        peer->event_subchannel = 0;
    }
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

static int l1_run_line(SGIL1State *s, const char *text, bool console);

/* Restart the controller. A C-brick's or I-brick's peers hear nothing. */
static void l1_reboot(SGIL1State *s)
{
    l1_boot(s);
    if (s->profile->o3k) {
        s->restart_until = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                           L1_O3K_RESTART_NS;
    }
}

/*
 * The commands each brick lacks, where every reading of the guide's table
 * agrees. Anything else the guide lists but this model does not run is
 * reported as an unimplemented command, not refused here.
 */
static bool l1_o3k_refused(const SGIL1State *s, const char *word)
{
    static const char *const ibrick[] = {
        "*", "autopower", "cpu", "ctc", "cti", "debug", "leds", "nmi",
        "pimm", "select", "sel", "softreset", "softrst", "junkbus",
    };

    if (l1_is_cxbrick(s)) {
        /* The C-brick table does not transfer to the Cx-brick. */
        return false;
    }
    if (!strcmp(word, "router")) {
        return true;
    }
    if (l1_is_cbrick(s)) {
        return !strcmp(word, "pci");
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ibrick); i++) {
        if (!strcmp(word, ibrick[i])) {
            return true;
        }
    }
    return false;
}

/*
 * Run @text on an attached controller and print what it said under its
 * name, as ctc does. The controller is asked in-process; a controller that
 * is restarting does not answer. A command run on another controller is
 * marked as relayed, so it cannot relay or broadcast onward; the sender's
 * own part of a broadcast is not relayed, only collected under its name.
 */
static int l1_relay(SGIL1State *s, SGIL1State *target, const char *text)
{
    g_autofree char *name = l1_module_name(target);
    g_autofree char *output = NULL;
    GString *saved = target->capture;
    bool serving = target->serving_peer;
    int result;

    /* A brick always hears itself; only another brick can be restarting. */
    if (target != s && l1_restarting(target)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: no response from a restarting controller\n");
        l1_printf(s, "%s:\r\nno response\r\n", name);
        return -1;
    }
    /* The output of the relayed command is collected, not printed. */
    target->capture = g_string_new(NULL);
    if (target != s) {
        target->serving_peer = true;
    }
    result = l1_run_line(target, text, false);
    output = g_string_free(target->capture, FALSE);
    target->capture = saved;
    target->serving_peer = serving;
    /* One line of its own, so a task-3 reply carries it as a field. */
    l1_printf(s, "%s:\r\n%s", name, output);
    return result;
}

/* A C-brick's "*": this brick, the other C-brick and the I-brick, in turn. */
static int l1_o3k_broadcast(SGIL1State *s, const char *text)
{
    SGIL1State *targets[] = { s, l1_linked_peer(s), s->io_peer };
    int result = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(targets); i++) {
        if (targets[i] && l1_relay(s, targets[i], text)) {
            result = -1;
        }
    }
    return result;
}

/* Power and reset on an I-brick act on that brick alone. */
static void l1_ibrick_power(SGIL1State *s, int argc, char **argv)
{
    if (l1_word(argv[0], "reset", "rst")) {
        l1_peer_reset(s);
        if (s->node_power) {
            qemu_irq_pulse(s->node_reset_out);
        }
    } else if (argc == 2 && l1_word(argv[1], "up", "u")) {
        if (s->node_power) {
            l1_printf(s, "Power is already on\r\n");
        } else {
            l1_peer_power(s, true);
        }
    } else if (argc == 2 && l1_word(argv[1], "down", "d")) {
        l1_peer_power(s, false);
    } else if (argc == 2 && !strcmp(argv[1], "check")) {
        l1_printf(s, "power appears %s\r\n", s->node_power ? "on" : "off");
    } else {
        l1_invalid(s, "power");
    }
}

/* The cfg listing: this brick at 0, each cabled brick at its port number. */
static void l1_o3k_config(SGIL1State *s)
{
    SGIL1ConfigRow rows[3];
    unsigned count = l1_config_rows(s, rows);

    for (unsigned i = 0; i < count; i++) {
        g_autofree char *name = l1_module_name(rows[i].brick);

        l1_printf(s, "%u: %s\r\n", rows[i].index, name);
    }
}

static void l1_o3k_brick_print(SGIL1State *s)
{
    char rack[8], bay[8];
    uint32_t partition = l1_partition(s);
    g_autofree char *partition_text = !partition ? g_strdup("none") :
        partition < 255 ? g_strdup_printf("%u", partition) : g_strdup("?");

    if (s->rack_valid) {
        snprintf(rack, sizeof(rack), "%03u", s->rack);
    } else {
        g_strlcpy(rack, "---", sizeof(rack));
    }
    if (s->bay_valid) {
        snprintf(bay, sizeof(bay), "%02u", s->bay);
    } else {
        g_strlcpy(bay, "--", sizeof(bay));
    }
    /*
     * Zero prints none, and the type is followed by the flash text and the
     * serial, as the L1 datasheet's brick row has them. That the Origin 3000
     * images print the same is inferred, not captured. The flash size is
     * unknown, so the unknown-size form is used, and a brick with no serial
     * prints the C library's null text. A partition of 255 or more prints
     * ?, also from the datasheet's row and inferred for this image.
     */
    l1_printf(s, "rack: %s, slot: %s, partition: %s, type: %c [?MB flash], "
              "serial:%s, source: EEPROM\r\n", rack, bay, partition_text,
              l1_is_iobrick(s) ? (char)s->profile->identity :
              !l1_is_cxbrick(s) ? 'C' : (char)l1_brick_type_character(s),
              s->brick_serial && s->brick_serial[0] ? s->brick_serial :
                                                      "(null)");
}

/*
 * "serial" of a Chimera interface board: the serial numbers and the time in
 * "%m/%d/%Y %X %Z" form. The brick serial comes from the same lookup as
 * brick, so an unset serial prints the C library's null text, and with no
 * time zone set the library's default name is GMT; both are inferred. The
 * "Security: OFF" suffix follows only when a security setting is non-zero,
 * and the model has none.
 */
static void l1_chimera_serial(SGIL1State *s)
{
    struct tm tm;
    char when[40];

    qemu_get_timedate(&tm, s->clock_offset);
    strftime(when, sizeof(when), "%m/%d/%Y %H:%M:%S GMT", &tm);
    if (!s->brick_serial || !s->brick_serial[0] ||
        !s->system_serial || !s->system_serial[0]) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: serial: an unset serial prints "
                      "(null), and the time zone name GMT, by inference\n");
    }
    l1_printf(s, "BSN: %s    SSN: %s    Time: %s\r\n",
              s->brick_serial && s->brick_serial[0] ? s->brick_serial :
                                                      "(null)",
              s->system_serial && s->system_serial[0] ? s->system_serial :
                                                        "(null)", when);
}

/*
 * "serial all" of a Chimera interface board. The Data table and the EEPROM
 * row formats are recorded, and so are the row lists of the 2U and
 * workstation profiles (inferred to belong to these letters). Only the node
 * board has a record in the model; the other positions print "no hardware
 * detected". Where a standalone brick's reference serials come from is not
 * recorded, so those rows read NVRAM and "not set". The brick-serial block
 * ends with an empty line and the EEPROM header starts with a line end of
 * its own, which leaves two empty lines (inferred); the header ends in four
 * spaces and the rule in one. Q uses the 2U list. The template allows
 * EEPROM or NVRAM for the local system serial: where it is held was not
 * traced, and the Q capture shows NVRAM, so NVRAM is printed.
 */
static void l1_o300_board_fields(SGIL1State *s, unsigned component,
                                 char fields[5][64]);

static bool l1_chimera_serial_all(SGIL1State *s)
{
    static const char *const list_2u[] = {
        "INTERFACE", "IO9", "ODYSSEY", "RISER", "NODE", "SNOWBALL",
        "PS 1", "PS 2",
    };
    static const char *const list_ws[] = {
        "INTERFACE", "IO9", "ODYSSEY", "SNOWBALL", "NODE", "IO DGHTR",
    };
    const char *const *list = s->brick_type_code == '9' ? list_ws : list_2u;
    unsigned count = s->brick_type_code == '9' ? ARRAY_SIZE(list_ws) :
                                                 ARRAY_SIZE(list_2u);
    char node[5][64] = { "", "", "", "", "" };
    const char *system_serial = s->system_serial && s->system_serial[0] ?
                                s->system_serial : "not set";
    const char *brick_serial = s->brick_serial && s->brick_serial[0] ?
                               s->brick_serial : "not set";

    l1_o300_board_fields(s, 0, node);
    qemu_log_mask(LOG_UNIMP, "sgi-l1: serial all: reference serial "
                  "locations and values and the local system serial's "
                  "location (NVRAM chosen) are not recorded, and only the "
                  "node board has a record\n");
    l1_printf(s,
              "Data                            Location      Value\r\n"
              "------------------------------  ------------  --------\r\n"
              "Local System Serial Number      %-12s  %s\r\n"
              "Reference System Serial Number  %-12s  %s\r\n"
              "Local Brick Serial Number       %-12s  %s\r\n"
              "Reference Brick Serial Number   %-12s  %s\r\n"
              "\r\n\r\n"
              "EEPROM      Product Name    Serial         "
              "Part Number           Rev  T/W    \r\n"
              "----------  --------------  -------------  "
              "--------------------  ---  ------ \r\n",
              "NVRAM", system_serial, "NVRAM", "not set",
              "EEPROM", brick_serial, "NVRAM", "not set");
    for (unsigned i = 0; i < count; i++) {
        if (!strcmp(list[i], "NODE")) {
            l1_printf(s, "%-10s  %-14s  %-13s  %-20s  %-3s  %-6s\r\n",
                      "NODE", node[0], node[1], node[2], node[3], node[4]);
        } else {
            l1_printf(s, "%-10s  no hardware detected\r\n", list[i]);
        }
    }
    return true;
}

/* The type names of the Chimera interface boards, by brick type letter. */
static const char *l1_chimera_type_name(const SGIL1State *s)
{
    if (s->profile->o3k) {
        return NULL;
    }
    switch (s->brick_type_code) {
    case '7':
        return "Chimera Server";
    case '8':
        return "Chimera Blade";
    case '9':
        return "Chimera WS";
    case 'Q':
        return "Rackmount WS";
    default:
        return NULL;
    }
}

/*
 * The Origin 300 "brick" line, shared by the console and the wire path (L1
 * datasheet, Origin 300 brick). The model holds no flash capacity or
 * identity source, so the flash label is EMU-UNKNOWN and the source says
 * whether the serial was configured.
 */
static char *l1_o300_brick_line(SGIL1State *s)
{
    bool configured = s->brick_serial && s->brick_serial[0];
    g_autofree char *partition = s->partition ?
        g_strdup_printf("%u", s->partition) : g_strdup("none");

    return g_strdup_printf(
        "rack: %03u, slot: %02u, partition: %s, type: C (IP45) "
        "[EMU-UNKNOWN flash], serial:%s, source: %s",
        s->rack, s->bay, partition,
        configured ? s->brick_serial : "EMU-UNKNOWN-BSN",
        configured ? "CONFIGURED" : "EMULATED");
}

static void l1_chimera_brick(SGIL1State *s, const char *type)
{
    g_autofree char *partition = NULL;

    if (!s->partition) {
        partition = g_strdup("none");
    } else if (s->partition < 255) {
        partition = g_strdup_printf("%u", s->partition);
    } else {
        partition = g_strdup("?");
    }
    if (!s->brick_serial || !s->brick_serial[0]) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: brick with no serial prints "
                      "(null) by inference\n");
    }
    /*
     * The line has no letter before the type name. The flash text is the
     * 2MB image's. The source comes from the type decision alone, so it
     * is EEPROM with or without a serial; a brick with no board-record
     * serial prints the C library's null text (inferred). The partition
     * prints "?" from 255 up.
     */
    l1_printf(s, "rack: %03u, slot: %02u, partition: %s, type: %s "
              "[2MB flash], serial:%s, source: EEPROM\r\n", s->rack, s->bay,
              partition, type,
              s->brick_serial && s->brick_serial[0] ? s->brick_serial :
                                                      "(null)");
}

/* One brick has itself at position 0 and nothing attached. */
static void l1_chimera_config(SGIL1State *s)
{
    g_autofree char *name = l1_module_name(s);

    qemu_log_mask(LOG_UNIMP, "sgi-l1: config lists only the local brick; "
                  "attached bricks are not modelled for this board\n");
    l1_printf(s, ":%-2u - %s (local)\r\n", 0, name);
}

/*
 * The "env" report of a Chimera interface board: type 7 is the 2U board,
 * type 9 the workstation board and type Q the rack-mounted workstation, and
 * the node board's name picks IP53 or IP59. Returns false for every other
 * brick, which keeps "env" unmodelled.
 */
static bool l1_chimera_env(SGIL1State *s)
{
    g_autofree char *text = NULL;

    if (s->profile->o3k) {
        return false;
    }
    text = sgi_l1_chimera_env(s->brick_type_code, s->node_board_name,
                              s->node_power, &s->env, true);
    if (!text) {
        return false;
    }
    l1_printf(s, "%s", text);
    return true;
}

/* "env off" with temperature checking off, or the reverse, warns once. */
static void l1_env_overtemp_warning(SGIL1State *s)
{
    l1_printf(s, "WARNING: environmental monitoring is disabled "
              "(with 'env off') and\r\n"
              "temperature monitoring is explicitly disabled "
              "(with 'env temp off');\r\n"
              "**** OVERTEMP SHUTDOWN IS DISABLED!!! ****\r\n");
}

/* An environmental event reaches the console unless the filter drops it. */
static void l1_chimera_env_event(SGIL1State *s, unsigned severity,
                                 const char *text)
{
    g_autofree char *name = l1_module_name(s);

    if (s->env.faults_only && severity < 2) {
        return;
    }
    l1_printf(s, "\r\n%s ATTN: %s\r\n", name, text);
}

static bool l1_chimera_fan_command(SGIL1State *s, int argc, char **argv)
{
    g_autofree char *text = NULL;
    unsigned long value;

    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "speed"))) {
        text = sgi_l1_chimera_fan(s->brick_type_code, s->node_board_name,
                                  s->node_power, &s->env, argc == 2);
        if (!text) {
            qemu_log_mask(LOG_UNIMP, "sgi-l1: fan speed on the workstation "
                          "needs its fan-to-channel mapping, which is not "
                          "recorded\n");
            return false;
        }
        l1_printf(s, "%s", text);
        return true;
    }
    if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: fan on and off do not change "
                      "the modelled fans\n");
        s->env.fans_off = !strcmp(argv[1], "off");
        return true;
    }
    if (argc == 3 && !strcmp(argv[1], "speed")) {
        int level = !strcmp(argv[2], "low") || !strcmp(argv[2], "l") ? 0 :
                    !strcmp(argv[2], "med") || !strcmp(argv[2], "m") ? 1 :
                    !strcmp(argv[2], "high") || !strcmp(argv[2], "h") ? 2 :
                    -1;

        return level >= 0 &&
               sgi_l1_chimera_fan_set(s->brick_type_code, s->node_board_name,
                                      &s->env, -1, level, 0);
    }
    if (argc == 4 && !strcmp(argv[1], "speed")) {
        unsigned long fan;

        if (qemu_strtoul(argv[2], NULL, 10, &fan) ||
            qemu_strtoul(argv[3], NULL, 10, &value) || fan > 255) {
            return false;
        }
        qemu_log_mask(LOG_UNIMP, "sgi-l1: fans that share a control "
                      "channel are written one at a time\n");
        return sgi_l1_chimera_fan_set(s->brick_type_code, s->node_board_name,
                                      &s->env, fan, -1, value);
    }
    if (argc == 3 && !strcmp(argv[1], "auto") &&
        sgi_l1_chimera_has_autofan(s->brick_type_code) &&
        (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        s->env.fan_auto_off = !strcmp(argv[2], "off");
        return true;
    }
    return false;
}

/*
 * The env and fan commands of a Chimera interface board. The settings are
 * the operator's; none of them drives a model of the node's sensors. Returns
 * false for a form that is not modelled, so it stays an unknown command.
 */
static bool l1_chimera_env_command(SGIL1State *s, int argc, char **argv)
{
    g_autofree char *probe = NULL;
    unsigned long limit[5];

    probe = s->profile->o3k ? NULL :
            sgi_l1_chimera_env(s->brick_type_code, s->node_board_name,
                               false, &s->env, false);
    if (!probe) {
        return false;
    }
    if (!strcmp(argv[0], "fan")) {
        return l1_chimera_fan_command(s, argc, argv);
    }
    if (argc == 2 && !strcmp(argv[1], "check")) {
        l1_printf(s, "%s", probe);
        return true;
    }
    if (argc == 2 && !strcmp(argv[1], "reset")) {
        /* The restart sets every fan back to its low level (inferred). */
        qemu_log_mask(LOG_UNIMP, "sgi-l1: env reset restores the fan low "
                      "levels by inference\n");
        s->env.fan_override_mask = 0;
        return true;
    }
    if (argc == 2 && !strcmp(argv[1], "on")) {
        /* Clears temperature-off and monitor-only, not "env power off". */
        s->env.disabled = false;
        s->env.temp_off = false;
        if (s->env.power_mode == 2) {
            s->env.power_mode = 0;
        }
        return true;
    }
    if (argc == 2 && !strcmp(argv[1], "off")) {
        s->env.disabled = true;
        if (s->env.temp_off) {
            l1_env_overtemp_warning(s);
        }
        return true;
    }
    if (argc == 3 && !strcmp(argv[1], "temp") &&
        (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
        s->env.temp_off = !strcmp(argv[2], "off");
        if (s->env.temp_off && s->env.disabled) {
            l1_env_overtemp_warning(s);
        }
        return true;
    }
    if (argc == 3 && !strcmp(argv[1], "power")) {
        if (!strcmp(argv[2], "on")) {
            s->env.power_mode = 0;
        } else if (!strcmp(argv[2], "off")) {
            s->env.power_mode = 1;
        } else if (!strcmp(argv[2], "monitor")) {
            s->env.power_mode = 2;
        } else {
            return false;
        }
        return true;
    }
    if (argc == 3 && !strcmp(argv[1], "altitude") &&
        (!strcmp(argv[2], "high") || !strcmp(argv[2], "low"))) {
        s->env.altitude_high = !strcmp(argv[2], "high");
        return true;
    }
    if (argc == 7 && !strcmp(argv[1], "test")) {
        /* The five-value limit form: advisory, critical, fault, max, okay. */
        for (unsigned i = 0; i < 5; i++) {
            if (qemu_strtoul(argv[2 + i], NULL, 10, &limit[i]) ||
                limit[i] > 255) {
                return false;
            }
        }
        for (unsigned i = 0; i < 4; i++) {
            s->env.custom[i] = limit[i];
        }
        s->env.custom_limits = true;
        l1_printf(s, "temperature limits set.\r\n");
        return true;
    }
    if (argc == 3 && !strcmp(argv[1], "test")) {
        static const struct {
            const char *word;
            const char *text;
            unsigned severity;
        } events[] = {
            { "warning", "Environmental warning test", 1 },
            { "fault", "Environmental fault test", 2 },
            { "tmpadv", "Environmental advisory temp test", 1 },
            { "tmpcrit", "Environmental critical temp test", 1 },
            { "pwrdn", "Environmental fault/powerdown test", 2 },
        };

        for (unsigned i = 0; i < ARRAY_SIZE(events); i++) {
            if (strcmp(argv[2], events[i].word)) {
                continue;
            }
            if (i != 4) {
                l1_printf(s, "test event generated.\r\n");
            } else {
                qemu_log_mask(LOG_UNIMP, "sgi-l1: env test pwrdn posts its "
                              "event but does not run the power-down "
                              "countdown or power the node off\n");
            }
            l1_chimera_env_event(s, events[i].severity, events[i].text);
            return true;
        }
        return false;
    }
    if (argc == 2 && !strcmp(argv[1], "messages")) {
        l1_printf(s, s->env.faults_only ?
                  "only environmental faults visible.\r\n" :
                  "all environmental events visible.\r\n");
        return true;
    }
    if (argc == 3 && !strcmp(argv[1], "messages") &&
        (!strcmp(argv[2], "faultsonly") || !strcmp(argv[2], "all"))) {
        s->env.faults_only = !strcmp(argv[2], "faultsonly");
        l1_printf(s, s->env.faults_only ?
                  "only environmental faults will be visible.\r\n" :
                  "all environmental events will be visible.\r\n");
        return true;
    }
    return false;
}

/* The "power" table of a Chimera interface board; false if not modelled. */
static bool l1_chimera_power(SGIL1State *s)
{
    g_autofree char *text = NULL;

    if (s->profile->o3k) {
        return false;
    }
    text = sgi_l1_chimera_power(s->brick_type_code, s->node_board_name,
                                s->node_power);
    if (!text) {
        return false;
    }
    l1_printf(s, "%s", text);
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
    if (argc && s->profile->o3k && l1_o3k_refused(s, argv[0])) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: command %s unsupported on the %s "
                      "profile\n", argv[0], s->profile->name);
        l1_printf(s, "INFO: command not supported on this brick type\r\n");
        result = -1;
        goto complete;
    }
    if (argc && l1_is_cbrick(s) && !strcmp(argv[0], "*") &&
        (s->serving_peer || (argc > 1 && !strcmp(argv[1], "*")))) {
        /*
         * A broadcast that reaches a brick through a relay, or that names
         * another broadcast, would reach the same bricks again, and each
         * extra "*" would double the work. The relay depth is one.
         */
        qemu_log_mask(LOG_UNIMP, "sgi-l1: nested * is unsupported\n");
        l1_printf(s, "INFO: nested * is unsupported.%s",
                  "\r\n");
        result = -1;
        goto complete;
    }
    if (argc > 1 && l1_is_cbrick(s) && !strcmp(argv[0], "*")) {
        result = l1_o3k_broadcast(s, original + (argv[1] - line));
        goto complete;
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
    if (l1_is_ibrick(s) && (l1_word(argv[0], "power", "pwr") ||
                            l1_word(argv[0], "reset", "rst"))) {
        l1_ibrick_power(s, argc, argv);
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
        /* The Origin 300 profile's text keeps its LF off the console. */
        l1_printf(s, "INFO: nested ctc is unsupported.%s",
                  console || s->profile->o3k ? "\r\n" : "\n");
        result = -1;
        goto complete;
    }
    if (!strcmp(argv[0], "ctc") && argc > 1 && peer) {
        /* The peer runs it; its brick id line comes first. */
        result = l1_relay(s, peer, original + (argv[1] - line));
        goto complete;
    }
    if (!strcmp(argv[0], "cti") && argc > 1 && l1_is_cbrick(s)) {
        if (s->serving_peer) {
            /* Refused with or without an I-brick: it is the nesting. */
            qemu_log_mask(LOG_UNIMP, "sgi-l1: nested cti is unsupported\n");
            l1_printf(s, "INFO: nested cti is unsupported.%s",
                      "\r\n");
            result = -1;
        } else if (!s->io_peer) {
            qemu_log_mask(LOG_UNIMP, "sgi-l1: cti needs an attached I-brick\n");
            l1_printf(s, "INFO: no I/O brick is attached.%s",
                      "\r\n");
            result = -1;
        } else {
            result = l1_relay(s, s->io_peer, original + (argv[1] - line));
        }
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
        } else if (argc == 1 && l1_chimera_power(s)) {
            /* Printed by l1_chimera_power(). */
        } else {
            if (argc == 2 && !strcmp(argv[1], "vrm")) {
                qemu_log_mask(LOG_UNIMP, "sgi-l1: power vrm: the VRM list "
                              "per board is not recorded\n");
            }
            if (argc == 3 && l1_word(argv[1], "up", "u") &&
                !strcmp(argv[2], "hold")) {
                qemu_log_mask(LOG_UNIMP, "sgi-l1: power up hold is not "
                              "modelled\n");
            }
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
            for (unsigned i = 0; i < SGI_L1_CX_BOARDS; i++) {
                qemu_irq_pulse(s->nmi_out[i]);
            }
        }
    } else if (!strcmp(argv[0], "pbay") && sgi_l1_pbay_active(&s->pbay)) {
        /* Only the link's status word is established; the rest is not. */
        qemu_log_mask(LOG_UNIMP, "sgi-l1: the pbay command's output format "
                      "is not established\n");
        l1_printf(s, "%s\r\n", sgi_l1_pbay_status_name(&s->pbay));
    } else if (!strcmp(argv[0], "reboot_l1")) {
        l1_reboot(s);
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
            g_autofree char *name = l1_module_name(s);
            g_autofree char *designation = l1_designation(s);

            l1_printf(s, "console input: %s %s\r\n"
                      "console output: %s\r\n", name, designation,
                      s->cx_filter ? "filtered." : "not filtered.");
        } else if (l1_is_cxbrick(s) && argc == 3 &&
                   !strcmp(argv[1], "filter") &&
                   (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
            s->cx_filter = !strcmp(argv[2], "on");
        } else if (l1_is_cxbrick(s)) {
            if (argc > 3 ||
                (argc == 3 && !l1_word(argv[1], "subchannel", "sub") &&
                 strcmp(argv[1], "s")) ||
                !l1_cx_select(s, argv[argc - 1])) {
                l1_printf(s, "ERROR: bad subchanel selection.\r\n");
            }
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

        if (argc == 1 && s->profile->o3k) {
            l1_o3k_brick_print(s);
        } else if (argc == 1 && l1_is_rbrick(s)) {
            /*
             * A router has no partition: the field prints N/A. The type
             * letter has no name entry in the firmware's switch, so it
             * prints as itself; the short tail is the C-brick and I-brick
             * line's, an approximation.
             */
            l1_printf(s, "rack: %03u, slot: %02u, partition: N/A, type: R, "
                      "source: EEPROM\r\n", s->rack, s->bay);
        } else if (argc == 3 && !strcmp(argv[1], "partition") &&
                   (l1_is_rbrick(s) || l1_is_iobrick(s))) {
            /* The refusal strings are the L1 sheet's; the code is ours. */
            l1_printf(s, "%s", l1_is_rbrick(s) ?
                      "INFO: can't set partition on routers.\r\n" :
                      "INFO: can't set partition on I/O brick (it is "
                      "acquired from attached C-brick)\r\n");
            result = -1;
        } else if (argc == 1 && l1_chimera_type_name(s)) {
            l1_chimera_brick(s, l1_chimera_type_name(s));
        } else if (argc == 1) {
            g_autofree char *brick = l1_o300_brick_line(s);

            l1_printf(s, "%s\r\n", brick);
        } else if (argc == 3 && !strcmp(argv[1], "slot") &&
                   !qemu_strtoul(argv[2], NULL, 10, &value)) {
            if (value < 1 || value > 39) {
                l1_printf(s, "Invalid slot specified.  "
                          "Valid slot numbers are 1-39.\r\n");
            } else {
                s->pending_bay = value;
                s->pending_bay_valid = true;
                l1_printf(s, "brick slot set to %02lu (takes effect on next "
                          "L1 reboot/power cycle)\r\n", value);
            }
        } else if (argc == 3 && !strcmp(argv[1], "rack") &&
                   !qemu_strtoul(argv[2], NULL, 10, &value) && value < 0x3ff) {
            s->pending_rack = value;
            s->pending_rack_valid = true;
            l1_printf(s, "brick rack set to %03lu (takes effect on next "
                      "L1 reboot/power cycle)\r\n", value);
        } else if (argc == 3 && !strcmp(argv[1], "partition") &&
                   !strcmp(argv[2], "none")) {
            /* The L1 sheet's brick setter: "none" clears the stored ID. */
            s->pending_partition = 0;
            l1_printf(s, "brick partition cleared.\r\n");
        } else if (argc == 3 && !strcmp(argv[1], "partition") &&
                   !qemu_strtoul(argv[2], NULL, 10, &value) &&
                   value <= 0xffff) {
            s->pending_partition = value;
            l1_printf(s, "brick partition set to %lu (takes effect on next "
                      "L1 reboot/power cycle)\r\n", value);
        } else {
            l1_invalid(s, "brick");
        }
    } else if (s->profile->o3k && argc == 1 &&
               l1_word(argv[0], "config", "cfg")) {
        l1_o3k_config(s);
    } else if (argc == 1 && l1_chimera_type_name(s) &&
               l1_word(argv[0], "config", "cfg")) {
        l1_chimera_config(s);
    } else if (argc == 1 && l1_chimera_type_name(s) &&
               !strcmp(argv[0], "serial")) {
        l1_chimera_serial(s);
    } else if (argc == 2 && l1_chimera_type_name(s) &&
               !strcmp(argv[0], "serial") && !strcmp(argv[1], "all") &&
               l1_chimera_serial_all(s)) {
        /* Printed by l1_chimera_serial_all(). */
    } else if (argc == 1 && !strcmp(argv[0], "env") && l1_chimera_env(s)) {
        /* Printed by l1_chimera_env(). */
    } else if ((!strcmp(argv[0], "env") || !strcmp(argv[0], "fan")) &&
               l1_chimera_env_command(s, argc, argv)) {
        /* The settings are applied by l1_chimera_env_command(). */
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

    SGIL1UARTState *uart = s->uart;

    if (l1_is_cxbrick(s)) {
        /* Input waits only on the console board's link, when it has one. */
        uart = s->transport[s->cx_console_board];
        if (!uart) {
            return 1;
        }
    }
    /* One input byte can expand to an eight-byte escaped console frame. */
    return uart && l1_uart_room(uart) >= 8;
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
                uint8_t payload[] = {
                    l1_is_cxbrick(s) ? s->cx_console_subchannel :
                                       s->port_field & 0x1f,
                    byte
                };

                /* A Cx-brick's leaves on the console board's link only. */
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

    /* A controller with no rack or slot answers only the broadcast key. */
    if (l1_named(s) && l1_is_cxbrick(s)) {
        /* A Cx-brick answers for each fitted board's slot. */
        for (unsigned board = 0; board < SGI_L1_CX_BOARDS; board++) {
            if (l1_cx_fitted_board(s, board) &&
                key == ((s->rack << 6) | (s->bay + board))) {
                return true;
            }
        }
    }
    return (l1_named(s) && key == ((s->rack << 6) | s->bay)) || key == 0xffff;
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

/*
 * The Cx-brick profile's codes: those its request-code table grades, and
 * those the public SN1 header names with the meaning the Origin 300 image
 * serves (debug switches, firmware revision, the display lines, partition
 * set and event subchannel), and the I/O link speed the stock PROM reads
 * from its node's own L1. Anything else has no reviewed meaning on a
 * Cx-brick and is refused.
 */
static bool l1_cx_graded(uint16_t command)
{
    switch (command) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 0xa:
    case 0xe:
    case 0x1002:
    case 0x1003:
    case 0x1004:
    case 0x1005:
    case 0x1006:
    case 0x1007:
    case 0x2001:
    case 0x2005:
    case 0x2006:
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

    if (l1_is_iobrick(s)) {
        /*
         * An I-brick or V-brick knows its C-brick and nothing else; an
         * N-brick has several and no operator relay to any.
         */
        peer = type == 4 || l1_is_nbrick(s) ? NULL : l1_host_of(s, NULL);
    } else if (type == 4) {
        /*
         * The attached I/O brick of a C-brick, or of a Cx-brick, whose
         * addressing of its four I/O ports is unknown: the one I-brick
         * answers the C-brick's request form (an approximation).
         */
        peer = l1_is_cbrick(s) || l1_is_cxbrick(s) ? s->io_peer : NULL;
    }
    trace_sgi_l1_forward(s->profile->name, address, length, peer != NULL);
    if (!peer || length > sizeof(copy)) {
        return false;
    }
    if (type == 3 || type == 4) {
        address &= ~(UINT32_C(0xf) << 28);
    } else if (type != 0 || !l1_named(peer) ||
               key != ((peer->rack << 6) | peer->bay)) {
        return false;
    }
    if (l1_restarting(peer)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: no response from a restarting controller\n");
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

/* The chunk transport also carries a request for the L2 while it is linked. */
static bool l1_chunk_enabled(const SGIL1State *s)
{
    return l1_irouter_enabled(s) || l1_l2_linked(s);
}

/*
 * Without an IRouter of its own, a linked controller takes a chunk frame only
 * if it starts, or goes on with, a request for the L2. Any other chunk frame
 * is handled as it is with no link, so the link adds nothing else.
 */
static bool l1_chunk_for_l2(const SGIL1State *s, const uint8_t *p,
                            unsigned length, unsigned transport)
{
    if (transport >= SGI_L1_IROUTER_TRANSPORTS) {
        return false;
    }
    if (p[6] != 1) {
        return s->irouter[transport * SGI_L1_IROUTER_CHANNELS +
                          (p[0] & 0x1f)].active;
    }
    return length >= 10 + SGI_L1_L2_SEG_DEST + 4 &&
           l1_l2_destination(ldl_be_p(p + 10 + SGI_L1_L2_SEG_DEST));
}

static void l1_request(SGIL1State *s, unsigned transport)
{
    SGIL1FrameState *frame = &s->frame[transport];

    if (!l1_is_cxbrick(s)) {
        l1_request_frame(s, frame->data, frame->length - 2, transport);
        return;
    }
    /*
     * A Cx-brick answers on the board link that asked, with that link's own
     * port field in port_field for the length of the request.
     */
    s->cx_serving = true;
    s->cx_link = transport;
    s->port_field = s->cx_port_field[transport];
    l1_request_frame(s, frame->data, frame->length - 2, transport);
    s->cx_port_field[transport] = s->port_field;
    s->cx_serving = false;
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

/*
 * The compatibility modes that reach this text are refused at realize for
 * the Origin 3000 profiles, so a Cx-brick never prints brick-serial here.
 */
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
        g_autofree char *line = l1_o300_brick_line(s);

        return g_strdup_printf("%s\n\n", line);
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
            if (l1_l2_forward_chunked(s, transport, channel, assembly->data,
                                      first_length)) {
                /* Left on the link; the response comes back as chunks. */
            } else if (l1_irouter_enabled(s)) {
                l1_irouter_reply(s, transport, channel, assembly->data,
                                 first_length, fragment_count);
            } else {
                qemu_log_mask(LOG_UNIMP,
                              "sgi-l1: chunked request has no receiver\n");
            }
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
    l1_trace_chunk(s, false, transport, p, length);
    assembly = &s->irouter[transport * SGI_L1_IROUTER_CHANNELS + channel];
    payload_length = length - 10;
    if (command == 0x3001) {
        uint8_t fragments = p[7];
        uint16_t fragment_length = lduw_be_p(p + 8);
        bool fits = fragments && fragments < 128 && fragment_length &&
                    fragment_length <= sizeof(assembly->data) &&
                    payload_length <= fragment_length;

        if (assembly->active) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sgi-l1: replacing pending IRouter request\n");
        }
        memset(assembly, 0, sizeof(*assembly));
        if (!l1_chunk_enabled(s) || !fits) {
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

    if (l1_chunk_enabled(s) && length >= 7 && !(p[0] & 0xe0) &&
        ldl_be_p(p + 1) == 0 &&
        p[5] == 0x30 && (p[6] == 1 || p[6] == 2 || p[6] == 3) &&
        (l1_irouter_enabled(s) || l1_chunk_for_l2(s, p, length, transport))) {
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
        if (l1_is_cxbrick(s) && s->cx_filter &&
            s->cx_link != s->cx_console_board) {
            /* The filter passes the selected board's output alone. */
            return;
        }
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
    if (trace_event_get_state_backends(TRACE_SGI_L1_REQUEST)) {
        g_autofree char *hex = l1_hex_string(p + MIN(length, 8),
                                             length > 8 ? length - 8 : 0);
        unsigned rack, bay;

        l1_trace_name(s, &rack, &bay);
        trace_sgi_l1_request(s->profile->name, rack, bay, transport, address,
                             command, length == 7 ? 0 : p[7], hex);
    }
    if (!l1_local(s, address)) {
        if (!s->reply_to && l1_l2_destination(address) &&
            l1_l2_forward_plain(s, channel, address, command,
                                p + MIN(length, 8),
                                length > 8 ? length - 8 : 0,
                                length == 7 ? 0 : p[7], transport)) {
            return;
        }
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
            g_autofree char *name = l1_module_name(s);
            g_autofree char *text = g_strdup_printf(
                "WARNING: power on %s appears off!", name);

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
    if (l1_is_cxbrick(s) && !l1_cx_graded(command)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-l1: request 0x%04x has no reviewed meaning on the "
                      "cxbrick profile\n", command);
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
                 command == 9 ? l1_partition(s) : s->two_bit_setting);
        field_count = 1;
        field_length = 5;
        break;
    case 0xa:
        /*
         * The I/O link speed: a first integer of 0 means 400 MHz, any other
         * 600 MHz. The stock IP35 PROM compares it with the speed it
         * expects for the attached brick's letter (400 for I, N and X, 600
         * for P and V) and warns on a mismatch, except on an Origin 300
         * board, which skips the check. Every profile answers 0 unless
         * port-speed-first says otherwise; that is right for the I-brick,
         * the N-brick and the V-brick, which answers X.
         *
         * An I/O brick's L1 firmware answers with its port A and port B
         * speeds, in that order. The V-brick's 0 and 0, 400 MHz on both,
         * are an approximation: they are the firmware's default with no
         * C-brick attached and match the guide's 800 MB/s port rate, read
         * as a 400 MHz link (inferred), but what a cabled V-brick stores
         * is unknown.
         */
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
            /* The C-brick and I-brick profiles store sixteen bits. */
            if (s->profile->o3k && ldl_be_p(args + 1) > 0xffff) {
                error = -105;
                break;
            }
            s->partition = s->pending_partition = ldl_be_p(args + 1);
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
        if (l1_is_cxbrick(s)) {
            /* The public SN1 header gives 0x2001 to PCI power-up instead. */
            qemu_log_mask(LOG_UNIMP,
                          "sgi-l1: cxbrick request 0x2001 served as the "
                          "Origin 300 reset; its Cx-brick meaning is "
                          "unknown\n");
        }
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
        l1_reboot(s);
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
        /*
         * Volunteering the only node changes nothing visible. On a Cx-brick
         * the volunteer's console subchannel takes the console input, a
         * model choice.
         */
        if (l1_is_cxbrick(s)) {
            s->cx_console_board = s->cx_link;
            s->cx_console_subchannel = 4;
        }
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
        stl_be_p(fields + 1, (s->rack << 6) |
                 (l1_is_cxbrick(s) ? l1_cx_board_slot(s) : s->bay));
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

    sgi_l1_pbay_reset(&s->pbay);
    memset(s->irouter, 0, sizeof(s->irouter));
    memset(s->l2_pending, 0, sizeof(s->l2_pending));
    s->l2_rx_len = 0;
    s->l2_chain_left = 0;
    s->l2_chain_tag = L1_L2_CHAIN_NONE;
    s->usb_test_state.set = false;
    g_strlcpy(s->usb_test_result, "none", sizeof(s->usb_test_result));
    g_strlcpy(s->usb_test_descriptor_result, "none",
              sizeof(s->usb_test_descriptor_result));
    /*
     * On the Origin 3000 machine every guest restart and L1 reset command
     * is a machine reset, and the machine resets this controller with its
     * brick. The L1 runs on standby power and no node reset restarts it,
     * so it keeps its settings, log and display; its own start is realize
     * and reboot_l1. The C-brick L1s are not reset with the machine.
     */
    if (s->profile->o3k) {
        return;
    }
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

    if ((!s->profile->compute_services && !l1_is_iobrick(s) &&
         (s->node_power || s->power_up_pending || s->power_down_pending ||
          s->console_mode || s->escaped)) ||
        (l1_is_iobrick(s) &&
         (s->power_up_pending || s->power_down_pending ||
          s->console_mode || s->escaped)) ||
        (!s->profile->o3k &&
         (!s->rack_valid || !s->bay_valid || !s->pending_rack_valid ||
          !s->pending_bay_valid)) ||
        (s->profile->o3k && s->pending_partition > 0xffff) ||
        s->command_len >= sizeof(s->command) ||
        s->port_field > 0x7f ||
        s->cx_console_board >= SGI_L1_CX_BOARDS ||
        s->cx_console_subchannel > 4 ||
        (l1_is_cxbrick(s) &&
         !l1_cx_fitted_board(s, s->cx_console_board)) ||
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
    for (unsigned i = 0; i < SGI_L1_CX_BOARDS; i++) {
        if (s->cx_port_field[i] > 0x7f) {
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

static bool l1_env_needed(void *opaque)
{
    SGIL1State *s = opaque;

    const SGIL1EnvSettings *e = &s->env;

    return e->disabled || e->temp_off || e->power_mode || e->altitude_high ||
           e->fan_auto_off || e->faults_only || e->fans_off ||
           e->custom_limits || e->fan_override_mask;
}

/* Sent only while an operator has changed an env or fan setting. */
static const VMStateDescription vmstate_l1_env = {
    .name = "sgi-l1/env",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = l1_env_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(env.disabled, SGIL1State),
        VMSTATE_BOOL(env.temp_off, SGIL1State),
        VMSTATE_UINT8(env.power_mode, SGIL1State),
        VMSTATE_BOOL(env.fan_auto_off, SGIL1State),
        VMSTATE_BOOL(env.altitude_high, SGIL1State),
        VMSTATE_BOOL(env.faults_only, SGIL1State),
        VMSTATE_BOOL(env.fans_off, SGIL1State),
        VMSTATE_BOOL(env.custom_limits, SGIL1State),
        VMSTATE_UINT8_ARRAY(env.custom, SGIL1State, 4),
        VMSTATE_UINT8(env.fan_override_mask, SGIL1State),
        VMSTATE_UINT8_ARRAY(env.fan_override, SGIL1State, 8),
        VMSTATE_END_OF_LIST()
    },
};

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
        VMSTATE_BOOL(rack_valid, SGIL1State),
        VMSTATE_BOOL(bay_valid, SGIL1State),
        VMSTATE_BOOL(pending_rack_valid, SGIL1State),
        VMSTATE_BOOL(pending_bay_valid, SGIL1State),
        VMSTATE_UINT32(pending_partition, SGIL1State),
        VMSTATE_INT64(restart_until, SGIL1State),
        VMSTATE_UINT32_EQUAL(io_port, SGIL1State),
        VMSTATE_UINT32_EQUAL(c_port, SGIL1State),
        VMSTATE_UINT32(io_gfx_bay, SGIL1State),
        VMSTATE_UINT32_EQUAL(cx_boards, SGIL1State),
        VMSTATE_UINT32_EQUAL(cx_compute, SGIL1State),
        VMSTATE_UINT8_ARRAY(cx_port_field, SGIL1State, SGI_L1_CX_BOARDS),
        VMSTATE_UINT8(cx_console_board, SGIL1State),
        VMSTATE_UINT8(cx_console_subchannel, SGIL1State),
        VMSTATE_BOOL(cx_filter, SGIL1State),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_l1_env,
        NULL
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
 * is @serial when the machine gave one, else component @letter's, instance
 * @index: "EMUN00" for node 0's board. On the model, with "000000" here
 * the stock IP35 PROM reported after every reset that the NVRAM did not
 * belong to this system and reinitialized it, and with the marked serials
 * it does not. Which property of "000000" the PROM rejects is unknown.
 */
static void l1_build_board_area(SGIL1EEPROM *record, const char *name,
                                const char *part, const char *revision,
                                const uint8_t *mac, char letter,
                                unsigned int index, const char *serial_text,
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
    l1_ia_text(area, serial_text ? serial_text : serial);
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

/*
 * The router board each product fits, as owner inventories print it. An
 * R-brick of an Origin 3400 reports 030-1634-002 revision B, and the
 * Origin 300 and 350 NUMAlink module 030-1634-003 revision A.
 *
 * The Origin 3900 Cx-brick's router board is an approximation: no
 * inventory of it has been read, so its part and revision are unknown. The
 * row carries the model's not-observed placeholders, which no hardware
 * reports, instead of borrowing either product's part. Its name ROUTER is
 * inferred. A real inventory or L1 listing of a Cx-brick replaces the row.
 * A product with no row gets no record.
 */
typedef struct SGIL1RouterBoard {
    const char *name;
    const char *part;
    const char *revision;
} SGIL1RouterBoard;

static const SGIL1RouterBoard l1_router_boards[] = {
    { "rbrick", "030-1634-002", "B" },
    { "numalink-module", "030-1634-003", "A" },
    { "cxbrick", "NOTOBSERVED0", "00" },
};

static const SGIL1RouterBoard *l1_router_board(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(l1_router_boards); i++) {
        if (!strcmp(name, l1_router_boards[i].name)) {
            return &l1_router_boards[i];
        }
    }
    return NULL;
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
                                s->board_serial, cpu_context_ptr);
        }
    }
    if (io8 && !l1_component_selected(s, 1)) {
        if (!s->records[1].length) {
            l1_build_chassis_area(&s->records[1]);
        }
        if (!s->records[17].length) {
            l1_build_board_area(&s->records[17], "IO8", "NOTOBSERVED0", "00",
                                mac, 'I', s->identity_index, NULL, NULL);
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
                                s->board_mac, 'N', s->identity_index,
                                s->board_serial, NULL);
        }
    }
    if (s->router_board && !l1_component_selected(s, 1)) {
        const SGIL1RouterBoard *board = l1_router_board(s->router_board);

        if (!s->records[1].length) {
            l1_build_chassis_area(&s->records[1]);
        }
        if (!s->records[17].length) {
            l1_build_board_area(&s->records[17], "ROUTER", board->part,
                                board->revision, NULL, 'R',
                                s->identity_index, NULL, NULL);
        }
    }
    return true;
}

static void l1_o3k_board(SGIL1State *s, unsigned component,
                         const char *name, const char *part,
                         const char *revision, const uint8_t *mac,
                         char letter, unsigned index,
                         const SGIL1CPUContext *cpu_context)
{
    l1_build_chassis_area(&s->records[component]);
    l1_build_board_area(&s->records[16 + component], name, part, revision,
                        mac, letter, index, NULL, cpu_context);
}

/*
 * A synthetic JEDEC-style SPD. Only the length and size bytes, the checksum
 * the model validates, and a position marker are set: the assembly serial
 * bytes name the DIMM and the next byte the node board (a Cx-brick's; 0 on
 * a C-brick). Byte 31 is the rank density the Cx-brick supplies (0 on a
 * C-brick). The type and timing bytes are unknown and stay zero.
 */
static void l1_o3k_dimm(SGIL1EEPROM *record, unsigned board, unsigned dimm,
                        uint8_t density)
{
    uint8_t *spd = g_malloc0(128);
    uint8_t sum = 0;

    spd[0] = 128;
    spd[1] = 8;
    spd[31] = density;
    memcpy(spd + 95, "EMU", 3);
    spd[98] = dimm;
    spd[99] = board;
    for (unsigned i = 0; i < 63; i++) {
        sum += spd[i];
    }
    spd[63] = sum;
    record->length = 128;
    record->data = spd;
    record->invalid = false;
}

/*
 * The C-brick's records: the node board at component 0, the PIMMs at 1 and
 * 2, and DIMM n at 3 + (n >> 1) + 4 * (n & 1), the numbering the published
 * C-brick dump and one public driver agree on. The I-brick's, N-brick's
 * and V-brick's: the power board at 0 and the logic board at 1, which on an
 * I-brick carries the base I/O Ethernet address. A position not fitted has
 * no record.
 */
static bool l1_build_o3k_identity(SGIL1State *s, Error **errp)
{
    uint8_t mac[6];

    if (l1_is_vbrick(s)) {
        if (s->node_board_name || s->node_board_part ||
            s->node_board_revision) {
            error_setg(errp, "L1 node board identity is for the C-brick "
                       "profile only");
            return false;
        }
        /*
         * The X-brick personality's two records, the power board at
         * component 0 and the logic board at 1, which the stock PROM and
         * IRIX read for a brick that answers X. The logic board is named
         * XBRICK, the name that makes the firmware answer X. What else a
         * fitted V-brick's EEPROMs hold is unknown: the parts stay
         * unobserved and the serials synthetic, lettered as the I-brick's
         * power and I/O boards. No base I/O, so no address.
         */
        l1_o3k_board(s, 0, "POWER", "NOTOBSERVED0", "00", NULL, 'S',
                     s->identity_index, NULL);
        l1_o3k_board(s, 1, "XBRICK", "NOTOBSERVED0", "00", NULL, 'I',
                     s->identity_index, NULL);
        return true;
    }
    if (l1_is_ibrick(s)) {
        if (s->node_board_name || s->node_board_part ||
            s->node_board_revision) {
            error_setg(errp, "L1 node board identity is for the C-brick "
                       "profile only");
            return false;
        }
        if (l1_is_nbrick(s)) {
            /* A power board and a logic board; no base I/O, so no address. */
            l1_o3k_board(s, 0, "POWER", "NOTOBSERVED0", "00", NULL, 'S',
                         s->identity_index, NULL);
            l1_o3k_board(s, 1, "LOGIC", "NOTOBSERVED0", "00", NULL, 'N',
                         s->identity_index, NULL);
            return true;
        }
        if (net_parse_macaddr(mac, s->mac) < 0 ||
            !memcmp(mac, "\0\0\0\0\0\0", 6) || (mac[0] & 1)) {
            error_setg(errp, "L1 mac must be a nonzero unicast address");
            return false;
        }
        l1_o3k_board(s, 0, "POWER", "NOTOBSERVED0", "00", NULL, 'S',
                     s->identity_index, NULL);
        l1_o3k_board(s, 1, "LOGIC", "NOTOBSERVED0", "00", mac, 'I',
                     s->identity_index, NULL);
        memcpy(s->board_mac, mac, sizeof(s->board_mac));
        s->board_mac_valid = true;
        return true;
    }
    if ((s->node_board_name || s->node_board_part ||
         s->node_board_revision) &&
        (!s->node_board_name || !s->node_board_part ||
         !s->node_board_revision)) {
        error_setg(errp, "L1 node board identity needs a name, part and "
                   "revision");
        return false;
    }
    if (s->node_board_name &&
        (strlen(s->node_board_name) > 0x3f ||
         strlen(s->node_board_part) > 0x3f ||
         strlen(s->node_board_revision) > 0x3f)) {
        error_setg(errp, "L1 node board identity field is too long");
        return false;
    }
    /*
     * The common header's first byte counts toward the sum the existing
     * board-area builder keeps at zero, which is the rule the Origin 300
     * records follow; the printed C-brick header does not satisfy it, so the
     * model uses the Origin 300 rule.
     */
    if (l1_is_cxbrick(s)) {
        /*
         * One node board record per fitted board, board n's at slot n;
         * l1_cx_eeprom numbers them as the firmware's table does and makes
         * the DIMMs' SPDs on demand.
         */
        SGIL1CPUContext cpu_context = {
            .mode_word = s->cpu_mode_word,
            .cpu_mhz = s->cpu_clock_hz / 1000000,
            .hub_mhz = s->hub_clock_hz / 1000000,
        };

        for (unsigned board = 0; board < SGI_L1_CX_BOARDS; board++) {
            if (!l1_cx_fitted_board(s, board)) {
                continue;
            }
            l1_o3k_board(s, board,
                         s->node_board_name ? s->node_board_name : "NODE",
                         s->node_board_part ? s->node_board_part :
                                              "NOTOBSERVED0",
                         s->node_board_revision ? s->node_board_revision :
                                                  "00",
                         NULL, 'N', s->identity_index * 4 + board,
                         s->cpu_context_present ? &cpu_context : NULL);
        }
        return true;
    }
    l1_o3k_board(s, 0, s->node_board_name ? s->node_board_name : "NODE",
                 s->node_board_part ? s->node_board_part : "NOTOBSERVED0",
                 s->node_board_revision ? s->node_board_revision : "00",
                 NULL, 'N', s->identity_index, NULL);
    for (unsigned pimm = 0; pimm < 2; pimm++) {
        if (s->pimm_mask & (1u << pimm)) {
            l1_o3k_board(s, 1 + pimm, "PIMM", "NOTOBSERVED0", "00", NULL,
                         'D', s->identity_index * 2 + pimm, NULL);
        }
    }
    for (unsigned dimm = 0; dimm < 8; dimm++) {
        if (s->dimm_mask & (1u << dimm)) {
            l1_o3k_dimm(&s->records[32 + 3 + (dimm >> 1) + 4 * (dimm & 1)],
                        0, dimm, 0);
        }
    }
    return true;
}

/*
 * What the C-brick and I-brick profiles accept. Most refusals are inputs
 * whose meaning for these products is not established: a type code, the
 * Origin 300 compatibility modes and raw record files. Rack and slot start
 * unset unless @named says the properties name them. Every other profile
 * has both.
 */
static bool l1_o3k_check(SGIL1State *s, Error **errp)
{
    static const unsigned ports[] = { 1, 2, 3, 4, 5, 10 };
    bool io_known = false, c_known = false;

    if (s->router_board &&
        (strcmp(s->profile->name, "router") ||
         !l1_router_board(s->router_board))) {
        error_setg(errp, "L1 router-board must be rbrick, numalink-module or "
                   "cxbrick and needs the router profile");
        return false;
    }
    if (!s->profile->o3k) {
        if (s->named) {
            error_setg(errp, "L1 named applies to the Origin 3000 "
                       "profiles only");
            return false;
        }
        if (s->pimm_mask != 3 || s->dimm_mask != 0xff || s->io_port != 1 ||
            s->c_port != 5) {
            error_setg(errp, "L1 pimm-mask, dimm-mask, io-port and c-port "
                       "apply to the C-brick profile only");
            return false;
        }
        if (s->cx_boards != 1 || s->cx_compute != 1) {
            error_setg(errp, "L1 boards and compute-boards apply to the "
                       "cxbrick profile only");
            return false;
        }
        s->rack_valid = s->bay_valid = true;
        return true;
    }
    if (l1_is_cxbrick(s)) {
        if (s->cx_boards == 0 || s->cx_boards > 0xf) {
            error_setg(errp, "L1 cxbrick boards must name boards 0 to 3");
            return false;
        }
        if (s->cx_compute & ~s->cx_boards) {
            error_setg(errp, "L1 cxbrick compute-boards must be fitted "
                       "boards");
            return false;
        }
        if (s->pimm_mask != 3 || s->dimm_mask != 0xff || s->io_port != 1 ||
            s->c_port != 5 || s->mac) {
            error_setg(errp, "L1 pimm-mask, dimm-mask, io-port, c-port and "
                       "mac do not apply to the cxbrick profile");
            return false;
        }
        if (s->node_position) {
            error_setg(errp, "L1 cxbrick link positions are board numbers; "
                       "node-position does not apply");
            return false;
        }
        if (s->identity_index > 24) {
            error_setg(errp, "L1 cxbrick identity-index must be 0 to 24");
            return false;
        }
    } else if (s->cx_boards != 1 || s->cx_compute != 1) {
        error_setg(errp, "L1 boards and compute-boards apply to the "
                   "cxbrick profile only");
        return false;
    }
    /* A Cx-brick takes a type character only from this explicit input. */
    if (s->brick_type_code && !l1_is_cxbrick(s)) {
        error_setg(errp, "L1 brick-type-code is not accepted by the C-brick "
                   "and I-brick profiles");
        return false;
    }
    if (s->irouter_compat || s->o300_l1cmd_compat) {
        error_setg(errp, "L1 Origin 300 compatibility is not accepted by the "
                   "Origin 3000 profiles");
        return false;
    }
    if (s->chassis_count || s->board_count || s->spd_count ||
        s->board_record_count || s->spd_record_count || s->mac_record ||
        s->fuel_mac_eeprom) {
        error_setg(errp, "L1 Origin 3000 profiles take no record files");
        return false;
    }
    if (!l1_is_cxbrick(s) &&
        (s->cpu_context_present || s->cpu_mode_word || s->cpu_clock_hz ||
         s->hub_clock_hz)) {
        error_setg(errp, "L1 CPU context applies to the Origin 300 and "
                   "cxbrick profiles only");
        return false;
    }
    if (s->partition > 0xffff) {
        error_setg(errp, "L1 partition must fit 16 bits");
        return false;
    }
    if (l1_is_cxbrick(s)) {
        /* Checked above. */
    } else if (l1_is_vbrick(s)) {
        if (s->pimm_mask != 3 || s->dimm_mask != 0xff || s->io_port != 1 ||
            s->c_port != 5 || s->mac) {
            error_setg(errp, "L1 pimm-mask, dimm-mask, io-port, c-port and "
                       "mac do not apply to the vbrick profile");
            return false;
        }
    } else if (l1_is_ibrick(s)) {
        if (s->pimm_mask != 3 || s->dimm_mask != 0xff || s->io_port != 1 ||
            s->c_port != 5) {
            error_setg(errp, "L1 pimm-mask, dimm-mask, io-port and c-port "
                       "apply to the C-brick profile only");
            return false;
        }
        if (l1_is_nbrick(s) ? s->mac != NULL : s->mac == NULL) {
            error_setg(errp, l1_is_nbrick(s) ?
                       "L1 N-brick profile takes no mac" :
                       "L1 I-brick profile needs a mac");
            return false;
        }
    } else {
        if (s->mac) {
            error_setg(errp, "L1 mac is for the I-brick profile only");
            return false;
        }
        if (s->identity_index > 49) {
            error_setg(errp, "L1 C-brick identity-index must be 0 to 49");
            return false;
        }
        if (s->pimm_mask > 3) {
            error_setg(errp, "L1 pimm-mask names PIMM 0 and 1 only");
            return false;
        }
        if (s->dimm_mask > 0xff) {
            error_setg(errp, "L1 dimm-mask names DIMMs 0 to 7 only");
            return false;
        }
        for (unsigned i = 0; i < ARRAY_SIZE(ports); i++) {
            io_known |= s->io_port == ports[i];
            c_known |= s->c_port == ports[i];
        }
        if (!io_known || !c_known) {
            error_setg(errp, "L1 io-port and c-port must be 1, 2, 3, 4, 5 "
                       "or 10");
            return false;
        }
        if (s->io_port == s->c_port) {
            error_setg(errp, "L1 io-port and c-port must name different "
                       "ports");
            return false;
        }
    }
    s->rack_valid = s->bay_valid = s->named;
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

static bool l1_links_check(SGIL1State *s, Error **errp);

static void l1_pbay_log(void *opaque, const char *text)
{
    l1_log(opaque, "%s", text);
}

static void l1_realize(DeviceState *dev, Error **errp)
{
    SGIL1State *s = SGI_L1(dev);

    char **files[] = { s->chassis_files, s->board_files, s->spd_files };
    uint32_t counts[] = { s->chassis_count, s->board_count, s->spd_count };

    s->irouter_profile_identity = l1_irouter_profile_identity(s);
    if (s->l2_partition_flag > 3) {
        error_setg(errp, "l2-partition-flag is a two-bit value");
        return;
    }
    s->two_bit_setting = s->l2_partition_flag;

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
                   "unknown, pci-expansion, router, "
                   "vbrick-discovery-reference, cbrick, ibrick, cxbrick "
                   "or vbrick");
        return;
    }
    if (s->bic_startup && strcmp(s->profile->name, "pci-expansion")) {
        error_setg(errp, "L1 BIC startup requires the expansion profile");
        return;
    }
    s->product_identity = (s->profile - l1_profiles) + 1;
    if (!l1_o3k_check(s, errp)) {
        return;
    }
    if (!l1_links_check(s, errp)) {
        return;
    }
    if (!s->profile->compute_services) {
        /* An I-brick's "auto-power" stands for its C-brick's power-up. */
        if (!l1_is_ibrick(s)) {
            s->auto_power = false;
        }
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
        (s->board_serial && strlen(s->board_serial) > 32) ||
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
    if (s->profile->o3k ? !l1_build_o3k_identity(s, errp) :
                          !l1_build_identity(s, errp)) {
        return;
    }
    s->pending_rack = s->rack;
    s->pending_bay = s->bay;
    s->pending_rack_valid = s->rack_valid;
    s->pending_bay_valid = s->bay_valid;
    s->pending_partition = s->partition;
    s->power_up_bh = qemu_bh_new(l1_power_up_bh, s);
    qemu_register_reset(l1_after_node_reset, s);
    qemu_chr_fe_set_handlers(&s->chr, l1_console_can_receive,
                             l1_console_receive, l1_console_event, NULL,
                             s, NULL, true);
    s->l2_chain_tag = L1_L2_CHAIN_NONE;
    if (qemu_chr_fe_backend_connected(&s->l2_chr)) {
        /* The peer's state is outside the machine, so is not saved. */
        error_setg(&s->l2_blocker,
                   "sgi-l1: migration is not supported with an L2 link");
        if (migrate_add_blocker(&s->l2_blocker, errp) < 0) {
            return;
        }
        qemu_chr_fe_set_handlers(&s->l2_chr, l1_l2_can_receive,
                                 l1_l2_receive, l1_l2_event, NULL, s, NULL,
                                 true);
    }
    if (!sgi_l1_pbay_realize(&s->pbay, OBJECT(s), l1_brick_type_character(s),
                             l1_pbay_log, s, errp)) {
        return;
    }
    l1_boot(s);
    if (s->auto_power && l1_is_ibrick(s)) {
        /*
         * An I-brick has no auto power of its own: the C-brick's countdown
         * ends in "* power up", which reaches the I-brick. The I-brick is
         * built after its C-brick has already powered up, so it takes that
         * power-up here, as a peer of the broadcast.
         */
        l1_peer_power(s, true);
    } else if (s->auto_power) {
        /* The real countdown is skipped: a model choice. */
        l1_log(s, "auto power up initiated");
        g_autofree char *name = l1_module_name(s);

        l1_printf(s, "INFO: %s powering up the system.\r\n", name);
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
    DEFINE_PROP_CHR("l2", SGIL1State, l2_chr),
    DEFINE_PROP_CHR("pbay", SGIL1State, pbay.chr),
    DEFINE_PROP_UINT16("pbay-address", SGIL1State, pbay.l1_address, 0x0001),
    DEFINE_PROP_UINT32("pbay-poll-ms", SGIL1State, pbay.poll_ms, 1000),
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
    DEFINE_PROP_UINT8("l2-partition-flag", SGIL1State, l2_partition_flag, 0),
    DEFINE_PROP_STRING("system-serial", SGIL1State, system_serial),
    DEFINE_PROP_STRING("board-serial", SGIL1State, board_serial),
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
    DEFINE_PROP_STRING("router-board", SGIL1State, router_board),
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
    /*
     * The cbrick and ibrick profiles start with no rack and slot. named makes
     * rack and bay (above) their name; for any other profile it is refused.
     */
    DEFINE_PROP_BOOL("named", SGIL1State, named, false),
    /* cbrick: the PIMMs and DIMMs fitted, one bit each. */
    DEFINE_PROP_UINT32("pimm-mask", SGIL1State, pimm_mask, 3),
    DEFINE_PROP_UINT32("dimm-mask", SGIL1State, dimm_mask, 0xff),
    /*
     * cbrick: the attached-brick port the I-brick and the other C-brick
     * appear at in requests 0x000c and 0x000d and in cfg. The guides number
     * these differently by edition, so the numbering is configuration.
     */
    DEFINE_PROP_UINT32("io-port", SGIL1State, io_port, 1),
    DEFINE_PROP_UINT32("c-port", SGIL1State, c_port, 5),
    /*
     * cxbrick: the node boards fitted and those with processors, one bit
     * per board 0 to 3. Each fitted board takes a UART window whose
     * transport-index is its board number.
     */
    DEFINE_PROP_UINT32("boards", SGIL1State, cx_boards, 1),
    DEFINE_PROP_UINT32("compute-boards", SGIL1State, cx_compute, 1),
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

/*
 * What the profiles allow across each cable. A fault is the reason two
 * controllers may not be joined, or NULL. It needs both profiles, which a
 * controller has only from its own realize. A link therefore names a
 * realized controller, and is checked when it is set if the controller that
 * holds it is realized too, and otherwise when that controller realizes.
 * Whichever order a machine builds in, the step that completes the pair sees
 * both profiles.
 */
static const char *l1_peer_fault(const SGIL1State *a, const SGIL1State *b)
{
    if (!a->profile->o3k && !b->profile->o3k) {
        return NULL;
    }
    if (l1_is_cxbrick(a) || l1_is_cxbrick(b)) {
        return "L1 cxbrick profile models no peer link";
    }
    if (!l1_is_cbrick(a) || !l1_is_cbrick(b)) {
        return "L1 peer of a C-brick must be another C-brick";
    }
    return NULL;
}

static const char *l1_io_peer_fault(const SGIL1State *c,
                                    const SGIL1State *ibrick)
{
    if (!l1_is_cbrick(c) && !l1_is_cxbrick(c)) {
        return "L1 io-peer is for the C-brick and Cx-brick profiles only";
    }
    if (!l1_is_iobrick(ibrick)) {
        return "L1 io-peer must name an I-brick, N-brick or V-brick";
    }
    /*
     * The retained system's cabling of a second C-brick is unknown, so an
     * I-brick takes one; an N-brick has a connector for each of four. A
     * configuration that names two is refused where the second cable is
     * made, or at realize when it was made earlier.
     */
    if (!l1_is_nbrick(ibrick) && l1_host_of(ibrick, c)) {
        return "L1 I-brick takes one C-brick";
    }
    return NULL;
}

/*
 * The RS-422 pair between two controllers: a C-brick pair for the Origin 3000
 * products, never a C-brick with anything else. Other profiles keep the
 * link they always had.
 */
static void l1_peer_check(const Object *obj, const char *name, Object *val,
                          Error **errp)
{
    const SGIL1State *s = SGI_L1(OBJECT(obj));
    const SGIL1State *peer = val ? SGI_L1(val) : NULL;
    const char *fault;

    if (!peer) {
        return;
    }
    if (peer == s) {
        error_setg(errp, "L1 peer cannot be the controller itself");
    } else if (peer == s->io_peer) {
        error_setg(errp, "L1 peer must differ from the io-peer");
    } else if (!DEVICE(val)->realized) {
        error_setg(errp, "L1 peer joins realized controllers");
    } else if (DEVICE(obj)->realized) {
        fault = l1_peer_fault(s, peer);
        if (fault) {
            error_setg(errp, "%s", fault);
        }
    }
}

/*
 * A C-brick's I-brick. The second slot names a different controller from
 * the first and never the controller itself.
 */
static void l1_io_peer_check(const Object *obj, const char *name, Object *val,
                             Error **errp)
{
    const SGIL1State *s = SGI_L1(OBJECT(obj));
    const SGIL1State *ibrick = val ? SGI_L1(val) : NULL;
    const char *fault;

    if (!ibrick) {
        return;
    }
    if (ibrick == s) {
        error_setg(errp, "L1 io-peer cannot be the controller itself");
    } else if (ibrick == s->peer) {
        error_setg(errp, "L1 io-peer must differ from the peer");
    } else if (!DEVICE(val)->realized) {
        error_setg(errp, "L1 io-peer joins realized controllers");
    } else if (DEVICE(obj)->realized) {
        fault = l1_io_peer_fault(s, ibrick);
        if (fault) {
            error_setg(errp, "%s", fault);
        }
    }
}

/* Check links set before this controller realized, now its profile is known. */
static bool l1_links_check(SGIL1State *s, Error **errp)
{
    const char *fault = NULL;

    if (s->peer) {
        fault = l1_peer_fault(s, s->peer);
    }
    if (!fault && s->io_peer) {
        fault = l1_io_peer_fault(s, s->io_peer);
    }
    if (fault) {
        error_setg(errp, "%s", fault);
    }
    return !fault;
}

static void l1_init(Object *obj)
{
    SGIL1State *s = SGI_L1(obj);

    l1_controllers = g_slist_prepend(l1_controllers, s);
    sgi_l1_pbay_init(obj, &s->pbay);
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
    qdev_init_gpio_out_named(DEVICE(obj), s->nmi_out, "nmi",
                             SGI_L1_CX_BOARDS);
    object_property_add_uint8_ptr(obj, "event-subchannel", &s->event_subchannel,
                                  OBJ_PROP_FLAG_READ);
    /*
     * Settable after realize: each chassis's node realizes its own L1, so the
     * machine can only join the two once both exist.
    */
    object_property_add_link(obj, "peer", TYPE_SGI_L1, (Object **)&s->peer,
                             l1_peer_check, 0);
    object_property_add_link(obj, "io-peer", TYPE_SGI_L1,
                             (Object **)&s->io_peer, l1_io_peer_check, 0);
    /*
     * The graphics brick on the I/O brick's other Crosstown2 port, set with
     * the io-peer link once the machine knows both.
     */
    object_property_add_uint32_ptr(obj, "io-gfx-bay", &s->io_gfx_bay,
                                   OBJ_PROP_FLAG_READWRITE);
}

static bool l1_l2_link_up_get(Object *obj, Error **errp)
{
    return SGI_L1(obj)->l2_up;
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
    object_class_property_add_bool(klass, "l2-link-up", l1_l2_link_up_get,
                                   NULL);
    object_class_property_add_str(klass, "display-line-1", l1_display_first_get,
                                  NULL);
    object_class_property_add_str(klass, "display-line-2",
                                  l1_display_get, NULL);
}

static void l1_finalize(Object *obj)
{
    SGIL1State *s = SGI_L1(obj);

    l1_controllers = g_slist_remove(l1_controllers, s);
    if (s->l2_blocker) {
        migrate_del_blocker(&s->l2_blocker);
    }
    sgi_l1_pbay_finalize(&s->pbay);
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
    g_free(s->board_serial);
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
    if (DEVICE(u->controller)->realized &&
        l1_is_cxbrick(u->controller) &&
        !l1_cx_fitted_board(u->controller, u->transport_index)) {
        error_setg(errp, "L1 UART transport index names no fitted Cx-brick "
                   "board");
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
