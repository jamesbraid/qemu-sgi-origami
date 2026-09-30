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

#define ROUTER_NIC                 0x38
#define NIC_PHASES                 ((UINT64_C(47) << 27) | (UINT64_C(47) << 20))
#define NIC_RESET                  ((UINT64_C(49) << 27) | (UINT64_C(49) << 20) | 2)
#define DATA_SIZE                  2048

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
    sgi_wait_migration_status(from, "completed", 60);
    /* The source completes when its state is sent, not when it is loaded. */
    sgi_wait_migration_status(to, "completed", 60);
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

typedef struct NIC {
    QTestState *qts;
    unsigned int node;
} NIC;

static const char *nic_fixture_path(NIC *nic)
{
    return nic->node < 2 ? "/machine/peripheral/router0" :
                           "/machine/peripheral/router1";
}

static void nic_select_port(NIC *nic)
{
    qtest_qmp_assert_success(nic->qts,
        "{'execute':'qom-set','arguments':{'path':%s,'property':'port',"
        "'value':%u}}", nic_fixture_path(nic), nic->node == 1 ? 4 : 5);
}

static uint64_t nic_read(NIC *nic)
{
    QDict *response;
    uint64_t value;

    response = qtest_qmp(nic->qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':'register'}}",
        nic_fixture_path(nic));
    g_assert_true(qdict_haskey(response, "return"));
    value = qdict_get_int(response, "return");
    qobject_unref(response);
    return value;
}

static bool nic_request(NIC *nic, uint64_t value)
{
    QDict *response;
    bool accepted;

    nic_select_port(nic);
    response = qtest_qmp(nic->qts,
        "{'execute':'qom-set','arguments':{'path':%s,'property':'register',"
        "'value':%llu}}", nic_fixture_path(nic), (unsigned long long)value);
    accepted = qdict_haskey(response, "return");
    if (!accepted) {
        /* A test-fixture refusal, not an asserted hardware error reply. */
        g_assert_cmpstr(qdict_get_str(qdict_get_qdict(response, "error"),
                                      "desc"), ==,
                        "Router target refused the test request");
    }
    qobject_unref(response);
    return accepted;
}

static uint64_t nic_request_word(unsigned int pulse, unsigned int sample)
{
    return NIC_PHASES | ((uint64_t)pulse << 10) | ((uint64_t)sample << 2);
}

static bool nic_slot(NIC *nic, unsigned int pulse, unsigned int sample)
{
    g_assert_true(nic_request(nic, nic_request_word(pulse, sample)));
    g_assert_cmphex(nic_read(nic) & 3, ==, 0);
    qtest_clock_step(nic->qts, (pulse + sample) * 1000);
    g_assert_cmphex(nic_read(nic) & 2, ==, 2);
    return nic_read(nic) & 1;
}

static void nic_reset_bus(NIC *nic)
{
    g_assert_false(nic_slot(nic, 520, 65));
}

static void nic_write_bit(NIC *nic, bool bit)
{
    g_assert_true(nic_slot(nic, bit ? 6 : 80, bit ? 110 : 30));
}

static bool nic_read_bit(NIC *nic)
{
    return nic_slot(nic, 6, 13);
}

static void nic_write_byte(NIC *nic, uint8_t value)
{
    for (unsigned int bit = 0; bit < 8; bit++) {
        nic_write_bit(nic, (value >> bit) & 1);
    }
}

static uint8_t nic_read_byte(NIC *nic)
{
    uint8_t value = 0;

    for (unsigned int bit = 0; bit < 8; bit++) {
        value |= nic_read_bit(nic) << bit;
    }
    return value;
}

/* Check CRC bits independently of the DS2505 production helpers. */
static uint16_t nic_crc16(uint16_t crc, uint8_t byte)
{
    for (unsigned int bit = 0; bit < 8; bit++) {
        bool carry = (crc ^ (byte >> bit)) & 1;

        crc >>= 1;
        if (carry) {
            crc ^= 0xa001;
        }
    }
    return crc;
}

static uint8_t nic_crc8(const uint8_t *bytes, size_t len)
{
    uint8_t crc = 0;

    for (size_t i = 0; i < len; i++) {
        for (unsigned int bit = 0; bit < 8; bit++) {
            bool carry = (crc ^ (bytes[i] >> bit)) & 1;

            crc >>= 1;
            if (carry) {
                crc ^= 0x8c;
            }
        }
    }
    return crc;
}

static void nic_search_rom(NIC *nic, uint8_t rom[8])
{
    memset(rom, 0, 8);
    nic_reset_bus(nic);
    nic_write_byte(nic, 0xf0);
    for (unsigned int bit = 0; bit < 64; bit++) {
        bool value = nic_read_bit(nic);

        g_assert_cmpint(nic_read_bit(nic), ==, !value);
        rom[bit / 8] |= value << (bit % 8);
        nic_write_bit(nic, value);
    }
    g_assert_cmphex(rom[0], ==, 0x0b);
    g_assert_cmphex(nic_crc8(rom, 8), ==, 0);
}

static QTestState *nic_start(const char *extra)
{
    QTestState *qts = qtest_initf("-M none -d unimp,guest_errors "
                       "-global sgi-router.nic-clock-hz=97500000 "
                       "-device sgi-router-test,id=router0,nic-serial=1 "
                       "-device sgi-router-test,id=router1,nic-serial=2 %s",
                       extra);

    /*
     * This test only addresses the NIC; polls need not reselect its address.
     */
    for (unsigned int node = 0; node <= 2; node += 2) {
        NIC nic = { qts, node };

        qtest_qmp_assert_success(qts,
            "{'execute':'qom-set','arguments':{'path':%s,'property':'address',"
            "'value':%u}}", nic_fixture_path(&nic), ROUTER_NIC);
    }
    return qts;
}

static void test_nic_search(void)
{
    QTestState *qts = nic_start("");
    NIC first = { qts, 0 }, other_port = { qts, 1 }, second = { qts, 2 };
    uint8_t a[8], b[8], c[8];

    nic_search_rom(&first, a);
    nic_search_rom(&other_port, b);
    nic_search_rom(&second, c);
    g_assert_cmpmem(a, 8, b, 8);
    g_assert_cmpint(memcmp(a, c, 8), !=, 0);
    nic_reset_bus(&first);
    nic_write_byte(&first, 0xf0);
    g_assert_true(nic_read_bit(&first)); /* Family 0x0b starts with one. */
    g_assert_false(nic_read_bit(&first));
    nic_write_bit(&first, false);
    for (unsigned int i = 0; i < 12; i++) {
        g_assert_true(nic_read_bit(&first));
    }
    nic_search_rom(&first, b);
    g_assert_cmpmem(a, 8, b, 8);
    qtest_quit(qts);
}

static void test_nic_timing(void)
{
    QTestState *qts = nic_start("");
    NIC nic = { qts, 0 };
    uint64_t value;

    g_assert_cmphex(nic_read(&nic), ==, NIC_RESET);
    g_assert_true(nic_request(&nic, nic_request_word(520, 65) | 3));
    for (unsigned int i = 0; i < 10; i++) {
        g_assert_cmphex(nic_read(&nic), ==, NIC_PHASES);
    }
    g_assert_false(nic_request(&nic, nic_request_word(6, 13)));
    qtest_clock_step(qts, 519999);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 0);
    qtest_clock_step(qts, 64999);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 2);
    value = nic_read(&nic);
    g_assert_false(nic_request(&nic, nic_request_word(6, 14)));
    g_assert_false(nic_request(&nic, nic_request_word(6, 13) ^ (UINT64_C(1) << 27)));
    g_assert_false(nic_request(&nic, (NIC_RESET & ~UINT64_C(3)) |
                                  (UINT64_C(6) << 10) | (13 << 2)));
    g_assert_cmphex(nic_read(&nic), ==, value);
    nic_write_byte(&nic, 0x33);
    g_assert_true(nic_read_bit(&nic));
    g_assert_true(nic_read_bit(&nic));
    /* Retain bit two (zero) beyond the DS2505's 15us guaranteed window. */
    g_assert_true(nic_request(&nic, nic_request_word(6, 13)));
    qtest_clock_step(qts, 15000);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 0);
    qtest_clock_step(qts, 3999);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(nic_read(&nic) & 3, ==, 2);
    g_assert_true(nic_read_bit(&nic)); /* One consumed bit: bit three is one. */
    for (unsigned int pulse = 6; pulse <= 80; pulse += 74) {
        unsigned int sample = pulse == 6 ? 110 : 30;

        g_assert_true(nic_request(&nic, nic_request_word(pulse, sample)));
        qtest_clock_step(qts, (pulse + sample) * 1000 - 1);
        g_assert_cmphex(nic_read(&nic) & 3, ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(nic_read(&nic) & 3, ==, 3);
    }
    qtest_quit(qts);
}

static void test_nic_absent(void)
{
    QTestState *qts = nic_start("-global sgi-router.nic-present=off");
    NIC nic = { qts, 0 };

    g_assert_true(nic_slot(&nic, 520, 65));
    g_assert_true(nic_read_bit(&nic));
    g_assert_true(nic_slot(&nic, 80, 30));
    qtest_quit(qts);
}

static uint8_t nic_pattern(unsigned int address)
{
    return (address * 29 + (address >> 3) + 7) & 0xff;
}

static void nic_check_crc(NIC *nic, uint16_t crc)
{
    uint16_t actual = nic_read_byte(nic);

    actual |= (uint16_t)nic_read_byte(nic) << 8;
    g_assert_cmphex(actual, ==, (uint16_t)~crc);
}

static uint16_t nic_memory_header(NIC *nic, uint8_t command, uint16_t address)
{
    uint16_t crc = nic_crc16(0, command);

    nic_reset_bus(nic);
    nic_write_byte(nic, 0xcc);
    nic_write_byte(nic, command);
    nic_write_byte(nic, address);
    nic_write_byte(nic, address >> 8);
    crc = nic_crc16(crc, address);
    return nic_crc16(crc, address >> 8);
}

static void test_nic_memory(void)
{
    g_autofree char *path = NULL;
    uint8_t image[DATA_SIZE];
    int fd = g_file_open_tmp("router-nic-data-XXXXXX", &path, NULL);
    QTestState *qts;
    NIC nic;
    uint16_t crc;

    g_assert_cmpint(fd, >=, 0);
    for (unsigned int i = 0; i < DATA_SIZE; i++) {
        image[i] = nic_pattern(i);
    }
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    close(fd);
    {
        g_autofree char *extra = g_strdup_printf(
            "-global sgi-router.nic-image=%s", path);

        qts = nic_start(extra);
    }
    nic = (NIC) { qts, 0 };
    crc = nic_memory_header(&nic, 0xf0, 29);
    for (unsigned int i = 29; i < DATA_SIZE; i++) {
        uint8_t byte = nic_read_byte(&nic);

        g_assert_cmphex(byte, ==, image[i]);
        crc = nic_crc16(crc, byte);
    }
    nic_check_crc(&nic, crc);
    g_assert_cmphex(nic_read_byte(&nic), ==, 0xff);
    crc = nic_memory_header(&nic, 0xa5, 29);
    g_assert_cmphex(nic_read_byte(&nic), ==, 0xff);
    nic_check_crc(&nic, nic_crc16(crc, 0xff));
    for (unsigned int page = 0; page < 2; page++) {
        unsigned int begin = page ? 32 : 29;
        unsigned int end = page ? 64 : 32;

        crc = 0;
        for (unsigned int i = begin; i < end; i++) {
            uint8_t byte = nic_read_byte(&nic);

            g_assert_cmphex(byte, ==, image[i]);
            crc = nic_crc16(crc, byte);
        }
        nic_check_crc(&nic, crc);
        if (!page) {
            g_assert_cmphex(nic_read_byte(&nic), ==, 0xff);
            nic_check_crc(&nic, nic_crc16(0, 0xff));
        }
    }
    crc = nic_memory_header(&nic, 0xaa, 3);
    for (unsigned int i = 3; i < 8; i++) {
        g_assert_cmphex(nic_read_byte(&nic), ==, 0xff);
        crc = nic_crc16(crc, 0xff);
    }
    nic_check_crc(&nic, crc);
    qtest_quit(qts);
    unlink(path);
}

static void test_nic_reset(void)
{
    QTestState *qts = nic_start("");
    NIC nic = { qts, 0 };
    uint8_t original[8], after[8];
    const int64_t elapsed[] = { 5000, 10000 };

    nic_search_rom(&nic, original);
    for (unsigned int i = 0; i < G_N_ELEMENTS(elapsed); i++) {
        g_assert_true(nic_request(&nic, nic_request_word(6, 13)));
        qtest_clock_step(qts, elapsed[i]);
        qtest_system_reset(qts);
        g_assert_cmphex(nic_read(&nic), ==, NIC_RESET);
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(nic_read(&nic), ==, NIC_RESET);
        nic_search_rom(&nic, after);
        g_assert_cmpmem(original, 8, after, 8);
    }
    qtest_quit(qts);
}

static QTestState *nic_migrate(NIC *nic, int64_t now)
{
    g_autofree char *directory = g_dir_make_tmp("router-nic-migrate-XXXXXX",
                                                NULL);
    g_autofree char *socket = g_build_filename(directory, "socket", NULL);
    g_autofree char *uri = g_strdup_printf("unix:%s", socket);
    QTestState *to = nic_start("-incoming defer");

    /*
     * Qtest's explicit clock is a harness input, not guest migration state.
     */
    qtest_clock_set(to, now);
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(nic->qts,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(nic->qts, "completed", 60);
    /*
     * The source completes once its state is sent, which can be before the
     * destination has loaded it; a read in between returns reset values.
     */
    sgi_wait_migration_status(to, "completed", 60);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
    return to;
}

static void test_nic_migration(void)
{
    const int64_t elapsed[] = { 5000, 10000 };

    for (unsigned int i = 0; i < G_N_ELEMENTS(elapsed); i++) {
        QTestState *from = nic_start("");
        NIC source = { from, 0 }, target;
        uint8_t rom[8];
        int64_t now;

        nic_search_rom(&source, rom);
        nic_reset_bus(&source);
        nic_write_byte(&source, 0xf0);
        g_assert_true(nic_read_bit(&source));
        /* Migrate the complement slot before/after its bus advancement. */
        g_assert_true(nic_request(&source, nic_request_word(6, 13)));
        now = qtest_clock_step(from, elapsed[i]);

        target = (NIC) { nic_migrate(&source, now), 0 };
        g_assert_cmphex(nic_read(&target), ==, nic_read(&source));
        qtest_clock_step(target.qts, 19000 - elapsed[i] - 1);
        g_assert_cmphex(nic_read(&target) & 3, ==, 0);
        qtest_clock_step(target.qts, 1);
        g_assert_cmphex(nic_read(&target) & 3, ==, 2);
        nic_write_bit(&target, true);
        for (unsigned int bit = 1; bit < 64; bit++) {
            bool expected = (rom[bit / 8] >> (bit % 8)) & 1;

            g_assert_cmpint(nic_read_bit(&target), ==, expected);
            g_assert_cmpint(nic_read_bit(&target), ==, !expected);
            nic_write_bit(&target, expected);
        }
        qtest_quit(from);
        qtest_quit(target.qts);
    }
}

static void test_nic_migration_crc(void)
{
    QTestState *from = nic_start("");
    NIC source = { from, 0 }, target;
    uint16_t crc = nic_memory_header(&source, 0xf0, DATA_SIZE - 3);
    uint16_t expected;
    uint8_t actual = 0;
    int64_t now;

    for (unsigned int i = 0; i < 3; i++) {
        g_assert_cmphex(nic_read_byte(&source), ==, 0xff);
        crc = nic_crc16(crc, 0xff);
    }
    expected = ~crc;
    g_assert_cmphex(nic_read_byte(&source), ==, (uint8_t)expected);
    /*
     * Preserve a partial CRC byte, including a latched awaiting-sample bit.
     */
    for (unsigned int bit = 0; bit < 3; bit++) {
        actual |= nic_read_bit(&source) << bit;
    }
    g_assert_true(nic_request(&source, nic_request_word(6, 13)));
    now = qtest_clock_step(from, 10000);

    target = (NIC) { nic_migrate(&source, now), 0 };
    qtest_clock_step(target.qts, 9000);
    actual |= (nic_read(&target) & 1) << 3;
    for (unsigned int bit = 4; bit < 8; bit++) {
        actual |= nic_read_bit(&target) << bit;
    }
    g_assert_cmphex(actual, ==, expected >> 8);
    g_assert_cmphex(nic_read_byte(&target), ==, 0xff);
    qtest_quit(from);
    qtest_quit(target.qts);
}

static void test_nic_oversize_image(void)
{
    g_autofree char *path = NULL;
    g_autofree char *output = NULL;
    g_autofree char *property = NULL;
    int fd = g_file_open_tmp("router-nic-oversize-XXXXXX", &path, NULL);
    int status;
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "none", "-display", "none",
        "-S", "-global", NULL, "-device", "sgi-router-test", NULL,
    };

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, DATA_SIZE + 1), ==, 0);
    close(fd);
    property = g_strdup_printf("sgi-router.nic-image=%s", path);
    args[7] = property;
    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &output, &status, NULL));
    unlink(path);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), !=, 0);
    g_assert_nonnull(strstr(output, "exceeds 2048 bytes"));
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
    qtest_add_func("sn0/router-nic/search", test_nic_search);
    qtest_add_func("sn0/router-nic/timing", test_nic_timing);
    qtest_add_func("sn0/router-nic/absent", test_nic_absent);
    qtest_add_func("sn0/router-nic/memory", test_nic_memory);
    qtest_add_func("sn0/router-nic/reset", test_nic_reset);
    qtest_add_func("sn0/router-nic/migration", test_nic_migration);
    qtest_add_func("sn0/router-nic/migration-crc", test_nic_migration_crc);
    qtest_add_func("sn0/router-nic/oversize-image", test_nic_oversize_image);
    return g_test_run();
}
