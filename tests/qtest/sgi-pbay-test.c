/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Power-bay controller link: the bay end of the L1's power-bay serial link,
 * driven over a socket the test holds. Each case cites the rows of the
 * power-bay controller datasheet it checks. Wire bytes are written out
 * rather than built by the helpers wherever a case is about the encoding.
 */
#include "qemu/osdep.h"
#include <poll.h>
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"

#define BAY_LO  0x00
#define BAY_HI  0x44
#define L1_LO   0x02
#define L1_HI   0x01
#define IO_TIMEOUT_MS 20000

typedef struct Rig {
    QTestState *qts;
    int fd;
    char *log;
} Rig;

static uint16_t checksum(const uint8_t *body, size_t len)
{
    unsigned sum = 0;

    for (size_t i = 0; i < len; i++) {
        sum += body[i];
    }
    return 0xffff - (sum + len + 1);
}

/* Datasheet link table: 0xa0, 0xa5, 0xaa travel as 0xaa 0xb0/0xb5/0xba. */
static void stuff(GByteArray *wire, const uint8_t *body, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        static const uint8_t esc = 0xaa;
        uint8_t b = body[i], m;

        if (b == 0xa0 || b == 0xa5 || b == 0xaa) {
            m = b == 0xa0 ? 0xb0 : b == 0xa5 ? 0xb5 : 0xba;
            g_byte_array_append(wire, &esc, 1);
            g_byte_array_append(wire, &m, 1);
        } else {
            g_byte_array_append(wire, &b, 1);
        }
    }
}

static void wire_write(Rig *r, const uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t n = write(r->fd, buf, len);

        g_assert_cmpint(n, >, 0);
        buf += n;
        len -= n;
    }
}

/* @body, its checksum (corrupted if @bad_sum), stuffed and terminated. */
static void frame_wire(GByteArray *wire, const uint8_t *body, size_t len,
                       bool bad_sum)
{
    g_autoptr(GByteArray) raw = g_byte_array_new();
    uint16_t sum = checksum(body, len);
    uint8_t end = 0xa5;

    g_byte_array_append(raw, body, len);
    g_byte_array_append(raw, (uint8_t[]){ sum & 0xff, (sum >> 8) ^ bad_sum },
                        2);
    stuff(wire, raw->data, raw->len);
    g_byte_array_append(wire, &end, 1);
}

static void send_body(Rig *r, const uint8_t *body, size_t len, bool bad_sum)
{
    g_autoptr(GByteArray) wire = g_byte_array_new();

    frame_wire(wire, body, len, bad_sum);
    wire_write(r, wire->data, wire->len);
}

/*
 * A request from the L1 at 0x0102 to @dst: header, argument at offset 7 and
 * data after it.
 */
static size_t build(uint8_t *out, uint16_t dst, uint16_t src, uint8_t ctl,
                    uint8_t cmd, const uint8_t *tail, size_t tail_len)
{
    size_t n = 0;

    out[n++] = dst & 0xff;
    out[n++] = dst >> 8;
    out[n++] = src & 0xff;
    out[n++] = src >> 8;
    out[n++] = 0;
    out[n++] = ctl;
    out[n++] = cmd;
    memcpy(out + n, tail, tail_len);
    return n + tail_len;
}

static void request(Rig *r, uint8_t ctl, uint8_t cmd, const uint8_t *tail,
                    size_t tail_len)
{
    uint8_t body[64];

    send_body(r, body, build(body, 0x4400, 0x0102, ctl, cmd, tail, tail_len),
              false);
}

/*
 * A tunnelled request: the target byte, the network function (already
 * shifted), the command, then the data from offset 10.
 */
static void tunnel(Rig *r, uint8_t target, uint8_t netfn, uint8_t cmd,
                   const uint8_t *data, size_t n)
{
    uint8_t tail[32] = { target, netfn, cmd };

    memcpy(tail + 3, data, n);
    request(r, 0x03, 0x20, tail, 3 + n);
}

/* One raw frame off the wire up to and including its 0xa5. */
static size_t wire_read(Rig *r, uint8_t *buf, size_t max)
{
    size_t n = 0;

    for (;;) {
        struct pollfd p = { .fd = r->fd, .events = POLLIN };
        ssize_t got;

        g_assert_cmpint(poll(&p, 1, IO_TIMEOUT_MS), ==, 1);
        got = read(r->fd, buf + n, 1);
        g_assert_cmpint(got, ==, 1);
        n++;
        g_assert_cmpuint(n, <=, max);
        if (buf[n - 1] == 0xa5) {
            return n;
        }
    }
}

/*
 * Read one reply, check it is well stuffed and its checksum holds, and
 * return the unstuffed body without the checksum.
 */
static size_t reply(Rig *r, uint8_t *body)
{
    uint8_t raw[512];
    size_t n = wire_read(r, raw, sizeof(raw));
    size_t len = 0;
    uint16_t sum;

    /* The bay opens every reply with a start byte. */
    g_assert_cmphex(raw[0], ==, 0xa0);
    for (size_t i = 1; i < n - 1; i++) {
        g_assert_cmphex(raw[i], !=, 0xa0);
        g_assert_cmphex(raw[i], !=, 0xa5);
        if (raw[i] == 0xaa) {
            i++;
            g_assert_cmpuint(i, <, n - 1);
            switch (raw[i]) {
            case 0xb0:
                body[len++] = 0xa0;
                break;
            case 0xb5:
                body[len++] = 0xa5;
                break;
            case 0xba:
                body[len++] = 0xaa;
                break;
            default:
                g_assert_not_reached();
            }
        } else {
            body[len++] = raw[i];
        }
    }
    g_assert_cmpuint(len, >=, 9);
    len -= 2;
    sum = body[len] | body[len + 1] << 8;
    g_assert_cmphex(sum, ==, checksum(body, len));
    return len;
}

/*
 * Expect the reply to command @cmd with control @ctl to the L1 and the
 * bytes from offset 7 on equal to @want.
 */
static void expect(Rig *r, uint8_t ctl, uint8_t cmd, const uint8_t *want,
                   size_t want_len)
{
    uint8_t body[512];
    size_t len = reply(r, body);
    const uint8_t head[] = { L1_LO, L1_HI, BAY_LO, BAY_HI, 0, ctl, cmd };

    g_assert_cmpmem(body, MIN(len, sizeof(head)), head, sizeof(head));
    g_assert_cmpmem(body + 7, len - 7, want, want_len);
}

/*
 * The reply to a tunnelled request: link completion code, the target byte
 * echoed, netfn + 4, the command, the inner completion code, then @data.
 */
#define EXPECT_TUNNEL(r, nf, cmd, ...) do { \
        const uint8_t w_[] = { 0x00, 0x22, (nf) + 4, cmd, 0x00, \
                               ##__VA_ARGS__ }; \
        expect(r, 0x83, 0x20, w_, sizeof(w_)); \
    } while (0)

/* An error reply: header only, with the completion code. */
#define EXPECT_ERROR(r, ctl, cmd, cc) EXPECT(r, ctl, cmd, cc)

#define EXPECT(r, ctl, cmd, ...) do { \
        const uint8_t w_[] = { __VA_ARGS__ }; \
        expect(r, ctl, cmd, w_, sizeof(w_)); \
    } while (0)

#define REQUEST(r, ctl, cmd, ...) do { \
        const uint8_t t_[] = { __VA_ARGS__ }; \
        request(r, ctl, cmd, t_, sizeof(t_)); \
    } while (0)

#define TUNNEL(r, nf, cmd, ...) do { \
        const uint8_t a_[] = { 0, ##__VA_ARGS__ }; \
        tunnel(r, 0x22, nf, cmd, a_ + 1, sizeof(a_) - 1); \
    } while (0)

/* Send @n bytes of body and checksum exactly as given. */
static void send_exact(Rig *r, const uint8_t *bytes, size_t n)
{
    g_autoptr(GByteArray) wire = g_byte_array_new();
    uint8_t end = 0xa5;

    stuff(wire, bytes, n);
    g_byte_array_append(wire, &end, 1);
    wire_write(r, wire->data, wire->len);
}

/* Expect exactly the reply @bytes (body and checksum) on the wire. */
static void expect_exact(Rig *r, const uint8_t *bytes, size_t n)
{
    g_autoptr(GByteArray) wire = g_byte_array_new();
    uint8_t start = 0xa0, end = 0xa5, got[512];
    size_t len;

    g_byte_array_append(wire, &start, 1);
    stuff(wire, bytes, n);
    g_byte_array_append(wire, &end, 1);
    len = wire_read(r, got, sizeof(got));
    g_assert_cmpmem(got, len, wire->data, wire->len);
}

/* A sheet vector: a request and its reply, each with its checksum. */
#define UNPAREN(...) __VA_ARGS__
#define VECTOR(r, req_, rep_) do { \
        const uint8_t q_[] = { UNPAREN req_ }; \
        const uint8_t a_[] = { UNPAREN rep_ }; \
        send_exact(r, q_, sizeof(q_)); \
        expect_exact(r, a_, sizeof(a_)); \
    } while (0)

/*
 * Prove the bay took everything sent so far and answered nothing: a command
 * it always answers must be the next frame back.
 */
static void sentinel(Rig *r)
{
    REQUEST(r, 0x09, 0x02, 0);
    EXPECT(r, 0x89, 0x02, 0x00, 0x00, 0x44);
}

static void rig_start_with(Rig *r, const char *props, const char *extra)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("pbay-XXXXXX", &error);
    int sv[2];

    g_assert_no_error(error);
    r->log = g_build_filename(dir, "log", NULL);
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    r->qts = qtest_initf("-M none -S -d unimp,guest_errors -D %s "
                         "-chardev socket,id=bay,fd=%d "
                         "-device sgi-pbay,id=pbay,chardev=bay%s %s",
                         r->log, sv[1], props, extra);
    close(sv[1]);
    r->fd = sv[0];
}

static void rig_start(Rig *r, const char *props)
{
    rig_start_with(r, props, "");
}

static uint64_t pbay_u32(Rig *r, const char *prop)
{
    QDict *resp = qtest_qmp(r->qts, "{'execute':'qom-get','arguments':"
                            "{'path':'/machine/peripheral/pbay',"
                            "'property':%s}}", prop);
    uint64_t v = qdict_get_int(resp, "return");

    qobject_unref(resp);
    return v;
}

static void rig_stop(Rig *r)
{
    g_autofree char *dir = g_path_get_dirname(r->log);

    close(r->fd);
    qtest_quit(r->qts);
    unlink(r->log);
    g_assert_cmpint(rmdir(dir), ==, 0);
    g_free(r->log);
}

/* The whole log, read once the machine has quit or flushed. */
static char *rig_log(Rig *r)
{
    char *text = NULL;

    /* A sentinel round trip first so the line has been written. */
    sentinel(r);
    g_assert_true(g_file_get_contents(r->log, &text, NULL, NULL));
    return text;
}

static void test_worked_frame(void)
{
    /* Worked frame: the 0xc0 reply to an L1 at 0x0102, as sent on the wire. */
    Rig r;

    rig_start(&r, "");
    REQUEST(&r, 0x04, 0xc0);
    expect_exact(&r, (uint8_t[]){
        0x02, 0x01, 0x00, 0x44, 0x00, 0x84, 0xc0, 0x00, 0x12, 0x00, 0x60,
        0xa0, 0xd4, 0x39, 0x80, 0xc5, 0xfb }, 17);
    rig_stop(&r);
}
static void test_address_reply(void)
{
    /* Worked 0x02 reply, control tag 5; and it ignores the destination. */
    static const uint8_t want[] = {
        0x02, 0x01, 0x00, 0x44, 0x00, 0x85, 0x02, 0x00, 0x00, 0x44, 0xe2,
        0xfe,
    };
    uint8_t body[64];
    Rig r;

    rig_start(&r, "");
    REQUEST(&r, 0x05, 0x02);
    expect_exact(&r, want, sizeof(want));

    send_body(&r, body, build(body, 0x0000, 0x0102, 0x05, 0x02, NULL, 0),
              false);
    expect_exact(&r, want, sizeof(want));
    rig_stop(&r);
}
static void test_bad_checksum(void)
{
    uint8_t body[64];
    Rig r;

    rig_start(&r, "");
    /* One data bit altered with the checksum unchanged: ignored. */
    send_body(&r, body, build(body, 0x4400, 0x0102, 0x05, 0x02, NULL, 0),
              true);
    sentinel(&r);
    rig_stop(&r);
}

static void test_destination(void)
{
    uint8_t body[64], got[64];
    Rig r;

    rig_start(&r, "");
    /* A request for another address is ignored, except 0x02 and 0xc0. */
    send_body(&r, body, build(body, 0x4500, 0x0102, 0x01, 0x11, NULL, 0),
              false);
    sentinel(&r);
    /* The reply to 0xc0 is from the address the request used. */
    send_body(&r, body, build(body, 0x4500, 0x0102, 0x01, 0xc0, NULL, 0),
              false);
    g_assert_cmpuint(reply(&r, got), ==, 15);
    g_assert_cmpmem(got, 7, ((uint8_t[]){ 2, 1, 0x00, 0x45, 0, 0x81, 0xc0 }),
                    7);
    rig_stop(&r);
}
static void test_stuffing(void)
{
    static const uint8_t head[] = {
        0xa0, 0xaa, 0xba, 0xaa, 0xb5, 0x00, 0x44, 0x00, 0x85, 0x02, 0x00,
        0x00, 0x44,
    };
    static const uint8_t data[] = { 0xaa, 0xb0 };
    uint8_t body[64], raw[128], key4[] = { 0x00, 0x00, 0x04, 0x00, 0x00,
                                           0xff };
    bool found = false;
    size_t n;
    Rig r;

    rig_start(&r, "");
    /*
     * An L1 whose address 0xa5aa needs stuffing: the request arrives
     * stuffed, and the reply addressed to it goes back stuffed.
     */
    send_body(&r, body, build(body, 0x4400, 0xa5aa, 0x05, 0x02, NULL, 0),
              false);
    n = wire_read(&r, raw, sizeof(raw));
    g_assert_cmpuint(n, >=, sizeof(head) + 3);
    g_assert_cmpmem(raw, sizeof(head), head, sizeof(head));

    /*
     * A reply to that L1 carrying all three bytes (the address, and the
     * locator record's 0xa0) has no bare 0xa0 or 0xaa.
     */
    {
        uint8_t tail[3 + sizeof(key4)] = { 0x22, 0x28, 0x23 };

        memcpy(tail + 3, key4, sizeof(key4));
        send_body(&r, body, build(body, 0x4400, 0xa5aa, 0x03, 0x20, tail,
                                  sizeof(tail)), false);
    }
    n = wire_read(&r, raw, sizeof(raw));
    for (size_t i = 0; i + sizeof(data) <= n; i++) {
        found |= !memcmp(raw + i, data, sizeof(data));
    }
    g_assert_true(found);
    for (size_t i = 1; i < n - 1; i++) {
        g_assert_cmphex(raw[i], !=, 0xa0);
        g_assert_cmphex(raw[i], !=, 0xa5);
        if (raw[i] == 0xaa) {
            g_assert_true(raw[i + 1] == 0xb0 || raw[i + 1] == 0xb5 ||
                          raw[i + 1] == 0xba);
            i++;
        }
    }
    rig_stop(&r);
}
static void test_simple_commands(void)
{
    Rig r;

    rig_start(&r, "");
    REQUEST(&r, 0x01, 0x0a, 0);
    EXPECT(&r, 0x81, 0x0a, 0x00, 0x01, 0x08, 0x02);
    REQUEST(&r, 0x02, 0x0b, 0);
    EXPECT(&r, 0x82, 0x0b, 0x00, 0x00);
    REQUEST(&r, 0x03, 0x10, 0);
    REQUEST(&r, 0x04, 0xa2, 0);
    sentinel(&r);
    REQUEST(&r, 0x05, 0x11, 0);
    EXPECT(&r, 0x85, 0x11, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
           0);
    /* The L1 sends the bay address as data; the reply is header only. */
    REQUEST(&r, 0x06, 0x12, 0x00, 0x44);
    EXPECT(&r, 0x86, 0x12, 0x00);
    REQUEST(&r, 0x07, 0x13, 0);
    EXPECT(&r, 0x87, 0x13, 0x00, 0x22);
    REQUEST(&r, 0x08, 0xc0, 0);
    EXPECT(&r, 0x88, 0xc0, 0x00, 0x12, 0x00, 0x60, 0xa0, 0xd4, 0x39, 0x80);
    rig_stop(&r);
}

static void test_state_code(void)
{
    Rig r;

    /* 0x0b returns the bit number (1-8) of a one-hot state code, or 0. */
    rig_start(&r, ",state-code=0x01");
    REQUEST(&r, 0x01, 0x0b, 0);
    EXPECT(&r, 0x81, 0x0b, 0x00, 0x01);
    rig_stop(&r);
    rig_start(&r, ",state-code=0x80");
    REQUEST(&r, 0x01, 0x0b, 0);
    EXPECT(&r, 0x81, 0x0b, 0x00, 0x08);
    rig_stop(&r);
}

static void test_tunnelled(void)
{
    Rig r;

    rig_start(&r, "");
    /*
     * Reply: link completion code, target byte, netfn + 4, command, inner
     * completion code, data. The preceding byte is echoed whatever it is.
     */
    TUNNEL(&r, 0x00, 0x00);
    EXPECT_TUNNEL(&r, 0x00, 0x00, 0x00, 0x22, 0x22, 0x00, 0x00);
    TUNNEL(&r, 0x18, 0x04);
    EXPECT_TUNNEL(&r, 0x18, 0x04, 0x56, 0x00);
    TUNNEL(&r, 0x28, 0x20);
    EXPECT_TUNNEL(&r, 0x28, 0x20, 0x01, 0x0c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                  0, 0);
    /* The handler returns its own reply bytes whatever was asked. */
    TUNNEL(&r, 0x28, 0x10, 0x05, 1, 2, 3, 4);
    EXPECT_TUNNEL(&r, 0x28, 0x10, 0x10, 0x00, 0x10, 0x00);
    TUNNEL(&r, 0x18, 0x03);
    EXPECT_TUNNEL(&r, 0x18, 0x03);
    /* The unit in the network function's low bits is kept in the reply. */
    {
        uint8_t tail[] = { 0x22, 0x19, 0x04 };

        request(&r, 0x03, 0x20, tail, sizeof(tail));
        EXPECT(&r, 0x83, 0x20, 0x00, 0x22, 0x1d, 0x04, 0x00, 0x56, 0x00);
    }
    rig_stop(&r);
}

static void test_streamed_read(void)
{
    g_autofree char *log = NULL;
    Rig r;

    rig_start(&r, "");
    /* Device byte at offset 10, count at offset 13. */
    for (uint8_t dev = 0xa0; dev <= 0xae; dev += 2) {
        TUNNEL(&r, 0x28, 0x11, dev, 0x00, 0x00, 0x04);
        EXPECT_TUNNEL(&r, 0x28, 0x11, 0x04, 0, 0, 0, 0);
    }
    /* A count of 0 streams 256 samples. */
    TUNNEL(&r, 0x28, 0x11, 0xa0, 0x00, 0x00, 0x00);
    {
        uint8_t got[512];

        g_assert_cmpuint(reply(&r, got), ==, 7 + 5 + 1 + 256);
    }
    /* A device byte outside the list is an error. */
    TUNNEL(&r, 0x28, 0x11, 0xa1, 0x00, 0x00, 0x04);
    EXPECT_ERROR(&r, 0x83, 0x20, 0xc1);
    TUNNEL(&r, 0x28, 0x11, 0xb0, 0x00, 0x00, 0x04);
    EXPECT_ERROR(&r, 0x83, 0x20, 0xc1);
    log = rig_log(&r);
    g_assert_nonnull(strstr(log, "sgi-pbay: device 0xa0 read returns zeros"));
    rig_stop(&r);
}

static void test_tunnelled_errors(void)
{
    g_autofree char *log = NULL;
    Rig r;

    rig_start(&r, "");
    /* Pair 0x10,0x23 and an unknown pair are header-only 0xc1 replies. */
    TUNNEL(&r, 0x10, 0x23);
    EXPECT_ERROR(&r, 0x83, 0x20, 0xc1);
    TUNNEL(&r, 0x18, 0x77);
    EXPECT_ERROR(&r, 0x83, 0x20, 0xc1);
    /* So is a network function the bay does not recognise (status 0). */
    TUNNEL(&r, 0x30, 0x77);
    EXPECT_ERROR(&r, 0x83, 0x20, 0xc1);
    TUNNEL(&r, 0x08, 0x00);
    EXPECT_ERROR(&r, 0x83, 0x20, 0xc1);
    log = rig_log(&r);
    g_assert_nonnull(strstr(log, "sgi-pbay: unimplemented tunnelled "
                                 "request 0x18,0x77"));
    g_assert_nonnull(strstr(log, "sgi-pbay: unimplemented tunnelled "
                                 "request 0x30,0x77"));
    rig_stop(&r);
}

static void pbay_set_u8(Rig *r, const char *prop, uint8_t v)
{
    QDict *resp = qtest_qmp(r->qts, "{'execute':'qom-set','arguments':"
                            "{'path':'/machine/peripheral/pbay',"
                            "'property':%s,'value':%d}}", prop, v);

    g_assert_false(qdict_haskey(resp, "error"));
    qobject_unref(resp);
}

/*
 * Datasheet error replies: with the loader's status 0xff, a command in the
 * table answers 0xff (header-only for a link command, the short prefix for
 * a tunnelled request of network function 00 10 18 28, silence for any
 * other), 0xa2 stays silent and a command not in the table still gets 0xc1.
 */
static void test_frame_status(void)
{
    Rig r;

    rig_start(&r, "");
    pbay_set_u8(&r, "frame-status", 0xff);

    REQUEST(&r, 0x01, 0x0a);
    EXPECT_ERROR(&r, 0x81, 0x0a, 0xff);
    REQUEST(&r, 0x02, 0x02, 0);
    EXPECT_ERROR(&r, 0x82, 0x02, 0xff);
    REQUEST(&r, 0x03, 0x10);
    EXPECT_ERROR(&r, 0x83, 0x10, 0xff);
    REQUEST(&r, 0x04, 0x55);
    EXPECT_ERROR(&r, 0x84, 0x55, 0xc1);

    /* 0xa2 is silent: the next reply is the one to the request after it. */
    REQUEST(&r, 0x05, 0xa2);
    REQUEST(&r, 0x06, 0x13);
    EXPECT_ERROR(&r, 0x86, 0x13, 0xff);

    /*
     * Link code 0xff, the echo, netfn + 4, the command and inner code 0xff,
     * with no data, for any pair of a recognised network function.
     */
    TUNNEL(&r, 0x00, 0x00);
    EXPECT(&r, 0x83, 0x20, 0xff, 0x22, 0x04, 0x00, 0xff);
    TUNNEL(&r, 0x10, 0x2d, 1);
    EXPECT(&r, 0x83, 0x20, 0xff, 0x22, 0x14, 0x2d, 0xff);
    TUNNEL(&r, 0x18, 0x03);
    EXPECT(&r, 0x83, 0x20, 0xff, 0x22, 0x1c, 0x03, 0xff);
    TUNNEL(&r, 0x28, 0x77);
    EXPECT(&r, 0x83, 0x20, 0xff, 0x22, 0x2c, 0x77, 0xff);

    /* Any other network function is silent. */
    TUNNEL(&r, 0x30, 0x77);
    TUNNEL(&r, 0x08, 0x00);
    REQUEST(&r, 0x07, 0x0a);
    EXPECT_ERROR(&r, 0x87, 0x0a, 0xff);

    /* The status is the only difference: back at 0 the pairs answer. */
    pbay_set_u8(&r, "frame-status", 0);
    TUNNEL(&r, 0x18, 0x04);
    EXPECT_TUNNEL(&r, 0x18, 0x04, 0x56, 0x00);
    TUNNEL(&r, 0x30, 0x77);
    EXPECT_ERROR(&r, 0x83, 0x20, 0xc1);
    rig_stop(&r);
}

static void test_sensor_groups(void)
{
    Rig r;

    /* With no input set every input reads clear. */
    rig_start(&r, "");
    for (uint8_t sensor = 1; sensor <= 14; sensor++) {
        TUNNEL(&r, 0x10, 0x2d, sensor);
        EXPECT_TUNNEL(&r, 0x10, 0x2d, 0x00, 0x00, 0x00, 0x00);
    }
    /* Sensor 15 replies with the snapshot; 0 and 16 and above are errors. */
    TUNNEL(&r, 0x10, 0x2d, 15);
    EXPECT_TUNNEL(&r, 0x10, 0x2d, 0x00, 0x00);
    TUNNEL(&r, 0x10, 0x2d, 16);
    EXPECT_ERROR(&r, 0x83, 0x20, 0xc1);
    TUNNEL(&r, 0x10, 0x2d, 0);
    EXPECT_ERROR(&r, 0x83, 0x20, 0xc1);
    rig_stop(&r);

    /* The snapshot is masked per sensor: bit n-1 for sensors 1 to 6. */
    rig_start(&r, ",input-snapshot=0x04");
    TUNNEL(&r, 0x10, 0x2d, 3);
    EXPECT_TUNNEL(&r, 0x10, 0x2d, 0x00, 0x00, 0x1f, 0x00);
    TUNNEL(&r, 0x10, 0x2d, 1);
    EXPECT_TUNNEL(&r, 0x10, 0x2d, 0x00, 0x00, 0x10, 0x00);
    TUNNEL(&r, 0x10, 0x2d, 9);
    EXPECT_TUNNEL(&r, 0x10, 0x2d, 0x00, 0x00, 0x03, 0x00);
    TUNNEL(&r, 0x10, 0x2d, 7);
    EXPECT_TUNNEL(&r, 0x10, 0x2d, 0x00, 0x00, 0x00, 0x00);
    rig_stop(&r);
}

/* A record read: key at offset 12, count at offset 15. */
#define RECORD(r, key, count) \
    TUNNEL(r, 0x28, 0x23, 0x00, 0x00, key, 0x00, 0x00, count)

static void test_record_keys(void)
{
    static const uint8_t next[] = {
        0x02, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06,
        0x00, 0x07, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0a, 0x00, 0x0b, 0x00,
        0x0c, 0x00, 0xff, 0xff,
    };
    Rig r;

    rig_start(&r, "");
    /* Each key's next id and the record's own id; 13 and above are errors. */
    for (uint8_t key = 0; key <= 12; key++) {
        uint8_t got[512], id = key ? key : 1;

        if (key == 1) {
            id = 1;
        }
        RECORD(&r, key, 2);
        g_assert_cmpuint(reply(&r, got), ==, 7 + 5 + 4);
        g_assert_cmpmem(got + 12, 2, next + 2 * key, 2);
        g_assert_cmphex(got[14], ==, id);
        g_assert_cmphex(got[15], ==, 0);
    }
    for (uint8_t key = 13; key < 20; key++) {
        RECORD(&r, key, 5);
        EXPECT_ERROR(&r, 0x83, 0x20, 0xc1);
    }
    rig_stop(&r);
}

static void test_record_counts(void)
{
    uint8_t got[512];
    Rig r;

    rig_start(&r, "");
    /* 0xff is the table size; counts above it, or zero, give no body. */
    static const struct { uint8_t key, count; size_t body; } rows[] = {
        { 0, 0xff, 32 }, { 1, 0xff, 32 }, { 2, 0xff, 32 }, { 3, 0xff, 16 },
        { 4, 0xff, 16 }, { 9, 0xff, 16 }, { 10, 0xff, 16 },
        { 11, 0xff, 16 }, { 12, 0xff, 48 },
        { 0, 0x20, 32 }, { 0, 0x21, 0 }, { 0, 0, 0 }, { 3, 0x10, 16 },
        { 3, 0x11, 0 }, { 12, 0x30, 48 }, { 12, 0x31, 0 }, { 12, 1, 1 },
        { 5, 0x10, 16 }, { 5, 3, 3 }, { 5, 0, 1 }, { 5, 0x11, 1 },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(rows); i++) {
        RECORD(&r, rows[i].key, rows[i].count);
        g_assert_cmpuint(reply(&r, got), ==, 7 + 5 + 2 + rows[i].body);
    }
    rig_stop(&r);
}

static void test_sheet_vectors(void)
{
    Rig r;

    rig_start(&r, "");
    VECTOR(&r, (0x00, 0x44, 0x02, 0x01, 0x00, 0x05, 0x20, 0x22, 0x00, 0x00,
           0x66, 0xff),
           (0x02, 0x01, 0x00, 0x44, 0x00, 0x85, 0x20, 0x00, 0x22, 0x04, 0x00,
           0x00, 0x00, 0x22, 0x22, 0x00, 0x00, 0x97, 0xfe));
    VECTOR(&r, (0x00, 0x44, 0x02, 0x01, 0x00, 0x05, 0x20, 0x22, 0x18, 0x04,
           0x4a, 0xff),
           (0x02, 0x01, 0x00, 0x44, 0x00, 0x85, 0x20, 0x00, 0x22, 0x1c, 0x04,
           0x00, 0x56, 0x00, 0x6c, 0xfe));
    VECTOR(&r, (0x00, 0x44, 0x02, 0x01, 0x00, 0x05, 0x20, 0x22, 0x28, 0x20,
           0x1e, 0xff),
           (0x02, 0x01, 0x00, 0x44, 0x00, 0x85, 0x20, 0x00, 0x22, 0x2c, 0x20,
           0x00, 0x01, 0x0c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x7d, 0xfe));
    VECTOR(&r, (0x00, 0x44, 0x02, 0x01, 0x00, 0x05, 0x20, 0x22, 0x28, 0x23,
           0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x10, 0xff),
           (0x02, 0x01, 0x00, 0x44, 0x00, 0x85, 0x20, 0x00, 0x22, 0x2c, 0x23,
           0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x02, 0x1b, 0x6d, 0xfe));
    VECTOR(&r, (0x00, 0x44, 0x02, 0x01, 0x00, 0x05, 0x20, 0x22, 0x28, 0x23,
           0x00, 0x00, 0x0c, 0x00, 0x00, 0x05, 0x04, 0xff),
           (0x02, 0x01, 0x00, 0x44, 0x00, 0x85, 0x20, 0x00, 0x22, 0x2c, 0x23,
           0x00, 0xff, 0xff, 0x0c, 0x00, 0x01, 0x01, 0x2c, 0x56, 0xfc));
    /* The reply holds 0xa0, so it travels as 0xaa 0xb0. */
    VECTOR(&r, (0x00, 0x44, 0x02, 0x01, 0x00, 0x05, 0x20, 0x22, 0x28, 0x23,
           0x00, 0x00, 0x04, 0x00, 0x00, 0xff, 0x12, 0xfe),
           (0x02, 0x01, 0x00, 0x44, 0x00, 0x85, 0x20, 0x00, 0x22, 0x2c, 0x23,
           0x00, 0x05, 0x00, 0x04, 0x00, 0x01, 0x11, 0x0b, 0x22, 0xa0, 0x80,
           0x00, 0x00, 0x10, 0x00, 0x0a, 0x01, 0x00, 0x00, 0x00, 0xfd));
    rig_stop(&r);

    rig_start(&r, ",input-snapshot=0x01");
    VECTOR(&r, (0x00, 0x44, 0x02, 0x01, 0x00, 0x05, 0x20, 0x22, 0x10, 0x2d,
           0x01, 0x27, 0xff),
           (0x02, 0x01, 0x00, 0x44, 0x00, 0x85, 0x20, 0x00, 0x22, 0x14, 0x2d,
           0x00, 0x00, 0x00, 0x0f, 0x00, 0x90, 0xfe));
    rig_stop(&r);
}

static void test_unknown_command(void)
{
    g_autofree char *log = NULL;
    Rig r;

    rig_start(&r, "");
    /* A command not in the table is a header-only 0xc1 reply. */
    REQUEST(&r, 0x01, 0x55);
    EXPECT_ERROR(&r, 0x81, 0x55, 0xc1);
    /* The loader answers these, and no loader is modelled. */
    REQUEST(&r, 0x02, 0xc1, 3, 0, 0, 1, 2, 3);
    EXPECT_ERROR(&r, 0x82, 0xc1, 0xc1);
    REQUEST(&r, 0x03, 0xc2);
    EXPECT_ERROR(&r, 0x83, 0xc2, 0xc1);
    log = rig_log(&r);
    g_assert_nonnull(strstr(log, "sgi-pbay: unimplemented command 0x55"));
    g_assert_nonnull(strstr(log, "sgi-pbay: unimplemented command 0xc1"));
    g_assert_nonnull(strstr(log, "sgi-pbay: unimplemented command 0xc2"));
    rig_stop(&r);
}

static void test_fragmented(void)
{
    g_autoptr(GByteArray) w = g_byte_array_new();
    uint8_t body[64], got[64];
    size_t len;
    Rig r;

    rig_start(&r, "");
    frame_wire(w, body, build(body, 0x4400, 0xa5aa, 0x05, 0x02, NULL, 0),
               false);
    /* One byte at a time, splitting every escape pair. */
    for (guint i = 0; i < w->len; i++) {
        wire_write(&r, w->data + i, 1);
    }
    len = reply(&r, got);
    g_assert_cmpuint(len, ==, 10);
    g_assert_cmphex(got[0], ==, 0xaa);
    g_assert_cmphex(got[1], ==, 0xa5);
    rig_stop(&r);
}

static void test_back_to_back(void)
{
    g_autoptr(GByteArray) w = g_byte_array_new();
    uint8_t body[64];
    Rig r;

    rig_start(&r, "");
    frame_wire(w, body, build(body, 0x4400, 0x0102, 0x05, 0x02, NULL, 0),
               false);
    frame_wire(w, body, build(body, 0x4400, 0x0102, 0x06, 0x13, NULL, 0),
               false);
    wire_write(&r, w->data, w->len);
    EXPECT(&r, 0x85, 0x02, 0x00, 0x00, 0x44);
    EXPECT(&r, 0x86, 0x13, 0x00, 0x22);
    rig_stop(&r);
}

static void test_resync(void)
{
    uint8_t body[64], junk[300];
    Rig r;

    rig_start(&r, "");
    /* Truncated: a frame too short to hold a header and checksum. */
    wire_write(&r, (uint8_t[]){ 0x01, 0x02, 0x03, 0xa5 }, 4);
    sentinel(&r);
    /* A bad escape discards the frame up to its terminator. */
    wire_write(&r, (uint8_t[]){ 0x00, 0x44, 0xaa, 0x00, 0x02, 0xa5 }, 6);
    sentinel(&r);
    /* An escape cut off by the terminator. */
    wire_write(&r, (uint8_t[]){ 0x00, 0x44, 0xaa, 0xa5 }, 4);
    sentinel(&r);
    /* A body past the receive limit is discarded to its terminator. */
    memset(junk, 0x11, sizeof(junk));
    junk[sizeof(junk) - 1] = 0xa5;
    wire_write(&r, junk, sizeof(junk));
    sentinel(&r);
    /* A bare start byte abandons a partial frame and begins a new one. */
    wire_write(&r, (uint8_t[]){ 0x00, 0x44, 0x02, 0x01 }, 4);
    wire_write(&r, (uint8_t[]){ 0xa0 }, 1);
    sentinel(&r);
    /* A leading start byte before a good frame is accepted. */
    wire_write(&r, (uint8_t[]){ 0xa0 }, 1);
    sentinel(&r);
    /* Garbage before the terminator of one frame does not cost the next. */
    wire_write(&r, (uint8_t[]){ 0xde, 0xad, 0xa5 }, 3);
    send_body(&r, body, build(body, 0x4400, 0x0102, 0x05, 0x02, NULL, 0),
              false);
    EXPECT(&r, 0x85, 0x02, 0x00, 0x00, 0x44);
    rig_stop(&r);
}

static void test_flash_chunk(void)
{
    QDict *resp;
    Rig r;

    /* The advertised chunk is at most 0x8c; the default is 0x80. */
    rig_start(&r, ",flash-chunk=0x8c");
    REQUEST(&r, 0x01, 0xc0, 0);
    EXPECT(&r, 0x81, 0xc0, 0x00, 0x12, 0x00, 0x60, 0xa0, 0xd4, 0x39, 0x8c);
    resp = qtest_qmp(r.qts, "{'execute':'device_add','arguments':"
                     "{'driver':'sgi-pbay','id':'over','flash-chunk':141}}");
    g_assert_true(qdict_haskey(resp, "error"));
    qobject_unref(resp);
    resp = qtest_qmp(r.qts, "{'execute':'device_add','arguments':"
                     "{'driver':'sgi-pbay','id':'zero','flash-chunk':0}}");
    g_assert_true(qdict_haskey(resp, "error"));
    qobject_unref(resp);
    resp = qtest_qmp(r.qts, "{'execute':'device_add','arguments':"
                     "{'driver':'sgi-pbay','id':'bad','state-code':3}}");
    g_assert_true(qdict_haskey(resp, "error"));
    qobject_unref(resp);
    rig_stop(&r);
}

static void test_reset(void)
{
    g_autoptr(GByteArray) w = g_byte_array_new();
    uint8_t body[64];
    Rig r;

    rig_start(&r, "");
    /*
     * A whole frame but for its terminator, then reset: the terminator
     * alone must not complete it.
     */
    frame_wire(w, body, build(body, 0x4400, 0x0102, 0x05, 0x02, NULL, 0),
               false);
    wire_write(&r, w->data, w->len - 1);
    while (pbay_u32(&r, "rx-len") != 9) {
        g_usleep(1000);
    }
    qtest_qmp_assert_success(r.qts, "{'execute':'system_reset'}");
    g_assert_cmpuint(pbay_u32(&r, "rx-len"), ==, 0);
    wire_write(&r, w->data + w->len - 1, 1);
    sentinel(&r);
    rig_stop(&r);
}
static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("pbay-migration-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    g_autoptr(GByteArray) w = g_byte_array_new();
    uint8_t body[64], got[64];
    Rig from, to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    rig_start(&from, "");
    rig_start_with(&to, "", "-incoming defer");

    /* At the boundary: a frame cut inside an escape. */
    frame_wire(w, body, build(body, 0x4400, 0xa5aa, 0x05, 0x02, NULL, 0),
               false);
    wire_write(&from, w->data, 3);
    while (pbay_u32(&from, "rx-len") != 2) {
        g_usleep(1000);
    }

    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from.qts, uri);
    sgi_wait_migration(to.qts, "completed");
    g_assert_cmpuint(pbay_u32(&to, "rx-len"), ==, 2);

    /* The rest of the frame, as the escape expects, finishes it. */
    wire_write(&to, w->data + 3, w->len - 3);
    g_assert_cmpuint(reply(&to, got), ==, 10);
    g_assert_cmphex(got[0], ==, 0xaa);
    g_assert_cmphex(got[1], ==, 0xa5);

    rig_stop(&to);
    rig_stop(&from);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sgi-pbay/worked-frame", test_worked_frame);
    qtest_add_func("/sgi-pbay/address-reply", test_address_reply);
    qtest_add_func("/sgi-pbay/bad-checksum", test_bad_checksum);
    qtest_add_func("/sgi-pbay/destination", test_destination);
    qtest_add_func("/sgi-pbay/stuffing", test_stuffing);
    qtest_add_func("/sgi-pbay/simple-commands", test_simple_commands);
    qtest_add_func("/sgi-pbay/state-code", test_state_code);
    qtest_add_func("/sgi-pbay/tunnelled", test_tunnelled);
    qtest_add_func("/sgi-pbay/tunnelled-errors", test_tunnelled_errors);
    qtest_add_func("/sgi-pbay/frame-status", test_frame_status);
    qtest_add_func("/sgi-pbay/streamed-read", test_streamed_read);
    qtest_add_func("/sgi-pbay/record-counts", test_record_counts);
    qtest_add_func("/sgi-pbay/sheet-vectors", test_sheet_vectors);
    qtest_add_func("/sgi-pbay/sensor-groups", test_sensor_groups);
    qtest_add_func("/sgi-pbay/record-keys", test_record_keys);
    qtest_add_func("/sgi-pbay/unknown-command", test_unknown_command);
    qtest_add_func("/sgi-pbay/fragmented", test_fragmented);
    qtest_add_func("/sgi-pbay/back-to-back", test_back_to_back);
    qtest_add_func("/sgi-pbay/resync", test_resync);
    qtest_add_func("/sgi-pbay/flash-chunk", test_flash_chunk);
    qtest_add_func("/sgi-pbay/reset", test_reset);
    qtest_add_func("/sgi-pbay/migration", test_migration);

    return g_test_run();
}
