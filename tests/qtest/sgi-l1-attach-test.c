/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The Origin 3200 with C-brick 0's L1 outside the machine, against a
 * test-owned peer that plays the controller's board from Unix sockets it
 * listens on. The lines it drives mean nothing: no claim is made about what
 * a real board does.
 */
#include "qemu/osdep.h"
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include "qemu/bswap.h"
#include "qemu/bitops.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"
#include "sgi-test.h"

#define TIMEOUT_MS 10000
#define MSG_LEN 9
#define EPOCH 0x4321
#define NODE0 "/machine/node0"
#define L1 "/machine/node0/l1"
#define SIGNALS "/machine/node0/l1/signal-link"
/* Node 0's local scratch register: set before a reset, clear after one. */
#define SCRATCH0 (SPACE_IO + 0x01000000 + 0x608000)
#define MARK UINT64_C(0x5a5a)
/* The L1's UART window, in the Bedrock's local register block. */
#define UART (SPACE_HUB + UINT64_C(0x10000000) + 0x80)
#define UART_DATA (UART + 0x00)
#define UART_LSR (UART + 0x28)

enum { HELLO = 1, CHANGE, STATE };
enum { NODE_POWER = 1, RESET_HOLD, SOFT_RESET, NMI0 };
#define POWER (1U << (NODE_POWER - 1))
#define HOLD (1U << (RESET_HOLD - 1))

typedef struct Rig {
    QTestState *qts;
    char *dir, *prom;
    int sig_listener, node_listener;
    int sig_fd, node_fd;
    /* The board's own line levels and change counter. */
    unsigned lines;
    uint32_t seq;
    unsigned resets, shutdowns;
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

static int accept_on(int listener)
{
    struct pollfd p = { .fd = listener, .events = POLLIN };
    int fd;

    g_assert_cmpint(poll(&p, 1, TIMEOUT_MS), ==, 1);
    fd = accept(listener, NULL, NULL);
    g_assert_cmpint(fd, >=, 0);
    return fd;
}

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

static void send_msg(int fd, unsigned kind, uint32_t seq, unsigned line,
                     unsigned level)
{
    uint8_t m[MSG_LEN];

    m[0] = kind;
    stl_be_p(m + 1, seq);
    m[5] = line;
    stw_be_p(m + 6, level);
    m[8] = crc8(m, MSG_LEN - 1);
    g_assert_cmpint(send(fd, m, MSG_LEN, 0), ==, MSG_LEN);
}

static void recv_msg(int fd)
{
    uint8_t m[MSG_LEN];

    for (size_t got = 0; got < MSG_LEN;) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        ssize_t n;

        g_assert_cmpint(poll(&p, 1, TIMEOUT_MS), ==, 1);
        n = read(fd, m + got, MSG_LEN - got);
        g_assert_cmpint(n, >, 0);
        got += n;
    }
}

static uint64_t prop_uint(Rig *r, const char *path, const char *name)
{
    QDict *resp = qtest_qmp(r->qts, "{'execute':'qom-get','arguments':"
                            "{'path':%s,'property':%s}}", path, name);
    uint64_t v = qdict_get_uint(resp, "return");

    qobject_unref(resp);
    return v;
}

static char *prop_str(Rig *r, const char *path, const char *name)
{
    QDict *resp = qtest_qmp(r->qts, "{'execute':'qom-get','arguments':"
                            "{'path':%s,'property':%s}}", path, name);
    char *v = g_strdup(qdict_get_str(resp, "return"));

    qobject_unref(resp);
    return v;
}

static bool event_cb(QTestState *qts, const char *name, QDict *event,
                     void *opaque)
{
    Rig *r = opaque;

    if (!strcmp(name, "RESET")) {
        r->resets++;
    } else if (!strcmp(name, "SHUTDOWN")) {
        r->shutdowns++;
    }
    return false;
}

static bool halted(Rig *r)
{
    return prop_uint(r, NODE0, "halted-cpus") & 1;
}

static bool node1_halted(Rig *r)
{
    return prop_uint(r, "/machine/node1", "halted-cpus") & 1;
}

/* The machine took everything the board sent, and its main loop is idle. */
static void settle(Rig *r)
{
    for (int i = 0; i < 3; i++) {
        (void)prop_uint(r, SIGNALS, "lines");
        g_usleep(10 * 1000);
    }
}

static void wait_lines(Rig *r)
{
    int64_t deadline = g_get_monotonic_time() + TIMEOUT_MS * 1000;

    while (prop_uint(r, SIGNALS, "lines") != r->lines) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    settle(r);
}

static void wait_halted(Rig *r, bool want)
{
    int64_t deadline = g_get_monotonic_time() + TIMEOUT_MS * 1000;

    while (halted(r) != want) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void mark(Rig *r)
{
    qtest_writeq(r->qts, SCRATCH0, MARK);
    g_assert_cmphex(qtest_readq(r->qts, SCRATCH0), ==, MARK);
}

static bool marked(Rig *r)
{
    return qtest_readq(r->qts, SCRATCH0) == MARK;
}

/* One line changes, as its own message. */
static void board_set(Rig *r, unsigned line, bool level)
{
    r->lines = deposit32(r->lines, line - 1, 1, level);
    send_msg(r->sig_fd, CHANGE, ++r->seq, line, level);
    wait_lines(r);
}

/* Every line at once, as a state message, in the order the link applies. */
static void board_state(Rig *r, unsigned lines)
{
    r->lines = lines;
    send_msg(r->sig_fd, STATE, r->seq, 7, lines);
    wait_lines(r);
}

/* The board answers the machine's hello and state with its own. */
static void board_hello(Rig *r)
{
    recv_msg(r->sig_fd);
    recv_msg(r->sig_fd);
    send_msg(r->sig_fd, HELLO, EPOCH, 1, 2);
    send_msg(r->sig_fd, STATE, r->seq, 7, r->lines);
    wait_lines(r);
}

static void board_drop(Rig *r)
{
    close(r->sig_fd);
    r->sig_fd = -1;
    while (prop_uint(r, SIGNALS, "up")) {
        g_usleep(1000);
    }
}

static void board_return(Rig *r)
{
    r->sig_fd = accept_on(r->sig_listener);
    board_hello(r);
}

static char *zero_prom(void)
{
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    GError *error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("sgi-l1-attach-prom-XXXXXX", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

/* The machine with C-brick 0's L1 on links, and the board still away. */
static void rig_start(Rig *r)
{
    g_autofree char *sig = NULL, *node = NULL;
    GError *error = NULL;

    r->dir = g_dir_make_tmp("sgi-l1-attach-XXXXXX", &error);
    g_assert_no_error(error);
    r->prom = zero_prom();
    r->sig_fd = r->node_fd = -1;
    sig = g_build_filename(r->dir, "signals.sock", NULL);
    node = g_build_filename(r->dir, "node.sock", NULL);
    r->sig_listener = listen_on(sig);
    r->node_listener = listen_on(node);
    /* The rig plays a peer that asserts hold before raising node-power. */
    r->lines = HOLD;
    r->qts = qtest_initf("-M origin3000,controller-l1-signal-chardev.0=0=sig,"
                         "controller-l1-node-chardev.0=0/0=nl "
                         "-cpu R12000 -smp 8 -m 8192 -bios %s -nodefaults "
                         "-display none -no-shutdown "
                         "-chardev socket,id=sig,path=%s,reconnect-ms=50 "
                         "-chardev socket,id=nl,path=%s,reconnect-ms=50",
                         r->prom, sig, node);
    qtest_qmp_set_event_callback(r->qts, event_cb, r);
}

static void rig_board(Rig *r)
{
    r->sig_fd = accept_on(r->sig_listener);
    r->node_fd = accept_on(r->node_listener);
    board_hello(r);
}

static Rig *rig_new(void)
{
    Rig *r = g_new0(Rig, 1);

    rig_start(r);
    rig_board(r);
    return r;
}

static void rig_free(Rig *r)
{
    g_autofree char *sig = g_build_filename(r->dir, "signals.sock", NULL);
    g_autofree char *node = g_build_filename(r->dir, "node.sock", NULL);

    qtest_quit(r->qts);
    for (int *fd = &r->sig_listener; fd <= &r->node_fd; fd++) {
        if (*fd >= 0) {
            close(*fd);
        }
    }
    unlink(sig);
    unlink(node);
    rmdir(r->dir);
    unlink(r->prom);
    g_free(r->dir);
    g_free(r->prom);
    g_free(r);
}

/* Power up as the controller does: hold, supply, then release. */
static void power_up(Rig *r)
{
    board_set(r, NODE_POWER, true);
    board_set(r, RESET_HOLD, false);
    wait_halted(r, false);
}

/* A board holding a powered-off node in reset starts it by a release. */
static void test_power_up_hold_then_release(void)
{
    Rig *r = rig_new();

    g_assert_true(halted(r));
    board_set(r, NODE_POWER, true);
    /* The supply is on and the start is owed until the hold lets go. */
    g_assert_true(halted(r));
    board_set(r, RESET_HOLD, false);
    wait_halted(r, false);
    g_assert_cmpuint(r->resets, ==, 0);
    rig_free(r);
}

/* A reset is a hold and a release: the node restarts alone and runs on. */
static void test_reset_while_powered(void)
{
    Rig *r = rig_new();

    power_up(r);
    mark(r);
    board_set(r, RESET_HOLD, true);
    /* The hold alone changes nothing. */
    g_assert_true(marked(r));
    g_assert_false(halted(r));
    board_set(r, RESET_HOLD, false);
    g_assert_false(marked(r));
    g_assert_false(halted(r));
    g_assert_false(node1_halted(r));
    /* Another node runs, so this is not a whole-machine reset. */
    g_assert_cmpuint(r->resets, ==, 0);
    rig_free(r);
}

/* Both lines rise together and the hold never lets go: nothing starts. */
static void test_power_up_hold_never_released(void)
{
    Rig *r = rig_new();

    board_state(r, 0);
    board_state(r, POWER | HOLD);
    g_assert_true(halted(r));
    board_set(r, RESET_HOLD, false);
    wait_halted(r, false);
    rig_free(r);
}

static void test_power_loss(void)
{
    Rig *r = rig_new();

    power_up(r);
    board_set(r, NODE_POWER, false);
    wait_halted(r, true);
    /* The brick's other node, behavioural, runs on. */
    g_assert_false(node1_halted(r));
    g_assert_cmpuint(r->shutdowns, ==, 0);
    rig_free(r);
}

/* A supply rising with no hold starts the node; a hold first still owes it. */
static void test_power_rise_with_hold_low(void)
{
    Rig *r = rig_new();

    board_state(r, 0);
    board_state(r, POWER);
    wait_halted(r, false);
    board_set(r, NODE_POWER, false);
    wait_halted(r, true);

    /* The hold first, then the supply, as two changes. */
    board_set(r, RESET_HOLD, true);
    board_set(r, NODE_POWER, true);
    g_assert_true(halted(r));
    board_set(r, RESET_HOLD, false);
    wait_halted(r, false);
    rig_free(r);
}

/* A supply that falls while a start is owed takes the start with it. */
static void test_power_fall_clears_owed_start(void)
{
    Rig *r = rig_new();

    board_set(r, NODE_POWER, true);
    board_set(r, NODE_POWER, false);
    board_set(r, RESET_HOLD, false);
    g_assert_true(halted(r));
    g_assert_cmpuint(r->resets, ==, 0);
    rig_free(r);
}

static void test_re_rise_owes_a_start(void)
{
    Rig *r = rig_new();

    board_set(r, NODE_POWER, true);
    board_set(r, NODE_POWER, false);
    board_set(r, NODE_POWER, true);
    g_assert_true(halted(r));
    board_set(r, RESET_HOLD, false);
    wait_halted(r, false);
    rig_free(r);
}

static void test_second_release_is_a_reset(void)
{
    Rig *r = rig_new();

    power_up(r);
    for (int i = 0; i < 2; i++) {
        mark(r);
        board_set(r, RESET_HOLD, true);
        board_set(r, RESET_HOLD, false);
        g_assert_false(marked(r));
        g_assert_false(halted(r));
    }
    rig_free(r);
}

/* The hold stays asserted while the supply goes away and returns. */
static void test_power_cycle_across_a_hold(void)
{
    Rig *r = rig_new();

    power_up(r);
    board_set(r, RESET_HOLD, true);
    board_set(r, NODE_POWER, false);
    wait_halted(r, true);
    board_set(r, NODE_POWER, true);
    g_assert_true(halted(r));
    board_set(r, RESET_HOLD, false);
    wait_halted(r, false);
    rig_free(r);
}

/* The documented approximation: a hold on a running node does not stop it. */
static void test_hold_without_release_keeps_running(void)
{
    Rig *r = rig_new();

    power_up(r);
    mark(r);
    board_set(r, RESET_HOLD, true);
    g_assert_false(halted(r));
    g_assert_true(marked(r));
    rig_free(r);
}

static void test_release_while_off_is_nothing(void)
{
    Rig *r = rig_new();

    board_set(r, RESET_HOLD, false);
    g_assert_true(halted(r));
    board_set(r, RESET_HOLD, true);
    board_set(r, RESET_HOLD, false);
    g_assert_true(halted(r));
    g_assert_cmpuint(r->resets, ==, 0);
    rig_free(r);
}

/*
 * The link drops with the hold asserted and the board releases it while
 * away: the release arrives at resync as a level change, in either order
 * of the two lines.
 */
static void test_link_drop_and_resync(void)
{
    Rig *r = rig_new();

    /* A running node's release lost to an outage is a reset at resync. */
    power_up(r);
    board_set(r, RESET_HOLD, true);
    mark(r);
    board_drop(r);
    r->lines &= ~HOLD;
    r->seq++;
    board_return(r);
    g_assert_false(marked(r));
    g_assert_false(halted(r));

    /* The supply rose and the hold went while away: power, then hold. */
    board_set(r, NODE_POWER, false);
    wait_halted(r, true);
    board_set(r, RESET_HOLD, true);
    board_set(r, NODE_POWER, true);
    board_drop(r);
    r->lines = POWER;
    r->seq++;
    board_return(r);
    wait_halted(r, false);

    /* Hold, then power, as changes after the link is back. */
    board_set(r, NODE_POWER, false);
    wait_halted(r, true);
    board_set(r, RESET_HOLD, false);
    board_set(r, NODE_POWER, true);
    wait_halted(r, false);
    rig_free(r);
}

/* The board's process is not reset with the machine, nor is its owed start. */
static void test_system_reset_keeps_the_owed_start(void)
{
    Rig *r = rig_new();

    board_set(r, NODE_POWER, true);
    qtest_system_reset(r->qts);
    g_assert_true(halted(r));
    board_set(r, RESET_HOLD, false);
    wait_halted(r, false);
    rig_free(r);
}

/* With no other node powered, a node reset is the whole machine's. */
static void test_only_powered_node_resets_the_machine(void)
{
    Rig *r = rig_new();

    power_up(r);
    qtest_set_irq_in(r->qts, "/machine/node1", "power", 0, 0);
    g_assert_true(node1_halted(r));
    board_set(r, RESET_HOLD, true);
    board_set(r, RESET_HOLD, false);
    qtest_qmp_eventwait(r->qts, "RESET");
    g_assert_false(halted(r));
    g_assert_true(node1_halted(r));
    rig_free(r);
}

/* Only C-brick 0's controller is a link, and the edges to it are gone. */
static void test_firmware_l1_replaces_one_controller(void)
{
    Rig *r = rig_new();
    g_autofree char *edges = prop_str(r, "/machine", "l1-dropped-edges");
    g_autofree char *type0 = prop_str(r, L1, "type");
    g_autofree char *type1 = prop_str(r, "/machine/node1/l1", "type");
    g_autofree char *peer = prop_str(r, "/machine/node1/l1", "peer");
    g_autofree char *io_peer = prop_str(r, "/machine/node1/l1", "io-peer");

    g_assert_cmpstr(type0, ==, "sgi-l1-link");
    g_assert_cmpstr(type1, ==, "sgi-l1");
    g_assert_cmpstr(edges, ==, "io-peer node0>controller2,peer 0>1,peer 1>0");
    g_assert_cmpstr(peer, ==, "");
    g_assert_cmpstr(io_peer, ==, "");
    rig_free(r);
}

/* Bytes cross the node's UART window and the node socket both ways. */
static void test_node_bytes_cross_the_window(void)
{
    Rig *r = rig_new();
    uint8_t byte = 0;
    struct pollfd p = { .events = POLLIN };
    int64_t deadline = g_get_monotonic_time() + TIMEOUT_MS * 1000;

    while (!prop_uint(r, L1, "nodes-up")) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_writeq(r->qts, UART_DATA, 0x41);
    p.fd = r->node_fd;
    g_assert_cmpint(poll(&p, 1, TIMEOUT_MS), ==, 1);
    g_assert_cmpint(read(r->node_fd, &byte, 1), ==, 1);
    g_assert_cmphex(byte, ==, 0x41);

    g_assert_cmphex(qtest_readq(r->qts, UART_LSR) & 1, ==, 0);
    byte = 0x42;
    g_assert_cmpint(write(r->node_fd, &byte, 1), ==, 1);
    while (!(qtest_readq(r->qts, UART_LSR) & 1)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readq(r->qts, UART_DATA) & 0xff, ==, 0x42);
    rig_free(r);
}

/*
 * Both C-bricks on firmware controllers that never connect: no node runs,
 * the machine does not end, and it still has host ids and survives a reset.
 */
static void test_no_board_keeps_the_machine_idle(void)
{
    Rig r = { .sig_fd = -1, .node_fd = -1, .sig_listener = -1,
              .node_listener = -1 };
    g_autofree char *ids = NULL;

    r.prom = zero_prom();
    r.dir = g_dir_make_tmp("sgi-l1-attach-XXXXXX", NULL);
    r.qts = qtest_initf("-M origin3000,"
                        "controller-l1-signal-chardev.0=0=s0,"
                        "controller-l1-node-chardev.0=0/0=n0,"
                        "controller-l1-signal-chardev.1=1=s1,"
                        "controller-l1-node-chardev.1=1/0=n1 "
                        "-cpu R12000 -smp 8 -m 8192 -bios %s -nodefaults "
                        "-display none -no-shutdown "
                        "-chardev socket,id=s0,path=%s/a,reconnect-ms=50 "
                        "-chardev socket,id=n0,path=%s/b,reconnect-ms=50 "
                        "-chardev socket,id=s1,path=%s/c,reconnect-ms=50 "
                        "-chardev socket,id=n1,path=%s/d,reconnect-ms=50",
                        r.prom, r.dir, r.dir, r.dir, r.dir);
    qtest_qmp_set_event_callback(r.qts, event_cb, &r);
    settle(&r);
    g_assert_true(halted(&r));
    g_assert_true(node1_halted(&r));
    g_assert_cmpuint(prop_uint(&r, SIGNALS, "up"), ==, 0);
    ids = prop_str(&r, "/machine", "hostids");
    g_assert_cmpuint(strlen(ids), >, 0);
    qtest_system_reset(r.qts);
    settle(&r);
    g_assert_true(halted(&r));
    g_assert_true(node1_halted(&r));
    g_assert_cmpuint(r.shutdowns, ==, 0);
    qtest_quit(r.qts);
    unlink(r.prom);
    rmdir(r.dir);
}

/* The firmware may power its node again: the last node off is not the end. */
static void test_last_node_off_keeps_the_machine(void)
{
    Rig *r = rig_new();

    power_up(r);
    qtest_set_irq_in(r->qts, "/machine/node1", "power", 0, 0);
    board_set(r, NODE_POWER, false);
    wait_halted(r, true);
    settle(r);
    g_assert_true(node1_halted(r));
    g_assert_cmpuint(r->shutdowns, ==, 0);
    board_set(r, NODE_POWER, true);
    wait_halted(r, false);
    rig_free(r);
}

/* Without a firmware controller the last node going off still ends it. */
static void test_behavioural_machine_still_ends(void)
{
    g_autofree char *prom = zero_prom();
    QTestState *qts = qtest_initf("-M origin3000 -cpu R12000 -smp 8 -m 8192 "
                                  "-bios %s -nodefaults -display none "
                                  "-no-shutdown", prom);

    qtest_set_irq_in(qts, "/machine/node0", "power", 0, 0);
    qtest_set_irq_in(qts, "/machine/node1", "power", 0, 0);
    qtest_qmp_eventwait(qts, "SHUTDOWN");
    qtest_quit(qts);
    unlink(prom);
}

#define OPTIONS "controller-l1-signal-chardev.0=0=a,"\
                "controller-l1-node-chardev.0=0/0=b"

static void assert_refused(const char *machine, const char *extra,
                           const char *message)
{
    g_autofree char *cmd = g_strdup_printf(
        "%s -M %s -cpu R12000 -smp 8 -m 8192 -bios /dev/null -nodefaults "
        "-display none -accel qtest -qtest stdio -S "
        "-chardev null,id=a -chardev null,id=b -chardev null,id=c %s",
        qtest_qemu_binary(NULL), machine, extra);
    g_auto(GStrv) argv = NULL;
    g_autofree char *err = NULL;
    int status = 0;

    g_assert_true(g_shell_parse_argv(cmd, NULL, &argv, NULL));
    g_assert_true(g_spawn_sync(NULL, argv, NULL,
                               G_SPAWN_STDOUT_TO_DEV_NULL |
                               G_SPAWN_STDIN_FROM_DEV_NULL, NULL, NULL, NULL,
                               &err, &status, NULL));
    g_assert_false(WIFSIGNALED(status));
    g_assert_cmpint(WEXITSTATUS(status), !=, 0);
    if (!strstr(err, message)) {
        g_printerr("expected '%s' in: %s\n", message, err);
    }
    g_assert_nonnull(strstr(err, message));
}

static void test_refusals(void)
{
    static const struct {
        const char *machine;
        const char *extra;
        const char *message;
    } cases[] = {
        { "origin3000,controller-l1-signal-chardev.0=0=a", "",
          "needs both an L1 signal chardev and an L1 node chardev" },
        { "origin3000,controller-l1-node-chardev.0=0/0=b", "",
          "needs both an L1 signal chardev and an L1 node chardev" },
        { "origin3000,controller-l1-signal-chardev.0=0=a,"
          "controller-l1-node-chardev.0=0/1=b", "", "one node link" },
        { "origin3000,controller-l1-signal-chardev.0=0/0=a", "",
          "invalid controller input" },
        { "origin3000,controller-l1-signal-chardev.0=0=a,"
          "controller-l1-node-chardev.0=0=b", "",
          "invalid controller input" },
        /* The I-brick's controller, a missing controller. */
        { "origin3000,controller-l1-signal-chardev.0=2=a,"
          "controller-l1-node-chardev.0=2/0=b", "",
          "does not take a firmware L1" },
        { "origin3000,controller-l1-signal-chardev.0=9=a,"
          "controller-l1-node-chardev.0=9/0=b", "",
          "does not take a firmware L1" },
        { "origin3000,controller-l1-signal-chardev.0=0=a,"
          "controller-l1-node-chardev.0=0/0=gone", "", "does not exist" },
        { "origin3000,controller-l1-signal-chardev.0=0=a,"
          "controller-l1-node-chardev.0=0/0=a", "",
          "controller inputs reuse" },
        { "origin3000," OPTIONS ",controller-l1-signal-chardev.1=1=a,"
          "controller-l1-node-chardev.1=1/0=c", "",
          "controller inputs reuse" },
        { "origin3000," OPTIONS ",controller-l1-signal-chardev.1=0=c", "",
          "duplicate controller input" },
        { "origin3000," OPTIONS ",controller-l2-chardev.0=0=c", "",
          "takes no L2, power-bay or partition option" },
        { "origin3000," OPTIONS ",controller-pbay-chardev.0=0=c", "",
          "takes no L2, power-bay or partition option" },
        { "origin3000," OPTIONS ",controller-l2-partition-flag.0=0=1", "",
          "takes no L2, power-bay or partition option" },
        /* A partitioned machine, and one that needs routers. */
        { "origin3000,topology=origin3400-two-partitions," OPTIONS, "",
          "unpartitioned topology" },
        { "origin3000,topology=origin3400," OPTIONS, "",
          "no routers, L1 management" },
        { "origin3000," OPTIONS, "-accel tcg -icount shift=0",
          "-icount is not allowed" },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        assert_refused(cases[i].machine, cases[i].extra, cases[i].message);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi-l1-attach/power-up-hold-then-release",
                   test_power_up_hold_then_release);
    qtest_add_func("/sgi-l1-attach/reset-while-powered",
                   test_reset_while_powered);
    qtest_add_func("/sgi-l1-attach/power-up-hold-never-released",
                   test_power_up_hold_never_released);
    qtest_add_func("/sgi-l1-attach/power-loss", test_power_loss);
    qtest_add_func("/sgi-l1-attach/power-rise-with-hold-low",
                   test_power_rise_with_hold_low);
    qtest_add_func("/sgi-l1-attach/power-fall-clears-owed-start",
                   test_power_fall_clears_owed_start);
    qtest_add_func("/sgi-l1-attach/re-rise-owes-a-start",
                   test_re_rise_owes_a_start);
    qtest_add_func("/sgi-l1-attach/second-release-is-a-reset",
                   test_second_release_is_a_reset);
    qtest_add_func("/sgi-l1-attach/power-cycle-across-a-hold",
                   test_power_cycle_across_a_hold);
    qtest_add_func("/sgi-l1-attach/hold-without-release-keeps-running",
                   test_hold_without_release_keeps_running);
    qtest_add_func("/sgi-l1-attach/release-while-off-is-nothing",
                   test_release_while_off_is_nothing);
    qtest_add_func("/sgi-l1-attach/link-drop-and-resync",
                   test_link_drop_and_resync);
    qtest_add_func("/sgi-l1-attach/system-reset-keeps-the-owed-start",
                   test_system_reset_keeps_the_owed_start);
    qtest_add_func("/sgi-l1-attach/only-powered-node-resets-the-machine",
                   test_only_powered_node_resets_the_machine);
    qtest_add_func("/sgi-l1-attach/firmware-l1-replaces-one-controller",
                   test_firmware_l1_replaces_one_controller);
    qtest_add_func("/sgi-l1-attach/node-bytes-cross-the-window",
                   test_node_bytes_cross_the_window);
    qtest_add_func("/sgi-l1-attach/no-board-keeps-the-machine-idle",
                   test_no_board_keeps_the_machine_idle);
    qtest_add_func("/sgi-l1-attach/last-node-off-keeps-the-machine",
                   test_last_node_off_keeps_the_machine);
    qtest_add_func("/sgi-l1-attach/behavioural-machine-still-ends",
                   test_behavioural_machine_still_ends);
    qtest_add_func("/sgi-l1-attach/refusals", test_refusals);
    return g_test_run();
}
