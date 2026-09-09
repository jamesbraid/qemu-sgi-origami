/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define BASE UINT64_C(0x78000000)
#define MD(_x) (BASE + (_x))
#define MD_PATH "/machine/peripheral/md"
#define DIRECTORY_BASE UINT64_C(0x100000000)
#define DIRECTORY(_x) (DIRECTORY_BASE + (_x))
#define DIRECTORY_LINE_01A00000 0x0340000
#define DIRECTORY_OTHER_LINE 0x0123450
#define DIRECTORY_LAST_512M 0x3fffff0
#define DIRECTORY_FIRST_ABSENT_512M 0x4000000
#define DIRECTORY_LAST_256M 0x1fffff0
#define DIRECTORY_FIRST_ABSENT_256M 0x2000000
#define NODE_MEMORY_512M \
    "-object memory-backend-ram,id=node-memory,size=512M "

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

    qtest_sysbus_mmio_map(s, "/machine/peripheral/bedrock", 5,
                          DIRECTORY_BASE);
    return s;
}

static char *log_file_new(void)
{
    GError *error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("sn1-md-directory-log-XXXXXX", &path, &error);

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
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000), 0x76543210);
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 8),
                 UINT64_C(0x0123456789abcdef));
    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18),
                 UINT64_C(0xfedcba9876543210));
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
    qtest_quit(s);
}

static void test_directory_storage(void)
{
    QTestState *s = start("");

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
    qtest_quit(s);

    contents = log_file_take(log);
    g_assert_nonnull(strstr(contents, "not an aligned 64-bit carrier word"));
    g_assert_nonnull(strstr(contents, "outside fitted node memory"));
}

static void test_directory_fitted_memory(void)
{
    QTestState *s = start_with_memory("256M", "");

    qtest_writeq(s, DIRECTORY(DIRECTORY_LAST_256M), 0x2468ace0);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LAST_256M)),
                    ==, 0x2468ace0);
    qtest_writeq(s, DIRECTORY(DIRECTORY_FIRST_ABSENT_256M), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_FIRST_ABSENT_256M)),
                    ==, 0);
    qtest_quit(s);
}

static void test_parent_exports_directory(void)
{
    QTestState *s = start_parent();

    qtest_writeq(s, DIRECTORY(DIRECTORY_LINE_01A00000), 0x13579bdf);
    g_assert_cmphex(qtest_readq(s, DIRECTORY(DIRECTORY_LINE_01A00000)),
                    ==, 0x13579bdf);
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
    qtest_writeq(from, MD(0x8), UINT64_MAX);
    qtest_writeq(from, MD(0x20000), UINT64_C(0x8000000000000c35));
    qtest_writeq(from, MD(0x40000), 3);
    qtest_writeq(from, DIRECTORY(DIRECTORY_LINE_01A00000),
                 UINT64_C(0x89abcdef01234567));
    qtest_writeq(from, DIRECTORY(DIRECTORY_LINE_01A00000 + 8),
                 UINT64_C(0xfedcba9876543210));
    qtest_writeq(from, DIRECTORY(DIRECTORY_LINE_01A00000 + 0x18),
                 UINT64_C(0x0123456789abcdef));
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
    g_assert_cmphex(qtest_readq(to, MD(0x8)), ==, MEMORY_CONFIG_MASK);
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
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock-md/register-contract", test_register_contract);
    qtest_add_func("sn1/bedrock-md/access-width-policy",
                   test_access_width_policy);
    qtest_add_func("sn1/bedrock-md/directory-storage",
                   test_directory_storage);
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
    return g_test_run();
}
