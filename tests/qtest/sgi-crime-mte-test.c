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
    qtest_writeq(from, BASE + 0x18, UINT32_MAX);
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
    g_assert_cmphex(qtest_readq(to, BASE + 0x18), ==, UINT32_MAX);
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
        if (!strcmp(selected_profile, "legacy-software") &&
            (unsupported[i] == 0x10 || unsupported[i] == 0x70 ||
             unsupported[i] == 0x78)) {
            g_assert_cmpint(memtx_writel(s, MTE + unsupported[i], 0),
                            ==, MEMTX_OK);
        } else {
            g_assert_cmpint(memtx_writel(s, MTE + unsupported[i], 0),
                            !=, MEMTX_OK);
        }
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
    for (unsigned i = 0; i < 32; i++) {
        uint32_t mask = (1U << i) | (1U << 4);

        g_assert_cmpint(memtx_writeq(s, BASE + 0x18, mask), ==, MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, mask);
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
    g_assert_cmpint(memtx_store(s, BASE + 0x270, 0xa5, 8), ==, MEMTX_OK);
    g_assert_cmpint(memtx_store(s, BASE + 0x270, 0x100, 8), !=, MEMTX_OK);
    g_assert_cmpint(memtx_load(s, BASE + 0x270, &value, 8), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0xa5);
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
    /* The legacy mask slot is valid only with functional transfer enabled. */
    bool functional = fault != 5 &&
        !(fault == 2 && !strcmp(selected_profile, "legacy-software"));
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
        /* Software-only positions cannot appear in saved hardware state. */
        g_assert_cmphex(ldl_be_p(state + 4), ==, 2);
        stl_be_p(state + 8, 1U << 28);
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
            size_t length = 2 + sizeof(subsection) - 1 + 4 + 346;

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

#define CLIP_TLB 0x150016e0ULL
#define CLIP_MODE (6U << 2)
#define CLIP_STATE_BYTES 346
#define CLIP_STATE_OFFSET 314

static void clip_entries(uint16_t entries[16])
{
    for (unsigned i = 0; i < 16; i++) {
        entries[i] = 0x8000 | (3 + i * 6);
    }
}

static void clip_program(QTestState *s, const uint16_t entries[16])
{
    for (unsigned word = 0; word < 4; word++) {
        uint64_t value = 0;

        for (unsigned lane = 0; lane < 4; lane++) {
            value = (value << 16) | entries[word * 4 + lane];
        }
        g_assert_cmpint(memtx_writeq(s, CLIP_TLB + word * 8, value), ==,
                        MEMTX_OK);
    }
}

static void clip_setup(QTestState *s, unsigned x0, unsigned y0,
                        unsigned x1, unsigned y1, uint32_t fill)
{
    qtest_writel(s, MTE, CLIP_MODE | (7U << 5));
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x18, fill);
    qtest_writel(s, MTE + 0x20, UINT32_MAX);
    qtest_writel(s, MTE + 0x28, 0x87654321);
    qtest_writel(s, MTE + 0x30, (x0 << 16) | y0);
    qtest_writel(s, MTE + 0x38, (x1 << 16) | y1);
    qtest_writel(s, MTE + 0x40, UINT32_MAX);
    qtest_writel(s, MTE + 0x48, 1);
}

static void clip_save(QTestState *s, const char *path,
                       uint8_t bytes[CLIP_STATE_BYTES])
{
    const char section[] = "sgi-crime/linear-transfer";
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *uri = g_strdup_printf("exec:cat > %s", quoted);
    g_autofree char *stream = NULL;
    gsize length;
    const char *state;

    sgi_migrate_stopped(s, uri);
    g_assert_true(g_file_get_contents(path, &stream, &length, NULL));
    state = memmem(stream, length, section, sizeof(section) - 1);
    g_assert_nonnull(state);
    state += sizeof(section) - 1;
    g_assert_cmpuint(length - (state - stream), >=, 4 + CLIP_STATE_BYTES);
    g_assert_cmphex(ldl_be_p(state), ==, 1);
    memcpy(bytes, state + 4, CLIP_STATE_BYTES);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    qtest_qmp_assert_success(s, "{'execute':'stop'}");
}

static void test_clip_programming(void)
{
    g_autofree char *dir = g_dir_make_tmp("crime-clip-state-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    const uint64_t words[] = {
        0, UINT64_MAX, 0x8123834585678789ULL, 0x13579bdf02468aceULL,
    };
    uint8_t state[CLIP_STATE_BYTES], previous[CLIP_STATE_BYTES];
    uint64_t expected[4] = { 0 };
    QTestState *s = start();

    clip_save(s, path, previous);
    qtest_memset(s, 0x30000, 0x5a, 64);
    for (unsigned pass = 0; pass < ARRAY_SIZE(words); pass++) {
        for (unsigned word = 0; word < 4; word++) {
            g_assert_cmpint(memtx_writeq(s, CLIP_TLB + word * 8, words[pass]),
                            ==, MEMTX_OK);
            expected[word] = words[pass];
            clip_save(s, path, state);
            g_assert_cmpmem(state, CLIP_STATE_OFFSET,
                            previous, CLIP_STATE_OFFSET);
            for (unsigned i = 0; i < 4; i++) {
                g_assert_cmphex(ldq_be_p(state + CLIP_STATE_OFFSET + i * 8),
                                ==, expected[i]);
            }
            assert_bytes(s, 0x30000, 64, 0x5a);
            g_assert_cmphex(qtest_readl(s, STATUS), ==, 0x1e000000);
            memcpy(previous, state, sizeof(state));
        }
    }
    qtest_system_reset(s);
    clip_save(s, path, state);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(ldq_be_p(state + CLIP_STATE_OFFSET + i * 8), ==, 0);
    }
    assert_bytes(s, 0x30000, 64, 0x5a);
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_clip_start_forms(void)
{
    const unsigned offsets[] = { 0, 8, 0x18, 0x20, 0x28, 0x30, 0x38,
                                 0x40, 0x48, 0x70 };
    uint16_t entries[16];

    clip_entries(entries);
    for (unsigned command = 0; command < ARRAY_SIZE(offsets); command++) {
        QTestState *s = start();
        unsigned offset = offsets[command];
        uint32_t value;
        uint64_t a = 0x30000 + 127 * 512, b = 0x90000;

        clip_program(s, entries);
        /* Framebuffer C points elsewhere; it does not select CLIP storage. */
        qtest_writeq(s, 0x15001400, 0x800f000000000000ULL);
        clip_setup(s, 255, 127, 256, 128, 0x80808080);
        value = offset == 0x70 ? 0x13579bdf :
                offset == 0x30 ? (255U << 16) | 127 :
                offset == 0x38 ? (256U << 16) | 128 :
                                qtest_readl(s, MTE + offset);
        if (offset != 0x70) {
            qtest_writel(s, MTE + offset, offset == 0x18 ? 0x55 : 0);
        }
        qtest_memset(s, a + 254, 0x5a, 4);
        qtest_memset(s, b + 254, 0x6c, 4);
        g_assert_cmpint(memtx_writel(s, MTE + 0x800 + offset, value), ==,
                        MEMTX_OK);
        assert_bytes(s, a + 255, 2, 0x80);
        assert_bytes(s, b + 255, 2, 0x80);
        g_assert_cmphex(qtest_readb(s, a + 254), ==, 0x5a);
        g_assert_cmphex(qtest_readb(s, a + 257), ==, 0x5a);
        g_assert_cmphex(qtest_readb(s, b + 254), ==, 0x6c);
        g_assert_cmphex(qtest_readb(s, b + 257), ==, 0x6c);
        g_assert_cmphex(qtest_readl(s, STATUS), ==, 0x1e000000);
        qtest_quit(s);
    }
}

static void test_clip_bitmap(void)
{
    uint16_t entries[16];
    uint8_t page[65536];
    const uint32_t fills[] = { 0, 0x80808080, 0x7f7f7f7f };
    QTestState *s = start();

    clip_entries(entries);
    clip_program(s, entries);
    for (unsigned pass = 0; pass < ARRAY_SIZE(fills); pass++) {
        for (unsigned entry = 0; entry < 8; entry++) {
            qtest_memset(s, (entries[entry] & 0x7fff) * 65536ULL,
                         0xa5, sizeof(page));
        }
        clip_setup(s, 256, 0, 511, 1023, fills[pass]);
        g_assert_cmpint(memtx_writel(s, MTE + 0x870, 0), ==, MEMTX_OK);
        for (unsigned entry = 0; entry < 8; entry++) {
            qtest_memread(s, (entries[entry] & 0x7fff) * 65536ULL,
                         page, sizeof(page));
            for (unsigned i = 0; i < sizeof(page); i++) {
                g_assert_cmphex(page[i], ==,
                                i % 512 >= 256 ? fills[pass] & 0xff : 0xa5);
            }
        }
    }
    uint64_t last = (entries[15] & 0x7fff) * 65536ULL + 65535;

    qtest_memset(s, last - 1, 0x5a, 3);
    clip_setup(s, 511, 2047, 511, 2047, 0x123456b7);
    g_assert_cmpint(memtx_writel(s, MTE + 0x870, 0), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readb(s, last - 1), ==, 0x5a);
    g_assert_cmphex(qtest_readb(s, last), ==, 0xb7);
    g_assert_cmphex(qtest_readb(s, last + 1), ==, 0x5a);
    /* Sharing a page with framebuffer C is ordinary coherent RAM sharing. */
    qtest_writeq(s, 0x15001400, (uint64_t)entries[0] << 48);
    qtest_writel(s, MTE, 2U << 2);
    qtest_writel(s, MTE + 0x30, 255U << 16);
    qtest_writel(s, MTE + 0x38, 255U << 16);
    qtest_writel(s, MTE + 0x18, 0x3c);
    g_assert_cmpint(memtx_writel(s, MTE + 0x870, 0), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readb(s, 0x300ff), ==, 0x3c);
    qtest_quit(s);
}

static void clip_stopped(QTestState *s)
{
    g_autoptr(QDict) status =
        qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");

    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "internal-error");
}

static void test_clip_aliases(void)
{
    for (unsigned alias = 0; alias < 2; alias++) {
        for (unsigned disjoint = 0; disjoint < 2; disjoint++) {
            uint16_t entries[16];
            uint8_t page[65536];
            QTestState *s = start();

            clip_entries(entries);
            entries[1] = entries[0] | (alias ? 0x4000 : 0);
            clip_program(s, entries);
            qtest_memset(s, 0x30000, 0x5a, sizeof(page));
            clip_setup(s, 7, disjoint ? 127 : 0, 9, 128, 0x80);
            qtest_qmp_assert_success(s, "{'execute':'cont'}");
            g_assert_cmpint(memtx_writel(s, MTE + 0x870, 0), ==,
                            disjoint ? MEMTX_OK : MEMTX_ERROR);
            if (!disjoint) {
                clip_stopped(s);
            }
            qtest_memread(s, 0x30000, page, sizeof(page));
            for (unsigned i = 0; i < sizeof(page); i++) {
                bool changed = disjoint && i % 512 >= 7 && i % 512 <= 9 &&
                               (i / 512 == 0 || i / 512 == 127);

                g_assert_cmphex(page[i], ==, changed ? 0x80 : 0x5a);
            }
            qtest_quit(s);
        }
    }
}

static void test_clip_failure(void)
{
    const struct { unsigned index; uint32_t value; } faults[] = {
        { 0, CLIP_MODE | (1U << 11) }, { 0, CLIP_MODE | (1U << 10) },
        { 0, CLIP_MODE | (1U << 8) }, { 0, CLIP_MODE | (2U << 8) },
        { 1, 0 }, { 1, 0xfffffffe }, { 9, 0 }, { 9, UINT32_MAX },
        { 6, (512U << 16) | 127 }, { 7, (512U << 16) | 128 },
        { 6, (7U << 16) | 2048 }, { 7, (9U << 16) | 2048 },
        { 6, (10U << 16) | 127 }, { 6, (7U << 16) | 129 },
        { 0, 7U << 2 }, { 0, (1U << 11) | (6U << 5) | (4U << 2) },
    };
    const uint32_t good[10] = {
        CLIP_MODE | (7U << 5), UINT32_MAX, 0, 0x55, UINT32_MAX, 0x87654321,
        (7U << 16) | 127, (9U << 16) | 128, UINT32_MAX, 1,
    };
    g_autofree char *dir = g_dir_make_tmp("crime-clip-failure-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);

    for (unsigned fault = 0; fault < ARRAY_SIZE(faults) + 4; fault++) {
        uint16_t entries[16];
        uint8_t before[CLIP_STATE_BYTES], after[CLIP_STATE_BYTES], page[65536];
        QTestState *s = start_options(
            "-device sgi-crime-test-fixture,id=inject,"
            "crime=/machine/peripheral/crime");

        clip_entries(entries);
        clip_setup(s, 7, 127, 9, 128, 0x55);
        if (fault < ARRAY_SIZE(faults)) {
            qtest_writel(s, MTE + faults[fault].index * 8,
                         faults[fault].value);
        } else {
            switch (fault - ARRAY_SIZE(faults)) {
            case 0:
                entries[0] = 0;
                break;
            case 1:
                entries[1] = 0;
                break;
            case 2:
                entries[1] = 0x8400;
                break;
            case 3:
                entries[1] = 0x9400;
                break;
            }
        }
        clip_program(s, entries);
        qtest_qmp_assert_success(s,
            "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
            "'property':'memory-error-status','value':234881024}}");
        qtest_qmp_assert_success(s,
            "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
            "'property':'memory-error-address','value':74565}}");
        qtest_irq_intercept_out_named(s, "/machine/peripheral/crime", "cpu-ip");
        qtest_set_irq_in(s, "/machine/peripheral/crime", "mace", 4, 1);
        qtest_writeq(s, BASE + 0x18, 1U << 4);
        qtest_memset(s, 0x30000, 0x5a, sizeof(page));
        qtest_memset(s, 0x90000, 0x6c, sizeof(page));
        clip_save(s, path, before);
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
        g_assert_cmpint(memtx_writel(s, MTE + 0x818, 0x99), ==, MEMTX_ERROR);
        clip_stopped(s);
        g_assert_cmphex(qtest_readl(s, STATUS), ==, 0x1e000000);
        g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1U << 4);
        g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, 1U << 4);
        g_assert_cmphex(qtest_readq(s, BASE + 0x250), ==, 234881024);
        g_assert_cmphex(qtest_readq(s, BASE + 0x258), ==, 74565);
        g_assert_cmpint(qtest_get_irq(s, 0), ==, 1);
        assert_bytes(s, 0x30000, sizeof(page), 0x5a);
        assert_bytes(s, 0x90000, sizeof(page), 0x6c);
        clip_save(s, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
        if (fault < ARRAY_SIZE(faults)) {
            qtest_writel(s, MTE + faults[fault].index * 8,
                         good[faults[fault].index]);
        }
        clip_entries(entries);
        clip_program(s, entries);
        g_assert_cmpint(memtx_writel(s, MTE + 0x870, 0), ==, MEMTX_OK);
        for (unsigned entry = 0; entry < 2; entry++) {
            qtest_memread(s, entry ? 0x90000 : 0x30000, page, sizeof(page));
            for (unsigned i = 0; i < sizeof(page); i++) {
                bool changed = i % 512 >= 7 && i % 512 <= 9 &&
                               i / 512 == (entry ? 0 : 127);

                g_assert_cmphex(page[i], ==,
                                changed ? 0x55 : entry ? 0x6c : 0x5a);
            }
        }
        qtest_quit(s);
    }
    g_unlink(path);
    g_rmdir(dir);
}

static void test_clip_accesses(void)
{
    const struct {
        int offset;
        unsigned width;
        bool write, hard;
    } accesses[] = {
        { 0, 8, false, true }, { 0, 4, false, true },
        { 0, 1, true, true }, { 0, 2, true, true }, { 0, 4, true, true },
        { 1, 8, true, true }, { 4, 8, true, true }, { -4, 8, true, true },
        { 24, 4, true, true }, { 25, 8, true, true },
        { 0x800, 8, true, true }, { 0x800, 8, false, true },
        { -8, 8, true, false }, { -8, 8, false, false },
    };
    uint16_t entries[16];
    uint8_t before[CLIP_STATE_BYTES], after[CLIP_STATE_BYTES];
    g_autofree char *dir = g_dir_make_tmp("crime-clip-access-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start();

    clip_entries(entries);
    clip_program(s, entries);
    clip_setup(s, 7, 0, 9, 0, 0x55);
    clip_save(s, path, before);
    for (unsigned i = 0; i < ARRAY_SIZE(accesses); i++) {
        uint64_t value;
        uint64_t address = CLIP_TLB + accesses[i].offset;
        MemTxResult result;

        qtest_qmp_assert_success(s, "{'execute':'cont'}");
        result = accesses[i].write ?
            memtx_write(s, address, 0x1122334455667788ULL, accesses[i].width) :
            memtx_read(s, address, &value, accesses[i].width);
        g_assert_cmpint(result, ==, MEMTX_ERROR);
        if (accesses[i].hard) {
            clip_stopped(s);
        }
        clip_save(s, path, after);
        g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    }
    qtest_quit(s);
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned functional = 0; functional < 2; functional++) {
            const char *previous = selected_profile;

            if (legacy && functional) {
                continue;
            }
            selected_profile = legacy ? "legacy-software" : "1.5";
            s = start_profile(functional, "");
            selected_profile = previous;
            for (unsigned i = 0; i < 4; i++) {
                g_assert_cmpint(memtx_writeq(s, CLIP_TLB + i * 8,
                    0x8123834585678789ULL), ==, MEMTX_ERROR);
            }
            clip_save(s, path, after);
            for (unsigned i = CLIP_STATE_OFFSET; i < CLIP_STATE_BYTES; i++) {
                g_assert_cmpuint(after[i], ==, 0);
            }
            qtest_quit(s);
        }
    }
    g_unlink(path);
    g_rmdir(dir);
}

static void test_clip_migration(void)
{
    uint16_t entries[16];
    uint8_t before[CLIP_STATE_BYTES], after[CLIP_STATE_BYTES];
    g_autofree char *dir = g_dir_make_tmp("crime-clip-migrate-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *incoming = g_strdup_printf("exec:cat %s", quoted);
    QTestState *s = start(), *to = start_options("-incoming defer");

    clip_entries(entries);
    clip_program(s, entries);
    clip_setup(s, 255, 127, 256, 128, 0x80808080);
    qtest_memset(s, 0x30000 + 127 * 512 + 254, 0x5a, 4);
    qtest_memset(s, 0x90000 + 254, 0x6c, 4);
    clip_save(s, path, before);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", incoming);
    sgi_wait_migration(to, "completed");
    clip_save(to, path, after);
    g_assert_cmpmem(before, sizeof(before), after, sizeof(after));
    assert_bytes(to, 0x30000 + 127 * 512 + 254, 4, 0x5a);
    assert_bytes(to, 0x90000 + 254, 4, 0x6c);
    g_assert_cmpint(memtx_writel(to, MTE + 0x870, 0), ==, MEMTX_OK);
    assert_bytes(to, 0x30000 + 127 * 512 + 255, 2, 0x80);
    assert_bytes(to, 0x90000 + 255, 2, 0x80);
    qtest_system_reset(to);
    clip_save(to, path, after);
    for (unsigned i = CLIP_STATE_OFFSET; i < CLIP_STATE_BYTES; i++) {
        g_assert_cmpuint(after[i], ==, 0);
    }
    assert_bytes(to, 0x30000 + 127 * 512 + 255, 2, 0x80);
    assert_bytes(to, 0x90000 + 255, 2, 0x80);
    qtest_quit(s);
    qtest_quit(to);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_clip_invalid_stream(void)
{
    const char section[] = "sgi-crime/linear-transfer";
    g_autofree char *dir = g_dir_make_tmp("crime-clip-invalid-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *incoming = g_strdup_printf("exec:cat %s", quoted);

    for (unsigned fault = 0; fault < 5; fault++) {
        const char *previous = selected_profile;
        QTestState *from, *to;
        uint16_t entries[16], old[16];
        uint8_t snapshot[CLIP_STATE_BYTES];
        g_autofree char *stream = NULL;
        gsize length;
        char *state;

        selected_profile = fault == 3 ? "1.5" : "legacy-software";
        from = start_profile(fault != 4, "");
        to = start_profile(fault != 4, "-incoming defer");
        selected_profile = previous;
        clip_entries(entries);
        clip_entries(old);
        for (unsigned i = 0; i < 16; i++) {
            old[i] += 2;
        }
        if (fault < 3) {
            clip_program(from, entries);
            clip_setup(from, 1, 1, 2, 2, 0x99);
            clip_program(to, old);
            clip_setup(to, 7, 0, 9, 0, 0x55);
        }
        qtest_writeq(to, BASE + 0x18, 1U << 4);
        clip_save(from, path, snapshot);
        g_assert_true(g_file_get_contents(path, &stream, &length, NULL));
        state = memmem(stream, length, section, sizeof(section) - 1);
        g_assert_nonnull(state);
        state += sizeof(section) - 1 + 4;
        switch (fault) {
        case 0:
            memmove(state + CLIP_STATE_OFFSET, state + CLIP_STATE_BYTES,
                    length - (state + CLIP_STATE_BYTES - stream));
            length -= CLIP_STATE_BYTES - CLIP_STATE_OFFSET;
            break;
        case 1:
            length = state - stream + CLIP_STATE_OFFSET + 17;
            break;
        case 2:
            state[0] = 2;
            break;
        case 3:
        case 4:
            stq_be_p(state + CLIP_STATE_OFFSET, 0x8123834585678789ULL);
            break;
        }
        g_assert_true(g_file_set_contents(path, stream, length, NULL));
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming',"
            "'arguments':{'uri':%s,'exit-on-error':false}}", incoming);
        wait_migration_failed(to);
        g_assert_cmphex(qtest_readq(to, BASE + 0x18), ==, 1U << 4);
        if (fault < 3) {
            /* Incoming RAM may arrive before device validation rejects load. */
            g_assert_cmphex(qtest_readl(to, MTE), ==, CLIP_MODE | (7U << 5));
            g_assert_cmphex(qtest_readl(to, MTE + 0x18), ==, 0x55);
            for (unsigned i = 0; i < 16; i++) {
                uint64_t old_address = (old[i] & 0x7fff) * 65536ULL + 7;
                uint64_t new_address = (entries[i] & 0x7fff) * 65536ULL + 7;

                qtest_memset(to, old_address, 0x5a, 3);
                qtest_memset(to, new_address, 0x6c, 3);
                qtest_writel(to, MTE + 0x30, (7U << 16) | (i * 128));
                qtest_writel(to, MTE + 0x38, (9U << 16) | (i * 128));
                g_assert_cmpint(memtx_writel(to, MTE + 0x870, 0), ==,
                                MEMTX_OK);
                assert_bytes(to, old_address, 3, 0x55);
                assert_bytes(to, new_address, 3, 0x6c);
            }
        }
        qtest_quit(from);
        qtest_quit(to);
    }
    g_unlink(path);
    g_rmdir(dir);
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
        { "migration-hardware-software-overlap", NULL,
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
    static const TransferCase clip_tests[] = {
        { "programming", test_clip_programming, NULL, NULL, "legacy-software" },
        { "start-forms", test_clip_start_forms, NULL, NULL, "legacy-software" },
        { "bitmap", test_clip_bitmap, NULL, NULL, "legacy-software" },
        { "aliases", test_clip_aliases, NULL, NULL, "legacy-software" },
        { "failure", test_clip_failure, NULL, NULL, "legacy-software" },
        { "accesses", test_clip_accesses, NULL, NULL, "legacy-software" },
        { "migration", test_clip_migration, NULL, NULL, "legacy-software" },
        { "invalid-stream", test_clip_invalid_stream, NULL, NULL,
          "legacy-software" },
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
    for (unsigned i = 0; i < ARRAY_SIZE(clip_tests); i++) {
        g_autofree char *name = g_strdup_printf("/sgi/crime/mte/clip/%s",
                                               clip_tests[i].name);

        qtest_add_data_func(name, &clip_tests[i], run_transfer_case);
    }
    return g_test_run();
}
