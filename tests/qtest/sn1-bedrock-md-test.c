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
#define PROTECTION_FIRST 0x408
#define PROTECTION_INTERLEAVE_3 0x438
#define PROTECTION_NEXT_PAGE 0xc08
#define PROTECTION_SAMPLE_STRIDE 0x800000
#define PROTECTION_BANK1_HIGH 0xf800408
#define PROTECTION_LAST 0x3ffffc38
#define PROTECTION_FIRST_ABSENT 0x40000408
#define NODE_MEMORY_512M \
    "-object memory-backend-ram,id=node-memory,size=512M "

/*
 * The Origin 300 machine address of the five-eighths ECC-byte carrier for
 * node zero. The machine does not map it yet, so these tests place the
 * device's own region there and use the datasheet's addresses verbatim.
 */
#define ECC_BASE UINT64_C(0x10140000000)
#define ECC(_x) (ECC_BASE + (_x))
#define ECC_LAST_DOUBLEWORD_512M 0x3fffff8
#define ECC_LAST_BYTE_512M 0x3ffffff
#define ECC_LAST_HALFWORD_512M 0x3fffffe
#define ECC_FIRST_ABSENT_BYTE_512M 0x4000007
#define ECC_FIRST_ABSENT_HALFWORD_512M 0x4000006
#define ECC_LAST_BYTE_256M 0x1ffffff
#define ECC_FIRST_ABSENT_BYTE_256M 0x2000007

/*
 * The four diagnostics the reviewed contract spells out character for
 * character. Matching them is what separates this model from an empty
 * mapping, a full-aperture sink and an over-wide handler.
 */
#define ECC_READ_UNESTABLISHED \
    "sgi-bedrock-md: ECC backdoor read offset 0x00000007 size 1 " \
    "rejected: readback unestablished"
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

#define ECC_LOG_OPTIONS "-d guest_errors,unimp -D "
#define MEMORY_LOG_OPTIONS "-d guest_errors -D "
#define MEMORY_DIAGNOSTIC "sgi-bedrock-md: memory "

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
 * Captured status, self-test results, migration candidates and the LED
 * banks read zero while the model generates no event. A guest write is
 * dropped, so no valid, done or passed bit can come from guest input.
 */
static const uint64_t policy_zero_registers[] = {
    0x20, 0x28, 0x20010, 0x20018, 0x20020, 0x20028, 0x20040, 0x20048,
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
    qtest_writeq(s, MD(0x8), memory_bank_config(0, 4));
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000), 0x76543210);
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8),
                 UINT64_C(0x0123456789abcdef));
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18),
                 UINT64_C(0xfedcba9876543210));
    qtest_writeq(s, DIRECTORY(PROTECTION_BANK1_HIGH),
                 UINT64_C(0x599a599a599a599a));
    qtest_writeq(s, DIRECTORY(PROTECTION_LAST),
                 UINT64_C(0xa55aa55aa55aa55a));
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
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_BANK1_HIGH)), ==, 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_LAST)), ==, 0);
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
        g_assert_cmphex(qtest_readq(s, DIRECTORY(interleave_offsets[i])),
                        ==, UINT64_C(0x1020304050607080) + i);
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

            g_assert_cmphex(qtest_readq(s, DIRECTORY(offset)), ==, value);
        }
    }
    qtest_quit(s);
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
    qtest_writeq(s, DIRECTORY(DIRECTORY_FIRST_ABSENT_512M),
                 UINT64_C(0x13579bdf2468ace0));
    g_assert_cmphex(qtest_readq(s,
                               DIRECTORY(DIRECTORY_FIRST_ABSENT_512M)),
                    ==, 0);
    qtest_writeq(s, DIRECTORY(DIRECTORY_FIRST_ABSENT_512M + 8),
                 UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(s,
                               DIRECTORY(DIRECTORY_FIRST_ABSENT_512M + 8)),
                    ==, 0);

    qtest_writeq(s, DIRECTORY(DIRECTORY_LAST_512M), 0xc001d00d);
    qtest_writeq(s, DIRECTORY(DIRECTORY_LAST_512M + 8),
                 UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LAST_512M)),
                    ==, 0xc001d00d);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LAST_512M + 8)),
                    ==, UINT64_C(0x0123456789abcdef));

    qtest_writeq(s, DIRECTORY(PROTECTION_BANK1_HIGH),
                 UINT64_C(0x599a599a599a599a));
    qtest_writeb(s, DIRECTORY(PROTECTION_BANK1_HIGH), 0);
    qtest_writew(s, DIRECTORY(PROTECTION_BANK1_HIGH), 0);
    qtest_writel(s, DIRECTORY(PROTECTION_BANK1_HIGH), 0);
    qtest_writeq(s, DIRECTORY(PROTECTION_BANK1_HIGH + 1), 0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_BANK1_HIGH)),
                    ==, UINT64_C(0x599a599a599a599a));
    qtest_writeq(s, DIRECTORY(PROTECTION_FIRST_ABSENT), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_FIRST_ABSENT)),
                    ==, 0);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents, "not an aligned 64-bit carrier word"));
    g_assert_nonnull(strstr(contents, "outside fitted node memory"));
}

static void test_directory_fitted_memory(void)
{
    QTestState *s = start_with_memory("256M", "");

    qtest_writeq(s, MD(0x8), memory_bank_config(0, 3));
    qtest_writeq(s, DIRECTORY(DIRECTORY_LAST_256M), 0x2468ace0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LAST_256M)),
                    ==, 0x2468ace0);
    qtest_writeq(s, DIRECTORY(DIRECTORY_FIRST_ABSENT_256M), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_FIRST_ABSENT_256M)),
                    ==, 0);
    qtest_writeq(s, DIRECTORY(PROTECTION_LAST),
                 UINT64_C(0x599a599a599a599a));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_LAST)),
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
    qtest_writeq(s, DIRECTORY(PROTECTION_BANK1_HIGH),
                 UINT64_C(0x599a599a599a599a));
    g_assert_cmphex(qtest_readq(s, DIRECTORY(PROTECTION_BANK1_HIGH)),
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
                    ==, UINT64_C(0x599a599a599a599a));
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
    /* Start, stop, reset, bank and DIMM selections, and all bits at once. */
    static const uint64_t selftest_commands[] = { 1, 2, 4, 0x70, UINT64_MAX };
    QTestState *s = start("");

    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        qtest_writeq(s, MD(storage_registers[i].offset), UINT64_MAX);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(policy_zero_registers); i++) {
        qtest_writeq(s, MD(policy_zero_registers[i]), UINT64_MAX);
        g_assert_cmphex(qtest_readq(s, MD(policy_zero_registers[i])), ==, 0);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(selftest_commands); i++) {
        qtest_writeq(s, MD(0x20030), selftest_commands[i]);
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

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-md-migration-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = start("");
    to = start("-incoming defer");
    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        qtest_writeq(from, MD(storage_registers[i].offset),
                     UINT64_MAX ^ (UINT64_C(1) << (i + 2)));
    }
    qtest_writeq(from, MD(0), 0x55aa);
    qtest_writeq(from, MD(0x8), memory_bank_config(0, 4));
    qtest_writeq(from, MD(0x20000), UINT64_C(0x8000000000000c35));
    qtest_writeq(from, MD(0x40000), 3);
    qtest_writeq(from, DIRECTORY(DIRECTORY_LINE_01A00000),
                 UINT64_C(0x89abcdef01234567));
    qtest_writeq(from, DIRECTORY(DIRECTORY_LINE_01A00000 + 8),
                 UINT64_C(0xfedcba9876543210));
    qtest_writeq(from, DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18),
                 UINT64_C(0x0123456789abcdef));
    qtest_writeq(from, DIRECTORY(PROTECTION_BANK1_HIGH),
                 UINT64_C(0x599a599a599a599a));
    qtest_writeq(from, DIRECTORY(PROTECTION_LAST),
                 UINT64_C(0xa55aa55aa55aa55a));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    migrate_wait(from);
    migrate_wait(to);
    for (unsigned i = 0; i < ARRAY_SIZE(storage_registers); i++) {
        g_assert_cmphex(qtest_readq(to, MD(storage_registers[i].offset)), ==,
                        (UINT64_MAX ^ (UINT64_C(1) << (i + 2))) &
                        storage_registers[i].mask);
    }
    g_assert_cmphex(qtest_readq(to, MD(0)), ==, 0x55aa);
    g_assert_cmphex(qtest_readq(to, MD(0x8)), ==,
                    memory_bank_config(0, 4));
    g_assert_cmphex(qtest_readq(to, MD(0x20000)), ==,
                    UINT64_C(0x8000000000000c35));
    g_assert_cmphex(qtest_readq(to, MD(0x40000)), ==, 3);
    g_assert_cmphex(qtest_readq(to, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, UINT64_C(0x89abcdef01234567));
    g_assert_cmphex(qtest_readq(to, DIRECTORY(DIRECTORY_LINE_01A00000 + 8)),
                    ==, UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(to,
                               DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18)),
                    ==, UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(to, DIRECTORY(PROTECTION_BANK1_HIGH)),
                    ==, UINT64_C(0x599a599a599a599a));
    g_assert_cmphex(qtest_readq(to, DIRECTORY(PROTECTION_LAST)),
                    ==, UINT64_C(0xa55aa55aa55aa55a));
    qtest_writeq(to, MEMORY(0x23450), UINT64_C(0x13579bdf2468ace0));
    g_assert_cmphex(qtest_readq(to, MEMORY(0x23450)), ==,
                    UINT64_C(0x13579bdf2468ace0));
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

/*
 * One pass over the whole reviewed ECC contract: the aligned byte and
 * halfword stores the PROM posts, a read of each width, and one rejection
 * of each reason. Every pass adds exactly ECC_PASS_DIAGNOSTICS lines.
 */
#define ECC_PASS_DIAGNOSTICS 8

static void ecc_pass(QTestState *s)
{
    /* Accepted discard writes: first, interior and final of each width. */
    qtest_writeb(s, ECC(0), 0xff);
    qtest_writeb(s, ECC(7), 0xa5);
    qtest_writeb(s, ECC(ECC_LAST_DOUBLEWORD_512M), 0x5a);
    qtest_writeb(s, ECC(ECC_LAST_BYTE_512M), 0xc3);
    qtest_writew(s, ECC(0), 0xffff);
    qtest_writew(s, ECC(4), 0x1234);
    qtest_writew(s, ECC(6), 0x5678);
    qtest_writew(s, ECC(ECC_LAST_DOUBLEWORD_512M), 0x9abc);
    qtest_writew(s, ECC(ECC_LAST_HALFWORD_512M), 0xdef0);

    /* Reads answer zero and say the readback is unestablished. */
    g_assert_cmphex(qtest_readb(s, ECC(7)), ==, 0);
    g_assert_cmphex(qtest_readw(s, ECC(6)), ==, 0);
    g_assert_cmphex(qtest_readb(s, ECC(ECC_LAST_BYTE_512M)), ==, 0);
    g_assert_cmphex(qtest_readw(s, ECC(ECC_LAST_HALFWORD_512M)), ==, 0);

    /* One rejection of each reason, all four of them contract examples. */
    qtest_writew(s, ECC(7), 0xffff);
    qtest_writel(s, ECC(4), 0xffffffff);
    qtest_writeb(s, ECC(ECC_FIRST_ABSENT_BYTE_512M), 0xff);
    qtest_writew(s, ECC(ECC_FIRST_ABSENT_HALFWORD_512M), 0xffff);
}

static void ecc_assert_log(const char *contents, unsigned passes)
{
    g_assert_nonnull(strstr(contents, ECC_READ_UNESTABLISHED));
    g_assert_nonnull(strstr(contents, ECC_WRITE_UNALIGNED));
    g_assert_nonnull(strstr(contents, ECC_WRITE_WIDTH));
    g_assert_nonnull(strstr(contents, ECC_WRITE_ABSENT));
    g_assert_cmpuint(log_count(contents, ECC_DIAGNOSTIC), ==,
                     passes * ECC_PASS_DIAGNOSTICS);
}

static void test_ecc_carrier(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(ECC_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start(extra);

    ecc_pass(s);
    qtest_quit(s);

    contents = log_file_take(log);
    ecc_assert_log(contents, 1);
}

/*
 * Width first, alignment second, bounds last. Each access below breaks
 * more than one rule, so a model that checked them in another order would
 * name the wrong reason.
 */
static void test_ecc_validation_order(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(ECC_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start(extra);

    /* Too wide, unaligned and absent; then unaligned and absent. */
    qtest_writel(s, ECC(0x4000005), 0xffffffff);
    qtest_writew(s, ECC(0x4000007), 0xffff);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x04000005 size 4 "
        "rejected: unsupported width"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x04000007 size 2 "
        "rejected: unaligned halfword"));
    g_assert_cmpuint(log_count(contents, ECC_DIAGNOSTIC), ==, 2);
}

/* The extent follows fitted memory rather than a fixed 512 MiB node. */
static void test_ecc_fitted_memory(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(ECC_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start_with_memory("256M", extra);

    qtest_writeb(s, ECC(ECC_LAST_BYTE_256M), 0xa5);
    qtest_writeb(s, ECC(ECC_FIRST_ABSENT_BYTE_256M), 0xa5);
    qtest_writeb(s, ECC(ECC_LAST_BYTE_512M), 0xa5);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x02000007 size 1 "
        "rejected: outside fitted memory"));
    g_assert_nonnull(strstr(contents,
        "sgi-bedrock-md: ECC backdoor write offset 0x03ffffff size 1 "
        "rejected: outside fitted memory"));
    g_assert_cmpuint(log_count(contents, ECC_DIAGNOSTIC), ==, 2);
}

/* With nothing fitted the carrier answers nowhere. */
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

/*
 * The approximation holds no state, so a reset changes nothing about it.
 * The second pass has to behave exactly like the first.
 */
static void test_ecc_reset(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *extra = g_strdup_printf(ECC_LOG_OPTIONS "%s", log);
    g_autofree char *contents = NULL;
    QTestState *s = start(extra);

    ecc_pass(s);
    qtest_system_reset(s);
    ecc_pass(s);
    qtest_quit(s);

    contents = log_file_take(log);
    ecc_assert_log(contents, 2);
}

/* Nothing is migrated, so the destination answers like a fresh machine. */
static void test_ecc_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-md-ecc-XXXXXX", &error);
    g_autofree char *from_log = log_file_new();
    g_autofree char *to_log = log_file_new();
    g_autofree char *from_extra = NULL;
    g_autofree char *to_extra = NULL;
    g_autofree char *from_contents = NULL;
    g_autofree char *to_contents = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from_extra = g_strdup_printf(ECC_LOG_OPTIONS "%s", from_log);
    to_extra = g_strdup_printf(ECC_LOG_OPTIONS "%s -incoming defer", to_log);
    from = start(from_extra);
    to = start(to_extra);
    ecc_pass(from);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    migrate_wait(from);
    migrate_wait(to);
    ecc_pass(to);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);

    from_contents = log_file_take(from_log);
    to_contents = log_file_take(to_log);
    ecc_assert_log(from_contents, 1);
    ecc_assert_log(to_contents, 1);
}

/*
 * A machine maps what the node controller exports, not the units inside it,
 * so the carrier has to be reachable through the parent's own region list.
 */
static void test_parent_exports_ecc(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *contents = NULL;
    QTestState *s = qtest_initf("-M none -cpu R10000 " NODE_MEMORY_512M
                                "-object memory-backend-ram,id=dma,size=2M "
                                "-device sgi-bedrock,id=bedrock,"
                                "dma-memory=/objects/dma/dma[0],"
                                "node-memory=/objects/node-memory/"
                                "node-memory[0] -S "
                                ECC_LOG_OPTIONS "%s", log);

    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 6, ECC_BASE);
    qtest_writeb(s, ECC(7), 0xa5);
    g_assert_cmphex(qtest_readb(s, ECC(7)), ==, 0);
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents, ECC_READ_UNESTABLISHED));
    g_assert_cmpuint(log_count(contents, ECC_DIAGNOSTIC), ==, 1);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-md/register-contract", test_register_contract);
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
    qtest_add_func("sn1/bedrock-md/protection-storage",
                   test_protection_storage);
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
