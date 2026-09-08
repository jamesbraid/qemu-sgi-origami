/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define BEDROCK_PATH "/machine/peripheral/bedrock"
#define BASE UINT64_C(0x10000000)
#define PIO_BASE UINT64_C(0x100000000)
/*
 * The inbound aperture is 2^48 bytes from INBOUND_BASE, ending at
 * 0x1020000000000; the other windows sit above that so nothing overlaps.
 */
#define INBOUND_BASE UINT64_C(0x20000000000)
#define REMOTE_BASE UINT64_C(0x2000000000000)
#define IO_REGISTERS_BASE UINT64_C(0x3000000000000)
/* Crosstalk bit 47, small window 1 in its remote form, then the register. */
#define HUB_REMOTE_WINDOW (UINT64_C(1) << 47 | UINT64_C(0x1800000))
#define HUB_INTERRUPT_SEND(node, pi) \
    (INBOUND_BASE + HUB_REMOTE_WINDOW + ((uint64_t)(node) << 39) + \
     (pi) * UINT64_C(0x200000) + 0x90)
#define L1_PATH "/machine/peripheral/l1c"
#define IO_BASE (BASE + UINT64_C(0x400000))
#define LB_BASE (BASE + UINT64_C(0x600000))
#define XB_BASE (BASE + UINT64_C(0x700000))
#define MD_BASE (BASE + UINT64_C(0x780000))

#define MACHINE_BASE \
    "-M none -cpu R10000 -m 256M " \
    "-object memory-backend-ram,id=dma,size=2M " \
    "-object memory-backend-ram,id=widget8,size=2M " \
    "-object memory-backend-ram,id=widget9,size=2M "
#define BEDROCK_DEVICE \
    "-device sgi-bedrock,id=bedrock,revision=2," \
    "dma-memory=/objects/dma/dma[0]," \
    "widget-8=/objects/widget8/widget8[0]," \
    "widget-9=/objects/widget9/widget9[0]"
#define MACHINE MACHINE_BASE BEDROCK_DEVICE " -S"
#define MACHINE_L1 \
    MACHINE_BASE \
    "-device sgi-l1,id=l1c -device sgi-l1-uart,id=l1,controller=l1c " \
    BEDROCK_DEVICE \
    ",l1-uart=/machine/peripheral/l1/sgi-l1-uart[0],initiator-region=5 -S"

static QTestState *bedrock_start(const char *extra)
{
    QTestState *s = qtest_initf(MACHINE " %s", extra);

    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 0, BASE);
    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 1, PIO_BASE);
    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 2, INBOUND_BASE);
    return s;
}

static uint16_t l1_fcs(uint16_t value, uint8_t byte)
{
    value ^= byte;
    for (unsigned i = 0; i < 8; i++) {
        value = (value >> 1) ^ ((value & 1) ? 0x8408 : 0);
    }
    return value;
}

static void l1_put(QTestState *s, uint64_t base, uint8_t byte)
{
    if (byte == 0x7e || byte == 0x7d) {
        qtest_writeq(s, base, 0x7d);
        byte ^= 0x20;
    }
    qtest_writeq(s, base, byte);
}

static void l1_send_frame(QTestState *s, uint64_t base,
                          const uint8_t *payload, size_t length)
{
    uint16_t crc = 0xffff;

    qtest_writeq(s, base, 0x7e);
    for (size_t i = 0; i < length; i++) {
        crc = l1_fcs(crc, payload[i]);
        l1_put(s, base, payload[i]);
    }
    crc = ~crc;
    l1_put(s, base, crc);
    l1_put(s, base, crc >> 8);
    qtest_writeq(s, base, 0x7e);
}

static QTestState *bedrock_l1_start(const char *extra)
{
    QTestState *s = qtest_initf(MACHINE_L1 " %s", extra);

    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 0, BASE);
    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 1, PIO_BASE);
    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 2, INBOUND_BASE);
    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 3, REMOTE_BASE);
    return s;
}

static void test_parent_l1_protection(void)
{
    static const uint8_t display_a[] = {
        0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 5, 1, 1, 'a', 0,
    };
    static const uint8_t display_b[] = {
        0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 5, 1, 1, 'b', 0,
    };
    g_autoptr(QDict) result = NULL;
    QTestState *s = bedrock_l1_start("");

    l1_send_frame(s, REMOTE_BASE + 0x80, display_a, sizeof(display_a));
    result = qtest_qmp(s, "{'execute':'qom-get','arguments':"
                         "{'path':%s,'property':'display-line-2'}}", L1_PATH);
    g_assert_cmpstr(qdict_get_str(result, "return"), ==, "a");
    qobject_unref(result);
    result = NULL;
    g_assert_cmphex(qtest_readq(s, REMOTE_BASE + 0xa8), ==, 0x61);

    qtest_writeq(s, MD_BASE, UINT64_MAX & ~(UINT64_C(1) << 5));
    (void)qtest_readq(s, REMOTE_BASE + 0x80);
    l1_send_frame(s, REMOTE_BASE + 0x80, display_b, sizeof(display_b));
    result = qtest_qmp(s, "{'execute':'qom-get','arguments':"
                         "{'path':%s,'property':'display-line-2'}}", L1_PATH);
    g_assert_cmpstr(qdict_get_str(result, "return"), ==, "a");
    qobject_unref(result);
    result = NULL;

    qtest_writeq(s, MD_BASE, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REMOTE_BASE + 0x80), ==, 0x7e);
    l1_send_frame(s, REMOTE_BASE + 0x80, display_b, sizeof(display_b));
    result = qtest_qmp(s, "{'execute':'qom-get','arguments':"
                         "{'path':%s,'property':'display-line-2'}}", L1_PATH);
    g_assert_cmpstr(qdict_get_str(result, "return"), ==, "b");
    qobject_unref(result);
    result = NULL;

    qtest_writeq(s, REMOTE_BASE + 0x98, 3);
    qtest_writeq(s, REMOTE_BASE + 0xb8, 0x5a);
    qtest_writeq(s, REMOTE_BASE + 0x40, UINT64_MAX);
    qtest_writeq(s, REMOTE_BASE + 0xc0, UINT64_MAX);
    qtest_writeq(s, REMOTE_BASE + UINT64_C(0x8000000), UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REMOTE_BASE + 0x98), ==, 3);
    g_assert_cmphex(qtest_readq(s, REMOTE_BASE + 0xb8), ==, 0x5a);

    qtest_writeq(s, MD_BASE, 0);
    qtest_writeq(s, BASE + 0x680008, 4);
    g_assert_cmphex(qtest_readq(s, MD_BASE), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REMOTE_BASE + 0x98), ==, 3);
    g_assert_cmphex(qtest_readq(s, REMOTE_BASE + 0xb8), ==, 0x5a);
    g_assert_cmphex(qtest_readq(s, REMOTE_BASE + 0xa8), ==, 0x61);
    result = qtest_qmp(s, "{'execute':'qom-get','arguments':"
                         "{'path':%s,'property':'display-line-2'}}", L1_PATH);
    g_assert_cmpstr(qdict_get_str(result, "return"), ==, "b");
    qobject_unref(result);
    result = NULL;
    qtest_writeq(s, MD_BASE, 0);
    qtest_writeq(s, LB_BASE + 0x4018, 1);
    g_assert_cmphex(qtest_readq(s, MD_BASE), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REMOTE_BASE + 0x98), ==, 3);
    g_assert_cmphex(qtest_readq(s, REMOTE_BASE + 0xb8), ==, 0x5a);
    g_assert_cmphex(qtest_readq(s, REMOTE_BASE + 0xa8), ==, 0x61);
    g_assert_cmphex(qtest_readq(s, REMOTE_BASE + 0x80), ==, 0x30);
    result = qtest_qmp(s, "{'execute':'qom-get','arguments':"
                         "{'path':%s,'property':'display-line-2'}}", L1_PATH);
    g_assert_cmpstr(qdict_get_str(result, "return"), ==, "b");
    qobject_unref(result);
    result = NULL;

    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, REMOTE_BASE + 0x98), ==, 0);
    g_assert_cmphex(qtest_readq(s, REMOTE_BASE + 0xb8), ==, 0);
    result = qtest_qmp(s, "{'execute':'qom-get','arguments':"
                         "{'path':%s,'property':'display-line-2'}}", L1_PATH);
    g_assert_cmpstr(qdict_get_str(result, "return"), ==, "");
    qtest_quit(s);
}

static void test_parent_dma_and_reset(void)
{
    QTestState *s = bedrock_start("");
    uint8_t source[128], actual[128];

    for (unsigned i = 0; i < sizeof(source); i++) {
        source[i] = i ^ 0xa5;
    }
    qtest_memwrite(s, INBOUND_BASE + 0x10000, source, sizeof(source));
    qtest_memset(s, INBOUND_BASE + 0x20000, 0, sizeof(actual));
    qtest_memset(s, 0x20000, 0x6d, sizeof(actual));
    qtest_writeq(s, IO_BASE + 0x10000, 1 | (1 << 20));
    qtest_writeq(s, IO_BASE + 0x10008, 0x10000);
    qtest_writeq(s, IO_BASE + 0x10010, 0x20000);
    qtest_writeq(s, IO_BASE + 0x10018, 1 << 4);
    qtest_qmp_assert_success(s, "{'execute':'cont'}");
    qtest_clock_step(s, 1);
    qtest_memread(s, INBOUND_BASE + 0x20000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
    qtest_memread(s, 0x20000, actual, sizeof(actual));
    for (unsigned i = 0; i < sizeof(actual); i++) {
        g_assert_cmphex(actual[i], ==, 0x6d);
    }
    qtest_writel(s, INBOUND_BASE + 0x30000, 0x89abcdef);
    g_assert_cmphex(qtest_readl(s, INBOUND_BASE + 0x30000), ==, 0x89abcdef);
    g_assert_cmphex(qtest_readl(s, 0x30000), ==, 0);

    qtest_writeq(s, IO_BASE + 0x150, UINT64_C(0xfeedfacecafebeef));
    qtest_writeq(s, LB_BASE + 0x8008, UINT64_C(0x0123456789abcdef));
    qtest_writeq(s, BASE + 0x680008, 4);
    /*
     * The I/O interface's scratch register has no stated reset value; zero
     * after the local reset is the model's choice. Local-block scratch 1
     * keeping its value is the header's statement.
     */
    g_assert_cmphex(qtest_readq(s, IO_BASE + 0x150), ==, 0);
    g_assert_cmphex(qtest_readq(s, LB_BASE + 0x8008), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_quit(s);
}

static void test_parent_identity_and_pio(void)
{
    QTestState *s = bedrock_start("");

    g_assert_cmphex(qtest_readq(s, IO_BASE), ==, UINT64_C(0x2c11006c));
    g_assert_cmphex(qtest_readq(s, LB_BASE) & UINT64_C(0xffffffff), ==,
                    UINT64_C(0x2301206c));
    g_assert_cmphex(qtest_readq(s, XB_BASE), ==,
                    UINT64_C(0x0008000080000021));

    qtest_writeq(s, IO_BASE + 0x160, UINT64_C(0x800));
    qtest_writel(s, PIO_BASE + 0x20000100, 0x12345678);
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x20000100), ==, 0x12345678);
    qtest_writeq(s, IO_BASE + 0x160, UINT64_C(0x900));
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x20000100), ==, 0);
    qtest_writeq(s, IO_BASE + 0x100, 0x55);
    qtest_writeq(s, IO_BASE + 0x10100, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, IO_BASE + 0x100), ==, 0x55);
    qtest_quit(s);
}

static void program_md(QTestState *s)
{
    qtest_writeq(s, MD_BASE + 0x8, UINT64_MAX);
    qtest_writeq(s, MD_BASE + 0x20000, UINT64_C(0x8000000000000555));
    qtest_writeq(s, MD_BASE + 0x40000, 3);
    qtest_writeq(s, MD_BASE, 0);
    qtest_writeq(s, MD_BASE + 0x10010, 0x12345);
}

static void check_md_soft_reset(QTestState *s)
{
    g_assert_cmphex(qtest_readq(s, MD_BASE + 0x8), ==,
                    UINT64_C(0x373ff003fbfbfbfb));
    g_assert_cmphex(qtest_readq(s, MD_BASE + 0x20000), ==,
                    UINT64_C(0x8000000000000555));
    g_assert_cmphex(qtest_readq(s, MD_BASE + 0x40000), ==, 3);
    g_assert_cmphex(qtest_readq(s, MD_BASE), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD_BASE + 0x10010), ==, 0);
}

static void test_parent_md_decode_and_reset(void)
{
    QTestState *s = bedrock_start("");

    qtest_writeq(s, LB_BASE + 0x8008, UINT64_C(0x0123456789abcdef));
    qtest_writeq(s, MD_BASE + 0x60040, 1);
    qtest_writeq(s, MD_BASE + 0x60048, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, MD_BASE + 0x60040), ==, 1);
    g_assert_cmphex(qtest_readq(s, LB_BASE + 0x8008), ==,
                    UINT64_C(0x0123456789abcdef));
    program_md(s);
    qtest_writeq(s, XB_BASE + 0x20, 1);
    g_assert_cmphex(qtest_readq(s, BASE + 0xa0), ==, 1ULL << 54);
    g_assert_cmphex(qtest_readq(s, BASE + 0x2000a0), ==, 1ULL << 54);
    qtest_writeq(s, BASE + 0x680008, 4);
    check_md_soft_reset(s);
    /* The local reset cold-resets the crossbar; its error drops. */
    g_assert_cmphex(qtest_readq(s, BASE + 0xa0), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x2000a0), ==, 0);
    program_md(s);
    qtest_writeq(s, XB_BASE + 0x20, 1);
    g_assert_cmphex(qtest_readq(s, BASE + 0x2000a0), ==, 1ULL << 54);
    qtest_writeq(s, LB_BASE + 0x4018, 1);
    check_md_soft_reset(s);
    g_assert_cmphex(qtest_readq(s, BASE + 0xa0), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0x2000a0), ==, 0);
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, MD_BASE + 0x8), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD_BASE + 0x20000), ==, 0);
    g_assert_cmphex(qtest_readq(s, MD_BASE + 0x40000), ==, 0);
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

static void test_parent_migration(void)
{
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sn1-bedrock-migration-XXXXXX", &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = bedrock_l1_start("");
    to = bedrock_l1_start("-incoming defer");
    qtest_writeq(from, IO_BASE + 0x150, UINT64_C(0x13579bdf2468ace0));
    qtest_writeq(from, IO_BASE + 0x160, 0x800);
    qtest_writeq(from, MD_BASE + 0x8, UINT64_MAX);
    qtest_writeq(from, MD_BASE + 0x10010, UINT64_C(0x123456789ab));
    {
        static const uint8_t display[] = {
            0x10, 0x0f, 0xff, 0xf0, 6, 0x10, 5, 1, 1, 'm', 0,
        };
        l1_send_frame(from, REMOTE_BASE + 0x80, display, sizeof(display));
    }
    qtest_writeq(from, MD_BASE,
                 UINT64_MAX & ~(UINT64_C(1) << 5));
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    migrate_wait(from);
    g_assert_cmphex(qtest_readq(to, IO_BASE + 0x150), ==,
                    UINT64_C(0x13579bdf2468ace0));
    g_assert_cmphex(qtest_readq(to, IO_BASE + 0x160), ==, 0x800);
    g_assert_cmphex(qtest_readq(to, MD_BASE + 0x8), ==,
                    UINT64_C(0x373ff003fbfbfbfb));
    g_assert_cmphex(qtest_readq(to, MD_BASE + 0x10010), ==,
                    UINT64_C(0x123456789ab));
    g_assert_cmphex(qtest_readq(to, MD_BASE) & (UINT64_C(1) << 5), ==, 0);
    (void)qtest_readq(to, REMOTE_BASE + 0x80);
    qtest_writeq(to, MD_BASE, UINT64_MAX);
    g_assert_cmphex(qtest_readq(to, REMOTE_BASE + 0x80), ==, 0x7e);
    {
        g_autoptr(QDict) result =
            qtest_qmp(to, "{'execute':'qom-get','arguments':"
                          "{'path':%s,'property':'display-line-2'}}", L1_PATH);
        g_assert_cmpstr(qdict_get_str(result, "return"), ==, "m");
    }
    qtest_writel(to, PIO_BASE + 0x20000100, 0x55aa33cc);
    g_assert_cmphex(qtest_readl(to, PIO_BASE + 0x20000100), ==, 0x55aa33cc);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_parent_l1_invalid_region(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*initiator-region must be set below 64 when l1-uart is linked*");
        return;
    }
    s = qtest_init(MACHINE_BASE
        "-device sgi-l1,id=l1c -device sgi-l1-uart,id=l1,controller=l1c "
        BEDROCK_DEVICE
        ",l1-uart=/machine/peripheral/l1/sgi-l1-uart[0],initiator-region=64 -S");
    qtest_quit(s);
}

static void test_parent_l1_missing_region(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr(
            "*initiator-region must be set below 64 when l1-uart is linked*");
        return;
    }
    s = qtest_init(MACHINE_BASE
        "-device sgi-l1,id=l1c -device sgi-l1-uart,id=l1,controller=l1c "
        BEDROCK_DEVICE
        ",l1-uart=/machine/peripheral/l1/sgi-l1-uart[0] -S");
    qtest_quit(s);
}

static void test_parent_l1_missing_link(void)
{
    QTestState *s = bedrock_start("");

    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 3, REMOTE_BASE);
    qtest_writeq(s, MD_BASE + 0x10010, 0x12345);
    qtest_writeq(s, REMOTE_BASE + 0x80, 0x7e);
    g_assert_cmphex(qtest_readq(s, MD_BASE + 0x10010), ==, 0x12345);
    qtest_quit(s);
}

static void assert_log_contains(const char *path, const char *needle)
{
    GError *error = NULL;
    g_autofree char *contents = NULL;

    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    if (!strstr(contents, needle)) {
        g_test_message("log lacks \"%s\"; log follows: %s", needle, contents);
        g_assert_not_reached();
    }
}

/*
 * A refused access to the remote register window must be visible: a
 * permission denial as a guest error, an unlinked or undecoded target as
 * unimplemented. The log file is read after the machine has exited.
 */
static void test_parent_remote_window_logs(void)
{
    GError *error = NULL;
    g_autofree char *directory =
        g_dir_make_tmp("sn1-bedrock-remote-log-XXXXXX", &error);
    g_autofree char *log = NULL;
    g_autofree char *extra = NULL;
    QTestState *s;

    g_assert_no_error(error);
    log = g_build_filename(directory, "qemu.log", NULL);
    extra = g_strdup_printf("-d guest_errors,unimp -D %s", log);

    s = bedrock_l1_start(extra);
    qtest_writeq(s, MD_BASE, UINT64_MAX & ~(UINT64_C(1) << 5));
    qtest_writeq(s, REMOTE_BASE + 0x80, 0x7e);
    qtest_writeq(s, MD_BASE, UINT64_MAX);
    (void)qtest_readq(s, REMOTE_BASE + 0x1000);
    qtest_quit(s);
    assert_log_contains(log, "sgi-bedrock: remote register window write "
                        "at 0x80 denied for region 5");
    assert_log_contains(log, "sgi-bedrock: remote register window read "
                        "at 0x1000 is not decoded");
    g_assert_cmpint(unlink(log), ==, 0);

    s = bedrock_start(extra);
    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 3, REMOTE_BASE);
    qtest_writeq(s, REMOTE_BASE + 0x80, 0x7e);
    qtest_quit(s);
    assert_log_contains(log, "sgi-bedrock: remote register window write "
                        "at 0x80 with no L1 controller linked");
    g_assert_cmpint(unlink(log), ==, 0);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

/*
 * The remote form of the hub's own register window, as a widget's
 * interrupt write reaches it. Only the two interrupt-pending-modify
 * registers answer, and each write is qualified by the target interface's
 * I/O protect bit for the node's local region.
 */
static void test_parent_io_interrupt_write(void)
{
    QTestState *s = bedrock_start("");

    qtest_sysbus_mmio_map(s, BEDROCK_PATH, 4, IO_REGISTERS_BASE);
    qtest_irq_intercept_out_named(s, BEDROCK_PATH, "hardware-irq");
    qtest_writeq(s, BASE + 0xa8, 1ULL << 7);
    qtest_writeq(s, BASE + 0x2000b0, 1ULL << 10);

    qtest_writeq(s, IO_REGISTERS_BASE + 0x90, 0x107);
    g_assert_cmphex(qtest_readq(s, BASE + 0x98), ==, 1ULL << 7);
    g_assert_cmphex(qtest_readq(s, BASE + 0x200098), ==, 0);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, IO_REGISTERS_BASE + 0x200090, 0x14a);
    g_assert_cmphex(qtest_readq(s, BASE + 0x2000a0), ==, 1ULL << 10);
    g_assert_cmphex(qtest_readq(s, BASE + 0xa0), ==, 0);
    g_assert_true(qtest_get_irq(s, 11));
    qtest_writeq(s, IO_REGISTERS_BASE + 0x90, 0x007);
    g_assert_cmphex(qtest_readq(s, BASE + 0x98), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));

    /* No other register is reachable this way, and none is readable. */
    qtest_writeq(s, IO_REGISTERS_BASE + 0x180, 0x107);
    qtest_writeq(s, IO_REGISTERS_BASE + 0xa8, UINT64_MAX);
    qtest_writeq(s, IO_REGISTERS_BASE + 0x600040, 1);
    qtest_writel(s, IO_REGISTERS_BASE + 0x94, 0x107);
    g_assert_cmphex(qtest_readq(s, BASE + 0x98), ==, 0);
    g_assert_cmphex(qtest_readq(s, BASE + 0xa8), ==, 1ULL << 7);
    g_assert_cmphex(qtest_readq(s, BASE + 0xa0), ==, 0);
    g_assert_cmphex(qtest_readq(s, IO_REGISTERS_BASE + 0x90), ==, 0);

    /* I/O protect for the local region gates the write; CPU stores are not. */
    qtest_writeq(s, BASE + 0x10, UINT64_MAX ^ 1);
    qtest_writeq(s, IO_REGISTERS_BASE + 0x90, 0x107);
    g_assert_cmphex(qtest_readq(s, BASE + 0x98), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));
    qtest_writeq(s, BASE + 0x10, UINT64_MAX);
    qtest_writeq(s, IO_REGISTERS_BASE + 0x90, 0x107);
    g_assert_cmphex(qtest_readq(s, BASE + 0x98), ==, 1ULL << 7);
    qtest_writeq(s, BASE + 0x10, 0);
    qtest_writeq(s, IO_REGISTERS_BASE + 0x90, 0x007);
    g_assert_cmphex(qtest_readq(s, BASE + 0x98), ==, 1ULL << 7);
    qtest_writeq(s, BASE + 0x90, 0x007);
    g_assert_cmphex(qtest_readq(s, BASE + 0x98), ==, 0);
    qtest_quit(s);
}

/*
 * A widget's interrupt write as the I/O interface receives it: crosstalk
 * bit 47, the node at bit 39, small window 1 in its remote form, and the
 * modify register of the chosen interface. The I/O interface forwards it
 * to the parent's register window only after its inbound-access check and
 * only for this node; the parent then applies the interface's I/O protect.
 */
static void test_parent_widget_interrupt_write(void)
{
    QTestState *s = bedrock_start("");

    qtest_irq_intercept_out_named(s, BEDROCK_PATH, "hardware-irq");
    qtest_writeq(s, BASE + 0xa8, 1ULL << 7);
    qtest_writeq(s, BASE + 0x2000b0, 1ULL << 10);

    qtest_writeq(s, IO_BASE + 0x118, UINT64_MAX ^ (1ULL << 8));
    qtest_writeq(s, HUB_INTERRUPT_SEND(0, 0), 0x107);
    g_assert_cmphex(qtest_readq(s, BASE + 0x98), ==, 0);
    qtest_writeq(s, IO_BASE + 0x118, UINT64_MAX);
    qtest_writeq(s, HUB_INTERRUPT_SEND(0, 0), 0x107);
    g_assert_cmphex(qtest_readq(s, BASE + 0x98), ==, 1ULL << 7);
    g_assert_true(qtest_get_irq(s, 0));
    qtest_writeq(s, HUB_INTERRUPT_SEND(0, 1), 0x14a);
    g_assert_cmphex(qtest_readq(s, BASE + 0x2000a0), ==, 1ULL << 10);
    g_assert_true(qtest_get_irq(s, 11));

    /* A packet naming another node arrives nowhere. */
    qtest_writeq(s, HUB_INTERRUPT_SEND(1, 0), 0x007);
    qtest_writeq(s, HUB_INTERRUPT_SEND(1, 1), 0x04a);
    g_assert_cmphex(qtest_readq(s, BASE + 0x98), ==, 1ULL << 7);
    g_assert_cmphex(qtest_readq(s, BASE + 0x2000a0), ==, 1ULL << 10);

    /* I/O protect for the local region gates this path as well. */
    qtest_writeq(s, BASE + 0x10, UINT64_MAX ^ 1);
    qtest_writeq(s, HUB_INTERRUPT_SEND(0, 0), 0x007);
    g_assert_cmphex(qtest_readq(s, BASE + 0x98), ==, 1ULL << 7);
    qtest_writeq(s, BASE + 0x10, UINT64_MAX);
    qtest_writeq(s, HUB_INTERRUPT_SEND(0, 0), 0x007);
    g_assert_cmphex(qtest_readq(s, BASE + 0x98), ==, 0);
    g_assert_false(qtest_get_irq(s, 0));

    /* Only the modify registers answer through the window. */
    qtest_writeq(s, INBOUND_BASE + HUB_REMOTE_WINDOW + 0x600040, 1);
    g_assert_cmphex(qtest_readq(s, BASE + 0xa0), ==, 0);
    qtest_quit(s);
}

/*
 * The parent's cpu-enable group has four lines, interface times two plus
 * processor, fanned out from the two interfaces' own groups.
 */
static void test_parent_cpu_enable_lines(void)
{
    QTestState *s = bedrock_start("");

    qtest_irq_intercept_out_named(s, BEDROCK_PATH, "cpu-enable");
    for (unsigned line = 0; line < 4; line++) {
        uint64_t reg = BASE + (line / 2) * UINT64_C(0x200000) +
                       (line % 2 ? 0x58 : 0x50);

        qtest_writeq(s, reg, 1);
        for (unsigned other = 0; other < 4; other++) {
            g_assert_cmpint(qtest_get_irq(s, other), ==, other == line);
        }
        qtest_writeq(s, reg, 0);
        g_assert_false(qtest_get_irq(s, line));
    }
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("sn1/bedrock/composition/identity-pio",
                   test_parent_identity_and_pio);
    qtest_add_func("sn1/bedrock/composition/dma-reset",
                   test_parent_dma_and_reset);
    qtest_add_func("sn1/bedrock/composition/migration",
                   test_parent_migration);
    qtest_add_func("sn1/bedrock/composition/l1-invalid-region",
                   test_parent_l1_invalid_region);
    qtest_add_func("sn1/bedrock/composition/l1-missing-region",
                   test_parent_l1_missing_region);
    qtest_add_func("sn1/bedrock/composition/l1-missing-link",
                   test_parent_l1_missing_link);
    qtest_add_func("sn1/bedrock/composition/remote-window-logs",
                   test_parent_remote_window_logs);
    qtest_add_func("sn1/bedrock/composition/io-interrupt-write",
                   test_parent_io_interrupt_write);
    qtest_add_func("sn1/bedrock/composition/widget-interrupt-write",
                   test_parent_widget_interrupt_write);
    qtest_add_func("sn1/bedrock/composition/l1-protection",
                   test_parent_l1_protection);
    qtest_add_func("sn1/bedrock/composition/md-decode-reset",
                   test_parent_md_decode_and_reset);
    qtest_add_func("sn1/bedrock/composition/cpu-enable-lines",
                   test_parent_cpu_enable_lines);
    return g_test_run();
}
