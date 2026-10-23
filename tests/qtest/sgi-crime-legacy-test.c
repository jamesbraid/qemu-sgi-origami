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
    partial = g_strdup_printf("%s,rom-image=%s,rom-size=524288,rom-offset=0,"
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
            size_t length = 2 + sizeof(name) - 1 + 4 + 30;

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
            stl_be_p(state + 6, 1U << 4);
            break;
        case 4:
            stl_be_p(state + 18, 8);
            break;
        case 5:
            stl_be_p(state + 26, 0x40000000);
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
    const unsigned unsupported[] = { 0x248, 0x260, 0x268, 0x4048, 0x50 };
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

int main(int argc, char **argv)
{
    const char *faults[] = {
        "missing-section", "profile-id", "functional-policy", "latch-mask",
        "cpu-error", "memory-address", "hardware", "timer-fraction", "version",
    };
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/sgi/crime/legacy/identity", test_identity);
    qtest_add_func("/sgi/crime/legacy/controls", test_controls);
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
    return g_test_run();
}
