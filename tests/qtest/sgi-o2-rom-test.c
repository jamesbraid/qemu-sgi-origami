/* SPDX-License-Identifier: BSD-3-Clause */
/* Synthetic MACE system ROM decode and diagnostic image selections. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-migrate.h"
#include "sgi-wait.h"
#include "qobject/qdict.h"

#define ROM_BASE UINT64_C(0x1fc00000)
#define ROM_WINDOW (4 * 1024 * 1024)
#define MACE_MISC UINT64_C(0x1f310008)
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

static QTestState *start_o2(const char *image, const char *machine_extra,
                            const char *qemu_extra)
{
    return qtest_initf(
        "-M o2-test,password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,"
        "rom-pad-byte=255%s -cpu R10000 -m 64M -nic none "
        MEMTX_TEST_DEVICE " %s",
        image, machine_extra, qemu_extra);
}

static bool o2_write_enabled(QTestState *qts)
{
    return qtest_qom_get_bool(qts, "/machine/rom", "write-enable");
}

static QTestState *start_at29_extra(const char *image,
                                    const char *protection,
                                    const char *lower_lock,
                                    const char *upper_lock,
                                    const char *qemu_extra)
{
    g_autofree char *options = g_strdup_printf(
        ",rom-at29c040a=on,rom-at29c040a-protection=%s,"
        "rom-at29c040a-lower-lock=%s,rom-at29c040a-upper-lock=%s",
        protection, lower_lock, upper_lock);

    return start_o2(image, options, qemu_extra);
}

static QTestState *start_at29(const char *image, const char *protection,
                              const char *lower_lock,
                              const char *upper_lock)
{
    return start_at29_extra(image, protection, lower_lock, upper_lock, "");
}

static void at29_prefix(QTestState *qts)
{
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x5555, 0xaa), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 1000);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x2aaa, 0x55), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 1000);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x5555, 0xa0), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 1000);
}

static void at29_load_sector(QTestState *qts, uint32_t sector,
                             const uint8_t data[256])
{
    for (unsigned i = 0; i < 256; i++) {
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + i, data[i]),
                        ==, MEMTX_OK);
        if (i != 255) {
            qtest_clock_step(qts, 1000);
        }
    }
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

static void test_machine_flash_gate(void)
{
    g_autofree char *path = NULL;
    g_autofree uint8_t *bytes = make_image(512 * 1024,
                                            "machine-gate.bin", &path);
    QTestState *qts = start_o2(path, "", "");
    g_autoptr(QDict) direct_gate = qtest_qmp(qts,
        "{'execute':'qom-set','arguments':{"
        "'path':'/machine/rom','property':'write-enable','value':true}}");

    g_assert_nonnull(qdict_get_qdict(direct_gate, "error"));
    g_assert_cmphex(qtest_readq(qts, MACE_MISC) & 1, ==, 0);
    g_assert_false(o2_write_enabled(qts));
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 5, 0x55), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + 5), ==, bytes[5]);
    qtest_writeq(qts, MACE_MISC, 1);
    g_assert_cmphex(qtest_readq(qts, MACE_MISC) & 1, ==, 1);
    g_assert_true(o2_write_enabled(qts));
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 5, 0xa0), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + 5), ==, bytes[5]);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + 512 * 1024 + 5), ==,
                    bytes[5]);
    qtest_writeq(qts, MACE_MISC, 1 | 4 | 8);
    g_assert_cmphex(qtest_readq(qts, MACE_MISC) & 0xf, ==, 1 | 4);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 512 * 1024 + 5, 0x80),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_writew(qts, ROM_BASE + 5, 0x1234), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writel(qts, ROM_BASE + 4, 0x12345678),
                    !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(qts, ROM_BASE + 8, UINT64_MAX),
                    !=, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + 5), ==, bytes[5]);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, MACE_MISC) & 1, ==, 0);
    g_assert_false(o2_write_enabled(qts));
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 5, 0x55), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + 5), ==, bytes[5]);
    qtest_quit(qts);
}

static void test_machine_flash_gate_word_pio(void)
{
    g_autofree char *path = NULL;
    g_autofree char *eprom_path =
        g_build_filename(test_dir, "machine-gate-eprom.bin", NULL);
    g_autofree uint8_t *bytes = make_image(512 * 1024,
                                            "machine-gate-word.bin", &path);
    uint8_t eprom[128] = { 0 };
    g_autofree char *machine_extra = NULL;
    QTestState *qts;

    g_assert_true(g_file_set_contents(eprom_path, (const char *)eprom,
                                      sizeof(eprom), NULL));
    machine_extra = g_strdup_printf(
        ",identity-diagnostic=on,identity-test-serial=1032547698ba,"
        "identity-eprom-file=%s,identity-status=0123456789abcd,"
        "mace-misc-word-pio=on", eprom_path);
    qts = start_o2(path, machine_extra, "");
    g_assert_cmphex(qtest_readl(qts, MACE_MISC + 4) & 1, ==, 0);
    qtest_writel(qts, MACE_MISC + 4, 1 | 8);
    g_assert_cmphex(qtest_readl(qts, MACE_MISC + 4) & 0xf, ==, 1);
    g_assert_true(o2_write_enabled(qts));
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 5, 0xa0), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + 5), ==, bytes[5]);
    qtest_writel(qts, MACE_MISC + 4, 1 | 4);
    qtest_clock_step(qts, 500000);
    qtest_writel(qts, MACE_MISC + 4, 1);
    qtest_clock_step(qts, 520000);
    qtest_writel(qts, MACE_MISC + 4, 1 | 4);
    qtest_clock_step(qts, 65000);
    g_assert_false(qtest_readl(qts, MACE_MISC + 4) & 8);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, MACE_MISC + 4) & 1, ==, 0);
    g_assert_false(o2_write_enabled(qts));
    qtest_quit(qts);
}

static void test_at29_sector_program(void)
{
    const uint32_t sector = 0x10000;
    g_autofree char *path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-sector.bin", &path);
    uint8_t data[256], got[256];
    g_autofree char *seed_after = NULL;
    gsize seed_length;
    QTestState *qts = start_at29(path, "disabled", "unlocked",
                                 "unlocked");
    uint8_t poll0, poll1;

    for (unsigned i = 0; i < sizeof(data); i++) {
        data[i] = i * 13 + 0x25;
    }
    g_assert_cmphex(qtest_readq(qts, MACE_MISC) & 1, ==, 0);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector, 0x55), ==,
                    MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + sector), ==, seed[sector]);
    qtest_writeq(qts, MACE_MISC, 1);
    for (unsigned i = 0; i < sizeof(data); i++) {
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + i, data[i]),
                        ==, MEMTX_OK);
        if (i + 1 < sizeof(data)) {
            qtest_clock_step(qts, 1000);
        }
    }
    qtest_writeq(qts, MACE_MISC, 0);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 256, 0x77),
                    ==, MEMTX_OK);
    poll0 = qtest_readb(qts, ROM_BASE + sector + 255);
    poll1 = qtest_readb(qts, ROM_BASE + sector + 255);
    g_assert_cmphex((poll0 ^ data[255]) & 0x80, ==, 0x80);
    g_assert_cmphex((poll0 ^ poll1) & 0x40, ==, 0x40);
    qtest_clock_step(qts, 149999);
    poll0 = qtest_readb(qts, ROM_BASE + sector + 256);
    poll1 = qtest_readb(qts, ROM_BASE + sector + 256);
    g_assert_cmphex((poll0 ^ poll1) & 0x40, ==, 0x40);
    qtest_clock_step(qts, 10 * 1000 * 1000 - 150000);
    g_assert_cmphex((qtest_readb(qts, ROM_BASE + sector + 255) ^
                     data[255]) & 0x80, ==, 0x80);
    qtest_clock_step(qts, 1);
    for (unsigned alias = 0; alias < 8; alias++) {
        qtest_memread(qts, ROM_BASE + alias * 512 * 1024 + sector,
                      got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
        g_assert_cmphex(qtest_readb(qts, ROM_BASE +
                                    alias * 512 * 1024 + sector - 1), ==,
                        seed[sector - 1]);
        g_assert_cmphex(qtest_readb(qts, ROM_BASE +
                                    alias * 512 * 1024 + sector + 256), ==,
                        seed[sector + 256]);
    }
    g_assert_true(g_file_get_contents(path, &seed_after, &seed_length, NULL));
    g_assert_cmpuint(seed_length, ==, 512 * 1024);
    g_assert_cmpmem(seed_after + sector, 256, seed + sector, 256);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, MACE_MISC) & 1, ==, 0);
    qtest_memread(qts, ROM_BASE + sector, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
    qtest_quit(qts);
}

static void test_at29_full_sector_starts_busy(void)
{
    const uint32_t sector = 0x18000;
    g_autofree char *path = NULL;
    g_autofree char *log_path =
        g_build_filename(test_dir, "at29-full-sector-busy.log", NULL);
    g_autofree char *extra = g_strdup_printf("-d unimp -D %s", log_path);
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-full-sector-busy.bin", &path);
    g_autofree char *log = NULL;
    uint8_t data[256], got[256], poll0, poll1;
    QTestState *qts = start_at29_extra(path, "disabled", "unlocked",
                                       "unlocked", extra);

    for (unsigned i = 0; i < sizeof(data); i++) {
        data[i] = seed[sector + i] ^ 0x5a;
    }
    data[255] = seed[sector + 255];
    qtest_writeq(qts, MACE_MISC, 1);
    at29_load_sector(qts, sector, data);
    poll0 = qtest_readb(qts, ROM_BASE + sector + 255);
    poll1 = qtest_readb(qts, ROM_BASE + sector + 255);
    g_assert_cmphex((poll0 ^ data[255]) & 0x80, ==, 0x80);
    g_assert_cmphex((poll0 ^ poll1) & 0x40, ==, 0x40);
    qtest_clock_step(qts, 50 * 1000);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 256, 0x3c), !=,
                    MEMTX_OK);
    qtest_clock_step(qts, 10 * 1000 * 1000 - 50 * 1000);
    qtest_memread(qts, ROM_BASE + sector, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + sector + 256), ==,
                    seed[sector + 256]);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 256, 0x3c), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 150000);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + sector + 256), ==,
                    seed[sector + 256]);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "AT29C040A write during program cycle"));
    g_assert_nonnull(strstr(log, "partial AT29C040A sector load"));
    g_assert_null(strstr(log, "cross-sector AT29C040A load"));
}

static void test_at29_protection(void)
{
    const uint32_t sector = 0x20000;
    g_autofree char *path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-protected.bin", &path);
    uint8_t data[256], got[256], poll0, poll1;
    QTestState *qts = start_at29(path, "enabled", "unlocked",
                                 "unlocked");

    for (unsigned i = 0; i < sizeof(data); i++) {
        data[i] = 0xc3 ^ (i * 17);
    }
    qtest_writeq(qts, MACE_MISC, 1);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 5, 0x34), ==,
                    MEMTX_OK);
    poll0 = qtest_readb(qts, ROM_BASE + sector + 5);
    poll1 = qtest_readb(qts, ROM_BASE + sector + 5);
    g_assert_cmphex((poll0 ^ 0x34) & 0x80, ==, 0x80);
    g_assert_cmphex((poll0 ^ poll1) & 0x40, ==, 0x40);
    qtest_clock_step(qts, 10 * 1000 * 1000);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + sector + 5), ==,
                    seed[sector + 5]);

    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x5555, 0xaa), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 1000);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x2aab, 0x55), !=,
                    MEMTX_OK);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 6, 0x56), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 10 * 1000 * 1000);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + sector + 6), ==,
                    seed[sector + 6]);

    at29_prefix(qts);
    at29_load_sector(qts, sector, data);
    qtest_clock_step(qts, 10 * 1000 * 1000);
    qtest_memread(qts, ROM_BASE + sector, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 256 + 7, 0x78),
                    ==, MEMTX_OK);
    qtest_clock_step(qts, 10 * 1000 * 1000);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + sector + 256 + 7), ==,
                    seed[sector + 256 + 7]);
    qtest_system_reset(qts);
    qtest_writeq(qts, MACE_MISC, 1);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 256 + 8, 0x9a),
                    ==, MEMTX_OK);
    qtest_clock_step(qts, 10 * 1000 * 1000);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + sector + 256 + 8), ==,
                    seed[sector + 256 + 8]);
    qtest_quit(qts);
}

static void test_at29_prefix_enables_protection(void)
{
    const uint32_t sector = 0x24000;
    g_autofree char *path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-enable-protect.bin", &path);
    uint8_t data[256];
    QTestState *qts = start_at29(path, "disabled", "unlocked",
                                 "unlocked");

    memset(data, 0x5a, sizeof(data));
    qtest_writeq(qts, MACE_MISC, 1);
    at29_prefix(qts);
    at29_load_sector(qts, sector, data);
    qtest_clock_step(qts, 10 * 1000 * 1000);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + sector + 100), ==, 0x5a);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 256, 0x33), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 10 * 1000 * 1000);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + sector + 256), ==,
                    seed[sector + 256]);
    qtest_quit(qts);
}

static void test_at29_locked_boot_blocks(void)
{
    static const uint32_t locked_sectors[] = { 0, 0x7ff00 };
    g_autofree char *path = NULL;
    g_autofree char *log_path =
        g_build_filename(test_dir, "at29-locked.log", NULL);
    g_autofree char *extra = g_strdup_printf("-d unimp -D %s", log_path);
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-locked.bin", &path);
    g_autofree char *log = NULL;
    uint8_t data[256], got[256];
    QTestState *qts = start_at29_extra(path, "disabled", "locked",
                                       "locked", extra);

    memset(data, 0x6b, sizeof(data));
    qtest_writeq(qts, MACE_MISC, 1);
    for (unsigned sector = 0; sector < G_N_ELEMENTS(locked_sectors);
         sector++) {
        uint32_t address = locked_sectors[sector];

        for (unsigned i = 0; i < 256; i++) {
            g_assert_cmpint(memtx_writeb(qts, ROM_BASE + address + i,
                                         data[i]), !=, MEMTX_OK);
        }
        qtest_clock_step(qts, 10 * 1000 * 1000);
        qtest_memread(qts, ROM_BASE + address, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), seed + address, sizeof(got));
    }
    at29_load_sector(qts, 0x4000, data);
    qtest_clock_step(qts, 10 * 1000 * 1000);
    qtest_memread(qts, ROM_BASE + 0x4000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "AT29C040A boot block is locked"));
}

static void test_at29_protected_locked_boot_blocks(void)
{
    static const uint32_t addresses[] = { 0x100, 0x7ff00 };
    g_autofree char *path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-protected-locked.bin", &path);
    QTestState *qts = start_at29(path, "enabled", "locked", "locked");
    uint8_t data[256], got[256];

    qtest_writeq(qts, MACE_MISC, 1);
    for (unsigned i = 0; i < G_N_ELEMENTS(addresses); i++) {
        uint32_t address = addresses[i];

        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + address, 0x42), !=,
                        MEMTX_OK);
        qtest_clock_step(qts, 10 * 1000 * 1000);
        g_assert_cmphex(qtest_readb(qts, ROM_BASE + address), ==,
                        seed[address]);
    }
    memset(data, 0x5c, sizeof(data));
    at29_prefix(qts);
    at29_load_sector(qts, 0x4000, data);
    qtest_clock_step(qts, 10 * 1000 * 1000);
    qtest_memread(qts, ROM_BASE + 0x4000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
    qtest_quit(qts);
}

static void test_at29_refused_sequences(void)
{
    const uint32_t partial = 0x30000;
    const uint32_t crossing = 0x32000;
    g_autofree char *path = NULL;
    g_autofree char *log_path =
        g_build_filename(test_dir, "at29-refused.log", NULL);
    g_autofree char *extra = g_strdup_printf("-d unimp -D %s", log_path);
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-refused.bin", &path);
    g_autofree char *log = NULL;
    uint8_t got[256];
    QTestState *qts = start_at29_extra(path, "disabled", "unlocked",
                                       "unlocked", extra);

    qtest_writeq(qts, MACE_MISC, 1);
    for (unsigned i = 0; i < 255; i++) {
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + partial + i,
                                     i ^ 0x5a), ==, MEMTX_OK);
        qtest_clock_step(qts, 1000);
    }
    qtest_clock_step(qts, 150000);
    qtest_memread(qts, ROM_BASE + partial, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), seed + partial, sizeof(got));
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + crossing + 255, 0xa1),
                    ==, MEMTX_OK);
    qtest_clock_step(qts, 1000);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + crossing + 256, 0xb2),
                    !=, MEMTX_OK);
    qtest_memread(qts, ROM_BASE + crossing, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), seed + crossing, sizeof(got));

    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x5555, 0xaa), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 1000);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x2aaa, 0x55), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 1000);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x5555, 0x90), !=,
                    MEMTX_OK);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x5555, 0xaa), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 1000);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x2aaa, 0x55), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 1000);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x5555, 0xf0), !=,
                    MEMTX_OK);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x5555, 0xaa), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 1000);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x2aaa, 0x55), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 1000);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x5555, 0x80), !=,
                    MEMTX_OK);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + 0x5555), ==, seed[0x5555]);
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "partial AT29C040A sector load"));
    g_assert_nonnull(strstr(log, "cross-sector AT29C040A load"));
    g_assert_nonnull(strstr(log, "unsupported AT29C040A command"));
}

static void test_at29_command_address_data(void)
{
    static const uint8_t values[] = { 0x80, 0x90, 0xf0 };
    const uint32_t sector = 0x5500;
    g_autofree char *path = NULL;
    g_free(make_image(512 * 1024, "at29-command-data.bin", &path));
    uint8_t data[256], got[256];
    QTestState *qts = start_at29(path, "disabled", "unlocked",
                                 "unlocked");

    qtest_writeq(qts, MACE_MISC, 1);
    for (unsigned case_index = 0; case_index < G_N_ELEMENTS(values);
         case_index++) {
        for (unsigned i = 0; i < sizeof(data); i++) {
            data[i] = i * 3 + case_index * 17 + 0x22;
        }
        data[0x55] = values[case_index];
        for (unsigned i = 0; i < sizeof(data); i++) {
            unsigned index = (0x55 + i) & 0xff;

            g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + index,
                                         data[index]), ==, MEMTX_OK);
            if (i + 1 < sizeof(data)) {
                qtest_clock_step(qts, 1000);
            }
        }
        qtest_clock_step(qts, 10 * 1000 * 1000);
        qtest_memread(qts, ROM_BASE + sector, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
    }
    qtest_quit(qts);
}

static void test_at29_protected_command_address_data(void)
{
    g_autofree char *path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-protected-data.bin", &path);
    QTestState *qts = start_at29(path, "enabled", "unlocked",
                                 "unlocked");
    uint8_t poll0, poll1;

    qtest_writeq(qts, MACE_MISC, 1);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x5555, 0x90), ==,
                    MEMTX_OK);
    poll0 = qtest_readb(qts, ROM_BASE + 0x5555);
    poll1 = qtest_readb(qts, ROM_BASE + 0x5555);
    g_assert_cmphex((poll0 ^ poll1) & 0x40, ==, 0x40);
    qtest_clock_step(qts, 10 * 1000 * 1000);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + 0x5555), ==, seed[0x5555]);
    qtest_quit(qts);
}

static void test_at29_load_deadline(void)
{
    const uint32_t sector = 0x36000;
    g_autofree char *path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-deadline.bin", &path);
    uint8_t got[256];
    QTestState *qts = start_at29(path, "disabled", "unlocked",
                                 "unlocked");

    qtest_writeq(qts, MACE_MISC, 1);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector, 0x71), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 149999);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 1, 0x72), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 150000);
    qtest_memread(qts, ROM_BASE + sector, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), seed + sector, sizeof(got));

    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 256, 0x73), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 150000);
    for (unsigned i = 1; i < 256; i++) {
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 256 + i,
                                     i ^ 0x8c), ==, MEMTX_OK);
        qtest_clock_step(qts, 1000);
    }
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 511, 0x42), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 150000);
    qtest_memread(qts, ROM_BASE + sector + 256, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), seed + sector + 256,
                    sizeof(got));
    qtest_quit(qts);
}

static void assert_at29_migration_blocked(QTestState *qts)
{
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'migrate','arguments':{'uri':'unix:/nonexistent'}}");
    QDict *error = qdict_get_qdict(reply, "error");

    g_assert_nonnull(error);
    g_assert_nonnull(strstr(qdict_get_str(error, "desc"), "AT29C040A"));
}

static void test_at29_migration(void)
{
    const uint32_t sector = 0x28000;
    g_autofree char *path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-migrate.bin", &path);
    g_autofree char *socket =
        g_build_filename(test_dir, "at29-migrate.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    uint8_t data[256], got[256];
    QTestState *from = start_at29(path, "disabled", "unlocked",
                                   "unlocked");
    QTestState *to = start_at29_extra(path, "disabled", "unlocked",
                                      "unlocked", "-incoming defer");

    for (unsigned i = 0; i < sizeof(data); i++) {
        data[i] = i * 19 + 0x35;
    }
    qtest_writeq(from, MACE_MISC, 1);
    at29_prefix(from);
    g_assert_cmpint(memtx_writeb(from, ROM_BASE + sector, data[0]), ==,
                    MEMTX_OK);
    assert_at29_migration_blocked(from);
    for (unsigned i = 1; i < sizeof(data); i++) {
        qtest_clock_step(from, 1000);
        g_assert_cmpint(memtx_writeb(from, ROM_BASE + sector + i, data[i]),
                        ==, MEMTX_OK);
    }
    qtest_clock_step(from, 150000);
    assert_at29_migration_blocked(from);
    g_assert_cmphex((qtest_readb(from, ROM_BASE + sector + 255) ^
                     data[255]) & 0x80, ==, 0x80);
    qtest_system_reset(from);
    g_assert_cmphex(qtest_readq(from, MACE_MISC) & 1, ==, 0);
    g_assert_false(o2_write_enabled(from));
    g_assert_cmphex((qtest_readb(from, ROM_BASE + sector + 255) ^
                     data[255]) & 0x80, ==, 0x80);
    qtest_clock_step(from, 10 * 1000 * 1000);
    qtest_memread(from, ROM_BASE + sector, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    qtest_memread(to, ROM_BASE + sector, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
    g_assert_cmphex(qtest_readq(to, MACE_MISC) & 1, ==, 0);
    qtest_writeq(to, MACE_MISC, 1);
    g_assert_cmpint(memtx_writeb(to, ROM_BASE + sector + 256, 0x22), ==,
                    MEMTX_OK);
    qtest_clock_step(to, 10 * 1000 * 1000);
    g_assert_cmphex(qtest_readb(to, ROM_BASE + sector + 256), ==,
                    seed[sector + 256]);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
}

static void test_at29_migration_rejects_active_target(void)
{
    g_autofree char *path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-active-target.bin", &path);
    g_autofree char *socket =
        g_build_filename(test_dir, "at29-active-target.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *log_path =
        g_build_filename(test_dir, "at29-active-target.log", NULL);
    g_autofree char *extra = g_strdup_printf(
        "-incoming defer -d guest_errors -D %s", log_path);
    g_autofree char *log = NULL;
    g_autofree char *seed_after = NULL;
    gsize seed_length;
    QTestState *from = start_at29(path, "disabled", "unlocked",
                                   "unlocked");
    QTestState *to = start_at29_extra(path, "disabled", "unlocked",
                                      "unlocked", extra);
    SgiWait wait;

    qtest_set_expected_status(to, EXIT_FAILURE);
    qtest_writeq(to, MACE_MISC, 1);
    g_assert_cmpint(memtx_writeb(to, ROM_BASE + 0x28000, 0x5a), ==,
                    MEMTX_OK);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_start(&wait, "active AT29 target refuses incoming migration");
    while (qtest_probe_child(to)) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "active AT29C040A target refuses "
                                 "incoming migration"));
    g_assert_true(g_file_get_contents(path, &seed_after, &seed_length, NULL));
    g_assert_cmpuint(seed_length, ==, 512 * 1024);
    g_assert_cmpmem(seed_after, seed_length, seed, 512 * 1024);
}

static void test_at29_migration_rejects_read_only_target(void)
{
    g_autofree char *path = NULL;
    g_free(make_image(512 * 1024, "at29-mismatch.bin", &path));
    g_autofree char *socket =
        g_build_filename(test_dir, "at29-mismatch.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start_at29(path, "disabled", "unlocked",
                                   "unlocked");
    QTestState *to = start_o2(path, "", "-incoming defer");
    SgiWait wait;

    qtest_set_expected_status(to, EXIT_FAILURE);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_start(&wait, "read-only ROM refuses AT29 migration");
    while (qtest_probe_child(to)) {
        sgi_wait_pause(&wait, 1000);
    }
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
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

static void expect_machine_startup_error(const char *machine,
                                         const char *message)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", machine, "-accel", "qtest",
        "-cpu", "R10000", "-m", "64M", "-S", "-nic", "none",
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
    g_autofree char *too_small_path = NULL;
    g_free(make_image(512 * 1024, "config.bin", &path));
    g_free(make_image(512 * 1024 + 1, "oversize.bin", &too_large_path));
    g_free(make_image(512 * 1024 - 1, "undersize.bin", &too_small_path));
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
    {
        g_autofree char *missing_state = g_strdup_printf(
            "sgi-o2-rom,id=rom,flash-size=524288,image=%s,image-offset=0,"
            "pad-byte=255,at29c040a-diagnostic=on", path);
        g_autofree char *bad_capacity = g_strdup_printf(
            "sgi-o2-rom,id=rom,flash-size=1048576,image=%s,image-offset=0,"
            "pad-byte=255,at29c040a-diagnostic=on,"
            "at29c040a-protection=disabled,"
            "at29c040a-lower-lock=unlocked,"
            "at29c040a-upper-lock=unlocked", path);
        g_autofree char *short_image = g_strdup_printf(
            "sgi-o2-rom,id=rom,flash-size=524288,image=%s,image-offset=0,"
            "pad-byte=255,at29c040a-diagnostic=on,"
            "at29c040a-protection=disabled,"
            "at29c040a-lower-lock=unlocked,"
            "at29c040a-upper-lock=unlocked", too_small_path);
        g_autofree char *nonzero_offset = g_strdup_printf(
            "sgi-o2-rom,id=rom,flash-size=524288,image=%s,image-offset=1,"
            "pad-byte=255,at29c040a-diagnostic=on,"
            "at29c040a-protection=disabled,"
            "at29c040a-lower-lock=unlocked,"
            "at29c040a-upper-lock=unlocked", too_small_path);
        g_autofree char *bad_state = g_strdup_printf(
            "sgi-o2-rom,id=rom,flash-size=524288,image=%s,image-offset=0,"
            "pad-byte=255,at29c040a-diagnostic=on,"
            "at29c040a-protection=unknown,"
            "at29c040a-lower-lock=unlocked,"
            "at29c040a-upper-lock=unlocked", path);
        g_autofree char *implicit_mode = g_strdup_printf(
            "sgi-o2-rom,id=rom,flash-size=524288,image=%s,image-offset=0,"
            "pad-byte=255,at29c040a-protection=disabled", path);
        g_autofree char *machine_missing = g_strdup_printf(
            "o2-test,password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,"
            "rom-pad-byte=255,rom-at29c040a=on", path);
        g_autofree char *machine_implicit = g_strdup_printf(
            "o2-test,password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,"
            "rom-pad-byte=255,rom-at29c040a-protection=disabled", path);

        expect_startup_error(missing_state, "requires protection");
        expect_startup_error(bad_capacity, "needs 512 KiB");
        expect_startup_error(short_image, "full 512 KiB image");
        expect_startup_error(nonzero_offset, "image offset zero");
        expect_startup_error(bad_state,
                             "protection must be enabled or disabled");
        expect_startup_error(implicit_mode,
                             "states require the diagnostic profile");
        expect_machine_startup_error(machine_missing,
            "requires a full 512 KiB ROM image at offset zero");
        expect_machine_startup_error(machine_implicit,
            "states require rom-at29c040a=on");
    }
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

static void test_machine_flash_gate_migration(void)
{
    g_autofree char *path = NULL;
    g_autofree uint8_t *bytes = make_image(512 * 1024,
                                            "machine-migrate.bin", &path);
    g_autofree char *socket =
        g_build_filename(test_dir, "machine-migrate.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start_o2(path, "", "");
    QTestState *to = start_o2(path, "", "-incoming defer");

    g_assert_false(o2_write_enabled(from));
    g_assert_false(o2_write_enabled(to));
    qtest_writeq(from, MACE_MISC, 1 | 4);
    g_assert_cmphex(qtest_readq(from, MACE_MISC) & 1, ==, 1);
    g_assert_true(o2_write_enabled(from));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readq(to, MACE_MISC) & 1, ==, 1);
    g_assert_true(o2_write_enabled(to));
    g_assert_cmpint(memtx_writeb(to, ROM_BASE + 7, 0xa0), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readb(to, ROM_BASE + 7), ==, bytes[7]);
    qtest_system_reset(to);
    g_assert_cmphex(qtest_readq(to, MACE_MISC) & 1, ==, 0);
    g_assert_false(o2_write_enabled(to));
    g_assert_cmpint(memtx_writeb(to, ROM_BASE + 7, 0x55), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readb(to, ROM_BASE + 7), ==, bytes[7]);
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
    g_test_add_func("/sgi/o2-rom/machine-flash-gate",
                    test_machine_flash_gate);
    g_test_add_func("/sgi/o2-rom/machine-flash-gate-word-pio",
                    test_machine_flash_gate_word_pio);
    g_test_add_func("/sgi/o2-rom/at29-sector-program",
                    test_at29_sector_program);
    g_test_add_func("/sgi/o2-rom/at29-full-sector-starts-busy",
                    test_at29_full_sector_starts_busy);
    g_test_add_func("/sgi/o2-rom/at29-protection",
                    test_at29_protection);
    g_test_add_func("/sgi/o2-rom/at29-prefix-enables-protection",
                    test_at29_prefix_enables_protection);
    g_test_add_func("/sgi/o2-rom/at29-locked-boot-blocks",
                    test_at29_locked_boot_blocks);
    g_test_add_func("/sgi/o2-rom/at29-protected-locked-boot-blocks",
                    test_at29_protected_locked_boot_blocks);
    g_test_add_func("/sgi/o2-rom/at29-refused-sequences",
                    test_at29_refused_sequences);
    g_test_add_func("/sgi/o2-rom/at29-command-address-data",
                    test_at29_command_address_data);
    g_test_add_func("/sgi/o2-rom/at29-protected-command-address-data",
                    test_at29_protected_command_address_data);
    g_test_add_func("/sgi/o2-rom/at29-load-deadline",
                    test_at29_load_deadline);
    g_test_add_func("/sgi/o2-rom/at29-migration", test_at29_migration);
    g_test_add_func("/sgi/o2-rom/at29-migration-rejects-active-target",
                    test_at29_migration_rejects_active_target);
    g_test_add_func("/sgi/o2-rom/at29-migration-rejects-read-only",
                    test_at29_migration_rejects_read_only_target);
    g_test_add_func("/sgi/o2-rom/invalid-configuration",
                    test_invalid_configuration);
    g_test_add_func("/sgi/o2-rom/migration", test_migration);
    g_test_add_func("/sgi/o2-rom/machine-flash-gate-migration",
                    test_machine_flash_gate_migration);
    result = g_test_run();
    g_free(test_dir);
    return result;
}
