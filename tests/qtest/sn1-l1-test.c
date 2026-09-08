/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Isolated L1 protocol checks; this fixture does not execute firmware. */
#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include <poll.h>
#include <sys/socket.h>

#define BASE 0x11000000ULL
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
        "\"id\":\"l1c\",\"spd-eeprom\":[\"%s\"],\"board-eeprom\":[\"%s\"]}' "
        /* JSON devices are created after key=value ones; keep the order. */
        "-device '{\"driver\":\"sgi-l1-uart\",\"id\":\"l1\","
        "\"controller\":\"l1c\"}'",
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
                              "-device sgi-l1,id=l1c,revision-major=1,"
                              "revision-minor=18,revision-patch=5" UART);
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
    uint8_t reply[129];
    const uint8_t output[] = { 0x44, 'P', 'R', 'O', 'M' };

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
    uint8_t reply[129];

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
    qtest_quit(s);
    close(fd);
}

static void test_node_power_line(void)
{
    int fd;
    QTestState *s = console_start(&fd, "", "-no-shutdown " RTC);
    uint8_t select[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 0x02, 1,
                         0, 0, 0, 0, 4 };
    uint8_t reply[129];

    console_expect(fd, POWERING ENTER_CPU0);
    qtest_irq_intercept_out_named(s, L1, "node-power");
    send_frame(s, select, sizeof(select), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
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
        uint8_t reply[129];
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
    uint8_t reply[129];

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
    uint8_t reply[129];

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
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(from);
    assert_display(to, "moved");
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

static void test_route_forms(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 -S "
                              "-device sgi-l1,id=l1c,rack=7,bay=11" UART);
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
                              "-device sgi-l1,id=l1c,rack=7,bay=11" UART);
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
    /* Rejected on the I/O view, so the L1's own line stays. */
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
    const uint8_t bad_task[] = { 0x37, 0xff, 0xff, 0xff, 0x9b, 0 };
    uint8_t reply[129];

    for (unsigned i = 0; i < G_N_ELEMENTS(commands); i++) {
        unsigned length = commands[i] == 10 ? 16 : 11;
        request[6] = commands[i];
        expected[5] = commands[i] == 10 ? 2 : 1;
        for (unsigned count_byte = 0; count_byte < 2; count_byte++) {
            send_frame(s, request, 7 + count_byte, false);
            g_assert_cmpuint(receive(s, reply), ==, length);
            g_assert_cmpmem(reply, length, expected, length);
        }
        request[7] = 1;
        send_frame(s, request, sizeof(request), false);
        g_assert_cmpuint(receive(s, reply), ==, sizeof(bad_count));
        g_assert_cmpmem(reply, sizeof(bad_count), bad_count, sizeof(bad_count));
        request[7] = 0;
        request[4] = 3;
        send_frame(s, request, 8, false);
        g_assert_cmpuint(receive(s, reply), ==, sizeof(bad_task));
        g_assert_cmpmem(reply, sizeof(bad_task), bad_task, sizeof(bad_task));
        request[4] = 6;
        request[1] = 0x4f;
        send_frame(s, request, 8, false);
        g_assert_cmpuint(receive(s, reply), ==, sizeof(bad_task));
        g_assert_cmpmem(reply, sizeof(bad_task), bad_task, sizeof(bad_task));
        request[1] = 0x0f;
    }
    qtest_quit(s);
}

static void test_uart_interrupts(void)
{
    QTestState *s = start();
    const uint8_t request[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 3 };
    uint8_t reply[129];

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
    uint8_t reply[129];

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
    uint8_t reply[129];
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
    uint8_t reply[129];
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
    uint8_t reply[129];

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
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9b);
    set_destination(request, 0x0ffff003);
    send_frame(s, request, sizeof(request), false);
    g_assert_cmpuint(receive(s, reply), ==, 6);
    g_assert_cmphex(reply[4], ==, 0x9b);
    set_destination(request, 0x0ffff006);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t select[] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 0x02, 1,
                             0, 0, 0, 0, 0x40 };
        set_destination(select, i ? 0x0ffff003 : 0x4ffff006);
        send_frame(s, select, sizeof(select), false);
        g_assert_cmpuint(receive(s, reply), ==, 6);
        g_assert_cmphex(reply[4], ==, 0x9b);
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
    qtest_add_func("sn1/l1/route-rejection", test_route_rejection);
    qtest_add_func("sn1/l1/task-commands", test_task_commands);
    qtest_add_func("sn1/l1/settings-queries", test_settings_queries);
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
