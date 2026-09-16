/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Isolated L1 protocol checks; this fixture does not execute firmware. */
#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include <poll.h>
#include <sys/socket.h>

#define BASE 0x11000000ULL

static void expect_router_error(QTestState *s, uint8_t subchannel);
static void expect_raw_reply(QTestState *s, const uint8_t *expected,
                             size_t length);
static size_t general(QTestState *s, uint16_t code, const uint8_t *args,
                      size_t args_len, unsigned count, uint8_t *reply);
static void integer(uint8_t *out, uint32_t value);
#define PATH "/machine/peripheral/l1"
#define L1 "/machine/peripheral/l1c"
#define UART " -device sgi-l1-uart,id=l1,controller=l1c"

static QTestState *start(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -device sgi-l1,id=l1c"
                               UART " -S");
    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    return s;
}

static uint16_t fcs(uint16_t v, uint8_t b)
{
    v ^= b;
    for (unsigned i = 0; i < 8; i++) {
        v = (v >> 1) ^ ((v & 1) ? 0x8408 : 0);
    }
    return v;
}

static void put(QTestState *s, uint8_t b)
{
    if (b == 0x7e || b == 0x7d) {
        qtest_writeq(s, BASE, 0x7d);
        b ^= 0x20;
    }
    qtest_writeq(s, BASE, b);
}

static void send_frame(QTestState *s, const uint8_t *p, size_t n, bool corrupt)
{
    uint16_t crc = 0xffff;
    qtest_writeq(s, BASE, 0x7e);
    for (size_t i = 0; i < n; i++) {
        crc = fcs(crc, p[i]);
        put(s, p[i]);
    }
    crc = ~crc;
    put(s, crc ^ corrupt);
    put(s, crc >> 8);
    qtest_writeq(s, BASE, 0x7e);
}

static size_t receive(QTestState *s, uint8_t *p)
{
    size_t n = 0;
    bool escape = false;
    uint16_t crc = 0xffff;
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x61);
    g_assert_cmphex(qtest_readq(s, BASE), ==, 0x7e);
    for (;;) {
        uint8_t b;
        g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x61);
        b = qtest_readq(s, BASE);
        if (b == 0x7e) {
            break;
        }
        if (b == 0x7d && !escape) {
            escape = true;
            continue;
        }
        if (escape) {
            b ^= 0x20;
            escape = false;
        }
        g_assert_cmpuint(n, <, 131);
        p[n++] = b;
        crc = fcs(crc, b);
    }
    g_assert_false(escape);
    g_assert_cmpuint(n, >=, 3);
    g_assert_cmphex(crc, ==, 0xf0b8);
    return n - 2;
}

static void test_uart(void)
{
    QTestState *s = start();
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    g_assert_cmphex(qtest_readq(s, BASE + 0x30), ==, 0x10);
    qtest_writeq(s, BASE + 0x18, 0x80);
    qtest_writeq(s, BASE, 13);
    qtest_writeq(s, BASE + 8, 0x42);
    g_assert_cmphex(qtest_readq(s, BASE), ==, 13);
    g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 0x42);
    qtest_writeq(s, BASE + 0x18, 3);
    g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, 3);
    qtest_writeb(s, BASE + 0x1f, 0x80);
    g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, 3);
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1);
    qtest_quit(s);
}

static void test_frames(void)
{
    QTestState *s = start();
    const uint8_t display[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 5,
                                1, 1, 'a', 0x7e, 0x7d, 0 };
    const uint8_t success[] = { 0x30, 0, 0, 0, 0, 0 };
    const uint8_t wire[] = { 0x7e, 0x30, 0, 0, 0, 0, 0, 0x5f, 0x30, 0x7e };
    uint8_t reply[131];
    g_autoptr(QDict) result = NULL;
    send_frame(s, display, sizeof(display), true);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    send_frame(s, display, sizeof(display), false);
    for (unsigned i = 0; i < sizeof(wire); i++) {
        g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x61);
        g_assert_cmphex(qtest_readq(s, BASE), ==, wire[i]);
    }
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    result = qtest_qmp(s, "{'execute':'qom-get','arguments':"
                         "{'path':%s,'property':'display-line-2'}}", L1);
    g_assert_cmpstr(qdict_get_str(result, "return"), ==, "a~}");
    qtest_writeq(s, BASE, 0x7e);
    for (unsigned i = 0; i < 150; i++) {
        qtest_writeq(s, BASE, 0x33);
    }
    qtest_writeq(s, BASE, 0x7e);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    qtest_writeq(s, BASE, 0x7e);
    qtest_writeq(s, BASE, 0x7d);
    qtest_writeq(s, BASE, 0x7e);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    send_frame(s, display, sizeof(display), false);
    send_frame(s, display, sizeof(display), false);
    g_assert_cmpuint(receive(s, reply), ==, sizeof(success));
    g_assert_cmpuint(receive(s, reply), ==, sizeof(success));
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    qtest_quit(s);
}

static void test_output_escape_startup(void)
{
    QTestState *s = qtest_init(
        "-M none -cpu R10000 -S -device '{\"driver\":\"sgi-l1\","
        "\"id\":\"l1c\",\"brick-serial\":\"A\\r\\u0011\\u0013B\"}' "
        "-device '{\"driver\":\"sgi-l1-uart\",\"id\":\"l1\","
        "\"controller\":\"l1c\"}'");
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 4, 0 };
    const uint8_t wire[] = {
        0x7e, 0x30, 0, 0, 0, 0, 1, 1, 'A',
        0x7d, 0x2d, 0x11, 0x13, 'B', 0, 0x42, 0x84, 0x7e
    };

    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    send_frame(s, request, sizeof(request), false);
    expect_raw_reply(s, wire, sizeof(wire));
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    qtest_system_reset(s);
    send_frame(s, request, sizeof(request), false);
    expect_raw_reply(s, wire, sizeof(wire));
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    qtest_quit(s);
}

static void test_identity(void)
{
    QTestState *s = start();
    uint8_t request[] = { 0x11, 0x0f, 0xff, 0xf0, 6, 0, 3, 0 };
    uint8_t expected[] = { 0x31, 0, 0, 0, 0, 2, 0, 0, 0, 0, 65,
                           0, 0, 0, 0, '4' };
    uint8_t reply[131];
    for (unsigned i = 0; i < 2; i++) {
        send_frame(s, request, sizeof(request) - i, false);
        g_assert_cmpuint(receive(s, reply), ==, sizeof(expected));
        g_assert_cmpmem(reply, sizeof(expected), expected, sizeof(expected));
    }
    request[1] = 0x4f;
    send_frame(s, request, sizeof(request), false);
    expect_router_error(s, 17);
    qtest_quit(s);
}

static void test_errors(void)
{
    QTestState *s = start();
    uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0xab, 0xcd, 0 };
    uint8_t reply[131];
    const uint8_t unknown[] = { 0x30, 0xff, 0xff, 0xff, 0x9b, 0 };
    const uint8_t count[] = { 0x30, 0xff, 0xff, 0xff, 0x9c, 0 };
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, sizeof(unknown));
    g_assert_cmpmem(reply, sizeof(unknown), unknown, sizeof(unknown));
    request[5] = 0x10;
    request[6] = 5;
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, sizeof(count));
    g_assert_cmpmem(reply, sizeof(count), count, sizeof(count));
    qtest_quit(s);
}

static void test_reset(void)
{
    QTestState *s = start();
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 3 };
    qtest_writeq(s, BASE + 0x18, 3);
    send_frame(s, request, sizeof(request), false);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x61);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, 0);
    send_frame(s, request, sizeof(request), false);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x61);
    qtest_quit(s);
}

static QTestState *start_files(const char *spd, const char *board,
                               const char *io8, const char *chassis,
                               const char *extra)
{
    QTestState *s = qtest_initf(
        "-M none -cpu R10000 -S %s -device '{\"driver\":\"sgi-l1\","
        "\"id\":\"l1c\",\"spd-eeprom\":[\"%s\"],"
        "\"board-eeprom\":[\"%s\",\"%s\"],"
        "\"chassis-eeprom\":[\"%s\"]}' "
        /* JSON devices are created after key=value ones; keep the order. */
        "-device '{\"driver\":\"sgi-l1-uart\",\"id\":\"l1\","
        "\"controller\":\"l1c\"}'",
        extra, spd, board, io8, chassis);
    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    return s;
}

static QTestState *start_selected(const char *board, const char *spd,
                                  const char *extra)
{
    QTestState *s = qtest_initf(
        "-M none -cpu R10000 -S %s -device '{\"driver\":\"sgi-l1\","
        "\"id\":\"l1c\",\"board-record-eeprom\":[\"%s\"],"
        "\"spd-record-eeprom\":[\"\",\"\",\"%s\"]}' "
        "-device '{\"driver\":\"sgi-l1-uart\",\"id\":\"l1\","
        "\"controller\":\"l1c\"}'",
        extra, board, spd);

    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    return s;
}

static size_t eeprom_format(QTestState *s, uint32_t component,
                            uint8_t *reply)
{
    uint8_t arg[5];

    integer(arg, component);
    return general(s, 7, arg, sizeof(arg), 1, reply);
}

static void expect_format(QTestState *s, uint32_t component,
                          const uint32_t *values, unsigned count)
{
    uint8_t expected[51] = { 0x30, 0, 0, 0, 0 };
    uint8_t reply[131];

    expected[5] = count;
    for (unsigned i = 0; i < count; i++) {
        integer(expected + 6 + 5 * i, values[i]);
    }
    g_assert_cmpuint(eeprom_format(s, component, reply), ==, 6 + 5 * count);
    g_assert_cmpmem(reply, 6 + 5 * count, expected, 6 + 5 * count);
}

static size_t eeprom_read(QTestState *s, unsigned component, unsigned area,
                          unsigned offset, unsigned length, uint8_t *reply)
{
    uint8_t request[28] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 6, 4 };
    unsigned values[] = { component, area, offset, length };
    for (unsigned i = 0; i < 4; i++) {
        unsigned v = values[i];
        request[9 + 5 * i] = v >> 24;
        request[10 + 5 * i] = v >> 16;
        request[11 + 5 * i] = v >> 8;
        request[12 + 5 * i] = v;
    }
    send_frame(s, request, sizeof(request), false);
    return receive(s, reply);
}

static void test_eeprom(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-eeprom-XXXXXX", &error);
    g_autofree char *spd_path = NULL;
    g_autofree char *board_path = NULL;
    uint8_t spd[128] = { 0 }, board[17], reply[129];
    QTestState *s;
    g_assert_no_error(error);
    spd_path = g_build_filename(dir, "spd.bin", NULL);
    board_path = g_build_filename(dir, "board.bin", NULL);
    spd[0] = 64;
    spd[63] = 64;
    memset(spd + 64, 0x37, 64);
    memset(board, 0x83, sizeof(board) - 1);
    board[sizeof(board) - 1] = 0xd0;
    g_assert_true(g_file_set_contents(spd_path, (char *)spd,
                                      sizeof(spd), &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(board_path, (char *)board,
                                      sizeof(board), &error));
    g_assert_no_error(error);
    s = start_files(spd_path, board_path, "", "", "");
    {
        const uint8_t unavailable[] = { 0x30, 0xff, 0xff, 0xff, 0x98, 0 };

        g_assert_cmpuint(eeprom_format(s, 0, reply), ==,
                         sizeof(unavailable));
        g_assert_cmpmem(reply, sizeof(unavailable), unavailable,
                        sizeof(unavailable));
    }
    g_assert_cmpuint(eeprom_read(s, 0, 4, 0, 0, reply), ==, 11);
    g_assert_cmphex(reply[10], ==, 128);
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmpuint(eeprom_read(s, 0, 4, 64 * i, 64, reply), ==, 76);
        g_assert_cmphex(reply[10], ==, 64 * (1 - i));
        g_assert_cmphex(reply[11], ==, 0xc0);
        g_assert_cmpmem(reply + 12, 64, spd + 64 * i, 64);
    }
    /* 116 bytes fill the 128-byte frame; 117 answers the declared minus 105. */
    g_assert_cmpuint(eeprom_read(s, 0, 4, 0, 116, reply), ==, 128);
    g_assert_cmpmem(reply + 12, 116, spd, 116);
    g_assert_cmpuint(eeprom_read(s, 0, 4, 0, 117, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x97);
    g_assert_cmpuint(eeprom_read(s, 0, 4, 28, 127, reply), ==, 112);
    g_assert_cmpmem(reply + 12, 100, spd + 28, 100);
    g_assert_cmpuint(eeprom_read(s, 0, 2, 0, 64, reply), ==, 29);
    g_assert_cmpmem(reply + 12, sizeof(board), board, sizeof(board));
    g_assert_cmpuint(eeprom_read(s, 1, 4, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x98);
    g_assert_cmpuint(eeprom_read(s, 0, 4, 129, 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x97);
    /*
     * Component 6 is beyond the table; an absent component answers minus
     * 104 before any range check; area 5 on a present one is minus 105.
     */
    g_assert_cmpuint(eeprom_read(s, 6, 4, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x98);
    g_assert_cmpuint(eeprom_read(s, 1, 5, 200, 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x98);
    g_assert_cmpuint(eeprom_read(s, 0, 5, 0, 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x97);
    /* An offset at or past the area is out of range. */
    g_assert_cmpuint(eeprom_read(s, 0, 4, 128, 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x97);
    {
        /* Five arguments are served as four; three answer minus 100. */
        uint8_t five[33] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 6, 5 };
        uint8_t three[23] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 6, 3 };

        five[12] = 0;
        five[17] = 4;
        five[27] = 64;
        five[32] = 9;
        send_frame(s, five, sizeof(five), false);
        g_assert_cmpuint(receive(s, reply), ==, 76);
        three[12] = 0;
        three[17] = 4;
        send_frame(s, three, sizeof(three), false);
        g_assert_cmpuint(receive(s, reply), ==, 6);
        g_assert_cmphex(reply[4], ==, 0x9c);
    }
    qtest_quit(s);
    spd[63]++;
    g_assert_true(g_file_set_contents(spd_path, (char *)spd,
                                      sizeof(spd), &error));
    g_assert_no_error(error);
    s = start_files(spd_path, board_path, "", "", "");
    g_assert_cmpuint(eeprom_read(s, 0, 4, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x95);
    qtest_quit(s);
    spd[63] = 192;
    g_assert_true(g_file_set_contents(spd_path, (char *)spd,
                                      sizeof(spd), &error));
    g_assert_no_error(error);
    s = start_files(spd_path, board_path, "", "", "");
    g_assert_cmpuint(eeprom_read(s, 0, 4, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x95);
    qtest_quit(s);
    unlink(spd_path);
    unlink(board_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_eeprom_six_entries(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-six-records-XXXXXX",
                                        &error);
    g_autofree char *spd_path = NULL;
    uint8_t spd[64] = { 0 };
    uint8_t reply[131];
    QTestState *s;

    g_assert_no_error(error);
    spd_path = g_build_filename(dir, "spd.bin", NULL);
    g_assert_true(g_file_set_contents(spd_path, (char *)spd,
                                      sizeof(spd), &error));
    g_assert_no_error(error);
    s = qtest_initf(
        "-M none -cpu R10000 -S -device '{\"driver\":\"sgi-l1\","
        "\"id\":\"l1c\",\"spd-eeprom\":[\"\",\"\",\"\",\"\",\"\",\"\",\"%s\"]}' "
        "-device '{\"driver\":\"sgi-l1-uart\",\"id\":\"l1\","
        "\"controller\":\"l1c\"}'",
        spd_path);
    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    g_assert_cmpuint(eeprom_read(s, 6, 4, 0, 64, reply), ==, 76);
    g_assert_cmpmem(reply + 12, 64, spd, sizeof(spd));
    g_assert_cmpuint(eeprom_read(s, 6, 5, 200, 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x97);
    qtest_quit(s);
    unlink(spd_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_board_area_validity(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-board-area-XXXXXX",
                                        &error);
    g_autofree char *node_path = NULL, *io8_path = NULL;
    g_autofree char *chassis_path = NULL;
    uint8_t node[] = { 0x10, 0xf0 };
    uint8_t io8[] = { 0x23, 0xdd };
    uint8_t chassis[] = { 0x46, 0xba };
    uint8_t reply[131];
    QTestState *s;

    g_assert_no_error(error);
    node_path = g_build_filename(dir, "node.bin", NULL);
    io8_path = g_build_filename(dir, "io8.bin", NULL);
    chassis_path = g_build_filename(dir, "chassis.bin", NULL);
    g_assert_true(g_file_set_contents(node_path, (char *)node,
                                      sizeof(node), &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(io8_path, (char *)io8,
                                      sizeof(io8), &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(chassis_path, (char *)chassis,
                                      sizeof(chassis), &error));
    g_assert_no_error(error);

    s = start_files("", node_path, io8_path, chassis_path, "");
    g_assert_cmpuint(eeprom_read(s, 0, 2, 0, 2, reply), ==, 14);
    g_assert_cmpmem(reply + 12, 2, node, 2);
    g_assert_cmpuint(eeprom_read(s, 1, 2, 0, 2, reply), ==, 14);
    g_assert_cmpmem(reply + 12, 2, io8, 2);
    g_assert_cmpuint(eeprom_read(s, 0, 1, 0, 2, reply), ==, 14);
    g_assert_cmpmem(reply + 12, 2, chassis, 2);
    qtest_quit(s);

    node[1] = 0xf1;
    chassis[1] = 0xbb;
    g_assert_true(g_file_set_contents(node_path, (char *)node,
                                      sizeof(node), &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(chassis_path, (char *)chassis,
                                      sizeof(chassis), &error));
    g_assert_no_error(error);
    s = start_files("", node_path, io8_path, chassis_path, "");
    g_assert_cmpuint(eeprom_read(s, 0, 2, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x95);
    g_assert_cmpuint(eeprom_read(s, 0, 2, 2, 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x95);
    g_assert_cmpuint(eeprom_read(s, 0, 1, 2, 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x95);
    g_assert_cmpuint(eeprom_read(s, 1, 2, 0, 2, reply), ==, 14);
    g_assert_cmpmem(reply + 12, 2, io8, 2);
    qtest_quit(s);

    unlink(node_path);
    unlink(io8_path);
    unlink(chassis_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_selected_board(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-selected-board-XXXXXX",
                                        &error);
    g_autofree char *path = NULL;
    uint8_t image[64] = { 0 }, reply[131];
    const uint8_t area[] = { 1, 1, 0, 0, 0, 0, 0, 0xfe };
    const uint8_t iua_reply[] = {
        0x30, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0x88,
        1, 1, 0, 0, 0, 0, 0, 0xfe
    };
    const uint32_t all_ok[] = { 1, 8, 8, 8, 8, 0, 0, 0, 0 };
    const uint32_t rounded[] = { 1, 0, 0, 16, 8, 1, 1, 0, 0 };
    const uint32_t equal_capacity[] = { 1, 0, 0, 0, 8, 1, 1, 1, 0 };
    const uint32_t bad_iua[] = { 1, 0, 0, 0, 8, 1, 1, 3, 0 };
    const uint8_t invalid[] = { 0x30, 0xff, 0xff, 0xff, 0x95, 0 };
    const uint8_t too_high[] = { 0x30, 0xff, 0xff, 0xff, 0x97, 0 };
    QTestState *s;

    g_assert_no_error(error);
    path = g_build_filename(dir, "board.bin", NULL);
    memcpy(image, (uint8_t[]){ 1, 2, 3, 4, 0, 0, 0, 0xf6 }, 8);
    memcpy(image + 16, area, 8);
    memcpy(image + 24, area, 8);
    memcpy(image + 32, area, 8);
    g_assert_true(g_file_set_contents(path, (char *)image,
                                      sizeof(image), &error));
    g_assert_no_error(error);
    s = start_selected(path, "", "");
    expect_format(s, 0, all_ok, G_N_ELEMENTS(all_ok));
    {
        const uint8_t missing[] = { 0x30, 0xff, 0xff, 0xff, 0x9c, 0 };
        const uint8_t absent[] = { 0x30, 0xff, 0xff, 0xff, 0x98, 0 };

        g_assert_cmpuint(general(s, 7, NULL, 0, 0, reply), ==,
                         sizeof(missing));
        g_assert_cmpmem(reply, sizeof(missing), missing, sizeof(missing));
        g_assert_cmpuint(eeprom_format(s, 6, reply), ==, sizeof(absent));
        g_assert_cmpmem(reply, sizeof(absent), absent, sizeof(absent));
    }
    g_assert_cmpuint(eeprom_read(s, 0, 3, 0, 8, reply), ==,
                     sizeof(iua_reply));
    g_assert_cmpmem(reply, sizeof(iua_reply), iua_reply,
                    sizeof(iua_reply));
    for (unsigned area_index = 0; area_index < 3; area_index++) {
        unsigned area_code = area_index == 0 ? 0 : area_index == 1 ? 1 : 2;
        g_assert_cmpuint(eeprom_read(s, 0, area_code, 0, 8, reply), ==, 20);
        g_assert_cmpmem(reply + 12, 8,
                        area_code == 0 ? image : area, 8);
    }
    g_assert_cmpuint(eeprom_format(s, 256, reply), ==, sizeof(too_high));
    g_assert_cmpmem(reply, sizeof(too_high), too_high, sizeof(too_high));
    qtest_quit(s);

    memset(image, 0, sizeof(image));
    memcpy(image, (uint8_t[]){ 1, 2, 0, 0, 0, 0, 0, 0xfd }, 8);
    image[18] = 4;
    image[31] = 0xfc;
    g_assert_true(g_file_set_contents(path, (char *)image,
                                      sizeof(image), &error));
    g_assert_no_error(error);
    s = start_selected(path, "", "");
    expect_format(s, 0, rounded, G_N_ELEMENTS(rounded));
    g_assert_cmpuint(eeprom_read(s, 0, 3, 0, 16, reply), ==, 28);
    g_assert_cmpmem(reply + 12, 16, image + 16, 16);
    qtest_quit(s);

    g_assert_true(g_file_set_contents(path, (char *)image, 32, &error));
    g_assert_no_error(error);
    s = start_selected(path, "", "");
    expect_format(s, 0, equal_capacity, G_N_ELEMENTS(equal_capacity));
    g_assert_cmpuint(eeprom_read(s, 0, 3, 99, 1, reply), ==,
                     sizeof(invalid));
    g_assert_cmpmem(reply, sizeof(invalid), invalid, sizeof(invalid));
    qtest_quit(s);

    image[18] = 3;
    image[23] = 0xfe;
    g_assert_true(g_file_set_contents(path, (char *)image,
                                      sizeof(image), &error));
    g_assert_no_error(error);
    s = start_selected(path, "", "");
    expect_format(s, 0, bad_iua, G_N_ELEMENTS(bad_iua));
    g_assert_cmpuint(eeprom_read(s, 0, 3, 99, 1, reply), ==,
                     sizeof(invalid));
    g_assert_cmpmem(reply, sizeof(invalid), invalid, sizeof(invalid));
    qtest_quit(s);

    unlink(path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_selected_spd(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-selected-spd-XXXXXX",
                                        &error);
    g_autofree char *path = NULL;
    uint8_t spd[160] = { 0 }, reply[131];
    const uint8_t valid[] = {
        0x30, 0, 0, 0, 0, 3, 0, 0, 0, 0, 2,
        0, 0, 0, 0, 0xa0, 0, 0, 0, 0, 0
    };
    const uint8_t failed[] = {
        0x30, 0, 0, 0, 0, 3, 0, 0, 0, 0, 2,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 3
    };
    const uint8_t invalid[] = { 0x30, 0xff, 0xff, 0xff, 0x95, 0 };
    const uint8_t dropped[] = { 0x30, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0x2b };
    const uint8_t probe[] = { 0x30, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0xa0 };
    const uint8_t first_chunk[] = {
        0x30, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0x60, 0xc0
    };
    const uint8_t second_chunk[] = {
        0x30, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0x20, 0xc0
    };
    const uint8_t final_chunk[] = {
        0x30, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0xa0
    };
    QTestState *s;

    g_assert_no_error(error);
    path = g_build_filename(dir, "spd.bin", NULL);
    spd[0] = 0x40;
    spd[63] = 0x40;
    memset(spd + 64, 0x5a, 96);
    g_assert_true(g_file_set_contents(path, (char *)spd,
                                      sizeof(spd), &error));
    g_assert_no_error(error);
    s = start_selected("", path, "");
    g_assert_cmpuint(eeprom_format(s, 2, reply), ==, sizeof(valid));
    g_assert_cmpmem(reply, sizeof(valid), valid, sizeof(valid));
    g_assert_cmpuint(eeprom_read(s, 2, 4, 0, 0, reply), ==,
                     sizeof(probe));
    g_assert_cmpmem(reply, sizeof(probe), probe, sizeof(probe));
    g_assert_cmpuint(eeprom_read(s, 2, 4, 0, 64, reply), ==, 76);
    g_assert_cmpmem(reply, sizeof(first_chunk), first_chunk,
                    sizeof(first_chunk));
    g_assert_cmphex(reply[12], ==, 0x40);
    for (unsigned i = 13; i < 75; i++) {
        g_assert_cmphex(reply[i], ==, 0);
    }
    g_assert_cmphex(reply[75], ==, 0x40);
    g_assert_cmpuint(eeprom_read(s, 2, 4, 64, 64, reply), ==, 76);
    g_assert_cmpmem(reply, sizeof(second_chunk), second_chunk,
                    sizeof(second_chunk));
    for (unsigned i = 12; i < 76; i++) {
        g_assert_cmphex(reply[i], ==, 0x5a);
    }
    g_assert_cmpuint(eeprom_read(s, 2, 4, 128, 64, reply), ==, 44);
    g_assert_cmpmem(reply, sizeof(final_chunk), final_chunk,
                    sizeof(final_chunk));
    for (unsigned i = 12; i < 44; i++) {
        g_assert_cmphex(reply[i], ==, 0x5a);
    }
    g_assert_cmpuint(eeprom_read(s, 2, 4, 0, 117, reply), ==,
                     sizeof(dropped));
    g_assert_cmpmem(reply, sizeof(dropped), dropped, sizeof(dropped));
    qtest_quit(s);

    spd[63] = 0x41;
    g_assert_true(g_file_set_contents(path, (char *)spd,
                                      sizeof(spd), &error));
    g_assert_no_error(error);
    s = start_selected("", path, "");
    g_assert_cmpuint(eeprom_format(s, 2, reply), ==, sizeof(failed));
    g_assert_cmpmem(reply, sizeof(failed), failed, sizeof(failed));
    g_assert_cmpuint(eeprom_read(s, 2, 4, 200, 0, reply), ==,
                     sizeof(invalid));
    g_assert_cmpmem(reply, sizeof(invalid), invalid, sizeof(invalid));
    qtest_quit(s);
    unlink(path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_selected_raw_conflict(void)
{
    const char *path = g_getenv("L1_SELECTED_CONFLICT_PATH");
    const char *kind = g_getenv("L1_SELECTED_CONFLICT_KIND");

    if (g_test_subprocess()) {
        if (!strcmp(kind, "board")) {
            qtest_initf(
                "-M none -cpu R10000 -S -device '{\"driver\":\"sgi-l1\","
                "\"id\":\"l1c\",\"board-record-eeprom\":[\"%s\"],"
                "\"board-eeprom\":[\"%s\"]}'" , path, path);
        } else {
            qtest_initf(
                "-M none -cpu R10000 -S -device '{\"driver\":\"sgi-l1\","
                "\"id\":\"l1c\",\"spd-record-eeprom\":[\"\","
                "\"\",\"%s\"],\"spd-eeprom\":[\"\",\"\",\"%s\"]}'",
                path, path);
        }
        return;
    }
    {
        GError *error = NULL;
        g_autofree char *dir = g_dir_make_tmp("sn1-l1-mixed-files-XXXXXX",
                                            &error);
        g_autofree char *board_path = NULL, *spd_path = NULL;
        uint8_t board[64] = { 1, 0, 0, 0, 0, 0, 0, 0xff };
        uint8_t spd[160] = { 0 };

        g_assert_no_error(error);
        board_path = g_build_filename(dir, "board.bin", NULL);
        spd_path = g_build_filename(dir, "spd.bin", NULL);
        g_assert_true(g_file_set_contents(board_path, (char *)board,
                                          sizeof(board), &error));
        g_assert_no_error(error);
        g_assert_true(g_file_set_contents(spd_path, (char *)spd,
                                          sizeof(spd), &error));
        g_assert_no_error(error);
        g_setenv("L1_SELECTED_CONFLICT_PATH", board_path, TRUE);
        g_setenv("L1_SELECTED_CONFLICT_KIND", "board", TRUE);
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*conflicts with raw area file*");
        g_setenv("L1_SELECTED_CONFLICT_PATH", spd_path, TRUE);
        g_setenv("L1_SELECTED_CONFLICT_KIND", "spd", TRUE);
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*conflicts with raw area file*");
        g_unsetenv("L1_SELECTED_CONFLICT_PATH");
        g_unsetenv("L1_SELECTED_CONFLICT_KIND");
        unlink(board_path);
        unlink(spd_path);
        g_assert_cmpint(rmdir(dir), ==, 0);
    }
}

static void wait_migration(QTestState *s)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    for (;;) {
        g_autoptr(QDict) reply = qtest_qmp(s, "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(qdict_get_qdict(reply, "return"),
                                          "status");
        if (!strcmp(status, "completed")) {
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-migrate-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from = start(), *to;
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 3 };
    uint8_t reply[131];
    uint16_t crc = 0xffff;
    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    to = qtest_init("-M none -cpu R10000 -device sgi-l1,id=l1c" UART
                    " -S -incoming defer");
    qtest_sysbus_mmio_map(to, PATH, 0, BASE);
    send_frame(from, request, sizeof(request), false);
    qtest_writeq(from, BASE, 0x7e);
    for (unsigned i = 0; i < 4; i++) {
        crc = fcs(crc, request[i]);
        put(from, request[i]);
    }
    qtest_writeq(from, BASE + 0x18, 3);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(from);
    /* The source completes when its state is sent, not when it is loaded. */
    wait_migration(to);
    g_assert_cmphex(qtest_readq(to, BASE + 0x18), ==, 3);
    g_assert_cmpuint(receive(to, reply), ==, 16);
    for (unsigned i = 4; i < sizeof(request); i++) {
        crc = fcs(crc, request[i]);
        put(to, request[i]);
    }
    crc = ~crc;
    put(to, crc);
    put(to, crc >> 8);
    qtest_writeq(to, BASE, 0x7e);
    g_assert_cmpuint(receive(to, reply), ==, 16);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_eeprom_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-record-migrate-XXXXXX",
                                         &error);
    g_autofree char *socket = NULL, *uri = NULL;
    g_autofree char *source = NULL, *destination = NULL;
    QTestState *from, *to;
    uint8_t reply[131];
    const uint8_t source_bytes[] = { 0xa5, 0x7e, 0xdd };
    const uint8_t destination_bytes[] = { 0x33, 0xce };
    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    source = g_build_filename(dir, "source.bin", NULL);
    destination = g_build_filename(dir, "destination.bin", NULL);
    g_assert_true(g_file_set_contents(source, (char *)source_bytes,
                                      sizeof(source_bytes), &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(destination, (char *)destination_bytes,
                                      sizeof(destination_bytes), &error));
    g_assert_no_error(error);
    from = start_files("", source, "", "", "");
    to = start_files("", destination, "", "", "-incoming defer");
    g_assert_cmpuint(eeprom_read(to, 0, 2, 0, 64, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x95);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(from);
    wait_migration(to);
    g_assert_cmpuint(eeprom_read(to, 0, 2, 0, 64, reply), ==, 15);
    g_assert_cmpmem(reply + 12, 3, source_bytes, 3);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    unlink(source);
    unlink(destination);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_selected_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-selected-migrate-XXXXXX",
                                        &error);
    g_autofree char *socket = NULL, *uri = NULL;
    g_autofree char *from_board = NULL, *to_board = NULL;
    g_autofree char *from_spd = NULL, *to_spd = NULL;
    uint8_t board[64] = { 0 }, spd[160] = { 0 }, reply[131];
    const uint32_t board_ok[] = { 1, 0, 0, 8, 8, 1, 1, 0, 0 };
    const uint32_t board_bad[] = { 1, 0, 0, 0, 8, 1, 1, 1, 3 };
    const uint8_t spd_failed[] = {
        0x30, 0, 0, 0, 0, 3, 0, 0, 0, 0, 2,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 3
    };
    const uint8_t iua[] = { 1, 1, 0, 0, 0, 0, 0, 0xfe };
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from_board = g_build_filename(dir, "from-board.bin", NULL);
    to_board = g_build_filename(dir, "to-board.bin", NULL);
    from_spd = g_build_filename(dir, "from-spd.bin", NULL);
    to_spd = g_build_filename(dir, "to-spd.bin", NULL);
    memcpy(board, (uint8_t[]){ 1, 2, 0, 0, 0, 0, 0, 0xfd }, 8);
    memcpy(board + 16,
           (uint8_t[]){ 1, 1, 0, 0, 0, 0, 0, 0xfe }, 8);
    g_assert_true(g_file_set_contents(from_board, (char *)board,
                                      sizeof(board), &error));
    g_assert_no_error(error);
    board[7]++;
    g_assert_true(g_file_set_contents(to_board, (char *)board,
                                      sizeof(board), &error));
    g_assert_no_error(error);
    spd[63] = 1;
    g_assert_true(g_file_set_contents(from_spd, (char *)spd,
                                      sizeof(spd), &error));
    g_assert_no_error(error);
    spd[63] = 0;
    g_assert_true(g_file_set_contents(to_spd, (char *)spd,
                                      sizeof(spd), &error));
    g_assert_no_error(error);

    from = start_selected(from_board, from_spd, "");
    to = start_selected(to_board, to_spd, "-incoming defer");
    expect_format(to, 0, board_bad, G_N_ELEMENTS(board_bad));
    expect_format(from, 0, board_ok, G_N_ELEMENTS(board_ok));
    g_assert_cmpuint(eeprom_format(from, 2, reply), ==,
                     sizeof(spd_failed));
    g_assert_cmpmem(reply, sizeof(spd_failed), spd_failed,
                    sizeof(spd_failed));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(from);
    wait_migration(to);
    expect_format(to, 0, board_ok, G_N_ELEMENTS(board_ok));
    g_assert_cmpuint(eeprom_read(to, 0, 3, 0, 8, reply), ==, 20);
    g_assert_cmpmem(reply + 12, 8, iua, sizeof(iua));
    g_assert_cmpuint(eeprom_format(to, 2, reply), ==,
                     sizeof(spd_failed));
    g_assert_cmpmem(reply, sizeof(spd_failed), spd_failed,
                    sizeof(spd_failed));
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    unlink(from_board);
    unlink(to_board);
    unlink(from_spd);
    unlink(to_spd);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_metadata(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -S "
                              "-device sgi-l1,id=l1c,revision-major=1,"
                              "revision-minor=18,revision-patch=5" UART);
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 5 };
    const uint8_t expected[] = { 0x30, 0, 0, 0, 0, 3,
                                 0, 0, 0, 0, 1,
                                 0, 0, 0, 0, 18,
                                 0, 0, 0, 0, 5 };
    uint8_t reply[131];
    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, sizeof(expected));
    g_assert_cmpmem(reply, sizeof(expected), expected, sizeof(expected));
    qtest_quit(s);
    s = start();
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x98);
    qtest_quit(s);
}

static void console_expect(int fd, const char *expected)
{
    g_autofree char *actual = g_malloc(strlen(expected) + 1);
    size_t length = strlen(expected), offset = 0;
    while (offset < length) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        ssize_t n;
        g_assert_cmpint(poll(&pfd, 1, 3000), ==, 1);
        n = read(fd, actual + offset, length - offset);
        g_assert_cmpint(n, >, 0);
        offset += n;
    }
    actual[length] = 0;
    g_assert_cmpstr(actual, ==, expected);
}

static void console_send(int fd, const char *text)
{
    g_assert_cmpint(write(fd, text, strlen(text)), ==, strlen(text));
}

static void assert_display(QTestState *s, const char *expected);
static uint32_t node_position(QTestState *s, uint32_t address);

#define ID "001c01"
#define PROMPT ID "-L1> "
#define ENTER_CPU0 \
    "entering console mode  " ID " CPU0, <CTRL_T> to escape to L1\r\n"
#define RETURN_CPU0 \
    "returning to console mode  " ID " CPU0, <CTRL_T> to escape to L1\r\n"
#define ESCAPE "escaping to L1 system controller\r\n" PROMPT
#define POWERING "INFO: " ID " powering up the system.\r\n" PROMPT "\r\n"
#define ENGAGED "L1 command processor engaged, <CTRL-D> to exit.\r\n" PROMPT
#define OFF_WARNING "WARNING: power appears off, console unavailable\r\n"
#define STAMP "09/07/26 00:00:00 "
#define RTC "-rtc base=2026-09-07T00:00:00,clock=vm"

static QTestState *console_start(int *fd, const char *props,
                                 const char *extra)
{
    int sv[2];
    QTestState *s;

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    s = qtest_initf("-M none -cpu R10000 -S %s "
                    "-chardev socket,id=console,fd=%d "
                    "-device sgi-l1,id=l1c,chardev=console%s" UART,
                    extra, sv[1], props);
    close(sv[1]);
    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    *fd = sv[0];
    return s;
}

static void console_frame(QTestState *s, uint8_t *reply, size_t expected)
{
    for (unsigned i = 0; !(qtest_readq(s, BASE + 0x28) & 1); i++) {
        g_assert_cmpuint(i, <, 1000);
        g_usleep(1000);
    }
    g_assert_cmpuint(receive(s, reply), ==, expected);
}

static const char *status(QTestState *s)
{
    static char text[32];
    g_autoptr(QDict) reply = qtest_qmp(s, "{'execute':'query-status'}");
    pstrcpy(text, sizeof(text),
            qdict_get_str(qdict_get_qdict(reply, "return"), "status"));
    return text;
}

static void wait_running(QTestState *s)
{
    for (unsigned i = 0; strcmp(status(s), "running"); i++) {
        g_assert_cmpuint(i, <, 3000);
        g_usleep(1000);
    }
    qtest_qmp_assert_success(s, "{'execute':'stop'}");
}

static void test_console(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", "");
    uint8_t reply[131];
    /* The PROM prints on its CPU's subchannel, which keeps input there. */
    const uint8_t output[] = { 0x40, 'P', 'R', 'O', 'M' };

    console_expect(fd, POWERING ENTER_CPU0);
    send_frame(s, output, sizeof(output), false);
    console_expect(fd, "PROM");
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    console_send(fd, "x");
    console_frame(s, reply, 2);
    g_assert_cmphex(reply[0], ==, 0);
    g_assert_cmphex(reply[1], ==, 'x');
    console_send(fd, "\024");
    console_expect(fd, ESCAPE);
    console_send(fd, "version\r");
    console_expect(fd, "L1 protocol responder; "
                       "firmware revision unavailable\r\n" RETURN_CPU0);
    console_send(fd, "\004");
    console_frame(s, reply, 2);
    g_assert_cmphex(reply[0], ==, 0);
    g_assert_cmphex(reply[1], ==, 4);
    console_send(fd, "\024l1\r");
    console_expect(fd, ESCAPE ENGAGED);
    console_send(fd, "bogus\r");
    console_expect(fd, "ERROR: command not found.\r\n" PROMPT);
    console_send(fd, "\004");
    console_expect(fd, ENTER_CPU0);
    console_send(fd, "\024bogus\r");
    console_expect(fd, ESCAPE "ERROR: command not found.\r\n" RETURN_CPU0);
    qtest_quit(s);
    close(fd);
}

static void test_fresh_off(void)
{
    int fd;
    QTestState *s = console_start(&fd, ",auto-power=off", RTC);

    console_send(fd, "\r");
    console_expect(fd, PROMPT);
    console_send(fd, "display\r");
    console_expect(fd, "revision: 2.0\r\n"
                       "line 1: \"" ID "     /\"\r\n"
                       "line 2: \"L1 running  \"\r\n"
                       "leds:  PWR=off   SERVICE=off     FAULT=off\r\n" PROMPT);
    console_send(fd, "power check\r");
    console_expect(fd, "power appears off\r\n" PROMPT);
    console_send(fd, "log\r");
    console_expect(fd, STAMP "L1 booting\r\n" PROMPT);
    console_send(fd, "\004");
    console_expect(fd, ENTER_CPU0);
    console_send(fd, "x");
    console_expect(fd, OFF_WARNING);
    console_send(fd, "y");
    g_usleep(20000);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    console_send(fd, "\024power up\r");
    console_expect(fd, ESCAPE RETURN_CPU0);
    qtest_qmp_eventwait(s, "RESET");
    wait_running(s);
    assert_display(s, "Powered Up");
    console_send(fd, "\024log\r");
    console_expect(fd, ESCAPE STAMP "L1 booting\r\n"
                       STAMP "power up (COMMAND)\r\n" RETURN_CPU0);
    qtest_quit(s);
    close(fd);
}

static void test_power(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", "-no-shutdown " RTC);

    console_expect(fd, POWERING ENTER_CPU0);
    assert_display(s, "Powered Up");
    console_send(fd, "\024power check\r");
    console_expect(fd, ESCAPE "power appears on\r\n" RETURN_CPU0);
    console_send(fd, "\024power up\r");
    console_expect(fd, ESCAPE "Power is already on\r\n" RETURN_CPU0);
    /* The first power-off request while up is deferred; the second acts. */
    console_send(fd, "\024power down\r");
    console_expect(fd, ESCAPE RETURN_CPU0);
    console_send(fd, "\024power down\r");
    console_expect(fd, ESCAPE PROMPT);
    qtest_qmp_eventwait(s, "SHUTDOWN");
    /* A machine that never ran stays in prelaunch; a running one stops. */
    g_assert_cmpstr(status(s), !=, "running");
    assert_display(s, "Powered Down");
    console_send(fd, "log\r");
    console_expect(fd, STAMP "L1 booting\r\n"
                       STAMP "auto power up initiated\r\n"
                       STAMP "power up (COMMAND)\r\n"
                       STAMP "power down (COMMAND)\r\n" PROMPT);
    console_send(fd, "power check\r");
    console_expect(fd, "power appears off\r\n" PROMPT);
    console_send(fd, "power up\r");
    console_expect(fd, PROMPT "\r\n" ENTER_CPU0);
    qtest_qmp_eventwait(s, "RESET");
    wait_running(s);
    assert_display(s, "Powered Up");
    qtest_quit(s);
    close(fd);
}

static void test_poweroff(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", "-no-shutdown " RTC);
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 3, 0, 0, 1,
                                1, '*', ' ', 'p', 'w', 'r', ' ', 'd', 0 };
    uint8_t reply[131];

    console_expect(fd, POWERING ENTER_CPU0);
    send_frame(s, request, sizeof(request), false);
    qtest_qmp_eventwait(s, "SHUTDOWN");
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    assert_display(s, "Powered Down");
    console_send(fd, "\024power check\r");
    console_expect(fd, ESCAPE "power appears off\r\n" PROMPT);
    console_send(fd, "log\r");
    console_expect(fd, STAMP "L1 booting\r\n"
                       STAMP "auto power up initiated\r\n"
                       STAMP "power up (COMMAND)\r\n"
                       STAMP "power down (COMMAND)\r\n" PROMPT);
    qtest_quit(s);
    close(fd);
}

static void test_node_reset(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", RTC);
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 3 };

    console_expect(fd, POWERING ENTER_CPU0);
    qtest_irq_intercept_out_named(s, L1, "node-reset");
    send_frame(s, request, sizeof(request), false);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x61);
    qtest_writeq(s, BASE + 0x18, 3);
    console_send(fd, "\024reset\r");
    console_expect(fd, ESCAPE RETURN_CPU0);
    qtest_qmp_eventwait(s, "RESET");
    g_assert_cmpuint(qtest_get_irq_raise_count(s, 0), ==, 1);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, 0);
    assert_display(s, "Reset done");
    console_send(fd, "\024log\r");
    console_expect(fd, ESCAPE STAMP "L1 booting\r\n"
                       STAMP "auto power up initiated\r\n"
                       STAMP "power up (COMMAND)\r\n"
                       STAMP "reset (COMMAND)\r\n" RETURN_CPU0);
    qtest_quit(s);
    close(fd);
}

static void test_signals_off(void)
{
    int fd;
    QTestState *s = console_start(&fd, ",auto-power=off", RTC);

    qtest_irq_intercept_out_named(s, L1, "nmi");
    console_send(fd, "nmi\r");
    console_expect(fd, PROMPT);
    console_send(fd, "softreset\r");
    console_expect(fd, PROMPT);
    console_send(fd, "reset\r");
    console_expect(fd, PROMPT);
    g_assert_cmpuint(qtest_get_irq_raise_count(s, 0), ==, 0);
    assert_display(s, "L1 running");
    console_send(fd, "log\r");
    console_expect(fd, STAMP "L1 booting\r\n" PROMPT);
    /* A console-task request with the node off gets the warning reply. */
    {
        const uint8_t console_in[] = { 0x10, 0x0f, 0xff, 0xf0, 5, 0, 0, 1,
                                       0, 0, 0, 0, 0x41 };
        const char *warning = "WARNING: power on " ID " appears off!";
        uint8_t reply[131];

        send_frame(s, console_in, sizeof(console_in), false);
        g_assert_cmpuint(receive(s, reply), ==, 7 + strlen(warning) + 1);
        g_assert_cmphex(reply[4], ==, 0xff);
        g_assert_cmphex(reply[5], ==, 1);
        g_assert_cmpstr((char *)reply + 7, ==, warning);
    }
    qtest_quit(s);
    close(fd);
}

static void test_node_power_line(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", "-no-shutdown " RTC);
    uint8_t select[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 0x02, 1,
                         0, 0, 0, 0, 4 };
    uint8_t reply[131];

    console_expect(fd, POWERING ENTER_CPU0);
    qtest_irq_intercept_out_named(s, L1, "node-power");
    send_frame(s, select, sizeof(select), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    /* The first power-off request while up is deferred; the second acts. */
    console_send(fd, "\024power down\r");
    console_expect(fd, ESCAPE "returning to console mode  " ID " console, "
                       "<CTRL_T> to escape to L1\r\n");
    console_send(fd, "\024power down\r");
    console_expect(fd, ESCAPE PROMPT);
    qtest_qmp_eventwait(s, "SHUTDOWN");
    g_assert_false(qtest_get_irq(s, 0));
    console_send(fd, "power up\r");
    /* The selection returns to CPU 0 with the power. */
    console_expect(fd, PROMPT "\r\n" ENTER_CPU0);
    qtest_qmp_eventwait(s, "RESET");
    g_assert_true(qtest_get_irq(s, 0));
    wait_running(s);
    qtest_quit(s);
    close(fd);
}

static void test_version_metadata(void)
{
    int fd;
    QTestState *s = console_start(&fd, ",revision-major=1,revision-minor=44,"
                                  "revision-patch=0", RTC);

    console_expect(fd, POWERING ENTER_CPU0);
    console_send(fd, "\024version\r");
    console_expect(fd, ESCAPE "L1 1.44.0 (Image A) [Fuel/PE/O300 1MB image]\r\n"
                       RETURN_CPU0);
    console_send(fd, "\024log\r");
    console_expect(fd, ESCAPE STAMP "L1 booting 1.44.0\r\n"
                       STAMP "auto power up initiated\r\n"
                       STAMP "power up (COMMAND)\r\n" RETURN_CPU0);
    qtest_quit(s);
    close(fd);
}

static void test_log_capacity(void)
{
    int fd;
    QTestState *s = console_start(&fd, ",auto-power=off", RTC);
    g_autofree char *expected = NULL;
    GString *text = g_string_new(NULL);

    /* 128 entries fit; the 129th drops the oldest, the boot line. */
    for (unsigned i = 1; i <= 128; i++) {
        g_autofree char *command = g_strdup_printf("log insert e%u\r", i);
        console_send(fd, command);
        console_expect(fd, "log entry made.\r\n" PROMPT);
    }
    for (unsigned i = 1; i <= 128; i++) {
        g_string_append_printf(text, STAMP "e%u\r\n", i);
    }
    g_string_append(text, PROMPT);
    expected = g_string_free(text, FALSE);
    console_send(fd, "log\r");
    console_expect(fd, expected);
    qtest_quit(s);
    close(fd);
}

static void test_signals(void)
{
    static const struct {
        const char *command, *line, *display, *log;
    } cases[] = {
        { "softreset", "soft-reset", "Sft rst done", "soft reset (COMMAND)" },
        { "nmi", "nmi", "NMI done", "NMI (COMMAND)" },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        int fd;
        QTestState *s = console_start(&fd, "", RTC);
        g_autofree char *command = g_strdup_printf("\024%s\r",
                                                   cases[i].command);
        g_autofree char *log = g_strdup_printf(
            ESCAPE STAMP "L1 booting\r\n" STAMP "auto power up initiated\r\n"
            STAMP "power up (COMMAND)\r\n" STAMP "%s\r\n" RETURN_CPU0,
            cases[i].log);

        console_expect(fd, POWERING ENTER_CPU0);
        qtest_irq_intercept_out_named(s, L1, cases[i].line);
        console_send(fd, command);
        console_expect(fd, ESCAPE RETURN_CPU0);
        g_assert_cmpuint(qtest_get_irq_raise_count(s, 0), ==, 1);
        g_assert_false(qtest_get_irq(s, 0));
        assert_display(s, cases[i].display);
        console_send(fd, "\024log\r");
        console_expect(fd, log);
        qtest_quit(s);
        close(fd);
    }
}

static void test_reboot_l1(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", RTC);

    console_expect(fd, POWERING ENTER_CPU0);
    console_send(fd, "\024brick slot 02\r");
    console_expect(fd, ESCAPE "brick slot set to 02 "
                       "(takes effect on next L1 reboot/power cycle)\r\n"
                       RETURN_CPU0);
    console_send(fd, "\024brick slot 40\r");
    console_expect(fd, ESCAPE "Invalid slot specified.  "
                       "Valid slot numbers are 1-39.\r\n" RETURN_CPU0);
    console_send(fd, "\024reboot_l1\r");
    console_expect(fd, ESCAPE);
    console_send(fd, "\r");
    console_expect(fd, "001c02-L1> ");
    g_assert_cmpstr(status(s), ==, "prelaunch");
    console_send(fd, "display\r");
    console_expect(fd, "revision: 2.0\r\n"
                       "line 1: \"001c02     /\"\r\n"
                       "line 2: \"L1 running  \"\r\n"
                       "leds:  PWR=on    SERVICE=off     FAULT=off\r\n"
                       "001c02-L1> ");
    console_send(fd, "power check\r");
    console_expect(fd, "power appears on\r\n001c02-L1> ");
    console_send(fd, "log\r");
    console_expect(fd, STAMP "L1 booting\r\n"
                       STAMP "auto power up initiated\r\n"
                       STAMP "power up (COMMAND)\r\n"
                       STAMP "L1 booting\r\n" "001c02-L1> ");
    console_send(fd, "\004");
    console_expect(fd, "entering console mode  001c02 CPU0, "
                       "<CTRL_T> to escape to L1\r\n");
    qtest_quit(s);
    close(fd);

    /* An event subchannel of 4 or more is non-volatile; a lower one is not. */
    for (unsigned subchannel = 1; subchannel <= 23; subchannel += 22) {
        uint8_t events[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 0x07, 1,
                             0, 0, 0, 0, subchannel };
        uint8_t reply[131];
        g_autoptr(QDict) result = NULL;

        s = console_start(&fd, "", "");
        console_expect(fd, POWERING ENTER_CPU0);
        send_frame(s, events, sizeof(events), false);
        g_assert_cmpuint(receive(s, reply), ==, 6);
        console_send(fd, "\024reboot_l1\r");
        console_expect(fd, ESCAPE);
        console_send(fd, "\r");
        console_expect(fd, PROMPT);
        result = qtest_qmp(s, "{'execute':'qom-get','arguments':"
                              "{'path':%s,'property':'event-subchannel'}}", L1);
        g_assert_cmpint(qdict_get_int(result, "return"), ==,
                        subchannel >= 4 ? subchannel : 0);
        qtest_quit(s);
        close(fd);
    }
}

static void test_selection(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", "");
    uint8_t select[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 0x02, 1,
                         0, 0, 0, 0, 4 };
    const uint8_t volunteer[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 0x03 };
    const uint8_t events[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 0x07, 1,
                               0, 0, 0, 0, 23 };
    uint8_t reply[131];

    console_expect(fd, POWERING ENTER_CPU0);
    console_send(fd, "\024select\r");
    console_expect(fd, ESCAPE "console input: " ID " CPU0\r\n"
                       "console output: not filtered.\r\n" RETURN_CPU0);
    send_frame(s, select, sizeof(select), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    console_send(fd, "x");
    console_frame(s, reply, 2);
    g_assert_cmphex(reply[0], ==, 4);
    console_send(fd, "\024select\r");
    console_expect(fd, ESCAPE "console input: " ID " console\r\n"
                       "console output: not filtered.\r\n"
                       "returning to console mode  " ID " console, "
                       "<CTRL_T> to escape to L1\r\n");
    send_frame(s, volunteer, sizeof(volunteer), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    send_frame(s, events, sizeof(events), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    console_send(fd, "\024select subchannel 1\r");
    console_expect(fd, ESCAPE "returning to console mode  " ID " CPU1, "
                       "<CTRL_T> to escape to L1\r\n");
    console_send(fd, "\024select subchannel 9\r");
    console_expect(fd, ESCAPE "ERROR: bad subchanel selection.\r\n"
                       "returning to console mode  " ID " CPU1, "
                       "<CTRL_T> to escape to L1\r\n");
    g_assert_cmpuint(node_position(s, 0x0ffff006), ==, 0);
    qtest_quit(s);
    close(fd);
}

static void test_display_command(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", "-no-shutdown " RTC);

    console_expect(fd, POWERING ENTER_CPU0);
    console_send(fd, "\024display 2 hello\r");
    console_expect(fd, ESCAPE RETURN_CPU0);
    assert_display(s, "hello");
    console_send(fd, "\024display\r");
    console_expect(fd, ESCAPE "revision: 2.0\r\n"
                       "line 1: \"" ID "     /\"\r\n"
                       "line 2: \"hello       \"\r\n"
                       "leds:  PWR=on    SERVICE=off     FAULT=off\r\n"
                       RETURN_CPU0);
    /* The first power-off request while up is deferred; the second acts. */
    console_send(fd, "\024power down\r");
    console_expect(fd, ESCAPE RETURN_CPU0);
    console_send(fd, "\024power down\r");
    console_expect(fd, ESCAPE PROMPT);
    qtest_qmp_eventwait(s, "SHUTDOWN");
    assert_display(s, "Powered Down");
    console_send(fd, "display 2 again\r");
    console_expect(fd, PROMPT);
    assert_display(s, "again");
    console_send(fd, "power up\r");
    console_expect(fd, PROMPT "\r\n" ENTER_CPU0);
    qtest_qmp_eventwait(s, "RESET");
    wait_running(s);
    assert_display(s, "Powered Up");
    qtest_quit(s);
    close(fd);
}

static void test_lifecycle_persistence(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", RTC);
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 3 };
    uint8_t reply[131];

    console_expect(fd, POWERING ENTER_CPU0);
    console_send(fd, "\024display 2 kept\r");
    console_expect(fd, ESCAPE RETURN_CPU0);
    send_frame(s, request, sizeof(request), false);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x61);
    /* A node reset clears the window and nothing the controller keeps. */
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    assert_display(s, "kept");
    console_send(fd, "\024log\r");
    console_expect(fd, ESCAPE STAMP "L1 booting\r\n"
                       STAMP "auto power up initiated\r\n"
                       STAMP "power up (COMMAND)\r\n" RETURN_CPU0);
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, 16);
    qtest_quit(s);
    close(fd);
}

static void test_lifecycle_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-lifecycle-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    int from_fd, to_fd;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = console_start(&from_fd, "", RTC);
    to = console_start(&to_fd, ",auto-power=off", "-incoming defer " RTC);
    console_expect(from_fd, POWERING ENTER_CPU0);
    console_send(from_fd, "\024brick slot 02\r");
    console_expect(from_fd, ESCAPE "brick slot set to 02 "
                            "(takes effect on next L1 reboot/power cycle)\r\n"
                            RETURN_CPU0);
    console_send(from_fd, "\024display 2 moved\r");
    console_expect(from_fd, ESCAPE RETURN_CPU0);
    {
        uint8_t args[5], reply[131];

        integer(args, 0x1234);
        g_assert_cmpuint(general(from, 0x1001, args, 5, 1, reply), ==, 6);
        integer(args, 5);
        g_assert_cmpuint(general(from, 0x1006, args, 5, 1, reply), ==, 6);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(from);
    wait_migration(to);
    assert_display(to, "moved");
    {
        uint8_t reply[131];

        g_assert_cmpuint(general(to, 0x0001, NULL, 0, 0, reply), ==, 11);
        g_assert_cmphex(reply[9], ==, 0x12);
        g_assert_cmphex(reply[10], ==, 0x34);
        g_assert_cmpuint(general(to, 0x0009, NULL, 0, 0, reply), ==, 11);
        g_assert_cmphex(reply[10], ==, 5);
    }
    console_send(to_fd, "\024log\r");
    console_expect(to_fd, ESCAPE STAMP "L1 booting\r\n"
                          STAMP "auto power up initiated\r\n"
                          STAMP "power up (COMMAND)\r\n" RETURN_CPU0);
    console_send(to_fd, "\024reboot_l1\r");
    console_expect(to_fd, ESCAPE);
    console_send(to_fd, "\r");
    console_expect(to_fd, "001c02-L1> ");
    qtest_quit(from);
    qtest_quit(to);
    close(from_fd);
    close(to_fd);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void set_destination(uint8_t *request, uint32_t address)
{
    request[1] = address >> 24;
    request[2] = address >> 16;
    request[3] = address >> 8;
    request[4] = address;
}

static void assert_display(QTestState *s, const char *expected)
{
    g_autoptr(QDict) result = qtest_qmp(s,
        "{'execute':'qom-get','arguments':"
        "{'path':%s,'property':'display-line-2'}}", L1);
    g_assert_cmpstr(qdict_get_str(result, "return"), ==, expected);
}

static void expect_router_error(QTestState *s, uint8_t subchannel)
{
    const uint8_t expected[] = { 0x20 | subchannel, 0xff, 0xff, 0xff, 0xff,
                                 0, 0, 0, 8 };
    uint8_t reply[131];

    g_assert_cmpuint(receive(s, reply), ==, sizeof(expected));
    g_assert_cmpmem(reply, sizeof(expected), expected, sizeof(expected));
}

static void expect_identity(QTestState *s, uint8_t subchannel,
                            uint16_t packed)
{
    const uint8_t expected[] = { 0x20 | subchannel, 0, 0, 0, 0, 2,
                                 0, 0, 0, packed >> 8, packed, 0, 0, 0, 0,
                                 '4' };
    uint8_t reply[131];

    g_assert_cmpuint(receive(s, reply), ==, sizeof(expected));
    g_assert_cmpmem(reply, sizeof(expected), expected, sizeof(expected));
}

/* One 20-bit key routes: the local identity or the all-local sentinel. */
static void test_route_forms(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -S "
                               "-device sgi-l1,id=l1c,rack=7,bay=11" UART);
    uint8_t request[] = { 0x12, 0, 0, 0, 0, 0, 3 };
    const uint32_t local = 0x0ffff006, own = (7 << 18) | (11 << 12) | 6;
    const uint32_t local_forms[] = { local, own, local | (1 << 5),
                                     local | (1 << 11), own | (0x7f << 5) };
    const uint32_t foreign_forms[] = {
        (0x3ff << 18) | (11 << 12) | 6, (7 << 18) | (0x3f << 12) | 6,
        (8 << 18) | (11 << 12) | 6, (7 << 18) | (12 << 12) | 6,
        (1 << 28) | local, (2 << 28) | local, (3 << 28) | local,
        (4 << 28) | local, (5 << 28) | local, (6 << 28) | local,
        (7 << 28) | local, (8 << 28) | local, (0xf << 28) | own,
    };

    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    for (unsigned i = 0; i < G_N_ELEMENTS(local_forms); i++) {
        set_destination(request, local_forms[i]);
        send_frame(s, request, sizeof(request), false);
        expect_identity(s, 18, 0x1cb);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(foreign_forms); i++) {
        set_destination(request, foreign_forms[i]);
        send_frame(s, request, sizeof(request), false);
        expect_router_error(s, 18);
    }
    /* Any subchannel carries a request and gets its reply on the same one. */
    request[0] = 0x00;
    set_destination(request, local);
    send_frame(s, request, sizeof(request), false);
    expect_identity(s, 0, 0x1cb);
    request[0] = 0x1f;
    send_frame(s, request, sizeof(request), false);
    expect_identity(s, 31, 0x1cb);
    qtest_quit(s);
}

static void expect_silence(QTestState *s, const uint8_t *request, size_t n)
{
    const uint8_t recover[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 3 };

    send_frame(s, request, n, false);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    send_frame(s, recover, sizeof(recover), false);
    expect_identity(s, 16, 0x41);
}

/* Which local tasks answer, echo, or drop. */
static void test_tasks(void)
{
    QTestState *s = start();
    uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 3 };
    const uint8_t loopback[] = { 0x10, 0x0f, 0xff, 0xf0, 18, 0xab, 0xcd, 2,
                                 0, 0, 0, 0, 5, 1, 'h', 'i', 0 };
    const uint8_t echoed[] = { 0x30, 0, 0, 0xab, 0xcd, 2,
                               0, 0, 0, 0, 5, 1, 'h', 'i', 0 };
    const uint8_t console_in[] = { 0x10, 0x0f, 0xff, 0xf0, 4 << 5 | 5,
                                   0x00, 0x00, 1, 0, 0, 0, 0, 0x41 };
    const uint8_t console_in_21[] = { 0x10, 0x0f, 0xff, 0xf0, 2 << 5 | 21,
                                      0x12, 0x34, 1, 0, 0, 0, 0, 0x62 };
    static const unsigned silent[] = {
        0, 1, 4, 7, 8, 9, 10, 11, 12, 13, 17, 20, 22, 23, 24, 25, 26,
        27, 28, 29, 30, 31, 2, 14, 15, 16, 19
    };
    uint8_t reply[131];

    for (unsigned i = 0; i < G_N_ELEMENTS(silent); i++) {
        set_destination(request, 0x0ffff000 | silent[i]);
        expect_silence(s, request, sizeof(request));
    }
    send_frame(s, loopback, sizeof(loopback), false);
    g_assert_cmpuint(receive(s, reply), ==, sizeof(echoed));
    g_assert_cmpmem(reply, sizeof(echoed), echoed, sizeof(echoed));
    /* The Bedrock console tasks send the first argument back as input. */
    send_frame(s, console_in, sizeof(console_in), false);
    g_assert_cmpuint(receive(s, reply), ==, 2);
    g_assert_cmphex(reply[0], ==, 4);
    g_assert_cmphex(reply[1], ==, 'A');
    send_frame(s, console_in_21, sizeof(console_in_21), false);
    g_assert_cmpuint(receive(s, reply), ==, 2);
    g_assert_cmphex(reply[0], ==, 2);
    g_assert_cmphex(reply[1], ==, 'b');
    qtest_quit(s);
}

static void text_request(uint8_t *request, size_t *n, uint16_t code,
                         const char *text)
{
    request[0] = 0x10;
    set_destination(request, 0x0ffff003);
    request[5] = code >> 8;
    request[6] = code;
    request[7] = 1;
    request[8] = 1;
    memcpy(request + 9, text, strlen(text) + 1);
    *n = 10 + strlen(text);
}

/* The command interpreter runs any code's text and returns its lines. */
static void test_command_interpreter(void)
{
    QTestState *s = start();
    uint8_t request[128], reply[129];
    size_t n;
    const uint8_t version[] = { 0x30, 0, 0, 0, 0, 1, 1,
                                'L', '1', ' ', 'p', 'r', 'o', 't', 'o', 'c',
                                'o', 'l', ' ', 'r', 'e', 's', 'p', 'o', 'n',
                                'd', 'e', 'r', ';', ' ', 'f', 'i', 'r', 'm',
                                'w', 'a', 'r', 'e', ' ', 'r', 'e', 'v', 'i',
                                's', 'i', 'o', 'n', ' ', 'u', 'n', 'a', 'v',
                                'a', 'i', 'l', 'a', 'b', 'l', 'e', 0 };
    const uint8_t two_lines[] = { 0x30, 0, 0, 0, 0, 2,
                                  1, 'c', 'o', 'n', 's', 'o', 'l', 'e', ' ',
                                  'i', 'n', 'p', 'u', 't', ':', ' ', '0', '0',
                                  '1', 'c', '0', '1', ' ', 'C', 'P', 'U', '0',
                                  0,
                                  1, 'c', 'o', 'n', 's', 'o', 'l', 'e', ' ',
                                  'o', 'u', 't', 'p', 'u', 't', ':', ' ', 'n',
                                  'o', 't', ' ', 'f', 'i', 'l', 't', 'e', 'r',
                                  'e', 'd', '.', 0 };
    const uint8_t unknown[] = { 0x30, 0xff, 0xff, 0xff, 0xff, 1,
                                1, 'E', 'R', 'R', 'O', 'R', ':', ' ', 'c',
                                'o', 'm', 'm', 'a', 'n', 'd', ' ', 'n', 'o',
                                't', ' ', 'f', 'o', 'u', 'n', 'd', '.', 0 };

    text_request(request, &n, 0x1234, "version");
    send_frame(s, request, n, false);
    g_assert_cmpuint(receive(s, reply), ==, sizeof(version));
    g_assert_cmpmem(reply, sizeof(version), version, sizeof(version));
    text_request(request, &n, 0, "* select");
    send_frame(s, request, n, false);
    g_assert_cmpuint(receive(s, reply), ==, sizeof(two_lines));
    g_assert_cmpmem(reply, sizeof(two_lines), two_lines, sizeof(two_lines));
    text_request(request, &n, 0, "bogus");
    send_frame(s, request, n, false);
    g_assert_cmpuint(receive(s, reply), ==, sizeof(unknown));
    g_assert_cmpmem(reply, sizeof(unknown), unknown, sizeof(unknown));
    /* Not one text argument: minus 100, a model choice. */
    request[7] = 0;
    send_frame(s, request, 8, false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9c);
    request[7] = 1;
    request[8] = 0;
    send_frame(s, request, 13, false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9c);
    /* The command ran only through the interpreter: no console text. */
    assert_display(s, "Powered Up");
    qtest_quit(s);
}

static void raw_bytes(QTestState *s, const uint8_t *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        qtest_writeq(s, BASE, bytes[i]);
    }
}

/* The link reader's tolerance, from the firmware's frame reader. */
static void test_receive_tolerance(void)
{
    QTestState *s = start();
    const uint8_t identity[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 3 };
    uint8_t reply[129], big[300];
    uint16_t crc;
    size_t n;

    /* Frames of one to four bytes are dropped; a good frame follows. */
    for (unsigned length = 1; length <= 4; length++) {
        uint8_t frame[4] = { 0x10, 0x0f, 0xff, 0xf0 };
        qtest_writeq(s, BASE, 0x7e);
        raw_bytes(s, frame, length);
        qtest_writeq(s, BASE, 0x7e);
        g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
        send_frame(s, identity, sizeof(identity), false);
        expect_identity(s, 16, 0x41);
    }
    /* A three-byte frame with a good check is still short. */
    crc = ~fcs(0xffff, 0x10);
    qtest_writeq(s, BASE, 0x7e);
    qtest_writeq(s, BASE, 0x10);
    put(s, crc);
    put(s, crc >> 8);
    qtest_writeq(s, BASE, 0x7e);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    /*
     * An escape before the closing flag drops the frame; that flag opens
     * the next one, so the next frame needs no flag of its own.
     */
    qtest_writeq(s, BASE, 0x7e);
    qtest_writeq(s, BASE, 0x10);
    qtest_writeq(s, BASE, 0x7d);
    qtest_writeq(s, BASE, 0x7e);
    crc = 0xffff;
    for (unsigned i = 0; i < sizeof(identity); i++) {
        crc = fcs(crc, identity[i]);
        put(s, identity[i]);
    }
    crc = ~crc;
    put(s, crc);
    put(s, crc >> 8);
    qtest_writeq(s, BASE, 0x7e);
    expect_identity(s, 16, 0x41);
    /* Escape after escape re-arms: the header 0x10 sent as 7d 7d 30. */
    qtest_writeq(s, BASE, 0x7e);
    qtest_writeq(s, BASE, 0x7d);
    qtest_writeq(s, BASE, 0x7d);
    qtest_writeq(s, BASE, 0x30);
    crc = fcs(0xffff, 0x10);
    for (unsigned i = 1; i < sizeof(identity); i++) {
        crc = fcs(crc, identity[i]);
        put(s, identity[i]);
    }
    crc = ~crc;
    put(s, crc);
    put(s, crc >> 8);
    qtest_writeq(s, BASE, 0x7e);
    expect_identity(s, 16, 0x41);
    /* A 136-byte payload is within the 256-byte receive cap. */
    memcpy(big, identity, 4);
    big[4] = 6;
    big[5] = 0;
    big[6] = 0x0e;
    big[7] = 1;
    big[8] = 0xff;
    memset(big + 9, 0x5a, 127);
    n = 136;
    send_frame(s, big, n, false);
    g_assert_cmpuint(receive(s, reply), ==, 11);
    g_assert_cmphex(reply[4], ==, 0);
    /* Two such arguments make 264 bytes: over the cap, dropped, recovered. */
    big[7] = 2;
    big[136] = 0xff;
    memset(big + 137, 0xa5, 127);
    n = 264;
    send_frame(s, big, n, false);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    send_frame(s, identity, sizeof(identity), false);
    expect_identity(s, 16, 0x41);
    qtest_quit(s);
}

/* The PROM-request debugging switch logs each general-task request. */
static void test_request_log(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", RTC);
    const uint8_t identity[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 3 };
    const uint8_t command[] = { 0x11, 0x0f, 0xff, 0xf0, 3, 0, 0, 1,
                                1, 'l', '1', 0 };
    uint8_t reply[131];

    console_expect(fd, POWERING ENTER_CPU0);
    console_send(fd, "\024l1dbg\r");
    console_expect(fd, ESCAPE "L1 PROM request debugging is off\r\n"
                       RETURN_CPU0);
    console_send(fd, "\024l1dbg promreq on\r");
    console_expect(fd, ESCAPE RETURN_CPU0);
    console_send(fd, "\024l1dbg\r");
    console_expect(fd, ESCAPE "L1 PROM request debugging is on\r\n"
                       RETURN_CPU0);
    send_frame(s, identity, sizeof(identity), false);
    expect_identity(s, 16, 0x41);
    /* A command-interpreter request is not logged; it returns its line. */
    send_frame(s, command, sizeof(command), false);
    g_assert_cmpuint(receive(s, reply), ==, 55);
    g_assert_cmphex(reply[4], ==, 0);
    g_assert_cmphex(reply[5], ==, 1);
    console_send(fd, "\024log\r");
    console_expect(fd, ESCAPE STAMP "L1 booting\r\n"
                       STAMP "auto power up initiated\r\n"
                       STAMP "power up (COMMAND)\r\n"
                       STAMP "L1REQ: 00041205 - 0003\r\n" RETURN_CPU0);
    console_send(fd, "\024l1dbg promreq on\r");
    console_expect(fd, ESCAPE RETURN_CPU0);
    console_send(fd, "\024reboot_l1\r");
    console_expect(fd, ESCAPE);
    console_send(fd, "\r");
    console_expect(fd, PROMPT);
    console_send(fd, "l1dbg\r");
    console_expect(fd, "L1 PROM request debugging is off\r\n" PROMPT);
    console_send(fd, "\004");
    console_expect(fd, ENTER_CPU0);
    send_frame(s, identity, sizeof(identity), false);
    expect_identity(s, 16, 0x41);
    console_send(fd, "\024log\r");
    console_expect(fd, ESCAPE STAMP "L1 booting\r\n"
                       STAMP "auto power up initiated\r\n"
                       STAMP "power up (COMMAND)\r\n"
                       STAMP "L1REQ: 00041205 - 0003\r\n"
                       STAMP "L1 booting\r\n" RETURN_CPU0);
    qtest_quit(s);
    close(fd);
}

/* Console output on a subchannel retargets the operator's input to it. */
static void test_console_retarget(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", "");
    const uint8_t output[] = { 0x50, 'P', 'R', 'O', 'M' };
    uint8_t reply[131];

    console_expect(fd, POWERING ENTER_CPU0);
    send_frame(s, output, sizeof(output), false);
    console_expect(fd, "PROM");
    console_send(fd, "x");
    console_frame(s, reply, 2);
    g_assert_cmphex(reply[0], ==, 0x10);
    g_assert_cmphex(reply[1], ==, 'x');
    qtest_quit(s);
    close(fd);
}

static void test_task_commands(void)
{
    QTestState *s = start();
    uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 3, 0, 3, 0 };
    uint8_t reply[131];
    /* The interpreter takes any code; without its text it answers minus 100. */
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9c);
    request[4] = 6;
    request[6] = 0;
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9b);
    request[1] = 0x4f;
    request[5] = 0x10;
    request[6] = 5;
    send_frame(s, request, sizeof(request), false);
    /* The attached-I/O address is not local: router error, line unchanged. */
    expect_router_error(s, 16);
    assert_display(s, "Powered Up");
    request[1] = 0x0f;
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9c);
    qtest_quit(s);
}

static void test_settings_queries(void)
{
    QTestState *s = start();
    const uint8_t commands[] = { 1, 9, 10 };
    uint8_t request[] = { 0x17, 0x0f, 0xff, 0xf0, 6, 0, 0, 0,
                          0, 0, 0, 0, 0x55 };
    uint8_t expected[16] = { 0x37, 0, 0, 0, 0, 1 };
    const uint8_t bad_count[] = { 0x37, 0xff, 0xff, 0xff, 0x9c, 0 };
    uint8_t reply[131];

    for (unsigned i = 0; i < G_N_ELEMENTS(commands); i++) {
        unsigned length = commands[i] == 10 ? 16 : 11;
        request[6] = commands[i];
        expected[5] = commands[i] == 10 ? 2 : 1;
        for (unsigned count_byte = 0; count_byte < 2; count_byte++) {
            send_frame(s, request, 7 + count_byte, false);
            g_assert_cmpuint(receive(s, reply), ==, length);
            g_assert_cmpmem(reply, length, expected, length);
        }
        /* Extra arguments are accepted: the same reply comes back. */
        request[7] = 1;
        send_frame(s, request, sizeof(request), false);
        g_assert_cmpuint(receive(s, reply), ==, length);
        g_assert_cmpmem(reply, length, expected, length);
        request[7] = 0;
        if (commands[i] == 1) {
            /* The identity request accepts an extra integer too. */
            uint8_t identity[] = { 0x17, 0x0f, 0xff, 0xf0, 6, 0, 3, 1,
                                   0, 0, 0, 0, 9 };
            const uint8_t answer[] = { 0x37, 0, 0, 0, 0, 2, 0, 0, 0, 0, 65,
                                       0, 0, 0, 0, '4' };

            send_frame(s, identity, sizeof(identity), false);
            g_assert_cmpuint(receive(s, reply), ==, sizeof(answer));
            g_assert_cmpmem(reply, sizeof(answer), answer, sizeof(answer));
        }
        request[4] = 3;
        /* The interpreter takes any code but wants one text argument. */
        send_frame(s, request, 8, false);
        g_assert_cmpuint(receive(s, reply), ==, sizeof(bad_count));
        g_assert_cmpmem(reply, sizeof(bad_count), bad_count, sizeof(bad_count));
        request[4] = 6;
        request[1] = 0x4f;
        send_frame(s, request, 8, false);
        expect_router_error(s, 23);
        request[1] = 0x0f;
    }
    qtest_quit(s);
}

static void expect_debug_switches(QTestState *s, uint8_t high, uint8_t low)
{
    uint8_t reply[131];
    const uint8_t expected[] = { 0x30, 0, 0, 0, 0, 1,
                                 0, 0, 0, high, low };

    g_assert_cmpuint(general(s, 1, NULL, 0, 0, reply), ==,
                     sizeof(expected));
    g_assert_cmpmem(reply, sizeof(expected), expected, sizeof(expected));
}

static void send_task3(QTestState *s, const char *command)
{
    uint8_t request[128] = { 0x10, 0x0f, 0xff, 0xf0, 3, 0, 3, 1, 1 };
    size_t length = strlen(command) + 1;

    g_assert_cmpuint(length, <=, sizeof(request) - 9);
    memcpy(request + 9, command, length);
    send_frame(s, request, length + 9, false);
}

static void expect_raw_reply(QTestState *s, const uint8_t *expected,
                             size_t length)
{
    uint8_t actual[131];

    g_assert_cmpuint(length, <=, sizeof(actual));
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x61);
    for (size_t i = 0; i < length; i++) {
        actual[i] = qtest_readq(s, BASE);
    }
    g_assert_cmpmem(actual, length, expected, length);
}

static void expect_raw_hex_reply(QTestState *s, const char *hex)
{
    uint8_t expected[131];
    size_t length = 0;

    for (const char *p = hex; *p; ) {
        int high, low;

        if (*p == ' ') {
            p++;
            continue;
        }
        high = g_ascii_xdigit_value(p[0]);
        low = g_ascii_xdigit_value(p[1]);
        g_assert_cmpint(high, >=, 0);
        g_assert_cmpint(low, >=, 0);
        g_assert_cmpuint(length, <, sizeof(expected));
        expected[length++] = high << 4 | low;
        p += 2;
    }
    expect_raw_reply(s, expected, length);
}

static void test_debug_set(void)
{
    int fd;
    QTestState *s = console_start(&fd, ",auto-power=off", "");
    static const struct {
        const char *command;
        const char *response;
        uint8_t high;
        uint8_t low;
    } writes[] = {
        { "debug 269\r", "debug switches set to 0x010d\r\n" PROMPT,
          0x01, 0x0d },
        { "debug 040\r", "debug switches set to 0x0020\r\n" PROMPT,
          0x00, 0x20 },
        { "debug +0x010d\r", "debug switches set to 0x010d\r\n" PROMPT,
          0x01, 0x0d },
        { "debug -1\r", "debug switches set to 0xffff\r\n" PROMPT,
          0xff, 0xff },
        { "debug 65536\r", "debug switches set to 0x0000\r\n" PROMPT,
          0x00, 0x00 },
        { "debug 0x\r", "debug switches set to 0x0000\r\n" PROMPT,
          0x00, 0x00 },
        { "debug 4294967296\r", "debug switches set to 0xffff\r\n"
          PROMPT, 0xff, 0xff },
        { "debug 0x010d\r", "debug switches set to 0x010d\r\n" PROMPT,
          0x01, 0x0d },
    };
    static const char *bad[] = {
        "debug nope\r", "debug 0x10junk\r", "debug 09\r",
        "debug 4294967296junk\r", "debug 0x10 extra\r",
    };

    console_send(fd, "\r");
    console_expect(fd, PROMPT);
    console_send(fd, "debug\r");
    console_expect(fd, "debug switches set to 0x0000\r\n" PROMPT);
    expect_debug_switches(s, 0, 0);
    for (size_t i = 0; i < ARRAY_SIZE(writes); i++) {
        console_send(fd, writes[i].command);
        console_expect(fd, writes[i].response);
        expect_debug_switches(s, writes[i].high, writes[i].low);
    }
    for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
        console_send(fd, bad[i]);
        console_expect(fd, "ERROR: invalid arguments for 'debug' command, "
                       "try \"help debug\"\r\n" PROMPT);
        expect_debug_switches(s, 1, 0x0d);
    }
    console_send(fd, "debug\r");
    console_expect(fd, "debug switches set to 0x010d\r\n" PROMPT);

    /* Node reset retention is inferred by the reviewed L1 contract. */
    qtest_system_reset(s);
    expect_debug_switches(s, 1, 0x0d);
    qtest_quit(s);
    close(fd);
}

static void test_debug_task3(void)
{
    QTestState *s = start();
    const uint8_t accepted[] = {
        0x7e, 0x30, 0, 0, 0, 0, 1, 1,
        'd', 'e', 'b', 'u', 'g', ' ', 's', 'w', 'i', 't', 'c', 'h', 'e', 's',
        ' ', 's', 'e', 't', ' ', 't', 'o', ' ', '0', 'x', '0', '1', '0', 'd',
        '\n', 0, 0x15, 0x5b, 0x7e,
    };
    const uint8_t malformed[] = {
        0x7e, 0x30, 0xff, 0xff, 0xfb, 0x50, 1, 1,
        'E', 'R', 'R', 'O', 'R', ':', ' ', 'c', 'o', 'm', 'm', 'a', 'n', 'd',
        ' ', 'n', 'o', 't', ' ', 'f', 'o', 'u', 'n', 'd', '.', '\n', 0,
        0x42, 0xac, 0x7e,
    };
    const char *wildcard =
        "7e 30 ff ff ff ff 01 01 49 4e 46 4f 3a 20 77 69 "
        "6c 64 63 61 72 64 20 64 65 62 75 67 20 69 73 20 "
        "75 6e 73 75 70 70 6f 72 74 65 64 20 77 69 74 68 "
        "6f 75 74 20 63 6f 6e 6e 65 63 74 65 64 20 6d 6f "
        "64 75 6c 65 73 2e 0a 00 f4 6e 7e";
    const char *peer =
        "7e 30 ff ff ff ff 01 01 49 4e 46 4f 3a 20 70 65 "
        "65 72 20 64 65 62 75 67 20 69 73 20 75 6e 73 "
        "75 70 70 6f 72 74 65 64 2e 0a 00 f6 65 7e";

    send_task3(s, "debug 269");
    expect_raw_reply(s, accepted, sizeof(accepted));
    expect_debug_switches(s, 1, 0x0d);
    send_task3(s, "debug nope");
    expect_raw_reply(s, malformed, sizeof(malformed));
    expect_debug_switches(s, 1, 0x0d);

    send_task3(s, "* debug 0");
    expect_raw_hex_reply(s, wildcard);
    expect_debug_switches(s, 1, 0x0d);
    send_task3(s, "ctc debug 0");
    expect_raw_hex_reply(s, peer);
    expect_debug_switches(s, 1, 0x0d);
    qtest_quit(s);
}

static void test_uart_interrupts(void)
{
    QTestState *s = start();
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 3 };
    uint8_t reply[131];

    send_frame(s, request, sizeof(request), false);
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1);
    qtest_writeq(s, BASE + 8, 1);
    qtest_writeq(s, BASE + 0x10, 1);
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 4);
    qtest_writeq(s, BASE + 8, 0);
    qtest_irq_intercept_out_named(s, PATH, "uart");
    g_assert_false(qtest_get_irq(s, 0));
    qtest_writeq(s, BASE + 8, 3);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 4);
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 4);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_cmpuint(receive(s, reply), ==, 16);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 2);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1);
    qtest_clock_step(s, 1000000000);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_writeq(s, BASE + 8, 3);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_writeq(s, BASE, 0x7e);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 2);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_writeq(s, BASE + 0x18, 0x80);
    qtest_writeq(s, BASE, 0x33);
    qtest_writeq(s, BASE + 8, 0xff);
    g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 0xff);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_writeq(s, BASE + 0x18, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 3);
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1);
    qtest_writeq(s, BASE + 8, 1);
    qtest_writeq(s, BASE + 8, 3);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, BASE + 0x18, 0x80);
    qtest_writeq(s, BASE, 1);
    qtest_writeq(s, BASE + 8, 0);
    qtest_writeq(s, BASE + 0x18, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 3);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, BASE + 8, 1);
    g_assert_false(qtest_get_irq(s, 0));
    send_frame(s, request, sizeof(request), false);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, BASE + 8, 0);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x61);
    qtest_writeq(s, BASE + 8, 1);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_cmpuint(receive(s, reply), ==, 16);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_writeq(s, BASE + 8, 0xfc);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1);
    qtest_writeq(s, BASE + 8, 2);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_system_reset(s);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1);
    qtest_quit(s);
}

static void test_uart_irq_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-irq-migrate-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 3 };
    uint8_t reply[131];

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    for (unsigned state = 0; state < 3; state++) {
        QTestState *from = start();
        QTestState *to = qtest_init("-M none -cpu R10000 -S -incoming defer "
                                    "-device sgi-l1,id=l1c" UART);
        qtest_sysbus_mmio_map(to, PATH, 0, BASE);
        qtest_irq_intercept_out_named(to, PATH, "uart");
        qtest_writeq(from, BASE + 8, 3);
        if (state == 0) {
            g_assert_cmphex(qtest_readq(from, BASE + 0x10), ==, 2);
        } else if (state == 2) {
            send_frame(from, request, sizeof(request), false);
        }
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        qtest_qmp_assert_success(from,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        wait_migration(from);
        wait_migration(to);
        g_assert_cmpint(qtest_get_irq(to, 0), ==, state != 0);
        if (state == 2) {
            g_assert_cmphex(qtest_readq(to, BASE + 0x10), ==, 4);
            g_assert_cmpuint(receive(to, reply), ==, 16);
        }
        g_assert_cmphex(qtest_readq(to, BASE + 0x10), ==, state ? 2 : 1);
        g_assert_false(qtest_get_irq(to, 0));
        g_assert_cmphex(qtest_readq(to, BASE + 0x10), ==, 1);
        qtest_quit(from);
        qtest_quit(to);
        unlink(socket);
    }
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void expect_wire(QTestState *s, const uint8_t *wire, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x61);
        g_assert_cmphex(qtest_readq(s, BASE), ==, wire[i]);
    }
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
}

static uint32_t node_position(QTestState *s, uint32_t address)
{
    uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 0x0e };
    uint8_t reply[131];
    set_destination(request, address);
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, 11);
    g_assert_cmphex(reply[4], ==, 0);
    g_assert_cmphex(reply[5], ==, 1);
    g_assert_cmphex(reply[6], ==, 0);
    return reply[10];
}

static void console_select(QTestState *s, uint32_t argument, int32_t code)
{
    uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 0x02, 1,
                          0, argument >> 24, argument >> 16, argument >> 8,
                          argument };
    uint8_t reply[131];
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, (uint8_t)code);
    g_assert_cmphex(reply[5], ==, 0);
}

static void test_node_position(void)
{
    QTestState *s = start();
    int fd;
    uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 0x0e, 0 };
    const uint8_t wire[] = { 0x7e, 0x30, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,
                             0x30, 0x98, 0x7e };
    const uint8_t wire_two[] = { 0x7e, 0x30, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2,
                                 0x22, 0xbb, 0x7e };
    const uint8_t two_ints[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 0x0e, 2,
                                 0, 0, 0, 0, 1, 0, 0, 0, 0, 2 };
    const uint8_t text[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 0x0e, 1,
                             1, 'x', 0 };
    const uint8_t select_none[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 0x02, 0 };
    const uint8_t select_text[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 0x02, 1,
                                    1, 'x', 0 };
    const uint8_t wire_range[] = { 0x7e, 0x30, 0xff, 0xff, 0xff, 0x97, 0,
                                   0x71, 0x92, 0x7e };
    uint8_t reply[131];

    send_frame(s, request, sizeof(request), false);
    expect_wire(s, wire, sizeof(wire));
    send_frame(s, request, sizeof(request) - 1, false);
    expect_wire(s, wire, sizeof(wire));
    send_frame(s, two_ints, sizeof(two_ints), false);
    expect_wire(s, wire, sizeof(wire));
    send_frame(s, text, sizeof(text), false);
    expect_wire(s, wire, sizeof(wire));
    console_select(s, 4, 0);
    g_assert_cmpuint(node_position(s, 0x0ffff006), ==, 0);
    console_select(s, 0x40, 0);
    send_frame(s, request, sizeof(request), false);
    expect_wire(s, wire_two, sizeof(wire_two));
    console_select(s, 4, 0);
    g_assert_cmpuint(node_position(s, 0x0ffff006), ==, 2);
    send_frame(s, select_none, sizeof(select_none), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9c);
    g_assert_cmpuint(node_position(s, 0x0ffff006), ==, 2);
    send_frame(s, select_text, sizeof(select_text), false);
    expect_wire(s, wire_range, sizeof(wire_range));
    g_assert_cmpuint(node_position(s, 0x0ffff006), ==, 2);
    send_frame(s, request, sizeof(request), false);
    expect_wire(s, wire_two, sizeof(wire_two));
    /* Neither code is served on the command interpreter or the I/O view. */
    set_destination(request, 0x4ffff006);
    send_frame(s, request, sizeof(request), false);
    expect_router_error(s, 16);
    /* The interpreter takes any code but wants one text argument. */
    set_destination(request, 0x0ffff003);
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9c);
    set_destination(request, 0x0ffff006);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t select[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 0x02, 1,
                             0, 0, 0, 0, 0x40 };
        set_destination(select, i ? 0x0ffff003 : 0x4ffff006);
        send_frame(s, select, sizeof(select), false);
        if (i) {
            g_assert_cmpuint(receive(s, reply), ==, 6);
            g_assert_cmphex(reply[4], ==, 0x9c);
        } else {
            expect_router_error(s, 16);
        }
    }
    g_assert_cmpuint(node_position(s, 0x0ffff006), ==, 2);
    /* A node reset does not reach the controller; a controller restart does. */
    qtest_system_reset(s);
    g_assert_cmpuint(node_position(s, 0x0ffff006), ==, 2);
    qtest_quit(s);

    s = qtest_init("-M none -cpu R10000 -S "
                   "-device sgi-l1,id=l1c,node-position=2" UART);
    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    g_assert_cmpuint(node_position(s, 0x0ffff006), ==, 2);
    console_select(s, 4, 0);
    g_assert_cmpuint(node_position(s, 0x0ffff006), ==, 2);
    /* Position bits are only ever or-ed in: from 2, bit 5 makes 3. */
    console_select(s, 0x20, 0);
    g_assert_cmpuint(node_position(s, 0x0ffff006), ==, 3);
    qtest_system_reset(s);
    g_assert_cmpuint(node_position(s, 0x0ffff006), ==, 3);
    qtest_quit(s);

    s = console_start(&fd, ",node-position=2", "");
    console_expect(fd, POWERING "entering console mode  " ID " CPU0, "
                       "<CTRL_T> to escape to L1\r\n");
    console_select(s, 0x20, 0);
    g_assert_cmpuint(node_position(s, 0x0ffff006), ==, 3);
    console_send(fd, "\024reboot_l1\r");
    console_expect(fd, ESCAPE);
    /* The prompt proves the restart ran before the next frame is sent. */
    console_send(fd, "\r");
    console_expect(fd, PROMPT);
    g_assert_cmpuint(node_position(s, 0x0ffff006), ==, 2);
    qtest_quit(s);
    close(fd);
}

static void test_node_position_range(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*node-position must be 0 to 3*");
        return;
    }
    s = qtest_init("-M none -cpu R10000 -S "
                   "-device sgi-l1,id=l1c,node-position=4" UART);
    qtest_quit(s);
}

static void test_node_position_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-position-migrate-XXXXXX",
                                         &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from = start(), *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    to = qtest_init("-M none -cpu R10000 -S -incoming defer "
                    "-device sgi-l1,id=l1c" UART);
    qtest_sysbus_mmio_map(to, PATH, 0, BASE);
    console_select(from, 0x60, 0);
    g_assert_cmpuint(node_position(from, 0x0ffff006), ==, 3);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(from);
    wait_migration(to);
    g_assert_cmpuint(node_position(to, 0x0ffff006), ==, 3);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_uart_unlinked(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*L1 UART window needs a controller link*");
        return;
    }
    s = qtest_init("-M none -cpu R10000 -S -device sgi-l1-uart,id=l1");
    qtest_quit(s);
}

static void test_uart_shared(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*L1 controller already has a UART window*");
        return;
    }
    s = qtest_init("-M none -cpu R10000 -S -device sgi-l1,id=l1c" UART
                   " -device sgi-l1-uart,id=l2,controller=l1c");
    qtest_quit(s);
}

static size_t general(QTestState *s, uint16_t code, const uint8_t *args,
                      size_t args_len, unsigned count, uint8_t *reply)
{
    uint8_t request[136] = { 0x10, 0x0f, 0xff, 0xf0, 6, code >> 8, code,
                             count };
    size_t n = 8;

    if (args_len) {
        memcpy(request + 8, args, args_len);
        n += args_len;
    }
    send_frame(s, request, n, false);
    return receive(s, reply);
}

static void integer(uint8_t *out, uint32_t value)
{
    out[0] = 0;
    out[1] = value >> 24;
    out[2] = value >> 16;
    out[3] = value >> 8;
    out[4] = value;
}

/* The read-only rows the firmware serves that the model answers from state. */
static void test_general_reads(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -S -device sgi-l1,id=l1c,"
                               "revision-major=1,revision-minor=44,"
                               "revision-patch=0,debug-switches=0x10d,"
                               "system-serial=L1234567,partition=3" UART);
    uint8_t reply[129], args[10];
    static const uint8_t revision[] = { 0x30, 0, 0, 0, 0, 3, 0, 0, 0, 0, 1,
                                        0, 0, 0, 0, 44, 0, 0, 0, 0, 0 };
    static const uint8_t serial[] = { 0x30, 0, 0, 0, 0, 1, 1,
                                      'L', '1', '2', '3', '4', '5', '6', '7',
                                      0 };

    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    /* 0x0001 debug switches, 0x0009 partition: stored settings. */
    g_assert_cmpuint(general(s, 0x0001, NULL, 0, 0, reply), ==, 11);
    g_assert_cmphex(reply[9], ==, 0x01);
    g_assert_cmphex(reply[10], ==, 0x0d);
    g_assert_cmpuint(general(s, 0x0009, NULL, 0, 0, reply), ==, 11);
    g_assert_cmphex(reply[10], ==, 3);
    /* 0x1001 writes the switches; 0x1006 the partition; both answer 0. */
    integer(args, 0x4);
    g_assert_cmpuint(general(s, 0x1001, args, 5, 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    g_assert_cmpuint(general(s, 0x0001, NULL, 0, 0, reply), ==, 11);
    g_assert_cmphex(reply[10], ==, 4);
    integer(args, 7);
    g_assert_cmpuint(general(s, 0x1006, args, 5, 1, reply), ==, 6);
    g_assert_cmpuint(general(s, 0x0009, NULL, 0, 0, reply), ==, 11);
    g_assert_cmphex(reply[10], ==, 7);
    g_assert_cmpuint(general(s, 0x1001, NULL, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9c);
    /* 0x0005 firmware revision, 0x0008 system serial. */
    g_assert_cmpuint(general(s, 0x0005, NULL, 0, 0, reply), ==,
                     sizeof(revision));
    g_assert_cmpmem(reply, sizeof(revision), revision, sizeof(revision));
    g_assert_cmpuint(general(s, 0x0008, NULL, 0, 0, reply), ==,
                     sizeof(serial));
    g_assert_cmpmem(reply, sizeof(serial), serial, sizeof(serial));
    /* 0x0004 brick serial: none configured, minus 104. */
    g_assert_cmpuint(general(s, 0x0004, NULL, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x98);
    /* 0x000a port speeds, 0x000b the two-bit setting, 0x1008 sets it. */
    g_assert_cmpuint(general(s, 0x000a, NULL, 0, 0, reply), ==, 16);
    g_assert_cmpuint(general(s, 0x000b, NULL, 0, 0, reply), ==, 11);
    g_assert_cmphex(reply[10], ==, 0);
    integer(args, 2);
    g_assert_cmpuint(general(s, 0x1008, args, 5, 1, reply), ==, 6);
    g_assert_cmpuint(general(s, 0x000b, NULL, 0, 0, reply), ==, 11);
    g_assert_cmphex(reply[10], ==, 2);
    integer(args, 7);
    g_assert_cmpuint(general(s, 0x1008, args, 5, 1, reply), ==, 6);
    g_assert_cmpuint(general(s, 0x000b, NULL, 0, 0, reply), ==, 11);
    g_assert_cmphex(reply[10], ==, 3);
    /* 0x000c, 0x000d: no attached bricks, so rack 0 bay 0 type 0 per port. */
    g_assert_cmpuint(general(s, 0x000c, NULL, 0, 0, reply), ==, 6 + 8 * 5);
    g_assert_cmphex(reply[5], ==, 8);
    g_assert_cmpuint(general(s, 0x000d, NULL, 0, 0, reply), ==, 6 + 4 * 5);
    /* 0x000f last event reasons: none yet, then COMMAND after a reset. */
    g_assert_cmpuint(general(s, 0x000f, NULL, 0, 0, reply), ==, 16);
    /* 0x0010 processor masks: two processors known. */
    g_assert_cmpuint(general(s, 0x0010, NULL, 0, 0, reply), ==, 6 + 4 * 5);
    g_assert_cmphex(reply[10], ==, 1);
    g_assert_cmphex(reply[15], ==, 2);
    /* 0x0011 scan record: not modeled, minus 106 with a status. */
    g_assert_cmpuint(general(s, 0x0011, NULL, 0, 0, reply), ==, 11);
    g_assert_cmphex(reply[4], ==, 0x96);
    /* 0x4000 inventory: empty, minus 104. */
    g_assert_cmpuint(general(s, 0x4000, NULL, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x98);
    /* 0x0007 EEPROM format: nothing configured, minus 104 for any index. */
    integer(args, 0);
    g_assert_cmpuint(general(s, 0x0007, args, 5, 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x98);
    integer(args, 9);
    g_assert_cmpuint(general(s, 0x0007, args, 5, 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x98);
    /* Codes with no handler in this image answer minus 101. */
    g_assert_cmpuint(general(s, 0x2000, NULL, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9b);
    g_assert_cmpuint(general(s, 0x4001, NULL, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9b);
    qtest_quit(s);
}

/* A synthetic board serial must survive either local task-6 route unchanged. */
static void test_brick_serial(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -S "
                               "-device sgi-l1,id=l1c,rack=1,bay=1,"
                               "brick-serial=T4ST42" UART);
    uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 4, 0 };
    const uint32_t addresses[] = { 0x0ffff006, 0x00041006 };
    /* RFC 1662 reflected FCS: 0xffff seed, complement, low byte first. */
    const uint8_t wire[] = { 0x7e, 0x30, 0, 0, 0, 0, 1, 1,
                             'T', '4', 'S', 'T', '4', '2', 0,
                             0x0c, 0x73, 0x7e };

    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    for (unsigned i = 0; i < G_N_ELEMENTS(addresses); i++) {
        set_destination(request, addresses[i]);
        send_frame(s, request, sizeof(request), false);
        expect_wire(s, wire, sizeof(wire));
    }
    qtest_quit(s);
}

/* Action and setting requests answer the fieldless success, or their error. */
static void test_general_actions(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", "-no-shutdown " RTC);
    uint8_t reply[129], args[10];

    console_expect(fd, POWERING ENTER_CPU0);
    qtest_irq_intercept_out_named(s, L1, "node-reset");
    /* 0x1009 front-panel control: two ints, selector at most 2. */
    integer(args, 1);
    integer(args + 5, 1);
    g_assert_cmpuint(general(s, 0x1009, args, 10, 2, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    integer(args + 5, 3);
    g_assert_cmpuint(general(s, 0x1009, args, 10, 2, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x97);
    g_assert_cmpuint(general(s, 0x1009, args, 5, 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9c);
    /* 0x100b PROM version: exactly three ints; 0x100c: exactly two. */
    integer(args, 6);
    integer(args + 5, 210);
    g_assert_cmpuint(general(s, 0x100b, args, 10, 2, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9c);
    {
        uint8_t three[15];

        integer(three, 6);
        integer(three + 5, 210);
        integer(three + 10, 3);
        g_assert_cmpuint(general(s, 0x100b, three, 15, 3, reply), ==, 6);
        g_assert_cmphex(reply[4], ==, 0);
        console_send(fd, "\024log\r");
        console_expect(fd, ESCAPE STAMP "L1 booting\r\n"
                           STAMP "auto power up initiated\r\n"
                           STAMP "power up (COMMAND)\r\n"
                           STAMP "N0: PROM 6.d2 (PR 3)\r\n" RETURN_CPU0);
    }
    g_assert_cmpuint(general(s, 0x100c, args, 10, 2, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    g_assert_cmpuint(general(s, 0x100c, args, 5, 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9c);
    /* 0x1007 then 0x100a: the registration is cleared. */
    integer(args, 23);
    g_assert_cmpuint(general(s, 0x1007, args, 5, 1, reply), ==, 6);
    g_assert_cmpuint(general(s, 0x100a, NULL, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    {
        g_autoptr(QDict) result = qtest_qmp(s,
            "{'execute':'qom-get','arguments':"
            "{'path':%s,'property':'event-subchannel'}}", L1);
        g_assert_cmpint(qdict_get_int(result, "return"), ==, 0);
    }
    /* 0x2007, 0x2008: not on this brick type. */
    g_assert_cmpuint(general(s, 0x2007, NULL, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x94);
    /* 0x2002 to 0x2004: PCI slot operations, minus 100 without a handler. */
    integer(args, 0);
    integer(args + 5, 0xff);
    g_assert_cmpuint(general(s, 0x2002, args, 10, 2, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9c);
    /*
     * 0x2001 resets the node, logged as OS. Its reply is queued and then
     * cut off by the reset it triggers, as the contract records; the
     * window comes back empty.
     */
    {
        uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x20, 0x01, 0 };

        send_frame(s, request, sizeof(request), false);
    }
    qtest_qmp_eventwait(s, "RESET");
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    g_assert_cmpuint(qtest_get_irq_raise_count(s, 0), ==, 1);
    g_assert_cmpuint(general(s, 0x000f, NULL, 0, 0, reply), ==, 16);
    g_assert_cmphex(reply[10], ==, 3);
    console_send(fd, "\024log\r");
    console_expect(fd, ESCAPE STAMP "L1 booting\r\n"
                       STAMP "auto power up initiated\r\n"
                       STAMP "power up (COMMAND)\r\n"
                       STAMP "N0: PROM 6.d2 (PR 3)\r\n"
                       STAMP "reset (OS)\r\n" RETURN_CPU0);
    /* 0x2006 while on: a power cycle, down then up, both logged as OS. */
    {
        uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x20, 0x06, 0 };

        send_frame(s, request, sizeof(request), false);
    }
    console_expect(fd, PROMPT "\r\n" ENTER_CPU0);
    qtest_qmp_eventwait(s, "RESET");
    wait_running(s);
    console_send(fd, "\024log\r");
    console_expect(fd, ESCAPE STAMP "L1 booting\r\n"
                       STAMP "auto power up initiated\r\n"
                       STAMP "power up (COMMAND)\r\n"
                       STAMP "N0: PROM 6.d2 (PR 3)\r\n"
                       STAMP "reset (OS)\r\n"
                       STAMP "power down (OS)\r\n"
                       STAMP "power up (OS)\r\n" RETURN_CPU0);
    /* 0x2005 powers the node off: success first, then the shutdown. */
    g_assert_cmpuint(general(s, 0x2005, NULL, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    qtest_qmp_eventwait(s, "SHUTDOWN");
    assert_display(s, "Powered Down");
    console_send(fd, "\024log\r");
    console_expect(fd, ESCAPE STAMP "L1 booting\r\n"
                       STAMP "auto power up initiated\r\n"
                       STAMP "power up (COMMAND)\r\n"
                       STAMP "N0: PROM 6.d2 (PR 3)\r\n"
                       STAMP "reset (OS)\r\n"
                       STAMP "power down (OS)\r\n"
                       STAMP "power up (OS)\r\n"
                       STAMP "power down (OS)\r\n" PROMPT);
    /*
     * 0x2006 power-cycles: from off it is a power-up, whose reset cuts
     * off the reply the way 0x2001's does.
     */
    console_send(fd, "\004");
    console_expect(fd, ENTER_CPU0);
    {
        uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x20, 0x06, 0 };

        send_frame(s, request, sizeof(request), false);
    }
    /* Nobody escaped, so the guest's power-up prints the locked-state form. */
    console_expect(fd, PROMPT "\r\n" ENTER_CPU0);
    qtest_qmp_eventwait(s, "RESET");
    wait_running(s);
    assert_display(s, "Powered Up");
    qtest_quit(s);
    close(fd);
}

/* 0x2006 power-cycles a running node without shutting down QEMU. */
static void test_power_cycle(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", RTC);
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x20, 0x06, 0 };

    console_expect(fd, POWERING ENTER_CPU0);
    send_frame(s, request, sizeof(request), false);
    console_expect(fd, PROMPT "\r\n" ENTER_CPU0);
    qtest_qmp_eventwait(s, "RESET");
    wait_running(s);
    console_send(fd, "\024log\r");
    console_expect(fd, ESCAPE STAMP "L1 booting\r\n"
                       STAMP "auto power up initiated\r\n"
                       STAMP "power up (COMMAND)\r\n"
                       STAMP "power down (OS)\r\n"
                       STAMP "power up (OS)\r\n" RETURN_CPU0);
    qtest_quit(s);
    close(fd);
}

/* Power requests made while off neither shut down QEMU nor power down again. */
static void test_power_requests_off(void)
{
    int fd;
    QTestState *s = console_start(&fd, ",auto-power=off", RTC);
    uint8_t reply[129];
    const uint8_t cycle[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x20, 0x06, 0 };

    qtest_irq_intercept_out_named(s, L1, "node-power");
    console_send(fd, "\r");
    console_expect(fd, PROMPT);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmpuint(qtest_get_irq_raise_count(s, 0), ==, 0);

    /* 0x2005 still succeeds, but changes nothing while power is off. */
    g_assert_cmpuint(general(s, 0x2005, NULL, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmpuint(qtest_get_irq_raise_count(s, 0), ==, 0);
    assert_display(s, "L1 running");
    console_send(fd, "log\r");
    console_expect(fd, STAMP "L1 booting\r\n" PROMPT);

    /* 0x2006 powers up, resets and resumes, with no power-down action. */
    console_send(fd, "\004");
    console_expect(fd, ENTER_CPU0);
    send_frame(s, cycle, sizeof(cycle), false);
    console_expect(fd, PROMPT "\r\n" ENTER_CPU0);
    qtest_qmp_eventwait(s, "RESET");
    wait_running(s);
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_cmpuint(qtest_get_irq_raise_count(s, 0), ==, 1);
    assert_display(s, "Powered Up");
    console_send(fd, "\024log\r");
    console_expect(fd, ESCAPE STAMP "L1 booting\r\n"
                       STAMP "power up (OS)\r\n" RETURN_CPU0);
    qtest_quit(s);
    close(fd);
}

/* The two display requests write line 2 as the firmware does. */
static void test_display_requests(void)
{
    QTestState *s = start();
    uint8_t reply[131];
    const uint8_t text[] = { 1, 'h', 'i', 0x01, 'x', 0 };
    const uint8_t not_text[] = { 0, 0, 0, 0, 1 };

    g_assert_cmpuint(general(s, 0x1004, text, sizeof(text), 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    assert_display(s, "hi?x");
    g_assert_cmpuint(general(s, 0x1005, text, sizeof(text), 1, reply), ==, 6);
    assert_display(s, "hi?x");
    g_assert_cmpuint(general(s, 0x1005, not_text, sizeof(not_text), 1, reply),
                     ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9a);
    assert_display(s, "hi?x");
    qtest_quit(s);
}

/* 0x4002 restarts the controller and logs the request. */
static void test_reboot_request(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", RTC);
    uint8_t reply[131];

    console_expect(fd, POWERING ENTER_CPU0);
    console_send(fd, "\024display 2 gone\r");
    console_expect(fd, ESCAPE RETURN_CPU0);
    g_assert_cmpuint(general(s, 0x4002, NULL, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    console_send(fd, "\r");
    console_expect(fd, PROMPT);
    assert_display(s, "L1 running");
    console_send(fd, "log\r");
    console_expect(fd, STAMP "L1 booting\r\n"
                       STAMP "auto power up initiated\r\n"
                       STAMP "power up (COMMAND)\r\n"
                       STAMP "L1REQ_REBOOT requested...\r\n"
                       STAMP "L1 booting\r\n" PROMPT);
    qtest_quit(s);
    close(fd);
}

/* The first power-off request while up sends the 0x8000 event and defers. */
static void test_power_down_event(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", "-no-shutdown " RTC);
    uint8_t reply[129], args[5];
    /* Source: rack 1 in bits 27:18, bay 1 in 17:12, task 4. */
    const uint8_t event_head[] = { 0x40 | 23, 0, 4, 0x10, 4, 0, 0, 0x80, 0, 2,
                                   0, 0, 0, 0, 0, 1 };
    const char *expected_text = "\r\n" ID " ATTN: Power Down: issue 'pwr d' "
                                "again to immediately power down.\r\n";

    console_expect(fd, POWERING ENTER_CPU0);
    integer(args, 23);
    g_assert_cmpuint(general(s, 0x1007, args, 5, 1, reply), ==, 6);
    console_send(fd, "\024power down\r");
    console_expect(fd, ESCAPE RETURN_CPU0);
    g_assert_cmpuint(receive(s, reply), ==,
                     sizeof(event_head) + strlen(expected_text) + 1);
    g_assert_cmpmem(reply, sizeof(event_head), event_head, sizeof(event_head));
    g_assert_cmpstr((char *)reply + sizeof(event_head), ==, expected_text);
    assert_display(s, "Powered Up");
    console_send(fd, "\024power down\r");
    console_expect(fd, ESCAPE PROMPT);
    qtest_qmp_eventwait(s, "SHUTDOWN");
    assert_display(s, "Powered Down");
    qtest_quit(s);
    close(fd);
}


int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/l1/uart", test_uart);
    qtest_add_func("sn1/l1/frames", test_frames);
    qtest_add_func("sn1/l1/output-escape-startup",
                   test_output_escape_startup);
    qtest_add_func("sn1/l1/identity", test_identity);
    qtest_add_func("sn1/l1/errors", test_errors);
    qtest_add_func("sn1/l1/reset", test_reset);
    qtest_add_func("sn1/l1/eeprom", test_eeprom);
    qtest_add_func("sn1/l1/eeprom-six-entries", test_eeprom_six_entries);
    qtest_add_func("sn1/l1/eeprom-selected-board", test_selected_board);
    qtest_add_func("sn1/l1/eeprom-selected-spd", test_selected_spd);
    qtest_add_func("sn1/l1/eeprom-selected-raw-conflict",
                   test_selected_raw_conflict);
    qtest_add_func("sn1/l1/board-area-validity", test_board_area_validity);
    qtest_add_func("sn1/l1/migration", test_migration);
    qtest_add_func("sn1/l1/eeprom-migration", test_eeprom_migration);
    qtest_add_func("sn1/l1/eeprom-selected-migration",
                   test_selected_migration);
    qtest_add_func("sn1/l1/console", test_console);
    qtest_add_func("sn1/l1/metadata", test_metadata);
    qtest_add_func("sn1/l1/poweroff", test_poweroff);
    qtest_add_func("sn1/l1/fresh-off", test_fresh_off);
    qtest_add_func("sn1/l1/power", test_power);
    qtest_add_func("sn1/l1/node-reset", test_node_reset);
    qtest_add_func("sn1/l1/signals", test_signals);
    qtest_add_func("sn1/l1/signals-off", test_signals_off);
    qtest_add_func("sn1/l1/node-power-line", test_node_power_line);
    qtest_add_func("sn1/l1/version-metadata", test_version_metadata);
    qtest_add_func("sn1/l1/log-capacity", test_log_capacity);
    qtest_add_func("sn1/l1/reboot-l1", test_reboot_l1);
    qtest_add_func("sn1/l1/selection", test_selection);
    qtest_add_func("sn1/l1/display-command", test_display_command);
    qtest_add_func("sn1/l1/lifecycle-persistence", test_lifecycle_persistence);
    qtest_add_func("sn1/l1/lifecycle-migration", test_lifecycle_migration);
    qtest_add_func("sn1/l1/route-forms", test_route_forms);
    qtest_add_func("sn1/l1/tasks", test_tasks);
    qtest_add_func("sn1/l1/command-interpreter", test_command_interpreter);
    qtest_add_func("sn1/l1/receive-tolerance", test_receive_tolerance);
    qtest_add_func("sn1/l1/request-log", test_request_log);
    qtest_add_func("sn1/l1/console-retarget", test_console_retarget);
    qtest_add_func("sn1/l1/general-reads", test_general_reads);
    qtest_add_func("sn1/l1/brick-serial", test_brick_serial);
    qtest_add_func("sn1/l1/general-actions", test_general_actions);
    qtest_add_func("sn1/l1/power-cycle", test_power_cycle);
    qtest_add_func("sn1/l1/power-requests-off", test_power_requests_off);
    qtest_add_func("sn1/l1/display-requests", test_display_requests);
    qtest_add_func("sn1/l1/reboot-request", test_reboot_request);
    qtest_add_func("sn1/l1/power-down-event", test_power_down_event);
    qtest_add_func("sn1/l1/task-commands", test_task_commands);
    qtest_add_func("sn1/l1/settings-queries", test_settings_queries);
    qtest_add_func("sn1/l1/debug-set", test_debug_set);
    qtest_add_func("sn1/l1/debug-task3", test_debug_task3);
    qtest_add_func("sn1/l1/uart-interrupts", test_uart_interrupts);
    qtest_add_func("sn1/l1/uart-irq-migration", test_uart_irq_migration);
    qtest_add_func("sn1/l1/node-position", test_node_position);
    qtest_add_func("sn1/l1/node-position-migration",
                   test_node_position_migration);
    qtest_add_func("sn1/l1/node-position-range", test_node_position_range);
    qtest_add_func("sn1/l1/uart-unlinked", test_uart_unlinked);
    qtest_add_func("sn1/l1/uart-shared", test_uart_shared);
    return g_test_run();
}
