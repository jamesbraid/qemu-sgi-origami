/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* O2 MACE identity-line wiring tests. */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest.h"
#include "memtx.h"
#include "sgi-launch.h"

#define ID_REG UINT64_C(0x1f310008)
#define ID_RELEASE (1U << 2)
#define ID_SAMPLE (1U << 3)

static char *id_image_file(void)
{
    char *path = g_strdup_printf("%s/o2-id-XXXXXX", g_get_tmp_dir());
    uint8_t bytes[128];
    int fd;

    for (size_t i = 0; i < sizeof(bytes); i++) {
        bytes[i] = (i * 37) ^ 0xa5;
    }
    fd = g_mkstemp(path);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, bytes, sizeof(bytes)), ==, sizeof(bytes));
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *id_machine_start(const char *image)
{
    return qtest_initf(
        "-M o2-test,identity-diagnostic=on,"
        "identity-test-serial=1032547698ba,identity-eprom-file=%s,"
        "identity-status=0123456789abcd -nic none", image);
}

static bool id_level(QTestState *qts)
{
    return qtest_readq(qts, ID_REG) & ID_SAMPLE;
}

static void id_drive(QTestState *qts, unsigned size, bool release)
{
    MemTxResult result;

    if (size == 4) {
        result = memtx_writel(qts, ID_REG + 4,
                              release ? ID_RELEASE : 0);
    } else {
        result = memtx_writeq(qts, ID_REG, release ? ID_RELEASE : 0);
    }
    g_assert_cmpint(result, ==, MEMTX_OK);
}

static void id_assert_word_level(QTestState *qts, bool expected)
{
    uint32_t word;

    g_assert_cmpint(memtx_readl(qts, ID_REG + 4, &word), ==, MEMTX_OK);
    g_assert_cmpint(!!(word & ID_SAMPLE), ==, expected);
    g_assert_cmpint(id_level(qts), ==, expected);
}

static bool id_word_level(QTestState *qts)
{
    uint32_t word;

    g_assert_cmpint(memtx_readl(qts, ID_REG + 4, &word), ==, MEMTX_OK);
    return word & ID_SAMPLE;
}

static void id_word_write_bit(QTestState *qts, bool bit)
{
    id_drive(qts, 4, false);
    qtest_clock_step(qts, bit ? 6000 : 80000);
    id_drive(qts, 4, true);
    qtest_clock_step(qts, bit ? 114000 : 40000);
}

static void id_word_write_byte(QTestState *qts, uint8_t value)
{
    for (unsigned bit = 0; bit < 8; bit++) {
        id_word_write_bit(qts, (value >> bit) & 1);
    }
}

static bool id_word_read_bit(QTestState *qts)
{
    bool level;

    id_drive(qts, 4, false);
    qtest_clock_step(qts, 6000);
    id_drive(qts, 4, true);
    qtest_clock_step(qts, 9000);
    level = id_word_level(qts);
    qtest_clock_step(qts, 105000);
    return level;
}

static uint8_t id_word_read_byte(QTestState *qts)
{
    uint8_t value = 0;

    for (unsigned bit = 0; bit < 8; bit++) {
        value |= id_word_read_bit(qts) << bit;
    }
    return value;
}

static void id_word_reset(QTestState *qts)
{
    id_drive(qts, 4, true);
    qtest_clock_step(qts, 500000);
    id_drive(qts, 4, false);
    qtest_clock_step(qts, 520000);
    id_drive(qts, 4, true);
    qtest_clock_step(qts, 65000);
    id_assert_word_level(qts, false);
    qtest_clock_step(qts, 100000);
    id_assert_word_level(qts, true);
}

static uint8_t id_crc8(const uint8_t *bytes, size_t length)
{
    uint8_t crc = 0;

    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x8c : 0);
        }
    }
    return crc;
}

static uint8_t id_eprom_byte(unsigned address)
{
    return (address * 37) ^ 0xa5;
}

static void id_word_read_header(QTestState *qts, uint8_t command,
                                uint16_t address)
{
    const uint8_t header[] = { command, address & 0xff, address >> 8 };

    for (size_t i = 0; i < sizeof(header); i++) {
        id_word_write_byte(qts, header[i]);
    }
    g_assert_cmphex(id_word_read_byte(qts), ==,
                    id_crc8(header, sizeof(header)));
}

static void id_write_bit(QTestState *qts, bool bit)
{
    qtest_writeq(qts, ID_REG, 0);
    qtest_clock_step(qts, bit ? 6000 : 80000);
    qtest_writeq(qts, ID_REG, ID_RELEASE);
    qtest_clock_step(qts, bit ? 114000 : 40000);
}

static void id_write_byte(QTestState *qts, uint8_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        id_write_bit(qts, (value >> i) & 1);
    }
}

static bool id_read_bit(QTestState *qts)
{
    bool level;

    qtest_writeq(qts, ID_REG, 0);
    qtest_clock_step(qts, 6000);
    qtest_writeq(qts, ID_REG, ID_RELEASE);
    qtest_clock_step(qts, 9000);
    level = id_level(qts);
    qtest_clock_step(qts, 105000);
    return level;
}

static uint8_t id_read_byte(QTestState *qts)
{
    uint8_t value = 0;

    for (unsigned i = 0; i < 8; i++) {
        value |= id_read_bit(qts) << i;
    }
    return value;
}

static void id_reset(QTestState *qts)
{
    qtest_writeq(qts, ID_REG, ID_RELEASE);
    qtest_clock_step(qts, 500000);
    qtest_writeq(qts, ID_REG, 0);
    qtest_clock_step(qts, 520000);
    qtest_writeq(qts, ID_REG, ID_RELEASE);
    qtest_clock_step(qts, 65000);
    g_assert_false(id_level(qts));
    qtest_clock_step(qts, 100000);
    g_assert_true(id_level(qts));
}

static void test_machine_read_rom(void)
{
    static const uint8_t rom[] = {
        0x09, 0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0x81
    };
    g_autofree char *image = id_image_file();
    QTestState *qts = qtest_init("-M o2-test -nic none");
    g_autoptr(QDict) default_mode = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'identity-diagnostic'}}");

    g_assert_null(qdict_get_qdict(default_mode, "error"));
    g_assert_false(qdict_get_bool(default_mode, "return"));
    qtest_writeq(qts, ID_REG, ID_RELEASE);
    g_assert_cmphex(qtest_readq(qts, ID_REG) & (ID_RELEASE | ID_SAMPLE),
                    ==, ID_RELEASE);
    qtest_quit(qts);
    qts = id_machine_start(image);

    g_assert_cmphex(qtest_readq(qts, ID_REG) & (ID_RELEASE | ID_SAMPLE),
                    ==, 0);
    id_reset(qts);
    id_write_byte(qts, 0x33);
    for (size_t i = 0; i < sizeof(rom); i++) {
        g_assert_cmphex(id_read_byte(qts), ==, rom[i]);
    }
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(image), ==, 0);
}

static void test_misc_word_protocol(void)
{
    static const uint8_t rom_prefix[7] = {
        0x09, 0x10, 0x32, 0x54, 0x76, 0x98, 0xba
    };
    static const uint8_t status[8] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0x00
    };
    static const uint64_t station = UINT64_C(0x02a14c7395e6);
    const uint64_t station_reg = UINT64_C(0x1f2800a0);
    g_autofree char *image = id_image_file();
    QTestState *qts = qtest_initf(
        MEMTX_TEST_DEVICE "-M o2-test,identity-diagnostic=on,"
        "identity-test-serial=1032547698ba,identity-eprom-file=%s,"
        "identity-status=0123456789abcd,mace-misc-word-pio=on "
        "-nic none", image);
    uint8_t tail[2];

    qtest_writeq(qts, station_reg, station);
    id_word_reset(qts);
    id_word_write_byte(qts, 0x33);
    for (size_t i = 0; i < sizeof(rom_prefix); i++) {
        g_assert_cmphex(id_word_read_byte(qts), ==, rom_prefix[i]);
    }
    g_assert_cmphex(id_word_read_byte(qts), ==,
                    id_crc8(rom_prefix, sizeof(rom_prefix)));

    id_word_reset(qts);
    id_word_write_byte(qts, 0xcc);
    id_word_read_header(qts, 0xf0, 0x7e);
    tail[0] = id_eprom_byte(0x7e);
    tail[1] = id_eprom_byte(0x7f);
    g_assert_cmphex(id_word_read_byte(qts), ==, tail[0]);
    g_assert_cmphex(id_word_read_byte(qts), ==, tail[1]);
    g_assert_cmphex(id_word_read_byte(qts), ==, id_crc8(tail, 2));
    g_assert_cmphex(id_word_read_byte(qts), ==, 0xff);

    id_word_reset(qts);
    id_word_write_byte(qts, 0xcc);
    id_word_read_header(qts, 0xc3, 0x1e);
    tail[0] = id_eprom_byte(0x1e);
    tail[1] = id_eprom_byte(0x1f);
    g_assert_cmphex(id_word_read_byte(qts), ==, tail[0]);
    g_assert_cmphex(id_word_read_byte(qts), ==, tail[1]);
    g_assert_cmphex(id_word_read_byte(qts), ==, id_crc8(tail, 2));
    g_assert_cmphex(id_word_read_byte(qts), ==, id_eprom_byte(0x20));

    id_word_reset(qts);
    id_word_write_byte(qts, 0xcc);
    id_word_read_header(qts, 0xaa, 0);
    for (size_t i = 0; i < sizeof(status); i++) {
        g_assert_cmphex(id_word_read_byte(qts), ==, status[i]);
    }
    g_assert_cmphex(id_word_read_byte(qts), ==, id_crc8(status, 8));
    g_assert_cmphex(id_word_read_byte(qts), ==, 0xff);
    g_assert_cmphex(qtest_readq(qts, station_reg), ==, station);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(image), ==, 0);
}

static void assert_machine_rejected(const char *machine,
                                    const char *reason)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", machine, "-m", "64M",
        "-nodefaults", "-display", "none", "-S", "-qmp", "stdio",
        NULL
    };

    assert_launch_rejected(args, reason);
}

static void test_machine_options(void)
{
    g_autofree char *image = id_image_file();
    g_autofree char *without_enable = g_strdup_printf(
        "o2-test,identity-test-serial=1032547698ba,"
        "identity-eprom-file=%s,identity-status=0123456789abcd", image);
    g_autofree char *without_serial = g_strdup_printf(
        "o2-test,identity-diagnostic=on,identity-eprom-file=%s,"
        "identity-status=0123456789abcd", image);
    g_autofree char *invalid_image = g_strdup_printf(
        "o2-test,identity-diagnostic=on,"
        "identity-test-serial=1032547698ba,"
        "identity-eprom-file=%s,identity-status=0123456789abcd", image);
    uint8_t short_image[127] = { 0 };

    assert_machine_rejected(without_enable,
                            "identity inputs require identity-diagnostic=on");
    assert_machine_rejected("o2-test,identity-diagnostic=on",
                            "identity diagnostic requires");
    assert_machine_rejected("o2-test,mace-misc-word-pio=on",
                            "requires identity-diagnostic=on");
    assert_machine_rejected(without_serial,
                            "identity diagnostic requires");
    g_assert_true(g_file_set_contents(image, (char *)short_image,
                                      sizeof(short_image), NULL));
    assert_machine_rejected(invalid_image, "exactly 128 bytes");
    g_assert_cmpint(g_unlink(image), ==, 0);
}

static void test_machine_memory_reset_and_mac(void)
{
    static const uint64_t station = UINT64_C(0x02a14c7395e6);
    g_autofree char *image = id_image_file();
    QTestState *qts = id_machine_start(image);

    qtest_writeq(qts, UINT64_C(0x1f2800a0), station);
    g_assert_cmphex(qtest_readq(qts, UINT64_C(0x1f2800a0)), ==, station);
    id_reset(qts);
    id_write_byte(qts, 0xcc);
    id_write_byte(qts, 0xf0);
    id_write_byte(qts, 0x19);
    id_write_byte(qts, 0x00);
    g_assert_cmphex(id_read_byte(qts), ==, 0xd3);
    g_assert_cmphex(id_read_byte(qts), ==, 0x38);
    g_assert_cmphex(qtest_readq(qts, UINT64_C(0x1f2800a0)), ==, station);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, ID_REG) & (ID_RELEASE | ID_SAMPLE),
                    ==, 0);
    qtest_writeq(qts, ID_REG, ID_RELEASE);
    qtest_clock_step(qts, 200000);
    g_assert_true(id_level(qts));
    id_reset(qts);
    id_write_byte(qts, 0xcc);
    id_write_byte(qts, 0xf0);
    id_write_byte(qts, 0x19);
    id_write_byte(qts, 0x00);
    g_assert_cmphex(id_read_byte(qts), ==, 0xd3);
    g_assert_cmphex(id_read_byte(qts), ==, 0x38);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(image), ==, 0);
}

static void test_machine_migration_boundary(void)
{
    g_autofree char *image = id_image_file();
    QTestState *qts = qtest_init("-M o2-test -nic none");

    {
        g_autoptr(QDict) control = qtest_qmp(qts,
            "{'execute':'migrate','arguments':{'uri':'file:/dev/null'}}");
        QDict *error = qdict_get_qdict(control, "error");

        if (error) {
            g_assert_null(strstr(qdict_get_str(error, "desc"),
                                 "identity diagnostic"));
        }
    }
    qtest_quit(qts);

    qts = id_machine_start(image);
    {
        g_autoptr(QDict) migration = qtest_qmp(qts,
            "{'execute':'migrate','arguments':{'uri':'file:/dev/null'}}");
        QDict *error = qdict_get_qdict(migration, "error");
        g_autofree char *snapshot = NULL;

        g_assert_nonnull(error);
        g_assert_nonnull(strstr(qdict_get_str(error, "desc"),
                                "identity diagnostic"));
        snapshot = qtest_hmp(qts, "savevm o2-identity-test");
        g_assert_nonnull(strstr(snapshot, "identity diagnostic"));
    }
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(image), ==, 0);
}

static void test_misc_word_pio_default_off(void)
{
    g_autofree char *image = id_image_file();
    QTestState *qts = qtest_initf(
        MEMTX_TEST_DEVICE "-M o2-test,identity-diagnostic=on,"
        "identity-test-serial=1032547698ba,identity-eprom-file=%s,"
        "identity-status=0123456789abcd -nic none", image);
    g_autoptr(QDict) option = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine',"
        "'property':'mace-misc-word-pio'}}");
    uint64_t wide;
    uint32_t word;

    g_assert_null(qdict_get_qdict(option, "error"));
    g_assert_false(qdict_get_bool(option, "return"));
    g_assert_cmpint(memtx_readq(qts, ID_REG, &wide), ==, MEMTX_OK);
    g_assert_cmphex(wide, ==, 0x20);
    g_assert_cmpint(memtx_readl(qts, ID_REG, &word), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readl(qts, ID_REG + 4, &word), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writel(qts, ID_REG, 0), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writel(qts, ID_REG + 4, ID_RELEASE), !=,
                    MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x20);
    id_drive(qts, 8, true);
    qtest_clock_step(qts, 500000);
    id_drive(qts, 8, false);
    qtest_clock_step(qts, 520000);
    id_drive(qts, 8, true);
    qtest_clock_step(qts, 30000);
    g_assert_false(id_level(qts));
    qtest_clock_step(qts, 120000);
    g_assert_true(id_level(qts));
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(image), ==, 0);
}

static void test_identity_diagnostic_disabled(void)
{
    static const int64_t command_low_ns[8] = {
        16792, 21922, 98217, 98307, 15640, 15660, 97004, 96955
    };
    static const int64_t read_low_ns[8] = {
        15800, 15589, 16111, 15830, 14798, 14848, 14948, 15570
    };
    QTestState *qts = qtest_init(MEMTX_TEST_DEVICE "-M o2-test -nic none");

    qtest_writeq(qts, ID_REG, ID_RELEASE);
    qtest_clock_step(qts, 500000);
    qtest_writeq(qts, ID_REG, 0);
    qtest_clock_step(qts, 520000);
    qtest_writeq(qts, ID_REG, ID_RELEASE);
    qtest_clock_step(qts, 65000);
    g_assert_false(id_level(qts));

    for (unsigned bit = 0; bit < 8; bit++) {
        qtest_writeq(qts, ID_REG, 0);
        qtest_clock_step(qts, command_low_ns[bit]);
        qtest_writeq(qts, ID_REG, ID_RELEASE);
        qtest_clock_step(qts, 120000 - command_low_ns[bit]);
    }
    for (unsigned bit = 0; bit < 8; bit++) {
        qtest_writeq(qts, ID_REG, 0);
        qtest_clock_step(qts, read_low_ns[bit]);
        qtest_writeq(qts, ID_REG, ID_RELEASE);
        qtest_clock_step(qts, 25000 - read_low_ns[bit]);
        g_assert_false(id_level(qts));
        qtest_clock_step(qts, 95000);
    }
    qtest_writeq(qts, ID_REG, 0);
    qtest_clock_step(qts, 8088);
    qtest_writeq(qts, ID_REG, ID_RELEASE);
    qtest_clock_step(qts, 1168);
    g_assert_false(id_level(qts));
    qtest_clock_step(qts, 5744);
    g_assert_false(id_level(qts));
    qtest_clock_step(qts, 45000);
    g_assert_false(id_level(qts));
    qtest_clock_step(qts, 48520);
    g_assert_false(id_level(qts));
    qtest_quit(qts);
}

static void test_misc_word_reset_timing(void)
{
    static const struct {
        int64_t high_ns;
        int64_t low_ns;
        bool presence;
    } cases[] = {
        { 479999, 520000, false },
        { 480000, 520000, true },
        { 500000, 479999, false },
        { 500000, 480000, true },
        { 500000, 959999, true },
        { 500000, 960000, false },
    };
    g_autofree char *image = id_image_file();
    QTestState *qts = qtest_initf(
        MEMTX_TEST_DEVICE "-M o2-test,identity-diagnostic=on,"
        "identity-test-serial=1032547698ba,identity-eprom-file=%s,"
        "identity-status=0123456789abcd,mace-misc-word-pio=on "
        "-nic none", image);

    for (unsigned size = 4; size <= 8; size += 4) {
        for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
            qtest_system_reset(qts);
            id_drive(qts, size, true);
            id_assert_word_level(qts, true);
            qtest_clock_step(qts, cases[i].high_ns);
            id_drive(qts, size, false);
            id_assert_word_level(qts, false);
            qtest_clock_step(qts, cases[i].low_ns);
            id_drive(qts, size, true);
            id_assert_word_level(qts, true);
            qtest_clock_step(qts, 29999);
            id_assert_word_level(qts, true);
            qtest_clock_step(qts, 1);
            id_assert_word_level(qts, !cases[i].presence);
            qtest_clock_step(qts, 119999);
            id_assert_word_level(qts, !cases[i].presence);
            qtest_clock_step(qts, 1);
            id_assert_word_level(qts, true);
        }
    }
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(image), ==, 0);
}

static void test_misc_word_presence_without_timer_service(void)
{
    g_autofree char *image = id_image_file();
    QTestState *qts = qtest_initf(
        MEMTX_TEST_DEVICE "-M o2-test,identity-diagnostic=on,"
        "identity-test-serial=1032547698ba,identity-eprom-file=%s,"
        "identity-status=0123456789abcd,mace-misc-word-pio=on "
        "-nic none", image);

    qtest_clock_timers(qts, false);
    id_drive(qts, 4, true);
    qtest_clock_step(qts, 500000);
    id_drive(qts, 4, false);
    qtest_clock_step(qts, 520000);
    id_drive(qts, 4, true);
    qtest_clock_step(qts, 29999);
    id_assert_word_level(qts, true);
    qtest_clock_step(qts, 1);
    id_assert_word_level(qts, false);
    qtest_clock_step(qts, 119999);
    id_assert_word_level(qts, false);
    qtest_clock_step(qts, 1);
    id_assert_word_level(qts, true);
    qtest_clock_step(qts, 5000000);
    qtest_clock_timers(qts, true);
    qtest_clock_step(qts, 1);
    id_assert_word_level(qts, true);
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(image), ==, 0);
}

static void test_misc_word_session_without_timer_service(void)
{
    g_autofree char *image = id_image_file();
    QTestState *qts = qtest_initf(
        MEMTX_TEST_DEVICE "-M o2-test,identity-diagnostic=on,"
        "identity-test-serial=1032547698ba,identity-eprom-file=%s,"
        "identity-status=0123456789abcd,mace-misc-word-pio=on "
        "-nic none", image);

    qtest_clock_timers(qts, false);
    id_drive(qts, 4, true);
    qtest_clock_step(qts, 500000);
    id_drive(qts, 4, false);
    qtest_clock_step(qts, 520000);
    id_drive(qts, 4, true);
    qtest_clock_step(qts, 150000);
    id_write_byte(qts, 0x33);
    qtest_clock_timers(qts, true);
    qtest_clock_step(qts, 1);
    g_assert_true(id_read_bit(qts));
    g_assert_false(id_read_bit(qts));
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(image), ==, 0);
}

static void test_misc_word_pio_selected(void)
{
    static const uint8_t reset_raw[8] = {
        0, 0, 0, 0, 0, 0, 0, 0x20
    };
    const uint64_t ring_base = UINT64_C(0x1f310000);
    const uint64_t irq_status = UINT64_C(0x1f310010);
    const uint64_t irq_mask = UINT64_C(0x1f310018);
    g_autofree char *image = id_image_file();
    g_autofree char *dir = g_dir_make_tmp("o2-misc-word-XXXXXX", NULL);
    g_autofree char *log_path = g_build_filename(dir, "qemu.log", NULL);
    g_autofree char *log_text = NULL;
    uint8_t raw[8];
    uint64_t wide;
    uint32_t high, low;
    uint16_t half;
    uint8_t byte;
    QTestState *qts = qtest_initf(
        MEMTX_TEST_DEVICE "-M o2-test,identity-diagnostic=on,"
        "identity-test-serial=1032547698ba,identity-eprom-file=%s,"
        "identity-status=0123456789abcd,mace-misc-word-pio=on "
        "-nic none -d unimp -D %s", image, log_path);

    g_assert_cmpint(memtx_readq(qts, ID_REG, &wide), ==, MEMTX_OK);
    g_assert_cmphex(wide, ==, 0x20);
    g_assert_cmpint(memtx_readl(qts, ID_REG, &high), ==, MEMTX_OK);
    g_assert_cmpint(memtx_readl(qts, ID_REG + 4, &low), ==, MEMTX_OK);
    g_assert_cmphex(high, ==, 0);
    g_assert_cmphex(low, ==, 0x20);
    g_assert_cmphex(((uint64_t)high << 32) | low, ==, wide);
    qtest_memread(qts, ID_REG, raw, sizeof(raw));
    g_assert_cmpmem(raw, sizeof(raw), reset_raw, sizeof(reset_raw));

    g_assert_cmpint(memtx_writeq(qts, ID_REG, 0x34), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x3c);
    g_assert_cmpint(memtx_writel(qts, ID_REG, 0), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x3c);
    g_assert_cmpint(memtx_writel(qts, ID_REG, 0xa5c30042), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x3c);
    g_assert_cmpint(memtx_writel(qts, ID_REG + 4, ID_RELEASE), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x0c);

    g_assert_cmpint(memtx_writel(qts, ID_REG + 4, ID_SAMPLE), ==, MEMTX_OK);
    g_assert_cmpint(memtx_readl(qts, ID_REG + 4, &low), ==, MEMTX_OK);
    g_assert_cmphex(low & ID_SAMPLE, ==, 0);
    qtest_writeq(qts, ID_REG, ID_RELEASE);
    qtest_clock_step(qts, 500000);
    qtest_writeq(qts, ID_REG, 0);
    qtest_clock_step(qts, 520000);
    qtest_writeq(qts, ID_REG, ID_RELEASE);
    qtest_clock_step(qts, 65000);
    g_assert_false(id_level(qts));
    g_assert_cmpint(memtx_writel(qts, ID_REG + 4,
                                 ID_RELEASE | ID_SAMPLE), ==, MEMTX_OK);
    g_assert_cmpint(memtx_readl(qts, ID_REG + 4, &low), ==, MEMTX_OK);
    g_assert_cmphex(low & ID_SAMPLE, ==, 0);
    qtest_clock_step(qts, 100000);
    g_assert_cmpint(memtx_writel(qts, ID_REG + 4, ID_RELEASE), ==, MEMTX_OK);
    g_assert_cmpint(memtx_readl(qts, ID_REG + 4, &low), ==, MEMTX_OK);
    g_assert_cmphex(low & ID_SAMPLE, ==, ID_SAMPLE);
    g_assert_cmphex(qtest_readq(qts, ID_REG) &
                    (ID_RELEASE | ID_SAMPLE), ==,
                    ID_RELEASE | ID_SAMPLE);

    g_assert_cmpint(memtx_readb(qts, ID_REG, &byte), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readb(qts, ID_REG + 4, &byte), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readw(qts, ID_REG, &half), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readw(qts, ID_REG + 4, &half), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeb(qts, ID_REG + 4, 0), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writew(qts, ID_REG + 4, 0), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x0c);

    g_assert_cmphex(qtest_readq(qts, ring_base), ==, 1);
    g_assert_cmpint(memtx_readl(qts, ring_base, &low), ==, MEMTX_OK);
    g_assert_cmpint(memtx_writel(qts, ring_base, 0), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ring_base), ==, 1);
    g_assert_cmpint(memtx_readl(qts, irq_status, &low), ==, MEMTX_OK);
    qtest_writeq(qts, irq_mask, 1);
    g_assert_cmpint(memtx_writel(qts, irq_mask, 0), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, irq_mask), ==, 1);
    g_assert_cmpint(memtx_readl(qts, ID_REG + 0x20, &low), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x0c);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x20);
    g_assert_cmpint(memtx_readl(qts, ID_REG, &high), ==, MEMTX_OK);
    g_assert_cmpint(memtx_readl(qts, ID_REG + 4, &low), ==, MEMTX_OK);
    g_assert_cmphex(high, ==, 0);
    g_assert_cmphex(low, ==, 0x20);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log_text, NULL, NULL));
    g_assert_nonnull(strstr(log_text,
                            "unsupported nonzero Misc high word"));
    g_assert_nonnull(strstr(log_text,
                            "sgi-mace: read offset 0x310000 size 4"));
    g_assert_nonnull(strstr(log_text,
                            "sgi-mace: read offset 0x310010 size 4"));
    g_assert_nonnull(strstr(log_text,
                            "sgi-mace: read offset 0x310028 size 4"));
    g_assert_cmpint(g_remove(log_path), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    g_assert_cmpint(g_unlink(image), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/o2-id/read-rom", test_machine_read_rom);
    qtest_add_func("/sgi/o2-id/misc-word-protocol",
                   test_misc_word_protocol);
    qtest_add_func("/sgi/o2-id/options", test_machine_options);
    qtest_add_func("/sgi/o2-id/memory-reset-mac",
                   test_machine_memory_reset_and_mac);
    qtest_add_func("/sgi/o2-id/migration",
                   test_machine_migration_boundary);
    qtest_add_func("/o2/identity/misc-word-default-off",
                   test_misc_word_pio_default_off);
    qtest_add_func("/o2/identity/diagnostic-disabled",
                   test_identity_diagnostic_disabled);
    qtest_add_func("/o2/identity/misc-word-reset-timing",
                   test_misc_word_reset_timing);
    qtest_add_func("/o2/identity/misc-word-presence-late-timers",
                   test_misc_word_presence_without_timer_service);
    qtest_add_func("/o2/identity/misc-word-session-late-timers",
                   test_misc_word_session_without_timer_service);
    qtest_add_func("/o2/identity/misc-word-selected",
                   test_misc_word_pio_selected);
    return g_test_run();
}
