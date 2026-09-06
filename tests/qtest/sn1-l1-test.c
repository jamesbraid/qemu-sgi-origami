/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Isolated L1 protocol checks; this fixture does not execute firmware. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include <poll.h>
#include <sys/socket.h>

#define BASE 0x11000000ULL
#define PATH "/machine/peripheral/l1"

static QTestState *start(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -device sgi-l1,id=l1 -S");
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
        g_assert_cmpuint(n, <, 129);
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
    uint8_t reply[129];
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
                         "{'path':%s,'property':'display-line-2'}}", PATH);
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

static void test_identity(void)
{
    QTestState *s = start();
    uint8_t request[] = { 0x11, 0x0f, 0xff, 0xf0, 6, 0, 3, 0 };
    uint8_t expected[] = { 0x31, 0, 0, 0, 0, 2, 0, 0, 0, 0, 65,
                           0, 0, 0, 0, 'C' };
    uint8_t reply[129];
    for (unsigned i = 0; i < 2; i++) {
        send_frame(s, request, sizeof(request) - i, false);
        g_assert_cmpuint(receive(s, reply), ==, sizeof(expected));
        g_assert_cmpmem(reply, sizeof(expected), expected, sizeof(expected));
    }
    request[1] = 0x4f;
    expected[15] = 0x34;
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, sizeof(expected));
    g_assert_cmpmem(reply, sizeof(expected), expected, sizeof(expected));
    qtest_quit(s);
}

static void test_errors(void)
{
    QTestState *s = start();
    uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0xab, 0xcd, 0 };
    uint8_t reply[129];
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
                               const char *extra)
{
    QTestState *s = qtest_initf(
        "-M none -cpu R10000 -S %s -device '{\"driver\":\"sgi-l1\","
        "\"id\":\"l1\",\"spd-eeprom\":[\"%s\"],\"board-eeprom\":[\"%s\"]}'",
        extra, spd, board);
    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    return s;
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
    spd[63] = 192;
    memset(spd + 64, 0x37, 64);
    memset(board, 0x83, sizeof(board));
    g_assert_true(g_file_set_contents(spd_path, (char *)spd,
                                      sizeof(spd), &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(board_path, (char *)board,
                                      sizeof(board), &error));
    g_assert_no_error(error);
    s = start_files(spd_path, board_path, "");
    g_assert_cmpuint(eeprom_read(s, 0, 4, 0, 0, reply), ==, 11);
    g_assert_cmphex(reply[10], ==, 128);
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmpuint(eeprom_read(s, 0, 4, 64 * i, 64, reply), ==, 76);
        g_assert_cmphex(reply[10], ==, 64 * (1 - i));
        g_assert_cmphex(reply[11], ==, 0xc0);
        g_assert_cmpmem(reply + 12, 64, spd + 64 * i, 64);
    }
    g_assert_cmpuint(eeprom_read(s, 0, 4, 0, 115, reply), ==, 127);
    g_assert_cmpmem(reply + 12, 115, spd, 115);
    g_assert_cmpuint(eeprom_read(s, 0, 4, 0, 116, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x97);
    g_assert_cmpuint(eeprom_read(s, 0, 4, 28, 127, reply), ==, 112);
    g_assert_cmpmem(reply + 12, 100, spd + 28, 100);
    g_assert_cmpuint(eeprom_read(s, 0, 2, 0, 64, reply), ==, 29);
    g_assert_cmpmem(reply + 12, sizeof(board), board, sizeof(board));
    g_assert_cmpuint(eeprom_read(s, 1, 4, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x98);
    g_assert_cmpuint(eeprom_read(s, 0, 4, 129, 1, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x97);
    qtest_quit(s);
    spd[63]++;
    g_assert_true(g_file_set_contents(spd_path, (char *)spd,
                                      sizeof(spd), &error));
    g_assert_no_error(error);
    s = start_files(spd_path, board_path, "");
    g_assert_cmpuint(eeprom_read(s, 0, 4, 0, 0, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x95);
    qtest_quit(s);
    unlink(spd_path);
    unlink(board_path);
    g_assert_cmpint(rmdir(dir), ==, 0);
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
    uint8_t reply[129];
    uint16_t crc = 0xffff;
    g_assert_no_error(error);
    socket = g_build_filename(dir, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    to = qtest_init("-M none -cpu R10000 -device sgi-l1,id=l1 "
                    "-S -incoming defer");
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
    uint8_t reply[129];
    const uint8_t source_bytes[] = { 0xa5, 0x7e, 0x7d };
    const uint8_t destination_bytes[] = { 0x33, 0x44 };
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
    from = start_files("", source, "");
    to = start_files("", destination, "-incoming defer");
    g_assert_cmpuint(eeprom_read(to, 0, 2, 0, 64, reply), ==, 14);
    g_assert_cmpmem(reply + 12, 2, destination_bytes, 2);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(from);
    g_assert_cmpuint(eeprom_read(to, 0, 2, 0, 64, reply), ==, 15);
    g_assert_cmpmem(reply + 12, 3, source_bytes, 3);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    unlink(source);
    unlink(destination);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_metadata(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -S "
                              "-device sgi-l1,id=l1,revision-major=1,"
                              "revision-minor=18,revision-patch=5");
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 5 };
    const uint8_t expected[] = { 0x30, 0, 0, 0, 0, 3,
                                 0, 0, 0, 0, 1,
                                 0, 0, 0, 0, 18,
                                 0, 0, 0, 0, 5 };
    uint8_t reply[129];
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

static void test_console(void)
{
    int sv[2];
    QTestState *s;
    uint8_t reply[129];
    const uint8_t output[] = { 0x44, 'P', 'R', 'O', 'M' };
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    s = qtest_initf("-M none -cpu R10000 -S -chardev socket,id=console,fd=%d "
                    "-device sgi-l1,id=l1,chardev=console", sv[1]);
    close(sv[1]);
    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    console_expect(sv[0], "001c01-L1> ");
    console_send(sv[0], "\004");
    console_expect(sv[0], "entering console mode 001c01 console, "
                          "<CTRL-T> to escape to L1\r\n");
    send_frame(s, output, sizeof(output), false);
    console_expect(sv[0], "PROM");
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    console_send(sv[0], "x");
    for (unsigned i = 0; !(qtest_readq(s, BASE + 0x28) & 1); i++) {
        g_assert_cmpuint(i, <, 1000);
        g_usleep(1000);
    }
    g_assert_cmpuint(receive(s, reply), ==, 2);
    g_assert_cmphex(reply[0], ==, 4);
    g_assert_cmphex(reply[1], ==, 'x');
    console_send(sv[0], "\024");
    console_expect(sv[0], "escaping to L1 system controller\r\n001c01-L1> ");
    console_send(sv[0], "version\r");
    console_expect(sv[0], "L1 protocol responder; "
                          "firmware revision unavailable\r\n"
                          "re-entering console mode 001c01 console, "
                          "<CTRL-T> to escape to L1\r\n");
    console_send(sv[0], "\004");
    for (unsigned i = 0; !(qtest_readq(s, BASE + 0x28) & 1); i++) {
        g_assert_cmpuint(i, <, 1000);
        g_usleep(1000);
    }
    g_assert_cmpuint(receive(s, reply), ==, 2);
    g_assert_cmphex(reply[1], ==, 4);
    console_send(sv[0], "\024l1\r");
    console_expect(sv[0], "escaping to L1 system controller\r\n001c01-L1> "
                          "001c01-L1> ");
    console_send(sv[0], "\004");
    console_expect(sv[0], "entering console mode 001c01 console, "
                          "<CTRL-T> to escape to L1\r\n");
    qtest_quit(s);
    close(sv[0]);
}

static void test_poweroff(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -S -no-shutdown "
                              "-device sgi-l1,id=l1");
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 3, 0, 0, 1,
                                1, '*', ' ', 'p', 'w', 'r', ' ', 'd', 0 };
    uint8_t reply[129];
    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    send_frame(s, request, sizeof(request), false);
    qtest_qmp_eventwait(s, "SHUTDOWN");
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    qtest_quit(s);
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
        "{'path':%s,'property':'display-line-2'}}", PATH);
    g_assert_cmpstr(qdict_get_str(result, "return"), ==, expected);
}

static void test_route_forms(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -S "
                              "-device sgi-l1,id=l1,rack=7,bay=11");
    uint8_t request[] = { 0x12, 0, 0, 0, 0, 0, 3 };
    uint8_t expected[] = { 0x32, 0, 0, 0, 0, 2,
                           0, 0, 0, 1, 0xcb, 0, 0, 0, 0, 'C' };
    uint8_t reply[129];
    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    for (unsigned type = 0; type <= 4; type += 4) {
        expected[15] = type ? 0x34 : 'C';
        for (unsigned form = 0; form < 4; form++) {
            unsigned rack = form & 1 ? 0x3ff : 7;
            unsigned bay = form & 2 ? 0x3f : 11;
            set_destination(request, (type << 28) | (rack << 18) |
                                     (bay << 12) | 6);
            send_frame(s, request, sizeof(request), false);
            g_assert_cmpuint(receive(s, reply), ==, sizeof(expected));
            g_assert_cmpmem(reply, sizeof(expected), expected,
                            sizeof(expected));
        }
    }
    qtest_quit(s);
}

static void reject_route(QTestState *s, uint32_t address, bool malformed)
{
    uint8_t request[] = { 0x10, 0, 0, 0, 0, 0x10, 5, 1,
                          1, 'B', 'A', 'D', 0 };
    const uint8_t recover[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 5, 1,
                                1, 'k', 'e', 'e', 'p', 0 };
    uint8_t reply[129];
    set_destination(request, address);
    if (malformed) {
        request[7] = 2;
    }
    send_frame(s, request, sizeof(request), false);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0x60);
    assert_display(s, "keep");
    send_frame(s, recover, sizeof(recover), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0);
    assert_display(s, "keep");
}

static void test_route_rejection(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -S "
                              "-device sgi-l1,id=l1,rack=7,bay=11");
    const uint8_t initial[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 5, 1,
                                1, 'k', 'e', 'e', 'p', 0 };
    const uint32_t local = 0x0ffff006;
    uint8_t reply[129];
    qtest_sysbus_mmio_map(s, PATH, 0, BASE);
    send_frame(s, initial, sizeof(initial), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    for (unsigned malformed = 0; malformed < 2; malformed++) {
        reject_route(s, (8 << 18) | (0x3f << 12) | 6, malformed);
        reject_route(s, (0x3ff << 18) | (12 << 12) | 6, malformed);
        reject_route(s, (8 << 18) | (12 << 12) | 6, malformed);
        for (unsigned bit = 5; bit <= 11; bit++) {
            reject_route(s, local | (1U << bit), malformed);
        }
        for (unsigned type = 1; type < 16; type++) {
            if (type != 4) {
                reject_route(s, local | (type << 28), malformed);
            }
        }
        for (unsigned task = 0; task < 32; task++) {
            if (task != 3 && task != 6) {
                reject_route(s, (local & ~31U) | task, malformed);
            }
            if (task != 6) {
                reject_route(s, 0x4ffff000 | task, malformed);
            }
        }
    }
    qtest_quit(s);
}

static void test_task_commands(void)
{
    QTestState *s = start();
    uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 3, 0, 3, 0 };
    uint8_t reply[129];
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9b);
    request[4] = 6;
    request[6] = 0;
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9b);
    request[1] = 0x4f;
    request[5] = 0x10;
    request[6] = 5;
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9b);
    assert_display(s, "");
    request[1] = 0x0f;
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9c);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/l1/uart", test_uart);
    qtest_add_func("sn1/l1/frames", test_frames);
    qtest_add_func("sn1/l1/identity", test_identity);
    qtest_add_func("sn1/l1/errors", test_errors);
    qtest_add_func("sn1/l1/reset", test_reset);
    qtest_add_func("sn1/l1/eeprom", test_eeprom);
    qtest_add_func("sn1/l1/migration", test_migration);
    qtest_add_func("sn1/l1/eeprom-migration", test_eeprom_migration);
    qtest_add_func("sn1/l1/console", test_console);
    qtest_add_func("sn1/l1/metadata", test_metadata);
    qtest_add_func("sn1/l1/poweroff", test_poweroff);
    qtest_add_func("sn1/l1/route-forms", test_route_forms);
    qtest_add_func("sn1/l1/route-rejection", test_route_rejection);
    qtest_add_func("sn1/l1/task-commands", test_task_commands);
    return g_test_run();
}
