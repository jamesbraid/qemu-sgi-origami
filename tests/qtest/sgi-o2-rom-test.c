/* SPDX-License-Identifier: BSD-3-Clause */
/* Synthetic MACE system ROM decode and diagnostic image selections. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "sgi-migrate.h"
#include "qobject/qdict.h"

#define ROM_BASE UINT64_C(0x1fc00000)
#define ROM_WINDOW (4 * 1024 * 1024)
#define PAYLOAD_SIZE 431872
#define ROM_PATH "/machine/peripheral/rom"

static char *test_dir;

static uint8_t pattern(size_t offset)
{
    return (uint8_t)(offset * 37 + (offset >> 8) * 11 + 0x2b);
}

static uint8_t *make_image(size_t length, const char *name, char **path)
{
    uint8_t *bytes = g_malloc(length);

    for (size_t i = 0; i < length; i++) {
        bytes[i] = pattern(i);
    }
    bytes[0] = 0x12;
    bytes[length - 1] = 0xa7;
    *path = g_build_filename(test_dir, name, NULL);
    g_assert_true(g_file_set_contents(*path, (const char *)bytes,
                                      length, NULL));
    return bytes;
}

static QTestState *start(uint32_t capacity, const char *image,
                         uint32_t offset, uint8_t pad, const char *extra)
{
    QTestState *qts = qtest_initf("-M none -cpu R10000 -S "
                                   "-device sgi-o2-rom,id=rom,flash-size=%u,"
                                   "image=%s,image-offset=%u,pad-byte=%u %s",
                                   capacity, image, offset, pad, extra);

    qtest_sysbus_mmio_map(qts, ROM_PATH, 0, ROM_BASE);
    return qts;
}

static uint64_t big_endian_word(const uint8_t *bytes, size_t offset,
                                unsigned width)
{
    uint64_t value = 0;

    for (unsigned i = 0; i < width; i++) {
        value = (value << 8) | bytes[offset + i];
    }
    return value;
}

static void check_full_image(uint32_t capacity)
{
    g_autofree char *name = g_strdup_printf("full-%u.bin", capacity);
    g_autofree char *path = NULL;
    g_autofree uint8_t *bytes = make_image(capacity, name, &path);
    QTestState *qts = start(capacity, path, 0, 0x6d, "");
    const uint32_t points[] = { 0, 1, capacity / 2 - 1,
                                capacity / 2, capacity - 2, capacity - 1 };

    for (unsigned alias = 0; alias < ROM_WINDOW / capacity; alias++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(points); i++) {
            uint32_t offset = points[i];

            g_assert_cmphex(qtest_readb(qts, ROM_BASE + alias * capacity +
                                        offset), ==, bytes[offset]);
        }
        for (uint32_t offset = 0; offset <= capacity - 8;
             offset += capacity / 2 - 8) {
            uint64_t address = ROM_BASE + alias * capacity + offset;

            g_assert_cmphex(qtest_readb(qts, address), ==,
                            big_endian_word(bytes, offset, 1));
            g_assert_cmphex(qtest_readw(qts, address), ==,
                            big_endian_word(bytes, offset, 2));
            g_assert_cmphex(qtest_readl(qts, address), ==,
                            big_endian_word(bytes, offset, 4));
            g_assert_cmphex(qtest_readq(qts, address), ==,
                            big_endian_word(bytes, offset, 8));
        }
        g_assert_cmphex(qtest_readq(qts, ROM_BASE +
                                    (alias + 1) * capacity - 8), ==,
                        big_endian_word(bytes, capacity - 8, 8));
    }
    g_assert_cmphex(qtest_readb(qts, ROM_BASE - 1), !=, bytes[capacity - 1]);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + ROM_WINDOW), !=, bytes[0]);
    qtest_quit(qts);
}

static void test_aliases(void)
{
    check_full_image(512 * 1024);
    check_full_image(1024 * 1024);
    check_full_image(2048 * 1024);
}

static void check_placement(uint32_t offset, const char *name)
{
    const uint32_t capacity = 512 * 1024;
    g_autofree char *path = NULL;
    g_autofree uint8_t *bytes = make_image(PAYLOAD_SIZE, name, &path);
    QTestState *qts = start(capacity, path, offset, 0xff, "");

    for (unsigned alias = 0; alias < 8; alias++) {
        uint64_t base = ROM_BASE + alias * capacity;

        g_assert_cmphex(qtest_readb(qts, base + offset), ==, bytes[0]);
        g_assert_cmphex(qtest_readb(qts, base + offset + PAYLOAD_SIZE - 1),
                        ==, bytes[PAYLOAD_SIZE - 1]);
        if (offset) {
            g_assert_cmphex(qtest_readb(qts, base + offset - 1), ==, 0xff);
        } else {
            g_assert_cmphex(qtest_readb(qts, base + PAYLOAD_SIZE), ==,
                            0xff);
        }
    }
    qtest_quit(qts);
}

static void test_placements(void)
{
    check_placement(0, "leading.bin");
    check_placement(0x16900, "trailing.bin");
}

static bool write_enabled(QTestState *qts)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{"
        "'path':'/machine/peripheral/rom',"
        "'property':'write-enable'}}");
    bool enabled = qdict_get_bool(response, "return");

    qobject_unref(response);
    return enabled;
}

static void test_write_protection(void)
{
    g_autofree char *path = NULL;
    g_autofree char *logfile = g_build_filename(test_dir, "writes.log", NULL);
    g_autofree char *log = NULL;
    g_autofree uint8_t *bytes = make_image(512 * 1024, "writes.bin", &path);
    g_autofree char *extra = g_strdup_printf("-d unimp -D %s", logfile);
    QTestState *qts = start(512 * 1024, path, 0, 0xff, extra);

    g_assert_false(write_enabled(qts));
    qtest_writeb(qts, ROM_BASE, 0xa0);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE), ==, bytes[0]);
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/rom',"
        "'property':'write-enable','value':true}}");
    g_assert_true(write_enabled(qts));
    qtest_writeb(qts, ROM_BASE, 0xa0);
    qtest_writel(qts, ROM_BASE, 0x12345678);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE), ==, bytes[0]);
    qtest_system_reset(qts);
    g_assert_false(write_enabled(qts));
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(logfile, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "unsupported enabled flash write"));
    g_assert_nonnull(strstr(log, "unsupported multi-byte flash write"));
}

static void expect_startup_error(const char *device, const char *message)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "none", "-accel", "qtest",
        "-cpu", "R10000", "-S", "-device", device,
        "-display", "none", NULL
    };
    g_autofree char *error_output = NULL;
    int status;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                               NULL, &error_output, &status, NULL));
    g_assert_false(g_spawn_check_exit_status(status, NULL));
    if (!strstr(error_output, message)) {
        g_test_message("QEMU rejection: %s", error_output);
    }
    g_assert_nonnull(strstr(error_output, message));
}

static void test_invalid_configuration(void)
{
    g_autofree char *path = NULL;
    g_autofree char *too_large_path = NULL;
    g_free(make_image(512 * 1024, "config.bin", &path));
    g_free(make_image(512 * 1024 + 1, "oversize.bin", &too_large_path));
    g_autofree char *small = g_strdup_printf(
        "sgi-o2-rom,id=rom,flash-size=262144,image=%s,image-offset=0,pad-byte=255",
        path);
    g_autofree char *missing_offset = g_strdup_printf(
        "sgi-o2-rom,id=rom,flash-size=524288,image=%s,pad-byte=255", path);
    g_autofree char *missing_pad = g_strdup_printf(
        "sgi-o2-rom,id=rom,flash-size=524288,image=%s,image-offset=0", path);
    g_autofree char *bad_offset = g_strdup_printf(
        "sgi-o2-rom,id=rom,flash-size=524288,image=%s,image-offset=524289,"
        "pad-byte=255", path);
    g_autofree char *oversize = g_strdup_printf(
        "sgi-o2-rom,id=rom,flash-size=524288,image=%s,image-offset=0,pad-byte=255",
        too_large_path);

    expect_startup_error("sgi-o2-rom,id=rom", "flash-size");
    expect_startup_error(small, "flash-size");
    expect_startup_error("sgi-o2-rom,id=rom,flash-size=524288,image-offset=0,"
                         "pad-byte=255", "requires image");
    expect_startup_error(missing_offset, "image-offset");
    expect_startup_error(missing_pad, "pad-byte");
    expect_startup_error(bad_offset, "image-offset exceeds flash-size");
    expect_startup_error(oversize, "does not fit");
}

static void test_migration(void)
{
    g_autofree char *source_path = NULL;
    g_autofree char *destination_path = NULL;
    g_autofree char *socket =
        g_build_filename(test_dir, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree uint8_t *source =
        make_image(512 * 1024, "source.bin", &source_path);
    g_autofree uint8_t *destination =
        make_image(512 * 1024, "destination.bin", &destination_path);
    QTestState *from, *to;
    const uint32_t points[] = { 0, 0x12345, 512 * 1024 - 1 };

    destination[0] ^= 0xff;
    destination[0x12345] ^= 0xff;
    destination[512 * 1024 - 1] ^= 0xff;
    g_assert_true(g_file_set_contents(destination_path,
                                      (const char *)destination,
                                      512 * 1024, NULL));
    from = start(512 * 1024, source_path, 0, 0xff, "");
    to = start(512 * 1024, destination_path, 0, 0xff,
               "-incoming defer");
    g_assert_cmphex(qtest_readb(to, ROM_BASE), ==, destination[0]);
    g_assert_false(write_enabled(to));
    qtest_qmp_assert_success(from,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/peripheral/rom',"
        "'property':'write-enable','value':true}}");
    g_assert_true(write_enabled(from));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_true(write_enabled(to));
    for (unsigned alias = 0; alias < 8; alias++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(points); i++) {
            uint32_t offset = points[i];

            g_assert_cmphex(qtest_readb(to, ROM_BASE +
                                        alias * 512 * 1024 + offset), ==,
                            source[offset]);
        }
    }
    qtest_system_reset(to);
    g_assert_false(write_enabled(to));
    g_assert_cmphex(qtest_readb(to, ROM_BASE), ==, source[0]);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
}

int main(int argc, char **argv)
{
    int result;

    g_test_init(&argc, &argv, NULL);
    test_dir = g_dir_make_tmp("sgi-o2-rom-XXXXXX", NULL);
    g_assert_nonnull(test_dir);
    g_test_add_func("/sgi/o2-rom/aliases", test_aliases);
    g_test_add_func("/sgi/o2-rom/placements", test_placements);
    g_test_add_func("/sgi/o2-rom/write-protection", test_write_protection);
    g_test_add_func("/sgi/o2-rom/invalid-configuration",
                    test_invalid_configuration);
    g_test_add_func("/sgi/o2-rom/migration", test_migration);
    result = g_test_run();
    g_free(test_dir);
    return result;
}
