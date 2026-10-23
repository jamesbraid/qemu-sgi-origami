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
        qtest_writeq(s, BASE + 0x18, bit | held_bit);
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

static void test_source_routing(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        QTestState *s = start_profile(legacy, "");

        qtest_irq_intercept_out_named(s, CRIME_PATH, "cpu-ip");
        exercise_sources(s, legacy);
        qtest_quit(s);
    }
}

static void test_source_reset(void)
{
    for (unsigned legacy = 0; legacy < 2; legacy++) {
        QTestState *s = start_profile(legacy, "");

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
    bool legacy = GPOINTER_TO_INT(data);
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("crime-profile-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from = start_profile(legacy, "");
    QTestState *to = start_profile(legacy, "-incoming defer");
    unsigned software_bit = legacy ? 30 : 29;
    uint32_t hardware = (1U << 4) | (1U << 16) | (1U << 22);

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    qtest_irq_intercept_out_named(to, CRIME_PATH, "cpu-ip");
    qtest_set_irq_in(from, CRIME_PATH, "mace", 4, 1);
    cause(from, 16, 1);
    cause(from, 22, 1);
    cause(from, 22, 0);
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
    assert_outputs(to, 0);
    qtest_clock_step(to, 1);
    g_assert_cmphex(qtest_readq(to, BASE + 0x38), ==, 0xffffffff);
    qtest_writeq(to, BASE + 0x18, hardware | (1U << software_bit));
    assert_outputs(to, legacy ? 1 : 0xb);
    qtest_writeq(to, BASE + 0x28, 0);
    g_assert_cmphex(qtest_readq(to, BASE + 0x28), ==, hardware & ~(1U << 22));
    cause(to, 16, 0);
    qtest_writeq(to, BASE + 0x28, 0);
    g_assert_cmphex(qtest_readq(to, BASE + 0x28), ==, 1U << 4);
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
    g_assert_cmpint(memtx_writeq(s, BASE + 0x270, 1), !=, MEMTX_OK);
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

static void seed_renderer_state(QTestState *s)
{
    const unsigned parameters[] = { 0, 8, 0x18, 0x20, 0x28, 0x40, 0x48 };

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

static void test_renderer_refusal(void)
{
    const RendererAccess accesses[] = {
        { 0x408, 4, true }, { 0x600, 8, true },
        { 0x2080, 8, true }, { 0x2081, 4, true },
        { 0x2080, 4, false }, { 0x20d0, 1, true },
        { 0x2400, 4, true }, { 0x400, 4, false },
        { 0x21f0, 4, false }, { 0x21f8, 4, false },
        { 0x16fc, 4, true }, { 0x27fc, 4, true },
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
        { 0x400, 4, true }, { 0x2880, 4, true },
        { 0x21f0, 4, true }, { 0x21f8, 4, true },
        { 0x3070, 4, true }, { 0x3078, 4, true },
        { 0x3808, 4, true }, { 0x3800, 4, true },
        { 0x3838, 4, true },
        { 0x3000, 8, true }, { 0x3001, 4, true },
        { 0x1700, 4, true }, { 0x16ff, 4, true },
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
    bool hard = kind >= 2;
    unsigned offset = kind == 2 ? 0x400 : kind == 3 ? 0x2880 :
                      kind == 4 ? 0x3000 : 0x408;
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
        read ? mips_lw(11, 8, offset) : kind == 4 ?
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
    if (!hard) {
        g_assert_nonnull(strstr(contents, "sgi-crime: unsupported renderer"));
        g_assert_nonnull(strstr(contents, "0x15000408"));
    }
    g_unlink(path);
    g_rmdir(directory);
}

int main(int argc, char **argv)
{
    const char *faults[] = {
        "missing-section", "profile-id", "functional-policy", "latch-mask",
        "cpu-error", "memory-address", "hardware", "timer-fraction", "version",
    };
    const char *renderer_cpu[] = {
        "posted-store", "read-bus-error", "control-stop", "go-stop",
        "malformed-stop",
    };
    g_test_init(&argc, &argv, NULL);
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
    qtest_add_func("/sgi/crime/legacy/banks", test_banks);
    qtest_add_func("/sgi/crime/legacy/immutable", test_immutable);
    qtest_add_func("/sgi/crime/legacy/level-latches", test_level_latches);
    qtest_add_func("/sgi/crime/legacy/source-routing", test_source_routing);
    qtest_add_func("/sgi/crime/legacy/source-reset", test_source_reset);
    qtest_add_func("/sgi/crime/legacy/counter", test_counter);
    qtest_add_data_func("/sgi/crime/legacy/migration",
                        GINT_TO_POINTER(1), test_migration);
    qtest_add_data_func("/sgi/crime/default/migration-sources",
                        GINT_TO_POINTER(0), test_migration);
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
    return g_test_run();
}
