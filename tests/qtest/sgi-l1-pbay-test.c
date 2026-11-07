/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The L1's end of the power-bay link. Some cases play the bay from a socket
 * the test holds, so the frames the L1 sends can be checked; others join the
 * L1 to a power-bay controller model through a socket pair.
 *
 * The frames and replies are the power-bay controller datasheet's: the
 * initialisation order, the worked tunnelled frames and the L1's receive
 * rules. The text the pbay command prints is not established, so these
 * cases check only the status word.
 */
#include "qemu/osdep.h"
#include <poll.h>
#include <sys/socket.h>
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-l1-test.h"

#define BASE UINT64_C(0x11000000)
#define L1 "/machine/peripheral/l1c"
#define LOCAL_COMMAND UINT32_C(0x0ffff003)
#define L1_ADDRESS 0x0102
#define IO_TIMEOUT_MS 20000

typedef struct Request {
    uint8_t body[300];
    size_t len;         /* without the checksum */
    uint8_t cmd, tag;
    uint16_t dst, src;
} Request;

typedef struct Rig {
    QTestState *qts;
    int bay;            /* the test's end, or -1 when a bay model holds it */
    Request *held;      /* a request read ahead of its pass, or NULL */
    Request last;       /* the last request read, to skip a repeat */
} Rig;

static void rig_start(Rig *r, bool scripted, const char *l1_props,
                      const char *extra)
{
    int sv[2];
    g_autofree char *bay = scripted ? g_strdup("") :
        g_strdup_printf("-chardev socket,id=pb,fd=%d "
                        "-device sgi-pbay,id=pbay,chardev=pb", 0);

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    if (!scripted) {
        g_free(bay);
        bay = g_strdup_printf("-chardev socket,id=pb,fd=%d "
                              "-device sgi-pbay,id=pbay,chardev=pb", sv[1]);
    }
    r->held = NULL;
    memset(&r->last, 0, sizeof(r->last));
    r->qts = qtest_initf("-M none -cpu R10000 -S -d unimp "
        "-chardev socket,id=pa,fd=%d %s "
        "-device sgi-l1,id=l1c,brick-type=cbrick,named=on,rack=1,bay=7,"
        "pbay=pa,pbay-address=0x%04x,pbay-poll-ms=50,pbay-tick-us=200%s "
        "-device sgi-l1-uart,id=l1u,controller=l1c %s",
        sv[0], bay, L1_ADDRESS, l1_props, extra);
    qtest_sysbus_mmio_map(r->qts, "/machine/peripheral/l1u", 0, BASE);
    close(sv[0]);
    if (scripted) {
        r->bay = sv[1];
    } else {
        close(sv[1]);
        r->bay = -1;
    }
}

static void rig_stop(Rig *r)
{
    if (r->bay >= 0) {
        close(r->bay);
    }
    qtest_quit(r->qts);
}

static uint64_t l1_u(Rig *r, const char *prop)
{
    QDict *resp = qtest_qmp(r->qts, "{'execute':'qom-get','arguments':"
                            "{'path':%s,'property':%s}}", L1, prop);
    uint64_t v = qdict_get_int(resp, "return");

    qobject_unref(resp);
    return v;
}

static void wait_status(Rig *r, unsigned want)
{
    int64_t end = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;

    while (l1_u(r, "pbay-status") != want) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(2000);
    }
}

/* One operator command through the UART window; its lines, one per line. */
static char *command(Rig *r, const char *text)
{
    uint8_t request[136] = { 0x10, 0x0f, 0xff, 0xf0, 0x03, 0, 0, 1 };
    uint8_t reply[131];
    GString *out = g_string_new(NULL);
    size_t length, offset = 6;

    request[8] = 1;
    memcpy(request + 9, text, strlen(text) + 1);
    sgi_l1_test_send(r->qts, BASE, request, 9 + strlen(text) + 1, false);
    length = sgi_l1_test_receive(r->qts, BASE, reply, 131);
    for (unsigned i = 0; i < reply[5]; i++) {
        g_assert_cmpuint(reply[offset], ==, 1);
        offset++;
        g_string_append(out, (const char *)reply + offset);
        g_string_append_c(out, '\n');
        offset += strlen((const char *)reply + offset) + 1;
    }
    g_assert_cmpuint(offset, ==, length);
    return g_string_free(out, FALSE);
}

/* The bay side of a socket. */

static uint16_t checksum(const uint8_t *body, size_t len)
{
    unsigned sum = 0;

    for (size_t i = 0; i < len; i++) {
        sum += body[i];
    }
    return 0xffff - (sum + len + 1);
}

static void stuff(GByteArray *w, uint8_t b)
{
    static const uint8_t esc = 0xaa;
    uint8_t m;

    if (b == 0xa0 || b == 0xa5 || b == 0xaa) {
        m = b == 0xa0 ? 0xb0 : b == 0xa5 ? 0xb5 : 0xba;
        g_byte_array_append(w, &esc, 1);
        g_byte_array_append(w, &m, 1);
    } else {
        g_byte_array_append(w, &b, 1);
    }
}

static void bay_write(Rig *r, const uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t n = write(r->bay, buf, len);

        g_assert_cmpint(n, >, 0);
        buf += n;
        len -= n;
    }
}

/* Send @body behind a start byte with its checksum, corrupted if asked. */
static void bay_send(Rig *r, const uint8_t *body, size_t len, bool bad)
{
    g_autoptr(GByteArray) w = g_byte_array_new();
    uint16_t sum = checksum(body, len);
    uint8_t start = 0xa0, end = 0xa5;

    g_byte_array_append(w, &start, 1);
    for (size_t i = 0; i < len; i++) {
        stuff(w, body[i]);
    }
    stuff(w, sum & 0xff);
    stuff(w, (sum >> 8) ^ bad);
    g_byte_array_append(w, &end, 1);
    bay_write(r, w->data, w->len);
}


/* The next frame the L1 sends: start byte, stuffed body, terminator. */
static void bay_read_raw(Rig *r, Request *q)
{
    uint8_t b;
    bool escape = false;
    size_t n = 0;
    struct pollfd p = { .fd = r->bay, .events = POLLIN };

    g_assert_cmpint(poll(&p, 1, IO_TIMEOUT_MS), ==, 1);
    g_assert_cmpint(read(r->bay, &b, 1), ==, 1);
    g_assert_cmphex(b, ==, 0xa0);
    for (;;) {
        g_assert_cmpint(poll(&p, 1, IO_TIMEOUT_MS), ==, 1);
        g_assert_cmpint(read(r->bay, &b, 1), ==, 1);
        if (b == 0xa5) {
            break;
        }
        g_assert_cmphex(b, !=, 0xa0);
        if (escape) {
            b = b == 0xb0 ? 0xa0 : b == 0xb5 ? 0xa5 : b == 0xba ? 0xaa : 0;
            g_assert_cmphex(b, !=, 0);
            escape = false;
        } else if (b == 0xaa) {
            escape = true;
            continue;
        }
        g_assert_cmpuint(n, <, sizeof(q->body));
        q->body[n++] = b;
    }
    g_assert_false(escape);
    g_assert_cmpuint(n, >=, 9);
    n -= 2;
    g_assert_cmphex(q->body[n] | q->body[n + 1] << 8, ==,
                    checksum(q->body, n));
    q->len = n;
    q->dst = lduw_le_p(q->body);
    q->src = lduw_le_p(q->body + 2);
    q->tag = q->body[5];
    q->cmd = q->body[6];
    g_assert_cmphex(q->src, ==, L1_ADDRESS);
    g_assert_cmpuint(q->body[4], ==, 0);
    g_assert_cmpuint(q->tag & 0x80, ==, 0);
}

/*
 * The next request the L1 has not already sent: one it repeats while the
 * test is slow to answer is skipped.
 */
static void bay_read(Rig *r, Request *q)
{
    if (r->held) {
        *q = *r->held;
        g_free(r->held);
        r->held = NULL;
        r->last = *q;
        return;
    }
    do {
        bay_read_raw(r, q);
    } while (q->len == r->last.len &&
             !memcmp(q->body, r->last.body, q->len));
    r->last = *q;
}

/* A reply to @q from @from: the bytes from offset 7 on are @tail. */
static void bay_reply_from(Rig *r, const Request *q, uint16_t from,
                           const uint8_t *tail, size_t n, bool bad)
{
    uint8_t body[300] = { q->src & 0xff, q->src >> 8, from & 0xff,
                          from >> 8, 0, q->tag | 0x80, q->cmd };

    memcpy(body + 7, tail, n);
    bay_send(r, body, 7 + n, bad);
}

static void bay_reply(Rig *r, const Request *q, const uint8_t *tail, size_t n)
{
    bay_reply_from(r, q, q->dst, tail, n, false);
}

#define REPLY(r, q, ...) do { \
        const uint8_t t_[] = { __VA_ARGS__ }; \
        bay_reply(r, q, t_, sizeof(t_)); \
    } while (0)

/* A tunnelled reply: both completion codes zero, then @data. */
static void tunnel_reply(Rig *r, const Request *q, const uint8_t *data,
                         size_t n)
{
    uint8_t tail[300] = { 0x00, q->body[7], q->body[8] + 4, q->body[9], 0x00 };

    memcpy(tail + 5, data, n);
    bay_reply(r, q, tail, 5 + n);
}

#define TUNNEL_REPLY(r, q, ...) do { \
        const uint8_t d_[] = { __VA_ARGS__ }; \
        tunnel_reply(r, q, d_, sizeof(d_)); \
    } while (0)

static void expect_link(const Request *q, uint8_t cmd, uint16_t dst,
                        const uint8_t *data, size_t n)
{
    g_assert_cmphex(q->cmd, ==, cmd);
    g_assert_cmphex(q->dst, ==, dst);
    g_assert_cmpmem(q->body + 7, q->len - 7, data, n);
}

#define EXPECT_LINK(q, cmd, dst, ...) do { \
        const uint8_t e_[] = { __VA_ARGS__ }; \
        expect_link(q, cmd, dst, e_, sizeof(e_)); \
    } while (0)

/*
 * Play the bay through the initialisation sequence of datasheet section
 * "L1 behaviour a model must serve": 0x10 and 0x02 to address 0, 0x12 with
 * the replied address, 0x13, the 0x00,0x00 capability pair, then the record
 * loop until the next id 0xffff.
 */
static void serve_init(Rig *r)
{
    Request q;
    unsigned id = 0;

    bay_read(r, &q);
    EXPECT_LINK(&q, 0x10, 0x0000);
    bay_read(r, &q);
    EXPECT_LINK(&q, 0x02, 0x0000);
    REPLY(r, &q, 0x00, 0x00, 0x44);
    bay_read(r, &q);
    EXPECT_LINK(&q, 0x12, 0x4400, 0x00, 0x44);
    REPLY(r, &q, 0x00);
    bay_read(r, &q);
    EXPECT_LINK(&q, 0x13, 0x4400);
    REPLY(r, &q, 0x00, 0x22);
    bay_read(r, &q);
    EXPECT_LINK(&q, 0x20, 0x4400, 0x22, 0x00, 0x00);
    TUNNEL_REPLY(r, &q, 0x00, 0x22, 0x22, 0x00, 0x00);

    for (;;) {
        uint16_t next;

        /* Five bytes to learn the record id... */
        bay_read(r, &q);
        EXPECT_LINK(&q, 0x20, 0x4400, 0x22, 0x28, 0x23, 0x00, 0x00,
                    id & 0xff, id >> 8, 0x00, 0x05);
        next = id == 12 ? 0xffff : id + 1;
        if (!id) {
            next = 2;
        }
        TUNNEL_REPLY(r, &q, next & 0xff, next >> 8, id ? id : 1, 0x00, 0x01,
                     0x02, 0x1b);
        /* ...then the record with the full count. */
        bay_read(r, &q);
        EXPECT_LINK(&q, 0x20, 0x4400, 0x22, 0x28, 0x23, 0x00, 0x00,
                    id ? id : 1, 0x00, 0x00, 0xff);
        TUNNEL_REPLY(r, &q, next & 0xff, next >> 8, id ? id : 1, 0x00);
        if (next == 0xffff) {
            break;
        }
        id = next;
    }
}

/* One polling pass; @masks[n] is sensor n's reading. */
static void serve_pass(Rig *r, const uint8_t masks[15])
{
    Request q;

    for (unsigned n = 1; n <= 14; n++) {
        bay_read(r, &q);
        EXPECT_LINK(&q, 0x20, 0x4400, 0x22, 0x10, 0x2d, n);
        TUNNEL_REPLY(r, &q, 0x00, 0x00, masks[n], 0x00);
    }
}

static void test_init_sequence(void)
{
    uint8_t clear[15] = { 0 };
    Rig r;

    rig_start(&r, true, "", "");
    serve_init(&r);
    wait_status(&r, 2);
    g_assert_cmpuint(l1_u(&r, "pbay-records"), ==, 12);
    {
        g_autofree char *out = command(&r, "pbay");

        g_assert_cmpstr(out, ==, "ready\n");
    }
    serve_pass(&r, clear);
    rig_stop(&r);
}

/*
 * Serve one pass with @masks and then read the first request of the next,
 * which proves the L1 compared the pass; the L1's log must then hold
 * @expect and nothing else new. The request is kept for the next pass.
 */
static void pass_expect(Rig *r, const uint8_t masks[15], const char *expect)
{
    g_autofree char *log = NULL;

    serve_pass(r, masks);
    r->held = g_new(Request, 1);
    bay_read(r, r->held);
    log = command(r, "log");
    if (expect) {
        g_assert_nonnull(strstr(log, expect));
    } else {
        g_assert_nonnull(strstr(log, "log is empty"));
    }
    g_free(command(r, "log clear"));
}

static void test_polling_log(void)
{
    uint8_t masks[15] = { 0 };
    Rig r;

    rig_start(&r, true, "", "");
    serve_init(&r);
    g_free(command(&r, "log clear"));
    serve_pass(&r, masks);      /* a clear bay logs nothing */
    r.held = g_new(Request, 1);
    bay_read(&r, r.held);
    g_free(command(&r, "log clear"));

    masks[1] = 0x02;
    pass_expect(&r, masks, "Power Bay DPS 1 fail, lost redundancy");
    /* Unchanged: a failure is logged once however many passes see it. */
    pass_expect(&r, masks, NULL);
    masks[2] = 0x08;
    pass_expect(&r, masks, "Power Bay DPS 2 AC fail, lost redundancy");
    masks[2] = 0x09;
    pass_expect(&r, masks, "Power Bay DPS 2 removed");
    masks[2] = 0x08;
    pass_expect(&r, masks, "Power Bay DPS 2 inserted");
    masks[3] = 0x04;
    pass_expect(&r, masks, "Power Bay DPS 3 predictive fail");
    /* A DC port that is not present is skipped; a failed one is logged. */
    masks[8] = 0x03;
    pass_expect(&r, masks, NULL);
    masks[9] = 0x02;
    pass_expect(&r, masks, "Power Bay DCport 3 fail, lost power to brick");
    /*
     * A port read with bit 0 set is skipped and keeps what the poll stored,
     * which was clear, so the failure shows as new when the port comes on.
     */
    masks[8] = 0x02;
    pass_expect(&r, masks, "Power Bay DCport 2 fail, lost power to brick");
    /*
     * A supply read with bit 0 set clears what the poll stored, so its
     * failure is new again when it returns.
     */
    masks[4] = 0x02;
    pass_expect(&r, masks, "Power Bay DPS 4 fail, lost redundancy");
    masks[4] = 0x03;
    pass_expect(&r, masks, "Power Bay DPS 4 removed");
    masks[4] = 0x02;
    pass_expect(&r, masks, "Power Bay DPS 4 fail, lost redundancy");
    /*
     * A port that failed keeps its stored fault while it reads as not
     * present, so the same failure is not new when the port comes back.
     */
    masks[10] = 0x02;
    pass_expect(&r, masks, "Power Bay DCport 4 fail, lost power to brick");
    masks[10] = 0x03;
    pass_expect(&r, masks, NULL);
    masks[10] = 0x02;
    pass_expect(&r, masks, NULL);
    rig_stop(&r);
}

/*
 * Nothing is stored at start: a fault present on the first pass is new, but
 * a supply that is absent on it is not reported as removed.
 */
static void test_first_pass(void)
{
    g_autofree char *log = NULL;
    uint8_t masks[15] = { 0 };
    Rig r;

    rig_start(&r, true, "", "");
    serve_init(&r);
    g_free(command(&r, "log clear"));
    masks[2] = 0x04;
    masks[5] = 0x01;
    serve_pass(&r, masks);
    r.held = g_new(Request, 1);
    bay_read(&r, r.held);
    log = command(&r, "log");
    g_assert_nonnull(strstr(log, "Power Bay DPS 2 predictive fail"));
    g_assert_null(strstr(log, "DPS 5"));
    g_free(command(&r, "log clear"));
    masks[5] = 0x00;
    pass_expect(&r, masks, "Power Bay DPS 5 inserted");
    rig_stop(&r);
}

static void test_receive_rules(void)
{
    Request q, again;
    Rig r;

    rig_start(&r, true, "", "");
    bay_read(&r, &q);
    EXPECT_LINK(&q, 0x10, 0x0000);
    bay_read(&r, &q);
    EXPECT_LINK(&q, 0x02, 0x0000);

    /* A reply to another address is dropped, and the L1 asks again. */
    {
        uint8_t body[] = { 0x55, 0x05, 0x00, 0x44, 0x00, q.tag | 0x80, 0x02,
                           0x00, 0x00, 0x44 };

        bay_send(&r, body, sizeof(body), false);
    }
    bay_read_raw(&r, &again);
    g_assert_cmpmem(again.body, again.len, q.body, q.len);

    /* A bad checksum reaches the transaction layer and is refused there. */
    bay_reply_from(&r, &q, 0x4400, (uint8_t[]){ 0x00, 0x00, 0x44 }, 3, true);
    bay_read_raw(&r, &again);
    g_assert_cmpmem(again.body, again.len, q.body, q.len);

    /*
     * Bytes before the start byte are ignored; a bare start byte inside the
     * frame is dropped without restarting it; an escape followed by some
     * other byte drops that byte; a reply to address 0 is accepted.
     */
    {
        const uint8_t wire[] = {
            0x11, 0x22, 0x33,
            0xa0, 0x00, 0x00, 0xa0, 0x00, 0x44, 0x00, q.tag | 0x80, 0x02,
            0xaa, 0x00, 0x00, 0x00, 0x44,
        };
        uint8_t body[] = { 0x00, 0x00, 0x00, 0x44, 0x00, q.tag | 0x80, 0x02,
                           0x00, 0x00, 0x44 };
        uint16_t sum = checksum(body, sizeof(body));
        uint8_t tail[] = { sum & 0xff, sum >> 8, 0xa5 };

        bay_write(&r, wire, sizeof(wire));
        bay_write(&r, tail, sizeof(tail));
    }
    bay_read(&r, &q);
    EXPECT_LINK(&q, 0x12, 0x4400, 0x00, 0x44);

    /*
     * A stray 0xaa before the start byte leaves the receiver in its escape
     * state: the next 0xa0 is dropped, and so is everything up to the 0xa5
     * that resets it. A frame after that parses.
     */
    {
        uint8_t body[] = { 0x02, 0x01, 0x00, 0x44, 0x00, q.tag | 0x80, 0x12,
                           0x00 };
        uint16_t sum = checksum(body, sizeof(body));
        uint8_t wire[3 + sizeof(body) + 2] = { 0xaa, 0xa0 };
        uint8_t end = 0xa5;

        /* A frame that would complete the request, but for the escape. */
        memcpy(wire + 2, body, sizeof(body));
        wire[2 + sizeof(body)] = sum & 0xff;
        wire[3 + sizeof(body)] = sum >> 8;
        for (unsigned i = 2; i < sizeof(wire) - 1; i++) {
            g_assert_true(wire[i] != 0xa0 && wire[i] != 0xa5 &&
                          wire[i] != 0xaa);
        }
        bay_write(&r, wire, sizeof(wire) - 1);
        bay_write(&r, &end, 1);
        /* The dropped frame did not complete the request: it is repeated. */
        bay_read_raw(&r, &again);
        g_assert_cmpmem(again.body, again.len, q.body, q.len);
        bay_send(&r, body, sizeof(body), false);
    }
    bay_read(&r, &q);
    EXPECT_LINK(&q, 0x13, 0x4400);

    /*
     * A terminator that arrives with an escape pending ends the frame and
     * clears the escape, as one outside a frame does: the reply that
     * follows is not swallowed.
     */
    {
        const uint8_t wire[] = { 0xa0, 0x00, 0x00, 0xaa, 0xa5 };

        bay_write(&r, wire, sizeof(wire));
    }
    REPLY(&r, &q, 0x00, 0x22);
    bay_read(&r, &q);
    EXPECT_LINK(&q, 0x20, 0x4400, 0x22, 0x00, 0x00);
    rig_stop(&r);
}

/*
 * Task start waits 100 * bay + 300 ticks before the first request (bay 7
 * here), and a transaction sends three times: a read that times out waits 20
 * ticks and resends. The bounds are lower ones so a loaded host cannot fail
 * them.
 */
static void test_start_delay_and_resends(void)
{
    Request q, again;
    int64_t t0, t1, t2;
    Rig r;

    rig_start(&r, true, ",pbay-tick-us=500", "");
    t0 = g_get_monotonic_time();
    bay_read_raw(&r, &q);
    t1 = g_get_monotonic_time();
    g_assert_cmpint(t1 - t0, >=, 1000 * 500 * 9 / 10);
    g_assert_cmphex(q.cmd, ==, 0x10);
    bay_read_raw(&r, &q);
    g_assert_cmphex(q.cmd, ==, 0x02);
    t1 = g_get_monotonic_time();
    bay_read_raw(&r, &again);
    t2 = g_get_monotonic_time();
    g_assert_cmpmem(again.body, again.len, q.body, q.len);
    /* A read lasts at least 100 ticks, then 20 more before the resend. */
    g_assert_cmpint(t2 - t1, >=, 120 * 500 * 9 / 10);
    rig_stop(&r);
}

/*
 * A frame that fails the reply checks costs a read, and three of them end
 * the transmission without waiting out a read timeout: the resend follows
 * after 20 ticks, well before the 100 to 200 ticks a silent read takes.
 */
static void test_bad_frames_cost_reads(void)
{
    Request q, again;
    int64_t t0, t1;
    Rig r;

    rig_start(&r, true, ",pbay-tick-us=10000", "");
    bay_read_raw(&r, &q);
    bay_read_raw(&r, &q);
    g_assert_cmphex(q.cmd, ==, 0x02);
    t0 = g_get_monotonic_time();
    for (unsigned i = 0; i < 3; i++) {
        bay_reply_from(&r, &q, 0x4400, (uint8_t[]){ 0x00, 0x00, 0x44 }, 3,
                       true);
    }
    bay_read_raw(&r, &again);
    t1 = g_get_monotonic_time();
    g_assert_cmpmem(again.body, again.len, q.body, q.len);
    g_assert_cmpint(t1 - t0, <, 1000 * 1000);
    rig_stop(&r);
}

static void test_no_bay(void)
{
    Request q, again;
    g_autofree char *out = NULL;
    Rig r;

    rig_start(&r, true, "", "");
    bay_read(&r, &q);
    EXPECT_LINK(&q, 0x10, 0x0000);
    bay_read(&r, &q);
    EXPECT_LINK(&q, 0x02, 0x0000);
    /* Three attempts at the same request, then a communication failure. */
    for (unsigned i = 1; i < 3; i++) {
        bay_read_raw(&r, &again);
        g_assert_cmpmem(again.body, again.len, q.body, q.len);
    }
    wait_status(&r, 5);
    out = command(&r, "pbay");
    g_assert_cmpstr(out, ==, "communication failure\n");
    rig_stop(&r);

    /* A zero address in the reply means no bay either. */
    rig_start(&r, true, "", "");
    bay_read(&r, &q);
    bay_read(&r, &q);
    REPLY(&r, &q, 0x00, 0x00, 0x00);
    wait_status(&r, 5);
    rig_stop(&r);
}

static void test_with_bay_model(void)
{
    g_autofree char *out = NULL;
    int64_t end = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
    Rig r;

    rig_start(&r, false, "", "");
    wait_status(&r, 2);
    g_assert_cmpuint(l1_u(&r, "pbay-records"), ==, 12);
    out = command(&r, "pbay");
    g_assert_cmpstr(out, ==, "ready\n");
    g_free(out);

    /*
     * Pulling supply 1 shows in the log after a later pass. A reply holds
     * only the first lines of the log, so start it empty.
     */
    g_free(command(&r, "log clear"));
    qtest_qmp_assert_success(r.qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/pbay',"
        "'property':'input-snapshot','value':1}}");
    for (;;) {
        out = command(&r, "log");
        if (strstr(out, "Power Bay DPS 1 removed")) {
            break;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_free(out);
        g_usleep(20000);
    }
    g_assert_null(strstr(out, "DPS 2"));
    rig_stop(&r);
}

/*
 * Start QEMU with the link on a brick of type @letter and @props added to the
 * controller; return its stderr.
 */
static char *start_on_brick(char letter, const char *props, int *exit_status)
{
    const char *binary = qtest_qemu_binary(NULL);
    g_autofree char *chardev = NULL, *device = NULL;
    char *err = NULL;
    int sv[2], status;
    gchar *argv[12];
    unsigned n = 0;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    chardev = g_strdup_printf("socket,id=pa,fd=%d", sv[0]);
    device = g_strdup_printf("sgi-l1,id=l1c,pbay=pa,brick-type-code=%d%s",
                             letter, props);
    argv[n++] = (char *)binary;
    argv[n++] = (char *)"-M";
    argv[n++] = (char *)"none";
    argv[n++] = (char *)"-display";
    argv[n++] = (char *)"none";
    argv[n++] = (char *)"-chardev";
    argv[n++] = chardev;
    argv[n++] = (char *)"-device";
    argv[n++] = device;
    argv[n] = NULL;
    g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                               NULL, NULL, NULL, &err, &status, NULL));
    g_assert_true(WIFEXITED(status));
    *exit_status = WEXITSTATUS(status);
    close(sv[0]);
    close(sv[1]);
    return err;
}

/*
 * Datasheet "Which bricks run the power-bay task": the task does not start,
 * and the L1 refuses the link, on 0 4 7 8 9 A E G O Q V. G's second port is
 * a 38400 baud console, not the bay link.
 */
static void test_closed_port(void)
{
    const char *closed = "04789AEGOQV";

    for (const char *c = closed; *c; c++) {
        g_autofree char *err = NULL;
        g_autofree char *want = g_strdup_printf("on brick type '%c'", *c);
        int status;

        err = start_on_brick(*c, "", &status);
        g_assert_cmpint(status, !=, 0);
        g_assert_nonnull(strstr(err, want));
        if (*c == 'G') {
            g_assert_nonnull(strstr(err, "38400 baud console"));
        }
    }
}

/* A poll period or tick of zero would run without pause, so it is refused. */
static void test_poll_period(void)
{
    g_autofree char *err = NULL;
    int status;

    err = start_on_brick('C', ",pbay-poll-ms=0", &status);
    g_assert_cmpint(status, !=, 0);
    g_assert_nonnull(strstr(err, "pbay-poll-ms"));
    g_free(err);
    err = start_on_brick('C', ",pbay-tick-us=0", &status);
    g_assert_cmpint(status, !=, 0);
    g_assert_nonnull(strstr(err, "pbay-tick-us"));
}

static void test_blocks_migration(void)
{
    Rig r;
    QDict *resp;

    rig_start(&r, true, "", "");
    resp = qtest_qmp(r.qts, "{'execute':'migrate','arguments':"
                     "{'uri':'unix:/nonexistent/pbay'}}");
    g_assert_true(qdict_haskey(resp, "error"));
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(resp, "error"),
                                          "desc"), "power-bay"));
    qobject_unref(resp);
    rig_stop(&r);
}

static void test_unset_is_unchanged(void)
{
    g_autofree char *known = NULL, *pbay = NULL, *unknown = NULL;
    QTestState *qts = qtest_init("-M none -cpu R10000 -S "
        "-device sgi-l1,id=l1c,brick-type=cbrick,named=on,rack=1,bay=7 "
        "-device sgi-l1-uart,id=l1u,controller=l1c");
    Rig r = { .qts = qts, .bay = -1 };

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/l1u", 0, BASE);
    /* With no backend the pbay command is as unknown as any other word. */
    pbay = command(&r, "pbay");
    unknown = command(&r, "nosuchword");
    g_assert_cmpstr(pbay, ==, unknown);
    g_assert_cmpuint(l1_u(&r, "pbay-status"), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sgi-l1-pbay/init-sequence", test_init_sequence);
    qtest_add_func("/sgi-l1-pbay/polling-log", test_polling_log);
    qtest_add_func("/sgi-l1-pbay/first-pass", test_first_pass);
    qtest_add_func("/sgi-l1-pbay/receive-rules", test_receive_rules);
    qtest_add_func("/sgi-l1-pbay/start-delay-and-resends",
                   test_start_delay_and_resends);
    qtest_add_func("/sgi-l1-pbay/bad-frames-cost-reads",
                   test_bad_frames_cost_reads);
    qtest_add_func("/sgi-l1-pbay/no-bay", test_no_bay);
    qtest_add_func("/sgi-l1-pbay/bay-model", test_with_bay_model);
    qtest_add_func("/sgi-l1-pbay/closed-port", test_closed_port);
    qtest_add_func("/sgi-l1-pbay/poll-period", test_poll_period);
    qtest_add_func("/sgi-l1-pbay/blocks-migration", test_blocks_migration);
    qtest_add_func("/sgi-l1-pbay/unset", test_unset_is_unchanged);

    return g_test_run();
}
