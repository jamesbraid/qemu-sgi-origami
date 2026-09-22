/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Router register target tests. The host fixture does not model a fabric. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "sgi-migrate.h"

#define RR_DIAG_PARMS              0x00030
#define RR_PORT_RESET              0x00008
#define RR_PROTECTION_CONFIG       0x00010
#define RR_SCRATCH_REG0            0x00020
#define RR_BIST_DATA               0x00050
#define RR_BIST_READY              0x00058
#define RR_BIST_COUNT_TARGET       0x00060
#define RR_BIST_SHIFT_LOAD         0x00068
#define RR_BIST_SHIFT_UNLOAD       0x00070
#define RR_BIST_ENTER_RUN          0x00078
#define RR_PORT_PARMS(port)        ((uint64_t)(port) << 16)
#define RR_STATUS_ERROR(port)      (RR_PORT_PARMS(port) + 0x08)
#define RR_HISTOGRAM(port)         (RR_PORT_PARMS(port) + 0x10)
#define PORT_HISTOGRAM_MODE(mode)  ((uint64_t)(mode) << 18)
#define PORT_HISTOGRAM_VC(vc)      ((uint64_t)(vc) << 16)
#define PORT_PARMS_RESET           UINT64_C(0x1bf0)
#define DIAG_ABSOLUTE_HISTOGRAM    (UINT64_C(1) << 19)
#define BIST_TARGET_LOGIC          1
#define BIST_TARGET_ARRAY          2
#define BIST_TARGET_CLOCK          3
#define BIST_COMPLETE_RAW          (UINT64_C(1) << 62)
#define PORT_RESET_ADMIN           (UINT64_C(1) << 0)
#define PORT_RESET_PORT(port)      (UINT64_C(1) << (port))
#define PORT_RESET_WARM            (UINT64_C(1) << 7)

static void set(QTestState *qts, const char *name, uint64_t value)
{
    qtest_qmp_assert_success(qts,
        "{'execute':'qom-set','arguments':{'path':'/machine/peripheral/router',"
        "'property':%s,'value':%llu}}", name, (unsigned long long)value);
}

static uint64_t get(QTestState *qts, const char *name)
{
    QDict *response = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/peripheral/router',"
        "'property':%s}}", name);
    uint64_t value;

    g_assert_true(qdict_haskey(response, "return"));
    value = qdict_get_int(response, "return");
    qobject_unref(response);
    return value;
}

static uint64_t read_reg(QTestState *qts, uint64_t addr)
{
    set(qts, "address", addr);
    return get(qts, "register");
}

static void write_reg(QTestState *qts, uint64_t addr, uint64_t value)
{
    set(qts, "address", addr);
    set(qts, "register", value);
}

static QTestState *start(const char *extra)
{
    return qtest_initf("-M none -device sgi-router-test,id=router %s", extra);
}

static void bist_shift_load(QTestState *qts, unsigned int target,
                            unsigned int count, uint64_t data)
{
    write_reg(qts, RR_BIST_DATA, data);
    write_reg(qts, RR_BIST_COUNT_TARGET,
              ((uint64_t)target << 6) | (count & 0x3f));
    write_reg(qts, RR_BIST_SHIFT_LOAD, 0);
}

static void bist_shift_unload(QTestState *qts, unsigned int target,
                              unsigned int count)
{
    write_reg(qts, RR_BIST_COUNT_TARGET,
              ((uint64_t)target << 6) | (count & 0x3f));
    write_reg(qts, RR_BIST_SHIFT_UNLOAD, 0);
}

static void bist_run(QTestState *qts, unsigned int target)
{
    bist_shift_load(qts, BIST_TARGET_CLOCK, 1, 1);
    if (target == BIST_TARGET_LOGIC) {
        bist_shift_load(qts, target, 64, UINT64_MAX);
        bist_shift_load(qts, target, 64, UINT64_MAX);
        bist_shift_load(qts, target, 63, UINT64_MAX >> 1);
    } else {
        bist_shift_load(qts, target, 64, UINT64_MAX);
        bist_shift_load(qts, target, 46, (UINT64_C(1) << 46) - 1);
    }
    write_reg(qts, RR_BIST_ENTER_RUN, 0);
}

static void test_bist_and_programmed_resets(void)
{
    QTestState *qts = start("");
    const uint64_t working = UINT64_C(1) << (26 + 3 * 3);
    const uint64_t width = working << 1;

    g_assert_cmphex(read_reg(qts, RR_BIST_READY), ==, 1);

    bist_run(qts, BIST_TARGET_LOGIC);
    g_assert_cmphex(read_reg(qts, RR_BIST_READY), ==, 1);
    bist_shift_unload(qts, BIST_TARGET_LOGIC, 64);
    g_assert_cmphex(read_reg(qts, RR_BIST_DATA), ==, BIST_COMPLETE_RAW);

    bist_run(qts, BIST_TARGET_ARRAY);
    bist_shift_unload(qts, BIST_TARGET_ARRAY, 64);
    g_assert_cmphex(read_reg(qts, RR_BIST_DATA), ==, BIST_COMPLETE_RAW);
    bist_shift_unload(qts, BIST_TARGET_ARRAY, 46);
    g_assert_cmphex(read_reg(qts, RR_BIST_DATA), ==, 0);

    /* The status bit keeps its stream position across partial unloads. */
    bist_run(qts, BIST_TARGET_ARRAY);
    bist_shift_unload(qts, BIST_TARGET_ARRAY, 31);
    g_assert_cmphex(read_reg(qts, RR_BIST_DATA), ==, 0);
    bist_shift_unload(qts, BIST_TARGET_ARRAY, 32);
    g_assert_cmphex(read_reg(qts, RR_BIST_DATA), ==, UINT64_C(1) << 31);
    bist_shift_unload(qts, BIST_TARGET_ARRAY, 47);
    g_assert_cmphex(read_reg(qts, RR_BIST_DATA), ==, 0);

    /* An incomplete selected chain does not acquire a completion status. */
    bist_shift_load(qts, BIST_TARGET_CLOCK, 1, 1);
    bist_shift_load(qts, BIST_TARGET_ARRAY, 64, UINT64_MAX);
    write_reg(qts, RR_BIST_ENTER_RUN, 0);
    bist_shift_unload(qts, BIST_TARGET_ARRAY, 64);
    g_assert_cmphex(read_reg(qts, RR_BIST_DATA), ==, 0);

    set(qts, "port", 3);
    set(qts, "link", 3);
    set(qts, "port-errors", 0x1234);
    write_reg(qts, RR_SCRATCH_REG0, 0x1234);
    write_reg(qts, RR_PROTECTION_CONFIG, 0x12345);

    /* A selected port reset leaves all modeled administrative state alone. */
    write_reg(qts, RR_PORT_RESET, PORT_RESET_PORT(3));
    g_assert_cmphex(read_reg(qts, RR_SCRATCH_REG0), ==, 0x1234);
    g_assert_cmphex(read_reg(qts, RR_STATUS_ERROR(3)), ==, 0x1234);
    g_assert_cmphex(read_reg(qts, 0) & (working | width), ==,
                    working | width);

    /* Administrative reset clears programming while preserving link state. */
    write_reg(qts, RR_PORT_RESET, PORT_RESET_ADMIN);
    g_assert_cmphex(read_reg(qts, RR_SCRATCH_REG0), ==, 0);
    g_assert_cmphex(read_reg(qts, RR_PROTECTION_CONFIG), ==, 0x3f);
    g_assert_cmphex(read_reg(qts, RR_STATUS_ERROR(3)), ==, 0);
    g_assert_cmphex(read_reg(qts, 0) & (working | width), ==,
                    working | width);

    write_reg(qts, RR_SCRATCH_REG0, 0x5678);
    write_reg(qts, RR_PORT_RESET, PORT_RESET_WARM | PORT_RESET_ADMIN);
    g_assert_cmphex(read_reg(qts, RR_SCRATCH_REG0), ==, 0);
    g_assert_cmphex(read_reg(qts, 0) & (working | width), ==, 0);
    g_assert_cmphex(read_reg(qts, RR_BIST_READY), ==, 1);
    qtest_quit(qts);
}

static unsigned int substring_count(const char *text, const char *needle)
{
    unsigned int count = 0;

    while ((text = strstr(text, needle))) {
        count++;
        text += strlen(needle);
    }
    return count;
}

static void test_registers(void)
{
    QTestState *qts = start("");

    set(qts, "port", 5);
    g_assert_cmphex(read_reg(qts, 0), ==, (UINT64_C(5) << 46) | 0x1111);
    g_assert_cmphex(read_reg(qts, 0x10), ==, 0x3f);
    g_assert_cmphex(read_reg(qts, 0x18), ==, UINT64_C(0x3fffffe0a01ffff1));
    write_reg(qts, 0x20, UINT64_C(0x123456789abcdef0));
    g_assert_cmphex(read_reg(qts, 0x20), ==, UINT64_C(0x123456789abcdef0));
    write_reg(qts, 0x28, UINT64_C(0x12345678));
    g_assert_cmphex(read_reg(qts, 0x28), ==, 0x5678);
    write_reg(qts, 0x28, 0);
    set(qts, "exchange", 0xffff);
    g_assert_cmphex(get(qts, "exchange"), ==, 0);
    set(qts, "exchange", 0x1234);
    g_assert_cmphex(get(qts, "exchange"), ==, 0xffff);
    g_assert_cmphex(read_reg(qts, 0x28), ==, 0xffff);
    for (unsigned int i = 0; i < 48; i++) {
        uint64_t address = 0x70000 + i * 8;

        write_reg(qts, address, UINT64_C(0xabc00000) | i);
        g_assert_cmphex(read_reg(qts, address), ==, 0xc00000 | i);
    }
    for (unsigned int port = 1; port <= 6; port++) {
        uint64_t base = port << 16;

        g_assert_cmphex(read_reg(qts, base), ==, 0x1bf0);
        g_assert_cmphex(read_reg(qts, base + 8), ==, 0);
        g_assert_cmphex(read_reg(qts, base + 0x10), ==, 0);
        g_assert_cmphex(read_reg(qts, base + 0x18), ==, 0x3f);
        write_reg(qts, base, 0x123fffff);
        g_assert_cmphex(read_reg(qts, base), ==, 0xfffff);
        write_reg(qts, base + 0x18, 0x40 | port);
        g_assert_cmphex(read_reg(qts, base + 0x18), ==, port);
        set(qts, "port", port);
        set(qts, "port-errors", 0x12345);
        write_reg(qts, base + 8, 0x45);
        g_assert_cmphex(read_reg(qts, base + 0x88), ==, 0x12300);
        g_assert_cmphex(read_reg(qts, base + 8), ==, 0);
    }
    qtest_quit(qts);
}

static void test_links_reset(void)
{
    QTestState *qts = start("");

    for (unsigned int port = 1; port <= 6; port++) {
        uint64_t working = UINT64_C(1) << (26 + 3 * port);
        uint64_t width = working << 1;

        set(qts, "port", port);
        set(qts, "link", 3);
        g_assert_cmphex(read_reg(qts, 0) & (working | width), ==,
                        working | width);
        set(qts, "link", 0);
        g_assert_cmphex(read_reg(qts, 0) & (working | width), ==, 0);
        set(qts, "link", 1);
        g_assert_cmphex(read_reg(qts, 0) & (working | width), ==, working);
        write_reg(qts, 0x30, (UINT64_C(1) << (5 + port)) |
                            (UINT64_C(1) << (11 + port)));
        g_assert_cmphex(get(qts, "diagnostic"), ==, 2);
        write_reg(qts, 0x30, 0);
        g_assert_cmphex(get(qts, "diagnostic"), ==, 1);
        write_reg(qts, (port << 16) + 0x18, port);
    }
    write_reg(qts, 0x20, 0x12345);
    write_reg(qts, 0x10, 0x1fffff);
    write_reg(qts, 0x70120, 0x123456);
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(1) |
              PORT_HISTOGRAM_VC(3));
    set(qts, "port", 1);
    set(qts, "histogram-receive", 7);
    write_reg(qts, RR_DIAG_PARMS, DIAG_ABSOLUTE_HISTOGRAM);
    qtest_system_reset(qts);
    g_assert_cmphex(read_reg(qts, 0x20), ==, 0);
    g_assert_cmphex(read_reg(qts, 0x10), ==, 0x3f);
    g_assert_cmphex(read_reg(qts, RR_DIAG_PARMS), ==, 0);
    g_assert_cmphex(read_reg(qts, RR_HISTOGRAM(1)), ==, 0);
    g_assert_cmphex(read_reg(qts, RR_PORT_PARMS(1)), ==,
                    PORT_PARMS_RESET);
    for (unsigned int port = 1; port <= 6; port++) {
        set(qts, "port", port);
        g_assert_cmphex(get(qts, "diagnostic"), ==, 1);
        g_assert_cmphex(read_reg(qts, (port << 16) + 0x18), ==, 0x3f);
        g_assert_cmphex(read_reg(qts, 0) &
                        (UINT64_C(3) << (26 + 3 * port)), ==, 0);
    }
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(1));
    qtest_clock_step(qts, 19);
    g_assert_cmphex(read_reg(qts, RR_HISTOGRAM(1)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(read_reg(qts, RR_HISTOGRAM(1)), ==,
                    UINT64_C(1) << 48);
    qtest_quit(qts);
}

static void test_histograms(void)
{
    QTestState *qts = start("");
    uint64_t before;

    set(qts, "port", 1);
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(1));
    qtest_clock_step(qts, 19);
    g_assert_cmphex(read_reg(qts, RR_HISTOGRAM(1)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(read_reg(qts, RR_HISTOGRAM(1)), ==,
                    UINT64_C(1) << 48);
    qtest_clock_step(qts, 40);

    set(qts, "histogram-receive", 6);
    set(qts, "histogram-receive", 5);
    set(qts, "histogram-receive", 3);
    set(qts, "histogram-receive", 7);
    set(qts, "histogram-send", 0);
    set(qts, "histogram-send", 1);
    before = UINT64_C(0x0003000100030001);
    g_assert_cmphex(read_reg(qts, RR_HISTOGRAM(1)), ==, before);

    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_VC(2));
    qtest_clock_step(qts, 40);
    g_assert_cmphex(read_reg(qts, RR_HISTOGRAM(1)), ==, before);
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(2) |
              PORT_HISTOGRAM_VC(3));
    qtest_clock_step(qts, 40);
    g_assert_cmphex(read_reg(qts, RR_HISTOGRAM(1)), ==, before);
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(3) |
              PORT_HISTOGRAM_VC(1));
    qtest_clock_step(qts, 40);
    g_assert_cmphex(read_reg(qts, RR_HISTOGRAM(1)), ==, before);
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(1));
    write_reg(qts, RR_DIAG_PARMS, DIAG_ABSOLUTE_HISTOGRAM);
    qtest_clock_step(qts, 40);
    g_assert_cmphex(read_reg(qts, RR_HISTOGRAM(1)), ==, before);

    write_reg(qts, RR_HISTOGRAM(1), UINT64_C(1) | (UINT64_C(1) << 48));
    g_assert_cmphex(read_reg(qts, RR_HISTOGRAM(1)), ==,
                    UINT64_C(0x0002000100030000));
    write_reg(qts, RR_HISTOGRAM(1), 0);
    g_assert_cmphex(read_reg(qts, RR_HISTOGRAM(1)), ==, 0);
    qtest_quit(qts);
}

static void test_histogram_common_rescale(void)
{
    QTestState *qts = start("");

    set(qts, "port", 1);
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(1));
    set(qts, "histogram-send-repeat", (UINT64_C(5) << 1) | 1);
    set(qts, "histogram-receive-repeat", (UINT64_C(0xffff) << 3) | 7);
    write_reg(qts, RR_HISTOGRAM(1),
              (UINT64_C(0xffff) ^ 2) << 16);
    set(qts, "histogram-receive", 7);
    g_assert_cmphex(read_reg(qts, RR_HISTOGRAM(1)), ==,
                    UINT64_C(0x0000000200018000));
    qtest_quit(qts);
}

static void test_histogram_unsupported_diagnostics(void)
{
    g_autofree char *path = NULL;
    g_autofree char *extra = NULL;
    g_autofree char *log = NULL;
    int fd = g_file_open_tmp("router-histogram-log-XXXXXX", &path, NULL);
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    extra = g_strdup_printf("-d unimp -D %s", path);
    qts = start(extra);
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(2));
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(2));
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(2) |
              PORT_HISTOGRAM_VC(3));
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(3));
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(3));
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(1));
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(2));
    write_reg(qts, RR_PORT_PARMS(1), PORT_PARMS_RESET);
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_VC(2));
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(1));
    write_reg(qts, RR_DIAG_PARMS, DIAG_ABSOLUTE_HISTOGRAM);
    write_reg(qts, RR_DIAG_PARMS, DIAG_ABSOLUTE_HISTOGRAM);
    write_reg(qts, RR_DIAG_PARMS, 0);
    write_reg(qts, RR_DIAG_PARMS, DIAG_ABSOLUTE_HISTOGRAM);
    qtest_system_reset(qts);
    write_reg(qts, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(1));
    qtest_clock_step(qts, 20);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(path, &log, NULL, NULL));
    g_assert_cmpuint(substring_count(log, "histogram selector 2 on port 1"),
                     ==, 2);
    g_assert_cmpuint(substring_count(log, "histogram selector 3 on port 1"),
                     ==, 1);
    g_assert_cmpuint(substring_count(log, "histogram selector 0 on port 1"),
                     ==, 1);
    g_assert_cmpuint(substring_count(log, "absolute histogram mode"), ==, 2);
    g_assert_cmpint(unlink(path), ==, 0);
}

static void test_migration(void)
{
    g_autofree char *directory = g_dir_make_tmp("router-migrate-XXXXXX", NULL);
    g_autofree char *socket = g_build_filename(directory, "socket", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *from = start("");
    QTestState *to = start("-incoming defer");

    for (unsigned int i = 0; i < 48; i++) {
        write_reg(from, 0x70000 + i * 8, 0x654300 | i);
    }
    write_reg(from, 0x10, 0x12345);
    write_reg(from, RR_PORT_PARMS(1),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(1));
    set(from, "port", 1);
    set(from, "histogram-receive", 7);
    set(from, "histogram-send", 1);
    qtest_clock_step(from, 50);
    g_assert_cmphex(read_reg(from, RR_HISTOGRAM(1)), ==,
                    UINT64_C(0x0002000100010001));
    write_reg(from, RR_PORT_PARMS(6),
              PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(2) |
              PORT_HISTOGRAM_VC(3));
    write_reg(from, RR_DIAG_PARMS,
              UINT64_C(0x12345) | DIAG_ABSOLUTE_HISTOGRAM);
    for (unsigned int port = 1; port <= 6; port++) {
        write_reg(from, (port << 16) + 0x18, port);
        set(from, "port", port);
        set(from, "link", port & 3);
    }
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed");
    /* The source completes when its state is sent, not when it is loaded. */
    sgi_wait_migration_status(to, "completed");
    for (unsigned int i = 0; i < 48; i++) {
        g_assert_cmphex(read_reg(to, 0x70000 + i * 8), ==, 0x654300 | i);
    }
    g_assert_cmphex(read_reg(to, 0x10), ==, 0x12345);
    g_assert_cmphex(read_reg(to, RR_DIAG_PARMS), ==,
                    UINT64_C(0x12345) | DIAG_ABSOLUTE_HISTOGRAM);
    g_assert_cmphex(read_reg(to, RR_HISTOGRAM(1)), ==,
                    UINT64_C(0x0002000100010001));
    g_assert_cmphex(read_reg(to, RR_PORT_PARMS(6)), ==,
                    PORT_PARMS_RESET | PORT_HISTOGRAM_MODE(2) |
                    PORT_HISTOGRAM_VC(3));
    for (unsigned int port = 1; port <= 6; port++) {
        g_assert_cmphex(read_reg(to, (port << 16) + 0x18), ==, port);
        set(from, "port", port);
        set(to, "port", port);
        g_assert_cmphex(read_reg(to, 0), ==, read_reg(from, 0));
    }
    write_reg(to, RR_DIAG_PARMS, 0x12345);
    qtest_clock_step(to, 9);
    g_assert_cmphex(read_reg(to, RR_HISTOGRAM(1)), ==,
                    UINT64_C(0x0002000100010001));
    qtest_clock_step(to, 1);
    g_assert_cmphex(read_reg(to, RR_HISTOGRAM(1)), ==,
                    UINT64_C(0x0003000100010001));
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn0/router/registers", test_registers);
    qtest_add_func("sn0/router/bist-and-programmed-resets",
                   test_bist_and_programmed_resets);
    qtest_add_func("sn0/router/links-reset", test_links_reset);
    qtest_add_func("sn0/router/histograms", test_histograms);
    qtest_add_func("sn0/router/histogram-common-rescale",
                   test_histogram_common_rescale);
    qtest_add_func("sn0/router/histogram-unsupported-diagnostics",
                   test_histogram_unsupported_diagnostics);
    qtest_add_func("sn0/router/migration", test_migration);
    return g_test_run();
}
