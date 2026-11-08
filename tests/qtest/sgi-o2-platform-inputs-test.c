/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* O2 board input and the existing DS1685 storage connection. */
#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest.h"
#include "qobject/qdict.h"

#define MACE 0x1f000000ULL
#define MISC (MACE + 0x310008)
#define RTC (MACE + 0x3a0000)
#define UART (MACE + 0x390000)
#define ISTAT (MACE + 0x310010)
#define IMASK (MACE + 0x310018)
#define CRIME 0x14000000ULL

static void rtc_write(QTestState *s, unsigned index, uint8_t value)
{
    qtest_writeb(s, RTC + index * 0x100, value);
}

static uint8_t rtc_read(QTestState *s, unsigned index)
{
    return qtest_readb(s, RTC + index * 0x100);
}

static QTestState *o2_start(const char *extra)
{
    return qtest_initf("-M o2,register-test-profile=on,"
        "password-clear-input=1 -m 64M "
                       "-nodefaults -rtc base=2024-01-01T00:00:00,clock=vm %s",
                       extra);
}

static void test_board_input(void)
{
    QTestState *s = o2_start("");

    g_assert_cmphex(qtest_readq(s, MISC), ==, 0x22);
    qtest_writeq(s, MISC, 0x3f);
    g_assert_cmphex(qtest_readq(s, MISC), ==, 0x37);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readq(s, MISC), ==, 0x22);
    QDict *error = qtest_qmp_assert_failure_ref(s,
        "{'execute':'qom-set','arguments':{'path':'/machine',"
        "'property':'password-clear-input','value':'0'}}");
    qobject_unref(error);
    qtest_quit(s);
}

static void test_rtc_alarm_route(void)
{
    QTestState *s = o2_start("");

    qtest_irq_intercept_out_named(s, "/machine/crime", "cpu-ip");
    /* The alarm must reach CRIME through MACE peripheral interrupt 8. */
    qtest_writeq(s, CRIME + 0x18, 1ULL << 5);
    qtest_writeq(s, IMASK, 1ULL << 8);
    rtc_write(s, 0x0b, 0x86);
    rtc_write(s, 0x00, 10);
    rtc_write(s, 0x01, 11);
    rtc_write(s, 0x03, 0xc0);
    rtc_write(s, 0x05, 0xc0);
    rtc_write(s, 0x0a, 0x20);
    rtc_write(s, 0x0b, 0x26);
    qtest_clock_step(s, 1000000000);
    g_assert_true(qtest_readq(s, ISTAT) & (1ULL << 8));
    g_assert_true(qtest_readq(s, CRIME + 0x10) & (1ULL << 5));
    g_assert_true(qtest_get_irq(s, 0));
    g_assert_true(rtc_read(s, 0x0c) & 0x20);
    g_assert_false(qtest_readq(s, ISTAT) & (1ULL << 8));
    g_assert_false(qtest_readq(s, CRIME + 0x10) & (1ULL << 5));
    g_assert_false(qtest_get_irq(s, 0));
    qtest_writeq(s, MACE + 0x310000, 0);
    qtest_writeb(s, UART + 0x700, 0x5a);
    rtc_write(s, 0x20, 0x6b);
    rtc_write(s, 0x21, 0x7c);
    g_assert_cmphex(rtc_read(s, 0x20), ==, 0x6b);
    g_assert_cmphex(rtc_read(s, 0x21), ==, 0x7c);
    g_assert_cmphex(qtest_readb(s, UART + 0x700), ==, 0x5a);
    qtest_writeb(s, MACE + 0x3b2000, 0xee);
    g_assert_cmphex(rtc_read(s, 0x20), ==, 0x6b);
    qtest_quit(s);
}

static QTestState *o2_store_start(const char *data, const char *clock,
                                 const char *base)
{
    g_autofree char *args = g_strdup_printf(
        "-drive if=none,id=nvram0,file=%s,format=raw "
        "-drive if=none,id=nvram0-clock,file=%s,format=raw "
        "-M o2,register-test-profile=on,"
        "password-clear-input=1 -m 64M -nodefaults "
        "-rtc base=%s,clock=vm", data, clock, base);

    return qtest_init(args);
}

static void test_rtc_storage(void)
{
    g_autofree char *dir = g_dir_make_tmp("o2-rtc-XXXXXX", NULL);
    g_autofree char *data = g_build_filename(dir, "data", NULL);
    g_autofree char *clock = g_build_filename(dir, "clock", NULL);
    char image[512] = { 0 };
    char reference[24] = { 0 };
    g_autofree char *stored = NULL;
    gsize length;
    QTestState *s;

    g_assert_true(g_file_set_contents(data, image, sizeof(image), NULL));
    g_assert_true(g_file_set_contents(clock, reference, sizeof(reference),
                                      NULL));
    s = o2_store_start(data, clock, "2024-01-01T00:00:00");
    rtc_write(s, 0x0b, 0x86);
    rtc_write(s, 0x00, 37);
    rtc_write(s, 0x02, 24);
    rtc_write(s, 0x04, 15);
    rtc_write(s, 0x06, 3);
    rtc_write(s, 0x07, 18);
    rtc_write(s, 0x08, 2);
    rtc_write(s, 0x09, 25);
    rtc_write(s, 0x0a, 0x30);
    rtc_write(s, 0x48, 20);
    rtc_write(s, 0x0a, 0x20);
    rtc_write(s, 0x0e, 0xa5);
    rtc_write(s, 0x3f, 0x6b);
    rtc_write(s, 0x0b, 0x06);
    qtest_clock_step(s, 2000000000);
    g_assert_cmpuint(rtc_read(s, 0x00), ==, 39);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    g_assert_cmphex(rtc_read(s, 0x0e), ==, 0xa5);
    g_assert_cmphex(rtc_read(s, 0x3f), ==, 0x6b);
    g_assert_cmpuint(rtc_read(s, 0x00), ==, 39);
    qtest_quit(s);

    g_assert_true(g_file_get_contents(data, &stored, &length, NULL));
    g_assert_cmpuint(length, ==, 512);
    g_assert_cmphex((uint8_t)stored[0x0e], ==, 0xa5);
    g_assert_cmphex((uint8_t)stored[0x3f], ==, 0x6b);
    g_clear_pointer(&stored, g_free);
    g_assert_true(g_file_get_contents(clock, &stored, &length, NULL));
    g_assert_cmpuint(length, ==, 24);
    g_assert_cmpmem(stored, 8, "DS168RF1", 8);

    /* Reopening at the saved virtual reference adds no elapsed seconds. */
    s = o2_store_start(data, clock, "2024-01-01T00:00:02");
    g_assert_cmphex(rtc_read(s, 0x0e), ==, 0xa5);
    g_assert_cmphex(rtc_read(s, 0x3f), ==, 0x6b);
    g_assert_cmpuint(rtc_read(s, 0x00), ==, 39);
    g_assert_cmpuint(rtc_read(s, 0x02), ==, 24);
    g_assert_cmpuint(rtc_read(s, 0x04), ==, 15);
    qtest_clock_step(s, 1000000000);
    g_assert_cmpuint(rtc_read(s, 0x00), ==, 40);
    qtest_quit(s);
    /* The same reference follows an explicitly advanced selected clock. */
    s = o2_store_start(data, clock, "2024-01-01T00:00:12");
    g_assert_cmpuint(rtc_read(s, 0x00), ==, 49);
    g_assert_cmphex(rtc_read(s, 0x0e), ==, 0xa5);
    g_assert_cmphex(rtc_read(s, 0x3f), ==, 0x6b);
    qtest_quit(s);
    g_assert_cmpint(g_unlink(data), ==, 0);
    g_assert_cmpint(g_unlink(clock), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

static void assert_start_rejected(const char *options, const char *reason)
{
    g_autofree char *command = g_strdup_printf(
        "timeout 5 %s -nodefaults -display none -accel qtest %s",
        qtest_qemu_binary(NULL), options);
    g_auto(GStrv) argv = NULL;
    g_autofree char *errors = NULL;
    int status;

    g_assert_true(g_shell_parse_argv(command, NULL, &argv, NULL));
    g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                               NULL, NULL, NULL, &errors, &status, NULL));
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), !=, 0);
    g_assert_cmpint(WEXITSTATUS(status), !=, 124);
    if (!errors || !strstr(errors, reason)) {
        g_error("unexpected startup error: %s", errors ?: "(none)");
    }
}

/* The one-wire CRC, polynomial x^8 + x^5 + x^4 + 1, written out bitwise. */
static uint8_t crc8(const uint8_t *bytes, unsigned length)
{
    uint8_t crc = 0;

    for (unsigned i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
        }
    }
    return crc;
}

static void read_clock_serial(QTestState *s, uint8_t bytes[8])
{
    rtc_write(s, 0x0b, 0x86);
    rtc_write(s, 0x0a, 0x30);
    for (unsigned i = 0; i < 8; i++) {
        bytes[i] = rtc_read(s, 0x40 + i);
    }
    rtc_write(s, 0x0a, 0x20);
    rtc_write(s, 0x0b, 0x06);
}

/*
 * The clock's silicon serial: model byte, six serial bytes and a CRC that
 * checks them. The six bytes are 45 4d 55 ("EMU"), the kind and the machine's
 * emu-id as two bytes. The clock is read-only and the serial survives a
 * reset.
 */
static void test_clock_serial(void)
{
    static const uint8_t marked[8] = { 0x71, 0x45, 0x4d, 0x55, 0x01, 0x00,
                                       0x2a, 0 };
    static const uint8_t top[7] = { 0x71, 0x45, 0x4d, 0x55, 0x01, 0x0f,
                                    0xff };
    uint8_t bytes[8], again[8], unset[8];
    QTestState *s = qtest_initf("-M o2,register-test-profile=on,"
        "password-clear-input=1,emu-id=42 -m 64M "
        "-nodefaults -rtc base=2024-01-01T00:00:00,clock=vm");

    read_clock_serial(s, bytes);
    g_assert_cmpmem(bytes, 7, marked, 7);
    g_assert_cmphex(crc8(bytes, 7), ==, bytes[7]);
    rtc_write(s, 0x0a, 0x30);
    for (unsigned i = 1; i < 8; i++) {
        rtc_write(s, 0x40 + i, 0);
    }
    rtc_write(s, 0x0a, 0x20);
    qtest_qmp_assert_success(s, "{'execute':'system_reset'}");
    read_clock_serial(s, again);
    g_assert_cmpmem(again, 8, bytes, 8);
    qtest_quit(s);

    /* The largest serial fills the low two bytes. */
    s = qtest_initf("-M o2,register-test-profile=on,"
        "password-clear-input=1,emu-id=4095 -m 64M "
        "-nodefaults -rtc base=2024-01-01T00:00:00,clock=vm");
    read_clock_serial(s, bytes);
    g_assert_cmpmem(bytes, 7, top, 7);
    g_assert_cmphex(crc8(bytes, 7), ==, bytes[7]);
    qtest_quit(s);

    /* Without an emu-id the marker is there with zeros: not configured. */
    s = o2_start("");
    read_clock_serial(s, unset);
    g_assert_cmphex(unset[0], ==, 0x71);
    g_assert_cmphex(crc8(unset, 7), ==, unset[7]);
    g_assert_cmpmem(unset + 1, 3, marked + 1, 3);
    g_assert_cmphex(unset[4], ==, 0x01);
    g_assert_cmphex(unset[5], ==, 0);
    g_assert_cmphex(unset[6], ==, 0);
    qtest_quit(s);
}

static void test_refused_inputs(void)
{
    g_autofree char *dir = g_dir_make_tmp("o2-rtc-invalid-XXXXXX", NULL);
    g_autofree char *data = g_build_filename(dir, "data", NULL);
    g_autofree char *clock = g_build_filename(dir, "clock", NULL);
    g_autofree char *args = NULL;
    char bytes[1024] = { 0 };

    assert_start_rejected("-M o2,register-test-profile=on,"
        "password-clear-input=2 -m 64M",
                          "password-clear-input must be 0 or 1");
    assert_start_rejected("-M o2,register-test-profile=on,"
        "rom-image=unused,rom-size=524288,"
                          "rom-offset=0,rom-pad-byte=238 -m 64M",
                          "requires password-clear-input");
    assert_start_rejected("-M none -cpu R10000 "
                          "-device sgi-mace,drive=missing",
                          "can't find value 'missing'");
    g_assert_true(g_file_set_contents(clock, bytes, 24, NULL));
    args = g_strdup_printf("-M o2,register-test-profile=on -m 64M "
        "-drive if=none,id=nvram0-clock,file=%s,format=raw", clock);
    assert_start_rejected(args, "clock-drive requires drive");
    g_clear_pointer(&args, g_free);
    g_assert_true(g_file_set_contents(data, bytes, sizeof(bytes), NULL));
    args = g_strdup_printf("-M o2,register-test-profile=on -m 64M "
        "-drive if=none,id=nvram0,file=%s,format=raw", data);
    assert_start_rejected(args, "requires exactly 512");
    g_clear_pointer(&args, g_free);
    g_assert_true(g_file_set_contents(data, bytes, 512, NULL));
    g_assert_true(g_file_set_contents(clock, bytes, 0, NULL));
    args = g_strdup_printf("-M o2,register-test-profile=on -m 64M "
        "-drive if=none,id=nvram0,file=%s,format=raw "
        "-drive if=none,id=nvram0-clock,file=%s,format=raw", data, clock);
    assert_start_rejected(args, "requires at least 24");
    g_clear_pointer(&args, g_free);
    args = g_strdup_printf("-M o2,register-test-profile=on -m 64M "
        "-drive if=none,id=nvram0,file=%s,format=raw,readonly=on", data);
    assert_start_rejected(args, "read-only");
    g_clear_pointer(&args, g_free);
    g_assert_true(g_file_set_contents(clock, bytes, 24, NULL));
    args = g_strdup_printf("-M o2,register-test-profile=on -m 64M "
        "-drive if=none,id=nvram0,file=%s,format=raw "
        "-drive if=none,id=nvram0-clock,file=%s,format=raw,readonly=on",
        data, clock);
    assert_start_rejected(args, "read-only");
    g_assert_cmpint(g_unlink(data), ==, 0);
    g_assert_cmpint(g_unlink(clock), ==, 0);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/o2-platform/board-input", test_board_input);
    qtest_add_func("/sgi/o2-platform/rtc-alarm-route", test_rtc_alarm_route);
    qtest_add_func("/sgi/o2-platform/rtc-storage", test_rtc_storage);
    qtest_add_func("/sgi/o2-platform/clock-serial", test_clock_serial);
    qtest_add_func("/sgi/o2-platform/refused-inputs", test_refused_inputs);
    return g_test_run();
}
