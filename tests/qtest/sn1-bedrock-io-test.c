/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define IO_PATH "/machine/peripheral/io"
#define REG_BASE UINT64_C(0x40000000)
#define PIO_BASE UINT64_C(0x100000000)
#define IN_BASE UINT64_C(0x20000000000)
#define MACHINE_BASE \
    "-M none -cpu R10000 -m 256M " \
    "-object memory-backend-ram,id=dma,size=32M " \
    "-object memory-backend-ram,id=hub,size=8M " \
    "-object memory-backend-ram,id=widget8,size=2M " \
    "-object memory-backend-ram,id=widget9,size=2M " \
    "-device sgi-bedrock-io,id=io," \
    "dma-memory=/objects/dma/dma[0]," \
    "widget-8=/objects/widget8/widget8[0]," \
    "widget-9=/objects/widget9/widget9[0]"
#define MACHINE MACHINE_BASE ",hub-registers=/objects/hub/hub[0] -S"
#define MACHINE_NO_HUB MACHINE_BASE " -S"

/* Crosstalk address terms of the interrupt-send form. */
#define XIO_REGISTER (UINT64_C(1) << 47)
#define XIO_NODE(n) ((uint64_t)(n) << 39)
#define HUB_REMOTE UINT64_C(0x1800000)

#define WIDGET_STATUS  0x8
#define WIDGET_CONTROL 0x20
#define LOCAL_PROTECT  0x100
#define OUT_ACCESS     0x110
#define IN_ACCESS      0x118
#define DEVICE_MASK    0x120
#define LINK_STATUS    0x128
#define SCRATCH0       0x150
#define ITTE0          0x160
#define BTE0           0x10000
#define BTE1           0x20000

static QTestState *io_start_machine(const char *machine, const char *extra)
{
    QTestState *s = qtest_initf("%s %s", machine, extra);

    qtest_sysbus_mmio_map(s, IO_PATH, 0, REG_BASE);
    qtest_sysbus_mmio_map(s, IO_PATH, 1, PIO_BASE);
    qtest_sysbus_mmio_map(s, IO_PATH, 2, IN_BASE);
    if (!strstr(extra, "incoming")) {
        qtest_qmp_assert_success(s, "{'execute':'cont'}");
    }
    return s;
}

static QTestState *io_start(const char *extra)
{
    return io_start_machine(MACHINE, extra);
}

/*
 * What the model refuses or leaves unimplemented is part of its contract,
 * and the only trace of it is the log.  These run QEMU with -d unimp into
 * a file and hand its contents back once the machine has exited.
 */
static char *log_file_new(void)
{
    GError *error = NULL;
    char *path = NULL;
    int fd = g_file_open_tmp("sn1-io-log-XXXXXX", &path, &error);

    g_assert_no_error(error);
    close(fd);
    return path;
}

static QTestState *io_start_logged(const char *log, const char *extra)
{
    g_autofree char *with_log = g_strdup_printf("-d unimp -D %s %s", log,
                                                extra);

    return io_start(with_log);
}

static char *log_file_take(const char *path)
{
    GError *error = NULL;
    char *contents = NULL;

    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    unlink(path);
    return contents;
}

static void test_widget_status(void)
{
    g_autofree char *log = log_file_new();
    g_autofree char *contents = NULL;
    QTestState *s = io_start_logged(log, "");

    g_assert_cmphex(qtest_readq(s, REG_BASE + WIDGET_STATUS), ==, 0);
    qtest_writeq(s, REG_BASE + WIDGET_STATUS, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + WIDGET_STATUS), ==, 0);
    qtest_quit(s);
    contents = log_file_take(log);
    g_assert_null(strstr(contents, "unsupported read at 0x8\n"));
    g_assert_nonnull(strstr(contents, "widget status"));
}

/*
 * The unit's DMA memory is a separate backend, not the machine's RAM.  The
 * inbound window is the only path a test has into it, so every buffer the
 * engines and the inbound path touch is seeded and read back through it.
 * Plain qtest memory accesses reach the machine's RAM, which no II transfer
 * may touch.
 */
static void dma_write(QTestState *s, uint64_t addr, const void *data,
                      size_t size)
{
    qtest_memwrite(s, IN_BASE + addr, data, size);
}

static void dma_read(QTestState *s, uint64_t addr, void *data, size_t size)
{
    qtest_memread(s, IN_BASE + addr, data, size);
}

static void dma_set(QTestState *s, uint64_t addr, uint8_t pattern,
                    size_t size)
{
    qtest_memset(s, IN_BASE + addr, pattern, size);
}

static uint64_t dma_readq(QTestState *s, uint64_t addr)
{
    return qtest_readq(s, IN_BASE + addr);
}

static void dma_writeq(QTestState *s, uint64_t addr, uint64_t value)
{
    qtest_writeq(s, IN_BASE + addr, value);
}

static void assert_ram_filled(QTestState *s, uint64_t addr, uint8_t pattern,
                              size_t size)
{
    g_autofree uint8_t *actual = g_malloc(size);

    qtest_memread(s, addr, actual, size);
    for (size_t i = 0; i < size; i++) {
        g_assert_cmphex(actual[i], ==, pattern);
    }
}

/*
 * A widget's view of the node's register space: crosstalk bit 47 set, the
 * node in bits 46:39, then the Bedrock's own small window in its remote
 * form.  The linked hub-registers RAM is reachable only this way.
 */
static uint64_t xio_readq(QTestState *s, uint64_t address)
{
    return qtest_readq(s, IN_BASE + XIO_REGISTER + address);
}

static void xio_writeq(QTestState *s, uint64_t address, uint64_t value)
{
    qtest_writeq(s, IN_BASE + XIO_REGISTER + address, value);
}

static void test_inbound_register_access(void)
{
    static const uint64_t nowhere[] = {
        XIO_NODE(1) | HUB_REMOTE | 0x90,      /* another node */
        UINT64_C(0x1000090),                  /* local form, no bit 23 */
        UINT64_C(0x2800090),                  /* widget 2's window */
        UINT64_C(0x8800090),                  /* widget 8's window */
        UINT64_C(0x20000090),                 /* big window 0 */
        HUB_REMOTE | 0x90 | (UINT64_C(1) << 28), /* above the small windows */
    };
    QTestState *s = io_start("");

    /*
     * The DMA line at the same 39-bit address shows the register form
     * never reaches memory, and the memory form never reaches a register.
     */
    dma_writeq(s, HUB_REMOTE + 0x90, UINT64_C(0x1111111111111111));
    dma_writeq(s, 0x1000090, UINT64_C(0x3333333333333333));
    xio_writeq(s, HUB_REMOTE + 0x90, 0x105);
    g_assert_cmphex(xio_readq(s, HUB_REMOTE + 0x90), ==, 0x105);
    g_assert_cmphex(dma_readq(s, HUB_REMOTE + 0x90), ==,
                    UINT64_C(0x1111111111111111));
    dma_writeq(s, HUB_REMOTE + 0x90, UINT64_C(0x2222222222222222));
    g_assert_cmphex(xio_readq(s, HUB_REMOTE + 0x90), ==, 0x105);

    xio_writeq(s, HUB_REMOTE + 0x200090, 0x14a);
    g_assert_cmphex(xio_readq(s, HUB_REMOTE + 0x200090), ==, 0x14a);
    g_assert_cmphex(xio_readq(s, HUB_REMOTE + 0x90), ==, 0x105);

    qtest_writel(s, IN_BASE + XIO_REGISTER + HUB_REMOTE + 0x94, 0xaabbccdd);
    g_assert_cmphex(xio_readq(s, HUB_REMOTE + 0x90), ==, 0xaabbccdd);
    g_assert_cmphex(qtest_readl(s, IN_BASE + XIO_REGISTER + HUB_REMOTE + 0x90),
                    ==, 0);
    qtest_writeb(s, IN_BASE + XIO_REGISTER + HUB_REMOTE + 0x97, 0xee);
    g_assert_cmphex(xio_readq(s, HUB_REMOTE + 0x90), ==, 0xaabbccee);

    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX & ~(1 << 8));
    xio_writeq(s, HUB_REMOTE + 0x90, 0x777);
    g_assert_cmphex(xio_readq(s, HUB_REMOTE + 0x90), ==, 0);
    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX);
    g_assert_cmphex(xio_readq(s, HUB_REMOTE + 0x90), ==, 0xaabbccee);

    for (size_t i = 0; i < ARRAY_SIZE(nowhere); i++) {
        xio_writeq(s, nowhere[i], 0x999);
        g_assert_cmphex(xio_readq(s, nowhere[i]), ==, 0);
        g_assert_cmphex(xio_readq(s, HUB_REMOTE + 0x90), ==, 0xaabbccee);
    }
    g_assert_cmphex(dma_readq(s, HUB_REMOTE + 0x90), ==,
                    UINT64_C(0x2222222222222222));
    g_assert_cmphex(dma_readq(s, 0x1000090), ==,
                    UINT64_C(0x3333333333333333));
    qtest_quit(s);

    s = io_start("-global sgi-bedrock-io.node-number=3");
    xio_writeq(s, XIO_NODE(3) | HUB_REMOTE | 0x90, 0x123);
    g_assert_cmphex(xio_readq(s, XIO_NODE(3) | HUB_REMOTE | 0x90), ==, 0x123);
    xio_writeq(s, HUB_REMOTE | 0x90, 0x456);
    g_assert_cmphex(xio_readq(s, HUB_REMOTE | 0x90), ==, 0);
    g_assert_cmphex(xio_readq(s, XIO_NODE(3) | HUB_REMOTE | 0x90), ==, 0x123);
    qtest_quit(s);

    s = io_start("-global sgi-bedrock-io.device-mask-reset="
                 "0xfffffffffffffffe");
    xio_writeq(s, HUB_REMOTE | 0x90, 0x123);
    qtest_writeq(s, REG_BASE + DEVICE_MASK, 1);
    g_assert_cmphex(xio_readq(s, HUB_REMOTE | 0x90), ==, 0);
    qtest_quit(s);

    s = io_start_machine(MACHINE_NO_HUB, "");
    dma_writeq(s, HUB_REMOTE + 0x90, UINT64_C(0x4444444444444444));
    xio_writeq(s, HUB_REMOTE | 0x90, 0x123);
    g_assert_cmphex(xio_readq(s, HUB_REMOTE | 0x90), ==, 0);
    g_assert_cmphex(dma_readq(s, HUB_REMOTE + 0x90), ==,
                    UINT64_C(0x4444444444444444));
    qtest_quit(s);
}

static void test_dma_link_required(void)
{
    QTestState *s;

    if (!g_test_subprocess()) {
        g_test_trap_subprocess(NULL, 0, 0);
        g_test_trap_assert_failed();
        g_test_trap_assert_stderr("*'dma-memory' link not set*");
        return;
    }
    s = qtest_init("-M none -cpu R10000 -m 256M -device sgi-bedrock-io,id=io "
                   "-S");
    qtest_quit(s);
}

static void test_identity_and_registers(void)
{
    QTestState *s = io_start("");

    g_assert_cmphex(qtest_readq(s, REG_BASE), ==, UINT64_C(0x1c11006c));
    g_assert_cmphex(qtest_readq(s, REG_BASE + WIDGET_CONTROL) & 0x2000f, ==,
                    0xa);
    g_assert_true(qtest_readq(s, REG_BASE + LINK_STATUS) & (1 << 13));
    g_assert_cmphex(qtest_readq(s, REG_BASE + LOCAL_PROTECT), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + OUT_ACCESS), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + IN_ACCESS), ==, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + DEVICE_MASK), ==, UINT64_MAX);

    qtest_writeq(s, REG_BASE + WIDGET_CONTROL, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + WIDGET_CONTROL), ==, 0x7fe01f);
    qtest_writeq(s, REG_BASE + SCRATCH0, UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(s, REG_BASE + SCRATCH0), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_writeq(s, REG_BASE + ITTE0, 0x1822);
    g_assert_cmphex(qtest_readq(s, REG_BASE + ITTE0), ==, 0x1802);
    qtest_writeq(s, REG_BASE + LOCAL_PROTECT, 0x55);
    qtest_writeq(s, REG_BASE + BTE0 + LOCAL_PROTECT, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + LOCAL_PROTECT), ==, 0x55);
    qtest_quit(s);
}

static void test_probe_and_retained_registers(void)
{
    QTestState *s = io_start("");
    static const uint64_t retained_offsets[] = {
        0x140, 0x148, 0x208, 0x210, 0x218, 0x220, 0x228, 0x300,
        0x388, 0x390, 0x398, 0x3a0, 0x600, 0x608, 0x610,
        0x30000, 0x30008,
    };
    static const struct {
        uint64_t offset;
        uint64_t value;
        uint64_t expected;
    } registers[] = {
        { 0x1e0, UINT64_MAX, 0x3ffffff },
        { 0x1e8, 0x10, 0x10 },
        { 0x1f0, UINT64_MAX, UINT64_C(0x0fffffff03ffffff) },
        { 0x200, UINT64_MAX, 0x10f },
        { 0x3a8, UINT64_MAX, UINT64_C(0x13ffffffff30f3) },
        { 0x3b0, UINT64_MAX, 0x1ffff },
        { 0x3b8, UINT64_MAX, 0xff },
        { 0x3c0, UINT64_MAX, 0xffffff },
    };
    const uint8_t bytes[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    for (size_t i = 0; i < ARRAY_SIZE(registers); i++) {
        qtest_writeq(s, REG_BASE + registers[i].offset, registers[i].value);
        g_assert_cmphex(qtest_readq(s, REG_BASE + registers[i].offset), ==,
                        registers[i].expected);
    }
    qtest_writeq(s, REG_BASE + LINK_STATUS, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + LINK_STATUS), ==,
                    UINT64_C(0x3ff0fff7d3f));
    qtest_writeq(s, REG_BASE + 0x138, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + 0x138), ==, 0x101ff7f);
    for (size_t i = 0; i < ARRAY_SIZE(retained_offsets); i++) {
        uint64_t value = UINT64_C(0xfeed000000000000) | i;

        qtest_writeq(s, REG_BASE + retained_offsets[i], value);
        g_assert_cmphex(qtest_readq(s, REG_BASE + retained_offsets[i]), ==,
                        value);
    }
    for (unsigned i = 0; i < 7; i++) {
        qtest_writeq(s, REG_BASE + ITTE0 + i * 8, 0x1000 | (i << 8) | i);
        g_assert_cmphex(qtest_readq(s, REG_BASE + ITTE0 + i * 8), ==,
                        0x1000 | (i << 8) | i);
    }
    for (unsigned i = 0; i < 9; i++) {
        uint64_t offset = i ? 0x198 + i * 8 : 0x198;
        qtest_writeq(s, REG_BASE + offset, UINT64_C(0x1000000000000000) | i);
        g_assert_cmphex(qtest_readq(s, REG_BASE + offset), ==,
                        UINT64_C(0x1000000000000000) | i);
    }
    for (unsigned i = 0; i < 60; i++) {
        qtest_writeq(s, REG_BASE + 0x400 + i * 8,
                     UINT64_C(0x8000000000000000) | i);
        g_assert_cmphex(qtest_readq(s, REG_BASE + 0x400 + i * 8), ==,
                        UINT64_C(0x8000000000000000) | i);
    }
    for (unsigned i = 0; i < 8; i++) {
        qtest_writeq(s, REG_BASE + 0x308 + i * 8,
                     UINT64_C(0x4000000000000000) | i);
        g_assert_cmphex(qtest_readq(s, REG_BASE + 0x308 + i * 8), ==,
                        UINT64_C(0x4000000000000000) | i);
    }
    qtest_memwrite(s, REG_BASE + SCRATCH0, bytes, sizeof(bytes));
    g_assert_cmphex(qtest_readq(s, REG_BASE + SCRATCH0), ==,
                    UINT64_C(0x0102030405060708));
    qtest_writeb(s, REG_BASE + SCRATCH0 + 7, 0);
    qtest_writew(s, REG_BASE + SCRATCH0 + 6, 0);
    qtest_writel(s, REG_BASE + SCRATCH0 + 4, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + SCRATCH0), ==,
                    UINT64_C(0x0102030405060708));
    qtest_writeq(s, REG_BASE + 0x1f8, UINT64_MAX);
    g_assert_cmphex(qtest_readq(s, REG_BASE + 0x1f8), ==, 0);
    qtest_quit(s);
}

static void test_pio_translation_and_protection(void)
{
    QTestState *s = io_start("");

    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x1000000), ==, 0x1c11006c);
    qtest_writel(s, PIO_BASE + 0x1000000, UINT32_MAX);
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x1000000), ==, 0x1c11006c);
    qtest_writel(s, 0x100, 0x0badcafe);
    dma_writeq(s, 0x100, UINT64_C(0xd00d5eedd00d5eed));
    qtest_writeq(s, REG_BASE + ITTE0, 0x800);
    qtest_writel(s, PIO_BASE + 0x20000100, 0x12345678);
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x20000100), ==, 0x12345678);
    qtest_writeq(s, REG_BASE + ITTE0, 0x900);
    qtest_writel(s, PIO_BASE + 0x20000100, 0x87654321);
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x20000100), ==, 0x87654321);
    qtest_writeq(s, REG_BASE + ITTE0, 0x800);
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x20000100), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(s, 0x100), ==, 0x0badcafe);
    g_assert_cmphex(dma_readq(s, 0x100), ==, UINT64_C(0xd00d5eedd00d5eed));
    qtest_writeq(s, REG_BASE + OUT_ACCESS, 0);
    qtest_writel(s, PIO_BASE + 0x20000100, 0xa5a5a5a5);
    qtest_writeq(s, REG_BASE + OUT_ACCESS, 1 << 8);
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x20000100), ==, 0x12345678);
    qtest_writeq(s, REG_BASE + LOCAL_PROTECT, 0);
    qtest_writel(s, PIO_BASE + 0x20000100, 0xa5a5a5a5);
    qtest_writeq(s, REG_BASE + LOCAL_PROTECT + 8,
                 UINT64_C(0x53474972756c6573));
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x20000100), ==, 0x12345678);
    qtest_writel(s, PIO_BASE + 0x20000100, 0xa5a5a5a5);
    g_assert_cmphex(qtest_readl(s, PIO_BASE + 0x20000100), ==, 0xa5a5a5a5);
    qtest_quit(s);
}

static void test_inbound_protection(void)
{
    QTestState *s = io_start("");

    qtest_memset(s, 0x190000, 0x77, 16);
    dma_writeq(s, 0x190000, UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(dma_readq(s, 0x190000), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX & ~(1 << 8));
    dma_writeq(s, 0x190000, UINT64_C(0xfedcba9876543210));
    qtest_writeq(s, REG_BASE + IN_ACCESS, UINT64_MAX);
    g_assert_cmphex(dma_readq(s, 0x190000), ==,
                    UINT64_C(0x0123456789abcdef));
    qtest_writel(s, IN_BASE + 0x190008, 0xa55a3cc3);
    g_assert_cmphex(qtest_readl(s, IN_BASE + 0x190008), ==, 0xa55a3cc3);
    assert_ram_filled(s, 0x190000, 0x77, 16);
    qtest_quit(s);

    s = io_start("-global sgi-bedrock-io.device-mask-reset="
                 "0xfffffffffffffffe");
    dma_writeq(s, 0x190000, UINT64_MAX);
    qtest_writeq(s, REG_BASE + DEVICE_MASK, 1);
    g_assert_cmphex(dma_readq(s, 0x190000), ==, 0);
    qtest_quit(s);
}

static void fill_pattern(uint8_t *buf, size_t size, uint8_t salt)
{
    for (size_t i = 0; i < size; i++) {
        buf[i] = (i * 37) ^ salt;
    }
}

static void test_bte_copy_and_zero(void)
{
    QTestState *s = io_start("");
    uint8_t source[256], actual[256], zero[128] = { 0 };

    fill_pattern(source, sizeof(source), 0x5a);
    dma_write(s, 0x100000, source, sizeof(source));
    dma_set(s, 0x110000, 0xcc, sizeof(source));
    qtest_memset(s, 0x110000, 0xee, sizeof(source));
    qtest_memset(s, 0x120000, 0xee, 8);
    qtest_writeq(s, REG_BASE + BTE0, 2 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 8, 0x100000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, 0x110000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x20, 0x120000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 1 << 4);
    qtest_writeq(s, REG_BASE + BTE0, 7 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 8, 0x130000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, 0x140000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x20, 0x150000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x28, UINT64_MAX);
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 1);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 2 | (1 << 20));
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 8), ==, 0x100000);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 0x10), ==, 0x110000);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 0x20), ==, 0x120000);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 0x28), ==, 0);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 1 | (1 << 20));
    dma_read(s, 0x110000, actual, sizeof(actual));
    g_assert_cmpmem(actual, 128, source, 128);
    for (size_t i = 128; i < sizeof(actual); i++) {
        g_assert_cmphex(actual[i], ==, 0xcc);
    }
    qtest_clock_step(s, 1);
    dma_read(s, 0x110000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));
    g_assert_cmphex(dma_readq(s, 0x120000), ==, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 0);
    assert_ram_filled(s, 0x110000, 0xee, sizeof(source));
    assert_ram_filled(s, 0x120000, 0xee, 8);

    dma_set(s, 0x130000, 0xcc, sizeof(zero));
    qtest_writeq(s, REG_BASE + BTE1, 1 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE1 + 0x10, 0x130000);
    qtest_writeq(s, REG_BASE + BTE1 + 0x20, 0x140000);
    qtest_writeq(s, REG_BASE + BTE1 + 0x18, 1 | (1 << 4));
    qtest_clock_step(s, 1);
    dma_read(s, 0x130000, actual, sizeof(zero));
    g_assert_cmpmem(actual, sizeof(zero), zero, sizeof(zero));
    g_assert_cmphex(dma_readq(s, 0x140000), ==, 0);
    qtest_quit(s);
}

static void test_bte_error_and_termination(void)
{
    QTestState *s = io_start("");
    uint8_t source[256], actual[256], unchanged[256];

    fill_pattern(source, sizeof(source), 0x93);
    memset(unchanged, 0xcc, sizeof(unchanged));
    dma_write(s, 0x150000, source, sizeof(source));
    dma_write(s, 0x160000, unchanged, sizeof(unchanged));
    dma_write(s, 0x170000, unchanged, sizeof(unchanged));

    qtest_writeq(s, REG_BASE + BTE0, 2 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 8, 0x150000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, 0x160000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0 + 0x18), ==, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 2);

    qtest_writeq(s, REG_BASE + BTE1, 2 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE1 + 8, 0x150000);
    qtest_writeq(s, REG_BASE + BTE1 + 0x10, 0x170000);
    qtest_writeq(s, REG_BASE + BTE1 + 0x18, 0);
    qtest_clock_step(s, 1);
    qtest_clock_step(s, 1);
    dma_read(s, 0x160000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), unchanged, sizeof(unchanged));
    dma_read(s, 0x170000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), source, sizeof(source));

    qtest_writeq(s, REG_BASE + BTE0, 2 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 8, UINT64_C(0x80000000));
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, 0x160000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x20, 0x180000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 1 << 4);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 2 | (1 << 16));
    g_assert_cmphex(dma_readq(s, 0x180000), ==, 2 | (1 << 16));

    qtest_writeq(s, REG_BASE + BTE0, 1 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 8, 0x150000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, UINT64_C(0x80000000));
    qtest_writeq(s, REG_BASE + BTE0 + 0x20, 0x180080);
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 1 << 4);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 1 | (1 << 16));
    g_assert_cmphex(dma_readq(s, 0x180080), ==, 1 | (1 << 16));

    dma_set(s, 0x160000, 0, 128);
    qtest_writeq(s, REG_BASE + BTE0, 1 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 8, 0x150000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, 0x160000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x20, UINT64_C(0x80000000));
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 1 << 4);
    qtest_clock_step(s, 1);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 0);
    dma_read(s, 0x160000, actual, 128);
    g_assert_cmpmem(actual, 128, source, 128);

    qtest_writeq(s, REG_BASE + BTE0, 1 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 1 << 8);
    qtest_clock_step(s, 2);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 1 | (1 << 20));
    qtest_readq(s, REG_BASE + BTE0 + 0x18);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 1);
    qtest_quit(s);
}

static void test_bte_programming_edges(void)
{
    QTestState *s = io_start("");
    uint8_t unchanged[256], actual[256];

    memset(unchanged, 0x6d, sizeof(unchanged));
    dma_write(s, 0x1c0000, unchanged, sizeof(unchanged));
    dma_writeq(s, 0x1d0000, UINT64_MAX);
    qtest_writeq(s, REG_BASE + BTE0, 1 << 20);
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, 0x1c0000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x20, 0x1d0000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 1 << 4);
    qtest_clock_step(s, 2);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 0);
    g_assert_cmphex(dma_readq(s, 0x1d0000), ==, UINT64_MAX);
    dma_read(s, 0x1c0000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), unchanged, sizeof(unchanged));

    dma_writeq(s, 0, UINT64_C(0x8877665544332211));
    qtest_writeq(s, REG_BASE + BTE0, 2 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE0 + 8, UINT64_C(0xffffffff80));
    qtest_writeq(s, REG_BASE + BTE0 + 0x10, 0x1c0000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x20, 0x1d0000);
    qtest_writeq(s, REG_BASE + BTE0 + 0x18, 1 << 4);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 2 | (1 << 16));
    g_assert_cmphex(dma_readq(s, 0x1d0000), ==, 2 | (1 << 16));
    g_assert_cmphex(dma_readq(s, 0), ==, UINT64_C(0x8877665544332211));
    dma_read(s, 0x1c0000, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), unchanged, sizeof(unchanged));

    qtest_writeq(s, REG_BASE + BTE1, 2 | (1 << 20));
    qtest_writeq(s, REG_BASE + BTE1 + 8, 0x100000);
    qtest_writeq(s, REG_BASE + BTE1 + 0x10, UINT64_C(0xffffffff80));
    qtest_writeq(s, REG_BASE + BTE1 + 0x20, 0x1d0080);
    qtest_writeq(s, REG_BASE + BTE1 + 0x18, 1 << 4);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE1), ==, 2 | (1 << 16));
    g_assert_cmphex(dma_readq(s, 0x1d0080), ==, 2 | (1 << 16));
    g_assert_cmphex(dma_readq(s, 0), ==, UINT64_C(0x8877665544332211));
    qtest_quit(s);
}

static void test_reset(void)
{
    QTestState *s = io_start("");

    qtest_writeq(s, REG_BASE + SCRATCH0, UINT64_MAX);
    qtest_writeq(s, REG_BASE + ITTE0, 0x1802);
    qtest_writeq(s, REG_BASE + BTE0, 3 | (1 << 20));
    qtest_system_reset(s);
    g_assert_cmphex(qtest_readq(s, REG_BASE + SCRATCH0), ==, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + ITTE0), ==, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + BTE0), ==, 0);
    g_assert_cmphex(qtest_readq(s, REG_BASE + LOCAL_PROTECT), ==, UINT64_MAX);
    qtest_quit(s);
}

static void migrate_wait(QTestState *from)
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

static void test_migration_inflight(void)
{
    GError *error = NULL;
    g_autofree char *directory = g_dir_make_tmp("sn1-io-migration-XXXXXX",
                                                &error);
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    QTestState *from, *to;
    uint8_t source[256], prefill[256], actual[256];

    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    from = io_start("");
    to = io_start("-incoming defer");
    fill_pattern(source, sizeof(source), 0xc7);
    memset(prefill, 0x3c, sizeof(prefill));
    dma_write(from, 0x1a0000, source, sizeof(source));
    dma_set(from, 0x1b0000, 0, sizeof(source));
    /*
     * The DMA backend is not migrated RAM.  Seed the destination's copy
     * so the resumed engine has a source, with a distinct fill in the
     * destination line the source side already copied: the resumed
     * transfer must continue at line 2 and leave line 1 alone.
     */
    dma_write(to, 0x1a0000, source, sizeof(source));
    dma_write(to, 0x1b0000, prefill, sizeof(prefill));
    qtest_writeq(from, REG_BASE + SCRATCH0, UINT64_C(0x13579bdf2468ace0));
    qtest_writeq(from, REG_BASE + ITTE0, 0x1802);
    qtest_writeq(from, REG_BASE + BTE0, 2 | (1 << 20));
    qtest_writeq(from, REG_BASE + BTE0 + 8, 0x1a0000);
    qtest_writeq(from, REG_BASE + BTE0 + 0x10, 0x1b0000);
    qtest_writeq(from, REG_BASE + BTE0 + 0x18, 0);
    qtest_clock_step(from, 1);
    g_assert_cmphex(qtest_readq(from, REG_BASE + BTE0), ==,
                    1 | (1 << 20));
    dma_read(from, 0x1b0000, actual, sizeof(actual));
    g_assert_cmpmem(actual, 128, source, 128);
    for (size_t i = 128; i < sizeof(actual); i++) {
        g_assert_cmphex(actual[i], ==, 0);
    }

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    migrate_wait(from);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    g_assert_cmphex(qtest_readq(to, REG_BASE + SCRATCH0), ==,
                    UINT64_C(0x13579bdf2468ace0));
    g_assert_cmphex(qtest_readq(to, REG_BASE + ITTE0), ==, 0x1802);
    g_assert_cmphex(qtest_readq(to, REG_BASE + BTE0), ==,
                    1 | (1 << 20));
    g_assert_cmphex(qtest_readq(to, REG_BASE + BTE0 + 8), ==, 0x1a0080);
    g_assert_cmphex(qtest_readq(to, REG_BASE + BTE0 + 0x10), ==, 0x1b0080);
    qtest_clock_step_next(to);
    g_assert_cmphex(qtest_readq(to, REG_BASE + BTE0), ==, 0);
    dma_read(to, 0x1b0000, actual, sizeof(actual));
    g_assert_cmpmem(actual, 128, prefill, 128);
    g_assert_cmpmem(actual + 128, 128, source + 128, 128);
    qtest_quit(from);
    qtest_quit(to);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/mips64/sn1/bedrock-io/dma-link-required",
                   test_dma_link_required);
    qtest_add_func("/mips64/sn1/bedrock-io/identity-registers",
                   test_identity_and_registers);
    qtest_add_func("/mips64/sn1/bedrock-io/widget-status",
                   test_widget_status);
    qtest_add_func("/mips64/sn1/bedrock-io/probe-retained",
                   test_probe_and_retained_registers);
    qtest_add_func("/mips64/sn1/bedrock-io/pio-translation",
                   test_pio_translation_and_protection);
    qtest_add_func("/mips64/sn1/bedrock-io/inbound-protection",
                   test_inbound_protection);
    qtest_add_func("/mips64/sn1/bedrock-io/inbound-register-access",
                   test_inbound_register_access);
    qtest_add_func("/mips64/sn1/bedrock-io/bte", test_bte_copy_and_zero);
    qtest_add_func("/mips64/sn1/bedrock-io/bte-error-termination",
                   test_bte_error_and_termination);
    qtest_add_func("/mips64/sn1/bedrock-io/bte-programming-edges",
                   test_bte_programming_edges);
    qtest_add_func("/mips64/sn1/bedrock-io/reset", test_reset);
    qtest_add_func("/mips64/sn1/bedrock-io/migration-inflight",
                   test_migration_inflight);
    return g_test_run();
}
