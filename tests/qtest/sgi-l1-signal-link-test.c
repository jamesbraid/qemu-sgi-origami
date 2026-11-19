/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * sgi-l1-signal-link against a test-owned peer that plays the board from a
 * Unix socket it listens on. Its levels mean nothing: no claim is made about
 * what a real board drives.
 */
#include "qemu/osdep.h"
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"

#define DEV "/machine/peripheral/sig"
#define TIMEOUT_MS 10000
#define EPOCH 0x1234
#define MSG_LEN 9

enum { HELLO = 1, CHANGE, STATE };
enum { NODE_POWER = 1, RESET_HOLD, SOFT_RESET, NMI0, POWER_GOOD = 8,
       NODE_RESET_STATE };
enum { ROLE_MACHINE = 1, ROLE_BOARD };

/* The end under test; the board tests change it and put it back. */
static const char *device_type = "sgi-l1-signal-link";

typedef struct Rig {
    QTestState *qts;
    char *dir;
    char *trace;
    int listener;
    int fd;
} Rig;

static int listen_on(const char *path)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpuint(strlen(path), <, sizeof(address.sun_path));
    strcpy(address.sun_path, path);
    g_assert_cmpint(bind(fd, (struct sockaddr *)&address, sizeof(address)),
                    ==, 0);
    g_assert_cmpint(listen(fd, 1), ==, 0);
    return fd;
}

static char *sock_path(Rig *r)
{
    return g_build_filename(r->dir, "signals.sock", NULL);
}

/* Start the machine end with its chardev reconnecting to a held listener. */
static void rig_start(Rig *r, const char *extra)
{
    g_autofree char *path = NULL;
    GError *error = NULL;

    r->dir = g_dir_make_tmp("sgi-l1-signal-XXXXXX", &error);
    g_assert_no_error(error);
    r->trace = g_build_filename(r->dir, "trace", NULL);
    r->fd = -1;
    path = sock_path(r);
    r->listener = listen_on(path);
    r->qts = qtest_initf("-M none -cpu R10000 -S -chardev socket,id=sig,"
                         "path=%s,reconnect-ms=50 -trace "
                         "enable=sgi_l1_signal_* -D %s "
                         "-device %s,id=sig,signals=sig,"
                         "epoch=%d %s", path, r->trace, device_type, EPOCH,
                         extra);
}

/* Returns the trace log. */
static char *rig_stop(Rig *r)
{
    g_autofree char *path = sock_path(r);
    char *text = NULL;

    if (r->fd >= 0) {
        close(r->fd);
    }
    if (r->listener >= 0) {
        close(r->listener);
    }
    unlink(path);
    qtest_quit(r->qts);
    g_file_get_contents(r->trace, &text, NULL, NULL);
    unlink(r->trace);
    rmdir(r->dir);
    g_free(r->trace);
    g_free(r->dir);
    return text;
}

static uint64_t prop(Rig *r, const char *name)
{
    QDict *resp = qtest_qmp(r->qts, "{'execute':'qom-get','arguments':"
                            "{'path':%s,'property':%s}}", DEV, name);
    uint64_t v = qdict_get_int(resp, "return");

    qobject_unref(resp);
    return v;
}

static void wait_prop(Rig *r, const char *name, uint64_t want)
{
    int64_t deadline = g_get_monotonic_time() + TIMEOUT_MS * 1000;

    while (prop(r, name) != want) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void peer_accept(Rig *r)
{
    struct pollfd p = { .fd = r->listener, .events = POLLIN };

    g_assert_cmpint(poll(&p, 1, TIMEOUT_MS), ==, 1);
    r->fd = accept(r->listener, NULL, NULL);
    g_assert_cmpint(r->fd, >=, 0);
    wait_prop(r, "up", 1);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

/* CRC-8, polynomial 0x07, initial value 0. */
static uint8_t crc8(const uint8_t *m, size_t len)
{
    uint8_t crc = 0;

    for (size_t i = 0; i < len; i++) {
        crc ^= m[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
        }
    }
    return crc;
}

static void encode(uint8_t *m, unsigned kind, uint32_t seq, unsigned line,
                   unsigned level)
{
    m[0] = kind;
    put32(m + 1, seq);
    m[5] = line;
    m[6] = level >> 8;
    m[7] = level;
    m[8] = crc8(m, MSG_LEN - 1);
}

static void peer_send(Rig *r, unsigned kind, uint32_t seq, unsigned line,
                      unsigned level)
{
    uint8_t m[MSG_LEN];

    encode(m, kind, seq, line, level);
    g_assert_cmpint(send(r->fd, m, MSG_LEN, 0), ==, MSG_LEN);
}

static void peer_send_raw(Rig *r, const uint8_t *buf, size_t len)
{
    g_assert_cmpint(send(r->fd, buf, len, 0), ==, len);
}

typedef struct Msg {
    unsigned kind, line, level;
    uint32_t seq;
} Msg;

static void peer_recv(Rig *r, Msg *msg)
{
    uint8_t m[MSG_LEN];

    for (size_t got = 0; got < MSG_LEN;) {
        struct pollfd p = { .fd = r->fd, .events = POLLIN };
        ssize_t n;

        g_assert_cmpint(poll(&p, 1, TIMEOUT_MS), ==, 1);
        n = read(r->fd, m + got, MSG_LEN - got);
        g_assert_cmpint(n, >, 0);
        got += n;
    }
    g_assert_cmphex(m[8], ==, crc8(m, MSG_LEN - 1));
    msg->kind = m[0];
    msg->seq = ldl_be_p(m + 1);
    msg->line = m[5];
    msg->level = lduw_be_p(m + 6);
}

static bool peer_readable(Rig *r)
{
    struct pollfd p = { .fd = r->fd, .events = POLLIN };

    return poll(&p, 1, 0) == 1;
}

/* Whether the machine closed the link: the peer reads end of file. */
static bool peer_sees_close(Rig *r)
{
    struct pollfd p = { .fd = r->fd, .events = POLLIN };
    char c;

    return poll(&p, 1, TIMEOUT_MS) == 1 && read(r->fd, &c, 1) == 0;
}

/* The machine's opening: its hello, then its state. */
static void peer_expect_open(Rig *r, uint32_t state_seq, unsigned state_level)
{
    Msg m;

    peer_recv(r, &m);
    g_assert_cmpuint(m.kind, ==, HELLO);
    g_assert_cmpuint(m.seq, ==, EPOCH);
    g_assert_cmpuint(m.line, ==, 1);
    g_assert_cmpuint(m.level, ==, ROLE_MACHINE);
    peer_recv(r, &m);
    g_assert_cmpuint(m.kind, ==, STATE);
    g_assert_cmpuint(m.seq, ==, state_seq);
    g_assert_cmpuint(m.line, ==, 9);
    g_assert_cmphex(m.level, ==, state_level);
}

/* Connect, read the machine's opening and answer as a board of @epoch. */
static void handshake(Rig *r, uint32_t epoch, uint32_t seq, unsigned levels)
{
    peer_accept(r);
    peer_expect_open(r, 0, 0);
    peer_send(r, HELLO, epoch, 1, ROLE_BOARD);
    peer_send(r, STATE, seq, 9, levels);
}

/* The peer leaves and the machine sees the link go down, then comes back. */
static void peer_drop(Rig *r)
{
    close(r->fd);
    r->fd = -1;
    wait_prop(r, "up", 0);
}

static bool has(const char *text, const char *needle)
{
    return text && strstr(text, needle);
}

static void test_hello(void)
{
    Rig r;

    rig_start(&r, "");
    peer_accept(&r);
    peer_expect_open(&r, 0, 0);
    rig_stop(&r);
}

/* A peer that is not a board of this version is refused. */
static void test_hello_refused(void)
{
    Rig r;
    g_autofree char *text = NULL;

    rig_start(&r, "");
    peer_accept(&r);
    peer_expect_open(&r, 0, 0);
    peer_send(&r, HELLO, 7, 1, ROLE_MACHINE);
    g_assert_true(peer_sees_close(&r));
    close(r.fd);
    peer_accept(&r);
    peer_expect_open(&r, 0, 0);
    peer_send(&r, HELLO, 7, 2, ROLE_BOARD);
    g_assert_true(peer_sees_close(&r));
    text = rig_stop(&r);
    g_assert_true(has(text, "why=hello"));
}

/* Each board line reaches the GPIO the behavioural L1 has under its name. */
static void check_wiring(const char *name, unsigned line, unsigned irq)
{
    Rig r;
    g_autofree char *text = NULL;

    rig_start(&r, "");
    qtest_irq_intercept_out_named(r.qts, DEV, name);
    handshake(&r, 5, 0, 0);
    peer_send(&r, CHANGE, 1, line, 1);
    wait_prop(&r, "lines", 1u << (line - 1));
    g_assert_true(qtest_get_irq(r.qts, irq));
    g_assert_cmpuint(qtest_get_irq_raise_count(r.qts, irq), ==, 1);
    peer_send(&r, CHANGE, 2, line, 0);
    wait_prop(&r, "lines", 0);
    g_assert_false(qtest_get_irq(r.qts, irq));
    text = rig_stop(&r);
}

static void test_wiring(void)
{
    check_wiring("node-power", NODE_POWER, 0);
    check_wiring("reset-hold", RESET_HOLD, 0);
    check_wiring("soft-reset", SOFT_RESET, 0);
    for (unsigned i = 0; i < 4; i++) {
        check_wiring("nmi", NMI0 + i, i);
    }
}

/* The machine's two lines go to the board, in sequence, once per change. */
static void test_machine_lines(void)
{
    Rig r;
    Msg m;

    rig_start(&r, "");
    handshake(&r, 5, 0, 0);
    qtest_set_irq_in(r.qts, DEV, "power-good", 0, 1);
    qtest_set_irq_in(r.qts, DEV, "power-good", 0, 1);
    qtest_set_irq_in(r.qts, DEV, "node-reset-state", 0, 1);
    qtest_set_irq_in(r.qts, DEV, "power-good", 0, 0);
    peer_recv(&r, &m);
    g_assert_true(m.kind == CHANGE && m.seq == 1 && m.line == POWER_GOOD &&
                  m.level == 1);
    peer_recv(&r, &m);
    g_assert_true(m.kind == CHANGE && m.seq == 2 &&
                  m.line == NODE_RESET_STATE && m.level == 1);
    peer_recv(&r, &m);
    g_assert_true(m.kind == CHANGE && m.seq == 3 && m.line == POWER_GOOD &&
                  m.level == 0);
    g_assert_false(peer_readable(&r));
    rig_stop(&r);
}

/* The counter runs on while the link is down, and the state reports it. */
static void test_machine_lines_across_reconnect(void)
{
    Rig r;
    Msg m;

    rig_start(&r, "");
    handshake(&r, 5, 0, 0);
    qtest_set_irq_in(r.qts, DEV, "power-good", 0, 1);
    peer_recv(&r, &m);
    peer_drop(&r);
    qtest_set_irq_in(r.qts, DEV, "node-reset-state", 0, 1);
    peer_accept(&r);
    /* Both changes are in the state: neither is replayed as a change. */
    peer_expect_open(&r, 2, 0x180);
    g_assert_false(peer_readable(&r));
    rig_stop(&r);
}

/* A duplicate and a stale change are ignored; the link stays up. */
static void test_duplicate_and_stale(void)
{
    Rig r;
    g_autofree char *text = NULL;

    rig_start(&r, "");
    handshake(&r, 5, 0, 0);
    peer_send(&r, CHANGE, 1, NODE_POWER, 1);
    peer_send(&r, CHANGE, 1, NODE_POWER, 0);
    peer_send(&r, CHANGE, 2, RESET_HOLD, 1);
    wait_prop(&r, "lines", 3);
    peer_send(&r, CHANGE, 1, RESET_HOLD, 0);
    peer_send(&r, CHANGE, 3, NODE_POWER, 0);
    wait_prop(&r, "lines", 2);
    g_assert_false(peer_readable(&r));
    text = rig_stop(&r);
    g_assert_true(has(text, "why=stale seq=1"));
}

/* A change that skips one is not applied: the link closes and resyncs. */
static void test_gap(void)
{
    Rig r;
    g_autofree char *text = NULL;

    rig_start(&r, "");
    handshake(&r, 5, 0, 0);
    peer_send(&r, CHANGE, 1, NODE_POWER, 1);
    peer_send(&r, CHANGE, 3, RESET_HOLD, 1);
    g_assert_true(peer_sees_close(&r));
    close(r.fd);
    r.fd = -1;
    wait_prop(&r, "up", 0);
    g_assert_cmpuint(prop(&r, "lines"), ==, 1);

    /* The board's truth after the gap restores both lines. */
    handshake(&r, 5, 3, 3);
    wait_prop(&r, "lines", 3);
    peer_send(&r, CHANGE, 4, SOFT_RESET, 1);
    wait_prop(&r, "lines", 7);
    text = rig_stop(&r);
    g_assert_true(has(text, "why=gap seq=3"));
}

/* A pulse that came and went while the link was down is lost, not replayed. */
static void test_pulse_lost_while_down(void)
{
    Rig r;
    g_autofree char *text = NULL;

    rig_start(&r, "");
    qtest_irq_intercept_out_named(r.qts, DEV, "soft-reset");
    handshake(&r, 5, 0, 0);
    peer_send(&r, CHANGE, 1, NODE_POWER, 1);
    wait_prop(&r, "lines", 1);
    peer_drop(&r);

    /* The board pulsed soft-reset twice over, changes 2 to 5, meanwhile. */
    peer_accept(&r);
    peer_expect_open(&r, 0, 0);
    peer_send(&r, HELLO, 5, 1, ROLE_BOARD);
    peer_send(&r, STATE, 5, 9, 1);
    peer_send(&r, CHANGE, 6, RESET_HOLD, 1);
    wait_prop(&r, "lines", 3);
    g_assert_cmpuint(qtest_get_irq_raise_count(r.qts, 0), ==, 0);
    text = rig_stop(&r);
    g_assert_true(has(text, "changed=0 lost=4 restart=0"));
}

/* A board that restarted has a new epoch and starts counting again. */
static void test_restart(void)
{
    Rig r;
    g_autofree char *text = NULL;

    rig_start(&r, "");
    handshake(&r, 5, 0, 0);
    peer_send(&r, CHANGE, 1, NODE_POWER, 1);
    peer_send(&r, CHANGE, 2, NODE_POWER, 0);
    peer_send(&r, CHANGE, 3, RESET_HOLD, 1);
    wait_prop(&r, "lines", 2);
    peer_drop(&r);

    handshake(&r, 6, 0, 2);
    peer_send(&r, CHANGE, 1, NODE_POWER, 1);
    wait_prop(&r, "lines", 3);
    text = rig_stop(&r);
    g_assert_true(has(text, "epoch=6 changed=0 lost=0 restart=1"));
}

/* Bytes that are not a message are skipped up to the next one that is. */
static void test_junk(void)
{
    uint8_t junk[4][MSG_LEN];
    size_t len[4] = { MSG_LEN, 3, MSG_LEN, MSG_LEN };

    /* A bad check byte, stray bytes, an unknown kind, an unowned line. */
    encode(junk[0], CHANGE, 1, NODE_POWER, 1);
    junk[0][8]++;
    memset(junk[1], 0x55, 3);
    encode(junk[2], 9, 1, NODE_POWER, 1);
    encode(junk[3], CHANGE, 1, 40, 1);
    for (unsigned i = 0; i < 4; i++) {
        Rig r;
        g_autofree char *text = NULL;

        rig_start(&r, "");
        handshake(&r, 5, 0, 0);
        peer_send_raw(&r, junk[i], len[i]);
        peer_send(&r, CHANGE, 1, RESET_HOLD, 1);
        wait_prop(&r, "lines", 2);
        g_assert_false(peer_readable(&r));
        text = rig_stop(&r);
        g_assert_true(has(text, "why=junk"));
    }
}

/* The counter wraps: a change after 0xffffffff is the next one, not stale. */
static void test_sequence_wrap(void)
{
    Rig r;

    rig_start(&r, "");
    handshake(&r, 5, 0xfffffffe, 0);
    peer_send(&r, CHANGE, 0xffffffff, NODE_POWER, 1);
    peer_send(&r, CHANGE, 0, RESET_HOLD, 1);
    wait_prop(&r, "lines", 3);
    g_assert_false(peer_readable(&r));
    rig_stop(&r);
}

/* The check is the standard CRC-8 (SMBUS) and not just the device's copy. */
static void test_crc8_vector(void)
{
    g_assert_cmphex(crc8((const uint8_t *)"123456789", 9), ==, 0xf4);
}

/* Stray bytes ahead of a message do not make a window that closes the link. */
static void test_stray_bytes(void)
{
    Rig r;

    rig_start(&r, "");
    handshake(&r, 5, 0, 0);
    peer_send_raw(&r, (const uint8_t *)"\x01\xab", 2);
    peer_send(&r, STATE, 0x48, 9, 1);
    wait_prop(&r, "lines", 1);
    g_assert_cmpuint(prop(&r, "up"), ==, 1);
    rig_stop(&r);
}

/* Half a message does not survive a close: the next link starts clean. */
static void test_partial_message_dropped(void)
{
    Rig r;
    uint8_t m[MSG_LEN];

    rig_start(&r, "");
    qtest_irq_intercept_out_named(r.qts, DEV, "soft-reset");
    handshake(&r, 5, 0, 0);
    encode(m, CHANGE, 1, SOFT_RESET, 1);
    peer_send_raw(&r, m, 5);
    qtest_qmp_assert_success(r.qts, "{'execute':'query-status'}");
    peer_drop(&r);
    peer_accept(&r);
    peer_expect_open(&r, 0, 0);
    peer_send_raw(&r, m + 5, MSG_LEN - 5);
    peer_send(&r, HELLO, 5, 1, ROLE_BOARD);
    peer_send(&r, STATE, 0, 9, 0);
    peer_send(&r, CHANGE, 1, RESET_HOLD, 1);
    wait_prop(&r, "lines", 2);
    g_assert_cmpuint(qtest_get_irq_raise_count(r.qts, 0), ==, 0);
    rig_stop(&r);
}

/* What the levels do not explain is lost, and never counts below zero. */
static void test_lost_count(void)
{
    Rig r;
    g_autofree char *text = NULL;

    rig_start(&r, "");
    handshake(&r, 5, 0, 0);
    peer_send(&r, CHANGE, 1, NODE_POWER, 1);
    wait_prop(&r, "lines", 1);
    peer_drop(&r);
    /* Three changes passed: one shows in the levels, two were a pulse. */
    handshake(&r, 5, 4, 3);
    wait_prop(&r, "lines", 3);
    peer_drop(&r);
    /* A state numbered below ours loses nothing, even if its levels differ. */
    handshake(&r, 5, 2, 3);
    peer_drop(&r);
    handshake(&r, 5, 1, 1);
    peer_send(&r, CHANGE, 2, SOFT_RESET, 1);
    wait_prop(&r, "lines", 5);
    text = rig_stop(&r);
    g_assert_true(has(text, "epoch=5 changed=1 lost=2 restart=0"));
    g_assert_true(has(text, "epoch=5 changed=0 lost=0 restart=0"));
    g_assert_true(has(text, "epoch=5 changed=1 lost=0 restart=0"));
}

/* A peer that never reads fills the socket: a write fails, the link closes. */
static void test_short_write_closes(void)
{
    Rig r;
    g_autofree char *text = NULL;

    rig_start(&r, "");
    handshake(&r, 5, 0, 0);
    /* No reconnect, so the close is seen. */
    close(r.listener);
    r.listener = -1;
    for (unsigned i = 0; i < 4000 && prop(&r, "up"); i++) {
        qtest_set_irq_in(r.qts, DEV, "power-good", 0, !(i & 1));
    }
    wait_prop(&r, "up", 0);
    text = rig_stop(&r);
    g_assert_true(has(text, "why=write"));
}

/* Blockers are process-wide and savevm shows one, so read query-migrate. */
static unsigned link_reasons(QTestState *qts)
{
    QDict *resp = qtest_qmp(qts, "{'execute':'query-migrate'}");
    QList *reasons = qdict_get_qlist(qdict_get_qdict(resp, "return"),
                                     "blocked-reasons");
    unsigned found = 0;

    for (const QListEntry *e = reasons ? qlist_first(reasons) : NULL; e;
         e = qlist_next(e)) {
        QString *text = qobject_to(QString, qlist_entry_obj(e));

        found += !!strstr(qstring_get_str(text), "sgi-l1-signal-link");
    }
    qobject_unref(resp);
    return found;
}

/* A backend that cannot disconnect is refused, on either end. */
static void test_socket_only(void)
{
    static const char *const types[] = {
        "sgi-l1-signal-link", "sgi-l1-signal-link-board"
    };
    QTestState *qts = qtest_init("-M none -cpu R10000 -S "
                                 "-chardev null,id=n");

    for (unsigned i = 0; i < ARRAY_SIZE(types); i++) {
        QDict *resp = qtest_qmp(qts, "{'execute':'device_add','arguments':"
                                "{'driver':%s,'id':'x','signals':'n'}}",
                                types[i]);

        g_assert_true(qdict_haskey(resp, "error"));
        qobject_unref(resp);
    }
    qtest_quit(qts);
}

static void test_blocker_follows_chardev(void)
{
    QTestState *qts;
    Rig r;

    qts = qtest_init("-M none -cpu R10000 -S "
                     "-device sgi-l1-signal-link,id=sig");
    g_assert_cmpuint(link_reasons(qts), ==, 0);
    qtest_quit(qts);

    rig_start(&r, "");
    g_assert_cmpuint(link_reasons(r.qts), ==, 1);
    /* Unrealizing removes it, and realizing again adds it once. */
    qtest_qmp_assert_success(r.qts, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'realized','value':false}}", DEV);
    g_assert_cmpuint(link_reasons(r.qts), ==, 0);
    qtest_qmp_assert_success(r.qts, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'realized','value':true}}", DEV);
    g_assert_cmpuint(link_reasons(r.qts), ==, 1);
    rig_stop(&r);
}

/* The board end: the peer plays the machine; the board drives lines 1 to 7. */
static void board_start(Rig *r, const char *extra)
{
    device_type = "sgi-l1-signal-link-board";
    rig_start(r, extra);
    device_type = "sgi-l1-signal-link";
}

/* The board's opening: its hello, then its state. */
static void board_expect_open(Rig *r, uint32_t state_seq, unsigned state_level)
{
    Msg m;

    peer_recv(r, &m);
    g_assert_cmpuint(m.kind, ==, HELLO);
    g_assert_cmpuint(m.seq, ==, EPOCH);
    g_assert_cmpuint(m.line, ==, 1);
    g_assert_cmpuint(m.level, ==, ROLE_BOARD);
    peer_recv(r, &m);
    g_assert_cmpuint(m.kind, ==, STATE);
    g_assert_cmpuint(m.seq, ==, state_seq);
    g_assert_cmpuint(m.line, ==, 9);
    g_assert_cmphex(m.level, ==, state_level);
}

/* Connect, read the board's opening and answer as a machine of @epoch. */
static void board_handshake(Rig *r, uint32_t epoch, uint32_t seq,
                            unsigned levels)
{
    peer_accept(r);
    board_expect_open(r, 0, 0);
    peer_send(r, HELLO, epoch, 1, ROLE_MACHINE);
    peer_send(r, STATE, seq, 9, levels);
}

static void test_board_hello(void)
{
    Rig r;

    board_start(&r, "");
    peer_accept(&r);
    board_expect_open(&r, 0, 0);
    rig_stop(&r);
}

/* Only a machine of this version is accepted. */
static void test_board_hello_refused(void)
{
    Rig r;
    g_autofree char *text = NULL;

    board_start(&r, "");
    peer_accept(&r);
    board_expect_open(&r, 0, 0);
    peer_send(&r, HELLO, 7, 1, ROLE_BOARD);
    g_assert_true(peer_sees_close(&r));
    close(r.fd);
    peer_accept(&r);
    board_expect_open(&r, 0, 0);
    peer_send(&r, HELLO, 7, 2, ROLE_MACHINE);
    g_assert_true(peer_sees_close(&r));
    text = rig_stop(&r);
    g_assert_true(has(text, "why=hello"));
}

/* Each board input is a change on its own line, once per change. */
static void test_board_lines(void)
{
    static const struct {
        const char *name;
        unsigned n;
        unsigned line;
    } in[] = {
        { "node-power", 0, NODE_POWER }, { "reset-hold", 0, RESET_HOLD },
        { "soft-reset", 0, SOFT_RESET }, { "nmi", 0, NMI0 },
        { "nmi", 3, NMI0 + 3 },
    };
    Rig r;
    Msg m;

    board_start(&r, "");
    board_handshake(&r, 5, 0, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(in); i++) {
        qtest_set_irq_in(r.qts, DEV, in[i].name, in[i].n, 1);
        /* A level set to what it already is is not a change. */
        qtest_set_irq_in(r.qts, DEV, in[i].name, in[i].n, 1);
        peer_recv(&r, &m);
        g_assert_true(m.kind == CHANGE && m.seq == i + 1 &&
                      m.line == in[i].line && m.level == 1);
    }
    qtest_set_irq_in(r.qts, DEV, "node-power", 0, 0);
    peer_recv(&r, &m);
    g_assert_true(m.kind == CHANGE && m.seq == 6 && m.line == NODE_POWER &&
                  m.level == 0);
    g_assert_false(peer_readable(&r));
    wait_prop(&r, "lines", 0x4e);
    rig_stop(&r);
}

/* The machine's two lines come out as GPIOs under their own names. */
static void check_board_wiring(const char *name, unsigned line)
{
    Rig r;
    g_autofree char *text = NULL;

    board_start(&r, "");
    qtest_irq_intercept_out_named(r.qts, DEV, name);
    board_handshake(&r, 5, 0, 0);
    peer_send(&r, CHANGE, 1, line, 1);
    wait_prop(&r, "lines", 1u << (line - 1));
    g_assert_true(qtest_get_irq(r.qts, 0));
    g_assert_cmpuint(qtest_get_irq_raise_count(r.qts, 0), ==, 1);
    peer_send(&r, CHANGE, 2, line, 0);
    wait_prop(&r, "lines", 0);
    g_assert_false(qtest_get_irq(r.qts, 0));
    /* A line the board drives is not the machine's to change. */
    peer_send(&r, CHANGE, 3, NODE_POWER, 1);
    peer_send(&r, CHANGE, 3, line, 1);
    wait_prop(&r, "lines", 1u << (line - 1));
    text = rig_stop(&r);
    g_assert_true(has(text, "why=junk"));
}

static void test_board_wiring(void)
{
    check_board_wiring("power-good", POWER_GOOD);
    check_board_wiring("node-reset-state", NODE_RESET_STATE);
}

/* A reconnect restores the levels; a pulse made meanwhile is not replayed. */
static void test_board_resync(void)
{
    Rig r;
    Msg m;

    board_start(&r, "");
    board_handshake(&r, 5, 0, 0);
    qtest_set_irq_in(r.qts, DEV, "node-power", 0, 1);
    peer_recv(&r, &m);
    peer_drop(&r);

    qtest_set_irq_in(r.qts, DEV, "soft-reset", 0, 1);
    qtest_set_irq_in(r.qts, DEV, "soft-reset", 0, 0);
    qtest_set_irq_in(r.qts, DEV, "reset-hold", 0, 1);
    peer_accept(&r);
    /* Three changes after the one sent; the state shows two lines high. */
    board_expect_open(&r, 4, 0x3);
    g_assert_false(peer_readable(&r));
    rig_stop(&r);
}

/* A restarted machine has a new epoch; its state is applied as changes. */
static void test_board_machine_restart(void)
{
    Rig r;
    g_autofree char *text = NULL;

    board_start(&r, "");
    qtest_irq_intercept_out_named(r.qts, DEV, "node-reset-state");
    board_handshake(&r, 5, 0, 0);
    peer_send(&r, CHANGE, 1, NODE_RESET_STATE, 1);
    wait_prop(&r, "lines", 1u << (NODE_RESET_STATE - 1));
    peer_drop(&r);

    board_handshake(&r, 6, 0, 0);
    wait_prop(&r, "lines", 0);
    g_assert_false(qtest_get_irq(r.qts, 0));
    text = rig_stop(&r);
    g_assert_true(has(text, "epoch=6 changed=1 lost=0 restart=1"));
}

/* A gap closes the link, as it does on the machine end. */
static void test_board_gap(void)
{
    Rig r;
    g_autofree char *text = NULL;

    board_start(&r, "");
    board_handshake(&r, 5, 0, 0);
    peer_send(&r, CHANGE, 2, POWER_GOOD, 1);
    g_assert_true(peer_sees_close(&r));
    text = rig_stop(&r);
    g_assert_true(has(text, "why=gap"));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sgi-l1-signal-link/hello", test_hello);
    qtest_add_func("sgi-l1-signal-link/hello-refused", test_hello_refused);
    qtest_add_func("sgi-l1-signal-link/wiring", test_wiring);
    qtest_add_func("sgi-l1-signal-link/machine-lines", test_machine_lines);
    qtest_add_func("sgi-l1-signal-link/machine-lines-across-reconnect",
                   test_machine_lines_across_reconnect);
    qtest_add_func("sgi-l1-signal-link/duplicate-and-stale",
                   test_duplicate_and_stale);
    qtest_add_func("sgi-l1-signal-link/gap", test_gap);
    qtest_add_func("sgi-l1-signal-link/pulse-lost-while-down",
                   test_pulse_lost_while_down);
    qtest_add_func("sgi-l1-signal-link/restart", test_restart);
    qtest_add_func("sgi-l1-signal-link/junk", test_junk);
    qtest_add_func("sgi-l1-signal-link/sequence-wrap", test_sequence_wrap);
    qtest_add_func("sgi-l1-signal-link/crc8-vector", test_crc8_vector);
    qtest_add_func("sgi-l1-signal-link/stray-bytes", test_stray_bytes);
    qtest_add_func("sgi-l1-signal-link/partial-message-dropped",
                   test_partial_message_dropped);
    qtest_add_func("sgi-l1-signal-link/lost-count", test_lost_count);
    qtest_add_func("sgi-l1-signal-link/short-write-closes",
                   test_short_write_closes);
    qtest_add_func("sgi-l1-signal-link/blocker-follows-chardev",
                   test_blocker_follows_chardev);
    qtest_add_func("sgi-l1-signal-link/socket-only", test_socket_only);
    qtest_add_func("sgi-l1-signal-link/board-hello", test_board_hello);
    qtest_add_func("sgi-l1-signal-link/board-hello-refused",
                   test_board_hello_refused);
    qtest_add_func("sgi-l1-signal-link/board-lines", test_board_lines);
    qtest_add_func("sgi-l1-signal-link/board-wiring", test_board_wiring);
    qtest_add_func("sgi-l1-signal-link/board-resync", test_board_resync);
    qtest_add_func("sgi-l1-signal-link/board-machine-restart",
                   test_board_machine_restart);
    qtest_add_func("sgi-l1-signal-link/board-gap", test_board_gap);
    return g_test_run();
}
