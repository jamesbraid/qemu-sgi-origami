/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * sgi-l1-link against a test-owned peer that listens on a Unix socket. Its
 * bytes mean nothing: no claim is made about what a real controller sends.
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

#define BASE UINT64_C(0x11000000)
#define STRIDE UINT64_C(0x1000)
#define LINK "/machine/peripheral/l1l"
#define TIMEOUT_MS 10000
#define WINDOW_QUEUE 4096

#define REG_DATA 0x00
#define REG_IER  0x08
#define REG_IIR  0x10
#define REG_LSR  0x28

typedef struct Rig {
    QTestState *qts;
    char *dir;
    char *trace;
    int listener[2];
    int fd[2];
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

static char *node_path(Rig *r, unsigned n)
{
    return g_strdup_printf("%s/node%u.sock", r->dir, n);
}

/*
 * @windows windows, each chardev connecting to a held listener if @listen.
 * A @traced rig logs the link's transmissions to r->trace, read after it stops.
 */
static void rig_open(Rig *r, unsigned windows, bool listen, bool traced)
{
    GString *cmd = g_string_new("-M none -cpu R10000 -S");
    GError *error = NULL;

    r->dir = g_dir_make_tmp("sgi-l1-link-XXXXXX", &error);
    g_assert_no_error(error);
    r->trace = g_build_filename(r->dir, "trace", NULL);
    if (traced) {
        g_string_append_printf(cmd, " -trace enable=sgi_l1_link_tx -D %s",
                               r->trace);
    }
    for (unsigned n = 0; n < 2; n++) {
        r->listener[n] = r->fd[n] = -1;
    }
    for (unsigned n = 0; n < windows; n++) {
        g_autofree char *path = node_path(r, n);

        if (listen) {
            r->listener[n] = listen_on(path);
        }
        g_string_append_printf(cmd, " -chardev socket,id=n%u,path=%s,"
                               "reconnect-ms=50", n, path);
    }
    g_string_append_printf(cmd, " -chardev socket,id=sg,path=%s/signals.sock,"
                           "reconnect-ms=50 -device sgi-l1-link,id=l1l,"
                           "signals=sg", r->dir);
    for (unsigned n = 0; n < windows; n++) {
        g_string_append_printf(cmd, ",node%u=n%u", n, n);
    }
    for (unsigned n = 0; n < windows; n++) {
        g_string_append_printf(cmd, " -device sgi-l1-uart,id=u%u,"
                               "controller=l1l,transport-index=%u", n, n);
    }
    r->qts = qtest_init(cmd->str);
    g_string_free(cmd, TRUE);
    for (unsigned n = 0; n < windows; n++) {
        g_autofree char *path = g_strdup_printf("/machine/peripheral/u%u", n);

        qtest_sysbus_mmio_map(r->qts, path, 0, BASE + n * STRIDE);
    }
}

static void rig_start(Rig *r, unsigned windows, bool listen)
{
    rig_open(r, windows, listen, false);
}

/* Returns the trace of a traced rig, else NULL. */
static char *rig_stop(Rig *r)
{
    char *text = NULL;

    for (unsigned n = 0; n < 2; n++) {
        g_autofree char *path = node_path(r, n);

        if (r->fd[n] >= 0) {
            close(r->fd[n]);
        }
        if (r->listener[n] >= 0) {
            close(r->listener[n]);
        }
        unlink(path);
    }
    qtest_quit(r->qts);
    g_file_get_contents(r->trace, &text, NULL, NULL);
    unlink(r->trace);
    rmdir(r->dir);
    g_free(r->trace);
    g_free(r->dir);
    return text;
}

static uint64_t nodes_up(Rig *r)
{
    QDict *resp = qtest_qmp(r->qts, "{'execute':'qom-get','arguments':"
                            "{'path':%s,'property':'nodes-up'}}", LINK);
    uint64_t v = qdict_get_int(resp, "return");

    qobject_unref(resp);
    return v;
}

/* Wait for the link's state, which the chardev layer changes by itself. */
static void wait_nodes_up(Rig *r, uint64_t mask)
{
    int64_t deadline = g_get_monotonic_time() + TIMEOUT_MS * 1000;

    while (nodes_up(r) != mask) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void peer_accept(Rig *r, unsigned n)
{
    struct pollfd p = { .fd = r->listener[n], .events = POLLIN };

    g_assert_cmpint(poll(&p, 1, TIMEOUT_MS), ==, 1);
    r->fd[n] = accept(r->listener[n], NULL, NULL);
    g_assert_cmpint(r->fd[n], >=, 0);
}

/* Hang up and stop listening, so the chardev cannot reconnect at once. */
static void peer_leave(Rig *r, unsigned n)
{
    g_autofree char *path = node_path(r, n);

    close(r->fd[n]);
    r->fd[n] = -1;
    close(r->listener[n]);
    r->listener[n] = -1;
    unlink(path);
}

static void peer_send(Rig *r, unsigned n, const uint8_t *buf, size_t len)
{
    g_assert_cmpint(send(r->fd[n], buf, len, 0), ==, len);
}

static void peer_recv(Rig *r, unsigned n, uint8_t *buf, size_t len)
{
    while (len) {
        struct pollfd p = { .fd = r->fd[n], .events = POLLIN };
        ssize_t got;

        g_assert_cmpint(poll(&p, 1, TIMEOUT_MS), ==, 1);
        got = read(r->fd[n], buf, len);
        g_assert_cmpint(got, >, 0);
        buf += got;
        len -= got;
    }
}

/* Whether anything is waiting on the peer's end. */
static bool peer_readable(Rig *r, unsigned n)
{
    struct pollfd p = { .fd = r->fd[n], .events = POLLIN };

    return poll(&p, 1, 0) == 1;
}

static uint64_t reg_read(Rig *r, unsigned w, unsigned reg)
{
    return qtest_readq(r->qts, BASE + w * STRIDE + reg);
}

static void reg_write(Rig *r, unsigned w, unsigned reg, uint64_t value)
{
    qtest_writeq(r->qts, BASE + w * STRIDE + reg, value);
}

/* Read @len bytes the way the node does, waiting for each to arrive. */
static void window_recv(Rig *r, unsigned w, uint8_t *buf, size_t len)
{
    int64_t deadline = g_get_monotonic_time() + 2 * TIMEOUT_MS * 1000;

    for (size_t i = 0; i < len;) {
        if (reg_read(r, w, REG_LSR) & 1) {
            buf[i++] = reg_read(r, w, REG_DATA);
        } else {
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_usleep(200);
        }
    }
}

static void pattern(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = i * 7 + 3;
    }
}

static void connect_peers(Rig *r, unsigned windows)
{
    uint64_t mask = 0;

    for (unsigned n = 0; n < windows; n++) {
        peer_accept(r, n);
        mask |= 1 << n;
    }
    wait_nodes_up(r, mask);
}

static void test_node_to_peer(void)
{
    Rig r;
    uint8_t sent[256], got[257];

    rig_start(&r, 1, true);
    connect_peers(&r, 1);
    pattern(sent, sizeof(sent));
    for (unsigned i = 0; i < sizeof(sent); i++) {
        reg_write(&r, 0, REG_DATA, sent[i]);
    }
    /* The data is the low byte of an 8-byte big-endian access. */
    reg_write(&r, 0, REG_DATA, UINT64_C(0x1122334455667788));
    peer_recv(&r, 0, got, sizeof(got));
    g_assert_cmpmem(got, 256, sent, 256);
    g_assert_cmphex(got[256], ==, 0x88);
    g_assert_false(peer_readable(&r, 0));
    rig_stop(&r);
}

static void test_peer_to_node(void)
{
    Rig r;
    uint8_t sent[256], got[256];

    rig_start(&r, 1, true);
    connect_peers(&r, 1);
    pattern(sent, sizeof(sent));
    g_assert_cmphex(reg_read(&r, 0, REG_IIR), ==, 1);
    reg_write(&r, 0, REG_IER, 1);
    peer_send(&r, 0, sent, sizeof(sent));
    /* Received data raises the interrupt, cause 4, until it is read. */
    while (reg_read(&r, 0, REG_IIR) != 4) {
        g_usleep(200);
    }
    window_recv(&r, 0, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), sent, sizeof(sent));
    g_assert_cmphex(reg_read(&r, 0, REG_IIR), ==, 1);
    g_assert_cmphex(reg_read(&r, 0, REG_LSR) & 1, ==, 0);
    rig_stop(&r);
}

/* More than the window holds, sent at once, arrives whole and in order. */
static void test_back_pressure(void)
{
    enum { TOTAL = 3 * WINDOW_QUEUE + 123 };
    Rig r;
    g_autofree uint8_t *sent = g_malloc(TOTAL);
    g_autofree uint8_t *got = g_malloc(TOTAL);

    rig_start(&r, 1, true);
    connect_peers(&r, 1);
    pattern(sent, TOTAL);
    peer_send(&r, 0, sent, TOTAL);
    /* Let the main loop offer the stream to the window several times over. */
    for (unsigned i = 0; i < 50; i++) {
        qtest_qmp_assert_success(r.qts, "{'execute':'query-status'}");
    }
    window_recv(&r, 0, got, TOTAL);
    g_assert_cmpmem(got, TOTAL, sent, TOTAL);
    rig_stop(&r);
}

/* Each transport has its own chardev, and bytes never cross. */
static void test_transports(void)
{
    Rig r;
    uint8_t got;

    rig_start(&r, 2, true);
    connect_peers(&r, 2);
    reg_write(&r, 1, REG_DATA, 0xa1);
    peer_recv(&r, 1, &got, 1);
    g_assert_cmphex(got, ==, 0xa1);
    g_assert_false(peer_readable(&r, 0));
    peer_send(&r, 0, (const uint8_t *)"\x5a", 1);
    window_recv(&r, 0, &got, 1);
    g_assert_cmphex(got, ==, 0x5a);
    g_assert_cmphex(reg_read(&r, 1, REG_LSR) & 1, ==, 0);
    rig_stop(&r);
}

/* With no peer, nothing arrives and the node's bytes are dropped. */
static void test_silent_without_peer(void)
{
    g_autofree char *path = NULL;
    Rig r;
    uint8_t got;

    rig_start(&r, 1, false);
    g_assert_cmpuint(nodes_up(&r), ==, 0);
    g_assert_cmphex(reg_read(&r, 0, REG_LSR), ==, 0x60);
    reg_write(&r, 0, REG_DATA, 0x11);
    reg_write(&r, 0, REG_DATA, 0x22);
    g_assert_cmphex(reg_read(&r, 0, REG_LSR), ==, 0x60);

    path = node_path(&r, 0);
    r.listener[0] = listen_on(path);
    connect_peers(&r, 1);
    reg_write(&r, 0, REG_DATA, 0x33);
    peer_recv(&r, 0, &got, 1);
    g_assert_cmphex(got, ==, 0x33);
    g_assert_false(peer_readable(&r, 0));
    rig_stop(&r);
}

/*
 * Bytes queued toward the node survive the peer leaving; those written
 * meanwhile are dropped, not resent when it returns.
 */
/*
 * A peer that stops reading fills the socket. The write that no longer fits
 * is dropped and traced instead of stalling the machine, and what did fit
 * arrives in order.
 */
static void test_stalled_peer(void)
{
    enum { TOTAL = 20000 };
    Rig r;
    g_autofree uint8_t *sent = g_malloc(TOTAL);
    g_autofree uint8_t *got = g_malloc(TOTAL);
    g_autofree char *text = NULL;
    size_t len = 0;

    rig_open(&r, 1, true, true);
    connect_peers(&r, 1);
    pattern(sent, TOTAL);
    for (unsigned i = 0; i < TOTAL; i++) {
        reg_write(&r, 0, REG_DATA, sent[i]);
    }
    while (peer_readable(&r, 0)) {
        ssize_t n = read(r.fd[0], got + len, TOTAL - len);

        g_assert_cmpint(n, >, 0);
        len += n;
    }
    g_assert_cmpuint(len, >, 0);
    g_assert_cmpuint(len, <, TOTAL);
    g_assert_cmpmem(got, len, sent, len);
    text = rig_stop(&r);
    g_assert_nonnull(strstr(text, "sent=0"));
}

static void test_reconnect(void)
{
    g_autofree char *path = NULL;
    Rig r;
    uint8_t got[4];

    rig_start(&r, 1, true);
    connect_peers(&r, 1);
    peer_send(&r, 0, (const uint8_t *)"\x01\x02\x03", 3);
    while (!(reg_read(&r, 0, REG_LSR) & 1)) {
        g_usleep(200);
    }
    peer_leave(&r, 0);
    wait_nodes_up(&r, 0);
    reg_write(&r, 0, REG_DATA, 0x44);
    window_recv(&r, 0, got, 3);
    g_assert_cmphex(got[0], ==, 1);
    g_assert_cmphex(got[1], ==, 2);
    g_assert_cmphex(got[2], ==, 3);

    path = node_path(&r, 0);
    r.listener[0] = listen_on(path);
    connect_peers(&r, 1);
    reg_write(&r, 0, REG_DATA, 0x55);
    peer_recv(&r, 0, got, 1);
    g_assert_cmphex(got[0], ==, 0x55);
    g_assert_false(peer_readable(&r, 0));
    peer_send(&r, 0, (const uint8_t *)"\x09", 1);
    window_recv(&r, 0, got, 1);
    g_assert_cmphex(got[0], ==, 9);
    rig_stop(&r);
}

/* Blockers are process-wide and savevm shows one, so read query-migrate. */
static unsigned reasons_with(QTestState *qts, const char *needle)
{
    QDict *resp = qtest_qmp(qts, "{'execute':'query-migrate'}");
    QList *reasons = qdict_get_qlist(qdict_get_qdict(resp, "return"),
                                     "blocked-reasons");
    unsigned found = 0;

    for (const QListEntry *e = reasons ? qlist_first(reasons) : NULL; e;
         e = qlist_next(e)) {
        QString *text = qobject_to(QString, qlist_entry_obj(e));

        found += !!strstr(qstring_get_str(text), needle);
    }
    qobject_unref(resp);
    return found;
}

static unsigned link_reasons(QTestState *qts)
{
    return reasons_with(qts, "sgi-l1-link:");
}

static unsigned signal_reasons(QTestState *qts)
{
    return reasons_with(qts, "sgi-l1-signal-link:");
}

static void test_blocker_follows_chardev(void)
{
    QTestState *qts;
    g_autofree char *dir = NULL;
    g_autofree char *path = NULL;
    GError *error = NULL;

    qts = qtest_init("-M none -cpu R10000 -S -device sgi-l1-link,id=l1l");
    g_assert_cmpuint(link_reasons(qts), ==, 0);
    g_assert_cmpuint(signal_reasons(qts), ==, 0);
    qtest_quit(qts);

    dir = g_dir_make_tmp("sgi-l1-link-XXXXXX", &error);
    g_assert_no_error(error);
    path = g_build_filename(dir, "node.sock", NULL);
    qts = qtest_initf("-M none -cpu R10000 -S -chardev socket,id=n0,path=%s,"
                      "reconnect-ms=50 -chardev socket,id=sg,path=%s,"
                      "reconnect-ms=50 -device sgi-l1-link,id=l1l,node0=n0,"
                      "signals=sg", path, path);
    g_assert_cmpuint(link_reasons(qts), ==, 1);
    g_assert_cmpuint(signal_reasons(qts), ==, 1);
    /* Unrealizing removes both, and realizing again adds each once. */
    qtest_qmp_assert_success(qts, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'realized','value':false}}", LINK);
    g_assert_cmpuint(link_reasons(qts), ==, 0);
    g_assert_cmpuint(signal_reasons(qts), ==, 0);
    qtest_qmp_assert_success(qts, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'realized','value':true}}", LINK);
    g_assert_cmpuint(link_reasons(qts), ==, 1);
    g_assert_cmpuint(signal_reasons(qts), ==, 1);
    qtest_quit(qts);

    /* Any one node chardev is enough. */
    qts = qtest_initf("-M none -cpu R10000 -S -chardev socket,id=n1,path=%s,"
                      "reconnect-ms=50 -chardev socket,id=sg,path=%s,"
                      "reconnect-ms=50 -device sgi-l1-link,id=l1l,node1=n1,"
                      "signals=sg", path, path);
    g_assert_cmpuint(link_reasons(qts), ==, 1);
    qtest_quit(qts);
    rmdir(dir);
}

static void test_nodes_up_clears_on_unrealize(void)
{
    Rig r;

    rig_start(&r, 1, true);
    connect_peers(&r, 1);
    qtest_qmp_assert_success(r.qts, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'realized','value':false}}", LINK);
    g_assert_cmpuint(nodes_up(&r), ==, 0);
    rig_stop(&r);
}

#define CHILD LINK "/signal-link"

static uint8_t crc8(const uint8_t *m)
{
    uint8_t crc = 0;

    for (int i = 0; i < 8; i++) {
        crc ^= m[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
        }
    }
    return crc;
}

/* A 9-byte signal message: kind, sequence, line, level, check. */
static void board_send(int fd, unsigned kind, uint32_t seq, unsigned line,
                       unsigned level)
{
    uint8_t m[9];

    m[0] = kind;
    stl_be_p(m + 1, seq);
    m[5] = line;
    stw_be_p(m + 6, level);
    m[8] = crc8(m);
    g_assert_cmpint(send(fd, m, 9, 0), ==, 9);
}

static void board_recv(int fd, uint8_t *m)
{
    for (size_t got = 0; got < 9;) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        ssize_t n;

        g_assert_cmpint(poll(&p, 1, TIMEOUT_MS), ==, 1);
        n = read(fd, m + got, 9 - got);
        g_assert_cmpint(n, >, 0);
        got += n;
    }
}

static uint64_t qom_get_int(QTestState *qts, const char *path,
                            const char *name)
{
    QDict *resp = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                            "{'path':%s,'property':%s}}", path, name);
    uint64_t v = qdict_get_int(resp, "return");

    qobject_unref(resp);
    return v;
}

static char *qom_get_str(QTestState *qts, const char *path, const char *name)
{
    QDict *resp = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                            "{'path':%s,'property':%s}}", path, name);
    char *v = g_strdup(qdict_get_str(resp, "return"));

    qobject_unref(resp);
    return v;
}

/* Play the board on the signals socket: connect, swap hellos and states. */
static int board_connect(Rig *r, int listener)
{
    struct pollfd p = { .fd = listener, .events = POLLIN };
    int64_t deadline = g_get_monotonic_time() + TIMEOUT_MS * 1000;
    uint8_t m[9];
    int fd;

    g_assert_cmpint(poll(&p, 1, TIMEOUT_MS), ==, 1);
    fd = accept(listener, NULL, NULL);
    g_assert_cmpint(fd, >=, 0);
    board_recv(fd, m);
    g_assert_cmpuint(m[0], ==, 1);
    board_recv(fd, m);
    g_assert_cmpuint(m[0], ==, 3);
    board_send(fd, 1, 5, 1, 2);
    board_send(fd, 3, 0, 9, 0);
    while (qom_get_int(r->qts, CHILD, "up") != 1) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    return fd;
}

/* The board's lines leave the link under sgi-l1's names, as its outputs. */
static void check_signal_out(const char *name, unsigned line, unsigned irq)
{
    g_autofree char *path = NULL;
    Rig r;
    int listener, fd;

    rig_start(&r, 1, false);
    path = g_build_filename(r.dir, "signals.sock", NULL);
    listener = listen_on(path);
    qtest_irq_intercept_out_named(r.qts, LINK, name);
    fd = board_connect(&r, listener);
    board_send(fd, 2, 1, line, 1);
    while (qom_get_int(r.qts, CHILD, "lines") != 1u << (line - 1)) {
        g_usleep(1000);
    }
    g_assert_true(qtest_get_irq(r.qts, irq));
    board_send(fd, 2, 2, line, 0);
    while (qom_get_int(r.qts, CHILD, "lines") != 0) {
        g_usleep(1000);
    }
    g_assert_false(qtest_get_irq(r.qts, irq));
    close(fd);
    close(listener);
    unlink(path);
    rig_stop(&r);
}

static void test_signal_outputs(void)
{
    check_signal_out("node-power", 1, 0);
    check_signal_out("reset-hold", 2, 0);
    check_signal_out("soft-reset", 3, 0);
    for (unsigned i = 0; i < 4; i++) {
        check_signal_out("nmi", 4 + i, i);
    }
}

/* The machine's two lines go in under the same names and reach the board. */
static void test_signal_inputs(void)
{
    g_autofree char *path = NULL;
    uint8_t m[9];
    Rig r;
    int listener, fd;

    rig_start(&r, 1, false);
    path = g_build_filename(r.dir, "signals.sock", NULL);
    listener = listen_on(path);
    fd = board_connect(&r, listener);
    qtest_set_irq_in(r.qts, LINK, "power-good", 0, 1);
    board_recv(fd, m);
    g_assert_true(m[0] == 2 && m[5] == 8 && m[7] == 1);
    qtest_set_irq_in(r.qts, LINK, "node-reset-state", 0, 1);
    board_recv(fd, m);
    g_assert_true(m[0] == 2 && m[5] == 9 && m[7] == 1);
    close(fd);
    close(listener);
    unlink(path);
    rig_stop(&r);
}

/* The signal link is a child, and its two properties are the link's own. */
static void test_signal_child(void)
{
    QTestState *qts;
    g_autofree char *signals = NULL;

    qts = qtest_init("-M none -cpu R10000 -S -chardev socket,id=n0,"
                     "path=/nonexistent/n0,reconnect-ms=50 -chardev socket,"
                     "id=sg,path=/nonexistent/sg,reconnect-ms=50 "
                     "-device sgi-l1-link,id=l1l,node0=n0,signals=sg,"
                     "epoch=77");
    signals = qom_get_str(qts, LINK, "signals");
    g_assert_cmpstr(signals, ==, "sg");
    g_assert_cmpuint(qom_get_int(qts, LINK, "epoch"), ==, 77);
    g_assert_cmpuint(qom_get_int(qts, CHILD, "epoch"), ==, 77);
    g_assert_cmpuint(qom_get_int(qts, CHILD, "up"), ==, 0);
    qtest_quit(qts);

    /* With no chardev the child is there, unset, and the link stays inert. */
    qts = qtest_init("-M none -cpu R10000 -S -device sgi-l1-link,id=l1l");
    g_free(signals);
    signals = qom_get_str(qts, LINK, "signals");
    g_assert_cmpstr(signals, ==, "");
    g_assert_cmpuint(qom_get_int(qts, LINK, "epoch"), ==, 1);
    qtest_quit(qts);
}

/* Run QEMU for up to @secs; returns its exit status. */
static int run_qemu(const char *accel, unsigned secs, const char *args,
                    char **err)
{
    g_autofree char *cmd = g_strdup_printf(
        "timeout %u %s -M none -display none -S -nodefaults "
        "-accel %s -qtest /dev/null %s",
        secs, qtest_qemu_binary(NULL), accel, args);
    g_autoptr(GError) error = NULL;
    int status;

    g_assert_true(g_spawn_command_line_sync(cmd, NULL, err, &status, &error));
    return WEXITSTATUS(status);
}

/* QEMU must refuse these at start-up, with the reason on stderr. */
static void refused_with(const char *accel, const char *args,
                         const char *reason)
{
    g_autofree char *err = NULL;

    g_assert_cmpint(run_qemu(accel, 20, args, &err), ==, 1);
    g_assert_nonnull(strstr(err, reason));
}

static void refused(const char *args, const char *reason)
{
    refused_with("qtest", args, reason);
}

static void test_window_refusals(void)
{
    refused("-device sgi-l1-link,id=l1l "
            "-device sgi-l1-uart,id=u0,controller=l1l "
            "-device sgi-l1-uart,id=u1,controller=l1l",
            "transport 0 already has a UART window");
    refused("-device sgi-l1-link,id=l1l "
            "-device sgi-l1-uart,id=u0,controller=l1l,transport-index=4",
            "transport index is invalid");
    refused("-device sgi-l1-link,id=l1l "
            "-device sgi-l1-uart,id=u0,controller=l1l "
            "-device sgi-l1-uart,id=u1,controller=u0",
            "expected: sgi-l1-provider");
}

#define NODE_CHR "-chardev socket,id=n0,path=/nonexistent/n,reconnect-ms=50 "
#define SIGNAL_CHR "-chardev socket,id=sg,path=/nonexistent/s,reconnect-ms=50 "

static void test_chardev_pairing(void)
{
    const char *both = "must be given together";

    refused(NODE_CHR "-device sgi-l1-link,id=l1l,node0=n0", both);
    refused(NODE_CHR "-device sgi-l1-link,id=l1l,node3=n0", both);
    refused(SIGNAL_CHR "-device sgi-l1-link,id=l1l,signals=sg", both);
}

/* -icount needs TCG, which the machine's other tests do not run. */
static void test_icount_refused(void)
{
    const char *why = "must not run ahead of a separate controller process";

    refused_with("tcg", "-icount shift=0 " NODE_CHR SIGNAL_CHR
                 "-device sgi-l1-link,id=l1l,node0=n0,signals=sg", why);
    refused_with("tcg", "-icount shift=auto " NODE_CHR SIGNAL_CHR
                 "-device sgi-l1-link,id=l1l,node0=n0,signals=sg", why);
}

/* Without a chardev the link is inert: QEMU starts under -icount and quits. */
static void test_icount_allowed_when_inert(void)
{
    g_autofree char *cmd = g_strdup_printf(
        "printf '{\"execute\":\"qmp_capabilities\"}"
        "{\"execute\":\"quit\"}' | timeout 60 %s -M none -display none -S "
        "-nodefaults -accel tcg -icount shift=0 -qmp stdio "
        "-device sgi-l1-link,id=l1l", qtest_qemu_binary(NULL));
    char *argv[] = { (char *)"sh", (char *)"-c", cmd, NULL };
    g_autofree char *out = NULL;
    g_autofree char *err = NULL;
    g_autoptr(GError) error = NULL;
    int status;

    g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
                               NULL, &out, &err, &status, &error));
    g_assert_cmpint(WEXITSTATUS(status), ==, 0);
    g_assert_nonnull(strstr(out, "\"QMP\""));
    g_assert_null(strstr(err, "icount"));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sgi-l1-link/node-to-peer", test_node_to_peer);
    qtest_add_func("sgi-l1-link/peer-to-node", test_peer_to_node);
    qtest_add_func("sgi-l1-link/back-pressure", test_back_pressure);
    qtest_add_func("sgi-l1-link/transports", test_transports);
    qtest_add_func("sgi-l1-link/silent-without-peer",
                   test_silent_without_peer);
    qtest_add_func("sgi-l1-link/stalled-peer", test_stalled_peer);
    qtest_add_func("sgi-l1-link/reconnect", test_reconnect);
    qtest_add_func("sgi-l1-link/nodes-up-clears-on-unrealize",
                   test_nodes_up_clears_on_unrealize);
    qtest_add_func("sgi-l1-link/blocker-follows-chardev",
                   test_blocker_follows_chardev);
    qtest_add_func("sgi-l1-link/window-refusals", test_window_refusals);
    qtest_add_func("sgi-l1-link/signal-child", test_signal_child);
    qtest_add_func("sgi-l1-link/signal-outputs", test_signal_outputs);
    qtest_add_func("sgi-l1-link/signal-inputs", test_signal_inputs);
    qtest_add_func("sgi-l1-link/chardev-pairing", test_chardev_pairing);
    qtest_add_func("sgi-l1-link/icount-refused", test_icount_refused);
    qtest_add_func("sgi-l1-link/icount-inert", test_icount_allowed_when_inert);
    return g_test_run();
}
