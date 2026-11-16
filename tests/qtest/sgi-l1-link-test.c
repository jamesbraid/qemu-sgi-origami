/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * sgi-l1-link against a test-owned peer that listens on a Unix socket. Its
 * bytes mean nothing: no claim is made about what a real controller sends.
 */
#include "qemu/osdep.h"
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
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
    g_string_append(cmd, " -device sgi-l1-link,id=l1l");
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
static unsigned link_reasons(QTestState *qts)
{
    QDict *resp = qtest_qmp(qts, "{'execute':'query-migrate'}");
    QList *reasons = qdict_get_qlist(qdict_get_qdict(resp, "return"),
                                     "blocked-reasons");
    unsigned found = 0;

    for (const QListEntry *e = reasons ? qlist_first(reasons) : NULL; e;
         e = qlist_next(e)) {
        QString *text = qobject_to(QString, qlist_entry_obj(e));

        found += !!strstr(qstring_get_str(text), "sgi-l1-link");
    }
    qobject_unref(resp);
    return found;
}

static void test_blocker_follows_chardev(void)
{
    QTestState *qts;
    g_autofree char *dir = NULL;
    g_autofree char *path = NULL;
    GError *error = NULL;

    qts = qtest_init("-M none -cpu R10000 -S -device sgi-l1-link,id=l1l");
    g_assert_cmpuint(link_reasons(qts), ==, 0);
    qtest_quit(qts);

    dir = g_dir_make_tmp("sgi-l1-link-XXXXXX", &error);
    g_assert_no_error(error);
    path = g_build_filename(dir, "node.sock", NULL);
    qts = qtest_initf("-M none -cpu R10000 -S -chardev socket,id=n0,path=%s,"
                      "reconnect-ms=50 -device sgi-l1-link,id=l1l,node0=n0",
                      path);
    g_assert_cmpuint(link_reasons(qts), ==, 1);
    /* Unrealizing removes it, and realizing again adds it once. */
    qtest_qmp_assert_success(qts, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'realized','value':false}}", LINK);
    g_assert_cmpuint(link_reasons(qts), ==, 0);
    qtest_qmp_assert_success(qts, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'realized','value':true}}", LINK);
    g_assert_cmpuint(link_reasons(qts), ==, 1);
    qtest_quit(qts);

    /* Any one node chardev is enough. */
    qts = qtest_initf("-M none -cpu R10000 -S -chardev socket,id=n1,path=%s,"
                      "reconnect-ms=50 -device sgi-l1-link,id=l1l,node1=n1",
                      path);
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

/* QEMU must refuse these at start-up, with the reason on stderr. */
static void refused(const char *args, const char *reason)
{
    g_autofree char *cmd = g_strdup_printf(
        "timeout 20 %s -M none -display none -S -nodefaults "
        "-accel qtest -qtest /dev/null %s",
        qtest_qemu_binary(NULL), args);
    g_autofree char *err = NULL;
    g_autoptr(GError) error = NULL;
    int status;

    g_assert_true(g_spawn_command_line_sync(cmd, NULL, &err, &status, &error));
    g_assert_cmpint(WEXITSTATUS(status), ==, 1);
    g_assert_nonnull(strstr(err, reason));
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
    return g_test_run();
}
