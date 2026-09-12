/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define BASE UINT64_C(0x78000000)
#define MD(_x) (BASE + (_x))
#define MD_PATH "/machine/peripheral/md"
#define DIRECTORY_BASE UINT64_C(0x100000000)
#define DIRECTORY(_x) (DIRECTORY_BASE + (_x))
#define MEMORY_BASE UINT64_C(0x400000000)
#define MEMORY(_x) (MEMORY_BASE + (_x))
#define MEMORY_BANK_SIZE UINT64_C(0x40000000)
#define DIRECTORY_BANK_SIZE UINT64_C(0x08000000)
#define BEDROCK_BASE UINT64_C(0x70000000)
#define BEDROCK_MD(_x) (BEDROCK_BASE + 0x780000 + (_x))
#define DIRECTORY_LINE_01A00000 0x0340000
#define DIRECTORY_OTHER_LINE 0x0123450
#define DIRECTORY_LAST_512M 0x3fffff0
#define DIRECTORY_FIRST_ABSENT_512M 0x4000000
#define DIRECTORY_LAST_256M 0x1fffff0
#define DIRECTORY_FIRST_ABSENT_256M 0x2000000
#define DIRECTORY_COMPAT_ENTRIES 74
#define PROTECTION_FIRST 0x408
#define PROTECTION_INTERLEAVE_3 0x438
#define PROTECTION_NEXT_PAGE 0xc08
#define PROTECTION_SAMPLE_STRIDE 0x800000
#define PROTECTION_FITTED_192M 0x01800408
#define PROTECTION_FITTED_448M 0x03800408
#define PROTECTION_BANK1_HIGH 0xf800408
#define PROTECTION_LAST 0x3ffffc38
#define PROTECTION_FIRST_ABSENT 0x40000408
#define PROTECTION_MACHINE_BASE UINT64_C(0x101c0000000)
#define PROTECTION_MACHINE_FIRST \
    (PROTECTION_MACHINE_BASE + PROTECTION_FIRST)
#define PROTECTION_MACHINE_512M_ALIAS \
    (PROTECTION_MACHINE_BASE + 0x04000408)
#define PROTECTION_MACHINE_BANK1_HIGH \
    (PROTECTION_MACHINE_BASE + PROTECTION_BANK1_HIGH)
#define DIRECTORY_UNFITTED_512M (PROTECTION_MACHINE_BASE + 0x04000000)
#define PROM_SIZE UINT64_C(0x1686a8)
#define NODE_MEMORY_512M \
    "-object memory-backend-ram,id=node-memory,size=512M "

/*
 * The Origin 300 machine address of the five-eighths ECC-byte carrier for
 * node zero. The machine does not map it yet, so these tests place the
 * device's own region there and use the datasheet's addresses verbatim.
 */
#define ECC_BASE UINT64_C(0x10140000000)
#define ECC(_x) (ECC_BASE + (_x))
#define ECC_FIRST_ABSENT_BYTE_512M 0x4000007
#define ECC_ADMITTED_HALFWORD_LOW 0x4000006
#define ECC_ADMITTED_HALFWORD_HIGH 0x8000006

/*
 * The three diagnostics the reviewed contract spells out character for
 * character. Matching them is what separates this model from an empty
 * mapping, a full-aperture sink and an over-wide handler.
 */
#define ECC_WRITE_UNALIGNED \
    "sgi-bedrock-md: ECC backdoor write offset 0x00000007 size 2 " \
    "rejected: unaligned halfword"
#define ECC_WRITE_WIDTH \
    "sgi-bedrock-md: ECC backdoor write offset 0x00000004 size 4 " \
    "rejected: unsupported width"
#define ECC_WRITE_ABSENT \
    "sgi-bedrock-md: ECC backdoor write offset 0x04000007 size 1 " \
    "rejected: outside fitted memory"

/* Every diagnostic carries this, so counting it counts all of them. */
#define ECC_DIAGNOSTIC "ECC backdoor "

#define ECC_LOG_OPTIONS "-d guest_errors -D "
#define MEMORY_LOG_OPTIONS "-d guest_errors -D "
#define MEMORY_DIAGNOSTIC "sgi-bedrock-md: memory "
#define SELFTEST_DIAGNOSTIC "sgi-bedrock-md: unsupported self-test command"

#define SELFTEST_CONTROL 0x20030
#define SELFTEST_DATA 0x20038
#define SELFTEST_ERROR_ADDRESS 0x20040
#define SELFTEST_STATUS 0x20048
#define SELFTEST_DIRECTORY_ERROR 0x40058
#define SELFTEST_MEMORY_ERROR0 0x60020
#define SELFTEST_INTERVAL_NS 1000
#define SELFTEST_DONE 2
#define SELFTEST_PASSED 1

/*
 * Bits 58:56 are write-only in the public definition. Reading them back,
 * like the zero cold-reset value of the whole register, is model policy.
 */
#define MEMORY_CONFIG_MASK UINT64_C(0x373ff003fbfbfbfb)
#define REPLY_QUEUE UINT64_C(0x001818180c0c0c00)
#define REQUEST_QUEUE UINT64_C(0x0000000004040400)

/* Every register with retained storage and its writable mask. */
static const struct {
    uint64_t offset;
    uint64_t mask;
} storage_registers[] = {
    { 0x10, 0xff },
    { 0x18, UINT64_C(0x100ff03ff) },
    { 0x10000, UINT64_C(0x8fffffffffff7ff3) },
    { 0x10008, UINT64_C(0x8fffffffffff7ff3) },
    { 0x10010, UINT64_C(0x1ffffffffff) },
    { 0x10018, UINT64_C(0x1ffffffffff) },
    { 0x20008, UINT64_C(0xffffffffffff) },
    { 0x20038, 0xff }, { 0x20060, 3 },
    { 0x40040, UINT64_C(0x8000003f00007fff) },
    { 0x40048, UINT64_C(0xc000003f00007fff) },
    { 0x40060, 3 }, { 0x60000, 1 }, { 0x60040, 1 },
};

static const uint64_t protection_bank_bounds[8][2] = {
    { 0x00000408, 0x07800408 },
    { 0x08000408, 0x0f800408 },
    { 0x10000408, 0x17800408 },
    { 0x18000408, 0x1f800408 },
    { 0x20000408, 0x27800408 },
    { 0x28000408, 0x2f800408 },
    { 0x30000408, 0x37800408 },
    { 0x38000408, 0x3f800408 },
};

/*
 * Captured error results, migration candidates and the LED banks read zero
 * while the model generates no event. A guest write is dropped, so no valid
 * or error state can come from guest input.
 */
static const uint64_t policy_zero_registers[] = {
    0x20, 0x28, 0x20010, 0x20018, 0x20020, 0x20028, 0x20040,
    0x40030, 0x40038, 0x40058, 0x60020, 0x60028, 0x60030, 0x60038,
};

static QTestState *start_with_memory(const char *size, const char *extra)
{
    QTestState *s = qtest_initf("-M none -cpu R10000 "
                                "-object memory-backend-ram,"
                                "id=node-memory,size=%s "
                                "-device sgi-bedrock-md,id=md,"
                                "node-memory=/objects/node-memory/"
                                "node-memory[0] -S %s", size, extra);

    qtest_sysbus_mmio_map(s, MD_PATH, 0, BASE);
    qtest_sysbus_mmio_map(s, MD_PATH, 1, DIRECTORY_BASE);
    qtest_sysbus_mmio_map(s, MD_PATH, 2, ECC_BASE);
    qtest_sysbus_mmio_map(s, MD_PATH, 3, MEMORY_BASE);
    return s;
}

static QTestState *start(const char *extra)
{
    return start_with_memory("512M", extra);
}

static void ensure_clock_running(QTestState *s)
{
    g_autoptr(QDict) response =
        qtest_qmp(s, "{'execute':'query-status'}");
    QDict *status = qdict_get_qdict(response, "return");

    if (!qdict_get_bool(status, "running")) {
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
    }
}

static QTestState *start_clock_with_memory(const char *size, const char *extra)
{
    QTestState *s = start_with_memory(size, extra);

    ensure_clock_running(s);
    return s;
}

static QTestState *start_parent(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 " NODE_MEMORY_512M
                               "-object memory-backend-ram,id=dma,size=2M "
                               "-device sgi-bedrock,id=bedrock,"
                               "dma-memory=/objects/dma/dma[0],"
                               "node-memory=/objects/node-memory/"
                               "node-memory[0] -S");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 0, BEDROCK_BASE);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 5,
                         DIRECTORY_BASE);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 7, MEMORY_BASE);
    return s;
}

static char *log_file_new(void)
{
    GError *error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("sn1-md-log-XXXXXX", &path, &error);

    g_assert_no_error(error);
    close(fd);
    return path;
}

static char *empty_prom_new(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd = g_file_open_tmp("sn1-md-prom-XXXXXX.bin", &path, &error);

    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static char *log_file_take(const char *path)
{
    GError *error = NULL;
    char *contents = NULL;

    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    unlink(path);
    return contents;
}

static unsigned log_count(const char *contents, const char *needle)
{
    unsigned found = 0;

    for (const char *p = strstr(contents, needle); p;
         p = strstr(p + strlen(needle), needle)) {
        found++;
    }
    return found;
}

static uint64_t memory_bank_config(unsigned locational_bank,
                                   unsigned size_code)
{
    unsigned shift = (locational_bank / 2) * 8;
    uint64_t byte = (size_code << 4) | (1 << (locational_bank & 1));

    return byte << shift;
}

static uint64_t memory_bank_size_config(unsigned locational_bank,
                                        unsigned size_code)
{
    return (uint64_t)size_code << ((locational_bank / 2) * 8 + 4);
}

static void selftest_start(QTestState *s, unsigned selector,
                           uint64_t data)
{
    qtest_writeq(s, MD(SELFTEST_DATA), data);
    qtest_writeq(s, MD(SELFTEST_CONTROL), (selector << 4) | 1);
}

static void assert_selftest_result(const char *memory_size,
                                   uint64_t memory_config,
                                   unsigned selector,
                                   uint64_t expected_status)
{
    QTestState *s = start_clock_with_memory(memory_size, "");

    qtest_writeq(s, MD(0x8), memory_config);
    selftest_start(s, selector, 0x5a);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS - 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    expected_status);
    qtest_quit(s);
}

static void test_selftest_phases(void)
{
    uint64_t config = (UINT64_C(3) << 32) | memory_bank_config(0, 1);
    QTestState *s = start_clock_with_memory("64M", "");

    qtest_writeq(s, MD(0x8), config);
    selftest_start(s, 3, 0xda);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_CONTROL)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_writeq(s, MD(0x8), 0);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS - 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);

    qtest_writeq(s, MD(0x8), config);
    selftest_start(s, 3, 0x5a);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    qtest_quit(s);
}

static void test_selftest_bank_remap(void)
{
    uint64_t config = (UINT64_C(3) << 32) | memory_bank_config(2, 1);
    QTestState *s = start_clock_with_memory("64M", "");

    /* Selector one maps to locational bank two; bank one is absent. */
    qtest_writeq(s, MD(0x8), config);
    selftest_start(s, 1, 0x5a);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    qtest_quit(s);
}

static void test_selftest_negative_carriers(void)
{
    assert_selftest_result("64M", 0, 1, SELFTEST_DONE);
    assert_selftest_result("64M", memory_bank_size_config(1, 1), 1,
                           SELFTEST_DONE);
    assert_selftest_result("64M", memory_bank_config(1, 6), 1,
                           SELFTEST_DONE);
    assert_selftest_result("96M", memory_bank_config(0, 1) |
                                  memory_bank_config(1, 1), 1,
                           SELFTEST_DONE);
    assert_selftest_result("128M", memory_bank_config(0, 6) |
                                   memory_bank_config(1, 1), 1,
                           SELFTEST_DONE);
}

static void test_selftest_replacement(void)
{
    QTestState *s = start_clock_with_memory("64M", "");

    qtest_writeq(s, MD(0x8), memory_bank_config(1, 1));
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2);
    qtest_writeq(s, MD(0x8), 0);
    selftest_start(s, 1, 0x5a);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2 - 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, SELFTEST_DONE);
    qtest_quit(s);
}

static void assert_selftest_cancelled(QTestState *s)
{
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
}

static void test_selftest_cancellation(void)
{
    QTestState *s = start_clock_with_memory("64M", "");

    qtest_writeq(s, MD(0x8), memory_bank_config(1, 1));
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 4);
    assert_selftest_cancelled(s);

    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 4);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);

    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 2);
    assert_selftest_cancelled(s);

    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 2);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);

    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2);
    qtest_set_irq_in(s, MD_PATH, "soft-reset", 0, 1);
    assert_selftest_cancelled(s);

    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS / 2);
    qtest_system_reset(s);
    assert_selftest_cancelled(s);
    qtest_quit(s);
}

static void test_selftest_unsupported_writes(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf("-d unimp -D %s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_clock_with_memory("64M", extra);

    qtest_writeq(s, MD(0x8), memory_bank_config(1, 1));
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, 400);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 3);
    qtest_clock_step(s, 599);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, 0);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 0x100);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    qtest_writeq(s, MD(SELFTEST_CONTROL), 0x14);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_cmpuint(log_count(contents, SELFTEST_DIAGNOSTIC), ==, 3);
}

static void test_selftest_read_only_results(void)
{
    static const uint64_t detail[] = {
        SELFTEST_ERROR_ADDRESS, SELFTEST_DIRECTORY_ERROR,
        SELFTEST_MEMORY_ERROR0, SELFTEST_MEMORY_ERROR0 + 8,
        SELFTEST_MEMORY_ERROR0 + 0x10, SELFTEST_MEMORY_ERROR0 + 0x18,
    };
    QTestState *s = start_clock_with_memory("64M", "");

    selftest_start(s, 1, 0x1da);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_DATA)), ==, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, SELFTEST_DONE);
    qtest_writeq(s, MD(SELFTEST_STATUS), UINT64_MAX);
    for (unsigned i = 0; i < ARRAY_SIZE(detail); i++) {
        qtest_writeq(s, MD(detail[i]), UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, MD(detail[i])), ==, 0);
    }
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==, SELFTEST_DONE);
    qtest_quit(s);
}

static void test_selftest_access_width_policy(void)
{
    QTestState *s = start_clock_with_memory("64M", "");

    qtest_writeq(s, MD(0x8), memory_bank_config(1, 1));
    selftest_start(s, 1, 0xda);
    qtest_clock_step(s, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    qtest_writeb(s, MD(SELFTEST_CONTROL), 4);
    qtest_writew(s, MD(SELFTEST_CONTROL), 4);
    qtest_writel(s, MD(SELFTEST_CONTROL), 4);
    qtest_writeq(s, MD(SELFTEST_CONTROL) + 1, 4);
    qtest_writel(s, MD(SELFTEST_STATUS), UINT32_MAX);
    g_assert_cmphex(qtest_readq(s, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    qtest_quit(s);
}

static unsigned directory_walk(uint64_t base, uint64_t offsets[49])
{
    unsigned count = 0;

    offsets[count++] = base;
    for (unsigned bit = 3; bit <= 26; bit++) {
        offsets[count++] = base | (UINT64_C(1) << bit);
    }
    offsets[count++] = base | UINT64_C(0x07fffff8);
    for (unsigned bit = 3; bit <= 25; bit++) {
        offsets[count++] = (base | UINT64_C(0x07fffff8)) ^
                           (UINT64_C(1) << bit);
    }
    g_assert_cmpuint(count, ==, 49);
    return count;
}

static unsigned directory_untranslated_walk(
    uint64_t offsets[DIRECTORY_COMPAT_ENTRIES])
{
    uint64_t bank0[49], bank1[49];
    unsigned count = 0;

    directory_walk(0, bank0);
    directory_walk(DIRECTORY_BANK_SIZE, bank1);
    for (unsigned i = 0; i < ARRAY_SIZE(bank0); i++) {
        if (bank0[i] >= DIRECTORY_FIRST_ABSENT_512M) {
            offsets[count++] = bank0[i];
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(bank1); i++) {
        offsets[count++] = bank1[i];
    }
    g_assert_cmphex(bank1[7], ==, DIRECTORY_BANK_SIZE + 0x200);
    g_assert_cmpuint(count, ==, DIRECTORY_COMPAT_ENTRIES);
    return count;
}

static void test_memory_banks(void)
{
    QTestState *s = start_with_memory("192M", "");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1) |
                              memory_bank_config(2, 2));
    qtest_writeq(s, MEMORY(0x0123450), UINT64_C(0x0123456789abcdef));
    qtest_writeq(s, MEMORY(2 * MEMORY_BANK_SIZE + 0x0765430),
                 UINT64_C(0xfedcba9876543210));

    g_assert_cmphex(qtest_readq(s, MEMORY(0x0123450)), ==,
                    UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s,
                    MEMORY(2 * MEMORY_BANK_SIZE + 0x0765430)), ==,
                    UINT64_C(0xfedcba9876543210));
    qtest_quit(s);
}

static void test_directory_bank_translation(void)
{
    const uint64_t config = memory_bank_config(0, 1);
    const uint64_t value = UINT64_C(0x89abcdef);
    QTestState *s = start_with_memory("64M", "");

    /* Selector one maps software bank one to locational bank zero. */
    qtest_writeq(s, MD(0x8), (UINT64_C(1) << 32) | config);
    qtest_writeq(s, DIRECTORY(DIRECTORY_BANK_SIZE + 0x20), value);
    g_assert_cmphex(qtest_readq(s,
                    DIRECTORY(DIRECTORY_BANK_SIZE + 0x20)), ==, value);

    qtest_writeq(s, MD(0x8), config);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(0x20)), ==, value);
    g_assert_cmphex(qtest_readq(s,
                    DIRECTORY(DIRECTORY_BANK_SIZE + 0x20)), ==, 0);
    qtest_quit(s);
}

static void test_memory_size_boundaries(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_with_memory("192M", extra);

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1) |
                              memory_bank_config(2, 2));
    qtest_writeb(s, MEMORY(0x03ffffff), 0xa5);
    g_assert_cmphex(qtest_readb(s, MEMORY(0x03ffffff)), ==, 0xa5);
    qtest_writeb(s, MEMORY(0x04000000), 0x5a);
    qtest_writeb(s, MEMORY(2 * MEMORY_BANK_SIZE + 0x07ffffff), 0x3c);
    g_assert_cmphex(qtest_readb(s,
                    MEMORY(2 * MEMORY_BANK_SIZE + 0x07ffffff)), ==, 0x3c);
    qtest_writeb(s, MEMORY(2 * MEMORY_BANK_SIZE + 0x08000000), 0xc3);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
                           "memory write at offset 0x04000000 is beyond "
                           "locational bank 0 size 0x04000000"));
    g_assert_nonnull(strstr(contents,
                           "memory write at offset 0x88000000 is beyond "
                           "locational bank 2 size 0x08000000"));
    g_assert_cmpuint(log_count(contents, MEMORY_DIAGNOSTIC), ==, 2);
}

static void test_memory_absent_banks(void)
{
    g_autofree char *log_disabled = log_file_new();
    g_autofree char *log_empty = log_file_new();
    g_autofree char *log_unsupported = log_file_new();
    g_autofree char *extra_disabled =
        g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log_disabled);
    g_autofree char *extra_empty =
        g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log_empty);
    g_autofree char *extra_unsupported =
        g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log_unsupported);
    g_autofree char *contents_disabled = NULL;
    g_autofree char *contents_empty = NULL;
    g_autofree char *contents_unsupported = NULL;
    QTestState *s;

    s = start_with_memory("64M", extra_disabled);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeb(s, MEMORY(MEMORY_BANK_SIZE), 0x5a);
    qtest_quit(s);

    s = start_with_memory("64M", extra_empty);
    qtest_writeq(s, MD(0x8), 0x01);
    qtest_readb(s, MEMORY(0));
    qtest_quit(s);

    s = start_with_memory("64M", extra_unsupported);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 6));
    qtest_readb(s, MEMORY(0));
    qtest_quit(s);

    contents_disabled = log_file_take(log_disabled);
    contents_empty = log_file_take(log_empty);
    contents_unsupported = log_file_take(log_unsupported);
    g_assert_nonnull(strstr(contents_disabled,
                           "memory write at offset 0x40000000 rejected: "
                           "locational bank 1 is disabled"));
    g_assert_nonnull(strstr(contents_empty,
                           "memory read at offset 0x00000000 rejected: "
                           "locational bank 0 has size code 0"));
    g_assert_nonnull(strstr(contents_unsupported,
                           "memory read at offset 0x00000000 rejected: "
                           "locational bank 0 has unsupported size code 6"));
    g_assert_cmpuint(log_count(contents_disabled, MEMORY_DIAGNOSTIC), ==, 1);
    g_assert_cmpuint(log_count(contents_empty, MEMORY_DIAGNOSTIC), ==, 1);
    g_assert_cmpuint(log_count(contents_unsupported, MEMORY_DIAGNOSTIC), ==, 1);
}

static void test_memory_crossing_does_not_corrupt(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_with_memory("128M", extra);

    qtest_writeq(s, MD(0x8), 0x13);
    qtest_writel(s, MEMORY(0x03fffffc), 0x11223344);
    qtest_writel(s, MEMORY(MEMORY_BANK_SIZE), 0x55667788);
    qtest_writeq(s, MEMORY(0x03fffffc), UINT64_C(0xa5a5a5a5c3c3c3c3));
    g_assert_cmphex(qtest_readl(s, MEMORY(0x03fffffc)), ==, 0x11223344);
    g_assert_cmphex(qtest_readl(s, MEMORY(MEMORY_BANK_SIZE)), ==, 0x55667788);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
                           "memory write at offset 0x03fffffc size 8 "
                           "crosses locational bank 0 size 0x04000000"));
    g_assert_cmpuint(log_count(contents, MEMORY_DIAGNOSTIC), ==, 1);
}

static void test_memory_selector_and_backing_bound(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_with_memory("64M", extra);

    /* Selector one maps software bank one to locational bank zero. */
    qtest_writeq(s, MD(0x8), (UINT64_C(1) << 32) | 0x13);
    qtest_writeq(s, MEMORY(MEMORY_BANK_SIZE + 0x18),
                 UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, MEMORY(MEMORY_BANK_SIZE + 0x18)), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_writeb(s, MEMORY(0), 0xa5);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
                           "memory write at offset 0x00000000 rejected: "
                           "locational bank 1 exceeds raw backing size "
                           "0x04000000"));
    g_assert_cmpuint(log_count(contents, MEMORY_DIAGNOSTIC), ==, 1);
}

static void test_memory_selector_high_bank(void)
{
    QTestState *s = start_with_memory("64M", "");

    /* Selector three maps software bank five to locational bank six. */
    qtest_writeq(s, MD(0x8), (UINT64_C(3) << 32) |
                              memory_bank_config(6, 1));
    qtest_writeq(s, MEMORY(5 * MEMORY_BANK_SIZE + 0x28),
                 UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s, MEMORY(5 * MEMORY_BANK_SIZE + 0x28)), ==,
                    UINT64_C(0xfedcba9876543210));
    qtest_quit(s);
}

static void test_memory_access_widths_and_byte_order(void)
{
    QTestState *s = start_with_memory("64M", "");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MEMORY(0x20), UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readb(s, MEMORY(0x20)), ==, 0x01);
    g_assert_cmphex(qtest_readw(s, MEMORY(0x20)), ==, 0x0123);
    g_assert_cmphex(qtest_readl(s, MEMORY(0x20)), ==, 0x01234567);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x20)), ==,
                    UINT64_C(0x0123456789abcdef));

    qtest_writeb(s, MEMORY(0x28), 0xa5);
    qtest_writew(s, MEMORY(0x2a), 0xbcde);
    qtest_writel(s, MEMORY(0x2c), 0xf0123456);
    g_assert_cmphex(qtest_readq(s, MEMORY(0x28)), ==,
                    UINT64_C(0xa500bcdef0123456));
    qtest_quit(s);
}

static void test_memory_reset_decode(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(MEMORY_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_with_memory("64M", extra);

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_writeb(s, MEMORY(0x30), 0xa5);
    qtest_set_irq_in(s, MD_PATH, "soft-reset", 0, 1);
    g_assert_cmphex(qtest_readb(s, MEMORY(0x30)), ==, 0xa5);
    qtest_system_reset(s);
    qtest_readb(s, MEMORY(0x30));
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
                           "memory read at offset 0x00000030 rejected: "
                           "locational bank 0 is disabled"));
    g_assert_cmpuint(log_count(contents, MEMORY_DIAGNOSTIC), ==, 1);
}

static void test_memory_without_node_memory(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *contents = NULL;
    QTestState *s = qtest_initf("-M none -cpu R10000 "
                                "-device sgi-bedrock-md,id=md -S "
                                MEMORY_LOG_OPTIONS "%s", log);

    qtest_sysbus_mmio_map(s, MD_PATH, 0, BASE);
    qtest_sysbus_mmio_map(s, MD_PATH, 3, MEMORY_BASE);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    qtest_readb(s, MEMORY(0));
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
                           "memory read at offset 0x00000000 rejected: "
                           "node memory is not linked"));
    g_assert_cmpuint(log_count(contents, MEMORY_DIAGNOSTIC), ==, 1);
}

static void test_parent_exports_memory(void)
{
    QTestState *s = start_parent();

    qtest_writeq(s, BEDROCK_MD(0x8), memory_bank_config(0, 1));
    qtest_writeq(s, MEMORY(0x40), UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, MEMORY(0x40)), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_quit(s);
}

static void program_all(QTestState *s)
{
    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        qtest_writeq(s, MD(storage_registers[i].offset), UINT64_MAX);
    }
    qtest_writeq(s, MD(0x8), UINT64_MAX);
    qtest_writeq(s, MD(0x20000), UINT64_C(0x8000000000000555));
    qtest_writeq(s, MD(0x40000), 3);
    qtest_writeq(s, MD(0), 0);
}

static void assert_storage_zero(QTestState *s)
{
    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        g_assert_cmphex(qtest_readq(s, MD(storage_registers[i].offset)),
                        ==, 0);
    }
}

static void test_reset(void)
{
    uint64_t compat_offsets[DIRECTORY_COMPAT_ENTRIES];
    unsigned compat_count = directory_untranslated_walk(compat_offsets);
    QTestState *s = start("");

    program_all(s);
    qtest_set_irq_in(s, "/machine/peripheral/md", "soft-reset", 0, 1);
    assert_storage_zero(s);
    g_assert_cmphex(qtest_readq(s, MD(0x8)), ==, MEMORY_CONFIG_MASK);
    g_assert_cmphex(qtest_readq(s, MD(0x20000)), ==,
                    UINT64_C(0x8000000000000555));
    g_assert_cmphex(qtest_readq(s, MD(0x40000)), ==, 3);
    g_assert_cmphex(qtest_readq(s, MD(0)), ==, UINT64_MAX);

    program_all(s);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 5) |
                              memory_bank_config(1, 5));
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000), 0x76543210);
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8),
                 UINT64_C(0x0123456789abcdef));
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18),
                 UINT64_C(0xfedcba9876543210));
    qtest_writeq(s, DIRECTORY(PROTECTION_FIRST),
                 UINT64_C(0x599a599a599a599a));
    qtest_writeq(s, DIRECTORY(PROTECTION_FITTED_448M),
                 UINT64_C(0xa55aa55aa55aa55a));
    for (unsigned i = 0; i < compat_count; i++) {
        qtest_writeq(s, DIRECTORY(compat_offsets[i]),
                     UINT64_C(0x13579bdf00000000) | i);
    }
    qtest_system_reset(s);
    assert_storage_zero(s);
    g_assert_cmphex(qtest_readq(s, MD(0x8)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(0x20000)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(0x40000)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(0)), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s,
                               DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_FIRST)), ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_FITTED_448M)), ==, 0);
    for (unsigned i = 0; i < compat_count; i++) {
        g_assert_cmphex(qtest_readq(s, DIRECTORY(compat_offsets[i])), ==, 0);
    }
    qtest_quit(s);
}

static void test_protection_storage(void)
{
    static const uint64_t interleave_offsets[] = {
        PROTECTION_FIRST, PROTECTION_FIRST + 0x10,
        PROTECTION_FIRST + 0x20, PROTECTION_INTERLEAVE_3,
        PROTECTION_NEXT_PAGE, PROTECTION_BANK1_HIGH, PROTECTION_LAST,
    };
    QTestState *s = start("");

    for (unsigned i = 0; i < ARRAY_SIZE(interleave_offsets); i++) {
        uint64_t value = UINT64_C(0x1020304050607080) + i;

        g_assert_cmphex(qtest_readq(s, DIRECTORY(interleave_offsets[i])),
                        ==, 0);
        qtest_writeq(s, DIRECTORY(interleave_offsets[i]), value);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(interleave_offsets); i++) {
        uint64_t value = UINT64_C(0x1020304050607080) + i;

        if (i < 5) {
            g_assert_cmphex(qtest_readq(s, DIRECTORY(interleave_offsets[i])),
                            ==, value);
        } else {
            g_assert_cmphex(qtest_readq(s, DIRECTORY(interleave_offsets[i])),
                            !=, value);
        }
    }

    for (unsigned bank = 0; bank < ARRAY_SIZE(protection_bank_bounds); bank++) {
        g_assert_cmphex(protection_bank_bounds[bank][1] -
                        15 * PROTECTION_SAMPLE_STRIDE,
                        ==, protection_bank_bounds[bank][0]);
        for (unsigned sample = 0; sample < 16; sample++) {
            uint64_t offset = protection_bank_bounds[bank][1] -
                              sample * PROTECTION_SAMPLE_STRIDE;
            uint64_t value = UINT64_C(0xfedcba9876543200) +
                             bank * 16 + sample;

            qtest_writeq(s, DIRECTORY(offset), value);
        }
    }
    for (unsigned bank = 0; bank < ARRAY_SIZE(protection_bank_bounds); bank++) {
        for (unsigned sample = 0; sample < 16; sample++) {
            uint64_t offset = protection_bank_bounds[bank][1] -
                              sample * PROTECTION_SAMPLE_STRIDE;
            uint64_t value = UINT64_C(0xfedcba9876543200) +
                             bank * 16 + sample;

            if (bank == 0 && sample >= 8) {
                g_assert_cmphex(qtest_readq(s, DIRECTORY(offset)), ==, value);
            } else {
                g_assert_cmphex(qtest_readq(s, DIRECTORY(offset)), !=, value);
            }
        }
    }
    qtest_quit(s);
}

static void test_protection_capacity_probe(void)
{
    g_autofree char *prom = empty_prom_new();
    g_autofree char *quoted_prom = g_shell_quote(prom);
    g_autofree char *log = log_file_new();
    g_autofree char *contents = NULL;
    const uint64_t low = UINT64_C(0xa55aa55aa55aa55a);
    const uint64_t high = UINT64_C(0x5aa55aa55aa55aa5);
    const uint64_t absent = UINT64_C(0x599a599a599a599a);
    const uint64_t selected_lanes = UINT64_C(0x0000ffff0000ffff);
    QTestState *s = qtest_initf("-M origin300 -cpu R14000 -smp 2 "
                                "-m 512M -bios %s -S -d guest_errors -D %s",
                                quoted_prom, log);

    qtest_writeq(s, PROTECTION_MACHINE_FIRST, low);
    g_assert_cmphex(qtest_readq(s, PROTECTION_MACHINE_FIRST), ==, low);
    qtest_writeq(s, PROTECTION_MACHINE_512M_ALIAS, high);
    g_assert_cmphex(qtest_readq(s, PROTECTION_MACHINE_FIRST), ==, high);
    g_assert_cmphex(qtest_readq(s, PROTECTION_MACHINE_512M_ALIAS), ==, high);

    qtest_writeq(s, PROTECTION_MACHINE_BANK1_HIGH, absent);
    g_assert_cmphex(qtest_readq(s, PROTECTION_MACHINE_BANK1_HIGH) &
                    selected_lanes, !=, absent & selected_lanes);
    qtest_writeq(s, DIRECTORY_UNFITTED_512M, UINT64_MAX);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_null(strstr(contents, "protection"));
    g_assert_nonnull(strstr(contents,
                           "directory write at offset 0x4000000"));
    unlink(prom);
}

static void test_directory_storage(void)
{
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 4));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s,
                               DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_OTHER_LINE)), ==, 0);

    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000),
                 UINT64_C(0x89abcdef01234567));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, UINT64_C(0x01234567));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_OTHER_LINE)), ==, 0);

    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8),
                 UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8)),
                    ==, UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, UINT64_C(0x01234567));

    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000), 0x76543210);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8)),
                    ==, UINT64_C(0xfedcba9876543210));

    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 0x10),
                 0x76543210);
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18),
                 UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, UINT64_C(0x76543210));
    g_assert_cmphex(qtest_readq(s,
                               DIRECTORY(DIRECTORY_LINE_01A00000 + 0x10)),
                    ==, 0x76543210);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8)),
                    ==, UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s,
                               DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18)),
                    ==, UINT64_C(0x0123456789abcdef));

    qtest_writeq(s, MD(0x40000), 1);
    qtest_writeq(s, DIRECTORY(DIRECTORY_OTHER_LINE),
                 UINT64_C(0x89abcdef01234567));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_OTHER_LINE)),
                    ==, UINT64_C(0x89abcdef01234567));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, UINT64_C(0x76543210));
    qtest_quit(s);
}

static void test_directory_compatibility_storage(void)
{
    uint64_t offsets[DIRECTORY_COMPAT_ENTRIES];
    unsigned count = directory_untranslated_walk(offsets);
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 5) |
                              memory_bank_config(1, 5));
    qtest_writeq(s, DIRECTORY(offsets[0]),
                 UINT64_C(0x89abcdef01234567));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(offsets[0])), ==, 0x01234567);
    qtest_writeq(s, DIRECTORY(offsets[1]),
                 UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(offsets[1])), ==,
                    UINT64_C(0xfedcba9876543210));

    qtest_writeq(s, MD(0x40000), 1);
    for (unsigned i = 0; i < count; i++) {
        uint64_t value = UINT64_C(0xa500000000000000) | i;

        qtest_writeq(s, DIRECTORY(offsets[i]), value);
    }
    for (unsigned i = 0; i < count; i++) {
        g_assert_cmphex(qtest_readq(s, DIRECTORY(offsets[i])), ==,
                        UINT64_C(0xa500000000000000) | i);
    }
    qtest_writeq(s, DIRECTORY(offsets[0]),
                 UINT64_C(0x89abcdef01234567));
    qtest_writeq(s, MD(0x40000), 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(offsets[0])), ==, 0x01234567);
    qtest_writeq(s, MD(0x40000), 1);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(offsets[0])), ==,
                    UINT64_C(0x89abcdef01234567));
    qtest_quit(s);
}

static void test_directory_compatibility_capacity(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf("-d guest_errors -D %s", log);
    g_autofree char *contents = NULL;
    uint64_t offsets[DIRECTORY_COMPAT_ENTRIES];
    unsigned count = directory_untranslated_walk(offsets);
    unsigned admitted = 1;
    QTestState *s = start(extra);

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 5) |
                              memory_bank_config(1, 5));
    qtest_writeq(s, MD(0x40000), 1);
    qtest_writeq(s, DIRECTORY(0x04001230), UINT64_C(0xfeedfacecafebeef));
    for (unsigned i = 0; i < count - 1; i++) {
        qtest_writeq(s, DIRECTORY(offsets[i]), admitted++);
    }
    g_assert_cmpuint(admitted, ==, DIRECTORY_COMPAT_ENTRIES);
    qtest_writeq(s, DIRECTORY(offsets[count - 1]), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s,
                    DIRECTORY(offsets[count - 1])), ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(0x04001230)), ==,
                    UINT64_C(0xfeedfacecafebeef));
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents, "compatibility store is full"));
}

static void test_directory_access_policy(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf("-d guest_errors -D %s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start(extra);
    const uint64_t addr[] = {
        DIRECTORY(DIRECTORY_LINE_01A00000),
        DIRECTORY(DIRECTORY_LINE_01A00000 + 8),
    };
    const uint64_t value[] = {
        UINT64_C(0x1122334455667788),
        UINT64_C(0x8877665544332211),
    };

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 4));
    qtest_writeq(s, MD(0x40000), 1);
    for (unsigned i = 0; i < ARRAY_SIZE(addr); i++) {
        qtest_writeq(s, addr[i], value[i]);
        qtest_writeb(s, addr[i], 0);
        qtest_writew(s, addr[i], 0);
        qtest_writel(s, addr[i], 0);
        qtest_writeq(s, addr[i] + 1, 0);
        g_assert_cmphex(qtest_readq(s, addr[i]), ==, value[i]);
        g_assert_cmphex(qtest_readb(s, addr[i]), ==, 0);
        g_assert_cmphex(qtest_readw(s, addr[i]), ==, 0);
        g_assert_cmphex(qtest_readl(s, addr[i]), ==, 0);
        g_assert_cmphex(qtest_readq(s, addr[i] + 1), ==, 0);
    }
    qtest_writeq(s, DIRECTORY(DIRECTORY_LAST_512M), 0xc001d00d);
    qtest_writeq(s, DIRECTORY(DIRECTORY_LAST_512M + 8),
                 UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LAST_512M)),
                    ==, 0xc001d00d);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LAST_512M + 8)),
                    ==, UINT64_C(0x0123456789abcdef));

    qtest_writeq(s, DIRECTORY(PROTECTION_FITTED_448M),
                 UINT64_C(0x599a599a599a599a));
    qtest_writeb(s, DIRECTORY(PROTECTION_FITTED_448M), 0);
    qtest_writew(s, DIRECTORY(PROTECTION_FITTED_448M), 0);
    qtest_writel(s, DIRECTORY(PROTECTION_FITTED_448M), 0);
    qtest_writeq(s, DIRECTORY(PROTECTION_FITTED_448M + 1), 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_FITTED_448M)),
                    ==, UINT64_C(0x599a599a599a599a));
    qtest_writeq(s, DIRECTORY(PROTECTION_FIRST_ABSENT), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_FIRST_ABSENT)),
                    ==, 0);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents, "not an aligned 64-bit carrier word"));
}

static void test_directory_fitted_memory(void)
{
    QTestState *s = start_with_memory("256M", "");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 3));
    qtest_writeq(s, DIRECTORY(DIRECTORY_LAST_256M), 0x2468ace0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LAST_256M)),
                    ==, 0x2468ace0);
    qtest_writeq(s, DIRECTORY(DIRECTORY_FIRST_ABSENT_256M), 0x13579bdf);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_FIRST_ABSENT_256M)),
                    ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LAST_256M)), ==,
                    0x2468ace0);
    qtest_writeq(s, DIRECTORY(PROTECTION_FITTED_192M),
                 UINT64_C(0x599a599a599a599a));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_FITTED_192M)),
                    ==, UINT64_C(0x599a599a599a599a));
    qtest_quit(s);
}

static void test_parent_exports_directory(void)
{
    QTestState *s = start_parent();

    qtest_writeq(s, BEDROCK_MD(0x8), memory_bank_config(0, 4));
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000), 0x13579bdf);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, 0x13579bdf);
    qtest_writeq(s, DIRECTORY(PROTECTION_FITTED_448M),
                 UINT64_C(0x599a599a599a599a));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_FITTED_448M)),
                    ==, UINT64_C(0x599a599a599a599a));
    qtest_quit(s);
}

static void test_without_node_memory(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 "
                               "-device sgi-bedrock-md,id=md -S");

    qtest_sysbus_mmio_map(s, MD_PATH, 1, DIRECTORY_BASE);
    qtest_writeq(s, DIRECTORY(0), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(0)), ==, 0);
    qtest_writeq(s, DIRECTORY(8), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(8)), ==, 0);
    qtest_writeq(s, DIRECTORY(PROTECTION_LAST),
                 UINT64_C(0x599a599a599a599a));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_LAST)),
                    !=, UINT64_C(0x599a599a599a599a));
    qtest_quit(s);
}

static void test_register_contract(void)
{
    QTestState *s = start("");

    g_assert_cmphex(qtest_readq(s, MD(0)), ==, UINT64_MAX);
    qtest_writeq(s, MD(0), 0);
    g_assert_cmphex(qtest_readq(s, MD(0)), ==, 0);
    qtest_writeq(s, MD(0x8), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x8)), ==, MEMORY_CONFIG_MASK);
    qtest_writeq(s, MD(0x10), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x10)), ==, 0xff);
    qtest_writeq(s, MD(0x10000), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x10000)), ==,
                    UINT64_C(0x8fffffffffff7ff3));
    qtest_writeq(s, MD(0x10010), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x10010)), ==,
                    (UINT64_C(1) << 41) - 1);
    qtest_writeq(s, MD(0x20000), UINT64_C(0x8000000000000c35));
    g_assert_cmphex(qtest_readq(s, MD(0x20000)), ==,
                    UINT64_C(0x8000000000000c35));
    qtest_writeq(s, MD(0x20008), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x20008)), ==,
                    UINT64_C(0xffffffffffff));
    qtest_writeq(s, MD(0x40000), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x40000)), ==, 3);
    g_assert_cmphex(qtest_readq(s, MD(0x60)), ==, REPLY_QUEUE);
    g_assert_cmphex(qtest_readq(s, MD(0x40050)), ==, REQUEST_QUEUE);
    qtest_writeq(s, MD(0x20030), 1);
    g_assert_cmphex(qtest_readq(s, MD(0x20030)), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD(0x20048)), ==, 0);
    qtest_writeq(s, MD(0x60040), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x60040)), ==, 1);
    qtest_writeq(s, MD(0x10100), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x10010)), ==,
                    (UINT64_C(1) << 41) - 1);
    qtest_quit(s);
}

/*
 * Every register is a big-endian 64-bit word. Byte order is the hardware
 * contract. Refusing a narrower access is model policy: the datasheet
 * states the width and nothing about a narrower access.
 */
static void test_access_width_policy(void)
{
    QTestState *s = start("");
    const uint8_t bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const uint8_t expected[8] = { 0, 0, 3, 4, 5, 6, 7, 8 };
    uint8_t actual[8];

    qtest_memwrite(s, MD(0x20008), bytes, sizeof(bytes));
    qtest_memread(s, MD(0x20008), actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    g_assert_cmphex(qtest_readq(s, MD(0x20008)), ==,
                    UINT64_C(0x030405060708));
    qtest_writeb(s, MD(0x20008), 0);
    qtest_writew(s, MD(0x20008), 0);
    qtest_writel(s, MD(0x20008), 0);
    g_assert_cmphex(qtest_readq(s, MD(0x20008)), ==,
                    UINT64_C(0x030405060708));
    qtest_quit(s);
}

/* The registers outside the storage table keep state of their own. */
static void test_explicit_registers_independent(void)
{
    static const struct {
        uint64_t offset;
        uint64_t value;
    } registers[] = {
        { 0x0, UINT64_C(0x0123456789abcdef) },
        { 0x8, UINT64_C(0x3000000000000101) },
        { 0x10, 0x5a },
        { 0x20000, UINT64_C(0x8000000000000c35) },
        { 0x20008, UINT64_C(0x112233445566) },
        { 0x40000, 2 },
    };
    QTestState *s = start("");

    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        qtest_writeq(s, MD(registers[i].offset), registers[i].value);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        g_assert_cmphex(qtest_readq(s, MD(registers[i].offset)), ==,
                        registers[i].value);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(registers); i++) {
        qtest_writeq(s, MD(registers[i].offset), 0);
        for (unsigned j = 0; j < ARRAY_SIZE(registers); j++) {
            if (j != i) {
                g_assert_cmphex(qtest_readq(s, MD(registers[j].offset)), ==,
                                registers[j].value);
            }
        }
        qtest_writeq(s, MD(registers[i].offset), registers[i].value);
    }
    qtest_quit(s);
}

/*
 * A write to the reply queue register resets hardware credit counters and
 * the request queue fields are programmable up to four. The model has
 * neither credits nor queues, so both keep their fixed reads.
 */
static void test_queue_sizes_fixed(void)
{
    QTestState *s = start("");

    g_assert_cmphex(qtest_readq(s, MD(0x60)), ==, REPLY_QUEUE);
    qtest_writeq(s, MD(0x60), 0);
    g_assert_cmphex(qtest_readq(s, MD(0x60)), ==, REPLY_QUEUE);
    qtest_writeq(s, MD(0x60), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x60)), ==, REPLY_QUEUE);
    g_assert_cmphex(qtest_readq(s, MD(0x40050)), ==, REQUEST_QUEUE);
    qtest_writeq(s, MD(0x40050), UINT64_C(0x0000000001010100));
    g_assert_cmphex(qtest_readq(s, MD(0x40050)), ==, REQUEST_QUEUE);
    qtest_writeq(s, MD(0x40050), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD(0x40050)), ==, REQUEST_QUEUE);
    qtest_quit(s);
}

/*
 * The Itanium probe reads the four clear registers and expects the base
 * error registers clean. Idle presence only: the model raises no error,
 * so nothing here shows read-to-clear.
 */
static void test_error_cluster_idle(void)
{
    static const uint64_t clears[] = { 0x58, 0x40018, 0x40028, 0x60018 };
    static const uint64_t bases[] = { 0x40, 0x48, 0x40010, 0x40020, 0x60010 };
    QTestState *s = start("");

    for (unsigned i = 0; i < ARRAY_SIZE(clears); i++) {
        g_assert_cmphex(qtest_readq(s, MD(clears[i])), ==, 0);
        g_assert_cmphex(qtest_readq(s, MD(clears[i])), ==, 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(bases); i++) {
        g_assert_cmphex(qtest_readq(s, MD(bases[i])), ==, 0);
    }
    qtest_quit(s);
}

static void test_policy_zero_ignores_writes(void)
{
    static const uint64_t unsupported_commands[] = {
        0, 3, 5, 6, 7, 0x12, 0x14, 0x70, UINT64_MAX,
    };
    QTestState *s = start("");

    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        qtest_writeq(s, MD(storage_registers[i].offset), UINT64_MAX);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(policy_zero_registers); i++) {
        qtest_writeq(s, MD(policy_zero_registers[i]), UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, MD(policy_zero_registers[i])), ==, 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(unsupported_commands); i++) {
        qtest_writeq(s, MD(0x20030), unsupported_commands[i]);
        g_assert_cmphex(qtest_readq(s, MD(0x20030)), ==, 0);
        g_assert_cmphex(qtest_readq(s, MD(0x20040)), ==, 0);
        g_assert_cmphex(qtest_readq(s, MD(0x20048)), ==, 0);
        g_assert_cmphex(qtest_readq(s, MD(0x40058)), ==, 0);
        g_assert_cmphex(qtest_readq(s, MD(0x60020)), ==, 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        g_assert_cmphex(qtest_readq(s, MD(storage_registers[i].offset)), ==,
                        storage_registers[i].mask);
    }
    qtest_quit(s);
}

/*
 * Bits 62:24 are reserved. Bits 23:12 are the live counter, whose write
 * rule is open, so they are never set here.
 */
static void test_refresh_mask(void)
{
    QTestState *s = start("");

    qtest_writeq(s, MD(0x20000), UINT64_C(0xffffffffff000c35));
    g_assert_cmphex(qtest_readq(s, MD(0x20000)), ==,
                    UINT64_C(0x8000000000000c35));
    qtest_writeq(s, MD(0x20000), UINT64_C(0x7fffffffff000000));
    g_assert_cmphex(qtest_readq(s, MD(0x20000)), ==, 0);
    qtest_quit(s);
}

/* The Itanium L1 code sets and clears single bits of a 20-bit lock word. */
static void test_perf_counter_lock_word(void)
{
    QTestState *s = start("");
    uint64_t pattern = 0;

    for (unsigned bit = 0; bit < 20; bit++) {
        pattern |= UINT64_C(1) << bit;
        qtest_writeq(s, MD(0x10010),
                     qtest_readq(s, MD(0x10010)) | (UINT64_C(1) << bit));
        g_assert_cmphex(qtest_readq(s, MD(0x10010)), ==, pattern);
    }
    for (unsigned bit = 0; bit < 20; bit++) {
        pattern &= ~(UINT64_C(1) << bit);
        qtest_writeq(s, MD(0x10010),
                     qtest_readq(s, MD(0x10010)) & ~(UINT64_C(1) << bit));
        g_assert_cmphex(qtest_readq(s, MD(0x10010)), ==, pattern);
    }
    qtest_quit(s);
}

static void test_writable_masks(void)
{
    QTestState *s = start("");

    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        uint64_t value = UINT64_MAX ^ (UINT64_C(1) << (i + 2));

        for (unsigned j = 0; j < ARRAY_SIZE(storage_registers); j++) {
            qtest_writeq(s, MD(storage_registers[j].offset), 0);
        }
        qtest_writeq(s, MD(storage_registers[i].offset), value);
        g_assert_cmphex(qtest_readq(s, MD(storage_registers[i].offset)), ==,
                        value & storage_registers[i].mask);
        for (unsigned j = 0; j < ARRAY_SIZE(storage_registers); j++) {
            if (j != i) {
                g_assert_cmphex(qtest_readq(s,
                                            MD(storage_registers[j].offset)),
                                ==, 0);
            }
        }
    }
    qtest_quit(s);
}

static void migrate_wait(QTestState *from)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    for (;;) {
        g_autoptr(QDict) response =
            qtest_qmp(from, "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(qdict_get_qdict(response, "return"),
                                           "status");

        if (!strcmp(status, "completed")) {
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void migrate_pair(QTestState *from, QTestState *to, const char *uri)
{
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    migrate_wait(from);
    migrate_wait(to);
}

static void test_selftest_migration_running(void)
{
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sn1-md-selftest-migration-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    int64_t source_time;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start_clock_with_memory("64M", "");
    to = start_with_memory("64M", "-incoming defer");
    qtest_clock_set(from, 0);
    qtest_writeq(from, MD(0x8), (UINT64_C(3) << 32) |
                                  memory_bank_config(0, 1));
    selftest_start(from, 3, 0xda);
    source_time = qtest_clock_step(from, 400);
    migrate_pair(from, to, uri);
    ensure_clock_running(to);
    qtest_clock_set(to, source_time);
    g_assert_cmphex(qtest_readq(to, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(to, 599);
    g_assert_cmphex(qtest_readq(to, MD(SELFTEST_STATUS)), ==, 0);
    qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readq(to, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE | SELFTEST_PASSED);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-md-migration-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    uint64_t compat_offsets[DIRECTORY_COMPAT_ENTRIES];
    unsigned compat_count = directory_untranslated_walk(compat_offsets);
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start_clock_with_memory("512M", "");
    to = start("-incoming defer");
    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        qtest_writeq(from, MD(storage_registers[i].offset),
                     UINT64_MAX ^ (UINT64_C(1) << (i + 2)));
    }
    qtest_writeq(from, MD(0), 0x55aa);
    qtest_writeq(from, MD(0x8), memory_bank_config(0, 5) |
                                 memory_bank_config(1, 5));
    qtest_writeq(from, MD(0x20000), UINT64_C(0x8000000000000c35));
    qtest_writeq(from, MD(0x40000), 3);
    selftest_start(from, 1, UINT64_MAX);
    qtest_clock_step(from, SELFTEST_INTERVAL_NS);
    g_assert_cmphex(qtest_readq(from, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE);
    qtest_writeq(from, DIRECTORY(DIRECTORY_LINE_01A00000),
                 UINT64_C(0x89abcdef01234567));
    qtest_writeq(from, DIRECTORY(DIRECTORY_LINE_01A00000 + 8),
                 UINT64_C(0xfedcba9876543210));
    qtest_writeq(from, DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18),
                 UINT64_C(0x0123456789abcdef));
    qtest_writeq(from, DIRECTORY(PROTECTION_FIRST),
                 UINT64_C(0x599a599a599a599a));
    qtest_writeq(from, DIRECTORY(PROTECTION_FITTED_448M),
                 UINT64_C(0xa55aa55aa55aa55a));
    for (unsigned i = 0; i < compat_count; i++) {
        qtest_writeq(from, DIRECTORY(compat_offsets[i]),
                     UINT64_C(0x13579bdf00000000) | i);
    }
    migrate_pair(from, to, uri);
    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        g_assert_cmphex(qtest_readq(to, MD(storage_registers[i].offset)), ==,
                        (UINT64_MAX ^ (UINT64_C(1) << (i + 2))) &
                        storage_registers[i].mask);
    }
    g_assert_cmphex(qtest_readq(to, MD(0)), ==, 0x55aa);
    g_assert_cmphex(qtest_readq(to, MD(0x8)), ==,
                    memory_bank_config(0, 5) | memory_bank_config(1, 5));
    g_assert_cmphex(qtest_readq(to, MD(0x20000)), ==,
                    UINT64_C(0x8000000000000c35));
    g_assert_cmphex(qtest_readq(to, MD(0x40000)), ==, 3);
    g_assert_cmphex(qtest_readq(to, MD(SELFTEST_STATUS)), ==,
                    SELFTEST_DONE);
    g_assert_cmphex(qtest_readq(to, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, UINT64_C(0x89abcdef01234567));
    g_assert_cmphex(qtest_readq(to, DIRECTORY(DIRECTORY_LINE_01A00000 + 8)),
                    ==, UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(to,
                               DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18)),
                    ==, UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(to, DIRECTORY(PROTECTION_FIRST)),
                    ==, UINT64_C(0x599a599a599a599a));
    g_assert_cmphex(qtest_readq(to, DIRECTORY(PROTECTION_FITTED_448M)),
                    ==, UINT64_C(0xa55aa55aa55aa55a));
    for (unsigned i = 0; i < compat_count; i++) {
        g_assert_cmphex(qtest_readq(to, DIRECTORY(compat_offsets[i])), ==,
                        UINT64_C(0x13579bdf00000000) | i);
    }
    qtest_writeq(to, MEMORY(0x23450), UINT64_C(0x13579bdf2468ace0));
    g_assert_cmphex(qtest_readq(to, MEMORY(0x23450)), ==,
                    UINT64_C(0x13579bdf2468ace0));
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

/* Fitted ECC bytes retain state in physical doubleword order. */
static void test_ecc_carrier(void)
{
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 4));
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0);
    qtest_writew(s, ECC(6), 0xa55a);
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0xa55a);
    g_assert_cmphex(qtest_readb(s, ECC(7)), ==, 0xa5);
    g_assert_cmphex(qtest_readb(s, ECC(6)), ==, 0x5a);

    qtest_writeb(s, ECC(7), 0x12);
    qtest_writeb(s, ECC(6), 0x34);
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0x1234);

    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_LOW), 0x5aa5);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0x9999);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_LOW)), ==,
                    0x5aa5);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==,
                    0x9999);
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0x1234);
    qtest_quit(s);
}

static void test_ecc_bank_translation(void)
{
    QTestState *s = start_with_memory("64M", "");

    qtest_writeq(s, MD(0x8), (UINT64_C(1) << 32) |
                              memory_bank_config(0, 1));
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0x1357);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0x1357);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==, 0);

    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0x2468);
    qtest_writeq(s, MD(0x8), (UINT64_C(1) << 32) |
                              memory_bank_config(0, 1));
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==,
                    0x1357);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1));
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==,
                    0x2468);
    qtest_quit(s);
}

/* Width first, alignment second, admission last. */
static void test_ecc_validation_order(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(ECC_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start(extra);

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 4));
    qtest_writel(s, ECC(4), 0xffffffff);
    qtest_writew(s, ECC(7), 0xffff);
    qtest_writeb(s, ECC(ECC_FIRST_ABSENT_BYTE_512M), 0xff);
    qtest_writeb(s, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0xff);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_LOW + 0x10), 0xffff);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_HIGH + 0x10), 0xffff);
    qtest_writew(s, ECC(2 * DIRECTORY_BANK_SIZE + 6), 0xffff);
    qtest_writel(s, ECC(0x4000005), 0xffffffff);
    qtest_writew(s, ECC(0x4000007), 0xffff);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents, ECC_WRITE_WIDTH));
    g_assert_nonnull(strstr(contents, ECC_WRITE_UNALIGNED));
    g_assert_nonnull(strstr(contents, ECC_WRITE_ABSENT));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x08000006 size 1 "
        "rejected: outside fitted memory"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x04000016 size 2 "
        "rejected: outside fitted memory"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x08000016 size 2 "
        "rejected: outside fitted memory"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x10000006 size 2 "
        "rejected: outside fitted memory"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x04000005 size 4 "
        "rejected: unsupported width"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x04000007 size 2 "
        "rejected: unaligned halfword"));
    g_assert_cmpuint(log_count(contents, ECC_DIAGNOSTIC), ==, 9);
}

static void test_ecc_fitted_memory(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(ECC_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_with_memory("192M", extra);

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 1) |
                              memory_bank_config(2, 2));
    qtest_writeb(s, ECC(0x007fffff), 0xa5);
    qtest_writeb(s, ECC(2 * DIRECTORY_BANK_SIZE + 0x00ffffff), 0x5a);
    g_assert_cmphex(qtest_readb(s, ECC(0x007fffff)), ==, 0xa5);
    g_assert_cmphex(qtest_readb(s,
                    ECC(2 * DIRECTORY_BANK_SIZE + 0x00ffffff)), ==, 0x5a);
    qtest_writeb(s, ECC(DIRECTORY_BANK_SIZE + 7), 0xff);
    qtest_writeb(s, ECC(2 * DIRECTORY_BANK_SIZE + 0x01000007), 0xff);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x08000007 size 1 "
        "rejected: outside fitted memory"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x11000007 size 1 "
        "rejected: outside fitted memory"));
    g_assert_cmpuint(log_count(contents, ECC_DIAGNOSTIC), ==, 2);
}

static void test_ecc_without_node_memory(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *contents = NULL;
    QTestState *s = qtest_initf("-M none -cpu R10000 "
                                "-device sgi-bedrock-md,id=md -S "
                                ECC_LOG_OPTIONS "%s", log);

    qtest_sysbus_mmio_map(s, MD_PATH, 2, ECC_BASE);
    qtest_writeb(s, ECC(0), 0xff);
    g_assert_cmphex(qtest_readb(s, ECC(0)), ==, 0);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_LOW), 0xa55a);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0x5aa5);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_LOW)), ==,
                    0xa55a);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==,
                    0x5aa5);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x00000000 size 1 "
        "rejected: outside fitted memory"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor read offset 0x00000000 size 1 "
        "rejected: outside fitted memory"));
    g_assert_cmpuint(log_count(contents, ECC_DIAGNOSTIC), ==, 2);
}

static void test_ecc_reset(void)
{
    QTestState *s = start("");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 4));
    qtest_writew(s, ECC(6), 0xa55a);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_LOW), 0x5aa5);
    qtest_writew(s, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0x9999);
    qtest_system_reset(s);
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 4));
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_LOW)), ==, 0);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==, 0);
    qtest_quit(s);
}

static void test_ecc_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-md-ecc-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start("");
    to = start("-incoming defer");
    qtest_writeq(from, MD(0x8), memory_bank_config(0, 4));
    qtest_writew(from, ECC(6), 0xa55a);
    qtest_writew(from, ECC(ECC_ADMITTED_HALFWORD_LOW), 0x5aa5);
    qtest_writew(from, ECC(ECC_ADMITTED_HALFWORD_HIGH), 0x9999);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    migrate_wait(from);
    migrate_wait(to);
    g_assert_cmphex(qtest_readw(to, ECC(6)), ==, 0xa55a);
    g_assert_cmphex(qtest_readw(to, ECC(ECC_ADMITTED_HALFWORD_LOW)), ==,
                    0x5aa5);
    g_assert_cmphex(qtest_readw(to, ECC(ECC_ADMITTED_HALFWORD_HIGH)), ==,
                    0x9999);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_parent_exports_ecc(void)
{
    QTestState *s = qtest_init("-M none -cpu R10000 " NODE_MEMORY_512M
                               "-object memory-backend-ram,id=dma,size=2M "
                               "-device sgi-bedrock,id=bedrock,"
                               "dma-memory=/objects/dma/dma[0],"
                               "node-memory=/objects/node-memory/"
                               "node-memory[0] -S");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 0, BEDROCK_BASE);
    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 6, ECC_BASE);
    qtest_writeq(s, BEDROCK_MD(0x8), memory_bank_config(0, 4));
    qtest_writew(s, ECC(6), 0xa55a);
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0xa55a);
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-md/register-contract", test_register_contract);
    qtest_add_func("sn1/bedrock-md/selftest-phases", test_selftest_phases);
    qtest_add_func("sn1/bedrock-md/selftest-bank-remap",
                   test_selftest_bank_remap);
    qtest_add_func("sn1/bedrock-md/selftest-negative-carriers",
                   test_selftest_negative_carriers);
    qtest_add_func("sn1/bedrock-md/selftest-replacement",
                   test_selftest_replacement);
    qtest_add_func("sn1/bedrock-md/selftest-cancellation",
                   test_selftest_cancellation);
    qtest_add_func("sn1/bedrock-md/selftest-unsupported-writes",
                   test_selftest_unsupported_writes);
    qtest_add_func("sn1/bedrock-md/selftest-read-only-results",
                   test_selftest_read_only_results);
    qtest_add_func("sn1/bedrock-md/selftest-access-width-policy",
                   test_selftest_access_width_policy);
    qtest_add_func("sn1/bedrock-md/selftest-migration-running",
                   test_selftest_migration_running);
    qtest_add_func("sn1/bedrock-md/memory-banks", test_memory_banks);
    qtest_add_func("sn1/bedrock-md/directory-bank-translation",
                   test_directory_bank_translation);
    qtest_add_func("sn1/bedrock-md/memory-size-boundaries",
                   test_memory_size_boundaries);
    qtest_add_func("sn1/bedrock-md/memory-absent-banks",
                   test_memory_absent_banks);
    qtest_add_func("sn1/bedrock-md/memory-crossing-does-not-corrupt",
                   test_memory_crossing_does_not_corrupt);
    qtest_add_func("sn1/bedrock-md/memory-selector-and-backing-bound",
                   test_memory_selector_and_backing_bound);
    qtest_add_func("sn1/bedrock-md/memory-selector-high-bank",
                   test_memory_selector_high_bank);
    qtest_add_func("sn1/bedrock-md/memory-access-widths-and-byte-order",
                   test_memory_access_widths_and_byte_order);
    qtest_add_func("sn1/bedrock-md/memory-reset-decode",
                   test_memory_reset_decode);
    qtest_add_func("sn1/bedrock-md/memory-without-node-memory",
                   test_memory_without_node_memory);
    qtest_add_func("sn1/bedrock-md/parent-memory-export",
                   test_parent_exports_memory);
    qtest_add_func("sn1/bedrock-md/access-width-policy",
                   test_access_width_policy);
    qtest_add_func("sn1/bedrock-md/directory-storage",
                   test_directory_storage);
    qtest_add_func("sn1/bedrock-md/directory-compatibility-storage",
                   test_directory_compatibility_storage);
    qtest_add_func("sn1/bedrock-md/directory-compatibility-capacity",
                   test_directory_compatibility_capacity);
    qtest_add_func("sn1/bedrock-md/protection-storage",
                   test_protection_storage);
    qtest_add_func("sn1/bedrock-md/protection-capacity-probe",
                   test_protection_capacity_probe);
    qtest_add_func("sn1/bedrock-md/directory-access-policy",
                   test_directory_access_policy);
    qtest_add_func("sn1/bedrock-md/directory-fitted-memory",
                   test_directory_fitted_memory);
    qtest_add_func("sn1/bedrock-md/parent-directory-export",
                   test_parent_exports_directory);
    qtest_add_func("sn1/bedrock-md/without-node-memory",
                   test_without_node_memory);
    qtest_add_func("sn1/bedrock-md/explicit-registers",
                   test_explicit_registers_independent);
    qtest_add_func("sn1/bedrock-md/queue-sizes", test_queue_sizes_fixed);
    qtest_add_func("sn1/bedrock-md/error-cluster-idle",
                   test_error_cluster_idle);
    qtest_add_func("sn1/bedrock-md/policy-zero-writes",
                   test_policy_zero_ignores_writes);
    qtest_add_func("sn1/bedrock-md/refresh-mask", test_refresh_mask);
    qtest_add_func("sn1/bedrock-md/perf-counter-lock-word",
                   test_perf_counter_lock_word);
    qtest_add_func("sn1/bedrock-md/reset", test_reset);
    qtest_add_func("sn1/bedrock-md/migration", test_migration);
    qtest_add_func("sn1/bedrock-md/writable-masks", test_writable_masks);
    qtest_add_func("sn1/bedrock-md/ecc-carrier", test_ecc_carrier);
    qtest_add_func("sn1/bedrock-md/ecc-bank-translation",
                   test_ecc_bank_translation);
    qtest_add_func("sn1/bedrock-md/ecc-validation-order",
                   test_ecc_validation_order);
    qtest_add_func("sn1/bedrock-md/ecc-fitted-memory", test_ecc_fitted_memory);
    qtest_add_func("sn1/bedrock-md/ecc-without-node-memory",
                   test_ecc_without_node_memory);
    qtest_add_func("sn1/bedrock-md/ecc-reset", test_ecc_reset);
    qtest_add_func("sn1/bedrock-md/ecc-migration", test_ecc_migration);
    qtest_add_func("sn1/bedrock-md/parent-ecc-export",
                   test_parent_exports_ecc);
    return g_test_run();
}
