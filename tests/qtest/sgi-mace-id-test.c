/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* MACE identity line diagnostic tests. */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest.h"

#define MACE_ID_REG 0x1f310008ULL
#define MACE_ID_RELEASE (1ULL << 2)
#define MACE_ID_SAMPLE (1ULL << 3)

static uint8_t eprom_byte(unsigned address)
{
    return (address * 37) ^ 0xa5;
}

static char *id_image_file(void)
{
    char *path = g_strdup_printf("%s/mace-id-XXXXXX", g_get_tmp_dir());
    uint8_t bytes[128];
    int fd;

    for (size_t i = 0; i < sizeof(bytes); i++) {
        bytes[i] = eprom_byte(i);
    }
    fd = g_mkstemp(path);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, bytes, sizeof(bytes)), ==, sizeof(bytes));
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *id_start(const char *image)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 "
                                  "-device sgi-mace-id,id=identity,"
                                  "test-serial=1032547698ba,"
                                  "eprom-file=%s,status=0123456789abcd",
                                  image);

    qtest_sysbus_mmio_map(qts, "/machine/peripheral/identity", 0,
                          MACE_ID_REG);
    return qts;
}

static void id_stop(QTestState *qts, const char *image)
{
    qtest_quit(qts);
    g_assert_cmpint(g_unlink(image), ==, 0);
}

static bool id_level(QTestState *qts)
{
    return qtest_readq(qts, MACE_ID_REG) & MACE_ID_SAMPLE;
}

static void id_write_bit(QTestState *qts, bool bit)
{
    qtest_writeq(qts, MACE_ID_REG, 0);
    qtest_clock_step(qts, bit ? 6000 : 80000);
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, bit ? 114000 : 40000);
}

static void id_pulse_width(QTestState *qts, int64_t low_ns)
{
    qtest_writeq(qts, MACE_ID_REG, 0);
    qtest_clock_step(qts, low_ns);
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, 120000 - low_ns);
}

static bool id_read_bit_width(QTestState *qts, int64_t low_ns)
{
    bool level;

    qtest_writeq(qts, MACE_ID_REG, 0);
    qtest_clock_step(qts, low_ns);
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, 25000 - low_ns);
    level = id_level(qts);
    qtest_clock_step(qts, 95000);
    return level;
}

static bool id_read_bit(QTestState *qts)
{
    bool level;

    qtest_writeq(qts, MACE_ID_REG, 0);
    qtest_clock_step(qts, 6000);
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, 9000);
    level = id_level(qts);
    qtest_clock_step(qts, 105000);
    return level;
}

static void id_write_byte(QTestState *qts, uint8_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        id_write_bit(qts, (value >> i) & 1);
    }
}

static uint8_t id_read_byte(QTestState *qts)
{
    uint8_t value = 0;

    for (unsigned i = 0; i < 8; i++) {
        value |= id_read_bit(qts) << i;
    }
    return value;
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

static void id_pulse_reset(QTestState *qts)
{
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, 500000);
    qtest_writeq(qts, MACE_ID_REG, 0);
    qtest_clock_step(qts, 520000);
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
}

static void id_reset(QTestState *qts)
{
    id_pulse_reset(qts);
    qtest_clock_step(qts, 65000);
    g_assert_false(id_level(qts));
    qtest_clock_step(qts, 100000);
    g_assert_true(id_level(qts));
}

static void id_select_skip(QTestState *qts)
{
    id_reset(qts);
    id_write_byte(qts, 0xcc);
}

static void id_read_header(QTestState *qts, uint8_t cmd, uint16_t address)
{
    const uint8_t header[] = { cmd, address & 0xff, address >> 8 };

    id_write_byte(qts, header[0]);
    id_write_byte(qts, header[1]);
    id_write_byte(qts, header[2]);
    g_assert_cmphex(id_read_byte(qts), ==, id_crc8(header, sizeof(header)));
}

static void id_select_search(QTestState *qts)
{
    static const uint8_t identity[7] = {
        0x09, 0x10, 0x32, 0x54, 0x76, 0x98, 0xba
    };

    id_reset(qts);
    id_write_byte(qts, 0xf0);
    for (unsigned bit = 0; bit < 64; bit++) {
        uint8_t byte = bit < 56 ? identity[bit / 8] :
                       id_crc8(identity, sizeof(identity));
        bool expected = (byte >> (bit % 8)) & 1;

        g_assert_cmpint(id_read_bit(qts), ==, expected);
        g_assert_cmpint(id_read_bit(qts), ==, !expected);
        id_write_bit(qts, expected);
    }
}

static void test_reset_presence(void)
{
    g_autofree char *image = id_image_file();
    QTestState *qts;

    qts = id_start(image);
    g_assert_cmphex(qtest_readq(qts, MACE_ID_REG) &
                    (MACE_ID_RELEASE | MACE_ID_SAMPLE), ==, 0);

    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    g_assert_cmphex(qtest_readq(qts, MACE_ID_REG) & MACE_ID_SAMPLE, ==,
                    MACE_ID_SAMPLE);
    qtest_clock_step(qts, 500000);
    qtest_writeq(qts, MACE_ID_REG, 0);
    qtest_clock_step(qts, 100000);
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, 200000);
    g_assert_cmphex(qtest_readq(qts, MACE_ID_REG) & MACE_ID_SAMPLE, ==,
                    MACE_ID_SAMPLE);

    qtest_clock_step(qts, 500000);
    qtest_writeq(qts, MACE_ID_REG, 0);
    qtest_clock_step(qts, 520000);
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, 29000);
    g_assert_cmphex(qtest_readq(qts, MACE_ID_REG) & MACE_ID_SAMPLE, ==,
                    MACE_ID_SAMPLE);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readq(qts, MACE_ID_REG) & MACE_ID_SAMPLE, ==, 0);
    qtest_clock_step(qts, 120000);
    g_assert_cmphex(qtest_readq(qts, MACE_ID_REG) & MACE_ID_SAMPLE, ==,
                    MACE_ID_SAMPLE);
    id_stop(qts, image);
}

static void test_rom_selection(void)
{
    static const uint8_t identity[7] = {
        0x09, 0x10, 0x32, 0x54, 0x76, 0x98, 0xba
    };
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);
    uint8_t tail[2];

    id_reset(qts);
    id_write_byte(qts, 0x33);
    for (size_t i = 0; i < sizeof(identity); i++) {
        g_assert_cmphex(id_read_byte(qts), ==, identity[i]);
    }
    g_assert_cmphex(id_read_byte(qts), ==,
                    id_crc8(identity, sizeof(identity)));

    id_reset(qts);
    id_write_byte(qts, 0x55);
    for (size_t i = 0; i < sizeof(identity); i++) {
        id_write_byte(qts, identity[i]);
    }
    id_write_byte(qts, id_crc8(identity, sizeof(identity)) ^ 1);
    id_write_byte(qts, 0xf0);
    id_write_byte(qts, 0);
    id_write_byte(qts, 0);
    g_assert_cmphex(id_read_byte(qts), ==, 0xff);

    id_select_search(qts);
    id_read_header(qts, 0xf0, 0x7e);
    g_assert_cmphex(id_read_byte(qts), ==, eprom_byte(0x7e));
    g_assert_cmphex(id_read_byte(qts), ==, eprom_byte(0x7f));
    tail[0] = eprom_byte(0x7e);
    tail[1] = eprom_byte(0x7f);
    g_assert_cmphex(id_read_byte(qts), ==, id_crc8(tail, sizeof(tail)));
    g_assert_cmphex(id_read_byte(qts), ==, 0xff);
    id_stop(qts, image);
}

static void test_match_and_search_rejection(void)
{
    static const uint8_t identity[7] = {
        0x09, 0x10, 0x32, 0x54, 0x76, 0x98, 0xba
    };
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);

    id_reset(qts);
    id_write_byte(qts, 0x55);
    for (size_t i = 0; i < sizeof(identity); i++) {
        id_write_byte(qts, identity[i]);
    }
    id_write_byte(qts, id_crc8(identity, sizeof(identity)));
    id_read_header(qts, 0xf0, 0x34);
    g_assert_cmphex(id_read_byte(qts), ==, eprom_byte(0x34));

    id_reset(qts);
    id_write_byte(qts, 0xf0);
    g_assert_cmpint(id_read_bit(qts), ==, true);
    g_assert_cmpint(id_read_bit(qts), ==, false);
    id_write_bit(qts, false);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_true(id_read_bit(qts));
    }
    id_stop(qts, image);
}

static void test_read_slot_window(void)
{
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);

    id_reset(qts);
    id_write_byte(qts, 0x33);
    g_assert_true(id_read_bit(qts));  /* bit 0 of family 09h */
    qtest_writeq(qts, MACE_ID_REG, 0);
    qtest_clock_step(qts, 6000);
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, 8000);
    g_assert_false(id_level(qts));
    qtest_clock_step(qts, 1000);
    g_assert_false(id_level(qts));   /* bit 1 of family 09h */
    qtest_clock_step(qts, 45000);
    g_assert_true(id_level(qts));
    id_stop(qts, image);
}

static void test_early_read_rom(void)
{
    static const uint8_t rom_prefix[7] = {
        0x09, 0x10, 0x32, 0x54, 0x76, 0x98, 0xba
    };
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);
    uint8_t rom[8];

    memcpy(rom, rom_prefix, sizeof(rom_prefix));
    rom[7] = id_crc8(rom_prefix, sizeof(rom_prefix));
    id_reset(qts);
    id_write_byte(qts, 0x33);
    for (unsigned bit = 0; bit < 64; bit++) {
        bool expected = (rom[bit / 8] >> (bit % 8)) & 1;

        qtest_writeq(qts, MACE_ID_REG, 0);
        qtest_clock_step(qts, 8088);
        qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
        qtest_clock_step(qts, 1168);
        g_assert_cmpint(id_level(qts), ==, expected);
        g_assert_cmpint(id_level(qts), ==, expected);
        qtest_clock_step(qts, 5744);
        g_assert_cmpint(id_level(qts), ==, expected);
        qtest_clock_step(qts, 45000);
        g_assert_true(id_level(qts));
        qtest_clock_step(qts, 48520);
        g_assert_true(id_level(qts));
        qtest_clock_step(qts, 32);
    }

    id_reset(qts);
    id_write_byte(qts, 0x33);
    g_assert_true(id_read_bit(qts));
    qtest_writeq(qts, MACE_ID_REG, 0);
    qtest_clock_step(qts, 8088);
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, 1168);
    g_assert_false(id_level(qts));
    qtest_writeq(qts, MACE_ID_REG, 0);
    qtest_clock_step(qts, 520000);
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, 65000);
    g_assert_true(id_level(qts));
    for (unsigned bit = 0; bit < 8; bit++) {
        g_assert_true(id_read_bit(qts));
    }
    id_reset(qts);
    id_write_byte(qts, 0x33);
    g_assert_true(id_read_bit(qts));
    g_assert_false(id_read_bit(qts));
    id_stop(qts, image);
}

static void test_observed_slot_widths_read_rom(void)
{
    static const int64_t command_low_ns[8] = {
        16792, 21922, 98217, 98307, 15640, 15660, 97004, 96955
    };
    static const int64_t read_low_ns[8] = {
        15800, 15589, 16111, 15830, 14798, 14848, 14948, 15570
    };
    static const uint8_t rom_prefix[7] = {
        0x09, 0x10, 0x32, 0x54, 0x76, 0x98, 0xba
    };
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);

    id_reset(qts);
    for (unsigned bit = 0; bit < 8; bit++) {
        g_assert_cmpint(command_low_ns[bit] < 60000, ==,
                        !!(0x33 & (1U << bit)));
        id_pulse_width(qts, command_low_ns[bit]);
    }
    for (unsigned bit = 0; bit < 64; bit++) {
        uint8_t byte = bit < 56 ? rom_prefix[bit / 8] :
                       id_crc8(rom_prefix, sizeof(rom_prefix));

        g_assert_cmpint(id_read_bit_width(qts, read_low_ns[bit % 8]), ==,
                        !!(byte & (1U << (bit % 8))));
    }
    id_stop(qts, image);
}

static void test_wider_command_one(void)
{
    static const int64_t widths_ns[] = { 1000, 14000, 15000, 22000, 23000 };
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);

    for (size_t i = 0; i < G_N_ELEMENTS(widths_ns); i++) {
        id_reset(qts);
        id_pulse_width(qts, widths_ns[i]);
        for (unsigned bit = 1; bit < 8; bit++) {
            id_write_bit(qts, (0x33 >> bit) & 1);
        }
        g_assert_cmphex(id_read_byte(qts), ==, 0x09);
    }
    id_stop(qts, image);
}

static void test_wider_read_slots(void)
{
    static const int64_t widths_ns[] = { 1000, 14000, 15000, 22000, 23000 };
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);

    for (size_t i = 0; i < G_N_ELEMENTS(widths_ns); i++) {
        id_reset(qts);
        id_write_byte(qts, 0x33);
        g_assert_true(id_read_bit_width(qts, widths_ns[i]));
        g_assert_false(id_read_bit_width(qts, widths_ns[i]));
    }
    id_stop(qts, image);
}

static void test_invalid_slot_widths(void)
{
    static const int64_t invalid_ns[] = { 999, 24000, 59000 };
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);

    for (size_t i = 0; i < G_N_ELEMENTS(invalid_ns); i++) {
        id_reset(qts);
        id_pulse_width(qts, invalid_ns[i]);
        id_write_byte(qts, 0x33);
        g_assert_cmphex(id_read_byte(qts), ==, 0x09);

        id_reset(qts);
        id_write_byte(qts, 0x33);
        id_pulse_width(qts, invalid_ns[i]);
        g_assert_true(id_read_bit(qts));
        g_assert_false(id_read_bit(qts));
    }

    id_reset(qts);
    id_pulse_width(qts, 60000);
    for (unsigned bit = 1; bit < 8; bit++) {
        id_write_bit(qts, (0xf0 >> bit) & 1);
    }
    g_assert_true(id_read_bit(qts));
    g_assert_false(id_read_bit(qts));
    id_stop(qts, image);
}

static void test_read_slot_without_timer_service(void)
{
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);

    id_reset(qts);
    id_write_byte(qts, 0x33);
    g_assert_true(id_read_bit(qts));
    qtest_clock_timers(qts, false);
    qtest_writeq(qts, MACE_ID_REG, 0);
    qtest_clock_step(qts, 6000);
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, 8000);
    g_assert_false(id_level(qts));
    qtest_clock_step(qts, 1000);
    g_assert_false(id_level(qts));
    qtest_clock_step(qts, 44999);
    g_assert_false(id_level(qts));
    qtest_clock_step(qts, 1);
    g_assert_true(id_level(qts));
    qtest_clock_timers(qts, true);
    qtest_clock_step(qts, 1);
    g_assert_true(id_level(qts));
    id_stop(qts, image);
}

static void test_memory_and_status(void)
{
    static const uint8_t status[8] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0x00
    };
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);
    uint8_t page_tail[2];

    id_select_skip(qts);
    id_read_header(qts, 0xc3, 0x1e);
    page_tail[0] = eprom_byte(0x1e);
    page_tail[1] = eprom_byte(0x1f);
    g_assert_cmphex(id_read_byte(qts), ==, page_tail[0]);
    g_assert_cmphex(id_read_byte(qts), ==, page_tail[1]);
    g_assert_cmphex(id_read_byte(qts), ==,
                    id_crc8(page_tail, sizeof(page_tail)));
    g_assert_cmphex(id_read_byte(qts), ==, eprom_byte(0x20));

    id_select_skip(qts);
    id_read_header(qts, 0xc3, 0x7f);
    g_assert_cmphex(id_read_byte(qts), ==, eprom_byte(0x7f));
    page_tail[0] = eprom_byte(0x7f);
    g_assert_cmphex(id_read_byte(qts), ==, id_crc8(page_tail, 1));
    g_assert_cmphex(id_read_byte(qts), ==, 0xff);

    id_select_skip(qts);
    id_read_header(qts, 0xaa, 0);
    for (size_t i = 0; i < sizeof(status); i++) {
        g_assert_cmphex(id_read_byte(qts), ==, status[i]);
    }
    g_assert_cmphex(id_read_byte(qts), ==, id_crc8(status, sizeof(status)));
    g_assert_cmphex(id_read_byte(qts), ==, 0xff);
    id_stop(qts, image);
}

static void test_rejected_commands_and_image_copy(void)
{
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);
    uint8_t replacement[128] = { 0 };

    g_assert_true(g_file_set_contents(image, (char *)replacement,
                                      sizeof(replacement), NULL));
    id_select_skip(qts);
    id_write_byte(qts, 0xf0);
    id_write_byte(qts, 0x80);
    id_write_byte(qts, 0);
    g_assert_cmphex(id_read_byte(qts), ==, 0xff);
    g_assert_cmphex(id_read_byte(qts), ==, 0xff);

    id_select_skip(qts);
    id_write_byte(qts, 0xc3);
    id_write_byte(qts, 0x80);
    id_write_byte(qts, 0);
    g_assert_cmphex(id_read_byte(qts), ==, 0xff);

    id_select_skip(qts);
    id_write_byte(qts, 0xaa);
    id_write_byte(qts, 8);
    id_write_byte(qts, 0);
    g_assert_cmphex(id_read_byte(qts), ==, 0xff);

    id_select_skip(qts);
    id_write_byte(qts, 0x0f);
    id_write_byte(qts, 0xf0);
    id_write_byte(qts, 0);
    id_write_byte(qts, 0);
    g_assert_cmphex(id_read_byte(qts), ==, 0xff);

    id_select_skip(qts);
    id_write_byte(qts, 0x55);
    id_write_byte(qts, 0xf0);
    id_write_byte(qts, 0);
    id_write_byte(qts, 0);
    g_assert_cmphex(id_read_byte(qts), ==, 0xff);

    id_select_skip(qts);
    id_write_byte(qts, 0x42);
    id_write_byte(qts, 0xf0);
    id_write_byte(qts, 0);
    id_write_byte(qts, 0);
    g_assert_cmphex(id_read_byte(qts), ==, 0xff);

    id_select_skip(qts);
    id_read_header(qts, 0xf0, 0);
    g_assert_cmphex(id_read_byte(qts), ==, eprom_byte(0));
    id_stop(qts, image);
}

static void test_machine_reset_state(void)
{
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);

    id_pulse_reset(qts);
    qtest_clock_step(qts, 40000);
    g_assert_false(id_level(qts));
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(qts, MACE_ID_REG) &
                    (MACE_ID_RELEASE | MACE_ID_SAMPLE), ==, 0);
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, 500000);
    g_assert_true(id_level(qts));

    id_reset(qts);
    id_write_byte(qts, 0xf0);
    g_assert_true(id_read_bit(qts));
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(qts, MACE_ID_REG) &
                    (MACE_ID_RELEASE | MACE_ID_SAMPLE), ==, 0);

    id_select_skip(qts);
    id_read_header(qts, 0xf0, 0);
    g_assert_cmphex(id_read_byte(qts), ==, eprom_byte(0));
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(qts, MACE_ID_REG) &
                    (MACE_ID_RELEASE | MACE_ID_SAMPLE), ==, 0);
    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, 500000);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_true(id_read_bit(qts));
    }
    id_reset(qts);
    id_write_byte(qts, 0x33);
    g_assert_cmphex(id_read_byte(qts), ==, 0x09);
    id_stop(qts, image);
}

static void test_machine_reset_during_search(void)
{
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);

    id_reset(qts);
    id_write_byte(qts, 0xf0);
    g_assert_true(id_read_bit(qts));
    g_assert_false(id_read_bit(qts));
    id_write_bit(qts, true);
    g_assert_false(id_read_bit(qts)); /* next ROM bit if Search continues */

    id_reset(qts);
    id_write_byte(qts, 0xf0);
    g_assert_true(id_read_bit(qts));
    g_assert_false(id_read_bit(qts));
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(qts, MACE_ID_REG) &
                    (MACE_ID_RELEASE | MACE_ID_SAMPLE), ==, 0);

    qtest_writeq(qts, MACE_ID_REG, MACE_ID_RELEASE);
    qtest_clock_step(qts, 500000);
    id_write_bit(qts, true); /* matching branch if Search ROM survived */
    g_assert_true(id_read_bit(qts));
    id_write_byte(qts, 0xf0);
    g_assert_true(id_read_bit(qts));
    g_assert_true(id_read_bit(qts));

    id_select_search(qts);
    id_read_header(qts, 0xf0, 0x42);
    g_assert_cmphex(id_read_byte(qts), ==, eprom_byte(0x42));
    id_stop(qts, image);
}

static void id_assert_migration_blocked(QTestState *qts)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'migrate','arguments':{'uri':'file:/dev/null'}}");
    g_autofree char *save_reply = qtest_hmp(qts, "savevm identity-diagnostic");
    QDict *error = qdict_get_qdict(reply, "error");

    g_assert_nonnull(error);
    g_assert_nonnull(strstr(qdict_get_str(error, "desc"),
                            "identity diagnostic"));
    g_assert_nonnull(strstr(save_reply, "identity diagnostic"));
}

static void test_absent_device_has_no_identity_blocker(void)
{
    QTestState *qts = qtest_init("-M none -cpu R10000");
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'migrate','arguments':{'uri':'file:/dev/null'}}");
    QDict *error = qdict_get_qdict(reply, "error");

    if (error) {
        g_assert_null(strstr(qdict_get_str(error, "desc"),
                             "identity diagnostic"));
    }
    qtest_quit(qts);
}

static void test_immutable_config_and_migration(void)
{
    g_autofree char *image = id_image_file();
    QTestState *qts = id_start(image);
    g_autoptr(QDict) change;

    change = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/identity',"
        "'property':'test-serial','value':'000000000001'}}");
    g_assert_nonnull(qdict_get_qdict(change, "error"));
    qobject_unref(change);
    change = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/identity',"
        "'property':'status','value':'00000000000000'}}");
    g_assert_nonnull(qdict_get_qdict(change, "error"));
    qobject_unref(change);
    change = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/identity',"
        "'property':'eprom-file','value':'/dev/null'}}");
    g_assert_nonnull(qdict_get_qdict(change, "error"));
    id_assert_migration_blocked(qts);

    id_select_skip(qts);
    id_read_header(qts, 0xf0, 0x19);
    g_assert_cmphex(id_read_byte(qts), ==, eprom_byte(0x19));
    id_assert_migration_blocked(qts);
    id_stop(qts, image);
}

static void id_assert_start_rejected(const char *device, const char *reason)
{
    const char *binary = qtest_qemu_binary(NULL);
    g_autofree char *timeout = g_find_program_in_path("timeout");
    const char *argv[] = {
        timeout, "5", binary, "-M", "none", "-cpu", "R10000",
        "-nodefaults", "-display", "none", "-qtest", "stdio",
        "-accel", "qtest", "-device", device, NULL
    };
    g_autofree char *stderr_text = NULL;
    int status = 0;

    g_assert_nonnull(timeout);
    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL, 0, NULL, NULL,
                               NULL, &stderr_text, &status, NULL));
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), !=, 0);
    g_assert_cmpint(WEXITSTATUS(status), !=, 124);
    if (!stderr_text || !strstr(stderr_text, reason)) {
        g_error("unexpected startup error: %s", stderr_text ?: "(none)");
    }
}

static void test_missing_and_wrong_size_input(void)
{
    g_autofree char *image = id_image_file();
    g_autofree char *device = NULL;
    uint8_t truncated[127] = { 0 };

    id_assert_start_rejected("sgi-mace-id,id=identity", "needs 12 hexadecimal");
    g_assert_true(g_file_set_contents(image, (char *)truncated,
                                      sizeof(truncated), NULL));
    device = g_strdup_printf("sgi-mace-id,id=identity,"
                             "test-serial=1032547698ba,"
                             "status=0123456789abcd,eprom-file=%s", image);
    id_assert_start_rejected(device, "exactly 128 bytes");
    g_assert_cmpint(g_unlink(image), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/mace-id/reset-presence", test_reset_presence);
    qtest_add_func("/sgi/mace-id/rom-selection", test_rom_selection);
    qtest_add_func("/sgi/mace-id/match-search-rejection",
                   test_match_and_search_rejection);
    qtest_add_func("/sgi/mace-id/read-slot-window", test_read_slot_window);
    qtest_add_func("/sgi/mace-id/early-read-rom", test_early_read_rom);
    qtest_add_func("/sgi/mace-id/observed-slot-widths-rom",
                   test_observed_slot_widths_read_rom);
    qtest_add_func("/sgi/mace-id/wider-command-one",
                   test_wider_command_one);
    qtest_add_func("/sgi/mace-id/wider-read-slots",
                   test_wider_read_slots);
    qtest_add_func("/sgi/mace-id/invalid-slot-widths",
                   test_invalid_slot_widths);
    qtest_add_func("/sgi/mace-id/read-slot-late-timers",
                   test_read_slot_without_timer_service);
    qtest_add_func("/sgi/mace-id/memory-status", test_memory_and_status);
    qtest_add_func("/sgi/mace-id/rejected-and-copy",
                   test_rejected_commands_and_image_copy);
    qtest_add_func("/sgi/mace-id/machine-reset", test_machine_reset_state);
    qtest_add_func("/sgi/mace-id/machine-reset-search",
                   test_machine_reset_during_search);
    qtest_add_func("/sgi/mace-id/immutable-migration",
                   test_immutable_config_and_migration);
    qtest_add_func("/sgi/mace-id/absent-migration",
                   test_absent_device_has_no_identity_blocker);
    qtest_add_func("/sgi/mace-id/input-validation",
                   test_missing_and_wrong_size_input);
    return g_test_run();
}
