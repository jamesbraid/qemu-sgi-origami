/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Public IO8 example records, read through the Origin 300 L1 UART. The
 * sample serial and identity bytes below are a fixture, not factory defaults.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define PROM_SIZE UINT64_C(0x1686a8)
#define L1_UART_BASE (UINT64_C(0x10000000000) + UINT64_C(0x30000000) + 0x80)
#define L1_UART_STATUS 0x28
#define L1_STATUS_READY 0x61
#define IOC3_PATH "/machine/io8-ioc3"
#define MAC "02:00:5d:aa:bb:cc"

/*
 * 007-4498-002, printed page 182: IO8 CIA and BIA. BIA's twelve MAC
 * characters are changed from the printed sample to MAC; byte 79 is the
 * independently recalculated negative byte sum, 0x2c.
 */
static const uint8_t cia[16] = {
    0x00, 0x02, 0x17, 0xc2, 0x4e, 0x41, 0xc2, 0x4e,
    0x41, 0xc1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x84,
};

static const uint8_t bia[80] = {
    0x00, 0x0a, 0x00, 0x8f, 0xaa, 0x2c, 0xc9, 0x43,
    0x45, 0x4c, 0x45, 0x53, 0x54, 0x49, 0x43, 0x41,
    0xc3, 0x49, 0x4f, 0x38, 0xc6, 0x4d, 0x48, 0x45,
    0x35, 0x34, 0x39, 0xcc, 0x30, 0x33, 0x30, 0x5f,
    0x31, 0x36, 0x37, 0x33, 0x5f, 0x30, 0x30, 0x33,
    0x00, 0xc2, 0x5f, 0x45, 0x01, 0x02, 0xc2, 0x30,
    0x30, 0x04, 0x00, 0x00, 0x24, 0x17, 0x04, 0x00,
    0x00, 0x00, 0x02, 0x04, 0x00, 0x00, 0x1c, 0x75,
    0xcc, 0x30, 0x32, 0x30, 0x30, 0x35, 0x44, 0x41,
    0x41, 0x42, 0x42, 0x43, 0x43, 0xc1, 0x00, 0x2c,
};

static uint16_t fcs(uint16_t value, uint8_t byte)
{
    value ^= byte;
    for (unsigned bit = 0; bit < 8; bit++) {
        value = (value & 1) ? (value >> 1) ^ 0x8408 : value >> 1;
    }
    return value;
}

static void put(QTestState *qts, uint8_t byte)
{
    if (byte == 0x7e || byte == 0x7d) {
        qtest_writeq(qts, L1_UART_BASE, 0x7d);
        byte ^= 0x20;
    }
    qtest_writeq(qts, L1_UART_BASE, byte);
}

static void send_frame(QTestState *qts, const uint8_t *frame, size_t length)
{
    uint16_t crc = 0xffff;

    qtest_writeq(qts, L1_UART_BASE, 0x7e);
    for (size_t i = 0; i < length; i++) {
        crc = fcs(crc, frame[i]);
        put(qts, frame[i]);
    }
    crc = ~crc;
    put(qts, crc);
    put(qts, crc >> 8);
    qtest_writeq(qts, L1_UART_BASE, 0x7e);
}

static size_t receive_frame(QTestState *qts, uint8_t *reply)
{
    size_t length = 0;
    bool escaped = false;
    uint16_t crc = 0xffff;

    g_assert_cmphex(qtest_readq(qts, L1_UART_BASE + L1_UART_STATUS), ==,
                    L1_STATUS_READY);
    g_assert_cmphex(qtest_readq(qts, L1_UART_BASE), ==, 0x7e);
    for (;;) {
        uint8_t byte;

        g_assert_cmphex(qtest_readq(qts, L1_UART_BASE + L1_UART_STATUS), ==,
                        L1_STATUS_READY);
        byte = qtest_readq(qts, L1_UART_BASE);
        if (byte == 0x7e) {
            break;
        }
        if (byte == 0x7d && !escaped) {
            escaped = true;
            continue;
        }
        if (escaped) {
            byte ^= 0x20;
            escaped = false;
        }
        g_assert_cmpuint(length, <, 131);
        reply[length++] = byte;
        crc = fcs(crc, byte);
    }
    g_assert_false(escaped);
    g_assert_cmpuint(length, >=, 3);
    g_assert_cmphex(crc, ==, 0xf0b8);
    return length - 2;
}

static size_t read_area(QTestState *qts, unsigned area, unsigned length,
                        uint8_t *reply)
{
    uint8_t request[28] = { 0x10, 0x0f, 0xff, 0xf0, 6, 0, 6, 4 };
    const unsigned values[] = { 1, area, 0, length };

    for (unsigned i = 0; i < ARRAY_SIZE(values); i++) {
        unsigned value = values[i];
        request[9 + 5 * i] = value >> 24;
        request[10 + 5 * i] = value >> 16;
        request[11 + 5 * i] = value >> 8;
        request[12 + 5 * i] = value;
    }
    send_frame(qts, request, sizeof(request));
    return receive_frame(qts, reply);
}

static uint8_t sum_bytes(const uint8_t *bytes, size_t length)
{
    uint8_t sum = 0;

    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum;
}

/* The published record contains another 0xcc before the address field. */
static bool board_matches_mac(const uint8_t *board, const char *mac)
{
    char parsed[18];

    if (board[64] != 0xcc || board[77] != 0xc1 || board[78] != 0 ||
        sum_bytes(board, 80) != 0) {
        return false;
    }
    for (unsigned i = 0; i < 12; i++) {
        if (!g_ascii_isxdigit(board[65 + i])) {
            return false;
        }
    }
    g_snprintf(parsed, sizeof(parsed), "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
               board[65], board[66], board[67], board[68],
               board[69], board[70], board[71], board[72],
               board[73], board[74], board[75], board[76]);
    return g_ascii_strcasecmp(parsed, mac) == 0;
}

static char *write_fixture(const char *directory, const char *name,
                           const uint8_t *data, size_t length)
{
    g_autoptr(GError) error = NULL;
    char *path = g_build_filename(directory, name, NULL);

    g_assert_true(g_file_set_contents(path, (const char *)data, length,
                                      &error));
    g_assert_no_error(error);
    return path;
}

static void check_record(bool mismatched)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-io8-mac-XXXXXX", &error);
    g_autofree char *prom_path = NULL;
    g_autofree char *chassis_path = NULL;
    g_autofree char *board_path = NULL;
    g_autofree char *quoted_prom = NULL;
    g_autofree char *quoted_chassis = NULL;
    g_autofree char *quoted_board = NULL;
    g_autofree uint8_t *prom = g_malloc0(PROM_SIZE);
    uint8_t record[sizeof(bia)], reply[131], board[sizeof(bia)];
    g_autoptr(QDict) result = NULL;
    QTestState *qts;
    const char *actual_mac;

    g_assert_no_error(error);
    memcpy(record, bia, sizeof(record));
    if (mismatched) {
        record[65] = '1';
        record[79] = 0x2b; /* +1 ASCII byte needs -1 checksum byte. */
    }
    prom_path = write_fixture(directory, "prom.bin", prom, PROM_SIZE);
    chassis_path = write_fixture(directory, "chassis.bin", cia, sizeof(cia));
    board_path = write_fixture(directory, "board.bin", record,
                               sizeof(record));
    quoted_prom = g_shell_quote(prom_path);
    quoted_chassis = g_shell_quote(chassis_path);
    quoted_board = g_shell_quote(board_path);

    qts = qtest_initf(
        "-M origin300,chassis-eeprom.0=,chassis-eeprom.1=%s,"
        "board-eeprom.0=,board-eeprom.1=%s "
        "-cpu R14000 -smp 2 -bios %s -display none -S "
        "-nic user,mac=" MAC,
        quoted_chassis, quoted_board, quoted_prom);

    result = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                           "{'path':'" IOC3_PATH "','property':'mac'}}");
    g_assert_true(qdict_haskey(result, "return"));
    actual_mac = qdict_get_str(result, "return");
    g_assert_cmpstr(actual_mac, ==, MAC);

    g_assert_cmpuint(read_area(qts, 1, sizeof(cia), reply), ==,
                     12 + sizeof(cia));
    g_assert_cmphex(reply[4], ==, 0);
    g_assert_cmphex(reply[5], ==, 2);
    g_assert_cmphex(reply[10], ==, 0);
    g_assert_cmphex(reply[11], ==, 0x80 | sizeof(cia));
    g_assert_cmpmem(reply + 12, sizeof(cia), cia, sizeof(cia));
    g_assert_cmphex(reply[12 + 9], ==, 0xc1);
    g_assert_cmphex(sum_bytes(reply + 12, sizeof(cia)), ==, 0);

    g_assert_cmpuint(read_area(qts, 2, sizeof(bia), reply), ==,
                     12 + sizeof(bia));
    g_assert_cmphex(reply[4], ==, 0);
    g_assert_cmphex(reply[5], ==, 2);
    g_assert_cmphex(reply[10], ==, 0);
    g_assert_cmphex(reply[11], ==, 0x80 | sizeof(bia));
    g_assert_cmpmem(reply + 12, sizeof(bia), record, sizeof(record));
    memcpy(board, reply + 12, sizeof(board));
    g_assert_cmphex(board[49], ==, 0x04);
    g_assert_cmphex(board[54], ==, 0x04);
    g_assert_cmphex(board[59], ==, 0x04);
    g_assert_cmphex(board[64], ==, 0xcc);
    g_assert_cmphex(board[77], ==, 0xc1);
    g_assert_cmphex(board[78], ==, 0);
    g_assert_cmphex(sum_bytes(board, sizeof(board)), ==, 0);
    g_assert_cmpint(board_matches_mac(board, actual_mac), ==, !mismatched);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(prom_path), ==, 0);
    g_assert_cmpint(g_unlink(chassis_path), ==, 0);
    g_assert_cmpint(g_unlink(board_path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_matching_record(void)
{
    check_record(false);
}

static void test_mismatched_record(void)
{
    check_record(true);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("origin300/io8/mac/record-matches-ioc3",
                   test_matching_record);
    qtest_add_func("origin300/io8/mac/mismatched-record-detected",
                   test_mismatched_record);
    return g_test_run();
}
