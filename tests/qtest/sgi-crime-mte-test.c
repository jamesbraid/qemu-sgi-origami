/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "memtx.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"
#include "sgi-mips-emit.h"

#define BASE 0x14000000ULL
#define TLB 0x15001700ULL
#define MTE 0x15003000ULL
#define STATUS 0x15004000ULL
#define CLEAR_A (4U << 2)
#define COPY_AB ((1U << 11) | (4U << 5) | (5U << 2))

static const char *selected_profile = "1.5";

static QTestState *start_profile(bool functional, const char *extra)
{
    QTestState *s = qtest_initf("-device memory-transaction-test,id=memtx,"
        "migrate-ram=/objects/fit/fit[0] -M none -cpu R10000 -S -d guest_errors,unimp "
        "-object memory-backend-ram,id=fit,size=64M "
        "-device sgi-crime,id=crime,ram=/objects/fit/fit[0],"
        "crime-profile=%s,functional-memory-transfer=%s %s",
        selected_profile, functional ? "on" : "off", extra);

    qtest_sysbus_mmio_map(s, "/machine/peripheral/crime", 0, BASE);
    return s;
}

static QTestState *start_options(const char *extra)
{
    return start_profile(true, extra);
}

static QTestState *start(void)
{
    return start_options("");
}

static void page_pair(QTestState *s, unsigned bank, unsigned pair,
                      uint32_t first, uint32_t second)
{
    qtest_writeq(s, TLB + bank * 128 + pair * 8,
                 ((uint64_t)(0x80000000U | first) << 32) |
                 (0x80000000U | second));
}

static void clear(QTestState *s, uint32_t first, uint32_t last,
                  uint32_t fill)
{
    qtest_writel(s, MTE, CLEAR_A);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x18, fill);
    qtest_writel(s, MTE + 0x30, first);
    qtest_writel(s, MTE + 0x838, last);
}

static void assert_bytes(QTestState *s, uint64_t address, unsigned length,
                          uint8_t expected)
{
    g_autofree uint8_t *bytes = g_malloc(length);

    qtest_memread(s, address, bytes, length);
    for (unsigned i = 0; i < length; i++) {
        g_assert_cmphex(bytes[i], ==, expected);
    }
}

static void test_clear(void)
{
    QTestState *s = start();
    const unsigned lengths[] = { 1, 31, 33, 4113 };

    page_pair(s, 0, 0, 3, 9);
    g_assert_cmphex(qtest_readq(s, TLB), ==, 0x8000000380000009ULL);
    for (unsigned pattern = 0; pattern < 2; pattern++) {
        for (unsigned i = 0; i < ARRAY_SIZE(lengths); i++) {
            unsigned n = lengths[i];
            uint8_t fill = pattern ? 0xa7 : 0;

            qtest_memset(s, 0x3000, 0x5c, 4096);
            qtest_memset(s, 0x9000, 0x5c, 4096);
            clear(s, 7, 7 + n - 1, 0x12340000 | fill);
            assert_bytes(s, 0x3007, MIN(n, 4089), fill);
            if (n > 4089) {
                assert_bytes(s, 0x9000, n - 4089, fill);
            }
            g_assert_cmphex(qtest_readb(s, 0x3006), ==, 0x5c);
            g_assert_cmphex(qtest_readl(s, STATUS), ==, 0x1e000000);
        }
    }
    qtest_quit(s);
}

static void test_copy(void)
{
    QTestState *s = start();
    uint8_t input[4133], output[4133];

    for (unsigned i = 0; i < sizeof(input); i++) {
        input[i] = i * 17 + (i >> 8);
    }
    page_pair(s, 0, 0, 3, 9);
    page_pair(s, 1, 0, 11, 15);
    qtest_memwrite(s, 0x3007, input, 4089);
    qtest_memwrite(s, 0x9000, input + 4089, sizeof(input) - 4089);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x20, 7);
    qtest_writel(s, MTE + 0x28, 7 + sizeof(input) - 1);
    qtest_writel(s, MTE + 0x30, 13);
    qtest_writel(s, MTE + 0x38, 13 + sizeof(input) - 1);
    qtest_writel(s, MTE + 0x800, COPY_AB);
    qtest_memread(s, 0xb00d, output, 4083);
    qtest_memread(s, 0xf000, output + 4083, sizeof(output) - 4083);
    g_assert_cmpmem(output, sizeof(output), input, sizeof(input));
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 0);
    qtest_quit(s);
}

static void test_invalid_translation(void)
{
    QTestState *s = start();
    QDict *status;

    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    qtest_memset(s, 0, 0x9b, 64);
    qtest_memset(s, 0x3000, 0x6a, 64);
    qtest_writel(s, MTE, COPY_AB);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x28, 63);
    qtest_writel(s, MTE + 0x38, 42);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 63), !=, MEMTX_OK);
    status = qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");
    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "internal-error");
    qobject_unref(status);
    assert_bytes(s, 0, 64, 0x9b);
    assert_bytes(s, 0x3000, 64, 0x6a);
    g_assert_cmphex(qtest_readl(s, MTE), ==, COPY_AB);
    page_pair(s, 0, 0, 3, 9);
    qtest_writel(s, MTE + 0x800, CLEAR_A);
    assert_bytes(s, 0x3000, 43, 0);
    assert_bytes(s, 0x302b, 21, 0x6a);
    qtest_quit(s);
}

static void test_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("crime-mte-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = start(), *to = start_options("-incoming defer");
    const unsigned readable[] = { 0, 8, 0x18, 0x20, 0x28, 0x40, 0x48 };

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    page_pair(from, 0, 0, 3, 9);
    page_pair(from, 1, 0, 11, 15);
    for (unsigned i = 0; i < ARRAY_SIZE(readable); i++) {
        qtest_writel(from, MTE + readable[i], 0x12345600 + i);
    }
    qtest_writeq(from, BASE + 0x200, 1);
    qtest_writel(from, MTE + 0x30, 7);
    qtest_writel(from, MTE + 0x38, 39);
    qtest_memset(from, 0x3000, 0xa5, 4096);
    qtest_memset(from, 0xb000, 0xb6, 4096);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    for (unsigned i = 0; i < ARRAY_SIZE(readable); i++) {
        g_assert_cmphex(qtest_readl(to, MTE + readable[i]), ==, 0x12345600 + i);
    }
    g_assert_cmphex(qtest_readq(to, TLB), ==, 0x8000000380000009ULL);
    g_assert_cmphex(qtest_readq(to, TLB + 128), ==, 0x8000000b8000000fULL);
    g_assert_cmphex(qtest_readq(to, BASE + 0x200), ==, 1);
    assert_bytes(to, 0x3000, 4096, 0xa5);
    assert_bytes(to, 0xb000, 4096, 0xb6);
    qtest_writel(to, MTE + 8, UINT32_MAX);
    qtest_writel(to, MTE + 0x18, 0x7c);
    qtest_writel(to, MTE + 0x800, CLEAR_A);
    assert_bytes(to, 0x3007, 33, 0x7c);
    clear(to, 3, 33, 0x7c);
    assert_bytes(to, 0x3003, 31, 0x7c);
    qtest_writel(to, MTE + 0x30, 9);
    qtest_writel(to, MTE + 0x38, 39);
    qtest_writel(to, MTE + 0x800, 5U << 2);
    assert_bytes(to, 0xb009, 31, 0x7c);
    qtest_system_reset(to);
    g_assert_cmphex(qtest_readq(to, TLB), ==, 0);
    g_assert_cmphex(qtest_readq(to, TLB + 128), ==, 0);
    g_assert_cmphex(qtest_readq(to, BASE + 0x200), ==, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(readable); i++) {
        g_assert_cmphex(qtest_readl(to, MTE + readable[i]), ==, 0);
    }
    qtest_quit(from);
    qtest_quit(to);
    g_rmdir(directory);
}

static void assert_stopped(QTestState *s)
{
    g_autoptr(QDict) status =
        qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");

    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "internal-error");
}

static void test_registers(void)
{
    QTestState *s = start();
    const unsigned readable[] = { 0, 8, 0x18, 0x20, 0x28, 0x40, 0x48 };
    const unsigned unsupported[] = { 0x10, 0x50, 0x70, 0x78, 0x808, 0x830 };
    const unsigned write_only[] = { 0x30, 0x38, 0x800, 0x838 };
    uint8_t b;
    uint16_t h;
    uint32_t w;
    uint64_t d;

    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    for (unsigned i = 0; i < ARRAY_SIZE(readable); i++) {
        uint64_t reg = MTE + readable[i];

        g_assert_cmphex(qtest_readl(s, reg), ==, 0);
        qtest_writel(s, reg, 0x91234500 + i);
        g_assert_cmphex(qtest_readl(s, reg), ==, 0x91234500 + i);
        g_assert_cmpint(memtx_readb(s, reg, &b), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readw(s, reg, &h), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readq(s, reg, &d), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readl(s, reg + 1, &w), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writeb(s, reg, 0), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writew(s, reg, 0), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writeq(s, reg, 0), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writel(s, reg + 1, 0), !=, MEMTX_OK);
        g_assert_cmphex(qtest_readl(s, reg), ==, 0x91234500 + i);
    }
    for (unsigned i = 0; i < 32; i++) {
        uint64_t reg = TLB + 8 * i;

        g_assert_cmphex(qtest_readq(s, reg), ==, 0);
        g_assert_cmpint(memtx_readb(s, reg, &b), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readw(s, reg, &h), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readl(s, reg, &w), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readq(s, reg + 1, &d), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writeb(s, reg, 0), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writew(s, reg, 0), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writeq(s, reg + 1, 0), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writel(s, reg, 0), !=, MEMTX_OK);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(unsupported); i++) {
        g_assert_cmpint(memtx_readl(s, MTE + unsupported[i], &w), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writel(s, MTE + unsupported[i], 0), !=, MEMTX_OK);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(write_only); i++) {
        uint64_t reg = MTE + write_only[i];

        g_assert_cmpint(memtx_readb(s, reg, &b), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readw(s, reg, &h), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readq(s, reg, &d), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writeb(s, reg, 0), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writew(s, reg, 0), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writeq(s, reg, 0), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writel(s, reg + 1, 0), !=, MEMTX_OK);
    }
    g_assert_cmpint(memtx_readl(s, MTE + 0x30, &w), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readl(s, MTE + 0x38, &w), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readl(s, MTE + 0x800, &w), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readl(s, MTE + 0x838, &w), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, MTE + 0x800, 0), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, MTE + 0x838, 0), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readb(s, STATUS, &b), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readw(s, STATUS, &h), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readq(s, STATUS, &d), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readl(s, STATUS + 1, &w), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writel(s, STATUS, 0), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readl(s, STATUS + 4, &w), !=, MEMTX_OK);
    assert_stopped(s);
    qtest_quit(s);
}

static void test_translation_and_commit(void)
{
    QTestState *s = start();
    const uint64_t aliases[] = { 0, 0x40000000, 0x80000000 };

    page_pair(s, 0, 0, 3, 9);
    page_pair(s, 0, 15, 13, 17);
    page_pair(s, 1, 0, 23, 29);
    qtest_memset(s, 0x3000, 0x6c, 4096);
    qtest_writel(s, MTE, CLEAR_A | 0xe2); /* Clear ignores source fields. */
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x18, 0x12345678);
    qtest_writel(s, MTE + 0x20, UINT32_MAX);
    qtest_writel(s, MTE + 0x28, 0);
    qtest_writel(s, MTE + 0x30, 4095);
    qtest_writel(s, MTE + 0x38, 4096);
    qtest_writel(s, MTE + 0x40, UINT32_MAX);
    qtest_writel(s, MTE + 0x48, UINT32_MAX);
    assert_bytes(s, 0x3000, 4096, 0x6c);
    qtest_writel(s, MTE + 0x800, CLEAR_A | 0xe2);
    for (unsigned i = 0; i < ARRAY_SIZE(aliases); i++) {
        g_assert_cmphex(qtest_readb(s, aliases[i] + 0x3fff), ==, 0x78);
        g_assert_cmphex(qtest_readb(s, aliases[i] + 0x9000), ==, 0x78);
        g_assert_cmphex(qtest_readb(s, aliases[i] + 0x3ffe), ==, 0x6c);
    }
    qtest_writel(s, MTE + 0x18, 0xa5);
    g_assert_cmphex(qtest_readb(s, 0x3fff), ==, 0x78);
    clear(s, 0x1ffff, 0x20000, 0xd1);
    g_assert_cmphex(qtest_readb(s, 0x11fff), ==, 0xd1);
    g_assert_cmphex(qtest_readb(s, 0x3000), ==, 0xd1);
    page_pair(s, 0, 0, 19, 9);
    clear(s, 0, 0, 0x5a);
    g_assert_cmphex(qtest_readb(s, 0x13000), ==, 0x5a);
    g_assert_cmphex(qtest_readb(s, 0x3000), ==, 0xd1);
    qtest_writel(s, MTE + 0x30, 5);
    qtest_writel(s, MTE + 0x38, 35);
    qtest_writel(s, MTE + 0x800, 5U << 2);
    assert_bytes(s, 0x17005, 31, 0x5a);
    qtest_quit(s);
}

static void test_rejected_commands(void)
{
    QTestState *s = start();
    const uint32_t bad_mode[] = {
        1U << 31, 1U << 12, 1U << 10, 1U << 9, 1U << 8, 0,
        (1U << 11) | CLEAR_A,
    };

    page_pair(s, 0, 0, 3, 9);
    qtest_memset(s, 0x3000, 0x6c, 4096);
    qtest_writel(s, MTE, CLEAR_A);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x30, 0);
    qtest_writel(s, MTE + 0x38, 31);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    for (unsigned i = 0; i < ARRAY_SIZE(bad_mode); i++) {
        g_assert_cmpint(memtx_writel(s, MTE + 0x800, bad_mode[i]),
                        !=, MEMTX_OK);
        g_assert_cmphex(qtest_readl(s, MTE), ==, CLEAR_A);
        assert_bytes(s, 0x3000, 4096, 0x6c);
        assert_stopped(s);
    }
    page_pair(s, 1, 0, 11, 15);
    qtest_writel(s, MTE + 0x20, 0);
    qtest_writel(s, MTE + 0x28, 30);
    g_assert_cmpint(memtx_writel(s, MTE + 0x800,
        (1U << 11) | (4U << 5) | (5U << 2)), !=, MEMTX_OK);
    qtest_writel(s, MTE + 8, 0xfffffffe);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 31), !=, MEMTX_OK);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x30, 32);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 31), !=, MEMTX_OK);
    qtest_writel(s, MTE + 0x30, 1);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 0x20000), !=, MEMTX_OK);
    qtest_writel(s, MTE + 0x30, 0);
    qtest_writeq(s, TLB, 0x8000000380004000ULL);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 4096), !=, MEMTX_OK);
    assert_bytes(s, 0x3000, 4096, 0x6c);
    page_pair(s, 0, 0, 3, 9);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t raw = 0x8000000380000009ULL | (1ULL << (i * 32 + 20));

        g_assert_cmpint(memtx_writeq(s, TLB, raw), ==, MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, TLB), ==, raw);
    }
    qtest_writeq(s, BASE + 0x18, 1U << 4);
    qtest_set_irq_in(s, "/machine/peripheral/crime", "mace", 4, 1);
    for (unsigned i = 22; i <= 27; i++) {
        g_assert_cmpint(memtx_writeq(s, BASE + 0x18, (1U << i) | 8),
                        !=, MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, 1U << 4);
        g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1U << 4);
    }
    qtest_quit(s);
}

static void test_overlap(void)
{
    QTestState *s = start();

    page_pair(s, 0, 0, 3, 9);
    page_pair(s, 1, 0, 0x40003, 0x80009);
    qtest_memset(s, 0x3000, 0x6c, 4096);
    qtest_writel(s, MTE, COPY_AB);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x20, 7);
    qtest_writel(s, MTE + 0x28, 39);
    qtest_writel(s, MTE + 0x30, 0);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 32), !=, MEMTX_OK);
    qtest_writel(s, MTE + 0x30, 14);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 46), !=, MEMTX_OK);
    assert_bytes(s, 0x3000, 4096, 0x6c);
    page_pair(s, 0, 0, 3, 3);
    qtest_writel(s, MTE + 0x30, 0);
    qtest_writel(s, MTE + 0x38, 4096);
    g_assert_cmpint(memtx_writel(s, MTE + 0x800, CLEAR_A), !=, MEMTX_OK);
    assert_bytes(s, 0x3000, 4096, 0x6c);
    page_pair(s, 0, 0, 3, 9);
    page_pair(s, 1, 0, 11, 11);
    qtest_memset(s, 0xb000, 0x82, 4096);
    qtest_writel(s, MTE + 0x20, 0);
    qtest_writel(s, MTE + 0x28, 4096);
    g_assert_cmpint(memtx_writel(s, MTE + 0x800, COPY_AB), !=, MEMTX_OK);
    assert_bytes(s, 0xb000, 4096, 0x82);
    assert_stopped(s);
    qtest_quit(s);
}

static QTestState *start_banked(bool approximation)
{
    QTestState *s = qtest_initf(MEMTX_TEST_DEVICE
        "-M none -cpu R10000 -S -d guest_errors,unimp "
        "-object memory-backend-ram,id=fit,size=1G "
        "-device sgi-crime,id=crime,ram=/objects/fit/fit[0],"
        "eight-128m-banks=on,crime-profile=%s,functional-memory-transfer=on%s",
        selected_profile, approximation ? ",reset-low-ram-approx=on" : "");

    qtest_sysbus_mmio_map(s, "/machine/peripheral/crime", 0, BASE);
    return s;
}

static void test_healthy_memory(void)
{
    QTestState *s = start_banked(false);
    uint64_t value;

    /* Ordinary misses establish decoder latches before transfer validation. */
    qtest_writeq(s, BASE + 0x208, 0x104);
    g_assert_cmpint(memtx_writeb(s, 0x40012345, 0x77), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, BASE + 0x250), ==, 1U << 26);
    g_assert_cmphex(qtest_readq(s, BASE + 0x258), ==, 0x12345);
    qtest_writeq(s, BASE + 0x200, 1);
    g_assert_cmphex(qtest_readq(s, BASE + 0x200), ==, 1);
    qtest_writeq(s, BASE + 0x250, 0);
    qtest_writeq(s, BASE + 0x270, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x260), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x270), ==, 0);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    g_assert_cmpint(memtx_writeq(s, BASE + 0x200, 2), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, BASE + 0x200), ==, 1);
    g_assert_cmpint(memtx_writeq(s, BASE + 0x250, 1), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, BASE + 0x258, 0), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, BASE + 0x260, 0), !=, MEMTX_OK);
    g_assert_cmpint(memtx_readq(s, BASE + 0x268, &value), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, BASE + 0x268, 0), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, BASE + 0x270, 1), !=, MEMTX_OK);
    page_pair(s, 0, 0, 3, 9);
    qtest_writel(s, MTE, CLEAR_A);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 31), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, BASE + 0x250), ==, 1U << 26);
    g_assert_cmphex(qtest_readq(s, BASE + 0x258), ==, 0x12345);
    assert_stopped(s);
    qtest_quit(s);
}

static void test_banked_views(void)
{
    QTestState *s = start_banked(true);
    const uint32_t pages[] = { 0, 0x40000, 0x80000, 0x20000, 0x60000, 0xa0000 };
    const uint64_t views[] = { 0x20000000, 0x60000000, 0xa0000000 };

    /* Transfer validation must retain the temporary reset response. */
    page_pair(s, 0, 0, 0, 1);
    clear(s, 0, 32, 0x5b);
    assert_bytes(s, 0, 33, 0x5b);
    g_assert_cmphex(qtest_readq(s, BASE + 0x208), ==, 0);
    qtest_writeq(s, BASE + 0x208, 0x100);
    qtest_writeq(s, BASE + 0x228, 0x110);
    for (unsigned i = 0; i < ARRAY_SIZE(pages); i++) {
        page_pair(s, 0, 0, pages[i], pages[i] + 1);
        clear(s, 4095, 4127, 0x81 + i);
        uint64_t physical = (uint64_t)pages[i] << 12;

        assert_bytes(s, physical + 4095, 33, 0x81 + i);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(views); i++) {
        assert_bytes(s, views[i] + 4095, 33, 0x86);
    }
    qtest_writeq(s, BASE + 0x208, 0x104);
    page_pair(s, 0, 0, 0x8000, 0x8001);
    clear(s, 0, 32, 0x93);
    assert_bytes(s, 0x08000000, 33, 0x93);
    qtest_writeq(s, BASE + 0x208, 0x100);
    assert_bytes(s, 0, 33, 0x93);
    qtest_quit(s);
}

static void test_bounds_and_holes(void)
{
    QTestState *s = start();
    const uint32_t bad_pages[] = { 0x10000, 0xc0000, 0xfffff, 0x4000 };

    qtest_memset(s, 0x3000, 0x6c, 4096);
    qtest_writel(s, MTE, CLEAR_A);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    for (unsigned i = 0; i < ARRAY_SIZE(bad_pages); i++) {
        page_pair(s, 0, 0, 3, bad_pages[i]);
        g_assert_cmpint(memtx_writel(s, MTE + 0x838, 4096), !=, MEMTX_OK);
        assert_bytes(s, 0x3000, 4096, 0x6c);
    }
    page_pair(s, 0, 15, 3, 0xfffff);
    qtest_writel(s, MTE + 0x30, UINT32_MAX);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, UINT32_MAX), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 0), !=, MEMTX_OK);
    page_pair(s, 0, 15, 3, 9);
    qtest_writel(s, MTE + 0x18, 0x82);
    qtest_writel(s, MTE + 0x838, UINT32_MAX);
    g_assert_cmphex(qtest_readb(s, 0x9fff), ==, 0x82);
    assert_stopped(s);
    qtest_quit(s);
}

static void test_maximum_interval(void)
{
    QTestState *s = start();

    for (unsigned i = 0; i < 16; i++) {
        page_pair(s, 0, i, 64 + i * 2, 65 + i * 2);
    }
    qtest_memset(s, 0x40000, 0x5a, 0x20001);
    clear(s, 1, 0x1ffff, 0xb3);
    assert_bytes(s, 0x40001, 0x1ffff, 0xb3);
    g_assert_cmphex(qtest_readb(s, 0x40000), ==, 0x5a);
    g_assert_cmphex(qtest_readb(s, 0x60000), ==, 0x5a);
    qtest_quit(s);
}

static void test_source_alias(void)
{
    QTestState *s = start();

    page_pair(s, 0, 0, 3, 0x40003);
    page_pair(s, 1, 0, 11, 15);
    qtest_memset(s, 0x3000, 0x6c, 4096);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x20, 0);
    qtest_writel(s, MTE + 0x28, 8191);
    qtest_writel(s, MTE + 0x30, 0);
    qtest_writel(s, MTE + 0x38, 8191);
    qtest_writel(s, MTE + 0x800, COPY_AB | 3);
    assert_bytes(s, 0xb000, 4096, 0x6c);
    assert_bytes(s, 0xf000, 4096, 0x6c);
    qtest_quit(s);
}

static void wait_migration_failed(QTestState *s)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    for (;;) {
        g_autoptr(QDict) info =
            qtest_qmp_assert_success_ref(s, "{'execute':'query-migrate'}");

        if (g_str_equal(qdict_get_str(info, "status"), "failed")) {
            g_assert_true(qdict_haskey(info, "error-desc"));
            return;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void test_migration_policy(gconstpointer data)
{
    bool source_functional = GPOINTER_TO_INT(data);
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("crime-mte-policy-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = start_profile(source_functional, "");
    QTestState *to = start_profile(!source_functional, "-incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s,'exit-on-error':false}}",
        uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration_failed(to);
    qtest_quit(from);
    qtest_quit(to);
    g_rmdir(directory);
}

static void test_migration_invalid_state(gconstpointer data)
{
    unsigned fault = GPOINTER_TO_UINT(data);
    const char subsection[] = "sgi-crime/linear-transfer";
    const char section[] = "sgi-crime";
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("crime-mte-invalid-XXXXXX", &error);
    g_autofree char *path = NULL;
    g_autofree char *quoted = NULL;
    g_autofree char *outgoing = NULL;
    g_autofree char *incoming = NULL;
    g_autofree char *stream = NULL;
    gsize size;
    char *state;
    bool functional = fault != 5;
    QTestState *from = start_profile(functional, "");
    QTestState *to = start_profile(functional, "-incoming defer");

    g_assert_no_error(error);
    path = g_build_filename(directory, "synthetic.migration", NULL);
    quoted = g_shell_quote(path);
    outgoing = g_strdup_printf("exec:cat > %s", quoted);
    incoming = g_strdup_printf("exec:cat %s", quoted);
    if (functional) {
        page_pair(from, 0, 0, 3, 9);
        page_pair(to, 0, 0, 11, 15);
    }
    qtest_writeq(to, BASE + 0x18, 1U << 4);
    sgi_migrate_stopped(from, outgoing);
    g_assert_true(g_file_get_contents(path, &stream, &size, &error));
    g_assert_no_error(error);
    if (fault == 0) {
        state = memmem(stream, size, section, sizeof(section) - 1);
        g_assert_nonnull(state);
        state += sizeof(section) - 1;
        /* Main section: instance, version, hardware, then interrupt enable. */
        g_assert_cmphex(ldl_be_p(state + 4), ==, 2);
        stl_be_p(state + 12, 1U << 22);
    } else {
        state = memmem(stream, size, subsection, sizeof(subsection) - 1);
        g_assert_nonnull(state);
        state += sizeof(subsection) - 1 + 4;
        g_assert_cmphex((uint8_t)state[0], ==, functional ? 1 : 0);
        switch (fault) {
        case 1: /* Policy identity precedes the saved programming. */
            state[0] = 2;
            break;
        case 2:
            stl_be_p(state + 2 + 256 + 8, 1);
            break;
        case 3:
            stq_be_p(state + 2 + 256 + 40, 2);
            break;
        case 4:
        case 5: {
            char *first = state - 4 - (sizeof(subsection) - 1) - 2;
            size_t length = 2 + sizeof(subsection) - 1 + 4 + 314;

            memmove(first, first + length, size - (first - stream) - length);
            size -= length;
            break;
        }
        default:
            g_assert_not_reached();
        }
    }
    g_assert_true(g_file_set_contents(path, stream, size, &error));
    g_assert_no_error(error);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s,'exit-on-error':false}}",
        incoming);
    wait_migration_failed(to);
    if (functional) {
        g_assert_cmphex(qtest_readq(to, TLB), ==, 0x8000000b8000000fULL);
    }
    g_assert_cmphex(qtest_readq(to, BASE + 0x18), ==, 1U << 4);
    qtest_quit(from);
    qtest_quit(to);
    g_unlink(path);
    g_rmdir(directory);
}

static void test_diagnostics(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("crime-mte-log-XXXXXX", &error);
    g_autofree char *path = NULL;
    g_autofree char *options = NULL;
    g_autofree char *contents = NULL;
    const char *notice;
    QTestState *s;

    g_assert_no_error(error);
    path = g_build_filename(directory, "diagnostics.txt", NULL);
    options = g_strdup_printf("-d guest_errors,unimp -D %s", path);
    s = start_options(options);
    qtest_writeq(s, BASE + 0x200, 1);
    qtest_writeq(s, BASE + 0x200, 0);
    qtest_writeq(s, BASE + 0x200, 1);
    qtest_writel(s, MTE, COPY_AB | 1);
    qtest_writel(s, MTE + 0x28, 31);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 31), !=, MEMTX_OK);
    assert_stopped(s);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    notice = strstr(contents, "healthy-memory approximation:");
    g_assert_nonnull(notice);
    g_assert_null(strstr(notice + 1, "healthy-memory approximation:"));
    g_assert_nonnull(strstr(contents,
        "0x15003838: invalid destination translation or bounds"));
    g_unlink(path);
    g_rmdir(directory);
}

static void test_translated_code(void)
{
    bool legacy = !strcmp(selected_profile, "legacy-software");
    QTestState *s = qtest_initf(MEMTX_TEST_DEVICE
        "-M o2-test,crime-profile=%s,functional-memory-transfer=on%s "
        "-cpu R10000 -m %s -nodefaults -accel tcg,thread=single -S "
        "-device loader,addr=0xffffffffa0001000,cpu-num=0", selected_profile,
        legacy ? ",bank-profile=8x128m-synthetic,reset-low-ram-approx=on" : "",
        legacy ? "1G" : "64M");
    int64_t deadline;

    qtest_writel(s, 0x1000, mips_lui(8, 0xa000));
    qtest_writel(s, 0x1004, mips_addiu(9, 0, 0x11));
    qtest_writel(s, 0x1008, mips_sw(9, 8, 0x200));
    qtest_writel(s, 0x100c, mips_branch(MIPS_OPC_BEQ, 0, 0, -3));
    qtest_writel(s, 0x1010, 0);
    qtest_writel(s, 0x3000, mips_addiu(9, 0, 0x22));
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (qtest_readl(s, 0x200) != 0x11) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_qmp_assert_success(s, "{'execute':'stop'}");
    page_pair(s, 0, 0, 3, 9);
    page_pair(s, 1, 0, 1, 15);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x20, 0);
    qtest_writel(s, MTE + 0x28, 3);
    qtest_writel(s, MTE + 0x30, 4);
    qtest_writel(s, MTE + 0x38, 7);
    qtest_writel(s, MTE + 0x800, COPY_AB);
    g_assert_cmphex(qtest_readl(s, 0x1004), ==, mips_addiu(9, 0, 0x22));
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (qtest_readl(s, 0x200) != 0x22) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_quit(s);
}

typedef struct TransferCase {
    const char *name;
    GTestFunc function;
    GTestDataFunc data_function;
    gconstpointer data;
    const char *profile;
} TransferCase;

static void run_transfer_case(gconstpointer data)
{
    const TransferCase *test = data;

    selected_profile = test->profile;
    if (test->function) {
        test->function();
    } else {
        test->data_function(test->data);
    }
    selected_profile = "1.5";
}

int main(int argc, char **argv)
{
    static const TransferCase tests[] = {
        { "clear", test_clear, NULL, NULL, NULL },
        { "copy", test_copy, NULL, NULL, NULL },
        { "invalid-translation", test_invalid_translation, NULL, NULL, NULL },
        { "registers", test_registers, NULL, NULL, NULL },
        { "translation-commit", test_translation_and_commit, NULL, NULL, NULL },
        { "rejected-commands", test_rejected_commands, NULL, NULL, NULL },
        { "overlap", test_overlap, NULL, NULL, NULL },
        { "healthy-memory", test_healthy_memory, NULL, NULL, NULL },
        { "banked-views", test_banked_views, NULL, NULL, NULL },
        { "bounds-holes", test_bounds_and_holes, NULL, NULL, NULL },
        { "source-alias", test_source_alias, NULL, NULL, NULL },
        { "maximum-interval", test_maximum_interval, NULL, NULL, NULL },
        { "migration", test_migration, NULL, NULL, NULL },
        { "diagnostics", test_diagnostics, NULL, NULL, NULL },
        { "translated-code", test_translated_code, NULL, NULL, NULL },
        { "migration-missing-state", NULL,
          test_migration_policy, GINT_TO_POINTER(0), NULL },
        { "migration-unselected-state", NULL,
          test_migration_policy, GINT_TO_POINTER(1), NULL },
        { "migration-rendering-enable", NULL,
          test_migration_invalid_state, GUINT_TO_POINTER(0), NULL },
        { "migration-policy", NULL,
          test_migration_invalid_state, GUINT_TO_POINTER(1), NULL },
        { "migration-unadmitted-parameter", NULL,
          test_migration_invalid_state, GUINT_TO_POINTER(2), NULL },
        { "migration-ecc-enable", NULL,
          test_migration_invalid_state, GUINT_TO_POINTER(3), NULL },
        { "migration-absent-section", NULL,
          test_migration_invalid_state, GUINT_TO_POINTER(4), NULL },
        { "migration-absent-disabled-section", NULL,
          test_migration_invalid_state, GUINT_TO_POINTER(5), NULL },
    };
    const char *profiles[] = { "1.5", "legacy-software" };

    g_test_init(&argc, &argv, NULL);
    for (unsigned profile = 0; profile < ARRAY_SIZE(profiles); profile++) {
        for (unsigned i = 0; i < ARRAY_SIZE(tests); i++) {
            TransferCase *test = g_new(TransferCase, 1);
            g_autofree char *name = g_strdup_printf("/sgi/crime/mte/%s%s",
                profile ? "legacy/" : "", tests[i].name);

            *test = tests[i];
            test->profile = profiles[profile];
            qtest_add_data_func_full(name, test, run_transfer_case, g_free);
        }
    }
    return g_test_run();
}
