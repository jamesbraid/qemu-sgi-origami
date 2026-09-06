/*
 * SGI Origin 300 (IP45) address-space routing.
 *
 * These are machine-bus addresses after the R10000-family uncached
 * attribute has been carried above the 40-bit physical address.  The
 * values are repeated here rather than shared with the machine so a bad
 * decoder constant cannot make both sides agree.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define MACHINE_ARGS "-M origin300 -cpu R14000 -smp 2"

#define SPACE_HUB UINT64_C(0x10000000000)
#define SPACE_XTALK UINT64_C(0x20000000000)
#define SPACE_MEMORY_SPECIAL UINT64_C(0x30000000000)

#define BEDROCK_LOCAL (SPACE_XTALK + UINT64_C(0x01000000))
#define BEDROCK_NODE0 (SPACE_XTALK + UINT64_C(0x01800000))
#define BEDROCK_PI1 UINT64_C(0x200000)
#define BEDROCK_IO UINT64_C(0x400000)
#define BEDROCK_LB UINT64_C(0x600000)
#define BEDROCK_NI UINT64_C(0x680000)
#define BEDROCK_WINDOW_SIZE UINT64_C(0x800000)

#define PI_CPU_NUMBER UINT64_C(0x20)
#define PI_CPU_ENABLE_B UINT64_C(0x58)
#define IO_WIDGET_ID UINT64_C(0x0)
#define IO_SCRATCH0 UINT64_C(0x150)
#define IO_ITTE0 UINT64_C(0x160)
#define LB_SCRATCH2 UINT64_C(0x8010)
#define NI_PORT_STATUS UINT64_C(0x0)

#define ITTE_WIDGET(w) ((uint64_t)(w) << 8)
#define BIG_WINDOW(n) (SPACE_XTALK + UINT64_C(0x20000000) + \
                       (uint64_t)(n) * UINT64_C(0x20000000))

#define HUB_LOCAL_REGISTERS (SPACE_HUB + UINT64_C(0x10000000))
#define HUB_LOCAL_BOOT (SPACE_HUB + UINT64_C(0x18000000))
#define HUB_REMOTE_REGISTERS (SPACE_HUB + UINT64_C(0x30000000))
#define HUB_REMOTE_BOOT (SPACE_HUB + UINT64_C(0x38000000))
#define L1_UART UINT64_C(0x80)
#define L1_SCRATCH UINT64_C(0x38)

#define PROM_RESET UINT64_C(0x1fc00000)
#define PROM_SIZE UINT64_C(0x1686a8)
#define PROM_CONFIG_OFFSET UINT64_C(0x60)

#define BEDROCK_WIDGET_PART UINT64_C(0xc110)
#define BEDROCK_WIDGET_MFG UINT64_C(0x036)
#define WIDGET_PART(v) (((v) >> 12) & 0xffff)
#define WIDGET_MFG(v) (((v) >> 1) & 0x7ff)

static char *prom_path;

static char *create_prom(uint8_t first, uint8_t config, uint8_t last)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    image[0] = first;
    image[PROM_CONFIG_OFFSET] = config;
    image[PROM_SIZE - 1] = last;

    fd = g_file_open_tmp("sn1-address-map-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static char *create_cpu_control_prom(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(PROM_SIZE);
    char *path = NULL;
    int fd;

    stl_be_p(image + 0x000, 0x100000ff); /* beq zero, zero, 0x400 */
    for (unsigned int offset = 0x200; offset <= 0x380; offset += 0x80) {
        stl_be_p(image + offset, 0x1000ffff); /* beq zero, zero, self */
    }

    /* Each CPU increments its own t0 while it is runnable. */
    stl_be_p(image + 0x400, 0x65080001); /* daddiu t0, t0, 1 */
    stl_be_p(image + 0x404, 0x1000fffe); /* beq zero, zero, 0x400 */

    fd = g_file_open_tmp("sn1-cpu-control-prom-XXXXXX.bin", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, PROM_SIZE), ==, (ssize_t)PROM_SIZE);
    g_assert_cmpint(close(fd), ==, 0);
    return path;
}

static QTestState *machine_start_with(const char *path, const char *extra)
{
    g_autofree char *quoted = g_shell_quote(path);

    return qtest_initf(MACHINE_ARGS " -bios %s %s", quoted, extra);
}

static QTestState *machine_start(void)
{
    return machine_start_with(prom_path, "");
}

static uint64_t cpu_gpr(QTestState *qts, unsigned int cpu, const char *name)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers %u", cpu);
    g_autofree char *field = g_strdup_printf("%s ", name);
    const char *value_text = strstr(registers, field);
    uint64_t value;

    g_assert_nonnull(value_text);
    value_text += strlen(field);
    g_assert_cmpint(sscanf(value_text, "%" SCNx64, &value), ==, 1);
    return value;
}

static void stop_vm(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{'execute':'stop'}");
    qtest_qmp_eventwait(qts, "STOP");
}

static void run_vm_for_10ms(QTestState *qts)
{
    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    g_usleep(10 * 1000);
    stop_vm(qts);
}

static void wait_for_migration(QTestState *from)
{
    int64_t deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

    for (;;) {
        g_autoptr(QDict) response = qtest_qmp(from,
                                             "{'execute':'query-migrate'}");
        QDict *result = qdict_get_qdict(response, "return");
        const char *status = qdict_get_str(result, "status");

        if (!strcmp(status, "completed")) {
            return;
        }
        g_assert_cmpstr(status, !=, "failed");
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_usleep(1000);
    }
}

static void check_not_writable(QTestState *qts, uint64_t address)
{
    uint64_t before = qtest_readq(qts, address);

    qtest_writeq(qts, address, ~before);
    g_assert_cmphex(qtest_readq(qts, address), ==, before);
}

static void test_ram_and_hub_alias(const void *data)
{
    QTestState *qts = machine_start();

    qtest_writeq(qts, 0x1000, UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(qts, SPACE_HUB + 0x1000), ==,
                    UINT64_C(0x0123456789abcdef));

    qtest_writeq(qts, SPACE_HUB + 0x2000,
                 UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(qts, 0x2000), ==,
                    UINT64_C(0xfedcba9876543210));

    /* The local uncached alias is exactly 256 MiB. */
    check_not_writable(qts, SPACE_HUB + 256 * MiB);
    /* Node memory ends at the selected 512 MiB population. */
    check_not_writable(qts, 512 * MiB);
    /* Memory-special has no implemented operation in this skeleton. */
    check_not_writable(qts, SPACE_MEMORY_SPECIAL + 0x1000);

    qtest_quit(qts);
}

static void test_bedrock_register_aliases(const void *data)
{
    QTestState *qts = machine_start();
    const uint64_t scratch = UINT64_C(0x8badf00d12345678);

    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + PI_CPU_NUMBER), ==, 0);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_PI1 +
                                PI_CPU_NUMBER), ==, 2);

    qtest_writeq(qts, BEDROCK_LOCAL + BEDROCK_LB + LB_SCRATCH2, scratch);
    g_assert_cmphex(qtest_readq(qts, BEDROCK_NODE0 + BEDROCK_LB +
                                LB_SCRATCH2), ==, scratch);

    g_assert_cmphex(qtest_readq(qts, BEDROCK_LOCAL + BEDROCK_NI +
                                NI_PORT_STATUS) & (UINT64_C(1) << 18),
                    ==, 0);

    /* The 8 MiB architectural window does not turn its tail into storage. */
    check_not_writable(qts, BEDROCK_LOCAL + BEDROCK_WINDOW_SIZE - 8);
    check_not_writable(qts, BEDROCK_NODE0 + BEDROCK_WINDOW_SIZE - 8);

    qtest_quit(qts);
}

static void test_crosstalk_big_window_routes(const void *data)
{
    QTestState *qts = machine_start();
    uint64_t identity;

    /* Aim big window 0 at Bedrock's widget-1 face. */
    qtest_writeq(qts, BEDROCK_LOCAL + BEDROCK_IO + IO_ITTE0,
                 ITTE_WIDGET(1));
    identity = qtest_readq(qts, BIG_WINDOW(0) + IO_WIDGET_ID);
    g_assert_cmphex(WIDGET_PART(identity), ==, BEDROCK_WIDGET_PART);
    g_assert_cmphex(WIDGET_MFG(identity), ==, BEDROCK_WIDGET_MFG);

    /* Retargeting the translation entry removes that route. */
    qtest_writeq(qts, BEDROCK_LOCAL + BEDROCK_IO + IO_ITTE0,
                 ITTE_WIDGET(2));
    g_assert_cmphex(qtest_readq(qts, BIG_WINDOW(0) + IO_WIDGET_ID), !=,
                    identity);

    /* The gap between the small and big windows is not decoded. */
    check_not_writable(qts, SPACE_XTALK + UINT64_C(0x10000000));

    qtest_quit(qts);
}

static void test_hub_special_register_windows(const void *data)
{
    QTestState *qts = machine_start();
    const uint64_t scratch = UINT64_C(0x5a);

    qtest_writeq(qts, HUB_REMOTE_REGISTERS + L1_UART + L1_SCRATCH, scratch);
    g_assert_cmphex(qtest_readq(qts, HUB_REMOTE_REGISTERS + L1_UART +
                                L1_SCRATCH), ==, scratch);

    /* Public consumers establish the L1 decoder only in the remote form. */
    g_assert_cmphex(qtest_readq(qts, HUB_LOCAL_REGISTERS + L1_UART +
                                L1_SCRATCH), !=, scratch);
    check_not_writable(qts, HUB_REMOTE_REGISTERS + 0x1000);

    qtest_quit(qts);
}

static void test_prom_three_read_only_views(const void *data)
{
    QTestState *qts = machine_start();
    static const uint64_t views[] = {
        PROM_RESET, HUB_LOCAL_BOOT, HUB_REMOTE_BOOT,
    };
    static const struct {
        uint64_t offset;
        uint8_t value;
    } probes[] = {
        { 0, 0x31 },
        { PROM_CONFIG_OFFSET, 0x62 },
        { PROM_SIZE - 1, 0x93 },
    };

    for (size_t i = 0; i < ARRAY_SIZE(views); i++) {
        for (size_t j = 0; j < ARRAY_SIZE(probes); j++) {
            uint64_t address = views[i] + probes[j].offset;

            g_assert_cmphex(qtest_readb(qts, address), ==, probes[j].value);
            qtest_writeb(qts, address, probes[j].value ^ 0xff);
            g_assert_cmphex(qtest_readb(qts, address), ==, probes[j].value);
        }
    }

    /* Both boot aliases stop at the exact supplied length. */
    check_not_writable(qts, HUB_LOCAL_BOOT + PROM_SIZE);
    check_not_writable(qts, HUB_REMOTE_BOOT + PROM_SIZE);

    /* The reset view ends there too and reveals the RAM underneath. */
    qtest_writeq(qts, PROM_RESET + PROM_SIZE,
                 UINT64_C(0x7766554433221100));
    g_assert_cmphex(qtest_readq(qts, PROM_RESET + PROM_SIZE), ==,
                    UINT64_C(0x7766554433221100));

    qtest_quit(qts);
}

static void assert_flat_view_extent(QTestState *qts, uint64_t start,
                                    uint64_t size)
{
    g_autofree char *mtree = qtest_hmp(qts, "info mtree -f");
    g_autofree char *range = g_strdup_printf("%016" PRIx64 "-%016" PRIx64,
                                             start, start + size - 1);
    const char *hit = strstr(mtree, range);
    const char *line_end;
    g_autofree char *line = NULL;

    g_assert_nonnull(hit);
    line_end = strchr(hit, '\n');
    line = g_strndup(hit, line_end ? (size_t)(line_end - hit) : strlen(hit));
    g_assert_nonnull(strstr(line, "sn1.prom"));
}

static void test_prom_flat_view_extents(const void *data)
{
    QTestState *qts = machine_start();

    assert_flat_view_extent(qts, PROM_RESET, PROM_SIZE);
    assert_flat_view_extent(qts, HUB_LOCAL_BOOT, PROM_SIZE);
    assert_flat_view_extent(qts, HUB_REMOTE_BOOT, PROM_SIZE);

    qtest_quit(qts);
}

static void assert_qemu_rejects(const char *const args[], const char *message)
{
    g_autofree char *error_output = NULL;
    int wait_status = 0;

    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, 0, NULL, NULL,
                              NULL, &error_output, &wait_status, NULL));
    g_assert_false(g_spawn_check_exit_status(wait_status, NULL));
    g_assert_nonnull(strstr(error_output, message));
}

static void test_prom_size_is_exact(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *empty = NULL;
    g_autofree char *short_path = NULL;
    g_autofree char *long_path = NULL;
    int fd;

    fd = g_file_open_tmp("sn1-empty-prom-XXXXXX.bin", &empty, &error);
    g_assert_no_error(error);
    g_assert_cmpint(close(fd), ==, 0);

    fd = g_file_open_tmp("sn1-short-prom-XXXXXX.bin", &short_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(fd, PROM_SIZE - 1), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);

    fd = g_file_open_tmp("sn1-long-prom-XXXXXX.bin", &long_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(ftruncate(fd, PROM_SIZE + 1), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);

    const char *empty_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-bios", empty, "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };
    const char *short_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-bios", short_path, "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };
    const char *long_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-bios", long_path, "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };

    assert_qemu_rejects(empty_args, "must be exactly 1476264 bytes");
    assert_qemu_rejects(short_args, "must be exactly 1476264 bytes");
    assert_qemu_rejects(long_args, "must be exactly 1476264 bytes");

    g_assert_cmpint(g_unlink(empty), ==, 0);
    g_assert_cmpint(g_unlink(short_path), ==, 0);
    g_assert_cmpint(g_unlink(long_path), ==, 0);
}

static void test_prom_is_required(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };

    assert_qemu_rejects(args, "requires -bios");
}

static void test_cpu_count_is_exact(const void *data)
{
    const char *one_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "1", "-bios", prom_path, "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };
    const char *three_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "3", "-bios", prom_path, "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };

    assert_qemu_rejects(one_args, "Invalid SMP CPUs 1");
    assert_qemu_rejects(three_args, "Invalid SMP CPUs 3");
}

static void test_ram_size_is_exact(const void *data)
{
    const char *small_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-m", "256M", "-bios", prom_path,
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL,
    };
    const char *large_args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R14000",
        "-smp", "2", "-m", "1G", "-bios", prom_path,
        "-display", "none", "-kernel", "/definitely/not/a/kernel", NULL,
    };

    assert_qemu_rejects(small_args, "requires 512 MiB of RAM");
    assert_qemu_rejects(large_args, "requires 512 MiB of RAM");
}

static void test_cpu_type_is_r14000(const void *data)
{
    const char *args[] = {
        qtest_qemu_binary(NULL), "-M", "origin300", "-cpu", "R4000",
        "-smp", "2", "-bios", prom_path, "-display", "none",
        "-kernel", "/definitely/not/a/kernel", NULL,
    };

    assert_qemu_rejects(args, "Invalid CPU model: R4000");
}

static void test_cpu_b_follows_enable(const void *data)
{
    g_autofree char *path = NULL;
    QTestState *qts;
    uint64_t stopped;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    path = create_cpu_control_prom();
    qts = machine_start_with(
        path, "-accel tcg,thread=multi -no-shutdown -S");

    run_vm_for_10ms(qts);
    g_assert_cmphex(cpu_gpr(qts, 0, "t0"), !=, 0);
    stopped = cpu_gpr(qts, 1, "t0");
    g_assert_cmphex(stopped, ==, 0);

    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 0);
    run_vm_for_10ms(qts);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), ==, stopped);

    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    run_vm_for_10ms(qts);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), !=, stopped);

    qtest_qmp_assert_success(qts, "{'execute':'cont'}");
    g_usleep(10 * 1000);
    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 0);
    g_usleep(10 * 1000);
    stop_vm(qts);
    stopped = cpu_gpr(qts, 1, "t0");
    run_vm_for_10ms(qts);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), ==, stopped);

    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 1);
    qtest_system_reset(qts);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), ==, 0);
    run_vm_for_10ms(qts);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), !=, 0);

    qtest_writeq(qts, BEDROCK_LOCAL + PI_CPU_ENABLE_B, 0);
    qtest_system_reset(qts);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), ==, 0);
    run_vm_for_10ms(qts);
    g_assert_cmphex(cpu_gpr(qts, 0, "t0"), !=, 0);
    g_assert_cmphex(cpu_gpr(qts, 1, "t0"), ==, 0);

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(path), ==, 0);
}

static void test_prom_migration(const void *data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sn1-prom-migration-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *destination_prom = create_prom(0xa1, 0xb2, 0xc3);
    QTestState *from;
    QTestState *to;
    static const uint64_t views[] = {
        PROM_RESET, HUB_LOCAL_BOOT, HUB_REMOTE_BOOT,
    };
    static const struct {
        uint64_t offset;
        uint8_t source;
        uint8_t destination;
    } probes[] = {
        { 0, 0x31, 0xa1 },
        { PROM_CONFIG_OFFSET, 0x62, 0xb2 },
        { PROM_SIZE - 1, 0x93, 0xc3 },
    };

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = machine_start();
    to = machine_start_with(destination_prom, "-incoming defer");

    for (size_t i = 0; i < ARRAY_SIZE(views); i++) {
        for (size_t j = 0; j < ARRAY_SIZE(probes); j++) {
            g_assert_cmphex(qtest_readb(to, views[i] + probes[j].offset), ==,
                            probes[j].destination);
        }
    }

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    wait_for_migration(from);

    for (size_t i = 0; i < ARRAY_SIZE(views); i++) {
        for (size_t j = 0; j < ARRAY_SIZE(probes); j++) {
            g_assert_cmphex(qtest_readb(to, views[i] + probes[j].offset), ==,
                            probes[j].source);
        }
    }

    qtest_quit(from);
    qtest_quit(to);
    g_unlink(socket);
    g_assert_cmpint(g_unlink(destination_prom), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static void test_system_reset_clears_bedrock_state(const void *data)
{
    QTestState *qts = machine_start();
    uint64_t address = BEDROCK_LOCAL + BEDROCK_IO + IO_SCRATCH0;

    qtest_writeq(qts, address, UINT64_C(0x0ead0040));
    g_assert_cmphex(qtest_readq(qts, address), ==, UINT64_C(0x0ead0040));
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readq(qts, address), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int result;

    g_test_init(&argc, &argv, NULL);
    prom_path = create_prom(0x31, 0x62, 0x93);

    qtest_add_data_func("sn1/address-map/ram-and-hub-alias", NULL,
                        test_ram_and_hub_alias);
    qtest_add_data_func("sn1/address-map/bedrock-register-aliases", NULL,
                        test_bedrock_register_aliases);
    qtest_add_data_func("sn1/address-map/crosstalk-big-window", NULL,
                        test_crosstalk_big_window_routes);
    qtest_add_data_func("sn1/address-map/hub-special-register-windows", NULL,
                        test_hub_special_register_windows);
    qtest_add_data_func("sn1/address-map/prom-three-read-only-views", NULL,
                        test_prom_three_read_only_views);
    qtest_add_data_func("sn1/address-map/prom-flat-view-extents", NULL,
                        test_prom_flat_view_extents);
    qtest_add_data_func("sn1/address-map/prom-size-is-exact", NULL,
                        test_prom_size_is_exact);
    qtest_add_data_func("sn1/address-map/prom-is-required", NULL,
                        test_prom_is_required);
    qtest_add_data_func("sn1/address-map/cpu-count-is-exact", NULL,
                        test_cpu_count_is_exact);
    qtest_add_data_func("sn1/address-map/ram-size-is-exact", NULL,
                        test_ram_size_is_exact);
    qtest_add_data_func("sn1/address-map/cpu-type-is-r14000", NULL,
                        test_cpu_type_is_r14000);
    qtest_add_data_func("sn1/address-map/cpu-b-follows-enable", NULL,
                        test_cpu_b_follows_enable);
    qtest_add_data_func("sn1/address-map/prom-migration", NULL,
                        test_prom_migration);
    qtest_add_data_func("sn1/address-map/system-reset", NULL,
                        test_system_reset_clears_bedrock_state);

    result = g_test_run();
    g_assert_cmpint(g_unlink(prom_path), ==, 0);
    return result;
}
