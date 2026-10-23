/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Invalid-entry suppression is a model policy, not silicon evidence. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-migrate.h"

#define BASE 0x14000000ULL
#define TLB 0x15001700ULL
#define MTE 0x15003000ULL
#define STATUS 0x15004000ULL
#define CLEAR_A (4U << 2)
#define COPY_AB ((1U << 11) | (4U << 5) | (5U << 2))
#define CRIME_PATH "/machine/peripheral/crime"
#define SUPPRESSION "sgi-crime: LINEAR CLEAR suppressed "

static const char *profile = "1.5";

typedef struct Fixture {
    QTestState *qts;
    char *directory;
    char *log;
} Fixture;

static Fixture start(bool banked, const char *extra)
{
    Fixture f = { 0 };
    GError *error = NULL;

    f.directory = g_dir_make_tmp("crime-invalid-clear-XXXXXX", &error);
    g_assert_no_error(error);
    f.log = g_build_filename(f.directory, "model.txt", NULL);
    f.qts = qtest_initf(
        "-M none -cpu R10000 -S -object memory-backend-ram,id=fit,size=%s "
        "-device memory-transaction-test,id=memtx%s "
        "-device sgi-crime,id=crime,ram=/objects/fit/fit[0],crime-profile=%s,"
        "functional-memory-transfer=on%s -d unimp -D %s %s",
        banked ? "1G" : "64M",
        banked ? "" : ",migrate-ram=/objects/fit/fit[0]", profile,
        banked ? ",eight-128m-banks=on" : "", f.log, extra);
    qtest_sysbus_mmio_map(f.qts, CRIME_PATH, 0, BASE);
    if (banked) {
        qtest_writeq(f.qts, BASE + 0x208, 0x100);
    }
    qtest_irq_intercept_out_named(f.qts, CRIME_PATH, "cpu-ip");
    return f;
}

static void finish(Fixture *f, unsigned records, unsigned clear, unsigned mask)
{
    g_autofree char *contents = NULL;
    GError *error = NULL;
    const char *cursor;
    unsigned found = 0, lines = 0;

    qtest_quit(f->qts);
    g_assert_true(g_file_get_contents(f->log, &contents, NULL, &error));
    g_assert_no_error(error);
    cursor = contents;
    while ((cursor = strstr(cursor, SUPPRESSION))) {
        unsigned pages, validity, outside;

        g_assert_cmpint(sscanf(cursor, SUPPRESSION
            "%u destination pages (validity-bit-clear=%u, outside-mask=%u)",
            &pages, &validity, &outside), ==, 3);
        g_assert_cmpuint(pages, ==, clear + mask);
        g_assert_cmpuint(validity, ==, clear);
        g_assert_cmpuint(outside, ==, mask);
        found++;
        cursor++;
    }
    for (const char *p = contents; *p; p++) {
        lines += *p == '\n';
    }
    g_assert_cmpuint(found, ==, records);
    g_assert_cmpuint(lines, ==, records);
    g_unlink(f->log);
    g_rmdir(f->directory);
    g_free(f->log);
    g_free(f->directory);
}

static void pair(QTestState *s, unsigned bank, unsigned word,
                  uint32_t first, uint32_t second)
{
    g_assert_cmpint(memtx_writeq(s, TLB + bank * 128 + word * 8,
        ((uint64_t)first << 32) | second), ==, MEMTX_OK);
}

static void bytes(QTestState *s, uint64_t address, unsigned length,
                  uint8_t value)
{
    g_autofree uint8_t *data = g_malloc(length);

    qtest_memread(s, address, data, length);
    for (unsigned i = 0; i < length; i++) {
        g_assert_cmphex(data[i], ==, value);
    }
}

static void canaries(QTestState *s)
{
    qtest_memset(s, 0x3000, 0x51, 4096);
    qtest_memset(s, 0x7000, 0x62, 4096);
    qtest_memset(s, 0xb000, 0x73, 4096);
}

static void parameters(QTestState *s, uint32_t first, uint32_t last)
{
    qtest_writel(s, MTE, CLEAR_A);
    qtest_writel(s, MTE + 8, UINT32_MAX);
    qtest_writel(s, MTE + 0x18, 0x123456a6);
    qtest_writel(s, MTE + 0x30, first);
    qtest_writel(s, MTE + 0x38, last);
}

static void pending(QTestState *s)
{
    qtest_set_irq_in(s, CRIME_PATH, "mace", 4, 1);
    qtest_writeq(s, BASE + 0x18, 1U << 4);
}

static void unchanged_interrupts(QTestState *s)
{
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1U << 4);
    g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, 1U << 4);
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmpint(qtest_get_irq(s, i), ==, i == 0);
    }
}

static void stopped(QTestState *s)
{
    g_autoptr(QDict) status =
        qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");

    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "internal-error");
}

static void mixed_result(QTestState *s)
{
    bytes(s, 0x3000, 4080, 0x51);
    bytes(s, 0x3ff0, 16, 0xa6);
    bytes(s, 0x7000, 4096, 0x62);
    bytes(s, 0xb000, 20, 0xa6);
    bytes(s, 0xb014, 4076, 0x73);
    g_assert_cmphex(qtest_readl(s, STATUS), ==, 0x1e000000);
    unchanged_interrupts(s);
}

static void test_raw(gconstpointer unused)
{
    Fixture f = start(false, "");
    QTestState *s = f.qts;
    uint64_t words[32];
    const uint64_t patterns[] = {
        0xffffffff80040001ULL, 0x76543210fedcba98ULL,
        0x7fffffff80100007ULL, 0xffffffffffffffffULL,
    };

    canaries(s);
    pending(s);
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        words[i] = patterns[i % ARRAY_SIZE(patterns)] ^ ((uint64_t)i << 32);
        g_assert_cmpint(memtx_writeq(s, TLB + i * 8, words[i]), ==, MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, TLB + i * 8), ==, words[i]);
    }
    qtest_writeq(s, TLB + 7 * 8, 0x4012345681000000ULL);
    words[7] = 0x4012345681000000ULL;
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        g_assert_cmphex(qtest_readq(s, TLB + i * 8), ==, words[i]);
    }
    bytes(s, 0x3000, 4096, 0x51);
    bytes(s, 0x7000, 4096, 0x62);
    bytes(s, 0xb000, 4096, 0x73);
    unchanged_interrupts(s);
    g_assert_cmphex(qtest_readq(s, BASE + 0x250), ==, 0);
    qtest_system_reset(s);
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        g_assert_cmphex(qtest_readq(s, TLB + i * 8), ==, 0);
    }
    finish(&f, 0, 0, 0);
}

static void test_mixed(gconstpointer data)
{
    uint32_t invalid = GPOINTER_TO_UINT(data);
    bool validity_clear = !(invalid & 0x80000000U);
    Fixture f = start(false, "");
    QTestState *s = f.qts;

    canaries(s);
    pending(s);
    pair(s, 0, 0, 0x80000003, invalid);
    pair(s, 0, 1, 0x8000000b, 0xffffffff);
    parameters(s, 0xff0, 0xfff);
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 0x2013), ==, MEMTX_OK);
    mixed_result(s);
    finish(&f, 1, validity_clear, !validity_clear);
}

static void test_all_invalid(gconstpointer unused)
{
    Fixture f = start(false, "");
    QTestState *s = f.qts;

    canaries(s);
    pending(s);
    pair(s, 0, 0, 3, 0x80100007);
    pair(s, 0, 1, 0x4010000b, 0xffffffff);
    parameters(s, 0xff0, 0x2013);
    qtest_writel(s, MTE, COPY_AB);
    g_assert_cmpint(memtx_writel(s, MTE + 0x800, CLEAR_A), ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, MTE), ==, CLEAR_A);
    bytes(s, 0x3000, 4096, 0x51);
    bytes(s, 0x7000, 4096, 0x62);
    bytes(s, 0xb000, 4096, 0x73);
    g_assert_cmphex(qtest_readl(s, STATUS), ==, 0x1e000000);
    unchanged_interrupts(s);
    finish(&f, 1, 2, 1);
}

static void test_packed_word(gconstpointer unused)
{
    Fixture f = start(false, "");
    QTestState *s = f.qts;

    pending(s);
    qtest_memset(s, 0x1000, 0x5c, 4096);
    qtest_writeq(s, TLB, 0xffffffff80040001ULL);
    parameters(s, 0xffc, 0x1003);
    g_assert_cmpint(memtx_writel(s, MTE + 0x800, CLEAR_A), ==, MEMTX_OK);
    bytes(s, 0x1000, 4, 0xa6);
    bytes(s, 0x1004, 4092, 0x5c);
    g_assert_cmphex(qtest_readq(s, TLB), ==, 0xffffffff80040001ULL);
    unchanged_interrupts(s);
    finish(&f, 1, 0, 1);
}

static void test_valid(gconstpointer data)
{
    bool copy = GPOINTER_TO_INT(data);
    Fixture f = start(false, "");
    QTestState *s = f.qts;
    uint8_t input[4096], output[4096];

    canaries(s);
    pending(s);
    pair(s, 0, 0, 0x80000003, 0x80000007);
    pair(s, 0, 1, 0x8000000b, 0);
    parameters(s, 0xff0, 0x2013);
    if (copy) {
        for (unsigned i = 0; i < sizeof(input); i++) {
            input[i] = i * 17 + (i >> 7);
        }
        qtest_memwrite(s, 0x7000, input, sizeof(input));
        pair(s, 1, 0, 0x80000013, 0x80000017);
        pair(s, 1, 1, 0x8000001b, 0);
        qtest_memset(s, 0x13000, 0x81, 4096);
        qtest_memset(s, 0x17000, 0x82, 4096);
        qtest_memset(s, 0x1b000, 0x83, 4096);
        qtest_writel(s, MTE + 0x20, 0xff0);
        qtest_writel(s, MTE + 0x28, 0x2013);
        g_assert_cmpint(memtx_writel(s, MTE + 0x800, COPY_AB), ==, MEMTX_OK);
        bytes(s, 0x13000, 4080, 0x81);
        bytes(s, 0x13ff0, 16, 0x51);
        qtest_memread(s, 0x17000, output, sizeof(output));
        g_assert_cmpmem(input, sizeof(input), output, sizeof(output));
        bytes(s, 0x1b000, 20, 0x73);
        bytes(s, 0x1b014, 4076, 0x83);
    } else {
        g_assert_cmpint(memtx_writel(s, MTE + 0x800, CLEAR_A), ==, MEMTX_OK);
        bytes(s, 0x3000, 4080, 0x51);
        bytes(s, 0x3ff0, 16, 0xa6);
        bytes(s, 0x7000, 4096, 0xa6);
        bytes(s, 0xb000, 20, 0xa6);
        bytes(s, 0xb014, 4076, 0x73);
    }
    g_assert_cmphex(qtest_readl(s, STATUS), ==, 0x1e000000);
    unchanged_interrupts(s);
    finish(&f, 0, 0, 0);
}

static void test_copy_refusal(gconstpointer data)
{
    unsigned kind = GPOINTER_TO_UINT(data);
    Fixture f = start(false, "");
    QTestState *s = f.qts;
    uint32_t source = 0x80000007, destination = 0x80000013;

    canaries(s);
    pending(s);
    qtest_memset(s, 0x13000, 0x84, 4096);
    if (kind == 0) {
        source = 7;
    } else if (kind == 1) {
        source = 0x80100007;
    } else if (kind == 2) {
        destination = 0x13;
    } else if (kind == 3) {
        destination = 0xc0000013;
    } else {
        source = 0x80004000;
    }
    pair(s, 0, 0, 0x80000003, source);
    pair(s, 1, 0, 0x8000000b, destination);
    parameters(s, 0, 4096);
    qtest_writel(s, MTE + 0x20, 0);
    qtest_writel(s, MTE + 0x28, 4096);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    g_assert_cmpint(memtx_writel(s, MTE + 0x800, COPY_AB), !=, MEMTX_OK);
    stopped(s);
    g_assert_cmphex(qtest_readl(s, MTE), ==, CLEAR_A);
    bytes(s, 0x3000, 4096, 0x51);
    bytes(s, 0x7000, 4096, 0x62);
    bytes(s, 0xb000, 4096, 0x73);
    bytes(s, 0x13000, 4096, 0x84);
    unchanged_interrupts(s);
    finish(&f, 0, 0, 0);
}

static void test_clear_refusal(gconstpointer data)
{
    unsigned kind = GPOINTER_TO_UINT(data);
    Fixture f = start(kind == 1 || kind == 2, "");
    QTestState *s = f.qts;
    uint32_t last = kind == 0 ? 0x80010000 :
                    kind == 1 ? 0x80020000 :
                    kind == 2 ? 0x80008000 : 0x80040003;
    uint64_t errors = 0, address = 0;
    uint8_t ignored;

    canaries(s);
    pending(s);
    if (kind == 1 || kind == 2) {
        g_assert_cmpint(memtx_readb(s, 0x20000123, &ignored), !=, MEMTX_OK);
        errors = qtest_readq(s, BASE + 0x250);
        address = qtest_readq(s, BASE + 0x258);
    }
    pair(s, 0, 0, 0x80000003, 0x40100007);
    pair(s, 0, 1, last, 0);
    parameters(s, kind == 3 ? 0 : 0xff0, 0xfff);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    g_assert_cmpint(memtx_writel(s, MTE + 0x838, 0x2013), !=, MEMTX_OK);
    stopped(s);
    bytes(s, 0x3000, 4096, 0x51);
    bytes(s, 0x7000, 4096, 0x62);
    bytes(s, 0xb000, 4096, 0x73);
    g_assert_cmphex(qtest_readq(s, BASE + 0x250), ==, errors);
    g_assert_cmphex(qtest_readq(s, BASE + 0x258), ==, address);
    pair(s, 0, 1, 0x8000000b, 0);
    g_assert_cmpint(memtx_writel(s, MTE + 0x800, CLEAR_A), ==, MEMTX_OK);
    if (kind == 3) {
        bytes(s, 0x3000, 4096, 0xa6);
    } else {
        bytes(s, 0x3000, 4080, 0x51);
        bytes(s, 0x3ff0, 16, 0xa6);
    }
    bytes(s, 0x7000, 4096, 0x62);
    bytes(s, 0xb000, 4096, 0x73);
    unchanged_interrupts(s);
    finish(&f, 0, 0, 0);
}

static void test_migration(gconstpointer unused)
{
    Fixture from = start(false, ""), to = start(false, "-incoming defer");
    QTestState *s = from.qts;
    g_autofree char *socket =
        g_build_filename(from.directory, "move.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    uint64_t words[32];

    canaries(s);
    pending(s);
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        words[i] = 0x12345678fedcba98ULL ^ ((uint64_t)i << 48);
        qtest_writeq(s, TLB + i * 8, words[i]);
    }
    words[0] = 0x8000000380100007ULL;
    words[1] = 0x8000000bffffffffULL;
    qtest_writeq(s, TLB, words[0]);
    qtest_writeq(s, TLB + 8, words[1]);
    parameters(s, 0xff0, 0x2013);
    qtest_writel(s, MTE, 0xdeadbeef);
    qtest_writel(s, MTE + 0x40, 0x12345678);
    qtest_writel(s, MTE + 0x48, 0x87654321);
    qtest_qmp_assert_success(to.qts,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(s, uri);
    sgi_wait_migration(to.qts, "completed");
    for (unsigned i = 0; i < ARRAY_SIZE(words); i++) {
        g_assert_cmphex(qtest_readq(to.qts, TLB + i * 8), ==, words[i]);
    }
    g_assert_cmphex(qtest_readl(to.qts, MTE), ==, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(to.qts, MTE + 0x40), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(to.qts, MTE + 0x48), ==, 0x87654321);
    g_assert_cmpint(memtx_writel(to.qts, MTE + 0x800, CLEAR_A), ==, MEMTX_OK);
    mixed_result(to.qts);
    finish(&to, 1, 0, 1);
    finish(&from, 0, 0, 0);
}

typedef struct Case {
    const char *name;
    GTestDataFunc function;
    gconstpointer data;
    const char *profile;
} Case;

static void run_case(gconstpointer data)
{
    const Case *test = data;

    profile = test->profile;
    test->function(test->data);
}

int main(int argc, char **argv)
{
    const Case tests[] = {
        { "raw-storage", test_raw, NULL, NULL },
        { "mixed-validity-clear", test_mixed, GUINT_TO_POINTER(7), NULL },
        { "mixed-outside-bit20", test_mixed,
          GUINT_TO_POINTER(0x80100007), NULL },
        { "mixed-outside-bit30", test_mixed,
          GUINT_TO_POINTER(0xc0000007), NULL },
        { "invalid-reason-order", test_mixed,
          GUINT_TO_POINTER(0x40120007), NULL },
        { "all-invalid", test_all_invalid, NULL, NULL },
        { "packed-word", test_packed_word, NULL, NULL },
        { "valid-clear", test_valid, GINT_TO_POINTER(0), NULL },
        { "valid-copy", test_valid, GINT_TO_POINTER(1), NULL },
        { "copy-invalid-source", test_copy_refusal, GUINT_TO_POINTER(0), NULL },
        { "copy-outside-source", test_copy_refusal, GUINT_TO_POINTER(1), NULL },
        { "copy-invalid-dest", test_copy_refusal, GUINT_TO_POINTER(2), NULL },
        { "copy-outside-dest", test_copy_refusal, GUINT_TO_POINTER(3), NULL },
        { "copy-decoder-fault", test_copy_refusal, GUINT_TO_POINTER(4), NULL },
        { "clear-unmapped-view", test_clear_refusal,
          GUINT_TO_POINTER(0), NULL },
        { "clear-absent-bank", test_clear_refusal, GUINT_TO_POINTER(1), NULL },
        { "clear-wrong-geometry", test_clear_refusal,
          GUINT_TO_POINTER(2), NULL },
        { "clear-self-alias", test_clear_refusal, GUINT_TO_POINTER(3), NULL },
        { "raw-migration", test_migration, NULL, NULL },
    };
    const char *profiles[] = { "1.5", "legacy-software" };

    g_test_init(&argc, &argv, NULL);
    for (unsigned p = 0; p < ARRAY_SIZE(profiles); p++) {
        for (unsigned i = 0; i < ARRAY_SIZE(tests); i++) {
            Case *test = g_new(Case, 1);
            g_autofree char *name =
                g_strdup_printf("/sgi/crime/invalid-clear/%s/%s",
                                profiles[p], tests[i].name);

            *test = tests[i];
            test->profile = profiles[p];
            qtest_add_data_func_full(name, test, run_case, g_free);
        }
    }
    return g_test_run();
}
