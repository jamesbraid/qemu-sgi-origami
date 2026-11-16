/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * ycable chardev: Malta's 16550 sits on a ycable whose legs are the wacom-iv
 * tablet and unix sockets this test serves. The tablet's replies are the
 * WTC-1002 literals wacom-iv-test.c checks.
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
#define UART_LSR    (COM1 + 5)

#define LSR_DR      0x01
#define LSR_BI      0x10
#define LCR_DLAB    0x80
#define LCR_8N1     0x03

#define DIV_9600    12
#define DIV_19200   6

#define IDENTITY    "~#UD-1212-R00 V1.3-5\r"
#define POLL_MS     5000

typedef struct Rig {
    QTestState *qts;
    char *dir;
    int peer[2];
} Rig;

/* Let QEMU's main loop notice a peer coming or going. */
static void settle(Rig *r)
{
    qobject_unref(qtest_qmp(r->qts, "{'execute': 'query-status'}"));
    qobject_unref(qtest_qmp(r->qts, "{'execute': 'query-status'}"));
}

/* A ycable over ntab wacom-iv tablets and nsock unix-socket legs. */
static Rig *rig_start_opts(int ntab, int nsock, const char *sockopts)
{
    Rig *r = g_new0(Rig, 1);
    GString *args = g_string_new("-M malta -display none ");
    GString *legs = g_string_new("");
    int n = 0;

    r->peer[0] = r->peer[1] = -1;
    for (int i = 0; i < ntab; i++) {
        g_string_append_printf(args, "-chardev wacom-iv,id=tab%d ", i);
        g_string_append_printf(legs, ",legs.%d=tab%d", n++, i);
    }
    if (nsock) {
        r->dir = g_dir_make_tmp("ycable-XXXXXX", NULL);
        g_assert(r->dir);
    }
    for (int i = 0; i < nsock; i++) {
        g_string_append_printf(args, "-chardev socket,id=s%d,path=%s/%d.sock,"
                               "server=on,wait=off%s ", i, r->dir, i, sockopts);
        g_string_append_printf(legs, ",legs.%d=s%d", n++, i);
    }
    g_string_append_printf(args, "-chardev ycable,id=line%s -serial "
                           "chardev:line", legs->str);
    r->qts = qtest_init(args->str);
    g_string_free(args, true);
    g_string_free(legs, true);

    for (int i = 0; i < nsock; i++) {
        g_autofree char *path = g_strdup_printf("%s/%d.sock", r->dir, i);

        r->peer[i] = unix_connect(path, NULL);
        g_assert_cmpint(r->peer[i], >=, 0);
    }
    if (nsock) {
        settle(r);
    }
    return r;
}

static Rig *rig_start(int ntab, int nsock)
{
    return rig_start_opts(ntab, nsock, "");
}

static void rig_stop(Rig *r)
{
    qtest_quit(r->qts);
    for (int i = 0; i < 2; i++) {
        if (r->peer[i] >= 0) {
            g_autofree char *path = g_strdup_printf("%s/%d.sock", r->dir, i);

            close(r->peer[i]);
            unlink(path);
        }
    }
    if (r->dir) {
        rmdir(r->dir);
    }
    g_free(r->dir);
    g_free(r);
}

static void line(QTestState *qts, int divisor)
{
    qtest_outb(qts, UART_LCR, LCR_DLAB);
    qtest_outb(qts, UART_DLL, divisor & 0xff);
    qtest_outb(qts, UART_DLM, divisor >> 8);
    qtest_outb(qts, UART_LCR, LCR_8N1);
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

/* Read what the guest has received, waiting up to POLL_MS for want bytes. */
static int rx(QTestState *qts, uint8_t *buf, int want)
{
    int n = 0;

    /* A 16550 hands over at most a FIFO's worth per step. */
    for (int i = 0; i < POLL_MS / 10 + want / 8 && n < want; i++) {
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

static void expect_rx(QTestState *qts, const char *want)
{
    uint8_t buf[128];
    int n = rx(qts, buf, strlen(want));

    g_assert_cmpint(n, ==, strlen(want));
    g_assert(!memcmp(buf, want, n));
}

static void expect_silence(QTestState *qts)
{
    qtest_clock_step(qts, 100 * 1000 * 1000);
    g_assert(!(qtest_inb(qts, UART_LSR) & LSR_DR));
}

/* Exactly want bytes from a socket leg's peer, or fail after POLL_MS. */
static void peer_expect(Rig *r, int leg, const char *want)
{
    char buf[64] = "";
    size_t n = 0;

    while (n < strlen(want)) {
        struct pollfd p = { .fd = r->peer[leg], .events = POLLIN };
        ssize_t got;

        g_assert_cmpint(poll(&p, 1, POLL_MS), ==, 1);
        got = read(r->peer[leg], buf + n, strlen(want) - n);
        g_assert_cmpint(got, >, 0);
        n += got;
    }
    g_assert_cmpstr(buf, ==, want);
}

static void peer_say(Rig *r, int leg, const char *s)
{
    g_assert_cmpint(write(r->peer[leg], s, strlen(s)), ==, strlen(s));
}

/* The tablet answers only once the line matches its own. */
static void test_line_speed_reaches_leg(void)
{
    Rig *r = rig_start(1, 1);

    line(r->qts, DIV_19200);
    tx(r->qts, "~#");
    expect_silence(r->qts);
    line(r->qts, DIV_9600);
    tx(r->qts, "~#");
    expect_rx(r->qts, IDENTITY);
    rig_stop(r);
}

static void test_single_leg(void)
{
    Rig *r = rig_start(1, 0);

    line(r->qts, DIV_9600);
    tx(r->qts, "~#");
    expect_rx(r->qts, IDENTITY);
    rig_stop(r);
}

static void test_both_legs_hear(void)
{
    Rig *r = rig_start(1, 1);

    line(r->qts, DIV_9600);
    tx(r->qts, "~#");
    peer_expect(r, 0, "~#");
    expect_rx(r->qts, IDENTITY);
    peer_say(r, 0, "ACK\r");
    expect_rx(r->qts, "ACK\r");
    rig_stop(r);
}

static void test_two_sockets_hear(void)
{
    Rig *r = rig_start(0, 2);

    line(r->qts, DIV_9600);
    tx(r->qts, "HELLO");
    peer_expect(r, 0, "HELLO");
    peer_expect(r, 1, "HELLO");
    rig_stop(r);
}

/* Replies over the FIFO size from both peers at once arrive one whole. */
static void test_long_replies_unsplit(void)
{
    const char *a = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r";
    const char *b = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\r";
    Rig *r = rig_start(0, 2);
    uint8_t buf[128];
    size_t len = strlen(a);
    int n;

    line(r->qts, DIV_9600);
    peer_say(r, 0, a);
    peer_say(r, 1, b);
    n = rx(r->qts, buf, 2 * len);
    g_assert_cmpint(n, ==, 2 * len);
    g_assert(!memcmp(buf, a, len) || !memcmp(buf, b, len));
    g_assert(!memcmp(buf, a, len) ? !memcmp(buf + len, b, len)
                                  : !memcmp(buf + len, a, len));
    rig_stop(r);
}

/* More than the 4096 bytes held for the guest still arrives, in order. */
static void test_backpressure(void)
{
    enum { TOTAL = 6000 };
    Rig *r = rig_start(0, 1);
    g_autofree uint8_t *out = g_malloc(TOTAL);
    g_autofree uint8_t *in = g_malloc(TOTAL);

    for (int i = 0; i < TOTAL; i++) {
        out[i] = i % 251;
    }
    line(r->qts, DIV_9600);
    g_assert_cmpint(write(r->peer[0], out, TOTAL), ==, TOTAL);
    g_assert_cmpint(rx(r->qts, in, TOTAL), ==, TOTAL);
    g_assert(!memcmp(in, out, TOTAL));
    rig_stop(r);
}

/* A break on a leg reaches the guest as a break condition. */
static void test_break_from_leg(void)
{
    const uint8_t iac_break[] = { 0xff, 0xf3 };
    Rig *r = rig_start_opts(0, 1, ",telnet=on");

    line(r->qts, DIV_9600);
    g_assert(!(qtest_inb(r->qts, UART_LSR) & LSR_BI));
    g_assert_cmpint(write(r->peer[0], iac_break, 2), ==, 2);
    qtest_clock_step(r->qts, 10 * 1000 * 1000);
    settle(r);
    g_assert(qtest_inb(r->qts, UART_LSR) & LSR_BI);
    rig_stop(r);
}

/* Line settings reach every tablet, not only the first leg. */
static void test_line_reaches_every_leg(void)
{
    Rig *r = rig_start(2, 0);

    line(r->qts, DIV_19200);
    tx(r->qts, "~#");
    expect_silence(r->qts);
    line(r->qts, DIV_9600);
    tx(r->qts, "~#");
    expect_rx(r->qts, IDENTITY IDENTITY);
    rig_stop(r);
}

/* Losing one leg keeps the line open on the rest, and it can come back. */
static void test_leg_disconnect(void)
{
    Rig *r = rig_start(1, 1);
    g_autofree char *path = g_strdup_printf("%s/0.sock", r->dir);

    line(r->qts, DIV_9600);
    close(r->peer[0]);
    settle(r);
    tx(r->qts, "~#");
    expect_rx(r->qts, IDENTITY);

    r->peer[0] = unix_connect(path, NULL);
    g_assert_cmpint(r->peer[0], >=, 0);
    settle(r);
    tx(r->qts, "~#");
    peer_expect(r, 0, "~#");
    expect_rx(r->qts, IDENTITY);
    rig_stop(r);
}

/* Two legs answer at once; each reply arrives whole, in either order. */
static void test_merged_reply_unsplit(void)
{
    Rig *r = rig_start(0, 2);
    uint8_t buf[32];
    int n;

    line(r->qts, DIV_9600);
    peer_say(r, 0, "AAAAAAAAA\r");
    peer_say(r, 1, "BBBBBBBBB\r");
    n = rx(r->qts, buf, 20);
    g_assert_cmpint(n, ==, 20);
    buf[n] = 0;
    g_assert(!strcmp((char *)buf, "AAAAAAAAA\rBBBBBBBBB\r") ||
             !strcmp((char *)buf, "BBBBBBBBB\rAAAAAAAAA\r"));
    rig_stop(r);
}

/* A reply longer than the 16-byte FIFO must arrive whole. */
static void test_reply_beyond_fifo(void)
{
    const char *reply = "0123456789abcdefghijklmnopqrstuvwxyz0123456789\r";
    Rig *r = rig_start(0, 1);

    line(r->qts, DIV_9600);
    peer_say(r, 0, reply);
    expect_rx(r->qts, reply);
    rig_stop(r);
}

/* A socket leg hears the guest whatever the line is; only the tablet minds. */
static void test_mismatched_line(void)
{
    Rig *r = rig_start(1, 1);

    line(r->qts, DIV_19200);
    tx(r->qts, "~#");
    peer_expect(r, 0, "~#");
    peer_say(r, 0, "TAP\r");
    expect_rx(r->qts, "TAP\r");
    expect_silence(r->qts);
    rig_stop(r);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/ycable/line-speed-reaches-leg",
                   test_line_speed_reaches_leg);
    qtest_add_func("/ycable/single-leg", test_single_leg);
    qtest_add_func("/ycable/both-legs-hear", test_both_legs_hear);
    qtest_add_func("/ycable/two-sockets-hear", test_two_sockets_hear);
    qtest_add_func("/ycable/merged-reply-unsplit", test_merged_reply_unsplit);
    qtest_add_func("/ycable/reply-beyond-fifo", test_reply_beyond_fifo);
    qtest_add_func("/ycable/mismatched-line", test_mismatched_line);
    qtest_add_func("/ycable/long-replies-unsplit", test_long_replies_unsplit);
    qtest_add_func("/ycable/backpressure", test_backpressure);
    qtest_add_func("/ycable/break-from-leg", test_break_from_leg);
    qtest_add_func("/ycable/line-reaches-every-leg",
                   test_line_reaches_every_leg);
    qtest_add_func("/ycable/leg-disconnect", test_leg_disconnect);

    return g_test_run();
}
