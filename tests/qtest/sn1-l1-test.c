/* SPDX-License-Identifier: BSD-3-Clause */
/* Isolated L1 protocol checks; this fixture does not execute firmware. */
#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "sgi-migrate.h"
#include "sgi-l1-test.h"
#include <poll.h>
#include <sys/socket.h>

static uint64_t l1_test_base = 0x11000000ULL;
#define BASE l1_test_base

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
    return sgi_l1_test_fcs(v, b);
}

static void put(QTestState *s, uint8_t b)
{
    sgi_l1_test_put(s, BASE, b);
}

static void send_frame(QTestState *s, const uint8_t *p, size_t n, bool corrupt)
{
    sgi_l1_test_send(s, BASE, p, n, corrupt);
}

static size_t receive(QTestState *s, uint8_t *p)
{
    return sgi_l1_test_receive(s, BASE, p, 131);
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
    /*
     * 116 bytes fill the 128-byte frame; 117 answers the declared minus 105.
     */
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
     * Component 6 is beyond the table; an absent component answers minus 104
     * before any range check; area 5 on a present one is minus 105.
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


static QTestState *start_expansion_records(const char *path,
                                          const char *bic1,
                                          const char *extra)
{
    QTestState *s = qtest_initf(
        "-M none -cpu R10000 -S %s -device '{\"driver\":\"sgi-l1\","
        "\"id\":\"l1c\",\"brick-type\":\"pci-expansion\","
        "\"board-record-eeprom\":[\"%s\",\"%s\",\"%s\",\"%s\"]}' "
        "-device '{\"driver\":\"sgi-l1-uart\",\"id\":\"l1\", "
        "\"controller\":\"l1c\"}'",
        extra, path, path, path, bic1);

    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    return s;
}

static QTestState *start_expansion(const char *path, const char *extra)
{
    return start_expansion_records(path, "", extra);
}

static void expect_expansion_identity(QTestState *s)
{
    uint8_t reply[131];
    const uint8_t expected[] = {
        0x30, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0x41, 0, 0, 0, 0, 0x25
    };

    g_assert_cmpuint(general(s, 3, NULL, 0, 0, reply), ==, sizeof(expected));
    g_assert_cmpmem(reply, sizeof(expected), expected, sizeof(expected));
}

static void test_expansion_records(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-expansion-XXXXXX", &error);
    g_autofree char *path = NULL;
    uint8_t image[512] = { 1, 2, 0, 0, 0, 0, 0, 0xfd };
    uint8_t reply[131];
    const uint32_t valid[] = { 1, 0, 0, 16, 8, 1, 1, 0, 0 };
    const uint32_t absent_area[] = { 1, 0, 0, 0, 8, 1, 1, 1, 0 };
    const uint8_t absent[] = { 0x30, 0xff, 0xff, 0xff, 0x98, 0 };
    const uint8_t invalid[] = { 0x30, 0xff, 0xff, 0xff, 0x95, 0 };
    const uint8_t unsupported[] = { 0x30, 0xff, 0xff, 0xff, 0x9c, 0 };
    QTestState *s;

    g_assert_no_error(error);
    path = g_build_filename(dir, "synthetic-board.bin", NULL);
    /* The area ends at byte 32, below the product's configured capacity. */
    image[18] = 4;
    image[31] = 0xfc;
    g_assert_true(g_file_set_contents(path, (char *)image, 32, &error));
    g_assert_no_error(error);
    s = start_expansion(path, "");
    expect_expansion_identity(s);
    for (unsigned component = 0; component < 3; component++) {
        expect_format(s, component, valid, G_N_ELEMENTS(valid));
        g_assert_cmpuint(eeprom_read(s, component, 3, 0, 16, reply), ==, 28);
        g_assert_cmpmem(reply + 12, 16, image + 16, 16);
        /* Board positions never acquire a compute SPD area. */
        g_assert_cmpuint(eeprom_read(s, component, 4, 0, 1, reply), ==, 6);
        g_assert_cmpmem(reply, 6, absent, 6);
    }
    for (unsigned component = 3; component < 6; component++) {
        g_assert_cmpuint(eeprom_read(s, component, 5, 999, 1, reply), ==, 6);
        g_assert_cmpmem(reply, 6, absent, 6);
    }
    for (unsigned code = 0x2002; code <= 0x2004; code++) {
        g_assert_cmpuint(general(s, code, NULL, 0, 0, reply), ==, 6);
        g_assert_cmpmem(reply, 6, unsupported, 6);
    }
    qtest_system_reset(s);
    expect_expansion_identity(s);
    expect_format(s, 2, valid, G_N_ELEMENTS(valid));
    qtest_quit(s);

    /* A checksummed area ending at 512 is rejected by the strict bound. */
    memset(image, 0, sizeof(image));
    memcpy(image, (uint8_t[]){ 1, 62, 0, 0, 0, 0, 0, 0xc1 }, 8);
    memcpy(image + 496, (uint8_t[]){ 1, 2, 0, 0, 0, 0, 0, 0xfd }, 8);
    g_assert_true(g_file_set_contents(path, (char *)image, 512, &error));
    g_assert_no_error(error);
    s = start_expansion(path, "");
    expect_format(s, 2, absent_area, G_N_ELEMENTS(absent_area));
    qtest_quit(s);

    image[7]++;
    g_assert_true(g_file_set_contents(path, (char *)image, 512, &error));
    g_assert_no_error(error);
    s = start_expansion(path, "");
    /* Stored invalid status precedes the offset check. */
    g_assert_cmpuint(eeprom_read(s, 2, 0, 999, 1, reply), ==, 6);
    g_assert_cmpmem(reply, 6, invalid, 6);
    qtest_quit(s);
    unlink(path);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_expansion_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-expansion-migrate-XXXXXX",
                                        &error);
    g_autofree char *socket = NULL, *uri = NULL, *path = NULL;
    const uint8_t image[32] = { 1, 0, 0, 0, 0, 0, 0, 0xff };
    const uint32_t valid[] = { 1, 0, 0, 0, 8, 1, 1, 1, 0 };
    QTestState *from, *to;

    g_assert_no_error(error);
    path = g_build_filename(dir, "synthetic-board.bin", NULL);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    g_assert_true(g_file_set_contents(path, (char *)image, sizeof(image),
                                      &error));
    g_assert_no_error(error);
    from = start_expansion_records(path, path, "");
    to = start_expansion("", "-incoming defer");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
    expect_expansion_identity(to);
    expect_format(to, 2, valid, G_N_ELEMENTS(valid));
    expect_format(to, 3, valid, G_N_ELEMENTS(valid));
    qtest_quit(from);
    qtest_quit(to);
    unlink(path);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}


static void test_expansion_input_refusal(void)
{
    if (g_test_subprocess()) {
        qtest_initf("-M none -cpu R10000 -S -device "
                    "'{\"driver\":\"sgi-l1\",\"brick-type\":"
                    "\"pci-expansion\",%s}'", g_getenv("L1_EXPANSION_INPUT"));
        return;
    }
    {
        const char *inputs[] = {
            "\"spd-record-eeprom\":[\"\",\"\",\"missing\"]",
            "\"board-record-eeprom\":[\"\",\"\",\"\",\"\",\"missing\"]",
            "\"board-eeprom\":[\"missing\"]",
            "\"chassis-eeprom\":[\"missing\"]",
            "\"spd-eeprom\":[\"missing\"]",
        };

        for (unsigned i = 0; i < G_N_ELEMENTS(inputs); i++) {
            g_setenv("L1_EXPANSION_INPUT", inputs[i], TRUE);
            g_test_trap_subprocess(NULL, 0, 0);
            g_test_trap_assert_failed();
            g_test_trap_assert_stderr(
                "*accepts only four selected board records*");
        }
        g_unsetenv("L1_EXPANSION_INPUT");
    }
}


static void test_expansion_oversize_refusal(void)
{
    if (g_test_subprocess()) {
        start_expansion(g_getenv("L1_EXPANSION_FILE"), "");
        return;
    }
    {
        GError *error = NULL;
        g_autofree char *dir = g_dir_make_tmp("sn1-l1-oversize-XXXXXX", &error);
        g_autofree char *path = NULL;
        const uint8_t image[513] = { 1, 0, 0, 0, 0, 0, 0, 0xff };

        g_assert_no_error(error);
        path = g_build_filename(dir, "synthetic-oversize.bin", NULL);
        g_assert_true(g_file_set_contents(path, (char *)image, sizeof(image),
                                          &error));
        g_assert_no_error(error);
        g_setenv("L1_EXPANSION_FILE", path, TRUE);
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*selected EEPROM record length is invalid*");
        g_unsetenv("L1_EXPANSION_FILE");
        unlink(path);
        g_assert_cmpint(rmdir(dir), ==, 0);
    }
}

static void test_expansion_migration_mismatch(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-profile-mismatch-XXXXXX",
                                        &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start_expansion("", "");
    to = qtest_init("-M none -cpu R10000 -S -incoming defer "
                    "-device sgi-l1,id=l1c" UART);
    qtest_set_expected_status(to, EXIT_FAILURE);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    qtest_wait_qemu(to);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_profile_migration(gconstpointer opaque)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-profile-mismatch-XXXXXX",
                                        &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    unsigned selection = GPOINTER_TO_UINT(opaque);
    static const char *const profiles[] = {
        "ip34", "unknown", "ip45", "ip35", "ip37", "pci-expansion",
    };
    const char *source = profiles[selection == G_N_ELEMENTS(profiles) ? 2 :
                                  selection % G_N_ELEMENTS(profiles)];
    const char *destination = selection < G_N_ELEMENTS(profiles) ? source :
        profiles[selection == G_N_ELEMENTS(profiles) ? 1 : 2];
    bool mismatch = selection >= G_N_ELEMENTS(profiles);

    from = qtest_initf("-M none -cpu R10000 -S -device "
                      "sgi-l1,id=l1c,brick-type=%s" UART, source);
    to = qtest_initf("-M none -cpu R10000 -S -incoming defer -device "
                    "sgi-l1,id=l1c,brick-type=%s" UART, destination);
    if (mismatch) {
        qtest_set_expected_status(to, EXIT_FAILURE);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    if (mismatch) {
        qtest_wait_qemu(to);
    } else {
        sgi_wait_migration_status(from, "completed", 30);
        sgi_wait_migration_status(to, "completed", 30);
    }
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket);
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

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-migrate-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 3 };
    uint8_t reply[131];
    uint16_t crc = 0xffff;
    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = qtest_init("-M none -cpu R10000 "
                      "-device sgi-l1,id=l1c,brick-type-code=0x61" UART
                      " -S");
    qtest_sysbus_mmio_map(from, PATH, 0, BASE);
    to = qtest_init("-M none -cpu R10000 "
                    "-device sgi-l1,id=l1c,brick-type-code=0x43" UART
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
    sgi_wait_migration_status(from, "completed", 30);
    /* The source completes when its state is sent, not when it is loaded. */
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(qtest_readq(to, BASE + 0x18), ==, 3);
    g_assert_cmpuint(receive(to, reply), ==, 16);
    g_assert_cmphex(reply[15], ==, 0x61);
    for (unsigned i = 4; i < sizeof(request); i++) {
        crc = fcs(crc, request[i]);
        put(to, request[i]);
    }
    crc = ~crc;
    put(to, crc);
    put(to, crc >> 8);
    qtest_writeq(to, BASE, 0x7e);
    g_assert_cmpuint(receive(to, reply), ==, 16);
    g_assert_cmphex(reply[15], ==, 0x61);
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
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
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
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
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

    /*
     * An event subchannel of 4 or more is non-volatile; a lower one is not.
     */
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
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
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

/*
 * Two chassis's L1s communicate over the NUMAlink cable's RS-422 pair. A
 * request for the peer's own rack and bay, or a type-3 attached-C-brick
 * request, is answered by the peer as itself on the requester's subchannel.
 * Either controller's network mode other than 422 leaves the link unusable
 * and the router error stands. "ctc" runs a command on the peer and prints
 * its brick id first.
 */
static void test_peer_pair(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -S "
                               "-device sgi-l1,id=l1c,rack=1,bay=1" UART
                               " -device sgi-l1,id=l1d,rack=1,bay=2");
    uint8_t request[] = { 0x12, 0, 0, 0, 0, 0, 3 };
    const uint8_t command[] = { 0x11, 0x0f, 0xff, 0xf0, 3, 0, 0, 1,
                                1, 'c', 't', 'c', ' ', 'v', 'e', 'r', 0 };
    const char peer_id[] = "001c02:";
    uint8_t reply[131];
    size_t n;

    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    /* Unlinked: a peer's address is foreign. */
    set_destination(request, (1 << 18) | (2 << 12) | 6);
    send_frame(s, request, sizeof(request), false);
    expect_router_error(s, 18);

    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'peer','value':'/machine/peripheral/l1d'}}",
        L1);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':"
        "{'path':'/machine/peripheral/l1d','property':'peer','value':%s}}",
        L1);
    send_frame(s, request, sizeof(request), false);
    expect_identity(s, 18, 0x42);
    set_destination(request, 0x3ffff006);
    send_frame(s, request, sizeof(request), false);
    expect_identity(s, 18, 0x42);
    /* The requester's own address is still its own. */
    set_destination(request, 0x0ffff006);
    send_frame(s, request, sizeof(request), false);
    expect_identity(s, 18, 0x41);

    send_frame(s, command, sizeof(command), false);
    n = receive(s, reply);
    g_assert_cmpuint(n, >, 6 + sizeof(peer_id));
    g_assert_cmphex(reply[5], >=, 2);
    g_assert_cmphex(reply[6], ==, 1);
    g_assert_cmpmem(reply + 7, sizeof(peer_id), peer_id, sizeof(peer_id));

    qtest_quit(s);

    s = qtest_init("-M none -cpu R10000 -S "
                   "-device sgi-l1,id=l1c,rack=1,bay=1" UART
                   " -device sgi-l1,id=l1d,rack=1,bay=2,network-422=off");
    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'peer','value':'/machine/peripheral/l1d'}}",
        L1);
    set_destination(request, 0x3ffff006);
    send_frame(s, request, sizeof(request), false);
    expect_router_error(s, 18);
    qtest_quit(s);
}

#define PEER "/machine/peripheral/l1d"

static void link_peers(QTestState *s)
{
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'peer','value':%s}}", L1, PEER);
    qtest_qmp_assert_success(s, "{'execute':'qom-set','arguments':"
        "{'path':%s,'property':'peer','value':%s}}", PEER, L1);
}

static void assert_peer_display(QTestState *s, const char *expected)
{
    g_autoptr(QDict) result = qtest_qmp(s,
        "{'execute':'qom-get','arguments':"
        "{'path':%s,'property':'display-line-2'}}", PEER);
    g_assert_cmpstr(qdict_get_str(result, "return"), ==, expected);
}

/*
 * A broadcast power command reaches the peer only as up or down, and a
 * power-down reaches it only once this chassis has gone down: the first
 * request, waiting for its confirmation, leaves both chassis up.
 */
static void test_peer_power(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", "-no-shutdown " RTC
                                  " -device sgi-l1,id=l1d,rack=1,bay=2");

    console_expect(fd, POWERING ENTER_CPU0);
    link_peers(s);
    assert_peer_display(s, "Powered Up");
    console_send(fd, "\024* power check\r");
    console_expect(fd, ESCAPE "power appears on\r\n" RETURN_CPU0);
    assert_peer_display(s, "Powered Up");
    console_send(fd, "\024* power down\r");
    console_expect(fd, ESCAPE RETURN_CPU0);
    assert_display(s, "Powered Up");
    assert_peer_display(s, "Powered Up");
    console_send(fd, "\024* power down\r");
    console_expect(fd, ESCAPE PROMPT);
    qtest_qmp_eventwait(s, "SHUTDOWN");
    assert_display(s, "Powered Down");
    assert_peer_display(s, "Powered Down");
    qtest_quit(s);
    close(fd);
}

/*
 * A command relayed to the peer with ctc cannot be relayed back: the peer
 * refuses a nested ctc and the requester's own collection survives, so a
 * later request still answers.
 */
static void test_peer_nested_ctc(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -S "
                               "-device sgi-l1,id=l1c,rack=1,bay=1" UART
                               " -device sgi-l1,id=l1d,rack=1,bay=2");
    const uint8_t nested[] = { 0x30, 0xff, 0xff, 0xff, 0xff, 2,
                               1, '0', '0', '1', 'c', '0', '2', ':', 0,
                               1, 'I', 'N', 'F', 'O', ':', ' ', 'n', 'e',
                               's', 't', 'e', 'd', ' ', 'c', 't', 'c', ' ',
                               'i', 's', ' ', 'u', 'n', 's', 'u', 'p', 'p',
                               'o', 'r', 't', 'e', 'd', '.', '\n', 0 };
    uint8_t request[128], reply[129];
    size_t n;

    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    link_peers(s);
    text_request(request, &n, 0, "ctc ctc version");
    send_frame(s, request, n, false);
    g_assert_cmpuint(receive(s, reply), ==, sizeof(nested));
    g_assert_cmpmem(reply, sizeof(nested), nested, sizeof(nested));
    text_request(request, &n, 0, "ctc version");
    send_frame(s, request, n, false);
    n = receive(s, reply);
    g_assert_cmphex(reply[0], ==, 0x30);
    g_assert_cmphex(reply[4], ==, 0);
    g_assert_cmpmem(reply + 7, 8, "001c02:", 8);
    qtest_quit(s);
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


static void assert_expansion_idle(QTestState *s)
{
    g_autoptr(QDict) reset = NULL, shutdown = NULL, stop = NULL, resume = NULL;

    g_assert_cmpstr(status(s), ==, "prelaunch");
    reset = qtest_qmp_event_ref(s, "RESET");
    shutdown = qtest_qmp_event_ref(s, "SHUTDOWN");
    stop = qtest_qmp_event_ref(s, "STOP");
    resume = qtest_qmp_event_ref(s, "RESUME");
    g_assert_null(reset);
    g_assert_null(shutdown);
    g_assert_null(stop);
    g_assert_null(resume);
    assert_display(s, "L1 running");
    g_assert_false(qtest_get_irq(s, 0));
    g_assert_cmpuint(qtest_get_irq_raise_count(s, 0), ==, 0);
}

static bool reject_expansion_lifecycle_event(QTestState *s, const char *name,
                                             QDict *event, void *opaque)
{
    g_assert_cmpstr(name, !=, "RESET");
    g_assert_cmpstr(name, !=, "SHUTDOWN");
    g_assert_cmpstr(name, !=, "STOP");
    g_assert_cmpstr(name, !=, "RESUME");
    return false;
}

static void check_expansion_lifecycle_refusal(const char *pin)
{
    int fd;
    QTestState *s = console_start(&fd, ",brick-type=pci-expansion", "");
    uint8_t reply[131], args[5], request[128];
    const unsigned codes[] = { 0x2001, 0x2005, 0x2006, 0x2007, 0x2008 };
    const char *commands[] = {
        "power up", "power down", "pwr u", "pwr d", "* pwr d",
        "reset", "rst", "* reset", "softreset", "softrst", "nmi",
        "select", "sel subchannel 1",
    };
    const uint8_t unsupported[] = { 0x30, 0xff, 0xff, 0xff, 0x9b, 0 };
    const uint8_t no_processors[] = { 0x30, 0xff, 0xff, 0xff, 0x94, 0 };
    const char *warning = "INFO: compute service is unsupported on "
                          "expansion profile.\r\n";

    qtest_irq_intercept_out_named(s, L1, pin);
    qtest_qmp_set_event_callback(s, reject_expansion_lifecycle_event, NULL);
    assert_expansion_idle(s);
    console_send(fd, "\r");
    console_expect(fd, PROMPT);
    g_assert_cmpuint(general(s, 0x10, NULL, 0, 0, reply), ==, 6);
    g_assert_cmpmem(reply, 6, no_processors, 6);
    for (unsigned i = 0; i < G_N_ELEMENTS(codes); i++) {
        g_assert_cmpuint(general(s, codes[i], NULL, 0, 0, reply), ==, 6);
        g_assert_cmpmem(reply, 6, unsupported, 6);
        assert_expansion_idle(s);
    }
    integer(args, 1);
    g_assert_cmpuint(general(s, 0x1002, args, sizeof(args), 1, reply), ==, 6);
    g_assert_cmpmem(reply, 6, unsupported, 6);
    for (unsigned i = 0; i < G_N_ELEMENTS(commands); i++) {
        size_t n;
        g_autofree char *line = g_strdup_printf("%s\r", commands[i]);
        g_autofree char *expected = g_strconcat(warning, PROMPT, NULL);

        console_send(fd, line);
        console_expect(fd, expected);
        assert_expansion_idle(s);
        text_request(request, &n, 0, commands[i]);
        send_frame(s, request, n, false);
        g_assert_cmpuint(receive(s, reply), >, 7);
        g_assert_cmphex(reply[1], ==, 0xff);
        g_assert_cmphex(reply[4], ==, 0xff);
        g_assert_cmphex(reply[5], ==, 1);
        g_assert_cmphex(reply[6], ==, 1);
        g_assert_cmpstr((char *)reply + 7, ==,
            "INFO: compute service is unsupported on expansion profile.");
        assert_expansion_idle(s);
    }
    /* Ctrl-D cannot enter a processor console. */
    console_send(fd, "\004");
    {
        g_autofree char *expected = g_strconcat(warning, PROMPT, NULL);

        console_expect(fd, expected);
    }
    for (unsigned task = 5; task <= 21; task += 16) {
        size_t n;

        text_request(request, &n, 0, "x");
        set_destination(request, 0x0ffff000 | task);
        send_frame(s, request, n, false);
        g_assert_cmpuint(receive(s, reply), ==, 6);
        g_assert_cmpmem(reply, 6, unsupported, 6);
    }
    {
        const uint8_t output[] = { 0x44, 'x' };

        send_frame(s, output, sizeof(output), false);
        g_assert_cmpuint(general(s, 0x000e, NULL, 0, 0, reply), ==, 11);
        g_assert_cmphex(reply[10], ==, 0);
    }
    assert_expansion_idle(s);
    qtest_qmp_set_event_callback(s, NULL, NULL);
    qtest_system_reset(s);
    assert_expansion_idle(s);
    expect_expansion_identity(s);
    qtest_quit(s);
    close(fd);
}

static void test_expansion_lifecycle_refusal(void)
{
    const char *pins[] = { "node-power", "node-reset", "soft-reset", "nmi" };

    for (unsigned i = 0; i < G_N_ELEMENTS(pins); i++) {
        check_expansion_lifecycle_refusal(pins[i]);
    }
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

    /*
     * One or two bytes is a short frame; three or four bytes of a request
     * header fail the check. All are dropped and a good frame follows.
     */
    for (unsigned length = 1; length <= 4; length++) {
        uint8_t frame[4] = { 0x10, 0x0f, 0xff, 0xf0 };
        qtest_writeq(s, BASE, 0x7e);
        raw_bytes(s, frame, length);
        qtest_writeq(s, BASE, 0x7e);
        g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
        send_frame(s, identity, sizeof(identity), false);
        expect_identity(s, 16, 0x41);
    }
    /* A well-checked header alone is a request with no destination. */
    crc = ~fcs(0xffff, 0x10);
    qtest_writeq(s, BASE, 0x7e);
    qtest_writeq(s, BASE, 0x10);
    put(s, crc);
    put(s, crc >> 8);
    qtest_writeq(s, BASE, 0x7e);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    /*
     * An escape before the closing flag drops the frame; that flag opens the
     * next one, so the next frame needs no flag of its own.
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

/*
 * The PROM echoes each keystroke, pads and breaks lines one character per
 * frame: a header, one byte and the check sequence, four bytes between the
 * flags. A header alone with its check is three.
 */
static void test_console_short_frames(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", "");
    const uint8_t echo[] = { 0x44, '1' };
    const uint8_t newline[] = { 0x44, '\r' };
    const uint8_t header_only[] = { 0x40 };
    const uint8_t text[] = { 0x40, 'O', 'K' };
    uint8_t reply[131];

    console_expect(fd, POWERING ENTER_CPU0);
    send_frame(s, echo, sizeof(echo), false);
    console_expect(fd, "1");
    send_frame(s, newline, sizeof(newline), false);
    console_expect(fd, "\r");
    /* Output on subchannel 4 retargets typed input there. */
    console_send(fd, "x");
    console_frame(s, reply, 2);
    g_assert_cmphex(reply[0], ==, 4);
    g_assert_cmphex(reply[1], ==, 'x');
    /* A header alone carries no text; it still retargets input. */
    send_frame(s, header_only, sizeof(header_only), false);
    send_frame(s, text, sizeof(text), false);
    console_expect(fd, "OK");
    console_send(fd, "y");
    console_frame(s, reply, 2);
    g_assert_cmphex(reply[0], ==, 0);
    g_assert_cmphex(reply[1], ==, 'y');
    /* One or two bytes between flags is a short frame, still dropped. */
    qtest_writeq(s, BASE, 0x7e);
    raw_bytes(s, echo, 1);
    qtest_writeq(s, BASE, 0x7e);
    raw_bytes(s, echo, 2);
    qtest_writeq(s, BASE, 0x7e);
    send_frame(s, text, sizeof(text), false);
    console_expect(fd, "OK");
    qtest_quit(s);
    close(fd);
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
        sgi_wait_migration_status(from, "completed", 30);
        sgi_wait_migration_status(to, "completed", 30);
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
    /*
     * A node reset does not reach the controller; a controller restart does.
     */
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
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
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

/*
 * The read-only rows the firmware serves that the model answers from state.
 */
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

static void fuel_mac_checksum(uint8_t *image)
{
    unsigned sum = 0;

    image[32] = 0;
    image[0] = 0;
    for (unsigned i = 32; i < 32 + image[33] * 8; i++) {
        sum += image[i];
    }
    image[32] = -sum;
    sum = 0;
    for (unsigned i = 0; i < 8; i++) {
        sum += image[i];
    }
    image[0] = -sum;
}

static void fuel_mac_reject(const char *path, const uint8_t *image,
                            size_t length)
{
    GError *error = NULL;

    g_assert_true(g_file_set_contents(path, (const char *)image, length,
                                      &error));
    g_assert_no_error(error);
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*Fuel MAC EEPROM record is malformed*");
}

static void test_fuel_mac_eeprom(void)
{
    static const char mac[] = "02A1B2C3D4E5";
    uint8_t image[513] = { 0 }, reply[129];
    g_autofree char *path = NULL;
    GError *error = NULL;
    QTestState *s;
    int fd;

    if (g_test_subprocess()) {
        qtest_initf("-M none -cpu R10000 -S -device "
                    "sgi-l1,id=l1c,fuel-mac-eeprom=%s",
                    g_getenv("L1_FUEL_MAC_PATH"));
        return;
    }
    fd = g_file_open_tmp("l1-fuel-mac-XXXXXX.bin", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    image[3] = 4;
    image[33] = 16;
    image[39] = 4;
    memcpy(image + 40, "MAC ", 4);
    image[44] = 1;
    image[45] = 'X';
    image[46] = 1;
    image[47] = 'Y';
    image[49] = 1;
    image[50] = 'Z';
    image[53] = 1;
    image[54] = 'Q';
    image[70] = 12;
    memcpy(image + 71, mac, 12);
    fuel_mac_checksum(image);
    g_assert_true(g_file_set_contents(path, (char *)image, 512, &error));
    g_assert_no_error(error);
    s = qtest_initf("-M none -cpu R10000 -S -device "
                    "sgi-l1,id=l1c,fuel-mac-eeprom=%s" UART, path);
    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    g_assert_cmpuint(general(s, 8, NULL, 0, 0, reply), ==, 20);
    g_assert_cmpmem(reply + 7, 12, mac, 12);
    qtest_quit(s);

    g_setenv("L1_FUEL_MAC_PATH", path, true);
    fuel_mac_reject(path, image, 0);
    fuel_mac_reject(path, image, 511);
    image[512] = 0xa5;
    fuel_mac_reject(path, image, sizeof(image));
    image[71] = 'G';
    fuel_mac_checksum(image);
    fuel_mac_reject(path, image, 512);
    image[71] = '0';
    image[39] = 63;
    image[33] = 8;
    fuel_mac_checksum(image);
    fuel_mac_reject(path, image, 512);
    image[39] = 4;
    image[33] = 60;
    fuel_mac_checksum(image);
    fuel_mac_reject(path, image, 512);
    image[33] = 16;
    fuel_mac_checksum(image);
    image[0]++;
    fuel_mac_reject(path, image, 512);
    g_assert_cmpint(unlink(path), ==, 0);
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*Fuel MAC EEPROM:*");
    g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*Fuel MAC EEPROM:*");
    g_assert_cmpint(g_rmdir(path), ==, 0);
    g_unsetenv("L1_FUEL_MAC_PATH");
}

/*
 * A synthetic board serial must survive either local task-6 route unchanged.
 */
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

/*
 * Action and setting requests answer the fieldless success, or their error.
 */
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
     * 0x2001 resets the node, logged as OS. Its reply is queued and then cut
     * off by the reset it triggers, as the contract records; the window comes
     * back empty.
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
     * 0x2006 power-cycles: from off it is a power-up, whose reset cuts off
     * the reply the way 0x2001's does.
     */
    console_send(fd, "\004");
    console_expect(fd, ENTER_CPU0);
    {
        uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x20, 0x06, 0 };

        send_frame(s, request, sizeof(request), false);
    }
    /*
     * Nobody escaped, so the guest's power-up prints the locked-state form.
     */
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

/*
 * Power requests made while off neither shut down QEMU nor power down again.
 */
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


#define DEFAULT_STATE "config=1,ep0=2,out1=1,in2=2"
#define DEVICE_HEX "12010001ffff00405e063412000101020301"
#define TREE_HEX "09022000010104600009040000020000ff05" \
                 "0705010240000007058202400000"

static QDict *set(QTestState *s, const char *property, const char *value)
{
    return qtest_qmp(s, "{'execute':'qom-set','arguments':"
                       "{'path':%s,'property':%s,'value':%s}}",
                       L1, property, value);
}

static char *get(QTestState *s, const char *property)
{
    g_autoptr(QDict) reply = qtest_qmp(s,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        L1, property);

    g_assert_true(qdict_haskey(reply, "return"));
    return g_strdup(qdict_get_str(reply, "return"));
}

static void expect_set(QTestState *s, const char *property,
                       const char *value, bool success)
{
    g_autoptr(QDict) reply = set(s, property, value);

    g_assert_cmpint(qdict_haskey(reply, "return"), ==, success);
    g_assert_cmpint(qdict_haskey(reply, "error"), !=, success);
}

static void expect_result(QTestState *s, const char *expected)
{
    g_autofree char *actual = get(s, "usb-test-result");

    g_assert_cmpstr(actual, ==, expected);
}

static void expect_state(QTestState *s, const char *expected)
{
    g_autofree char *actual = get(s, "usb-test-cached-state-readback");

    g_assert_cmpstr(actual, ==, expected);
}

static void expect_descriptor(QTestState *s, const char *expected)
{
    g_autofree char *actual = get(s, "usb-test-descriptor-result");

    g_assert_cmpstr(actual, ==, expected);
}

static void test_usb_matrix(void)
{
    static const struct {
        const char *state;
        const char *setup;
        const char *result;
    } cases[] = {
        { DEFAULT_STATE, "8000000000000200", "prepared:0100" },
        { DEFAULT_STATE, "8100000000000200", "prepared:0000" },
        { DEFAULT_STATE, "8200000000000200", "prepared:0000" },
        { "config=1,ep0=1,out1=1,in2=2", "8200000080000200", "prepared:0100" },
        { DEFAULT_STATE, "8200000001000200", "prepared:0100" },
        { "config=0,ep0=2,out1=1,in2=2", "8200000001000200", "prepared:0100" },
        { "config=1,ep0=2,out1=2,in2=2", "8200000001000200", "prepared:0000" },
        { DEFAULT_STATE, "8200000082000200", "prepared:0000" },
        { "config=1,ep0=2,out1=1,in2=1", "8200000082000200", "prepared:0100" },
        { DEFAULT_STATE, "8200000081000200", "none" },
        { DEFAULT_STATE, "8200000002000200", "none" },
        { DEFAULT_STATE, "8200000003000200", "none" },
        { DEFAULT_STATE, "8000000000000000", "prepared:" },
        { DEFAULT_STATE, "8000000000000100", "prepared:01" },
        { DEFAULT_STATE, "8100000000000100", "prepared:00" },
        { DEFAULT_STATE, "8000000000000300", "prepared:0100" },
    };
    QTestState *s = start();
    g_autoptr(QDict) listing = qtest_qmp(s,
        "{'execute':'qom-list','arguments':{'path':%s}}", L1);
    QList *entries = qdict_get_qlist(listing, "return");
    unsigned found = 0;

    for (const QListEntry *entry = qlist_first(entries); entry;
         entry = qlist_next(entry)) {
        QDict *item = qobject_to(QDict, qlist_entry_obj(entry));
        const char *name = qdict_get_str(item, "name");

        if (g_str_has_prefix(name, "usb-test-")) {
            found++;
        }
    }
    g_assert_cmpuint(found, ==, 6);
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        expect_set(s, "usb-test-cached-state", cases[i].state, true);
        expect_state(s, cases[i].state);
        expect_set(s, "usb-test-setup", cases[i].setup, true);
        expect_result(s, cases[i].result);
    }
    qtest_quit(s);
}

static void test_usb_descriptor_matrix(void)
{
    static const struct {
        const char *setup;
        const char *result;
    } cases[] = {
        { "8006000100000000", "prepared:" },
        { "8006000100000100", "prepared:12" },
        { "8006000100001200", "prepared:" DEVICE_HEX },
        { "8006000100001300", "prepared:" DEVICE_HEX },
        { "8006000200000000", "prepared:" },
        { "8006000200000100", "prepared:09" },
        { "8006000200000900", "prepared:090220000101046000" },
        { "8006000200000a00", "prepared:09022000010104600009" },
        { "8006000200001200", "prepared:09022000010104600009040000020000ff05" },
        { "8006000200001300", "prepared:090220000101046000"
                              "09040000020000ff0507" },
        { "8006000200001900", "prepared:090220000101046000"
                              "09040000020000ff0507050102400000" },
        { "8006000200001a00", "prepared:090220000101046000"
                              "09040000020000ff050705010240000007" },
        { "8006000200002000", "prepared:" TREE_HEX },
        { "8006000200002100", "prepared:" TREE_HEX },
        { "8006000200000A00", "prepared:09022000010104600009" },
    };
    QTestState *s = start();

    expect_descriptor(s, "none");
    expect_set(s, "usb-test-cached-state", DEFAULT_STATE, true);
    expect_set(s, "usb-test-setup", "8000000000000200", true);
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        expect_set(s, "usb-test-descriptor-setup", cases[i].setup, true);
        expect_descriptor(s, cases[i].result);
        expect_state(s, DEFAULT_STATE);
        expect_result(s, "prepared:0100");
    }
    qtest_quit(s);
}

static void test_usb_descriptor_errors_reset(void)
{
    static const char *invalid[] = {
        "80060002000020", "800600020000200000", "80060002000020gg",
        "0006000200002000", "8106000200002000", "8005000200002000",
        "8006000300002000", "8006000900002000",
        "8006010100002000", "8006010200002000",
        "8006000201002000", "8006000200012000",
        "8006000309002000", "8006000300002000",
        "8006000309042000",
    };
    QTestState *s = start();

    expect_set(s, "usb-test-cached-state", DEFAULT_STATE, true);
    expect_set(s, "usb-test-setup", "8000000000000200", true);
    expect_set(s, "usb-test-descriptor-setup", "8006000200002000", true);
    for (unsigned i = 0; i < G_N_ELEMENTS(invalid); i++) {
        expect_set(s, "usb-test-descriptor-setup", invalid[i], false);
        expect_descriptor(s, "prepared:" TREE_HEX);
        expect_state(s, DEFAULT_STATE);
        expect_result(s, "prepared:0100");
    }
    qtest_system_reset(s);
    expect_descriptor(s, "none");
    expect_state(s, "unset");
    expect_result(s, "none");
    qtest_quit(s);
}

static void test_usb_errors_reset(void)
{
    static const char *bad_setup[] = {
        "80000000000002", "800000000000020000", "80000000000002gg",
        "8001000000000200", "8000000100000200", "8000000000010200",
        "8200000001010200", "0000000000000200",
    };
    static const char *bad_state[] = {
        "config=2,ep0=2,out1=1,in2=2", "config=1,ep0=0,out1=1,in2=2",
        "config=1,ep0=2,out1=1,in2=3", "config=1, ep0=2,out1=1,in2=2",
        "config=1,ep0=2,out1=1,in2=2,extra=0",
    };
    QTestState *s = start();

    expect_state(s, "unset");
    expect_result(s, "none");
    expect_set(s, "usb-test-setup", "8000000000000200", false);
    expect_set(s, "usb-test-cached-state", DEFAULT_STATE, true);
    expect_set(s, "usb-test-setup", "8000000000000200", true);
    for (unsigned i = 0; i < G_N_ELEMENTS(bad_setup); i++) {
        expect_set(s, "usb-test-setup", bad_setup[i], false);
        expect_result(s, "prepared:0100");
        expect_state(s, DEFAULT_STATE);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(bad_state); i++) {
        expect_set(s, "usb-test-cached-state", bad_state[i], false);
        expect_result(s, "prepared:0100");
        expect_state(s, DEFAULT_STATE);
    }
    qtest_system_reset(s);
    expect_state(s, "unset");
    expect_result(s, "none");
    expect_set(s, "usb-test-setup", "8000000000000200", false);
    qtest_quit(s);
}

static void test_usb_software_migration(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *dir = g_dir_make_tmp("sn1-l1-usb-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start();
    to = qtest_init("-M none -cpu R10000 -S -incoming defer "
                    "-device sgi-l1,id=l1c" UART);
    expect_set(from, "usb-test-cached-state", DEFAULT_STATE, true);
    expect_set(from, "usb-test-setup", "8000000000000200", true);
    expect_set(from, "usb-test-descriptor-setup", "8006000200002000", true);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
    expect_state(to, "unset");
    expect_result(to, "none");
    expect_descriptor(to, "none");
    expect_set(to, "usb-test-setup", "8000000000000200", false);
    expect_set(to, "usb-test-descriptor-setup", "8006000100001200", true);
    expect_descriptor(to, "prepared:" DEVICE_HEX);
    qtest_quit(from);
    qtest_quit(to);
    g_assert_cmpint(rmdir(dir), ==, 0);
}



#define QUERY_FIXTURE_BASE 0x12000000ULL
#define QUERY_TOPOLOGY 0x33cc

static void query_uart(unsigned owner)
{
    l1_test_base = 0x11000000ULL + owner * 0x1000;
}

static QTestState *query_start_config(char **paths, const char *const *serials,
                                      uint32_t topology,
                                      uint64_t last_identity,
                                      const char *attachments,
                                      const char *extra)
{
    GString *args = g_string_new("-M none -cpu R10000 -S ");
    QTestState *s;

    for (unsigned i = 0; i < 4; i++) {
        g_string_append_printf(args,
            "-device '{\"driver\":\"sgi-l1\",\"id\":\"qc%u\","
            "\"rack\":1,\"bay\":%u,\"auto-power\":false,"
            "\"network-422\":false%s",
            i, i + 1, i >= 2 ? ",\"brick-type\":\"pci-expansion\"" : "");
        if (paths) {
            g_string_append_printf(args,
                ",\"board-record-eeprom\":[\"%s\"]", paths[i]);
        }
        if (serials && serials[i]) {
            g_autofree char *escaped = g_strescape(serials[i], NULL);

            g_string_append_printf(args, ",\"brick-serial\":\"%s\"", escaped);
        }
        g_string_append_printf(args,
            "}' -device '{\"driver\":\"sgi-l1-uart\","
            "\"id\":\"qu%u\",\"controller\":\"qc%u\"}' ", i, i);
    }
    g_string_append_printf(args,
        "-device '{\"driver\":\"sgi-l1-query-routing-fixture\","
        "\"id\":\"query\",\"owner0\":\"qc0\",\"owner1\":\"qc1\","
        "\"owner2\":\"qc2\",\"owner3\":\"qc3\",\"identity0\":101,"
        "\"identity1\":102,\"identity2\":103,\"identity3\":%" PRIu64 ","
        "\"topology\":%u,\"reachable\":%u%s}' %s", last_identity,
        topology, topology, attachments, extra);
    s = qtest_init(args->str);
    g_string_free(args, true);
    for (unsigned i = 0; i < 4; i++) {
        g_autofree char *path = g_strdup_printf("/machine/peripheral/qu%u", i);
        query_uart(i);
        qtest_sysbus_mmio_map(s, path, 0, BASE);
    }
    qtest_sysbus_mmio_map(s, "/machine/peripheral/query", 0,
                          QUERY_FIXTURE_BASE);
    query_uart(0);
    return s;
}

static QTestState *query_start(char **paths, uint32_t topology,
                               uint64_t last_identity, const char *extra)
{
    return query_start_config(paths, NULL, topology, last_identity, "", extra);
}

static QTestState *query_start_attached(char **paths, const char *extra)
{
    return query_start_config(paths, NULL, QUERY_TOPOLOGY, 104,
                              ",\"attached0\":2,\"attached1\":3,"
                              "\"attached2\":0,\"attached3\":1", extra);
}

static void query_send(QTestState *s, unsigned issuer, uint32_t address,
                        uint16_t command, uint8_t channel,
                        const uint8_t *args, unsigned args_len, unsigned count)
{
    uint8_t request[128] = { channel };

    query_uart(issuer);
    set_destination(request, address);
    request[5] = command >> 8;
    request[6] = command;
    request[7] = count;
    if (args_len) {
        memcpy(request + 8, args, args_len);
    }
    send_frame(s, request, 8 + args_len, false);
}

static uint32_t query_address(unsigned owner, unsigned task)
{
    return (1u << 18) | ((owner + 1) << 12) | task;
}

static void query_identity(QTestState *s, unsigned issuer, unsigned serving,
                            uint8_t channel)
{
    uint8_t reply[131];

    query_send(s, issuer, query_address(serving, 6), 3, channel, NULL, 0, 0);
    g_assert_cmpuint(receive(s, reply), ==, 16);
    g_assert_cmphex(reply[0], ==, 0x20 | channel);
    g_assert_cmphex(reply[5], ==, 2);
    g_assert_cmphex(reply[10], ==, 0x41 + serving);
    g_assert_cmphex(reply[15], ==, serving >= 2 ? 0x25 : '4');
}

static void query_empty_except(QTestState *s, unsigned issuer)
{
    for (unsigned i = 0; i < 4; i++) {
        if (i != issuer) {
            query_uart(i);
            g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
        }
    }
    query_uart(issuer);
}

static void query_local_command(QTestState *s, unsigned owner, const char *text)
{
    uint8_t args[128] = { 1 }, reply[131];

    pstrcpy((char *)args + 1, sizeof(args) - 1, text);
    query_send(s, owner, 0x0ffff003, 0, 16, args, strlen(text) + 2, 1);
    g_assert_cmpuint(receive(s, reply), >=, 6);
    g_assert_cmphex(reply[1], ==, 0);
}

static void check_query_records(bool attached)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("l1-query-records-XXXXXX", &error);
    char *paths[4];
    uint8_t images[4][64] = { { 0 } };
    uint8_t args[20], reply[131];
    QTestState *s;

    g_assert_no_error(error);
    for (unsigned i = 0; i < 4; i++) {
        paths[i] = g_strdup_printf("%s/owner%u.bin", dir, i);
        images[i][0] = 1;
        images[i][1] = 2;
        images[i][7] = 0xfd;
        images[i][18] = 4;
        images[i][20] = i + 1;
        images[i][31] = -(4 + i + 1);
        g_assert_true(g_file_set_contents(paths[i], (char *)images[i], 64,
                                          &error));
        g_assert_no_error(error);
    }
    s = attached ? query_start_attached(paths, "") :
                   query_start(paths, QUERY_TOPOLOGY, 104, "");
    for (unsigned issuer = 0; issuer < 4; issuer++) {
        for (unsigned serving = 0; serving < 4; serving++) {
            if ((issuer < 2) == (serving < 2)) {
                continue;
            }
            uint32_t address = attached ? 0x4ffff006 :
                                          query_address(serving, 6);

            if (attached && serving != (issuer + 2) % 4) {
                continue;
            }
            if (attached) {
                query_send(s, issuer, address, 3, issuer * 7 + serving,
                           NULL, 0, 0);
                g_assert_cmpuint(receive(s, reply), ==, 16);
                g_assert_cmphex(reply[0], ==, 0x20 | (issuer * 7 + serving));
                g_assert_cmphex(reply[10], ==, 0x41 + serving);
                g_assert_cmphex(reply[15], ==, serving >= 2 ? 0x25 : '4');
            } else {
                query_identity(s, issuer, serving, issuer * 7 + serving);
            }
            query_empty_except(s, issuer);
            integer(args, 0);
            integer(args + 5, 3);
            integer(args + 10, 0);
            integer(args + 15, 16);
            query_send(s, issuer, address | (attached ? 0 : 0xfe0),
                       6, 23, args, sizeof(args), 4);
            g_assert_cmpuint(receive(s, reply), ==, 28);
            g_assert_cmphex(reply[0], ==, 0x37);
            g_assert_cmpmem(reply + 12, 16, images[serving] + 16, 16);
            query_empty_except(s, issuer);
            query_send(s, issuer, address, 7, 31,
                       args, 5, 1);
            g_assert_cmpuint(receive(s, reply), ==, 51);
            g_assert_cmphex(reply[0], ==, 0x3f);
            g_assert_cmphex(reply[5], ==, 9);
            g_assert_cmphex(reply[10], ==, 1);
            query_empty_except(s, issuer);
            if (attached) {
                query_send(s, issuer, address, 0xc, 11, NULL, 0, 0);
                g_assert_cmpuint(receive(s, reply), ==, 46);
                g_assert_cmphex(reply[0], ==, 0x2b);
                g_assert_cmphex(reply[5], ==, 8);
                for (unsigned i = 6; i < 46; i++) {
                    g_assert_cmphex(reply[i], ==, 0);
                }
                query_empty_except(s, issuer);
            }
        }
    }
    qtest_quit(s);
    query_uart(0);
    for (unsigned i = 0; i < 4; i++) {
        unlink(paths[i]);
        g_free(paths[i]);
    }
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_query_owners(void)
{
    check_query_records(false);
}

static void test_attached_records(void)
{
    check_query_records(true);
}

static void query_expect_serial(QTestState *s, unsigned issuer,
                                 uint8_t channel, const char *serial)
{
    uint8_t reply[131], expected[64] = { 0x20 | channel, 0, 0, 0, 0, 1, 1 };
    unsigned length = strlen(serial) + 8;

    memcpy(expected + 7, serial, strlen(serial) + 1);
    g_assert_cmpuint(receive(s, reply), ==, length);
    g_assert_cmpmem(reply, length, expected, length);
    query_empty_except(s, issuer);
}

static void query_serial(QTestState *s, unsigned issuer, unsigned serving,
                          uint8_t channel, const char *serial,
                          const uint8_t *args, unsigned args_len,
                          unsigned count)
{
    query_send(s, issuer, query_address(serving, 6), 4, channel,
               args, args_len, count);
    query_expect_serial(s, issuer, channel, serial);
}

static void test_query_serial(void)
{
    const char *serials[] = { "compute-A", "compute-B", "expansion-A~}",
                              "expansion-B" };
    const uint8_t extra[] = { 0, 0, 0, 0, 7 };
    QTestState *s = query_start_config(NULL, serials, QUERY_TOPOLOGY,
                                      104, "", "");

    for (unsigned issuer = 0; issuer < 4; issuer++) {
        for (unsigned owner = 0; owner < 2; owner++) {
            unsigned serving = issuer < 2 ? 2 + owner : owner;
            uint8_t channel = (issuer * 7 + owner) & 0x1f;

            query_serial(s, issuer, serving, channel, serials[serving],
                          NULL, 0, 0);
            query_serial(s, issuer, serving, channel ^ 0x1f, serials[serving],
                          extra, sizeof(extra), 1);
        }
    }
    qtest_quit(s);
    query_uart(0);
}

static void test_query_serial_refusals(void)
{
    const char *serials[] = { NULL, "", "expansion-A~}", "expansion-B" };
    const uint8_t malformed[] = { 0x82, 0x44 };
    const uint32_t denied[] = { 0x0007f006, 0x0ffc3006, 0x00049006,
                                 0x4ffff006, 0x3ffff006 };
    uint8_t reply[131];
    QTestState *s = query_start_config(NULL, serials, QUERY_TOPOLOGY,
                                      104, "", "");

    for (unsigned issuer = 2; issuer < 4; issuer++) {
        for (unsigned serving = 0; serving < 2; serving++) {
            uint8_t channel = issuer * 5 + serving;
            uint8_t absent[] = { 0x20 | channel, 0xff, 0xff, 0xff, 0x98, 0 };

            query_send(s, issuer, query_address(serving, 6), 4, channel,
                       NULL, 0, 0);
            g_assert_cmpuint(receive(s, reply), ==, sizeof(absent));
            g_assert_cmpmem(reply, sizeof(absent), absent, sizeof(absent));
            query_empty_except(s, issuer);
        }
    }
    query_send(s, 0, query_address(2, 6), 4, 7,
               malformed, sizeof(malformed), 1);
    {
        const uint8_t invalid[] = { 0x27, 0xff, 0xff, 0xff, 0x9c, 0 };

        g_assert_cmpuint(receive(s, reply), ==, sizeof(invalid));
        g_assert_cmpmem(reply, sizeof(invalid), invalid, sizeof(invalid));
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(denied); i++) {
        query_send(s, 0, denied[i], 4, 11, NULL, 0, 0);
        expect_router_error(s, 11);
        query_empty_except(s, 0);
    }
    query_send(s, 0, query_address(2, 6), 8, 13, NULL, 0, 0);
    expect_router_error(s, 13);
    query_send(s, 0, query_address(2, 3), 4, 13, NULL, 0, 0);
    expect_router_error(s, 13);
    qtest_writeq(s, QUERY_FIXTURE_BASE, QUERY_TOPOLOGY & ~(1 << 2));
    query_send(s, 0, query_address(2, 6), 4, 17, NULL, 0, 0);
    expect_router_error(s, 17);
    qtest_writeq(s, QUERY_FIXTURE_BASE, QUERY_TOPOLOGY);
    qtest_writeq(s, QUERY_FIXTURE_BASE + 8, 1);
    query_send(s, 0, query_address(2, 6), 4, 21, NULL, 0, 0);
    expect_router_error(s, 21);
    qtest_writeq(s, QUERY_FIXTURE_BASE + 8, 0);
    query_serial(s, 0, 2, 21, serials[2], NULL, 0, 0);
    query_local_command(s, 3, "brick slot 3");
    qtest_writeq(s, QUERY_FIXTURE_BASE + 16, 3);
    query_send(s, 0, query_address(2, 6), 4, 5, NULL, 0, 0);
    expect_router_error(s, 5);
    query_local_command(s, 3, "brick slot 4");
    qtest_writeq(s, QUERY_FIXTURE_BASE + 16, 3);
    query_local_command(s, 2, "brick rack 9");
    query_local_command(s, 2, "brick slot 8");
    qtest_writeq(s, QUERY_FIXTURE_BASE + 16, 2);
    query_send(s, 0, query_address(2, 6), 4, 5, NULL, 0, 0);
    expect_router_error(s, 5);
    query_send(s, 0, (9u << 18) | (8u << 12) | 6, 4, 5, NULL, 0, 0);
    query_expect_serial(s, 0, 5, serials[2]);
    g_assert_cmpuint(qtest_readq(s, QUERY_FIXTURE_BASE + 48), ==, 103);
    qtest_quit(s);
    query_uart(0);
}

static void test_query_refusals_reset(void)
{
    QTestState *s = query_start(NULL, QUERY_TOPOLOGY, 104, "");
    const uint16_t commands[] = { 0x2001, 0x2005, 0x2006, 0x1002, 0x1003 };
    const unsigned tasks[] = { 3, 5, 14, 18 };
    const uint32_t addresses[] = { 0x0007f006, 0x0ffc3006, 0x00049006,
                                   0x4ffff006, 0x3ffff006 };
    uint8_t arg[5];

    qtest_qmp_set_event_callback(s, reject_expansion_lifecycle_event, NULL);
    integer(arg, 0x41);
    for (unsigned issuer = 0; issuer < 4; issuer += 2) {
        unsigned serving = issuer ? 0 : 2;
        for (unsigned i = 0; i < G_N_ELEMENTS(commands); i++) {
            query_send(s, issuer, query_address(serving, 6), commands[i], 17,
                       arg, sizeof(arg), 1);
            expect_router_error(s, 17);
        }
        for (unsigned i = 0; i < G_N_ELEMENTS(tasks); i++) {
            query_send(s, issuer, query_address(serving, tasks[i]), 3, 9,
                       arg, sizeof(arg), 1);
            expect_router_error(s, 9);
        }
        query_empty_except(s, issuer);
    }
    g_assert_cmpstr(status(s), ==, "prelaunch");
    for (unsigned i = 0; i < 4; i++) {
        g_autofree char *path = g_strdup_printf("/machine/peripheral/qc%u", i);
        g_autoptr(QDict) display = qtest_qmp(s,
            "{'execute':'qom-get','arguments':{'path':%s,"
            "'property':'display-line-2'}}", path);
        g_assert_cmpstr(qdict_get_str(display, "return"), ==, "L1 running");
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(addresses); i++) {
        query_send(s, 0, addresses[i], 3, 4, NULL, 0, 0);
        expect_router_error(s, 4);
    }
    query_send(s, 0, 0x0ffff004, 3, 2, NULL, 0, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    query_identity(s, 0, 0, 0);
    qtest_writeq(s, QUERY_FIXTURE_BASE, QUERY_TOPOLOGY & ~(1 << 2));
    query_send(s, 0, query_address(2, 6), 3, 7, NULL, 0, 0);
    expect_router_error(s, 7);
    qtest_writeq(s, QUERY_FIXTURE_BASE + 16, 0);
    g_assert_cmphex(qtest_readq(s, QUERY_FIXTURE_BASE), ==,
                    QUERY_TOPOLOGY & ~(1 << 2));
    qtest_qmp_set_event_callback(s, NULL, NULL);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(s, QUERY_FIXTURE_BASE), ==, QUERY_TOPOLOGY);
    query_identity(s, 0, 2, 7);
    qtest_writeq(s, QUERY_FIXTURE_BASE + 8, 1);
    query_send(s, 0, query_address(2, 6), 3, 13, NULL, 0, 0);
    expect_router_error(s, 13);
    qtest_writeq(s, QUERY_FIXTURE_BASE + 8, 0);
    query_identity(s, 0, 2, 13);
    qtest_quit(s);
    query_uart(0);
}

static void test_query_coordinates(void)
{
    QTestState *s = query_start(NULL, QUERY_TOPOLOGY, 104, "");
    uint8_t reply[131];

    query_local_command(s, 2, "brick slot 8");
    qtest_writeq(s, QUERY_FIXTURE_BASE + 16, 2);
    query_send(s, 0, query_address(2, 6), 3, 8, NULL, 0, 0);
    expect_router_error(s, 8);
    query_send(s, 0, (1 << 18) | (8 << 12) | 6, 3, 8, NULL, 0, 0);
    g_assert_cmpuint(receive(s, reply), ==, 16);
    g_assert_cmphex(reply[10], ==, 0x48);
    g_assert_cmphex(reply[15], ==, 0x25);
    g_assert_cmpuint(qtest_readq(s, QUERY_FIXTURE_BASE + 48), ==, 103);
    g_assert_cmphex(qtest_readq(s, QUERY_FIXTURE_BASE), ==, QUERY_TOPOLOGY);
    query_local_command(s, 3, "brick slot 8");
    qtest_writeq(s, QUERY_FIXTURE_BASE + 16, 3);
    query_send(s, 0, (1 << 18) | (8 << 12) | 6, 3, 8, NULL, 0, 0);
    expect_router_error(s, 8);
    qtest_quit(s);
    query_uart(0);
}

static void test_query_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("l1-query-migrate-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;
    uint8_t reply[131];

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = query_start(NULL, QUERY_TOPOLOGY, 104, "");
    to = query_start(NULL, QUERY_TOPOLOGY, 104, "-incoming defer");
    query_local_command(from, 2, "brick slot 8");
    qtest_writeq(from, QUERY_FIXTURE_BASE + 16, 2);
    qtest_writeq(from, QUERY_FIXTURE_BASE, QUERY_TOPOLOGY & ~(1 << 3));
    query_send(from, 0, (1 << 18) | (8 << 12) | 6, 3, 19, NULL, 0, 0);
    query_send(from, 1, query_address(3, 6), 3, 29, NULL, 0, 0);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
    query_uart(0);
    g_assert_cmpuint(receive(to, reply), ==, 16);
    g_assert_cmphex(reply[0], ==, 0x33);
    g_assert_cmphex(reply[10], ==, 0x48);
    query_uart(1);
    g_assert_cmpuint(receive(to, reply), ==, 16);
    g_assert_cmphex(reply[0], ==, 0x3d);
    g_assert_cmphex(reply[10], ==, 0x44);
    g_assert_cmphex(qtest_readq(to, QUERY_FIXTURE_BASE), ==,
                    QUERY_TOPOLOGY & ~(1 << 3));
    query_send(to, 0, query_address(3, 6), 3, 3, NULL, 0, 0);
    expect_router_error(to, 3);
    query_send(to, 0, (1 << 18) | (8 << 12) | 6, 3, 3, NULL, 0, 0);
    g_assert_cmpuint(receive(to, reply), ==, 16);
    g_assert_cmphex(reply[10], ==, 0x48);
    qtest_quit(from);
    qtest_quit(to);
    query_uart(0);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_query_migration_mismatch(void)
{
    for (unsigned mismatch = 0; mismatch < 2; mismatch++) {
        GError *error = NULL;
        g_autofree char *dir = g_dir_make_tmp("l1-query-mismatch-XXXXXX",
                                            &error);
        g_autofree char *socket = NULL, *uri = NULL;
        QTestState *from, *to;

        g_assert_no_error(error);
        socket = g_build_filename(dir, "migration.sock", NULL);
        uri = g_strdup_printf("unix:%s", socket);
        from = query_start(NULL, QUERY_TOPOLOGY, 104, "");
        to = query_start(NULL, mismatch ? QUERY_TOPOLOGY & ~(1 << 3) :
                          QUERY_TOPOLOGY, mismatch ? 104 : 105,
                          "-incoming defer");
        qtest_set_expected_status(to, EXIT_FAILURE);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        qtest_qmp_assert_success(from,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        qtest_wait_qemu(to);
        qtest_quit(to);
        qtest_quit(from);
        unlink(socket);
        g_assert_cmpint(rmdir(dir), ==, 0);
    }
    query_uart(0);
}

static void test_attached_identity(void)
{
    QTestState *s = query_start_attached(NULL, "");
    uint8_t reply[131];

    for (unsigned issuer = 0; issuer < 4; issuer++) {
        unsigned serving = (issuer + 2) % 4;
        query_send(s, issuer, 0x4ffff006, 3, issuer * 7, NULL, 0, 0);
        g_assert_cmpuint(receive(s, reply), ==, 16);
        g_assert_cmphex(reply[0], ==, 0x20 | (issuer * 7));
        g_assert_cmphex(reply[10], ==, 0x41 + serving);
        g_assert_cmphex(reply[15], ==, serving >= 2 ? 0x25 : '4');
        query_empty_except(s, issuer);
    }
    qtest_quit(s);
    query_uart(0);
}


static void test_attached_refusals_reset(void)
{
    QTestState *s = query_start_attached(NULL, "");
    const uint16_t denied[] = { 0x2001, 0x2005, 0x2006, 0x1002, 0x1003, 1, 4 };
    const uint32_t addresses[] = { 0x4ffff206, 0x40043006, 0x4ffff003,
                                   0x4ffff005, 0x4ffff00e };
    const uint8_t malformed[] = { 0x82, 0x44 };
    const uint8_t short_format[] = { 1, 0 };
    const uint8_t invalid[] = { 0x37, 0xff, 0xff, 0xff, 0x9c, 0 };
    uint8_t reply[131];

    qtest_qmp_set_event_callback(s, reject_expansion_lifecycle_event, NULL);
    for (unsigned issuer = 0; issuer < 4; issuer++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(denied); i++) {
            query_send(s, issuer, 0x4ffff006, denied[i], 17, NULL, 0, 0);
            expect_router_error(s, 17);
        }
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(addresses); i++) {
        query_send(s, 0, addresses[i], 3, 6, NULL, 0, 0);
        expect_router_error(s, 6);
    }
    query_send(s, 0, query_address(2, 6), 0xc, 6, NULL, 0, 0);
    expect_router_error(s, 6);
    query_send(s, 0, 0x4ffff006, 3, 23, malformed, sizeof(malformed), 1);
    g_assert_cmpuint(receive(s, reply), ==, sizeof(invalid));
    g_assert_cmpmem(reply, sizeof(invalid), invalid, sizeof(invalid));
    query_send(s, 0, 0x4ffff006, 6, 23, NULL, 0, 0);
    g_assert_cmpuint(receive(s, reply), ==, sizeof(invalid));
    g_assert_cmpmem(reply, sizeof(invalid), invalid, sizeof(invalid));
    query_send(s, 0, 0x4ffff006, 7, 23, short_format, sizeof(short_format), 1);
    g_assert_cmpuint(receive(s, reply), ==, sizeof(invalid));
    g_assert_cmpmem(reply, sizeof(invalid), invalid, sizeof(invalid));
    g_assert_cmpstr(status(s), ==, "prelaunch");
    query_empty_except(s, 0);
    qtest_writeq(s, QUERY_FIXTURE_BASE, QUERY_TOPOLOGY & ~(1 << 2));
    query_send(s, 0, 0x4ffff006, 3, 5, NULL, 0, 0);
    expect_router_error(s, 5);
    qtest_writeq(s, QUERY_FIXTURE_BASE + 16, 0);
    query_send(s, 0, 0x4ffff006, 3, 5, NULL, 0, 0);
    expect_router_error(s, 5);
    qtest_qmp_set_event_callback(s, NULL, NULL);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    query_send(s, 0, 0x4ffff006, 3, 5, NULL, 0, 0);
    g_assert_cmpuint(receive(s, reply), ==, 16);
    g_assert_cmphex(reply[10], ==, 0x43);
    g_assert_cmphex(reply[15], ==, 0x25);
    qtest_writeq(s, QUERY_FIXTURE_BASE + 8, 1);
    query_send(s, 0, 0x4ffff006, 3, 9, NULL, 0, 0);
    expect_router_error(s, 9);
    qtest_writeq(s, QUERY_FIXTURE_BASE + 8, 0);
    qtest_quit(s);
    s = query_start(NULL, QUERY_TOPOLOGY, 104, "");
    query_send(s, 0, 0x4ffff006, 3, 8, NULL, 0, 0);
    expect_router_error(s, 8);
    query_identity(s, 0, 0, 8);
    qtest_quit(s);
    query_uart(0);
}

static void test_attached_coordinates(void)
{
    QTestState *s = query_start_attached(NULL, "");
    uint8_t reply[131];

    query_local_command(s, 2, "brick rack 9");
    query_local_command(s, 2, "brick slot 8");
    qtest_writeq(s, QUERY_FIXTURE_BASE + 16, 2);
    query_send(s, 0, 0x4ffff006, 3, 28, NULL, 0, 0);
    g_assert_cmpuint(receive(s, reply), ==, 16);
    g_assert_cmphex(reply[0], ==, 0x3c);
    g_assert_cmphex(reply[9], ==, 2);
    g_assert_cmphex(reply[10], ==, 0x48);
    g_assert_cmphex(reply[15], ==, 0x25);
    query_send(s, 0, query_address(2, 6), 3, 28, NULL, 0, 0);
    expect_router_error(s, 28);
    query_send(s, 0, (9 << 18) | (8 << 12) | 6, 3, 28, NULL, 0, 0);
    g_assert_cmpuint(receive(s, reply), ==, 16);
    g_assert_cmphex(reply[9], ==, 2);
    g_assert_cmphex(reply[10], ==, 0x48);
    g_assert_cmpuint(qtest_readq(s, QUERY_FIXTURE_BASE + 48), ==, 103);
    query_empty_except(s, 0);
    qtest_quit(s);
    query_uart(0);
}

static void test_attached_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("l1-attached-migrate-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;
    uint8_t reply[131];

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = query_start_attached(NULL, "");
    to = query_start_attached(NULL, "-incoming defer");
    query_local_command(from, 2, "brick slot 8");
    qtest_writeq(from, QUERY_FIXTURE_BASE + 16, 2);
    qtest_writeq(from, QUERY_FIXTURE_BASE, QUERY_TOPOLOGY & ~(1 << 7));
    query_send(from, 0, 0x4ffff006, 3, 19, NULL, 0, 0);
    query_send(from, 1, 0x4ffff006, 3, 31, NULL, 0, 0);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 30);
    sgi_wait_migration_status(to, "completed", 30);
    query_uart(0);
    g_assert_cmpuint(receive(to, reply), ==, 16);
    g_assert_cmphex(reply[0], ==, 0x33);
    g_assert_cmphex(reply[10], ==, 0x48);
    query_uart(1);
    expect_router_error(to, 31);
    g_assert_cmphex(qtest_readq(to, QUERY_FIXTURE_BASE), ==,
                    QUERY_TOPOLOGY & ~(1 << 7));
    query_send(to, 1, 0x4ffff006, 3, 12, NULL, 0, 0);
    expect_router_error(to, 12);
    query_send(to, 0, 0x4ffff006, 3, 12, NULL, 0, 0);
    g_assert_cmpuint(receive(to, reply), ==, 16);
    g_assert_cmphex(reply[10], ==, 0x48);
    query_empty_except(to, 0);
    qtest_qmp_assert_success(to, "{'execute':'system_reset'}");
    query_send(to, 1, 0x4ffff006, 3, 12, NULL, 0, 0);
    g_assert_cmpuint(receive(to, reply), ==, 16);
    g_assert_cmphex(reply[10], ==, 0x44);
    qtest_quit(from);
    qtest_quit(to);
    query_uart(0);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_attached_migration_mismatch(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("l1-attached-mismatch-XXXXXX",
                                        &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = query_start_attached(NULL, "");
    to = query_start_config(NULL, NULL, QUERY_TOPOLOGY, 104,
        ",\"attached0\":3,\"attached1\":3,\"attached2\":0,\"attached3\":1",
        "-incoming defer");
    qtest_set_expected_status(to, EXIT_FAILURE);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    qtest_wait_qemu(to);
    qtest_quit(to);
    qtest_quit(from);
    query_uart(0);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_attached_composition_refusal(void)
{
    if (g_test_subprocess()) {
        g_autofree char *selection = g_strdup_printf(
            ",\"attached0\":%s", g_getenv("L1_BAD_ATTACHMENT"));
        QTestState *s = query_start_config(NULL, NULL, QUERY_TOPOLOGY, 104,
                                          selection, "");
        qtest_quit(s);
    } else {
        const char *owners[] = { "0", "1", "4" };
        for (unsigned i = 0; i < G_N_ELEMENTS(owners); i++) {
            g_setenv("L1_BAD_ATTACHMENT", owners[i], true);
            g_test_trap_subprocess(NULL, 0, 0);
            g_test_trap_assert_failed();
            g_test_trap_assert_stderr(i == 1 ? "*no registry association*" :
                                             "*Invalid L1 attached owner*");
        }
        g_unsetenv("L1_BAD_ATTACHMENT");
    }
}

#define BIC_STARTUP_BASE UINT64_C(0x12000000)

static QTestState *bic_start(unsigned population, unsigned failure,
                            bool selected, bool initial, const char *extra)
{
    QTestState *s = qtest_initf(
        "-M none -cpu R10000 -S -device sgi-l1-bic-startup-fixture,id=startup,"
        "population=%u,fail-mask=%u,selected=%s,initial-set=%s "
        "-device sgi-l1-uart,id=startup-uart,"
        "controller=/machine/peripheral/startup/l1 %s",
        population, failure, selected ? "on" : "off",
        initial ? "on" : "off", extra);

    qtest_sysbus_mmio_map(s, "/machine/peripheral/startup", 0,
                          BIC_STARTUP_BASE);
    l1_test_base = 0x11000000ULL;
    qtest_sysbus_mmio_map(s, "/machine/peripheral/startup-uart", 0, BASE);
    return s;
}

static uint64_t bic_read(QTestState *s, unsigned offset)
{
    return qtest_readq(s, BIC_STARTUP_BASE + offset);
}

static void bic_write(QTestState *s, unsigned offset, uint64_t value)
{
    qtest_writeq(s, BIC_STARTUP_BASE + offset, value);
}

static void bic_operations(QTestState *s, unsigned population,
                           uint16_t coordinates)
{
    g_assert_cmpuint(bic_read(s, 0), ==, 2 * population);
    for (unsigned i = 0; i < population; i++) {
        uint64_t destination = ((uint64_t)i << 32) | ((0x8c + 2 * i) << 16);

        g_assert_cmphex(bic_read(s, 16 + 16 * i), ==,
                        destination | 0x1300 | (coordinates >> 8));
        g_assert_cmphex(bic_read(s, 24 + 16 * i), ==,
                        destination | 0x1200 | (coordinates & 0xff));
    }
}

static void test_bic_startup_operations(void)
{
    for (unsigned population = 1; population <= 2; population++) {
        for (unsigned failure = 0; failure < (1u << (2 * population));
             failure++) {
            QTestState *s = bic_start(population, failure, true, true, "");

            bic_operations(s, population, 0x41);
            for (unsigned i = 0; i < population; i++) {
                uint16_t expected = (failure & (1u << (2 * i)) ? 0xab00 : 0) |
                    (failure & (2u << (2 * i)) ? 0xcd : 0x41);

                g_assert_cmphex(bic_read(s, 88 + 8 * i), ==, expected);
            }
            /* The binding cannot change after realization. */
            bic_write(s, 104, 0);
            qtest_quit(s);
        }
    }
    for (unsigned failure = 1; failure <= 2; failure++) {
        QTestState *s = bic_start(1, failure, true, false, "");

        bic_operations(s, 1, 0x41);
        g_assert_cmphex(bic_read(s, 88), ==, UINT64_C(1) << 63);
        bic_write(s, 8, 0);
        bic_write(s, 80, 0);
        g_assert_cmphex(bic_read(s, 88), ==, 0x41);
        qtest_quit(s);
    }
}

static void test_bic_startup_failure_visibility(void)
{
    GError *error = NULL;
    g_autofree char *path = NULL, *log = NULL;
    g_autofree char *quoted = NULL, *extra = NULL;
    QTestState *s;
    int fd = g_file_open_tmp("l1-bic-operation-XXXXXX.txt", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    quoted = g_shell_quote(path);
    extra = g_strdup_printf("-trace enable=sgi_l1_bic_startup_operation "
                            "-d guest_errors -D %s", quoted);
    s = bic_start(1, 3, true, true, extra);
    bic_operations(s, 1, 0x41);
    g_assert_cmphex(bic_read(s, 88), ==, 0xabcd);
    bic_write(s, 8, 0);
    bic_write(s, 80, 0);
    g_assert_cmphex(bic_read(s, 88), ==, 0x41);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(path, &log, NULL, &error));
    g_assert_no_error(error);
    g_assert_nonnull(strstr(log, "sgi-l1: BIC startup operation failed: "
                           "position=0 bus=0 address=0x8c "
                           "command=0x13 value=0x00"));
    g_assert_nonnull(strstr(log, "sgi-l1: BIC startup operation failed: "
                           "position=0 bus=0 address=0x8c "
                           "command=0x12 value=0x41"));
    g_assert_nonnull(strstr(log, "command=0x13 value=0x00 completed=0"));
    g_assert_nonnull(strstr(log, "command=0x12 value=0x41 completed=0"));
    g_assert_nonnull(strstr(log, "command=0x13 value=0x00 completed=1"));
    g_assert_nonnull(strstr(log, "command=0x12 value=0x41 completed=1"));
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_bic_startup_resets(void)
{
    QTestState *s = bic_start(1, 0, true, true, "");
    uint8_t reply[131];

    bic_write(s, 64, 9);
    bic_write(s, 72, 5);
    bic_operations(s, 1, 0x41);
    g_assert_cmphex(bic_read(s, 88), ==, 0x41);
    /* The existing reboot request activates settings without resetting BIC. */
    bic_write(s, 0, 0);
    g_assert_cmpuint(general(s, 0x4002, NULL, 0, 0, reply), ==, 6);
    bic_operations(s, 1, 0x245);
    g_assert_cmphex(bic_read(s, 88), ==, 0x245);
    bic_write(s, 8, 1);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    qtest_qmp_eventwait(s, "RESET");
    bic_operations(s, 1, 0x245);
    /* Reset-enter restored the high byte before the failed startup high. */
    g_assert_cmphex(bic_read(s, 88), ==, 0xab45);
    bic_write(s, 80, 1);
    g_assert_cmpuint(bic_read(s, 0), ==, 0);
    g_assert_cmphex(bic_read(s, 88), ==, 0xabcd);
    bic_write(s, 8, 0);
    bic_write(s, 80, 0);
    bic_operations(s, 1, 0x245);
    g_assert_cmphex(bic_read(s, 88), ==, 0x245);
    qtest_quit(s);

    s = bic_start(1, 0, false, true, "");
    g_assert_cmpuint(bic_read(s, 0), ==, 0);
    g_assert_cmphex(bic_read(s, 88), ==, 0xabcd);
    bic_write(s, 64, 9);
    bic_write(s, 80, 0);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    qtest_qmp_eventwait(s, "RESET");
    g_assert_cmpuint(bic_read(s, 0), ==, 0);
    g_assert_cmphex(bic_read(s, 88), ==, 0xabcd);
    qtest_quit(s);
}

static void test_bic_startup_migration(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("l1-bic-migration-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = bic_start(1, 1, true, true, "");
    to = bic_start(1, 0, true, true, "-incoming defer");
    bic_write(from, 64, 9);
    bic_write(from, 72, 5);
    bic_write(to, 0, 0);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    /* Destination completion state came from the stream, with no replay. */
    g_assert_cmpuint(bic_read(to, 0), ==, 0);
    g_assert_cmphex(bic_read(to, 88), ==, 0xab41);
    bic_write(to, 8, 0);
    bic_write(to, 80, 0);
    bic_operations(to, 1, 0x245);
    g_assert_cmphex(bic_read(to, 88), ==, 0x245);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_bic_startup_migration_mismatch(void)
{
    for (unsigned mismatch = 0; mismatch < 3; mismatch++) {
        GError *error = NULL;
        g_autofree char *dir = g_dir_make_tmp("l1-bic-mismatch-XXXXXX", &error);
        g_autofree char *socket = NULL, *uri = NULL;
        QTestState *from, *to;

        g_assert_no_error(error);
        socket = g_build_filename(dir, "migration.sock", NULL);
        uri = g_strdup_printf("unix:%s", socket);
        from = bic_start(1, 0, true, true, "");
        to = bic_start(mismatch == 1 ? 2 : 1, 0, mismatch != 0, true,
            mismatch == 2 ? "-global "
                            "sgi-l1-bic-startup-fixture.identity-base=10 "
                            "-incoming defer" : "-incoming defer");
        qtest_set_expected_status(to, EXIT_FAILURE);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        qtest_qmp_assert_success(from,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        qtest_wait_qemu(to);
        qtest_quit(to);
        qtest_quit(from);
        unlink(socket);
        g_assert_cmpint(rmdir(dir), ==, 0);
    }
}

static void test_bic_startup_active_save(void)
{
    GError *error = NULL;
    g_autofree char *dir = g_dir_make_tmp("l1-bic-active-XXXXXX", &error);
    g_autofree char *socket = NULL, *uri = NULL;
    QTestState *from, *to;
    bool failed = false;

    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = bic_start(1, 0, true, true, "");
    to = bic_start(1, 0, true, true, "-incoming defer");
    bic_write(from, 112, 1);
    qtest_set_expected_status(to, EXIT_FAILURE);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    for (unsigned tries = 0; tries < 1000; tries++) {
        g_autoptr(QDict) status = qtest_qmp_assert_success_ref(from,
            "{'execute':'query-migrate'}");

        if (g_str_equal(qdict_get_str(status, "status"), "failed")) {
            failed = true;
            break;
        }
        g_usleep(1000);
    }
    g_assert_true(failed);
    bic_write(from, 112, 0);
    qtest_wait_qemu(to);
    qtest_quit(to);
    qtest_quit(from);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/l1/bic-startup/failure-visibility",
                   test_bic_startup_failure_visibility);
    qtest_add_func("sn1/l1/bic-startup/active-save",
                   test_bic_startup_active_save);
    qtest_add_func("sn1/l1/bic-startup/operations",
                   test_bic_startup_operations);
    qtest_add_func("sn1/l1/bic-startup/resets", test_bic_startup_resets);
    qtest_add_func("sn1/l1/bic-startup/migration", test_bic_startup_migration);
    qtest_add_func("sn1/l1/bic-startup/migration-mismatch",
                   test_bic_startup_migration_mismatch);
    qtest_add_func("sn1/l1/uart", test_uart);
    qtest_add_func("sn1/l1/frames", test_frames);
    qtest_add_func("sn1/l1/output-escape-startup",
                   test_output_escape_startup);
    qtest_add_func("sn1/l1/identity", test_identity);
    qtest_add_func("sn1/l1/errors", test_errors);
    qtest_add_func("sn1/l1/reset", test_reset);
    qtest_add_func("sn1/l1/eeprom", test_eeprom);
    qtest_add_func("sn1/l1/eeprom-six-entries", test_eeprom_six_entries);
    qtest_add_func("sn1/l1/attached-routing/records", test_attached_records);
    qtest_add_func("sn1/l1/attached-routing/refusals-reset",
                   test_attached_refusals_reset);
    qtest_add_func("sn1/l1/attached-routing/coordinates",
                   test_attached_coordinates);
    qtest_add_func("sn1/l1/attached-routing/migration",
                   test_attached_migration);
    qtest_add_func("sn1/l1/attached-routing/migration-mismatch",
                   test_attached_migration_mismatch);
    qtest_add_func("sn1/l1/attached-routing/composition-refusal",
                   test_attached_composition_refusal);
    qtest_add_func("sn1/l1/attached-routing/identity", test_attached_identity);
    qtest_add_func("sn1/l1/query-routing/serial-refusals",
                   test_query_serial_refusals);
    qtest_add_func("sn1/l1/query-routing/serial", test_query_serial);
    qtest_add_func("sn1/l1/query-routing/owners", test_query_owners);
    qtest_add_func("sn1/l1/query-routing/refusals-reset",
                   test_query_refusals_reset);
    qtest_add_func("sn1/l1/query-routing/coordinates", test_query_coordinates);
    qtest_add_func("sn1/l1/query-routing/migration", test_query_migration);
    qtest_add_func("sn1/l1/query-routing/migration-mismatch",
                   test_query_migration_mismatch);
    qtest_add_func("sn1/l1/expansion/lifecycle-refusal",
                   test_expansion_lifecycle_refusal);
    qtest_add_func("sn1/l1/expansion/records", test_expansion_records);
    qtest_add_data_func("sn1/l1/profile-migration/ip34",
                        GUINT_TO_POINTER(0), test_profile_migration);
    qtest_add_data_func("sn1/l1/profile-migration/unknown",
                        GUINT_TO_POINTER(1), test_profile_migration);
    qtest_add_data_func("sn1/l1/profile-migration/ip45",
                        GUINT_TO_POINTER(2), test_profile_migration);
    qtest_add_data_func("sn1/l1/profile-migration/ip35",
                        GUINT_TO_POINTER(3), test_profile_migration);
    qtest_add_data_func("sn1/l1/profile-migration/ip37",
                        GUINT_TO_POINTER(4), test_profile_migration);
    qtest_add_data_func("sn1/l1/profile-migration/pci-expansion",
                        GUINT_TO_POINTER(5), test_profile_migration);
    qtest_add_data_func("sn1/l1/profile-migration/ip45-to-unknown",
                        GUINT_TO_POINTER(6), test_profile_migration);
    qtest_add_data_func("sn1/l1/profile-migration/unknown-to-ip45",
                        GUINT_TO_POINTER(7), test_profile_migration);
    qtest_add_func("sn1/l1/expansion/migration", test_expansion_migration);
    qtest_add_func("sn1/l1/expansion/input-refusal",
                   test_expansion_input_refusal);
    qtest_add_func("sn1/l1/expansion/oversize-refusal",
                   test_expansion_oversize_refusal);
    qtest_add_func("sn1/l1/expansion/migration-mismatch",
                   test_expansion_migration_mismatch);
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
    qtest_add_func("sn1/l1/poweroff", test_poweroff);
    qtest_add_func("sn1/l1/fresh-off", test_fresh_off);
    qtest_add_func("sn1/l1/power", test_power);
    qtest_add_func("sn1/l1/node-reset", test_node_reset);
    qtest_add_func("sn1/l1/signals", test_signals);
    qtest_add_func("sn1/l1/signals-off", test_signals_off);
    qtest_add_func("sn1/l1/node-power-line", test_node_power_line);
    qtest_add_func("sn1/l1/reboot-l1", test_reboot_l1);
    qtest_add_func("sn1/l1/selection", test_selection);
    qtest_add_func("sn1/l1/lifecycle-persistence", test_lifecycle_persistence);
    qtest_add_func("sn1/l1/lifecycle-migration", test_lifecycle_migration);
    qtest_add_func("sn1/l1/route-forms", test_route_forms);
    qtest_add_func("sn1/l1/peer-pair", test_peer_pair);
    qtest_add_func("sn1/l1/peer-power", test_peer_power);
    qtest_add_func("sn1/l1/peer-nested-ctc", test_peer_nested_ctc);
    qtest_add_func("sn1/l1/command-interpreter", test_command_interpreter);
    qtest_add_func("sn1/l1/receive-tolerance", test_receive_tolerance);
    qtest_add_func("sn1/l1/request-log", test_request_log);
    qtest_add_func("sn1/l1/console-retarget", test_console_retarget);
    qtest_add_func("sn1/l1/console-short-frames", test_console_short_frames);
    qtest_add_func("sn1/l1/general-reads", test_general_reads);
    qtest_add_func("sn1/l1/fuel-mac-eeprom", test_fuel_mac_eeprom);
    qtest_add_func("sn1/l1/brick-serial", test_brick_serial);
    qtest_add_func("sn1/l1/general-actions", test_general_actions);
    qtest_add_func("sn1/l1/power-cycle", test_power_cycle);
    qtest_add_func("sn1/l1/power-requests-off", test_power_requests_off);
    qtest_add_func("sn1/l1/reboot-request", test_reboot_request);
    qtest_add_func("sn1/l1/power-down-event", test_power_down_event);
    qtest_add_func("sn1/l1/uart-interrupts", test_uart_interrupts);
    qtest_add_func("sn1/l1/uart-irq-migration", test_uart_irq_migration);
    qtest_add_func("sn1/l1/node-position", test_node_position);
    qtest_add_func("sn1/l1/node-position-migration",
                   test_node_position_migration);
    qtest_add_func("sn1/l1/node-position-range", test_node_position_range);
    qtest_add_func("sn1/l1/uart-unlinked", test_uart_unlinked);
    qtest_add_func("sn1/l1/uart-shared", test_uart_shared);
    qtest_add_func("sn1/l1/usb-software/matrix", test_usb_matrix);
    qtest_add_func("sn1/l1/usb-software/descriptor-matrix",
                   test_usb_descriptor_matrix);
    qtest_add_func("sn1/l1/usb-software/descriptor-errors-reset",
                   test_usb_descriptor_errors_reset);
    qtest_add_func("sn1/l1/usb-software/errors-reset",
                   test_usb_errors_reset);
    qtest_add_func("sn1/l1/usb-software/migration",
                   test_usb_software_migration);
    return g_test_run();
}
