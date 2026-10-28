/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The Cx-brick profile of the L1 controller: one controller serving up to
 * four IP53 node boards, each through its own UART window. Each case drives
 * the controller with the same protocol frames the Origin 300 profile uses;
 * no firmware runs.
 *
 * Most of what is checked here is a declared model choice, not captured
 * controller output: one window per board named by its board number, the
 * console default, the per-link position and port field, the graded request
 * codes and the board-scoped records. docs/system/mips/sgi/hardware/l1.rst
 * lists them, and these cases pin them so a change to one is deliberate.
 */
#include "qemu/osdep.h"
#include <poll.h>
#include <sys/socket.h>
#include "qemu/bswap.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include "sgi-wait.h"
#include "sgi-l1-test.h"

#define SGI_L1_TEST_BOARDS 4
#define BASE UINT64_C(0x11000000)
#define STRIDE UINT64_C(0x1000)
#define L1 "/machine/peripheral/l1c"

#define LOCAL_GENERAL UINT32_C(0x0ffff006)
#define LOCAL_COMMAND UINT32_C(0x0ffff003)
#define ATTACHED_IO   UINT32_C(0x4ffff006)

/* Named rack 1, slot 7: the guide's example brick 001c07. */
#define NAMED "brick-type=cxbrick,named=on,rack=1,bay=7"
/* A letter the test supplies; the Cx-brick's own is unknown. */
#define TYPED NAMED ",brick-type-code=67"
#define ID "001c07"
#define PROMPT ID "-L1> "
#define POWERING "INFO: " ID " powering up the system.\r\n" PROMPT "\r\n"
#define ENTER(target) \
    "entering console mode  " ID " " target ", <CTRL_T> to escape to L1\r\n"
#define RETURN(target) \
    "returning to console mode  " ID " " target \
    ", <CTRL_T> to escape to L1\r\n"
#define ESCAPE "escaping to L1 system controller\r\n" PROMPT
#define NO_CONSOLE \
    "no response from " ID " junk bus console UART:UART_TIMEOUT\r\n"

typedef struct CxRig {
    QTestState *qts;
    int console;
    unsigned boards;
} CxRig;

/*
 * One controller with @props and a UART window for each board in @boards,
 * transport-index the board number; each board has processors unless
 * @props names compute-boards. With @console the brick's console is a
 * socket the test holds.
 */
static void rig_start(CxRig *rig, const char *props, unsigned boards,
                      bool console, const char *extra)
{
    GString *line = g_string_new("-M none -cpu R10000 -S ");
    int sv[2] = { -1, -1 };

    if (console) {
        g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
        g_string_append_printf(line, "-chardev socket,id=console,fd=%d ",
                               sv[1]);
    }
    g_string_append_printf(line, "-device sgi-l1,id=l1c,%s,boards=%u%s ",
                           props, boards, console ? ",chardev=console" : "");
    /* Every fitted board has processors unless the case says otherwise. */
    if (!strstr(props, "compute-boards")) {
        g_string_truncate(line, line->len - 1);
        g_string_append_printf(line, ",compute-boards=%u ", boards);
    }
    for (unsigned i = 0; i < SGI_L1_TEST_BOARDS; i++) {
        if (boards & (1u << i)) {
            g_string_append_printf(line, "-device sgi-l1-uart,id=l1u%u,"
                                   "controller=l1c,transport-index=%u ", i,
                                   i);
        }
    }
    g_string_append(line, extra);
    rig->qts = qtest_init(line->str);
    g_string_free(line, TRUE);
    if (console) {
        close(sv[1]);
    }
    rig->console = sv[0];
    rig->boards = boards;
    for (unsigned i = 0; i < SGI_L1_TEST_BOARDS; i++) {
        if (boards & (1u << i)) {
            g_autofree char *path =
                g_strdup_printf("/machine/peripheral/l1u%u", i);

            qtest_sysbus_mmio_map(rig->qts, path, 0, BASE + i * STRIDE);
        }
    }
}

static void rig_stop(CxRig *rig)
{
    qtest_quit(rig->qts);
    if (rig->console >= 0) {
        close(rig->console);
    }
}

static uint64_t link_base(unsigned board)
{
    return BASE + board * STRIDE;
}

/* No frame waits on any board's window. */
static void expect_links_idle(CxRig *rig)
{
    for (unsigned i = 0; i < SGI_L1_TEST_BOARDS; i++) {
        if (rig->boards & (1u << i)) {
            g_assert_cmphex(qtest_readq(rig->qts, link_base(i) + 0x28), ==,
                            0x60);
        }
    }
}

static void send_on(CxRig *rig, unsigned board, const uint8_t *frame,
                    size_t length)
{
    sgi_l1_test_send(rig->qts, link_base(board), frame, length, false);
}

static size_t receive_on(CxRig *rig, unsigned board, uint8_t *reply)
{
    return sgi_l1_test_receive(rig->qts, link_base(board), reply, 131);
}

static size_t rpc(CxRig *rig, unsigned board, uint32_t destination,
                  uint16_t code, const uint8_t *args, size_t args_len,
                  unsigned count, uint8_t *reply)
{
    uint8_t request[136] = { 0x10, destination >> 24, destination >> 16,
                             destination >> 8, destination, code >> 8, code,
                             count };
    size_t length;

    g_assert_cmpuint(args_len, <=, sizeof(request) - 8);
    if (args_len) {
        memcpy(request + 8, args, args_len);
    }
    send_on(rig, board, request, 8 + args_len);
    length = receive_on(rig, board, reply);
    /* The answer leaves on the asking board's window and nowhere else. */
    expect_links_idle(rig);
    return length;
}

static void integer(uint8_t *out, uint32_t value)
{
    out[0] = 0;
    stl_be_p(out + 1, value);
}

static void expect_code(const uint8_t *reply, size_t length, int32_t code)
{
    g_assert_cmpuint(length, >=, 6);
    g_assert_cmphex(reply[0], ==, 0x30);
    g_assert_cmpint((int32_t)ldl_be_p(reply + 1), ==, code);
}

static uint32_t position(CxRig *rig, unsigned board)
{
    uint8_t reply[131];
    size_t length = rpc(rig, board, LOCAL_GENERAL, 0xe, NULL, 0, 0, reply);

    g_assert_cmpuint(length, ==, 11);
    expect_code(reply, length, 0);
    g_assert_cmpuint(reply[5], ==, 1);
    return ldl_be_p(reply + 7);
}

static void select_subchannel(CxRig *rig, unsigned board, uint32_t value)
{
    uint8_t args[5], reply[131];
    size_t length;

    integer(args, value);
    length = rpc(rig, board, LOCAL_GENERAL, 0x1002, args, sizeof(args), 1,
                 reply);
    g_assert_cmpuint(length, ==, 6);
    expect_code(reply, length, 0);
}

/* Runs one operator command line on @board's window; returns its lines. */
static char *command(CxRig *rig, unsigned board, const char *text,
                     int32_t *code)
{
    uint8_t args[130];
    uint8_t reply[131];
    GString *out = g_string_new(NULL);
    size_t length, offset;

    g_assert_cmpuint(strlen(text), <, 120);
    args[0] = 1;
    memcpy(args + 1, text, strlen(text) + 1);
    length = rpc(rig, board, LOCAL_COMMAND, 0, args, 2 + strlen(text), 1,
                 reply);
    g_assert_cmpuint(length, >=, 6);
    if (code) {
        *code = (int32_t)ldl_be_p(reply + 1);
    }
    offset = 6;
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

static void expect_command(CxRig *rig, unsigned board, const char *text,
                           const char *expected)
{
    g_autofree char *output = command(rig, board, text, NULL);

    g_assert_cmpstr(output, ==, expected);
}

static void console_expect(CxRig *rig, const char *expected)
{
    g_autofree char *actual = g_malloc(strlen(expected) + 1);
    size_t length = strlen(expected), offset = 0;

    while (offset < length) {
        struct pollfd pfd = { .fd = rig->console, .events = POLLIN };
        ssize_t n;

        g_assert_cmpint(poll(&pfd, 1, SGI_WAIT_S * 1000), ==, 1);
        n = read(rig->console, actual + offset, length - offset);
        g_assert_cmpint(n, >, 0);
        offset += n;
    }
    actual[length] = 0;
    g_assert_cmpstr(actual, ==, expected);
}

static void console_send(CxRig *rig, const char *text)
{
    g_assert_cmpint(write(rig->console, text, strlen(text)), ==,
                    strlen(text));
}

/* Console output from @board's node, as the PROM sends it on CPU A. */
static void node_output(CxRig *rig, unsigned board, const char *text)
{
    uint8_t frame[64] = { 0x40 };

    g_assert_cmpuint(strlen(text), <, sizeof(frame));
    memcpy(frame + 1, text, strlen(text));
    send_on(rig, board, frame, 1 + strlen(text));
}

/* A keystroke's request frame on @board's window: subchannel and byte. */
static void expect_keystroke(CxRig *rig, unsigned board, uint8_t subchannel,
                             uint8_t byte)
{
    uint8_t reply[131];
    SgiWait w;

    sgi_wait_start(&w, "keystroke frame on board %u", board);
    while (!(qtest_readq(rig->qts, link_base(board) + 0x28) & 1)) {
        sgi_wait_pause(&w, 1000);
    }
    g_assert_cmpuint(receive_on(rig, board, reply), ==, 2);
    g_assert_cmphex(reply[0], ==, subchannel);
    g_assert_cmphex(reply[1], ==, byte);
    expect_links_idle(rig);
}

/*
 * Every board's window answers the identity requests with the brick's slot
 * plus its board number, since the stock PROM refuses two modules at one
 * slot, and the type the test supplied. The reply goes back on the window
 * that asked; the others stay empty.
 */
static void test_identity(void)
{
    CxRig rig;
    uint8_t reply[131];
    size_t length;

    rig_start(&rig, TYPED, 0xf, false, "");
    for (unsigned board = 0; board < 4; board++) {
        length = rpc(&rig, board, LOCAL_GENERAL, 3, NULL, 0, 0, reply);
        g_assert_cmpuint(length, ==, 16);
        expect_code(&reply[0], length, 0);
        g_assert_cmphex(ldl_be_p(reply + 7), ==, (1 << 6) | (7 + board));
        g_assert_cmphex(ldl_be_p(reply + 12), ==, 67);
        length = rpc(&rig, board, LOCAL_GENERAL, 2, NULL, 0, 0, reply);
        g_assert_cmpuint(length, ==, 11);
        expect_code(reply, length, 0);
        g_assert_cmphex(ldl_be_p(reply + 7), ==, (1 << 6) | (7 + board));
    }
    rig_stop(&rig);

    /* Without a supplied letter the brick has no type to give. */
    rig_start(&rig, NAMED, 0x5, false, "");
    length = rpc(&rig, 2, LOCAL_GENERAL, 3, NULL, 0, 0, reply);
    g_assert_cmpuint(length, ==, 6);
    expect_code(reply, length, -104);
    rig_stop(&rig);

    /* An unnamed brick is ---c--, with the guide's unknown-type letter. */
    rig_start(&rig, "brick-type=cxbrick", 0x3, false, "");
    expect_command(&rig, 1, "config", "0: ---c--\n");
    expect_command(&rig, 0, "brick",
                   "rack: ---, slot: --, partition: 0, type: ?, "
                   "source: EEPROM\n");
    rig_stop(&rig);
}

/*
 * Each board's serial request is answered with the brick's serial and the
 * board number, so a PROM that tells nodes apart by that serial finds four
 * (an approximation; no source says what a Cx-brick returns).
 */
static void test_board_serials(void)
{
    CxRig rig;
    uint8_t reply[131];

    rig_start(&rig, NAMED ",brick-serial=CXTEST", 0xf, false, "");
    for (unsigned board = 0; board < 4; board++) {
        g_autofree char *expected = g_strdup_printf("CXTEST.%u", board);
        size_t length = rpc(&rig, board, LOCAL_GENERAL, 4, NULL, 0, 0, reply);

        expect_code(reply, length, 0);
        g_assert_cmpstr((const char *)reply + 7, ==, expected);
    }
    rig_stop(&rig);
}

/*
 * A Cx-brick's L1 reaches one I-brick: the attached I/O brick request is
 * the I-brick's own to answer, in the C-brick's request form (an
 * approximation; the addressing of a Cx-brick's four I/O ports is unknown).
 */
static void test_io_peer(void)
{
    CxRig rig;
    uint8_t reply[131];
    size_t length;
    QDict *response;

    rig_start(&rig, TYPED, 0xf, false,
              "-device sgi-l1,id=ib,brick-type=ibrick,named=on,rack=1,bay=16,"
              "mac=02:ab:10:00:00:01");
    response = qtest_qmp(rig.qts,
        "{'execute':'qom-set','arguments':{'path':'" L1 "',"
        "'property':'io-peer','value':'/machine/peripheral/ib'}}");
    g_assert_false(qdict_haskey(response, "error"));
    qobject_unref(response);
    length = rpc(&rig, 0, ATTACHED_IO, 3, NULL, 0, 0, reply);
    g_assert_cmpuint(length, ==, 16);
    expect_code(reply, length, 0);
    g_assert_cmphex(ldl_be_p(reply + 7), ==, (1 << 6) | 16);
    g_assert_cmphex(ldl_be_p(reply + 12), ==, 'I');
    rig_stop(&rig);
}

/* A request on board 2's window is answered there and nowhere else. */
static void test_reply_routing(void)
{
    CxRig rig;
    uint8_t reply[131];

    rig_start(&rig, TYPED, 0xf, false, "");
    expect_links_idle(&rig);
    g_assert_cmpuint(rpc(&rig, 2, LOCAL_GENERAL, 3, NULL, 0, 0, reply), ==,
                     16);
    rig_stop(&rig);
}

/*
 * Each window keeps its own port field: 0x000e answers that window's board
 * number after L1 power-on, and 0x1002 rewrites only the asking window's.
 */
static void test_positions(void)
{
    CxRig rig;

    rig_start(&rig, NAMED, 0xf, false, "");
    for (unsigned board = 0; board < 4; board++) {
        g_assert_cmpuint(position(&rig, board), ==, board);
    }
    select_subchannel(&rig, 1, 4);
    g_assert_cmpuint(position(&rig, 1), ==, 1);
    g_assert_cmpuint(position(&rig, 2), ==, 2);
    /*
     * The existing rule ors the argument into the field and keeps the
     * position bits, so 0x40 on board 1's field (0x20) leaves 0x60.
     */
    select_subchannel(&rig, 1, 0x40);
    g_assert_cmpuint(position(&rig, 1), ==, 3);
    g_assert_cmpuint(position(&rig, 3), ==, 3);
    g_assert_cmpuint(position(&rig, 0), ==, 0);
    rig_stop(&rig);
}

/*
 * Codes outside the profile's graded set, the attached-brick requests among
 * them, are refused; 0x2000 is the Origin 300 image's unknown code.
 */
static void test_request_grades(void)
{
    static const uint16_t refused[] = {
        0xa, 0xb, 0xc, 0xd, 0xf, 0x10, 0x11, 0x1001, 0x1008, 0x1009,
        0x100a, 0x100b, 0x100c, 0x2000, 0x2002, 0x2003, 0x2004, 0x2007,
        0x2008, 0x4000, 0x4002,
    };
    CxRig rig;
    uint8_t reply[131];
    size_t length;

    rig_start(&rig, NAMED, 0x3, false, "");
    for (size_t i = 0; i < ARRAY_SIZE(refused); i++) {
        length = rpc(&rig, 1, LOCAL_GENERAL, refused[i], NULL, 0, 0, reply);
        g_assert_cmpuint(length, ==, 6);
        expect_code(reply, length, -101);
    }
    /* The attached I/O brick is not modelled: the router's error reply. */
    length = rpc(&rig, 0, ATTACHED_IO, 3, NULL, 0, 0, reply);
    g_assert_cmpuint(length, ==, 9);
    g_assert_cmphex((uint32_t)ldl_be_p(reply + 1), ==, 0xffffffff);
    g_assert_cmphex(ldl_be_p(reply + 5), ==, 8);
    /* The firmware revision is unknown for the Cx-brick. */
    length = rpc(&rig, 0, LOCAL_GENERAL, 5, NULL, 0, 0, reply);
    expect_code(reply, length, -104);
    rig_stop(&rig);
}

/*
 * Board n's window reads board n's node board record at component 0. Other
 * component numbers, including the ones the model keeps other boards'
 * records at, answer minus 104.
 */
static void test_board_records(void)
{
    CxRig rig;
    uint8_t args[20], reply[131];
    size_t length;

    rig_start(&rig, NAMED ",identity-index=2", 0xf, false, "");
    for (unsigned board = 0; board < 4; board++) {
        g_autofree char *serial = g_strdup_printf("EMUN%02u", 8 + board);
        bool found = false;

        integer(args, 0);
        integer(args + 5, 2);
        integer(args + 10, 0);
        integer(args + 15, 116);
        length = rpc(&rig, board, LOCAL_GENERAL, 6, args, sizeof(args), 4,
                     reply);
        expect_code(reply, length, 0);
        g_assert_cmpuint(reply[5], ==, 2);
        for (size_t i = 12; i + strlen(serial) <= length; i++) {
            found |= !memcmp(reply + i, serial, strlen(serial));
        }
        g_assert_true(found);
        for (unsigned component = 1; component < 8; component++) {
            integer(args, component);
            length = rpc(&rig, board, LOCAL_GENERAL, 6, args, sizeof(args),
                         4, reply);
            g_assert_cmpuint(length, ==, 6);
            expect_code(reply, length, -104);
        }
    }
    rig_stop(&rig);
}

/*
 * The brick's one console shows every node's output with the filter off,
 * and only the selected board's with it on.
 */
static void test_console_filter(void)
{
    CxRig rig;

    rig_start(&rig, NAMED, 0xf, true, "");
    console_expect(&rig, POWERING ENTER("console0"));
    node_output(&rig, 0, "zero");
    console_expect(&rig, "zero");
    node_output(&rig, 3, "three");
    console_expect(&rig, "three");
    expect_command(&rig, 0, "select",
                   "console input: " ID " console0\n"
                   "console output: not filtered.\n");
    expect_command(&rig, 0, "select sub console3", "");
    expect_command(&rig, 0, "select filter on", "");
    expect_command(&rig, 0, "select",
                   "console input: " ID " console3\n"
                   "console output: filtered.\n");
    node_output(&rig, 0, "hidden");
    node_output(&rig, 3, "shown");
    console_expect(&rig, "shown");
    rig_stop(&rig);
}

/*
 * The default console is board 0's console subchannel, or the lowest board
 * with processors when board 0 has none (a declared choice).
 */
static void test_console_default(void)
{
    CxRig rig;

    rig_start(&rig, NAMED, 0xf, false, "");
    expect_command(&rig, 2, "select",
                   "console input: " ID " console0\n"
                   "console output: not filtered.\n");
    rig_stop(&rig);

    rig_start(&rig, NAMED ",compute-boards=14", 0xf, false, "");
    expect_command(&rig, 0, "select",
                   "console input: " ID " console1\n"
                   "console output: not filtered.\n");
    rig_stop(&rig);
}

/*
 * select names any of the twenty subchannels of a fitted board. An empty
 * board position is refused and leaves the selection; a board without
 * processors is accepted, and console mode toward it answers nothing.
 */
static void test_console_selection(void)
{
    CxRig rig;

    rig_start(&rig, NAMED, 0xb, false, "");
    expect_command(&rig, 0, "select sub console2",
                   "ERROR: bad subchanel selection.\n");
    expect_command(&rig, 0, "select sub 2A",
                   "ERROR: bad subchanel selection.\n");
    expect_command(&rig, 0, "select sub console",
                   "ERROR: bad subchanel selection.\n");
    expect_command(&rig, 0, "select",
                   "console input: " ID " console0\n"
                   "console output: not filtered.\n");
    expect_command(&rig, 1, "select sub 3d", "");
    expect_command(&rig, 1, "select",
                   "console input: " ID " 3D\n"
                   "console output: not filtered.\n");
    rig_stop(&rig);

    rig_start(&rig, NAMED ",compute-boards=11", 0xf, true, "");
    console_expect(&rig, POWERING ENTER("console0"));
    /* Returning to console mode toward board 2 finds nothing to answer. */
    console_send(&rig, "\024select sub console2\r");
    console_expect(&rig, ESCAPE NO_CONSOLE PROMPT);
    console_send(&rig, "\004");
    console_expect(&rig, NO_CONSOLE PROMPT);
    console_send(&rig, "select sub 2B\r\004");
    console_expect(&rig, PROMPT "no response from " ID " Junk bus CPU2B "
                         "system not responding\r\n" PROMPT);
    /* Still in L1 mode: a keystroke is a command byte, sent nowhere. */
    console_send(&rig, "x");
    expect_command(&rig, 0, "select",
                   "console input: " ID " 2B\n"
                   "console output: not filtered.\n");
    rig_stop(&rig);
}

/* A keystroke in console mode leaves on the selected board's window only. */
static void test_console_input(void)
{
    CxRig rig;

    rig_start(&rig, NAMED, 0xf, true, "");
    console_expect(&rig, POWERING ENTER("console0"));
    console_send(&rig, "a");
    expect_keystroke(&rig, 0, 4, 'a');
    console_send(&rig, "\024select sub 2C\r");
    console_expect(&rig, ESCAPE RETURN("2C"));
    console_send(&rig, "b");
    expect_keystroke(&rig, 2, 2, 'b');
    rig_stop(&rig);
}

/* 0x1003 from board 3's window moves the console input to console3. */
static void test_volunteer(void)
{
    CxRig rig;
    uint8_t reply[131];
    size_t length;

    rig_start(&rig, NAMED, 0xf, false, "");
    length = rpc(&rig, 3, LOCAL_GENERAL, 0x1003, NULL, 0, 0, reply);
    g_assert_cmpuint(length, ==, 6);
    expect_code(reply, length, 0);
    expect_command(&rig, 0, "select",
                   "console input: " ID " console3\n"
                   "console output: not filtered.\n");
    rig_stop(&rig);
}

/*
 * A request whose action resets the machine. The reset may empty the
 * windows before the reply is read, so the reply is checked only if it is
 * still there, and only on the asking board's window.
 */
static void send_resetting(CxRig *rig, unsigned board, uint32_t destination,
                           uint16_t code, const char *args, size_t args_len,
                           unsigned count)
{
    uint8_t request[136] = { 0x10, destination >> 24, destination >> 16,
                             destination >> 8, destination, code >> 8, code,
                             count };
    uint8_t reply[131];

    memcpy(request + 8, args, args_len);
    send_on(rig, board, request, 8 + args_len);
    qtest_qmp_eventwait(rig->qts, "RESET");
    for (unsigned i = 0; i < SGI_L1_TEST_BOARDS; i++) {
        if (i != board && (rig->boards & (1u << i))) {
            g_assert_cmphex(qtest_readq(rig->qts, link_base(i) + 0x28), ==,
                            0x60);
        }
    }
    if (qtest_readq(rig->qts, link_base(board) + 0x28) & 1) {
        expect_code(reply, receive_on(rig, board, reply), 0);
    }
}

/* How many log lines end with @entry. */
static unsigned log_count(CxRig *rig, const char *entry)
{
    g_autofree char *log = command(rig, 0, "log", NULL);
    unsigned count = 0;

    for (const char *p = strstr(log, entry); p; p = strstr(p + 1, entry)) {
        count++;
    }
    return count;
}

/*
 * Power and reset act on the whole brick, and the log gains one entry per
 * action, whichever window asked. The machine stands for the brick here.
 */
static void test_power_scope(void)
{
    CxRig rig;
    uint8_t reply[131];
    size_t length;
    int32_t code;
    g_autofree char *output = NULL;

    /* A task-3 reply holds a few log lines: start each case empty. */
    rig_start(&rig, NAMED, 0xf, false, "-no-shutdown");
    expect_command(&rig, 0, "log clear", "log reset.\n");
    output = command(&rig, 3, "power down", &code);
    g_assert_cmpint(code, ==, 0);
    qtest_qmp_eventwait(rig.qts, "SHUTDOWN");
    g_assert_cmpuint(log_count(&rig, " power down (COMMAND)\n"), ==, 1);
    send_resetting(&rig, 1, LOCAL_COMMAND, 0, "\001power up", 10, 1);
    g_assert_cmpuint(log_count(&rig, " power up (COMMAND)\n"), ==, 1);
    g_assert_cmpuint(log_count(&rig, " power down (COMMAND)\n"), ==, 1);
    rig_stop(&rig);

    rig_start(&rig, NAMED, 0xf, false, "-no-shutdown");
    expect_command(&rig, 0, "log clear", "log reset.\n");
    length = rpc(&rig, 1, LOCAL_GENERAL, 0x2005, NULL, 0, 0, reply);
    g_assert_cmpuint(length, ==, 6);
    expect_code(reply, length, 0);
    qtest_qmp_eventwait(rig.qts, "SHUTDOWN");
    g_assert_cmpuint(log_count(&rig, " power down (OS)\n"), ==, 1);
    rig_stop(&rig);

    rig_start(&rig, NAMED, 0xf, false, "");
    expect_command(&rig, 0, "log clear", "log reset.\n");
    length = rpc(&rig, 2, LOCAL_GENERAL, 0x2000, NULL, 0, 0, reply);
    expect_code(reply, length, -101);
    g_assert_cmpuint(log_count(&rig, " reset (OS)\n"), ==, 0);
    send_resetting(&rig, 2, LOCAL_GENERAL, 0x2001, NULL, 0, 0);
    g_assert_cmpuint(log_count(&rig, " reset (OS)\n"), ==, 1);
    rig_stop(&rig);
}

/* nmi pulses one line per board; the machine wires the boards' processors. */
static void test_nmi(void)
{
    CxRig rig;

    rig_start(&rig, NAMED ",compute-boards=13", 0xf, false, "");
    qtest_irq_intercept_out_named(rig.qts, L1, "nmi");
    expect_command(&rig, 0, "nmi", "");
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmpuint(qtest_get_irq_raise_count(rig.qts, i), ==, 1);
    }
    rig_stop(&rig);
}

/* The refusals at realize, each an input the profile does not take. */
static const struct {
    const char *args;
    const char *message;
} refusal_cases[] = {
    { "-device sgi-l1,brick-type=cxbrick,boards=0",
      "*boards must name boards 0 to 3*" },
    { "-device sgi-l1,brick-type=cxbrick,boards=16",
      "*boards must name boards 0 to 3*" },
    { "-device sgi-l1,brick-type=cxbrick,boards=3,compute-boards=4",
      "*compute-boards must be fitted*" },
    { "-device sgi-l1,brick-type=cxbrick,node-position=1",
      "*node-position does not apply*" },
    { "-device sgi-l1,brick-type=cxbrick,revision-major=1,"
      "revision-minor=0,revision-patch=0",
      "*firmware revision is unknown*" },
    { "-device sgi-l1,brick-type=cxbrick,irouter-compat=on",
      "*compatibility is not accepted*" },
    { "-device sgi-l1,brick-type=cxbrick,identity-index=25",
      "*identity-index must be 0 to 24*" },
    { "-device sgi-l1,brick-type=cbrick,boards=3",
      "*apply to the cxbrick profile only*" },
    { "-device sgi-l1,boards=3",
      "*apply to the cxbrick profile only*" },
    { "-device sgi-l1,id=l1c,brick-type=cxbrick,boards=5 "
      "-device sgi-l1-uart,controller=l1c,transport-index=1",
      "*names no fitted Cx-brick board*" },
};

static void test_refusal(gconstpointer opaque)
{
    unsigned index = GPOINTER_TO_UINT(opaque);

    if (g_test_subprocess()) {
        QTestState *s = qtest_initf("-M none -cpu R10000 -S %s",
                                    refusal_cases[index].args);

        qtest_quit(s);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr(refusal_cases[index].message);
}

/* A Cx-brick takes no peer link. */
static void test_no_peer(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -S "
                               "-device sgi-l1,id=a,brick-type=cxbrick "
                               "-device sgi-l1,id=b,brick-type=cxbrick");
    QDict *response = qtest_qmp(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/a',"
        "'property':'peer','value':'/machine/peripheral/b'}}");

    g_assert_true(qdict_haskey(response, "error"));
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(response, "error"),
                                          "desc"),
                            "models no peer link"));
    qobject_unref(response);
    qtest_quit(s);
}

/*
 * The console selection, the filter and each window's port field cross a
 * migration; a destination whose boards differ refuses the stream.
 */
static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sgi-l1-cxbrick-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    CxRig from, to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    rig_start(&from, NAMED, 0xf, false, "");
    rig_start(&to, NAMED, 0xf, false, "-incoming defer");
    expect_command(&from, 0, "select sub 2B", "");
    expect_command(&from, 0, "select filter on", "");
    select_subchannel(&from, 1, 0x40);
    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from.qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration(from.qts, "completed");
    sgi_wait_migration(to.qts, "completed");
    expect_command(&to, 3, "select",
                   "console input: " ID " 2B\n"
                   "console output: filtered.\n");
    g_assert_cmpuint(position(&to, 1), ==, 3);
    g_assert_cmpuint(position(&to, 2), ==, 2);
    rig_stop(&from);
    rig_stop(&to);
    unlink(socket);

    rig_start(&from, NAMED, 0xf, false, "");
    rig_start(&to, NAMED, 0x7, false, "-incoming defer");
    qtest_set_expected_status(to.qts, EXIT_FAILURE);
    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from.qts,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    qtest_wait_qemu(to.qts);
    rig_stop(&to);
    rig_stop(&from);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sgi-l1-cxbrick/identity", test_identity);
    qtest_add_func("sgi-l1-cxbrick/board-serials", test_board_serials);
    qtest_add_func("sgi-l1-cxbrick/io-peer", test_io_peer);
    qtest_add_func("sgi-l1-cxbrick/reply-routing", test_reply_routing);
    qtest_add_func("sgi-l1-cxbrick/positions", test_positions);
    qtest_add_func("sgi-l1-cxbrick/request-grades", test_request_grades);
    qtest_add_func("sgi-l1-cxbrick/board-records", test_board_records);
    qtest_add_func("sgi-l1-cxbrick/console-filter", test_console_filter);
    qtest_add_func("sgi-l1-cxbrick/console-default", test_console_default);
    qtest_add_func("sgi-l1-cxbrick/console-selection",
                   test_console_selection);
    qtest_add_func("sgi-l1-cxbrick/console-input", test_console_input);
    qtest_add_func("sgi-l1-cxbrick/volunteer", test_volunteer);
    qtest_add_func("sgi-l1-cxbrick/power-scope", test_power_scope);
    qtest_add_func("sgi-l1-cxbrick/nmi", test_nmi);
    for (unsigned i = 0; i < ARRAY_SIZE(refusal_cases); i++) {
        g_autofree char *name =
            g_strdup_printf("sgi-l1-cxbrick/refusal/%u", i);

        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_refusal);
    }
    qtest_add_func("sgi-l1-cxbrick/no-peer", test_no_peer);
    qtest_add_func("sgi-l1-cxbrick/migration", test_migration);
    return g_test_run();
}
