/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Wacom IV UD-series tablet chardev (hw/input/wacom-iv.c), driven through
 * Malta's 16550 the way a guest driver talks to it: program the line, write
 * commands, read replies and packets, and feed the tablet pointer events
 * over QMP.
 *
 * The expected bytes come from the WTC-1002 manual (the command replies of
 * chapter 3, the ROM 1.2 binary packet of section 4.1 and the power-up state
 * of appendix C.2), written out as literals rather than computed by the
 * model's own formulas.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define COM1        0x3f8
#define UART_RBR    (COM1 + 0)
#define UART_THR    (COM1 + 0)
#define UART_DLL    (COM1 + 0)
#define UART_DLM    (COM1 + 1)
#define UART_FCR    (COM1 + 2)
#define UART_LCR    (COM1 + 3)
#define UART_LSR    (COM1 + 5)

#define LSR_DR      0x01
#define LCR_DLAB    0x80
#define LCR_8N1     0x03
#define LCR_8E1     0x1b

/* The 16550 clocks at 115200 baud / divisor. */
#define DIV_9600    12
#define DIV_19200   6

#define PACKET_LEN  7

static QTestState *start(void)
{
    return qtest_init("-M malta -display none -chardev wacom-iv,id=tab "
                      "-serial chardev:tab");
}

static void line(QTestState *qts, int divisor, uint8_t lcr)
{
    qtest_outb(qts, UART_LCR, LCR_DLAB);
    qtest_outb(qts, UART_DLL, divisor & 0xff);
    qtest_outb(qts, UART_DLM, divisor >> 8);
    qtest_outb(qts, UART_LCR, lcr);
    qtest_outb(qts, UART_FCR, 0x07);
}

static void tx(QTestState *qts, const char *s)
{
    for (; *s; s++) {
        qtest_outb(qts, UART_THR, *s);
        /* A character takes about 1 ms at 9600 baud. */
        qtest_clock_step(qts, 2 * 1000 * 1000);
    }
}

/* Whatever the tablet has sent so far, up to cap bytes. */
static int drain(QTestState *qts, uint8_t *buf, int cap)
{
    int n = 0;

    qtest_clock_step(qts, 100 * 1000 * 1000);
    while (n < cap && (qtest_inb(qts, UART_LSR) & LSR_DR)) {
        buf[n++] = qtest_inb(qts, UART_RBR);
    }
    return n;
}

static void expect_bytes(QTestState *qts, const char *want)
{
    uint8_t buf[64];
    int n = drain(qts, buf, sizeof(buf));

    g_assert_cmpint(n, ==, strlen(want));
    g_assert(!memcmp(buf, want, n));
}

static void expect_silence(QTestState *qts)
{
    uint8_t buf[64];

    g_assert_cmpint(drain(qts, buf, sizeof(buf)), ==, 0);
}

static void expect_packet(QTestState *qts, const uint8_t *want)
{
    uint8_t buf[64];
    int n = drain(qts, buf, sizeof(buf));

    g_assert_cmpint(n, ==, PACKET_LEN);
    for (int i = 0; i < PACKET_LEN; i++) {
        g_assert_cmphex(buf[i], ==, want[i]);
    }
}

/* Returns whether the console took the event; a PS/2 mouse refuses it. */
static bool try_move(QTestState *qts, int x, int y)
{
    bool ok;
    QDict *r = qtest_qmp(qts, "{'execute': 'input-send-event', 'arguments':"
                         " {'events': ["
                         "{'type': 'abs', 'data': {'axis': 'x', 'value': %d}},"
                         "{'type': 'abs', 'data': {'axis': 'y', 'value': %d}}"
                         "]}}", x, y);
    ok = qdict_haskey(r, "return");
    qobject_unref(r);
    return ok;
}

static void move(QTestState *qts, int x, int y)
{
    g_assert(try_move(qts, x, y));
}

static void tip(QTestState *qts, bool down)
{
    QDict *r = qtest_qmp(qts, "{'execute': 'input-send-event', 'arguments':"
                         " {'events': [{'type': 'btn', 'data':"
                         " {'button': 'left', 'down': %i}}]}}", down);
    g_assert(qdict_haskey(r, "return"));
    qobject_unref(r);
}

/* Is the tablet on the console's pointer list, and the current one? */
static bool tablet_is_current(QTestState *qts, bool *listed)
{
    QDict *r = qtest_qmp(qts, "{'execute': 'query-mice'}");
    QList *mice = qdict_get_qlist(r, "return");
    bool current = false;

    *listed = false;
    for (const QListEntry *e = qlist_first(mice); e; e = qlist_next(e)) {
        QDict *m = qobject_to(QDict, qlist_entry_obj(e));

        if (!strcmp(qdict_get_str(m, "name"), "QEMU Wacom IV Tablet")) {
            *listed = true;
            current = qdict_get_bool(m, "current");
            g_assert(qdict_get_bool(m, "absolute"));
        }
    }
    qobject_unref(r);
    return current;
}

/* A driver's open: set the line and ask which tablet this is. */
static void open_tablet(QTestState *qts)
{
    line(qts, DIV_9600, LCR_8N1);
    tx(qts, "~#");
    expect_bytes(qts, "~#UD-1212-R00 V1.3-5\r");
}

static void test_identity(void)
{
    QTestState *qts = start();

    open_tablet(qts);
    tx(qts, "\r~C\r");
    expect_bytes(qts, "~C15240,15240\r");
    /* Appendix C.2's answer for a UD tablet, ROM 1.3. */
    tx(qts, "~R\r");
    expect_bytes(qts, "~RE203C000,000,02,1270,1270\r");
    qtest_quit(qts);
}

static void test_reset_is_silent(void)
{
    QTestState *qts = start();

    open_tablet(qts);
    tx(qts, "\rRE\r");
    expect_silence(qts);
    /* Nothing was changed by it either. */
    tx(qts, "~R\r");
    expect_bytes(qts, "~RE203C000,000,02,1270,1270\r");
    qtest_quit(qts);
}

static void test_rewrite_and_reset(void)
{
    QTestState *qts = start();

    open_tablet(qts);
    /* ~* gives no reply; ~R shows what it wrote, RE restores power-up. */
    tx(qts, "\r~*E202C000\r");
    expect_silence(qts);
    tx(qts, "~R\r");
    expect_bytes(qts, "~RE202C000,000,02,1270,1270\r");
    tx(qts, "SU3\rNR2540\r~R\r");
    expect_bytes(qts, "~RE202C000,003,02,2540,2540\r");
    tx(qts, "RE\r~R\r");
    expect_bytes(qts, "~RE203C000,000,02,1270,1270\r");
    qtest_quit(qts);
}

static void test_streams_from_power_up(void)
{
    QTestState *qts = start();
    bool listed;
    /*
     * x = 16384/32767 of 15240 is 7620 = 0x1dc4; y = 0. Pen up: switch 0,
     * pressure -120 = 0x88, so P0 = 0 and byte 7 = Sp 1, P6..P1 = 000100.
     */
    const uint8_t want[PACKET_LEN] = { 0xe0, 0x3b, 0x44, 0x00, 0x00, 0x00,
                                       0x44 };

    /* No driver yet: PS/2 keeps the pointer. */
    g_assert(!tablet_is_current(qts, &listed));
    g_assert(!listed);
    open_tablet(qts);
    /* No ST was sent. */
    g_assert(tablet_is_current(qts, &listed));
    g_assert(listed);

    move(qts, 16384, 0);
    expect_packet(qts, want);
    qtest_quit(qts);
}

static void test_pen_down(void)
{
    QTestState *qts = start();
    /* Tip switch 1, flag set, pressure +120 = 0x78: byte 7 = 0x3c. */
    const uint8_t down[PACKET_LEN] = { 0xe8, 0x3b, 0x44, 0x08, 0x00, 0x00,
                                       0x3c };
    const uint8_t up[PACKET_LEN] = { 0xe0, 0x3b, 0x44, 0x00, 0x00, 0x00,
                                     0x44 };

    open_tablet(qts);
    move(qts, 16384, 0);
    expect_packet(qts, up);

    /* A switch change sends a packet at an unchanged position. */
    tip(qts, true);
    expect_packet(qts, down);
    tip(qts, false);
    expect_packet(qts, up);
    qtest_quit(qts);
}

static void test_still_pointer_is_silent(void)
{
    QTestState *qts = start();
    const uint8_t want[PACKET_LEN] = { 0xe0, 0x3b, 0x44, 0x00, 0x00, 0x00,
                                       0x44 };

    open_tablet(qts);
    move(qts, 16384, 0);
    expect_packet(qts, want);
    move(qts, 16384, 0);
    expect_silence(qts);
    qtest_quit(qts);
}

/*
 * NR sets the logical resolution in lines per inch, and ~C gives the
 * maximum at 0.02 mm, which is 1270 lpi (WTC-1002 p. 11 and 21). At NR2540
 * the far edge is 30480 = 0x7710 and the middle of the other axis 15240 =
 * 0x3b88; NR1270 puts back 15240 and 7619. Out-of-range resolutions from
 * NR or ~* leave the old one, and # restores 1270.
 */
static void test_resolution_scales_coordinates(void)
{
    QTestState *qts = start();
    const uint8_t fine[PACKET_LEN] = { 0xe1, 0x6e, 0x10, 0x00, 0x77, 0x08,
                                       0x44 };
    const uint8_t coarse[PACKET_LEN] = { 0xe0, 0x77, 0x08, 0x00, 0x3b, 0x43,
                                         0x44 };

    open_tablet(qts);
    tx(qts, "\rNR2540\r");
    move(qts, 32767, 16384);
    expect_packet(qts, fine);
    tx(qts, "NR1270\r");
    move(qts, 32767, 16383);
    expect_packet(qts, coarse);
    /* Above 2540 the command is not taken. */
    tx(qts, "NR9000\r~R\r");
    expect_bytes(qts, "~RE203C000,000,02,1270,1270\r");
    /* A Setting rewrite takes each axis' resolution by the same rule. */
    tx(qts, "~*E203C000,000,02,0000,9999\r~R\r");
    expect_bytes(qts, "~RE203C000,000,02,1270,1270\r");
    tx(qts, "~*E203C000,000,02,2540,1270\r~R\r");
    expect_bytes(qts, "~RE203C000,000,02,2540,1270\r");
    /* The # reset puts back the factory 1270 on both axes. */
    tx(qts, "NR2540\r#~R\r");
    expect_bytes(qts, "~RE203C000,000,02,1270,1270\r");
    qtest_quit(qts);
}

static void test_increment(void)
{
    QTestState *qts = start();
    uint8_t buf[64];

    open_tablet(qts);
    tx(qts, "\rSU3\r");
    move(qts, 16384, 0);
    g_assert_cmpint(drain(qts, buf, sizeof(buf)), ==, PACKET_LEN);
    /* About two counts: inside the increment. */
    move(qts, 16389, 0);
    expect_silence(qts);
    /* About 9 counts: past it. */
    move(qts, 16404, 0);
    g_assert_cmpint(drain(qts, buf, sizeof(buf)), ==, PACKET_LEN);
    qtest_quit(qts);
}

static void test_interval(void)
{
    QTestState *qts = start();
    uint8_t buf[64];

    open_tablet(qts);
    /* IT50 is one packet per 250 ms. */
    tx(qts, "\rIT50\r");
    move(qts, 16384, 0);
    g_assert_cmpint(drain(qts, buf, sizeof(buf)), ==, PACKET_LEN);
    move(qts, 20000, 0);
    /* drain() steps 100 ms: still inside the interval. */
    g_assert_cmpint(drain(qts, buf, sizeof(buf)), ==, 0);
    qtest_clock_step(qts, 300 * 1000 * 1000);
    g_assert_cmpint(drain(qts, buf, sizeof(buf)), ==, PACKET_LEN);
    qtest_quit(qts);
}

static void test_stop_and_start(void)
{
    QTestState *qts = start();
    uint8_t buf[64];
    bool listed;

    open_tablet(qts);
    tx(qts, "\rSP\r");
    expect_silence(qts);
    /* SP lets go of the pointer and stops output. */
    g_assert(!tablet_is_current(qts, &listed));
    g_assert(!listed);
    /* With no absolute pointer left, the console refuses the event. */
    g_assert(!try_move(qts, 16384, 0));
    expect_silence(qts);

    tx(qts, "ST\r");
    g_assert(tablet_is_current(qts, &listed));
    move(qts, 8000, 8000);
    g_assert_cmpint(drain(qts, buf, sizeof(buf)), ==, PACKET_LEN);

    /* RE ends a stop too: the tablet transmits again from reset. */
    tx(qts, "SP\r");
    tx(qts, "RE\r");
    g_assert(tablet_is_current(qts, &listed));
    move(qts, 9000, 9000);
    g_assert_cmpint(drain(qts, buf, sizeof(buf)), ==, PACKET_LEN);
    qtest_quit(qts);
}

static void test_wrong_line_is_silent(void)
{
    QTestState *qts = start();
    bool listed;

    line(qts, DIV_19200, LCR_8N1);
    tx(qts, "~#");
    expect_silence(qts);
    g_assert(!tablet_is_current(qts, &listed));

    line(qts, DIV_9600, LCR_8E1);
    tx(qts, "~#");
    expect_silence(qts);
    g_assert(!tablet_is_current(qts, &listed));

    /* Back at 9600 8N1 it answers again; the earlier bytes were lost. */
    line(qts, DIV_9600, LCR_8N1);
    tx(qts, "~#");
    expect_bytes(qts, "~#UD-1212-R00 V1.3-5\r");
    qtest_quit(qts);
}

/*
 * The Discreet driver's open: a Setting rewrite, RE, ~C, ~#, then OC1, IT2,
 * SU1 and NR1270, and no ST. The answers come in order and the tablet
 * transmits.
 */
static void test_discreet_driver_sequence(void)
{
    QTestState *qts = start();
    bool listed;
    const uint8_t want[PACKET_LEN] = { 0xe0, 0x3b, 0x44, 0x00, 0x00, 0x00,
                                       0x44 };

    line(qts, DIV_9600, LCR_8N1);
    tx(qts, "~*E202C000\rRE\r~C\r~#");
    tx(qts, "\rOC1\rIT2\rSU1\rNR1270\r");
    expect_bytes(qts, "~C15240,15240\r~#UD-1212-R00 V1.3-5\r");
    g_assert(tablet_is_current(qts, &listed));
    move(qts, 16384, 0);
    expect_packet(qts, want);
    qtest_quit(qts);
}

/*
 * The stock IRIX 6.5.30 module's open, as a guest run traced it: queries at
 * 9600 baud, a Setting rewrite to 19200 baud (F2 in the body's top byte,
 * WTC-1002 Appendix B), then the rest of the set-up and ST at 19200; then
 * its reopen, which starts with the "$" global reset.
 */
static void test_irix_module_sequence(void)
{
    QTestState *qts = start();
    bool listed;
    uint8_t buf[64];

    line(qts, DIV_9600, LCR_8N1);
    tx(qts, "SP\r~#");
    expect_bytes(qts, "~#UD-1212-R00 V1.3-5\r");
    g_assert(!tablet_is_current(qts, &listed));
    tx(qts, "SP\r~C\r");
    expect_bytes(qts, "~C15240,15240\r");
    tx(qts, "SP\r~*F203C000\r");
    expect_silence(qts);
    /* The tablet now listens at 19200 only. */
    tx(qts, "~#");
    expect_silence(qts);
    line(qts, DIV_19200, LCR_8N1);
    tx(qts, "SP\rAS1\rPH1\rDE0\rHC0\rSP\rSP\r~M1\r~M5\rNR2540\rSU5\rST\r");
    expect_silence(qts);
    g_assert(tablet_is_current(qts, &listed));
    move(qts, 16384, 0);
    g_assert_cmpint(drain(qts, buf, sizeof(buf)), ==, PACKET_LEN);
    tx(qts, "~R\r");
    expect_bytes(qts, "~RF203C000,005,02,2540,2540\r");
    /*
     * Reopening, the module sends "$" at 19200: the WACOM II-S reset puts
     * back the factory 9600 line (Appendix C.4), and the tablet answers
     * queries there but sends no coordinates until a Setting selects
     * WACOM IV again.
     */
    tx(qts, "$");
    expect_silence(qts);
    g_assert(!tablet_is_current(qts, &listed));
    line(qts, DIV_9600, LCR_8N1);
    tx(qts, "SP\r~#");
    expect_bytes(qts, "~#UD-1212-R00 V1.3-5\r");
    tx(qts, "~R\r");
    expect_bytes(qts, "~RA21BC800,000,00,1270,1270\r");
    g_assert(!tablet_is_current(qts, &listed));
    tx(qts, "#SP\r~C\rSP\r~*F203C000\r");
    expect_bytes(qts, "~C15240,15240\r");
    line(qts, DIV_19200, LCR_8N1);
    tx(qts, "ST\r");
    g_assert(tablet_is_current(qts, &listed));
    move(qts, 16384, 16384);
    g_assert_cmpint(drain(qts, buf, sizeof(buf)), ==, PACKET_LEN);

    /* RE puts back the factory line. */
    tx(qts, "RE\r");
    expect_silence(qts);
    g_assert(!tablet_is_current(qts, &listed));
    line(qts, DIV_9600, LCR_8N1);
    tx(qts, "~#");
    expect_bytes(qts, "~#UD-1212-R00 V1.3-5\r");
    qtest_quit(qts);
}

/*
 * The Linux driver's open: global reset, the three queries, then a setup
 * that ends in ST. ST on a tablet that is already sending changes nothing.
 */
static void test_linux_driver_sequence(void)
{
    QTestState *qts = start();
    bool listed;
    uint8_t buf[64];

    line(qts, DIV_9600, LCR_8N1);
    tx(qts, "\r#");
    expect_silence(qts);
    tx(qts, "~#");
    expect_bytes(qts, "~#UD-1212-R00 V1.3-5\r");
    tx(qts, "~R\r");
    expect_bytes(qts, "~RE203C000,000,02,1270,1270\r");
    tx(qts, "~C\r");
    expect_bytes(qts, "~C15240,15240\r");
    tx(qts, "OC1\rIT0\rIN0\rSR\rPH1\rST\r");
    expect_silence(qts);
    g_assert(tablet_is_current(qts, &listed));
    move(qts, 16384, 0);
    g_assert_cmpint(drain(qts, buf, sizeof(buf)), ==, PACKET_LEN);
    qtest_quit(qts);
}

/*
 * RQ1 and "@" send one coordinate pair whatever the suppression rules say
 * and leave the tablet answering requests only, until ST.
 */
static void test_remote_request(void)
{
    QTestState *qts = start();
    bool listed;
    uint8_t buf[64];
    const uint8_t origin[PACKET_LEN] = { 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x44 };
    const uint8_t moved[PACKET_LEN] = { 0xe0, 0x3b, 0x44, 0x00, 0x00, 0x00,
                                        0x44 };

    open_tablet(qts);
    /* A still pointer sends nothing by itself, but a request is answered. */
    tx(qts, "\rRQ1\r");
    expect_packet(qts, origin);
    /* In remote request mode movement is not sent. */
    move(qts, 16384, 0);
    expect_silence(qts);
    tx(qts, "@");
    expect_packet(qts, moved);
    /* ST ends the mode: movement is sent again. */
    tx(qts, "ST\r");
    move(qts, 20000, 0);
    g_assert_cmpint(drain(qts, buf, sizeof(buf)), ==, PACKET_LEN);

    /* RQ also ends an SP, and the pointer comes back with it. */
    tx(qts, "SP\r");
    g_assert(!tablet_is_current(qts, &listed));
    tx(qts, "RQ1\r");
    g_assert_cmpint(drain(qts, buf, sizeof(buf)), ==, PACKET_LEN);
    g_assert(tablet_is_current(qts, &listed));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/wacom-iv/identity", test_identity);
    qtest_add_func("/wacom-iv/reset-is-silent", test_reset_is_silent);
    qtest_add_func("/wacom-iv/rewrite-and-reset", test_rewrite_and_reset);
    qtest_add_func("/wacom-iv/streams-from-power-up",
                   test_streams_from_power_up);
    qtest_add_func("/wacom-iv/pen-down", test_pen_down);
    qtest_add_func("/wacom-iv/still-pointer-is-silent",
                   test_still_pointer_is_silent);
    qtest_add_func("/wacom-iv/resolution-scales-coordinates",
                   test_resolution_scales_coordinates);
    qtest_add_func("/wacom-iv/increment", test_increment);
    qtest_add_func("/wacom-iv/interval", test_interval);
    qtest_add_func("/wacom-iv/stop-and-start", test_stop_and_start);
    qtest_add_func("/wacom-iv/remote-request", test_remote_request);
    qtest_add_func("/wacom-iv/discreet-driver-sequence",
                   test_discreet_driver_sequence);
    qtest_add_func("/wacom-iv/irix-module-sequence",
                   test_irix_module_sequence);
    qtest_add_func("/wacom-iv/linux-driver-sequence",
                   test_linux_driver_sequence);
    qtest_add_func("/wacom-iv/wrong-line-is-silent",
                   test_wrong_line_is_silent);

    return g_test_run();
}
