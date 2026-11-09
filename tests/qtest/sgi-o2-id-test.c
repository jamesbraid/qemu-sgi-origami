/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* O2 MACE identity-line wiring tests. */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest.h"
#include "memtx.h"
#include "sgi-launch.h"
#include "sgi-o2-product-fixture.h"
#include "qobject/qdict.h"

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
        "-m 64M -M o2,register-test-profile=on,identity-diagnostic=on "
        "-global sgi-mace-id.test-serial=1032547698ba "
        "-global sgi-mace-id.eprom-file=%s "
        "-global sgi-mace-id.status=0123456789abcd -nic none", image);
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
    QTestState *qts = qtest_init("-m 64M -M o2,register-test-profile=on "
        "-nic none");
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
        MEMTX_TEST_DEVICE "-m 64M -M o2,register-test-profile=on,"
        "identity-diagnostic=on,"
        "mace-misc-word-pio=on "
        "-global sgi-mace-id.test-serial=1032547698ba "
        "-global sgi-mace-id.eprom-file=%s "
        "-global sgi-mace-id.status=0123456789abcd "
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

static void assert_machine_rejected_with(const char *machine,
                                         const char *extra,
                                         const char *reason)
{
    static const char *const fixed[] = {
        "-m", "64M", "-nodefaults", "-display", "none", "-S", "-qmp", "stdio",
    };
    g_auto(GStrv) words = g_strsplit(extra, " ", -1);
    g_autoptr(GPtrArray) args = g_ptr_array_new();

    g_ptr_array_add(args, (gpointer)qtest_qemu_binary(NULL));
    g_ptr_array_add(args, (gpointer)"-M");
    g_ptr_array_add(args, (gpointer)machine);
    for (char **word = words; *word; word++) {
        if (**word) {
            g_ptr_array_add(args, *word);
        }
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(fixed); i++) {
        g_ptr_array_add(args, (gpointer)fixed[i]);
    }
    g_ptr_array_add(args, NULL);
    assert_qtest_launch_rejected((const char **)args->pdata, reason);
}

static void assert_machine_rejected(const char *machine, const char *reason)
{
    assert_machine_rejected_with(machine, "", reason);
}

static void test_machine_options(void)
{
    g_autofree char *image = id_image_file();
    g_autofree char *serial_global =
        g_strdup("-global sgi-mace-id.test-serial=1032547698ba ");
    g_autofree char *image_global = g_strdup_printf(
        "-global sgi-mace-id.eprom-file=%s ", image);
    g_autofree char *status_global =
        g_strdup("-global sgi-mace-id.status=0123456789abcd ");
    g_autofree char *no_enable = g_strdup_printf(
        "%s%s%s", serial_global, image_global, status_global);
    g_autofree char *no_serial = g_strdup_printf(
        "%s%s", image_global, status_global);
    uint8_t short_image[127] = { 0 };

    /* Inputs without the diagnostic have no responder to feed. */
    assert_machine_rejected_with("o2,register-test-profile=on", no_enable,
                                 "no-responder rejects synthetic identity "
                                 "inputs");
    assert_machine_rejected_with("o2,register-test-profile=on,"
                                 "identity-diagnostic=on", "",
                                 "sgi-mace-id needs 12 hexadecimal");
    assert_machine_rejected("o2,register-test-profile=on,mace-misc-word-pio=on",
                            "requires identity-diagnostic=on");
    assert_machine_rejected_with("o2,register-test-profile=on,"
                                 "identity-diagnostic=on", no_serial,
                                 "sgi-mace-id needs 12 hexadecimal");
    g_assert_true(g_file_set_contents(image, (char *)short_image,
                                      sizeof(short_image), NULL));
    assert_machine_rejected_with("o2,register-test-profile=on,"
                                 "identity-diagnostic=on", no_enable,
                                 "exactly 128 bytes");
    /* The replay, mac and id inputs belong to the product machine. */
    assert_machine_rejected("o2,register-test-profile=on,"
                            "mac=08:00:69:00:00:01",
                            "need the product machine");
    assert_machine_rejected("o2,register-test-profile=on,hostid=69000001",
                            "need the product machine");
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
    QTestState *qts = qtest_init("-m 64M -M o2,register-test-profile=on "
        "-nic none");

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
        MEMTX_TEST_DEVICE "-m 64M -M o2,register-test-profile=on,"
        "identity-diagnostic=on "
        "-global sgi-mace-id.test-serial=1032547698ba "
        "-global sgi-mace-id.eprom-file=%s "
        "-global sgi-mace-id.status=0123456789abcd -nic none", image);
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
    QTestState *qts = qtest_init(MEMTX_TEST_DEVICE
                               "-m 64M -M o2,register-test-profile=on "
        "-nic none");

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
        MEMTX_TEST_DEVICE "-m 64M -M o2,register-test-profile=on,"
        "identity-diagnostic=on,"
        "mace-misc-word-pio=on "
        "-global sgi-mace-id.test-serial=1032547698ba "
        "-global sgi-mace-id.eprom-file=%s "
        "-global sgi-mace-id.status=0123456789abcd "
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
        MEMTX_TEST_DEVICE "-m 64M -M o2,register-test-profile=on,"
        "identity-diagnostic=on,"
        "mace-misc-word-pio=on "
        "-global sgi-mace-id.test-serial=1032547698ba "
        "-global sgi-mace-id.eprom-file=%s "
        "-global sgi-mace-id.status=0123456789abcd "
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
        MEMTX_TEST_DEVICE "-m 64M -M o2,register-test-profile=on,"
        "identity-diagnostic=on,"
        "mace-misc-word-pio=on "
        "-global sgi-mace-id.test-serial=1032547698ba "
        "-global sgi-mace-id.eprom-file=%s "
        "-global sgi-mace-id.status=0123456789abcd "
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
        0, 0, 0, 0, 0, 0, 0, 0x22
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
        MEMTX_TEST_DEVICE "-m 64M -M o2,register-test-profile=on,"
        "password-clear-input=1,"
        "identity-diagnostic=on,"
        "mace-misc-word-pio=on "
        "-global sgi-mace-id.test-serial=1032547698ba "
        "-global sgi-mace-id.eprom-file=%s "
        "-global sgi-mace-id.status=0123456789abcd "
        "-nic none -d unimp -D %s", image, log_path);

    g_assert_cmpint(memtx_readq(qts, ID_REG, &wide), ==, MEMTX_OK);
    g_assert_cmphex(wide, ==, 0x22);
    g_assert_cmpint(memtx_readl(qts, ID_REG, &high), ==, MEMTX_OK);
    g_assert_cmpint(memtx_readl(qts, ID_REG + 4, &low), ==, MEMTX_OK);
    g_assert_cmphex(high, ==, 0);
    g_assert_cmphex(low, ==, 0x22);
    g_assert_cmphex(((uint64_t)high << 32) | low, ==, wide);
    qtest_memread(qts, ID_REG, raw, sizeof(raw));
    g_assert_cmpmem(raw, sizeof(raw), reset_raw, sizeof(reset_raw));

    g_assert_cmpint(memtx_writeq(qts, ID_REG, 0x34), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x3e);
    g_assert_cmpint(memtx_writel(qts, ID_REG, 0), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x3e);
    g_assert_cmpint(memtx_writel(qts, ID_REG, 0xa5c30042), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x3e);
    g_assert_cmpint(memtx_writel(qts, ID_REG + 4, ID_RELEASE), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x0e);

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
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x0e);

    g_assert_cmphex(qtest_readq(qts, ring_base), ==, 1);
    g_assert_cmpint(memtx_readl(qts, ring_base, &low), ==, MEMTX_OK);
    g_assert_cmpint(memtx_writel(qts, ring_base, 0), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ring_base), ==, 1);
    g_assert_cmpint(memtx_readl(qts, irq_status, &low), ==, MEMTX_OK);
    qtest_writeq(qts, irq_mask, 1);
    g_assert_cmpint(memtx_writel(qts, irq_mask, 0), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, irq_mask), ==, 1);
    g_assert_cmpint(memtx_readl(qts, ID_REG + 0x20, &low), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x0e);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, ID_REG), ==, 0x22);
    g_assert_cmpint(memtx_readl(qts, ID_REG, &high), ==, MEMTX_OK);
    g_assert_cmpint(memtx_readl(qts, ID_REG + 4, &low), ==, MEMTX_OK);
    g_assert_cmphex(high, ==, 0);
    g_assert_cmphex(low, ==, 0x22);
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


/*
 * The product machine builds its own identity chip. Memory bytes 0 to 5 hold
 * the station address in reverse order, as the PROM reads them, and every
 * other byte is 0xff, the DS2502's unprogrammed state. Read ROM gives the
 * marked constant 45 4d 55, the kind and two zero bytes, and Read Status
 * gives seven 0xff and the fixed 0x00. The vectors' checksums are computed
 * by id_crc8() above, not by the model, and were also worked by hand from
 * the DS2502 polynomial x^8 + x^5 + x^4 + 1.
 */
static const uint8_t product_rom[8] = {
    0x09, 0x45, 0x4d, 0x55, 0x01, 0x00, 0x00, 0xa5
};

typedef struct ProductChip {
    const char *options;       /* appended to the product -M */
    uint8_t head[6];           /* memory bytes 0 to 5 */
    uint8_t data_crc;          /* CRC of all 128 memory bytes */
    const char *hostids;
    const char *adapter;       /* the adapter's address */
} ProductChip;

static QTestState *product_start(O2ProductTest *t, const char *options,
                                 const char *extra)
{
    return qtest_initf("-M %s%s%s -nodefaults -S %s", t->machine,
                       options[0] ? "," : "", options, extra);
}

static char *product_qom_str(QTestState *qts, const char *path,
                             const char *property)
{
    g_autoptr(QDict) response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, property);

    g_assert_true(qdict_haskey(response, "return"));
    return g_strdup(qdict_get_str(response, "return"));
}

/* Reads the whole chip over the identity line and checks it against @image. */
static void product_check_chip(QTestState *qts, const uint8_t image[128],
                               int data_crc)
{
    static const uint8_t status[8] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
    };
    const uint8_t header[3] = { 0xf0, 0, 0 };

    id_reset(qts);
    id_write_byte(qts, 0x33);
    for (unsigned i = 0; i < sizeof(product_rom); i++) {
        g_assert_cmphex(id_read_byte(qts), ==, product_rom[i]);
    }
    g_assert_cmphex(id_crc8(product_rom, 7), ==, 0xa5);

    id_reset(qts);
    id_write_byte(qts, 0xcc);
    for (unsigned i = 0; i < sizeof(header); i++) {
        id_write_byte(qts, header[i]);
    }
    g_assert_cmphex(id_read_byte(qts), ==, id_crc8(header, sizeof(header)));
    g_assert_cmphex(id_crc8(header, sizeof(header)), ==, 0x8d);
    for (unsigned i = 0; i < 128; i++) {
        g_assert_cmphex(id_read_byte(qts), ==, image[i]);
    }
    g_assert_cmphex(id_read_byte(qts), ==, id_crc8(image, 128));
    if (data_crc >= 0) {
        g_assert_cmphex(id_crc8(image, 128), ==, data_crc);
    }

    id_reset(qts);
    id_write_byte(qts, 0xcc);
    {
        const uint8_t status_header[3] = { 0xaa, 0, 0 };

        for (unsigned i = 0; i < sizeof(status_header); i++) {
            id_write_byte(qts, status_header[i]);
        }
        g_assert_cmphex(id_read_byte(qts), ==, 0x9c);
        g_assert_cmphex(id_crc8(status_header, 3), ==, 0x9c);
    }
    for (unsigned i = 0; i < sizeof(status); i++) {
        g_assert_cmphex(id_read_byte(qts), ==, status[i]);
    }
    g_assert_cmphex(id_read_byte(qts), ==, 0xfc);
    g_assert_cmphex(id_crc8(status, 8), ==, 0xfc);
}

static void test_product_chip(void)
{
    static const ProductChip chips[] = {
        { "emu-id=42", { 0xa0, 0x02, 0xe1, 0x69, 0x00, 0x08 }, 0x5c,
          "69e102a0", "08:00:69:e1:02:a0" },
        { "emu-id=0", { 0x00, 0x00, 0xe1, 0x69, 0x00, 0x08 }, 0xf1,
          "69e10000", "08:00:69:e1:00:00" },
        { "emu-id=4095", { 0xf0, 0xff, 0xe1, 0x69, 0x00, 0x08 }, 0x7a,
          "69e1fff0", "08:00:69:e1:ff:f0" },
        { "mac=08:00:69:0b:ea:40", { 0x40, 0xea, 0x0b, 0x69, 0x00, 0x08 },
          0x96, "690bea40", "08:00:69:0b:ea:40" },
        { "hostid=0bea40", { 0x40, 0xea, 0x0b, 0x69, 0x00, 0x08 }, 0x96,
          "690bea40", "08:00:69:0b:ea:40" },
        { "hostid=690bea40", { 0x40, 0xea, 0x0b, 0x69, 0x00, 0x08 }, 0x96,
          "690bea40", "08:00:69:0b:ea:40" },
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(chips); i++) {
        O2ProductTest t = { 0 };
        uint8_t image[128];
        QTestState *qts;
        g_autofree char *hostids = NULL, *hostid = NULL, *adapter = NULL;

        o2_product_prepare(&t);
        memset(image, 0xff, sizeof(image));
        memcpy(image, chips[i].head, 6);
        qts = product_start(&t, chips[i].options, "");
        hostids = product_qom_str(qts, "/machine", "hostids");
        hostid = product_qom_str(qts, "/machine", "hostid");
        adapter = product_qom_str(qts, "/machine/mace/eth", "mac");
        g_assert_cmpstr(hostids, ==, chips[i].hostids);
        g_assert_cmpstr(hostid, ==, chips[i].hostids);
        g_assert_cmpstr(adapter, ==, chips[i].adapter);
        /* QEMU never programs the station register; the firmware does. */
        g_assert_cmphex(qtest_readq(qts, UINT64_C(0x1f2800a0)), ==, 0);
        product_check_chip(qts, image, chips[i].data_crc);
        g_assert_cmphex(qtest_readq(qts, UINT64_C(0x1f2800a0)), ==, 0);
        qtest_system_reset(qts);
        g_assert_cmphex(qtest_readq(qts, UINT64_C(0x1f2800a0)), ==, 0);
        qtest_quit(qts);
        o2_product_cleanup(&t);
    }
}

static void assert_product_rejected(const char *options, const char *extra,
                                    const char *reason)
{
    static const char *const fixed[] = {
        "-nodefaults", "-display", "none", "-S", "-qmp", "stdio",
    };
    O2ProductTest t = { 0 };
    g_autofree char *spec = NULL;
    g_auto(GStrv) words = g_strsplit(extra, " ", -1);
    g_autoptr(GPtrArray) args = g_ptr_array_new();

    o2_product_prepare(&t);
    spec = g_strdup_printf("%s%s%s", t.machine, options[0] ? "," : "",
                           options);
    g_ptr_array_add(args, (gpointer)qtest_qemu_binary(NULL));
    g_ptr_array_add(args, (gpointer)"-M");
    g_ptr_array_add(args, spec);
    for (char **word = words; *word; word++) {
        if (**word) {
            g_ptr_array_add(args, *word);
        }
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(fixed); i++) {
        g_ptr_array_add(args, (gpointer)fixed[i]);
    }
    g_ptr_array_add(args, NULL);
    assert_qtest_launch_rejected((const char **)args->pdata, reason);
    o2_product_cleanup(&t);
}

static void test_product_identity_refused(void)
{
    assert_product_rejected("mac=52:54:00:12:34:56", "",
                            "station address must start 08:00:69");
    assert_product_rejected("hostid=d2100887", "", "hostid");
    assert_product_rejected("serial=M2100887", "", "serial");
    assert_product_rejected("mac=08:00:69:0b:ea:40,hostid=69000001", "",
                            "make them agree");
    assert_product_rejected("emu-id=4096", "",
                            "emu-id must be 0 to 4095, not 4096");
}

/*
 * The adapter takes the machine's address when -nic gives none, and a -nic
 * mac= that says otherwise is refused: the guest never sees the adapter's
 * address, so honouring it would only mislead.
 */
static void test_product_nic(void)
{
    O2ProductTest t = { 0 };
    g_autofree char *adapter = NULL;
    QTestState *qts;

    o2_product_prepare(&t);
    qts = product_start(&t, "emu-id=42", "-nic user");
    adapter = product_qom_str(qts, "/machine/mace/eth", "mac");
    g_assert_cmpstr(adapter, ==, "08:00:69:e1:02:a0");
    qtest_quit(qts);

    qts = product_start(&t, "emu-id=42", "-nic user,mac=08:00:69:e1:02:a0");
    g_free(adapter);
    adapter = product_qom_str(qts, "/machine/mace/eth", "mac");
    g_assert_cmpstr(adapter, ==, "08:00:69:e1:02:a0");
    qtest_quit(qts);
    o2_product_cleanup(&t);

    assert_product_rejected("emu-id=42", "-nic user,mac=08:00:69:00:00:01",
                            "-nic mac=08:00:69:00:00:01 is not the machine's "
                            "station address 08:00:69:e1:02:a0");
    assert_product_rejected("emu-id=42", "-nic user,mac=52:54:00:12:34:56",
                            "station address 08:00:69:e1:02:a0");
}

/*
 * identity-eprom-file replays a captured chip byte for byte. It wins over
 * the identity inputs, the host id comes from its bytes 3 to 0 and the
 * adapter takes the address its first six bytes spell, reversed.
 */
static void test_product_replay(void)
{
    g_autofree char *image_path = id_image_file();
    g_autofree char *options = g_strdup_printf(
        "mac=08:00:69:0b:ea:40,identity-eprom-file=%s", image_path);
    g_autofree char *wrong = g_strdup_printf(
        "identity-eprom-file=%s", image_path);
    uint8_t image[128];
    O2ProductTest t = { 0 };
    g_autofree char *hostids = NULL, *adapter = NULL;
    QTestState *qts;

    for (unsigned i = 0; i < sizeof(image); i++) {
        image[i] = id_eprom_byte(i);
    }
    o2_product_prepare(&t);
    qts = product_start(&t, options, "");
    hostids = product_qom_str(qts, "/machine", "hostids");
    adapter = product_qom_str(qts, "/machine/mace/eth", "mac");
    g_assert_cmpstr(hostids, ==, "caef80a5");
    g_assert_cmpstr(adapter, ==, "1c:31:ca:ef:80:a5");
    product_check_chip(qts, image, -1);
    qtest_quit(qts);
    o2_product_cleanup(&t);

    /* The file is read once at start: changing it afterwards does nothing. */
    o2_product_prepare(&t);
    qts = product_start(&t, options, "");
    {
        uint8_t other[128] = { 0 };
        g_autofree char *again = NULL;

        g_assert_true(g_file_set_contents(image_path, (char *)other,
                                          sizeof(other), NULL));
        again = product_qom_str(qts, "/machine", "hostids");
        g_assert_cmpstr(again, ==, "caef80a5");
        qtest_system_reset(qts);
        product_check_chip(qts, image, -1);
    }
    qtest_quit(qts);
    o2_product_cleanup(&t);

    {
        uint8_t short_image[127] = { 0 };

        g_assert_true(g_file_set_contents(image_path, (char *)short_image,
                                          sizeof(short_image), NULL));
    }
    assert_product_rejected(wrong, "", "exactly 128 bytes");
    g_assert_cmpint(g_unlink(image_path), ==, 0);
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
    qtest_add_func("/o2/identity/product-chip", test_product_chip);
    qtest_add_func("/o2/identity/product-refused",
                   test_product_identity_refused);
    qtest_add_func("/o2/identity/product-nic", test_product_nic);
    qtest_add_func("/o2/identity/product-replay", test_product_replay);
    return g_test_run();
}
