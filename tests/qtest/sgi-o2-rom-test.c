/* SPDX-License-Identifier: BSD-3-Clause */
/* Synthetic MACE system ROM decode and diagnostic image selections. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-migrate.h"
#include "sgi-wait.h"
#include "sgi-launch.h"
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
        "-M o2,register-test-profile=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,"
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
    qtest_clock_step(qts, 10000000);
    memset(data, 0xff, sizeof(data));
    data[0] = 0x3c;
    qtest_memread(qts, ROM_BASE + sector + 256, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
    qtest_quit(qts);
    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "AT29C040A write during program cycle"));
    g_assert_null(strstr(log, "partial AT29C040A sector load"));
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
    qtest_clock_step(qts, 10000000);
    qtest_memread(qts, ROM_BASE + partial, got, sizeof(got));
    for (unsigned i = 0; i < sizeof(got); i++) {
        g_assert_cmphex(got[i], ==, i < 255 ? (i ^ 0x5a) : 0xff);
    }
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + crossing + 255, 0xa1),
                    ==, MEMTX_OK);
    qtest_clock_step(qts, 1000);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + crossing + 256, 0xb2),
                    !=, MEMTX_OK);
    qtest_clock_step(qts, 10000000);
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
    g_assert_null(strstr(log, "partial AT29C040A sector load"));
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

static void at29_assert_polling(QTestState *qts, uint32_t address,
                                uint8_t data)
{
    for (unsigned alias = 0; alias < 8; alias++) {
        uint64_t mapped = ROM_BASE + alias * 512 * 1024 + address;
        uint8_t first = qtest_readb(qts, mapped);
        uint8_t second = qtest_readb(qts, mapped);

        g_assert_cmphex((first ^ data) & 0x80, ==, 0x80);
        g_assert_cmphex((first ^ second) & 0x40, ==, 0x40);
    }
}

static void at29_assert_sector(QTestState *qts, uint32_t sector,
                               const uint8_t expected[256],
                               const uint8_t *array)
{
    uint8_t got[256];

    for (unsigned alias = 0; alias < 8; alias++) {
        uint64_t mapped = ROM_BASE + alias * 512 * 1024 + sector;

        qtest_memread(qts, mapped, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), expected, 256);
        qtest_memread(qts, mapped - 256, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), array + sector - 256, 256);
        qtest_memread(qts, mapped + 256, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), array + sector + 256, 256);
    }
}

static void test_at29_partial_counts(void)
{
    static const unsigned counts[] = { 1, 2, 7, 63, 128, 240, 255, 256 };
    const uint32_t sector = 0x18000;
    g_autofree char *path = NULL;
    g_autofree uint8_t *array = make_image(512 * 1024,
                                            "at29-partial-counts.bin", &path);
    QTestState *qts = start_at29(path, "disabled", "unlocked", "unlocked");

    qtest_writeq(qts, MACE_MISC, 1);
    for (unsigned test = 0; test < G_N_ELEMENTS(counts); test++) {
        uint8_t expected[256];
        unsigned last = 0;
        int64_t loaded;

        memset(expected, 0xff, sizeof(expected));
        for (unsigned i = 0; i < counts[test]; i++) {
            last = (13 + i * 37) & 255;
            expected[last] = i == 0 ? 0xff : i * 19 + test;
            g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + last,
                                         expected[last]), ==, MEMTX_OK);
            if (i + 1 < counts[test]) {
                qtest_clock_step(qts, 1000);
            }
        }
        loaded = qtest_clock_step(qts, 1) - 1;
        if (counts[test] < 256) {
            /* Total writes cannot stand in for distinct loaded positions. */
            for (unsigned i = counts[test]; i < 256; i++) {
                last = 13;
                expected[last] = i ^ 0xa5;
                g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + last,
                                             expected[last]), ==, MEMTX_OK);
            }
            loaded = qtest_clock_step(qts, 1) - 1;
            qtest_clock_set(qts, loaded + 149999);
            at29_assert_sector(qts, sector, array + sector, array);
            expected[last] ^= 0x80;
            g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + last,
                                         expected[last]), ==, MEMTX_OK);
            loaded += 149999;
            qtest_clock_step(qts, 1);
            at29_assert_sector(qts, sector, array + sector, array);
            qtest_clock_set(qts, loaded + 149999);
            at29_assert_sector(qts, sector, array + sector, array);
            qtest_clock_step(qts, 1);
        }
        at29_assert_polling(qts, sector + last, expected[last]);
        qtest_clock_set(qts, loaded + 9999999);
        at29_assert_polling(qts, sector + last, expected[last]);
        qtest_clock_step(qts, 1);
        at29_assert_sector(qts, sector, expected, array);
        memcpy(array + sector, expected, sizeof(expected));
    }
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 256 + 73, 0x96),
                    ==, MEMTX_OK);
    qtest_clock_step(qts, 10000000);
    memset(array + sector + 256, 0xff, 256);
    array[sector + 256 + 73] = 0x96;
    at29_assert_sector(qts, sector + 256, array + sector + 256, array);
    g_autofree uint8_t *got = g_malloc(512 * 1024);
    qtest_memread(qts, ROM_BASE, got, 512 * 1024);
    g_assert_cmpmem(got, 512 * 1024, array, 512 * 1024);
    qtest_quit(qts);
}

static void test_at29_load_deadline(void)
{
    const uint32_t sector = 0x36000;
    g_autofree char *path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-deadline.bin", &path);
    uint8_t expected[256];
    QTestState *qts = start_at29(path, "disabled", "unlocked", "unlocked");

    memset(expected, 0xff, sizeof(expected));
    expected[0] = 0x71;
    expected[1] = 0x72;
    qtest_writeq(qts, MACE_MISC, 1);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector, expected[0]), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 149999);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 1, expected[1]), ==,
                    MEMTX_OK);
    qtest_clock_step(qts, 150000);
    at29_assert_polling(qts, sector + 1, expected[1]);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 2, 0x73), !=,
                    MEMTX_OK);
    qtest_clock_step(qts, 9850000);
    at29_assert_sector(qts, sector, expected, seed);
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

static void test_at29_partial_controls(void)
{
    const uint32_t sector = 0x22000;
    g_autofree char *path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-partial-controls.bin", &path);

    for (unsigned mode = 0; mode < 4; mode++) {
        uint8_t expected[256];
        QTestState *qts = start_at29(path, "disabled", "unlocked", "unlocked");
        int64_t loaded;

        memset(expected, 0xff, sizeof(expected));
        expected[73] = 0x35;
        expected[17] = 0x82;
        qtest_writeq(qts, MACE_MISC, 1);
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 73,
                                     expected[73]), ==, MEMTX_OK);
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 17,
                                     expected[17]), ==, MEMTX_OK);
        loaded = qtest_clock_step(qts, 1) - 1;
        assert_at29_migration_blocked(qts);
        if (mode & 1) {
            qtest_clock_set(qts, loaded + 150000);
            at29_assert_polling(qts, sector + 17, expected[17]);
        }
        if (mode < 2) {
            qtest_writeq(qts, MACE_MISC, 0);
        } else {
            qtest_system_reset(qts);
        }
        g_assert_false(o2_write_enabled(qts));
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 17, 0x44),
                        ==, MEMTX_OK);
        qtest_clock_step(qts, 100000);
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 256, 0x55),
                        ==, MEMTX_OK);
        if (!(mode & 1)) {
            qtest_clock_set(qts, loaded + 149999);
            at29_assert_sector(qts, sector, seed + sector, seed);
            qtest_clock_step(qts, 1);
        }
        assert_at29_migration_blocked(qts);
        at29_assert_polling(qts, sector + 17, expected[17]);
        qtest_clock_set(qts, loaded + 9999999);
        at29_assert_polling(qts, sector + 17, expected[17]);
        qtest_clock_step(qts, 1);
        at29_assert_sector(qts, sector, expected, seed);
        qtest_quit(qts);
    }
}

static void test_at29_partial_abort(void)
{
    const uint32_t aborted = 0x28000;
    const uint32_t sector = 0x2a000;
    g_autofree char *path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-partial-abort.bin", &path);
    uint8_t expected[256];
    QTestState *qts = start_at29(path, "disabled", "unlocked", "unlocked");

    qtest_writeq(qts, MACE_MISC, 1);
    at29_prefix(qts);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + aborted + 23, 0x86), ==,
                    MEMTX_OK);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + aborted + 256, 0x62), !=,
                    MEMTX_OK);
    qtest_clock_step(qts, 10000000);
    at29_assert_sector(qts, aborted, seed + aborted, seed);

    at29_prefix(qts);
    qtest_clock_step(qts, 150000);
    g_assert_cmpint(memtx_writel(qts, ROM_BASE + sector, 0x12345678), !=,
                    MEMTX_OK);
    memset(expected, 0xff, sizeof(expected));
    expected[19] = 0x37;
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 19, expected[19]),
                    ==, MEMTX_OK);
    qtest_clock_step(qts, 149999);
    at29_assert_sector(qts, sector, seed + sector, seed);
    qtest_clock_step(qts, 1);
    at29_assert_polling(qts, sector + 19, expected[19]);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 256, 0x62), !=,
                    MEMTX_OK);
    qtest_clock_step(qts, 9850000);
    at29_assert_sector(qts, sector, expected, seed);
    qtest_clock_step(qts, 10000000);
    at29_assert_sector(qts, sector, expected, seed);
    at29_assert_sector(qts, aborted, seed + aborted, seed);
    qtest_quit(qts);
}

static void test_at29_partial_protection(void)
{
    const uint32_t sector = 0x2c000;
    g_autofree char *path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-partial-protection.bin",
                                           &path);

    for (unsigned protected = 0; protected < 2; protected++) {
        uint8_t expected[256];
        QTestState *qts = start_at29(path, protected ? "enabled" : "disabled",
                                     "unlocked", "unlocked");

        memset(expected, 0xff, sizeof(expected));
        expected[31] = 0x35;
        expected[4] = 0x82;
        qtest_writeq(qts, MACE_MISC, 1);
        at29_prefix(qts);
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 73, 0xff),
                        ==, MEMTX_OK);
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 31, expected[31]),
                        ==, MEMTX_OK);
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 4, expected[4]),
                        ==, MEMTX_OK);
        at29_assert_sector(qts, sector, seed + sector, seed);
        qtest_clock_step(qts, 150000);
        at29_assert_polling(qts, sector + 4, expected[4]);
        qtest_clock_step(qts, 9850000);
        at29_assert_sector(qts, sector, expected, seed);
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 256 + 19, 0x62),
                        ==, MEMTX_OK);
        at29_assert_polling(qts, sector + 256 + 19, 0x62);
        qtest_clock_step(qts, 10000000);
        at29_assert_sector(qts, sector, expected, seed);
        qtest_quit(qts);
    }
}

static void test_at29_migration_case(bool partial)
{
    const uint32_t sector = 0x28000;
    g_autofree char *path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-migrate.bin", &path);
    g_autofree char *socket =
        g_build_filename(test_dir, "at29-migrate.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    uint8_t data[256], got[256];
    unsigned count = partial ? 7 : 256;
    QTestState *from = start_at29(path, "disabled", "unlocked",
                                   "unlocked");
    QTestState *to = start_at29_extra(path, "disabled", "unlocked",
                                      "unlocked", "-incoming defer");

    memset(data, 0xff, sizeof(data));
    for (unsigned i = 0; i < count; i++) {
        data[i] = i * 19 + 0x35;
    }
    qtest_writeq(from, MACE_MISC, 1);
    at29_prefix(from);
    assert_at29_migration_blocked(from);
    g_assert_cmpint(memtx_writeb(from, ROM_BASE + sector, data[0]), ==,
                    MEMTX_OK);
    assert_at29_migration_blocked(from);
    for (unsigned i = 1; i < count; i++) {
        qtest_clock_step(from, 1000);
        g_assert_cmpint(memtx_writeb(from, ROM_BASE + sector + i, data[i]),
                        ==, MEMTX_OK);
    }
    qtest_clock_step(from, 150000);
    assert_at29_migration_blocked(from);
    g_assert_cmphex((qtest_readb(from, ROM_BASE + sector + (count - 1)) ^
                     data[count - 1]) & 0x80, ==, 0x80);
    qtest_system_reset(from);
    g_assert_cmphex(qtest_readq(from, MACE_MISC) & 1, ==, 0);
    g_assert_false(o2_write_enabled(from));
    g_assert_cmphex((qtest_readb(from, ROM_BASE + sector + (count - 1)) ^
                     data[count - 1]) & 0x80, ==, 0x80);
    qtest_clock_step(from, 10 * 1000 * 1000);
    qtest_memread(from, ROM_BASE + sector, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    at29_assert_sector(to, sector, data, seed);
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

static void test_at29_migration(void)
{
    test_at29_migration_case(false);
    test_at29_migration_case(true);
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
            "o2,register-test-profile=on,"
            "password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,"
            "rom-pad-byte=255,rom-at29c040a=on", path);
        g_autofree char *machine_implicit = g_strdup_printf(
            "o2,register-test-profile=on,"
            "password-clear-input=0,rom-image=%s,rom-size=524288,rom-offset=0,"
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


static void make_at29_state(const char *path, const uint8_t *array,
                             uint8_t flags)
{
    uint8_t state[512] = { 'O', '2', 'A', 'T', '2', '9', 0, 1 };
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    gsize length = 32;

    state[8] = flags;
    g_checksum_update(sum, array, 512 * 1024);
    g_checksum_get_digest(sum, state + 32, &length);
    g_checksum_free(sum);
    g_assert_true(g_file_set_contents(path, (const char *)state,
                                      sizeof(state), NULL));
}

static void test_at29_backing_persistence_case(bool partial)
{
    g_autofree char *seed_path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
                                           "at29-backed-seed.bin", &seed_path);
    g_autofree char *backing = g_build_filename(test_dir, "flash.raw", NULL);
    g_autofree char *state = g_build_filename(test_dir, "flash.state", NULL);
    g_autofree char *extra = g_strdup_printf(
        "-drive if=none,id=o2flash,file=%s,format=raw "
        "-drive if=none,id=o2flash-state,file=%s,format=raw", backing, state);
    g_autofree uint8_t *expected = g_memdup2(seed, 512 * 1024);
    g_autofree uint8_t *reopened = g_malloc(512 * 1024);
    g_autofree char *stored = NULL;
    g_autofree char *metadata = NULL;
    gsize length;
    uint8_t data[256], got[256];
    const uint32_t sector = 0x10000;
    QTestState *qts;

    g_assert_true(g_file_set_contents(backing, (const char *)seed,
                                      512 * 1024, NULL));
    make_at29_state(state, seed, 4);
    qts = start_at29_extra(seed_path, "disabled", "unlocked", "locked", extra);
    for (unsigned i = 0; i < sizeof(data); i++) {
        data[i] = i * 19 + 0x53;
    }
    qtest_writeq(qts, MACE_MISC, 1);
    at29_prefix(qts);
    if (partial) {
        memset(data, 0xff, sizeof(data));
        data[31] = 0x35;
        data[4] = 0x86;
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 31, data[31]),
                        ==, MEMTX_OK);
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector + 4, data[4]),
                        ==, MEMTX_OK);
        qtest_clock_step(qts, 150000);
        g_assert_true(g_file_get_contents(backing, &stored, &length, NULL));
        g_assert_cmpmem(stored, length, seed, 512 * 1024);
        g_clear_pointer(&stored, g_free);
        qtest_clock_step(qts, 9850000);
    } else {
        at29_load_sector(qts, sector, data);
        qtest_clock_step(qts, 10 * 1000 * 1000);
    }
    g_assert_true(g_file_get_contents(backing, &stored, &length, NULL));
    g_assert_cmpuint(length, ==, 512 * 1024);
    memcpy(expected + sector, data, sizeof(data));
    g_assert_cmpmem(stored, 512 * 1024, expected, 512 * 1024);
    g_assert_cmphex((uint8_t)stored[sector - 1], ==, seed[sector - 1]);
    g_assert_cmphex((uint8_t)stored[sector + 256], ==, seed[sector + 256]);
    g_assert_true(g_file_get_contents(state, &metadata, &length, NULL));
    g_assert_cmpuint(length, ==, 512);
    g_assert_cmphex(metadata[8], ==, 5);
    qtest_system_reset(qts);
    qtest_memread(qts, ROM_BASE + sector, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
    qtest_quit(qts);
    /* Existing nonvolatile state wins over fresh synthetic startup fields. */
    qts = start_at29_extra(seed_path, "disabled", "unlocked",
                             "unlocked", extra);
    qtest_memread(qts, ROM_BASE, reopened, 512 * 1024);
    g_assert_cmpmem(reopened, 512 * 1024, expected, 512 * 1024);
    qtest_writeq(qts, MACE_MISC, 1);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + sector, 0x62), ==, MEMTX_OK);
    qtest_clock_step(qts, 10 * 1000 * 1000);
    g_assert_cmphex(qtest_readb(qts, ROM_BASE + sector), ==, data[0]);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 512 * 1024 - 1, 0x62),
                    !=, MEMTX_OK);
    qtest_quit(qts);
    g_clear_pointer(&stored, g_free);
    g_assert_true(g_file_get_contents(seed_path, &stored, &length, NULL));
    g_assert_cmpmem(stored, length, seed, 512 * 1024);
}


static void test_at29_backing_persistence(void)
{
    test_at29_backing_persistence_case(false);
    test_at29_backing_persistence_case(true);
}

static char *at29_drive(const char *id, const char *path, const char *extra)
{
    return g_strdup_printf("if=none,id=%s,file=%s,format=raw%s",
                            id, path, extra);
}

static void backing_startup_error(const char *seed, const char *array_drive,
                                   const char *state_drive, bool selected,
                                   const char *message)
{
    g_autofree char *machine = g_strdup_printf(
        "o2,register-test-profile=on,"
        "password-clear-input=0,rom-image=%s,rom-size=524288,"
        "rom-offset=0,rom-pad-byte=255%s", seed,
        selected ? ",rom-at29c040a=on,rom-at29c040a-protection=disabled,"
                   "rom-at29c040a-lower-lock=unlocked,"
                   "rom-at29c040a-upper-lock=unlocked" : "");
    const char *args[24] = { qtest_qemu_binary(NULL), "-M", machine,
        "-cpu", "R10000", "-m", "64M", "-accel", "qtest",
        "-nodefaults", "-display", "none", "-qmp", "stdio" };
    unsigned n = 14;

    if (array_drive) {
        args[n++] = "-drive";
        args[n++] = array_drive;
    }
    if (state_drive) {
        args[n++] = "-drive";
        args[n++] = state_drive;
    }
    args[n] = NULL;
    assert_qtest_launch_rejected(args, message);
}

static void test_at29_backing_validation(void)
{
    g_autofree char *seed_path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
        "at29-validation-seed.bin", &seed_path);
    g_autofree char *array = g_build_filename(test_dir, "validate.raw", NULL);
    g_autofree char *state = g_build_filename(test_dir, "validate.state", NULL);
    g_autofree char *array_drive = at29_drive("o2flash", array, "");
    g_autofree char *state_drive = at29_drive("o2flash-state", state, "");
    g_autofree char *ro_array = at29_drive("o2flash", array, ",readonly=on");
    g_autofree char *ro_state = at29_drive("o2flash-state", state,
                                           ",readonly=on");
    g_autofree char *unsafe = at29_drive("o2flash", array, ",cache=unsafe");
    g_autofree char *temporary = at29_drive("o2flash", array, ",snapshot=on");
    g_autofree char *extra = g_strdup_printf("-drive %s -drive %s",
                                             array_drive, state_drive);
    uint8_t zero[1024] = { 0 };
    g_autofree char *stored = NULL;
    gsize length;

    g_assert_true(g_file_set_contents(array, (const char *)seed,
                                      512 * 1024, NULL));
    make_at29_state(state, seed, 0);
    backing_startup_error(seed_path, array_drive, NULL, true, "both drive");
    backing_startup_error(seed_path, NULL, state_drive, true, "both drive");
    backing_startup_error(seed_path, array_drive, state_drive, false,
                           "backends require rom-at29c040a");
    backing_startup_error(seed_path, ro_array, state_drive, true,
                           "array backend must be writable");
    backing_startup_error(seed_path, unsafe, state_drive, true,
                           "durable persistent writes");
    backing_startup_error(seed_path, temporary, state_drive, true,
                           "durable persistent writes");
    backing_startup_error(seed_path, array_drive, ro_state, true,
                           "state backend must be writable");
    g_assert_true(g_file_set_contents(array, "", 0, NULL));
    backing_startup_error(seed_path, array_drive, state_drive, true,
                           "array backend requires exactly");
    g_assert_true(g_file_set_contents(array, (const char *)seed,
                                      512 * 1024, NULL));
    g_assert_true(g_file_set_contents(state, (const char *)zero,
                                      sizeof(zero), NULL));
    backing_startup_error(seed_path, array_drive, state_drive, true,
                           "state backend requires exactly");
    const unsigned invalid_offsets[] = { 0, 7, 8, 9, 32, 64 };
    for (unsigned i = 0; i < ARRAY_SIZE(invalid_offsets); i++) {
        make_at29_state(state, seed, 0);
        g_clear_pointer(&stored, g_free);
        g_assert_true(g_file_get_contents(state, &stored, &length, NULL));
        stored[invalid_offsets[i]] ^= 0x80;
        g_assert_true(g_file_set_contents(state, stored, length, NULL));
        backing_startup_error(seed_path, array_drive, state_drive, true,
                               "invalid backing state or array digest");
    }
    make_at29_state(state, seed, 0);
    QTestState *qts = start_at29_extra(seed_path, "disabled", "unlocked",
                                       "unlocked", extra);
    backing_startup_error(seed_path, array_drive, state_drive, true,
                           "Failed to get");
    g_autoptr(QDict) reply = qtest_qmp(qts,
        "{'execute':'migrate','arguments':{'uri':'file:/dev/null'}}");
    const char *description = qdict_get_str(qdict_get_qdict(reply, "error"),
                                             "desc");
    g_assert_nonnull(strstr(description, "persistent AT29C040A backing"));
    qtest_quit(qts);
    g_clear_pointer(&stored, g_free);
    g_assert_true(g_file_get_contents(array, &stored, &length, NULL));
    g_assert_cmpmem(stored, length, seed, 512 * 1024);
}

static void test_at29_backing_incomplete(void)
{
    g_autofree char *seed_path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
        "at29-incomplete-seed.bin", &seed_path);
    g_autofree char *array = g_build_filename(test_dir, "incomplete.raw", NULL);
    g_autofree char *state = g_build_filename(test_dir, "incomplete.state",
                                              NULL);
    g_autofree char *extra = g_strdup_printf(
        "-drive if=none,id=o2flash,file=%s,format=raw "
        "-drive if=none,id=o2flash-state,file=%s,format=raw", array, state);
    g_autofree char *stored = NULL;
    gsize length;
    uint8_t data[256], got[256];
    QTestState *qts;

    memset(data, 0x6d, sizeof(data));
    g_assert_true(g_file_set_contents(array, (const char *)seed,
                                      512 * 1024, NULL));
    make_at29_state(state, seed, 0);
    qts = start_at29_extra(seed_path, "disabled", "unlocked",
                             "unlocked", extra);
    qtest_writeq(qts, MACE_MISC, 1);
    for (unsigned i = 0; i < 255; i++) {
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x10000 + i, data[i]),
                        ==, MEMTX_OK);
    }
    qtest_clock_step(qts, 150000);
    g_assert_true(g_file_get_contents(array, &stored, &length, NULL));
    g_assert_cmpmem(stored, length, seed, 512 * 1024);
    data[255] = 0xff;
    qtest_system_reset(qts);
    g_assert_false(o2_write_enabled(qts));
    qtest_clock_step(qts, 9850000);
    qtest_memread(qts, ROM_BASE + 0x10000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
    qtest_writeq(qts, MACE_MISC, 1);
    at29_load_sector(qts, 0x20000, data);
    qtest_quit(qts);
    qts = start_at29_extra(seed_path, "disabled", "unlocked",
                             "unlocked", extra);
    qtest_memread(qts, ROM_BASE + 0x10000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), data, sizeof(data));
    qtest_memread(qts, ROM_BASE + 0x20000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), seed + 0x20000, sizeof(got));
    qtest_writeq(qts, MACE_MISC, 1);
    g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x30000 + 4, 0x25),
                    ==, MEMTX_OK);
    qtest_clock_step(qts, 150000);
    qtest_quit(qts);
    qts = start_at29_extra(seed_path, "disabled", "unlocked",
                             "unlocked", extra);
    qtest_memread(qts, ROM_BASE + 0x30000, got, sizeof(got));
    g_assert_cmpmem(got, sizeof(got), seed + 0x30000, sizeof(got));
    qtest_quit(qts);
}

static void test_at29_backing_io_errors(void)
{
    g_autofree char *seed_path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
        "at29-io-seed.bin", &seed_path);
    uint8_t data[256], got[256];

    memset(data, 0x6e, sizeof(data));
    for (unsigned fault = 0; fault < 8; fault++) {
        unsigned kind = fault % 4;
        g_autofree char *name = g_strdup_printf("io-%u", fault);
        g_autofree char *array = g_strdup_printf("%s/%s.raw", test_dir, name);
        g_autofree char *state = g_strdup_printf("%s/%s.state", test_dir, name);
        g_autofree char *config = g_strdup_printf("%s/%s.cfg", test_dir, name);
        g_autofree char *rule = g_strdup_printf(
            "[inject-error]\nevent = \"none\"\niotype = \"%s\"\n"
            "errno = \"5\"\nimmediately = \"on\"\nonce = \"on\"\n",
            kind & 1 ? "flush" : "write");
        g_autofree char *debug = g_strdup_printf("blkdebug:%s:%s", config,
                                                  kind < 2 ? array : state);
        g_autofree char *array_drive = at29_drive("o2flash",
            kind < 2 ? debug : array, "");
        g_autofree char *state_drive = at29_drive("o2flash-state",
            kind >= 2 ? debug : state, "");
        g_autofree char *log_path = g_strdup_printf("%s/%s.trace",
                                                     test_dir, name);
        g_autofree char *log = NULL;
        g_autofree char *extra = g_strdup_printf(
            "-drive %s -drive %s -trace enable=o2_at29_timeout -D %s",
            array_drive, state_drive, log_path);
        QTestState *qts;

        g_assert_true(g_file_set_contents(array, (const char *)seed,
                                          512 * 1024, NULL));
        make_at29_state(state, seed, 0);
        g_assert_true(g_file_set_contents(config, rule, -1, NULL));
        qts = start_at29_extra(seed_path, "disabled", "unlocked",
                             "unlocked", extra);
        qtest_qmp_assert_success(qts, "{'execute':'cont'}");
        qtest_writeq(qts, MACE_MISC, 1);
        if (fault < 4) {
            at29_load_sector(qts, 0x10000, data);
        } else {
            g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x10000 + 31,
                                         0x6e), ==, MEMTX_OK);
        }
        qtest_clock_step(qts, 10000000);
        qtest_qmp_eventwait(qts, "STOP");
        g_autoptr(QDict) reply = qtest_qmp(qts, "{'execute':'query-status'}");
        const char *status = qdict_get_str(qdict_get_qdict(reply, "return"),
                                            "status");
        g_assert_cmpstr(status, ==, "io-error");
        qtest_memread(qts, ROM_BASE + 0x10000, got, sizeof(got));
        g_assert_cmpmem(got, sizeof(got), seed + 0x10000, sizeof(got));
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x20000, 0x7a),
                        !=, MEMTX_OK);
        qtest_writeq(qts, MACE_MISC, 0);
        g_assert_cmpint(memtx_writeb(qts, ROM_BASE + 0x20000, 0x7a),
                        ==, MEMTX_OK);
        qtest_quit(qts);
        g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
        g_assert_nonnull(strstr(log, "backing-error"));
        g_assert_null(strstr(log, "program-complete"));
        if (kind == 1 || kind == 2) {
            g_autofree char *plain_array = at29_drive("o2flash", array, "");
            g_autofree char *plain_state = at29_drive("o2flash-state",
                                                        state, "");

            backing_startup_error(seed_path, plain_array, plain_state, true,
                                   "invalid backing state or array digest");
        }
    }
    for (unsigned target = 0; target < 2; target++) {
        g_autofree char *array = g_strdup_printf("%s/read-%u.raw",
                                                 test_dir, target);
        g_autofree char *state = g_strdup_printf("%s/read-%u.state",
                                                 test_dir, target);
        g_autofree char *config = g_strdup_printf("%s/read-%u.cfg",
                                                 test_dir, target);
        g_autofree char *debug = g_strdup_printf("blkdebug:%s:%s", config,
                                                  target ? state : array);
        g_autofree char *array_drive = at29_drive("o2flash",
                                                   target ? array : debug, "");
        g_autofree char *state_drive = at29_drive("o2flash-state",
                                                   target ? debug : state, "");

        g_assert_true(g_file_set_contents(array, (const char *)seed,
                                      512 * 1024, NULL));
        make_at29_state(state, seed, 0);
        g_assert_true(g_file_set_contents(config,
            "[inject-error]\nevent = \"none\"\niotype = \"read\"\n"
            "errno = \"5\"\nimmediately = \"on\"\n", -1, NULL));
        backing_startup_error(seed_path, array_drive, state_drive, true,
                               target ? "reading state backend" :
                                        "reading array backend");
    }
}


static void test_at29_backing_incoming(void)
{
    g_autofree char *seed_path = NULL;
    g_autofree uint8_t *seed = make_image(512 * 1024,
        "at29-incoming-seed.bin", &seed_path);
    g_autofree char *array = g_build_filename(test_dir, "incoming.raw", NULL);
    g_autofree char *state = g_build_filename(test_dir, "incoming.state", NULL);
    g_autofree char *socket = g_build_filename(test_dir, "incoming.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *extra = g_strdup_printf(
        "-drive if=none,id=o2flash,file=%s,format=raw "
        "-drive if=none,id=o2flash-state,file=%s,format=raw", array, state);
    g_autofree char *incoming = g_strdup_printf("%s -incoming defer", extra);
    g_autofree char *stored = NULL;
    gsize length;
    QTestState *from, *to;
    SgiWait wait;

    seed[0x10020] ^= 0x80;
    g_assert_true(g_file_set_contents(array, (const char *)seed,
                                      512 * 1024, NULL));
    make_at29_state(state, seed, 0);
    from = start_at29(seed_path, "disabled", "unlocked", "unlocked");
    to = start_at29_extra(seed_path, "disabled", "unlocked", "unlocked",
                           incoming);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s,"
        "'exit-on-error':false}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_start(&wait, "persistent AT29 refuses incoming migration");
    for (;;) {
        g_autoptr(QDict) reply = qtest_qmp_assert_success_ref(to,
            "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(reply, "status");

        if (!strcmp(status, "failed")) {
            g_assert_nonnull(strstr(qdict_get_str(reply, "error-desc"),
                                    "sgi-o2-rom"));
            break;
        }
        g_assert_cmpstr(status, !=, "completed");
        sgi_wait_pause(&wait, 1000);
    }
    qtest_quit(from);
    qtest_quit(to);
    g_assert_true(g_file_get_contents(array, &stored, &length, NULL));
    g_assert_cmpmem(stored, length, seed, 512 * 1024);
    to = start_at29_extra(seed_path, "disabled", "unlocked", "unlocked",
                           extra);
    g_assert_cmphex(qtest_readb(to, ROM_BASE + 0x10020), ==, seed[0x10020]);
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
    qtest_add_func("/sgi/o2-rom/at29-partial-controls",
                    test_at29_partial_controls);
    qtest_add_func("/sgi/o2-rom/at29-partial-abort",
                    test_at29_partial_abort);
    qtest_add_func("/sgi/o2-rom/at29-partial-protection",
                    test_at29_partial_protection);
    qtest_add_func("/sgi/o2-rom/at29-partial-counts",
                    test_at29_partial_counts);
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
    qtest_add_func("/sgi/o2-rom/backing/persistence",
                    test_at29_backing_persistence);
    qtest_add_func("/sgi/o2-rom/backing/validation",
                    test_at29_backing_validation);
    qtest_add_func("/sgi/o2-rom/backing/incomplete",
                    test_at29_backing_incomplete);
    qtest_add_func("/sgi/o2-rom/backing/io-errors",
                    test_at29_backing_io_errors);
    qtest_add_func("/sgi/o2-rom/backing/incoming", test_at29_backing_incoming);
    result = g_test_run();
    g_free(test_dir);
    return result;
}
