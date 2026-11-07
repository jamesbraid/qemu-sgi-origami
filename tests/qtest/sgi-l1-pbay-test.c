/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The L1's end of the power-bay link. Some cases play the bay from a socket
 * the test holds, so the frames the L1 sends can be checked; others join the
 * L1 to a power-bay controller model through a socket pair.
 *
 * The frames and replies follow the power-bay controller's documented
 * behaviour: the initialisation order, the worked tunnelled frames and the
 * L1's receive rules. The text the pbay command prints is not established,
 * so these cases check only the status word.
 */
#include "qemu/osdep.h"
#include <poll.h>
#include <sys/socket.h>
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-l1-test.h"

#define BASE UINT64_C(0x11000000)
#define L1 "/machine/peripheral/l1c"
#define LOCAL_COMMAND UINT32_C(0x0ffff003)
/*
 * The destination of a task-3 request relayed to the I/O brick attached to
 * the issuing L1. It is LOCAL_COMMAND with the type-4 prefix, the form of
 * the attached-brick address that sgi_l1_query_route() recognises for task
 * 6, which the router strips before serving the request. The tree has no
 * shared definition of it: sgi-l1.c spells such addresses as literals.
 */
#define ATTACHED_IO_COMMAND UINT32_C(0x4ffff003)
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
    int console;        /* the operator console's other end */
    Request last;       /* the last request read, to skip a repeat */
    int64_t started;    /* just before QEMU was launched */
} Rig;

static char *console_command(Rig *r, const char *text);

/*
 * The controller's brick options; a test may change them. Slot 1 keeps the
 * task's start delay (100 * slot + 300 ticks) short; the cases that measure
 * the delay use slot 7.
 */
#define RIG_BRICK "brick-type=cbrick,named=on,rack=1,bay=1"
#define RIG_BRICK_SLOT7 "brick-type=cbrick,named=on,rack=1,bay=7"
static const char *rig_brick = RIG_BRICK;

static void rig_start(Rig *r, bool scripted, const char *l1_props,
                      const char *extra)
{
    int sv[2], cs[2];
    g_autofree char *bay = scripted ? g_strdup("") :
        g_strdup_printf("-chardev socket,id=pb,fd=%d "
                        "-device sgi-pbay,id=pbay,chardev=pb", 0);

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, cs), ==, 0);
    if (!scripted) {
        g_free(bay);
        bay = g_strdup_printf("-chardev socket,id=pb,fd=%d "
                              "-device sgi-pbay,id=pbay,chardev=pb", sv[1]);
    }
    memset(&r->last, 0, sizeof(r->last));
    r->started = g_get_monotonic_time();
    r->qts = qtest_initf("-M none -cpu R10000 -S -d unimp "
        "-chardev socket,id=pa,fd=%d -chardev socket,id=cn,fd=%d %s "
        "-device sgi-l1,id=l1c,%s,"
        "chardev=cn,pbay=pa,pbay-address=0x%04x,pbay-poll-ms=100,pbay-tick-us=1000%s "
        "-device sgi-l1-uart,id=l1u,controller=l1c %s",
        sv[0], cs[0], bay, rig_brick, L1_ADDRESS, l1_props, extra);
    qtest_sysbus_mmio_map(r->qts, "/machine/peripheral/l1u", 0, BASE);
    close(sv[0]);
    close(cs[0]);
    r->console = cs[1];
    if (scripted) {
        r->bay = sv[1];
    } else {
        close(sv[1]);
        r->bay = -1;
    }
    /* The power-up notice comes first; a command brings the prompt. */
    g_free(console_command(r, "version"));
}

/*
 * A C-brick with the UART window, cabled to an I-brick that holds the
 * scripted bay link: a request for the attached I/O brick runs on the
 * I-brick and its reply must come back through the C-brick.
 */
static void rig_start_pair(Rig *r)
{
    int sv[2];

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    memset(&r->last, 0, sizeof(r->last));
    r->started = g_get_monotonic_time();
    r->qts = qtest_initf("-M none -cpu R10000 -S -d unimp "
        "-chardev socket,id=pa,fd=%d "
        "-device sgi-l1,id=l1c,brick-type=ibrick,named=on,rack=1,bay=7,"
        "mac=02:ab:10:00:00:01,pbay=pa,pbay-address=0x%04x,"
        "pbay-poll-ms=100,pbay-tick-us=1000 "
        "-device sgi-l1,id=l1h,brick-type=cbrick,named=on,rack=1,bay=4,"
        "io-peer=/machine/peripheral/l1c "
        "-device sgi-l1-uart,id=l1u,controller=l1h",
        sv[0], L1_ADDRESS);
    qtest_sysbus_mmio_map(r->qts, "/machine/peripheral/l1u", 0, BASE);
    close(sv[0]);
    r->bay = sv[1];
    r->console = -1;
}

static void rig_stop(Rig *r)
{
    if (r->bay >= 0) {
        close(r->bay);
    }
    if (r->console >= 0) {
        close(r->console);
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

static void wait_passes(Rig *r, unsigned want)
{
    int64_t end = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;

    while (l1_u(r, "pbay-passes") < want) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(1000);
    }
}

/* Wait until the UART window at @base has a frame to read. */
static void wait_frame(Rig *r, uint64_t base)
{
    int64_t end = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;

    while (qtest_readq(r->qts, base + 0x28) != 0x61) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(2000);
    }
}

/* Send one task-3 request (an operator command) to @destination. */
static void task3_send(Rig *r, uint32_t destination, const char *text)
{
    uint8_t request[136] = { 0x10, destination >> 24, destination >> 16,
                             destination >> 8, destination, 0, 0, 1 };

    request[8] = 1;
    memcpy(request + 9, text, strlen(text) + 1);
    sgi_l1_test_send(r->qts, BASE, request, 9 + strlen(text) + 1, false);
}

/*
 * The reply to a task-3 request, which may wait for the bay: its lines, one
 * per line, and in @fields how many there were and in @code its status.
 */
static char *task3_receive(Rig *r, int32_t *code, unsigned *fields,
                           size_t *wire)
{
    uint8_t reply[131];
    GString *out = g_string_new(NULL);
    size_t length, offset = 6;

    wait_frame(r, BASE);
    length = sgi_l1_test_receive(r->qts, BASE, reply, 131);
    for (unsigned i = 0; i < reply[5]; i++) {
        g_assert_cmpuint(reply[offset], ==, 1);
        offset++;
        g_string_append(out, (const char *)reply + offset);
        g_string_append_c(out, '\n');
        offset += strlen((const char *)reply + offset) + 1;
    }
    g_assert_cmpuint(offset, ==, length);
    if (code) {
        *code = ldl_be_p(reply + 1);
    }
    if (fields) {
        *fields = reply[5];
    }
    if (wire) {
        *wire = length;
    }
    return g_string_free(out, FALSE);
}

/* One operator command through the UART window; its lines, one per line. */
static char *command(Rig *r, const char *text)
{
    task3_send(r, LOCAL_COMMAND, text);
    return task3_receive(r, NULL, NULL, NULL);
}


/*
 * The operator console. The controller starts in console mode, so a command
 * is Ctrl-T, the line and a return; the output comes back between the
 * escape's prompt and the return to console mode. The bay may need serving
 * between sending the line and finishing it.
 */
static void console_send(Rig *r, const char *text)
{
    g_autofree char *line = g_strdup_printf("\024%s\r", text);
    size_t n = strlen(line);
    ssize_t w = write(r->console, line, n);

    g_assert_cmpint(w, ==, n);
}

static char *console_finish(Rig *r)
{
    GString *out = g_string_new(NULL);
    struct pollfd p = { .fd = r->console, .events = POLLIN };
    const char *start, *end;
    char *text;

    do {
        char c;

        g_assert_cmpint(poll(&p, 1, IO_TIMEOUT_MS), ==, 1);
        g_assert_cmpint(read(r->console, &c, 1), ==, 1);
        g_string_append_c(out, c);
    } while (!(strstr(out->str, "returning to console mode") &&
               g_str_has_suffix(out->str, "to escape to L1\r\n")));
    start = strstr(out->str, "-L1> ");
    g_assert_nonnull(start);
    start += 5;
    end = strstr(start, "returning to console mode");
    text = g_strndup(start, end - start);
    g_string_free(out, TRUE);
    return text;
}

static char *console_command(Rig *r, const char *text)
{
    console_send(r, text);
    return console_finish(r);
}

/* Switch the environment monitoring on, as `pbay env on` does. */
static void monitoring_on(Rig *r)
{
    wait_status(r, 2);
    g_free(console_command(r, "pbay env on"));
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
 * The bay's twelve records, the bytes after the record id. Sensors 1 to 6 are
 * the supplies and 7 to 14 the DC ports unless a test moves them.
 */
static unsigned supply_base = 1, port_base = 7;

static size_t record_bytes(unsigned id, uint8_t *out)
{
    static const uint8_t r3[] = { 0x01, 0x12, 0x0b, 0x22, 0x01, 0xcb, 0x00,
                                  0x00, 0x00, 0x00, 0x15, 0x01, 0x00, 0x00 };
    uint8_t tail[] = { 0x00, 0x03, 0x09, 0x71, 0x00, 0x00, 0x00, 0x00, 0x03,
                       0x00, 0xc0, 0x00, 0x00, 0x08, 0x81, 0x00, 0x00, 0x00,
                       0x00, 0x00, 0x00, 0x00 };
    size_t n = 0;

    memset(out, 0, 48);
    out[n++] = id;
    out[n++] = 0;
    if (id == 1 || id == 2) {
        const uint8_t head[] = { 0x01, 0x02, 0x1b, 0x22, 0x00 };

        memcpy(out + n, head, sizeof(head));
        n += sizeof(head);
        out[n++] = id == 1 ? port_base : supply_base;
        out[n++] = id == 1 ? 0x15 : 0x0a;
        out[n++] = 0x01;
        memcpy(out + n, tail, sizeof(tail));
        out[n + 2] = id == 1 ? 0x09 : 0x08;
        return n + sizeof(tail);
    }
    if (id == 3) {
        memcpy(out + n, r3, sizeof(r3));
        return n + sizeof(r3);
    }
    if (id == 12) {
        const uint8_t head[] = { 0x01, 0x01, 0x2c, 0x22, 0x00, 0x0f, 0x14,
                                 0x09, 0x00, 0x03, 0x03, 0x01 };

        memcpy(out + n, head, sizeof(head));
        return n + 48 - 2;
    }
    {
        static const uint8_t device[] = { 0xa0, 0xa2, 0xa4, 0xa6, 0xa8, 0xaa,
                                          0xac, 0xae };
        static const uint8_t entity[] = { 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a,
                                          0x15, 0xb0 };
        const uint8_t head[] = { 0x01, 0x11, 0x0b, 0x22, device[id - 4], 0x80,
                                 0x00, 0x00, 0x10, 0x00, entity[id - 4],
                                 id <= 9 ? id - 3 : 1, 0x00, 0x00 };

        memcpy(out + n, head, sizeof(head));
        return n + sizeof(head);
    }
}

/*
 * Play the bay through the initialisation sequence the L1 expects: 0x10 and
 * 0x02 to address 0, 0x12 with the replied address, 0x13, the 0x00,0x00
 * capability pair, then the record loop until the next id 0xffff. @fru is
 * capability byte 1 and @records byte 2.
 */
static void serve_init_with(Rig *r, uint8_t fru, uint8_t records)
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
    TUNNEL_REPLY(r, &q, 0x00, fru, records, 0x00, 0x00);
    if (!records) {
        return;
    }

    for (;;) {
        uint8_t rec[48];
        size_t n;
        uint16_t next;

        /* Five bytes to learn the record id... */
        bay_read(r, &q);
        EXPECT_LINK(&q, 0x20, 0x4400, 0x22, 0x28, 0x23, 0x00, 0x00,
                    id & 0xff, id >> 8, 0x00, 0x05);
        next = id == 12 ? 0xffff : id + 1;
        if (!id) {
            next = 2;
        }
        n = record_bytes(id ? id : 1, rec);
        {
            uint8_t d[7] = { next & 0xff, next >> 8 };

            memcpy(d + 2, rec, 5);
            tunnel_reply(r, &q, d, sizeof(d));
        }
        /* ...then the record with the full count. */
        bay_read(r, &q);
        EXPECT_LINK(&q, 0x20, 0x4400, 0x22, 0x28, 0x23, 0x00, 0x00,
                    id ? id : 1, 0x00, 0x00, 0xff);
        {
            uint8_t d[2 + 48] = { next & 0xff, next >> 8 };

            memcpy(d + 2, rec, n);
            tunnel_reply(r, &q, d, 2 + n);
        }
        if (next == 0xffff) {
            break;
        }
        id = next;
    }
}

static void serve_init(Rig *r)
{
    serve_init_with(r, 0x22, 0x22);
}

static uint8_t sensor_number(unsigned n)
{
    return n <= 6 ? supply_base + n - 1 : port_base + n - 7;
}

/* Sensor reads of supplies or ports @first to @last (1 to 14 in turn). */
static void serve_range(Rig *r, const uint8_t masks[15], unsigned first,
                        unsigned last)
{
    Request q;

    for (unsigned n = first; n <= last; n++) {
        bay_read(r, &q);
        EXPECT_LINK(&q, 0x20, 0x4400, 0x22, 0x10, 0x2d, sensor_number(n));
        TUNNEL_REPLY(r, &q, 0x00, 0x00, masks[n], 0x00);
    }
}

/*
 * A polling pass: the supplies, up to three re-reads of the supplies while
 * an AC fail is in sight, the ports, and up to two re-reads of the ports
 * while a port failure is. A re-read with no masks of its own repeats @first.
 * @first[n] is the reading of supply or port n.
 */
typedef struct Pass {
    const uint8_t *first;
    unsigned ac_rereads;
    const uint8_t *ac[3];
    unsigned port_rereads;
    const uint8_t *port[2];
} Pass;

/*
 * Serve a pass and return once the L1 has compared it ("pbay-passes" has
 * gone up), so its log is final. The next pass starts a poll period later;
 * a command run in between must finish before that request's three
 * transmissions are spent, or the L1 gives the link up.
 */
static void serve_script(Rig *r, const Pass *ps)
{
    unsigned before = l1_u(r, "pbay-passes");

    serve_range(r, ps->first, 1, 6);
    for (unsigned i = 0; i < ps->ac_rereads; i++) {
        serve_range(r, ps->ac[i] ? ps->ac[i] : ps->first, 1, 6);
    }
    serve_range(r, ps->first, 7, 14);
    for (unsigned i = 0; i < ps->port_rereads; i++) {
        serve_range(r, ps->port[i] ? ps->port[i] : ps->first, 7, 14);
    }
    wait_passes(r, before + 1);
}

/* One polling pass with no fault coming into sight. */
static void serve_pass(Rig *r, const uint8_t masks[15])
{
    serve_script(r, &(Pass){ .first = masks });
}

static void test_init_sequence(void)
{
    Rig r;

    rig_start(&r, true, "", "");
    serve_init(&r);
    wait_status(&r, 2);
    g_assert_cmpuint(l1_u(&r, "pbay-records"), ==, 12);
    {
        /* The per-device current figures are not modelled: no output. */
        g_autofree char *out = console_command(&r, "pbay");

        g_assert_cmpstr(out, ==, "");
    }
    rig_stop(&r);
}

/*
 * Serve one pass; the L1's log must then hold @expect and nothing else new.
 */
static void pass_expect_script(Rig *r, const Pass *ps, const char *expect)
{
    g_autofree char *log = NULL;

    serve_script(r, ps);
    log = command(r, "log");
    if (expect) {
        g_assert_nonnull(strstr(log, expect));
    } else {
        g_assert_nonnull(strstr(log, "log is empty"));
    }
    g_free(command(r, "log clear"));
}

static void pass_expect(Rig *r, const uint8_t masks[15], const char *expect)
{
    pass_expect_script(r, &(Pass){ .first = masks }, expect);
}

static void test_polling_log(void)
{
    uint8_t masks[15] = { 0 };
    Rig r;

    rig_start(&r, true, "", "");
    serve_init(&r);
    monitoring_on(&r);
    g_free(command(&r, "log clear"));
    serve_pass(&r, masks);      /* a clear bay logs nothing */
    g_free(command(&r, "log clear"));

    masks[1] = 0x02;
    pass_expect(&r, masks, "Power Bay DPS 1 fail, lost redundancy");
    /* Unchanged: a failure is logged once however many passes see it. */
    pass_expect(&r, masks, NULL);
    masks[2] = 0x08;
    pass_expect_script(&r, &(Pass){ .first = masks, .ac_rereads = 3 },
                       "Power Bay DPS 2 AC fail, lost redundancy");
    masks[2] = 0x09;
    pass_expect(&r, masks, "Power Bay DPS 2 removed");
    masks[2] = 0x08;
    pass_expect_script(&r, &(Pass){ .first = masks, .ac_rereads = 3 },
                       "Power Bay DPS 2 inserted");
    masks[3] = 0x04;
    pass_expect(&r, masks, "Power Bay DPS 3 predictive fail");
    /* A DC port that is not present is skipped; a failed one is logged. */
    masks[8] = 0x03;
    pass_expect(&r, masks, NULL);
    masks[9] = 0x02;
    pass_expect_script(&r, &(Pass){ .first = masks, .port_rereads = 2 },
                       "Power Bay DCport 3 fail, lost power to brick");
    /*
     * A port read with bit 0 set is skipped and keeps what the poll stored,
     * which was clear, so the failure shows as new when the port comes on.
     */
    masks[8] = 0x02;
    pass_expect_script(&r, &(Pass){ .first = masks, .port_rereads = 2 },
                       "Power Bay DCport 2 fail, lost power to brick");
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
    pass_expect_script(&r, &(Pass){ .first = masks, .port_rereads = 2 },
                       "Power Bay DCport 4 fail, lost power to brick");
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
    monitoring_on(&r);
    g_free(command(&r, "log clear"));
    masks[2] = 0x04;
    masks[5] = 0x01;
    serve_pass(&r, masks);
    log = command(&r, "log");
    g_assert_nonnull(strstr(log, "Power Bay DPS 2 predictive fail"));
    g_assert_null(strstr(log, "DPS 5"));
    g_free(command(&r, "log clear"));
    masks[5] = 0x00;
    pass_expect(&r, masks, "Power Bay DPS 5 inserted");
    rig_stop(&r);
}

/*
 * The sensors behind the supplies and DC ports come from the sensor
 * records, so the poll reads whatever numbers they give. Moved, the pass
 * asks for the new ones.
 */
static void test_sensor_map(void)
{
    uint8_t clear[15] = { 0 };
    Rig r;

    supply_base = 21;
    port_base = 40;
    rig_start(&r, true, "", "");
    serve_init(&r);
    monitoring_on(&r);
    serve_pass(&r, clear);
    rig_stop(&r);
    supply_base = 1;
    port_base = 7;
}

/*
 * Capability byte 1, when nonzero, is the fallback FRU device of slot 0; a
 * FRU locator for entity 0xb0 replaces it. With byte 2 zero the record loop
 * does not run, so the fallback stands.
 */
static void test_fru_fallback(void)
{
    Rig r;

    rig_start(&r, true, "", "");
    serve_init(&r);
    wait_status(&r, 2);
    g_assert_cmphex(l1_u(&r, "pbay-fru-slot0"), ==, 0xae);
    rig_stop(&r);

    rig_start(&r, true, "", "");
    serve_init_with(&r, 0x5a, 0x00);
    wait_status(&r, 2);
    g_assert_cmpuint(l1_u(&r, "pbay-records"), ==, 0);
    g_assert_cmphex(l1_u(&r, "pbay-fru-slot0"), ==, 0x5a);
    rig_stop(&r);

    rig_start(&r, true, "", "");
    serve_init_with(&r, 0x00, 0x00);
    wait_status(&r, 2);
    g_assert_cmphex(l1_u(&r, "pbay-fru-slot0"), ==, 0);
    rig_stop(&r);
}

/* Every line of @text matches the matching pattern, in order. */
static void expect_lines(const char *text, const char *const *patterns,
                         unsigned n)
{
    g_auto(GStrv) lines = g_strsplit(text, "\r\n", -1);

    g_assert_cmpuint(g_strv_length(lines), ==, n + 1);
    g_assert_cmpstr(lines[n], ==, "");
    for (unsigned i = 0; i < n; i++) {
        g_assert_true(g_regex_match_simple(patterns[i], lines[i], 0, 0));
    }
}

/* Take whatever the L1 has sent that the test has not read. */
static void bay_drain(Rig *r)
{
    struct pollfd p = { .fd = r->bay, .events = POLLIN };
    uint8_t b[256];

    while (poll(&p, 1, 0) == 1) {
        g_assert_cmpint(read(r->bay, b, sizeof(b)), >, 0);
    }
    memset(&r->last, 0, sizeof(r->last));
}

static void test_states(void)
{
    g_autofree char *out = NULL;
    Request q;
    Rig r;

    /* The task is still waiting for its start delay. */
    rig_start(&r, true, ",pbay-tick-us=5000", "");
    out = console_command(&r, "pbay");
    g_assert_cmpstr(out, ==, "PBay busy initializing\r\n");
    rig_stop(&r);

    /* No bay: communication failure; init runs the start-up again. */
    rig_start(&r, true, "", "");
    wait_status(&r, 5);
    g_free(out);
    out = console_command(&r, "pbay version");
    g_assert_cmpstr(out, ==, "PBay communication failure\r\n");
    bay_drain(&r);
    console_send(&r, "pbay init");
    serve_init(&r);
    g_free(out);
    out = console_finish(&r);
    g_assert_cmpstr(out, ==, "PBay ready\r\n");
    wait_status(&r, 2);
    g_free(out);
    out = console_command(&r, "pbay init");
    g_assert_cmpstr(out, ==, "PBay already initialized\r\n");

    /* A second command while one waits for the bay is refused. */
    console_send(&r, "pbay version");
    bay_read(&r, &q);
    g_assert_cmphex(q.cmd, ==, 0xc0);
    g_free(out);
    out = command(&r, "pbay version");
    g_assert_nonnull(strstr(out, "PBay busy try again later"));
    REPLY(&r, &q, 0x00, 0x12, 0x00, 0x60, 0xa0, 0xd4, 0x39, 0x80);
    g_free(out);
    out = console_finish(&r);
    g_assert_cmpstr(out, ==,
                    "PBay FW Version 00.18, Built 09/29/2000 14:00:00\r\n");
    rig_stop(&r);
}

static void test_version_and_reset(void)
{
    g_autofree char *out = NULL;
    Request q;
    Rig r;

    rig_start(&r, true, "", "");
    serve_init(&r);
    wait_status(&r, 2);

    console_send(&r, "pbay reset");
    bay_read(&r, &q);
    EXPECT_LINK(&q, 0x20, 0x4400, 0x22, 0x18, 0x03);
    TUNNEL_REPLY(&r, &q, 0x00);
    out = console_finish(&r);
    g_assert_cmpstr(out, ==, "Ok\r\n");

    /* No reply to either: three transmissions, then the failure text. */
    console_send(&r, "pbay reset");
    g_free(out);
    out = console_finish(&r);
    g_assert_cmpstr(out, ==, "Timeout\r\n");
    bay_drain(&r);
    console_send(&r, "pbay version");
    g_free(out);
    out = console_finish(&r);
    g_assert_cmpstr(out, ==, "PBay version unavailable\r\n");
    wait_status(&r, 2);
    rig_stop(&r);
}

static void test_dps_dcport_env(void)
{
    static const char *const dps_head[] = {
        "^DPS +AC State +AC Range +Predictive Fail +Fail$", "^-+( +-+)*$",
    };
    uint8_t masks[15] = { 0 };
    g_autofree char *out = NULL;
    g_auto(GStrv) lines = NULL;
    Rig r;

    rig_start(&r, true, "", "");
    serve_init(&r);
    wait_status(&r, 2);

    masks[2] = 0x01;
    masks[3] = 0x18;
    masks[4] = 0x04;
    masks[5] = 0x02;
    console_send(&r, "pbay dps");
    serve_range(&r, masks, 1, 6);
    out = console_finish(&r);
    {
        const char *const rows[] = {
            dps_head[0], dps_head[1],
            "^1 +Ok +220Vac +Ok +Ok$", "^2 +Not_Present$",
            "^3 +Low +110Vac +Ok +Ok$", "^4 +Ok +220Vac +Failing +Ok$",
            "^5 +Ok +220Vac +Ok +Failed$", "^6 +Ok +220Vac +Ok +Ok$",
        };

        expect_lines(out, rows, G_N_ELEMENTS(rows));
    }

    /* Digits pick the supplies; only those are read. */
    console_send(&r, "pbay dps 53");
    serve_range(&r, masks, 3, 3);
    serve_range(&r, masks, 5, 5);
    g_free(out);
    out = console_finish(&r);
    g_assert_nonnull(strstr(out, "\r\n3 "));
    g_assert_nonnull(strstr(out, "\r\n5 "));
    g_assert_null(strstr(out, "\r\n1 "));

    masks[7 + 1] = 0x01;
    masks[7 + 2] = 0x02;
    console_send(&r, "pbay dcport");
    serve_range(&r, masks, 7, 14);
    g_free(out);
    out = console_finish(&r);
    {
        const char *const rows[] = {
            "^DcPort +State$", "^-+$",
            "^1 +48V On, Ok$", "^2 +48V Off$", "^3 +48V On, Failed$",
            "^4 +48V On, Ok$", "^5 +48V On, Ok$", "^6 +48V On, Ok$",
            "^7 +48V On, Ok$", "^8 +48V On, Ok$",
        };

        expect_lines(out, rows, G_N_ELEMENTS(rows));
    }

    /* A sensor that does not answer prints an error and the rest go on. */
    console_send(&r, "pbay dps 12");
    {
        Request q;

        bay_read(&r, &q);
        for (unsigned i = 0; i < 2; i++) {
            bay_read_raw(&r, &q);
        }
        serve_range(&r, masks, 2, 2);
    }
    g_free(out);
    out = console_finish(&r);
    g_assert_nonnull(strstr(out, "\r\n1 "));
    g_assert_nonnull(strstr(out, "**read error**"));
    g_assert_nonnull(strstr(out, "\r\n2 "));

    /* env: the setting, a blank line, then both tables. */
    g_free(out);
    out = console_command(&r, "pbay env on");
    g_assert_cmpstr(out, ==, "");
    console_send(&r, "pbay env");
    serve_range(&r, masks, 1, 14);
    g_free(out);
    out = console_finish(&r);
    g_assert_true(g_str_has_prefix(out, "Power Bay env monitoring enabled"
                                        "\r\n\r\nDPS "));
    g_assert_nonnull(strstr(out, "\r\nDcPort "));
    rig_stop(&r);
}

/* A FRU image: board area (date and manufacturer) and product area. */
static void fru_image(uint8_t *img, const char *serial, unsigned minutes,
                      bool good)
{
    uint8_t *board = img + 8, *prod = img + 24;
    const char *fields[] = { "SGI", "PWR SUPPLY", "030-1234", "A1", serial };
    unsigned n = 3;
    uint8_t sum = 0;

    memset(img, 0, 128);
    img[0] = 1;
    img[3] = 1;
    img[4] = 3;
    img[7] = -(1 + 1 + 3);
    board[0] = 1;
    board[1] = 2;
    board[3] = minutes & 0xff;
    board[4] = (minutes >> 8) & 0xff;
    board[5] = minutes >> 16;
    board[6] = 0xc1;
    for (unsigned i = 0; i < 16; i++) {
        sum += board[i];
    }
    board[15] = -sum;
    prod[0] = 1;
    prod[1] = 6;
    for (unsigned i = 0; i < G_N_ELEMENTS(fields); i++) {
        prod[n++] = 0xc0 | strlen(fields[i]);
        memcpy(prod + n, fields[i], strlen(fields[i]));
        n += strlen(fields[i]);
    }
    prod[n] = 0xc1;
    sum = 0;
    for (unsigned i = 0; i < 48; i++) {
        sum += prod[i];
    }
    prod[47] = -sum;
    if (!good) {
        prod[10] ^= 1;
    }
}

static void serve_fru(Rig *r, uint8_t device, const uint8_t *img)
{
    Request q;
    uint8_t d[1 + 128] = { 0x80 };

    memcpy(d + 1, img, 128);
    bay_read(r, &q);
    EXPECT_LINK(&q, 0x20, 0x4400, 0x22, 0x28, 0x11, device, 0x00, 0x00, 0x80);
    tunnel_reply(r, &q, d, sizeof(d));
}

/*
 * The FRU table and the serial. 1996-01-01 plus 2,720,160 minutes is
 * 2001-03-04.
 */
static void test_fru_and_serial(void)
{
    uint8_t good[128], bad[128];
    g_autofree char *out = NULL;
    Rig r;

    fru_image(good, "SN12345678A", 2720160, true);
    fru_image(bad, "SN12345678A", 2720160, false);
    rig_start(&r, true, "", "");
    serve_init(&r);
    wait_status(&r, 2);

    /* Slot 0 is the bay (device ae), 1 to 6 the supplies (a0 to aa). */
    console_send(&r, "pbay fru");
    serve_fru(&r, 0xae, good);
    for (unsigned i = 1; i <= 6; i++) {
        serve_fru(&r, 0xa0 + 2 * (i - 1), i == 2 ? bad : good);
    }
    out = console_finish(&r);
    {
        const char *const rows[] = {
            "^Dps +Mfg +Product Name +Part Number +Rev +Serial +Mfg Date$",
            "^-+( +-+)*$",
            "^Bay +SGI +PWR SUPPLY +030-1234 +A1 +SN12345678A +03/04/2001$",
            "^1 +SGI +PWR SUPPLY +030-1234 +A1 +SN12345678A +03/04/2001$",
            "^2 +\\*\\*FRU checksum error\\*\\*$",
            "^3 +SGI ", "^4 +SGI ", "^5 +SGI ", "^6 +SGI ",
        };

        expect_lines(out, rows, G_N_ELEMENTS(rows));
    }

    /* A negative argument selects the seventh device (ac). */
    console_send(&r, "pbay fru -1");
    serve_fru(&r, 0xac, good);
    g_free(out);
    out = console_finish(&r);
    g_assert_nonnull(strstr(out, "\r\nCard "));

    /* The serial is read from slot 0 once and then kept. */
    console_send(&r, "pbay serial");
    serve_fru(&r, 0xae, good);
    g_free(out);
    out = console_finish(&r);
    g_assert_cmpstr(out, ==, "SN12345678A\r\n");
    g_free(out);
    out = console_command(&r, "pbay serial");
    g_assert_cmpstr(out, ==, "SN12345678A\r\n");
    rig_stop(&r);
}

static void test_arguments_and_bricks(void)
{
    g_autofree char *out = NULL;
    Rig r;

    rig_start(&r, true, "", "");
    serve_init(&r);
    wait_status(&r, 2);
    out = console_command(&r, "pbay dps 7");
    g_assert_nonnull(strstr(out, "invalid arguments for 'pbay'"));
    g_free(out);
    out = console_command(&r, "pbay frobnicate");
    g_assert_nonnull(strstr(out, "invalid arguments for 'pbay'"));
    rig_stop(&r);
}

/*
 * The command takes the brick types in C356P%#=IXYRN&! only. Every letter of
 * the set takes it, and a letter outside the set whose link does open (G and
 * V do not, which test_closed_port covers) refuses it.
 */
static void test_command_brick_filter(void)
{
    const char *accepted = "C356P\x25#=IXYRN&!";
    const char *refused = "BDFHJKLMSTUWZ12";

    for (const char *c = accepted; *c; c++) {
        g_autofree char *out = NULL;
        g_autofree char *brick = g_strdup_printf("brick-type-code=%d,bay=1",
                                                 *c);
        Rig r;

        rig_brick = brick;
        rig_start(&r, true, ",pbay-tick-us=5000", "");
        rig_brick = RIG_BRICK;
        out = console_command(&r, "pbay");
        g_assert_cmpstr(out, ==, "PBay busy initializing\r\n");
        rig_stop(&r);
    }
    for (const char *c = refused; *c; c++) {
        g_autofree char *out = NULL;
        g_autofree char *brick = g_strdup_printf("brick-type-code=%d,bay=1",
                                                 *c);
        Rig r;

        rig_brick = brick;
        rig_start(&r, true, ",pbay-tick-us=5000", "");
        rig_brick = RIG_BRICK;
        out = console_command(&r, "pbay");
        g_assert_nonnull(strstr(out, "ERROR: command not found."));
        rig_stop(&r);
    }
}

/*
 * Polling does nothing until `pbay env on` (setting 85): with it off no
 * request is sent and no pass completes, and a pass completes after it is on.
 */
static void test_poll_gate(void)
{
    uint8_t clear[15] = { 0 };
    struct pollfd p;
    Rig r;

    rig_start(&r, true, "", "");
    serve_init(&r);
    wait_status(&r, 2);
    p = (struct pollfd){ .fd = r.bay, .events = POLLIN };
    /* Five poll periods of 100 ms. */
    g_usleep(500 * 1000);
    g_assert_cmpint(poll(&p, 1, 0), ==, 0);
    g_assert_cmpuint(l1_u(&r, "pbay-passes"), ==, 0);
    g_free(console_command(&r, "pbay env on"));
    serve_pass(&r, clear);
    g_assert_cmpuint(l1_u(&r, "pbay-passes"), ==, 1);
    g_free(console_command(&r, "pbay env off"));
    rig_stop(&r);
}

/*
 * An AC fail is logged on the fourth consecutive sighting, with three
 * immediate re-reads of the supplies between, so three sightings and then a
 * clear log nothing. A port failure is logged on the third, one re-read 100
 * ticks after the other.
 */
static void test_debounce(void)
{
    uint8_t ac[15] = { 0 }, port[15] = { 0 }, clear[15] = { 0 };
    Rig r;

    ac[1] = 0x08;
    port[9] = 0x02;
    rig_start(&r, true, "", "");
    serve_init(&r);
    monitoring_on(&r);
    g_free(command(&r, "log clear"));
    serve_pass(&r, clear);
    g_free(command(&r, "log clear"));

    /* Seen on the first read and two re-reads, gone on the third. */
    pass_expect_script(&r, &(Pass){ .first = ac, .ac_rereads = 3,
                                    .ac = { NULL, NULL, clear } }, NULL);
    pass_expect_script(&r, &(Pass){ .first = ac, .ac_rereads = 3 },
                       "Power Bay DPS 1 AC fail, lost redundancy");
    /* Still set on the next pass: no longer new. */
    pass_expect(&r, ac, NULL);
    pass_expect(&r, clear, NULL);

    /* A port seen on the first read and gone on the re-read. */
    pass_expect_script(&r, &(Pass){ .first = port, .port_rereads = 2,
                                    .port = { NULL, clear } }, NULL);
    pass_expect_script(&r, &(Pass){ .first = clear }, NULL);
    pass_expect_script(&r, &(Pass){ .first = port, .port_rereads = 2 },
                       "Power Bay DCport 3 fail, lost power to brick");
    rig_stop(&r);
}

/*
 * One fault per supply per pass: the first matching test wins, AC fail
 * before fail before predictive fail. The setting's single bits (bit 2 alone
 * is predictive fail) cannot be set through the pbay command, which writes
 * all of them or none, so only the all-on case is exercised.
 */
static void test_one_fault_per_supply(void)
{
    uint8_t all[15] = { 0 }, clear[15] = { 0 };
    g_autofree char *log = NULL;
    Rig r;

    all[1] = 0x0e;
    all[2] = 0x06;
    rig_start(&r, true, "", "");
    serve_init(&r);
    monitoring_on(&r);
    serve_pass(&r, clear);
    g_free(command(&r, "log clear"));

    serve_script(&r, &(Pass){ .first = all, .ac_rereads = 3 });
    log = command(&r, "log");
    g_assert_nonnull(strstr(log, "Power Bay DPS 1 AC fail"));
    g_assert_null(strstr(log, "DPS 1 fail"));
    g_assert_null(strstr(log, "DPS 1 predictive"));
    g_assert_nonnull(strstr(log, "Power Bay DPS 2 fail, lost redundancy"));
    g_assert_null(strstr(log, "DPS 2 predictive"));
    g_free(command(&r, "log clear"));

    g_free(console_command(&r, "pbay env off"));
    rig_stop(&r);
}

/*
 * reboot_l1 restarts every task, the power-bay link included. Settings 85
 * and 105 are the L1's, so they survive: monitoring is still on, and a supply
 * missing from the stored expected set is reported removed by the first pass
 * after the restart, where a fresh record would log nothing.
 */
static void test_restart_keeps_settings(void)
{
    uint8_t clear[15] = { 0 }, gone[15] = { 0 };
    g_autofree char *log = NULL;
    Rig r;

    gone[3] = 0x01;
    rig_start(&r, true, "", "");
    serve_init(&r);
    monitoring_on(&r);
    serve_pass(&r, clear);          /* the baseline: all six */
    console_send(&r, "reboot_l1");
    /* The controller is restarting for a second of virtual time. */
    qtest_clock_step(r.qts, NANOSECONDS_PER_SECOND * 2);
    wait_status(&r, 1);
    bay_drain(&r);
    serve_init(&r);
    wait_status(&r, 2);
    g_free(command(&r, "log clear"));
    /* Monitoring is still on: the pass comes unprompted. */
    serve_pass(&r, gone);
    log = command(&r, "log");
    g_assert_nonnull(strstr(log, "Power Bay DPS 3 removed"));
    rig_stop(&r);
}

/*
 * Task 3 (the command interpreter request) runs a pbay command that waits
 * for the bay: the reply is sent when the bay has answered, with status 0 for
 * a command that succeeded and -1 for one that failed.
 */
static void test_task3_pbay_reply(void)
{
    g_autofree char *out = NULL;
    int32_t code;
    Request q;
    Rig r;

    rig_start(&r, true, "", "");
    serve_init(&r);
    wait_status(&r, 2);

    task3_send(&r, LOCAL_COMMAND, "pbay version");
    bay_read(&r, &q);
    g_assert_cmphex(q.cmd, ==, 0xc0);
    REPLY(&r, &q, 0x00, 0x12, 0x00, 0x60, 0xa0, 0xd4, 0x39, 0x80);
    out = task3_receive(&r, &code, NULL, NULL);
    g_assert_cmpstr(out, ==,
                    "PBay FW Version 00.18, Built 09/29/2000 14:00:00\n");
    g_assert_cmpint(code, ==, 0);

    task3_send(&r, LOCAL_COMMAND, "pbay reset");
    bay_read(&r, &q);
    EXPECT_LINK(&q, 0x20, 0x4400, 0x22, 0x18, 0x03);
    TUNNEL_REPLY(&r, &q, 0x00);
    g_free(out);
    out = task3_receive(&r, &code, NULL, NULL);
    g_assert_cmpstr(out, ==, "Ok\n");
    g_assert_cmpint(code, ==, 0);

    /* No reply to the reset: the failure text, and status -1. */
    task3_send(&r, LOCAL_COMMAND, "pbay reset");
    g_free(out);
    out = task3_receive(&r, &code, NULL, NULL);
    g_assert_cmpstr(out, ==, "Timeout\n");
    g_assert_cmpint(code, ==, -1);
    rig_stop(&r);
}

/*
 * A reply holds the lines that fit in 121 bytes of fields, two bytes of
 * framing and the text with its terminator each, and no partial line.
 */
static void test_task3_pbay_truncation(void)
{
    uint8_t masks[15] = { 0 };
    g_autofree char *full = NULL, *out = NULL;
    g_auto(GStrv) lines = NULL;
    unsigned fields, used = 0, i;
    size_t wire;
    Rig r;

    rig_start(&r, true, "", "");
    serve_init(&r);
    wait_status(&r, 2);

    console_send(&r, "pbay dps");
    serve_range(&r, masks, 1, 6);
    full = console_finish(&r);
    lines = g_strsplit(full, "\r\n", -1);

    task3_send(&r, LOCAL_COMMAND, "pbay dps");
    serve_range(&r, masks, 1, 6);
    out = task3_receive(&r, NULL, &fields, &wire);
    g_assert_cmpuint(fields, >, 0);
    for (i = 0; i < fields; i++) {
        used += 2 + strlen(lines[i]);
    }
    g_assert_cmpuint(used, <=, 121);
    g_assert_cmpuint(wire, ==, 6 + used);
    /* The table is longer than the reply, and the next line did not fit. */
    g_assert_cmpuint(g_strv_length(lines) - 1, >, fields);
    g_assert_cmpuint(used + 2 + strlen(lines[fields]), >, 121);
    rig_stop(&r);
}

/*
 * The limit is 121 bytes of fields whatever the lines are. The log's lines
 * are the text of an entry after 18 bytes of date and time, so entries make
 * lines of any length: three lines that add up to 121 with their framing
 * all fit, with a fourth line to show the reply stopped for want of room
 * and not of lines, and a line that would bring 121 bytes to 122 does not.
 * Either side of the limit is a byte away from a wrong limit.
 */
static void log_lines_reply(Rig *r, const unsigned *texts, unsigned count,
                            unsigned *fields, size_t *wire)
{
    g_autofree char *out = NULL;

    g_free(command(r, "log clear"));
    for (unsigned i = 0; i < count; i++) {
        g_autofree char *line = g_strdup_printf("log insert %.*s",
                                                (int)texts[i],
                                                "abcdefghijklmnopqrstuvwxyz"
                                                "abcdefghijklmnopqrstuvwxyz");

        g_free(command(r, line));
    }
    task3_send(r, LOCAL_COMMAND, "log");
    out = task3_receive(r, NULL, fields, wire);
}

static void test_task3_reply_limit(void)
{
    /* A line costs 2 + 18 + its text; the first three cost 41 + 41 + 39. */
    const unsigned exact[] = { 21, 21, 19, 1 };
    /* 41 + 41 = 82, and a third line of 40 would make 122. */
    const unsigned over[] = { 21, 21, 20, 1 };
    unsigned fields;
    size_t wire;
    Rig r;

    rig_start(&r, true, "", "");
    serve_init(&r);
    wait_status(&r, 2);

    log_lines_reply(&r, exact, G_N_ELEMENTS(exact), &fields, &wire);
    g_assert_cmpuint(fields, ==, 3);
    g_assert_cmpuint(wire, ==, 6 + 121);

    log_lines_reply(&r, over, G_N_ELEMENTS(over), &fields, &wire);
    g_assert_cmpuint(fields, ==, 2);
    g_assert_cmpuint(wire, ==, 6 + 82);
    rig_stop(&r);
}

/*
 * A task-3 request relayed to an I/O brick that has a power-bay link runs
 * on that brick, and its reply, sent once the bay has answered, goes back to
 * the requester and not out of the I/O brick's own link.
 */
static void test_task3_pbay_forwarded(void)
{
    g_autofree char *out = NULL;
    int32_t code;
    Request q;
    Rig r;

    rig_start_pair(&r);
    serve_init(&r);
    wait_status(&r, 2);

    task3_send(&r, ATTACHED_IO_COMMAND, "pbay version");
    bay_read(&r, &q);
    g_assert_cmphex(q.cmd, ==, 0xc0);
    REPLY(&r, &q, 0x00, 0x12, 0x00, 0x60, 0xa0, 0xd4, 0x39, 0x80);
    out = task3_receive(&r, &code, NULL, NULL);
    g_assert_cmpstr(out, ==,
                    "PBay FW Version 00.18, Built 09/29/2000 14:00:00\n");
    g_assert_cmpint(code, ==, 0);
    rig_stop(&r);
}

/*
 * A new slot number takes effect at the next restart, and the power-bay
 * task's start delay (100 * bay + 300 ticks) follows it. The bound is a
 * lower one: the old slot's delay is far shorter than the new one's.
 */
static void test_restart_uses_new_slot(void)
{
    int64_t t0, t1;
    Request q;
    Rig r;

    rig_start(&r, true, ",pbay-tick-us=500", "");
    serve_init(&r);
    wait_status(&r, 2);
    g_free(console_command(&r, "brick slot 39"));
    t0 = g_get_monotonic_time();
    console_send(&r, "reboot_l1");
    qtest_clock_step(r.qts, NANOSECONDS_PER_SECOND * 2);
    wait_status(&r, 1);
    bay_drain(&r);
    bay_read_raw(&r, &q);
    t1 = g_get_monotonic_time();
    g_assert_cmphex(q.cmd, ==, 0x10);
    g_assert_cmpint(t1 - t0, >=, (100 * 39 + 300) * 500 * 9 / 10);
    rig_stop(&r);
}

/*
 * The poll task waits 6000 + 100 * bay ticks (bay 7: 6700), and a request
 * restarts the wait. The bound is a lower one, measured from before the
 * reply that restarts the wait: a late read of the next request can only
 * make the interval longer.
 */
static void test_poll_period(void)
{
    int64_t t0, t1;
    Request q;
    Rig r;

    rig_brick = RIG_BRICK_SLOT7;
    rig_start(&r, true, ",pbay-poll-ms=0,pbay-tick-us=100", "");
    rig_brick = RIG_BRICK;
    serve_init(&r);
    wait_status(&r, 2);
    g_free(console_command(&r, "pbay env on"));
    g_usleep(300 * 1000);
    /* A request to the bay restarts the wait; setting a mask does not. */
    console_send(&r, "pbay version");
    bay_read(&r, &q);
    t0 = g_get_monotonic_time();
    REPLY(&r, &q, 0x00, 0x12, 0x00, 0x60, 0xa0, 0xd4, 0x39, 0x80);
    g_free(console_finish(&r));
    bay_read(&r, &q);
    t1 = g_get_monotonic_time();
    g_assert_cmphex(q.cmd, ==, 0x20);
    /* The clocks read whole microseconds. */
    g_assert_cmpint(t1 - t0 + 2, >=, 6700 * 100);
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
 * ticks and resends. Both bounds are lower ones and are measured from before
 * QEMU launched, which is before the L1's timers started, so a slow test or
 * a loaded host can only lengthen what they measure.
 */
static void test_start_delay_and_resends(void)
{
    Request q, again;
    int64_t t1, t2;
    Rig r;

    rig_brick = RIG_BRICK_SLOT7;
    rig_start(&r, true, ",pbay-tick-us=500", "");
    rig_brick = RIG_BRICK;
    bay_read_raw(&r, &q);
    t1 = g_get_monotonic_time();
    g_assert_cmpint(t1 - r.started, >=, 1000 * 500);
    g_assert_cmphex(q.cmd, ==, 0x10);
    bay_read_raw(&r, &q);
    g_assert_cmphex(q.cmd, ==, 0x02);
    bay_read_raw(&r, &again);
    t2 = g_get_monotonic_time();
    g_assert_cmpmem(again.body, again.len, q.body, q.len);
    /*
     * The address request follows the silent one at once. A read lasts at
     * least 100 ticks, then 20 more before the resend.
     */
    g_assert_cmpint(t2 - r.started, >=, (1000 + 120) * 500);
    rig_stop(&r);
}

/*
 * A frame that fails the reply checks costs a read, and three of them end
 * the transmission without waiting out a read timeout: the resend follows
 * after 20 ticks, well before the 100 to 200 ticks a silent read takes. The
 * only upper bound in this file: it is the 20 ticks against the least a
 * silent read lasts, 100, so a host that stalls the test for under 80 ticks
 * (2.4 s at 30 ms a tick) cannot fail it. The tick is long for that reason;
 * the start delay of this brick, 400 ticks (12 s), is the price, and stays
 * inside the 20 s read timeout.
 */
static void test_bad_frames_cost_reads(void)
{
    Request q, again;
    int64_t t0, t1;
    Rig r;

    rig_start(&r, true, ",pbay-tick-us=30000", "");
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
    g_assert_cmpint(t1 - t0, <, 100 * 30000);

    /*
     * Two bad frames leave a read: a good reply after them is taken, and
     * the next request follows and not a resend. This is the part that
     * needs no clock, and a bad frame that ended the transmission at once
     * would fail it.
     */
    for (unsigned i = 0; i < 2; i++) {
        bay_reply_from(&r, &again, 0x4400, (uint8_t[]){ 0x00, 0x00, 0x44 }, 3,
                       true);
    }
    REPLY(&r, &again, 0x00, 0x00, 0x44);
    bay_read_raw(&r, &q);
    g_assert_cmphex(q.cmd, ==, 0x12);
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
    out = console_command(&r, "pbay");
    g_assert_cmpstr(out, ==, "PBay communication failure\r\n");
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
    monitoring_on(&r);

    /*
     * Pulling supply 1 shows in the log after a later pass. A reply holds
     * only the first lines of the log, so start it empty.
     */
    while (l1_u(&r, "pbay-passes") < 1) {
        g_assert_cmpint(g_get_monotonic_time(), <, end);
        g_usleep(5000);
    }
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
 * The power-bay task does not start, and the L1 refuses the link, on 0 4 7 8
 * 9 A E G O Q V. G's second port is a 38400 baud console, not the bay link.
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

/* A tick of zero would run without pause, so it is refused. */
static void test_tick_refused(void)
{
    g_autofree char *err = NULL;
    int status;

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
    qtest_add_func("/sgi-l1-pbay/sensor-map", test_sensor_map);
    qtest_add_func("/sgi-l1-pbay/fru-fallback", test_fru_fallback);
    qtest_add_func("/sgi-l1-pbay/states", test_states);
    qtest_add_func("/sgi-l1-pbay/version-and-reset", test_version_and_reset);
    qtest_add_func("/sgi-l1-pbay/dps-dcport-env", test_dps_dcport_env);
    qtest_add_func("/sgi-l1-pbay/fru-and-serial", test_fru_and_serial);
    qtest_add_func("/sgi-l1-pbay/arguments-and-bricks",
                   test_arguments_and_bricks);
    qtest_add_func("/sgi-l1-pbay/command-brick-filter",
                   test_command_brick_filter);
    qtest_add_func("/sgi-l1-pbay/poll-gate", test_poll_gate);
    qtest_add_func("/sgi-l1-pbay/debounce", test_debounce);
    qtest_add_func("/sgi-l1-pbay/one-fault-per-supply",
                   test_one_fault_per_supply);
    qtest_add_func("/sgi-l1-pbay/restart-keeps-settings",
                   test_restart_keeps_settings);
    qtest_add_func("/sgi-l1-pbay/task3-reply", test_task3_pbay_reply);
    qtest_add_func("/sgi-l1-pbay/task3-truncation",
                   test_task3_pbay_truncation);
    qtest_add_func("/sgi-l1-pbay/task3-reply-limit", test_task3_reply_limit);
    qtest_add_func("/sgi-l1-pbay/task3-forwarded", test_task3_pbay_forwarded);
    qtest_add_func("/sgi-l1-pbay/restart-new-slot",
                   test_restart_uses_new_slot);
    qtest_add_func("/sgi-l1-pbay/period", test_poll_period);
    qtest_add_func("/sgi-l1-pbay/receive-rules", test_receive_rules);
    qtest_add_func("/sgi-l1-pbay/start-delay-and-resends",
                   test_start_delay_and_resends);
    qtest_add_func("/sgi-l1-pbay/bad-frames-cost-reads",
                   test_bad_frames_cost_reads);
    qtest_add_func("/sgi-l1-pbay/no-bay", test_no_bay);
    qtest_add_func("/sgi-l1-pbay/bay-model", test_with_bay_model);
    qtest_add_func("/sgi-l1-pbay/closed-port", test_closed_port);
    qtest_add_func("/sgi-l1-pbay/tick-refused", test_tick_refused);
    qtest_add_func("/sgi-l1-pbay/blocks-migration", test_blocks_migration);
    qtest_add_func("/sgi-l1-pbay/unset", test_unset_is_unchanged);

    return g_test_run();
}
