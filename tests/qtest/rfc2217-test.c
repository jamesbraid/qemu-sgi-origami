/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * rfc2217 chardev: Malta's 16550 sits on an rfc2217 chardev over a unix
 * socket this test serves as the RFC 2217 access server. The expected bytes
 * are the commands of RFC 2217 sections 2 and 3 written out as literals.
 */

#include "qemu/osdep.h"
#include <poll.h>
#include "qemu/sockets.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define COM1        0x3f8
#define UART_RBR    (COM1 + 0)
#define UART_THR    (COM1 + 0)
#define UART_DLL    (COM1 + 0)
#define UART_DLM    (COM1 + 1)
#define UART_FCR    (COM1 + 2)
#define UART_LCR    (COM1 + 3)
#define UART_MCR    (COM1 + 4)
#define UART_LSR    (COM1 + 5)
#define UART_MSR    (COM1 + 6)

#define LSR_DR      0x01
#define LCR_DLAB    0x80
#define POLL_MS     5000

#define HELLO       "\xff\xfb\x2c" "\xff\xfb\x00" "\xff\xfd\x00"
#define CTL(v)      "\xff\xfa\x2c\x05" v "\xff\xf0"
#define DTR_OFF     CTL("\x09")
#define RTS_OFF     CTL("\x0c")
#define BRK_OFF     CTL("\x06")
#define BAUD(b)     "\xff\xfa\x2c\x01" b "\xff\xf0"
#define SIZE(v)     "\xff\xfa\x2c\x02" v "\xff\xf0"
#define PARITY(v)   "\xff\xfa\x2c\x03" v "\xff\xf0"
#define STOP(v)     "\xff\xfa\x2c\x04" v "\xff\xf0"
#define PARAMS(b, sz, par, stop) BAUD(b) SIZE(sz) PARITY(par) STOP(stop)

/*
 * The 16550 reports its line after every LCR and divisor write, so a
 * programming sequence reaches the server as a series of full settings. At
 * reset the divisor is 12 (9600 baud) and LCR is 8N1.
 */
#define B9600       "\x00\x00\x25\x80"
#define B19200      "\x00\x00\x4b\x00"
/* line(12, 0x03): DLAB set, divisor written twice, then 8N1. */
#define SET_9600_8N1 \
    PARAMS(B9600, "\x05", "\x01", "\x01") \
    PARAMS(B9600, "\x05", "\x01", "\x01") \
    PARAMS(B9600, "\x05", "\x01", "\x01") \
    PARAMS(B9600, "\x08", "\x01", "\x01")

typedef struct Rig Rig;
static void peer_expect(Rig *r, const char *want, size_t len);
#define PEER_EXPECT(r, s) peer_expect(r, s, sizeof(s) - 1)

struct Rig {
    QTestState *qts;
    char *dir;
    char *path;
    int peer;
};

/* Two round trips: QEMU has accepted the peer by the second. */
static void settle(Rig *r)
{
    qobject_unref(qtest_qmp(r->qts, "{'execute': 'query-status'}"));
    qobject_unref(qtest_qmp(r->qts, "{'execute': 'query-status'}"));
}

static void rig_connect(Rig *r)
{
    r->peer = unix_connect(r->path, NULL);
    g_assert_cmpint(r->peer, >=, 0);
    settle(r);
}

static Rig *rig_start(void)
{
    Rig *r = g_new0(Rig, 1);
    g_autofree char *args = NULL;

    r->dir = g_dir_make_tmp("rfc2217-XXXXXX", NULL);
    g_assert(r->dir);
    r->path = g_build_filename(r->dir, "line.sock", NULL);
    args = g_strdup_printf("-M malta -display none "
                           "-chardev socket,id=s,path=%s,server=on,wait=off "
                           "-chardev rfc2217,id=line,chardev=s "
                           "-serial chardev:line", r->path);
    r->qts = qtest_init(args);
    rig_connect(r);
    /* Every test starts after the opening negotiation. */
    PEER_EXPECT(r, HELLO DTR_OFF RTS_OFF BRK_OFF);
    return r;
}

static void rig_stop(Rig *r)
{
    qtest_quit(r->qts);
    close(r->peer);
    unlink(r->path);
    rmdir(r->dir);
    g_free(r->path);
    g_free(r->dir);
    g_free(r);
}

static void line(QTestState *qts, int divisor, uint8_t lcr)
{
    qtest_outb(qts, UART_LCR, LCR_DLAB);
    qtest_outb(qts, UART_DLL, divisor & 0xff);
    qtest_outb(qts, UART_DLM, divisor >> 8);
    qtest_outb(qts, UART_LCR, lcr);
    qtest_outb(qts, UART_FCR, 0x07);
}

static void tx(QTestState *qts, uint8_t c)
{
    qtest_outb(qts, UART_THR, c);
    /* A character takes about 1 ms at 9600 baud. */
    qtest_clock_step(qts, 2 * 1000 * 1000);
}

/* The next len bytes the peer receives must be exactly want. */
static void peer_expect(Rig *r, const char *want, size_t len)
{
    g_autofree uint8_t *got = g_malloc(len);
    size_t n = 0;

    while (n < len) {
        struct pollfd p = { .fd = r->peer, .events = POLLIN };
        ssize_t got_n;

        g_assert_cmpint(poll(&p, 1, POLL_MS), ==, 1);
        got_n = read(r->peer, got + n, len - n);
        g_assert_cmpint(got_n, >, 0);
        n += got_n;
    }
    if (memcmp(got, want, len)) {
        g_autofree char *g = g_strdup("");
        g_autofree char *w = g_strdup("");

        for (size_t i = 0; i < len; i++) {
            char *t = g_strdup_printf("%s%02x ", g, got[i]);
            g_free(g);
            g = t;
            t = g_strdup_printf("%s%02x ", w, (uint8_t)want[i]);
            g_free(w);
            w = t;
        }
        g_test_message("got  %s", g);
        g_test_message("want %s", w);
        g_assert_not_reached();
    }
}

/* Nothing more is waiting for the peer. */
static void peer_idle(Rig *r)
{
    struct pollfd p = { .fd = r->peer, .events = POLLIN };

    /* Let QEMU run so anything it means to send has been sent. */
    qtest_clock_step(r->qts, 10 * 1000 * 1000);
    settle(r);
    g_assert_cmpint(poll(&p, 1, 100), ==, 0);
}

static void peer_say(Rig *r, const char *s, size_t len)
{
    g_assert_cmpint(write(r->peer, s, len), ==, len);
}

static int rx(QTestState *qts, uint8_t *buf, int want)
{
    int n = 0;

    for (int i = 0; i < POLL_MS / 10 && n < want; i++) {
        qtest_clock_step(qts, 10 * 1000 * 1000);
        while (n < want && (qtest_inb(qts, UART_LSR) & LSR_DR)) {
            buf[n++] = qtest_inb(qts, UART_RBR);
        }
        if (n < want) {
            g_usleep(10 * 1000);
        }
    }
    return n;
}

static void test_announces_and_sets_line(void)
{
    Rig *r = rig_start();

    peer_idle(r);
    line(r->qts, 12, 0x03);
    PEER_EXPECT(r, SET_9600_8N1);
    /* 19200 baud, 7 data bits, even parity, 2 stop bits. */
    line(r->qts, 6, 0x1e);
    PEER_EXPECT(r, PARAMS(B9600, "\x05", "\x01", "\x01")
                   PARAMS(B19200, "\x05", "\x01", "\x01")
                   PARAMS(B19200, "\x05", "\x01", "\x01")
                   PARAMS(B19200, "\x07", "\x03", "\x02"));
    peer_idle(r);
    rig_stop(r);
}

static void test_break_and_modem_lines(void)
{
    Rig *r = rig_start();

    qtest_outb(r->qts, UART_LCR, 0x43);
    PEER_EXPECT(r, PARAMS(B9600, "\x08", "\x01", "\x01") CTL("\x05"));
    qtest_outb(r->qts, UART_LCR, 0x03);
    PEER_EXPECT(r, PARAMS(B9600, "\x08", "\x01", "\x01") CTL("\x06"));
    qtest_outb(r->qts, UART_MCR, 0x01);
    PEER_EXPECT(r, CTL("\x08"));
    qtest_outb(r->qts, UART_MCR, 0x03);
    PEER_EXPECT(r, CTL("\x0b"));
    peer_idle(r);
    rig_stop(r);
}

/* Data bytes of 0xff are doubled; the server's option traffic is dropped. */
static void test_data_is_escaped_both_ways(void)
{
    static const char down[] = "AB\xff\xff" "C" "\xff\xfd\x2c"
                               "\xff\xfa\x2c\x6b\x30\xff\xf0" "D";
    uint8_t buf[16];
    Rig *r = rig_start();

    line(r->qts, 12, 0x03);
    PEER_EXPECT(r, SET_9600_8N1);
    tx(r->qts, 0xff);
    PEER_EXPECT(r, "\xff\xff");
    peer_say(r, down, sizeof(down) - 1);
    g_assert_cmpint(rx(r->qts, buf, 5), ==, 5);
    g_assert(!memcmp(buf, "AB\xff" "CD", 5));
    rig_stop(r);
}

/* We answered the Com Port and binary offers; refuse the rest, once. */
static void test_server_options(void)
{
    Rig *r = rig_start();

    peer_say(r, "\xff\xfd\x01", 3);
    PEER_EXPECT(r, "\xff\xfc\x01");
    peer_say(r, "\xff\xfb\x03", 3);
    PEER_EXPECT(r, "\xff\xfe\x03");
    peer_say(r, "\xff\xfb\x2c", 3);
    PEER_EXPECT(r, "\xff\xfd\x2c");
    /* Answers to our offers and refusals need no reply. */
    peer_say(r, "\xff\xfd\x00\xff\xfb\x00\xff\xfd\x2c\xff\xfc\x01"
                "\xff\xfe\x03", 15);
    peer_idle(r);
    rig_stop(r);
}

/* A subnegotiation split across reads does not leak into the data. */
static void test_split_subnegotiation(void)
{
    static const char *const parts[] = {
        "A\xff", "\xfa\x2c", "\x6b\x30\xff", "\xf0", "B\xff", "\xff",
        "\xff\xfa\x2c\x6b\xff\xff\xff\xf0" "C",
    };
    uint8_t buf[8];
    Rig *r = rig_start();

    line(r->qts, 12, 0x03);
    PEER_EXPECT(r, SET_9600_8N1);
    for (int i = 0; i < ARRAY_SIZE(parts); i++) {
        peer_say(r, parts[i], strlen(parts[i]));
        qtest_clock_step(r->qts, 10 * 1000 * 1000);
        settle(r);
    }
    g_assert_cmpint(rx(r->qts, buf, 4), ==, 4);
    g_assert(!memcmp(buf, "AB\xff" "C", 4));
    rig_stop(r);
}

/* A 0xff in a command value is doubled: 255 baud is 00 00 00 ff. */
static void test_ff_in_value(void)
{
    Rig *r = rig_start();

    line(r->qts, 451, 0x03);
    /* Divisor 0xc3, 0x1c3, so 590 baud on the way to 255. */
    PEER_EXPECT(r, PARAMS(B9600, "\x05", "\x01", "\x01")
                   PARAMS("\x00\x00\x02\x4e", "\x05", "\x01", "\x01")
                   PARAMS("\x00\x00\x00\xff\xff", "\x05", "\x01", "\x01")
                   PARAMS("\x00\x00\x00\xff\xff", "\x08", "\x01",
                          "\x01"));
    peer_idle(r);
    rig_stop(r);
}

/* The guest sets its line long before a server connects, and again after. */
static void test_line_is_resent_on_reconnect(void)
{
    Rig *r = rig_start();

    line(r->qts, 12, 0x03);
    PEER_EXPECT(r, SET_9600_8N1);
    qtest_outb(r->qts, UART_MCR, 0x01);
    PEER_EXPECT(r, CTL("\x08"));
    close(r->peer);
    rig_connect(r);
    PEER_EXPECT(r, HELLO PARAMS(B9600, "\x08", "\x01", "\x01") CTL("\x08")
                   RTS_OFF BRK_OFF);
    peer_idle(r);
    rig_stop(r);
}

/* A ycable over a tablet and two rfc2217 legs: fan-out of ioctls. */
static void test_ycable_fan_out(void)
{
    g_autofree char *dir = g_dir_make_tmp("rfc2217-XXXXXX", NULL);
    g_autofree char *p0 = g_build_filename(dir, "0.sock", NULL);
    g_autofree char *p1 = g_build_filename(dir, "1.sock", NULL);
    g_autofree char *args = g_strdup_printf(
        "-M malta -display none -chardev wacom-iv,id=tab "
        "-chardev socket,id=s0,path=%s,server=on,wait=off "
        "-chardev socket,id=s1,path=%s,server=on,wait=off "
        "-chardev rfc2217,id=r0,chardev=s0 "
        "-chardev rfc2217,id=r1,chardev=s1 "
        "-chardev ycable,id=line,legs.0=tab,legs.1=r0,legs.2=r1 "
        "-serial chardev:line", p0, p1);
    QTestState *qts = qtest_init(args);
    Rig a = { .qts = qts, .path = p0 }, b = { .qts = qts, .path = p1 };

    rig_connect(&a);
    rig_connect(&b);
    PEER_EXPECT(&a, HELLO DTR_OFF RTS_OFF BRK_OFF);
    PEER_EXPECT(&b, HELLO DTR_OFF RTS_OFF BRK_OFF);

    /* Break and modem lines reach every leg, not only the first. */
    qtest_outb(qts, UART_LCR, 0x43);
    PEER_EXPECT(&a, PARAMS(B9600, "\x08", "\x01", "\x01") CTL("\x05"));
    PEER_EXPECT(&b, PARAMS(B9600, "\x08", "\x01", "\x01") CTL("\x05"));
    qtest_outb(qts, UART_MCR, 0x01);
    PEER_EXPECT(&a, CTL("\x08"));
    PEER_EXPECT(&b, CTL("\x08"));
    /* The tablet cannot answer a modem read; the first leg that can does. */
    g_assert_cmpint(qtest_inb(qts, UART_MSR) & 0xb0, ==, 0xb0);

    qtest_quit(qts);
    close(a.peer);
    close(b.peer);
    unlink(p0);
    unlink(p1);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rfc2217/announces-and-sets-line",
                   test_announces_and_sets_line);
    qtest_add_func("/rfc2217/break-and-modem-lines",
                   test_break_and_modem_lines);
    qtest_add_func("/rfc2217/data-is-escaped-both-ways",
                   test_data_is_escaped_both_ways);
    qtest_add_func("/rfc2217/server-options", test_server_options);
    qtest_add_func("/rfc2217/split-subnegotiation",
                   test_split_subnegotiation);
    qtest_add_func("/rfc2217/ff-in-value", test_ff_in_value);
    qtest_add_func("/rfc2217/ycable-fan-out", test_ycable_fan_out);
    qtest_add_func("/rfc2217/line-is-resent-on-reconnect",
                   test_line_is_resent_on_reconnect);

    return g_test_run();
}
