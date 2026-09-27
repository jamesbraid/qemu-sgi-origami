/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native DS1742 register contract, without an IOC4 board mapping. */
#include "qemu/osdep.h"
#include "qemu/bcd.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-migrate.h"

#define PATH "/machine/peripheral/clock"
#define BASE UINT64_C(0x10000000)
#define REG(n) (BASE + (n))
#define NS_PER_SEC UINT64_C(1000000000)

static QTestState *start(const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
                                  "-rtc base=2024-01-01T00:00:00 "
                                  "-device ds1742,id=clock %s", extra);
    qtest_sysbus_mmio_map(qts, PATH, 0, BASE);
    return qts;
}

static uint8_t rd(QTestState *qts, unsigned offset)
{
    return qtest_readb(qts, REG(offset));
}

static void wr(QTestState *qts, unsigned offset, uint8_t value)
{
    qtest_writeb(qts, REG(offset), value);
}

static void set_time(QTestState *qts, uint8_t second, uint8_t minute,
                     uint8_t hour, uint8_t date, uint8_t month,
                     uint8_t year, uint8_t century)
{
    wr(qts, 0x7f8, 0x80);
    wr(qts, 0x7f9, second);
    wr(qts, 0x7fa, minute);
    wr(qts, 0x7fb, hour);
    wr(qts, 0x7fc, 0x02);
    wr(qts, 0x7fd, date);
    wr(qts, 0x7fe, month);
    wr(qts, 0x7ff, year);
    wr(qts, 0x7f8, century);
}

static void test_calendar(void)
{
    QTestState *qts = start("");

    wr(qts, 0, 0xa5);
    wr(qts, 0x7f7, 0x5a);
    g_assert_cmphex(rd(qts, 0), ==, 0xa5);
    g_assert_cmphex(rd(qts, 0x7f7), ==, 0x5a);
    qtest_writew(qts, REG(0x7f6), 0xabcd);
    g_assert_cmphex(rd(qts, 0x7f6), ==, 0);
    g_assert_cmphex(rd(qts, 0x7f7), ==, 0x5a);
    wr(qts, 0x7f5, 0x11);
    qtest_writew(qts, REG(0x7f5), 0x1234);
    g_assert_cmphex(rd(qts, 0x7f5), ==, 0x11);
    g_assert_cmphex(rd(qts, 0x7f6), ==, 0);
    qtest_writew(qts, REG(0x7f8), 0x8000);
    g_assert_cmphex(rd(qts, 0x7f8), ==, 0x20);
    g_assert_cmphex(rd(qts, 0x7f8) & 0x3f, ==, 0x20);
    wr(qts, 0x7f8, 0x40);
    g_assert_cmphex(rd(qts, 0x7f8), ==, 0x60);
    wr(qts, 0x7f8, 0x00);
    g_assert_cmphex(rd(qts, 0x7f8), ==, 0x20);
    wr(qts, 0x7f8, 0x80);
    g_assert_cmphex(rd(qts, 0x7f8), ==, 0xa0);
    wr(qts, 0x7f8, 0x20);
    set_time(qts, 0x59, 0x59, 0x23, 0x31, 0x12, 0x24, 0x20);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 0x59);
    g_assert_cmphex(rd(qts, 0x7fc) & 0x80, ==, 0x80);
    g_assert_cmphex(rd(qts, 0x7fc) & 7, ==, 2);
    qtest_clock_step(qts, NS_PER_SEC);
    g_assert_cmphex(rd(qts, 0x7fc) & 7, ==, 3);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 0);
    g_assert_cmphex(rd(qts, 0x7fd), ==, 1);
    g_assert_cmphex(rd(qts, 0x7fe), ==, 1);
    g_assert_cmphex(rd(qts, 0x7ff), ==, 0x25);
    set_time(qts, 0x59, 0x59, 0x23, 0x31, 0x12, 0x99, 0x20);
    qtest_clock_step(qts, NS_PER_SEC);
    g_assert_cmphex(rd(qts, 0x7f8) & 0x3f, ==, 0x21);
    g_assert_cmphex(rd(qts, 0x7ff), ==, 0);
    wr(qts, 0x7f8, 0x80);
    wr(qts, 0x7fe, 0);
    wr(qts, 0x7f8, 0x19);
    g_assert_cmphex(rd(qts, 0x7f8), ==, 0x21);
    g_assert_cmphex(rd(qts, 0x7fe), ==, 1);
    wr(qts, 0x7f8, 0x40);
    qtest_clock_step(qts, 2 * NS_PER_SEC);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 0);
    wr(qts, 0x7f8, 0);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 2);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(rd(qts, 0), ==, 0xa5);
    g_assert_cmphex(rd(qts, 0x7f7), ==, 0x5a);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 2);
    qtest_quit(qts);
}

static void test_invalid_dates(void)
{
    QTestState *qts = start("");

    set_time(qts, 0x80 | 0x12, 0x34, 0x05, 0x15, 0x01, 0x24, 0x20);
    set_time(qts, 0x59, 0x59, 0x23, 0x30, 0x02, 0x24, 0x20);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 0x92);
    g_assert_cmphex(rd(qts, 0x7fd), ==, 0x15);
    g_assert_cmphex(rd(qts, 0x7fe), ==, 0x01);
    g_assert_cmphex(rd(qts, 0x7ff), ==, 0x24);
    set_time(qts, 0x59, 0x59, 0x23, 0x29, 0x02, 0x23, 0x20);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 0x92);
    g_assert_cmphex(rd(qts, 0x7fd), ==, 0x15);
    g_assert_cmphex(rd(qts, 0x7fe), ==, 0x01);
    g_assert_cmphex(rd(qts, 0x7ff), ==, 0x24);
    set_time(qts, 0x59, 0x59, 0x23, 0x29, 0x02, 0x24, 0x20);
    qtest_clock_step(qts, NS_PER_SEC);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 0);
    g_assert_cmphex(rd(qts, 0x7fd), ==, 1);
    g_assert_cmphex(rd(qts, 0x7fe), ==, 3);
    g_assert_cmphex(rd(qts, 0x7ff), ==, 0x24);
    qtest_quit(qts);
}

static void test_reset_with_write(void)
{
    QTestState *qts = start("");

    set_time(qts, 0x80 | 0x12, 0x34, 0x05, 0x15, 0x01, 0x24, 0x20);
    wr(qts, 0x7f8, 0x80);
    wr(qts, 0x7f9, 0x80 | 0x45);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(rd(qts, 0x7f8), ==, 0xa0);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 0xc5);
    wr(qts, 0x7f8, 0x20);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 0xc5);
    qtest_quit(qts);
}

static void test_reset_with_read(void)
{
    QTestState *qts = start("");

    set_time(qts, 0x12, 0x34, 0x05, 0x15, 0x01, 0x24, 0x20);
    wr(qts, 0x7f8, 0x40);
    qtest_clock_step(qts, 2 * NS_PER_SEC);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(rd(qts, 0x7f8), ==, 0x60);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 0x12);
    wr(qts, 0x7f8, 0);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 0x14);
    qtest_quit(qts);
}

static void test_stop_and_test_mode(void)
{
    QTestState *qts = start("");
    uint8_t bit;

    set_time(qts, 0x80 | 0x12, 0x34, 0x05, 0x06, 0x07, 0x26, 0x20);
    qtest_clock_step(qts, 3 * NS_PER_SEC);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 0x92);
    wr(qts, 0x7f8, 0x80);
    wr(qts, 0x7f9, 0x12);
    wr(qts, 0x7fc, 0xc2); /* BF is read-only; FT is writable. */
    wr(qts, 0x7f8, 0x20);
    g_assert_cmphex(rd(qts, 0x7fc), ==, 0xc2);
    g_assert_cmphex(rd(qts, 0x7f9) & 0xfe, ==, 0x12);
    bit = rd(qts, 0x7f9) & 1;
    qtest_clock_step(qts, 976562);
    g_assert_cmphex(rd(qts, 0x7f9) & 1, ==, bit ^ 1);
    wr(qts, 0x7f8, 0x80);
    wr(qts, 0x7fc, 0x02);
    wr(qts, 0x7f8, 0x20);
    bit = rd(qts, 0x7f9) & 1;
    qtest_clock_step(qts, 976562);
    g_assert_cmphex(rd(qts, 0x7f9) & 1, ==, bit);
    wr(qts, 0x7f8, 0x80);
    wr(qts, 0x7fc, 0x42);
    wr(qts, 0x7f9, 0x92);
    wr(qts, 0x7f8, 0x20);
    bit = rd(qts, 0x7f9) & 1;
    qtest_clock_step(qts, 3 * 976562);
    g_assert_cmphex(rd(qts, 0x7f9) & 1, ==, bit);
    qtest_quit(qts);
}

static void test_migration(void)
{
    g_autofree char *dir = g_dir_make_tmp("ds1742-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start("");
    QTestState *to = start("-incoming defer");

    wr(from, 0x7f7, 0x5a);
    set_time(from, 0x58, 0x59, 0x23, 0x31, 0x12, 0x29, 0x20);
    wr(from, 0x7f8, 0x40);
    qtest_clock_step(from, 4 * NS_PER_SEC);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration_status(to, "completed", 30);
    g_assert_cmphex(rd(to, 0x7f7), ==, 0x5a);
    g_assert_cmphex(rd(to, 0x7f9), ==, 0x58);
    wr(to, 0x7f8, 0);
    g_assert_cmphex(rd(to, 0x7f9), ==, 2);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

static void test_restart_persistence(void)
{
    g_autofree char *dir = g_dir_make_tmp("ds1742-store-XXXXXX", NULL);
    g_autofree char *image = g_build_filename(dir, "calendar.img", NULL);
    g_autofree char *reference = g_build_filename(dir, "clock.img", NULL);
    g_autofree char *extra = g_strdup_printf(
        "-drive if=none,id=nvram,file=%s,format=raw "
        "-drive if=none,id=reference,file=%s,format=raw "
        "-device ds1742,id=clock,drive=nvram,clock-drive=reference",
        image, reference);
    char contents[0x800] = { 0 };
    char clock_record[512] = { 0 };
    g_autofree char *saved_record = NULL;
    gsize saved_length;
    uint8_t persisted_sec;
    QTestState *qts;

    g_assert_true(g_file_set_contents(image, contents, sizeof(contents), NULL));
    g_assert_true(g_file_set_contents(reference, clock_record,
                                      sizeof(clock_record), NULL));
    qts = qtest_initf("-M none -cpu R10000 -S "
                      "-rtc base=2024-01-01T00:00:00 %s", extra);
    qtest_sysbus_mmio_map(qts, PATH, 0, BASE);
    wr(qts, 0, 0xa5);
    wr(qts, 0x7f7, 0x5a);
    set_time(qts, 0x47, 0x23, 0x10, 0x25, 0x09, 0x26, 0x20);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(reference, &saved_record,
                                       &saved_length, NULL));
    g_assert_cmpuint(saved_length, >=, 16);
    g_assert_cmpmem(saved_record, 8, "DS1742R1", 8);
    stq_le_p(saved_record + 8, g_get_real_time() / G_USEC_PER_SEC - 3600);
    g_assert_true(g_file_set_contents(reference, saved_record,
                                      saved_length, NULL));

    qts = qtest_initf("-M none -cpu R10000 -S "
                      "-rtc base=2024-01-01T00:00:00 %s", extra);
    qtest_sysbus_mmio_map(qts, PATH, 0, BASE);
    g_assert_cmphex(rd(qts, 0), ==, 0xa5);
    g_assert_cmphex(rd(qts, 0x7f7), ==, 0x5a);
    g_assert_cmphex(rd(qts, 0x7f8), ==, 0x20);
    g_assert_cmphex(rd(qts, 0x7fb), ==, 0x11);
    persisted_sec = rd(qts, 0x7f9);
    g_assert_cmpuint(from_bcd(persisted_sec), >=, 47);
    g_assert_cmpuint(from_bcd(persisted_sec), <, 59);
    g_assert_cmphex(rd(qts, 0x7fa), ==, 0x23);
    g_assert_cmphex(rd(qts, 0x7fd), ==, 0x25);
    qtest_clock_step(qts, NS_PER_SEC);
    g_assert_cmpuint(from_bcd(rd(qts, 0x7f9)), ==,
                     from_bcd(persisted_sec) + 1);
    qtest_quit(qts);

    qts = qtest_initf("-M none -cpu R10000 -S "
                      "-rtc base=2024-01-01T00:00:00 %s", extra);
    qtest_sysbus_mmio_map(qts, PATH, 0, BASE);
    set_time(qts, 0x80 | 0x12, 0x23, 0x10, 0x25, 0x09, 0x26, 0x20);
    qtest_quit(qts);
    qts = qtest_initf("-M none -cpu R10000 -S "
                      "-rtc base=2024-01-01T00:00:00 %s", extra);
    qtest_sysbus_mmio_map(qts, PATH, 0, BASE);
    qtest_clock_step(qts, 3 * NS_PER_SEC);
    g_assert_cmphex(rd(qts, 0x7f9), ==, 0x92);
    g_assert_cmphex(rd(qts, 0x7f7), ==, 0x5a);
    qtest_quit(qts);

    qts = qtest_initf("-M none -cpu R10000 -S "
                      "-rtc base=2024-01-01T00:00:00 %s", extra);
    qtest_sysbus_mmio_map(qts, PATH, 0, BASE);
    set_time(qts, 0x47, 0x23, 0x10, 0x25, 0x09, 0x26, 0x20);
    wr(qts, 0x7f8, 0x80);
    wr(qts, 0x7f9, 0x05);
    qtest_quit(qts);
    qts = qtest_initf("-M none -cpu R10000 -S "
                      "-rtc base=2024-01-01T00:00:00 %s", extra);
    qtest_sysbus_mmio_map(qts, PATH, 0, BASE);
    g_assert_cmphex(rd(qts, 0x7f8), ==, 0x20);
    g_assert_cmpuint(from_bcd(rd(qts, 0x7f9)), >=, 47);
    wr(qts, 0x7f8, 0x40);
    qtest_clock_step(qts, 3 * NS_PER_SEC);
    qtest_quit(qts);
    qts = qtest_initf("-M none -cpu R10000 -S "
                      "-rtc base=2024-01-01T00:00:00 %s", extra);
    qtest_sysbus_mmio_map(qts, PATH, 0, BASE);
    g_assert_cmphex(rd(qts, 0x7f8), ==, 0x20);
    g_assert_cmpuint(from_bcd(rd(qts, 0x7f9)), >=, 50);
    qtest_quit(qts);
    g_assert_cmpint(unlink(image), ==, 0);
    g_assert_cmpint(unlink(reference), ==, 0);
    g_assert_cmpint(rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ds1742/calendar", test_calendar);
    qtest_add_func("/ds1742/invalid-dates", test_invalid_dates);
    qtest_add_func("/ds1742/reset-with-write", test_reset_with_write);
    qtest_add_func("/ds1742/reset-with-read", test_reset_with_read);
    qtest_add_func("/ds1742/stop-test-mode", test_stop_and_test_mode);
    qtest_add_func("/ds1742/migration", test_migration);
    qtest_add_func("/ds1742/restart-persistence", test_restart_persistence);
    return g_test_run();
}
