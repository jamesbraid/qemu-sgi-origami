/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The Timekeeper store images and clock records as the interchange page
 * (docs/system/mips/sgi/interchange.rst) states them.
 *
 * Each case builds its vector at test time, with no committed fixture: an
 * image whose calendar reads 2025-12-31 23:00:00 and a clock record that
 * says it was saved two hours before the start. The device must open both,
 * read 2026-01-01 01:00:00 (a year boundary on purpose), leave the storage
 * bytes alone, and write back a record with the same magic, the same length
 * and the instant of the save as 64-bit little-endian seconds. Another
 * emulator that reads and writes the same bytes moves a store pair between
 * emulators unchanged.
 *
 * The M48T35 and the DS1685 sit behind an IOC3 and are read through its byte
 * bus, the way a guest reads them. The DS1742 is a plain device.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "hw/pci/pci_regs.h"
#include "sgi-test.h"

#define CONSOLE_WIDGET  8
#define BRIDGE_BASE     (SPACE_IO + ((uint64_t)CONSOLE_WIDGET << 24))
#define BRIDGE_CFG0     0x020000
#define CFG0_DEVICE_STRIDE 0x1000
#define CFG0(device)    (BRIDGE_BASE + BRIDGE_CFG0 + \
                         (device) * CFG0_DEVICE_STRIDE)
#define REG_DATA        4
#define BRIDGE_DEVICE(n) (BRIDGE_BASE + 0x200 + 8 * (n) + REG_DATA)

/* The Origin 200's own IOC3 and the second one the DS1685 case adds. */
#define M48T35_DEVICE   2
#define M48T35_MMIO     0x10000000u
#define M48T35_WINDOW   (BRIDGE_BASE + 0x600000)
#define DS1685_DEVICE   3
#define DS1685_MMIO     0x10100000u
#define DS1685_WINDOW   (BRIDGE_BASE + 0x700000)

/* The byte-bus selects inside an IOC3's window. */
#define BYTEBUS_DEV0    0x80000
#define BYTEBUS_DEV1    0xa0000
#define BYTEBUS_DEV2    0xc0000

#define DS1742_PATH     "/machine/peripheral/clock"
#define DS1742_BASE     UINT64_C(0x10000000)

/* -rtc base: every part's record counts the -rtc clock. */
#define RTC_BASE        "2026-06-01T12:00:00"
#define RTC_BASE_SECONDS INT64_C(1780315200)
#define ELAPSED         7200

/* 2025-12-31 23:00:00 UTC, the calendar every image starts at. */
#define SAVED_SECONDS   INT64_C(1767222000)

/* A byte of ordinary storage that no restore or save may touch. */
#define MARKER          0xa5

typedef struct Files {
    char *dir;
    char *image;
    char *record;
} Files;

static void files_make(Files *f, const char *name)
{
    f->dir = g_dir_make_tmp("sgi-store-format-XXXXXX", NULL);
    g_assert_nonnull(f->dir);
    f->image = g_build_filename(f->dir, name, NULL);
    f->record = g_strconcat(f->image, ".clock", NULL);
}

static void files_free(Files *f)
{
    g_assert_cmpint(unlink(f->image), ==, 0);
    g_assert_cmpint(unlink(f->record), ==, 0);
    g_assert_cmpint(rmdir(f->dir), ==, 0);
    g_free(f->image);
    g_free(f->record);
    g_free(f->dir);
}

static void file_put(const char *path, const void *data, size_t size)
{
    g_assert_true(g_file_set_contents(path, data, size, NULL));
}

/* A record: magic, the instant of the save and, if 24 bytes, the counter. */
static void record_put(const Files *f, const char *magic, size_t size,
                       int64_t saved_at, int64_t counter)
{
    uint8_t record[24] = { 0 };

    g_assert_cmpuint(size, <=, sizeof(record));
    memcpy(record, magic, 8);
    stq_le_p(record + 8, saved_at);
    if (size == 24) {
        stq_le_p(record + 16, counter);
    }
    file_put(f->record, record, size);
}

/* The record after the device has exited. */
static void record_check(const Files *f, const char *magic, size_t size,
                         int64_t counter)
{
    g_autofree gchar *record = NULL;
    gsize length;
    int64_t saved_at;

    g_assert_true(g_file_get_contents(f->record, &record, &length, NULL));
    g_assert_cmpuint(length, ==, size);
    g_assert_cmpmem(record, 8, magic, 8);
    saved_at = ldq_le_p(record + 8);
    /* No clock step ran, so the save is at the -rtc base exactly. */
    g_assert_cmpint(saved_at, ==, RTC_BASE_SECONDS);
    if (size == 24) {
        g_assert_cmpint(ldq_le_p(record + 16), ==, counter);
    }
}

static void ioc3_attach(QTestState *qts, unsigned device, uint32_t mmio)
{
    qtest_writel(qts, CFG0(device) + PCI_BAR0, mmio);
    qtest_writel(qts, CFG0(device) + PCI_COMMAND, PCI_COMMAND_MEMORY);
    qtest_writel(qts, BRIDGE_DEVICE(device), DEV_MEMORY_SPACE | (mmio >> 20));
}

/*
 * M48T35: the clock is the last eight bytes of the 32 KiB image, BCD, with
 * a two-digit year the part counts from 1970 under its own leap rule.
 */
#define M48T35_SIZE     32768
#define M48T35_CLOCK    0x7ff9
#define M48T35_MARKER   0x100

static uint8_t m48t35_read(QTestState *qts, unsigned offset)
{
    return qtest_readb(qts, M48T35_WINDOW + BYTEBUS_DEV0 + offset);
}

static void test_m48t35(const void *data)
{
    bool usable = GPOINTER_TO_INT(data);
    /* seconds, minutes, hours, weekday, date, month, year */
    static const uint8_t before[7] = { 0x00, 0x00, 0x23, 0x00, 0x31, 0x12,
                                       0x55 };
    static const uint8_t after[7] = { 0x00, 0x00, 0x01, 0x00, 0x01, 0x01,
                                      0x56 };
    g_autofree uint8_t *image = g_malloc0(M48T35_SIZE);
    g_autofree gchar *saved = NULL;
    const uint8_t *expected;
    gsize length;
    Files f;
    QTestState *qts;

    files_make(&f, "nvram0.raw");
    image[M48T35_MARKER] = MARKER;
    memcpy(image + M48T35_CLOCK, before, sizeof(before));
    file_put(f.image, image, M48T35_SIZE);
    record_put(&f, usable ? "M48TREF1" : "NOTMAGIC", 16,
               RTC_BASE_SECONDS - ELAPSED, 0);

    qts = qtest_initf("-M origin200,nvram0=nvram0,nvram0-clock=nvram0-clock "
                      "-rtc base=" RTC_BASE ",clock=vm "
                      "-blockdev driver=raw,node-name=nvram0,"
                      "file.driver=file,file.filename=%s "
                      "-blockdev driver=raw,node-name=nvram0-clock,"
                      "file.driver=file,file.filename=%s",
                      f.image, f.record);
    ioc3_attach(qts, M48T35_DEVICE, M48T35_MMIO);
    /* A record that cannot be used adds no time. */
    expected = usable ? after : before;
    for (size_t i = 0; i < sizeof(after); i++) {
        g_assert_cmphex(m48t35_read(qts, M48T35_CLOCK + i), ==, expected[i]);
    }
    g_assert_cmphex(m48t35_read(qts, M48T35_MARKER), ==, MARKER);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(f.image, &saved, &length, NULL));
    g_assert_cmpuint(length, ==, M48T35_SIZE);
    g_assert_cmphex((uint8_t)saved[M48T35_MARKER], ==, MARKER);
    g_assert_cmpmem(saved + M48T35_CLOCK, sizeof(after), expected,
                    sizeof(after));
    record_check(&f, "M48TREF1", 16, 0);
    files_free(&f);
}

/*
 * DS1685: registers 0 to 9 are the calendar, here in binary with the 24-hour
 * bit, and the image is the 512-byte dump of the part's 0x140 bytes. The
 * record adds the guest calendar as seconds since 1970 to the instant of the
 * save, so a pair restores without a century of its own.
 */
#define DS1685_SIZE     512
#define DS1685_MARKER   0x0e
#define DS1685_CENTURY  0x88    /* register 0x48 of the second bank */
/* 2024-06-15 10:00:00 UTC, the image's own calendar. */
#define DS1685_IMAGE_SECONDS INT64_C(1718445600)

/*
 * The image's own calendar is 2024-06-15 10:00:00, a Saturday, deliberately not
 * the record's counter: a valid record's counter is the base, so the part must
 * read the counter plus the elapsed time, not the image plus it.
 */
typedef struct Ds1685Reg {
    uint8_t reg;
    uint8_t image;
    uint8_t after;
    uint8_t unusable;           /* what a record that cannot be used leaves */
} Ds1685Reg;

static const Ds1685Reg ds1685_clock[] = {
    { 0x00, 0, 0, 0 },          /* seconds */
    { 0x02, 0, 0, 0 },          /* minutes */
    { 0x04, 10, 1, 10 },        /* hours */
    { 0x06, 7, 5, 7 },          /* weekday: a Thursday from the counter */
    { 0x07, 15, 1, 15 },        /* date */
    { 0x08, 6, 1, 6 },          /* month */
    { 0x09, 24, 26, 24 },       /* year */
    { 0x0a, 0x20, 0x20, 0x20 }, /* register A: oscillator on */
    { 0x0b, 0x06, 0x06, 0x06 }, /* register B: binary, 24 hour */
};

static uint8_t ds1685_read(QTestState *qts, uint8_t reg)
{
    qtest_writeb(qts, DS1685_WINDOW + BYTEBUS_DEV1, reg);
    return qtest_readb(qts, DS1685_WINDOW + BYTEBUS_DEV2);
}

static void test_ds1685(const void *data)
{
    bool usable = GPOINTER_TO_INT(data);
    g_autofree uint8_t *image = g_malloc0(DS1685_SIZE);
    g_autofree gchar *saved = NULL;
    gsize length;
    Files f;
    QTestState *qts;

    files_make(&f, "nvram0.raw");
    for (size_t i = 0; i < G_N_ELEMENTS(ds1685_clock); i++) {
        image[ds1685_clock[i].reg] = ds1685_clock[i].image;
    }
    image[DS1685_MARKER] = MARKER;
    image[DS1685_CENTURY] = 20;
    file_put(f.image, image, DS1685_SIZE);
    record_put(&f, usable ? "DS168RF1" : "NOTMAGIC", 24,
               RTC_BASE_SECONDS - ELAPSED, SAVED_SECONDS);

    qts = qtest_initf("-M origin200 -rtc base=" RTC_BASE ",clock=vm "
                      "-device sgi-ioc3,id=octane,addr=3,subsystem-id=0xc304,"
                      "ds1685-drive=rtc-store "
                      "-drive if=none,id=rtc-store,file=%s,format=raw "
                      "-drive if=none,id=rtc-store-clock,file=%s,format=raw",
                      f.image, f.record);
    ioc3_attach(qts, DS1685_DEVICE, DS1685_MMIO);
    for (size_t i = 0; i < G_N_ELEMENTS(ds1685_clock); i++) {
        g_assert_cmphex(ds1685_read(qts, ds1685_clock[i].reg), ==,
                        usable ? ds1685_clock[i].after
                               : ds1685_clock[i].unusable);
    }
    g_assert_cmphex(ds1685_read(qts, DS1685_MARKER), ==, MARKER);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(f.image, &saved, &length, NULL));
    g_assert_cmpuint(length, ==, DS1685_SIZE);
    g_assert_cmphex((uint8_t)saved[DS1685_MARKER], ==, MARKER);
    for (size_t i = 0; i < G_N_ELEMENTS(ds1685_clock); i++) {
        g_assert_cmphex((uint8_t)saved[ds1685_clock[i].reg], ==,
                        usable ? ds1685_clock[i].after
                               : ds1685_clock[i].unusable);
    }
    /* The counter saved is the running one: the base plus what elapsed. */
    record_check(&f, "DS168RF1", 24,
                 usable ? SAVED_SECONDS + ELAPSED : DS1685_IMAGE_SECONDS);
    files_free(&f);
}

/*
 * DS1742: the clock is the last eight bytes of the part's storage. Byte 0 is
 * control and century (BCD), then seconds, minutes, hours, day of week, date,
 * month and year, all BCD.
 */
#define DS1742_SIZE     0x800
#define DS1742_CLOCK    0x7f8

static uint8_t ds1742_read(QTestState *qts, unsigned offset)
{
    return qtest_readb(qts, DS1742_BASE + offset);
}

static void test_ds1742(const void *data)
{
    bool usable = GPOINTER_TO_INT(data);
    /* century, seconds, minutes, hours, weekday, date, month, year */
    static const uint8_t before[8] = { 0x20, 0x00, 0x00, 0x23, 0x04, 0x31,
                                       0x12, 0x25 };
    static const uint8_t after[8] = { 0x20, 0x00, 0x00, 0x01, 0x05, 0x01,
                                      0x01, 0x26 };
    g_autofree uint8_t *image = g_malloc0(DS1742_SIZE);
    g_autofree gchar *saved = NULL;
    const uint8_t *expected;
    gsize length;
    Files f;
    QTestState *qts;

    files_make(&f, "nvram0.raw");
    image[0] = MARKER;
    memcpy(image + DS1742_CLOCK, before, sizeof(before));
    file_put(f.image, image, DS1742_SIZE);
    record_put(&f, usable ? "DS1742R1" : "NOTMAGIC", 16,
               RTC_BASE_SECONDS - ELAPSED, 0);

    qts = qtest_initf("-M none -cpu R10000 -S "
                      "-rtc base=" RTC_BASE ",clock=vm "
                      "-drive if=none,id=nvram,file=%s,format=raw "
                      "-drive if=none,id=nvram-clock,file=%s,format=raw "
                      "-device ds1742,id=clock,drive=nvram,"
                      "clock-drive=nvram-clock", f.image, f.record);
    qtest_sysbus_mmio_map(qts, DS1742_PATH, 0, DS1742_BASE);
    expected = usable ? after : before;
    for (int i = 3; i < 8; i++) {
        /* Bits 7 and 6 of the weekday are the battery and test flags. */
        uint8_t mask = i == 4 ? 0x07 : 0xff;

        g_assert_cmphex(ds1742_read(qts, DS1742_CLOCK + i) & mask, ==,
                        expected[i]);
    }
    g_assert_cmphex(ds1742_read(qts, DS1742_CLOCK + 2), ==, 0);
    g_assert_cmphex(ds1742_read(qts, 0), ==, MARKER);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(f.image, &saved, &length, NULL));
    g_assert_cmpuint(length, ==, DS1742_SIZE);
    g_assert_cmphex((uint8_t)saved[0], ==, MARKER);
    g_assert_cmphex((uint8_t)saved[DS1742_CLOCK + 3], ==, expected[3]);
    g_assert_cmpmem(saved + DS1742_CLOCK + 5, 3, expected + 5, 3);
    record_check(&f, "DS1742R1", 16, 0);
    files_free(&f);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_data_func("/sgi-store-format/m48t35", GINT_TO_POINTER(1),
                        test_m48t35);
    qtest_add_data_func("/sgi-store-format/m48t35-wrong-magic",
                        GINT_TO_POINTER(0), test_m48t35);
    qtest_add_data_func("/sgi-store-format/ds1685", GINT_TO_POINTER(1),
                        test_ds1685);
    qtest_add_data_func("/sgi-store-format/ds1685-wrong-magic",
                        GINT_TO_POINTER(0), test_ds1685);
    qtest_add_data_func("/sgi-store-format/ds1742", GINT_TO_POINTER(1),
                        test_ds1742);
    qtest_add_data_func("/sgi-store-format/ds1742-wrong-magic",
                        GINT_TO_POINTER(0), test_ds1742);
    return g_test_run();
}
