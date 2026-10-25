/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "memtx.h"
#include "sgi-migrate.h"
#include "sgi-mips-emit.h"
#include "sgi-launch.h"

#define BASE 0x14000000ULL
#define CRIME_PATH "/machine/peripheral/crime"
#define LEVEL_LATCHED 0x81cf0000U

static QTestState *start_profile(bool legacy, const char *extra)
{
    QTestState *s = qtest_initf(
        "-object memory-backend-ram,id=fit,size=64M "
        "-device memory-transaction-test,id=memtx,"
        "migrate-ram=/objects/fit/fit[0] "
        "-M none -cpu R10000 -S -d guest_errors,unimp "
        "-device sgi-crime,id=crime,ram=/objects/fit/fit[0],"
        "crime-profile=%s %s", legacy ? "legacy-software" : "1.5", extra);

    qtest_sysbus_mmio_map(s, CRIME_PATH, 0, BASE);
    return s;
}

static void cause(QTestState *s, unsigned source, bool level)
{
    qtest_set_irq_in(s, CRIME_PATH, source < 16 ? "mace" : "external",
                     source < 16 ? source : source - 16, level);
}

static void assert_outputs(QTestState *s, unsigned outputs)
{
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmpint(qtest_get_irq(s, i), ==, !!(outputs & (1U << i)));
    }
}

static void assert_stopped(QTestState *s)
{
    g_autoptr(QDict) status =
        qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");

    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "internal-error");
}

static void test_identity(void)
{
    QTestState *s;
    uint64_t value;
    uint32_t word;

    for (unsigned legacy = 0; legacy < 2; legacy++) {
        s = start_profile(legacy, "");
        g_assert_cmphex(qtest_readq(s, BASE), ==, legacy ? 0xa1 : 0xb1);
        g_assert_cmpint(memtx_readq(s, BASE + (legacy ? 0x48 : 0x4048),
                                    &value), ==, MEMTX_OK);
        g_assert_cmphex(value, ==, 0);
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
        g_assert_cmpint(memtx_readq(s, BASE + (legacy ? 0x4048 : 0x48),
                                    &value), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readl(s, BASE + 4, &word), !=, MEMTX_OK);
        if (legacy) {
            assert_stopped(s);
        }
        qtest_system_reset(s);
        g_assert_cmphex(qtest_readq(s, BASE), ==, legacy ? 0xa1 : 0xb1);
        qtest_quit(s);
        s = start_profile(legacy, "-global sgi-crime.prom-id-word=on");
        g_assert_cmphex(qtest_readl(s, BASE + 4), ==, legacy ? 0xa1 : 0xb1);
        qtest_quit(s);
    }
}

/* Include nonzero latches so a mistaken error clear is observable. */
static void check_error_enable_zero(QTestState *s)
{
    const unsigned offsets[] = {
        0, 8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38,
        0x40, 0x48, 0x200, 0x250, 0x258, 0x270,
    };
    uint64_t before[ARRAY_SIZE(offsets)];
    unsigned outputs = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++) {
        before[i] = qtest_readq(s, BASE + offsets[i]);
    }
    for (unsigned i = 0; i < 5; i++) {
        outputs |= qtest_get_irq(s, i) << i;
    }
    for (unsigned write = 0; write < 2; write++) {
        g_assert_cmpint(memtx_writeq(s, BASE + 0x50, 0), ==, MEMTX_OK);
        for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++) {
            g_assert_cmphex(qtest_readq(s, BASE + offsets[i]), ==, before[i]);
        }
        assert_outputs(s, outputs);
    }
}

static void check_error_enable_log(const char *path, unsigned expected)
{
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    unsigned count = 0;

    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    lines = g_strsplit(contents, "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        if (strstr(lines[i], "legacy CPU error enable")) {
            g_assert_nonnull(strstr(lines[i], "0x14000050"));
            g_assert_nonnull(strstr(lines[i], "8-byte"));
            count++;
        }
    }
    g_assert_cmpuint(count, ==, expected);
}

static void test_error_enable_zero(void)
{
    g_autofree char *directory =
        g_dir_make_tmp("crime-error-enable-XXXXXX", NULL);
    g_autofree char *from_log = g_build_filename(directory, "from.txt", NULL);
    g_autofree char *to_log = g_build_filename(directory, "to.txt", NULL);
    g_autofree char *socket =
        g_build_filename(directory, "migration.sock", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    g_autofree char *extra = g_strdup_printf(
        "-D %s -device sgi-crime-test-fixture,id=inject,crime=" CRIME_PATH,
        from_log);
    g_autofree char *incoming = g_strdup_printf(
        "-D %s -incoming defer "
        "-device sgi-crime-test-fixture,id=inject,crime=" CRIME_PATH, to_log);
    QTestState *from = start_profile(true, extra);
    QTestState *to = start_profile(true, incoming);

    qtest_irq_intercept_out_named(from, CRIME_PATH, "cpu-ip");
    qtest_irq_intercept_out_named(to, CRIME_PATH, "cpu-ip");
    qtest_writeq(from, BASE + 0x18, 1U << 4);
    cause(from, 4, 1);
    qtest_writeq(from, BASE + 0x20, 1U << 30);
    qtest_writeq(from, BASE + 0x38, 0x12345678);
    qtest_qmp_assert_success(from,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
        "'property':'cpu-error-address','value':305419896}}");
    qtest_qmp_assert_success(from,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
        "'property':'cpu-error-status','value':5}}");
    check_error_enable_zero(from);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readq(to, BASE + 0x40), ==, 0x12345678);
    g_assert_cmphex(qtest_readq(to, BASE + 0x48), ==, 5);
    g_assert_cmphex(qtest_readq(to, BASE + 0x10), ==, (1U << 4) | (1U << 30));
    assert_outputs(to, 1);
    check_error_enable_zero(to);
    qtest_system_reset(to);
    g_assert_cmphex(qtest_readq(to, BASE + 0x40), ==, 0);
    g_assert_cmphex(qtest_readq(to, BASE + 0x48), ==, 0);
    assert_outputs(to, 0);
    check_error_enable_zero(to);
    qtest_quit(from);
    qtest_quit(to);
    check_error_enable_log(from_log, 2);
    check_error_enable_log(to_log, 4);
    g_unlink(from_log);
    g_unlink(to_log);
    g_rmdir(directory);
}

static void test_error_enable_refusal(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        QTestState *s = start_profile(legacy, "");
        uint64_t value;

        qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
        qtest_writeq(s, BASE + 0x18, 1U << 4);
        cause(s, 4, 1);
        for (unsigned size = 1; size <= 8; size *= 2) {
            g_assert_cmpint(memtx_read(s, BASE + 0x50, &value, size),
                            !=, MEMTX_OK);
            if (size < 8 || !legacy) {
                g_assert_cmpint(memtx_write(s, BASE + 0x50, 0, size),
                                !=, MEMTX_OK);
            }
        }
        for (unsigned bit = 0; bit < 64; bit++) {
            g_assert_cmpint(memtx_writeq(s, BASE + 0x50, 1ULL << bit),
                            !=, MEMTX_OK);
        }
        for (unsigned offset = 1; offset < 8; offset++) {
            g_assert_cmpint(memtx_writeq(s, BASE + 0x50 + offset, 0),
                            !=, MEMTX_OK);
            g_assert_cmpint(memtx_readq(s, BASE + 0x50 + offset, &value),
                            !=, MEMTX_OK);
        }
        g_assert_cmphex(qtest_readq(s, BASE + 0x40), ==, 0);
        g_assert_cmphex(qtest_readq(s, BASE + (legacy ? 0x48 : 0x4048)), ==, 0);
        g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1U << 4);
        g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, 1U << 4);
        assert_outputs(s, 1);
        qtest_quit(s);
    }
}

static void test_controls(void)
{
    QTestState *s = start_profile(true, "");

    g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 0x100);
    qtest_writeq(s, BASE + 0x18, 1U << 4);
    qtest_set_irq_in(s, CRIME_PATH, "mace", 4, 1);
    qtest_writeq(s, BASE + 8, 0);
    qtest_writeq(s, BASE + 8, 0x100);
    qtest_writeq(s, BASE + 0x10, 0);
    qtest_writeq(s, BASE + 0x30, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 0x100);
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1U << 4);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    for (unsigned i = 0; i < 64; i++) {
        if (i == 8) {
            continue;
        }
        g_assert_cmpint(memtx_writeq(s, BASE + 8, 1ULL << i), !=, MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, BASE + 8), ==, 0x100);
        g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1U << 4);
    }
    g_assert_cmpint(memtx_writeq(s, BASE + 0x10, 1), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, BASE + 0x30, 1), !=, MEMTX_OK);
    assert_stopped(s);
    qtest_quit(s);
}

static void test_level_latches(void)
{
    QTestState *s = start_profile(true, "");
    uint32_t level = (1U << 4) | (1U << 8);
    uint32_t events = (1U << 16) | (1U << 22);

    qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
    qtest_set_irq_in(s, CRIME_PATH, "mace", 4, 1);
    qtest_set_irq_in(s, CRIME_PATH, "mace", 8, 1);
    cause(s, 16, 1);
    cause(s, 22, 1);
    qtest_writeq(s, BASE + 0x18, level | events);
    assert_outputs(s, 1);
    cause(s, 16, 0);
    qtest_writeq(s, BASE + 0x28, ~(uint32_t)(1U << 16));
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, level | (1U << 22));
    qtest_writeq(s, BASE + 0x28, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, level | (1U << 22));
    assert_outputs(s, 1);
    cause(s, 22, 0);
    qtest_writeq(s, BASE + 0x28, UINT32_MAX);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, level | (1U << 22));
    qtest_writeq(s, BASE + 0x28, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, level);
    cause(s, 19, 1);
    cause(s, 16, 1);
    cause(s, 16, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, level | (1U << 19));
    assert_outputs(s, 0);
    qtest_writeq(s, BASE + 0x18, 1U << 19);
    assert_outputs(s, 1);
    qtest_quit(s);
}

static const unsigned default_group[32] = {
    1, 1, 1, 0, 0, 0, 3, 4, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 4, 4, 0, 0, 0, 0, 0, 0, 2, 3, 4, 0
};

static void exercise_sources(QTestState *s, bool legacy)
{
    for (unsigned source = 0; source < 32; source++) {
        uint32_t bit = 1U << source;
        unsigned held = source == 4 ? 8 : 4;
        uint32_t held_bit = 1U << held;
        unsigned held_output = 1U << (legacy ? 0 : default_group[held]);
        unsigned output = held_output |
                          (1U << (legacy ? 0 : default_group[source]));
        bool software = source >= 28 && source <= (legacy ? 30 : 29);
        bool latched = (legacy ? LEVEL_LATCHED : 0x41cf0000U) & bit;

        cause(s, held, 1);
        g_assert_cmpint(memtx_writeq(s, BASE + 0x18, bit | held_bit),
                        ==, MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, bit | held_bit);
        cause(s, source, 1);
        if (software) {
            assert_outputs(s, held_output);
            g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, held_bit);
            qtest_writeq(s, BASE + 0x20, bit);
        } else {
            g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, bit | held_bit);
        }
        assert_outputs(s, output);
        qtest_writeq(s, BASE + 0x18, held_bit);
        assert_outputs(s, held_output);
        qtest_writeq(s, BASE + 0x18, bit | held_bit);
        assert_outputs(s, output);
        qtest_writeq(s, BASE + 0x28, ~bit);
        assert_outputs(s, output);
        cause(s, source, 0);
        if (software) {
            qtest_writeq(s, BASE + 0x20, 0);
        } else if (latched) {
            assert_outputs(s, output);
            qtest_writeq(s, BASE + 0x28, UINT32_MAX);
            g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, bit | held_bit);
            qtest_writeq(s, BASE + 0x28, ~bit);
        }
        g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, held_bit);
        assert_outputs(s, held_output);
        cause(s, held, 0);
        qtest_writeq(s, BASE + 0x18, 0);
        assert_outputs(s, 0);
    }
}

static void test_source_routing(gconstpointer data)
{
    bool functional = GPOINTER_TO_UINT(data);

    for (unsigned legacy = 0; legacy < 2; legacy++) {
        QTestState *s = start_profile(legacy, functional ?
            "-global sgi-crime.functional-memory-transfer=on" : "");

        qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
        g_assert_cmpint(memtx_writeq(s, BASE + 0x18, UINT32_MAX), ==, MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, UINT32_MAX);
        g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 0);
        assert_outputs(s, 0);
        exercise_sources(s, legacy);
        qtest_quit(s);
    }
}

static void test_source_reset(gconstpointer data)
{
    bool functional = GPOINTER_TO_UINT(data);

    for (unsigned legacy = 0; legacy < 2; legacy++) {
        QTestState *s = start_profile(legacy, functional ?
            "-global sgi-crime.functional-memory-transfer=on" : "");

        qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
        for (unsigned source = 0; source < 32; source++) {
            uint32_t bit = 1U << source;
            bool software = source >= 28 && source <= (legacy ? 30 : 29);
            bool latched = (legacy ? LEVEL_LATCHED : 0x41cf0000U) & bit;

            if (latched) {
                cause(s, source, 1);
                cause(s, source, 0);
                g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, bit);
                qtest_system_reset(s);
                g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 0);
            }
            cause(s, source, 1);
            qtest_system_reset(s);
            g_assert_cmphex(qtest_readq(s, BASE + 0x28),
                            ==, software ? 0 : bit);
            assert_outputs(s, 0);
            qtest_writeq(s, BASE + 0x18, bit);
            assert_outputs(s, software ? 0 :
                           (1U << (legacy ? 0 : default_group[source])));
            cause(s, source, 0);
            qtest_writeq(s, BASE + 0x28, 0);
            assert_outputs(s, 0);
        }
        qtest_quit(s);
    }
}

static void test_counter(void)
{
    QTestState *s = start_profile(true, "");

    qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
    qtest_writeq(s, BASE + 0x38, 0xa1b2c3d4fffffffeULL);
    g_assert_cmphex(qtest_readq(s, BASE + 0x38), ==, 0xfffffffe);
    qtest_clock_step(s, 15);
    g_assert_cmphex(qtest_readq(s, BASE + 0x38), ==, 0xfffffffe);
    qtest_clock_step(s, 16);
    g_assert_cmphex(qtest_readq(s, BASE + 0x38), ==, 0);
    qtest_writeq(s, BASE + 0x38, 0xfedcba9865341278ULL);
    g_usleep(1000);
    g_assert_cmphex(qtest_readq(s, BASE + 0x38), ==, 0x65341278);
    qtest_clock_step(s, 1000000000);
    g_assert_cmphex(qtest_readq(s, BASE + 0x38), ==, 0x65341278 + 66666666);
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 0);
    assert_outputs(s, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, BASE + 0x38), ==, 0);
    qtest_quit(s);
}

static void test_migration(gconstpointer data)
{
    unsigned selection = GPOINTER_TO_UINT(data);
    bool legacy = selection & 1;
    bool functional = selection & 2;
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("crime-profile-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    const char *source_options = functional ?
        "-global sgi-crime.functional-memory-transfer=on" : "";
    g_autofree char *target_options = g_strdup_printf("%s -incoming defer",
        source_options);
    QTestState *from = start_profile(legacy, source_options);
    QTestState *to = start_profile(legacy, target_options);
    unsigned software_bit = legacy ? 30 : 29;
    uint32_t hardware = (1U << 4) | (1U << 16) | (1U << 22) |
                        (functional ? 1U << 25 : 0);

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_irq_intercept_out_named(to, CRIME_PATH, "cpu-ip");
    qtest_set_irq_in(from, CRIME_PATH, "mace", 4, 1);
    cause(from, 16, 1);
    cause(from, 22, 1);
    cause(from, 22, 0);
    if (functional) {
        cause(from, 25, 1);
        qtest_writeq(from, BASE + 0x18, UINT32_MAX);
    }
    qtest_writeq(from, BASE + 0x20, 1U << software_bit);
    qtest_writeq(from, BASE + 0x38, 0xfffffffe);
    qtest_clock_step(from, 15);
    qtest_writeq(from, 0x500, 0x88776655aabbccddULL);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    sgi_migrate_stopped(from, uri);
    sgi_wait_migration(to, "completed");
    g_assert_cmphex(qtest_readq(to, BASE + 0x28), ==, hardware);
    g_assert_cmphex(qtest_readq(to, BASE + 0x20), ==, 1U << software_bit);
    g_assert_cmphex(qtest_readq(to, BASE + 0x38), ==, 0xfffffffe);
    g_assert_cmphex(qtest_readq(to, 0x40000500), ==, 0x88776655aabbccddULL);
    g_assert_cmphex(qtest_readq(to, BASE + 0x18), ==,
                    functional ? UINT32_MAX : 0);
    assert_outputs(to, functional ? (legacy ? 1 : 0xb) : 0);
    qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readq(to, BASE + 0x38), ==, 0xffffffff);
    qtest_writeq(to, BASE + 0x18, hardware | (1U << software_bit));
    assert_outputs(to, legacy ? 1 : 0xb);
    qtest_writeq(to, BASE + 0x28, 0);
    g_assert_cmphex(qtest_readq(to, BASE + 0x28), ==, hardware & ~(1U << 22));
    cause(to, 16, 0);
    qtest_writeq(to, BASE + 0x28, 0);
    g_assert_cmphex(qtest_readq(to, BASE + 0x28), ==,
                    (1U << 4) | (functional ? 1U << 25 : 0));
    if (functional) {
        cause(to, 25, 0);
    }
    cause(to, 4, 0);
    qtest_writeq(to, BASE + 0x20, 0);
    qtest_writeq(to, BASE + 0x18, 0);
    exercise_sources(to, legacy);
    qtest_quit(from);
    qtest_quit(to);
    g_rmdir(directory);
}

static void test_clock_interrupt_independence(void)
{
    QTestState *s = qtest_init(MEMTX_TEST_DEVICE
        "-M o2-test,crime-profile=legacy-software,"
        "bank-profile=8x128m-synthetic,reset-low-ram-approx=on "
        "-cpu R10000 -m 1G -nodefaults -accel tcg,thread=single -S "
        "-device loader,addr=0xffffffffa0001000,cpu-num=0");
    uint32_t instructions[] = {
        mips_lui(8, 0x10), mips_mtc0(0, 9), mips_mtc0(8, 11),
        0, 0, 0, 0, mips_lui(10, 0xa000), mips_mfc0(9, 13),
        mips_sw(9, 10, 0x200), mips_mfc0(11, 9), mips_sw(11, 10, 0x208),
        mips_branch(MIPS_OPC_BEQ, 0, 0, -5), 0,
    };
    int64_t deadline;

    for (unsigned i = 0; i < ARRAY_SIZE(instructions); i++) {
        qtest_writel(s, 0x1000 + i * 4, instructions[i]);
    }
    qtest_set_irq_in(s, "/machine/crime", "external", 4, 1);
    qtest_writeq(s, BASE + 0x18, 1U << 20);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while ((qtest_readl(s, 0x200) & 0x8400) != 0x8400) {
        if (g_get_monotonic_time() >= deadline) {
            g_autofree char *registers = qtest_hmp(s, "info registers");

            g_test_message("cause marker: 0x%x, Count: 0x%x, "
                           "CRIME: 0x%" PRIx64 "; %s",
                           qtest_readl(s, 0x200), qtest_readl(s, 0x208),
                           qtest_readq(s, BASE + 0x10), registers);
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_set_irq_in(s, "/machine/crime", "external", 4, 0);
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while ((qtest_readl(s, 0x200) & 0x8400) != 0x8000) {
        if (g_get_monotonic_time() >= deadline) {
            g_autofree char *registers = qtest_hmp(s, "info registers");

            g_test_message("cause marker: 0x%x, Count: 0x%x, "
                           "CRIME: 0x%" PRIx64 "; %s",
                           qtest_readl(s, 0x200), qtest_readl(s, 0x208),
                           qtest_readq(s, BASE + 0x10), registers);
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    qtest_quit(s);
}

static void reject_machine(const char *machine, const char *ram,
                            const char *cpu, const char *option,
                            const char *value, const char *reason)
{
    const char *args[24];
    unsigned n = 0;

    args[n++] = qtest_qemu_binary(NULL);
    args[n++] = "-M";
    args[n++] = machine;
    args[n++] = "-m";
    args[n++] = ram;
    args[n++] = "-cpu";
    args[n++] = cpu;
    args[n++] = "-display";
    args[n++] = "none";
    args[n++] = "-nodefaults";
    args[n++] = "-S";
    args[n++] = "-qmp";
    args[n++] = "stdio";
    if (option) {
        args[n++] = option;
        args[n++] = value;
    }
    args[n] = NULL;
    assert_launch_rejected(args, reason);
}

static void test_machine_admission(void)
{
    const char *selected = "o2-test,crime-profile=legacy-software,"
                           "bank-profile=8x128m-synthetic";
    const char *choices[] = {
        "empty-renderer-status", "functional-memory-transfer"
    };
    g_autofree char *directory =
        g_dir_make_tmp("crime-profile-rom-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(directory, "synthetic.bin", NULL);
    g_autofree char *partial = NULL;
    const char image[] = { 0, 0, 0, 0 };

    reject_machine("o2-test,crime-profile=unknown", "64M", "R10000",
                   NULL, NULL, "crime-profile");
    reject_machine("o2-test,crime-profile=legacy-software", "64M", "R10000",
                   NULL, NULL, "legacy-software requires");
    reject_machine(selected, "128M", "R10000", NULL, NULL,
                   "legacy-software requires");
    reject_machine(selected, "1G", "R12000", NULL, NULL, "Invalid CPU model");
    reject_machine(selected, "1G", "R10000", "-smp", "2", "max CPUs");
    reject_machine(selected, "1G", "R10000,cache-data-visibility=on",
                   NULL, NULL, "requires cache-data-visibility=off");
    reject_machine(selected, "1G", "R10000", "-device",
                   "e1000,bus=mace.0,addr=3", "does not support attached PCI");
    reject_machine(selected, "1G", "R10000", "-bios", "unused",
                   "does not support -bios");
    g_assert_true(g_file_set_contents(path, image, sizeof(image), NULL));
    partial = g_strdup_printf("%s,password-clear-input=0,rom-image=%s,"
                              "rom-size=524288,rom-offset=0,"
                              "rom-pad-byte=238", selected, path);
    reject_machine(partial, "1G", "R10000", NULL, NULL,
                   "legacy-software ROM requires");
    for (unsigned i = 0; i < ARRAY_SIZE(choices); i++) {
        g_autofree char *complete = g_strdup_printf(
            "%s,reset-low-ram-approx=on,prom-private-window=on,"
            "prom-sb64-reset=on,prom-id-word=on,gbe11-registers=on,%s=on",
            partial, choices[i]);
        QTestState *s = qtest_initf(MEMTX_TEST_DEVICE
            "-M %s -m 1G -cpu R10000 -nodefaults -accel tcg,thread=single -S",
            complete);
        g_autoptr(QDict) machine = qtest_qmp(s,
            "{'execute':'qom-get','arguments':{'path':'/machine',"
            "'property':'crime-profile'}}");
        g_autoptr(QDict) child = qtest_qmp(s,
            "{'execute':'qom-get','arguments':{'path':'/machine/crime',"
            "'property':'crime-profile'}}");
        g_autoptr(QDict) change = qtest_qmp(s,
            "{'execute':'qom-set','arguments':{'path':'/machine',"
            "'property':'crime-profile','value':'1.5'}}");

        g_assert_cmpstr(qdict_get_str(machine, "return"),
                        ==, "legacy-software");
        g_assert_cmpstr(qdict_get_str(child, "return"), ==, "legacy-software");
        g_assert_nonnull(qdict_get_qdict(change, "error"));
        g_assert_cmphex(qtest_readq(s, BASE), ==, 0xa1);
        g_assert_cmphex(qtest_readl(s, BASE + 4), ==, 0xa1);
        qtest_quit(s);
    }
    g_unlink(path);
    g_rmdir(directory);
}

static void wait_failed(QTestState *s)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    for (;;) {
        g_autoptr(QDict) info =
            qtest_qmp_assert_success_ref(s, "{'execute':'query-migrate'}");

        if (!strcmp(qdict_get_str(info, "status"), "failed")) {
            g_assert_true(qdict_haskey(info, "error-desc"));
            return;
        }
        g_assert_cmpstr(qdict_get_str(info, "status"), !=, "completed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void test_profile_mismatch(gconstpointer data)
{
    bool source_legacy = GPOINTER_TO_INT(data);
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("crime-profile-id-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = start_profile(source_legacy, "");
    QTestState *to = start_profile(!source_legacy, "-incoming defer");

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_writeq(to, BASE + 0x18, 1U << 4);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s,'exit-on-error':false}}",
        uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_failed(to);
    g_assert_cmphex(qtest_readq(to, BASE), ==, source_legacy ? 0xb1 : 0xa1);
    g_assert_cmphex(qtest_readq(to, BASE + 0x18), ==, 1U << 4);
    qtest_quit(from);
    qtest_quit(to);
    g_rmdir(directory);
}

static void test_invalid_profile_state(gconstpointer data)
{
    unsigned fault = GPOINTER_TO_UINT(data);
    const char name[] = "sgi-crime/profile";
    const char section[] = "sgi-crime";

    for (unsigned legacy = 0; legacy < 2; legacy++) {
        GError *error = NULL;
        g_autofree char *directory =
            g_dir_make_tmp("crime-profile-state-XXXXXX", &error);
        g_autofree char *path = NULL;
        g_autofree char *quoted = NULL;
        g_autofree char *outgoing = NULL;
        g_autofree char *incoming = NULL;
        g_autofree char *stream = NULL;
        gsize size;
        char *state;
        QTestState *from = start_profile(legacy, "");
        QTestState *to = start_profile(legacy, "-incoming defer");

        g_assert_no_error(error);
        path = g_build_filename(directory, "synthetic.migration", NULL);
        quoted = g_shell_quote(path);
        outgoing = g_strdup_printf("exec:cat > %s", quoted);
        incoming = g_strdup_printf("exec:cat %s", quoted);
        qtest_irq_intercept_out_named(to, CRIME_PATH, "cpu-ip");
        cause(to, 4, 1);
        qtest_writeq(to, BASE + 0x18, 1U << 4);
        sgi_migrate_stopped(from, outgoing);
        g_assert_true(g_file_get_contents(path, &stream, &size, &error));
        g_assert_no_error(error);
        state = memmem(stream, size, name, sizeof(name) - 1);
        g_assert_nonnull(state);
        state += sizeof(name) - 1 + 4;
        switch (fault) {
        case 0: {
            char *first = state - 4 - (sizeof(name) - 1) - 2;
            size_t length = 2 + sizeof(name) - 1 + 4 + 31;

            memmove(first, first + length, size - (first - stream) - length);
            size -= length;
            break;
        }
        case 1:
            state[0] = 2;
            break;
        case 2:
            state[1] = 1;
            break;
        case 3:
            stl_be_p(state + 7, 1U << 4);
            break;
        case 4:
            stl_be_p(state + 19, 8);
            break;
        case 5:
            stl_be_p(state + 27, 0x40000000);
            break;
        case 6:
        case 7:
        case 8:
            state = memmem(stream, size, section, sizeof(section) - 1);
            g_assert_nonnull(state);
            state += sizeof(section) - 1;
            g_assert_cmphex(ldl_be_p(state + 4), ==, 2);
            if (fault == 6) {
                stl_be_p(state + 8, 1U << 20);
            } else if (fault == 7) {
                stl_be_p(state + 24, 1000000000);
            } else {
                stl_be_p(state + 4, 1);
            }
            break;
        default:
            g_assert_not_reached();
        }
        g_assert_true(g_file_set_contents(path, stream, size, &error));
        g_assert_no_error(error);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming',"
            "'arguments':{'uri':%s,'exit-on-error':false}}", incoming);
        wait_failed(to);
        g_assert_cmphex(qtest_readq(to, BASE), ==, legacy ? 0xa1 : 0xb1);
        g_assert_cmphex(qtest_readq(to, BASE + 0x18), ==, 1U << 4);
        g_assert_cmphex(qtest_readq(to, BASE + 0x28), ==, 1U << 4);
        assert_outputs(to, 1);
        qtest_quit(from);
        qtest_quit(to);
        g_unlink(path);
        g_rmdir(directory);
    }
}

static void test_register_access(void)
{
    const unsigned readable[] = {
        0, 8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48,
        0x200, 0x250, 0x258, 0x270,
    };
    const unsigned unsupported[] = { 0x248, 0x260, 0x268, 0x4048 };
    QTestState *s = start_profile(true, "");
    uint8_t byte;
    uint16_t half;
    uint32_t word;
    uint64_t wide;

    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    for (unsigned i = 0; i < ARRAY_SIZE(readable); i++) {
        uint64_t address = BASE + readable[i];
        uint64_t value = qtest_readq(s, address);

        g_assert_cmpint(memtx_readb(s, address, &byte), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readw(s, address, &half), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readl(s, address, &word), !=, MEMTX_OK);
        g_assert_cmpint(memtx_readq(s, address + 1, &wide), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writeb(s, address, 0), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writew(s, address, 0), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writel(s, address, 0), !=, MEMTX_OK);
        g_assert_cmpint(memtx_writeq(s, address + 1, 0), !=, MEMTX_OK);
        g_assert_cmphex(qtest_readq(s, address), ==, value);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(unsupported); i++) {
        g_assert_cmpint(memtx_readq(s, BASE + unsupported[i], &wide),
                        !=, MEMTX_OK);
        g_assert_cmpint(memtx_writeq(s, BASE + unsupported[i], 0),
                        !=, MEMTX_OK);
    }
    g_assert_cmpint(memtx_writeq(s, BASE + 0x200, 1), !=, MEMTX_OK);
    g_assert_cmpint(memtx_store(s, BASE + 0x270, 0x100, 8), !=, MEMTX_OK);
    g_assert_cmpint(memtx_writeq(s, BASE + 0x48, 1), !=, MEMTX_OK);
    assert_stopped(s);
    qtest_quit(s);
    s = start_profile(false, "");
    qtest_writeq(s, BASE + 0x20, 1U << 28);
    g_assert_cmpint(memtx_writeq(s, BASE + 0x20, 1U << 30), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, BASE + 0x20), ==, 1U << 28);
    qtest_quit(s);
}

static void test_banks(void)
{
    const uint64_t bank_size = 128ULL * 1024 * 1024;
    QTestState *s = qtest_init(MEMTX_TEST_DEVICE
        "-M none -cpu R10000 -S -object memory-backend-ram,id=fit,size=1G "
        "-device sgi-crime,id=crime,ram=/objects/fit/fit[0],"
        "crime-profile=legacy-software,eight-128m-banks=on,"
        "reset-low-ram-approx=on");
    uint8_t byte;
    g_autoptr(QDict) migration = NULL;

    qtest_sysbus_mmio_map(s, CRIME_PATH, 0, BASE);
    qtest_writeb(s, 0x40000100, 0x61);
    for (unsigned bank = 0; bank < 8; bank++) {
        qtest_writeq(s, BASE + 0x208 + bank * 8, 0x100 | (bank << 2));
        qtest_writeb(s, 0x40000100 + bank_size * bank, 0xa0 + bank);
    }
    for (unsigned bank = 0; bank < 8; bank++) {
        g_assert_cmphex(qtest_readb(s, 0x80000100 + bank_size * bank),
                        ==, 0xa0 + bank);
        if (bank < 2 || bank >= 4) {
            g_assert_cmphex(qtest_readb(s, 0x100 + bank_size * bank),
                            ==, 0xa0 + bank);
        }
    }
    qtest_writeq(s, BASE + 0x208, 0x104);
    g_assert_cmphex(qtest_readb(s, 0x40000100 + bank_size), ==, 0xa0);
    g_assert_cmpint(memtx_readb(s, 0x40000100, &byte), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, BASE + 0x250), ==, 1U << 25);
    g_assert_cmphex(qtest_readq(s, BASE + 0x258), ==, 0x100);
    qtest_writeq(s, BASE + 0x208, 0x100);
    g_assert_cmphex(qtest_readb(s, 0x40000100), ==, 0xa0);
    g_assert_cmphex(qtest_readb(s, 0x40000100 + bank_size), ==, 0xa1);
    qtest_writeq(s, BASE + 0x208, 0);
    g_assert_cmpint(memtx_writeb(s, 0x40000100, 0xff), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, BASE + 0x250), ==, 1U << 25);
    g_assert_cmphex(qtest_readq(s, BASE + 0x258), ==, 0x100);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readb(s, 0x40000100), ==, 0xa0);
    qtest_writeq(s, BASE + 0x210, 1);
    g_assert_cmpint(memtx_readb(s, 0x40000100, &byte), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readq(s, BASE + 0x250), ==, 0);
    qtest_system_reset(s);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    g_assert_cmpint(memtx_writeq(s, BASE + 0x208, 0x120), !=, MEMTX_OK);
    g_assert_cmphex(qtest_readb(s, 0x40000100), ==, 0xa0);
    assert_stopped(s);
    migration = qtest_qmp(s, "{'execute':'migrate',"
        "'arguments':{'uri':'unix:/no-crime-legacy-migration'}}");
    g_assert_nonnull(qdict_get_qdict(migration, "error"));
    g_assert_nonnull(strstr(qdict_get_str(qdict_get_qdict(migration, "error"),
                                         "desc"), "non-migratable device"));
    qtest_quit(s);
}

static void test_immutable(void)
{
    QTestState *s = start_profile(true, "");
    g_autoptr(QDict) change = qtest_qmp(s,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'crime-profile','value':'1.5'}}", CRIME_PATH);
    g_autoptr(QDict) unrealized_change = NULL;

    g_assert_nonnull(qdict_get_qdict(change, "error"));
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'realized','value':false}}", CRIME_PATH);
    unrealized_change = qtest_qmp(s,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'crime-profile','value':'1.5'}}", CRIME_PATH);
    g_assert_nonnull(qdict_get_qdict(unrealized_change, "error"));
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':%s,"
        "'property':'realized','value':true}}", CRIME_PATH);
    g_assert_cmphex(qtest_readq(s, BASE), ==, 0xa1);
    qtest_quit(s);
}

static void test_feature_mismatch(gconstpointer data)
{
    const char *property = data;

    for (unsigned legacy = 0; legacy < 2; legacy++) {
        GError *error = NULL;
        g_autofree char *directory =
            g_dir_make_tmp("crime-feature-id-XXXXXX", &error);
        g_autofree char *socket = NULL;
        g_autofree char *uri = NULL;
        g_autofree char *option =
            g_strdup_printf("-global sgi-crime.%s=on", property);
        QTestState *from = start_profile(legacy, option);
        QTestState *to = start_profile(legacy, "-incoming defer");

        g_assert_no_error(error);
        socket = g_build_filename(directory, "migration.sock", NULL);
        uri = g_strdup_printf("unix:%s", socket);
        qtest_writeq(to, BASE + 0x18, 1U << 4);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming',"
            "'arguments':{'uri':%s,'exit-on-error':false}}", uri);
        qtest_qmp_assert_success(from,
            "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        wait_failed(to);
        g_assert_cmphex(qtest_readq(to, BASE + 0x18), ==, 1U << 4);
        qtest_quit(from);
        qtest_quit(to);
        g_rmdir(directory);
    }
}

#define RENDERER 0x15000000ULL
#define LINEAR_TLB (RENDERER + 0x1700)
#define TRANSFER (RENDERER + 0x3000)

static void assert_run_status(QTestState *s, const char *expected)
{
    g_autoptr(QDict) status =
        qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");

    g_assert_cmpstr(qdict_get_str(status, "status"), ==, expected);
}

static void test_interrupt_mask_upper(void)
{
    static const uint64_t upper[] = {
        0, 0x0000000100000000ULL, 0xa1b2c3d400000000ULL,
        0xffffffff00000000ULL,
    };
    static const uint32_t masks[] = { 0, 1U << 4, 1U << 8, 1U << 30 };
    g_autofree char *directory = g_dir_make_tmp("crime-mask-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(directory, "device.txt", NULL);

    for (unsigned functional = 0; functional < 2; functional++) {
        g_autofree char *options = g_strdup_printf("%s -D %s",
            functional ? "-global sgi-crime.functional-memory-transfer=on" : "",
            path);
        g_autofree char *contents = NULL;
        QTestState *s = start_profile(true, options);

        qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
        cause(s, 4, 1);
        qtest_writeq(s, BASE + 0x20, 1U << 30);
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
        for (unsigned u = 0; u < ARRAY_SIZE(upper); u++) {
            qtest_writeq(s, BASE + 0x18, 1U << 4);
            for (unsigned m = 0; m < ARRAY_SIZE(masks); m++) {
                g_assert_cmpint(memtx_writeq(s, BASE + 0x18,
                                            upper[u] | masks[m]), ==, MEMTX_OK);
                g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, masks[m]);
                assert_outputs(s, masks[m] && masks[m] != (1U << 8));
                assert_run_status(s, "running");
            }
        }
        for (unsigned u = 0; u < ARRAY_SIZE(upper); u++) {
            for (unsigned bit = 0; bit < 32; bit++) {
                uint32_t mask = (1U << 4) | (1U << 30) | (1U << bit);

                g_assert_cmpint(memtx_writeq(s, BASE + 0x18, upper[u] | mask),
                                ==, MEMTX_OK);
                g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, mask);
                g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==,
                                (1U << 4) | (1U << 30));
                assert_outputs(s, 1);
                assert_run_status(s, "running");
            }
        }
        qtest_quit(s);
        g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
        g_assert_nonnull(strstr(contents,
                               "ignoring legacy interrupt-mask upper bits"));
    }
    g_unlink(path);
    g_rmdir(directory);
}

static void test_interrupt_mask_upper_refusal(void)
{
    static const uint64_t upper[] = {
        0x0000000100000000ULL, 0xa1b2c3d400000000ULL,
        0xffffffff00000000ULL,
    };

    for (unsigned functional = 0; functional < 2; functional++) {
        for (unsigned u = 0; u < ARRAY_SIZE(upper); u++) {
            QTestState *s = start_profile(false, functional ?
                "-global sgi-crime.functional-memory-transfer=on" : "");

            qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
            cause(s, 4, 1);
            qtest_writeq(s, BASE + 0x18, UINT32_MAX);
            g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, UINT32_MAX);
            g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1U << 4);
            assert_outputs(s, 1);
            qtest_qmp_assert_success(s, "{'execute':'cont'}");
            g_assert_cmpint(memtx_writeq(s, BASE + 0x18,
                                        upper[u] | (1U << 8)), ==, MEMTX_ERROR);
            g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, UINT32_MAX);
            g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1U << 4);
            g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 1U << 4);
            assert_outputs(s, 1);
            assert_run_status(s, functional ? "internal-error" : "running");
            qtest_quit(s);
        }
    }
}

static void seed_renderer_state(QTestState *s)
{
    const unsigned parameters[] = { 0, 8, 0x18, 0x20, 0x28, 0x40, 0x48 };

    /* Refusal probes need a shape that remains unsupported. */
    qtest_writel(s, RENDERER + 0x2060, 0x02000000);
    qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
    qtest_writeq(s, BASE + 0x18, 1U << 4);
    cause(s, 4, 1);
    for (unsigned i = 0; i < 32; i++) {
        qtest_writeq(s, LINEAR_TLB + 8 * i, 0x8000000380000009ULL + i);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(parameters); i++) {
        qtest_writel(s, TRANSFER + parameters[i], 0x12340000 + i);
    }
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
        "'property':'cpu-error-address','value':305419896}}");
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
        "'property':'cpu-error-status','value':5}}");
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
        "'property':'memory-error-address','value':124076833}}");
    qtest_qmp_assert_success(s,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
        "'property':'memory-error-status','value':234881024}}");
    qtest_writel(s, TRANSFER + 0x30, 7);
    qtest_writel(s, TRANSFER + 0x38, 15);
    qtest_memset(s, 0x3000, 0x5a, 64);
}

static void assert_renderer_state(QTestState *s)
{
    const unsigned parameters[] = { 0, 8, 0x18, 0x20, 0x28, 0x40, 0x48 };

    for (unsigned i = 0; i < 32; i++) {
        g_assert_cmphex(qtest_readq(s, LINEAR_TLB + 8 * i),
                        ==, 0x8000000380000009ULL + i);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(parameters); i++) {
        g_assert_cmphex(qtest_readl(s, TRANSFER + parameters[i]),
                        ==, 0x12340000 + i);
    }
    for (unsigned i = 0; i < 64; i++) {
        g_assert_cmphex(qtest_readb(s, 0x3000 + i), ==, 0x5a);
    }
    g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1U << 4);
    g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, 1U << 4);
    g_assert_cmphex(qtest_readq(s, BASE + 0x40), ==, 0x12345678);
    g_assert_cmphex(qtest_readq(s, BASE + 0x48), ==, 5);
    g_assert_cmphex(qtest_readq(s, BASE + 0x250), ==, 7U << 25);
    g_assert_cmphex(qtest_readq(s, BASE + 0x258), ==, 0x7654321);
    assert_outputs(s, 1);
}

typedef struct RendererAccess {
    unsigned offset;
    unsigned size;
    bool write;
} RendererAccess;

/* A valid clear exposes any change to the two write-only bounds. */
static void assert_renderer_bounds(QTestState *s)
{
    qtest_writel(s, TRANSFER + 8, UINT32_MAX);
    qtest_writel(s, TRANSFER + 0x18, 0x6b);
    g_assert_cmpint(memtx_writel(s, TRANSFER + 0x800, 4U << 2), ==, MEMTX_OK);
    for (unsigned i = 0; i < 64; i++) {
        g_assert_cmphex(qtest_readb(s, 0x3000 + i),
                        ==, i >= 7 && i <= 15 ? 0x6b : 0x5a);
    }
    g_assert_cmphex(qtest_readl(s, RENDERER + 0x4000), ==, 0x1e000000);
}

static const unsigned ifb_parameters[] = {
    0, 8, 0x10, 0x18, 0x50, 0x58, 0xd8, 0x1b0, 0x1b8,
};
static const uint32_t ifb_values[] = {
    0x124, 0, 0x210, 0x1002f8, 0, 0, 0x89abcdef, 6, 0x0f0f0f0f,
};

static void seed_ifb_state(QTestState *s)
{
    seed_renderer_state(s);
    for (unsigned i = 0; i < ARRAY_SIZE(ifb_parameters); i++) {
        qtest_writel(s, RENDERER + 0x2000 + ifb_parameters[i], ifb_values[i]);
    }
    for (unsigned i = 0; i < 6; i++) {
        qtest_writeq(s, RENDERER + 0x2020 + i * 8,
                     0x0001000200090007ULL + ((uint64_t)i << 16));
    }
    qtest_writeq(s, RENDERER + 0x1000, 0x8003000000000000ULL);
    qtest_writeq(s, RENDERER + 0x1200, 0x8123456789abcdefULL);
    qtest_writel(s, RENDERER + 0x2060, 0x00031234);
    qtest_writel(s, RENDERER + 0x2070, 0x00030002);
    qtest_writel(s, RENDERER + 0x2074, UINT32_MAX);
    qtest_writel(s, RENDERER + 0x20d0, 0x123456cc);
    qtest_writeq(s, 0x30400, 0x1122335a55667788ULL);
}

static void assert_ifb_state(QTestState *s, bool executed)
{
    assert_renderer_state(s);
    for (unsigned i = 0; i < ARRAY_SIZE(ifb_parameters); i++) {
        g_assert_cmphex(qtest_readl(s, RENDERER + 0x2000 + ifb_parameters[i]),
                       ==, ifb_values[i]);
    }
    for (unsigned i = 0; i < 6; i++) {
        g_assert_cmphex(qtest_readq(s, RENDERER + 0x2020 + i * 8), ==,
                       0x0001000200090007ULL + ((uint64_t)i << 16));
    }
    g_assert_cmphex(qtest_readq(s, RENDERER + 0x1000), ==,
                   0x8003000000000000ULL);
    g_assert_cmphex(qtest_readq(s, RENDERER + 0x1200), ==,
                   0x8123456789abcdefULL);
    g_assert_cmphex(qtest_readq(s, 0x30400), ==,
                   executed ? 0x1122335655667788ULL : 0x1122335a55667788ULL);
    g_assert_cmphex(qtest_readl(s, RENDERER + 0x4000), ==, 0x1e000000);
}

static void test_renderer_no_generated_irq(void)
{
    QTestState *s = start_profile(true,
        "-global sgi-crime.functional-memory-transfer=on "
        "-device sgi-crime-test-fixture,id=inject,crime=" CRIME_PATH);

    seed_ifb_state(s);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    g_assert_cmpint(memtx_writeq(s, BASE + 0x18, UINT32_MAX), ==, MEMTX_OK);
    for (unsigned operation = 0; operation < 6; operation++) {
        switch (operation) {
        case 0:
            g_assert_cmphex(qtest_readl(s, RENDERER + 0x4000), ==, 0x1e000000);
            break;
        case 1:
            g_assert_cmpint(memtx_writel(s, RENDERER + 0x400, 0xa6c319e7),
                            ==, MEMTX_OK);
            break;
        case 2:
            g_assert_cmpint(memtx_writel(s, RENDERER + 0x4008, UINT32_MAX),
                            ==, MEMTX_OK);
            break;
        case 3:
            g_assert_cmpint(memtx_writel(s, RENDERER + 0x29f0, 0x12345678),
                            ==, MEMTX_OK);
            g_assert_cmphex(qtest_readq(s, 0x30400), ==, 0x1122335655667788ULL);
            break;
        case 4:
            qtest_writel(s, TRANSFER, 4U << 2);
            qtest_writel(s, TRANSFER + 8, UINT32_MAX);
            qtest_writel(s, TRANSFER + 0x18, 0x6d);
            qtest_writel(s, TRANSFER + 0x30, 16);
            g_assert_cmpint(memtx_writel(s, TRANSFER + 0x838, 31),
                            ==, MEMTX_OK);
            for (unsigned i = 16; i < 32; i++) {
                g_assert_cmphex(qtest_readb(s, 0x3000 + i), ==, 0x6d);
            }
            break;
        case 5:
            g_assert_cmpint(memtx_writel(s, TRANSFER + 0x78, 1),
                            ==, MEMTX_ERROR);
            break;
        }
        g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, UINT32_MAX);
        g_assert_cmphex(qtest_readq(s, BASE + 0x10), ==, 1U << 4);
        g_assert_cmphex(qtest_readq(s, BASE + 0x28), ==, 1U << 4);
        assert_outputs(s, 1);
        assert_run_status(s, operation == 5 ? "internal-error" : "running");
    }
    qtest_quit(s);
}

static void test_ifb_read(void)
{
    g_autofree char *directory = g_dir_make_tmp("crime-ifb-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(directory, "device.txt", NULL);
    g_autofree char *options = g_strdup_printf(
        "-global sgi-crime.functional-memory-transfer=on -D %s "
        "-device sgi-crime-test-fixture,id=inject,crime=" CRIME_PATH, path);
    QTestState *s = start_profile(true, options);
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    uint32_t value;
    unsigned count = 0;

    seed_ifb_state(s);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    for (unsigned i = 0; i < 20; i++) {
        if (i == 16) {
            qtest_writel(s, RENDERER + 0x29f0, 0x12345678);
        }
        qtest_clock_step(s, 997 + i * 101);
        value = UINT32_MAX;
        g_assert_cmpint(memtx_readl(s, RENDERER + 0x400, &value), ==, MEMTX_OK);
        g_assert_cmphex(value, ==, 0);
        assert_run_status(s, "running");
        assert_ifb_state(s, i >= 16);
    }
    qtest_system_reset(s);
    g_assert_cmpint(memtx_readl(s, RENDERER + 0x400, &value), ==, MEMTX_OK);
    g_assert_cmphex(value, ==, 0);
    g_assert_cmphex(qtest_readl(s, RENDERER + 0x2018), ==, 0);
    g_assert_cmphex(qtest_readq(s, RENDERER + 0x1000), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, 0);
    g_assert_cmphex(qtest_readq(s, 0x30400), ==, 0x1122335655667788ULL);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    lines = g_strsplit(contents, "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        if (strstr(lines[i],
                   "sgi-crime: undefined interface-buffer diagnostic")) {
            g_assert_nonnull(strstr(lines[i], "4-byte read"));
            g_assert_nonnull(strstr(lines[i], "0x15000400"));
            count++;
        }
    }
    g_assert_cmpuint(count, ==, 21);
    g_unlink(path);
    g_rmdir(directory);
}

static void test_opaque_renderer_write(gconstpointer data)
{
    static const uint32_t payloads[] = {
        0, UINT32_MAX, 0x13579bdf, 0xa6c319e7,
    };
    bool status_bank = GPOINTER_TO_UINT(data);
    unsigned offset = status_bank ? 0x4008 : 0x400;
    const char *diagnostic = status_bank ?
        "opaque engine-status-bank" : "opaque empty interface-buffer control";
    const char *limitation = status_bank ? "undefined semantics" :
                                         "unimplemented fields";
    g_autofree char *address = g_strdup_printf("0x%" PRIx64,
                                              (uint64_t)(RENDERER + offset));
    g_autofree char *directory =
        g_dir_make_tmp("crime-opaque-write-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(directory, "device.txt", NULL);
    g_autofree char *options = g_strdup_printf(
        "-global sgi-crime.functional-memory-transfer=on -D %s "
        "-device sgi-crime-test-fixture,id=inject,crime=" CRIME_PATH, path);
    QTestState *s = start_profile(true, options);
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    unsigned count = 0;

    seed_ifb_state(s);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    for (unsigned phase = 0; phase < 2; phase++) {
        if (phase) {
            qtest_writel(s, RENDERER + 0x29f0, 0x12345678);
        }
        for (unsigned i = 0; i < ARRAY_SIZE(payloads); i++) {
            uint32_t value = UINT32_MAX;

            qtest_clock_step(s, 997 + i * 101);
            g_assert_cmpint(memtx_writel(s, RENDERER + offset, payloads[i]),
                            ==, MEMTX_OK);
            g_assert_cmpint(memtx_readl(s, RENDERER +
                (status_bank ? 0x4000 : 0x400), &value),
                            ==, MEMTX_OK);
            g_assert_cmphex(value, ==, status_bank ? 0x1e000000 : 0);
            assert_run_status(s, "running");
            assert_ifb_state(s, phase);
        }
    }
    qtest_system_reset(s);
    g_assert_cmpint(memtx_writel(s, RENDERER + offset, 0x13579bdf),
                    ==, MEMTX_OK);
    g_assert_cmphex(qtest_readl(s, RENDERER + 0x2018), ==, 0);
    g_assert_cmphex(qtest_readq(s, RENDERER + 0x1000), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x18), ==, 0);
    g_assert_cmphex(qtest_readq(s, 0x30400), ==, 0x1122335655667788ULL);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    lines = g_strsplit(contents, "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        g_autofree char *payload = NULL;
        uint32_t expected;

        if (!strstr(lines[i], diagnostic)) {
            continue;
        }
        expected = count < 2 * ARRAY_SIZE(payloads) ?
            payloads[count % ARRAY_SIZE(payloads)] : 0x13579bdf;
        payload = g_strdup_printf("value 0x%" PRIx32, expected);
        g_assert_nonnull(strstr(lines[i], limitation));
        g_assert_nonnull(strstr(lines[i], "4-byte write"));
        g_assert_nonnull(strstr(lines[i], address));
        g_assert_true(g_str_has_suffix(lines[i], payload));
        count++;
    }
    g_assert_cmpuint(count, ==, 2 * ARRAY_SIZE(payloads) + 1);
    g_unlink(path);
    g_rmdir(directory);
}

static void test_status_bank_refusal(void)
{
    static const RendererAccess probes[] = {
        { 0x4000, 1, false }, { 0x4000, 2, false }, { 0x4000, 8, false },
        { 0x4001, 4, false }, { 0x4000, 4, true },
        { 0x4008, 1, true }, { 0x4008, 2, true }, { 0x4008, 8, true },
        { 0x4009, 4, true }, { 0x4007, 2, true },
        { 0x4008, 1, false }, { 0x4008, 2, false }, { 0x4008, 4, false },
        { 0x4008, 8, false },
        { 0x3ffc, 8, true }, { 0x4000, 8, true }, { 0x4004, 8, true },
        { 0x4ffc, 8, true },
        { 0x4004, 4, true }, { 0x4004, 4, false },
        { 0x4010, 4, true }, { 0x4010, 4, false },
        { 0x4080, 4, true }, { 0x4080, 4, false },
        { 0x4808, 4, true }, { 0x4808, 4, false },
        { 0x4ffc, 4, true }, { 0x4ffc, 4, false },
    };
    g_autofree char *directory =
        g_dir_make_tmp("crime-status-bank-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(directory, "device.txt", NULL);
    g_autofree char *options = g_strdup_printf(
        "-global sgi-crime.functional-memory-transfer=on -D %s "
        "-device sgi-crime-test-fixture,id=inject,crime=" CRIME_PATH, path);

    for (unsigned i = 0; i < ARRAY_SIZE(probes); i++) {
        const RendererAccess *probe = &probes[i];

        for (unsigned n = 0; n < (probe->write ? 2 : 1); n++) {
            QTestState *s = start_profile(true, options);
            uint64_t value = n ? 0xa6c319e7d51b0842ULL : 0;
            uint64_t readback;
            g_autofree char *contents = NULL;
            MemTxResult result;

            seed_ifb_state(s);
            qtest_qmp_assert_success(s, "{'execute':'cont'}");
            result = probe->write ?
                memtx_write(s, RENDERER + probe->offset, value, probe->size) :
                memtx_read(s, RENDERER + probe->offset, &readback, probe->size);
            g_assert_cmpint(result, ==, MEMTX_ERROR);
            assert_stopped(s);
            assert_ifb_state(s, false);
            qtest_quit(s);
            g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
            g_assert_null(strstr(contents, "opaque engine-status-bank"));
        }
    }
    g_unlink(path);
    g_rmdir(directory);
}

static void test_status_bank_profiles(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned functional = 0; functional < 2; functional++) {
            QTestState *s = start_profile(legacy, functional ?
                "-global sgi-crime.functional-memory-transfer=on" :
                "-global sgi-crime.empty-renderer-status=on");
            bool admitted = legacy && functional;

            qtest_qmp_assert_success(s, "{'execute':'cont'}");
            g_assert_cmphex(qtest_readl(s, RENDERER + 0x4000), ==, 0x1e000000);
            g_assert_cmpint(memtx_writel(s, RENDERER + 0x4008, 0xa6c319e7),
                            ==, admitted ? MEMTX_OK : MEMTX_ERROR);
            assert_run_status(s, !admitted && (legacy || functional) ?
                              "internal-error" : "running");
            g_assert_cmphex(qtest_readl(s, RENDERER + 0x4000), ==, 0x1e000000);
            qtest_quit(s);
        }
    }
}

static void test_ifb_refusal(void)
{
    const struct {
        RendererAccess access;
        bool hard;
    } probes[] = {
        { { 0x400, 1, false }, false }, { { 0x400, 2, false }, false },
        { { 0x400, 8, false }, false }, { { 0x401, 4, false }, false },
        { { 0x402, 4, false }, false }, { { 0x3ff, 2, false }, false },
        { { 0x3fc, 8, false }, false }, { { 0x3fc, 4, false }, false },
        { { 0x404, 4, false }, false }, { { 0x408, 4, false }, false },
        { { 0x40c, 4, false }, false }, { { 0x000, 8, false }, false },
        { { 0x200, 8, false }, false }, { { 0x4008, 4, false }, true },
        { { 0x4000, 8, false }, true },
        { { 0x400, 1, true }, true }, { { 0x400, 2, true }, true },
        { { 0x401, 4, true }, true }, { { 0x400, 8, true }, true },
        { { 0x3fc, 8, true }, true }, { { 0x3ff, 2, true }, true },
        { { 0x401, 1, true }, true }, { { 0x403, 1, true }, true },
        { { 0x408, 4, true }, false },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(probes); i++) {
        const RendererAccess *a = &probes[i].access;

        for (unsigned n = 0; n < (a->write ? 2 : 1); n++) {
            QTestState *s = start_profile(true,
                "-global sgi-crime.functional-memory-transfer=on "
                "-device sgi-crime-test-fixture,id=inject,crime=" CRIME_PATH);
            uint64_t value = n ? 0x1122334455667788ULL : 0;
            uint64_t readback;
            MemTxResult result;

            seed_ifb_state(s);
            qtest_qmp_assert_success(s, "{'execute':'cont'}");
            result = a->write ?
                memtx_write(s, RENDERER + a->offset, value, a->size) :
                memtx_read(s, RENDERER + a->offset, &readback, a->size);
            g_assert_cmpint(result, ==, MEMTX_ERROR);
            assert_run_status(s, probes[i].hard ? "internal-error" : "running");
            assert_ifb_state(s, false);
            qtest_quit(s);
        }
    }
}

static void test_ifb_profiles(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned functional = 0; functional < 2; functional++) {
            for (unsigned write = 0; write < 2; write++) {
                QTestState *s = start_profile(legacy, functional ?
                    "-global sgi-crime.functional-memory-transfer=on" : "");
                uint32_t value = UINT32_MAX;
                MemTxResult result;

                qtest_qmp_assert_success(s, "{'execute':'cont'}");
                result = write ? memtx_writel(s, RENDERER + 0x400, 0xa6c319e7) :
                                 memtx_readl(s, RENDERER + 0x400, &value);
                g_assert_cmpint(result, ==,
                                legacy && functional ? MEMTX_OK : MEMTX_ERROR);
                assert_run_status(s, legacy != functional ?
                                  "internal-error" : "running");
                if (legacy && functional && !write) {
                    g_assert_cmphex(value, ==, 0);
                }
                qtest_quit(s);
            }
        }
    }
}

static void test_renderer_refusal(void)
{
    const RendererAccess accesses[] = {
        { 0x408, 4, true }, { 0x600, 8, true },
        { 0x2080, 8, true }, { 0x2081, 4, true },
        { 0x2080, 4, false }, { 0x2084, 1, true },
        { 0x2400, 4, true }, { 0x408, 4, false },
        { 0x21f0, 4, false }, { 0x21f8, 4, false },
        { 0x27fc, 4, true },
        { 0x3080, 4, true }, { 0x37fc, 4, true },
    };
    g_autofree char *directory = g_dir_make_tmp("crime-renderer-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(directory, "device.txt", NULL);
    g_autofree char *options = g_strdup_printf(
        "-global sgi-crime.functional-memory-transfer=on -D %s "
        "-device sgi-crime-test-fixture,id=inject,crime=" CRIME_PATH, path);
    QTestState *s = start_profile(true, options);
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    unsigned count = 0;

    seed_renderer_state(s);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    for (unsigned i = 0; i < ARRAY_SIZE(accesses); i++) {
        const RendererAccess *a = &accesses[i];
        uint64_t value;
        MemTxResult result = a->write ?
            memtx_write(s, RENDERER + a->offset, 0x12345678, a->size) :
            memtx_read(s, RENDERER + a->offset, &value, a->size);

        g_assert_cmpint(result, ==, MEMTX_ERROR);
        assert_run_status(s, "running");
        assert_renderer_state(s);
    }
    assert_renderer_bounds(s);
    qtest_quit(s);
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    lines = g_strsplit(contents, "\n", -1);
    for (unsigned i = 0; lines[i]; i++) {
        const RendererAccess *a;
        g_autofree char *address = NULL;
        g_autofree char *width = NULL;

        if (!strstr(lines[i], "sgi-crime: unsupported renderer")) {
            continue;
        }
        g_assert_cmpuint(count, <, ARRAY_SIZE(accesses));
        a = &accesses[count++];
        address = g_strdup_printf("0x%" PRIx64,
                                  (uint64_t)(RENDERER + a->offset));
        width = g_strdup_printf("%u-byte", a->size);
        g_assert_nonnull(strstr(lines[i], address));
        g_assert_nonnull(strstr(lines[i], width));
        g_assert_nonnull(strstr(lines[i], a->write ? "write" : "read"));
        if (a->write) {
            g_autofree char *supplied = g_strdup_printf("value 0x%x",
                a->size == 1 ? 0x78 : 0x12345678);

            g_assert_nonnull(strstr(lines[i], supplied));
        }
    }
    g_assert_cmpuint(count, ==, ARRAY_SIZE(accesses));
    g_unlink(path);
    g_rmdir(directory);
}

static void check_renderer_hard_stop(const RendererAccess *a)
{
    QTestState *s = start_profile(true,
        "-global sgi-crime.functional-memory-transfer=on "
        "-device sgi-crime-test-fixture,id=inject,crime=" CRIME_PATH);
    uint64_t value;
    MemTxResult result;

    seed_renderer_state(s);
    g_test_message("renderer %u-byte %s at +0x%x", a->size,
                   a->write ? "write" : "read", a->offset);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    result = a->write ?
        memtx_write(s, RENDERER + a->offset, 0x12345678, a->size) :
        memtx_read(s, RENDERER + a->offset, &value, a->size);
    g_assert_cmpint(result, ==, MEMTX_ERROR);
    assert_stopped(s);
    assert_renderer_state(s);
    assert_renderer_bounds(s);
    qtest_quit(s);
}

static void test_renderer_hard_stop(void)
{
    const RendererAccess accesses[] = {
        { 0x400, 8, true }, { 0x2880, 4, true },
        { 0x21f0, 8, true }, { 0x21f8, 4, true },
        { 0x3070, 8, true }, { 0x3078, 4, true },
        { 0x3808, 4, true }, { 0x3800, 4, true },
        { 0x3838, 4, true },
        { 0x3000, 8, true }, { 0x3001, 4, true },
        { 0x1700, 4, true }, { 0x16ff, 4, true }, { 0x16fc, 4, true },
        { 0x3030, 4, false }, { 0x4000, 4, true },
        { 0x2fff, 8, true }, { 0x3fff, 8, true },
        { 0x287f, 4, true }, { 0x3ff, 2, true },
        { 0x5000, 4, true }, { 0x4fff, 2, true },
        { 0x4ff8, 8, true }, { 0x4004, 4, false },
        { 0x3010, 1, true }, { 0x3050, 4, true },
        { 0x3810, 4, true }, { 0x3818, 4, true },
        { 0x3820, 4, true }, { 0x3828, 4, true },
        { 0x3830, 4, true }, { 0x3840, 4, true },
        { 0x3848, 4, true }, { 0x281c, 8, true },
        { 0x3804, 8, true }, { 0x3fc, 8, true },
        { 0x2804, 4, true }, { 0x29f0, 4, true },
        { 0x29f8, 4, true }, { 0x3870, 4, true },
        { 0x3878, 4, true }, { 0x2ffc, 4, true },
        { 0x2880, 4, false }, { 0x3808, 4, false },
        { 0x3800, 4, false }, { 0x3838, 4, false },
        { 0x2804, 4, false }, { 0x29f0, 4, false },
        { 0x3870, 4, false },
        { 0x27ff, 2, true }, { 0x37ff, 2, true },
    };
    const unsigned pixel_go[] = {
        0x000, 0x008, 0x010, 0x018, 0x020, 0x028, 0x030, 0x038,
        0x040, 0x048, 0x050, 0x058, 0x060, 0x070, 0x074, 0x078,
        0x080, 0x084, 0x088, 0x08c, 0x090, 0x094, 0x0a0, 0x0a8,
        0x0ac, 0x0b0, 0x0b4, 0x0c0, 0x0c4, 0x0d0, 0x0d8, 0x0e0,
        0x0e4, 0x0e8, 0x0ec, 0x0f0, 0x0f4, 0x0f8, 0x0fc, 0x100,
        0x104, 0x108, 0x10c, 0x110, 0x118, 0x120, 0x128, 0x130,
        0x138, 0x140, 0x148, 0x150, 0x158, 0x15c, 0x160, 0x168,
        0x170, 0x178, 0x180, 0x188, 0x190, 0x194, 0x198, 0x1a0,
        0x1a8, 0x1b0, 0x1b8, 0x1c0, 0x1c8, 0x1d0, 0x1d8, 0x1e0,
        0x1e8,
    };
    const unsigned pixel_wide[] = {
        0x020, 0x028, 0x030, 0x038, 0x040, 0x048, 0x118, 0x120,
        0x128, 0x138, 0x140, 0x148, 0x150, 0x1c8, 0x1d0, 0x1d8,
    };

    for (unsigned i = 0; i < ARRAY_SIZE(accesses); i++) {
        check_renderer_hard_stop(&accesses[i]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(pixel_go); i++) {
        RendererAccess a = { .offset = 0x2800 + pixel_go[i],
                             .size = 4, .write = true };

        for (unsigned j = 0; j < ARRAY_SIZE(pixel_wide); j++) {
            if (pixel_go[i] == pixel_wide[j]) {
                a.size = 8;
            }
        }
        check_renderer_hard_stop(&a);
    }
}

/* Observe write-only programming through the existing migration encoding. */
static uint32_t saved_transfer_mask(QTestState *s, const char *path)
{
    const char name[] = "sgi-crime/linear-transfer";
    g_autofree char *quoted = g_shell_quote(path);
    g_autofree char *uri = g_strdup_printf("exec:cat > %s", quoted);
    g_autofree char *stream = NULL;
    gsize size;
    char *state;

    sgi_migrate_stopped(s, uri);
    g_assert_true(g_file_get_contents(path, &stream, &size, NULL));
    state = memmem(stream, size, name, sizeof(name) - 1);
    g_assert_nonnull(state);
    state += sizeof(name) - 1;
    g_assert_cmphex(ldl_be_p(state), ==, 1);
    /* Leave postmigration so the same qtest fixture can be observed again. */
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    qtest_qmp_assert_success(s, "{'execute':'stop'}");
    /* Version, two policy bytes, translation bank, then parameter slot 2. */
    return ldl_be_p(state + 4 + 2 + 256 + 8);
}

static void test_transfer_mask_stage(void)
{
    const uint32_t masks[] = { 0, UINT32_MAX, 0x80402001, 0x13579bdf, 0 };
    g_autofree char *dir = g_dir_make_tmp("crime-stipple-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);
    QTestState *s = start_profile(true,
        "-global sgi-crime.functional-memory-transfer=on "
        "-device sgi-crime-test-fixture,id=inject,crime=" CRIME_PATH);

    seed_renderer_state(s);
    for (unsigned i = 0; i < ARRAY_SIZE(masks); i++) {
        g_assert_cmpint(memtx_writel(s, TRANSFER + 0x10, masks[i]), ==,
                        MEMTX_OK);
        assert_renderer_state(s);
        g_assert_cmphex(saved_transfer_mask(s, path), ==, masks[i]);
    }
    qtest_writel(s, TRANSFER + 0x10, 0x80402001);
    qtest_system_reset(s);
    g_assert_cmphex(saved_transfer_mask(s, path), ==, 0);
    qtest_quit(s);
    g_unlink(path);
    g_rmdir(dir);
}

static void test_transfer_mask_refusal(void)
{
    const RendererAccess accesses[] = {
        { 0x3010, 4, false }, { 0x3010, 1, true },
        { 0x3010, 2, true }, { 0x3010, 8, true },
        { 0x3011, 4, true }, { 0x300f, 2, true },
        { 0x300c, 8, true }, { 0x3014, 4, true },
        { 0x3810, 4, true }, { 0x3810, 4, false },
    };
    g_autofree char *dir = g_dir_make_tmp("crime-stipple-refuse-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);

    for (unsigned i = 0; i < ARRAY_SIZE(accesses); i++) {
        const RendererAccess *a = &accesses[i];
        QTestState *s = start_profile(true,
            "-global sgi-crime.functional-memory-transfer=on "
            "-device sgi-crime-test-fixture,id=inject,crime=" CRIME_PATH);
        uint64_t value;

        seed_renderer_state(s);
        qtest_writel(s, TRANSFER + 0x10, 0x80402001);
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
        g_assert_cmpint(a->write ?
            memtx_write(s, RENDERER + a->offset, 0x13579bdf, a->size) :
            memtx_read(s, RENDERER + a->offset, &value, a->size), ==,
            MEMTX_ERROR);
        assert_stopped(s);
        assert_renderer_state(s);
        g_assert_cmphex(saved_transfer_mask(s, path), ==, 0x80402001);
        qtest_quit(s);
    }
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned functional = 0; functional < 2; functional++) {
            QTestState *s;

            if (legacy && functional) {
                continue;
            }
            s = start_profile(legacy, functional ?
                "-global sgi-crime.functional-memory-transfer=on" : "");
            g_assert_cmpint(memtx_writel(s, TRANSFER + 0x10, 0x80402001),
                            ==, MEMTX_ERROR);
            g_assert_cmphex(saved_transfer_mask(s, path), ==, 0);
            qtest_quit(s);
        }
    }
    g_unlink(path);
    g_rmdir(dir);
}

static void test_transfer_mask_commands(void)
{
    const uint32_t masks[] = { 0, UINT32_MAX, 0x80402001, 0x13579bdf };
    g_autofree char *dir = g_dir_make_tmp("crime-stipple-command-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);

    for (unsigned i = 0; i < ARRAY_SIZE(masks); i++) {
        QTestState *s = start_profile(true,
            "-global sgi-crime.functional-memory-transfer=on");
        QTestState *to = start_profile(true,
            "-global sgi-crime.functional-memory-transfer=on -incoming defer");
        g_autofree char *quoted = g_shell_quote(path);
        g_autofree char *incoming = g_strdup_printf("exec:cat %s", quoted);

        qtest_writel(s, TRANSFER + 0x10, masks[i]);
        qtest_writeq(s, LINEAR_TLB, 0x8000000380000004ULL);
        qtest_writel(s, TRANSFER + 8, UINT32_MAX);
        qtest_writel(s, TRANSFER + 0x18, 0x5a);
        qtest_writel(s, TRANSFER + 0x30, 7);
        qtest_writel(s, TRANSFER + 0x38, 22);
        qtest_memset(s, 0x3000, 0x6c, 64);
        qtest_writel(s, TRANSFER + 0x800, 4U << 2);
        for (unsigned b = 0; b < 64; b++) {
            g_assert_cmphex(qtest_readb(s, 0x3000 + b), ==,
                            b >= 7 && b <= 22 ? 0x5a : 0x6c);
        }
        g_assert_cmphex(saved_transfer_mask(s, path), ==, masks[i]);
        qtest_qmp_assert_success(to,
            "{'execute':'migrate-incoming','arguments':{'uri':%s}}", incoming);
        sgi_wait_migration(to, "completed");
        g_assert_cmphex(saved_transfer_mask(to, path), ==, masks[i]);
        qtest_quit(s);
        s = to;
        qtest_writel(s, TRANSFER + 0x20, 7);
        qtest_writel(s, TRANSFER + 0x28, 22);
        qtest_writel(s, TRANSFER + 0x30, 0x1007);
        qtest_writel(s, TRANSFER + 0x38, 0x1016);
        qtest_memset(s, 0x4000, 0xa3, 64);
        qtest_writel(s, TRANSFER + 0x800,
                     (1U << 11) | (4U << 5) | (4U << 2));
        for (unsigned b = 0; b < 64; b++) {
            g_assert_cmphex(qtest_readb(s, 0x4000 + b), ==,
                            b >= 7 && b <= 22 ? 0x5a : 0xa3);
        }
        g_assert_cmphex(saved_transfer_mask(s, path), ==, masks[i]);
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
        g_assert_cmpint(memtx_writel(s, TRANSFER + 0x800,
            (1U << 10) | (4U << 2)), ==, MEMTX_ERROR);
        assert_stopped(s);
        g_assert_cmphex(qtest_readl(s, TRANSFER), ==,
                        (1U << 11) | (4U << 5) | (4U << 2));
        for (unsigned b = 0; b < 64; b++) {
            g_assert_cmphex(qtest_readb(s, 0x4000 + b), ==,
                            b >= 7 && b <= 22 ? 0x5a : 0xa3);
        }
        g_assert_cmphex(saved_transfer_mask(s, path), ==, masks[i]);
        qtest_system_reset(s);
        g_assert_cmphex(saved_transfer_mask(s, path), ==, 0);
        qtest_quit(s);
    }
    g_unlink(path);
    g_rmdir(dir);
}

static void test_transfer_mask_tiled(void)
{
    const uint32_t masks[] = { 0, UINT32_MAX, 0x80402001, 0x13579bdf };
    g_autofree char *dir = g_dir_make_tmp("crime-stipple-tiled-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(dir, "state", NULL);

    for (unsigned i = 0; i < ARRAY_SIZE(masks); i++) {
        QTestState *s = start_profile(true,
            "-global sgi-crime.functional-memory-transfer=on");

        qtest_writel(s, TRANSFER + 0x10, masks[i]);
        qtest_writeq(s, RENDERER + 0x1000, 0x8003000000000000ULL);
        qtest_writel(s, TRANSFER, 0);
        qtest_writel(s, TRANSFER + 8, UINT32_MAX);
        qtest_writel(s, TRANSFER + 0x18, 0xb7b7b7b7);
        qtest_writel(s, TRANSFER + 0x40, 1);
        qtest_writel(s, TRANSFER + 0x48, 1);
        qtest_writel(s, TRANSFER + 0x30, 7U << 16);
        qtest_memset(s, 0x30000, 0x6c, 64);
        qtest_writel(s, TRANSFER + 0x838, 22U << 16);
        g_assert_cmphex(saved_transfer_mask(s, path), ==, masks[i]);
        qtest_writel(s, TRANSFER + 0x20, 7U << 16);
        qtest_writel(s, TRANSFER + 0x28, 22U << 16);
        qtest_writel(s, TRANSFER + 0x30, 32U << 16);
        qtest_writel(s, TRANSFER + 0x38, 47U << 16);
        qtest_writel(s, TRANSFER + 0x800, 1U << 11);
        for (unsigned b = 0; b < 64; b++) {
            g_assert_cmphex(qtest_readb(s, 0x30000 + b), ==,
                (b >= 7 && b <= 22) || (b >= 32 && b <= 47) ? 0xb7 : 0x6c);
        }
        g_assert_cmphex(saved_transfer_mask(s, path), ==, masks[i]);
        qtest_writel(s, TRANSFER, (1U << 11) | (1U << 10));
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
        g_assert_cmpint(memtx_writel(s, TRANSFER + 0x818, 0x55), ==,
                        MEMTX_ERROR);
        assert_stopped(s);
        g_assert_cmphex(qtest_readl(s, TRANSFER + 0x18), ==, 0xb7b7b7b7);
        for (unsigned b = 0; b < 64; b++) {
            g_assert_cmphex(qtest_readb(s, 0x30000 + b), ==,
                (b >= 7 && b <= 22) || (b >= 32 && b <= 47) ? 0xb7 : 0x6c);
        }
        g_assert_cmphex(saved_transfer_mask(s, path), ==, masks[i]);
        qtest_quit(s);
    }
    g_unlink(path);
    g_rmdir(dir);
}

static void test_renderer_profile_policy(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned functional = 0; functional < 2; functional++) {
            QTestState *s = start_profile(legacy, functional ?
                "-global sgi-crime.functional-memory-transfer=on" : "");

            qtest_qmp_assert_success(s, "{'execute':'cont'}");
            g_assert_cmpint(memtx_writel(s, RENDERER + 0x408, 0),
                            ==, MEMTX_ERROR);
            assert_run_status(s, legacy != functional ?
                              "internal-error" : "running");
            qtest_quit(s);
        }
    }
}

static void test_renderer_cpu(gconstpointer data)
{
    unsigned kind = GPOINTER_TO_UINT(data);
    bool read = kind == 1;
    bool status_bank = kind == 5;
    bool hard = kind >= 2 && !status_bank;
    unsigned offset = status_bank ? 0x4008 : kind == 2 ? 0x400 :
                      kind == 3 ? 0x2880 : kind == 4 ? 0x3000 : 0x408;
    g_autofree char *directory =
        g_dir_make_tmp("crime-render-cpu-XXXXXX", NULL);
    g_autofree char *path = g_build_filename(directory, "device.txt", NULL);
    g_autofree char *contents = NULL;
    QTestState *s = qtest_initf(MEMTX_TEST_DEVICE
        "-M o2-test,crime-profile=legacy-software,"
        "bank-profile=8x128m-synthetic,reset-low-ram-approx=on,"
        "functional-memory-transfer=on -cpu R10000 -m 1G -nodefaults "
        "-accel tcg,thread=single -S -d unimp,guest_errors -D %s "
        "-device loader,addr=0xffffffffa0001000,cpu-num=0", path);
    uint32_t instructions[] = {
        mips_mtc0(0, 12), 0, 0, 0, 0,
        mips_lui(8, 0xb500), mips_lui(10, 0xa000),
        mips_addiu(9, 0, 0x55),
        read ? mips_lw(11, 8, offset) : (kind == 2 || kind == 4) ?
            mips_sd(9, 8, offset) : mips_sw(9, 8, offset),
        mips_sw(9, 10, 0x200), mips_branch(MIPS_OPC_BEQ, 0, 0, -1), 0,
    };
    uint32_t handler[] = {
        mips_lui(10, 0xa000), mips_mfc0(11, 13), mips_sw(11, 10, 0x210),
        mips_mfc0(11, 14), mips_sw(11, 10, 0x218),
        mips_addiu(11, 0, 0x66), mips_sw(11, 10, 0x220),
        mips_branch(MIPS_OPC_BEQ, 0, 0, -1), 0,
    };
    int64_t deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    for (unsigned i = 0; i < ARRAY_SIZE(instructions); i++) {
        qtest_writel(s, 0x1000 + 4 * i, instructions[i]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(handler); i++) {
        qtest_writel(s, 0x180 + 4 * i, handler[i]);
    }
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    for (;;) {
        g_autoptr(QDict) status =
            qtest_qmp_assert_success_ref(s, "{'execute':'query-status'}");

        if (!strcmp(qdict_get_str(status, "status"), "internal-error")) {
            g_assert_true(hard);
            break;
        }
        if (qtest_readl(s, read ? 0x220 : 0x200)) {
            g_assert_false(hard);
            break;
        }
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
    if (hard) {
        assert_stopped(s);
    } else {
        assert_run_status(s, "running");
    }
    g_assert_cmphex(qtest_readl(s, 0x200), ==, !hard && !read ? 0x55 : 0);
    g_assert_cmphex(qtest_readl(s, 0x220), ==, read ? 0x66 : 0);
    if (read) {
        g_assert_cmphex(qtest_readl(s, 0x210) & 0x7c, ==, 7 << 2);
        g_assert_cmphex(qtest_readl(s, 0x218), ==, 0xa0001020);
    }
    qtest_quit(s);
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    if (status_bank) {
        g_assert_nonnull(strstr(contents, "opaque engine-status-bank"));
        g_assert_nonnull(strstr(contents,
            "4-byte write at 0x15004008 value 0x55"));
    } else if (!hard) {
        g_assert_nonnull(strstr(contents, "sgi-crime: unsupported renderer"));
        g_assert_nonnull(strstr(contents, "0x15000408"));
    }
    if (kind == 2) {
        g_assert_nonnull(strstr(contents,
            "rejected renderer 8-byte write at 0x15000400 value 0x55"));
    }
    g_unlink(path);
    g_rmdir(directory);
}

static void test_ecc_replacement_stage(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        QTestState *s = start_profile(legacy, "");
        uint64_t value;

        g_assert_cmpint(memtx_store(s, BASE + 0x270, 0xa5, 8), ==, MEMTX_OK);
        g_assert_cmpint(memtx_load(s, BASE + 0x270, &value, 8), ==, MEMTX_OK);
        g_assert_cmphex(value, ==, 0xa5);
        qtest_quit(s);
    }
}

static const char *const ecc_error_properties[] = {
    "cpu-error-address", "cpu-error-status", "memory-error-address",
    "memory-error-status",
};
static const unsigned ecc_observed_registers[] = {
    0, 8, 0x10, 0x18, 0x20, 0x28, 0x200,
};

typedef struct EccSnapshot {
    uint64_t registers[ARRAY_SIZE(ecc_observed_registers)];
    uint64_t errors[ARRAY_SIZE(ecc_error_properties)];
    uint8_t ram[32];
    unsigned outputs;
} EccSnapshot;

static uint64_t ecc_load(QTestState *s, unsigned offset)
{
    uint64_t value;

    g_assert_cmpint(memtx_load(s, BASE + offset, &value, 8), ==, MEMTX_OK);
    return value;
}

static void ecc_store(QTestState *s, unsigned offset, uint64_t value)
{
    g_assert_cmpint(memtx_store(s, BASE + offset, value, 8), ==, MEMTX_OK);
}

static char *ecc_options(bool functional, const char *log, const char *extra)
{
    return g_strdup_printf("%s -device sgi-crime-test-fixture,id=inject,crime="
        CRIME_PATH " -D %s %s",
        functional ? "-global sgi-crime.functional-memory-transfer=on" : "",
        log, extra);
}

static void ecc_seed(QTestState *s, bool errors)
{
    const uint64_t values[] = {
        0xfedcba9876543210ULL, 5, 0x23456789, 7U << 25,
    };

    qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
    ecc_store(s, 0x18, (1U << 4) | (1U << 28));
    cause(s, 4, 1);
    ecc_store(s, 0x20, 1U << 28);
    qtest_memset(s, 0x30000, 0x6c, 32);
    for (unsigned i = 0; i < ARRAY_SIZE(ecc_error_properties); i++) {
        qtest_qmp_assert_success(s,
            "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
            "'property':%s,'value':%" PRIu64 "}}",
            ecc_error_properties[i], errors ? values[i] : 0);
    }
}

static EccSnapshot ecc_snapshot(QTestState *s)
{
    EccSnapshot snapshot = { 0 };

    for (unsigned i = 0; i < ARRAY_SIZE(ecc_observed_registers); i++) {
        snapshot.registers[i] = ecc_load(s, ecc_observed_registers[i]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ecc_error_properties); i++) {
        g_autoptr(QDict) reply = qtest_qmp(s,
            "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/inject',"
            "'property':%s}}", ecc_error_properties[i]);

        snapshot.errors[i] = qdict_get_uint(reply, "return");
    }
    qtest_memread(s, 0x30000, snapshot.ram, sizeof(snapshot.ram));
    for (unsigned i = 0; i < 5; i++) {
        snapshot.outputs |= qtest_get_irq(s, i) << i;
    }
    return snapshot;
}

static void ecc_unchanged(QTestState *s, const EccSnapshot *before,
                           unsigned control, unsigned byte)
{
    EccSnapshot after = ecc_snapshot(s);

    for (unsigned i = 0; i < ARRAY_SIZE(ecc_observed_registers); i++) {
        g_assert_cmphex(after.registers[i], ==,
            ecc_observed_registers[i] == 0x200 ? control :
                                               before->registers[i]);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ecc_error_properties); i++) {
        g_assert_cmphex(after.errors[i], ==, before->errors[i]);
    }
    g_assert_cmpmem(after.ram, sizeof(after.ram),
                    before->ram, sizeof(before->ram));
    g_assert_cmpuint(after.outputs, ==, before->outputs);
    g_assert_cmphex(ecc_load(s, 0x270), ==, byte);
}

static void ecc_no_first_use(const char *path)
{
    g_autofree char *contents = NULL;

    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_assert_null(strstr(contents, "healthy-memory approximation:"));
}

static void test_ecc_replacement_values(void)
{
    const unsigned repeated[] = { 0xa5, 0x3c, 0xf0, 0, 255, 0x81, 0xa5 };

    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned functional = 0; functional < 2; functional++) {
            for (unsigned seeded = 0; seeded < 2; seeded++) {
                g_autofree char *dir =
                    g_dir_make_tmp("crime-ecc-values-XXXXXX", NULL);
                g_autofree char *log =
                    g_build_filename(dir, "device.txt", NULL);
                g_autofree char *options = ecc_options(functional, log, "");
                QTestState *s = start_profile(legacy, options);
                EccSnapshot before;

                g_assert_cmphex(ecc_load(s, 0x270), ==, 0);
                ecc_seed(s, seeded);
                before = ecc_snapshot(s);
                qtest_qmp_assert_success(s, "{'execute':'cont'}");
                for (unsigned value = 0; value <= 255; value++) {
                    uint8_t raw[8];

                    ecc_store(s, 0x270, value);
                    g_assert_cmphex(ecc_load(s, 0x270), ==, value);
                    qtest_memread(s, BASE + 0x270, raw, sizeof(raw));
                    for (unsigned i = 0; i < 7; i++) {
                        g_assert_cmphex(raw[i], ==, 0);
                    }
                    g_assert_cmphex(raw[7], ==, value);
                    ecc_unchanged(s, &before, 0, value);
                }
                for (unsigned i = 0; i < ARRAY_SIZE(repeated); i++) {
                    ecc_store(s, 0x270, repeated[i]);
                    ecc_store(s, 0x270, repeated[i]);
                    g_assert_cmphex(ecc_load(s, 0x270), ==, repeated[i]);
                    ecc_unchanged(s, &before, 0, repeated[i]);
                }
                assert_run_status(s, "running");
                qtest_system_reset(s);
                g_assert_cmphex(ecc_load(s, 0x270), ==, 0);
                g_assert_cmphex(ecc_load(s, 0x200), ==, 0);
                qtest_quit(s);
                ecc_no_first_use(log);
                g_unlink(log);
                g_rmdir(dir);
            }
        }
    }
}

static void test_ecc_replacement_refusal(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned functional = 0; functional < 2; functional++) {
            g_autofree char *dir =
                g_dir_make_tmp("crime-ecc-refuse-XXXXXX", NULL);
            g_autofree char *log = g_build_filename(dir, "device.txt", NULL);
            g_autofree char *options = ecc_options(functional, log, "");
            QTestState *s = start_profile(legacy, options);
            uint64_t value;
            EccSnapshot before;

            ecc_seed(s, true);
            if (functional) {
                ecc_store(s, 0x200, 1);
            }
            ecc_store(s, 0x270, 0xa5);
            before = ecc_snapshot(s);
            qtest_qmp_assert_success(s, "{'execute':'cont'}");
            for (unsigned bit = 8; bit < 64; bit++) {
                for (unsigned mixed = 0; mixed < 2; mixed++) {
                    uint64_t bad = (1ULL << bit) | (mixed ? 0x3c : 0);

                    g_assert_cmpint(memtx_store(s, BASE + 0x270, bad, 8),
                                    !=, MEMTX_OK);
                    ecc_unchanged(s, &before, functional, 0xa5);
                }
            }
            for (unsigned size = 1; size < 8; size *= 2) {
                g_assert_cmpint(memtx_store(s, BASE + 0x270, 0x3c, size),
                                !=, MEMTX_OK);
                g_assert_cmpint(memtx_load(s, BASE + 0x270, &value, size),
                                !=, MEMTX_OK);
                ecc_unchanged(s, &before, functional, 0xa5);
                g_assert_cmpint(memtx_store(s, BASE + 0x278 - size, 0x3c, size),
                                !=, MEMTX_OK);
                g_assert_cmpint(memtx_load(s, BASE + 0x278 - size,
                                          &value, size),
                                !=, MEMTX_OK);
                ecc_unchanged(s, &before, functional, 0xa5);
            }
            for (unsigned delta = 1; delta < 8; delta++) {
                g_assert_cmpint(memtx_store(s, BASE + 0x270 + delta, 0x3c, 8),
                                !=, MEMTX_OK);
                g_assert_cmpint(memtx_load(s, BASE + 0x270 + delta, &value, 8),
                                !=, MEMTX_OK);
                ecc_unchanged(s, &before, functional, 0xa5);
                g_assert_cmpint(memtx_store(s, BASE + 0x270 - delta, 0x3c, 8),
                                !=, MEMTX_OK);
                g_assert_cmpint(memtx_load(s, BASE + 0x270 - delta, &value, 8),
                                !=, MEMTX_OK);
                ecc_unchanged(s, &before, functional, 0xa5);
            }
            if (legacy || functional) {
                assert_stopped(s);
            }
            qtest_quit(s);
            g_unlink(log);
            g_rmdir(dir);
        }
    }
}

static void test_ecc_replacement_controls(void)
{
    const unsigned staged[] = { 0xa5, 0x3c, 0x81 };

    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned functional = 0; functional < 2; functional++) {
            for (unsigned old = 0; old <= functional; old++) {
                for (unsigned zero = 0; zero < 2; zero++) {
                    g_autofree char *dir =
                        g_dir_make_tmp("crime-ecc-control-XXXXXX", NULL);
                    g_autofree char *log =
                        g_build_filename(dir, "device.txt", NULL);
                    g_autofree char *options = ecc_options(functional, log, "");
                    QTestState *s = start_profile(legacy, options);
                    unsigned byte = zero ? 0 : 0xa5;
                    EccSnapshot before;

                    ecc_seed(s, !zero);
                    ecc_store(s, 0x270, byte);
                    if (legacy || functional) {
                        ecc_store(s, 0x200, old);
                    }
                    before = ecc_snapshot(s);
                    qtest_qmp_assert_success(s, "{'execute':'cont'}");
                    if (functional) {
                        for (unsigned control = 0; control < 2; control++) {
                            ecc_store(s, 0x200, control);
                            ecc_unchanged(s, &before, control, byte);
                            for (unsigned i = 0; i < ARRAY_SIZE(staged); i++) {
                                ecc_store(s, 0x270, staged[i]);
                                ecc_unchanged(s, &before, control, staged[i]);
                            }
                            ecc_store(s, 0x270, byte);
                        }
                        ecc_store(s, 0x200, old);
                    } else {
                        g_assert_cmpint(memtx_store(s, BASE + 0x200, 1, 8),
                                        !=, MEMTX_OK);
                        ecc_unchanged(s, &before, old, byte);
                        if (!legacy) {
                            g_assert_cmpint(memtx_store(s, BASE + 0x200, 0, 8),
                                            !=, MEMTX_OK);
                        }
                    }
                    for (unsigned control = 2; control <= 3; control++) {
                        g_assert_cmpint(memtx_store(s, BASE + 0x200,
                                                   control, 8),
                                        !=, MEMTX_OK);
                        ecc_unchanged(s, &before, old, byte);
                    }
                    for (unsigned bit = 2; bit < 64; bit++) {
                        g_assert_cmpint(memtx_store(s, BASE + 0x200,
                                                   1ULL << bit, 8),
                                        !=, MEMTX_OK);
                        ecc_unchanged(s, &before, old, byte);
                    }
                    if (legacy || functional) {
                        assert_stopped(s);
                    }
                    qtest_quit(s);
                    g_unlink(log);
                    g_rmdir(dir);
                }
            }
        }
    }
}

static void test_ecc_replacement_peers(void)
{
    const unsigned staged[] = { 0x35, 0xc7 };

    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned functional = 0; functional < 2; functional++) {
            uint8_t ram[2][32];
            EccSnapshot snapshots[2];

            for (unsigned peer = 0; peer < 2; peer++) {
                g_autofree char *dir =
                    g_dir_make_tmp("crime-ecc-peer-XXXXXX", NULL);
                g_autofree char *log =
                    g_build_filename(dir, "device.txt", NULL);
                g_autofree char *options = ecc_options(functional, log, "");
                QTestState *s = start_profile(legacy, options);
                EccSnapshot before;
                uint64_t value;

                ecc_seed(s, true);
                ecc_store(s, 0x270, staged[peer]);
                before = ecc_snapshot(s);
                g_assert_cmpint(memtx_store(s, 0x30008,
                                           0x1122334455667788ULL, 8),
                                ==, MEMTX_OK);
                stq_be_p(before.ram + 8, 0x1122334455667788ULL);
                ecc_unchanged(s, &before, 0, staged[peer]);
                if (functional) {
                    ecc_store(s, 0x200, 1);
                    g_assert_cmpint(memtx_store(s, 0x15001700,
                        0x8000003080000031ULL, 8), ==, MEMTX_OK);
                    g_assert_cmpint(memtx_store(s, 0x15003000,
                                               (4U << 2) | 2, 4),
                                    ==, MEMTX_OK);
                    g_assert_cmpint(memtx_store(s, 0x15003008, UINT32_MAX, 4),
                                    ==, MEMTX_OK);
                    g_assert_cmpint(memtx_store(s, 0x15003018, 0x5b, 4),
                                    ==, MEMTX_OK);
                    g_assert_cmpint(memtx_store(s, 0x15003030, 8, 4),
                                    ==, MEMTX_OK);
                    g_assert_cmpint(memtx_store(s, 0x15003838, 15, 4),
                                    ==, MEMTX_OK);
                    memset(before.ram + 8, 0x5b, 8);
                    ecc_unchanged(s, &before, 1, staged[peer]);
                    g_assert_cmpint(memtx_load(s, BASE + 0x260, &value, 8),
                                    ==, MEMTX_OK);
                    g_assert_cmphex(value, ==, 0);
                } else {
                    g_assert_cmpint(memtx_load(s, BASE + 0x260, &value, 8),
                                    !=, MEMTX_OK);
                }
                if (legacy || functional) {
                    g_assert_cmphex(ecc_load(s, 0x250), ==, before.errors[3]);
                    g_assert_cmphex(ecc_load(s, 0x258), ==, before.errors[2]);
                } else {
                    g_assert_cmpint(memtx_load(s, BASE + 0x250, &value, 8),
                                    !=, MEMTX_OK);
                    g_assert_cmpint(memtx_load(s, BASE + 0x258, &value, 8),
                                    !=, MEMTX_OK);
                }
                g_assert_cmpint(memtx_load(s, BASE + 0x268, &value, 8),
                                !=, MEMTX_OK);
                g_assert_cmpint(memtx_store(s, BASE + 0x268, 0, 8),
                                !=, MEMTX_OK);
                g_assert_cmpint(memtx_load(s, BASE + 0x248, &value, 8),
                                !=, MEMTX_OK);
                g_assert_cmpint(memtx_store(s, BASE + 0x260, 0, 8),
                                !=, MEMTX_OK);
                ecc_unchanged(s, &before, functional, staged[peer]);
                snapshots[peer] = ecc_snapshot(s);
                qtest_memread(s, 0x30000, ram[peer], sizeof(ram[peer]));
                qtest_quit(s);
                if (!functional) {
                    ecc_no_first_use(log);
                }
                g_unlink(log);
                g_rmdir(dir);
            }
            g_assert_cmpmem(ram[0], sizeof(ram[0]), ram[1], sizeof(ram[1]));
            g_assert_cmpmem(snapshots[0].registers,
                            sizeof(snapshots[0].registers),
                            snapshots[1].registers,
                            sizeof(snapshots[1].registers));
            g_assert_cmpmem(snapshots[0].errors, sizeof(snapshots[0].errors),
                            snapshots[1].errors, sizeof(snapshots[1].errors));
            g_assert_cmpuint(snapshots[0].outputs, ==, snapshots[1].outputs);
        }
    }
}

static void ecc_migration_seed(QTestState *s, bool legacy)
{
    ecc_seed(s, true);
    if (!legacy) {
        for (unsigned i = 0; i < 2; i++) {
            qtest_qmp_assert_success(s,
                "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/inject',"
                "'property':%s,'value':0}}", ecc_error_properties[i]);
        }
    }
}

static void test_ecc_replacement_migration(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned functional = 0; functional < 2; functional++) {
            for (unsigned control = 0; control <= functional; control++) {
                g_autofree char *dir =
                    g_dir_make_tmp("crime-ecc-move-XXXXXX", NULL);
                g_autofree char *path = g_build_filename(dir, "state", NULL);
                g_autofree char *from_log =
                    g_build_filename(dir, "from.txt", NULL);
                g_autofree char *to_log = g_build_filename(dir, "to.txt", NULL);
                g_autofree char *quoted = g_shell_quote(path);
                g_autofree char *outgoing =
                    g_strdup_printf("exec:cat > %s", quoted);
                g_autofree char *incoming =
                    g_strdup_printf("exec:cat %s", quoted);
                g_autofree char *from_options =
                    ecc_options(functional, from_log, "");
                g_autofree char *to_options = ecc_options(functional, to_log,
                                                         "-incoming defer");
                QTestState *from = start_profile(legacy, from_options);
                QTestState *to = start_profile(legacy, to_options);
                EccSnapshot before;
                uint8_t ram[32];
                g_autofree char *stream = NULL;
                gsize length;
                const char name[] = "sgi-crime/ecc-replacement";
                const char *state;

                ecc_migration_seed(from, legacy);
                qtest_irq_intercept_out_named(to, CRIME_PATH, "cpu-ip");
                ecc_store(from, 0x270, 0xa5);
                if (legacy || functional) {
                    ecc_store(from, 0x200, control);
                }
                before = ecc_snapshot(from);
                ecc_store(to, 0x270, 0x3c);
                sgi_migrate_stopped(from, outgoing);
                g_assert_true(g_file_get_contents(path, &stream,
                                                  &length, NULL));
                state = memmem(stream, length, name, sizeof(name) - 1);
                g_assert_nonnull(state);
                state += sizeof(name) - 1;
                g_assert_cmphex(ldl_be_p(state), ==, 1);
                g_assert_cmphex(ldq_be_p(state + 4), ==, 0xa5);
                qtest_qmp_assert_success(to,
                    "{'execute':'migrate-incoming','arguments':{'uri':%s}}",
                    incoming);
                sgi_wait_migration(to, "completed");
                ecc_unchanged(to, &before, control, 0xa5);
                ecc_store(to, 0x270, 0x81);
                ecc_unchanged(to, &before, control, 0x81);
                g_assert_cmpint(memtx_store(to, BASE + 0x200, 2, 8),
                                !=, MEMTX_OK);
                ecc_unchanged(to, &before, control, 0x81);
                g_assert_cmpint(memtx_store(to, BASE + 0x200, 3, 8),
                                !=, MEMTX_OK);
                ecc_unchanged(to, &before, control, 0x81);
                qtest_system_reset(to);
                g_assert_cmphex(ecc_load(to, 0x270), ==, 0);
                g_assert_cmphex(ecc_load(to, 0x200), ==, 0);
                qtest_memread(to, 0x30000, ram, sizeof(ram));
                g_assert_cmpmem(ram, sizeof(ram),
                                before.ram, sizeof(before.ram));
                assert_outputs(to, 0);
                qtest_quit(from);
                qtest_quit(to);
                g_unlink(path);
                g_unlink(from_log);
                g_unlink(to_log);
                g_rmdir(dir);
            }
        }
    }
}

static void test_ecc_replacement_invalid_stream(void)
{
    const char name[] = "sgi-crime/ecc-replacement";
    const char transfer[] = "sgi-crime/linear-transfer";

    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned fault = 0; fault < 10; fault++) {
            g_autofree char *dir =
                g_dir_make_tmp("crime-ecc-invalid-XXXXXX", NULL);
            g_autofree char *path = g_build_filename(dir, "state", NULL);
            g_autofree char *from_log = g_build_filename(dir, "from.txt", NULL);
            g_autofree char *to_log = g_build_filename(dir, "to.txt", NULL);
            g_autofree char *quoted = g_shell_quote(path);
            g_autofree char *outgoing =
                g_strdup_printf("exec:cat > %s", quoted);
            g_autofree char *incoming = g_strdup_printf("exec:cat %s", quoted);
            g_autofree char *from_options = ecc_options(true, from_log, "");
            g_autofree char *to_options = ecc_options(fault != 8, to_log,
                                                     "-incoming defer");
            g_autofree char *stream = NULL;
            gsize size;
            char *state;
            QTestState *from = start_profile(legacy, from_options);
            QTestState *to = start_profile(fault == 7 ? !legacy : legacy,
                                           to_options);
            EccSnapshot before;

            ecc_migration_seed(from, legacy);
            ecc_store(from, 0x270, 0xa5);
            ecc_store(from, 0x200, 1);
            ecc_seed(to, true);
            ecc_store(to, 0x270, 0x5a);
            ecc_store(to, 0x18, (1U << 8) | (1U << 28));
            cause(to, 4, 0);
            cause(to, 8, 1);
            before = ecc_snapshot(to);
            sgi_migrate_stopped(from, outgoing);
            g_assert_true(g_file_get_contents(path, &stream, &size, NULL));
            state = memmem(stream, size, name, sizeof(name) - 1);
            g_assert_nonnull(state);
            state += sizeof(name) - 1;
            g_assert_cmphex(ldl_be_p(state), ==, 1);
            g_assert_cmphex(ldq_be_p(state + 4), ==, 0xa5);
            switch (fault) {
            case 0:
                stq_be_p(state + 4, 0x100);
                break;
            case 1:
                stq_be_p(state + 4, 0x80000000000000a5ULL);
                break;
            case 2: {
                char *first = state - (sizeof(name) - 1) - 2;
                size_t length = 2 + sizeof(name) - 1 + 4 + 8;

                g_assert_cmphex((uint8_t)first[0], ==, 5);
                g_assert_cmpuint((uint8_t)first[1], ==, sizeof(name) - 1);
                memmove(first, first + length,
                        size - (first - stream) - length);
                size -= length;
                break;
            }
            case 3:
                stl_be_p(state, 0);
                break;
            case 4:
                stl_be_p(state, 2);
                break;
            case 5: case 6:
                state = memmem(stream, size, transfer, sizeof(transfer) - 1);
                g_assert_nonnull(state);
                state += sizeof(transfer) - 1 + 4;
                /* Existing public transfer test's accepted-control field. */
                g_assert_cmphex(ldq_be_p(state + 2 + 256 + 40), ==, 1);
                stq_be_p(state + 2 + 256 + 40, fault == 5 ? 2 : 3);
                break;
            case 7: case 8:
                /* The destination rejects the intact profile/feature tag. */
                break;
            case 9: {
                const char profile[] = "sgi-crime/profile";

                state = memmem(stream, size, profile, sizeof(profile) - 1);
                g_assert_nonnull(state);
                state += sizeof(profile) - 1 + 4;
                state[0] = 2;
                break;
            }
            default:
                g_assert_not_reached();
            }
            g_assert_true(g_file_set_contents(path, stream, size, NULL));
            qtest_qmp_assert_success(to,
                "{'execute':'migrate-incoming',"
                "'arguments':{'uri':%s,'exit-on-error':false}}", incoming);
            wait_failed(to);
            ecc_unchanged(to, &before, 0, 0x5a);
            qtest_quit(from);
            qtest_quit(to);
            g_unlink(path);
            g_unlink(from_log);
            g_unlink(to_log);
            g_rmdir(dir);
        }
    }
}

static void test_ecc_replacement_width_stop(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        for (unsigned functional = 0; functional < 2; functional++) {
            for (unsigned load = 0; load < 2; load++) {
                g_autofree char *dir =
                    g_dir_make_tmp("crime-ecc-width-XXXXXX", NULL);
                g_autofree char *log =
                    g_build_filename(dir, "device.txt", NULL);
                g_autofree char *options = ecc_options(functional, log, "");
                QTestState *s = start_profile(legacy, options);
                EccSnapshot before;
                uint64_t value;

                ecc_seed(s, true);
                ecc_store(s, 0x270, 0xa5);
                before = ecc_snapshot(s);
                qtest_qmp_assert_success(s, "{'execute':'cont'}");
                if (load) {
                    g_assert_cmpint(memtx_load(s, BASE + 0x270, &value, 2),
                                    !=, MEMTX_OK);
                } else {
                    g_assert_cmpint(memtx_store(s, BASE + 0x270, 0x3c, 4),
                                    !=, MEMTX_OK);
                }
                if (legacy || functional) {
                    assert_stopped(s);
                }
                ecc_unchanged(s, &before, 0, 0xa5);
                qtest_quit(s);
                g_unlink(log);
                g_rmdir(dir);
            }
        }
    }
}

int main(int argc, char **argv)
{
    const char *faults[] = {
        "missing-section", "profile-id", "functional-policy", "latch-mask",
        "cpu-error", "memory-address", "hardware", "timer-fraction", "version",
    };
    const char *renderer_cpu[] = {
        "posted-store", "read-bus-error", "control-stop", "go-stop",
        "malformed-stop", "status-bank-store",
    };
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/crime/transfer-mask/tiled", test_transfer_mask_tiled);
    qtest_add_func("/sgi/crime/transfer-mask/stage", test_transfer_mask_stage);
    qtest_add_func("/sgi/crime/transfer-mask/refusal",
                   test_transfer_mask_refusal);
    qtest_add_func("/sgi/crime/transfer-mask/commands",
                   test_transfer_mask_commands);
    qtest_add_func("/sgi/crime/renderer/ifb/read", test_ifb_read);
    qtest_add_data_func("/sgi/crime/renderer/ifb/write",
                        GUINT_TO_POINTER(0), test_opaque_renderer_write);
    qtest_add_data_func("/sgi/crime/renderer/status-bank/write",
                        GUINT_TO_POINTER(1), test_opaque_renderer_write);
    qtest_add_func("/sgi/crime/renderer/status-bank/refusal",
                   test_status_bank_refusal);
    qtest_add_func("/sgi/crime/renderer/status-bank/profiles",
                   test_status_bank_profiles);
    qtest_add_func("/sgi/crime/renderer/ifb/refusal", test_ifb_refusal);
    qtest_add_func("/sgi/crime/renderer/ifb/profiles", test_ifb_profiles);
    qtest_add_func("/sgi/crime/renderer/refusal", test_renderer_refusal);
    qtest_add_func("/sgi/crime/renderer/hard-stop", test_renderer_hard_stop);
    qtest_add_func("/sgi/crime/renderer/profiles",
                   test_renderer_profile_policy);
    for (unsigned i = 0; i < ARRAY_SIZE(renderer_cpu); i++) {
        g_autofree char *name = g_strdup_printf("/sgi/crime/renderer/cpu/%s",
                                               renderer_cpu[i]);

        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_renderer_cpu);
    }
    qtest_add_func("/sgi/crime/legacy/identity", test_identity);
    qtest_add_func("/sgi/crime/legacy/controls", test_controls);
    qtest_add_func("/sgi/crime/legacy/error-enable-zero",
                   test_error_enable_zero);
    qtest_add_func("/sgi/crime/legacy/error-enable-refusal",
                   test_error_enable_refusal);
    qtest_add_func("/sgi/crime/legacy/register-access", test_register_access);
    qtest_add_func("/sgi/crime/legacy/interrupt-mask-upper",
                   test_interrupt_mask_upper);
    qtest_add_func("/sgi/crime/1.5/interrupt-mask-upper-refusal",
                   test_interrupt_mask_upper_refusal);
    qtest_add_func("/sgi/crime/legacy/banks", test_banks);
    qtest_add_func("/sgi/crime/legacy/immutable", test_immutable);
    qtest_add_func("/sgi/crime/legacy/level-latches", test_level_latches);
    qtest_add_data_func("/sgi/crime/legacy/source-routing",
                        GUINT_TO_POINTER(0), test_source_routing);
    qtest_add_data_func("/sgi/crime/legacy/functional-source-routing",
                        GUINT_TO_POINTER(1), test_source_routing);
    qtest_add_data_func("/sgi/crime/legacy/source-reset",
                        GUINT_TO_POINTER(0), test_source_reset);
    qtest_add_data_func("/sgi/crime/legacy/functional-source-reset",
                        GUINT_TO_POINTER(1), test_source_reset);
    qtest_add_func("/sgi/crime/renderer/no-generated-irq",
                   test_renderer_no_generated_irq);
    qtest_add_func("/sgi/crime/legacy/counter", test_counter);
    qtest_add_data_func("/sgi/crime/legacy/migration",
                        GINT_TO_POINTER(1), test_migration);
    qtest_add_data_func("/sgi/crime/default/migration-sources",
                        GINT_TO_POINTER(0), test_migration);
    qtest_add_data_func("/sgi/crime/legacy/functional-migration-sources",
                        GUINT_TO_POINTER(3), test_migration);
    qtest_add_data_func("/sgi/crime/default/functional-migration-sources",
                        GUINT_TO_POINTER(2), test_migration);
    qtest_add_func("/sgi/crime/legacy/cpu-clock-interrupt",
                   test_clock_interrupt_independence);
    qtest_add_func("/sgi/crime/legacy/machine-admission",
                   test_machine_admission);
    qtest_add_data_func("/sgi/crime/legacy/mismatch-to-default",
                        GINT_TO_POINTER(1), test_profile_mismatch);
    qtest_add_data_func("/sgi/crime/legacy/mismatch-from-default",
                        GINT_TO_POINTER(0), test_profile_mismatch);
    for (unsigned i = 0; i < ARRAY_SIZE(faults); i++) {
        g_autofree char *name = g_strdup_printf("/sgi/crime/legacy/invalid-%s",
                                               faults[i]);

        qtest_add_data_func(name, GUINT_TO_POINTER(i),
                            test_invalid_profile_state);
    }
    qtest_add_data_func("/sgi/crime/legacy/feature-id-word", "prom-id-word",
                        test_feature_mismatch);
    qtest_add_data_func("/sgi/crime/legacy/feature-inert-renderer",
                        "empty-renderer-status", test_feature_mismatch);
    qtest_add_func("/sgi/crime/legacy/ecc-replacement/stage",
                   test_ecc_replacement_stage);
    qtest_add_func("/sgi/crime/legacy/ecc-replacement/values",
                   test_ecc_replacement_values);
    qtest_add_func("/sgi/crime/legacy/ecc-replacement/refusal",
                   test_ecc_replacement_refusal);
    qtest_add_func("/sgi/crime/legacy/ecc-replacement/controls",
                   test_ecc_replacement_controls);
    qtest_add_func("/sgi/crime/legacy/ecc-replacement/peers",
                   test_ecc_replacement_peers);
    qtest_add_func("/sgi/crime/legacy/ecc-replacement/migration",
                   test_ecc_replacement_migration);
    qtest_add_func("/sgi/crime/legacy/ecc-replacement/invalid-stream",
                   test_ecc_replacement_invalid_stream);
    qtest_add_func("/sgi/crime/legacy/ecc-replacement/width-stop",
                   test_ecc_replacement_width_stop);
    return g_test_run();
}
